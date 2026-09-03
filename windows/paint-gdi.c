/*
 * paint-gdi.c - the GDI painter: the drawing calls window.c always made,
 * moved behind the painter table (paint.h) unchanged, so that a GPU painter
 * can sit beside them. Nothing here is new behaviour; every call and its
 * order is what do_text_internal, wintw_draw_cursor, wintw_draw_trust_sigil,
 * wintw_char_width and the WM_PAINT border fill did in place.
 */
#include "putty.h"
#include "paint.h"

typedef struct GdiPainter {
    KittyPainter p;
    HWND hwnd;
    HPALETTE *pal;
    HDC hdc;
    bool owned;                        /* GetDC'd here, not WM_PAINT's */
} GdiPainter;

static bool gdi_begin(KittyPainter *p, HDC given)
{
    GdiPainter *g = (GdiPainter *)p;
    assert(!g->hdc);
    if (given) {
        g->hdc = given;
        g->owned = false;
        return true;
    }
    if (!g->hwnd)
        return false;
    g->hdc = GetDC(g->hwnd);
    if (!g->hdc)
        return false;
    SelectPalette(g->hdc, *g->pal, false);
    g->owned = true;
    return true;
}

static void gdi_end(KittyPainter *p)
{
    GdiPainter *g = (GdiPainter *)p;
    assert(g->hdc);
    if (g->owned) {
        SelectPalette(g->hdc, GetStockObject(DEFAULT_PALETTE), false);
        ReleaseDC(g->hwnd, g->hdc);
    }
    g->hdc = NULL;
}

static void gdi_style(KittyPainter *p, HFONT font, COLORREF fg, COLORREF bg,
                      bool opaque, bool centre)
{
    GdiPainter *g = (GdiPainter *)p;
    SelectObject(g->hdc, font);
    SetTextColor(g->hdc, fg);
    SetBkColor(g->hdc, bg);
    SetBkMode(g->hdc, opaque ? OPAQUE : TRANSPARENT);
    SetTextAlign(g->hdc, TA_TOP | (centre ? TA_CENTER : TA_LEFT) |
                 TA_NOUPDATECP);
}

static void gdi_opaque(KittyPainter *p, bool opaque)
{
    GdiPainter *g = (GdiPainter *)p;
    SetBkMode(g->hdc, opaque ? OPAQUE : TRANSPARENT);
}

static void gdi_text_w(KittyPainter *p, int x, int y, const RECT *clip,
                       bool opaque, const wchar_t *s, int n, const int *dx)
{
    GdiPainter *g = (GdiPainter *)p;
    ExtTextOutW(g->hdc, x, y, ETO_CLIPPED | (opaque ? ETO_OPAQUE : 0),
                clip, s, n, dx);
}

static void gdi_text_a(KittyPainter *p, int x, int y, const RECT *clip,
                       bool opaque, const char *s, int n, const int *dx)
{
    GdiPainter *g = (GdiPainter *)p;
    ExtTextOut(g->hdc, x, y, ETO_CLIPPED | (opaque ? ETO_OPAQUE : 0),
               clip, s, n, dx);
}

/*
 * Wrapper around ExtTextOut() which takes care of text that contains
 * right-to-left characters: they are placed glyph by glyph through
 * GetCharacterPlacement, so that Windows does no bidi or Arabic shaping
 * of its own and we know which character went where.
 */
