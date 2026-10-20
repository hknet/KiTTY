/*
 * File holding the procedures common to all the putty, pscp, psftp, plink and
 * pageant programs
 */

#include "kitty_common.h"
#include "kitty_tools.h"
#include "kitty_inikeys.h"  /* KI_*: the kitty.ini key names */
#include "kitty_pwmem.h"    /* passwords wrapped in memory */
#include "kitty_inifile.h"  /* kitty_inifile_readonly: the readonly flag */

// Flag enabling extra code paths that put more information into kitty.dmp
int debug_flag = 0 ;

#ifdef MOD_PERSO

#ifndef SAVEMODE_REG
#define SAVEMODE_REG 0
#endif
#ifndef SAVEMODE_FILE
#define SAVEMODE_FILE 1
#endif
#ifndef SAVEMODE_DIR
#define SAVEMODE_DIR 2
#endif

// Flag for running in "portable" mode (settings kept in files)
#ifdef MOD_PORTABLE
int IniFileFlag = SAVEMODE_DIR ;
#else
int IniFileFlag = SAVEMODE_REG ;
#endif
int GetIniFileFlag(void) { return IniFileFlag ; }

/*
 * Folders as SUBDIRECTORIES under Sessions\, for savemode=dir. Off by default
 * in every build, and opt-in through [KiTTY] browsedirectory=yes.
 *
 * It used to default on in the portable build, and savemode=dir switched it on
 * everywhere else - which sent the folder lookup hunting for subdirectories
 * that nothing creates. The session writer stores one flat file per session
 * with Folder= inside it either way, so with the flag on, no folder was
 * ever found: the folder list was empty and the tray menu could not group.
 *
 * WARNING: it is not merely unused. The portable enumerator skips directories and
 * does not recurse (enum_settings_start), so a session inside a subdirectory is
 * not listed at all - turning this on does not make a legacy tree readable, it
 * only changes where a folder NAME is looked for.
 */

int ModalErrorsFlag = 0 ;
int GetModalErrorsFlag(void) { return ModalErrorsFlag ; }
void SetModalErrorsFlag( const int flag ) { ModalErrorsFlag = flag ; }

/* Inline-first security prompts (successor to cyd01/KiTTY #548). Each flag
 * defaults to 1 = classic modal box (today's behaviour). Set the matching
 * kitty.ini [KiTTY] key to no to get an OpenSSH-style in-terminal prompt
 * instead. See windows/dialog.c for the surfacing logic. */
int ModalNewHostKeyConfirmationFlag = 1 ;
int GetModalNewHostKeyConfirmationFlag(void) { return ModalNewHostKeyConfirmationFlag ; }
void SetModalNewHostKeyConfirmationFlag( const int flag ) { ModalNewHostKeyConfirmationFlag = flag ; }

int ModalChangedHostKeyConfirmationFlag = 1 ;
int GetModalChangedHostKeyConfirmationFlag(void) { return ModalChangedHostKeyConfirmationFlag ; }
void SetModalChangedHostKeyConfirmationFlag( const int flag ) { ModalChangedHostKeyConfirmationFlag = flag ; }

int ModalWeakKeyConfirmationFlag = 1 ;
int GetModalWeakKeyConfirmationFlag(void) { return ModalWeakKeyConfirmationFlag ; }
void SetModalWeakKeyConfirmationFlag( const int flag ) { ModalWeakKeyConfirmationFlag = flag ; }

// Flag disabling the automatic saving of the login details (user/password)
// on SSH connection
static int UserPassSSHNoSave = 0 ;
int GetUserPassSSHNoSave(void) { return UserPassSSHNoSave ; }
void SetUserPassSSHNoSave( const int flag ) { UserPassSSHNoSave = flag ; }

// [KiTTY] readonly=yes: the settings store is not written (registry hive,
// folder store, kitty.ini, kitty.sav). The answer is the resolver's
// (kitty_inifile.c), known from the moment kitty.ini is chosen - before
// LoadParameters - and the same one the other programs of the suite read.
// SetReadOnlyFlag is the kitty.ini table's setter: a readonly value found in
// the hive (ReadParameter reads it first) still switches it on.
int GetReadOnlyFlag(void) { return kitty_inifile_readonly() ; }
void SetReadOnlyFlag( const int flag ) { kitty_inifile_readonly_force( flag ) ; }

