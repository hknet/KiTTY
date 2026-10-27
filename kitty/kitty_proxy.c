/*
 * kitty_proxy.c - named proxy definitions: a set of proxy settings saved
 * under a name and picked per session, instead of retyping host, port,
 * user and password in every session.
 * It enumerates the definitions into proxies[] (registry key Proxies\<name>,
 * or a Proxies\ folder in portable mode), loads one into a Conf and saves
 * one back, and decides when the config box offers the choice droplist and
 * the Edit button. Passwords are protected at rest through the same backend
 * policy as session passwords, and wrapped in memory once loaded.
 * It also carries the definitions through the migration from the legacy hive
 * and through the whole-store export and import bundle.
 */
#include "putty.h"
#include "kitty.h"
#include "kitty_tools.h"
#include "kitty_registry.h"
#include "kitty_proxy.h"
#include "kitty_store.h"
#include "dialog.h"
#include "storage.h"
#include <sys/types.h>
#include <dirent.h>
#include <windows.h>
#include "kitty_msgbox.h"   /* themed MessageBox routing */
#include "kitty_text.h"     /* shared captions */
#include "kitty_pwmem.h"    /* passwords wrapped in memory */
#include "kitty_storage.h"
#include "kitty_inifile.h"   /* kitty_store_readonly: readonly=yes */
#include "kitty_secretstore.h"
#include "kitty_win.h"
#include "kitty_winutil.h"
#include "kitty_store_acl.h"   /* the Proxies folder: owner-only rights */

/* The registry hive chosen AT RUNTIME (kitty_set_registry_root, driven by
 * kitty.ini KiClassName). Named proxies used the compile-time PUTTY_REG_POS
 * macro while sessions used this, so with KiClassName=PuTTY the sessions and
 * the proxies - passwords included - landed in DIFFERENT hives. */


/* Proxy-choice selector visibility (kitty.ini [ConfigBox] proxyselection):
 *   1  = yes   -> always shown
 *   0  = auto  -> shown when the user has proxy definitions (default)
 *  -1  = no    -> never shown
 * (hknet/KiTTY#11: old KiTTY only had off/on via "yes"; auto makes it
 * discoverable, "no" lets a user with proxies defined still hide it.) */
static int ProxySelectionFlag = 0 ;
int GetProxySelectionFlag(void) { return ProxySelectionFlag ; }
void SetProxySelectionFlag( const int flag ) { ProxySelectionFlag = flag ; }


/* At-rest password protection shared with sessions (windows/storage.c): wrap for
 * the active backend (registry DPAPI / portable MPW / explicit legacy), unwrap by
 * stored marker. So a named proxy's password is protected exactly like a session
 * password (hknet/KiTTY#11). */

struct Proxies proxies[MAX_PROXY] ;

/* True once InitProxyList() has found at least one user-defined proxy (beyond
 * the two built-in KITTY_PROXY_SESSION / KITTY_PROXY_NONE entries at
 * indices 0/1). Lets the config box show the Proxy-choice droplist
 * automatically for users who actually have proxies defined, without needing
 * the explicit [ConfigBox] proxyselection=yes flag (hknet/KiTTY#11). */
int kitty_has_proxy_definitions( void ) { return proxies[2].name != NULL ; }

/* Whether the Proxy-choice droplist is shown (and its selection applied on
 * connect): forced by proxyselection=yes/no, else auto (shown iff proxies
 * are defined). Single source of truth for the config box, the connect-time
 * apply path and the config-box height accounting. */
int kitty_proxy_choice_shown( void ) {
	if( ProxySelectionFlag > 0 ) return 1 ;   /* yes */
	if( ProxySelectionFlag < 0 ) return 0 ;   /* no  */
	return kitty_has_proxy_definitions() ;    /* auto */
}

/* Whether the named-proxy Edit button is offered: everything except "no".
 * Looser than kitty_proxy_choice_shown() so a fresh auto setup still has an
 * entry point (the Connection/Proxy panel button) to create the first proxy. */
int kitty_proxy_editor_available( void ) { return ProxySelectionFlag >= 0 ; }

/* Resolve CONF_proxyselection at load/connect (hknet/KiTTY#11): a named proxy
 * that no longer exists (or KITTY_PROXY_SESSION / empty) collapses to
 * KITTY_PROXY_SESSION when the session has its OWN proxy configured,
 * else KITTY_PROXY_NONE. KITTY_PROXY_NONE and an existing named proxy are kept.
 * Needs proxies[] populated (InitProxyList, done at startup). */
void kitty_proxy_resolve_selection( Conf *conf ) {
	const char *cur = conf_get_str( conf, CONF_proxyselection ) ;
	if( cur && !strcmp( cur, KITTY_PROXY_NONE ) ) return ;
	if( cur && cur[0] && strcmp( cur, KITTY_PROXY_SESSION ) ) {
		for( int i = 2 ; i < MAX_PROXY && proxies[i].name ; i++ )
			if( !strcmp( cur, proxies[i].name ) ) return ;   /* still exists */
	}
	conf_set_str( conf, CONF_proxyselection,
		conf_get_int( conf, CONF_proxy_type ) != PROXY_NONE
			? KITTY_PROXY_SESSION : KITTY_PROXY_NONE ) ;
}

