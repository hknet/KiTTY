/*
 * kitty_hello_host.c - the anchor host window of the Hello-protected keys:
 * the small always-on-top card that identifies WHO is asking for a Windows
 * Hello verification while the credential platform runs on the calling
 * thread. It lives on its own pumping thread (khw_anchor_*), is created and
 * destroyed by the credential code in kitty_hello.c through
 * kitty_hello_int.h, and carries the one-shot context line an app with
 * several instances sets (kitty_hello_set_context).
 */
#define COBJMACROS
#include <winsock2.h>   /* putty.h pulls it; it insists on preceding windows.h */
#include <windows.h>
#include <initguid.h>
#include <roapi.h>
#include <winstring.h>
#include <inspectable.h>
#include <asyncinfo.h>
#include <windows.security.credentials.ui.h>
#include <windows.security.credentials.h>
#include <windows.storage.streams.h>
#include <robuffer.h>

#include "putty.h"
#include "platform.h"   /* HandleWaitList: the nested WebAuthn pump must
                         * serve the app's handle waits (pipe accepts) */
#include "ssh.h"
#include "kitty_hello.h"
#include "kitty_foreground.h"

/* Shorten the ABI mouthfuls locally. */
#include "kitty_oldwin.h"   /* APIs newer than the oldest Windows we load on */
#include "kitty_text.h"     /* shared captions and feature names */
#include "kitty_hello_int.h"   /* what the credential code and this window share */

/*
 * The window the platform's credential UI attaches to. The UI presents
 * reliably only on a real, visible window holding the FOREGROUND -
 * with an arbitrary foreground window of another
 * process the NGC prompt sometimes never presents at all (camera on, no
 * UI, until the 120 s timeout). So each call gets a small visible host
 * window of our own, granted the foreground by briefly attaching to the
 * current foreground thread's input queue (the consent host's proven
 * trick). Destroyed when the call returns.
 */
/*
 * The anchor is drawn as a current-Windows card: borderless, rounded
 * (DWMWA_WINDOW_CORNER_PREFERENCE), acrylic behind it
 * (DWMWA_SYSTEMBACKDROP_TYPE = transient), dark-mode aware, Segoe UI
 * type with an accent-coloured Hello glyph and an animated status line.
 * Everything newer than base Win32 is loaded dynamically and skipped
 * where absent - a machine with Windows Hello has all of it anyway.
 * Text is drawn with DrawThemeTextEx(DTT_COMPOSITED): plain GDI text
 * would punch alpha holes into the DWM-composed surface.
 */
#include <dwmapi.h>
#include <uxtheme.h>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#endif
#define KHW_DWMWCP_ROUND 2
#define KHW_DWMSBT_TRANSIENT 3

typedef HRESULT (WINAPI *pDwmSetAttr_t)(HWND, DWORD, LPCVOID, DWORD);
typedef HRESULT (WINAPI *pDwmExtend_t)(HWND, const MARGINS *);
typedef HRESULT (WINAPI *pDwmColor_t)(DWORD *, BOOL *);
typedef HTHEME (WINAPI *pOpenTheme_t)(HWND, LPCWSTR);
typedef HRESULT (WINAPI *pCloseTheme_t)(HTHEME);
typedef HRESULT (WINAPI *pDrawThemeTextEx_t)(HTHEME, HDC, int, int, LPCWSTR,
                                             int, DWORD, RECT *,
                                             const DTTOPTS *);
typedef UINT (WINAPI *pGetDpiForWindow_t)(HWND);

static struct {
    int loaded;
    pDwmSetAttr_t SetAttr;
    pDwmExtend_t Extend;
    pDwmColor_t Color;
    pOpenTheme_t OpenTheme;
    pCloseTheme_t CloseTheme;
    pDrawThemeTextEx_t DrawTextEx;
    pGetDpiForWindow_t DpiForWindow;
} khw_ui;

