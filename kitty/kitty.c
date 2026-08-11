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
#include "winfont_fallback.h"
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
/* KiTTY: warn above 5120 characters by default - the same threshold Windows
 * Terminal uses for its large-paste warning. It used to be 0, i.e. no check,
 * which meant a mis-aimed paste of a whole file went to the shell unasked.
 * 0 still disables the warning; see the reader in LoadParameters(). */
static int PasteSize = 5120 ;
int GetPasteSize(void) { return PasteSize ; }
void SetPasteSize( const int size ) { PasteSize = size ; }

/* How many SSH proxies may be chained before we refuse. An SSH jump host is
 * configured from a Conf that may itself name a proxy, so a config that leads
 * back into the chain recurses without bound: observed as a wall of
 * "Making proxy^N SSH connection to ..." and a window that stops responding.
 * There is no identity or cycle rule here on purpose - a jump host reached
 * through itself is legitimate (a service bound to its own localhost, an
 * internal interface that routes differently) and we cannot know how anyone's
 * hosts are named. A depth BOUND is the only honest limit.
 * kitty.ini [KiTTY] proxychainmax=<n> raises it; 0 or absent keeps the default. */
static int ProxyChainMax = 5 ;
int GetProxyChainMax(void) { return ProxyChainMax ; }
void SetProxyChainMax( const int n ) { if( n > 0 ) ProxyChainMax = n ; }

/* How a named proxy's Host is read when the proxy itself does not say
 * (kitty.ini [KiTTY] namedproxy):
 *   sessionorhostname  the title of a saved session first, then a hostname -
 *                      what PuTTY has always done, and the DEFAULT, because
 *                      existing configurations may rely on it;
 *   hostname           a hostname, full stop.
 * A proxy that DOES say overrides this, per definition. The silent
 * substitution - a jump host that happens to share a name with a saved session
 * quietly dragging that session's whole config in - is the reason a proxy can
 * now say. */
static int NamedProxyHostnameOnly = 0 ;
int kitty_named_proxy_default_hostname( void ) { return NamedProxyHostnameOnly ; }
void SetNamedProxyHostnameOnly( const int flag ) { NamedProxyHostnameOnly = flag ? 1 : 0 ; }

/* [KiTTY] funkeys: the function-key mode a session that has none of its own
 * starts with. -1 means "say nothing", which is the default and leaves PuTTY's
 * own default (ESC[n~) in place. The values are the modes the Keyboard panel
 * lists, spelled as the panel spells them (cyd01/KiTTY#556 - see the comment on
 * the load hook in windows/putty.c for why anyone wants this).
 * NOTE: the ini parser matches keys case-SENSITIVELY, so this is "funkeys". */
static int FunkeysDefault = -1 ;
int GetFunkeysDefault( void ) { return FunkeysDefault ; }
void SetFunkeysDefault( const int t ) { FunkeysDefault = t ; }

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
/* DEFAULT_SAV_FILE is defined in kitty.h (included above) - do NOT add a
 * second #ifndef definition here, it would be dead code. */
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

/* KiTTY: the multiple-icon feature is gone - the [KiTTY] icon and
 * numberoficons keys, IconeFlag, IconeNum and NumberOfIcons with it. Nothing
 * ever asked SetNewIcon to cycle or randomise (every caller passes SI_INIT),
 * and the ini key could only switch the flag on, so the feature did nothing.
 * The 50 embedded icons remain and are still selectable per session, by
 * number or as an external file (CONF_icone / CONF_iconefile). */
#ifndef MOD_PERSO
#define IDI_MAINICON_0 1
#define IDC_RESULT 1008
#endif

// La librairie dans laquelle chercher les icones (fichier defini dans kitty.ini, sinon kitty.dll s'il existe, sinon kitty.exe)
static HINSTANCE hInstIcons =  NULL ;
HINSTANCE GethInstIcons(void) { return hInstIcons ; }
void SethInstIcons( const HINSTANCE h ) { hInstIcons = h ; }

// Fichier contenant les icones à charger
static char * IconFile = NULL ;

// [KiTTY] size=yes: append the live terminal size [rows x cols] to the window
// title (not while maximized). Applied by the title decorator in
// windows/window.c; needs wintitle=yes (TitleBarFlag) like classic KiTTY.
static int SizeFlag = 0 ;
int GetSizeFlag(void) { return SizeFlag ; }
void SetSizeFlag( const int flag ) { SizeFlag = flag ; }

// [KiTTY] wintitle=yes (default): enable KiTTY's title decorations - the size
// suffix (size=yes) and the (PROTECTED)/(ONTOP) status markers. wintitle=no =
// plain stock titles. SECURITY: unlike classic KiTTY the title text is never
// PARSED (the __xy title-scan dispatcher stays dead) - decoration is strictly
// one-way output in windows/window.c wintw_set_title.
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
/* "the configuration store changed" flag - implemented in kitty_storage.c so
 * that windows/storage.c can set it too. Declared up here because SaveFolderList
 * below is the first user. */
void kitty_store_mark_dirty( void ) ;
int kitty_store_take_dirty( void ) ;
/* Startup cleanup of the master password the old export behaviour created as a
 * side effect - implemented in kitty_storage.c, which owns the store scan. */
void kitty_retire_orphan_master_password( void ) ;
int kitty_migrate_portable_mpw_state( void ) ;
void kitty_show_mpw_moved( HWND hwnd ) ;

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

// [ConfigBox] noexit=yes: when a window that ran a connected session closes,
// spawn a fresh instance (= the config box) so you land back in the session
// picker. Gated on is_backend_first_connected at exit (windows/window.c), so a
// config-box-only process never respawns - classic KiTTY's version fired on
// config-box exit too, which is the bug that kept it broken there.
static int ConfigBoxNoExitFlag = 0 ;
int GetConfigBoxNoExitFlag(void) { return ConfigBoxNoExitFlag ; }
void SetConfigBoxNoExitFlag( const int flag ) { ConfigBoxNoExitFlag = flag ; }

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

/* KiTTY (hknet/KiTTY#23) [ConfigBox] loadlastsession=no: open the
 * configuration box on Default Settings instead of pre-filling it with the
 * session used last, and put the caret straight into "Host Name (or IP
 * address)" -- the quick-connect way of working, where a host is typed rather
 * than picked, and every connection is expected to start from the same
 * defaults. Default yes = the behaviour of this port so far. */
static int LoadLastSessionFlag = 1 ;
int GetLoadLastSessionFlag(void) { return LoadLastSessionFlag ; }
void SetLoadLastSessionFlag( const int flag ) { LoadLastSessionFlag = flag ; }

/* KiTTY (hknet/KiTTY#23): quick connect for THIS run of the configuration box.
 * Set at startup either by loadlastsession=no, or because the session used
 * last was "Default Settings" - loading the defaults arms quick connect and it
 * stays armed until some other session is loaded, so the two ways of working
 * need no switch flipped between them. Runtime state, not a kitty.ini key. */
