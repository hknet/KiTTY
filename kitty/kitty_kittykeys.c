/*
 * kitty_kittykeys.c - the kitty keyboard protocol's key encoding. See
 * kitty_kittykeys.h; the rules are the specification's, in the order of
 * kitty's own encoder so the two agree on every key:
 *
 *  - a release goes out only with flag 2, a modifier key only with flag 8;
 *  - without flag 1 and 8 a keypad key is its plain counterpart;
 *  - without flag 8 a key that types text (a plain or shifted key, Caps Lock
 *    or not; never a Ctrl, Alt or Super chord) is that text - legacy;
 *  - a functional key: without flags 1, 2 and 8 every one of them is legacy;
 *    Escape alone is legacy without flag 1 or 8; Enter, Tab and Backspace
 *    alone (lock keys aside) are legacy without flag 8, and their release
 *    sends nothing; the rest go out as CSI number ; mods u, the cursor and
 *    editing keys and F1..F12 in their legacy numbers (CSI 1 ; mods A ...),
 *    which carry no lock-key bits as legacy mode has no encoding for them;
 *  - any other key with a modifier: without flag 1 and 8 the legacy text
 *    keys (a-z 0-9 and the US symbols) stay legacy with Shift, Alt, Ctrl or
 *    Ctrl+Alt (Shift folded into the shifted character where there is one,
 *    Ctrl+letter aside), as does Space with Ctrl+Shift and Alt+Shift, and a
 *    key whose base-layout key would; everything else is CSI u.
 *
 * Legacy means "today's bytes": the caller sends what it always did, which
 * is how level 0 stays byte-for-byte what it was.
 */
#include <stdio.h>

#include "kitty_kittykeys.h"

#define CHORD (KKP_ALT | KKP_CTRL | KKP_SUPER | KKP_HYPER | KKP_META)

bool kitty_kkp_is_modifier_key(unsigned key)
{
    return key >= KKP_KEY_LEFT_SHIFT && key <= KKP_KEY_ISO_LEVEL5_SHIFT;
}

unsigned kitty_kkp_base_key(unsigned scancode)
{
    static const char row1[] = "1234567890-=";       /* 0x02.. */
    static const char row2[] = "qwertyuiop[]";       /* 0x10.. */
    static const char row3[] = "asdfghjkl;'`";       /* 0x1e.. */
    static const char row4[] = "\\zxcvbnm,./";       /* 0x2b.. */

    if (scancode >= 0x02 && scancode <= 0x0d) return row1[scancode - 0x02];
    if (scancode >= 0x10 && scancode <= 0x1b) return row2[scancode - 0x10];
    if (scancode >= 0x1e && scancode <= 0x29) return row3[scancode - 0x1e];
    if (scancode >= 0x2b && scancode <= 0x35) return row4[scancode - 0x2b];
    if (scancode == 0x39) return ' ';
    return 0;
}

static bool is_functional(unsigned key)
{
    return key >= KKP_KEY_FIRST && key <= KKP_KEY_LAST;
}

/* The legacy text keys: a-z, 0-9 and the US symbols, shifted or not. */
static bool legacy_ascii(unsigned key)
{
    if ((key >= 'a' && key <= 'z') || (key >= '0' && key <= '9'))
        return true;
    switch (key) {
      case '!': case '@': case '#': case '$': case '%': case '^': case '&':
      case '*': case '(': case ')': case '`': case '~': case '-': case '_':
      case '=': case '+': case '[': case '{': case ']': case '}': case '\\':
      case '|': case ';': case ':': case '\'': case '"': case ',': case '<':
      case '.': case '>': case '/': case '?': case ' ':
        return true;
    }
    return false;
}

/* A keypad key as its plain counterpart (no flag 1 or 8). */
static unsigned keypad_as_plain(unsigned key)
{
    switch (key) {
      case KKP_KEY_KP_ENTER: return KKP_KEY_ENTER;
      case KKP_KEY_KP_HOME: return KKP_KEY_HOME;
      case KKP_KEY_KP_END: return KKP_KEY_END;
      case KKP_KEY_KP_INSERT: return KKP_KEY_INSERT;
      case KKP_KEY_KP_DELETE: return KKP_KEY_DELETE;
      case KKP_KEY_KP_PAGE_UP: return KKP_KEY_PAGE_UP;
      case KKP_KEY_KP_PAGE_DOWN: return KKP_KEY_PAGE_DOWN;
      case KKP_KEY_KP_UP: return KKP_KEY_UP;
      case KKP_KEY_KP_DOWN: return KKP_KEY_DOWN;
      case KKP_KEY_KP_LEFT: return KKP_KEY_LEFT;
      case KKP_KEY_KP_RIGHT: return KKP_KEY_RIGHT;
      case KKP_KEY_KP_DECIMAL: return '.';
      case KKP_KEY_KP_DIVIDE: return '/';
      case KKP_KEY_KP_MULTIPLY: return '*';
      case KKP_KEY_KP_SUBTRACT: return '-';
      case KKP_KEY_KP_ADD: return '+';
      case KKP_KEY_KP_EQUAL: return '=';
    }
    if (key >= KKP_KEY_KP_0 && key <= KKP_KEY_KP_9)
        return '0' + (key - KKP_KEY_KP_0);
    return key;
}