#ifdef MOD_ZMODEM
// Flag to disable the ZMODEM functions
/* KiTTY: on by default. The shipped kitty.ini template has always written
 * zmodem=yes, so the feature was present for anyone whose kitty.ini had been
 * generated and absent for everyone else - including every registry-mode
 * install without one. The template line is now a no-op restating this. */
static int ZModemFlag = 1 ;
int GetZModemFlag(void) { return ZModemFlag ; }
void SetZModemFlag( const int flag ) { ZModemFlag = flag ; }
#endif

// Flag to display the background image
#ifdef MOD_BACKGROUNDIMAGE
// Since PuTTY 0.61 the covidimus patch no longer works very well
// It forces sessions to start with -load even from the config box (CONFIG.C)
// The patch is disabled by default
int BackgroundImageFlag = 0 ;
#else
int BackgroundImageFlag = 0 ;
#endif
int GetBackgroundImageFlag(void) { return BackgroundImageFlag ; }
void SetBackgroundImageFlag( const int flag ) { BackgroundImageFlag = flag ; }

// To remove the salt from the password encryption
int CryptSaltFlag = 0 ;
int GetCryptSaltFlag(void) { return CryptSaltFlag ; }
void SetCryptSaltFlag( int flag ) { CryptSaltFlag = flag ; }

// Directory the configuration is saved into (savemode=dir)
char * ConfigDirectory = NULL ;

char * GetConfigDirectory( void ) { return ConfigDirectory ; }

#ifndef stricmp	/* platform.h may #define stricmp _stricmp (CRT); don't redeclare */
int stricmp(const char *s1, const char *s2) ;
#endif
int readINI( const char * filename, const char * section, const char * key, char * pStr, size_t pStrSize) ;
char * SetSessPath( const char * dec ) ;

// Clean folder names: replace "/" with "\" and " \ " with " \"
void CleanFolderName( char * folder ) {
	int i, j ;
	if( folder == NULL ) return ;
	if( strlen( folder ) == 0 ) return ;
	for( i=0 ; i<strlen(folder) ; i++ ) if( folder[i]=='/' ) folder[i]='\\' ;
	for( i=0 ; i<(strlen(folder)-1) ; i++ ) 
		if( folder[i]=='\\' ) 
			while( folder[i+1]==' ' ) for( j=i+1 ; j<strlen(folder) ; j++ ) folder[j]=folder[j+1] ;
	for( i=(strlen(folder)-1) ; i>0 ; i-- )
		if( folder[i]=='\\' )
			while( folder[i-1]==' ' ) {
				for( j=i-1 ; j<strlen(folder) ; j++ ) folder[j]=folder[j+1] ;
				i-- ;
				}
	}

#include <sys/types.h>
#include <dirent.h>
#define MAX_VALUE_NAME 16383
/* a\b into out (n wide characters); 0 when it does not fit */
static int kitty_wjoin( wchar_t * out, size_t n, const wchar_t * a, const wchar_t * b ) {
	size_t la = wcslen( a ), lb = wcslen( b ) ;
	if( la + 1 + lb + 1 > n ) return 0 ;
	wcscpy( out, a ) ;
	out[la] = L'\\' ;
	wcscpy( out + la + 1, b ) ;
	return 1 ;
	}

/* The tree walk in wide characters: the ANSI walk (opendir) handed back '?'
 * for every character outside the code page, and a name with '?' can be
 * neither opened nor deleted, so such a file - a session named in Cyrillic -
 * and every folder above it stayed behind. A junction or symbolic link to a
 * folder is removed as the link; what it points to is not emptied. */
static void kitty_deldir_w( const wchar_t * dir ) {
	wchar_t pattern[4096], full[4096] ;
	WIN32_FIND_DATAW fd ;
	HANDLE h ;
	if( kitty_wjoin( pattern, 4096, dir, L"*" ) &&
	    ( h = FindFirstFileW( pattern, &fd ) ) != INVALID_HANDLE_VALUE ) {
		do {
			if( !wcscmp( fd.cFileName, L"." ) || !wcscmp( fd.cFileName, L".." ) ) continue ;
			if( !kitty_wjoin( full, 4096, dir, fd.cFileName ) ) continue ;
			if( fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) {
				if( fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT ) RemoveDirectoryW( full ) ;
				else kitty_deldir_w( full ) ;
				}
			else DeleteFileW( full ) ;
			} while( FindNextFileW( h, &fd ) ) ;
		FindClose( h ) ;
		}
	RemoveDirectoryW( dir ) ;
	}

