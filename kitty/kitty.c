/*
 * kitty.c - core of the KiTTY fork of PuTTY.
 * Holds the [KiTTY] settings flags with their Get/Set accessors, the
 * parameter layer over kitty.ini and the registry (export, import and
 * timestamped backups), keyboard and automatic-command injection, the
 * broadcast gate that repeats a command to the other windows, window
 * chrome (title, tray, rollup, background images), the port-forward
 * table and the KiTTY startup path. kitty_commands.c and
 * kitty_launcher.c are included here; kitty.h declares the interface.
 */

/*************************************************
** INCLUDES
*************************************************/
// Standard includes
#include <dirent.h>
#include <io.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/locking.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

// PuTTY includes
#include "putty.h"
#include "terminal.h"
#include "putty-rc.h"

// Windows-specific includes (windows.h must come first)
#include <windows.h>
#include "kitty_oldwin.h"   /* record what an older Windows does not have */
#include <psapi.h>
#include <iphlpapi.h>

// KiTTY includes
#include "kitty.h"
#include "kitty_params.h"   /* the application-wide flags, delays, paths and the parameter table */
#include "kitty_broadcast.h"   /* the other KiTTY windows: count, broadcast, resize */
#include "kitty_portfwd.h"     /* the port-forward display */
#include "kitty_regbackup.h"   /* the .sav export/import and the backup rotation */
#include "kitty_int.h"         /* what the split-off files share with this one */
#include "kitty_startup.h"    /* InitWinMain */
#include "kitty_defs.h"     /* KITTY_DEFAULT_SESSION */
#include "kitty_commun.h"
#include "kitty_image.h"
#include "kitty_crypt.h"
#include "kitty_registry.h"
#include "kitty_tools.h"
#include "kitty_win.h"
#include "kitty_updater.h"
#include "kitty_winutil.h"
#include "kitty_dlgbox.h"
#include "kitty_launcher.h"
#include "winfont_fallback.h"
#include "kitty_msgbox.h"   /* themed MessageBox routing */
#include "kitty_oldwin_reg.h"   /* XP: RegDeleteTree/RegGetValue via oldwin */
#include "kitty_text.h"   /* shared captions and wordings */
#include "kitty_inikeys.h"   /* KI_*: the kitty.ini key names */
#include "kitty_notes.h"   /* the application notification, marked owed at startup */
#include "kitty_pwmem.h"   /* passwords wrapped in memory */
#include "kitty_storage.h"
#include "kitty_secretstore.h"
#include "kitty_gui.h"
#include "kitty_bridge.h"
#include "kitty_exportbundle.h"
#include "mini/mini.h"
#include "kitty_b64.h"
#include "kitty_store.h"

/* The hive this process is ACTUALLY using. Not TEXT(PUTTY_REG_POS): that is the
 * compile-time DEFAULT, and with kitty.ini's KiClassName=PuTTY the two differ -
 * see the long note on kitty_registry_base() in kitty/kitty_storage.c. */

/*************************************************
** END OF INCLUDES
*************************************************/


/*************************************************
** CONFIGURATION STRUCTURE
*************************************************/
// The configuration structure is instantiated in window.c

// Flag for "portable" mode (settings kept in files), defined in
// kitty_commun.c
extern int IniFileFlag ;

// Name of the application window class
char KiTTYClassName[128] = "" ;

// Printing parameters

extern char puttystr[1024] ;

#ifdef MOD_PROXY
#include "kitty_proxy.h"
#endif

// Handle to the main window
HWND MainHwnd ;
HWND GetMainHwnd(void) { return MainHwnd ; }

NOTIFYICONDATA TrayIcone ;

#define TIMER_LOGROTATION 8707
#define TIMER_ANTIIDLE 8708

#ifndef BUILD_TIME
#define BUILD_TIME "Undefined"
#endif

#ifndef BUILD_VERSION
#define BUILD_VERSION "0.0"
#endif

#ifndef BUILD_SUBVERSION
#define BUILD_SUBVERSION 0
#endif

char BuildVersionTime[256] = "0.0.0.0 @ 0" ;


// Debug procedure
void debug_log( const char *fmt, ... ) {
	char filename[4096]="" ;
	va_list ap;
	FILE *fp ;

	if( (InitialDirectory!=NULL) && (strlen(InitialDirectory)>0) )
		snprintf( filename, sizeof(filename),"%s\\kitty.log",InitialDirectory);
	else strcpy(filename,"kitty.log");

	va_start( ap, fmt ) ;
	//vfprintf( stdout, fmt, ap ) ; // write to the screen
	if( ( fp = fopen( filename, "ab" ) ) != NULL ) {
		vfprintf( fp, fmt, ap ) ; // write to a file
		fclose( fp ) ;
	}
 
	va_end( ap ) ;
}

char *dupvprintf(const char *fmt, va_list ap) ;
	
// Procedure returning the value of a flag
int get_param( const char * val ) {
	if( !stricmp( val, "PUTTY" ) ) return GetPuttyFlag() ;
	else if( !stricmp( val, "INIFILE" ) ) return IniFileFlag ;
	else if( !stricmp( val, "DIRECTORYBROWSE" ) ) return DirectoryBrowseFlag ;
	else if( !stricmp( val, "HYPERLINK" ) ) return HyperlinkFlag ;
	else if( !stricmp( val, "TRANSPARENCY" ) )return TransparencyFlag ;
#ifdef MOD_ZMODEM
	else if( !stricmp( val, "ZMODEM" ) ) return GetZModemFlag() ;
#endif
#ifdef MOD_BACKGROUNDIMAGE
	else if( !stricmp( val, "BACKGROUNDIMAGE" ) ) return GetBackgroundImageFlag() ;
#endif
	// else if( !stricmp( val, "CONFIGBOXHEIGHT" ) ) return ConfigBoxHeight ;
	// else if( !stricmp( val, "CONFIGBOXWINDOWHEIGHT" ) ) return ConfigBoxWindowHeight ;
	// else if( !stricmp( val, "NUMBEROFICONS" ) ) return NumberOfIcons ;	// ==> replaced by GetNumberOfIcons()
	// else if( !stricmp( val, "ICON" ) ) return IconeFlag ; // ==> replaced by GetIconeFlag()
	// else if( !stricmp( val, "SESSIONFILTER" ) ) return SessionFilterFlag ;
	return 0 ;
	}

// Procedure returning the value of a string
char * get_param_str( const char * val ) {
	if( !stricmp( val, "INI" ) ) return KittyIniFile ;
	else if( !stricmp( val, "SAV" ) ) return KittySavFile ;
	else if( !stricmp( val, "NAME" ) ) return INIT_SECTION ;
	else if( !stricmp( val, "CLASS" ) ) return KiTTYClassName ;
	return NULL ;
	}

#ifdef MOD_ZMODEM
/* 0.84 port: the 0.76b xyz_updateMenuItems() relied on terminal-struct fields
 * (term->xyz_transfering) that don't exist in 0.84. The new no-global ZModem
 * (kitty_zmodem.c) drives menu greying from kitty_zmodem_active() at menu-build
 * time in window.c, so this routine is no longer needed. */
#endif

/* --- OSC 7 remote working-directory tracking (data-plane only) -------------
 * A shell with directory reporting emits ESC ] 7 ; file://host/path BEL on
 * every prompt.  do_osc() (terminal.c) hands us the payload; we validate and
 * store the path so kitty_current_dir() can offer it as the default remote
 * target for drag-drop kscp uploads and StartWinSCP.  Opt-in per session
 * (CONF_osc7_cwd_tracking, default off).  NOTHING is ever executed - this is
 * the safe replacement for the removed __pw/__ws title-scan dispatcher
 * (CVE-2024-23749 RCE), which stays dead. */
static char RemoteCwd[2048] = "" ;     /* validated path portion; "" = none  */
static char RemoteCwdHost[256] = "" ;  /* host portion (future nested-SSH use) */

/* The OSC 7 path validators (osc7_urldecode / osc7_path_ok and their helpers)
 * live in a dependency-free header shared verbatim with the regression test
 * test/test_osc7.c, so this security-critical parsing is unit-tested. */
#include "kitty_osc7_parse.h"

