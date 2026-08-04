#include <fts.h>
#include <string.h>
#include <errno.h>

#include "platform.h"
#include "common/types.h"
#include "common/log.h"

int platform_walk(const char *root, pwalk_cb cb, void *user)
{
    char *paths[] = { (char *)root, NULL };
    /* FTS_PHYSICAL: don't follow symlinks; statp is an lstat. */
    FTS *f = fts_open(paths, FTS_PHYSICAL | FTS_NOCHDIR, NULL);
    if (!f) return -1;

    int errors = 0;
    int root_failed = 0;            /* the source root itself was inaccessible */
    FTSENT *e;
    errno = 0;                      /* distinguish fts_read end from error */
    while ((e = fts_read(f)) != NULL) {
        int kind;
        switch (e->fts_info) {
        case FTS_D:                 /* directory, pre-order */
            kind = FK_DIR;
            if (cb(e->fts_path, e->fts_statp, kind, user) == PWALK_SKIP)
                fts_set(f, e, FTS_SKIP);
            break;
        case FTS_F:                 /* regular file */
            cb(e->fts_path, e->fts_statp, FK_REG, user);
            break;
        case FTS_SL:                /* symlink */
        case FTS_SLNONE:            /* symlink to nonexistent target */
            cb(e->fts_path, e->fts_statp, FK_SYMLINK, user);
            break;
        case FTS_DP:                /* directory, post-order: ignore */
        case FTS_DOT:
        case FTS_DEFAULT:           /* device/socket/fifo: not backed up */
            break;
        case FTS_DNR:               /* unreadable dir: its contents are unseen */
            log_warn("cannot read directory: %s (%s)",
                     e->fts_path, strerror(e->fts_errno));
            errors++;
            if (e->fts_level == FTS_ROOTLEVEL) root_failed = 1;
            break;
        case FTS_ERR:
        case FTS_NS:                /* stat failed: entry's metadata is unknown */
            log_warn("stat failed: %s (%s)",
                     e->fts_path, strerror(e->fts_errno));
            errors++;
            if (e->fts_level == FTS_ROOTLEVEL) root_failed = 1;
            break;
        default:
            break;
        }
        errno = 0;
    }
    /* fts_read returns NULL both at end-of-walk and on error; errno set here
       means the traversal itself failed partway (the listing is truncated). */
    if (errno != 0) {
        log_warn("walk of %s ended early: %s", root, strerror(errno));
        errors++;
    }
    fts_close(f);
    /* A failed root means the whole source is inaccessible (-1, the loudest
       case); a failure deeper in means a partial listing (positive count). */
    return root_failed ? -1 : errors;
}
