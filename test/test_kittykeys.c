/*
 * test_kittykeys - regression tests for the kitty keyboard protocol encoding
 * in kitty/kitty_kittykeys.c: what each key goes out as at each enhancement
 * flag, which keys stay legacy, and that flags 0 never encodes anything.
 *
 * Returns non-zero on any failure.
 */
#include <stdio.h>
#include <string.h>

#include "../kitty/kitty_kittykeys.h"

static int failures = 0;

static void show(const char *s)
{
    for (; *s; s++) {
        if (*s == '\033') printf("ESC");
        else if ((unsigned char)*s < 0x20) printf("^%c", *s + '@');
        else putchar(*s);
    }
}

/* want: the bytes; "" for KKP_LEGACY; NULL for KKP_NOTHING. */
static void check(const char *what, unsigned flags, unsigned key,
                  unsigned shifted, unsigned base, unsigned text,
                  unsigned mods, int event, const char *want)
{
    KittyKeyEvent ev = { key, shifted, base, text, mods, event };
    char out[KKP_BUFFER_SIZE];
    int n = kitty_kkp_encode(flags, &ev, out, sizeof(out));
    int wantn = !want ? KKP_NOTHING : !*want ? KKP_LEGACY : (int)strlen(want);

    if (n != wantn || (n > 0 && memcmp(out, want, (size_t)n) != 0)) {
        printf("FAIL [%s] flags %u: got ", what, flags);
        if (n > 0) show(out);
        else printf(n == KKP_LEGACY ? "legacy" : "nothing");
        printf(", want ");
        if (wantn > 0) show(want);
        else printf(wantn == KKP_LEGACY ? "legacy" : "nothing");
        printf("\n");
        failures++;
    }
}

#define S KKP_SHIFT
#define A KKP_ALT
#define C KKP_CTRL
#define W KKP_SUPER
#define CAPS KKP_CAPS
#define NUM KKP_NUM
#define P KKP_PRESS
#define R KKP_REPEAT
#define U KKP_RELEASE

/* A character key: key, shifted, base and the text it types. */
#define CH(what, flags, key, shifted, base, text, mods, ev, want) \
    check(what, flags, key, shifted, base, text, mods, ev, want)
/* A functional key: no text. */
#define FN(what, flags, key, mods, ev, want) \
    check(what, flags, key, 0, 0, 0, mods, ev, want)

