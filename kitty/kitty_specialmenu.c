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
#include "kitty_params.h"
#include "kitty_broadcast.h"
/* The hive in use, not the compile-time default - see kitty_storage.c. */

#include "kitty_defs.h"      /* KITTY_DEFAULT_SESSION */
#include "kitty_commun.h"    /* ConfigDirectory, mungestr/unmungestr */
#include "kitty_registry.h"  /* MAX_VALUE_NAME */
#include "kitty_tools.h"     /* str_rtrim */
#include "kitty_text.h"      /* the menu wording */
#include "kitty_storage.h"
#include "kitty_sessionpath.h" /* ksp_natcasecmp: the launcher's order */

/* Provided elsewhere in the KiTTY tree (not in kitty.h). */

/* Shim so the moved ReadSpecialMenu body below stays textually identical to
 * its kitty.c original: the flag stayed behind as a kitty.c static with an
 * existing accessor. */
#define ShortcutsFlag (GetShortcutsFlag())

#define NB_MENU_MAX 1024
char *SpecialMenu[NB_MENU_MAX] ;   /* shared: kitty_launcher.c uses it directly */

/* KiTTY: the entries of one menu level, gathered before they are appended.
 * The launcher's copy of the sessions is then listed by the name each entry
 * shows, digit runs by value: the store hands them over in its own order (the
 * registry's order of the escaped full session name, or the directory's), in
 * which a session stored by its bare name sorted apart from the ones stored
 * as "folder\name" paths. The User Command menus keep their stored order. */
