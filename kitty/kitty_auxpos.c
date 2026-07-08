/*
 * kitty_auxpos.c - self-contained (Win32 + registry only) position memory and
 * DPI/multi-monitor-safe placement for pop-up dialogs (About boxes, etc.),
 * shared across kitty, kageant, kittygen and the rest of the suite. See
 * kitty_auxpos.h for the contract.
 */
#include <windows.h>
#include <stdio.h>
#include "kitty_auxpos.h"

#define KITTY_AUXPOS_REGKEY "Software\\kapper.net\\KiTTY\\AuxWinPos"

static int kitty_auxpos_persist = 1;   /* off in portable mode -> place only */
void kitty_auxpos_set_persist(int on) { kitty_auxpos_persist = on ? 1 : 0; }

/* Order-independent FNV-1a over each monitor's rcMonitor, summed - so a docked
 * multi-monitor layout and a single screen produce distinct, stable keys. */
static unsigned long kitty_auxpos_topo_sum;
static BOOL CALLBACK kitty_auxpos_mon_cb(HMONITOR mon, HDC dc, LPRECT r, LPARAM lp)
{
    MONITORINFO mi; mi.cbSize = sizeof(mi);
    if (GetMonitorInfo(mon, &mi)) {
        unsigned long h = 2166136261UL;
        const unsigned char *p = (const unsigned char*)&mi.rcMonitor;
        size_t i;
        for (i = 0; i < sizeof(mi.rcMonitor); i++) { h ^= p[i]; h *= 16777619UL; }
        kitty_auxpos_topo_sum += h;
    }
    (void)dc; (void)r; (void)lp;
    return TRUE;
}
static unsigned long kitty_auxpos_topo(void)
{
    kitty_auxpos_topo_sum = 0;
    EnumDisplayMonitors(NULL, NULL, kitty_auxpos_mon_cb, 0);
    return kitty_auxpos_topo_sum;
}
static void kitty_auxpos_valname(char *out, int n, const char *key)
{
    snprintf(out, n, "%s_%08lx", key, kitty_auxpos_topo());
}

/* Move dlg so its top-left is (x,y), clamped inside the work area of the monitor
 * nearest that point (never behind the taskbar / off-screen). Size unchanged. */
static void kitty_auxpos_place(HWND dlg, int x, int y)
{
    RECT rd; MONITORINFO mi; HMONITOR mon; POINT pt;
    int dw, dh;
    GetWindowRect(dlg, &rd);
    dw = rd.right - rd.left; dh = rd.bottom - rd.top;
    pt.x = x; pt.y = y;
    mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    mi.cbSize = sizeof(mi); GetMonitorInfo(mon, &mi);
    if (x < mi.rcWork.left) x = mi.rcWork.left;
    if (y < mi.rcWork.top)  y = mi.rcWork.top;
    if (x > mi.rcWork.right  - dw) x = mi.rcWork.right  - dw;
    if (y > mi.rcWork.bottom - dh) y = mi.rcWork.bottom - dh;
    SetWindowPos(dlg, NULL, x, y, 0, 0, SWP_NOZORDER | SWP_NOSIZE);
}

/* Is a saved top-left point still on a currently-visible monitor? */
static int kitty_auxpos_on_screen(POINT pt)
{
    return MonitorFromPoint(pt, MONITOR_DEFAULTTONULL) != NULL;
}

/* Resolve the anchor's window rect (falls back to the foreground/desktop). */
static void kitty_auxpos_anchor_rect(HWND anchor, RECT *out)
{
    if (anchor && IsWindow(anchor) && !IsIconic(anchor)) { GetWindowRect(anchor, out); return; }
    { HWND fg = GetForegroundWindow();
      if (fg && IsWindow(fg)) { GetWindowRect(fg, out); return; } }
    GetWindowRect(GetDesktopWindow(), out);
}

