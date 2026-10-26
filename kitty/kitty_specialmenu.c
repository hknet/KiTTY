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
#include "kitty_common.h"    /* ConfigDirectory, mungestr/unmungestr */
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

/* KiTTY: the entry payloads live in kitty_menuslots.c, a table that grows
 * up to NB_MENU_MAX. That ceiling must keep IDM_USERCMD + n clear of every
 * other command id the launcher and the terminal window handle. */
_Static_assert( IDM_USERCMD + NB_MENU_MAX <= IDM_GOHIDE,
                "User-Command / launcher ids run into IDM_GOHIDE" ) ;
_Static_assert( IDM_USERCMD + NB_MENU_MAX <= IDM_WORKPLACE,
                "User-Command / launcher ids run into IDM_WORKPLACE" ) ;
_Static_assert( IDM_USERCMD + NB_MENU_MAX <= IDM_QUIT,
                "User-Command / launcher ids run into IDM_QUIT" ) ;

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

/* KiTTY: one folder's submenu `sub`, filled, appended to `menu`. A folder
 * whose entries were ALL cut by the ceiling is left out rather than shown
 * empty; its entries are in the count of the ones not shown. A folder that
 * is empty in the store is still listed, as before. cut: the count before
 * the submenu was filled. */
static void sm_add_folder( HMENU menu, HMENU sub, int cut, const char * label ) {
	if( GetMenuItemCount( sub ) == 0 && kitty_menuslots_cut() > cut ) {
		DestroyMenu( sub ) ;
		return ;
		}
	AppendMenu( menu, MF_POPUP, (UINT_PTR)sub, label ) ;
	}

/* KiTTY: one folder's submenu, read and appended to `menu`. */
static void sm_read_folder( HMENU menu, HMENU sub, char * key, int * nbitem, const char * label ) {
	int cut = kitty_menuslots_cut() ;
	ReadSpecialMenu( sub, key, nbitem, 0 ) ;
	sm_add_folder( menu, sub, cut, label ) ;
	}

/* KiTTY: the entries of one menu level, appended as numbered commands
 * (kitty_menuslots); past the ceiling an entry is only counted
 * (kitty_menuslots_cut). accel: with the Ctrl+Shift+<letter> column of the
 * first 26. Returns how many were appended. */
static int sm_append_entries( HMENU menu, struct sm_item *items, int nitems, int accel ) {
	char * buffer ;
	int k, nb, n = 0 ;
	for( k = 0 ; k < nitems ; k++ ) {
		if( ( nb = kitty_menuslots_add( items[k].data ) ) < 0 ) continue ;
		if( accel && nb < 26 )
			buffer = dupprintf( "%s\tCtrl+Shift+%c", items[k].label, ('A'+nb) ) ;
		else
			buffer = dupstr( items[k].label ) ;
		AppendMenu( menu, MF_ENABLED, IDM_USERCMD+nb, buffer ) ;
		sfree( buffer ) ;
		n++ ;
		}
	return n ;
	}

/* One value of an open key, its name and its data in full (sfree both; the
 * data gets two NULs after it, so a string is always terminated). The answer
 * is RegEnumValue's: anything but ERROR_SUCCESS leaves both NULL. */
static LONG sm_enum_value( HKEY hKey, DWORD i, char ** name, char ** data ) {
	DWORD dcap = 4096 ;
	*name = NULL ; *data = NULL ;
	for( ;; ) {
		/* a value name is at most 16383 characters */
		DWORD nlen = MAX_VALUE_NAME + 1, dlen = dcap, type ;
		LONG r ;
		*name = snewn( MAX_VALUE_NAME + 1, char ) ;
		*data = snewn( dcap + 2, char ) ;
		r = RegEnumValue( hKey, i, *name, &nlen, NULL, &type, (LPBYTE)*data, &dlen ) ;
		if( r == ERROR_SUCCESS && dlen <= dcap ) {
			(*data)[dlen] = (*data)[dlen+1] = '\0' ;
			return r ;
			}
		sfree( *name ) ; sfree( *data ) ;
		*name = NULL ; *data = NULL ;
		if( r != ERROR_MORE_DATA && r != ERROR_SUCCESS ) return r ;
		/* the data grew: dlen holds the size it needs now */
		if( dlen <= dcap ) dlen = dcap * 2 ;
		if( dlen > 64 * 1024 * 1024 ) return ERROR_MORE_DATA ;
		dcap = dlen ;
		}
	}

