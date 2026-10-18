/*
 * kitty_psoverlay.c - the program status overlay.
 *
 * While two or more jobs in this window report their status (OSC 7501, the
 * records are kitty_progstatus.c's), a panel at the top right of the terminal
 * lists them: a state icon, the name below its parent, the percentage or the
 * state, the program's message and a bar. The blocked row is highlighted,
 * done rows are faded. The panel can be dragged by its header, and its place
 * is kept per session, measured from the terminal area's top-right corner;
 * the x hides it until a job it has not shown yet starts.
 *
 * How it is built, and why:
 *  - A layered popup OWNED by the terminal window, not a child: a child would
 *    be painted over by the terminal's own renderer (Direct2D presents the
 *    whole client area). Owned, it stays above the terminal, goes with it
 *    when it is minimised, and never takes the focus (WS_EX_NOACTIVATE,
 *    MA_NOACTIVATE): typing goes on into the terminal.
 *  - Drawn into a 32-bit DIB and handed to UpdateLayeredWindow with per-pixel
 *    alpha: about 90 % opaque, with rounded corners. Icons, bars and corners
 *    are drawn from signed distances, so their edges are smooth without
 *    GDI+; the text is GDI's, drawn on the panel's opaque colour, and gets
 *    its alpha afterwards with everything else.
 *  - In high contrast the system colours are used and the panel is opaque.
 */
#include <winsock2.h>   /* before windows.h: putty.h pulls it in further down */
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "putty.h"
#include "kitty_progstatus.h"
#include "kitty_psoverlay.h"
#include "kitty_overlay_draw.h"   /* the canvas and its rounded rectangles */
#include "kitty_ctxcrumb.h"       /* the breadcrumb it keeps clear of */
#include "kitty_text.h"
#include "kitty_win.h"        /* kitty_theme_app_dark */
#include "kitty_winpos.h"     /* the overlay's place, per session */
#include "kitty_oldwin.h"     /* kitty_api_from: GetDpiForWindow */

extern HWND MainHwnd;          /* kitty.c: the terminal window */

#define PO_CLASS    "KiTTYStatusOverlay"
#define PO_TIMER    1
#define PO_TICK_MS  100        /* the spinner and the busy bar */
#define PO_ALPHA    230        /* about 90 % */
#define PO_MAXROWS  8          /* more are counted in a last line */
/* Sizes in 96-dpi pixels. */
#define PO_WIDTH    340
#define PO_MARGIN   14         /* the default distance from the corner */
#define PO_HEAD     34
#define PO_RADIUS   10
#define WM_PO_SHOW  (WM_APP + 1)   /* to itself: the owner is visible now */

typedef struct {
    int state, kind, progress, depth;
    wchar_t name[64];
    wchar_t parent[40];
    wchar_t right[48];
    wchar_t detail[160];
} PoRow;

static HWND po_hwnd;
static PoRow po_rows[PO_MAXROWS];
static int po_n, po_more;
static wchar_t po_head[200];
static wchar_t po_jobs[32];
static char po_session[256];
static bool po_have_pos;               /* po_dx/po_dy read for po_session */
static int po_dx = PO_MARGIN, po_dy = PO_MARGIN;
static bool po_due;                    /* the records call for it */
/* The x hides it until the next job: the ids there were then. */
static bool po_dismissed;
static char po_ids[PS_RECORDS_MAX][PS_ID_MAX + 1];
static int po_ids_n;
static char po_hidden_ids[PS_RECORDS_MAX][PS_ID_MAX + 1];
static int po_hidden_n;
static RECT po_close_rc;               /* client pixels, set by po_render */
static int po_head_px;
static bool po_animating;

/* ---- sizes, colours, fonts ---------------------------------------------- */

static UINT po_dpi(void)
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

static int po_high_contrast(void)
{
    HIGHCONTRASTA hc;
    memset(&hc, 0, sizeof(hc));
    hc.cbSize = sizeof(hc);
    if (!SystemParametersInfoA(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0))
        return 0;
    return (hc.dwFlags & HCF_HIGHCONTRASTON) ? 1 : 0;
}

typedef struct {
    COLORREF bg, fg, dim, line, track;
    COLORREF run;                      /* working: icon, bar and text */
    COLORREF ok_icon, ok_text;         /* done */
    COLORREF err_icon, err_text;       /* failed */
    COLORREF blk_icon, blk_text;       /* blocked */
    COLORREF blk_bg, blk_edge;         /* the blocked row */
    COLORREF mark;                     /* the check, cross and bars on an icon */
    int alpha;
} PoTheme;

