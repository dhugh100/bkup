#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <sys/file.h>
#include <sys/stat.h>

#include "commands.h"
#include "hook.h"
#include "common/db.h"
#include "common/types.h"
#include "common/chunk.h"
#include "common/catalog_chunk.h"
#include "common/compress.h"
#include "common/crypto.h"
#include "common/transport.h"
#include "common/log.h"

static void wipe_pass(char *s);

/* ---- shared key derivation (used by backup, restore, verify) ---- */

void key_from_meta(Ctx *c)
{
    char *salt_hex = db_meta_get(c->db, "kdf_salt");
    char *ops_s    = db_meta_get(c->db, "kdf_ops");
    char *mem_s    = db_meta_get(c->db, "kdf_mem");
    char *kc_hex   = db_meta_get(c->db, "keycheck");
    if (!salt_hex || !ops_s || !mem_s || !kc_hex)
        die_perm("catalog is missing KDF metadata; run `bkup init` or `fetch-catalog`");

    KdfParams kdf;
    if (hex_decode(salt_hex, kdf.salt, BK_SALTBYTES) != 0)
        die_perm("corrupt kdf_salt in catalog");
    kdf.ops = strtoull(ops_s, NULL, 10);
    kdf.mem = strtoull(mem_s, NULL, 10);

    size_t kc_len = strlen(kc_hex) / 2;
    uint8_t kc[128];
    if (kc_len > sizeof kc || hex_decode(kc_hex, kc, kc_len) != 0)
        die_perm("corrupt keycheck in catalog");

    char *pass = prompt_passphrase("Passphrase: ", 0, ctx_pass_source(c));
    if (crypto_derive_key(pass, &kdf, &c->key) != 0)
        die("key derivation failed (out of memory?)");   /* transient: resource pressure */
    wipe_pass(pass);
    if (crypto_keycheck_verify(&c->key, kc, kc_len) != 0)
        die_perm("wrong passphrase");
    c->have_key = 1;

    free(salt_hex); free(ops_s); free(mem_s); free(kc_hex);
}

/* wipe+free a passphrase string so it does not linger in the heap */
static void wipe_pass(char *s)
{
    if (s) { memset(s, 0, strlen(s)); free(s); }
}

/* ---- repo id guard ---- */

void check_repo_id(Ctx *c)
{
    char *cfgpath = repo_path(c, "config");
    Buf raw; buf_init(&raw);
    if (transport_get(c->t, cfgpath, &raw) != 0)
        die("cannot read repo config at %s (is the repo initialized?)", cfgpath);
    RepoConf rc;
    if (repoconf_parse(raw.data, raw.len, &rc) != 0)
        die_perm("repo config is malformed");
    char *local_id = db_meta_get(c->db, "repo_id");
    if (!local_id || strcmp(local_id, rc.repo_id) != 0)
        die_perm("repo id mismatch: catalog=%s server=%s (wrong server/repo?)",
            local_id ? local_id : "(none)", rc.repo_id);
    free(local_id);
    free(cfgpath);
    buf_free(&raw);
}

/* ---- blob upload ---- */

static void ensure_blob_dir(Ctx *c, const char hex[BK_HEX_LEN + 1])
{
    char sub[16];
    snprintf(sub, sizeof sub, "blobs/%c%c", hex[0], hex[1]);
    char *dir = path_join(c->src->repo, sub);
    transport_mkdir(c->t, dir);
    free(dir);
}

/* Returns 1 if the chunk was newly uploaded, 0 if it was a dedup hit.
   The caller supplies the precomputed chunk hash. */