int ReadSpecialMenu( HMENU menu, char * KeyName, int * nbitem, int separator ) {
	HKEY hKey ;
	HMENU SubMenu ;
	char * buffer, * p ;
	int i ;
	int local_nb = 0 ;
	if( (IniFileFlag == SAVEMODE_REG)||(IniFileFlag == SAVEMODE_FILE) ) {
	if( RegOpenKeyEx( HKEY_CURRENT_USER, TEXT(KeyName), 0, KEY_READ, &hKey) == ERROR_SUCCESS ) {
		TCHAR achClass[MAX_PATH] = TEXT("");
		DWORD  cchClassName=MAX_PATH,cSubKeys=0,cbMaxSubKey,cchMaxClass,cValues=0,cchMaxValue,cbMaxValueData,cbSecurityDescriptor;
		FILETIME ftLastWriteTime;

		char * launcherkey ;
		struct sm_item *items = NULL ;
		int nitems = 0, k, is_launcher ;
		launcherkey = dupprintf( "%s\\Launcher", kitty_registry_base() ) ;
		is_launcher = !strncmp( KeyName, launcherkey, strlen(launcherkey) ) ;
		sfree( launcherkey ) ;

		if( RegQueryInfoKey(hKey,achClass,&cchClassName,NULL,&cSubKeys,&cbMaxSubKey,&cchMaxClass,&cValues,&cchMaxValue,&cbMaxValueData,&cbSecurityDescriptor,&ftLastWriteTime) != ERROR_SUCCESS ) {
			cSubKeys = 0 ; cValues = 0 ;
			}

		if( cSubKeys>0 ) { // collect the submenus
		for (i=0; i<cSubKeys; i++) {
			/* a key name is at most 255 characters */
			char lpData[MAX_KEY_LENGTH + 1] ;
			DWORD cchValue = sizeof(lpData) ;

			if( RegEnumKeyEx(hKey, i, lpData, &cchValue, NULL, NULL, NULL, &ftLastWriteTime) == ERROR_SUCCESS ) {
				char * plain = unmungestr_dup( lpData ) ;
				sm_items_add( &items, &nitems, plain, lpData ) ;
				sfree( plain ) ;
				}
			}
		}
		if( is_launcher ) sm_items_sort( items, nitems ) ;
		for( k = 0 ; k < nitems ; k++ ) {
			SubMenu = CreateMenu() ;
			buffer = dupprintf( "%s\\%s", KeyName, items[k].data ) ;
			sm_read_folder( menu, SubMenu, buffer, nbitem, items[k].label ) ;
			sfree( buffer ) ;
			}
		sm_items_free( &items, &nitems ) ;


		if (cValues) { // collect the menu items
		if( separator ) AppendMenu( menu, MF_SEPARATOR, 0, 0 ) ;

	        for (i=0; i<cValues; i++) {
			char * achValue, * lpData ;
			if( sm_enum_value( hKey, i, &achValue, &lpData ) == ERROR_SUCCESS ) {
			/* Default Settings never appears in the launcher, at ANY level of
			 * its copy: the writer skips it now (InitLauncherRegistry), this
			 * keeps a copy an older build left behind out of the menu too. */
			if( strcmp(achValue,KITTY_DEFAULT_SESSION) || !is_launcher )
				sm_items_add( &items, &nitems, achValue, lpData ) ;
			sfree( achValue ) ;
			sfree( lpData ) ;
				}
			}
    		}
		if( is_launcher ) sm_items_sort( items, nitems ) ;
		local_nb += sm_append_entries( menu, items, nitems, ShortcutsFlag ) ;
		sm_items_free( &items, &nitems ) ;

		(*nbitem)=kitty_menuslots_count() ;

		RegCloseKey( hKey ) ;
		}
		}
	else if( IniFileFlag == SAVEMODE_DIR ) {
		char * fullpath = portable_config_subdir( KeyName ) ;
		DIR * dir ;
		struct dirent * de ;
		FILE *fp ;
		if( fullpath != NULL && ( dir = opendir( fullpath ) ) != NULL ) {
			struct sm_item *items = NULL ;
			int nitems = 0, k, is_launcher = !strncmp(KeyName,"Launcher",8) ;
			if( separator ) AppendMenu( menu, MF_SEPARATOR, 0, 0 ) ;
			while( ( de = readdir(dir) ) != NULL ) { // look for subkeys (directories)
				if( strcmp(de->d_name,".") && strcmp(de->d_name,"..") ) {
					DWORD attr ;
					buffer = dupprintf( "%s\\%s", fullpath, de->d_name ) ;
					attr = GetFileAttributes( buffer ) ;
					sfree( buffer ) ;
					if( attr != INVALID_FILE_ATTRIBUTES && ( attr & FILE_ATTRIBUTE_DIRECTORY ) ) {
						char * plain = unmungestr_dup( de->d_name ) ;
						sm_items_add( &items, &nitems, plain, de->d_name ) ;
						sfree( plain ) ;
						}
					}
				}
			if( is_launcher ) sm_items_sort( items, nitems ) ;
			for( k = 0 ; k < nitems ; k++ ) {
				SubMenu = CreateMenu() ;
				buffer = dupprintf( "%s\\%s", KeyName, items[k].data ) ;
				sm_read_folder( menu, SubMenu, buffer, nbitem, items[k].label ) ;
				sfree( buffer ) ;
				}
			sm_items_free( &items, &nitems ) ;
			rewinddir( dir ) ;

			while( ( de = readdir(dir) ) != NULL ) { // look for keys
				if( strcmp(de->d_name,".") && strcmp(de->d_name,"..") ) {
				/* Default Settings must not show up in the launcher - at any
				 * level of its copy, and by the unmunged name (the writer skips
				 * it now; this covers a copy an older build left behind). */
				char * plain = unmungestr_dup( de->d_name ) ;
				if( strcmp(plain,KITTY_DEFAULT_SESSION) || strncmp(KeyName,"Launcher",8) ) {
					DWORD attr ;
					buffer = dupprintf( "%s\\%s", fullpath, de->d_name ) ;
					attr = GetFileAttributes( buffer ) ;
					if( attr != INVALID_FILE_ATTRIBUTES && !( attr & FILE_ATTRIBUTE_DIRECTORY ) ) {
						if( ( fp=fopen(buffer,"rb")) != NULL ) {
							char * line ;
							/* each line in full: a long one used to arrive in pieces */
							while( ( line = kitty_fgetline( fp ) ) != NULL ) {
								str_rtrim( line, "\n\r" ) ;
								if( strlen(line)>0 && line[strlen(line)-1]=='\\' ) {
									line[strlen(line)-1]='\0' ;

									if( (p=strstr(line,"\\"))!=NULL ){
										p[0]='\0';
										sm_items_add( &items, &nitems, line, p+1 ) ;
										}
									}
								free( line ) ;
								}
							fclose(fp) ;
							}
						}
					sfree( buffer ) ;
					}
				sfree( plain ) ;
					}
				}
			if( is_launcher ) sm_items_sort( items, nitems ) ;
			local_nb += sm_append_entries( menu, items, nitems, 0 ) ;
			sm_items_free( &items, &nitems ) ;
			(*nbitem)=kitty_menuslots_count() ;
			closedir( dir ) ;
			}
		sfree( fullpath ) ;
		}
	return local_nb ;
	}

