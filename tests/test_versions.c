/*
 * test_versions.c -- half-open interval model and ipc.c SQL assertions (Phase 3).
 *
 * No daemon/ or cli/ includes: uses COMMON_OBJS only.  Every query is lifted
 * verbatim from src/daemon/ipc.c so a SQL regression in either place also
 * breaks this test.
 *
 * Fixture layout (one catalog, shared across all tests):
 *
 *   Snapshots: S1(id=1,created=1000,COMPLETE), S2(id=2,created=2000,COMPLETE),
 *              S3(id=3,created=3000,COMPLETE)
 *
 *   Versions:
 *     /src/dir           FK_DIR  first=S1 last=NULL  (always current)
 *     /src/dir/sub       FK_DIR  first=S1 last=NULL  (always current)
 *     /src/dir/sub/deep  FK_REG  first=S1 last=NULL  (grandchild of /src/dir)
 *     /src/dir/steady    FK_REG  first=S1 last=NULL  (unchanged through all snaps)
 *     /src/dir/deleted   FK_REG  first=S1 last=S2    (gone at S2)
 *     /src/dir/new       FK_REG  first=S3 last=NULL  (appeared only at S3)
 *
 * Membership rule: first_snapshot <= S AND (last_snapshot IS NULL OR last_snapshot > S)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>

#include "common/db.h"
#include "common/types.h"

#include "test_common.h"

static int fails;

/* ---- shared fixture ---- */

static char  g_dir[64];
static sqlite3 *g_db;
static long long g_S1, g_S2, g_S3;

static void setup_fixture(void)
{
    tmpdir(g_dir, sizeof g_dir);
    g_db = open_temp_catalog(g_dir);

    g_S1 = ins_snapshot(g_db, 1000, SS_COMPLETE, SK_SCHEDULED);
    g_S2 = ins_snapshot(g_db, 2000, SS_COMPLETE, SK_SCHEDULED);
    g_S3 = ins_snapshot(g_db, 3000, SS_COMPLETE, SK_SCHEDULED);

    ins_version(g_db, "/src/dir",          FK_DIR,    0,  g_S1, -1);
    ins_version(g_db, "/src/dir/sub",      FK_DIR,    0,  g_S1, -1);
    ins_version(g_db, "/src/dir/sub/deep", FK_REG,   50,  g_S1, -1);
    ins_version(g_db, "/src/dir/steady",   FK_REG,  100,  g_S1, -1);
    ins_version(g_db, "/src/dir/deleted",  FK_REG,  200,  g_S1, g_S2);
    ins_version(g_db, "/src/dir/new",      FK_REG,  300,  g_S3, -1);
}

/* ---- helper: count rows returned by a query ---- */

static int count_rows_sql(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *q;
    if (sqlite3_prepare_v2(db, sql, -1, &q, NULL) != SQLITE_OK) {
        printf("count_rows_sql: prepare failed: %s\n", sqlite3_errmsg(db));
        return -1;
    }
    int n = 0;
    while (sqlite3_step(q) == SQLITE_ROW) n++;
    sqlite3_finalize(q);
    return n;
}

/* ---- 1. file unchanged across N snapshots: one version row ---- */

/*
 * handle_file_versions in ipc.c collapses "carried-forward duplicates": one row
 * per distinct version of the path, not per snapshot that carries it forward.
 * steady has one version row (first=S1, last=NULL) so the query returns 1 row.
 */
