/*
 * File holding the procedures common to all the putty, pscp, psftp, plink and
 * pageant programs
 */

#include "kitty_commun.h"
#include "kitty_tools.h"
#include "kitty_inikeys.h"  /* KI_*: the kitty.ini key names */
#include "kitty_pwmem.h"    /* passwords wrapped in memory */

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
 * with Folder= inside it whatever this says, so with the flag on, no folder was
 * ever found: the folder list was empty and the tray menu could not group.
 *
 * WARNING: it is not merely unused. The portable enumerator skips directories and
 * does not recurse (enum_settings_start), so a session inside a subdirectory is
 * not listed at all - turning this on does not make a legacy tree readable, it
 * only changes where a folder NAME is looked for.
 */
int DirectoryBrowseFlag = 0 ;

// Flag to fall back to plain PuTTY mode
int PuttyFlag = 0 ;
int GetPuttyFlag(void) { return PuttyFlag ; }
void SetPuttyFlag( const int flag ) { PuttyFlag = flag ; }

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

// Flag preventing files from being written (default settings, jump list ...)
// [KiTTY] readonly=no
static int ReadOnlyFlag = 0 ;
int GetReadOnlyFlag(void) { return ReadOnlyFlag ; }
void SetReadOnlyFlag( const int flag ) { ReadOnlyFlag = flag ; }

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
// Delete a directory tree
int _rmdir( const char *dirname ) ;
void DelDir( const char * directory ) {
	DIR * dir ;
	struct dirent * de ;
	char fullpath[MAX_VALUE_NAME] ;

	if( (dir=opendir(directory)) != NULL ) {
		while( (de=readdir( dir ) ) != NULL ) 
		if( strcmp(de->d_name,".") && strcmp(de->d_name,"..") ) {
			snprintf( fullpath, sizeof(fullpath), "%s\\%s", directory, de->d_name ) ;
			if( GetFileAttributes( fullpath ) & FILE_ATTRIBUTE_DIRECTORY ) { DelDir( fullpath ) ; }
			else if( !(GetFileAttributes( fullpath ) & FILE_ATTRIBUTE_DIRECTORY) ) { unlink( fullpath ) ; }
			}
		closedir( dir ) ;
		_rmdir( directory ) ;
		}
	}

// Read a parameter either from the configuration file or from the registry
char  * IniFile = NULL ;
char INIT_SECTION[10];

/* The hive in use, not the compile-time default - see kitty_storage.c. */
extern const char *kitty_registry_base( void ) ;

// Bounded variant: never writes more than `size` bytes into `value`,
// final NUL included.
int ReadParameterLightN( const char * key, const char * name, char * value, size_t size ) {
	char buffer[4096] ;
	strcpy( buffer, "" ) ;

	if( GetValueData( HKEY_CURRENT_USER, kitty_registry_base(), name, buffer ) == NULL ) {
		if( !readINI( IniFile, key, name, buffer, sizeof(buffer) ) ) {
			strcpy( buffer, "" ) ;
			}
		}
	if( size == 0 ) return 0 ;
	if( strlen(buffer) >= size ) buffer[size-1] = '\0' ;
	strcpy( value, buffer ) ;
	return strcmp( buffer, "" ) ;
	}

// Compat: the old unbounded signature -- the destination buffer MUST be at
// least 4096 bytes. Prefer ReadParameterLightN( ..., sizeof(buf) ).
int ReadParameterLight( const char * key, const char * name, char * value ) {
	return ReadParameterLightN( key, name, value, 4096 ) ;
	}

