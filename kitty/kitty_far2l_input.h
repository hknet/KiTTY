/*
 * kitty_far2l_input.h - far2l terminal extensions: key and mouse EVENTS
 * (terminal -> host), the encoding half.
 *
 * Once a far2l client has activated the extensions (ESC _ far2l1 ESC \,
 * answered ESC _ far2lok BEL), the terminal sends keyboard and mouse input as
 * events instead of escape sequences:
 *
 *     ESC _ f2l <base64 of a stack> BEL
 *
 * No colon after "f2l": far2l's base64 decoder stops at the first character
 * outside the alphabet, so "f2l:" would decode as an empty stack. The stack is
 * a byte array whose LAST byte is popped first; integers are little-endian.
 * The fields are those of the Win32 KEY_EVENT_RECORD / MOUSE_EVENT_RECORD,
 * because far2l is built on a layer that emulates the Windows console.
 *
 *   'K' / 'k'  key down / up, full form (stack, first byte first):
 *              repeat u16, vk u16, scan u16, control state u32, char u32, code
 *   'C' / 'c'  key down / up, compact form, only once the client asked for
 *              compact input (request 'x', feature bit 1) and only if nothing
 *              is lost: vk u8, control state u16, char u16, code
 *   'M'        mouse, full form:
 *              x i16, y i16, buttons u32, control state u32, flags u32, code
 *   'm'        mouse, compact form (same condition as 'C'):
 *              x i16, y i16, buttons squeezed to u16, control u8, flags u8, code
 *
 * Everything here is plain C with no Windows headers, so the unit test builds
 * it on its own (test/test_far2l_keys.c). The window code gathers the fields
 * from the Win32 messages and calls these functions.
 *
 * One translation unit defines KITTY_FAR2L_INPUT_IMPL before including this
 * file to get the function bodies (windows/window.c, and the test).
 */
#ifndef KITTY_FAR2L_INPUT_H
#define KITTY_FAR2L_INPUT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* dwControlKeyState bits (the Windows console values). */
#define F2L_RIGHT_ALT_PRESSED   0x0001u
#define F2L_LEFT_ALT_PRESSED    0x0002u
#define F2L_RIGHT_CTRL_PRESSED  0x0004u
#define F2L_LEFT_CTRL_PRESSED   0x0008u
#define F2L_SHIFT_PRESSED       0x0010u
#define F2L_NUMLOCK_ON          0x0020u
#define F2L_SCROLLLOCK_ON       0x0040u
#define F2L_CAPSLOCK_ON         0x0080u
#define F2L_ENHANCED_KEY        0x0100u

/* dwEventFlags of a mouse event; 0 = a button went down or up. */
#define F2L_MOUSE_MOVED         0x0001u
#define F2L_DOUBLE_CLICK        0x0002u
#define F2L_MOUSE_WHEELED       0x0004u
#define F2L_MOUSE_HWHEELED      0x0008u

/* dwButtonState low bits. */
#define F2L_BUTTON_LEFT         0x0001u   /* FROM_LEFT_1ST_BUTTON_PRESSED */
#define F2L_BUTTON_RIGHT        0x0002u   /* RIGHTMOST_BUTTON_PRESSED */
#define F2L_BUTTON_MIDDLE       0x0004u   /* FROM_LEFT_2ND_BUTTON_PRESSED */
#define F2L_BUTTON_X1           0x0008u   /* FROM_LEFT_3RD_BUTTON_PRESSED */
#define F2L_BUTTON_X2           0x0010u   /* FROM_LEFT_4TH_BUTTON_PRESSED */

/* Feature bits of the client's request 'x' (FARTTY_FEAT_*). */
#define F2L_FEAT_COMPACT_INPUT  0x00000001u
#define F2L_FEAT_TERMINAL_SIZE  0x00000002u

/* The scan code of the right Shift key: the compact key form cannot carry it
 * (the client rebuilds the scan code from the virtual key, which is VK_SHIFT
 * for both Shift keys). */
#define F2L_RIGHT_SHIFT_VSC     0x36u

