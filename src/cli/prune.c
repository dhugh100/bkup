#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>

#include "commands.h"
#include "prune_internal.h"
#include "common/db.h"
#include "common/types.h"
#include "common/transport.h"
#include "common/log.h"

/* A stable integer key identifying which day/week/month/year a time falls in.
   Two snapshots share a key iff they belong to the same period. */
long long period_key(time_t t, int kind)
{
    struct tm tm;
    localtime_r(&t, &tm);
    switch (kind) {
    case P_DAILY:   return (long long)(tm.tm_year + 1900) * 1000 + tm.tm_yday;
    case P_MONTHLY: return (long long)(tm.tm_year + 1900) * 100 + tm.tm_mon;
    case P_YEARLY:  return tm.tm_year + 1900;
    case P_WEEKLY: {
        char buf[16];
        strftime(buf, sizeof buf, "%G%V", &tm);   /* ISO year + week */
        return atoll(buf);
    }
    }
    return 0;
}

/* Keep the newest SCHEDULED snapshot in each of the most recent `count` periods
   of the given kind. snaps must be sorted by created descending. Continuous
   snapshots are retained by a separate rule (anchor to the last scheduled
   backup) and are skipped here. */
void apply_periodic(Snap *s, int n, int *keep, int kind, int count)
{
    if (count <= 0) return;
    long long last = LLONG_MIN;
    int kept = 0;
    for (int i = 0; i < n && kept < count; i++) {
        if (s[i].kind != SK_SCHEDULED) continue;
        long long k = period_key(s[i].created, kind);
        if (k != last) { keep[i] = 1; kept++; last = k; }
    }
}

/* Drop directory versions whose subtree holds no file or symlink version.
   Such a directory is dead weight: restoring it recreates an empty skeleton
   and nothing else. Every ancestor path of a surviving non-directory version
   is marked live, then the unmarked directory versions go. Ancestors are cut
   at '/' boundaries, so a sibling like "/a/bc" cannot keep "/a/b" alive.
   Nesting needs no recursion: a directory holding only empty subdirectories is
   itself unmarked, so the whole chain goes in one pass. Returns the number of
   directory versions removed. */
int prune_empty_dirs(sqlite3 *db)
{
    db_exec(db, "CREATE TEMP TABLE IF NOT EXISTS live_dirs(path TEXT PRIMARY KEY)");
    db_exec(db, "DELETE FROM live_dirs");

    sqlite3_stmt *lq = db_prep(db, "SELECT DISTINCT path FROM versions WHERE kind<>?");
    sqlite3_bind_int(lq, 1, FK_DIR);
    sqlite3_stmt *li = db_prep(db, "INSERT OR IGNORE INTO live_dirs(path) VALUES(?)");
    while (sqlite3_step(lq) == SQLITE_ROW) {
        const char *p = (const char *)sqlite3_column_text(lq, 0);
        if (!p) continue;
        for (size_t i = 1; p[i]; i++) {
            if (p[i] != '/') continue;
            sqlite3_bind_text(li, 1, p, (int)i, SQLITE_TRANSIENT);
            sqlite3_step(li); sqlite3_reset(li);
        }
    }
    sqlite3_finalize(li);
    sqlite3_finalize(lq);

    int n = 0;
    sqlite3_stmt *dc = db_prep(db,
        "SELECT COUNT(*) FROM versions WHERE kind=? "
        "AND path NOT IN (SELECT path FROM live_dirs)");
    sqlite3_bind_int(dc, 1, FK_DIR);
    if (sqlite3_step(dc) == SQLITE_ROW) n = sqlite3_column_int(dc, 0);
    sqlite3_finalize(dc);
    if (n == 0) return 0;

    /* The files table may still point at a removed version (an empty directory
       that is present on disk right now). Clear the reference so the pointer
       never dangles; the next backup re-captures the directory if it has gained
       content by then. */
    sqlite3_stmt *uf = db_prep(db,
        "UPDATE files SET version_id=NULL WHERE version_id IN "
        "(SELECT id FROM versions WHERE kind=? "
        " AND path NOT IN (SELECT path FROM live_dirs))");
    sqlite3_bind_int(uf, 1, FK_DIR);
    sqlite3_step(uf); sqlite3_finalize(uf);

    sqlite3_stmt *dd = db_prep(db,
        "DELETE FROM versions WHERE kind=? "
        "AND path NOT IN (SELECT path FROM live_dirs)");
    sqlite3_bind_int(dd, 1, FK_DIR);
    sqlite3_step(dd); sqlite3_finalize(dd);
    return n;
}

