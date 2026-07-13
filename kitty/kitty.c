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

// Pointeur sur la commande a passer ligne a ligne
char * PasteCommand = NULL ;

// Flag pour utiliser la commande paste ligne a ligne (shift+bouton droit au lieu de bouton droit seul) dans le cas de serveur lent
static int PasteCommandFlag = 0 ;
int GetPasteCommandFlag(void) { return PasteCommandFlag ; }

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

// Flag de gestion de la fonction "rutty" (script automatique)
static int RuttyFlag = 1 ;
int GetRuttyFlag(void) { return RuttyFlag ; } 
void SetRuttyFlag( const int flag ) { RuttyFlag = flag ; }

// Flag de gestion de la Transparence
// The feature stays available (so transparency is configurable PER SESSION via the
// Window > Transparency panel), but it is OFF by default because the per-session
// TransparencyValue defaults to 0 = fully opaque / non-layered (see conf.h):
// kitty_apply_transparency() applies nothing until a session sets a value > 0.
// kitty.ini [KiTTY] transparency=no remains the master switch that removes the
// whole feature (config panel + system-menu adjust items).
#ifdef FLJ
static int TransparencyFlag = 1 ;
#else
static int TransparencyFlag = 1 ;
#endif
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
#if (defined MOD_PERSO) && (!defined FLJ)
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
#ifdef FLJ
static int ShortcutsFlag = 0 ;
#else
static int ShortcutsFlag = 1 ;
#endif
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

// Flag pour l'affichage de la taille de la fenetre
static int SizeFlag = 0 ;
int GetSizeFlag(void) { return SizeFlag ; }
void SetSizeFlag( const int flag ) { SizeFlag = flag ; }

// Flag pour imposer le passage en majuscule
static int CapsLockFlag = 0 ;
int GetCapsLockFlag(void) { return CapsLockFlag ; }
void SetCapsLockFlag( const int flag ) { CapsLockFlag = flag ; }

// Flag pour gerer la presence de la barre de titre
static int TitleBarFlag = 1 ;
int GetTitleBarFlag(void) { return TitleBarFlag ; }
void SetTitleBarFlag( const int flag ) { TitleBarFlag = flag ; }

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

// Flag pour retourner à la Config Box en fin d'execution
static int ConfigBoxNoExitFlag = 0 ;
int GetConfigBoxNoExitFlag(void) { return ConfigBoxNoExitFlag ; }
void SetConfigBoxNoExitFlag( const int flag ) { ConfigBoxNoExitFlag = flag ; }

// ConfigBox X-position
static int ConfigBoxLeft = -1 ;
int GetConfigBoxLeft() { return ConfigBoxLeft ; }
void SetConfigBoxLeft( const int val ) { ConfigBoxLeft = val ; }

// ConfigBox Y-position
static int ConfigBoxTop = -1 ;
int GetConfigBoxTop() { return ConfigBoxTop ; }
void SetConfigBoxTop( const int val ) { ConfigBoxTop= val ; }

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

// Flag pour inhiber le comportement ou toutes les sessions appartiennent au folder defaut
static int SessionsInDefaultFlag = 1 ;
int GetSessionsInDefaultFlag(void) { return SessionsInDefaultFlag ; }
void SetSessionsInDefaultFlag( const int flag ) { SessionsInDefaultFlag = flag ; }

// Flag pour inhiber la creation automatique de la session Default Settings
// [ConfigBox] defaultsettings=yes
static int DefaultSettingsFlag = 1 ;
int GetDefaultSettingsFlag(void) { return DefaultSettingsFlag ; }
void SetDefaultSettingsFlag( const int flag ) { DefaultSettingsFlag = flag ; }

// Flag pour définir l'action a executer sur un double clic sur une session de la liste des sessions
// [ConfigBox] dblclick=open
static int DblClickFlag = 0 ;
int GetDblClickFlag(void) { return DblClickFlag ; }
void SetDblClickFlag( const int flag ) { DblClickFlag = flag ; }

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

// Nombre de clignotements max de l'icone dans le systeme tray lors de la reception d'un BELL
int MaxBlinkingTime = 0 ;

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

// Chemin vers le programme plink.exe
char * PlinkPath = NULL ;

// Repertoire de lancement
char InitialDirectory[4096]="" ;

// Chemin complet des fichiers de configuration kitty.ini et kitty.sav
static char * KittyIniFile = NULL ;
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
#if (defined MOD_BACKGROUNDIMAGE) && (!defined FLJ)
#define TIMER_SLIDEBG 8703
#endif
#define TIMER_REDRAW 8704
#define TIMER_AUTOPASTE 8705
#define TIMER_BLINKTRAYICON 8706
#define TIMER_LOGROTATION 8707
#define TIMER_ANTIIDLE 8708

/*
#define TIMER_INIT 12341
#define TIMER_AUTOCOMMAND 12342
#if (defined MOD_BACKGROUNDIMAGE) && (!defined FLJ)
#define TIMER_SLIDEBG 12343
#endif
#define TIMER_REDRAW 12344
#define TIMER_AUTOPASTE 12345
#define TIMER_BLINKTRAYICON 12346
#define TIMER_LOGROTATION 12347
*/


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

// Gestion des raccourcis
#define SHIFTKEY 500
#define CONTROLKEY 1000
#define ALTKEY 2000
#define ALTGRKEY 4000
#define WINKEY 8000

// F1=0x70 (112) => F12=0x7B (123)
	
static struct TShortcuts {
	int autocommand ;
	int command ;
	int editor ;
	int editorclipboard ;
	int getfile ;
	int imagechange ;
	int input ;
	int inputm ;
	int print ;
	int printall ;
	int protect ;
	int script ;
	int sendfile ;
	int rollup ;
	int tray ;
	int viewer ;
	int visible ;
	int winscp ;
	int switchlogmode ;
	int showportforward ;
	int resetterminal ;
	int duplicate ;
	int opennew ;
	int opennewcurrent ;
	int changesettings ;
	int clearscrollback ;
	int clearlogfile ;
	int closerestart ;
	int eventlog ;
	int fullscreen ;
	int fontup ;
	int fontdown ;
	int copyall ;
	int fontnegative ;
	int fontblackandwhite ;
	int keyexchange ;
	} shortcuts_tab ;

static int NbShortCuts = 0 ;
static struct TShortcuts2 { int num ; char * st ; } shortcuts_tab2[512] ;

static char * InputBoxResult = NULL ;

// Procedure de debug
void debug_log( const char *fmt, ... ) {
	char filename[4096]="" ;
	va_list ap;
	FILE *fp ;

	if( (InitialDirectory!=NULL) && (strlen(InitialDirectory)>0) )
		sprintf(filename,"%s\\kitty.log",InitialDirectory);
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
	// else if( !stricmp( val, "AUTOSTORESSHKEY" ) ) return AutoStoreSSHKeyFlag ;
	// else if( !stricmp( val, "CONFIGBOXWINDOWHEIGHT" ) ) return ConfigBoxWindowHeight ;
	// else if( !stricmp( val, "NUMBEROFICONS" ) ) return NumberOfIcons ;	// ==> Remplace par GetNumberOfIcons()
	// else if( !stricmp( val, "ICON" ) ) return IconeFlag ; // ==> Remplace par GetIconeFlag()
	// else if( !stricmp( val, "SESSIONFILTER" ) ) return SessionFilterFlag ;
	return 0 ;
	}

#if (defined MOD_BACKGROUNDIMAGE) && (!defined FLJ)
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

		sprintf( buffer, "%s\\\\Sessions", PUTTY_REG_POS );
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
					sprintf( nValue, "%s\\%s", buffer, achKey ) ;
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
		sprintf( buffer, "%s\\Sessions", ConfigDirectory ) ;
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
	if( !strcmp(subdir,"") ) sprintf( buffer, "%s\\Sessions", ConfigDirectory ) ;
	else sprintf(buffer,"%s\\Sessions\\%s",ConfigDirectory, subdir ) ;
	if( (dir=opendir(buffer))!=NULL ) {
		while( (de=readdir(dir)) != NULL ) 
			if( strcmp(de->d_name,".") && strcmp(de->d_name,"..") )	{
				if( !strcmp(subdir,"") ) sprintf(buf,"%s\\Sessions\\%s",ConfigDirectory,de->d_name ) ;
				else sprintf(buf,"%s\\Sessions\\%s\\%s",ConfigDirectory, subdir,de->d_name ) ;
				if( existdirectory( buf ) ) {
					if( !strcmp(subdir,"") ) sprintf( buf, "%s", de->d_name ) ;
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
		sprintf( buffer, "%s\\Sessions\\%s", PUTTY_REG_POS, session ) ;
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
			sprintf(buffer,"%s\\Sessions\\%s", ConfigDirectory, session );
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
	sprintf( buffer, "%s\\Sessions\\%s", PUTTY_REG_POS, session ) ;
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
			if( !strcmp(folder,"Default") || !strcmp(folder,"") ) sprintf(buffer,"%s\\Sessions\\%s", ConfigDirectory, session ) ;
			else sprintf(buffer,"%s\\Sessions\\%s\\%s", ConfigDirectory, folder, session ) ;
			}
		else {
			sprintf(buffer,"%s\\Sessions\\%s", ConfigDirectory, session ) ;
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
	sprintf( str, "[HKEY_CURRENT_USER\\%s]", TEXT(lpSubKey) ) ;
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
						sprintf( str, "\"%s\"=dword:%08x", achValue, (unsigned int) *((DWORD*)lpData) ) ; // Ca ca marchait bien mais avec une erreur de compilation
						break;
					case REG_EXPAND_SZ:
					case REG_MULTI_SZ:
					case REG_SZ:
						//fprintf( fp_out, "\"" ) ;
						sprintf( str, "\"%s\"=\"", achValue ) ;
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

// Permet de recuperer les sessions KiTTY dans PuTTY  (PUTTY_REG_POS)
void RepliqueToPuTTY( LPCTSTR Key ) { 
	char buffer[1024] ;
#ifdef FLJ
return ;
#endif
	if( IniFileFlag == SAVEMODE_REG )
	if( readINI( KittyIniFile, "PuTTY", "keys", buffer, sizeof(buffer) ) ) {
		str_rtrim( buffer, "\n\r \t" ) ;
		if( !stricmp( buffer, "load" ) ) {
			sprintf( buffer, "%s\\Sessions", Key ) ;
			RegDelTree (HKEY_CURRENT_USER, "Software\\SimonTatham\\PuTTY\\Sessions" ) ;
			kitty_RegCopyTree( HKEY_CURRENT_USER, buffer, "Software\\SimonTatham\\PuTTY\\Sessions" ) ;
			sprintf( buffer, "%s\\SshHostKeys", Key ) ;
			kitty_RegCopyTree( HKEY_CURRENT_USER, buffer, "Software\\SimonTatham\\PuTTY\\SshHostKeys" ) ;
			}
		//delINI( KittyIniFile, "PuTTY", "keys" ) ;
		RegCleanPuTTY() ;
		}
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
	sprintf( buffer, "%ld", n ) ;
	WriteParameter( INIT_SECTION, "KiCount", buffer) ;
	
	if( ReadParameterN( INIT_SECTION, "KiLastUp", buffer, sizeof(buffer) ) == 0 ) { sprintf( buffer, "%ld/", time(0) ) ; }
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
		sprintf( buffer, "%s\\Sessions", PUTTY_REG_POS ) ;
		n = (long int) RegCountKey( HKEY_CURRENT_USER, buffer ) ;
		sprintf( buffer, "%ld", n ) ;
		WriteParameter( INIT_SECTION, "KiSess", buffer) ;
	} else {
		sprintf( buffer, "0 (Not in registry mode)" ) ;
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
		sprintf( buffer, "%s\\%s", TEXT(PUTTY_REG_PARENT), key ) ;
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
	sprintf( buffer, "%s\\%s", TEXT(PUTTY_REG_PARENT), key ) ;
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
				if( InputBoxResult == NULL ) exit(0) ;
				if( strlen( InputBoxResult ) == 0 ) exit(0) ;
				strcpy( PasswordConf, InputBoxResult ) ;
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
						sprintf( buffer, "Loading %s", KeyName ) ;
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
	
	char buffer[MAX_PATH] ; sprintf( buffer, "%s\\kchat.dll", InitialDirectory ) ;
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
				sprintf( stb, "0x%c%c", st[i+2],st[i+3] ) ;
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
						sprintf( stb, "%c", j ) ;
						SendStrToTerminal( stb, 1 ) ;
						//SendMessage(hwnd, WM_CHAR, j, 0) ;
					}
				}
				Sleep( internal_delay ) ;
				buffer[0] = '\0' ; j = 0 ;
				i++ ; i++ ; i++ ;
			} else if( st[i+1] == 'x' ) {
				SendKeyboard( hwnd, buffer ) ;
				sprintf( stb, "0x%c%c", st[i+2],st[i+3] ) ;
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
                sprintf(portbuf, "%d", conf_get_int(conf, CONF_port));
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

// Envoi d'un fichier par SCP vers la racine du compte
int SearchPSCP( void ) ;
/* KiTTY security: launch a console command line WITHOUT a shell. Replaces
 * system()/"start" for the pscp/plink command builders below, so session fields
 * spliced into the command line cannot inject shell commands - CreateProcess does
 * NOT run cmd.exe, so & | > ` and friends are taken literally (kills the shell
 * command-injection class). A new console is created (pscp/plink are console
 * tools); `wait` blocks until exit (the old inline system() behaviour) or returns
 * immediately (the old "start" new-window behaviour). Returns 0 on success.
 * NOTE (staged follow-up): the callers still build the command line by unbounded
 * strcat of session fields and only wrap them in "..."; a field containing a
 * double-quote can still inject extra *arguments* (not shell commands), and an
 * over-long field can still overflow buffer[4096]. Bounding (strbuf) + argv
 * quoting is the next step; this change removes the shell/RCE exposure. */
static int kitty_run_noshell( char *cmdline, int wait ) {
	STARTUPINFOA si ; PROCESS_INFORMATION pi ;
	memset( &si, 0, sizeof(si) ) ; si.cb = sizeof(si) ;
	memset( &pi, 0, sizeof(pi) ) ;
	if( !CreateProcessA( NULL, cmdline, NULL, NULL, FALSE,
	                     CREATE_NEW_CONSOLE, NULL, NULL, &si, &pi ) )
		return -1 ;
	if( wait ) WaitForSingleObject( pi.hProcess, INFINITE ) ;
	CloseHandle( pi.hThread ) ; CloseHandle( pi.hProcess ) ;
	return 0 ;
}

/* Watch a launched transfer process (pscp/plink) on a background thread: wait
 * for it, and on a NON-zero exit pop a dialog with the exit code + a hint. Runs
 * off the GUI thread so a long transfer never freezes KiTTY, and the visible
 * console (CREATE_NEW_CONSOLE below) still shows live progress. Never shows the
 * command line (it carries -pw). */
/* Transient system-tray balloon (non-modal, auto-dismiss): add a short-lived
 * notify icon on `hwnd`, fire the balloon, keep it alive briefly, then remove
 * it. Used for the file-transfer SUCCESS notice. Safe if hwnd is gone (the
 * Shell_NotifyIcon calls just fail). Runs on the watcher thread (the Sleep is
 * off the GUI thread). */
static void kitty_tray_balloon( HWND hwnd, const char *title, const char *msg ) {
	static volatile LONG s_uid = 0xC000 ;
	NOTIFYICONDATA nid ;
	memset( &nid, 0, sizeof(nid) ) ;
	nid.cbSize = sizeof(nid) ;
	nid.hWnd = hwnd ;
	nid.uID = (UINT)InterlockedIncrement( &s_uid ) ;
	nid.uFlags = NIF_ICON | NIF_INFO ;
	nid.hIcon = LoadIcon( NULL, IDI_INFORMATION ) ;
	nid.dwInfoFlags = NIIF_INFO ;
	strncpy( nid.szInfoTitle, title, sizeof(nid.szInfoTitle)-1 ) ;
	strncpy( nid.szInfo,      msg,   sizeof(nid.szInfo)-1 ) ;
	if( Shell_NotifyIcon( NIM_ADD, &nid ) ) {
		Sleep( 8000 ) ;   /* keep the icon present while the balloon is shown */
		Shell_NotifyIcon( NIM_DELETE, &nid ) ;
	}
}

/* Fire the success tray balloon on its own short-lived thread (kitty_tray_balloon
 * Sleeps to keep the icon alive, so it must NOT run on the GUI thread). */
struct ktx_balloon { HWND hwnd ; char *title ; char *msg ; } ;
static DWORD WINAPI ktx_balloon_thread( LPVOID p ) {
	struct ktx_balloon *b = (struct ktx_balloon *)p ;
	kitty_tray_balloon( b->hwnd, b->title, b->msg ) ;
	sfree( b->title ) ; sfree( b->msg ) ; free( b ) ;
	return 0 ;
}
static void kitty_tray_balloon_async( HWND hwnd, const char *title, const char *msg ) {
	struct ktx_balloon *b = (struct ktx_balloon *)malloc( sizeof(*b) ) ;
	if( !b ) return ;
	b->hwnd = hwnd ; b->title = dupstr(title) ; b->msg = dupstr(msg) ;
	HANDLE t = CreateThread( NULL, 0, ktx_balloon_thread, b, 0, NULL ) ;
	if( t ) CloseHandle( t ) ; else { sfree(b->title) ; sfree(b->msg) ; free(b) ; }
}

/* ---- KiTTY file-transfer window -----------------------------------------
 * Runs pscp with its output captured to a pipe (no shell -> injection
 * hardening preserved) and streams it LIVE into a scrollable window. On
 * SUCCESS the window auto-closes and a tray balloon pops; on FAILURE the
 * window STAYS OPEN showing the full error context (so the user can read /
 * copy it), with a Close button. Never displays the command line (it carries
 * -pw). The window runs on the GUI thread; a reader thread pumps output to it
 * via posted messages, so KiTTY never blocks during a transfer. */
#define KTX_WM_APPEND (WM_APP+11)
#define KTX_WM_DONE   (WM_APP+12)
#define KTX_ID_EDIT   2001
#define KTX_ID_CLOSE  2002
struct ktx_win {
	HWND hwnd, edit, closebtn, parent ;
	HANDLE proc, rd, thread ;
	char *what ;
	int done ;
	int cancelled ;
	/* mini line-discipline so pscp's \r progress meter overwrites the current
	 * line in place (terminal-style) instead of stacking new lines: */
	char curline[2048] ;   /* current uncommitted line */
	int  curcol ;          /* write cursor within curline (\r resets to 0) */
	int  curlen ;          /* length of curline */
	int  committed ;       /* edit-control char index where curline begins */
} ;

/* Make the edit-control tail (from w->committed to end) equal curline[0..curlen]. */
static void ktx_set_curline( struct ktx_win *w ) {
	char save = w->curline[w->curlen] ; w->curline[w->curlen] = 0 ;
	SendMessageA( w->edit, EM_SETSEL, w->committed, -1 ) ;
	SendMessageA( w->edit, EM_REPLACESEL, FALSE, (LPARAM)w->curline ) ;
	w->curline[w->curlen] = save ;
}

/* Feed captured pscp bytes through a tiny line-discipline: '\r' returns the
 * cursor to column 0 (so the progress meter overwrites its line in place),
 * '\n' commits the line and starts a new one, tabs expand, other control
 * chars are dropped. Keeps the live transfer readable as one updating line. */
static void ktx_feed( struct ktx_win *w, const char *s, int len ) {
	int i ;
	for( i=0 ; i<len ; i++ ) {
		char c = s[i] ;
		if( c=='\r' ) {
			w->curcol = 0 ;
		} else if( c=='\n' ) {
			ktx_set_curline( w ) ;
			int n = GetWindowTextLength( w->edit ) ;
			SendMessageA( w->edit, EM_SETSEL, n, n ) ;
			SendMessageA( w->edit, EM_REPLACESEL, FALSE, (LPARAM)"\r\n" ) ;
			w->committed = n + 2 ;
			w->curcol = 0 ; w->curlen = 0 ; w->curline[0] = 0 ;
		} else if( c=='\b' ) {
			if( w->curcol>0 ) w->curcol-- ;
		} else if( c=='\t' ) {
			do {
				if( w->curcol < (int)sizeof(w->curline)-1 ) {
					w->curline[w->curcol++] = ' ' ;
					if( w->curcol > w->curlen ) w->curlen = w->curcol ;
				}
			} while( (w->curcol % 8) && w->curcol < (int)sizeof(w->curline)-1 ) ;
		} else if( (unsigned char)c >= 0x20 ) {
			if( w->curcol < (int)sizeof(w->curline)-1 ) {
				w->curline[w->curcol++] = c ;
				if( w->curcol > w->curlen ) w->curlen = w->curcol ;
			}
		}
	}
	ktx_set_curline( w ) ;
	SendMessageA( w->edit, EM_SCROLLCARET, 0, 0 ) ;
}

static DWORD WINAPI ktx_reader_thread( LPVOID param ) {
	struct ktx_win *w = (struct ktx_win *)param ;
	char buf[4096] ; DWORD nrd ; DWORD code = (DWORD)-1 ;
	while( ReadFile( w->rd, buf, sizeof(buf), &nrd, NULL ) && nrd>0 ) {
		char *chunk = (char *)malloc( nrd ) ;
		if( chunk ) { memcpy( chunk, buf, nrd ) ;
			PostMessage( w->hwnd, KTX_WM_APPEND, (WPARAM)nrd, (LPARAM)chunk ) ; }
	}
	CloseHandle( w->rd ) ; w->rd = NULL ;
	WaitForSingleObject( w->proc, INFINITE ) ;
	GetExitCodeProcess( w->proc, &code ) ;
	/* leave w->proc open: the GUI thread owns it (for Cancel/TerminateProcess)
	 * and closes it in WM_DESTROY. We don't touch it again after this. */
	PostMessage( w->hwnd, KTX_WM_DONE, (WPARAM)code, 0 ) ;
	return 0 ;
}

static LRESULT CALLBACK ktx_wndproc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp ) {
	struct ktx_win *w = (struct ktx_win *)GetWindowLongPtr( hwnd, GWLP_USERDATA ) ;
	switch( msg ) {
	  case WM_CREATE: {
		CREATESTRUCT *cs = (CREATESTRUCT *)lp ;
		w = (struct ktx_win *)cs->lpCreateParams ;
		SetWindowLongPtr( hwnd, GWLP_USERDATA, (LONG_PTR)w ) ;
		w->hwnd = hwnd ;
		w->edit = CreateWindowEx( WS_EX_CLIENTEDGE, "EDIT", "",
			WS_CHILD|WS_VISIBLE|WS_VSCROLL|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL,
			0,0,0,0, hwnd, (HMENU)(UINT_PTR)KTX_ID_EDIT, GetModuleHandle(NULL), NULL ) ;
		SendMessage( w->edit, WM_SETFONT, (WPARAM)GetStockObject(ANSI_FIXED_FONT), TRUE ) ;
		SendMessage( w->edit, EM_LIMITTEXT, (WPARAM)0x200000, 0 ) ;
		w->closebtn = CreateWindow( "BUTTON", "&Cancel",
			WS_CHILD|WS_VISIBLE|BS_DEFPUSHBUTTON,
			0,0,0,0, hwnd, (HMENU)(UINT_PTR)KTX_ID_CLOSE, GetModuleHandle(NULL), NULL ) ;
		return 0 ;
	  }
	  case WM_SIZE: {
		RECT rc ; GetClientRect( hwnd, &rc ) ;
		int pad=8, bh=26, bw=90 ;
		MoveWindow( w->edit, pad, pad, rc.right-2*pad, rc.bottom-bh-3*pad, TRUE ) ;
		MoveWindow( w->closebtn, rc.right-bw-pad, rc.bottom-bh-pad, bw, bh, TRUE ) ;
		return 0 ;
	  }
	  case KTX_WM_APPEND: {
		char *chunk = (char *)lp ;
		if( w && chunk ) ktx_feed( w, chunk, (int)wp ) ;
		if( chunk ) free( chunk ) ;
		return 0 ;
	  }
	  case KTX_WM_DONE: {
		DWORD code = (DWORD)wp ;
		if( !w ) return 0 ;
		w->done = 1 ;
		const char *what = w->what ? w->what : "Transfer" ;
		if( code == 0 && !w->cancelled ) {
			char *m = dupprintf( "%s complete.", what ) ;
			kitty_tray_balloon_async( w->parent, "KiTTY transfer", m ) ;
			sfree( m ) ;
			DestroyWindow( hwnd ) ;   /* success: auto-close, balloon confirms */
		} else {
			char *m ;
			if( w->cancelled ) {
				m = dupprintf( "\r\n==== %s cancelled ====\r\n", what ) ;
				SetWindowTextA( hwnd, "KiTTY transfer - cancelled" ) ;
			} else {
				const char *hint = ( code==127 )
					? "\r\n\r\nExit 127 = the server could not start the SCP/SFTP "
					  "subsystem (command not found). Try switching the transfer "
					  "protocol (Connection -> SSH -> PSCP and WinSCP) between SCP "
					  "and SFTP, or check the server's sftp-server/scp."
					: "" ;
				m = dupprintf( "\r\n==== %s FAILED  (pscp exit code %lu) ====%s\r\n",
				               what, (unsigned long)code, hint ) ;
				char *t = dupprintf( "KiTTY transfer - FAILED (exit %lu)", (unsigned long)code ) ;
				SetWindowTextA( hwnd, t ) ; sfree( t ) ;
			}
			ktx_feed( w, m, (int)strlen(m) ) ; sfree( m ) ;
			SetWindowTextA( w->closebtn, "&Close" ) ;
			EnableWindow( w->closebtn, TRUE ) ;
			SetForegroundWindow( hwnd ) ;
			SetFocus( w->closebtn ) ;
		}
		return 0 ;
	  }
	  case WM_COMMAND:
		if( LOWORD(wp)==KTX_ID_CLOSE && w ) {
			if( w->done ) {
				DestroyWindow( hwnd ) ;          /* finished -> button is "Close" */
			} else if( !w->cancelled ) {
				w->cancelled = 1 ;               /* running -> button is "Cancel": kill pscp */
				if( w->proc ) TerminateProcess( w->proc, 2 ) ;
				SetWindowTextA( w->closebtn, "Stopping..." ) ;
				EnableWindow( w->closebtn, FALSE ) ;
			}
			return 0 ;
		}
		break ;
	  case WM_CLOSE:
		if( w && !w->done ) {                     /* X mid-transfer: cancel, wait for DONE */
			if( !w->cancelled ) {
				w->cancelled = 1 ;
				if( w->proc ) TerminateProcess( w->proc, 2 ) ;
				SetWindowTextA( w->closebtn, "Stopping..." ) ;
				EnableWindow( w->closebtn, FALSE ) ;
			}
			return 0 ;
		}
		DestroyWindow( hwnd ) ;
		return 0 ;
	  case WM_DESTROY:
		if( w ) {
			if( w->proc ) CloseHandle( w->proc ) ;
			if( w->thread ) CloseHandle( w->thread ) ;
			if( w->what ) sfree( w->what ) ;
			free( w ) ;
			SetWindowLongPtr( hwnd, GWLP_USERDATA, 0 ) ;
		}
		return 0 ;
	}
	return DefWindowProc( hwnd, msg, wp, lp ) ;
}

/* Launch pscp into a transfer window (above). No shell. Returns 0 if launched. */
static int kitty_run_xfer( HWND parent, char *cmdline, const char *what ) {
	static int registered = 0 ;
	HINSTANCE hi = GetModuleHandle( NULL ) ;
	if( !registered ) {
		WNDCLASS wc ; memset( &wc, 0, sizeof(wc) ) ;
		wc.lpfnWndProc = ktx_wndproc ;
		wc.hInstance = hi ;
		wc.hCursor = LoadCursor( NULL, IDC_ARROW ) ;
		wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE+1) ;
		wc.hIcon = LoadIcon( NULL, IDI_APPLICATION ) ;
		wc.lpszClassName = "KiTTYxferwin" ;
		RegisterClass( &wc ) ;
		registered = 1 ;
	}
	SECURITY_ATTRIBUTES sa ; HANDLE rd=NULL, wr=NULL ;
	memset( &sa, 0, sizeof(sa) ) ; sa.nLength = sizeof(sa) ; sa.bInheritHandle = TRUE ;
	if( !CreatePipe( &rd, &wr, &sa, 0 ) ) return -1 ;
	SetHandleInformation( rd, HANDLE_FLAG_INHERIT, 0 ) ;
	STARTUPINFOA si ; PROCESS_INFORMATION pi ;
	memset( &si, 0, sizeof(si) ) ; si.cb = sizeof(si) ;
	si.dwFlags = STARTF_USESTDHANDLES ;
	si.hStdOutput = wr ; si.hStdError = wr ; si.hStdInput = GetStdHandle( STD_INPUT_HANDLE ) ;
	memset( &pi, 0, sizeof(pi) ) ;
	if( !CreateProcessA( NULL, cmdline, NULL, NULL, TRUE,
	                     CREATE_NO_WINDOW, NULL, NULL, &si, &pi ) ) {
		CloseHandle( rd ) ; CloseHandle( wr ) ;
		MessageBox( NULL, "Could not launch the transfer client (pscp).",
		            "KiTTY transfer", MB_OK|MB_ICONERROR ) ;
		return -1 ;
	}
	CloseHandle( wr ) ; CloseHandle( pi.hThread ) ;
	struct ktx_win *w = (struct ktx_win *)malloc( sizeof(*w) ) ;
	if( !w ) { CloseHandle( pi.hProcess ) ; CloseHandle( rd ) ; return -1 ; }
	memset( w, 0, sizeof(*w) ) ;
	w->parent = parent ; w->proc = pi.hProcess ; w->rd = rd ;
	w->what = dupstr( what ? what : "Transfer" ) ;
	char *title = dupprintf( "KiTTY transfer - %s", w->what ) ;
	HWND hwnd = CreateWindow( "KiTTYxferwin", title,
		WS_OVERLAPPEDWINDOW|WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 680, 420,
		parent, NULL, hi, w ) ;
	sfree( title ) ;
	if( !hwnd ) { CloseHandle( pi.hProcess ) ; CloseHandle( rd ) ; sfree( w->what ) ; free( w ) ; return -1 ; }
	SetForegroundWindow( hwnd ) ;          /* bring the transfer window to the front */
	BringWindowToTop( hwnd ) ;
	w->thread = CreateThread( NULL, 0, ktx_reader_thread, w, 0, NULL ) ;
	return 0 ;
}

