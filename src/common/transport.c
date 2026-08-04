#include <libssh2.h>
#include <libssh2_sftp.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <netdb.h>

#include "transport.h"
#include "util.h"
#include "log.h"

/* A Transport is normally the libssh2/SFTP backend (TR_SSH).  TR_LOCAL is a
   local-filesystem backend that maps every absolute server path P under
   local_root (i.e. local_root acts as a chroot): a put to "/repo/config"
   writes local_root + "/repo/config".  It exists so backup/restore/continuous/
   fetch-catalog can be tested end to end with no SFTP server.  The ssh code
   paths below are unchanged; each public function just dispatches on kind. */
enum { TR_SSH = 0, TR_LOCAL = 1 };

struct Transport {
    int               kind;
    char             *local_root;   /* TR_LOCAL only */
    int               sock;
    LIBSSH2_SESSION  *session;
    LIBSSH2_SFTP     *sftp;
};

static int lib_ready;

/* ---- TR_LOCAL backend: a real filesystem tree rooted at local_root -------- */

static char *lmap(const struct Transport *t, const char *path)
{
    size_t rl = strlen(t->local_root), pl = strlen(path);
    char *m = xmalloc(rl + pl + 1);
    memcpy(m, t->local_root, rl);
    memcpy(m + rl, path, pl + 1);   /* server paths are absolute (begin with '/') */
    return m;
}

/* Create the parent directories of a mapped file path. */
static void lmkparents(const char *mapped)
{
    char *tmp = xstrdup(mapped);
    char *slash = strrchr(tmp, '/');
    if (slash) { *slash = '\0'; mkdir_p(tmp, 0700); }
    free(tmp);
}

static int lrmtree(const char *m)
{
    struct stat st;
    if (lstat(m, &st) != 0) return (errno == ENOENT) ? 0 : -1;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(m);
        if (!d) return -1;
        int rc = 0;
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char *child = path_join(m, e->d_name);
            if (lrmtree(child) != 0) rc = -1;
            free(child);
        }
        closedir(d);
        if (rmdir(m) != 0 && errno != ENOENT) rc = -1;
        return rc;
    }
    if (unlink(m) != 0 && errno != ENOENT) return -1;
    return 0;
}

static int local_exists(const struct Transport *t, const char *path)
{
    char *m = lmap(t, path);
    struct stat st;
    int r = (stat(m, &st) == 0) ? 1 : 0;
    free(m);
    return r;
}

static int local_mkdir(const struct Transport *t, const char *path)
{
    char *m = lmap(t, path);
    if (mkdir(m, 0700) != 0 && errno != EEXIST) { /* tolerate, like the ssh path */ }
    free(m);
    return 0;
}

static int local_mkdir_p(const struct Transport *t, const char *path)
{
    char *m = lmap(t, path);
    mkdir_p(m, 0700);
    free(m);
    return 0;
}

static int local_delete(const struct Transport *t, const char *path)
{
    char *m = lmap(t, path);
    int r = (unlink(m) == 0 || errno == ENOENT) ? 0 : -1;
    free(m);
    return r;
}

static int local_rmtree(const struct Transport *t, const char *path)
{
    char *m = lmap(t, path);
    int r = lrmtree(m);
    free(m);
    return r;
}

static int local_put(const struct Transport *t, const char *path,
                     const uint8_t *data, size_t len, int overwrite)
{
    char *m = lmap(t, path);
    if (!overwrite) {
        struct stat st;
        if (stat(m, &st) == 0) { free(m); return 0; }   /* identical blob present */
    }
    lmkparents(m);
    char *tmp = xmalloc(strlen(m) + 5);
    sprintf(tmp, "%s.tmp", m);
    int rc = 0;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) rc = -1;
    else {
        size_t off = 0;
        while (off < len) {
            ssize_t w = write(fd, data + off, len - off);
            if (w < 0) { rc = -1; break; }
            off += (size_t)w;
        }
        if (close(fd) != 0) rc = -1;
    }
    if (rc == 0 && rename(tmp, m) != 0) rc = -1;
    if (rc != 0) unlink(tmp);
    free(tmp); free(m);
    return rc;
}

static int local_get(const struct Transport *t, const char *path, Buf *out)
{
    char *m = lmap(t, path);
    int fd = open(m, O_RDONLY);
    free(m);
    if (fd < 0) return -1;
    char buf[65536];
    int rc = 0;
    for (;;) {
        ssize_t r = read(fd, buf, sizeof buf);
        if (r < 0) { rc = -1; break; }
        if (r == 0) break;
        buf_append(out, buf, (size_t)r);
    }
    close(fd);
    return rc;
}

