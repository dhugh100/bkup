#include <string.h>
#include <stdio.h>

#include "coalesce.h"

void path_common_ancestor(const char *a, const char *b, char *out, size_t cap)
{
    size_t i = 0, lastsep = 0;
    while (a[i] && a[i] == b[i]) {
        if (a[i] == '/') lastsep = i;
        i++;
    }
    /* a is a directory-prefix of b (or they are equal): a is the ancestor. */
    if (a[i] == '\0' && (b[i] == '/' || b[i] == '\0')) {
        snprintf(out, cap, "%s", a);
        return;
    }
    /* b is a directory-prefix of a: b is the ancestor. */
    if (b[i] == '\0' && a[i] == '/') {
        snprintf(out, cap, "%s", b);
        return;
    }
    /* mismatch within a path segment: cut back to the last separator. */
    if (lastsep == 0) { snprintf(out, cap, "/"); return; }
    snprintf(out, cap, "%.*s", (int)lastsep, a);
}

void coalesce_reset(Coalescer *cz)
{
    cz->root[0] = '\0';
    cz->active = 0;
}

int coalesce_fold(Coalescer *cz, const char *path, const char *source)
{
    if (!cz->active) {
        snprintf(cz->root, sizeof cz->root, "%s", path);
        cz->active = 1;
        return 0;
    }

    char ca[PATH_MAX];
    path_common_ancestor(cz->root, path, ca, sizeof ca);

    /* If the common ancestor is not within `source`, the pending root and
       `path` belong to different sources: recommend a flush. */
    size_t slen = strlen(source);
    if (strncmp(ca, source, slen) != 0 || (ca[slen] != '\0' && ca[slen] != '/'))
        return 1;

    snprintf(cz->root, sizeof cz->root, "%s", ca);
    return 0;
}

int coalesce_take(Coalescer *cz, char *out, size_t cap)
{
    if (!cz->active) return 0;
    snprintf(out, cap, "%s", cz->root);
    coalesce_reset(cz);
    return 1;
}

int watch_effective_path(const char *path, int removed, char *out, size_t cap)
{
    if (!removed) {
        size_t n = strlen(path);
        if (n + 1 > cap) return 0;
        memcpy(out, path, n + 1);
        return 1;
    }
    /* Removal: queue the parent directory.  The deleted object is already gone,
       so scan_run_subtree must walk the parent to discover the deletion via the
       scoped vanished-file sweep.  Folding the child path itself would make the
       subtree walk fail (could-not-open), skipping stale-entry cleanup. */
    const char *slash = strrchr(path, '/');
    if (!slash || slash == path) return 0;   /* no valid parent within a source */
    size_t plen = (size_t)(slash - path);
    if (plen >= cap) return 0;
    memcpy(out, path, plen);
    out[plen] = '\0';
    return 1;
}
