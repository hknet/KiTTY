/*
 * kitty_switchnotice.c - one line at the top of the terminal area, for a
 * moment, when something switched:
 *  - OSC 3008: the active context runs as root (on the root frame's dark
 *    red) or as another user (on a dark yellow) - a word of why the frame
 *    and the tint just changed colour. Nothing when the context ends.
 *  - OSC 7501: one job became blocked, failed or finished while the status
 *    panel (two jobs or more) does not list it - in the panel's colours,
 *    with its state icon.
 * Both at once: the user switch on top, the job's line below it. Which text
 * and whether at all is decided platform-free (ctx_switch_notice,
 * ps_switch_notice); this file only shows it.
 *
 * The same kind of window as the status panel and the breadcrumb
 * (kitty_psoverlay.c, kitty_ctxcrumb.c; all three draw with
 * kitty_overlay_draw.c): a layered popup OWNED by the terminal window, so
 * Direct2D's presents never paint over it, above the terminal, hidden with
 * it. Unlike them it is click-through (WS_EX_TRANSPARENT) and never takes
 * the focus (WS_EX_NOACTIVATE): a line that is gone in two seconds must not
 * catch a click meant for the terminal. Per-pixel alpha about 95 % opaque,
 * rounded ends, a font 1.25 x the line's and 4 px above and below, so it
 * stands out from the red frame and tint; high contrast: the system
 * colours, opaque. It fades in (~150 ms), holds and fades out (~500 ms),
 * all within [KiTTY] osc3008ctxnotice / osc7501notice seconds; a timer runs
 * only during the fades. Its window text is the line, for a screen reader
 * and a harness.
 */
#include <winsock2.h>   /* before windows.h, as everywhere in kitty/ */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kitty_switchnotice.h"
#include "kitty_overlay_draw.h"   /* the canvas, the shape, the DPI, the font */
#include "kitty_psoverlay.h"      /* the panel's colours and its state icon */
#include "kitty_inikeys.h"

extern HWND MainHwnd;          /* kitty.c: the terminal window */
int ReadParameterN(const char *key, const char *name, char *value, size_t size);

#define SN_CLASS     "KiTTYSwitchNotice"
#define SN_TIMER     1
#define SN_FRAME_MS  16
#define SN_ALPHA     242       /* about 95 % */
#define SN_PAD_Y     4         /* px at 96 dpi, above and below the text */
#define SN_FADE_IN   150       /* ms */
#define SN_FADE_OUT  500
#define SN_MARGIN    6         /* from the terminal area's top, 96 dpi */
#define SN_GAP       4         /* between the two lines, 96 dpi */
#define SN_TEXT_MAX  512

typedef struct {
    HWND hwnd;
    KovDib dib;                /* the line, premultiplied */
    double s;                  /* the scale it was drawn at */
    int look, state;
    wchar_t text[SN_TEXT_MAX];
    DWORD start;               /* GetTickCount when it came */
    int total, fin, fout;      /* ms: the whole time and the two fades */
    bool live;                 /* inside its time */
} SnLine;

static SnLine sn[KSN_SLOTS];
static int sn_line_px;
static HFONT sn_font;
static int sn_font_px;

int kitty_switchnotice_seconds(const char *key)
{
    char buf[16] = "";
    char *end;
    long v = 2;
    if (ReadParameterN(KI_SECTION_KITTY, key, buf, sizeof(buf)) && buf[0]) {
        long n = strtol(buf, &end, 10);
        if (end != buf)
            v = n;
    }
    if (v < 0) v = 0;
    if (v > 10) v = 10;
    return (int)v;
}

static bool sn_owner_shown(void)
{
    return MainHwnd && IsWindowVisible(MainHwnd) && !IsIconic(MainHwnd);
}

static void sn_wide(wchar_t *dst, int n, const char *src)
{
    int k = MultiByteToWideChar(CP_UTF8, 0, src, -1, NULL, 0);
    wchar_t *tmp;
    dst[0] = 0;
    if (k <= 0)
        return;
    if (k <= n) {
        MultiByteToWideChar(CP_UTF8, 0, src, -1, dst, n);
        return;
    }
    tmp = (wchar_t *)malloc(sizeof(wchar_t) * (size_t)k);
    if (!tmp)
        return;
    MultiByteToWideChar(CP_UTF8, 0, src, -1, tmp, k);
    wcsncpy(dst, tmp, (size_t)n - 1);
    dst[n - 1] = 0;
    free(tmp);
}

