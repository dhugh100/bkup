/* Drives the real bin/bkup binary against a stub daemon on a scratch socket,
   checking the request it builds and how it reports the reply. Uses --socket so
   nothing here can touch the live daemon, the real repo, or the event log. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>

#include "../src/common/ipcwire.h"
#include "test_common.h"

static int fail_count = 0;
static char sock_path[100];   /* sun_path is 108 bytes */

static void check(int cond, const char *what)
{
    tc_checks++;
    printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
    if (!cond) fail_count++;
}

static void check_has(const char *hay, const char *needle, const char *what)
{
    int ok = strstr(hay, needle) != NULL;
    printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) {
        fail_count++;
        printf("        wanted: %s\n        in:     %s\n", needle, hay);
    }
}

static int listen_sock(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); exit(1); }
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof a.sun_path, "%s", sock_path);
    unlink(sock_path);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0) { perror("bind"); exit(1); }
    if (listen(fd, 4) != 0) { perror("listen"); exit(1); }
    return fd;
}

/* Run bin/bkup with `args`, serving it `reply` (a NULL-terminated list of event
   lines). The request it sent is copied into `req`; its exit code is returned
   and its stderr is copied into `err`. */
static int run_cli(char *const args[], const char *const *reply,
                   char *req, size_t reqcap, char *err, size_t errcap)
{
    int srv = listen_sock();

    int errpipe[2];
    if (pipe(errpipe) != 0) { perror("pipe"); exit(1); }

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); exit(1); }
    if (pid == 0) {
        close(srv);
        close(errpipe[0]);
        dup2(errpipe[1], STDERR_FILENO);
        dup2(errpipe[1], STDOUT_FILENO);
        close(errpipe[1]);
        execv(args[0], args);
        _exit(127);
    }
    close(errpipe[1]);

    int c = accept(srv, NULL, NULL);
    if (c < 0) { perror("accept"); exit(1); }
    req[0] = '\0';
    if (ipc_readline(c, req, reqcap) != 0) snprintf(req, reqcap, "(no request)");
    for (int i = 0; reply[i]; i++) ipc_send(c, reply[i]);
    close(c);
    close(srv);
    unlink(sock_path);

    size_t n = 0;
    ssize_t r;
    while (n < errcap - 1 && (r = read(errpipe[0], err + n, errcap - 1 - n)) > 0)
        n += (size_t)r;
    err[n] = '\0';
    close(errpipe[0]);

    int st;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) { }
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static void test_restore_request(void)
{
    printf("restore: flags become one request\n");
    char *args[] = { "bin/bkup", "--socket", sock_path, "--wait", "30", "restore",
                     "-p", "/home/u/Docs", "-f", "/home/u/notes.txt",
                     "-A", "1700000000", "/tmp/out", NULL };
    const char *reply[] = { "{\"event\":\"log\",\"level\":\"I\",\"msg\":\"restored 2 file(s)\"}",
                            "{\"event\":\"done\"}", NULL };
    char req[8192], err[8192];
    int rc = run_cli(args, reply, req, sizeof req, err, sizeof err);

    check_has(req, "\"cmd\":\"restore\"", "sends the restore verb");
    check_has(req, "\"dest\":\"/tmp/out\"", "carries the destination");
    check_has(req, "\"dirs\":[\"/home/u/Docs\"]", "-p becomes the dirs array");
    check_has(req, "\"files\":[\"/home/u/notes.txt\"]", "-f becomes the files array");
    check_has(req, "\"asof\":1700000000", "-A becomes asof");
    check_has(req, "\"wait\":30", "--wait is passed through");
    check(strstr(req, "\"snapshot\"") == NULL, "no snapshot key when -s is absent");
    check_has(err, "restored 2 file(s)", "log events reach the terminal");
    check(rc == 0, "exits 0 on done");
}

