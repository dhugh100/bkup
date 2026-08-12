#!/bin/bash
# run_tests.sh -- discover, build, and run the Bkup test suite as a single
# command, printing a numbered list of tests grouped by area with PASS/FAIL,
# and cleaning up each test's scratch space after it runs.
#
# Always runs `make all` first, then the suite. A single positional argument
# selects the integration tier: 0 (default) = offline tiers only, 1 = also run
# the end-to-end integration tier (needs an SFTP server; see integration.sh).
#
# Usage:
#   tests/run_tests.sh                     # build + unit/logic tiers (offline)
#   tests/run_tests.sh 0                   # same as above (explicit)
#   tests/run_tests.sh 1                   # build + offline tiers + integration
#   BKUP_TEST_SAN=1 tests/run_tests.sh     # add -fsanitize=address,undefined
#
# Object sets (determined by which headers each test includes):
#   common  -- obj/common/*.o  obj/platform/*.o
#   cli     -- common + obj/cli/*.o (excluding main.o)
#   daemon  -- cli   + obj/daemon/*.o (excluding main.o)
#
# Exit status is nonzero if any test failed (skips do not fail the run).

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

# Single positional arg: 0 = no integration (default), 1 = run integration.
case "${1:-0}" in
    0) BKUP_TEST_INTEGRATION=0 ;;
    1) BKUP_TEST_INTEGRATION=1 ;;
    *) echo "usage: $0 [0|1]   (0 = no integration, 1 = run integration)" >&2
       exit 2 ;;
esac

# Always build first -- cheap, and keeps obj/ in sync with the tests. Build only
# the CLI + daemon (whose objects are what the tests link); skip bin/bkup-gui so
# GTK4 is not required to run the suite (e.g. in CI containers without GTK4).
echo "[Build] make bin/bkup bin/bkupd"
make bin/bkup bin/bkupd || { echo "make failed" >&2; exit 1; }

CC=gcc
CFLAGS="-std=c11 -D_GNU_SOURCE -Wall -Wextra -Isrc"
CFLAGS="$CFLAGS $(pkg-config --cflags sqlite3 libzstd libsodium libssh2)"
LIBS="$(pkg-config --libs sqlite3 libzstd libsodium libssh2)"

if [ "${BKUP_TEST_SAN:-0}" = "1" ]; then
    # Probe whether the sanitizer libraries are actually usable on this system.
    # On some Fedora/RHEL builds the GCC 'libasan.so' linker script references
    # a versioned DSO that is not installed (missing libasan package).
    san_probe="$(mktemp /tmp/bkup_san_probe_XXXXXX.c)"
    san_out="$(mktemp /tmp/bkup_san_probe_XXXXXX)"
    printf 'int main(void){return 0;}\n' >"$san_probe"
    if $CC -fsanitize=address,undefined -o "$san_out" "$san_probe" 2>/dev/null; then
        CFLAGS="$CFLAGS -fsanitize=address,undefined -fno-omit-frame-pointer -g"
        LIBS="$LIBS -fsanitize=address,undefined"
        echo "Sanitizer mode: ASan + UBSan enabled"
    else
        echo "WARNING: BKUP_TEST_SAN=1 requested but sanitizer libraries are not"
        echo "  available on this system (likely missing 'libasan' package)."
        echo "  Running tests WITHOUT sanitizers.  Install the package to enable:"
        echo "    sudo dnf install libasan libubsan   (Fedora/RHEL)"
        echo "    sudo apt install libasan8 libubsan1 (Debian/Ubuntu)"
    fi
    rm -f "$san_probe" "$san_out"
fi

# Object sets
COMMON_OBJS="$(ls obj/common/*.o) $(ls obj/platform/*.o)"
CLI_OBJS="$COMMON_OBJS $(ls obj/cli/*.o | grep -v '/main\.o$')"
DAEMON_OBJS="$CLI_OBJS $(ls obj/daemon/*.o | grep -v '/main\.o$')"

RUNDIR="$(mktemp -d /tmp/bkup_tests_XXXXXX)"
trap 'rm -rf "$RUNDIR"' EXIT

pass=0
fail=0
skip=0
num=0
failed_names=""

# Display group for a test name, and the order groups print in.
GROUP_ORDER=(
    "Library and codec unit tests"
    "Config parsing"
    "Catalog and backup logic"
    "Daemon logic"
    "Offline round-trip (fake transport)"
)

group_of() {
    case "$1" in
        test_util|test_compress|test_crypto|test_chunk|test_catman|test_catalog_chunk)
            echo "Library and codec unit tests" ;;
        test_config)
            echo "Config parsing" ;;
        test_scan|test_sweep|test_versions|test_prune)
            echo "Catalog and backup logic" ;;
        test_coalesce|test_catalog_push|test_watch_path)
            echo "Daemon logic" ;;
        test_roundtrip|test_backup_restore|test_continuous|test_fetch_catalog|test_prune_gc)
            echo "Offline round-trip (fake transport)" ;;
        *)
            echo "Other" ;;
    esac
}