static int process_blob(Ctx *c, const uint8_t h[BK_HASH_LEN],
                        const uint8_t *data, size_t len, Stats *st)
{
    char hex[BK_HEX_LEN + 1];
    hex_encode(h, BK_HASH_LEN, hex);

    st->chunks_total++;
    st->bytes_raw += (long long)len;

    sqlite3_stmt *sel = db_prep(c->db, "SELECT state FROM blobs WHERE hash=?");
    sqlite3_bind_blob(sel, 1, h, BK_HASH_LEN, SQLITE_STATIC);
    int have = (sqlite3_step(sel) == SQLITE_ROW);
    int state = have ? sqlite3_column_int(sel, 0) : -1;
    sqlite3_finalize(sel);

    if (have && state == BS_UPLOADED) {
        st->chunks_dedup++;
        return 0;
    }
    if (!have) {
        sqlite3_stmt *ins = db_prep(c->db,
            "INSERT INTO blobs(hash,size,state) VALUES(?,?,0)");
        sqlite3_bind_blob(ins, 1, h, BK_HASH_LEN, SQLITE_STATIC);
        sqlite3_bind_int64(ins, 2, (long long)len);
        db_step_done(c->db, ins, "blob insert");
        sqlite3_finalize(ins);
    }

    char *remote = blob_repo_path(c, hex);

    /* resume: a previous run may have uploaded this blob before crashing */
    if (transport_exists(c->t, remote) == 1) {
        sqlite3_stmt *u = db_prep(c->db,
            "UPDATE blobs SET state=1 WHERE hash=?");
        sqlite3_bind_blob(u, 1, h, BK_HASH_LEN, SQLITE_STATIC);
        db_step_done(c->db, u, "blob resume-mark");
        sqlite3_finalize(u);
        free(remote);
        st->chunks_dedup++;
        return 0;
    }

    Buf comp; buf_init(&comp);
    zstd_compress(data, len, BK_ZSTD_LEVEL, &comp);
    Buf ct; buf_init(&ct);
    crypto_seal(&c->key, comp.data, comp.len, &ct);
    buf_free(&comp);

    ensure_blob_dir(c, hex);
    if (transport_put(c->t, remote, ct.data, ct.len, 0) != 0)
        die("upload of blob %s failed", hex);

    sqlite3_stmt *u = db_prep(c->db,
        "UPDATE blobs SET state=1, stored_size=? WHERE hash=?");
    sqlite3_bind_int64(u, 1, (long long)ct.len);
    sqlite3_bind_blob(u, 2, h, BK_HASH_LEN, SQLITE_STATIC);
    db_step_done(c->db, u, "blob upload-mark");
    sqlite3_finalize(u);

    st->chunks_new++;
    st->bytes_new += (long long)len;
    st->bytes_stored += (long long)ct.len;
    free(remote);
    buf_free(&ct);
    return 1;
}

/* Chunk a regular file, uploading new blobs. Appends each chunk hash to
   *hashes (a Buf of raw 32-byte hashes). Returns bytes read, or -1 on open
   failure. */
static long long chunk_file(Ctx *c, const char *path, Buf *hashes, Stats *st)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        log_warn("cannot open %s (%s) -- left for the next backup, not yet "
                 "backed up", path, strerror(errno));
        return -1;
    }

    Buf win; buf_init(&win);
    long long total = 0;
    int eof = 0;
    uint8_t tmp[262144];

    while (!eof || win.len > 0) {
        while (!eof && win.len < BK_CHUNK_MAX) {
            ssize_t r = read(fd, tmp, sizeof tmp);
            if (r < 0) {
                log_warn("read error on %s (%s) -- left for the next backup, "
                         "not yet backed up", path, strerror(errno));
                close(fd); buf_free(&win); return -1;
            }
            if (r == 0) { eof = 1; break; }
            buf_append(&win, tmp, (size_t)r);
        }
        if (win.len == 0) break;

        size_t clen = chunk_cut(win.data, win.len, eof);
        uint8_t h[BK_HASH_LEN];
        bk_hash(win.data, clen, h);
        process_blob(c, h, win.data, clen, st);
        buf_append(hashes, h, BK_HASH_LEN);
        total += (long long)clen;
        buf_consume_front(&win, clen);
    }
    close(fd);
    buf_free(&win);
    return total;
}

