# Bkup tests

One command runs the whole suite and prints a single numbered list of tests,
grouped by area, each marked PASS / FAIL / SKIP:

```sh
make test
```

It builds first (`make all`), then `tests/run_tests.sh` compiles every
`tests/test_*.c` against the built `obj/*.o`, runs it, and wipes that test's
scratch space before the next one. Exit status is nonzero if any test failed
(skips do not fail the run).

## Tiers

- **Library and codec unit tests** -- util, compress, crypto, chunk, catman,
  catalog_chunk. Pure, in-memory, no external state.
- **Config parsing** -- the config file reader and exclude semantics.
- **Catalog and backup logic** -- scan/change-detection, scoped deletion sweep,
  the version-interval model, prune retention + blob GC. Run against a real
  catalog schema (`db_open`) in a temp dir; no server.
- **Daemon logic** -- coalescing, catalog-push throttle, the watcher
  delete-event parent-fold helper.
- **Offline round-trip (fake transport)** -- `test_roundtrip` drives the real
  backup/restore/dedup/versioning/fetch-catalog code through the TR_LOCAL
  filesystem transport (no SFTP). This is the closest offline analogue to a real
  backup. `test_prune_gc` drives `cmd_prune` over the same transport and checks
  both halves of its "remove what is now empty" pass: directory versions with
  nothing beneath them leave the catalog, and emptied `blobs/<2 hex>` fan-out
  directories are rmdir'd on the server.

## Sanitizers

```sh
BKUP_TEST_SAN=1 make test
```

Adds `-fsanitize=address,undefined` (AddressSanitizer + UBSan, with
LeakSanitizer). This is the primary guard against memory/leak/UB regressions.
On a system without the sanitizer runtime it prints a warning and falls back to
a normal run (Fedora/RHEL: `sudo dnf install libasan libubsan`).

## Notes

- `test_chunk` contains a GOLDEN guard on the FastCDC chunk boundaries. A failure
  there almost always means the frozen `gear[]`/seed/masks changed, which breaks
  dedup against every existing repo -- it is rarely a "fix the test" situation.
- Catalog fixtures use the real schema via `db_open`; do not hand-roll
  `CREATE TABLE` in tests (it drifts from the live schema).
- `die()` calls `exit()`; wrap "must die" assertions in `check_dies()`
  (`tests/test_common.h`).
