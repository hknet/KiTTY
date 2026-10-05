/*
 * kitty_auxpos.h - position and size memory and safe placement for the
 * suite's own windows (kitty_auxpos.c): restore where a window was last left
 * on this monitor layout, or place it sensibly when there is nothing
 * remembered. Win32 only, shared across the suite's executables.
 */
#ifndef KITTY_AUXPOS_H
#define KITTY_AUXPOS_H
#include <windows.h>

/* Self-contained (Win32 only, no KiTTY deps) memory + DPI/multi-monitor-safe
 * placement for dialogs and tool windows (About boxes, the configuration
 * window, Organize sessions, kageant's key list and agent log, ...), shared
 * across kitty, kageant, kittygen and the other suite exes. NOT for terminal
 * windows: those keep their columns x rows (kitty_winpos.c).
 *
 *   kitty_auxpos_apply(dlg,key,...) - on WM_INITDIALOG for a window that keeps
 *                                  its template size: the remembered position,
 *                                  else centre over the anchor (or by the tray).
 *   kitty_auxpos_restore(dlg,key,sized,minw,minh)
 *                                - the remembered position (and, with sized,
 *                                  the size, kept at least minw x minh window
 *                                  pixels); 1 when applied, 0 when nothing is
 *                                  remembered - the caller places the window.
 *   kitty_auxpos_save(dlg,key)   - on WM_DESTROY: position and size, not while
 *                                  minimised or maximised.
 *   kitty_auxpos_seed(key,...)   - carry an older stored value over once: written
 *                                  only when the key has no entry yet (1).
 *
 * Entries are keyed <key>_<monitor-topology-hash>, so a docked multi-monitor
 * layout and a single screen each keep their own; the value is "x,y,w,h"
 * (w,h in logical pixels, 96 DPI). A restored window is always moved fully
 * onto the work area of the nearest monitor, its size capped to it. Stored
 * under HKCU\Software\kapper.net\KiTTY\AuxWinPos (kitty.exe: AuxWinPos under
 * the hive in use, kitty_auxpos_set_regbase) - or, for a portable copy,
 * in [AuxWinPos] of KITTY_AUXPOS_FILE beside the exe
 * (kitty_auxpos_set_file_beside_exe, called at start-up). */

#define KITTY_AUXPOS_FILE "kitty_windowpos.ini"

/* anchor  = the window the dialog was invoked from (its owner); may be NULL.
 * near_tray = 1 for tray apps (kageant/launcher): first-open placement goes to
 *             the notification-area corner of the anchor's monitor instead of
 *             centring over a window. Remembered positions still take priority. */
void kitty_auxpos_apply(HWND dlg, const char *key, HWND anchor, int near_tray);
int kitty_auxpos_restore(HWND dlg, const char *key, int sized, int minw, int minh);
void kitty_auxpos_save(HWND dlg, const char *key);
/* x,y,w,h in window pixels of the current display (w,h 0 = position only) */
int kitty_auxpos_seed(const char *key, int x, int y, int w, int h);
void kitty_auxpos_set_persist(int on);   /* default on; 0 => place only, nothing stored */
void kitty_auxpos_set_file_beside_exe(void);   /* portable: the file, not the registry */
void kitty_auxpos_set_regbase(const char *base);   /* <base>\AuxWinPos, not kapper.net's */

#endif