/* Build the version row + manifest for one file and mark it clean/committed. */
static void commit_file(Ctx *c, long long snap, long long file_id,
                        const char *path, int kind,
                        long long mtime_sec, long long mtime_nsec,
                        long long mode, long long uid, long long gid,
                        long long size, const char *link_target,
                        const Buf *hashes, int still_dirty)
{
    db_exec(c->db, "BEGIN");

    /* close the file's previous current version: it was live up to the prior
       snapshot, so its half-open interval ends at this one. No-op for a file
       being captured for the first time (files.version_id IS NULL). */
    sqlite3_stmt *cl = db_prep(c->db,
        "UPDATE versions SET last_snapshot=? WHERE last_snapshot IS NULL "
        "AND id=(SELECT version_id FROM files WHERE id=?)");
    sqlite3_bind_int64(cl, 1, snap);
    sqlite3_bind_int64(cl, 2, file_id);
    db_step_done(c->db, cl, "version supersession close");
    sqlite3_finalize(cl);

    sqlite3_stmt *iv = db_prep(c->db,
        "INSERT INTO versions(path,kind,size,mtime_sec,mtime_nsec,"
        "mode,uid,gid,link_target,first_snapshot,last_snapshot) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,NULL)");
    sqlite3_bind_text (iv, 1, path, -1, SQLITE_STATIC);
    sqlite3_bind_int  (iv, 2, kind);
    sqlite3_bind_int64(iv, 3, size);
    sqlite3_bind_int64(iv, 4, mtime_sec);
    sqlite3_bind_int64(iv, 5, mtime_nsec);
    sqlite3_bind_int64(iv, 6, mode);
    sqlite3_bind_int64(iv, 7, uid);
    sqlite3_bind_int64(iv, 8, gid);
    if (link_target) sqlite3_bind_text(iv, 9, link_target, -1, SQLITE_STATIC);
    else             sqlite3_bind_null(iv, 9);
    sqlite3_bind_int64(iv, 10, snap);   /* first_snapshot (inclusive) */
    if (sqlite3_step(iv) != SQLITE_DONE)
        die("insert version failed: %s", sqlite3_errmsg(c->db));
    sqlite3_finalize(iv);
    long long vid = sqlite3_last_insert_rowid(c->db);

    size_t nh = hashes->len / BK_HASH_LEN;
    sqlite3_stmt *ivb = db_prep(c->db,
        "INSERT INTO version_blobs(version_id,seq,hash) VALUES(?,?,?)");
    for (size_t i = 0; i < nh; i++) {
        sqlite3_reset(ivb);
        sqlite3_bind_int64(ivb, 1, vid);
        sqlite3_bind_int64(ivb, 2, (long long)i);
        sqlite3_bind_blob (ivb, 3, hashes->data + i * BK_HASH_LEN,
                           BK_HASH_LEN, SQLITE_STATIC);
        if (sqlite3_step(ivb) != SQLITE_DONE)
            die("insert version_blob failed: %s", sqlite3_errmsg(c->db));
    }
    sqlite3_finalize(ivb);

    sqlite3_stmt *uf = db_prep(c->db,
        "UPDATE files SET version_id=?, state=? WHERE id=?");
    sqlite3_bind_int64(uf, 1, vid);
    sqlite3_bind_int  (uf, 2, still_dirty ? FS_DIRTY : FS_CLEAN);
    sqlite3_bind_int64(uf, 3, file_id);
    db_step_done(c->db, uf, "file state update");
    sqlite3_finalize(uf);
    db_exec(c->db, "COMMIT");
}