static int QuickConnectMode = 0 ;
int GetQuickConnectMode(void) { return QuickConnectMode ; }
void SetQuickConnectMode( const int flag ) { QuickConnectMode = flag ; }

// [ConfigBox] dblclick: what a double-click on a saved session does.
// 0 = open (load it and open in this window, like the Open button; default);
// 1 = start (launch it in a NEW window and keep the config box open, like the
// Start button). Consumed by sessionsaver_handler in kitty_config.c.
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

/* --- OSC 7 remote working-directory tracking (data-plane only) -------------
 * A shell with directory reporting emits ESC ] 7 ; file://host/path BEL on
 * every prompt.  do_osc() (terminal.c) hands us the payload; we validate and
 * store the path so kitty_current_dir() can offer it as the default remote
 * target for drag-drop pscp uploads and StartWinSCP.  Opt-in per session
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

char * kitty_current_dir() {
	/* Respect the CURRENT setting, not just what it was when the cwd was stored:
	 * if the user turns OSC 7 tracking off at runtime (Change Settings), stop
	 * offering the tracked directory immediately so uploads fall back to the
	 * fixed remote dir / home, matching a fresh start with it disabled. */
	if( conf == NULL || !conf_get_bool( conf, CONF_osc7_cwd_tracking ) ) return NULL ;
	if( RemoteCwd[0] == '\0' ) return NULL ;
	return RemoteCwd ;
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
		WriteParameter( INIT_SECTION, "Folders", buffer ) ;
	}

// Sauvegarde une cle de registre dans un fichier

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
	
	/*
	 * KiTTY: KiLastUp, KiLastUH, KiSess, KiVers and KiPath used to be written
	 * here on every run. They are gone (2026-08-02), and nothing replaces them.
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

/* Run Windows' own registry tool, hidden, and report whether it succeeded.
 * `hive` is NULL for verbs that take only a file (import). */
static int kitty_reg_tool( const char *verb, const char *hive, const char *path ) {
	char sysdir[MAX_PATH], cmd[8192] ;
	STARTUPINFOA si ; PROCESS_INFORMATION pi ; DWORD rc = 1 ;
	if( GetSystemDirectoryA( sysdir, sizeof(sysdir) ) == 0 ) return 0 ;
	if( hive != NULL )
		snprintf( cmd, sizeof(cmd), "\"%s\\reg.exe\" %s \"%s\" \"%s\" /y", sysdir, verb, hive, path ) ;
	else
		snprintf( cmd, sizeof(cmd), "\"%s\\reg.exe\" %s \"%s\"", sysdir, verb, path ) ;
	memset( &si, 0, sizeof(si) ) ; si.cb = sizeof(si) ;
	si.dwFlags = STARTF_USESHOWWINDOW ; si.wShowWindow = SW_HIDE ;
	if( !CreateProcessA( NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi ) ) return 0 ;
	WaitForSingleObject( pi.hProcess, 60000 ) ;
	if( !GetExitCodeProcess( pi.hProcess, &rc ) ) rc = 1 ;
	CloseHandle( pi.hThread ) ; CloseHandle( pi.hProcess ) ;
	return ( rc == 0 ) ;
	}

/* Import a .reg file produced by SaveRegistryKeyEx(). */
static int kitty_reg_import( const char *filename ) {
	return kitty_reg_tool( "import", NULL, filename ) ;
	}

// Sauvegarde de la cle de registre
/* The backup is produced by Windows' own exporter rather than a hand-rolled
 * serialiser. The QueryKey() this replaces did not write REG_BINARY values at
 * all (and, reusing its line buffer, emitted the previous line again in their
 * place), wrote REG_MULTI_SZ and REG_EXPAND_SZ as plain strings so they came
 * back with the wrong type, dropped anything past a fixed 1 KB, and could
 * overflow that buffer on a long value name. reg.exe gets all of it right and
 * produces a genuine .reg the user can read or import by hand. */
