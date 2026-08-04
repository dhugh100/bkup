#ifndef BK_UTIL_H
#define BK_UTIL_H

#include <stddef.h>
#include <stdint.h>

/* Allocation that aborts on failure. */
void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t sz);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);

/* Hex encoding. out_hex must hold 2*n+1 bytes (NUL terminated). */
void hex_encode(const uint8_t *in, size_t n, char *out_hex);
/* Decode 2*n hex chars into out (n bytes). Returns 0 on success, -1 on bad
   input. Tolerates an optional trailing newline. */
int hex_decode(const char *hex, uint8_t *out, size_t n);

/* Join base + "/" + name into a freshly malloc'd string. */
char *path_join(const char *base, const char *name);

/* Expand a leading "~/" using $HOME. Returns malloc'd string (may be a copy
   of the input if no expansion needed). */
char *path_expand(const char *p);

/* Recursively create a local directory path (like mkdir -p). 0 on success. */
int mkdir_p(const char *path, unsigned mode);

/* Read an entire local file into a malloc'd buffer. Returns 0 and sets
   buf and len on success; caller frees. */
int read_file(const char *path, uint8_t **buf, size_t *len);

/* A growable byte buffer. */
typedef struct {
    uint8_t *data;
    size_t   len;
    size_t   cap;
} Buf;

void buf_init(Buf *b);
void buf_reserve(Buf *b, size_t extra);
void buf_append(Buf *b, const void *p, size_t n);
void buf_consume_front(Buf *b, size_t n);   /* drop first n bytes, shift rest */
void buf_free(Buf *b);

#endif
