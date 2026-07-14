/*************************************************
** DEFINITION DES INCLUDES
*************************************************/
// Includes classiques
#include <dirent.h>
#include <io.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/locking.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

// Includes de PuTTY
#include "putty.h"
#include "terminal.h"
//#include "ldisc.h"
//#include "win_res.h"
#include "putty-rc.h"

// Include specifiques Windows (windows.h doit imperativement etre declare en premier)
#include <windows.h>
#include <psapi.h>
#include <iphlpapi.h>

// Includes de KiTTY
#include "kitty.h"
#include "kitty_defs.h"     /* KITTY_DEFAULT_SESSION */
#include "kitty_commun.h"
#include "kitty_image.h"
#include "kitty_crypt.h"
#include "kitty_registry.h"
#include "kitty_tools.h"
#include "kitty_win.h"
#include "kitty_launcher.h"
#include "MD5check.h"
/*************************************************
** FIN DE LA DEFINITION DES INCLUDES
*************************************************/


/*************************************************
** DEFINITION DE LA STRUCTURE DE CONFIGURATION
*************************************************/
// La structure de configuration est instanciee dans window.c
extern Conf *conf ;

#ifndef SAVEMODE_REG
#define SAVEMODE_REG 0
#endif
#ifndef SAVEMODE_FILE
#define SAVEMODE_FILE 1
#endif
#ifndef SAVEMODE_DIR
#define SAVEMODE_DIR 2
#endif

// Flag pour le fonctionnement en mode "portable" (gestion par fichiers), defini dans kitty_commun.c
extern int IniFileFlag ;

// Flag permettant la gestion de l'arborscence (dossier=folder) dans le cas d'un savemode=dir, defini dans kitty_commun.c
extern int DirectoryBrowseFlag ;
int GetDirectoryBrowseFlag(void) { return DirectoryBrowseFlag ; }
void SetDirectoryBrowseFlag( const int flag ) { DirectoryBrowseFlag = flag ; }


#define SI_INIT 0
#define SI_NEXT 1
#define SI_RANDOM 2

// Stuff for drag-n-drop transfers
#define TIMER_DND 8777
int dnd_delay = 250;
HDROP hDropInf = NULL;

// Delai avant d'envoyer le password et d'envoyer vers le tray (automatiquement à la connexion) (en milliseconde)
int init_delay = 2000 ;
// Delai entre chaque ligne de la commande automatique (en milliseconde)
int autocommand_delay = 5 ;
// Delai entre chaque caracteres d'une commande (en millisecondes)
int between_char_delay = 0 ;
// Delai entre deux lignes d'une meme commande et entre deux raccourcis \x \k
int internal_delay = 10 ;

// Pointeur sur la commande autocommand
char * AutoCommand = NULL ;

// Contenu d'un script a envoyer à l'ecran
char * ScriptCommand = NULL ;

// paste size limit (number of characters). Above the limit a confirmation is requested. (0 means unlimited)
static int PasteSize = 0 ;
int GetPasteSize(void) { return PasteSize ; }
void SetPasteSize( const int size ) { PasteSize = size ; }

// Flag de gestion de la fonction hyperlink. In 0.84 hyperlinks are provided by
// kitty_url.c/window.c, not the historical terminal.c hyperlink patch, so keep
// the feature available by default and let kitty.ini "hyperlink" disable it.
int HyperlinkFlag = 1 ;
int GetHyperlinkFlag(void) { return HyperlinkFlag ; }
void SetHyperlinkFlag( const int flag ) { HyperlinkFlag = flag ; }

// Flag de gestion de la Transparence
// The feature stays available (so transparency is configurable PER SESSION via the
// Window > Transparency panel), but it is OFF by default because the per-session
// TransparencyValue defaults to 0 = fully opaque / non-layered (see conf.h):
// kitty_apply_transparency() applies nothing until a session sets a value > 0.
// kitty.ini [KiTTY] transparency=no remains the master switch that removes the
// whole feature (config panel + system-menu adjust items).
static int TransparencyFlag = 1 ;
int GetTransparencyFlag(void) { return TransparencyFlag ; }
void SetTransparencyFlag( const int flag ) { TransparencyFlag = flag ; }

// Gestion du script file au lancement
char * ScriptFileContent = NULL ;

// Flag pour la protection contre les saisies malheureuses
static int ProtectFlag = 0 ; 
int GetProtectFlag(void) { return ProtectFlag ; }
void SetProtectFlag( const int flag ) { ProtectFlag = flag ; }

// Flags de definition du mode de sauvegarde
#ifndef SAVEMODE_REG
#define SAVEMODE_REG 0
#endif
#ifndef SAVEMODE_FILE
#define SAVEMODE_FILE 1
#endif
#ifndef SAVEMODE_DIR
#define SAVEMODE_DIR 2
#endif

// Definition de la section du fichier de configuration
#ifdef MOD_PERSO
#ifndef INIT_SECTION
#define INIT_SECTION "KiTTY"
#endif
#ifndef DEFAULT_INIT_FILE
#define DEFAULT_INIT_FILE "kitty.ini"
#endif
#ifndef DEFAULT_SAV_FILE
/* KiTTY 0.84: kittynew.sav (timestamped copies: kittynew-YYYYMMDD-HHMMSS.sav),
 * NOT kitty.sav, so we never overwrite the registry backup of an old (0.76)
 * KiTTY installed side by side. */
#define DEFAULT_SAV_FILE "kittynew.sav"
#endif
#ifndef DEFAULT_EXE_FILE
#define DEFAULT_EXE_FILE "kitty.exe"
#endif
#else
#ifndef INIT_SECTION
#define INIT_SECTION "PuTTY"
#endif
#ifndef DEFAULT_INIT_FILE
#define DEFAULT_INIT_FILE "putty.ini"
#endif
#ifndef DEFAULT_SAV_FILE
#define DEFAULT_SAV_FILE "putty.sav"
#endif
#ifndef DEFAULT_EXE_FILE
#define DEFAULT_EXE_FILE "putty.exe"
#endif
#endif

#ifndef VISIBLE_NO
#define VISIBLE_NO 0
#endif
#ifndef VISIBLE_YES
#define VISIBLE_YES 1
#endif
#ifndef VISIBLE_TRAY
#define VISIBLE_TRAY -1
#endif

// Flag de definition de la visibilite d'une fenetres
static int VisibleFlag = VISIBLE_YES ;
int GetVisibleFlag(void) { return VisibleFlag ; }
void SetVisibleFlag( const int flag ) { VisibleFlag = flag ; }

// Flag pour inhiber les raccourcis clavier
static int ShortcutsFlag = 1 ;
int GetShortcutsFlag(void) { return ShortcutsFlag ; }
void SetShortcutsFlag( const int flag ) { ShortcutsFlag = flag ; }

// Flag pour inhiber les raccourcis souris
static int MouseShortcutsFlag = 1 ;
int GetMouseShortcutsFlag(void) { return MouseShortcutsFlag  ; }
void SetMouseShortcutsFlag( const int flag ) { MouseShortcutsFlag  = flag ; }

// Flag pour permettre la definition d'icone de connexion
static int IconeFlag = 0 ;
int GetIconeFlag(void) { return IconeFlag ; }
void SetIconeFlag( const int flag ) { IconeFlag = flag ; }

// Nombre d'icones differentes (identifiant commence a 1 dans le fichier .rc)
#ifndef MOD_PERSO
#define NB_ICONES 1
#define IDI_MAINICON_0 1
#define IDC_RESULT 1008
#endif
static int NumberOfIcons = NB_ICONES ;
int GetNumberOfIcons(void) { return NumberOfIcons ; }
void SetNumberOfIcons( const int flag ) { NumberOfIcons = flag ; }

static int IconeNum = 0 ;
int GetIconeNum(void) { return IconeNum ; }
void SetIconeNum( const int num ) { IconeNum = num ; }

// La librairie dans laquelle chercher les icones (fichier defini dans kitty.ini, sinon kitty.dll s'il existe, sinon kitty.exe)
static HINSTANCE hInstIcons =  NULL ;
HINSTANCE GethInstIcons(void) { return hInstIcons ; }
void SethInstIcons( const HINSTANCE h ) { hInstIcons = h ; }

// Fichier contenant les icones à charger
static char * IconFile = NULL ;

// Hauteur de la fenetre pour la fonction WinHeight
static int WinHeight = -1 ;
int GetWinHeight(void) { return WinHeight ; }
void SetWinHeight( const int num ) { WinHeight = num ; }
// Flag pour inhiber le Winrol
static int WinrolFlag = 1 ;
int GetWinrolFlag(void) { return WinrolFlag ; }
void SetWinrolFlag( const int num ) { WinrolFlag  = num ; }

// Password de protection de la configuration (registry)
static char PasswordConf[cstMaxRegLength+2] = "" ; /* filled from the registry "password" value via GetValueData, which writes up to cstMaxRegLength data bytes + NUL */

// Renvoi automatiquement dans le tray (pour les tunnel), fonctionne avec le l'option -send-to-tray
static int AutoSendToTray = 0 ;
int GetAutoSendToTray( void ) { return AutoSendToTray ; }
void SetAutoSendToTray( const int flag ) { AutoSendToTray = flag ; }

// Flag pour ne pas creer les fichiers kitty.ini et kitty.sav
static int NoKittyFileFlag = 0 ;
int GetNoKittyFileFlag(void) { return NoKittyFileFlag ; }
void SetNoKittyFileFlag( const int flag ) { NoKittyFileFlag = flag ; }

// Hauteur de la boite de configuration (visible saved-session rows; 16 = stock fit)
static int ConfigBoxHeight = 16 ;
int GetConfigBoxHeight(void) { return ConfigBoxHeight ; }
void SetConfigBoxHeight( const int num ) { ConfigBoxHeight = num ; }

// Hauteur de la fenetre de la boite de configuration (0=valeur par defaut)
static int ConfigBoxWindowHeight = 0 ;
int GetConfigBoxWindowHeight(void) { return ConfigBoxWindowHeight ; }
void SetConfigBoxWindowHeight( const int num ) { ConfigBoxWindowHeight = num ; }

// Flag pour inhiber la gestion du CTRL+TAB
static int CtrlTabFlag = 1 ;
int GetCtrlTabFlag(void) { return CtrlTabFlag  ; }
void SetCtrlTabFlag( const int flag ) { CtrlTabFlag  = flag ; }

#ifdef MOD_RECONNECT
// Flag pour inhiber le mécanisme de reconnexion automatique
static int AutoreconnectFlag = 1 ;
int GetAutoreconnectFlag( void ) { return AutoreconnectFlag ; }
void SetAutoreconnectFlag( const int flag ) { AutoreconnectFlag = flag ; }
// Delai avant de tenter une reconnexion automatique
static int ReconnectDelay = 5 ;
int GetReconnectDelay(void) { return ReconnectDelay ; }
void SetReconnectDelay( const int flag ) { ReconnectDelay = flag ; }
#endif

// Flag pour inhiber la creation automatique de la session Default Settings
// [ConfigBox] defaultsettings=yes
static int DefaultSettingsFlag = 1 ;
int GetDefaultSettingsFlag(void) { return DefaultSettingsFlag ; }
void SetDefaultSettingsFlag( const int flag ) { DefaultSettingsFlag = flag ; }

// Flag pour inhiber le filtre sur la liste des sessions de la boite de configuration
static int SessionFilterFlag = 1 ;
int GetSessionFilterFlag(void) { return SessionFilterFlag ; }
void SetSessionFilterFlag( const int flag ) { SessionFilterFlag = flag ; }

// Flag pour passer en mode visualiseur d'images
static int ImageViewerFlag = 0 ;
int GetImageViewerFlag(void) { return ImageViewerFlag  ; }
void SetImageViewerFlag( const int flag ) { ImageViewerFlag = flag ; }

// Duree (en secondes) pour switcher l'image de fond d'ecran (<=0 pas de slide)
int ImageSlideDelay = - 1 ;

// Compteur pour l'envoi de anti-idle
int AntiIdleCount = 0 ;
int AntiIdleCountMax = 6 ;
char AntiIdleStr[128] = "" ;  // Ex: " \x08"   => Fait un espace et le retire tout de suite

// Chemin vers le programme cthelper.exe
char * CtHelperPath = NULL ;

// Chemin vers le programme WinSCP
char * WinSCPPath = NULL ;

// Chemin vers le programme pscp.exe
char * PSCPPath = NULL ;

// Repertoire de lancement
char InitialDirectory[4096]="" ;

// Chemin complet des fichiers de configuration kitty.ini et kitty.sav
static char * KittyIniFile = NULL ;
char * GetKittyIniFile(void) { return KittyIniFile ; }
static char * KittySavFile = NULL ;

// Nom de la classe de l'application
char KiTTYClassName[128] = "" ;

// Parametres de l'impression
extern int PrintCharSize ;
extern int PrintMaxLinePerPage ;
extern int PrintMaxCharPerLine ;

extern char puttystr[1024] ;

#ifdef MOD_PROXY
#include "kitty_proxy.h"
#endif

// Handle sur la fenetre principale
HWND MainHwnd ;
HWND GetMainHwnd(void) { return MainHwnd ; }

// Decompte du nombre de fenetres en cours de KiTTY
static int NbWindows = 0 ;

NOTIFYICONDATA TrayIcone ;
#define MYWM_NOTIFYICON		(WM_USER+3)

#define TIMER_INIT 8701
#define TIMER_AUTOCOMMAND 8702
#ifdef MOD_BACKGROUNDIMAGE
#define TIMER_SLIDEBG 8703
#endif
#define TIMER_REDRAW 8704
#define TIMER_BLINKTRAYICON 8706
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


// Procedure de debug
void debug_log( const char *fmt, ... ) {
	char filename[4096]="" ;
	va_list ap;
	FILE *fp ;

	if( (InitialDirectory!=NULL) && (strlen(InitialDirectory)>0) )
		snprintf( filename, sizeof(filename),"%s\\kitty.log",InitialDirectory);
	else strcpy(filename,"kitty.log");

	va_start( ap, fmt ) ;
	//vfprintf( stdout, fmt, ap ) ; // Ecriture a l'ecran
	if( ( fp = fopen( filename, "ab" ) ) != NULL ) {
		vfprintf( fp, fmt, ap ) ; // ecriture dans un fichier
		fclose( fp ) ;
	}
 
	va_end( ap ) ;
}

// Procedure d'affichage d'un message
void debug_msg( const char *fmt, ... ) {
	char buffer[4096]="" ;
	va_list ap;
	va_start( ap, fmt ) ;
	vsprintf( buffer, fmt, ap ) ;
	MessageBox( NULL, buffer, "Debug", MB_OK ) ;
	va_end( ap ) ;
}

char *dupvprintf(const char *fmt, va_list ap) ;
	
// Procedure de recuperation de la valeur d'un flag
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
	// else if( !stricmp( val, "NUMBEROFICONS" ) ) return NumberOfIcons ;	// ==> Remplace par GetNumberOfIcons()
	// else if( !stricmp( val, "ICON" ) ) return IconeFlag ; // ==> Remplace par GetIconeFlag()
	// else if( !stricmp( val, "SESSIONFILTER" ) ) return SessionFilterFlag ;
	return 0 ;
	}

#ifdef MOD_BACKGROUNDIMAGE
	/* Le patch Background image ne marche plus bien sur la version PuTTY 0.61
		- il est en erreur lorsqu'on passe par la config box
		- il est ok lorsqu'on demarrer par -load ou par duplicate session
	   On le desactive dans la config box (fin du fichier WINCFG.C)
	*/
void DisableBackgroundImage( void ) { SetBackgroundImageFlag(0) ; }
#endif

// Procedure de recuperation de la valeur d'une chaine
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

char * kitty_current_dir() { 
	

return NULL ;  /* Ce code est tres specifique et ne marche pas partout */
	/*
	static char cdir[1024]; 
	char * dir = strstr(term->osc_string, ":") ; 
	if(dir) { 
		if( strlen(dir) > 1 ) {
			dir = dir + 1 ;
			if(*dir == '~') {
				if(strlen(conf_get_str_ambi(conf,CONF_username,NULL))>0) { 
					snprintf(cdir, 1024, "\"/home/%s/%s\"", conf_get_str_ambi(conf,CONF_username,NULL), dir + 1); 
					return cdir; 
				}
			} else if(*dir == '/') { 
				snprintf(cdir, 1024, "\"%s\"", dir); 
				return cdir; 
			} 
		} 
	}
	return NULL; 
	*/
} 

// Liste des folder
char **FolderList=NULL ;

