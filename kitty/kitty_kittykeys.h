/*
 * kitty_kittykeys.h - the kitty keyboard protocol (CSI u with progressive
 * enhancement, https://sw.kovidgoyal.net/kitty/keyboard-protocol/): the
 * bytes a key event goes out as once a program has pushed enhancement
 * flags. The platform half - which key, what it types, the modifiers held -
 * is the window's; the terminal keeps the flag stacks. No Win32 and no
 * PuTTY types, so test/test_kittykeys.c covers it without a window.
 */
#ifndef KITTY_KITTYKEYS_H
#define KITTY_KITTYKEYS_H

#include <stdbool.h>
#include <stddef.h>

/* The progressive enhancement flags. */
#define KKP_DISAMBIGUATE   1u   /* Esc, Ctrl/Alt chords, keypad as CSI u */
#define KKP_EVENT_TYPES    2u   /* repeat and release events */
#define KKP_ALTERNATE_KEYS 4u   /* shifted and base-layout key */
#define KKP_ALL_KEYS       8u   /* every key as an escape code */
#define KKP_TEXT          16u   /* the text the key types, as code points */
#define KKP_ALL_FLAGS     31u
#define KKP_STACK_MAX     16    /* entries per screen; the oldest drops out */

/* Modifier bits, the protocol's: the parameter is 1 + the sum. */
#define KKP_SHIFT   1u
#define KKP_ALT     2u
#define KKP_CTRL    4u
#define KKP_SUPER   8u
#define KKP_HYPER  16u
#define KKP_META   32u
#define KKP_CAPS   64u
#define KKP_NUM   128u
#define KKP_LOCKS (KKP_CAPS | KKP_NUM)

/* Event types, the protocol's sub-parameter values. */
enum { KKP_PRESS = 1, KKP_REPEAT = 2, KKP_RELEASE = 3 };

/* Functional keys: the protocol's Private Use Area numbers, in the order of
 * its table. ESCAPE..END and F1..F12 go out under their legacy numbers
 * (27 u, 2 ~, 1 A ...); the encoder translates. */
enum {
    KKP_KEY_FIRST = 57344,
    KKP_KEY_ESCAPE = 57344, KKP_KEY_ENTER, KKP_KEY_TAB, KKP_KEY_BACKSPACE,
    KKP_KEY_INSERT, KKP_KEY_DELETE, KKP_KEY_LEFT, KKP_KEY_RIGHT, KKP_KEY_UP,
    KKP_KEY_DOWN, KKP_KEY_PAGE_UP, KKP_KEY_PAGE_DOWN, KKP_KEY_HOME,
    KKP_KEY_END, KKP_KEY_CAPS_LOCK, KKP_KEY_SCROLL_LOCK, KKP_KEY_NUM_LOCK,
    KKP_KEY_PRINT_SCREEN, KKP_KEY_PAUSE, KKP_KEY_MENU,
    KKP_KEY_F1 = 57364,                 /* F1..F35 contiguous, to 57398 */
    KKP_KEY_F12 = 57375, KKP_KEY_F13, KKP_KEY_F35 = 57398,
    KKP_KEY_KP_0 = 57399,               /* KP_0..KP_9 contiguous */
    KKP_KEY_KP_9 = 57408, KKP_KEY_KP_DECIMAL, KKP_KEY_KP_DIVIDE,
    KKP_KEY_KP_MULTIPLY, KKP_KEY_KP_SUBTRACT, KKP_KEY_KP_ADD,
    KKP_KEY_KP_ENTER, KKP_KEY_KP_EQUAL, KKP_KEY_KP_SEPARATOR, KKP_KEY_KP_LEFT,
    KKP_KEY_KP_RIGHT, KKP_KEY_KP_UP, KKP_KEY_KP_DOWN, KKP_KEY_KP_PAGE_UP,
    KKP_KEY_KP_PAGE_DOWN, KKP_KEY_KP_HOME, KKP_KEY_KP_END, KKP_KEY_KP_INSERT,
    KKP_KEY_KP_DELETE, KKP_KEY_KP_BEGIN,
    KKP_KEY_MEDIA_PLAY = 57428, KKP_KEY_MEDIA_PAUSE, KKP_KEY_MEDIA_PLAY_PAUSE,
    KKP_KEY_MEDIA_REVERSE, KKP_KEY_MEDIA_STOP, KKP_KEY_MEDIA_FAST_FORWARD,
    KKP_KEY_MEDIA_REWIND, KKP_KEY_MEDIA_TRACK_NEXT,
    KKP_KEY_MEDIA_TRACK_PREVIOUS, KKP_KEY_MEDIA_RECORD, KKP_KEY_LOWER_VOLUME,
    KKP_KEY_RAISE_VOLUME, KKP_KEY_MUTE_VOLUME,
    KKP_KEY_LEFT_SHIFT = 57441, KKP_KEY_LEFT_CONTROL, KKP_KEY_LEFT_ALT,
    KKP_KEY_LEFT_SUPER, KKP_KEY_LEFT_HYPER, KKP_KEY_LEFT_META,
    KKP_KEY_RIGHT_SHIFT, KKP_KEY_RIGHT_CONTROL, KKP_KEY_RIGHT_ALT,
    KKP_KEY_RIGHT_SUPER, KKP_KEY_RIGHT_HYPER, KKP_KEY_RIGHT_META,
    KKP_KEY_ISO_LEVEL3_SHIFT, KKP_KEY_ISO_LEVEL5_SHIFT,
    KKP_KEY_LAST = 57454
};

/* One key event. key: the code point of the key with no modifier applied
 * (lower case, as the layout has it) or a KKP_KEY_* number; 0 when the key
 * is unknown and only text is. shifted: the code point with Shift, 0 when
 * none or the same. base: the key at that position in the standard PC-101
 * layout, 0 when none or the same. text: the code point the key types with
 * the modifiers held, 0 when none; ignored with Ctrl, Alt, Super, Hyper or
 * Meta held (no text comes of those chords) and on a release. */
typedef struct KittyKeyEvent {
    unsigned key, shifted, base, text;
    unsigned mods;                     /* KKP_* bits */
    int event;                         /* KKP_PRESS, KKP_REPEAT, KKP_RELEASE */
} KittyKeyEvent;

#define KKP_LEGACY  0                  /* the key keeps today's encoding */
#define KKP_NOTHING (-1)               /* nothing goes out */

/* The bytes for ev at flags, written to out. Returns their count,
 * KKP_LEGACY (0) when the protocol leaves the key to the legacy encoding -
 * always at flags 0 - or KKP_NOTHING when the event sends nothing (a
 * release without flag 2, a modifier key without flag 8). Needs
 * KKP_BUFFER_SIZE bytes in out; a smaller buffer yields KKP_NOTHING. */
#define KKP_BUFFER_SIZE 48
int kitty_kkp_encode(unsigned flags, const KittyKeyEvent *ev,
                     char *out, size_t outsz);

/* The modifier keys themselves (KKP_KEY_LEFT_SHIFT..ISO_LEVEL5_SHIFT). */
bool kitty_kkp_is_modifier_key(unsigned key);

/* The character at a PC-101 position, from the key's scan code (set 1 make
 * code, 1..0x39), for the base-layout key: 0 for a position that has none. */
unsigned kitty_kkp_base_key(unsigned scancode);

#endif /* KITTY_KITTYKEYS_H */
