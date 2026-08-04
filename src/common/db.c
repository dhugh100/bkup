#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "db.h"
#include "util.h"
#include "log.h"

static const char *SCHEMA =
"PRAGMA journal_mode=WAL;"
"PRAGMA synchronous=NORMAL;"
"PRAGMA foreign_keys=ON;"

"CREATE TABLE IF NOT EXISTS meta("
"  key TEXT PRIMARY KEY, value TEXT);"

"CREATE TABLE IF NOT EXISTS files("
"  id INTEGER PRIMARY KEY,"
"  path TEXT UNIQUE NOT NULL,"
"  kind INTEGER NOT NULL,"
"  size INTEGER, mtime_sec INTEGER, mtime_nsec INTEGER,"
"  ino INTEGER, dev INTEGER, mode INTEGER, uid INTEGER, gid INTEGER,"
"  state INTEGER NOT NULL DEFAULT 1,"   /* FS_DIRTY */
"  version_id INTEGER,"
"  seen INTEGER);"

/* A version is "live" over a half-open range of snapshots: from first_snapshot
   (inclusive, the snapshot that captured it) to last_snapshot (exclusive, the
   snapshot during which it was superseded or deleted); last_snapshot IS NULL
   means still current. Membership of a version in snapshot S is
   first_snapshot<=S AND (last_snapshot IS NULL OR last_snapshot>S). This
   replaces a per-(snapshot,version) membership table that grew as
   snapshot_count x file_count. */
"CREATE TABLE IF NOT EXISTS versions("
"  id INTEGER PRIMARY KEY,"
"  path TEXT NOT NULL, kind INTEGER NOT NULL,"
"  size INTEGER, mtime_sec INTEGER, mtime_nsec INTEGER,"
"  mode INTEGER, uid INTEGER, gid INTEGER,"
"  link_target TEXT,"
"  first_snapshot INTEGER, last_snapshot INTEGER);"

"CREATE TABLE IF NOT EXISTS version_blobs("
"  version_id INTEGER NOT NULL,"
"  seq INTEGER NOT NULL,"
"  hash BLOB NOT NULL,"
"  PRIMARY KEY(version_id, seq));"

"CREATE TABLE IF NOT EXISTS blobs("
"  hash BLOB PRIMARY KEY,"
"  size INTEGER NOT NULL,"
"  stored_size INTEGER,"
"  state INTEGER NOT NULL DEFAULT 0);"   /* BS_NEEDED */

"CREATE TABLE IF NOT EXISTS snapshots("
"  id INTEGER PRIMARY KEY,"
"  created INTEGER NOT NULL,"
"  state INTEGER NOT NULL DEFAULT 0,"    /* SS_OPEN */
"  kind INTEGER NOT NULL DEFAULT 0,"     /* SK_SCHEDULED */
"  hostname TEXT);"

"CREATE INDEX IF NOT EXISTS idx_files_state ON files(state);"
"CREATE INDEX IF NOT EXISTS idx_vb_version ON version_blobs(version_id);"
"CREATE INDEX IF NOT EXISTS idx_versions_path ON versions(path);"
/* membership queries filter versions by their snapshot interval; the table is
   now ~file_count rows (not snapshot_count x file_count), so these are light. */
"CREATE INDEX IF NOT EXISTS idx_versions_first ON versions(first_snapshot);"
"CREATE INDEX IF NOT EXISTS idx_versions_last ON versions(last_snapshot);";

/* Return 1 if `table` already has a column named `col`. */
static int db_has_column(sqlite3 *db, const char *table, const char *col)
{
    char sql[128];
    snprintf(sql, sizeof sql, "PRAGMA table_info(%s)", table);
    sqlite3_stmt *st = db_prep(db, sql);
    int found = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *name = sqlite3_column_text(st, 1);  /* col 1 = name */
        if (name && strcmp((const char *)name, col) == 0) { found = 1; break; }
    }
    sqlite3_finalize(st);
    return found;
}

/* Bring an existing catalog up to the current schema. CREATE TABLE IF NOT
   EXISTS never adds columns to a table that already exists, so new columns are
   added here. Idempotent: each step is guarded so re-running is a no-op. */
