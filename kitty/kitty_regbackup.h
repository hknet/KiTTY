/*
 * kitty_regbackup.h - the declarations for kitty_regbackup.c: the .sav
 * export of the registry store (now, or on a worker), its restore, the
 * export of one key to a file, and the deletion of the hive.
 */
#ifndef KITTY_REGBACKUP_H
#define KITTY_REGBACKUP_H

#include "putty.h"
#include <windows.h>

void SaveRegistryKeyEx( HKEY hMainKey, LPCTSTR lpSubKey, const char * filename ) ;
void SaveRegistryKey( void ) ;
void SaveRegistryKeyNow( void ) ;
void LoadRegistryKey( HWND hdlg );
void DelRegistryKey( void ) ;

/* /savereg: a backup now, whether or not the store changed since the last
 * one, waited for. path gets what was written (the .sav, or a portable copy's
 * backup folder) or what could not be. Returns 1 written, 0 failed,
 * -1 backups switched off (savbackupcount / portablebackupcount = 0),
 * -2 no backup file in this mode (no kitty.ini allowed). */
int kitty_sav_backup_report( char * path, size_t n ) ;

/* The folder the .sav backups go to (/savesessions writes beside them). */
void kitty_sav_dir( char * out, size_t n ) ;

/* The backup a restore takes: the newest dated one, else the fixed-name or
 * legacy file. 1 found (out = its path), 0 none. */
int sav_find_for_restore( const char *savfile, char *out, size_t outlen ) ;

#endif /* KITTY_REGBACKUP_H */
