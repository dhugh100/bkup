#include "chunk.h"
#include "types.h"

/* FastCDC with two-level normalization (NC=2 around a 1 MiB average).

   The gear table and the two masks are FIXED FOREVER: dedup across every
   backup and every machine depends on identical boundaries, so changing any
   constant here orphans all previously stored chunks. The gear table is
   generated deterministically from a fixed splitmix64 seed so we never carry
   256 magic literals, but the resulting values must never change. */

#define MIN_SZ     BK_CHUNK_MIN      /* 256 KiB: no cut before this */
#define NORMAL_SZ  BK_CHUNK_NORMAL   /* 1 MiB: mask transition point */
#define MAX_SZ     BK_CHUNK_MAX      /* 4 MiB: forced cut here */

/* Test the top N bits of the rolling fingerprint. fp = (fp<<1)+gear means the
   high bits depend on roughly the last 64 bytes -- that span is the rolling
   window. Probability of a boundary at any position is 1/2^N. Before the
   average we use a stricter (22-bit) mask to discourage early cuts; after it
   a looser (18-bit) mask to encourage cuts, pulling sizes toward the mean. */
#define MASK_S  (~0ULL << (64 - 22))
#define MASK_L  (~0ULL << (64 - 18))

static uint64_t gear[256];
static int gear_ready;

static uint64_t splitmix64(uint64_t *s)
{
    uint64_t z = (*s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static void gear_init(void)
{
    uint64_t s = 0x1234567890ABCDEFULL;   /* fixed seed, never change */
    for (int i = 0; i < 256; i++)
        gear[i] = splitmix64(&s);
    gear_ready = 1;
}

size_t chunk_cut_ex(const uint8_t *buf, size_t len, int final,
                    const ChunkParams *p)
{
    if (!gear_ready) gear_init();

    /* Not enough buffered to make a normal decision and more is coming:
       caller should buffer more. We still must return something <= len, so
       only happens when final or len already large. */
    if (len <= p->min)
        return len;                 /* short tail or tiny input */

    size_t n = len;
    if (n > p->max) n = p->max;

    size_t normal = p->normal;
    if (normal > n) normal = n;

    uint64_t fp = 0;
    size_t i = p->min;

    for (; i < normal; i++) {
        fp = (fp << 1) + gear[buf[i]];
        if ((fp & p->mask_s) == 0) return i + 1;
    }
    for (; i < n; i++) {
        fp = (fp << 1) + gear[buf[i]];
        if ((fp & p->mask_l) == 0) return i + 1;
    }
    /* Reached the scan limit. If that limit was p->max, cut there. Otherwise
       we ran out of buffered data: only emit the short chunk if this is the
       final tail, else tell the caller to buffer more by returning n==len
       (caller checks `final`). */
    (void)final;
    return n;
}

/* Frozen file params: identical boundaries to every prior release. */
static const ChunkParams FILE_PARAMS = {
    MIN_SZ, NORMAL_SZ, MAX_SZ, MASK_S, MASK_L,
};

size_t chunk_cut(const uint8_t *buf, size_t len, int final)
{
    return chunk_cut_ex(buf, len, final, &FILE_PARAMS);
}
