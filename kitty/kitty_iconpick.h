/*
 * kitty_iconpick.h - the icon picker of Window > Title & Icon (hknet/KiTTY#62).
 */
#ifndef KITTY_ICONPICK_H
#define KITTY_ICONPICK_H

#include <windows.h>

/* A modal box over owner with every icon the session can choose by number.
 * lib: a library file (.dll, .exe, .icl) to show, or NULL / "" for the
 * built-in set. cur: the number set now (selected). Returns the number
 * chosen, or -1 when cancelled. */
int kitty_icon_pick(HWND owner, const char *lib, int cur);

#endif
