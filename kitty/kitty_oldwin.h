/*
 * kitty_oldwin.h: the handful of Windows APIs KiTTY uses that are newer than
 * the oldest Windows it is meant to load on, resolved at RUNTIME.
 *
 * A statically imported symbol the running Windows does not export makes the
 * whole binary refuse to start - the loader fails it before main(), with a
 * message naming an entry point rather than anything a user can act on. So
 * these are looked up with GetProcAddress and each has a fallback that works
 * on the older system.
 *
 * This is what lets ONE binary run everywhere instead of a separate legacy
 * build: the features that genuinely need a modern Windows (Windows Hello,
 * dark mode, per-monitor DPI, Restart Manager) already load their libraries
 * the same way, so they simply stay inactive rather than blocking startup.
 *
 * Windows-only; include after putty.h / windows.h.
 */
#ifndef KITTY_OLDWIN_H
#define KITTY_OLDWIN_H

/*
 * ---- resolving an API that may not be there ----
 *
 * Every runtime lookup in KiTTY should go through kitty_api(), so that a
 * Windows too old to provide something is answered with a sentence naming
 * WHAT is missing and WHAT it was for, rather than with a silently dead
 * feature or a crash.
 *
 *   KITTY_API_OPTIONAL  the caller has a fallback, or the feature simply
 *                       stays off. Recorded, reported in the Event Log, and
 *                       never shown to the user as an error.
 *   KITTY_API_REQUIRED  KiTTY cannot run without it. Recorded; no lookup
 *                       asks for this level today.
 *
 * `feature` is the words the user reads: "Windows Hello", "dark mode", "the
 * agent-verification check". Keep it a NOUN PHRASE - it is printed as
 * "<feature> needs <symbol> from <dll>".
 */
#define KITTY_API_OPTIONAL 0
#define KITTY_API_REQUIRED 1

FARPROC kitty_api(const char *dll, const char *symbol, int need,
                  const char *feature);

/*
 * The same, for a caller that already holds the module handle - which most of
 * KiTTY's lookups do, because they load a DLL once and resolve several
 * symbols from it, and some of them free it again afterwards. Using this
 * rather than kitty_api() keeps that lifetime exactly as it was: this only
 * resolves and records, it never loads or frees anything.
 *
 * `dll` is then just the name to PRINT in the report.
 */
FARPROC kitty_api_from(HMODULE module, const char *dll, const char *symbol,
                       int need, const char *feature);

/*
 * Record a lookup this file did not perform. For the cases the two above
 * cannot express - an ORDINAL rather than a name (uxtheme's dark-mode entry
 * points are exported by number only), or a symbol chosen at runtime. Pass
 * the name you want in the report and whether it was found.
 */
void kitty_api_record(const char *dll, const char *symbol, int need,
                      const char *feature, int found);

/*
 * What was missing, as text the caller owns (sfree it), or NULL when nothing
 * was.
 *
 * _degraded() is the list of features that switched themselves off. It
 * belongs in the Event Log, not in a box.
 *
 * It only knows about lookups that have actually happened, so ask AFTER the
 * startup path has resolved what it needs.
 */
char *kitty_oldwin_degraded(void);

/*
 * The same news in one line: the FEATURE names, comma separated, each once -
 * "dark mode, Windows Hello". For the notice in the terminal, which has room
 * for what is missing but not for which entry point of which DLL. NULL when
 * nothing is missing; caller frees.
 */
char *kitty_oldwin_degraded_brief(void);

/*
 * Milliseconds since boot, as a 64-bit count that does not wrap.
 *
 * GetTickCount64 is Vista and later. Where it is missing this keeps its own
 * high word, advancing it when the 32-bit GetTickCount wraps (every 49.7
 * days), so callers comparing two readings get the same answer on either
 * system. Both callers are timers - "expire this in N minutes" - which is a
 * DELTA between readings, and that is what stays correct across the wrap.
 */
ULONGLONG kitty_tick_count64(void);

/*
 * The full path of the executable behind a process handle.
 *
 * QueryFullProcessImageNameA is Vista and later; the fallback is
 * GetModuleFileNameExA out of psapi.dll, which goes back to NT 4. Returns
 * FALSE with *buf untouched when neither is available, so a caller that is
 * identifying a program - the agent check, the workplace-proxy owner test -
 * treats it as "cannot tell" instead of as a match.
 *
 * bufsize is in bytes, as with the API it replaces.
 */
BOOL kitty_process_image_path(HANDLE proc, char *buf, DWORD bufsize);

/*
 * Attach to the console of the process that started us, so a GUI binary can
 * print to the shell that ran it. XP and later; FALSE on anything older, and
 * on a process that has no parent console. Nothing depends on it - the caller
 * simply has nowhere to print.
 */
BOOL kitty_attach_parent_console(void);

#endif /* KITTY_OLDWIN_H */
