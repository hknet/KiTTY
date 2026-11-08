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
 *  - The Event Log badge (KSN_EVLOG, windows/window.c): a small quiet
 *    marker at the top right of the terminal area, "Event Log: 2 new",
 *    below the status panel and the breadcrumb when they show there. It
 *    stays until the Event Log opens (kitty_switchnotice_badge, secs < 0)
 *    or for a number of seconds. A minimised or hidden window keeps it for
 *    the restore. Its tooltip holds the latest notable line. It is the one
 *    slot that takes a click: the click opens the Event Log
 *    (kitty_switchnotice_click). Never the focus either.
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
#include <commctrl.h>

#include "kitty_switchnotice.h"
#include "kitty_overlay_draw.h"   /* the canvas, the shape, the DPI, the font */
#include "kitty_psoverlay.h"      /* the panel's colours and its state icon */
#include "kitty_ctxcrumb.h"       /* the breadcrumb's rectangle: the badge keeps clear */
#include "kitty_theme.h"          /* kitty_theme_tooltip */
#include "kitty_win.h"            /* kitty_theme_app_dark */
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
#define SN_BADGE_ALPHA 224     /* the badge: about 88 % */
#define SN_PLACE_MS  1000      /* the badge looks for the panel and the breadcrumb */

typedef struct {
    HWND hwnd;
    KovDib dib;                /* the line, premultiplied */
    double s;                  /* the scale it was drawn at */
    int look, state;
    wchar_t text[SN_TEXT_MAX];
    DWORD start;               /* GetTickCount when it came */
    int total, fin, fout;      /* ms: the whole time and the two fades */
    bool live;                 /* inside its time */
    bool sticky;               /* the badge until the Event Log opens: no end */
} SnLine;

