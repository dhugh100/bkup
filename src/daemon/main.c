#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <pthread.h>

#include "ipc.h"
#include "scheduler.h"
#include "userstate.h"
#include "watch.h"
#include "common/config.h"
#include "common/log.h"
#include "common/util.h"

static volatile sig_atomic_t g_stop   = 0;
static volatile sig_atomic_t g_reload = 0;

static void on_stop(int sig)   { (void)sig; g_stop   = 1; }
static void on_reload(int sig) { (void)sig; g_reload = 1; }

/* Returns 1 if a live daemon is already listening on sock_path (connect
   succeeds), 0 if the path is free or holds a stale/dead socket. */
static int socket_in_use(const char *sock_path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", sock_path);
    int live = (connect(fd, (struct sockaddr *)&addr, sizeof addr) == 0);
    close(fd);
    return live;
}

static char *default_sock_path(void)
{
    /* Shared with the CLI client, which has no config to read the path from. */
    char *buf = xmalloc(256);
    snprintf(buf, 256, "%s", BKUPD_SOCK_PATH);
    return buf;
}

int main(int argc, char **argv)
{
    const char *config_path = NULL;
    const char *sock_path   = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-c") && i + 1 < argc) config_path = argv[++i];
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) sock_path = argv[++i];
    }
    if (!config_path) {
        fprintf(stderr, "usage: bkupd -c CONFIG [-s SOCKET]\n");
        return 1;
    }

    /* Open the durable event log before doing anything else.  An unattended
       daemon that cannot record what it does is worse than no daemon: refuse
       to start so the failure is visible (systemd marks the unit failed). */
    Config *bootcfg = config_load(config_path);
    if (log_open_file(bootcfg->log_file) != 0) {
        fprintf(stderr, "bkupd: cannot open log file %s: %s\n",
                bootcfg->log_file, strerror(errno));
        return 1;
    }
    config_free(bootcfg);

    char *sp_alloc = NULL;
    if (!sock_path) { sp_alloc = default_sock_path(); sock_path = sp_alloc; }

    /* Refuse to start if another daemon is already serving this socket;
       only remove the path if it is stale (dead/unconnectable). */
    if (socket_in_use(sock_path)) {
        fprintf(stderr,
                "bkupd: another daemon is already listening on %s\n", sock_path);
        free(sp_alloc);
        return 1;
    }
    unlink(sock_path);  /* remove stale socket, if any */

    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", sock_path);

    if (bind(srv, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("bind"); return 1;
    }
    chmod(sock_path, 0666);   /* any user may connect; peer uid is authorized */

    if (listen(srv, 16) < 0) { perror("listen"); return 1; }

    /* Install without SA_RESTART so SIGTERM/SIGINT/SIGHUP interrupt the blocking
       accept() (return EINTR) and the loop can check g_stop/g_reload promptly.
       signal() defaults to SA_RESTART on Linux, which would auto-restart accept()
       and leave the daemon unstoppable until a connection happened to arrive. */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sa.sa_handler = on_stop;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);
    sa.sa_handler = on_reload;
    sigaction(SIGHUP,  &sa, NULL);
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);

    log_info("bkupd listening on %s (config: %s)", sock_path, config_path);

    scheduler_start(config_path);
    watch_start(config_path);

    while (!g_stop) {
        struct sockaddr_un ca;
        socklen_t clen = sizeof ca;
        int fd = accept(srv, (struct sockaddr *)&ca, &clen);
        if (fd < 0) {
            if (g_stop) break;
            if (g_reload) {
                g_reload = 0;
                userstate_reset();   /* operator fixed the key_file -> re-enable users */
                log_info("bkupd reloaded (config loaded per-connection)");
            }
            continue;
        }

        /* Identify the peer via SO_PEERCRED; the daemon serves only that user's
           section. A peer we cannot identify is refused. */
        struct ucred cr;
        socklen_t crlen = sizeof cr;
        if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &crlen) != 0) {
            log_warn("rejected connection: cannot read peer credentials");
            close(fd);
            continue;
        }

        ConnArg *arg = xmalloc(sizeof *arg);
        arg->fd          = fd;
        arg->config_path = xstrdup(config_path);
        arg->caller_uid  = cr.uid;

        pthread_t tid;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&tid, &attr, ipc_conn_thread, arg) != 0) {
            log_err("pthread_create failed");
            close(fd);
            free(arg->config_path);
            free(arg);
        }
        pthread_attr_destroy(&attr);
    }

    close(srv);
    unlink(sock_path);
    free(sp_alloc);
    log_info("bkupd stopped");
    return 0;
}
