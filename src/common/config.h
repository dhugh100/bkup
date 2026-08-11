#ifndef BK_CONFIG_H
#define BK_CONFIG_H

/* The system config: what bkupd is pointed at by convention, and the config
   the CLI and GUI load when no -c is given. */
#define BK_SYSTEM_CONFIG "/etc/bkup.conf"

enum { SCOPE_USER, SCOPE_SYSTEM };

/* One user's backup set: the sources/excludes/schedule/retention for a single
   owning user, with its own repo subdir, catalog, and passphrase. Built from a
   [user "name"] section (name = owner). A flat (sectionless) config yields one
   ownerless source named "default" that matches any user (dev/single-user). */
typedef struct {
    char  *name;            /* owning username; "default" for a flat config */
    char  *owner;           /* owning username (NULL only for flat "default") */
    int    scope;           /* SCOPE_USER / SCOPE_SYSTEM (derived from owner) */

    char  *repo;            /* full repo path on the server */
    char  *db;              /* local catalog path (expanded) */
    char  *passphrase_file; /* optional: read passphrase from this file */

    char **sources;           /* directories to back up */
    int    nsources;
    char **excludes;        /* fnmatch patterns */
    int    nexcludes;
    char **continuous_excludes; /* fnmatch patterns: backed up, but never trigger
                                   a continuous snapshot (e.g. a hot mail store) */
    int    ncontinuous_excludes;

    int    continuous;      /* watcher-driven continuous backups on/off (default on) */

    char  *backup_sched;    /* schedule string, e.g. "daily 02:00" (raw) */
    char  *prune_sched;     /* schedule string, e.g. "weekly" (raw) */

    int    keep_last, keep_daily, keep_weekly, keep_monthly, keep_yearly;
} User;

typedef struct {
    char   *server;         /* required (global transport) */
    int     port;           /* default 22 */
    char   *user;           /* ssh user; default: $USER */
    char   *repo_root;      /* global repo root; users live in repo_root/<slug> */
    char   *key_file;       /* root-only passphrase file used for all users' repos */
    char   *log_file;       /* durable event log; default /var/log/bkup/bkup.log */

    /* Global exclude patterns -- merged into every user's lists at load time.
       Stored raw (tilde unexpanded) and expanded per-user in finalize_user. */
    char **global_excludes;
    int    nglobal_excludes;
    char **global_continuous_excludes;
    int    nglobal_continuous_excludes;

    /* Global prune/keep defaults -- applied to users that have no per-user value. */
    char  *global_prune_sched;
    int    global_keep_last, global_keep_daily, global_keep_weekly,
           global_keep_monthly, global_keep_yearly;

    User *users;
    int     nusers;
} Config;

/* Load config from `path` (or the default location if NULL). Aborts via die()
   on a missing required field or unreadable file. */
Config *config_load(const char *path);

/* The path config_load(NULL) resolves to: BK_SYSTEM_CONFIG unless overridden.
   The setter is a test seam -- it exists so the default-path branch can be
   exercised without writing to /etc, and nothing in the shipped binaries calls
   it. Deliberately not wired to an env var or a flag: which config a root-run
   CLI loads must not be settable from the ambient environment. `path` is
   borrowed, not copied, and must outlive the config_load(NULL) call; NULL
   restores the built-in default. */
const char *config_default_path(void);
void        config_set_default_path(const char *path);
void    config_free(Config *c);

/* Find a user section by name, or NULL. */
User *config_find_user(Config *c, const char *name);

/* Warn if any of a non-root user's sources are not owned by that user, so
   restored files would lose their original ownership. Never fatal. */
void user_check_ownership(const User *s);

/* Returns 1 if `path` matches any of the source's exclude rules. A pattern
   without '/' matches the basename; with '/' it matches the whole path. A
   leading '~/' in an exclude is expanded at load time to the owning user's
   home directory (see expand_user_tilde in config.c). */
int     user_excluded(const User *s, const char *path);

/* Returns 1 if `path`, or any ancestor directory of it down to (and including)
   `root`, is excluded. The full-backup walk prunes excluded directories so
   their descendants are never visited; the continuous watcher sees leaf events and
   must apply the same subtree semantics explicitly. `root` is the source root
   the path lives under; ancestors above it are never tested. */
int     user_excluded_subtree(const User *s, const char *path,
                              const char *root);

/* Like user_excluded_subtree, but tests the `continuous-exclude` patterns: a
   match means the path is still backed up by scans, but a change to it must not
   trigger a continuous snapshot. Consulted only by the watcher, never by the
   backup walk. */
int     user_continuous_excluded(const User *s, const char *path,
                                 const char *root);

#endif