void SaveRegistryKeyEx( HKEY hMainKey, LPCTSTR lpSubKey, const char * filename ) {
	char hive[4096] ;
	const char * root ;
	if( hMainKey == HKEY_CURRENT_USER ) root = "HKCU" ;
	else if( hMainKey == HKEY_LOCAL_MACHINE ) root = "HKLM" ;
	else return ;
	snprintf( hive, sizeof(hive), "%s\\%s", root, TEXT(lpSubKey) ) ;
	/* reg.exe /y overwrites, but a stale file must not survive a failed export. */
	unlink( filename ) ;
	kitty_reg_tool( "export", hive, filename ) ;
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
	/* FindFirstFile understands * and ? only - it has no character classes, so
	 * the "[0-9]" this used to carry was matched LITERALLY, nothing was ever
	 * found, and pruning silently never happened (portablebackupcount was
	 * therefore ignored and the backup folders grew without bound). The
	 * "-latest" directory is filtered out by name just below. */
	snprintf( pattern, sizeof(pattern), "%s\\kitty-portable-*", root ) ;
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

/* What must NOT go into a backup. Everything else in the portable directory is
 * treated as configuration and copied, so a store that gains a new folder or
 * file is covered without anyone having to remember a list - the allowlist this
 * replaces had silently omitted "Launcher" ever since the feature shipped.
 *
 * Left out: the Backups tree itself (it holds the older copies, and the
 * destination lives inside it, so copying it would nest backups inside
 * backups); the programs; and the bulky by-products that are not configuration
 * - session logs, and any leftover .dmp diagnostic dumps from the removed
 * savedump feature, which are large and hold secrets. */
static int portable_backup_skip( const WIN32_FIND_DATAA *fd ) {
	static const char *skipdirs[] = { "Backups", NULL } ;
	static const char *skipexts[] = { ".exe", ".dll", ".log", ".dmp", NULL } ;
	const char *dot ;
	int i ;
	if( !strcmp(fd->cFileName,".") || !strcmp(fd->cFileName,"..") ) return 1 ;
	if( fd->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) {
		for( i=0 ; skipdirs[i]!=NULL ; i++ )
			if( !stricmp( fd->cFileName, skipdirs[i] ) ) return 1 ;
		return 0 ;
	}
	dot = strrchr( fd->cFileName, '.' ) ;
	if( dot != NULL )
		for( i=0 ; skipexts[i]!=NULL ; i++ )
			if( !stricmp( dot, skipexts[i] ) ) return 1 ;
	return 0 ;
}

static void portable_backup_write_one( const char *dst ) {
	char pattern[4096], s[4096], d[4096] ;
	WIN32_FIND_DATAA fd ;
	HANDLE h ;
	DelDir( dst ) ;
	CreateDirectoryA( dst, NULL ) ;
	/* kitty.ini can be resolved from outside the portable directory, so it is
	 * copied by its resolved path and under its canonical name. When it does
	 * live here, the sweep below simply copies it again over the same file. */
	if( KittyIniFile != NULL && strlen(KittyIniFile)>0 && existfile(KittyIniFile) ) {
		snprintf( d, sizeof(d), "%s\\kitty.ini", dst ) ;
		CopyFileA( KittyIniFile, d, FALSE ) ;
	}
	if( ConfigDirectory == NULL || strlen(ConfigDirectory) == 0 ) return ;
	snprintf( pattern, sizeof(pattern), "%s\\*", ConfigDirectory ) ;
	h = FindFirstFileA( pattern, &fd ) ;
	if( h == INVALID_HANDLE_VALUE ) return ;
	do {
		if( portable_backup_skip( &fd ) ) continue ;
		snprintf( s, sizeof(s), "%s\\%s", ConfigDirectory, fd.cFileName ) ;
		snprintf( d, sizeof(d), "%s\\%s", dst, fd.cFileName ) ;
		if( fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY )
			portable_backup_copy_tree( s, d ) ;
		else
			CopyFileA( s, d, FALSE ) ;
	} while( FindNextFileA( h, &fd ) ) ;
	FindClose( h ) ;
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

/* Backups written before the DEFAULT_SAV_FILE shadowing was fixed (2026-07-26)
 * are named kitty-YYYYMMDD-HHMMSS.sav, because the intended kittynew.sav default
 * never took effect. Once it does, sav_find_newest() no longer sees them, so a
 * machine upgrading from such a build would silently skip its first-run restore.
 * Resolve the legacy name too - for READING ONLY; nothing is ever written back
 * under it, and only when the sav path is still the default (a user-configured
 * sav= is taken literally). Returns 1 + path in out. */
#define LEGACY_SAV_FILE "kitty.sav"
static int sav_find_newest_legacy( const char *savfile, char *out, size_t outlen ) {
	char dir[4096], base[256], ext[64], legacy[4096] ;
	char ddir[16], dbase[256], dext[64] ;
	sav_split( savfile, dir, sizeof(dir), base, sizeof(base), ext, sizeof(ext) ) ;
	sav_split( DEFAULT_SAV_FILE, ddir, sizeof(ddir), dbase, sizeof(dbase), dext, sizeof(dext) ) ;
	if( strcmp( base, dbase ) ) return 0 ;              /* custom sav=: no fallback */
	snprintf( legacy, sizeof(legacy), "%s\\%s", dir, LEGACY_SAV_FILE ) ;
	if( sav_find_newest( legacy, out, outlen ) ) return 1 ;
	if( existfile( legacy ) ) { snprintf( out, outlen, "%s", legacy ) ; return 1 ; }
	return 0 ;
}

/* The one place that decides WHICH backup to restore from: newest timestamped,
 * else the fixed-name file, else the legacy name above. */
static int sav_find_for_restore( const char *savfile, char *out, size_t outlen ) {
	if( sav_find_newest( savfile, out, outlen ) ) return 1 ;
	if( existfile( savfile ) ) { snprintf( out, outlen, "%s", savfile ) ; return 1 ; }
	return sav_find_newest_legacy( savfile, out, outlen ) ;
}

/* The configuration password (/configpassword) is retired: it encrypted the
 * .sav while storing its own key in the CLEAR in the hive, and the kitty.ini
 * copy was only obfuscated with a constant compiled into every build. Nothing
 * reads either value any more, so remove them rather than leave a plaintext
 * secret lying about. Self-limiting: both deletes are skipped when absent. */
void RetireConfigPasswordLeftovers( void ) {
	char buf[4096] ;
	if( GetValueDataN( HKEY_CURRENT_USER, TEXT(PUTTY_REG_POS), "password", buf, sizeof(buf) ) != NULL )
		RegDelValue( HKEY_CURRENT_USER, TEXT(PUTTY_REG_POS), "password" ) ;
	if( ( KittyIniFile != NULL ) && !GetReadOnlyFlag()
	    && readINI( KittyIniFile, INIT_SECTION, "password", buf, sizeof(buf) ) )
		delINI( KittyIniFile, INIT_SECTION, "password" ) ;
	memset( buf, 0, sizeof(buf) ) ;
	}

/*
 * CountUp() used to write five bookkeeping values on every run - KiLastUp,
 * KiLastUH, KiSess, KiVers, KiPath - that nothing ever read back. The writes are
 * gone; this removes them from stores that already have them, because stopping
 * the writes on its own would just freeze stale values in place for ever.
 *
 * Two of them mattered more than the rest. KiLastUH held your Windows username
 * and computer name, and KiVers your OS version, both scrambled with the
 * compiled-in public constant - which is obfuscation, not encryption, and which
 * made the leak invisible to the one person who would care: kitty.ini gets
 * shared, and nobody reading their own file could tell those two lines were
 * their username and machine.
 *
 * Self-limiting, like RetireConfigPasswordLeftovers above: every delete is
 * skipped when the value is absent, so this costs nothing on a clean store and
 * runs at most once usefully.
 */
void RetireCountUpLeftovers( void ) {
	/* char* rather than const char*: RegDelValue takes LPTSTR. */
	static char *const dead[] = {
		"KiLastUp", "KiLastUH", "KiSess", ";KiSess", "KiVers", "KiPath" } ;
	char buf[4096] ;
	size_t i ;
	for( i = 0 ; i < lenof(dead) ; i++ ) {
		if( GetValueDataN( HKEY_CURRENT_USER, TEXT(PUTTY_REG_POS), dead[i], buf, sizeof(buf) ) != NULL )
			RegDelValue( HKEY_CURRENT_USER, TEXT(PUTTY_REG_POS), dead[i] ) ;
		if( ( KittyIniFile != NULL ) && !GetReadOnlyFlag()
		    && readINI( KittyIniFile, INIT_SECTION, dead[i], buf, sizeof(buf) ) )
			delINI( KittyIniFile, INIT_SECTION, dead[i] ) ;
	}
	memset( buf, 0, sizeof(buf) ) ;
	}

/* The export spawns reg.exe, so it runs on a worker thread rather than making
 * the config box wait for it - upstream did the same with _beginthread. One at
 * a time: a request arriving while one is in flight is dropped, since it would
 * only write a near-identical snapshot a moment later. */
static LONG sav_worker_busy = 0 ;

struct sav_job { char dated[4096] ; char base[4096] ; int keep ; } ;

static DWORD WINAPI sav_worker( LPVOID param ) {
	struct sav_job * j = (struct sav_job *)param ;
	SaveRegistryKeyEx( HKEY_CURRENT_USER, TEXT(PUTTY_REG_POS), j->dated ) ;
	sav_prune( j->base, j->keep ) ;
	free( j ) ;
	InterlockedExchange( &sav_worker_busy, 0 ) ;
	return 0 ;
	}

/* async=0 blocks until the backup is on disk. Callers that are about to DESTROY
 * something - delete a session or a folder, overwrite a saved session - must use
 * that: the whole point of those backups is to capture the store as it was
 * BEFORE the change, and a worker thread could just as easily snapshot it after.
 * Portable (dir) mode is always synchronous; it copies files rather than
 * spawning a process, and its backup has the same ordering requirement. */
static void sav_backup( int async ) {
	int keep = 5 ; char kb[64] ;
	/* Routine snapshots happen only when the store actually changed since the
	 * last one. The path that triggers them is also what plain Enter in the
	 * session list goes through, so without this a handful of opens would push
	 * every interesting backup out of the retention window. The blocking variant
	 * deliberately ignores the flag: it runs BEFORE a destructive edit, where
	 * capturing the prior state is the whole point whether or not anything was
	 * edited first. */
	if( async && !kitty_store_take_dirty() ) return ;
	if( IniFileFlag == SAVEMODE_DIR ) { SavePortableDirBackup() ; return ; }
	if( NoKittyFileFlag || (KittySavFile==NULL) ) return ;
	if( strlen(KittySavFile)==0 ) return ;

	if( ReadParameterN( INIT_SECTION, "savbackupcount", kb, sizeof(kb) ) ) keep = atoi( kb ) ;
	if( keep <= 0 ) return ;              /* savbackupcount=0 disables the backup */
	if( keep > 50 ) keep = 50 ;
	/* Write a FRESH timestamped file now (kittynew-YYYYMMDD-HHMMSS.sav) so its
	 * name matches its content's save time, then keep only the newest `keep`.
	 * (The old scheme wrote a fixed kittynew.sav and renamed the PREVIOUS one to
	 * a now-stamped name - the timestamp then lied about the content's age.) */
	{ char dated[4096] ;
	  sav_timestamped_path( KittySavFile, dated, sizeof(dated) ) ;
	  if( !async ) {
		SaveRegistryKeyEx( HKEY_CURRENT_USER, TEXT(PUTTY_REG_POS), dated ) ;
		sav_prune( KittySavFile, keep ) ;
		return ;
		}
	  if( InterlockedCompareExchange( &sav_worker_busy, 1, 0 ) != 0 ) return ;
	  { struct sav_job * j = (struct sav_job *)malloc( sizeof(*j) ) ;
	    HANDLE th ;
	    if( j == NULL ) { InterlockedExchange( &sav_worker_busy, 0 ) ; return ; }
	    snprintf( j->dated, sizeof(j->dated), "%s", dated ) ;
	    snprintf( j->base, sizeof(j->base), "%s", KittySavFile ) ;
	    j->keep = keep ;
	    th = CreateThread( NULL, 0, sav_worker, j, 0, NULL ) ;
	    if( th == NULL ) { free( j ) ; InterlockedExchange( &sav_worker_busy, 0 ) ; return ; }
	    CloseHandle( th ) ;
	  }
	}
	}

/* Routine snapshot after a deliberate config change - never blocks the UI. */
void SaveRegistryKey( void ) { sav_backup( 1 ) ; }

/* Snapshot that must be on disk before the caller changes anything. */
void SaveRegistryKeyNow( void ) { sav_backup( 0 ) ; }

// Charge la cle de registre
void LoadRegistryKey( HWND hdlg ) { // hdlg est la boite de dialogue d'information de l'avancement (si null pas d'info)
	FILE *fp ;
	HKEY hKey = NULL ;
	char buffer[4096], KeyName[1024] = "", ValueName[1024], *Value ;
	char savpath[4096] ;
	int nb=0 ;
	
	if( KittySavFile==NULL ) return ;
	if( strlen(KittySavFile)==0 ) return ;
	snprintf( savpath, sizeof(savpath), "%s", KittySavFile ) ;

	if( ( fp = fopen( savpath, "rb" ) ) == NULL ) {
		/* Nothing under the current name: a store written by a pre-fix build is
		 * still called kitty*.sav. Read it rather than come up empty - this path
		 * matters most in savemode=file, where the .sav IS the session store. */
		if( !sav_find_newest_legacy( KittySavFile, savpath, sizeof(savpath) ) ) return ;
		if( ( fp = fopen( savpath, "rb" ) ) == NULL ) return ;
		}

	/* Current backups are what reg.exe writes: UTF-16LE with a BOM. Let Windows
	 * import those, so every value type is restored exactly as exported. Older
	 * backups are our own ASCII format (optionally encrypted with the retired
	 * configuration password) and are still parsed below. */
	{	unsigned char bom[2] ;
		if( ( fread( bom, 1, 2, fp ) == 2 ) && ( bom[0] == 0xFF ) && ( bom[1] == 0xFE ) ) {
			fclose( fp ) ;
			if( hdlg != NULL ) InfoBoxSetText( hdlg, "Loading saved sessions." ) ;
			kitty_reg_import( savpath ) ;
			return ;
			}
		rewind( fp ) ;
		}
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
				/* Decrypt-only: the configuration password is retired, so the
				 * value is never written back to the hive or kitty.ini. */
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
		} else {
			CheckMenuItem( m, (UINT)IDM_PROTECT, MF_BYCOMMAND|MF_UNCHECKED ) ;
			ProtectFlag = 0 ;
		}
		/* re-decorate the CURRENT title (keeps a remote OSC-set title intact;
		 * the old set_title(tw,title) reset it to the config template) */
		kitty_refresh_title() ;
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
	AppendMenu( menu, MF_ENABLED, IDM_QUIT, "E&xit" ) ;
		
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

/* /save + /savenew <name>: write the LIVE settings to a saved session. With a
 * name, the window's session identity switches to it first (save-as), so later
 * /save calls and save-on-exit land there. Mirrors the config-box Save button
 * (folder default + launcher refresh broadcast, cf. kitty_config.c). The
 * classic /save (.ktx file exporter, SaveCurrentSetting) lives on as /savektx. */
void kitty_set_last_session(const char *sessionname);   /* kitty_storage.c */
static void kitty_save_current_session( HWND hwnd, const char * newname ) {
	char buffer[1024] ;
	char * errmsg ;
	if( newname != NULL ) {
		while( newname[0]==' ' ) newname++ ;
		if( newname[0]=='\0' ) return ;
		conf_set_str( conf, CONF_sessionname, newname ) ;
	}
	if( strlen( conf_get_str(conf,CONF_folder) ) == 0 ) conf_set_str( conf, CONF_folder, "Default" ) ;
	errmsg = save_settings( conf_get_str(conf,CONF_sessionname), conf ) ;
	if( errmsg != NULL ) {
		MessageBox( hwnd, errmsg, "Save session", MB_OK|MB_ICONERROR ) ;
		sfree( errmsg ) ;
		return ;
	}
	if( newname != NULL ) kitty_set_last_session( conf_get_str(conf,CONF_sessionname) ) ;
	{	/* same best-effort refresh broadcast as the config-box Save button */
		UINT msg = RegisterWindowMessageA( "KiTTYLauncherRefreshSessionsAndHotkeys" ) ;
		if( msg ) PostMessageA( HWND_BROADCAST, msg, 0, 0 ) ;
	}
	snprintf( buffer, sizeof(buffer), "Settings saved to session\n-%s-", conf_get_str(conf,CONF_sessionname) ) ;
	MessageBox( hwnd, buffer, "Save session", MB_OK|MB_ICONINFORMATION ) ;
}

#include "kitty_commands.c"



	
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

/* At-rest protection for the login script: the same chokepoint saved passwords
 * use (kitty_storage.c), plus the base64 pair, because the script is a
 * NUL-separated blob rather than a C string. kitty_proxy.c declares the wrap the
 * same way - these live in kitty_storage.c but not all of them in its header. */
extern char *kitty_secret_wrap_current_backend( const char *plaintext ) ;
extern char *ksec_b64_encode( const unsigned char *in, int len ) ;
extern unsigned char *ksec_b64_decode( const char *in, int *outlen ) ;
extern int ksec_unprotect( const char *stored, char **out ) ;
extern int ksec_stored_is_legacy( const char *stored ) ;
extern char *kitty_loginscript_blob_to_lines( const unsigned char *blob, int len ) ;
extern unsigned char *kitty_loginscript_lines_to_blob( const char *text, int *outlen ) ;

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

	if( !ksec_stored_is_legacy( stored ) &&
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
			 * path and breaks it. ksec_stored_is_legacy tests the markers.
			 */
			if( !ksec_stored_is_legacy( name ) &&
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

/*
 * Most kitty.ini keys are plain "keyword sets a flag" or "number sets an
 * int": those are described by ini_params[] below and applied in one pass
 * by load_ini_params(). yes/no/other give the value stored when the key
 * holds that keyword (matched case-insensitively; INIP_IGNORE = leave the
 * current value untouched, i.e. that keyword has no effect - several keys
 * deliberately work only one way, e.g. size=yes can enable but never
 * disable). Integer keys store atoi(), clamped up to intmin when that is
 * not INIP_IGNORE. The value lands in *var or setter(v). Keys with richer
 * semantics (path existence checks, string copies, scaled floats, nested
 * or conditional reads, non-yes/no keywords) stay hand-written in
 * LoadParameters.
 */
#define INIP_IGNORE (-999999)
typedef struct {
	const char * section ;
	const char * key ;
	int use_readini ;	/* 1 = readINI(KittyIniFile,..) - plain ini-file key;
				 * 0 = ReadParameterN (ini with registry fallback) */
	int is_int ;		/* 1 = atoi() value; 0 = yes/no keyword */
	int yes, no, other ;	/* keyword -> stored value (INIP_IGNORE = skip) */
	int intmin ;		/* int keys: clamp up to this (INIP_IGNORE = none) */
	int * var ;		/* exactly one of var / setter is set */
	void (*setter)(int) ;
} IniParam ;

/*
 * [KiTTY] restrictacl=yes - apply the restricted process ACL to every KiTTY
 * process, without having to add -restrict-acl to each shortcut target.
 *
 * This setter keeps NO state of its own: the single source of truth is
 * security.c's acl_restricted, read back through restricted_acl(). That
 * matters because the &R propagation to spawned windows (kitty_bridge.c,
 * kitty_launcher.c) keys off restricted_acl(), so it picks this up for free.
 *
 * We run EARLIER than the command-line switch, not later: LoadParameters() is
 * called from InitWinMain() near the top of WinMain, while -restrict-acl and
 * the &R prefix are handled further down in gui_term_process_cmdline().
 *
 * There is deliberately no way back off: restrictacl=no leaves the current
 * state alone (INIP_IGNORE below) rather than pretending it can un-restrict a
 * process that was started with -restrict-acl or &R. The restricted_acl()
 * guard only avoids a redundant second SetSecurityInfo when a child that was
 * already restricted via &R also reads restrictacl=yes.
 *
 * Note this inherits restrict_process_acl()'s fail-closed behaviour: if the
 * ACL cannot be applied it bombs out via modalfatalbox rather than run
 * unprotected. That is upstream's deliberate choice for the switch, and it is
 * the right one for an opt-in hardening key too.
 *
 * The table row below uses use_readini=1 - kitty.ini ONLY - which every other
 * [KiTTY] key does not. Those go through ReadParameterN, which on an installed
 * copy reads HKCU FIRST and falls back to kitty.ini only when the registry has
 * no such value (see ReadParameterN above, and the precedence hazard written
 * up in design/SETTINGS_STORAGE_MODEL.md). For a security switch that ordering
 * fails OPEN: restrictacl=yes in kitty.ini would be silently ignored whenever
 * a stale registry value exists - including one left by another install, since
 * the hive is shared by name. Nothing ever writes this key to the registry, so
 * reading it from there could only ever surprise. kitty.ini is the only place
 * it is honoured, in every save mode.
 */
static void SetRestrictAclFlag( const int flag ) {
	if( flag && !restricted_acl() ) { restrict_process_acl() ; }
}

#define IGN INIP_IGNORE
/* keyword key: values for yes / no / anything-else */
#define INIP_KW(sec,rdini,k,y,n,o,v,fn)		{ sec, k, rdini, 0, y, n, o, IGN, v, fn }
/* integer key: atoi, clamped up to min */
#define INIP_NUM(sec,rdini,k,min,v,fn)		{ sec, k, rdini, 1, 0, 0, 0, min, v, fn }

static const IniParam ini_params[] = {
	/* "debug" stays first (historical "A lire en premier"). */
	INIP_KW( INIT_SECTION, 0, "debug",		1, IGN, IGN,	&debug_flag, NULL ),
#ifdef MOD_BACKGROUNDIMAGE
	INIP_KW( INIT_SECTION, 0, "bgimage",		1, 0, IGN,	NULL, SetBackgroundImageFlag ),
#endif
	INIP_NUM( INIT_SECTION, 0, "bcdelay",		IGN,		&between_char_delay, NULL ),
	/* conf=no: do NOT auto-create kitty.ini/kitty.sav */
	INIP_KW( INIT_SECTION, 0, "conf",		IGN, 1, IGN,	&NoKittyFileFlag, NULL ),
	INIP_NUM( INIT_SECTION, 0, "cryptsalt",		IGN,		NULL, SetCryptSaltFlag ),
	INIP_KW( INIT_SECTION, 0, "ctrltab",		IGN, 0, IGN,	NULL, SetCtrlTabFlag ),
	INIP_KW( INIT_SECTION, 0, "hyperlink",		1, 0, IGN,	&HyperlinkFlag, NULL ),
	INIP_NUM( INIT_SECTION, 0, "internaldelay",	1,		&internal_delay, NULL ),
	INIP_KW( INIT_SECTION, 0, "mouseshortcuts",	1, 0, IGN,	&MouseShortcutsFlag, NULL ),
	/* cyd01/KiTTY #548: force classic modal error boxes instead of inline terminal errors */
	INIP_KW( INIT_SECTION, 0, "modalerrors",	1, 0, IGN,	NULL, SetModalErrorsFlag ),
	/* Inline-first security prompts (#548 successor): default yes = classic modal
	 * box; no = OpenSSH-style in-terminal prompt (typed "yes"). */
	INIP_KW( INIT_SECTION, 0, "modalnewhostkeyconfirmation",	1, 0, IGN,	NULL, SetModalNewHostKeyConfirmationFlag ),
	INIP_KW( INIT_SECTION, 0, "modalchangedhostkeyconfirmation",	1, 0, IGN,	NULL, SetModalChangedHostKeyConfirmationFlag ),
	INIP_KW( INIT_SECTION, 0, "modalweakkeyconfirmation",	1, 0, IGN,	NULL, SetModalWeakKeyConfirmationFlag ),
	INIP_KW( INIT_SECTION, 0, "readonly",		1, IGN, IGN,	NULL, SetReadOnlyFlag ),
	/* restrictacl=yes: -restrict-acl for every process; no way back off.
	 * use_readini=1 (kitty.ini ONLY) is deliberate and unlike its [KiTTY]
	 * neighbours - see the comment on SetRestrictAclFlag. */
	INIP_KW( INIT_SECTION, 1, "restrictacl",	1, IGN, IGN,	NULL, SetRestrictAclFlag ),
	INIP_KW( INIT_SECTION, 0, "shortcuts",		1, 0, IGN,	&ShortcutsFlag, NULL ),
	INIP_KW( INIT_SECTION, 0, "size",		1, IGN, IGN,	&SizeFlag, NULL ),
	INIP_NUM( INIT_SECTION, 0, "slidedelay",	IGN,		&ImageSlideDelay, NULL ),
	INIP_KW( INIT_SECTION, 0, "userpasssshnosave",	1, 0, IGN,	NULL, SetUserPassSSHNoSave ),
	INIP_KW( INIT_SECTION, 0, "winroll",		1, 0, IGN,	&WinrolFlag, NULL ),
	/* wintitle=no disables the title decorations; there is no way back on */
	INIP_KW( INIT_SECTION, 0, "wintitle",		IGN, 0, IGN,	&TitleBarFlag, NULL ),
#ifdef MOD_PROXY
	/* proxyselection: yes = always, no = never, auto (or anything else) = when defined */
	INIP_KW( "ConfigBox", 0, "proxyselection",	1, -1, 0,	NULL, SetProxySelectionFlag ),
#endif
#ifdef MOD_ZMODEM
	INIP_KW( INIT_SECTION, 0, "zmodem",		1, 0, IGN,	NULL, SetZModemFlag ),
#endif
#ifdef MOD_RECONNECT
	INIP_KW( INIT_SECTION, 0, "autoreconnect",	IGN, 0, IGN,	&AutoreconnectFlag, NULL ),
	INIP_NUM( INIT_SECTION, 0, "ReconnectDelay",	1,		&ReconnectDelay, NULL ),
#endif
	INIP_KW( INIT_SECTION, 0, "scriptmode",		1, 0, IGN,	NULL, kitty_script_set_enabled ),
#ifndef MOD_NOTRANSPARENCY
	/* transparency: anything but an explicit yes disables */
	INIP_KW( INIT_SECTION, 0, "transparency",	1, 0, 0,	&TransparencyFlag, NULL ),
#endif
#ifdef MOD_BACKGROUNDIMAGE
	INIP_KW( INIT_SECTION, 0, "shrinkbitmap",	1, 0, 0,	NULL, SetShrinkBitmapEnable ),
#endif
	INIP_KW( "ConfigBox", 1, "noexit",		1, IGN, IGN,	&ConfigBoxNoExitFlag, NULL ),
	INIP_KW( "ConfigBox", 1, "filter",		IGN, 0, IGN,	&SessionFilterFlag, NULL ),
	INIP_KW( "ConfigBox", 1, "defaultsettings",	IGN, 0, IGN,	&DefaultSettingsFlag, NULL ),
	INIP_KW( "ConfigBox", 1, "loadlastsession",	1, 0, IGN,	&LoadLastSessionFlag, NULL ),
	INIP_NUM( "ConfigBox", 1, "height",		IGN,		&ConfigBoxHeight, NULL ),
	INIP_NUM( "ConfigBox", 1, "windowheight",	IGN,		&ConfigBoxWindowHeight, NULL ),
	INIP_NUM( "Print", 1, "height",			IGN,		&PrintCharSize, NULL ),
	INIP_NUM( "Print", 1, "maxline",		IGN,		&PrintMaxLinePerPage, NULL ),
	INIP_NUM( "Print", 1, "maxchar",		IGN,		&PrintMaxCharPerLine, NULL ),
	INIP_KW( "FontFallback", 1, "active",		1, 0, IGN,	NULL, SetFontFallbackFlag ),
} ;
#undef IGN

static void load_ini_params( void ) {
	char buffer[4096] ;
	size_t i ;
	for( i = 0 ; i < lenof(ini_params) ; i++ ) {
		const IniParam *p = &ini_params[i] ;
		int v ;
		int found = p->use_readini
			? readINI( KittyIniFile, p->section, p->key, buffer, sizeof(buffer) )
			: ReadParameterN( p->section, p->key, buffer, sizeof(buffer) ) ;
		if( !found ) continue ;
		if( p->is_int ) {
			v = atoi( buffer ) ;
			if( (p->intmin != INIP_IGNORE) && (v < p->intmin) ) v = p->intmin ;
		} else {
			if( !stricmp( buffer, "YES" ) ) v = p->yes ;
			else if( !stricmp( buffer, "NO" ) ) v = p->no ;
			else v = p->other ;
			if( v == INIP_IGNORE ) continue ;
		}
		if( p->var != NULL ) *(p->var) = v ;
		else p->setter( v ) ;
	}
}

void LoadParameters( void ) {
	char buffer[4096] ;

	/* All the plain keyword/int keys, in table order ("debug" first). */
	load_ini_params() ;

	/* The remaining keys have richer semantics and stay hand-written. */
	if( ReadParameterN( INIT_SECTION, "antiidle", buffer, sizeof(buffer) ) ) { buffer[127]='\0'; strcpy( AntiIdleStr, buffer ) ; }
	if( ReadParameterN( INIT_SECTION, "antiidledelay", buffer, sizeof(buffer) ) ) 
		{ AntiIdleCountMax = (int)floor(atoi(buffer)/10.0) ; if( AntiIdleCountMax<=0 ) AntiIdleCountMax =1 ; }
	if( ReadParameterN( INIT_SECTION, "browsedirectory", buffer, sizeof(buffer) ) ) { 
		if( !stricmp( buffer, "NO" ) ) { DirectoryBrowseFlag = 0 ; }
		else if( (!stricmp( buffer, "YES" )) && (IniFileFlag==SAVEMODE_DIR) ) DirectoryBrowseFlag = 1 ;
	}
	if( ReadParameterN( INIT_SECTION, "commanddelay", buffer, sizeof(buffer) ) ) {
		autocommand_delay = (int)(1000*atof( buffer )) ;
		if(autocommand_delay<5) autocommand_delay = 5 ; 
	}
	if( ReadParameterN( INIT_SECTION, "configdir", buffer, sizeof(buffer) ) ) {
		if( strlen( buffer ) > 0 ) {
			if( existdirectory(buffer) ) { SetConfigDirectory( buffer ) ; }
			else {
				/*
				 * KiTTY: ASK, do not just announce (cyd01/KiTTY#549).
				 *
				 * This used to fall through in silence, and KiTTY starting with
				 * the default store and none of your sessions looks like lost
				 * data rather than like a setting pointing at nothing. But a
				 * notice with one OK button is barely better: it is dismissed on
				 * reflex, and then KiTTY runs with a configuration nobody chose.
				 *
				 * So the choice is the user's. Carrying on is genuinely useful -
				 * a terminal you can work in beats no terminal when the
				 * directory is on a drive that is not plugged in - but so is
				 * stopping, fixing the path and starting again, and only the
				 * person at the keyboard knows which.
				 *
				 * ⚠️ It says "as if configdir had not been set" rather than
				 * naming the fallback store, because at this point in startup we
				 * do not know it: GetSaveMode() has not run yet, so whether
				 * settings come from the registry or from a directory is still
				 * undecided. Calling it "the usual configuration" was worse than
				 * vague - if configdir was set, the missing directory WAS the
				 * usual one. The path is on its own
				 * line, and the consequence is the red warning line, because the
				 * interesting cases are a typo, a disconnected drive, and (until
				 * this release) a value that arrived with a stray leading space
				 * or quotes around it.
				 */
				extern int kitty_confirm_box( HWND owner, const char *caption,
				                              const char *text, const char *warn_red ) ;
				char msg[4096+512] ;
				/*
				 * Say what we actually KNOW, and no more. All this code has
				 * established is that the path does not exist right now; it
				 * cannot know whether the directory was deleted, renamed, never
				 * created, or is simply on a drive that is not plugged in - and
				 * "nothing has been deleted" would be a claim about the world
				 * rather than about KiTTY.
				 *
				 * Two cheap tests do narrow it down, though, and the answer
				 * changes what the user should do next: whether the DRIVE is
				 * there at all, and whether the PARENT folder is. A missing
				 * drive is a disconnected disk; a present parent with a missing
				 * leaf is a typo or a rename.
				 */
				char diag[512], parent[4096] ;
				strcpy( parent, buffer ) ;
				{
					char *slash = strrchr( parent, '\\' ) ;
					char *fwd   = strrchr( parent, '/' ) ;
					if( fwd > slash ) slash = fwd ;
					if( slash && slash != parent ) *slash = '\0' ; else parent[0] = '\0' ;
				}
				if( buffer[0] && buffer[1] == ':' ) {
					char root[8] ; snprintf( root, sizeof(root), "%c:\\", buffer[0] ) ;
					if( GetDriveType( root ) <= DRIVE_NO_ROOT_DIR )
						snprintf( diag, sizeof(diag),
							"Drive %c: is not available, so this looks like a "
							"disconnected disk rather than a missing folder.", buffer[0] ) ;
					else if( parent[0] && existdirectory( parent ) )
						snprintf( diag, sizeof(diag),
							"The folder above it does exist, so only the last part "
							"of the path is missing - a typo or a rename." ) ;
					else
						snprintf( diag, sizeof(diag),
							"Neither it nor the folder above it exists." ) ;
				} else {
					snprintf( diag, sizeof(diag),
						"KiTTY has not created or removed anything here - it only "
						"looked." ) ;
				}
				snprintf( msg, sizeof(msg),
					"kitty.ini points configdir at a directory that is not there:\n\n"
					"    %s\n\n"
					"%s\n\n"
					"Start anyway, as if configdir had not been set?\n\n"
					"Yes  -  start now; whatever is kept in that directory is not listed.\n"
					"No   -  quit, so you can fix the path in kitty.ini first.",
					buffer, diag ) ;
				if( !kitty_confirm_box( NULL, "KiTTY: configdir not found", msg,
					"KiTTY has not written to or removed that directory - this "
					"check runs before anything is opened." ) ) {
					exit( 0 ) ;
				}
			}
		}
	}
	if( ReadParameterN( INIT_SECTION, "iconfile", buffer, sizeof(buffer) ) ) {
		if( existfile( buffer ) ) {
			if( IconFile != NULL ) free( IconFile ) ;
			IconFile = (char*) malloc( strlen(buffer)+1 ) ;
			strcpy( IconFile, buffer ) ;
		}
	}
	if( ReadParameterN( INIT_SECTION, "initdelay", buffer, sizeof(buffer) ) ) { 
		init_delay = (int)(1000*atof( buffer )) ;
		if( init_delay < 0 ) init_delay = 2000 ; 
	}
	if( ReadParameterN( INIT_SECTION, "fileextension", buffer, sizeof(buffer) ) ) {
		if( strlen(buffer) > 0 ) {
			snprintf( FileExtension, sizeof(FileExtension), "%s%s", (buffer[0]!='.')?".":"", buffer ) ;
			str_rtrim( FileExtension, " " ) ;
		}				
	}
	if( ReadParameterN( INIT_SECTION, "pastesize", buffer, sizeof(buffer) ) ) {
		/* KiTTY: accept 0 as "no warning". The old test was atoi(buffer)>0, so a
		 * 0 could not turn the check off - harmless while the default WAS 0, and
		 * wrong the moment it became 5120. Still requires a number, so a stray
		 * value cannot silently disable the warning. */
		const char * pv = buffer ;
		while( (*pv==' ') || (*pv=='\t') ) pv++ ;
		if( (*pv >= '0') && (*pv <= '9') ) SetPasteSize( atoi(pv) ) ;
	}
	/* NOTE: this parser is CASE-SENSITIVE (strcmp, not stricmp), so the key is
	 * documented as exactly "proxychainmax". */
	if( ReadParameterN( INIT_SECTION, "proxychainmax", buffer, sizeof(buffer) ) ) { if( atoi(buffer)>0 ) SetProxyChainMax( atoi(buffer) ) ; }
	/* [KiTTY] funkeys=<mode>: the function-key mode for sessions that do not
	 * carry one. Spelled as the Keyboard panel spells the modes; "xterm216" is
	 * the one worth setting, because it is the only mode in which Shift+F1..F12
	 * mean F13..F24 the way terminfo and every modern host expect. */
	if( ReadParameterN( INIT_SECTION, "funkeys", buffer, sizeof(buffer) ) ) {
		str_rtrim( buffer, "\n\r \t" ) ;
		if( !stricmp(buffer,"xterm216") || !stricmp(buffer,"xterm 216+") ) SetFunkeysDefault( FUNKY_XTERM_216 ) ;
		else if( !stricmp(buffer,"tilde") || !stricmp(buffer,"esc[n~") )   SetFunkeysDefault( FUNKY_TILDE ) ;
		else if( !stricmp(buffer,"linux") )                                SetFunkeysDefault( FUNKY_LINUX ) ;
		else if( !stricmp(buffer,"xtermr6") || !stricmp(buffer,"xterm r6") ) SetFunkeysDefault( FUNKY_XTERM ) ;
		else if( !stricmp(buffer,"vt400") )                                SetFunkeysDefault( FUNKY_VT400 ) ;
		else if( !stricmp(buffer,"vt100p") || !stricmp(buffer,"vt100+") )  SetFunkeysDefault( FUNKY_VT100P ) ;
		else if( !stricmp(buffer,"sco") )                                  SetFunkeysDefault( FUNKY_SCO ) ;
	}
	/* Same case-sensitivity note: exactly "namedproxy", value "hostname" or
	 * "sessionorhostname" (the default). */
	if( ReadParameterN( INIT_SECTION, "namedproxy", buffer, sizeof(buffer) ) ) {
		str_rtrim( buffer, "\n\r \t" ) ;
		if( !stricmp( buffer, "hostname" ) ) SetNamedProxyHostnameOnly( 1 ) ;
		else if( !stricmp( buffer, "sessionorhostname" ) ) SetNamedProxyHostnameOnly( 0 ) ;
	}
	if( ReadParameterN( INIT_SECTION, "PSCPPath", buffer, sizeof(buffer) ) ) {
		if( existfile( buffer ) ) { 
			if( PSCPPath!=NULL) { free(PSCPPath) ; PSCPPath = NULL ; }
			PSCPPath = (char*) malloc( strlen(buffer) + 1 ) ; strcpy( PSCPPath, buffer ) ;
		}
	}
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
	if( ReadParameterN( INIT_SECTION, "sshversion", buffer, sizeof(buffer) ) ) { set_sshver( buffer ) ; }
	if( ReadParameterN( INIT_SECTION, "WinSCPPath", buffer, sizeof(buffer) ) ) {
		if( existfile( buffer ) ) { 
			if( WinSCPPath!=NULL) { free(WinSCPPath) ; WinSCPPath = NULL ; }
			WinSCPPath = (char*) malloc( strlen(buffer) + 1 ) ; strcpy( WinSCPPath, buffer ) ;
		}
	}
	if( readINI( KittyIniFile, "ConfigBox", "dblclick", buffer, sizeof(buffer) ) ) {
		if( !strcmp(buffer,"open") ) { SetDblClickFlag(0) ; }
		if( !strcmp(buffer,"start") ) { SetDblClickFlag(1) ; }
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
	if( readINI( KittyIniFile, "Folder", "del", buffer, sizeof(buffer) ) ) {
		StringList_Del( FolderList, buffer ) ;
		delINI( KittyIniFile, "Folder", "del" ) ;
	}
	/* [FontFallback] string settings (kitty/winfont_fallback.c). The
	 * "active" master switch is handled by the ini_params table above;
	 * the free-form keys are read here and handed over in one shot.
	 * NB the mini ini parser matches section/key names case-SENSITIVELY. */
	{
	char fbList[1024]="", fbOvr[2048]="", fbLog[64]="", fbLogFile[MAX_PATH]="" ;
	readINI( KittyIniFile, "FontFallback", "fallback", fbList, sizeof(fbList) ) ;
	readINI( KittyIniFile, "FontFallback", "override", fbOvr, sizeof(fbOvr) ) ;
	readINI( KittyIniFile, "FontFallback", "log", fbLog, sizeof(fbLog) ) ;
	readINI( KittyIniFile, "FontFallback", "logfile", fbLogFile, sizeof(fbLogFile) ) ;
	winfb_config_set( fbList, fbOvr, fbLog, fbLogFile ) ;
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
	char buffer[4096] = "" ;   /* the KITTY_INI_FILE test below reads this even
	                            * when the variable is unset - it used to be
	                            * uninitialised stack, so the first existfile()
	                            * ran on whatever happened to be there */
	if( KittyIniFile != NULL ) { free( KittyIniFile ) ; }
	KittyIniFile=NULL ;

	/* snprintf, not strcpy: the value comes from the environment and is not
	 * length-bounded. */
	if( getenv("KITTY_INI_FILE") != NULL ) { snprintf( buffer, sizeof(buffer), "%s", getenv("KITTY_INI_FILE") ) ; }
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

	/* KiTTY: install the client-side serving-agent check (security). */
	{ extern void kitty_install_agent_check(void); kitty_install_agent_check(); }
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
		if( IniFileFlag == SAVEMODE_REG ) { RepairSharrowDefaults() ; MigrateScpAutoPwd() ; }
		/* One-time migration of legacy 9bis named proxies into our hive, with its
		 * own marker so it fires even when sessions were migrated in an earlier
		 * build. Registry-mode only (REG||FILE); DPAPI-protects passwords on copy
		 * and in place (hknet/KiTTY#11, TASK_named_proxies.md Piece 5). */
		kitty_migrate_old_proxies() ;
	}
	/* Not gated on the save mode: the obsolete kitty.ini copy exists in
	 * portable installs too. */
	RetireConfigPasswordLeftovers() ;
	/* Same reasoning, same place: the retired CountUp bookkeeping can be in a
	 * kitty.ini as well as in the hive, and two of those values held the user's
	 * username and machine name. */
	RetireCountUpLeftovers() ;

	// Chargement de la base de registre si besoin
	if( IniFileFlag == SAVEMODE_REG ) { // Mode de sauvegarde registry
		// Si la cle n'existe pas ...
		if( !RegTestKey( HKEY_CURRENT_USER, TEXT(PUTTY_REG_POS) ) ) { 
			HWND hdlg = InfoBox( hinst, NULL ) ;
			// ... on charge le backup le plus recent (kittynew-<timestamp>.sav),
			// ou l'ancien fichier a nom fixe s'il existe encore.
			char newestsav[4096] = "" ;
			int havesav = sav_find_for_restore( KittySavFile, newestsav, sizeof(newestsav) ) ;
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

	/* Both of these ask the storage layer which backend is active, so they must
	 * run AFTER kitty_set_storage_mode() above - not next to the other one-time
	 * startup repairs further up, where a portable run still looks like a
	 * registry one and the registry branch fires by mistake.
	 *
	 * Portable stores used to read their master-password salt out of the
	 * registry on every unlock. That is gone, so a store that relied on it gets
	 * the state copied in once here - before anything tries to unlock. */
	if( kitty_migrate_portable_mpw_state() ) { kitty_show_mpw_moved( NULL ) ; }
	/* Same idea for the master password the OLD export behaviour created as a
	 * side effect: drop it when nothing in the store is wrapped with it
	 * (design/TASK_export_password.md SS7b). Portable stores only - see the
	 * function's comment for why the registry hive is left alone. */
	kitty_retire_orphan_master_password() ;

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
