/*
 * kitty_ctxcrumb.c - the OSC 3008 breadcrumb.
 *
 * One line while the host's contexts (kitty_ctx3008.c, ctx_breadcrumb) show
 * that something has switched: "alice@web01 > root > nginx-test". The root
 * segment sits on the root frame's dark red, a switched user on a dark
 * yellow; the rest in the panel's own colours. Too long for its room, it
 * shows the first segment, "...", and the last two. Hidden while the chain
 * has a single segment (a plain login). Information, never a security
 * signal: it must not look like KiTTY++'s own trust prompts.
 *
 * Where ([KiTTY] ctxbreadcrumbplace):
 *  - title (the default): over the right end of the title bar, left of the
 *    caption buttons, at the caption's height. CLICK-THROUGH, so dragging the
 *    window and a double click on the caption still work there - which also
 *    means no drag of its own and no tooltip. It covers no terminal text, so
 *    it stays on the alternate screen. A window without a title bar (full
 *    screen, the title bar switched off) falls back to:
 *  - window: the top right of the terminal area. Dragged with the mouse, its
 *    place kept per session (ContextBreadcrumbPos, from the area's top-right
 *    corner); the whole chain in a tooltip; hidden on the alternate screen
 *    (vim, less, htop), where it would cover text. The program status panel
 *    shares that corner: it opens below the breadcrumb (kitty_ctxcrumb_rect).
 *
 * The same kind of window as the program status panel (kitty_psoverlay.c;
 * both draw with kitty_overlay_draw.c): a layered popup OWNED by the terminal
 * window, so Direct2D's presents never paint over it; per-pixel alpha via
 * UpdateLayeredWindow, [KiTTY] ctxbreadcrumbopacity percent (70 by
 * default); it never takes the focus. In high contrast: system colours,
 * opaque. Its window text is the whole chain, joined " > ", for a screen
 * reader and a harness.
 */
#include <winsock2.h>   /* before windows.h: putty.h pulls it in further down */
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "putty.h"
#include "kitty_ctxcrumb.h"
#include "kitty_overlay_draw.h"   /* the canvas and its rounded rectangles */
#include "kitty_psoverlay.h"      /* the status panel keeps clear of it */
#include "kitty_inikeys.h"
#include "kitty_text.h"
#include "kitty_win.h"        /* kitty_theme_app_dark */
#include "kitty_theme.h"      /* kitty_theme_tooltip */
#include "kitty_winpos.h"     /* the place, per session */
#include "kitty_oldwin.h"     /* kitty_api_from: GetDpiForWindow */

extern HWND MainHwnd;          /* kitty.c: the terminal window */
int ReadParameterN(const char *key, const char *name, char *value, size_t size);

#define KC_CLASS     "KiTTYContextCrumb"
#define KC_MARGIN    6         /* the default distance from the corner, 96 dpi */
#define KC_MAXSEG    CTX_DEPTH_MAX
#define WM_KC_SHOW   (WM_APP + 1)   /* to itself: the owner is visible now */

static HWND kc_hwnd, kc_tip;
static CtxSeg kc_seg[KC_MAXSEG];
static int kc_n;
static bool kc_due;
static bool kc_on, kc_alt;             /* the session's switch; the alternate screen */
static bool kc_title;                  /* placed over the title bar right now */
static char kc_session[256];
static bool kc_have_pos;
static int kc_dx = KC_MARGIN, kc_dy = KC_MARGIN;
static int kc_line_px = 16;
static wchar_t kc_chain[2048];         /* the whole chain: tooltip + window text */

/* the picture */
static HDC kc_dc;
static HBITMAP kc_bmp, kc_oldbmp;
static DWORD *kc_px;
static int kc_cw, kc_ch;
static double kc_s = 1;

/* ---- sizes, colours, fonts ---------------------------------------------- */

static UINT kc_dpi(void)
{
    typedef UINT (WINAPI *fn_t)(HWND);
    static fn_t fn;
    static int tried;
    UINT d = 0;
    if (!tried) {
        fn = (fn_t)kitty_api_from(GetModuleHandleA("user32.dll"), "user32.dll",
                                  "GetDpiForWindow", KITTY_API_OPTIONAL,
                                  KT_WINFEAT_DPI);
        tried = 1;
    }
    if (fn && MainHwnd)
        d = fn(MainHwnd);
    if (!d) {
        HDC dc = GetDC(NULL);
        if (dc) {
            d = GetDeviceCaps(dc, LOGPIXELSX);
            ReleaseDC(NULL, dc);
        }
    }
    return d ? d : 96;
}