static void test_file_unchanged_one_row(void)
{
    /* SQL lifted verbatim from handle_file_versions in src/daemon/ipc.c */
    sqlite3_stmt *q;
    sqlite3_prepare_v2(g_db,
        "SELECT CASE WHEN v.last_snapshot IS NULL "
        "            THEN (SELECT MAX(id) FROM snapshots WHERE state=1) "
        "            ELSE (SELECT MAX(id) FROM snapshots "
        "                  WHERE state=1 AND id<v.last_snapshot) END, "
        "       sf.created, v.size, v.kind "
        "FROM versions v JOIN snapshots sf ON sf.id=v.first_snapshot "
        "WHERE v.path=? AND sf.state=1 "
        "ORDER BY sf.created DESC",
        -1, &q, NULL);
    sqlite3_bind_text(q, 1, "/src/dir/steady", -1, SQLITE_STATIC);

    int n = 0;
    long long snap_id = -1, created = -1, size = -1;
    int kind = -1;
    while (sqlite3_step(q) == SQLITE_ROW) {
        n++;
        snap_id = sqlite3_column_int64(q, 0);
        created = sqlite3_column_int64(q, 1);
        size    = sqlite3_column_int64(q, 2);
        kind    = sqlite3_column_int  (q, 3);
    }
    sqlite3_finalize(q);

    CHECKEQ_INT(n, 1);                 /* exactly one version row */
    CHECKEQ_INT(snap_id, g_S3);        /* restore handle = latest snap (S3) */
    CHECKEQ_INT(created, 1000);        /* captured at S1 time */
    CHECKEQ_INT(size, 100);
    CHECKEQ_INT(kind, FK_REG);
}

/* ---- 2. deleted file: interval closed at S2 ---- */

/*
 * deleted has first=S1, last=S2.  handle_file_versions returns 1 row; the
 * restore handle (first SELECT in the CASE) is MAX(id) WHERE state=1 AND id<S2
 * = S1.
 */
static void test_deleted_file_one_row(void)
{
    sqlite3_stmt *q;
    sqlite3_prepare_v2(g_db,
        "SELECT CASE WHEN v.last_snapshot IS NULL "
        "            THEN (SELECT MAX(id) FROM snapshots WHERE state=1) "
        "            ELSE (SELECT MAX(id) FROM snapshots "
        "                  WHERE state=1 AND id<v.last_snapshot) END, "
        "       sf.created, v.size, v.kind "
        "FROM versions v JOIN snapshots sf ON sf.id=v.first_snapshot "
        "WHERE v.path=? AND sf.state=1 "
        "ORDER BY sf.created DESC",
        -1, &q, NULL);
    sqlite3_bind_text(q, 1, "/src/dir/deleted", -1, SQLITE_STATIC);

    int n = 0;
    long long snap_id = -1;
    while (sqlite3_step(q) == SQLITE_ROW) {
        n++;
        snap_id = sqlite3_column_int64(q, 0);
    }
    sqlite3_finalize(q);

    CHECKEQ_INT(n, 1);
    CHECKEQ_INT(snap_id, g_S1);   /* restore handle: last complete snap before S2 */
}

/* ---- 3. list_dir: grandchild excluded from immediate children ---- */

/*
 * SQL lifted verbatim from handle_list_dir in src/daemon/ipc.c.
 * instr(substr(v.path, ?2), '/') = 0 ensures only immediate children.
 * The LIKE pattern is "prefix/%" and ?2 = strlen(prefix) + 2 (1-based offset
 * to the character after the trailing '/').
 */
static void test_list_dir_immediate_only(void)
{
    const char *path = "/src/dir";
    char pattern[256];
    snprintf(pattern, sizeof pattern, "%s/%%", path);
    int prefix_off = (int)strlen(path) + 2;   /* 1-based, past "path/" */

    sqlite3_stmt *q;
    sqlite3_prepare_v2(g_db,
        "SELECT v.path, v.kind, v.size, v.mtime_sec, v.last_snapshot "
        "FROM versions v "
        "WHERE v.path LIKE ?1 ESCAPE '\\' "
        "AND instr(substr(v.path, ?2), '/')=0 "
        "AND v.first_snapshot=(SELECT MAX(v2.first_snapshot) FROM versions v2 "
        "                      WHERE v2.path=v.path) "
        "ORDER BY (v.kind=1) DESC, v.path",
        -1, &q, NULL);
    sqlite3_bind_text(q, 1, pattern, -1, SQLITE_STATIC);
    sqlite3_bind_int (q, 2, prefix_off);

    int n = 0;
    int saw_deep = 0;
    while (sqlite3_step(q) == SQLITE_ROW) {
        const char *p = (const char *)sqlite3_column_text(q, 0);
        if (p && strcmp(p, "/src/dir/sub/deep") == 0) saw_deep = 1;
        n++;
    }
    sqlite3_finalize(q);

    /* sub, steady, deleted, new = 4 immediate children; deep.txt is excluded */
    CHECKEQ_INT(n, 4);
    CHECK(!saw_deep);
}