void kitty_set_remote_cwd( const char * osc7 ) {
	if( conf == NULL || !conf_get_bool( conf, CONF_osc7_cwd_tracking ) ) return ;
	if( osc7 == NULL ) return ;
	if( strncmp( osc7, "file://", 7 ) != 0 ) return ;
	const char * host = osc7 + 7 ;
	const char * slash = strchr( host, '/' ) ;   /* first '/' ends the host */
	if( slash == NULL ) return ;
	size_t hostlen = (size_t)( slash - host ) ;
	if( hostlen >= sizeof(RemoteCwdHost) ) return ;
	if( strlen(slash) >= sizeof(RemoteCwd) ) return ;   /* too long: drop, no truncation */
	char path[ sizeof(RemoteCwd) ] ;
	strcpy( path, slash ) ;
	if( !osc7_urldecode( path ) ) return ;    /* embedded NUL -> reject */
	if( !osc7_path_ok( path ) ) return ;
	/* commit only once fully validated */
	memcpy( RemoteCwdHost, host, hostlen ) ; RemoteCwdHost[hostlen] = '\0' ;
	osc7_urldecode( RemoteCwdHost ) ;         /* host unused in phase 1; NUL harmless */
	strcpy( RemoteCwd, path ) ;
}

char * kitty_current_dir(void) {
	/* Respect the CURRENT setting, not just what it was when the cwd was stored:
	 * if the user turns OSC 7 tracking off at runtime (Change Settings), stop
	 * offering the tracked directory immediately so uploads fall back to the
	 * fixed remote dir / home, matching a fresh start with it disabled. */
	if( conf == NULL || !conf_get_bool( conf, CONF_osc7_cwd_tracking ) ) return NULL ;
	if( RemoteCwd[0] == '\0' ) return NULL ;
	return RemoteCwd ;
}

// List of folders
char **FolderList=NULL ;

// Initialise the folder list from the existing sessions and from kitty.ini
void InitFolderList( void ) {
	char * pst, fList[4096], buffer[4096] ;
	int i ;
	FolderList=(char**)malloc( 1024*sizeof(char*) );
	FolderList[0] = NULL ;
	StringList_Add( FolderList, "Default" ) ;
	//if( GetValueData(HKEY_CURRENT_USER, TEXT(PUTTY_REG_POS), "Folders", fList) == NULL ) return ;
	//if( ReadParameter( KI_SECTION_KITTY, KI_FOLDERS, fList ) == 0 ) return ;
	ReadParameter( INIT_SECTION, KI_FOLDERS, fList ) ;
	if( strlen( fList ) != 0 ) {
		pst = fList ;
		while( strlen( pst ) > 0 ) {
			i = 0 ;
			while( ( pst[i] != ',' ) && ( pst[i] != '\0' ) ) {
				buffer[i] = pst[i] ;
				i++ ;
				}
			buffer[i] = '\0' ;
			StringList_Add( FolderList, buffer ) ;
			if( pst[i] == '\0' ) pst = pst + i ;
			else pst = pst + i + 1 ;
			}
		//free( fList ) ; fList = NULL ;
		}
	
	if( (IniFileFlag==SAVEMODE_REG)||(IniFileFlag==SAVEMODE_FILE) ) {
		HKEY hKey ;
		TCHAR    achKey[MAX_KEY_LENGTH];   // buffer for subkey name
		DWORD    cbName;                   // size of name string 
		TCHAR    achClass[MAX_PATH] = TEXT("");  // buffer for class name 
		DWORD    cchClassName = MAX_PATH;  // size of class string 
		DWORD    cSubKeys=0;               // number of subkeys 
		DWORD    cbMaxSubKey;              // longest subkey size 
		DWORD    cchMaxClass;              // longest class string 
		DWORD    cValues;              // number of values for key 
		DWORD    cchMaxValue;          // longest value name 
		DWORD    cbMaxValueData;       // longest value data 
		DWORD    cbSecurityDescriptor; // size of security descriptor 
		FILETIME ftLastWriteTime;      // last write time 
		DWORD retCode; 

		snprintf( buffer, sizeof(buffer), "%s", kitty_reg_sessions() );
		if( RegOpenKeyEx( HKEY_CURRENT_USER, buffer, 0, KEY_READ, &hKey) != ERROR_SUCCESS ) return ;
	
		retCode = RegQueryInfoKey(
		hKey,                    // key handle 
		achClass,                // buffer for class name 
		&cchClassName,           // size of class string 
		NULL,                    // reserved 
		&cSubKeys,               // number of subkeys 
		&cbMaxSubKey,            // longest subkey size 
		&cchMaxClass,            // longest class string 
		&cValues,                // number of values for this key 
		&cchMaxValue,            // longest value name 
		&cbMaxValueData,         // longest value data 
		&cbSecurityDescriptor,   // security descriptor 
		&ftLastWriteTime);       // last write time 
		// Enumerate the subkeys, until RegEnumKeyEx fails.
		if (cSubKeys) {
			for (i=0; i<cSubKeys; i++) { 
				cbName = MAX_KEY_LENGTH;
				retCode = RegEnumKeyEx(hKey, i, achKey, &cbName, NULL, NULL, NULL, &ftLastWriteTime); 
				if (retCode == ERROR_SUCCESS) {
					char nValue[1024] ;
					snprintf( nValue, sizeof(nValue), "%s\\%s", buffer, achKey ) ;
					if( GetValueData(HKEY_CURRENT_USER, nValue, KR_FOLDER, fList ) != NULL ) {
						if( strlen( fList ) > 0 ) 
							StringList_Add( FolderList, fList ) ;
						//free( fList ) ; fList = NULL ;
						}
			
		}
				}
			} 
		RegCloseKey( hKey ) ;
		}
	else if( (IniFileFlag == SAVEMODE_DIR)&&(!DirectoryBrowseFlag) ) {
		DIR * dir ;
		struct dirent * de ;
		snprintf( buffer, sizeof(buffer), "%s\\Sessions", ConfigDirectory ) ;
		if( (dir=opendir(buffer)) != NULL ) {
			while( (de=readdir(dir)) != NULL ) 
			if( strcmp(de->d_name, ".")&&strcmp(de->d_name, "..") ) {
				unmungestr( de->d_name, fList, 1024 ) ;
				GetSessionFolderName( fList, buffer ) ;
				if( strlen(buffer)>0 ) StringList_Add( FolderList, buffer ) ;
				}
			closedir( dir ) ;
			}
		}
	
	if( readINI( KittyIniFile, KI_SECTION_FOLDER, KI_FOLDER_NEW, buffer, sizeof(buffer) ) ) {
		if( strlen( buffer ) > 0 ) {
			for( i=0; i<strlen(buffer); i++ ) if( buffer[i]==',' ) buffer[i]='\0' ;
			StringList_Add( FolderList, buffer ) ;
			}
		delINI( KittyIniFile, KI_SECTION_FOLDER, KI_FOLDER_NEW ) ;
		}
	
	}

static int GetSessionFolderNameInSubDir( const char * session, const char * subdir, char * folder ) {
	int return_code=0;
	char buffer[2048], buf[2048] ;
	DIR * dir ;
	struct dirent * de ;
	if( !strcmp(subdir,"") ) snprintf( buffer, sizeof(buffer), "%s\\Sessions", ConfigDirectory ) ;
	else sprintf(buffer,"%s\\Sessions\\%s",ConfigDirectory, subdir ) ;
	if( (dir=opendir(buffer))!=NULL ) {
		while( (de=readdir(dir)) != NULL ) 
			if( strcmp(de->d_name,".") && strcmp(de->d_name,"..") )	{
				if( !strcmp(subdir,"") ) snprintf( buf, sizeof(buf),"%s\\Sessions\\%s",ConfigDirectory,de->d_name ) ;
				else sprintf(buf,"%s\\Sessions\\%s\\%s",ConfigDirectory, subdir,de->d_name ) ;
				if( existdirectory( buf ) ) {
					if( !strcmp(subdir,"") ) snprintf( buf, sizeof(buf), "%s", de->d_name ) ;
					else sprintf( buf, "%s\\%s", subdir, de->d_name ) ;
					return_code = GetSessionFolderNameInSubDir( session, buf, folder ) ;
					if( return_code ) break ;
				} else if( !strcmp(session,de->d_name) ) {
					strcpy( folder, subdir ) ;
					return_code=1;
					break ;
				}
		}			
		closedir(dir) ;
	}
		
	return return_code ;
}

