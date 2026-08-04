/*
 * test_config.c -- config parsing unit tests (Phase 2).
 *
 * Writes temp .conf files to a tmpdir and calls config_load(path).  Covers
 * flat / sectioned configs, global excludes, keep-* defaults, bool parsing,
 * port, exclude semantics (basename vs fullpath, tilde expansion), subtree
 * exclusion, continuous-exclude, config_find_user, and the die() on a missing
 * required field.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "common/config.h"
#include "common/util.h"

#include "test_common.h"

static int fails;

/* ---- helpers ---- */

/*
 * Write `content` to <dir>/bkup.conf and return an xmalloc'd path to it.
 * The caller must free the returned string.
 */
static char *write_conf(const char *dir, const char *content)
{
    char *path = xmalloc(strlen(dir) + 16);
    sprintf(path, "%s/bkup.conf", dir);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { perror("open conf"); exit(2); }
    size_t len = strlen(content);
    if ((size_t)write(fd, content, len) != len) { perror("write conf"); exit(2); }
    close(fd);
    return path;
}

/* ---- flat sectionless config ---- */

static void test_flat_config(void)
{
    char dir[64]; tmpdir(dir, sizeof dir);
    char *path = write_conf(dir,
        "server = myserver\n"
        "repo = /backups\n"
        "source = /home/testuser\n");
    Config *c = config_load(path);
    CHECK(c != NULL);
    CHECK(c->nusers == 1);
    CHECKEQ_STR(c->users[0].name, "default");
    CHECK(c->users[0].owner == NULL);      /* flat "default" has no owner */
    CHECK(c->users[0].nsources == 1);
    CHECKEQ_STR(c->users[0].sources[0], "/home/testuser");
    CHECKEQ_STR(c->server, "myserver");
    CHECKEQ_INT(c->port, 22);              /* default port */
    config_free(c);
    free(path);
    rmtree_local(dir);
}

/* ---- [user "alice"] section: name, owner, scope, source and deprecated root ---- */

static void test_user_section(void)
{
    char dir[64]; tmpdir(dir, sizeof dir);
    char *path = write_conf(dir,
        "server = myserver\n"
        "repo = /backups\n"
        "[user \"alice\"]\n"
        "source = /home/alice/docs\n"
        "root = /home/alice/photos\n"     /* deprecated alias -> also appended */
        "scope = user\n");
    Config *c = config_load(path);
    CHECK(c != NULL);
    CHECK(c->nusers == 1);
    CHECKEQ_STR(c->users[0].name, "alice");
    CHECKEQ_STR(c->users[0].owner, "alice");
    CHECKEQ_INT(c->users[0].scope, SCOPE_USER);
    CHECKEQ_INT(c->users[0].nsources, 2);
    CHECKEQ_STR(c->users[0].sources[0], "/home/alice/docs");
    CHECKEQ_STR(c->users[0].sources[1], "/home/alice/photos");
    config_free(c);
    free(path);
    rmtree_local(dir);
}

/* ---- legacy [source "x"] header parses as a user ---- */

static void test_legacy_source_section(void)
{
    char dir[64]; tmpdir(dir, sizeof dir);
    char *path = write_conf(dir,
        "server = myserver\n"
        "repo = /backups\n"
        "[source \"bob\"]\n"
        "source = /home/bob\n");
    Config *c = config_load(path);
    CHECK(c != NULL);
    CHECK(c->nusers == 1);
    CHECKEQ_STR(c->users[0].name, "bob");
    CHECKEQ_STR(c->users[0].owner, "bob");
    config_free(c);
    free(path);
    rmtree_local(dir);
}

/* ---- [global] excludes merged; keep-* defaults; per-user override ---- */

static void test_global_excludes_and_keep(void)
{
    char dir[64]; tmpdir(dir, sizeof dir);
    char *path = write_conf(dir,
        "server = myserver\n"
        "repo = /backups\n"
        "[global]\n"
        "exclude = *.tmp\n"
        "exclude = .git\n"
        "continuous-exclude = *.mbox\n"
        "keep-last = 5\n"
        "keep-daily = 7\n"
        "keep-weekly = 4\n"
        "keep-monthly = 6\n"
        "keep-yearly = 2\n"
        "[user \"carol\"]\n"
        "source = /home/carol\n");
    Config *c = config_load(path);
    CHECK(c != NULL);
    CHECK(c->nusers == 1);
    User *u = &c->users[0];
    CHECK(u->nexcludes >= 2);           /* *.tmp and .git merged in */
    CHECK(u->ncontinuous_excludes >= 1);/* *.mbox merged in */
    CHECKEQ_INT(u->keep_last, 5);
    CHECKEQ_INT(u->keep_daily, 7);
    CHECKEQ_INT(u->keep_weekly, 4);
    CHECKEQ_INT(u->keep_monthly, 6);
    CHECKEQ_INT(u->keep_yearly, 2);
    config_free(c);
    free(path);
    rmtree_local(dir);
}