void InitProxyList(void) {
	HKEY hKey ;
	int i,j;
	for( i=0; i<lenof(proxies); i++) {
		if( proxies[i].name != NULL ) { free( proxies[i].name ) ; }   /* re-callable, no leak */
		proxies[i].name = NULL;
		proxies[i].val = i;
	}
	/* sizeof, not a hand-counted length: these were malloc(26) and malloc(13),
	 * which happened to be right for the strings as spelled and would have
	 * overflowed the moment either was edited. */
	proxies[0].name=(char*)malloc(sizeof(KITTY_PROXY_SESSION)); strcpy(proxies[0].name,KITTY_PROXY_SESSION);
	proxies[1].name=(char*)malloc(sizeof(KITTY_PROXY_NONE));    strcpy(proxies[1].name,KITTY_PROXY_NONE);
	j=2;
	if( (IniFileFlag == SAVEMODE_REG)||(IniFileFlag == SAVEMODE_FILE) ) {
		TCHAR 	achClass[MAX_PATH] = TEXT("");
		DWORD   cchClassName=MAX_PATH,cSubKeys=0,cbMaxSubKey,cchMaxClass;
		DWORD	cValues,cchMaxValue,cbMaxValueData,cbSecurityDescriptor;
		FILETIME ftLastWriteTime;
		char *key = dupprintf( "%s\\Proxies", kitty_registry_base() ) ;
		LONG r ;
		RegTestOrCreate( HKEY_CURRENT_USER, key, NULL, NULL ) ;
		r = RegOpenKeyEx( HKEY_CURRENT_USER, key, 0, KEY_READ, &hKey) ;
		sfree( key ) ;
		if( r != ERROR_SUCCESS ) return ;
		if( RegQueryInfoKey(hKey,achClass,&cchClassName,NULL,&cSubKeys,&cbMaxSubKey,&cchMaxClass,&cValues,&cchMaxValue,&cbMaxValueData,&cbSecurityDescriptor,&ftLastWriteTime) != ERROR_SUCCESS )
			cSubKeys = 0 ;
		if( cSubKeys>0 )
			for (i=0; i<cSubKeys; i++) {
				/* a key name is at most 255 characters */
				char lpData[MAX_KEY_LENGTH + 1] ;
				DWORD cchValue = sizeof(lpData) ;
				if( j>=MAX_PROXY ) break; /* SECURITY: bound proxies[] */
				if( RegEnumKeyEx(hKey, i, lpData, &cchValue, NULL, NULL, NULL, &ftLastWriteTime) == ERROR_SUCCESS ) {
					if( strcmp(lpData,"None") && strcmp(lpData,"Default") ) {
						/* unescaping never makes a name longer */
						proxies[j].name=(char*)malloc(strlen(lpData)+1);
						unmungestr( lpData, proxies[j].name, (int)strlen(lpData)+1 ) ;
						j++;
					}
				}
			}
		RegCloseKey( hKey ) ;
	} else if( IniFileFlag == SAVEMODE_DIR ) {
		char *pdir = portable_config_subdir( "Proxies" ) ;
		DIR * dir ;
		struct dirent * de ;
		/* Reading only: this runs at every connect. No Proxies folder = no
		 * named proxies; the folder is made by the first save (SaveProxyInfo).
		 * It used to be made here, with a box when that failed - at every
		 * connect from a write-protected store. */
		if( pdir && pdir[0] && (dir=opendir(pdir)) != NULL ) {
			while( (de=readdir(dir)) != NULL )
			if( j>=MAX_PROXY ) break; /* SECURITY: bound proxies[] */
			else if( de->d_name[0] != '.' )	{	/* ., .., .gitkeep */
				DWORD a ;
				char *fullpath = dupprintf( "%s\\%s", pdir, de->d_name ) ;
				/* Files only. The test was !(attributes & NORMAL): it skipped a
				 * file whose only attribute is NORMAL and listed folders. */
				a = GetFileAttributes( fullpath ) ;
				sfree( fullpath ) ;
				if( a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY) ) {
					if( strcmp(de->d_name,"None") && strcmp(de->d_name,"Default") ) {
						proxies[j].name=(char*)malloc(strlen(de->d_name)+1);
						unmungestr( de->d_name, proxies[j].name, (int)strlen(de->d_name)+1 ) ;
						j++;
					}
				}
			}
			closedir(dir) ;
		}
		sfree( pdir ) ;
	}
}

/* The definition's file in the folder store: <configdir>\Proxies\<escaped
 * name> in full (sfree it); NULL when there is no configuration folder. */
static char *proxy_file_path( const char *name ) {
	char *pdir = portable_config_subdir( "Proxies" ), *fn, *path ;
	if( pdir == NULL ) return NULL ;
	fn = mungestr_dup( name ) ;
	path = dupprintf( "%s\\%s", pdir, fn ) ;
	sfree( fn ) ;
	sfree( pdir ) ;
	return path ;
}

/* The definition's registry key below the hive in use, in full (sfree it). */
static char *proxy_reg_key( const char *name ) {
	char *m = mungestr_dup( name ) ;
	char *key = dupprintf( "%s\\Proxies\\%s", kitty_registry_base(), m ) ;
	sfree( m ) ;
	return key ;
}

