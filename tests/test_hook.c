/* Exercises the pre/post-backup hook runner (cli/hook.c): exit-status
   mapping, the environment a hook sees, output forwarding into the log, and
   the one guarantee that matters -- the post hook runs exactly once whether
   the backup ends normally, die()s midway, or never starts because the pre
   hook refused. die() is caught with a thread die handler that longjmps, the
   same way the daemon's scheduler catches it. */
#include <pthread.h>
#include <setjmp.h>

#include "test_common.h"
#include "cli/hook.h"
#include "common/log.h"

static int fails;

static jmp_buf jb;
static int die_reached;
static void catch_die(void) { die_reached++; longjmp(jb, 1); }

/* Log capture: count lines whose text contains a marker. */
static char logbuf[8192];
static void log_cb(const char *lvl, const char *msg, void *ud)
{
    (void)lvl; (void)ud;
    size_t n = strlen(logbuf);
    snprintf(logbuf + n, sizeof logbuf - n, "%s\n", msg);
}
static int log_count(const char *needle)
{
    int n = 0;
    for (const char *p = logbuf; (p = strstr(p, needle)); p += strlen(needle)) n++;
    return n;
}

static char *slurp(const char *path)
{
    static char buf[256];
    memset(buf, 0, sizeof buf);
    FILE *fp = fopen(path, "r");
    if (!fp) return buf;
    size_t n = fread(buf, 1, sizeof buf - 1, fp);
    fclose(fp);
    while (n > 0 && buf[n - 1] == '\n') buf[--n] = '\0';
    return buf;
}

/* A backup thread that arms, then waits to be told to finish or die. */
static User *thr_user;
static int thr_mode;           /* 0 = hook_backup_end, 1 = die() */
static pthread_mutex_t thr_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  thr_cv = PTHREAD_COND_INITIALIZER;
static int thr_armed, thr_go;
static void thr_die(void) { pthread_exit(NULL); }
static void *backup_thread(void *arg)
{
    (void)arg;
    log_set_thread_cb(log_cb, NULL);
    log_set_thread_die(thr_die);
    hook_backup_begin(thr_user);
    pthread_mutex_lock(&thr_mu);
    thr_armed = 1; pthread_cond_broadcast(&thr_cv);
    while (!thr_go) pthread_cond_wait(&thr_cv, &thr_mu);
    pthread_mutex_unlock(&thr_mu);
    if (thr_mode == 1) die("synthetic");
    hook_backup_end();
    return NULL;
}
static void run_shutdown_case(User *u, int mode, const char *out)
{
    pthread_t t;
    thr_user = u; thr_mode = mode; thr_armed = thr_go = 0;
    unlink(out);
    pthread_create(&t, NULL, backup_thread, NULL);
    pthread_mutex_lock(&thr_mu);
    while (!thr_armed) pthread_cond_wait(&thr_cv, &thr_mu);
    pthread_mutex_unlock(&thr_mu);

    hook_shutdown();                       /* main thread: daemon stopping */
    CHECKEQ_STR(slurp(out), "post:failed");

    pthread_mutex_lock(&thr_mu);
    thr_go = 1; pthread_cond_broadcast(&thr_cv);
    pthread_mutex_unlock(&thr_mu);
    pthread_join(t, NULL);
    CHECKEQ_STR(slurp(out), "post:failed");   /* not run a second time */
}

