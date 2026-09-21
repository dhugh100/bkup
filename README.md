# bkup

An encrypted, deduplicating backup system for Linux. Files are content-chunked,
compressed, encrypted, and uploaded over SFTP to a storage server. A local
SQLite catalog tracks everything so that restores, retention management, and
catalog recovery need no server-side indexing -- the server stores only opaque
blobs and a catalog pointer.

## Overview

There are three binaries. `bkup` is the command-line tool users interact with
directly. `bkupd` is a long-running root daemon that schedules backups, watches
the filesystem for changes, and serves the GUI over a Unix socket. `bkup-gui` is
a GTK4 desktop application that talks to the daemon and provides browse/restore
access without a terminal.

The design is conservative: a backup that does not have a durable record in the
event log is treated as if it never ran. The daemon refuses to start if it cannot
open the log file, and the CLI aborts before doing anything for the same reason.
Crash safety relies on the catalog being the ground truth: every state transition
is written to SQLite before the action it records (a blob row goes to
`BS_UPLOADED` only after the server confirms the put), so an interrupted backup
or restore leaves the catalog in a state that the next run can sweep and
continue.

## Architecture

### The catalog

The SQLite catalog, kept locally per user, is the backbone of the whole system.
It is opened in WAL mode so readers (GUI queries, status checks) do not block a
concurrent backup write, and with a busy timeout so a brief lock from a
concurrent prune does not immediately error.

The schema has five main tables. `snapshots` records each backup run with a
timestamp, a completion state (open/complete), and a kind (scheduled or
continuous). `files` tracks every path the scanner has seen, with a dirty/clean
state that drives what needs to be uploaded. `versions` is the historical record
of what a file looked like at each point in time; rather than a per-(snapshot,
version) membership table, each version carries a half-open interval
`[first_snapshot, last_snapshot)` so the table grows at roughly the rate of
file changes, not the product of files and snapshots. `version_blobs` maps
versions to their chunk hashes in sequence order. `blobs` is the content-
addressed store: one row per unique chunk, with its size and upload state.

A small `meta` table stores the KDF parameters (Argon2id salt, ops, mem), the
repo identity token used to guard against pointing the CLI at the wrong server
path, and the keycheck ciphertext used to verify the passphrase without exposing
the key.

### Files and versions

When a backup runs, the scanner walks every configured source directory and
compares each file's stat (size, mtime, inode, device) against what is in the
`files` table. Files whose metadata has not changed since they were last
captured are left clean; changed or new files are marked dirty. The backup then
reads every dirty file, chunks and uploads the novel blobs, and inserts a new
version row. The old version's interval is closed at that snapshot; the new
version's interval is left open (last_snapshot IS NULL), which means it is
implicitly live in every subsequent snapshot until it changes again. This
interval model is what makes the version table compact: an unchanged file
across a hundred snapshots contributes exactly one row.

When a file is removed from the source, the scanner marks it as no longer seen.
The version's open interval is closed the next time a backup runs and finds the
file missing. The version remains queryable for restore from any snapshot in its
interval.

### Content chunking

Files are split at content-defined boundaries using FastCDC, a rolling hash
algorithm that places cut points where the top bits of a running fingerprint
match a mask. bkup uses a two-level normalization: a stricter 22-bit mask before
the 1 MiB average size biases against early cuts; a looser 18-bit mask after it
biases toward cuts, pulling chunk sizes toward the mean. The result is chunks
between 256 KiB and 4 MiB with a 1 MiB average.

The gear table -- the 256 64-bit values the rolling hash indexes per byte -- is
generated at startup from a fixed splitmix64 seed, and it and the masks are
frozen forever. Changing any of these constants would produce different chunk
boundaries for the same file content, breaking deduplication against every chunk
already stored. The catalog chunker (used to push the catalog database itself to
the server) uses a much smaller average size (~16 KiB) and is not frozen: the
catalog is regenerated on every push, so different boundaries just mean a few
extra chunks on the next run.

