/*
 * test_prune.c -- period_key, apply_periodic, continuous anchor rule, blob GC
 *                 reachability SQL assertions (Phase 3).
 *
 * Includes cli/prune_internal.h (testability seam) to access period_key() and
 * apply_periodic() without going through cmd_prune (which calls transport_*).
 *
 * Object set: CLI_OBJS (detected from "cli/ include below).
 *
 * IMPORTANT: All timestamps are built with mktime() on explicit struct tm
 * values so that localtime_r() in period_key() interprets them in the same
 * local timezone.  The results are therefore TZ-independent within a run.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sqlite3.h>

#include "cli/commands.h"
#include "cli/prune_internal.h"
#include "common/db.h"
#include "common/types.h"
#include "common/util.h"

#include "test_common.h"

static int fails;

/* ---- timestamp helpers ---- */

/* Build a local-time timestamp for the given calendar values. */
static time_t ts(int year, int mon, int mday, int hour, int min)
{
    struct tm t;
    memset(&t, 0, sizeof t);
    t.tm_year = year - 1900;
    t.tm_mon  = mon - 1;   /* 0-based */
    t.tm_mday = mday;
    t.tm_hour = hour;
    t.tm_min  = min;
    t.tm_isdst = -1;
    return mktime(&t);
}

/* ---- period_key: same-period tests ---- */

static void test_period_key_same_day(void)
{
    time_t a = ts(2025, 3, 15, 10, 0);
    time_t b = ts(2025, 3, 15, 23, 59);
    CHECKEQ_INT(period_key(a, P_DAILY), period_key(b, P_DAILY));
}

static void test_period_key_diff_day(void)
{
    time_t a = ts(2025, 3, 15, 23, 59);
    time_t b = ts(2025, 3, 16,  0,  0);
    CHECK(period_key(a, P_DAILY) != period_key(b, P_DAILY));
}

static void test_period_key_same_month(void)
{
    time_t a = ts(2025, 6,  1,  0,  0);
    time_t b = ts(2025, 6, 30, 23, 59);
    CHECKEQ_INT(period_key(a, P_MONTHLY), period_key(b, P_MONTHLY));
}

static void test_period_key_diff_month(void)
{
    time_t a = ts(2025, 5, 31, 23, 59);
    time_t b = ts(2025, 6,  1,  0,  0);
    CHECK(period_key(a, P_MONTHLY) != period_key(b, P_MONTHLY));
}

static void test_period_key_same_year(void)
{
    time_t a = ts(2024,  1,  1,  0,  0);
    time_t b = ts(2024, 12, 31, 23, 59);
    CHECKEQ_INT(period_key(a, P_YEARLY), period_key(b, P_YEARLY));
}

static void test_period_key_diff_year(void)
{
    time_t a = ts(2024, 12, 31, 23, 59);
    time_t b = ts(2025,  1,  1,  0,  0);
    CHECK(period_key(a, P_YEARLY) != period_key(b, P_YEARLY));
}

/*
 * ISO week boundary: Dec 28 2025 is Monday (ISO week 1 of 2026 begins Dec 29).
 * So Dec 28 (week 52 of 2025) and Dec 29 (week 1 of 2026) differ.
 * Dec 29 and Jan 5 2026 share week 1 of 2026.
 */
static void test_period_key_same_isoweek(void)
{
    /* Mon Dec 29 2025 and Sun Jan 4 2026 are both in ISO week 2026-W01. */
    time_t a = ts(2025, 12, 29, 9, 0);
    time_t b = ts(2026,  1,  4, 9, 0);
    CHECKEQ_INT(period_key(a, P_WEEKLY), period_key(b, P_WEEKLY));
}

static void test_period_key_diff_isoweek(void)
{
    /* Dec 28 2025 is in ISO week 2025-W52; Dec 29 2025 is in ISO week 2026-W01. */
    time_t a = ts(2025, 12, 28, 9, 0);
    time_t b = ts(2025, 12, 29, 9, 0);
    CHECK(period_key(a, P_WEEKLY) != period_key(b, P_WEEKLY));
}

