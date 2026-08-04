#include <sodium.h>
#include <stdio.h>
#include <string.h>

#include "crypto.h"
#include "log.h"

/* Per-blob framing: magic, then the secretstream header, then one FINAL
   message. The whole blob is small enough to hold in memory. */
static const uint8_t BLOB_MAGIC[4] = { 'B', 'K', 'B', '1' };
/* Streamed-file framing: magic, header, then u32-length-prefixed messages. */
static const uint8_t FILE_MAGIC[4] = { 'B', 'K', 'F', '1' };

#define HDR  crypto_secretstream_xchacha20poly1305_HEADERBYTES  /* 24 */
#define ABY  crypto_secretstream_xchacha20poly1305_ABYTES       /* 17 */
#define TAG_MSG   crypto_secretstream_xchacha20poly1305_TAG_MESSAGE
#define TAG_FINAL crypto_secretstream_xchacha20poly1305_TAG_FINAL

/* Fixed token sealed at init; opening it with the derived key proves the
   passphrase is correct. Exactly 32 bytes, never changes across versions. */
static const uint8_t KEYCHECK_TOKEN[32] =
    "bkup-keycheck-token-version-001";

#define FILE_CHUNK (1024u * 1024u)   /* plaintext block size for streaming */

void crypto_global_init(void)
{
    if (sodium_init() < 0)
        die("libsodium initialization failed");
    /* Compile-time-ish sanity: our key size must match secretstream's. */
    if (crypto_secretstream_xchacha20poly1305_KEYBYTES != BK_KEYBYTES)
        die("key size mismatch with libsodium");
}

void crypto_kdf_default(KdfParams *p)
{
    randombytes_buf(p->salt, sizeof p->salt);
    p->ops = crypto_pwhash_OPSLIMIT_MODERATE;
    p->mem = crypto_pwhash_MEMLIMIT_MODERATE;
}

int crypto_derive_key(const char *passphrase, const KdfParams *p, Key *out)
{
    if (crypto_pwhash(out->k, sizeof out->k,
                      passphrase, strlen(passphrase),
                      p->salt,
                      (unsigned long long)p->ops, (size_t)p->mem,
                      crypto_pwhash_ALG_ARGON2ID13) != 0)
        return -1;
    return 0;
}

void crypto_seal(const Key *k, const uint8_t *pt, size_t ptlen, Buf *out)
{
    crypto_secretstream_xchacha20poly1305_state st;
    uint8_t header[HDR];

    crypto_secretstream_xchacha20poly1305_init_push(&st, header, k->k);
    buf_append(out, BLOB_MAGIC, sizeof BLOB_MAGIC);
    buf_append(out, header, sizeof header);

    buf_reserve(out, ptlen + ABY);
    unsigned long long clen = 0;
    crypto_secretstream_xchacha20poly1305_push(&st,
        out->data + out->len, &clen, pt, ptlen, NULL, 0, TAG_FINAL);
    out->len += clen;
}

int crypto_open(const Key *k, const uint8_t *ct, size_t ctlen, Buf *out)
{
    if (ctlen < sizeof BLOB_MAGIC + HDR + ABY) return -1;
    if (memcmp(ct, BLOB_MAGIC, sizeof BLOB_MAGIC) != 0) return -1;

    const uint8_t *header = ct + sizeof BLOB_MAGIC;
    const uint8_t *body   = header + HDR;
    size_t bodylen        = ctlen - sizeof BLOB_MAGIC - HDR;

    crypto_secretstream_xchacha20poly1305_state st;
    if (crypto_secretstream_xchacha20poly1305_init_pull(&st, header, k->k) != 0)
        return -1;

    buf_reserve(out, bodylen);     /* plaintext <= ciphertext */
    unsigned long long mlen = 0;
    unsigned char tag = 0;
    if (crypto_secretstream_xchacha20poly1305_pull(&st,
            out->data + out->len, &mlen, &tag,
            body, bodylen, NULL, 0) != 0)
        return -1;
    if (tag != TAG_FINAL) return -1;
    out->len += mlen;
    return 0;
}

void crypto_keycheck_make(const Key *k, Buf *out)
{
    crypto_seal(k, KEYCHECK_TOKEN, sizeof KEYCHECK_TOKEN, out);
}

