#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include "commands.h"
#include "common/db.h"
#include "common/transport.h"
#include "common/log.h"

/* Remove a local file and its sqlite -wal/-shm sidecars; tolerate absence. */
static void remove_local_catalog(const char *db)
{
    static const char *suffix[] = { "", "-wal", "-shm" };
    for (int i = 0; i < 3; i++) {
        char *p = xmalloc(strlen(db) + strlen(suffix[i]) + 1);
        sprintf(p, "%s%s", db, suffix[i]);
        if (unlink(p) != 0 && errno != ENOENT)
            die("cannot remove local catalog %s: %s", p, strerror(errno));
        free(p);
    }
}

int cmd_init(Ctx *c, int argc, char **argv)
{
    int force = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--force") || !strcmp(argv[i], "-f")) force = 1;
        else die("init: unexpected argument '%s'", argv[i]);
    }
    ctx_connect(c);

    char *cfgpath = repo_path(c, "config");
    if (transport_exists(c->t, cfgpath) == 1 && !force)
        die("repo %s already initialized (config exists); "
            "pass --force to wipe it and start clean", c->src->repo);

    if (force) {
        log_warn("--force: wiping repo %s and local catalog %s",
                 c->src->repo, c->src->db);
        if (transport_rmtree(c->t, c->src->repo) != 0)
            die("failed to wipe existing repo %s", c->src->repo);
        remove_local_catalog(c->src->db);
    }

    char *pass = prompt_passphrase("New passphrase: ", 1, ctx_pass_source(c));

    RepoConf rc;
    memset(&rc, 0, sizeof rc);
    rc.version = 1;
    uint8_t id[16];
    crypto_random(id, sizeof id);
    hex_encode(id, sizeof id, rc.repo_id);
    crypto_kdf_default(&rc.kdf);

    log_info("deriving key (Argon2id, this takes a moment)...");
    if (crypto_derive_key(pass, &rc.kdf, &c->key) != 0)
        die("key derivation failed");
    c->have_key = 1;

    Buf kc; buf_init(&kc);
    crypto_keycheck_make(&c->key, &kc);
    if (kc.len > sizeof rc.keycheck) die("keycheck too large");
    memcpy(rc.keycheck, kc.data, kc.len);
    rc.keycheck_len = kc.len;
    buf_free(&kc);

    memset(pass, 0, strlen(pass));
    free(pass);

    /* create the repo skeleton on the server */
    char *repo  = xstrdup(c->src->repo);
    char *blobs = repo_path(c, "blobs");
    char *cat   = repo_path(c, "catalog");
    transport_mkdir_p(c->t, repo);
    transport_mkdir(c->t, blobs);
    transport_mkdir(c->t, cat);

    Buf cfgtext; buf_init(&cfgtext);
    repoconf_format(&rc, &cfgtext);
    if (transport_put(c->t, cfgpath, cfgtext.data, cfgtext.len, 0) != 0)
        die("failed to write repo config");
    buf_free(&cfgtext);

    /* create the local catalog and record the same parameters */
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

    free(cfgpath); free(repo); free(blobs); free(cat);

    log_info("initialized repo %s (id %s)", c->src->repo, rc.repo_id);
    return 0;
}
