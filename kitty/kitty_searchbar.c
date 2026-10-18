/*
 * kitty_searchbar.c - KiTTY++: the find bar of the scrollback search
 * (Ctrl+Shift+F, Find... in the window's menu). The terminal's half is
 * terminal/kitty_search.c.
 *
 * A small SOLID bar at the top right of the terminal (below the program
 * status panel when that is there): a grip, a real edit field (so IME, paste
 * and the caret work as anywhere in Windows), Up and Down, the counter and a
 * close button. It is dragged by the grip, the counter or its background,
 * inside the terminal area; its distance from the area's top-right corner is
 * kept in the session (SearchBarPos), like the status panel's. The field
 * turns red while nothing is found. A modeless dialog on the shared themed
 * template machinery (IDD_SEARCHBAR, dressed by the theme hook, Tab/Esc
 * through ShinyAddAuxDialog); owned by the terminal window, so it stays above
 * it and goes with it. While it has the focus nothing typed reaches the host.
 *
 * Keys in the field: Enter, F3 and Up = Up (older output); Shift+Enter,
 * Shift+F3 and Down = Down; Ctrl+Enter and Ctrl+F3 = the first hit (the
 * oldest), Ctrl+Shift+Enter and Ctrl+Shift+F3 = the last one; Esc closes;
 * Ctrl+Shift+F selects the text; Ctrl+Backspace deletes the word before the
 * caret. Opened again, the bar shows the text it was closed with, selected,
 * and the terminal makes the hit it was closed on current again. While
 * there are hits, a second line - the two Ctrl keys - slides open below the
 * controls (ksb_hint), and shut again when there are none.
 */
#include <stdio.h>
#include <string.h>

#include "putty.h"
#include "terminal.h"
#include <windows.h>
#include <commctrl.h>

#include "kitty_rc_additions.h"   /* IDD_SEARCHBAR, IDC_SB_* */

#include "kitty_searchbar.h"
#include "kitty_dlgbox.h"     /* kitty_dialog_icon */
#include "kitty_oldwin.h"     /* kitty_api: DwmSetWindowAttribute on Windows 11 */
#include "kitty_text.h"
#include "kitty_theme.h"      /* kitty_theme_mark_field_bad */
#include "kitty_winpos.h"     /* kitty_winpos_searchbar_get/set */
#include "kitty_psoverlay.h"  /* kitty_psoverlay_rect */

/* a narrow text macro as a wide literal: L"..." */
#define KSB_W2(x) L##x
#define KSB_W(x) KSB_W2(x)

#define KSB_TIMER        1
#define KSB_TIMER_MS     100
#define KSB_ANIM         2            /* the hint line sliding open or shut */
#define KSB_ANIM_MS      16
#define KSB_ANIM_STEPS   8            /* ~130 ms either way */
#define KSB_MARGIN_PX96  8

typedef struct ksb {
    struct ksb *next;
    HWND owner, h, edit;
    WNDPROC edit_proc;
    bool closing;                 /* closed by the user: tell the terminal */
    bool bad;                     /* the field is red: nothing found */
    char session[256];            /* where the place is kept, "" = nowhere */
    int dx, dy;                   /* from the area's top right, 96-dpi px */
    bool placed;                  /* dx/dy read or dragged (not the default) */
    int h_shut, h_open, h_now;    /* the bar's height without / with the hint */
    bool hint;                    /* the hint line is (or is going) open */
} ksb;

static ksb *ksb_list = NULL;

static ksb *ksb_find(HWND owner)
{
    ksb *b;
    for (b = ksb_list; b; b = b->next)
        if (b->owner == owner)
            return b;
    return NULL;
}

static ksb *ksb_of(HWND h)
{
    ksb *b;
    for (b = ksb_list; b; b = b->next)
        if (b->h == h || b->edit == h)
            return b;
    return NULL;
}

static UINT ksb_dpi(ksb *b)
{
    UINT dpi = 96;
    HDC dc = GetDC(b->owner);
    if (dc) { dpi = GetDeviceCaps(dc, LOGPIXELSX); ReleaseDC(b->owner, dc); }
    return dpi;
}

/* The terminal's client area in screen coordinates. */
static bool ksb_area(ksb *b, RECT *r)
{
    POINT tl = { 0, 0 };
    if (!b->owner || !IsWindow(b->owner) || !GetClientRect(b->owner, r))
        return false;
    ClientToScreen(b->owner, &tl);
    OffsetRect(r, tl.x, tl.y);
    return true;
}

