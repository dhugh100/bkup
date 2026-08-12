#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <pwd.h>
#include <time.h>
#include <errno.h>
#include <sys/socket.h>

#include "ipc.h"
#include "userstate.h"
#include "cli/commands.h"
#include "cli/ctx.h"
#include "common/db.h"
#include "common/types.h"
#include "common/log.h"

/* ---- one-at-a-time operation mutex ---- */

static pthread_mutex_t g_op_mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_busy = 0;
static char g_busy_cmd[64] = "";
/* How long an interactive command waits for the op mutex before reporting busy.
   Routine background work (a continuous backup, or the catalog-push that
   follows it) holds the lock for seconds, so a restore/list should queue behind
   it rather than fail outright; the bound just guards against a wedged holder.
   This is only the default: a caller sets its own bound with a "wait" field
   (0 = fail immediately), because a person at a terminal and a cron job want
   opposite things here. Another user's *full* backup can hold the lock for far
   longer than the routine work this default was chosen against. */
#define OP_WAIT_SEC 120
/* Ceiling on a caller-supplied wait. Each waiter parks a thread and an fd for
   the duration (main.c spawns one detached thread per connection, unbounded),
   so a client must not be able to ask to wait forever on a wedged holder. */
#define OP_WAIT_MAX 3600
/* Per-thread: does *this* thread currently hold g_op_mutex?  Lets a command
   release the mutex early -- before the slow Ctx teardown (which closes the
   SSH/SFTP session) and before the terminal "done" -- while keeping the
   die()-path cleanup handler from double-unlocking. */
static __thread int g_op_held = 0;

static void release_op_mutex(void *arg)
{
    (void)arg;
    if (!g_op_held) return;          /* idempotent: safe to call more than once */
    g_op_held = 0;
    g_busy = 0;
    g_busy_cmd[0] = '\0';
    pthread_mutex_unlock(&g_op_mutex);
}

void ipc_op_lock(const char *cmd)
{
    pthread_mutex_lock(&g_op_mutex);
    g_op_held = 1;
    g_busy = 1;
    snprintf(g_busy_cmd, sizeof g_busy_cmd, "%s", cmd ? cmd : "");
}

void ipc_op_unlock(void)
{
    release_op_mutex(NULL);
}

/* Resources a serialized-op connection owns past the point where die() may
   unwind the thread via conn_die()/pthread_exit(). Registered with
   pthread_cleanup_push so a fatal op still releases the op mutex AND frees the
   Ctx (closing its SSH/SFTP session) and the cmd string, instead of leaking
   them for the daemon's lifetime. release_op_mutex is idempotent, so an early
   op_finish() release on the normal path is harmless.

   config_path is held by address, not by value: the eight early `goto out`
   paths above the cleanup handler still have to free it themselves, so this
   frees it and NULLs the caller's pointer, leaving the out: label's free() a
   no-op rather than a double free. */
typedef struct {
    Ctx   *c;
    char  *cmd;
    char **config_path;
} OpCleanup;

static void op_cleanup(void *arg)
{
    OpCleanup *oc = arg;
    release_op_mutex(NULL);
    if (oc->c) ctx_free(oc->c);
    free(oc->cmd);
    if (oc->config_path) { free(*oc->config_path); *oc->config_path = NULL; }
    oc->c = NULL;
    oc->cmd = NULL;
}

/* ---- per-thread connection state (for log callback and die handler) ---- */

typedef struct { int fd; } ConnState;
static _Thread_local ConnState *tls_cs = NULL;

static void conn_log_cb(const char *lvl, const char *msg, void *ud)
{
    ConnState *cs = ud;
    if (cs->fd < 0) return;
    char emsg[4096];
    ipc_json_escape(msg, emsg, sizeof emsg);
    char buf[4352];
    snprintf(buf, sizeof buf,
             "{\"event\":\"log\",\"level\":\"%s\",\"msg\":\"%s\"}", lvl, emsg);
    ipc_send(cs->fd, buf);
}

/* Called by die() in place of exit(1).  Closes the socket and exits the
   connection thread; the pthread_cleanup_push handler then releases the
   op mutex. */
static void conn_die(void)
{
    if (tls_cs && tls_cs->fd >= 0) {
        close(tls_cs->fd);
        tls_cs->fd = -1;
    }
    pthread_exit(NULL);
}

/* Is the caller still on the other end?  The protocol is one request per
   connection, so a peek should find nothing pending: EAGAIN is the healthy
   answer and a zero-length read means the client closed the socket. Pending
   data would be unexpected, but it still proves the peer is there. */