// Get the name of the folder a session belongs to
void GetSessionFolderName( const char * session_in, char * folder ) {
	HKEY hKey ;
	char buffer[1024], session[1024] ;
	FILE *fp ;
	
	strcpy( folder, "" ) ;
	if( session_in == NULL ) return ;
	if( strlen(session_in)==0 ) return ;
	
	strcpy( buffer, session_in ) ;
	//if( (p = strrchr(buffer, '[')) != NULL ) *(p-1) = '\0' ;

	if( (IniFileFlag==SAVEMODE_REG)||(IniFileFlag==SAVEMODE_FILE) ) {
		mungestr(buffer, session) ;
		snprintf( buffer, sizeof(buffer), "%s\\%s", kitty_reg_sessions(), session ) ;
		if( RegOpenKeyEx( HKEY_CURRENT_USER, buffer, 0, KEY_READ, &hKey) == ERROR_SUCCESS ) {
			DWORD lpType ;
			unsigned char lpData[1024] ;
			DWORD dwDataSize = 1024 ;
			if( RegQueryValueEx( hKey, KR_FOLDER, 0, &lpType, lpData, &dwDataSize ) == ERROR_SUCCESS ) {
				/* SECURITY: RegQueryValueEx may not NUL-terminate; bound + terminate
				 * before strcpy into the caller's char[1024]. */
				if( dwDataSize >= sizeof(lpData) ) dwDataSize = sizeof(lpData)-1 ;
				lpData[dwDataSize] = '\0' ;
				strcpy( folder, (char*)lpData ) ;
			}
			RegCloseKey( hKey ) ;
		}
	} else if( IniFileFlag==SAVEMODE_DIR ) {
		mungestr(session_in, session ) ;
		if( DirectoryBrowseFlag ) {
			GetSessionFolderNameInSubDir( session, "", folder ) ;
		} else {
			snprintf( buffer, sizeof(buffer),"%s\\Sessions\\%s", ConfigDirectory, session );
			if( (fp=fopen(buffer,"r"))!=NULL ) {
				while( fgets(buffer,1024,fp)!=NULL ) {
					str_rtrim( buffer, "\n\r" ) ;
					if( strstr( buffer, "Folder=" ) == buffer ) {
						unmungestr(buffer+7, folder, MAX_PATH) ;
						break ;
					}
					if( strlen(buffer)>0 && buffer[strlen(buffer)-1]=='\\' )
						if( strstr( buffer, KR_FOLDER ) == buffer ) {
							if( buffer[6]=='\\' ) strcpy( folder, buffer+7 ) ;
							{ size_t _fl=strlen(folder); if(_fl>0) folder[_fl-1] = '\0' ; }
							unmungestr(folder, buffer, MAX_PATH) ;
							strcpy( folder, buffer) ;
							break  ;
						}
				}
				fclose(fp);
			}
		}
	}
}

// Get one entry of a session (returns 1 if it exists)
int GetSessionField( const char * session_in, const char * folder_in, const char * field, char * result ) {
	HKEY hKey ;
	char buffer[1024], session[1024], folder[1024], *p ;
	int res = 0 ;
	FILE * fp ;

	if( session_in == NULL ) return 0 ;
	if( strlen(session_in)==0 ) return 0 ;
	
	strcpy( result, "" ) ;
	strcpy( buffer, session_in ) ;
	if( (p = strrchr(buffer, '[')) != NULL ) *(p-1) = '\0' ;
	mungestr(buffer, session) ;
	snprintf( buffer, sizeof(buffer), "%s\\%s", kitty_reg_sessions(), session ) ;
	strcpy( folder, folder_in );
	CleanFolderName( folder );

	if( (IniFileFlag==SAVEMODE_REG)||(IniFileFlag==SAVEMODE_FILE) ) {
		if( RegOpenKeyEx( HKEY_CURRENT_USER, buffer, 0, KEY_READ, &hKey) == ERROR_SUCCESS ) {
			DWORD lpType ;
			unsigned char lpData[1024] ;
			DWORD dwDataSize = 1024 ;
			if( RegQueryValueEx( hKey, field, 0, &lpType, lpData, &dwDataSize ) == ERROR_SUCCESS ) {
				strcpy( result, (char*)lpData ) ;
				res = 1 ;
				}
			RegCloseKey( hKey ) ;
			}
		}
	else if( IniFileFlag==SAVEMODE_DIR ) {
		if( DirectoryBrowseFlag ) {
			if( !strcmp(folder,"Default") || !strcmp(folder,"") ) snprintf( buffer, sizeof(buffer),"%s\\Sessions\\%s", ConfigDirectory, session ) ;
			else sprintf(buffer,"%s\\Sessions\\%s\\%s", ConfigDirectory, folder, session ) ;
			}
		else {
			snprintf( buffer, sizeof(buffer),"%s\\Sessions\\%s", ConfigDirectory, session ) ;
			}

		if( debug_flag ) { debug_logevent( "GetSessionField(%s,%s,%s,%s)=%s", ConfigDirectory, session, folder, field, buffer ) ; }
		if( (fp=fopen(buffer,"r"))!=NULL ) {
			while( fgets(buffer,1024,fp)!=NULL ) {
				str_rtrim( buffer, "\n\r" ) ;
				if( strlen(buffer)>0 && buffer[strlen(buffer)-1]=='\\' )
					if( (strstr( buffer, field )==buffer) && ((buffer+strlen(field))[0]=='\\') ) {
						if( buffer[strlen(field)]=='\\' ) strcpy( result, buffer+strlen(field)+1 ) ;
						{ size_t _rl=strlen(result); if(_rl>0) result[_rl-1] = '\0' ; }
						unmungestr(result, buffer,MAX_PATH) ;
						strcpy( result, buffer) ;
						if( debug_flag ) debug_logevent( "Result=%s", result );
						res = 1 ;
						break ;
						}
				}
			fclose(fp);
			}
		}
	return res ;
	}
	
void RenewPassword( Conf *conf ) {
	return ;
	if( !GetUserPassSSHNoSave() )
	if( kitty_pw_empty(conf, CONF_password) ) {
		char buffer[1024] = "", host[1024], termtype[1024] ;
		if( GetSessionField( conf_get_str(conf,CONF_sessionname), conf_get_str(conf,CONF_folder), KR_PASSWORD, buffer ) ) {
			GetSessionField( conf_get_str(conf,CONF_sessionname), conf_get_str(conf,CONF_folder), "HostName", host );
			GetSessionField( conf_get_str(conf,CONF_sessionname), conf_get_str(conf,CONF_folder), "TerminalType", termtype );
			decryptpassword( GetCryptSaltFlag(), buffer, host, termtype ) ;
			MASKPASS(GetCryptSaltFlag(),buffer);
			kitty_pw_set_burn(conf,CONF_password,buffer) ;
			}
		}
	}


/* Put a password the user typed at the SSH prompt into the running session's
 * settings. It goes in exactly as typed, wrapped in memory like every other
 * password field (kitty_pwmem.c). It used to be MASKPASS-encoded here, which
 * every consumer of the running conf would have read as garbage, and trimmed
 * of trailing whitespace and of literal "\n"/"\r" pairs, which would have
 * silently altered a password that legitimately ends in one. No
 * DebugAddPassword() call here: that writes the password in clear to a file
 * beside the exe, and this is a path every interactive login takes.
 * KITTY_PWDEBUG (lengths and checksums only) is the diagnostic. */
void SetPasswordInConfig( const char * password ) {
	if( GetUserPassSSHNoSave() || (password==NULL) || (conf==NULL) ) { return ; }
	kitty_pw_set( conf, CONF_password, password ) ;
	}

/* The same for the user name. CONF_username is a STR_AMBI key and the SSH
 * login prompt is UTF-8, so the typed name is stored as UTF-8 - the same way
 * cmdline.c stores a -l argument that arrived as UTF-8. */
void SetUsernameInConfig( const char * username ) {
	if( GetUserPassSSHNoSave() || (username==NULL) || (conf==NULL) ) { return ; }
	conf_set_utf8( conf, CONF_username, username ) ;
	}

/* hknet/KiTTY#50: the SSH layer's hand-back of a login the user TYPED, rather
 * than one held in the session. Installed by the terminal window (see
 * ssh_userauth_set_credentials_hook); the user name arrives as soon as it is
 * typed, the password only once the server has accepted it. Both setters above
 * do nothing when [KiTTY] userpasssshnosave is set. */
