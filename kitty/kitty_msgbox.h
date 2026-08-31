/*
 * KiTTY: route a file's plain MessageBox calls through the themed boxes.
 *
 * Included at the TOP OF SELECTED .c FILES, never from a shared header: the
 * wrapper lives in kitty_win.c, so only files linked into binaries that carry
 * it (kitty / kitty_portable, plus the stock variants via the stub in
 * kitty_config_stubs.c) may use this. The wrapper maps what it can - MB_OK to
 * the info box, MB_YESNO to the confirm box, keeping each site's default
 * button - and hands everything else (three-way choices, system-modal) to the
 * real MessageBox, so no call site changes meaning. MessageBoxW is left
 * alone: the wrapper is ANSI.
 */
#ifndef KITTY_MSGBOX_H
#define KITTY_MSGBOX_H

#include <windows.h>

int kitty_message_box(HWND owner, const char *text, const char *caption,
                      unsigned type);

#undef MessageBox
#undef MessageBoxA
#define MessageBox  kitty_message_box
#define MessageBoxA kitty_message_box

#endif
