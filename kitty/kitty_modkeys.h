/*
 * kitty_modkeys.h - xterm modifyOtherKeys (CSI > 4 ; level m): the escape
 * sequence a modified ordinary key goes out as, CSI 27 ; mod ; code ~, at
 * levels 1 and 2. The rules are xterm's (input.c, ModifyOtherKeys and
 * allowedCharModifiers); the platform half - which key was pressed and what
 * character it produces - is the window's. No Win32 and no PuTTY types, so
 * test/test_modkeys.c covers it without a window.
 */
#ifndef KITTY_MODKEYS_H
#define KITTY_MODKEYS_H

#include <stdbool.h>
#include <stddef.h>

/* Modifier bits, in xterm's parameter order: mod = 1 + the sum. */
#define KMK_SHIFT 1u
#define KMK_ALT   2u
#define KMK_CTRL  4u

/* Which key: an ordinary character key, or one of the four xterm treats by
 * name. The cursor, editing, keypad and function keys never come here; they
 * carry their modifiers in their own sequences. */
enum {
    KMK_KEY_CHAR = 0,
    KMK_KEY_ENTER,
    KMK_KEY_TAB,
    KMK_KEY_BACKSPACE,
    KMK_KEY_ESCAPE
};

/* The sequence for a key pressed with mods (KMK_* bits) at modifyOtherKeys
 * level `level`. ch: for KMK_KEY_CHAR, the Unicode code point the key yields
 * with Shift and Caps Lock applied and Ctrl/Alt ignored (xterm's keysym).
 * bksp_is_delete: Backspace sends DEL rather than BS (the DECBKM-off case).
 * Returns the bytes written to out, or 0 when the key keeps its ordinary
 * encoding - always 0 at level 0 and with no modifier. */
size_t kitty_modkeys_encode(int level, unsigned mods, int key, unsigned ch,
                            bool bksp_is_delete, char *out, size_t outsz);

#endif /* KITTY_MODKEYS_H */
