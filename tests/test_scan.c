/*
 * test_scan.c -- change-detection matrix, sweep, and regression tests
 *                (Phase 3).
 *
 * Uses a hand-wired Ctx/User (same pattern as test_sweep.c) with a temp
 * on-disk tree and a temp catalog opened via db_open (real full schema).
 *
 * REGRESSION TESTS: uid change -> DIRTY, gid change -> DIRTY.  These must
 * fail on any revision of scan.c that drops the uid/gid comparison.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sqlite3.h>

#include "cli/ctx.h"
#include "cli/commands.h"
#include "common/config.h"
#include "common/db.h"
#include "common/types.h"

#include "test_common.h"

static int fails;

/* ---- low-level helpers ---- */

static void mkdirp(const char *p) { mkdir(p, 0755); }

static void touch(const char *p)
{
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) close(fd);
}

static int row_exists(sqlite3 *db, const char *path)
{
    sqlite3_stmt *q;
    sqlite3_prepare_v2(db, "SELECT 1 FROM files WHERE path=?", -1, &q, NULL);
    sqlite3_bind_text(q, 1, path, -1, SQLITE_STATIC);
    int found = (sqlite3_step(q) == SQLITE_ROW);
    sqlite3_finalize(q);
    return found;
}

static int file_state(sqlite3 *db, const char *path)
{
    sqlite3_stmt *q;
    sqlite3_prepare_v2(db, "SELECT state FROM files WHERE path=?", -1, &q, NULL);
    sqlite3_bind_text(q, 1, path, -1, SQLITE_STATIC);
    int state = -1;
    if (sqlite3_step(q) == SQLITE_ROW) state = sqlite3_column_int(q, 0);
    sqlite3_finalize(q);
    return state;
}

/*
 * Insert a files row with all stat fields (including mtime_nsec).
 * uid and gid can be overridden independently of the stat struct.
 */
static void ins_all_fields(sqlite3 *db, const char *path,
                           int kind,
                           long long size, long long mtime_sec,
                           long long mtime_nsec, long long ino,
                           long long dev, long long mode,
                           long long uid, long long gid,
                           int state, long long seen)
{
    sqlite3_stmt *q;
    sqlite3_prepare_v2(db,
        "INSERT INTO files(path,kind,size,mtime_sec,mtime_nsec,ino,dev,"
        "mode,uid,gid,state,seen) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)",
        -1, &q, NULL);
    sqlite3_bind_text (q,  1, path, -1, SQLITE_STATIC);
    sqlite3_bind_int  (q,  2, kind);
    sqlite3_bind_int64(q,  3, size);
    sqlite3_bind_int64(q,  4, mtime_sec);
    sqlite3_bind_int64(q,  5, mtime_nsec);
    sqlite3_bind_int64(q,  6, ino);
    sqlite3_bind_int64(q,  7, dev);
    sqlite3_bind_int64(q,  8, mode);
    sqlite3_bind_int64(q,  9, uid);
    sqlite3_bind_int64(q, 10, gid);
    sqlite3_bind_int  (q, 11, state);
    sqlite3_bind_int64(q, 12, seen);
    if (sqlite3_step(q) != SQLITE_DONE)
        printf("ins_all_fields failed: %s\n", sqlite3_errmsg(db));
    sqlite3_finalize(q);
}

/* Insert a files row carrying a version_id for interval-close tests. */
static void ins_file_with_vid(sqlite3 *db, const char *path,
                              long long version_id, long long seen)
{
    sqlite3_stmt *q;
    sqlite3_prepare_v2(db,
        "INSERT INTO files(path,kind,size,mtime_sec,mtime_nsec,ino,dev,mode,"
        "uid,gid,state,seen,version_id) VALUES(?,0,0,0,0,0,0,0,0,0,0,?,?)",
        -1, &q, NULL);
    sqlite3_bind_text (q, 1, path, -1, SQLITE_STATIC);
    sqlite3_bind_int64(q, 2, seen);
    sqlite3_bind_int64(q, 3, version_id);
    if (sqlite3_step(q) != SQLITE_DONE)
        printf("ins_file_with_vid failed: %s\n", sqlite3_errmsg(db));
    sqlite3_finalize(q);
}