static bool kc_high_contrast(void)
{
    HIGHCONTRASTA hc;
    memset(&hc, 0, sizeof(hc));
    hc.cbSize = sizeof(hc);
    return SystemParametersInfoA(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0) &&
           (hc.dwFlags & HCF_HIGHCONTRASTON);
}

typedef struct {
    COLORREF bg, fg, dim, line;
    COLORREF root_bg, sw_bg, chip_fg;  /* the root and switched-user segments */
    int alpha;
} KcTheme;

/* [KiTTY] ctxbreadcrumbopacity: percent, 40-100, 70 by default. */
static int kc_opacity(void)
{
    char buf[16] = "";
    int v = 70;
    if (ReadParameterN(KI_SECTION_KITTY, KI_CTXBREADCRUMBOPACITY, buf, sizeof(buf)) && buf[0])
        v = atoi(buf);
    if (v < 40) v = 40;
    if (v > 100) v = 100;
    return v;
}

static void kc_theme(KcTheme *t)
{
    if (kc_high_contrast()) {
        t->bg = GetSysColor(COLOR_WINDOW);
        t->fg = t->dim = t->line = GetSysColor(COLOR_WINDOWTEXT);
        t->root_bg = t->sw_bg = GetSysColor(COLOR_HIGHLIGHT);
        t->chip_fg = GetSysColor(COLOR_HIGHLIGHTTEXT);
        t->alpha = 255;
        return;
    }
    if (kitty_theme_app_dark()) {
        t->bg = RGB(32, 33, 36);     t->fg = RGB(232, 234, 237);
        t->dim = RGB(154, 160, 166); t->line = RGB(66, 68, 72);
    } else {
        t->bg = RGB(255, 255, 255);  t->fg = RGB(31, 31, 31);
        t->dim = RGB(95, 99, 104);   t->line = RGB(218, 221, 226);
    }
    t->root_bg = RGB(96, 16, 16);      /* the root frame's: dark red, not alarm red */
    t->sw_bg = RGB(112, 92, 0);        /* a dark yellow of the same weight */
    t->chip_fg = RGB(245, 245, 245);
    t->alpha = kc_opacity() * 255 / 100;
}

static HFONT kc_font;
static int kc_font_px;

static HFONT kc_get_font(int px)
{
    LOGFONTW lf;
    if (kc_font && kc_font_px == px)
        return kc_font;
    if (kc_font)
        DeleteObject(kc_font);
    memset(&lf, 0, sizeof(lf));
    lf.lfHeight = -px;
    lf.lfWeight = FW_SEMIBOLD;
    lf.lfCharSet = DEFAULT_CHARSET;
    /* grey antialiasing: the text gets alpha, ClearType's fringes would show */
    lf.lfQuality = ANTIALIASED_QUALITY;
    wcscpy(lf.lfFaceName, L"Segoe UI");
    kc_font = CreateFontIndirectW(&lf);
    kc_font_px = px;
    return kc_font;
}

static void kc_wide(wchar_t *dst, size_t n, const char *src)
{
    if (!MultiByteToWideChar(CP_UTF8, 0, src, -1, dst, (int)n))
        dst[0] = 0;
    dst[n - 1] = 0;
}

/* ---- the picture ---------------------------------------------------------- */

static void kc_canvas_free(void)
{
    if (kc_dc) {
        SelectObject(kc_dc, kc_oldbmp);
        DeleteDC(kc_dc);
    }
    if (kc_bmp)
        DeleteObject(kc_bmp);
    kc_dc = NULL;
    kc_bmp = NULL;
    kc_px = NULL;
    kc_cw = kc_ch = 0;
}