/* test if we are in portable mode by looking for putty.ini or kitty.ini in running directory */
int LoadParametersLight( void ) {
	FILE * fp = NULL ;
	int ret = 0 ;
	char buffer[4096] ;

	if( (getenv("KITTY_INI_FILE")!=NULL) && ((fp = fopen( getenv("KITTY_INI_FILE"), "r" )) != NULL) ) {
		fclose(fp ) ;
		IniFile = (char*)malloc(strlen(getenv("KITTY_INI_FILE"))+1) ; 
		strcpy( IniFile,getenv("KITTY_INI_FILE") ) ;
		strcpy(INIT_SECTION,"KiTTY");
		if( readINI( IniFile, KI_SECTION_KITTY, KI_SAVEMODE, buffer, sizeof(buffer) ) ) {
			str_rtrim( buffer, "\n\r \t" ) ;
			if( !stricmp( buffer, "registry" ) ) IniFileFlag = SAVEMODE_REG ;
			else if( !stricmp( buffer, "file" ) ) IniFileFlag = SAVEMODE_FILE ;
			else if( !stricmp( buffer, "dir" ) ) { IniFileFlag = SAVEMODE_DIR ; ret = 1 ; }
		}
		if(  IniFileFlag == SAVEMODE_DIR ) {
			if( readINI( IniFile, KI_SECTION_KITTY, KI_BROWSEDIRECTORY, buffer, sizeof(buffer) ) ) { 
				if( !stricmp( buffer, "NO" )&&(IniFileFlag==SAVEMODE_DIR) ) DirectoryBrowseFlag = 0 ; 
				else DirectoryBrowseFlag = 1 ;
			}
			if( readINI( IniFile, KI_SECTION_KITTY, KI_CONFIGDIR, buffer, sizeof(buffer) ) ) {
				if( strlen( buffer ) > 0 ) { 
					ConfigDirectory = (char*)malloc( strlen(buffer) + 1 ) ;
					strcpy( ConfigDirectory, buffer ) ;
				}
			}
		} else  DirectoryBrowseFlag = 0 ;
	} else if( (fp = fopen( "kitty.ini", "r" )) != NULL ) {
		IniFile = (char*)malloc(11) ; strcpy(IniFile,"kitty.ini");
		strcpy(INIT_SECTION,"KiTTY");
		fclose(fp ) ;
		if( readINI( "kitty.ini", KI_SECTION_KITTY, KI_SAVEMODE, buffer, sizeof(buffer) ) ) {
			str_rtrim( buffer, "\n\r \t" ) ;
			if( !stricmp( buffer, "registry" ) ) IniFileFlag = SAVEMODE_REG ;
			else if( !stricmp( buffer, "file" ) ) IniFileFlag = SAVEMODE_FILE ;
			else if( !stricmp( buffer, "dir" ) ) { IniFileFlag = SAVEMODE_DIR ; ret = 1 ; }
		}
		if(  IniFileFlag == SAVEMODE_DIR ) {
			if( readINI( "kitty.ini", KI_SECTION_KITTY, KI_BROWSEDIRECTORY, buffer, sizeof(buffer) ) ) { 
				if( !stricmp( buffer, "NO" )&&(IniFileFlag==SAVEMODE_DIR) ) DirectoryBrowseFlag = 0 ; 
				else DirectoryBrowseFlag = 1 ;
			}
			if( readINI( "kitty.ini", KI_SECTION_KITTY, KI_CONFIGDIR, buffer, sizeof(buffer) ) ) { 
				if( strlen( buffer ) > 0 ) { 
					ConfigDirectory = (char*)malloc( strlen(buffer) + 1 ) ;
					strcpy( ConfigDirectory, buffer ) ;
				}
			}
		} else  DirectoryBrowseFlag = 0 ;
	} else 
	if( (fp = fopen( "putty.ini", "r" )) != NULL ) {
		IniFile = (char*)malloc(11) ; strcpy(IniFile,"putty.ini");
		strcpy(INIT_SECTION,"PuTTY");
		fclose(fp ) ;
		if( readINI( "putty.ini", KI_SECTION_PUTTY, KI_SAVEMODE, buffer, sizeof(buffer) ) ) {
			str_rtrim( buffer, "\n\r \t" ) ;
			if( !stricmp( buffer, "registry" ) ) IniFileFlag = SAVEMODE_REG ;
			else if( !stricmp( buffer, "file" ) ) IniFileFlag = SAVEMODE_FILE ;
			else if( !stricmp( buffer, "dir" ) ) { IniFileFlag = SAVEMODE_DIR ; DirectoryBrowseFlag = 1 ; ret = 1 ; }
		}
		if(  IniFileFlag == SAVEMODE_DIR ) {
			if( readINI( "putty.ini", KI_SECTION_PUTTY, KI_BROWSEDIRECTORY, buffer, sizeof(buffer) ) ) {
				if( !stricmp( buffer, "NO" )&&(IniFileFlag==SAVEMODE_DIR) ) DirectoryBrowseFlag = 0 ; 
				else DirectoryBrowseFlag = 1 ;
			}
			if( readINI( "putty.ini", KI_SECTION_PUTTY, KI_CONFIGDIR, buffer, sizeof(buffer) ) ) { 
				if( strlen( buffer ) > 0 ) { 
					ConfigDirectory = (char*)malloc( strlen(buffer) + 1 ) ;
					strcpy( ConfigDirectory, buffer ) ;
				}
			}
		} else  DirectoryBrowseFlag = 0 ;
	} else {
		snprintf( buffer, sizeof(buffer), "%s/KiTTY/kitty.ini", getenv("APPDATA") );
		if( (fp = fopen( buffer, "r" )) != NULL ) {
			IniFile = (char*)malloc(strlen(buffer)+1) ; 
			strcpy(IniFile,buffer);
			strcpy(INIT_SECTION,"KiTTY");
			fclose(fp);
		} else {
			snprintf( buffer, sizeof(buffer), "%s/PuTTY/putty.ini", getenv("APPDATA") );
			if( (fp = fopen( buffer, "r" )) != NULL ) {
				IniFile = (char*)malloc(strlen(buffer)+1) ; 
				strcpy(IniFile,buffer);
				strcpy(INIT_SECTION,"PuTTY");
				fclose(fp);
			} 
		}
	}
	if( ReadParameterLightN( INIT_SECTION, KI_FILEEXTENSION, buffer, sizeof(buffer) ) ) {
		if( strlen(buffer) > 0 ) {
			snprintf( FileExtension, sizeof(FileExtension), "%s%s", (buffer[0]!='.')?".":"", buffer ) ;
			str_rtrim( FileExtension, " " ) ;
		}				
	}
	return ret ;
}

// Flag saying whether we are connected
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
void unmungestr( const char *in, char *out, int outlen ) {
	while (*in) {
		if( *in == '%' && in[1] && in[2] ) {
			int i, j ;
			i = in[1] - '0' ;
			i -= (i > 9 ? 7 : 0) ;
			j = in[2] - '0' ;
			j -= (j > 9 ? 7 : 0) ;
			*out++ = (i << 4) + j ;
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
