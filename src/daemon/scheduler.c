#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <setjmp.h>
#include <pthread.h>
#include <unistd.h>

#include "scheduler.h"
#include "ipc.h"
#include "catalog_push.h"
#include "userstate.h"
#include "cli/commands.h"
#include "cli/ctx.h"
#include "common/config.h"
#include "common/db.h"
#include "common/log.h"

enum { S_NONE, S_HOURLY, S_DAILY, S_WEEKLY, S_MONTHLY };

typedef struct {
    int kind;
    int hour, min;   /* time of day */
    int dow;         /* 0=Sun..6=Sat, for weekly */
    int dom;         /* 1..28, for monthly */
} Sched;

/* Parse "hourly" | "daily [HH:MM]" | "weekly [HH:MM]" | "monthly [HH:MM]".
   Defaults: time 00:00, weekly on Monday, monthly on the 1st. */
static void parse_sched(const char *s, Sched *out)
{
    memset(out, 0, sizeof *out);
    out->kind = S_NONE;
    out->dow = 1;     /* Monday */
    out->dom = 1;
    if (!s || !s[0]) return;

    char buf[128];
    snprintf(buf, sizeof buf, "%s", s);

    char *save = NULL;
    char *tok = strtok_r(buf, " \t", &save);
    if (!tok) return;

    if      (!strcmp(tok, "hourly"))  out->kind = S_HOURLY;
    else if (!strcmp(tok, "daily"))   out->kind = S_DAILY;
    else if (!strcmp(tok, "weekly"))  out->kind = S_WEEKLY;
    else if (!strcmp(tok, "monthly")) out->kind = S_MONTHLY;
    else { log_warn("scheduler: unknown schedule '%s'", s); return; }

    /* Optional HH:MM token. */
    while ((tok = strtok_r(NULL, " \t", &save))) {
        if (strchr(tok, ':')) {
            int h = 0, m = 0;
            if (sscanf(tok, "%d:%d", &h, &m) == 2) { out->hour = h; out->min = m; }
        }
    }
}

/* Smallest time strictly after `ref` matching the schedule. */
static time_t next_due(const Sched *s, time_t ref)
{
    struct tm tm;
    localtime_r(&ref, &tm);
    tm.tm_sec = 0;

    if (s->kind == S_HOURLY) {
        tm.tm_min = 0;
        time_t t = mktime(&tm);
        if (t <= ref) t += 3600;
        return t;
    }

    tm.tm_hour = s->hour;
    tm.tm_min  = s->min;

    if (s->kind == S_DAILY) {
        time_t t = mktime(&tm);
        if (t <= ref) t += 24 * 3600;
        return t;
    }
    if (s->kind == S_WEEKLY) {
        time_t t = mktime(&tm);
        struct tm c; localtime_r(&t, &c);
        int delta = (s->dow - c.tm_wday + 7) % 7;
        t += delta * 24 * 3600;
        if (t <= ref) t += 7 * 24 * 3600;
        return t;
    }
    if (s->kind == S_MONTHLY) {
        tm.tm_mday = s->dom;
        time_t t = mktime(&tm);
        if (t <= ref) {
            struct tm n = tm;
            n.tm_mon += 1;          /* mktime normalizes year rollover */
            t = mktime(&n);
        }
        return t;
    }
    return ref + 365L * 24 * 3600;  /* S_NONE: effectively never */
}

/* ---- running a scheduled op (die() must not kill the daemon) ---- */

static _Thread_local jmp_buf g_sched_jb;
static void sched_die(void) { longjmp(g_sched_jb, 1); }

static void run_backup(const char *config_path, const char *source)
{
    if (userstate_disabled(source)) return;
    Ctx *c = ctx_new_nosel(config_path);
    ctx_use_user(c, source);
    log_set_thread_die(sched_die);
    ipc_op_lock("backup");
    if (setjmp(g_sched_jb) == 0) {
        log_info("scheduler: backup '%s' started", source);
        cmd_backup(c, 0, NULL);
        /* a full backup uploaded the whole catalog -> no continuous debt remains */
        catalog_clear(source);
        userstate_enable(source);   /* success clears any prior permanent latch */
        log_info("scheduler: backup '%s' completed", source);
    } else {
        log_err("scheduler: backup '%s' FAILED -- see the [F] line above for "
                "the cause; this source was NOT backed up", source);
        if (log_take_permanent())
            userstate_disable(source, "backup failed with a permanent error");
    }
    ipc_op_unlock();
    log_set_thread_die(NULL);
    ctx_free(c);
}