void kitty_userauth_credentials( Seat * seat, const char * username, const char * password ) {
	/* Only a login of a TERMINAL WINDOW's own connection counts. An SSH jump
	 * host authenticates behind a seat of its own, in this process, and its
	 * password is not the session's - writing it here is what handed the
	 * target the jump host's password (hknet/KiTTY#51). */
	Conf * c = kitty_seat_conf( seat ) ;
	if( c == NULL || GetUserPassSSHNoSave() ) { return ; }
	if( username != NULL ) { conf_set_utf8( c, CONF_username, username ) ; }
	if( password != NULL ) { kitty_pw_set( c, CONF_password, password ) ; }
	}

// Save the folder list
void SaveFolderList( void ) {
	int i = 0 ;
	kitty_store_mark_dirty() ;
	char buffer[4096] = "" ;
	while( FolderList[i] != NULL ) {
		if( strlen( FolderList[i] ) > 0 )
			strcat( buffer, FolderList[i] ) ;
		if( FolderList[i+1] != NULL ) 
			if( strlen( FolderList[i+1] ) > 0 )
				strcat( buffer, "," ) ;
		i++;
		}

	if( strlen( buffer ) > 0 ) 
		WriteParameter( INIT_SECTION, KI_FOLDERS, buffer ) ;
	}

// Save a registry key into a file

// Rename a registry key
void RegRenameTree( HWND hdlg, HKEY hMainKey, LPCTSTR lpSubKey, LPCTSTR lpDestKey ) { // hdlg = information box
	if( RegTestKey( hMainKey, lpDestKey ) ) {
		if( hdlg != NULL ) InfoBoxSetText( hdlg, KT_MAIN_INFO_CLEANING_BACKUP ) ;
		RegDelTree( hMainKey, lpDestKey ) ;
		}
	if( hdlg != NULL ) InfoBoxSetText( hdlg, KT_MAIN_INFO_SAVING_REGISTRY ) ;
	kitty_RegCopyTree( hMainKey, lpSubKey, lpDestKey ) ;
	if( hdlg != NULL ) InfoBoxSetText( hdlg, KT_MAIN_INFO_PREPARING_REGISTRY ) ;
	RegDelTree( hMainKey, lpSubKey ) ;
	}

int license_make_with_first( char * license, int length, int modulo, int result ) ;
void license_form( char * license, char sep, int size ) ;
int license_test( char * license, char sep, int modulo, int result ) ;

// Increment the usage counter in the registry
void CountUp( void ) {
	char buffer[4096] = "0", *pst ;
	long int n ;
	int len = 1024 ;
	
	if( ReadParameterN( INIT_SECTION, KI_KICOUNT, buffer, sizeof(buffer) ) == 0 ) { strcpy( buffer, "0" ) ; }
	n = atol( buffer ) + 1 ;
	snprintf( buffer, sizeof(buffer), "%ld", n ) ;
	WriteParameter( INIT_SECTION, KI_KICOUNT, buffer) ;
	
	/*
	 * KiTTY: KiLastUp, KiLastUH, KiSess, KiVers and KiPath used to be written
	 * here on every run. They are gone, and nothing replaces them.
	 *
	 * NOTHING EVER READ THEM. Verified repo-wide: the only references besides the
	 * writes were RegDeleteValue calls in the registry scrub. KiLastUp read only
	 * its OWN previous value, to append a second timestamp to it. This is
	 * telemetry-shaped bookkeeping inherited from classic KiTTY that outlived
	 * whatever was meant to consume it.
	 *
	 * Two of them - KiLastUH (your Windows username @ computer name) and KiVers
	 * (OS info) - were additionally "encrypted" with the compiled-in public
	 * constant, which is obfuscation and not encryption. That was the worst part
	 * rather than a redeeming one: kitty.ini gets shared - posted for support,
	 * copied between machines, committed to dotfiles - and scrambling those two
	 * values hid from their OWNER that their username and hostname were in the
	 * file at all. Plaintext would at least have been visible.
	 *
	 * kitty_retire_countup_leftovers() below removes them from stores that already
	 * have them; stopping the writes alone would just freeze stale values in place.
	 */

	if( ReadParameterN( INIT_SECTION, KI_KILIC, buffer, sizeof(buffer) ) == 0 ) {
		strcpy( buffer, "KI67" ) ;
		license_make_with_first( buffer, 25, 97, 0 )  ;
		license_form( buffer, '-', 5 ) ;
		WriteParameter( INIT_SECTION, KI_KILIC, buffer) ; 
		}
	else if( !license_test( buffer, '-', 97, 0 ) ) {
		strcpy( buffer, "KI67" ) ;
		license_make_with_first( buffer, 25, 97, 0 )  ;
		license_form( buffer, '-', 5 ) ;
		WriteParameter( INIT_SECTION, KI_KILIC, buffer) ; 
		}
	}

#include "kitty_help.h"
char * GetHelpMessage(void) {
	return default_help_file_content ;
}

// If kitty.ini does not exist, create the default file
#include "kitty_ini.h"
void CreateIniFile( const char * filename ) {
	FILE *fp;
	if( (fp=fopen(filename,"w")) != NULL ) {
		if( IniFileFlag == SAVEMODE_DIR ) {
			int p = poss( ";savemode=registry", default_init_file_content );
			del( default_init_file_content, p, 18 );
			insert( default_init_file_content, "savemode=dir", p );
		}
		fputs(default_init_file_content,fp);
		fclose(fp);
	}
}

#ifdef MOD_PERSO
/* KiTTY: expand window-title placeholders.
 *   %%h - hostname (falls back to the configured host)
 *   %%s - saved session name
 *   %%u - username
 *   %%p - port number
 *   %%P - protocol display name (title case)
 *   %%f - folder name
 *   %%l - forwarded local ports list
 *   %%d - forwarded dynamic ports list
 *   %%X (unknown) - collapse to a single '%', preserving the literal text
 */
char *kitty_expand_wintitle(const char *title, const char *hostname, Conf *conf)
{
    strbuf *sb = strbuf_new();
    const char *p = title;
    while (*p) {
        if (p[0] == '%' && p[1] == '%') {
            const char *val = NULL;
            char portbuf[32];
            strbuf *list = NULL;
            char code = p[2];

            switch (code) {
              case 'h':
                val = hostname;
                if (!val || !*val)
                    val = conf_get_str(conf, CONF_host);
                if (!val)
                    val = "";
                break;
              case 's':
                val = conf_get_str(conf, CONF_sessionname);
                if (!val)
                    val = "";
                break;
              case 'u':
                val = conf_get_str_ambi(conf, CONF_username, NULL);
                if (!val)
                    val = "";
                break;
              case 'f':
                val = conf_get_str(conf, CONF_folder);
                if (!val)
                    val = "";
                break;
              case 'p':
                snprintf( portbuf, sizeof(portbuf), "%d", conf_get_int(conf, CONF_port));
                val = portbuf;
                break;
              case 'P': {
                const struct BackendVtable *vt =
                    backend_vt_from_proto(conf_get_int(conf, CONF_protocol));
                val = vt ? vt->displayname_tc : "";
                break;
              }
              case 'l':
              case 'd': {
                char *key, *valfwd;
                list = strbuf_new();
                for (valfwd = conf_get_str_strs(conf, CONF_portfwd, NULL, &key);
                     valfwd != NULL;
                     valfwd = conf_get_str_strs(conf, CONF_portfwd, key, &key)) {
                    const char *k = key;
                    if (k[0] == ' ')
                        k++;
                    if ((code == 'l' && k[0] == 'L' &&
                         strcmp(valfwd, "D") != 0) ||
                        (code == 'd' && k[0] == 'L' &&
                         !strcmp(valfwd, "D"))) {
                        const char *port = k + 1;
                        if (list->len > 0)
                            put_fmt(list, ", %s", port);
                        else
                            put_fmt(list, "%s", port);
                    }
                }
                val = list->s;
                break;
              }
              default:
                val = NULL;
                break;
            }

            if (val) {
                put_data(sb, val, strlen(val));
                p += 3;
                if (list)
                    strbuf_free(list);
            } else {
                put_byte(sb, '%');
                p += 2;
            }
        } else {
            put_byte(sb, *p);
            p++;
        }
    }
    {
        char *result = dupstr(sb->s);
        strbuf_free(sb);
        return result;
    }
}
#endif

