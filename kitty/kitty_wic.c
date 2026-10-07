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

/* Windows' own PNG, JPEG, GIF, TIFF and BMP decoders (patched with Windows),
 * Microsoft's WebP and HEIF decoders (Store packages, updated by the Store)
 * and their container formats. Data is decoded only by the decoder of the
 * format the caller named, never by whatever codec WIC would pick from its
 * content: a host or a file must not be able to feed the codecs other
 * programs install (camera RAW, vendors' ones), which are updated by
 * whoever shipped them, if at all. */
static const GUID kw_CLSID_WICPngDecoder =
    { 0x389ea17b, 0x5078, 0x4cde, { 0xb6, 0xef, 0x25, 0xc1, 0x51, 0x75, 0xc7, 0x51 } };
static const GUID kw_CLSID_WICJpegDecoder =
    { 0x9456a480, 0xe88b, 0x43ea, { 0x9e, 0x73, 0x0b, 0x2d, 0x9b, 0x71, 0xb1, 0xca } };
static const GUID kw_CLSID_WICGifDecoder =
    { 0x381dda3c, 0x9ce9, 0x4834, { 0xa2, 0x3e, 0x1f, 0x98, 0xf8, 0xfc, 0x52, 0xbe } };
static const GUID kw_CLSID_WICTiffDecoder =
    { 0xb54e85d9, 0xfe23, 0x499f, { 0x8b, 0x88, 0x6a, 0xce, 0xa7, 0x13, 0x75, 0x2b } };
static const GUID kw_CLSID_WICBmpDecoder =
    { 0x6b462062, 0x7cbf, 0x400d, { 0x9f, 0xdb, 0x81, 0x3d, 0xd1, 0x0f, 0x27, 0x78 } };
static const GUID kw_CLSID_WICWebpDecoder =
    { 0x7693e886, 0x51c9, 0x4070, { 0x84, 0x19, 0x9f, 0x70, 0x73, 0x8e, 0xc8, 0xfa } };
static const GUID kw_CLSID_WICHeifDecoder =
    { 0xe9a4a80a, 0x44fe, 0x4de4, { 0x89, 0x71, 0x71, 0x50, 0xb1, 0x0a, 0x51, 0x99 } };
static const GUID kw_GUID_VendorMicrosoft =
    { 0xf0e749ca, 0xedef, 0x4589, { 0xa7, 0x3a, 0xee, 0x0e, 0x62, 0x6a, 0x2a, 0x2b } };
static const GUID kw_IID_IWICBitmapDecoder =
    { 0x9edde9e7, 0x8dee, 0x47ea, { 0x99, 0xdf, 0xe6, 0xfa, 0xf2, 0xed, 0x44, 0xbf } };
static const GUID kw_GUID_ContainerFormatPng =
    { 0x1b7cfaf4, 0x713f, 0x473c, { 0xbb, 0xcd, 0x61, 0x37, 0x42, 0x5f, 0xae, 0xaf } };
static const GUID kw_GUID_ContainerFormatJpeg =
    { 0x19e4a5aa, 0x5662, 0x4fc5, { 0xa0, 0xc0, 0x17, 0x58, 0x02, 0x8e, 0x10, 0x57 } };
static const GUID kw_GUID_ContainerFormatGif =
    { 0x1f8a5601, 0x7d4d, 0x4cbd, { 0x9c, 0x82, 0x1b, 0xc8, 0xd4, 0xee, 0xb9, 0xa5 } };
static const GUID kw_GUID_ContainerFormatTiff =
    { 0x163bcc30, 0xe2e9, 0x4f0b, { 0x96, 0x1d, 0xa3, 0xe9, 0xfd, 0xb7, 0x88, 0xa3 } };
static const GUID kw_GUID_ContainerFormatBmp =
    { 0x0af1d87e, 0xfcfe, 0x4188, { 0xbd, 0xeb, 0xa7, 0x90, 0x64, 0x71, 0xcb, 0xe3 } };
static const GUID kw_GUID_ContainerFormatWebp =
    { 0xe094b0e2, 0x67f2, 0x45b3, { 0xb0, 0xea, 0x11, 0x53, 0x37, 0xca, 0x7c, 0xf3 } };
