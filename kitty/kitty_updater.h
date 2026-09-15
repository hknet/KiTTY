/*
 * kitty_updater.h - the declarations for kitty_updater.c, the in-app
 * updater: the release check, the background check that caches the latest
 * version, and the session-start notice rendered from that cache.
 */
#ifndef KITTY_UPDATER_H
#define KITTY_UPDATER_H

#include "putty.h"
#include <windows.h>

// Check whether an update is available on the web site
void CheckVersionFromWebSite( HWND hwnd, int is_terminal ) ;

// KiTTY: background (async) update check that caches the latest version, and a
// session-start notice rendered from that cache (see window.c).
void kitty_start_update_check( void ) ;
void kitty_start_update_check_notify( HWND hwnd, UINT msg ) ;
int kitty_update_notice( char *buf, int n ) ;
int kitty_update_available( char *latest_out, int latest_n, char *cur_out, int cur_n, int *beta_out ) ;

#endif