/* One wheel notch, as Windows reports it (WHEEL_DELTA). far2l looks only at
 * the sign of the delta. */
#define F2L_WHEEL_NOTCH         120

/* A whole event on the wire is at most 5 + 24 + 1 bytes; room to spare. */
#define F2L_EVENT_MAX           48

typedef struct Far2lKeyEvent {
    bool down;              /* 'K'/'C' when true, 'k'/'c' when false */
    uint32_t uchar;         /* the character as a code point, or 0 */
    uint32_t ctrl;          /* F2L_* control key state */
    uint16_t vsc;           /* virtual scan code */
    uint16_t vk;            /* virtual key code */
    uint16_t repeat;        /* repeat count */
} Far2lKeyEvent;

typedef struct Far2lMouseEvent {
    uint32_t flags;         /* F2L_MOUSE_* / F2L_DOUBLE_CLICK, or 0 */
    uint32_t ctrl;          /* F2L_* control key state */
    uint32_t buttons;       /* F2L_BUTTON_* held, wheel delta in the high 16 bits */
    int16_t x, y;           /* cell column and row, counted from 0 */
} Far2lMouseEvent;

/* The modifier and lock state, as the window sees it, turned into the
 * control key state word. */
typedef struct Far2lModifiers {
    bool lctrl, rctrl, lalt, ralt, shift;
    bool numlock, scrolllock, capslock;
} Far2lModifiers;

uint32_t far2l_ctrl_state(const Far2lModifiers *m, bool enhanced);

/* The scan code to report for a key. Tab, Backspace, Escape, Delete and the
 * four arrows are reported with scan code 0, as the PuTTY forks with far2l
 * support have always sent them: they did it for far2l's editor
 * autocompletion and its Alt+arrow handling, and far2l users have relied on
 * that behaviour since. Every other key carries its hardware scan code. */
uint16_t far2l_key_scan(uint16_t vk, uint16_t hw_scan);

/* The first code point of a UTF-16 run as ToUnicode produced it (a surrogate
 * pair becomes one code point). *used gets the number of units consumed (0
 * when n <= 0). An unpaired surrogate is passed through as is. */
uint32_t far2l_utf16_next(const uint16_t *s, int n, int *used);

/* Ctrl+V and Shift+Insert: the keys far2l itself treats as a paste. The read
 * gate of the far2l clipboard opens on them, as upstream far2l does it. */
bool far2l_key_is_paste_gesture(uint16_t vk, uint32_t ctrl);

/* The compact key/mouse forms are allowed only when nothing is lost. */
bool far2l_key_fits_compact(const Far2lKeyEvent *ev);
bool far2l_mouse_fits_compact(const Far2lMouseEvent *ev);

/* The 32-bit button state squeezed into the compact 16 bits, and back. */
uint16_t far2l_buttons_squeeze(uint32_t state);
uint32_t far2l_buttons_unsqueeze(uint16_t v);

/* The button state of a wheel event: the held buttons, and the signed delta
 * in the high 16 bits. */
uint32_t far2l_wheel_buttons(uint32_t held, int delta);

/* Build the stack of one event (bottom byte first). compact_ok = the client
 * asked for compact input; the compact form is still used only when the
 * event fits it. Returns the stack length (at most 17). */
size_t far2l_key_stack(const Far2lKeyEvent *ev, bool compact_ok,
                       unsigned char *stk);
size_t far2l_mouse_stack(const Far2lMouseEvent *ev, bool compact_ok,
                         unsigned char *stk);

/* Frame a stack as ESC _ f2l <base64> BEL into out (NUL-terminated). Returns
 * the number of bytes written, excluding the NUL; 0 if out is too small. */
size_t far2l_frame_event(const unsigned char *stk, size_t n,
                         char *out, size_t outsz);

/* The two in one: the complete wire bytes of a key / mouse event. */
size_t far2l_encode_key(const Far2lKeyEvent *ev, bool compact_ok,
                        char *out, size_t outsz);