static void exact_textout(HDC hdc, int x, int y, CONST RECT *lprc,
                          const unsigned short *lpString, UINT cbCount,
                          CONST INT *lpDx, bool opaque)
{
#if HAVE_GCP_RESULTSW
    GCP_RESULTSW gcpr;
#else
    /*
     * If building against old enough headers that the GCP_RESULTSW
     * type isn't available, we can make do with GCP_RESULTS proper:
     * the differences aren't important to us (the only variable-width
     * string parameter is one we don't use anyway).
     */
    GCP_RESULTS gcpr;
#endif
    char *buffer = snewn(cbCount*2+2, char);
    char *classbuffer = snewn(cbCount, char);
    memset(&gcpr, 0, sizeof(gcpr));
    memset(buffer, 0, cbCount*2+2);
    memset(classbuffer, GCPCLASS_NEUTRAL, cbCount);

    gcpr.lStructSize = sizeof(gcpr);
    gcpr.lpGlyphs = (void *)buffer;
    gcpr.lpClass = (void *)classbuffer;
    gcpr.nGlyphs = cbCount;
    GetCharacterPlacementW(hdc, lpString, cbCount, 0, &gcpr,
                           FLI_MASK | GCP_CLASSIN | GCP_DIACRITIC);

    ExtTextOut(hdc, x, y,
               ETO_GLYPH_INDEX | ETO_CLIPPED | (opaque ? ETO_OPAQUE : 0),
               lprc, buffer, cbCount, lpDx);
    sfree(buffer);
    sfree(classbuffer);
}

/*
 * The exact_textout() wrapper, unfortunately, destroys the useful
 * Windows `font linking' behaviour: automatic handling of Unicode
 * code points not supported in this font by falling back to a font
 * which does contain them. Therefore, we adopt a multi-layered
 * approach: for any potentially-bidi text, we use exact_textout(),
 * and for everything else we use a simple ExtTextOut as we did
 * before exact_textout() was introduced.
 */
static void gdi_text_general(KittyPainter *p, int x, int y, const RECT *lprc,
                             bool opaque, bool varpitch,
                             const wchar_t *lpString, int cbCount,
                             const int *lpDx)
{
    GdiPainter *g = (GdiPainter *)p;
    HDC hdc = g->hdc;
    int i, j, xp, xn;
    int bkmode = 0;
    bool got_bkmode = false;

    xp = xn = x;

    for (i = 0; i < cbCount ;) {
        bool rtl = is_rtl(lpString[i]);

        xn += lpDx[i];

        for (j = i+1; j < cbCount; j++) {
            if (rtl != is_rtl(lpString[j]))
                break;
            xn += lpDx[j];
        }

        /*
         * Now [i,j) indicates a maximal substring of lpString
         * which should be displayed using the same textout
         * function.
         */
        if (rtl) {
            exact_textout(hdc, xp, y, lprc, lpString+i, j-i,
                          varpitch ? NULL : lpDx+i, opaque);
        } else {
            ExtTextOutW(hdc, xp, y, ETO_CLIPPED | (opaque ? ETO_OPAQUE : 0),
                        lprc, lpString+i, j-i,
                        varpitch ? NULL : lpDx+i);
        }

        i = j;
        xp = xn;

        bkmode = GetBkMode(hdc);
        got_bkmode = true;
        SetBkMode(hdc, TRANSPARENT);
        opaque = false;
    }

    if (got_bkmode)
        SetBkMode(hdc, bkmode);
}

static void gdi_blit_background(KittyPainter *p, const RECT *dst,
                                HDC src, int sx, int sy)
{
    GdiPainter *g = (GdiPainter *)p;
    BitBlt(g->hdc, dst->left, dst->top,
           dst->right - dst->left, dst->bottom - dst->top,
           src, sx, sy, SRCCOPY);
}

static void gdi_line(KittyPainter *p, int x0, int y0, int x1, int y1,
                     COLORREF c)
{
    GdiPainter *g = (GdiPainter *)p;
    HPEN oldpen = SelectObject(g->hdc, CreatePen(PS_SOLID, 0, c));
    MoveToEx(g->hdc, x0, y0, NULL);
    LineTo(g->hdc, x1, y1);
    oldpen = SelectObject(g->hdc, oldpen);
    DeleteObject(oldpen);
}

static void gdi_rect_outline(KittyPainter *p, const RECT *r, COLORREF c)
{
    GdiPainter *g = (GdiPainter *)p;
    POINT pts[5];
    HPEN oldpen;
    pts[0].x = pts[1].x = pts[4].x = r->left;
    pts[2].x = pts[3].x = r->right;
    pts[0].y = pts[3].y = pts[4].y = r->top;
    pts[1].y = pts[2].y = r->bottom;
    oldpen = SelectObject(g->hdc, CreatePen(PS_SOLID, 0, c));
    Polyline(g->hdc, pts, 5);
    oldpen = SelectObject(g->hdc, oldpen);
    DeleteObject(oldpen);
}

