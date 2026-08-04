#!/usr/bin/env bash
# Build the bkup RPM (bkup + bkupd + bkup-gui -> /usr/local/bin, plus the bkupd
# SELinux policy module with automatic relabel in %post). Self-contained: stages
# a source tarball and builds under a temp _topdir so ~/rpmbuild is untouched.
#
# Usage:  packaging/build-rpm.sh
# Output: packaging/rpms/bkup-<ver>-<rel>.<arch>.rpm
set -euo pipefail

NAME=bkup
HERE=$(cd "$(dirname "$0")/.." && pwd)   # repo root
SPEC="$HERE/packaging/bkup.spec"

# The spec is the single source of truth for the version.
VERSION=$(grep '^Version:' "$SPEC" | awk '{print $2}')
[ -n "$VERSION" ] || { echo "build-rpm: could not read Version from $SPEC" >&2; exit 1; }

TOP=$(mktemp -d)
trap 'rm -rf "$TOP"' EXIT
mkdir -p "$TOP"/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS}

# Stage exactly the sources rpmbuild's %build needs, into a versioned dir.
STAGE="$TOP/$NAME-$VERSION"
mkdir -p "$STAGE"
cp -a "$HERE/Makefile" "$HERE/LICENSE" "$HERE/src" "$HERE/selinux" "$HERE/doSetup.sh" "$STAGE/"
# Drop any stale build artifacts so %build compiles clean.
rm -rf "$STAGE/bin" "$STAGE/obj" "$STAGE/selinux/tmp" "$STAGE"/selinux/*.pp

tar -C "$TOP" -czf "$TOP/SOURCES/$NAME-$VERSION.tar.gz" "$NAME-$VERSION"
# Auxiliary Source1..N files referenced by the spec live alongside it.
cp "$HERE/packaging/60-bkupd-fanotify.conf" "$TOP/SOURCES/"
cp "$HERE/bkupd.service" "$TOP/SOURCES/"
cp "$SPEC" "$TOP/SPECS/"

rpmbuild --define "_topdir $TOP" -bb "$TOP/SPECS/bkup.spec"

mkdir -p "$HERE/packaging/rpms"
find "$TOP/RPMS" -name '*.rpm' -exec cp -v {} "$HERE/packaging/rpms/" \;
echo
echo "Done. RPM(s) in $HERE/packaging/rpms/"
echo "Install with:  sudo dnf install ./packaging/rpms/$NAME-$VERSION-*.rpm"
