#ifndef BK_CATALOG_CHUNK_H
#define BK_CATALOG_CHUNK_H

#include <stdint.h>

#include "chunk.h"
#include "util.h"   /* Buf */

/* Small content-defined chunk params for the catalog DB. ~16 KiB average so
   that successive VACUUMed snapshots, which differ in only a few 4 KiB SQLite
   pages, dedup down to a handful of changed chunks. These are NOT frozen
   forever like the file chunker (chunk_cut): the catalog is regenerated on
   every push, so changing them merely orphans the current catalog chunks,
   which the next push re-uploads and the GC reclaims -- no data is at risk. */
extern const ChunkParams CAT_PARAMS;

/* Catalog manifest: the ordered list of chunk hashes that reconstruct one
   VACUUMed catalog DB, with a small header. On-disk layout (explicit
   little-endian packing, no struct padding):

     magic[4] "BCM1" | version u32 | snap_id i64 | db_size i64 | nchunks u32
     | hash[BK_HASH_LEN] * nchunks

   It is self-contained: fetch-catalog downloads and decrypts the manifest with
   only the repo config + passphrase, then pulls the chunks it names. */
#define CATMAN_MAGIC   "BCM1"
#define CATMAN_VERSION 1u

typedef struct {
    int64_t   snap_id;
    int64_t   db_size;     /* plaintext size of the reassembled DB, for sanity */
    uint32_t  nchunks;
    uint8_t  *hashes;      /* nchunks * BK_HASH_LEN, malloc'd; free via catman_free */
} CatManifest;

/* Append a serialized manifest to *out. `hashes` is nchunks contiguous
   BK_HASH_LEN-byte hashes in chunk order. */
void catman_serialize(Buf *out, int64_t snap_id, int64_t db_size,
                      const uint8_t *hashes, uint32_t nchunks);

/* Parse a serialized manifest. Returns 0 on success (and fills *m, owning
   m->hashes), -1 on a bad magic/version/truncation. */
int catman_parse(const uint8_t *data, size_t len, CatManifest *m);

void catman_free(CatManifest *m);

#endif
