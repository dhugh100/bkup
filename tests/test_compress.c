/* Unit tests for src/common/compress.c (zstd wrap).

   Build + run (see run_tests.sh):
     gcc -std=c11 -D_GNU_SOURCE -Isrc tests/test_compress.c \
         obj/common/compress.o obj/common/util.o obj/common/log.o \
         $(pkg-config --libs sqlite3 libzstd libsodium libssh2) \
         -o /tmp/test_compress && /tmp/test_compress
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "common/compress.h"
#include "common/util.h"
#include "test_common.h"

static int fails;

/* Round-trip helper: compress src, decompress, check byte-identical. */
static void roundtrip(const uint8_t *src, size_t n, const char *label)
{
    Buf comp;
    buf_init(&comp);
    zstd_compress(src, n, BK_ZSTD_LEVEL, &comp);
    CHECK(comp.len > 0);

    Buf pt;
    buf_init(&pt);
    int rc = zstd_decompress(comp.data, comp.len, &pt);
    if (rc != 0) {
        printf("FAIL roundtrip(%s): zstd_decompress returned %d\n", label, rc);
        fails++;
    } else {
        if (pt.len != n) {
            printf("FAIL roundtrip(%s): len %zu != %zu\n", label, pt.len, n);
            fails++;
        } else if (n > 0 && memcmp(pt.data, src, n) != 0) {
            printf("FAIL roundtrip(%s): bytes differ\n", label);
            fails++;
        }
    }
    buf_free(&comp);
    buf_free(&pt);
}

static void test_roundtrips(void)
{
    /* random (effectively incompressible) */
    size_t N = 256u * 1024u;
    uint8_t *rand_buf = xmalloc(N);
    fill(rand_buf, N, 0xCAFEBABEULL);
    roundtrip(rand_buf, N, "random 256KiB");
    free(rand_buf);

    /* highly compressible: all zeros */
    uint8_t *zeros = xcalloc(N, 1);
    roundtrip(zeros, N, "zeros 256KiB");
    free(zeros);

    /* empty */
    roundtrip(NULL, 0, "empty");

    /* ~4 MiB (larger than a typical chunk) */
    size_t M = 4u * 1024u * 1024u;
    uint8_t *big = xmalloc(M);
    fill(big, M, 0xDEADBEEFULL);
    roundtrip(big, M, "random 4MiB");
    free(big);
}

static void test_errors(void)
{
    /* Build a valid frame first */
    size_t N = 64 * 1024;
    uint8_t *src = xmalloc(N);
    fill(src, N, 0x1234ULL);
    Buf comp;
    buf_init(&comp);
    zstd_compress(src, N, BK_ZSTD_LEVEL, &comp);
    free(src);

    /* corrupt frame: flip the zstd magic bytes (first 4 bytes of the frame).
       ZSTD_getFrameContentSize detects the bad magic and returns CONTENTSIZE_ERROR,
       which our zstd_decompress wrapper maps to -1.
       Note: ZSTD_compress does not add a per-frame checksum by default, so
       corrupting data bytes mid-frame may silently produce wrong plaintext;
       that is acceptable because in bkup the BLAKE2b hash on top catches it.
       What we test here is detection of a completely garbled frame header. */
    Buf bad;
    buf_init(&bad);
    buf_append(&bad, comp.data, comp.len);
    bad.data[0] ^= 0xFF;   /* clobber zstd magic 0xFD2FB528 */
    bad.data[1] ^= 0xFF;

    Buf out;
    buf_init(&out);
    CHECK(zstd_decompress(bad.data, bad.len, &out) == -1);
    buf_free(&out);
    buf_free(&bad);

    /* truncated frame: pass only the first half.
       ZSTD_getFrameContentSize reads the header (which is still intact) and
       returns the declared content size.  ZSTD_decompress then fails because
       the frame is incomplete. */
    buf_init(&out);
    size_t half = comp.len / 2;
    CHECK(zstd_decompress(comp.data, half, &out) == -1);
    buf_free(&out);

    /* frame self-describes size: decompress with no external length succeeds */
    buf_init(&out);
    CHECK(zstd_decompress(comp.data, comp.len, &out) == 0);
    CHECK(out.len == N);
    buf_free(&out);

    buf_free(&comp);
}

int main(void)
{
    test_roundtrips();
    test_errors();

    TEST_DONE("test_compress");
}
