#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <pwd.h>

#include "ctx.h"
#include "common/db.h"
#include "common/log.h"
#include "common/util.h"

/* Login name for a uid, falling back to $USER / "root". Caller frees. */
static char *uid_name(uid_t uid)
{
    struct passwd *pw = getpwuid(uid);
    if (pw && pw->pw_name && pw->pw_name[0]) return xstrdup(pw->pw_name);
    const char *u = getenv("USER");
    return xstrdup(u && u[0] ? u : "root");
}

/* The source owned by `uname`, matched by login name. A flat config's ownerless
   "default" source matches any user. Returns NULL if no section matches. */
static User *user_for_name(Config *cfg, const char *uname)
{
    User *flat = NULL, *mine = NULL;
    for (int i = 0; i < cfg->nusers; i++) {
        User *s = &cfg->users[i];
        if (!s->owner) { if (!flat) flat = s; }
        else if (!strcmp(s->owner, uname)) { mine = s; break; }
    }
    return mine ? mine : flat;
}

/* Load config without selecting a source (admin/scheduler enumerate users). */
Ctx *ctx_new_nosel(const char *config_path)
{
    /* Load first, allocate second: config_load() die()s on a bad config, and in
       the daemon that unwinds the connection thread -- a Ctx allocated ahead of
       it would be abandoned with no owner to free it. */
    Config *cfg = config_load(config_path);
    Ctx *c = xcalloc(1, sizeof *c);
    c->cfg = cfg;
    return c;
}

/* Load config and select the source owned by `uid` (root daemon: the calling
   user). Dies if that user has no section. */
Ctx *ctx_new_uid(const char *config_path, uid_t uid)
{
    Ctx *c = ctx_new_nosel(config_path);
    char *name = uid_name(uid);
    c->src = user_for_name(c->cfg, name);
    free(name);
    if (!c->src) die("no backup configuration for the current user");
    return c;
}

/* Load config and select the current user's source (CLI default). */
Ctx *ctx_new(const char *config_path)
{
    return ctx_new_uid(config_path, getuid());
}

/* The passphrase source: the global root-only key_file if set, else the
   selected source's per-user passphrase_file (flat/dev). */
const char *ctx_pass_source(Ctx *c)
{
    if (c->cfg->key_file && c->cfg->key_file[0]) return c->cfg->key_file;
    return c->src ? c->src->passphrase_file : NULL;
}

void ctx_use_user(Ctx *c, const char *name)
{
    User *s = config_find_user(c->cfg, name);
    if (!s) die("no such source '%s' in config", name);
    c->src = s;
}

void ctx_open_db(Ctx *c)
{
    if (!c->db) c->db = db_open(c->src->db);
}

void ctx_connect(Ctx *c)
{
    if (!c->t) c->t = transport_connect(c->cfg->server, c->cfg->port, c->cfg->user);
}

void ctx_free(Ctx *c)
{
    if (!c) return;
    if (c->t) transport_disconnect(c->t);
    if (c->db) db_close(c->db);
    config_free(c->cfg);
    free(c);
}

char *repo_path(Ctx *c, const char *sub)
{
    return path_join(c->src->repo, sub);
}

char *blob_repo_path(Ctx *c, const char hex[BK_HEX_LEN + 1])
{
    /* repo/blobs/<first two hex>/<full hex> */
    char sub[8 + 2 + 1 + BK_HEX_LEN + 1];
    snprintf(sub, sizeof sub, "blobs/%c%c/%s", hex[0], hex[1], hex);
    return path_join(c->src->repo, sub);
}

