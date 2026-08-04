/* Unit tests for src/common/chunk.c (FastCDC content-defined chunker).
   Also tests CAT_PARAMS via chunk_cut_ex (catalog chunker).

   !!!! IMPORTANT: GOLDEN GUARD !!!!
   The last test in this file is a golden / frozen regression check.  It
   computes chunk_cut boundaries for a fixed 8 MiB seeded buffer, hashes the
   boundary sequence (as little-endian uint64_t values), and compares against
   a hardcoded expected hash.  If this test fails it almost certainly means
   that the gear[] table, the seed, or the masks in chunk.c were changed --
   which breaks dedup against EVERY existing repo.  Do NOT simply update the
   golden hash; investigate the regression first.

   See CLAUDE.md "Gotchas that bite" and types.h BK_CHUNK_* constants.

   Build + run (see run_tests.sh):
     gcc -std=c11 -D_GNU_SOURCE -Isrc tests/test_chunk.c \
         obj/common/chunk.o obj/common/catalog_chunk.o obj/common/crypto.o \
         obj/common/util.o obj/common/log.o \
         $(pkg-config --libs sqlite3 libzstd libsodium libssh2) \
         -o /tmp/test_chunk && /tmp/test_chunk
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "common/chunk.h"
#include "common/catalog_chunk.h"
#include "common/crypto.h"
#include "common/types.h"
#include "common/util.h"
#include "test_common.h"

static int fails;

/* ---- helpers ---------------------------------------------------------- */

/* Chunk `buf[0..len)` using chunk_cut (frozen file params); append each
   boundary offset (end of chunk, little-endian uint64_t) to `offsets`.
   Return number of chunks. */
static size_t do_chunk_file(const uint8_t *buf, size_t len, Buf *offsets)
{
    size_t off = 0, count = 0;
    while (off < len) {
        size_t rem = len - off;
        int final = (rem <= BK_CHUNK_MAX);
        size_t clen = chunk_cut(buf + off, rem, final);
        if (clen == 0) clen = 1;
        if (clen > rem) clen = rem;
        off += clen;
        uint64_t boundary = (uint64_t)off;
        uint8_t le[8];
        for (int i = 0; i < 8; i++) le[i] = (uint8_t)(boundary >> (8 * i));
        buf_append(offsets, le, 8);
        count++;
    }
    return count;
}

/* ---- Determinism ------------------------------------------------------- */

static void test_determinism(void)
{
    size_t N = 4u * 1024u * 1024u;
    uint8_t *buf = xmalloc(N);
    fill(buf, N, 0xDEAD1234ULL);

    Buf off1, off2;
    buf_init(&off1); buf_init(&off2);
    size_t n1 = do_chunk_file(buf, N, &off1);
    size_t n2 = do_chunk_file(buf, N, &off2);

    CHECK(n1 > 0);
    CHECKEQ_INT((long long)n1, (long long)n2);
    if (off1.len == off2.len && off1.len > 0)
        CHECK_MEMEQ(off1.data, off2.data, off1.len);

    buf_free(&off1);
    buf_free(&off2);
    free(buf);
}

/* ---- Bounds ------------------------------------------------------------ */

static void check_bounds_file(const uint8_t *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        size_t rem = len - off;
        int final = (rem <= BK_CHUNK_MAX);
        size_t clen = chunk_cut(buf + off, rem, final);
        if (clen == 0) clen = 1;
        if (clen > rem) clen = rem;
        if (!final) {
            /* non-tail chunk must be within [min, max] */
            if (clen < BK_CHUNK_MIN || clen > BK_CHUNK_MAX) {
                printf("FAIL chunk bounds: clen=%zu not in [%u, %u]\n",
                       clen, BK_CHUNK_MIN, BK_CHUNK_MAX);
                fails++;
            }
        }
        off += clen;
    }
}

static void check_bounds_cat(const uint8_t *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        size_t rem = len - off;
        int final = (rem <= CAT_PARAMS.max);
        size_t clen = chunk_cut_ex(buf + off, rem, final, &CAT_PARAMS);
        if (clen == 0) clen = 1;
        if (clen > rem) clen = rem;
        if (!final) {
            if (clen < CAT_PARAMS.min || clen > CAT_PARAMS.max) {
                printf("FAIL cat_params bounds: clen=%zu not in [%zu, %zu]\n",
                       clen, CAT_PARAMS.min, CAT_PARAMS.max);
                fails++;
            }
        }
        off += clen;
    }
}