static void khw_ui_load(void)
{
    if (khw_ui.loaded)
        return;
    khw_ui.loaded = 1;
    {
        HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
        if (dwm) {
            khw_ui.SetAttr = (pDwmSetAttr_t)
                kitty_api_from(dwm, "dwmapi.dll", "DwmSetWindowAttribute", KITTY_API_OPTIONAL,
                                  "dark title bars");
            khw_ui.Extend = (pDwmExtend_t)
                kitty_api_from(dwm, "dwmapi.dll", "DwmExtendFrameIntoClientArea", KITTY_API_OPTIONAL,
                                  "the glass frame on KiTTY dialogs");
            khw_ui.Color = (pDwmColor_t)
                kitty_api_from(dwm, "dwmapi.dll", "DwmGetColorizationColor", KITTY_API_OPTIONAL,
                                  "matching the desktop's accent colour");
        }
    }
    {
        HMODULE ux = LoadLibraryW(L"uxtheme.dll");
        if (ux) {
            khw_ui.OpenTheme = (pOpenTheme_t)
                kitty_api_from(ux, "uxtheme.dll", "OpenThemeData", KITTY_API_OPTIONAL,
                                  "themed text in KiTTY dialogs");
            khw_ui.CloseTheme = (pCloseTheme_t)
                kitty_api_from(ux, "uxtheme.dll", "CloseThemeData", KITTY_API_OPTIONAL,
                                  "themed text in KiTTY dialogs");
            khw_ui.DrawTextEx = (pDrawThemeTextEx_t)
                kitty_api_from(ux, "uxtheme.dll", "DrawThemeTextEx", KITTY_API_OPTIONAL,
                                  "themed text in KiTTY dialogs");
        }
    }
    khw_ui.DpiForWindow = (pGetDpiForWindow_t)
        kitty_api_from(GetModuleHandleW(L"user32.dll"), "user32.dll", "GetDpiForWindow", KITTY_API_OPTIONAL,
                                  "per-monitor DPI scaling");
}

/*
 * An anchor window on its OWN pumping thread: credential CREATION must
 * run on the calling thread (a worker gets RPC_E_WRONG_THREAD from the
 * platform), and while that thread blocks inside the call, the owner
 * window still has to live on a thread that answers messages - the
 * proven shape (a console window's conhost) reproduced in-process.
 */

/*
 * The context line: WHICH window/session is asking. Set (one-shot,
 * consumed by the next Hello operation) by an app that can have several
 * instances - the terminal - so the card identifies the asker. While a
 * context is set the card is ALWAYS shown, and it opens over the asking
 * window instead of the screen centre.
 */
static char khw_context[220];
static HWND khw_context_near;

void kitty_hello_set_context(const char *line, HWND near_window)
{
    if (line)
        snprintf(khw_context, sizeof(khw_context), "%s", line);
    else
        khw_context[0] = '\0';
    khw_context_near = near_window;
}


static DWORD WINAPI khw_anchor_thread(LPVOID p)
{
    struct khw_anchor *a = (struct khw_anchor *)p;
    MSG msg;
    a->hwnd = khw_host_create();
    SetEvent(a->ready);
    if (!a->hwnd)
        return 0;
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        if (msg.message == WM_QUIT)
            break;
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    khw_host_destroy(a->hwnd);
    return 0;
}

void khw_anchor_start(struct khw_anchor *a)
{
    memset(a, 0, sizeof(*a));
    a->ready = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!a->ready)
        return;
    a->thread = CreateThread(NULL, 0, khw_anchor_thread, a, 0, &a->tid);
    if (a->thread)
        WaitForSingleObject(a->ready, 10000);
}

void khw_anchor_stop(struct khw_anchor *a)
{
    if (a->thread) {
        PostThreadMessage(a->tid, WM_QUIT, 0, 0);
        WaitForSingleObject(a->thread, 5000);
        CloseHandle(a->thread);
    }
    if (a->ready)
        CloseHandle(a->ready);
}

/* Animated ellipsis state, one host at a time (khw_busy serialises). */
static int khw_host_phase;

