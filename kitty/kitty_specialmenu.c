/*
 * KiTTY User-Command special menu, moved verbatim out of kitty.c to shrink
 * that monolith: builds the "User Command" popup from the Commands registry
 * keys / portable-mode Commands directories (global, per-folder and
 * per-session), and replays a chosen entry (inline text or @file) into the
 * terminal. Compiled into the same targets as kitty.c (kitty +
 * kitty_portable), so behaviour is unchanged.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

#include "putty.h"

#include <windows.h>

#include "kitty.h"
#include "kitty_defs.h"      /* KITTY_DEFAULT_SESSION */
#include "kitty_commun.h"    /* ConfigDirectory, mungestr/unmungestr */
#include "kitty_registry.h"  /* MAX_VALUE_NAME */
#include "kitty_tools.h"     /* str_rtrim */

/* Provided elsewhere in the KiTTY tree (not in kitty.h). */
void SendKeyboardPlus( HWND hwnd, const char * st ) ;   /* kitty.c */

/* Shim so the moved ReadSpecialMenu body below stays textually identical to
 * its kitty.c original: the flag stayed behind as a kitty.c static with an
 * existing accessor. */
#define ShortcutsFlag (GetShortcutsFlag())

#define NB_MENU_MAX 1024
char *SpecialMenu[NB_MENU_MAX] ;   /* shared: kitty_launcher.c uses it directly */
int ReadSpecialMenu( HMENU menu, char * KeyName, int * nbitem, int separator ) {
	HKEY hKey ;
	HMENU SubMenu ;
	char buffer[4096], fullpath[1024], *p ;
	int i, nb ;
	int local_nb = 0 ;
	if( (IniFileFlag == SAVEMODE_REG)||(IniFileFlag == SAVEMODE_FILE) ) {
	if( RegOpenKeyEx( HKEY_CURRENT_USER, TEXT(KeyName), 0, KEY_READ, &hKey) == ERROR_SUCCESS ) {
		TCHAR achValue[MAX_VALUE_NAME], achClass[MAX_PATH] = TEXT("");
		DWORD  cchClassName=MAX_PATH,cSubKeys=0,cbMaxSubKey,cchMaxClass,cValues,cchMaxValue,cbMaxValueData,cbSecurityDescriptor;
		FILETIME ftLastWriteTime;

		RegQueryInfoKey(hKey,achClass,&cchClassName,NULL,&cSubKeys,&cbMaxSubKey,&cchMaxClass,&cValues,&cchMaxValue,&cbMaxValueData,&cbSecurityDescriptor,&ftLastWriteTime);
		nb = (*nbitem) ;

		if( cSubKeys>0 ) { // Recuperation des sous-menu
		for (i=0; (i<cSubKeys)&&(nb<NB_MENU_MAX); i++) {
			DWORD cchValue = MAX_VALUE_NAME; 
			char lpData[4096] ;
			achValue[0] = '\0';

			if( RegEnumKeyEx(hKey, i, lpData, &cchValue, NULL, NULL, NULL, &ftLastWriteTime) == ERROR_SUCCESS ) {
				SubMenu = CreateMenu() ;
				snprintf( buffer, sizeof(buffer), "%s\\%s", KeyName, lpData ) ;
				ReadSpecialMenu( SubMenu, buffer, nbitem, 0 ) ;
				unmungestr( lpData, buffer, MAX_PATH ) ;
				AppendMenu( menu, MF_POPUP, (UINT_PTR)SubMenu, buffer ) ;
				}
			}
		}
		
		nb = (*nbitem) ;
		
		if (cValues) { // Recuperation des item de menu
		if( separator ) AppendMenu( menu, MF_SEPARATOR, 0, 0 ) ;
		
		if( nb<NB_MENU_MAX )
	        for (i=0; (i<cValues)&&(nb<NB_MENU_MAX); i++) {
			DWORD cchValue = MAX_VALUE_NAME; 
			DWORD lpType,dwDataSize=4096 ;
			unsigned char lpData[4096] ;
			dwDataSize = 4096 ;
			achValue[0] = '\0';

			if( RegEnumValue(hKey,i,achValue,&cchValue,NULL,&lpType,lpData,&dwDataSize) == ERROR_SUCCESS ) {
			if( strcmp(achValue,KITTY_DEFAULT_SESSION) || strcmp(KeyName,TEXT(PUTTY_REG_POS) "\\Launcher") ) {
				if( ShortcutsFlag ) {
					if( nb < 26 ) 
						snprintf( buffer, sizeof(buffer), "%s\tCtrl+Shift+%c", achValue, ('A'+nb) ) ;
					else 
						snprintf( buffer, sizeof(buffer), "%s", achValue ) ;
					}
				else
					snprintf( buffer, sizeof(buffer), "%s", achValue ) ;
				AppendMenu(menu, MF_ENABLED, IDM_USERCMD+nb, buffer ) ;
				SpecialMenu[nb]=(char*)malloc( strlen( (char*)lpData ) + 1 ) ;
				strcpy( SpecialMenu[nb], (char*)lpData ) ;
				nb++ ;
				local_nb++ ;
				}
				}
			}
    		}

		(*nbitem)=nb ;
		
		RegCloseKey( hKey ) ;
		}
		}
	else if( IniFileFlag == SAVEMODE_DIR ) {
		snprintf( fullpath, sizeof(fullpath), "%s\\%s", ConfigDirectory, KeyName ) ;
		DIR * dir ;
		struct dirent * de ;
		FILE *fp ;
		if( ( dir = opendir( fullpath ) ) != NULL ) {
			if( separator ) AppendMenu( menu, MF_SEPARATOR, 0, 0 ) ;
			nb = (*nbitem) ;
			while( ( de = readdir(dir) ) != NULL ) { // Recherche de sous-cle (repertoire)
				if( strcmp(de->d_name,".") && strcmp(de->d_name,"..") ) {
					snprintf( buffer, sizeof(buffer), "%s\\%s", fullpath, de->d_name ) ;
					if( GetFileAttributes( buffer ) & FILE_ATTRIBUTE_DIRECTORY ) {
						SubMenu = CreateMenu() ;
						snprintf( buffer, sizeof(buffer), "%s\\%s", KeyName, de->d_name ) ;
						ReadSpecialMenu( SubMenu, buffer, nbitem, 0 ) ;
						unmungestr( de->d_name, buffer, MAX_PATH ) ;
						AppendMenu( menu, MF_POPUP, (UINT_PTR)SubMenu, buffer ) ;
						}
					/*if( stat( buffer, &statBuf ) != -1 ) {
						if( ( statBuf.st_mode & S_IFMT) == S_IFDIR ) {
							snprintf( buffer, sizeof(buffer), "%s\\%s", KeyName, de->d_name ) ;
							ReadSpecialMenu( menu, buffer, nbitem, separator ) ;
							}
						}*/
					}
				}
			rewinddir( dir ) ;

			nb = (*nbitem) ;
			while( ( de = readdir(dir) ) != NULL ) { // Recherche de cle
				if( strcmp(de->d_name,".") && strcmp(de->d_name,"..") ) {
				if( strcmp(de->d_name,"Default%20Settings") || strcmp(KeyName,"Launcher") ) { // Default Settings ne doit pas apparaitre dans le Launcher
					
					snprintf( buffer, sizeof(buffer), "%s\\%s", fullpath, de->d_name ) ;
					if( !(GetFileAttributes( buffer ) & FILE_ATTRIBUTE_DIRECTORY) ) {
						if( ( fp=fopen(buffer,"rb")) != NULL ) {
							while( fgets( buffer, 4096, fp )!=NULL ){
								str_rtrim( buffer, "\n\r" ) ;
								if( strlen(buffer)>0 && buffer[strlen(buffer)-1]=='\\' ) {
									buffer[strlen(buffer)-1]='\0' ;
									
									if( (p=strstr(buffer,"\\"))!=NULL ){
										p[0]='\0';
										AppendMenu(menu, MF_ENABLED, IDM_USERCMD+nb, buffer ) ;
										SpecialMenu[nb]=(char*)malloc( strlen( p+1 ) + 1 ) ;
										strcpy( SpecialMenu[nb], p+1 ) ;
										nb++ ;
										local_nb++ ;
										}
									}
								}
							fclose(fp) ;
							}
						}
						/*
					if( stat( buffer, &statBuf ) != -1 ) {
						if( ( statBuf.st_mode & S_IFMT ) == S_IFREG ) {
							
							}
						}*/
					}
					}
				}
			(*nbitem)=nb ;
			closedir( dir ) ;
			}
		}
	return local_nb ;
	}