/* Bounded string append: never writes past dst[cap-1], always NUL-terminates,
 * silently truncates rather than overflowing. Replaces the unbounded strcat()s
 * in the pscp/plink command builders. */
static void bcat( char *dst, size_t cap, const char *s ) {
	if( !s || cap==0 ) return ;
	size_t dl = strlen(dst) ;
	if( dl >= cap-1 ) return ;
	size_t room = cap-1-dl, sl = strlen(s) ;
	if( sl > room ) sl = room ;
	memcpy( dst+dl, s, sl ) ; dst[dl+sl] = '\0' ;
}

/* Append ONE argument, wrapped in double-quotes and escaped per Windows
 * CommandLineToArgvW rules, so a value containing a quote or space cannot inject
 * extra command-line arguments/switches. Bounded via bcat. Use for single-value
 * fields (password, key/file path, source path, the user@host:dir target); do NOT
 * use for fields that are intentionally raw option strings (pscpoptions etc.). */
static void qcat( char *dst, size_t cap, const char *s ) {
	char one[2] = {0,0} ;
	bcat( dst, cap, "\"" ) ;
	if( s ) {
		size_t nbs = 0 ;   /* run of pending backslashes */
		for( const char *p = s ; *p ; p++ ) {
			if( *p == '\\' ) { nbs++ ; }
			else if( *p == '"' ) {
				for( size_t k=0 ; k<2*nbs+1 ; k++ ) bcat(dst,cap,"\\") ;
				bcat( dst, cap, "\"" ) ; nbs = 0 ;
			} else {
				for( size_t k=0 ; k<nbs ; k++ ) bcat(dst,cap,"\\") ;
				nbs = 0 ; one[0]=*p ; bcat( dst, cap, one ) ;
			}
		}
		for( size_t k=0 ; k<2*nbs ; k++ ) bcat(dst,cap,"\\") ;   /* trailing backslashes before closing quote */
	}
	bcat( dst, cap, "\"" ) ;
}

/* #535: percent-encode and append, for the userinfo (user/password) of a
 * WinSCP "proto://user:pass@host:port/dir" URL. Without this, an '@' '/' ':' or
 * '?' in the password or username is parsed as URL grammar by WinSCP -- e.g. a
 * password "x@evil.host/" redirects the connection to an attacker-chosen host.
 * Everything outside the RFC 3986 "unreserved" set is escaped as %XX. Bounded
 * via bcat. (qcat is Win32 argv quoting -- a different layer -- so it does NOT
 * cover this URL-grammar injection.) */
static void urlcat( char *dst, size_t cap, const char *s ) {
	static const char hexd[] = "0123456789ABCDEF" ;
	char esc[4] = {0,0,0,0} ;
	if( !s ) return ;
	for( const unsigned char *p = (const unsigned char *)s ; *p ; p++ ) {
		unsigned char c = *p ;
		if( (c>='A'&&c<='Z') || (c>='a'&&c<='z') || (c>='0'&&c<='9')
		    || c=='-' || c=='.' || c=='_' || c=='~' ) {
			esc[0]=(char)c ; esc[1]='\0' ; bcat( dst, cap, esc ) ;
		} else {
			esc[0]='%' ; esc[1]=hexd[(c>>4)&0xF] ; esc[2]=hexd[c&0xF] ; esc[3]='\0' ;
			bcat( dst, cap, esc ) ;
		}
	}
}

void SendOneFile( HWND hwnd, char * directory, char * filename, char * distantdir) {
	char buffer[4096], pscppath[4096]="", pscpport[4096]="22", remotedir[4096]=".",dir[4096], b1[256] ;
	int p ;
	
	if( distantdir == NULL ) { distantdir = kitty_current_dir() ; } 
	if( PSCPPath==NULL ) {
		if( IniFileFlag == SAVEMODE_REG ) return ;
		else if( !SearchPSCP() ) return ;
	}
	if( !existfile( PSCPPath ) ) {
		if( IniFileFlag == SAVEMODE_REG ) return ;
		else if( !SearchPSCP() ) return ;
	}
		
	if( !GetShortPathName( PSCPPath, pscppath, 4095 ) ) return ;
	
	if( ReadParameterN( INIT_SECTION, "uploaddir", dir, sizeof(dir) ) ) {
		if( !existdirectory( dir ) ) 
			strcpy( dir, InitialDirectory ) ;
	}
	if (strlen( dir ) == 0) strcpy( dir, InitialDirectory ) ;

	if( (distantdir != NULL ) && ( strlen(distantdir)>0 ) ) {
		strcpy( remotedir, distantdir ) ;
	} else if( strlen(conf_get_str(conf,CONF_pscpremotedir))>0 ) {
		strcpy( remotedir, conf_get_str(conf,CONF_pscpremotedir) ) ;
	} else { strcpy( remotedir, "." ) ; 
	}
	if( strlen( remotedir ) == 0 ) strcpy( remotedir, "." ) ;

	buffer[0] = '\0' ;
	const size_t BC = sizeof(buffer) ;

	bcat( buffer, BC, pscppath ) ; bcat( buffer, BC, " " ) ;   /* exe: 8.3 path, no spaces/quotes */

	if( strlen(conf_get_str(conf, CONF_pscpoptions))>0 ) {     /* raw user options - intentionally unquoted */
		bcat( buffer, BC, conf_get_str(conf, CONF_pscpoptions) ) ; bcat( buffer, BC, " " ) ;
	}
	bcat( buffer, BC, conf_get_int(conf, CONF_winscpprot)==0 ? "-scp " : "-sftp " ) ;

	if( ReadParameterN( INIT_SECTION, "pscpport", pscpport, sizeof(pscpport) ) ) {
		pscpport[17]='\0';
		if( !strcmp( pscpport,"*" ) ) sprintf( pscpport, "%d", conf_get_int(conf, CONF_port) ) ;
		bcat( buffer, BC, "-P " ) ; bcat( buffer, BC, pscpport ) ; bcat( buffer, BC, " " ) ;
	} else {
		if( (p=poss(":",conf_get_str(conf, CONF_sftpconnect) )) > 0 ) {
			sprintf( b1, "-P %d ", atoi(conf_get_str(conf, CONF_sftpconnect)+p) ) ;
		} else {
			sprintf( b1, "-P %d ", conf_get_int(conf, CONF_port) ) ;
		}
		bcat( buffer, BC, b1 ) ;
	}

	if( conf_get_int(conf, CONF_sshprot) == 3 ) { bcat( buffer, BC, "-2 " ) ; }   // SSH-2 Only

	if( strlen( conf_get_str(conf,CONF_password)) > 0 ) {
		/* CONF_password is plaintext at runtime; do NOT MASKPASS. qcat escapes any
		 * quote so the password can't inject an extra switch. */
		bcat( buffer, BC, "-pw " ) ; qcat( buffer, BC, conf_get_str(conf,CONF_password) ) ; bcat( buffer, BC, " " ) ;
	}
	if( strlen( conf_get_str(conf,CONF_portknockingoptions)) > 0 ) {
		bcat( buffer, BC, "-knock " ) ; qcat( buffer, BC, conf_get_str(conf,CONF_portknockingoptions) ) ; bcat( buffer, BC, " " ) ;
	}
	if( strlen( filename_to_str(conf_get_filename(conf, CONF_keyfile)) ) > 0 ) {
		bcat( buffer, BC, "-i " ) ; qcat( buffer, BC, filename_to_str(conf_get_filename(conf, CONF_keyfile)) ) ; bcat( buffer, BC, " " ) ;
	}

	/* source path (single quoted argument) */
	{
		char src[4096] ; src[0]='\0' ;
		if( (strlen(directory)>0) && (strlen(filename)>0) ) {
			bcat(src,sizeof(src),directory) ; bcat(src,sizeof(src),"\\") ; bcat(src,sizeof(src),filename) ;
		} else if( (directory!=NULL)&&(strlen(directory)>0) ) {
			bcat(src,sizeof(src),directory) ;
		} else {
			bcat(src,sizeof(src),filename) ;
		}
		qcat( buffer, BC, src ) ; bcat( buffer, BC, " " ) ;
	}

	/* destination user@host:remotedir (single quoted argument) */
	{
		char tgt[4096] ; tgt[0]='\0' ;
		if( strlen( conf_get_str(conf, CONF_sftpconnect) ) > 0 ) {
			snprintf( b1, sizeof(b1), "%s", conf_get_str(conf, CONF_sftpconnect) ) ;
			if( (p=poss(":",b1)) > 0 ) { b1[p-1]='\0'; }
			bcat( tgt, sizeof(tgt), b1 ) ;
		} else {
			bcat( tgt, sizeof(tgt), conf_get_str_ambi(conf,CONF_username,NULL) ) ; bcat( tgt, sizeof(tgt), "@" ) ;
			if( poss( ":", conf_get_str(conf,CONF_host))>0 ) { bcat(tgt,sizeof(tgt),"[") ; bcat(tgt,sizeof(tgt),conf_get_str(conf,CONF_host)) ; bcat(tgt,sizeof(tgt),"]") ; }
			else { bcat( tgt, sizeof(tgt), conf_get_str(conf,CONF_host) ) ; }
		}
		bcat( tgt, sizeof(tgt), ":" ) ; bcat( tgt, sizeof(tgt), remotedir ) ;
		qcat( buffer, BC, tgt ) ;
	}

	chdir( InitialDirectory ) ;
	if( debug_flag ) { debug_logevent( "Run: %s", buffer ) ; }
	/* Capture output + show it on failure, instead of flashing a console shut
	 * (so e.g. a server's exit-127 "Cannot initialize SFTP" is readable). */
	{ char whatbuf[600] ; snprintf( whatbuf, sizeof(whatbuf), "Upload of \"%s\"", filename ? filename : "file" ) ;
	  kitty_run_xfer( hwnd, buffer, whatbuf ) ; }

	//debug_log("%s\n",buffer);MessageBox( NULL, buffer, "Info",MB_OK );
	
	memset(buffer,0,strlen(buffer));
	}

void SendFileList( HWND hwnd, char * filelist ) {
	char *pname=NULL,dir[4096] ;
	int i;

	if( filelist==NULL ) return ;
	if( strlen( filelist ) == 0 ) return ;

	if( (filelist[strlen(filelist)]=='\0') && (filelist[strlen(filelist)+1]=='\0') ) {
		i=strlen(filelist) ;
		while( i>0 ) {
			i--;
			if( (filelist[i]=='/')||(filelist[i]=='\\') ) { filelist[i]='\0' ; break ; }
			}
		}
	snprintf( dir, sizeof(dir), "%s", filelist ) ;

	pname=filelist+strlen(filelist)+1;
	
	i = 0 ;
	while( pname[i] != '\0' ){
			
		while( (pname[i] != '\0') && (pname[i] != '\n') && (pname[i] != '\r') ) { i++ ; }
		pname[i]='\0';
		SendOneFile( hwnd, dir, pname, NULL ) ;
		pname=pname+i+1 ; i=0;
		}
		
	}

void SendFile( HWND hwnd ) {
	char filename[32768] ;

	if( conf_get_int(conf,CONF_protocol) != PROT_SSH ) {
		MessageBox( hwnd, "This function is only available with SSH connections.", "Error", MB_OK|MB_ICONERROR ) ;
		return ;
		}

	if( OpenFileName( hwnd, filename, "Send file...", "All files (*.*)|*.*|" ) ) 
		if( strlen( filename ) > 0 ) {
			SendFileList( hwnd, filename ) ;
		}
	}