void process_file(Ctx *c, long long snap, long long file_id, Stats *st)
{
    sqlite3_stmt *q = db_prep(c->db,
        "SELECT path,kind,size,mtime_sec,mtime_nsec,mode,uid,gid "
        "FROM files WHERE id=?");
    sqlite3_bind_int64(q, 1, file_id);
    if (sqlite3_step(q) != SQLITE_ROW) { sqlite3_finalize(q); return; }

    char *path = xstrdup((const char *)sqlite3_column_text(q, 0));
    int kind   = sqlite3_column_int(q, 1);
    long long size      = sqlite3_column_int64(q, 2);
    long long mtime_sec = sqlite3_column_int64(q, 3);
    long long mtime_nsec= sqlite3_column_int64(q, 4);
    long long mode      = sqlite3_column_int64(q, 5);
    long long uid       = sqlite3_column_int64(q, 6);
    long long gid       = sqlite3_column_int64(q, 7);
    sqlite3_finalize(q);

    Buf hashes; buf_init(&hashes);
    char *link_target = NULL;
    int still_dirty = 0;
    long long stored_size = size;

    if (kind == FK_REG) {
        struct stat pre, post;
        int havepre = (lstat(path, &pre) == 0);
        long long n = chunk_file(c, path, &hashes, st);
        if (n < 0) { free(path); buf_free(&hashes); return; }  /* stays dirty */
        stored_size = n;
        /* torn-read guard: if the file changed under us, keep it dirty */
        if (havepre && lstat(path, &post) == 0) {
            if (pre.st_size != post.st_size ||
                pre.st_mtim.tv_sec != post.st_mtim.tv_sec ||
                pre.st_mtim.tv_nsec != post.st_mtim.tv_nsec)
                still_dirty = 1;
        }
    } else if (kind == FK_SYMLINK) {
        char buf[PATH_MAX];
        ssize_t n = readlink(path, buf, sizeof buf - 1);
        if (n < 0) { log_warn("cannot readlink %s, skipping", path);
                     free(path); buf_free(&hashes); return; }
        buf[n] = '\0';
        link_target = xstrdup(buf);
        stored_size = 0;
    } else { /* FK_DIR */
        stored_size = 0;
    }

    commit_file(c, snap, file_id, path, kind, mtime_sec, mtime_nsec,
                mode, uid, gid, stored_size, link_target, &hashes, still_dirty);
    st->files_changed++;

    free(path);
    free(link_target);
    buf_free(&hashes);
}

/* ---- catalog snapshot upload (content-defined, deduplicated) ----

   The catalog is VACUUMed to a compact file, chunked with the small CAT_PARAMS
   chunker, and only chunks not already on the server are sealed and uploaded.
   A small encrypted manifest names the chunks in order; catalog/latest points
   at it. Two snapshots minutes apart differ in only a few SQLite pages, so a
   push uploads a handful of chunks instead of the whole catalog.

   A sidecar DB (<catalog>.catmeta) remembers which chunks are already uploaded
   so dedup needs no per-chunk round trip. It is a pure local cache: never
   uploaded, and safe to lose -- a missing entry just falls back to a remote
   existence check (or, worst case, a re-upload, which is idempotent). */

#define CAT_BLOB_PREFIX "catalog/blobs"

static char *cat_blob_path(Ctx *c, const char hex[BK_HEX_LEN + 1])
{
    char sub[sizeof CAT_BLOB_PREFIX + 4 + BK_HEX_LEN];
    snprintf(sub, sizeof sub, CAT_BLOB_PREFIX "/%c%c/%s", hex[0], hex[1], hex);
    return repo_path(c, sub);
}

static void cat_ensure_blob_dir(Ctx *c, const char hex[BK_HEX_LEN + 1])
{
    char sub[32];
    snprintf(sub, sizeof sub, CAT_BLOB_PREFIX "/%c%c", hex[0], hex[1]);
    char *dir = repo_path(c, sub);
    transport_mkdir(c->t, dir);
    free(dir);
}

/* Local-only dedup cache. Raw sqlite (not db_open, which would graft the whole
   catalog schema onto the sidecar). */
static sqlite3 *cat_meta_open(Ctx *c)
{
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s.catmeta", c->src->db);
    sqlite3 *m = NULL;
    if (sqlite3_open(path, &m) != SQLITE_OK)
        die("cannot open catalog meta cache %s: %s", path, sqlite3_errmsg(m));
    db_exec(m, "PRAGMA busy_timeout=10000");
    db_exec(m, "CREATE TABLE IF NOT EXISTS cat_chunks("
               "hash BLOB PRIMARY KEY, stored_size INTEGER)");
    db_exec(m, "CREATE TABLE IF NOT EXISTS cat_manifests(name TEXT PRIMARY KEY)");
    return m;
}

