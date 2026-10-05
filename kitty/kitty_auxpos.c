/*
 * kitty_auxpos.c - self-contained (Win32 only) position and size memory and
 * DPI/multi-monitor-safe placement for the suite's own windows (About boxes,
 * the configuration window, Organize sessions, kageant's key list and agent
 * log, ...), shared across kitty, kageant, kittygen and the rest of the
 * suite. See kitty_auxpos.h for the contract.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "kitty_auxpos.h"

#define KITTY_AUXPOS_REGKEY  "Software\\kapper.net\\KiTTY\\AuxWinPos"
#define KITTY_AUXPOS_SECTION "AuxWinPos"   /* the file store's section */

static int kitty_auxpos_persist = 1;       /* 0 => place only, nothing stored */
static char kitty_auxpos_file[MAX_PATH];   /* set => this file, not the registry */
void kitty_auxpos_set_persist(int on) { kitty_auxpos_persist = on ? 1 : 0; }

void kitty_auxpos_set_file_beside_exe(void)
{
    char *slash;
    DWORD n = GetModuleFileNameA(NULL, kitty_auxpos_file, MAX_PATH);
    if (n == 0 || n >= MAX_PATH || !(slash = strrchr(kitty_auxpos_file, '\\'))) {
        kitty_auxpos_file[0] = '\0';
        kitty_auxpos_persist = 0;          /* nowhere to keep it: place only */
        return;
    }
    if ((size_t)(slash + 1 - kitty_auxpos_file) + strlen(KITTY_AUXPOS_FILE) + 1 > MAX_PATH) {
        kitty_auxpos_file[0] = '\0';
        kitty_auxpos_persist = 0;
        return;
    }
    strcpy(slash + 1, KITTY_AUXPOS_FILE);
    kitty_auxpos_persist = 1;
}

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

/* The display scale (system DPI; 1.0 = 96): sizes are kept in logical pixels
 * so a window fits a scaled and an unscaled display alike. */
static double kitty_auxpos_scale(void)
{
    HDC dc = GetDC(NULL);
    double s = 1.0;
    if (dc) {
        int dpi = GetDeviceCaps(dc, LOGPIXELSX);
        if (dpi > 0) s = dpi / 96.0;
        ReleaseDC(NULL, dc);
    }
    return s;
}

/* ---- the store: "x,y" or "x,y,w,h" (w,h logical), registry or file ---- */

/* 1 = position read (*w,*h 0 when none stored), 0 = no entry */
static int kitty_auxpos_read(const char *key, int *x, int *y, int *w, int *h)
{
    char vn[160], buf[96];
    *w = *h = 0;
    if (!kitty_auxpos_persist) return 0;
    kitty_auxpos_valname(vn, sizeof(vn), key);
    if (kitty_auxpos_file[0]) {
        GetPrivateProfileStringA(KITTY_AUXPOS_SECTION, vn, "", buf, sizeof(buf),
                                 kitty_auxpos_file);
    } else {
        HKEY hk;
        DWORD type = 0, sz = sizeof(buf) - 1;
        buf[0] = '\0';
        if (RegOpenKeyExA(HKEY_CURRENT_USER, KITTY_AUXPOS_REGKEY, 0, KEY_READ, &hk)
            != ERROR_SUCCESS)
            return 0;
        if (RegQueryValueExA(hk, vn, NULL, &type, (LPBYTE)buf, &sz) != ERROR_SUCCESS) {
            RegCloseKey(hk);
            return 0;
        }
        RegCloseKey(hk);
        if (type == REG_BINARY && sz == sizeof(POINT)) {
            /* written by an earlier version: the top-left only */
            POINT pt;
            memcpy(&pt, buf, sizeof(pt));
            *x = pt.x; *y = pt.y;
            return 1;
        }
        if (type != REG_SZ) return 0;
        buf[sz] = '\0';
    }
    if (!buf[0]) return 0;
    {
        int n = sscanf(buf, "%d,%d,%d,%d", x, y, w, h);
        if (n < 2) return 0;
        if (n < 4 || *w <= 0 || *h <= 0) *w = *h = 0;
    }
    return 1;
}