Deduplication is decided on the plaintext chunk hash (BLAKE2b-256) before
compression and encryption. Two identical chunks in different files, or in the
same file across backups, produce the same hash and the second is not uploaded.
Because the hash is taken on plaintext, dedup is per-repo-key and there is no
convergent-encryption cross-user information leak.

Each chunk is compressed with Zstandard and then encrypted with
XChaCha20-Poly1305 (libsodium's secretstream). The key is derived from a
passphrase using Argon2id (OPSLIMIT_MODERATE / MEMLIMIT_MODERATE defaults).
Each repo carries its own random 16-byte salt, so repos with the same passphrase
produce different derived keys.

Uploaded blobs live under `blobs/XX/<hash>` on the server, where XX is the
first two hex digits of the hash. This fan-out prevents any single directory
from accumulating millions of entries on server filesystems that handle large
directories poorly.

### Catalog upload

After every backup and every prune, the local catalog is pushed to the server so
that it could be recovered without the local machine. The push VACUUMs the
catalog to a compact snapshot file, splits that file into chunks with the small
catalog chunker, encrypts and uploads any chunks not already on the server, and
then writes an encrypted manifest (a header plus an ordered list of chunk hashes)
to `catalog/<manifest-name>.enc`. A pointer file at `catalog/latest` is then
overwritten to name that manifest. The old manifest and any catalog chunks it
alone referenced are deleted after the new pointer is in place, so a crash during
GC only leaks a few bytes of orphaned server objects, which the next push cleans.

A sidecar SQLite file (`<catalog>.catmeta`) records which catalog chunks are
already on the server, avoiding a round-trip existence check per chunk on every
push. The sidecar is a local cache only: losing it is safe because the code
falls back to a remote existence check, and at worst re-uploads a handful of
chunks.

To recover the catalog on a new machine -- or after losing the local state --
`bkup fetch-catalog` downloads the manifest named by `catalog/latest`, verifies
each chunk's hash as it assembles the plaintext database, and installs it. A
passphrase is required; no other local state is needed.

## The daemon and multi-user model

`bkupd` runs as root and serves every configured user from a single Unix socket
at `/run/bkupd.sock` (mode 0666). Each incoming connection is authorized by
reading the peer's uid via `SO_PEERCRED`; the daemon looks up the matching
`[user "name"]` section in the config and serves only that user's data. A
client-supplied source name is ignored entirely: the daemon never acts on what
the client claims its identity to be.

Backup runs as root so it can read files regardless of ownership. Restore runs
differently: the daemon forks a child process, drops it to the calling user's
uid and gid (using `initgroups`, `setgid`, `setuid` in that order, with a check
that root cannot be re-acquired), and then performs all file creation in that
child. The kernel's own access checks then confine the restore to paths the
calling user could write. This means a user cannot restore files outside their
own writable tree, even though the blobs are decrypted by the root daemon before
the fork.

Config is root-administered: a single `/etc/bkup.conf` describes all users,
their sources, schedules, and retention. Users cannot modify their own backup
configuration. One `key_file` holds the single passphrase that protects all
repos; the derived key differs per repo because each has its own salt. This
trades per-user cryptographic isolation for a simpler operational model with one
password to manage.

The daemon's op mutex ensures that only one state-changing operation (backup,
prune, restore, verify) runs at a time. The GUI and scheduler both queue behind
this mutex; interactive commands wait up to two minutes rather than failing
immediately, so a running background catalog push does not cause a spurious
"busy" error for a GUI action.

One thread in the daemon runs the scheduler; another runs the filesystem watcher.
The main thread accepts IPC connections and dispatches each to a detached thread.
A `die()` inside any daemon-internal operation is caught by a per-thread
`longjmp` that logs the failure and returns control to the daemon's main loop
rather than exiting the process.

## Continuous backup

The watcher thread uses Linux `fanotify` with `FAN_REPORT_DFID_NAME` to receive
events for every directory under every configured source. Because fanotify inode
marks are not recursive, the watcher walks each source tree at startup with
`nftw` and marks every directory individually. When a new directory is created
or moved in, the watcher marks it on the fly from the `FAN_CREATE|FAN_ONDIR`
event, so new subtrees are covered without a full re-mark.