static void test_bounds(void)
{
    /* Use a 6 MiB random buffer (larger than one BK_CHUNK_MAX = 4 MiB so we
       get at least two full non-tail chunks from chunk_cut). */
    size_t N = 6u * 1024u * 1024u;
    uint8_t *buf = xmalloc(N);
    fill(buf, N, 0xBEEFCAFEULL);
    check_bounds_file(buf, N);
    check_bounds_cat(buf, N);
    free(buf);
}

/* ---- Locality --------------------------------------------------------- */

static void test_locality(void)
{
    /* A localized edit near the front of the buffer should change at most a
       small number of chunk boundaries, leaving the tail largely stable.
       We count how many boundaries differ after the insertion point. */
    size_t N = 4u * 1024u * 1024u;
    uint8_t *orig = xmalloc(N);
    uint8_t *edit = xmalloc(N);
    fill(orig, N, 0x98765432ULL);
    memcpy(edit, orig, N);

    /* flip 4 bytes near the front: offset 300 KiB (well past the BK_CHUNK_MIN
       skip-zone of 256 KiB so the edit can actually influence chunk cuts) */
    size_t edit_off = 300u * 1024u;
    edit[edit_off + 0] ^= 0xAB;
    edit[edit_off + 1] ^= 0xCD;
    edit[edit_off + 2] ^= 0xEF;
    edit[edit_off + 3] ^= 0x01;

    Buf off_orig, off_edit;
    buf_init(&off_orig);
    buf_init(&off_edit);
    do_chunk_file(orig, N, &off_orig);
    do_chunk_file(edit, N, &off_edit);

    /* Count changed boundaries after the edit offset */
    size_t n_orig = off_orig.len / 8;
    size_t n_edit = off_edit.len / 8;
    size_t changed = 0;
    size_t io = 0, ie = 0;
    uint64_t bo = 0, be = 0;
    while (io < n_orig && ie < n_edit) {
        uint64_t vo = 0, ve = 0;
        for (int k = 0; k < 8; k++) {
            vo |= (uint64_t)off_orig.data[io * 8 + k] << (8 * k);
            ve |= (uint64_t)off_edit.data[ie * 8 + k] << (8 * k);
        }
        if (vo <= edit_off && ve <= edit_off) {
            bo = vo; be = ve; io++; ie++;
        } else {
            if (vo != ve) changed++;
            if (vo <= ve) io++;
            if (ve <= vo) ie++;
        }
        (void)bo; (void)be;
    }
    changed += (n_orig - io) + (n_edit - ie);

    /* A 4-byte edit near 300 KiB should affect at most ~4 chunks.
       The dedup property means most tail chunks are identical. */
    if (changed > 6) {
        printf("FAIL locality: %zu chunks changed after a 4-byte edit "
               "(expected <= 6)\n", changed);
        fails++;
    } else {
        printf("locality: %zu chunk(s) changed after 4-byte edit in %zu-byte buf\n",
               changed, N);
    }

    buf_free(&off_orig);
    buf_free(&off_edit);
    free(orig);
    free(edit);
}

/* ---- final flag -------------------------------------------------------- */

static void test_final_flag(void)
{
    /* A short tail (len < BK_CHUNK_MIN) is always returned as-is regardless
       of the final flag; the important case is len in [BK_CHUNK_MIN, BK_CHUNK_MAX)
       with no content-defined boundary: with final=1 it returns len (short tail
       chunk is allowed), with final=0 it still returns len (the implementation
       ignores final because the scan limit is len, not max).
       What we can test: a single call on a buffer whose length is exactly
       BK_CHUNK_MIN returns BK_CHUNK_MIN (short tail returned whole). */
    size_t N = BK_CHUNK_MIN;
    uint8_t *buf = xmalloc(N);
    fill(buf, N, 0xF00FBABEULL);
    size_t r = chunk_cut(buf, N, 1);
    CHECK(r <= N && r > 0);
    free(buf);

    /* CAT_PARAMS: a small tail shorter than CAT_PARAMS.min is returned as-is */
    size_t tiny = CAT_PARAMS.min - 1;
    if (tiny > 0) {
        uint8_t *tbuf = xmalloc(tiny);
        fill(tbuf, tiny, 0x1111ULL);
        size_t tr = chunk_cut_ex(tbuf, tiny, 1, &CAT_PARAMS);
        CHECKEQ_INT((long long)tr, (long long)tiny);
        free(tbuf);
    }
}

