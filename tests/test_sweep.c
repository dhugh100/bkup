/* Exercises the real scoped deletion sweep in scan_run_subtree (src/cli/scan.c)
   against a temp catalog + temp directory tree. This is the dangerous line: a
   spot backup scanning subtree R must delete only catalog rows under R, never
   siblings -- including sibling paths that share R's name as a string prefix
   (/x/proj vs /x/proj2). No transport or key is needed; the scan touches only
   the local catalog and the filesystem.

   The catalog is opened via db_open() (real full schema) so that scan_run_subtree
   can write both the files table and the versions table without hitting
   "no such table: versions". */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <sqlite3.h>

#include "cli/ctx.h"
#include "cli/commands.h"
#include "common/config.h"
#include "common/db.h"

#include "test_common.h"

static int fails;

static void mkdirp(const char *p) { mkdir(p, 0755); }
static void touch(const char *p) { int fd = creat(p, 0644); if (fd >= 0) close(fd); }

static void insert_row(sqlite3 *db, const char *path, long long seen)
{
    sqlite3_stmt *st;
    sqlite3_prepare_v2(db,
        "INSERT INTO files(path,kind,size,mtime_sec,mtime_nsec,ino,dev,mode,"
        "uid,gid,state,seen) VALUES(?,0,0,0,0,0,0,0,0,0,0,?)",
        -1, &st, NULL);
    sqlite3_bind_text (st, 1, path, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, seen);
    sqlite3_step(st);
    sqlite3_finalize(st);
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

int main(void)
{
    char base[PATH_MAX];
    tmpdir(base, sizeof base);   /* honors $TMPDIR; runner wipes it per-iteration */

    char root[256], proj_a[256], proj_sub[256], proj_b[256], proj_gone[256];
    char sib_x[256], other_y[256], catalog[256];
    snprintf(root,      sizeof root,      "%s/proj", base);
    snprintf(proj_a,    sizeof proj_a,    "%s/proj/a.txt", base);
    snprintf(proj_sub,  sizeof proj_sub,  "%s/proj/sub", base);
    snprintf(proj_b,    sizeof proj_b,    "%s/proj/sub/b.txt", base);
    snprintf(proj_gone, sizeof proj_gone, "%s/proj/gone.txt", base);
    snprintf(sib_x,     sizeof sib_x,     "%s/proj2/x.txt", base); /* prefix trap */
    snprintf(other_y,   sizeof other_y,   "%s/other/y.txt", base);
    snprintf(catalog,   sizeof catalog,   "%s/catalog.db", base);

    /* on-disk tree under root: a.txt and sub/b.txt exist; gone.txt does not */
    mkdirp(root);
    touch(proj_a);
    mkdirp(proj_sub);
    touch(proj_b);

    /* Use db_open() so the catalog has the full real schema (files + versions +
       version_blobs + blobs + snapshots + meta).  The old hand-rolled CREATE
       TABLE only made 'files', causing scan_run_subtree to die with
       "no such table: versions" when it closes stale version intervals. */
    sqlite3 *db = db_open(catalog);

    /* prior catalog state: every row stale (seen=1); the scan uses seen=2 */
    insert_row(db, proj_a,    1);   /* under root, still on disk -> survives  */
    insert_row(db, proj_b,    1);   /* under root, still on disk -> survives  */
    insert_row(db, proj_gone, 1);   /* under root, vanished      -> swept     */
    insert_row(db, sib_x,     1);   /* sibling (prefix trap)     -> survives  */
    insert_row(db, other_y,   1);   /* unrelated sibling         -> survives  */

    /* minimal Ctx/User wired to the temp catalog and the one source root */
    char *sources[1] = { root };
    User u; memset(&u, 0, sizeof u);
    u.name = "test";
    u.db = catalog;
    u.sources = sources;
    u.nsources = 1;
    u.nexcludes = 0;

    Ctx c; memset(&c, 0, sizeof c);
    c.src = &u;
    c.db = db;

    scan_run_subtree(&c, 2, root);

    CHECK(row_exists(db, proj_a));      /* refreshed, survives */
    CHECK(row_exists(db, proj_b));      /* refreshed, survives */
    CHECK(!row_exists(db, proj_gone));  /* vanished under root -> swept */
    CHECK(row_exists(db, sib_x));       /* PREFIX TRAP: must NOT be swept */
    CHECK(row_exists(db, other_y));     /* unrelated sibling -> untouched */

    sqlite3_close(db);

    /* best-effort cleanup of everything under base */
    rmtree_local(base);

    TEST_DONE("test_sweep");
}