/* Whether the legacy text keys keep their legacy bytes with these
 * modifiers (kitty's encode_printable_ascii_key_legacy, the decision only). */
static bool legacy_with_mods(unsigned key, unsigned shifted, unsigned mods)
{
    unsigned m = mods;

    if ((m & KKP_SHIFT) && shifted && shifted != key &&
        (!(m & KKP_CTRL) || key < 'a' || key > 'z')) {
        key = shifted;
        m &= ~KKP_SHIFT;
    }
    if (mods == KKP_SHIFT || m == KKP_ALT || m == KKP_CTRL ||
        m == (KKP_CTRL | KKP_ALT))
        return true;
    if (key == ' ' &&
        (m == (KKP_CTRL | KKP_SHIFT) || m == (KKP_ALT | KKP_SHIFT)))
        return true;
    return false;
}

/* The CSI, as kitty serialises it: the key number (omitted when it is 1 and
 * nothing follows), the alternates, "; mods [: event]", "; text". */
static int serialize(unsigned key, unsigned shifted, unsigned base,
                     unsigned mods, int event, unsigned text, char trailer,
                     char *out, size_t outsz)
{
    bool alternates = shifted || base;
    bool second = mods || event != KKP_PRESS;
    int pos = 0, n;

#define P(...) do { \
        n = snprintf(out + pos, outsz > (size_t)pos ? outsz - pos : 0, \
                     __VA_ARGS__); \
        if (n < 0 || (size_t)n >= outsz - pos) return KKP_NOTHING; \
        pos += n; \
    } while (0)

    P("\033[");
    if (key != 1 || alternates || second || text)
        P("%u", key);
    if (alternates) {
        P(":");
        if (shifted) P("%u", shifted);
        if (base) P(":%u", base);
    }
    if (second || text) {
        P(";");
        if (second) P("%u", mods + 1);
        if (event != KKP_PRESS) P(":%d", event);
    }
    if (text)
        P(";%u", text);
    if ((size_t)pos + 2 > outsz)
        return KKP_NOTHING;
    out[pos++] = trailer;
    out[pos] = '\0';
#undef P
    return pos;
}

