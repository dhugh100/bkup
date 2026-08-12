/* Offline end-to-end tests of the real backup/restore/dedup/versioning and
   disaster-recovery (fetch-catalog) paths, using the TR_LOCAL transport backend
   (transport_local_new) so NO SFTP server is needed.  The whole stack runs for
   real: scan -> chunk -> compress -> encrypt -> "upload" to a local repo tree,
   then download -> decrypt -> decompress -> reassemble on restore.

   The Ctx/Config/User are hand-wired (like test_sweep) and the repo is seeded
   with a low-cost KDF (tc_kdf_params) so key derivation is fast; the passphrase
   comes from $BKUP_PASSPHRASE.  We never call ctx_free/config_free (the structs
   are stack-allocated), and clean the temp tree ourselves. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>

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

#define PASS "round-trip-test-pw"

static void write_file(const char *path, const void *data, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(2); }
    if (n && fwrite(data, 1, n, f) != n) { perror("fwrite"); exit(2); }
    fclose(f);
}

/* read a whole file; returns 0 and fills buf/len on success, -1 if absent */
static int read_whole(const char *path, uint8_t **buf, size_t *len)
{
    return read_file(path, buf, len);
}

static long long blob_count(sqlite3 *db)
{
    sqlite3_stmt *q = db_prep(db, "SELECT COUNT(*) FROM blobs");
    long long n = 0;
    if (sqlite3_step(q) == SQLITE_ROW) n = sqlite3_column_int64(q, 0);
    sqlite3_finalize(q);
    return n;
}

/* Seed the repo skeleton + config on the (fake) server and the matching KDF
   meta in the local catalog, mirroring cmd_init but with a fast KDF and no
   prompt.  Leaves c->db open and c->t set. */
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

/* Compare the restored copy of an absolute source path against expected bytes.
   Whole-snapshot restore writes to dest + the absolute source path. */
static int restored_matches(const char *dest, const char *abs_src,
                            const void *expect, size_t elen)
{
    char rp[PATH_MAX];
    snprintf(rp, sizeof rp, "%s%s", dest, abs_src);  /* abs_src begins with '/' */
    uint8_t *got; size_t glen;
    if (read_whole(rp, &got, &glen) != 0) return 0;
    int ok = (glen == elen) && (elen == 0 || memcmp(got, expect, elen) == 0);
    free(got);
    return ok;
}

