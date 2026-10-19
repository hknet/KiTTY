/*
 * kitty_notice.c: a small notification window of our own, near the clock.
 *
 * Why not the standard tray balloon, which the clipboard notices use: Windows
 * has ignored the requested balloon duration since Vista - the shell decides,
 * driven by accessibility settings - and a standard balloon cannot be coloured.
 * Workplace proxy mode needs both: the notice carries the mode's colour so the
 * notice and the coloured window frame read as one thing, and it stays up long
 * enough to be read.
 *
 * Owning a window means owning it properly, and these are the parts that are
 * easy to get wrong:
 *
 * - It must NEVER take the focus. A notification that steals focus while
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
#define NOTICE_LINK_PROP "KiTTY.noticelink"   /* the link's centre, see below */
#define NOTICE_BTN_PROP  "KiTTY.noticebtn%d"  /* each button's centre, 1-3 */

struct notice_state {
    char title[128];
    char *text;               /* malloc'd: a note can be much longer than a line */
    COLORREF accent;
    /* What this notice was holding, released when it goes for good. `clicked`
     * records whether the user dismissed it or it simply ran out. */
    void (*on_close)(void *, int clicked);
    void *close_ctx;
    HFONT title_font;
    HFONT body_font;
    UINT dpi;
    HWND click_hwnd;          /* notified when the notice is clicked */
    UINT click_msg;           /* 0 = clicking only dismisses */
    /* Hover holds the notice open: its time can run out while somebody is
     * reading it, or while a screen reader is speaking it, and a notice that
     * disappears mid-sentence has failed at the only thing it does. */
    int expired;              /* the time ran out while the pointer was on it */
    int tracking;             /* TrackMouseEvent is armed for WM_MOUSELEAVE */
    int seconds;              /* its full display time */
    /* kitty_notice_show_x: up to three buttons and the link line, NULL when
     * none. The rectangles are set by WM_PAINT, in client coordinates. */
    char *action[3];
    int nbtn;
    char *link_lead;
    char *link;
    RECT action_rc[3], link_rc;
};

static void notice_state_free(struct notice_state *st)
{
    if (st->title_font) DeleteObject(st->title_font);
    if (st->body_font)  DeleteObject(st->body_font);
    free(st->text);
    free(st->action[0]);
    free(st->action[1]);
    free(st->action[2]);
    free(st->link_lead);
    free(st->link);
    free(st);
}

/* The button's padding, and the gaps above the button and the link line,
 * at 96 dpi. */
#define NOTICE_BTN_PADX  12
#define NOTICE_BTN_PADY   5
#define NOTICE_GAP       10
#define NOTICE_BTN_SPACE  8   /* between two buttons on a row */
#define NOTICE_BTN_ROWGAP 6   /* between two rows of buttons */

static int notice_scale(int v, UINT dpi);

/* The buttons laid out left to right in the width `avail`, a button that does
 * not fit starting a new row: the row each one is on and its width. Returns
 * the number of rows. WM_PAINT and the height measured before the window
 * exists both call this, so the two cannot disagree. */
static int notice_button_rows(HDC dc, const struct notice_state *st, int avail,
                              int rowof[3], int width[3])
{
    int padx = notice_scale(NOTICE_BTN_PADX, st->dpi);
    int sp = notice_scale(NOTICE_BTN_SPACE, st->dpi);
    int i, rows = 0, x = 0;
    HFONT old = (HFONT)SelectObject(dc, st->title_font);
    for (i = 0; i < st->nbtn; i++) {
        SIZE s;
        GetTextExtentPoint32A(dc, st->action[i], (int)strlen(st->action[i]), &s);
        width[i] = s.cx + padx * 2;
        if (rows == 0 || x + sp + width[i] > avail) {
            rows++;
            x = width[i];
        } else {
            x += sp + width[i];
        }
        rowof[i] = rows - 1;
    }
    SelectObject(dc, old);
    return rows;
}

/* Grace period after the pointer leaves an already-expired notice, so a hand
 * that slips off it does not take the notice with it. */
#define NOTICE_GRACE_MS 2000

static HWND notice_hwnd = NULL;