static int conn_alive(int fd)
{
    char b;
    ssize_t r = recv(fd, &b, 1, MSG_PEEK | MSG_DONTWAIT);
    if (r > 0)  return 1;
    if (r == 0) return 0;
    return errno == EAGAIN || errno == EWOULDBLOCK;
}

/* ---- JSON helpers ---- */

/* The scalar getters, the escaper and the line I/O live in common/ipcwire.c so
   the CLI client shares them; only this array reader is daemon-side, because
   only requests carry arrays. */



/* Extract a JSON array of strings into out[] (each strdup'd; the caller frees up
   to the returned count). Returns how many were stored (<= max). A missing key
   or a non-array value yields 0. Unescaping matches ipc_get_str. */
static int ipc_get_str_array(const char *json, const char *key,
                             char **out, int max)
{
    char needle[128];
    snprintf(needle, sizeof needle, "\"%s\":", key);
    const char *p = strstr(json, needle);
    if (!p) return 0;
    p += strlen(needle);
    while (*p == ' ') p++;
    if (*p != '[') return 0;
    p++;
    int n = 0;
    while (*p && *p != ']' && n < max) {
        while (*p == ' ' || *p == ',') p++;
        if (*p != '"') break;          /* end of array or malformed */
        p++;
        char buf[4096];
        size_t b = 0;
        while (*p && b < sizeof buf - 1) {
            if (*p == '\\' && p[1] == '"') { buf[b++] = '"'; p += 2; }
            else if (*p == '\\' && p[1] == '\\') { buf[b++] = '\\'; p += 2; }
            else if (*p == '"') break;
            else buf[b++] = *p++;
        }
        buf[b] = '\0';
        if (*p == '"') p++;
        out[n++] = strdup(buf);
    }
    return n;
}

/* ---- command handlers ---- */

static void send_done(int fd)
{
    ipc_send(fd, "{\"event\":\"done\"}");
}

/* Run a query whose first column of its first row is an integer; return that
   value, or `def` if there is no row. */
