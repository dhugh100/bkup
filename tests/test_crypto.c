/* Unit tests for src/common/crypto.c.

   Uses the low-cost KdfParams from test_common.h (ops=1, mem=8192) instead of
   crypto_kdf_default() which uses MODERATE cost (~1-2 seconds).  Even with
   OPSLIMIT_MIN the tests cover the full KDF path; speed matters here because
   derive_key is called several times.

   BLAKE2b known-answer vector:
     bk_hash("abc", 3) == bddd813c634239723171ef3fee98579b94964e3bb1cb3e427262c8c068d52319
   This is the standard BLAKE2b-256 (unkeyed, 32-byte output) test vector for
   the message "abc", per the BLAKE2 specification.  A mismatch here means the
   hash function was silently changed or the libsodium crypto_generichash
   output changed -- both are catastrophic for dedup.

   Build + run (see run_tests.sh):
     gcc -std=c11 -D_GNU_SOURCE -Isrc tests/test_crypto.c \
         obj/common/crypto.o obj/common/util.o obj/common/log.o \
         $(pkg-config --libs sqlite3 libzstd libsodium libssh2) \
         -o /tmp/test_crypto && /tmp/test_crypto
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "common/crypto.h"
#include "common/util.h"
#include "test_common.h"

static int fails;

/* ---- crypto_seal / crypto_open ---------------------------------------- */

static void test_seal_open(void)
{
    Key key;
    memset(&key, TC_KEY_BYTE, sizeof key);

    /* various sizes including empty */
    size_t sizes[] = { 0, 1, 15, 16, 255, 1024, 64*1024 };
    for (size_t si = 0; si < sizeof sizes / sizeof *sizes; si++) {
        size_t n = sizes[si];
        uint8_t *pt = n ? xmalloc(n) : NULL;
        if (n) fill(pt, n, (uint64_t)si);

        Buf ct;
        buf_init(&ct);
        crypto_seal(&key, pt ? pt : (const uint8_t *)"", n, &ct);
        CHECK(ct.len > n);  /* ciphertext is larger (overhead) */

        Buf recovered;
        buf_init(&recovered);
        CHECK(crypto_open(&key, ct.data, ct.len, &recovered) == 0);
        CHECK(recovered.len == n);
        if (n) CHECK_MEMEQ(recovered.data, pt, n);

        buf_free(&ct);
        buf_free(&recovered);
        free(pt);
    }

    /* wrong key returns -1 */
    {
        uint8_t pt[32] = {0};
        fill(pt, sizeof pt, 42ULL);
        Buf ct;
        buf_init(&ct);
        crypto_seal(&key, pt, sizeof pt, &ct);

        Key bad_key;
        memset(&bad_key, 0x00, sizeof bad_key);  /* different from 0x5a */
        Buf out;
        buf_init(&out);
        CHECK(crypto_open(&bad_key, ct.data, ct.len, &out) == -1);
        buf_free(&ct);
        buf_free(&out);
    }

    /* byte-flip in ciphertext: auth must catch it */
    {
        uint8_t pt[64];
        fill(pt, sizeof pt, 99ULL);
        Buf ct;
        buf_init(&ct);
        crypto_seal(&key, pt, sizeof pt, &ct);

        /* flip one byte in the auth/ciphertext body */
        ct.data[ct.len / 2] ^= 0x01;
        Buf out;
        buf_init(&out);
        CHECK(crypto_open(&key, ct.data, ct.len, &out) == -1);
        buf_free(&ct);
        buf_free(&out);
    }
}

/* ---- bk_hash ----------------------------------------------------------- */

static void test_bk_hash(void)
{
    /* Determinism: same input -> same output */
    uint8_t data[256];
    fill(data, sizeof data, 0xFADE0123ULL);
    uint8_t h1[BK_HASH_LEN], h2[BK_HASH_LEN];
    bk_hash(data, sizeof data, h1);
    bk_hash(data, sizeof data, h2);
    CHECK_MEMEQ(h1, h2, BK_HASH_LEN);

    /* Different input -> different output (statistical near-certainty) */
    data[0] ^= 0x01;
    uint8_t h3[BK_HASH_LEN];
    bk_hash(data, sizeof data, h3);
    CHECK(memcmp(h1, h3, BK_HASH_LEN) != 0);

    /*
     * KNOWN-ANSWER VECTOR: BLAKE2b-256 (unkeyed, 32-byte output) of "abc".
     * Standard test vector from the BLAKE2 specification.
     * libsodium crypto_generichash(out,32,"abc",3,NULL,0) must produce this.
     * A failure here means the hash function semantics changed -- investigate
     * before doing anything else.
     */
    static const uint8_t EXPECTED_ABC[BK_HASH_LEN] = {
        0xbd, 0xdd, 0x81, 0x3c, 0x63, 0x42, 0x39, 0x72,
        0x31, 0x71, 0xef, 0x3f, 0xee, 0x98, 0x57, 0x9b,
        0x94, 0x96, 0x4e, 0x3b, 0xb1, 0xcb, 0x3e, 0x42,
        0x72, 0x62, 0xc8, 0xc0, 0x68, 0xd5, 0x23, 0x19,
    };
    uint8_t got[BK_HASH_LEN];
    bk_hash((const uint8_t *)"abc", 3, got);
    if (memcmp(got, EXPECTED_ABC, BK_HASH_LEN) != 0) {
        char hex[BK_HASH_LEN * 2 + 1];
        hex_encode(got, BK_HASH_LEN, hex);
        printf("FAIL bk_hash KAV: got %s\n", hex);
        printf("     expected   : bddd813c634239723171ef3fee98579b94964e3bb1cb3e427262c8c068d52319\n");
        fails++;
    }

    /* empty input is also deterministic */
    uint8_t he[BK_HASH_LEN];
    bk_hash(NULL, 0, he);
    uint8_t he2[BK_HASH_LEN];
    bk_hash((const uint8_t *)"", 0, he2);
    CHECK_MEMEQ(he, he2, BK_HASH_LEN);
}

