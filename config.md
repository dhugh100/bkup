# bkup Administrator's Guide

This document covers everything an administrator needs to deploy, configure, and
operate bkup: the configuration file, both operating modes, every CLI command
and option, daemon behavior, and the GUI. End users interact only with the GUI
or the `bkup` CLI; configuration and initialization are root tasks.

---

## Operating modes

### System daemon mode (the normal case)

One `bkupd` process runs as root and serves every Linux user on the host. It
reads a single config file (typically `/etc/bkup.conf`), holds the passphrase
in memory derived from the root-only `key_file`, and authorizes every incoming
request by reading the calling process's uid from the Unix socket
(`SO_PEERCRED`). Users never hold the key and never have direct access to the
storage server; all I/O runs through the daemon.

Deploy the systemd unit from `bkupd.service` in the repo root. Adjust the
`ExecStart` path if you install to `/usr/local/bin`:

```ini
[Service]
Environment=HOME=/root
ExecStart=/usr/local/bin/bkupd -c /etc/bkup.conf
```

`HOME` must be set so the daemon resolves `~/.ssh/known_hosts` and user
catalog paths. Start and enable:

```sh
sudo systemctl daemon-reload
sudo systemctl enable --now bkupd
```

`SIGHUP` (`systemctl reload bkupd`) causes the daemon to log a reload notice;
it re-reads the config on the next connection (not at signal time). `SIGTERM`
and `SIGINT` shut the daemon down cleanly.

### Flat / single-user mode

A config file with no section headers defines one ownerless `default` source.
Any user can run `bkup` against it directly. Useful for development or for a
machine with a single user who runs backups themselves. The passphrase comes
from `passphrase_file`, the `BKUP_PASSPHRASE` environment variable, or a tty
prompt -- not a system `key_file`. Run a dev daemon on a private socket:

```sh
bin/bkupd -c test.conf -s /tmp/bkupd.sock
bin/bkup-gui -s /tmp/bkupd.sock
```

The GUI and CLI both accept `-s SOCKET` to point at a non-default socket.

---

## Configuration file

### Location

`bkupd` requires an explicit `-c CONFIG` argument. The `bkup` CLI and the GUI
load `/etc/bkup.conf` unless `-c CONFIG` names another file (CLI only).

There is deliberately no per-user default. A flat/single-user config is still
supported -- name it with `-c`, which is how the test fixtures drive one --
but nothing on an installed host ever writes a per-user copy, so searching for
one only produced a default that could never resolve.

`/etc/bkup.conf` should be owned by `root:root` and mode `0644`: it contains no
secrets (the passphrase is in a separate file), so it must stay readable by the
users who run the CLI.

### Syntax

The file is line-oriented and INI-like.

- Blank lines and lines beginning with `#` are ignored.
- A `#` preceded by at least one space or tab ends the rest of a line (inline
  comment). A `#` that is not preceded by whitespace is kept verbatim, so it
  can appear in path values.
- Settings are `key = value`. Whitespace around the key and value is stripped.
  A line that is not blank, not a comment, and not a section header must contain
  `=` or the load aborts.
- `[global]` opens the global section.
- `[user "name"]` opens a per-user section where `name` is the Linux login
  name. Settings inside it apply only to that user.
- Keys before the first section header are applied as global keys; any
  user-specific keys before the first `[user]` section are treated as flat-mode
  source keys (for the sectionless `default` source).
- `[source "name"]` is a deprecated alias for `[user "name"]` and still works.

### Tilde expansion

A leading `~/` in the values of `key_file`, `log_file`, `db`, `source`,
`exclude`, `continuous-exclude`, and `passphrase_file` expands as follows:

- In `key_file` and `log_file`: expands to the home directory of the running
  process (typically root for the system daemon).
- In `db`, `source`, `exclude`, `continuous-exclude`: expands to the home
  directory of the **owning user** (the name in `[user "name"]`), looked up
  via `getpwnam`. This means `~/.local/state/foo` under a `[user "alice"]`
  section resolves to `/home/alice/.local/state/foo` even when the daemon runs
  as root.

A bare `~` (no trailing slash) also expands to the home directory.

---

## `[global]` keys

All are valid before the first section header and inside `[global]`.

### `server` (required)

The hostname or IP address of the SSH storage server. The daemon opens an SSH
connection to this host for every backup, restore, prune, and verify. There is
no persistent connection; each operation connects and disconnects.

```ini
server = backup-server
```

### `port`

SSH port number. Default `22`.

```ini
port = 22
```

### `ssh_user` (alias: `user`)

The SSH login name on the storage server. Defaults to the process's `$USER`
environment variable, or the login name from `getpwuid(getuid())`. Only one
user ever connects: the root daemon. Individual Linux users never have SSH
access to the storage server.

