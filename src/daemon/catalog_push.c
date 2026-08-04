#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#include "catalog_push.h"

#define CP_MAX          64
#define CP_MIN_INTERVAL 300     /* seconds: push a user's catalog at most this often */

static pthread_mutex_t cp_mtx = PTHREAD_MUTEX_INITIALIZER;
static struct {
    char          user[128];
    unsigned long dirty_seq;    /* bumped by each continuous backup after its COMMIT */
    unsigned long pushed_seq;   /* highest seq durably uploaded to the server  */
    time_t        last_push;    /* when we last successfully uploaded          */
} cp[CP_MAX];
static int cp_n;

static time_t real_now(void) { return time(NULL); }
static time_t (*cp_clock)(void) = real_now;

void catalog_push_set_clock(time_t (*fn)(void)) { cp_clock = fn ? fn : real_now; }

/* Find (or create) the slot for `user`. Caller holds cp_mtx. Returns -1 if the
   table is full -- the daemon serves a small, fixed set of users, so this only
   trips on misconfiguration, and dropping the batch just falls back to the
   next full backup/prune pushing the catalog. */
static int cp_find(const char *user)
{
    for (int i = 0; i < cp_n; i++)
        if (!strcmp(cp[i].user, user)) return i;
    if (cp_n >= CP_MAX) return -1;
    snprintf(cp[cp_n].user, sizeof cp[cp_n].user, "%s", user);
    cp[cp_n].dirty_seq = cp[cp_n].pushed_seq = 0;
    cp[cp_n].last_push = 0;
    return cp_n++;
}

void catalog_mark_dirty(const char *user)
{
    pthread_mutex_lock(&cp_mtx);
    int i = cp_find(user);
    if (i >= 0) cp[i].dirty_seq++;
    pthread_mutex_unlock(&cp_mtx);
}

int catalog_take_pending(const char *user, unsigned long *seq_out)
{
    int need = 0;
    pthread_mutex_lock(&cp_mtx);
    int i = cp_find(user);
    if (i >= 0 && cp[i].dirty_seq != cp[i].pushed_seq &&
        cp_clock() - cp[i].last_push >= CP_MIN_INTERVAL) {
        *seq_out = cp[i].dirty_seq;      /* capture BEFORE the upload */
        need = 1;
    }
    pthread_mutex_unlock(&cp_mtx);
    return need;
}

void catalog_mark_pushed(const char *user, unsigned long seq)
{
    pthread_mutex_lock(&cp_mtx);
    int i = cp_find(user);
    if (i >= 0 && seq > cp[i].pushed_seq) {
        cp[i].pushed_seq = seq;
        cp[i].last_push  = cp_clock();
    }
    pthread_mutex_unlock(&cp_mtx);
}

void catalog_clear(const char *user)
{
    pthread_mutex_lock(&cp_mtx);
    int i = cp_find(user);
    if (i >= 0) {
        cp[i].pushed_seq = cp[i].dirty_seq;
        cp[i].last_push  = cp_clock();
    }
    pthread_mutex_unlock(&cp_mtx);
}
