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


/* Proxy-choice selector visibility (kitty.ini [ConfigBox] proxyselection):
 *   1  = yes   -> always shown
 *   0  = auto  -> shown when the user has proxy definitions (default)
 *  -1  = no    -> never shown
 * (hknet/KiTTY#11: old KiTTY only had off/on via "yes"; auto makes it
 * discoverable, "no" lets a user with proxies defined still hide it.) */
static int ProxySelectionFlag = 0 ;
int GetProxySelectionFlag() { return ProxySelectionFlag ; }
void SetProxySelectionFlag( const int flag ) { ProxySelectionFlag = flag ; }

void debug_logevent( const char *fmt, ... ) ;

/* At-rest password protection shared with sessions (windows/storage.c): wrap for
 * the active backend (registry DPAPI / portable MPW / explicit legacy), unwrap by
 * stored marker. So a named proxy's password is protected exactly like a session
 * password (hknet/KiTTY#11, TASK_named_proxies.md Phase B). */
extern char *kitty_secret_wrap_current_backend( const char *plaintext ) ;
extern int   kitty_secret_unwrap( const char *stored, char **out ) ;
extern int   kitty_secret_is_marked( const char *stored ) ;
extern int   kitty_portable_password_legacy( void ) ;
extern const char *kitty_secret_strip_plain( const char *stored ) ;

struct Proxies proxies[MAX_PROXY] ;

/* True once InitProxyList() has found at least one user-defined proxy (beyond
 * the two built-in "- Session defined proxy -" / "- No proxy -" entries at
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
 * that no longer exists (or "- Session defined proxy -" / empty) collapses to
 * "- Session defined proxy -" when the session has its OWN proxy configured,
 * else "- No proxy -". "- No proxy -" and an existing named proxy are kept.
 * Needs proxies[] populated (InitProxyList, done at startup). */
void kitty_proxy_resolve_selection( Conf *conf ) {
	const char *cur = conf_get_str( conf, CONF_proxyselection ) ;
	if( cur && !strcmp( cur, "- No proxy -" ) ) return ;
	if( cur && cur[0] && strcmp( cur, "- Session defined proxy -" ) ) {
		for( int i = 2 ; i < MAX_PROXY && proxies[i].name ; i++ )
			if( !strcmp( cur, proxies[i].name ) ) return ;   /* still exists */
	}
	conf_set_str( conf, CONF_proxyselection,
		conf_get_int( conf, CONF_proxy_type ) != PROXY_NONE
			? "- Session defined proxy -" : "- No proxy -" ) ;
}

