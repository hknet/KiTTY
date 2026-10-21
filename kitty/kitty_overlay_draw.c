/*
 * kitty_overlay_draw.c - shapes on a pixel buffer for the layered overlays
 * (kitty_psoverlay.c, kitty_ctxcrumb.c, kitty_switchnotice.c); see
 * kitty_overlay_draw.h.
 */
#include <windows.h>
#include <math.h>
#include <string.h>
#include <wchar.h>

#include "kitty_overlay_draw.h"
#include "kitty_oldwin.h"     /* kitty_api_from: GetDpiForWindow */
#include "kitty_text.h"       /* KT_WINFEAT_DPI */

void kov_put(KovCanvas *c, int x, int y, COLORREF col, double a)
{
    DWORD *p;
    int r, g, b;
    if (a <= 0 || x < 0 || y < 0 || x >= c->w || y >= c->h)
        return;
    if (a > 1)
        a = 1;
    p = &c->px[y * c->w + x];
    r = (*p >> 16) & 0xFF; g = (*p >> 8) & 0xFF; b = *p & 0xFF;
    r = (int)(r + (GetRValue(col) - r) * a + 0.5);
    g = (int)(g + (GetGValue(col) - g) * a + 0.5);
    b = (int)(b + (GetBValue(col) - b) * a + 0.5);
    *p = ((DWORD)r << 16) | ((DWORD)g << 8) | (DWORD)b;
}

double kov_cov(double d)
{
    return d <= -0.5 ? 1 : d >= 0.5 ? 0 : 0.5 - d;
}

double kov_sd_rrect(double px, double py, double x0, double y0,
                    double x1, double y1, double r)
{
    double cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
    double qx = fabs(px - cx) - ((x1 - x0) / 2 - r);
    double qy = fabs(py - cy) - ((y1 - y0) / 2 - r);
    double ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    double in = qx > qy ? qx : qy;
    return sqrt(ox * ox + oy * oy) + (in < 0 ? in : 0) - r;
}

void kov_rrect(KovCanvas *c, double x0, double y0, double x1, double y1,
               double r, COLORREF col, double a)
{
    int x, y;
    for (y = (int)floor(y0) - 1; y <= (int)ceil(y1); y++)
        for (x = (int)floor(x0) - 1; x <= (int)ceil(x1); x++)
            kov_put(c, x, y, col,
                    a * kov_cov(kov_sd_rrect(x + .5, y + .5, x0, y0, x1, y1, r)));
}

void kov_rrect_edge(KovCanvas *c, double x0, double y0, double x1,
                    double y1, double r, double w, COLORREF col)
{
    int x, y;
    for (y = (int)floor(y0) - 1; y <= (int)ceil(y1); y++)
        for (x = (int)floor(x0) - 1; x <= (int)ceil(x1); x++) {
            double d = kov_sd_rrect(x + .5, y + .5, x0, y0, x1, y1, r);
            kov_put(c, x, y, col, kov_cov(fabs(d + w / 2) - w / 2));
        }
}

void kov_fill(KovCanvas *c, COLORREF col)
{
    DWORD v = ((DWORD)GetRValue(col) << 16) | ((DWORD)GetGValue(col) << 8) |
              GetBValue(col);
    int i;
    for (i = 0; i < c->w * c->h; i++)
        c->px[i] = v;
}

void kov_premultiply_rrect(KovCanvas *c, double r, int alpha)
{
    int i;
    for (i = 0; i < c->w * c->h; i++) {
        int px = i % c->w, py = i / c->w;
        int a = (int)(kov_cov(kov_sd_rrect(px + .5, py + .5, 0, 0, c->w, c->h, r)) *
                      alpha + .5);
        DWORD v = c->px[i];
        int cr = (v >> 16) & 0xFF, cg = (v >> 8) & 0xFF, cb = v & 0xFF;
        c->px[i] = ((DWORD)a << 24) | ((DWORD)(cr * a / 255) << 16) |
                   ((DWORD)(cg * a / 255) << 8) | (DWORD)(cb * a / 255);
    }
}

void kov_dib_free(KovDib *d)
{
    if (d->dc) {
        SelectObject(d->dc, d->old);
        DeleteDC(d->dc);
    }
    if (d->bmp)
        DeleteObject(d->bmp);
    memset(d, 0, sizeof(*d));
}

bool kov_dib(KovDib *d, int w, int h)
{
    BITMAPINFO bi;
    void *bits = NULL;
    HDC sdc;
    if (d->dc && w == d->w && h == d->h)
        return true;
    kov_dib_free(d);
    if (w <= 0 || h <= 0)
        return false;
    sdc = GetDC(NULL);
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;        /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    d->bmp = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    d->dc = CreateCompatibleDC(sdc);
    ReleaseDC(NULL, sdc);
    if (!d->bmp || !bits || !d->dc) {
        if (d->dc) DeleteDC(d->dc);
        if (d->bmp) DeleteObject(d->bmp);
        memset(d, 0, sizeof(*d));
        return false;
    }
    d->old = (HBITMAP)SelectObject(d->dc, d->bmp);
    d->px = (DWORD *)bits;
    d->w = w;
    d->h = h;
    return true;
}

UINT kov_dpi(HWND owner)
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
    if (fn && owner)
        d = fn(owner);
    if (!d) {
        HDC dc = GetDC(NULL);
        if (dc) {
            d = GetDeviceCaps(dc, LOGPIXELSX);
            ReleaseDC(NULL, dc);
        }
    }
    return d ? d : 96;
}

bool kov_high_contrast(void)
{
    HIGHCONTRASTA hc;
    memset(&hc, 0, sizeof(hc));
    hc.cbSize = sizeof(hc);
    return SystemParametersInfoA(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0) &&
           (hc.dwFlags & HCF_HIGHCONTRASTON);
}

HFONT kov_font(int px, bool bold)
{
    LOGFONTW lf;
    memset(&lf, 0, sizeof(lf));
    lf.lfHeight = -px;
    lf.lfWeight = bold ? FW_SEMIBOLD : FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = ANTIALIASED_QUALITY;
    wcscpy(lf.lfFaceName, L"Segoe UI");
    return CreateFontIndirectW(&lf);
}
