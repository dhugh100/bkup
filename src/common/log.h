#ifndef BK_LOG_H
#define BK_LOG_H

void log_info(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_err(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void die(const char *fmt, ...)
    __attribute__((format(printf, 1, 2), noreturn));

/* Like die(), but flags the failure as permanent (non-retryable): a wrong
   passphrase, repo-id mismatch, corrupt/missing KDF metadata, or a bad
   key_file -- conditions that fail identically on every retry. The daemon uses
   this (via log_take_permanent below) to stop re-driving a hopeless op every
   tick instead of spinning forever. */
void die_perm(const char *fmt, ...)
    __attribute__((format(printf, 1, 2), noreturn));

/* After a die()/die_perm() has unwound the thread via a registered die handler
   (longjmp), tells the catcher whether the failure was permanent. Reads and
   clears the per-thread flag. Returns 1 for die_perm(), 0 for die(). */
int log_take_permanent(void);

/* Durable event log.  Once opened, every log_info/warn/err/die line is also
   appended (with a timestamp) to this file, independent of any per-thread cb,
   so CLI, daemon (GUI-driven), and scheduled events all land in one place.
   Returns 0 on success, -1 on failure with errno set (caller must treat this
   as fatal: a backup with no durable record is the failure we are guarding
   against, so we never run blind). */
int  log_open_file(const char *path);
void log_close_file(void);

/* Per-thread log callback.  If set, log_info/warn/err/die forward to cb
   instead of stderr.  lvl is "I", "W", "E", or "F". */
void log_set_thread_cb(void (*cb)(const char *lvl, const char *msg, void *ud),
                       void *ud);

/* Per-thread die handler.  If set, called instead of exit(1) after the
   fatal message is delivered.  Must not return (use pthread_exit, etc.). */
void log_set_thread_die(void (*fn)(void));

#endif