/*
 * The parked sticky notice.
 *
 * A sticky notice (KITTY_NOTICE_STICKY - the application notification) stays
 * up until it is clicked, but the same process goes on to raise ordinary
 * notices: an update is available, the old session list is showing, the agent
 * is not verified. Only one notice is on screen at a time, so one of those
 * would have ended the sticky one for good and the note would be lost without
 * ever having been read.
 *
 * So a sticky notice that is REPLACED is parked here rather than closed: it
 * keeps whatever it was holding (its on_close is not run, which is what keeps
 * the application notification's desktop mutex held), and the notice that
 * displaced it puts it back on screen when IT goes away. One deep: a second
 * sticky notice supersedes the parked one outright.
 */
struct notice_parked {
    int valid;
    char title[128];
    char *text;
    COLORREF accent;
    void (*on_close)(void *, int clicked);
    void *close_ctx;
    HWND click_hwnd;
    UINT click_msg;
};
static struct notice_parked notice_park;

static void notice_park_drop(int clicked)
{
    if (!notice_park.valid)
        return;
    notice_park.valid = 0;
    if (notice_park.text) { free(notice_park.text); notice_park.text = NULL; }
    if (notice_park.on_close)
        notice_park.on_close(notice_park.close_ctx, clicked);
    notice_park.on_close = NULL;
}

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
 * WARNING: Read directly rather than inferred from WM_MOUSEMOVE, because a pointer
 * that is already resting on the notice and does not move sends no messages at
 * all - so the first version closed the notice under a pointer that had been
 * parked on it for the whole fifteen seconds. Movement shows
 * a hover STARTED; only the cursor position shows one is happening.
 */
static int notice_pointer_is_over(HWND hwnd)
{
    POINT pt;
    RECT rc;
    if (!GetCursorPos(&pt) || !GetWindowRect(hwnd, &rc))
        return 0;
    return PtInRect(&rc, pt) ? 1 : 0;
}

static void notice_close_reason(HWND hwnd, int clicked)
{
    struct notice_state *st =
        (struct notice_state *)GetWindowLongPtr(hwnd, GWLP_USERDATA);
    KillTimer(hwnd, NOTICE_TIMER);
    if (st) {
        /* Before anything is freed, and before the replacing notice (if that
         * is what is happening) takes the screen: whatever this one was
         * holding is released here, once. */
        void (*on_close)(void *, int) = st->on_close;
        void *ctx = st->close_ctx;
        st->on_close = NULL;
        notice_state_free(st);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, 0);
        if (on_close)
            on_close(ctx, clicked);
    }
    if (notice_hwnd == hwnd)
        notice_hwnd = NULL;
    DestroyWindow(hwnd);
}

static void notice_close(HWND hwnd)
{
    notice_close_reason(hwnd, 0);
}

/* Put the parked sticky notice back on screen. Called only from the paths
 * where a notice goes away on its own (clicked, or timed out) - never from
 * the replacement path, which runs inside kitty_notice_show_ex and would
 * otherwise recurse into itself. */