// Delete a directory tree
void DelDir( const char * directory ) {
	wchar_t w[4096] ;
	if( directory == NULL || !directory[0] ) return ;
	if( !MultiByteToWideChar( CP_ACP, 0, directory, -1, w, 4096 ) ) return ;
	kitty_deldir_w( w ) ;
	}

// Read a parameter either from the configuration file or from the registry
char  * IniFile = NULL ;

// Flag recording whether we are connected
int is_backend_connected = 0 ;

#ifdef MOD_RECONNECT
int is_backend_first_connected = 0 ; 
void SetSSHConnected( int flag ) {
	is_backend_connected = flag ; 
	if( flag ) is_backend_first_connected = 1 ; 
	}
#else
void SetSSHConnected( int flag ) { 
	is_backend_connected = flag ; 
	}
#endif

//PVOID SecureZeroMemory( PVOID ptr, SIZE_T cnt) { return memset( ptr, 0, cnt ) ; }

// Functions escaping strings with %XY sequences
void mungestr( const char *in, char *out ) {
	char hex[16] = "0123456789ABCDEF";
	int candot = 0 ;
	while( *in ) {
		if( *in == ' ' || *in == '\\' || *in == '*' || *in == '?' ||
			*in ==':' || *in =='/' || *in =='\"' || *in =='<' || *in =='>' || *in =='|' ||
			*in == '%' || *in < ' ' || *in > '~' || (*in == '.'
			&& !candot) ) {
			*out++ = '%' ;
			*out++ = hex[((unsigned char) *in) >> 4] ;
			*out++ = hex[((unsigned char) *in) & 15] ;
		} else {
			*out++ = *in ;
		}
		in++ ;
		candot = 1 ;
	}
	*out = '\0' ;
	return ;
}
static int unmunge_hex( char c ) {
	if( c >= '0' && c <= '9' ) return c - '0' ;
	if( c >= 'A' && c <= 'F' ) return c - 'A' + 10 ;
	if( c >= 'a' && c <= 'f' ) return c - 'a' + 10 ;
	return -1 ;
}
/* %XX in either case. A % not followed by two hex digits stays as it is,
   as in ksf_unmunge. The old arithmetic took any two characters and turned
   lower-case hex into a wrong byte. */
void unmungestr( const char *in, char *out, int outlen ) {
	while (*in) {
		int i, j ;
		if( *in == '%' && in[1] && in[2] &&
		    (i = unmunge_hex( in[1] )) >= 0 && (j = unmunge_hex( in[2] )) >= 0 ) {
			*out++ = (char)((i << 4) + j) ;
			if( !--outlen )
				return ;
			in += 3	;
		} else {
			*out++ = *in++ ;
			if( !--outlen )
				return;
		}
	}
	*out = '\0' ;
	return ;
}

/* The session password, out of the running configuration.
 *
 * The value is held wrapped in memory (kitty/kitty_pwmem.c), so these read it
 * through the accessors. They no longer apply MASKPASS: that undid an encoding
 * the running configuration has not carried since the load path started
 * decrypting a stored password, so it turned a good password into garbage. */
extern Conf *conf;
void GetPasswordInConfig( char * p ) {
	/* `p` is the caller's buffer and must hold KITTY_PW_MAX+1 bytes. */
	if( p == NULL ) return ;
	kitty_pw_get( conf, CONF_password, p, KITTY_PW_MAX + 1 ) ;
}

int IsPasswordInConf(void) {
	char bufpass[KITTY_PW_MAX+1] ;
	int len = (int)kitty_pw_get( conf, CONF_password, bufpass, sizeof(bufpass) ) ;
	smemclr( bufpass, sizeof(bufpass) ) ;
	return len ;
}

/* Session-file extension in portable mode (a .ktx among them) */
char FileExtension[15] = "" ;

// Current folder for portable mode
char CurrentFolder[1024] = "Default" ;

#endif
