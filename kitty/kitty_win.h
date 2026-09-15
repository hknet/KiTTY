/*
 * kitty_win.h - the declarations for kitty_win.c, the Win32 glue that stays
 * with the terminal window: the inline terminal messages, the terminal
 * system-menu actions, the window-title placeholder window, the
 * missing-features report, and the application-wide theme and
 * check-for-updates settings. The pickers, printing, the clipboard and
 * process launching are in kitty_winutil.h, the themed boxes in
 * kitty_dlgbox.h, the in-app updater in kitty_updater.h.
 */
#ifndef KITTY_WIN
#define KITTY_WIN

#include <stdlib.h>
#include <stdio.h>
#include "putty.h"
#include <windows.h>

// Inline (non-modal) rendering of connection errors in the terminal
// (upstream cyd01/KiTTY #548)
void kitty_term_print_inline_error(Terminal *term, const char *msg, int fatal) ;
void kitty_print_session_comment(Terminal *term, Conf *conf) ;   /* framed Comment at session start */

/* One line in the terminal, and the whole list in the Event Log, naming what
 * this version of Windows is too old to provide. Once per process; silent when
 * nothing is missing, and switched off with [KiTTY] warnmissingfeatures=no.
 * See kitty_oldwin.h. */
void kitty_report_missing_features(Terminal *term) ;

// Bodies of the system-menu commands (window.c WM_COMMAND), with no
// dependency on window.c's statics
void kitty_menu_adjust_transparency(HWND term_hwnd, Conf *conf, int up) ;
void kitty_menu_toggle_alwaysontop(HWND term_hwnd, Conf *conf) ;
void kitty_menu_reposition(HWND term_hwnd, Conf *conf, int x, int y) ;

// [ConfigBox] noexit: start a fresh instance (= the config box) when a
// session closes
void kitty_respawn_config_box(void) ;

// Reapply the title decorations ([KiTTY] wintitle/size, PROTECTED/ONTOP)
// - windows/window.c
void kitty_refresh_title(void) ;
void kitty_menu_toggle_hyperlink(HWND hwnd) ;

// KiTTY: the application-wide colour theme, [KiTTY] theme. kitty_theme_app_dark
// is the resolver handed to kitty_theme_hook_dialogs() in WinMain.
int kitty_theme_app_pref( void ) ;
void kitty_theme_app_pref_forget( void ) ;   /* after writing [KiTTY] theme */
bool kitty_theme_app_dark( void ) ;
// KiTTY: the update check, an application setting in kitty.ini ([KiTTY]
// checkupdate, default on). It used to be per-session, which meant the answer
// depended on which session opened first. See kitty_win.c.
int kitty_check_update_enabled( void ) ;
void kitty_set_check_update_enabled( int on ) ;

/* ---- exported from kitty/kitty_win.c ---- */
int kitty_autopw_warn( void );
void kitty_show_title_placeholders(HWND owner);
void kitty_sync_transparency_menu(HMENU menu, Conf *conf, UINT id_up, UINT id_down, UINT id_anchor);

#endif
