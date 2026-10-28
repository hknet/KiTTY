/*
 * kitty_commentview.c - the Session panel's comment box, drawn by the
 * program (kitty_commentview.h): the comment wrapped to the box, its links
 * underlined in the theme's link colour and opened with a click, a scroll
 * bar when it is longer than the box, a right-click menu to copy a link or
 * the comment.
 *
 * What a link is and where a line wraps is kitty_commentlinks.c (no
 * window, unit-tested); this file measures, paints and takes the input.
 *
 * The text arrives by WM_SETTEXT (dlg_editbox_set) in the program's ANSI
 * code page, as the comment is stored; the class is a Unicode one, so
 * Windows hands it over as UTF-16 and WM_GETTEXT answers either kind.
 */
#include "putty.h"            /* first: it brings winsock2.h before windows.h */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "kitty_commentview.h"
#include "kitty_commentlinks.h"
#include "kitty_theme.h"
#include "kitty_dlgbox.h"      /* kitty_info_modeless: a failed open or copy */
#include "kitty_text.h"
#include "kitty_winutil.h"     /* SetTextToClipboardW */
#include "kitty_winerr.h"      /* kitty_win_reason */
#include "url/urlhack.h"       /* urlhack_open_url: the terminal's open call */
#include "kitty_buildlabel.h"  /* KITTY_TEST_BUILD_LABEL: the open trace */

/* windowsx.h's two, without the rest of its macros */
#ifndef GET_X_LPARAM
#define GET_X_LPARAM(lp) ((int)(short)LOWORD(lp))
#define GET_Y_LPARAM(lp) ((int)(short)HIWORD(lp))
#endif

#define KCV_MENU_COPY_LINK 1
#define KCV_MENU_COPY_ALL  2

typedef struct {
    HWND hwnd;
    HFONT font;           /* the dialog's, from WM_SETFONT */
    HFONT ufont;          /* the same underlined, ours */
    wchar_t *raw;         /* the text as set (Copy comment) */
    wchar_t *text;        /* the same, tabs and control characters as blanks */
    int n;
    int hint;             /* a placeholder: hint ink, no links */
    kcl_span *links;
    int nlinks;
    kcl_layout lay;
    int lineh, padx, pady;
    int top;              /* the first line shown */
    int visible;          /* lines that fit */
    int wheel;            /* wheel delta not yet turned into lines */
    int down_link;        /* the link a left button went down on, -1 */
    int in_layout;
} kcv;

