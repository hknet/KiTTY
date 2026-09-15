/*
 * kitty_launcher.h - the entry points of kitty_launcher.c: the tray
 * launcher's own WinMain, and the two helpers that start a child KiTTY
 * (a bare instance or a named saved session) with this process's settings
 * carried over.
 */
#ifndef KITTY_LAUNCHER_H
#define KITTY_LAUNCHER_H

#include <windows.h>

void RunPuTTY( HWND hwnd, char * param ) ;
int RunSession( HWND hwnd, const char * folder_in, char * session_in ) ;
int WINAPI Launcher_WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show) ;


/* ---- exported from kitty/kitty_launcher.c ---- */
void ManageHideOne( HWND hwnd );
void ManageSwitch( const int n );
void ManageUnHideOne( HWND hwnd );
int RefreshWinList( HWND hwnd );
void RunConfig( Conf * conf );

#endif