static void notice_restore_parked(void)
{
    char title[128];
    char *text;
    COLORREF accent;
    void (*on_close)(void *, int);
    void *ctx;
    HWND click_hwnd;
    UINT click_msg;

    if (!notice_park.valid)
        return;
    notice_park.valid = 0;
    snprintf(title, sizeof(title), "%s", notice_park.title);
    text = notice_park.text;        notice_park.text = NULL;
    accent = notice_park.accent;
    on_close = notice_park.on_close; notice_park.on_close = NULL;
    ctx = notice_park.close_ctx;
    click_hwnd = notice_park.click_hwnd;
    click_msg = notice_park.click_msg;
    if (text) {
        kitty_notice_show_ex(title, text, accent, KITTY_NOTICE_STICKY,
                             click_hwnd, click_msg, on_close, ctx);
        free(text);
    } else if (on_close) {
        on_close(ctx, 0);
    }
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
            /* This notice has had its time; if it displaced a sticky one,
             * that one goes back up now. */
            notice_restore_parked();
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
      case WM_SETCURSOR:
        /* The hand over what is clickable on its own: the button and the link. */
        if (st && (st->nbtn || st->link)) {
            POINT pt;
            int i, on = 0;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            for (i = 0; i < st->nbtn; i++)
                if (PtInRect(&st->action_rc[i], pt))
                    on = 1;
            if (on || (st->link && PtInRect(&st->link_rc, pt))) {
                SetCursor(LoadCursor(NULL, IDC_HAND));
                return TRUE;
            }
        }
        break;
      case WM_LBUTTONUP: {
        /* A click does whatever the notice was given to do, and dismisses it
         * either way - a notice you cannot get rid of is a nuisance. The link
         * is the one exception to "whatever it was given": it is its own
         * action, and the owner hears of it through on_close. */
        /* With two or three buttons, each one is its own answer and the
         * rest of the notice only closes it - an answer by accident would be
         * worse than none; closed so, it counts as a notice that ran out. */
        POINT pt;
        int on_link, b = -1, i, reason;
        pt.x = (short)LOWORD(lp);
        pt.y = (short)HIWORD(lp);
        on_link = st && st->link && PtInRect(&st->link_rc, pt);
        for (i = 0; st && i < st->nbtn; i++)
            if (PtInRect(&st->action_rc[i], pt))
                b = i;
        if (on_link)
            reason = KITTY_NOTICE_LINK;
        else if (st && st->nbtn >= 2)
            reason = b == 0 ? 1 : b == 1 ? KITTY_NOTICE_BUTTON2 :
                     b == 2 ? KITTY_NOTICE_BUTTON3 : 0;
        else
            reason = 1;
        if (reason == 1 && st && st->click_hwnd && st->click_msg)
            PostMessage(st->click_hwnd, st->click_msg, 0, 0);
        notice_close_reason(hwnd, reason);
        /* A click dismisses THIS notice. A sticky one it had displaced has
         * still not been read, so it comes back. */
        notice_restore_parked();
        return 0;
      }
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
            /* The extras sit at the bottom, laid out upwards from it, so a text
             * cut at the work area's height clips itself and never them. */
            if (st->nbtn || st->link) {
                TEXTMETRICA tm;
                int y = rc.bottom - pad;
                int gap = notice_scale(NOTICE_GAP, st->dpi);
                int lh;
                GetTextMetricsA(dc, &tm);
                lh = tm.tmHeight;
                if (st->link) {
                    SIZE ls, ks;
                    int x = tr.left;
                    int ly = y - lh;
                    COLORREF sep = RGB((GetRValue(bg) * 2 + GetRValue(fg)) / 3,
                                       (GetGValue(bg) * 2 + GetGValue(fg)) / 3,
                                       (GetBValue(bg) * 2 + GetBValue(fg)) / 3);
                    HPEN pen = CreatePen(PS_SOLID, notice_scale(1, st->dpi), sep);
                    HPEN oldpen = (HPEN)SelectObject(dc, pen);
                    MoveToEx(dc, tr.left, ly - gap / 2, NULL);
                    LineTo(dc, tr.right, ly - gap / 2);
                    SelectObject(dc, oldpen);
                    DeleteObject(pen);
                    if (st->link_lead && st->link_lead[0]) {
                        GetTextExtentPoint32A(dc, st->link_lead,
                                              (int)strlen(st->link_lead), &ls);
                        TextOutA(dc, x, ly, st->link_lead, (int)strlen(st->link_lead));
                        x += ls.cx + notice_scale(5, st->dpi);
                    }
                    SelectObject(dc, st->title_font);
                    GetTextExtentPoint32A(dc, st->link, (int)strlen(st->link), &ks);
                    TextOutA(dc, x, ly, st->link, (int)strlen(st->link));
                    {
                        /* Underlined by hand: one font fewer to own. */
                        HPEN up = CreatePen(PS_SOLID, notice_scale(1, st->dpi), fg);
                        HPEN oldp = (HPEN)SelectObject(dc, up);
                        MoveToEx(dc, x, ly + ks.cy, NULL);
                        LineTo(dc, x + ks.cx, ly + ks.cy);
                        SelectObject(dc, oldp);
                        DeleteObject(up);
                    }
                    SelectObject(dc, st->body_font);
                    SetRect(&st->link_rc, x - 2, ly - 2, x + ks.cx + 2, ly + ks.cy + 3);
                    SetPropA(hwnd, NOTICE_LINK_PROP,
                             (HANDLE)(ULONG_PTR)MAKELONG((st->link_rc.left + st->link_rc.right) / 2,
                                                         (st->link_rc.top + st->link_rc.bottom) / 2));
                    y = ly - gap;
                }
                if (st->nbtn) {
                    /* The first button filled, as the one a notice with a
                     * single button has always had; the others outlined. */
                    int rowof[3], width[3], i, rows, bh, top, x = tr.left, row = 0;
                    int padx = notice_scale(NOTICE_BTN_PADX, st->dpi);
                    int pady = notice_scale(NOTICE_BTN_PADY, st->dpi);
                    int sp = notice_scale(NOTICE_BTN_SPACE, st->dpi);
                    int rgap = notice_scale(NOTICE_BTN_ROWGAP, st->dpi);
                    int r = notice_scale(6, st->dpi);
                    TEXTMETRICA tb;
                    rows = notice_button_rows(dc, st, tr.right - tr.left, rowof, width);
                    SelectObject(dc, st->title_font);
                    GetTextMetricsA(dc, &tb);
                    bh = tb.tmHeight + pady * 2;
                    top = y - (rows * bh + (rows - 1) * rgap);
                    for (i = 0; i < st->nbtn; i++) {
                        RECT b;
                        int by;
                        HBRUSH fill = NULL, oldb;
                        HPEN edge, oldp;
                        if (rowof[i] != row) {
                            row = rowof[i];
                            x = tr.left;
                        }
                        by = top + row * (bh + rgap);
                        SetRect(&b, x, by, x + width[i], by + bh);
                        if (i == 0)
                            fill = CreateSolidBrush(hc ? GetSysColor(COLOR_BTNFACE)
                                                       : RGB(255, 255, 255));
                        edge = CreatePen(PS_SOLID, notice_scale(1, st->dpi),
                                         hc ? GetSysColor(COLOR_BTNTEXT) : fg);
                        oldb = (HBRUSH)SelectObject(dc, fill ? (HGDIOBJ)fill
                                                             : GetStockObject(NULL_BRUSH));
                        oldp = (HPEN)SelectObject(dc, edge);
                        RoundRect(dc, b.left, b.top, b.right, b.bottom, r, r);
                        SetTextColor(dc, i == 0 ? (hc ? GetSysColor(COLOR_BTNTEXT) : st->accent)
                                                : fg);
                        TextOutA(dc, b.left + padx, b.top + pady, st->action[i],
                                 (int)strlen(st->action[i]));
                        SetTextColor(dc, fg);
                        SelectObject(dc, oldb);
                        SelectObject(dc, oldp);
                        if (fill)
                            DeleteObject(fill);
                        DeleteObject(edge);
                        st->action_rc[i] = b;
                        x = b.right + sp;
                        {
                            char prop[32];
                            snprintf(prop, sizeof(prop), NOTICE_BTN_PROP, i + 1);
                            SetPropA(hwnd, prop,
                                     (HANDLE)(ULONG_PTR)MAKELONG((b.left + b.right) / 2,
                                                                 (b.top + b.bottom) / 2));
                        }
                    }
                    SelectObject(dc, st->body_font);
                    y = top - gap;
                }
                tr.bottom = y + gap - notice_scale(4, st->dpi);
            }
            DrawTextA(dc, st->text ? st->text : "", -1, &tr,
                      DT_LEFT | DT_TOP | DT_WORDBREAK);
            SelectObject(dc, old);
        }
        EndPaint(hwnd, &ps);
        return 0;
      }
      case WM_DESTROY:
        RemovePropA(hwnd, NOTICE_LINK_PROP);
        {
            int i;
            for (i = 1; i <= 3; i++) {
                char prop[32];
                snprintf(prop, sizeof(prop), NOTICE_BTN_PROP, i);
                RemovePropA(hwnd, prop);
            }
        }
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
    kitty_notice_show_ex(title, text, accent, seconds, click_hwnd, click_msg,
                         NULL, NULL);
}