static void khw_host_paint(HWND w)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(w, &ps);
    RECT rc;
    GetClientRect(w, &rc);
    {
        UINT dpi = khw_ui.DpiForWindow ? khw_ui.DpiForWindow(w) : 96;
        int px = (int)dpi;   /* scale helper: (v * px / 96) */
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HBITMAP oldbmp = (HBITMAP)SelectObject(mem, bmp);
        HTHEME th = khw_ui.OpenTheme ? khw_ui.OpenTheme(w, L"TEXTSTYLE")
                                     : NULL;

        /* Black = fully transparent to the acrylic backdrop. */
        {
            HBRUSH b = (HBRUSH)GetStockObject(BLACK_BRUSH);
            FillRect(mem, &rc, b);
        }

        /* Accent-coloured Hello glyph (fingerprint, Segoe Fluent/MDL2). */
        {
            DWORD argb = 0;
            BOOL opaque = FALSE;
            COLORREF accent = RGB(96, 205, 255);
            if (khw_ui.Color && SUCCEEDED(khw_ui.Color(&argb, &opaque)))
                accent = RGB((argb >> 16) & 0xff, (argb >> 8) & 0xff,
                             argb & 0xff);
            if (th && khw_ui.DrawTextEx) {
                static const WCHAR glyph[] = { 0xE928, 0 };
                HFONT f = CreateFontW(-(36 * px / 96), 0, 0, 0, FW_NORMAL,
                                      FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                      CLEARTYPE_QUALITY, DEFAULT_PITCH,
                                      L"Segoe Fluent Icons");
                HFONT of = (HFONT)SelectObject(mem, f);
                DTTOPTS o;
                RECT gr = rc;
                memset(&o, 0, sizeof(o));
                o.dwSize = sizeof(o);
                o.dwFlags = DTT_COMPOSITED | DTT_TEXTCOLOR;
                o.crText = accent;
                gr.left = 22 * px / 96;
                gr.top = 20 * px / 96;
                khw_ui.DrawTextEx(th, mem, 0, 0, glyph, -1,
                                  DT_LEFT | DT_TOP | DT_SINGLELINE, &gr, &o);
                SelectObject(mem, of);
                DeleteObject(f);
            }
        }

        /* Title + animated status line. */
        if (th && khw_ui.DrawTextEx) {
            static const WCHAR *dots[] = { L"", L".", L"..", L"..." };
            WCHAR status[64];
            DTTOPTS o;
            RECT tr = rc;
            HFONT f1 = CreateFontW(-(15 * px / 96), 0, 0, 0, FW_SEMIBOLD,
                                   FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                   OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   CLEARTYPE_QUALITY, DEFAULT_PITCH,
                                   L"Segoe UI Variable Display");
            HFONT f2 = CreateFontW(-(12 * px / 96), 0, 0, 0, FW_NORMAL,
                                   FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                   OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   CLEARTYPE_QUALITY, DEFAULT_PITCH,
                                   L"Segoe UI");
            HFONT of = (HFONT)SelectObject(mem, f1);
            memset(&o, 0, sizeof(o));
            o.dwSize = sizeof(o);
            o.dwFlags = DTT_COMPOSITED | DTT_TEXTCOLOR;
            o.crText = RGB(255, 255, 255);
            tr.left = 76 * px / 96;
            tr.top = 22 * px / 96;
            khw_ui.DrawTextEx(th, mem, 0, 0, L"Windows Hello", -1,
                              DT_LEFT | DT_TOP | DT_SINGLELINE, &tr, &o);
            SelectObject(mem, f2);
            o.crText = RGB(190, 190, 190);
            tr.top = 48 * px / 96;
            wsprintfW(status, L"Authentication processing%s",
                      dots[khw_host_phase & 3]);
            khw_ui.DrawTextEx(th, mem, 0, 0, status, -1,
                              DT_LEFT | DT_TOP | DT_SINGLELINE, &tr, &o);
            if (khw_context[0]) {
                WCHAR wctx[220];
                MultiByteToWideChar(CP_ACP, 0, khw_context, -1, wctx, 220);
                o.crText = RGB(150, 150, 150);
                tr.top = 66 * px / 96;
                khw_ui.DrawTextEx(th, mem, 0, 0, wctx, -1,
                                  DT_LEFT | DT_TOP | DT_SINGLELINE |
                                  DT_END_ELLIPSIS, &tr, &o);
            }
            SelectObject(mem, of);
            DeleteObject(f1);
            DeleteObject(f2);
        }

        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        if (th && khw_ui.CloseTheme)
            khw_ui.CloseTheme(th);
        SelectObject(mem, oldbmp);
        DeleteObject(bmp);
        DeleteDC(mem);
    }
    EndPaint(w, &ps);
}

