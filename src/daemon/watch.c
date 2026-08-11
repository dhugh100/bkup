#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <setjmp.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <errno.h>
#include <limits.h>
#include <ftw.h>
#include <sys/stat.h>
#include <sys/fanotify.h>
#include <sys/vfs.h>

#include "watch.h"
#include "ipc.h"
#include "catalog_push.h"
#include "userstate.h"
#include "coalesce.h"
#include "cli/commands.h"
#include "cli/ctx.h"
#include "common/config.h"
#include "common/log.h"

/* NOTE: the fanotify event loop below cannot run in a sandbox -- it needs root
   (FAN_REPORT_DFID_NAME requires CAP_SYS_ADMIN) and a live filesystem
   generating events. The pure coalescing logic (coalesce.c) is unit-tested;
   this file is compile-verified and must be validated on the server. */

#define WATCH_MAX_USERS   64
#define WATCH_MAX_SOURCES 256
#define WATCH_MAX_FS      32        /* distinct filesystems/btrfs subvolumes */
#define DEBOUNCE_MS       2000      /* quiet period before flushing a root */
#define MAX_PENDING_MS    30000     /* cap: flush even under steady activity */

/* Events that should trigger a rescan of the affected subtree. FAN_DELETE_SELF
   and FAN_MOVE_SELF are requested but discarded on arrival -- see handle_events
   for why they cannot be resolved to a path. */
#define WATCH_MASK (FAN_CREATE | FAN_DELETE | FAN_MOVED_FROM | FAN_MOVED_TO | \
                    FAN_CLOSE_WRITE | FAN_ATTRIB | FAN_DELETE_SELF | \
                    FAN_MOVE_SELF | FAN_ONDIR)

typedef struct {
    char        name[128];
    const User *user;       /* into Watch.cfg_ctx, for user_excluded() */
    char        datadir[PATH_MAX]; /* bkup's own state dir (dirname of db) */
    Coalescer   cz;
    long long   first_ms;   /* when the current batch started */
    long long   last_ms;    /* most recent event in the batch */
    int         pending;
} UserWatch;

typedef struct {
    char src[PATH_MAX];
    int  user;              /* index into uw[] */
} SrcMap;

/* One fanotify group per filesystem/subvolume. A FID-reporting group cannot
   hold inode marks that span btrfs subvolumes: each subvolume has a distinct
   fsid and (since Linux 6.8) fanotify_mark returns EXDEV when a group's marks
   would straddle two of them. So every distinct fsid gets its own group, with
   an open dir fd into that fsid for open_by_handle_at path resolution. */
typedef struct {
    __kernel_fsid_t fsid;
    int             fan;    /* fanotify group fd for this fsid */
    int             mfd;    /* dir fd in the fsid, for open_by_handle_at */
} FsGroup;

typedef struct {
    char       config_path[1024];
    Ctx       *cfg_ctx;         /* kept alive for the watch's lifetime so the
                                   UserWatch.user pointers stay valid */
    UserWatch  uw[WATCH_MAX_USERS];
    int        nuw;
    SrcMap     smap[WATCH_MAX_SOURCES];
    int        nsmap;
    FsGroup    grp[WATCH_MAX_FS];
    int        ngrp;
    long long  drops;           /* unresolved events since the last drop report */
    long long  drop_report_ms;  /* when we last logged a drop summary (0 = never) */
} Watch;

/* die() inside a triggered continuous must not kill the daemon */
static _Thread_local jmp_buf g_watch_jb;
static void watch_die(void) { longjmp(g_watch_jb, 1); }

/* defined below in the setup section; used by handle_events() above it */
static int  mark_dir(Watch *w, const char *path);
static long mark_tree(Watch *w, const char *root);

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* A dropped event is a filesystem change the continuous-backup watcher could not act
   on -- i.e. a change that will not be backed up until the next scheduled or
   coalesced full pass.  This must never be silent (the whole point of the
   watch), but a systemic failure could fire per-event, so the first drop logs
   immediately and the rest are summarized at most once per 30s.  Logged at warn
   level: the watcher is still running and the daily backup remains the backstop;
   a flood of these means the watch is effectively broken and needs attention. */
