/* Offline end-to-end test of prune's garbage collection, using the TR_LOCAL
   transport backend (transport_local_new) so NO SFTP server is needed.  It
   covers the two "remove what is now empty" behaviours:

     1. Catalog: a directory version with no file or symlink version anywhere
        beneath it is dropped, so a restore no longer recreates an empty
        skeleton.
     2. Repo: deleting the last orphaned blob out of a "blobs/<2 hex>" fan-out
        directory leaves that directory empty on the server, and prune rmdirs
        it.

   The Ctx/Config/User are hand-wired and the repo seeded with a low-cost KDF
   exactly as in test_roundtrip.c. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>
#include <dirent.h>

#include "cli/ctx.h"
#include "cli/commands.h"
#include "common/config.h"
#include "common/db.h"
#include "common/crypto.h"
#include "common/transport.h"
#include "common/util.h"
#include "common/types.h"
#include "test_common.h"

static int fails;

#define PASS "prune-gc-test-pw"

static void write_file(const char *path, const void *data, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(2); }
    if (n && fwrite(data, 1, n, f) != n) { perror("fwrite"); exit(2); }
    fclose(f);
}

/* Seed the repo skeleton + config on the fake server and the matching KDF meta
   in the local catalog: cmd_init with a fast KDF and no prompt. */
static void seed_repo(Ctx *c)
{
    KdfParams kdf = tc_kdf_params();
    Key key;
    if (crypto_derive_key(PASS, &kdf, &key) != 0) { fprintf(stderr, "kdf\n"); exit(2); }

    Buf kc; buf_init(&kc);
    crypto_keycheck_make(&key, &kc);

    RepoConf rc;
    memset(&rc, 0, sizeof rc);
    rc.version = 1;
    uint8_t id[16];
    crypto_random(id, sizeof id);
    hex_encode(id, sizeof id, rc.repo_id);
    rc.kdf = kdf;
    if (kc.len > sizeof rc.keycheck) { fprintf(stderr, "keycheck too big\n"); exit(2); }
    memcpy(rc.keycheck, kc.data, kc.len);
    rc.keycheck_len = kc.len;

    ctx_connect(c);                 /* no-op: c->t already set */
    char *blobs = repo_path(c, "blobs");
    char *cat   = repo_path(c, "catalog");
    char *cfgp  = repo_path(c, "config");
    transport_mkdir_p(c->t, c->src->repo);
    transport_mkdir(c->t, blobs);
    transport_mkdir(c->t, cat);

    Buf cfgtext; buf_init(&cfgtext);
    repoconf_format(&rc, &cfgtext);
    if (transport_put(c->t, cfgp, cfgtext.data, cfgtext.len, 0) != 0)
        { fprintf(stderr, "put config\n"); exit(2); }
    buf_free(&cfgtext);

    ctx_open_db(c);
    char salt_hex[BK_SALTBYTES * 2 + 1];
    char kc_hex[sizeof rc.keycheck * 2 + 1];
    char numbuf[32];
    hex_encode(rc.kdf.salt, BK_SALTBYTES, salt_hex);
    hex_encode(rc.keycheck, rc.keycheck_len, kc_hex);
    db_meta_set(c->db, "schema_version", "1");
    db_meta_set(c->db, "repo_id", rc.repo_id);
    db_meta_set(c->db, "kdf_salt", salt_hex);
    snprintf(numbuf, sizeof numbuf, "%llu", (unsigned long long)rc.kdf.ops);
    db_meta_set(c->db, "kdf_ops", numbuf);
    snprintf(numbuf, sizeof numbuf, "%llu", (unsigned long long)rc.kdf.mem);
    db_meta_set(c->db, "kdf_mem", numbuf);
    db_meta_set(c->db, "keycheck", kc_hex);

    buf_free(&kc);
    free(blobs); free(cat); free(cfgp);
}

/* Number of version rows for an exact path. */
static int have_version(sqlite3 *db, const char *path)
{
    sqlite3_stmt *st = db_prep(db, "SELECT COUNT(*) FROM versions WHERE path=?");
    sqlite3_bind_text(st, 1, path, -1, SQLITE_STATIC);
    int n = (sqlite3_step(st) == SQLITE_ROW) ? sqlite3_column_int(st, 0) : -1;
    sqlite3_finalize(st);
    return n;
}

/* Count the entries (excluding . and ..) in a local directory; -1 if absent. */
static int dir_entries(const char *path)
{
    DIR *d = opendir(path);
    if (!d) return -1;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)))
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) n++;
    closedir(d);
    return n;
}

/* Number of immediate subdirectories of `blobs` that hold no entries at all. */
static int empty_fanout_dirs(const char *blobs)
{
    DIR *d = opendir(blobs);
    if (!d) return -1;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char child[PATH_MAX];
        snprintf(child, sizeof child, "%s/%s", blobs, e->d_name);
        struct stat st;
        if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode) && dir_entries(child) == 0)
            n++;
    }
    closedir(d);
    return n;
}

