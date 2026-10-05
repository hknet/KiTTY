/*
 * kitty_wic.c - images through the Windows Imaging Component; see kitty_wic.h.
 *
 * Created at run time through COM (CoCreateInstance): nothing here is
 * imported that XP lacks, and a Windows without WIC reports the formats
 * unavailable. Own copies of the GUIDs, so nothing needs uuid.lib.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COBJMACROS
#include <windows.h>
#include <wincodec.h>

#include "kitty_wic.h"

static const GUID kw_CLSID_WICImagingFactory =
    { 0xcacaf262, 0x9370, 0x4615, { 0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a } };
static const GUID kw_IID_IWICImagingFactory =
    { 0xec5ec8a9, 0xc395, 0x4314, { 0x9c, 0x77, 0x54, 0xd7, 0xa9, 0x35, 0xff, 0x70 } };
static const GUID kw_GUID_WICPixelFormat32bppPBGRA =
    { 0x6fddc324, 0x4e03, 0x4bfe, { 0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x10 } };
static const GUID kw_GUID_WICPixelFormat24bppBGR =
    { 0x6fddc324, 0x4e03, 0x4bfe, { 0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0c } };

/* Windows' own PNG and JPEG decoders and their container formats. Data is
 * decoded only by the decoder of the format the caller named, never by
 * whatever codec WIC would pick from its content: a host or a file must not
 * be able to feed other installed codecs (TIFF, ICO, third-party ones). */
static const GUID kw_CLSID_WICPngDecoder =
    { 0x389ea17b, 0x5078, 0x4cde, { 0xb6, 0xef, 0x25, 0xc1, 0x51, 0x75, 0xc7, 0x51 } };
static const GUID kw_CLSID_WICJpegDecoder =
    { 0x9456a480, 0xe88b, 0x43ea, { 0x9e, 0x73, 0x0b, 0x2d, 0x9b, 0x71, 0xb1, 0xca } };
static const GUID kw_IID_IWICBitmapDecoder =
    { 0x9edde9e7, 0x8dee, 0x47ea, { 0x99, 0xdf, 0xe6, 0xfa, 0xf2, 0xed, 0x44, 0xbf } };
static const GUID kw_GUID_ContainerFormatPng =
    { 0x1b7cfaf4, 0x713f, 0x473c, { 0xbb, 0xcd, 0x61, 0x37, 0x42, 0x5f, 0xae, 0xaf } };
static const GUID kw_GUID_ContainerFormatJpeg =
    { 0x19e4a5aa, 0x5662, 0x4fc5, { 0xa0, 0xc0, 0x17, 0x58, 0x02, 0x8e, 0x10, 0x57 } };

static IWICImagingFactory *wic_factory;
static bool wic_tried;

static IWICBitmapDecoder *wic_decoder(int fmt)
{
    void *dec = NULL;
    if (FAILED(CoCreateInstance(fmt == KITTY_WIC_PNG ? &kw_CLSID_WICPngDecoder
                                                     : &kw_CLSID_WICJpegDecoder,
                                NULL, CLSCTX_INPROC_SERVER,
                                &kw_IID_IWICBitmapDecoder, &dec)))
        return NULL;
    return (IWICBitmapDecoder *)dec;
}

/* The factory, once both decoders proved creatable. */
static IWICImagingFactory *wic_get(void)
{
    if (!wic_tried) {
        void *f = NULL;
        IWICBitmapDecoder *png, *jpg;
        wic_tried = true;
        png = wic_decoder(KITTY_WIC_PNG);
        jpg = wic_decoder(KITTY_WIC_JPG);
        if (png && jpg &&
            SUCCEEDED(CoCreateInstance(&kw_CLSID_WICImagingFactory, NULL,
                                       CLSCTX_INPROC_SERVER,
                                       &kw_IID_IWICImagingFactory, &f)))
            wic_factory = (IWICImagingFactory *)f;
        if (png) IWICBitmapDecoder_Release(png);
        if (jpg) IWICBitmapDecoder_Release(jpg);
    }
    return wic_factory;
}

bool kitty_wic_available(void)
{
    return wic_get() != NULL;
}

bool kitty_wic_decode(int fmt, const unsigned char *data, size_t len,
                      unsigned max_side, uint64_t max_pixels,
                      unsigned char **px, int *w, int *h)
{
    IWICImagingFactory *fac = wic_get();
    IWICStream *stream = NULL;
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *conv = NULL;
    GUID container;
    const GUID *want = fmt == KITTY_WIC_PNG ? &kw_GUID_ContainerFormatPng
                                            : &kw_GUID_ContainerFormatJpeg;
    UINT uw = 0, uh = 0;
    bool ok = false;

    *px = NULL;
    if (!fac || len == 0 || len > 0x7FFFFFFF ||
        (fmt != KITTY_WIC_PNG && fmt != KITTY_WIC_JPG))
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
                   &kw_GUID_WICPixelFormat32bppPBGRA,
                   WICBitmapDitherTypeNone, NULL, 0.0,
                   WICBitmapPaletteTypeCustom)) ||
        FAILED(IWICFormatConverter_GetSize(conv, &uw, &uh)))
        goto out;
    if (uw == 0 || uh == 0 || uw > max_side || uh > max_side ||
        (uint64_t)uw * uh > max_pixels)
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

/* A background image: up to 16384 pixels a side, 64 megapixels. */
#define KW_FILE_MAX_BYTES  (256u * 1024u * 1024u)
#define KW_FILE_MAX_SIDE   16384u
#define KW_FILE_MAX_PIXELS (64ull * 1024u * 1024u)

