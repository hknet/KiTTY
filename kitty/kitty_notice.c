/*
 * kitty_notice.c: a small notification window of our own, near the clock.
 *
 * Why not the standard tray balloon, which the clipboard notices use: Windows
 * has ignored the requested balloon duration since Vista - the shell decides,
 * driven by accessibility settings - and a standard balloon cannot be coloured.
 * Workplace proxy mode needs both: the notice carries the mode's colour so the
 * notice and the coloured window frame read as one thing, and it stays up long
 * enough to be read (design/TASK_workplace_proxy.md §6a).
 *
 * Owning a window means owning it properly, and these are the parts that are
 * easy to get wrong:
 *
 *  - ⛔ It must NEVER take the focus. A notification that steals focus while
 *    somebody is typing into a terminal is worse than no notification at all:
 *    the next keystrokes go somewhere else. Hence WS_EX_NOACTIVATE and
 *    SW_SHOWNOACTIVATE, and no SetForegroundWindow anywhere.
 *  - Per-DPI sizing: the font and every measurement come from the DPI of the
 *    monitor the notice appears on, not from the primary monitor's.
 *  - Multi-monitor: it belongs beside the tray, which is not always on the
 *    primary display, so the position is derived from the tray window's own
 *    monitor and that monitor's WORK AREA (so it does not sit under the
 *    taskbar, wherever the taskbar is docked).
 *  - High contrast: in a high-contrast theme the accent colour is used only for
 *    a border, and the system window/text colours are used for the body. A
 *    themed background with our own text colour on it is how notices become
 *    unreadable for the people who need the theme.
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "kitty_notice.h"
#include "kitty_oldwin.h"   /* record what an older Windows does not have */
#include "kitty_text.h"     /* the feature name in the old-Windows report */

#define NOTICE_CLASS   "KiTTYNoticeWindow"
#define NOTICE_TIMER   1
#define NOTICE_MARGIN  16     /* gap from the work-area edge, at 96 dpi */
#define NOTICE_PAD     14     /* inside padding, at 96 dpi */
#define NOTICE_WIDTH   340    /* at 96 dpi; the height is measured from the text */

struct notice_state {
    char title[128];
    char text[512];
    COLORREF accent;
    HFONT title_font;
    HFONT body_font;
    UINT dpi;
    HWND click_hwnd;          /* who to tell when the notice is clicked */
    UINT click_msg;           /* 0 = clicking only dismisses */
    /* Hover holds the notice open: its time can run out while somebody is
     * reading it, or while a screen reader is speaking it, and a notice that
     * disappears mid-sentence has failed at the only thing it does. */
    int expired;              /* the time ran out while the pointer was on it */
    int tracking;             /* TrackMouseEvent is armed for WM_MOUSELEAVE */
    int seconds;              /* its full display time */
};

/* Grace period after the pointer leaves an already-expired notice, so a hand
 * that slips off it does not take the notice with it. */
#define NOTICE_GRACE_MS 2000

static HWND notice_hwnd = NULL;

static int notice_high_contrast(void)
{
    HIGHCONTRASTA hc;
    memset(&hc, 0, sizeof(hc));
    hc.cbSize = sizeof(hc);
    if (!SystemParametersInfoA(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0))
        return 0;
    return (hc.dwFlags & HCF_HIGHCONTRASTON) ? 1 : 0;
}

/* DPI of a given monitor, falling back to the system DPI on Windows 7/8 where
 * GetDpiForMonitor does not exist. Resolved at runtime so the binary still
 * starts on those. */
static UINT notice_dpi_for_monitor(HMONITOR mon)
{
    typedef HRESULT (WINAPI *getdpi_fn)(HMONITOR, int, UINT *, UINT *);
    static getdpi_fn fn = NULL;
    static int tried = 0;
    UINT x = 96, y = 96;
    if (!tried) {
        HMODULE m = LoadLibraryA("shcore.dll");
        if (m)
            fn = (getdpi_fn)kitty_api_from(m, "shcore.dll", "GetDpiForMonitor", KITTY_API_OPTIONAL,
                                  KT_WINFEAT_DPI);
        tried = 1;
    }
    if (fn && mon && SUCCEEDED(fn(mon, 0 /* MDT_EFFECTIVE_DPI */, &x, &y)))
        return x;
    {
        HDC dc = GetDC(NULL);
        if (dc) {
            x = GetDeviceCaps(dc, LOGPIXELSX);
            ReleaseDC(NULL, dc);
        }
    }
    return x ? x : 96;
}

static int notice_scale(int v, UINT dpi) { return MulDiv(v, (int)dpi, 96); }

static HFONT notice_font(UINT dpi, int bold)
{
    NONCLIENTMETRICSA ncm;
    LOGFONTA lf;
    memset(&ncm, 0, sizeof(ncm));
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        lf = ncm.lfMessageFont;
        /* SPI_GETNONCLIENTMETRICS reports at the SYSTEM dpi, so rescale to the
         * monitor this notice is actually on. */
        lf.lfHeight = -MulDiv(-lf.lfHeight, (int)dpi, (int)notice_dpi_for_monitor(NULL));
    } else {
        memset(&lf, 0, sizeof(lf));
        lf.lfHeight = -notice_scale(12, dpi);
        strcpy(lf.lfFaceName, "Segoe UI");
    }
    if (bold)
        lf.lfWeight = FW_BOLD;
    return CreateFontIndirectA(&lf);
}

