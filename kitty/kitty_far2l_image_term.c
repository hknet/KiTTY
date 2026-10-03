/*
 * kitty_far2l_image_term.c - far2l images (request 'i'): the terminal's side.
 *
 * The request is parsed and the store kept by kitty_far2l_image.c; this file
 * gives it what only the terminal and the window know - whether images are
 * on (the session's "Show far2l images" and an active far2l), the cell size,
 * the cursor, a PNG/JPEG decoder - and turns every change of an image into a
 * repaint of the cells it covered and covers now. The painters draw the
 * images over the text at the end of each frame (windows/paint.h, overlay),
 * so the cells under an image must be drawn again whenever it changes: that
 * repaint is what makes both painters show the change.
 *
 * PNG and JPEG are decoded with the Windows Imaging Component, created at
 * run time through COM (CoCreateInstance): a Windows without WIC (XP as
 * shipped) simply reports those formats unsupported, and nothing here is
 * imported that XP lacks. COM is already initialised on the window's thread.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "putty.h"
#include "terminal.h"
#include "kitty_far2l_image.h"
#include "kitty_far2l_image_term.h"
#include "kitty_buildlabel.h"          /* the test build's trace hook */

#ifdef _WIN32
#define COBJMACROS
#include <windows.h>
#include <wincodec.h>
#endif

bool (*kitty_far2l_cell_hook)(TermWin *win, int *cell_w, int *cell_h,
                              int *off_x, int *off_y);

/* ---- PNG / JPEG through WIC -------------------------------------------- */

#ifdef _WIN32
/* Own copies of the GUIDs, so nothing needs uuid.lib. */
static const GUID f2l_CLSID_WICImagingFactory =
    { 0xcacaf262, 0x9370, 0x4615, { 0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a } };
static const GUID f2l_IID_IWICImagingFactory =
    { 0xec5ec8a9, 0xc395, 0x4314, { 0x9c, 0x77, 0x54, 0xd7, 0xa9, 0x35, 0xff, 0x70 } };
static const GUID f2l_GUID_WICPixelFormat32bppPBGRA =
    { 0x6fddc324, 0x4e03, 0x4bfe, { 0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x10 } };

/* Windows' own PNG and JPEG decoders and their container formats. A file
 * is decoded only by the decoder of the format far2l named, never by
 * whatever codec WIC would pick from its content: a host must not be able to
 * feed other installed codecs (TIFF, ICO, third-party ones) with its bytes. */
static const GUID f2l_CLSID_WICPngDecoder =
    { 0x389ea17b, 0x5078, 0x4cde, { 0xb6, 0xef, 0x25, 0xc1, 0x51, 0x75, 0xc7, 0x51 } };
static const GUID f2l_CLSID_WICJpegDecoder =
    { 0x9456a480, 0xe88b, 0x43ea, { 0x9e, 0x73, 0x0b, 0x2d, 0x9b, 0x71, 0xb1, 0xca } };
static const GUID f2l_IID_IWICBitmapDecoder =
    { 0x9edde9e7, 0x8dee, 0x47ea, { 0x99, 0xdf, 0xe6, 0xfa, 0xf2, 0xed, 0x44, 0xbf } };
static const GUID f2l_GUID_ContainerFormatPng =
    { 0x1b7cfaf4, 0x713f, 0x473c, { 0xbb, 0xcd, 0x61, 0x37, 0x42, 0x5f, 0xae, 0xaf } };
static const GUID f2l_GUID_ContainerFormatJpeg =
    { 0x19e4a5aa, 0x5662, 0x4fc5, { 0xa0, 0xc0, 0x17, 0x58, 0x02, 0x8e, 0x10, 0x57 } };

static IWICImagingFactory *wic_factory;
static bool wic_tried;

static IWICBitmapDecoder *wic_decoder(unsigned fmt)
{
    void *dec = NULL;
    if (FAILED(CoCreateInstance(fmt == F2L_IMG_PNG ? &f2l_CLSID_WICPngDecoder
                                                   : &f2l_CLSID_WICJpegDecoder,
                                NULL, CLSCTX_INPROC_SERVER,
                                &f2l_IID_IWICBitmapDecoder, &dec)))
        return NULL;
    return (IWICBitmapDecoder *)dec;
}

/* The factory, once both decoders proved creatable (PNG and JPEG are one
 * capability to far2l: WP_IMGCAP_JPG is both low bits). */
static IWICImagingFactory *wic_get(void)
{
    if (!wic_tried) {
        void *f = NULL;
        IWICBitmapDecoder *png, *jpg;
        wic_tried = true;
        png = wic_decoder(F2L_IMG_PNG);
        jpg = wic_decoder(F2L_IMG_JPG);
        if (png && jpg &&
            SUCCEEDED(CoCreateInstance(&f2l_CLSID_WICImagingFactory, NULL,
                                       CLSCTX_INPROC_SERVER,
                                       &f2l_IID_IWICImagingFactory, &f)))
            wic_factory = (IWICImagingFactory *)f;
        if (png) IWICBitmapDecoder_Release(png);
        if (jpg) IWICBitmapDecoder_Release(jpg);
    }
    return wic_factory;
}