void set_title( TermWin *tw, const char *title ) { return win_set_title(tw,title,CP_ACP) ; } // Gone since version 0.71
void ManageProtect( HWND hwnd, TermWin *tw, char * title ) {
	HMENU m ;
	if( ( m = GetSystemMenu (hwnd, FALSE) ) != NULL ) {
		DWORD fdwMenu = GetMenuState( m, (UINT) IDM_PROTECT, MF_BYCOMMAND);
		if (!(fdwMenu & MF_CHECKED)) {
			CheckMenuItem( m, (UINT)IDM_PROTECT, MF_BYCOMMAND|MF_CHECKED ) ;
			ProtectFlag = 1 ;
		} else {
			CheckMenuItem( m, (UINT)IDM_PROTECT, MF_BYCOMMAND|MF_UNCHECKED ) ;
			ProtectFlag = 0 ;
		}
		/* re-decorate the CURRENT title (keeps a remote OSC-set title intact;
		 * the old set_title(tw,title) reset it to the config template) */
		kitty_refresh_title() ;
	}
}

// Handles sending the window to the system tray
int ManageToTray( HWND hwnd ) {
	//SendMessage(hwnd, WM_SYSCOMMAND, SC_MINIMIZE, 0);
	//MessageBox( NULL, "To tray", "Tray", MB_OK ) ;
	//SendMessage( hwnd, WM_SETICON, ICON_SMALL, (LPARAM)LoadIcon( hInstIcons, MAKEINTRESOURCE(IDI_MAINICON_0 + IconeNum ) ) );
	//The MYWM_NOTIFYICON message brings the window back

	int ResShell ;
	char buffer[256] ;
	/* Fully initialise the tray-icon struct here. Send-to-tray must NOT rely on
	 * the launcher having set it up, otherwise uFlags/uCallbackMessage/hIcon are
	 * unset -> a blank icon that ignores clicks, leaving no way to restore. */
	memset( &TrayIcone, 0, sizeof(TrayIcone) ) ;
	TrayIcone.cbSize = sizeof(TrayIcone) ;
	TrayIcone.hWnd = hwnd ;
	TrayIcone.uID = 1 ;
	TrayIcone.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP ;
	TrayIcone.uCallbackMessage = MYWM_NOTIFYICON ;
	TrayIcone.hIcon = (HICON)SendMessage( hwnd, WM_GETICON, ICON_SMALL, 0 ) ;
	if( !TrayIcone.hIcon ) TrayIcone.hIcon = (HICON)SendMessage( hwnd, WM_GETICON, ICON_BIG, 0 ) ;
	if( !TrayIcone.hIcon ) TrayIcone.hIcon = (HICON)(LONG_PTR)GetClassLongPtr( hwnd, GCLP_HICON ) ;
	if( !TrayIcone.hIcon ) TrayIcone.hIcon = LoadIcon( NULL, IDI_APPLICATION ) ;
	GetWindowText( hwnd, buffer, sizeof(buffer)-1 ) ; buffer[sizeof(buffer)-1] = '\0' ;
	strncpy( TrayIcone.szTip, buffer, sizeof(TrayIcone.szTip)-1 ) ;
	ResShell = Shell_NotifyIcon(NIM_ADD, &TrayIcone);
	if( ResShell ) {
		if (IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_HIDE);
		VisibleFlag = VISIBLE_TRAY ;
		return 1 ;
		}
	else return 0 ;
	}

// Restore a window sent to the system tray (click on the tray icon)
int RestoreFromTray( HWND hwnd ) {
	Shell_NotifyIcon( NIM_DELETE, &TrayIcone ) ;
	ShowWindow( hwnd, SW_SHOW ) ;
	ShowWindow( hwnd, SW_RESTORE ) ;
	SetForegroundWindow( hwnd ) ;
	VisibleFlag = VISIBLE_YES ;
	return 1 ;
	}

// Handles the always visible option
void ManageVisible( HWND hwnd, TermWin *tw, char * title ) {
	HMENU m ;
	if( ( m = GetSystemMenu (hwnd, FALSE) ) != NULL ) {
		DWORD fdwMenu = GetMenuState( m, (UINT) IDM_VISIBLE, MF_BYCOMMAND); 
		if (!(fdwMenu & MF_CHECKED)) {
			CheckMenuItem( m, (UINT)IDM_VISIBLE, MF_BYCOMMAND|MF_CHECKED ) ;
			SetWindowPos(hwnd,(HWND)-1,0,0,0,0,  SWP_NOMOVE |SWP_NOSIZE ) ;
			conf_set_bool( conf, CONF_alwaysontop, true ) ;
			set_title(tw, title) ;
		} else {
			CheckMenuItem( m, (UINT)IDM_VISIBLE, MF_BYCOMMAND|MF_UNCHECKED ) ;
			SetWindowPos(hwnd,(HWND)-2,0,0,0,0,  SWP_NOMOVE |SWP_NOSIZE ) ;
			conf_set_bool( conf, CONF_alwaysontop, false ) ;
			set_title(tw, title) ;
		}
	}
}

void ManageShortcutsFlag( HWND hwnd ) {
	HMENU m ;
	SetShortcutsFlag( abs(GetShortcutsFlag()-1) ) ;
	if( ( m = GetSystemMenu (hwnd, FALSE) ) != NULL ) {
		if( GetShortcutsFlag() ) {
			CheckMenuItem( m, (UINT)IDM_SHORTCUTSTOGGLE, MF_BYCOMMAND|MF_CHECKED ) ;
		} else {
			CheckMenuItem( m, (UINT)IDM_SHORTCUTSTOGGLE, MF_BYCOMMAND|MF_UNCHECKED ) ;
		}
	}
}

// Opens a config box with the current settings (but without a hostname)
void del_settings(const char *sessionname);

// Change the application icon
//SendMessage( hwnd, WM_SETICON, ICON_SMALL, (LPARAM)LoadIcon( hInstIcons, MAKEINTRESOURCE(IDI_MAINICON_0 + IconeNum ) ) );
void SetNewIcon( HWND hwnd, char * iconefile, int icone, const int mode ) {
	
	HICON hIcon = NULL, hIconBig = NULL ;
	if( (strlen(iconefile)>0) && existfile(iconefile) ) {
		hIcon = LoadImage(NULL, iconefile, IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_LOADFROMFILE|LR_SHARED) ; 
		hIconBig = LoadImage(NULL, iconefile, IMAGE_ICON, 0, 0, LR_LOADFROMFILE|LR_SHARED|LR_DEFAULTSIZE) ; 
	}

	if( hIcon || hIconBig ) {
		if(!hIcon) hIcon = hIconBig ;
		if(!hIconBig) hIconBig = hIcon ;
		SendMessage(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIconBig) ; 
		SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon) ;
		TrayIcone.hIcon = hIcon ;
		//DeleteObject( hIcon ) ; 
	} else {
		/* KiTTY: the embedded icon, indexed by the session's own CONF_icone.
		 * The cycling/randomising branch that used to sit here was dead - no
		 * caller ever passed anything but SI_INIT - and went with the rest of
		 * the multiple-icon feature. `mode` is kept for the callers. */
		int num = ( icone != 0 ) ? icone - 1 : 0 ;
		(void)mode ;
		hIcon = LoadIcon( hInstIcons, MAKEINTRESOURCE(IDI_MAINICON_0 + num ) ) ;
		SendMessage( hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon );
		SendMessage( hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon );
		TrayIcone.hIcon = hIcon ;
	}
	Shell_NotifyIcon(NIM_MODIFY, &TrayIcone);
}

// Switch the icon to the connection-lost icon
void SetConnBreakIcon( HWND hwnd ) {
#ifdef MOD_PERSO
	HICON hIcon = NULL ;
	hIcon = LoadIcon( hInstIcons, MAKEINTRESOURCE(IDI_NOCON) ) ;
	SendMessage( hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon );	
	SendMessage( hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon );
	TrayIcone.hIcon = hIcon ;
	Shell_NotifyIcon(NIM_MODIFY, &TrayIcone);
#endif
//To put it back
//SetNewIcon( hwnd, filename_to_str(conf_get_filename(conf,CONF_iconefile)), 0, SI_INIT ) ;
}