static void db_migrate(sqlite3 *db)
{
    /* snapshots.kind: pre-existing snapshots default to SK_SCHEDULED (0), which
       keeps prune conservative across an upgrade. */
    if (!db_has_column(db, "snapshots", "kind"))
        db_exec(db, "ALTER TABLE snapshots ADD COLUMN kind INTEGER NOT NULL DEFAULT 0");

    /* snapshot_versions (per-(snapshot,version) membership) -> validity intervals
       on `versions`. Membership of a version across snapshots is provably a
       single contiguous run [min(snapshot_id) .. max(snapshot_id)], so the
       half-open interval is first_snapshot = min, last_snapshot = the next
       committed snapshot AFTER max (exclusive) -- NULL when max is the latest
       committed snapshot, i.e. the version is still current. Runs once; guarded
       on the new column. */
    if (!db_has_column(db, "versions", "first_snapshot")) {
        db_exec(db, "ALTER TABLE versions ADD COLUMN first_snapshot INTEGER");
        db_exec(db, "ALTER TABLE versions ADD COLUMN last_snapshot INTEGER");

        /* one grouped scan of snapshot_versions (index-only via idx_sv_ver,
           which still exists here) + ~file_count row updates. */
        db_exec(db,
            "UPDATE versions AS v SET "
            "  first_snapshot = a.fs, "
            "  last_snapshot = (SELECT MIN(s.id) FROM snapshots s "
            "                   WHERE s.state=1 AND s.id > a.ls) "
            "FROM (SELECT version_id, MIN(snapshot_id) AS fs, "
            "             MAX(snapshot_id) AS ls "
            "      FROM snapshot_versions GROUP BY version_id) AS a "
            "WHERE a.version_id = v.id");

        db_exec(db, "CREATE INDEX IF NOT EXISTS idx_versions_first "
                    "ON versions(first_snapshot)");
        db_exec(db, "CREATE INDEX IF NOT EXISTS idx_versions_last "
                    "ON versions(last_snapshot)");

        /* the old membership table and its indexes go with the drop. */
        db_exec(db, "DROP TABLE snapshot_versions");

        /* DROP leaves the freed pages on the freelist; reclaim the (often large)
           space so the catalog file actually shrinks. One-shot, not in a txn. */
        db_exec(db, "VACUUM");
    }
}

sqlite3 *db_open(const char *path)
{
    /* ensure parent directory exists */
    const char *slash = strrchr(path, '/');
    if (slash) {
        char *dir = xstrdup(path);
        dir[slash - path] = '\0';
        if (dir[0]) mkdir_p(dir, 0700);
        free(dir);
    }

    sqlite3 *db;
    if (sqlite3_open(path, &db) != SQLITE_OK)
        die("cannot open catalog %s: %s", path, sqlite3_errmsg(db));
    sqlite3_busy_timeout(db, 10000);
    db_exec(db, SCHEMA);
    db_migrate(db);
    return db;
}

void db_close(sqlite3 *db)
{
    if (db) sqlite3_close(db);
}

void db_exec(sqlite3 *db, const char *sql)
{
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        char msg[512];
        snprintf(msg, sizeof msg, "%s", err ? err : "(unknown)");
        sqlite3_free(err);
        die("sql error: %s", msg);
    }
}

sqlite3_stmt *db_prep(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        die("prepare failed: %s (%s)", sqlite3_errmsg(db), sql);
    return st;
}

void db_step_done(sqlite3 *db, sqlite3_stmt *st, const char *what)
{
    if (sqlite3_step(st) != SQLITE_DONE)
        die("%s failed: %s", what, sqlite3_errmsg(db));
}

char *db_meta_get(sqlite3 *db, const char *key)
{
    sqlite3_stmt *st = db_prep(db, "SELECT value FROM meta WHERE key=?");
    sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
    char *val = NULL;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *v = sqlite3_column_text(st, 0);
        if (v) val = xstrdup((const char *)v);
    }
    sqlite3_finalize(st);
    return val;
}

void db_meta_set(sqlite3 *db, const char *key, const char *val)
{
    sqlite3_stmt *st = db_prep(db,
        "INSERT INTO meta(key,value) VALUES(?,?) "
        "ON CONFLICT(key) DO UPDATE SET value=excluded.value");
    sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, val, -1, SQLITE_STATIC);
    if (sqlite3_step(st) != SQLITE_DONE)
        die("meta_set failed: %s", sqlite3_errmsg(db));
    sqlite3_finalize(st);
}