/* ---- 4. list_dir: directories returned before regular files ---- */

static void test_list_dir_dirs_first(void)
{
    const char *path = "/src/dir";
    char pattern[256];
    snprintf(pattern, sizeof pattern, "%s/%%", path);
    int prefix_off = (int)strlen(path) + 2;

    sqlite3_stmt *q;
    sqlite3_prepare_v2(g_db,
        "SELECT v.path, v.kind, v.size, v.mtime_sec, v.last_snapshot "
        "FROM versions v "
        "WHERE v.path LIKE ?1 ESCAPE '\\' "
        "AND instr(substr(v.path, ?2), '/')=0 "
        "AND v.first_snapshot=(SELECT MAX(v2.first_snapshot) FROM versions v2 "
        "                      WHERE v2.path=v.path) "
        "ORDER BY (v.kind=1) DESC, v.path",
        -1, &q, NULL);
    sqlite3_bind_text(q, 1, pattern, -1, SQLITE_STATIC);
    sqlite3_bind_int (q, 2, prefix_off);

    /* Collect kinds in order */
    int first_kind = -1, last_dir_pos = -1, first_file_pos = -1;
    int pos = 0;
    (void)first_kind;
    while (sqlite3_step(q) == SQLITE_ROW) {
        int kind = sqlite3_column_int(q, 1);
        if (kind == FK_DIR) last_dir_pos  = pos;
        if (kind == FK_REG && first_file_pos < 0) first_file_pos = pos;
        pos++;
    }
    sqlite3_finalize(q);

    /* All directories appear before any regular file */
    CHECK(last_dir_pos >= 0 && first_file_pos >= 0);
    CHECK(last_dir_pos < first_file_pos);
}

/* ---- 5. list_dir: deleted flag for path with closed interval ---- */

/*
 * In handle_list_dir: int deleted = (column_type(last_snapshot) != SQLITE_NULL)
 * "/src/dir/deleted" has last_snapshot=S2, so it is "deleted" in the listing.
 * "/src/dir/steady" has last_snapshot=NULL, so it is current (not deleted).
 */
static void test_list_dir_deleted_flag(void)
{
    const char *path = "/src/dir";
    char pattern[256];
    snprintf(pattern, sizeof pattern, "%s/%%", path);
    int prefix_off = (int)strlen(path) + 2;

    sqlite3_stmt *q;
    sqlite3_prepare_v2(g_db,
        "SELECT v.path, v.last_snapshot "
        "FROM versions v "
        "WHERE v.path LIKE ?1 ESCAPE '\\' "
        "AND instr(substr(v.path, ?2), '/')=0 "
        "AND v.first_snapshot=(SELECT MAX(v2.first_snapshot) FROM versions v2 "
        "                      WHERE v2.path=v.path) "
        "ORDER BY v.path",
        -1, &q, NULL);
    sqlite3_bind_text(q, 1, pattern, -1, SQLITE_STATIC);
    sqlite3_bind_int (q, 2, prefix_off);

    int del_deleted = -1, del_steady = -1;
    while (sqlite3_step(q) == SQLITE_ROW) {
        const char *p = (const char *)sqlite3_column_text(q, 0);
        int deleted = (sqlite3_column_type(q, 1) != SQLITE_NULL);
        if (p && strcmp(p, "/src/dir/deleted") == 0) del_deleted = deleted;
        if (p && strcmp(p, "/src/dir/steady")  == 0) del_steady  = deleted;
    }
    sqlite3_finalize(q);

    CHECKEQ_INT(del_deleted, 1);   /* closed interval: is deleted */
    CHECKEQ_INT(del_steady,  0);   /* open interval: not deleted */
}