static int local_put_file(const struct Transport *t, const char *local,
                          const char *remote, int overwrite)
{
    char *m = lmap(t, remote);
    if (!overwrite) {
        struct stat st;
        if (stat(m, &st) == 0) { free(m); return 0; }
    }
    lmkparents(m);
    char *tmp = xmalloc(strlen(m) + 5);
    sprintf(tmp, "%s.tmp", m);
    FILE *in = fopen(local, "rb");
    if (!in) { free(tmp); free(m); return -1; }
    FILE *outp = fopen(tmp, "wb");
    if (!outp) { fclose(in); free(tmp); free(m); return -1; }
    char buf[65536];
    size_t n;
    int rc = 0;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0)
        if (fwrite(buf, 1, n, outp) != n) { rc = -1; break; }
    if (ferror(in)) rc = -1;
    fclose(in);
    if (fflush(outp) != 0 || fclose(outp) != 0) rc = -1;
    if (rc == 0 && rename(tmp, m) != 0) rc = -1;
    if (rc != 0) unlink(tmp);
    free(tmp); free(m);
    return rc;
}

static int local_get_file(const struct Transport *t, const char *remote,
                          const char *local)
{
    char *m = lmap(t, remote);
    FILE *in = fopen(m, "rb");
    free(m);
    if (!in) return -1;
    FILE *outp = fopen(local, "wb");
    if (!outp) { fclose(in); return -1; }
    char buf[65536];
    size_t n;
    int rc = 0;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0)
        if (fwrite(buf, 1, n, outp) != n) { rc = -1; break; }
    if (ferror(in)) rc = -1;
    fclose(in);
    if (fflush(outp) != 0 || fclose(outp) != 0) rc = -1;
    return rc;
}

Transport *transport_local_new(const char *root)
{
    Transport *t = xcalloc(1, sizeof *t);
    t->kind = TR_LOCAL;
    t->sock = -1;
    t->local_root = xstrdup(root);
    return t;
}

static int tcp_connect(const char *host, int port)
{
    char portstr[16];
    snprintf(portstr, sizeof portstr, "%d", port);
    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, portstr, &hints, &res) != 0)
        return -1;
    int sock = -1;
    for (rp = res; rp; rp = rp->ai_next) {
        sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock < 0) continue;
        if (connect(sock, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(sock);
        sock = -1;
    }
    freeaddrinfo(res);
    return sock;
}

static int hostkey_typebit(int hk_type)
{
    switch (hk_type) {
    case LIBSSH2_HOSTKEY_TYPE_RSA:       return LIBSSH2_KNOWNHOST_KEY_SSHRSA;
#ifdef LIBSSH2_HOSTKEY_TYPE_ED25519
    case LIBSSH2_HOSTKEY_TYPE_ED25519:   return LIBSSH2_KNOWNHOST_KEY_ED25519;
#endif
#ifdef LIBSSH2_HOSTKEY_TYPE_ECDSA_256
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_256: return LIBSSH2_KNOWNHOST_KEY_ECDSA_256;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_384: return LIBSSH2_KNOWNHOST_KEY_ECDSA_384;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_521: return LIBSSH2_KNOWNHOST_KEY_ECDSA_521;
#endif
    default:                             return 0;
    }
}

static const char *knownhost_alg(int keybits)
{
    switch (keybits) {
#ifdef LIBSSH2_KNOWNHOST_KEY_ED25519
    case LIBSSH2_KNOWNHOST_KEY_ED25519:   return "ssh-ed25519";
#endif
#ifdef LIBSSH2_KNOWNHOST_KEY_ECDSA_256
    case LIBSSH2_KNOWNHOST_KEY_ECDSA_256: return "ecdsa-sha2-nistp256";
    case LIBSSH2_KNOWNHOST_KEY_ECDSA_384: return "ecdsa-sha2-nistp384";
    case LIBSSH2_KNOWNHOST_KEY_ECDSA_521: return "ecdsa-sha2-nistp521";
#endif
    case LIBSSH2_KNOWNHOST_KEY_SSHRSA:
        return "rsa-sha2-512,rsa-sha2-256,ssh-rsa";
    default: return NULL;
    }
}

/* Mirror OpenSSH: propose first the host-key algorithms we already have
   pinned for this host in known_hosts, so the server presents a key type we
   can actually verify (otherwise libssh2 may negotiate, say, rsa when we only
   trust an ed25519 key, yielding a spurious CHECK_NOTFOUND). */
