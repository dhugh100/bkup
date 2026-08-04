/*
 * test_watch_path.c -- unit tests for watch_effective_path() (Phase 3).
 *
 * watch_effective_path is a pure function extracted into coalesce.c so it can
 * be tested without pulling in fanotify or the full watcher machinery.
 *
 * Spec (from src/daemon/coalesce.h):
 *   Returns 1 and writes the queued path to out[0..cap-1] (NUL-terminated).
 *   Returns 0 (skip this event) when:
 *     - removed: no slash in path, or slash is path[0] (no valid parent), or
 *       the parent path would not fit in cap.
 *     - not removed: path would not fit in cap.
 *
 * Object set: DAEMON_OBJS (detected from "daemon/ include below).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include "daemon/coalesce.h"

#include "test_common.h"

static int fails;

/* ---- 1. Non-removal: path copied unchanged to out ---- */

static void test_non_removal_path_unchanged(void)
{
    char out[PATH_MAX];
    const char *path = "/src/dir/file.txt";
    int ret = watch_effective_path(path, 0, out, sizeof out);
    CHECKEQ_INT(ret, 1);
    CHECKEQ_STR(out, path);
}

/* ---- 2. Removal: parent directory extracted ---- */

static void test_removal_yields_parent(void)
{
    char out[PATH_MAX];
    int ret = watch_effective_path("/src/dir/deleted.txt", 1, out, sizeof out);
    CHECKEQ_INT(ret, 1);
    CHECKEQ_STR(out, "/src/dir");
}

/*
 * Deeper path: parent extraction must stop at the last slash, not earlier.
 */
static void test_removal_deep_path_yields_parent(void)
{
    char out[PATH_MAX];
    int ret = watch_effective_path("/a/b/c/d.txt", 1, out, sizeof out);
    CHECKEQ_INT(ret, 1);
    CHECKEQ_STR(out, "/a/b/c");
}

/* ---- 3. Removal of top-level "/x": no valid parent -> skip ---- */

/*
 * strrchr("/x", '/') finds the '/' at position 0 (slash == path), so there is
 * no valid parent directory within a source tree.  watch_effective_path must
 * return 0.
 */
static void test_removal_top_level_skip(void)
{
    char out[PATH_MAX];
    int ret = watch_effective_path("/x", 1, out, sizeof out);
    CHECKEQ_INT(ret, 0);
}

/* Path with no slash at all (pathological case): no parent -> skip. */
static void test_removal_no_slash_skip(void)
{
    char out[PATH_MAX];
    int ret = watch_effective_path("nosuchpath", 1, out, sizeof out);
    CHECKEQ_INT(ret, 0);
}

/* ---- 4. Buffer too small -> skip ---- */

/* Non-removal: path "/src/file" does not fit in a 4-byte buffer. */
static void test_non_removal_buf_too_small(void)
{
    char out[4];
    int ret = watch_effective_path("/src/file.txt", 0, out, sizeof out);
    CHECKEQ_INT(ret, 0);
}

/* Removal: parent "/long/path" does not fit in a 5-byte buffer. */
static void test_removal_parent_buf_too_small(void)
{
    char out[5];
    /* parent of "/long/path/file" is "/long/path" (10 chars + NUL = 11) */
    int ret = watch_effective_path("/long/path/file", 1, out, sizeof out);
    CHECKEQ_INT(ret, 0);
}

/* Exact fit: buffer is exactly right for the non-removal path. */
static void test_non_removal_exact_fit(void)
{
    const char *path = "/ab";
    char out[4];   /* strlen("/ab")+1 = 4 */
    int ret = watch_effective_path(path, 0, out, sizeof out);
    CHECKEQ_INT(ret, 1);
    CHECKEQ_STR(out, path);
}

/* One byte too small for non-removal path. */
static void test_non_removal_one_short(void)
{
    char out[3];   /* one short of strlen("/ab")+1 */
    int ret = watch_effective_path("/ab", 0, out, sizeof out);
    CHECKEQ_INT(ret, 0);
}

/* ---- main ---- */

int main(void)
{
    test_non_removal_path_unchanged();
    test_removal_yields_parent();
    test_removal_deep_path_yields_parent();
    test_removal_top_level_skip();
    test_removal_no_slash_skip();
    test_non_removal_buf_too_small();
    test_removal_parent_buf_too_small();
    test_non_removal_exact_fit();
    test_non_removal_one_short();
    TEST_DONE("test_watch_path");
}