// Send a local script file
void RunScriptFile( HWND hwnd, const char * filename ) {
	long len = 0 ; size_t lread ;
	char * oldcmd = NULL ;
	FILE * fp ;
		/*
		strcpy( buffer, "" ) ;
		if( ( fp = fopen( filename, "r" ) ) != NULL ){
			while( fgets( buffer, 4096, fp) != NULL ) {
				SendKeyboard( hwnd, buffer ) ;
				}
			SendKeyboard( hwnd, "\n" ) ;
			fclose( fp ) ;
			}
		*/
	if( ScriptCommand != NULL ) { free( ScriptCommand ) ; ScriptCommand = NULL ; }
		if( existfile( filename ) ) {

		len = filesize( filename ) ;
		if( (AutoCommand!=NULL)&&(strlen(AutoCommand)>0) ) {
			oldcmd=(char*)malloc(strlen(AutoCommand)+3) ;
			sprintf( oldcmd, "\\n%s", AutoCommand );
			}
		if( oldcmd==NULL ) ScriptCommand = (char*) malloc( len + 1 ) ; 
		else ScriptCommand = (char*) malloc( len + strlen(oldcmd) + 2 ) ; 
		if( ( fp = fopen( filename, "r" ) ) != NULL ) {
			lread = fread( ScriptCommand, 1, len, fp ) ;
			ScriptCommand[lread]='\0' ;
			fclose( fp ) ;
			if( oldcmd!=NULL ) strcat( ScriptCommand, oldcmd ) ;
			if( strlen( ScriptCommand) > 0 ) {
				if( AutoCommand!= NULL ) { free(AutoCommand); AutoCommand=NULL; }
				AutoCommand = (char*) malloc( strlen(ScriptCommand) + 10 ) ;
				strcpy( AutoCommand, ScriptCommand ) ;//AutoCommand = ScriptCommand ;
				SetTimer(hwnd, TIMER_AUTOCOMMAND, autocommand_delay, NULL) ;
				}
			}
		if( oldcmd!=NULL ) free( oldcmd ) ;
		}
	}

void OpenAndSendScriptFile( HWND hwnd ) {
    char filename[4096], buffer[4096] ;
    if( ReadParameterN( INIT_SECTION, KI_SCRIPTFILEFILTER, buffer, sizeof(buffer) ) ) {
        buffer[4090]='\0';
    } else { 
        strcpy( buffer, "Script files (*.ksh,*.sh)|*.ksh;*.sh|SQL files (*.sql)|*.sql|All files (*.*)|*.*|" ) ;
    }
    if( strlen(buffer)==0 || buffer[strlen(buffer)-1]!='|' ) strcat( buffer, "|" ) ;
    if( OpenFileName( hwnd, filename, KT_CAP_OPEN_FILE, buffer ) ) {
        RunScriptFile( hwnd, filename ) ;
    }
}

// Winroll (window rollup) handling
void ManageWinrol( HWND hwnd, int resize_action ) {
    RECT rcClient ;
    int mode = -1 ;

    if( resize_action==RESIZE_DISABLED ) {
        mode = GetWindowLong(hwnd, GWL_STYLE) ;
        resize_action = RESIZE_TERM ;
        SetWindowLongPtr( hwnd, GWL_STYLE, mode|WS_THICKFRAME|WS_MAXIMIZEBOX ) ;
        SetWindowPos( hwnd, 0, 0, 0, 0, 0, SWP_FRAMECHANGED|SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER ) ;
    }

    if( WinHeight == -1 ) {
        GetWindowRect(hwnd, &rcClient) ;
        WinHeight  = rcClient.bottom-rcClient.top ;
        resize(0, rcClient.right-rcClient.left) ;
        MoveWindow( hwnd, rcClient.left, rcClient.top, rcClient.right-rcClient.left, 0, TRUE ) ;
    } else {
        GetWindowRect(hwnd, &rcClient) ;
        rcClient.bottom = rcClient.top + WinHeight ;
        resize(WinHeight, -1) ;
        MoveWindow( hwnd, rcClient.left, rcClient.top, rcClient.right-rcClient.left, WinHeight, TRUE ) ;
        WinHeight = -1 ;
    }

    if( mode != -1 ) {
        //winmode &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
        SetWindowLongPtr(hwnd, GWL_STYLE, mode ) ;
        SetWindowPos( hwnd, 0, 0, 0, 0, 0, SWP_FRAMECHANGED|SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER ) ;
        resize_action = RESIZE_DISABLED ;
    }

    InvalidateRect(hwnd, NULL, TRUE);
}

#ifdef MOD_BACKGROUNDIMAGE
#endif

void RefreshBackground( HWND hwnd ) {
#ifdef MOD_BACKGROUNDIMAGE
	if( GetBackgroundImageFlag() ) RedrawBackground( hwnd ) ;
	else
#endif
	InvalidateRect( hwnd, NULL, true ) ;
}

#ifdef MOD_BACKGROUNDIMAGE
/* Changing the background image */
static int GetExt( const char * filename, char * ext, size_t extsz) {
	int i;
	if( extsz>0 ) ext[0]='\0';
	if( filename==NULL ) return 0;
	if( strlen(filename)<=0 ) return 0;
	for( i=(strlen(filename)-1) ; i>=0 ; i-- )
		if( filename[i]=='.' ) snprintf( ext, extsz, "%s", filename+i+1 ) ;
	if( i<0 ) return 0;
	return 1;
	}

int PreviousBgImage( HWND hwnd ) {
	char buffer[1024], basename[1024], ext[10], previous[1024]="" ;
	int i ;
	DIR * dir ;
	struct dirent * de ;

	strcpy( basename, filename_to_str(conf_get_filename(conf,CONF_bg_image_filename)) ) ;

	for( i=(strlen(basename)-1) ; i>=0 ; i-- ) 
		if( (basename[i]=='\\')||(basename[i]=='/') ) { basename[i]='\0' ; break ; }
	if( i<0 ) strcpy( basename, ".") ;

	if( ( dir = opendir( basename ) ) == NULL ) { return 0 ; }
	
	while( ( de = readdir(dir) ) != NULL ) {
		if( strcmp(de->d_name,".") && strcmp(de->d_name,"..") ) {
			snprintf( buffer, sizeof(buffer), "%s\\%s", basename, de->d_name ) ;
			if( !(GetFileAttributes( buffer ) & FILE_ATTRIBUTE_DIRECTORY) ) {
				if( !strcmp(buffer, filename_to_str(conf_get_filename(conf,CONF_bg_image_filename)) ) )
					if( strcmp( previous, "" ) ) break ;
		
				GetExt( de->d_name, ext, sizeof(ext) ) ;
				if( (!stricmp(ext,"BMP"))||(!stricmp(ext,"JPG"))||(!stricmp(ext,"JPEG"))) 
					{ snprintf( previous, sizeof(previous), "%s\\%s", basename, de->d_name ) ; }
				}
			}
		}
	if( strcmp( previous, "" ) ){
		Filename * fn = filename_from_str( previous ) ;
		conf_set_filename(conf,CONF_bg_image_filename,fn); 
		filename_free(fn);
		RefreshBackground( hwnd ) ;
		}
	return 1 ;
	}

int NextBgImage( HWND hwnd ) {
	char buffer[1024], basename[1024], ext[10] ;
	int i ;
	DIR * dir ;
	struct dirent * de ;

	strcpy( basename, filename_to_str(conf_get_filename(conf,CONF_bg_image_filename)) ) ;

	for( i=(strlen(basename)-1) ; i>=0 ; i-- ) 
		if( (basename[i]=='\\')||(basename[i]=='/') ) { basename[i]='\0' ; break ; }
	if( i<0 ) strcpy( basename, ".") ;

	if( ( dir = opendir( basename ) ) == NULL ) { return 0 ; }
	
	while( ( de = readdir(dir) ) != NULL ) {
		GetExt( de->d_name, ext, sizeof(ext) ) ;

		if( strcmp(de->d_name,".") && strcmp(de->d_name,"..") 
			&& ( (!stricmp(ext,"BMP"))||(!stricmp(ext,"JPG"))||(!stricmp(ext,"JPEG"))) 
			) {
			snprintf( buffer, sizeof(buffer), "%s\\%s", basename, de->d_name ) ;
			if( !(GetFileAttributes( buffer ) & FILE_ATTRIBUTE_DIRECTORY) ) {
				if( !stricmp( buffer, filename_to_str(conf_get_filename(conf,CONF_bg_image_filename)) ) ) {
					if( ( de = readdir(dir) ) != NULL ) 
						GetExt( de->d_name, ext, sizeof(ext) ) ; 
					else 
						strcpy( ext, "" ) ;
						
					while( (de!=NULL)&&stricmp(ext,"BMP")&&stricmp(ext,"JPG")&&stricmp(ext,"JPEG") ) {
						if( ( de = readdir(dir) ) != NULL ) 
							GetExt( de->d_name, ext, sizeof(ext) ) ; 
						else 
							strcpy( ext, "" ) ;
						}
					break ;
					}
				}
			}
		}
	if( de==NULL ) { rewinddir( dir ) ; do { de = readdir(dir) ; } while( (!strcmp(de->d_name,".")) || (!strcmp(de->d_name,"..")) ) ; }
	if( de!=NULL ) GetExt( de->d_name, ext, sizeof(ext) ) ; else strcpy( ext, "" ) ;
	if( de!=NULL )
	while( (de!=NULL)&&stricmp(ext,"BMP")&&stricmp(ext,"JPG")&&stricmp(ext,"JPEG") ) {
		if( ( de = readdir(dir) ) != NULL ) GetExt( de->d_name, ext, sizeof(ext) ) ; else { strcpy( ext, "" ) ; break ; }
		}
	if( de != NULL  ) {
		snprintf( buffer, sizeof(buffer), "%s\\%s", basename, de->d_name ) ;
		Filename * fn = filename_from_str( buffer ) ;
		conf_set_filename( conf,CONF_bg_image_filename,fn);
		filename_free(fn);
		RefreshBackground( hwnd );
		}
	else { closedir(dir) ; return 0 ; }

	closedir( dir ) ;
	return 1 ;
	}