static void po_theme(PoTheme *t)
{
    if (po_high_contrast()) {
        COLORREF bg = GetSysColor(COLOR_WINDOW), fg = GetSysColor(COLOR_WINDOWTEXT);
        COLORREF hl = GetSysColor(COLOR_HIGHLIGHT);
        t->bg = bg; t->fg = fg; t->dim = fg; t->line = fg; t->track = GetSysColor(COLOR_GRAYTEXT);
        t->run = hl; t->ok_icon = fg; t->ok_text = fg; t->err_icon = fg; t->err_text = fg;
        t->blk_icon = hl; t->blk_text = fg; t->blk_bg = bg; t->blk_edge = hl;
        t->mark = bg;
        t->alpha = 255;
    } else if (kitty_theme_app_dark()) {
        t->bg = RGB(32, 33, 36);      t->fg = RGB(232, 234, 237);
        t->dim = RGB(154, 160, 166);  t->line = RGB(66, 68, 72);
        t->track = RGB(60, 62, 66);   t->run = RGB(138, 180, 248);
        t->ok_icon = RGB(30, 142, 62);  t->ok_text = RGB(129, 201, 149);
        t->err_icon = RGB(217, 48, 37); t->err_text = RGB(242, 139, 130);
        t->blk_icon = RGB(227, 116, 0); t->blk_text = RGB(253, 186, 100);
        t->blk_bg = RGB(62, 46, 24);  t->blk_edge = RGB(128, 84, 30);
        t->mark = RGB(255, 255, 255);
        t->alpha = PO_ALPHA;
    } else {
        t->bg = RGB(255, 255, 255);   t->fg = RGB(31, 31, 31);
        t->dim = RGB(95, 99, 104);    t->line = RGB(218, 221, 226);
        t->track = RGB(232, 234, 237); t->run = RGB(11, 87, 208);
        t->ok_icon = RGB(30, 142, 62);  t->ok_text = RGB(30, 123, 52);
        t->err_icon = RGB(180, 35, 24); t->err_text = RGB(180, 35, 24);
        t->blk_icon = RGB(227, 116, 0); t->blk_text = RGB(163, 82, 0);
        t->blk_bg = RGB(255, 244, 229); t->blk_edge = RGB(245, 196, 138);
        t->mark = RGB(255, 255, 255);
        t->alpha = 237;                /* light needs a little more to read */
    }
}

/* A done row is faded: its colours move towards the panel's. */
static COLORREF po_fade(COLORREF c, COLORREF bg, bool fade)
{
    if (!fade)
        return c;
    return RGB((GetRValue(c) * 7 + GetRValue(bg) * 3) / 10,
               (GetGValue(c) * 7 + GetGValue(bg) * 3) / 10,
               (GetBValue(c) * 7 + GetBValue(bg) * 3) / 10);
}

static HFONT po_f_head, po_f_name, po_f_small, po_f_right;
static UINT po_f_dpi;

static HFONT po_font(int px, bool bold, UINT dpi)
{
    LOGFONTW lf;
    memset(&lf, 0, sizeof(lf));
    lf.lfHeight = -MulDiv(px, (int)dpi, 96);
    lf.lfWeight = bold ? FW_SEMIBOLD : FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    /* Grey antialiasing, not ClearType: the text ends up with alpha, and
     * ClearType's coloured fringes would show through it. */
    lf.lfQuality = ANTIALIASED_QUALITY;
    wcscpy(lf.lfFaceName, L"Segoe UI");
    return CreateFontIndirectW(&lf);
}

static void po_fonts_free(void)
{
    if (po_f_head)  DeleteObject(po_f_head);
    if (po_f_name)  DeleteObject(po_f_name);
    if (po_f_small) DeleteObject(po_f_small);
    if (po_f_right) DeleteObject(po_f_right);
    po_f_head = po_f_name = po_f_small = po_f_right = NULL;
    po_f_dpi = 0;
}

static void po_fonts(UINT dpi)
{
    if (po_f_dpi == dpi && po_f_head)
        return;
    po_fonts_free();
    po_f_head = po_font(12, true, dpi);
    po_f_name = po_font(13, true, dpi);
    po_f_small = po_font(11, false, dpi);
    po_f_right = po_font(12, false, dpi);
    po_f_dpi = dpi;
}

/* ---- shapes on the pixels ------------------------------------------------ */

/* The canvas and the rounded rectangles are kitty_overlay_draw.c's, shared
 * with the breadcrumb; the icons below are the panel's own. */

static void po_circle(KovCanvas *c, double cx, double cy, double r, COLORREF col)
{
    int x, y;
    for (y = (int)floor(cy - r) - 1; y <= (int)ceil(cy + r); y++)
        for (x = (int)floor(cx - r) - 1; x <= (int)ceil(cx + r); x++)
            kov_put(c, x, y, col, kov_cov(hypot(x + .5 - cx, y + .5 - cy) - r));
}

/* A stroke from (x0,y0) to (x1,y1), w wide, with round ends. */
static double po_sd_seg(double px, double py, double x0, double y0,
                        double x1, double y1)
{
    double dx = x1 - x0, dy = y1 - y0, l2 = dx * dx + dy * dy, t;
    t = l2 > 0 ? ((px - x0) * dx + (py - y0) * dy) / l2 : 0;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return hypot(px - (x0 + t * dx), py - (y0 + t * dy));
}

static void po_seg(KovCanvas *c, double x0, double y0, double x1, double y1,
                   double w, COLORREF col)
{
    int x, y;
    double lx = (x0 < x1 ? x0 : x1) - w, hx = (x0 > x1 ? x0 : x1) + w;
    double ly = (y0 < y1 ? y0 : y1) - w, hy = (y0 > y1 ? y0 : y1) + w;
    for (y = (int)floor(ly); y <= (int)ceil(hy); y++)
        for (x = (int)floor(lx); x <= (int)ceil(hx); x++)
            kov_put(c, x, y, col,
                   kov_cov(po_sd_seg(x + .5, y + .5, x0, y0, x1, y1) - w / 2));
}

/* A ring of radius r, w wide; only the part from angle a0 over span
 * (radians, clockwise on screen) when span < 2 pi, with round ends. */