/* Return last_snapshot of a version row; -1 = NULL (still current). */
static long long version_last_snap(sqlite3 *db, long long version_id)
{
    sqlite3_stmt *q;
    sqlite3_prepare_v2(db, "SELECT last_snapshot FROM versions WHERE id=?",
                       -1, &q, NULL);
    sqlite3_bind_int64(q, 1, version_id);
    long long last = -2;  /* -2 = not found */
    if (sqlite3_step(q) == SQLITE_ROW) {
        if (sqlite3_column_type(q, 0) == SQLITE_NULL)
            last = -1;   /* NULL: version still current */
        else
            last = sqlite3_column_int64(q, 0);
    }
    sqlite3_finalize(q);
    return last;
}

/* ---- new file on disk, not yet in catalog -> inserted DIRTY ---- */

static void test_new_file_dirty(void)
{
    char base[64]; tmpdir(base, sizeof base);
    char src[256], filepath[256], catpath[256];
    snprintf(src,      sizeof src,      "%s/src",          base);
    snprintf(filepath, sizeof filepath, "%s/src/hello.txt", base);
    snprintf(catpath,  sizeof catpath,  "%s/catalog.db",   base);
    mkdirp(src);
    touch(filepath);

    sqlite3 *db = db_open(catpath);

    char *sources[1] = { src };
    User u; memset(&u, 0, sizeof u);
    u.name = "test"; u.db = catpath;
    u.sources = sources; u.nsources = 1;
    Ctx c; memset(&c, 0, sizeof c);
    c.src = &u; c.db = db;

    scan_run_subtree(&c, 1, src);

    CHECK(row_exists(db, filepath));
    CHECKEQ_INT(file_state(db, filepath), FS_DIRTY);

    sqlite3_close(db);
    rmtree_local(base);
}

/* ---- unchanged file -> stays CLEAN, seen updated ---- */

static void test_unchanged_stays_clean(void)
{
    char base[64]; tmpdir(base, sizeof base);
    char src[256], filepath[256], catpath[256];
    snprintf(src,      sizeof src,      "%s/src",            base);
    snprintf(filepath, sizeof filepath, "%s/src/steady.txt", base);
    snprintf(catpath,  sizeof catpath,  "%s/catalog.db",     base);
    mkdirp(src);
    touch(filepath);

    struct stat st;
    lstat(filepath, &st);

    sqlite3 *db = db_open(catpath);
    ins_all_fields(db, filepath, FK_REG,
                   (long long)st.st_size,
                   (long long)st.st_mtim.tv_sec,
                   (long long)st.st_mtim.tv_nsec,
                   (long long)st.st_ino, (long long)st.st_dev,
                   (long long)st.st_mode,
                   (long long)st.st_uid, (long long)st.st_gid,
                   FS_CLEAN, 1);

    char *sources[1] = { src };
    User u; memset(&u, 0, sizeof u);
    u.name = "test"; u.db = catpath;
    u.sources = sources; u.nsources = 1;
    Ctx ct; memset(&ct, 0, sizeof ct);
    ct.src = &u; ct.db = db;

    scan_run_subtree(&ct, 2, src);

    CHECKEQ_INT(file_state(db, filepath), FS_CLEAN);

    sqlite3_close(db);
    rmtree_local(base);
}

/*
 * Macro that creates a one-file source tree, inserts a catalog row with ONE
 * field mutated vs the on-disk stat, runs scan_run_subtree, and asserts DIRTY.
 * Used for tests 3-9.
 */