`FAN_REPORT_DFID_NAME` delivers parent-directory file handles and child names
rather than path strings. The watcher resolves these to full paths by opening
the directory handle with `open_by_handle_at` and reading the resulting fd from
`/proc/self/fd/N`. This approach works reliably across renames but requires
`CAP_SYS_ADMIN`, which is why the watcher can only run inside the root daemon.

Events are coalesced before a backup is triggered. The coalescer folds multiple
changed paths under one user into their common ancestor: if `/home/alice/a` and
`/home/alice/b/c` both change in quick succession, the backup scope becomes
`/home/alice`. A 2-second debounce quiet period is applied after the last event
in a batch; a 30-second cap fires a backup even under steady activity so changes
are not deferred indefinitely.

A continuous snapshot rescans only the coalesced root subtree. Every file
outside that subtree is unchanged, so its version's open interval implicitly
covers the new snapshot too -- there is nothing to carry forward explicitly. The
result is a complete snapshot of the whole source, built cheaply by only
re-examining the changed corner. A continuous snapshot cannot run if no prior
full backup exists; it would capture only the subtree and misrepresent itself as
a complete image.

Changes to bkup's own catalog directory are always suppressed to prevent every
snapshot write from triggering the next backup. Changes to paths matching
`exclude` patterns are also suppressed. A third class, `continuous-exclude`,
covers paths that should be backed up by scans but whose own churn -- a live
database, a mail store -- should not trigger continuous snapshots. Such a path
is captured whenever something else in the source changes, or by the next
scheduled backup, but it alone cannot mint a snapshot.

Because btrfs subvolumes have distinct `fsid` values and a single fanotify group
cannot hold marks spanning two of them, the watcher creates one fanotify group
per distinct fsid. A source that spans multiple btrfs subvolumes is therefore
fully covered, with path resolution happening in the group that owns each event's
filesystem.

## Hooks and virtual machines

A `[user]` section may name a `pre-backup` and a `post-backup` command
(`config.md`). They bracket every full backup of that user: `pre-backup` runs
before the scan and aborts the backup by exiting non-zero; `post-backup` runs
afterwards, exactly once, on success and on failure alike -- it is chained onto
the thread's `die()` handler (`src/cli/hook.c`) so that a backup cut short
midway still releases whatever the pre hook set up, and the daemon runs it
on shutdown if a backup is in flight. Continuous snapshots do not run hooks.

`vm-snap.sh` is the hook pair for libvirt/KVM guests with qcow2 disks. `begin`
takes an external disk-only snapshot of each running guest (`--quiesce` through
the guest agent when present, crash-consistent otherwise), so the guest writes
to a `*.bkup-overlay` file and the base image holds still for the scan; it also
stages `virsh dumpxml --security-info` and the UEFI NVRAM under
`/var/lib/bkup/vm`. `end` does `blockcommit --pivot` and deletes the overlay
through libvirt. `begin` runs `end` first, so an overlay left by a crashed run
is folded rather than stacked. The backup is then an ordinary scan of the
images directory plus the staging directory with `*.bkup-overlay` excluded:
each run reads and hashes every disk in full (upload is only what changed, via
dedup), so schedule it, not continuous.

Restore is manual by design (writing straight into `/dev` or the images pool
from the daemon is a foot-gun). `vm-restore.sh DOMAIN` (`-s SNAP` / `-A EPOCH`
for an older version, `-f` to overwrite an image already in place) does the
steps below: it restores the XML, reads the disk and NVRAM paths from it,
restores those into `/root/vmrestore/DOMAIN`, `qemu-img check`s each image,
moves them into place with their labels and `virsh define`s the domain. By
hand, as root since the set is root-owned:

```sh
# 1. pull the image and its definition out of the latest snapshot
#    (-s SNAP or -A EPOCH for an older one; `bkup -U vm snapshots` lists them)
sudo bkup -U vm restore \
    -f /var/lib/libvirt/images/fedora.qcow2 \
    -f /var/lib/bkup/vm/fedora.xml \
    /root/vmrestore
# -> /root/vmrestore/fedora.qcow2, /root/vmrestore/fedora.xml

# 2. put the disk back where the XML expects it, with libvirt's label
sudo mv /root/vmrestore/fedora.qcow2 /var/lib/libvirt/images/fedora.qcow2
sudo restorecon -v /var/lib/libvirt/images/fedora.qcow2

# 3. re-register the domain (replaces an existing definition of the same name)
sudo virsh -c qemu:///system define /root/vmrestore/fedora.xml

# 4. start it -- or from virt-manager, which lists it as soon as define returns
sudo virsh -c qemu:///system start fedora
```

A UEFI guest also needs its NVRAM back before step 3 (the path is the
`<nvram>` element in the XML):

```sh
sudo bkup -U vm restore -f /var/lib/bkup/vm/win11.nvram /root/vmrestore
sudo cp /root/vmrestore/win11.nvram /var/lib/libvirt/qemu/nvram/win11_VARS.qcow2
sudo chown qemu:qemu /var/lib/libvirt/qemu/nvram/win11_VARS.qcow2
sudo restorecon -v /var/lib/libvirt/qemu/nvram/win11_VARS.qcow2
```

For a drill while the real guest still exists, stop after step 1 and run
`qemu-img check` on the restored image, or bring it up as a clone: change the
XML's `<name>`, delete its `<uuid>`, point `<source file=...>` at a renamed copy
of the image, then do steps 2-4 on those.

## Prune and retention

`bkup prune` enforces retention by examining all complete snapshots and deciding
which to keep. The keep rules -- `--keep-last N`, `--keep-daily N`,
`--keep-weekly N`, `--keep-monthly N`, `--keep-yearly N` -- apply exclusively to
scheduled snapshots. For each periodic rule, prune keeps the newest scheduled
snapshot in each of the most recent N periods of that kind (day, ISO week, month,
calendar year).

Continuous snapshots are handled by a separate rule: every continuous snapshot
older than the most recent scheduled snapshot is dropped, because the scheduled
backup is a complete image of that moment and makes the earlier continuous
snapshots redundant. The continuous snapshots since the last scheduled backup are
kept -- they represent the fine-grained safety net for changes not yet covered by
a full run. If no scheduled snapshot exists at all, all continuous snapshots are
kept, since they are the only backups.

Whatever the rules produce, the newest snapshot in the catalog is always kept
regardless of kind, so there is always at least one restorable state.

After removing snapshot rows, prune cascades to versions and blobs. A version is
orphaned when no surviving snapshot falls within its `[first_snapshot,
last_snapshot)` interval; orphaned versions lose their blob references. Blobs
not referenced by any version are then deleted from the server -- but only after
the remote file is confirmed gone, so a failed deletion is retried on the next
prune run rather than leaving a dangling catalog reference.

Prune then removes what those deletions left empty, at both ends. In the
catalog, a directory version with no file or symlink version anywhere beneath it
is dropped, so restoring an old snapshot no longer recreates a skeleton of empty
directories; ancestry is matched on whole path components, so `/a/bc` never
counts as content of `/a/b`, and a directory holding only empty subdirectories
falls in the same pass as its children. Any `files.version_id` still pointing at
a removed directory version is cleared rather than left dangling, and the next
backup re-captures the directory if it has gained content by then. This is a
deliberate trade of fidelity for size: a directory that was genuinely empty on
disk when it was captured also stops being restored. On the server, deleting the
last orphaned blob out of a `blobs/<2 hex>` fan-out directory leaves it empty, so
prune attempts an `rmdir` on each directory it deleted from -- one that still
holds blobs simply fails the rmdir and is left alone.

## Restore

`bkup restore` has two resolution modes. In point-in-time mode (`-s SNAP`), it
reconstructs the exact contents of one snapshot: a file is restored from the
version whose interval covers that snapshot id. In as-of mode (`-A EPOCH`), each
path is resolved independently to its newest version captured at or before the
given timestamp; a file deleted before the cutoff is still restored from its last
known state.

The restore takes all targets in a single pass over one transport connection.
Directories are created first with mode 0700, then their metadata (mode and
mtime) is applied in a second pass in children-first order, so writing files
inside them during the first pass does not re-stamp the directory times the
backup recorded.