static void gdi_pixel(KittyPainter *p, int x, int y, COLORREF c)
{
    GdiPainter *g = (GdiPainter *)p;
    SetPixel(g->hdc, x, y, c);
}

static void gdi_icon(KittyPainter *p, int x, int y, HICON ic, int w, int h)
{
    GdiPainter *g = (GdiPainter *)p;
    DrawIconEx(g->hdc, x, y, ic, w, h, 0, NULL, DI_NORMAL);
}

static void gdi_fill_outside(KittyPainter *p, const RECT *paint,
                             const RECT *keep, COLORREF c)
{
    GdiPainter *g = (GdiPainter *)p;
    HDC hdc = g->hdc;
    HBRUSH fillcolour, oldbrush;
    HPEN edge, oldpen;
    fillcolour = CreateSolidBrush(c);
    oldbrush = SelectObject(hdc, fillcolour);
    edge = CreatePen(PS_SOLID, 0, c);
    oldpen = SelectObject(hdc, edge);

    /*
     * Jordan Russell reports that this apparently
     * ineffectual IntersectClipRect() call masks a
     * Windows NT/2K bug causing strange display
     * problems when the PuTTY window is taller than
     * the primary monitor. It seems harmless enough...
     */
    IntersectClipRect(hdc, paint->left, paint->top,
                      paint->right, paint->bottom);

    ExcludeClipRect(hdc, keep->left, keep->top, keep->right, keep->bottom);

    Rectangle(hdc, paint->left, paint->top, paint->right, paint->bottom);

    /* SelectClipRgn(hdc, NULL); */

    SelectObject(hdc, oldbrush);
    DeleteObject(fillcolour);
    SelectObject(hdc, oldpen);
    DeleteObject(edge);
}

static bool gdi_char_width(KittyPainter *p, HFONT font, unsigned ch,
                           bool wide, int *width)
{
    GdiPainter *g = (GdiPainter *)p;
    int ibuf = 0;
    SelectObject(g->hdc, font);
    if (wide) {
        if (GetCharWidth32W(g->hdc, ch, ch, &ibuf) == 1)
            /* Okay that one worked */ ;
        else if (GetCharWidthW(g->hdc, ch, ch, &ibuf) == 1)
            /* This should work on 9x too, but it's "less accurate" */ ;
        else
            return false;
    } else {
        if (GetCharWidth32(g->hdc, ch, ch, &ibuf) != 1 &&
            GetCharWidth(g->hdc, ch, ch, &ibuf) != 1)
            return false;
    }
    *width = ibuf;
    return true;
}

static HDC gdi_hdc(KittyPainter *p)
{
    return ((GdiPainter *)p)->hdc;
}

static const KittyPainterVtable gdi_vt = {
    .begin = gdi_begin,
    .end = gdi_end,
    .style = gdi_style,
    .opaque = gdi_opaque,
    .text_w = gdi_text_w,
    .text_a = gdi_text_a,
    .text_general = gdi_text_general,
    .blit_background = gdi_blit_background,
    .line = gdi_line,
    .rect_outline = gdi_rect_outline,
    .pixel = gdi_pixel,
    .icon = gdi_icon,
    .fill_outside = gdi_fill_outside,
    .char_width = gdi_char_width,
    .hdc = gdi_hdc,
};

KittyPainter *kitty_painter_gdi_new(HWND hwnd, HPALETTE *pal)
{
    GdiPainter *g = snew(GdiPainter);
    memset(g, 0, sizeof(*g));
    g->p.vt = &gdi_vt;
    g->hwnd = hwnd;
    g->pal = pal;
    return &g->p;
}

void kitty_painter_free(KittyPainter *p)
{
    sfree(p);
}
