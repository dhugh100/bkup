#include <zstd.h>

#include "compress.h"
#include "log.h"

void zstd_compress(const uint8_t *src, size_t n, int level, Buf *out)
{
    size_t bound = ZSTD_compressBound(n);
    buf_reserve(out, bound);
    size_t w = ZSTD_compress(out->data + out->len, bound, src, n, level);
    if (ZSTD_isError(w))
        die("zstd compress failed: %s", ZSTD_getErrorName(w));
    out->len += w;
}

int zstd_decompress(const uint8_t *src, size_t n, Buf *out)
{
    unsigned long long sz = ZSTD_getFrameContentSize(src, n);
    if (sz == ZSTD_CONTENTSIZE_UNKNOWN || sz == ZSTD_CONTENTSIZE_ERROR)
        return -1;
    buf_reserve(out, (size_t)sz);
    size_t w = ZSTD_decompress(out->data + out->len, (size_t)sz, src, n);
    if (ZSTD_isError(w) || w != sz)
        return -1;
    out->len += w;
    return 0;
}
