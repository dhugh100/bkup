#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fnmatch.h>
#include <ctype.h>
#include <limits.h>
#include <pwd.h>
#include <errno.h>
#include <sys/stat.h>

#include "config.h"
#include "util.h"
#include "log.h"

static char *current_user(void)
{
    const char *u = getenv("USER");
    if (u && u[0]) return xstrdup(u);
    struct passwd *pw = getpwuid(getuid());
    if (pw) return xstrdup(pw->pw_name);
    return xstrdup("root");
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' ||
                     e[-1] == '\n' || e[-1] == '\r'))
        *--e = '\0';
    return s;
}

/* Truncate an inline comment: a '#' preceded by whitespace (so a '#' inside a
   path or pattern value is preserved). Full-line comments are handled earlier. */
static void strip_inline_comment(char *s)
{
    for (char *p = s; *p; p++)
        if (*p == '#' && p != s && (p[-1] == ' ' || p[-1] == '\t')) {
            *p = '\0';
            return;
        }
}

static void append_str(char ***arr, int *n, char *val)
{
    *arr = xrealloc(*arr, (size_t)(*n + 1) * sizeof **arr);
    (*arr)[(*n)++] = val;
}

/* "My files" -> "my-files": lowercase, runs of non-alphanumerics become '-'. */
static char *slugify(const char *name)
{
    size_t n = strlen(name);
    char *out = xmalloc(n + 1);
    size_t o = 0;
    int prev_dash = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)name[i];
        if (isalnum(ch)) { out[o++] = (char)tolower(ch); prev_dash = 0; }
        else if (!prev_dash && o > 0) { out[o++] = '-'; prev_dash = 1; }
    }
    while (o > 0 && out[o - 1] == '-') o--;   /* trailing dash */
    if (o == 0) out[o++] = 'x';
    out[o] = '\0';
    return out;
}

static User *new_user(Config *c, const char *name)
{
    c->users = xrealloc(c->users, (size_t)(c->nusers + 1) * sizeof *c->users);
    User *s = &c->users[c->nusers++];
    memset(s, 0, sizeof *s);
    s->name  = xstrdup(name);
    s->scope = SCOPE_USER;
    s->continuous = 1;          /* watcher-driven continuous backups on by default */
    return s;
}

enum { SEC_NONE = 0, SEC_GLOBAL, SEC_USER };

/* Classify a section header.  [global] -> SEC_GLOBAL; [user "Name"] (or the
   legacy [source "Name"]) -> SEC_USER with *name filled (caller frees). */
static int parse_section(const char *line, char **name)
{
    if (line[0] != '[') return SEC_NONE;
    if (!strcmp(line, "[global]")) return SEC_GLOBAL;
    if (!strstr(line, "user") && !strstr(line, "source")) return SEC_NONE;
    const char *q1 = strchr(line, '"');
    if (!q1) return SEC_NONE;
    const char *q2 = strchr(q1 + 1, '"');
    if (!q2) return SEC_NONE;
    size_t len = (size_t)(q2 - q1 - 1);
    char *nm = xmalloc(len + 1);
    memcpy(nm, q1 + 1, len);
    nm[len] = '\0';
    *name = nm;
    return SEC_USER;
}

/* Apply a global key (valid in [global] or before the first section).
   Returns 1 if the key was recognized and applied, 0 otherwise. */
