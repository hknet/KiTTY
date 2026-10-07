/*
 * paint-gdi.c - the GDI painter: the drawing calls window.c always made,
 * moved behind the painter table (paint.h) unchanged, so that a GPU painter
 * can sit beside them. Nothing here is new behaviour; every call and its
 * order is what do_text_internal, wintw_draw_cursor, wintw_draw_trust_sigil,
 * wintw_char_width and the WM_PAINT border fill did in place.
 */
#include "putty.h"
#include "paint.h"
#include "paint-widthcache.h"

/* KiTTY: one far2l image as the window shows it - scaled, composited on the
 * background colour, cut to the terminal area - in a memory DC, so a frame
 * copies the part it needs instead of scaling the picture again. */
typedef struct GdiOvCache {
    unsigned long serial;
    COLORREF bg;
    RECT dst, region;                  /* where the picture goes; the part kept */
    HDC mem;
    HBITMAP bmp;
    HGDIOBJ old;
    bool used;
} GdiOvCache;
/* At most this many pixels cached for all pictures together (64 MB at
 * 32 bpp); a picture past it is drawn straight from its pixels. */
#define GDI_OVC_MAX_PX (16LL * 1024 * 1024)

typedef struct GdiPainter {
    KittyPainter p;
    HWND hwnd;
    HPALETTE *pal;
    HDC hdc;
    bool owned;                        /* GetDC'd here, not WM_PAINT's */
    /* What this frame's DC holds already, so a style call sets only what
     * changed. Valid from the first style call of a frame; forgotten at
     * every begin (a new DC) and whenever the DC is handed out (kp_hdc: the
     * caller draws with GDI directly and may change anything). */
    bool st_valid;
    HFONT st_font;
    COLORREF st_fg, st_bg;
    int st_bkmode;
    UINT st_align;
    KittyWidthCache wc;                /* measured widths (paint-widthcache.h) */
    /* KiTTY: what this frame drew, for the overlay (gdi_overlay): a
     * bounding rectangle, or everything once the DC was handed out. */
    RECT touched;
    bool touched_any, touched_all;
    /* KiTTY: the overlay's finished pictures (gdi_overlay), kept while they
     * are shown. */
    GdiOvCache ovc[KITTY_OVERLAY_MAX];
    int novc;
} GdiPainter;

/* Widen this frame's drawn area (right and bottom exclusive). */
static void gdi_touch(GdiPainter *g, int l, int t, int r, int b)
{
    if (r <= l || b <= t)
        return;
    if (!g->touched_any) {
        g->touched.left = l; g->touched.top = t;
        g->touched.right = r; g->touched.bottom = b;
        g->touched_any = true;
        return;
    }
    if (l < g->touched.left) g->touched.left = l;
    if (t < g->touched.top) g->touched.top = t;
    if (r > g->touched.right) g->touched.right = r;
    if (b > g->touched.bottom) g->touched.bottom = b;
}

static void gdi_touch_rect(GdiPainter *g, const RECT *r)
{
    if (r)
        gdi_touch(g, r->left, r->top, r->right, r->bottom);
    else
        g->touched_all = true;
}

static bool gdi_begin(KittyPainter *p, HDC given)
{
    GdiPainter *g = (GdiPainter *)p;
    assert(!g->hdc);
    g->st_valid = false;
    g->touched_any = g->touched_all = false;
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
    int bkmode = opaque ? OPAQUE : TRANSPARENT;
    UINT align = TA_TOP | (centre ? TA_CENTER : TA_LEFT) | TA_NOUPDATECP;
    if (!g->st_valid || g->st_font != font)
        SelectObject(g->hdc, font);
    if (!g->st_valid || g->st_fg != fg)
        SetTextColor(g->hdc, fg);
    if (!g->st_valid || g->st_bg != bg)
        SetBkColor(g->hdc, bg);
    if (!g->st_valid || g->st_bkmode != bkmode)
        SetBkMode(g->hdc, bkmode);
    if (!g->st_valid || g->st_align != align)
        SetTextAlign(g->hdc, align);
    g->st_font = font; g->st_fg = fg; g->st_bg = bg;
    g->st_bkmode = bkmode; g->st_align = align;
    g->st_valid = true;
}

static void gdi_opaque(KittyPainter *p, bool opaque)
{
    GdiPainter *g = (GdiPainter *)p;
    int bkmode = opaque ? OPAQUE : TRANSPARENT;
    if (!g->st_valid || g->st_bkmode != bkmode)
        SetBkMode(g->hdc, bkmode);
    g->st_bkmode = bkmode;             /* the rest of the cache is untouched */
}