int main(void)
{
    const unsigned D = KKP_DISAMBIGUATE, E = KKP_EVENT_TYPES,
                   AK = KKP_ALTERNATE_KEYS, ALL = KKP_ALL_KEYS, T = KKP_TEXT;

    /* --- flags 0: never anything --- */
    CH("0 a", 0, 'a', 'A', 'a', 'a', 0, P, "");
    CH("0 Ctrl+a", 0, 'a', 'A', 'a', 0, C, P, "");
    FN("0 Escape", 0, KKP_KEY_ESCAPE, 0, P, "");
    FN("0 Up", 0, KKP_KEY_UP, 0, P, "");
    FN("0 F5", 0, KKP_KEY_F1 + 4, S, P, "");
    FN("0 release", 0, KKP_KEY_UP, 0, U, "");
    FN("0 Left Shift", 0, KKP_KEY_LEFT_SHIFT, S, P, "");
    CH("0 flags beyond 31 ignored", 32, 'a', 'A', 'a', 0, C, P, "");

    /* --- flag 1: disambiguate --- */
    CH("a", D, 'a', 'A', 'a', 'a', 0, P, "");
    CH("Shift+a", D, 'a', 'A', 'a', 'A', S, P, "");
    CH("Caps a", D, 'a', 'A', 'a', 'A', CAPS, P, "");
    CH("Ctrl+a", D, 'a', 'A', 'a', 0, C, P, "\033[97;5u");
    CH("Ctrl+Shift+a", D, 'a', 'A', 'a', 0, C|S, P, "\033[97;6u");
    CH("Alt+a", D, 'a', 'A', 'a', 0, A, P, "\033[97;3u");
    CH("Ctrl+Alt+a", D, 'a', 'A', 'a', 0, C|A, P, "\033[97;7u");
    CH("Shift+Alt+a", D, 'a', 'A', 'a', 0, S|A, P, "\033[97;4u");
    CH("Super+a", D, 'a', 'A', 'a', 0, W, P, "\033[97;9u");
    CH("Ctrl+Caps a", D, 'a', 'A', 'a', 0, C|CAPS, P, "\033[97;69u");
    CH("Ctrl+NumLock a", D, 'a', 'A', 'a', 0, C|NUM, P, "\033[97;133u");
    CH("Ctrl+Space", D, ' ', 0, ' ', 0, C, P, "\033[32;5u");
    CH("Shift+Space", D, ' ', 0, ' ', ' ', S, P, "");
    CH("Ctrl+umlaut", D, 0xe4, 0xc4, '\'', 0, C, P, "\033[228;5u");
    CH("Ctrl+Cyrillic es", D, 0x441, 0x421, 'c', 0, C, P, "\033[1089;5u");
    CH("Ctrl+2", D, '2', '@', '2', 0, C, P, "\033[50;5u");
    CH("Ctrl+[", D, '[', '{', '[', 0, C, P, "\033[91;5u");
    CH("Ctrl+-", D, '-', '_', '-', 0, C, P, "\033[45;5u");
    FN("Escape", D, KKP_KEY_ESCAPE, 0, P, "\033[27u");
    FN("Ctrl+Escape", D, KKP_KEY_ESCAPE, C, P, "\033[27;5u");
    FN("Shift+Escape", D, KKP_KEY_ESCAPE, S, P, "\033[27;2u");
    FN("Enter", D, KKP_KEY_ENTER, 0, P, "");
    FN("Enter, Caps on", D, KKP_KEY_ENTER, CAPS, P, "");
    FN("Shift+Enter", D, KKP_KEY_ENTER, S, P, "\033[13;2u");
    FN("Alt+Enter", D, KKP_KEY_ENTER, A, P, "\033[13;3u");
    FN("Ctrl+Enter", D, KKP_KEY_ENTER, C, P, "\033[13;5u");
    FN("Tab", D, KKP_KEY_TAB, 0, P, "");
    FN("Shift+Tab", D, KKP_KEY_TAB, S, P, "\033[9;2u");
    FN("Ctrl+Shift+Tab", D, KKP_KEY_TAB, C|S, P, "\033[9;6u");
    FN("Backspace", D, KKP_KEY_BACKSPACE, 0, P, "");
    FN("Ctrl+Backspace", D, KKP_KEY_BACKSPACE, C, P, "\033[127;5u");
    FN("Alt+Backspace", D, KKP_KEY_BACKSPACE, A, P, "\033[127;3u");
    FN("Up", D, KKP_KEY_UP, 0, P, "\033[A");
    FN("Ctrl+Up", D, KKP_KEY_UP, C, P, "\033[1;5A");
    /* the legacy forms carry no lock-key bits; CSI u does */
    FN("Up, NumLock on", D, KKP_KEY_UP, NUM, P, "\033[A");
    FN("Ctrl+Up, NumLock on", D, KKP_KEY_UP, C|NUM, P, "\033[1;5A");
    FN("F5, Caps on", D, KKP_KEY_F1 + 4, CAPS, P, "\033[15~");
    FN("Shift+F5, Caps+NumLock", D, KKP_KEY_F1 + 4, S|CAPS|NUM, P,
       "\033[15;2~");
    FN("Insert, NumLock on", D, KKP_KEY_INSERT, NUM, P, "\033[2~");
    FN("keypad Begin, NumLock on", D, KKP_KEY_KP_BEGIN, NUM, P, "\033[E");
    FN("Up repeat, NumLock on", D|E, KKP_KEY_UP, NUM, R, "\033[1;1:2A");
    FN("Up release, Caps on", D|E, KKP_KEY_UP, CAPS, U, "\033[1;1:3A");
    FN("Up, NumLock, flag 8", D|ALL, KKP_KEY_UP, NUM, P, "\033[A");
    FN("Escape, Caps on (CSI u keeps it)", D, KKP_KEY_ESCAPE, CAPS, P,
       "\033[27;65u");
    FN("Shift+Enter, NumLock on", D, KKP_KEY_ENTER, S|NUM, P,
       "\033[13;130u");
    FN("F13, NumLock on", D, KKP_KEY_F13, NUM, P, "\033[57376;129u");
    FN("Down", D, KKP_KEY_DOWN, 0, P, "\033[B");
    FN("Right", D, KKP_KEY_RIGHT, 0, P, "\033[C");
    FN("Left", D, KKP_KEY_LEFT, 0, P, "\033[D");
    FN("Home", D, KKP_KEY_HOME, 0, P, "\033[H");
    FN("Shift+End", D, KKP_KEY_END, S, P, "\033[1;2F");
    FN("Insert", D, KKP_KEY_INSERT, 0, P, "\033[2~");
    FN("Ctrl+Delete", D, KKP_KEY_DELETE, C, P, "\033[3;5~");
    FN("PgUp", D, KKP_KEY_PAGE_UP, 0, P, "\033[5~");
    FN("Alt+PgDn", D, KKP_KEY_PAGE_DOWN, A, P, "\033[6;3~");
    FN("F1", D, KKP_KEY_F1, 0, P, "\033[P");
    FN("F2", D, KKP_KEY_F1 + 1, 0, P, "\033[Q");
    FN("F3", D, KKP_KEY_F1 + 2, 0, P, "\033[13~");
    FN("Ctrl+F4", D, KKP_KEY_F1 + 3, C, P, "\033[1;5S");
    FN("F5", D, KKP_KEY_F1 + 4, 0, P, "\033[15~");
    FN("Shift+F5", D, KKP_KEY_F1 + 4, S, P, "\033[15;2~");
    FN("F6", D, KKP_KEY_F1 + 5, 0, P, "\033[17~");
    FN("F10", D, KKP_KEY_F1 + 9, 0, P, "\033[21~");
    FN("F11", D, KKP_KEY_F1 + 10, 0, P, "\033[23~");
    FN("F12", D, KKP_KEY_F12, 0, P, "\033[24~");
    FN("F13", D, KKP_KEY_F13, 0, P, "\033[57376u");
    FN("Shift+F35", D, KKP_KEY_F35, S, P, "\033[57398;2u");
    FN("Menu", D, KKP_KEY_MENU, 0, P, "\033[57363u");
    FN("Caps Lock key", D, KKP_KEY_CAPS_LOCK, CAPS, P, "\033[57358;65u");
    FN("Pause", D, KKP_KEY_PAUSE, 0, P, "\033[57362u");
    FN("Print Screen", D, KKP_KEY_PRINT_SCREEN, 0, P, "\033[57361u");
    FN("Mute", D, KKP_KEY_MUTE_VOLUME, 0, P, "\033[57440u");
    FN("Left Shift", D, KKP_KEY_LEFT_SHIFT, S, P, NULL);
    FN("Right Control", D, KKP_KEY_RIGHT_CONTROL, C, P, NULL);
    FN("Left Super", D, KKP_KEY_LEFT_SUPER, W, P, NULL);
    FN("keypad 5", D, KKP_KEY_KP_0 + 5, 0, P, "\033[57404u");
    CH("keypad 5, NumLock types 5", D, KKP_KEY_KP_0 + 5, 0, 0, '5', NUM, P,
       "");
    CH("Ctrl+keypad 5, NumLock", D, KKP_KEY_KP_0 + 5, 0, 0, '5', C|NUM, P,
       "\033[57404;133u");
    FN("keypad Begin", D, KKP_KEY_KP_BEGIN, 0, P, "\033[E");
    FN("Ctrl+keypad Begin", D, KKP_KEY_KP_BEGIN, C, P, "\033[1;5E");
    FN("keypad Enter", D, KKP_KEY_KP_ENTER, 0, P, "\033[57414u");
    FN("keypad Home", D, KKP_KEY_KP_HOME, 0, P, "\033[57423u");
    FN("keypad Delete", D, KKP_KEY_KP_DELETE, 0, P, "\033[57426u");
    CH("keypad +", D, KKP_KEY_KP_ADD, 0, 0, '+', 0, P, "");
    CH("Ctrl+keypad +", D, KKP_KEY_KP_ADD, 0, 0, 0, C, P, "\033[57413;5u");
    CH("no key, text", D, 0, 0, 0, 0xe1, 0, P, "");
    CH("no key, no text", D, 0, 0, 0, 0, C, P, NULL);
    FN("release without flag 2", D, KKP_KEY_UP, 0, U, NULL);
    FN("repeat without flag 2 is a press", D, KKP_KEY_UP, 0, R, "\033[A");
    CH("Ctrl+a repeat without flag 2", D, 'a', 'A', 'a', 0, C, R,
       "\033[97;5u");
    CH("a release without flag 2", D, 'a', 'A', 'a', 0, 0, U, NULL);

    /* --- flag 2: event types --- */
    FN("Up press", D|E, KKP_KEY_UP, 0, P, "\033[A");
    FN("Up repeat", D|E, KKP_KEY_UP, 0, R, "\033[1;1:2A");
    FN("Up release", D|E, KKP_KEY_UP, 0, U, "\033[1;1:3A");
    FN("Ctrl+Up release", D|E, KKP_KEY_UP, C, U, "\033[1;5:3A");
    FN("F5 repeat", D|E, KKP_KEY_F1 + 4, 0, R, "\033[15;1:2~");
    FN("Shift+F5 release", D|E, KKP_KEY_F1 + 4, S, U, "\033[15;2:3~");
    FN("Escape release", D|E, KKP_KEY_ESCAPE, 0, U, "\033[27;1:3u");
    FN("Ctrl+Escape repeat", D|E, KKP_KEY_ESCAPE, C, R, "\033[27;5:2u");
    CH("a press is text", D|E, 'a', 'A', 'a', 'a', 0, P, "");
    CH("a repeat is text", D|E, 'a', 'A', 'a', 'a', 0, R, "");
    CH("a release", D|E, 'a', 'A', 'a', 0, 0, U, "\033[97;1:3u");
    CH("Shift+a release", D|E, 'a', 'A', 'a', 0, S, U, "\033[97;2:3u");
    CH("Ctrl+a press", D|E, 'a', 'A', 'a', 0, C, P, "\033[97;5u");
    CH("Ctrl+a repeat", D|E, 'a', 'A', 'a', 0, C, R, "\033[97;5:2u");
    CH("Ctrl+a release", D|E, 'a', 'A', 'a', 0, C, U, "\033[97;5:3u");
    FN("Enter press stays legacy", D|E, KKP_KEY_ENTER, 0, P, "");
    FN("Enter release sends nothing", D|E, KKP_KEY_ENTER, 0, U, NULL);
    FN("Tab release sends nothing", D|E, KKP_KEY_TAB, 0, U, NULL);
    FN("Backspace release sends nothing", D|E, KKP_KEY_BACKSPACE, 0, U, NULL);
    FN("Shift+Enter release", D|E, KKP_KEY_ENTER, S, U, "\033[13;2:3u");
    FN("Alt+Enter press", D|E, KKP_KEY_ENTER, A, P, "\033[13;3u");
    FN("Alt+Enter release", D|E, KKP_KEY_ENTER, A, U, "\033[13;3:3u");
    FN("keypad 5 release", D|E, KKP_KEY_KP_0 + 5, 0, U, "\033[57404;1:3u");
    FN("Left Shift release, no flag 8", D|E, KKP_KEY_LEFT_SHIFT, 0, U, NULL);

    /* flag 2 alone: legacy text keys keep their bytes, the rest is CSI u */
    FN("2: Up press", E, KKP_KEY_UP, 0, P, "\033[A");
    FN("2: Up release", E, KKP_KEY_UP, 0, U, "\033[1;1:3A");
    FN("2: Escape alone stays legacy", E, KKP_KEY_ESCAPE, 0, P, "");
    FN("2: Escape release sends nothing", E, KKP_KEY_ESCAPE, 0, U, NULL);
    FN("2: Ctrl+Escape", E, KKP_KEY_ESCAPE, C, P, "\033[27;5u");
    FN("2: F1", E, KKP_KEY_F1, 0, P, "\033[P");
    FN("2: Shift+Enter", E, KKP_KEY_ENTER, S, P, "\033[13;2u");
    CH("2: Ctrl+a", E, 'a', 'A', 'a', 0, C, P, "");
    CH("2: Alt+a", E, 'a', 'A', 'a', 0, A, P, "");
    CH("2: Ctrl+Alt+a", E, 'a', 'A', 'a', 0, C|A, P, "");
    CH("2: Shift+Alt+a", E, 'a', 'A', 'a', 0, S|A, P, "");
    CH("2: Ctrl+Shift+a", E, 'a', 'A', 'a', 0, C|S, P, "\033[97;6u");
    CH("2: Ctrl+Shift+2 is Ctrl+@", E, '2', '@', '2', 0, C|S, P, "");
    CH("2: Ctrl+Shift+Space", E, ' ', 0, ' ', 0, C|S, P, "");
    CH("2: Alt+Shift+Space", E, ' ', 0, ' ', 0, A|S, P, "");
    CH("2: Ctrl+Alt+Shift+a", E, 'a', 'A', 'a', 0, C|A|S, P, "\033[97;8u");
    CH("2: Super+a", E, 'a', 'A', 'a', 0, W, P, "\033[97;9u");
    CH("2: Ctrl+Caps a", E, 'a', 'A', 'a', 0, C|CAPS, P, "\033[97;69u");
    CH("2: Ctrl+umlaut is Ctrl+' by position", E, 0xe4, 0xc4, '\'', 0, C, P,
       "");
    CH("2: Ctrl+umlaut, no base key", E, 0xe4, 0xc4, 0, 0, C, P,
       "\033[228;5u");
    CH("2: Ctrl+Cyrillic es is Ctrl+c", E, 0x441, 0x421, 'c', 0, C, P, "");
    CH("2: Ctrl+a release", E, 'a', 'A', 'a', 0, C, U, "\033[97;5:3u");
    FN("2: keypad Home is Home", E, KKP_KEY_KP_HOME, 0, P, "\033[H");
    FN("2: keypad Enter is Enter", E, KKP_KEY_KP_ENTER, 0, P, "");
    CH("2: Ctrl+keypad 5 is Ctrl+5", E, KKP_KEY_KP_0 + 5, 0, 0, 0, C, P, "");
    CH("2: Ctrl+keypad 5, NumLock on", E, KKP_KEY_KP_0 + 5, 0, 0, 0, C|NUM,
       P, "\033[53;133u");

    /* --- flag 4: alternate keys, only on what is encoded anyway --- */
    CH("4 alone: Ctrl+a legacy", AK, 'a', 'A', 'a', 0, C, P, "");
    FN("4 alone: Up legacy", AK, KKP_KEY_UP, 0, P, "");
    FN("4 alone: F13 legacy", AK, KKP_KEY_F13, 0, P, "");
    CH("4 alone: Ctrl+Shift+a", AK, 'a', 'A', 'a', 0, C|S, P,
       "\033[97:65;6u");
    CH("Ctrl+a, no Shift: no shifted key", D|AK, 'a', 'A', 'a', 0, C, P,
       "\033[97;5u");
    CH("Ctrl+Shift+a", D|AK, 'a', 'A', 'a', 0, C|S, P, "\033[97:65;6u");
    CH("Ctrl+Shift+=", D|AK, '=', '+', '=', 0, C|S, P, "\033[61:43;6u");
    CH("Ctrl+Cyrillic es", D|AK, 0x441, 0x421, 'c', 0, C, P,
       "\033[1089::99;5u");
    CH("Ctrl+Shift+Cyrillic es", D|AK, 0x441, 0x421, 'c', 0, C|S, P,
       "\033[1089:1057:99;6u");
    CH("Ctrl+Shift+a, same base", D|AK, 'a', 'A', 'a', 0, C|S, P,
       "\033[97:65;6u");
    CH("Shift+a stays text", D|AK, 'a', 'A', 'a', 'A', S, P, "");
    CH("Ctrl+Shift+a release", D|E|AK, 'a', 'A', 'a', 0, C|S, U,
       "\033[97:65;6:3u");
    FN("Shift+F5, no alternates", D|E|AK, KKP_KEY_F1 + 4, S, P,
       "\033[15;2~");
    CH("Ctrl+umlaut, base", D|AK, 0xe4, 0xc4, '\'', 0, C, P,
       "\033[228::39;5u");

    /* --- flag 8: every key --- */
    CH("8: a", D|ALL, 'a', 'A', 'a', 'a', 0, P, "\033[97u");
    CH("8: Shift+a", D|ALL, 'a', 'A', 'a', 'A', S, P, "\033[97;2u");
    CH("8: Caps a", D|ALL, 'a', 'A', 'a', 'A', CAPS, P, "\033[97;65u");
    CH("8: Ctrl+a", D|ALL, 'a', 'A', 'a', 0, C, P, "\033[97;5u");
    FN("8: Enter", D|ALL, KKP_KEY_ENTER, 0, P, "\033[13u");
    FN("8: Tab", D|ALL, KKP_KEY_TAB, 0, P, "\033[9u");
    FN("8: Backspace", D|ALL, KKP_KEY_BACKSPACE, 0, P, "\033[127u");
    FN("8: Escape", D|ALL, KKP_KEY_ESCAPE, 0, P, "\033[27u");
    FN("8: Left Shift press", D|ALL, KKP_KEY_LEFT_SHIFT, S, P,
       "\033[57441;2u");
    FN("8: Left Shift release", D|E|ALL, KKP_KEY_LEFT_SHIFT, 0, U,
       "\033[57441;1:3u");
    FN("8: Right Control press", D|ALL, KKP_KEY_RIGHT_CONTROL, C, P,
       "\033[57448;5u");
    FN("8: Left Alt press", D|ALL, KKP_KEY_LEFT_ALT, A, P, "\033[57443;3u");
    FN("8: Left Super press", D|ALL, KKP_KEY_LEFT_SUPER, W, P,
       "\033[57444;9u");
    FN("8: Enter release", D|E|ALL, KKP_KEY_ENTER, 0, U, "\033[13;1:3u");
    CH("8: a release", D|E|ALL, 'a', 'A', 'a', 0, 0, U, "\033[97;1:3u");
    CH("8: a repeat", D|E|ALL, 'a', 'A', 'a', 'a', 0, R, "\033[97;1:2u");
    CH("8 alone: a", ALL, 'a', 'A', 'a', 'a', 0, P, "\033[97u");
    CH("8 alone: Ctrl+a", ALL, 'a', 'A', 'a', 0, C, P, "\033[97;5u");
    FN("8 alone: keypad 5 stays keypad", ALL, KKP_KEY_KP_0 + 5, 0, P,
       "\033[57404u");
    CH("8: keypad 5 with NumLock", D|ALL, KKP_KEY_KP_0 + 5, 0, 0, '5', NUM, P,
       "\033[57404;129u");
    CH("8: Shift+a with alternates", D|AK|ALL, 'a', 'A', 'a', 'A', S, P,
       "\033[97:65;2u");
    CH("8: no key, text only", D|ALL, 0, 0, 0, 0xe1, 0, P, "\033[0u");

    /* --- flag 16: the text --- */
    CH("16: a", D|ALL|T, 'a', 'A', 'a', 'a', 0, P, "\033[97;;97u");
    CH("16: Shift+a", D|ALL|T, 'a', 'A', 'a', 'A', S, P, "\033[97;2;65u");
    CH("16: Shift+a, alternates", D|AK|ALL|T, 'a', 'A', 'a', 'A', S, P,
       "\033[97:65;2;65u");
    CH("16: Ctrl+a has no text", D|ALL|T, 'a', 'A', 'a', 1, C, P,
       "\033[97;5u");
    CH("16: Alt+a has no text", D|ALL|T, 'a', 'A', 'a', 'a', A, P,
       "\033[97;3u");
    CH("16: no key, text only", D|ALL|T, 0, 0, 0, 0xe1, 0, P,
       "\033[0;;225u");
    CH("16: dead key composed", D|ALL|T, 'a', 'A', 'a', 0xe1, 0, P,
       "\033[97;;225u");
    CH("16: a repeat", D|E|ALL|T, 'a', 'A', 'a', 'a', 0, R,
       "\033[97;1:2;97u");
    CH("16: a release has no text", D|E|ALL|T, 'a', 'A', 'a', 'a', 0, U,
       "\033[97;1:3u");
    CH("16: keypad 5 types 5", D|ALL|T, KKP_KEY_KP_0 + 5, 0, 0, '5', NUM, P,
       "\033[57404;129;53u");
    CH("16: control text dropped", D|ALL|T, 'a', 'A', 'a', 0x1b, 0, P,
       "\033[97u");
    CH("16: non-BMP text", D|ALL|T, 'a', 'A', 'a', 0x1f600, 0, P,
       "\033[97;;128512u");
    FN("16: Enter has no text", D|ALL|T, KKP_KEY_ENTER, 0, P, "\033[13u");
    CH("16 without 8: text is text", D|T, 'a', 'A', 'a', 'a', 0, P, "");

    /* --- the buffer --- */
    {
        KittyKeyEvent ev = { 'a', 'A', 'a', 'a', 0, KKP_PRESS };
        char small[KKP_BUFFER_SIZE - 1];
        if (kitty_kkp_encode(D|ALL, &ev, small, sizeof(small)) != KKP_NOTHING) {
            printf("FAIL [short buffer]: wrote into a buffer too small\n");
            failures++;
        }
        if (kitty_kkp_encode(D|ALL, &ev, NULL, 0) != KKP_NOTHING) {
            printf("FAIL [no buffer]\n");
            failures++;
        }
        if (kitty_kkp_encode(D|ALL, NULL, small, sizeof(small)) != KKP_LEGACY) {
            printf("FAIL [no event]\n");
            failures++;
        }
    }

    /* --- the base-layout table --- */
    {
        static const struct { unsigned scan; unsigned ch; } t[] = {
            { 0x02, '1' }, { 0x0b, '0' }, { 0x0c, '-' }, { 0x0d, '=' },
            { 0x10, 'q' }, { 0x19, 'p' }, { 0x1a, '[' }, { 0x1b, ']' },
            { 0x1e, 'a' }, { 0x26, 'l' }, { 0x27, ';' }, { 0x28, '\'' },
            { 0x29, '`' }, { 0x2b, '\\' }, { 0x2c, 'z' }, { 0x32, 'm' },
            { 0x33, ',' }, { 0x34, '.' }, { 0x35, '/' }, { 0x39, ' ' },
            { 0x01, 0 }, { 0x0e, 0 }, { 0x0f, 0 }, { 0x1c, 0 }, { 0x1d, 0 },
            { 0x2a, 0 }, { 0x36, 0 }, { 0x3b, 0 }, { 0x48, 0 }, { 0, 0 },
        };
        size_t i;
        for (i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
            if (kitty_kkp_base_key(t[i].scan) != t[i].ch) {
                printf("FAIL [base key]: scan 0x%02x gives %u, want %u\n",
                       t[i].scan, kitty_kkp_base_key(t[i].scan), t[i].ch);
                failures++;
            }
        }
    }

    if (failures)
        printf("test_kittykeys: %d failure(s)\n", failures);
    else
        printf("test_kittykeys: all passed\n");
    return failures ? 1 : 0;
}