static void po_arc(KovCanvas *c, double cx, double cy, double r, double w,
                   double a0, double span, COLORREF col, double a)
{
    const double tau = 6.283185307179586;
    double ex0 = cx + r * cos(a0), ey0 = cy + r * sin(a0);
    double ex1 = cx + r * cos(a0 + span), ey1 = cy + r * sin(a0 + span);
    int x, y;
    for (y = (int)floor(cy - r - w); y <= (int)ceil(cy + r + w); y++)
        for (x = (int)floor(cx - r - w); x <= (int)ceil(cx + r + w); x++) {
            double px = x + .5, py = y + .5, d;
            double rel = fmod(atan2(py - cy, px - cx) - a0 + 2 * tau, tau);
            if (span >= tau || rel <= span)
                d = fabs(hypot(px - cx, py - cy) - r) - w / 2;
            else {
                double d0 = hypot(px - ex0, py - ey0), d1 = hypot(px - ex1, py - ey1);
                d = (d0 < d1 ? d0 : d1) - w / 2;
            }
            kov_put(c, x, y, col, a * kov_cov(d));
        }
}

/* A state icon, sz pixels, top-left at (x,y). */
static void po_icon(KovCanvas *c, const PoTheme *t, int state, double x,
                    double y, double sz, bool fade, DWORD tick)
{
    double cx = x + sz / 2, cy = y + sz / 2, k = sz / 18.0;
    COLORREF mark = po_fade(t->mark, t->bg, fade);
    switch (state) {
      case PS_WORKING: {
        double a0 = fmod(tick / 1100.0, 1.0) * 6.283185307179586;
        po_arc(c, cx, cy, 7 * k, 2.2 * k, 0, 7, t->run, .25);
        po_arc(c, cx, cy, 7 * k, 2.2 * k, a0, 1.57, t->run, 1);
        break;
      }
      case PS_BLOCKED:
        po_circle(c, cx, cy, 8 * k, po_fade(t->blk_icon, t->bg, fade));
        kov_rrect(c, x + 6.2 * k, y + 5.2 * k, x + 8.1 * k, y + 12.8 * k, .8 * k, mark, 1);
        kov_rrect(c, x + 9.9 * k, y + 5.2 * k, x + 11.8 * k, y + 12.8 * k, .8 * k, mark, 1);
        break;
      case PS_ERROR:
        po_circle(c, cx, cy, 8 * k, po_fade(t->err_icon, t->bg, fade));
        po_seg(c, x + 6.2 * k, y + 6.2 * k, x + 11.8 * k, y + 11.8 * k, 2 * k, mark);
        po_seg(c, x + 11.8 * k, y + 6.2 * k, x + 6.2 * k, y + 11.8 * k, 2 * k, mark);
        break;
      default:                         /* done */
        po_circle(c, cx, cy, 8 * k, po_fade(t->ok_icon, t->bg, fade));
        po_seg(c, x + 5.2 * k, y + 9.2 * k, x + 7.7 * k, y + 11.6 * k, 2 * k, mark);
        po_seg(c, x + 7.7 * k, y + 11.6 * k, x + 12.7 * k, y + 6.4 * k, 2 * k, mark);
        break;
    }
}

static void po_wide(wchar_t *dst, size_t n, const char *src, UINT cp);
static void po_show(void);

/* ---- layout and paint ----------------------------------------------------- */

static bool po_has_bar(const PoRow *r)
{
    return r->state == PS_WORKING ||
           (r->progress >= 0 && (r->state == PS_BLOCKED || r->state == PS_ERROR));
}

/* A row's height in 96-dpi pixels. */
static int po_row_h(const PoRow *r)
{
    return 7 + 17 + (r->detail[0] ? 4 + 15 : 0) + (po_has_bar(r) ? 6 + 4 : 0) + 7;
}

static int po_height96(void)
{
    int i, h = PO_HEAD + 6 + 6;
    for (i = 0; i < po_n; i++)
        h += po_row_h(&po_rows[i]) + (i ? 2 : 0);
    if (po_more)
        h += 22;
    return h;
}

static void po_text(HDC dc, HFONT f, COLORREF col, const wchar_t *s, int x0,
                    int y0, int x1, int y1, UINT align)
{
    RECT r;
    SetRect(&r, x0, y0, x1, y1);
    SelectObject(dc, f);
    SetTextColor(dc, col);
    DrawTextW(dc, s, -1, &r, align | DT_SINGLELINE | DT_NOPREFIX |
              DT_END_ELLIPSIS | DT_VCENTER);
}

static int po_text_w(HDC dc, HFONT f, const wchar_t *s)
{
    SIZE sz;
    SelectObject(dc, f);
    if (!GetTextExtentPoint32W(dc, s, (int)wcslen(s), &sz))
        return 0;
    return sz.cx;
}

/* Where the panel goes: its offset from the terminal area's top-right corner,
 * kept inside that area. */
