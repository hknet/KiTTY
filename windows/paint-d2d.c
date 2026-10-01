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
 * put it. The background image is a client-sized copy of the image
 * module's DC kept as a Direct2D bitmap; the trust icon is rendered once
 * into a DIB and kept the same way. No GDI interop on the canvas: the
 * interop DC broke the frame (everything black) when tried.
 *
 * Everything is bound at run time (d3d11.dll, d2d1.dll, dwrite.dll): a build
 * that also runs on old Windows must not import them, and a machine that
 * cannot create the device simply stays on GDI (kitty_painter_d2d_new
 * returns NULL). Windows 8.1 or later is the honest floor: Direct2D 1.1 and
 * the flip-model swap chain, and PrintWindow's PW_RENDERFULLCONTENT for the
 * harnesses that capture the window.
 *
 * Glyphs the primary font lacks come from an [FontFallback] override= range
 * first, then the configured fallback list, then Windows' own DirectWrite
 * fallback, monochrome - the same override ranges and list the GDI
 * painter's module (kitty/winfont_fallback.c) parses and owns. active=no
 * turns off the override range and the list here too, as under GDI, but
 * not Windows' own fallback: it keeps drawing, as GDI's own font linking
 * does with the switch off there.
 *
 * A lost device (a driver update, a GPU reset, a session switch): EndDraw,
 * Present and ResizeBuffers are checked for it. The device and everything
 * made from it (swap chain, device context, canvas, brush, bitmaps) are
 * released at once and re-created at the start of the next frame, and the
 * whole window is invalidated, since the canvas went with the device. The
 * DirectWrite factory, the font faces and the Direct2D factory do not belong
 * to a device and are kept. Three failures in a row - a loss, or a
 * re-creation that fails, each within D2D_LOSS_RUN_MS of the one before -
 * and this window's device is made on WARP, the software rasteriser, for
 * the rest of its life, with one line in the event log. Not GDI: after the
 * first Present1 of a flip-model swap chain, GDI's output never reaches
 * that window again, even once the chain is gone (Microsoft, "DXGI flip
 * model"; seen: GDI drew, the window kept the last Direct2D frame). WARP
 * presents through a swap chain like the hardware device did. Three more
 * failures on WARP and the window stops drawing.
 *
 * Right-to-left text needs no shaping here. The terminal core
 * (term_bidi_line) has already put each line in visual order, mirrored the
 * brackets of right-to-left runs and replaced Arabic letters by their joined
 * presentation forms (unless the session turns those off) before a run
 * reaches text_general. The GDI painter then maps each character to one
 * glyph in its own cell (exact_textout: every character classed neutral, so
 * Windows does not reorder), and draw_run does the same. Running a shaper
 * over these runs would reverse and mirror them a second time.
 *
 * Not yet: a native background image.
 */
#define COBJMACROS
#define INITGUID
#include <stdint.h>
#include "putty.h"
#include <initguid.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dxgi1_3.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <dwrite_2.h>
#include "paint.h"
#include "paint-widthcache.h"
#include "../kitty/kitty_text.h"    /* KiTTY: the renderer badge's word */
#include "../kitty/kitty_renameguard.h"   /* kitty_eventlog_line */
#include "kitty_buildlabel.h"       /* the test build's label, if this is one */

/* The frame pacing's display signal (windows/kitty_pace_frame.c): it must
 * never hold a handle this file has closed. */
void kitty_pace_set_frame_signal(HANDLE h);

/* Device loss: how close together failures must come to count as a run,
 * how many make the window give up on the graphics card (and then on
 * WARP), and how long to wait before trying again after a re-creation
 * failed. */
#define D2D_LOSS_RUN_MS     5000
#define D2D_LOSS_MAX        3
#define D2D_RETRY_MS        2000
#define D2D_RETRY_TIMER_ID  0x44324401     /* next to the badge's timer id */

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
/* An override= range's slot for a code point (ahead of the list and of
 * this file's own system_fallback), and the active= master switch --
 * both owned by the same module, read fresh on every resolve_face call. */
int winfb_override_slot(uint32_t cp);
int GetFontFallbackFlag(void);

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
    HANDLE frame_ready;                /* the swap chain's frame-latency waitable object */
    RECT dirty;                        /* what this frame touched, for Present1 */
    bool dirty_any;
    ID2D1Factory1 *factory;
    ID2D1Device *device;
    ID2D1DeviceContext *dc;
    ID2D1Bitmap1 *target;              /* the swap chain's back buffer */
    ID2D1Bitmap1 *canvas;              /* what the window shows, persistent */
    ID2D1Bitmap1 *scratch;             /* scroll_rows: the band on its way */
    int scratch_w, scratch_h;
    HRESULT mid_hr;                    /* scroll_rows' EndDraw, for d2d_end */
    KittyWidthCache wc;                /* measured widths (paint-widthcache.h) */
    ID2D1SolidColorBrush *brush;
    IDWriteFactory *dw;
    IDWriteGdiInterop *gdi;
    int width, height;
    bool in_frame;
    int pend_w, pend_h;                /* a resize that arrived mid-frame, applied at its end */
    /* The renderer badge: "D2D" in the top-right corner for the first
     * seconds of the window, so whoever opted in can see the opt-in took
     * (a silent fall-back to GDI shows no badge). Drawn onto the back
     * buffer, never into the canvas, so the picture underneath stays. */
    DWORD badge_t0;                    /* GetTickCount at creation */
    IDWriteTextFormat *badge_fmt;
    WCHAR badge_text[16];
    float badge_w, badge_h, badge_margin;
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
    /* the background image, client-sized, and what it was copied from */
    ID2D1Bitmap *bgbmp;
    HDC bgsrc;
    int bgoffx, bgoffy, bggen, bgw, bgh;
    struct { HICON icon; int w, h; ID2D1Bitmap *bmp; } icons[4];
    int nicons;
    /* device loss (see the top of the file) */
    D3D11CreateDevice_t create_d3d;    /* bound once, used again to re-create */
    bool force_warp;                   /* the card failed too often: WARP only */
    bool dead;                         /* WARP failed too: no more frames */
    bool lost;                         /* no device: re-create at the next frame */
    int fail_run;                      /* failures in the current run */
    DWORD fail_tick;                   /* GetTickCount of the last failure */
    DWORD retry_at;                    /* no re-creation before this tick */
    HRESULT last_hr;                   /* the last loss's code, for the event log */
#ifdef KITTY_TEST_BUILD_LABEL
    unsigned long npresent;            /* presents so far, KITTY_D2D_FAIL_PRESENT */
    unsigned long inj_from, inj_count;
#endif
} D2DPainter;
#define D2D_ICON_CACHE 4

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

/* Every drawing call widens the frame's dirty rectangle; Present1 hands it
 * to the compositor so only that much of the window is recomposed. */
