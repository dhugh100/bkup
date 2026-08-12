/* CLI-side client for bkupd.
 *
 * Why this exists: backup/restore/verify/prune need the repo passphrase
 * (/etc/bkup.key, root-only), an SSH identity for the storage server, and write
 * access to the event log -- all root-owned by design, so a normal user cannot
 * run them in-process. The daemon already holds exactly those, authorizes the
 * caller by peer uid (SO_PEERCRED), ignores any client-supplied user section,
 * and forks+drops to the calling user for restore writes so the kernel decides
 * where files may land. Routing the command there is what lets a user recover
 * their own files without sudo.
 *
 * The protocol is one request line, then events until "done" or "error"; log
 * events are printed as they arrive, so a long operation shows progress. */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

#include "client.h"
#include "common/ipcwire.h"
#include "common/log.h"
#include "common/util.h"

/* Grows a JSON request line. Fixed-size and fatal on overflow: the only
   unbounded input is the target list, and a silently truncated restore request
   would recover the wrong set of files. */
typedef struct { char buf[64 * 1024]; size_t len; } Req;

static void req_add(Req *r, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(r->buf + r->len, sizeof r->buf - r->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof r->buf - r->len)
        die("request too large (too many restore targets?)");
    r->len += (size_t)n;
}

/* Append "key":["a","b"] for a NULL-free array of paths. */
static void req_add_array(Req *r, const char *key, const char *const *v, int n)
{
    if (n <= 0) return;
    req_add(r, ",\"%s\":[", key);
    for (int i = 0; i < n; i++) {
        char esc[8192];
        ipc_json_escape(v[i], esc, sizeof esc);
        req_add(r, "%s\"%s\"", i ? "," : "", esc);
    }
    req_add(r, "]");
}

/* The daemon resolves a relative dest against its own cwd, not the caller's, so
   anything relative has to be made absolute here. Returns a malloc'd path. */
static char *abs_path(const char *p)
{
    if (p[0] == '/') return xstrdup(p);
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof cwd)) die("cannot read the current directory");
    return path_join(cwd, p);
}

/* Stream events until the daemon finishes. Returns 0 on "done", 1 otherwise. */
static int stream_events(int fd)
{
    char line[8192];
    int rc = 1;

    while (ipc_readline(fd, line, sizeof line) == 0) {
        char *evt = ipc_get_str(line, "event");
        if (!evt) continue;

        if (!strcmp(evt, "log")) {
            char *lvl = ipc_get_str(line, "level");
            char *msg = ipc_get_str(line, "msg");
            fprintf(stderr, "[%s] %s\n", lvl ? lvl : "I", msg ? msg : "");
            free(lvl); free(msg);
        } else if (!strcmp(evt, "waiting")) {
            /* Say why we are about to sit here, so a queued command does not
               look like a hung one. */
            char *holder = ipc_get_str(line, "cmd");
            long long w  = ipc_get_int(line, "wait", 0);
            fprintf(stderr, "waiting for %s to finish (up to %llds; "
                            "--wait 0 to fail instead)\n",
                    holder ? holder : "another operation", w);
            free(holder);
        } else if (!strcmp(evt, "done")) {
            rc = 0;
            free(evt);
            break;
        } else if (!strcmp(evt, "error")) {
            char *msg = ipc_get_str(line, "msg");
            fprintf(stderr, "bkup: %s\n", msg ? msg : "error");
            free(msg); free(evt);
            break;
        }
        free(evt);
    }
    return rc;
}

/* Send `req` and stream the reply. Ctrl-C here closes the socket, which the
   daemon notices before starting a queued command. */
static int client_call(const Req *r, const char *sock_path)
{
    int fd = ipc_connect(sock_path);
    if (fd < 0) {
        fprintf(stderr,
                "bkup: cannot reach bkupd on %s -- is the daemon running?\n"
                "      (systemctl status bkupd)\n", sock_path);
        return 1;
    }
    if (ipc_send(fd, r->buf) != 0) {
        fprintf(stderr, "bkup: sending the request to bkupd failed\n");
        close(fd);
        return 1;
    }
    int rc = stream_events(fd);
    close(fd);
    return rc;
}