static void po_place(int w, int h, double s, POINT *pt)
{
    RECT cr;
    POINT tl = { 0, 0 };
    int x, y, right, bottom;
    GetClientRect(MainHwnd, &cr);
    ClientToScreen(MainHwnd, &tl);
    right = tl.x + cr.right;
    bottom = tl.y + cr.bottom;
    x = right - (int)(po_dx * s + .5) - w;
    y = tl.y + (int)(po_dy * s + .5);
    if (x + w > right) x = right - w;
    if (x < tl.x) x = tl.x;
    {
        /* The OSC 3008 breadcrumb shares the top right when it is in the
         * terminal area: the panel opens below it, never over it. */
        RECT br, mine, both;
        SetRect(&mine, x, y, x + w, y + h);
        if (kitty_ctxcrumb_rect(&br) && IntersectRect(&both, &mine, &br))
            y = br.bottom + (int)(6 * s + .5);
    }
    if (y + h > bottom) y = bottom - h;
    if (y < tl.y) y = tl.y;
    pt->x = x;
    pt->y = y;
}

/*
 * The picture is kept between frames. A full render (po_render) draws
 * everything that stands still into the DIB, keeps a copy of it (po_base)
 * and the panel's alpha per pixel (po_mask), and notes where something moves
 * (po_anim: a spinner, a busy bar). A frame (po_tick) then redraws only
 * those places from the copy and hands only them over to the window. Measured
 * with scripts\measure_psoverlay_cpu.ps1: redrawing everything every 60 ms
 * cost 6.5 % of a core for two working jobs.
 */
typedef struct {
    RECT rc;                           /* what a frame redraws, in pixels */
    int kind;                          /* 0 spinner, 1 busy bar */
    double x, y, sz;                   /* spinner: the icon's box */
    double bx0, by, bx1;               /* busy bar */
} PoAnim;

static HDC po_dc;
static HBITMAP po_bmp, po_oldbmp;
static DWORD *po_px, *po_base;
static BYTE *po_mask;
static int po_cw, po_ch;               /* the DIB's size */
static double po_s;
static PoTheme po_t;
static PoAnim po_anim[PO_MAXROWS * 2];
static int po_nanim;

static void po_canvas_free(void)
{
    if (po_dc) {
        SelectObject(po_dc, po_oldbmp);
        DeleteDC(po_dc);
    }
    if (po_bmp)
        DeleteObject(po_bmp);
    free(po_base);
    free(po_mask);
    po_dc = NULL;
    po_bmp = NULL;
    po_px = po_base = NULL;
    po_mask = NULL;
    po_cw = po_ch = 0;
}

static bool po_canvas(int w, int h)
{
    BITMAPINFO bi;
    void *bits = NULL;
    HDC sdc;
    if (po_dc && w == po_cw && h == po_ch)
        return true;
    po_canvas_free();
    sdc = GetDC(NULL);
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;        /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    po_bmp = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    po_dc = CreateCompatibleDC(sdc);
    ReleaseDC(NULL, sdc);
    po_base = (DWORD *)malloc(sizeof(DWORD) * (size_t)w * h);
    po_mask = (BYTE *)malloc((size_t)w * h);
    if (!po_bmp || !bits || !po_dc || !po_base || !po_mask) {
        if (po_dc) DeleteDC(po_dc);
        if (po_bmp) DeleteObject(po_bmp);
        free(po_base);
        free(po_mask);
        po_dc = NULL;
        po_bmp = NULL;
        po_base = NULL;
        po_mask = NULL;
        return false;
    }
    po_oldbmp = (HBITMAP)SelectObject(po_dc, po_bmp);
    po_px = (DWORD *)bits;
    po_cw = w;
    po_ch = h;
    return true;
}

/* What moves, drawn at this moment. */
static void po_anim_draw(KovCanvas *cv, const PoAnim *a, DWORD tick)
{
    if (a->kind == 0) {
        po_icon(cv, &po_t, PS_WORKING, a->x, a->y, a->sz, false, tick);
    } else {
        int px, py;
        double s = po_s, period = 14 * s, off = fmod(tick / 900.0, 1.0) * 2 * period;
        for (py = (int)a->by; py < (int)(a->by + 4 * s + .5); py++)
            for (px = (int)a->bx0 + 1; px < (int)a->bx1 - 1; px++)
                if (fmod(px - py - off + 100 * period, period) < period / 2)
                    kov_put(cv, px, py, RGB(255, 255, 255), .28);
    }
}

/* Premultiplied by the panel's alpha, within r. */
static void po_premultiply(const RECT *r)
{
    int x, y;
    for (y = r->top; y < r->bottom; y++)
        for (x = r->left; x < r->right; x++) {
            DWORD *p = &po_px[y * po_cw + x];
            int a = po_mask[y * po_cw + x];
            int cr = (*p >> 16) & 0xFF, cg = (*p >> 8) & 0xFF, cb = *p & 0xFF;
            *p = ((DWORD)a << 24) | ((DWORD)(cr * a / 255) << 16) |
                 ((DWORD)(cg * a / 255) << 8) | (DWORD)(cb * a / 255);
        }
}

