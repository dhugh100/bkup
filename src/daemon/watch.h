#ifndef BK_WATCH_H
#define BK_WATCH_H

/* Start the fanotify watcher thread. It marks every configured source's
   filesystem, coalesces change events into per-user subtree roots, and fires
   debounced continuous backups (serialized with scheduled ops via the op lock).
   If fanotify is unavailable (not root, or an old kernel/glibc), it logs a
   warning and disables itself; the daemon keeps running without continuous backups. */
void watch_start(const char *config_path);

#endif
