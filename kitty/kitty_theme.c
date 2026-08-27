/*
 * kitty_theme.c - make a plain Win32 dialog paint dark.
 *
 * Windows never gave Win32 dialogs a supported dark mode. What exists is a set
 * of UNEXPORTED uxtheme.dll entry points that the shell itself uses, reachable
 * only by ordinal, plus a documented DWM attribute for the title bar. This
 * file is the one place in KiTTY that touches them:
 *
 *   ordinal 104  RefreshImmersiveColorPolicyState()
 *   ordinal 133  AllowDarkModeForWindow(HWND, BOOL)
 *   ordinal 135  SetPreferredAppMode(int)          (1809: AllowDarkModeForApp)
 *   DwmSetWindowAttribute(..., 20 or 19, ...)      dark title bar
 *
 * Every one of them is resolved at run time and every one may be absent. When
 * anything is missing kitty_theme_available() answers false and the whole file
 * becomes a no-op that leaves the dialog in the light theme it has always had
 * - which is exactly what a Windows 7 / Server 2008 R2 build gets, with no
 * separate code path to maintain.
 *
 * Ordinal 135 changed meaning in 1903: on 1809 the ordinal is a BOOL-taking
 * AllowDarkModeForApp, afterwards an enum-taking SetPreferredAppMode. Both
 * accept 1 as "yes, dark", which is the only value used here, so one call
 * covers both without a version branch on the call itself.
 *
 * The controls are a separate problem from the window. A checkbox under visual
 * styles paints its own background from its theme class and ignores
 * WM_CTLCOLORBTN, so setting brushes alone leaves black text on a black
 * window. The fix is to re-point each control at the shell's dark theme
 * classes - "DarkMode_Explorer" for buttons and the tab strip, "DarkMode_CFD"
 * for the edit and combo boxes - which is what kitty_theme_apply() walks the
 * children to do.
 */

#include <windows.h>
#include <stdbool.h>
#include <string.h>
#include <commctrl.h>   /* SetWindowSubclass + the tab-control messages */

#include "kitty_theme.h"

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
/* 1809 and 1903 used attribute 19 for the same thing before it was given a
 * documented number; try 20 first, then 19, and ignore both failures. */
#define DWMWA_USE_IMMERSIVE_DARK_MODE_PRE20H1 19

/*
 * The Windows 11 caption attributes (build 22000 and later). They are
 * documented and they are the whole modern title bar: instead of the system
 * painting a band in its own colour on top of the window, the caption is
 * given the window's colour and stops reading as a separate strip.
 *
 * Defined here rather than taken from the header because the MinGW dwmapi
 * headers are older than these values, and every call is failure-tolerant
 * anyway - on Windows 10 they simply return an error and the caption keeps
 * the immersive dark/light treatment set above, which is all that Windows
 * offers there.
 */
#define KT_DWMWA_BORDER_COLOR            34
#define KT_DWMWA_CAPTION_COLOR           35
#define KT_DWMWA_TEXT_COLOR              36
#define KT_DWMWA_WINDOW_CORNER_PREFERENCE 33
#define KT_DWMWCP_ROUND                   2

/* The palette. Chosen to match the Windows 11 Settings surfaces rather than
 * pure black, which reads as a hole on an OLED panel and hides the frame. */
#define KT_DARK_BACK  RGB(0x20, 0x20, 0x20)
#define KT_DARK_CTL   RGB(0x2b, 0x2b, 0x2b)
#define KT_DARK_TEXT  RGB(0xf0, 0xf0, 0xf0)
#define KT_DARK_LINE  RGB(0x50, 0x50, 0x50)

typedef BOOL (WINAPI *fn_AllowDarkModeForWindow)(HWND, BOOL);
typedef BOOL (WINAPI *fn_SetPreferredAppMode)(int);
typedef void (WINAPI *fn_RefreshImmersiveColorPolicyState)(void);
typedef HRESULT (WINAPI *fn_DwmSetWindowAttribute)(HWND, DWORD, LPCVOID, DWORD);
typedef HRESULT (WINAPI *fn_SetWindowTheme)(HWND, LPCWSTR, LPCWSTR);

static bool kt_inited = false;
static bool kt_usable = false;
static fn_AllowDarkModeForWindow p_AllowDarkModeForWindow;
static fn_SetPreferredAppMode p_SetPreferredAppMode;
static fn_RefreshImmersiveColorPolicyState p_RefreshImmersiveColorPolicyState;
static fn_DwmSetWindowAttribute p_DwmSetWindowAttribute;
static fn_SetWindowTheme p_SetWindowTheme;

static HBRUSH kt_back_brush;   /* window background */
static HBRUSH kt_ctl_brush;    /* edit/list interiors */

/* Attached to every list view; catches the header's custom draw, which is
 * delivered to the list view and never travels any further. */
static LRESULT CALLBACK kt_lv_subclass(HWND lv, UINT msg, WPARAM wParam,
                                       LPARAM lParam, UINT_PTR id,
                                       DWORD_PTR ref);

/*
 * Which windows have been themed, and how. A window procedure gets
 * WM_CTLCOLOR* long before it could pass its own state in, so the state lives
 * here, keyed by HWND.
 *
 * `dark` alone would not be enough: "not in the table" and "in the table as
 * light" have to be told apart, or a window that is already correct gets
 * re-themed on every activation. A fixed table - kageant never has more than
 * a handful of windows up, and it avoids allocating in a paint path.
 */
#define KT_MAX_WINDOWS 16
static struct kt_window {
    HWND w;
    bool dark;
    bool msgbox;   /* a system message box, not one of our own templates */
} kt_windows[KT_MAX_WINDOWS];