/* readonly=yes (the launcher, InitLauncherMenu): the session menu built from
 * the entries InitLauncherRegistry kept in memory, as its Launcher copy
 * cannot be rebuilt - the same levels, order, ceiling and accelerators as
 * ReadSpecialMenu gives that copy: the folders one level below `prefix`
 * first, each a submenu, then the sessions of `prefix` itself, both by the
 * name shown. Returns the entries appended at this level. */
int ReadLauncherEntries( HMENU menu, const struct kitty_launcher_entry * e, int n,
                         const char * prefix, int * nbitem ) {
	struct sm_item *items = NULL ;
	int nitems = 0, i, k, local_nb ;
	size_t pl = strlen( prefix ) ;
	for( i = 0 ; i < n ; i++ ) {	/* the folders one level below prefix */
		const char * rest = e[i].folder, * end ;
		char * comp ;
		size_t cl ;
		if( pl ) {
			if( strnicmp( rest, prefix, pl ) || rest[pl] != '\\' ) continue ;
			rest += pl + 1 ;
			}
		if( !*rest ) continue ;
		end = strchr( rest, '\\' ) ;
		cl = end ? (size_t)( end - rest ) : strlen( rest ) ;
		if( !cl ) continue ;
		comp = snewn( cl + 1, char ) ;
		memcpy( comp, rest, cl ) ;
		comp[cl] = '\0' ;
		for( k = 0 ; k < nitems && stricmp( items[k].label, comp ) ; k++ ) ;
		if( k == nitems ) sm_items_add( &items, &nitems, comp, comp ) ;
		sfree( comp ) ;
		}
	sm_items_sort( items, nitems ) ;
	for( k = 0 ; k < nitems ; k++ ) {
		HMENU sub = CreateMenu() ;
		int cut = kitty_menuslots_cut() ;
		char * sp = pl ? dupprintf( "%s\\%s", prefix, items[k].data ) : dupstr( items[k].data ) ;
		ReadLauncherEntries( sub, e, n, sp, nbitem ) ;
		sfree( sp ) ;
		sm_add_folder( menu, sub, cut, items[k].label ) ;
		}
	sm_items_free( &items, &nitems ) ;
	for( i = 0 ; i < n ; i++ )	/* the sessions at this level */
		if( !stricmp( e[i].folder, prefix ) )
			sm_items_add( &items, &nitems, ksp_leaf( e[i].session ), e[i].session ) ;
	sm_items_sort( items, nitems ) ;
	local_nb = sm_append_entries( menu, items, nitems,
	                              IniFileFlag != SAVEMODE_DIR && ShortcutsFlag ) ;
	sm_items_free( &items, &nitems ) ;
	(*nbitem) = kitty_menuslots_count() ;
	return local_nb ;
	}