static void test_per_user_keep_overrides(void)
{
    char dir[64]; tmpdir(dir, sizeof dir);
    char *path = write_conf(dir,
        "server = myserver\n"
        "repo = /backups\n"
        "[global]\n"
        "keep-last = 5\n"
        "keep-daily = 7\n"
        "[user \"dan\"]\n"
        "source = /home/dan\n"
        "keep-last = 3\n");            /* per-user overrides global */
    Config *c = config_load(path);
    CHECK(c != NULL);
    User *u = &c->users[0];
    CHECKEQ_INT(u->keep_last, 3);      /* per-user wins */
    CHECKEQ_INT(u->keep_daily, 7);     /* global default applied */
    config_free(c);
    free(path);
    rmtree_local(dir);
}

/* ---- continuous: default on; bool parsing for all accepted spellings ---- */

static void test_continuous_bool(void)
{
    char dir[64]; tmpdir(dir, sizeof dir);

    /* default is on when not specified */
    char *path = write_conf(dir,
        "server = s\n"
        "repo = /r\n"
        "[user \"u1\"]\n"
        "source = /home/u1\n");
    Config *c = config_load(path);
    CHECK(c != NULL);
    CHECKEQ_INT(c->users[0].continuous, 1);
    config_free(c);
    free(path);

    /* all accepted spellings, case-insensitive */
    struct { const char *val; int want; } cases[] = {
        {"yes",   1}, {"YES",   1}, {"true",  1}, {"TRUE",  1},
        {"1",     1}, {"on",    1}, {"ON",    1},
        {"no",    0}, {"NO",    0}, {"false", 0}, {"FALSE", 0},
        {"0",     0}, {"off",   0}, {"OFF",   0},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char content[256];
        snprintf(content, sizeof content,
            "server = s\nrepo = /r\n"
            "[user \"u%zu\"]\nsource = /home/u\ncontinuous = %s\n",
            i, cases[i].val);
        path = write_conf(dir, content);
        c = config_load(path);
        CHECK(c != NULL);
        if (c) {
            CHECKEQ_INT(c->users[0].continuous, cases[i].want);
            config_free(c);
        }
        free(path);
    }
    rmtree_local(dir);
}

/* ---- port: default 22, explicit override ---- */

static void test_port(void)
{
    char dir[64]; tmpdir(dir, sizeof dir);

    char *path = write_conf(dir,
        "server = s\nrepo = /r\n"
        "[user \"u\"]\nsource = /home/u\n");
    Config *c = config_load(path);
    CHECK(c != NULL);
    CHECKEQ_INT(c->port, 22);
    config_free(c);
    free(path);

    path = write_conf(dir,
        "server = s\nport = 2222\nrepo = /r\n"
        "[user \"u\"]\nsource = /home/u\n");
    c = config_load(path);
    CHECK(c != NULL);
    CHECKEQ_INT(c->port, 2222);
    config_free(c);
    free(path);
    rmtree_local(dir);
}

/* ---- user_excluded: basename vs fullpath; tilde expansion ---- */

static void test_user_excluded(void)
{
    char dir[64]; tmpdir(dir, sizeof dir);
    char *path = write_conf(dir,
        "server = s\nrepo = /r\n"
        "[user \"eve\"]\n"
        "source = /home/eve\n"
        "exclude = *.log\n"
        "exclude = /home/eve/secret\n");
    Config *c = config_load(path);
    CHECK(c != NULL);
    User *u = &c->users[0];

    /* basename pattern (no '/') matches basename only */
    CHECK( user_excluded(u, "/home/eve/app.log"));
    CHECK(!user_excluded(u, "/home/eve/app.log.bak"));
    CHECK(!user_excluded(u, "/home/eve/app.txt"));

    /* fullpath pattern (has '/') matches full path */
    CHECK( user_excluded(u, "/home/eve/secret"));
    CHECK(!user_excluded(u, "/home/eve/secrets"));   /* different name */
    CHECK(!user_excluded(u, "/other/secret"));       /* different parent */

    config_free(c);
    free(path);
    rmtree_local(dir);
}