int LoadProxyInfo( Conf * conf, const char * name ) {
	if( !strcmp(name,KITTY_PROXY_SESSION) ) { return 0 ; }
	if( !strcmp(name,KITTY_PROXY_NONE) ) {
		debug_logevent( "Remove proxy definition" ) ;
		conf_set_int(conf, CONF_proxy_type, PROXY_NONE) ;
		return 1 ;
	}
	debug_logevent( "Load proxy \"%s\" definition", name ) ;
	if( (IniFileFlag == SAVEMODE_REG)||(IniFileFlag == SAVEMODE_FILE) ) {
		HKEY hKey ;
		char *buffer = proxy_reg_key( name ) ;
		char *lpData ;
		if( RegOpenKeyEx( HKEY_CURRENT_USER, buffer, 0, KEY_READ, &hKey) != ERROR_SUCCESS ) {
			debug_logevent( "Unable to load proxy definition" ) ;
			sfree( buffer ) ;
			return 0;
		}
		/* each value read in full (GetValueDataDup); NULL = not there */
		#define PXV(N) ( lpData = GetValueDataDup( HKEY_CURRENT_USER, buffer, (N) ) )
		if( PXV( "ProxyExcludeList" ) ) {
			conf_set_str( conf, CONF_proxy_exclude_list, lpData ) ;
			sfree( lpData ) ;
		}
		if( PXV( "ProxyDNS" ) ) {
			int i=atoi(lpData);
			conf_set_int(conf, CONF_proxy_dns, (i+1)%3);
			sfree( lpData ) ;
		}
		if( PXV( "ProxyLocalhost" ) ) {
			conf_set_bool( conf, CONF_even_proxy_localhost, atoi(lpData) != 0 ) ;
			sfree( lpData ) ;
		}
		if( PXV( "ProxyMethod" ) ) {
			int i = atoi(lpData) ;
			if (i == 0) conf_set_int(conf, CONF_proxy_type, PROXY_NONE);
			else if (i == 1) conf_set_int(conf, CONF_proxy_type, PROXY_SOCKS4) ;
			else if (i == 2) conf_set_int(conf, CONF_proxy_type, PROXY_SOCKS5) ;
			else if (i == 3) conf_set_int(conf, CONF_proxy_type, PROXY_HTTP) ;
			else if (i == 4) conf_set_int(conf, CONF_proxy_type, PROXY_TELNET) ;
			else if (i == 5) conf_set_int(conf, CONF_proxy_type, PROXY_CMD) ;
			else if (i == 6) conf_set_int(conf, CONF_proxy_type, PROXY_SSH_TCPIP) ;
			else if (i == 7) conf_set_int(conf, CONF_proxy_type, PROXY_SSH_EXEC) ;
			else if (i == 8) conf_set_int(conf, CONF_proxy_type, PROXY_SSH_SUBSYSTEM) ;
			else conf_set_int(conf, CONF_proxy_type, PROXY_NONE) ;
			sfree( lpData ) ;
		}
		if( PXV( "ProxyHost" ) ) { conf_set_str( conf, CONF_proxy_host, lpData ) ; sfree( lpData ) ; }
		if( PXV( "ProxyPort" ) ) { conf_set_int( conf, CONF_proxy_port, atoi(lpData) ) ; sfree( lpData ) ; }
		if( PXV( "ProxyUsername" ) ) { conf_set_str( conf, CONF_proxy_username, lpData ) ; sfree( lpData ) ; }
		if( PXV( "ProxyPassword" ) ) {
			char *pt = NULL ; kitty_secret_unwrap( lpData, &pt ) ;   /* DPAPI/MPW/plain */
			/* Wrapped again for memory, not left in the clear in the Conf. */
			kitty_pw_set( conf, CONF_proxy_password, pt ? pt : "" ) ;
			if( pt ) { memset( pt, 0, strlen(pt) ) ; free( pt ) ; }
			smemclr( lpData, strlen(lpData) ) ;
			sfree( lpData ) ;
		}
		if( PXV( "ProxyTelnetCommand" ) ) { conf_set_str( conf, CONF_proxy_telnet_command, lpData ) ; sfree( lpData ) ; }
		if( PXV( "ProxyLogToTerm" ) ) { conf_set_int( conf, CONF_proxy_log_to_term, atoi(lpData) ) ; sfree( lpData ) ; }
		/* Absent means the proxy sets nothing, so this is not read with a 0
		 * default: 0 is an answer here. */
		if( PXV( "ProxyHostIs" ) ) {
			conf_set_int( conf, CONF_proxy_host_kind, atoi(lpData) ? 1 : 0 ) ;
			sfree( lpData ) ;
		} else
			conf_set_int( conf, CONF_proxy_host_kind, -1 ) ;
		#undef PXV
		RegCloseKey( hKey ) ;
		sfree( buffer ) ;
	} else if( IniFileFlag == SAVEMODE_DIR ) {
		char *fullpath = proxy_file_path( name ) ;
		struct ksf_item *items ;
		const char *v ;
		/* Read with the session-file reader (ksf_load): it takes both line
		 * forms, Key=value and old KiTTY's Key\value\, and hex escapes in
		 * either case. The line reader used before knew only Key\value\, so
		 * a definition written as Key=value loaded empty (no host, port 0). */
		items = fullpath ? ksf_load( fullpath ) : NULL ;
		sfree( fullpath ) ;
		if( items == NULL ) return 1 ;
		if( (v = ksf_list_get( items, "ProxyExcludeList" )) != NULL ) conf_set_str( conf, CONF_proxy_exclude_list, v ) ;
		if( (v = ksf_list_get( items, "ProxyDNS" )) != NULL ) conf_set_int( conf, CONF_proxy_dns, (atoi(v)+1)%3 ) ;
		if( (v = ksf_list_get( items, "ProxyLocalhost" )) != NULL ) conf_set_bool( conf, CONF_even_proxy_localhost, atoi(v) != 0 ) ;
		if( (v = ksf_list_get( items, "ProxyMethod" )) != NULL ) {
			int i = atoi(v) ;
			if (i == 0) conf_set_int(conf, CONF_proxy_type, PROXY_NONE);
			else if (i == 1) conf_set_int(conf, CONF_proxy_type, PROXY_SOCKS4) ;
			else if (i == 2) conf_set_int(conf, CONF_proxy_type, PROXY_SOCKS5) ;
			else if (i == 3) conf_set_int(conf, CONF_proxy_type, PROXY_HTTP) ;
			else if (i == 4) conf_set_int(conf, CONF_proxy_type, PROXY_TELNET) ;
			else if (i == 5) conf_set_int(conf, CONF_proxy_type, PROXY_CMD) ;
			else if (i == 6) conf_set_int(conf, CONF_proxy_type, PROXY_SSH_TCPIP) ;
			else if (i == 7) conf_set_int(conf, CONF_proxy_type, PROXY_SSH_EXEC) ;
			else if (i == 8) conf_set_int(conf, CONF_proxy_type, PROXY_SSH_SUBSYSTEM) ;
			else conf_set_int(conf, CONF_proxy_type, PROXY_NONE) ;
		}
		if( (v = ksf_list_get( items, "ProxyHost" )) != NULL ) conf_set_str( conf, CONF_proxy_host, v ) ;
		if( (v = ksf_list_get( items, "ProxyPort" )) != NULL ) conf_set_int( conf, CONF_proxy_port, atoi(v) ) ;
		if( (v = ksf_list_get( items, "ProxyUsername" )) != NULL ) conf_set_str( conf, CONF_proxy_username, v ) ;
		if( (v = ksf_list_get( items, "ProxyPassword" )) != NULL ) {
			char *pt = NULL ; kitty_secret_unwrap( v, &pt ) ;   /* DPAPI/MPW/plain */
			/* Wrapped again for memory (kitty_pwmem.c). */
			kitty_pw_set( conf, CONF_proxy_password, pt ? pt : "" ) ;
			if( pt ) { memset( pt, 0, strlen(pt) ) ; free( pt ) ; }
		}
		if( (v = ksf_list_get( items, "ProxyTelnetCommand" )) != NULL ) conf_set_str( conf, CONF_proxy_telnet_command, v ) ;
		if( (v = ksf_list_get( items, "ProxyLogToTerm" )) != NULL ) conf_set_int( conf, CONF_proxy_log_to_term, atoi(v) ) ;
		/* Absent means the definition follows the global default, as in the
		 * registry branch above: -1, not the value the Conf happened to hold. */
		v = ksf_list_get( items, "ProxyHostIs" ) ;
		conf_set_int( conf, CONF_proxy_host_kind, v ? (atoi(v) ? 1 : 0) : -1 ) ;
		ksf_list_free( items ) ;
	}
	return 1;
}