static bool wic_decode(void *ctx, unsigned fmt, const unsigned char *data,
                       size_t len, unsigned char **px, int *w, int *h)
{
    IWICImagingFactory *fac = wic_get();
    IWICStream *stream = NULL;
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *conv = NULL;
    GUID container;
    const GUID *want = fmt == F2L_IMG_PNG ? &f2l_GUID_ContainerFormatPng
                                          : &f2l_GUID_ContainerFormatJpeg;
    UINT uw = 0, uh = 0;
    bool ok = false;
    (void)ctx;

    *px = NULL;
    if (!fac || len == 0 || len > 0x7FFFFFFF ||
        (fmt != F2L_IMG_PNG && fmt != F2L_IMG_JPG))
        return false;
    if (FAILED(IWICImagingFactory_CreateStream(fac, &stream)) ||
        FAILED(IWICStream_InitializeFromMemory(stream, (BYTE *)data,
                                               (DWORD)len)) ||
        !(dec = wic_decoder(fmt)) ||
        FAILED(IWICBitmapDecoder_Initialize(dec, (IStream *)stream,
                                            WICDecodeMetadataCacheOnDemand)) ||
        FAILED(IWICBitmapDecoder_GetContainerFormat(dec, &container)) ||
        !IsEqualGUID(&container, want) ||
        FAILED(IWICBitmapDecoder_GetFrame(dec, 0, &frame)) ||
        FAILED(IWICImagingFactory_CreateFormatConverter(fac, &conv)) ||
        FAILED(IWICFormatConverter_Initialize(
                   conv, (IWICBitmapSource *)frame,
                   &f2l_GUID_WICPixelFormat32bppPBGRA,
                   WICBitmapDitherTypeNone, NULL, 0.0,
                   WICBitmapPaletteTypeCustom)) ||
        FAILED(IWICFormatConverter_GetSize(conv, &uw, &uh)))
        goto out;
    if (uw == 0 || uh == 0 || uw > F2L_IMG_MAX_SIDE || uh > F2L_IMG_MAX_SIDE ||
        (uint64_t)uw * uh > F2L_IMG_MAX_PIXELS)
        goto out;
    *px = malloc((size_t)uw * uh * 4);
    if (!*px)
        goto out;
    if (FAILED(IWICFormatConverter_CopyPixels(conv, NULL, uw * 4,
                                              uw * uh * 4, *px))) {
        free(*px);
        *px = NULL;
        goto out;
    }
    *w = (int)uw;
    *h = (int)uh;
    ok = true;
  out:
    if (conv) IWICFormatConverter_Release(conv);
    if (frame) IWICBitmapFrameDecode_Release(frame);
    if (dec) IWICBitmapDecoder_Release(dec);
    if (stream) IWICStream_Release(stream);
    return ok;
}
#endif /* _WIN32 */

/* ---- the environment of a request -------------------------------------- */

typedef struct TermCtx {
    Terminal *term;
    int off_x, off_y;
} TermCtx;

/* An image covered `before` and covers `after`: the cells under both are
 * drawn again, and the overlay is drawn over them at the end of the frame. */
static void img_changed(void *vctx, const Far2lRect *before,
                        const Far2lRect *after)
{
    TermCtx *tc = (TermCtx *)vctx;
    const Far2lRect *r[2];
    int i, c0, r0, c1, r1, cw = 0, ch = 0, ox, oy;
    r[0] = before;
    r[1] = after;
    if (!kitty_far2l_cell_hook ||
        !kitty_far2l_cell_hook(tc->term->win, &cw, &ch, &ox, &oy))
        return;
    for (i = 0; i < 2; i++)
        if (far2l_img_rect_cells(r[i], cw, ch, &c0, &r0, &c1, &r1) &&
            c1 >= 0 && r1 >= 0 && c0 < tc->term->cols && r0 < tc->term->rows)
            term_paint(tc->term, c0, r0, c1, r1, false);
}

#ifdef KITTY_TEST_BUILD_LABEL
/*
 * KITTY_FAR2L_IMG_TRACE=<file> (test builds only): one line per image
 * request, for the harness that checks the overlay's pixels:
 *   caps caps=0x<hex> cell=<w>x<h>
 *   set|transform|delete id=<id> ok=<1|0|-1> rect=<l>,<t>,<r>,<b>|none
 *                         (ok -1: malformed, answered with the ID alone)
 *   clear count=<n>       (kitty_far2l_images_reset: all images removed)
 * The rectangle is where the image is drawn, in client pixels of the
 * terminal window, right and bottom exclusive, before clipping to the
 * terminal area; "none" when no image is shown under that identity. Bytes of
 * the identity outside printable ASCII are written %XX.
 */