static void note_drop(Watch *w, const char *fmt, ...)
{
    w->drops++;
    long long now = now_ms();
    if (w->drop_report_ms != 0 && now - w->drop_report_ms < 30000) return;

    char detail[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, sizeof detail, fmt, ap);
    va_end(ap);

    log_warn("watch: DROPPED %lld change event(s) since last report (%s) -- "
             "those changes are NOT in a continuous backup until the next full pass",
             w->drops, detail);
    w->drop_report_ms = now;
    w->drops = 0;
}

/* ---- source -> user mapping (longest matching source prefix) ---- */

static int find_source(Watch *w, const char *path, char *src_out, size_t cap)
{
    int best = -1; size_t best_len = 0;
    for (int i = 0; i < w->nsmap; i++) {
        const char *s = w->smap[i].src;
        size_t sl = strlen(s);
        int under = (strncmp(path, s, sl) == 0) &&
                    (path[sl] == '\0' || path[sl] == '/');
        if (under && sl > best_len) { best = i; best_len = sl; }
    }
    if (best < 0) return -1;
    snprintf(src_out, cap, "%s", w->smap[best].src);
    return w->smap[best].user;
}

/* ---- fanotify path resolution ---- */

static int group_for_fsid(Watch *w, const __kernel_fsid_t *fsid)
{
    for (int i = 0; i < w->ngrp; i++)
        if (memcmp(&w->grp[i].fsid, fsid, sizeof *fsid) == 0)
            return i;
    return -1;
}

/* Resolve a DFID_NAME info record (parent dir handle + child name) to a full
   path. Returns 0 on success. */
static int resolve_path(Watch *w, struct fanotify_event_info_fid *fid,
                        const char *name, char *out, size_t cap)
{
    int gi = group_for_fsid(w, &fid->fsid);
    if (gi < 0) {
        note_drop(w, "no fanotify group for event filesystem");
        return -1;
    }

    struct file_handle *fh = (struct file_handle *)fid->handle;
    int fd = open_by_handle_at(w->grp[gi].mfd, fh, O_PATH);
    if (fd < 0) {
        note_drop(w, "open_by_handle_at: %s", strerror(errno));
        return -1;
    }

    char proc[64], dir[PATH_MAX];
    snprintf(proc, sizeof proc, "/proc/self/fd/%d", fd);
    ssize_t n = readlink(proc, dir, sizeof dir - 1);
    close(fd);
    if (n < 0) {
        note_drop(w, "readlink of resolved handle: %s", strerror(errno));
        return -1;
    }
    dir[n] = '\0';

    /* Build "dir/name" with explicit bounds (snprintf's "%s/%s" of two
       PATH_MAX strings trips -Wformat-truncation). */
    size_t dl = strlen(dir);
    if (name && name[0] && strcmp(name, ".") != 0) {
        size_t nl = strlen(name);
        if (dl + 1 + nl + 1 > cap) {
            note_drop(w, "resolved path too long under %s", dir);
            return -1;
        }
        memcpy(out, dir, dl);
        out[dl] = '/';
        memcpy(out + dl + 1, name, nl + 1);
    } else {
        if (dl + 1 > cap) {
            note_drop(w, "resolved path too long: %s", dir);
            return -1;
        }
        memcpy(out, dir, dl + 1);
    }
    return 0;
}

/* ---- triggering a continuous backup (serialized with scheduled ops) ---- */

static void trigger_continuous(Watch *w, const char *user, const char *root)
{
    if (userstate_disabled(user)) return;
    Ctx *c = ctx_new_nosel(w->config_path);
    ctx_use_user(c, user);
    log_set_thread_die(watch_die);
    ipc_op_lock("continuous");
    if (setjmp(g_watch_jb) == 0) {
        ctx_open_db(c);
        ctx_connect(c);
        key_from_meta(c);
        check_repo_id(c);
        long long snap = continuous_snapshot(c, root);
        if (snap >= 0)
            catalog_mark_dirty(user);   /* defer the catalog push (batched) */
        userstate_enable(user);         /* success clears any prior latch */
    } else {
        log_warn("watch: continuous backup of '%s' under %s failed", user, root);
        if (log_take_permanent())
            userstate_disable(user, "continuous backup failed with a permanent error");
    }
    ipc_op_unlock();
    log_set_thread_die(NULL);
    ctx_free(c);
}

