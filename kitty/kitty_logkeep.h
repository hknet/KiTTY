/*
 * kitty_logkeep.h - per-session log retention (cyd01/KiTTY#439).
 *
 * Session > Logging > "Delete logs after [N] days" and "Keep logsize below
 * [N] MB": a session deletes its OWN old log files, the way journald trims its
 * journal by age (MaxRetentionSec) and by total size (SystemMaxUse).
 *
 * Called from logging.c only, twice per session: before the first log file
 * opens and after the last one closes. Never on a timer. Windows only, and
 * nothing newer than Windows XP.
 */
#ifndef KITTY_LOGKEEP_H
#define KITTY_LOGKEEP_H

#include "defs.h"

/*
 * Apply the retention rules to this session's log files.
 *
 * `configured` is the log file name as set (with its &-codes and %VAR%s),
 * `active` the name the session writes or is about to write, codes already
 * substituted - it is never deleted, and the folder searched is its folder.
 * `keep_days` / `keep_mb` are the two settings; 0 or less is off.
 *
 * Deletes nothing unless the name part of `configured` has a time code
 * (&Y &M &D &T): without one the session only ever writes one file, and a
 * pattern with &H or &P but no date would match other sessions' logs.
 *
 * Returns NULL when nothing was deleted. Otherwise it has already written one
 * line to the Application event log, and returns the same line (caller frees
 * it with sfree) for the session's own Event Log.
 */
char *kitty_logkeep_run(const Filename *configured, const Filename *active,
                        int keep_days, int keep_mb);

/*
 * Does the name part of a configured log file name hold a time code (&Y &M
 * &D &T)? Rotation and retention both do nothing without one; the Logging
 * panel greys their fields when it returns false.
 */
bool kitty_logkeep_has_time_code(const Filename *configured);

#endif /* KITTY_LOGKEEP_H */
