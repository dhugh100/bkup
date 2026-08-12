#ifndef BK_TRANSPORT_H
#define BK_TRANSPORT_H

#include <stdint.h>
#include <stddef.h>

#include "util.h"

typedef struct Transport Transport;

/* Connect, verify the host key against ~/.ssh/known_hosts, authenticate via
   ssh-agent or ~/.ssh key files, and open an SFTP channel. die()s on any
   fatal error (unknown host key, no usable auth). */
Transport *transport_connect(const char *host, int port, const char *user);
void       transport_disconnect(Transport *t);

/* Local-filesystem backend: every absolute server path is mapped under `root`
   (root acts as a chroot). No network, no SFTP. Intended for offline tests of
   the full backup/restore/continuous/fetch-catalog paths; assign the result to
   Ctx.t before the command runs so ctx_connect() leaves it in place. Freed by
   transport_disconnect like any Transport.

   TEST ONLY: declared, defined, and dispatched to only when the test runner
   defines BKUP_TEST_TRANSPORT, so it is absent from the shipped binaries. */
#ifdef BKUP_TEST_TRANSPORT
Transport *transport_local_new(const char *root);
#endif

/* 1 = exists, 0 = absent, -1 = error. */
int transport_exists(Transport *t, const char *path);

/* mkdir (existing dir is success). mkdir_p creates parents too. */
int transport_mkdir(Transport *t, const char *path);
int transport_mkdir_p(Transport *t, const char *path);

/* Delete a remote file. Returns 0 on success or if already absent, -1 on
   other errors. */
int transport_delete(Transport *t, const char *path);

/* Recursively delete a remote directory tree (its files and subdirs) and then
   the directory itself. Tolerates an already-absent path. Returns 0 on
   success, -1 if any entry could not be removed. */
int transport_rmtree(Transport *t, const char *path);

/* Delete a remote directory only if it is empty. Returns 0 on success or if
   already absent, -1 otherwise -- including the ordinary "not empty" case, so
   callers can attempt it speculatively and ignore the failure. */
int transport_rmdir(Transport *t, const char *path);

/* Atomic publish: write path.tmp then rename onto path. If overwrite is 0 and
   the target already exists, the tmp file is removed and success is returned
   (content-addressed blobs are identical by construction). */
int transport_put(Transport *t, const char *path,
                  const uint8_t *data, size_t len, int overwrite);
int transport_get(Transport *t, const char *path, Buf *out);

/* Same as transport_put/get but streaming to/from a local file (for the
   catalog snapshot, which is too large to hold in memory comfortably). */
int transport_put_file(Transport *t, const char *local, const char *remote,
                       int overwrite);
int transport_get_file(Transport *t, const char *remote, const char *local);

#endif