/* ---- coalescer flushing ---- */

static void flush_user(Watch *w, int u)
{
    char root[PATH_MAX];
    if (!coalesce_take(&w->uw[u].cz, root, sizeof root)) return;
    w->uw[u].pending = 0;
    trigger_continuous(w, w->uw[u].name, root);
}

/* Flush every user whose debounce or max-pending deadline has passed. Returns
   the ms until the next pending deadline, or -1 if nothing is pending. */
static int flush_due(Watch *w)
{
    long long now = now_ms();
    long long next = -1;
    for (int u = 0; u < w->nuw; u++) {
        if (!w->uw[u].pending) continue;
        long long quiet_at = w->uw[u].last_ms + DEBOUNCE_MS;
        long long cap_at   = w->uw[u].first_ms + MAX_PENDING_MS;
        long long due = quiet_at < cap_at ? quiet_at : cap_at;
        if (now >= due) {
            flush_user(w, u);
            continue;
        }
        long long wait = due - now;
        if (next < 0 || wait < next) next = wait;
    }
    return next < 0 ? -1 : (int)next;
}

/* ---- event handling ---- */

/* True if `path` is `dir` itself or lives under `dir/`. */
static int path_in_dir(const char *path, const char *dir)
{
    if (!dir || !dir[0]) return 0;
    size_t dl = strlen(dir);
    return strncmp(path, dir, dl) == 0 && (path[dl] == '\0' || path[dl] == '/');
}

static void note_path(Watch *w, const char *path, int removed)
{
    /* Derive the effective path to queue: for removals, fold to the parent
       directory (the deleted object is gone; its parent's rescan discovers the
       deletion via the scoped vanished-file sweep).  Pure logic extracted to
       watch_effective_path (coalesce.c) so it can be unit-tested. */
    char effective[PATH_MAX];
    if (!watch_effective_path(path, removed, effective, sizeof effective))
        return;
    path = effective;

    char src[PATH_MAX];
    int u = find_source(w, path, src, sizeof src);
    if (u < 0) {
        /* We only ever mark dirs inside configured sources, so a resolved event
           should always map to one. If not, our source map is out of sync with
           what we marked -- surface it rather than silently skip the change. */
        note_drop(w, "resolved path under no configured source: %s", path);
        return;
    }

    /* Deliberately ignored (not dropped -- these never trigger a continuous
       snapshot):
       - bkup's own catalog dir, whose writes would otherwise self-trigger an
         endless continuous loop (every snapshot writes the db, which fires events);
       - anything the user excludes from backup, so a hot file (e.g. a live
         database) the user has excluded does not drive constant continuous churn;
       - anything in `continuous-exclude`: still captured by scans (so it round-
         trips), but its own churn (e.g. a live mail store) must not mint snapshots.
       note_drop() is for changes we wanted but missed; these we never wanted. */
    if (path_in_dir(path, w->uw[u].datadir)) return;
    if (w->uw[u].user && user_excluded_subtree(w->uw[u].user, path, src)) return;
    if (w->uw[u].user && user_continuous_excluded(w->uw[u].user, path, src)) return;

    /* cross-source change: flush the pending root, then start fresh */
    if (coalesce_fold(&w->uw[u].cz, path, src) == 1) {
        flush_user(w, u);
        coalesce_fold(&w->uw[u].cz, path, src);
    }
    long long now = now_ms();
    if (!w->uw[u].pending) { w->uw[u].pending = 1; w->uw[u].first_ms = now; }
    w->uw[u].last_ms = now;
}