static void prefer_known_hostkey_types(LIBSSH2_SESSION *s,
                                       LIBSSH2_KNOWNHOSTS *nh,
                                       const char *host)
{
    Buf pref; buf_init(&pref);
    struct libssh2_knownhost *cur = NULL, *prev = NULL;
    while (libssh2_knownhost_get(nh, &cur, prev) == 0) {
        prev = cur;
        if (!cur->name || strcmp(cur->name, host) != 0) continue;
        const char *alg = knownhost_alg(cur->typemask & LIBSSH2_KNOWNHOST_KEY_MASK);
        if (!alg) continue;
        if (pref.len && memmem(pref.data, pref.len, alg, strlen(alg)))
            continue;   /* already listed */
        if (pref.len) buf_append(&pref, ",", 1);
        buf_append(&pref, alg, strlen(alg));
    }
    if (pref.len) {
        buf_append(&pref, "", 1);   /* NUL terminate */
        libssh2_session_method_pref(s, LIBSSH2_METHOD_HOSTKEY,
                                    (const char *)pref.data);
    }
    buf_free(&pref);
}

static void verify_hostkey(LIBSSH2_SESSION *s, LIBSSH2_KNOWNHOSTS *nh,
                           const char *host, int port)
{
    int hk_type = 0;
    size_t klen = 0;
    const char *key = libssh2_session_hostkey(s, &klen, &hk_type);
    if (!key) die("could not obtain server host key");

    int typemask = LIBSSH2_KNOWNHOST_TYPE_PLAIN |
                   LIBSSH2_KNOWNHOST_KEYENC_RAW |
                   hostkey_typebit(hk_type);
    struct libssh2_knownhost *found;
    int rc = libssh2_knownhost_checkp(nh, host, port, key, klen,
                                      typemask, &found);
    if (rc != LIBSSH2_KNOWNHOST_CHECK_MATCH)
        die("host key for %s not trusted (check=%d). Run `ssh -p %d %s` once "
            "to record it in ~/.ssh/known_hosts, then retry.",
            host, rc, port, host);
}

static int try_keyfiles(LIBSSH2_SESSION *s, const char *user)
{
    static const char *names[] = { "~/.ssh/id_ed25519", "~/.ssh/id_ecdsa",
                                   "~/.ssh/id_rsa", NULL };
    for (int i = 0; names[i]; i++) {
        char *priv = path_expand(names[i]);
        if (access(priv, R_OK) != 0) { free(priv); continue; }
        char *pub = xmalloc(strlen(priv) + 5);
        sprintf(pub, "%s.pub", priv);
        int rc = libssh2_userauth_publickey_fromfile(s, user, pub, priv, NULL);
        free(pub); free(priv);
        if (rc == 0) return 0;
    }
    return -1;
}

static int try_agent(LIBSSH2_SESSION *s, const char *user)
{
    LIBSSH2_AGENT *agent = libssh2_agent_init(s);
    if (!agent) return -1;
    int ok = -1;
    if (libssh2_agent_connect(agent) == 0 &&
        libssh2_agent_list_identities(agent) == 0) {
        struct libssh2_agent_publickey *id = NULL, *prev = NULL;
        for (;;) {
            int rc = libssh2_agent_get_identity(agent, &id, prev);
            if (rc != 0) break;     /* 1 = end of list, <0 = error */
            if (libssh2_agent_userauth(agent, user, id) == 0) { ok = 0; break; }
            prev = id;
        }
        libssh2_agent_disconnect(agent);
    }
    libssh2_agent_free(agent);
    return ok;
}

Transport *transport_connect(const char *host, int port, const char *user)
{
    if (!lib_ready) {
        if (libssh2_init(0) != 0) die("libssh2_init failed");
        lib_ready = 1;
    }

    int sock = tcp_connect(host, port);
    if (sock < 0) die("cannot connect to %s:%d", host, port);

    LIBSSH2_SESSION *s = libssh2_session_init();
    if (!s) die("libssh2_session_init failed");
    libssh2_session_set_blocking(s, 1);

    /* Load known_hosts before the handshake so we can bias host-key
       negotiation toward the algorithms we already trust for this host. */
    LIBSSH2_KNOWNHOSTS *nh = libssh2_knownhost_init(s);
    if (!nh) die("knownhost init failed");
    char *kh = path_expand("~/.ssh/known_hosts");
    if (libssh2_knownhost_readfile(nh, kh, LIBSSH2_KNOWNHOST_FILE_OPENSSH) < 0)
        log_warn("could not read %s; host key cannot be verified", kh);
    free(kh);
    prefer_known_hostkey_types(s, nh, host);

    if (libssh2_session_handshake(s, sock) != 0)
        die("SSH handshake with %s failed", host);

    verify_hostkey(s, nh, host, port);
    libssh2_knownhost_free(nh);

    if (try_agent(s, user) != 0 && try_keyfiles(s, user) != 0)
        die("authentication to %s@%s failed (no agent identity or usable key "
            "in ~/.ssh)", user, host);

    LIBSSH2_SFTP *sftp = libssh2_sftp_init(s);
    if (!sftp) die("SFTP session init failed");

    Transport *t = xcalloc(1, sizeof *t);
    t->sock = sock;
    t->session = s;
    t->sftp = sftp;
    return t;
}

