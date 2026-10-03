/*
 * kitty_far2l.c - the platform half of the far2l terminal extensions:
 * the Windows clipboard in far2l's formats, the modeless
 * permission box, and saving "Always allow this far2l" into the session.
 *
 * The protocol and every permission decision are in terminal/terminal.c, which
 * reaches this file only through the seams in kitty_far2l.h; the test targets
 * stub those seams, so no test touches the real clipboard.
 *
 * The clipboard is opened and closed again inside every call. far2l's own
 * server keeps it open from CLIP_OPEN to CLIP_CLOSE; here that would lock
 * every other program out of the clipboard for as long as a host chose.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "putty.h"
#include "terminal.h"

#include <windows.h>

#include "kitty_far2l.h"
#include "far2l/far2l_proto.h"
#include "kitty_text.h"     /* the box's words */
#include "kitty_dlgbox.h"   /* kitty_confirm_modeless_check: the shared box */

extern HWND MainHwnd;          /* kitty.c: the terminal window */

/* ------------------------------------------------------------------------
 * The clipboard
 * ------------------------------------------------------------------------ */

/* Another program may be mid-copy: retried briefly, as the OSC 52 side does. */
static bool f2l_open_clipboard(void)
{
    int attempt;
    for (attempt = 0; attempt < 10; attempt++) {
        if (OpenClipboard(NULL))
            return true;
        Sleep(20);
    }
    return false;
}

static UINT f2l_html_format(void)
{
    return RegisterClipboardFormatW(L"HTML Format");
}

/* far2l's number to the Windows one; 0 for a format we do not carry. */
static UINT f2l_win_format(uint32_t fmt)
{
    if (fmt == F2L_CF_TEXT || fmt == F2L_CF_UNICODETEXT)
        return CF_UNICODETEXT;
    if (fmt == F2L_CF_HTML)
        return f2l_html_format();
    if (fmt >= F2L_CF_REGISTERED && fmt <= 0xFFFF)
        return (UINT)fmt;
    return 0;
}

uint32_t kitty_far2l_clip_register(const char *name, size_t len)
{
    int cnt = MultiByteToWideChar(CP_UTF8, 0, name, (int)len, NULL, 0);
    wchar_t *w;
    UINT fmt = 0;
    if (cnt <= 0)
        return 0;
    w = snewn(cnt + 1, wchar_t);
    MultiByteToWideChar(CP_UTF8, 0, name, (int)len, w, cnt);
    w[cnt] = L'\0';
    fmt = RegisterClipboardFormatW(w);
    sfree(w);
    return fmt;
}

bool kitty_far2l_clip_available(uint32_t fmt)
{
    UINT wf = f2l_win_format(fmt);
    return wf && IsClipboardFormatAvailable(wf);
}

bool kitty_far2l_clip_empty(void)
{
    bool ok;
    if (!f2l_open_clipboard())
        return false;
    ok = EmptyClipboard() != 0;
    CloseClipboard();
    return ok;
}

static HGLOBAL f2l_hglobal(const void *data, size_t len)
{
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, len ? len : 1);
    void *p;
    if (!h)
        return NULL;
    if (!(p = GlobalLock(h))) {
        GlobalFree(h);
        return NULL;
    }
    if (len)
        memcpy(p, data, len);
    GlobalUnlock(h);
    return h;
}