static int set_global_kv(Config *c, const char *key, const char *val)
{
    if      (!strcmp(key, "server")) { free(c->server); c->server = xstrdup(val); }
    else if (!strcmp(key, "port"))   c->port = atoi(val);
    else if (!strcmp(key, "user") ||
             !strcmp(key, "ssh_user")) { free(c->user); c->user = xstrdup(val); }
    else if (!strcmp(key, "repo"))   { free(c->repo_root); c->repo_root = xstrdup(val); }
    else if (!strcmp(key, "key_file")) { free(c->key_file); c->key_file = path_expand(val); }
    else if (!strcmp(key, "log_file")) { free(c->log_file); c->log_file = path_expand(val); }
    /* Global exclude patterns: stored raw, expanded per-user in finalize_user. */
    else if (!strcmp(key, "exclude"))
        append_str(&c->global_excludes, &c->nglobal_excludes, xstrdup(val));
    else if (!strcmp(key, "continuous-exclude"))
        append_str(&c->global_continuous_excludes, &c->nglobal_continuous_excludes, xstrdup(val));
    /* Global prune/keep defaults: applied to users with no per-user value. */
    else if (!strcmp(key, "prune"))        { free(c->global_prune_sched); c->global_prune_sched = xstrdup(val); }
    else if (!strcmp(key, "keep-last"))    c->global_keep_last    = atoi(val);
    else if (!strcmp(key, "keep-daily"))   c->global_keep_daily   = atoi(val);
    else if (!strcmp(key, "keep-weekly"))  c->global_keep_weekly  = atoi(val);
    else if (!strcmp(key, "keep-monthly")) c->global_keep_monthly = atoi(val);
    else if (!strcmp(key, "keep-yearly"))  c->global_keep_yearly  = atoi(val);
    else return 0;
    return 1;
}

/* Expand a leading "~/" (or a bare "~") in a value to the home directory of
   this source's OWNING user, looked up via getpwnam -- not the process's
   $HOME. Used for exclude patterns and the local catalog (db) path so that
   one root daemon anchors each user's paths under that user's home (e.g.
   "~/.local/state", "~/.local/share/bkup/<name>.db") regardless of who runs
   it. For the ownerless flat "default" source, fall back to $HOME. A value
   that does not start with "~/" or bare "~" is returned unchanged. */
static char *expand_user_tilde(const char *val, const char *owner)
{
    if (val[0] != '~' || (val[1] != '/' && val[1] != '\0'))
        return xstrdup(val);
    const char *home = NULL;
    if (owner) {
        struct passwd *pw = getpwnam(owner);
        if (pw) home = pw->pw_dir;
    }
    if (!home) home = getenv("HOME");
    if (!home) return xstrdup(val);          /* unresolved: leave literal */
    if (val[1] == '\0') return xstrdup(home);
    return path_join(home, val + 2);
}

/* Parse a boolean config value. Accepts on/off, true/false, yes/no, 1/0
   (case-insensitive). Unrecognized values fall back to `def`. */
static int parse_bool(const char *val, int def)
{
    if (!strcasecmp(val, "on")  || !strcasecmp(val, "true")  ||
        !strcasecmp(val, "yes") || !strcmp(val, "1")) return 1;
    if (!strcasecmp(val, "off") || !strcasecmp(val, "false") ||
        !strcasecmp(val, "no")  || !strcmp(val, "0")) return 0;
    return def;
}

static void set_user_kv(User *s, const char *key, const char *val,
                          const char *path, int lineno)
{
    if      (!strcmp(key, "owner"))           { free(s->owner); s->owner = xstrdup(val); }
    else if (!strcmp(key, "scope"))           s->scope = !strcmp(val, "system") ? SCOPE_SYSTEM : SCOPE_USER;
    else if (!strcmp(key, "repo"))            { free(s->repo); s->repo = xstrdup(val); }
    else if (!strcmp(key, "db"))              { free(s->db); s->db = expand_user_tilde(val, s->owner); }
    else if (!strcmp(key, "passphrase_file")) { free(s->passphrase_file); s->passphrase_file = path_expand(val); }
    else if (!strcmp(key, "source") ||
             !strcmp(key, "root"))            append_str(&s->sources, &s->nsources, path_expand(val));  /* "root": deprecated alias */
    else if (!strcmp(key, "exclude"))         append_str(&s->excludes, &s->nexcludes, expand_user_tilde(val, s->owner));
    else if (!strcmp(key, "continuous-exclude")) append_str(&s->continuous_excludes, &s->ncontinuous_excludes, expand_user_tilde(val, s->owner));
    else if (!strcmp(key, "continuous"))      s->continuous = parse_bool(val, 1);
    else if (!strcmp(key, "pre-backup"))      { free(s->pre_backup);  s->pre_backup  = xstrdup(val); }
    else if (!strcmp(key, "post-backup"))     { free(s->post_backup); s->post_backup = xstrdup(val); }
    else if (!strcmp(key, "backup"))          { free(s->backup_sched); s->backup_sched = xstrdup(val); }
    else if (!strcmp(key, "prune"))           { free(s->prune_sched); s->prune_sched = xstrdup(val); }
    else if (!strcmp(key, "keep-last"))       s->keep_last    = atoi(val);
    else if (!strcmp(key, "keep-daily"))      s->keep_daily   = atoi(val);
    else if (!strcmp(key, "keep-weekly"))     s->keep_weekly  = atoi(val);
    else if (!strcmp(key, "keep-monthly"))    s->keep_monthly = atoi(val);
    else if (!strcmp(key, "keep-yearly"))     s->keep_yearly  = atoi(val);
    else log_warn("config %s line %d: unknown user key '%s'", path, lineno, key);
}