```ini
ssh_user = backup
```

### `repo`

Root directory on the storage server under which all users' repos live. Each
user gets a subdirectory named by the slugified version of their login name:
`repo/<slug>`. A slug is the username lowercased with runs of non-alphanumeric
characters collapsed to a single `-`. For example, `[user "Alice Smith"]`
yields slug `alice-smith` and repo path `<repo>/alice-smith`.

If a user section sets its own `repo` key, that full path is used directly and
this global root is ignored for that user.

This key or a per-user `repo` override is required. If neither is set, load
aborts.

```ini
repo = /mnt/raid1/bkup
```

### `key_file` (required in system mode)

Path to a file holding the single backup passphrase. The file must be readable
by root only (`0600`). The daemon reads this file at key derivation time (once
per `init`, then once per backup/restore/verify/prune connection). It is not
kept open persistently.

```ini
key_file = /etc/bkup.key
```

Create the passphrase file:

```sh
printf '%s' 'correct horse battery staple' > /etc/bkup.key
chown root:root /etc/bkup.key
chmod 0600 /etc/bkup.key
```

In flat mode without `key_file`, the passphrase is sourced from
`passphrase_file` (per-user key), the `BKUP_PASSPHRASE` environment variable,
or an interactive tty prompt, in that order.

### `log_file`

Path to the durable event log. Every started/completed/failed event for every
backup, restore, prune, and verify is written here with a timestamp, flushed
per line so it survives crashes. Default: `/var/log/bkup/bkup.log`.

The daemon (and CLI) refuse to start if this file cannot be opened. Create the
directory writable by the group your admin login belongs to if you need
non-root CLI access:

```sh
sudo install -d -m 2775 -g wheel /var/log/bkup
sudo touch /var/log/bkup/bkup.log
sudo chgrp wheel /var/log/bkup/bkup.log
sudo chmod 0664 /var/log/bkup/bkup.log
```

In flat/dev mode, point this at a user-writable path to avoid needing root:

```ini
log_file = /tmp/bkup-dev.log
```

### `exclude` (repeatable)

An `fnmatch(3)` pattern applied to every user's source tree, merged with any
per-user `exclude` lines at config load time. The same pattern rules apply as
for per-user `exclude` (see below). Useful for patterns that should apply to
all users: cache directories, editor swap files, shell history.

```ini
exclude = .cache
exclude = *.swp
exclude = .bash_history
```

### `continuous-exclude` (repeatable)

Like global `exclude` but applied to the `continuous-exclude` list of every
user. Paths matching these patterns are still backed up by scheduled scans but
will not trigger continuous snapshots. Useful for hot files that every user is
likely to have, such as browser cache databases.

```ini
continuous-exclude = .mozilla/firefox/*/places.sqlite
```

### `prune` (global default)

A prune schedule applied to any user that has no per-user `prune` key. Same
grammar as per-user `prune`. See Schedules below.

```ini
prune = weekly 04:00
```

### `keep-last`, `keep-daily`, `keep-weekly`, `keep-monthly`, `keep-yearly` (global defaults)

Default retention counts. Applied to any user that has not set the
corresponding key in their own section. Per-user values always take precedence;
these are only fallbacks.

```ini
keep-daily   = 7
keep-weekly  = 4
keep-monthly = 12
keep-yearly  = 1
```

---

## `[user "name"]` keys

`name` is a Linux login name. It must match the owning user exactly; the daemon
authorizes connections by resolving `SO_PEERCRED` to a login name and looking
up that name in the config. A user whose name is not in the config gets a
connection error.

### `source` (alias: `root`, repeatable, required)

A directory tree to back up. List as many `source` lines as needed. Absolute
paths only. Tilde expansion applies.

The system daemon reads these paths as root, so the user's own read permissions
do not limit coverage. However, if a source directory is not owned by the
configured user, a warning is logged at backup time: restored files will not
carry their original ownership, because the restore child drops to the calling
user's uid before writing.

```ini
source = /home/alice/Documents
source = /home/alice/Projects
```

### `exclude` (repeatable)

An `fnmatch(3)` pattern. Two matching rules:

- **Without `/`**: matched against the **basename** of each path. `*.pyc` skips
  every `.pyc` file anywhere in the tree.
- **With `/`**: matched against the **full path**. `/home/alice/Projects/build`
  skips exactly that directory (and everything inside it). Patterns with a
  trailing `/` still work as expected since the path being matched is always
  absolute.

A leading `~/` is expanded to the owning user's home directory (not the
running user), so `~/.local/state` in a `[user "alice"]` section correctly
anchors to `/home/alice/.local/state`.

