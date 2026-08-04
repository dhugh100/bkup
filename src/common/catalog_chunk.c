#include <stdlib.h>
#include <string.h>

#include "catalog_chunk.h"
#include "types.h"
#include "util.h"

/* ~16 KiB average around SQLite's 4 KiB page size. See catalog_chunk.h. */
const ChunkParams CAT_PARAMS = {
    .min    = 8u * 1024u,
    .normal = 16u * 1024u,
    .max    = 64u * 1024u,
    .mask_s = ~0ULL << (64 - 16),   /* strict: ~1/65536 before the average */
    .mask_l = ~0ULL << (64 - 13),   /* loose:  ~1/8192 after, pulls toward 16 KiB */
};

#define CATMAN_HDR_LEN (4 + 4 + 8 + 8 + 4)   /* magic|ver|snap|dbsize|nchunks */

static void put_u32(Buf *b, uint32_t v)
{
    uint8_t t[4] = { v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, (v >> 24) & 0xff };
    buf_append(b, t, 4);
}

static void put_i64(Buf *b, int64_t v)
{
    uint64_t u = (uint64_t)v;
    uint8_t t[8];
    for (int i = 0; i < 8; i++) t[i] = (u >> (8 * i)) & 0xff;
    buf_append(b, t, 8);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int64_t get_i64(const uint8_t *p)
{
    uint64_t u = 0;
    for (int i = 0; i < 8; i++) u |= (uint64_t)p[i] << (8 * i);
    return (int64_t)u;
}

void catman_serialize(Buf *out, int64_t snap_id, int64_t db_size,
                      const uint8_t *hashes, uint32_t nchunks)
{
    buf_append(out, CATMAN_MAGIC, 4);
    put_u32(out, CATMAN_VERSION);
    put_i64(out, snap_id);
    put_i64(out, db_size);
    put_u32(out, nchunks);
    buf_append(out, hashes, (size_t)nchunks * BK_HASH_LEN);
}

int catman_parse(const uint8_t *data, size_t len, CatManifest *m)
{
    memset(m, 0, sizeof *m);
    if (len < CATMAN_HDR_LEN) return -1;
    if (memcmp(data, CATMAN_MAGIC, 4) != 0) return -1;
    if (get_u32(data + 4) != CATMAN_VERSION) return -1;

    m->snap_id = get_i64(data + 8);
    m->db_size = get_i64(data + 16);
    m->nchunks = get_u32(data + 24);

    size_t need = (size_t)CATMAN_HDR_LEN + (size_t)m->nchunks * BK_HASH_LEN;
    if (len < need) return -1;

    if (m->nchunks) {
        m->hashes = xmalloc((size_t)m->nchunks * BK_HASH_LEN);
        memcpy(m->hashes, data + CATMAN_HDR_LEN,
               (size_t)m->nchunks * BK_HASH_LEN);
    }
    return 0;
}

void catman_free(CatManifest *m)
{
    free(m->hashes);
    m->hashes = NULL;
}