/* Read an entire local file into *out. die()s on failure. Returns byte count. */
static long long read_file_all(const char *path, Buf *out)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) die("cannot open %s: %s", path, strerror(errno));
    uint8_t tmp[262144];
    long long total = 0;
    for (;;) {
        ssize_t r = read(fd, tmp, sizeof tmp);
        if (r < 0) die("read error on %s: %s", path, strerror(errno));
        if (r == 0) break;
        buf_append(out, tmp, (size_t)r);
        total += r;
    }
    close(fd);
    return total;
}

/* Upload one catalog chunk unless it is already on the server, then record it
   in the sidecar. */
static void cat_upload_chunk(Ctx *c, sqlite3 *meta, const uint8_t h[BK_HASH_LEN],
                             const uint8_t *data, size_t len)
{
    sqlite3_stmt *sel = db_prep(meta, "SELECT 1 FROM cat_chunks WHERE hash=?");
    sqlite3_bind_blob(sel, 1, h, BK_HASH_LEN, SQLITE_STATIC);
    int have = (sqlite3_step(sel) == SQLITE_ROW);
    sqlite3_finalize(sel);
    if (have) return;

    char hex[BK_HEX_LEN + 1];
    hex_encode(h, BK_HASH_LEN, hex);
    char *remote = cat_blob_path(c, hex);

    long long stored = 0;
    if (transport_exists(c->t, remote) != 1) {
        Buf comp; buf_init(&comp);
        zstd_compress(data, len, BK_ZSTD_LEVEL, &comp);
        Buf ct; buf_init(&ct);
        crypto_seal(&c->key, comp.data, comp.len, &ct);
        buf_free(&comp);
        cat_ensure_blob_dir(c, hex);
        if (transport_put(c->t, remote, ct.data, ct.len, 0) != 0)
            die("upload of catalog chunk %s failed", hex);
        stored = (long long)ct.len;
        buf_free(&ct);
    }

    sqlite3_stmt *ins = db_prep(meta,
        "INSERT OR IGNORE INTO cat_chunks(hash,stored_size) VALUES(?,?)");
    sqlite3_bind_blob(ins, 1, h, BK_HASH_LEN, SQLITE_STATIC);
    sqlite3_bind_int64(ins, 2, stored);
    db_step_done(meta, ins, "cat_chunk insert");
    sqlite3_finalize(ins);
    free(remote);
}

/* Delete catalog chunks no longer named by the current manifest. Runs after the
   manifest and latest pointer are committed.

   Order matters, and it is the reverse of the intuitive one: forget the chunk
   in the sidecar FIRST, then delete the remote object. cat_upload_chunk trusts
   a sidecar row absolutely (it skips the upload without asking the server), so
   a row that outlives its object turns every later manifest naming that content
   into a dangling reference -- fetch-catalog is then unrecoverable (this lost
   roscoe root's history, 2026-07-14). A row removed before the object is the
   safe failure: the chunk is merely orphaned, and the next push that needs its
   content misses the sidecar, finds the object via transport_exists, and
   re-adopts it. Interrupted GC therefore only ever leaks orphans. */
