#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "util.h"
#include "log.h"

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) die("out of memory (malloc %zu)", n);
    return p;
}

void *xcalloc(size_t n, size_t sz)
{
    void *p = calloc(n ? n : 1, sz ? sz : 1);
    if (!p) die("out of memory (calloc %zu x %zu)", n, sz);
    return p;
}

void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) die("out of memory (realloc %zu)", n);
    return q;
}

char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = xmalloc(n);
    memcpy(p, s, n);
    return p;
}

static const char HEXD[] = "0123456789abcdef";

void hex_encode(const uint8_t *in, size_t n, char *out_hex)
{
    for (size_t i = 0; i < n; i++) {
        out_hex[2 * i]     = HEXD[in[i] >> 4];
        out_hex[2 * i + 1] = HEXD[in[i] & 0xf];
    }
    out_hex[2 * n] = '\0';
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int hex_decode(const char *hex, uint8_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int hi = hexval(hex[2 * i]);
        int lo = hexval(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    /* allow trailing NUL or newline only */
    char t = hex[2 * n];
    if (t != '\0' && t != '\n') return -1;
    return 0;
}

char *path_join(const char *base, const char *name)
{
    size_t bl = strlen(base);
    int slash = (bl > 0 && base[bl - 1] == '/') ? 0 : 1;
    size_t nl = strlen(name);
    char *p = xmalloc(bl + slash + nl + 1);
    memcpy(p, base, bl);
    if (slash) p[bl] = '/';
    memcpy(p + bl + slash, name, nl + 1);
    return p;
}

char *path_expand(const char *p)
{
    if (p[0] == '~' && p[1] == '/') {
        const char *home = getenv("HOME");
        if (home) return path_join(home, p + 2);
    }
    return xstrdup(p);
}

int mkdir_p(const char *path, unsigned mode)
{
    char *tmp = xstrdup(path);
    int rc = 0;
    for (char *s = tmp + 1; *s; s++) {
        if (*s == '/') {
            *s = '\0';
            if (mkdir(tmp, mode) != 0 && errno != EEXIST) { rc = -1; break; }
            *s = '/';
        }
    }
    if (rc == 0 && mkdir(tmp, mode) != 0 && errno != EEXIST) rc = -1;
    free(tmp);
    return rc;
}

int read_file(const char *path, uint8_t **buf, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    Buf b;
    buf_init(&b);
    uint8_t tmp[65536];
    size_t n;
    while ((n = fread(tmp, 1, sizeof tmp, f)) > 0)
        buf_append(&b, tmp, n);
    int err = ferror(f);
    fclose(f);
    if (err) { buf_free(&b); return -1; }
    *buf = b.data;
    *len = b.len;
    return 0;
}

void buf_init(Buf *b)
{
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void buf_reserve(Buf *b, size_t extra)
{
    if (b->len + extra <= b->cap) return;
    size_t cap = b->cap ? b->cap : 4096;
    while (cap < b->len + extra) cap *= 2;
    b->data = xrealloc(b->data, cap);
    b->cap = cap;
}

void buf_append(Buf *b, const void *p, size_t n)
{
    buf_reserve(b, n);
    memcpy(b->data + b->len, p, n);
    b->len += n;
}

void buf_consume_front(Buf *b, size_t n)
{
    if (n >= b->len) { b->len = 0; return; }
    memmove(b->data, b->data + n, b->len - n);
    b->len -= n;
}

void buf_free(Buf *b)
{
    free(b->data);
    buf_init(b);
}
