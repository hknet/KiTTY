/*
 * paint-d2d.c - the Direct2D + DirectWrite painter (paint.h).
 *
 * The device layer, shared with any later Direct3D renderer: a Direct3D 11
 * device, a DXGI flip-model swap chain on the terminal window, and the frame
 * loop. Direct2D draws into a PERSISTENT canvas bitmap - the terminal paints
 * only the cells that changed, and a swap chain's back buffer holds nothing
 * dependable between frames - and each frame ends by copying the canvas to
 * the back buffer and presenting it. Text is DirectWrite glyph runs placed
 * with the terminal's own per-cell advances, so the grid is the terminal's;
 * fonts come from the window's HFONTs through the GDI interop, and the
 * baseline from GDI's own metrics for the same font, so text sits where GDI
 * put it. The trust sigil and the background-image blit go through the GDI
 * interop of the device context (a DC on the canvas), which keeps them
 * exact until a native form is worth having.
 *
 * Everything is bound at run time (d3d11.dll, d2d1.dll, dwrite.dll): a build
 * that also runs on old Windows must not import them, and a machine that
 * cannot create the device simply stays on GDI (kitty_painter_d2d_new
 * returns NULL). Windows 8.1 or later is the honest floor: Direct2D 1.1 and
 * the flip-model swap chain, and PrintWindow's PW_RENDERFULLCONTENT for the
 * harnesses that capture the window.
 *
 * Glyphs the primary font lacks come from the configured fallback list
 * ([FontFallback] in kitty.ini - the list the GDI painter's module uses)
 * and then from Windows' own DirectWrite fallback, monochrome.
 *
 * Not yet (see design/TASK_gpu_renderer.md): right-to-left shaping
 * (runs are placed glyph by glyph like the GDI exact_textout path, without
 * the reordering GetCharacterPlacement did), and a native background image.
 */
#define COBJMACROS
#define INITGUID
#include "putty.h"
#include <initguid.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <dwrite_2.h>
#include "paint.h"