# Build + run one unit test, report it on a numbered line, then wipe its scratch.
run_one() {
    local src="$1" name objs extra_libs out build_log run_log tdir
    name="$(basename "$src" .c)"
    num=$((num + 1))

    if [ "$name" = "test_opwait" ]; then
        # Special: it #includes src/daemon/ipc.c to reach that file's static op
        # mutex, so it needs the daemon set MINUS ipc.o -- linking ipc.o as well
        # is a duplicate-symbol error. Its relative-path include also misses the
        # '"daemon/' probe below, which is why this case comes first.
        objs="$(printf '%s\n' $DAEMON_OBJS | grep -v '/ipc\.o$' | tr '\n' ' ')"
        extra_libs="-lpthread"
    elif grep -q '"daemon/' "$src" 2>/dev/null; then
        objs="$DAEMON_OBJS"; extra_libs="-lpthread"
    elif grep -q '"cli/' "$src" 2>/dev/null; then
        objs="$CLI_OBJS"; extra_libs=""
    else
        objs="$COMMON_OBJS"; extra_libs=""
    fi

    out="$RUNDIR/$name"
    build_log="$RUNDIR/${name}.build.log"
    if ! $CC $CFLAGS "$src" $objs $LIBS $extra_libs -o "$out" >"$build_log" 2>&1; then
        printf "%3d. %-32s BUILD FAILED\n" "$num" "$name"
        sed 's/^/        /' "$build_log"
        failed_names="$failed_names $name(build)"
        fail=$((fail + 1))
        rm -f "$out" "$build_log"
        return
    fi

    # Per-iteration cleanup: each test gets a private TMPDIR that is removed
    # right after it runs, whether it passed, failed, or crashed.
    tdir="$RUNDIR/${name}.tmp"
    mkdir -p "$tdir"
    run_log="$RUNDIR/${name}.run.log"
    if TMPDIR="$tdir" "$out" >"$run_log" 2>&1; then
        printf "%3d. %-32s PASS\n" "$num" "$name"
        pass=$((pass + 1))
    else
        printf "%3d. %-32s FAIL\n" "$num" "$name"
        sed 's/^/        /' "$run_log"
        failed_names="$failed_names $name"
        fail=$((fail + 1))
    fi
    rm -rf "$tdir" "$out" "$build_log" "$run_log"
}

shopt -s nullglob
all_tests=(tests/test_*.c)
shopt -u nullglob

echo "Bkup test suite"
echo "==============="

for group in "${GROUP_ORDER[@]}"; do
    header_done=0
    for src in "${all_tests[@]}"; do
        name="$(basename "$src" .c)"
        [ "$(group_of "$name")" = "$group" ] || continue
        if [ $header_done -eq 0 ]; then echo; echo "[$group]"; header_done=1; fi
        run_one "$src"
    done
done

# Any test that did not match a known group still runs (under "Other").
other_done=0
for src in "${all_tests[@]}"; do
    name="$(basename "$src" .c)"
    [ "$(group_of "$name")" = "Other" ] || continue
    if [ $other_done -eq 0 ]; then echo; echo "[Other]"; other_done=1; fi
    run_one "$src"
done

# ---- Integration tier (needs an SFTP server; auto-skips otherwise) ----------
echo
echo "[Integration (end-to-end over SFTP)]"
if [ "${BKUP_TEST_INTEGRATION:-0}" = "1" ] && [ -x tests/integration.sh ]; then
    mkdir -p "$RUNDIR/integration.tmp"
    if TMPDIR="$RUNDIR/integration.tmp" tests/integration.sh; then
        num=$((num + 1))
        printf "%3d. %-32s PASS\n" "$num" "integration (end-to-end SFTP)"
        pass=$((pass + 1))
    else
        rc=$?
        num=$((num + 1))
        if [ $rc -eq 77 ]; then        # 77 = integration prerequisites missing
            printf "%3d. %-32s SKIP (server/loopback unavailable)\n" \
                   "$num" "integration (end-to-end SFTP)"
            skip=$((skip + 1))
        else
            printf "%3d. %-32s FAIL\n" "$num" "integration (end-to-end SFTP)"
            failed_names="$failed_names integration"
            fail=$((fail + 1))
        fi
    fi
    rm -rf "$RUNDIR/integration.tmp"
else
    num=$((num + 1))
    printf "%3d. %-32s SKIP (set BKUP_TEST_INTEGRATION=1)\n" \
           "$num" "integration (end-to-end SFTP)"
    skip=$((skip + 1))
fi

echo
echo "==============="
echo "Results: $pass passed, $fail failed, $skip skipped (of $num listed)"
if [ $fail -ne 0 ]; then
    echo "Failed:$failed_names"
    exit 1
fi
exit 0