static const GUID kw_GUID_ContainerFormatHeif =
    { 0xe1e62521, 0x6787, 0x405b, { 0xa3, 0x39, 0x50, 0x07, 0x15, 0xb5, 0x76, 0x3f } };

/* KITTY_WIC_* -> its decoder and its container format; NULL for no such. */
static const GUID *wic_clsid(int fmt)
{
    switch (fmt) {
      case KITTY_WIC_PNG: return &kw_CLSID_WICPngDecoder;
      case KITTY_WIC_JPG: return &kw_CLSID_WICJpegDecoder;
      case KITTY_WIC_GIF: return &kw_CLSID_WICGifDecoder;
      case KITTY_WIC_TIFF: return &kw_CLSID_WICTiffDecoder;
      case KITTY_WIC_BMP: return &kw_CLSID_WICBmpDecoder;
      case KITTY_WIC_WEBP: return &kw_CLSID_WICWebpDecoder;
      case KITTY_WIC_HEIF: return &kw_CLSID_WICHeifDecoder;
    }
    return NULL;
}

static const GUID *wic_container(int fmt)
{
    switch (fmt) {
      case KITTY_WIC_PNG: return &kw_GUID_ContainerFormatPng;
      case KITTY_WIC_JPG: return &kw_GUID_ContainerFormatJpeg;
      case KITTY_WIC_GIF: return &kw_GUID_ContainerFormatGif;
      case KITTY_WIC_TIFF: return &kw_GUID_ContainerFormatTiff;
      case KITTY_WIC_BMP: return &kw_GUID_ContainerFormatBmp;
      case KITTY_WIC_WEBP: return &kw_GUID_ContainerFormatWebp;
      case KITTY_WIC_HEIF: return &kw_GUID_ContainerFormatHeif;
    }
    return NULL;
}

static IWICImagingFactory *wic_factory;
static bool wic_tried;

static IWICBitmapDecoder *wic_decoder(int fmt)
{
    void *dec = NULL;
    const GUID *clsid = wic_clsid(fmt);
    if (!clsid ||
        FAILED(CoCreateInstance(clsid, NULL, CLSCTX_INPROC_SERVER,
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

/* A decoder of a Store codec (WebP, HEIF): by its class where that is
 * registered for this process, else through WIC's list of components -
 * taken only when it is that very class from Microsoft, so another
 * vendor's codec for the same format never gets the data. */
static IWICBitmapDecoder *wic_store_decoder(IWICImagingFactory *fac, int fmt)
{
    IWICBitmapDecoder *dec = wic_decoder(fmt);
    IWICBitmapDecoderInfo *info = NULL;
    CLSID got;
    GUID vendor;
    bool ok = false;

    if (dec)
        return dec;
    if (FAILED(IWICImagingFactory_CreateDecoder(fac, wic_container(fmt),
                                                &kw_GUID_VendorMicrosoft,
                                                &dec)) || !dec)
        return NULL;
    if (SUCCEEDED(IWICBitmapDecoder_GetDecoderInfo(dec, &info)) && info &&
        SUCCEEDED(IWICComponentInfo_GetCLSID((IWICComponentInfo *)info,
                                             &got)) &&
        SUCCEEDED(IWICComponentInfo_GetVendorGUID((IWICComponentInfo *)info,
                                                  &vendor)) &&
        IsEqualGUID(&got, wic_clsid(fmt)) &&
        IsEqualGUID(&vendor, &kw_GUID_VendorMicrosoft))
        ok = true;
    if (info)
        IWICBitmapDecoderInfo_Release(info);
    if (!ok) {
        IWICBitmapDecoder_Release(dec);
        return NULL;
    }
    return dec;
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
    const GUID *want = wic_container(fmt);
    UINT uw = 0, uh = 0;
    bool ok = false;

    *px = NULL;
    if (!fac || !want || len == 0 || len > 0x7FFFFFFF)
        return false;
    if (FAILED(IWICImagingFactory_CreateStream(fac, &stream)) ||
        FAILED(IWICStream_InitializeFromMemory(stream, (BYTE *)data,
                                               (DWORD)len)) ||
        !(dec = fmt == KITTY_WIC_WEBP || fmt == KITTY_WIC_HEIF ?
                    wic_store_decoder(fac, fmt) : wic_decoder(fmt)) ||
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
