/* Unit tests for src/common/util.c:
   hex_encode/hex_decode, path_join, path_expand, mkdir_p, Buf, read_file.

   Build + run (see run_tests.sh):
     gcc -std=c11 -D_GNU_SOURCE -Isrc tests/test_util.c \
         obj/common/util.o obj/common/log.o \
         $(pkg-config --libs sqlite3 libzstd libsodium libssh2) \
         -o /tmp/test_util && /tmp/test_util
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "common/util.h"
#include "test_common.h"

static int fails;

/* ---- hex_encode / hex_decode ----------------------------------------- */

static void test_hex(void)
{
    /* round-trip: random bytes */
    uint8_t src[32];
    fill(src, sizeof src, 0xABCDEF01ULL);
    char hex[65];
    hex_encode(src, sizeof src, hex);
    CHECK(strlen(hex) == 64);           /* NUL-terminated 2n chars */
    CHECK(hex[64] == '\0');

    uint8_t dst[32];
    CHECK(hex_decode(hex, dst, sizeof dst) == 0);
    CHECK_MEMEQ(src, dst, sizeof src);  /* exact bytes back */

    /* known value: 0x00..0x0f */
    uint8_t known[8] = {0x00,0x11,0x22,0xaa,0xbb,0xcc,0xde,0xad};
    char kh[17];
    hex_encode(known, 8, kh);
    CHECK(strcmp(kh, "001122aabbccddead") != 0); /* sanity: wrong length fails */
    CHECK(strcmp(kh, "001122aabbccdead") == 0);

    /* hex_decode with trailing newline: OK */
    char hexnl[65];
    memcpy(hexnl, hex, 64);
    hexnl[64] = '\n';
    /* hex_decode expects n=32 and checks hex[64], which is '\n' -> ok */
    CHECK(hex_decode(hexnl, dst, 32) == 0);

    /* hex_decode rejects odd effective length:
       "abc" decoded as 1 byte leaves 'c' at hex[2], which is not '\0'/'\n' */
    uint8_t tmp[2];
    CHECK(hex_decode("abc", tmp, 1) == -1);  /* trailing 'c' -> reject */

    /* hex_decode rejects non-hex characters */
    CHECK(hex_decode("zz", tmp, 1) == -1);
    CHECK(hex_decode("0g", tmp, 1) == -1);
    CHECK(hex_decode("GH", tmp, 1) == -1);

    /* uppercase hex accepted */
    uint8_t up[1];
    CHECK(hex_decode("AB", up, 1) == 0);
    CHECK(up[0] == 0xAB);

    /* encode produces NUL-terminated 2n chars for n=0 */
    char empty_hex[1] = {0x7f};   /* poison */
    hex_encode(src, 0, empty_hex);
    CHECK(empty_hex[0] == '\0');
}

/* ---- path_join --------------------------------------------------------- */

static void test_path_join(void)
{
    /* normal: no trailing slash */
    char *r = path_join("/a/b", "c.txt");
    CHECKEQ_STR(r, "/a/b/c.txt");
    free(r);

    /* base has trailing slash: no double slash */
    r = path_join("/a/b/", "c.txt");
    CHECKEQ_STR(r, "/a/b/c.txt");
    free(r);

    /* empty name: base + "/" */
    r = path_join("/a/b", "");
    CHECKEQ_STR(r, "/a/b/");
    free(r);

    /* both non-trivial */
    r = path_join("/x", "y/z");
    CHECKEQ_STR(r, "/x/y/z");
    free(r);
}

/* ---- path_expand ------------------------------------------------------- */

static void test_path_expand(void)
{
    /* save and restore HOME so we don't pollute the environment */
    const char *saved = getenv("HOME");
    char saved_home[512] = "";
    if (saved) snprintf(saved_home, sizeof saved_home, "%s", saved);

    setenv("HOME", "/home/testuser", 1);

    /* ~/x expands to $HOME/x */
    char *e = path_expand("~/foo/bar");
    CHECKEQ_STR(e, "/home/testuser/foo/bar");
    free(e);

    /* bare ~ with path */
    e = path_expand("~/");
    CHECKEQ_STR(e, "/home/testuser/");
    free(e);

    /* no tilde at all: returns a copy */
    e = path_expand("/absolute/path");
    CHECKEQ_STR(e, "/absolute/path");
    free(e);

    e = path_expand("relative/path");
    CHECKEQ_STR(e, "relative/path");
    free(e);

    /* tilde NOT at position 0: must be untouched */
    e = path_expand("prefix~/path");
    CHECKEQ_STR(e, "prefix~/path");
    free(e);

    e = path_expand("/a/~/b");
    CHECKEQ_STR(e, "/a/~/b");
    free(e);

    /* restore HOME */
    if (saved_home[0])
        setenv("HOME", saved_home, 1);
    else
        unsetenv("HOME");
}