HBITMAP kitty_wic_load_file(const char *path, int fmt)
{
    FILE *fp;
    long size = 0;
    unsigned char *data = NULL, *px = NULL;
    int w = 0, h = 0, x, y, stride;
    HBITMAP bm = NULL;
    BITMAPINFOHEADER bih;
    BYTE *bits = NULL;
    HDC dc;

    if (!wic_get() || !(fp = fopen(path, "rb")))
        return NULL;
    if (fseek(fp, 0, SEEK_END) == 0 && (size = ftell(fp)) > 0 &&
        (unsigned long)size <= KW_FILE_MAX_BYTES &&
        fseek(fp, 0, SEEK_SET) == 0 && (data = malloc((size_t)size)) != NULL &&
        fread(data, 1, (size_t)size, fp) != (size_t)size) {
        free(data);
        data = NULL;
    }
    fclose(fp);
    if (!data)
        return NULL;
    if (!kitty_wic_decode(fmt, data, (size_t)size, KW_FILE_MAX_SIDE,
                          KW_FILE_MAX_PIXELS, &px, &w, &h)) {
        free(data);
        return NULL;
    }
    free(data);

    /* 24 bpp bottom-up, as the BMP path gives: transparent pixels end up
     * black (the premultiplied colour), which the background then shows */
    memset(&bih, 0, sizeof(bih));
    bih.biSize = sizeof(bih);
    bih.biWidth = w;
    bih.biHeight = h;
    bih.biPlanes = 1;
    bih.biBitCount = 24;
    bih.biCompression = BI_RGB;
    dc = CreateCompatibleDC(NULL);
    bm = CreateDIBSection(dc, (BITMAPINFO *)&bih, DIB_RGB_COLORS,
                          (void **)&bits, NULL, 0);
    DeleteDC(dc);
    if (bm && bits) {
        stride = ((w * 3) + 3) & ~3;
        for (y = 0; y < h; y++) {
            const unsigned char *s = px + (size_t)y * w * 4;
            BYTE *d = bits + (size_t)(h - 1 - y) * stride;
            for (x = 0; x < w; x++, s += 4, d += 3) {
                d[0] = s[0];
                d[1] = s[1];
                d[2] = s[2];
            }
        }
    }
    free(px);
    return bm;
}

bool kitty_wic_save_png(HBITMAP hbm, const char *path)
{
    IWICImagingFactory *fac = wic_get();
    IWICStream *stream = NULL;
    IWICBitmapEncoder *enc = NULL;
    IWICBitmapFrameEncode *frame = NULL;
    WICPixelFormatGUID pf;
    BITMAP bm;
    BITMAPINFO bi;
    BYTE *bits = NULL;
    HDC dc;
    WCHAR *wpath = NULL;
    int n, stride;
    bool ok = false;

    if (!fac || !hbm || !GetObject(hbm, sizeof(bm), &bm) ||
        bm.bmWidth <= 0 || bm.bmHeight <= 0)
        return false;

    /* the pixels, 24 bpp top-down, as the PNG encoder takes them */
    stride = ((bm.bmWidth * 3) + 3) & ~3;
    bits = malloc((size_t)stride * bm.bmHeight);
    if (!bits)
        return false;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = bm.bmWidth;
    bi.bmiHeader.biHeight = -bm.bmHeight;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 24;
    bi.bmiHeader.biCompression = BI_RGB;
    dc = GetDC(NULL);
    n = GetDIBits(dc, hbm, 0, bm.bmHeight, bits, &bi, DIB_RGB_COLORS);
    ReleaseDC(NULL, dc);
    if (n != bm.bmHeight)
        goto out;

    n = MultiByteToWideChar(CP_ACP, 0, path, -1, NULL, 0);
    if (n <= 0 || !(wpath = malloc(n * sizeof(WCHAR))) ||
        !MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, n))
        goto out;

    pf = kw_GUID_WICPixelFormat24bppBGR;
    if (FAILED(IWICImagingFactory_CreateStream(fac, &stream)) ||
        FAILED(IWICStream_InitializeFromFilename(stream, wpath, GENERIC_WRITE)) ||
        FAILED(IWICImagingFactory_CreateEncoder(fac, &kw_GUID_ContainerFormatPng,
                                                NULL, &enc)) ||
        FAILED(IWICBitmapEncoder_Initialize(enc, (IStream *)stream,
                                            WICBitmapEncoderNoCache)) ||
        FAILED(IWICBitmapEncoder_CreateNewFrame(enc, &frame, NULL)) ||
        FAILED(IWICBitmapFrameEncode_Initialize(frame, NULL)) ||
        FAILED(IWICBitmapFrameEncode_SetSize(frame, bm.bmWidth, bm.bmHeight)) ||
        FAILED(IWICBitmapFrameEncode_SetPixelFormat(frame, &pf)) ||
        !IsEqualGUID(&pf, &kw_GUID_WICPixelFormat24bppBGR) ||
        FAILED(IWICBitmapFrameEncode_WritePixels(frame, bm.bmHeight, stride,
                                                 stride * bm.bmHeight, bits)) ||
        FAILED(IWICBitmapFrameEncode_Commit(frame)) ||
        FAILED(IWICBitmapEncoder_Commit(enc)))
        goto out;
    ok = true;
  out:
    if (frame) IWICBitmapFrameEncode_Release(frame);
    if (enc) IWICBitmapEncoder_Release(enc);
    if (stream) IWICStream_Release(stream);
    free(wpath);
    free(bits);
    return ok;
}