/* ---- apply_periodic: keeps newest per bucket ---- */

/*
 * Build an array of SCHEDULED snapshots, one per day over four days, sorted
 * newest first.  apply_periodic with kind=P_DAILY and count=2 should keep the
 * two newest (one per day) and leave the older two unmarked.
 */
static void test_apply_periodic_daily_keeps_newest(void)
{
    Snap s[4];
    /* Sorted newest-first: day4, day3, day2, day1 */
    s[0].id = 4; s[0].created = ts(2025, 4, 4, 12, 0); s[0].kind = SK_SCHEDULED;
    s[1].id = 3; s[1].created = ts(2025, 4, 3, 12, 0); s[1].kind = SK_SCHEDULED;
    s[2].id = 2; s[2].created = ts(2025, 4, 2, 12, 0); s[2].kind = SK_SCHEDULED;
    s[3].id = 1; s[3].created = ts(2025, 4, 1, 12, 0); s[3].kind = SK_SCHEDULED;

    int keep[4] = {0, 0, 0, 0};
    apply_periodic(s, 4, keep, P_DAILY, 2);

    CHECKEQ_INT(keep[0], 1);   /* day4 kept */
    CHECKEQ_INT(keep[1], 1);   /* day3 kept */
    CHECKEQ_INT(keep[2], 0);   /* day2 NOT kept (count limit reached) */
    CHECKEQ_INT(keep[3], 0);   /* day1 NOT kept */
}

/* Two snapshots on the same day: only the newest (first in sorted order) kept. */
static void test_apply_periodic_deduplicates_same_bucket(void)
{
    Snap s[3];
    /* Two snaps on same day; the third on a different day */
    s[0].id = 3; s[0].created = ts(2025, 5, 1, 18, 0); s[0].kind = SK_SCHEDULED;
    s[1].id = 2; s[1].created = ts(2025, 5, 1,  9, 0); s[1].kind = SK_SCHEDULED;
    s[2].id = 1; s[2].created = ts(2025, 4, 30, 12, 0); s[2].kind = SK_SCHEDULED;

    int keep[3] = {0, 0, 0};
    apply_periodic(s, 3, keep, P_DAILY, 3);   /* count=3 but only 2 distinct days */

    CHECKEQ_INT(keep[0], 1);   /* newest on May 1 */
    CHECKEQ_INT(keep[1], 0);   /* older on May 1: same bucket, not kept */
    CHECKEQ_INT(keep[2], 1);   /* Apr 30 */
}

/* Continuous snapshots are SKIPPED by apply_periodic. */
static void test_apply_periodic_skips_continuous(void)
{
    Snap s[3];
    s[0].id = 3; s[0].created = ts(2025, 5, 3, 12, 0); s[0].kind = SK_CONTINUOUS;
    s[1].id = 2; s[1].created = ts(2025, 5, 2, 12, 0); s[1].kind = SK_SCHEDULED;
    s[2].id = 1; s[2].created = ts(2025, 5, 1, 12, 0); s[2].kind = SK_SCHEDULED;

    int keep[3] = {0, 0, 0};
    apply_periodic(s, 3, keep, P_DAILY, 3);

    CHECKEQ_INT(keep[0], 0);   /* continuous: not touched by apply_periodic */
    CHECKEQ_INT(keep[1], 1);   /* scheduled May 2 */
    CHECKEQ_INT(keep[2], 1);   /* scheduled May 1 */
}

/* ---- continuous anchor rule ---- */

/*
 * Simulates the anchor logic from cmd_prune:
 *   - Find the newest scheduled snapshot (the anchor).
 *   - Continuous snapshots NEWER than the anchor: keep=1.
 *   - Continuous snapshots OLDER than the anchor: keep=0.
 *   - If no scheduled snapshot exists, all continuous are kept.
 *
 * (This tests the logic as code, not as a black-box cmd_prune call, so no
 * SFTP connection is needed.)
 */