/* Map PuTTY's CONF_proxy_type to the stored ProxyMethod int (mirror of the
 * switch in LoadProxyInfo). */
static int proxy_method_from_conf( Conf *conf ) {
	switch( conf_get_int(conf, CONF_proxy_type) ) {
		case PROXY_SOCKS4: return 1 ;
		case PROXY_SOCKS5: return 2 ;
		case PROXY_HTTP:   return 3 ;
		case PROXY_TELNET: return 4 ;
		case PROXY_CMD:    return 5 ;
		/* 6-8 are a KiTTY-0.84 extension (classic 9bis had no SSH proxy);
		 * an old KiTTY reading such a definition falls back to None. */
		case PROXY_SSH_TCPIP:     return 6 ;
		case PROXY_SSH_EXEC:      return 7 ;
		case PROXY_SSH_SUBSYSTEM: return 8 ;
		default:           return 0 ;   /* PROXY_NONE */
	}
}

/* Create/update a named proxy definition from the current CONF_proxy_* values.
 * 1 saved; 0 the folder store's file (or its Proxies folder) not written,
 * Windows' reason in GetLastError().
 * name must be a real definition (not a built-in). Registry -> PUTTY_REG_POS\
 * Proxies\<name>; portable -> ConfigDirectory\Proxies\<name>, one "Key\value\"
 * line per field (the format ReadPortableValue expects). ProxyPassword is
 * protected at rest via the shared backend policy (DPAPI registry / MPW
 * portable / explicit legacy), the same chokepoint as session passwords. */