/* Is the pointer on the notice RIGHT NOW?
 *
 * ⚠️ Asked directly rather than inferred from WM_MOUSEMOVE, because a pointer
 * that is already resting on the notice and does not move sends no messages at
 * all - so the first version closed the notice under a pointer that had been
 * parked on it for the whole fifteen seconds. Movement tells
 * us a hover STARTED; only the cursor position tells us one is happening.
 */
static int notice_pointer_is_over(HWND hwnd)
{
    POINT pt;
    RECT rc;
    if (!GetCursorPos(&pt) || !GetWindowRect(hwnd, &rc))
        return 0;
    return PtInRect(&rc, pt) ? 1 : 0;
}

static void notice_close(HWND hwnd)
{
    struct notice_state *st =
        (struct notice_state *)GetWindowLongPtr(hwnd, GWLP_USERDATA);
    KillTimer(hwnd, NOTICE_TIMER);
    if (st) {
        if (st->title_font) DeleteObject(st->title_font);
        if (st->body_font)  DeleteObject(st->body_font);
        free(st);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, 0);
    }
    if (notice_hwnd == hwnd)
        notice_hwnd = NULL;
    DestroyWindow(hwnd);
}

static LRESULT CALLBACK notice_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    struct notice_state *st =
        (struct notice_state *)GetWindowLongPtr(hwnd, GWLP_USERDATA);

    switch (msg) {
      case WM_TIMER:
        if (wp == NOTICE_TIMER) {
            /* Time is up - but not while the pointer is on the notice. Remember
             * that it lapsed and stop the timer; leaving restarts it with the
             * short grace period below. */
            KillTimer(hwnd, NOTICE_TIMER);
            if (st && notice_pointer_is_over(hwnd)) {
                st->expired = 1;
                /* Arm the leave notification if a stationary pointer meant we
                 * never saw a WM_MOUSEMOVE to arm it - otherwise the notice
                 * would hang there for ever once the pointer moved away. */
                if (!st->tracking) {
                    TRACKMOUSEEVENT tme;
                    memset(&tme, 0, sizeof(tme));
                    tme.cbSize = sizeof(tme);
                    tme.dwFlags = TME_LEAVE;
                    tme.hwndTrack = hwnd;
                    if (TrackMouseEvent(&tme))
                        st->tracking = 1;
                }
                return 0;
            }
            notice_close(hwnd);
            return 0;
        }
        break;

      case WM_MOUSEMOVE:
        if (st) {
            /* Coming back on during the grace period holds it open again; the
             * countdown itself is not touched, because whether the pointer is
             * on the notice is decided when the time actually runs out. */
            if (st->expired)
                KillTimer(hwnd, NOTICE_TIMER);
            if (!st->tracking) {
                TRACKMOUSEEVENT tme;
                memset(&tme, 0, sizeof(tme));
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = hwnd;
                if (TrackMouseEvent(&tme))
                    st->tracking = 1;
            }
        }
        return 0;

      case WM_MOUSELEAVE:
        if (st) {
            st->tracking = 0;
            /* Only an expired notice needs anything here: the pointer was what
             * was keeping it alive, so give the short grace period in case the
             * hand slipped. One still counting down keeps its own timer. */
            if (st->expired)
                SetTimer(hwnd, NOTICE_TIMER, NOTICE_GRACE_MS, NULL);
        }
        return 0;
      case WM_LBUTTONUP:
        /* A click does whatever the notice was given to do, and dismisses it
         * either way - a notice you cannot get rid of is a nuisance. */
        if (st && st->click_hwnd && st->click_msg)
            PostMessage(st->click_hwnd, st->click_msg, 0, 0);
        notice_close(hwnd);
        return 0;
      case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc, tr;
        int hc = notice_high_contrast();
        COLORREF bg = hc ? GetSysColor(COLOR_WINDOW) : (st ? st->accent : RGB(0,100,0));
        COLORREF fg = hc ? GetSysColor(COLOR_WINDOWTEXT) : RGB(255, 255, 255);
        int pad = notice_scale(NOTICE_PAD, st ? st->dpi : 96);
        HBRUSH br = CreateSolidBrush(bg);
        HFONT old;

        GetClientRect(hwnd, &rc);
        FillRect(dc, &rc, br);
        DeleteObject(br);
        /* A border always: on the themed background it separates the notice
         * from whatever is behind it, and in high contrast it is the only place
         * the mode's colour appears. */
        {
            HPEN pen = CreatePen(PS_SOLID, notice_scale(1, st ? st->dpi : 96),
                                 st ? st->accent : RGB(0,100,0));
            HPEN oldpen = (HPEN)SelectObject(dc, pen);
            HBRUSH oldbr = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
            Rectangle(dc, 0, 0, rc.right, rc.bottom);
            SelectObject(dc, oldpen);
            SelectObject(dc, oldbr);
            DeleteObject(pen);
        }

        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, fg);
        tr = rc;
        tr.left += pad; tr.right -= pad; tr.top += pad;
        if (st) {
            old = (HFONT)SelectObject(dc, st->title_font);
            DrawTextA(dc, st->title, -1, &tr, DT_LEFT | DT_TOP | DT_SINGLELINE);
            {
                TEXTMETRICA tm;
                GetTextMetricsA(dc, &tm);
                tr.top += tm.tmHeight + notice_scale(4, st->dpi);
            }
            SelectObject(dc, st->body_font);
            DrawTextA(dc, st->text, -1, &tr, DT_LEFT | DT_TOP | DT_WORDBREAK);
            SelectObject(dc, old);
        }
        EndPaint(hwnd, &ps);
        return 0;
      }
      case WM_DESTROY:
        if (notice_hwnd == hwnd)
            notice_hwnd = NULL;
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

