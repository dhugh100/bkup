#ifndef BK_CHUNK_H
#define BK_CHUNK_H

#include <stddef.h>
#include <stdint.h>

/* Tunable chunker parameters. The shared gear[256] table is fixed forever, but
   the size thresholds and masks can vary per stream: file backups use the
   frozen file params (see chunk_cut); the catalog uses smaller params (it is
   regenerated each push, so its chunks are disposable and not frozen). */
typedef struct {
    size_t   min;        /* no boundary before this offset */
    size_t   normal;     /* mask transition point (target average) */
    size_t   max;        /* forced cut here */
    uint64_t mask_s;     /* strict mask used before `normal` */
    uint64_t mask_l;     /* loose mask used after `normal` */
} ChunkParams;

/* Content-defined chunking (FastCDC, normalized). Given a buffer, return the
   length of the first chunk: the boundary is chosen from content so that
   inserting/removing bytes only re-chunks locally, preserving dedup.

   buf/len is whatever is currently available. If len could grow (more file
   to read), only call once you have at least p->max buffered, or pass
   `final=1` to signal this is the tail of the stream so a short final chunk
   is allowed. The returned length is always <= len. */
size_t chunk_cut_ex(const uint8_t *buf, size_t len, int final,
                    const ChunkParams *p);

/* Backup-file chunker: the frozen file params. Boundaries here must never
   change -- dedup against every existing repo depends on them. */
size_t chunk_cut(const uint8_t *buf, size_t len, int final);

#endif