// Start a plink command
/*
run() { printf "\033]0;__pl:$*\007" ; }
*/
int SearchPlink( void ) ;
void RunExternPlink( HWND hwnd, const char * cmd ) {
    char buffer[4096], plinkpath[4096]="", b1[256] ;

    if( PlinkPath==NULL ) {
        if( IniFileFlag == SAVEMODE_REG ) return ;
        else if( !SearchPlink() ) return ;
    }
    if( !existfile( PlinkPath ) ) {
        if( IniFileFlag == SAVEMODE_REG ) return ;
        else if( !SearchPlink() ) return ;
    }

    if( !GetShortPathName( PlinkPath, plinkpath, 4095 ) ) return ;

    buffer[0]='\0' ;
    const size_t BC = sizeof(buffer) ;
    bcat( buffer, BC, plinkpath ) ; bcat( buffer, BC, " " ) ;

    if( strlen( conf_get_str(conf, CONF_sftpconnect) ) == 0 ) {
        sprintf( b1, "-P %d ", conf_get_int(conf, CONF_port)) ; bcat( buffer, BC, b1 ) ;
    }
    if( conf_get_int(conf,CONF_sshprot) == 3 ) { bcat( buffer, BC, "-2 " ) ; }   // SSH-2 Only

    if( strlen( conf_get_str(conf,CONF_password) ) > 0 ) {
        bcat( buffer, BC, "-pw " ) ; qcat( buffer, BC, conf_get_str(conf,CONF_password) ) ; bcat( buffer, BC, " " ) ;
    }
    if( strlen( filename_to_str(conf_get_filename(conf,CONF_keyfile)) ) > 0 ) {
        bcat( buffer, BC, "-i " ) ; qcat( buffer, BC, filename_to_str(conf_get_filename(conf,CONF_keyfile)) ) ; bcat( buffer, BC, " " ) ;
    }
    /* target (single quoted argument) */
    {
        char tgt[4096] ; tgt[0]='\0' ;
        if( strlen( conf_get_str(conf, CONF_sftpconnect) ) > 0 ) {
            bcat( tgt, sizeof(tgt), conf_get_str(conf, CONF_sftpconnect) ) ;
        } else {
            bcat( tgt, sizeof(tgt), conf_get_str_ambi(conf,CONF_username,NULL) ) ; bcat( tgt, sizeof(tgt), "@" ) ;
            if( poss( ":", conf_get_str(conf,CONF_host) )>0 ) { bcat(tgt,sizeof(tgt),"[") ; bcat(tgt,sizeof(tgt),conf_get_str(conf,CONF_host)) ; bcat(tgt,sizeof(tgt),"]") ; }
            else { bcat( tgt, sizeof(tgt), conf_get_str(conf,CONF_host) ) ; }
        }
        qcat( buffer, BC, tgt ) ; bcat( buffer, BC, " " ) ;
    }
    qcat( buffer, BC, cmd ) ;   /* remote command (single quoted argument) */

    chdir( InitialDirectory ) ;
    if( debug_flag ) { debug_logevent( "Run: %s", buffer) ; }
    if( kitty_run_noshell( buffer, 1 ) ) { MessageBox( NULL, buffer, "Execute problem", MB_OK|MB_ICONERROR  ) ; }
}

// Get a remote file throught scp
/*
get()
{
echo "\033]0;__pw:"`pwd`"\007"
for file in ${*} ; do echo "\033]0;__rv:"${file}"\007" ; done
}
*/
void GetOneFile( HWND hwnd, char * directory, const char * filename ) {
    char buffer[4096], pscppath[4096]="", pscpport[4096]="22", dir[4096]=".", b1[256] ;
    int p;

    if( PSCPPath==NULL ) {
        if( IniFileFlag == SAVEMODE_REG ) return ;
        else if( !SearchPSCP() ) return ;
    }
    if( !existfile( PSCPPath ) ) {
        if( IniFileFlag == SAVEMODE_REG ) return ;
        else if( !SearchPSCP() ) return ;
    }

    if( !GetShortPathName( PSCPPath, pscppath, 4095 ) ) return ;

    if( ReadParameterN( INIT_SECTION, "downloaddir", dir, sizeof(dir) ) ) {
        if( !existdirectory( dir ) ) { strcpy( dir, InitialDirectory ) ; }
    }

    if(strlen( dir ) == 0) { strcpy( dir, InitialDirectory ) ; }

    buffer[0]='\0' ;
    const size_t BC = sizeof(buffer) ;

    bcat( buffer, BC, pscppath ) ; bcat( buffer, BC, " " ) ;

    if( strlen(conf_get_str(conf, CONF_pscpoptions))>0 ) {     /* raw user options - unquoted */
        bcat( buffer, BC, conf_get_str(conf, CONF_pscpoptions) ) ; bcat( buffer, BC, " " ) ;
    }
    bcat( buffer, BC, conf_get_int(conf, CONF_winscpprot)==0 ? "-scp " : "-sftp " ) ;

    if( ReadParameterN( INIT_SECTION, "pscpport", pscpport, sizeof(pscpport) ) ) {
        pscpport[17]='\0';
        if( !strcmp( pscpport,"*" ) ) sprintf( pscpport, "%d", conf_get_int(conf,CONF_port) ) ;
        bcat( buffer, BC, "-P " ) ; bcat( buffer, BC, pscpport ) ; bcat( buffer, BC, " " ) ;
    } else {
        if( (p=poss(":",conf_get_str(conf, CONF_sftpconnect) )) > 0 ) sprintf( b1, "-P %d ", atoi(conf_get_str(conf, CONF_sftpconnect)+p) ) ;
        else sprintf( b1, "-P %d ", conf_get_int(conf, CONF_port) ) ;
        bcat( buffer, BC, b1 ) ;
    }
    if( conf_get_int(conf,CONF_sshprot) == 3 ) { bcat( buffer, BC, "-2 " ) ; }   // SSH-2 Only

    if( strlen( conf_get_str(conf,CONF_password) ) > 0 ) {
        bcat( buffer, BC, "-pw " ) ; qcat( buffer, BC, conf_get_str(conf,CONF_password) ) ; bcat( buffer, BC, " " ) ;
    }
    if( strlen( conf_get_str(conf,CONF_portknockingoptions)) > 0 ) {
        bcat( buffer, BC, "-knock " ) ; qcat( buffer, BC, conf_get_str(conf,CONF_portknockingoptions) ) ; bcat( buffer, BC, " " ) ;
    }
    if( strlen( filename_to_str(conf_get_filename(conf,CONF_keyfile)) ) > 0 ) {
        bcat( buffer, BC, "-i " ) ; qcat( buffer, BC, filename_to_str(conf_get_filename(conf,CONF_keyfile)) ) ; bcat( buffer, BC, " " ) ;
    }

    /* remote source user@host:path (single quoted argument) */
    {
        char src[4096] ; src[0]='\0' ;
        if( strlen( conf_get_str(conf, CONF_sftpconnect) ) > 0 ) {
            snprintf( b1, sizeof(b1), "%s", conf_get_str(conf, CONF_sftpconnect) ) ;
            if( (p=poss(":",b1)) > 0 ) { b1[p-1]='\0'; }
            bcat( src, sizeof(src), b1 ) ;
        } else {
            bcat( src, sizeof(src), conf_get_str_ambi(conf,CONF_username,NULL) ) ; bcat( src, sizeof(src), "@" ) ;
            if( poss( ":", conf_get_str(conf,CONF_host) )>0 ) { bcat(src,sizeof(src),"[") ; bcat(src,sizeof(src),conf_get_str(conf,CONF_host)) ; bcat(src,sizeof(src),"]") ; }
            else { bcat( src, sizeof(src), conf_get_str(conf,CONF_host) ) ; }
        }
        bcat( src, sizeof(src), ":" ) ;
        if( filename[0]=='/' ) {
            bcat( src, sizeof(src), filename ) ;
        } else if( (directory!=NULL) && (strlen(directory)>0) && (strlen(filename)>0) ) {
            bcat(src,sizeof(src),directory) ; bcat(src,sizeof(src),"/") ; bcat(src,sizeof(src),filename) ;
        } else if( (directory!=NULL) && (strlen(directory)>0) ) {
            bcat(src,sizeof(src),directory) ; bcat(src,sizeof(src),"/*") ;
        } else {
            bcat(src,sizeof(src),filename) ;
        }
        qcat( buffer, BC, src ) ; bcat( buffer, BC, " " ) ;
    }
    qcat( buffer, BC, dir ) ;   /* local destination dir (single quoted argument) */
    //strcat( buffer, " > kitty.log 2>&1" ) ; //if( !system( buffer ) ) unlink( "kitty.log" ) ;

    chdir( InitialDirectory ) ;

    if( debug_flag ) { debug_logevent( "Get on file: %s", buffer) ; }
    /* Capture output + show on failure (no vanishing console). */
    { char whatbuf[600] ; snprintf( whatbuf, sizeof(whatbuf), "Download of \"%s\"", filename ? filename : "file" ) ;
      kitty_run_xfer( hwnd, buffer, whatbuf ) ; }

    //debug_log("%s\n",buffer);//MessageBox( NULL, buffer, "Info",MB_OK );

    memset(buffer,0,strlen(buffer));
}

// Get a remote file throught SCP
void GetFile( HWND hwnd ) {
    char buffer[4096]="", b1[256], *pst ;
    char dir[4096], pscppath[4096]="", pscpport[4096]="22" ;
    int p;

    if( conf_get_int(conf,CONF_protocol) != PROT_SSH ) {
        MessageBox( hwnd, "This function is only available with SSH connections.", "Error", MB_OK|MB_ICONERROR ) ;
        return ;
    }

    if( PSCPPath==NULL ) {
        if( IniFileFlag == SAVEMODE_REG ) return ;
        else if( !SearchPSCP() ) return ;
    }

    if( !existfile( PSCPPath ) ) {
        if( IniFileFlag == SAVEMODE_REG ) return ;
        else if( !SearchPSCP() ) return ;
    }

    if( !GetShortPathName( PSCPPath, pscppath, 4095 ) ) return ;

    if (!IsClipboardFormatAvailable(CF_TEXT)) return ;

    if( OpenClipboard(NULL) ) {
        HGLOBAL hglb ;

        if( (hglb = GetClipboardData( CF_TEXT ) ) != NULL ) {
            if( ( pst = GlobalLock( hglb ) ) != NULL ) {
//sprintf(buffer,"#%s#%d",pst,strlen(pst));MessageBox(hwnd,buffer,"Info",MB_OK);
                str_rtrim( pst, "\n\r \t" ) ;
//sprintf(buffer,"#%s#%d",pst,strlen(pst));MessageBox(hwnd,buffer,"Info",MB_OK);
                strcpy( buffer, "" ) ;
                if( strlen( pst ) > 0 ) {
                    if( ReadParameterN( INIT_SECTION, "downloaddir", dir, sizeof(dir) ) ) {
                        if( !existdirectory( dir ) ) {
                            strcpy( dir, InitialDirectory ) ;
                        }
                    } else if( OpenDirName( hwnd, dir ) ) {
                        if( !existdirectory( dir ) ) { GlobalUnlock( hglb ) ; CloseClipboard(); return ; }
                        //strcpy( dir, InitialDirectory ) ;
                    } else { 
                        return ; 
                    }
                    //else { strcpy( dir, InitialDirectory ) ; }

                    buffer[0]='\0' ;
                    bcat( buffer, sizeof(buffer), pscppath ) ; bcat( buffer, sizeof(buffer), " " ) ;
                    if( strlen(conf_get_str(conf, CONF_pscpoptions))>0 ) {   /* raw user options */
                        bcat( buffer, sizeof(buffer), conf_get_str(conf, CONF_pscpoptions) ) ; bcat( buffer, sizeof(buffer), " " ) ;
                    }
                    bcat( buffer, sizeof(buffer), conf_get_int(conf, CONF_winscpprot)==0 ? "-scp " : "-sftp " ) ;
                    if( conf_get_int(conf,CONF_sshprot) == 3 ) { bcat( buffer, sizeof(buffer), "-2 " ) ; }   // SSH-2 Only
                    if( ReadParameterN( INIT_SECTION, "pscpport", pscpport, sizeof(pscpport) ) ) {
                        pscpport[17]='\0';
                        if( !strcmp( pscpport,"*" ) ) { sprintf( pscpport, "%d", conf_get_int(conf,CONF_port) ) ; }
                        bcat( buffer, sizeof(buffer), "-P " ) ; bcat( buffer, sizeof(buffer), pscpport ) ; bcat( buffer, sizeof(buffer), " " ) ;
                    } else {
                        if( (p=poss(":",conf_get_str(conf, CONF_sftpconnect) )) > 0 ) sprintf( b1, "-P %d ", atoi(conf_get_str(conf, CONF_sftpconnect)+p) ) ;
                        else sprintf( b1, "-P %d ", conf_get_int(conf, CONF_port) ) ;
                        bcat( buffer, sizeof(buffer), b1 ) ;
                    }
                    if( strlen( conf_get_str(conf,CONF_password) ) > 0 ) {
                        bcat( buffer, sizeof(buffer), "-pw " ) ; qcat( buffer, sizeof(buffer), conf_get_str(conf,CONF_password) ) ; bcat( buffer, sizeof(buffer), " " ) ;
                    }
                    if( strlen( filename_to_str(conf_get_filename(conf,CONF_keyfile)) ) > 0 ) {
                        bcat( buffer, sizeof(buffer), "-i " ) ; qcat( buffer, sizeof(buffer), filename_to_str(conf_get_filename(conf,CONF_keyfile)) ) ; bcat( buffer, sizeof(buffer), " " ) ;
                    }
                    /* remote source user@host:path (single quoted argument) */
                    {
                        char src[4096] ; src[0]='\0' ;
                        if( strlen( conf_get_str(conf, CONF_sftpconnect) ) > 0 ) {
                            snprintf( b1, sizeof(b1), "%s", conf_get_str(conf, CONF_sftpconnect) ) ;
                            if( (p=poss(":",b1)) > 0 ) { b1[p-1]='\0'; }
                            bcat( src, sizeof(src), b1 ) ;
                        } else {
                            bcat( src, sizeof(src), conf_get_str_ambi(conf,CONF_username,NULL) ) ; bcat( src, sizeof(src), "@" ) ;
                            if( poss( ":", conf_get_str(conf,CONF_host))>0 ) { bcat(src,sizeof(src),"[") ; bcat(src,sizeof(src),conf_get_str(conf,CONF_host)) ; bcat(src,sizeof(src),"]") ; }
                            else { bcat( src, sizeof(src), conf_get_str(conf,CONF_host) ) ; }
                        }
                        bcat( src, sizeof(src), ":" ) ; bcat( src, sizeof(src), pst ) ;
                        qcat( buffer, sizeof(buffer), src ) ; bcat( buffer, sizeof(buffer), " " ) ;
                    }
                    qcat( buffer, sizeof(buffer), dir ) ;   /* local destination (single quoted argument) */
                }
                GlobalUnlock( hglb ) ;
            }
        }
        CloseClipboard();
    }
    if( strlen( buffer ) > 0 ) {
        chdir( InitialDirectory ) ;
        if( debug_flag ) { debug_logevent("Get file: %s", buffer) ; }
        if( kitty_run_noshell( buffer, 0 ) ) { MessageBox( NULL, buffer, "Transfer problem", MB_OK|MB_ICONERROR  ) ; }
        //if( !system( buffer ) ) unlink( "kitty.log" ) ;
    }
}

// Start a locale commande (Internet Explorer for example)
/* #9 hardening: RunCmd() runs text taken straight from the Windows clipboard as
 * a local command (default Ctrl+F5), so attacker-planted clipboard content could
 * execute on a single keypress. Two independent, PER-SESSION safeguards, both ON
 * by default and each toggled in the config dialog (Window/Selection): a
 * confirmation prompt (CONF_runcmdconfirm) and a post-launch tray balloon
 * (CONF_runcmdnotify, mirroring kageant's key-use balloon). */
void RunCmd( HWND hwnd ) {
    char buffer[4096]="", * pst = NULL ;
    if (!IsClipboardFormatAvailable(CF_TEXT)) return ;
    if( OpenClipboard(NULL) ) {
        HGLOBAL hglb ;

        if( (hglb = GetClipboardData( CF_TEXT ) ) != NULL ) {
            if( ( pst = GlobalLock( hglb ) ) != NULL ) {
                snprintf( buffer, sizeof(buffer), "%s", pst ) ;
                GlobalUnlock( hglb ) ;
            }
        }
        CloseClipboard();
    }
    if( strlen( buffer ) > 0 ) {
        if( conf_get_bool( conf, CONF_runcmdconfirm ) ) {
            char prompt[4096+160] ;
            snprintf( prompt, sizeof(prompt),
                "Run this command from the clipboard?\n\n%s", buffer ) ;
            if( MessageBox( hwnd, prompt, "KiTTY - run clipboard command",
                    MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON2 ) != IDYES ) return ;
        }
        chdir( InitialDirectory ) ;
        //system( buffer ) ;
        STARTUPINFO si ;
        PROCESS_INFORMATION pi ;
        ZeroMemory( &si, sizeof(si) );
        si.cb = sizeof(si);
        ZeroMemory( &pi, sizeof(pi) );
        if( !CreateProcess(NULL, buffer, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi) ) {
            ShellExecute(hwnd, "open", buffer,0, 0, SW_SHOWDEFAULT);
        }
        if( conf_get_bool( conf, CONF_runcmdnotify ) ) {
            char note[4096+64] ;
            snprintf( note, sizeof(note), "Ran clipboard command:\n%s", buffer ) ;
            kitty_tray_balloon_async( hwnd, "KiTTY", note ) ;
        }
    }
}

// Manage local commands
static char * RemotePath = NULL ;
char * GetRemotePath() { return RemotePath ; }
/* Save remote path in RemotePath variable
pw() { printf "\033]0;__pw:`pwd`\007" ; }
*/
/* Execute a local command
cmd()
{
if [ $# -eq 0 ] ; then echo "Usage: cmd command" ; return 0 ; fi
printf "\033]0;__cm:"$@"\007"
}
*/
/* Send a file that is present locally
scriptfile()
{
if [ $# -eq 0 ] ; then echo "Usage: scriptfile filename" ; return 0 ; fi
printf "\033]0;__ls:"$@"\007"
}
*/
/* Start default browser
ie()
{
if [ $# -eq 0 ] ; then echo "Usage: ie url" ; return 0 ; fi
printf "\033]0;__ie:"$@"\007"
}
*/
/* Copy stdin into clipboard
function wcl {
  echo -ne '\e''[5i'
  cat $*
  echo -ne '\e''[4i'
  echo "Copied to Windows clipboard" 1>&2
}
*/
/* Start WinSCP pointing into the same directory
winscp() { printf "\033]0;__ws:"`pwd`"\007" ; printf "\033]0;__ti\007" ; }
winscp() { echo -ne "\033];__ws:${PWD}\007" ; }
*/
/* Start WinSCP with specific user, host and directory
wt() { printf "\033]0;__wt:"$(hostname)":"${USER}":"`pwd`"\007" ; printf "\033]0;__ti\007" ; }
*/
/* Start a duplicated session into the same directory
ds() { printf "\033]0;__ds:`pwd`\007" ; }
# Start a duplicated session with specific user, host and directory
dt() { printf "\033]0;__dt:"$(hostname)":"${USER}":"`pwd`"\007" ; }
*/
static int LocalCmdFlag = 1 ;
static int LocalUnsecureCmdFlag = 0 ;

/* #525 (CVE-2024-25003 / CVE-2024-25004): the __dt and __wt metacommands carry a
 * "host:user:path" payload straight from a remote-controlled ANSI escape
 * sequence. The original code strcpy()'d that into fixed host[1024]/user[256]
 * buffers with no bounds check, so a long host or user overflowed the stack.
 * Split it safely instead: copies into host/user are length-capped, the field
 * separators are validated, and the path is returned in a freshly-sized
 * allocation. Returns 1 on success (caller owns *path), 0 on malformed input. */
static int kitty_split_host_user_path( const char *s,
                                       char *host, size_t hostsz,
                                       char *user, size_t usersz, char **path ) {
    size_t n ; int i ;
    *path = NULL ;
    if( s == NULL ) return 0 ;
    n = strlen( s ) ; if( n >= hostsz ) n = hostsz - 1 ; memcpy( host, s, n ) ; host[n] = '\0' ;
    i = poss( ":", host ) ; if( i <= 0 ) return 0 ;             /* need a user field */
    n = strlen( host + i ) ; if( n >= usersz ) n = usersz - 1 ; memcpy( user, host + i, n ) ; user[n] = '\0' ;
    host[i-1] = '\0' ;
    i = poss( ":", user ) ; if( i <= 0 ) return 0 ;            /* need a path field */
    *path = (char*) malloc( strlen( user + i ) + 1 ) ; if( *path == NULL ) return 0 ;
    strcpy( *path, user + i ) ;
    user[i-1] = '\0' ;
    return 1 ;
}

/* SECURITY (0.84.1.37): the ManageLocalCmd "__xy" remote-escape metacommand
 * dispatcher has been REMOVED. It was dead code in 0.84 (the OSC hook that fed
 * it was never forward-ported, so it had no caller) and carried a latent
 * command-injection / RCE surface (__cm/__pl/__ie/__ds, cf. CVE-2024-23749) if
 * ever re-wired. If a future feature needs remote metacommands, re-introduce a
 * hardened, opt-in implementation rather than restoring this. */

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
            sprintf( key, "%s\\Sessions\\%s", TEXT(PUTTY_REG_POS), session ) ;
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

#if (defined MOD_BACKGROUNDIMAGE) && (!defined FLJ)
BOOL load_bg_bmp() ;
void clean_bg( void ) ;
void RedrawBackground( HWND hwnd ) ;
#endif

void RefreshBackground( HWND hwnd ) {
#if (defined MOD_BACKGROUNDIMAGE) && (!defined FLJ)
	if( GetBackgroundImageFlag() ) RedrawBackground( hwnd ) ;
	else
#endif
	InvalidateRect( hwnd, NULL, true ) ;
}

#if (defined MOD_BACKGROUNDIMAGE) && (!defined FLJ)
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
	