static void test_relative_dest(void)
{
    printf("restore: a relative dest is resolved for the daemon\n");
    char *args[] = { "bin/bkup", "--socket", sock_path, "restore", "out", NULL };
    const char *reply[] = { "{\"event\":\"done\"}", NULL };
    char req[8192], err[8192];
    run_cli(args, reply, req, sizeof req, err, sizeof err);

    char cwd[4096], want[4352];
    if (!getcwd(cwd, sizeof cwd)) { perror("getcwd"); exit(1); }
    snprintf(want, sizeof want, "\"dest\":\"%s/out\"", cwd);
    check_has(req, want, "dest is absolute (the daemon has a different cwd)");
}

static void test_multi_target(void)
{
    printf("restore: repeated -p/-f accumulate\n");
    char *args[] = { "bin/bkup", "--socket", sock_path, "restore",
                     "-p", "/a", "-p", "/b", "-f", "/c.txt", "-s", "7",
                     "/tmp/out", NULL };
    const char *reply[] = { "{\"event\":\"done\"}", NULL };
    char req[8192], err[8192];
    run_cli(args, reply, req, sizeof req, err, sizeof err);

    check_has(req, "\"dirs\":[\"/a\",\"/b\"]", "both -p targets, in order");
    check_has(req, "\"files\":[\"/c.txt\"]", "the -f target");
    check_has(req, "\"snapshot\":7", "-s becomes snapshot");
}

static void test_waiting_and_busy(void)
{
    printf("queued request: waiting is reported, busy fails\n");
    char *args[] = { "bin/bkup", "--socket", sock_path, "backup", NULL };
    const char *reply[] = { "{\"event\":\"waiting\",\"cmd\":\"backup\",\"wait\":120}",
                            "{\"event\":\"error\",\"msg\":\"busy: backup in progress\"}",
                            NULL };
    char req[8192], err[8192];
    int rc = run_cli(args, reply, req, sizeof req, err, sizeof err);

    check_has(req, "\"cmd\":\"backup\"", "sends the backup verb");
    check(strstr(req, "\"wait\"") == NULL, "no wait key without --wait");
    check_has(err, "waiting for backup", "the wait is announced, not silent");
    check_has(err, "busy: backup in progress", "the busy reason is shown");
    check(rc != 0, "exits non-zero on error");
}

static void test_prune_flags(void)
{
    printf("prune: keep flags map to request fields\n");
    char *args[] = { "bin/bkup", "--socket", sock_path, "prune",
                     "--keep-last", "5", "--keep-monthly", "3", NULL };
    const char *reply[] = { "{\"event\":\"done\"}", NULL };
    char req[8192], err[8192];
    int rc = run_cli(args, reply, req, sizeof req, err, sizeof err);

    check_has(req, "\"keep_last\":5", "--keep-last");
    check_has(req, "\"keep_monthly\":3", "--keep-monthly");
    check(rc == 0, "exits 0");
}

/* No stub daemon here: the point is the message when nothing is listening. */
static void test_no_daemon(void)
{
    printf("no daemon: the error names the real problem\n");
    char missing[112];
    snprintf(missing, sizeof missing, "%s.absent", sock_path);
    unlink(missing);

    char cmdline[1024];
    snprintf(cmdline, sizeof cmdline,
             "bin/bkup --socket %s backup 2>&1", missing);
    FILE *p = popen(cmdline, "r");
    if (!p) { perror("popen"); exit(1); }
    char out[4096];
    size_t n = fread(out, 1, sizeof out - 1, p);
    out[n] = '\0';
    int rc = pclose(p);

    check_has(out, "cannot reach bkupd", "says the daemon is unreachable");
    check(strstr(out, "log file") == NULL, "does not blame the log file");
    check(rc != 0, "exits non-zero");
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);

    if (access("bin/bkup", X_OK) != 0) {
        fprintf(stderr, "run from the repo root after `make all`\n");
        return 1;
    }
    snprintf(sock_path, sizeof sock_path, "/tmp/bkup-test-client-%d.sock",
             (int)getpid());

    test_restore_request();
    test_relative_dest();
    test_multi_target();
    test_waiting_and_busy();
    test_prune_flags();
    test_no_daemon();

    TEST_REPORT("test_client", fail_count, tc_checks);
}