void kitty_auxpos_apply(HWND dlg, const char *key, HWND anchor, int near_tray)
{
    RECT rd, ra; MONITORINFO mi; HMONITOR mon; int dw, dh, x, y;

    /* 1) A remembered position for this monitor topology wins (if on-screen). */
    if (kitty_auxpos_persist) {
        char vn[160]; POINT pt; DWORD sz = sizeof(pt), type = 0; HKEY hk;
        kitty_auxpos_valname(vn, sizeof(vn), key);
        if (RegOpenKeyExA(HKEY_CURRENT_USER, KITTY_AUXPOS_REGKEY, 0, KEY_READ, &hk) == ERROR_SUCCESS) {
            LONG r = RegQueryValueExA(hk, vn, NULL, &type, (LPBYTE)&pt, &sz);
            RegCloseKey(hk);
            if (r == ERROR_SUCCESS && type == REG_BINARY && sz == sizeof(pt) &&
                kitty_auxpos_on_screen(pt)) {
                kitty_auxpos_place(dlg, pt.x, pt.y);
                return;
            }
        }
    }

    /* 2) First-open placement. */
    GetWindowRect(dlg, &rd);
    dw = rd.right - rd.left; dh = rd.bottom - rd.top;
    kitty_auxpos_anchor_rect(anchor, &ra);
    mon = MonitorFromRect(&ra, MONITOR_DEFAULTTONEAREST);
    mi.cbSize = sizeof(mi); GetMonitorInfo(mon, &mi);

    if (near_tray) {
        /* Around the corner from the notification area: the bottom-right of the
         * work area OF THE MONITOR THAT HOSTS THE TASKBAR (so it lands by the tray
         * even on a multi-monitor setup), inset by a small margin. */
        HWND tray = FindWindowA("Shell_TrayWnd", NULL);
        HMONITOR tmon = tray ? MonitorFromWindow(tray, MONITOR_DEFAULTTOPRIMARY)
                             : MonitorFromRect(&ra, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO tmi; tmi.cbSize = sizeof(tmi);
        GetMonitorInfo(tmon, &tmi);
        int margin = 16;
        x = tmi.rcWork.right  - dw - margin;
        y = tmi.rcWork.bottom - dh - margin;
    } else {
        /* Centre over the anchor window. */
        x = ra.left + ((ra.right - ra.left) - dw) / 2;
        y = ra.top  + ((ra.bottom - ra.top) - dh) / 2;
    }
    kitty_auxpos_place(dlg, x, y);
}

/* A GUI font scaled to `ref`'s monitor DPI, for hand-built pop-up windows (update
 * popup, launcher About) that -- unlike dialog-resource boxes -- get no automatic
 * font scaling from the dialog manager. Without this the window is DPI-sized but the
 * text stays at 96-dpi (tiny). Pass the OWNER window (the same one whose DPI drives
 * the layout), not the freshly-created pop-up (which may still sit at 0,0 on another
 * monitor). The caller owns the result: DeleteObject() it on WM_DESTROY (harmless if
 * it's the stock fallback). */
HFONT kitty_auxpos_gui_font(HWND ref)
{
    int dpi = 96;
    LOGFONT lf;
    HFONT stock = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HMODULE u = GetModuleHandleA("user32.dll");
    if (u) {
        UINT (WINAPI *pf)(HWND) = (UINT(WINAPI*)(HWND))GetProcAddress(u, "GetDpiForWindow");
        if (pf && ref) { UINT d = pf(ref); if (d) dpi = (int)d; }
    }
    if (dpi != 96 && GetObject(stock, sizeof(lf), &lf)) {
        HFONT f;
        lf.lfHeight = MulDiv(lf.lfHeight, dpi, 96);
        lf.lfWidth  = 0;   /* derive width from the scaled height */
        f = CreateFontIndirect(&lf);
        if (f) return f;
    }
    return stock;   /* 96 dpi or failure: stock GUI font (DeleteObject is a safe no-op) */
}

void kitty_auxpos_save(HWND dlg, const char *key)
{
    char vn[160]; RECT rc; POINT pt; HKEY hk;
    if (!kitty_auxpos_persist) return;
    if (!GetWindowRect(dlg, &rc)) return;
    if (IsIconic(dlg) || IsZoomed(dlg)) return;
    pt.x = rc.left; pt.y = rc.top;
    kitty_auxpos_valname(vn, sizeof(vn), key);
    if (RegCreateKeyExA(HKEY_CURRENT_USER, KITTY_AUXPOS_REGKEY, 0, NULL, 0,
                        KEY_WRITE, NULL, &hk, NULL) == ERROR_SUCCESS) {
        RegSetValueExA(hk, vn, 0, REG_BINARY, (const BYTE*)&pt, sizeof(pt));
        RegCloseKey(hk);
    }
}
