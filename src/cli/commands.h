#ifndef BK_COMMANDS_H
#define BK_COMMANDS_H

#include "ctx.h"

/* Each returns a process exit code. argv is the args AFTER the subcommand. */
int cmd_init(Ctx *c, int argc, char **argv);
int cmd_backup(Ctx *c, int argc, char **argv);
int cmd_restore(Ctx *c, int argc, char **argv);
int cmd_verify(Ctx *c, int argc, char **argv);

/* One restore request: a set of directory and/or file targets restored together
   into `dest`. With `asof > 0` each target path is restored at its newest
   version captured at or before that epoch (deleted-since files still come
   back); otherwise snapshot `snap` is used (-1 = latest complete). owner_uid/gid
   >= 0 makes the root daemon hand restored files back to the calling user. With
   no targets the whole snapshot / everything-as-of-date is restored. */
typedef struct {
    const char        *dest;
    long               owner_uid, owner_gid;
    long long          asof;
    long long          snap;
    const char *const *dirs;   int ndirs;
    const char *const *files;  int nfiles;
} RestoreReq;
int restore_run(Ctx *c, const RestoreReq *r);
int cmd_snapshots(Ctx *c, int argc, char **argv);
int cmd_fetch_catalog(Ctx *c, int argc, char **argv);
int cmd_prune(Ctx *c, int argc, char **argv);
int cmd_sources(Ctx *c, int argc, char **argv);

/* Encrypt the current catalog and upload it to the server, updating the
   catalog/latest pointer. `snap` is used only in the remote filename. */
void upload_catalog(Ctx *c, long long snap);

/* Walk all configured roots, stat-diff against the catalog, mark changed and
   new files DIRTY, and delete catalog rows for files that have vanished. The
   `seen` marker is the current snapshot id. Runs inside one transaction. */
void scan_run(Ctx *c, long long seen);

/* Like scan_run, but walk only `root` and scope the vanished-file sweep to
   that subtree (path == root, or under root + "/"). Used by continuous backups. */
void scan_run_subtree(Ctx *c, long long seen, const char *root);

/* Build a complete snapshot from a scoped scan of `root`: carry forward every
   CLEAN file's version, refresh `root`, process the dirty files, finalize.
   Does NOT upload the catalog. Returns the snapshot id, or -1 if nothing was
   captured. The caller (CLI: cmd_continuous; daemon: watcher) decides catalog
   push. */
long long continuous_snapshot(Ctx *c, const char *root);
int cmd_continuous(Ctx *c, int argc, char **argv);

/* Derive the repo key from the local catalog's stored KDF params and verify
   it against the stored keycheck. die()s on a wrong passphrase. */
void key_from_meta(Ctx *c);

/* Guard: die() if the local catalog's repo_id does not match the server's. */
void check_repo_id(Ctx *c);

/* Per-run counters, accumulated by process_file. */
typedef struct {
    long long files_changed;
    long long chunks_total;
    long long chunks_new;
    long long chunks_dedup;
    long long bytes_raw;        /* plaintext bytes across all chunks */
    long long bytes_new;        /* plaintext bytes actually uploaded */
    long long bytes_stored;     /* compressed+encrypted bytes uploaded */
} Stats;

/* Back up one catalog row (file_id) into snapshot `snap`, chunking and
   uploading new blobs and recording the version. Shared by backup and
   continuous. */
void process_file(Ctx *c, long long snap, long long file_id, Stats *st);

#endif
