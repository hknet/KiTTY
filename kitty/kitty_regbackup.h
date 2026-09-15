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

#endif /* KITTY_REGBACKUP_H */