void InitSpecialMenu( HMENU m, const char * folder, const char * sessionname ) {
	char * KeyName, * mfolder, * msession ;
	int nbitem = 0 ;

	HMENU menu ;
	menu = CreateMenu() ;
	/* Each build of the menu starts the numbering anew (the system menu and
	 * the context menu get the same numbers for the same commands). */
	kitty_menuslots_reset( 0 ) ;
	/* the escaped names in full: up to three bytes per character */
	mfolder = mungestr_dup( folder ? folder : "" ) ;
	msession = mungestr_dup( sessionname ? sessionname : "" ) ;

	if( IniFileFlag == SAVEMODE_DIR ) {
		KeyName = dupstr( "Commands" ) ;
		ReadSpecialMenu( menu, KeyName, &nbitem, 0 ) ;
		sfree( KeyName ) ;

		KeyName = dupprintf( "Folders\\%s\\Commands", mfolder ) ;
		ReadSpecialMenu( menu, KeyName, &nbitem, 1 ) ;
		sfree( KeyName ) ;

		KeyName = dupprintf( "Sessions_Commands\\%s", msession ) ;
		ReadSpecialMenu( menu, KeyName, &nbitem, 1 ) ;
		sfree( KeyName ) ;

		}
	else {
		KeyName = dupprintf( "%s\\Commands", kitty_registry_base() ) ;
		ReadSpecialMenu( menu, KeyName, &nbitem, 0 ) ;
		sfree( KeyName ) ;

		KeyName = dupprintf( "%s\\Folders\\%s\\Commands", kitty_registry_base(), mfolder ) ;
		ReadSpecialMenu( menu, KeyName, &nbitem, 1 ) ;
		sfree( KeyName ) ;

		KeyName = dupprintf( "%s\\Sessions\\%s\\Commands", kitty_registry_base(), msession ) ;
		ReadSpecialMenu( menu, KeyName, &nbitem, 1 ) ;
		sfree( KeyName ) ;
		}
	sfree( mfolder ) ;
	sfree( msession ) ;

	if( GetMenuItemCount( menu ) > 0 )
		AppendMenu( m, MF_POPUP, (UINT_PTR)menu, KT_MENU_USER_COMMAND ) ;

	}

void ManageSpecialCommand( HWND hwnd, int menunum ) {
	char * line ;
	FILE *fp ;
	/* A copy: replaying the keys can run messages, and a menu rebuild frees
	 * the table's payloads. */
	char * cmd = kitty_menuslots_get( menunum ) ? dupstr( kitty_menuslots_get( menunum ) ) : NULL ;
	if( cmd != NULL ) {
		if( strlen( cmd ) > 0 ) {
			if( ( fp=fopen( cmd, "r") ) != NULL ) {
				/* each line in full: a long one used to arrive in pieces */
				while( ( line = kitty_fgetline( fp ) ) != NULL ) {
					SendKeyboardPlus( hwnd, line ) ;
					free( line ) ;
					}
				fclose( fp ) ;
				}
			else SendKeyboardPlus( hwnd, cmd ) ;
			}
		sfree( cmd ) ;
		}
	}

/* Seam: the startup code (kitty_startup.c) used to zero the table with an inline loop. */
void InitSpecialMenuTab( void ) {
	kitty_menuslots_reset( 0 ) ;
	}