static kcv *kcv_of(HWND hwnd)
{
    return (kcv *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
}

/* ---- text and layout --------------------------------------------------- */

static int kcv_width(void *ctx, const wchar_t *s, int n)
{
    SIZE sz;
    if (n <= 0 || !GetTextExtentPoint32W((HDC)ctx, s, n, &sz))
        return 0;
    return sz.cx;
}

/* Is the text one of the two placeholders update_comment_display() puts
 * there? Compared in the form it arrived in. */
static int kcv_is_placeholder(const wchar_t *t)
{
    static const char *const ph[] = { KT_CFG_COMMENT_SELECT, KT_CFG_COMMENT_NONE };
    size_t i;
    for (i = 0; i < sizeof(ph) / sizeof(ph[0]); i++) {
        wchar_t w[128];
        if (MultiByteToWideChar(CP_ACP, 0, ph[i], -1, w, 128) > 0 && !wcscmp(t, w))
            return 1;
    }
    return 0;
}

static void kcv_make_ufont(kcv *v)
{
    LOGFONTW lf;
    if (v->ufont)
        DeleteObject(v->ufont);
    v->ufont = NULL;
    if (v->font && GetObjectW(v->font, sizeof(lf), &lf)) {
        lf.lfUnderline = TRUE;
        v->ufont = CreateFontIndirectW(&lf);
    }
}

static HFONT kcv_font(kcv *v)
{
    return v->font ? v->font : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
}

static void kcv_set_scroll(kcv *v)
{
    SCROLLINFO si;
    int maxtop = v->lay.nlines - v->visible;
    if (maxtop < 0)
        maxtop = 0;
    if (v->top > maxtop)
        v->top = maxtop;
    if (v->top < 0)
        v->top = 0;
    if (v->lay.nlines <= v->visible)
        return;                         /* no bar shown */
    memset(&si, 0, sizeof(si));
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = v->lay.nlines - 1;
    si.nPage = (UINT)v->visible;
    si.nPos = v->top;
    SetScrollInfo(v->hwnd, SB_VERT, &si, TRUE);
}

/*
 * Wrap the text to the box. Measured without the scroll bar first; a text
 * that does not fit gets the bar and is wrapped again to the narrower
 * width (narrower never needs fewer lines, so it never comes off again).
 */
static void kcv_relayout(kcv *v)
{
    HDC dc;
    HGDIOBJ old;
    TEXTMETRICW tm;
    RECT wr, cr;
    int full, h;

    kcl_layout_free(&v->lay);
    dc = GetDC(v->hwnd);
    if (!dc)
        return;
    old = SelectObject(dc, kcv_font(v));
    GetTextMetricsW(dc, &tm);
    v->lineh = tm.tmHeight > 0 ? tm.tmHeight : 1;
    v->padx = tm.tmAveCharWidth / 2 > 2 ? tm.tmAveCharWidth / 2 : 2;
    v->pady = 1;
    GetWindowRect(v->hwnd, &wr);
    GetClientRect(v->hwnd, &cr);
    h = cr.bottom - 2 * v->pady;
    v->visible = h / v->lineh > 0 ? h / v->lineh : 1;
    full = (wr.right - wr.left) - 2 * GetSystemMetrics(SM_CXBORDER) - 2 * v->padx;

    kcl_layout_build(&v->lay, v->text, v->n, v->hint ? NULL : v->links,
                     v->hint ? 0 : v->nlinks, full, kcv_width, dc);
    v->in_layout++;
    if (v->lay.nlines > v->visible) {
        ShowScrollBar(v->hwnd, SB_VERT, TRUE);
        GetClientRect(v->hwnd, &cr);
        kcl_layout_free(&v->lay);
        kcl_layout_build(&v->lay, v->text, v->n, v->hint ? NULL : v->links,
                         v->hint ? 0 : v->nlinks, cr.right - 2 * v->padx,
                         kcv_width, dc);
    } else {
        ShowScrollBar(v->hwnd, SB_VERT, FALSE);
    }
    v->in_layout--;
    SelectObject(dc, old);
    ReleaseDC(v->hwnd, dc);
    kcv_set_scroll(v);
}

/* The text from the window (after DefWindowProc stored it). A different
 * text - another session - starts at the top again; the same one (a
 * refresh) keeps its place. */
static void kcv_take_text(kcv *v)
{
    int len = GetWindowTextLengthW(v->hwnd), i, same;
    wchar_t *raw = malloc(((size_t)len + 1) * sizeof(wchar_t));
    wchar_t *text = malloc(((size_t)len + 1) * sizeof(wchar_t));
    if (!raw || !text) {
        free(raw);
        free(text);
        return;
    }
    len = GetWindowTextW(v->hwnd, raw, len + 1);
    if (len < 0)
        len = 0;
    raw[len] = 0;
    for (i = 0; i <= len; i++) {
        wchar_t c = raw[i];
        text[i] = (c && c < 0x20 && c != '\r' && c != '\n') ? ' ' : c;
    }
    same = v->raw && !wcscmp(v->raw, raw);
    free(v->raw);
    free(v->text);
    free(v->links);
    v->raw = raw;
    v->text = text;
    v->n = len;
    v->links = NULL;
    v->nlinks = 0;
    v->hint = kcv_is_placeholder(raw);
    if (!v->hint)
        v->nlinks = kcl_find_links(text, len, &v->links);
    if (!same)
        v->top = 0;
    v->down_link = -1;
}

/* ---- hit-testing ------------------------------------------------------- */

static int kcv_hit(kcv *v, int x, int y)
{
    int line, i;
    if (v->hint || y < v->pady || v->lineh <= 0)
        return -1;
    line = v->top + (y - v->pady) / v->lineh;
    for (i = 0; i < v->lay.nruns; i++) {
        const kcl_run *r = &v->lay.runs[i];
        if (r->line == line && r->link >= 0 &&
            x >= v->padx + r->x && x < v->padx + r->x + r->w)
            return r->link;
    }
    return -1;
}

/* The middle of the link's first piece on screen; false when none is. */
static bool kcv_link_point(kcv *v, int link, POINT *pt)
{
    int i;
    for (i = 0; i < v->lay.nruns; i++) {
        const kcl_run *r = &v->lay.runs[i];
        if (r->link == link && r->line >= v->top && r->line < v->top + v->visible) {
            pt->x = v->padx + r->x + r->w / 2;
            pt->y = v->pady + (r->line - v->top) * v->lineh + v->lineh / 2;
            return true;
        }
    }
    return false;
}

/* ---- what the box does ------------------------------------------------- */

static void kcv_report(kcv *v, const char *msg)
{
    /* Modeless: the mid-session Change Settings box has a live session
     * behind it, which a modal box would stop. */
    kitty_info_modeless(GetAncestor(v->hwnd, GA_ROOT), KT_CAP_ERROR, msg, NULL, NULL);
}

static char *kcv_to_mb(UINT cp, const wchar_t *w)
{
    int len = WideCharToMultiByte(cp, 0, w, -1, NULL, 0, NULL, NULL);
    char *s;
    if (len <= 0)
        return NULL;
    s = malloc((size_t)len);
    if (s && WideCharToMultiByte(cp, 0, w, -1, s, len, NULL, NULL) <= 0) {
        free(s);
        s = NULL;
    }
    return s;
}

#ifdef KITTY_TEST_BUILD_LABEL
/*
 * KITTY_COMMENTVIEW_TRACE=<file> (test builds only): a link is not opened
 * but written there, one line "open <target>" in UTF-8, for the harness -
 * no browser starts in the gate.
 */
static bool kcv_trace_open(const wchar_t *target)
{
    const char *path = getenv("KITTY_COMMENTVIEW_TRACE");
    char *u8;
    FILE *f;
    if (!path || !*path)
        return false;
    u8 = kcv_to_mb(CP_UTF8, target);
    f = fopen(path, "a");
    if (f) {
        fprintf(f, "open %s\n", u8 ? u8 : "");
        fclose(f);
    }
    free(u8);
    return true;
}
#endif

/* Open a link with the program Windows has registered for it, through the
 * terminal's own call (url/urlhack.c). No question first. */
static void kcv_open(kcv *v, int link)
{
    wchar_t *target;
    char *a;
    DWORD err;
    if (v->hint || link < 0 || link >= v->nlinks)
        return;
    target = kcl_link_target(v->text, &v->links[link]);
    if (!target)
        return;
#ifdef KITTY_TEST_BUILD_LABEL
    if (kcv_trace_open(target)) {
        free(target);
        return;
    }
#endif
    /* The comment came in the ANSI code page, so it goes back to it whole. */
    a = kcv_to_mb(CP_ACP, target);
    free(target);
    if (!a)
        return;
    err = urlhack_open_url(NULL, a);
    if (err != 0 && err != ERROR_CANCELLED) {
        char *msg = dupprintf(KT_URL_OPEN_FAILED_FMT, a, kitty_win_reason(err));
        kcv_report(v, msg);
        sfree(msg);
    }
    free(a);
}

/* "Copy link" (link >= 0) or "Copy comment" (-1): 1 copied, 0 not. */
static int kcv_copy(kcv *v, int link)
{
    wchar_t *t;
    int ok;
    if (link >= 0) {
        int len;
        if (v->hint || link >= v->nlinks)
            return 0;
        len = v->links[link].end - v->links[link].start;
        t = malloc(((size_t)len + 1) * sizeof(wchar_t));
        if (!t)
            return 0;
        memcpy(t, v->text + v->links[link].start, (size_t)len * sizeof(wchar_t));
        t[len] = 0;
    } else {
        if (v->hint || !v->raw || !v->n)
            return 0;
        t = malloc(((size_t)v->n + 1) * sizeof(wchar_t));
        if (!t)
            return 0;
        memcpy(t, v->raw, ((size_t)v->n + 1) * sizeof(wchar_t));
    }
    ok = SetTextToClipboardW(t);
    if (!ok) {
        char *msg = dupprintf(KT_CFG_COMMENT_COPY_FAILED, kitty_win_reason(GetLastError()));
        kcv_report(v, msg);
        sfree(msg);
    }
    free(t);
    return ok;
}

static void kcv_menu(kcv *v, LPARAM lParam)
{
    POINT pt;
    int link = -1, cmd;
    HMENU m;
    if (lParam == (LPARAM)-1) {
        /* the keyboard (Shift+F10, the menu key): the link Enter opens */
        if (!v->hint && v->nlinks > 0)
            link = 0;
        if (link < 0 || !kcv_link_point(v, link, &pt)) {
            pt.x = v->padx;
            pt.y = v->pady + v->lineh;
        }
        ClientToScreen(v->hwnd, &pt);
    } else {
        POINT c;
        pt.x = GET_X_LPARAM(lParam);
        pt.y = GET_Y_LPARAM(lParam);
        c = pt;
        ScreenToClient(v->hwnd, &c);
        link = kcv_hit(v, c.x, c.y);
    }
    m = CreatePopupMenu();
    if (!m)
        return;
    if (link >= 0)
        AppendMenuA(m, MF_STRING, KCV_MENU_COPY_LINK, KT_CFG_COMMENT_M_COPY_LINK);
    AppendMenuA(m, MF_STRING | ((v->hint || !v->n) ? MF_GRAYED : 0),
                KCV_MENU_COPY_ALL, KT_CFG_COMMENT_M_COPY_ALL);
    cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                              pt.x, pt.y, 0, v->hwnd, NULL);
    DestroyMenu(m);
    if (cmd == KCV_MENU_COPY_LINK)
        kcv_copy(v, link);
    else if (cmd == KCV_MENU_COPY_ALL)
        kcv_copy(v, -1);
}