static void handle_events(Watch *w, const char *buf, size_t len)
{
    const struct fanotify_event_metadata *m = (const void *)buf;
    while (FAN_EVENT_OK(m, len)) {
        /* Queue overflow: the kernel dropped events because our queue filled
           up, so some changes were missed entirely. The next full/coalesced
           pass is the backstop, but never let this be silent. */
        if (m->mask & FAN_Q_OVERFLOW) {
            note_drop(w, "fanotify queue overflow (raise fs.fanotify.max_queued_events)");
            m = FAN_EVENT_NEXT(m, len);
            continue;
        }
        /* Self events (the marked object itself was deleted or moved) carry a
           FAN_EVENT_INFO_TYPE_FID record naming that object, not its parent.
           For FAN_DELETE_SELF the inode is already unlinked, so resolving the
           handle always fails with ESTALE -- counting that as a drop would make
           every rmdir of a watched dir look like a lost change. Skip them: the
           same deletion/move arrives as FAN_DELETE / FAN_MOVED_FROM on the
           parent's mark, with a resolvable parent handle plus the entry name,
           and that is what queues the rescan. (The one case not covered is
           deleting a source root itself, whose parent we never marked.) */
        if (m->mask & (FAN_DELETE_SELF | FAN_MOVE_SELF)) {
            m = FAN_EVENT_NEXT(m, len);
            continue;
        }
        /* Walk the info records following the fixed metadata; act on the first
           one that carries a directory fid + name (DFID_NAME / DFID / FID). */
        const char *p   = (const char *)m + sizeof *m;
        const char *end = (const char *)m + m->event_len;
        while (p + sizeof(struct fanotify_event_info_header) <= end) {
            const struct fanotify_event_info_header *h = (const void *)p;
            if (h->len == 0 || p + h->len > end) break;
            if (h->info_type == FAN_EVENT_INFO_TYPE_DFID_NAME ||
                h->info_type == FAN_EVENT_INFO_TYPE_DFID ||
                h->info_type == FAN_EVENT_INFO_TYPE_FID) {
                struct fanotify_event_info_fid *fid =
                    (struct fanotify_event_info_fid *)p;
                const char *name = NULL;
                if (h->info_type == FAN_EVENT_INFO_TYPE_DFID_NAME) {
                    struct file_handle *fh = (struct file_handle *)fid->handle;
                    name = (const char *)fh->f_handle + fh->handle_bytes;
                }
                char path[PATH_MAX];
                if (resolve_path(w, fid, name, path, sizeof path) == 0) {
                    /* Removal events name an object that is already gone;
                       note_path() folds the parent dir so its rescan can
                       record the deletion (see note_path). Self events never
                       get here (skipped above), so only the parent-reported
                       removals matter. */
                    int removed = (m->mask & (FAN_DELETE | FAN_MOVED_FROM)) != 0;
                    note_path(w, path, removed);
                    /* A new directory entered the tree (created or moved in):
                       extend coverage into it, since inode marks don't cascade.
                       mark_tree() also catches anything already inside a
                       moved-in subtree. */
                    if ((m->mask & (FAN_CREATE | FAN_MOVED_TO)) &&
                        (m->mask & FAN_ONDIR)) {
                        if (mark_dir(w, path) == 0)
                            mark_tree(w, path);
                        else
                            log_warn("watch: could not extend watch into new "
                                     "dir %s (%s) -- changes inside it may be "
                                     "missed until the next full backup",
                                     path, strerror(errno));
                    }
                }
            }
            p += h->len;
        }
        m = FAN_EVENT_NEXT(m, len);
    }
}

/* ---- setup ---- */

/* Find, or lazily create, the fanotify group for the filesystem backing
   `path`. Returns the group index, or -1 on failure. A new fsid gets its own
   fanotify group (inode marks can't span btrfs subvolumes -- see FsGroup)
   plus an open dir fd into the fsid for open_by_handle_at. */