static void cat_gc(Ctx *c, sqlite3 *meta, const uint8_t *hashes, uint32_t nchunks)
{
    db_exec(meta, "CREATE TEMP TABLE cur(hash BLOB PRIMARY KEY)");
    sqlite3_stmt *ins = db_prep(meta, "INSERT OR IGNORE INTO cur(hash) VALUES(?)");
    for (uint32_t i = 0; i < nchunks; i++) {
        sqlite3_reset(ins);
        sqlite3_bind_blob(ins, 1, hashes + (size_t)i * BK_HASH_LEN,
                          BK_HASH_LEN, SQLITE_STATIC);
        sqlite3_step(ins);
    }
    sqlite3_finalize(ins);

    /* materialize stale hashes before doing IO + row deletes */
    Buf stale; buf_init(&stale);
    sqlite3_stmt *q = db_prep(meta,
        "SELECT hash FROM cat_chunks WHERE hash NOT IN (SELECT hash FROM cur)");
    while (sqlite3_step(q) == SQLITE_ROW) {
        if (sqlite3_column_bytes(q, 0) == BK_HASH_LEN)
            buf_append(&stale, sqlite3_column_blob(q, 0), BK_HASH_LEN);
    }
    sqlite3_finalize(q);

    size_t ns = stale.len / BK_HASH_LEN;
    sqlite3_stmt *del = db_prep(meta, "DELETE FROM cat_chunks WHERE hash=?");
    for (size_t i = 0; i < ns; i++) {
        const uint8_t *h = stale.data + i * BK_HASH_LEN;
        char hex[BK_HEX_LEN + 1];
        hex_encode(h, BK_HASH_LEN, hex);
        char *remote = cat_blob_path(c, hex);
        sqlite3_reset(del);
        sqlite3_bind_blob(del, 1, h, BK_HASH_LEN, SQLITE_STATIC);
        if (sqlite3_step(del) != SQLITE_DONE) {
            /* Row survives, object survives: still consistent. Skip the
               object so the row never over-claims; retried next push. */
            log_warn("catalog GC: could not forget %s (%s); keeping object",
                     hex, sqlite3_errmsg(meta));
            free(remote);
            continue;
        }
        if (transport_delete(c->t, remote) != 0)
            log_warn("catalog GC: could not delete %s (orphan; re-adopted if "
                     "its content recurs)", hex);
        free(remote);
    }
    sqlite3_finalize(del);
    buf_free(&stale);
    db_exec(meta, "DROP TABLE cur");
}

/* Delete superseded manifest objects, keeping only `keep`. Tracked in the
   sidecar so this needs no remote directory listing. Runs after catalog/latest
   already names `keep`, so an interrupted prune only leaks old manifests, which
   the next push retries. (Pre-existing monolithic catalog-*.db.enc objects from
   before the chunked format are not tracked here and are left untouched.) */
static void cat_prune_manifests(Ctx *c, sqlite3 *meta, const char *keep)
{
    sqlite3_stmt *ins = db_prep(meta,
        "INSERT OR IGNORE INTO cat_manifests(name) VALUES(?)");
    sqlite3_bind_text(ins, 1, keep, -1, SQLITE_STATIC);
    db_step_done(meta, ins, "cat_manifest insert");
    sqlite3_finalize(ins);

    /* materialize stale names before doing IO + row deletes */
    char **stale = NULL;
    int ns = 0, cap = 0;
    sqlite3_stmt *q = db_prep(meta, "SELECT name FROM cat_manifests WHERE name<>?");
    sqlite3_bind_text(q, 1, keep, -1, SQLITE_STATIC);
    while (sqlite3_step(q) == SQLITE_ROW) {
        const char *nm = (const char *)sqlite3_column_text(q, 0);
        if (!nm) continue;
        if (ns == cap) { cap = cap ? cap * 2 : 8;
                         stale = xrealloc(stale, (size_t)cap * sizeof *stale); }
        stale[ns++] = xstrdup(nm);
    }
    sqlite3_finalize(q);

    sqlite3_stmt *del = db_prep(meta, "DELETE FROM cat_manifests WHERE name=?");
    for (int i = 0; i < ns; i++) {
        char rel[256];
        snprintf(rel, sizeof rel, "catalog/%s", stale[i]);
        char *remote = repo_path(c, rel);
        if (transport_delete(c->t, remote) == 0) {
            sqlite3_reset(del);
            sqlite3_bind_text(del, 1, stale[i], -1, SQLITE_STATIC);
            sqlite3_step(del);
        } else {
            log_warn("catalog GC: could not delete manifest %s (retry next push)",
                     stale[i]);
        }
        free(remote);
        free(stale[i]);
    }
    sqlite3_finalize(del);
    free(stale);
}

