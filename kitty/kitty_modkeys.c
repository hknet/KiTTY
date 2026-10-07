/*
 * kitty_modkeys.c - xterm modifyOtherKeys: which modified ordinary keys go
 * out as CSI 27 ; mod ; code ~ and with what code. See kitty_modkeys.h.
 *
 * Transcribed from xterm's input.c. Level 2 (the switch under "case 2" in
 * ModifyOtherKeys): Enter, Tab and Escape with any modifier; Backspace with
 * anything but Ctrl alone; a key whose character is in the "control input"
 * range 0x40..0x7f (letters and @ [ \ ] ^ _ ` { | } ~) with any modifier,
 * Shift alone included; any other character with Ctrl or Alt, and Space
 * with Shift alone. Level 1 ("mokUser", moderated by allowedCharModifiers):
 * Enter with any modifier, Tab with any but Shift alone; Escape only with
 * Alt; Backspace never; a key Ctrl turns into a control character (the
 * control-input range, Space, 2 to 8 and /) only with Alt as well; any other
 * character with Ctrl, or with Alt - Shift alone is the character, and
 * without Ctrl it drops out of the modifiers. Two choices this transcription
 * makes: Shift+Tab at level 2 is encoded as xterm's ctlseqs documents
 * (CSI 27;2;9~) where the source keeps CSI Z, and Alt is always a modifier
 * here, as the window always sends it as ESC.
 */
#include <stdio.h>

#include "kitty_modkeys.h"

#define IN_CONTROL_INPUT(ch) ((ch) >= 0x40 && (ch) <= 0x7f)

/* A key whose character Ctrl turns into a control character: the X library's
 * table (what xterm calls a control alias), the control-input range aside. */
static bool ctrl_alias(unsigned ch)
{
    return ch == ' ' || (ch >= '2' && ch <= '8') || ch == '/';
}

/* Decides, and leaves the modifiers to report in *mods and the code in *code. */
static bool wanted(int level, unsigned *mods, int key, unsigned ch,
                   bool bksp_is_delete, unsigned *code)
{
    unsigned m = *mods;

    switch (key) {
      case KMK_KEY_ENTER:
        *code = 13;
        return true;
      case KMK_KEY_TAB:
        *code = 9;
        return level >= 2 || m != KMK_SHIFT;   /* Shift+Tab alone: CSI Z */
      case KMK_KEY_ESCAPE:
        *code = 27;
        return level >= 2 || (m & KMK_ALT);
      case KMK_KEY_BACKSPACE:
        /* xterm: the key's own byte is always BS here. With DECBKM set
         * (Backspace sends BS) Ctrl flips the key to Delete and drops out of
         * the modifiers; without it Ctrl stays a modifier. Either way Ctrl
         * alone keeps today's bytes. */
        *code = 8;
        if (level < 2 || m == KMK_CTRL)
            return false;
        if (!bksp_is_delete)
            m &= ~KMK_CTRL;
        *mods = m;
        return m != 0;
      case KMK_KEY_CHAR:
        break;
      default:
        return false;
    }

    if (ch < 0x20 || ch == 0x7f)
        return false;                  /* not a character key */
    *code = ch;

    if (level >= 2) {
        if (IN_CONTROL_INPUT(ch))
            return true;
        if (m == KMK_SHIFT)
            return ch == ' ';
        return true;                   /* Ctrl or Alt present */
    }

    /* level 1 */
    if (!(m & KMK_CTRL)) {
        m &= ~KMK_SHIFT;               /* the character already carries Shift */
        *mods = m;
        return m != 0;                 /* Alt */
    }
    if (IN_CONTROL_INPUT(ch) || ctrl_alias(ch))
        return (m & KMK_ALT) != 0;     /* Ctrl's control character otherwise */
    return true;
}

size_t kitty_modkeys_encode(int level, unsigned mods, int key, unsigned ch,
                            bool bksp_is_delete, char *out, size_t outsz)
{
    unsigned code = 0;
    int n;

    mods &= KMK_SHIFT | KMK_ALT | KMK_CTRL;
    if (level <= 0 || !mods || !out || !outsz)
        return 0;
    if (!wanted(level, &mods, key, ch, bksp_is_delete, &code))
        return 0;
    n = snprintf(out, outsz, "\033[27;%u;%u~", 1 + mods, code);
    if (n <= 0 || (size_t)n >= outsz)
        return 0;
    return (size_t)n;
}