static void kcv_scroll_to(kcv *v, int top)
{
    int maxtop = v->lay.nlines - v->visible;
    if (maxtop < 0)
        maxtop = 0;
    if (top > maxtop)
        top = maxtop;
    if (top < 0)
        top = 0;
    if (top == v->top)
        return;
    v->top = top;
    kcv_set_scroll(v);
    InvalidateRect(v->hwnd, NULL, FALSE);
}

/* ---- painting ---------------------------------------------------------- */

static void kcv_paint(kcv *v)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(v->hwnd, &ps), mdc;
    HBITMAP bm;
    HGDIOBJ oldbm, oldfont;
    HBRUSH bg;
    RECT cr;
    bool dark = kitty_theme_window_dark(v->hwnd);
    bool enabled = IsWindowEnabled(v->hwnd) != 0;
    COLORREF normal = kitty_theme_ink(dark, KITTY_INK_NORMAL);
    COLORREF hint = kitty_theme_ink(dark, KITTY_INK_HINT);
    COLORREF linkc = kitty_theme_ink(dark, KITTY_INK_LINK);
    int i;

    GetClientRect(v->hwnd, &cr);
    if (cr.right <= 0 || cr.bottom <= 0) {
        EndPaint(v->hwnd, &ps);
        return;
    }
    mdc = CreateCompatibleDC(dc);
    bm = CreateCompatibleBitmap(dc, cr.right, cr.bottom);
    if (!mdc || !bm) {
        if (bm) DeleteObject(bm);
        if (mdc) DeleteDC(mdc);
        EndPaint(v->hwnd, &ps);
        return;
    }
    oldbm = SelectObject(mdc, bm);
    /* the background the read-only EDIT had: the dialog's, in either theme */
    bg = kitty_theme_backbrush(v->hwnd);
    FillRect(mdc, &cr, bg ? bg : GetSysColorBrush(COLOR_BTNFACE));
    SetBkMode(mdc, TRANSPARENT);
    oldfont = SelectObject(mdc, kcv_font(v));

    for (i = 0; i < v->lay.nruns; i++) {
        const kcl_run *r = &v->lay.runs[i];
        int y;
        if (r->line < v->top || r->line > v->top + v->visible)
            continue;
        y = v->pady + (r->line - v->top) * v->lineh;
        if (y >= cr.bottom)
            continue;
        SelectObject(mdc, (r->link >= 0 && v->ufont) ? v->ufont : kcv_font(v));
        SetTextColor(mdc, (!enabled || v->hint) ? hint :
                          r->link >= 0 ? linkc : normal);
        TextOutW(mdc, v->padx + r->x, y, v->text + r->start, r->end - r->start);
    }

    if (GetFocus() == v->hwnd &&
        !(SendMessageW(v->hwnd, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS)) {
        RECT f = cr;
        InflateRect(&f, -1, -1);
        SetTextColor(mdc, normal);
        SetBkColor(mdc, dark ? RGB(0, 0, 0) : RGB(255, 255, 255));
        DrawFocusRect(mdc, &f);
    }

    BitBlt(dc, 0, 0, cr.right, cr.bottom, mdc, 0, 0, SRCCOPY);
    SelectObject(mdc, oldfont);
    SelectObject(mdc, oldbm);
    DeleteObject(bm);
    DeleteDC(mdc);
    EndPaint(v->hwnd, &ps);
}