static void run_prune(const char *config_path, const char *source)
{
    if (userstate_disabled(source)) return;
    Ctx *c = ctx_new_nosel(config_path);
    ctx_use_user(c, source);

    /* Build prune args from the source's retention config. */
    char b[5][16];
    char *argv[10];
    int argc = 0;
    User *s = c->src;
    if (s->keep_last > 0)    { snprintf(b[0], 16, "%d", s->keep_last);    argv[argc++] = "--keep-last";    argv[argc++] = b[0]; }
    if (s->keep_daily > 0)   { snprintf(b[1], 16, "%d", s->keep_daily);   argv[argc++] = "--keep-daily";   argv[argc++] = b[1]; }
    if (s->keep_weekly > 0)  { snprintf(b[2], 16, "%d", s->keep_weekly);  argv[argc++] = "--keep-weekly";  argv[argc++] = b[2]; }
    if (s->keep_monthly > 0) { snprintf(b[3], 16, "%d", s->keep_monthly); argv[argc++] = "--keep-monthly"; argv[argc++] = b[3]; }
    if (s->keep_yearly > 0)  { snprintf(b[4], 16, "%d", s->keep_yearly);  argv[argc++] = "--keep-yearly";  argv[argc++] = b[4]; }

    if (argc == 0) {
        log_warn("scheduled prune of '%s' skipped: no keep-* rules configured", source);
        ctx_free(c);
        return;
    }

    log_set_thread_die(sched_die);
    ipc_op_lock("prune");
    if (setjmp(g_sched_jb) == 0) {
        log_info("scheduler: prune '%s' started", source);
        cmd_prune(c, argc, argv);
        /* prune uploaded the whole catalog -> no continuous debt remains */
        catalog_clear(source);
        userstate_enable(source);   /* success clears any prior permanent latch */
        log_info("scheduler: prune '%s' completed", source);
    } else {
        log_err("scheduler: prune '%s' FAILED -- see the [F] line above for "
                "the cause", source);
        if (log_take_permanent())
            userstate_disable(source, "prune failed with a permanent error");
    }
    ipc_op_unlock();
    log_set_thread_die(NULL);
    ctx_free(c);
}

/* Flush a user's catalog if continuous backups have batched changes and the throttle
   interval has elapsed. Serialized against backup/prune via the op lock, so the
   catalog/latest pointer is never raced. Cheap no-op when nothing is pending. */
static void run_catalog_push(const char *config_path, const char *source)
{
    if (userstate_disabled(source)) return;
    unsigned long seq;
    if (!catalog_take_pending(source, &seq))
        return;

    Ctx *c = ctx_new_nosel(config_path);
    ctx_use_user(c, source);
    log_set_thread_die(sched_die);
    ipc_op_lock("catalog-push");
    if (setjmp(g_sched_jb) == 0) {
        ctx_open_db(c);
        ctx_connect(c);
        key_from_meta(c);
        /* snap id is only the remote filename; the upload VACUUMs the whole
           catalog, so any recent id naming a complete catalog is fine. */
        sqlite3_stmt *q = db_prep(c->db,
            "SELECT MAX(id) FROM snapshots WHERE state=1");
        long long snap = (sqlite3_step(q) == SQLITE_ROW)
                       ? sqlite3_column_int64(q, 0) : 0;
        sqlite3_finalize(q);

        upload_catalog(c, snap);
        catalog_mark_pushed(source, seq);   /* only on success */
        userstate_enable(source);           /* success clears any prior latch */
        log_info("continuous backup: catalog synced to server for '%s' "
                 "(routine, nothing to do)", source);
    } else {
        /* pushed_seq untouched -> stays queued, retried on a later tick (unless
           the failure was permanent, in which case we latch the user off so we
           do not re-drive a hopeless push every 60s forever). */
        if (log_take_permanent())
            userstate_disable(source, "catalog sync failed with a permanent error");
        else
            log_warn("continuous backup: catalog sync for '%s' failed -- still "
                     "queued, retries automatically (no action needed, do not "
                     "resubmit)", source);
    }
    ipc_op_unlock();
    log_set_thread_die(NULL);
    ctx_free(c);
}