static void po_ulw(const POINT *pt, const RECT *dirty)
{
    /* UpdateLayeredWindowIndirect hands over only the dirty part (Vista and
     * newer, so looked up); without it the whole picture goes every time. */
    typedef struct {
        DWORD cbSize;
        HDC hdcDst;
        const POINT *pptDst;
        const SIZE *psize;
        HDC hdcSrc;
        const POINT *pptSrc;
        COLORREF crKey;
        const BLENDFUNCTION *pblend;
        DWORD dwFlags;
        const RECT *prcDirty;
    } PoUlwInfo;
    typedef BOOL (WINAPI *fn_t)(HWND, const PoUlwInfo *);
    static fn_t fn;
    static int tried;
    POINT src = { 0, 0 };
    SIZE size;
    BLENDFUNCTION bf;
    size.cx = po_cw;
    size.cy = po_ch;
    bf.BlendOp = AC_SRC_OVER;
    bf.BlendFlags = 0;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat = AC_SRC_ALPHA;
    if (!tried) {
        fn = (fn_t)kitty_api_from(GetModuleHandleA("user32.dll"), "user32.dll",
                                  "UpdateLayeredWindowIndirect", KITTY_API_OPTIONAL,
                                  KT_WINFEAT_ULWINDIRECT);
        tried = 1;
    }
    if (dirty && fn) {
        PoUlwInfo info;
        memset(&info, 0, sizeof(info));
        info.cbSize = sizeof(info);
        info.pptDst = pt;
        info.psize = &size;
        info.hdcSrc = po_dc;
        info.pptSrc = &src;
        info.pblend = &bf;
        info.dwFlags = ULW_ALPHA;
        info.prcDirty = dirty;
        if (fn(po_hwnd, &info))
            return;
    }
    UpdateLayeredWindow(po_hwnd, NULL, (POINT *)pt, &size, po_dc, &src, 0, &bf, ULW_ALPHA);
}

