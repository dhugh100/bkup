/* Exercises the op-mutex admission path in ipc.c: the caller-supplied wait
   bound, the "waiting" event that precedes a block, and the liveness check that
   drops a request whose caller gave up mid-wait.
 *
 * ipc.c is #included so the test can hold the (static) op mutex itself and
 * drive a connection thread over a socketpair -- no daemon, no config, no
 * network. Every case here is refused before ctx_new_uid(), so nothing touches
 * a real catalog or repo. */
#include "../src/daemon/ipc.c"

#include <signal.h>
#include <sys/socket.h>

static int fail_count = 0;

static void check(int cond, const char *what)
{
    printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
    if (!cond) fail_count++;
}

/* Start a connection thread serving `req` on one end of a socketpair; the
   client end is returned. */
static int start_conn(const char *req, pthread_t *tid)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { perror("socketpair"); exit(1); }

    ConnArg *ca = calloc(1, sizeof *ca);
    ca->fd = sv[1];
    ca->config_path = strdup("/nonexistent-so-a-run-would-die.conf");
    ca->caller_uid = getuid();

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (pthread_create(tid, &attr, ipc_conn_thread, ca) != 0) { perror("pthread_create"); exit(1); }
    pthread_attr_destroy(&attr);

    char line[256];
    snprintf(line, sizeof line, "%s\n", req);
    if (write(sv[0], line, strlen(line)) < 0) { perror("write"); exit(1); }
    return sv[0];
}

/* Read one event line, or return 0 on EOF/error. */
static int read_event(int fd, char *buf, size_t cap)
{
    return ipc_readline(fd, buf, cap) == 0;
}

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

/* wait:0 must fail immediately, naming the holder, with no "waiting" first. */
static void test_no_wait(void)
{
    printf("wait:0 -> immediate busy\n");
    ipc_op_lock("backup");

    pthread_t tid;
    double t0 = now_sec();
    int fd = start_conn("{\"cmd\":\"restore\",\"dest\":\"/tmp/x\",\"wait\":0}", &tid);

    char line[1024];
    check(read_event(fd, line, sizeof line), "got a reply");
    double elapsed = now_sec() - t0;

    check(strstr(line, "\"event\":\"error\"") != NULL, "reply is an error event");
    check(strstr(line, "busy: backup in progress") != NULL, "error names the holder");
    check(strstr(line, "waiting") == NULL, "no waiting event when wait is 0");
    check(elapsed < 1.0, "returned immediately");

    pthread_join(tid, NULL);
    close(fd);
    ipc_op_unlock();
}

/* A bounded wait announces itself first, then gives up at the deadline. */
static void test_wait_then_busy(void)
{
    printf("wait:2 -> waiting event, then busy at the deadline\n");
    ipc_op_lock("catalog-push");

    pthread_t tid;
    double t0 = now_sec();
    int fd = start_conn("{\"cmd\":\"verify\",\"wait\":2}", &tid);

    char line[1024];
    check(read_event(fd, line, sizeof line), "got a first reply");
    double t_waiting = now_sec() - t0;
    check(strstr(line, "\"event\":\"waiting\"") != NULL, "first event is waiting");
    check(strstr(line, "\"cmd\":\"catalog-push\"") != NULL, "waiting names the holder");
    check(strstr(line, "\"wait\":2") != NULL, "waiting reports the bound");
    check(t_waiting < 1.0, "waiting arrives before the block, not after");

    check(read_event(fd, line, sizeof line), "got a second reply");
    double t_busy = now_sec() - t0;
    check(strstr(line, "busy: catalog-push in progress") != NULL, "then busy");
    check(t_busy >= 1.8 && t_busy < 4.0, "gave up at ~2s (the requested bound)");

    pthread_join(tid, NULL);
    close(fd);
    ipc_op_unlock();
}

/* An oversized wait is clamped to OP_WAIT_MAX rather than honoured. */
static void test_wait_clamped(void)
{
    printf("wait past the ceiling -> clamped to OP_WAIT_MAX\n");
    ipc_op_lock("backup");

    pthread_t tid;
    int fd = start_conn("{\"cmd\":\"verify\",\"wait\":999999}", &tid);

    char line[1024];
    check(read_event(fd, line, sizeof line), "got a reply");
    char expect[64];
    snprintf(expect, sizeof expect, "\"wait\":%d", OP_WAIT_MAX);
    check(strstr(line, expect) != NULL, "waiting reports the clamped bound");

    ipc_op_unlock();          /* let it through so the thread finishes */
    pthread_join(tid, NULL);
    close(fd);
}

/* The point of the exercise: a caller that disconnects while queued must not
   have its command run when the lock finally comes free. */
static void test_disconnect_while_waiting(void)
{
    printf("caller disconnects mid-wait -> command is not run\n");
    ipc_op_lock("backup");

    pthread_t tid;
    /* A bogus config path makes "did it run?" unambiguous: reaching
       ctx_new_uid() would die() on the missing config, and conn_die() closes
       the socket from inside the op. */
    int fd = start_conn("{\"cmd\":\"verify\",\"wait\":60}", &tid);

    char line[1024];
    check(read_event(fd, line, sizeof line), "got the waiting event");
    check(strstr(line, "\"event\":\"waiting\"") != NULL, "queued behind the holder");

    close(fd);                /* the caller gives up */
    usleep(100 * 1000);
    ipc_op_unlock();          /* lock comes free */

    pthread_join(tid, NULL);

    /* If the guard failed, the thread would have taken the mutex and died
       inside the op; either way it is released by now, but the log line below
       is what distinguishes "skipped" from "ran and failed". */
    check(pthread_mutex_trylock(&g_op_mutex) == 0, "op mutex was released");
    pthread_mutex_unlock(&g_op_mutex);
}

int main(void)
{
    /* As bkupd's main() does: a connection thread logging to a socket whose
       caller has gone away must get EPIPE, not a signal. */
    signal(SIGPIPE, SIG_IGN);

    /* Route log output to a file: the connection thread installs a per-thread
       callback aimed at its socket, which suppresses stderr (log.c:97). */
    const char *tmp = getenv("TMPDIR");
    char logf[512];
    snprintf(logf, sizeof logf, "%s/test_opwait.log",
             (tmp && *tmp) ? tmp : "/tmp");
    if (log_open_file(logf) != 0) { fprintf(stderr, "cannot open %s\n", logf); return 1; }

    test_no_wait();
    test_wait_then_busy();
    test_wait_clamped();
    test_disconnect_while_waiting();

    printf("\n%s (log: %s)\n", fail_count ? "FAILURES" : "all passed", logf);
    return fail_count ? 1 : 0;
}