static SnLine sn[KSN_SLOTS];
static int sn_line_px;
void (*kitty_switchnotice_click)(int slot) = NULL;
static HFONT sn_font;
static int sn_font_px;
static HWND sn_tip;            /* the badge's tooltip (it keeps its own copy of the text) */
static POINT sn_badge_at;      /* where the badge was placed last */

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
    if (look == KSN_LOOK_STATUS || look == KSN_LOOK_PROGRESS ||
        look == KSN_LOOK_EVLOG) {
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
    int alpha, line, h, w, maxw, pad, icon = 0, left, tw = 0, row, padY;
    int nrows = 1, k;
    const wchar_t *rows[2] = { NULL, NULL };
    int rowlen[2] = { 0, 0 };
    HDC sdc;
    HFONT f, oldf;
    SIZE ts;
    RECT cr, r;
    KovCanvas cv;

    /* the rows of the text: one, or two at a '\n' (KSN_EVLOG) */
    rows[0] = n->text;
    rowlen[0] = (int)wcslen(n->text);
    {
        const wchar_t *nl = wcschr(n->text, L'\n');
        if (nl) {
            rowlen[0] = (int)(nl - n->text);
            rows[1] = nl + 1;
            rowlen[1] = (int)wcscspn(nl + 1, L"\n");
            nrows = 2;
        }
    }

    sn_look(n->look, &bg, &fg, &edge, &alpha);
    line = sn_line_px > (int)(18 * s + .5) ? sn_line_px : (int)(18 * s + .5);
    padY = (int)(SN_PAD_Y * s + .5);
    if (n->look == KSN_LOOK_EVLOG) {
        /* the badge: quiet - the terminal's own text size, one line, a
         * little more see-through */
        line = line * 80 / 100;
        padY = (int)(2 * s + .5);
        if (alpha != 255)
            alpha = SN_BADGE_ALPHA;
    }
    row = line + 2 * padY;
    h = nrows * line + 2 * padY;
    pad = row / 2;                     /* the rounded ends */
    f = sn_get_font(n->look == KSN_LOOK_EVLOG ? line * 80 / 100
                                              : line * 62 / 100 * 125 / 100);
    if (n->look == KSN_LOOK_STATUS || n->look == KSN_LOOK_PROGRESS) {
        icon = (int)(h * 0.7 + .5);
        left = (h - icon) / 2 + icon + (int)(6 * s + .5);
    } else {
        left = pad;
    }
    sdc = GetDC(NULL);
    oldf = (HFONT)SelectObject(sdc, f);
    for (k = 0; k < nrows; k++)
        if (GetTextExtentPoint32W(sdc, rows[k], rowlen[k], &ts) && ts.cx > tw)
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
        kov_rrect_edge(&cv, 0, 0, w, h, row / 2.0, 1 * s, edge);
    GdiFlush();
    oldf = (HFONT)SelectObject(n->dib.dc, f);
    SetBkMode(n->dib.dc, TRANSPARENT);
    SetTextColor(n->dib.dc, fg);
    for (k = 0; k < nrows; k++) {
        if (nrows == 1)
            SetRect(&r, left, 0, w - pad, h);
        else
            SetRect(&r, left, padY + k * line, w - pad, padY + (k + 1) * line);
        DrawTextW(n->dib.dc, rows[k], rowlen[k], &r, DT_LEFT | DT_VCENTER |
                  DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
    SelectObject(n->dib.dc, oldf);
    GdiFlush();
    kov_premultiply_rrect(&cv, row / 2.0, alpha);
    n->s = s;
    return true;
}

/* Top centre of the terminal area, the job's line below the user's. The
 * Event Log badge at the top right, below the status panel and the
 * breadcrumb where they show in the terminal area. */
static void sn_place(int i, POINT *pt)
{
    RECT cr;
    POINT tl = { 0, 0 };
    double s = sn[i].s;
    int y;
    GetClientRect(MainHwnd, &cr);
    ClientToScreen(MainHwnd, &tl);
    if (i == KSN_EVLOG) {
        int margin = (int)(SN_MARGIN * s + .5), gap = (int)(SN_GAP * s + .5);
        int pass;
        RECT r;
        pt->x = tl.x + cr.right - margin - sn[i].dib.w;
        pt->y = tl.y + margin;
        /* twice: the breadcrumb and the panel stack, either may be first */
        for (pass = 0; pass < 2; pass++) {
            if (kitty_ctxcrumb_rect(&r) && r.bottom > pt->y && r.top < pt->y + sn[i].dib.h &&
                r.left < pt->x + sn[i].dib.w && r.right > pt->x)
                pt->y = r.bottom + gap;
            if (kitty_psoverlay_rect(&r) && r.bottom > pt->y && r.top < pt->y + sn[i].dib.h &&
                r.left < pt->x + sn[i].dib.w && r.right > pt->x)
                pt->y = r.bottom + gap;
        }
        sn_badge_at = *pt;
        return;
    }
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
    n->sticky = false;
    if (n->hwnd) {
        KillTimer(n->hwnd, SN_TIMER);
        if (i == KSN_EVLOG && sn_tip)
            SendMessageW(sn_tip, TTM_POP, 0, 0);
        if (IsWindowVisible(n->hwnd))
            ShowWindow(n->hwnd, SW_HIDE);
    }
    if (was && i == KSN_CTX3008 && sn[KSN_PS7501].live)
        sn_tick(KSN_PS7501);           /* the job's line moves up */
}

/* The owner went (minimised, the tray): a line ends, the badge that stays
 * until the Event Log opens only leaves the screen until the restore. */
static void sn_owner_gone(int i)
{
    SnLine *n = &sn[i];
    if (!n->sticky) {
        sn_hide(i);
        return;
    }
    if (n->hwnd) {
        KillTimer(n->hwnd, SN_TIMER);
        if (sn_tip)
            SendMessageW(sn_tip, TTM_POP, 0, 0);
        if (IsWindowVisible(n->hwnd))
            ShowWindow(n->hwnd, SW_HIDE);
    }
}

/* This moment of its time: fading in, holding (no frames: one timer to the
 * end of the hold) or fading out; gone at the end. The badge that stays
 * fades in and holds, a slow timer keeps it clear of the status panel and
 * the breadcrumb when they come or go. */
static void sn_tick(int i)
{
    SnLine *n = &sn[i];
    int e;
    double f;
    UINT next = SN_FRAME_MS;
    if (!n->live)
        return;
    e = (int)(GetTickCount() - n->start);
    if (n->sticky) {
        if (e < n->fin) {
            sn_blend(i, (double)e / n->fin);
        } else {
            POINT was = sn_badge_at, now;
            sn_place(i, &now);
            /* the first hold frame, then only when the place changed */
            if (now.x != was.x || now.y != was.y || e < n->fin + SN_PLACE_MS)
                sn_blend(i, 1);
            next = SN_PLACE_MS;
        }
        SetTimer(n->hwnd, SN_TIMER, next, NULL);
        return;
    }
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
        /* the clicks go to what is below - but the Event Log notice's own */
        if ((int)GetWindowLongPtr(hwnd, GWLP_USERDATA) == KSN_EVLOG)
            return HTCLIENT;
        return HTTRANSPARENT;
      case WM_SETCURSOR:
        if ((int)GetWindowLongPtr(hwnd, GWLP_USERDATA) == KSN_EVLOG) {
            SetCursor(LoadCursor(NULL, IDC_HAND));
            return TRUE;
        }
        break;
      case WM_LBUTTONUP:
        if ((int)GetWindowLongPtr(hwnd, GWLP_USERDATA) == KSN_EVLOG) {
            sn_hide(KSN_EVLOG);
            if (kitty_switchnotice_click)
                kitty_switchnotice_click(KSN_EVLOG);
            return 0;
        }
        break;
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
     * the taskbar, never the focus, never the mouse - the Event Log notice
     * takes its click */
    sn[i].hwnd = CreateWindowExA(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
                                 (i == KSN_EVLOG ? 0 : WS_EX_TRANSPARENT),
                                 SN_CLASS, "", WS_POPUP,
                                 0, 0, 1, 1, MainHwnd, NULL, GetModuleHandle(NULL), NULL);
    if (!sn[i].hwnd)
        return false;
    SetWindowLongPtr(sn[i].hwnd, GWLP_USERDATA, (LONG_PTR)i);
    if (i == KSN_EVLOG) {
        /* the badge's tooltip: the latest notable line and what a click does */
        sn_tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL,
                                 WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                                 CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                 CW_USEDEFAULT, sn[i].hwnd, NULL, GetModuleHandle(NULL), NULL);
        if (sn_tip) {
            TOOLINFOW ti;
            memset(&ti, 0, sizeof(ti));
            ti.cbSize = sizeof(ti);
            ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
            ti.hwnd = sn[i].hwnd;
            ti.uId = (UINT_PTR)sn[i].hwnd;
            ti.lpszText = L"";
            SendMessageW(sn_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
            SendMessageW(sn_tip, TTM_SETMAXTIPWIDTH, 0, 600);
        }
    }
    return true;
}

/* The badge's tooltip text, in the theme. */
static void sn_tip_set(const char *tip)
{
    TOOLINFOW ti;
    wchar_t *w;
    int k;
    if (!sn_tip || !sn[KSN_EVLOG].hwnd)
        return;
    if (!tip)
        tip = "";
    k = MultiByteToWideChar(CP_UTF8, 0, tip, -1, NULL, 0);
    if (k <= 0 || !(w = (wchar_t *)malloc(sizeof(wchar_t) * (size_t)k)))
        return;
    MultiByteToWideChar(CP_UTF8, 0, tip, -1, w, k);
    memset(&ti, 0, sizeof(ti));
    ti.cbSize = sizeof(ti);
    ti.uFlags = TTF_IDISHWND;
    ti.hwnd = sn[KSN_EVLOG].hwnd;
    ti.uId = (UINT_PTR)sn[KSN_EVLOG].hwnd;
    ti.lpszText = w;
    SendMessageW(sn_tip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
    free(w);
    kitty_theme_tooltip(sn_tip, kitty_theme_app_dark());
}

void kitty_switchnotice_badge(const char *text, const char *tip, int secs,
                              int line_px)
{
    SnLine *n = &sn[KSN_EVLOG];
    bool up;
    if (secs == 0 || !text || !*text)
        return;
    if (line_px > 0)
        sn_line_px = line_px;
    if (!sn_create(KSN_EVLOG))
        return;
    sn_wide(n->text, SN_TEXT_MAX, text);
    n->look = KSN_LOOK_EVLOG;
    n->state = 0;
    up = n->live && IsWindowVisible(n->hwnd);
    n->sticky = secs < 0;
    sn_times(n, secs < 0 ? 10000 : secs * 1000);
    n->start = GetTickCount() - (up ? (DWORD)n->fin : 0);
    n->live = true;
    SetWindowTextW(n->hwnd, n->text);
    sn_tip_set(tip);
    if (!sn_owner_shown()) {
        if (!n->sticky)
            sn_hide(KSN_EVLOG);        /* a timed one is not kept for later */
        return;                        /* shown at the restore */
    }
    if (!sn_render(KSN_EVLOG)) {
        sn_hide(KSN_EVLOG);
        return;
    }
    sn_badge_at.x = sn_badge_at.y = -32768;
    sn_tick(KSN_EVLOG);
    if (n->live && !IsWindowVisible(n->hwnd))
        ShowWindow(n->hwnd, SW_SHOWNOACTIVATE);
}

/* A kept badge back on the screen (the owner restored or shown again). */
static void sn_badge_back(void)
{
    SnLine *n = &sn[KSN_EVLOG];
    if (!n->live || !n->sticky || !n->hwnd || !sn_owner_shown())
        return;
    if (!sn_render(KSN_EVLOG))
        return;
    n->start = GetTickCount() - (DWORD)n->fin;
    sn_badge_at.x = sn_badge_at.y = -32768;
    sn_tick(KSN_EVLOG);
    if (!IsWindowVisible(n->hwnd))
        ShowWindow(n->hwnd, SW_SHOWNOACTIVATE);
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

void kitty_switchnotice_hide(int slot)
{
    if (slot >= 0 && slot < KSN_SLOTS && sn[slot].live)
        sn_hide(slot);
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
            sn_owner_gone(i);
            continue;
        }
        if (sn[i].sticky && !IsWindowVisible(sn[i].hwnd)) {
            sn_badge_back();           /* restored: the kept badge again */
            continue;
        }
        sn_render(i);                  /* another DPI, another width */
        sn_tick(i);
    }
}

void kitty_switchnotice_owner_shown(bool shown)
{
    int i;
    if (shown) {
        sn_badge_back();               /* back from the tray: the kept badge */
        return;
    }
    for (i = 0; i < KSN_SLOTS; i++)
        if (sn[i].live)
            sn_owner_gone(i);
}

void kitty_switchnotice_restyle(void)
{
    int i;
    for (i = 0; i < KSN_SLOTS; i++)
        if (sn[i].live) {
            sn_render(i);
            sn_tick(i);
            if (i == KSN_EVLOG && sn_tip)
                kitty_theme_tooltip(sn_tip, kitty_theme_app_dark());
        }
}

void kitty_switchnotice_close(void)
{
    int i;
    if (sn_tip)
        DestroyWindow(sn_tip);
    sn_tip = NULL;
    for (i = 0; i < KSN_SLOTS; i++) {
        sn[i].live = false;
        sn[i].sticky = false;
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
