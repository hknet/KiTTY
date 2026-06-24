#ifndef KITTY_WIN
#define KITTY_WIN

#include <stdlib.h>
#include <stdio.h>
#include "putty.h"
#include <windows.h>

void SetTransparency( HWND hwnd, int value ) ;
void GetOSInfo( char * version ) ;
BOOL IsWow64() ; // Test si on est en Windows 64 bits
int OpenFileName( HWND hFrame, char * filename, char * Title, char * Filter ) ;
int OpenDirName( HWND hFrame, char * dirname ) ;
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
void CheckVersionFromWebSite( HWND hwnd ) ;

// KiTTY: background (async) update check that caches the latest version, and a
// session-start notice rendered from that cache (see window.c).
void kitty_start_update_check( void ) ;
int kitty_update_notice( char *buf, int n ) ;
int kitty_update_available( char *latest_out, int latest_n, char *cur_out, int cur_n, int *beta_out ) ;

// Affichage d'un message dans l'event log
void debug_logevent( const char *fmt, ... ) ;

// Test si un chemin est absolu
bool IsPathAbsolute( const char * path ) ;

// Affiche le menu system de l'application
void PopUpSystemMenu( HWND hwnd, int npos  ) ;

// Affiche une bulle tooltip
HWND CreateToolTip(int toolID, HWND hDlg, PTSTR pszText) ;
#endif
