/*
 * Fichier contenant les procedures communes à tous les programmes putty, pscp, psftp, plink, pageant
 */

#include "kitty_commun.h"
#include "kitty_tools.h"

// Flag permettant d'activer l'acces a du code particulier permettant d'avoir plus d'info dans le kitty.dmp
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

// Flag pour le fonctionnement en mode "portable" (gestion par fichiers)
#ifdef MOD_PORTABLE
int IniFileFlag = SAVEMODE_DIR ;
#else
int IniFileFlag = SAVEMODE_REG ;
#endif
int GetIniFileFlag(void) { return IniFileFlag ; }
void SetIniFileFlag( const int flag ) { IniFileFlag = flag ; }
void SwitchIniFileFlag(void) { if( IniFileFlag == SAVEMODE_REG ) { IniFileFlag = SAVEMODE_DIR ; } else if ( IniFileFlag == SAVEMODE_DIR ) { IniFileFlag = SAVEMODE_REG ; } }

// Flag permettant la gestion de l'arborscence (dossier=folder) dans le cas d'un savemode=dir
#ifdef MOD_PORTABLE
int DirectoryBrowseFlag = 1 ;
#else
int DirectoryBrowseFlag = 0 ;
#endif

// Flag pour repasser en mode Putty basic
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

// Flag permettant de desactiver la sauvegarde automatique des informations de connexion (user/password) Ã  la connexion SSH
static int UserPassSSHNoSave = 0 ;
int GetUserPassSSHNoSave(void) { return UserPassSSHNoSave ; }
void SetUserPassSSHNoSave( const int flag ) { UserPassSSHNoSave = flag ; }

// Flag pour empêcher l'écriture des fichiers (default settings, jump file list ...)
// [KiTTY] readonly=no
static int ReadOnlyFlag = 0 ;
int GetReadOnlyFlag(void) { return ReadOnlyFlag ; }
void SetReadOnlyFlag( const int flag ) { ReadOnlyFlag = flag ; }

#ifdef MOD_ZMODEM
// Flag pour inhiber les fonctions ZMODEM
static int ZModemFlag = 0 ;
int GetZModemFlag(void) { return ZModemFlag ; }
void SetZModemFlag( const int flag ) { ZModemFlag = flag ; }
#endif

// Flag pour afficher l'image de fond
#ifdef MOD_BACKGROUNDIMAGE
// Suite à PuTTY 0.61, le patch covidimus ne fonctionne plus tres bien
// Il impose de demarrer les sessions avec -load meme depuis la config box (voir CONFIG.C)
// Le patch est desactive par defaut
int BackgroundImageFlag = 0 ;
#else
int BackgroundImageFlag = 0 ;
#endif
int GetBackgroundImageFlag(void) { return BackgroundImageFlag ; }
void SetBackgroundImageFlag( const int flag ) { BackgroundImageFlag = flag ; }

// Pour supprimer le sel dans le cryptage du mot de passe
int CryptSaltFlag = 0 ;
int GetCryptSaltFlag() { return CryptSaltFlag ; }
void SetCryptSaltFlag( int flag ) { CryptSaltFlag = flag ; }

// Répertoire de sauvegarde de la configuration (savemode=dir)
char * ConfigDirectory = NULL ;

char * GetConfigDirectory( void ) { return ConfigDirectory ; }

#ifndef stricmp	/* platform.h may #define stricmp _stricmp (CRT); don't redeclare */
int stricmp(const char *s1, const char *s2) ;
#endif
int readINI( const char * filename, const char * section, const char * key, char * pStr, size_t pStrSize) ;
char * SetSessPath( const char * dec ) ;

// Nettoie les noms de folder en remplaçant les "/" par des "\" et les " \ " par des " \"
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
// Supprime une arborescence
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

// Lit un parametre soit dans le fichier de configuration, soit dans le registre
char  * IniFile = NULL ;
char INIT_SECTION[10];
// Variante bornee: n'ecrit jamais plus de `size` octets (NUL final compris) dans `value`.
int ReadParameterLightN( const char * key, const char * name, char * value, size_t size ) {
	char buffer[4096] ;
	strcpy( buffer, "" ) ;

	if( GetValueData( HKEY_CURRENT_USER, TEXT(PUTTY_REG_POS), name, buffer ) == NULL ) {
		if( !readINI( IniFile, key, name, buffer, sizeof(buffer) ) ) {
			strcpy( buffer, "" ) ;
			}
		}
	if( size == 0 ) return 0 ;
	if( strlen(buffer) >= size ) buffer[size-1] = '\0' ;
	strcpy( value, buffer ) ;
	return strcmp( buffer, "" ) ;
	}

