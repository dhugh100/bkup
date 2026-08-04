#ifndef BK_DB_H
#define BK_DB_H

#include <sqlite3.h>

/* Open (creating if needed) the catalog at `path`, set WAL + busy_timeout,
   and ensure the schema exists. die()s on failure. */
sqlite3 *db_open(const char *path);
void     db_close(sqlite3 *db);

/* Run SQL with no result rows; die() on error. */
void db_exec(sqlite3 *db, const char *sql);

/* meta key/value helpers. db_meta_get returns a malloc'd string or NULL. */
char *db_meta_get(sqlite3 *db, const char *key);
void  db_meta_set(sqlite3 *db, const char *key, const char *val);

/* Convenience: prepare or die. */
sqlite3_stmt *db_prep(sqlite3 *db, const char *sql);

/* Step a write statement (INSERT/UPDATE/DELETE) that must run to completion;
   die() if it does not reach SQLITE_DONE. Use wherever a silent step failure
   would corrupt the catalog or lose a change. Does not finalize -- the caller
   still owns the statement. `what` names the operation for the error message. */
void db_step_done(sqlite3 *db, sqlite3_stmt *st, const char *what);

#endif