void InitProxyList(void) {
	HKEY hKey ;
	int i,j;
	char buffer[MAX_VALUE_NAME] ;
	for( i=0; i<lenof(proxies); i++) {
		if( proxies[i].name != NULL ) { free( proxies[i].name ) ; }   /* re-callable, no leak */
		proxies[i].name = NULL;
		proxies[i].val = i;
	}
	proxies[0].name=(char*)malloc(26); strcpy(proxies[0].name,"- Session defined proxy -");
	proxies[1].name=(char*)malloc(13); strcpy(proxies[1].name,"- No proxy -");
	j=2;
	if( (IniFileFlag == SAVEMODE_REG)||(IniFileFlag == SAVEMODE_FILE) ) {
		TCHAR 	achClass[MAX_PATH] = TEXT("");
		DWORD   cchClassName=MAX_PATH,cSubKeys=0,cbMaxSubKey,cchMaxClass;
		DWORD	cValues,cchMaxValue,cbMaxValueData,cbSecurityDescriptor;
		FILETIME ftLastWriteTime;
		snprintf( buffer, sizeof(buffer), "%s\\Proxies", PUTTY_REG_POS ) ;
		RegTestOrCreate( HKEY_CURRENT_USER, buffer, NULL, NULL ) ;
		if( RegOpenKeyEx( HKEY_CURRENT_USER, buffer, 0, KEY_READ, &hKey) != ERROR_SUCCESS ) return ;
		RegQueryInfoKey(hKey,achClass,&cchClassName,NULL,&cSubKeys,&cbMaxSubKey,&cchMaxClass,&cValues,&cchMaxValue,&cbMaxValueData,&cbSecurityDescriptor,&ftLastWriteTime);
		if( cSubKeys>0 )
			for (i=0; i<cSubKeys; i++) {
				DWORD cchValue = MAX_VALUE_NAME;
				char lpData[4096] ;
				if( j>=MAX_PROXY ) break; /* SECURITY: bound proxies[] */
				if( RegEnumKeyEx(hKey, i, lpData, &cchValue, NULL, NULL, NULL, &ftLastWriteTime) == ERROR_SUCCESS ) {
					if( strcmp(lpData,"None") && strcmp(lpData,"Default") ) {
						proxies[j].name=(char*)malloc(strlen(lpData)+1);
						unmungestr( lpData, proxies[j].name, MAX_VALUE_NAME ) ;
						j++;
					}
				}
			}
		RegCloseKey( hKey ) ;
	} else if( IniFileFlag == SAVEMODE_DIR ) {
		char fullpath[MAX_VALUE_NAME];
		DIR * dir ;
		struct dirent * de ;
		snprintf( fullpath, sizeof(fullpath), "%s\\Proxies", ConfigDirectory ) ;
		if(!MakeDir( fullpath ) ) { MessageBox(NULL,"Unable to create the proxy definitions directory","Error",MB_OK|MB_ICONERROR); }
		if( (dir=opendir(fullpath)) != NULL ) {
			while( (de=readdir(dir)) != NULL )
			if( j>=MAX_PROXY ) break; /* SECURITY: bound proxies[] */
			else if( strcmp(de->d_name,".") && strcmp(de->d_name,"..") )	{
				snprintf( fullpath, sizeof(fullpath), "%s\\Proxies\\%s", ConfigDirectory, de->d_name ) ;
				if( !(GetFileAttributes( fullpath ) & FILE_ATTRIBUTE_NORMAL) ) {
					if( strcmp(de->d_name,"None") && strcmp(de->d_name,"Default") ) {
						proxies[j].name=(char*)malloc(strlen(de->d_name)+1);
						unmungestr( de->d_name, proxies[j].name, MAX_VALUE_NAME ) ;
						j++;
					}
				}
			}
			closedir(dir) ;
		}
	}
}