static long long db_scalar(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *st = db_prep(db, sql);
    long long v = 0;
    if (sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return v;
}

/* Terminal "done" for a serialized op.  Releases the op mutex *before* telling
   the client, so a follow-up request (e.g. the GUI refreshing snapshots right
   after a backup/restore) never races this thread's slow teardown and gets a
   spurious "busy".  The trailing pthread_cleanup_pop then releases nothing. */
static void op_finish(int fd)
{
    release_op_mutex(NULL);
    send_done(fd);
}

static void handle_status(int fd)
{
    char buf[128];
    pthread_mutex_lock(&g_op_mutex);
    int busy = g_busy;
    char cmd[64];
    snprintf(cmd, sizeof cmd, "%s", g_busy_cmd);
    pthread_mutex_unlock(&g_op_mutex);

    if (busy)
        snprintf(buf, sizeof buf,
                 "{\"event\":\"status\",\"state\":\"busy\",\"cmd\":\"%s\"}", cmd);
    else
        snprintf(buf, sizeof buf, "{\"event\":\"status\",\"state\":\"idle\"}");
    ipc_send(fd, buf);
}

static void handle_list_snapshots(int fd, sqlite3 *db)
{
    sqlite3_stmt *q = db_prep(db,
        "SELECT s.id, s.created, s.state, s.hostname, "
        "       (SELECT COUNT(*) FROM versions v "
        "        WHERE v.first_snapshot<=s.id "
        "        AND (v.last_snapshot IS NULL OR v.last_snapshot>s.id)) "
        "FROM snapshots s ORDER BY s.id");

    while (sqlite3_step(q) == SQLITE_ROW) {
        long long id    = sqlite3_column_int64(q, 0);
        long long ts    = sqlite3_column_int64(q, 1);
        int state       = sqlite3_column_int(q, 2);
        const char *hn  = (const char *)sqlite3_column_text(q, 3);
        long long nf    = sqlite3_column_int64(q, 4);
        char ehn[256];
        ipc_json_escape(hn ? hn : "", ehn, sizeof ehn);
        char buf[512];
        snprintf(buf, sizeof buf,
                 "{\"event\":\"snapshot\",\"id\":%lld,\"time\":%lld,"
                 "\"state\":%d,\"files\":%lld,\"hostname\":\"%s\"}",
                 id, ts, state, nf, ehn);
        ipc_send(fd, buf);
    }
    sqlite3_finalize(q);
    send_done(fd);
}

/* Substring search over the latest complete snapshot's paths. */
static void handle_search(int fd, sqlite3 *db, const char *q)
{
    if (!q || !q[0]) { send_done(fd); return; }

    /* Find latest complete snapshot */
    sqlite3_stmt *sq = db_prep(db, "SELECT MAX(id) FROM snapshots WHERE state=1");
    long long snap = -1;
    if (sqlite3_step(sq) == SQLITE_ROW &&
        sqlite3_column_type(sq, 0) != SQLITE_NULL)
        snap = sqlite3_column_int64(sq, 0);
    sqlite3_finalize(sq);
    if (snap < 0) { send_done(fd); return; }

    char pattern[4096];
    snprintf(pattern, sizeof pattern, "%%%s%%", q);

    sqlite3_stmt *r = db_prep(db,
        "SELECT v.path, v.size, v.mtime_sec FROM versions v "
        "WHERE v.first_snapshot<=?1 "
        "AND (v.last_snapshot IS NULL OR v.last_snapshot>?1) "
        "AND v.path LIKE ?2 "
        "ORDER BY v.path LIMIT 500");
    sqlite3_bind_int64(r, 1, snap);
    sqlite3_bind_text(r, 2, pattern, -1, SQLITE_STATIC);

    while (sqlite3_step(r) == SQLITE_ROW) {
        const char *path = (const char *)sqlite3_column_text(r, 0);
        long long size   = sqlite3_column_int64(r, 1);
        long long mtime  = sqlite3_column_int64(r, 2);
        char epath[4096];
        ipc_json_escape(path ? path : "", epath, sizeof epath);
        char buf[4352];
        snprintf(buf, sizeof buf,
                 "{\"event\":\"result\",\"path\":\"%s\","
                 "\"size\":%lld,\"mtime\":%lld,\"snapshot\":%lld}",
                 epath, size, mtime, snap);
        ipc_send(fd, buf);
    }
    sqlite3_finalize(r);
    send_done(fd);
}

static void emit_entry(int fd, const char *name, const char *path,
                       int kind, long long size, long long mtime, int deleted)
{
    char ename[1024], epath[4096], buf[5632];
    ipc_json_escape(name, ename, sizeof ename);
    ipc_json_escape(path, epath, sizeof epath);
    snprintf(buf, sizeof buf,
             "{\"event\":\"entry\",\"name\":\"%s\",\"path\":\"%s\","
             "\"kind\":%d,\"size\":%lld,\"mtime\":%lld,\"deleted\":%d}",
             ename, epath, kind, size, mtime, deleted);
    ipc_send(fd, buf);
}

/* Escape a string for use as a LIKE pattern with ESCAPE '\'. */
static void like_escape(const char *src, char *dst, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; src[i] && o + 2 < cap; i++) {
        char ch = src[i];
        if (ch == '%' || ch == '_' || ch == '\\') dst[o++] = '\\';
        dst[o++] = ch;
    }
    dst[o] = '\0';
}

/* List the immediate children of `path` across ALL complete snapshots, so an
   item deleted in a later backup still shows up (and is still recoverable from
   an earlier version). An entry is flagged `deleted` when its newest version
   predates the latest snapshot, i.e. it is no longer in the current tree. With
   an empty path, list the source's configured sources as top-level dirs. */
static void handle_list_dir(int fd, sqlite3 *db, User *src, const char *path)
{
    if (!path || !path[0]) {
        for (int i = 0; i < src->nsources; i++) {
            const char *r = src->sources[i];
            const char *base = strrchr(r, '/');
            base = base ? base + 1 : r;
            emit_entry(fd, base[0] ? base : r, r, FK_DIR, 0, 0, 0);
        }
        send_done(fd);
        return;
    }

    char esc[8192], pattern[8200];
    like_escape(path, esc, sizeof esc);
    snprintf(pattern, sizeof pattern, "%s/%%", esc);

    /* instr() on the part after "path/" is 0 only for immediate children. One
       row per distinct child path: its newest version (greatest first_snapshot).
       That version's interval is still open (last_snapshot IS NULL) iff the path
       is in the current tree; a closed interval means it was deleted before the
       latest backup but is still recoverable from history. */
    sqlite3_stmt *q = db_prep(db,
        "SELECT v.path, v.kind, v.size, v.mtime_sec, v.last_snapshot "
        "FROM versions v "
        "WHERE v.path LIKE ?1 ESCAPE '\\' "
        "AND instr(substr(v.path, ?2), '/')=0 "
        "AND v.first_snapshot=(SELECT MAX(v2.first_snapshot) FROM versions v2 "
        "                      WHERE v2.path=v.path) "
        "ORDER BY (v.kind=1) DESC, v.path");   /* directories first */
    sqlite3_bind_text(q, 1, pattern, -1, SQLITE_STATIC);
    sqlite3_bind_int(q, 2, (int)strlen(path) + 2);   /* 1-based, past "path/" */

    while (sqlite3_step(q) == SQLITE_ROW) {
        const char *p = (const char *)sqlite3_column_text(q, 0);
        int kind       = sqlite3_column_int(q, 1);
        long long size = sqlite3_column_int64(q, 2);
        long long mt   = sqlite3_column_int64(q, 3);
        int deleted    = sqlite3_column_type(q, 4) != SQLITE_NULL;
        const char *base = strrchr(p, '/');
        base = base ? base + 1 : p;
        emit_entry(fd, base, p, kind, size, mt, deleted);
    }
    sqlite3_finalize(q);
    send_done(fd);
}