#define DIRTY_WHEN_DIFF(testname, field_expr, val_expr) \
static void testname(void) \
{ \
    char base[64]; tmpdir(base, sizeof base); \
    char src[256], filepath[256], catpath[256]; \
    snprintf(src,      sizeof src,      "%s/src", base); \
    snprintf(filepath, sizeof filepath, "%s/src/f.txt", base); \
    snprintf(catpath,  sizeof catpath,  "%s/catalog.db", base); \
    mkdirp(src); touch(filepath); \
    struct stat st; lstat(filepath, &st); \
    sqlite3 *db = db_open(catpath); \
    ins_all_fields(db, filepath, FK_REG, \
                   (long long)st.st_size, \
                   (long long)st.st_mtim.tv_sec, \
                   (long long)st.st_mtim.tv_nsec, \
                   (long long)st.st_ino, (long long)st.st_dev, \
                   (long long)st.st_mode, \
                   (long long)st.st_uid, (long long)st.st_gid, \
                   FS_CLEAN, 1); \
    sqlite3_stmt *upd_q; \
    sqlite3_prepare_v2(db, "UPDATE files SET " field_expr "=? WHERE path=?", \
                       -1, &upd_q, NULL); \
    sqlite3_bind_int64(upd_q, 1, (val_expr)); \
    sqlite3_bind_text (upd_q, 2, filepath, -1, SQLITE_STATIC); \
    sqlite3_step(upd_q); sqlite3_finalize(upd_q); \
    char *sources[1] = { src }; \
    User u; memset(&u, 0, sizeof u); \
    u.name = "test"; u.db = catpath; \
    u.sources = sources; u.nsources = 1; \
    Ctx ct; memset(&ct, 0, sizeof ct); ct.src = &u; ct.db = db; \
    scan_run_subtree(&ct, 2, src); \
    CHECKEQ_INT(file_state(db, filepath), FS_DIRTY); \
    sqlite3_close(db); rmtree_local(base); \
}

DIRTY_WHEN_DIFF(test_size_change_dirty,     "size",
                (long long)st.st_size + 1)
DIRTY_WHEN_DIFF(test_mtime_sec_dirty,       "mtime_sec",
                (long long)st.st_mtim.tv_sec - 1)
DIRTY_WHEN_DIFF(test_mtime_nsec_dirty,      "mtime_nsec",
                (st.st_mtim.tv_nsec == 0 ? 1LL : 0LL))
DIRTY_WHEN_DIFF(test_mode_dirty,            "mode",
                (long long)(st.st_mode ^ 0111))
/*
 * REGRESSION TESTS: uid and gid change must trigger DIRTY.
 * These fail on old scan.c code that omitted uid/gid from the change check.
 * To simulate chown without root: insert the row with a wrong uid/gid, then
 * scan.  The on-disk file has the real uid/gid; the mismatch is detected.
 */
DIRTY_WHEN_DIFF(test_uid_dirty,             "uid",      99999LL)
DIRTY_WHEN_DIFF(test_gid_dirty,             "gid",      99999LL)
DIRTY_WHEN_DIFF(test_ino_dirty,             "ino",
                (long long)st.st_ino + 1)
DIRTY_WHEN_DIFF(test_dev_dirty,             "dev",
                (long long)st.st_dev + 1)

/* ---- excluded path not inserted ---- */

static void test_excluded_path(void)
{
    char base[64]; tmpdir(base, sizeof base);
    char src[256], excdir[256], excfile[256], good[256], catpath[256];
    snprintf(src,     sizeof src,     "%s/src",              base);
    snprintf(excdir,  sizeof excdir,  "%s/src/skip",         base);
    snprintf(excfile, sizeof excfile, "%s/src/skip/hide.txt",base);
    snprintf(good,    sizeof good,    "%s/src/good.txt",     base);
    snprintf(catpath, sizeof catpath, "%s/catalog.db",       base);
    mkdirp(src); mkdirp(excdir);
    touch(excfile); touch(good);

    sqlite3 *db = db_open(catpath);

    /* exclude the directory named "skip" */
    char excl[] = "skip";
    char *excludes_arr[1] = { excl };

    char *sources[1] = { src };
    User u; memset(&u, 0, sizeof u);
    u.name = "test"; u.db = catpath;
    u.sources = sources; u.nsources = 1;
    u.excludes = excludes_arr; u.nexcludes = 1;
    Ctx ct; memset(&ct, 0, sizeof ct);
    ct.src = &u; ct.db = db;

    scan_run_subtree(&ct, 1, src);

    CHECK( row_exists(db, good));     /* non-excluded file was scanned */
    CHECK(!row_exists(db, excdir));   /* excluded dir itself not inserted */
    CHECK(!row_exists(db, excfile));  /* child of excluded dir not inserted */

    sqlite3_close(db);
    rmtree_local(base);
}