/* Expand and append global exclude patterns into a user's per-user lists.
   Called from finalize_user after the user's own keys are parsed. */
static void apply_global_excludes(Config *c, User *s)
{
    for (int i = 0; i < c->nglobal_excludes; i++)
        append_str(&s->excludes, &s->nexcludes,
                   expand_user_tilde(c->global_excludes[i], s->owner));
    for (int i = 0; i < c->nglobal_continuous_excludes; i++)
        append_str(&s->continuous_excludes, &s->ncontinuous_excludes,
                   expand_user_tilde(c->global_continuous_excludes[i], s->owner));
}

/* Apply global prune/keep defaults to a user that has not set its own values. */
static void apply_global_prune(Config *c, User *s)
{
    if (!s->prune_sched && c->global_prune_sched)
        s->prune_sched = xstrdup(c->global_prune_sched);
    if (!s->keep_last    && c->global_keep_last)    s->keep_last    = c->global_keep_last;
    if (!s->keep_daily   && c->global_keep_daily)   s->keep_daily   = c->global_keep_daily;
    if (!s->keep_weekly  && c->global_keep_weekly)  s->keep_weekly  = c->global_keep_weekly;
    if (!s->keep_monthly && c->global_keep_monthly) s->keep_monthly = c->global_keep_monthly;
    if (!s->keep_yearly  && c->global_keep_yearly)  s->keep_yearly  = c->global_keep_yearly;
}

/* Fill in derived defaults for a source after parsing. */
static void finalize_user(Config *c, User *s, int flat)
{
    if (s->owner && !strcmp(s->owner, "root")) s->scope = SCOPE_SYSTEM;

    if (!s->repo) {
        if (flat)
            s->repo = c->repo_root ? xstrdup(c->repo_root) : NULL;
        else if (c->repo_root) {
            char *slug = slugify(s->name);
            s->repo = path_join(c->repo_root, slug);
            free(slug);
        }
    }
    if (!s->db) {
        if (flat) {
            s->db = path_expand("~/.local/share/bkup/catalog.db");
        } else {
            char *slug = slugify(s->name);
            char rel[256];
            snprintf(rel, sizeof rel, "~/.local/share/bkup/%s.db", slug);
            s->db = expand_user_tilde(rel, s->owner);   /* owner's home, not $HOME */
            free(slug);
        }
    }
    if (!s->repo) die("config: user '%s' has no repo and no global 'repo'", s->name);

    apply_global_excludes(c, s);
    apply_global_prune(c, s);
}

/* Overridden only by the tests (see config_set_default_path in config.h). */
static const char *g_default_config = BK_SYSTEM_CONFIG;

const char *config_default_path(void)
{
    return g_default_config;
}

void config_set_default_path(const char *path)
{
    g_default_config = path ? path : BK_SYSTEM_CONFIG;
}

