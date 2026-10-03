/*
 * test_far2l_keys - regression tests for the far2l key and mouse events
 * (kitty/kitty_far2l_input.h): the bytes the terminal sends to a far2l
 * client once the extensions are active.
 *
 * Checked: the wire examples of the protocol description, which were
 * produced by far2l's own serializer (golden vectors); every field of both
 * key forms and both mouse forms, decoded back the way the far2l client pops
 * them; the rules for choosing the compact form; the control key state bits
 * (left/right Ctrl and Alt, Shift, the three locks, enhanced), AltGr as
 * Windows reports it; the button squeeze; wheel deltas; surrogate pairs; the
 * paste gestures; the scan codes reported as 0. Dependency-free: builds
 * natively on the build host. Returns non-zero on any failure.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KITTY_FAR2L_INPUT_IMPL
#include "../kitty/kitty_far2l_input.h"

static int failures = 0;

#define CHECK(cond, what) do { \
    if (!(cond)) { printf("FAIL: %s (line %d)\n", what, __LINE__); failures++; } \
} while (0)

/* ---- a small client: base64 decode and pop, as far2l's client does ---- */

typedef struct {
    unsigned char b[64];
    size_t n;                       /* bytes left; the top is b[n-1] */
    int bad;                        /* popped past the bottom */
} Stk;

static int b64val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* Unframe ESC _ f2l <b64> BEL into a stack. Returns 0 on a framing error. */
static int unframe(const char *w, size_t len, Stk *s)
{
    size_t i;
    uint32_t acc = 0;
    int bits = 0;
    memset(s, 0, sizeof(*s));
    if (len < 6 || memcmp(w, "\x1b_f2l", 5) || w[len - 1] != '\x07')
        return 0;
    for (i = 5; i < len - 1; i++) {
        int v = b64val(w[i]);
        if (v < 0)
            break;                  /* '=' or anything else ends it */
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            s->b[s->n++] = (unsigned char)((acc >> bits) & 0xFF);
        }
    }
    return 1;
}

static uint32_t pop(Stk *s, int bytes)
{
    uint32_t v = 0;
    int i;
    if (s->n < (size_t)bytes) { s->bad = 1; return 0; }
    /* The value's bytes sit little-endian with its first byte deepest. */
    for (i = 0; i < bytes; i++)
        v |= (uint32_t)s->b[s->n - bytes + i] << (8 * i);
    s->n -= bytes;
    return v;
}

/* ---- golden vectors (protocol description, section 7.8) ---- */

static void expect_wire(const char *got, size_t glen, const char *want,
                        const char *what)
{
    if (glen != strlen(want) || memcmp(got, want, glen)) {
        printf("FAIL: %s\n  got  ", what);
        fwrite(got + 1, 1, glen > 2 ? glen - 2 : 0, stdout);
        printf("\n  want ");
        fwrite(want + 1, 1, strlen(want) - 2, stdout);
        printf("\n");
        failures++;
    }
}

static void test_golden(void)
{
    char out[F2L_EVENT_MAX];
    size_t n;
    Far2lKeyEvent k = { true, 0x61, 0, 0x1E, 0x41, 1 };
    Far2lMouseEvent m = { 0, 0, F2L_BUTTON_LEFT, 10, 5 };
    Far2lMouseEvent w = { F2L_MOUSE_WHEELED, 0, 0, 10, 5 };

    n = far2l_encode_key(&k, false, out, sizeof(out));
    expect_wire(out, n, "\x1b_f2lAQBBAB4AAAAAAGEAAABL\x07", "key down 'a', full form");

    n = far2l_encode_key(&k, true, out, sizeof(out));
    expect_wire(out, n, "\x1b_f2lQQAAYQBD\x07", "key down 'a', compact form");

    n = far2l_encode_mouse(&m, true, out, sizeof(out));
    expect_wire(out, n, "\x1b_f2lCgAFAAEAAABt\x07", "left press, compact mouse");

    w.buttons = far2l_wheel_buttons(0, -1);
    CHECK(w.buttons == 0xFFFF0000u, "wheel -1 sits in the high word");
    n = far2l_encode_mouse(&w, true, out, sizeof(out));
    expect_wire(out, n, "\x1b_f2lCgAFAAAA//8AAAAABAAAAE0=\x07",
                "wheel down: full form even when compact is allowed");

    /* No colon after f2l, BEL at the end, NUL after it. */
    CHECK(out[5] != ':', "no colon after f2l");
    CHECK(out[n] == '\0', "NUL-terminated");
    CHECK(far2l_encode_key(&k, false, out, 10) == 0, "too small a buffer is refused");
}