int readINI( const char * filename, const char * section, const char * key, char * pStr, size_t pStrSize) ;
int writeINI( const char * filename, const char * section, const char * key, char * pStr) ;
int delINI( const char * filename, const char * section, const char * key ) ;
// Initialise la liste des folders a partir des sessions deja existantes et du fichier kitty.ini
void InitFolderList( void ) {
	char * pst, fList[4096], buffer[4096] ;
	int i ;
	FolderList=(char**)malloc( 1024*sizeof(char*) );
	FolderList[0] = NULL ;
	StringList_Add( FolderList, "Default" ) ;
	//if( GetValueData(HKEY_CURRENT_USER, TEXT(PUTTY_REG_POS), "Folders", fList) == NULL ) return ;
	//if( ReadParameter( "KiTTY", "Folders", fList ) == 0 ) return ;
	ReadParameter( INIT_SECTION, "Folders", fList ) ;
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

		snprintf( buffer, sizeof(buffer), "%s\\\\Sessions", PUTTY_REG_POS );
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
					if( GetValueData(HKEY_CURRENT_USER, nValue, "Folder", fList ) != NULL ) {
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
	
	if( readINI( KittyIniFile, "Folder", "new", buffer, sizeof(buffer) ) ) {
		if( strlen( buffer ) > 0 ) {
			for( i=0; i<strlen(buffer); i++ ) if( buffer[i]==',' ) buffer[i]='\0' ;
			StringList_Add( FolderList, buffer ) ;
			}
		delINI( KittyIniFile, "Folder", "new" ) ;
		}
	
	}

int GetSessionFolderNameInSubDir( const char * session, const char * subdir, char * folder ) {
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

// Recupere le nom du folder associe à une session
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
		snprintf( buffer, sizeof(buffer), "%s\\Sessions\\%s", PUTTY_REG_POS, session ) ;
		if( RegOpenKeyEx( HKEY_CURRENT_USER, buffer, 0, KEY_READ, &hKey) == ERROR_SUCCESS ) {
			DWORD lpType ;
			unsigned char lpData[1024] ;
			DWORD dwDataSize = 1024 ;
			if( RegQueryValueEx( hKey, "Folder", 0, &lpType, lpData, &dwDataSize ) == ERROR_SUCCESS ) {
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
						if( strstr( buffer, "Folder" ) == buffer ) {
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

// Recupere une entree d'une session ( retourne 1 si existe )
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
	snprintf( buffer, sizeof(buffer), "%s\\Sessions\\%s", PUTTY_REG_POS, session ) ;
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
	if( strlen( conf_get_str(conf,CONF_password) ) == 0 ) {
		char buffer[1024] = "", host[1024], termtype[1024] ;
		if( GetSessionField( conf_get_str(conf,CONF_sessionname), conf_get_str(conf,CONF_folder), "Password", buffer ) ) {
			GetSessionField( conf_get_str(conf,CONF_sessionname), conf_get_str(conf,CONF_folder), "HostName", host );
			GetSessionField( conf_get_str(conf,CONF_sessionname), conf_get_str(conf,CONF_folder), "TerminalType", termtype );
			decryptpassword( GetCryptSaltFlag(), buffer, host, termtype ) ;
			MASKPASS(GetCryptSaltFlag(),buffer);
			conf_set_str(conf,CONF_password,buffer) ;
			memset(buffer,0,strlen(buffer) );
			}
		}
	}

int DebugAddPassword( const char*fct, const char*pwd ) ;
void SetPasswordInConfig( const char * password ) {
	int len ;
	char bufpass[1024] ;
	if( (!GetUserPassSSHNoSave())&&(password!=NULL) ) {
		len = strlen( password ) ;
		if( len > 126 ) len = 126 ;
		if( len>0 ) {
			memcpy( bufpass, password, len+1 ) ;
			bufpass[len]='\0' ;
			{ size_t _l; while( (_l=strlen(bufpass))>=2 && ( ((bufpass[_l-1]=='n')&&(bufpass[_l-2]=='\\')) || ((bufpass[_l-1]=='r')&&(bufpass[_l-2]=='\\')) ) ) { bufpass[_l-2]='\0'; bufpass[_l-1]='\0'; } }
			str_rtrim( bufpass, "\n\r\t " ) ;
			DebugAddPassword( "SetPasswordInConfig(before mask)", bufpass ) ;
			MASKPASS(GetCryptSaltFlag(),bufpass) ;
			DebugAddPassword( "SetPasswordInConfig(after mask)", bufpass ) ;
		} else {
			strcpy( bufpass, "" ) ;
		}
		conf_set_str(conf,CONF_password,bufpass);
		memset( bufpass, 0, strlen(bufpass) ) ;
	}
}

void SetUsernameInConfig( const char * username ) {
	int len ;
	if( (!GetUserPassSSHNoSave())&&(username!=NULL) ) {
		len = strlen( username ) ;
		if( len > 126 ) { len = 126 ; }
		char *b = (char*) malloc( len+1 ) ;
		memcpy( (void*)b, (const void*)username, len+1 ) ;
		b[len] = '\0' ;
		conf_set_str(conf,CONF_username,b);
		free(b);
		}
	}

// Sauvegarde la liste des folders
void SaveFolderList( void ) {
	int i = 0 ;
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
		WriteParameter( INIT_SECTION, "Folders", buffer ) ;
	}

// Sauvegarde une cle de registre dans un fichier
void QueryKey( HKEY hMainKey, LPCTSTR lpSubKey, FILE * fp_out ) { 
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
 
    DWORD i,j, retCode; 
 
    TCHAR  achValue[MAX_VALUE_NAME]; 
    DWORD cchValue = MAX_VALUE_NAME; 
	
    char str[4096], b[2] =" " ;
	
	DWORD lpType, dwDataSize = 1024 ;
	char * buffer = NULL ;
	
	// On ouvre la cle
	if( RegOpenKeyEx( hMainKey, TEXT(lpSubKey), 0, KEY_READ, &hKey) != ERROR_SUCCESS ) return ;
 
    // Get the class name and the value count. 
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
 
	//fprintf( fp_out, "\r\n[HKEY_CURRENT_USER\\%s]\r\n" TEXT(lpSubKey) ) ;
	snprintf( str, sizeof(str), "[HKEY_CURRENT_USER\\%s]", TEXT(lpSubKey) ) ;
	if( strlen( PasswordConf ) > 0 ) { cryptstring( GetCryptSaltFlag(), str, PasswordConf ) ; }
	fprintf( fp_out, "\r\n%s\r\n", str ) ;

    // Enumerate the key values. 
    if (cValues) 
    {
        //printf( "\nNumber of values: %d\n", cValues);

        for (i=0, retCode=ERROR_SUCCESS; i<cValues; i++) 
        { 
            cchValue = MAX_VALUE_NAME; 
            achValue[0] = '\0'; 
            retCode = RegEnumValue(hKey, i, 
                achValue, 
                &cchValue, 
                NULL, 
                NULL,
                NULL,
                NULL);
 
            if (retCode == ERROR_SUCCESS ) 
            { 
                //fprintf( fp_out, "\"%s\"=",  achValue ) ;
				unsigned char lpData[1024] ;
				dwDataSize = 1024 ;
				RegQueryValueEx( hKey, TEXT( achValue ), 0, &lpType, lpData, &dwDataSize ) ;
				switch ((int)lpType){
					case REG_BINARY:
							// A FAIRE
						break ;
					case REG_DWORD:
						//sprintf( str, "\"%s\"=dword:%08x", achValue, (unsigned int)*lpData ) ;
						snprintf( str, sizeof(str), "\"%s\"=dword:%08x", achValue, (unsigned int) *((DWORD*)lpData) ) ; // Ca ca marchait bien mais avec une erreur de compilation
						break;
					case REG_EXPAND_SZ:
					case REG_MULTI_SZ:
					case REG_SZ:
						//fprintf( fp_out, "\"" ) ;
						snprintf( str, sizeof(str), "\"%s\"=\"", achValue ) ;
						for( j=0; j<strlen((char*)lpData) ; j++ ) {
							//fprintf( fp_out, "%c", lpData[j] ) ;
							b[0]=lpData[j] ;
							strcat( str, b ) ;
							//if( lpData[j]=='\\' ) fprintf( fp_out, "\\" ) ;
							if( lpData[j]=='\\' ) strcat( str,"\\" ) ;
							}
						//fprintf( fp_out, "\"" ) ;
						strcat( str, "\"" ) ;
						break;
					}
				//fprintf( fp_out, "\r\n");
				if( strlen( PasswordConf ) > 0 ) { cryptstring( GetCryptSaltFlag(), str, PasswordConf ) ; }
				fprintf( fp_out, "%s\r\n", str ) ;
            } 
        }
    }
	
    // Enumerate the subkeys, until RegEnumKeyEx fails.
    if (cSubKeys)
    {
        //printf( "\nNumber of subkeys: %d\n", cSubKeys);

        for (i=0; i<cSubKeys; i++) 
        { 
            cbName = MAX_KEY_LENGTH;
            retCode = RegEnumKeyEx(hKey, i,
                     achKey, 
                     &cbName, 
                     NULL, 
                     NULL, 
                     NULL, 
                     &ftLastWriteTime); 
            if (retCode == ERROR_SUCCESS) 
            {
				buffer = (char*) malloc( strlen( TEXT(lpSubKey) ) + strlen( achKey ) + 3 ) ;
                sprintf( buffer, "%s\\%s", TEXT(lpSubKey), achKey ) ;
				QueryKey( hMainKey, buffer, fp_out ) ;
				free( buffer );				
            }
        }
    } 
 
	RegCloseKey( hKey ) ;
}

// Renomme une Cle de registre
void RegRenameTree( HWND hdlg, HKEY hMainKey, LPCTSTR lpSubKey, LPCTSTR lpDestKey ) { // hdlg boite d'information
	if( RegTestKey( hMainKey, lpDestKey ) ) {
		if( hdlg != NULL ) InfoBoxSetText( hdlg, "Cleaning backup registry" ) ;
		RegDelTree( hMainKey, lpDestKey ) ;
		}
	if( hdlg != NULL ) InfoBoxSetText( hdlg, "Saving registry" ) ;
	kitty_RegCopyTree( hMainKey, lpSubKey, lpDestKey ) ;
	if( hdlg != NULL ) InfoBoxSetText( hdlg, "Preparing local registry" ) ;
	RegDelTree( hMainKey, lpSubKey ) ;
	}

int license_make_with_first( char * license, int length, int modulo, int result ) ;
void license_form( char * license, char sep, int size ) ;
int license_test( char * license, char sep, int modulo, int result ) ;

// Augmente le compteur d'utilisation dans la base de registre
void CountUp( void ) {
	char buffer[4096] = "0", *pst ;
	long int n ;
	int len = 1024 ;
	
	if( ReadParameterN( INIT_SECTION, "KiCount", buffer, sizeof(buffer) ) == 0 ) { strcpy( buffer, "0" ) ; }
	n = atol( buffer ) + 1 ;
	snprintf( buffer, sizeof(buffer), "%ld", n ) ;
	WriteParameter( INIT_SECTION, "KiCount", buffer) ;
	
	if( ReadParameterN( INIT_SECTION, "KiLastUp", buffer, sizeof(buffer) ) == 0 ) { snprintf( buffer, sizeof(buffer), "%ld/", time(0) ) ; }
	buffer[2048]='\0';
	if( (pst=strstr(buffer,"/"))==NULL ) { strcat(buffer,"/") ; pst=buffer+strlen(buffer)-1 ; }
	sprintf( pst+1, "%ld", time(0) ) ;
	WriteParameter( INIT_SECTION, "KiLastUp", buffer) ;
	
	if( GetUserName( buffer, (void*)&len ) ) { 
		strcat( buffer, "@" ) ;
		len = 1024 ;
		if( GetComputerName( buffer+strlen(buffer), (void*)&len ) ) {
			cryptstring( GetCryptSaltFlag(), buffer, MASTER_PASSWORD ) ;
			WriteParameter( INIT_SECTION, "KiLastUH", buffer) ;
			}
		}
		
	if( IniFileFlag != SAVEMODE_DIR ) {
		snprintf( buffer, sizeof(buffer), "%s\\Sessions", PUTTY_REG_POS ) ;
		n = (long int) RegCountKey( HKEY_CURRENT_USER, buffer ) ;
		snprintf( buffer, sizeof(buffer), "%ld", n ) ;
		WriteParameter( INIT_SECTION, "KiSess", buffer) ;
	} else {
		snprintf( buffer, sizeof(buffer), "0 (Not in registry mode)" ) ;
		WriteParameter( INIT_SECTION, ";KiSess", buffer) ;
	}
			
	GetOSInfo( buffer ) ;
	cryptstring( GetCryptSaltFlag(), buffer, MASTER_PASSWORD ) ;
	WriteParameter( INIT_SECTION, "KiVers", buffer) ;
		
	if( GetModuleFileName( NULL, (LPTSTR)buffer, 1024 ) ) 
		if( strlen( buffer ) > 0 ) 
			{ WriteParameter( INIT_SECTION, "KiPath", buffer) ; }
			
	if( ReadParameterN( INIT_SECTION, "KiLic", buffer, sizeof(buffer) ) == 0 ) { 
		strcpy( buffer, "KI67" ) ;
		license_make_with_first( buffer, 25, 97, 0 )  ;
		license_form( buffer, '-', 5 ) ;
		WriteParameter( INIT_SECTION, "KiLic", buffer) ; 
		}
	else if( !license_test( buffer, '-', 97, 0 ) ) {
		strcpy( buffer, "KI67" ) ;
		license_make_with_first( buffer, 25, 97, 0 )  ;
		license_form( buffer, '-', 5 ) ;
		WriteParameter( INIT_SECTION, "KiLic", buffer) ; 
		}
	}

#include "kitty_help.h"
char * GetHelpMessage(void) {
	return default_help_file_content ;
}

// Si le fichier kitty.ini n'existe pas => creation du fichier par defaut
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
void CreateDefaultIniFile( void ) {
	if( !NoKittyFileFlag ) if( !GetReadOnlyFlag() ) {
		if( KittyIniFile==NULL ) return ;
		if( strlen(KittyIniFile)==0 ) return ;
		if( !existfile( KittyIniFile ) ) {
			CreateIniFile( KittyIniFile ) ;
		}
		if( !existfile( KittyIniFile ) ) { MessageBox( NULL, "Unable to create configuration file !", "Error", MB_OK|MB_ICONERROR ) ; }
	}
}

// Ecrit un parametre soit en registre soit dans le fichier de configuration
int WriteParameter( const char * key, const char * name, char * value ) {
	int ret = 1 ;
	char buffer[4096] ;
	if( IniFileFlag == SAVEMODE_DIR ) { 
		if( !GetReadOnlyFlag() ) {
			ret = writeINI( KittyIniFile, key, name, value ) ; 
		}
	} else { 
		snprintf( buffer, sizeof(buffer), "%s\\%s", TEXT(PUTTY_REG_PARENT), key ) ;
		RegTestOrCreate( HKEY_CURRENT_USER, buffer, name, value ) ; 
	}
	return ret ;
}

// Lit un parametre soit dans le fichier de configuration, soit dans le registre.
// Variante bornee: n'ecrit jamais plus de `size` octets (NUL final compris)
// dans `value`; une valeur trop longue est tronquee au lieu de deborder.
int ReadParameterN( const char * key, const char * name, char * value, size_t size ) {
	char buffer[4096] ;
	strcpy( buffer, "" ) ;
	if( IniFileFlag == SAVEMODE_DIR ) {
		/* Portable directory mode must be registry-independent: global KiTTY
		 * parameters such as Folders are read from kitty.ini, not from a stale
		 * HKCU value left by an installed/registry-mode copy. */
		if( !readINI( KittyIniFile, key, name, buffer, sizeof(buffer) ) ) strcpy( buffer, "" ) ;
	} else if( GetValueData( HKEY_CURRENT_USER, TEXT(PUTTY_REG_POS), name, buffer ) == NULL ) {
		if( !readINI( KittyIniFile, key, name, buffer, sizeof(buffer) ) ) {
			strcpy( buffer, "" ) ;
			}
		}
	buffer[4095] = '\0' ;
	if( size == 0 ) return 0 ;
	if( strlen(buffer) >= size ) buffer[size-1] = '\0' ;
	strcpy( value, buffer ) ;
	return strcmp( buffer, "" ) ;
	}

// Compat: ancienne signature non bornee -- le buffer destinataire DOIT faire
// au moins 4096 octets. Preferer ReadParameterN( ..., sizeof(buf) ).
int ReadParameter( const char * key, const char * name, char * value ) {
	return ReadParameterN( key, name, value, 4096 ) ;
	}
	
// Supprime un parametre
int DelParameter( const char * key, const char * name ) {
	char buffer[4096] ;
	if( !GetReadOnlyFlag() ) { delINI( KittyIniFile, key, name ) ; }
	snprintf( buffer, sizeof(buffer), "%s\\%s", TEXT(PUTTY_REG_PARENT), key ) ;
	RegDelValue( HKEY_CURRENT_USER, buffer, (char*)name ) ;
	return 1 ;
	}
	
// Test la configuration (mode file ou registry) et charge le fichier kitty.sav si besoin
void GetSaveMode( void ) {
	char buffer[256] ;
	if( readINI( KittyIniFile, INIT_SECTION, "savemode", buffer, sizeof(buffer) ) ) {
		str_rtrim( buffer, "\n\r \t" ) ;
		if( !stricmp( buffer, "registry" ) ) IniFileFlag = SAVEMODE_REG ;
		else if( !stricmp( buffer, "file" ) ) IniFileFlag = SAVEMODE_FILE ;
		else if( !stricmp( buffer, "dir" ) ) { IniFileFlag = SAVEMODE_DIR ; DirectoryBrowseFlag = 1 ; }
	}
	if( IniFileFlag!=SAVEMODE_DIR ) DirectoryBrowseFlag = 0 ;
}

// Sauvegarde de la cle de registre
void SaveRegistryKeyEx( HKEY hMainKey, LPCTSTR lpSubKey, const char * filename ) {
	FILE * fp_out ;
	//FILE * fp_out1 ;
	char buffer[4096] ;

	if( ( fp_out=fopen( filename, "wb" ) ) == NULL ) return ;
	if( _locking( fileno(fp_out) , LK_LOCK, 10000000L ) == -1 ) { fclose(fp_out); return ; }
	
	strcpy( buffer, "Windows Registry Editor Version 5.00" ) ;

	if( strlen( PasswordConf ) > 0 ) cryptstring( GetCryptSaltFlag(), buffer, PasswordConf ) ;
	fprintf( fp_out, "%s\r\n", buffer ); 

	QueryKey( hMainKey, lpSubKey, fp_out ) ;
	
	_locking( fileno(fp_out) , LK_UNLCK, 10000000L );
	fclose( fp_out ) ;
	}

static int portable_backup_copy_tree( const char *src, const char *dst ) {
	char pattern[4096], s[4096], d[4096] ;
	WIN32_FIND_DATAA fd ;
	HANDLE h ;
	DWORD attr = GetFileAttributesA( src ) ;
	if( attr == INVALID_FILE_ATTRIBUTES ) return 1 ;
	if( !(attr & FILE_ATTRIBUTE_DIRECTORY) ) return CopyFileA( src, dst, FALSE ) ? 1 : 0 ;
	CreateDirectoryA( dst, NULL ) ;
	snprintf( pattern, sizeof(pattern), "%s\\*", src ) ;
	h = FindFirstFileA( pattern, &fd ) ;
	if( h == INVALID_HANDLE_VALUE ) return 1 ;
	do {
		if( !strcmp(fd.cFileName,".") || !strcmp(fd.cFileName,"..") ) continue ;
		snprintf( s, sizeof(s), "%s\\%s", src, fd.cFileName ) ;
		snprintf( d, sizeof(d), "%s\\%s", dst, fd.cFileName ) ;
		if( fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) {
			if( !portable_backup_copy_tree( s, d ) ) { FindClose(h) ; return 0 ; }
		} else if( !CopyFileA( s, d, FALSE ) ) { FindClose(h) ; return 0 ; }
	} while( FindNextFileA( h, &fd ) ) ;
	FindClose( h ) ;
	return 1 ;
}

struct portable_backup_name { char name[MAX_PATH] ; } ;

static int portable_backup_name_cmp_desc( const void *a, const void *b ) {
	const struct portable_backup_name *aa = (const struct portable_backup_name *)a ;
	const struct portable_backup_name *bb = (const struct portable_backup_name *)b ;
	return strcmp( bb->name, aa->name ) ;
}

static void portable_backup_prune( const char *root, int keep ) {
	char pattern[4096], path[4096] ;
	WIN32_FIND_DATAA fd ;
	HANDLE h ;
	struct portable_backup_name backups[128] ;
	int n = 0, i ;
	if( keep < 1 ) keep = 1 ;
	snprintf( pattern, sizeof(pattern), "%s\\kitty-portable-[0-9]*", root ) ;
	h = FindFirstFileA( pattern, &fd ) ;
	if( h == INVALID_HANDLE_VALUE ) return ;
	do {
		if( !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ) continue ;
		if( !strcmp(fd.cFileName,"kitty-portable-latest") ) continue ;
		if( n < (int)(sizeof(backups)/sizeof(backups[0])) ) {
			snprintf( backups[n].name, sizeof(backups[n].name), "%s", fd.cFileName ) ;
			n++ ;
		}
	} while( FindNextFileA( h, &fd ) ) ;
	FindClose( h ) ;
	qsort( backups, n, sizeof(backups[0]), portable_backup_name_cmp_desc ) ;
	for( i = keep ; i < n ; i++ ) {
		snprintf( path, sizeof(path), "%s\\%s", root, backups[i].name ) ;
		DelDir( path ) ;
	}
}

static void portable_backup_write_one( const char *dst ) {
	char src[4096], d[4096] ;
	const char *items[] = { "Sessions", "SshHostKeys", "SshHostCAs", "Commands", "Folders", "Sessions_Commands", "Proxies", "Security", NULL } ;
	const char *files[] = { "PUTTY.RND", "KiTTYState", "Jumplist", NULL } ;
	int i ;
	DelDir( dst ) ;
	CreateDirectoryA( dst, NULL ) ;
	if( KittyIniFile != NULL && strlen(KittyIniFile)>0 && existfile(KittyIniFile) ) {
		snprintf( d, sizeof(d), "%s\\kitty.ini", dst ) ;
		CopyFileA( KittyIniFile, d, FALSE ) ;
	}
	for( i=0 ; items[i]!=NULL ; i++ ) {
		snprintf( src, sizeof(src), "%s\\%s", ConfigDirectory, items[i] ) ;
		snprintf( d, sizeof(d), "%s\\%s", dst, items[i] ) ;
		portable_backup_copy_tree( src, d ) ;
	}
	for( i=0 ; files[i]!=NULL ; i++ ) {
		snprintf( src, sizeof(src), "%s\\%s", ConfigDirectory, files[i] ) ;
		snprintf( d, sizeof(d), "%s\\%s", dst, files[i] ) ;
		if( existfile(src) ) CopyFileA( src, d, FALSE ) ;
	}
}

static void SavePortableDirBackup( void ) {
	char root[4096], latest[4096], stampdir[4096], buffer[64] ;
	int keep = 5 ;
	time_t now ;
	struct tm *tmnow ;
	if( NoKittyFileFlag || ConfigDirectory == NULL || strlen(ConfigDirectory)==0 ) return ;
	if( ReadParameterN( INIT_SECTION, "portablebackupcount", buffer, sizeof(buffer) ) ) keep = atoi( buffer ) ;
	if( keep <= 0 ) return ;
	if( keep > 50 ) keep = 50 ;
	snprintf( root, sizeof(root), "%s\\Backups", ConfigDirectory ) ;
	CreateDirectoryA( root, NULL ) ;
	now = time(NULL) ;
	tmnow = localtime( &now ) ;
	if( tmnow != NULL )
		snprintf( buffer, sizeof(buffer), "%04d%02d%02d-%02d%02d%02d",
			1900+tmnow->tm_year, 1+tmnow->tm_mon, tmnow->tm_mday,
			tmnow->tm_hour, tmnow->tm_min, tmnow->tm_sec ) ;
	else snprintf( buffer, sizeof(buffer), "%ld", (long)now ) ;
	snprintf( stampdir, sizeof(stampdir), "%s\\kitty-portable-%s", root, buffer ) ;
	snprintf( latest, sizeof(latest), "%s\\kitty-portable-latest", root ) ;
	portable_backup_write_one( stampdir ) ;
	portable_backup_write_one( latest ) ;
	portable_backup_prune( root, keep ) ;
}

/* Split a sav path into dir / base / ext (e.g. "C:\x\kittynew.sav" ->
 * "C:\x", "kittynew", ".sav"). */
static void sav_split( const char *savfile, char *dir, size_t dl, char *base, size_t bl, char *ext, size_t el ) {
	const char *slash = strrchr( savfile, '\\' ) ;
	const char *fname = slash ? slash + 1 : savfile ;
	const char *dot = strrchr( fname, '.' ) ;
	snprintf( dir, dl, "%.*s", slash ? (int)(slash - savfile) : 1, slash ? savfile : "." ) ;
	if( dot ) { snprintf( base, bl, "%.*s", (int)(dot - fname), fname ) ; snprintf( ext, el, "%s", dot ) ; }
	else { snprintf( base, bl, "%s", fname ) ; if( el ) ext[0] = '\0' ; }
}

/* Build a fresh timestamped backup path (<dir>\<base>-YYYYMMDD-HHMMSS<ext>), so
 * each backup's FILENAME reflects when it was actually written. */
static void sav_timestamped_path( const char *savfile, char *out, size_t outlen ) {
	char dir[4096], base[256], ext[64], stamp[64] ;
	time_t now ; struct tm *tmnow ;
	sav_split( savfile, dir, sizeof(dir), base, sizeof(base), ext, sizeof(ext) ) ;
	now = time(NULL) ; tmnow = localtime( &now ) ;
	if( tmnow != NULL )
		snprintf( stamp, sizeof(stamp), "%04d%02d%02d-%02d%02d%02d",
			1900+tmnow->tm_year, 1+tmnow->tm_mon, tmnow->tm_mday, tmnow->tm_hour, tmnow->tm_min, tmnow->tm_sec ) ;
	else snprintf( stamp, sizeof(stamp), "%ld", (long)now ) ;
	snprintf( out, outlen, "%s\\%s-%s%s", dir, base, stamp, ext ) ;
}

/* Keep only the `keep` newest <base>-*<ext> timestamped backups next to savfile. */
static void sav_prune( const char *savfile, int keep ) {
	char dir[4096], base[256], ext[64], pattern[4096], path[4096] ;
	struct portable_backup_name backups[256] ;
	WIN32_FIND_DATAA fd ; HANDLE h ; int n = 0, i ;
	if( keep < 0 ) keep = 0 ;
	sav_split( savfile, dir, sizeof(dir), base, sizeof(base), ext, sizeof(ext) ) ;
	snprintf( pattern, sizeof(pattern), "%s\\%s-*%s", dir, base, ext ) ;
	h = FindFirstFileA( pattern, &fd ) ;
	if( h == INVALID_HANDLE_VALUE ) return ;
	do {
		if( fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) continue ;
		if( n < (int)(sizeof(backups)/sizeof(backups[0])) ) { snprintf( backups[n].name, sizeof(backups[n].name), "%s", fd.cFileName ) ; n++ ; }
	} while( FindNextFileA( h, &fd ) ) ;
	FindClose( h ) ;
	qsort( backups, n, sizeof(backups[0]), portable_backup_name_cmp_desc ) ;   /* newest name first */
	for( i = keep ; i < n ; i++ ) { snprintf( path, sizeof(path), "%s\\%s", dir, backups[i].name ) ; unlink( path ) ; }
}

/* Newest <base>-*<ext> backup next to savfile (for the first-run restore).
 * Returns 1 + path in out, else 0. */
static int sav_find_newest( const char *savfile, char *out, size_t outlen ) {
	char dir[4096], base[256], ext[64], pattern[4096], best[MAX_PATH] = "" ;
	WIN32_FIND_DATAA fd ; HANDLE h ;
	sav_split( savfile, dir, sizeof(dir), base, sizeof(base), ext, sizeof(ext) ) ;
	snprintf( pattern, sizeof(pattern), "%s\\%s-*%s", dir, base, ext ) ;
	h = FindFirstFileA( pattern, &fd ) ;
	if( h == INVALID_HANDLE_VALUE ) return 0 ;
	do {
		if( fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) continue ;
		if( best[0] == '\0' || strcmp( fd.cFileName, best ) > 0 ) snprintf( best, sizeof(best), "%s", fd.cFileName ) ;
	} while( FindNextFileA( h, &fd ) ) ;
	FindClose( h ) ;
	if( best[0] == '\0' ) return 0 ;
	snprintf( out, outlen, "%s\\%s", dir, best ) ;
	return 1 ;
}

void SaveRegistryKey( void ) {
	int keep = 5 ; char kb[64] ;
	if( IniFileFlag == SAVEMODE_DIR ) { SavePortableDirBackup() ; return ; }
	if( NoKittyFileFlag || (KittySavFile==NULL) ) return ;
	if( strlen(KittySavFile)==0 ) return ;

	if( GetValueData( HKEY_CURRENT_USER, TEXT(PUTTY_REG_POS), "password", PasswordConf ) == NULL )
		{ strcpy( PasswordConf, "" ) ; }

	if( strlen( PasswordConf ) > 0 )
		{ WriteParameter( INIT_SECTION, "password", PasswordConf ) ; }

	if( ReadParameterN( INIT_SECTION, "savbackupcount", kb, sizeof(kb) ) ) keep = atoi( kb ) ;
	if( keep <= 0 ) return ;              /* savbackupcount=0 disables the backup */
	if( keep > 50 ) keep = 50 ;
	/* Write a FRESH timestamped file now (kittynew-YYYYMMDD-HHMMSS.sav) so its
	 * name matches its content's save time, then keep only the newest `keep`.
	 * (The old scheme wrote a fixed kittynew.sav and renamed the PREVIOUS one to
	 * a now-stamped name - the timestamp then lied about the content's age.) */
	{ char dated[4096] ;
	  sav_timestamped_path( KittySavFile, dated, sizeof(dated) ) ;
	  SaveRegistryKeyEx( HKEY_CURRENT_USER, TEXT(PUTTY_REG_POS), dated ) ;
	  sav_prune( KittySavFile, keep ) ; }
	}

void routine_SaveRegistryKey( void * st ) { SaveRegistryKey() ; }

// Charge la cle de registre
void LoadRegistryKey( HWND hdlg ) { // hdlg est la boite de dialogue d'information de l'avancement (si null pas d'info)
	FILE *fp ;
	HKEY hKey = NULL ;
	char buffer[4096], KeyName[1024] = "", ValueName[1024], *Value ;
	int nb=0 ;
	
	if( KittySavFile==NULL ) return ;
	if( strlen(KittySavFile)==0 ) return ;
	
	if( ( fp = fopen( KittySavFile,"rb" ) ) == NULL ) return ;
	while( fgets( buffer, 4096, fp ) != NULL ) {
		str_rtrim( buffer, "\n\r \t" ) ;
		
		// Test si on a un fichier crypte
		if( nb == 0 ) {
			if( strcmp( buffer, "Windows Registry Editor Version 5.00" ) ) {
				GetAndSendLinePassword( NULL ) ;
				if( GetInputBoxResult() == NULL ) exit(0) ;
				if( strlen( GetInputBoxResult() ) == 0 ) exit(0) ;
				strcpy( PasswordConf, GetInputBoxResult() ) ;
				decryptstring( GetCryptSaltFlag(), buffer, PasswordConf ) ;
				if( strcmp( buffer, "Windows Registry Editor Version 5.00" ) ) {
					MessageBox( NULL, "Wrong password", "Error", MB_OK|MB_ICONERROR ) ;
					exit(1) ;
					}
				if( strlen(PasswordConf) > 0 )
					WriteParameter( INIT_SECTION, "password", PasswordConf ) ;
				}
			}
		nb++ ;
		if( strlen( PasswordConf ) > 0 ) {
			decryptstring( GetCryptSaltFlag(), buffer, PasswordConf ) ;
			}
			
		if( strlen( buffer ) == 0 ) ;
		if( (buffer[0]=='[') && (buffer[strlen(buffer)-1]==']') ) {
			snprintf( KeyName, sizeof(KeyName), "%s", buffer+19 ) ; // +19 pour supprimer [HKEY_CURRENT_USER
			{ size_t _kl=strlen(KeyName); if(_kl>0) KeyName[_kl-1] = '\0' ; }
			if( hKey != NULL ) { RegCloseKey( hKey ) ; hKey = NULL ; }
			if( RegOpenKeyEx( HKEY_CURRENT_USER, TEXT(KeyName), 0, KEY_WRITE, &hKey) != ERROR_SUCCESS ) 
				{
					if( hdlg != NULL ) {
						snprintf( buffer, sizeof(buffer), "Loading %s", KeyName ) ;
						InfoBoxSetText( hdlg, buffer ) ;
						}
					RegCreateKey( HKEY_CURRENT_USER, TEXT(KeyName), &hKey ) ; 
					}
			}
		else {
			if( ( Value = strstr( buffer, "=" ) ) != NULL ) {
				snprintf( ValueName, sizeof(ValueName), "%s", buffer+1 ) ;
				{ int _vni = (int)(Value-buffer-2); if( _vni >= 0 && _vni < (int)sizeof(ValueName) ) ValueName[ _vni ] = '\0' ; }
				Value++;
			if( Value[0] == '\"' ) { // REG_SZ
			  	Value++;
			  	{ size_t _vl=strlen(Value); if(_vl>0) Value[_vl-1] = '\0' ; }
				DelDoubleBackSlash( Value ) ;
			  	RegSetValueEx( hKey, TEXT( ValueName ), 0, REG_SZ, (LPBYTE)Value, strlen(Value)+1 ) ;
			  	}
			else if( strstr(Value,"dword:") == Value ) { //REG_DWORD
				int dwData = 0 ;			  	
				Value = Value + 6 ;
				sscanf( Value, "%08x", (int*)&dwData ) ;
				RegSetValueEx( hKey, TEXT( ValueName ), 0, REG_DWORD, (LPBYTE)&dwData, sizeof(DWORD) ) ;
				}
			else { // erreur
				MessageBox( NULL, "Unknown value type", "Error", MB_OK|MB_ICONERROR ); 
				exit( 1 ) ;
				}
			}
			}
		}
	if( hKey != NULL ) { RegCloseKey( hKey ) ; hKey = NULL ; }
	fclose( fp ) ;
	}

void DelRegistryKey( void ) {
	RegDelTree( HKEY_CURRENT_USER, TEXT(PUTTY_REG_PARENT) ) ;
	}

//extern const int Default_Port ;
//void server_run_routine( const int port, const int timeout ) ;

//extern int PORT ; int main_m1( void ) ;
//void routine_server( void * st ) { main_m1() ; }

typedef void (CALLBACK* LPFNDLLFUNC1)( void ) ;
void routine_server( void * st ) {
	HMODULE lphDLL ;               // Handle to DLL
	LPFNDLLFUNC1 lpfnDllFunc1 ;    // Function pointer
	
	char buffer[MAX_PATH] ; snprintf( buffer, sizeof(buffer), "%s\\kchat.dll", InitialDirectory ) ;
	lphDLL = LoadLibrary( TEXT( buffer ) ) ;
	//lphDLL = LoadLibrary( TEXT("kchat.dll") ) ;
	if( lphDLL == NULL ) {
		MessageBox( MainHwnd, "Unable to load library kchat.dll", "Error", MB_OK|MB_ICONERROR ) ;
		return ;
		}
	if( !( lpfnDllFunc1 = (LPFNDLLFUNC1) GetProcAddress( lphDLL, TEXT("main_m1") ) ) ) {
		MessageBox( NULL, "Unable to load main chat function from library kchat.dll", "Error", MB_OK|MB_ICONERROR  );
		FreeLibrary( lphDLL ) ;
		return ;
		}
	(lpfnDllFunc1) () ;
	FreeLibrary( lphDLL ) ;
	return ;
}

void SendStrToTerminal( const char * str, const int len ) ;

void SendKeyboard( HWND hwnd, const char * buffer ) {
	int i ; 
	if( strlen( buffer) > 0 ) {
		for( i=0; i< strlen( buffer ) ; i++ ) {
			if( buffer[i] == '\n' ) {
				SendMessage(hwnd, WM_KEYDOWN, VK_RETURN, 0) ;
				}
			else if( buffer[i] == '\r' ) {
				}
			else 
				//lpage_send(ldisc, CP_ACP, buffer+i, 1, 1);
				SendMessage(hwnd, WM_CHAR, buffer[i], 0) ;
			if( between_char_delay > 0 ) Sleep( between_char_delay ) ;
			}
		}
	}

/*
SetForegroundWindow(hwnd);
keybd_event(VK_CONTROL, 0x1D, 0, 0);
keybd_event(0x58, 0x47, 0, 0);
keybd_event(0x58, 0x47, KEYEVENTF_KEYUP, 0);
keybd_event(VK_CONTROL, 0x1D, KEYEVENTF_KEYUP, 0);
*/
	
static int keyb_control_flag = 0 ;
static int keyb_shift_flag = 0 ;
static int keyb_win_flag = 0 ;
static int keyb_alt_flag = 0 ;
static int keyb_altgr_flag = 0 ;
	
void SendKeyboardPlus( HWND hwnd, const char * st ) {
	if( strlen(st) <= 0 ) return ;
	int i=0, j=0;
	//int internal_delay = 10 ;
	char *buffer = NULL, stb[6] ;
	if( ( buffer = (char*) malloc( 2*strlen( st ) ) ) != NULL ) {
		buffer[0] = '\0' ;
		do {
		if( st[i] == '\\' ) {
			if( st[i+1] == '\\' ) { buffer[j] = '\\' ; i++ ; j++ ;
			} else if( (st[i+1] == '/') && (i==0) ) { buffer[j] = '/' ; i++ ; j++ ; 
			} else if( st[i+1] == 't' ) { buffer[j] = '\t' ; i++ ; j++ ; 
			} else if( st[i+1] == 'r' ) { buffer[j] = '\r' ; i++ ; j++ ; 
			} else if( st[i+1] == 'h' ) { buffer[j] = 8 ; i++ ; j++ ; 
			} else if( st[i+1] == 'n' ) {
				SendKeyboard( hwnd, buffer ) ; SendKeyboard( hwnd, "\n" ) ;
				Sleep( internal_delay ) ;
				buffer[0] = '\0' ; j = 0 ;
				i++ ;
			} else if( st[i+1] == 'p' ) { 			// \p pause une seconde
				SendKeyboard( hwnd, buffer ) ;
				Sleep(1000);
				buffer[0] = '\0' ; j = 0 ;
				i++ ; 
			} else if( st[i+1] == 's' ) { 			// \s03 pause 3 secondes
				SendKeyboard( hwnd, buffer ) ;
				j = 1 ;
				if( (st[i+2]>='0')&&(st[i+2]<='9')&&(st[i+3]>='0')&&(st[i+3]<='9') ) {
					stb[0]=st[i+2];stb[1]=st[i+3];stb[2]='\0' ;
					j=atoi(stb) ;
					i++ ; i++ ;
				}
				Sleep(j*1000);
				buffer[0] = '\0' ; j = 0 ;
				i++ ; 
			} else if( st[i+1] == 'c' ) { 
				keybd_event(VK_CONTROL, 0x1D, 0, 0) ;
				keybd_event(VK_CANCEL, 0x1D, 0, 0) ;
				keybd_event(VK_CANCEL, 0x1D, KEYEVENTF_KEYUP, 0) ;
				keybd_event(VK_CONTROL, 0x1D, KEYEVENTF_KEYUP, 0) ;
				buffer[0] = '\0' ; j = 0 ;
				i++ ; 
			} else if( st[i+1] == 'k' ) {
				SendKeyboard( hwnd, buffer ) ;
				snprintf( stb, sizeof(stb), "0x%c%c", st[i+2],st[i+3] ) ;
				sscanf( stb, "%x", &j ) ;
				//SendMessage(hwnd, WM_KEYDOWN, j, 0) ;
				if( j==VK_CONTROL ) keyb_control_flag = abs( keyb_control_flag - 1 ) ;
				else if( j==VK_SHIFT ) keyb_shift_flag = abs( keyb_shift_flag - 1 ) ;
				else if( (j==VK_MENU)||(j==VK_LMENU) ) keyb_alt_flag = abs( keyb_alt_flag - 1 ) ;
				else if( j==VK_RMENU ) keyb_altgr_flag = abs( keyb_altgr_flag - 1 ) ;
				else if( (j==VK_RWIN)||(j==VK_LWIN) ) keyb_win_flag = abs( keyb_win_flag - 1 ) ;
				else {
					if( keyb_control_flag || keyb_shift_flag || keyb_win_flag || keyb_alt_flag || keyb_altgr_flag ) {
						SetForegroundWindow(hwnd) ; Sleep( internal_delay ) ;
						if( keyb_control_flag )	keybd_event(VK_CONTROL, 0x1D, 0, 0) ;
						if( keyb_shift_flag )	keybd_event(VK_SHIFT , 0x1D, 0, 0) ;
						if( keyb_alt_flag )	keybd_event(VK_LMENU , 0x1D, 0, 0) ;
						if( keyb_altgr_flag )	keybd_event(VK_RMENU , 0x1D, 0, 0) ;
						if( keyb_win_flag )	keybd_event(VK_LWIN , 0x1D, 0, 0) ;
						keybd_event( j, 0x47, 0, 0); 
						keybd_event( j, 0x47, KEYEVENTF_KEYUP, 0) ;
						if( keyb_win_flag )	keybd_event(VK_LWIN, 0x1D, KEYEVENTF_KEYUP, 0) ;
						if( keyb_altgr_flag )	keybd_event(VK_RMENU, 0x1D, KEYEVENTF_KEYUP, 0) ;
						if( keyb_alt_flag )	keybd_event(VK_LMENU, 0x1D, KEYEVENTF_KEYUP, 0) ;
						if( keyb_shift_flag )	keybd_event(VK_SHIFT, 0x1D, KEYEVENTF_KEYUP, 0) ;
						if( keyb_control_flag )	keybd_event(VK_CONTROL, 0x1D, KEYEVENTF_KEYUP, 0) ;
					} else if( j==VK_ESCAPE ) {
						keybd_event( VK_ESCAPE, 1, 0, 0); 
						keybd_event( VK_ESCAPE, 1, KEYEVENTF_KEYUP, 0) ;
					} else if( (j==VK_END) || (j==VK_HOME) ) {
						keybd_event( j ,0, KEYEVENTF_EXTENDEDKEY, 0 ) ;
						keybd_event( j, 0, KEYEVENTF_EXTENDEDKEY|KEYEVENTF_KEYUP, 0 ) ; 
					} else {
						snprintf( stb, sizeof(stb), "%c", j ) ;
						SendStrToTerminal( stb, 1 ) ;
						//SendMessage(hwnd, WM_CHAR, j, 0) ;
					}
				}
				Sleep( internal_delay ) ;
				buffer[0] = '\0' ; j = 0 ;
				i++ ; i++ ; i++ ;
			} else if( st[i+1] == 'x' ) {
				SendKeyboard( hwnd, buffer ) ;
				snprintf( stb, sizeof(stb), "0x%c%c", st[i+2],st[i+3] ) ;
				sscanf( stb, "%X", &j ) ;
				stb[0]=j ; stb[1] = '\0' ;
				SendStrToTerminal( stb, 1 ) ;
				Sleep( internal_delay ) ;
				buffer[0] = '\0' ; j = 0 ;
				i++ ; i++ ; i++ ;
			} else { buffer[j] = '\\' ; j++ ; 
			}
		} else { buffer[j] = st[i] ; j++ ; }
		buffer[j] = '\0' ;
		i++ ; 
		} while( st[i] != '\0' ) ;
		
		if( strlen( buffer ) > 0 ) {
			if( buffer[strlen(buffer)-1]=='\\' ) { // si la command se termine par \ on n'envoie pas de retour charriot
				buffer[strlen(buffer)-1]='\0' ;
				SendKeyboard( hwnd, buffer ) ;
			} else {
				SendKeyboard( hwnd, buffer ) ;
				if( buffer[strlen(buffer)-1] != '\n' ) // On ajoute un retour charriot au besoin
					SendKeyboard( hwnd, "\n" ) ;
			}
		}
		free( buffer ) ;
	}
}

void SendAutoCommand( HWND hwnd, const char * cmd ) {
	if( strlen( cmd ) > 0 ) {
		/*FILE * fp ;
		if( ( fp = fopen( cmd, "r" ) ) != NULL ){
			char buffer[4096] ;
			while( fgets( buffer, 4096, fp) != NULL ) {
				SendKeyboard( hwnd, buffer ) ;
				}
			SendKeyboard( hwnd, "\n" ) ;
			fclose( fp ) ;
			}*/
		char *buf;
		buf=(char*)malloc( strlen(cmd)+30 ) ;
		strcpy( buf, "Send automatic command" ) ;
		if( debug_flag ) { strcat( buf, ": ") ; strcat( buf, cmd ) ; }
		if( conf_get_int(conf,CONF_protocol) != PROT_TELNET ) debug_logevent( buf ) ; // On logue que si on est pas en telnet (à cause du password envoyé en clair)
		free(buf);
		if( existfile( cmd ) ) { 
			RunScriptFile( hwnd, cmd ) ; 
		} else if( (toupper(cmd[0])=='C')&&(toupper(cmd[1])==':')&&(toupper(cmd[2])=='\\') ) { 
			//MessageBox( NULL, cmd,"Info", MB_OK );
			return ;
		} else { 
			SendKeyboardPlus( hwnd, cmd ) ; 
		}
	} else { 
		if( debug_flag ) debug_logevent( "No automatic command !" ) ; 
	}
}

// Command sender (send a command to all windows)
BOOL CALLBACK SendCommandProc( HWND hwnd, LPARAM lParam ) {
	char buffer[256] ;
	GetClassName( hwnd, buffer, 256 ) ;
	if( !strcmp( buffer, KiTTYClassName ) ) {
		if( hwnd != MainHwnd ) {
			COPYDATASTRUCT data;
			data.dwData = 1 ;
			data.cbData = strlen( (char*)lParam ) + 1 ;
			data.lpData = (char*)lParam ;
			SendMessage( hwnd, WM_COPYDATA, (WPARAM)(HWND)MainHwnd, (LPARAM) (LPVOID)&data ) ;
			NbWindows++ ;
		}
	}
	return TRUE ;
}

int SendCommandAllWindows( HWND hwnd, char * cmd ) {
	NbWindows=0 ;
	if( cmd==NULL ) return 0 ;
	if( strlen(cmd) > 0 ) {
		EnumWindows( SendCommandProc, (LPARAM)cmd ) ;
	}
	return NbWindows ;
}
	
// Gestion de la taille des fenetres de la meme classe
BOOL CALLBACK ResizeWinListProc( HWND hwnd, LPARAM lParam ) {
	char buffer[256] ;
	GetClassName( hwnd, buffer, 256 ) ;
	
	if( !strcmp( buffer, KiTTYClassName ) )
	if( hwnd != MainHwnd ) {
		RECT * rc = (RECT*) lParam ;
		LPARAM pos = MAKELPARAM( rc->left, rc->top ) ;
		LPARAM size = MAKELPARAM( rc->right, rc->bottom ) ;
		//SendNotifyMessage( hwnd, WM_COMMAND, IDM_RESIZE, size ) ;
		//SendNotifyMessage( hwnd, WM_COMMAND, IDM_REPOS, pos ) ;
		PostMessage( hwnd, WM_COMMAND, IDM_REPOS, pos ) ;
		PostMessage( hwnd, WM_COMMAND, IDM_RESIZE, size ) ;
		//PostMessage( hwnd, WM_COMMAND, IDM_RESIZEH, rc->bottom ) ;
		//SetWindowPos( hwnd, 0, 0, 0, rc->right-rc->left+1, rc->bottom-rc->top+1, SWP_NOZORDER|SWP_NOMOVE|SWP_NOREPOSITION|SWP_NOACTIVATE ) ;
		//SetWindowPos( hwnd, 0, 0, 0, 50,50, SWP_NOZORDER|SWP_NOMOVE|SWP_NOREPOSITION|SWP_NOACTIVATE);
		NbWindows++ ;
	}

	return TRUE ;
}

int ResizeWinList( HWND hwnd, int width, int height ) {
	NbWindows=0 ;
	RECT rc;
	GetWindowRect(hwnd, &rc) ;
	rc.right = width ;
	rc.bottom = height ;
	EnumWindows( ResizeWinListProc, (LPARAM)&rc ) ;
	SetForegroundWindow( hwnd ) ;
	return NbWindows ;
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

void set_title( TermWin *tw, const char *title ) { return win_set_title(tw,title,CP_ACP) ; } // Disparue avec la version 0.71
void ManageProtect( HWND hwnd, TermWin *tw, char * title ) {
	HMENU m ;
	if( ( m = GetSystemMenu (hwnd, FALSE) ) != NULL ) {
		DWORD fdwMenu = GetMenuState( m, (UINT) IDM_PROTECT, MF_BYCOMMAND); 
		if (!(fdwMenu & MF_CHECKED)) {
			CheckMenuItem( m, (UINT)IDM_PROTECT, MF_BYCOMMAND|MF_CHECKED ) ;
			ProtectFlag = 1 ;
			set_title(tw, title) ;
		} else {
			CheckMenuItem( m, (UINT)IDM_PROTECT, MF_BYCOMMAND|MF_UNCHECKED ) ;
			ProtectFlag = 0 ;
			set_title(tw, title);
		}
	}
}

// Affiche un menu dans le systeme Tray
void DisplaySystemTrayMenu( HWND hwnd ) {
	HMENU menu ;
	POINT pt;

	menu = CreatePopupMenu () ;
	AppendMenu( menu, MF_ENABLED, IDM_FROMTRAY, "&Restore" ) ;
	AppendMenu( menu, MF_SEPARATOR, 0, 0 ) ;
	AppendMenu( menu, MF_ENABLED, IDM_ABOUT, "&About" ) ;
	AppendMenu( menu, MF_ENABLED, IDM_QUIT, "&Quit" ) ;
		
	SetForegroundWindow( hwnd ) ;
	GetCursorPos (&pt);
	TrackPopupMenu (menu, TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, NULL);
	}
	
// Gere l'envoi dans le System Tray
int ManageToTray( HWND hwnd ) {
	//SendMessage(hwnd, WM_SYSCOMMAND, SC_MINIMIZE, 0);
	//MessageBox( NULL, "To tray", "Tray", MB_OK ) ;
	//SendMessage( hwnd, WM_SETICON, ICON_SMALL, (LPARAM)LoadIcon( hInstIcons, MAKEINTRESOURCE(IDI_MAINICON_0 + IconeNum ) ) );
	//Message MYWM_NOTIFYICON pour faire reapparaitre

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

// Restaure une fenetre envoyee dans le systray (clic sur l'icone tray)
int RestoreFromTray( HWND hwnd ) {
	Shell_NotifyIcon( NIM_DELETE, &TrayIcone ) ;
	ShowWindow( hwnd, SW_SHOW ) ;
	ShowWindow( hwnd, SW_RESTORE ) ;
	SetForegroundWindow( hwnd ) ;
	VisibleFlag = VISIBLE_YES ;
	return 1 ;
	}

// Gere l'option always visible
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
	
// Gere la demande de relance de l'application
void ManageRestart( HWND hwnd ) {
	SendMessage( hwnd, WM_COMMAND, IDM_RESTART, 0 ) ;
}

// Lance une configbox avec les paramètres courants (mais sans hostname)
void del_settings(const char *sessionname);
void RunSessionWithCurrentSettings( HWND hwnd, Conf *conf, const char * host, const char * user, const char * pass, const int port, const char * remotepath ) ;

// Modification de l'icone de l'application
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
		if( mode == SI_INIT ) {
			if( icone!=0 ) IconeNum = icone - 1 ;
			hIcon = LoadIcon( hInstIcons, MAKEINTRESOURCE(IDI_MAINICON_0 + IconeNum ) ) ;
			SendMessage( hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon );
			SendMessage( hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon );
			TrayIcone.hIcon = hIcon ;
		} else {
			if( IconeFlag==0 ) return ;
			if( IconeFlag <= 0 ) { IconeNum = 0 ; 
			} else {
				if( mode == SI_RANDOM ) { 
					SYSTEMTIME st ;
					GetSystemTime( &st ) ;
					IconeNum = ( GetCurrentProcessId() * time( NULL ) ) % NumberOfIcons ; 
				} else { 
					IconeNum++ ; if( IconeNum >= NumberOfIcons ) IconeNum = 0 ; 
				}
			}
			hIcon = LoadIcon( hInstIcons, MAKEINTRESOURCE(IDI_MAINICON_0 + IconeNum ) ) ;
			SendMessage( hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon );	
			SendMessage( hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon );
			TrayIcone.hIcon = hIcon ;
		}
	}
	Shell_NotifyIcon(NIM_MODIFY, &TrayIcone);
}

// Modification de l'icone pour mettre l'icone de perte de connexion
void SetConnBreakIcon( HWND hwnd ) {
#ifdef MOD_PERSO
	HICON hIcon = NULL ;
	hIcon = LoadIcon( hInstIcons, MAKEINTRESOURCE(IDI_NOCON) ) ;
	SendMessage( hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon );	
	SendMessage( hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon );
	TrayIcone.hIcon = hIcon ;
	Shell_NotifyIcon(NIM_MODIFY, &TrayIcone);
#endif
//Pour remettre
//SetNewIcon( hwnd, filename_to_str(conf_get_filename(conf,CONF_iconefile)), 0, SI_INIT ) ;
}

// Envoi d'un fichier de script local
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
    if( ReadParameterN( INIT_SECTION, "scriptfilefilter", buffer, sizeof(buffer) ) ) {
        buffer[4090]='\0';
    } else { 
        strcpy( buffer, "Script files (*.ksh,*.sh)|*.ksh;*.sh|SQL files (*.sql)|*.sql|All files (*.*)|*.*|" ) ;
    }
    if( strlen(buffer)==0 || buffer[strlen(buffer)-1]!='|' ) strcat( buffer, "|" ) ;
    if( OpenFileName( hwnd, filename, "Open file...", buffer ) ) {
        RunScriptFile( hwnd, filename ) ;
    }
}

// Get window coodinates
void GetWindowCoord( HWND hwnd ) {
    RECT rc ;
    GetWindowRect( hwnd, &rc ) ;

    conf_set_int(conf,CONF_xpos,rc.left);
    conf_set_int(conf,CONF_ypos,rc.top);

    conf_set_int(conf,CONF_windowstate,IsZoomed( hwnd ));
}

// Save window coordinates
void SaveWindowCoord( Conf * conf ) {
    char key[1024], session[1024] ;
    if( conf_get_bool(conf,CONF_saveonexit) )
    if( conf_get_str(conf,CONF_sessionname)!= NULL )
    if( strlen( conf_get_str(conf,CONF_sessionname) ) > 0 ) {
        if( IniFileFlag == SAVEMODE_REG ) {
            mungestr( conf_get_str(conf,CONF_sessionname), session ) ;
            snprintf( key, sizeof(key), "%s\\Sessions\\%s", TEXT(PUTTY_REG_POS), session ) ;
            RegTestOrCreateDWORD( HKEY_CURRENT_USER, key, "TermXPos", conf_get_int(conf,CONF_xpos) ) ;
            RegTestOrCreateDWORD( HKEY_CURRENT_USER, key, "TermYPos", conf_get_int(conf,CONF_ypos) ) ;
            RegTestOrCreateDWORD( HKEY_CURRENT_USER, key, "TermWidth", conf_get_int(conf,CONF_width) ) ;
            RegTestOrCreateDWORD( HKEY_CURRENT_USER, key, "TermHeight", conf_get_int(conf,CONF_height) ) ;
            RegTestOrCreateDWORD( HKEY_CURRENT_USER, key, "WindowState", conf_get_int(conf,CONF_windowstate) ) ;
            RegTestOrCreateDWORD( HKEY_CURRENT_USER, key, "TransparencyValue", conf_get_int(conf,CONF_transparencynumber) ) ;
        } else { 
            int xpos=conf_get_int(conf,CONF_xpos)
                , ypos=conf_get_int(conf,CONF_ypos)
                , width=conf_get_int(conf,CONF_width)
                , height=conf_get_int(conf,CONF_height)
                , windowstate=conf_get_int(conf,CONF_windowstate)
                , transparency=conf_get_int(conf,CONF_transparencynumber);
            load_settings( conf_get_str(conf,CONF_sessionname), conf ) ;
            conf_set_int(conf,CONF_xpos,xpos) ; 
            conf_set_int(conf,CONF_ypos,ypos) ; 
            conf_set_int(conf,CONF_width,width) ;
            conf_set_int(conf,CONF_height,height) ;
            conf_set_int(conf,CONF_windowstate,windowstate) ; 
            conf_set_int(conf,CONF_transparencynumber,transparency) ; 
            save_settings( conf_get_str(conf,CONF_sessionname), conf ) ;
        }
    }
}

// Gestion de la fonction winroll
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
BOOL load_bg_bmp() ;
void clean_bg( void ) ;
void RedrawBackground( HWND hwnd ) ;
#endif

void RefreshBackground( HWND hwnd ) {
#ifdef MOD_BACKGROUNDIMAGE
	if( GetBackgroundImageFlag() ) RedrawBackground( hwnd ) ;
	else
#endif
	InvalidateRect( hwnd, NULL, true ) ;
}

#ifdef MOD_BACKGROUNDIMAGE
/* Changement du fond d'ecran */
int GetExt( const char * filename, char * ext, size_t extsz) {
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

// Demarre le timer d'autocommand a la connexion
void CreateTimerInit( void ) {
	SetTimer(MainHwnd, TIMER_INIT, init_delay, NULL) ; 
	}

// Positionne le repertoire ou se trouve la configuration 
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
	
void GotoInitialDirectory( void ) { chdir( InitialDirectory ) ; }
void GotoConfigDirectory( void ) { if( ConfigDirectory!=NULL ) chdir( ConfigDirectory ) ; }

/* The User-Command special menu (ReadSpecialMenu / InitSpecialMenu /
 * ManageSpecialCommand) lives in kitty_specialmenu.c. */

BOOL CALLBACK EnumWindowsProc( HWND hwnd, LPARAM lParam ) {
	char buffer[256] ;
	GetClassName( hwnd, buffer, 256 ) ;
	
	if( (!strcmp( buffer, appname )) || (!strcmp( buffer, "PuTTYConfigBox" )) ) {
		NbWindows++ ;
	}
	
	return TRUE ;
}

// Decompte le nombre de fenetre de la meme classe que KiTTY
int WindowsCount( HWND hwnd ) {
	char buffer[256] ;
	NbWindows = 0 ;
	
	if( GetClassName( hwnd, buffer, 256 ) == 0 ) {
		NbWindows = 1 ;
	} else {
		if( !strcmp( buffer, "" ) ) {
			NbWindows = 1 ;
		}
	}

	EnumWindows( EnumWindowsProc, 0 ) ;
	return NbWindows ;
}

	
// Gestion de la fenetre d'affichage des portforward
// Mettre la liste des port forward dans le presse-papier et l'afficher a l'ecran
// [C] en Listen dans le process courant, [X] en listen dans un autre process, [-] absent
DWORD (WINAPI *pGetExtendedTcpTable)(
  PVOID pTcpTable,
  PDWORD pdwSize,
  BOOL bOrder,
  ULONG ulAf,
  TCP_TABLE_CLASS TableClass,
  ULONG Reserved
) ;

int GetPortFwdState( const int port, const DWORD pid ) {
	int result = -1 ;
	MIB_TCPTABLE_OWNER_PID *pTCPInfo ;
	MIB_TCPROW_OWNER_PID *owner ;
	DWORD size ;
	DWORD dwResult ;
	DWORD dwLoop ;
	
	HMODULE hLib = LoadLibrary( "iphlpapi.dll" );

	if( hLib ) {
		pGetExtendedTcpTable = (DWORD (WINAPI *)(PVOID,PDWORD,BOOL,ULONG,TCP_TABLE_CLASS,ULONG)) 
		GetProcAddress(hLib, "GetExtendedTcpTable") ;
		dwResult = pGetExtendedTcpTable(NULL, &size, 0, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0) ;
		pTCPInfo = (MIB_TCPTABLE_OWNER_PID*)malloc(size) ;
		dwResult = pGetExtendedTcpTable(pTCPInfo, &size, 0, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0) ;
		if( pGetExtendedTcpTable && (dwResult == NO_ERROR) ) {
			if( pTCPInfo->dwNumEntries > 0 ) {
				for (dwLoop = 0; dwLoop < pTCPInfo->dwNumEntries; dwLoop++) {
					owner = &pTCPInfo->table[dwLoop];
					if ( owner->dwState == MIB_TCP_STATE_LISTEN ) {
						if( ntohs(owner->dwLocalPort) == port ) {
							if( pid == owner->dwOwningPid ) { 
								result = 0 ; 
							} else { 
								result = owner->dwOwningPid ; 
							}
							break;
						}
					}
				}
			}
		}
		
		free(pTCPInfo) ;
		FreeLibrary( hLib ) ;
	}
	return result ;
}

int ShowPortfwd( HWND hwnd, Conf * conf ) {
	char pf[2100]="" ;
	char *key, *val;
	for (val = conf_get_str_strs(conf, CONF_portfwd, NULL, &key) ;
		val != NULL;
		val = conf_get_str_strs(conf, CONF_portfwd, key, &key)) {
		char *p;
		if (( key[0]=='R' ) || ( key[1]=='R' )) {
			p = dupprintf("[-] %s \t\t<-- \t%s\n", (key[1]=='R')? key+2 : key+1,val) ;
		} else if (( key[0]=='L' ) || ( key[1]=='L' )) {
			char *key_pos = (key[1]=='L')? key+2 : key+1 ;
			int res ;
			switch( res=GetPortFwdState( atoi(key_pos), GetCurrentProcessId() ) ) {
				case -1:
					p = dupprintf("[-] %s \t\t--> \t%s\n", key_pos,val) ;
					break ;
				case 0:
					p = dupprintf("[C] %s \t\t--> \t%s\n", key_pos,val) ;
					break ;
				default:
					p = dupprintf("[X] %s(%u)\t--> \t%s\n", key_pos,(unsigned int)res,val) ;
			}
		} else if (( key[0]=='D' ) || ( key[1]=='D' )) {
			p = dupprintf("D%s\t\n", key+1) ;
		} else {
			p = dupprintf("%s\t%s\n", key, val) ;
		}
		if( (strlen(pf)+strlen(p))<2000 ) {
			strcat( pf, p ) ;
			sfree(p) ;
		} else {
			strcat( pf, "...\n" ) ;
			break ;
		}
	}
	/*
	MIB_TCPTABLE_OWNER_PID *pTCPInfo;
	MIB_TCPROW_OWNER_PID *owner;
	DWORD size;
	DWORD dwResult;
	DWORD dwLoop;

	HMODULE hLib = LoadLibrary( "iphlpapi.dll" );

	if( hLib ) {
		pGetExtendedTcpTable = (DWORD (WINAPI *)(PVOID,PDWORD,BOOL,ULONG,TCP_TABLE_CLASS,ULONG))
		GetProcAddress(hLib, "GetExtendedTcpTable");
	}
	dwResult = pGetExtendedTcpTable(NULL, &size, 0, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0);
	pTCPInfo = (MIB_TCPTABLE_OWNER_PID*)malloc(size);
	dwResult = pGetExtendedTcpTable(pTCPInfo, &size, 0, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0);

	for (val = conf_get_str_strs(conf, CONF_portfwd, NULL, &key);
		val != NULL;
		val = conf_get_str_strs(conf, CONF_portfwd, key, &key)) {
		char *p;
		
		if (( key[0]=='R' ) || ( key[1]=='R' )) {
			p = dupprintf("[-] %s \t\t<-- \t%s\n", (key[1]=='R')? key+2 : key+1,val);
		} else if (( key[0]=='L' ) || ( key[1]=='L' )) {
			char *key_pos = (key[1]=='L')? key+2 : key+1;
			if( pGetExtendedTcpTable && (dwResult == NO_ERROR) ) {
				int found=0 ;
				if( pTCPInfo->dwNumEntries > 0 ) {
					for (dwLoop = 0; dwLoop < pTCPInfo->dwNumEntries; dwLoop++) {
						owner = &pTCPInfo->table[dwLoop];
						if ( owner->dwState == MIB_TCP_STATE_LISTEN ) {
							if( ntohs(owner->dwLocalPort) == atoi(key_pos) ) {
								if( GetCurrentProcessId() == owner->dwOwningPid ) p = dupprintf("[C] %s \t\t--> \t%s\n", key_pos,val);
								else p = dupprintf("[X] %s(%u)\t--> \t%s\n", key_pos,(unsigned int)owner->dwOwningPid,val) ;
								found=1;
								break;
							}
						}
					}
				}
				if( !found ) { p = dupprintf("[-] %s \t\t--> \t%s\n", key_pos,val); }
			} else {
				p = dupprintf("[-] %s \t\t--> \t%s\n", key_pos,val);
			}
		} else if (( key[0]=='D' ) || ( key[1]=='D' )) {
			p = dupprintf("D%s\t\n", key+1) ;
		} else {
			p = dupprintf("%s\t%s\n", key, val) ;
		}
		
		strcat( pf, p ) ;
		sfree(p);
	}
	
	if( hLib ) { FreeLibrary( hLib ) ; }
	*/
	strcat( pf, "\n[C] Listening in the current process\n[X] Listening in another process\n[-] No Listening\n" );
	MessageBox( NULL, pf, "Port forwarding", MB_OK ) ;
	return SetTextToClipboard( pf ) ;
}
	
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
	if( SaveFileName( hwnd, filename, "Save file...", buffer ) ) {
		save_open_settings_forced( filename, conf ) ;
		}
	}

// Procedures de generation du dump "memoire" (/savedump)
#ifdef MOD_SAVEDUMP
#include "kitty_savedump.c"
#endif

int InternalCommand( HWND hwnd, char * st ) {
	char buffer[4096] ;
	if( strstr( st, "/message " ) == st ) { 
		MessageBox( hwnd, st+9, "Info", MB_OK ) ; 
		return 1 ; 
	} else if( !strcmp( st, "/copytoputty" ) ) {
		RegDelTree (HKEY_CURRENT_USER, "Software\\SimonTatham\\PuTTY\\Sessions" ) ;
		snprintf( buffer, sizeof(buffer), "%s\\Sessions", PUTTY_REG_POS ) ;
		kitty_RegCopyTree( HKEY_CURRENT_USER, buffer, "Software\\SimonTatham\\PuTTY\\Sessions" ) ;
		snprintf( buffer, sizeof(buffer), "%s\\SshHostKeys", PUTTY_REG_POS ) ;
		kitty_RegCopyTree( HKEY_CURRENT_USER, buffer, "Software\\SimonTatham\\PuTTY\\SshHostKeys" ) ;
		RegCleanPuTTY() ;
		return 1 ;
	} else if( !strcmp( st, "/copytokitty" ) ) {
		kitty_RegCopyTree( HKEY_CURRENT_USER, "Software\\SimonTatham\\PuTTY", PUTTY_REG_POS ) ;
		return 1 ;
	} else if( !strcmp( st, "/backgroundimage" ) ) { 
		SetBackgroundImageFlag( abs( GetBackgroundImageFlag() - 1 ) ) ; 
		return 1 ; 
	} else if( !strcmp( st, "/debug" ) ) { 
		debug_flag = abs( debug_flag - 1 ) ; 
		return 1 ;
	} else if( !strcmp( st, "/hyperlink" ) ) { 
		HyperlinkFlag = abs( HyperlinkFlag - 1 ) ; 
		return 1 ;
	} else if( !strcmp( st, "/urlregex" ) ) { 
		char b[1024] ;
		snprintf(b,sizeof(b),"%d: %s",conf_get_int(conf,CONF_url_defregex),conf_get_str(conf,CONF_url_regex));
		MessageBox( NULL, b, "URL regex", MB_OK ) ; return 1 ; 
	} else if( !strcmp( st, "/save" ) ) { 
		SaveCurrentSetting(hwnd);
		return 1 ; 
#ifdef MOD_SAVEDUMP
	} else if( !strcmp( st, "/savedump" ) ) { SaveDump() ; return 1 ; 
#endif
	} else if( !strcmp( st, "/screenshot" ) ) { 
		char screenShotFile[1024] ;
		snprintf( screenShotFile, sizeof(screenShotFile), "%s\\screenshot-%d-%ld.jpg", InitialDirectory, getpid(), time(0) );
		screenCaptureClientRect( GetParent(hwnd), screenShotFile, 100 ) ;
		//screenCaptureWinRect( GetParent(hwnd), screenShotFile, 100 ) ;
		//screenCaptureAll( screenShotFile, 100 ) ;
		//MakeScreenShot() ; 
		return 1 ; 
	} else if( !strcmp( st, "/fileassoc" ) ) { 
		CreateFileAssoc() ; 
		return 1 ; 
	} else if( !strcmp( st, "/savereg" ) ) {
		chdir( InitialDirectory ) ; 
		SaveRegistryKey() ; 
		return 1 ; 
	} else if( !strcmp( st, "/savesessions" ) ) { 
		chdir( InitialDirectory ) ; 
		snprintf( buffer, sizeof(buffer), "%s\\Sessions", PUTTY_REG_POS ) ;
		SaveRegistryKeyEx( HKEY_CURRENT_USER, buffer, "kitty.ses" ) ; 
		return 1 ; 
	} else if( !strcmp( st, "/loadinitscript" ) ) { 
		ReadInitScript( NULL ) ; 
		return 1 ; 
	} else if( strstr( st, "/loadinitscript " ) == st ) { 
		ReadInitScript( st+16 ) ; 
		return 1 ; 
	} else if( !strcmp( st, "/loadreg" ) ) { 
		chdir( InitialDirectory ) ; 
		LoadRegistryKey(NULL) ; 
		return 1 ; 
	} else if( !strcmp( st, "/delreg" ) ) { 
		RegDelTree (HKEY_CURRENT_USER, TEXT(PUTTY_REG_PARENT)) ; 
		return 1 ; 
	} else if( strstr( st, "/delfolder " ) == st ) { 
		StringList_Del( FolderList, st+11 ) ; 
		return 1 ; 
	} else if( !strcmp( st, "/noshortcuts" ) ) { 
		ShortcutsFlag = 0 ; 
		return 1 ; 
	} else if( !strcmp( st, "/nomouseshortcuts" ) ) { 
		MouseShortcutsFlag = 0 ; 
		return 1 ; 
	} else if( !strcmp( st, "/icon" ) ) { 
		//sprintf( buffer, "%d / %d = %d", IconeNum, NB_ICONES, NumberOfIcons );
		//MessageBox( hwnd, buffer, "info", MB_OK ) ;
		IconeFlag = abs( IconeFlag - 1 ) ; conf_set_int(conf,CONF_icone,IconeNum) ; return 1 ; 
	} else if( !strcmp( st, "/savemode" ) ) {
		//IniFileFlag = abs( IniFileFlag - 1 ) ;
		IniFileFlag++ ; if( IniFileFlag>SAVEMODE_DIR ) IniFileFlag = 0 ;
		if( IniFileFlag == SAVEMODE_REG )  {
			delINI( KittyIniFile, INIT_SECTION, "savemode" ) ;
			MessageBox( NULL, "Savemode is \"registry\"", "Info", MB_OK ) ;
		} else if( IniFileFlag == SAVEMODE_FILE ) {
			if(!NoKittyFileFlag) writeINI( KittyIniFile, INIT_SECTION, "savemode", "file" ) ;
			MessageBox( NULL, "Savemode is \"file\"", "Info", MB_OK ) ;
		} else if( IniFileFlag == SAVEMODE_DIR ) {
			delINI( KittyIniFile, INIT_SECTION, "savemode" ) ;
			MessageBox( NULL, "Savemode is \"dir\"", "Info", MB_OK ) ;
		}
		return 1 ;
	} else if( !strcmp( st, "/init" ) ) {
		char buffer[4096] ;
		snprintf( buffer, sizeof(buffer),"ConfigDirectory=%s\nIniFileFlag=%d\nDirectoryBrowseFlag=%d\nInitialDirectory=%s\nKittyIniFile=%s\nKittySavFile=%s\nKiTTYClassName=%s\n"
			,ConfigDirectory,IniFileFlag,DirectoryBrowseFlag,InitialDirectory,KittyIniFile,KittySavFile,KiTTYClassName ) ;
		MessageBox(hwnd,buffer,"Configuration infomations",MB_OK);
		return 1 ; 
	} else if( !strcmp( st, "/transparency" ) ) {
#ifndef MOD_NOTRANSPARENCY
		if( (conf_get_int(conf,CONF_transparencynumber) == -1) || (TransparencyFlag == 0 ) ) {
			TransparencyFlag = 1 ;
			SetWindowLongPtr(MainHwnd, GWL_EXSTYLE, GetWindowLong(MainHwnd, GWL_EXSTYLE) | WS_EX_LAYERED ) ;
			SetWindowPos( MainHwnd, 0, 0, 0, 0, 0, SWP_FRAMECHANGED|SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER ) ;
			if( conf_get_int(conf,CONF_transparencynumber) == -1 ) conf_set_int(conf,CONF_transparencynumber,0) ; 
			SetTransparency( MainHwnd, 255-conf_get_int(conf,CONF_transparencynumber) ) ;
			SetForegroundWindow( hwnd ) ;
		} else {
			TransparencyFlag = 0 ;
			SetTransparency( MainHwnd, 255 ) ;
			SetWindowLongPtr(MainHwnd, GWL_EXSTYLE, GetWindowLong(hwnd, GWL_EXSTYLE) & ~WS_EX_LAYERED ) ;
			RedrawWindow(MainHwnd, NULL, NULL, RDW_ERASE | RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
			SetWindowPos( MainHwnd, 0, 0, 0, 0, 0, SWP_FRAMECHANGED|SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER ) ;
			SetForegroundWindow( hwnd ) ;
		}
#endif
		return 1 ;
	} else if( !strcmp( st, "/bcdelay" ) ) {
		between_char_delay=3 ;
		return 1 ; 
	} else if( strstr( st, "/bcdelay " ) == st ) { 
		between_char_delay=atoi( st+9 ) ; 
		return 1 ; 
	} else if( strstr( st, "/title " ) == st ) { 
		set_title( NULL, st+7 ) ; 
		return 1 ; 
	} else if( !strcmp( st, "/session" ) ) {
		if( strlen( conf_get_str(conf,CONF_sessionname) ) > 0 ) {
			char buffer[1024] ;
			snprintf( buffer, sizeof(buffer), "Your session name is\n-%s-", conf_get_str(conf,CONF_sessionname) ) ;
			MessageBox( hwnd, buffer, "Session name", MB_OK|MB_ICONWARNING ) ;
		} else
			MessageBox( hwnd, "No session name.", "Session name", MB_OK|MB_ICONWARNING ) ;
		return 1 ;
	} else if( !strcmp( st, "/passwd" ) && debug_flag ) {
		if( strlen( conf_get_str(conf,CONF_password) ) > 0 ) {
			char bufpass[4096], buffer[4096] ;
			strcpy( bufpass, conf_get_str(conf,CONF_password) ) ;
			/* plaintext at runtime; do NOT MASKPASS */
			snprintf( buffer, sizeof(buffer), "Your password is\n-%s-", bufpass ) ;
			SetTextToClipboard( bufpass ) ;
			memset(bufpass,0,strlen(bufpass));
			MessageBox( hwnd, buffer, "Password", MB_OK|MB_ICONWARNING ) ;
			memset(buffer,0,strlen(buffer));
		} else
			MessageBox( hwnd, "No password.", "Password", MB_OK|MB_ICONWARNING ) ;
		return 1 ;
	} else if( !strcmp( st, "/configpassword" ) ) {
		strcpy( PasswordConf, "" ) ;
		RegDelValue( HKEY_CURRENT_USER, TEXT(PUTTY_REG_POS), "password" ) ;
		delINI( KittyIniFile, INIT_SECTION, "password" ) ;
		SaveRegistryKey() ;
		MessageBox( NULL, "At next launch,\ndon't forget to check your configuration save mode\n(file or registry ?)", "Info", MB_OK );
		return 1 ;
	} else if( strstr( st, "/configpassword " ) == st ) {
		strcpy( PasswordConf, st+16 ) ;
		if( strlen(PasswordConf) > 0 ) {
			strcpy( buffer, PasswordConf ) ;
			WriteParameter( INIT_SECTION, "password", PasswordConf ) ;
			SaveRegistryKey() ;
			cryptstring( GetCryptSaltFlag(), buffer, MASTER_PASSWORD ) ;
			// On passe automatiquement en mode de sauvegarde par fichier
			if(!NoKittyFileFlag) writeINI( KittyIniFile, INIT_SECTION, "savemode", "file" ) ;
			IniFileFlag = SAVEMODE_FILE ;
			if(!NoKittyFileFlag) writeINI( KittyIniFile, INIT_SECTION, "password", buffer ) ;
		}
		return 1 ;
	} else if( !strcmp( st, "/-configpassword" ) ) {
		if( ReadParameterN( INIT_SECTION, "password", buffer, sizeof(buffer) ) ) {
			if( decryptstring( GetCryptSaltFlag(), buffer, MASTER_PASSWORD ) ) {
				MessageBox( hwnd, buffer, "Your password is ...", MB_OK|MB_ICONWARNING ) ;
			}
		}
		return 1 ;
	} else if( !strcmp( st, "/shortcuts" ) ) {
		InitShortcuts() ; 
		return 1 ;
	} else if( !strcmp( st, "/switchcrypt" ) ) {
		SwitchCryptFlag() ;
		return 1 ;
	} else if( !strcmp( st, "/redraw" ) ) {
		InvalidateRect( MainHwnd, NULL, TRUE ) ;
		return 1 ;
	} else if( !strcmp( st, "/refresh" ) ) {
		RefreshBackground( MainHwnd ) ;
		return 1 ;
	} else if( strstr( st, "/PrintCharSize " ) == st ) {
		PrintCharSize=atoi( st+15 ) ;
		return 1 ;
	} else if( strstr( st, "/PrintMaxLinePerPage " ) == st ) { 
		PrintMaxLinePerPage=atoi( st+21 ) ;
		return 1 ;
	} else if( strstr( st, "/PrintMaxCharPerLine " ) == st ) {
		PrintMaxCharPerLine=atoi( st+21 ) ;
		return 1 ;
#ifdef MOD_LAUNCHER
	} else if( !strcmp( st, "/initlauncher" ) ) {
		InitLauncherRegistry() ;
		return 1 ; 
#endif
	} else if( !strcmp( st, "/winroll" ) ) { 
		WinrolFlag = abs(WinrolFlag-1) ;
		return 1 ;
	} else if( strstr( st, "/command " ) == st ) {
		SendCommandAllWindows( hwnd, st+9 ) ;
		return 1 ;
	} else if( !strcmp( st, "/sizeall" ) ) {
		ResizeWinList( hwnd, conf_get_int(conf,CONF_width), conf_get_int(conf,CONF_height) ) ; 
		return 1 ;
#ifdef MOD_ZMODEM
	} else if( !strcmp( st, "/zmodem" ) ) {
		SetZModemFlag( abs(GetZModemFlag()-1) ) ; 
#endif
	}
	return 0 ;
}



	
// Appel d'une DLL
/*
typedef int (CALLBACK* LPFNDLLFUNC1)(int,char**); 
int calldll( HWND hwnd, char * filename, char * functionname ) {
	int return_code = 0 ;
	char buffer[1024] ;
	HMODULE lphDLL ;               // Handle to DLL
	LPFNDLLFUNC1 lpfnDllFunc1 ;    // Function pointer
	
	lphDLL = LoadLibrary( TEXT(filename) ) ;
	if( lphDLL == NULL ) {
		//print_error( "Unable to load library %s\n", filename ) ;
		snprintf( buffer, sizeof(buffer), "Unable to load library %s\n", filename ) ;
		MessageBox( hwnd, buffer, "Error" , MB_OK|MB_ICONERROR ) ;
		return -1 ;
		}
		
	if( !( lpfnDllFunc1 = (LPFNDLLFUNC1) GetProcAddress( lphDLL, TEXT(functionname) ) ) ) {
		//print_error( "Unable to load function %s from library %s (%d)\n", functionname, filename, GetLastError() );
		snprintf( buffer, sizeof(buffer),"Unable to load function %s from library %s (%d)\n", functionname, filename, (int)GetLastError() ) ;
		MessageBox( hwnd, buffer, "Error" , MB_OK|MB_ICONERROR ) ;
		FreeLibrary( lphDLL ) ;
		return -1 ;
		}
	
	char **tab ;
	tab=(char**)malloc( 10*sizeof(char* ) ) ;
	int i ;
	for(i=0;i<10;i++) tab[i]=(char*)malloc(256) ;
	strcpy( tab[0], "pscp.exe" ) ; 
	strcpy( tab[1], "-2" ) ;
	strcpy( tab[2], "-scp" ) ;
	strcpy( tab[3], "c:\\tmp\\putty.exe" ) ;
	strcpy( tab[4], "xxxxxx@xxxxxx.xxx.xx:." ) ;
	int tabn = 5 ;
		
	return_code = (lpfnDllFunc1) ( tabn, tab ) ;
	
	for(i=0;i<10;i++) free(tab[i]) ;
	free(tab);
	
	FreeLibrary( lphDLL ) ;
	
	return return_code ;
	}
*/

// Gestion du script au lancement
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
	
void ReadAutoCommandFromFile( const char * filename ) {
	FILE *fp ;
	long l;
	int n;
	char *pst, * buffer = NULL ;
	if( existfile( filename ) ) {
		l=filesize(filename) ;
		buffer=(char*)malloc(5*l+1);
		buffer[0]='\0' ;
		pst = buffer ;
		if( ( fp = fopen( filename,"rb") ) != NULL ) {
			while( fgets( pst, 1024, fp ) != NULL ) {
				pst = buffer + strlen(buffer) ;
			}
			fclose( fp ) ;
		}
	}
	if( buffer == NULL ) return ;
	while( (n=poss("\r",buffer))>0 ) { del(buffer,n,1) ; }
	str_rtrim( buffer, "\n" ) ;
	while( (n=poss("\n",buffer))>0 ) { buffer[n-1]='n' ; insert(buffer,"\\",n) ; }
	conf_set_str(conf, CONF_autocommand, buffer );
	free(buffer);
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
				bcrypt_string_base64( ScriptFileContent, buffer, l, MASTER_PASSWORD, 0 ) ;
				if( IniFileFlag==SAVEMODE_REG ) {
					//WriteParameter( INIT_SECTION, "KiCrSt", buffer ) ;
				}
				conf_set_str(conf, CONF_scriptfilecontent, buffer );
			}
			if( buffer!=NULL ) { free(buffer); buffer=NULL; }
		} else {
			if( (buffer=(char*)malloc(strlen(name)+1))!=NULL ) {
				strcpy( buffer, name ) ;
				l = decryptstring( GetCryptSaltFlag(), buffer, MASTER_PASSWORD ) ;
				if( ScriptFileContent!= NULL ) free( ScriptFileContent ) ;
				ScriptFileContent = (char*) malloc( l + 1 ) ;
				memcpy( ScriptFileContent, buffer, l ) ;
				free(buffer);buffer=NULL;
			}
		}
	}
}


#include "kitty_launcher.c"

// Creer une arborescence de repertoire à partir du registre
int MakeDirTree( const char * Directory, const char * s, const char * sd ) {
	char buffer[MAX_VALUE_NAME], fullpath[MAX_VALUE_NAME] ;
	HKEY hKey;
	int retCode, i ;
	unsigned char lpData[1024] ;
	DWORD lpType, dwDataSize = 1024, cchValue = MAX_VALUE_NAME ;
	FILE * fp ;

	TCHAR achClass[MAX_PATH] = TEXT(""), achKey[MAX_KEY_LENGTH], achValue[MAX_VALUE_NAME] ;
	DWORD cchClassName = MAX_PATH, cSubKeys=0, cbMaxSubKey, cchMaxClass, cValues, cchMaxValue, cbMaxValueData, cbSecurityDescriptor, cbName;
	FILETIME ftLastWriteTime; 
	
	snprintf( fullpath, sizeof(fullpath), "%s\\%s", Directory, sd ) ; 
	if( !MakeDir( fullpath ) ) {
		snprintf( fullpath, sizeof(fullpath),"Unable to create directory: %s\\%s !",Directory, sd);
		MessageBox(NULL,fullpath,"Error",MB_OK|MB_ICONERROR); 
		return 0 ;
	}
	
	snprintf( buffer, sizeof(buffer), "%s\\%s", TEXT(PUTTY_REG_POS), s ) ;

	if( RegOpenKeyEx( HKEY_CURRENT_USER, TEXT(buffer), 0, KEY_READ, &hKey) == ERROR_SUCCESS ) {
		if( RegQueryInfoKey(hKey,achClass,&cchClassName,NULL,&cSubKeys,&cbMaxSubKey
			,&cchMaxClass,&cValues,&cchMaxValue,&cbMaxValueData,&cbSecurityDescriptor,&ftLastWriteTime) == ERROR_SUCCESS ) {
			if (cSubKeys) for (i=0; i<cSubKeys; i++) {
				cbName = MAX_KEY_LENGTH;
				retCode = RegEnumKeyEx(hKey, i, achKey, &cbName, NULL, NULL, NULL, &ftLastWriteTime) ;
				snprintf( buffer, sizeof(buffer), "%s\\%s", s, achKey ) ;
				snprintf( fullpath, sizeof(fullpath), "%s\\%s", sd, achKey ) ;
				MakeDirTree( Directory, buffer, fullpath ) ;
				}
			retCode = ERROR_SUCCESS ;
			if(cValues) for (i=0, retCode=ERROR_SUCCESS; i<cValues; i++) {
				cchValue = MAX_VALUE_NAME; 
				achValue[0] = '\0'; 
				if( (retCode = RegEnumValue(hKey, i, achValue, &cchValue, NULL, NULL,NULL,NULL) ) == ERROR_SUCCESS ){
					dwDataSize = 1024 ;
					RegQueryValueEx( hKey, TEXT( achValue ), 0, &lpType, lpData, &dwDataSize ) ;
					if( (int)lpType == REG_SZ ) {
						mungestr( achValue, buffer ) ;
						snprintf( fullpath, sizeof(fullpath), "%s\\%s\\%s", Directory, sd, buffer ) ;
						if( ( fp=fopen( fullpath, "wb") ) != NULL ) {
							fprintf( fp, "%s\\%s\\",achValue,lpData );
							fclose(fp);
							}
						}
					}
				}
			}
		RegCloseKey( hKey ) ;
		}
	return 1;
	}

// Convertit la base de registre en repertoire pour le mode savemode=dir
int Convert2Dir( const char * Directory ) {
	char buffer[MAX_VALUE_NAME], fullpath[MAX_VALUE_NAME] ;
	HKEY hKey;
	int retCode, i, delkeyflag=0 ;
	FILE *fp ;

	unsigned char lpData[1024] ;
	TCHAR achClass[MAX_PATH] = TEXT(""), achKey[MAX_KEY_LENGTH], achValue[MAX_VALUE_NAME] ; 
	DWORD cchClassName = MAX_PATH, cSubKeys=0, cbMaxSubKey, cchMaxClass, cValues, cchMaxValue, cbMaxValueData, cbSecurityDescriptor, cbName,cchValue = MAX_VALUE_NAME , dwDataSize, lpType;
	FILETIME ftLastWriteTime; 
	
	if( !RegTestKey( HKEY_CURRENT_USER, TEXT(PUTTY_REG_POS) ) ) // Si la cle de KiTTY n'existe pas on recupere celle de PuTTY
		{ TestRegKeyOrCopyFromPuTTY( HKEY_CURRENT_USER, TEXT(PUTTY_REG_POS) ) ; delkeyflag = 1 ; }
	
	snprintf( buffer, sizeof(buffer), "%s\\Commands", Directory ) ; DelDir( buffer) ; MakeDirTree( Directory, "Commands", "Commands" ) ;
	snprintf( buffer, sizeof(buffer), "%s\\Launcher", Directory ) ; DelDir( buffer) ; MakeDirTree( Directory, "Launcher", "Launcher" ) ;
	snprintf( buffer, sizeof(buffer), "%s\\Folders", Directory ) ; DelDir( buffer) ; MakeDirTree( Directory, "Folders", "Folders" ) ;
	snprintf( buffer, sizeof(buffer), "%s\\Commands", Directory ) ; DelDir( buffer) ; MakeDirTree( Directory, "Commands", "Commands" ) ;
	
	snprintf( buffer, sizeof(buffer), "%s\\Sessions", Directory ) ; DelDir( buffer) ; { 
		if(!MakeDir( buffer )) MessageBox(NULL,"Unable to create directory for storing sessions","Error",MB_OK|MB_ICONERROR); 
	}
	snprintf( buffer, sizeof(buffer), "%s\\Sessions", TEXT(PUTTY_REG_POS) ) ;
		
	snprintf( fullpath, sizeof(fullpath), "%s\\Sessions_Commands", Directory ) ; DelDir( fullpath ) ; 
	if(!MakeDir( fullpath )) { 
		MessageBox(NULL,"Unable to create directory for storing session commands","Error",MB_OK|MB_ICONERROR); 
	}

	if( RegOpenKeyEx( HKEY_CURRENT_USER, TEXT(buffer), 0, KEY_READ, &hKey) == ERROR_SUCCESS ) {
		if( RegQueryInfoKey(hKey,achClass,&cchClassName,NULL,&cSubKeys,&cbMaxSubKey
			,&cchMaxClass,&cValues,&cchMaxValue,&cbMaxValueData,&cbSecurityDescriptor,&ftLastWriteTime) == ERROR_SUCCESS ) {
			if (cSubKeys) for (i=0; i<cSubKeys; i++) {
				cbName = MAX_KEY_LENGTH;
				retCode = RegEnumKeyEx(hKey, i, achKey, &cbName, NULL, NULL, NULL, &ftLastWriteTime) ;
				unmungestr( achKey, buffer,MAX_PATH) ;
				IniFileFlag = SAVEMODE_REG ;
				load_settings( buffer, conf ) ;
				IniFileFlag = SAVEMODE_DIR ;
				SetInitialSessPath() ;
				SetCurrentDirectory( Directory ) ;
				
				if( DirectoryBrowseFlag ) if( strcmp(conf_get_str(conf,CONF_folder), "Default")&&strcmp(conf_get_str(conf,CONF_folder), "") ) {
					snprintf( fullpath, sizeof(fullpath), "%s\\Sessions\\%s", Directory, conf_get_str(conf,CONF_folder)) ;
					if( !MakeDir( fullpath ) ) { MessageBox(NULL,"Unable to create directory for storing session informations","Error",MB_OK|MB_ICONERROR); }
					SetSessPath( conf_get_str(conf,CONF_folder) ) ; 
				}
				
				save_settings( buffer, conf) ;

				snprintf( buffer, sizeof(buffer), "%s\\Sessions\\%s\\Commands", TEXT(PUTTY_REG_POS), achKey ) ;
				if( RegTestKey( HKEY_CURRENT_USER, buffer ) ) {
					snprintf( buffer, sizeof(buffer), "Sessions\\%s\\Commands", achKey ) ;
					snprintf( fullpath, sizeof(fullpath), "Sessions_Commands\\%s", achKey ) ;
					MakeDirTree( Directory, buffer, fullpath ) ;
				}
			}
		}
		RegCloseKey( hKey ) ;
	}
	
	snprintf( buffer, sizeof(buffer), "%s\\SshHostKeys", Directory ) ; DelDir( buffer) ; if( !MakeDir( buffer ) ) { 
		MessageBox(NULL,"Unable to create directory for storing ssh host keys","Error",MB_OK|MB_ICONERROR) ; 
	}
	snprintf( buffer, sizeof(buffer), "%s\\SshHostKeys", TEXT(PUTTY_REG_POS) ) ;
	if( RegOpenKeyEx( HKEY_CURRENT_USER, TEXT(buffer), 0, KEY_READ, &hKey) == ERROR_SUCCESS ) {
		if( RegQueryInfoKey(hKey,achClass,&cchClassName,NULL,&cSubKeys,&cbMaxSubKey
			,&cchMaxClass,&cValues,&cchMaxValue,&cbMaxValueData,&cbSecurityDescriptor,&ftLastWriteTime) == ERROR_SUCCESS ) {
			retCode = ERROR_SUCCESS ;
			if(cValues) for (i=0, retCode=ERROR_SUCCESS; i<cValues; i++) {
				cchValue = MAX_VALUE_NAME; 
				achValue[0] = '\0'; 
				if( (retCode = RegEnumValue(hKey, i, achValue, &cchValue, NULL, NULL,NULL,NULL) ) == ERROR_SUCCESS ) {
					dwDataSize = 1024 ;
					RegQueryValueEx( hKey, TEXT( achValue ), 0, &lpType, lpData, &dwDataSize ) ;
					if( (int)lpType == REG_SZ ) {
						mungestr( achValue, buffer ) ;
						snprintf( fullpath, sizeof(fullpath), "%s\\SshHostKeys\\%s", Directory, buffer ) ;
						if( ( fp=fopen( fullpath, "wb") ) != NULL ) {
							fprintf( fp, "%s",lpData ) ;
							fclose(fp);
						}
					}
				}
			}
				
		}
		RegCloseKey( hKey ) ;
	}
#ifdef MOD_PROXY
	snprintf( buffer, sizeof(buffer), "%s\\Proxies", Directory ) ; DelDir( buffer) ; if( !MakeDir( buffer ) ) { 
		MessageBox(NULL,"Unable to create directory for storing proxies definition","Error",MB_OK|MB_ICONERROR) ; 
	}
	snprintf( buffer, sizeof(buffer), "%s\\Proxies", TEXT(PUTTY_REG_POS) ) ;
		if( RegOpenKeyEx( HKEY_CURRENT_USER, TEXT(buffer), 0, KEY_READ, &hKey) == ERROR_SUCCESS ) {
		if( RegQueryInfoKey(hKey,achClass,&cchClassName,NULL,&cSubKeys,&cbMaxSubKey
			,&cchMaxClass,&cValues,&cchMaxValue,&cbMaxValueData,&cbSecurityDescriptor,&ftLastWriteTime) == ERROR_SUCCESS ) {
			if (cSubKeys) for (i=0; i<cSubKeys; i++) {
				cbName = MAX_KEY_LENGTH;
				retCode = RegEnumKeyEx(hKey, i, achKey, &cbName, NULL, NULL, NULL, &ftLastWriteTime) ;
				ExportSubKeyToFile( HKEY_CURRENT_USER, buffer, achKey, ConfigDirectory, "Proxies" ) ;
			}
		}
		RegCloseKey( hKey ) ;
	}
#endif
	
	if( delkeyflag ) { RegDelTree (HKEY_CURRENT_USER, TEXT(PUTTY_REG_PARENT) ) ; }
	
	return 0;
}

// Convertit une sauvegarde en mode savemode=dir vers la base de registre
	// NE FONCTIONNE PAS
	// PREFERER LA FONCTION Convert1Reg() fichier par fichier
	// A appeler avec le parametre -convert1reg
void ConvertDir2Reg( const char * Directory, HKEY hKey, char * path )  {
	char directory[MAX_VALUE_NAME], buffer[MAX_VALUE_NAME], session[MAX_VALUE_NAME] ;
	DIR * dir ;
	struct dirent * de ;
	if( strlen(path)>0 ) {
		snprintf( directory, sizeof(directory), "%s\\Sessions\\%s", Directory, path ) ;
	} else {
		snprintf( directory, sizeof(directory), "%s\\Sessions", Directory ) ;
	}
	if( ( dir = opendir( directory ) ) != NULL ) {
		while( (de=readdir(dir)) != NULL ) 
		if( strcmp(de->d_name,".")&&strcmp(de->d_name,"..")  ) {
			snprintf( buffer, sizeof(buffer), "%s\\%s", directory, de->d_name ) ;
			if( GetFileAttributes( buffer ) & FILE_ATTRIBUTE_DIRECTORY ) {
				if( strlen(path)>0 ) snprintf( buffer, sizeof(buffer), "%s\\%s", path, de->d_name ) ;
				else strcpy( buffer, de->d_name ) ;
				
//debug_log("Directory=%s|\n",buffer);
				ConvertDir2Reg( Directory, hKey, buffer ) ;
			} else {
				SetSessPath( path ) ;
				IniFileFlag = SAVEMODE_DIR ;
//debug_log("Session=%s|\n",de->d_name);
				unmungestr( de->d_name, session, MAX_PATH) ;
//debug_log("	new\n");
				Conf * tmpConf = conf_new() ;
//debug_log("	load\n");
				load_settings( session, tmpConf ) ;
				IniFileFlag = SAVEMODE_REG ;
				strcpy( conf_get_str( tmpConf, CONF_folder ), path ) ;
//debug_log("	save\n");
				save_settings( session, tmpConf ) ;
//debug_log("	free\n");
				conf_free( tmpConf ) ;
//debug_log("	end\n");
			}
		}
		closedir( dir ) ;
	}
}
 
int Convert2Reg( const char * Directory ) {
	char buffer[MAX_VALUE_NAME] ;
	HKEY hKey;
 
	snprintf( buffer, sizeof(buffer), "%s\\Sessions", TEXT(PUTTY_REG_POS) ) ;
	if( RegTestKey( HKEY_CURRENT_USER, buffer ) ) 
		{ RegDelTree (HKEY_CURRENT_USER, buffer ) ; }
 
	SetCurrentDirectory( Directory ) ;
	snprintf( buffer, sizeof(buffer), "%s\\Sessions", TEXT(PUTTY_REG_POS) ) ;
	RegTestOrCreate( HKEY_CURRENT_USER, buffer, NULL, NULL ) ;
 
	if( RegOpenKeyEx( HKEY_CURRENT_USER, TEXT(buffer), 0, KEY_READ, &hKey) == ERROR_SUCCESS ) {
		strcpy( buffer, "" ) ;
		ConvertDir2Reg( Directory, hKey, buffer ) ;
		RegCloseKey( hKey ) ;
		}
 
	return 0 ;
	}

char *dirname(char *path);
int Convert1Reg( const char * filename ) {
	char buffer[MAX_VALUE_NAME] = "", session[MAX_VALUE_NAME], dname[MAX_VALUE_NAME], *bname ;
	HKEY hKey;
	int i;
	if( (filename==NULL)||(strlen(filename)==0) ) { return 1 ; }
	snprintf( buffer, sizeof(buffer), "%s\\Sessions", TEXT(PUTTY_REG_POS) ) ;
	if( RegOpenKeyEx( HKEY_CURRENT_USER, TEXT(buffer), 0, KEY_READ, &hKey) == ERROR_SUCCESS ) {
		strcpy(buffer,filename);
		bname = buffer ;
		for( i=0; i<strlen(buffer); i++ ) { 
			if(buffer[i]=='/') buffer[i]='\\' ;
			if( (buffer[i]=='\\')&&(buffer[i+1]!='\0') ) { bname = buffer+i+1 ; }
		}
		strcpy(dname,buffer);
		strcpy(dname,dirname(dname));
		SetCurrentDirectory( dname ) ;
		SetSessPath(".");
		IniFileFlag = SAVEMODE_DIR ;
		unmungestr( bname, session, MAX_PATH) ;
		Conf * tmpConf = conf_new() ;
		load_settings( session, tmpConf ) ;
		IniFileFlag = SAVEMODE_REG ;
		strcpy( conf_get_str( tmpConf, CONF_folder ), dname ) ;
		save_settings( session, tmpConf ) ;
		RegCloseKey( hKey ) ;
	} else {
		MessageBox(NULL,"Unable to open sessions registry key","Error",MB_OK|MB_ICONERROR) ;
	}
	return 0 ;
}

void ResetWindow(int reinit) ;
#ifndef IDM_RECONF
#define IDM_RECONF    0x0050
#endif

/* NegativeColours / BlackOnWhiteColours / ChangeFontSize / ChangeSettings /
 * ManageViewer moved to kitty_colours.c (declared in kitty.h). */
	
/* The keyboard-shortcut machinery (DefineShortcuts / TranslateShortcuts /
 * InitShortcuts / ManageShortcuts + the shortcut tables) lives in
 * kitty_shortcuts.c; the tables are declared in kitty.h. */

// Initialisation des parametres a partir du fichier kitty.ini
#ifdef MOD_BACKGROUNDIMAGE
void SetShrinkBitmapEnable(int) ;
#endif

void LoadParameters( void ) {
	char buffer[4096] ;

	/* A lire en premier */
	if( ReadParameterN( INIT_SECTION, "debug", buffer, sizeof(buffer) ) ) { if( !stricmp( buffer, "YES" ) ) debug_flag = 1 ; }

	if( ReadParameterN( INIT_SECTION, "antiidle", buffer, sizeof(buffer) ) ) { buffer[127]='\0'; strcpy( AntiIdleStr, buffer ) ; }
	if( ReadParameterN( INIT_SECTION, "antiidledelay", buffer, sizeof(buffer) ) ) 
		{ AntiIdleCountMax = (int)floor(atoi(buffer)/10.0) ; if( AntiIdleCountMax<=0 ) AntiIdleCountMax =1 ; }
#ifdef MOD_BACKGROUNDIMAGE
	//if( debug_flag )
	if( ReadParameterN( INIT_SECTION, "bgimage", buffer, sizeof(buffer) ) ) {	
		if( !stricmp( buffer, "NO" ) ) SetBackgroundImageFlag( 0 ) ; 
		if( !stricmp( buffer, "YES" ) ) SetBackgroundImageFlag( 1 ) ;  // Broken en 0.71 ==> on desactive
	}
#endif
	if( ReadParameterN( INIT_SECTION, "bcdelay", buffer, sizeof(buffer) ) ) { between_char_delay = atoi( buffer ) ; }
	if( ReadParameterN( INIT_SECTION, "browsedirectory", buffer, sizeof(buffer) ) ) { 
		if( !stricmp( buffer, "NO" ) ) { DirectoryBrowseFlag = 0 ; }
		else if( (!stricmp( buffer, "YES" )) && (IniFileFlag==SAVEMODE_DIR) ) DirectoryBrowseFlag = 1 ;
	}
	if( ReadParameterN( INIT_SECTION, "commanddelay", buffer, sizeof(buffer) ) ) {
		autocommand_delay = (int)(1000*atof( buffer )) ;
		if(autocommand_delay<5) autocommand_delay = 5 ; 
	}
	if( ReadParameterN( INIT_SECTION, "conf", buffer, sizeof(buffer) ) ) { if( !stricmp( buffer, "NO" ) ) NoKittyFileFlag = 1 ; }
	if( ReadParameterN( INIT_SECTION, "configdir", buffer, sizeof(buffer) ) ) { 
		if( strlen( buffer ) > 0 ) { if( existdirectory(buffer) ) SetConfigDirectory( buffer ) ; }
	}
	if( ReadParameterN( INIT_SECTION, "cryptsalt", buffer, sizeof(buffer) ) ) { SetCryptSaltFlag( atoi(buffer) ) ; }
	if( ReadParameterN( INIT_SECTION, "ctrltab", buffer, sizeof(buffer) ) ) { if( !stricmp( buffer, "NO" ) ) SetCtrlTabFlag( 0 ) ; }
	if( ReadParameterN( INIT_SECTION, "hyperlink", buffer, sizeof(buffer) ) ) {
		if( !stricmp( buffer, "NO" ) ) HyperlinkFlag = 0 ;
		if( !stricmp( buffer, "YES" ) ) HyperlinkFlag = 1 ;
	}
	if( ReadParameterN( INIT_SECTION, "icon", buffer, sizeof(buffer) ) ) { if( !stricmp( buffer, "YES" ) ) IconeFlag = 1 ; }
	if( ReadParameterN( INIT_SECTION, "iconfile", buffer, sizeof(buffer) ) ) {
		if( existfile( buffer ) ) {
			if( IconFile != NULL ) free( IconFile ) ;
			IconFile = (char*) malloc( strlen(buffer)+1 ) ;
			strcpy( IconFile, buffer ) ;
			if( ReadParameterN( INIT_SECTION, "numberoficons", buffer, sizeof(buffer) ) ) { NumberOfIcons = atof( buffer ) ; }
		}
	}
	if( ReadParameterN( INIT_SECTION, "initdelay", buffer, sizeof(buffer) ) ) { 
		init_delay = (int)(1000*atof( buffer )) ;
		if( init_delay < 0 ) init_delay = 2000 ; 
	}
	if( ReadParameterN( INIT_SECTION, "internaldelay", buffer, sizeof(buffer) ) ) { 
		internal_delay = atoi( buffer ) ; 
		if( internal_delay < 1 ) internal_delay = 1 ;
	}
	if( ReadParameterN( INIT_SECTION, "fileextension", buffer, sizeof(buffer) ) ) {
		if( strlen(buffer) > 0 ) {
			snprintf( FileExtension, sizeof(FileExtension), "%s%s", (buffer[0]!='.')?".":"", buffer ) ;
			str_rtrim( FileExtension, " " ) ;
		}				
	}
	if( ReadParameterN( INIT_SECTION, "mouseshortcuts", buffer, sizeof(buffer) ) ) {
		if( !stricmp( buffer, "NO" ) ) MouseShortcutsFlag = 0 ;
		if( !stricmp( buffer, "YES" ) ) MouseShortcutsFlag = 1 ;
	}
	/* cyd01/KiTTY #548: force classic modal error boxes instead of inline terminal errors */
	if( ReadParameterN( INIT_SECTION, "modalerrors", buffer, sizeof(buffer) ) ) {
		if( !stricmp( buffer, "YES" ) ) SetModalErrorsFlag( 1 ) ;
		if( !stricmp( buffer, "NO" ) ) SetModalErrorsFlag( 0 ) ;
	}
	if( ReadParameterN( INIT_SECTION, "pastesize", buffer, sizeof(buffer) ) ) { if( atoi(buffer)>0 ) SetPasteSize( atoi(buffer) ) ; }
	if( ReadParameterN( INIT_SECTION, "PSCPPath", buffer, sizeof(buffer) ) ) {
		if( existfile( buffer ) ) { 
			if( PSCPPath!=NULL) { free(PSCPPath) ; PSCPPath = NULL ; }
			PSCPPath = (char*) malloc( strlen(buffer) + 1 ) ; strcpy( PSCPPath, buffer ) ;
		}
	}
	if( ReadParameterN( INIT_SECTION, "readonly", buffer, sizeof(buffer) ) ) { if( !stricmp( buffer, "YES" ) ) SetReadOnlyFlag(1) ; }
	if( ReadParameterN( INIT_SECTION, "sav", buffer, sizeof(buffer) ) ) {
		if( strlen( buffer ) > 0 ) {
			/* Ignore an inherited legacy default (kitty.sav / kitty084.sav) written
			 * by an older KiTTY, so the current default (kittynew.sav) takes over
			 * without the user having to edit kitty.ini. A genuinely custom path is
			 * still honoured. */
			const char *bn = strrchr( buffer, '\\' ) ; bn = bn ? bn+1 : buffer ;
			if( stricmp( bn, "kitty.sav" ) && stricmp( bn, "kitty084.sav" ) ) {
				if( KittySavFile!=NULL ) free( KittySavFile ) ;
				KittySavFile=(char*)malloc( strlen(buffer)+1 ) ;
				strcpy( KittySavFile, buffer) ;
			}
		}
	}
	if( ReadParameterN( INIT_SECTION, "shortcuts", buffer, sizeof(buffer) ) ) { 
		if( !stricmp( buffer, "NO" ) ) ShortcutsFlag = 0 ; 
		if( !stricmp( buffer, "YES" ) ) ShortcutsFlag = 1 ; 
	}
	if( ReadParameterN( INIT_SECTION, "slidedelay", buffer, sizeof(buffer) ) ) { ImageSlideDelay = atoi( buffer ) ; }
	if( ReadParameterN( INIT_SECTION, "sshversion", buffer, sizeof(buffer) ) ) { set_sshver( buffer ) ; }
	if( ReadParameterN( INIT_SECTION, "userpasssshnosave", buffer, sizeof(buffer) ) ) { 
		if( !stricmp( buffer, "no" ) ) SetUserPassSSHNoSave(0) ;
		if( !stricmp( buffer, "yes" ) ) SetUserPassSSHNoSave(1) ;
	}
	if( ReadParameterN( INIT_SECTION, "winroll", buffer, sizeof(buffer) ) ) { 
		if( !stricmp( buffer, "no" ) ) WinrolFlag = 0 ;
		if( !stricmp( buffer, "yes" ) ) WinrolFlag = 1 ;
	}
	if( ReadParameterN( INIT_SECTION, "WinSCPPath", buffer, sizeof(buffer) ) ) {
		if( existfile( buffer ) ) { 
			if( WinSCPPath!=NULL) { free(WinSCPPath) ; WinSCPPath = NULL ; }
			WinSCPPath = (char*) malloc( strlen(buffer) + 1 ) ; strcpy( WinSCPPath, buffer ) ;
		}
	}
#ifdef MOD_PROXY
	if( ReadParameterN( "ConfigBox", "proxyselection", buffer, sizeof(buffer) ) ) {
		/* yes = always, no = never, auto (or anything else) = when defined */
		if( !stricmp( buffer, "YES" ) ) { SetProxySelectionFlag(1) ; }
		else if( !stricmp( buffer, "NO" ) ) { SetProxySelectionFlag(-1) ; }
		else { SetProxySelectionFlag(0) ; }
	}
#endif
#ifdef MOD_ZMODEM
	if( ReadParameterN( INIT_SECTION, "zmodem", buffer, sizeof(buffer) ) ) {
		if( !stricmp( buffer, "NO" ) ) SetZModemFlag( 0 ) ;
		if( !stricmp( buffer, "YES" ) ) SetZModemFlag( 1 ) ; /* re-enabled: 0.84 port has a real helper spawn (kitty_zmodem.c) */
		}
#endif
#ifdef MOD_RECONNECT
	if( ReadParameterN( INIT_SECTION, "autoreconnect", buffer, sizeof(buffer) ) ) { if( !stricmp( buffer, "NO" ) ) AutoreconnectFlag = 0 ; }
	if( ReadParameterN( INIT_SECTION, "ReconnectDelay", buffer, sizeof(buffer) ) ) { 
		ReconnectDelay = atoi( buffer ) ;
		if( ReconnectDelay < 1 ) ReconnectDelay = 1 ;
	}
#endif
#ifndef MOD_NOTRANSPARENCY
	if( ReadParameterN( INIT_SECTION, "transparency", buffer, sizeof(buffer) ) ) {
		if( !stricmp( buffer, "YES" ) ) { TransparencyFlag = 1 ; }
		else { TransparencyFlag = 0 ; } 
	}
#endif

#ifdef MOD_BACKGROUNDIMAGE
	if( ReadParameterN( INIT_SECTION, "shrinkbitmap", buffer, sizeof(buffer) ) ) { if( !stricmp( buffer, "YES" ) ) SetShrinkBitmapEnable(1) ; else SetShrinkBitmapEnable(0) ; }
#endif

	if( readINI( KittyIniFile, "ConfigBox", "height", buffer, sizeof(buffer) ) ) {
		ConfigBoxHeight = atoi( buffer ) ;
		/* NB: the extra row taken by the Proxy-choice droplist (when shown) is
		 * now accounted for in the config-box window sizing (windows/dialog.c),
		 * not by shrinking the list here. */
	}
	if( readINI( KittyIniFile, "ConfigBox", "windowheight", buffer, sizeof(buffer) ) ) {
		ConfigBoxWindowHeight = atoi( buffer ) ;
	}
	if( readINI( KittyIniFile, "ConfigBox", "filter", buffer, sizeof(buffer) ) ) {
		if( !stricmp( buffer, "NO" ) ) SessionFilterFlag = 0 ;
	}
	if( readINI( KittyIniFile, "ConfigBox", "defaultsettings", buffer, sizeof(buffer) ) ) {
		if( !stricmp( buffer, "NO" ) ) DefaultSettingsFlag = 0 ;
	}
	/* How many levels of the config-box Category tree to auto-expand. Default
	 * (unset / all / full) = fully expanded; a number 1..N expands only that
	 * deep (1 = top categories only, like stock PuTTY). */
	if( readINI( KittyIniFile, "ConfigBox", "categoryexpand", buffer, sizeof(buffer) ) ) {
		extern int kitty_category_expand_depth ;
		if( strlen(buffer)==0 || !stricmp(buffer,"all") || !stricmp(buffer,"full") || !stricmp(buffer,"max") || !stricmp(buffer,"yes") )
			kitty_category_expand_depth = 99 ;
		else { int d = atoi(buffer) ; kitty_category_expand_depth = (d >= 1) ? d : 99 ; }
	}
	if( readINI( KittyIniFile, "Print", "height", buffer, sizeof(buffer) ) ) {
		PrintCharSize = atoi( buffer ) ;
	}
	if( readINI( KittyIniFile, "Print", "maxline", buffer, sizeof(buffer) ) ) {
		PrintMaxLinePerPage = atoi( buffer ) ;
	}
	if( readINI( KittyIniFile, "Print", "maxchar", buffer, sizeof(buffer) ) ) {
		PrintMaxCharPerLine = atoi( buffer ) ;
	}
	if( readINI( KittyIniFile, "Folder", "del", buffer, sizeof(buffer) ) ) {
		StringList_Del( FolderList, buffer ) ;
		delINI( KittyIniFile, "Folder", "del" ) ;
	}
}

// Initialisation de noms de fichiers de configuration kitty.ini et kitty.sav
// APPDATA = 	C:\Documents and Settings\U502190\Application Data sur XP
//		C:\Users\Cyril\AppData\Roaming sur Vista
//
// En mode base de registre on cherche le fichier de configuration
// - dans la variable d'environnement KITTY_INI_FILE
// - kitty.ini dans le repertoire de lancement de kitty.exe s'il existe
// - sinon putty.ini dans le repertoire de lancement de kitty.exe s'il existe
// - sinon kitty.ini dans le repertoire %APPDATA%/KiTTY s'il existe
//
// En mode portable on cherche le fichier de configuration
// - kitty.ini dans le repertoire de lancement de kitty.exe s'il existe
// - sinon putty.ini dans le repertoire de lancement de kitty.exe s'il existe
// 
void InitNameConfigFile( void ) {
	char buffer[4096] ;
	if( KittyIniFile != NULL ) { free( KittyIniFile ) ; }
	KittyIniFile=NULL ;

	if( getenv("KITTY_INI_FILE") != NULL ) { strcpy( buffer, getenv("KITTY_INI_FILE") ) ; }
	if( !existfile( buffer ) ) {
		snprintf( buffer, sizeof(buffer), "%s\\%s", InitialDirectory, DEFAULT_INIT_FILE ) ;
		if( !existfile( buffer ) ) {
			snprintf( buffer, sizeof(buffer), "%s\\putty.ini", InitialDirectory ) ;
			if( !existfile( buffer ) ) {
				if( IniFileFlag != SAVEMODE_DIR ) {
					snprintf( buffer, sizeof(buffer), "%s\\%s\\%s", getenv("APPDATA"), INIT_SECTION, DEFAULT_INIT_FILE ) ;
					if( !existfile( buffer ) ) {
						snprintf( buffer, sizeof(buffer), "%s\\%s", getenv("APPDATA"), INIT_SECTION ) ;
						CreateDirectory( buffer, NULL ) ;
						snprintf( buffer, sizeof(buffer), "%s\\%s\\%s", getenv("APPDATA"), INIT_SECTION, DEFAULT_INIT_FILE ) ;
					}
				} else {
					snprintf( buffer, sizeof(buffer), "%s\\%s", InitialDirectory, DEFAULT_INIT_FILE ) ;
				}
			}
		}
	}
	KittyIniFile=(char*)malloc( strlen( buffer)+2 ) ; strcpy( KittyIniFile, buffer) ;

	if( KittySavFile != NULL ) { free( KittySavFile ) ; } 
	KittySavFile=NULL ;
	snprintf( buffer, sizeof(buffer), "%s\\%s", InitialDirectory, DEFAULT_SAV_FILE ) ;
	if( !existfile( buffer ) ) {
		if( IniFileFlag != SAVEMODE_DIR ) {
			snprintf( buffer, sizeof(buffer), "%s\\%s\\%s", getenv("APPDATA"), INIT_SECTION, DEFAULT_SAV_FILE ) ;
			if( !existfile( buffer ) ) {
				snprintf( buffer, sizeof(buffer), "%s\\%s", getenv("APPDATA"), INIT_SECTION ) ;
				CreateDirectory( buffer, NULL ) ;
				snprintf( buffer, sizeof(buffer), "%s\\%s\\%s", getenv("APPDATA"), INIT_SECTION, DEFAULT_SAV_FILE ) ;
			}
		}
	}
	KittySavFile=(char*)malloc( strlen( buffer)+2 ) ; strcpy( KittySavFile, buffer) ;
	
	snprintf( buffer, sizeof(buffer), "%s\\kitty.dft", InitialDirectory ) ;
	if( existfile( KittyIniFile ) && existfile( buffer ) )  unlink( buffer ) ;
	if( !existfile( KittyIniFile ) )
		if( existfile( buffer ) ) rename( buffer, KittyIniFile ) ;
}
	
// Ecriture de l'increment de compteurs
void WriteCountUpAndPath( void ) {
	// Sauvegarde la liste des folders
	SaveFolderList() ;
		
	// Incremente le compteur d'utilisation
	CountUp() ;

	// Positionne la version du binaire
	WriteParameter( INIT_SECTION, "Build", BuildVersionTime ) ;
	
	// Recherche cthelper.exe s'il existe
	SearchCtHelper() ;
		
	// Recherche pscp s'il existe
	SearchPSCP() ;
	
	// Recherche WinSCP s'il existe
	SearchWinSCP() ;
	}

// Initialisation specifique a KiTTY
void appendPath(const char *append) ;
extern char sesspath[];
int loadPath() ;
#ifdef MOD_NETDEBUG
/* KiTTY netdebug: append a millisecond-timestamped startup checkpoint to the
 * same %USERPROFILE%\kitty_netdebug.log used by the event-log tee, so a slow
 * once-per-process startup can be pinpointed offline. Compiled only in the
 * MOD_NETDEBUG build; a no-op (absent) otherwise. */
void kitty_netdbg_ts( const char *msg ) {
	static FILE *f = NULL ;
	SYSTEMTIME s ; GetLocalTime( &s ) ;
	if( !f ) { char p[MAX_PATH] ; const char *h=getenv("USERPROFILE") ;
		snprintf( p, sizeof(p), "%s\\kitty_netdebug.log", h?h:"C:" ) ; f=fopen( p, "a" ) ; }
	if( f ) { fprintf( f, "%02d:%02d:%02d.%03d  [STARTUP] %s\n",
		s.wHour, s.wMinute, s.wSecond, s.wMilliseconds, msg ) ; fflush( f ) ; }
}
#define NETDBG_TS(m) kitty_netdbg_ts(m)
#else
#define NETDBG_TS(m) ((void)0)
#endif

void InitWinMain( void ) {
	char buffer[4096];
	int i ;

	NETDBG_TS("InitWinMain: enter");
	srand(time(NULL));
	
	if( existfile("kitty.log") ) { unlink( "kitty.log" ) ; }
	
	//if( !RegTestKey(HKEY_CLASSES_ROOT,"kitty.connect.1") ) { CreateFileAssoc() ; }

	// Initialisation de la version binaire
	sprintf( BuildVersionTime, "%s @ %s", BUILD_VERSION, BUILD_TIME ) ;
#ifdef MOD_PORTABLE
	sprintf( BuildVersionTime, "%s-portable @ %s", BUILD_VERSION, BUILD_TIME ) ;
#endif
#ifdef MOD_NOTRANSPARENCY
	sprintf( BuildVersionTime, "%s-nt @ %s", BUILD_VERSION, BUILD_TIME ) ;
#endif

	// Initialisation de la librairie de cryptage
	NETDBG_TS("before bcrypt_init");
	bcrypt_init( 0 ) ;
	NETDBG_TS("after bcrypt_init");

	// Recupere le repertoire de depart et le repertoire de la configuration pour savemode=dir
	GetInitialDirectory( InitialDirectory ) ;
	NETDBG_TS("after GetInitialDirectory");

	// Initialise les noms des fichier de configuration kitty.ini et kitty.sav
	InitNameConfigFile() ;

	// Initialisation du nom de la classe
	strcpy( KiTTYClassName, appname ) ;

#ifdef MOD_PERSO
	if( ReadParameterN( INIT_SECTION, "KiClassName", buffer, sizeof(buffer) ) )
		{ if( (strlen(buffer)>0) && (strlen(buffer)<128) ) { buffer[127]='\0'; strcpy( KiTTYClassName, buffer ) ; } }
	appname = KiTTYClassName ;
	/* Select the registry hive to match KiClassName: default KiTTY's own
	 * (Software\9bis.com\KiTTY); PuTTY's hive when KiClassName=PuTTY. */
	{ extern void kitty_set_registry_root(int use_putty);
	  kitty_set_registry_root( !stricmp(KiTTYClassName, "PuTTY") ) ; }
#endif

	// Initialise le tableau des menus
	InitSpecialMenuTab() ;
	
	// Test le mode de fonctionnement de la sauvegarde des sessions
	GetSaveMode() ;
	NETDBG_TS("after GetSaveMode");

	/* Aux-window position memory (About boxes, etc.) is registry-backed. In portable
	 * modes (anything but SAVEMODE_REG) place windows correctly but do NOT persist, so
	 * we leave no registry footprint -- consistent with the rest of portable KiTTY. */
	{ void kitty_auxpos_set_persist( int on ) ;
	  if( IniFileFlag != SAVEMODE_REG ) kitty_auxpos_set_persist( 0 ) ; }

	// Initialisation des parametres à partir du fichier kitty.ini
	LoadParameters() ;
	NETDBG_TS("after LoadParameters (kitty.ini read)");

	// Ajoute les répertoires InitialDirectory et ConfigDirectory au PATH

	// Initialisation des shortcuts
	InitShortcuts() ;
	NETDBG_TS("after InitShortcuts");

	/* KiTTY 0.84: migrate the old 9bis.com\KiTTY hive to kapper.net\KiTTY BEFORE the
	 * PuTTY-import check below, so once our hive exists that import path stays out of the
	 * way. Idempotent + non-destructive (see kitty_registry.c). */
	if( (IniFileFlag == SAVEMODE_REG) || (IniFileFlag == SAVEMODE_FILE) ) {
		NETDBG_TS("before MigrateOldKittyHive");
		MigrateOldKittyHive() ;
		NETDBG_TS("after MigrateOldKittyHive");
		/* One-time repair of registry sessions that persisted the buggy
		 * SHARROW_APPLICATION default; must run after the hive exists. Registry-only:
		 * it reads/writes the kapper.net hive and its marker, so it must NOT run in
		 * portable (SAVEMODE_FILE) mode -- portable sessions are files, healed instead
		 * by the conf.h SHARROW_BITMAP default; letting portable flip the marker would
		 * also consume the one-shot before an installed KiTTY could run it. Idempotent
		 * (marker-guarded). */
		if( IniFileFlag == SAVEMODE_REG ) { RepairSharrowDefaults() ; }
		/* One-time migration of legacy 9bis named proxies into our hive, with its
		 * own marker so it fires even when sessions were migrated in an earlier
		 * build. Registry-mode only (REG||FILE); DPAPI-protects passwords on copy
		 * and in place (hknet/KiTTY#11, TASK_named_proxies.md Piece 5). */
		kitty_migrate_old_proxies() ;
	}

	// Chargement de la base de registre si besoin
	if( IniFileFlag == SAVEMODE_REG ) { // Mode de sauvegarde registry
		// Si la cle n'existe pas ...
		if( !RegTestKey( HKEY_CURRENT_USER, TEXT(PUTTY_REG_POS) ) ) { 
			HWND hdlg = InfoBox( hinst, NULL ) ;
			// ... on charge le backup le plus recent (kittynew-<timestamp>.sav),
			// ou l'ancien fichier a nom fixe s'il existe encore.
			char newestsav[4096] = "" ;
			int havesav = sav_find_newest( KittySavFile, newestsav, sizeof(newestsav) ) ;
			if( !havesav && existfile( KittySavFile ) ) { snprintf( newestsav, sizeof(newestsav), "%s", KittySavFile ) ; havesav = 1 ; }
			if( havesav ) {
				char *savedptr = KittySavFile ;
				KittySavFile = newestsav ;   /* LoadRegistryKey reads the global */
				InfoBoxSetText( hdlg, "Initializing registry." ) ;
				InfoBoxSetText( hdlg, "Loading saved sessions from file." ) ;
				LoadRegistryKey( hdlg ) ;
				InfoBoxClose( hdlg ) ;
				KittySavFile = savedptr ;
			} else { // Sinon on regarde si il y a la cle de PuTTY et on la recupere
				InfoBoxSetText( hdlg, "Initializing registry." ) ;
				InfoBoxSetText( hdlg, "First time running. Loading saved sessions from PuTTY registry." ) ;
				TestRegKeyOrCopyFromPuTTY( HKEY_CURRENT_USER, TEXT(PUTTY_REG_POS) ) ; 
				InfoBoxClose( hdlg ) ;
			}
		}
	} else if( IniFileFlag == SAVEMODE_FILE ){ // Mode de sauvegarde fichier
		if( !RegTestKey( HKEY_CURRENT_USER, TEXT(PUTTY_REG_POS) ) ) { // la cle de registre n'existe pas 
			HWND hdlg = InfoBox( hinst, NULL ) ;
			InfoBoxSetText( hdlg, "Initializing registry." ) ;
			InfoBoxSetText( hdlg, "Loading saved sessions from file." ) ;
			LoadRegistryKey( hdlg ) ; 
			InfoBoxClose( hdlg ) ;
			}
#ifdef MOD_PERSO
		else { // la cle de registre existe deja
			if( WindowsCount( MainHwnd ) == 1 ) { // Si c'est le 1er kitty on sauvegarde la cle de registre avant de charger le fichier kitty.sav
				HWND hdlg = InfoBox( hinst, NULL ) ;
				InfoBoxSetText( hdlg, "Initializing registry." ) ;
				RegRenameTree( hdlg, HKEY_CURRENT_USER, TEXT(PUTTY_REG_POS), TEXT(PUTTY_REG_POS_SAVE) ) ;
				InfoBoxSetText( hdlg, "Loading saved sessions." ) ;
				LoadRegistryKey( hdlg ) ;
				InfoBoxClose( hdlg ) ;
				}
			}
#endif
		}
	else if( IniFileFlag == SAVEMODE_DIR ){ // Mode de sauvegarde directory
		if( strlen(sesspath) == 0 ) { loadPath() ; }
		/* KiTTY 0.84: activate the portable file storage backend (windows/storage.c)
		 * now that sesspath is known. Sessions are then read/written as one file per
		 * session under sesspath, instead of the registry. Decoupled setters so
		 * libsettings stays registry-only in tools that never call them. */
		{
			extern void kitty_set_storage_mode( int ) ;
			extern void kitty_set_session_dir( const char * ) ;
			extern void kitty_set_portable_password_protection( const char * ) ;
			char ppmode[32] = "" ;
			kitty_set_storage_mode( SAVEMODE_DIR ) ;
			kitty_set_session_dir( sesspath ) ;
			/* Portable at-rest password policy: master (default) or the
			 * explicit legacy/plaintext compatibility escape hatch. */
			if( readINI( KittyIniFile, INIT_SECTION, "PortablePasswordProtection", ppmode, sizeof(ppmode) ) )
				kitty_set_portable_password_protection( ppmode ) ;
		}
		/* Test Default Settings */
		/*
		char * defaultfile = (char*)malloc( strlen(sesspath)+20 ) ;
		sprintf( defaultfile, "%s\\Default Settings", sesspath ) ;
		if( !existfile(defaultfile) && GetDefaultSettingsFlag() ) {
			create_settings(KITTY_DEFAULT_SESSION) ;
		}
		free( defaultfile ) ;
		*/
	}

	// Make mandatory registry keys
	snprintf( buffer, sizeof(buffer), "%s\\%s", TEXT(PUTTY_REG_POS), "Commands" ) ;
	if( (IniFileFlag == SAVEMODE_REG)||( IniFileFlag == SAVEMODE_FILE) ) 
		RegTestOrCreate( HKEY_CURRENT_USER, buffer, NULL, NULL ) ;

#ifdef MOD_PROXY
	// Initiate proxies list
	InitProxyList() ;
#endif
#ifdef MOD_LAUNCHER
	// Initiate launcher
	snprintf( buffer, sizeof(buffer), "%s\\%s", TEXT(PUTTY_REG_POS), "Launcher" ) ;
	if( (IniFileFlag == SAVEMODE_REG)||( IniFileFlag == SAVEMODE_FILE) )  
		if( !RegTestKey( HKEY_CURRENT_USER, buffer ) ) { InitLauncherRegistry() ; }
#endif
	NETDBG_TS("after registry/savemode block");
	// Initiate folders list
	InitFolderList() ;
	NETDBG_TS("after InitFolderList");

	// Incremente et ecrit les compteurs
	if( IniFileFlag == SAVEMODE_REG ) {
		WriteCountUpAndPath() ;
	}

	// Initialise la gestion des icones depuis la librairie kitty.dll si elle existe
	if( !GetPuttyFlag() ) {
		if( IconFile != NULL )
		if( existfile( IconFile ) ) 
			{ HMODULE hDll ; if( ( hDll = LoadLibrary( TEXT(IconFile) ) ) != NULL ) hInstIcons = hDll ; }
		if( hInstIcons==NULL )
		if( existfile( "kitty.dll" ) )
			{ HMODULE hDll ; if( ( hDll = LoadLibrary( TEXT("kitty.dll") ) ) != NULL ) hInstIcons = hDll ; }
		// No external icon DLL: fall back to icons embedded in the executable itself
		if( hInstIcons==NULL ) hInstIcons = GetModuleHandle( NULL ) ;
		}

	NETDBG_TS("after icon-dll init");
	// Teste la presence d'une note et l'affiche
	if( GetValueData( HKEY_CURRENT_USER, TEXT(PUTTY_REG_POS), "Notes", buffer ) )
		{ if( strlen( buffer ) > 0 ) MessageBox( NULL, buffer, "Notes", MB_OK ) ; }
		
	// Genere un fichier (4096ko max) d'initialisation de toute les Sessions
	snprintf( buffer, sizeof(buffer), "%s\\%s.ses.updt", InitialDirectory, appname ) ;
	if( existfile( buffer ) ) { InitAllSessions( HKEY_CURRENT_USER, TEXT(PUTTY_REG_POS), "Sessions", buffer ) ; }
	/* Format: registry like => UTF-8 encoded !!!
	"ProxyUsername"="mylogin"
	"ProxyPassword"="mypassword"
	*/
	
	// Initialise les logs
	char hostname[4096], username[4096] ;
	NETDBG_TS("before GetUserName/GetComputerName");
	i = sizeof(username) ;
	GetUserName( username, (void*)&i ) ;
	i = 4095 ;
	GetComputerName( hostname, (void*)&i ) ;
	NETDBG_TS("after GetUserName/GetComputerName");
	snprintf( buffer, sizeof(buffer), "Starting %ld from %s@%s", GetCurrentProcessId(), username, hostname ) ;
	debug_logevent(buffer) ;
	NETDBG_TS("InitWinMain: return");
}


/* Pour compilation 64bits */
/*
void bzero (void *s, size_t n){ memset (s, 0, n); }
void bcopy (const void *src, void *dest, size_t n){ memcpy (dest, src, n); }
int bcmp (const void *s1, const void *s2, size_t n){ return memcmp (s1, s2, n); }
*/



// Commandes internes
int InternalCommand( HWND hwnd, char * st ) ;

// Positionne le repertoire ou se trouve la configuration 
void SetConfigDirectory( const char * Directory ) ;

// Creation du fichier kitty.ini par defaut si besoin
void CreateDefaultIniFile( void ) ;

// Initialisation des parametres a partir du fichier kitty.ini
void LoadParameters( void ) ;

// Initialisation de noms de fichiers de configuration kitty.ini et kitty.sav
void InitNameConfigFile( void ) ;

// Ecriture de l'increment de compteurs
void WriteCountUpAndPath( void ) ;

// Initialisation spécifique a KiTTY
void InitWinMain( void ) ;

// Initialisation des shortcuts


// Gestion des raccourcis
int ManageShortcuts( Terminal *term, Conf *conf, HWND hwnd, const int* clips_system, int key_num, int shift_flag, int control_flag, int alt_flag, int altgr_flag, int win_flag ) ;

// Nettoie la clé de PuTTY pour enlever les clés et valeurs spécifique à KiTTY
// Se trouve dans le fichier kitty_registry.c
BOOL RegCleanPuTTY( void ) ;

// Envoi de caractères
void SendKeyboardPlus( HWND hwnd, const char * st ) ;

// Envoi d'une commande à l'écran
void SendAutoCommand( HWND hwnd, const char * cmd ) ;