static void gdi_text_w(KittyPainter *p, int x, int y, const RECT *clip,
                       bool opaque, const wchar_t *s, int n, const int *dx)
{
    GdiPainter *g = (GdiPainter *)p;
    gdi_touch_rect(g, clip);
    ExtTextOutW(g->hdc, x, y, ETO_CLIPPED | (opaque ? ETO_OPAQUE : 0),
                clip, s, n, dx);
}

static void gdi_text_a(KittyPainter *p, int x, int y, const RECT *clip,
                       bool opaque, const char *s, int n, const int *dx)
{
    GdiPainter *g = (GdiPainter *)p;
    gdi_touch_rect(g, clip);
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

    gdi_touch_rect(g, lprc);
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
    gdi_touch_rect(g, dst);
    BitBlt(g->hdc, dst->left, dst->top,
           dst->right - dst->left, dst->bottom - dst->top,
           src, sx, sy, SRCCOPY);
}

static void gdi_line(KittyPainter *p, int x0, int y0, int x1, int y1,
                     COLORREF c)
{
    GdiPainter *g = (GdiPainter *)p;
    HPEN oldpen;
    gdi_touch(g, min(x0, x1), min(y0, y1), max(x0, x1) + 1, max(y0, y1) + 1);
    oldpen = SelectObject(g->hdc, CreatePen(PS_SOLID, 0, c));
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
    gdi_touch(g, r->left, r->top, r->right + 1, r->bottom + 1);
    oldpen = SelectObject(g->hdc, CreatePen(PS_SOLID, 0, c));
    Polyline(g->hdc, pts, 5);
    oldpen = SelectObject(g->hdc, oldpen);
    DeleteObject(oldpen);
}

static void gdi_pixel(KittyPainter *p, int x, int y, COLORREF c)
{
    GdiPainter *g = (GdiPainter *)p;
    gdi_touch(g, x, y, x + 1, y + 1);
    SetPixel(g->hdc, x, y, c);
}

static void gdi_icon(KittyPainter *p, int x, int y, HICON ic, int w, int h)
{
    GdiPainter *g = (GdiPainter *)p;
    gdi_touch(g, x, y, x + w, y + h);
    DrawIconEx(g->hdc, x, y, ic, w, h, 0, NULL, DI_NORMAL);
}

static void gdi_fill_rect(KittyPainter *p, const RECT *r, COLORREF c)
{
    GdiPainter *g = (GdiPainter *)p;
    HBRUSH brush = CreateSolidBrush(c);
    gdi_touch(g, r->left, r->top, r->right, r->bottom);
    FillRect(g->hdc, r, brush);
    DeleteObject(brush);
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
    if (kitty_wc_get(&g->wc, font, ch, wide, width))
        return true;                   /* measured before */
    SelectObject(g->hdc, font);
    g->st_font = font;                 /* the DC now holds this one */
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
    kitty_wc_put(&g->wc, font, ch, wide, ibuf);
    return true;
}

static HDC gdi_hdc(KittyPainter *p)
{
    GdiPainter *g = (GdiPainter *)p;
    g->st_valid = false;               /* drawn on directly: state unknown */
    g->touched_all = true;             /* ...and where, too */
    return g->hdc;
}

static void gdi_resize(KittyPainter *p, int w, int h)
{
    (void)p; (void)w; (void)h;         /* the window IS the surface */
}

static void gdi_ovc_free(GdiOvCache *c);
static void gdi_destroy(KittyPainter *p)
{
    GdiPainter *g = (GdiPainter *)p;
    int i;
    for (i = 0; i < g->novc; i++)       /* KiTTY: the overlay's pictures */
        gdi_ovc_free(&g->ovc[i]);
    RemovePropA(((GdiPainter *)p)->hwnd, "KiTTY.renderer");
    kitty_wc_free(&((GdiPainter *)p)->wc);
    sfree(p);
}

static HANDLE gdi_frame_signal(KittyPainter *p)
{
    (void)p;
    return NULL;                       /* GDI has no display signal */
}

static void gdi_fonts_changed(KittyPainter *p)
{
    /* GDI draws with the HFONTs as they are; only the measured widths can
     * belong to a recycled handle */
    kitty_wc_clear(&((GdiPainter *)p)->wc);
}

/* The band's pixels moved by dy within the window DC. What ScrollDC could
 * not copy (a covered part, on Windows without composition) comes back in
 * the update region beside the rows the caller draws anyway; that rest is
 * invalidated, so WM_PAINT repairs it from the terminal's own record. */
