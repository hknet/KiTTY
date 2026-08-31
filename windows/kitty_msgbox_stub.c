/*
 * kitty_message_box for binaries that compile windows/controls.c but carry
 * neither kitty_win.c (the real themed boxes) nor kitty_config_stubs.c
 * (which has this same fallback for putty/puttytel/pterm). Currently that is
 * puttygen alone. The call stays a real MessageBox there.
 */
#include <windows.h>

int kitty_message_box(HWND owner, const char *text, const char *caption,
                      unsigned type)
{
    return MessageBoxA(owner, text, caption, type);
}