/* ---- every field, both key forms ---- */

static void roundtrip_key(const Far2lKeyEvent *ev, bool compact_ok,
                          bool want_compact, const char *what)
{
    char out[F2L_EVENT_MAX];
    char msg[160];
    Stk s;
    size_t n = far2l_encode_key(ev, compact_ok, out, sizeof(out));
    int code;

    snprintf(msg, sizeof(msg), "%s: framed", what);
    CHECK(n > 0 && unframe(out, n, &s), msg);
    code = (int)pop(&s, 1);
    if (want_compact) {
        uint32_t ch, ctrl, vk;
        snprintf(msg, sizeof(msg), "%s: compact code", what);
        CHECK(code == (ev->down ? 'C' : 'c'), msg);
        ch = pop(&s, 2); ctrl = pop(&s, 2); vk = pop(&s, 1);
        snprintf(msg, sizeof(msg), "%s: compact fields", what);
        CHECK(!s.bad && s.n == 0 && ch == ev->uchar && ctrl == ev->ctrl &&
              vk == ev->vk, msg);
    } else {
        uint32_t ch, ctrl, vsc, vk, rep;
        snprintf(msg, sizeof(msg), "%s: full code", what);
        CHECK(code == (ev->down ? 'K' : 'k'), msg);
        ch = pop(&s, 4); ctrl = pop(&s, 4); vsc = pop(&s, 2);
        vk = pop(&s, 2); rep = pop(&s, 2);
        snprintf(msg, sizeof(msg), "%s: full fields", what);
        CHECK(!s.bad && s.n == 0 && ch == ev->uchar && ctrl == ev->ctrl &&
              vsc == ev->vsc && vk == ev->vk && rep == ev->repeat, msg);
    }
}

static void test_key_fields(void)
{
    Far2lKeyEvent e;

    /* Plain key, down and up, full and compact. */
    e = (Far2lKeyEvent){ true, 'x', 0, 0x2D, 'X', 1 };
    roundtrip_key(&e, false, false, "x down full");
    roundtrip_key(&e, true, true, "x down compact");
    e.down = false;
    roundtrip_key(&e, false, false, "x up full");
    roundtrip_key(&e, true, true, "x up compact");

    /* Every field at a distinctive value, full form. */
    e = (Far2lKeyEvent){ true, 0x1F600, 0x01FF, 0xABCD, 0x1234, 0x7FFF };
    roundtrip_key(&e, false, false, "all fields wide");
    roundtrip_key(&e, true, false, "all fields wide, compact refused");

    /* Each reason the compact form is refused, one at a time. */
    e = (Far2lKeyEvent){ true, 'a', 0, 0x1E, 'A', 2 };
    roundtrip_key(&e, true, false, "repeat 2 -> full");
    e = (Far2lKeyEvent){ true, 0, F2L_SHIFT_PRESSED, F2L_RIGHT_SHIFT_VSC, 0x10, 1 };
    roundtrip_key(&e, true, false, "right Shift scan -> full");
    e.vsc = 0x2A;
    roundtrip_key(&e, true, true, "left Shift scan -> compact");
    e = (Far2lKeyEvent){ true, 0x10000, 0, 0x10, 'Q', 1 };
    roundtrip_key(&e, true, false, "char beyond 16 bits -> full");
    e = (Far2lKeyEvent){ true, 'a', 0x10000, 0x1E, 'A', 1 };
    roundtrip_key(&e, true, false, "control state beyond 16 bits -> full");
    e = (Far2lKeyEvent){ true, 'a', 0, 0x1E, 0x100, 1 };
    roundtrip_key(&e, true, false, "vk beyond 8 bits -> full");
    e = (Far2lKeyEvent){ true, 'a', 0, 0x1E, 'A', 0 };
    roundtrip_key(&e, true, true, "repeat 0 -> compact");

    CHECK(!far2l_key_fits_compact(&(Far2lKeyEvent){ true, 0xFFFF + 1, 0, 0, 0, 1 }),
          "0x10000 does not fit");
    CHECK(far2l_key_fits_compact(&(Far2lKeyEvent){ true, 0xFFFF, 0xFFFF, 0, 0xFF, 1 }),
          "0xFFFF / 0xFFFF / 0xFF fit");
}