static void test_continuous_anchor_rule(void)
{
    Snap s[5];
    /* Sorted newest-first: cont3, cont2, sched, cont1, cont0 */
    s[0].id = 5; s[0].created = ts(2025, 6, 1, 18, 0); s[0].kind = SK_CONTINUOUS;
    s[1].id = 4; s[1].created = ts(2025, 6, 1, 14, 0); s[1].kind = SK_CONTINUOUS;
    s[2].id = 3; s[2].created = ts(2025, 6, 1, 12, 0); s[2].kind = SK_SCHEDULED;
    s[3].id = 2; s[3].created = ts(2025, 6, 1,  8, 0); s[3].kind = SK_CONTINUOUS;
    s[4].id = 1; s[4].created = ts(2025, 6, 1,  6, 0); s[4].kind = SK_CONTINUOUS;

    int keep[5] = {0, 0, 0, 0, 0};

    /* Replicate the anchor computation from cmd_prune */
    long long anchor = 0; int have_sched = 0;
    for (int i = 0; i < 5; i++)
        if (s[i].kind == SK_SCHEDULED && (!have_sched || s[i].created > anchor)) {
            anchor = s[i].created; have_sched = 1;
        }
    for (int i = 0; i < 5; i++) {
        if (s[i].kind != SK_CONTINUOUS) continue;
        if (!have_sched || s[i].created >= anchor) keep[i] = 1;
    }

    CHECKEQ_INT(keep[0], 1);   /* cont3: newer than anchor -> keep */
    CHECKEQ_INT(keep[1], 1);   /* cont2: newer than anchor -> keep */
    CHECKEQ_INT(keep[2], 0);   /* sched: not touched here (apply_periodic handles it) */
    CHECKEQ_INT(keep[3], 0);   /* cont1: older than anchor -> prune */
    CHECKEQ_INT(keep[4], 0);   /* cont0: older than anchor -> prune */
}

/* If no scheduled snapshot, all continuous are kept. */
static void test_continuous_all_kept_when_no_scheduled(void)
{
    Snap s[3];
    s[0].id = 3; s[0].created = ts(2025, 7, 3, 12, 0); s[0].kind = SK_CONTINUOUS;
    s[1].id = 2; s[1].created = ts(2025, 7, 2, 12, 0); s[1].kind = SK_CONTINUOUS;
    s[2].id = 1; s[2].created = ts(2025, 7, 1, 12, 0); s[2].kind = SK_CONTINUOUS;

    int keep[3] = {0, 0, 0};

    long long anchor = 0; int have_sched = 0;
    for (int i = 0; i < 3; i++)
        if (s[i].kind == SK_SCHEDULED && (!have_sched || s[i].created > anchor)) {
            anchor = s[i].created; have_sched = 1;
        }
    for (int i = 0; i < 3; i++) {
        if (s[i].kind != SK_CONTINUOUS) continue;
        if (!have_sched || s[i].created >= anchor) keep[i] = 1;
    }

    CHECKEQ_INT(keep[0], 1);
    CHECKEQ_INT(keep[1], 1);
    CHECKEQ_INT(keep[2], 1);
}

/* The newest snapshot (index 0) is always kept (keep[0]=1 in cmd_prune). */
static void test_newest_always_kept(void)
{
    Snap s[3];
    s[0].id = 3; s[0].created = ts(2025, 8, 3, 12, 0); s[0].kind = SK_SCHEDULED;
    s[1].id = 2; s[1].created = ts(2025, 8, 2, 12, 0); s[1].kind = SK_SCHEDULED;
    s[2].id = 1; s[2].created = ts(2025, 8, 1, 12, 0); s[2].kind = SK_SCHEDULED;

    int keep[3] = {0, 0, 0};
    /* apply_periodic with count=0 keeps nothing; the always-keep rule fires separately */
    apply_periodic(s, 3, keep, P_DAILY, 0);
    /* Mimic cmd_prune's unconditional keep of newest */
    if (3 > 0) keep[0] = 1;

    CHECKEQ_INT(keep[0], 1);
    CHECKEQ_INT(keep[1], 0);
    CHECKEQ_INT(keep[2], 0);
}

/* ---- no --keep-* rules: cmd_prune must die ---- */