static struct kt_window *kt_find_window(HWND w)
{
    int i;
    for (i = 0; i < KT_MAX_WINDOWS; i++)
        if (kt_windows[i].w == w)
            return &kt_windows[i];
    return NULL;
}

static bool kt_is_dark_window(HWND w)
{
    struct kt_window *e = kt_find_window(w);
    return e && e->dark;
}

static void kt_set_dark_window(HWND w, bool dark)
{
    struct kt_window *e = kt_find_window(w);
    int i;
    if (e) {
        e->dark = dark;
        return;
    }
    for (i = 0; i < KT_MAX_WINDOWS; i++) {
        if (!kt_windows[i].w) {
            kt_windows[i].w = w;
            kt_windows[i].dark = dark;
            return;
        }
    }
    /* Full. The table is only an optimisation for repeat work, so the window
     * still gets themed - it just gets themed again next time. */
}

/*
 * Dark mode arrived in Windows 10 1809 (build 17763). The version is read from
 * ntdll rather than GetVersionEx, which reports 6.2 to a process whose
 * manifest does not claim the newer OS.
 */
static bool kt_build_at_least(DWORD want)
{
    typedef LONG (WINAPI *fn_RtlGetVersion)(PRTL_OSVERSIONINFOW);
    HMODULE nt = GetModuleHandleA("ntdll.dll");
    fn_RtlGetVersion p;
    RTL_OSVERSIONINFOW vi;

    if (!nt)
        return false;
    p = (fn_RtlGetVersion)(void *)GetProcAddress(nt, "RtlGetVersion");
    if (!p)
        return false;
    memset(&vi, 0, sizeof(vi));
    vi.dwOSVersionInfoSize = sizeof(vi);
    if (p(&vi) != 0)
        return false;
    return vi.dwMajorVersion > 10 ||
        (vi.dwMajorVersion == 10 && vi.dwBuildNumber >= want);
}

static void kt_init(void)
{
    HMODULE ux, dwm;

    if (kt_inited)
        return;
    kt_inited = true;

    if (!kt_build_at_least(17763))
        return;

    ux = LoadLibraryA("uxtheme.dll");
    dwm = LoadLibraryA("dwmapi.dll");
    if (!ux || !dwm)
        return;

    /* By ordinal: these have no names in the export table. MAKEINTRESOURCEA
     * is how GetProcAddress is told the argument is an ordinal. */
    p_AllowDarkModeForWindow = (fn_AllowDarkModeForWindow)(void *)
        GetProcAddress(ux, MAKEINTRESOURCEA(133));
    p_SetPreferredAppMode = (fn_SetPreferredAppMode)(void *)
        GetProcAddress(ux, MAKEINTRESOURCEA(135));
    p_RefreshImmersiveColorPolicyState =
        (fn_RefreshImmersiveColorPolicyState)(void *)
        GetProcAddress(ux, MAKEINTRESOURCEA(104));
    p_SetWindowTheme = (fn_SetWindowTheme)(void *)
        GetProcAddress(ux, "SetWindowTheme");
    p_DwmSetWindowAttribute = (fn_DwmSetWindowAttribute)(void *)
        GetProcAddress(dwm, "DwmSetWindowAttribute");

    if (!p_AllowDarkModeForWindow || !p_SetPreferredAppMode ||
        !p_SetWindowTheme || !p_DwmSetWindowAttribute)
        return;

    /* 1 = AllowDark on 1809, ForceDark on 1903+. Either way the process may
     * now ask for dark controls; individual windows still opt in one at a
     * time through AllowDarkModeForWindow. */
    p_SetPreferredAppMode(1);
    if (p_RefreshImmersiveColorPolicyState)
        p_RefreshImmersiveColorPolicyState();

    kt_back_brush = CreateSolidBrush(KT_DARK_BACK);
    kt_ctl_brush = CreateSolidBrush(KT_DARK_CTL);
    kt_usable = (kt_back_brush && kt_ctl_brush);
}

bool kitty_theme_available(void)
{
    kt_init();
    return kt_usable;
}

bool kitty_theme_system_is_dark(void)
{
    HKEY hk;
    DWORD val = 1, sz = sizeof(val), type = 0;
    bool dark = false;

    /* Absent (every Windows before 10 1809, and a fresh profile) means the
     * classic light theme, which is the safe answer. */
    if (RegOpenKeyExA(HKEY_CURRENT_USER,
                      "Software\\Microsoft\\Windows\\CurrentVersion\\"
                      "Themes\\Personalize",
                      0, KEY_QUERY_VALUE, &hk) == ERROR_SUCCESS) {
        if (RegQueryValueExA(hk, "AppsUseLightTheme", NULL, &type,
                             (LPBYTE)&val, &sz) == ERROR_SUCCESS &&
            type == REG_DWORD)
            dark = (val == 0);
        RegCloseKey(hk);
    }
    return dark;
}

bool kitty_theme_dark_for(int pref)
{
    if (!kitty_theme_available())
        return false;
    if (pref == KITTY_THEME_DARK)
        return true;
    if (pref == KITTY_THEME_LIGHT)
        return false;
    return kitty_theme_system_is_dark();
}

COLORREF kitty_theme_text_colour(bool dark)
{
    return dark ? KT_DARK_TEXT : GetSysColor(COLOR_WINDOWTEXT);
}

COLORREF kitty_theme_back_colour(bool dark)
{
    return dark ? KT_DARK_BACK : GetSysColor(COLOR_BTNFACE);
}

COLORREF kitty_theme_line_colour(bool dark)
{
    return dark ? KT_DARK_LINE : GetSysColor(COLOR_BTNSHADOW);
}