/* List every complete snapshot that contains `path`, newest first. */
static void handle_file_versions(int fd, sqlite3 *db, const char *path)
{
    if (!path || !path[0]) { send_done(fd); return; }
    /* Collapse carried-forward duplicates: one row per distinct version of the
       path, dated by when that version was FIRST captured -- not by each
       snapshot that merely carried it forward. A file unchanged across hundreds
       of continuous snapshots yields a single row at its true capture time. The
       restore handle is the newest snapshot still holding that version. */
    sqlite3_stmt *q = db_prep(db,
        "SELECT CASE WHEN v.last_snapshot IS NULL "
        "            THEN (SELECT MAX(id) FROM snapshots WHERE state=1) "
        "            ELSE (SELECT MAX(id) FROM snapshots "
        "                  WHERE state=1 AND id<v.last_snapshot) END, "
        "       sf.created, v.size, v.kind "
        "FROM versions v JOIN snapshots sf ON sf.id=v.first_snapshot "
        "WHERE v.path=? AND sf.state=1 "
        "ORDER BY sf.created DESC");
    sqlite3_bind_text(q, 1, path, -1, SQLITE_STATIC);
    while (sqlite3_step(q) == SQLITE_ROW) {
        long long id   = sqlite3_column_int64(q, 0);
        long long ts   = sqlite3_column_int64(q, 1);
        long long size = sqlite3_column_int64(q, 2);
        int kind       = sqlite3_column_int(q, 3);
        char buf[256];
        snprintf(buf, sizeof buf,
                 "{\"event\":\"version\",\"snapshot\":%lld,\"time\":%lld,"
                 "\"size\":%lld,\"kind\":%d}", id, ts, size, kind);
        ipc_send(fd, buf);
    }
    sqlite3_finalize(q);
    send_done(fd);
}

/* Stream every distinct path across all complete snapshots, one row per path
   with its newest-version metadata, flagged `deleted` when that newest version
   predates the latest complete snapshot (i.e. gone from the current tree but
   still recoverable). Backs the GUI flat "Build List" recovery view. */
/* Turn a plain substring into a LIKE pattern that matches it literally: wrap in
   %...% and backslash-escape LIKE's own metacharacters (% _ \) so a path that
   happens to contain them is matched as text, not as a wildcard. Used with
   "LIKE ? ESCAPE '\'". Returns 0 on success, -1 if the escaped pattern would
   not fit in `out`. */
static int like_contains_pattern(const char *term, char *out, size_t cap)
{
    size_t j = 0;
    if (j + 1 >= cap) return -1;
    out[j++] = '%';
    for (const char *p = term; *p; p++) {
        if (*p == '\\' || *p == '%' || *p == '_') {
            if (j + 1 >= cap) return -1;
            out[j++] = '\\';
        }
        if (j + 1 >= cap) return -1;
        out[j++] = *p;
    }
    if (j + 1 >= cap) return -1;
    out[j++] = '%';
    out[j] = '\0';
    return 0;
}

/* `filter`, when non-empty, is a case-insensitive substring matched against the
   full path -- the same semantics as the GUI's live filter, applied here so a
   scoped Build List only materializes matching rows.

   `asof`, when > 0, scopes the list to snapshots taken at or before that epoch:
   fewer snapshots to scan (a faster build) and a point-in-time view (paths added
   after the cutoff vanish; the newest version at or before it is shown). asof==0
   means "now" -- every committed snapshot. The value is a server-parsed integer,
   so it is embedded directly; the substring still goes through a bound LIKE. */