static bool kc_canvas(int w, int h)
{
    BITMAPINFO bi;
    void *bits = NULL;
    HDC sdc;
    if (kc_dc && w == kc_cw && h == kc_ch)
        return true;
    kc_canvas_free();
    sdc = GetDC(NULL);
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;        /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    kc_bmp = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    kc_dc = CreateCompatibleDC(sdc);
    ReleaseDC(NULL, sdc);
    if (!kc_bmp || !bits || !kc_dc) {
        if (kc_dc) DeleteDC(kc_dc);
        if (kc_bmp) DeleteObject(kc_bmp);
        kc_dc = NULL;
        kc_bmp = NULL;
        return false;
    }
    kc_oldbmp = (HBITMAP)SelectObject(kc_dc, kc_bmp);
    kc_px = (DWORD *)bits;
    kc_cw = w;
    kc_ch = h;
    return true;
}

/* What is drawn: the segments to show, as indices into kc_seg, -1 = "...". */
typedef struct {
    int idx[KC_MAXSEG + 1];
    int k;
} KcPlan;

static int kc_text_w(HDC dc, const wchar_t *s)
{
    SIZE sz;
    return GetTextExtentPoint32W(dc, s, (int)wcslen(s), &sz) ? sz.cx : 0;
}

/* The width of a plan in pixels, with the font selected into dc. */
static int kc_plan_w(HDC dc, const KcPlan *p, int pad, int chip, int gap)
{
    wchar_t w[CTX_SEG_TEXT];
    int i, x = pad;
    for (i = 0; i < p->k; i++) {
        if (i)
            x += gap * 2 + kc_text_w(dc, L"\x203A");
        if (p->idx[i] < 0) {
            x += kc_text_w(dc, L"\x2026");
            continue;
        }
        kc_wide(w, CTX_SEG_TEXT, kc_seg[p->idx[i]].text);
        x += kc_text_w(dc, w) + (kc_seg[p->idx[i]].as != CTX_AS_PLAIN ? 2 * chip : 0);
    }
    return x + pad;
}

/* [KiTTY] ctxbreadcrumbplace: "window" puts it at the top right of the
 * terminal area; anything else, or nothing, is the title bar. */
static bool kc_want_title(void)
{
    char buf[16] = "";
    ReadParameterN(KI_SECTION_KITTY, KI_CTXBREADCRUMBPLACE, buf, sizeof(buf));
    return stricmp(buf, "window") != 0;
}

/* The free part of the title bar in screen pixels: from the window's left
 * edge to just left of the caption buttons, the caption's own height. False
 * when the window has no title bar now (full screen, switched off,
 * minimised). The buttons' bounds come from the DWM (relative to the
 * window rectangle); without it, from the system metrics. */