static void img_trace(void *vctx, char op, const char *id, size_t idlen,
                      int ok, const Far2lImage *img, uint64_t caps)
{
    TermCtx *tc = (TermCtx *)vctx;
    const char *path = getenv("KITTY_FAR2L_IMG_TRACE");
    int cw = 0, ch = 0, ox = 0, oy = 0;
    FILE *f;
    size_t i;
    Far2lRect r;
    if (!path || !*path)
        return;
    f = fopen(path, "a");
    if (!f)
        return;
    if (kitty_far2l_cell_hook)
        kitty_far2l_cell_hook(tc->term->win, &cw, &ch, &ox, &oy);
    if (op == 'c') {
        fprintf(f, "caps caps=0x%llx cell=%dx%d\n", (unsigned long long)caps,
                cw, ch);
        fclose(f);
        return;
    }
    fprintf(f, "%s id=", op == 's' ? "set" : op == 't' ? "transform" : "delete");
    for (i = 0; i < idlen; i++) {
        unsigned char c = (unsigned char)id[i];
        if (c > 0x20 && c < 0x7F && c != '%')
            fputc(c, f);
        else
            fprintf(f, "%%%02X", c);
    }
    fprintf(f, " ok=%d", ok);
    if (img && far2l_img_rect(img, cw, ch, &r))
        fprintf(f, " rect=%d,%d,%d,%d\n", r.x0 + ox, r.y0 + oy,
                r.x1 + ox, r.y1 + oy);
    else
        fprintf(f, " rect=none\n");
    fclose(f);
}
#endif

static void env_init(Terminal *term, TermCtx *tc, Far2lImageEnv *env)
{
    int cw = 0, ch = 0;
    memset(env, 0, sizeof(*env));
    tc->term = term;
    tc->off_x = tc->off_y = 0;
    if (kitty_far2l_cell_hook &&
        !kitty_far2l_cell_hook(term->win, &cw, &ch, &tc->off_x, &tc->off_y))
        cw = ch = 0;
    env->cell_w = cw;
    env->cell_h = ch;
    env->enabled = term->far2l_ext && cw > 0 && ch > 0 &&
                   conf_get_bool(term->conf, CONF_far2l_images);
    env->cur_x = term->curs.x;
    env->cur_y = term->curs.y;
#ifdef _WIN32
    if (env->enabled && wic_get()) {
        env->codecs = F2L_IMGCAP_PNG;
        env->decode = wic_decode;
    }
#endif
    env->changed = img_changed;
#ifdef KITTY_TEST_BUILD_LABEL
    env->trace = img_trace;
#endif
    env->ctx = tc;
}

char *kitty_far2l_image_request(Terminal *term, const char *stk, int len,
                                size_t max_data, int *reply_size)
{
    unsigned char out[F2L_IMG_REPLY_MAX];
    Far2lImageEnv env;
    TermCtx tc;
    char *reply;
    int n;

    if (!term->far2l_images) {
        term->far2l_images = snew(Far2lImageStore);
        memset(term->far2l_images, 0, sizeof(*term->far2l_images));
    }
    env_init(term, &tc, &env);
    env.max_data = max_data;
    n = far2l_img_request(term->far2l_images, &env, (const unsigned char *)stk,
                          len > 0 ? (size_t)len : 0, out);
    reply = snewn(n, char);
    memcpy(reply, out, n - 1);
    reply[n - 1] = 0;                  /* the caller's request ID goes here */
    *reply_size = n;
    return reply;
}

void kitty_far2l_images_reset(Terminal *term)
{
    Far2lImageEnv env;
    TermCtx tc;
    if (!term->far2l_images || !term->far2l_images->n)
        return;
    env_init(term, &tc, &env);
#ifdef KITTY_TEST_BUILD_LABEL
    {
        /* "clear count=<n>": every image went (reset, far2l0, session end) */
        const char *path = getenv("KITTY_FAR2L_IMG_TRACE");
        FILE *f = (path && *path) ? fopen(path, "a") : NULL;
        if (f) {
            fprintf(f, "clear count=%d\n", term->far2l_images->n);
            fclose(f);
        }
    }
#endif
    far2l_img_store_clear(term->far2l_images, &env);
}

void kitty_far2l_images_free(Terminal *term)
{
    if (!term->far2l_images)
        return;
    far2l_img_store_clear(term->far2l_images, NULL);
    sfree(term->far2l_images);
    term->far2l_images = NULL;
}

bool kitty_far2l_images_any(Terminal *term)
{
    return term && term->far2l_images && term->far2l_images->n > 0;
}
