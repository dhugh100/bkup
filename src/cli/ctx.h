#ifndef BK_CTX_H
#define BK_CTX_H

#include <sqlite3.h>
#include <sys/types.h>

#include "common/config.h"
#include "common/crypto.h"
#include "common/transport.h"
#include "common/util.h"

/* The plaintext repo config stored on the server at repo/config. It holds
   everything needed to derive the key from a passphrase with NO local state,
   which is what makes disaster recovery (fetch-catalog) possible. */
typedef struct {
    int       version;
    char      repo_id[33];          /* 32 hex chars + NUL */
    KdfParams kdf;
    uint8_t   keycheck[128];
    size_t    keycheck_len;
} RepoConf;

typedef struct {
    Config    *cfg;
    User    *src;                 /* the selected source (per-source settings) */
    sqlite3   *db;                  /* may be NULL (fetch-catalog) */
    Transport *t;                   /* may be NULL until ctx_connect */
    Key        key;
    int        have_key;
} Ctx;

Ctx *ctx_new(const char *config_path);   /* loads config, selects the current user's source */
Ctx *ctx_new_uid(const char *config_path, uid_t uid); /* select the uid's source; die if absent */
Ctx *ctx_new_nosel(const char *config_path); /* load config, select no source */
void ctx_use_user(Ctx *c, const char *name);  /* select source by name; die if absent */

/* The passphrase source path (global key_file, else the source's
   passphrase_file). NULL if neither is configured. */
const char *ctx_pass_source(Ctx *c);
void ctx_open_db(Ctx *c);                /* opens the local catalog */
void ctx_connect(Ctx *c);                /* opens the SFTP session */
void ctx_free(Ctx *c);

/* Build a path inside the repo: repo_path(c,"blobs") -> "<repo>/blobs". */
char *repo_path(Ctx *c, const char *sub);
/* Full repo path of a blob: "<repo>/blobs/<ab>/<64hex>". */
char *blob_repo_path(Ctx *c, const char hex[BK_HEX_LEN + 1]);

/* Return the passphrase: read from passphrase_file if non-NULL, else from
   $BKUP_PASSPHRASE, else prompt on the tty (echo off).  If confirm, ask twice.
   Dies if no source is available.  Caller frees. */
char *prompt_passphrase(const char *prompt, int confirm, const char *passphrase_file);

/* RepoConf <-> text serialization (stored as the repo/config object). */
void repoconf_format(const RepoConf *rc, Buf *out);
int  repoconf_parse(const uint8_t *data, size_t len, RepoConf *rc);

#endif