static bool gdi_scroll_rows(KittyPainter *p, const RECT *band, int dy)
{
    GdiPainter *g = (GdiPainter *)p;
    HRGN upd, fresh;
    RECT in = *band;
    if (!g->hdc || !g->owned)          /* never inside WM_PAINT's clipped DC */
        return false;
    upd = CreateRectRgn(0, 0, 0, 0);
    if (!ScrollDC(g->hdc, 0, dy, band, band, upd, NULL)) {
        DeleteObject(upd);
        return false;
    }
    if (dy < 0) in.top = band->bottom + dy; else in.bottom = band->top + dy;
    fresh = CreateRectRgnIndirect(&in);
    if (CombineRgn(upd, upd, fresh, RGN_DIFF) != NULLREGION)
        InvalidateRgn(g->hwnd, upd, FALSE);
    DeleteObject(fresh);
    DeleteObject(upd);
    return true;
}

/* AlphaBlend lives in msimg32.dll, which the program does not link: bound
 * on first use, as the background image does it. */
typedef BOOL (WINAPI *AlphaBlend_t)(HDC, int, int, int, int,
                                    HDC, int, int, int, int, BLENDFUNCTION);
static AlphaBlend_t gdi_alphablend(void)
{
    static bool tried;
    static AlphaBlend_t fn;
    if (!tried) {
        HMODULE m = LoadLibraryA("msimg32.dll");
        tried = true;
        if (m)
            fn = (AlphaBlend_t)(void *)GetProcAddress(m, "AlphaBlend");
    }
    return fn;
}

/* One picture, opaque: straight from its pixels, stretched to dst. */
static void gdi_stretch(HDC hdc, const KittyOverlayItem *it, const RECT *dst)
{
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = it->w;
    bi.bmiHeader.biHeight = -it->h;    /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    StretchDIBits(hdc, dst->left, dst->top,
                  dst->right - dst->left, dst->bottom - dst->top,
                  0, 0, it->w, it->h, it->bgra, &bi, DIB_RGB_COLORS, SRCCOPY);
}

/* One picture with transparency: the background colour, then the picture
 * blended over it through a DIB section (AlphaBlend reads a DC). Without
 * AlphaBlend the premultiplied pixels are drawn as they are, which is the
 * picture over black. */
static void gdi_blend(HDC hdc, const KittyOverlayItem *it, const RECT *dst,
                      COLORREF bg)
{
    AlphaBlend_t ab = gdi_alphablend();
    HBRUSH br;
    BITMAPINFO bi;
    HDC mem;
    HBITMAP dib;
    HGDIOBJ old;
    void *bits = NULL;
    BLENDFUNCTION bf;

    br = CreateSolidBrush(bg);
    FillRect(hdc, dst, br);
    DeleteObject(br);
    if (!ab) {
        gdi_stretch(hdc, it, dst);
        return;
    }
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = it->w;
    bi.bmiHeader.biHeight = -it->h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    mem = CreateCompatibleDC(hdc);
    if (!mem)
        return;
    dib = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!dib || !bits) {
        if (dib)
            DeleteObject(dib);
        DeleteDC(mem);
        return;
    }
    memcpy(bits, it->bgra, (size_t)it->w * it->h * 4);
    old = SelectObject(mem, dib);
    bf.BlendOp = AC_SRC_OVER;
    bf.BlendFlags = 0;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat = AC_SRC_ALPHA;
    ab(hdc, dst->left, dst->top, dst->right - dst->left, dst->bottom - dst->top,
       mem, 0, 0, it->w, it->h, bf);
    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
}

static void gdi_ovc_free(GdiOvCache *c)
{
    SelectObject(c->mem, c->old);
    DeleteObject(c->bmp);
    DeleteDC(c->mem);
}

/* The cached, finished pixels of one picture: scaled, composited on the
 * background, and only the part inside the terminal area. Built once per
 * picture, size, place and background; NULL when it cannot be (or the
 * cache is full: the caller then draws straight from the pixels). */
static GdiOvCache *gdi_ovc_get(GdiPainter *g, const KittyOverlayItem *it,
                               const RECT *region, COLORREF bg)
{
    GdiOvCache *c;
    RECT local;
    int i, w = region->right - region->left, h = region->bottom - region->top;
    long long px = 0;
    for (i = 0; i < g->novc; i++) {
        c = &g->ovc[i];
        if (c->serial == it->serial && c->bg == bg &&
            EqualRect(&c->dst, &it->dst) && EqualRect(&c->region, region)) {
            c->used = true;
            return c;
        }
        px += (long long)(c->region.right - c->region.left) *
              (c->region.bottom - c->region.top);
    }
    if (g->novc >= KITTY_OVERLAY_MAX || px + (long long)w * h > GDI_OVC_MAX_PX)
        return NULL;
    c = &g->ovc[g->novc];
    c->mem = CreateCompatibleDC(g->hdc);
    if (!c->mem)
        return NULL;
    c->bmp = CreateCompatibleBitmap(g->hdc, w, h);
    if (!c->bmp) {
        DeleteDC(c->mem);
        return NULL;
    }
    c->old = SelectObject(c->mem, c->bmp);
    SetStretchBltMode(c->mem, HALFTONE);
    SetBrushOrgEx(c->mem, 0, 0, NULL);
    local = it->dst;
    OffsetRect(&local, -region->left, -region->top);
    if (it->opaque)
        gdi_stretch(c->mem, it, &local);
    else
        gdi_blend(c->mem, it, &local, bg);
    c->serial = it->serial;
    c->bg = bg;
    c->dst = it->dst;
    c->region = *region;
    c->used = true;
    g->novc++;
    return c;
}