int LoadProxyInfo( Conf * conf, const char * name ) {
	char buffer[MAX_VALUE_NAME] ;
	if( !strcmp(name,"- Session defined proxy -") ) { return 0 ; }
	if( !strcmp(name,"- No proxy -") ) { 
		debug_logevent( "Remove proxy definition" ) ;
		conf_set_int(conf, CONF_proxy_type, PROXY_NONE) ; 
		return 1 ;
	}
	debug_logevent( "Load proxy \"%s\" definition", name ) ;
	if( (IniFileFlag == SAVEMODE_REG)||(IniFileFlag == SAVEMODE_FILE) ) {
		HKEY hKey ;
		snprintf( buffer, sizeof(buffer), "%s\\Proxies\\", PUTTY_REG_POS ) ;
		char *b = (char*)malloc(4*strlen(name)+1);
		mungestr(name,b);
		{ size_t _bl=strlen(buffer); snprintf( buffer+_bl, sizeof(buffer)-_bl, "%s", b ) ; }
		free(b);
		if( RegOpenKeyEx( HKEY_CURRENT_USER, buffer, 0, KEY_READ, &hKey) != ERROR_SUCCESS ) {
			debug_logevent( "Unable to load proxy definition" ) ;
			return 0;
		}
		char lpData[4096] ;
		if( GetValueDataN(HKEY_CURRENT_USER, buffer, "ProxyExcludeList", lpData, sizeof(lpData) ) ) { 
			conf_set_str( conf, CONF_proxy_exclude_list, lpData ) ; 
		}
		if( GetValueDataN(HKEY_CURRENT_USER, buffer, "ProxyDNS", lpData, sizeof(lpData) ) ) {
			int i=atoi(lpData);
			conf_set_int(conf, CONF_proxy_dns, (i+1)%3);
		}
		if( GetValueDataN(HKEY_CURRENT_USER, buffer, "ProxyLocalhost", lpData, sizeof(lpData) ) ) { 
			if( atoi(lpData) == 0 ) { conf_set_bool( conf, CONF_even_proxy_localhost, false ) ; 
			} else { conf_set_bool( conf, CONF_even_proxy_localhost, true ) ;
			}			
		}
		if( GetValueDataN(HKEY_CURRENT_USER, buffer, "ProxyMethod", lpData, sizeof(lpData) ) ) {
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
		}
		if( GetValueDataN(HKEY_CURRENT_USER, buffer, "ProxyHost", lpData, sizeof(lpData) ) ) { conf_set_str( conf, CONF_proxy_host, lpData ) ; }
		if( GetValueDataN(HKEY_CURRENT_USER, buffer, "ProxyPort", lpData, sizeof(lpData) ) ) { conf_set_int( conf, CONF_proxy_port, atoi(lpData) ) ; }
		if( GetValueDataN(HKEY_CURRENT_USER, buffer, "ProxyUsername", lpData, sizeof(lpData) ) ) { conf_set_str( conf, CONF_proxy_username, lpData ) ; }
		if( GetValueDataN(HKEY_CURRENT_USER, buffer, "ProxyPassword", lpData, sizeof(lpData) ) ) {
			char *pt = NULL ; kitty_secret_unwrap( lpData, &pt ) ;   /* DPAPI/MPW/plain */
			conf_set_str( conf, CONF_proxy_password, pt ? pt : "" ) ;
			if( pt ) { memset( pt, 0, strlen(pt) ) ; free( pt ) ; }
		}
		if( GetValueDataN(HKEY_CURRENT_USER, buffer, "ProxyTelnetCommand", lpData, sizeof(lpData) ) ) { conf_set_str( conf, CONF_proxy_telnet_command, lpData ) ; }
		if( GetValueDataN(HKEY_CURRENT_USER, buffer, "ProxyLogToTerm", lpData, sizeof(lpData) ) ) { conf_set_int( conf, CONF_proxy_log_to_term, atoi(lpData) ) ; }
		RegCloseKey( hKey ) ;
	} else if( IniFileFlag == SAVEMODE_DIR ) {
		char fullpath[MAX_VALUE_NAME] ;
		char *filename = (char*) malloc( 4*strlen(name)+1 ) ;
		mungestr( name, filename ) ;
		snprintf( fullpath, sizeof(fullpath), "%s\\Proxies\\%s", ConfigDirectory, filename ) ;
		if( existfile(fullpath) ) {
			FILE *fp;
			if( (fp=fopen(fullpath,"r")) != NULL ) {
				char buf2[MAX_VALUE_NAME]="";
				while( fgets(buffer,MAX_VALUE_NAME,fp)!=NULL ) {
					if( ReadPortableValue(buffer, "ProxyExcludeList", buf2, MAX_VALUE_NAME) ) {
						conf_set_str( conf, CONF_proxy_exclude_list, buf2 ) ; 
					} else if( ReadPortableValue(buffer, "ProxyDNS", buf2, MAX_VALUE_NAME) ) {
						int i=atoi(buf2);
						conf_set_int(conf, CONF_proxy_dns, (i+1)%3);
					} else if( ReadPortableValue(buffer, "ProxyLocalhost", buf2, MAX_VALUE_NAME) ) {
						if( atoi(buf2) == 0 ) { conf_set_bool( conf, CONF_even_proxy_localhost, false ) ; 
						} else { conf_set_bool( conf, CONF_even_proxy_localhost, true ) ;
						}	
					} else if( ReadPortableValue(buffer, "ProxyMethod", buf2, MAX_VALUE_NAME) ) {
						int i = atoi(buf2) ;
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
					} else if( ReadPortableValue(buffer, "ProxyHost", buf2, MAX_VALUE_NAME) ) { 
						conf_set_str( conf, CONF_proxy_host, buf2 ) ; 
					} else if( ReadPortableValue(buffer, "ProxyPort", buf2, MAX_VALUE_NAME) ) { 
						conf_set_int( conf, CONF_proxy_port, atoi(buf2) ) ; 
					} else if( ReadPortableValue(buffer, "ProxyUsername", buf2, MAX_VALUE_NAME) ) { 
						conf_set_str( conf, CONF_proxy_username, buf2 ) ; 
					} else if( ReadPortableValue(buffer, "ProxyPassword", buf2, MAX_VALUE_NAME) ) {
						char *pt = NULL ; kitty_secret_unwrap( buf2, &pt ) ;   /* DPAPI/MPW/plain */
						conf_set_str( conf, CONF_proxy_password, pt ? pt : "" ) ;
						if( pt ) { memset( pt, 0, strlen(pt) ) ; free( pt ) ; }
					} else if( ReadPortableValue(buffer, "ProxyTelnetCommand", buf2, MAX_VALUE_NAME) ) {
						conf_set_str( conf, CONF_proxy_telnet_command, buf2 ) ; 
					} else if( ReadPortableValue(buffer, "ProxyLogToTerm", buf2, MAX_VALUE_NAME) ) { 
						conf_set_int( conf, CONF_proxy_log_to_term, atoi(buf2) ) ; 
					}
				}
				fclose(fp);
			}
		}
		free( filename ) ;
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
 * name must be a real definition (not a built-in). Registry -> PUTTY_REG_POS\
 * Proxies\<name>; portable -> ConfigDirectory\Proxies\<name>, one "Key\value\"
 * line per field (the format ReadPortableValue expects). ProxyPassword is
 * protected at rest via the shared backend policy (DPAPI registry / MPW
 * portable / explicit legacy), the same chokepoint as session passwords. */
void kitty_store_mark_dirty(void) ;   /* kitty_storage.c */
int SaveProxyInfo( Conf *conf, const char *name ) {
	kitty_store_mark_dirty() ;   /* named proxies live in the store too */
	if( name == NULL || name[0] == '\0' ) return 0 ;
	if( !strcmp(name,"- Session defined proxy -") || !strcmp(name,"- No proxy -") ) return 0 ;
	int method = proxy_method_from_conf( conf ) ;
	int dns    = (conf_get_int(conf, CONF_proxy_dns) + 2) % 3 ;   /* mirror LoadProxyInfo */
	int local  = conf_get_bool(conf, CONF_even_proxy_localhost) ? 1 : 0 ;
	/* Protect the password at rest via the shared backend policy (may prompt to
	 * create/unlock the master password in portable mode; empty -> "", no prompt). */
	char *pwblob = kitty_secret_wrap_current_backend( conf_get_str(conf, CONF_proxy_password) ) ;
	if( (IniFileFlag == SAVEMODE_REG) || (IniFileFlag == SAVEMODE_FILE) ) {
		char sub[2048] ;
		char *m = (char*)malloc(4*strlen(name)+1) ; mungestr( name, m ) ;
		snprintf( sub, sizeof(sub), "%s\\Proxies\\%s", PUTTY_REG_POS, m ) ;
		free( m ) ;
		RegTestOrCreate( HKEY_CURRENT_USER, sub, NULL, NULL ) ;
		RegTestOrCreate( HKEY_CURRENT_USER, sub, "ProxyExcludeList", conf_get_str(conf, CONF_proxy_exclude_list) ) ;
		RegTestOrCreateDWORD( HKEY_CURRENT_USER, sub, "ProxyDNS", dns ) ;
		RegTestOrCreateDWORD( HKEY_CURRENT_USER, sub, "ProxyLocalhost", local ) ;
		RegTestOrCreateDWORD( HKEY_CURRENT_USER, sub, "ProxyMethod", method ) ;
		RegTestOrCreate( HKEY_CURRENT_USER, sub, "ProxyHost", conf_get_str(conf, CONF_proxy_host) ) ;
		RegTestOrCreateDWORD( HKEY_CURRENT_USER, sub, "ProxyPort", conf_get_int(conf, CONF_proxy_port) ) ;
		RegTestOrCreate( HKEY_CURRENT_USER, sub, "ProxyUsername", conf_get_str(conf, CONF_proxy_username) ) ;
		RegTestOrCreate( HKEY_CURRENT_USER, sub, "ProxyPassword", pwblob ? pwblob : "" ) ;
		RegTestOrCreate( HKEY_CURRENT_USER, sub, "ProxyTelnetCommand", conf_get_str(conf, CONF_proxy_telnet_command) ) ;
		RegTestOrCreateDWORD( HKEY_CURRENT_USER, sub, "ProxyLogToTerm", conf_get_int(conf, CONF_proxy_log_to_term) ) ;
	} else if( IniFileFlag == SAVEMODE_DIR ) {
		char dir[2048], fullpath[2048], mv[4096] ;
		char *fn = (char*)malloc(4*strlen(name)+1) ; mungestr( name, fn ) ;
		snprintf( dir, sizeof(dir), "%s\\Proxies", ConfigDirectory ) ;
		MakeDir( dir ) ;
		snprintf( fullpath, sizeof(fullpath), "%s\\%s", dir, fn ) ;
		free( fn ) ;
		FILE *fp = fopen( fullpath, "w" ) ;
		if( fp ) {
			#define WPS(K,V) do { mungestr((V), mv) ; fprintf(fp, "%s\\%s\\\n", (K), mv) ; } while(0)
			#define WPD(K,D) fprintf(fp, "%s\\%d\\\n", (K), (D))
			WPS( "ProxyExcludeList", conf_get_str(conf, CONF_proxy_exclude_list) ) ;
			WPD( "ProxyDNS", dns ) ;
			WPD( "ProxyLocalhost", local ) ;
			WPD( "ProxyMethod", method ) ;
			WPS( "ProxyHost", conf_get_str(conf, CONF_proxy_host) ) ;
			WPD( "ProxyPort", conf_get_int(conf, CONF_proxy_port) ) ;
			WPS( "ProxyUsername", conf_get_str(conf, CONF_proxy_username) ) ;
			WPS( "ProxyPassword", pwblob ? pwblob : "" ) ;
			WPS( "ProxyTelnetCommand", conf_get_str(conf, CONF_proxy_telnet_command) ) ;
			WPD( "ProxyLogToTerm", conf_get_int(conf, CONF_proxy_log_to_term) ) ;
			#undef WPS
			#undef WPD
			fclose( fp ) ;
		}
	}
	if( pwblob ) { memset( pwblob, 0, strlen(pwblob) ) ; free( pwblob ) ; }
	return 1 ;
}

/* Delete a named proxy definition (registry key or portable file). */
int DeleteProxyInfo( const char *name ) {
	if( name == NULL || name[0] == '\0' ) return 0 ;
	if( !strcmp(name,"- Session defined proxy -") || !strcmp(name,"- No proxy -") ) return 0 ;
	if( (IniFileFlag == SAVEMODE_REG) || (IniFileFlag == SAVEMODE_FILE) ) {
		char sub[2048] ;
		char *m = (char*)malloc(4*strlen(name)+1) ; mungestr( name, m ) ;
		snprintf( sub, sizeof(sub), "%s\\Proxies\\%s", PUTTY_REG_POS, m ) ;
		free( m ) ;
		RegDeleteKey( HKEY_CURRENT_USER, sub ) ;
	} else if( IniFileFlag == SAVEMODE_DIR ) {
		char fullpath[2048] ;
		char *fn = (char*)malloc(4*strlen(name)+1) ; mungestr( name, fn ) ;
		snprintf( fullpath, sizeof(fullpath), "%s\\Proxies\\%s", ConfigDirectory, fn ) ;
		free( fn ) ;
		remove( fullpath ) ;
	}
	return 1 ;
}

/* One-time, marker-gated migration of named proxies from the legacy 9bis hive
 * (Software\9bis.com\KiTTY\Proxies) into ours (PUTTY_REG_POS\Proxies). Runs at
 * startup in registry mode only (portable reads its Proxies\ folder in place);
 * INDEPENDENT of the session-hive migration so it also fires for users who
 * migrated sessions in an earlier build. Registry-only, so at-rest protection
 * is DPAPI (transparent, account-bound, losslessly reversible — no prompt):
 *   - a proxy absent on our side is copied over whole, then its ProxyPassword
 *     is DPAPI-encrypted;
 *   - a proxy already present keeps its (possibly user-edited) fields, but a
 *     still-plaintext ProxyPassword is DPAPI-encrypted in place — this also
 *     auto-protects proxies the whole-tree MigrateOldKittyHive copied earlier.
 * A marked (already-encrypted) password is never re-wrapped. The one-shot marker
 * means a proxy the user later deletes never resurrects (hknet/KiTTY#11,
 * TASK_named_proxies.md Piece 5). */
static int kitty_proxies_migrated( void ) {
	HKEY h ; DWORD val = 0, sz = sizeof(val), type = 0 ; int got = 0 ;
	if( RegOpenKeyEx( HKEY_CURRENT_USER, PUTTY_REG_POS, 0, KEY_READ, &h ) == ERROR_SUCCESS ) {
		if( RegQueryValueEx( h, "ProxiesMigrated", NULL, &type, (LPBYTE)&val, &sz ) == ERROR_SUCCESS
		    && type == REG_DWORD && val ) got = 1 ;
		RegCloseKey( h ) ;
	}
	return got ;
}

static void kitty_proxy_encrypt_in_place( const char *dstpath ) {
	char raw[4096] = "" ;
	if( GetValueData( HKEY_CURRENT_USER, (char*)dstpath, "ProxyPassword", raw )
	    && raw[0] && !kitty_secret_is_marked( raw ) ) {
		/* strip_plain: a PLAIN:-marked value is cleartext with a marker, so
		 * protect the password and not the marker along with it. */
		char *enc = kitty_secret_wrap_current_backend( kitty_secret_strip_plain( raw ) ) ;   /* registry -> DPAPI1 */
		if( enc ) {
			RegTestOrCreate( HKEY_CURRENT_USER, dstpath, "ProxyPassword", enc ) ;
			memset( enc, 0, strlen(enc) ) ; free( enc ) ;
		}
	}
	memset( raw, 0, sizeof(raw) ) ;
}

void kitty_migrate_old_proxies( void ) {
	if( (IniFileFlag != SAVEMODE_REG) && (IniFileFlag != SAVEMODE_FILE) ) return ;
	if( kitty_proxies_migrated() ) return ;
	HKEY hSrc ;
	if( RegOpenKeyEx( HKEY_CURRENT_USER, "Software\\9bis.com\\KiTTY\\Proxies", 0, KEY_READ, &hSrc ) == ERROR_SUCCESS ) {
		DWORD idx ; FILETIME ft ;
		for( idx = 0 ; ; idx++ ) {
			char sub[MAX_KEY_LENGTH] ; DWORD subsz = sizeof(sub) ;
			if( RegEnumKeyEx( hSrc, idx, sub, &subsz, NULL, NULL, NULL, &ft ) != ERROR_SUCCESS ) break ;
			if( !strcmp(sub,"None") || !strcmp(sub,"Default") ) continue ;
			char srcpath[2048], dstpath[2048] ;
			snprintf( srcpath, sizeof(srcpath), "Software\\9bis.com\\KiTTY\\Proxies\\%s", sub ) ;
			snprintf( dstpath, sizeof(dstpath), "%s\\Proxies\\%s", PUTTY_REG_POS, sub ) ;
			if( !RegTestKey( HKEY_CURRENT_USER, dstpath ) )
				kitty_RegCopyTree( HKEY_CURRENT_USER, srcpath, dstpath ) ;  /* all fields (pw plaintext) */
			kitty_proxy_encrypt_in_place( dstpath ) ;                       /* protect if still plaintext */
		}
		RegCloseKey( hSrc ) ;
	}
	RegTestOrCreateDWORD( HKEY_CURRENT_USER, PUTTY_REG_POS, "ProxiesMigrated", 1 ) ;
}

/* True if any named proxy definition still has a NON-empty, UNENCRYPTED password
 * at rest (raw value present but unmarked). The editor uses this to warn the
 * user to open+save such proxies to protect them. Suppressed in explicit legacy
 * mode, where plaintext is the chosen policy (hknet/KiTTY#11, Phase B). */
int kitty_proxy_any_plaintext_password( void ) {
	if( kitty_portable_password_legacy() ) return 0 ;
	for( int i = 2 ; i < MAX_PROXY && proxies[i].name ; i++ ) {
		char raw[4096] = "" ; int got = 0 ;
		if( (IniFileFlag == SAVEMODE_REG) || (IniFileFlag == SAVEMODE_FILE) ) {
			char sub[2048] ; char *m = (char*)malloc(4*strlen(proxies[i].name)+1) ; mungestr( proxies[i].name, m ) ;
			snprintf( sub, sizeof(sub), "%s\\Proxies\\%s", PUTTY_REG_POS, m ) ; free( m ) ;
			got = ( GetValueDataN( HKEY_CURRENT_USER, sub, "ProxyPassword", raw, sizeof(raw) ) != NULL ) ;
		} else if( IniFileFlag == SAVEMODE_DIR ) {
			char fullpath[2048] ; char *fn = (char*)malloc(4*strlen(proxies[i].name)+1) ; mungestr( proxies[i].name, fn ) ;
			snprintf( fullpath, sizeof(fullpath), "%s\\Proxies\\%s", ConfigDirectory, fn ) ; free( fn ) ;
			FILE *fp = fopen( fullpath, "r" ) ;
			if( fp ) {
				char line[4096], buf2[4096] ;
				/* pass the raw fgets line (newline included) to ReadPortableValue,
				 * which relies on the trailing delimiter+newline (as LoadProxyInfo does). */
				while( fgets( line, sizeof(line), fp ) != NULL ) {
					if( ReadPortableValue( line, "ProxyPassword", buf2, sizeof(buf2) ) ) { snprintf( raw, sizeof(raw), "%s", buf2 ) ; got = 1 ; break ; }
				}
				fclose( fp ) ;
			}
		}
		if( got && raw[0] && !kitty_secret_is_marked( raw ) ) return 1 ;
	}
	return 0 ;
}

/* ---- Piece 7: carry named proxies in the whole-store export/import bundle ----
 * Definitions are written under <dir>\Proxies\<munged> as portable "key\value\"
 * files with the password wrapped MPW2 (self-contained salt, machine-independent
 * — the same policy the session bundle uses). Import re-wraps per DESTINATION
 * backend via SaveProxyInfo (registry -> DPAPI1, portable -> MPW2), overwriting
 * by name to match session import (hknet/KiTTY#11, TASK_named_proxies.md). */
extern char *kitty_secret_wrap_portable( const char *plaintext ) ;

int kitty_export_proxies_to_dir( const char *dir ) {
	char pdir[2048] ; int count = 0 ;
	InitProxyList() ;
	if( !proxies[2].name ) return 0 ;   /* no named proxies -> don't create an empty Proxies\ folder */
	snprintf( pdir, sizeof(pdir), "%s\\Proxies", dir ) ;
	MakeDir( pdir ) ;
	for( int i = 2 ; i < MAX_PROXY && proxies[i].name ; i++ ) {
		Conf *conf = conf_new() ; do_defaults( NULL, conf ) ;
		LoadProxyInfo( conf, proxies[i].name ) ;         /* decrypts pw into CONF_proxy_password */
		char *fn = (char*)malloc(4*strlen(proxies[i].name)+1) ; mungestr( proxies[i].name, fn ) ;
		char path[2048] ; snprintf( path, sizeof(path), "%s\\%s", pdir, fn ) ; free( fn ) ;
		char *pwblob = kitty_secret_wrap_portable( conf_get_str(conf, CONF_proxy_password) ) ;  /* MPW2 */
		FILE *fp = fopen( path, "w" ) ;
		if( fp ) {
			char mv[4096] ;
			int dns = (conf_get_int(conf, CONF_proxy_dns) + 2) % 3 ;
			int local = conf_get_bool(conf, CONF_even_proxy_localhost) ? 1 : 0 ;
			#define WPS(K,V) do { mungestr((V), mv) ; fprintf(fp, "%s\\%s\\\n", (K), mv) ; } while(0)
			#define WPD(K,D) fprintf(fp, "%s\\%d\\\n", (K), (D))
			WPS( "ProxyExcludeList", conf_get_str(conf, CONF_proxy_exclude_list) ) ;
			WPD( "ProxyDNS", dns ) ;
			WPD( "ProxyLocalhost", local ) ;
			WPD( "ProxyMethod", proxy_method_from_conf( conf ) ) ;
			WPS( "ProxyHost", conf_get_str(conf, CONF_proxy_host) ) ;
			WPD( "ProxyPort", conf_get_int(conf, CONF_proxy_port) ) ;
			WPS( "ProxyUsername", conf_get_str(conf, CONF_proxy_username) ) ;
			WPS( "ProxyPassword", pwblob ? pwblob : "" ) ;
			WPS( "ProxyTelnetCommand", conf_get_str(conf, CONF_proxy_telnet_command) ) ;
			WPD( "ProxyLogToTerm", conf_get_int(conf, CONF_proxy_log_to_term) ) ;
			#undef WPS
			#undef WPD
			fclose( fp ) ; count++ ;
		}
		if( pwblob ) { memset(pwblob,0,strlen(pwblob)) ; free(pwblob) ; }
		conf_free( conf ) ;
	}
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
	char pdir[2048], pat[2048] ; int c = 0 ;
	snprintf( pdir, sizeof(pdir), "%s\\Proxies", dir ) ;
	snprintf( pat, sizeof(pat), "%s\\*", pdir ) ;
	WIN32_FIND_DATAA fd ; HANDLE h = FindFirstFileA( pat, &fd ) ;
	if( h == INVALID_HANDLE_VALUE ) return 0 ;
	do {
		if( fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) continue ;
		char *name = (char*)malloc( strlen(fd.cFileName)*4 + 1 ) ;
		unmungestr( fd.cFileName, name, MAX_VALUE_NAME ) ;
		if( name[0] && kitty_proxy_name_exists( name ) ) c++ ;
		free( name ) ;
	} while( FindNextFileA( h, &fd ) ) ;
	FindClose( h ) ;
	return c ;
}

/* overwrite==0: keep existing proxies of the same name, import only new ones.
 * Returns the number imported; *skippedOut (optional) gets the number kept. */
int kitty_import_proxies_from_dir( const char *dir, int overwrite, int *skippedOut ) {
	char pdir[2048], pat[2048] ; int count = 0, skipped = 0 ;
	snprintf( pdir, sizeof(pdir), "%s\\Proxies", dir ) ;
	snprintf( pat, sizeof(pat), "%s\\*", pdir ) ;
	WIN32_FIND_DATAA fd ; HANDLE h = FindFirstFileA( pat, &fd ) ;
	if( h == INVALID_HANDLE_VALUE ) return 0 ;
	do {
		if( fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) continue ;
		char path[2048] ; snprintf( path, sizeof(path), "%s\\%s", pdir, fd.cFileName ) ;
		FILE *fp = fopen( path, "r" ) ;
		if( !fp ) continue ;
		Conf *conf = conf_new() ; do_defaults( NULL, conf ) ;
		char buffer[MAX_VALUE_NAME], buf2[MAX_VALUE_NAME]="" ;
		while( fgets(buffer,MAX_VALUE_NAME,fp)!=NULL ) {
			if( ReadPortableValue(buffer, "ProxyExcludeList", buf2, MAX_VALUE_NAME) ) {
				conf_set_str( conf, CONF_proxy_exclude_list, buf2 ) ;
			} else if( ReadPortableValue(buffer, "ProxyDNS", buf2, MAX_VALUE_NAME) ) {
				conf_set_int( conf, CONF_proxy_dns, (atoi(buf2)+1)%3 ) ;
			} else if( ReadPortableValue(buffer, "ProxyLocalhost", buf2, MAX_VALUE_NAME) ) {
				conf_set_bool( conf, CONF_even_proxy_localhost, atoi(buf2)!=0 ) ;
			} else if( ReadPortableValue(buffer, "ProxyMethod", buf2, MAX_VALUE_NAME) ) {
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
			} else if( ReadPortableValue(buffer, "ProxyHost", buf2, MAX_VALUE_NAME) ) {
				conf_set_str( conf, CONF_proxy_host, buf2 ) ;
			} else if( ReadPortableValue(buffer, "ProxyPort", buf2, MAX_VALUE_NAME) ) {
				conf_set_int( conf, CONF_proxy_port, atoi(buf2) ) ;
			} else if( ReadPortableValue(buffer, "ProxyUsername", buf2, MAX_VALUE_NAME) ) {
				conf_set_str( conf, CONF_proxy_username, buf2 ) ;
			} else if( ReadPortableValue(buffer, "ProxyPassword", buf2, MAX_VALUE_NAME) ) {
				char *pt = NULL ; kitty_secret_unwrap( buf2, &pt ) ;   /* MPW2/legacy -> plain */
				conf_set_str( conf, CONF_proxy_password, pt ? pt : "" ) ;
				if( pt ) { memset( pt, 0, strlen(pt) ) ; free( pt ) ; }
			} else if( ReadPortableValue(buffer, "ProxyTelnetCommand", buf2, MAX_VALUE_NAME) ) {
				conf_set_str( conf, CONF_proxy_telnet_command, buf2 ) ;
			} else if( ReadPortableValue(buffer, "ProxyLogToTerm", buf2, MAX_VALUE_NAME) ) {
				conf_set_int( conf, CONF_proxy_log_to_term, atoi(buf2) ) ;
			}
		}
		fclose( fp ) ;
		char *name = (char*)malloc( strlen(fd.cFileName)*4 + 1 ) ;
		unmungestr( fd.cFileName, name, MAX_VALUE_NAME ) ;
		if( name[0] ) {
			if( overwrite || !kitty_proxy_name_exists(name) ) {
				SaveProxyInfo( conf, name ) ; count++ ;   /* re-wraps per dest backend */
			} else skipped++ ;                            /* keep the existing proxy */
		}
		free( name ) ;
		conf_free( conf ) ;
	} while( FindNextFileA( h, &fd ) ) ;
	FindClose( h ) ;
	if( skippedOut ) *skippedOut = skipped ;
	return count ;
}