static void kitty_auxpos_write(const char *key, int x, int y, int w, int h)
{
    char vn[160], val[96];
    if (!kitty_auxpos_persist) return;
    kitty_auxpos_valname(vn, sizeof(vn), key);
    if (w > 0 && h > 0) snprintf(val, sizeof(val), "%d,%d,%d,%d", x, y, w, h);
    else snprintf(val, sizeof(val), "%d,%d", x, y);
    if (kitty_auxpos_file[0]) {
        WritePrivateProfileStringA(KITTY_AUXPOS_SECTION, vn, val, kitty_auxpos_file);
    } else {
        HKEY hk;
        if (RegCreateKeyExA(HKEY_CURRENT_USER, KITTY_AUXPOS_REGKEY, 0, NULL, 0,
                            KEY_WRITE, NULL, &hk, NULL) == ERROR_SUCCESS) {
            RegSetValueExA(hk, vn, 0, REG_SZ, (const BYTE*)val, (DWORD)strlen(val) + 1);
            RegCloseKey(hk);
        }
    }
}

/* ---- placement ---- */

/* Move dlg so its top-left is (x,y) and, with w,h > 0, give it that size -
 * the size capped to and the whole window kept inside the work area of the
 * nearest monitor (never behind the taskbar / off-screen). */
static void kitty_auxpos_place_rect(HWND dlg, int x, int y, int w, int h)
{
    RECT rd, want; MONITORINFO mi; HMONITOR mon;
    UINT flags = SWP_NOZORDER | SWP_NOACTIVATE;
    GetWindowRect(dlg, &rd);
    if (w <= 0 || h <= 0) {
        w = rd.right - rd.left; h = rd.bottom - rd.top;
        flags |= SWP_NOSIZE;
    }
    SetRect(&want, x, y, x + w, y + h);
    mon = MonitorFromRect(&want, MONITOR_DEFAULTTONEAREST);
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfo(mon, &mi)) {
        RECT wk = mi.rcWork;
        if (!(flags & SWP_NOSIZE)) {
            if (w > wk.right - wk.left) w = wk.right - wk.left;
            if (h > wk.bottom - wk.top) h = wk.bottom - wk.top;
        }
        if (x > wk.right  - w) x = wk.right  - w;
        if (y > wk.bottom - h) y = wk.bottom - h;
        if (x < wk.left) x = wk.left;
        if (y < wk.top)  y = wk.top;
    }
    SetWindowPos(dlg, NULL, x, y, w, h, flags);
}

int kitty_auxpos_restore(HWND dlg, const char *key, int sized, int minw, int minh)
{
    int x, y, w, h;
    if (!kitty_auxpos_read(key, &x, &y, &w, &h)) return 0;
    if (sized && w > 0 && h > 0) {
        double s = kitty_auxpos_scale();
        w = (int)(w * s + 0.5);
        h = (int)(h * s + 0.5);
        if (w < minw) w = minw;
        if (h < minh) h = minh;
    } else {
        w = h = 0;                         /* the window keeps its own size */
    }
    kitty_auxpos_place_rect(dlg, x, y, w, h);
    return 1;
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

    /* 1) A remembered position for this monitor topology wins. */
    if (kitty_auxpos_restore(dlg, key, 0, 0, 0))
        return;

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
    kitty_auxpos_place_rect(dlg, x, y, 0, 0);
}

void kitty_auxpos_save(HWND dlg, const char *key)
{
    RECT rc;
    double s;
    if (!kitty_auxpos_persist) return;
    if (!GetWindowRect(dlg, &rc)) return;
    if (IsIconic(dlg) || IsZoomed(dlg)) return;
    s = kitty_auxpos_scale();
    kitty_auxpos_write(key, rc.left, rc.top,
                       (int)((rc.right - rc.left) / s + 0.5),
                       (int)((rc.bottom - rc.top) / s + 0.5));
}

int kitty_auxpos_seed(const char *key, int x, int y, int w, int h)
{
    int ox, oy, ow, oh;
    double s;
    if (!kitty_auxpos_persist) return 0;
    if (kitty_auxpos_read(key, &ox, &oy, &ow, &oh)) return 0;   /* has one */
    s = kitty_auxpos_scale();
    kitty_auxpos_write(key, x, y,
                       w > 0 ? (int)(w / s + 0.5) : 0,
                       h > 0 ? (int)(h / s + 0.5) : 0);
    return 1;
}
