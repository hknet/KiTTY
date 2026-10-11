/*
 * kitty_regbackup.h - the declarations for kitty_regbackup.c: the .sav
 * export of the registry store (now, or on a worker), its restore, the
 * export of one key to a file, and the deletion of the hive.
 */
#ifndef KITTY_REGBACKUP_H
#define KITTY_REGBACKUP_H

#include "putty.h"
#include <windows.h>

int SaveRegistryKeyEx( HKEY hMainKey, LPCTSTR lpSubKey, const char * filename ) ;   /* 1 a whole export, 0 not: kitty_sav_save_reason() */
const char *kitty_sav_save_reason( void ) ;   /* "<file> (<reason>)" of the last failed save */
void SaveRegistryKey( void ) ;
int SaveRegistryKeyNow( void ) ;   /* 1 written (or backups off), 0 failed: kitty_sav_save_reason() */
/* The backup before a delete, a move or an overwrite: 1 go on, 0 stop (a
 * failed backup the user did not choose to go on without - the question or
 * the stop goes where kitty_reach() puts it). Use this, not SaveRegistryKeyNow, before a
 * destructive change. own_report: the caller shows kitty_backup_not_done()
 * on its own result line after a stop, so it is not reported twice. */
int kitty_backup_before( HWND owner, int own_report ) ;
char *kitty_backup_not_done( void ) ;   /* "Not done: ..." for a result line; snewn'd */
int LoadRegistryKey( HWND hdlg );   /* 1 loaded, 0 not: kitty_sav_load_reason() */
const char *kitty_sav_load_reason( void );   /* "<file> (<reason>)" of the last failed load */
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
