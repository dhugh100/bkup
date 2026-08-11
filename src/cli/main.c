#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "commands.h"
#include "common/crypto.h"
#include "common/log.h"
#include "common/version.h"

static int usage(void)
{
    fprintf(stderr,
        "usage: bkup [-c CONFIG] [-U USER] COMMAND [ARGS]\n"
        "       bkup --version\n"
        "\n"
        "commands:\n"
        "  init [--force]               create the repo and local catalog\n"
        "                               (--force wipes any existing repo first)\n"
        "  backup                       scan sources and back up changes\n"
        "  continuous PATH              back up changes under PATH only\n"
        "                               (still a complete snapshot; PATH must be\n"
        "                               inside a configured source)\n"
        "  restore [-s SNAP] [-p DIR] [-f FILE] DEST\n"
        "                               restore into DEST; -p DIR / -f FILE are\n"
        "                               recreated by name under DEST (DEST/DIR,\n"
        "                               DEST/FILE); default restores everything\n"
        "  verify                       check every stored blob\n"
        "  sources                      list configured users and their sources\n"
        "  snapshots                    list snapshots\n"
        "  fetch-catalog [--force]      rebuild the catalog from the server\n"
        "  prune [--keep-last N] [--keep-daily N] [--keep-weekly N]\n"
        "        [--keep-monthly N] [--keep-yearly N] [-n]\n"
        "                               remove old snapshots and unused blobs\n");
    return 2;
}

/* `read_only` marks commands that only read local state. They change neither
   the repo nor the catalog, so there is no event worth refusing to run over --
   which is what lets a normal user list snapshots without write access to the
   daemon's root-owned event log. */
static const struct {
    const char *name;
    int       (*fn)(Ctx *, int, char **);
    int         read_only;
} commands[] = {
    { "init",          cmd_init,          0 },
    { "backup",        cmd_backup,        0 },
    { "continuous",    cmd_continuous,    0 },
    { "spot",          cmd_continuous,    0 },   /* deprecated alias */
    { "restore",       cmd_restore,       0 },
    { "verify",        cmd_verify,        0 },
    { "snapshots",     cmd_snapshots,     1 },
    { "fetch-catalog", cmd_fetch_catalog, 0 },
    { "prune",         cmd_prune,         0 },
    { "sources",       cmd_sources,       1 },
};
#define NCOMMANDS (sizeof commands / sizeof commands[0])

int main(int argc, char **argv)
{
    crypto_global_init();

    const char *config = NULL;
    const char *source = NULL;
    int i = 1;
    while (i < argc && argv[i][0] == '-') {
        if (!strcmp(argv[i], "-c") && i + 1 < argc) { config = argv[i + 1]; i += 2; }
        /* -U selects the user section; -S is the deprecated spelling. */
        else if ((!strcmp(argv[i], "-U") || !strcmp(argv[i], "-S")) && i + 1 < argc) { source = argv[i + 1]; i += 2; }
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) return usage();
        /* Answered before ctx_new so it works with no config file present. */
        else if (!strcmp(argv[i], "--version")) { printf("bkup %s\n", BKUP_VERSION); return 0; }
        else break;
    }
    if (i >= argc) return usage();

    const char *cmd = argv[i++];
    int sub_argc = argc - i;
    char **sub_argv = argv + i;

    /* Resolve the command before loading config or opening the log, so an
       unknown command (or a mistyped flag, which lands here as the command)
       prints usage rather than dying on a log the caller cannot open. */
    size_t ci = 0;
    while (ci < NCOMMANDS && strcmp(cmd, commands[ci].name)) ci++;
    if (ci == NCOMMANDS) return usage();

    Ctx *c = ctx_new(config);
    if (source) ctx_use_user(c, source);

    /* A backup with no durable record is the failure we are guarding against,
       so refuse to run if the event log cannot be opened -- except for the
       read-only commands, which record nothing worth keeping and would
       otherwise be root-only on a host whose log the daemon owns. Without the
       file open, log output falls back to stderr; keep the bookkeeping lines
       out of a listing that a user is reading. */
    int logged = (log_open_file(c->cfg->log_file) == 0);
    if (!logged && !commands[ci].read_only)
        die("cannot open log file %s: %s", c->cfg->log_file, strerror(errno));
    if (logged) log_info("cli: %s '%s' started", cmd, c->src->name);

    if (!strcmp(cmd, "spot")) log_warn("cli: `spot` is deprecated; use `continuous`");

    int rc = commands[ci].fn(c, sub_argc, sub_argv);

    if (logged) {
        if (rc == 0) log_info("cli: %s '%s' completed", cmd, c->src->name);
        else         log_warn("cli: %s '%s' failed (rc=%d)", cmd, c->src->name, rc);
    }

    ctx_free(c);
    return rc;
}