/* ---- catalog db/-wal/-shm under the source are ignored ---- */

static void test_catalog_files_ignored(void)
{
    char base[64]; tmpdir(base, sizeof base);
    /* put the source AND the catalog inside the same directory */
    char src[256], catpath[256], good[256];
    snprintf(src,     sizeof src,     "%s",              base);
    snprintf(catpath, sizeof catpath, "%s/catalog.db",   base);
    snprintf(good,    sizeof good,    "%s/data.txt",     base);
    touch(good);

    sqlite3 *db = db_open(catpath);   /* creates catalog.db (and wal/shm) */

    char *sources[1] = { src };
    User u; memset(&u, 0, sizeof u);
    u.name = "test"; u.db = catpath;  /* u.db = path scan_open ignores */
    u.sources = sources; u.nsources = 1;
    Ctx ct; memset(&ct, 0, sizeof ct);
    ct.src = &u; ct.db = db;

    scan_run_subtree(&ct, 1, src);

    /* catalog itself and its WAL sidecars must not appear in the files table */
    CHECK(!row_exists(db, catpath));
    char wal[256], shm[256];
    snprintf(wal, sizeof wal, "%s-wal", catpath);
    snprintf(shm, sizeof shm, "%s-shm", catpath);
    CHECK(!row_exists(db, wal));
    CHECK(!row_exists(db, shm));
    CHECK( row_exists(db, good));   /* non-catalog file was scanned */

    sqlite3_close(db);
    rmtree_local(base);
}

/* ---- vanished file: row deleted + open version interval closed ---- */

static void test_vanished_file_swept(void)
{
    char base[64]; tmpdir(base, sizeof base);
    char src[256], live[256], vanished[256], catpath[256];
    snprintf(src,      sizeof src,      "%s/src",                 base);
    snprintf(live,     sizeof live,     "%s/src/live.txt",        base);
    snprintf(vanished, sizeof vanished, "%s/src/vanished.txt",    base);
    snprintf(catpath,  sizeof catpath,  "%s/catalog.db",          base);
    mkdirp(src);
    touch(live);
    /* vanished.txt is NOT created on disk */

    sqlite3 *db = db_open(catpath);

    /* Create a snapshot for the previous scan pass */
    long long s1 = ins_snapshot(db, 1000000, SS_COMPLETE, SK_SCHEDULED);

    /* Open version for the vanished file */
    long long vid = ins_version(db, vanished, FK_REG, 0, s1, -1 /*NULL*/);

    /* File row pointing at the open version, seen=1 (stale) */
    ins_file_with_vid(db, vanished, vid, 1);

    /* Current scan snapshot */
    long long s2 = ins_snapshot(db, 2000000, SS_OPEN, SK_SCHEDULED);

    char *sources[1] = { src };
    User u; memset(&u, 0, sizeof u);
    u.name = "test"; u.db = catpath;
    u.sources = sources; u.nsources = 1;
    Ctx ct; memset(&ct, 0, sizeof ct);
    ct.src = &u; ct.db = db;

    scan_run(&ct, s2);

    /* Vanished file: row must be deleted */
    CHECK(!row_exists(db, vanished));

    /* Its open version interval must be closed at the current scan (s2) */
    CHECKEQ_INT((int)version_last_snap(db, vid), (int)s2);

    /* Live file: row must exist (newly inserted, DIRTY) */
    CHECK(row_exists(db, live));

    sqlite3_close(db);
    rmtree_local(base);
}

/* ---- incomplete walk skips sweep (unreadable dir simulated via chmod 000) ---- */

