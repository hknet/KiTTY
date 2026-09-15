/*
 * kitty_win.h - the declarations for kitty_win.c, the grab-bag of Win32
 * helpers: window transparency and OS version, the file and folder pickers,
 * printing, the clipboard, launching a process, the update check and its
 * notices, the shared dialog helpers (icon, centring, fit-to-text), the
 * inline terminal messages, the terminal system-menu actions, and the
 * application-wide theme and check-for-updates settings.
 */
#ifndef KITTY_WIN
#define KITTY_WIN

#include <stdlib.h>
#include <stdio.h>
#include "putty.h"
#include <windows.h>

void SetTransparency( HWND hwnd, int value ) ;
void GetOSInfo( char * version ) ;

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
BOOL IsWow64() ; // Test whether we are on 64-bit Windows
int OpenFileName( HWND hFrame, char * filename, char * Title, char * Filter ) ;
/* The same picker opened in a given folder (NULL or empty = wherever
 * Windows would open it). */
int OpenFileNameFrom( HWND hFrame, char * filename, char * Title, char * Filter, const char * initialdir ) ;
int OpenDirName( HWND hFrame, char * dirname ) ;
/* Same picker, opened on `initial` (when that folder exists) with `title`
 * (NULL = the default caption). */
int OpenDirNameFrom( HWND hFrame, char * dirname, const char * initial, const char * title ) ;
/* Centre a modal dialog over its owner (WM_INITDIALOG, after final size). */
void kitty_centre_on_owner( HWND dlg ) ;
/* Give a dialog the caption and taskbar icon of the window that raised it -
 * which is the SESSION's own icon when that session carries one. `owner`
 * NULL = the dialog's owner. Called from WM_INITDIALOG. */
void kitty_dialog_icon( HWND dlg, HWND owner ) ;
/* Grow one static control to fit `text` at the dialog's font, moved down by
 * extra_dy; returns the height change in pixels (the caller moves what sits
 * below and grows the window). Empty text collapses and hides the control. */
int kitty_fit_text( HWND dlg, int ctlid, const char *text, int extra_dy ) ;
int SaveFileName( HWND hFrame, char * filename, char * Title, char * Filter ) ;
int SaveFileNameFrom( HWND hFrame, char * filename, char * Title, char * Filter, const char * initialdir ) ;
	
// Centre a dialog in the middle of its parent window
void CenterDlgInParent(HWND hDlg) ;

// Send to the printer
int PrintText( const char * Text ) ;

// Print the text in the notepad
void ManagePrint( HWND hwnd ) ;

// Put a text into the clipboard
int SetTextToClipboard( const char * buf ) ;

// Run a command
void RunCommand( HWND hwnd, const char * cmd ) ;

// Start the built-in editor
void RunPuttyEd( HWND hwnd, char * filename ) ;

// Check whether an update is available on the web site
void CheckVersionFromWebSite( HWND hwnd, int is_terminal ) ;

// KiTTY: background (async) update check that caches the latest version, and a
// session-start notice rendered from that cache (see window.c).
void kitty_start_update_check( void ) ;
int kitty_update_notice( char *buf, int n ) ;
int kitty_update_available( char *latest_out, int latest_n, char *cur_out, int cur_n, int *beta_out ) ;

// Display a message in the event log
void debug_logevent( const char *fmt, ... ) ;

// Test whether a path is absolute
bool IsPathAbsolute( const char * path ) ;

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
int kitty_confirm_box( HWND owner, const char *caption, const char *text, const char *warn_red );
int kitty_confirm_box3( HWND owner, const char *caption, const char *text, const char *b_over, const char *b_keep, const char *b_cancel );
int kitty_confirm_box_yes( HWND owner, const char *caption, const char *text, const char *warn_red );
void kitty_demo_templates( void );
void kitty_info_box( HWND owner, const char *caption, const char *text, const char *warn_red );
int kitty_message_box( HWND owner, const char *text, const char *caption, unsigned type );
void kitty_notice_box( HWND owner, const char *caption, const char *text );
void kitty_show_title_placeholders(HWND owner);
void kitty_start_update_check_notify( HWND hwnd, UINT msg );
void kitty_sync_transparency_menu(HMENU menu, Conf *conf, UINT id_up, UINT id_down, UINT id_anchor);
extern int PrintCharSize;
extern int PrintMaxCharPerLine;
extern int PrintMaxLinePerPage;

#endif