static void touch(D2DPainter *d, int l, int t, int r, int b)
{
    if (r <= l || b <= t)
        return;
    if (!d->dirty_any) {
        d->dirty.left = l; d->dirty.top = t; d->dirty.right = r; d->dirty.bottom = b;
        d->dirty_any = true;
    } else {
        if (l < d->dirty.left) d->dirty.left = l;
        if (t < d->dirty.top) d->dirty.top = t;
        if (r > d->dirty.right) d->dirty.right = r;
        if (b > d->dirty.bottom) d->dirty.bottom = b;
    }
}

static void fill(D2DPainter *d, int l, int t, int r, int b, COLORREF c)
{
    touch(d, l, t, r, b);
    D2D1_COLOR_F col = colour_of(c);
    D2D1_RECT_F rc = rectf(l, t, r, b);
    if (r <= l || b <= t)
        return;
    ID2D1SolidColorBrush_SetColor(d->brush, &col);
    ID2D1RenderTarget_FillRectangle((ID2D1RenderTarget *)d->dc, &rc,
                                    (ID2D1Brush *)d->brush);
}

/* ---- test-build trace ---------------------------------------------- */

/* A test build started with KITTY_D2D_TRACE set appends what the device
 * does (which adapter, each loss, each re-creation, the fallback) to
 * d2d_trace.log beside the exe, one "<GetTickCount> <text>" line each. A
 * normal build has none of it. */
#ifdef KITTY_TEST_BUILD_LABEL
static void d2d_trace(const char *fmt, ...)
{
    char path[MAX_PATH], *slash;
    const char *on = getenv("KITTY_D2D_TRACE");
    FILE *f;
    va_list ap;
    if (!on || !*on)
        return;
    if (!GetModuleFileNameA(NULL, path, sizeof(path)))
        return;
    if ((slash = strrchr(path, '\\')) == NULL)
        return;
    strcpy(slash + 1, "d2d_trace.log");
    if ((f = fopen(path, "a")) == NULL)
        return;
    fprintf(f, "%lu ", (unsigned long)GetTickCount());
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}
#define D2D_TRACE(...) d2d_trace(__VA_ARGS__)
#else
#define D2D_TRACE(...) ((void)0)
#endif

/* ---- device loss --------------------------------------------------- */

/* The codes that mean the device is gone, not that one call went wrong.
 * Any other failure is checked against the device's own removed reason. */
static bool hr_is_loss(D2DPainter *d, HRESULT hr)
{
    if (hr == D2DERR_RECREATE_TARGET || hr == DXGI_ERROR_DEVICE_REMOVED ||
        hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG ||
        hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR)
        return true;
    return FAILED(hr) && d->d3d &&
        FAILED(ID3D11Device_GetDeviceRemovedReason(d->d3d));
}

static void device_lost(D2DPainter *d, HRESULT hr, const char *where);

/* ---- the swap chain's two bitmaps ---------------------------------- */

static void release_targets(D2DPainter *d)
{
    ID2D1DeviceContext_SetTarget(d->dc, NULL);
    if (d->target) { IUnknown_Release((IUnknown *)d->target); d->target = NULL; }
    if (d->canvas) { IUnknown_Release((IUnknown *)d->canvas); d->canvas = NULL; }
    if (d->scratch) { IUnknown_Release((IUnknown *)d->scratch); d->scratch = NULL; }
    d->scratch_w = d->scratch_h = 0;
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
    d->dirty.left = 0; d->dirty.top = 0; d->dirty.right = w; d->dirty.bottom = h;
    d->dirty_any = true;

    /* A fresh canvas is black until the terminal repaints into it. */
    ID2D1DeviceContext_SetTarget(d->dc, (struct ID2D1Image *)d->canvas);
    ID2D1RenderTarget_BeginDraw((ID2D1RenderTarget *)d->dc);
    ID2D1RenderTarget_Clear((ID2D1RenderTarget *)d->dc, &black);
    ID2D1RenderTarget_EndDraw((ID2D1RenderTarget *)d->dc, NULL, NULL);
    return true;
}

/*
 * The resize itself. Three things here each showed as "the window went
 * black after a resize":
 *  - the new canvas starts black and only the strip Windows invalidated
 *    got repainted into it: the old canvas is copied into the new one
 *    first, and the whole client area is invalidated so the terminal
 *    repaints everything at the new size;
 *  - ResizeBuffers refuses while anything still references a back buffer
 *    (Direct2D can hold one past the release until the device flushes):
 *    the D3D context is flushed first and the result is checked, and a
 *    failure is recorded on the window (KiTTY.renderer.hr) and retried;
 *  - a resize that arrives mid-frame used to be dropped for good; it is
 *    now kept and applied when the frame ends.
 */
static void do_resize(D2DPainter *d, int w, int h)
{
    ID2D1Bitmap1 *old = d->canvas;
    int oldw = d->width, oldh = d->height;
    UINT flags = d->frame_ready ? DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT : 0;
    HRESULT hr;

    ID2D1DeviceContext_SetTarget(d->dc, NULL);
    if (d->target) { IUnknown_Release((IUnknown *)d->target); d->target = NULL; }
    d->canvas = NULL;                  /* kept in `old` until copied */
    ID3D11DeviceContext_Flush(d->d3dctx);
    hr = IDXGISwapChain1_ResizeBuffers(d->swap, 0, w, h, DXGI_FORMAT_UNKNOWN, flags);
    if (FAILED(hr)) {
        SetPropA(d->hwnd, "KiTTY.renderer.hr", (HANDLE)(ULONG_PTR)(DWORD)hr);
        ID3D11DeviceContext_Flush(d->d3dctx);
        hr = IDXGISwapChain1_ResizeBuffers(d->swap, 0, w, h, DXGI_FORMAT_UNKNOWN, flags);
    }
    if (FAILED(hr) && hr_is_loss(d, hr)) {
        /* the device went: the re-creation takes the new size from the
         * window, and the canvas is repainted whole anyway */
        if (old) IUnknown_Release((IUnknown *)old);
        device_lost(d, hr, "ResizeBuffers");
        return;
    }
    if (SUCCEEDED(hr) && !create_targets(d, w, h)) {
        HRESULT why = ID3D11Device_GetDeviceRemovedReason(d->d3d);
        if (FAILED(why)) {
            if (old) IUnknown_Release((IUnknown *)old);
            device_lost(d, why, "CreateBitmap");
            return;
        }
    } else if (SUCCEEDED(hr) && old) {
        /* what was on screen, at the new size's top-left, until the
         * repaint lands: no black flash during a live resize */
        D2D1_RECT_F src = rectf(0, 0, oldw < w ? oldw : w, oldh < h ? oldh : h);
        ID2D1DeviceContext_SetTarget(d->dc, (struct ID2D1Image *)d->canvas);
        ID2D1RenderTarget_BeginDraw((ID2D1RenderTarget *)d->dc);
        ID2D1RenderTarget_DrawBitmap((ID2D1RenderTarget *)d->dc, (ID2D1Bitmap *)old,
                                     &src, 1.0f,
                                     D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR,
                                     &src);
        ID2D1RenderTarget_EndDraw((ID2D1RenderTarget *)d->dc, NULL, NULL);
        ID2D1DeviceContext_SetTarget(d->dc, NULL);
    }
    if (old) IUnknown_Release((IUnknown *)old);
    InvalidateRect(d->hwnd, NULL, FALSE);
}