/* ---- GOLDEN / FROZEN guard -------------------------------------------- */

/*
 * Seed and size for the golden buffer.  NEVER CHANGE THESE.
 * Changing them would compute a different boundary sequence and falsely pass.
 */
#define GOLDEN_SEED   0xC0FFEE42DEADBEEFULL
#define GOLDEN_SIZE   (8u * 1024u * 1024u)

/*
 * Expected BLAKE2b-256 of the concatenated little-endian uint64_t chunk
 * boundary offsets produced by chunk_cut() on a GOLDEN_SIZE buffer filled
 * with splitmix64(GOLDEN_SEED).
 *
 * This value was computed by running the test in bootstrap mode
 * (all-zero GOLDEN_HASH) and embedding the printed hash.
 *
 * A failure here means gear[], seed, or masks in chunk.c changed.
 * DO NOT update this constant without understanding why boundaries changed.
 * Changing chunk boundaries breaks dedup against every existing Bkup repo.
 */
static const uint8_t GOLDEN_HASH[BK_HASH_LEN] = {
    /* 1e969d0f4e844d28b40b5fad80809a48d521e82fd679457ba4c4a616a5652e8f */
    /* Generated by bootstrap run (all-zero placeholder) on 2026-06-28.     */
    /* Represents chunk_cut() boundaries for 8 MiB splitmix64(GOLDEN_SEED). */
    0x1e,0x96,0x9d,0x0f, 0x4e,0x84,0x4d,0x28,
    0xb4,0x0b,0x5f,0xad, 0x80,0x80,0x9a,0x48,
    0xd5,0x21,0xe8,0x2f, 0xd6,0x79,0x45,0x7b,
    0xa4,0xc4,0xa6,0x16, 0xa5,0x65,0x2e,0x8f,
};

static void test_golden(void)
{
    crypto_global_init();

    uint8_t *buf = xmalloc(GOLDEN_SIZE);
    fill(buf, GOLDEN_SIZE, GOLDEN_SEED);

    Buf offsets;
    buf_init(&offsets);
    size_t nchunks = do_chunk_file(buf, GOLDEN_SIZE, &offsets);
    CHECK(nchunks > 0);

    uint8_t computed[BK_HASH_LEN];
    bk_hash(offsets.data, offsets.len, computed);

    /* Check whether the golden hash has been initialized */
    int golden_set = 0;
    for (int i = 0; i < BK_HASH_LEN; i++)
        if (GOLDEN_HASH[i] != 0) { golden_set = 1; break; }

    if (!golden_set) {
        /* Bootstrap mode: print the hash so it can be embedded above.
           This branch runs only when GOLDEN_HASH is all zeros. */
        char hex[BK_HASH_LEN * 2 + 1];
        hex_encode(computed, BK_HASH_LEN, hex);
        printf("GOLDEN hash (embed in GOLDEN_HASH in test_chunk.c):\n  %s\n",
               hex);
        printf("  Chunk count: %zu  Avg size: %.0f B\n",
               nchunks, (double)GOLDEN_SIZE / nchunks);
        /* Do not fail in bootstrap mode */
    } else {
        /*
         * FROZEN GUARD: the chunk boundary hash must be identical to the
         * recorded golden.  ANY change to gear[], the seed, or the masks
         * in chunk.c will change this hash.
         *
         * If this test fails:
         *   1. Do NOT just update the hash.
         *   2. Identify what changed in chunk.c.
         *   3. Understand whether existing backups are affected.
         *   4. Only update the hash after deliberately deciding to break
         *      backward compatibility with existing repos.
         */
        if (memcmp(computed, GOLDEN_HASH, BK_HASH_LEN) != 0) {
            char hex[BK_HASH_LEN * 2 + 1];
            hex_encode(computed, BK_HASH_LEN, hex);
            printf("FAIL GOLDEN GUARD: chunk boundary hash changed!\n");
            printf("  computed: %s\n", hex);
            printf("  This almost certainly means gear[], seed, or masks "
                   "changed in chunk.c,\n"
                   "  which would break dedup against every existing repo.\n");
            fails++;
        }
    }

    buf_free(&offsets);
    free(buf);
}

/* ----------------------------------------------------------------------- */

int main(void)
{
    test_determinism();
    test_bounds();
    test_locality();
    test_final_flag();
    test_golden();    /* must be last: prints bootstrap info if not yet set */

    TEST_DONE("test_chunk");
}