// Boite de dialogue d'information
static LRESULT CALLBACK InfoCallBack( HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam ) {
	switch(message) {
		case WM_INITDIALOG: SetWindowText( GetDlgItem(hwnd,IDC_RESULT), "" ) ; break;
		case WM_COMMAND:
			if( LOWORD(wParam) == 1001 ) {
				SetWindowText( GetDlgItem(hwnd,IDC_RESULT), (char*)lParam ) ;
				}

			if (LOWORD(wParam) == IDCANCEL)
			{
				EndDialog(hwnd, LOWORD(0));
			}
			break;
		case WM_CLOSE:
			EndDialog(hwnd, LOWORD(0));
			break;

		return DefWindowProc (hwnd, message, wParam, lParam);
		}
	return 0;
	}
	
HWND InfoBox( HINSTANCE hInstance, HWND hwnd ) {
	HWND hdlg = CreateDialog( hInstance, MAKEINTRESOURCE(IDD_INFOBOX), hwnd, (DLGPROC)InfoCallBack ) ;
	return hdlg ;
	}
	
void InfoBoxSetText( HWND hwnd, char * st ) { SendMessage( hwnd, WM_COMMAND, 1001, (LPARAM) st ) ; SendMessage( hwnd, WM_PAINT, 0,0 ) ; }

void InfoBoxClose( HWND hwnd ) { EndDialog(hwnd, LOWORD(0)) ; DestroyWindow( hwnd ) ; }

//CallBack du dialog InputBox
static int InputBox_Flag = 0 ;

static LRESULT CALLBACK InputCallBack(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam ) {
	HWND handle;
	switch (message) {
		case WM_INITDIALOG:
			if( IniFileFlag == SAVEMODE_DIR ) {
				SetWindowText(hwnd,"Text input (portable mode)");
			}
			handle = GetDlgItem(hwnd,IDC_RESULT);
			if( InputBoxResult == NULL ) SetWindowText(handle,"") ;
			else SetWindowText( handle, InputBoxResult ) ;
			break;
		case WM_COMMAND:
			if (LOWORD(wParam) == IDOK)
			{
				handle = GetDlgItem(hwnd,IDC_RESULT);
				if( InputBoxResult != NULL ) { free( InputBoxResult ) ; InputBoxResult = NULL ; }
				size_t length = GetWindowTextLength( handle ) ;
				InputBoxResult = (char*) malloc( length + 10 ) ;
				GetWindowText(handle,InputBoxResult,length+1);
				//EndDialog(hwnd, LOWORD(1));
				if( !InternalCommand( hwnd, InputBoxResult ) )
					SendKeyboardPlus( MainHwnd, InputBoxResult );
				SetWindowText(handle,"") ;
				
			}
			if (LOWORD(wParam) == IDCANCEL)
			{
				if( InputBoxResult != NULL ) { free( InputBoxResult ) ; InputBoxResult = NULL ; }
				EndDialog(hwnd, LOWORD(0));
			}
			
			break;

		case WM_CLOSE:
			EndDialog(hwnd, LOWORD(0));
			break;

		return DefWindowProc (hwnd, message, wParam, lParam);
	}
	return 0;
}

static LRESULT CALLBACK InputCallBackPassword(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam){
	HWND handle;
	switch (message) {
		case WM_INITDIALOG:
			handle = GetDlgItem(hwnd,IDC_RESULT);
			if( InputBoxResult == NULL ) SetWindowText(handle,"");
			else SetWindowText( handle, InputBoxResult ) ;
			break;

		case WM_COMMAND:
			if (LOWORD(wParam) == IDOK)
			{
				handle = GetDlgItem(hwnd,IDC_RESULT);
				if( InputBoxResult != NULL ) { free( InputBoxResult ) ; InputBoxResult = NULL ; }
				size_t length = GetWindowTextLength( handle ) ;
				InputBoxResult = (char*) malloc( length + 10 ) ;
				GetWindowText(handle,InputBoxResult,length+1);
				EndDialog(hwnd, LOWORD(0));
			}
			if (LOWORD(wParam) == IDCANCEL)
			{
				if( InputBoxResult != NULL ) { free( InputBoxResult ) ; InputBoxResult = NULL ; }
				EndDialog(hwnd, LOWORD(0));
			}
			
			break;

		case WM_CLOSE:
			EndDialog(hwnd, LOWORD(0));
			break;

		return DefWindowProc (hwnd, message, wParam, lParam);
	}
	return 0;
}

// Procedure specifique à la editbox multiligne (SHIFT+F8)
FARPROC lpfnOldEditProc ;
BOOL FAR PASCAL EditMultilineCallBack(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
	char buffer[4096], key_name[1024] ;
	switch (message) {
		case WM_KEYDOWN:
			if( (wParam==VK_RETURN) && (GetKeyState( VK_SHIFT )& 0x8000) ){
				SendMessage(GetParent(hwnd),WM_COMMAND,IDB_OK,0 ) ;
				return 0;
				}
			else if( (wParam==VK_F12) && (GetKeyState( VK_SHIFT )& 0x8000) ){
				GetWindowText( hwnd, buffer, 4096 ) ;
				cryptstring( GetCryptSaltFlag(), buffer, MASTER_PASSWORD ) ;
				SetWindowText( hwnd, buffer ) ;
				return 0 ;
				}
			else if( (wParam==VK_F11) && (GetKeyState( VK_SHIFT )& 0x8000) ){
				GetWindowText( hwnd, buffer, 4096 ) ;
				decryptstring( GetCryptSaltFlag(), buffer, MASTER_PASSWORD ) ;
				SetWindowText( hwnd, buffer ) ;
				return 0 ;
				}
			else
				return CallWindowProc((WNDPROC)lpfnOldEditProc, hwnd, message, wParam, lParam);
			break;
		case WM_KEYUP:
		case WM_CHAR:
		case WM_SYSKEYDOWN:
		case WM_SYSKEYUP:
			if( (wParam==VK_RETURN) && (GetKeyState( VK_SHIFT )& 0x8000) )
				return 0;
			else if( (wParam==VK_F2) && (GetKeyState( VK_SHIFT )& 0x8000) ) { // Charge une Notes
				sprintf( key_name, "%s\\Sessions\\%s", TEXT(PUTTY_REG_POS), conf_get_str(conf,CONF_sessionname) ) ;
				if( GetValueDataN(HKEY_CURRENT_USER, key_name, "Notes", buffer, sizeof(buffer)) != NULL ) {
					if( GetWindowTextLength(hwnd) > 0 ) 
						if( MessageBox(hwnd, "Are you sure you want to load Notes\nand erase this edit box ?","Load Warning", MB_YESNO|MB_ICONWARNING ) != IDYES ) break ;
					SetWindowText( hwnd, buffer ) ;
					}
				}
			else if( (wParam==VK_F3) && (GetKeyState( VK_SHIFT )& 0x8000) ) { // Sauve une Notes
				GetSessionField( conf_get_str(conf,CONF_sessionname), conf_get_str(conf,CONF_folder), "Notes", buffer ) ;
				if( strlen( buffer ) > 0 ) 
					if( MessageBox(hwnd, "Are you sure you want to save Edit box\ninto Notes registry ?","Save Warning", MB_YESNO|MB_ICONWARNING ) != IDYES ) break ;
				GetWindowText( hwnd, buffer, 4096 ) ;
				sprintf( key_name, "%s\\Sessions\\%s", TEXT(PUTTY_REG_POS), conf_get_str(conf,CONF_sessionname) ) ;
				RegTestOrCreate( HKEY_CURRENT_USER, key_name, "Notes", buffer ) ;
				}
			else 
				return CallWindowProc((WNDPROC)lpfnOldEditProc, hwnd, message, wParam, lParam);	
			break ;
            	default:
			return CallWindowProc((WNDPROC)lpfnOldEditProc, hwnd, message, wParam, lParam);
			break;
		}
	return TRUE ;
	}

static int EditReadOnly = 0 ;
#ifndef GWL_WNDPROC
#define GWL_WNDPROC GWLP_WNDPROC
#endif
static LRESULT CALLBACK InputMultilineCallBack (HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	HWND handle;

	switch (message)
	{
		case WM_INITDIALOG: {
			char * buffer ;
			buffer=(char*)malloc(1024);
			/* Caption from the live window title, not CONF_wintitle: the
			 * conf value can hold an unexpanded %%-placeholder template. */
			buffer[0] = '\0' ;
			if( (MainHwnd==NULL) || (GetWindowText( MainHwnd, buffer, 900 )<=0) || (buffer[0]=='\0') )
				strcpy( buffer, "KiTTY" ) ;
			strcat( buffer, " - Text input" ) ;
			SetWindowText( hwnd, buffer ) ;
			free(buffer);
			handle = GetDlgItem(hwnd,IDC_RESULT) ;
			if( EditReadOnly ) SendMessage(handle, EM_SETREADONLY, 1, 0);
			if( InputBoxResult == NULL ) SetWindowText(handle,"");
			else {
				int i,j=0;
				buffer=(char*)malloc(2*strlen(InputBoxResult)+10);
				for( i=0; i<=strlen(InputBoxResult);i++ ) {
					if( (InputBoxResult[i]=='\n') && (buffer[j-1]!='\r') ) 
						{ buffer[j]='\r';j++;}
					buffer[j]=InputBoxResult[i];
					j++;
					}
				SetWindowText( handle, buffer ) ;
				free(buffer);
				}
			
			FARPROC lpfnSubClassProc = (FARPROC)MakeProcInstance( EditMultilineCallBack, hInst );
			if( lpfnSubClassProc )
				lpfnOldEditProc = (FARPROC)SetWindowLongPtr( handle, GWLP_WNDPROC, (LONG_PTR)lpfnSubClassProc );
			}
			break;
		case WM_COMMAND:
			if (LOWORD(wParam) == IDB_OK) {
				handle = GetDlgItem(hwnd,IDC_RESULT);
				if( InputBoxResult != NULL ) { free( InputBoxResult ) ; InputBoxResult = NULL ; }
				size_t length = GetWindowTextLength( handle ) ;
				InputBoxResult = (char*) malloc( length + 10 ) ;

				GetWindowText(handle,InputBoxResult,length+1);

				// Si il y un texte selectionne, on ne recupere que celui-ci
				DWORD result = SendMessage( handle, EM_GETSEL, (WPARAM)0, (LPARAM)NULL );
				if( LOWORD(result) != HIWORD(result) ) {
					int i ;
					InputBoxResult[HIWORD(result)] = '\0' ;
					if( LOWORD(result) > 0 ) 
						for( i=0 ; i<=(HIWORD(result)-LOWORD(result)) ; i++ )
						InputBoxResult[i]=InputBoxResult[i+LOWORD(result)];
					}
				if( strlen(InputBoxResult)==0 || InputBoxResult[strlen(InputBoxResult)-1] != '\n' )
					strcat( InputBoxResult, "\n" ) ;

				SendKeyboard( MainHwnd, InputBoxResult ) ;
				ShowWindow( MainHwnd, SW_RESTORE ) ;
				BringWindowToTop( MainHwnd ) ;
				SetFocus( handle ) ;
				}
			else if (LOWORD(wParam) == IDCANCEL) {
				if( InputBoxResult != NULL ) { free( InputBoxResult ) ; InputBoxResult = NULL ; }
				EditReadOnly = 0 ;
				EndDialog(hwnd, LOWORD(0));
				}
			else if( HIWORD( wParam ) == EN_SETFOCUS ) {
				ShowWindow( MainHwnd, SW_SHOWNOACTIVATE ) ;
				DefWindowProc (hwnd, message, wParam, lParam) ;
				}
			break;
		case WM_CHAR: {
			DefWindowProc (hwnd, message, wParam, lParam) ;
			}
			break ;
		case WM_SIZE: {
			int h = HIWORD(lParam),w = LOWORD(lParam) ;
			int top = 35 ;
			RECT br ;
			handle = GetDlgItem( hwnd, IDC_RESULT ) ;
			/* The classic 35/43 constants are 96-dpi pixels and make the
			 * edit overlap the label/button row on high-DPI displays:
			 * derive the row height from the OK button's real bottom. */
			if( GetWindowRect( GetDlgItem( hwnd, IDB_OK ), &br ) ) {
				POINT pt ;
				pt.x = br.left ; pt.y = br.bottom ;
				ScreenToClient( hwnd, &pt ) ;
				top = pt.y + 4 ;
			}
			SetWindowPos( handle, HWND_TOP, 7, top, w-15, h-top-8, 0 ) ;
			DefWindowProc (hwnd, message, wParam, lParam) ;
			}
			break ;
		case WM_CLOSE:
			EditReadOnly = 0 ;
			EndDialog(hwnd, LOWORD(0));
			break;
		
		return DefWindowProc(hwnd, message, wParam, lParam);
	}
	return 0;
}

char * InputBox( HINSTANCE hInstance, HWND hwnd ) {
	if( InputBoxResult != NULL ) { free( InputBoxResult ) ; InputBoxResult = NULL ; }
	DialogBox(hInstance, MAKEINTRESOURCE(IDD_INPUTBOX), hwnd, (DLGPROC)InputCallBack) ;
	return InputBoxResult ;
	}

char * InputBoxMultiline( HINSTANCE hInstance, HWND hwnd ) {
	if( InputBoxResult != NULL ) { free( InputBoxResult ) ; InputBoxResult = NULL ; }
	
	if( IsClipboardFormatAvailable(CF_TEXT) ) {
		char * pst = NULL ;
		if( OpenClipboard(NULL) ) {
			HGLOBAL hglb ;
			if( (hglb = GetClipboardData( CF_TEXT ) ) != NULL ) {
				if( ( pst = GlobalLock( hglb ) ) != NULL ) {
					InputBoxResult = (char*) malloc( strlen( pst ) +1 ) ;
					strcpy( InputBoxResult, pst ) ;
					GlobalUnlock( hglb ) ;
					}
				}
			CloseClipboard();
			}
		}

	DialogBox(hInstance, MAKEINTRESOURCE(IDD_INPUTBOXMULTI), NULL, (DLGPROC)InputMultilineCallBack) ;
	return InputBoxResult ;
	}
	
char * InputBoxPassword( HINSTANCE hInstance, HWND hwnd ) {
	if( InputBoxResult != NULL ) { free( InputBoxResult ) ; InputBoxResult = NULL ; }
	DialogBox(hInstance, MAKEINTRESOURCE(IDD_INPUTBOXPW), hwnd, (DLGPROC)InputCallBackPassword) ;
	return InputBoxResult ;
	}

void GetAndSendLine( HWND hwnd ) {
	if( InputBox_Flag == 1 ) return ;
	InputBox_Flag = 1 ;
	InputBox( hinst, hwnd ) ; // Essayer avec GetModuleHandle(NULL)
	InputBox_Flag = 0 ;
	}
	
void GetAndSendMultiLine( HWND hwnd ) {
	if( InputBox_Flag == 1 ) return ;

	InputBox_Flag = 1 ;
	InputBoxMultiline( hinst, hwnd ) ;
	InputBox_Flag = 0 ;
	}

void GetAndSendLinePassword( HWND hwnd ) {
	if( InputBox_Flag == 1 ) return ;
	InputBox_Flag = 1 ;
	InputBoxPassword( hinst, hwnd ) ; // Essayer avec GetModuleHandle(NULL)
	InputBox_Flag = 0 ;
	}

void routine_inputbox( void * phwnd ) { 
	GetAndSendLine( MainHwnd ) ;
	}

void routine_inputbox_multiline( void * phwnd ) { 
	GetAndSendMultiLine( MainHwnd ) ;
	}

void routine_inputbox_password( void * phwnd ) { 
	GetAndSendLinePassword( MainHwnd ) ;
	}

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