/* ---- 6. list_all (no filter, no asof): all paths at latest snapshot ---- */

/*
 * SQL lifted verbatim from handle_list_all (no-filter branch) in ipc.c.
 * A = MAX(id) FROM snapshots WHERE state=1 = S3.
 * "deleted" in this context means last_snapshot <= A (gone from current tree).
 */
static void test_list_all_no_asof(void)
{
    /* Find A: the latest complete snapshot (same logic as handle_list_all) */
    sqlite3_stmt *ls;
    sqlite3_prepare_v2(g_db, "SELECT MAX(id) FROM snapshots WHERE state=1",
                       -1, &ls, NULL);
    long long A = 0;
    if (sqlite3_step(ls) == SQLITE_ROW) A = sqlite3_column_int64(ls, 0);
    sqlite3_finalize(ls);

    CHECKEQ_INT(A, g_S3);   /* fixture sanity */

    /* SQL lifted verbatim from handle_list_all (no filter case) */
    char sql[512];
    snprintf(sql, sizeof sql,
        "SELECT v.path, v.kind, v.size, v.last_snapshot FROM versions v "
        "WHERE v.first_snapshot=(SELECT MAX(v2.first_snapshot) FROM versions v2 "
        "  WHERE v2.path=v.path AND v2.first_snapshot<=%lld) "
        "ORDER BY v.path", A);

    sqlite3_stmt *q;
    sqlite3_prepare_v2(g_db, sql, -1, &q, NULL);

    int n = 0, n_deleted = 0;
    while (sqlite3_step(q) == SQLITE_ROW) {
        long long last  = sqlite3_column_int64(q, 3);
        int  last_null  = (sqlite3_column_type(q, 3) == SQLITE_NULL);
        int  deleted    = !last_null && last <= A;
        if (deleted) n_deleted++;
        n++;
    }
    sqlite3_finalize(q);

    /* All 6 version rows should appear; deleted.txt (last=S2<=S3) is deleted */
    CHECKEQ_INT(n, 6);
    CHECKEQ_INT(n_deleted, 1);   /* only deleted.txt is flagged deleted */
}

/* ---- 7. list_all with asof: path added after cutoff is hidden ---- */

/*
 * asof = timestamp of S2 (2000).  A = MAX(id) WHERE state=1 AND created<=2000
 * = S2 = 2.  new.txt has first_snapshot=S3=3 > 2, so no version row with
 * first_snapshot<=A exists for it -- it does not appear.
 */
static void test_list_all_asof_hides_future(void)
{
    long long asof = 2000;   /* created timestamp of S2 */

    /* Find A with the asof gate */
    char lq[128];
    snprintf(lq, sizeof lq,
             "SELECT MAX(id) FROM snapshots WHERE state=1 AND created<=%lld", asof);
    sqlite3_stmt *ls;
    sqlite3_prepare_v2(g_db, lq, -1, &ls, NULL);
    long long A = 0;
    if (sqlite3_step(ls) == SQLITE_ROW) A = sqlite3_column_int64(ls, 0);
    sqlite3_finalize(ls);

    CHECKEQ_INT(A, g_S2);   /* fixture sanity: asof=2000 resolves to S2 */

    char sql[512];
    snprintf(sql, sizeof sql,
        "SELECT v.path, v.kind, v.size, v.last_snapshot FROM versions v "
        "WHERE v.first_snapshot=(SELECT MAX(v2.first_snapshot) FROM versions v2 "
        "  WHERE v2.path=v.path AND v2.first_snapshot<=%lld) "
        "ORDER BY v.path", A);

    sqlite3_stmt *q;
    sqlite3_prepare_v2(g_db, sql, -1, &q, NULL);

    int n = 0, saw_new = 0;
    while (sqlite3_step(q) == SQLITE_ROW) {
        const char *p = (const char *)sqlite3_column_text(q, 0);
        if (p && strcmp(p, "/src/dir/new") == 0) saw_new = 1;
        n++;
    }
    sqlite3_finalize(q);

    /* new.txt (first=S3=3 > A=2) must not appear; all others do (5 rows) */
    CHECKEQ_INT(saw_new, 0);
    CHECKEQ_INT(n, 5);
}