int main(void)
{
    crypto_global_init();

    char base[PATH_MAX];
    tmpdir(base, sizeof base);

    char fakesrv[PATH_MAX], srctree[PATH_MAX], catalog[PATH_MAX];
    char rdir1[PATH_MAX], rdir2[PATH_MAX], rdir3[PATH_MAX];
    snprintf(fakesrv, sizeof fakesrv, "%s/srv", base);
    snprintf(srctree, sizeof srctree, "%s/src", base);
    snprintf(catalog, sizeof catalog, "%s/catalog.db", base);
    snprintf(rdir1, sizeof rdir1, "%s/restore1", base);
    snprintf(rdir2, sizeof rdir2, "%s/restore_v1", base);
    snprintf(rdir3, sizeof rdir3, "%s/restore_dr", base);
    mkdir(fakesrv, 0700);
    mkdir_p(srctree, 0700);

    /* a source tree: a small text file, a binary file big enough to chunk, and
       a file in a subdir */
    char f_small[PATH_MAX], f_big[PATH_MAX], f_sub[PATH_MAX], subdir[PATH_MAX];
    snprintf(f_small, sizeof f_small, "%s/small.txt", srctree);
    snprintf(f_big,   sizeof f_big,   "%s/big.bin", srctree);
    snprintf(subdir,  sizeof subdir,  "%s/sub", srctree);
    snprintf(f_sub,   sizeof f_sub,   "%s/sub/note.txt", srctree);
    mkdir(subdir, 0700);

    const char *small_v1 = "hello backup world\n";
    size_t bign = 3u * 1024 * 1024;          /* > chunk min, multiple chunks */
    uint8_t *big = xmalloc(bign);
    fill(big, bign, 0xABCDEF01ULL);
    const char *sub_v1 = "a note in a subdirectory\n";
    write_file(f_small, small_v1, strlen(small_v1));
    write_file(f_big, big, bign);
    write_file(f_sub, sub_v1, strlen(sub_v1));

    setenv("BKUP_PASSPHRASE", PASS, 1);

    Config cfg; memset(&cfg, 0, sizeof cfg);
    cfg.server = (char *)"local"; cfg.port = 22; cfg.user = (char *)"test";

    char *sources[1] = { srctree };
    User u; memset(&u, 0, sizeof u);
    u.name = (char *)"test"; u.owner = NULL; u.scope = SCOPE_USER;
    u.repo = (char *)"/repo"; u.db = catalog;
    u.sources = sources; u.nsources = 1; u.continuous = 1;

    Ctx c; memset(&c, 0, sizeof c);
    c.cfg = &cfg; c.src = &u;
    c.t = transport_local_new(fakesrv);

    seed_repo(&c);

    /* ---- 1. backup -> restore round-trip ---- */
    if (cmd_backup(&c, 0, NULL) != 0) { CHECK(0 && "backup1"); }
    long long after_b1 = blob_count(c.db);
    CHECK(after_b1 > 0);

    RestoreReq r; memset(&r, 0, sizeof r);
    r.dest = rdir1; r.owner_uid = -1; r.owner_gid = -1; r.asof = 0; r.snap = -1;
    restore_run(&c, &r);
    CHECK(restored_matches(rdir1, f_small, small_v1, strlen(small_v1)));
    CHECK(restored_matches(rdir1, f_big, big, bign));
    CHECK(restored_matches(rdir1, f_sub, sub_v1, strlen(sub_v1)));

    /* ---- 2. dedup: a second backup of an unchanged tree adds no blobs ---- */
    CHECK(cmd_backup(&c, 0, NULL) == 0);
    long long after_b2 = blob_count(c.db);
    CHECK(after_b2 == after_b1);     /* nothing new uploaded */

    /* ---- 3. versioning: modify small.txt, back up, restore latest + old ---- */
    const char *small_v2 = "hello backup world -- EDITED, second version\n";
    write_file(f_small, small_v2, strlen(small_v2));
    CHECK(cmd_backup(&c, 0, NULL) == 0);   /* snapshot 3 */

    /* latest restore sees the edited content; big.bin is unchanged */
    char rlatest[PATH_MAX];
    snprintf(rlatest, sizeof rlatest, "%s/restore_latest", base);
    RestoreReq rl; memset(&rl, 0, sizeof rl);
    rl.dest = rlatest; rl.owner_uid = -1; rl.owner_gid = -1; rl.snap = -1;
    restore_run(&c, &rl);
    CHECK(restored_matches(rlatest, f_small, small_v2, strlen(small_v2)));
    CHECK(restored_matches(rlatest, f_big, big, bign));

    /* restoring snapshot 1 brings back the ORIGINAL small.txt */
    RestoreReq ro; memset(&ro, 0, sizeof ro);
    ro.dest = rdir2; ro.owner_uid = -1; ro.owner_gid = -1; ro.snap = 1;
    restore_run(&c, &ro);
    CHECK(restored_matches(rdir2, f_small, small_v1, strlen(small_v1)));

    /* ---- 4. disaster recovery: wipe the local catalog, fetch-catalog, restore.
       Proves the repo is self-describing -- the catalog is reassembled purely
       from its encrypted chunks on the (fake) server. ---- */
    db_close(c.db);
    c.db = NULL;
    unlink(catalog);
    { char s[PATH_MAX];
      snprintf(s, sizeof s, "%s-wal", catalog); unlink(s);
      snprintf(s, sizeof s, "%s-shm", catalog); unlink(s); }
    CHECK(cmd_fetch_catalog(&c, 0, NULL) == 0);
    ctx_open_db(&c);                 /* reopen the rebuilt catalog */
    RestoreReq rd; memset(&rd, 0, sizeof rd);
    rd.dest = rdir3; rd.owner_uid = -1; rd.owner_gid = -1; rd.snap = -1;
    restore_run(&c, &rd);
    CHECK(restored_matches(rdir3, f_small, small_v2, strlen(small_v2)));
    CHECK(restored_matches(rdir3, f_big, big, bign));

    /* cleanup (matters under LeakSanitizer) */
    if (c.db) db_close(c.db);
    transport_disconnect(c.t);
    free(big);
    rmtree_local(base);

    TEST_DONE("test_roundtrip");
}
