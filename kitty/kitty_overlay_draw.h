/*
 * kitty_overlay_draw.h - shapes on a pixel buffer for the overlays drawn
 * into a layered window: the program status panel (kitty_psoverlay.c, OSC
 * 7501) and the OSC 3008 breadcrumb (kitty_ctxcrumb.c). The buffer is
 * 0x00RRGGBB, top-down; edges are smoothed from signed distances, so no
 * GDI+ is needed. The caller premultiplies by its alpha afterwards.
 */
#ifndef KITTY_OVERLAY_DRAW_H
#define KITTY_OVERLAY_DRAW_H

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

#endif