/* Translate one CLI invocation into a request. Flag parsing mirrors the local
   commands so `bkup restore -p DIR /tmp/out` means the same thing either way. */
int client_run(const char *cmd, int argc, char **argv, long long wait_sec,
               const char *sock_path)
{
    if (!sock_path) sock_path = BKUPD_SOCK_PATH;

    Req r;
    r.len = 0;
    r.buf[0] = '\0';

    if (!strcmp(cmd, "restore")) {
        const char *dirs[4096], *files[4096];
        int nd = 0, nf = 0;
        const char *dest = NULL;
        long long snap = -1, asof = 0;

        for (int i = 0; i < argc; i++) {
            if (!strcmp(argv[i], "-s") && i + 1 < argc) snap = atoll(argv[++i]);
            else if (!strcmp(argv[i], "-A") && i + 1 < argc) asof = atoll(argv[++i]);
            else if (!strcmp(argv[i], "-p") && i + 1 < argc) {
                if (nd < 4096) dirs[nd++] = argv[++i]; else i++;
            } else if (!strcmp(argv[i], "-f") && i + 1 < argc) {
                if (nf < 4096) files[nf++] = argv[++i]; else i++;
            } else if (!strcmp(argv[i], "-o") && i + 1 < argc) {
                /* Ownership is not the caller's to choose here: the daemon
                   hands restored files to the peer uid it authenticated. */
                die("-o is not available when the daemon performs the restore; "
                    "restored files are owned by you");
            } else if (argv[i][0] != '-') dest = argv[i];
        }
        if (!dest)
            die("usage: bkup restore [-s SNAP] [-A EPOCH] [-p DIR]... [-f FILE]... DEST");

        char *adest = abs_path(dest);
        char edest[8192];
        ipc_json_escape(adest, edest, sizeof edest);
        req_add(&r, "{\"cmd\":\"restore\",\"dest\":\"%s\"", edest);
        free(adest);
        if (snap >= 0) req_add(&r, ",\"snapshot\":%lld", snap);
        if (asof > 0)  req_add(&r, ",\"asof\":%lld", asof);
        req_add_array(&r, "dirs", dirs, nd);
        req_add_array(&r, "files", files, nf);

    } else if (!strcmp(cmd, "prune")) {
        static const struct { const char *flag, *key; } keeps[] = {
            { "--keep-last",    "keep_last"    },
            { "--keep-daily",   "keep_daily"   },
            { "--keep-weekly",  "keep_weekly"  },
            { "--keep-monthly", "keep_monthly" },
            { "--keep-yearly",  "keep_yearly"  },
        };
        req_add(&r, "{\"cmd\":\"prune\"");
        for (int i = 0; i < argc; i++) {
            if (!strcmp(argv[i], "-n"))
                die("prune -n (dry run) is only available locally, as root");
            int matched = 0;
            for (size_t k = 0; k < sizeof keeps / sizeof keeps[0]; k++) {
                if (!strcmp(argv[i], keeps[k].flag) && i + 1 < argc) {
                    req_add(&r, ",\"%s\":%lld", keeps[k].key, atoll(argv[++i]));
                    matched = 1;
                    break;
                }
            }
            if (!matched) die("unknown prune option '%s'", argv[i]);
        }

    } else {
        /* backup and verify take no arguments. */
        if (argc > 0) die("%s takes no arguments", cmd);
        req_add(&r, "{\"cmd\":\"%s\"", cmd);
    }

    if (wait_sec >= 0) req_add(&r, ",\"wait\":%lld", wait_sec);
    req_add(&r, "}");

    return client_call(&r, sock_path);
}
