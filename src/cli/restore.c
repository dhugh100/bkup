#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <pwd.h>
#include <grp.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>

#include "commands.h"
#include "common/db.h"
#include "common/types.h"
#include "common/compress.h"
#include "common/crypto.h"
#include "common/transport.h"
#include "common/log.h"

typedef struct {
    char     *path;
    long long mode, mtime_sec, mtime_nsec;
} DirRec;

typedef struct { DirRec *v; size_t n, cap; } DirList;

static void dl_push(DirList *d, const char *path, long long mode,
                    long long sec, long long nsec)
{
    if (d->n == d->cap) {
        d->cap = d->cap ? d->cap * 2 : 64;
        d->v = xrealloc(d->v, d->cap * sizeof *d->v);
    }
    d->v[d->n].path = xstrdup(path);
    d->v[d->n].mode = mode;
    d->v[d->n].mtime_sec = sec;
    d->v[d->n].mtime_nsec = nsec;
    d->n++;
}

static void set_times(const char *path, long long sec, long long nsec, int nofollow)
{
    struct timespec ts[2];
    ts[0].tv_sec = sec; ts[0].tv_nsec = nsec;   /* atime = mtime */
    ts[1].tv_sec = sec; ts[1].tv_nsec = nsec;
    if (utimensat(AT_FDCWD, path, ts, nofollow ? AT_SYMLINK_NOFOLLOW : 0) != 0)
        log_warn("restore: cannot set timestamps on %s (%s)", path, strerror(errno));
}

/* Chown a restored entry to uid:gid when requested (uid >= 0). Used by the root
   daemon to hand restored files back to the calling user. A failure here means
   the file is restored with the WRONG owner, so it must not be silent. */
static void set_owner(const char *path, long uid, long gid, int nofollow)
{
    if (uid < 0) return;
    int r = nofollow ? lchown(path, (uid_t)uid, (gid_t)gid)
                     : chown(path, (uid_t)uid, (gid_t)gid);
    if (r != 0)
        log_warn("restore: cannot chown %s to %ld:%ld (%s) -- restored with "
                 "wrong ownership", path, uid, gid, strerror(errno));
}

/* Build DEST/<relative> where relative is path stripped of prefix (or leading
   slash when no prefix).  Handles prefix with or without trailing slash. */
static char *dest_path(const char *dest, const char *prefix, const char *path)
{
    const char *rel;
    if (prefix) {
        size_t plen = strlen(prefix);
        /* strip trailing slashes from prefix length for comparison */
        while (plen > 0 && prefix[plen - 1] == '/') plen--;
        if (strncmp(path, prefix, plen) == 0 &&
                (path[plen] == '/' || path[plen] == '\0')) {
            rel = path + plen;
            while (*rel == '/') rel++;
        } else {
            rel = (path[0] == '/') ? path + 1 : path;
        }
    } else {
        rel = (path[0] == '/') ? path + 1 : path;
    }
    return path_join(dest, rel);
}

static void ensure_parent(const char *full)
{
    char *tmp = xstrdup(full);
    char *slash = strrchr(tmp, '/');
    if (slash) { *slash = '\0'; if (tmp[0]) mkdir_p(tmp, 0700); }
    free(tmp);
}

static void restore_regular(Ctx *c, long long vid, const char *full,
                            long long mode)
{
    int fd = open(full, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) die("cannot create %s", full);

    sqlite3_stmt *q = db_prep(c->db,
        "SELECT hash FROM version_blobs WHERE version_id=? ORDER BY seq");
    sqlite3_bind_int64(q, 1, vid);
    while (sqlite3_step(q) == SQLITE_ROW) {
        const uint8_t *h = sqlite3_column_blob(q, 0);
        char hex[BK_HEX_LEN + 1];
        hex_encode(h, BK_HASH_LEN, hex);
        char *remote = blob_repo_path(c, hex);

        Buf ct; buf_init(&ct);
        if (transport_get(c->t, remote, &ct) != 0)
            die("cannot fetch blob %s for %s", hex, full);
        Buf comp; buf_init(&comp);
        if (crypto_open(&c->key, ct.data, ct.len, &comp) != 0)
            die("decryption failed for blob %s (tampered or wrong key)", hex);
        Buf pt; buf_init(&pt);
        if (zstd_decompress(comp.data, comp.len, &pt) != 0)
            die("decompression failed for blob %s", hex);

        uint8_t check[BK_HASH_LEN];
        bk_hash(pt.data, pt.len, check);
        if (memcmp(check, h, BK_HASH_LEN) != 0)
            die("hash mismatch on blob %s (corrupt repo)", hex);

        size_t off = 0;
        while (off < pt.len) {
            ssize_t w = write(fd, pt.data + off, pt.len - off);
            if (w < 0) die("write failed on %s", full);
            off += (size_t)w;
        }
        buf_free(&ct); buf_free(&comp); buf_free(&pt);
        free(remote);
    }
    sqlite3_finalize(q);
    close(fd);
    if (chmod(full, (mode_t)mode) != 0)
        log_warn("restore: cannot chmod %s (%s) -- restored with wrong "
                 "permissions", full, strerror(errno));
}

