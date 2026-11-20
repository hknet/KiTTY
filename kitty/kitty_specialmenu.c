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
#include "kitty_globalsettings.h"   /* the registry through the settings module */
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
#include "kitty_sessorder.h"   /* the manual order of each level */
#include "kitty_sessicon.h"    /* session icons in front of the names (#62) */

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
	int c = ksp_name_cmp( x->label, y->label ) ;
	return c ? c : strcmp( x->data, y->data ) ;
}

/* KiTTY: the folder a level of the launcher's copy stands for ("" = the
 * top): its key's path below Launcher, each component unescaped. */
static char * sm_launcher_folder( const char * rest ) {
	strbuf * sb = strbuf_new() ;
	while( *rest == '\\' ) rest++ ;
	while( *rest ) {
		const char * e = strchr( rest, '\\' ) ;
		size_t n = e ? (size_t)( e - rest ) : strlen( rest ) ;
		char * comp = dupprintf( "%.*s", (int)n, rest ) ;
		char * plain = unmungestr_dup( comp ) ;
		if( sb->len ) put_byte( sb, '\\' ) ;
		put_dataz( sb, plain ) ;
		sfree( plain ) ;
		sfree( comp ) ;
		rest += n ;
		while( *rest == '\\' ) rest++ ;
		}
	return strbuf_to_str( sb ) ;
	}

/* KiTTY: the entries of one level after the sort, in the manual order stored
 * for `folder` (Manage Sessions' drag and drop): kind 'f' the folders, 's'
 * the sessions, by the name each entry shows. */