static void test_incomplete_walk_no_sweep(void)
{
    char base[64]; tmpdir(base, sizeof base);
    char src[256], unread[256], hidden[256], catpath[256];
    snprintf(src,     sizeof src,     "%s/src",                base);
    snprintf(unread,  sizeof unread,  "%s/src/sub",            base);
    snprintf(hidden,  sizeof hidden,  "%s/src/sub/secret.txt", base);
    snprintf(catpath, sizeof catpath, "%s/catalog.db",         base);
    mkdirp(src); mkdirp(unread);
    touch(hidden);

    sqlite3 *db = db_open(catpath);

    /* Catalog row for the file under the unreadable dir, seen=1 (stale) */
    ins_file(db, hidden, FK_REG, 0, 0, 0, 0, 0644, 0, 0, FS_CLEAN, 1);

    char *sources[1] = { src };
    User u; memset(&u, 0, sizeof u);
    u.name = "test"; u.db = catpath;
    u.sources = sources; u.nsources = 1;
    Ctx ct; memset(&ct, 0, sizeof ct);
    ct.src = &u; ct.db = db;

    /* Make the subdir unreadable to cause an incomplete walk */
    chmod(unread, 0000);
    scan_run(&ct, 2);  /* seen=2, stale rows have seen=1 */
    chmod(unread, 0755);  /* restore so rmtree_local can clean up */

    /* The sweep must have been SKIPPED: the stale row for hidden is preserved */
    CHECK(row_exists(db, hidden));

    sqlite3_close(db);
    rmtree_local(base);
}

/*
 * ---- subtree sweep: deleted child under D swept by scan_run_subtree(root=D)
 *
 * This confirms that the watcher's parent-fold (queuing the parent dir when a
 * child is deleted) leads to correct catalog cleanup in continuous backups.
 * The fanotify path is integration-tested; this is the unit view of the seam.
 */
static void test_subtree_sweep_delete_child(void)
{
    char base[64]; tmpdir(base, sizeof base);
    char src[256], dirD[256], child[256], catpath[256];
    snprintf(src,     sizeof src,     "%s/src",          base);
    snprintf(dirD,    sizeof dirD,    "%s/src/D",        base);
    snprintf(child,   sizeof child,   "%s/src/D/c.txt",  base);
    snprintf(catpath, sizeof catpath, "%s/catalog.db",   base);
    mkdirp(src); mkdirp(dirD);
    touch(child);

    sqlite3 *db = db_open(catpath);

    /* Row for child, seen=1 */
    ins_file(db, child, FK_REG, 0, 0, 0, 0, 0644, 0, 0, FS_CLEAN, 1);

    /* Now remove child from disk */
    unlink(child);

    char *sources[1] = { src };
    User u; memset(&u, 0, sizeof u);
    u.name = "test"; u.db = catpath;
    u.sources = sources; u.nsources = 1;
    Ctx ct; memset(&ct, 0, sizeof ct);
    ct.src = &u; ct.db = db;

    /* The watcher folds the parent (dirD); scan_run_subtree with root=dirD
       should detect that child is gone and sweep its row. */
    scan_run_subtree(&ct, 2, dirD);

    CHECK(!row_exists(db, child));   /* swept: the child's row is gone */

    sqlite3_close(db);
    rmtree_local(base);
}

/* ---- main ---- */

int main(void)
{
    test_new_file_dirty();
    test_unchanged_stays_clean();
    test_size_change_dirty();
    test_mtime_sec_dirty();
    test_mtime_nsec_dirty();
    test_mode_dirty();
    test_uid_dirty();    /* REGRESSION: uid chown must go DIRTY */
    test_gid_dirty();    /* REGRESSION: gid chown must go DIRTY */
    test_ino_dirty();
    test_dev_dirty();
    test_excluded_path();
    test_catalog_files_ignored();
    test_vanished_file_swept();
    test_incomplete_walk_no_sweep();
    test_subtree_sweep_delete_child();
    TEST_DONE("test_scan");
}