static void d2d_resize(KittyPainter *p, int w, int h)
{
    D2DPainter *d = (D2DPainter *)p;
    if (!d->swap)
        return;                        /* also while lost: re-creation reads the size */
    if (w < 1 || h < 1)
        return;
    if (d->in_frame) {
        d->pend_w = w; d->pend_h = h;
        return;
    }
    if (w == d->width && h == d->height)
        return;
    do_resize(d, w, h);
}

/* ---- the renderer badge -------------------------------------------- */

#define BADGE_SHOW_MS   3000           /* visible this long ... */
#define BADGE_FADE_MS   1000           /* ... fading over the last part of it */
#define BADGE_TIMER_ID  0x44324400     /* 'D2D', clear of the window's small ids */

static RECT badge_rect(D2DPainter *d)
{
    RECT r;
    r.right = (LONG)(d->width - d->badge_margin);
    r.left = (LONG)(r.right - d->badge_w);
    r.top = (LONG)d->badge_margin;
    r.bottom = (LONG)(r.top + d->badge_h);
    return r;
}

/* Every timer tick while the badge shows: repaint its corner so the fade
 * progresses even when the terminal itself has nothing to draw. The last
 * tick, after the badge's time, repaints the corner once more without it. */
static void CALLBACK badge_tick(HWND hwnd, UINT msg, UINT_PTR id, DWORD now)
{
    D2DPainter *d = (D2DPainter *)GetPropA(hwnd, "KiTTY.painter.d2d");
    RECT r;
    (void)msg; (void)now;
    if (!d || !d->badge_fmt) { KillTimer(hwnd, id); return; }
    r = badge_rect(d);
    InflateRect(&r, 2, 2);
    if (GetTickCount() - d->badge_t0 > BADGE_SHOW_MS)
        KillTimer(hwnd, id);
    InvalidateRect(hwnd, &r, FALSE);
}