/* Child-side die handler. The daemon's conn_die() closes the IPC socket and
   pthread_exit()s -- neither is right in the forked restore child, which must
   not touch the parent's socket. Just exit non-zero so the parent's waitpid
   sees the failure. */
static void restore_child_die(void) { _exit(1); }

/* Child-side log hook: forward the fatal reason to the parent over a pipe (ud
   holds the write fd) so the parent can report the real cause to the GUI rather
   than a useless "see log". Non-fatal lines stay in the durable log only. */
static void restore_child_log_cb(const char *lvl, const char *msg, void *ud)
{
    int wfd = (int)(intptr_t)ud;
    if (lvl && lvl[0] == 'F' && wfd >= 0) {
        size_t n = strlen(msg);
        if (write(wfd, msg, n) < 0) { /* best-effort; parent falls back */ }
    }
}

/* Drop this (forked, root) process to the calling user so the kernel -- not our
   own code -- enforces "restore only where that user could write". setuid()ing
   off root clears the effective/permitted capability sets, so CAP_DAC_OVERRIDE
   /CAP_CHOWN/etc. stop bypassing DAC; initgroups() also pulls in the user's
   supplementary groups. Fails closed (aborts, never returns on error) rather
   than risk writing as root. */
static void restore_drop_priv(uid_t uid, gid_t gid)
{
    struct passwd *pw = getpwuid(uid);
    if (pw) {
        if (initgroups(pw->pw_name, gid) != 0)
            die("restore: initgroups(%s) failed: %s", pw->pw_name, strerror(errno));
    } else if (setgroups(1, &gid) != 0) {
        die("restore: setgroups failed: %s", strerror(errno));
    }
    if (setgid(gid) != 0)
        die("restore: setgid(%d) failed: %s", (int)gid, strerror(errno));
    if (setuid(uid) != 0)
        die("restore: setuid(%d) failed: %s", (int)uid, strerror(errno));
    /* Defensive: confirm root is unrecoverable before writing anything. */
    if (setuid(0) == 0)
        die("restore: privilege drop failed -- regained root");
}

/* Restore one target into `dest`, accumulating directory metadata into `dl`
   (applied children-first by the caller) and counting restored files in
   `*nfiles`. `path` selects what to restore:
     - path == NULL : the whole snapshot / everything as of the date
     - is_dir != 0  : the directory `path` and everything beneath it
     - is_dir == 0  : the single file/symlink `path`
   When `asof > 0` each matching path is resolved to its newest version captured
   at or before `asof` (so a file deleted before the cutoff still restores its
   last copy); otherwise the exact contents of snapshot `want_snap` are used. */