static void po_render(void)
{
    UINT dpi = po_dpi();
    double s = dpi / 96.0;
    int w = (int)(PO_WIDTH * s + .5), h = (int)(po_height96() * s + .5);
    KovCanvas cv;
    HDC dc;
    DWORD tick = GetTickCount();
    POINT pt;
    RECT all;
    int i, x, y, pad = (int)(6 * s + .5);

    if (!po_hwnd || !po_canvas(w, h))
        return;
    po_s = s;
    po_theme(&po_t);
    po_fonts(dpi);
    dc = po_dc;
    po_nanim = 0;
    cv.px = po_px;
    cv.w = w;
    cv.h = h;
    for (i = 0; i < w * h; i++)
        cv.px[i] = ((DWORD)GetRValue(po_t.bg) << 16) | ((DWORD)GetGValue(po_t.bg) << 8) |
                   GetBValue(po_t.bg);

    /* The header: a clock, the title, the number of jobs, the x. */
    po_head_px = (int)(PO_HEAD * s + .5);
    {
        double ix = 12 * s, iy = (PO_HEAD * s - 14 * s) / 2;
        po_arc(&cv, ix + 7 * s, iy + 7 * s, 5.5 * s, 1.6 * s, 0, 7, po_t.run, 1);
        po_seg(&cv, ix + 7 * s, iy + 4 * s, ix + 7 * s, iy + 7.3 * s, 1.6 * s, po_t.run);
        po_seg(&cv, ix + 7 * s, iy + 7.3 * s, ix + 9 * s, iy + 8.7 * s, 1.6 * s, po_t.run);
        SetRect(&po_close_rc, w - (int)(32 * s), (po_head_px - (int)(22 * s)) / 2,
                w - (int)(10 * s), (po_head_px + (int)(22 * s)) / 2);
        {
            double cx = (po_close_rc.left + po_close_rc.right) / 2.0;
            double cy = (po_close_rc.top + po_close_rc.bottom) / 2.0, k = 4 * s;
            po_seg(&cv, cx - k, cy - k, cx + k, cy + k, 1.4 * s, po_t.dim);
            po_seg(&cv, cx + k, cy - k, cx - k, cy + k, 1.4 * s, po_t.dim);
        }
        for (x = 0; x < w; x++)
            kov_put(&cv, x, po_head_px - 1, po_t.line, 1);
    }

    /* The rows: their backgrounds, icons and bars first, the text after.
     * What moves is noted, not drawn. */
    y = po_head_px + pad;
    for (i = 0; i < po_n; i++) {
        const PoRow *r = &po_rows[i];
        int rh = (int)(po_row_h(r) * s + .5);
        int indent = (int)((r->depth > 2 ? 2 : r->depth) * 22 * s + .5);
        double isz = (r->depth ? 16 : 18) * s;
        int tx = pad + (int)(8 * s) + indent + (int)(isz + 10 * s);
        double ix = pad + 8 * s + indent, iy = y + 8 * s;
        bool fade = r->state == PS_DONE;
        if (r->state == PS_BLOCKED) {
            kov_rrect(&cv, pad, y, w - pad, y + rh, 7 * s, po_t.blk_bg, 1);
            kov_rrect_edge(&cv, pad, y, w - pad, y + rh, 7 * s, 1 * s, po_t.blk_edge);
        }
        if (r->state == PS_WORKING && po_nanim < PO_MAXROWS * 2) {
            PoAnim *a = &po_anim[po_nanim++];
            a->kind = 0;
            a->x = ix; a->y = iy; a->sz = isz;
            SetRect(&a->rc, (int)ix - 2, (int)iy - 2, (int)(ix + isz) + 3, (int)(iy + isz) + 3);
        } else {
            po_icon(&cv, &po_t, r->state, ix, iy, isz, fade, tick);
        }
        if (po_has_bar(r)) {
            double by = y + rh - 7 * s - 4 * s, bx0 = tx, bx1 = w - pad - 8 * s;
            COLORREF bc = r->state == PS_ERROR ? po_t.err_icon :
                          r->state == PS_BLOCKED ? po_t.blk_icon : po_t.run;
            kov_rrect(&cv, bx0, by, bx1, by + 4 * s, 2 * s, po_t.track, 1);
            if (r->progress >= 0) {
                if (r->progress > 0)
                    kov_rrect(&cv, bx0, by, bx0 + (bx1 - bx0) * r->progress / 100.0,
                             by + 4 * s, 2 * s, bc, 1);
            } else {
                /* Busy without a percentage: the bar stands, its stripes move. */
                kov_rrect(&cv, bx0, by, bx1, by + 4 * s, 2 * s, bc, .55);
                if (po_nanim < PO_MAXROWS * 2) {
                    PoAnim *a = &po_anim[po_nanim++];
                    a->kind = 1;
                    a->bx0 = bx0; a->by = by; a->bx1 = bx1;
                    SetRect(&a->rc, (int)bx0, (int)by, (int)bx1 + 1, (int)(by + 4 * s) + 2);
                }
            }
        }
        y += rh + (int)(2 * s + .5);
    }

    /* The panel's edge, then the text (GDI, on the finished colours). */
    kov_rrect_edge(&cv, 0, 0, w, h, PO_RADIUS * s, 1 * s, po_t.line);
    GdiFlush();
    SetBkMode(dc, TRANSPARENT);
    po_text(dc, po_f_head, po_t.fg, po_head, (int)(32 * s), 0,
            po_close_rc.left - (int)(8 * s) - po_text_w(dc, po_f_small, po_jobs) - (int)(8 * s),
            po_head_px, DT_LEFT);
    po_text(dc, po_f_small, po_t.dim, po_jobs, 0, 0, po_close_rc.left - (int)(6 * s),
            po_head_px, DT_RIGHT);
    y = po_head_px + pad;
    for (i = 0; i < po_n; i++) {
        const PoRow *r = &po_rows[i];
        int rh = (int)(po_row_h(r) * s + .5);
        int indent = (int)((r->depth > 2 ? 2 : r->depth) * 22 * s + .5);
        double isz = (r->depth ? 16 : 18) * s;
        int tx = pad + (int)(8 * s) + indent + (int)(isz + 10 * s);
        int rx = w - pad - (int)(8 * s);
        int ly = y + (int)(7 * s);
        int lh = (int)(17 * s + .5);
        bool fade = r->state == PS_DONE;
        COLORREF rc = r->state == PS_WORKING ? po_t.run :
                      r->state == PS_BLOCKED ? po_t.blk_text :
                      r->state == PS_ERROR ? po_t.err_text : po_t.ok_text;
        int rw = po_text_w(dc, r->state == PS_WORKING && r->progress >= 0 ?
                           po_f_right : po_f_small, r->right);
        int nx = tx;
        if (r->parent[0]) {
            wchar_t pbuf[48];
            _snwprintf(pbuf, 47, L"%ls \x203A", r->parent);
            pbuf[47] = 0;
            po_text(dc, po_f_small, po_fade(po_t.dim, po_t.bg, fade), pbuf, nx, ly,
                    rx - rw - (int)(8 * s), ly + lh, DT_LEFT);
            nx += po_text_w(dc, po_f_small, pbuf) + (int)(5 * s);
        }
        po_text(dc, po_f_name, po_fade(po_t.fg, po_t.bg, fade), r->name, nx, ly,
                rx - rw - (int)(8 * s), ly + lh, DT_LEFT);
        po_text(dc, r->state == PS_WORKING && r->progress >= 0 ? po_f_right : po_f_small,
                po_fade(rc, po_t.bg, fade), r->right, rx - rw, ly, rx, ly + lh, DT_RIGHT);
        if (r->detail[0])
            po_text(dc, po_f_small, po_fade(po_t.dim, po_t.bg, fade), r->detail, tx,
                    ly + lh + (int)(4 * s), rx, ly + lh + (int)(19 * s), DT_LEFT);
        y += rh + (int)(2 * s + .5);
    }
    if (po_more) {
        wchar_t more[48];
        char m[48];
        snprintf(m, sizeof(m), KT_PSO_MORE, po_more);
        po_wide(more, 48, m, CP_UTF8);
        po_text(dc, po_f_small, po_t.dim, more, pad + (int)(8 * s), y,
                w - pad, y + (int)(20 * s), DT_LEFT);
    }
    GdiFlush();

    /* What stands still is kept; the alpha of the rounded panel at the
     * theme's opacity too, once. Then this moment's moving parts. */
    memcpy(po_base, po_px, sizeof(DWORD) * (size_t)w * h);
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            po_mask[y * w + x] = (BYTE)(kov_cov(kov_sd_rrect(x + .5, y + .5, 0, 0, w, h,
                                                           PO_RADIUS * s)) * po_t.alpha + .5);
    for (i = 0; i < po_nanim; i++)
        po_anim_draw(&cv, &po_anim[i], tick);
    SetRect(&all, 0, 0, w, h);
    po_premultiply(&all);

    po_place(w, h, s, &pt);
    po_ulw(&pt, NULL);

    if ((po_nanim > 0) != po_animating) {
        if (po_nanim > 0)
            SetTimer(po_hwnd, PO_TIMER, PO_TICK_MS, NULL);
        else
            KillTimer(po_hwnd, PO_TIMER);
        po_animating = po_nanim > 0;
    }
}