void transport_disconnect(Transport *t)
{
    if (!t) return;
    if (t->kind == TR_LOCAL) { free(t->local_root); free(t); return; }
    if (t->sftp) libssh2_sftp_shutdown(t->sftp);
    if (t->session) {
        libssh2_session_disconnect(t->session, "bye");
        libssh2_session_free(t->session);
    }
    if (t->sock >= 0) close(t->sock);
    free(t);
}

int transport_exists(Transport *t, const char *path)
{
    if (t->kind == TR_LOCAL) return local_exists(t, path);
    LIBSSH2_SFTP_ATTRIBUTES attrs;
    int rc = libssh2_sftp_stat(t->sftp, path, &attrs);
    if (rc == 0) return 1;
    if (libssh2_sftp_last_error(t->sftp) == LIBSSH2_FX_NO_SUCH_FILE) return 0;
    return 0;   /* treat other errors as "absent"; caller will hit them again */
}

int transport_mkdir(Transport *t, const char *path)
{
    if (t->kind == TR_LOCAL) return local_mkdir(t, path);
    int rc = libssh2_sftp_mkdir(t->sftp, path, 0700);
    if (rc == 0) return 0;
    unsigned long e = libssh2_sftp_last_error(t->sftp);
    if (e == LIBSSH2_FX_FILE_ALREADY_EXISTS) return 0;
    return 0;   /* may already exist via a different error code; tolerate */
}

int transport_delete(Transport *t, const char *path)
{
    if (t->kind == TR_LOCAL) return local_delete(t, path);
    int rc = libssh2_sftp_unlink(t->sftp, path);
    if (rc == 0) return 0;
    if (libssh2_sftp_last_error(t->sftp) == LIBSSH2_FX_NO_SUCH_FILE) return 0;
    return -1;
}

int transport_rmtree(Transport *t, const char *path)
{
    if (t->kind == TR_LOCAL) return local_rmtree(t, path);
    LIBSSH2_SFTP_HANDLE *d = libssh2_sftp_opendir(t->sftp, path);
    if (!d) {
        /* not a directory (a plain file) or already absent: unlink it */
        if (libssh2_sftp_unlink(t->sftp, path) == 0) return 0;
        if (libssh2_sftp_last_error(t->sftp) == LIBSSH2_FX_NO_SUCH_FILE) return 0;
        return -1;
    }

    int rc = 0;
    char name[1024];
    LIBSSH2_SFTP_ATTRIBUTES attrs;
    for (;;) {
        int n = libssh2_sftp_readdir(d, name, sizeof name - 1, &attrs);
        if (n <= 0) break;
        name[n] = '\0';
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;

        char *child = path_join(path, name);
        if ((attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) &&
            LIBSSH2_SFTP_S_ISDIR(attrs.permissions)) {
            if (transport_rmtree(t, child) != 0) rc = -1;
        } else if (libssh2_sftp_unlink(t->sftp, child) != 0 &&
                   libssh2_sftp_last_error(t->sftp) != LIBSSH2_FX_NO_SUCH_FILE) {
            rc = -1;
        }
        free(child);
    }
    libssh2_sftp_closedir(d);

    if (libssh2_sftp_rmdir(t->sftp, path) != 0 &&
        libssh2_sftp_last_error(t->sftp) != LIBSSH2_FX_NO_SUCH_FILE)
        rc = -1;
    return rc;
}

int transport_mkdir_p(Transport *t, const char *path)
{
    if (t->kind == TR_LOCAL) return local_mkdir_p(t, path);
    char *tmp = xstrdup(path);
    for (char *s = tmp + 1; *s; s++) {
        if (*s == '/') {
            *s = '\0';
            transport_mkdir(t, tmp);
            *s = '/';
        }
    }
    transport_mkdir(t, tmp);
    free(tmp);
    return 0;
}

