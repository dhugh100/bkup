/* Smoke test for the chunked, deduplicated catalog (the #2 design).

   Exercises the real chunker, codec, and manifest format with no server:
     1. chunk a synthetic VACUUMed-DB buffer with CAT_PARAMS, build a manifest,
        seal each chunk (zstd + crypto_seal) into an in-memory "repo", then
        reassemble exactly as fetch-catalog does (open + decompress + per-chunk
        hash verify) and assert byte-identical recovery.
     2. dedup locality: a small in-place edit must change only a couple of
        chunk hashes -- the whole economic point, since a continuous push then
        uploads a handful of chunks instead of the entire catalog.

   Build + run (see CLAUDE.md "Tests"):
     make all
     gcc -std=c11 -D_GNU_SOURCE -Isrc tests/test_catalog_chunk.c \
         obj/common/catalog_chunk.o obj/common/chunk.o obj/common/crypto.o \
         obj/common/compress.o obj/common/util.o obj/common/log.o \
         $(pkg-config --libs libsodium libzstd) -o /tmp/test_catalog_chunk
     /tmp/test_catalog_chunk
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/chunk.h"
#include "common/catalog_chunk.h"
#include "common/crypto.h"
#include "common/compress.h"
#include "common/types.h"
#include "common/util.h"

static int fails;
#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); fails++; } } while (0)

/* deterministic pseudo-random fill (splitmix64) so the test is reproducible */
static void fill(uint8_t *b, size_t n, uint64_t seed)
{
    uint64_t s = seed;
    for (size_t i = 0; i < n; i++) {
        s += 0x9E3779B97F4A7C15ULL;
        uint64_t z = s;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        b[i] = (uint8_t)(z >> 31);
    }
}

/* Chunk `buf` with CAT_PARAMS, append each chunk hash to *hashes; return count. */
static uint32_t chunkit(const uint8_t *buf, size_t len, Buf *hashes)
{
    size_t off = 0;
    uint32_t n = 0;
    while (off < len) {
        size_t clen = chunk_cut_ex(buf + off, len - off, 1, &CAT_PARAMS);
        uint8_t h[BK_HASH_LEN];
        bk_hash(buf + off, clen, h);
        buf_append(hashes, h, BK_HASH_LEN);
        off += clen;
        n++;
    }
    return n;
}

/* count hashes in `b` (n entries) absent from `a` (m entries) -- O(n*m), fine
   for a few hundred chunks. */
static uint32_t count_new(const Buf *b, uint32_t n, const Buf *a, uint32_t m)
{
    uint32_t newc = 0;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *hb = b->data + (size_t)i * BK_HASH_LEN;
        int found = 0;
        for (uint32_t j = 0; j < m; j++)
            if (!memcmp(hb, a->data + (size_t)j * BK_HASH_LEN, BK_HASH_LEN)) {
                found = 1; break;
            }
        if (!found) newc++;
    }
    return newc;
}

typedef struct { uint8_t h[BK_HASH_LEN]; Buf ct; } Stored;

static int store_find(const Stored *s, int n, const uint8_t *h)
{
    for (int j = 0; j < n; j++)
        if (!memcmp(s[j].h, h, BK_HASH_LEN)) return j;
    return -1;
}

int main(void)
{
    crypto_global_init();
    Key key;
    memset(&key, 0x5a, sizeof key);          /* fixed test key (no KDF needed) */

    size_t N = 4u * 1024 * 1024;             /* a ~4 MiB synthetic catalog */
    uint8_t *db = xmalloc(N);
    fill(db, N, 0xC0FFEEULL);

    /* ---- chunk + manifest ---- */
    Buf h1; buf_init(&h1);
    uint32_t n1 = chunkit(db, N, &h1);
    CHECK(n1 > 0);
    double avg = (double)N / (n1 ? n1 : 1);
    CHECK(avg >= 8.0 * 1024 && avg <= 64.0 * 1024);   /* CAT_PARAMS ballpark */

    Buf man; buf_init(&man);
    catman_serialize(&man, 7, (long long)N, h1.data, n1);
    CatManifest m;
    CHECK(catman_parse(man.data, man.len, &m) == 0);
    CHECK(m.nchunks == n1);
    CHECK(m.db_size == (long long)N);

    /* ---- simulate upload: seal each unique chunk into an in-memory repo ---- */
    Stored *store = xcalloc(n1, sizeof *store);
    int nstore = 0;
    size_t off = 0;
    for (uint32_t i = 0; i < n1; i++) {
        size_t clen = chunk_cut_ex(db + off, N - off, 1, &CAT_PARAMS);
        const uint8_t *h = h1.data + (size_t)i * BK_HASH_LEN;
        if (store_find(store, nstore, h) < 0) {
            Buf comp; buf_init(&comp);
            zstd_compress(db + off, clen, BK_ZSTD_LEVEL, &comp);
            Buf ct; buf_init(&ct);
            crypto_seal(&key, comp.data, comp.len, &ct);
            buf_free(&comp);
            memcpy(store[nstore].h, h, BK_HASH_LEN);
            store[nstore].ct = ct;
            nstore++;
        }
        off += clen;
    }

    /* ---- reassemble from the manifest, exactly like fetch-catalog ---- */
    Buf rebuilt; buf_init(&rebuilt);
    for (uint32_t i = 0; i < m.nchunks; i++) {
        const uint8_t *h = m.hashes + (size_t)i * BK_HASH_LEN;
        int idx = store_find(store, nstore, h);
        CHECK(idx >= 0);
        if (idx < 0) break;
        Buf comp; buf_init(&comp);
        CHECK(crypto_open(&key, store[idx].ct.data, store[idx].ct.len, &comp) == 0);
        Buf pt; buf_init(&pt);
        CHECK(zstd_decompress(comp.data, comp.len, &pt) == 0);
        buf_free(&comp);
        uint8_t vh[BK_HASH_LEN];
        bk_hash(pt.data, pt.len, vh);
        CHECK(memcmp(vh, h, BK_HASH_LEN) == 0);     /* integrity check */
        buf_append(&rebuilt, pt.data, pt.len);
        buf_free(&pt);
    }
    CHECK(rebuilt.len == N);
    CHECK(rebuilt.len == N && memcmp(rebuilt.data, db, N) == 0);  /* byte-exact */

    /* ---- dedup locality: a localized edit dirties only a couple of chunks ---- */
    for (int i = 0; i < 64; i++) db[N / 2 + i] ^= 0xFF;
    Buf h2; buf_init(&h2);
    uint32_t n2 = chunkit(db, N, &h2);
    uint32_t newc = count_new(&h2, n2, &h1, n1);
    printf("locality: %u of %u chunks changed after a 64-byte edit\n", newc, n2);
    CHECK(newc <= 4);

    if (fails == 0)
        printf("OK: catalog round-trip + dedup locality "
               "(%u chunks, avg %.0f B)\n", n1, avg);

    /* Free everything so the suite is clean under LeakSanitizer
       (BKUP_TEST_SAN=1) -- a leaking test would mask a real future leak. */
    for (int i = 0; i < nstore; i++) buf_free(&store[i].ct);
    free(store);
    catman_free(&m);
    buf_free(&h1);
    buf_free(&man);
    buf_free(&h2);
    buf_free(&rebuilt);
    free(db);
    return fails ? 1 : 0;
}