static bool kc_caption(RECT *out)
{
    typedef HRESULT (WINAPI *fn_t)(HWND, DWORD, PVOID, DWORD);
    static fn_t fn;
    static int tried;
    RECT wr, bb;
    LONG style;

    if (!MainHwnd || IsIconic(MainHwnd) || !GetWindowRect(MainHwnd, &wr))
        return false;
    style = GetWindowLong(MainHwnd, GWL_STYLE);
    if ((style & WS_CAPTION) != WS_CAPTION)
        return false;
    if (!tried) {
        HMODULE dwm = LoadLibraryA("dwmapi.dll");
        fn = dwm ? (fn_t)(void *)GetProcAddress(dwm, "DwmGetWindowAttribute") : NULL;
        tried = 1;
    }
    /* DWMWA_CAPTION_BUTTON_BOUNDS = 5 */
    if (fn && SUCCEEDED(fn(MainHwnd, 5, &bb, sizeof(bb))) && bb.bottom > bb.top) {
        out->left = wr.left;
        out->right = wr.left + bb.left;
        out->top = wr.top + bb.top;
        out->bottom = wr.top + bb.bottom;
    } else {
        int frame = GetSystemMetrics(SM_CYFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
        out->left = wr.left;
        out->right = wr.right - frame - 3 * GetSystemMetrics(SM_CXSIZE);
        out->top = wr.top + frame;
        out->bottom = out->top + GetSystemMetrics(SM_CYCAPTION);
    }
    return out->right > out->left && out->bottom > out->top;
}

/* Where it goes: over the title bar's right end, or its offset from the
 * terminal area's top-RIGHT corner, kept inside that area. */
static void kc_place(int w, int h, POINT *pt)
{
    RECT cr;
    POINT tl = { 0, 0 };
    int x, y;
    if (kc_title && kc_caption(&cr)) {
        pt->x = cr.right - (int)(6 * kc_s + .5) - w;
        if (pt->x < cr.left)
            pt->x = cr.left;
        pt->y = cr.top + ((cr.bottom - cr.top) - h) / 2;
        return;
    }
    GetClientRect(MainHwnd, &cr);
    ClientToScreen(MainHwnd, &tl);
    x = tl.x + cr.right - (int)(kc_dx * kc_s + .5) - w;
    y = tl.y + (int)(kc_dy * kc_s + .5);
    if (x + w > tl.x + cr.right) x = tl.x + cr.right - w;
    if (x < tl.x) x = tl.x;
    if (y + h > tl.y + cr.bottom) y = tl.y + cr.bottom - h;
    if (y < tl.y) y = tl.y;
    pt->x = x;
    pt->y = y;
}

static void kc_render(void)
{
    UINT dpi = kc_dpi();
    double s = dpi / 96.0;
    KcTheme t;
    KcPlan plan;
    HDC sdc, dc;
    HFONT f, oldf;
    RECT cr;
    int h, w, maxw, pad, chip, gap, i, x;
    KovCanvas cv;
    BLENDFUNCTION bf;
    POINT pt, src = { 0, 0 };
    SIZE size;

    kc_s = s;
    kc_theme(&t);
    pad = (int)(7 * s + .5);
    chip = (int)(5 * s + .5);
    gap = (int)(5 * s + .5);
    if (kc_title && kc_caption(&cr)) {
        /* the caption's height less a little air; at most 60% of its width */
        h = (cr.bottom - cr.top) - (int)(6 * s + .5);
        if (h < (int)(16 * s)) h = (int)(16 * s);
        maxw = (cr.right - cr.left) * 6 / 10;
    } else {
        h = kc_line_px > (int)(18 * s) ? kc_line_px : (int)(18 * s);
        GetClientRect(MainHwnd, &cr);
        maxw = cr.right - 2 * (int)(KC_MARGIN * s);
    }
    f = kc_get_font(h * 62 / 100);

    /* the plan: every segment, or the first, "..." and the last two */
    sdc = GetDC(NULL);
    oldf = (HFONT)SelectObject(sdc, f);
    plan.k = 0;
    for (i = 0; i < kc_n && i < KC_MAXSEG; i++)
        plan.idx[plan.k++] = i;
    w = kc_plan_w(sdc, &plan, pad, chip, gap);
    if (w > maxw && kc_n > 3) {
        plan.k = 0;
        plan.idx[plan.k++] = 0;
        plan.idx[plan.k++] = -1;
        plan.idx[plan.k++] = kc_n - 2;
        plan.idx[plan.k++] = kc_n - 1;
        w = kc_plan_w(sdc, &plan, pad, chip, gap);
    }
    SelectObject(sdc, oldf);
    ReleaseDC(NULL, sdc);
    if (w > maxw && maxw > 4 * pad)
        w = maxw;                      /* the text is cut at the edge then */

    if (!kc_hwnd || !kc_canvas(w, h))
        return;
    dc = kc_dc;
    cv.px = kc_px;
    cv.w = w;
    cv.h = h;
    for (i = 0; i < w * h; i++)
        kc_px[i] = ((DWORD)GetRValue(t.bg) << 16) | ((DWORD)GetGValue(t.bg) << 8) |
                   GetBValue(t.bg);

    /* the chips of the root and switched-user segments first, the text on top */
    SelectObject(dc, f);
    x = pad;
    for (i = 0; i < plan.k; i++) {
        wchar_t wt[CTX_SEG_TEXT];
        if (i)
            x += gap * 2 + kc_text_w(dc, L"\x203A");
        if (plan.idx[i] < 0) {
            x += kc_text_w(dc, L"\x2026");
            continue;
        }
        kc_wide(wt, CTX_SEG_TEXT, kc_seg[plan.idx[i]].text);
        if (kc_seg[plan.idx[i]].as != CTX_AS_PLAIN) {
            int tw = kc_text_w(dc, wt) + 2 * chip;
            kov_rrect(&cv, x, 3 * s, x + tw, h - 3 * s, 4 * s,
                     kc_seg[plan.idx[i]].as == CTX_AS_ROOT ? t.root_bg : t.sw_bg, 1);
            x += tw;
        } else {
            x += kc_text_w(dc, wt);
        }
    }
    kov_rrect_edge(&cv, 0, 0, w, h, 6 * s, 1 * s, t.line);
    GdiFlush();
    SetBkMode(dc, TRANSPARENT);
    x = pad;
    for (i = 0; i < plan.k; i++) {
        wchar_t wt[CTX_SEG_TEXT];
        RECT r;
        if (i) {
            SetRect(&r, x + gap, 0, w, h);
            SetTextColor(dc, t.dim);
            DrawTextW(dc, L"\x203A", -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            x += gap * 2 + kc_text_w(dc, L"\x203A");
        }
        if (plan.idx[i] < 0) {
            SetRect(&r, x, 0, w, h);
            SetTextColor(dc, t.dim);
            DrawTextW(dc, L"\x2026", -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            x += kc_text_w(dc, L"\x2026");
            continue;
        }
        kc_wide(wt, CTX_SEG_TEXT, kc_seg[plan.idx[i]].text);
        if (kc_seg[plan.idx[i]].as != CTX_AS_PLAIN) {
            SetRect(&r, x + chip, 0, w - pad, h);
            SetTextColor(dc, t.chip_fg);
            x += 2 * chip;
        } else {
            SetRect(&r, x, 0, w - pad, h);
            SetTextColor(dc, t.fg);
        }
        DrawTextW(dc, wt, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
                  DT_END_ELLIPSIS);
        x += kc_text_w(dc, wt);
    }
    GdiFlush();

    /* the panel's shape at the opacity, premultiplied */
    for (i = 0; i < w * h; i++) {
        int px = i % w, py = i / w;
        int a = (int)(kov_cov(kov_sd_rrect(px + .5, py + .5, 0, 0, w, h, 6 * s)) * t.alpha + .5);
        DWORD c = kc_px[i];
        int cr_ = (c >> 16) & 0xFF, cg = (c >> 8) & 0xFF, cb = c & 0xFF;
        kc_px[i] = ((DWORD)a << 24) | ((DWORD)(cr_ * a / 255) << 16) |
                   ((DWORD)(cg * a / 255) << 8) | (DWORD)(cb * a / 255);
    }

    kc_place(w, h, &pt);
    size.cx = w;
    size.cy = h;
    bf.BlendOp = AC_SRC_OVER;
    bf.BlendFlags = 0;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat = AC_SRC_ALPHA;
    UpdateLayeredWindow(kc_hwnd, NULL, &pt, &size, kc_dc, &src, 0, &bf, ULW_ALPHA);
}

/* ---- the window ----------------------------------------------------------- */

bool kitty_ctxcrumb_rect(RECT *r)
{
    if (!kc_hwnd || kc_title || !IsWindowVisible(kc_hwnd))
        return false;
    return GetWindowRect(kc_hwnd, r) != 0;
}

/* In the terminal area it shares the top right with the program status
 * panel, which keeps clear of it (kitty_psoverlay.c reads kitty_ctxcrumb_rect):
 * notified whenever the breadcrumb comes, goes or moves there. */
static void kc_update_panel(void)
{
    kitty_psoverlay_follow();
}

static void kc_hide(void)
{
    if (kc_hwnd && IsWindowVisible(kc_hwnd)) {
        ShowWindow(kc_hwnd, SW_HIDE);
        kc_update_panel();
    }
}

static bool kc_owner_shown(void)
{
    return MainHwnd && IsWindowVisible(MainHwnd) && !IsIconic(MainHwnd);
}

/* Where it is now, as the offset that is kept (from the top-right corner). */
static void kc_take_place(void)
{
    RECT wr, cr;
    POINT tl = { 0, 0 };
    double s = kc_dpi() / 96.0;
    if (!GetWindowRect(kc_hwnd, &wr) || !GetClientRect(MainHwnd, &cr))
        return;
    ClientToScreen(MainHwnd, &tl);
    kc_dx = (int)((tl.x + cr.right - wr.right) / s + .5);
    kc_dy = (int)((wr.top - tl.y) / s + .5);
    if (kc_dx < 0) kc_dx = 0;
    if (kc_dy < 0) kc_dy = 0;
    if (kc_session[0])
        kitty_winpos_ctxcrumb_set(kc_session, kc_dx, kc_dy);
}

static void kc_show(void);

static LRESULT CALLBACK kc_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
      case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;          /* the terminal keeps the keyboard */
      case WM_SETCURSOR:
        SetCursor(LoadCursor(NULL, IDC_SIZEALL));
        return TRUE;
      case WM_LBUTTONDOWN:
        /* dragged anywhere; HTCLIENT elsewhere, so the tooltip sees the mouse */
        ReleaseCapture();
        SendMessage(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        return 0;
      case WM_MOVING: {
        /* dragged, it stays over the terminal area */
        RECT *r = (RECT *)lp, cr;
        POINT tl = { 0, 0 };
        int w = r->right - r->left, h = r->bottom - r->top;
        if (MainHwnd && GetClientRect(MainHwnd, &cr)) {
            ClientToScreen(MainHwnd, &tl);
            if (r->right > tl.x + cr.right) { r->right = tl.x + cr.right; r->left = r->right - w; }
            if (r->left < tl.x) { r->left = tl.x; r->right = r->left + w; }
            if (r->bottom > tl.y + cr.bottom) { r->bottom = tl.y + cr.bottom; r->top = r->bottom - h; }
            if (r->top < tl.y) { r->top = tl.y; r->bottom = r->top + h; }
        }
        return TRUE;
      }
      case WM_EXITSIZEMOVE:
        kc_take_place();
        kc_update_panel();
        return 0;
      case WM_KC_SHOW:
        kc_show();
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

static bool kc_create(void)
{
    static bool registered;
    if (kc_hwnd)
        return true;
    if (!MainHwnd)
        return false;
    if (!registered) {
        WNDCLASSA wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = kc_wndproc;
        wc.hInstance = GetModuleHandle(NULL);
        wc.hCursor = LoadCursor(NULL, IDC_SIZEALL);
        wc.lpszClassName = KC_CLASS;
        if (!RegisterClassA(&wc))
            return false;
        registered = true;
    }
    /* owned by the terminal window: above it, minimised with it, never in
     * the taskbar, never the focus */
    kc_hwnd = CreateWindowExA(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                              KC_CLASS, "", WS_POPUP, 0, 0, 1, 1, MainHwnd,
                              NULL, GetModuleHandle(NULL), NULL);
    if (!kc_hwnd)
        return false;
    /* the whole chain on hover */
    kc_tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL,
                             WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                             CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                             CW_USEDEFAULT, kc_hwnd, NULL, GetModuleHandle(NULL), NULL);
    if (kc_tip) {
        TOOLINFOW ti;
        memset(&ti, 0, sizeof(ti));
        ti.cbSize = sizeof(ti);
        ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        ti.hwnd = kc_hwnd;
        ti.uId = (UINT_PTR)kc_hwnd;
        ti.lpszText = kc_chain;
        SendMessageW(kc_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
        SendMessageW(kc_tip, TTM_SETMAXTIPWIDTH, 0, 32767);
    }
    return true;
}

/* The place for now, and whether it shows there: over the title bar when it
 * is wanted and the window has one (shown on the alternate screen too, it
 * covers no text), else in the terminal area (not on the alternate screen). */
static void kc_decide(void)
{
    RECT cap;
    kc_title = kc_want_title() && kc_caption(&cap);
    kc_due = kc_on && kc_n >= 2 && (kc_title || !kc_alt);
}

/* Click-through over the title bar, so the caption keeps its drag and its
 * double click; a window of its own in the terminal area. */
static void kc_set_mode(void)
{
    LONG ex = GetWindowLong(kc_hwnd, GWL_EXSTYLE);
    LONG want = kc_title ? (ex | WS_EX_TRANSPARENT) : (ex & ~WS_EX_TRANSPARENT);
    if (want != ex)
        SetWindowLong(kc_hwnd, GWL_EXSTYLE, want);
    if (kc_tip)
        SendMessageW(kc_tip, TTM_ACTIVATE, !kc_title, 0);
}

static void kc_show(void)
{
    kc_decide();
    if (!kc_due || !kc_owner_shown()) {
        kc_hide();
        return;
    }
    if (!kc_create())
        return;
    kc_set_mode();
    kc_render();
    SetWindowTextW(kc_hwnd, kc_chain);
    if (kc_tip) {
        TOOLINFOW ti;
        memset(&ti, 0, sizeof(ti));
        ti.cbSize = sizeof(ti);
        ti.uFlags = TTF_IDISHWND;
        ti.hwnd = kc_hwnd;
        ti.uId = (UINT_PTR)kc_hwnd;
        ti.lpszText = kc_chain;
        SendMessageW(kc_tip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
        /* the application's theme, as the link preview takes it */
        kitty_theme_tooltip(kc_tip, kitty_theme_app_dark());
    }
    if (!IsWindowVisible(kc_hwnd))
        ShowWindow(kc_hwnd, SW_SHOWNOACTIVATE);
    if (!kc_title)
        kc_update_panel();
}

/* ---- the chain ------------------------------------------------------------ */

void kitty_ctxcrumb_update(const CtxSeg *seg, int n, bool on, bool alt,
                           const char *session, int line_px)
{
    int i;
    size_t len = 0;

    if (!session)
        session = "";
    if (strcmp(session, kc_session)) {
        snprintf(kc_session, sizeof(kc_session), "%s", session);
        kc_have_pos = false;
    }
    kc_n = n < KC_MAXSEG ? n : KC_MAXSEG;
    for (i = 0; i < kc_n; i++)
        kc_seg[i] = seg[i];
    if (line_px > 0)
        kc_line_px = line_px;
    kc_on = on;
    kc_alt = alt;
    /* the place and the text even while hidden: a change of
     * ctxbreadcrumbplace can show it (kitty_ctxcrumb_restyle) */
    if (!kc_have_pos) {
        kc_dx = kc_dy = KC_MARGIN;
        kitty_winpos_ctxcrumb_get(kc_session, &kc_dx, &kc_dy);
        kc_have_pos = true;
    }
    /* the whole chain, " > " between the segments */
    kc_chain[0] = 0;
    for (i = 0; i < kc_n; i++) {
        wchar_t w[CTX_SEG_TEXT];
        kc_wide(w, CTX_SEG_TEXT, kc_seg[i].text);
        if (i && len + 3 < sizeof(kc_chain) / sizeof(*kc_chain)) {
            wcscat(kc_chain, L" > ");
            len += 3;
        }
        if (len + wcslen(w) + 1 < sizeof(kc_chain) / sizeof(*kc_chain)) {
            wcscat(kc_chain, w);
            len += wcslen(w);
        }
    }
    kc_decide();
    if (!kc_due) {
        kc_hide();
        return;
    }
    kc_show();
}

void kitty_ctxcrumb_follow(void)
{
    bool was_title = kc_title;
    if (!kc_hwnd)
        return;
    kc_decide();                       /* full screen takes the title bar away */
    if (!kc_due || !kc_owner_shown()) {
        kc_hide();
        return;
    }
    if (IsWindowVisible(kc_hwnd) && kc_dc && kc_dpi() / 96.0 == kc_s &&
        was_title == kc_title && !kc_title) {
        POINT pt;
        kc_place(kc_cw, kc_ch, &pt);
        SetWindowPos(kc_hwnd, NULL, pt.x, pt.y, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        kc_update_panel();
    } else {
        kc_show();                     /* a new DPI: drawn again */
    }
}

void kitty_ctxcrumb_owner_shown(bool shown)
{
    if (!kc_hwnd)
        return;
    if (!shown)
        kc_hide();
    else                               /* not visible yet: once it is */
        PostMessage(kc_hwnd, WM_KC_SHOW, 0, 0);
}

/* Also a new ctxbreadcrumbplace: kc_show decides the mode again, so one hidden
 * on the alternate screen in the terminal area comes back in the title bar,
 * and the other way round. */
void kitty_ctxcrumb_restyle(void)
{
    if (kc_on && kc_n >= 2)
        kc_show();
}