size_t far2l_encode_mouse(const Far2lMouseEvent *ev, bool compact_ok,
                          char *out, size_t outsz);

#ifdef KITTY_FAR2L_INPUT_IMPL

uint32_t far2l_ctrl_state(const Far2lModifiers *m, bool enhanced)
{
    uint32_t c = 0;
    if (m->ralt)       c |= F2L_RIGHT_ALT_PRESSED;
    if (m->lalt)       c |= F2L_LEFT_ALT_PRESSED;
    if (m->rctrl)      c |= F2L_RIGHT_CTRL_PRESSED;
    if (m->lctrl)      c |= F2L_LEFT_CTRL_PRESSED;
    if (m->shift)      c |= F2L_SHIFT_PRESSED;
    if (m->numlock)    c |= F2L_NUMLOCK_ON;
    if (m->scrolllock) c |= F2L_SCROLLLOCK_ON;
    if (m->capslock)   c |= F2L_CAPSLOCK_ON;
    if (enhanced)      c |= F2L_ENHANCED_KEY;
    return c;
}

uint16_t far2l_key_scan(uint16_t vk, uint16_t hw_scan)
{
    switch (vk) {
      case 0x09: /* VK_TAB */
      case 0x08: /* VK_BACK */
      case 0x1B: /* VK_ESCAPE */
      case 0x2E: /* VK_DELETE */
      case 0x25: /* VK_LEFT */
      case 0x26: /* VK_UP */
      case 0x27: /* VK_RIGHT */
      case 0x28: /* VK_DOWN */
        return 0;
      default:
        return hw_scan;
    }
}

uint32_t far2l_utf16_next(const uint16_t *s, int n, int *used)
{
    if (n <= 0) {
        *used = 0;
        return 0;
    }
    if (s[0] >= 0xD800 && s[0] < 0xDC00 && n >= 2 &&
        s[1] >= 0xDC00 && s[1] < 0xE000) {
        *used = 2;
        return 0x10000u + (((uint32_t)s[0] - 0xD800u) << 10) +
               ((uint32_t)s[1] - 0xDC00u);
    }
    *used = 1;
    return s[0];
}

bool far2l_key_is_paste_gesture(uint16_t vk, uint32_t ctrl)
{
    bool c = (ctrl & (F2L_LEFT_CTRL_PRESSED | F2L_RIGHT_CTRL_PRESSED)) != 0;
    bool a = (ctrl & (F2L_LEFT_ALT_PRESSED | F2L_RIGHT_ALT_PRESSED)) != 0;
    bool s = (ctrl & F2L_SHIFT_PRESSED) != 0;
    if (vk == 'V' && c && !a)
        return true;
    if (vk == 0x2D /* VK_INSERT */ && s && !c && !a)
        return true;
    return false;
}

bool far2l_key_fits_compact(const Far2lKeyEvent *ev)
{
    return ev->repeat <= 1 && ev->vsc != F2L_RIGHT_SHIFT_VSC &&
           ev->uchar < 0x10000u && ev->ctrl < 0x10000u && ev->vk < 0x100u;
}

bool far2l_mouse_fits_compact(const Far2lMouseEvent *ev)
{
    return (ev->buttons & 0xFF00FF00u) == 0 && ev->ctrl < 0x100u &&
           ev->flags < 0x100u;
}

uint16_t far2l_buttons_squeeze(uint32_t state)
{
    return (uint16_t)((state & 0xFFu) | ((state >> 8) & 0xFF00u));
}

uint32_t far2l_buttons_unsqueeze(uint16_t v)
{
    return (uint32_t)(v & 0xFFu) | (((uint32_t)v & 0xFF00u) << 8);
}

uint32_t far2l_wheel_buttons(uint32_t held, int delta)
{
    return (held & 0xFFFFu) | ((uint32_t)(uint16_t)(int16_t)delta << 16);
}

static size_t f2l_put16(unsigned char *p, uint16_t v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)(v >> 8);
    return 2;
}

static size_t f2l_put32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)(v >> 24);
    return 4;
}

