/*
 * test_common.h -- shared test infrastructure for Bkup unit tests.
 *
 * Header-only (static / static inline functions and macros). No .c companion.
 *
 * Each test file must declare   static int fails;   before using the CHECK
 * macros. The macros expand at the call site and reference that variable.
 *
 * Catalog fixture convention: open_temp_catalog(dir) creates the catalog at
 * <dir>/catalog.db. Callers that need the path construct it with:
 *   snprintf(catpath, sizeof catpath, "%s/catalog.db", dir);
 */

#ifndef TEST_COMMON_H
#define TEST_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <dirent.h>

#include <sqlite3.h>
#include "common/db.h"
#include "common/crypto.h"
#include "common/types.h"
#include "common/util.h"

/* ---- CHECK macros ------------------------------------------------------ */

/* Each test file must have:   static int fails;  */

#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    fails++; \
} } while (0)

#define CHECKEQ_INT(a, b) do { \
    long long _a = (long long)(a), _b = (long long)(b); \
    if (_a != _b) { \
        printf("FAIL %s:%d: %s=%lld expected %s=%lld\n", \
               __FILE__, __LINE__, #a, _a, #b, _b); \
        fails++; \
    } } while (0)

#define CHECKEQ_STR(a, b) do { \
    const char *_sa = (a), *_sb = (b); \
    if (!_sa || !_sb || strcmp(_sa, _sb) != 0) { \
        printf("FAIL %s:%d: %s=[%s] expected %s=[%s]\n", \
               __FILE__, __LINE__, \
               #a, _sa ? _sa : "(null)", #b, _sb ? _sb : "(null)"); \
        fails++; \
    } } while (0)

#define CHECK_MEMEQ(a, b, n) do { \
    if (memcmp((a), (b), (n)) != 0) { \
        printf("FAIL %s:%d: memcmp(%s, %s, %zu) != 0\n", \
               __FILE__, __LINE__, #a, #b, (size_t)(n)); \
        fails++; \
    } } while (0)

/* Print "name: OK" and return.  Must be the last statement in main(). */
#define TEST_DONE(name) do { \
    if (fails == 0) printf("%s: OK\n", (name)); \
    return fails ? 1 : 0; \
} while (0)

/* ---- Deterministic fill (splitmix64) ----------------------------------- */

static __attribute__((unused))
void fill(uint8_t *b, size_t n, uint64_t seed)
{
    uint64_t s = seed;
    for (size_t i = 0; i < n; i++) {
        s += 0x9E3779B97F4A7C15ULL;
        uint64_t z = s;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        b[i] = (uint8_t)(z >> 31);
    }
}

/* ---- tmpdir / rmtree --------------------------------------------------- */

/*
 * Create a temp directory and write its path into out[sz].
 * Dies (perror + exit) if mkdtemp fails.
 */
static __attribute__((unused))
void tmpdir(char *out, size_t sz)
{
    /* Honor $TMPDIR so the test runner can point each test at a private scratch
       dir and wipe it after that test (per-iteration cleanup). Falls back to
       /tmp when unset. */
    const char *base = getenv("TMPDIR");
    if (!base || !base[0]) base = "/tmp";
    size_t bl = strlen(base);
    while (bl > 1 && base[bl - 1] == '/') bl--;   /* trim trailing slashes */
    snprintf(out, sz, "%.*s/bkup_test_XXXXXX", (int)bl, base);
    if (!mkdtemp(out)) { perror("mkdtemp"); exit(2); }
}

/* Best-effort recursive removal of a local directory tree. */
static __attribute__((unused))
void rmtree_local(const char *path)
{
    DIR *d = opendir(path);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                continue;
            char child[4096];
            snprintf(child, sizeof child, "%s/%s", path, e->d_name);
            struct stat st;
            if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode))
                rmtree_local(child);
            else
                unlink(child);
        }
        closedir(d);
        rmdir(path);
    } else {
        unlink(path);
    }
}

/* ---- Catalog fixture helpers ------------------------------------------- */

/*
 * Open (or create) a full-schema catalog at <dir>/catalog.db using db_open().
 * See db.h for schema details.  The resulting handle is owned by the caller.
 */
static __attribute__((unused))
sqlite3 *open_temp_catalog(const char *dir)
{
    char path[4096];
    snprintf(path, sizeof path, "%s/catalog.db", dir);
    return db_open(path);
}

/* INSERT INTO snapshots; return rowid. */
static __attribute__((unused))
long long ins_snapshot(sqlite3 *db, long long created, int state, int kind)
{
    sqlite3_stmt *st;
    sqlite3_prepare_v2(db,
        "INSERT INTO snapshots(created,state,kind) VALUES(?,?,?)",
        -1, &st, NULL);
    sqlite3_bind_int64(st, 1, created);
    sqlite3_bind_int  (st, 2, state);
    sqlite3_bind_int  (st, 3, kind);
    if (sqlite3_step(st) != SQLITE_DONE)
        printf("ins_snapshot failed: %s\n", sqlite3_errmsg(db));
    sqlite3_finalize(st);
    return (long long)sqlite3_last_insert_rowid(db);
}

/*
 * INSERT INTO files; mtime maps to mtime_sec (mtime_nsec=0).
 * Returns rowid.
 */
