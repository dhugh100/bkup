#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "commands.h"
#include "platform/platform.h"
#include "common/db.h"
#include "common/types.h"
#include "common/log.h"

typedef struct {
    Ctx          *c;
    long long     seen;
    sqlite3_stmt *sel;      /* lookup by path */
    sqlite3_stmt *ins;      /* insert new dirty row */
    sqlite3_stmt *upd_dirty;
    sqlite3_stmt *upd_seen;
    /* paths to ignore: the catalog db and its WAL/SHM sidecars */
    char         *db_path;
    char         *db_wal;
    char         *db_shm;
} Scan;

static int is_catalog_file(const Scan *s, const char *path)
{
    return !strcmp(path, s->db_path) || !strcmp(path, s->db_wal) ||
           !strcmp(path, s->db_shm);
}

static int walk_cb(const char *path, const struct stat *st, int kind,
                   void *user)
{
    Scan *s = user;

    if (user_excluded(s->c->src, path))
        return kind == FK_DIR ? PWALK_SKIP : PWALK_OK;
    if (is_catalog_file(s, path))
        return PWALK_OK;

    long long size      = (long long)st->st_size;
    long long mtime_sec = (long long)st->st_mtim.tv_sec;
    long long mtime_ns  = (long long)st->st_mtim.tv_nsec;
    long long ino       = (long long)st->st_ino;
    long long dev       = (long long)st->st_dev;
    long long mode      = (long long)st->st_mode;
    long long uid       = (long long)st->st_uid;
    long long gid       = (long long)st->st_gid;

    sqlite3_reset(s->sel);
    sqlite3_bind_text(s->sel, 1, path, -1, SQLITE_STATIC);
    int found = (sqlite3_step(s->sel) == SQLITE_ROW);

    if (!found) {
        sqlite3_stmt *q = s->ins;
        sqlite3_reset(q);
        sqlite3_bind_text(q, 1, path, -1, SQLITE_STATIC);
        sqlite3_bind_int (q, 2, kind);
        sqlite3_bind_int64(q, 3, size);
        sqlite3_bind_int64(q, 4, mtime_sec);
        sqlite3_bind_int64(q, 5, mtime_ns);
        sqlite3_bind_int64(q, 6, ino);
        sqlite3_bind_int64(q, 7, dev);
        sqlite3_bind_int64(q, 8, mode);
        sqlite3_bind_int64(q, 9, uid);
        sqlite3_bind_int64(q, 10, gid);
        sqlite3_bind_int64(q, 11, s->seen);
        db_step_done(s->c->db, q, "scan insert");
        return PWALK_OK;
    }

    /* existing row: cols 0..9 = id,kind,size,mtime_sec,mtime_nsec,ino,dev,mode,uid,gid */
    long long oldkind = sqlite3_column_int64(s->sel, 1);
    long long osize   = sqlite3_column_int64(s->sel, 2);
    long long omsec   = sqlite3_column_int64(s->sel, 3);
    long long omns    = sqlite3_column_int64(s->sel, 4);
    long long oino    = sqlite3_column_int64(s->sel, 5);
    long long odev    = sqlite3_column_int64(s->sel, 6);
    long long omode   = sqlite3_column_int64(s->sel, 7);
    long long ouid    = sqlite3_column_int64(s->sel, 8);
    long long ogid    = sqlite3_column_int64(s->sel, 9);
    long long id      = sqlite3_column_int64(s->sel, 0);

    /* uid/gid included so a chown-only change (content, size, mtime, mode all
       unchanged) still mints a new version -- otherwise a restore would replay
       the stale owner. */
    int changed = (oldkind != kind) || (osize != size) ||
                  (omsec != mtime_sec) || (omns != mtime_ns) ||
                  (oino != ino) || (odev != dev) || (omode != mode) ||
                  (ouid != uid) || (ogid != gid);

    if (changed) {
        sqlite3_stmt *q = s->upd_dirty;
        sqlite3_reset(q);
        sqlite3_bind_int (q, 1, kind);
        sqlite3_bind_int64(q, 2, size);
        sqlite3_bind_int64(q, 3, mtime_sec);
        sqlite3_bind_int64(q, 4, mtime_ns);
        sqlite3_bind_int64(q, 5, ino);
        sqlite3_bind_int64(q, 6, dev);
        sqlite3_bind_int64(q, 7, mode);
        sqlite3_bind_int64(q, 8, uid);
        sqlite3_bind_int64(q, 9, gid);
        sqlite3_bind_int64(q, 10, s->seen);
        sqlite3_bind_int64(q, 11, id);
        db_step_done(s->c->db, q, "scan update");
    } else {
        sqlite3_stmt *q = s->upd_seen;
        sqlite3_reset(q);
        sqlite3_bind_int64(q, 1, s->seen);
        sqlite3_bind_int64(q, 2, id);
        db_step_done(s->c->db, q, "scan seen-update");
    }
    return PWALK_OK;
}