Config *config_load(const char *path)
{
    /* Without -c there is one place to look: the system config the daemon
       reads. A per-user config is still reachable, by naming it with -c. */
    /* A stack copy, not a heap one: every die() below would abandon a heap
       buffer. In the daemon die() unwinds the connection thread via conn_die()
       rather than exiting the process, so a leak here is per-bad-request and
       accumulates for the daemon's lifetime. */
    char resolved[PATH_MAX];
    snprintf(resolved, sizeof resolved, "%s",
             path ? path : config_default_path());
    FILE *f = fopen(resolved, "r");
    if (!f) die("cannot open config %s", resolved);

    Config *c = xcalloc(1, sizeof *c);
    c->port = 22;

    /* Globals are accumulated into an implicit flat source until the first
       [source] header; if no header ever appears, that source becomes the
       single "default" source. */
    User *flat = new_user(c, "default");
    User *cur  = NULL;          /* current [user]/[source] section, if any */
    int in_global = 0;            /* inside a [global] section */
    int saw_section = 0;          /* a [user]/[source] section was seen */

    char line[4096];
    int lineno = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        char *s = trim(line);
        if (*s == '\0' || *s == '#') continue;
        strip_inline_comment(s);

        char *secname;
        int sec = parse_section(s, &secname);
        if (sec == SEC_GLOBAL) { in_global = 1; cur = NULL; continue; }
        if (sec == SEC_USER) {
            cur = new_user(c, secname);
            cur->owner = xstrdup(secname);   /* section name = owning user */
            free(secname);
            saw_section = 1;
            in_global = 0;
            continue;
        }

        char *eq = strchr(s, '=');
        if (!eq) die("config %s line %d: no '='", resolved, lineno);
        *eq = '\0';
        char *key = trim(s);
        char *val = trim(eq + 1);

        if (in_global) {
            if (!set_global_kv(c, key, val))
                log_warn("config %s line %d: unknown global key '%s'",
                         resolved, lineno, key);
            continue;
        }
        if (cur) { set_user_kv(cur, key, val, resolved, lineno); continue; }

        /* Pre-section lines: globals, plus flat-source keys for sectionless use. */
        if (!set_global_kv(c, key, val))
            set_user_kv(flat, key, val, resolved, lineno);
    }
    fclose(f);

    if (!c->server) die("config: 'server' is required");
    if (!c->user)   c->user = current_user();
    if (!c->log_file) c->log_file = xstrdup("/var/log/bkup/bkup.log");

    if (saw_section) {
        /* Drop the unused implicit flat source (index 0); shift the rest down. */
        free(c->users[0].name);
        free(c->users[0].owner); free(c->users[0].repo);
        free(c->users[0].db); free(c->users[0].passphrase_file);
        free(c->users[0].backup_sched); free(c->users[0].prune_sched);
        for (int i = 0; i < c->users[0].nsources; i++) free(c->users[0].sources[i]);
        for (int i = 0; i < c->users[0].nexcludes; i++) free(c->users[0].excludes[i]);
        for (int i = 0; i < c->users[0].ncontinuous_excludes; i++) free(c->users[0].continuous_excludes[i]);
        free(c->users[0].sources); free(c->users[0].excludes);
        free(c->users[0].continuous_excludes);
        memmove(&c->users[0], &c->users[1],
                (size_t)(c->nusers - 1) * sizeof *c->users);
        c->nusers--;
    }

    if (c->nusers == 0) die("config: no users defined");

    for (int i = 0; i < c->nusers; i++)
        finalize_user(c, &c->users[i], !saw_section);

    return c;
}

static void free_user(User *s)
{
    free(s->name); free(s->owner); free(s->repo);
    free(s->db); free(s->passphrase_file);
    free(s->backup_sched); free(s->prune_sched);
    free(s->pre_backup); free(s->post_backup);
    for (int i = 0; i < s->nsources; i++) free(s->sources[i]);
    for (int i = 0; i < s->nexcludes; i++) free(s->excludes[i]);
    for (int i = 0; i < s->ncontinuous_excludes; i++) free(s->continuous_excludes[i]);
    free(s->sources); free(s->excludes);
    free(s->continuous_excludes);
}