bool kitty_far2l_clip_set(uint32_t fmt, const unsigned char *data, size_t len,
                          bool empty_first)
{
    UINT wf = f2l_win_format(fmt);
    HGLOBAL h = NULL;
    bool ok = false;

    if (!wf)
        return false;
    if (fmt == F2L_CF_TEXT) {
        /* UTF-8 in, UTF-16 out with the terminator the clipboard wants. A
         * terminator far2l sent along ends the text there. */
        size_t n = 0;
        int cnt;
        while (n < len && data[n])
            n++;
        cnt = n ? MultiByteToWideChar(CP_UTF8, 0, (LPCCH)data, (int)n, NULL, 0) : 0;
        if (n && cnt <= 0)
            return false;
        h = GlobalAlloc(GMEM_MOVEABLE, ((size_t)cnt + 1) * sizeof(wchar_t));
        if (h) {
            wchar_t *w = GlobalLock(h);
            if (w) {
                if (cnt)
                    MultiByteToWideChar(CP_UTF8, 0, (LPCCH)data, (int)n, w, cnt);
                w[cnt] = L'\0';
                GlobalUnlock(h);
            } else {
                GlobalFree(h);
                h = NULL;
            }
        }
    } else if (fmt == F2L_CF_UNICODETEXT) {
        size_t units;
        uint16_t *u = f2l_utf32_to_utf16(data, len, &units);
        h = f2l_hglobal(u, (units + 1) * sizeof(uint16_t));
        smemclr(u, (units + 1) * sizeof(uint16_t));
        sfree(u);
    } else if (fmt == F2L_CF_HTML) {
        size_t n;
        unsigned char *cf = f2l_html_wrap(data, len, &n);
        if (cf) {
            h = f2l_hglobal(cf, n);
            smemclr(cf, n);
            sfree(cf);
        }
    } else {
        h = f2l_hglobal(data, len);    /* a registered format: opaque bytes */
    }
    if (!h)
        return false;

    if (!f2l_open_clipboard()) {
        GlobalFree(h);
        return false;
    }
    if (empty_first && !EmptyClipboard()) {
        CloseClipboard();
        GlobalFree(h);
        return false;
    }
    if (SetClipboardData(wf, h))
        ok = true;                     /* the clipboard owns it now */
    else
        GlobalFree(h);
    CloseClipboard();
    return ok;
}

unsigned char *kitty_far2l_clip_get(uint32_t fmt, size_t *len)
{
    UINT wf = f2l_win_format(fmt);
    HANDLE h;
    const unsigned char *p;
    unsigned char *out = NULL;

    *len = 0;
    if (!wf || !IsClipboardFormatAvailable(wf) || !f2l_open_clipboard())
        return NULL;
    h = GetClipboardData(wf);
    if (h && (p = GlobalLock(h)) != NULL) {
        size_t size = GlobalSize(h);
        if (fmt == F2L_CF_TEXT) {
            /* UTF-16 to UTF-8, NUL-terminated as far2l tolerates */
            int n = (int)wcsnlen((const wchar_t *)p, size / sizeof(wchar_t));
            int need = n ? WideCharToMultiByte(CP_UTF8, 0, (const wchar_t *)p, n,
                                               NULL, 0, NULL, NULL) : 0;
            if (n > 0 && need > 0) {
                out = snewn((size_t)need + 1, unsigned char);
                WideCharToMultiByte(CP_UTF8, 0, (const wchar_t *)p, n,
                                    (char *)out, need, NULL, NULL);
                out[need] = '\0';
                *len = (size_t)need + 1;
            }
        } else if (fmt == F2L_CF_UNICODETEXT) {
            size_t n = wcsnlen((const wchar_t *)p, size / sizeof(wchar_t));
            if (n)
                out = f2l_utf16_to_utf32((const uint16_t *)p, n, len);
        } else if (fmt == F2L_CF_HTML) {
            out = f2l_html_unwrap(p, size, len);
            if (out && *len <= 1) {        /* an empty fragment is no data */
                sfree(out);
                out = NULL;
                *len = 0;
            }
        } else if (size) {
            /* GlobalSize is the block, which may be rounded up: an opaque
             * format carries its own length, so it goes as it is */
            out = snewn(size, unsigned char);
            memcpy(out, p, size);
            *len = size;
        }
        GlobalUnlock(h);
    }
    CloseClipboard();
    if (out && *len == 0) {
        sfree(out);
        out = NULL;
    }
    return out;
}

/* ------------------------------------------------------------------------
 * The permission box
 * ------------------------------------------------------------------------ */

/*
 * Modeless, on the shared confirmation box: Deny is the default, it sits on
 * the terminal window, and the terminal keeps running while it is open
 * (terminal.c holds the open request and everything after it). Opened and
 * answered from toplevel callbacks, never from inside the parser: creating a
 * window there sends focus messages into the terminal mid-parse, and the
 * answer goes back only once the box is gone and the focus is the terminal's
 * again.
 *
 * The holder outlives whichever comes last of the box and the two callbacks;
 * term = NULL means the terminal went first (kitty_far2l_confirm_end).
 */