static void restore_target(Ctx *c, const char *dest, long owner_uid, long owner_gid,
                           long long want_snap, long long asof,
                           const char *path, int is_dir,
                           DirList *dl, long long *nfiles)
{
    const char *prefix = NULL;
    const char *filter_file = NULL;
    const char *strip_base = NULL;
    char prefix_buf[4096];
    char parent_base[4096];

    if (path && is_dir) {
        /* Directory: strip its *parent* so it is recreated by name under dest --
           "recover C into /tmp" gives /tmp/C/... rather than spilling C's
           contents directly into /tmp. */
        prefix = path;
        snprintf(parent_base, sizeof parent_base, "%s", path);
        size_t pl = strlen(parent_base);
        while (pl > 1 && parent_base[pl - 1] == '/') parent_base[--pl] = '\0';
        char *slash = strrchr(parent_base, '/');
        if (slash == parent_base) parent_base[1] = '\0';   /* parent is "/" */
        else if (slash) *slash = '\0';
        else parent_base[0] = '\0';
        strip_base = parent_base[0] ? parent_base : NULL;
    } else if (path) {
        /* Single file: strip its own directory so it lands at dest/<basename>. */
        filter_file = path;
        snprintf(prefix_buf, sizeof prefix_buf, "%s", path);
        char *slash = strrchr(prefix_buf, '/');
        if (slash == prefix_buf) prefix_buf[1] = '\0';     /* file at root */
        else if (slash) *slash = '\0';
        prefix = prefix_buf;
        strip_base = prefix;
    }

    /* Two resolution modes, both reading only the (now small) versions table:
         as-of : per path, the newest version captured at or before `asof` --
                 i.e. the greatest first_snapshot that is <= the cutoff snapshot
                 A (the newest committed snapshot at/before asof). A file deleted
                 before the cutoff still restores its last copy.
         point : the version live in snapshot want_snap, via its half-open
                 interval first_snapshot<=S AND (last_snapshot IS NULL OR
                 last_snapshot>S).
       Intervals are disjoint per path, so at most one row per path either way. */
    char *sql;
    if (asof > 0) {
        if (filter_file)
            sql = sqlite3_mprintf(
                "SELECT v.id,v.path,v.kind,v.mode,v.mtime_sec,v.mtime_nsec,"
                "v.link_target FROM versions v "
                "WHERE v.path=%Q AND v.first_snapshot=("
                "  SELECT MAX(v2.first_snapshot) FROM versions v2 WHERE v2.path=%Q "
                "  AND v2.first_snapshot<="
                "    (SELECT MAX(id) FROM snapshots WHERE state=1 AND created<=%lld))",
                filter_file, filter_file, asof);
        else if (prefix) {
            char pdir[4096];
            snprintf(pdir, sizeof pdir, "%s", prefix);
            size_t pl = strlen(pdir);
            while (pl > 1 && pdir[pl - 1] == '/') pdir[--pl] = '\0';
            char punder[4098];
            int ulen = snprintf(punder, sizeof punder, "%s/", pdir);
            sql = sqlite3_mprintf(
                "SELECT v.id,v.path,v.kind,v.mode,v.mtime_sec,v.mtime_nsec,"
                "v.link_target FROM versions v "
                "WHERE (v.path=%Q OR substr(v.path,1,%d)=%Q) AND v.first_snapshot=("
                "  SELECT MAX(v2.first_snapshot) FROM versions v2 WHERE v2.path=v.path "
                "  AND v2.first_snapshot<="
                "    (SELECT MAX(id) FROM snapshots WHERE state=1 AND created<=%lld)) "
                "ORDER BY v.path",
                pdir, ulen, punder, asof);
        } else
            sql = sqlite3_mprintf(
                "SELECT v.id,v.path,v.kind,v.mode,v.mtime_sec,v.mtime_nsec,"
                "v.link_target FROM versions v "
                "WHERE v.first_snapshot=("
                "  SELECT MAX(v2.first_snapshot) FROM versions v2 WHERE v2.path=v.path "
                "  AND v2.first_snapshot<="
                "    (SELECT MAX(id) FROM snapshots WHERE state=1 AND created<=%lld)) "
                "ORDER BY v.path",
                asof);
    } else if (filter_file)
        sql = sqlite3_mprintf(
            "SELECT v.id,v.path,v.kind,v.mode,v.mtime_sec,v.mtime_nsec,"
            "v.link_target FROM versions v "
            "WHERE v.first_snapshot<=%lld "
            "AND (v.last_snapshot IS NULL OR v.last_snapshot>%lld) "
            "AND v.path=%Q ORDER BY v.path",
            want_snap, want_snap, filter_file);
    else if (prefix) {
        /* Restrict to the selected object: the directory itself and everything
           strictly beneath PREFIX/ -- never a sibling that just shares the name
           as a string prefix (selecting "C" must not pull in "C2" or "Cat").
           substr() avoids LIKE treating '_'/'%' in a name as wildcards. */
        char pdir[4096];
        snprintf(pdir, sizeof pdir, "%s", prefix);
        size_t pl = strlen(pdir);
        while (pl > 1 && pdir[pl - 1] == '/') pdir[--pl] = '\0';
        char punder[4098];
        int ulen = snprintf(punder, sizeof punder, "%s/", pdir);
        sql = sqlite3_mprintf(
            "SELECT v.id,v.path,v.kind,v.mode,v.mtime_sec,v.mtime_nsec,"
            "v.link_target FROM versions v "
            "WHERE v.first_snapshot<=%lld "
            "AND (v.last_snapshot IS NULL OR v.last_snapshot>%lld) "
            "AND (v.path=%Q OR substr(v.path,1,%d)=%Q) ORDER BY v.path",
            want_snap, want_snap, pdir, ulen, punder);
    }
    else
        sql = sqlite3_mprintf(
            "SELECT v.id,v.path,v.kind,v.mode,v.mtime_sec,v.mtime_nsec,"
            "v.link_target FROM versions v "
            "WHERE v.first_snapshot<=%lld "
            "AND (v.last_snapshot IS NULL OR v.last_snapshot>%lld) "
            "ORDER BY v.path",
            want_snap, want_snap);
    sqlite3_stmt *q = db_prep(c->db, sql);
    sqlite3_free(sql);

    while (sqlite3_step(q) == SQLITE_ROW) {
        long long vid  = sqlite3_column_int64(q, 0);
        const char *p  = (const char *)sqlite3_column_text(q, 1);
        int kind       = sqlite3_column_int(q, 2);
        long long mode = sqlite3_column_int64(q, 3);
        long long msec = sqlite3_column_int64(q, 4);
        long long mns  = sqlite3_column_int64(q, 5);
        const char *lt = (const char *)sqlite3_column_text(q, 6);

        char *full = dest_path(dest, strip_base, p);
        ensure_parent(full);

        if (kind == FK_DIR) {
            mkdir_p(full, 0700);
            set_owner(full, owner_uid, owner_gid, 0);
            dl_push(dl, full, mode, msec, mns);  /* metadata in pass 2 */
        } else if (kind == FK_SYMLINK) {
            unlink(full);
            if (lt && symlink(lt, full) != 0)
                log_warn("cannot create symlink %s", full);
            set_owner(full, owner_uid, owner_gid, 1);
            set_times(full, msec, mns, 1);
        } else {
            restore_regular(c, vid, full, mode);
            set_owner(full, owner_uid, owner_gid, 0);
            set_times(full, msec, mns, 0);
            (*nfiles)++;
        }
        free(full);
    }
    sqlite3_finalize(q);
}