int main(void)
{
    crypto_global_init();

    char base[PATH_MAX];
    tmpdir(base, sizeof base);

    char fakesrv[PATH_MAX], srctree[PATH_MAX], catalog[PATH_MAX];
    snprintf(fakesrv, sizeof fakesrv, "%s/srv", base);
    snprintf(srctree, sizeof srctree, "%s/src", base);
    snprintf(catalog, sizeof catalog, "%s/catalog.db", base);
    mkdir(fakesrv, 0700);
    mkdir_p(srctree, 0700);

    /* keep/  -- a file that survives every snapshot
       gone/  -- a file that is deleted before the second backup, leaving the
                 directory empty on disk
       empty/ -- a directory that never holds anything */
    char d_keep[PATH_MAX], d_gone[PATH_MAX], d_empty[PATH_MAX];
    char f_keep[PATH_MAX], f_gone[PATH_MAX];
    snprintf(d_keep,  sizeof d_keep,  "%s/keep", srctree);
    snprintf(d_gone,  sizeof d_gone,  "%s/gone", srctree);
    snprintf(d_empty, sizeof d_empty, "%s/empty", srctree);
    snprintf(f_keep,  sizeof f_keep,  "%s/keep/k.bin", srctree);
    snprintf(f_gone,  sizeof f_gone,  "%s/gone/g.bin", srctree);
    mkdir(d_keep, 0700);
    mkdir(d_gone, 0700);
    mkdir(d_empty, 0700);

    /* distinct random-ish content so the two files never share a blob */
    uint8_t kbuf[4096], gbuf[4096];
    fill(kbuf, sizeof kbuf, 0x1111);
    fill(gbuf, sizeof gbuf, 0x2222);
    write_file(f_keep, kbuf, sizeof kbuf);
    write_file(f_gone, gbuf, sizeof gbuf);

    setenv("BKUP_PASSPHRASE", PASS, 1);

    Config cfg; memset(&cfg, 0, sizeof cfg);
    cfg.server = (char *)"local"; cfg.port = 22; cfg.user = (char *)"test";

    char *sources[1] = { srctree };
    User u; memset(&u, 0, sizeof u);
    u.name = (char *)"test"; u.owner = NULL; u.scope = SCOPE_USER;
    u.repo = (char *)"/repo"; u.db = catalog;
    u.sources = sources; u.nsources = 1;

    Ctx c; memset(&c, 0, sizeof c);
    c.cfg = &cfg; c.src = &u;
    c.t = transport_local_new(fakesrv);

    seed_repo(&c);

    /* snapshot 1: both files present */
    CHECK(cmd_backup(&c, 0, NULL) == 0);

    /* snapshot 2: g.bin is gone, its directory is now empty on disk */
    CHECK(unlink(f_gone) == 0);
    CHECK(cmd_backup(&c, 0, NULL) == 0);

    /* every directory was captured while snapshot 1 was live */
    CHECK(have_version(c.db, d_keep)  >= 1);
    CHECK(have_version(c.db, d_gone)  >= 1);
    CHECK(have_version(c.db, d_empty) >= 1);

    char blobs_local[PATH_MAX];
    snprintf(blobs_local, sizeof blobs_local, "%s/repo/blobs", fakesrv);
    int fanout_before = dir_entries(blobs_local);
    CHECK(fanout_before > 0);

    /* prune to the newest snapshot only: g.bin's version interval [1,2) no
       longer overlaps a surviving snapshot, so its blob is collected. */
    char *argv[] = { (char *)"--keep-last", (char *)"1" };
    CHECK(cmd_prune(&c, 2, argv) == 0);

    /* 1. catalog: the two directories with nothing under them are gone; the
          one that still holds a file is untouched. */
    CHECKEQ_INT(have_version(c.db, d_gone),  0);
    CHECKEQ_INT(have_version(c.db, d_empty), 0);
    CHECK(have_version(c.db, d_keep) >= 1);
    CHECK(have_version(c.db, f_keep) >= 1);
    CHECKEQ_INT(have_version(c.db, f_gone), 0);

    /* no files row may be left pointing at a deleted version */
    sqlite3_stmt *dangl = db_prep(c.db,
        "SELECT COUNT(*) FROM files WHERE version_id IS NOT NULL "
        "AND version_id NOT IN (SELECT id FROM versions)");
    CHECK(sqlite3_step(dangl) == SQLITE_ROW);
    CHECKEQ_INT(sqlite3_column_int(dangl, 0), 0);
    sqlite3_finalize(dangl);

    /* 2. repo: the orphaned blob file is gone from the server and no fan-out
          directory was left behind empty. */
    CHECKEQ_INT(empty_fanout_dirs(blobs_local), 0);
    CHECK(dir_entries(blobs_local) > 0);   /* keep's blobs are still there */

    /* the surviving snapshot must still restore */
    char rdir[PATH_MAX];
    snprintf(rdir, sizeof rdir, "%s/restore", base);
    RestoreReq r; memset(&r, 0, sizeof r);
    r.dest = rdir; r.owner_uid = -1; r.owner_gid = -1; r.asof = 0; r.snap = -1;
    restore_run(&c, &r);
    char rk[PATH_MAX];
    snprintf(rk, sizeof rk, "%s%s", rdir, f_keep);
    uint8_t *got = NULL; size_t glen = 0;
    CHECKEQ_INT(read_file(rk, &got, &glen), 0);
    CHECKEQ_INT((long long)glen, (long long)sizeof kbuf);
    if (got && glen == sizeof kbuf) CHECK_MEMEQ(got, kbuf, sizeof kbuf);
    free(got);

    /* the pruned-away empty directories are not recreated by the restore */
    char re[PATH_MAX];
    snprintf(re, sizeof re, "%s%s", rdir, d_empty);
    CHECKEQ_INT(dir_entries(re), -1);

    if (c.db) db_close(c.db);
    transport_disconnect(c.t);
    rmtree_local(base);

    TEST_DONE("test_prune_gc");
}
