#ifndef KITTY_WIN
#define KITTY_WIN

#include <stdlib.h>
#include <stdio.h>
#include "putty.h"
#include <windows.h>

void SetTransparency( HWND hwnd, int value ) ;
void GetOSInfo( char * version ) ;

// Rendu inline (non modal) des erreurs de connexion dans le terminal (upstream cyd01/KiTTY #548)
void kitty_term_print_inline_error(Terminal *term, const char *msg, int fatal) ;
void kitty_print_session_comment(Terminal *term, Conf *conf) ;   /* framed Comment at session start */

/* One line in the terminal, and the whole list in the Event Log, naming what
 * this version of Windows is too old to provide. Once per process; silent when
 * nothing is missing, and switched off with [KiTTY] warnmissingfeatures=no.
 * See kitty_oldwin.h. */
void kitty_report_missing_features(Terminal *term) ;

// Corps des commandes du menu systeme (window.c WM_COMMAND) sans dependance aux statics de window.c
void kitty_menu_adjust_transparency(HWND term_hwnd, Conf *conf, int up) ;
void kitty_menu_toggle_alwaysontop(HWND term_hwnd, Conf *conf) ;
void kitty_menu_reposition(HWND term_hwnd, Conf *conf, int x, int y) ;

// [ConfigBox] noexit: relance une instance (= la config box) a la fermeture d'une session
void kitty_respawn_config_box(void) ;

// Reapplique les decorations de titre ([KiTTY] wintitle/size, PROTECTED/ONTOP) - windows/window.c
void kitty_refresh_title(void) ;
void kitty_menu_toggle_hyperlink(HWND hwnd) ;
BOOL IsWow64() ; // Test si on est en Windows 64 bits
int OpenFileName( HWND hFrame, char * filename, char * Title, char * Filter ) ;
int OpenDirName( HWND hFrame, char * dirname ) ;
/* Centre a modal dialog over its owner (WM_INITDIALOG, after final size). */
void kitty_centre_on_owner( HWND dlg ) ;
int SaveFileName( HWND hFrame, char * filename, char * Title, char * Filter ) ;
	
// Centre un dialog au milieu de la fenetre parent
void CenterDlgInParent(HWND hDlg) ;

// Envoi vers l'imprimante
int PrintText( const char * Text ) ;

// Impression du texte dans le bloc-notes
void ManagePrint( HWND hwnd ) ;

// Met un texte dans le press-papier
int SetTextToClipboard( const char * buf ) ;

// Execute une commande	
void RunCommand( HWND hwnd, const char * cmd ) ;

// Démarre l'éditeur embarqué
void RunPuttyEd( HWND hwnd, char * filename ) ;

// Verifie si une mise a jour est disponible sur le site web
void CheckVersionFromWebSite( HWND hwnd, int is_terminal ) ;

// KiTTY: background (async) update check that caches the latest version, and a
// session-start notice rendered from that cache (see window.c).
void kitty_start_update_check( void ) ;
int kitty_update_notice( char *buf, int n ) ;
int kitty_update_available( char *latest_out, int latest_n, char *cur_out, int cur_n, int *beta_out ) ;

// Affichage d'un message dans l'event log
void debug_logevent( const char *fmt, ... ) ;

// Test si un chemin est absolu
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
#endif
