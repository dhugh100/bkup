#ifndef BK_TYPES_H
#define BK_TYPES_H

#include <stdint.h>
#include <stddef.h>

#define BK_HASH_LEN     32              /* BLAKE2b-256 */
#define BK_HEX_LEN      (BK_HASH_LEN * 2)

/* FastCDC chunk size bounds (bytes). */
#define BK_CHUNK_MIN    (256u * 1024u)
#define BK_CHUNK_NORMAL (1024u * 1024u)
#define BK_CHUNK_MAX    (4u * 1024u * 1024u)

/* files.kind / versions.kind */
enum { FK_REG = 0, FK_DIR = 1, FK_SYMLINK = 2 };

/* files.state */
enum { FS_CLEAN = 0, FS_DIRTY = 1 };

/* blobs.state */
enum { BS_NEEDED = 0, BS_UPLOADED = 1 };

/* snapshots.state */
enum { SS_OPEN = 0, SS_COMPLETE = 1 };

/* snapshots.kind -- how the snapshot was produced. Distinct from files.kind /
   versions.kind (which describe file type). A scheduled snapshot is a full
   image from `bkup backup`; a continuous one is a watcher/`bkup continuous`
   subtree refresh that carries every other version forward. */
enum { SK_SCHEDULED = 0, SK_CONTINUOUS = 1 };

#endif