/* ---- control key state ---- */

static void test_ctrl_state(void)
{
    Far2lModifiers m;
    uint32_t c;

    memset(&m, 0, sizeof(m));
    CHECK(far2l_ctrl_state(&m, false) == 0, "nothing held");
    CHECK(far2l_ctrl_state(&m, true) == F2L_ENHANCED_KEY, "enhanced alone");

    m.lctrl = true;
    CHECK(far2l_ctrl_state(&m, false) == 0x0008, "left Ctrl");
    m.lctrl = false; m.rctrl = true;
    CHECK(far2l_ctrl_state(&m, false) == 0x0004, "right Ctrl");
    m.rctrl = false; m.lalt = true;
    CHECK(far2l_ctrl_state(&m, false) == 0x0002, "left Alt");
    m.lalt = false; m.ralt = true;
    CHECK(far2l_ctrl_state(&m, false) == 0x0001, "right Alt");
    m.ralt = false; m.shift = true;
    CHECK(far2l_ctrl_state(&m, false) == 0x0010, "Shift");
    m.shift = false; m.numlock = true;
    CHECK(far2l_ctrl_state(&m, false) == 0x0020, "NumLock");
    m.numlock = false; m.scrolllock = true;
    CHECK(far2l_ctrl_state(&m, false) == 0x0040, "ScrollLock");
    m.scrolllock = false; m.capslock = true;
    CHECK(far2l_ctrl_state(&m, false) == 0x0080, "CapsLock");

    /* Every Ctrl+Shift combination keeps the sides apart. */
    memset(&m, 0, sizeof(m));
    m.lctrl = true; m.shift = true;
    CHECK(far2l_ctrl_state(&m, false) == 0x0018, "left Ctrl+Shift");
    m.lctrl = false; m.rctrl = true;
    CHECK(far2l_ctrl_state(&m, false) == 0x0014, "right Ctrl+Shift");
    m.lctrl = true;
    CHECK(far2l_ctrl_state(&m, false) == 0x001C, "both Ctrl+Shift");
    m.lalt = true; m.ralt = true;
    CHECK(far2l_ctrl_state(&m, true) == 0x011F, "all modifiers, enhanced");

    /* AltGr: Windows reports it as left Ctrl + right Alt, and the key carries
     * the character the layout gives (German AltGr+Q = '@'). That is what a
     * Windows console reports too, and what is sent. */
    memset(&m, 0, sizeof(m));
    m.lctrl = true; m.ralt = true; m.numlock = true;
    c = far2l_ctrl_state(&m, false);
    CHECK(c == 0x0029, "AltGr = left Ctrl + right Alt (+ NumLock)");
    {
        Far2lKeyEvent e = { true, '@', c, 0x10, 'Q', 1 };
        roundtrip_key(&e, true, true, "AltGr+Q compact");
        roundtrip_key(&e, false, false, "AltGr+Q full");
        CHECK(!far2l_key_is_paste_gesture('V', c), "AltGr+V is no paste");
    }
}

/* ---- mouse ---- */

