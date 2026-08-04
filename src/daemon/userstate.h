#ifndef BK_USERSTATE_H
#define BK_USERSTATE_H

/* Tracks users disabled by a permanent (non-retryable) failure -- a wrong
   passphrase, repo-id mismatch, or corrupt/missing KDF metadata. The scheduler
   and watch consult this before doing automatic (unattended) work, so a
   hopeless op is attempted once and then skipped, instead of being re-driven
   every 60s tick forever (the "[F] wrong passphrase" log spam this guards
   against). Interactive GUI ops are NOT gated -- the user may be fixing the
   cause -- and a successful op re-enables the user. A SIGHUP reload clears the
   whole registry, so an operator who fixed the key_file can re-enable without a
   full restart. */

/* Latch `user` off. Logs the reason once; repeat calls are quiet. */
void userstate_disable(const char *user, const char *reason);

/* 1 if `user` is currently disabled for automatic work. */
int  userstate_disabled(const char *user);

/* Re-enable `user` (a successful op cleared the condition). Quiet no-op if the
   user was not disabled; logs once when it actually flips state. */
void userstate_enable(const char *user);

/* Re-enable every user (SIGHUP reload / operator fixed config). */
void userstate_reset(void);

#endif