For each regular file, the blobs are fetched in sequence, decrypted, and
decompressed. After decompression the plaintext is hashed and compared against
the stored chunk hash; a mismatch aborts the restore rather than silently writing
corrupt data.

## Verify

`bkup verify` downloads every uploaded blob, decrypts it, decompresses it, and
checks both the plaintext hash and the size against the catalog. It also counts
version_blob references that point to a missing or not-uploaded blob. This is a
thorough but slow operation: it makes one network round trip per blob. There is
no local shortcut that avoids the download.

## GUI

`bkup-gui` is a GTK4 application that connects to the daemon's Unix socket.
It can trigger backups and restores, browse snapshots (directory tree and flat
list views), search paths, list file versions across time, inspect per-user
source info and storage stats, and run prune and verify. Restores support
selecting multiple directories and files in a single operation, with an optional
as-of time to scope what version of each is recovered.

The GUI communicates over a newline-delimited JSON protocol. Events stream back
from the daemon (log lines, entries, done) while a long-running operation is in
progress; non-blocking queries (status, source list, directory listing) complete
immediately without holding the op mutex.

Everything else serializes through a single host-wide op mutex, so a request can
arrive while a scheduled backup, continuous backup or catalog-push holds it. A
request may set `"wait": N` to bound how long it queues: `0` fails at once with
`busy`, and the default is 120s, chosen against the seconds-long routine work --
another user's full backup can hold the lock far longer. Waits are clamped to one
hour, since each waiter parks a thread and an fd. When a request is about to
block, the daemon first sends `{"event":"waiting","cmd":HOLDER,"wait":N}` so the
caller can say why it is stopped rather than appear hung; on giving up it sends
the `busy` error. After a wait the daemon re-checks that the caller is still
connected before starting work, so a request abandoned mid-queue (Ctrl-C, closed
window) is dropped instead of running a restore nobody is waiting for.

## Build

Dependencies: `sqlite3 libzstd libsodium libssh2` (all three binaries) plus
`gtk4` (GUI only). Header dependencies are auto-tracked (`-MMD`) so a changed
header rebuilds its dependents.

```sh
make all            # builds bin/bkup, bin/bkupd, bin/bkup-gui
make clean
sudo make relabel   # restorecon after rebuild (see SELinux below)
```

Deploy to `/usr/local/bin` (stops daemon, rebuilds clean, copies, relabels):

```sh
sudo ./doSyncBin.sh
```

### Tests

Standalone harnesses in `tests/` exercise the dangerous paths:
- `test_sweep.c` -- scoped deletion must respect path boundaries (not touch siblings)
- `test_coalesce.c` -- common-ancestor coalescing logic
- `test_catalog_push.c` -- catalog push round-trip
- `test_catalog_chunk.c` -- chunked-catalog round-trip and dedup locality
- `test_opwait.c` -- op-mutex admission: wait bound, waiting event, and dropping
  a request whose caller disconnected while queued
- `test_client.c` -- drives bin/bkup against a stub daemon: the request each
  command builds, and how replies are reported

Build and run one (example):

```sh
make all
gcc -std=c11 -D_GNU_SOURCE -Isrc $(pkg-config --cflags sqlite3) \
    tests/test_sweep.c $(ls obj/cli/*.o | grep -v main.o) \
    obj/common/*.o obj/platform/*.o \
    $(pkg-config --libs sqlite3 libzstd libsodium libssh2) -o /tmp/test_sweep
/tmp/test_sweep
```

## SELinux

`bkupd` requires an SELinux policy module (`selinux/bkupd.te`) to run with the
correct label. A rebuild under `/home` creates new inodes that default to
`user_home_t`, losing the `bkupd_exec_t` label; systemd then runs the daemon in
`init_t` and it hits denials. Always run `sudo make relabel` (or
`doSyncBin.sh`) after rebuilding. When extending the policy for a new operation,
edit `bkupd.te` directly; never use `audit2allow` on the setroubleshoot
suggestion, which grafts rules onto `init_t` system-wide.

## Config