int crypto_keycheck_verify(const Key *k, const uint8_t *ct, size_t len)
{
    Buf pt;
    buf_init(&pt);
    int rc = crypto_open(k, ct, len, &pt);
    if (rc == 0) {
        if (pt.len != sizeof KEYCHECK_TOKEN ||
            memcmp(pt.data, KEYCHECK_TOKEN, sizeof KEYCHECK_TOKEN) != 0)
            rc = -1;
    }
    buf_free(&pt);
    return rc;
}

static int write_all(FILE *f, const void *p, size_t n)
{
    return fwrite(p, 1, n, f) == n ? 0 : -1;
}

int crypto_seal_file(const Key *k, const char *in_path, const char *out_path)
{
    FILE *in = fopen(in_path, "rb");
    if (!in) return -1;
    FILE *out = fopen(out_path, "wb");
    if (!out) { fclose(in); return -1; }

    int rc = -1;
    crypto_secretstream_xchacha20poly1305_state st;
    uint8_t header[HDR];
    crypto_secretstream_xchacha20poly1305_init_push(&st, header, k->k);

    uint8_t *pt = xmalloc(FILE_CHUNK);
    uint8_t *ct = xmalloc(FILE_CHUNK + ABY);

    if (write_all(out, FILE_MAGIC, sizeof FILE_MAGIC) != 0) goto done;
    if (write_all(out, header, sizeof header) != 0) goto done;

    for (;;) {
        size_t n = fread(pt, 1, FILE_CHUNK, in);
        int eof = feof(in);
        if (ferror(in)) goto done;
        unsigned char tag = eof ? TAG_FINAL : TAG_MSG;
        unsigned long long clen = 0;
        crypto_secretstream_xchacha20poly1305_push(&st, ct, &clen,
            pt, n, NULL, 0, tag);
        uint32_t framelen = (uint32_t)clen;
        if (write_all(out, &framelen, sizeof framelen) != 0) goto done;
        if (write_all(out, ct, clen) != 0) goto done;
        if (eof) break;
    }
    rc = (fflush(out) == 0) ? 0 : -1;

done:
    free(pt); free(ct);
    fclose(in);
    if (fclose(out) != 0) rc = -1;
    return rc;
}

int crypto_open_file(const Key *k, const char *in_path, const char *out_path)
{
    FILE *in = fopen(in_path, "rb");
    if (!in) return -1;
    FILE *out = fopen(out_path, "wb");
    if (!out) { fclose(in); return -1; }

    int rc = -1;
    uint8_t magic[4], header[HDR];
    if (fread(magic, 1, sizeof magic, in) != sizeof magic) goto done;
    if (memcmp(magic, FILE_MAGIC, sizeof magic) != 0) goto done;
    if (fread(header, 1, sizeof header, in) != sizeof header) goto done;

    crypto_secretstream_xchacha20poly1305_state st;
    if (crypto_secretstream_xchacha20poly1305_init_pull(&st, header, k->k) != 0)
        goto done;

    uint8_t *ct = xmalloc(FILE_CHUNK + ABY);
    uint8_t *pt = xmalloc(FILE_CHUNK);
    int finished = 0;
    for (;;) {
        uint32_t framelen;
        size_t r = fread(&framelen, 1, sizeof framelen, in);
        if (r == 0 && feof(in)) break;
        if (r != sizeof framelen) goto done2;
        if (framelen > FILE_CHUNK + ABY || framelen < ABY) goto done2;
        if (fread(ct, 1, framelen, in) != framelen) goto done2;

        unsigned long long mlen = 0;
        unsigned char tag = 0;
        if (crypto_secretstream_xchacha20poly1305_pull(&st, pt, &mlen, &tag,
                ct, framelen, NULL, 0) != 0)
            goto done2;
        if (write_all(out, pt, mlen) != 0) goto done2;
        if (tag == TAG_FINAL) { finished = 1; break; }
    }
    rc = finished ? 0 : -1;     /* truncated stream if no FINAL tag seen */

done2:
    free(ct); free(pt);
done:
    fclose(in);
    if (fflush(out) != 0 || fclose(out) != 0) rc = -1;
    return rc;
}

void bk_hash(const uint8_t *data, size_t len, uint8_t out[BK_HASH_LEN])
{
    crypto_generichash(out, BK_HASH_LEN, data, len, NULL, 0);
}

void crypto_random(uint8_t *buf, size_t n)
{
    randombytes_buf(buf, n);
}