#endif
	
/* The InfoBox / InputBox dialog family (F8 send-text, SHIFT+F8 multiline,
 * password prompt, InfoBox progress popup) lives in kitty_inputbox.c. */

// Start the auto-command timer at connection time
void CreateTimerInit( void ) {
	SetTimer(MainHwnd, TIMER_INIT, init_delay, NULL) ; 
	}

// Set the directory the configuration is kept in
void SetConfigDirectory( const char * Directory ) {
	char *buf ;
	if( ConfigDirectory != NULL ) { 
		free( ConfigDirectory ) ; 
		ConfigDirectory = NULL ; 
	}
	if( (Directory!=NULL)&&(strlen(Directory)>0) ) {
		if( IsPathAbsolute(Directory) ) {
			buf = (char*) malloc(strlen(Directory)+1) ;
			strcpy( buf, Directory ) ;
		} else {
			buf = (char*)malloc(strlen(InitialDirectory)+strlen(Directory)+2) ;
			sprintf( buf, "%s\\%s", InitialDirectory, Directory ) ;
		}
		if( existdirectory(buf) ) {
			ConfigDirectory = (char*)malloc( strlen(buf)+1 ) ; 
			strcpy( ConfigDirectory, buf ) ; 
		}
		free( buf ) ;
	}
	if( ConfigDirectory==NULL ) { 
		ConfigDirectory = (char*)malloc( strlen(InitialDirectory)+1 ) ; 
		strcpy( ConfigDirectory, InitialDirectory ) ; 
	}
}
	
void GetInitialDirectory( char * InitialDirectory ) {
	int i ;
	if( GetModuleFileName( NULL, (LPTSTR)InitialDirectory, 4096 ) ) {
		if( strlen( InitialDirectory ) > 0 ) {
			i = strlen( InitialDirectory ) -1 ;
			do {
				if( InitialDirectory[i] == '\\' ) { InitialDirectory[i]='\0' ; i = 0 ; }
				i-- ;
			} while( i >= 0 ) ;
		}
	} else { 
		strcpy( InitialDirectory, "" ) ; 
	}
	
	SetConfigDirectory( InitialDirectory ) ;
}

/* The User-Command special menu (ReadSpecialMenu / InitSpecialMenu /
 * ManageSpecialCommand) lives in kitty_specialmenu.c. */
	
void SaveCurrentSetting( HWND hwnd ) {
	char filename[4096], buffer[4096] ;
	if( strlen(FileExtension)>0 ) {
		strcpy( buffer, "Connection files (*" ) ;
		strcat( buffer, FileExtension ) ; strcat( buffer, ")|*" ) ;
		strcat( buffer, FileExtension ) ; strcat( buffer, "|" ) ;
	} else {
		strcpy( buffer, "Connection files (*.ktx)|*.ktx|" ) ;
	}
	strcat( buffer, "All files (*.*)|*.*|" ) ;
	if( strlen(buffer)==0 || buffer[strlen(buffer)-1]!='|' ) strcat( buffer, "|" ) ;
	if( SaveFileName( hwnd, filename, KT_CAP_SAVE_FILE, buffer ) ) {
		save_open_settings_forced( filename, conf ) ;
		}
	}





// Startup script handling
void ManageInitScript( const char * input_str, const int len ) {
	int i, l ;
	char * st = NULL ;


	if( ScriptFileContent==NULL ) return ;
	if( strlen( ScriptFileContent ) == 0 ) { free( ScriptFileContent ) ; ScriptFileContent = NULL ; return ; }

	st = (char*) malloc( len+2 ) ;
	memcpy( st, input_str, len+1 ) ;
	for( i=0 ; i<len ; i++ ) if( st[i]=='\0' ) st[i]=' ' ;
	
	//if( debug_flag ) { debug_log( ">%d|", len ) ; debug_log( "%s|\n", st ) ; }

	if( strstr( st, ScriptFileContent ) != NULL ) {
		SendKeyboardPlus( MainHwnd, ScriptFileContent+strlen(ScriptFileContent)+1 ) ;
		l = strlen( ScriptFileContent ) + strlen( ScriptFileContent+strlen(ScriptFileContent)+1 ) + 2 ;
		
		//if( debug_flag ) { debug_log( "<%d|", l ) ; debug_log( "%s|\n", ScriptFileContent+strlen(ScriptFileContent)+1 ) ; }
		
		ScriptFileContent[0]=ScriptFileContent[l] ;
		i = 0 ;
		do {
			i++ ;
			ScriptFileContent[i]=ScriptFileContent[i+l] ;
		} while( (ScriptFileContent[i]!='\0')||(ScriptFileContent[i-1]!='\0') ) ;
	}
		
	free( st ) ;
}

/* At-rest protection for the login script: the same chokepoint saved passwords
 * use (kitty_secretstore.c), plus the base64 pair, because the script is a
 * NUL-separated blob rather than a C string. kitty_proxy.c declares the wrap the
 * same way - these live in kitty_secretstore.c. */

/*
 * The login script, as text a person can read and edit.
 *
 * Stored, it is a NUL-separated blob (expect\0send\0...\0\0) wrapped in the same
 * at-rest protection as the password. As text it is simply one entry per line -
 * which is exactly the format of the script FILE it was read from, so what the
 * config box shows is what the user originally wrote.
 *
 * These two live here rather than in kitty_config.c so the legacy decode, and
 * therefore MASTER_PASSWORD, stays confined to this file while it is being
 * retired.
 */
char *kitty_loginscript_to_text( const char *stored )
{
	char *plain = NULL, *out = NULL ;
	unsigned char *blob = NULL ;
	int blen = 0 ;

	if( !stored || !stored[0] ) return dupstr( "" ) ;
	/* A path that has not been inlined yet: nothing to show, and showing the
	 * path in a content box would invite someone to "correct" it. */
	if( existfile( (char*)stored ) ) return dupstr( "" ) ;

	if( (!ksec_stored_is_legacy( stored ) ||
	     !strncmp( stored, "PLAIN:", 6 )) &&    /* see ReadInitScript */
	    ksec_unprotect( stored, &plain ) > 0 && plain && plain[0] ) {
		blob = ksec_b64_decode( plain, &blen ) ;
	} else {
		char *tmp = dupstr( stored ) ;
		int l = decryptstring( GetCryptSaltFlag(), tmp, MASTER_PASSWORD ) ;
		if( l > 0 ) { blob = (unsigned char*)tmp ; blen = l ; }
		else sfree( tmp ) ;
	}
	if( plain ) { smemclr( plain, strlen(plain) ) ; sfree( plain ) ; }
	if( !blob || blen <= 0 ) { if( blob ) sfree( blob ) ; return dupstr( "" ) ; }

	out = kitty_loginscript_blob_to_lines( blob, blen ) ;
	smemclr( blob, blen ) ; sfree( blob ) ;
	return out ;
}