int SaveProxyInfo( Conf *conf, const char *name ) {
	/* readonly=yes: not stored (and no master password prompt to wrap
	 * the password), KITTY_RO_REFUSED - DeleteProxyInfo the same */
	if( kitty_store_readonly() ) return KITTY_RO_REFUSED ;
	kitty_store_mark_dirty() ;   /* named proxies live in the store too */
	if( name == NULL || name[0] == '\0' ) return 0 ;
	if( !strcmp(name,KITTY_PROXY_SESSION) || !strcmp(name,KITTY_PROXY_NONE) ) return 0 ;
	int method = proxy_method_from_conf( conf ) ;
	int dns    = (conf_get_int(conf, CONF_proxy_dns) + 2) % 3 ;   /* mirror LoadProxyInfo */
	int local  = conf_get_bool(conf, CONF_even_proxy_localhost) ? 1 : 0 ;
	/* Protect the password at rest via the shared backend policy (may prompt to
	 * create/unlock the master password in portable mode; empty -> "", no prompt). */
	char pxpw[KITTY_PW_MAX + 1] ;   /* the Conf holds it wrapped for memory */
	kitty_pw_get( conf, CONF_proxy_password, pxpw, sizeof(pxpw) ) ;
	/* the wrap refused (the master password locked, DPAPI failing): the
	 * stored password stays, never DPAPI, the plaintext or an empty value
	 * in its place, and the user learns it */
	int pwlocked = 0 ;
	int saved = 1 ;          /* 0: the folder store's file not written */
	DWORD saverr = 0 ;
	char *pwblob = kitty_secret_wrap_current_backend_kept( pxpw, &pwlocked ) ;
	if( (IniFileFlag == SAVEMODE_REG) || (IniFileFlag == SAVEMODE_FILE) ) {
		char *sub = proxy_reg_key( name ) ;
		RegTestOrCreate( HKEY_CURRENT_USER, sub, NULL, NULL ) ;
		RegTestOrCreate( HKEY_CURRENT_USER, sub, "ProxyExcludeList", conf_get_str(conf, CONF_proxy_exclude_list) ) ;
		RegTestOrCreateDWORD( HKEY_CURRENT_USER, sub, "ProxyDNS", dns ) ;
		RegTestOrCreateDWORD( HKEY_CURRENT_USER, sub, "ProxyLocalhost", local ) ;
		RegTestOrCreateDWORD( HKEY_CURRENT_USER, sub, "ProxyMethod", method ) ;
		RegTestOrCreate( HKEY_CURRENT_USER, sub, "ProxyHost", conf_get_str(conf, CONF_proxy_host) ) ;
		RegTestOrCreateDWORD( HKEY_CURRENT_USER, sub, "ProxyPort", conf_get_int(conf, CONF_proxy_port) ) ;
		RegTestOrCreate( HKEY_CURRENT_USER, sub, "ProxyUsername", conf_get_str(conf, CONF_proxy_username) ) ;
		if( !pwlocked )
			RegTestOrCreate( HKEY_CURRENT_USER, sub, "ProxyPassword", pwblob ? pwblob : "" ) ;
		RegTestOrCreate( HKEY_CURRENT_USER, sub, "ProxyTelnetCommand", conf_get_str(conf, CONF_proxy_telnet_command) ) ;
		RegTestOrCreateDWORD( HKEY_CURRENT_USER, sub, "ProxyLogToTerm", conf_get_int(conf, CONF_proxy_log_to_term) ) ;
		if( conf_get_int(conf, CONF_proxy_host_kind) < 0 )
			RegDelValue( HKEY_CURRENT_USER, sub, "ProxyHostIs" ) ;   /* sets nothing */
		else
			RegTestOrCreateDWORD( HKEY_CURRENT_USER, sub, "ProxyHostIs", conf_get_int(conf, CONF_proxy_host_kind) ) ;
		sfree( sub ) ;
	} else if( IniFileFlag == SAVEMODE_DIR ) {
		/* Read first and saved through the folder store's line list
		 * (kitty_storage.c ksf_*), as a session file is: the lines keep their
		 * order, a comment, a blank line or a tool's key stays where it is, an
		 * unchanged value keeps its line as written and its line ending, a
		 * changed one is replaced in its place and a new key goes at the end -
		 * in the classic "Key\value\" form the proxy readers take
		 * (ReadPortableValue). The file used to be written afresh in a fixed
		 * order, so every save was a diff of the whole file.
		 * The Proxies folder is made here, by the first save, and nowhere else. */
		char *dir = portable_config_subdir( "Proxies" ), *fullpath = NULL, num[16] ;
		struct ksf_item *items ;
		if( dir == NULL ) {
			saverr = ERROR_PATH_NOT_FOUND ;   /* no configuration folder */
			saved = 0 ;
		} else {
			char *fn = mungestr_dup( name ) ;
			fullpath = dupprintf( "%s\\%s", dir, fn ) ;
			sfree( fn ) ;
			kitty_store_acl_mkdir( dir ) ;   /* the folder above missing: MakeDir makes both */
			if( !MakeDir( dir ) ) {
				saverr = GetLastError() ;
				saved = 0 ;
			}
		}
		items = saved ? ksf_load( fullpath ) : NULL ;
		#define SPS(K,V) ksf_list_set( &items, (K), (V) )
		#define SPD(K,D) do { snprintf( num, sizeof(num), "%d", (D) ) ; ksf_list_set( &items, (K), num ) ; } while(0)
		SPS( "ProxyExcludeList", conf_get_str(conf, CONF_proxy_exclude_list) ) ;
		SPD( "ProxyDNS", dns ) ;
		SPD( "ProxyLocalhost", local ) ;
		SPD( "ProxyMethod", method ) ;
		SPS( "ProxyHost", conf_get_str(conf, CONF_proxy_host) ) ;
		SPD( "ProxyPort", conf_get_int(conf, CONF_proxy_port) ) ;
		SPS( "ProxyUsername", conf_get_str(conf, CONF_proxy_username) ) ;
		/* the wrap refused, or the same password as stored: its line stays
		 * as it is (a fresh wrap differs on every save) */
		if( !pwlocked && !ksec_stored_keeps( ksf_list_get( items, "ProxyPassword" ), pxpw, store_is_file() ) )
			SPS( "ProxyPassword", pwblob ? pwblob : "" ) ;
		SPS( "ProxyTelnetCommand", conf_get_str(conf, CONF_proxy_telnet_command) ) ;
		SPD( "ProxyLogToTerm", conf_get_int(conf, CONF_proxy_log_to_term) ) ;
		if( conf_get_int(conf, CONF_proxy_host_kind) >= 0 )
			SPD( "ProxyHostIs", conf_get_int(conf, CONF_proxy_host_kind) ) ;
		else
			ksf_list_del( &items, "ProxyHostIs" ) ;   /* none given */
		#undef SPS
		#undef SPD
		if( saved && !ksf_save_classic( fullpath, items ) ) {
			saverr = GetLastError() ;
			saved = 0 ;
		}
		ksf_list_free( items ) ;
		sfree( fullpath ) ;
		sfree( dir ) ;
	}
	smemclr( pxpw, sizeof(pxpw) ) ;
	if( pwblob ) { memset( pwblob, 0, strlen(pwblob) ) ; free( pwblob ) ; }
	if( pwlocked && saved ) ksec_report_locked_save( KSEC_KEPT_PROXY, name ) ;
	if( !saved ) SetLastError( saverr ? saverr : ERROR_WRITE_FAULT ) ;
	return saved ;
}