/* ---- mkdir_p ----------------------------------------------------------- */

static void test_mkdir_p(void)
{
    char base[64];
    tmpdir(base, sizeof base);

    /* deep nested create */
    char deep[256];
    snprintf(deep, sizeof deep, "%s/a/b/c/d", base);
    CHECK(mkdir_p(deep, 0755) == 0);
    struct stat st;
    CHECK(stat(deep, &st) == 0 && S_ISDIR(st.st_mode));

    /* idempotent: calling again on existing path succeeds */
    CHECK(mkdir_p(deep, 0755) == 0);

    /* partial-existing path: shares prefix with existing */
    char sibling[256];
    snprintf(sibling, sizeof sibling, "%s/a/b/e/f", base);
    CHECK(mkdir_p(sibling, 0755) == 0);
    CHECK(stat(sibling, &st) == 0 && S_ISDIR(st.st_mode));

    rmtree_local(base);
}

/* ---- Buf --------------------------------------------------------------- */

static void test_buf(void)
{
    Buf b;
    buf_init(&b);
    CHECK(b.data == NULL);
    CHECK(b.len == 0);
    CHECK(b.cap == 0);

    /* append zero bytes: no crash, len stays 0 */
    buf_append(&b, "x", 0);
    CHECK(b.len == 0);

    /* small appends that stay within one allocation */
    buf_append(&b, "hello", 5);
    CHECK(b.len == 5);
    CHECK(memcmp(b.data, "hello", 5) == 0);

    buf_append(&b, " world", 6);
    CHECK(b.len == 11);
    CHECK(memcmp(b.data, "hello world", 11) == 0);

    /* buf_reserve: pre-allocate extra capacity */
    size_t old_cap = b.cap;
    buf_reserve(&b, 4096);
    CHECK(b.cap >= b.len + 4096);
    CHECK(b.len == 11);  /* data unchanged */
    CHECK(memcmp(b.data, "hello world", 11) == 0);
    (void)old_cap;

    /* append across realloc: fill past current capacity */
    uint8_t big[8192];
    fill(big, sizeof big, 0x1234ULL);
    buf_append(&b, big, sizeof big);
    CHECK(b.len == 11 + 8192);
    CHECK(memcmp(b.data + 11, big, sizeof big) == 0);

    /* buf_consume_front: drop first n bytes, shift the rest */
    buf_consume_front(&b, 6);       /* drop "hello " */
    CHECK(b.len == 11 + 8192 - 6);
    CHECK(memcmp(b.data, "world", 5) == 0);
    CHECK(memcmp(b.data + 5, big, 8192) == 0);

    /* consume all */
    size_t rem = b.len;
    buf_consume_front(&b, rem);
    CHECK(b.len == 0);

    /* consume more than len: clamps to 0 */
    buf_append(&b, "abc", 3);
    buf_consume_front(&b, 100);
    CHECK(b.len == 0);

    /* buf_free: resets to init state */
    buf_append(&b, "test", 4);
    buf_free(&b);
    CHECK(b.data == NULL);
    CHECK(b.len == 0);
    CHECK(b.cap == 0);
}

/* ---- read_file --------------------------------------------------------- */

static void test_read_file(void)
{
    char base[64];
    tmpdir(base, sizeof base);

    char path[256];
    snprintf(path, sizeof path, "%s/testfile.bin", base);

    /* write known content */
    const uint8_t content[] = {0x00, 0x01, 0x7f, 0xff, 0x42, 0xde, 0xad};
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);
    if (fd >= 0) {
        ssize_t w = write(fd, content, sizeof content);
        CHECK((size_t)w == sizeof content);
        close(fd);
    }

    /* round-trip */
    uint8_t *buf = NULL;
    size_t len = 0;
    CHECK(read_file(path, &buf, &len) == 0);
    CHECK(len == sizeof content);
    if (buf && len == sizeof content)
        CHECK_MEMEQ(buf, content, sizeof content);
    free(buf);

    /* empty file */
    char epath[256];
    snprintf(epath, sizeof epath, "%s/empty.bin", base);
    fd = open(epath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) close(fd);
    buf = NULL; len = 1; /* poison */
    CHECK(read_file(epath, &buf, &len) == 0);
    CHECK(len == 0);
    free(buf);

    /* missing file: returns nonzero */
    CHECK(read_file("/nonexistent/path/bkup_test_xyz", &buf, &len) != 0);

    rmtree_local(base);
}

/* ----------------------------------------------------------------------- */

int main(void)
{
    test_hex();
    test_path_join();
    test_path_expand();
    test_mkdir_p();
    test_buf();
    test_read_file();

    TEST_DONE("test_util");
}