/* ---- startup passphrase validation (offline) ---- */

/* Verify, at daemon start, that the configured passphrase actually decrypts a
   user's repo, so a wrong key_file is caught once here instead of failing every
   scheduled/continuous op afterward. The keycheck lives in the LOCAL catalog,
   so this needs no server connection. Only initialized repos (those that have a
   keycheck) are checked; a never-initialized user is left enabled so its first
   backup can auto-init as usual. A permanent failure (wrong passphrase, bad
   key_file) latches the user off; a transient one is ignored (retried later). */
static void validate_user(const char *config_path, const char *source)
{
    Ctx *c = ctx_new_nosel(config_path);
    ctx_use_user(c, source);
    log_set_thread_die(sched_die);
    if (setjmp(g_sched_jb) == 0) {
        ctx_open_db(c);
        char *kc = db_meta_get(c->db, "keycheck");
        if (kc) {
            free(kc);
            key_from_meta(c);     /* derives key + verifies keycheck; dies on wrong pass */
            log_info("scheduler: passphrase verified for '%s'", source);
        }
    } else {
        if (log_take_permanent())
            userstate_disable(source,
                "passphrase/keycheck verification failed at startup");
    }
    log_set_thread_die(NULL);
    ctx_free(c);
}

/* ---- scheduler thread ---- */

typedef struct {
    char  config_path[1024];
} SchedArg;

#define SCHED_MAX 64

static void *sched_loop(void *arg)
{
    SchedArg *sa = arg;

    /* The root daemon serves every user; schedule all users' users. */
    Ctx *c = ctx_new_nosel(sa->config_path);
    int n = c->cfg->nusers;
    if (n > SCHED_MAX) n = SCHED_MAX;

    char  names[SCHED_MAX][128];
    Sched bsched[SCHED_MAX], psched[SCHED_MAX];
    time_t lastb[SCHED_MAX], lastp[SCHED_MAX];
    int    any = 0;
    time_t now0 = time(NULL);

    for (int i = 0; i < n; i++) {
        User *s = &c->cfg->users[i];
        snprintf(names[i], sizeof names[i], "%s", s->name);
        parse_sched(s->backup_sched, &bsched[i]);
        parse_sched(s->prune_sched,  &psched[i]);
        lastb[i] = lastp[i] = now0;   /* anchor: first run at next occurrence */
        if (bsched[i].kind != S_NONE || psched[i].kind != S_NONE) {
            any = 1;
            log_info("scheduler: source '%s' backup=%s prune=%s", s->name,
                     s->backup_sched ? s->backup_sched : "(none)",
                     s->prune_sched  ? s->prune_sched  : "(none)");
        }
    }
    ctx_free(c);

    if (!any)
        log_info("scheduler: no backup/prune schedules; ticking for continuous "
                 "catalog-push only");

    /* Check every user's passphrase up front so a wrong key_file is reported
       once and the user latched off, rather than spamming [F] every tick. */
    for (int i = 0; i < n; i++)
        validate_user(sa->config_path, names[i]);

    for (;;) {
        sleep(60);
        time_t now = time(NULL);
        for (int i = 0; i < n; i++) {
            if (bsched[i].kind != S_NONE && now >= next_due(&bsched[i], lastb[i])) {
                lastb[i] = now;
                run_backup(sa->config_path, names[i]);
            }
            if (psched[i].kind != S_NONE && now >= next_due(&psched[i], lastp[i])) {
                lastp[i] = now;
                run_prune(sa->config_path, names[i]);
            }
        }
        /* Flush any catalog batched by continuous backups (throttled per user;
           no-op when nothing is pending). Runs for every user, not just
           those with a backup/prune schedule. */
        for (int i = 0; i < n; i++)
            run_catalog_push(sa->config_path, names[i]);
    }
    free(sa);
    return NULL;
}

void scheduler_start(const char *config_path)
{
    SchedArg *sa = malloc(sizeof *sa);
    snprintf(sa->config_path, sizeof sa->config_path, "%s", config_path);

    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&tid, &attr, sched_loop, sa) != 0) {
        log_err("scheduler: pthread_create failed");
        free(sa);
    }
    pthread_attr_destroy(&attr);
}