Excluded directories are pruned during the full-backup walk, so their entire
subtrees are never visited. The continuous-backup watcher applies the same
exclusion before triggering a snapshot: a change inside an excluded directory
never fires a continuous backup. This makes `exclude` the right choice for
hot databases or build trees that should not be backed up at all.

Global excludes are merged after per-user excludes; the combined list is what
the scanner and watcher test.

```ini
exclude = node_modules
exclude = /home/alice/Projects/build
exclude = *.tmp
exclude = ~/.local/state/foo-app
```

### `continuous-exclude` (repeatable)

Same pattern syntax as `exclude`, but with different semantics: a matching path
**is still backed up** by full scans and appears in snapshots, but a change to
it alone does not trigger a continuous snapshot. Use this for files that must
be captured but whose constant write activity would otherwise mint a continuous
snapshot every few seconds -- live databases (`*.sqlite`, Thunderbird's
`global-messages-db.sqlite`), watched mail stores, and similar.

A path that changes only according to `continuous-exclude` is captured the next
time a continuous snapshot fires for some other reason, or by the next
scheduled full backup. If such paths are the only things in a source that ever
change, ensure a `backup` schedule is set; without it and without other
changing files to trigger continuous backups, the source may never be captured.

```ini
continuous-exclude = ~/.thunderbird/*/global-messages-db.sqlite
continuous-exclude = ~/.config/chromium/Default/History
```

### `db`

Path to the local SQLite catalog. This file is the authoritative local record:
it tracks every file, version, blob, and snapshot. Losing it without a server
copy means running `bkup fetch-catalog` before restoring.

Default: `~/.local/share/bkup/<slug>.db`, expanded to the owning user's home.
For example, `[user "alice"]` defaults to
`/home/alice/.local/share/bkup/alice.db`.

The daemon creates the directory automatically if it does not exist.

```ini
db = /home/alice/.local/share/bkup/alice.db
```

### `repo`

Overrides the global `repo/<slug>` derived path with a full path on the
storage server. Useful when a user's repo should live in a non-standard
location.

```ini
repo = /mnt/backup-archive/special/alice
```

### `continuous`

Boolean (`on`/`off`, `true`/`false`, `yes`/`no`, `1`/`0`). Whether the
daemon's filesystem watcher arms for this user's sources. Default `on`.

When `off`, no fanotify marks are placed for this user; only scheduled
`backup` runs (and manual `bkup backup` or `bkup continuous`) produce
snapshots.

```ini
continuous = off
```

### `backup`

Backup schedule (see Schedules below). When set, the daemon's scheduler thread
runs `bkup backup` for this user at the specified intervals. Unset means no
automatic backups; only continuous (watcher-triggered) snapshots are produced,
or the admin runs backups manually.

```ini
backup = daily 02:00
```

### `prune`

Prune schedule (see Schedules below). If no `keep-*` rules are set (neither
per-user nor global), the scheduled prune is skipped with a warning. Prune
removes old snapshots and unreferenced blobs from the server; without it the
repo grows without bound.

```ini
prune = weekly 04:00
```

### `keep-last`

Keep the N most recent **scheduled** snapshots regardless of age. This is a
count of full backup runs, not a time window.

```ini
keep-last = 5
```

### `keep-daily`

Keep the newest scheduled snapshot from each of the most recent N calendar
days that had a scheduled backup. Days without a scheduled backup leave a gap
in the daily set; the rule does not invent a snapshot for missing days.

```ini
keep-daily = 7
```

### `keep-weekly`

Keep one snapshot per ISO week for the N most recent weeks that had a scheduled
backup. ISO week numbering is used (Monday as first day of week; weeks span
year boundaries correctly).

```ini
keep-weekly = 4
```

### `keep-monthly`

Keep one snapshot per calendar month for the N most recent months that had a
scheduled backup.

```ini
keep-monthly = 12
```

### `keep-yearly`

Keep one snapshot per calendar year for the N most recent years that had a
scheduled backup.

```ini
keep-yearly = 1
```

All `keep-*` rules apply **only to scheduled snapshots** (produced by `bkup
backup`). Continuous snapshots are managed separately: at each prune run, every
continuous snapshot older than the most recent scheduled snapshot is deleted.
Continuous snapshots newer than the most recent scheduled snapshot are kept,
because they are the fine-grained safety net for changes since the last full
backup. If no scheduled snapshot exists, all continuous snapshots are kept.

### `passphrase_file`

For flat/single-user configs only. Path to a file holding the passphrase for
this source's repo. Ignored when a global `key_file` is set. See Passphrase
sourcing below.

---

## Schedules

Both `backup` and `prune` use the same grammar:

