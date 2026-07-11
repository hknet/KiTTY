#ifndef KITTY_AUXPOS_H
#define KITTY_AUXPOS_H
#include <windows.h>

/* Self-contained (Win32 + registry only, no KiTTY deps) position memory +
 * DPI/multi-monitor-safe placement for pop-up dialogs (About boxes, etc.),
 * shared across kitty, kageant, kittygen and the other suite exes.
 *
 *   kitty_auxpos_apply(dlg,key)  - call on WM_INITDIALOG: restore the remembered
 *                                  position for the current monitor topology, or
 *                                  centre over the owner/parent if none/off-screen.
 *   kitty_auxpos_save(dlg,key)   - call on WM_DESTROY: remember the position.
 *
 * Positions are keyed by <key>_<monitor-topology-hash> under
 * HKCU\Software\kapper.net\KiTTY\AuxWinPos, so a docked multi-monitor layout and
 * a single screen each keep their own spot; a restored point is validated to be
 * on a currently-visible monitor first. Only the top-left is stored (dialogs keep
 * their template size). Persistence can be turned off (portable mode) with
 * kitty_auxpos_set_persist(0) - placement (centring) still works. */

/* anchor  = the window the dialog was invoked from (its owner); may be NULL.
 * near_tray = 1 for tray apps (kageant/launcher): first-open placement goes to
 *             the notification-area corner of the anchor's monitor instead of
 *             centring over a window. Remembered positions still take priority. */
void kitty_auxpos_apply(HWND dlg, const char *key, HWND anchor, int near_tray);
void kitty_auxpos_save(HWND dlg, const char *key);
void kitty_auxpos_set_persist(int on);   /* default on; 0 => place only, no registry */

#endif
