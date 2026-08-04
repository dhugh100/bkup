#ifndef BK_CATALOG_PUSH_H
#define BK_CATALOG_PUSH_H

#include <time.h>

/* Batches catalog uploads produced by continuous backups. A continuous backup commits its
   snapshot to the LOCAL catalog durably, then marks the user's catalog dirty
   here instead of uploading. The daemon pusher (driven by the scheduler tick)
   uploads at most once per CP_MIN_INTERVAL per user, serialized against
   backup/prune via the global op lock. The offsite catalog is only a recovery
   replica -- restore reads the local catalog -- so deferring the push only
   widens the window in which a fetch-catalog rebuild would miss the most
   recent continuous snapshots; the blobs themselves are already uploaded. */

/* Spot backup: record that `user`'s local catalog has un-pushed changes. */
void catalog_mark_dirty(const char *user);

/* Pusher: returns 1 and sets *seq_out if `user` owes a push and the throttle
   interval has elapsed; else 0. The captured seq is what mark_pushed records,
   so a continuous backup that arrives during the upload is not lost. */
int catalog_take_pending(const char *user, unsigned long *seq_out);

/* Pusher: record that the catalog as of `seq` is durably on the server.
   Advances pushed_seq (never past `seq`) and stamps the throttle clock. */
void catalog_mark_pushed(const char *user, unsigned long seq);

/* A full backup or prune just uploaded the whole catalog -> clear any pending
   debt for `user` and stamp the throttle clock. */
void catalog_clear(const char *user);

/* Test seam: install a fake clock for deterministic throttle tests.
   Pass NULL to restore the real time() clock. */
void catalog_push_set_clock(time_t (*fn)(void));

#endif