struct sm_item { char *label ; char *data ; } ;
static void sm_items_add( struct sm_item **v, int *n, const char *label, const char *data ) {
	*v = sresize( *v, *n + 1, struct sm_item ) ;
	(*v)[*n].label = dupstr( label ) ;
	(*v)[*n].data = dupstr( data ) ;
	(*n)++ ;
}
static int sm_item_cmp( const void *a, const void *b ) {
	const struct sm_item *x = a, *y = b ;
	int c = ksp_natcasecmp( x->label, y->label ) ;
	return c ? c : strcmp( x->data, y->data ) ;
}
static void sm_items_sort( struct sm_item *v, int n ) {
	if( n > 1 ) qsort( v, n, sizeof(*v), sm_item_cmp ) ;
}
static void sm_items_free( struct sm_item **v, int *n ) {
	int k ;
	for( k = 0 ; k < *n ; k++ ) { sfree( (*v)[k].label ) ; sfree( (*v)[k].data ) ; }
	sfree( *v ) ;
	*v = NULL ;
	*n = 0 ;
}

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

		char launcherkey[1024] ;
		struct sm_item *items = NULL ;
		int nitems = 0, k, is_launcher ;
		snprintf( launcherkey, sizeof(launcherkey), "%s\\Launcher", kitty_registry_base() ) ;
		is_launcher = !strncmp( KeyName, launcherkey, strlen(launcherkey) ) ;

		RegQueryInfoKey(hKey,achClass,&cchClassName,NULL,&cSubKeys,&cbMaxSubKey,&cchMaxClass,&cValues,&cchMaxValue,&cbMaxValueData,&cbSecurityDescriptor,&ftLastWriteTime);
		nb = (*nbitem) ;

		if( cSubKeys>0 ) { // collect the submenus
		for (i=0; (i<cSubKeys)&&(nb<NB_MENU_MAX); i++) {
			DWORD cchValue = MAX_VALUE_NAME;
			char lpData[4096] ;
			achValue[0] = '\0';

			if( RegEnumKeyEx(hKey, i, lpData, &cchValue, NULL, NULL, NULL, &ftLastWriteTime) == ERROR_SUCCESS ) {
				unmungestr( lpData, buffer, MAX_PATH ) ;
				sm_items_add( &items, &nitems, buffer, lpData ) ;
				}
			}
		}
		if( is_launcher ) sm_items_sort( items, nitems ) ;
		for( k = 0 ; k < nitems ; k++ ) {
			SubMenu = CreateMenu() ;
			snprintf( buffer, sizeof(buffer), "%s\\%s", KeyName, items[k].data ) ;
			ReadSpecialMenu( SubMenu, buffer, nbitem, 0 ) ;
			AppendMenu( menu, MF_POPUP, (UINT_PTR)SubMenu, items[k].label ) ;
			}
		sm_items_free( &items, &nitems ) ;

		nb = (*nbitem) ;

		if (cValues) { // collect the menu items
		if( separator ) AppendMenu( menu, MF_SEPARATOR, 0, 0 ) ;

		if( nb<NB_MENU_MAX )
	        for (i=0; i<cValues; i++) {
			DWORD cchValue = MAX_VALUE_NAME;
			DWORD lpType,dwDataSize=4096 ;
			unsigned char lpData[4096] ;
			dwDataSize = 4096 ;
			achValue[0] = '\0';

			if( RegEnumValue(hKey,i,achValue,&cchValue,NULL,&lpType,lpData,&dwDataSize) == ERROR_SUCCESS ) {
			/* Default Settings never appears in the launcher, at ANY level of
			 * its copy: the writer skips it now (InitLauncherRegistry), this
			 * keeps a copy an older build left behind out of the menu too. */
			lpData[dwDataSize < sizeof(lpData) ? dwDataSize : sizeof(lpData) - 1] = '\0' ;
			if( strcmp(achValue,KITTY_DEFAULT_SESSION) || !is_launcher )
				sm_items_add( &items, &nitems, achValue, (char*)lpData ) ;
				}
			}
    		}
		if( is_launcher ) sm_items_sort( items, nitems ) ;
		for( k = 0 ; k < nitems && nb < NB_MENU_MAX ; k++ ) {
			if( ShortcutsFlag ) {
				if( nb < 26 )
					snprintf( buffer, sizeof(buffer), "%s\tCtrl+Shift+%c", items[k].label, ('A'+nb) ) ;
				else
					snprintf( buffer, sizeof(buffer), "%s", items[k].label ) ;
				}
			else
				snprintf( buffer, sizeof(buffer), "%s", items[k].label ) ;
			AppendMenu(menu, MF_ENABLED, IDM_USERCMD+nb, buffer ) ;
			SpecialMenu[nb]=(char*)malloc( strlen( items[k].data ) + 1 ) ;
			strcpy( SpecialMenu[nb], items[k].data ) ;
			nb++ ;
			local_nb++ ;
			}
		sm_items_free( &items, &nitems ) ;

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
			struct sm_item *items = NULL ;
			int nitems = 0, k, is_launcher = !strncmp(KeyName,"Launcher",8) ;
			if( separator ) AppendMenu( menu, MF_SEPARATOR, 0, 0 ) ;
			nb = (*nbitem) ;
			while( ( de = readdir(dir) ) != NULL ) { // look for subkeys (directories)
				if( strcmp(de->d_name,".") && strcmp(de->d_name,"..") ) {
					snprintf( buffer, sizeof(buffer), "%s\\%s", fullpath, de->d_name ) ;
					if( GetFileAttributes( buffer ) & FILE_ATTRIBUTE_DIRECTORY ) {
						unmungestr( de->d_name, buffer, MAX_PATH ) ;
						sm_items_add( &items, &nitems, buffer, de->d_name ) ;
						}
					/*if( stat( buffer, &statBuf ) != -1 ) {
						if( ( statBuf.st_mode & S_IFMT) == S_IFDIR ) {
							snprintf( buffer, sizeof(buffer), "%s\\%s", KeyName, de->d_name ) ;
							ReadSpecialMenu( menu, buffer, nbitem, separator ) ;
							}
						}*/
					}
				}
			if( is_launcher ) sm_items_sort( items, nitems ) ;
			for( k = 0 ; k < nitems ; k++ ) {
				SubMenu = CreateMenu() ;
				snprintf( buffer, sizeof(buffer), "%s\\%s", KeyName, items[k].data ) ;
				ReadSpecialMenu( SubMenu, buffer, nbitem, 0 ) ;
				AppendMenu( menu, MF_POPUP, (UINT_PTR)SubMenu, items[k].label ) ;
				}
			sm_items_free( &items, &nitems ) ;
			rewinddir( dir ) ;

			nb = (*nbitem) ;
			while( ( de = readdir(dir) ) != NULL ) { // look for keys
				if( strcmp(de->d_name,".") && strcmp(de->d_name,"..") ) {
				/* Default Settings must not show up in the launcher - at any
				 * level of its copy, and by the unmunged name (the writer skips
				 * it now; this covers a copy an older build left behind). */
				char plain[MAX_PATH] ;
				unmungestr( de->d_name, plain, MAX_PATH ) ;
				if( strcmp(plain,KITTY_DEFAULT_SESSION) || strncmp(KeyName,"Launcher",8) ) {
					
					snprintf( buffer, sizeof(buffer), "%s\\%s", fullpath, de->d_name ) ;
					if( !(GetFileAttributes( buffer ) & FILE_ATTRIBUTE_DIRECTORY) ) {
						if( ( fp=fopen(buffer,"rb")) != NULL ) {
							while( fgets( buffer, 4096, fp )!=NULL ){
								str_rtrim( buffer, "\n\r" ) ;
								if( strlen(buffer)>0 && buffer[strlen(buffer)-1]=='\\' ) {
									buffer[strlen(buffer)-1]='\0' ;
									
									if( (p=strstr(buffer,"\\"))!=NULL ){
										p[0]='\0';
										sm_items_add( &items, &nitems, buffer, p+1 ) ;
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
			if( is_launcher ) sm_items_sort( items, nitems ) ;
			for( k = 0 ; k < nitems && nb < NB_MENU_MAX ; k++ ) {
				AppendMenu(menu, MF_ENABLED, IDM_USERCMD+nb, items[k].label ) ;
				SpecialMenu[nb]=(char*)malloc( strlen( items[k].data ) + 1 ) ;
				strcpy( SpecialMenu[nb], items[k].data ) ;
				nb++ ;
				local_nb++ ;
				}
			sm_items_free( &items, &nitems ) ;
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
		snprintf( KeyName, sizeof(KeyName), "%s\\Commands", kitty_registry_base() ) ;
		ReadSpecialMenu( menu, KeyName, &nbitem, 0 ) ;
		
		mungestr( folder, buffer ) ;
		snprintf( KeyName, sizeof(KeyName), "%s\\Folders\\%s\\Commands", kitty_registry_base(), buffer ) ;
		ReadSpecialMenu( menu, KeyName, &nbitem, 1 ) ;

		mungestr( sessionname, buffer ) ;
		snprintf( KeyName, sizeof(KeyName), "%s\\Sessions\\%s\\Commands", kitty_registry_base(), buffer ) ;
		ReadSpecialMenu( menu, KeyName, &nbitem, 1 ) ;
		}

	if( GetMenuItemCount( menu ) > 0 )
		AppendMenu( m, MF_POPUP, (UINT_PTR)menu, KT_MENU_USER_COMMAND ) ;

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

/* Seam: the startup code (kitty_startup.c) used to zero the table with an inline loop. */
void InitSpecialMenuTab( void ) {
	int i ;
	for( i=0 ; i < NB_MENU_MAX ; i++ ) SpecialMenu[i] = NULL ;
	}
