#ifndef BK_COALESCE_H
#define BK_COALESCE_H

#include <limits.h>
#include <stddef.h>

/* Coalescing logic for the fanotify watcher: many file events under a source
   are folded into a single subtree root R, which one continuous backup then refreshes.
   This is pure (no I/O), so it is unit-tested directly. */

/* Longest common directory ancestor of two absolute paths, written to out.
   Cuts only at '/' boundaries, so /a/p and /a/python yield /a, not /a/p.
   If one path is a directory-prefix of the other (/a/b and /a/b/c) the shorter
   is returned. Disjoint paths yield "/". */
void path_common_ancestor(const char *a, const char *b, char *out, size_t cap);

typedef struct {
    char root[PATH_MAX];   /* current coalesced root; empty when !active */
    int  active;
} Coalescer;

void coalesce_reset(Coalescer *cz);

/* Fold `path` into the coalesced root, never letting it rise above `source`
   (the configured source subtree that contains `path`). Returns 1 if `path`
   belongs to a different source than the pending root -- the caller should
   flush the current root first, then fold `path` into a fresh coalescer.
   Returns 0 on a normal fold. */
int coalesce_fold(Coalescer *cz, const char *path, const char *source);

/* If a root is pending, copy it to out and reset; returns 1. Else returns 0. */
int coalesce_take(Coalescer *cz, char *out, size_t cap);

/* Derive the effective filesystem path to queue for a watcher event.
   For a removal (removed != 0), the deleted object is already gone and cannot
   be walked; queue its PARENT directory instead so scan_run_subtree can walk
   it and record the deletion via the scoped vanished-file sweep.
   Returns 1 and writes the queued path to out[0..cap-1] (NUL-terminated).
   Returns 0 (skip this event) when:
     - removed: no slash in path, or slash is path[0] (no valid parent), or
       the parent path would not fit in cap.
     - not removed: path would not fit in cap. */
int watch_effective_path(const char *path, int removed, char *out, size_t cap);

#endif