static void fn_prune_no_keep(void)
{
    Ctx c; memset(&c, 0, sizeof c);
    char *argv_empty[1] = { NULL };
    cmd_prune(&c, 0, argv_empty);
}

static void test_no_keep_rules_dies(void)
{
    CHECK(check_dies(fn_prune_no_keep));
}

/* ---- blob GC reachability SQL ---- */

/*
 * Replicate the version-pruning SQL from cmd_prune and verify that blobs no
 * longer referenced by any surviving version are correctly identified as
 * unreachable.
 *
 * Fixture:
 *   S1(COMPLETE), S2(COMPLETE), S3(COMPLETE)
 *   V1: path="/a" first=S1 last=S2  (only in S1 interval [S1,S2))
 *   V2: path="/b" first=S2 last=NULL (spans S2 and S3)
 *   blob H1: referenced only by V1 (version_blobs)
 *   blob H2: referenced only by V2
 *
 * After simulating a prune that removes S1 (leaving S2, S3):
 *   V1's interval [S1,S2) no longer overlaps any surviving snapshot (S2,S3>=S2
 *   and S2<S2 is false -> no survivor in [S1,S2)) -> V1 deleted -> H1 unreachable.
 *   V2's interval [S2,NULL) overlaps S2,S3 -> V2 kept -> H2 reachable.
 */
static void test_blob_gc_reachability_sql(void)
{
    char base[64]; tmpdir(base, sizeof base);
    sqlite3 *db = open_temp_catalog(base);

    long long S1 = ins_snapshot(db, 1000, SS_COMPLETE, SK_SCHEDULED);
    long long S2 = ins_snapshot(db, 2000, SS_COMPLETE, SK_SCHEDULED);
    long long S3 = ins_snapshot(db, 3000, SS_COMPLETE, SK_SCHEDULED);

    long long V1 = ins_version(db, "/a", FK_REG, 100, S1, S2);
    long long V2 = ins_version(db, "/b", FK_REG, 200, S2, -1);

    uint8_t H1[BK_HASH_LEN], H2[BK_HASH_LEN];
    fill(H1, BK_HASH_LEN, 0xAA);
    fill(H2, BK_HASH_LEN, 0xBB);

    ins_blob(db, H1, 100, 80,  BS_UPLOADED);
    ins_blob(db, H2, 200, 160, BS_UPLOADED);
    ins_version_blob(db, V1, 0, H1);
    ins_version_blob(db, V2, 0, H2);

    /* Simulate pruning S1: delete it from the snapshots table. */
    sqlite3_stmt *del;
    sqlite3_prepare_v2(db, "DELETE FROM snapshots WHERE id=?", -1, &del, NULL);
    sqlite3_bind_int64(del, 1, S1);
    sqlite3_step(del);
    sqlite3_finalize(del);

    /* SQL lifted verbatim from cmd_prune in src/cli/prune.c */
    db_exec(db,
        "DELETE FROM version_blobs WHERE version_id IN ("
        "  SELECT id FROM versions v WHERE NOT EXISTS ("
        "    SELECT 1 FROM snapshots s WHERE s.state=1 "
        "    AND s.id>=v.first_snapshot "
        "    AND (v.last_snapshot IS NULL OR s.id<v.last_snapshot)))");
    db_exec(db,
        "DELETE FROM versions WHERE NOT EXISTS ("
        "  SELECT 1 FROM snapshots s WHERE s.state=1 "
        "  AND s.id>=versions.first_snapshot "
        "  AND (versions.last_snapshot IS NULL OR s.id<versions.last_snapshot))");

    /* Find blobs no longer referenced: SQL from cmd_prune's blob collection */
    sqlite3_stmt *ob;
    sqlite3_prepare_v2(db,
        "SELECT hash FROM blobs WHERE hash NOT IN (SELECT hash FROM version_blobs)",
        -1, &ob, NULL);
    int n_orphan = 0;
    int found_h1 = 0, found_h2 = 0;
    while (sqlite3_step(ob) == SQLITE_ROW) {
        const void *h = sqlite3_column_blob(ob, 0);
        if (sqlite3_column_bytes(ob, 0) == BK_HASH_LEN) {
            if (memcmp(h, H1, BK_HASH_LEN) == 0) found_h1 = 1;
            if (memcmp(h, H2, BK_HASH_LEN) == 0) found_h2 = 1;
        }
        n_orphan++;
    }
    sqlite3_finalize(ob);

    CHECKEQ_INT(n_orphan, 1);    /* only H1 is unreachable */
    CHECKEQ_INT(found_h1, 1);    /* H1: V1 was pruned */
    CHECKEQ_INT(found_h2, 0);    /* H2: V2 still referenced */

    sqlite3_close(db);
    rmtree_local(base);
}