static void badge_init(D2DPainter *d)
{
    HDC hdc = GetDC(d->hwnd);
    float scale = hdc ? GetDeviceCaps(hdc, LOGPIXELSY) / 96.0f : 1.0f;
    IDWriteTextLayout *layout = NULL;
    DWRITE_TEXT_METRICS m;
    int n;
    if (hdc) ReleaseDC(d->hwnd, hdc);
    n = MultiByteToWideChar(CP_ACP, 0, KT_D2D_BADGE, -1, d->badge_text,
                            (int)(sizeof(d->badge_text) / sizeof(WCHAR)));
    if (n <= 0) return;
    if (FAILED(IDWriteFactory_CreateTextFormat(
                   d->dw, L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_BOLD,
                   DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                   13.0f * scale, L"en-us", &d->badge_fmt)))
        return;
    IDWriteTextFormat_SetTextAlignment(d->badge_fmt, DWRITE_TEXT_ALIGNMENT_CENTER);
    IDWriteTextFormat_SetParagraphAlignment(d->badge_fmt, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    if (FAILED(IDWriteFactory_CreateTextLayout(d->dw, d->badge_text, n - 1,
                                               d->badge_fmt, 1000.0f, 100.0f,
                                               &layout)))
        { IDWriteTextFormat_Release(d->badge_fmt); d->badge_fmt = NULL; return; }
    IDWriteTextLayout_GetMetrics(layout, &m);
    IDWriteTextLayout_Release(layout);
    d->badge_w = m.width + 16.0f * scale;
    d->badge_h = m.height + 6.0f * scale;
    d->badge_margin = 10.0f * scale;
    d->badge_t0 = GetTickCount();
    SetPropA(d->hwnd, "KiTTY.painter.d2d", (HANDLE)d);
    /* 16 ms: a fade redrawn ten times a second reads as steps;
     * at display rate it is a fade. The corner is small. */
    SetTimer(d->hwnd, BADGE_TIMER_ID, 16, badge_tick);
}

static void badge_free(D2DPainter *d)
{
    KillTimer(d->hwnd, BADGE_TIMER_ID);
    RemovePropA(d->hwnd, "KiTTY.painter.d2d");
    if (d->badge_fmt) { IDWriteTextFormat_Release(d->badge_fmt); d->badge_fmt = NULL; }
}

/* Onto the back buffer, after the canvas went there. Opaque for two
 * seconds, then fades; nothing at all once its time is up. */
static void badge_draw(D2DPainter *d)
{
    DWORD t = GetTickCount() - d->badge_t0;
    float a;
    RECT r;
    D2D1_ROUNDED_RECT rr;
    D2D1_COLOR_F col;
    if (!d->badge_fmt || t >= BADGE_SHOW_MS)
        return;
    a = t > BADGE_SHOW_MS - BADGE_FADE_MS ? (BADGE_SHOW_MS - t) / (float)BADGE_FADE_MS : 1.0f;
    r = badge_rect(d);
    rr.rect = rectf(r.left, r.top, r.right, r.bottom);
    rr.radiusX = rr.radiusY = d->badge_h / 2.0f;
    ID2D1RenderTarget_SetAntialiasMode((ID2D1RenderTarget *)d->dc,
                                       D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    col.r = 0.10f; col.g = 0.45f; col.b = 0.80f; col.a = 0.85f * a;   /* a blue pill */
    ID2D1SolidColorBrush_SetColor(d->brush, &col);
    ID2D1RenderTarget_FillRoundedRectangle((ID2D1RenderTarget *)d->dc, &rr,
                                           (ID2D1Brush *)d->brush);
    col.r = col.g = col.b = 1.0f; col.a = a;                             /* white word */
    ID2D1SolidColorBrush_SetColor(d->brush, &col);
    ID2D1RenderTarget_DrawText((ID2D1RenderTarget *)d->dc, d->badge_text,
                               (UINT32)wcslen(d->badge_text), d->badge_fmt,
                               &rr.rect, (ID2D1Brush *)d->brush,
                               D2D1_DRAW_TEXT_OPTIONS_NONE,
                               DWRITE_MEASURING_MODE_NATURAL);
    ID2D1RenderTarget_SetAntialiasMode((ID2D1RenderTarget *)d->dc,
                                       D2D1_ANTIALIAS_MODE_ALIASED);
    /* the present's dirty rectangle must cover the corner too */
    if (r.left < d->dirty.left) d->dirty.left = r.left;
    if (r.top < d->dirty.top) d->dirty.top = r.top;
    if (r.right > d->dirty.right) d->dirty.right = r.right;
    if (r.bottom > d->dirty.bottom) d->dirty.bottom = r.bottom;
}

/* ---- frames -------------------------------------------------------- */

static bool device_recreate(D2DPainter *d);

static bool d2d_begin(KittyPainter *p, HDC given)
{
    D2DPainter *d = (D2DPainter *)p;
    /* given: WM_PAINT's DC, not ours to use */
    if (d->dead)
        return false;
    if (d->lost && !d->in_frame) {
        if ((LONG)(GetTickCount() - d->retry_at) < 0)
            return false;              /* the retry timer invalidates the window */
        if (!device_recreate(d))
            return false;
        /* A new canvas is black: the whole window is repainted. The
         * terminal's own update draws only what changed, so it is skipped
         * and WM_PAINT does the lot; WM_PAINT itself goes ahead. */
        InvalidateRect(d->hwnd, NULL, FALSE);
        if (!given)
            return false;
    }
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
    HRESULT hr;
    const char *where;
    if (!d->in_frame)
        return;
    hr = ID2D1RenderTarget_EndDraw((ID2D1RenderTarget *)d->dc, NULL, NULL);
    d->in_frame = false;
    if (SUCCEEDED(hr) && FAILED(d->mid_hr))
        hr = d->mid_hr;                /* scroll_rows ended part of it */
    d->mid_hr = S_OK;
    if (FAILED(hr)) {
        /* A failed frame leaves its code on the window for the harness. */
        SetPropA(d->hwnd, "KiTTY.renderer.hr", (HANDLE)(ULONG_PTR)(DWORD)hr);
        if (hr_is_loss(d, hr)) {
            device_lost(d, hr, "EndDraw");
            return;
        }
    }

    /* Nothing drawn: nothing to present, and the frame slot stays free -
     * but a resize that arrived during the frame is still owed. */
    if (!d->dirty_any) {
        if (d->pend_w) {
            int w = d->pend_w, h = d->pend_h;
            d->pend_w = d->pend_h = 0;
            if (w != d->width || h != d->height)
                do_resize(d, w, h);
        }
        return;
    }

    /* The canvas to the back buffer, and up, telling the compositor which
     * rectangle changed so it recomposes only that. The ready signal was
     * waited on before this frame was started (the pacing), so Present
     * does not block; what little it waits is measured and taken off the
     * paint cost (windows/kitty_pace.c). */
    all = rectf(0, 0, d->width, d->height);
    ID2D1DeviceContext_SetTarget(d->dc, (struct ID2D1Image *)d->target);
    ID2D1RenderTarget_BeginDraw((ID2D1RenderTarget *)d->dc);
    ID2D1RenderTarget_DrawBitmap((ID2D1RenderTarget *)d->dc,
                                 (ID2D1Bitmap *)d->canvas, &all, 1.0f,
                                 D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR,
                                 &all);
    badge_draw(d);
    hr = ID2D1RenderTarget_EndDraw((ID2D1RenderTarget *)d->dc, NULL, NULL);
    if (FAILED(hr) && hr_is_loss(d, hr)) {
        device_lost(d, hr, "EndDraw-backbuffer");
        return;
    }
    {
        extern double kitty_present_wait_ms;
        DXGI_PRESENT_PARAMETERS pp;
        RECT dr = d->dirty;
        LARGE_INTEGER f, a, b;
        if (dr.left < 0) dr.left = 0;
        if (dr.top < 0) dr.top = 0;
        if (dr.right > d->width) dr.right = d->width;
        if (dr.bottom > d->height) dr.bottom = d->height;
        memset(&pp, 0, sizeof(pp));
        pp.DirtyRectsCount = 1;
        pp.pDirtyRects = &dr;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&a);
        /* a Present1 that failed for another reason than a lost device is
         * retried whole */
        where = "Present1";
#ifdef KITTY_TEST_BUILD_LABEL
        /* KITTY_D2D_FAIL_PRESENT=<n>[:<k>]: presents n .. n+k-1 of this
         * painter report a removed device without presenting, so the
         * recovery below runs for real on a healthy device. */
        d->npresent++;
        if (d->inj_from && d->npresent >= d->inj_from &&
            d->npresent - d->inj_from < d->inj_count)
            hr = DXGI_ERROR_DEVICE_REMOVED;
        else
#endif
        {
            hr = IDXGISwapChain1_Present1(d->swap, 0, 0, &pp);
            if (FAILED(hr) && !hr_is_loss(d, hr)) {
                where = "Present";
                hr = IDXGISwapChain1_Present(d->swap, 0, 0);
            }
        }
        QueryPerformanceCounter(&b);
        kitty_present_wait_ms += (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart;
    }
    if (FAILED(hr) && hr_is_loss(d, hr)) {
        device_lost(d, hr, where);
        return;
    }
    d->dirty_any = false;
    ID2D1DeviceContext_SetTarget(d->dc, NULL);
    if (d->pend_w) {
        int w = d->pend_w, h = d->pend_h;
        d->pend_w = d->pend_h = 0;
        if (w != d->width || h != d->height)
            do_resize(d, w, h);
    }
}

/* ---- fonts --------------------------------------------------------- */

/* Forget every cached font. The window made new HFONTs (d2d_fonts_changed)
 * and handle values are recycled, so a kept entry could describe an old font
 * - its size above all - and without this the table fills after a few font
 * or DPI changes and text stops being drawn. The per-character fallback map
 * stores indices into this table, so it goes too; the fallback faces are
 * kept by family name and stay valid. */
static void fonts_forget(D2DPainter *d)
{
    int i;
    for (i = 0; i < d->nfonts; i++)
        if (d->fonts[i].face)
            IDWriteFontFace_Release(d->fonts[i].face);
    d->nfonts = 0;
    d->font = NULL;
    memset(d->fbmap, 0, sizeof(d->fbmap));
}

static void d2d_fonts_changed(KittyPainter *p)
{
    fonts_forget((D2DPainter *)p);
    kitty_wc_clear(&((D2DPainter *)p)->wc);   /* widths of recycled handles */
}

/* The band's pixels moved by dy within the persistent canvas, mid-frame:
 * the frame so far is ended (a bitmap copy is not made on a bitmap that is
 * being drawn into), the band goes canvas -> scratch -> canvas at its new
 * place, and the frame is begun again as d2d_begin began it. The band is
 * dirty for the present. Any failure: false, nothing is assumed moved. */
static bool d2d_scroll_rows(KittyPainter *p, const RECT *band, int dy)
{
    D2DPainter *d = (D2DPainter *)p;
    D2D1_RECT_U src;
    D2D1_POINT_2U at0 = { 0, 0 }, dst;
    D2D1_RECT_U back;
    HRESULT hr;
    int h;
    if (!d->in_frame || !d->canvas || d->lost || dy == 0 ||
        band->left < 0 || band->top < 0 ||
        band->right > d->width || band->bottom > d->height)
        return false;
    h = band->bottom - band->top - (dy < 0 ? -dy : dy);
    if (h <= 0 || band->right <= band->left)
        return false;
    if (!d->scratch || d->scratch_w != d->width || d->scratch_h != d->height) {
        D2D1_BITMAP_PROPERTIES1 props;
        D2D1_SIZE_U size;
        if (d->scratch) { IUnknown_Release((IUnknown *)d->scratch); d->scratch = NULL; }
        memset(&props, 0, sizeof(props));
        props.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
        props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_IGNORE;
        props.dpiX = 96; props.dpiY = 96;
        props.bitmapOptions = D2D1_BITMAP_OPTIONS_NONE;
        size.width = d->width; size.height = d->height;
        if (FAILED(d->dc->lpVtbl->CreateBitmap(d->dc, size, NULL, 0, &props,
                                               &d->scratch)))
            return false;
        d->scratch_w = d->width; d->scratch_h = d->height;
    }
    hr = ID2D1RenderTarget_EndDraw((ID2D1RenderTarget *)d->dc, NULL, NULL);
    d->in_frame = false;
    if (FAILED(hr))
        d->mid_hr = hr;                /* d2d_end handles it, device lost included */
    else {
        src.left = band->left; src.right = band->right;
        src.top = band->top + (dy < 0 ? -dy : 0);
        src.bottom = src.top + h;
        dst.x = band->left;
        dst.y = band->top + (dy > 0 ? dy : 0);
        back.left = 0; back.top = 0; back.right = band->right - band->left; back.bottom = h;
        hr = ((ID2D1Bitmap *)d->scratch)->lpVtbl->CopyFromBitmap(
            (ID2D1Bitmap *)d->scratch, &at0, (ID2D1Bitmap *)d->canvas, &src);
        if (SUCCEEDED(hr))
            hr = ((ID2D1Bitmap *)d->canvas)->lpVtbl->CopyFromBitmap(
                (ID2D1Bitmap *)d->canvas, &dst, (ID2D1Bitmap *)d->scratch, &back);
    }
    /* the frame goes on either way, as d2d_begin started it */
    ID2D1DeviceContext_SetTarget(d->dc, (struct ID2D1Image *)d->canvas);
    ID2D1RenderTarget_BeginDraw((ID2D1RenderTarget *)d->dc);
    ID2D1RenderTarget_SetAntialiasMode((ID2D1RenderTarget *)d->dc,
                                       D2D1_ANTIALIAS_MODE_ALIASED);
    ID2D1RenderTarget_SetTextAntialiasMode((ID2D1RenderTarget *)d->dc,
                                           d->textaa);
    d->in_frame = true;
    if (FAILED(hr))
        return false;
    touch(d, band->left, band->top, band->right, band->bottom);
    return true;
}

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
    if (!hfont)
        return NULL;
    if (d->nfonts >= D2D_FONT_CACHE)
        fonts_forget(d);               /* never full: start over */
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

/* Which fallback face draws `cp` for the primary font `pf`: an override=
 * range first, then the configured list in order, then Windows' fallback;
 * -1 = none (the primary's box). [FontFallback] active=no turns the first
 * two off and leaves Windows' fallback, matching GDI, where Windows' font
 * linking keeps drawing: GetFontFallbackFlag() is read fresh at the top of
 * every call, ahead of the cache, so a switch flip takes effect on the
 * next glyph drawn (GDI instead picks up active=no at the next font
 * setup/reinit, when winfb_reinit_from_config runs winfb_cleanup(); this
 * file has no equivalent reinit hook from the config box, so it checks the
 * flag per lookup rather than per font setup). While off the cache is
 * neither read nor written, so a flip is never served a stale cached face;
 * cached entries from while the switch was on stay valid once it is on
 * again, since neither the flag nor an override range changes what a
 * cached (cp, font) pair resolves to. */
static int resolve_face(D2DPainter *d, D2DFont *pf, UINT32 cp)
{
    /* [FontFallback] active=no turns off KiTTY's own list and override=,
     * as under GDI; Windows' own fallback still draws, as GDI's font
     * linking does there. Not cached: the switch can change while the
     * window is open, and the system's answer is cheap next to a redraw. */
    if (!GetFontFallbackFlag())
        return (d->fallback && d->sysfonts) ? system_fallback(d, pf, cp) : -1;

    short fontidx = (short)(pf - d->fonts);
    D2DFbEnt *e = &d->fbmap[(cp * 2654435761u + (unsigned)fontidx * 40503u)
                            % D2D_FB_MAP];
    int i, found = -1, n = winfb_slot_count();

    if (e->valid && e->cp == cp && e->font == fontidx)
        return e->face;

    /* override= (highest priority, same ranges as GDI's winfb_lookup_slot):
     * an unconditional pin, not a has-the-glyph check, exactly as GDI
     * trusts it; falls through to the list/system fallback only if the
     * pinned font is not in Direct2D's own font enumeration. */
    int ovr_slot = winfb_override_slot(cp);
    if (ovr_slot >= 0) {
        int fx = face_of_family(d, winfb_slot_name(ovr_slot), pf->weight, pf->style);
        if (fx >= 0) found = fx;
    }

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
    touch(d, clip->left, clip->top, clip->right, clip->bottom);
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
         * face, and so does any code point in an override= range, even one
         * the primary face has - the pin comes first, as in GDI's
         * winfb_lookup_slot. Consecutive code points sharing a face draw as
         * one run, each on its own cell advances, so the grid stays the
         * terminal's. */
        short fi[512];
        bool any = false;
        bool fb_on = GetFontFallbackFlag() != 0;
        for (i = 0; i < ncp; i++) {
            fi[i] = -1;
            if (cps[i] > 0x20 && cps[i] != 0xFFFD &&
                (gi[i] == 0 ||
                 (fb_on && winfb_override_slot(cps[i]) >= 0))) {
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

/* The run is in visual order, mirrored and shaped already (see the top of
 * the file): one glyph per character in its cell, as GDI's exact_textout. */
static void d2d_text_general(KittyPainter *p, int x, int y, const RECT *clip,
                             bool opaque, bool varpitch,
                             const wchar_t *s, int n, const int *dx)
{
    draw_run((D2DPainter *)p, x, y, clip, opaque, s, n, varpitch ? NULL : dx);
}

/* ---- bitmaps from GDI: the background image and the trust icon ------ */

/* A 32bpp top-down DIB to draw into with GDI, then upload. */
static bool dib_begin(int w, int h, HDC *mem, HBITMAP *dib, HGDIOBJ *old,
                      void **bits)
{
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    *mem = CreateCompatibleDC(NULL);
    if (!*mem)
        return false;
    *dib = CreateDIBSection(*mem, &bi, DIB_RGB_COLORS, bits, NULL, 0);
    if (!*dib) {
        DeleteDC(*mem);
        return false;
    }
    *old = SelectObject(*mem, *dib);
    memset(*bits, 0, (size_t)w * h * 4);
    return true;
}

static ID2D1Bitmap *dib_upload(D2DPainter *d, const void *bits, int w, int h,
                               bool alpha)
{
    D2D1_BITMAP_PROPERTIES props;
    D2D1_SIZE_U size;
    ID2D1Bitmap *bmp = NULL;
    memset(&props, 0, sizeof(props));
    props.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    props.pixelFormat.alphaMode = alpha ? D2D1_ALPHA_MODE_PREMULTIPLIED
                                        : D2D1_ALPHA_MODE_IGNORE;
    props.dpiX = 96; props.dpiY = 96;
    size.width = w; size.height = h;
    if (FAILED(ID2D1RenderTarget_CreateBitmap((ID2D1RenderTarget *)d->dc, size,
                                              bits, (UINT32)w * 4, &props,
                                              &bmp)))
        return NULL;
    return bmp;
}

static void dib_end(HDC mem, HBITMAP dib, HGDIOBJ old)
{
    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
}

/* The image module's DC holds the image in SCREEN coordinates and is
 * rebuilt when the image or the window changes (kitty_bg_generation).
 * One client-sized copy of it lives on the GPU; a run draws its cells
 * from that copy. */
extern int kitty_bg_generation;
static void d2d_blit_background(KittyPainter *p, const RECT *dst,
                                HDC src, int sx, int sy)
{
    D2DPainter *d = (D2DPainter *)p;
    int offx = sx - dst->left, offy = sy - dst->top;
    D2D1_RECT_F r = rectf(dst->left, dst->top, dst->right, dst->bottom);

    if (d->bgbmp && (d->bgsrc != src || d->bgoffx != offx || d->bgoffy != offy
                     || d->bggen != kitty_bg_generation
                     || d->bgw != d->width || d->bgh != d->height)) {
        ID2D1Bitmap_Release(d->bgbmp);
        d->bgbmp = NULL;
    }
    if (!d->bgbmp) {
        HDC mem; HBITMAP dib; HGDIOBJ old; void *bits;
        if (dib_begin(d->width, d->height, &mem, &dib, &old, &bits)) {
            BitBlt(mem, 0, 0, d->width, d->height, src, offx, offy, SRCCOPY);
            d->bgbmp = dib_upload(d, bits, d->width, d->height, false);
            dib_end(mem, dib, old);
        }
        d->bgsrc = src; d->bgoffx = offx; d->bgoffy = offy;
        d->bggen = kitty_bg_generation; d->bgw = d->width; d->bgh = d->height;
    }
    if (!d->bgbmp) {
        fill(d, dst->left, dst->top, dst->right, dst->bottom, d->bg);
        return;
    }
    touch(d, dst->left, dst->top, dst->right, dst->bottom);
    ID2D1RenderTarget_DrawBitmap((ID2D1RenderTarget *)d->dc, d->bgbmp, &r, 1.0f,
                                 D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR,
                                 &r);
}

/* The trust sigil: DrawIconEx into a 32bpp DIB (alpha kept), premultiplied
 * for Direct2D, cached per icon and size. */
static void d2d_icon(KittyPainter *p, int x, int y, HICON ic, int w, int h)
{
    D2DPainter *d = (D2DPainter *)p;
    int i;
    ID2D1Bitmap *bmp = NULL;
    D2D1_RECT_F r;
    for (i = 0; i < d->nicons; i++)
        if (d->icons[i].icon == ic && d->icons[i].w == w && d->icons[i].h == h)
            bmp = d->icons[i].bmp;
    if (!bmp && d->nicons < D2D_ICON_CACHE && w > 0 && h > 0) {
        HDC mem; HBITMAP dib; HGDIOBJ old; void *bits;
        if (dib_begin(w, h, &mem, &dib, &old, &bits)) {
            unsigned char *px = bits;
            int n;
            DrawIconEx(mem, 0, 0, ic, w, h, 0, NULL, DI_NORMAL);
            GdiFlush();
            for (n = 0; n < w * h; n++, px += 4) {
                unsigned a = px[3];
                px[0] = (unsigned char)(px[0] * a / 255);
                px[1] = (unsigned char)(px[1] * a / 255);
                px[2] = (unsigned char)(px[2] * a / 255);
            }
            bmp = dib_upload(d, bits, w, h, true);
            dib_end(mem, dib, old);
        }
        d->icons[d->nicons].icon = ic; d->icons[d->nicons].w = w;
        d->icons[d->nicons].h = h; d->icons[d->nicons].bmp = bmp;
        d->nicons++;
    }
    if (!bmp)
        return;
    touch(d, x, y, x + w, y + h);
    r = rectf(x, y, x + w, y + h);
    ID2D1RenderTarget_DrawBitmap((ID2D1RenderTarget *)d->dc, bmp, &r, 1.0f,
                                 D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR,
                                 NULL);
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
        touch(d, x0 < x1 ? x0 : x1, y0 < y1 ? y0 : y1,
              (x0 > x1 ? x0 : x1) + 1, (y0 > y1 ? y0 : y1) + 1);
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
    D2DFont *f;
    UINT32 cp = ch;
    UINT16 gi;
    DWRITE_GLYPH_METRICS gm;
    if (kitty_wc_get(&d->wc, font, ch, wide, width))
        return true;                   /* measured before */
    f = font_of(d, font);
    if (!f)
        return false;
    if (FAILED(IDWriteFontFace_GetGlyphIndices(f->face, &cp, 1, &gi)))
        return false;
    if (FAILED(IDWriteFontFace_GetDesignGlyphMetrics(f->face, &gi, 1, &gm, FALSE)))
        return false;
    *width = (int)(gm.advanceWidth * f->emsize / f->units_per_em + 0.5f);
    kitty_wc_put(&d->wc, font, ch, wide, *width);
    return true;
}

static HDC d2d_hdc(KittyPainter *p)
{
    (void)p;
    return NULL;
}

static void d2d_destroy(D2DPainter *d);
static void d2d_destroy_p(KittyPainter *p)
{
    kitty_wc_free(&((D2DPainter *)p)->wc);
    d2d_destroy((D2DPainter *)p);
}

static HANDLE d2d_frame_signal(KittyPainter *p)
{
    return ((D2DPainter *)p)->frame_ready;
}

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
    .frame_signal = d2d_frame_signal,
    .fonts_changed = d2d_fonts_changed,
    .scroll_rows = d2d_scroll_rows,
};

/* ---- the device ---------------------------------------------------- */

/* Everything made from the Direct3D device, released - after a loss, or
 * for good. The Direct2D factory and the DirectWrite side (factory, font
 * faces, fallback) belong to no device and are kept. */
static void release_device(D2DPainter *d)
{
    int i;
    if (d->dc) release_targets(d);
    for (i = 0; i < d->nicons; i++)
        if (d->icons[i].bmp) ID2D1Bitmap_Release(d->icons[i].bmp);
    d->nicons = 0;
    if (d->bgbmp) { ID2D1Bitmap_Release(d->bgbmp); d->bgbmp = NULL; }
    if (d->brush) { ID2D1SolidColorBrush_Release(d->brush); d->brush = NULL; }
    if (d->dc) { IUnknown_Release((IUnknown *)d->dc); d->dc = NULL; }
    if (d->device) { IUnknown_Release((IUnknown *)d->device); d->device = NULL; }
    if (d->swap) { IDXGISwapChain1_Release(d->swap); d->swap = NULL; }
    if (d->frame_ready) {
        kitty_pace_set_frame_signal(NULL);
        CloseHandle(d->frame_ready);
        d->frame_ready = NULL;
    }
    if (d->d3dctx) {
        /* Direct3D destroys objects late; this lets the swap chain go
         * now, so a new one can be made on the same window. */
        ID3D11DeviceContext_ClearState(d->d3dctx);
        ID3D11DeviceContext_Flush(d->d3dctx);
        ID3D11DeviceContext_Release(d->d3dctx);
        d->d3dctx = NULL;
    }
    if (d->d3d) { ID3D11Device_Release(d->d3d); d->d3d = NULL; }
    d->in_frame = false;
    d->dirty_any = false;
    d->pend_w = d->pend_h = 0;
    d->width = d->height = 0;
}

#ifdef KITTY_TEST_BUILD_LABEL
/* Which adapter the device got. kind: "warp" when the hardware device
 * could not be made and WARP was asked for; "basic" when a hardware
 * device landed on the Microsoft Basic Render Driver (vendor 0x1414,
 * device 0x8c: no GPU driver, rasterised by WARP all the same); "software"
 * for any other adapter DXGI flags as software; else "hardware". */
static void trace_adapter(D2DPainter *d, IDXGIAdapter *adapter)
{
    DXGI_ADAPTER_DESC desc;
    IDXGIAdapter1 *a1 = NULL;
    UINT flags = 0;
    char name[256];
    const char *kind;
    if (FAILED(IDXGIAdapter_GetDesc(adapter, &desc)))
        return;
    if (SUCCEEDED(IDXGIAdapter_QueryInterface(adapter, &IID_IDXGIAdapter1,
                                              (void **)&a1))) {
        DXGI_ADAPTER_DESC1 d1;
        if (SUCCEEDED(IDXGIAdapter1_GetDesc1(a1, &d1)))
            flags = d1.Flags;
        IDXGIAdapter1_Release(a1);
    }
    if (!WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name,
                             sizeof(name), NULL, NULL))
        strcpy(name, "?");
    if (d->warp)
        kind = "warp";
    else if (desc.VendorId == 0x1414 && desc.DeviceId == 0x8c)
        kind = "basic";
    else if (flags & DXGI_ADAPTER_FLAG_SOFTWARE)
        kind = "software";
    else
        kind = "hardware";
    D2D_TRACE("adapter: %s vendor=0x%04x device=0x%04x kind=%s", name,
              (unsigned)desc.VendorId, (unsigned)desc.DeviceId, kind);
}
#endif

/* The device and everything made from it, at the window's current size:
 * the hardware device, or WARP when there is none or the card has failed
 * too often (force_warp). 0, or the step that
 * failed (the numbers are kitty_painter_d2d_new's, for KiTTY.renderer.fail),
 * with nothing left half made. */
static int create_device(D2DPainter *d)
{
    IDXGIDevice *dxgidev = NULL;
    IDXGIAdapter *adapter = NULL;
    IDXGIFactory2 *dxgifactory = NULL;
    DXGI_SWAP_CHAIN_DESC1 sd;
    D2D1_COLOR_F black = { 0, 0, 0, 1 };
    RECT client;
    HRESULT hr;
    int failstep = 0;

    d->warp = false;
    hr = E_FAIL;
    if (!d->force_warp)
        hr = d->create_d3d(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL,
                           D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0,
                           D3D11_SDK_VERSION, &d->d3d, NULL, &d->d3dctx);
    if (FAILED(hr)) {
        d->warp = true;
        hr = d->create_d3d(NULL, D3D_DRIVER_TYPE_WARP, NULL,
                           D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0,
                           D3D11_SDK_VERSION, &d->d3d, NULL, &d->d3dctx);
    }
    if (FAILED(hr))
        { failstep = 3; goto done; }

    if (FAILED(ID3D11Device_QueryInterface(d->d3d, &IID_IDXGIDevice,
                                           (void **)&dxgidev)))
        { failstep = 5; goto done; }
    if (FAILED(ID2D1Factory1_CreateDevice(d->factory, dxgidev, &d->device)))
        { failstep = 6; goto done; }
    if (FAILED(ID2D1Device_CreateDeviceContext(
                   d->device, D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d->dc)))
        { failstep = 7; goto done; }
    ID2D1RenderTarget_SetDpi((ID2D1RenderTarget *)d->dc, 96, 96);

    if (FAILED(IDXGIDevice_GetAdapter(dxgidev, &adapter)))
        { failstep = 8; goto done; }
#ifdef KITTY_TEST_BUILD_LABEL
    trace_adapter(d, adapter);
#endif
    if (FAILED(IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory2,
                                      (void **)&dxgifactory)))
        { failstep = 9; goto done; }
    GetClientRect(d->hwnd, &client);
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
    /* The frame-latency waitable object: the compositor signals it when
     * it is ready for the next frame - the display's own pace, which the
     * frame pacing waits on instead of guessing at vblanks. Windows 8.1. */
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    /* Window transparency needs nothing here: a window that may be dimmed
     * is layered from its creation (window.c), and the layered alpha then
     * applies to this chain's frames as to GDI's output, title bar
     * included (measured on the composed screen). What breaks is
     * changing an open window's presentation - layered on or off, another
     * chain model - so neither is ever done to a window that has one. */
    hr = IDXGIFactory2_CreateSwapChainForHwnd(
        dxgifactory, (IUnknown *)d->d3d, d->hwnd, &sd, NULL, NULL, &d->swap);
    if (FAILED(hr) && sd.Flags) {
        sd.Flags = 0;                 /* a runtime without it: no signal */
        hr = IDXGIFactory2_CreateSwapChainForHwnd(
            dxgifactory, (IUnknown *)d->d3d, d->hwnd, &sd, NULL, NULL, &d->swap);
    }
    if (FAILED(hr))
        { failstep = 10; goto done; }
    if (sd.Flags) {
        IDXGISwapChain2 *sc2 = NULL;
        if (SUCCEEDED(IUnknown_QueryInterface((IUnknown *)d->swap,
                                              &IID_IDXGISwapChain2,
                                              (void **)&sc2))) {
            IDXGISwapChain2_SetMaximumFrameLatency(sc2, 1);
            d->frame_ready = IDXGISwapChain2_GetFrameLatencyWaitableObject(sc2);
            IUnknown_Release((IUnknown *)sc2);
        }
    }

    if (FAILED(ID2D1RenderTarget_CreateSolidColorBrush(
                   (ID2D1RenderTarget *)d->dc, &black, NULL, &d->brush)))
        { failstep = 11; goto done; }
    if (!create_targets(d, sd.Width, sd.Height))
        { failstep = 14; goto done; }

    /* Which painter the window ended up with, for whoever asks (the QA
     * harness reads it across processes): 1 GDI, 2 Direct2D on the GPU,
     * 3 Direct2D on WARP. */
    SetPropA(d->hwnd, "KiTTY.renderer", (HANDLE)(ULONG_PTR)(d->warp ? 3 : 2));

  done:
    if (dxgifactory) IDXGIFactory2_Release(dxgifactory);
    if (adapter) IDXGIAdapter_Release(adapter);
    if (dxgidev) IDXGIDevice_Release(dxgidev);
    if (failstep)
        release_device(d);
    return failstep;
}

/* The retry after a failed re-creation: a frame to try in. */
static void CALLBACK retry_tick(HWND hwnd, UINT msg, UINT_PTR id, DWORD now)
{
    (void)msg; (void)now;
    KillTimer(hwnd, id);
    InvalidateRect(hwnd, NULL, FALSE);
}

/* D2D_LOSS_MAX failures in a row on the graphics card: WARP for the rest
 * of the window's life, and one line in the event log for whoever wonders
 * why. The device is already released (device_lost, or the re-creation
 * that failed); the next frame makes it again, on WARP, with its own swap
 * chain on the same window - the flip model stays, so does the pacing. */
static void fall_back_to_warp(D2DPainter *d)
{
    char *msg;
    d->force_warp = true;
    d->fail_run = 0;
    d->retry_at = GetTickCount();
    D2D_TRACE("fallback to WARP after %d failures", D2D_LOSS_MAX);
    msg = dupprintf(KT_D2D_FALLBACK_EVENT, D2D_LOSS_MAX,
                    (unsigned long)(DWORD)d->last_hr);
    kitty_eventlog_line(EVENTLOG_WARNING_TYPE, msg);
    sfree(msg);
}

/* One more failure: a loss, or a re-creation that failed. A failure
 * within D2D_LOSS_RUN_MS of the one before continues the run, a later one
 * starts a new run. A full run on the card switches to WARP; a full run on
 * WARP as well and the window stops drawing: true, the caller stops. */
static bool count_failure(D2DPainter *d)
{
    DWORD now = GetTickCount();
    if (d->fail_run > 0 && now - d->fail_tick <= D2D_LOSS_RUN_MS)
        d->fail_run++;
    else
        d->fail_run = 1;
    d->fail_tick = now;
    if (d->fail_run < D2D_LOSS_MAX)
        return false;
    if (!d->force_warp) {
        fall_back_to_warp(d);
        return false;
    }
    D2D_TRACE("WARP failed %d times too: this window draws no more",
              d->fail_run);
    d->dead = true;
    d->lost = false;
    KillTimer(d->hwnd, D2D_RETRY_TIMER_ID);
    return true;
}

/* The device is gone (hr, reported by `where`): let everything made from
 * it go, and have the next frame make it again. Outside a frame always -
 * EndDraw has been called, or no frame was open. */
static void device_lost(D2DPainter *d, HRESULT hr, const char *where)
{
    D2D_TRACE("device lost hr=0x%08lx at %s", (unsigned long)(DWORD)hr, where);
    SetPropA(d->hwnd, "KiTTY.renderer.hr", (HANDLE)(ULONG_PTR)(DWORD)hr);
    d->last_hr = hr;
    release_device(d);
    d->lost = true;
    d->retry_at = GetTickCount();      /* the first attempt at once */
    if (count_failure(d))
        return;
    /* the canvas went with the device: the whole window is drawn again */
    InvalidateRect(d->hwnd, NULL, FALSE);
}

/* The next frame after a loss. False: no device yet (a retry is armed),
 * or WARP has failed too and the window draws no more (d->dead). */
static bool device_recreate(D2DPainter *d)
{
    int step = create_device(d);
    if (step) {
        D2D_TRACE("device recreation failed at step %d", step);
        if (count_failure(d))
            return false;
        d->retry_at = GetTickCount() + D2D_RETRY_MS;
        SetTimer(d->hwnd, D2D_RETRY_TIMER_ID, D2D_RETRY_MS, retry_tick);
        return false;
    }
    d->lost = false;
    D2D_TRACE("device recreated");
    kitty_pace_set_frame_signal(d->frame_ready);
    return true;
}

/* ---- creation ------------------------------------------------------ */

static void d2d_destroy(D2DPainter *d)
{
    int i;
    RemovePropA(d->hwnd, "KiTTY.renderer");
    KillTimer(d->hwnd, D2D_RETRY_TIMER_ID);
    badge_free(d);
    release_device(d);
    for (i = 0; i < d->nfb; i++)
        if (d->fbfaces[i].face) IDWriteFontFace_Release(d->fbfaces[i].face);
    if (d->fallback) IUnknown_Release((IUnknown *)d->fallback);
    if (d->sysfonts) IUnknown_Release((IUnknown *)d->sysfonts);
    for (i = 0; i < d->nfonts; i++)
        if (d->fonts[i].face)
            IDWriteFontFace_Release(d->fonts[i].face);
    if (d->gdi) IDWriteGdiInterop_Release(d->gdi);
    if (d->dw) IDWriteFactory_Release(d->dw);
    if (d->factory) ID2D1Factory1_Release(d->factory);
    if (d->dwritedll) FreeLibrary(d->dwritedll);
    if (d->d2d1dll) FreeLibrary(d->d2d1dll);
    if (d->d3d11dll) FreeLibrary(d->d3d11dll);
    sfree(d);
}

KittyPainter *kitty_painter_d2d_new(HWND hwnd, int font_quality)
{
    D2DPainter *d = snew(D2DPainter);
    D2D1CreateFactory_t pD2D1CreateFactory;
    DWriteCreateFactory_t pDWriteCreateFactory;
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
    d->create_d3d = (D3D11CreateDevice_t)
        GetProcAddress(d->d3d11dll, "D3D11CreateDevice");
    pD2D1CreateFactory = (D2D1CreateFactory_t)
        GetProcAddress(d->d2d1dll, "D2D1CreateFactory");
    pDWriteCreateFactory = (DWriteCreateFactory_t)
        GetProcAddress(d->dwritedll, "DWriteCreateFactory");
    if (!d->create_d3d || !pD2D1CreateFactory || !pDWriteCreateFactory)
        { failstep = 2; goto fail; }

    /* Device-independent: made once, kept across a lost device. */
    if (FAILED(pD2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                  &IID_ID2D1Factory1, NULL,
                                  (void **)&d->factory)))
        { failstep = 4; goto fail; }
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

    /* The device and all made from it (steps 3, 5-11, 14). */
    if ((failstep = create_device(d)) != 0)
        goto fail;

#ifdef KITTY_TEST_BUILD_LABEL
    {
        /* KITTY_D2D_FAIL_PRESENT=<n> or <n>:<k> (see d2d_end) */
        const char *e = getenv("KITTY_D2D_FAIL_PRESENT");
        if (e && *e) {
            char *end;
            unsigned long n = strtoul(e, &end, 10), k = 1;
            if (*end == ':')
                k = strtoul(end + 1, NULL, 10);
            if (n >= 1 && k >= 1) {
                d->inj_from = n;
                d->inj_count = k;
            }
        }
    }
#endif
    badge_init(d);
    return &d->p;

  fail:
    /* Which step gave up, for the QA harness / support: 1-2 the DLLs,
     * 4 the Direct2D factory, 12-13 DirectWrite, 3, 5-11 and 14 the
     * device (create_device). */
    SetPropA(hwnd, "KiTTY.renderer.fail", (HANDLE)(ULONG_PTR)failstep);
    d2d_destroy(d);
    return NULL;
}
