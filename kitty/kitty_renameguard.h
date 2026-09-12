/*
 * kitty_renameguard.h - a shipped program refuses to start under a foreign
 * file name.
 *
 * Each executable has a product name, and its file name must BEGIN with that
 * name (case-insensitively). The PuTTY name of the same program is accepted
 * alongside it - kitty/putty, klink/plink, kscp/pscp, ksftp/psftp,
 * kageant/pageant, kittygen/puttygen - for two reasons that both have to
 * hold: packaging renames the build outputs (plink.exe becomes klink.exe),
 * and a tool configured for putty.exe or plink.exe is routinely pointed at a
 * renamed KiTTY.
 *
 * Why the check exists: a signed tool that circulates under throwaway file
 * names is what living-off-the-land abuse looks like, and name-based
 * detection heuristics score it accordingly. Keeping the name recognisable
 * costs a legitimate user nothing - the prefix rule still passes a browser's
 * duplicate (`kitty (1).exe`), a version-named copy (`kitty-0.85.exe`) and
 * the portable build - while the random-name use becomes impossible.
 *
 * It is a nuisance bar, not a security boundary: an attacker who keeps the
 * name is unaffected, and the check can be patched out at the cost of the
 * signature. Nothing in the suite keys a MODE on the file name (PuTTY mode is
 * the -putty switch), so no feature depends on a rename.
 *
 * Windows-only. Uses nothing newer than Windows XP, and it is compiled into
 * the `utils` library, which every binary links.
 */
#ifndef KITTY_RENAMEGUARD_H
#define KITTY_RENAMEGUARD_H

/*
 * Check this process's own file name against `nprefixes` accepted prefixes.
 *
 * prefixes[0] is the PRODUCT name and is the one the message quotes; the rest
 * are the alternative names the same binary legitimately ships under.
 *
 * Returns 0 when the name is accepted - and also when the name cannot be
 * determined at all, so a failure to read it never keeps a legitimate copy
 * from starting.
 *
 * On a mismatch it reports once and returns nonzero. `gui` says the caller has
 * no console to print to, not that a box will certainly appear: a windowed
 * program gets the message box only on an interactive desktop, and a
 * scheduled task or service - where a modal would simply hang - gets one
 * Application event-log line instead. A console caller keeps stderr and gets
 * the log line too, because an unattended job has nobody reading its output.
 *
 * The caller must then end the process immediately: ExitProcess(1) from a
 * WinMain, exit status 1 from a main(). Call it FIRST, before any window, ini
 * or registry access.
 */
int kitty_rename_guard(const char *const *prefixes, int nprefixes, int gui);

/*
 * The release-only companion: does this binary still carry OUR Authenticode
 * signature?
 *
 * Compiled in ONLY under KITTY_RELEASE_SIGNED, which the release build scripts
 * set and nothing else does - an unsigned dev or test build would otherwise
 * refuse to start itself. Without that define the body is not there at all and
 * this returns 0.
 *
 * It answers on the OUTCOME of the verification, never on the Windows version:
 * compatibility mode lies about the version, and "can this machine judge an
 * Authenticode signature" is a question you answer by trying.
 *
 * Refuses on exactly three outcomes: the file was modified after signing, the
 * signature was stripped (the PE's certificate table is gone, not merely a
 * signature this Windows could not read), or the verification SUCCEEDED and
 * the signer is not our publisher. Everything else means "cannot judge" and
 * runs: an algorithm this Windows does not know, a root it does not have, a
 * signature it cannot parse, a distrust decision taken on this machine, no
 * revocation data, wintrust.dll not loadable at all. Windows XP and an offline
 * Windows 7 land there and start normally.
 *
 * Runs once per process; later calls return the first answer. Same contract as
 * kitty_rename_guard(): nonzero means the caller must end the process at once.
 * Call it directly after the name guard.
 */
int kitty_signature_guard(int gui);

#endif /* KITTY_RENAMEGUARD_H */
