/*
 * Edge anchoring for resizable dialogs. See kitty_anchor.h for the contract.
 *
 * This was three copies of the same twenty lines inside windows/pageant.c
 * before the configuration box became the fourth window that needed it.
 */
#include "kitty_anchor.h"

/* One control's new rectangle, given how far the client area grew. */
static void anchored_place(HDWP *hdwp, HWND c, unsigned a, RECT r,
                           int dx, int dy)
{
    int x = r.left +
        (((a & KL_ANCH_RIGHT) && !(a & KL_ANCH_LEFT)) ? dx : 0);
    int y = r.top +
        (((a & KL_ANCH_BOTTOM) && !(a & KL_ANCH_TOP)) ? dy : 0);
    int w = (r.right - r.left) +
        (((a & KL_ANCH_LEFT) && (a & KL_ANCH_RIGHT)) ? dx : 0);
    int h = (r.bottom - r.top) +
        (((a & KL_ANCH_TOP) && (a & KL_ANCH_BOTTOM)) ? dy : 0);
    *hdwp = DeferWindowPos(*hdwp, c, NULL, x, y, w, h,
                           SWP_NOZORDER | SWP_NOACTIVATE);
}

/* Where a child sits in its parent's client coordinates. */
static void anchored_rect(HWND parent, HWND c, RECT *out)
{
    RECT r = {0, 0, 0, 0};
    if (c) {
        GetWindowRect(c, &r);
        MapWindowPoints(NULL, parent, (POINT *)&r, 2);
    }
    *out = r;
}

static void anchored_capture_common(HWND hwnd, SIZE *basesize, SIZE *minsize)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    basesize->cx = rc.right - rc.left;
    basesize->cy = rc.bottom - rc.top;
    GetWindowRect(hwnd, &rc);
    minsize->cx = rc.right - rc.left;
    minsize->cy = rc.bottom - rc.top;
}

static void anchored_delta(HWND hwnd, SIZE basesize, int *dx, int *dy)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    *dx = (rc.right - rc.left) - basesize.cx;
    *dy = (rc.bottom - rc.top) - basesize.cy;
}

void anchored_capture(HWND hwnd, const struct kl_anchor *anchors, size_t n,
                      RECT *rects, SIZE *basesize, SIZE *minsize)
{
    for (size_t i = 0; i < n; i++)
        anchored_rect(hwnd, GetDlgItem(hwnd, anchors[i].id), &rects[i]);
    anchored_capture_common(hwnd, basesize, minsize);
}

void anchored_relayout(HWND hwnd, const struct kl_anchor *anchors, size_t n,
                       const RECT *rects, SIZE basesize)
{
    int dx, dy;
    anchored_delta(hwnd, basesize, &dx, &dy);
    HDWP hdwp = BeginDeferWindowPos((int)n);
    for (size_t i = 0; i < n; i++) {
        HWND c = GetDlgItem(hwnd, anchors[i].id);
        if (!c)
            continue;                  /* e.g. Help destroyed when no help */
        anchored_place(&hdwp, c, anchors[i].anchor, rects[i], dx, dy);
    }
    EndDeferWindowPos(hdwp);
    InvalidateRect(hwnd, NULL, TRUE);
}

void anchored_capture_windows(HWND parent, const struct kl_anchor_win *anchors,
                              size_t n, RECT *rects, SIZE *basesize,
                              SIZE *minsize)
{
    for (size_t i = 0; i < n; i++)
        anchored_rect(parent, anchors[i].hwnd, &rects[i]);
    anchored_capture_common(parent, basesize, minsize);
}

void anchored_relayout_windows(HWND parent, const struct kl_anchor_win *anchors,
                               size_t n, const RECT *rects, SIZE basesize)
{
    int dx, dy;
    anchored_delta(parent, basesize, &dx, &dy);
    HDWP hdwp = BeginDeferWindowPos((int)n);
    for (size_t i = 0; i < n; i++) {
        /* Unlike the id form there is nothing to re-resolve, so a window that
         * has since been destroyed has to be checked for here. */
        if (!anchors[i].hwnd || !IsWindow(anchors[i].hwnd))
            continue;
        anchored_place(&hdwp, anchors[i].hwnd, anchors[i].anchor, rects[i],
                       dx, dy);
    }
    EndDeferWindowPos(hdwp);
    InvalidateRect(parent, NULL, TRUE);
}
