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
/*
 * The Authenticode reading of ANY file, as a value - the decision the guard
 * above makes about its own file, for the Applications leaf to make about
 * every file that travels with the install. Compiled in every build (the
 * guard's refusal is release-only, the reading is not). `signer` receives
 * the certificate's subject CN when a valid chain was found (ours or not).
 */
enum {
    KG_SIG_OURS = 0,      /* valid chain, our publisher */
    KG_SIG_MODIFIED,      /* bad digest on a Windows that can compute it */
    KG_SIG_UNSIGNED,      /* no signature and no certificate table */
    KG_SIG_OTHER,         /* valid chain, another publisher (see `signer`) */
    KG_SIG_CANNOT         /* this Windows cannot judge; no verdict */
};
int kitty_signature_reading(const char *path, char *signer, size_t signersz);
/* Does the PE at `path` carry a certificate table? 1 / 0 / -1 = unreadable. */
int kitty_file_has_cert_table(const char *path);

/*
 * The shared pieces of the guards, for the third one (kitty_selfcheck.c),
 * which needs `crypto` and therefore cannot compile into `utils`.
 *
 * kitty_guard_interactive(): nonzero when the process's window station is
 * visible - somebody could see a box. Unknown counts as interactive.
 *
 * kitty_guard_report(): the one report, as the guards above make it. With
 * `allow_box` zero a windowed program gets the event-log line only - for a
 * refusal that is made precisely because nobody is there to click.
 */
int kitty_guard_interactive(void);
void kitty_guard_report(const char *msg, int gui, int allow_box);

#endif /* KITTY_RENAMEGUARD_H */
