/*
 * kitty_oldwin_reg.h - registry (and ConPTY-spawn) APIs newer than the
 * oldest Windows we load on, routed through kitty_oldwin.c.
 *
 * RegDeleteTreeA and RegGetValueA are Vista+; a STATIC import of either
 * kills the whole process in the LOADER on Windows XP ("Entry Point not
 * Found", first XP VM run, 2026-09-01) - the exact failure class
 * kitty_oldwin.c exists for. Included AFTER windows.h in each consumer .c
 * file; never from a shared header, and never from kitty_oldwin.c itself,
 * which needs the real names to resolve and call.
 *
 * The ProcThreadAttributeList pair is conpty.c's: also Vista+, also a
 * loader-killer for pterm. On XP the wrappers are never reached - conpty.c
 * checks CreatePseudoConsole availability first - so their fallback only
 * has to exist, not to work.
 */
#ifndef KITTY_OLDWIN_REG_H
#define KITTY_OLDWIN_REG_H

#include <windows.h>

LSTATUS kitty_oldwin_RegDeleteTreeA(HKEY key, LPCSTR subkey);
LSTATUS kitty_oldwin_RegGetValueA(HKEY key, LPCSTR subkey, LPCSTR value,
                                  DWORD flags, LPDWORD ptype, PVOID data,
                                  LPDWORD psize);
BOOL kitty_oldwin_InitializeProcThreadAttributeList(
    LPPROC_THREAD_ATTRIBUTE_LIST list, DWORD count, DWORD flags,
    PSIZE_T size);
BOOL kitty_oldwin_GetNamedPipeServerProcessId(HANDLE pipe, PULONG pid);
HRESULT kitty_oldwin_RegisterApplicationRestart(PCWSTR cmdline, DWORD flags);
BOOL kitty_oldwin_UpdateProcThreadAttribute(
    LPPROC_THREAD_ATTRIBUTE_LIST list, DWORD flags, DWORD_PTR attr,
    PVOID value, SIZE_T cb, PVOID prev, PSIZE_T rsize);

#undef RegDeleteTreeA
#undef RegGetValueA
#define RegDeleteTreeA kitty_oldwin_RegDeleteTreeA
#define RegGetValueA   kitty_oldwin_RegGetValueA
#define InitializeProcThreadAttributeList \
        kitty_oldwin_InitializeProcThreadAttributeList
#define UpdateProcThreadAttribute kitty_oldwin_UpdateProcThreadAttribute
#define GetNamedPipeServerProcessId kitty_oldwin_GetNamedPipeServerProcessId
#define RegisterApplicationRestart kitty_oldwin_RegisterApplicationRestart

#endif