/* Tilde in an exclude expands to the home directory. */
static void test_exclude_tilde_expands(void)
{
    const char *home = getenv("HOME");
    if (!home || !home[0]) return;     /* skip if HOME not set */

    char dir[64]; tmpdir(dir, sizeof dir);
    /* owner "noexist_user" won't be found by getpwnam -> falls back to $HOME */
    char *path = write_conf(dir,
        "server = s\nrepo = /r\n"
        "[user \"noexist_user\"]\n"
        "source = /home/noexist_user\n"
        "exclude = ~/mysecret\n");
    Config *c = config_load(path);
    CHECK(c != NULL);
    User *u = &c->users[0];

    int found = 0;
    for (int i = 0; i < u->nexcludes; i++) {
        /* After tilde expansion, the pattern should start with HOME */
        if (strncmp(u->excludes[i], home, strlen(home)) == 0)
            found = 1;
    }
    CHECK(found);

    config_free(c);
    free(path);
    rmtree_local(dir);
}

/* ---- user_excluded_subtree: path or ancestor matching ---- */

static void test_user_excluded_subtree(void)
{
    char dir[64]; tmpdir(dir, sizeof dir);
    char *path = write_conf(dir,
        "server = s\nrepo = /r\n"
        "[user \"frank\"]\n"
        "source = /home/frank\n"
        "exclude = cache\n");
    Config *c = config_load(path);
    CHECK(c != NULL);
    User *u = &c->users[0];
    const char *root = "/home/frank";

    CHECK( user_excluded_subtree(u, "/home/frank/cache",          root));
    CHECK( user_excluded_subtree(u, "/home/frank/cache/foo.txt",  root));
    CHECK( user_excluded_subtree(u, "/home/frank/cache/a/b/c",    root));
    CHECK(!user_excluded_subtree(u, "/home/frank/docs/file.txt",  root));
    CHECK(!user_excluded_subtree(u, "/home/frank/download",       root));

    config_free(c);
    free(path);
    rmtree_local(dir);
}

/* ---- user_continuous_excluded: only tests continuous-exclude list ---- */

static void test_user_continuous_excluded(void)
{
    char dir[64]; tmpdir(dir, sizeof dir);
    char *path = write_conf(dir,
        "server = s\nrepo = /r\n"
        "[user \"gina\"]\n"
        "source = /home/gina\n"
        "continuous-exclude = *.mbox\n"
        "exclude = *.tmp\n");
    Config *c = config_load(path);
    CHECK(c != NULL);
    User *u = &c->users[0];
    const char *root = "/home/gina";

    CHECK( user_continuous_excluded(u, "/home/gina/mail.mbox", root));
    /* regular exclude does NOT appear in the continuous list */
    CHECK(!user_continuous_excluded(u, "/home/gina/scratch.tmp", root));
    CHECK(!user_continuous_excluded(u, "/home/gina/notes.txt", root));

    config_free(c);
    free(path);
    rmtree_local(dir);
}

/* ---- config_find_user: hit and miss ---- */

static void test_find_user(void)
{
    char dir[64]; tmpdir(dir, sizeof dir);
    char *path = write_conf(dir,
        "server = s\nrepo = /r\n"
        "[user \"alice\"]\nsource = /home/alice\n"
        "[user \"bob\"]\nsource = /home/bob\n");
    Config *c = config_load(path);
    CHECK(c != NULL);
    CHECK(config_find_user(c, "alice") != NULL);
    CHECK(config_find_user(c, "bob")   != NULL);
    CHECK(config_find_user(c, "carol") == NULL);
    CHECK(config_find_user(c, "")      == NULL);
    config_free(c);
    free(path);
    rmtree_local(dir);
}

/* ---- missing required field (server) -> die() ---- */

static char g_die_path[256];

static void fn_missing_server(void)
{
    /* config_load should die() on "server is required" */
    config_load(g_die_path);
}

static void test_missing_server_dies(void)
{
    char dir[64]; tmpdir(dir, sizeof dir);
    snprintf(g_die_path, sizeof g_die_path, "%s/bkup.conf", dir);

    /* write a config without 'server = ...' */
    int fd = open(g_die_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
        const char *s = "repo = /r\nsource = /home/u\n";
        write(fd, s, strlen(s));
        close(fd);
    }
    CHECK(check_dies(fn_missing_server));
    rmtree_local(dir);
}

/* ---- main ---- */

int main(void)
{
    test_flat_config();
    test_user_section();
    test_legacy_source_section();
    test_global_excludes_and_keep();
    test_per_user_keep_overrides();
    test_continuous_bool();
    test_port();
    test_user_excluded();
    test_exclude_tilde_expands();
    test_user_excluded_subtree();
    test_user_continuous_excluded();
    test_find_user();
    test_missing_server_dies();
    TEST_DONE("test_config");
}