static __attribute__((unused))
long long ins_file(sqlite3 *db, const char *path, int kind,
                   long long size, long long mtime, long long ino,
                   long long dev, long long mode, long long uid,
                   long long gid, int state, long long seen)
{
    sqlite3_stmt *st;
    sqlite3_prepare_v2(db,
        "INSERT INTO files(path,kind,size,mtime_sec,mtime_nsec,ino,dev,mode,"
        "uid,gid,state,seen) VALUES(?,?,?,?,0,?,?,?,?,?,?,?)",
        -1, &st, NULL);
    sqlite3_bind_text (st,  1, path, -1, SQLITE_STATIC);
    sqlite3_bind_int  (st,  2, kind);
    sqlite3_bind_int64(st,  3, size);
    sqlite3_bind_int64(st,  4, mtime);
    sqlite3_bind_int64(st,  5, ino);
    sqlite3_bind_int64(st,  6, dev);
    sqlite3_bind_int64(st,  7, mode);
    sqlite3_bind_int64(st,  8, uid);
    sqlite3_bind_int64(st,  9, gid);
    sqlite3_bind_int  (st, 10, state);
    sqlite3_bind_int64(st, 11, seen);
    if (sqlite3_step(st) != SQLITE_DONE)
        printf("ins_file failed: %s\n", sqlite3_errmsg(db));
    sqlite3_finalize(st);
    return (long long)sqlite3_last_insert_rowid(db);
}

/*
 * INSERT INTO versions; last_snap < 0 means last_snapshot IS NULL (current).
 * Other fields (mtime, mode, uid, gid, link_target) are set to 0/NULL.
 * Returns rowid.
 */
static __attribute__((unused))
long long ins_version(sqlite3 *db, const char *path, int kind,
                      long long size, long long first_snap,
                      long long last_snap)
{
    sqlite3_stmt *st;
    sqlite3_prepare_v2(db,
        "INSERT INTO versions(path,kind,size,first_snapshot,last_snapshot)"
        " VALUES(?,?,?,?,?)",
        -1, &st, NULL);
    sqlite3_bind_text (st, 1, path, -1, SQLITE_STATIC);
    sqlite3_bind_int  (st, 2, kind);
    sqlite3_bind_int64(st, 3, size);
    sqlite3_bind_int64(st, 4, first_snap);
    if (last_snap < 0)
        sqlite3_bind_null(st, 5);
    else
        sqlite3_bind_int64(st, 5, last_snap);
    if (sqlite3_step(st) != SQLITE_DONE)
        printf("ins_version failed: %s\n", sqlite3_errmsg(db));
    sqlite3_finalize(st);
    return (long long)sqlite3_last_insert_rowid(db);
}

/* INSERT INTO version_blobs. hash must be BK_HASH_LEN bytes. */
static __attribute__((unused))
void ins_version_blob(sqlite3 *db, long long version_id, int seq,
                      const uint8_t *hash)
{
    sqlite3_stmt *st;
    sqlite3_prepare_v2(db,
        "INSERT INTO version_blobs(version_id,seq,hash) VALUES(?,?,?)",
        -1, &st, NULL);
    sqlite3_bind_int64(st, 1, version_id);
    sqlite3_bind_int  (st, 2, seq);
    sqlite3_bind_blob (st, 3, hash, BK_HASH_LEN, SQLITE_STATIC);
    if (sqlite3_step(st) != SQLITE_DONE)
        printf("ins_version_blob failed: %s\n", sqlite3_errmsg(db));
    sqlite3_finalize(st);
}

/* INSERT INTO blobs. hash must be BK_HASH_LEN bytes. */
static __attribute__((unused))
void ins_blob(sqlite3 *db, const uint8_t *hash, long long size,
              long long stored_size, int state)
{
    sqlite3_stmt *st;
    sqlite3_prepare_v2(db,
        "INSERT INTO blobs(hash,size,stored_size,state) VALUES(?,?,?,?)",
        -1, &st, NULL);
    sqlite3_bind_blob (st, 1, hash, BK_HASH_LEN, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, size);
    sqlite3_bind_int64(st, 3, stored_size);
    sqlite3_bind_int  (st, 4, state);
    if (sqlite3_step(st) != SQLITE_DONE)
        printf("ins_blob failed: %s\n", sqlite3_errmsg(db));
    sqlite3_finalize(st);
}

/* ---- Fixed test Key and low-cost KdfParams ----------------------------- */

/*
 * Fixed test key: memset 0x5a pattern.
 * Avoids running Argon2 in tests that only need a key, not KDF testing.
 * Initialise with: Key key; memset(&key, 0x5a, sizeof key);
 */
#define TC_KEY_BYTE 0x5a

/*
 * Low-cost KDF parameters for fast Argon2id in crypto tests.
 * Uses library minimums: ops=1, mem=8192 bytes.
 * Much faster than MODERATE (the default), at the cost of no security.
 * DO NOT use these values in production.
 *
 * crypto_pwhash_OPSLIMIT_MIN = 1
 * crypto_pwhash_MEMLIMIT_MIN = 8192
 */
#define TC_KDF_OPS 1
#define TC_KDF_MEM 8192

static __attribute__((unused))
KdfParams tc_kdf_params(void)
{
    KdfParams p;
    memset(p.salt, 0x42, BK_SALTBYTES);
    p.ops = TC_KDF_OPS;
    p.mem = TC_KDF_MEM;
    return p;
}

/* ---- check_dies -------------------------------------------------------- */

/*
 * Fork a child that runs fn().  fn() is expected to call die() (which calls
 * exit(1)).  Returns 1 if the child exited with a nonzero status (died as
 * expected), 0 otherwise.  Suppresses the child's stderr so die() messages
 * do not pollute test output.
 *
 * Use this to assert "must die" properties without killing the harness.
 */
static __attribute__((unused))
int check_dies(void (*fn)(void))
{
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) return 0;
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }
        fn();
        _exit(0);  /* fn() returned without dying: signal failure */
    }
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) && WEXITSTATUS(st) != 0;
}

#endif /* TEST_COMMON_H */