static int sftp_write_handle(LIBSSH2_SFTP_HANDLE *h,
                             const uint8_t *data, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t w = libssh2_sftp_write(h, (const char *)data + off, len - off);
        if (w < 0) return -1;
        off += (size_t)w;
    }
    return 0;
}

static int do_rename(Transport *t, const char *tmp, const char *path,
                     int overwrite)
{
    /* OpenSSH's sftp-server rejects SSH_FXP_RENAME onto an existing target and
       does not honor libssh2's ATOMIC/OVERWRITE flags, so for the overwrite
       case we remove the target first. The NATIVE flag still lets libssh2 use
       the posix-rename@openssh.com extension when available. */
    if (overwrite)
        libssh2_sftp_unlink(t->sftp, path);
    int rc = libssh2_sftp_rename_ex(t->sftp, tmp, strlen(tmp),
                                    path, strlen(path),
                                    LIBSSH2_SFTP_RENAME_NATIVE);
    if (rc == 0) return 0;
    if (!overwrite && transport_exists(t, path) == 1) {
        libssh2_sftp_unlink(t->sftp, tmp);   /* identical content already there */
        return 0;
    }
    return -1;
}

int transport_put(Transport *t, const char *path,
                  const uint8_t *data, size_t len, int overwrite)
{
    if (t->kind == TR_LOCAL) return local_put(t, path, data, len, overwrite);
    char *tmp = xmalloc(strlen(path) + 5);
    sprintf(tmp, "%s.tmp", path);

    LIBSSH2_SFTP_HANDLE *h = libssh2_sftp_open(t->sftp, tmp,
        LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC, 0600);
    int rc = -1;
    if (h) {
        rc = sftp_write_handle(h, data, len);
        libssh2_sftp_close(h);
    }
    if (rc == 0) rc = do_rename(t, tmp, path, overwrite);
    else libssh2_sftp_unlink(t->sftp, tmp);
    free(tmp);
    return rc;
}

int transport_get(Transport *t, const char *path, Buf *out)
{
    if (t->kind == TR_LOCAL) return local_get(t, path, out);
    LIBSSH2_SFTP_HANDLE *h = libssh2_sftp_open(t->sftp, path,
        LIBSSH2_FXF_READ, 0);
    if (!h) return -1;
    char tmp[65536];
    int rc = 0;
    for (;;) {
        ssize_t r = libssh2_sftp_read(h, tmp, sizeof tmp);
        if (r < 0) { rc = -1; break; }
        if (r == 0) break;
        buf_append(out, tmp, (size_t)r);
    }
    libssh2_sftp_close(h);
    return rc;
}

int transport_put_file(Transport *t, const char *local, const char *remote,
                       int overwrite)
{
    if (t->kind == TR_LOCAL) return local_put_file(t, local, remote, overwrite);
    FILE *f = fopen(local, "rb");
    if (!f) return -1;
    char *tmp = xmalloc(strlen(remote) + 5);
    sprintf(tmp, "%s.tmp", remote);

    LIBSSH2_SFTP_HANDLE *h = libssh2_sftp_open(t->sftp, tmp,
        LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC, 0600);
    int rc = 0;
    if (!h) { rc = -1; goto out; }
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        if (sftp_write_handle(h, buf, n) != 0) { rc = -1; break; }
    }
    if (ferror(f)) rc = -1;
    libssh2_sftp_close(h);
    if (rc == 0) rc = do_rename(t, tmp, remote, overwrite);
    else libssh2_sftp_unlink(t->sftp, tmp);
out:
    free(tmp);
    fclose(f);
    return rc;
}

int transport_get_file(Transport *t, const char *remote, const char *local)
{
    if (t->kind == TR_LOCAL) return local_get_file(t, remote, local);
    LIBSSH2_SFTP_HANDLE *h = libssh2_sftp_open(t->sftp, remote,
        LIBSSH2_FXF_READ, 0);
    if (!h) return -1;
    FILE *f = fopen(local, "wb");
    if (!f) { libssh2_sftp_close(h); return -1; }
    char buf[65536];
    int rc = 0;
    for (;;) {
        ssize_t r = libssh2_sftp_read(h, buf, sizeof buf);
        if (r < 0) { rc = -1; break; }
        if (r == 0) break;
        if (fwrite(buf, 1, (size_t)r, f) != (size_t)r) { rc = -1; break; }
    }
    libssh2_sftp_close(h);
    if (fflush(f) != 0 || fclose(f) != 0) rc = -1;
    return rc;
}