/* One frame: only what moves, from the kept picture. */
static void po_tick(void)
{
    KovCanvas cv;
    DWORD tick = GetTickCount();
    int i, y;
    if (!po_hwnd || !po_dc || !po_nanim)
        return;
    cv.px = po_px;
    cv.w = po_cw;
    cv.h = po_ch;
    for (i = 0; i < po_nanim; i++) {
        RECT r = po_anim[i].rc;
        if (r.left < 0) r.left = 0;
        if (r.top < 0) r.top = 0;
        if (r.right > po_cw) r.right = po_cw;
        if (r.bottom > po_ch) r.bottom = po_ch;
        if (r.right <= r.left || r.bottom <= r.top)
            continue;
        for (y = r.top; y < r.bottom; y++)
            memcpy(&po_px[y * po_cw + r.left], &po_base[y * po_cw + r.left],
                   sizeof(DWORD) * (size_t)(r.right - r.left));
        po_anim_draw(&cv, &po_anim[i], tick);
        po_premultiply(&r);
        po_ulw(NULL, &r);
    }
}

/* The terminal moved: the same picture, somewhere else. */
static void po_move(void)
{
    POINT pt;
    if (!po_hwnd || !po_dc)
        return;
    po_place(po_cw, po_ch, po_s, &pt);
    SetWindowPos(po_hwnd, NULL, pt.x, pt.y, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

/* ---- the window ----------------------------------------------------------- */

static void po_hide(void)
{
    if (po_hwnd && IsWindowVisible(po_hwnd))
        ShowWindow(po_hwnd, SW_HIDE);
    if (po_hwnd && po_animating) {
        KillTimer(po_hwnd, PO_TIMER);
        po_animating = false;
    }
}

/* The x: hidden until a job starts that is not among these. */
static void po_dismiss(void)
{
    memcpy(po_hidden_ids, po_ids, sizeof(po_ids[0]) * po_ids_n);
    po_hidden_n = po_ids_n;
    po_dismissed = true;
    po_hide();
}

/* Where the panel is now, as the offset that is kept. */
static void po_take_place(void)
{
    RECT wr, cr;
    POINT tl = { 0, 0 };
    double s = po_dpi() / 96.0;
    if (!GetWindowRect(po_hwnd, &wr) || !GetClientRect(MainHwnd, &cr))
        return;
    ClientToScreen(MainHwnd, &tl);
    po_dx = (int)((tl.x + cr.right - wr.right) / s + .5);
    po_dy = (int)((wr.top - tl.y) / s + .5);
    if (po_dx < 0) po_dx = 0;
    if (po_dy < 0) po_dy = 0;
    if (po_session[0])
        kitty_winpos_overlay_set(po_session, po_dx, po_dy);
}

static LRESULT CALLBACK po_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
      case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;          /* the terminal keeps the keyboard */
      case WM_NCHITTEST: {
        POINT pt;
        pt.x = (short)LOWORD(lp);
        pt.y = (short)HIWORD(lp);
        ScreenToClient(hwnd, &pt);
        if (PtInRect(&po_close_rc, pt))
            return HTCLIENT;
        if (pt.y >= 0 && pt.y < po_head_px)
            return HTCAPTION;          /* the header drags it */
        return HTCLIENT;
      }
      case WM_SETCURSOR:
        if (LOWORD(lp) == HTCAPTION) {
            SetCursor(LoadCursor(NULL, IDC_SIZEALL));
            return TRUE;
        } else {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            SetCursor(LoadCursor(NULL, PtInRect(&po_close_rc, pt) ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
      case WM_LBUTTONUP: {
        POINT pt;
        pt.x = (short)LOWORD(lp);
        pt.y = (short)HIWORD(lp);
        if (PtInRect(&po_close_rc, pt))
            po_dismiss();
        return 0;
      }
      case WM_MOVING: {
        /* Dragged, it stays over the terminal area. */
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
        po_take_place();
        return 0;
      case WM_TIMER:
        if (wp == PO_TIMER)
            po_tick();
        return 0;
      case WM_PO_SHOW:
        po_show();
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

static bool po_create(void)
{
    static bool registered;
    if (po_hwnd)
        return true;
    if (!MainHwnd)
        return false;
    if (!registered) {
        WNDCLASSA wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = po_wndproc;
        wc.hInstance = GetModuleHandle(NULL);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.lpszClassName = PO_CLASS;
        if (!RegisterClassA(&wc))
            return false;
        registered = true;
    }
    /* Owned by the terminal window: above it, minimised with it, never in the
     * taskbar. The name is the header's text, for a harness to read. */
    po_hwnd = CreateWindowExA(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                              PO_CLASS, "", WS_POPUP, 0, 0, 1, 1, MainHwnd,
                              NULL, GetModuleHandle(NULL), NULL);
    return po_hwnd != NULL;
}

static bool po_owner_shown(void)
{
    return MainHwnd && IsWindowVisible(MainHwnd) && !IsIconic(MainHwnd);
}

static void po_show(void)
{
    if (!po_due || po_dismissed || !po_owner_shown()) {
        po_hide();
        return;
    }
    if (!po_create())
        return;
    po_render();
    SetWindowTextW(po_hwnd, po_head);
    if (!IsWindowVisible(po_hwnd))
        ShowWindow(po_hwnd, SW_SHOWNOACTIVATE);
}

/* ---- the records ---------------------------------------------------------- */

static void po_wide(wchar_t *dst, size_t n, const char *src, UINT cp)
{
    int k = MultiByteToWideChar(cp, 0, src, -1, NULL, 0);
    if (k <= 0) {
        dst[0] = 0;
        return;
    }
    if ((size_t)k <= n) {
        MultiByteToWideChar(cp, 0, src, -1, dst, (int)n);
    } else {
        wchar_t *tmp = (wchar_t *)malloc(sizeof(wchar_t) * k);
        if (!tmp) { dst[0] = 0; return; }
        MultiByteToWideChar(cp, 0, src, -1, tmp, k);
        wcsncpy(dst, tmp, n - 1);
        dst[n - 1] = 0;
        free(tmp);
    }
}

static const char *po_state_text(int state, int kind)
{
    switch (state) {
      case PS_WORKING: return KT_PSO_WORKING;
      case PS_ERROR:   return KT_PSO_FAILED;
      case PS_DONE:    return KT_PSO_DONE;
      default:
        return kind == PS_KIND_PERMISSION ? KT_PSO_PERMISSION :
               kind == PS_KIND_QUESTION   ? KT_PSO_QUESTION :
               kind == PS_KIND_AUTH       ? KT_PSO_LOGIN : KT_PSO_WAITING;
    }
}

void kitty_psoverlay_update(const PsStore *ps, const char *session,
                            const char *title, bool on)
{
    PsRow rows[PS_RECORDS_MAX];
    int n, i, j;
    char buf[PS_MSG_MAX + PS_APP_MAX + 8];

    if (!session)
        session = "";
    if (strcmp(session, po_session)) {
        snprintf(po_session, sizeof(po_session), "%s", session);
        po_have_pos = false;
    }
    n = ps_rows(ps, rows);
    po_ids_n = 0;
    for (i = 0; i < n; i++)
        strcpy(po_ids[po_ids_n++], rows[i].e->r.id);
    po_due = on && ps_overlay_due(ps);
    if (!po_due) {
        po_dismissed = false;          /* the next batch shows again */
        po_hide();
        return;
    }
    if (po_dismissed) {
        for (i = 0; i < po_ids_n && po_dismissed; i++) {
            for (j = 0; j < po_hidden_n; j++)
                if (!strcmp(po_ids[i], po_hidden_ids[j]))
                    break;
            if (j == po_hidden_n)
                po_dismissed = false;  /* a job it has not shown */
        }
        if (po_dismissed)
            return;
    }
    if (!po_have_pos) {
        po_dx = po_dy = PO_MARGIN;
        kitty_winpos_overlay_get(po_session, &po_dx, &po_dy);
        po_have_pos = true;
    }

    po_n = n < PO_MAXROWS ? n : PO_MAXROWS;
    po_more = n - po_n;
    for (i = 0; i < po_n; i++) {
        const PsRecord *r = &rows[i].e->r;
        PoRow *o = &po_rows[i];
        const char *app = ps_app_of(ps, r);
        memset(o, 0, sizeof(*o));
        o->state = r->state;
        o->kind = r->kind;
        o->progress = r->progress;
        o->depth = rows[i].depth;
        po_wide(o->name, 64, rows[i].name, CP_UTF8);
        po_wide(o->parent, 40, rows[i].parent, CP_UTF8);
        if (r->state == PS_WORKING && r->progress >= 0)
            snprintf(buf, sizeof(buf), "%d%%", r->progress);
        else
            snprintf(buf, sizeof(buf), "%s", po_state_text(r->state, r->kind));
        po_wide(o->right, 48, buf, CP_UTF8);
        /* The app, unless it is already the row's name, then the message. */
        if (app[0] && strcmp(app, rows[i].name) && r->msg[0])
            snprintf(buf, sizeof(buf), KT_PSO_DETAIL, app, r->msg);
        else
            snprintf(buf, sizeof(buf), "%s", r->msg[0] ? r->msg :
                     (strcmp(app, rows[i].name) ? app : ""));
        po_wide(o->detail, 160, buf, CP_UTF8);
    }
    snprintf(buf, sizeof(buf), KT_PSO_HEAD, title ? title : "");
    po_wide(po_head, 200, buf, CP_ACP);
    snprintf(buf, sizeof(buf), KT_PSO_JOBS, n);
    po_wide(po_jobs, 32, buf, CP_UTF8);
    po_show();
}

void kitty_psoverlay_follow(void)
{
    if (!po_hwnd)
        return;
    if (!po_due || po_dismissed || !po_owner_shown()) {
        po_hide();
        return;
    }
    /* Shown already: the same picture moves along; while the terminal is
     * dragged this runs for every step, so nothing is redrawn. */
    if (IsWindowVisible(po_hwnd) && po_dc && po_dpi() / 96.0 == po_s)
        po_move();
    else
        po_show();
}

bool kitty_psoverlay_rect(RECT *r)
{
    if (!po_hwnd || !IsWindowVisible(po_hwnd))
        return false;
    return GetWindowRect(po_hwnd, r) != 0;
}

void kitty_psoverlay_owner_shown(bool shown)
{
    if (!po_hwnd)
        return;
    if (!shown)
        po_hide();
    else                               /* not visible yet: once it is */
        PostMessage(po_hwnd, WM_PO_SHOW, 0, 0);
}

void kitty_psoverlay_close(void)
{
    if (po_hwnd) {
        KillTimer(po_hwnd, PO_TIMER);
        DestroyWindow(po_hwnd);
        po_hwnd = NULL;
    }
    po_animating = false;
    po_due = false;
    po_canvas_free();
    po_fonts_free();
}
