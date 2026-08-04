#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "userstate.h"
#include "common/log.h"

#define US_MAX 64               /* the daemon serves a small, fixed set of users */

static pthread_mutex_t us_mtx = PTHREAD_MUTEX_INITIALIZER;
static char us_off[US_MAX][128];
static int  us_n;

/* Caller holds us_mtx. Index of `user` in us_off[], or -1. */
static int us_find(const char *user)
{
    for (int i = 0; i < us_n; i++)
        if (!strcmp(us_off[i], user)) return i;
    return -1;
}

void userstate_disable(const char *user, const char *reason)
{
    pthread_mutex_lock(&us_mtx);
    if (us_find(user) < 0 && us_n < US_MAX) {
        snprintf(us_off[us_n], sizeof us_off[us_n], "%s", user);
        us_n++;
        log_err("automatic backups for '%s' DISABLED: %s. Fix the cause, then "
                "reload (SIGHUP) or restart bkupd to re-enable.", user, reason);
    }
    pthread_mutex_unlock(&us_mtx);
}

int userstate_disabled(const char *user)
{
    pthread_mutex_lock(&us_mtx);
    int r = us_find(user) >= 0;
    pthread_mutex_unlock(&us_mtx);
    return r;
}

void userstate_enable(const char *user)
{
    pthread_mutex_lock(&us_mtx);
    int i = us_find(user);
    if (i >= 0) {
        /* compact: move the last entry into the freed slot (memmove tolerates
           the self-overlap when the removed entry is itself the last) */
        memmove(us_off[i], us_off[us_n - 1], sizeof us_off[i]);
        us_n--;
        log_info("automatic backups for '%s' re-enabled", user);
    }
    pthread_mutex_unlock(&us_mtx);
}

void userstate_reset(void)
{
    pthread_mutex_lock(&us_mtx);
    if (us_n > 0) {
        log_info("re-enabling automatic backups for all users (reload)");
        us_n = 0;
    }
    pthread_mutex_unlock(&us_mtx);
}