bool kitty_theme_window_dark(HWND w)
{
    return kt_is_dark_window(w);
}

/*
 * The dark inks are not the light ones inverted: a colour picked to be legible
 * as dark-on-white is close to unreadable as the same dark-on-near-black. Each
 * pair was chosen for contrast against its own background, which is why they
 * are listed side by side rather than derived from one another.
 */
COLORREF kitty_theme_ink(bool dark, kitty_ink which)
{
    switch (which) {
      case KITTY_INK_GOOD:
        return dark ? RGB(0x6c, 0xd0, 0x6c) : RGB(0x00, 0x7a, 0x00);
      case KITTY_INK_WARN:
        return dark ? RGB(0xe8, 0xb3, 0x39) : RGB(0xb0, 0x6a, 0x00);
      case KITTY_INK_BAD:
        return dark ? RGB(0xff, 0x7b, 0x72) : RGB(0xb2, 0x00, 0x00);
      case KITTY_INK_INFO:
        return dark ? RGB(0x6c, 0xb6, 0xff) : RGB(0x00, 0x4c, 0xa0);
      case KITTY_INK_NORMAL:
      default:
        return dark ? KT_DARK_TEXT : GetSysColor(COLOR_WINDOWTEXT);
    }
}

COLORREF kitty_theme_row_colour(bool dark, bool alternate)
{
    if (dark)
        return alternate ? RGB(0x27, 0x27, 0x27) : KT_DARK_BACK;
    return alternate ? RGB(0xf2, 0xf6, 0xfc) : GetSysColor(COLOR_WINDOW);
}

/*
 * Point one control at the right theme class. The class names are the shell's
 * own; an unrecognised name is not an error, the control simply keeps the
 * classic look, so passing "DarkMode_CFD" to a Windows that has never heard
 * of it costs nothing.
 */
static BOOL CALLBACK kt_theme_child(HWND child, LPARAM lp)
{
    BOOL dark = (BOOL)lp;
    char cls[64];

    if (!GetClassNameA(child, cls, sizeof(cls)))
        return TRUE;

    if (p_AllowDarkModeForWindow)
        p_AllowDarkModeForWindow(child, dark);

    if (!stricmp(cls, "Edit") || !stricmp(cls, "ComboBox")) {
        /*
         * CFD = the combo/edit "flat dark" class the shell's own search boxes
         * use, and it gives the right border - but it does NOT dark-theme a
         * scroll bar, so a multi-line box came out with a bright white bar
         * down its side.
         *
         * Explorer's class does theme the scroll bar. So the class is chosen
         * by whether this box HAS one: the interior is dark either way,
         * because WM_CTLCOLOREDIT answers with the dark brush regardless.
         */
        LONG st = GetWindowLong(child, GWL_STYLE);
        bool scrolls = (st & (WS_VSCROLL | WS_HSCROLL | ES_MULTILINE)) != 0;
        p_SetWindowTheme(child,
                         dark ? (scrolls ? L"DarkMode_Explorer"
                                         : L"DarkMode_CFD")
                              : NULL, NULL);
    } else {
        /* Buttons (which is also every checkbox and group box), static text,
         * the tab strip, list views, scroll bars. */
        p_SetWindowTheme(child, dark ? L"DarkMode_Explorer" : NULL, NULL);
    }

    if (!stricmp(cls, "SysHeader32")) {
        /* The column header is a child of the list view, not of the dialog,
         * and it takes a theme class of its own. Explorer's leaves it white. */
        p_SetWindowTheme(child, dark ? L"DarkMode_ItemsView" : NULL, NULL);
    }

    if (!stricmp(cls, "SysListView32")) {
        /* Grid lines are drawn from a system colour with no way to set them,
         * so in a dark list they come out as a white cage over the rows. They
         * go away while dark and come back in the light theme, where they
         * were a deliberate choice. */
        SendMessage(child, LVM_SETEXTENDEDLISTVIEWSTYLE,
                    LVS_EX_GRIDLINES, dark ? 0 : LVS_EX_GRIDLINES);
        /* The header's custom draw is delivered HERE and nowhere else. */
        SetWindowSubclass(child, kt_lv_subclass, 3, 0);
        /* The theme class reaches the header and the scroll bars but NOT the
         * item area, which keeps painting on white until it is told
         * otherwise. These three are the control's base colours; a row that
         * sets its own in custom draw still overrides them, which is what
         * keeps the key list's grey and tinted rows working. */
        SendMessage(child, LVM_SETBKCOLOR, 0,
                    (LPARAM)(dark ? KT_DARK_BACK : GetSysColor(COLOR_WINDOW)));
        SendMessage(child, LVM_SETTEXTBKCOLOR, 0,
                    (LPARAM)(dark ? KT_DARK_BACK : GetSysColor(COLOR_WINDOW)));
        SendMessage(child, LVM_SETTEXTCOLOR, 0,
                    (LPARAM)(dark ? KT_DARK_TEXT
                                  : GetSysColor(COLOR_WINDOWTEXT)));
    } else if (!stricmp(cls, "SysTabControl32")) {
        /* Hand-drawn in both themes - see kitty_theme_attach_tabs. Done here
         * so any dialog with a tab strip gets it without having to know. */
        kitty_theme_attach_tabs(child);
    }

    SendMessage(child, WM_THEMECHANGED, 0, 0);
    InvalidateRect(child, NULL, TRUE);
    return TRUE;
}