static void notice_register_class(void)
{
    static int done = 0;
    WNDCLASSA wc;
    if (done)
        return;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = notice_wndproc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = NOTICE_CLASS;
    RegisterClassA(&wc);
    done = 1;
}

void kitty_notice_show(const char *title, const char *text, COLORREF accent,
                       int seconds, HWND click_hwnd, unsigned int click_msg)
{
    struct notice_state *st;
    HWND hwnd;
    HMONITOR mon;
    MONITORINFO mi;
    HWND tray;
    UINT dpi;
    int w, h, pad;
    RECT measure;
    HDC dc;
    HFONT old;

    if (!title || !text)
        return;
    notice_register_class();

    /* Beside the TRAY's monitor, not the primary one: on a two-monitor desk the
     * taskbar is often not on the primary display, and a notice about the tray
     * icon that appears on the other screen is a notice nobody sees. */
    tray = FindWindowA("Shell_TrayWnd", NULL);
    mon = MonitorFromWindow(tray ? tray : GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
    memset(&mi, 0, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoA(mon, &mi))
        return;
    dpi = notice_dpi_for_monitor(mon);
    pad = notice_scale(NOTICE_PAD, dpi);
    w = notice_scale(NOTICE_WIDTH, dpi);

    st = (struct notice_state *)calloc(1, sizeof(*st));
    if (!st)
        return;
    st->accent = accent;
    st->dpi = dpi;
    st->seconds = seconds > 0 ? seconds : 15;
    st->click_hwnd = click_hwnd;
    st->click_msg = click_msg;
    snprintf(st->title, sizeof(st->title), "%s", title);
    snprintf(st->text, sizeof(st->text), "%s", text);
    st->title_font = notice_font(dpi, 1);
    st->body_font = notice_font(dpi, 0);

    /* Height from the text, so a longer notice is not clipped and a short one
     * is not a mostly-empty box. */
    dc = GetDC(NULL);
    h = pad * 2;
    if (dc) {
        TEXTMETRICA tm;
        old = (HFONT)SelectObject(dc, st->title_font);
        GetTextMetricsA(dc, &tm);
        h += tm.tmHeight + notice_scale(4, dpi);
        SelectObject(dc, st->body_font);
        measure.left = 0; measure.top = 0;
        measure.right = w - pad * 2; measure.bottom = 0;
        DrawTextA(dc, st->text, -1, &measure, DT_CALCRECT | DT_WORDBREAK | DT_LEFT);
        h += measure.bottom - measure.top;
        SelectObject(dc, old);
        ReleaseDC(NULL, dc);
    } else {
        h += notice_scale(70, dpi);
    }

    /* Only one at a time: a second notice replaces the first rather than
     * stacking, because they are about one thing whose state has just moved. */
    if (notice_hwnd && IsWindow(notice_hwnd))
        notice_close(notice_hwnd);

    /* The title goes in the window NAME as well as being painted. A WS_POPUP
     * with no caption never shows it, so nothing changes on screen - but it
     * makes a notice identifiable from outside the process, which is the only
     * way a test harness can tell WHICH notice appeared (the visible title is
     * drawn in WM_PAINT, and pixels are not readable). */
    hwnd = CreateWindowExA(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                           NOTICE_CLASS, title, WS_POPUP,
                           mi.rcWork.right - w - notice_scale(NOTICE_MARGIN, dpi),
                           mi.rcWork.bottom - h - notice_scale(NOTICE_MARGIN, dpi),
                           w, h, NULL, NULL, GetModuleHandle(NULL), NULL);
    if (!hwnd) {
        if (st->title_font) DeleteObject(st->title_font);
        if (st->body_font)  DeleteObject(st->body_font);
        free(st);
        return;
    }
    SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)st);
    notice_hwnd = hwnd;
    /* SHOWNOACTIVATE, and nothing that focuses it afterwards. */
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);
    SetTimer(hwnd, NOTICE_TIMER, (UINT)(st->seconds * 1000), NULL);
}