static void roundtrip_mouse(const Far2lMouseEvent *ev, bool compact_ok,
                            bool want_compact, const char *what)
{
    char out[F2L_EVENT_MAX];
    char msg[160];
    Stk s;
    size_t n = far2l_encode_mouse(ev, compact_ok, out, sizeof(out));
    uint32_t flags, ctrl, btn, x, y;
    int code;

    snprintf(msg, sizeof(msg), "%s: framed", what);
    CHECK(n > 0 && unframe(out, n, &s), msg);
    code = (int)pop(&s, 1);
    snprintf(msg, sizeof(msg), "%s: code", what);
    CHECK(code == (want_compact ? 'm' : 'M'), msg);
    if (want_compact) {
        flags = pop(&s, 1); ctrl = pop(&s, 1);
        btn = far2l_buttons_unsqueeze((uint16_t)pop(&s, 2));
    } else {
        flags = pop(&s, 4); ctrl = pop(&s, 4); btn = pop(&s, 4);
    }
    y = pop(&s, 2); x = pop(&s, 2);
    snprintf(msg, sizeof(msg), "%s: fields", what);
    CHECK(!s.bad && s.n == 0 && flags == ev->flags && ctrl == ev->ctrl &&
          btn == ev->buttons && (int16_t)y == ev->y && (int16_t)x == ev->x, msg);
}

static void test_mouse(void)
{
    Far2lMouseEvent e;
    static const uint32_t buttons[] = {
        F2L_BUTTON_LEFT, F2L_BUTTON_RIGHT, F2L_BUTTON_MIDDLE,
        F2L_BUTTON_X1, F2L_BUTTON_X2,
        F2L_BUTTON_LEFT | F2L_BUTTON_RIGHT | F2L_BUTTON_MIDDLE,
    };
    size_t i;

    for (i = 0; i < sizeof(buttons) / sizeof(*buttons); i++) {
        e = (Far2lMouseEvent){ 0, 0, buttons[i], 3, 7 };
        roundtrip_mouse(&e, true, true, "button press compact");
        roundtrip_mouse(&e, false, false, "button press full");
    }
    /* Release: no bit set. */
    e = (Far2lMouseEvent){ 0, 0, 0, 3, 7 };
    roundtrip_mouse(&e, true, true, "release");

    /* Move with a button held, modifiers in the control state. */
    e = (Far2lMouseEvent){ F2L_MOUSE_MOVED, F2L_LEFT_CTRL_PRESSED | F2L_SHIFT_PRESSED,
                           F2L_BUTTON_LEFT, 199, 59 };
    roundtrip_mouse(&e, true, true, "drag with Ctrl+Shift");

    e = (Far2lMouseEvent){ F2L_DOUBLE_CLICK, 0, F2L_BUTTON_LEFT, 0, 0 };
    roundtrip_mouse(&e, true, true, "double click");

    /* Enhanced in the mouse control state does not fit 8 bits. */
    e = (Far2lMouseEvent){ 0, F2L_ENHANCED_KEY, F2L_BUTTON_LEFT, 1, 1 };
    roundtrip_mouse(&e, true, false, "control state 0x100 -> full");

    /* Wheel: up fits compact, down does not (bits 24..31 of the delta). */
    e = (Far2lMouseEvent){ F2L_MOUSE_WHEELED, 0,
                           far2l_wheel_buttons(0, F2L_WHEEL_NOTCH), 4, 2 };
    CHECK(e.buttons == 0x00780000u, "wheel up +120");
    roundtrip_mouse(&e, true, true, "wheel up compact");
    e.buttons = far2l_wheel_buttons(0, -F2L_WHEEL_NOTCH);
    CHECK(e.buttons == 0xFF880000u, "wheel down -120");
    roundtrip_mouse(&e, true, false, "wheel down full");
    CHECK((int16_t)(e.buttons >> 16) < 0, "wheel down sign negative");

    /* Horizontal wheel, with a held button in the low word. */
    e = (Far2lMouseEvent){ F2L_MOUSE_HWHEELED, 0,
                           far2l_wheel_buttons(F2L_BUTTON_LEFT, F2L_WHEEL_NOTCH), 0, 0 };
    CHECK(e.buttons == 0x00780001u, "hwheel right with left held");
    roundtrip_mouse(&e, true, true, "hwheel compact");

    /* Squeeze round trip over every compact-able value. */
    for (i = 0; i < 0x10000; i++) {
        uint32_t st = (uint32_t)(i & 0xFF) | ((uint32_t)(i & 0xFF00) << 8);
        if (far2l_buttons_unsqueeze(far2l_buttons_squeeze(st)) != st) {
            CHECK(0, "squeeze round trip");
            break;
        }
    }
    CHECK(far2l_buttons_squeeze(0x00FF00FFu) == 0xFFFF, "squeeze keeps bits 0-7 and 16-23");

    /* Negative coordinates survive (a drag above the window). */
    e = (Far2lMouseEvent){ F2L_MOUSE_MOVED, 0, F2L_BUTTON_LEFT, -1, -3 };
    roundtrip_mouse(&e, false, false, "negative cell");
}