/* KiTTY: the overlay (far2l images), over what this frame drew. The window
 * is the surface, so a picture is drawn again wherever text under it was
 * redrawn: only that part (the frame's bounding rectangle, within the
 * terminal area) is copied, from a cached copy of the finished picture.
 * Drawn over its own earlier pixels it changes nothing (opaque, or
 * composited on the background colour). Cached pictures not shown in this
 * frame are freed, so a deleted or replaced picture goes at the next frame. */
static void gdi_overlay(KittyPainter *p, const KittyOverlayItem *items,
                        int n, const RECT *clip, COLORREF bg)
{
    GdiPainter *g = (GdiPainter *)p;
    RECT eff;
    int i, j, saved = 0;
    bool drew = false;

    if (!g->hdc)
        return;
    for (j = 0; j < g->novc; j++)
        g->ovc[j].used = false;
    eff = *clip;
    if (!g->touched_all && g->touched_any)
        IntersectRect(&eff, &eff, &g->touched);
    else if (!g->touched_all)
        SetRectEmpty(&eff);
    for (i = 0; i < n; i++) {
        const KittyOverlayItem *it = &items[i];
        RECT region, part;
        GdiOvCache *c;
        if (it->w <= 0 || it->h <= 0 || !it->bgra ||
            !IntersectRect(&region, &it->dst, clip))
            continue;
        if (!IntersectRect(&part, &region, &eff)) {
            /* not drawn this frame, but still shown: keep its copy */
            for (j = 0; j < g->novc; j++)
                if (g->ovc[j].serial == it->serial)
                    g->ovc[j].used = true;
            continue;
        }
        c = gdi_ovc_get(g, it, &region, bg);
        if (c) {
            BitBlt(g->hdc, part.left, part.top, part.right - part.left,
                   part.bottom - part.top, c->mem,
                   part.left - region.left, part.top - region.top, SRCCOPY);
        } else {
            /* no copy to be had: straight from the pixels, clipped */
            if (!drew) {
                saved = SaveDC(g->hdc);
                drew = true;
            }
            SelectClipRgn(g->hdc, NULL);
            IntersectClipRect(g->hdc, part.left, part.top, part.right, part.bottom);
            SetStretchBltMode(g->hdc, HALFTONE);
            SetBrushOrgEx(g->hdc, 0, 0, NULL);
            if (it->opaque)
                gdi_stretch(g->hdc, it, &it->dst);
            else
                gdi_blend(g->hdc, it, &it->dst, bg);
        }
    }
    if (drew) {
        RestoreDC(g->hdc, saved);
        g->st_valid = false;           /* RestoreDC put back the old state */
    }
    for (i = j = 0; i < g->novc; i++) {
        if (g->ovc[i].used)
            g->ovc[j++] = g->ovc[i];
        else
            gdi_ovc_free(&g->ovc[i]);
    }
    g->novc = j;
}

static const KittyPainterVtable gdi_vt = {
    .begin = gdi_begin,
    .end = gdi_end,
    .resize = gdi_resize,
    .destroy = gdi_destroy,
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
    .fill_rect = gdi_fill_rect,
    .char_width = gdi_char_width,
    .hdc = gdi_hdc,
    .frame_signal = gdi_frame_signal,
    .fonts_changed = gdi_fonts_changed,
    .scroll_rows = gdi_scroll_rows,
    .overlay = gdi_overlay,
};

KittyPainter *kitty_painter_gdi_new(HWND hwnd, HPALETTE *pal)
{
    GdiPainter *g = snew(GdiPainter);
    memset(g, 0, sizeof(*g));
    g->p.vt = &gdi_vt;
    g->hwnd = hwnd;
    g->pal = pal;
    /* Which painter the window has (paint-d2d.c sets 2 or 3); read by
     * the renderer QA harness across processes. */
    SetPropA(hwnd, "KiTTY.renderer", (HANDLE)1);
    return &g->p;
}