typedef struct {
    Terminal *term;
    HWND box;
    bool offer_always;
    int yes, always;
} far2l_confirm_t;
static far2l_confirm_t *far2l_confirm_open_box = NULL;

static void far2l_confirm_apply(void *ctx)
{
    far2l_confirm_t *c = (far2l_confirm_t *)ctx;
    Terminal *term = c->term;
    bool yes = c->yes != 0, always = c->always != 0;
    if (far2l_confirm_open_box == c)
        far2l_confirm_open_box = NULL;
    sfree(c);
    if (term)
        term_far2l_open_answer(term, yes, always);
}

static void far2l_confirm_done(int yes, int checked, void *ctx)
{
    far2l_confirm_t *c = (far2l_confirm_t *)ctx;
    c->yes = yes;
    c->always = checked;
    c->box = NULL;                     /* closing: the box frees itself */
    queue_toplevel_callback(far2l_confirm_apply, c);
}

static void far2l_confirm_show(void *ctx)
{
    far2l_confirm_t *c = (far2l_confirm_t *)ctx;

    if (!c->term) {                    /* the terminal went before the box came */
        if (far2l_confirm_open_box == c)
            far2l_confirm_open_box = NULL;
        sfree(c);
        return;
    }
    c->box = kitty_confirm_modeless_check(MainHwnd, KT_CAP_KITTYPP,
                                          KT_CLIP_FAR2L_ALLOW_Q,
                                          KT_CLIP_FAR2L_ALLOW, KT_CLIP_FAR2L_DENY,
                                          c->offer_always ? KT_CLIP_FAR2L_ALWAYS : NULL,
                                          far2l_confirm_done, c);
    if (!c->box) {                     /* not made: that is a Deny */
        c->yes = 0;
        c->always = 0;
        far2l_confirm_apply(c);
    }
}

void kitty_far2l_confirm(Terminal *term, bool offer_always)
{
    far2l_confirm_t *c;

    if (far2l_confirm_open_box)
        return;                        /* one box (terminal.c holds the rest) */
    c = snew(far2l_confirm_t);
    c->term = term;
    c->box = NULL;
    c->offer_always = offer_always;
    c->yes = 0;
    c->always = 0;
    far2l_confirm_open_box = c;
    queue_toplevel_callback(far2l_confirm_show, c);
}

/* Close the box from a toplevel callback: _end is reached from inside the
 * parser (far2l0, a new far2l1, a terminal reset), and destroying a window
 * there hands the focus back to the terminal mid-parse. Callbacks run in
 * order, so this runs before any apply that a click queued meanwhile. */
static void far2l_confirm_destroy(void *ctx)
{
    far2l_confirm_t *c = (far2l_confirm_t *)ctx;
    if (c->box) {
        HWND box = c->box;
        c->box = NULL;
        DestroyWindow(box);            /* done(No) -> apply, which frees c */
    }
}

void kitty_far2l_confirm_end(Terminal *term)
{
    far2l_confirm_t *c = far2l_confirm_open_box;

    if (!c || c->term != term)
        return;
    c->term = NULL;                    /* whatever runs next finds no terminal */
    far2l_confirm_open_box = NULL;
    if (c->box)
        queue_toplevel_callback(far2l_confirm_destroy, c);
}

/* ------------------------------------------------------------------------
 * "Always allow this far2l"
 * ------------------------------------------------------------------------ */

/*
 * Into the saved session, through save_settings() like "always deny for this
 * host" of OSC 52: it honours the ini/registry backend, and it writes the
 * window's other live settings along with it - they are this session's. An
 * unnamed session or Default Settings has nothing to write into; the ID then
 * stays in this window's settings only.
 */
bool kitty_far2l_save_client_ids(Terminal *term)
{
    const char *name;
    char *err;

    if (!term || !term->conf)
        return false;
    name = conf_get_str(term->conf, CONF_sessionname);
    if (!name || !*name || strcmp(name, "Default Settings") == 0)
        return false;
    err = save_settings(name, term->conf);
    if (err) {
        sfree(err);
        return false;
    }
    return true;
}