void upload_catalog(Ctx *c, long long snap)
{
    /* One push at a time per user, across processes. The daemon's op mutex
       only covers its own threads; a direct CLI run bypasses it, and two
       interleaved pushes can dangle a reference: A sees chunk X in the
       sidecar and skips the upload, B's GC deletes X, A's manifest still
       names it. The lock spans snapshot through GC so the sidecar-consult
       and the GC it must agree with are atomic. Blocking flock: pushes are
       short and callers (scheduler, continuous, prune) can afford to wait.
       O_RDONLY so whichever of root/the user creates it, the other can
       still open it (flock does not need write access). Released on close
       -- including by exit, so a die() cannot wedge later pushes. */
    char lockpath[PATH_MAX];
    snprintf(lockpath, sizeof lockpath, "%s.catmeta.lock", c->src->db);
    int lockfd = open(lockpath, O_RDONLY | O_CREAT | O_CLOEXEC, 0644);
    if (lockfd < 0)
        die("cannot open catalog push lock %s: %s", lockpath, strerror(errno));
    if (flock(lockfd, LOCK_EX) != 0)
        die("cannot lock %s: %s", lockpath, strerror(errno));

    char snapfile[PATH_MAX];
    snprintf(snapfile, sizeof snapfile, "%s.snap", c->src->db);
    unlink(snapfile);

    char *sql = sqlite3_mprintf("VACUUM INTO %Q", snapfile);
    db_exec(c->db, sql);
    sqlite3_free(sql);

    Buf plain; buf_init(&plain);
    long long db_size = read_file_all(snapfile, &plain);

    sqlite3 *meta = cat_meta_open(c);

    /* ensure catalog/ and catalog/blobs/ exist before uploading any chunk;
       per-chunk cat_ensure_blob_dir then only needs the 2-char fan-out level */
    char *blobsdir = repo_path(c, CAT_BLOB_PREFIX);
    transport_mkdir_p(c->t, blobsdir);
    free(blobsdir);

    /* chunk the fully buffered file (final=1 on every cut) and upload novelties */
    Buf hashes; buf_init(&hashes);
    size_t off = 0;
    while (off < plain.len) {
        size_t clen = chunk_cut_ex(plain.data + off, plain.len - off, 1, &CAT_PARAMS);
        const uint8_t *chunk = plain.data + off;
        uint8_t h[BK_HASH_LEN];
        bk_hash(chunk, clen, h);
        buf_append(&hashes, h, BK_HASH_LEN);
        cat_upload_chunk(c, meta, h, chunk, clen);
        off += clen;
    }
    uint32_t nchunks = (uint32_t)(hashes.len / BK_HASH_LEN);

    /* manifest -> seal -> upload */
    Buf man; buf_init(&man);
    catman_serialize(&man, snap, db_size, hashes.data, nchunks);
    Buf manct; buf_init(&manct);
    crypto_seal(&c->key, man.data, man.len, &manct);

    time_t now = time(NULL);
    char name[128];
    snprintf(name, sizeof name, "manifest-%lld-%ld.enc", snap, (long)now);
    char rel[256];
    snprintf(rel, sizeof rel, "catalog/%s", name);
    char *rpath = repo_path(c, rel);
    if (transport_put(c->t, rpath, manct.data, manct.len, 0) != 0)
        die("failed to upload catalog manifest");
    free(rpath);

    /* flip the latest pointer (overwrite) only after the manifest is durable */
    char *latest = repo_path(c, "catalog/latest");
    if (transport_put(c->t, latest, (const uint8_t *)name, strlen(name), 1) != 0)
        die("failed to update catalog/latest pointer");
    free(latest);

    /* reclaim chunks the new manifest no longer references, and the manifests
       this push superseded */
    cat_gc(c, meta, hashes.data, nchunks);
    cat_prune_manifests(c, meta, name);

    sqlite3_close(meta);
    buf_free(&man); buf_free(&manct);
    buf_free(&hashes); buf_free(&plain);
    unlink(snapfile);
    close(lockfd);   /* releases the flock */
}

/* ---- the command ---- */