static HFONT sn_get_font(int px)
{
    if (sn_font && sn_font_px == px)
        return sn_font;
    if (sn_font)
        DeleteObject(sn_font);
    sn_font = kov_font(px, true);
    sn_font_px = px;
    return sn_font;
}

/* The colours of a look: bright red with white text and edge for root,
 * bright yellow with black for a switched user (both must stand out from
 * the red frame and tint), or the status panel's own; the system colours
 * in high contrast, opaque. */
static void sn_look(int look, COLORREF *bg, COLORREF *fg, COLORREF *edge,
                    int *alpha)
{
    bool hc = kov_high_contrast();
    *alpha = hc ? 255 : SN_ALPHA;
    if (look == KSN_LOOK_STATUS || look == KSN_LOOK_PROGRESS) {
        kitty_psoverlay_look(bg, fg, edge);   /* high contrast there too */
        return;
    }
    if (hc) {
        *bg = GetSysColor(COLOR_HIGHLIGHT);
        *fg = *edge = GetSysColor(COLOR_HIGHLIGHTTEXT);
        return;
    }
    if (look == KSN_LOOK_ROOT) {
        *bg = RGB(220, 38, 38);
        *fg = *edge = RGB(255, 255, 255);
    } else {
        *bg = RGB(250, 204, 21);
        *fg = *edge = RGB(0, 0, 0);
    }
}