```
hourly
daily   [HH:MM]
weekly  [HH:MM]
monthly [HH:MM]
```

Time defaults to `00:00`. Weekly defaults to Monday. Monthly defaults to the
1st of the month. The scheduler wakes every 60 seconds and fires any schedule
that is past due since the last run.

Examples:

```ini
backup = hourly
backup = daily 02:30
backup = weekly 03:00
prune  = monthly
```

The scheduler fires at the **next occurrence** after the daemon starts; it does
not try to catch up for runs that were missed while the daemon was down.

---

## Passphrase sourcing

The passphrase is used to derive the encryption key for every operation that
touches the server (backup, restore, verify, prune, init, fetch-catalog). It is
sourced in this order:

1. **Global `key_file`** (system daemon mode): the daemon reads the first line
   of this file, stripping a trailing newline. This is the normal case. Users
   never see the passphrase.

2. **Per-user `passphrase_file`** (flat/dev mode): if no global `key_file` is
   set, the source's own `passphrase_file` is tried next.

3. **`BKUP_PASSPHRASE` environment variable**: if neither file is configured or
   readable, this environment variable is used. Useful in scripted or container
   environments.

4. **Interactive tty prompt**: if none of the above are available, the user is
   prompted on `/dev/tty` with echo disabled. On `init`, a confirmation prompt
   follows.

---

## Initial setup procedure

### 1. Write the config file

Install `/etc/bkup.conf` and `/etc/bkup.key` as described above.

### 2. Initialize each user's repo

For each `[user "name"]` section, run `bkup init` as the daemon user (root)
or via `bkup -U name init`:

```sh
sudo bkup -c /etc/bkup.conf -U alice init
```

`init` does three things: derives and verifies the encryption key from the
passphrase (Argon2id; takes a few seconds); creates the repo directory
skeleton on the storage server (`blobs/`, `catalog/`); and writes the KDF
parameters and keycheck token to the local catalog so subsequent operations
can verify the passphrase without storing it.

If the repo already exists (from a previous install or a different machine),
`init --force` wipes it and starts clean. Use with caution: this destroys all
existing backups in that repo.

### 3. Run the first backup

```sh
sudo bkup -c /etc/bkup.conf -U alice backup
```

The first backup scans every source directory, chunks and encrypts every file,
uploads all novel blobs, and pushes the catalog to the server. It will take
longer than subsequent runs.

### 4. Start the daemon

```sh
sudo systemctl start bkupd
```

From this point the scheduler and watcher handle automatic backups according to
the configured schedules.

---

## CLI reference: `bkup`

```
bkup [-c CONFIG] [-U USER] COMMAND [ARGS]
bkup --version
```

### Global flags

**`-c CONFIG`**  
Config file path. Defaults to `/etc/bkup.conf`.

**`-U USER`**  
Select a specific user section by name. If omitted, the CLI selects the section
whose owner matches the process's uid (or the flat `default` source). Use
`-U name` to run as root on behalf of a specific user, or to target a section
when the flat-mode default is not what you want.  
(Deprecated alias: `-S`. Accepted but logs a no-op notice.)

**`-h` / `--help`**  
Print usage and exit.

**`--version`**  
Print the version (`bkup X.Y.Z`) and exit. Answered before the config is read,
so it works on a host with no `/etc/bkup.conf`. The string is compiled in from
`src/common/version.h`, which `packaging/doRelease.sh` bumps alongside
`Version:` in the spec, so it always matches the installed package.

Every CLI run resolves the command name first: an unknown command prints usage
and exits 2 without touching the config or the log. A recognized command then
opens the event log before doing anything, and aborts if the log cannot be
opened -- a backup with no durable record is exactly the failure this guards
against.

The two read-only commands, `snapshots` and `sources`, are exempt: they change
neither the repo nor the catalog, so there is nothing about them worth
refusing to run over. If the log cannot be opened they proceed with output on
stderr instead, and skip the start/finish log lines. This is what lets a normal
user run `bkup snapshots` on a host where `bkupd` runs as root and owns
`/var/log/bkup/bkup.log`; the log stays root-only, which keeps the audit trail
trustworthy.

### Running as a normal user

On a daemon-managed host, a user with a `[user "name"]` section can run:

```sh
bkup snapshots      # their own snapshots
bkup sources        # what the config parsed to
```

with no flags and no `sudo`: the config defaults to `/etc/bkup.conf` and the
catalog is `~/.local/share/bkup/<name>.db`, which the user owns. Everything
else (`backup`, `restore`, `prune`, `verify`, `init`, `fetch-catalog`) needs
root, both for the event log and to read `key_file`.

---

### `bkup init [--force]`

Create the repo on the storage server and initialize the local catalog.