void config_free(Config *c)
{
    if (!c) return;
    free(c->server); free(c->user); free(c->repo_root); free(c->key_file);
    free(c->log_file); free(c->global_prune_sched);
    for (int i = 0; i < c->nglobal_excludes; i++) free(c->global_excludes[i]);
    for (int i = 0; i < c->nglobal_continuous_excludes; i++) free(c->global_continuous_excludes[i]);
    free(c->global_excludes); free(c->global_continuous_excludes);
    for (int i = 0; i < c->nusers; i++) free_user(&c->users[i]);
    free(c->users);
    free(c);
}

User *config_find_user(Config *c, const char *name)
{
    for (int i = 0; i < c->nusers; i++)
        if (!strcmp(c->users[i].name, name)) return &c->users[i];
    return NULL;
}

/* Warn (do not fail) when a non-root user's sources are not owned by that user.
   Recovery is controlled by the Linux user: the daemon chowns restored files
   to the calling uid, so a source owned by someone else cannot round-trip with
   its original ownership. We check ownership, not access(W_OK), because the
   daemon runs as root and access() would pass for every path. System-scope
   (root) sources are skipped -- root legitimately owns /etc, /usr, etc. */
void user_check_ownership(const User *s)
{
    if (!s->owner || s->scope == SCOPE_SYSTEM) return;
    struct passwd *pw = getpwnam(s->owner);
    if (!pw) return;                       /* unknown user: nothing to compare */
    for (int i = 0; i < s->nsources; i++) {
        struct stat st;
        if (stat(s->sources[i], &st) != 0)
            log_warn("user '%s': cannot stat source %s: %s",
                     s->name, s->sources[i], strerror(errno));
        else if (st.st_uid != pw->pw_uid)
            log_warn("user '%s': source %s is owned by uid %d, not '%s' (uid %d); "
                     "restored files will not keep their original owner",
                     s->name, s->sources[i], (int)st.st_uid,
                     s->owner, (int)pw->pw_uid);
    }
}

/* Match `path` against a pattern list: a pattern with '/' matches the whole
   path, one without matches the basename. */
static int match_patterns(char **pats, int npats, const char *path)
{
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    for (int i = 0; i < npats; i++) {
        const char *pat = pats[i];
        if (strchr(pat, '/')) {
            if (fnmatch(pat, path, 0) == 0) return 1;
        } else {
            if (fnmatch(pat, base, 0) == 0) return 1;
        }
    }
    return 0;
}

/* Match `path`, or any ancestor up to (and including) `root`, against a pattern
   list. Shared by user_excluded_subtree and user_continuous_excluded. */
static int match_subtree(char **pats, int npats, const char *path,
                         const char *root)
{
    if (match_patterns(pats, npats, path)) return 1;

    size_t rlen = strlen(root);
    char buf[PATH_MAX];
    if (strlen(path) >= sizeof buf) return 0;
    strcpy(buf, path);

    /* Walk parents up to the source root, matching the walk's directory
       pruning. Stop once a truncation drops below the root's length. */
    char *slash;
    while ((slash = strrchr(buf, '/')) != NULL) {
        *slash = '\0';
        if (strlen(buf) < rlen) break;
        if (match_patterns(pats, npats, buf)) return 1;
    }
    return 0;
}

int user_excluded(const User *s, const char *path)
{
    return match_patterns(s->excludes, s->nexcludes, path);
}

int user_excluded_subtree(const User *s, const char *path, const char *root)
{
    return match_subtree(s->excludes, s->nexcludes, path, root);
}

int user_continuous_excluded(const User *s, const char *path, const char *root)
{
    return match_subtree(s->continuous_excludes, s->ncontinuous_excludes,
                         path, root);
}