typedef HRESULT (WINAPI *D3D11CreateDevice_t)(
    IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *,
    UINT, UINT, ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
typedef HRESULT (WINAPI *D2D1CreateFactory_t)(
    D2D1_FACTORY_TYPE, REFIID, const D2D1_FACTORY_OPTIONS *, void **);
typedef HRESULT (WINAPI *DWriteCreateFactory_t)(
    DWRITE_FACTORY_TYPE, REFIID, IUnknown **);

#define D2D_FONT_CACHE 24

typedef struct D2DFont {
    HFONT hfont;
    IDWriteFontFace *face;
    float emsize;                      /* pixels per em */
    float ascent;                      /* GDI's tmAscent: the baseline */
    float units_per_em;
    bool underline;                    /* lfUnderline: GDI draws it, we must */
    WCHAR family[LF_FACESIZE];         /* for the fallback lookups */
    DWRITE_FONT_WEIGHT weight;
    DWRITE_FONT_STYLE style;
    float ul_top, ul_height;           /* pixels below the top of the cell */
} D2DFont;


/* A face found for glyphs the primary font lacks, keyed by family + weight
 * + style; `face` NULL = the family is not installed (cached so the lookup
 * is not repeated). */
typedef struct D2DFace {
    WCHAR family[LF_FACESIZE];
    DWRITE_FONT_WEIGHT weight;
    DWRITE_FONT_STYLE style;
    IDWriteFontFace *face;
} D2DFace;
#define D2D_FB_CACHE 16
/* code point + primary font -> fallback face, direct-mapped */
typedef struct D2DFbEnt { UINT32 cp; short font; short face; bool valid; } D2DFbEnt;
#define D2D_FB_MAP 1024

/* The configured fallback list ([FontFallback] in kitty.ini), owned by the
 * GDI fallback module; the same names serve here. */
int winfb_slot_count(void);
const char *winfb_slot_name(int i);

/* IDWriteFontFallback::MapCharacters reads its text through this
 * interface; one code point at a time is all it is asked here. */
typedef struct CpSource {
    const IDWriteTextAnalysisSourceVtbl *lpVtbl;
    const WCHAR *text;
    UINT32 len;
} CpSource;
static HRESULT STDMETHODCALLTYPE src_qi(IDWriteTextAnalysisSource *t,
                                        REFIID riid, void **out)
{
    if (IsEqualIID(riid, &IID_IUnknown) ||
        IsEqualIID(riid, &IID_IDWriteTextAnalysisSource)) {
        *out = t;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE src_ref(IDWriteTextAnalysisSource *t)
{
    return 1;                            /* lives on the caller's stack */
}
static HRESULT STDMETHODCALLTYPE src_at(IDWriteTextAnalysisSource *t,
                                        UINT32 pos, const WCHAR **text,
                                        UINT32 *n)
{
    CpSource *s = (CpSource *)t;
    if (pos < s->len) { *text = s->text + pos; *n = s->len - pos; }
    else { *text = NULL; *n = 0; }
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE src_before(IDWriteTextAnalysisSource *t,
                                            UINT32 pos, const WCHAR **text,
                                            UINT32 *n)
{
    CpSource *s = (CpSource *)t;
    if (pos > s->len) pos = s->len;
    *text = pos ? s->text : NULL; *n = pos;
    return S_OK;
}
static DWRITE_READING_DIRECTION STDMETHODCALLTYPE src_dir(
    IDWriteTextAnalysisSource *t)
{
    return DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;
}
static HRESULT STDMETHODCALLTYPE src_locale(IDWriteTextAnalysisSource *t,
                                            UINT32 pos, UINT32 *n,
                                            const WCHAR **locale)
{
    CpSource *s = (CpSource *)t;
    *n = pos < s->len ? s->len - pos : 0; *locale = L"en-us";
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE src_numsub(IDWriteTextAnalysisSource *t,
                                            UINT32 pos, UINT32 *n,
                                            IDWriteNumberSubstitution **sub)
{
    CpSource *s = (CpSource *)t;
    *n = pos < s->len ? s->len - pos : 0; *sub = NULL;
    return S_OK;
}
static const IDWriteTextAnalysisSourceVtbl src_vt = {
    src_qi, src_ref, src_ref, src_at, src_before, src_dir, src_locale, src_numsub
};

typedef struct D2DPainter {
    KittyPainter p;
    HWND hwnd;
    bool warp;                  /* the device is WARP, the software rasteriser */
    HMODULE d3d11dll, d2d1dll, dwritedll;
    ID3D11Device *d3d;
    ID3D11DeviceContext *d3dctx;
    IDXGISwapChain1 *swap;
    ID2D1Factory1 *factory;
    ID2D1Device *device;
    ID2D1DeviceContext *dc;
    ID2D1Bitmap1 *target;              /* the swap chain's back buffer */
    ID2D1Bitmap1 *canvas;              /* what the window shows, persistent */
    ID2D1SolidColorBrush *brush;
    IDWriteFactory *dw;
    IDWriteGdiInterop *gdi;
    int width, height;
    bool in_frame;
    D2D1_TEXT_ANTIALIAS_MODE textaa;
    /* the current style */
    D2DFont *font;
    COLORREF fg, bg;
    bool opaque, centre;
    D2DFont fonts[D2D_FONT_CACHE];
    int nfonts;
    /* font fallback */
    IDWriteFontFallback *fallback;     /* Windows' own (DirectWrite 2), or NULL */
    IDWriteFontCollection *sysfonts;
    D2DFace fbfaces[D2D_FB_CACHE];
    int nfb;
    D2DFbEnt fbmap[D2D_FB_MAP];
} D2DPainter;

static D2D1_COLOR_F colour_of(COLORREF c)
{
    D2D1_COLOR_F f;
    f.r = GetRValue(c) / 255.0f;
    f.g = GetGValue(c) / 255.0f;
    f.b = GetBValue(c) / 255.0f;
    f.a = 1.0f;
    return f;
}

static D2D1_RECT_F rectf(int l, int t, int r, int b)
{
    D2D1_RECT_F f;
    f.left = (float)l; f.top = (float)t; f.right = (float)r; f.bottom = (float)b;
    return f;
}

static void fill(D2DPainter *d, int l, int t, int r, int b, COLORREF c)
{
    D2D1_COLOR_F col = colour_of(c);
    D2D1_RECT_F rc = rectf(l, t, r, b);
    if (r <= l || b <= t)
        return;
    ID2D1SolidColorBrush_SetColor(d->brush, &col);
    ID2D1RenderTarget_FillRectangle((ID2D1RenderTarget *)d->dc, &rc,
                                    (ID2D1Brush *)d->brush);
}

/* ---- the swap chain's two bitmaps ---------------------------------- */

static void release_targets(D2DPainter *d)
{
    ID2D1DeviceContext_SetTarget(d->dc, NULL);
    if (d->target) { IUnknown_Release((IUnknown *)d->target); d->target = NULL; }
    if (d->canvas) { IUnknown_Release((IUnknown *)d->canvas); d->canvas = NULL; }
}

static bool create_targets(D2DPainter *d, int w, int h)
{
    IDXGISurface *surface = NULL;
    D2D1_BITMAP_PROPERTIES1 props;
    D2D1_SIZE_U size;
    D2D1_COLOR_F black = { 0, 0, 0, 1 };
    HRESULT hr;

    if (w < 1) w = 1;
    if (h < 1) h = 1;
    memset(&props, 0, sizeof(props));
    props.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_IGNORE;
    props.dpiX = 96; props.dpiY = 96;

    hr = IDXGISwapChain1_GetBuffer(d->swap, 0, &IID_IDXGISurface,
                                   (void **)&surface);
    if (FAILED(hr))
        return false;
    props.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET |
        D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
    hr = ID2D1DeviceContext_CreateBitmapFromDxgiSurface(
        d->dc, surface, &props, &d->target);
    IDXGISurface_Release(surface);
    if (FAILED(hr))
        return false;

    /* GDI_COMPATIBLE: the interop DC (icon, background blit) needs it. */
    props.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET |
        D2D1_BITMAP_OPTIONS_GDI_COMPATIBLE;
    size.width = w; size.height = h;
    hr = d->dc->lpVtbl->CreateBitmap(d->dc, size, NULL, 0, &props,
                                         &d->canvas);
    if (FAILED(hr))
        return false;
    d->width = w; d->height = h;

    /* A fresh canvas is black until the terminal repaints into it. */
    ID2D1DeviceContext_SetTarget(d->dc, (struct ID2D1Image *)d->canvas);
    ID2D1RenderTarget_BeginDraw((ID2D1RenderTarget *)d->dc);
    ID2D1RenderTarget_Clear((ID2D1RenderTarget *)d->dc, &black);
    ID2D1RenderTarget_EndDraw((ID2D1RenderTarget *)d->dc, NULL, NULL);
    return true;
}

static void d2d_resize(KittyPainter *p, int w, int h)
{
    D2DPainter *d = (D2DPainter *)p;
    if (!d->swap || d->in_frame)
        return;
    if (w < 1 || h < 1)
        return;
    if (w == d->width && h == d->height)
        return;
    release_targets(d);
    IDXGISwapChain1_ResizeBuffers(d->swap, 0, w, h, DXGI_FORMAT_UNKNOWN, 0);
    create_targets(d, w, h);
}

/* ---- frames -------------------------------------------------------- */

static bool d2d_begin(KittyPainter *p, HDC given)
{
    D2DPainter *d = (D2DPainter *)p;
    (void)given;                       /* WM_PAINT's DC: not ours to use */
    if (d->in_frame || !d->canvas)
        return false;
    ID2D1DeviceContext_SetTarget(d->dc, (struct ID2D1Image *)d->canvas);
    ID2D1RenderTarget_BeginDraw((ID2D1RenderTarget *)d->dc);
    ID2D1RenderTarget_SetAntialiasMode((ID2D1RenderTarget *)d->dc,
                                       D2D1_ANTIALIAS_MODE_ALIASED);
    ID2D1RenderTarget_SetTextAntialiasMode((ID2D1RenderTarget *)d->dc,
                                           d->textaa);
    d->in_frame = true;
    return true;
}

static void d2d_end(KittyPainter *p)
{
    D2DPainter *d = (D2DPainter *)p;
    D2D1_RECT_F all;
    if (!d->in_frame)
        return;
    ID2D1RenderTarget_EndDraw((ID2D1RenderTarget *)d->dc, NULL, NULL);
    d->in_frame = false;

    /* The canvas to the back buffer, and up. Present with no vsync wait:
     * the terminal's own update pacing decides the frame rate, and a wait
     * here would stall the thread that also reads the network. */
    all = rectf(0, 0, d->width, d->height);
    ID2D1DeviceContext_SetTarget(d->dc, (struct ID2D1Image *)d->target);
    ID2D1RenderTarget_BeginDraw((ID2D1RenderTarget *)d->dc);
    ID2D1RenderTarget_DrawBitmap((ID2D1RenderTarget *)d->dc,
                                 (ID2D1Bitmap *)d->canvas, &all, 1.0f,
                                 D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR,
                                 &all);
    ID2D1RenderTarget_EndDraw((ID2D1RenderTarget *)d->dc, NULL, NULL);
    IDXGISwapChain1_Present(d->swap, 0, 0);
    ID2D1DeviceContext_SetTarget(d->dc, NULL);
}

/* ---- fonts --------------------------------------------------------- */

static D2DFont *font_of(D2DPainter *d, HFONT hfont)
{
    int i;
    LOGFONTW lf;
    IDWriteFont *font = NULL;
    D2DFont *f;
    DWRITE_FONT_METRICS fm;
    HDC screen;
    TEXTMETRICW tm;
    HFONT old;

    for (i = 0; i < d->nfonts; i++)
        if (d->fonts[i].hfont == hfont)
            return &d->fonts[i];
    if (!hfont || d->nfonts >= D2D_FONT_CACHE)
        return NULL;
    if (!GetObjectW(hfont, sizeof(lf), &lf))
        return NULL;
    if (FAILED(IDWriteGdiInterop_CreateFontFromLOGFONT(d->gdi, &lf, &font)))
        return NULL;
    f = &d->fonts[d->nfonts];
    memset(f, 0, sizeof(*f));
    if (FAILED(IDWriteFont_CreateFontFace(font, &f->face))) {
        IDWriteFont_Release(font);
        return NULL;
    }
    IDWriteFont_Release(font);
    f->hfont = hfont;
    f->underline = lf.lfUnderline != 0;
    wcsncpy(f->family, lf.lfFaceName, LF_FACESIZE - 1);
    f->weight = lf.lfWeight >= FW_BOLD ? DWRITE_FONT_WEIGHT_BOLD
                                       : DWRITE_FONT_WEIGHT_NORMAL;
    f->style = lf.lfItalic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL;
    IDWriteFontFace_GetMetrics(f->face, &fm);
    f->units_per_em = (float)fm.designUnitsPerEm;
    /* The em size in pixels: a negative lfHeight IS the em height; a
     * positive one is the cell height, of which the em is the part below
     * the internal leading. */
    if (lf.lfHeight < 0)
        f->emsize = (float)(-lf.lfHeight);
    else
        f->emsize = lf.lfHeight * f->units_per_em /
            (float)(fm.ascent + fm.descent);
    /* The baseline where GDI put it: TA_TOP + tmAscent. */
    screen = GetDC(NULL);
    old = SelectObject(screen, hfont);
    if (GetTextMetricsW(screen, &tm))
        f->ascent = (float)tm.tmAscent;
    else
        f->ascent = fm.ascent * f->emsize / f->units_per_em;
    SelectObject(screen, old);
    ReleaseDC(NULL, screen);
    /* The face's own underline (what GDI draws for lfUnderline):
     * position is measured from the baseline, negative = below. */
    {
        float scale = f->emsize / f->units_per_em;
        float th = fm.underlineThickness * scale;
        f->ul_height = th < 1 ? 1 : (float)(int)(th + 0.5f);
        f->ul_top = (float)(int)(f->ascent - fm.underlinePosition * scale + 0.5f);
    }
    d->nfonts++;
    return f;
}

/* ---- text ---------------------------------------------------------- */

static void d2d_style(KittyPainter *p, HFONT font, COLORREF fg, COLORREF bg,
                      bool opaque, bool centre)
{
    D2DPainter *d = (D2DPainter *)p;
    d->font = font_of(d, font);
    d->fg = fg; d->bg = bg;
    d->opaque = opaque; d->centre = centre;
}

static void d2d_opaque(KittyPainter *p, bool opaque)
{
    ((D2DPainter *)p)->opaque = opaque;
}


/* ---- font fallback ------------------------------------------------- */

static bool face_has(IDWriteFontFace *face, UINT32 cp)
{
    UINT16 g = 0;
    return face && SUCCEEDED(IDWriteFontFace_GetGlyphIndices(face, &cp, 1, &g))
        && g != 0;
}

/* The cache slot for a family (installed or not); -1 = cache full. */
static int face_of_family_w(D2DPainter *d, const WCHAR *family,
                            DWRITE_FONT_WEIGHT weight, DWRITE_FONT_STYLE style,
                            IDWriteFont *ready)
{
    int i;
    D2DFace *f;
    for (i = 0; i < d->nfb; i++)
        if (!_wcsicmp(d->fbfaces[i].family, family) &&
            d->fbfaces[i].weight == weight && d->fbfaces[i].style == style)
            return i;
    if (d->nfb >= D2D_FB_CACHE)
        return -1;
    f = &d->fbfaces[d->nfb];
    memset(f, 0, sizeof(*f));
    wcsncpy(f->family, family, LF_FACESIZE - 1);
    f->weight = weight; f->style = style;
    if (ready) {
        IDWriteFont_CreateFontFace(ready, &f->face);
    } else if (d->sysfonts) {
        UINT32 idx = 0;
        BOOL exists = FALSE;
        IDWriteFontFamily *fam = NULL;
        IDWriteFont *font = NULL;
        if (SUCCEEDED(IDWriteFontCollection_FindFamilyName(
                          d->sysfonts, family, &idx, &exists)) && exists &&
            SUCCEEDED(IDWriteFontCollection_GetFontFamily(d->sysfonts, idx, &fam))) {
            if (SUCCEEDED(IDWriteFontFamily_GetFirstMatchingFont(
                              fam, weight, DWRITE_FONT_STRETCH_NORMAL, style,
                              &font))) {
                IDWriteFont_CreateFontFace(font, &f->face);
                IDWriteFont_Release(font);
            }
            IDWriteFontFamily_Release(fam);
        }
    }
    return d->nfb++;
}

static int face_of_family(D2DPainter *d, const char *family,
                          DWRITE_FONT_WEIGHT weight, DWRITE_FONT_STYLE style)
{
    WCHAR w[LF_FACESIZE];
    if (!family || !*family ||
        !MultiByteToWideChar(CP_ACP, 0, family, -1, w, LF_FACESIZE))
        return -1;
    return face_of_family_w(d, w, weight, style, NULL);
}

/* Windows' own choice for a code point the primary font lacks. */
static int system_fallback(D2DPainter *d, D2DFont *pf, UINT32 cp)
{
    WCHAR u[2];
    UINT32 nu = 0, mapped = 0;
    FLOAT scale = 1;
    IDWriteFont *font = NULL;
    IDWriteFontFamily *fam = NULL;
    IDWriteLocalizedStrings *names = NULL;
    WCHAR family[LF_FACESIZE];
    CpSource src;
    int idx = -1;

    if (cp >= 0x10000) {
        u[nu++] = (WCHAR)(0xD800 + ((cp - 0x10000) >> 10));
        u[nu++] = (WCHAR)(0xDC00 + ((cp - 0x10000) & 0x3FF));
    } else
        u[nu++] = (WCHAR)cp;
    src.lpVtbl = &src_vt; src.text = u; src.len = nu;
    if (FAILED(IDWriteFontFallback_MapCharacters(
                   d->fallback, (IDWriteTextAnalysisSource *)&src, 0, nu,
                   d->sysfonts, pf->family, pf->weight, pf->style,
                   DWRITE_FONT_STRETCH_NORMAL, &mapped, &font, &scale)) ||
        !font)
        return -1;
    if (SUCCEEDED(IDWriteFont_GetFontFamily(font, &fam))) {
        if (SUCCEEDED(IDWriteFontFamily_GetFamilyNames(fam, &names))) {
            if (SUCCEEDED(IDWriteLocalizedStrings_GetString(
                              names, 0, family, LF_FACESIZE)))
                idx = face_of_family_w(d, family, pf->weight, pf->style, font);
            IDWriteLocalizedStrings_Release(names);
        }
        IDWriteFontFamily_Release(fam);
    }
    IDWriteFont_Release(font);
    return idx;
}

/* Which fallback face draws `cp` for the primary font `pf`: the configured
 * list in order, then Windows' fallback; -1 = none (the primary's box). */
static int resolve_face(D2DPainter *d, D2DFont *pf, UINT32 cp)
{
    short fontidx = (short)(pf - d->fonts);
    D2DFbEnt *e = &d->fbmap[(cp * 2654435761u + (unsigned)fontidx * 40503u)
                            % D2D_FB_MAP];
    int i, found = -1, n = winfb_slot_count();

    if (e->valid && e->cp == cp && e->font == fontidx)
        return e->face;
    for (i = 0; i < n && found < 0; i++) {
        int fx = face_of_family(d, winfb_slot_name(i), pf->weight, pf->style);
        if (fx >= 0 && face_has(d->fbfaces[fx].face, cp))
            found = fx;
    }
    if (found < 0 && d->fallback && d->sysfonts)
        found = system_fallback(d, pf, cp);
    e->valid = true; e->cp = cp; e->font = fontidx; e->face = (short)found;
    return found;
}

static void draw_glyphs(D2DPainter *d, IDWriteFontFace *face, float emsize,
                        const UINT16 *gi, const float *adv, int n,
                        D2D1_POINT_2F origin)
{
    DWRITE_GLYPH_RUN run;
    memset(&run, 0, sizeof(run));
    run.fontFace = face;
    run.fontEmSize = emsize;
    run.glyphCount = n;
    run.glyphIndices = gi;
    run.glyphAdvances = adv;
    ID2D1RenderTarget_DrawGlyphRun((ID2D1RenderTarget *)d->dc, origin, &run,
                                   (ID2D1Brush *)d->brush,
                                   DWRITE_MEASURING_MODE_GDI_CLASSIC);
}

/* One run: UTF-16 in, glyphs out, the advances per code POINT (a surrogate
 * pair's two units carry 0 and the width in `dx`, as do variation
 * selectors; summing per code point keeps the cell grid exact). */
static void draw_run(D2DPainter *d, int x, int y, const RECT *clip,
                     bool opaque, const wchar_t *s, int n, const int *dx)
{
    UINT32 cps[512];
    float adv[512];
    UINT16 gi[512];
    int ncp = 0, i;
    D2D1_RECT_F clipf = rectf(clip->left, clip->top, clip->right, clip->bottom);
    D2D1_COLOR_F col;
    D2D1_POINT_2F origin;
    float total = 0;

    if (n <= 0)
        return;
    ID2D1RenderTarget_PushAxisAlignedClip((ID2D1RenderTarget *)d->dc, &clipf,
                                          D2D1_ANTIALIAS_MODE_ALIASED);
    if (opaque || d->opaque)
        fill(d, clip->left, clip->top, clip->right, clip->bottom, d->bg);
    if (!d->font) {
        ID2D1RenderTarget_PopAxisAlignedClip((ID2D1RenderTarget *)d->dc);
        return;
    }
    for (i = 0; i < n && ncp < 512; i++) {
        UINT32 cp = s[i];
        float a = dx ? (float)dx[i] : 0;
        if (IS_HIGH_SURROGATE(s[i]) && i + 1 < n && IS_LOW_SURROGATE(s[i+1])) {
            cp = 0x10000 + ((s[i] - 0xD800) << 10) + (s[i+1] - 0xDC00);
            i++;
            a += dx ? (float)dx[i] : 0;
        }
        cps[ncp] = cp; adv[ncp] = a; ncp++;
    }
    if (FAILED(IDWriteFontFace_GetGlyphIndices(d->font->face, cps, ncp, gi))) {
        ID2D1RenderTarget_PopAxisAlignedClip((ID2D1RenderTarget *)d->dc);
        return;
    }
    if (!dx) {
        /* No advances given: the font's own, as ExtTextOut without lpDx. */
        DWRITE_GLYPH_METRICS gm[512];
        if (SUCCEEDED(IDWriteFontFace_GetDesignGlyphMetrics(
                          d->font->face, gi, ncp, gm, FALSE)))
            for (i = 0; i < ncp; i++)
                adv[i] = gm[i].advanceWidth * d->font->emsize /
                    d->font->units_per_em;
    }
    for (i = 0; i < ncp; i++)
        total += adv[i];

    origin.x = (float)x - (d->centre ? total / 2 : 0);
    origin.y = (float)y + d->font->ascent;
    col = colour_of(d->fg);
    ID2D1SolidColorBrush_SetColor(d->brush, &col);
    {
        /* Glyphs the primary face lacks (index 0) come from a fallback
         * face; consecutive code points sharing a face draw as one run,
         * each on its own cell advances, so the grid stays the terminal's. */
        short fi[512];
        bool any = false;
        for (i = 0; i < ncp; i++) {
            fi[i] = -1;
            if (gi[i] == 0 && cps[i] > 0x20 && cps[i] != 0xFFFD) {
                fi[i] = (short)resolve_face(d, d->font, cps[i]);
                if (fi[i] >= 0)
                    any = true;
            }
        }
        if (!any) {
            draw_glyphs(d, d->font->face, d->font->emsize, gi, adv, ncp, origin);
        } else {
            D2D1_POINT_2F at = origin;
            int s0 = 0;
            while (s0 < ncp) {
                int s1 = s0 + 1;
                while (s1 < ncp && fi[s1] == fi[s0])
                    s1++;
                if (fi[s0] < 0) {
                    draw_glyphs(d, d->font->face, d->font->emsize,
                                gi + s0, adv + s0, s1 - s0, at);
                } else {
                    UINT16 fgi[512];
                    D2DFace *ff = &d->fbfaces[fi[s0]];
                    if (SUCCEEDED(IDWriteFontFace_GetGlyphIndices(
                                      ff->face, cps + s0, s1 - s0, fgi)))
                        draw_glyphs(d, ff->face, d->font->emsize,
                                    fgi, adv + s0, s1 - s0, at);
                }
                for (i = s0; i < s1; i++)
                    at.x += adv[i];
                s0 = s1;
            }
        }
    }
    if (d->font->underline) {
        D2D1_RECT_F ul = rectf(origin.x, (float)y + d->font->ul_top,
                               origin.x + total,
                               (float)y + d->font->ul_top + d->font->ul_height);
        ID2D1RenderTarget_FillRectangle((ID2D1RenderTarget *)d->dc, &ul,
                                        (ID2D1Brush *)d->brush);
    }
    ID2D1RenderTarget_PopAxisAlignedClip((ID2D1RenderTarget *)d->dc);
}

static void d2d_text_w(KittyPainter *p, int x, int y, const RECT *clip,
                       bool opaque, const wchar_t *s, int n, const int *dx)
{
    draw_run((D2DPainter *)p, x, y, clip, opaque, s, n, dx);
}

static void d2d_text_a(KittyPainter *p, int x, int y, const RECT *clip,
                       bool opaque, const char *s, int n, const int *dx)
{
    /* The single-byte path (a font's own charset): widened through the
     * system code page, the nearest thing to what GDI did with it. */
    wchar_t w[512];
    int m;
    if (n <= 0)
        return;
    m = MultiByteToWideChar(CP_ACP, 0, s, n, w, 512);
    if (m == n)
        draw_run((D2DPainter *)p, x, y, clip, opaque, w, m, dx);
}

static void d2d_text_general(KittyPainter *p, int x, int y, const RECT *clip,
                             bool opaque, bool varpitch,
                             const wchar_t *s, int n, const int *dx)
{
    draw_run((D2DPainter *)p, x, y, clip, opaque, s, n, varpitch ? NULL : dx);
}

/* ---- the GDI interop: a DC on the canvas for what has no D2D form ---- */

static HDC interop_dc(D2DPainter *d, ID2D1GdiInteropRenderTarget **out)
{
    HDC hdc = NULL;
    if (FAILED(IUnknown_QueryInterface((IUnknown *)
                   d->dc, &IID_ID2D1GdiInteropRenderTarget, (void **)out)))
        return NULL;
    if (FAILED(ID2D1GdiInteropRenderTarget_GetDC(
                   *out, D2D1_DC_INITIALIZE_MODE_COPY, &hdc))) {
        ID2D1GdiInteropRenderTarget_Release(*out);
        *out = NULL;
        return NULL;
    }
    return hdc;
}

static void interop_done(ID2D1GdiInteropRenderTarget *it, const RECT *r)
{
    ID2D1GdiInteropRenderTarget_ReleaseDC(it, r);
    ID2D1GdiInteropRenderTarget_Release(it);
}

static void d2d_blit_background(KittyPainter *p, const RECT *dst,
                                HDC src, int sx, int sy)
{
    D2DPainter *d = (D2DPainter *)p;
    ID2D1GdiInteropRenderTarget *it;
    HDC hdc = interop_dc(d, &it);
    if (!hdc)
        return;
    BitBlt(hdc, dst->left, dst->top, dst->right - dst->left,
           dst->bottom - dst->top, src, sx, sy, SRCCOPY);
    interop_done(it, dst);
}

static void d2d_icon(KittyPainter *p, int x, int y, HICON ic, int w, int h)
{
    D2DPainter *d = (D2DPainter *)p;
    ID2D1GdiInteropRenderTarget *it;
    RECT r;
    HDC hdc = interop_dc(d, &it);
    if (!hdc)
        return;
    DrawIconEx(hdc, x, y, ic, w, h, 0, NULL, DI_NORMAL);
    r.left = x; r.top = y; r.right = x + w; r.bottom = y + h;
    interop_done(it, &r);
}

/* ---- lines, points, fills ------------------------------------------ */

static void d2d_line(KittyPainter *p, int x0, int y0, int x1, int y1,
                     COLORREF c)
{
    D2DPainter *d = (D2DPainter *)p;
    if (y0 == y1) {
        /* GDI's LineTo leaves out the last pixel. */
        if (x1 >= x0) fill(d, x0, y0, x1, y0 + 1, c);
        else fill(d, x1 + 1, y0, x0 + 1, y0 + 1, c);
    } else if (x0 == x1) {
        if (y1 >= y0) fill(d, x0, y0, x0 + 1, y1, c);
        else fill(d, x0, y1 + 1, x0 + 1, y0 + 1, c);
    } else {
        D2D1_POINT_2F a, b;
        D2D1_COLOR_F col = colour_of(c);
        a.x = x0 + 0.5f; a.y = y0 + 0.5f; b.x = x1 + 0.5f; b.y = y1 + 0.5f;
        ID2D1SolidColorBrush_SetColor(d->brush, &col);
        ID2D1RenderTarget_DrawLine((ID2D1RenderTarget *)d->dc, a, b,
                                   (ID2D1Brush *)d->brush, 1.0f, NULL);
    }
}

static void d2d_rect_outline(KittyPainter *p, const RECT *r, COLORREF c)
{
    /* Polyline's five points: the rectangle's edges, inclusive. */
    D2DPainter *d = (D2DPainter *)p;
    fill(d, r->left, r->top, r->right + 1, r->top + 1, c);
    fill(d, r->left, r->bottom, r->right + 1, r->bottom + 1, c);
    fill(d, r->left, r->top, r->left + 1, r->bottom + 1, c);
    fill(d, r->right, r->top, r->right + 1, r->bottom + 1, c);
}

static void d2d_pixel(KittyPainter *p, int x, int y, COLORREF c)
{
    fill((D2DPainter *)p, x, y, x + 1, y + 1, c);
}

static void d2d_fill_outside(KittyPainter *p, const RECT *paint,
                             const RECT *keep, COLORREF c)
{
    D2DPainter *d = (D2DPainter *)p;
    /* The four strips of `paint` outside `keep`. */
    fill(d, paint->left, paint->top, paint->right, keep->top, c);
    fill(d, paint->left, keep->bottom, paint->right, paint->bottom, c);
    fill(d, paint->left, keep->top, keep->left, keep->bottom, c);
    fill(d, keep->right, keep->top, paint->right, keep->bottom, c);
}

static bool d2d_char_width(KittyPainter *p, HFONT font, unsigned ch,
                           bool wide, int *width)
{
    D2DPainter *d = (D2DPainter *)p;
    D2DFont *f = font_of(d, font);
    UINT32 cp = ch;
    UINT16 gi;
    DWRITE_GLYPH_METRICS gm;
    (void)wide;
    if (!f)
        return false;
    if (FAILED(IDWriteFontFace_GetGlyphIndices(f->face, &cp, 1, &gi)))
        return false;
    if (FAILED(IDWriteFontFace_GetDesignGlyphMetrics(f->face, &gi, 1, &gm, FALSE)))
        return false;
    *width = (int)(gm.advanceWidth * f->emsize / f->units_per_em + 0.5f);
    return true;
}

static HDC d2d_hdc(KittyPainter *p)
{
    (void)p;
    return NULL;
}

static void d2d_destroy(D2DPainter *d);
static void d2d_destroy_p(KittyPainter *p) { d2d_destroy((D2DPainter *)p); }

static const KittyPainterVtable d2d_vt = {
    .begin = d2d_begin,
    .end = d2d_end,
    .resize = d2d_resize,
    .destroy = d2d_destroy_p,
    .style = d2d_style,
    .opaque = d2d_opaque,
    .text_w = d2d_text_w,
    .text_a = d2d_text_a,
    .text_general = d2d_text_general,
    .blit_background = d2d_blit_background,
    .line = d2d_line,
    .rect_outline = d2d_rect_outline,
    .pixel = d2d_pixel,
    .icon = d2d_icon,
    .fill_outside = d2d_fill_outside,
    .char_width = d2d_char_width,
    .hdc = d2d_hdc,
};

/* ---- creation ------------------------------------------------------ */

static void d2d_destroy(D2DPainter *d)
{
    int i;
    RemovePropA(d->hwnd, "KiTTY.renderer");
    for (i = 0; i < d->nfb; i++)
        if (d->fbfaces[i].face) IDWriteFontFace_Release(d->fbfaces[i].face);
    if (d->fallback) IUnknown_Release((IUnknown *)d->fallback);
    if (d->sysfonts) IUnknown_Release((IUnknown *)d->sysfonts);
    for (i = 0; i < d->nfonts; i++)
        if (d->fonts[i].face)
            IDWriteFontFace_Release(d->fonts[i].face);
    if (d->dc) release_targets(d);
    if (d->brush) ID2D1SolidColorBrush_Release(d->brush);
    if (d->gdi) IDWriteGdiInterop_Release(d->gdi);
    if (d->dw) IDWriteFactory_Release(d->dw);
    if (d->dc) IUnknown_Release((IUnknown *)d->dc);
    if (d->device) IUnknown_Release((IUnknown *)d->device);
    if (d->factory) ID2D1Factory1_Release(d->factory);
    if (d->swap) IDXGISwapChain1_Release(d->swap);
    if (d->d3dctx) ID3D11DeviceContext_Release(d->d3dctx);
    if (d->d3d) ID3D11Device_Release(d->d3d);
    if (d->dwritedll) FreeLibrary(d->dwritedll);
    if (d->d2d1dll) FreeLibrary(d->d2d1dll);
    if (d->d3d11dll) FreeLibrary(d->d3d11dll);
    sfree(d);
}

KittyPainter *kitty_painter_d2d_new(HWND hwnd, int font_quality)
{
    D2DPainter *d = snew(D2DPainter);
    D3D11CreateDevice_t pD3D11CreateDevice;
    D2D1CreateFactory_t pD2D1CreateFactory;
    DWriteCreateFactory_t pDWriteCreateFactory;
    IDXGIDevice *dxgidev = NULL;
    IDXGIAdapter *adapter = NULL;
    IDXGIFactory2 *dxgifactory = NULL;
    DXGI_SWAP_CHAIN_DESC1 sd;
    D2D1_COLOR_F black = { 0, 0, 0, 1 };
    RECT client;
    HRESULT hr;
    int failstep = 0;

    memset(d, 0, sizeof(*d));
    d->p.vt = &d2d_vt;
    d->hwnd = hwnd;
    switch (font_quality) {
      case FQ_NONANTIALIASED: d->textaa = D2D1_TEXT_ANTIALIAS_MODE_ALIASED; break;
      case FQ_ANTIALIASED:    d->textaa = D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE; break;
      case FQ_CLEARTYPE:      d->textaa = D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE; break;
      default:                d->textaa = D2D1_TEXT_ANTIALIAS_MODE_DEFAULT; break;
    }

    d->d3d11dll = LoadLibraryA("d3d11.dll");
    d->d2d1dll = LoadLibraryA("d2d1.dll");
    d->dwritedll = LoadLibraryA("dwrite.dll");
    if (!d->d3d11dll || !d->d2d1dll || !d->dwritedll)
        { failstep = 1; goto fail; }
    pD3D11CreateDevice = (D3D11CreateDevice_t)
        GetProcAddress(d->d3d11dll, "D3D11CreateDevice");
    pD2D1CreateFactory = (D2D1CreateFactory_t)
        GetProcAddress(d->d2d1dll, "D2D1CreateFactory");
    pDWriteCreateFactory = (DWriteCreateFactory_t)
        GetProcAddress(d->dwritedll, "DWriteCreateFactory");
    if (!pD3D11CreateDevice || !pD2D1CreateFactory || !pDWriteCreateFactory)
        { failstep = 2; goto fail; }

    hr = pD3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL,
                            D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0,
                            D3D11_SDK_VERSION, &d->d3d, NULL, &d->d3dctx);
    if (FAILED(hr)) {
        d->warp = true;
        hr = pD3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL,
                                D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0,
                                D3D11_SDK_VERSION, &d->d3d, NULL, &d->d3dctx);
    }
    if (FAILED(hr))
        { failstep = 3; goto fail; }

    if (FAILED(pD2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                  &IID_ID2D1Factory1, NULL,
                                  (void **)&d->factory)))
        { failstep = 4; goto fail; }
    if (FAILED(ID3D11Device_QueryInterface(d->d3d, &IID_IDXGIDevice,
                                           (void **)&dxgidev)))
        { failstep = 5; goto fail; }
    if (FAILED(ID2D1Factory1_CreateDevice(d->factory, dxgidev, &d->device)))
        { failstep = 6; goto fail; }
    if (FAILED(ID2D1Device_CreateDeviceContext(
                   d->device, D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d->dc)))
        { failstep = 7; goto fail; }
    ID2D1RenderTarget_SetDpi((ID2D1RenderTarget *)d->dc, 96, 96);

    if (FAILED(IDXGIDevice_GetAdapter(dxgidev, &adapter)))
        { failstep = 8; goto fail; }
    if (FAILED(IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory2,
                                      (void **)&dxgifactory)))
        { failstep = 9; goto fail; }
    GetClientRect(hwnd, &client);
    memset(&sd, 0, sizeof(sd));
    sd.Width = client.right > 0 ? client.right : 1;
    sd.Height = client.bottom > 0 ? client.bottom : 1;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.Scaling = DXGI_SCALING_NONE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    hr = IDXGIFactory2_CreateSwapChainForHwnd(
        dxgifactory, (IUnknown *)d->d3d, hwnd, &sd, NULL, NULL, &d->swap);
    if (FAILED(hr))
        { failstep = 10; goto fail; }

    if (FAILED(ID2D1RenderTarget_CreateSolidColorBrush(
                   (ID2D1RenderTarget *)d->dc, &black, NULL, &d->brush)))
        { failstep = 11; goto fail; }
    if (FAILED(pDWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                    &IID_IDWriteFactory,
                                    (IUnknown **)&d->dw)))
        { failstep = 12; goto fail; }
    if (FAILED(IDWriteFactory_GetGdiInterop(d->dw, &d->gdi)))
        { failstep = 13; goto fail; }
    {
        /* Windows' own font fallback (DirectWrite 2, Windows 8.1); without
         * it only the configured [FontFallback] list serves. Not fatal. */
        IDWriteFactory2 *dw2 = NULL;
        if (SUCCEEDED(IUnknown_QueryInterface((IUnknown *)d->dw,
                                              &IID_IDWriteFactory2,
                                              (void **)&dw2))) {
            IDWriteFactory2_GetSystemFontFallback(dw2, &d->fallback);
            IUnknown_Release((IUnknown *)dw2);
        }
        IDWriteFactory_GetSystemFontCollection(d->dw, &d->sysfonts, FALSE);
    }
    if (!create_targets(d, sd.Width, sd.Height))
        { failstep = 14; goto fail; }

    IDXGIFactory2_Release(dxgifactory);
    IDXGIAdapter_Release(adapter);
    IDXGIDevice_Release(dxgidev);
    /* Which painter the window ended up with, for whoever asks (the QA
     * harness reads it across processes): 1 GDI, 2 Direct2D on the GPU,
     * 3 Direct2D on WARP. */
    SetPropA(hwnd, "KiTTY.renderer", (HANDLE)(ULONG_PTR)(d->warp ? 3 : 2));
    return &d->p;

  fail:
    /* Which step gave up (source order), for the QA harness / support. */
    SetPropA(hwnd, "KiTTY.renderer.fail", (HANDLE)(ULONG_PTR)failstep);
    if (dxgifactory) IDXGIFactory2_Release(dxgifactory);
    if (adapter) IDXGIAdapter_Release(adapter);
    if (dxgidev) IDXGIDevice_Release(dxgidev);
    d2d_destroy(d);
    return NULL;
}