The passphrase is prompted (or read from `key_file` / `passphrase_file` /
`BKUP_PASSPHRASE`). Key derivation with Argon2id runs once; this takes a
moment. The derived key is verified immediately with a stored keycheck token;
if the passphrase is wrong the command aborts with "wrong passphrase".

After init succeeds, the local catalog contains the KDF parameters needed to
re-derive the key on future operations, and the server has the repo config
file, `blobs/`, and `catalog/` directories.

`--force` wipes the existing repo on the server and removes the local catalog
before re-initializing. All previously stored backups for this user are lost.

Run `init` once per user. If you later want to start over, run it again with
`--force`.

---

### `bkup backup`

Scan all configured sources and upload any changed files.

The scanner walks each source directory, checks every file's stat (size, mtime,
inode, device) against the catalog, and marks changed or new files dirty.
Excluded paths are skipped. The catalog database itself (and its `-wal`/`-shm`
sidecars) is always silently ignored.

For each dirty file, the backup reads the content, splits it into content-
defined chunks (FastCDC), hashes each chunk (BLAKE2b-256), compresses it
(Zstandard), encrypts it (XChaCha20-Poly1305), and uploads it -- skipping any
chunk whose hash is already in the catalog as uploaded (deduplication).
Symlinks and directories are recorded with their metadata but have no blob
content.

After all files are uploaded, the snapshot is marked complete and the catalog
is pushed to the server. A backup that exits before the snapshot is marked
complete (due to a crash or kill) leaves the snapshot in the OPEN state; the
next backup automatically rolls it back, restoring the catalog to the
pre-backup state, then proceeds with a fresh snapshot.

Changes to a file while it is being read (detected by comparing pre- and
post-stat) leave that file dirty for the next backup run rather than recording
a torn version.

If a source directory is not owned by the configured user, a warning is logged
(ownership will not round-trip on restore) but the backup continues.

Takes no arguments.

---

### `bkup continuous PATH`

Run a continuous (scoped) snapshot over the subtree rooted at `PATH`.

`PATH` must resolve (via `realpath`) to a path inside one of the configured
sources. The scanner walks only `PATH` and everything beneath it; every file
outside that subtree is unchanged and is carried forward implicitly by the
snapshot model. The result is a complete snapshot of the whole source, but the
work done is proportional to the size of the changed subtree rather than the
whole source.

A continuous snapshot requires at least one prior full backup to exist. Without
it, the snapshot would be partial (only `PATH`) and would misrepresent itself as
a complete image.

After the snapshot is committed, the catalog is immediately pushed to the
server (unlike the daemon watcher, which defers the push and batches it).

This command is primarily used by the daemon watcher internally; run it manually
if you want to back up a specific subtree on demand without a full scan.

`bkup spot PATH` is a deprecated alias; it still works but logs a deprecation
warning.

---

### `bkup restore [-s SNAP] [-A EPOCH] [-p DIR]... [-f FILE]... DEST`

Restore files to `DEST`. If run under the system daemon (via the GUI or a
future daemon-side CLI wrapper), the restore write phase runs as the calling
user. If run directly as root with the CLI, files are written as root.

**`DEST`** (required)  
Destination directory. Created if it does not exist. Each restored item lands
at `DEST/<name>`: a directory `/home/alice/Projects` restored to `/tmp/restore`
produces `/tmp/restore/Projects/...`.

**`-s SNAP`**  
Restore the exact contents of snapshot `SNAP` (an integer snapshot id from
`bkup snapshots`). Exactly one version is live per path at any snapshot. If
omitted, and no `-A` is given, the most recent complete snapshot is used.

**`-A EPOCH`**  
As-of mode: restore each path at its newest version captured at or before the
Unix epoch timestamp `EPOCH`. This is per-path: a file that existed at the
cutoff but was deleted later is still restored from its last known content.
Files added after `EPOCH` are omitted. Mutually exclusive with `-s` in effect
(if both are given, `-A` wins).

**`-p DIR`** (repeatable)  
Restore the directory `DIR` and everything beneath it. `DIR` is recreated by
name under `DEST`: `DEST/basename(DIR)/...`. Specify as many `-p` targets as
needed; all are restored in a single pass over one transport connection.

**`-f FILE`** (repeatable)  
Restore the single file or symlink `FILE`. It lands at `DEST/basename(FILE)`.
Specify as many `-f` targets as needed.

With no `-p` or `-f`, the entire snapshot is restored into `DEST`.

**`-o UID:GID`**  
Force restored files to be owned by `UID:GID` rather than the backup-time
owner. Used internally by the daemon for GUI restores where the files must land
as the calling user. Not normally needed on the CLI.