void InitSpecialMenu( HMENU m, const char * folder, const char * sessionname ) {
	char KeyName[1024], buffer[1024] ;
	int nbitem = 0 ;

	HMENU menu ;
	menu = CreateMenu() ;
	
	if( IniFileFlag == SAVEMODE_DIR ) {
		strcpy( KeyName, "Commands" ) ;
		ReadSpecialMenu( menu, KeyName, &nbitem, 0 ) ;
		
		mungestr( folder, buffer ) ;
		snprintf( KeyName, sizeof(KeyName), "Folders\\%s\\Commands", buffer ) ;
		ReadSpecialMenu( menu, KeyName, &nbitem, 1 ) ;
		
		mungestr( sessionname, buffer ) ;
		snprintf( KeyName, sizeof(KeyName), "Sessions_Commands\\%s", buffer ) ;
		ReadSpecialMenu( menu, KeyName, &nbitem, 1 ) ;

		}
	else {
		snprintf( KeyName, sizeof(KeyName), "%s\\Commands", TEXT(PUTTY_REG_POS) ) ;
		ReadSpecialMenu( menu, KeyName, &nbitem, 0 ) ;
		
		mungestr( folder, buffer ) ;
		snprintf( KeyName, sizeof(KeyName), "%s\\Folders\\%s\\Commands", TEXT(PUTTY_REG_POS), buffer ) ;
		ReadSpecialMenu( menu, KeyName, &nbitem, 1 ) ;

		mungestr( sessionname, buffer ) ;
		snprintf( KeyName, sizeof(KeyName), "%s\\Sessions\\%s\\Commands", TEXT(PUTTY_REG_POS), buffer ) ;
		ReadSpecialMenu( menu, KeyName, &nbitem, 1 ) ;
		}

	if( GetMenuItemCount( menu ) > 0 )
		AppendMenu( m, MF_POPUP, (UINT_PTR)menu, "&User Command" ) ;

	}

void ManageSpecialCommand( HWND hwnd, int menunum ) {
	char buffer[4096] ;
	FILE *fp ;
	if( menunum < NB_MENU_MAX ) {
	if( SpecialMenu[menunum] != NULL ) 
		if( strlen( SpecialMenu[menunum] ) > 0 ) {
			if( ( fp=fopen( SpecialMenu[menunum], "r") ) != NULL ) {
				while( fgets( buffer, 4095, fp ) != NULL ) {
					SendKeyboardPlus( hwnd, buffer ) ;
					}
				fclose( fp ) ; 
				}
			else SendKeyboardPlus( hwnd, SpecialMenu[menunum] ) ;
			}
		}
	}

/* Seam: kitty.c's startup code used to zero the table with an inline loop. */
void InitSpecialMenuTab( void ) {
	int i ;
	for( i=0 ; i < NB_MENU_MAX ; i++ ) SpecialMenu[i] = NULL ;
	}