char *prompt_passphrase(const char *prompt, int confirm, const char *passphrase_file)
{
    if (passphrase_file && passphrase_file[0]) {
        FILE *f = fopen(passphrase_file, "r");
        if (!f) die_perm("cannot open passphrase_file %s", passphrase_file);
        char buf[1024];
        if (!fgets(buf, sizeof buf, f)) { fclose(f); die_perm("passphrase_file %s is empty", passphrase_file); }
        fclose(f);
        buf[strcspn(buf, "\r\n")] = '\0';
        return xstrdup(buf);
    }

    const char *env = getenv("BKUP_PASSPHRASE");
    if (env && env[0]) return xstrdup(env);

    FILE *tty = fopen("/dev/tty", "r+");
    if (!tty) die_perm("no passphrase: set BKUP_PASSPHRASE or run on a terminal");

    struct termios old, raw;
    tcgetattr(fileno(tty), &old);
    raw = old;
    raw.c_lflag &= ~(tcflag_t)ECHO;

    char buf1[1024], buf2[1024];
    fputs(prompt, tty); fflush(tty);
    tcsetattr(fileno(tty), TCSAFLUSH, &raw);
    if (!fgets(buf1, sizeof buf1, tty)) { tcsetattr(fileno(tty), TCSAFLUSH, &old); fclose(tty); die("read passphrase failed"); }
    fputc('\n', tty);
    buf1[strcspn(buf1, "\n")] = '\0';

    if (confirm) {
        fputs("Confirm passphrase: ", tty); fflush(tty);
        if (!fgets(buf2, sizeof buf2, tty)) { tcsetattr(fileno(tty), TCSAFLUSH, &old); fclose(tty); die("read passphrase failed"); }
        fputc('\n', tty);
        buf2[strcspn(buf2, "\n")] = '\0';
        if (strcmp(buf1, buf2) != 0) {
            tcsetattr(fileno(tty), TCSAFLUSH, &old);
            fclose(tty);
            die("passphrases do not match");
        }
    }
    tcsetattr(fileno(tty), TCSAFLUSH, &old);
    fclose(tty);
    return xstrdup(buf1);
}

void repoconf_format(const RepoConf *rc, Buf *out)
{
    char line[512];
    char salt_hex[BK_SALTBYTES * 2 + 1];
    char kc_hex[sizeof rc->keycheck * 2 + 1];

    hex_encode(rc->kdf.salt, BK_SALTBYTES, salt_hex);
    hex_encode(rc->keycheck, rc->keycheck_len, kc_hex);

    int n = snprintf(line, sizeof line,
        "version=%d\n"
        "repo_id=%s\n"
        "kdf_salt=%s\n"
        "kdf_ops=%llu\n"
        "kdf_mem=%llu\n"
        "keycheck=%s\n",
        rc->version, rc->repo_id, salt_hex,
        (unsigned long long)rc->kdf.ops,
        (unsigned long long)rc->kdf.mem,
        kc_hex);
    buf_append(out, line, (size_t)n);
}

static char *find_val(char *text, const char *key)
{
    /* Non-mutating: return a malloc'd copy of the value for `key` (the text
       up to the next newline), or NULL. Caller frees. Matching only at line
       starts, requiring `key=`. */
    size_t klen = strlen(key);
    const char *p = text;
    while (p && *p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *v = p + klen + 1;
            const char *e = strchr(v, '\n');
            size_t n = e ? (size_t)(e - v) : strlen(v);
            char *out = xmalloc(n + 1);
            memcpy(out, v, n);
            out[n] = '\0';
            return out;
        }
        p = strchr(p, '\n');
        if (p) p++;
    }
    return NULL;
}

int repoconf_parse(const uint8_t *data, size_t len, RepoConf *rc)
{
    char *text = xmalloc(len + 1);
    memcpy(text, data, len);
    text[len] = '\0';

    memset(rc, 0, sizeof *rc);
    int ok = 0;
    char *v;

    if ((v = find_val(text, "version"))) { rc->version = atoi(v); free(v); }
    if ((v = find_val(text, "repo_id"))) {
        snprintf(rc->repo_id, sizeof rc->repo_id, "%s", v);
        free(v);
    }
    if ((v = find_val(text, "kdf_salt"))) {
        int bad = hex_decode(v, rc->kdf.salt, BK_SALTBYTES);
        free(v);
        if (bad) goto out;
    } else goto out;
    if ((v = find_val(text, "kdf_ops"))) { rc->kdf.ops = strtoull(v, NULL, 10); free(v); }
    if ((v = find_val(text, "kdf_mem"))) { rc->kdf.mem = strtoull(v, NULL, 10); free(v); }
    if ((v = find_val(text, "keycheck"))) {
        size_t hl = strlen(v);
        if (hl % 2 != 0 || hl / 2 > sizeof rc->keycheck) { free(v); goto out; }
        rc->keycheck_len = hl / 2;
        int bad = hex_decode(v, rc->keycheck, rc->keycheck_len);
        free(v);
        if (bad) goto out;
    } else goto out;
    ok = 1;

out:
    free(text);
    return ok ? 0 : -1;
}