Restore proceeds in two passes: files and symlinks first, then directory
metadata (mode and mtime) in children-first order so the act of writing files
into a directory does not re-stamp the directory's mtime to now.

For each regular file, every blob is fetched from the server, decrypted, and
decompressed. The plaintext is rehashed and compared to the stored chunk hash;
a mismatch aborts the restore for that file rather than writing corrupt data.

---

### `bkup verify`

Download and verify every stored blob.

For each blob in the catalog marked as uploaded, `verify` fetches it from the
server, decrypts it, decompresses it, and checks both the BLAKE2b-256 hash and
the stored size. Any failure is logged as an error. Additionally, any
`version_blobs` row that references a blob not present or not uploaded is
counted as an orphan reference.

At the end, the command reports total blobs checked, failures, and orphan
references. Exit code is non-zero if any failure or orphan was found.

This is a thorough but slow operation: it makes one round trip per blob. For
large repos over a slow or metered link, schedule it infrequently. There is no
local cache that would allow a faster integrity check.

Takes no arguments.

---

### `bkup snapshots`

List all snapshots in the local catalog. Output is a table of snapshot id,
creation time, state (complete/OPEN), hostname, and file count (versions
live in that snapshot). Takes no arguments.

An OPEN snapshot is one that started but did not complete (crash or kill during
a backup). The next `bkup backup` automatically rolls these back.

---

### `bkup sources`

Print a summary of all users in the config: name, scope (user/system), number
of configured sources, and server repo path. Takes no arguments.

Useful to verify that the config was parsed as expected.

---

### `bkup fetch-catalog [--force]`

Rebuild the local catalog from the server.

Downloads the encrypted catalog manifest from `catalog/latest`, decrypts it
(requiring the passphrase), then fetches and verifies each catalog chunk in
order, assembles them into a temporary file, verifies each chunk's hash, and
renames the result into place as the local catalog.

This is the recovery procedure for a lost or corrupted local catalog. After
`fetch-catalog` completes, the catalog is at the same state as it was at the
last `upload_catalog` call (after the most recent backup, prune, or continuous
snapshot push), and `bkup restore` can proceed normally.

`--force` overwrites an existing local catalog without prompting. Without it,
`fetch-catalog` aborts if the catalog file already exists.

This command requires only the passphrase and a reachable server; no local
state is needed.

---

### `bkup prune [--keep-last N] [--keep-daily N] [--keep-weekly N] [--keep-monthly N] [--keep-yearly N] [-n]`

Remove old snapshots and their unreferenced blobs from the server.

At least one `--keep-*` rule is required.

**`--keep-last N`**  
Keep the N most recent scheduled snapshots.

**`--keep-daily N`**  
Keep one scheduled snapshot per calendar day for the N most recent days that
had one.

**`--keep-weekly N`**  
Keep one scheduled snapshot per ISO week for the N most recent weeks that had
one.

**`--keep-monthly N`**  
Keep one scheduled snapshot per calendar month for the N most recent months
that had one.

**`--keep-yearly N`**  
Keep one scheduled snapshot per calendar year for the N most recent years that
had one.

When multiple rules match the same snapshot, the snapshot is kept. Rules never
cause a snapshot to be deleted that another rule protects.

Continuous snapshots are not subject to these rules. Instead: any continuous
snapshot **older** than the most recent scheduled snapshot is deleted; any
continuous snapshot **newer** is kept. If no scheduled snapshot exists, all
continuous snapshots are kept. The newest snapshot of any kind is always kept.

After pruning snapshot rows, the cascade to versions and blobs runs atomically:
versions whose entire `[first_snapshot, last_snapshot)` interval falls in a
pruned gap are deleted; blobs not referenced by any surviving version are
deleted from the server. Blob deletion is retried on failure: a blob row
is removed from the catalog only after its remote file is confirmed gone.

Finally, the catalog is pushed to the server to record the post-prune state.

**`-n` / `--dry-run`**  
Print what would be removed without making any changes. The catalog is not
modified and nothing is deleted from the server. Use this to verify retention
rules before running for real.

---

## Daemon reference: `bkupd`

```
bkupd -c CONFIG [-s SOCKET]
```

**`-c CONFIG`** (required)  
Path to the config file. Re-read per connection (not held open continuously),
so changes to `exclude` or `source` paths take effect on the next operation
without restarting the daemon.

**`-s SOCKET`**  
Unix socket path. Default: `/run/bkupd.sock` (mode `0666`). Use a private
path (e.g. `/tmp/bkupd-dev.sock`) when running a development daemon as a
non-root user so it does not conflict with the system daemon.

### Signals

