#ifndef BK_COMPRESS_H
#define BK_COMPRESS_H

#include <stdint.h>
#include <stddef.h>

#include "util.h"

#define BK_ZSTD_LEVEL 3

/* Compress src into *out (appended). The zstd frame records the content size
   so decompress needs no external length. */
void zstd_compress(const uint8_t *src, size_t n, int level, Buf *out);

/* Decompress a zstd frame into *out (appended). Returns 0 on success, -1 if
   the frame is corrupt or the content size is unknown. */
int zstd_decompress(const uint8_t *src, size_t n, Buf *out);

#endif