/* Restore every target in `r` in a single pass: one transport connection, one
   privilege drop, one as-of date. See restore_target for the per-target rules. */
int restore_run(Ctx *c, const RestoreReq *r)
{
    if (!r->dest) die("restore requires a destination");
    long long want_snap = r->snap;
    long long asof = r->asof;
    int ntargets = r->ndirs + r->nfiles;

    ctx_open_db(c);
    ctx_connect(c);
    key_from_meta(c);

    /* The as-of path resolves each path's own newest version and needs no single
       snapshot id; only the legacy snapshot path resolves a snapshot here. */
    if (asof <= 0 && want_snap < 0) {
        sqlite3_stmt *q = db_prep(c->db,
            "SELECT MAX(id) FROM snapshots WHERE state=1");
        if (sqlite3_step(q) == SQLITE_ROW &&
            sqlite3_column_type(q, 0) != SQLITE_NULL)
            want_snap = sqlite3_column_int64(q, 0);
        sqlite3_finalize(q);
        if (want_snap < 0) die("no complete snapshot to restore");
    }

    if (asof > 0) {
        time_t t = (time_t)asof;
        struct tm tm; char tb[48];
        localtime_r(&t, &tm);
        strftime(tb, sizeof tb, "%Y-%m-%d %H:%M", &tm);
        if (ntargets == 0)
            log_info("restoring everything as of %s to %s", tb, r->dest);
        else
            log_info("restoring %d item(s) as of %s to %s", ntargets, tb, r->dest);
    } else {
        log_info("restoring snapshot %lld to %s", want_snap, r->dest);
    }

    /* The daemon runs as root (holding CAP_DAC_OVERRIDE), so neither DAC nor our
       own code would otherwise stop a restore from writing anywhere. Fork a
       child and drop it fully to the calling user; from there the kernel
       confines every create/write below to paths that user could write.
       owner_uid <= 0 (CLI invocation already running as the user, or a root
       restore) keeps the original in-process path. */
    int restore_in_child = 0;
    if (r->owner_uid > 0 && r->owner_gid >= 0) {
        int efd[2];
        if (pipe(efd) != 0)
            die("restore: pipe failed: %s", strerror(errno));
        pid_t pid = fork();
        if (pid < 0)
            die("restore: fork failed: %s", strerror(errno));
        if (pid == 0) {
            /* Swap the daemon's per-thread IPC log sink for one that pipes the
               fatal reason to the parent, and replace the die hook (both the
               daemon's reach into the parent's socket / pthread state). Then
               drop privileges before touching the filesystem. */
            close(efd[0]);
            log_set_thread_cb(restore_child_log_cb, (void *)(intptr_t)efd[1]);
            log_set_thread_die(restore_child_die);
            restore_drop_priv((uid_t)r->owner_uid, (gid_t)r->owner_gid);
            restore_in_child = 1;
        } else {
            /* Parent must not touch c->db / c->t while the child uses them. */
            close(efd[1]);
            char emsg[512];
            ssize_t en = read(efd[0], emsg, sizeof emsg - 1);
            close(efd[0]);
            emsg[en > 0 ? en : 0] = '\0';
            int st;
            while (waitpid(pid, &st, 0) < 0 && errno == EINTR) { }
            if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
                if (en > 0) die("restore failed: %s", emsg);
                die("restore failed (uid=%ld)", r->owner_uid);
            }
            return 0;
        }
    }

    mkdir_p(r->dest, 0700);

    DirList dirs; memset(&dirs, 0, sizeof dirs);
    long long nfiles = 0;

    if (ntargets == 0) {
        restore_target(c, r->dest, r->owner_uid, r->owner_gid, want_snap, asof,
                       NULL, 0, &dirs, &nfiles);
    } else {
        for (int i = 0; i < r->ndirs; i++)
            restore_target(c, r->dest, r->owner_uid, r->owner_gid, want_snap, asof,
                           r->dirs[i], 1, &dirs, &nfiles);
        for (int i = 0; i < r->nfiles; i++)
            restore_target(c, r->dest, r->owner_uid, r->owner_gid, want_snap, asof,
                           r->files[i], 0, &dirs, &nfiles);
    }

    /* pass 2: set directory mode + mtime children-first so writing files
       inside them did not re-stamp the times we want */
    for (size_t i = dirs.n; i-- > 0; ) {
        if (chmod(dirs.v[i].path, (mode_t)dirs.v[i].mode) != 0)
            log_warn("restore: cannot chmod %s (%s)", dirs.v[i].path,
                     strerror(errno));
        set_times(dirs.v[i].path, dirs.v[i].mtime_sec, dirs.v[i].mtime_nsec, 0);
        free(dirs.v[i].path);
    }
    free(dirs.v);

    log_info("restored %lld file(s) and %zu director(ies)", nfiles, dirs.n);
    /* In the dropped child, return would unwind back into the daemon's per-conn
       code as the wrong user; hand the exit status to the waiting parent. */
    if (restore_in_child)
        _exit(0);
    return 0;
}