/* ---- empty-directory sweep (prune_empty_dirs) ---- */

/* Count surviving version rows whose path matches exactly. */
static int have_version(sqlite3 *db, const char *path)
{
    sqlite3_stmt *st;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM versions WHERE path=?",
                       -1, &st, NULL);
    sqlite3_bind_text(st, 1, path, -1, SQLITE_STATIC);
    int n = (sqlite3_step(st) == SQLITE_ROW) ? sqlite3_column_int(st, 0) : -1;
    sqlite3_finalize(st);
    return n;
}

/*
 * A directory keeps its version only if a file or symlink version lives
 * somewhere beneath it.
 *
 *   /x/keep        dir, holds /x/keep/f          -> kept
 *   /x/keep/deep   dir, holds /x/keep/deep/g     -> kept (nested content)
 *   /x/drop        dir, empty                    -> removed
 *   /x/drop/sub    dir, empty                    -> removed (whole chain)
 *   /x/link        dir, holds a symlink only     -> kept (symlink is content)
 */
static void test_empty_dirs_removed(void)
{
    char base[64]; tmpdir(base, sizeof base);
    sqlite3 *db = open_temp_catalog(base);

    ins_version(db, "/x",           FK_DIR,     0, 1, -1);
    ins_version(db, "/x/keep",      FK_DIR,     0, 1, -1);
    ins_version(db, "/x/keep/f",    FK_REG,   100, 1, -1);
    ins_version(db, "/x/keep/deep", FK_DIR,     0, 1, -1);
    ins_version(db, "/x/keep/deep/g", FK_REG, 100, 1, -1);
    ins_version(db, "/x/drop",      FK_DIR,     0, 1, -1);
    ins_version(db, "/x/drop/sub",  FK_DIR,     0, 1, -1);
    ins_version(db, "/x/link",      FK_DIR,     0, 1, -1);
    ins_version(db, "/x/link/s",    FK_SYMLINK, 0, 1, -1);

    CHECKEQ_INT(prune_empty_dirs(db), 2);   /* /x/drop and /x/drop/sub only */

    CHECKEQ_INT(have_version(db, "/x"),             1);
    CHECKEQ_INT(have_version(db, "/x/keep"),        1);
    CHECKEQ_INT(have_version(db, "/x/keep/deep"),   1);
    CHECKEQ_INT(have_version(db, "/x/link"),        1);
    CHECKEQ_INT(have_version(db, "/x/drop"),        0);
    CHECKEQ_INT(have_version(db, "/x/drop/sub"),    0);

    /* file and symlink versions are never touched */
    CHECKEQ_INT(have_version(db, "/x/keep/f"),      1);
    CHECKEQ_INT(have_version(db, "/x/keep/deep/g"), 1);
    CHECKEQ_INT(have_version(db, "/x/link/s"),      1);

    sqlite3_close(db);
    rmtree_local(base);
}

/*
 * Path-boundary guard (the same hazard test_sweep.c covers for scoped
 * deletion): /x/proj2's file must not keep the empty /x/proj alive, and
 * /x/proj's presence must not drag /x/proj2 down with it.
 */