| Signal | Effect |
|--------|--------|
| `SIGTERM`, `SIGINT` | Stop the daemon gracefully. In-flight connections are allowed to complete. |
| `SIGHUP` | Logs a reload notice. Config is re-read per connection, so no restart is needed to pick up config changes. |
| `SIGPIPE` | Ignored. Broken client connections do not kill the daemon. |

### Socket and authorization

The socket is created at startup with mode `0666`, so any local user can
connect. The daemon reads `SO_PEERCRED` from each connection to determine the
caller's uid, resolves it to a login name, and selects that name's `[user]`
section. The client's connection is refused if no section matches. A
client-supplied source name in any IPC request is always ignored.

If another daemon is already listening on the socket path (connect succeeds),
`bkupd` refuses to start. If the socket file exists but no process is listening
(stale socket), it is removed before binding.

### Scheduler thread

The scheduler wakes every 60 seconds and checks every user's backup and prune
schedules. It fires any that are past due, running them serially (not
concurrently). After each backup it also clears the catalog-push debt for
that user (since the backup already pushed the full catalog). The scheduler
itself is serialized through the op mutex, so a scheduled backup waits for
any in-progress interactive operation rather than racing it.

The scheduler does not catch up for missed runs: if the daemon was down at
the scheduled time, the run is skipped.

### Watcher thread

The watcher places `fanotify` inode marks on every directory under every
configured source at startup, then polls for filesystem events. When events
arrive for a user:

1. The affected path is looked up against the source map.
2. Changes inside the catalog directory, excluded paths, and
   `continuous-exclude` paths are silently ignored.
3. Accepted paths are fed into the coalescer, which tracks the common ancestor
   of all changed paths in the current batch.
4. If the batch spans multiple sources, the pending root is flushed first.
5. After 2 seconds of quiet (no new events), or after 30 seconds of continuous
   activity, the coalesced root is handed to `bkup continuous` for that user.
6. After a continuous snapshot completes, the catalog upload is deferred and
   batched; the scheduler thread flushes it on the next 60-second tick.

New directories created or moved in during a watch session are marked on the
fly from the `FAN_CREATE|FAN_ONDIR` event. If a directory cannot be marked
(permissions, `fs.fanotify.max_user_marks` exhausted), a warning is logged and
changes inside it may be missed until the next full backup.

A fanotify queue overflow (`FAN_Q_OVERFLOW`) logs a warning at most once per
30 seconds and reports the count of dropped events. Dropped events will not be
captured until the next full or coalesced scan.

---

## GUI reference: `bkup-gui`

```
bkup-gui [-s SOCKET]
```

**`-s SOCKET`**  
Connect to a daemon on a non-default socket. Default: `/run/bkupd.sock`.

The GUI requires the daemon to be running. It does not operate in standalone
mode. All backup, restore, and prune operations run inside the daemon and are
logged to the event log.

### Window layout

The window is split horizontally. The left pane is the recovery list. The right
pane shows read-only settings and live status for the selected source. A log
strip at the bottom shows operation output as it arrives.

If more than one user section is in the config, a source dropdown appears in
the left pane to switch between them. Switching sources clears the recovery
list.

### Header bar controls

**Build List** (blue, left side)  
Clicking opens a fly-out with a calendar and "Most recent (now)". Select a day
from the calendar or click "Most recent" to set the as-of date, which
immediately starts building the recovery list for that date. The list is
fetched from the daemon in the background; a spinner runs while it loads.

If the filter box has text when Build List is clicked, only paths containing
that substring are fetched from the daemon, making the build faster for large
repos.

**As-of clear button** (appears when a dated cutoff is active)  
Shows the active date (e.g. "As of 2025-11-15"). Clicking resets to "most
recent" and immediately rebuilds the list.

**Recover** (grayed until rows are selected, left side)  
Opens the Recover dialog for the selected items. Disabled until Build List has
been run and at least one row selected.

**Backup Now** (right side)  
Starts a full backup for the current source. The button is disabled and a
spinner appears while the backup runs. After completion (success or failure),
the status pane refreshes.

### Recovery workflow

1. Optionally type a substring in the filter box to scope the build.
2. Click **Build List** and pick an as-of date or "Most recent (now)".
   The list loads with every path ever captured for this source, annotated:
   - Regular file icon with size shown in grey.
   - Folder icon for directories.
   - Symlink icon for symlinks.
   - Greyed-out paths with a "deleted" tag are gone from the current backup
     but still recoverable from older versions.
3. Select one or more rows (standard Ctrl/Shift click). The filter box narrows
   the visible list but does not deselect hidden items.
4. Click **Recover**. A dialog shows the count of selected items and the
   as-of date. The destination field defaults to the item's original parent
   directory (for a single item) or `/tmp/restore` (for multiple). Adjust as
   needed and click Recover.
