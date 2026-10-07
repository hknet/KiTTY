/*
 * test_modkeys - regression tests for the xterm modifyOtherKeys encoding in
 * kitty/kitty_modkeys.c: which modified keys become CSI 27 ; mod ; code ~
 * at levels 1 and 2, and that level 0 and unmodified keys never do.
 *
 * Returns non-zero on any failure.
 */
#include <stdio.h>
#include <string.h>

#include "../kitty/kitty_modkeys.h"

static int failures = 0;

/* want = NULL: the key must keep its ordinary encoding. */
static void check(const char *what, int level, unsigned mods, int key,
                  unsigned ch, bool bksp_del, const char *want)
{
    char out[32];
    size_t n = kitty_modkeys_encode(level, mods, key, ch, bksp_del,
                                    out, sizeof(out));
    if (!want) {
        if (n != 0) {
            printf("FAIL [%s]: encoded as %s, expected nothing\n",
                   what, out + 1);
            failures++;
        }
        return;
    }
    if (n != strlen(want) || memcmp(out, want, n) != 0) {
        printf("FAIL [%s]: got %s (%u), want %s\n", what,
               n ? out + 1 : "(nothing)", (unsigned)n, want + 1);
        failures++;
    }
}

#define S KMK_SHIFT
#define A KMK_ALT
#define C KMK_CTRL