static int group_for_path(Watch *w, const char *path)
{
    struct statfs sfs;
    if (statfs(path, &sfs) != 0) return -1;
    __kernel_fsid_t fsid;
    memcpy(&fsid, &sfs.f_fsid, sizeof fsid);

    int gi = group_for_fsid(w, &fsid);
    if (gi >= 0) return gi;                           /* already have it */
    if (w->ngrp >= WATCH_MAX_FS) return -1;

    /* FAN_REPORT_DFID_NAME delivers parent-dir file handles + names for
       create/delete/move, which is what lets us reconstruct paths. */
    int fan = fanotify_init(FAN_CLASS_NOTIF | FAN_REPORT_DFID_NAME, O_RDONLY);
    if (fan < 0) return -1;
    /* Mount fd for open_by_handle_at() during event resolution. It must be a
       real open fd, not O_PATH: open_by_handle_at() rejects an O_PATH mount fd
       with EBADF, which would silently fail every path resolution. */
    int mfd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (mfd < 0) { close(fan); return -1; }

    w->grp[w->ngrp].fsid = fsid;
    w->grp[w->ngrp].fan  = fan;
    w->grp[w->ngrp].mfd  = mfd;
    return w->ngrp++;
}

/* Add an fanotify inode mark on a single directory, in the group that owns its
   filesystem. Inode marks aren't recursive (mark_tree walks the subtree) and
   can't span btrfs subvolumes within one group, so each fsid is marked in its
   own group (see group_for_path / FsGroup). */
static int mark_dir(Watch *w, const char *path)
{
    int gi = group_for_path(w, path);
    if (gi < 0) return -1;
    return fanotify_mark(w->grp[gi].fan, FAN_MARK_ADD, WATCH_MASK,
                         AT_FDCWD, path);
}

/* nftw() has no user-data argument; the walk is single-threaded so file-scope
   state for the active Watch (and the mark counters) is sufficient. */
static Watch *g_walk_w;
static long   g_walk_marked;
static long   g_walk_failed;

static int walk_mark_cb(const char *path, const struct stat *sb,
                        int type, struct FTW *fb)
{
    (void)sb; (void)fb;
    /* Directories only: a directory inode mark reports entry changes
       (create/delete/move) for its immediate children. Per-path failures
       (e.g. a dir deleted mid-walk, or hitting fs.fanotify.max_user_marks)
       are skipped so one bad path doesn't abort the whole walk, but they are
       counted: a large failure count means coverage is incomplete. */
    if (type == FTW_D || type == FTW_DP) {
        if (mark_dir(g_walk_w, path) == 0)
            g_walk_marked++;
        else
            g_walk_failed++;
    }
    return 0;
}

/* Recursively mark every directory at or below `root`; returns the count
   marked. FTW_PHYS keeps us from following symlinks out of the tree; nested
   btrfs subvolumes are descended into (no FTW_MOUNT) so their dirs get marked
   too. A nonzero failure count is logged as a warning: the watch is then only
   partially covering the tree (likely fs.fanotify.max_user_marks too low). */
static long mark_tree(Watch *w, const char *root)
{
    g_walk_w = w;
    g_walk_marked = 0;
    g_walk_failed = 0;
    nftw(root, walk_mark_cb, 16, FTW_PHYS);
    g_walk_w = NULL;
    if (g_walk_failed > 0)
        log_warn("watch: failed to mark %ld of %ld dirs under %s -- watch "
                 "coverage there is INCOMPLETE (check fs.fanotify.max_user_marks)",
                 g_walk_failed, g_walk_failed + g_walk_marked, root);
    return g_walk_marked;
}

static void watch_close(Watch *w)
{
    for (int i = 0; i < w->ngrp; i++) {
        close(w->grp[i].fan);
        close(w->grp[i].mfd);
    }
    w->ngrp = 0;
}