static LRESULT CALLBACK khw_host_wndproc(HWND w, UINT msg, WPARAM wp,
                                         LPARAM lp)
{
    switch (msg) {
      case WM_PAINT:
        khw_host_paint(w);
        return 0;
      case WM_TIMER:
        khw_host_phase++;
        InvalidateRect(w, NULL, FALSE);
        return 0;
      case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

HWND khw_host_create(void)
{
    static ATOM cls = 0;
    HWND w;
    RECT rc;
    UINT dpi;
    int cx, cy;

    khw_ui_load();
    if (!cls) {
        WNDCLASSW wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = khw_host_wndproc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.lpszClassName = L"KiTTYHelloKeyHost";
        cls = RegisterClassW(&wc);
        if (!cls)
            return NULL;
    }
    SystemParametersInfoA(SPI_GETWORKAREA, 0, &rc, 0);
    if (khw_context_near && IsWindow(khw_context_near) &&
        IsWindowVisible(khw_context_near)) {
        RECT nr;
        if (GetWindowRect(khw_context_near, &nr) &&
            nr.right > nr.left && nr.bottom > nr.top) {
            /* Open over the ASKING window - with several instances the
             * card must say and show where the question comes from. */
            rc = nr;
        }
    }
    dpi = 96;
    cx = 340;
    cy = khw_context[0] ? 112 : 96;
    w = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"KiTTYHelloKeyHost",
                        L"Windows Hello", WS_POPUP,
                        (rc.left + rc.right - cx) / 2,
                        (rc.top + rc.bottom - cy) / 2, cx, cy,
                        NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (!w)
        return NULL;
    if (khw_ui.DpiForWindow) {
        dpi = khw_ui.DpiForWindow(w);
        if (dpi != 96) {
            cx = cx * (int)dpi / 96;
            cy = cy * (int)dpi / 96;
            SetWindowPos(w, NULL, (rc.left + rc.right - cx) / 2,
                         (rc.top + rc.bottom - cy) / 2, cx, cy,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
    if (khw_ui.SetAttr) {
        BOOL dark = TRUE;
        DWORD corner = KHW_DWMWCP_ROUND;
        DWORD backdrop = KHW_DWMSBT_TRANSIENT;
        khw_ui.SetAttr(w, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark,
                       sizeof(dark));
        khw_ui.SetAttr(w, DWMWA_WINDOW_CORNER_PREFERENCE, &corner,
                       sizeof(corner));
        khw_ui.SetAttr(w, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop,
                       sizeof(backdrop));
    }
    if (khw_ui.Extend) {
        MARGINS m = { -1, -1, -1, -1 };
        khw_ui.Extend(w, &m);
    }
    khw_host_phase = 0;
    SetTimer(w, 1, 400, NULL);
    ShowWindow(w, SW_SHOWNOACTIVATE);
    kitty_force_foreground(w);
    UpdateWindow(w);
    return w;
}

void khw_host_destroy(HWND w)
{
    if (w && IsWindow(w))
        DestroyWindow(w);
    /* one-shot: the context belongs to the operation that just ended */
    khw_context[0] = '\0';
    khw_context_near = NULL;
}

/* Is the caller's window good enough to anchor the credential UI - a
 * visible window of THIS process? Then no extra window appears (the
 * interactive flows: a dialog or main window is right there). The
 * anchor host is only for the windowless moments - agent startup,
 * tray-only unlocks - where it is also the visible sign of WHO is
 * asking for the verification. */
bool khw_owner_usable(HWND w)
{
    DWORD pid = 0;
    if (khw_context[0])
        return false;    /* a context is set: ALWAYS show the card - the
                          * identification is its purpose */
    return w && IsWindow(w) && IsWindowVisible(w) &&
           GetWindowThreadProcessId(w, &pid) &&
           pid == GetCurrentProcessId();
}