/* The line drawn into its canvas at the terminal's DPI and line height. */
static bool sn_render(int i)
{
    SnLine *n = &sn[i];
    double s = kov_dpi(MainHwnd) / 96.0;
    COLORREF bg, fg, edge;
    int alpha, line, h, w, maxw, pad, icon = 0, left, tw = 0;
    HDC sdc;
    HFONT f, oldf;
    SIZE ts;
    RECT cr, r;
    KovCanvas cv;

    sn_look(n->look, &bg, &fg, &edge, &alpha);
    line = sn_line_px > (int)(18 * s + .5) ? sn_line_px : (int)(18 * s + .5);
    h = line + 2 * (int)(SN_PAD_Y * s + .5);
    pad = h / 2;                       /* the rounded ends */
    f = sn_get_font(line * 62 / 100 * 125 / 100);
    if (n->look == KSN_LOOK_STATUS || n->look == KSN_LOOK_PROGRESS) {
        icon = (int)(h * 0.7 + .5);
        left = (h - icon) / 2 + icon + (int)(6 * s + .5);
    } else {
        left = pad;
    }
    sdc = GetDC(NULL);
    oldf = (HFONT)SelectObject(sdc, f);
    if (GetTextExtentPoint32W(sdc, n->text, (int)wcslen(n->text), &ts))
        tw = ts.cx;
    SelectObject(sdc, oldf);
    ReleaseDC(NULL, sdc);
    w = left + tw + pad;
    GetClientRect(MainHwnd, &cr);
    maxw = cr.right - 2 * (int)(SN_MARGIN * s + .5);
    if (w > maxw && maxw > 4 * h)
        w = maxw;                      /* the text ends in "..." then */

    if (!kov_dib(&n->dib, w, h))
        return false;
    cv.px = n->dib.px;
    cv.w = w;
    cv.h = h;
    kov_fill(&cv, bg);
    if (icon)
        kitty_psoverlay_icon(&cv, n->state, (h - icon) / 2.0, (h - icon) / 2.0, icon);
    if (edge != bg)
        kov_rrect_edge(&cv, 0, 0, w, h, h / 2.0, 1 * s, edge);
    GdiFlush();
    oldf = (HFONT)SelectObject(n->dib.dc, f);
    SetBkMode(n->dib.dc, TRANSPARENT);
    SetTextColor(n->dib.dc, fg);
    SetRect(&r, left, 0, w - pad, h);
    DrawTextW(n->dib.dc, n->text, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE |
              DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(n->dib.dc, oldf);
    GdiFlush();
    kov_premultiply_rrect(&cv, h / 2.0, alpha);
    n->s = s;
    return true;
}

/* Top centre of the terminal area; the job's line below the user's. */
static void sn_place(int i, POINT *pt)
{
    RECT cr;
    POINT tl = { 0, 0 };
    double s = sn[i].s;
    int y;
    GetClientRect(MainHwnd, &cr);
    ClientToScreen(MainHwnd, &tl);
    y = tl.y + (int)(SN_MARGIN * s + .5);
    if (i == KSN_PS7501 && sn[KSN_CTX3008].live && sn[KSN_CTX3008].dib.h)
        y += sn[KSN_CTX3008].dib.h + (int)(SN_GAP * s + .5);
    pt->x = tl.x + (cr.right - sn[i].dib.w) / 2;
    pt->y = y;
}

/* Placed, at f (0..1) of its opacity. */
static void sn_blend(int i, double f)
{
    SnLine *n = &sn[i];
    POINT pt, src = { 0, 0 };
    SIZE size;
    BLENDFUNCTION bf;
    if (!n->hwnd || !n->dib.dc)
        return;
    sn_place(i, &pt);
    size.cx = n->dib.w;
    size.cy = n->dib.h;
    bf.BlendOp = AC_SRC_OVER;
    bf.BlendFlags = 0;
    bf.SourceConstantAlpha = (BYTE)(f <= 0 ? 0 : f >= 1 ? 255 : (int)(f * 255 + .5));
    bf.AlphaFormat = AC_SRC_ALPHA;
    UpdateLayeredWindow(n->hwnd, NULL, &pt, &size, n->dib.dc, &src, 0, &bf, ULW_ALPHA);
}

static void sn_tick(int i);

static void sn_hide(int i)
{
    SnLine *n = &sn[i];
    bool was = n->live;
    n->live = false;
    if (n->hwnd) {
        KillTimer(n->hwnd, SN_TIMER);
        if (IsWindowVisible(n->hwnd))
            ShowWindow(n->hwnd, SW_HIDE);
    }
    if (was && i == KSN_CTX3008 && sn[KSN_PS7501].live)
        sn_tick(KSN_PS7501);           /* the job's line moves up */
}

/* This moment of its time: fading in, holding (no frames: one timer to the
 * end of the hold) or fading out; gone at the end. */
static void sn_tick(int i)
{
    SnLine *n = &sn[i];
    int e;
    double f;
    UINT next = SN_FRAME_MS;
    if (!n->live)
        return;
    e = (int)(GetTickCount() - n->start);
    if (e >= n->total) {
        sn_hide(i);
        return;
    }
    if (e < n->fin) {
        f = (double)e / n->fin;
    } else if (e < n->total - n->fout) {
        f = 1;
        next = (UINT)(n->total - n->fout - e);
    } else {
        f = (double)(n->total - e) / n->fout;
    }
    if (next < SN_FRAME_MS)
        next = SN_FRAME_MS;
    sn_blend(i, f);
    SetTimer(n->hwnd, SN_TIMER, next, NULL);
}

/* The fades inside ms: ~150 in, ~500 out; a short time shortens both, to
 * two thirds of it together at most. */
static void sn_times(SnLine *n, int ms)
{
    n->total = ms;
    n->fin = SN_FADE_IN;
    n->fout = SN_FADE_OUT;
    if ((n->fin + n->fout) * 3 > ms * 2) {
        n->fin = ms * 2 * SN_FADE_IN / (3 * (SN_FADE_IN + SN_FADE_OUT));
        n->fout = ms * 2 * SN_FADE_OUT / (3 * (SN_FADE_IN + SN_FADE_OUT));
        if (n->fin < 1) n->fin = 1;
        if (n->fout < 1) n->fout = 1;
    }
}

static LRESULT CALLBACK sn_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
      case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;          /* the terminal keeps the keyboard */
      case WM_NCHITTEST:
        return HTTRANSPARENT;          /* the clicks go to what is below */
      case WM_SHOWWINDOW:
        /* Windows hides an owned popup itself when the owner is minimised
         * and shows it again on the restore (SW_PARENTOPENING). A line
         * whose owner was minimised has ended (kitty_switchnotice_follow):
         * not shown again, with no timer left to take it away. */
        if (wp && lp == SW_PARENTOPENING) {
            int i = (int)GetWindowLongPtr(hwnd, GWLP_USERDATA);
            if (i < 0 || i >= KSN_SLOTS || !sn[i].live)
                return 0;
        }
        break;
      case WM_TIMER:
        if (wp == SN_TIMER) {
            int i = (int)GetWindowLongPtr(hwnd, GWLP_USERDATA);
            if (i >= 0 && i < KSN_SLOTS)
                sn_tick(i);
        }
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

static bool sn_create(int i)
{
    static bool registered;
    if (sn[i].hwnd)
        return true;
    if (!MainHwnd)
        return false;
    if (!registered) {
        WNDCLASSA wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = sn_wndproc;
        wc.hInstance = GetModuleHandle(NULL);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.lpszClassName = SN_CLASS;
        if (!RegisterClassA(&wc))
            return false;
        registered = true;
    }
    /* owned by the terminal window: above it, minimised with it, never in
     * the taskbar, never the focus, never the mouse */
    sn[i].hwnd = CreateWindowExA(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
                                 WS_EX_TRANSPARENT, SN_CLASS, "", WS_POPUP,
                                 0, 0, 1, 1, MainHwnd, NULL, GetModuleHandle(NULL), NULL);
    if (!sn[i].hwnd)
        return false;
    SetWindowLongPtr(sn[i].hwnd, GWLP_USERDATA, (LONG_PTR)i);
    return true;
}

void kitty_switchnotice_show(int slot, int look, int state, const char *text,
                             int secs, int line_px)
{
    SnLine *n;
    bool up;
    if (slot < 0 || slot >= KSN_SLOTS || secs <= 0 || !text || !*text)
        return;
    if (line_px > 0)
        sn_line_px = line_px;
    if (!sn_owner_shown() || !sn_create(slot))
        return;
    n = &sn[slot];
    sn_wide(n->text, SN_TEXT_MAX, text);
    n->look = look;
    n->state = state;
    up = n->live && IsWindowVisible(n->hwnd);
    sn_times(n, secs * 1000);
    /* a line already up takes the new text without a second fade in */
    n->start = GetTickCount() - (up ? (DWORD)n->fin : 0);
    n->live = true;
    if (!sn_render(slot)) {
        sn_hide(slot);
        return;
    }
    SetWindowTextW(n->hwnd, n->text);
    sn_tick(slot);                     /* placed, at this moment's opacity */
    if (n->live && !IsWindowVisible(n->hwnd))
        ShowWindow(n->hwnd, SW_SHOWNOACTIVATE);
    if (slot == KSN_CTX3008 && sn[KSN_PS7501].live)
        sn_tick(KSN_PS7501);           /* the job's line moves below it */
}

void kitty_switchnotice_line(int line_px)
{
    int i;
    if (line_px <= 0 || line_px == sn_line_px)
        return;
    sn_line_px = line_px;
    for (i = 0; i < KSN_SLOTS; i++)
        if (sn[i].live && sn_render(i))
            sn_tick(i);                /* placed again at its new size */
    if (sn[KSN_CTX3008].live && sn[KSN_PS7501].live)
        sn_tick(KSN_PS7501);           /* below the 3008 line's new height */
}

bool kitty_switchnotice_showing(int slot, int look)
{
    return slot >= 0 && slot < KSN_SLOTS && sn[slot].live &&
           sn[slot].look == look;
}

void kitty_switchnotice_follow(void)
{
    int i;
    for (i = 0; i < KSN_SLOTS; i++) {
        if (!sn[i].live)
            continue;
        if (!sn_owner_shown()) {
            sn_hide(i);
            continue;
        }
        sn_render(i);                  /* another DPI, another width */
        sn_tick(i);
    }
}

void kitty_switchnotice_owner_shown(bool shown)
{
    int i;
    if (shown)
        return;
    for (i = 0; i < KSN_SLOTS; i++)
        if (sn[i].live)
            sn_hide(i);
}

void kitty_switchnotice_restyle(void)
{
    int i;
    for (i = 0; i < KSN_SLOTS; i++)
        if (sn[i].live) {
            sn_render(i);
            sn_tick(i);
        }
}

void kitty_switchnotice_close(void)
{
    int i;
    for (i = 0; i < KSN_SLOTS; i++) {
        sn[i].live = false;
        if (sn[i].hwnd) {
            KillTimer(sn[i].hwnd, SN_TIMER);
            DestroyWindow(sn[i].hwnd);
            sn[i].hwnd = NULL;
        }
        kov_dib_free(&sn[i].dib);
    }
    if (sn_font)
        DeleteObject(sn_font);
    sn_font = NULL;
    sn_font_px = 0;
}
