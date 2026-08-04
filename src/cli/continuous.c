#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>
#include <unistd.h>

#include "commands.h"
#include "common/db.h"
#include "common/types.h"
#include "common/log.h"

/* A continuous backup refreshes one subtree and produces a complete snapshot of
   the whole source by carrying every other file's existing version forward. The
   subtree must lie inside a configured source, otherwise the scan would create
   catalog rows that a later full backup's global sweep would delete -- and the
   continuous snapshot would reference versions for paths no source owns. */
static int root_in_sources(const User *u, const char *root)
{
    size_t rlen = strlen(root);
    for (int i = 0; i < u->nsources; i++) {
        const char *src = u->sources[i];
        size_t slen = strlen(src);
        if (rlen == slen && memcmp(root, src, slen) == 0)
            return 1;                       /* root is the source itself */
        if (rlen > slen && memcmp(root, src, slen) == 0 && root[slen] == '/')
            return 1;                       /* root is strictly under a source */
    }
    return 0;
}

/* Build a complete snapshot from a scoped scan of `root`. Assumes the Ctx is
   already opened, connected, and keyed (cmd_continuous does this for the CLI;
   the daemon watcher derives the key once and reuses it). Mirrors cmd_backup's
   snapshot machinery exactly, except the scan is scoped to `root` and the
   catalog is NOT uploaded -- the caller decides that. */
long long continuous_snapshot(Ctx *c, const char *root)
{
    if (!root_in_sources(c->src, root)) {
        log_warn("continuous: %s is not inside any configured source; ignoring", root);
        return -1;
    }

    /* A continuous snapshot is "complete" only because it carries versions
       forward from a prior full backup. Without one, it would capture only
       `root` and masquerade as a full image -- refuse rather than record a
       partial. */
    sqlite3_stmt *pc = db_prep(c->db,
        "SELECT COUNT(*) FROM snapshots WHERE state=1");
    sqlite3_step(pc);
    long long prior = sqlite3_column_int64(pc, 0);
    sqlite3_finalize(pc);
    if (prior == 0) {
        log_warn("continuous: no prior full backup exists; run `bkup backup` first");
        return -1;
    }

    /* recovery sweep: roll back any interrupted (OPEN) snapshot's interval edits,
       exactly as a full backup would. Uploaded blobs survive (shared,
       content-addressed). */
    db_exec(c->db,
        "UPDATE versions SET last_snapshot=NULL WHERE last_snapshot IN "
        "(SELECT id FROM snapshots WHERE state=0)");
    db_exec(c->db,
        "DELETE FROM version_blobs WHERE version_id IN "
        "(SELECT id FROM versions WHERE first_snapshot IN "
        " (SELECT id FROM snapshots WHERE state=0))");
    db_exec(c->db,
        "DELETE FROM versions WHERE first_snapshot IN "
        "(SELECT id FROM snapshots WHERE state=0)");
    db_exec(c->db, "DELETE FROM snapshots WHERE state=0");

    char host[256] = "";
    gethostname(host, sizeof host - 1);
    sqlite3_stmt *is = db_prep(c->db,
        "INSERT INTO snapshots(created,state,kind,hostname) VALUES(?,0,?,?)");
    sqlite3_bind_int64(is, 1, (long long)time(NULL));
    sqlite3_bind_int  (is, 2, SK_CONTINUOUS);
    sqlite3_bind_text (is, 3, host, -1, SQLITE_STATIC);
    sqlite3_step(is);
    sqlite3_finalize(is);
    long long snap = sqlite3_last_insert_rowid(c->db);

    log_info("continuous snapshot %lld: scanning %s", snap, root);
    scan_run_subtree(c, snap, root);

    /* The snapshot is a complete image with no carry-forward work: every file
       outside `root` is untouched, so its version's interval stays open
       (last_snapshot IS NULL) and is implicitly live in this snapshot. */

    /* materialize the dirty list before mutating rows */
    Buf ids; buf_init(&ids);
    sqlite3_stmt *dq = db_prep(c->db, "SELECT id FROM files WHERE state=1");
    while (sqlite3_step(dq) == SQLITE_ROW) {
        long long id = sqlite3_column_int64(dq, 0);
        buf_append(&ids, &id, sizeof id);
    }
    sqlite3_finalize(dq);

    Stats st; memset(&st, 0, sizeof st);
    size_t nids = ids.len / sizeof(long long);
    for (size_t i = 0; i < nids; i++) {
        long long id;
        memcpy(&id, ids.data + i * sizeof(long long), sizeof id);
        process_file(c, snap, id, &st);
    }
    buf_free(&ids);

    /* safety: every blob reachable from this snapshot must be UPLOADED. Only
       versions created here (first_snapshot=snap) can carry new blobs. */
    sqlite3_stmt *chk = db_prep(c->db,
        "SELECT COUNT(*) FROM versions v "
        "JOIN version_blobs vb ON vb.version_id=v.id "
        "JOIN blobs b ON b.hash=vb.hash "
        "WHERE v.first_snapshot=? AND b.state!=1");
    sqlite3_bind_int64(chk, 1, snap);
    sqlite3_step(chk);
    long long pending = sqlite3_column_int64(chk, 0);
    sqlite3_finalize(chk);
    if (pending != 0)
        die("internal error: %lld blobs not uploaded; continuous snapshot left OPEN",
            pending);

    sqlite3_stmt *fin = db_prep(c->db, "UPDATE snapshots SET state=1 WHERE id=?");
    sqlite3_bind_int64(fin, 1, snap);
    sqlite3_step(fin);
    sqlite3_finalize(fin);

    log_info("continuous snapshot %lld complete: %lld changed, %lld chunks "
             "(%lld new %lld dedup), %lld bytes raw -> %lld bytes stored",
             snap, st.files_changed, st.chunks_total, st.chunks_new,
             st.chunks_dedup, st.bytes_new, st.bytes_stored);
    return snap;
}

/* ---- the command: bkup continuous <path> ---- */

int cmd_continuous(Ctx *c, int argc, char **argv)
{
    if (argc < 1) {
        fprintf(stderr, "usage: bkup continuous <path>\n");
        return 2;
    }

    /* Canonicalize so the path matches the absolute paths the walker records
       (and so a trailing slash or symlink in the argument cannot dodge the
       in-source check). */
    char canon[PATH_MAX];
    if (!realpath(argv[0], canon)) {
        log_warn("continuous: cannot resolve %s", argv[0]);
        return 1;
    }

    user_check_ownership(c->src);
    ctx_open_db(c);
    ctx_connect(c);
    key_from_meta(c);
    check_repo_id(c);

    long long snap = continuous_snapshot(c, canon);
    if (snap < 0)
        return 1;

    /* A manual continuous backup is self-contained: push the catalog now. The
       daemon watcher instead defers the push and batches it (catalog_push). */
    upload_catalog(c, snap);
    return 0;
}