void kitty_theme_apply(HWND dlg, bool dark)
{
    BOOL on;

    if (!dlg || !IsWindow(dlg))
        return;
    kt_init();
    if (!kt_usable) {
        /* Nothing available: make sure we are not remembered as dark, so the
         * CTLCOLOR handler stays out of the way. */
        kt_set_dark_window(dlg, false);
        return;
    }

    kt_set_dark_window(dlg, dark);

    p_AllowDarkModeForWindow(dlg, dark);
    if (p_RefreshImmersiveColorPolicyState)
        p_RefreshImmersiveColorPolicyState();

    /* Title bar, in two layers.
     *
     * FIRST, the immersive dark flag. 20 is the documented attribute; 19 was
     * the same thing on 1809/1903. Both are attempted and both failures
     * ignored - a light title bar over a dark client is ugly, not broken.
     * On Windows 10 this is the whole story: the caption gets the system's
     * dark or light treatment and nothing finer is available. */
    on = dark ? TRUE : FALSE;
    if (FAILED(p_DwmSetWindowAttribute(dlg, DWMWA_USE_IMMERSIVE_DARK_MODE,
                                       &on, sizeof(on))))
        p_DwmSetWindowAttribute(dlg, DWMWA_USE_IMMERSIVE_DARK_MODE_PRE20H1,
                                &on, sizeof(on));

    /* SECOND, on Windows 11, the caption's actual colours. Painting the
     * caption in the DIALOG's own background colour is what removes the
     * band: the title bar stops being a strip in the system's colour sitting
     * on top of the window and becomes the top of the window. The border is
     * given a quiet line so the window still has an edge against a same
     * coloured one behind it, and the corners are asked for explicitly
     * rather than left to whatever the window class inherited.
     *
     * All four fail on Windows 10 and earlier, which is exactly the intended
     * outcome - the immersive flag above already gave those the best caption
     * they have. Nothing here is retried or reported. */
    {
        COLORREF caption = dark ? KT_DARK_BACK : GetSysColor(COLOR_BTNFACE);
        COLORREF captext = dark ? KT_DARK_TEXT : GetSysColor(COLOR_WINDOWTEXT);
        COLORREF border  = dark ? KT_DARK_LINE : GetSysColor(COLOR_BTNSHADOW);
        DWORD corner = KT_DWMWCP_ROUND;

        p_DwmSetWindowAttribute(dlg, KT_DWMWA_CAPTION_COLOR,
                                &caption, sizeof(caption));
        p_DwmSetWindowAttribute(dlg, KT_DWMWA_TEXT_COLOR,
                                &captext, sizeof(captext));
        p_DwmSetWindowAttribute(dlg, KT_DWMWA_BORDER_COLOR,
                                &border, sizeof(border));
        p_DwmSetWindowAttribute(dlg, KT_DWMWA_WINDOW_CORNER_PREFERENCE,
                                &corner, sizeof(corner));
    }

    EnumChildWindows(dlg, kt_theme_child, (LPARAM)dark);

    InvalidateRect(dlg, NULL, TRUE);
    /* The frame is painted outside WM_PAINT, so an invalidate does not reach
     * it; this is what makes the title bar change colour without a resize. */
    SetWindowPos(dlg, NULL, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                 SWP_FRAMECHANGED);
}

HBRUSH kitty_theme_backbrush(HWND dlg)
{
    return kt_is_dark_window(dlg) ? kt_back_brush : NULL;
}

HBRUSH kitty_theme_ctlcolor(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    HDC dc = (HDC)wParam;

    if (!kt_is_dark_window(dlg))
        return NULL;

    switch (msg) {
      case WM_CTLCOLORDLG:
      case WM_CTLCOLORSTATIC:
      case WM_CTLCOLORBTN:
        SetTextColor(dc, KT_DARK_TEXT);
        SetBkColor(dc, KT_DARK_BACK);
        return kt_back_brush;
      case WM_CTLCOLOREDIT:
      case WM_CTLCOLORLISTBOX:
        SetTextColor(dc, KT_DARK_TEXT);
        SetBkColor(dc, KT_DARK_CTL);
        return kt_ctl_brush;
    }
    return NULL;
}

/*
 * ---- the tab strip ----
 *
 * Every other control here is fixed by pointing it at a dark theme class.
 * SysTabControl32 is the exception: no DarkMode_* class reaches it, so the
 * strip is drawn from scratch.
 *
 * It is drawn from scratch in BOTH themes, not only in dark. The control's own
 * light drawing says "selected" with a raised sheet joined to the pane below
 * it - and there is no pane here, so all that survives is a hairline outline
 * and a slightly darker label, which is not enough to see at a glance. One
 * drawing routine for both themes gives the two the same, legible marker and
 * removes the question of which theme looks like what.
 *
 * The marker is a bar in the SYSTEM ACCENT COLOUR, which is what Windows 11
 * itself uses to mark a selected pivot. It is read from DWM rather than
 * invented, so it follows whatever the user picked - and it is lightened or
 * darkened as needed, because an accent chosen to look good on the desktop can
 * be unreadable against this particular background.
 */
#define KT_TAB_ACCENT_H 2                        /* the marker bar, in pixels */
#define KT_DARK_TAB_DIM RGB(0xb0, 0xb0, 0xb0)    /* unselected label, dark */
#define KT_LIGHT_TAB_DIM RGB(0x60, 0x60, 0x60)   /* unselected label, light */
#define KT_FALLBACK_ACCENT RGB(0x00, 0x67, 0xc0) /* Windows 11's own default */

static int kt_luminance(COLORREF c)
{
    /* The usual perceptual weighting: green carries most of the brightness. */
    return (GetRValue(c) * 299 + GetGValue(c) * 587 + GetBValue(c) * 114) / 1000;
}