/* Delete a named proxy definition (registry key or portable file). */
int DeleteProxyInfo( const char *name ) {
	if( name == NULL || name[0] == '\0' ) return 0 ;
	if( kitty_store_readonly() ) return KITTY_RO_REFUSED ;
	if( !strcmp(name,KITTY_PROXY_SESSION) || !strcmp(name,KITTY_PROXY_NONE) ) return 0 ;
	if( (IniFileFlag == SAVEMODE_REG) || (IniFileFlag == SAVEMODE_FILE) ) {
		char *sub = proxy_reg_key( name ) ;
		RegDeleteKey( HKEY_CURRENT_USER, sub ) ;
		sfree( sub ) ;
	} else if( IniFileFlag == SAVEMODE_DIR ) {
		char *fullpath = proxy_file_path( name ) ;
		if( fullpath ) remove( fullpath ) ;
		sfree( fullpath ) ;
	}
	return 1 ;
}

/* One-time, marker-gated migration of named proxies from the legacy 9bis hive
 * (Software\9bis.com\KiTTY\Proxies) into ours (PUTTY_REG_POS\Proxies). Runs at
 * startup in registry mode only (portable reads its Proxies\ folder in place);
 * INDEPENDENT of the session-hive migration so it also fires for users who
 * migrated sessions in an earlier build. Registry-only, so at-rest protection
 * is DPAPI (transparent, account-bound, losslessly reversible - no prompt):
 *   - a proxy absent on our side is copied over whole, then its ProxyPassword
 *     is DPAPI-encrypted;
 *   - a proxy already present keeps its (possibly user-edited) fields, but a
 *     still-plaintext ProxyPassword is DPAPI-encrypted in place - this also
 *     auto-protects proxies the whole-tree MigrateOldKittyHive copied earlier.
 * A marked (already-encrypted) password is never re-wrapped. The one-shot marker
 * means a proxy the user later deletes never resurrects (hknet/KiTTY#11). */
static int kitty_proxies_migrated( void ) {
	HKEY h ; DWORD val = 0, sz = sizeof(val), type = 0 ; int got = 0 ;
	if( RegOpenKeyEx( HKEY_CURRENT_USER, kitty_registry_base(), 0, KEY_READ, &h ) == ERROR_SUCCESS ) {
		if( RegQueryValueEx( h, "ProxiesMigrated", NULL, &type, (LPBYTE)&val, &sz ) == ERROR_SUCCESS
		    && type == REG_DWORD && val ) got = 1 ;
		RegCloseKey( h ) ;
	}
	return got ;
}

static void kitty_proxy_encrypt_in_place( const char *dstpath, const char *name ) {
	char *raw = GetValueDataDup( HKEY_CURRENT_USER, dstpath, "ProxyPassword" ) ;
	if( raw && raw[0] && !kitty_secret_is_marked( raw ) ) {
		/* strip_plain: a PLAIN:-marked value is cleartext with a marker, so
		 * protect the password and not the marker along with it. A refused
		 * wrap (DPAPI failing) leaves the value as it is, and is reported. */
		int kept = 0 ;
		char *enc = kitty_secret_wrap_current_backend_kept( kitty_secret_strip_plain( raw ), &kept ) ;   /* registry -> DPAPI1 */
		if( enc ) {
			RegTestOrCreate( HKEY_CURRENT_USER, dstpath, "ProxyPassword", enc ) ;
			memset( enc, 0, strlen(enc) ) ; free( enc ) ;
		} else if( kept )
			ksec_report_locked_save( KSEC_KEPT_PROXY, name ) ;
	}
	if( raw ) { smemclr( raw, strlen(raw) ) ; sfree( raw ) ; }
}

void kitty_migrate_old_proxies( void ) {
	if( (IniFileFlag != SAVEMODE_REG) && (IniFileFlag != SAVEMODE_FILE) ) return ;
	if( kitty_store_readonly() ) return ;   /* readonly=yes: a later start migrates */
	if( kitty_proxies_migrated() ) return ;
	HKEY hSrc ;
	if( RegOpenKeyEx( HKEY_CURRENT_USER, "Software\\9bis.com\\KiTTY\\Proxies", 0, KEY_READ, &hSrc ) == ERROR_SUCCESS ) {
		DWORD idx ; FILETIME ft ;
		for( idx = 0 ; ; idx++ ) {
			/* a key name is at most 255 characters, plus its NUL */
			char sub[MAX_KEY_LENGTH + 1] ; DWORD subsz = sizeof(sub) ;
			if( RegEnumKeyEx( hSrc, idx, sub, &subsz, NULL, NULL, NULL, &ft ) != ERROR_SUCCESS ) break ;
			if( !strcmp(sub,"None") || !strcmp(sub,"Default") ) continue ;
			char *srcpath = dupprintf( "Software\\9bis.com\\KiTTY\\Proxies\\%s", sub ) ;
			char *dstpath = dupprintf( "%s\\Proxies\\%s", kitty_registry_base(), sub ) ;
			if( !RegTestKey( HKEY_CURRENT_USER, dstpath ) )
				kitty_RegCopyTree( HKEY_CURRENT_USER, srcpath, dstpath ) ;  /* all fields (pw plaintext) */
			kitty_proxy_encrypt_in_place( dstpath, sub ) ;                  /* protect if still plaintext */
			sfree( srcpath ) ;
			sfree( dstpath ) ;
		}
		RegCloseKey( hSrc ) ;
	}
	RegTestOrCreateDWORD( HKEY_CURRENT_USER, kitty_registry_base(), "ProxiesMigrated", 1 ) ;
}