5. Progress appears in the log strip. Each recovered directory and file is
   restored at its version as of the selected date.

Restore always uses as-of semantics in the GUI: each selected item is restored
at its newest version at or before the active cutoff. This means a file deleted
before the cutoff is still recovered from its last content.

### Filter box

A case-insensitive substring filter over the full path. Two uses:

- **Before Build List**: pre-scopes the fetch so only matching paths are
  returned from the daemon. Useful for large repos where building the full list
  is slow.
- **After Build List**: filters the already-built list live. All items are in
  memory; the filter costs nothing extra.

Typing in the filter box after a list is built refines it instantly. Clearing
the filter re-shows all items.

### Right pane: Settings

Read-only display of the current source's configuration as understood by the
daemon: server hostname, repo path, local catalog path, source directories,
backup/prune schedules, retention counts, and whether continuous backups are
on. Refreshes when the source changes or after a backup or prune.

The note "Edit /etc/bkup.conf as admin to change" is shown there as a reminder
that these fields are not editable in the GUI.

### Right pane: Status

Live snapshot counts, time of the last continuous snapshot, total server repo
size (stored, i.e. post-compression/encryption), and the dedup+compression
ratio (logical bytes / stored bytes). Refreshes every 20 seconds and after any
backup or prune.

### Log strip

Monospace read-only text view at the bottom. Receives every log event sent by
the daemon during an operation, prefixed with the log level
(`[I]` info, `[W]` warn, `[E]` error, `[F]` fatal). The view auto-scrolls
to the bottom as lines arrive. A `[op done]` or `[op FAILED]` summary line
is appended when the operation completes.

---

## Full configuration example

```ini
# /etc/bkup.conf   root:root 0644

[global]
server      = backup-server
port        = 22
ssh_user    = backup
repo        = /mnt/raid1/bkup
key_file    = /etc/bkup.key
log_file    = /var/log/bkup/bkup.log

# global patterns applied to all users
exclude           = .cache
exclude           = .bash_history
exclude           = *.swp
exclude           = *.tmp
continuous-exclude = .mozilla/firefox/*/places.sqlite

# global retention defaults (overridden per user if needed)
prune        = weekly
keep-daily   = 7
keep-weekly  = 4
keep-monthly = 12
keep-yearly  = 1

[user "root"]
source       = /root
source       = /etc
source       = /usr/local
# system files change rarely; continuous is not needed
continuous   = off
backup       = daily 02:00
prune        = weekly 03:00

[user "alice"]
source       = /home/alice
exclude      = /home/alice/Downloads
exclude      = /home/alice/.local/share/Steam
continuous-exclude = ~/.thunderbird/*/global-messages-db.sqlite
continuous   = on
backup       = daily 02:30
# keep-* not set here; uses global defaults above

[user "bob"]
source       = /home/bob/Documents
source       = /home/bob/Projects
# override weekly retention for this user
keep-weekly  = 8
continuous   = on
backup       = daily 03:00
```

```sh
# /etc/bkup.key   root:root 0600
printf '%s' 'correct horse battery staple' > /etc/bkup.key
chown root:root /etc/bkup.key && chmod 0600 /etc/bkup.key
```

Initialize each user's repo before starting the daemon:

```sh
sudo bkup -c /etc/bkup.conf -U root  init
sudo bkup -c /etc/bkup.conf -U alice init
sudo bkup -c /etc/bkup.conf -U bob   init
```

Then run the first full backup for each user:

```sh
sudo bkup -c /etc/bkup.conf -U root  backup
sudo bkup -c /etc/bkup.conf -U alice backup
sudo bkup -c /etc/bkup.conf -U bob   backup
```

Start the daemon:

```sh
sudo systemctl enable --now bkupd
```

---

## Disaster recovery

If the machine is lost or the local catalog is deleted:

1. Install bkup on the new machine.
2. Copy or recreate `/etc/bkup.conf` and `/etc/bkup.key`.
3. Run `fetch-catalog` for each user (requires the passphrase and server):

   ```sh
   sudo bkup -c /etc/bkup.conf -U alice fetch-catalog
   ```

4. Restore files:

   ```sh
   sudo bkup -c /etc/bkup.conf -U alice restore /restore/alice
   ```

   Or use the GUI once the daemon is running.

If you know the passphrase but do not have the config, you can reconstruct the
minimum config manually: `server`, `repo`, `key_file` pointing to a file you
create, and `[user "name"]` sections with the correct repo paths. The KDF
parameters are stored in the repo's `config` file on the server and are read
by `fetch-catalog` directly, so you do not need to know the Argon2id parameters
in advance.