int cmd_backup(Ctx *c, int argc, char **argv)
{
    (void)argc; (void)argv;
    user_check_ownership(c->src);
    ctx_open_db(c);
    ctx_connect(c);
    key_from_meta(c);
    check_repo_id(c);

    /* recovery sweep: roll back any interrupted (OPEN) snapshot's interval edits.
       Reopen versions it closed (last_snapshot pointed at the open snapshot) and
       drop the versions it created; then remove the snapshot. Uploaded blobs
       survive because they are shared, content-addressed state. */
    db_exec(c->db,
        "UPDATE versions SET last_snapshot=NULL WHERE last_snapshot IN "
        "(SELECT id FROM snapshots WHERE state=0)");
    db_exec(c->db,
        "DELETE FROM version_blobs WHERE version_id IN "
        "(SELECT id FROM versions WHERE first_snapshot IN "
        " (SELECT id FROM snapshots WHERE state=0))");
    db_exec(c->db,
        "DELETE FROM versions WHERE first_snapshot IN "
        "(SELECT id FROM snapshots WHERE state=0)");
    db_exec(c->db, "DELETE FROM snapshots WHERE state=0");

    /* From here until hook_backup_end, the post-backup hook runs even if a
       die() cuts the backup short (see hook.c). */
    hook_backup_begin(c->src);

    char host[256] = "";
    gethostname(host, sizeof host - 1);
    sqlite3_stmt *is = db_prep(c->db,
        "INSERT INTO snapshots(created,state,kind,hostname) VALUES(?,0,?,?)");
    sqlite3_bind_int64(is, 1, (long long)time(NULL));
    sqlite3_bind_int  (is, 2, SK_SCHEDULED);
    sqlite3_bind_text (is, 3, host, -1, SQLITE_STATIC);
    db_step_done(c->db, is, "snapshot insert");
    sqlite3_finalize(is);
    long long snap = sqlite3_last_insert_rowid(c->db);

    log_info("snapshot %lld: scanning %d source(s)", snap, c->src->nsources);
    scan_run(c, snap);

    /* Unchanged files need no work: their version's interval stays open
       (last_snapshot IS NULL), which already means "live in this snapshot." */

    /* materialize the dirty list before mutating rows */
    Buf ids; buf_init(&ids);
    sqlite3_stmt *dq = db_prep(c->db, "SELECT id FROM files WHERE state=1");
    while (sqlite3_step(dq) == SQLITE_ROW) {
        long long id = sqlite3_column_int64(dq, 0);
        buf_append(&ids, &id, sizeof id);
    }
    sqlite3_finalize(dq);

    Stats st; memset(&st, 0, sizeof st);
    size_t nids = ids.len / sizeof(long long);
    for (size_t i = 0; i < nids; i++) {
        long long id;
        memcpy(&id, ids.data + i * sizeof(long long), sizeof id);
        process_file(c, snap, id, &st);
    }
    buf_free(&ids);

    /* safety: every blob reachable from this snapshot must be UPLOADED. Only the
       versions created in this snapshot (first_snapshot=snap) can carry new
       blobs; carried-forward versions had theirs uploaded (and checked) when
       they were first captured, and blobs never revert from UPLOADED. */
    sqlite3_stmt *chk = db_prep(c->db,
        "SELECT COUNT(*) FROM versions v "
        "JOIN version_blobs vb ON vb.version_id=v.id "
        "JOIN blobs b ON b.hash=vb.hash "
        "WHERE v.first_snapshot=? AND b.state!=1");
    sqlite3_bind_int64(chk, 1, snap);
    sqlite3_step(chk);
    long long pending = sqlite3_column_int64(chk, 0);
    sqlite3_finalize(chk);
    if (pending != 0)
        die("internal error: %lld blobs not uploaded; snapshot left OPEN",
            pending);

    sqlite3_stmt *fin = db_prep(c->db,
        "UPDATE snapshots SET state=1 WHERE id=?");
    sqlite3_bind_int64(fin, 1, snap);
    db_step_done(c->db, fin, "snapshot finalize");
    sqlite3_finalize(fin);

    upload_catalog(c, snap);
    hook_backup_end();

    log_info("snapshot %lld complete: %lld changed, %lld chunks (%lld new %lld dedup), "
             "%lld bytes raw -> %lld bytes stored",
             snap, st.files_changed,
             st.chunks_total, st.chunks_new, st.chunks_dedup,
             st.bytes_new, st.bytes_stored);
    return 0;
}
