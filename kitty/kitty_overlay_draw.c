/*
 * kitty_overlay_draw.c - shapes on a pixel buffer for the layered overlays
 * (kitty_psoverlay.c, kitty_ctxcrumb.c); see kitty_overlay_draw.h.
 */
#include <windows.h>
#include <math.h>

#include "kitty_overlay_draw.h"

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
