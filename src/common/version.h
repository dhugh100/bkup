#ifndef BK_VERSION_H
#define BK_VERSION_H

/* Kept in lockstep with `Version:` in packaging/bkup.spec by
   packaging/doRelease.sh, which bumps both in the same commit. The spec is
   still the source of truth for the release number; this exists so the built
   binary can report it (`bkup --version`) without the spec, which the CI
   source tarball does not ship. */
#define BKUP_VERSION "0.11.1"

#endif