static COLORREF kt_mix(COLORREF c, COLORREF towards, int percent)
{
    int r = GetRValue(c) + (GetRValue(towards) - GetRValue(c)) * percent / 100;
    int g = GetGValue(c) + (GetGValue(towards) - GetGValue(c)) * percent / 100;
    int b = GetBValue(c) + (GetBValue(towards) - GetBValue(c)) * percent / 100;
    return RGB(r, g, b);
}

/*
 * The user's accent colour, made readable against the given background.
 *
 * DwmGetColorizationColor is the documented way to ask (Vista onwards) and
 * returns 0xAARRGGBB with the alpha meaning blend, not transparency - it is
 * discarded. A dark navy accent on a dark dialog and a pale mint one on a
 * light dialog are both invisible, so the result is pushed away from the
 * background until there is something to see.
 */
static COLORREF kt_accent_for(bool dark)
{
    typedef HRESULT (WINAPI *fn_DwmGetColorizationColor)(DWORD *, BOOL *);
    static bool asked = false;
    static COLORREF sys_accent;
    static bool have_sys = false;
    COLORREF c;

    if (!asked) {
        HMODULE dwm = LoadLibraryA("dwmapi.dll");
        asked = true;
        if (dwm) {
            fn_DwmGetColorizationColor p = (fn_DwmGetColorizationColor)(void *)
                GetProcAddress(dwm, "DwmGetColorizationColor");
            DWORD argb = 0;
            BOOL opaque = FALSE;
            if (p && SUCCEEDED(p(&argb, &opaque))) {
                /* DWM hands back 0xAARRGGBB; COLORREF wants 0x00BBGGRR. */
                sys_accent = RGB((argb >> 16) & 0xFF, (argb >> 8) & 0xFF,
                                 argb & 0xFF);
                have_sys = true;
            }
        }
    }

    c = have_sys ? sys_accent : KT_FALLBACK_ACCENT;

    if (dark) {
        /* Lift it towards white until it reads against a near-black panel. */
        if (kt_luminance(c) < 110)
            c = kt_mix(c, RGB(255, 255, 255), 55);
    } else {
        /* Push it towards black until it reads against a near-white panel. */
        if (kt_luminance(c) > 150)
            c = kt_mix(c, RGB(0, 0, 0), 45);
    }
    return c;
}