/* Keep the bar rectangle r (screen) inside the terminal area. */
static void ksb_clamp(const RECT *area, RECT *r)
{
    int w = r->right - r->left, h = r->bottom - r->top, x = r->left, y = r->top;
    if (x > area->right - w) x = area->right - w;
    if (x < area->left) x = area->left;
    if (y > area->bottom - h) y = area->bottom - h;
    if (y < area->top) y = area->top;
    SetRect(r, x, y, x + w, y + h);
}

/* At its kept distance from the terminal area's top right; by default a
 * margin in, clear of the scrollbar strip, and below the program status
 * panel when that is shown there. */
static void ksb_place(ksb *b)
{
    RECT area, wr, r, ov, x;
    double s;
    int w, h;
    if (!ksb_area(b, &area) || !GetWindowRect(b->h, &wr))
        return;
    s = ksb_dpi(b) / 96.0;
    if (!b->placed) {
        b->dx = KSB_MARGIN_PX96 * 3;
        b->dy = KSB_MARGIN_PX96;
        if (b->session[0] &&
            kitty_winpos_searchbar_get(b->session, &b->dx, &b->dy))
            b->placed = true;
    }
    w = wr.right - wr.left;
    h = wr.bottom - wr.top;
    r.left = area.right - w - (int)(b->dx * s + .5);
    r.top = area.top + (int)(b->dy * s + .5);
    r.right = r.left + w;
    r.bottom = r.top + h;
    if (!b->placed && kitty_psoverlay_rect(&ov) && IntersectRect(&x, &r, &ov))
        OffsetRect(&r, 0, ov.bottom + (int)(KSB_MARGIN_PX96 * s + .5) - r.top);
    ksb_clamp(&area, &r);
    SetWindowPos(b->h, NULL, r.left, r.top, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

/* Dropped after a drag: the distance from the area's top right is kept. */
static void ksb_take_place(ksb *b)
{
    RECT area, wr;
    double s;
    if (!ksb_area(b, &area) || !GetWindowRect(b->h, &wr))
        return;
    s = ksb_dpi(b) / 96.0;
    b->dx = (int)((area.right - wr.right) / s + .5);
    b->dy = (int)((wr.top - area.top) / s + .5);
    if (b->dx < 0) b->dx = 0;
    if (b->dy < 0) b->dy = 0;
    b->placed = true;
    if (b->session[0])
        kitty_winpos_searchbar_set(b->session, b->dx, b->dy);
}

/* n as the user's locale writes a whole number: its thousands separator
 * and grouping (9.000 in Germany, 9,000 in the US, 9'000 in Switzerland).
 * Wide, as the separator may be no ASCII character (a narrow no-break
 * space in French); GetNumberFormatW also exists on XP. */
static void ksb_num(int n, wchar_t *out, int size)
{
    wchar_t digits[24], sep[8] = L",", dec[8] = L".", grp[16] = L"3;0";
    NUMBERFMTW f;
    unsigned g = 0;
    const wchar_t *p;
    _snwprintf(digits, 24, L"%d", n);
    digits[23] = L'\0';
    GetLocaleInfoW(LOCALE_USER_DEFAULT, LOCALE_STHOUSAND, sep, 8);
    GetLocaleInfoW(LOCALE_USER_DEFAULT, LOCALE_SDECIMAL, dec, 8);
    GetLocaleInfoW(LOCALE_USER_DEFAULT, LOCALE_SGROUPING, grp, 16);
    /* "3;0" -> 3, "3;2;0" -> 32, "3" (no repeat) -> 30 (NUMBERFMT's form) */
    for (p = grp; *p; p++)
        if (*p >= L'0' && *p <= L'9')
            g = g * 10 + (unsigned)(*p - L'0');
    if (wcslen(grp) >= 2 && !wcscmp(grp + wcslen(grp) - 2, L";0"))
        g /= 10;
    else
        g *= 10;
    memset(&f, 0, sizeof(f));
    f.NumDigits = 0;
    f.Grouping = g;
    f.lpDecimalSep = dec;
    f.lpThousandSep = sep;
    f.NegativeOrder = 1;
    if (!GetNumberFormatW(LOCALE_USER_DEFAULT, 0, digits, &f, out, size))
        wcsncpy(out, digits, size);
    out[size - 1] = L'\0';
}

/* tmpl with its "%s" replaced by a, then b, in out (size wide chars). */
static void ksb_fill(const char *tmpl, const wchar_t *a, const wchar_t *b,
                     wchar_t *out, int size)
{
    const wchar_t *arg[2] = { a, b };
    int n = 0, k = 0;
    for (; *tmpl && n < size - 1; tmpl++) {
        if (tmpl[0] == '%' && tmpl[1] == 's' && k < 2) {
            const wchar_t *s = arg[k++];
            while (s && *s && n < size - 1)
                out[n++] = *s++;
            tmpl++;
        } else
            out[n++] = (wchar_t)(unsigned char)*tmpl;
    }
    out[n] = L'\0';
}

/* The hint line (Ctrl+Enter / Ctrl+Shift+Enter) slides open while there
 * are hits and shut when there are none: the bar's height moves a step per
 * KSB_ANIM tick, its top right staying where it is. */
static void ksb_hint(ksb *b, bool show)
{
    if (show == b->hint || !b->h_open)
        return;
    b->hint = show;
    SetTimer(b->h, KSB_ANIM, KSB_ANIM_MS, NULL);
}

static void ksb_anim_step(ksb *b)
{
    int target = b->hint ? b->h_open : b->h_shut;
    int step = (b->h_open - b->h_shut + KSB_ANIM_STEPS - 1) / KSB_ANIM_STEPS;
    RECT wr;
    if (step < 1)
        step = 1;
    if (b->h_now < target)
        b->h_now = b->h_now + step < target ? b->h_now + step : target;
    else if (b->h_now > target)
        b->h_now = b->h_now - step > target ? b->h_now - step : target;
    if (GetWindowRect(b->h, &wr))
        SetWindowPos(b->h, NULL, 0, 0, wr.right - wr.left, b->h_now,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    ksb_place(b);                     /* still inside the terminal area */
    if (b->h_now == target)
        KillTimer(b->h, KSB_ANIM);
}

static void ksb_counter(ksb *b)
{
    Terminal *term = kitty_searchbar_term(b->owner);
    int cur = 0, total = 0, st;
    wchar_t buf[160], ncur[32], ntotal[32];
    if (!term)
        return;
    st = term_search_status(term, &cur, &total);
    buf[0] = L'\0';
    ksb_num(cur, ncur, 32);
    ksb_num(total, ntotal, 32);
    switch (st) {
      case KS_STATE_NOT_AVAILABLE:
        ksb_fill(KT_SEARCH_NOT_AVAILABLE, NULL, NULL, buf, 160);
        break;
      case KS_STATE_SEARCHING:
        ksb_fill(KT_SEARCH_SEARCHING, ntotal, NULL, buf, 160);
        break;
      case KS_STATE_DONE:
        if (total == 0)
            ksb_fill(KT_SEARCH_NO_MATCH, NULL, NULL, buf, 160);
        else if (cur)
            ksb_fill(KT_SEARCH_OF, ncur, ntotal, buf, 160);
        else
            ksb_fill(KT_SEARCH_FOUND, ntotal, NULL, buf, 160);
        break;
      default:
        break;
    }
    {
        wchar_t old[160];
        bool bad = (st == KS_STATE_DONE && total == 0);
        GetDlgItemTextW(b->h, IDC_SB_COUNT, old, 160);
        if (wcscmp(old, buf))
            SetDlgItemTextW(b->h, IDC_SB_COUNT, buf);
        if (bad != b->bad) {
            b->bad = bad;
            kitty_theme_mark_field_bad(b->edit, bad);
        }
        ksb_hint(b, (st == KS_STATE_DONE || st == KS_STATE_SEARCHING) && total > 0);
    }
}

static void ksb_move(ksb *b, int dir)
{
    Terminal *term = kitty_searchbar_term(b->owner);
    if (term)
        term_search_move(term, dir);
    ksb_counter(b);
}

/* The first hit (dir < 0) or the last one (dir > 0). */
static void ksb_jump(ksb *b, int dir)
{
    Terminal *term = kitty_searchbar_term(b->owner);
    if (term)
        term_search_jump(term, dir);
    ksb_counter(b);
}

/* The field: Up/Down/F3 move, Ctrl+F3 jumps, Ctrl+Shift+F selects,
 * Ctrl+Backspace deletes the word before the caret. */
static LRESULT CALLBACK ksb_edit_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    ksb *b = ksb_of(h);
    WNDPROC old = b ? b->edit_proc : DefWindowProcW;
    bool shift = GetKeyState(VK_SHIFT) < 0, ctrl = GetKeyState(VK_CONTROL) < 0;
    if (b && msg == WM_KEYDOWN) {
        if (wp == VK_UP)  { ksb_move(b, -1); return 0; }
        if (wp == VK_DOWN) { ksb_move(b, +1); return 0; }
        if (wp == VK_F3 && ctrl) { ksb_jump(b, shift ? +1 : -1); return 0; }
        if (wp == VK_F3)  { ksb_move(b, shift ? +1 : -1); return 0; }
        if (wp == 'F' && ctrl && shift) { SendMessageW(h, EM_SETSEL, 0, -1); return 0; }
    }
    if (b && msg == WM_CHAR && wp == 0x0A)        /* Ctrl+Enter: no line feed */
        return 0;
    if (b && msg == WM_CHAR && wp == 0x7F) {      /* Ctrl+Backspace */
        DWORD s0 = 0, s1 = 0;
        int n = GetWindowTextLengthW(h), i;
        wchar_t *t = snewn(n + 1, wchar_t);
        GetWindowTextW(h, t, n + 1);
        SendMessageW(h, EM_GETSEL, (WPARAM)&s0, (LPARAM)&s1);
        i = (int)s0;
        while (i > 0 && t[i - 1] == L' ') i--;
        while (i > 0 && t[i - 1] != L' ') i--;
        SendMessageW(h, EM_SETSEL, i, s1);
        SendMessageW(h, EM_REPLACESEL, TRUE, (LPARAM)L"");
        sfree(t);
        return 0;
    }
    return CallWindowProcW(old, h, msg, wp, lp);
}

static void ksb_text_changed(ksb *b)
{
    Terminal *term = kitty_searchbar_term(b->owner);
    int n = GetWindowTextLengthW(b->edit);
    wchar_t *t = snewn(n + 1, wchar_t);
    GetWindowTextW(b->edit, t, n + 1);
    if (term)
        term_search_set(term, t);
    sfree(t);
    ksb_counter(b);
}

static INT_PTR CALLBACK ksb_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    ksb *b = (ksb *)GetWindowLongPtr(h, DWLP_USER);
    switch (msg) {
      case WM_INITDIALOG: {
        b = (ksb *)lp;
        SetWindowLongPtr(h, DWLP_USER, (LONG_PTR)b);
        b->h = h;
        b->edit = GetDlgItem(h, IDC_SB_EDIT);
        SetDlgItemTextW(h, IDOK, L"\x25B2 " KSB_W(KT_SEARCH_UP));
        SetDlgItemTextW(h, IDC_SB_DOWN, L"\x25BC " KSB_W(KT_SEARCH_DOWN));
        SetDlgItemTextW(h, IDCANCEL, L"\x2715");
        SetDlgItemTextW(h, IDC_SB_GRIP, L"\x22EE");   /* the grip */
        SendMessageW(b->edit, EM_SETCUEBANNER, TRUE, (LPARAM)KSB_W(KT_SEARCH_CUE));
        b->edit_proc = (WNDPROC)SetWindowLongPtrW(b->edit, GWLP_WNDPROC,
                                                  (LONG_PTR)ksb_edit_proc);
        {   /* the text it was last closed with, selected: typing replaces it */
            Terminal *term = kitty_searchbar_term(b->owner);
            wchar_t last[512];
            if (term && term_search_text(term, last, lenof(last)) > 0) {
                SetWindowTextW(b->edit, last);
                SendMessageW(b->edit, EM_SETSEL, 0, -1);
            }
        }
        {   /* rounded corners where Windows 11 offers them */
            typedef HRESULT (WINAPI *setattr_t)(HWND, DWORD, LPCVOID, DWORD);
            setattr_t setattr = (setattr_t)kitty_api("dwmapi.dll",
                "DwmSetWindowAttribute", KITTY_API_OPTIONAL, "rounded find bar");
            DWORD round = 2;              /* DWMWCP_ROUND */
            if (setattr)
                setattr(h, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof(round));
        }
        {   /* the hint line, below the template's height: the two heights */
            HWND hint = GetDlgItem(h, IDC_SB_HINT);
            RECT wr, hr;
            SetWindowTextW(hint, KSB_W(KT_SEARCH_HINT));
            kitty_theme_mark_ink(hint, KITTY_INK_HINT);
            if (GetWindowRect(h, &wr) && hint && GetWindowRect(hint, &hr)) {
                int gap = (hr.top - wr.top) - (wr.bottom - wr.top);
                b->h_shut = b->h_now = wr.bottom - wr.top;
                /* below the hint as much air as above it, and the border */
                b->h_open = (hr.bottom - wr.top) + (gap > 0 ? gap : 0) + 2;
            }
        }
        ksb_place(b);
        SetTimer(h, KSB_TIMER, KSB_TIMER_MS, NULL);
        SetFocus(b->edit);
        return FALSE;
      }
      case WM_TIMER:
        if (b && wp == KSB_TIMER)
            ksb_counter(b);
        else if (b && wp == KSB_ANIM)
            ksb_anim_step(b);
        return TRUE;
      case WM_NCHITTEST:
        /* Only the bar itself and its texts (statics are transparent to
         * the mouse) get here, never the field or the buttons: dragged. */
        SetWindowLongPtr(h, DWLP_MSGRESULT, HTCAPTION);
        return TRUE;
      case WM_SETCURSOR:
        if (LOWORD(lp) == HTCAPTION) {
            SetCursor(LoadCursor(NULL, IDC_SIZEALL));
            SetWindowLongPtr(h, DWLP_MSGRESULT, TRUE);
            return TRUE;
        }
        return FALSE;
      case WM_MOVING: {
        RECT area;
        if (b && ksb_area(b, &area))
            ksb_clamp(&area, (RECT *)lp);
        SetWindowLongPtr(h, DWLP_MSGRESULT, TRUE);
        return TRUE;
      }
      case WM_EXITSIZEMOVE:
        if (b)
            ksb_take_place(b);
        if (b)
            SetFocus(b->edit);
        return TRUE;
      case WM_COMMAND:
        if (!b)
            return FALSE;
        switch (LOWORD(wp)) {
          case IDC_SB_EDIT:
            if (HIWORD(wp) == EN_CHANGE)
                ksb_text_changed(b);
            return TRUE;
          case IDOK: {                    /* Enter / the Up button */
            int dir = GetKeyState(VK_SHIFT) < 0 ? +1 : -1;
            /* Ctrl+Enter in the field jumps; a click on Up with Ctrl held
             * stays a step (the focus is on the button then) */
            if (GetKeyState(VK_CONTROL) < 0 && GetFocus() == b->edit)
                ksb_jump(b, dir);
            else
                ksb_move(b, dir);
            SetFocus(b->edit);
            return TRUE;
          }
          case IDC_SB_DOWN:
            ksb_move(b, +1);
            SetFocus(b->edit);
            return TRUE;
          case IDCANCEL:                  /* Esc / the close button */
            b->closing = true;
            DestroyWindow(h);
            return TRUE;
        }
        return FALSE;
      case WM_CLOSE:
        if (b) b->closing = true;
        DestroyWindow(h);
        return TRUE;
      case WM_DESTROY:
        KillTimer(h, KSB_TIMER);
        KillTimer(h, KSB_ANIM);
        ShinyRemoveAuxDialog(h);
        if (b) {
            ksb **pp;
            if (b->closing) {
                Terminal *term = kitty_searchbar_term(b->owner);
                if (term)
                    term_search_close(term);
                if (IsWindow(b->owner))
                    SetFocus(b->owner);   /* back to the terminal */
            }
            if (b->edit && b->edit_proc)
                SetWindowLongPtrW(b->edit, GWLP_WNDPROC, (LONG_PTR)b->edit_proc);
            for (pp = &ksb_list; *pp; pp = &(*pp)->next)
                if (*pp == b) { *pp = b->next; break; }
            SetWindowLongPtr(h, DWLP_USER, 0);
            sfree(b);
        }
        return FALSE;
    }
    return FALSE;
}

void kitty_searchbar_open(HWND owner)
{
    ksb *b = ksb_find(owner);
    Terminal *term = kitty_searchbar_term(owner);
    HWND h;
    if (!term)
        return;
    if (b) {                              /* already open: back to the field */
        SetForegroundWindow(b->h);
        SetFocus(b->edit);
        SendMessageW(b->edit, EM_SETSEL, 0, -1);
        return;
    }
    term_search_open(term);
    b = snew(ksb);
    memset(b, 0, sizeof(*b));
    b->owner = owner;
    snprintf(b->session, sizeof(b->session), "%s", kitty_searchbar_session(owner));
    b->next = ksb_list;
    ksb_list = b;
    h = CreateDialogParamW(hinst, MAKEINTRESOURCEW(IDD_SEARCHBAR), owner,
                           ksb_proc, (LPARAM)b);
    if (!h) {
        ksb_list = b->next;
        sfree(b);
        term_search_close(term);
        return;
    }
    ShinyAddAuxDialog(h);
    ShowWindow(h, SW_SHOW);
}

void kitty_searchbar_follow(HWND owner)
{
    ksb *b = ksb_find(owner);
    if (b)
        ksb_place(b);
}

bool kitty_searchbar_is_open(HWND owner)
{
    return ksb_find(owner) != NULL;
}