static int encode_functional(unsigned flags, unsigned key, unsigned mods,
                             int event, unsigned text, char *out,
                             size_t outsz)
{
    bool legacy_mode = !(flags & (KKP_DISAMBIGUATE | KKP_EVENT_TYPES |
                                  KKP_ALL_KEYS));
    bool all = (flags & KKP_ALL_KEYS) != 0;
    char trailer = 'u';
    unsigned number = key;

    if (legacy_mode)
        return KKP_LEGACY;
    if (!mods && !(flags & KKP_DISAMBIGUATE) && !all &&
        key == KKP_KEY_ESCAPE)
        return event == KKP_RELEASE ? KKP_NOTHING : KKP_LEGACY;
    if (!(mods & ~KKP_LOCKS) && !all &&
        (key == KKP_KEY_ENTER || key == KKP_KEY_BACKSPACE ||
         key == KKP_KEY_TAB))
        return event == KKP_RELEASE ? KKP_NOTHING : KKP_LEGACY;

    switch (key) {
      case KKP_KEY_ESCAPE: number = 27; break;
      case KKP_KEY_ENTER: number = 13; break;
      case KKP_KEY_TAB: number = 9; break;
      case KKP_KEY_BACKSPACE: number = 127; break;
      case KKP_KEY_INSERT: number = 2; trailer = '~'; break;
      case KKP_KEY_DELETE: number = 3; trailer = '~'; break;
      case KKP_KEY_LEFT: number = 1; trailer = 'D'; break;
      case KKP_KEY_RIGHT: number = 1; trailer = 'C'; break;
      case KKP_KEY_UP: number = 1; trailer = 'A'; break;
      case KKP_KEY_DOWN: number = 1; trailer = 'B'; break;
      case KKP_KEY_PAGE_UP: number = 5; trailer = '~'; break;
      case KKP_KEY_PAGE_DOWN: number = 6; trailer = '~'; break;
      case KKP_KEY_HOME: number = 1; trailer = 'H'; break;
      case KKP_KEY_END: number = 1; trailer = 'F'; break;
      case KKP_KEY_F1: number = 1; trailer = 'P'; break;
      case KKP_KEY_F1 + 1: number = 1; trailer = 'Q'; break;
      case KKP_KEY_F1 + 2: number = 13; trailer = '~'; break;
      case KKP_KEY_F1 + 3: number = 1; trailer = 'S'; break;
      case KKP_KEY_F1 + 4: number = 15; trailer = '~'; break;
      case KKP_KEY_F1 + 5: number = 17; trailer = '~'; break;
      case KKP_KEY_F1 + 6: number = 18; trailer = '~'; break;
      case KKP_KEY_F1 + 7: number = 19; trailer = '~'; break;
      case KKP_KEY_F1 + 8: number = 20; trailer = '~'; break;
      case KKP_KEY_F1 + 9: number = 21; trailer = '~'; break;
      case KKP_KEY_F1 + 10: number = 23; trailer = '~'; break;
      case KKP_KEY_F12: number = 24; trailer = '~'; break;
      case KKP_KEY_KP_BEGIN: number = 1; trailer = 'E'; break;
    }
    /* The legacy forms (CSI number ; mods ~, CSI 1 ; mods letter) have no
     * encoding for the lock keys; only CSI u carries them. */
    if (trailer != 'u')
        mods &= ~KKP_LOCKS;
    return serialize(number, 0, 0, mods, event, text, trailer, out, outsz);
}

int kitty_kkp_encode(unsigned flags, const KittyKeyEvent *ev,
                     char *out, size_t outsz)
{
    unsigned key, mods, shifted = 0, base = 0, text = 0;
    bool has_text;
    int event;

    flags &= KKP_ALL_FLAGS;
    if (!flags || !ev)
        return KKP_LEGACY;
    if (!out || outsz < KKP_BUFFER_SIZE)
        return KKP_NOTHING;

    key = ev->key;
    mods = ev->mods & (KKP_LOCKS | CHORD | KKP_SHIFT);
    if (!(flags & KKP_ALL_KEYS) && kitty_kkp_is_modifier_key(key))
        return KKP_NOTHING;
    has_text = ev->text >= 0x20 && ev->text != 0x7f &&
               !(ev->text >= 0x80 && ev->text < 0xa0) &&
               !(mods & CHORD) && ev->event != KKP_RELEASE;
    if (!key && !has_text)
        return KKP_NOTHING;
    if (!(flags & (KKP_DISAMBIGUATE | KKP_ALL_KEYS)) && is_functional(key))
        key = keypad_as_plain(key);
    if (!(flags & KKP_ALL_KEYS) && has_text)
        return KKP_LEGACY;
    if (!(flags & KKP_EVENT_TYPES) && ev->event == KKP_RELEASE)
        return KKP_NOTHING;
    /* Without flag 2 a repeat is a press; from here on a release is only
     * seen with it. */
    event = (flags & KKP_EVENT_TYPES) ? ev->event : KKP_PRESS;
    if ((flags & KKP_TEXT) && has_text)
        text = ev->text;

    if (is_functional(key))
        return encode_functional(flags, key, mods, event, text, out, outsz);

    if (flags & KKP_ALTERNATE_KEYS) {
        if ((mods & KKP_SHIFT) && ev->shifted && ev->shifted != key)
            shifted = ev->shifted;
        if (ev->base && ev->base != key)
            base = ev->base;
    }

    if (event == KKP_PRESS) {
        if (!shifted && !base && !text) {
            if (!mods && !(flags & KKP_ALL_KEYS))
                return KKP_LEGACY;
            if (mods && !(flags & (KKP_DISAMBIGUATE | KKP_ALL_KEYS))) {
                if (legacy_ascii(key) ||
                    (ev->shifted && legacy_ascii(ev->shifted))) {
                    if (legacy_with_mods(key, ev->shifted, mods))
                        return KKP_LEGACY;
                } else if (ev->base && legacy_ascii(ev->base) &&
                           (mods == KKP_CTRL || mods == KKP_ALT ||
                            mods == (KKP_CTRL | KKP_ALT))) {
                    return KKP_LEGACY;
                }
            }
        }
    }
    return serialize(key, shifted, base, mods, event, text, 'u', out, outsz);
}