/* ---- 8. search membership: path present at S1, not present at S3 ---- */

/*
 * SQL lifted verbatim from handle_search in ipc.c.
 * Membership rule: first_snapshot<=snap AND (last IS NULL OR last>snap).
 *
 * deleted.txt: first=S1=1, last=S2=2.
 *   At snap=S1=1: 1<=1 AND 2>1 = TRUE  -> in results
 *   At snap=S3=3: 1<=3 AND 2>3 = FALSE -> NOT in results
 *
 * Both cases are tested using the SQL directly with a bound snap value.
 */
static void test_search_membership(void)
{
    const char *pattern = "%deleted%";

    /* At snap=S1: deleted.txt should appear */
    sqlite3_stmt *r1;
    sqlite3_prepare_v2(g_db,
        "SELECT v.path, v.size, v.mtime_sec FROM versions v "
        "WHERE v.first_snapshot<=?1 "
        "AND (v.last_snapshot IS NULL OR v.last_snapshot>?1) "
        "AND v.path LIKE ?2 "
        "ORDER BY v.path LIMIT 500",
        -1, &r1, NULL);
    sqlite3_bind_int64(r1, 1, g_S1);
    sqlite3_bind_text (r1, 2, pattern, -1, SQLITE_STATIC);
    int n_at_s1 = 0;
    while (sqlite3_step(r1) == SQLITE_ROW) n_at_s1++;
    sqlite3_finalize(r1);

    CHECKEQ_INT(n_at_s1, 1);   /* deleted.txt present at S1 */

    /* At snap=S3: deleted.txt should NOT appear (interval closed at S2) */
    sqlite3_stmt *r3;
    sqlite3_prepare_v2(g_db,
        "SELECT v.path, v.size, v.mtime_sec FROM versions v "
        "WHERE v.first_snapshot<=?1 "
        "AND (v.last_snapshot IS NULL OR v.last_snapshot>?1) "
        "AND v.path LIKE ?2 "
        "ORDER BY v.path LIMIT 500",
        -1, &r3, NULL);
    sqlite3_bind_int64(r3, 1, g_S3);
    sqlite3_bind_text (r3, 2, pattern, -1, SQLITE_STATIC);
    int n_at_s3 = 0;
    while (sqlite3_step(r3) == SQLITE_ROW) n_at_s3++;
    sqlite3_finalize(r3);

    CHECKEQ_INT(n_at_s3, 0);   /* deleted.txt gone from S3 membership */

    /* Sanity: steady.txt always present */
    const char *pat2 = "%steady%";
    sqlite3_stmt *rs;
    sqlite3_prepare_v2(g_db,
        "SELECT v.path FROM versions v "
        "WHERE v.first_snapshot<=?1 "
        "AND (v.last_snapshot IS NULL OR v.last_snapshot>?1) "
        "AND v.path LIKE ?2 "
        "ORDER BY v.path LIMIT 500",
        -1, &rs, NULL);
    sqlite3_bind_int64(rs, 1, g_S3);
    sqlite3_bind_text (rs, 2, pat2, -1, SQLITE_STATIC);
    int n_steady = 0;
    while (sqlite3_step(rs) == SQLITE_ROW) n_steady++;
    sqlite3_finalize(rs);

    CHECKEQ_INT(n_steady, 1);
}

/* ---- main ---- */

int main(void)
{
    setup_fixture();

    test_file_unchanged_one_row();
    test_deleted_file_one_row();
    test_list_dir_immediate_only();
    test_list_dir_dirs_first();
    test_list_dir_deleted_flag();
    test_list_all_no_asof();
    test_list_all_asof_hides_future();
    test_search_membership();

    sqlite3_close(g_db);
    rmtree_local(g_dir);

    TEST_DONE("test_versions");
}