int main(void)
{
    char dir[] = "/tmp/test_hook.XXXXXX";
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    char out[512], pre[1024], post[1024];
    snprintf(out, sizeof out, "%s/out", dir);

    log_set_thread_cb(log_cb, NULL);

    /* ---- hook_run: status mapping and environment ---- */
    CHECKEQ_INT(hook_run(NULL, "u", "pre", NULL), 0);
    CHECKEQ_INT(hook_run("", "u", "pre", NULL), 0);
    CHECKEQ_INT(hook_run("exit 3", "u", "pre", NULL), 3);
    CHECKEQ_INT(hook_run("kill -TERM $$", "u", "pre", NULL), 128 + 15);

    snprintf(pre, sizeof pre, "echo \"$BKUP_USER/$BKUP_HOOK/${BKUP_STATUS-unset}\" > '%s'", out);
    setenv("BKUP_STATUS", "stale-from-parent", 1);   /* must not leak through */
    CHECKEQ_INT(hook_run(pre, "alice", "pre", NULL), 0);
    CHECKEQ_STR(slurp(out), "alice/pre/unset");
    CHECKEQ_INT(hook_run(pre, "alice", "post", "failed"), 0);
    CHECKEQ_STR(slurp(out), "alice/post/failed");

    /* stdout and stderr both land in the log, tagged by hook */
    logbuf[0] = '\0';
    CHECKEQ_INT(hook_run("echo hello-out; echo hello-err >&2", "u", "pre", NULL), 0);
    CHECKEQ_INT(log_count("pre-backup: hello-out"), 1);
    CHECKEQ_INT(log_count("pre-backup: hello-err"), 1);

    /* ---- begin/end bracket ---- */
    User u; memset(&u, 0, sizeof u);
    u.name = "alice";
    snprintf(post, sizeof post, "echo \"post:$BKUP_STATUS\" >> '%s'", out);
    u.post_backup = post;

    /* normal completion: post once, status ok, handler restored */
    unlink(out);
    u.pre_backup = "true";
    log_set_thread_die(catch_die);
    hook_backup_begin(&u);
    hook_backup_end();
    CHECKEQ_STR(slurp(out), "post:ok");
    CHECK(log_get_thread_die() == catch_die);

    /* die() after end must not run post again */
    die_reached = 0;
    if (setjmp(jb) == 0) die("synthetic");
    CHECKEQ_INT(die_reached, 1);
    CHECKEQ_STR(slurp(out), "post:ok");

    /* die() midway: post runs with status failed, then the prior handler */
    unlink(out);
    die_reached = 0;
    if (setjmp(jb) == 0) {
        hook_backup_begin(&u);
        die("synthetic mid-backup failure");
        CHECK(0 && "die returned");
    }
    CHECKEQ_INT(die_reached, 1);
    CHECKEQ_STR(slurp(out), "post:failed");
    CHECK(log_get_thread_die() == catch_die);

    /* pre refuses: backup die()s, post still runs (status failed), once */
    unlink(out);
    die_reached = 0;
    logbuf[0] = '\0';
    u.pre_backup = "echo refusing; exit 7";
    if (setjmp(jb) == 0) {
        hook_backup_begin(&u);
        CHECK(0 && "begin returned despite failing pre hook");
    }
    CHECKEQ_INT(die_reached, 1);
    CHECKEQ_STR(slurp(out), "post:failed");
    CHECKEQ_INT(log_count("pre-backup: refusing"), 1);
    CHECKEQ_INT(log_count("exit 7"), 1);
    CHECK(log_get_thread_die() == catch_die);

    /* a failing post is logged, not fatal */
    logbuf[0] = '\0';
    u.pre_backup = NULL;
    u.post_backup = "exit 5";
    hook_backup_begin(&u);
    hook_backup_end();
    CHECKEQ_INT(log_count("post-backup hook failed (exit 5)"), 1);

    /* daemon shutdown while a backup is armed on another thread: post runs
       once from the shutting-down thread, and the backup thread's own end or
       die() afterwards must not run it again */
    u.pre_backup = "true";
    u.post_backup = post;
    run_shutdown_case(&u, 0, out);
    run_shutdown_case(&u, 1, out);

    /* nothing armed: shutdown is a no-op */
    unlink(out);
    hook_shutdown();
    CHECKEQ_STR(slurp(out), "");

    /* no hooks configured: begin/end are no-ops and leave the handler alone */
    u.pre_backup = NULL;
    u.post_backup = NULL;
    hook_backup_begin(&u);
    CHECK(log_get_thread_die() == catch_die);
    hook_backup_end();

    log_set_thread_die(NULL);
    unlink(out);
    rmdir(dir);
    TEST_DONE("test_hook");
}