/* ---- the rest ---- */

static void test_misc(void)
{
    uint16_t s[3];
    int used;

    CHECK(far2l_key_scan(0x09, 0x0F) == 0, "Tab scan 0");
    CHECK(far2l_key_scan(0x08, 0x0E) == 0, "Backspace scan 0");
    CHECK(far2l_key_scan(0x1B, 0x01) == 0, "Escape scan 0");
    CHECK(far2l_key_scan(0x2E, 0x53) == 0, "Delete scan 0");
    CHECK(far2l_key_scan(0x25, 0x4B) == 0 && far2l_key_scan(0x26, 0x48) == 0 &&
          far2l_key_scan(0x27, 0x4D) == 0 && far2l_key_scan(0x28, 0x50) == 0,
          "arrows scan 0");
    CHECK(far2l_key_scan('A', 0x1E) == 0x1E, "letters keep the scan code");
    CHECK(far2l_key_scan(0x70, 0x3B) == 0x3B, "F1 keeps the scan code");
    CHECK(far2l_key_scan(0x10, 0x36) == 0x36, "right Shift keeps 0x36");

    s[0] = 0xD83D; s[1] = 0xDE00;
    CHECK(far2l_utf16_next(s, 2, &used) == 0x1F600 && used == 2, "surrogate pair");
    s[0] = 0x00E9;
    CHECK(far2l_utf16_next(s, 1, &used) == 0xE9 && used == 1, "BMP char");
    s[0] = 0xD83D;
    CHECK(far2l_utf16_next(s, 1, &used) == 0xD83D && used == 1, "lone high surrogate");
    CHECK(far2l_utf16_next(s, 0, &used) == 0 && used == 0, "empty run");

    CHECK(far2l_key_is_paste_gesture('V', F2L_LEFT_CTRL_PRESSED), "Ctrl+V");
    CHECK(far2l_key_is_paste_gesture('V', F2L_RIGHT_CTRL_PRESSED | F2L_SHIFT_PRESSED),
          "Ctrl+Shift+V");
    CHECK(far2l_key_is_paste_gesture(0x2D, F2L_SHIFT_PRESSED | F2L_NUMLOCK_ON), "Shift+Ins");
    CHECK(!far2l_key_is_paste_gesture('V', 0), "V alone");
    CHECK(!far2l_key_is_paste_gesture('V', F2L_LEFT_CTRL_PRESSED | F2L_LEFT_ALT_PRESSED),
          "Ctrl+Alt+V");
    CHECK(!far2l_key_is_paste_gesture(0x2D, F2L_SHIFT_PRESSED | F2L_LEFT_CTRL_PRESSED),
          "Ctrl+Shift+Ins");
    CHECK(!far2l_key_is_paste_gesture(0x2D, 0), "Ins alone");
}

int main(void)
{
    test_golden();
    test_key_fields();
    test_ctrl_state();
    test_mouse();
    test_misc();
    if (failures) {
        printf("test_far2l_keys: %d FAILED\n", failures);
        return 1;
    }
    printf("test_far2l_keys: all passed\n");
    return 0;
}
