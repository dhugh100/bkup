/*
 * prune_internal.h -- testability seam for prune.c selection helpers.
 *
 * Gives tests direct access to period_key() and apply_periodic() without
 * going through cmd_prune (which requires a server connection for blob GC).
 * These are also the highest-stakes logic paths; direct unit testing is the
 * strongest regression guard for the retention-math correctness.
 *
 * Keep cmd_prune's external behavior unchanged; only the static keyword is
 * removed from these two helpers.
 */

#ifndef BK_PRUNE_INTERNAL_H
#define BK_PRUNE_INTERNAL_H

#include <time.h>

/* Retention period kinds for bucketing. */
enum { P_DAILY = 0, P_WEEKLY, P_MONTHLY, P_YEARLY };

/* Compact snapshot descriptor used by the selection logic. */
typedef struct { long long id, created; int kind; } Snap;

/* Return a stable integer key identifying which day / ISO-week / month / year
   the time_t `t` falls in.  Two times share a key iff they belong to the same
   calendar period.  Uses local time (localtime_r), so keys are TZ-independent
   within a single run but should not be compared across TZ changes. */
long long period_key(time_t t, int kind);

/* Mark the newest SCHEDULED snapshot in each of the most recent `count`
   distinct periods (of the given kind) by setting keep[i]=1.  `s` must be
   sorted by created DESC.  Continuous snapshots are skipped (they are handled
   separately by the anchor rule in cmd_prune). */
void apply_periodic(Snap *s, int n, int *keep, int kind, int count);

#endif /* BK_PRUNE_INTERNAL_H */