int main(void)
{
    /* --- level 0: nothing, ever --- */
    check("L0 Shift+Enter", 0, S, KMK_KEY_ENTER, 0, true, NULL);
    check("L0 Ctrl+a", 0, C, KMK_KEY_CHAR, 'a', true, NULL);
    check("L0 Ctrl+Alt+]", 0, C|A, KMK_KEY_CHAR, ']', true, NULL);
    check("L0 Alt+Escape", 0, A, KMK_KEY_ESCAPE, 0, true, NULL);

    /* --- no modifier: nothing at either level --- */
    check("L2 plain a", 2, 0, KMK_KEY_CHAR, 'a', true, NULL);
    check("L2 plain Enter", 2, 0, KMK_KEY_ENTER, 0, true, NULL);
    check("L1 plain Tab", 1, 0, KMK_KEY_TAB, 0, true, NULL);

    /* --- level 2: the requesting application's keys --- */
    check("L2 Shift+Enter", 2, S, KMK_KEY_ENTER, 0, true, "\033[27;2;13~");
    check("L2 Alt+Enter", 2, A, KMK_KEY_ENTER, 0, true, "\033[27;3;13~");
    check("L2 Ctrl+Enter", 2, C, KMK_KEY_ENTER, 0, true, "\033[27;5;13~");
    check("L2 Ctrl+Shift+Enter", 2, C|S, KMK_KEY_ENTER, 0, true,
          "\033[27;6;13~");
    check("L2 Ctrl+-", 2, C, KMK_KEY_CHAR, '-', true, "\033[27;5;45~");
    check("L2 Ctrl+a", 2, C, KMK_KEY_CHAR, 'a', true, "\033[27;5;97~");
    check("L2 Ctrl+Alt+]", 2, C|A, KMK_KEY_CHAR, ']', true, "\033[27;7;93~");
    check("L2 Alt+a", 2, A, KMK_KEY_CHAR, 'a', true, "\033[27;3;97~");
    check("L2 Shift+Tab", 2, S, KMK_KEY_TAB, 0, true, "\033[27;2;9~");
    check("L2 Ctrl+Tab", 2, C, KMK_KEY_TAB, 0, true, "\033[27;5;9~");
    check("L2 Ctrl+Shift+Tab", 2, C|S, KMK_KEY_TAB, 0, true, "\033[27;6;9~");
    check("L2 Alt+Tab", 2, A, KMK_KEY_TAB, 0, true, "\033[27;3;9~");
    check("L2 Shift+Escape", 2, S, KMK_KEY_ESCAPE, 0, true, "\033[27;2;27~");
    check("L2 Alt+Escape", 2, A, KMK_KEY_ESCAPE, 0, true, "\033[27;3;27~");
    check("L2 Ctrl+[", 2, C, KMK_KEY_CHAR, '[', true, "\033[27;5;91~");
    check("L2 Ctrl+Space", 2, C, KMK_KEY_CHAR, ' ', true, "\033[27;5;32~");
    check("L2 Shift+Space", 2, S, KMK_KEY_CHAR, ' ', true, "\033[27;2;32~");
    check("L2 Ctrl+2", 2, C, KMK_KEY_CHAR, '2', true, "\033[27;5;50~");
    check("L2 Ctrl+Shift+2 (@)", 2, C|S, KMK_KEY_CHAR, '@', true,
          "\033[27;6;64~");
    check("L2 Alt+1", 2, A, KMK_KEY_CHAR, '1', true, "\033[27;3;49~");
    check("L2 Alt+Shift+1 (!)", 2, A|S, KMK_KEY_CHAR, '!', true,
          "\033[27;4;33~");
    check("L2 Ctrl+Shift+Alt+a", 2, C|S|A, KMK_KEY_CHAR, 'A', true,
          "\033[27;8;65~");
    check("L2 Alt+umlaut", 2, A, KMK_KEY_CHAR, 0xe4, true, "\033[27;3;228~");

    /* Shift alone: a letter (control-input range) is encoded, any other
     * printable key stays the character it produces. */
    check("L2 Shift+a", 2, S, KMK_KEY_CHAR, 'A', true, "\033[27;2;65~");
    check("L2 Shift+1 (!)", 2, S, KMK_KEY_CHAR, '!', true, NULL);
    check("L2 Shift+- (_)", 2, S, KMK_KEY_CHAR, '_', true, "\033[27;2;95~");
    check("L2 Shift+umlaut", 2, S, KMK_KEY_CHAR, 0xc4, true, NULL);

    /* Backspace: Ctrl alone never; the code is always BS; with Backspace
     * sending BS (DECBKM set) Ctrl drops out of the modifiers. */
    check("L2 Ctrl+Backspace, DEL", 2, C, KMK_KEY_BACKSPACE, 0, true, NULL);
    check("L2 Ctrl+Backspace, BS", 2, C, KMK_KEY_BACKSPACE, 0, false, NULL);
    check("L2 Shift+Backspace", 2, S, KMK_KEY_BACKSPACE, 0, true,
          "\033[27;2;8~");
    check("L2 Alt+Backspace", 2, A, KMK_KEY_BACKSPACE, 0, true,
          "\033[27;3;8~");
    check("L2 Ctrl+Shift+Backspace, DEL", 2, C|S, KMK_KEY_BACKSPACE, 0, true,
          "\033[27;6;8~");
    check("L2 Ctrl+Shift+Backspace, BS", 2, C|S, KMK_KEY_BACKSPACE, 0, false,
          "\033[27;2;8~");

    /* control characters are not character keys */
    check("L2 Ctrl+NUL", 2, C, KMK_KEY_CHAR, 0, true, NULL);
    check("L2 Ctrl+DEL", 2, C, KMK_KEY_CHAR, 0x7f, true, NULL);

    /* --- level 1: only what has no well-known encoding --- */
    check("L1 Shift+Enter", 1, S, KMK_KEY_ENTER, 0, true, "\033[27;2;13~");
    check("L1 Alt+Enter", 1, A, KMK_KEY_ENTER, 0, true, "\033[27;3;13~");
    check("L1 Alt+Tab", 1, A, KMK_KEY_TAB, 0, true, "\033[27;3;9~");
    check("L1 Shift+Tab stays CSI Z", 1, S, KMK_KEY_TAB, 0, true, NULL);
    check("L1 Ctrl+Shift+Tab", 1, C|S, KMK_KEY_TAB, 0, true, "\033[27;6;9~");
    check("L1 Alt+Escape", 1, A, KMK_KEY_ESCAPE, 0, true, "\033[27;3;27~");
    check("L1 Shift+Escape", 1, S, KMK_KEY_ESCAPE, 0, true, NULL);
    check("L1 Ctrl+Escape", 1, C, KMK_KEY_ESCAPE, 0, true, NULL);
    check("L1 Shift+Backspace", 1, S, KMK_KEY_BACKSPACE, 0, true, NULL);
    check("L1 Alt+Backspace", 1, A, KMK_KEY_BACKSPACE, 0, true, NULL);
    check("L1 Ctrl+a", 1, C, KMK_KEY_CHAR, 'a', true, NULL);
    check("L1 Shift+a", 1, S, KMK_KEY_CHAR, 'A', true, NULL);
    check("L1 Ctrl+Shift+a is still ^A", 1, C|S, KMK_KEY_CHAR, 'A', true, NULL);
    check("L1 Alt+a", 1, A, KMK_KEY_CHAR, 'a', true, "\033[27;3;97~");
    check("L1 Shift+Alt+a, Shift dropped", 1, S|A, KMK_KEY_CHAR, 'A', true,
          "\033[27;3;65~");
    check("L1 Ctrl+Alt+a", 1, C|A, KMK_KEY_CHAR, 'a', true, "\033[27;7;97~");
    check("L1 Ctrl+Shift+Alt+a", 1, C|S|A, KMK_KEY_CHAR, 'A', true,
          "\033[27;8;65~");
    check("L1 Ctrl+Alt+]", 1, C|A, KMK_KEY_CHAR, ']', true, "\033[27;7;93~");
    check("L1 Ctrl+[ is still ESC", 1, C, KMK_KEY_CHAR, '[', true, NULL);
    check("L1 Ctrl+-", 1, C, KMK_KEY_CHAR, '-', true, "\033[27;5;45~");
    check("L1 Ctrl+Shift+- (_) is still ^_", 1, C|S, KMK_KEY_CHAR, '_', true,
          NULL);
    check("L1 Ctrl+Alt+-", 1, C|A, KMK_KEY_CHAR, '-', true, "\033[27;7;45~");
    check("L1 Ctrl+2 is still NUL", 1, C, KMK_KEY_CHAR, '2', true, NULL);
    check("L1 Ctrl+/ is still ^_", 1, C, KMK_KEY_CHAR, '/', true, NULL);
    check("L1 Ctrl+Shift+2 (@) is still NUL", 1, C|S, KMK_KEY_CHAR, '@', true,
          NULL);
    check("L1 Ctrl+Space", 1, C, KMK_KEY_CHAR, ' ', true, NULL);
    check("L1 Ctrl+Shift+Space", 1, C|S, KMK_KEY_CHAR, ' ', true, NULL);
    check("L1 Ctrl+Alt+Space", 1, C|A, KMK_KEY_CHAR, ' ', true,
          "\033[27;7;32~");
    check("L1 Shift+1 (!)", 1, S, KMK_KEY_CHAR, '!', true, NULL);
    check("L1 Alt+Shift+1 (!)", 1, A|S, KMK_KEY_CHAR, '!', true,
          "\033[27;3;33~");
    check("L1 Ctrl+1", 1, C, KMK_KEY_CHAR, '1', true, "\033[27;5;49~");

    /* --- the buffer --- */
    {
        char tiny[8];
        if (kitty_modkeys_encode(2, C, KMK_KEY_CHAR, 'a', true,
                                 tiny, sizeof(tiny)) != 0) {
            printf("FAIL [short buffer]: wrote into a buffer too small\n");
            failures++;
        }
        if (kitty_modkeys_encode(2, C, KMK_KEY_CHAR, 'a', true, NULL, 0)) {
            printf("FAIL [no buffer]\n");
            failures++;
        }
    }

    if (failures)
        printf("test_modkeys: %d failure(s)\n", failures);
    else
        printf("test_modkeys: all passed\n");
    return failures ? 1 : 0;
}
