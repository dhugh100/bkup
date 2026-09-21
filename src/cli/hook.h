#ifndef BK_HOOK_H
#define BK_HOOK_H

#include "common/config.h"

/* Run one hook command via /bin/sh -c with BKUP_USER, BKUP_HOOK (which) and
   BKUP_STATUS (status, or unset when NULL) in its environment. Its stdout and
   stderr are forwarded line by line to the log. Returns the command's exit
   status, 128+signal if it was killed, or -1 if it could not be started.
   Never die()s. A NULL or empty cmd is a no-op returning 0. */
int hook_run(const char *cmd, const char *user, const char *which,
             const char *status);

/* Bracket a backup with the user's pre-backup / post-backup hooks.
   hook_backup_begin runs pre-backup and die()s if it fails; from then until
   hook_backup_end, the post-backup hook is guaranteed to run exactly once --
   on hook_backup_end (status "ok") or on any die() in between (status
   "failed", before the thread's own die handler gets control). */
void hook_backup_begin(const User *u);
void hook_backup_end(void);

/* Daemon shutdown: if a backup is armed on some other thread, run its
   post-backup hook now (status "failed") and disarm it, so that stopping the
   daemon mid-backup still releases what the pre hook set up. That thread's
   end/die path then finds nothing armed and does not run it again. The
   backup thread is not stopped -- the process is about to exit -- so it may
   still be scanning while post runs; its OPEN snapshot is swept next run. */
void hook_shutdown(void);

#endif
