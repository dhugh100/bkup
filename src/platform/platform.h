#ifndef BK_PLATFORM_H
#define BK_PLATFORM_H

#include <sys/stat.h>

/* Tree-walk abstraction. The Windows port replaces platform_walk with a
   FindFirstFile-based implementation; everything above this line stays the
   same. The callback receives a physical (lstat) view -- symlinks are not
   followed. Return PWALK_SKIP from a directory entry to prune its subtree. */

enum { PWALK_OK = 0, PWALK_SKIP = 1 };

/* kind is one of FK_REG / FK_DIR / FK_SYMLINK (types.h). Entries that are
   none of those (devices, sockets, fifos) are not reported. */
typedef int (*pwalk_cb)(const char *path, const struct stat *st,
                        int kind, void *user);

/* Walk `root`, invoking `cb` per entry. Per-entry errors (unreadable dirs,
   failed stats) are logged and skipped rather than aborting the walk, but they
   make the walk INCOMPLETE -- which matters to the caller, because treating an
   incomplete listing as authoritative would drop catalog entries for files that
   still exist. Returns 0 for a fully complete walk, -1 if the root could not be
   opened at all, or a positive count of per-entry errors otherwise. A nonzero
   return means "do not treat this listing as the full contents of root". */
int platform_walk(const char *root, pwalk_cb cb, void *user);

#endif
