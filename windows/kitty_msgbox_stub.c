/*
 * kitty_message_box for binaries that compile windows/controls.c but carry
 * neither kitty_dlgbox.c (the real themed boxes) nor kitty_config_stubs.c
 * (which has this same fallback for putty/puttytel/pterm). Currently that is
 * puttygen alone. The call stays a real MessageBox there.
 */
#include <winsock2.h>   /* before windows.h: putty.h pulls it in further down */
#include <windows.h>
#include "kitty_gui.h"

int kitty_message_box(HWND owner, const char *text, const char *caption,
                      unsigned type)
{
    return MessageBoxA(owner, text, caption, type);
}