See `config.md` for the full reference. The short version: `/etc/bkup.conf`
(root-owned, mode 0644) has a `[global]` section naming the server, root repo
path, key file, and optional global exclude patterns and retention defaults, then
one `[user "name"]` section per Linux user listing their source directories,
exclude patterns, and schedule. A flat sectionless config is also supported for
development or single-user use.

The systemd unit (`bkupd.service`) starts `bkupd -c /etc/bkup.conf`. The
scheduler inside the daemon reads backup/prune schedules from the config and runs
them in a single background thread; no cron entries are needed.

The CLI and GUI default to `/etc/bkup.conf` too (there is no per-user config
path; `-c` names one if you need it), so a configured user can run
`bkup snapshots` and `bkup sources` with no flags and no `sudo`: those read only
that user's own catalog.

`backup`, `restore`, `verify` and `prune` cannot run in-process as a normal user
-- they need the repo passphrase (`key_file`, root-only), an SSH identity for the
storage server, and write access to the event log. Run without `sudo`, the CLI
therefore sends them to `bkupd` over `/run/bkupd.sock` and streams the resulting
events to the terminal, exactly as the GUI does. The daemon identifies the caller
by peer uid, serves only that user's section, and forks+drops to the caller for
restore writes, so recovering your own files needs no privilege and can only
write where you could. `--wait SEC` bounds how long the request queues behind a
running operation (see the IPC section).

Two escapes from that routing: `-c CONFIG` runs the command locally against that
config instead (a self-contained dev or test setup with its own key and log, as
`mktest.sh` uses), and `--socket PATH` reaches a daemon started with `bkupd -s`.
`init`, `fetch-catalog` and `continuous` have no daemon verb and remain root-only.

## Limitations

**Linux only.** The filesystem watcher depends on `fanotify` and
`FAN_REPORT_DFID_NAME`, which are Linux-specific. The daemon authorization model
uses `SO_PEERCRED`. Neither macOS nor Windows is supported.

**Root required for the watcher.** `fanotify` with `FAN_REPORT_DFID_NAME`
requires `CAP_SYS_ADMIN`. Continuous backup is therefore only available when
running as the system daemon. Manual `bkup backup` and `bkup continuous` work
as any user who can write the catalog and reach the server, but automatic
filesystem-event-driven backups do not.

**Chunker constants are permanently frozen.** The FastCDC gear table, seed,
chunk size bounds, and masks in `chunk.c` cannot change without orphaning all
previously stored file blobs. The catalog chunker is not frozen (the catalog is
regenerated fresh on every push), but the file chunker is.

**Single passphrase for all users.** One root-held passphrase (`key_file`)
protects every user's repo. Inter-user isolation is enforced by the daemon at
the IPC level (peer uid), not by cryptographic separation. Someone who can read
`key_file` can decrypt any user's backups.

**Watcher capacity is fixed at compile time.** The watcher supports at most 64
users, 256 source trees, and 32 distinct btrfs filesystems/subvolumes.
Configurations that exceed these limits are silently truncated.

**Fanotify queue overflows lose events.** If the kernel event queue fills up
(controlled by `fs.fanotify.max_queued_events`), the watcher receives a
`FAN_Q_OVERFLOW` flag and logs a warning. The changes that were dropped will not
be captured until the next full scheduled backup. The watcher logs a summary of
drops rather than one line per event, but it is never silent about them.

**Verify downloads everything.** `bkup verify` fetches every uploaded blob from
the server, decrypts, decompresses, and re-hashes it. For large repos over a
slow link this is expensive; there is no local shortcut.

**The local catalog is required for most operations.** Backup, restore, verify,
and prune all need the local SQLite file. If it is lost, `bkup fetch-catalog`
can rebuild it from the server given the passphrase, but that requires the
server to be reachable and `catalog/latest` to be intact.

**No conflict resolution.** If two `bkup` processes run against the same catalog
concurrently (e.g. from different terminals), the op mutex inside the daemon
serializes them, but running the CLI directly in parallel bypasses that mutex.
The SQLite busy timeout (10 seconds) is the only guard; a long-running concurrent
operation will cause the second to fail.