/* The one-pixel edge (WS_BORDER) in the theme's line colour: Windows would
 * draw it in the frame colour, black on a dark panel. */
static void kcv_paint_border(kcv *v)
{
    HDC dc = GetWindowDC(v->hwnd);
    RECT wr;
    HBRUSH b;
    if (!dc)
        return;
    GetWindowRect(v->hwnd, &wr);
    OffsetRect(&wr, -wr.left, -wr.top);
    b = CreateSolidBrush(kitty_theme_line_colour(kitty_theme_window_dark(v->hwnd)));
    if (b) {
        FrameRect(dc, &wr, b);
        DeleteObject(b);
    }
    ReleaseDC(v->hwnd, dc);
}

static void kcv_notify_parent(HWND hwnd, WORD code)
{
    HWND parent = GetParent(hwnd);
    if (parent)
        SendMessageW(parent, WM_COMMAND,
                     MAKEWPARAM((WORD)GetDlgCtrlID(hwnd), code), (LPARAM)hwnd);
}

static LRESULT CALLBACK kcv_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    kcv *v = kcv_of(hwnd);

    if (msg == WM_NCCREATE) {
        v = calloc(1, sizeof(*v));
        if (!v)
            return FALSE;
        v->hwnd = hwnd;
        v->down_link = -1;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)v);
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    if (!v)
        return DefWindowProcW(hwnd, msg, wParam, lParam);

    if (kitty_theme_is_change_message(msg)) {
        RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_FRAME);
        return 0;
    }

    switch (msg) {
      case WM_NCDESTROY:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        kcl_layout_free(&v->lay);
        if (v->ufont)
            DeleteObject(v->ufont);
        free(v->raw);
        free(v->text);
        free(v->links);
        free(v);
        return DefWindowProcW(hwnd, msg, wParam, lParam);

      case WM_SETFONT:
        v->font = (HFONT)wParam;
        kcv_make_ufont(v);
        kcv_relayout(v);
        if (LOWORD(lParam))
            InvalidateRect(hwnd, NULL, FALSE);
        return 0;

      case WM_GETFONT:
        return (LRESULT)v->font;

      case WM_SETTEXT: {
        LRESULT r = DefWindowProcW(hwnd, msg, wParam, lParam);
        kcv_take_text(v);
        kcv_relayout(v);
        InvalidateRect(hwnd, NULL, FALSE);
        return r;
      }

      case WM_SIZE:
        if (!v->in_layout) {
            kcv_relayout(v);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;

      case WM_ERASEBKGND:
        return 1;

      case WM_PAINT:
        kcv_paint(v);
        return 0;

      case WM_NCPAINT: {
        LRESULT r = DefWindowProcW(hwnd, msg, wParam, lParam);
        kcv_paint_border(v);
        return r;
      }

      case WM_THEMECHANGED:
      case WM_SYSCOLORCHANGE:
      case WM_ENABLE:
        RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_FRAME);
        return DefWindowProcW(hwnd, msg, wParam, lParam);

      case WM_SETFOCUS:
      case WM_KILLFOCUS:
        InvalidateRect(hwnd, NULL, FALSE);
        kcv_notify_parent(hwnd, msg == WM_SETFOCUS ? EN_SETFOCUS : EN_KILLFOCUS);
        return 0;

      case WM_UPDATEUISTATE: {
        LRESULT r = DefWindowProcW(hwnd, msg, wParam, lParam);
        InvalidateRect(hwnd, NULL, FALSE);
        return r;
      }

      case WM_GETDLGCODE: {
        /* Enter opens the first link instead of pressing the dialog's
         * default button - only when there is one to open. */
        LRESULT code = DLGC_WANTARROWS;
        const MSG *m = (const MSG *)lParam;
        if (m && m->message == WM_KEYDOWN && m->wParam == VK_RETURN &&
            !v->hint && v->nlinks > 0)
            code |= DLGC_WANTMESSAGE;
        return code;
      }

      case WM_KEYDOWN: {
        bool ctrl = GetKeyState(VK_CONTROL) < 0;
        switch (wParam) {
          case VK_RETURN: kcv_open(v, 0); return 0;
          case VK_UP:     kcv_scroll_to(v, v->top - 1); return 0;
          case VK_DOWN:   kcv_scroll_to(v, v->top + 1); return 0;
          case VK_PRIOR:  kcv_scroll_to(v, v->top - v->visible); return 0;
          case VK_NEXT:   kcv_scroll_to(v, v->top + v->visible); return 0;
          case VK_HOME:   kcv_scroll_to(v, 0); return 0;
          case VK_END:    kcv_scroll_to(v, v->lay.nlines); return 0;
          case 'C':
          case VK_INSERT:
            if (ctrl) {
                kcv_copy(v, -1);
                return 0;
            }
            break;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
      }

      case WM_CHAR:
        return 0;                       /* read-only: no beep, nothing typed */

      case WM_CONTEXTMENU:
        kcv_menu(v, lParam);
        return 0;

      case WM_LBUTTONDOWN:
        if (GetFocus() != hwnd)
            SetFocus(hwnd);
        v->down_link = kcv_hit(v, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;

      case WM_LBUTTONUP: {
        int link = kcv_hit(v, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        int down = v->down_link;
        v->down_link = -1;
        if (link >= 0 && link == down)
            kcv_open(v, link);
        return 0;
      }

      case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(hwnd, &p);
            SetCursor(LoadCursor(NULL, kcv_hit(v, p.x, p.y) >= 0 ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);

      case WM_MOUSEWHEEL: {
        UINT per = 3;
        int lines;
        if (v->lay.nlines <= v->visible)
            return DefWindowProcW(hwnd, msg, wParam, lParam);   /* the panel scrolls */
        SystemParametersInfo(SPI_GETWHEELSCROLLLINES, 0, &per, 0);
        if (per == WHEEL_PAGESCROLL || per > (UINT)v->visible)
            per = (UINT)v->visible;
        if (per == 0)
            return 0;
        v->wheel += GET_WHEEL_DELTA_WPARAM(wParam);
        lines = v->wheel / WHEEL_DELTA;
        v->wheel -= lines * WHEEL_DELTA;
        kcv_scroll_to(v, v->top - lines * (int)per);
        return 0;
      }

      case WM_VSCROLL: {
        SCROLLINFO si;
        switch (LOWORD(wParam)) {
          case SB_LINEUP:   kcv_scroll_to(v, v->top - 1); break;
          case SB_LINEDOWN: kcv_scroll_to(v, v->top + 1); break;
          case SB_PAGEUP:   kcv_scroll_to(v, v->top - v->visible); break;
          case SB_PAGEDOWN: kcv_scroll_to(v, v->top + v->visible); break;
          case SB_TOP:      kcv_scroll_to(v, 0); break;
          case SB_BOTTOM:   kcv_scroll_to(v, v->lay.nlines); break;
          case SB_THUMBTRACK:
          case SB_THUMBPOSITION:
            memset(&si, 0, sizeof(si));
            si.cbSize = sizeof(si);
            si.fMask = SIF_TRACKPOS;
            if (GetScrollInfo(hwnd, SB_VERT, &si))
                kcv_scroll_to(v, si.nTrackPos);
            break;
        }
        return 0;
      }

      case KCV_GETINFO:
        switch (wParam) {
          case 0: return v->hint ? 0 : v->nlinks;
          case 1: return v->lay.nlines;
          case 2: return v->top;
          case 3: return v->visible;
          case 4: return v->hint;
        }
        return -1;

      case KCV_HITTEST:
        return kcv_hit(v, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)) + 1;

      case KCV_LINKPOINT: {
        POINT pt;
        if (v->hint || (int)wParam < 0 || (int)wParam >= v->nlinks ||
            !kcv_link_point(v, (int)wParam, &pt))
            return -1;
        return MAKELRESULT(pt.x, pt.y);
      }

      case KCV_COPY:
        return kcv_copy(v, (int)wParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool kitty_commentview_register(void)
{
    static int done;
    static bool ok;
    if (!done) {
        WNDCLASSW wc;
        done = 1;
        memset(&wc, 0, sizeof(wc));
        /* global: windows/controls.c creates it with PuTTY's hinst */
        wc.style = CS_GLOBALCLASS;
        wc.lpfnWndProc = kcv_proc;
        wc.hInstance = GetModuleHandle(NULL);
        wc.hCursor = NULL;               /* WM_SETCURSOR picks arrow or hand */
        wc.hbrBackground = NULL;         /* painted whole, in the theme */
        wc.lpszClassName = L"" KITTY_COMMENTVIEW_CLASS;
        ok = RegisterClassW(&wc) != 0 ||
             GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }
    return ok;
}
