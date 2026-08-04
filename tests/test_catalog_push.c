/* Unit test for the catalog-push batching state machine (src/daemon/catalog_push.c).
   Uses the test clock seam for deterministic throttle behavior. */
#include <stdio.h>
#include <time.h>
#include "daemon/catalog_push.h"

static time_t fake;
static time_t fake_clock(void) { return fake; }

static int fails;
#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); fails++; } } while (0)

int main(void)
{
    catalog_push_set_clock(fake_clock);
    fake = 100000;                       /* mimic a real (large) epoch clock */
    unsigned long seq = 0;

    /* nothing dirty -> nothing owed */
    CHECK(catalog_take_pending("u", &seq) == 0);

    /* first dirty pushes immediately (last_push==0, so throttle is satisfied) */
    catalog_mark_dirty("u");
    CHECK(catalog_take_pending("u", &seq) == 1);
    CHECK(seq == 1);
    catalog_mark_pushed("u", 1);
    CHECK(catalog_take_pending("u", &seq) == 0);   /* clean now */

    /* throttle: a new change within the interval is held */
    catalog_mark_dirty("u");
    CHECK(catalog_take_pending("u", &seq) == 0);    /* diff 0 < interval */
    fake = 100000 + 299;
    CHECK(catalog_take_pending("u", &seq) == 0);    /* still inside interval */
    fake = 100000 + 300;
    CHECK(catalog_take_pending("u", &seq) == 1);    /* interval elapsed */
    CHECK(seq == 2);

    /* a spot backup that commits DURING the push must not be lost: it bumps
       dirty_seq past the captured seq, so the next interval re-pushes. */
    catalog_mark_dirty("u");                        /* dirty_seq -> 3 */
    catalog_mark_pushed("u", 2);                    /* record only up to 2 */
    fake = 100300 + 300;
    CHECK(catalog_take_pending("u", &seq) == 1);
    CHECK(seq == 3);                                /* captured the during-push change */
    catalog_mark_pushed("u", 3);
    CHECK(catalog_take_pending("u", &seq) == 0);

    /* a full backup/prune clears the debt via catalog_clear */
    catalog_mark_dirty("u");
    catalog_clear("u");
    CHECK(catalog_take_pending("u", &seq) == 0);

    /* per-user isolation: a different user has its own counters */
    fake += 100000;
    catalog_mark_dirty("v");
    CHECK(catalog_take_pending("v", &seq) == 1);
    CHECK(seq == 1);

    if (fails == 0) printf("test_catalog_push: OK\n");
    return fails ? 1 : 0;
}
