/* Pre/post-backup hook execution. See hook.h.

   The post hook must run even when the backup die()s partway through, because
   what it undoes (a VM disk snapshot, a frozen filesystem, a database dump)
   would otherwise outlive the backup. die() is not unwindable, so the guarantee
   is made by chaining onto the thread's die handler: while a backup is armed,
   die() lands in hook_die(), which runs the post hook and then hands off to
   whatever handler was installed before (the daemon's longjmp, or none, so the
   CLI's die() proceeds to exit(1)). */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#include "hook.h"
#include "common/log.h"
#include "common/util.h"

/* The one armed backup (the op mutex allows only one at a time). Process-wide
   rather than thread-local so hook_shutdown() can reach it from the main
   thread; the strings are copies because the owning Ctx may be freed at any
   moment once the backup thread is racing an exit. Whoever takes `armed`
   under the mutex owns the post hook. */
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
typedef struct { char *name, *post; } Armed;
static Armed *armed;
static _Thread_local void (*prev_die)(void);    /* handler to chain onto */

static Armed *take_armed(void)
{
    pthread_mutex_lock(&mu);
    Armed *a = armed;
    armed = NULL;
    pthread_mutex_unlock(&mu);
    return a;
}

/* Forward the child's output to the log, one line per record, until EOF. */
static void drain(int fd, const char *which)
{
    FILE *fp = fdopen(fd, "r");
    if (!fp) { close(fd); return; }
    char line[1024];
    while (fgets(line, sizeof line, fp)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
        log_info("%s-backup: %s", which, line);
    }
    fclose(fp);
}

/* The child's environment: the parent's, minus any stale BKUP_* values, plus
   ours. Built before fork() because the daemon is multi-threaded and only
   async-signal-safe calls are allowed between fork() and exec(); setenv() is
   not one. */
extern char **environ;
static char **build_env(const char *user, const char *which, const char *status)
{
    size_t n = 0;
    while (environ[n]) n++;
    char **env = xmalloc((n + 4) * sizeof *env);
    size_t o = 0;
    for (size_t i = 0; i < n; i++)
        if (strncmp(environ[i], "BKUP_", 5) != 0) env[o++] = environ[i];
    char *e;
    if (asprintf(&e, "BKUP_USER=%s", user ? user : "") < 0) die("asprintf");
    env[o++] = e;
    if (asprintf(&e, "BKUP_HOOK=%s", which) < 0) die("asprintf");
    env[o++] = e;
    if (status) {
        if (asprintf(&e, "BKUP_STATUS=%s", status) < 0) die("asprintf");
        env[o++] = e;
    }
    env[o] = NULL;
    return env;
}

static void free_env(char **env)
{
    for (char **p = env; *p; p++)
        if (strncmp(*p, "BKUP_", 5) == 0) free(*p);   /* only ours are heap */
    free(env);
}

int hook_run(const char *cmd, const char *user, const char *which,
             const char *status)
{
    if (!cmd || !*cmd) return 0;

    int pfd[2];
    if (pipe(pfd) != 0) {
        log_err("%s-backup hook: pipe: %s", which, strerror(errno));
        return -1;
    }
    char **env = build_env(user, which, status);
    char *const argv[] = { "sh", "-c", (char *)cmd, NULL };
    long maxfd = sysconf(_SC_OPEN_MAX);
    if (maxfd < 0 || maxfd > 65536) maxfd = 65536;

    pid_t pid = fork();
    if (pid < 0) {
        log_err("%s-backup hook: fork: %s", which, strerror(errno));
        close(pfd[0]); close(pfd[1]);
        free_env(env);
        return -1;
    }
    if (pid == 0) {
        /* async-signal-safe only from here to exec */
        dup2(pfd[1], STDOUT_FILENO);
        dup2(pfd[1], STDERR_FILENO);
        /* Leave the daemon's sockets, catalog and SSH fds behind. */
        for (int fd = 3; fd < maxfd; fd++) close(fd);
        execve("/bin/sh", argv, env);
        _exit(127);
    }
    close(pfd[1]);
    free_env(env);
    drain(pfd[0], which);

    int st = 0;
    if (waitpid(pid, &st, 0) < 0) {
        log_err("%s-backup hook: waitpid: %s", which, strerror(errno));
        return -1;
    }
    if (WIFEXITED(st))   return WEXITSTATUS(st);
    if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
    return -1;
}

/* Run the post hook of a taken arming and release it. */
static void run_post(Armed *a, const char *status)
{
    if (!a) return;
    if (a->post) {
        int rc = hook_run(a->post, a->name, "post", status);
        if (rc != 0)
            log_err("post-backup hook failed (exit %d) for '%s'", rc, a->name);
    }
    free(a->name); free(a->post); free(a);
}

static void hook_die(void)
{
    log_set_thread_die(prev_die);   /* a die() from here on is not ours */
    run_post(take_armed(), "failed");
    if (prev_die) prev_die();
    /* else: die() continues to exit(1) */
}

void hook_backup_begin(const User *u)
{
    if (!u->pre_backup && !u->post_backup) return;
    Armed *a = xcalloc(1, sizeof *a);
    a->name = xstrdup(u->name);
    a->post = u->post_backup ? xstrdup(u->post_backup) : NULL;
    pthread_mutex_lock(&mu);
    armed = a;
    pthread_mutex_unlock(&mu);
    prev_die = log_get_thread_die();
    log_set_thread_die(hook_die);
    if (u->pre_backup) {
        log_info("running pre-backup hook for '%s'", u->name);
        int rc = hook_run(u->pre_backup, u->name, "pre", NULL);
        if (rc != 0)
            die("pre-backup hook failed (exit %d); backup of '%s' aborted",
                rc, u->name);
    }
}

void hook_backup_end(void)
{
    if (log_get_thread_die() != hook_die) return;   /* nothing armed here */
    log_set_thread_die(prev_die);
    run_post(take_armed(), "ok");
}

void hook_shutdown(void)
{
    Armed *a = take_armed();
    if (!a) return;
    log_warn("stopping with backup of '%s' in progress; running its "
             "post-backup hook now", a->name);
    run_post(a, "failed");
}
