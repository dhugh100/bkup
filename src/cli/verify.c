#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>

#include "commands.h"
#include "common/db.h"
#include "common/types.h"
#include "common/compress.h"
#include "common/crypto.h"
#include "common/catalog_chunk.h"
#include "common/transport.h"
#include "common/log.h"

int cmd_verify(Ctx *c, int argc, char **argv)
{
    (void)argc; (void)argv;
    ctx_open_db(c);
    ctx_connect(c);
    key_from_meta(c);

    long long checked = 0, failed = 0;
    sqlite3_stmt *q = db_prep(c->db,
        "SELECT hash,size FROM blobs WHERE state=1");
    while (sqlite3_step(q) == SQLITE_ROW) {
        const uint8_t *h = sqlite3_column_blob(q, 0);
        long long size = sqlite3_column_int64(q, 1);
        char hex[BK_HEX_LEN + 1];
        hex_encode(h, BK_HASH_LEN, hex);
        char *remote = blob_repo_path(c, hex);

        Buf ct; buf_init(&ct);
        Buf comp; buf_init(&comp);
        Buf pt; buf_init(&pt);
        int ok = 1;
        if (transport_get(c->t, remote, &ct) != 0) {
            log_err("missing blob %s", hex); ok = 0;
        } else if (crypto_open(&c->key, ct.data, ct.len, &comp) != 0) {
            log_err("auth failed for blob %s", hex); ok = 0;
        } else if (zstd_decompress(comp.data, comp.len, &pt) != 0) {
            log_err("decompress failed for blob %s", hex); ok = 0;
        } else {
            uint8_t check[BK_HASH_LEN];
            bk_hash(pt.data, pt.len, check);
            if (memcmp(check, h, BK_HASH_LEN) != 0) {
                log_err("hash mismatch for blob %s", hex); ok = 0;
            } else if ((long long)pt.len != size) {
                log_err("size mismatch for blob %s (%zu != %lld)",
                        hex, pt.len, size); ok = 0;
            }
        }
        buf_free(&ct); buf_free(&comp); buf_free(&pt);
        free(remote);
        checked++;
        if (!ok) failed++;
    }
    sqlite3_finalize(q);

    /* orphan references: a version points at a blob that is missing or not
       marked uploaded */
    sqlite3_stmt *o = db_prep(c->db,
        "SELECT COUNT(*) FROM version_blobs vb "
        "LEFT JOIN blobs b ON b.hash=vb.hash "
        "WHERE b.hash IS NULL OR b.state!=1");
    sqlite3_step(o);
    long long orphans = sqlite3_column_int64(o, 0);
    sqlite3_finalize(o);

    log_info("verified %lld blob(s): %lld ok, %lld failed; %lld orphan ref(s)",
             checked, checked - failed, failed, orphans);
    return (failed == 0 && orphans == 0) ? 0 : 1;
}

int cmd_snapshots(Ctx *c, int argc, char **argv)
{
    (void)argc; (void)argv;
    ctx_open_db(c);
    sqlite3_stmt *q = db_prep(c->db,
        "SELECT s.id, datetime(s.created,'unixepoch','localtime'), s.state, "
        "s.hostname, (SELECT COUNT(*) FROM versions v "
        "             WHERE v.first_snapshot<=s.id "
        "             AND (v.last_snapshot IS NULL OR v.last_snapshot>s.id)) "
        "FROM snapshots s ORDER BY s.id");
    printf("%-5s %-20s %-9s %-16s %s\n",
           "ID", "CREATED", "STATE", "HOST", "FILES");
    while (sqlite3_step(q) == SQLITE_ROW) {
        printf("%-5lld %-20s %-9s %-16s %lld\n",
               sqlite3_column_int64(q, 0),
               (const char *)sqlite3_column_text(q, 1),
               sqlite3_column_int(q, 2) ? "complete" : "OPEN",
               sqlite3_column_text(q, 3) ?
                   (const char *)sqlite3_column_text(q, 3) : "",
               sqlite3_column_int64(q, 4));
    }
    sqlite3_finalize(q);
    return 0;
}