/* ---- crypto_derive_key ------------------------------------------------- */

static void test_derive_key(void)
{
    KdfParams p = tc_kdf_params();   /* ops=1, mem=8192 -- fast */
    Key k1, k2;

    /* deterministic: same inputs -> same key */
    CHECK(crypto_derive_key("passphrase", &p, &k1) == 0);
    CHECK(crypto_derive_key("passphrase", &p, &k2) == 0);
    CHECK_MEMEQ(k1.k, k2.k, BK_KEYBYTES);

    /* different salt -> different key */
    KdfParams p2 = p;
    p2.salt[0] ^= 0xFF;
    Key k3;
    CHECK(crypto_derive_key("passphrase", &p2, &k3) == 0);
    CHECK(memcmp(k1.k, k3.k, BK_KEYBYTES) != 0);

    /* wrong passphrase -> different key */
    Key k4;
    CHECK(crypto_derive_key("wrong", &p, &k4) == 0);
    CHECK(memcmp(k1.k, k4.k, BK_KEYBYTES) != 0);
}

/* ---- crypto_keycheck --------------------------------------------------- */

static void test_keycheck(void)
{
    KdfParams p = tc_kdf_params();
    Key correct, wrong;
    CHECK(crypto_derive_key("correct-pass", &p, &correct) == 0);
    CHECK(crypto_derive_key("wrong-pass",   &p, &wrong)   == 0);

    Buf kc;
    buf_init(&kc);
    crypto_keycheck_make(&correct, &kc);
    CHECK(kc.len > 0);

    /* correct key verifies */
    CHECK(crypto_keycheck_verify(&correct, kc.data, kc.len) == 0);

    /* wrong key fails */
    CHECK(crypto_keycheck_verify(&wrong, kc.data, kc.len) == -1);

    buf_free(&kc);
}

/* ---- crypto_seal_file / crypto_open_file ------------------------------ */

static void test_seal_open_file(void)
{
    Key key;
    memset(&key, TC_KEY_BYTE, sizeof key);

    char base[64];
    tmpdir(base, sizeof base);

    char plainpath[256], encpath[256], decpath[256];
    snprintf(plainpath, sizeof plainpath, "%s/plain.bin",  base);
    snprintf(encpath,   sizeof encpath,   "%s/enc.bin",    base);
    snprintf(decpath,   sizeof decpath,   "%s/dec.bin",    base);

    /* write plaintext */
    size_t N = 3 * 1024 * 1024;  /* 3 MiB: forces multiple streaming chunks */
    uint8_t *pt = xmalloc(N);
    fill(pt, N, 0xFEEDFACEULL);
    int fd = open(plainpath, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    CHECK(fd >= 0);
    if (fd >= 0) {
        size_t written = 0;
        while (written < N) {
            ssize_t r = write(fd, pt + written, N - written);
            if (r <= 0) break;
            written += (size_t)r;
        }
        close(fd);
    }

    /* seal */
    CHECK(crypto_seal_file(&key, plainpath, encpath) == 0);

    /* open */
    CHECK(crypto_open_file(&key, encpath, decpath) == 0);

    /* verify byte-identical */
    uint8_t *got = NULL;
    size_t glen = 0;
    CHECK(read_file(decpath, &got, &glen) == 0);
    CHECK(glen == N);
    if (glen == N) CHECK_MEMEQ(got, pt, N);
    free(got);
    free(pt);

    /* tampered encrypted file: should fail */
    /* flip a byte in the middle of the encrypted file */
    uint8_t *enc_buf = NULL;
    size_t enc_len = 0;
    CHECK(read_file(encpath, &enc_buf, &enc_len) == 0);
    if (enc_buf && enc_len > 50) {
        enc_buf[enc_len / 2] ^= 0x55;
        int tfd = open(encpath, O_WRONLY | O_TRUNC, 0600);
        if (tfd >= 0) {
            write(tfd, enc_buf, enc_len);
            close(tfd);
        }
        free(enc_buf);
        char decpath2[256];
        snprintf(decpath2, sizeof decpath2, "%s/dec2.bin", base);
        CHECK(crypto_open_file(&key, encpath, decpath2) == -1);
    } else {
        free(enc_buf);
    }

    rmtree_local(base);
}

/* ---- crypto_random ----------------------------------------------------- */

static void test_crypto_random(void)
{
    uint8_t a[32], b[32];
    memset(a, 0, sizeof a);
    memset(b, 0, sizeof b);
    crypto_random(a, sizeof a);
    crypto_random(b, sizeof b);
    /* Two independent draws should differ (probability 1 - 2^-256) */
    CHECK(memcmp(a, b, sizeof a) != 0);
    /* Should not be all zeros */
    int all_zero_a = 1;
    for (int i = 0; i < 32; i++) if (a[i]) { all_zero_a = 0; break; }
    CHECK(!all_zero_a);
}

/* ----------------------------------------------------------------------- */

int main(void)
{
    crypto_global_init();

    test_seal_open();
    test_bk_hash();
    test_derive_key();
    test_keycheck();
    test_seal_open_file();
    test_crypto_random();

    TEST_DONE("test_crypto");
}