#define NB_MENU_MAX 1024
static char *SpecialMenu[NB_MENU_MAX] ;
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

		RegQueryInfoKey(hKey,achClass,&cchClassName,NULL,&cSubKeys,&cbMaxSubKey,&cchMaxClass,&cValues,&cchMaxValue,&cbMaxValueData,&cbSecurityDescriptor,&ftLastWriteTime);
		nb = (*nbitem) ;

		if( cSubKeys>0 ) { // Recuperation des sous-menu
		for (i=0; (i<cSubKeys)&&(nb<NB_MENU_MAX); i++) {
			DWORD cchValue = MAX_VALUE_NAME; 
			char lpData[4096] ;
			achValue[0] = '\0';

			if( RegEnumKeyEx(hKey, i, lpData, &cchValue, NULL, NULL, NULL, &ftLastWriteTime) == ERROR_SUCCESS ) {
				SubMenu = CreateMenu() ;
				sprintf( buffer, "%s\\%s", KeyName, lpData ) ;
				ReadSpecialMenu( SubMenu, buffer, nbitem, 0 ) ;
				unmungestr( lpData, buffer, MAX_PATH ) ;
				AppendMenu( menu, MF_POPUP, (UINT_PTR)SubMenu, buffer ) ;
				}
			}
		}
		
		nb = (*nbitem) ;
		
		if (cValues) { // Recuperation des item de menu
		if( separator ) AppendMenu( menu, MF_SEPARATOR, 0, 0 ) ;
		
		if( nb<NB_MENU_MAX )
	        for (i=0; (i<cValues)&&(nb<NB_MENU_MAX); i++) {
			DWORD cchValue = MAX_VALUE_NAME; 
			DWORD lpType,dwDataSize=4096 ;
			unsigned char lpData[4096] ;
			dwDataSize = 4096 ;
			achValue[0] = '\0';

			if( RegEnumValue(hKey,i,achValue,&cchValue,NULL,&lpType,lpData,&dwDataSize) == ERROR_SUCCESS ) {
			if( strcmp(achValue,KITTY_DEFAULT_SESSION) || strcmp(KeyName,TEXT(PUTTY_REG_POS) "\\Launcher") ) {
				if( ShortcutsFlag ) {
					if( nb < 26 ) 
						sprintf( buffer, "%s\tCtrl+Shift+%c", achValue, ('A'+nb) ) ;
					else 
						sprintf( buffer, "%s", achValue ) ;
					}
				else
					sprintf( buffer, "%s", achValue ) ;
				AppendMenu(menu, MF_ENABLED, IDM_USERCMD+nb, buffer ) ;
				SpecialMenu[nb]=(char*)malloc( strlen( (char*)lpData ) + 1 ) ;
				strcpy( SpecialMenu[nb], (char*)lpData ) ;
				nb++ ;
				local_nb++ ;
				}
				}
			}
    		}

		(*nbitem)=nb ;
		
		RegCloseKey( hKey ) ;
		}
		}
	else if( IniFileFlag == SAVEMODE_DIR ) {
		sprintf( fullpath, "%s\\%s", ConfigDirectory, KeyName ) ;
		DIR * dir ;
		struct dirent * de ;
		FILE *fp ;
		if( ( dir = opendir( fullpath ) ) != NULL ) {
			if( separator ) AppendMenu( menu, MF_SEPARATOR, 0, 0 ) ;
			nb = (*nbitem) ;
			while( ( de = readdir(dir) ) != NULL ) { // Recherche de sous-cle (repertoire)
				if( strcmp(de->d_name,".") && strcmp(de->d_name,"..") ) {
					sprintf( buffer, "%s\\%s", fullpath, de->d_name ) ;
					if( GetFileAttributes( buffer ) & FILE_ATTRIBUTE_DIRECTORY ) {
						SubMenu = CreateMenu() ;
						sprintf( buffer, "%s\\%s", KeyName, de->d_name ) ;
						ReadSpecialMenu( SubMenu, buffer, nbitem, 0 ) ;
						unmungestr( de->d_name, buffer, MAX_PATH ) ;
						AppendMenu( menu, MF_POPUP, (UINT_PTR)SubMenu, buffer ) ;
						}
					/*if( stat( buffer, &statBuf ) != -1 ) {
						if( ( statBuf.st_mode & S_IFMT) == S_IFDIR ) {
							sprintf( buffer, "%s\\%s", KeyName, de->d_name ) ;
							ReadSpecialMenu( menu, buffer, nbitem, separator ) ;
							}
						}*/
					}
				}
			rewinddir( dir ) ;

			nb = (*nbitem) ;
			while( ( de = readdir(dir) ) != NULL ) { // Recherche de cle
				if( strcmp(de->d_name,".") && strcmp(de->d_name,"..") ) {
				if( strcmp(de->d_name,"Default%20Settings") || strcmp(KeyName,"Launcher") ) { // Default Settings ne doit pas apparaitre dans le Launcher
					
					sprintf( buffer, "%s\\%s", fullpath, de->d_name ) ;
					if( !(GetFileAttributes( buffer ) & FILE_ATTRIBUTE_DIRECTORY) ) {
						if( ( fp=fopen(buffer,"rb")) != NULL ) {
							while( fgets( buffer, 4096, fp )!=NULL ){
								str_rtrim( buffer, "\n\r" ) ;
								if( strlen(buffer)>0 && buffer[strlen(buffer)-1]=='\\' ) {
									buffer[strlen(buffer)-1]='\0' ;
									
									if( (p=strstr(buffer,"\\"))!=NULL ){
										p[0]='\0';
										AppendMenu(menu, MF_ENABLED, IDM_USERCMD+nb, buffer ) ;
										SpecialMenu[nb]=(char*)malloc( strlen( p+1 ) + 1 ) ;
										strcpy( SpecialMenu[nb], p+1 ) ;
										nb++ ;
										local_nb++ ;
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
		sprintf( KeyName, "Folders\\%s\\Commands", buffer ) ;
		ReadSpecialMenu( menu, KeyName, &nbitem, 1 ) ;
		
		mungestr( sessionname, buffer ) ;
		sprintf( KeyName, "Sessions_Commands\\%s", buffer ) ;
		ReadSpecialMenu( menu, KeyName, &nbitem, 1 ) ;

		}
	else {
		sprintf( KeyName, "%s\\Commands", TEXT(PUTTY_REG_POS) ) ;
		ReadSpecialMenu( menu, KeyName, &nbitem, 0 ) ;
		
		mungestr( folder, buffer ) ;
		sprintf( KeyName, "%s\\Folders\\%s\\Commands", TEXT(PUTTY_REG_POS), buffer ) ;
		ReadSpecialMenu( menu, KeyName, &nbitem, 1 ) ;

		mungestr( sessionname, buffer ) ;
		sprintf( KeyName, "%s\\Sessions\\%s\\Commands", TEXT(PUTTY_REG_POS), buffer ) ;
		ReadSpecialMenu( menu, KeyName, &nbitem, 1 ) ;
		}

	if( GetMenuItemCount( menu ) > 0 )
		AppendMenu( m, MF_POPUP, (UINT_PTR)menu, "&User Command" ) ;

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
void InitShortcuts( void ) ;

int InternalCommand( HWND hwnd, char * st ) {
	char buffer[4096] ;
	if( strstr( st, "/message " ) == st ) { 
		MessageBox( hwnd, st+9, "Info", MB_OK ) ; 
		return 1 ; 
	} else if( !strcmp( st, "/copytoputty" ) ) {
		RegDelTree (HKEY_CURRENT_USER, "Software\\SimonTatham\\PuTTY\\Sessions" ) ;
		sprintf( buffer, "%s\\Sessions", PUTTY_REG_POS ) ;
		kitty_RegCopyTree( HKEY_CURRENT_USER, buffer, "Software\\SimonTatham\\PuTTY\\Sessions" ) ;
		sprintf( buffer, "%s\\SshHostKeys", PUTTY_REG_POS ) ;
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
		sprintf( screenShotFile, "%s\\screenshot-%d-%ld.jpg", InitialDirectory, getpid(), time(0) );
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
		sprintf( buffer, "%s\\Sessions", PUTTY_REG_POS ) ;
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
	} else if( !strcmp( st, "/capslock" ) ) { 
		CapsLockFlag = abs( CapsLockFlag - 1 ) ; 
		return 1 ; 
	} else if( !strcmp( st, "/init" ) ) { 
		char buffer[4096] ;
		sprintf( buffer,"ConfigDirectory=%s\nIniFileFlag=%d\nDirectoryBrowseFlag=%d\nInitialDirectory=%s\nKittyIniFile=%s\nKittySavFile=%s\nKiTTYClassName=%s\n"
			,ConfigDirectory,IniFileFlag,DirectoryBrowseFlag,InitialDirectory,KittyIniFile,KittySavFile,KiTTYClassName ) ;
		MessageBox(hwnd,buffer,"Configuration infomations",MB_OK);
		return 1 ; 
	} else if( !strcmp( st, "/capslock" ) ) { 
		conf_set_int( conf, CONF_xpos, 10 ) ;
		conf_set_int( conf, CONF_ypos, 10 ) ;
		SetWindowPos( hwnd, 0, 10, 10, 0, 0, SWP_NOSIZE|SWP_NOZORDER|SWP_NOOWNERZORDER|SWP_NOACTIVATE ) ;
		return 1 ; 
	} else if( !strcmp( st, "/size" ) ) { 
		SizeFlag = abs( SizeFlag - 1 ) ; 
		set_title( NULL, conf_get_str(conf,CONF_wintitle) ) ;
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
			sprintf( buffer, "Your session name is\n-%s-", conf_get_str(conf,CONF_sessionname) ) ;
			MessageBox( hwnd, buffer, "Session name", MB_OK|MB_ICONWARNING ) ;
		} else
			MessageBox( hwnd, "No session name.", "Session name", MB_OK|MB_ICONWARNING ) ;
		return 1 ;
	} else if( !strcmp( st, "/passwd" ) && debug_flag ) {
		if( strlen( conf_get_str(conf,CONF_password) ) > 0 ) {
			char bufpass[4096], buffer[4096] ;
			strcpy( bufpass, conf_get_str(conf,CONF_password) ) ;
			/* plaintext at runtime; do NOT MASKPASS */
			sprintf( buffer, "Your password is\n-%s-", bufpass ) ;
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
	} else if( !strcmp( st, "/wintitle" ) ) { 
		TitleBarFlag = abs(TitleBarFlag-1) ; 
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


// Recherche le chemin vers le programme cthelper.exe
/* If `candidate` names an existing file, adopt it as the tool path *out
 * (freeing any previous value), optionally export it to environment `envname`,
 * optionally persist it under kitty.ini/registry key `param`, and return 1;
 * otherwise leave *out untouched and return 0. Pass param=NULL when the value
 * came from `param` itself (no need to write it back) and envname=NULL when the
 * tool has no associated environment variable. Generalises the former
 * per-tool malloc+strcpy+WriteParameter tails in the Search* helpers. */
static int adopt_tool_path_if_exists( char **out, const char *candidate,
                                      const char *param, const char *envname ) {
	if( candidate == NULL || !existfile( candidate ) ) return 0 ;
	if( *out != NULL ) free( *out ) ;
	*out = (char*) malloc( strlen(candidate) + 1 ) ;
	strcpy( *out, candidate ) ;
	if( envname != NULL ) set_env( (char*)envname, *out ) ;
	if( param != NULL ) WriteParameter( INIT_SECTION, (char*)param, *out ) ;
	return 1 ;
}

int SearchCtHelper( void ) {
	char buffer[4096] ;
	if( CtHelperPath!=NULL ) {
		free(CtHelperPath) ;
		CtHelperPath=NULL ;
	}
	if( ReadParameterN( INIT_SECTION, "CtHelperPath", buffer, sizeof(buffer) ) != 0 ) {
		if( adopt_tool_path_if_exists( &CtHelperPath, buffer, NULL, "CTHELPER_PATH" ) ) return 1 ;
		else { DelParameter( INIT_SECTION, "CtHelperPath" ) ; }
	}
	sprintf( buffer, "%s\\cthelper.exe", InitialDirectory ) ;
	if( adopt_tool_path_if_exists( &CtHelperPath, buffer, "CtHelperPath", "CTHELPER_PATH" ) ) return 1 ;
	return 0 ;
}
	
static int set_winscp_path_if_exists(const char *path)
{
	if( path != NULL && path[0] && existfile(path) ) {
		WinSCPPath = (char*) malloc( strlen(path) + 1 ) ;
		strcpy( WinSCPPath, path ) ;
		WriteParameter( INIT_SECTION, "WinSCPPath", WinSCPPath ) ;
		return 1 ;
	}
	return 0 ;
}

static int probe_winscp_env_dir(const char *envname, const char *subpath, char *buffer, size_t buflen)
{
	const char *base = getenv(envname) ;
	if( base == NULL || base[0] == '\0' ) return 0 ;
	snprintf( buffer, buflen, "%s\\%s", base, subpath ) ;
	return set_winscp_path_if_exists(buffer) ;
}

// Recherche le chemin vers le programme WinSCP
int SearchWinSCP( void ) {
	char buffer[4096] ;
	if( WinSCPPath!=NULL) { free(WinSCPPath) ; WinSCPPath = NULL ; }
	if( ReadParameterN( INIT_SECTION, "WinSCPPath", buffer, sizeof(buffer) ) != 0 ) {
		if( adopt_tool_path_if_exists( &WinSCPPath, buffer, NULL, NULL ) ) return 1 ;
		else { DelParameter( INIT_SECTION, "WinSCPPath" ) ; }
	}
	if( probe_winscp_env_dir("ProgramFiles", "WinSCP\\WinSCP.exe", buffer, sizeof(buffer)) ) return 1 ;
	if( probe_winscp_env_dir("ProgramFiles(x86)", "WinSCP\\WinSCP.exe", buffer, sizeof(buffer)) ) return 1 ;
	if( probe_winscp_env_dir("LOCALAPPDATA", "Programs\\WinSCP\\WinSCP.exe", buffer, sizeof(buffer)) ) return 1 ;
	if( probe_winscp_env_dir("ProgramFiles", "WinSCP3\\WinSCP3.exe", buffer, sizeof(buffer)) ) return 1 ;
	if( probe_winscp_env_dir("ProgramFiles(x86)", "WinSCP3\\WinSCP3.exe", buffer, sizeof(buffer)) ) return 1 ;
	snprintf( buffer, sizeof(buffer), "%s\\WinSCP.exe", InitialDirectory ) ;
	if( set_winscp_path_if_exists(buffer) ) return 1 ;
	if( ReadParameterN( INIT_SECTION, "winscpdir", buffer, sizeof(buffer) ) ) {
		buffer[4076]='\0';
		strcat( buffer, "\\" ) ; strcat( buffer, "WinSCP.exe" ) ;
		if( set_winscp_path_if_exists(buffer) ) return 1 ;
	}
	if( SearchPathA(NULL, "WinSCP.exe", NULL, sizeof(buffer), buffer, NULL) > 0 ) {
		if( set_winscp_path_if_exists(buffer) ) return 1 ;
	}
	return 0 ;
}

// Lance WinSCP à partir de la sesson courante eventuellement dans le repertoire courant
/* ALIAS UNIX A DEFINIR POUR DEMARRER WINSCP Dans le repertoire courant
winscp()
{
echo "\033]0;__ws:"`pwd`"\007"
}
Il faut ensuite simplement taper: winscp
(historique: jadis traite par ManageLocalCmd, supprime en 0.84.1.37 - voir la note securite plus haut)

Le chemin vers l'exécutable WinSCP est défini dans la variable WInSCPPath. Elle peut pointer sur un fichier .BAT pour passer des options supplémentaires.
@ECHO OFF
start "C:\Program Files\WinSCP\WinSCP.exe" "%1" "%2" "%3" "%4" "%5" "%6" "%7" "%8" "%9"
*/	
// winscp.exe [(sftp|ftp|scp)://][user[:password]@]host[:port][/path/[file]] [/privatekey=key_file] [/rawsettings (ProxyMethod=1) (Compression=1)]
void StartWinSCP( HWND hwnd, char * directory, char * host, char * user ) {
	char cmd[4096], shortpath[1024], buffer[4096], proto[10] ;
	int raw = 0;
	
	if( directory == NULL ) { directory = kitty_current_dir(); } 
	if( WinSCPPath==NULL ) {
		if( !SearchWinSCP() ) return ;
	}
	if( !existfile( WinSCPPath ) ) {
		if( !SearchWinSCP() ) return ;
	}
		
	if( !GetShortPathName( WinSCPPath, shortpath, 4095 ) ) return ;

	switch( conf_get_int(conf, CONF_winscpprot) ) {
		case 0: strcpy( proto, "scp" ) ; break ;
		case 2: strcpy( proto, "ftp" ) ; break ;
		case 3: strcpy( proto, "ftps" ) ; break ;
		case 4: strcpy( proto, "ftpes" ) ; break ;
		case 5: strcpy( proto, "http" ) ; break ;
		case 6: strcpy( proto, "https" ) ; break ;
		default: strcpy( proto, "sftp" ) ;
	}
	
	if( conf_get_int(conf,CONF_protocol) == PROT_SSH ) {
		sprintf( cmd, "\"%s\" %s://", shortpath, proto ) ;
			
		if( strlen( conf_get_str(conf, CONF_sftpconnect) ) > 0 ) {
			bcat( cmd, sizeof(cmd), conf_get_str(conf, CONF_sftpconnect) ) ;
		} else {
			urlcat( cmd, sizeof(cmd), user!=NULL ? user : conf_get_str_ambi(conf,CONF_username,NULL) ) ;
			if( strlen( conf_get_str(conf,CONF_password) ) > 0 ) {
				/* plaintext at runtime; do NOT MASKPASS. (Goes into the WinSCP URL -- #535: percent-encode so '@' '/' etc. can't redirect the host.) */
				bcat( cmd, sizeof(cmd), ":" ) ; urlcat( cmd, sizeof(cmd), conf_get_str(conf,CONF_password) ) ;
			}
			bcat( cmd, sizeof(cmd), "@" ) ;
			if( poss( ":", host!=NULL ? host : conf_get_str(conf,CONF_host) )>0 ) { bcat(cmd,sizeof(cmd),"[") ; bcat(cmd,sizeof(cmd), host!=NULL ? host : conf_get_str(conf,CONF_host)) ; bcat(cmd,sizeof(cmd),"]") ; }
			else { bcat( cmd, sizeof(cmd), host!=NULL ? host : conf_get_str(conf,CONF_host) ) ; }
			bcat( cmd, sizeof(cmd), ":" ) ; sprintf( buffer, "%d", conf_get_int(conf,CONF_port) ) ; bcat( cmd, sizeof(cmd), buffer ) ;
		}

		if( directory!=NULL ) if( strlen(directory)>0 ) {
			bcat( cmd, sizeof(cmd), directory ) ;
			if( directory[strlen(directory)-1]!='/' ) bcat( cmd, sizeof(cmd), "/" ) ;
		}
		if( strlen( filename_to_str(conf_get_filename(conf,CONF_keyfile)) ) > 0 ) {
			if( GetShortPathName( filename_to_str(conf_get_filename(conf,CONF_keyfile)), shortpath, 4095 ) ) {
				bcat( cmd, sizeof(cmd), " \"/privatekey=" ) ;
				bcat( cmd, sizeof(cmd), shortpath ) ;
				bcat( cmd, sizeof(cmd), "\"" ) ;
			}
		}
	} else {
		snprintf( cmd, sizeof(cmd), "\"%s\" %s://", shortpath, proto ) ;
		urlcat( cmd, sizeof(cmd), conf_get_str_ambi(conf,CONF_username,NULL) ) ; /* #535: percent-encode userinfo */
		if( strlen( conf_get_str(conf,CONF_password) ) > 0 ) {
			char bufpass[1024] ;
			bcat( cmd, sizeof(cmd), ":" ) ;
			snprintf(bufpass,sizeof(bufpass),"%s",conf_get_str(conf,CONF_password)); /* bounded */
			/* plaintext at runtime; do NOT MASKPASS */
			urlcat( cmd, sizeof(cmd), bufpass ) ;
			memset(bufpass,0,strlen(bufpass));
		}
		bcat( cmd, sizeof(cmd), "@" ) ;
		if( poss( ":", conf_get_str(conf,CONF_host) )>0 ) { bcat( cmd, sizeof(cmd), "[" ) ; bcat( cmd, sizeof(cmd), conf_get_str(conf,CONF_host) ) ; bcat( cmd, sizeof(cmd), "]" ) ; }
		else { bcat( cmd, sizeof(cmd), conf_get_str(conf,CONF_host) ) ; }
		bcat( cmd, sizeof(cmd), ":21" ) ;
		if( directory!=NULL ) if( strlen(directory)>0 ) {
			bcat( cmd, sizeof(cmd), directory ) ;
			if( directory[strlen(directory)-1]!='/' ) bcat( cmd, sizeof(cmd), "/" ) ;
		}
	}
	
	if( strlen(conf_get_str(conf, CONF_winscpoptions))>0 ) {
		bcat( cmd, sizeof(cmd), " " ) ; bcat( cmd, sizeof(cmd), conf_get_str(conf, CONF_winscpoptions) ) ;
	}

	if( (conf_get_int(conf,CONF_proxy_type) != PROXY_NONE) && (strlen( conf_get_str(conf, CONF_sftpconnect) )==0) ) {
		if( raw == 0 ) { bcat( cmd, sizeof(cmd), " /rawsettings" ) ; raw++ ; }
		switch( conf_get_int(conf,CONF_proxy_type) ) {
			case 2: bcat( cmd, sizeof(cmd), " ProxyMethod=2" ) ; break ;
			case 3: bcat( cmd, sizeof(cmd), " ProxyMethod=3" ) ; break ;
			case 4: bcat( cmd, sizeof(cmd), " ProxyMethod=4" ) ; break ;
			case 5: bcat( cmd, sizeof(cmd), " ProxyMethod=5" ) ; break ;
			default: bcat( cmd, sizeof(cmd), " ProxyMethod=1" ) ; break ;
		}
		if( strlen(conf_get_str(conf,CONF_proxy_host))>0 ) { bcat( cmd, sizeof(cmd), " ProxyHost=" ) ; bcat( cmd, sizeof(cmd), conf_get_str(conf,CONF_proxy_host) ) ; }
		snprintf( buffer, sizeof(buffer), " ProxyPort=%d", conf_get_int(conf,CONF_proxy_port)) ; bcat( cmd, sizeof(cmd), buffer ) ;
		if( strlen(conf_get_str(conf,CONF_proxy_username))>0 ) { bcat( cmd, sizeof(cmd), " ProxyUsername=" ) ; bcat( cmd, sizeof(cmd), conf_get_str(conf,CONF_proxy_username) ) ; }
		if( strlen(conf_get_str(conf,CONF_proxy_password))>0 ) { bcat( cmd, sizeof(cmd), " ProxyPassword=" ) ; bcat( cmd, sizeof(cmd), conf_get_str(conf,CONF_proxy_password) ) ; }
		if( strlen(conf_get_str(conf,CONF_proxy_telnet_command))>0 ) { bcat( cmd, sizeof(cmd), " ProxyTelnetCommand=\"" ) ; bcat( cmd, sizeof(cmd), conf_get_str(conf,CONF_proxy_telnet_command) ) ; bcat( cmd, sizeof(cmd), "\"") ; }
	}

	if( conf_get_bool(conf,CONF_compression) ) {
		if( raw == 0 ) { bcat( cmd, sizeof(cmd), " /rawsettings" ) ; raw++ ; }
		bcat( cmd, sizeof(cmd), " Compression=1" ) ;
	}

	if( conf_get_bool(conf, CONF_agentfwd) ) {
		if( raw == 0 ) { bcat( cmd, sizeof(cmd), " /rawsettings" ) ; raw++ ; }
		bcat( cmd, sizeof(cmd), " AgentFwd=1" ) ;
	}

	if( strlen(conf_get_str(conf, CONF_winscprawsettings))>0 ) {
		if( raw == 0 ) { bcat( cmd, sizeof(cmd), " /rawsettings" ) ; raw++ ; }
		bcat( cmd, sizeof(cmd), " " ) ; bcat( cmd, sizeof(cmd), conf_get_str(conf, CONF_winscprawsettings) ) ;
	}

	if( !strcmp(proto,"scp") && (strlen(conf_get_str(conf, CONF_pscpshell))>0) ) {
		if( raw == 0 ) { bcat( cmd, sizeof(cmd), " /rawsettings" ) ; raw++ ; }
		bcat( cmd, sizeof(cmd), " " ) ; bcat( cmd, sizeof(cmd), "Shell=\"" ) ; bcat( cmd, sizeof(cmd), conf_get_str(conf, CONF_pscpshell) ) ; bcat( cmd, sizeof(cmd), "\"" ) ;
	}
	
	if( debug_flag ) { debug_logevent( "Start WinSCP: %s", cmd ) ; }
	RunCommand( hwnd, cmd ) ;
	memset(cmd,0,strlen(cmd));
}

	
// Recherche le chemin vers le programme PSCP
int SearchPSCP( void ) {
	char buffer[4096], ki[10]="kscp.exe", pu[10]="pscp.exe" ;

	if( PSCPPath!=NULL ) { free(PSCPPath) ; PSCPPath = NULL ; }
	// Dans la base de registre
	if( ReadParameterN( INIT_SECTION, "PSCPPath", buffer, sizeof(buffer) ) != 0 ) {
		if( adopt_tool_path_if_exists( &PSCPPath, buffer, NULL, NULL ) ) return 1 ;
		else { DelParameter( INIT_SECTION, "PSCPPath" ) ; }
	}

	// Dans le fichier ini
	if( ReadParameterN( INIT_SECTION, "pscpdir", buffer, sizeof(buffer) ) ) {
		buffer[4076]='\0';
		strcat( buffer, "\\" ) ; strcat( buffer, ki ) ;
		if( adopt_tool_path_if_exists( &PSCPPath, buffer, "PSCPPath", NULL ) ) return 1 ;
		else {
			ReadParameterN( INIT_SECTION, "pscpdir", buffer, sizeof(buffer) ) ;
			buffer[4076]='\0';
			strcat( buffer, "\\" ) ; strcat( buffer, pu ) ;
			if( adopt_tool_path_if_exists( &PSCPPath, buffer, "PSCPPath", NULL ) ) return 1 ;
		}
	}
#ifndef FLJ
	// kscp dans le meme repertoire
	sprintf( buffer, "%s\\%s", InitialDirectory, ki ) ;
	if( adopt_tool_path_if_exists( &PSCPPath, buffer, "PSCPPath", NULL ) ) return 1 ;
#endif
	// pscp dans le repertoire normal de PuTTY
	sprintf( buffer, "%s\\PuTTY\\%s", getenv("ProgramFiles"), pu ) ;
	if( adopt_tool_path_if_exists( &PSCPPath, buffer, "PSCPPath", NULL ) ) return 1 ;

	// pscp dans le meme repertoire
	sprintf( buffer, "%s\\%s", InitialDirectory, pu ) ;
	if( adopt_tool_path_if_exists( &PSCPPath, buffer, "PSCPPath", NULL ) ) return 1 ;

	return 0 ;
}
	
// Recherche le chemin vers le programme Plink.exe
int SearchPlink( void ) {
	char buffer[4096], ki[10]="klink.exe", pu[10]="plink.exe" ;

	if( PlinkPath!=NULL ) { free(PlinkPath) ; PlinkPath = NULL ; }
	// Dans la base de registre
	if( ReadParameterN( INIT_SECTION, "PlinkPath", buffer, sizeof(buffer) ) != 0 ) {
		buffer[4076]='\0';
		if( adopt_tool_path_if_exists( &PlinkPath, buffer, NULL, NULL ) ) return 1 ;
		else { DelParameter( INIT_SECTION, "PlinkPath" ) ; }
	}

#ifndef FLJ
	// klink dans le meme repertoire
	sprintf( buffer, "%s\\%s", InitialDirectory, ki ) ;
	if( adopt_tool_path_if_exists( &PlinkPath, buffer, "PlinkPath", NULL ) ) return 1 ;
#endif

	// plink dans le repertoire normal de PuTTY
	sprintf( buffer, "%s\\PuTTY\\%s", getenv("ProgramFiles"), pu ) ;
	if( adopt_tool_path_if_exists( &PlinkPath, buffer, "PlinkPath", NULL ) ) return 1 ;

	// plink dans le meme repertoire
	sprintf( buffer, "%s\\%s", InitialDirectory, pu ) ;
	if( adopt_tool_path_if_exists( &PlinkPath, buffer, "PlinkPath", NULL ) ) return 1 ;

	return 0 ;
}
	
// Gestion du drap and drop
void recupNomFichierDragDrop(HWND hwnd, HDROP* leDrop ) {
        HDROP hDropInfo = *leDrop ;
        int nb,taille,i;
        taille=0;
        nb=0;
	if( leDrop==NULL ) return ;
        nb=DragQueryFile( hDropInfo, 0xFFFFFFFF, NULL, 0 ) ;
        char *fic ;
	if( nb>0 ) for( i = 0; i < nb; i++ ) {
                taille = DragQueryFile(hDropInfo, i, NULL, 0 ) ;   /* length, excluding NUL */
		fic = (char*)malloc(taille+2) ;
                { UINT _g = DragQueryFile( hDropInfo, i, fic, taille+1 ) ; fic[_g] = '\0' ; }  /* force-terminate: DragQueryFile doesn't always NUL-terminate -> a stray byte was reaching pscp ("...pdf\0") */
		if( !strcmp( fic+strlen(fic)-10,"\\kitty.ini" ) ) { // On charge le fichier de config dans l'editeur interne
			char buffer[1024]="", shortname[1024]="" ;
			if( GetModuleFileName( NULL, (LPTSTR)buffer, 1023 ) ) 
				if( GetShortPathName( buffer, shortname, 1023 ) ) {
					sprintf( buffer, "\"%s\" -ed %s", shortname, fic ) ;
					RunCommand( hwnd, buffer ) ;
				}
		} else { 
			if( !conf_get_bool( conf, CONF_scp_auto_pwd ) ) { SendOneFile( hwnd, "", fic, NULL ) ; }
			else { SendOneFile( hwnd, "", fic, RemotePath  ) ; }
		}
		free(fic);
	}
	DragFinish(hDropInfo) ;  //vidage de la mem...
        *leDrop = hDropInfo ;  //TOCHECK : transmistion de param...
}

void OnDropFiles(HWND hwnd, HDROP hDropInfo) {
	if( conf_get_int(conf,CONF_protocol) != PROT_SSH ) {
		MessageBox( hwnd, "This function is only available with SSH connections.", "Error", MB_OK|MB_ICONERROR ) ;
		return ;
	}
	if( !conf_get_bool( conf, CONF_scp_auto_pwd ) ) { 
		recupNomFichierDragDrop(hwnd, &hDropInfo) ; 
	} else { 
		if( RemotePath != NULL ) { free( RemotePath ) ; RemotePath = NULL ; }
		if( hDropInf != NULL ) { free(hDropInf) ; hDropInf = NULL ; }
		char cmd[1024] = "printf \"\\033]0;__pw:%s\\007\" `pwd`\\n" ;
		if( AutoCommand != NULL ) { free(AutoCommand) ; AutoCommand = NULL ; }
		AutoCommand = (char*) malloc( strlen(cmd) + 10 ) ;
		strcpy( AutoCommand, cmd );
		SetTimer(hwnd, TIMER_AUTOCOMMAND, autocommand_delay, NULL) ;
		hDropInf = hDropInfo ;
		SetTimer(hwnd, TIMER_DND, dnd_delay, NULL) ;
	}
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
		sprintf( buffer, "Unable to load library %s\n", filename ) ;
		MessageBox( hwnd, buffer, "Error" , MB_OK|MB_ICONERROR ) ;
		return -1 ;
		}
		
	if( !( lpfnDllFunc1 = (LPFNDLLFUNC1) GetProcAddress( lphDLL, TEXT(functionname) ) ) ) {
		//print_error( "Unable to load function %s from library %s (%d)\n", functionname, filename, GetLastError() );
		sprintf(buffer,"Unable to load function %s from library %s (%d)\n", functionname, filename, (int)GetLastError() ) ;
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
	
	sprintf( fullpath, "%s\\%s", Directory, sd ) ; 
	if( !MakeDir( fullpath ) ) {
		sprintf( fullpath,"Unable to create directory: %s\\%s !",Directory, sd);
		MessageBox(NULL,fullpath,"Error",MB_OK|MB_ICONERROR); 
		return 0 ;
	}
	
	sprintf( buffer, "%s\\%s", TEXT(PUTTY_REG_POS), s ) ;

	if( RegOpenKeyEx( HKEY_CURRENT_USER, TEXT(buffer), 0, KEY_READ, &hKey) == ERROR_SUCCESS ) {
		if( RegQueryInfoKey(hKey,achClass,&cchClassName,NULL,&cSubKeys,&cbMaxSubKey
			,&cchMaxClass,&cValues,&cchMaxValue,&cbMaxValueData,&cbSecurityDescriptor,&ftLastWriteTime) == ERROR_SUCCESS ) {
			if (cSubKeys) for (i=0; i<cSubKeys; i++) {
				cbName = MAX_KEY_LENGTH;
				retCode = RegEnumKeyEx(hKey, i, achKey, &cbName, NULL, NULL, NULL, &ftLastWriteTime) ;
				sprintf( buffer, "%s\\%s", s, achKey ) ;
				sprintf( fullpath, "%s\\%s", sd, achKey ) ;
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
						sprintf( fullpath, "%s\\%s\\%s", Directory, sd, buffer ) ;
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
	
	sprintf( buffer, "%s\\Commands", Directory ) ; DelDir( buffer) ; MakeDirTree( Directory, "Commands", "Commands" ) ;
	sprintf( buffer, "%s\\Launcher", Directory ) ; DelDir( buffer) ; MakeDirTree( Directory, "Launcher", "Launcher" ) ;
	sprintf( buffer, "%s\\Folders", Directory ) ; DelDir( buffer) ; MakeDirTree( Directory, "Folders", "Folders" ) ;
	sprintf( buffer, "%s\\Commands", Directory ) ; DelDir( buffer) ; MakeDirTree( Directory, "Commands", "Commands" ) ;
	
	sprintf( buffer, "%s\\Sessions", Directory ) ; DelDir( buffer) ; { 
		if(!MakeDir( buffer )) MessageBox(NULL,"Unable to create directory for storing sessions","Error",MB_OK|MB_ICONERROR); 
	}
	sprintf( buffer, "%s\\Sessions", TEXT(PUTTY_REG_POS) ) ;
		
	sprintf( fullpath, "%s\\Sessions_Commands", Directory ) ; DelDir( fullpath ) ; 
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
					sprintf( fullpath, "%s\\Sessions\\%s", Directory, conf_get_str(conf,CONF_folder)) ;
					if( !MakeDir( fullpath ) ) { MessageBox(NULL,"Unable to create directory for storing session informations","Error",MB_OK|MB_ICONERROR); }
					SetSessPath( conf_get_str(conf,CONF_folder) ) ; 
				}
				
				save_settings( buffer, conf) ;

				sprintf( buffer, "%s\\Sessions\\%s\\Commands", TEXT(PUTTY_REG_POS), achKey ) ;
				if( RegTestKey( HKEY_CURRENT_USER, buffer ) ) {
					sprintf( buffer, "Sessions\\%s\\Commands", achKey ) ;
					sprintf( fullpath, "Sessions_Commands\\%s", achKey ) ;
					MakeDirTree( Directory, buffer, fullpath ) ;
				}
			}
		}
		RegCloseKey( hKey ) ;
	}
	
	sprintf( buffer, "%s\\SshHostKeys", Directory ) ; DelDir( buffer) ; if( !MakeDir( buffer ) ) { 
		MessageBox(NULL,"Unable to create directory for storing ssh host keys","Error",MB_OK|MB_ICONERROR) ; 
	}
	sprintf( buffer, "%s\\SshHostKeys", TEXT(PUTTY_REG_POS) ) ;
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
						sprintf( fullpath, "%s\\SshHostKeys\\%s", Directory, buffer ) ;
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
	sprintf( buffer, "%s\\Proxies", Directory ) ; DelDir( buffer) ; if( !MakeDir( buffer ) ) { 
		MessageBox(NULL,"Unable to create directory for storing proxies definition","Error",MB_OK|MB_ICONERROR) ; 
	}
	sprintf( buffer, "%s\\Proxies", TEXT(PUTTY_REG_POS) ) ;
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
		sprintf( directory, "%s\\Sessions\\%s", Directory, path ) ;
	} else {
		sprintf( directory, "%s\\Sessions", Directory ) ;
	}
	if( ( dir = opendir( directory ) ) != NULL ) {
		while( (de=readdir(dir)) != NULL ) 
		if( strcmp(de->d_name,".")&&strcmp(de->d_name,"..")  ) {
			sprintf( buffer, "%s\\%s", directory, de->d_name ) ;
			if( GetFileAttributes( buffer ) & FILE_ATTRIBUTE_DIRECTORY ) {
				if( strlen(path)>0 ) sprintf( buffer, "%s\\%s", path, de->d_name ) ;
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
 
	sprintf( buffer, "%s\\Sessions", TEXT(PUTTY_REG_POS) ) ;
	if( RegTestKey( HKEY_CURRENT_USER, buffer ) ) 
		{ RegDelTree (HKEY_CURRENT_USER, buffer ) ; }
 
	SetCurrentDirectory( Directory ) ;
	sprintf( buffer, "%s\\Sessions", TEXT(PUTTY_REG_POS) ) ;
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
	sprintf( buffer, "%s\\Sessions", TEXT(PUTTY_REG_POS) ) ;
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

void NegativeColours(HWND hwnd) {
	int i ;
#ifdef MOD_TUTTYCOLOR
    for (i = 0; i < 34; i++) {
#else
    for (i = 0; i < 22; i++) {
#endif
	conf_set_int_int(conf, CONF_colours, i*3+0, 256-conf_get_int_int(conf, CONF_colours, i*3+0));
	conf_set_int_int(conf, CONF_colours, i*3+1, 256-conf_get_int_int(conf, CONF_colours, i*3+1));
	conf_set_int_int(conf, CONF_colours, i*3+2, 256-conf_get_int_int(conf, CONF_colours, i*3+2));
    }
    force_reconf = 0 ;
    PostMessage( hwnd, WM_COMMAND, IDM_RECONF, 0 ) ;
    
    RefreshBackground( hwnd ) ;
}

static int * BlackOnWhiteColoursSave = NULL ;
void BlackOnWhiteColours(HWND hwnd) {
	if( BlackOnWhiteColoursSave==NULL ) {
		BlackOnWhiteColoursSave = (int*) malloc( 6*sizeof(int) ) ;
		BlackOnWhiteColoursSave[0]=conf_get_int_int(conf, CONF_colours, 0); conf_set_int_int(conf, CONF_colours, 0, 0);
		BlackOnWhiteColoursSave[1]=conf_get_int_int(conf, CONF_colours, 1); conf_set_int_int(conf, CONF_colours, 1, 0);
		BlackOnWhiteColoursSave[2]=conf_get_int_int(conf, CONF_colours, 2); conf_set_int_int(conf, CONF_colours, 2, 0);
		BlackOnWhiteColoursSave[3]=conf_get_int_int(conf, CONF_colours, 6); conf_set_int_int(conf, CONF_colours, 6, 255);
		BlackOnWhiteColoursSave[4]=conf_get_int_int(conf, CONF_colours, 7); conf_set_int_int(conf, CONF_colours, 7, 255);
		BlackOnWhiteColoursSave[5]=conf_get_int_int(conf, CONF_colours, 8); conf_set_int_int(conf, CONF_colours, 8, 255);
	} else {
		if(conf_get_int_int(conf, CONF_colours, 0)==0) {
			conf_set_int_int(conf, CONF_colours, 0, 255);
			conf_set_int_int(conf, CONF_colours, 1, 255);
			conf_set_int_int(conf, CONF_colours, 2, 255);
			conf_set_int_int(conf, CONF_colours, 6, 0);
			conf_set_int_int(conf, CONF_colours, 7, 0);
			conf_set_int_int(conf, CONF_colours, 8, 0);
		} else {
			conf_set_int_int(conf, CONF_colours, 0, BlackOnWhiteColoursSave[0]) ;
			conf_set_int_int(conf, CONF_colours, 1, BlackOnWhiteColoursSave[1]) ;
			conf_set_int_int(conf, CONF_colours, 2, BlackOnWhiteColoursSave[2]) ;
			conf_set_int_int(conf, CONF_colours, 6, BlackOnWhiteColoursSave[3]) ;
			conf_set_int_int(conf, CONF_colours, 7, BlackOnWhiteColoursSave[4]) ;
			conf_set_int_int(conf, CONF_colours, 8, BlackOnWhiteColoursSave[5]) ;
			free(BlackOnWhiteColoursSave) ; 
			BlackOnWhiteColoursSave=NULL ;
		}
	}
	force_reconf = 0 ;
	PostMessage( hwnd, WM_COMMAND, IDM_RECONF, 0 ) ;

	ResetWindow(2);
}

static int original_fontsize = -1 ;
void ChangeFontSize(Terminal *term, Conf *conf,HWND hwnd, int dec) {
	FontSpec *fontspec = conf_get_fontspec(conf, CONF_font);
	if( original_fontsize<0 ) original_fontsize = fontspec->height ;
	if( dec == 0 ) { fontspec->height = original_fontsize ; }
	else {
		fontspec->height = fontspec->height + dec ;
		if(fontspec->height <=0 ) fontspec->height = 1 ;
	}
	conf_set_fontspec(conf, CONF_font, fontspec);
        fontspec_free(fontspec);
	force_reconf = 0 ;
	term_size(term,
				conf_get_int(conf, CONF_height),
				conf_get_int(conf, CONF_width),
				conf_get_int(conf, CONF_savelines));
	//PostMessage( hwnd, WM_COMMAND, IDM_RECONF, 0 ) ;

	ResetWindow(2);
}
void ChangeSettings(HWND hwnd) {
	//NegativeColours(hwnd);
	BlackOnWhiteColours(hwnd);
	//ChangeFontSize(hwnd,1);
	//ChangeFontSize(hwnd,-1);
}
	
#if (defined MOD_BACKGROUNDIMAGE) && (!defined FLJ)
// Gestion de l'image viewer
int ManageViewer( HWND hwnd, WORD wParam ) { // Gestion du mode image
	if( wParam==VK_BACK ) 
		{ if( PreviousBgImage( hwnd ) ) InvalidateRect(hwnd, NULL, TRUE) ; 
		set_title(NULL, conf_get_str(conf,CONF_wintitle) ) ; 
		return 1 ; 
		}
	else if( wParam==VK_SPACE ) 
		{ if( NextBgImage( hwnd ) ) InvalidateRect(hwnd, NULL, TRUE) ;
		set_title(NULL, conf_get_str(conf,CONF_wintitle)) ; 
		return 1 ; 
		}
	else if( wParam == VK_DOWN ) 	// Augmenter l'opacite de l'image de fond
		{ if( conf_get_int(conf,CONF_bg_type) != 0 ) {
			int n=conf_get_int(conf,CONF_bg_opacity) ;
			n += 5 ; if( n>100 ) n = 0 ;
			conf_set_int( conf, CONF_bg_opacity, n ) ;
			RefreshBackground( hwnd ) ;
			return 1 ; 
			}
		}
	else if( wParam == VK_UP ) 		// Diminuer l'opacite de l'image de fond
		{ if( conf_get_int(conf,CONF_bg_type) != 0 ) {
			int n=conf_get_int(conf,CONF_bg_opacity) ;
			n -= 5 ;
			if( n<0 ) n = 100 ;
			conf_set_int( conf, CONF_bg_opacity, n ) ;
			RefreshBackground( hwnd ) ; 
			return 1 ; 
			}
		}
	else if( wParam == VK_RETURN ) {
		  if (IsZoomed(hwnd)) { ShowWindow(hwnd, SW_RESTORE); } 
		  else { ShowWindow(hwnd, SW_MAXIMIZE); }
		return 1 ;
		}
	return 0 ;
	}
#endif
	
// Shortcuts managment
int DefineShortcuts( char * buf ) {
	char * pst = buf ;
	if( strlen(buf)==0 ) return 0 ;
	int key = 0 ;
	// Special keys
	while( (strstr(pst,"{SHIFT}")==pst) || (strstr(pst,"{CONTROL}")==pst) || (strstr(pst,"{ALT}")==pst) || (strstr(pst,"{ALTGR}")==pst) || (strstr(pst,"{WIN}")==pst) ) {
		while( strstr(pst,"{ALT}")==pst ) { key += ALTKEY ; pst += 5 ; }
		while( strstr(pst,"{ALTGR}")==pst ) { key += ALTGRKEY ; pst += 7 ; }
		while( strstr(pst,"{WIN}")==pst ) { key += WINKEY ; pst += 5 ; }
		while( strstr(pst,"{SHIFT}")==pst ) { key += SHIFTKEY ; pst += 7 ; }
		while( strstr(pst,"{CONTROL}")==pst ) { key += CONTROLKEY ; pst += 9 ; }
	}
	
	if( strstr( pst, "{F12}" )==pst ) { key = key + VK_F12 ; pst += 5 ; }
	else if( strstr( pst, "{F11}" )==pst ) { key = key + VK_F11 ; pst += 5 ; }
	else if( strstr( pst, "{F10}" )==pst ) { key = key + VK_F10 ; pst += 5 ; }
	else if( strstr( pst, "{F9}" )==pst ) { key = key + VK_F9 ; pst += 4 ; }
	else if( strstr( pst, "{F8}" )==pst ) { key = key + VK_F8 ; pst += 4 ; }
	else if( strstr( pst, "{F7}" )==pst ) { key = key + VK_F7 ; pst += 4 ; }
	else if( strstr( pst, "{F6}" )==pst ) { key = key + VK_F6 ; pst += 4 ; }
	else if( strstr( pst, "{F5}" )==pst ) { key = key + VK_F5 ; pst += 4 ; }
	else if( strstr( pst, "{F4}" )==pst ) { key = key + VK_F4 ; pst += 4 ; }
	else if( strstr( pst, "{F3}" )==pst ) { key = key + VK_F3 ; pst += 4 ; }
	else if( strstr( pst, "{F2}" )==pst ) { key = key + VK_F2 ; pst += 4 ; }
	else if( strstr( pst, "{F1}" )==pst ) { key = key + VK_F1 ; pst += 4 ; }
	else if( strstr( pst, "{RETURN}" )==pst ) { key = key + VK_RETURN ; pst += 8 ; }
	else if( strstr( pst, "{ESCAPE}" )==pst ) { key = key + VK_ESCAPE ; pst += 8 ; }
	else if( strstr( pst, "{SPACE}" )==pst ) { key = key + VK_SPACE ; pst += 7 ; }
	else if( strstr( pst, "{PRINT}" )==pst ) { key = key + VK_SNAPSHOT ; pst += 7 ; }
	else if( strstr( pst, "{PAUSE}" )==pst ) { key = key + VK_PAUSE ; pst += 7 ; }
	else if( strstr( pst, "{PRIOR}" )==pst ) { key = key + VK_PRIOR ; pst += 7 ; }
	else if( strstr( pst, "{RIGHT}" )==pst ) { key = key + VK_RIGHT ; pst += 7 ; }
	else if( strstr( pst, "{LEFT}" )==pst ) { key = key + VK_LEFT ; pst += 6 ; }
	else if( strstr( pst, "{NEXT}" )==pst ) { key = key + VK_NEXT ; pst += 6 ; }
	else if( strstr( pst, "{BACK}" )==pst ) { key = key + VK_BACK ; pst += 6 ; }
	else if( strstr( pst, "{HOME}" )==pst ) { key = key + VK_HOME ; pst += 6 ; }
	else if( strstr( pst, "{DOWN}" )==pst ) { key = key + VK_DOWN ; pst += 6 ; }
	else if( strstr( pst, "{ATTN}" )==pst ) { key = key + VK_ATTN ; pst += 6 ; }
	else if( strstr( pst, "{END}" )==pst ) { key = key + VK_END ; pst += 5 ; }
	else if( strstr( pst, "{TAB}" )==pst ) { key = key + VK_TAB ; pst += 5 ; }
	else if( strstr( pst, "{INS}" )==pst ) { key = key + VK_INSERT ; pst += 5 ; }
	else if( strstr( pst, "{DEL}" )==pst ) { key = key + VK_DELETE ; pst += 5 ; }
	else if( strstr( pst, "{UP}" )==pst ) { key = key + VK_UP ; pst += 4 ; }
	else if( strstr( pst, "{NUMPAD0}" )==pst ) { key = key + VK_NUMPAD0 ; pst += 9 ; }
	else if( strstr( pst, "{NUMPAD1}" )==pst ) { key = key + VK_NUMPAD1 ; pst += 9 ; }
	else if( strstr( pst, "{NUMPAD2}" )==pst ) { key = key + VK_NUMPAD2 ; pst += 9 ; }
	else if( strstr( pst, "{NUMPAD3}" )==pst ) { key = key + VK_NUMPAD3 ; pst += 9 ; }
	else if( strstr( pst, "{NUMPAD4}" )==pst ) { key = key + VK_NUMPAD4 ; pst += 9 ; }
	else if( strstr( pst, "{NUMPAD5}" )==pst ) { key = key + VK_NUMPAD5 ; pst += 9 ; }
	else if( strstr( pst, "{NUMPAD6}" )==pst ) { key = key + VK_NUMPAD6 ; pst += 9 ; }
	else if( strstr( pst, "{NUMPAD7}" )==pst ) { key = key + VK_NUMPAD7 ; pst += 9 ; }
	else if( strstr( pst, "{NUMPAD8}" )==pst ) { key = key + VK_NUMPAD8 ; pst += 9 ; }
	else if( strstr( pst, "{NUMPAD9}" )==pst ) { key = key + VK_NUMPAD9 ; pst += 9 ; }
	else if( strstr( pst, "{DECIMAL}" )==pst ) { key = key + VK_DECIMAL ; pst += 9 ; }
	else if( strstr( pst, "{BREAK}" )==pst ) { key = key + VK_CANCEL ; pst += 7 ; }
	else if( strstr( pst, "{NUMLOCK}" )==pst ) { key = key + VK_NUMLOCK ; pst += 9 ; }
	else if( strstr( pst, "{SCROLL}" )==pst ) { key = key + VK_SCROLL ; pst += 8 ; }
	else if( strstr( pst, "{ADD}" )==pst ) { key = key + VK_ADD ; pst += 5 ; }
	else if( strstr( pst, "{MULTIPLY}" )==pst ) { key = key + VK_MULTIPLY ; pst += 10 ; }
	else if( strstr( pst, "{SEPARATOR}" )==pst ) { key = key + VK_SEPARATOR ; pst += 11 ; }
	else if( strstr( pst, "{SUBTRACT}" )==pst ) { key = key + VK_SUBTRACT ; pst += 10 ; }
	else if( strstr( pst, "{DECIMAL}" )==pst ) { key = key + VK_DECIMAL ; pst += 9 ; }
	else if( strstr( pst, "{DIVIDE}" )==pst ) { key = key + VK_DIVIDE ; pst += 8 ; }
	else if( strstr( pst, "{ATTN}" )==pst ) { key = key + VK_ATTN ; pst += 6 ; }
	
	else if( strstr( pst, "{OEM_PLUS}" )==pst ) { key = key + VK_OEM_PLUS  ; pst += 10 ; } // +
	else if( strstr( pst, "{OEM_COMMA}" )==pst ) { key = key + VK_OEM_COMMA  ; pst += 11 ; } // ,
	else if( strstr( pst, "{OEM_MINUS}" )==pst ) { key = key + VK_OEM_MINUS  ; pst += 11 ; } // -
	else if( strstr( pst, "{OEM_PERIOD}" )==pst ) { key = key + VK_OEM_PERIOD  ; pst += 12 ; } // .
	
/* 
Example to change automatically , in .
[Shortcuts]
list={OEM_COMMA}
{OEM_COMMA}=.\
*/
	
	else if( pst[0] == '{' ) { key = 0 ; }
	else { key = key + toupper(pst[0]) ; }
	
	if( key==0 ) { key = -1 ; }
	return key ;
}
	
void TranslateShortcuts( char * st ) {
	int i,j,k,r ;
	char *buffer;
	if( st==NULL ) return ;
	if( strlen(st)==0 ) return ;
	buffer = (char*) malloc( strlen(st)+1 ) ;
	for( i=0 ; i<strlen(st) ; i++ ) {
		if( st[i]=='{' ) {
			if( strstr(st+i,"{{")==(st+i) ) {
				del(st,i+1,1);
			} else if( strstr(st+i,"{END}")==(st+i) ) {
				del(st,i+1,1);st[i]='\\';st[i+1]='k';st[i+2]='2';st[i+3]='3';
				i=i+3;
			} else if( strstr(st+i,"{HOME}")==(st+i) ) {
				del(st,i+1,2);st[i]='\\';st[i+1]='k';st[i+2]='2';st[i+3]='4';
				i=i+3;
			} else if( strstr(st+i,"{ESCAPE}")==(st+i) ) {
				del(st,i+1,4);st[i]='\\';st[i+1]='k';st[i+2]='1';st[i+3]='b';
				i=i+3;
			} else if( (j=poss( "}", st+i )) > 3 ) {
				for( k=i ; k<(i+j) ; k++ ) {
					buffer[k-i]=st[k] ;
					buffer[k-i+1]='\0' ;
				}
				r=DefineShortcuts( buffer ) ;
				del(st,i+1,j-1);
				st[i]=r ;
			}
		}
	}
	free(buffer);
}

// Init shortcuts map at startup
void InitShortcuts( void ) {
	char buffer[4096], list[4096], *pl ;
	int i, t=0 ;
	if( !readINI(KittyIniFile,"Shortcuts","editor",buffer, sizeof(buffer)) || ( (shortcuts_tab.editor=DefineShortcuts(buffer))<0 ) )
		shortcuts_tab.editor = SHIFTKEY+VK_F2 ;
	if( !readINI(KittyIniFile,"Shortcuts","editorclipboard",buffer, sizeof(buffer)) || ( (shortcuts_tab.editorclipboard=DefineShortcuts(buffer))<0 ) )
		shortcuts_tab.editorclipboard = CONTROLKEY+SHIFTKEY+VK_F2 ;
	if( !readINI(KittyIniFile,"Shortcuts","winscp",buffer, sizeof(buffer)) || ( (shortcuts_tab.winscp=DefineShortcuts(buffer))<0 ) )
		shortcuts_tab.winscp = SHIFTKEY+VK_F3 ;
	if( !readINI(KittyIniFile,"Shortcuts","switchlogmode",buffer, sizeof(buffer)) || ( (shortcuts_tab.switchlogmode=DefineShortcuts(buffer))<0 ) )
		shortcuts_tab.switchlogmode = SHIFTKEY+VK_F5 ;
	if( !readINI(KittyIniFile,"Shortcuts","showportforward",buffer, sizeof(buffer)) || ( (shortcuts_tab.showportforward=DefineShortcuts(buffer))<0 ) )
		shortcuts_tab.showportforward = SHIFTKEY+VK_F6 ;
//	if( !IsWow64() ) {
		if( !readINI(KittyIniFile,"Shortcuts","print",buffer, sizeof(buffer)) || ( (shortcuts_tab.print=DefineShortcuts(buffer))<0 ) )
			shortcuts_tab.print = SHIFTKEY+VK_F7 ;
		if( !readINI(KittyIniFile,"Shortcuts","printall",buffer, sizeof(buffer)) || ( (shortcuts_tab.printall=DefineShortcuts(buffer))<0 ) )
			shortcuts_tab.printall = VK_F7 ;
//	}
	if( !readINI(KittyIniFile,"Shortcuts","inputm",buffer, sizeof(buffer)) || ( (shortcuts_tab.inputm=DefineShortcuts(buffer))<0 ) )
		shortcuts_tab.inputm = SHIFTKEY+VK_F8 ;
#if (defined MOD_BACKGROUNDIMAGE) && (!defined FLJ)
	if( !readINI(KittyIniFile,"Shortcuts","viewer",buffer, sizeof(buffer)) || ( (shortcuts_tab.viewer=DefineShortcuts(buffer))<0 ) )
		shortcuts_tab.viewer = SHIFTKEY+VK_F11 ;
#endif
	if( !readINI(KittyIniFile,"Shortcuts","autocommand",buffer, sizeof(buffer)) || ( (shortcuts_tab.autocommand=DefineShortcuts(buffer))<0 ) ) 
		shortcuts_tab.autocommand = SHIFTKEY+VK_F12 ;

	if( !readINI(KittyIniFile,"Shortcuts","script",buffer, sizeof(buffer)) || ( (shortcuts_tab.script=DefineShortcuts(buffer))<0 ) )
		shortcuts_tab.script = CONTROLKEY+VK_F2 ;
	if( !readINI(KittyIniFile,"Shortcuts","sendfile",buffer, sizeof(buffer)) || ( (shortcuts_tab.sendfile=DefineShortcuts(buffer))<0 ) )
		shortcuts_tab.sendfile = CONTROLKEY+VK_F3 ;
	if( !readINI(KittyIniFile,"Shortcuts","getfile",buffer, sizeof(buffer)) || ( (shortcuts_tab.getfile=DefineShortcuts(buffer))<0 ) )
		shortcuts_tab.getfile = CONTROLKEY+VK_F4 ;
	if( !readINI(KittyIniFile,"Shortcuts","command",buffer, sizeof(buffer)) || ( (shortcuts_tab.command=DefineShortcuts(buffer))<0 ) )
		shortcuts_tab.command = CONTROLKEY+VK_F5 ;
	if( !readINI(KittyIniFile,"Shortcuts","tray",buffer, sizeof(buffer)) || ( (shortcuts_tab.tray=DefineShortcuts(buffer))<0 ) )
		shortcuts_tab.tray = CONTROLKEY+VK_F6 ;
	if( !readINI(KittyIniFile,"Shortcuts","visible",buffer, sizeof(buffer)) || ( (shortcuts_tab.visible=DefineShortcuts(buffer))<0 ) )
		shortcuts_tab.visible = CONTROLKEY+VK_F7 ;
	if( !readINI(KittyIniFile,"Shortcuts","input",buffer, sizeof(buffer)) || ( (shortcuts_tab.input=DefineShortcuts(buffer))<0 ) )
		shortcuts_tab.input = CONTROLKEY+VK_F8 ;
	if( !readINI(KittyIniFile,"Shortcuts","protect",buffer, sizeof(buffer)) || ( (shortcuts_tab.protect=DefineShortcuts(buffer))<0 ) )
		shortcuts_tab.protect = CONTROLKEY+VK_F9 ;
#if (defined MOD_BACKGROUNDIMAGE) && (!defined FLJ)
	if( !readINI(KittyIniFile,"Shortcuts","imagechange",buffer, sizeof(buffer)) || ( (shortcuts_tab.imagechange=DefineShortcuts(buffer))<0 ) )
		shortcuts_tab.imagechange = CONTROLKEY+VK_F11 ;
#endif
	if( !readINI(KittyIniFile,"Shortcuts","rollup",buffer, sizeof(buffer)) || ( (shortcuts_tab.rollup=DefineShortcuts(buffer))<0 ) )
		shortcuts_tab.rollup = CONTROLKEY+VK_F12 ;
	if( !readINI(KittyIniFile,"Shortcuts","resetterminal",buffer, sizeof(buffer)) || ( (shortcuts_tab.resetterminal=DefineShortcuts(buffer))<0 ) ) 
		shortcuts_tab.resetterminal = 0 ;
	if( !readINI(KittyIniFile,"Shortcuts","duplicate",buffer, sizeof(buffer)) || ( (shortcuts_tab.duplicate=DefineShortcuts(buffer))<0 ) )
		shortcuts_tab.duplicate = CONTROLKEY+ALTKEY+84 ;
	if( !readINI(KittyIniFile,"Shortcuts","opennew",buffer, sizeof(buffer)) || ( (shortcuts_tab.opennew=DefineShortcuts(buffer))<0 ) )
		shortcuts_tab.opennew = 0 ;
	if( !readINI(KittyIniFile,"Shortcuts","opennewcurrent",buffer, sizeof(buffer)) || ( (shortcuts_tab.opennewcurrent=DefineShortcuts(buffer))<0 ) )
		shortcuts_tab.opennewcurrent = 0 ;
	if( !readINI(KittyIniFile,"Shortcuts","changesettings",buffer, sizeof(buffer)) || ( (shortcuts_tab.changesettings=DefineShortcuts(buffer))<0 ) )
		shortcuts_tab.changesettings = 0 ;
	if( !readINI(KittyIniFile,"Shortcuts","clearscrollback",buffer, sizeof(buffer)) || ( (shortcuts_tab.clearscrollback=DefineShortcuts(buffer))<0 ) )
		shortcuts_tab.clearscrollback = 0 ;
	if( !readINI(KittyIniFile,"Shortcuts","clearlogfile",buffer, sizeof(buffer)) || ( (shortcuts_tab.clearlogfile=DefineShortcuts(buffer))<0 ) )
		shortcuts_tab.clearlogfile = 0 ;
	if( !readINI(KittyIniFile,"Shortcuts","closerestart",buffer, sizeof(buffer)) || ( (shortcuts_tab.closerestart=DefineShortcuts(buffer))<0 ) )
		shortcuts_tab.closerestart = 0 ;
	if( !readINI(KittyIniFile,"Shortcuts","eventlog",buffer, sizeof(buffer)) || ( (shortcuts_tab.eventlog=DefineShortcuts(buffer))<0 ) )
		shortcuts_tab.eventlog = 0 ;
	if( !readINI(KittyIniFile,"Shortcuts","fullscreen",buffer, sizeof(buffer)) || ( (shortcuts_tab.fullscreen=DefineShortcuts(buffer))<0 ) )
		shortcuts_tab.fullscreen = 0 ;
	if( !readINI(KittyIniFile,"Shortcuts","fontup",buffer, sizeof(buffer)) || ( (shortcuts_tab.fontup=DefineShortcuts(buffer))<0 ) )
		shortcuts_tab.fontup = 0 ;
	if( !readINI(KittyIniFile,"Shortcuts","fontdown",buffer, sizeof(buffer)) || ( (shortcuts_tab.fontdown=DefineShortcuts(buffer))<0 ) )
		shortcuts_tab.fontdown = 0 ;
	if( !readINI(KittyIniFile,"Shortcuts","copyall",buffer, sizeof(buffer)) || ( (shortcuts_tab.copyall=DefineShortcuts(buffer))<0 ) )
		shortcuts_tab.copyall = 0 ;
	if( !readINI(KittyIniFile,"Shortcuts","fontnegative",buffer, sizeof(buffer)) || ( (shortcuts_tab.fontnegative=DefineShortcuts(buffer))<0 ) )
		shortcuts_tab.fontnegative = 0 ;
	if( !readINI(KittyIniFile,"Shortcuts","fontblackandwhite",buffer, sizeof(buffer)) || ( (shortcuts_tab.fontblackandwhite=DefineShortcuts(buffer))<0 ) )
		shortcuts_tab.fontblackandwhite = 0 ;
	if( !readINI(KittyIniFile,"Shortcuts","keyexchange",buffer, sizeof(buffer)) || ( (shortcuts_tab.keyexchange=DefineShortcuts(buffer))<0 ) )
		shortcuts_tab.keyexchange = 0 ;

	
	if( NbShortCuts>0 ) for( i=0 ; i<NbShortCuts ; i++ ) { if( shortcuts_tab2[i].st!=NULL ) { free(shortcuts_tab2[i].st) ; } }
	NbShortCuts=0 ;
	if( ReadParameterN( "Shortcuts", "list", list, sizeof(list) ) ) {
		pl=list ;
		while( strlen(pl) > 0 ) {
			i=0;
			while( (i<strlen(pl))&&(pl[i]!=' ') ) { i++ ; }
			if( pl[i]==' ' ) { pl[i]='\0' ; t=1 ; }
			if( strlen(pl)>0 )
			if( ReadParameterN( "Shortcuts", pl, buffer, sizeof(buffer) ) ) {
				if( (pl[0]<'0')||(pl[0]>'9') ) {
					shortcuts_tab2[NbShortCuts].num = DefineShortcuts( pl );
				} else {
					shortcuts_tab2[NbShortCuts].num = atoi(pl) ;
				}
				TranslateShortcuts( buffer ) ;
				if( debug_flag ) { debug_logevent( "Remap key %s to %s", pl, buffer ) ; }

				shortcuts_tab2[NbShortCuts].st=(char*)malloc( strlen(buffer)+1 ) ;
				strcpy( shortcuts_tab2[NbShortCuts].st, buffer ) ;
				NbShortCuts++;
			}
			if( t==1 ) { pl[i]=' ' ; t = 0 ; pl=pl+i+1 ; }
			else pl=pl+i ;

			while( pl[0]==' ' ) pl++ ;
		}
	}
}

int SwitchLogMode(void) ;
int ManageShortcuts( Terminal *term, Conf *conf, HWND hwnd, const int* clips_system, int key_num, int shift_flag, int control_flag, int alt_flag, int altgr_flag, int win_flag ) {
	int key, i ;
	key = key_num ;
	if( alt_flag ) key = key + ALTKEY ;
	if( altgr_flag ) key = key + ALTGRKEY ;
	if( shift_flag ) key = key + SHIFTKEY ;
	if( control_flag ) key = key + CONTROLKEY ;
	if( win_flag ) key = key + WINKEY ;

//if( (key_num!=VK_SHIFT)&&(key_num!=VK_CONTROL) ) {char b[256] ; sprintf( b, "alt=%d altgr=%d shift=%d control=%d key_num=%d key=%d action=%d", alt_flag, altgr_flag, shift_flag, control_flag, key_num, key, shortcuts_tab.duplicate ); MessageBox(hwnd, b, "Info", MB_OK);}

	if( key == shortcuts_tab.protect )				// Protection
		{ SendMessage( hwnd, WM_COMMAND, IDM_PROTECT, 0 ) ; InvalidateRect( hwnd, NULL, TRUE ) ; return 1 ; }
	if( key == shortcuts_tab.rollup ) 				// Winroll
			{ SendMessage( hwnd, WM_COMMAND, IDM_WINROL, 0 ) ; return 1 ; }
	if( key == shortcuts_tab.switchlogmode ) {
		i = SwitchLogMode() ;
		if( i==1 ) { debug_logevent( "Enable logging" ) ; } else { debug_logevent( "Disable logging" ) ; }
		return 1 ;
	}
	if( key == shortcuts_tab.showportforward ) 				// Fonction show port forward
		{ SendMessage( hwnd, WM_COMMAND, IDM_SHOWPORTFWD, 0 ) ; return 1 ; }

	if( (ProtectFlag == 1) || (WinHeight != -1) ) return 1 ;
		
	if( NbShortCuts ) {
		for( i=0 ; i<NbShortCuts ; i++ )
		if( shortcuts_tab2[i].num == key ) {
			SendKeyboardPlus( hwnd, shortcuts_tab2[i].st ) ;
			return 1 ; 
		}
	}
	
#if (defined MOD_BACKGROUNDIMAGE) && (!defined FLJ)
	if( GetBackgroundImageFlag() && ImageViewerFlag ) { // Gestion du mode image
		if( ManageViewer( hwnd, key_num ) ) return 1 ;
		}
#endif
	if( control_flag && shift_flag && (key_num==VK_F12) ) {
		ResizeWinList( hwnd, conf_get_int(conf,CONF_width), conf_get_int(conf,CONF_height) ) ; return 1 ; 
	} // Resize all PuTTY windows to the size of the current one

	if( key == shortcuts_tab.printall ) {		
		SendMessage( hwnd, WM_COMMAND, IDM_COPYALL, 0 ) ;
		SendMessage( hwnd, WM_COMMAND, IDM_PRINT, 0 ) ;
		return 1 ;
	}

	if( ( IniFileFlag != SAVEMODE_DIR ) && shift_flag && control_flag ) {		
		if( ( key_num >= 'A' ) && ( key_num <= 'Z' ) ) // Raccourci commandes speciales (SpecialMenu) CTRL+SHIFT+'A' ... CTRL+SHIFT+'Z'
			{ SendMessage( hwnd, WM_COMMAND, IDM_USERCMD+key_num-'A', 0 ) ; return 1 ; }
	}
		
	if( key == shortcuts_tab.editor ) {			// Lancement d'un putty-ed
		if( debug_flag ) { debug_logevent( "Start empty internal editor" ) ; }
		RunPuttyEd( hwnd, NULL ) ; 
		return 1 ; 
	}
	if( key == (shortcuts_tab.editorclipboard ) ) {		// Lancement d'un putty-ed qui charge le contenu du presse-papier
		if( debug_flag ) { debug_logevent( "Start internal editor fullfiled with clipboard" ) ; }
		//term_copyall(term,clips_system,lenof(clips_system)) /* Full term clipboard */
		RunPuttyEd( hwnd, "1" ) ; 
		return 1 ; 
	} else if( key == shortcuts_tab.winscp ) {			// Lancement de WinSCP
		SendMessage( hwnd, WM_COMMAND, IDM_WINSCP, 0 ) ; return 1 ;
	} else if( key == shortcuts_tab.autocommand ) { 		// Rejouer la commande de demarrage
			RenewPassword( conf ) ; 
			SetTimer(hwnd, TIMER_AUTOCOMMAND,autocommand_delay, NULL) ;
			return 1 ; 
	} if( key == shortcuts_tab.print ) {			// Impression presse papier
		SendMessage( hwnd, WM_COMMAND, IDM_PRINT, 0 ) ; 
		return 1 ; 
	}
#ifndef FLJ
	if( key == shortcuts_tab.inputm )	 		// Fenetre de controle
		{
		MainHwnd = hwnd ; _beginthread( routine_inputbox_multiline, 0, (void*)&hwnd ) ;
		return 1 ;
		}
#endif
#if (defined MOD_BACKGROUNDIMAGE) && (!defined FLJ)
	if( GetBackgroundImageFlag() && (key == shortcuts_tab.viewer) ) 	// Switcher le mode visualiseur d'image
		{ ImageViewerFlag = abs(ImageViewerFlag-1) ; set_title(NULL, conf_get_str(conf,CONF_wintitle) ) ; return 1 ; }
#endif
	if( key == shortcuts_tab.script ) 			// Chargement d'un fichier de script
		{ OpenAndSendScriptFile( hwnd ) ; return 1 ; }
	else if( key == shortcuts_tab.sendfile ) 		// Envoi d'un fichier par SCP
		{ SendMessage( hwnd, WM_COMMAND, IDM_PSCP, 0 ) ; return 1 ; }
	else if( key == shortcuts_tab.getfile ) 		// Reception d'un fichier par SCP
		{ GetFile( hwnd ) ; return 1 ; }
	else if( key == shortcuts_tab.command )			// Execution d'une commande locale
		{ RunCmd( hwnd ) ; return 1 ; }
	else if( key == shortcuts_tab.tray ) 		// Send to tray
		{ SendMessage( hwnd, WM_COMMAND, IDM_TOTRAY, 0 ) ; return 1 ; }
	else if( key == shortcuts_tab.visible )  		// Always visible 
		{ SendMessage( hwnd, WM_COMMAND, IDM_VISIBLE, 0 ) ; return 1 ; }
	else if( key == shortcuts_tab.resetterminal ) 		// Envoi d'un fichier par SCP
		{ SendMessage( hwnd, WM_COMMAND, IDM_RESET, 0 ) ; return 1 ; }
	else if( key == shortcuts_tab.duplicate ) 		// Duplicate session
		{ SendMessage( hwnd, WM_COMMAND, IDM_DUPSESS, 0 ) ; return 1 ; }
	else if( key == shortcuts_tab.opennew ) 		// Open new session
		{ SendMessage( hwnd, WM_COMMAND, IDM_NEWSESS, 0 ) ; return 1 ; }
	else if( key == shortcuts_tab.opennewcurrent ) 		// Open new config box with current settings
		{ RunSessionWithCurrentSettings( hwnd, conf, NULL, NULL, NULL, 0, NULL ) ; return 1 ; }
	else if( key == shortcuts_tab.changesettings ) 		// Change settings
		{ SendMessage( hwnd, WM_COMMAND, IDM_RECONF, 0 ) ; return 1 ; }
	else if( key == shortcuts_tab.clearscrollback )		// Clear scrollback
		{ SendMessage( hwnd, WM_COMMAND, IDM_CLRSB, 0 ) ; return 1 ; }
	else if( key == shortcuts_tab.clearlogfile )		// Clear log file
		{ SendMessage( hwnd, WM_COMMAND, IDM_CLEARLOGFILE, 0 ) ; return 1 ; }
	else if( key == shortcuts_tab.closerestart )		// Close + restart
		{ SendMessage( hwnd, WM_COMMAND, IDM_RESTARTSESSION, 0 ) ; return 1 ; }
	else if( key == shortcuts_tab.eventlog )		// Event log
		{ SendMessage( hwnd, WM_COMMAND, IDM_SHOWLOG, 0 ) ; return 1 ; }
	else if( key == shortcuts_tab.fullscreen )		// Full screen
		{ SendMessage( hwnd, WM_COMMAND, IDM_FULLSCREEN, 0 ) ; return 1 ; }
	else if( key == shortcuts_tab.fontup )			// Font up
		{ SendMessage( hwnd, WM_COMMAND, IDM_FONTUP, 0 ) ; return 1 ; }
	else if( key == shortcuts_tab.fontdown )		// Font down
		{ SendMessage( hwnd, WM_COMMAND, IDM_FONTDOWN, 0 ) ; return 1 ; }
	else if( key == shortcuts_tab.copyall )			// Copy all to clipboard
		{ SendMessage( hwnd, WM_COMMAND, IDM_COPYALL, 0 ) ; return 1 ; }
	else if( key == shortcuts_tab.fontnegative )		// Font negative
		{ SendMessage( hwnd, WM_COMMAND, IDM_FONTNEGATIVE, 0 ) ; return 1 ; }
	else if( key == shortcuts_tab.fontblackandwhite )	// Font black and white
		{ SendMessage( hwnd, WM_COMMAND, IDM_FONTBLACKANDWHITE, 0 ) ; return 1 ; }
	else if( key == shortcuts_tab.keyexchange )		// Repeat key exchange
		{ SendMessage( hwnd, WM_COMMAND, IDM_REKEY, 0 ) ; return 1 ; }
		
#ifndef FLJ
	else if( key == shortcuts_tab.input ) 			// Fenetre de controle
		{ 
			MainHwnd = hwnd ; _beginthread( routine_inputbox, 0, (void*)&hwnd ) ;
			InvalidateRect( hwnd, NULL, TRUE ) ; return 1 ;
		}
#endif

#if (defined MOD_BACKGROUNDIMAGE) && (!defined FLJ)
	else if( GetBackgroundImageFlag() && (key == shortcuts_tab.imagechange) ) 		// Changement d'image de fond
		{ if( NextBgImage( hwnd ) ) InvalidateRect(hwnd, NULL, TRUE) ; return 1 ; }
#endif
/*
	if( control_flag && shift_flag ) {
		if(key_num == VK_UP) { SendMessage( hwnd, WM_COMMAND, IDM_FONTUP, 0 ) ; return 1 ; }
		if(key_num == VK_DOWN) { SendMessage( hwnd, WM_COMMAND, IDM_FONTDOWN, 0 ) ; return 1 ; }
		if(key_num == VK_LEFT ) {  ChangeFontSize(hwnd,0) ; return 1 ; }
	}*/
	if( control_flag && !shift_flag ) {
		if( TransparencyFlag && (conf_get_int(conf,CONF_transparencynumber)!=-1)&&(key_num == VK_UP) ) // Augmenter l'opacite (diminuer la transparence)
			{ SendMessage( hwnd, WM_COMMAND, IDM_TRANSPARUP, 0 ) ; return 1 ; }
		if( TransparencyFlag && (conf_get_int(conf,CONF_transparencynumber)!=-1)&&(key_num == VK_DOWN) ) // Diminuer l'opacite (augmenter la transparence)
			{ SendMessage( hwnd, WM_COMMAND, IDM_TRANSPARDOWN, 0 ) ; return 1 ; }

		if (key_num == VK_ADD) { SendMessage( hwnd, WM_COMMAND, IDM_FONTUP, 0 ) ; return 1 ; }
		if (key_num == VK_SUBTRACT) { SendMessage( hwnd, WM_COMMAND, IDM_FONTDOWN, 0 ) ; return 1 ; }
		if (key_num == VK_NUMPAD0) { ChangeFontSize(term,conf,hwnd,0) ; return 1 ; }
#ifdef MOD_LAUNCHER
		/*    ====> Ne fonctionne pas !!!
		if (key_num == VK_LEFT ) //Fenetre KiTTY precedente
			{ SendMessage( hwnd, WM_COMMAND, IDM_GOPREVIOUS, 0 ) ; return 1 ; }
		if (key_num == VK_RIGHT ) //Fenetre KiTTY Suivante
			{ SendMessage( hwnd, WM_COMMAND, IDM_GONEXT, 0 ) ; return 1 ; }
		*/
#endif
	}
	return 0 ;
}

// shift+bouton droit => paste ameliore pour serveur "lent"
// Le paste utilise la methode "autocommand"
void SetPasteCommand( HWND hwnd ) {
	if( !PasteCommandFlag ) return ;
	if( PasteCommand != NULL ) { free( PasteCommand ) ; PasteCommand = NULL ; }
	char *pst = NULL ;
	if( OpenClipboard(NULL) ) {
		HGLOBAL hglb ;
		if( (hglb = GetClipboardData( CF_TEXT ) ) != NULL ) {
			if( ( pst = GlobalLock( hglb ) ) != NULL ) {
				PasteCommand = (char*) malloc( strlen(pst)+1 ) ;
				strcpy( PasteCommand, pst ) ;
				SetTimer(hwnd, TIMER_AUTOPASTE, autocommand_delay, NULL) ;
				debug_logevent( "Sent paste command" ) ;
				GlobalUnlock( hglb ) ;
				}
			}
		CloseClipboard();
		}
	}
	
// Initialisation des parametres a partir du fichier kitty.ini
#if (defined MOD_BACKGROUNDIMAGE) && (!defined FLJ)
void SetShrinkBitmapEnable(int) ;
#endif

void LoadParameters( void ) {
	char buffer[4096] ;

	/* A lire en premier */
	if( ReadParameterN( INIT_SECTION, "debug", buffer, sizeof(buffer) ) ) { if( !stricmp( buffer, "YES" ) ) debug_flag = 1 ; }
	
	if( ReadParameterN( "Agent", "scrumble", buffer, sizeof(buffer) ) ) { if( !stricmp( buffer, "YES" ) ) SetScrumbleKeyFlag(1) ; }

#ifdef MOD_ADB
	if( ReadParameterN( INIT_SECTION, "adb", buffer, sizeof(buffer) ) ) {
		if( !stricmp( buffer, "YES" ) ) SetADBFlag( 1 ) ; 
		if( !stricmp( buffer, "NO" ) ) SetADBFlag( 0 ) ; 
	}
#endif
	if( ReadParameterN( INIT_SECTION, "antiidle", buffer, sizeof(buffer) ) ) { buffer[127]='\0'; strcpy( AntiIdleStr, buffer ) ; }
	if( ReadParameterN( INIT_SECTION, "antiidledelay", buffer, sizeof(buffer) ) ) 
		{ AntiIdleCountMax = (int)floor(atoi(buffer)/10.0) ; if( AntiIdleCountMax<=0 ) AntiIdleCountMax =1 ; }
	if( ReadParameterN( INIT_SECTION, "autostoresshkey", buffer, sizeof(buffer) ) ) { if( !stricmp( buffer, "YES" ) ) SetAutoStoreSSHKeyFlag( 1 ) ; }
#if (defined MOD_BACKGROUNDIMAGE) && (!defined FLJ)
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
	if( ReadParameterN( INIT_SECTION, "capslock", buffer, sizeof(buffer) ) ) { if( !stricmp( buffer, "YES" ) ) CapsLockFlag = 1 ; }
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
	if( ReadParameterN( INIT_SECTION, "hostkeyextension", buffer, sizeof(buffer) ) ) {
		if( strlen(buffer) > 0 ) { SetHostKeyExtension(buffer) ; }
	}
	if( ReadParameterN( INIT_SECTION, "KiPP", buffer, sizeof(buffer) ) != 0 ) {
		if( decryptstring( GetCryptSaltFlag(), buffer, MASTER_PASSWORD ) ) ManagePassPhrase( buffer ) ;
	}
	if( ReadParameterN( INIT_SECTION, "localcmd", buffer, sizeof(buffer) ) ) {
		if( !stricmp( buffer, "NO" ) ) LocalCmdFlag = 0 ;
		if( !stricmp( buffer, "YES" ) ) LocalCmdFlag = 1 ;
	}
	if( ReadParameterN( INIT_SECTION, "localunsecurecmd", buffer, sizeof(buffer) ) ) {
		if( !stricmp( buffer, "NO" ) ) LocalUnsecureCmdFlag = 0 ;
		if( !stricmp( buffer, "YES" ) ) LocalUnsecureCmdFlag = 1 ;
	}
	if( ReadParameterN( INIT_SECTION, "maxblinkingtime", buffer, sizeof(buffer) ) ) { MaxBlinkingTime=2*atoi(buffer);if(MaxBlinkingTime<0) MaxBlinkingTime=0; }
	if( ReadParameterN( INIT_SECTION, "mouseshortcuts", buffer, sizeof(buffer) ) ) { 
		if( !stricmp( buffer, "NO" ) ) MouseShortcutsFlag = 0 ;
		if( !stricmp( buffer, "YES" ) ) MouseShortcutsFlag = 1 ;
	}
	if( ReadParameterN( INIT_SECTION, "paste", buffer, sizeof(buffer) ) ) { if( !stricmp( buffer, "YES" ) ) PasteCommandFlag = 1 ; }
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
	if( ReadParameterN( INIT_SECTION, "size", buffer, sizeof(buffer) ) ) { if( !stricmp( buffer, "YES" ) ) SizeFlag = 1 ; }
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
	if( ReadParameterN( INIT_SECTION, "wintitle", buffer, sizeof(buffer) ) ) { if( !stricmp( buffer, "NO" ) ) TitleBarFlag = 0 ; }
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
	if( ReadParameterN( INIT_SECTION, "scriptmode", buffer, sizeof(buffer) ) ) { 
		if( !stricmp( buffer, "YES" ) ) RuttyFlag = 1 ;
		if( !stricmp( buffer, "NO" ) ) RuttyFlag = 0 ;
	}
#ifndef MOD_NOTRANSPARENCY
	if( ReadParameterN( INIT_SECTION, "transparency", buffer, sizeof(buffer) ) ) {
		if( !stricmp( buffer, "YES" ) ) { TransparencyFlag = 1 ; }
		else { TransparencyFlag = 0 ; } 
	}
#endif

#if (defined MOD_BACKGROUNDIMAGE) && (!defined FLJ)
	if( ReadParameterN( INIT_SECTION, "shrinkbitmap", buffer, sizeof(buffer) ) ) { if( !stricmp( buffer, "YES" ) ) SetShrinkBitmapEnable(1) ; else SetShrinkBitmapEnable(0) ; }
#endif

	if( readINI( KittyIniFile, "ConfigBox", "dblclick", buffer, sizeof(buffer) ) ) {
		if( !strcmp(buffer,"open") ) { SetDblClickFlag(0) ; }
		if( !strcmp(buffer,"start") ) { SetDblClickFlag(1) ; }
	}
	if( readINI( KittyIniFile, "ConfigBox", "height", buffer, sizeof(buffer) ) ) {
		ConfigBoxHeight = atoi( buffer ) ;
		/* NB: the extra row taken by the Proxy-choice droplist (when shown) is
		 * now accounted for in the config-box window sizing (windows/dialog.c),
		 * not by shrinking the list here. */
	}
	if( readINI( KittyIniFile, "ConfigBox", "windowheight", buffer, sizeof(buffer) ) ) {
		ConfigBoxWindowHeight = atoi( buffer ) ;
	}
	if( readINI( KittyIniFile, "ConfigBox", "noexit", buffer, sizeof(buffer) ) ) {
		if( !stricmp( buffer, "YES" ) ) ConfigBoxNoExitFlag = 1 ;
	}
	if( readINI( KittyIniFile, "ConfigBox", "filter", buffer, sizeof(buffer) ) ) {
		if( !stricmp( buffer, "NO" ) ) SessionFilterFlag = 0 ;
	}
	if( readINI( KittyIniFile, "ConfigBox", "default", buffer, sizeof(buffer) ) ) {
		if( !stricmp( buffer, "NO" ) ) SessionsInDefaultFlag = 0 ;
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
	if( readINI( KittyIniFile, "ConfigBox", "left", buffer, sizeof(buffer) ) ) {
		SetConfigBoxLeft( atoi(buffer) ) ;
	}
	if( readINI( KittyIniFile, "ConfigBox", "top", buffer, sizeof(buffer) ) ) {
		SetConfigBoxTop( atoi(buffer) ) ;
	}
	
	// Param RandomActiveFlag défini dans kitty_commun.c
	// Pour gérer le bug sur certaines machines:  https://github.com/cyd01/KiTTY/issues/113
	if( readINI( KittyIniFile, "Debug", "randomactive", buffer, sizeof(buffer) ) ) {
		if( !stricmp( buffer, "NO" ) ) SetRandomActiveFlag( 0 ) ;
		if( !stricmp( buffer, "YES" ) ) SetRandomActiveFlag( 1 ) ;
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
		sprintf( buffer, "%s\\%s", InitialDirectory, DEFAULT_INIT_FILE ) ;
		if( !existfile( buffer ) ) {
			sprintf( buffer, "%s\\putty.ini", InitialDirectory ) ;
			if( !existfile( buffer ) ) {
				if( IniFileFlag != SAVEMODE_DIR ) {
					sprintf( buffer, "%s\\%s\\%s", getenv("APPDATA"), INIT_SECTION, DEFAULT_INIT_FILE ) ;
					if( !existfile( buffer ) ) {
						sprintf( buffer, "%s\\%s", getenv("APPDATA"), INIT_SECTION ) ;
						CreateDirectory( buffer, NULL ) ;
						sprintf( buffer, "%s\\%s\\%s", getenv("APPDATA"), INIT_SECTION, DEFAULT_INIT_FILE ) ;
					}
				} else {
					sprintf( buffer, "%s\\%s", InitialDirectory, DEFAULT_INIT_FILE ) ;
				}
			}
		}
	}
	KittyIniFile=(char*)malloc( strlen( buffer)+2 ) ; strcpy( KittyIniFile, buffer) ;

	if( KittySavFile != NULL ) { free( KittySavFile ) ; } 
	KittySavFile=NULL ;
	sprintf( buffer, "%s\\%s", InitialDirectory, DEFAULT_SAV_FILE ) ;
	if( !existfile( buffer ) ) {
		if( IniFileFlag != SAVEMODE_DIR ) {
			sprintf( buffer, "%s\\%s\\%s", getenv("APPDATA"), INIT_SECTION, DEFAULT_SAV_FILE ) ;
			if( !existfile( buffer ) ) {
				sprintf( buffer, "%s\\%s", getenv("APPDATA"), INIT_SECTION ) ;
				CreateDirectory( buffer, NULL ) ;
				sprintf( buffer, "%s\\%s\\%s", getenv("APPDATA"), INIT_SECTION, DEFAULT_SAV_FILE ) ;
			}
		}
	}
	KittySavFile=(char*)malloc( strlen( buffer)+2 ) ; strcpy( KittySavFile, buffer) ;
	
	sprintf( buffer, "%s\\kitty.dft", InitialDirectory ) ;
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
	
	// Recherche plink s'il existe
	SearchPlink() ;

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
	
#ifdef FLJ
	CreateSSHHandler();
	CreateFileAssoc() ;
	SetADBFlag(0) ;
#else
	//if( !RegTestKey(HKEY_CLASSES_ROOT,"kitty.connect.1") ) { CreateFileAssoc() ; }
#endif

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

#if (defined MOD_PERSO) && (!defined FLJ)
	if( ReadParameterN( INIT_SECTION, "KiClassName", buffer, sizeof(buffer) ) )
		{ if( (strlen(buffer)>0) && (strlen(buffer)<128) ) { buffer[127]='\0'; strcpy( KiTTYClassName, buffer ) ; } }
	appname = KiTTYClassName ;
	/* Select the registry hive to match KiClassName: default KiTTY's own
	 * (Software\9bis.com\KiTTY); PuTTY's hive when KiClassName=PuTTY. */
	{ extern void kitty_set_registry_root(int use_putty);
	  kitty_set_registry_root( !stricmp(KiTTYClassName, "PuTTY") ) ; }
#endif

	// Initialise le tableau des menus
	for( i=0 ; i < NB_MENU_MAX ; i++ ) SpecialMenu[i] = NULL ;
	
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
	sprintf( buffer, "%s\\%s", TEXT(PUTTY_REG_POS), "Commands" ) ;
	if( (IniFileFlag == SAVEMODE_REG)||( IniFileFlag == SAVEMODE_FILE) ) 
		RegTestOrCreate( HKEY_CURRENT_USER, buffer, NULL, NULL ) ;

#ifdef MOD_PROXY
	// Initiate proxies list
	InitProxyList() ;
#endif
#ifdef MOD_LAUNCHER
	// Initiate launcher
	sprintf( buffer, "%s\\%s", TEXT(PUTTY_REG_POS), "Launcher" ) ;
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
	sprintf( buffer, "%s\\%s.ses.updt", InitialDirectory, appname ) ;
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
	sprintf( buffer, "Starting %ld from %s@%s", GetCurrentProcessId(), username, hostname ) ;
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
void InitShortcuts( void ) ;
int DefineShortcuts( char * buf ) ;


// Gestion des raccourcis
int ManageShortcuts( Terminal *term, Conf *conf, HWND hwnd, const int* clips_system, int key_num, int shift_flag, int control_flag, int alt_flag, int altgr_flag, int win_flag ) ;

// Nettoie la clé de PuTTY pour enlever les clés et valeurs spécifique à KiTTY
// Se trouve dans le fichier kitty_registry.c
BOOL RegCleanPuTTY( void ) ;

// Envoi de caractères
void SendKeyboardPlus( HWND hwnd, const char * st ) ;

// Envoi d'une commande à l'écran
void SendAutoCommand( HWND hwnd, const char * cmd ) ;