static void kt_paint_tabs(HWND tab, HDC dc, bool dark)
{
    RECT rc, ir;
    int i, n, sel;
    HFONT font, oldfont;
    int oldbk;
    COLORREF back, selfill, text, dim;
    HBRUSH backbr, selbr, accentbr;

    back    = dark ? KT_DARK_BACK : GetSysColor(COLOR_BTNFACE);
    selfill = dark ? KT_DARK_CTL  : GetSysColor(COLOR_WINDOW);
    text    = dark ? KT_DARK_TEXT : GetSysColor(COLOR_WINDOWTEXT);
    dim     = dark ? KT_DARK_TAB_DIM : KT_LIGHT_TAB_DIM;

    backbr = CreateSolidBrush(back);
    selbr = CreateSolidBrush(selfill);
    accentbr = CreateSolidBrush(kt_accent_for(dark));
    if (!backbr || !selbr || !accentbr)
        goto done;

    GetClientRect(tab, &rc);
    FillRect(dc, &rc, backbr);

    n = (int)SendMessage(tab, TCM_GETITEMCOUNT, 0, 0);
    sel = (int)SendMessage(tab, TCM_GETCURSEL, 0, 0);

    font = (HFONT)SendMessage(tab, WM_GETFONT, 0, 0);
    oldfont = font ? (HFONT)SelectObject(dc, font) : NULL;
    oldbk = SetBkMode(dc, TRANSPARENT);

    for (i = 0; i < n; i++) {
        char label[128];
        TCITEMA ti;

        if (!SendMessage(tab, TCM_GETITEMRECT, (WPARAM)i, (LPARAM)&ir))
            continue;

        label[0] = '\0';
        memset(&ti, 0, sizeof(ti));
        ti.mask = TCIF_TEXT;
        ti.pszText = label;
        ti.cchTextMax = sizeof(label);
        SendMessage(tab, TCM_GETITEMA, (WPARAM)i, (LPARAM)&ti);

        if (i == sel) {
            RECT bar = ir;
            FillRect(dc, &ir, selbr);
            bar.top = bar.bottom - KT_TAB_ACCENT_H;
            FillRect(dc, &bar, accentbr);
            SetTextColor(dc, text);
        } else {
            SetTextColor(dc, dim);
        }

        DrawTextA(dc, label, -1, &ir,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    SetBkMode(dc, oldbk);
    if (oldfont)
        SelectObject(dc, oldfont);

  done:
    if (backbr) DeleteObject(backbr);
    if (selbr) DeleteObject(selbr);
    if (accentbr) DeleteObject(accentbr);
}

static LRESULT CALLBACK kt_tab_subclass(HWND hwnd, UINT msg, WPARAM wParam,
                                        LPARAM lParam, UINT_PTR id,
                                        DWORD_PTR ref)
{
    (void)id; (void)ref;

    switch (msg) {
      case WM_ERASEBKGND:
        /* Claimed so the control never paints its own face, not even for the
         * instant before WM_PAINT lands. */
        return 1;
      case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (dc) {
            /* The dialog is the thing that is dark or light, so ask about the
             * parent - the strip has no theme of its own. */
            kt_paint_tabs(hwnd, dc, kt_is_dark_window(GetParent(hwnd)));
        }
        EndPaint(hwnd, &ps);
        return 0;
      }
      case TCM_SETCURSEL:
      case WM_LBUTTONDOWN:
      case WM_KEYDOWN: {
        /* The control moves the selection without knowing the marker moved
         * with it, and would repaint only the two items it thinks changed. */
        LRESULT r = DefSubclassProc(hwnd, msg, wParam, lParam);
        InvalidateRect(hwnd, NULL, FALSE);
        return r;
      }
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

void kitty_theme_attach_tabs(HWND tab)
{
    if (!tab || !IsWindow(tab))
        return;
    /* Deliberately NOT gated on kitty_theme_available(): the strip is drawn
     * by hand in the light theme too, and that needs nothing newer than GDI -
     * so a Windows that can never go dark still gets the same tab marker.
     * SetWindowSubclass replaces an entry with the same id rather than
     * stacking a second one, so a repeat call costs nothing. */
    SetWindowSubclass(tab, kt_tab_subclass, 1, 0);
}

void kitty_theme_forget(HWND dlg)
{
    struct kt_window *e = kt_find_window(dlg);
    if (e) {
        e->w = NULL;
        e->dark = false;
    }
}

/*
 * ---- theming every dialog in the process, including the message boxes ----
 *
 * The alternative was four extra lines in each of nine dialog procedures -
 * apply, three WM_CTLCOLOR cases, forget - plus a rewrite of thirty-nine
 * MessageBox calls into a home-made dialog. That is a lot of surface for one
 * behaviour, and the tenth dialog someone adds would quietly not be themed.
 *
 * Instead a CBT hook on this thread catches every dialog as it is activated
 * and subclasses it. The subclass answers WM_CTLCOLOR* before the dialog
 * manager does, which is what makes this work for windows whose procedure we
 * do not own - a MessageBox is a real #32770 dialog with a procedure inside
 * user32, and it colours exactly like our own once its window procedure is in
 * front of user32's.
 *
 * WHAT IT DELIBERATELY WILL NOT TOUCH: a dialog is themed only if we created
 * it, or if it has nothing but buttons and static text in it (which is what a
 * message box is). The common file dialogs are #32770 too, and they are full
 * of list views and toolbars, so they fail that test and are left alone -
 * which is right, because the shell already themes those itself once
 * SetPreferredAppMode has been called.
 */
static HHOOK kt_cbt_hook;

/* Does this window contain only the controls a message box contains? */
static BOOL CALLBACK kt_plain_child(HWND child, LPARAM lp)
{
    char cls[64];
    if (!GetClassNameA(child, cls, sizeof(cls)))
        return TRUE;
    if (stricmp(cls, "Button") && stricmp(cls, "Static")) {
        *(bool *)lp = false;
        return FALSE;    /* one foreign control is enough to decide */
    }
    return TRUE;
}

static bool kt_looks_like_messagebox(HWND w)
{
    bool plain = true;
    EnumChildWindows(w, kt_plain_child, (LPARAM)&plain);
    return plain;
}

/*
 * Radio buttons, drawn by hand while dark.
 *
 * This is NOT done for check boxes, and the asymmetry is the point:
 * "DarkMode_Explorer" maps the check-box parts, so a check box comes out with
 * white text and a dark glyph on its own. It does not map the radio parts,
 * and a radio button was left drawing BLACK text on the dark dialog -
 * measured on a screenshot, not deduced. Only the control that is actually
 * broken is taken over; a check box keeps the theme's own drawing, which is
 * better than anything reimplemented here and needs no maintenance.
 *
 * The glyph is drawn rather than themed for the same reason: if the dark
 * theme class had radio parts to hand out, none of this would be needed.
 */
static void kt_paint_radio(HWND btn, NMCUSTOMDRAW *cd)
{
    RECT rc = cd->rc, text;
    LONG st = GetWindowLong(btn, GWL_STYLE);
    bool checked = SendMessage(btn, BM_GETCHECK, 0, 0) == BST_CHECKED;
    bool enabled = IsWindowEnabled(btn) != 0;
    bool focus = (cd->uItemState & CDIS_FOCUS) != 0;
    int d, cx, cy;
    char label[256];
    HFONT font, oldfont;
    HPEN pen, oldpen;
    HBRUSH fill, oldbrush;
    COLORREF ring = enabled ? (checked ? kt_accent_for(true) : KT_DARK_LINE)
                            : KT_DARK_LINE;

    FillRect(cd->hdc, &rc, kt_back_brush);

    /* A ring the height of one line, centred vertically, with the label
     * beside it - the proportions the system uses. */
    d = 13 * (rc.bottom - rc.top) / 20;
    if (d < 10) d = 10;
    if (d > rc.bottom - rc.top) d = rc.bottom - rc.top;
    cx = rc.left + 1 + d / 2;
    cy = (rc.top + rc.bottom) / 2;

    pen = CreatePen(PS_SOLID, 1, ring);
    fill = CreateSolidBrush(checked ? ring : KT_DARK_CTL);
    oldpen = pen ? (HPEN)SelectObject(cd->hdc, pen) : NULL;
    oldbrush = fill ? (HBRUSH)SelectObject(cd->hdc, fill) : NULL;
    if (checked) {
        /* Windows 11 draws a filled ring with a hole, not a dot on a disc. */
        Ellipse(cd->hdc, cx - d / 2, cy - d / 2, cx + d / 2, cy + d / 2);
        {
            HBRUSH inner = CreateSolidBrush(KT_DARK_BACK);
            HPEN ipen = CreatePen(PS_SOLID, 1, ring);
            HBRUSH ob = inner ? (HBRUSH)SelectObject(cd->hdc, inner) : NULL;
            HPEN op = ipen ? (HPEN)SelectObject(cd->hdc, ipen) : NULL;
            int r2 = d / 4;
            if (r2 < 2) r2 = 2;
            Ellipse(cd->hdc, cx - r2, cy - r2, cx + r2, cy + r2);
            if (ob) SelectObject(cd->hdc, ob);
            if (op) SelectObject(cd->hdc, op);
            if (inner) DeleteObject(inner);
            if (ipen) DeleteObject(ipen);
        }
    } else {
        Ellipse(cd->hdc, cx - d / 2, cy - d / 2, cx + d / 2, cy + d / 2);
    }
    if (oldpen) SelectObject(cd->hdc, oldpen);
    if (oldbrush) SelectObject(cd->hdc, oldbrush);
    if (pen) DeleteObject(pen);
    if (fill) DeleteObject(fill);

    label[0] = '\0';
    GetWindowTextA(btn, label, sizeof(label));
    text = rc;
    text.left = cx + d / 2 + 6;

    font = (HFONT)SendMessage(btn, WM_GETFONT, 0, 0);
    oldfont = font ? (HFONT)SelectObject(cd->hdc, font) : NULL;
    SetBkMode(cd->hdc, TRANSPARENT);
    SetTextColor(cd->hdc, enabled ? KT_DARK_TEXT : KT_DARK_TAB_DIM);
    DrawTextA(cd->hdc, label, -1, &text,
              ((st & BS_RIGHT) ? DT_RIGHT : DT_LEFT) |
              DT_VCENTER | DT_SINGLELINE);
    if (focus) {
        RECT f = text;
        DrawFocusRect(cd->hdc, &f);
    }
    if (oldfont)
        SelectObject(cd->hdc, oldfont);
}

/*
 * A list view's column header, drawn by hand while dark.
 *
 * "DarkMode_ItemsView" gives the header a dark BACKGROUND but leaves its text
 * black, so the column titles came out black on near-black - present, but
 * only readable at an angle. There is no theme class that fixes the text, so
 * the header items are drawn here.
 *
 * The notification has to be caught ON THE LIST VIEW. The header sends its
 * WM_NOTIFY to its parent, which IS the list view, and the list view handles
 * it and stops - it does NOT pass it up to the dialog. Hooking the dialog and
 * waiting for it there was tried and drew nothing at all, which looked exactly
 * like the unfixed control.
 */
static void kt_paint_header_item(HWND hdr, NMCUSTOMDRAW *cd)
{
    RECT rc = cd->rc, text;
    char label[128];
    HDITEMA hi;
    HFONT font, oldfont;
    HBRUSH back, line;
    UINT align = DT_LEFT;

    back = CreateSolidBrush(KT_DARK_CTL);
    line = CreateSolidBrush(KT_DARK_LINE);
    if (!back || !line)
        goto done;

    FillRect(cd->hdc, &rc, back);
    /* The divider between columns, and the rule under the whole header - the
     * two edges the themed header would have drawn for us. */
    {
        RECT edge = rc;
        edge.left = edge.right - 1;
        FillRect(cd->hdc, &edge, line);
        edge = rc;
        edge.top = edge.bottom - 1;
        FillRect(cd->hdc, &edge, line);
    }

    label[0] = '\0';
    memset(&hi, 0, sizeof(hi));
    hi.mask = HDI_TEXT | HDI_FORMAT;
    hi.pszText = label;
    hi.cchTextMax = sizeof(label);
    if (SendMessage(hdr, HDM_GETITEMA, (WPARAM)cd->dwItemSpec, (LPARAM)&hi)) {
        if ((hi.fmt & HDF_JUSTIFYMASK) == HDF_RIGHT)
            align = DT_RIGHT;
        else if ((hi.fmt & HDF_JUSTIFYMASK) == HDF_CENTER)
            align = DT_CENTER;
    }

    text = rc;
    text.left += 6;
    text.right -= 6;
    font = (HFONT)SendMessage(hdr, WM_GETFONT, 0, 0);
    oldfont = font ? (HFONT)SelectObject(cd->hdc, font) : NULL;
    SetBkMode(cd->hdc, TRANSPARENT);
    SetTextColor(cd->hdc, KT_DARK_TEXT);
    DrawTextA(cd->hdc, label, -1, &text,
              align | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (oldfont)
        SelectObject(cd->hdc, oldfont);

  done:
    if (back) DeleteObject(back);
    if (line) DeleteObject(line);
}

/*
 * Sits on the LIST VIEW, purely to catch its header's custom draw - the one
 * place that notification is ever delivered.
 */
static LRESULT CALLBACK kt_lv_subclass(HWND lv, UINT msg, WPARAM wParam,
                                       LPARAM lParam, UINT_PTR id,
                                       DWORD_PTR ref)
{
    (void)id; (void)ref;

    if (msg == WM_NOTIFY) {
        NMHDR *nm = (NMHDR *)lParam;
        char cls[64];
        if (nm && nm->code == NM_CUSTOMDRAW && nm->hwndFrom &&
            kt_is_dark_window(GetParent(lv)) &&
            GetClassNameA(nm->hwndFrom, cls, sizeof(cls)) &&
            !stricmp(cls, "SysHeader32")) {
            NMCUSTOMDRAW *cd = (NMCUSTOMDRAW *)lParam;
            if (cd->dwDrawStage == CDDS_PREPAINT)
                return CDRF_NOTIFYITEMDRAW;
            if (cd->dwDrawStage == CDDS_ITEMPREPAINT) {
                kt_paint_header_item(nm->hwndFrom, cd);
                return CDRF_SKIPDEFAULT;
            }
        }
    }
    return DefSubclassProc(lv, msg, wParam, lParam);
}

static LRESULT CALLBACK kt_dlg_subclass(HWND hwnd, UINT msg, WPARAM wParam,
                                        LPARAM lParam, UINT_PTR id,
                                        DWORD_PTR ref)
{
    (void)id; (void)ref;

    switch (msg) {
      case WM_NOTIFY: {
        NMHDR *nm = (NMHDR *)lParam;
        char cls[64];
        if (nm && nm->code == NM_CUSTOMDRAW && kt_is_dark_window(hwnd) &&
            kt_back_brush && nm->hwndFrom &&
            GetClassNameA(nm->hwndFrom, cls, sizeof(cls)) &&
            !stricmp(cls, "Button")) {
            LONG type = GetWindowLong(nm->hwndFrom, GWL_STYLE) & BS_TYPEMASK;
            if (type == BS_AUTORADIOBUTTON || type == BS_RADIOBUTTON) {
                NMCUSTOMDRAW *cd = (NMCUSTOMDRAW *)lParam;
                if (cd->dwDrawStage == CDDS_PREPAINT) {
                    kt_paint_radio(nm->hwndFrom, cd);
                    return CDRF_SKIPDEFAULT;
                }
            }
        }
        break;
      }
      case WM_CTLCOLORDLG:
      case WM_CTLCOLORSTATIC:
      case WM_CTLCOLORBTN:
      case WM_CTLCOLOREDIT:
      case WM_CTLCOLORLISTBOX: {
        HBRUSH b = kitty_theme_ctlcolor(hwnd, msg, wParam, lParam);
        if (b)
            return (LRESULT)b;
        break;    /* light: let the dialog manager answer as it always did */
      }
      case WM_PAINT: {
        /*
         * A message box does not paint its whole background through
         * WM_CTLCOLORDLG. Windows 11 gives it a two-tone body - message area
         * above, a lighter strip along the bottom holding the buttons - and
         * user32 fills that strip itself, inside WM_PAINT. It therefore
         * survived both the colour brush and a WM_ERASEBKGND fill, and stayed
         * pale inside an otherwise dark box.
         *
         * So the background is painted here instead and user32's WM_PAINT is
         * never reached. Nothing is lost by that: everything a message box
         * shows - the icon, the text, the buttons - is a CHILD window that
         * paints itself, which is the same fact the "is this a message box"
         * test relies on.
         *
         * Restricted to message boxes. Our own dialogs keep the dialog
         * manager's paint, which they need. */
        struct kt_window *e = kt_find_window(hwnd);
        if (e && e->dark && e->msgbox && kt_back_brush) {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            if (dc) {
                RECT rc;
                GetClientRect(hwnd, &rc);
                FillRect(dc, &rc, kt_back_brush);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        break;
      }
      case WM_NCDESTROY:
        /* Both of these, or the table fills with dead handles and the next
         * window to be given a recycled HWND inherits a stale theme. */
        kitty_theme_forget(hwnd);
        RemoveWindowSubclass(hwnd, kt_dlg_subclass, 2);
        break;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

/*
 * Resolved once per themed window rather than read from a setting here: this
 * module has no idea where the preference is stored, and should not. The
 * owner of the preference installs the hook and hands the resolver in.
 */
static bool (*kt_want_dark)(void);

static LRESULT CALLBACK kt_cbt_proc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HCBT_ACTIVATE) {
        HWND w = (HWND)wParam;
        char cls[64];

        /* HCBT_ACTIVATE rather than HCBT_CREATEWND: by now the controls
         * exist, which both the message-box test and the child theming
         * need. */
        if (w && GetClassNameA(w, cls, sizeof(cls)) &&
            !strcmp(cls, "#32770") &&
            true) {
            bool dark = kt_want_dark ? kt_want_dark() : false;
            bool msgbox = kt_looks_like_messagebox(w);
            /* Ours, or a message box. A file dialog is this window class too,
             * but it is full of list views and toolbars so it fails the
             * message-box test and is not ours - and it is left alone, which
             * is right: the shell themes those itself. */
            if ((HINSTANCE)GetWindowLongPtr(w, GWLP_HINSTANCE) !=
                    GetModuleHandle(NULL) && !msgbox)
                return CallNextHookEx(kt_cbt_hook, code, wParam, lParam);
            struct kt_window *known = kt_find_window(w);

            /* ALWAYS, and before the early-out below. A dialog that themed
             * itself in WM_INITDIALOG is already in the table by the time it
             * activates, and skipping this would leave precisely that dialog
             * without the subclass that answers WM_CTLCOLOR. Repeat calls are
             * free - SetWindowSubclass replaces the entry with the same id. */
            SetWindowSubclass(w, kt_dlg_subclass, 2, 0);

            /* Re-theming on every activation would repaint a window each time
             * it is clicked, for nothing. */
            if (!known || known->dark != dark) {
                kitty_theme_apply(w, dark);
                known = kt_find_window(w);
            }
            if (known)
                known->msgbox = msgbox;
        }
    }
    return CallNextHookEx(kt_cbt_hook, code, wParam, lParam);
}

void kitty_theme_hook_dialogs(bool (*want_dark)(void))
{
    kt_want_dark = want_dark;
    if (kt_cbt_hook)
        return;
    /* Thread-local, not global: a global CBT hook is injected into every
     * process on the desktop, which is an enormous thing to do for a colour
     * scheme. This one only ever sees windows created on kageant's UI
     * thread. */
    kt_cbt_hook = SetWindowsHookEx(WH_CBT, kt_cbt_proc, NULL,
                                   GetCurrentThreadId());
}