/* text (one entry per line) -> the protected stored form. Caller frees. */
char *kitty_loginscript_from_text( const char *text )
{
	unsigned char *raw ;
	int rawlen = 0 ;
	char *b64, *wrapped ;

	if( !text || !text[0] ) return dupstr( "" ) ;
	raw = kitty_loginscript_lines_to_blob( text, &rawlen ) ;
	if( !raw ) return dupstr( "" ) ;
	b64 = ksec_b64_encode( raw, rawlen ) ;
	smemclr( raw, rawlen ) ; sfree( raw ) ;
	if( !b64 ) return dupstr( "" ) ;
	wrapped = kitty_secret_wrap_current_backend( b64 ) ;
	smemclr( b64, strlen(b64) ) ; sfree( b64 ) ;
	return wrapped ? wrapped : dupstr( "" ) ;
}

void ReadInitScript( const char * filename ) {
	char * pst, *buffer=NULL, *name=NULL ;
	FILE *fp ;
	long l ; 

	if( filename != NULL )
		if( strlen( filename ) > 0 ) {
			name = (char*) malloc( strlen( filename ) + 1 ) ;
			strcpy( name, filename ) ;
		}
	if( name == NULL ) {
		if( strlen(conf_get_str(conf,CONF_scriptfilecontent)) >0 ) {
			name = (char*) malloc( strlen( conf_get_str(conf,CONF_scriptfilecontent) ) + 1 ) ;
			strcpy( name, conf_get_str(conf,CONF_scriptfilecontent) ) ;
		}
	}
	if( name != NULL ) {
		if( existfile( name ) ) {
			l=filesize(name) ;
			buffer=(char*)malloc(5*l+1);
			buffer[0]='\0' ;
			if( ( fp = fopen( name,"rb") ) != NULL ) {
				if( ScriptFileContent!= NULL ) free( ScriptFileContent ) ;
				l = 0 ;

				ScriptFileContent = (char*) malloc( filesize(name)+10 ) ;
				ScriptFileContent[0] = '\0' ;
				pst=ScriptFileContent ;
				while( fgets( buffer, 1024, fp ) != NULL ) {
					str_rtrim( buffer, "\n\r" ) ;
					if( strlen( buffer ) > 0 ) {
						strcpy( pst, buffer ) ;
						pst = pst + strlen( pst ) + 1 ;
						l = l + strlen( buffer ) + 1 ;
					}
				}
				pst[0] = '\0' ;
				l++ ;
				fclose( fp ) ;
				/*
				 * The login script is stored with the SAME at-rest protection as
				 * a saved password - DPAPI in the registry, the master password
				 * in a portable store - rather than scrambled with the constant
				 * compiled into every build.
				 *
				 * It sits in the same session record as CONF_password, and its
				 * "send" halves are what gets typed at login prompts, so it can
				 * hold a credential itself. Protecting the password properly and
				 * the text typed at the password prompt with a public key was the
				 * inconsistency worth removing; the constant is incidental.
				 *
				 * base64 FIRST, because ScriptFileContent is NUL-separated
				 * (expect\0send\0...\0\0) with an explicit length, and the
				 * protection chokepoint takes C strings - handing it the raw blob
				 * would silently store only up to the first NUL, i.e. the first
				 * expect string and nothing else.
				 */
				{
					char *b64 = ksec_b64_encode( (const unsigned char*)ScriptFileContent, (int)l ) ;
					char *wrapped = b64 ? kitty_secret_wrap_current_backend( b64 ) : NULL ;
					/*
					 * The stored form MUST carry a marker, whatever the backend. In
					 * PortablePasswordProtection=legacy mode the wrap hands the base64
					 * back UNMARKED - right for passwords, whose legacy format is the
					 * bare value - but the reader above classifies an unmarked script
					 * as pre-change content scrambled with the compiled-in constant
					 * and "decrypts" it into garbage: the login script silently died
					 * on the connect after the one that inlined it. PLAIN: is the
					 * marker that says stored-as-is (KITTY_SECRET_PLAIN_MARK).
					 */
					if( wrapped && wrapped[0] && !kitty_secret_is_marked( wrapped ) ) {
						char *marked = (char*) malloc( strlen(wrapped) + 7 ) ;
						sprintf( marked, "PLAIN:%s", wrapped ) ;
						free( wrapped ) ;
						wrapped = marked ;
					}
					if( wrapped ) {
						conf_set_str( conf, CONF_scriptfilecontent, wrapped ) ;
						free( wrapped ) ;
					}
					if( b64 ) { smemclr( b64, strlen(b64) ) ; free( b64 ) ; }
				}
			}
			if( buffer!=NULL ) { free(buffer); buffer=NULL; }
		} else {
			/*
			 * Not a path, so it is stored content. Which form it is in says how to
			 * open it, and the marker answers that without guessing: a protected
			 * value carries DPAPI1:/MPW2:/PLAIN:, and anything else is a session
			 * written before this change and still scrambled with the constant.
			 *
			 * Both are read for as long as it takes users to re-save. Dropping the
			 * legacy branch is part of retiring MASTER_PASSWORD itself, not of
			 * this change.
			 */
			char *plain = NULL ;
			/*
			 * Ask whether it CARRIES A MARKER, not whether unprotect succeeded.
			 * ksec_unprotect returns 1 for an unmarked value too, handing it back
			 * verbatim ("unmarked legacy == plaintext"), so branching on its
			 * return sends every pre-existing scrambled script down the base64
			 * path and breaks it. ksec_stored_is_legacy tests the markers -
			 * except PLAIN:, which it deliberately does not know (passwords'
			 * legacy format IS the bare value), so that one is tested here:
			 * a PLAIN: script is stored-as-is base64, not scrambled content.
			 */
			if( (!ksec_stored_is_legacy( name ) ||
			     !strncmp( name, "PLAIN:", 6 )) &&
			    ksec_unprotect( name, &plain ) > 0 && plain && plain[0] ) {
				int blen = 0 ;
				unsigned char *raw = ksec_b64_decode( plain, &blen ) ;
				if( raw && blen > 0 ) {
					if( ScriptFileContent != NULL ) free( ScriptFileContent ) ;
					ScriptFileContent = (char*) malloc( blen + 1 ) ;
					memcpy( ScriptFileContent, raw, blen ) ;
					ScriptFileContent[blen] = '\0' ;
				}
				if( raw ) { smemclr( raw, blen ) ; free( raw ) ; }
			} else if( (buffer=(char*)malloc(strlen(name)+1))!=NULL ) {
				/* legacy: scrambled with the compiled-in constant */
				strcpy( buffer, name ) ;
				l = decryptstring( GetCryptSaltFlag(), buffer, MASTER_PASSWORD ) ;
				if( ScriptFileContent!= NULL ) free( ScriptFileContent ) ;
				ScriptFileContent = (char*) malloc( l + 1 ) ;
				memcpy( ScriptFileContent, buffer, l ) ;
				free(buffer);buffer=NULL;
			}
			if( plain ) { smemclr( plain, strlen(plain) ) ; free( plain ) ; }
		}
	}
}



char *dirname(char *path);
#ifndef IDM_RECONF
#define IDM_RECONF    0x0050
#endif

/* NegativeColours / BlackOnWhiteColours / ChangeFontSize / ChangeSettings /
 * ManageViewer moved to kitty_colours.c (declared in kitty.h). */
	
/* The keyboard-shortcut machinery (DefineShortcuts / TranslateShortcuts /
 * InitShortcuts / ManageShortcuts + the shortcut tables) lives in
 * kitty_shortcuts.c; the tables are declared in kitty.h. */


/* The settings tree changed the [FontFallback] fallback list: hand it over
 * together with the three keys it does not edit, read again from the file
 * (they are file-only, and winfb_config_set takes all four at once). */
void kitty_fontfallback_apply_list( const char * list ) {
	char fbOvr[2048]="", fbLog[64]="", fbLogFile[MAX_PATH]="" ;
	readINI( KittyIniFile, KI_SECTION_FONTFALLBACK, KI_FONTFALLBACK_OVERRIDE, fbOvr, sizeof(fbOvr) ) ;
	readINI( KittyIniFile, KI_SECTION_FONTFALLBACK, KI_FONTFALLBACK_LOG, fbLog, sizeof(fbLog) ) ;
	readINI( KittyIniFile, KI_SECTION_FONTFALLBACK, KI_FONTFALLBACK_LOGFILE, fbLogFile, sizeof(fbLogFile) ) ;
	winfb_config_set( list ? list : "", fbOvr, fbLog, fbLogFile ) ;
}