/* ---- Piece 7: carry named proxies in the whole-store export/import bundle ----
 * Definitions are written under <dir>\Proxies\<munged> as portable "key\value\"
 * files with the password wrapped MPW2 (self-contained salt, machine-independent
 * - the same policy the session bundle uses). Import re-wraps per DESTINATION
 * backend via SaveProxyInfo (registry -> DPAPI1, portable -> MPW2), overwriting
 * by name to match session import (hknet/KiTTY#11). */

int kitty_export_proxies_to_dir( const char *dir ) {
	char *pdir ; int count = 0 ;
	InitProxyList() ;
	if( !proxies[2].name ) return 0 ;   /* no named proxies -> don't create an empty Proxies\ folder */
	pdir = dupprintf( "%s\\Proxies", dir ) ;
	MakeDir( pdir ) ;
	for( int i = 2 ; i < MAX_PROXY && proxies[i].name ; i++ ) {
		Conf *conf = conf_new() ; do_defaults( NULL, conf ) ;
		LoadProxyInfo( conf, proxies[i].name ) ;         /* decrypts pw into CONF_proxy_password */
		char *fn = mungestr_dup( proxies[i].name ) ;
		char *path = dupprintf( "%s\\%s", pdir, fn ) ; sfree( fn ) ;
		char pxpw[KITTY_PW_MAX + 1] ;   /* the Conf holds it wrapped for memory */
		kitty_pw_get( conf, CONF_proxy_password, pxpw, sizeof(pxpw) ) ;
		/* MPW2 (or DPAPI1 for "this PC only"); a failed wrap writes no
		 * password line, the summary lists it - never DPAPI in its place;
		 * outside a bundle a refused wrap (master password locked, DPAPI
		 * failing) writes none either, and is reported */
		int pwlocked = 0, pwskipped = 0 ;
		char *pwblob = kitty_secret_wrap_export( pxpw, KSEC_EXPORT_PROXY_PW, proxies[i].name, &pwlocked, &pwskipped ) ;
		smemclr( pxpw, sizeof(pxpw) ) ;
		FILE *fp = fopen( path, "w" ) ;
		if( fp ) {
			int dns = (conf_get_int(conf, CONF_proxy_dns) + 2) % 3 ;
			int local = conf_get_bool(conf, CONF_even_proxy_localhost) ? 1 : 0 ;
			/* each value escaped in full (up to three bytes per character) */
			#define WPS(K,V) do { char *mv = mungestr_dup( (V) ) ; fprintf(fp, "%s\\%s\\\n", (K), mv) ; smemclr( mv, strlen(mv) ) ; sfree( mv ) ; } while(0)
			#define WPD(K,D) fprintf(fp, "%s\\%d\\\n", (K), (D))
			WPS( "ProxyExcludeList", conf_get_str(conf, CONF_proxy_exclude_list) ) ;
			WPD( "ProxyDNS", dns ) ;
			WPD( "ProxyLocalhost", local ) ;
			WPD( "ProxyMethod", proxy_method_from_conf( conf ) ) ;
			WPS( "ProxyHost", conf_get_str(conf, CONF_proxy_host) ) ;
			WPD( "ProxyPort", conf_get_int(conf, CONF_proxy_port) ) ;
			WPS( "ProxyUsername", conf_get_str(conf, CONF_proxy_username) ) ;
			if( !pwskipped && !pwlocked )
				WPS( "ProxyPassword", pwblob ? pwblob : "" ) ;
			WPS( "ProxyTelnetCommand", conf_get_str(conf, CONF_proxy_telnet_command) ) ;
			WPD( "ProxyLogToTerm", conf_get_int(conf, CONF_proxy_log_to_term) ) ;
			if( conf_get_int(conf, CONF_proxy_host_kind) >= 0 )
				WPD( "ProxyHostIs", conf_get_int(conf, CONF_proxy_host_kind) ) ;
			#undef WPS
			#undef WPD
			fclose( fp ) ; count++ ;
		}
		if( pwblob ) { memset(pwblob,0,strlen(pwblob)) ; free(pwblob) ; }
		if( pwlocked ) ksec_report_locked_save( KSEC_KEPT_EXPORT, path ) ;
		sfree( path ) ;
		conf_free( conf ) ;
	}
	sfree( pdir ) ;
	return count ;
}

/* Is a named proxy of this name already defined in the active store? */
int kitty_proxy_name_exists( const char *name ) {
	InitProxyList() ;
	for( int i = 2 ; i < MAX_PROXY && proxies[i].name ; i++ )
		if( !strcmp( proxies[i].name, name ) ) return 1 ;
	return 0 ;
}

/* How many proxy definitions in dir\Proxies already exist in the active store
 * (used to warn about overwrites before an import). */
int kitty_proxies_dir_collisions( const char *dir ) {
	char *pat ; int c = 0 ;
	pat = dupprintf( "%s\\Proxies\\*", dir ) ;
	WIN32_FIND_DATAA fd ; HANDLE h = FindFirstFileA( pat, &fd ) ;
	sfree( pat ) ;
	if( h == INVALID_HANDLE_VALUE ) return 0 ;
	do {
		if( fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) continue ;
		char *name = unmungestr_dup( fd.cFileName ) ;
		if( name[0] && kitty_proxy_name_exists( name ) ) c++ ;
		sfree( name ) ;
	} while( FindNextFileA( h, &fd ) ) ;
	FindClose( h ) ;
	return c ;
}