static int watch_setup(Watch *w)
{
    Ctx *c = ctx_new_nosel(w->config_path);
    int n = c->cfg->nusers;
    if (n > WATCH_MAX_USERS) n = WATCH_MAX_USERS;

    for (int i = 0; i < n; i++) {
        User *u = &c->cfg->users[i];
        if (!u->continuous) {
            log_info("watch: continuous backups disabled for user '%s' by config",
                     u->name);
            continue;
        }
        snprintf(w->uw[w->nuw].name, sizeof w->uw[w->nuw].name, "%s", u->name);
        w->uw[w->nuw].user = u;          /* c is kept alive in w->cfg_ctx */
        /* bkup's own state dir = dirname(db); events there are self-churn */
        w->uw[w->nuw].datadir[0] = '\0';
        if (u->db) {
            snprintf(w->uw[w->nuw].datadir, sizeof w->uw[w->nuw].datadir,
                     "%s", u->db);
            char *slash = strrchr(w->uw[w->nuw].datadir, '/');
            if (slash) *slash = '\0';
            else w->uw[w->nuw].datadir[0] = '\0';
        }
        coalesce_reset(&w->uw[w->nuw].cz);
        w->uw[w->nuw].pending = 0;
        int ui = w->nuw++;

        for (int s = 0; s < u->nsources; s++) {
            const char *src = u->sources[s];
            /* Mark the source root first as the "can we watch this?" gate, then
               recurse into its subtree. fanotify inode marks aren't recursive,
               so we walk and mark every directory; each directory's fsid is
               marked in its own group (mark_dir/group_for_path), which is what
               lets a source span btrfs subvolumes. New dirs are marked on the
               fly when their FAN_CREATE|FAN_ONDIR event arrives (see
               handle_events). We filter events back down to the source subtrees
               in find_source(). */
            if (mark_dir(w, src) != 0) {
                log_warn("watch: cannot mark %s (%s)", src, strerror(errno));
                continue;
            }
            long nd = mark_tree(w, src);
            if (w->nsmap < WATCH_MAX_SOURCES) {
                snprintf(w->smap[w->nsmap].src, sizeof w->smap[w->nsmap].src,
                         "%s", src);
                w->smap[w->nsmap].user = ui;
                w->nsmap++;
            }
            log_info("watch: watching %s (user '%s', %ld dirs)",
                     src, u->name, nd);
        }
    }

    if (w->nsmap == 0) {
        log_info("watch: no sources to watch; continuous backups idle");
        watch_close(w);
        ctx_free(c);
        return -1;
    }
    w->cfg_ctx = c;     /* kept alive: UserWatch.user points into c->cfg */
    return 0;
}

/* ---- thread entry ---- */

static void *watch_loop(void *arg)
{
    Watch *w = arg;
    if (watch_setup(w) != 0) { free(w); return NULL; }

    char buf[8192];
    int stop = 0;

    while (!stop) {
        int timeout = flush_due(w);     /* also flushes anything already due */

        /* Rebuilt each pass: handle_events() may add a group when a new btrfs
           subvolume appears under a watched tree, and its fd must join poll.
           Snapshot ngrp so a group added mid-pass isn't read against an
           uninitialized pollfd slot -- it joins on the next pass instead. */
        int ng = w->ngrp;
        struct pollfd pfd[WATCH_MAX_FS];
        for (int i = 0; i < ng; i++) {
            pfd[i].fd      = w->grp[i].fan;
            pfd[i].events  = POLLIN;
            pfd[i].revents = 0;
        }

        int r = poll(pfd, ng, timeout);
        if (r < 0) {
            if (errno == EINTR) continue;
            log_err("watch: poll failed (%s); watcher STOPPING -- automatic continuous "
                    "backups are now DISABLED until bkupd is restarted",
                    strerror(errno));
            break;
        }
        if (r == 0) continue;           /* timeout: loop flushes due roots */

        for (int i = 0; i < ng; i++) {
            if (!(pfd[i].revents & POLLIN)) continue;
            ssize_t n = read(w->grp[i].fan, buf, sizeof buf);
            if (n < 0) {
                if (errno == EINTR || errno == EAGAIN) continue;
                log_err("watch: read failed (%s); watcher STOPPING -- automatic "
                        "continuous backups are now DISABLED until bkupd is restarted",
                        strerror(errno));
                stop = 1;
                break;
            }
            if (n > 0) handle_events(w, buf, (size_t)n);
        }
    }

    watch_close(w);
    ctx_free(w->cfg_ctx);
    free(w);
    return NULL;
}

void watch_start(const char *config_path)
{
    Watch *w = calloc(1, sizeof *w);
    if (!w) { log_err("watch: out of memory"); return; }
    snprintf(w->config_path, sizeof w->config_path, "%s", config_path);

    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&tid, &attr, watch_loop, w) != 0) {
        log_err("watch: pthread_create failed");
        free(w);
    }
    pthread_attr_destroy(&attr);
}