// Compat: ancienne signature non bornee -- le buffer destinataire DOIT faire
// au moins 4096 octets. Preferer ReadParameterLightN( ..., sizeof(buf) ).
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
		if( readINI( IniFile, "KiTTY", "savemode", buffer, sizeof(buffer) ) ) {
			str_rtrim( buffer, "\n\r \t" ) ;
			if( !stricmp( buffer, "registry" ) ) IniFileFlag = SAVEMODE_REG ;
			else if( !stricmp( buffer, "file" ) ) IniFileFlag = SAVEMODE_FILE ;
			else if( !stricmp( buffer, "dir" ) ) { IniFileFlag = SAVEMODE_DIR ; ret = 1 ; }
		}
		if(  IniFileFlag == SAVEMODE_DIR ) {
			if( readINI( IniFile, "KiTTY", "browsedirectory", buffer, sizeof(buffer) ) ) { 
				if( !stricmp( buffer, "NO" )&&(IniFileFlag==SAVEMODE_DIR) ) DirectoryBrowseFlag = 0 ; 
				else DirectoryBrowseFlag = 1 ;
			}
			if( readINI( IniFile, "KiTTY", "configdir", buffer, sizeof(buffer) ) ) {
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
		if( readINI( "kitty.ini", "KiTTY", "savemode", buffer, sizeof(buffer) ) ) {
			str_rtrim( buffer, "\n\r \t" ) ;
			if( !stricmp( buffer, "registry" ) ) IniFileFlag = SAVEMODE_REG ;
			else if( !stricmp( buffer, "file" ) ) IniFileFlag = SAVEMODE_FILE ;
			else if( !stricmp( buffer, "dir" ) ) { IniFileFlag = SAVEMODE_DIR ; ret = 1 ; }
		}
		if(  IniFileFlag == SAVEMODE_DIR ) {
			if( readINI( "kitty.ini", "KiTTY", "browsedirectory", buffer, sizeof(buffer) ) ) { 
				if( !stricmp( buffer, "NO" )&&(IniFileFlag==SAVEMODE_DIR) ) DirectoryBrowseFlag = 0 ; 
				else DirectoryBrowseFlag = 1 ;
			}
			if( readINI( "kitty.ini", "KiTTY", "configdir", buffer, sizeof(buffer) ) ) { 
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
		if( readINI( "putty.ini", "PuTTY", "savemode", buffer, sizeof(buffer) ) ) {
			str_rtrim( buffer, "\n\r \t" ) ;
			if( !stricmp( buffer, "registry" ) ) IniFileFlag = SAVEMODE_REG ;
			else if( !stricmp( buffer, "file" ) ) IniFileFlag = SAVEMODE_FILE ;
			else if( !stricmp( buffer, "dir" ) ) { IniFileFlag = SAVEMODE_DIR ; DirectoryBrowseFlag = 1 ; ret = 1 ; }
		}
		if(  IniFileFlag == SAVEMODE_DIR ) {
			if( readINI( "putty.ini", "PuTTY", "browsedirectory", buffer, sizeof(buffer) ) ) {
				if( !stricmp( buffer, "NO" )&&(IniFileFlag==SAVEMODE_DIR) ) DirectoryBrowseFlag = 0 ; 
				else DirectoryBrowseFlag = 1 ;
			}
			if( readINI( "putty.ini", "PuTTY", "configdir", buffer, sizeof(buffer) ) ) { 
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
	if( ReadParameterLightN( INIT_SECTION, "fileextension", buffer, sizeof(buffer) ) ) {
		if( strlen(buffer) > 0 ) {
			snprintf( FileExtension, sizeof(FileExtension), "%s%s", (buffer[0]!='.')?".":"", buffer ) ;
			str_rtrim( FileExtension, " " ) ;
		}				
	}
	return ret ;
}

// Positionne un flag permettant de determiner si on est connecte
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

// Fonctions permettant de formatter les chaînes de caractères avec %XY
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

// Fonctions de gestion du mot de passe
extern Conf *conf;
void GetPasswordInConfig( char * p ) {
	if( strlen(conf_get_str(conf,CONF_password)) == 0 ) return ;
	/* On decrypte le password */
	char bufpass[4096] ;
	int len = strlen( conf_get_str(conf,CONF_password) ) ;
	if( len>4095 ) { len = 4095 ; }
	memcpy( bufpass, conf_get_str(conf,CONF_password), len ) ; 
	bufpass[len]='\0';
	MASKPASS(GetCryptSaltFlag(),bufpass) ;
	//DebugAddPassword( "GetPasswordInConfig", bufpass ) ; // in settings.c
	memcpy( p, bufpass, strlen(bufpass)+1 ) ;
	memset( bufpass,0,strlen(bufpass) ) ;
}

int IsPasswordInConf(void) {
	int len = 0 ;
	if( strlen(conf_get_str(conf,CONF_password)) == 0 ) return 0 ;
	/* On decrypte le password */
	char bufpass[4096] ;
	len = strlen( conf_get_str(conf,CONF_password) ) ;
	if( len>4095 ) { len = 4095 ; }
	memcpy( bufpass, conf_get_str(conf,CONF_password), len ) ; 
	bufpass[len]='\0';
	MASKPASS(GetCryptSaltFlag(),bufpass);
	//DebugAddPassword( "IsPasswordInConf", bufpass ) ; // in settings.c
	len = strlen( bufpass ) ;
	memset(bufpass,0,strlen(bufpass));
	return len ;
}

void CleanPassword( char * p ) { memset(p,0,strlen(p)); }

// Extention pour les fichiers de session en mode portable (peut être ktx)
char FileExtension[15] = "" ;

// Répertoire courant pourle mode portable
char CurrentFolder[1024] = "Default" ;

#endif
