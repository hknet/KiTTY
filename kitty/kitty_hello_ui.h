/*
 * kitty_hello_ui.h - the Hello-protected keys UI every app shares: the
 * once-shown printout window (passphrase or recovery code) and the door
 * wording. Built with in-memory dialog templates so no app has to carry
 * resource copies; kageant, kittygen and (later) kitty.exe all use the
 * same window and the same words.
 */
#ifndef KITTY_HELLO_UI_H
#define KITTY_HELLO_UI_H

#include <windows.h>

/* The one line every typed prompt shows for a protected key. */
#define KITTY_HELLO_DOOR_PROMPT \
    "Enter the recovery passphrase, the recovery code, or the printed " \
    "secret:"

/* Show the once-shown printout: the protected key's literal passphrase
 * (sidebound == 0) or its sidecar-bound recovery code (sidebound != 0).
 * Copy-to-clipboard button; X/Esc ask "stored it?" before giving up the
 * only display there will ever be. appname appears in the text
 * ("kageant"/"KiTTYgen"). Blocks until dismissed. */
void kitty_hello_ui_show_printout(HWND owner, const char *appname,
                                  const char *text, int sidebound);

#endif /* KITTY_HELLO_UI_H */
