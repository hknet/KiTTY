/*
 * kitty_serialmenu.h - the terminal window's Serial submenu and Find speed
 * (hknet/KiTTY#86). Called from windows/window.c, which hands over the
 * window's pieces; nothing here reaches into the window itself.
 *
 * The submenu is there while the session's protocol is Serial: the speeds
 * (the one in force ticked), Find speed, and the ports detected (the one in
 * use ticked). A speed applies to the open port at once (the silent
 * reconfiguration); a port restarts the session on it.
 */
#ifndef KITTY_SERIALMENU_H
#define KITTY_SERIALMENU_H

#include "putty.h"

/* Put the Serial submenu into the window's menus, or take it out: after the
 * Special Command submenu changed (win_seat_update_specials_menu). `before`
 * is the command it goes in front of; `own_separator` adds a separator
 * after it (when `before` is not a separator already). */
void kitty_serial_menu_update(HMENU *menus, int nmenus, Conf *conf,
                              UINT before, bool own_separator);

/* WM_INITMENUPOPUP: when `m` is the Serial submenu, fill it for the moment
 * (ports present now, the check marks) and return true. */
bool kitty_serial_menu_popup(HMENU m, Conf *conf, Backend *be);

/* WM_COMMAND / WM_SYSCOMMAND with an id of the IDM_SERIAL_MIN range: run it
 * and return true; false for any other id. */
bool kitty_serial_menu_command(HWND hwnd, WPARAM id, Conf *conf, Backend *be,
                               Terminal *term, LogContext *logctx);

/* After the back end took a new Conf (IDM_RECONF): the serial settings the
 * port refused are put back to the ones in force, so the Conf, the check
 * marks and Change Settings show what the port runs at. */
void kitty_serial_after_reconfig(Conf *conf, Backend *be);

#endif