int cmd_prune(Ctx *c, int argc, char **argv)
{
    int keep_last = 0, keep_daily = 0, keep_weekly = 0,
        keep_monthly = 0, keep_yearly = 0;
    int dry_run = 0;

    for (int i = 0; i < argc; i++) {
        if      (!strcmp(argv[i], "--keep-last")    && i+1 < argc) keep_last    = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--keep-daily")   && i+1 < argc) keep_daily   = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--keep-weekly")  && i+1 < argc) keep_weekly  = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--keep-monthly") && i+1 < argc) keep_monthly = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--keep-yearly")  && i+1 < argc) keep_yearly  = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-n") || !strcmp(argv[i], "--dry-run")) dry_run = 1;
        else die("prune: unknown argument '%s'", argv[i]);
    }

    if (keep_last + keep_daily + keep_weekly + keep_monthly + keep_yearly < 1)
        die("prune: specify at least one --keep-* rule "
            "(--keep-last/daily/weekly/monthly/yearly)");

    ctx_open_db(c);

    /* Load all complete snapshots, newest first. */
    int cap = 64, n = 0;
    Snap *s = xmalloc((size_t)cap * sizeof *s);
    sqlite3_stmt *q = db_prep(c->db,
        "SELECT id, created, kind FROM snapshots WHERE state=1 ORDER BY created DESC, id DESC");
    while (sqlite3_step(q) == SQLITE_ROW) {
        if (n == cap) { cap *= 2; s = xrealloc(s, (size_t)cap * sizeof *s); }
        s[n].id      = sqlite3_column_int64(q, 0);
        s[n].created = sqlite3_column_int64(q, 1);
        s[n].kind    = sqlite3_column_int(q, 2);
        n++;
    }
    sqlite3_finalize(q);

    if (n == 0) { log_info("prune: no complete snapshots"); free(s); return 0; }

    int *keep = xcalloc((size_t)n, sizeof *keep);

    /* The keep-* rules apply to SCHEDULED snapshots only (long-term retention).
       keep-last keeps the newest `keep_last` scheduled snapshots. */
    for (int i = 0, kept = 0; i < n && kept < keep_last; i++)
        if (s[i].kind == SK_SCHEDULED) { keep[i] = 1; kept++; }
    apply_periodic(s, n, keep, P_DAILY,   keep_daily);
    apply_periodic(s, n, keep, P_WEEKLY,  keep_weekly);
    apply_periodic(s, n, keep, P_MONTHLY, keep_monthly);
    apply_periodic(s, n, keep, P_YEARLY,  keep_yearly);

    /* Continuous snapshots are the fine-grained safety net between scheduled
       backups. A scheduled snapshot is already a complete image as of its time,
       so every continuous snapshot older than the most recent scheduled one is
       redundant -- drop it. Keep the tail captured since that anchor. If no
       scheduled snapshot exists, keep all continuous: they are the only backups
       and dropping them would lose data. */
    long long anchor = 0; int have_sched = 0;
    for (int i = 0; i < n; i++)
        if (s[i].kind == SK_SCHEDULED && (!have_sched || s[i].created > anchor)) {
            anchor = s[i].created; have_sched = 1;
        }
    for (int i = 0; i < n; i++) {
        if (s[i].kind != SK_CONTINUOUS) continue;
        if (!have_sched || s[i].created >= anchor) keep[i] = 1;
    }

    /* Never delete the single newest snapshot, whatever its kind: the most
       recent backup must always be restorable. (s is sorted newest-first.) */
    if (n > 0) keep[0] = 1;

    int n_keep = 0, n_prune = 0;
    for (int i = 0; i < n; i++) { if (keep[i]) n_keep++; else n_prune++; }

    log_info("prune: %d snapshot(s) -> keep %d, remove %d", n, n_keep, n_prune);
    for (int i = 0; i < n; i++) {
        time_t t = (time_t)s[i].created;
        char tb[32]; struct tm tm; localtime_r(&t, &tm);
        strftime(tb, sizeof tb, "%Y-%m-%d %H:%M", &tm);
        log_info("  [%s] %s snapshot %lld  %s", keep[i] ? "keep " : "prune",
                 s[i].kind == SK_CONTINUOUS ? "continuous" : "scheduled ",
                 s[i].id, tb);
    }

    if (n_prune == 0) { log_info("prune: nothing to remove"); free(keep); free(s); return 0; }

    /* Remove the pruned snapshot rows. */
    db_exec(c->db, "BEGIN");
    sqlite3_stmt *dsn = db_prep(c->db, "DELETE FROM snapshots WHERE id=?");
    for (int i = 0; i < n; i++) {
        if (keep[i]) continue;
        sqlite3_bind_int64(dsn, 1, s[i].id); sqlite3_step(dsn); sqlite3_reset(dsn);
    }
    sqlite3_finalize(dsn);

    /* Drop versions whose interval no longer overlaps any surviving snapshot:
       removing snapshots only orphans a version when its whole [first,last) span
       falls in the pruned gap. A version still spanning a kept snapshot stays. */
    db_exec(c->db,
        "DELETE FROM version_blobs WHERE version_id IN ("
        "  SELECT id FROM versions v WHERE NOT EXISTS ("
        "    SELECT 1 FROM snapshots s WHERE s.state=1 "
        "    AND s.id>=v.first_snapshot "
        "    AND (v.last_snapshot IS NULL OR s.id<v.last_snapshot)))");
    db_exec(c->db,
        "DELETE FROM versions WHERE NOT EXISTS ("
        "  SELECT 1 FROM snapshots s WHERE s.state=1 "
        "  AND s.id>=versions.first_snapshot "
        "  AND (versions.last_snapshot IS NULL OR s.id<versions.last_snapshot))");

    /* A surviving version whose first_snapshot was itself pruned must re-anchor
       to the oldest surviving snapshot still inside its interval, so the
       first_snapshot reference (joined for capture time / display) stays valid. */
    db_exec(c->db,
        "UPDATE versions SET first_snapshot=("
        "  SELECT MIN(s.id) FROM snapshots s WHERE s.state=1 "
        "  AND s.id>=versions.first_snapshot "
        "  AND (versions.last_snapshot IS NULL OR s.id<versions.last_snapshot)) "
        "WHERE first_snapshot NOT IN (SELECT id FROM snapshots)");

    int n_dirs = prune_empty_dirs(c->db);
    log_info("prune: %d empty director%s removed from the catalog",
             n_dirs, n_dirs == 1 ? "y" : "ies");

    /* Collect blobs no longer referenced by any version. */
    int bcap = 256, bn = 0;
    uint8_t (*hashes)[BK_HASH_LEN] = xmalloc((size_t)bcap * BK_HASH_LEN);
    long long freed_bytes = 0;
    sqlite3_stmt *ob = db_prep(c->db,
        "SELECT hash, stored_size FROM blobs WHERE hash NOT IN "
        "(SELECT hash FROM version_blobs)");
    while (sqlite3_step(ob) == SQLITE_ROW) {
        const void *h = sqlite3_column_blob(ob, 0);
        if (sqlite3_column_bytes(ob, 0) != BK_HASH_LEN) continue;
        if (bn == bcap) { bcap *= 2; hashes = xrealloc(hashes, (size_t)bcap * BK_HASH_LEN); }
        memcpy(hashes[bn++], h, BK_HASH_LEN);
        freed_bytes += sqlite3_column_int64(ob, 1);
    }
    sqlite3_finalize(ob);

    if (dry_run) {
        db_exec(c->db, "ROLLBACK");
        log_info("prune: DRY RUN -- would remove %d snapshot(s), %d empty "
                 "director%s, %d blob(s), ~%lld bytes from the repo",
                 n_prune, n_dirs, n_dirs == 1 ? "y" : "ies", bn, freed_bytes);
        free(hashes); free(keep); free(s);
        return 0;
    }

    db_exec(c->db, "COMMIT");

    /* Now delete the orphaned blob files from the server. A blob row is removed
       only after its remote file is gone, so a failed delete is retried by the
       next prune. */
    ctx_connect(c);
    key_from_meta(c);

    int removed = 0, failed = 0;
    /* Which of the 256 blobs/<2 hex> fan-out directories we emptied blobs from,
       so only those need an rmdir attempt afterwards. */
    unsigned char touched[256] = {0};
    sqlite3_stmt *db_del = db_prep(c->db, "DELETE FROM blobs WHERE hash=?");
    for (int i = 0; i < bn; i++) {
        char hex[BK_HEX_LEN + 1];
        hex_encode(hashes[i], BK_HASH_LEN, hex);
        char *remote = blob_repo_path(c, hex);
        if (transport_delete(c->t, remote) == 0) {
            sqlite3_bind_blob(db_del, 1, hashes[i], BK_HASH_LEN, SQLITE_STATIC);
            sqlite3_step(db_del); sqlite3_reset(db_del);
            touched[hashes[i][0]] = 1;
            removed++;
        } else {
            log_warn("prune: could not delete remote blob %s (will retry next prune)", hex);
            failed++;
        }
        free(remote);
    }
    sqlite3_finalize(db_del);

    /* Deleting the last blob out of a fan-out directory leaves an empty
       "blobs/<2 hex>" on the server. rmdir removes only empty directories, so
       attempting it on every directory we touched is safe: one that still holds
       blobs simply fails and is left alone. */
    int rmdirs = 0;
    for (int i = 0; i < 256; i++) {
        if (!touched[i]) continue;
        char sub[16];
        snprintf(sub, sizeof sub, "blobs/%02x", i);
        char *remote = repo_path(c, sub);
        if (transport_rmdir(c->t, remote) == 0) rmdirs++;
        free(remote);
    }

    /* Find the newest surviving snapshot id for the catalog filename. */
    long long latest = 0;
    sqlite3_stmt *mx = db_prep(c->db, "SELECT MAX(id) FROM snapshots");
    if (sqlite3_step(mx) == SQLITE_ROW) latest = sqlite3_column_int64(mx, 0);
    sqlite3_finalize(mx);

    upload_catalog(c, latest);

    log_info("prune: removed %d snapshot(s), %d empty catalog director%s, "
             "%d blob(s) (%lld bytes), %d empty repo director%s; "
             "%d blob delete(s) failed",
             n_prune, n_dirs, n_dirs == 1 ? "y" : "ies",
             removed, freed_bytes, rmdirs, rmdirs == 1 ? "y" : "ies", failed);

    free(hashes); free(keep); free(s);
    return 0;
}