/* overwrite==0: keep existing proxies of the same name, import only new ones.
 * Returns the number imported; *skippedOut (optional) gets the number kept. */
int kitty_import_proxies_from_dir( const char *dir, int overwrite, int *skippedOut ) {
	char *pdir, *pat ; int count = 0, skipped = 0 ;
	pdir = dupprintf( "%s\\Proxies", dir ) ;
	pat = dupprintf( "%s\\*", pdir ) ;
	WIN32_FIND_DATAA fd ; HANDLE h = FindFirstFileA( pat, &fd ) ;
	sfree( pat ) ;
	if( h == INVALID_HANDLE_VALUE ) { sfree( pdir ) ; return 0 ; }
	do {
		if( fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) continue ;
		char *path = dupprintf( "%s\\%s", pdir, fd.cFileName ) ;
		FILE *fp = fopen( path, "r" ) ;
		sfree( path ) ;
		if( !fp ) continue ;
		Conf *conf = conf_new() ; do_defaults( NULL, conf ) ;
		char *buffer ;
		/* each line in full; its value is never longer than the line */
		while( (buffer = kitty_fgetline( fp )) != NULL ) {
			int maxlen = (int)strlen( buffer ) + 1 ;
			char *buf2 = snewn( maxlen, char ) ;
			buf2[0] = '\0' ;
			if( ReadPortableValue(buffer, "ProxyExcludeList", buf2, maxlen) ) {
				conf_set_str( conf, CONF_proxy_exclude_list, buf2 ) ;
			} else if( ReadPortableValue(buffer, "ProxyDNS", buf2, maxlen) ) {
				conf_set_int( conf, CONF_proxy_dns, (atoi(buf2)+1)%3 ) ;
			} else if( ReadPortableValue(buffer, "ProxyLocalhost", buf2, maxlen) ) {
				conf_set_bool( conf, CONF_even_proxy_localhost, atoi(buf2)!=0 ) ;
			} else if( ReadPortableValue(buffer, "ProxyMethod", buf2, maxlen) ) {
				int i = atoi(buf2) ;
				if (i == 1) conf_set_int(conf, CONF_proxy_type, PROXY_SOCKS4) ;
				else if (i == 2) conf_set_int(conf, CONF_proxy_type, PROXY_SOCKS5) ;
				else if (i == 3) conf_set_int(conf, CONF_proxy_type, PROXY_HTTP) ;
				else if (i == 4) conf_set_int(conf, CONF_proxy_type, PROXY_TELNET) ;
				else if (i == 5) conf_set_int(conf, CONF_proxy_type, PROXY_CMD) ;
				else if (i == 6) conf_set_int(conf, CONF_proxy_type, PROXY_SSH_TCPIP) ;
				else if (i == 7) conf_set_int(conf, CONF_proxy_type, PROXY_SSH_EXEC) ;
				else if (i == 8) conf_set_int(conf, CONF_proxy_type, PROXY_SSH_SUBSYSTEM) ;
				else conf_set_int(conf, CONF_proxy_type, PROXY_NONE) ;
			} else if( ReadPortableValue(buffer, "ProxyHost", buf2, maxlen) ) {
				conf_set_str( conf, CONF_proxy_host, buf2 ) ;
			} else if( ReadPortableValue(buffer, "ProxyPort", buf2, maxlen) ) {
				conf_set_int( conf, CONF_proxy_port, atoi(buf2) ) ;
			} else if( ReadPortableValue(buffer, "ProxyUsername", buf2, maxlen) ) {
				conf_set_str( conf, CONF_proxy_username, buf2 ) ;
			} else if( ReadPortableValue(buffer, "ProxyPassword", buf2, maxlen) ) {
				char *pt = NULL ; kitty_secret_unwrap( buf2, &pt ) ;   /* MPW2/legacy -> plain */
				/* Wrapped again for memory (kitty_pwmem.c). */
				kitty_pw_set( conf, CONF_proxy_password, pt ? pt : "" ) ;
				if( pt ) { memset( pt, 0, strlen(pt) ) ; free( pt ) ; }
			} else if( ReadPortableValue(buffer, "ProxyTelnetCommand", buf2, maxlen) ) {
				conf_set_str( conf, CONF_proxy_telnet_command, buf2 ) ;
			} else if( ReadPortableValue(buffer, "ProxyLogToTerm", buf2, maxlen) ) {
				conf_set_int( conf, CONF_proxy_log_to_term, atoi(buf2) ) ;
			} else if( ReadPortableValue(buffer, "ProxyHostIs", buf2, maxlen) ) {
				conf_set_int( conf, CONF_proxy_host_kind, atoi(buf2) ? 1 : 0 ) ;
			}
			smemclr( buf2, maxlen ) ;
			sfree( buf2 ) ;
			memset( buffer, 0, strlen(buffer) ) ;
			free( buffer ) ;
		}
		fclose( fp ) ;
		char *name = unmungestr_dup( fd.cFileName ) ;
		if( name[0] ) {
			if( overwrite || !kitty_proxy_name_exists(name) ) {
				if( SaveProxyInfo( conf, name ) ) count++ ;   /* re-wraps per dest backend */
			} else skipped++ ;                            /* keep the existing proxy */
		}
		sfree( name ) ;
		conf_free( conf ) ;
	} while( FindNextFileA( h, &fd ) ) ;
	FindClose( h ) ;
	sfree( pdir ) ;
	if( skippedOut ) *skippedOut = skipped ;
	return count ;
}