static void handle_list_all(int fd, sqlite3 *db, const char *filter, long long asof)
{
    int has_filter = filter && filter[0];
    char pattern[4096];
    if (has_filter && like_contains_pattern(filter, pattern, sizeof pattern) != 0) {
        ipc_send(fd, "{\"event\":\"error\",\"msg\":\"filter too long\"}");
        return;
    }

    /* A = the cutoff snapshot: the newest committed snapshot at or before `asof`
       (or simply the newest committed snapshot when asof==0). Everything is
       evaluated as of A. */
    char asof_bare[48] = "";
    if (asof > 0)
        snprintf(asof_bare, sizeof asof_bare, " AND created<=%lld", asof);

    long long A = 0;
    char lq[128];
    snprintf(lq, sizeof lq,
             "SELECT MAX(id) FROM snapshots WHERE state=1%s", asof_bare);
    sqlite3_stmt *ls = db_prep(db, lq);
    if (sqlite3_step(ls) == SQLITE_ROW)
        A = sqlite3_column_int64(ls, 0);
    sqlite3_finalize(ls);
    if (A <= 0) { send_done(fd); return; }   /* no snapshot at/before the cutoff */

    /* Per path: its newest version visible at A (greatest first_snapshot that is
       <=A). That single row carries the metadata; the path is "deleted" when
       that version's interval has already closed at or before A (gone from the
       cutoff tree, still recoverable from history). The versions table is now
       ~file_count rows, so this is a light index-assisted scan -- no traversal
       of the former snapshot_count x file_count membership table.

       Filtered restricts to matching paths first via idx_versions_path. */
    sqlite3_stmt *q;
    char sql[1024];
    if (!has_filter) {
        snprintf(sql, sizeof sql,
            "SELECT v.path, v.kind, v.size, v.last_snapshot FROM versions v "
            "WHERE v.first_snapshot=(SELECT MAX(v2.first_snapshot) FROM versions v2 "
            "  WHERE v2.path=v.path AND v2.first_snapshot<=%lld) "
            "ORDER BY v.path", A);
        q = db_prep(db, sql);
    } else {
        snprintf(sql, sizeof sql,
            "SELECT v.path, v.kind, v.size, v.last_snapshot FROM versions v "
            "WHERE v.path LIKE ?1 ESCAPE '\\' "
            "AND v.first_snapshot=(SELECT MAX(v2.first_snapshot) FROM versions v2 "
            "  WHERE v2.path=v.path AND v2.first_snapshot<=%lld) "
            "ORDER BY v.path", A);
        q = db_prep(db, sql);
        sqlite3_bind_text(q, 1, pattern, -1, SQLITE_STATIC);
    }
    while (sqlite3_step(q) == SQLITE_ROW) {
        const char *p  = (const char *)sqlite3_column_text(q, 0);
        int kind       = sqlite3_column_int(q, 1);
        long long size = sqlite3_column_int64(q, 2);
        int last_null  = sqlite3_column_type(q, 3) == SQLITE_NULL;
        long long last = sqlite3_column_int64(q, 3);
        /* deleted as of A: the interval ended at or before A. */
        int deleted = !last_null && last <= A;
        const char *base = strrchr(p, '/');
        base = base ? base + 1 : p;
        emit_entry(fd, base, p, kind, size, 0, deleted);
    }
    sqlite3_finalize(q);
    send_done(fd);
}

/* ---- connection thread ---- */