/* Prepare the per-scan statements and the catalog-file ignore paths. */
static void scan_open(Scan *s, Ctx *c, long long seen)
{
    memset(s, 0, sizeof *s);
    s->c = c;
    s->seen = seen;
    s->db_path = xstrdup(c->src->db);
    s->db_wal = xmalloc(strlen(c->src->db) + 5);
    s->db_shm = xmalloc(strlen(c->src->db) + 5);
    sprintf(s->db_wal, "%s-wal", c->src->db);
    sprintf(s->db_shm, "%s-shm", c->src->db);

    s->sel = db_prep(c->db,
        "SELECT id,kind,size,mtime_sec,mtime_nsec,ino,dev,mode,uid,gid "
        "FROM files WHERE path=?");
    s->ins = db_prep(c->db,
        "INSERT INTO files(path,kind,size,mtime_sec,mtime_nsec,ino,dev,mode,"
        "uid,gid,state,seen) VALUES(?,?,?,?,?,?,?,?,?,?,1,?)");
    s->upd_dirty = db_prep(c->db,
        "UPDATE files SET kind=?,size=?,mtime_sec=?,mtime_nsec=?,ino=?,dev=?,"
        "mode=?,uid=?,gid=?,state=1,seen=? WHERE id=?");
    s->upd_seen = db_prep(c->db,
        "UPDATE files SET seen=? WHERE id=?");
}

static void scan_close(Scan *s)
{
    sqlite3_finalize(s->sel);
    sqlite3_finalize(s->ins);
    sqlite3_finalize(s->upd_dirty);
    sqlite3_finalize(s->upd_seen);
    free(s->db_path); free(s->db_wal); free(s->db_shm);
}

void scan_run(Ctx *c, long long seen)
{
    Scan s;
    scan_open(&s, c, seen);

    int incomplete = 0;
    db_exec(c->db, "BEGIN");
    for (int i = 0; i < c->src->nsources; i++) {
        int r = platform_walk(c->src->sources[i], walk_cb, &s);
        if (r < 0) {
            log_err("backup: source %s could not be read at all -- skipping "
                    "stale-entry cleanup (will not drop files that may still "
                    "exist)", c->src->sources[i]);
            incomplete = 1;
        } else if (r > 0) {
            log_warn("backup: source %s had %d unreadable path(s) -- skipping "
                     "stale-entry cleanup this pass", c->src->sources[i], r);
            incomplete = 1;
        }
    }

    /* vanished files: present in catalog but not seen this scan. Only safe when
       the walk was COMPLETE -- an unreadable dir leaves its files "unseen", and
       deleting them would silently drop still-existing files from the catalog
       (and from this snapshot). On an incomplete walk we keep those rows; they
       carry forward unchanged, and a later complete scan reconciles. */
    if (!incomplete) {
        /* close the interval of every vanished file's current version: it was
           live up to the prior snapshot, so its half-open interval ends at this
           one (`seen` is this snapshot's id). Must run before the rows are
           deleted, while files.version_id still points at the version. */
        sqlite3_stmt *cl = db_prep(c->db,
            "UPDATE versions SET last_snapshot=?1 WHERE last_snapshot IS NULL "
            "AND id IN (SELECT version_id FROM files "
            "           WHERE (seen!=?1 OR seen IS NULL) AND version_id IS NOT NULL)");
        sqlite3_bind_int64(cl, 1, seen);
        db_step_done(c->db, cl, "stale-entry interval close");
        sqlite3_finalize(cl);

        sqlite3_stmt *del = db_prep(c->db,
            "DELETE FROM files WHERE seen!=? OR seen IS NULL");
        sqlite3_bind_int64(del, 1, seen);
        db_step_done(c->db, del, "stale-entry sweep");
        sqlite3_finalize(del);
    } else {
        log_warn("backup: stale-entry cleanup SKIPPED due to unreadable paths; "
                 "any genuinely deleted files are reconciled on the next "
                 "complete backup");
    }
    db_exec(c->db, "COMMIT");

    scan_close(&s);
}

