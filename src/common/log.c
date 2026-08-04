#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>

#include "log.h"

static _Thread_local void (*tls_cb)(const char *, const char *, void *) = NULL;
static _Thread_local void *tls_cb_ud = NULL;
static _Thread_local void (*tls_die)(void) = NULL;

/* Set by die_perm(), cleared by die() and by log_take_permanent(): the thread's
   most recent fatal was a permanent (non-retryable) failure. */
static _Thread_local int tls_perm = 0;

/* Durable event log, shared by all threads.  NULL = not opened yet. */
static FILE           *log_fp = NULL;
static pthread_mutex_t  log_mu = PTHREAD_MUTEX_INITIALIZER;

/* Best-effort create of the immediate parent directory of `path`. */
static void make_parent_dir(const char *path)
{
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (!slash || slash == dir) return;
    *slash = '\0';
    mkdir(dir, 0775);   /* ignore EEXIST and any failure; fopen below decides */
}

int log_open_file(const char *path)
{
    if (!path || !path[0]) { errno = EINVAL; return -1; }
    make_parent_dir(path);
    FILE *f = fopen(path, "a");
    if (!f) return -1;              /* errno set by fopen; caller treats as fatal */
    /* Group-writable so a non-root CLI run can append to the daemon's log. */
    fchmod(fileno(f), 0664);
    setvbuf(f, NULL, _IOLBF, 0);    /* flush each line for crash durability */
    pthread_mutex_lock(&log_mu);
    if (log_fp) fclose(log_fp);
    log_fp = f;
    pthread_mutex_unlock(&log_mu);
    return 0;
}

void log_close_file(void)
{
    pthread_mutex_lock(&log_mu);
    if (log_fp) { fclose(log_fp); log_fp = NULL; }
    pthread_mutex_unlock(&log_mu);
}

void log_set_thread_cb(void (*cb)(const char *lvl, const char *msg, void *ud),
                       void *ud)
{
    tls_cb = cb;
    tls_cb_ud = ud;
}

void log_set_thread_die(void (*fn)(void))
{
    tls_die = fn;
}

static void vlog(const char *lvl, const char *fmt, va_list ap)
{
    char msg[2048];
    vsnprintf(msg, sizeof msg, fmt, ap);

    char ts[32];
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);

    /* Durable file sink first, always: the daemon redirects per-connection
       output to a thread cb (GUI socket), so writing here -- before that
       branch -- is what captures CLI, GUI, and scheduled events alike. */
    pthread_mutex_lock(&log_mu);
    if (log_fp) {
        fprintf(log_fp, "%s [%s] %s\n", ts, lvl, msg);
        fflush(log_fp);   /* keep the "durable" log durable, and keep the buffer
                             empty so a fork()ed child that _exit()s (e.g. the
                             privilege-dropped restore writer) neither loses its
                             own lines nor duplicates the parent's. */
    }
    pthread_mutex_unlock(&log_mu);

    if (tls_cb) {
        tls_cb(lvl, msg, tls_cb_ud);
        return;
    }
    fprintf(stderr, "%s [%s] %s\n", ts, lvl, msg);
}

void log_info(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog("I", fmt, ap);
    va_end(ap);
}

void log_warn(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog("W", fmt, ap);
    va_end(ap);
}

void log_err(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog("E", fmt, ap);
    va_end(ap);
}

int log_take_permanent(void)
{
    int p = tls_perm;
    tls_perm = 0;
    return p;
}

void die(const char *fmt, ...)
{
    tls_perm = 0;                  /* transient unless die_perm() says otherwise */
    va_list ap;
    va_start(ap, fmt);
    vlog("F", fmt, ap);
    va_end(ap);
    if (tls_die) tls_die();
    exit(1);
}

void die_perm(const char *fmt, ...)
{
    tls_perm = 1;
    va_list ap;
    va_start(ap, fmt);
    vlog("F", fmt, ap);
    va_end(ap);
    if (tls_die) tls_die();
    exit(1);
}