void *ipc_conn_thread(void *arg)
{
    ConnArg *ca = arg;
    int fd = ca->fd;
    char *config_path = ca->config_path;
    uid_t caller_uid = ca->caller_uid;
    free(ca);

    ConnState cs = { .fd = fd };
    tls_cs = &cs;
    log_set_thread_cb(conn_log_cb, &cs);
    log_set_thread_die(conn_die);

    char line[4096];
    if (ipc_readline(fd, line, sizeof line) != 0)
        goto out;

    char *cmd = ipc_get_str(line, "cmd");
    if (!cmd) {
        ipc_send(fd, "{\"event\":\"error\",\"msg\":\"missing cmd field\"}");
        goto out;
    }

    /* status is non-blocking -- no op mutex needed */
    if (strcmp(cmd, "status") == 0) {
        handle_status(fd);
        free(cmd);
        goto out;
    }

    /* list-sources is non-blocking: it only reads the config. */
    if (strcmp(cmd, "list-sources") == 0) {
        Ctx *c = ctx_new_uid(config_path, caller_uid);
        User *s = c->src;          /* only this user's own source */
        char ename[256], buf[512];
        ipc_json_escape(s->name, ename, sizeof ename);
        snprintf(buf, sizeof buf,
                 "{\"event\":\"source\",\"name\":\"%s\",\"scope\":%d}",
                 ename, s->scope);
        ipc_send(fd, buf);
        send_done(fd);
        ctx_free(c);
        free(cmd);
        goto out;
    }

    /* source-info is non-blocking: read-only config detail for one source. */
    if (strcmp(cmd, "source-info") == 0) {
        Ctx *c = ctx_new_uid(config_path, caller_uid);
        free(ipc_get_str(line, "source"));   /* ignored: own source only */
        User *s = c->src;
        char eserver[512], erepo[4096], edb[4096], ebk[256], epr[256], buf[10240];
        ipc_json_escape(c->cfg->server ? c->cfg->server : "", eserver, sizeof eserver);
        ipc_json_escape(s->repo ? s->repo : "", erepo, sizeof erepo);
        ipc_json_escape(s->db ? s->db : "", edb, sizeof edb);
        ipc_json_escape(s->backup_sched ? s->backup_sched : "", ebk, sizeof ebk);
        ipc_json_escape(s->prune_sched ? s->prune_sched : "", epr, sizeof epr);
        snprintf(buf, sizeof buf,
                 "{\"event\":\"info\",\"server\":\"%s\",\"repo\":\"%s\","
                 "\"db\":\"%s\",\"scope\":%d,\"continuous\":%d,"
                 "\"backup\":\"%s\",\"prune\":\"%s\","
                 "\"keep_last\":%d,\"keep_daily\":%d,\"keep_weekly\":%d,"
                 "\"keep_monthly\":%d,\"keep_yearly\":%d}",
                 eserver, erepo, edb, s->scope, s->continuous, ebk, epr,
                 s->keep_last, s->keep_daily, s->keep_weekly,
                 s->keep_monthly, s->keep_yearly);
        ipc_send(fd, buf);
        for (int i = 0; i < s->nsources; i++) {
            char er[4096], rb[4200];
            ipc_json_escape(s->sources[i], er, sizeof er);
            snprintf(rb, sizeof rb, "{\"event\":\"root\",\"path\":\"%s\"}", er);
            ipc_send(fd, rb);
        }
        send_done(fd);
        ctx_free(c);
        free(cmd);
        goto out;
    }

    /* repo-stats is non-blocking: read-only catalog aggregates for the status
       pane. WAL lets this reader run alongside an in-progress op. */
    if (strcmp(cmd, "repo-stats") == 0) {
        Ctx *c = ctx_new_uid(config_path, caller_uid);
        free(ipc_get_str(line, "source"));   /* ignored: own source only */
        ctx_open_db(c);
        long long sched = db_scalar(c->db,
            "SELECT COUNT(*) FROM snapshots WHERE state=1 AND kind=0");
        long long cont = db_scalar(c->db,
            "SELECT COUNT(*) FROM snapshots WHERE state=1 AND kind=1");
        long long last_cont = db_scalar(c->db,
            "SELECT COALESCE(MAX(created),0) FROM snapshots WHERE state=1 AND kind=1");
        long long stored = db_scalar(c->db,
            "SELECT COALESCE(SUM(stored_size),0) FROM blobs");
        long long logical = db_scalar(c->db,
            "SELECT COALESCE(SUM(b.size),0) FROM version_blobs vb "
            "JOIN blobs b ON b.hash=vb.hash");
        char buf[512];
        snprintf(buf, sizeof buf,
                 "{\"event\":\"stats\",\"scheduled\":%lld,\"continuous\":%lld,"
                 "\"last_continuous\":%lld,\"stored_bytes\":%lld,"
                 "\"logical_bytes\":%lld}",
                 sched, cont, last_cont, stored, logical);
        ipc_send(fd, buf);
        send_done(fd);
        ctx_free(c);
        free(cmd);
        goto out;
    }

    /* All other commands serialize through the op mutex. Try once without
       blocking: on the uncontended path that is the whole story, and only when
       it fails is there a wait to bound or to report. The holder's name is read
       unlocked (as handle_status does) and escaped before it goes out -- it is
       whatever command string some other client sent, so it is not trusted to
       be JSON-safe. */
    long long wait_sec = ipc_get_int(line, "wait", OP_WAIT_SEC);
    if (wait_sec < 0) wait_sec = 0;
    if (wait_sec > OP_WAIT_MAX) wait_sec = OP_WAIT_MAX;

    int waited = 0;
    if (pthread_mutex_trylock(&g_op_mutex) != 0) {
        char holder[64], eholder[192], buf[320];
        snprintf(holder, sizeof holder, "%s", g_busy_cmd);
        ipc_json_escape(holder, eholder, sizeof eholder);

        if (wait_sec > 0) {
            /* Say so before blocking. Without this the caller just stops for up
               to wait_sec with nothing on screen, which reads as a hang. */
            snprintf(buf, sizeof buf,
                     "{\"event\":\"waiting\",\"cmd\":\"%s\",\"wait\":%lld}",
                     eholder, wait_sec);
            ipc_send(fd, buf);

            struct timespec deadline;
            clock_gettime(CLOCK_REALTIME, &deadline);
            deadline.tv_sec += (time_t)wait_sec;
            waited = pthread_mutex_timedlock(&g_op_mutex, &deadline) == 0;
        }
        if (!waited) {
            snprintf(buf, sizeof buf,
                     "{\"event\":\"error\",\"msg\":\"busy: %s in progress\"}",
                     eholder);
            ipc_send(fd, buf);
            free(cmd);
            goto out;
        }
    }
    g_op_held = 1;
    g_busy = 1;
    snprintf(g_busy_cmd, sizeof g_busy_cmd, "%s", cmd);

    /* A caller that gave up during the wait (Ctrl-C, closed window) would
       otherwise have its request run in full on acquiring the lock -- a restore
       writing files nobody is waiting for, holding the mutex against everyone
       else. Only worth checking after an actual wait: without one, no time has
       passed in which to disconnect. */
    if (waited && !conn_alive(fd)) {
        log_info("ipc: caller disconnected while waiting; '%s' not run", cmd);
        release_op_mutex(NULL);
        free(cmd);
        goto out;
    }

    /* If die() fires inside a cmd_*, conn_die() closes the fd and calls
       pthread_exit(), which runs op_cleanup: it releases the mutex and frees the
       Ctx + request strings before the thread terminates, so a fatal op leaks
       neither its SSH session nor cmd/config_path. */
    OpCleanup oc = { .c = NULL, .cmd = cmd, .config_path = &config_path };
    pthread_cleanup_push(op_cleanup, &oc);

    Ctx *c = ctx_new_uid(config_path, caller_uid);
    oc.c = c;
    /* The daemon is bound to its own user's source (ctx_new picked it from the
       running uid). Ignore any client-supplied "source" so a caller can never
       operate another user's backups. */
    free(ipc_get_str(line, "source"));

    /* Bracket the state-changing ops in the durable log.  Read-only queries
       are omitted as noise.  On failure the op die()s out via conn_die before
       the completion line, and the fatal entry is the record. */
    int logged_op = !strcmp(cmd, "backup") || !strcmp(cmd, "restore") ||
                    !strcmp(cmd, "verify") || !strcmp(cmd, "prune");
    if (logged_op)
        log_info("ipc: %s '%s' started (uid=%d)", cmd, c->src->name,
                 (int)caller_uid);

    if (strcmp(cmd, "list-snapshots") == 0) {
        ctx_open_db(c);
        handle_list_snapshots(fd, c->db);
    } else if (strcmp(cmd, "search") == 0) {
        char *q = ipc_get_str(line, "q");
        ctx_open_db(c);
        handle_search(fd, c->db, q);
        free(q);
    } else if (strcmp(cmd, "list-dir") == 0) {
        char *path = ipc_get_str(line, "path");
        ctx_open_db(c);
        handle_list_dir(fd, c->db, c->src, path);
        free(path);
    } else if (strcmp(cmd, "file-versions") == 0) {
        char *path = ipc_get_str(line, "path");
        ctx_open_db(c);
        handle_file_versions(fd, c->db, path);
        free(path);
    } else if (strcmp(cmd, "list-all") == 0) {
        char *filter = ipc_get_str(line, "filter");
        long long asof = ipc_get_int(line, "asof", 0);
        ctx_open_db(c);
        handle_list_all(fd, c->db, filter, asof);
        free(filter);
    } else if (strcmp(cmd, "backup") == 0) {
        /* Auto-init if the catalog has no KDF metadata yet. */
        ctx_open_db(c);
        char *salt = db_meta_get(c->db, "kdf_salt");
        if (!salt) {
            log_info("catalog uninitialized -- running init");
            cmd_init(c, 0, NULL);
        } else {
            free(salt);
        }
        cmd_backup(c, 0, NULL);
        op_finish(fd);
    } else if (strcmp(cmd, "restore") == 0) {
        char *dest = ipc_get_str(line, "dest");
        if (!dest) {
            ipc_send(fd, "{\"event\":\"error\",\"msg\":\"restore requires dest\"}");
            logged_op = 0;   /* nothing ran; do not log a completion */
        } else {
            /* Multi-select: the GUI sends arrays of directory and file targets,
               restored in one pass under one as-of date. The single prefix/file
               form is still accepted for back-compat. */
            #define RESTORE_MAX 4096
            char *dirs[RESTORE_MAX], *files[RESTORE_MAX];
            int nd = ipc_get_str_array(line, "dirs", dirs, RESTORE_MAX);
            int nf = ipc_get_str_array(line, "files", files, RESTORE_MAX);
            char *prefix = ipc_get_str(line, "prefix");
            char *file   = ipc_get_str(line, "file");
            if (prefix) { if (nd < RESTORE_MAX) dirs[nd++] = prefix; else free(prefix); }
            if (file)   { if (nf < RESTORE_MAX) files[nf++] = file; else free(file); }

            /* root daemon: hand restored files back to the calling user. */
            struct passwd *pw = getpwuid(caller_uid);
            RestoreReq r;
            memset(&r, 0, sizeof r);
            r.dest      = dest;
            r.snap      = ipc_get_int(line, "snapshot", -1);
            r.asof      = ipc_get_int(line, "asof", 0);
            r.owner_uid = (long)caller_uid;
            r.owner_gid = (long)(pw ? pw->pw_gid : (gid_t)caller_uid);
            r.dirs  = (const char *const *)dirs;  r.ndirs  = nd;
            r.files = (const char *const *)files; r.nfiles = nf;
            restore_run(c, &r);
            op_finish(fd);

            for (int i = 0; i < nd; i++) free(dirs[i]);
            for (int i = 0; i < nf; i++) free(files[i]);
        }
        free(dest);
    } else if (strcmp(cmd, "verify") == 0) {
        cmd_verify(c, 0, NULL);
        op_finish(fd);
    } else if (strcmp(cmd, "prune") == 0) {
        char *argv[14];
        int argc = 0;
        char b_last[24], b_daily[24], b_weekly[24], b_monthly[24], b_yearly[24];
        long long v;
        if ((v = ipc_get_int(line, "keep_last",    0)) > 0) {
            snprintf(b_last, sizeof b_last, "%lld", v);
            argv[argc++] = "--keep-last"; argv[argc++] = b_last;
        }
        if ((v = ipc_get_int(line, "keep_daily",   0)) > 0) {
            snprintf(b_daily, sizeof b_daily, "%lld", v);
            argv[argc++] = "--keep-daily"; argv[argc++] = b_daily;
        }
        if ((v = ipc_get_int(line, "keep_weekly",  0)) > 0) {
            snprintf(b_weekly, sizeof b_weekly, "%lld", v);
            argv[argc++] = "--keep-weekly"; argv[argc++] = b_weekly;
        }
        if ((v = ipc_get_int(line, "keep_monthly", 0)) > 0) {
            snprintf(b_monthly, sizeof b_monthly, "%lld", v);
            argv[argc++] = "--keep-monthly"; argv[argc++] = b_monthly;
        }
        if ((v = ipc_get_int(line, "keep_yearly",  0)) > 0) {
            snprintf(b_yearly, sizeof b_yearly, "%lld", v);
            argv[argc++] = "--keep-yearly"; argv[argc++] = b_yearly;
        }
        if (ipc_get_int(line, "dry_run", 0) != 0) argv[argc++] = "-n";
        cmd_prune(c, argc, argv);
        op_finish(fd);
    } else {
        char ecmd[128], buf[256];
        ipc_json_escape(cmd, ecmd, sizeof ecmd);
        snprintf(buf, sizeof buf,
                 "{\"event\":\"error\",\"msg\":\"unknown cmd: %s\"}", ecmd);
        ipc_send(fd, buf);
    }

    if (logged_op) {
        /* A successful interactive backup/prune means the passphrase is good
           again: clear any permanent latch so automatic work resumes without a
           restart. (Reached only if the op did not die() out via conn_die.) */
        userstate_enable(c->src->name);
        log_info("ipc: %s '%s' completed", cmd, c->src->name);
    }

    pthread_cleanup_pop(1);   /* op_cleanup: release mutex, free Ctx + cmd */

out:
    if (cs.fd >= 0) close(cs.fd);
    free(config_path);        /* NULL here if op_cleanup already claimed it */
    tls_cs = NULL;
    return NULL;
}
