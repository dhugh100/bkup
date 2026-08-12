/* Unit test for the watcher's coalescing logic (src/daemon/coalesce.c). */
#include <stdio.h>
#include <string.h>
#include "daemon/coalesce.h"
#include "test_common.h"

static int fails;

static int ca_is(const char *a, const char *b, const char *want)
{
    char out[PATH_MAX];
    path_common_ancestor(a, b, out, sizeof out);
    if (strcmp(out, want) != 0) {
        printf("FAIL ca(%s,%s) = %s, want %s\n", a, b, out, want);
        return 0;
    }
    return 1;
}

int main(void)
{
    /* common ancestor */
    CHECK(ca_is("/a/b/c.txt", "/a/b/d.txt", "/a/b"));
    CHECK(ca_is("/a/b/x",     "/a/c/y",     "/a"));
    CHECK(ca_is("/a/b",       "/a/b/c",     "/a/b"));   /* prefix is ancestor */
    CHECK(ca_is("/a/b/c",     "/a/b",       "/a/b"));   /* order independent */
    CHECK(ca_is("/a/proj",    "/a/project", "/a"));     /* prefix trap: NOT /a/proj */
    CHECK(ca_is("/x",         "/y",         "/"));      /* disjoint -> root */
    CHECK(ca_is("/a/b",       "/a/b",       "/a/b"));   /* equal */

    /* coalescing folds within one source, widening to the common ancestor */
    Coalescer cz;
    coalesce_reset(&cz);
    char root[PATH_MAX];

    CHECK(coalesce_fold(&cz, "/src/p/a.txt", "/src") == 0);
    CHECK(coalesce_fold(&cz, "/src/p/b.txt", "/src") == 0);
    CHECK(strcmp(cz.root, "/src/p") == 0);
    CHECK(coalesce_fold(&cz, "/src/q/c.txt", "/src") == 0);
    CHECK(strcmp(cz.root, "/src") == 0);                /* widened to common ancestor */

    /* a move's two endpoints both fold in -> widens R to cover both (no flush
       while within the same source) */
    CHECK(coalesce_fold(&cz, "/src/q/moved_here", "/src") == 0);
    CHECK(strcmp(cz.root, "/src") == 0);

    /* a change in a different source recommends a flush first */
    CHECK(coalesce_fold(&cz, "/other/z", "/other") == 1);
    CHECK(strcmp(cz.root, "/src") == 0);                /* root unchanged on flush signal */

    /* caller flushes, then folds the new-source path into a fresh root */
    CHECK(coalesce_take(&cz, root, sizeof root) == 1);
    CHECK(strcmp(root, "/src") == 0);
    CHECK(coalesce_take(&cz, root, sizeof root) == 0);  /* empty after take */
    CHECK(coalesce_fold(&cz, "/other/z", "/other") == 0);
    CHECK(strcmp(cz.root, "/other/z") == 0);

    TEST_DONE("test_coalesce");
}
