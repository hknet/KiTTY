/*
 * kitty_overlay_draw.h - shapes on a pixel buffer for the overlays drawn
 * into a layered window: the program status panel (kitty_psoverlay.c, OSC
 * 7501), the OSC 3008 breadcrumb (kitty_ctxcrumb.c) and the one-line
 * switch notices (kitty_switchnotice.c). The buffer is 0x00RRGGBB,
 * top-down; edges are smoothed from signed distances, so no GDI+ is
 * needed. The caller premultiplies by its alpha afterwards. Also what all
 * three share around the drawing: the DIB, the DPI, high contrast, the
 * font.
 */
#ifndef KITTY_OVERLAY_DRAW_H
#define KITTY_OVERLAY_DRAW_H

#include <stdbool.h>
#include <windows.h>

typedef struct {
    DWORD *px;
    int w, h;
} KovCanvas;

/* Blend col into one pixel with coverage a (0..1). */
void kov_put(KovCanvas *c, int x, int y, COLORREF col, double a);
/* Coverage of a pixel whose centre is d pixels outside a shape's edge. */
double kov_cov(double d);
/* Signed distance from (px,py) to a rounded rectangle, radius r. */
double kov_sd_rrect(double px, double py, double x0, double y0,
                    double x1, double y1, double r);
/* A filled rounded rectangle at coverage a. */
void kov_rrect(KovCanvas *c, double x0, double y0, double x1, double y1,
               double r, COLORREF col, double a);
/* The outline only, w wide, inside the edge. */
void kov_rrect_edge(KovCanvas *c, double x0, double y0, double x1,
                    double y1, double r, double w, COLORREF col);
/* Every pixel one colour. */
void kov_fill(KovCanvas *c, COLORREF col);
/* The finished picture cut to a rounded rectangle over the whole canvas,
 * radius r, at alpha (0..255), premultiplied - what UpdateLayeredWindow
 * takes. */
void kov_premultiply_rrect(KovCanvas *c, double r, int alpha);

/* A 32-bit top-down DIB selected into a memory DC: what an overlay draws
 * into and hands to UpdateLayeredWindow. Kept between frames; made again
 * only for another size. Zeroed = none. */
typedef struct {
    HDC dc;
    HBITMAP bmp, old;
    DWORD *px;
    int w, h;
} KovDib;
bool kov_dib(KovDib *d, int w, int h);
void kov_dib_free(KovDib *d);

/* The DPI of the owner window (GetDpiForWindow where Windows has it, else
 * the screen's); 96 when unknown. */
UINT kov_dpi(HWND owner);
/* High contrast is on: the overlays take the system colours, opaque. */
bool kov_high_contrast(void);
/* Segoe UI, px pixels high. Grey antialiasing, not ClearType: the text
 * gets alpha with the rest, and ClearType's coloured fringes would show
 * through it. The caller deletes it. */
HFONT kov_font(int px, bool bold);

#endif
