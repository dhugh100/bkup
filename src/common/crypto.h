#ifndef BK_CRYPTO_H
#define BK_CRYPTO_H

#include <stdint.h>
#include <stddef.h>

#include "types.h"
#include "util.h"

#define BK_KEYBYTES   32        /* crypto_secretstream..._KEYBYTES */
#define BK_SALTBYTES  16        /* crypto_pwhash_SALTBYTES */

typedef struct { uint8_t k[BK_KEYBYTES]; } Key;

typedef struct {
    uint8_t  salt[BK_SALTBYTES];
    uint64_t ops;               /* crypto_pwhash opslimit */
    uint64_t mem;               /* crypto_pwhash memlimit, bytes */
} KdfParams;

/* Must be called once at startup before any other crypto_* call. */
void crypto_global_init(void);

/* Fill p with a fresh random salt and the default (MODERATE) cost limits. */
void crypto_kdf_default(KdfParams *p);

/* Derive the 32-byte repo key from passphrase + params (Argon2id).
   Returns 0 on success, -1 on failure (e.g. OOM at the requested memlimit). */
int crypto_derive_key(const char *passphrase, const KdfParams *p, Key *out);

/* Keycheck: a sealed copy of a fixed token, stored in the repo config so a
   wrong passphrase is detected before any real work. */
void crypto_keycheck_make(const Key *k, Buf *out);
int  crypto_keycheck_verify(const Key *k, const uint8_t *ct, size_t len);

/* In-memory authenticated encryption of a single blob.
   crypto_open returns 0 on success, -1 if authentication fails (tampering or
   wrong key). Output is appended to *out (caller buf_init's it). */
void crypto_seal(const Key *k, const uint8_t *pt, size_t ptlen, Buf *out);
int  crypto_open(const Key *k, const uint8_t *ct, size_t ctlen, Buf *out);

/* Streamed file encryption (for the catalog snapshot, too big to buffer).
   Return 0 on success, -1 on any I/O or authentication failure. */
int crypto_seal_file(const Key *k, const char *in_path, const char *out_path);
int crypto_open_file(const Key *k, const char *in_path, const char *out_path);

/* BLAKE2b-256 content hash. */
void bk_hash(const uint8_t *data, size_t len, uint8_t out[BK_HASH_LEN]);

/* Cryptographically secure random bytes. */
void crypto_random(uint8_t *buf, size_t n);

#endif