void kitty_notice_show_ex(const char *title, const char *text, COLORREF accent,
                          int seconds, HWND click_hwnd, unsigned int click_msg,
                          void (*on_close)(void *ctx, int clicked), void *ctx)
{
    kitty_notice_show_x(title, text, accent, seconds, click_hwnd, click_msg,
                        on_close, ctx, NULL);
}

void kitty_notice_show_x(const char *title, const char *text, COLORREF accent,
                         int seconds, HWND click_hwnd, unsigned int click_msg,
                         void (*on_close)(void *ctx, int clicked), void *ctx,
                         const KittyNoticeExtras *x)
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

    /* Every early return below releases what the caller was holding for the
     * notice (on_close, not clicked), exactly as a failed CreateWindowEx
     * does: a caller that tracks its notice must never be left believing
     * one is on screen. */
    if (!title || !text) {
        if (on_close)
            on_close(ctx, 0);
        return;
    }
    notice_register_class();

    /* Beside the TRAY's monitor, not the primary one: on a two-monitor desk the
     * taskbar is often not on the primary display, and a notice about the tray
     * icon that appears on the other screen is a notice nobody sees. */
    tray = FindWindowA("Shell_TrayWnd", NULL);
    mon = MonitorFromWindow(tray ? tray : GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
    memset(&mi, 0, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoA(mon, &mi)) {
        if (on_close)
            on_close(ctx, 0);
        return;
    }
    dpi = notice_dpi_for_monitor(mon);
    pad = notice_scale(NOTICE_PAD, dpi);
    w = notice_scale(NOTICE_WIDTH, dpi);

    st = (struct notice_state *)calloc(1, sizeof(*st));
    if (!st) {
        if (on_close)
            on_close(ctx, 0);
        return;
    }
    st->accent = accent;
    st->dpi = dpi;
    /* 0 = no timer at all (KITTY_NOTICE_STICKY); every other non-positive
     * value keeps the historical default of fifteen seconds. */
    st->seconds = seconds > 0 ? seconds :
                  (seconds == KITTY_NOTICE_STICKY ? 0 : 15);
    st->click_hwnd = click_hwnd;
    st->click_msg = click_msg;
    st->on_close = on_close;
    st->close_ctx = ctx;
    snprintf(st->title, sizeof(st->title), "%s", title);
    st->text = strdup(text);
    if (!st->text) {
        free(st);
        if (on_close)
            on_close(ctx, 0);
        return;
    }
    if (x) {
        const char *a[3];
        int i;
        a[0] = x->action; a[1] = x->action2; a[2] = x->action3;
        for (i = 0; i < 3; i++)
            if (a[i] && a[i][0])
                st->action[st->nbtn++] = strdup(a[i]);
    }
    if (x && x->link && x->link[0]) {
        st->link = strdup(x->link);
        st->link_lead = strdup(x->link_lead ? x->link_lead : "");
    }
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
        /* The extras, as WM_PAINT lays them out: the button is a line of the
         * title font with its padding, the link line one of the body font. */
        if (st->link) {
            GetTextMetricsA(dc, &tm);
            h += notice_scale(NOTICE_GAP, dpi) + tm.tmHeight;
        }
        if (st->nbtn) {
            int rowof[3], width[3], rows;
            rows = notice_button_rows(dc, st, w - pad * 2, rowof, width);
            SelectObject(dc, st->title_font);
            GetTextMetricsA(dc, &tm);
            h += notice_scale(NOTICE_GAP, dpi) +
                 rows * (tm.tmHeight + notice_scale(NOTICE_BTN_PADY, dpi) * 2) +
                 (rows - 1) * notice_scale(NOTICE_BTN_ROWGAP, dpi);
        }
        SelectObject(dc, old);
        ReleaseDC(NULL, dc);
    } else {
        h += notice_scale(70, dpi);
    }
    /* A long text must not grow the notice off the screen: cap it at the work
     * area (less the margin at each end) and let DT_WORDBREAK clip the rest. */
    {
        int maxh = (mi.rcWork.bottom - mi.rcWork.top) -
                   notice_scale(NOTICE_MARGIN, dpi) * 2;
        if (maxh > 0 && h > maxh)
            h = maxh;
    }

    /* Only one at a time: a second notice replaces the first rather than
     * stacking, because they are about one thing whose state has just moved.
     *
     * Unless the first one is STICKY and this one is not - then it is parked
     * instead of closed, and goes back up when this one is gone. Its on_close
     * is deliberately NOT run: a parked notice still holds what it held (the
     * application notification keeps the desktop slot, so no other process
     * shows the same note meanwhile). */
    if (notice_hwnd && IsWindow(notice_hwnd)) {
        struct notice_state *old_st =
            (struct notice_state *)GetWindowLongPtr(notice_hwnd, GWLP_USERDATA);
        if (old_st && old_st->seconds == 0 && seconds != KITTY_NOTICE_STICKY) {
            /* A second sticky would have nowhere to go: one park, and the
             * newer sticky is the one that matters. */
            notice_park_drop(0);
            snprintf(notice_park.title, sizeof(notice_park.title), "%s",
                     old_st->title);
            notice_park.text = old_st->text;    /* taken over, not copied */
            old_st->text = NULL;
            notice_park.accent = old_st->accent;
            notice_park.on_close = old_st->on_close;
            notice_park.close_ctx = old_st->close_ctx;
            notice_park.click_hwnd = old_st->click_hwnd;
            notice_park.click_msg = old_st->click_msg;
            old_st->on_close = NULL;            /* it is parked, not finished */
            notice_park.valid = 1;
        } else if (seconds == KITTY_NOTICE_STICKY) {
            notice_park_drop(0);
        }
        notice_close(notice_hwnd);
    }

    /* The title goes in the window NAME as well as being painted. A WS_POPUP
     * with no caption never shows it, so nothing changes on screen - but it
     * makes a notice identifiable from outside the process, which is the only
     * way a test harness can see WHICH notice appeared (the visible title is
     * drawn in WM_PAINT, and pixels are not readable). */
    hwnd = CreateWindowExA(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                           NOTICE_CLASS, title, WS_POPUP,
                           mi.rcWork.right - w - notice_scale(NOTICE_MARGIN, dpi),
                           mi.rcWork.bottom - h - notice_scale(NOTICE_MARGIN, dpi),
                           w, h, NULL, NULL, GetModuleHandle(NULL), NULL);
    if (!hwnd) {
        notice_state_free(st);
        /* Nothing is on screen, so anything the caller was holding for the
         * notice is released now rather than never. */
        if (on_close)
            on_close(ctx, 0);
        return;
    }
    SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)st);
    notice_hwnd = hwnd;
    /* SHOWNOACTIVATE, and nothing that focuses it afterwards. */
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);
    /* UpdateWindow painted it, which laid out the link and the buttons and
     * put their centres, in client coordinates, on the window as properties
     * (WM_PAINT does it at every paint, so they follow a new layout) - for
     * the reason the title goes in the window name: a harness can click
     * them. */
    /* A sticky notice sets no timer: it goes when it is clicked, when a later
     * notice replaces it, or when the process ends. */
    if (st->seconds > 0)
        SetTimer(hwnd, NOTICE_TIMER, (UINT)(st->seconds * 1000), NULL);
}

/* A caller that tracks its own notice by its on_close context (notifications
 * from the host) checks whether it is the one on screen, and closes it. Only
 * the notice ON SCREEN counts: a parked one is always sticky, and nothing
 * that tracks its notice this way raises sticky ones. */
static struct notice_state *notice_current_state(void)
{
    if (!notice_hwnd || !IsWindow(notice_hwnd))
        return NULL;
    return (struct notice_state *)GetWindowLongPtr(notice_hwnd, GWLP_USERDATA);
}

int kitty_notice_showing(void *ctx)
{
    struct notice_state *st = notice_current_state();
    return ctx && st && st->on_close && st->close_ctx == ctx;
}

void kitty_notice_close_ctx(void *ctx)
{
    if (!kitty_notice_showing(ctx))
        return;
    notice_close(notice_hwnd);
    /* As when it runs out: a sticky notice it displaced comes back. */
    notice_restore_parked();
}