size_t far2l_key_stack(const Far2lKeyEvent *ev, bool compact_ok,
                       unsigned char *stk)
{
    size_t n = 0;
    /* Pushed in the reverse of the order the client pops them: the code
     * goes last, on top. */
    if (compact_ok && far2l_key_fits_compact(ev)) {
        stk[n++] = (unsigned char)ev->vk;
        n += f2l_put16(stk + n, (uint16_t)ev->ctrl);
        n += f2l_put16(stk + n, (uint16_t)ev->uchar);
        stk[n++] = ev->down ? 'C' : 'c';
    } else {
        n += f2l_put16(stk + n, ev->repeat);
        n += f2l_put16(stk + n, ev->vk);
        n += f2l_put16(stk + n, ev->vsc);
        n += f2l_put32(stk + n, ev->ctrl);
        n += f2l_put32(stk + n, ev->uchar);
        stk[n++] = ev->down ? 'K' : 'k';
    }
    return n;
}

size_t far2l_mouse_stack(const Far2lMouseEvent *ev, bool compact_ok,
                         unsigned char *stk)
{
    size_t n = 0;
    n += f2l_put16(stk + n, (uint16_t)ev->x);
    n += f2l_put16(stk + n, (uint16_t)ev->y);
    if (compact_ok && far2l_mouse_fits_compact(ev)) {
        n += f2l_put16(stk + n, far2l_buttons_squeeze(ev->buttons));
        stk[n++] = (unsigned char)ev->ctrl;
        stk[n++] = (unsigned char)ev->flags;
        stk[n++] = 'm';
    } else {
        n += f2l_put32(stk + n, ev->buttons);
        n += f2l_put32(stk + n, ev->ctrl);
        n += f2l_put32(stk + n, ev->flags);
        stk[n++] = 'M';
    }
    return n;
}

size_t far2l_frame_event(const unsigned char *stk, size_t n,
                         char *out, size_t outsz)
{
    static const char b64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t need = 5 + 4 * ((n + 2) / 3) + 1 + 1;
    size_t o = 0, i;

    if (outsz < need)
        return 0;
    memcpy(out, "\x1b_f2l", 5);
    o = 5;
    for (i = 0; i + 2 < n; i += 3) {
        uint32_t v = ((uint32_t)stk[i] << 16) | ((uint32_t)stk[i+1] << 8) |
                     stk[i+2];
        out[o++] = b64[(v >> 18) & 63];
        out[o++] = b64[(v >> 12) & 63];
        out[o++] = b64[(v >> 6) & 63];
        out[o++] = b64[v & 63];
    }
    if (n - i == 1) {
        uint32_t v = (uint32_t)stk[i] << 16;
        out[o++] = b64[(v >> 18) & 63];
        out[o++] = b64[(v >> 12) & 63];
        out[o++] = '=';
        out[o++] = '=';
    } else if (n - i == 2) {
        uint32_t v = ((uint32_t)stk[i] << 16) | ((uint32_t)stk[i+1] << 8);
        out[o++] = b64[(v >> 18) & 63];
        out[o++] = b64[(v >> 12) & 63];
        out[o++] = b64[(v >> 6) & 63];
        out[o++] = '=';
    }
    out[o++] = '\x07';
    out[o] = '\0';
    return o;
}

size_t far2l_encode_key(const Far2lKeyEvent *ev, bool compact_ok,
                        char *out, size_t outsz)
{
    unsigned char stk[24];
    size_t n = far2l_key_stack(ev, compact_ok, stk);
    return far2l_frame_event(stk, n, out, outsz);
}

size_t far2l_encode_mouse(const Far2lMouseEvent *ev, bool compact_ok,
                          char *out, size_t outsz)
{
    unsigned char stk[24];
    size_t n = far2l_mouse_stack(ev, compact_ok, stk);
    return far2l_frame_event(stk, n, out, outsz);
}

#endif /* KITTY_FAR2L_INPUT_IMPL */

#endif /* KITTY_FAR2L_INPUT_H */