int cmd_fetch_catalog(Ctx *c, int argc, char **argv)
{
    int force = 0;
    for (int i = 0; i < argc; i++)
        if (!strcmp(argv[i], "--force")) force = 1;

    ctx_connect(c);

    /* read the server repo config: gives us KDF params with no local state */
    char *cfgpath = repo_path(c, "config");
    Buf raw; buf_init(&raw);
    if (transport_get(c->t, cfgpath, &raw) != 0)
        die("cannot read repo config at %s", cfgpath);
    RepoConf rc;
    if (repoconf_parse(raw.data, raw.len, &rc) != 0)
        die("repo config is malformed");
    buf_free(&raw);
    free(cfgpath);

    char *pass = prompt_passphrase("Passphrase: ", 0, ctx_pass_source(c));
    if (crypto_derive_key(pass, &rc.kdf, &c->key) != 0)
        die("key derivation failed");
    memset(pass, 0, strlen(pass));
    free(pass);
    if (crypto_keycheck_verify(&c->key, rc.keycheck, rc.keycheck_len) != 0)
        die("wrong passphrase");
    c->have_key = 1;

    /* refuse to clobber an existing catalog unless forced */
    if (!force && access(c->src->db, F_OK) == 0)
        die("catalog %s already exists; use --force to overwrite", c->src->db);

    char *latest = repo_path(c, "catalog/latest");
    Buf nameb; buf_init(&nameb);
    if (transport_get(c->t, latest, &nameb) != 0)
        die("cannot read catalog/latest pointer");
    char name[256];
    size_t nl = nameb.len < sizeof name - 1 ? nameb.len : sizeof name - 1;
    memcpy(name, nameb.data, nl);
    name[nl] = '\0';
    /* strip any trailing newline */
    name[strcspn(name, "\r\n")] = '\0';
    buf_free(&nameb);
    free(latest);

    /* The pointer now names an encrypted manifest. Download + decrypt it; it
       lists the catalog's chunks in order. */
    char remote[512];
    snprintf(remote, sizeof remote, "catalog/%s", name);
    char *rpath = repo_path(c, remote);
    Buf manct; buf_init(&manct);
    if (transport_get(c->t, rpath, &manct) != 0)
        die("cannot download catalog manifest %s", remote);
    free(rpath);
    Buf man; buf_init(&man);
    if (crypto_open(&c->key, manct.data, manct.len, &man) != 0)
        die("failed to decrypt catalog manifest (wrong passphrase?)");
    buf_free(&manct);
    CatManifest m;
    if (catman_parse(man.data, man.len, &m) != 0)
        die("catalog manifest is malformed");
    buf_free(&man);

    /* ensure parent dir of the local catalog exists */
    char *dbcopy = xstrdup(c->src->db);
    char *slash = strrchr(dbcopy, '/');
    if (slash) { *slash = '\0'; if (dbcopy[0]) mkdir_p(dbcopy, 0700); }
    free(dbcopy);

    /* reassemble the plaintext DB from its chunks, verifying each hash, into a
       temp file, then rename into place atomically. */
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s.fetch", c->src->db);
    unlink(tmp);
    FILE *out = fopen(tmp, "wb");
    if (!out) die("cannot create %s: %s", tmp, strerror(errno));
    for (uint32_t i = 0; i < m.nchunks; i++) {
        const uint8_t *h = m.hashes + (size_t)i * BK_HASH_LEN;
        char hex[BK_HEX_LEN + 1];
        hex_encode(h, BK_HASH_LEN, hex);
        char sub[32 + BK_HEX_LEN];
        snprintf(sub, sizeof sub, "catalog/blobs/%c%c/%s", hex[0], hex[1], hex);
        char *cpath = repo_path(c, sub);
        Buf ct; buf_init(&ct);
        if (transport_get(c->t, cpath, &ct) != 0)
            die("cannot download catalog chunk %s", hex);
        free(cpath);
        Buf comp; buf_init(&comp);
        if (crypto_open(&c->key, ct.data, ct.len, &comp) != 0)
            die("failed to decrypt catalog chunk %s", hex);
        buf_free(&ct);
        Buf pt; buf_init(&pt);
        if (zstd_decompress(comp.data, comp.len, &pt) != 0)
            die("failed to decompress catalog chunk %s", hex);
        buf_free(&comp);
        uint8_t vh[BK_HASH_LEN];
        bk_hash(pt.data, pt.len, vh);
        if (memcmp(vh, h, BK_HASH_LEN) != 0)
            die("catalog chunk %s failed integrity check", hex);
        if (fwrite(pt.data, 1, pt.len, out) != pt.len)
            die("write error assembling catalog at %s", tmp);
        buf_free(&pt);
    }
    if (fclose(out) != 0) die("error closing %s: %s", tmp, strerror(errno));
    uint32_t nchunks = m.nchunks;
    catman_free(&m);

    if (rename(tmp, c->src->db) != 0)
        die("cannot install catalog %s: %s", c->src->db, strerror(errno));

    log_info("catalog restored to %s (%u chunks); you can now run `bkup restore`",
             c->src->db, nchunks);
    return 0;
}