static void sm_items_order( struct sm_item *v, int n, const char * folder, char kind ) {
	struct ksp_order_store ost ;
	char ** nm ;
	int * perm, k ;
	struct sm_item * copy ;
	if( n < 2 || !folder ) return ;
	nm = snewn( n, char * ) ;
	perm = snewn( n, int ) ;
	copy = snewn( n, struct sm_item ) ;
	for( k = 0 ; k < n ; k++ ) nm[k] = v[k].label ;
	ksp_order_store_current( &ost ) ;
	ksp_order_sort_level( &ost, folder, kind, nm, n, perm, NULL ) ;
	ksp_order_store_free( &ost ) ;
	for( k = 0 ; k < n ; k++ ) copy[k] = v[perm[k]] ;
	memcpy( v, copy, n * sizeof(*v) ) ;
	sfree( copy ) ;
	sfree( perm ) ;
	sfree( nm ) ;
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
 * first 26. icons: each entry's data is a session name, its icon goes in
 * front of the text (hknet/KiTTY#62, kitty_sessicon.c), for the theme
 * sm_icons_dark records. Returns how many were appended. */
static int sm_icons_dark = -1 ;	/* the theme the launcher menu's icons were made for */
static int sm_append_entries( HMENU menu, struct sm_item *items, int nitems, int accel, int icons ) {
	char * buffer ;
	int k, nb, n = 0, px = GetSystemMetrics( SM_CXSMICON ) ;
	for( k = 0 ; k < nitems ; k++ ) {
		if( ( nb = kitty_menuslots_add( items[k].data ) ) < 0 ) continue ;
		if( accel && nb < 26 )
			buffer = dupprintf( "%s\tCtrl+Shift+%c", items[k].label, ('A'+nb) ) ;
		else
			buffer = dupstr( items[k].label ) ;
		AppendMenu( menu, MF_ENABLED, IDM_USERCMD+nb, buffer ) ;
		sfree( buffer ) ;
		if( icons ) {
			/* the bitmap stays the cache's: a menu never frees hbmpItem */
			HBITMAP bm = kitty_session_icon_bitmap( items[k].data, px, sm_icons_dark > 0 ) ;
			if( bm ) {
				MENUITEMINFOA mii ;
				memset( &mii, 0, sizeof(mii) ) ;
				mii.cbSize = sizeof(mii) ;
				mii.fMask = MIIM_BITMAP ;
				mii.hbmpItem = bm ;
				SetMenuItemInfoA( menu, IDM_USERCMD+nb, FALSE, &mii ) ;
				}
			}
		n++ ;
		}
	return n ;
	}

/* ReadSpecialMenu's collection of one key, for the settings module's
 * listings: the submenus by their key names, the items by their values. */
struct sm_collect {
	struct sm_item **items ;
	int *nitems ;
	int launcher, first, separator ;
	const char *key ;
	HMENU menu ;
	} ;
static int sm_sub_one( const char *lpData, void *ctx ) {
	struct sm_collect *c = (struct sm_collect *)ctx ;
	char * plain = unmungestr_dup( lpData ) ;
	sm_items_add( c->items, c->nitems, plain, lpData ) ;
	sfree( plain ) ;
	return 1 ;
	}
static int sm_value_one( const char *achValue, void *ctx ) {
	struct sm_collect *c = (struct sm_collect *)ctx ;
	char * lpData ;
	if( c->first ) {   /* the separator before the first value, as before */
		if( c->separator ) AppendMenu( c->menu, MF_SEPARATOR, 0, 0 ) ;
		c->first = 0 ;
		}
	lpData = kitty_settings_reg_stored_dup_at( c->key, achValue, NULL, NULL ) ;
	if( lpData == NULL ) return 1 ;
	/* Default Settings never appears in the launcher, at ANY level of
	 * its hand-edited copy (reload=no). */
	if( strcmp(achValue,KITTY_DEFAULT_SESSION) || !c->launcher )
		sm_items_add( c->items, c->nitems, achValue, lpData ) ;
	free( lpData ) ;
	return 1 ;
	}

int ReadSpecialMenu( HMENU menu, char * KeyName, int * nbitem, int separator ) {
	struct sm_collect sm_ctx ;
	HMENU SubMenu ;
	char * buffer, * p ;
	int i ;
	int local_nb = 0 ;
	if( (IniFileFlag == SAVEMODE_REG)||(IniFileFlag == SAVEMODE_FILE) ) {
	if( kitty_settings_reg_key_check( KeyName ) == ERROR_SUCCESS ) {
		/* the key's subkeys and values through the settings module */

		char * launcherkey, * lfolder ;
		struct sm_item *items = NULL ;
		int nitems = 0, k, is_launcher ;
		launcherkey = dupprintf( "%s\\Launcher", kitty_registry_base() ) ;
		is_launcher = !strncmp( KeyName, launcherkey, strlen(launcherkey) ) ;
		lfolder = is_launcher ? sm_launcher_folder( KeyName + strlen(launcherkey) ) : NULL ;
		sfree( launcherkey ) ;

		sm_ctx.items = &items ; sm_ctx.nitems = &nitems ; sm_ctx.launcher = is_launcher ;
		kitty_settings_reg_subkeys_at( KeyName, sm_sub_one, &sm_ctx ) ; // collect the submenus
		if( is_launcher ) sm_items_sort( items, nitems ) ;
		sm_items_order( items, nitems, lfolder, KSP_ORDER_FOLDER ) ;
		for( k = 0 ; k < nitems ; k++ ) {
			SubMenu = CreateMenu() ;
			buffer = dupprintf( "%s\\%s", KeyName, items[k].data ) ;
			sm_read_folder( menu, SubMenu, buffer, nbitem, items[k].label ) ;
			sfree( buffer ) ;
			}
		sm_items_free( &items, &nitems ) ;


		sm_ctx.key = KeyName ; sm_ctx.first = 1 ; sm_ctx.menu = menu ; sm_ctx.separator = separator ;
		kitty_settings_reg_names_at( KeyName, sm_value_one, &sm_ctx ) ; // collect the menu items
		if( is_launcher ) sm_items_sort( items, nitems ) ;
		sm_items_order( items, nitems, lfolder, KSP_ORDER_SESSION ) ;
		local_nb += sm_append_entries( menu, items, nitems, ShortcutsFlag, 0 ) ;
		sm_items_free( &items, &nitems ) ;

		(*nbitem)=kitty_menuslots_count() ;
		sfree( lfolder ) ;

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
			char * lfolder = is_launcher ? sm_launcher_folder( KeyName + 8 ) : NULL ;
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
			sm_items_order( items, nitems, lfolder, KSP_ORDER_FOLDER ) ;
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
				 * level of its hand-edited copy (reload=no), and by the
				 * unmunged name. */
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
			sm_items_order( items, nitems, lfolder, KSP_ORDER_SESSION ) ;
			local_nb += sm_append_entries( menu, items, nitems, 0, 0 ) ;
			sm_items_free( &items, &nitems ) ;
			(*nbitem)=kitty_menuslots_count() ;
			closedir( dir ) ;
			sfree( lfolder ) ;
			}
		sfree( fullpath ) ;
		}
	return local_nb ;
	}

/* [Launcher] reload=yes (the launcher, InitLauncherMenu): the session menu
 * built from the entries LauncherCollectSessions keeps in memory - the same
 * levels, order, ceiling and accelerators as ReadSpecialMenu gives the
 * hand-edited Launcher copy: the folders one level below `prefix`
 * first, each a submenu, then the sessions of `prefix` itself, both by the
 * name shown. Returns the entries appended at this level. */
int ReadLauncherEntries( HMENU menu, const struct kitty_launcher_entry * e, int n,
                         const char * prefix, int * nbitem ) {
	struct sm_item *items = NULL ;
	int nitems = 0, i, k, local_nb ;
	size_t pl = strlen( prefix ) ;
	/* the top level's call: the theme every level's icons are made for */
	if( !pl ) sm_icons_dark = kitty_session_icons_dark() ? 1 : 0 ;
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
		else if( strcmp( comp, items[k].label ) < 0 ) {
			/* one folder whatever the case: the spelling first by byte */
			sfree( items[k].label ) ; sfree( items[k].data ) ;
			items[k].label = dupstr( comp ) ; items[k].data = dupstr( comp ) ;
			}
		sfree( comp ) ;
		}
	sm_items_sort( items, nitems ) ;
	sm_items_order( items, nitems, prefix, KSP_ORDER_FOLDER ) ;
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
	sm_items_order( items, nitems, prefix, KSP_ORDER_SESSION ) ;
	local_nb = sm_append_entries( menu, items, nitems,
	                              IniFileFlag != SAVEMODE_DIR && ShortcutsFlag,
	                              kitty_session_icons_on() ) ;
	sm_items_free( &items, &nitems ) ;
	(*nbitem) = kitty_menuslots_count() ;
	return local_nb ;
	}

/* KiTTY (hknet/KiTTY#62): the launcher's menu is built at its start and on
 * Refresh. It is opened many times in between: when KiTTY++'s theme switched
 * since, each session entry (IDM_USERCMD + n, its session in the menu
 * slots) takes the icon for the theme now in force, at every level. */
static void sm_icons_apply( HMENU m, int dark, int px, int depth ) {
	int i, n = GetMenuItemCount( m ) ;
	if( depth > 32 ) return ;
	for( i = 0 ; i < n ; i++ ) {
		HMENU sub = GetSubMenu( m, i ) ;
		UINT id ;
		const char * s ;
		if( sub ) { sm_icons_apply( sub, dark, px, depth + 1 ) ; continue ; }
		id = GetMenuItemID( m, i ) ;
		if( id < IDM_USERCMD || id >= IDM_USERCMD + NB_MENU_MAX ) continue ;
		if( ( s = kitty_menuslots_get( (int)( id - IDM_USERCMD ) ) ) == NULL ) continue ;
		{
			MENUITEMINFOA mii ;
			memset( &mii, 0, sizeof(mii) ) ;
			mii.cbSize = sizeof(mii) ;
			mii.fMask = MIIM_BITMAP ;
			mii.hbmpItem = kitty_session_icon_bitmap( s, px, dark != 0 ) ;
			SetMenuItemInfoA( m, i, TRUE, &mii ) ;
		}
	}
}
void kitty_launcher_menu_icons_follow( HMENU m ) {
	int dark ;
	if( m == NULL || sm_icons_dark < 0 || !kitty_session_icons_on() ) return ;
	dark = kitty_session_icons_dark() ? 1 : 0 ;
	if( dark == sm_icons_dark ) return ;
	sm_icons_apply( m, dark, GetSystemMetrics( SM_CXSMICON ), 0 ) ;
	sm_icons_dark = dark ;
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
