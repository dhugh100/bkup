/* Unit tests for catalog manifest serialization (src/common/catalog_chunk.c:
   catman_serialize, catman_parse, catman_free).

   Complements test_catalog_chunk.c (which tests the chunking and encryption
   round-trip end-to-end).  These tests focus on the manifest format itself:
   field values, edge cases (nchunks==0), and rejection of malformed data.

   Build + run (see run_tests.sh):
     gcc -std=c11 -D_GNU_SOURCE -Isrc tests/test_catman.c \
         obj/common/catalog_chunk.o obj/common/util.o obj/common/log.o \
         $(pkg-config --libs sqlite3 libzstd libsodium libssh2) \
         -o /tmp/test_catman && /tmp/test_catman
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "common/catalog_chunk.h"
#include "common/types.h"
#include "common/util.h"
#include "test_common.h"

static int fails;

/*
 * Header layout (from catalog_chunk.c):
 *   magic[4] "BCM1" | version u32 | snap_id i64 | db_size i64 | nchunks u32
 * = 4 + 4 + 8 + 8 + 4 = 28 bytes.
 */
#define HDR_LEN 28

/* ---- Round-trip -------------------------------------------------------- */

static void test_roundtrip(void)
{
    /* 5 chunks with distinct hash bytes */
    uint32_t nchunks = 5;
    uint8_t hashes[5 * BK_HASH_LEN];
    fill(hashes, sizeof hashes, 0xABCD1234ULL);

    int64_t snap_id = 42LL;
    int64_t db_size = 1234567LL;

    Buf out;
    buf_init(&out);
    catman_serialize(&out, snap_id, db_size, hashes, nchunks);

    /* serialized size: header + nchunks * BK_HASH_LEN */
    CHECK(out.len == HDR_LEN + (size_t)nchunks * BK_HASH_LEN);

    CatManifest m;
    CHECK(catman_parse(out.data, out.len, &m) == 0);

    /* fields round-trip */
    CHECKEQ_INT(m.snap_id,  (long long)snap_id);
    CHECKEQ_INT(m.db_size,  (long long)db_size);
    CHECKEQ_INT(m.nchunks,  (long long)nchunks);
    CHECK(m.hashes != NULL);

    /* hash bytes are byte-identical */
    if (m.hashes)
        CHECK_MEMEQ(m.hashes, hashes, (size_t)nchunks * BK_HASH_LEN);

    catman_free(&m);
    CHECK(m.hashes == NULL);  /* free clears the pointer */

    buf_free(&out);
}

/* ---- nchunks == 0 round-trip ------------------------------------------ */

static void test_zero_chunks(void)
{
    Buf out;
    buf_init(&out);
    catman_serialize(&out, 0LL, 0LL, NULL, 0);

    /* header only, no hash bytes */
    CHECK(out.len == HDR_LEN);

    CatManifest m;
    CHECK(catman_parse(out.data, out.len, &m) == 0);
    CHECKEQ_INT(m.nchunks, 0);
    CHECK(m.hashes == NULL);

    catman_free(&m);  /* no-op on NULL hashes; must not crash */

    buf_free(&out);
}

/* ---- Rejection of malformed data -------------------------------------- */

static void test_reject_bad(void)
{
    /* Build a valid serialized manifest for mutation testing */
    uint32_t nchunks = 3;
    uint8_t hashes[3 * BK_HASH_LEN];
    fill(hashes, sizeof hashes, 0xDEAD5678ULL);

    Buf good;
    buf_init(&good);
    catman_serialize(&good, 10LL, 99999LL, hashes, nchunks);
    CHECK(good.len == HDR_LEN + (size_t)nchunks * BK_HASH_LEN);

    CatManifest m;

    /* bad magic: flip first byte */
    {
        Buf bad;
        buf_init(&bad);
        buf_append(&bad, good.data, good.len);
        bad.data[0] ^= 0xFF;
        CHECK(catman_parse(bad.data, bad.len, &m) == -1);
        buf_free(&bad);
    }

    /* wrong version: increment version field at bytes [4..7] */
    {
        Buf bad;
        buf_init(&bad);
        buf_append(&bad, good.data, good.len);
        bad.data[4] = (uint8_t)(CATMAN_VERSION + 1);  /* break version check */
        CHECK(catman_parse(bad.data, bad.len, &m) == -1);
        buf_free(&bad);
    }

    /* truncated header: fewer than HDR_LEN bytes */
    {
        CHECK(catman_parse(good.data, HDR_LEN - 1, &m) == -1);
        CHECK(catman_parse(good.data, 0, &m) == -1);
    }

    /* truncated hash array: provide fewer than nchunks*BK_HASH_LEN bytes
       after the header -- one byte short is enough */
    {
        size_t short_len = HDR_LEN + (size_t)nchunks * BK_HASH_LEN - 1;
        CHECK(catman_parse(good.data, short_len, &m) == -1);

        /* providing header + 0 hash bytes when nchunks > 0 also fails */
        CHECK(catman_parse(good.data, HDR_LEN, &m) == -1);
    }

    buf_free(&good);
}

/* ---- catman_free under ASan ------------------------------------------- */

static void test_free(void)
{
    uint32_t nchunks = 4;
    uint8_t hashes[4 * BK_HASH_LEN];
    fill(hashes, sizeof hashes, 0xCAFEFACEULL);

    Buf out;
    buf_init(&out);
    catman_serialize(&out, 1LL, 4096LL, hashes, nchunks);

    CatManifest m;
    CHECK(catman_parse(out.data, out.len, &m) == 0);
    catman_free(&m);

    /* Double-free must be safe: catman_free sets hashes=NULL */
    CHECK(m.hashes == NULL);
    catman_free(&m);  /* must not crash */

    buf_free(&out);
}

/* ----------------------------------------------------------------------- */

int main(void)
{
    test_roundtrip();
    test_zero_chunks();
    test_reject_bad();
    test_free();

    TEST_DONE("test_catman");
}