/* Scan a single subtree `root` rather than every configured source, and scope
   the vanished-file sweep to that subtree. This is the engine of continuous backups:
   the carry-forward of every CLEAN file (done by the caller) keeps the snapshot
   a complete image of the source, so here we only need to refresh `root`.

   The sweep is the dangerous line. A full scan deletes every row not seen this
   pass; doing that here would wipe the entire catalog outside `root`. We scope
   it to `root` itself plus everything strictly under `root + "/"`, using a
   prefix compare (substr) rather than GLOB so that glob metacharacters in the
   path (*, ?, [, ]) cannot widen or narrow the match. */
void scan_run_subtree(Ctx *c, long long seen, const char *root)
{
    Scan s;
    scan_open(&s, c, seen);

    size_t rlen = strlen(root);
    char *prefix = xmalloc(rlen + 2);   /* root + "/" + NUL */
    memcpy(prefix, root, rlen);
    prefix[rlen] = '/';
    prefix[rlen + 1] = '\0';

    db_exec(c->db, "BEGIN");
    int r = platform_walk(root, walk_cb, &s);
    if (r != 0)
        log_warn("continuous: walk of %s incomplete (%s) -- skipping stale-entry "
                 "cleanup for this subtree", root,
                 r < 0 ? "could not open" : "unreadable path(s)");

    /* vanished files within the subtree only: path == root, or path begins
       with root + "/". Rows outside the subtree are never touched. Skipped on
       an incomplete walk for the same reason as the full scan (see scan_run):
       an unreadable dir must not be mistaken for deletion. */
    if (r == 0) {
        /* close intervals for vanished files in the subtree before deleting the
           rows (same reasoning as scan_run; scoped to the subtree). */
        sqlite3_stmt *cl = db_prep(c->db,
            "UPDATE versions SET last_snapshot=?4 WHERE last_snapshot IS NULL "
            "AND id IN (SELECT version_id FROM files "
            "           WHERE (path=?1 OR substr(path,1,?2)=?3) "
            "           AND (seen!=?4 OR seen IS NULL) AND version_id IS NOT NULL)");
        sqlite3_bind_text (cl, 1, root, -1, SQLITE_STATIC);
        sqlite3_bind_int  (cl, 2, (int)(rlen + 1));
        sqlite3_bind_text (cl, 3, prefix, -1, SQLITE_STATIC);
        sqlite3_bind_int64(cl, 4, seen);
        db_step_done(c->db, cl, "continuous stale-entry interval close");
        sqlite3_finalize(cl);

        sqlite3_stmt *del = db_prep(c->db,
            "DELETE FROM files "
            "WHERE (path=?1 OR substr(path,1,?2)=?3) "
            "AND (seen!=?4 OR seen IS NULL)");
        sqlite3_bind_text (del, 1, root, -1, SQLITE_STATIC);
        sqlite3_bind_int  (del, 2, (int)(rlen + 1));
        sqlite3_bind_text (del, 3, prefix, -1, SQLITE_STATIC);
        sqlite3_bind_int64(del, 4, seen);
        db_step_done(c->db, del, "continuous stale-entry sweep");
        sqlite3_finalize(del);
    }
    db_exec(c->db, "COMMIT");

    free(prefix);
    scan_close(&s);
}