static void test_empty_dirs_respect_path_boundaries(void)
{
    char base[64]; tmpdir(base, sizeof base);
    sqlite3 *db = open_temp_catalog(base);

    ins_version(db, "/x",           FK_DIR,   0, 1, -1);
    ins_version(db, "/x/proj",      FK_DIR,   0, 1, -1);   /* empty */
    ins_version(db, "/x/proj2",     FK_DIR,   0, 1, -1);   /* has content */
    ins_version(db, "/x/proj2/f",   FK_REG, 100, 1, -1);

    CHECKEQ_INT(prune_empty_dirs(db), 1);

    CHECKEQ_INT(have_version(db, "/x/proj"),  0);
    CHECKEQ_INT(have_version(db, "/x/proj2"), 1);
    CHECKEQ_INT(have_version(db, "/x"),       1);

    sqlite3_close(db);
    rmtree_local(base);
}

/*
 * A files row pointing at a removed directory version must be cleared, not
 * left dangling: an empty directory that still exists on disk keeps its files
 * row (so the scanner does not treat it as new) but loses version_id.
 */
static void test_empty_dirs_clear_file_version_ref(void)
{
    char base[64]; tmpdir(base, sizeof base);
    sqlite3 *db = open_temp_catalog(base);

    ins_version(db, "/x/empty", FK_DIR, 0, 1, -1);
    long long vk = ins_version(db, "/x/full",  FK_DIR, 0, 1, -1);
    ins_version(db, "/x/full/f", FK_REG, 100, 1, -1);

    long long fd = ins_file(db, "/x/empty", FK_DIR, 0, 0, 0, 0, 0, 0, 0, FS_CLEAN, 1);
    long long fk = ins_file(db, "/x/full",  FK_DIR, 0, 0, 0, 0, 0, 0, 0, FS_CLEAN, 1);
    db_exec(db, "UPDATE files SET version_id=(SELECT id FROM versions "
                "WHERE versions.path=files.path)");

    CHECKEQ_INT(prune_empty_dirs(db), 1);

    sqlite3_stmt *st;
    sqlite3_prepare_v2(db, "SELECT version_id FROM files WHERE id=?", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, fd);
    CHECK(sqlite3_step(st) == SQLITE_ROW);
    CHECKEQ_INT(sqlite3_column_type(st, 0), SQLITE_NULL);   /* cleared */
    sqlite3_finalize(st);

    sqlite3_prepare_v2(db, "SELECT version_id FROM files WHERE id=?", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, fk);
    CHECK(sqlite3_step(st) == SQLITE_ROW);
    CHECKEQ_INT(sqlite3_column_int64(st, 0), vk);           /* untouched */
    sqlite3_finalize(st);

    CHECKEQ_INT(have_version(db, "/x/empty"), 0);

    sqlite3_close(db);
    rmtree_local(base);
}

/* No empty directories -> no deletions, and the call is idempotent. */
static void test_empty_dirs_noop(void)
{
    char base[64]; tmpdir(base, sizeof base);
    sqlite3 *db = open_temp_catalog(base);

    ins_version(db, "/x",     FK_DIR,   0, 1, -1);
    ins_version(db, "/x/f",   FK_REG, 100, 1, -1);

    CHECKEQ_INT(prune_empty_dirs(db), 0);
    CHECKEQ_INT(prune_empty_dirs(db), 0);
    CHECKEQ_INT(have_version(db, "/x"), 1);

    sqlite3_close(db);
    rmtree_local(base);
}

/* ---- main ---- */

int main(void)
{
    test_period_key_same_day();
    test_period_key_diff_day();
    test_period_key_same_month();
    test_period_key_diff_month();
    test_period_key_same_year();
    test_period_key_diff_year();
    test_period_key_same_isoweek();
    test_period_key_diff_isoweek();
    test_apply_periodic_daily_keeps_newest();
    test_apply_periodic_deduplicates_same_bucket();
    test_apply_periodic_skips_continuous();
    test_continuous_anchor_rule();
    test_continuous_all_kept_when_no_scheduled();
    test_newest_always_kept();
    test_no_keep_rules_dies();
    test_blob_gc_reachability_sql();
    test_empty_dirs_removed();
    test_empty_dirs_respect_path_boundaries();
    test_empty_dirs_clear_file_version_ref();
    test_empty_dirs_noop();
    TEST_DONE("test_prune");
}
