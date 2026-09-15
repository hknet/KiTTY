/*
 * kitty_selfcheck.h - a release file checks its own integrity at startup,
 * offline, without Windows crypto: an Ed25519-signed SHA-256 stamp in the
 * file's own `.ktstamp` section (layout and hash rule: kitty_selfcheck_core.h).
 *
 * WHY, when there is Authenticode already: on Windows XP the signature guard
 * (kitty_renameguard.c) cannot judge our SHA-256 signature and lets an edited
 * file start, and XP cannot reach GitHub for any online check either. This
 * check uses PuTTY's own SHA-256 and Ed25519 code, which run everywhere the
 * program does. It is not a security boundary - whoever patches the check
 * out gets past it - it makes every release a fresh piece of patch work and
 * gives the unwitting user a refusal and an event-log line.
 *
 * THE KEY is a fresh Ed25519 pair per release: the public key is compiled
 * into that release only (a generated header outside the repository), the
 * private key exists during the release run and is destroyed after the stamp
 * step. Older releases verify with the key they carry.
 *
 * COMPILED IN ONLY UNDER KITTY_SELFCHECK, which the 32-bit release build sets
 * and the local stamped test build; a dev or test
 * build has no body here and this returns 0. The stamp is written after
 * packaging and before Authenticode signing.
 */
#ifndef KITTY_SELFCHECK_H
#define KITTY_SELFCHECK_H

/*
 * Verify this process's own file against its stamp.
 *
 * Same contract as kitty_rename_guard() and kitty_signature_guard(): call it
 * directly after those, once, before any window, ini or registry access;
 * nonzero means the caller must end the process at once. Runs once per
 * process; later calls return the first answer.
 *
 * Refuses, with one report through the same path as the other guards (a
 * message box for a windowed program on an interactive desktop, otherwise
 * one line on stderr for a console program, and always one Application
 * event-log line), on:
 *   modified      the recomputed SHA-256 differs from the stamp;
 *   bad stamp     the stamp's signature does not verify against our key;
 *   no stamp      no stamp section, or an unfilled or unknown-version stamp;
 *   truncated     the file is shorter than the stamped length;
 *   cannot check  the check could not run AND the program is not visible.
 *
 * "Cannot check" - the own file not openable or readable, headers that do
 * not parse - RUNS when the program is visible: a windowed program (`gui`
 * nonzero) counts as visible when its window station is visible and it was
 * not started hidden (STARTF_USESHOWWINDOW with SW_HIDE); a console program
 * counts as visible on a visible window station alone, because KiTTY itself
 * starts klink and kscp without a window. Invisible plus cannot check is
 * refused, reason `cannot check`, event log only - never a box.
 *
 * KITTY_SELFCHECK_FAULT (test builds only, never a release): when the
 * environment variable KITTY_SELFCHECK_FAULT is set, the check behaves as if
 * the own file could not be opened, so the cannot-check paths can be driven.
 * Such a build carries KT_SELFCHECK_FAULT_MARKER and the stamp tool refuses
 * to stamp it.
 */
int kitty_selfcheck_guard(int gui);

#endif /* KITTY_SELFCHECK_H */