int cmd_restore(Ctx *c, int argc, char **argv)
{
    RestoreReq r;
    memset(&r, 0, sizeof r);
    r.snap = -1;
    r.owner_uid = -1;
    r.owner_gid = -1;

    const char *dirs[256];
    const char *files[256];
    int nd = 0, nf = 0;

    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "-s") && i + 1 < argc) r.snap = atoll(argv[++i]);
        else if (!strcmp(argv[i], "-A") && i + 1 < argc) r.asof = atoll(argv[++i]);
        else if (!strcmp(argv[i], "-p") && i + 1 < argc) {
            if (nd < 256) dirs[nd++] = argv[++i]; else i++;
        }
        else if (!strcmp(argv[i], "-f") && i + 1 < argc) {
            if (nf < 256) files[nf++] = argv[++i]; else i++;
        }
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) {
            const char *spec = argv[++i];
            r.owner_uid = atol(spec);
            const char *colon = strchr(spec, ':');
            r.owner_gid = colon ? atol(colon + 1) : -1;
        }
        else if (argv[i][0] != '-') r.dest = argv[i];
    }
    if (!r.dest)
        die("usage: bkup restore [-s SNAP] [-A EPOCH] [-p DIR]... [-f FILE]... DEST");

    r.dirs = dirs;   r.ndirs = nd;
    r.files = files; r.nfiles = nf;
    return restore_run(c, &r);
}
