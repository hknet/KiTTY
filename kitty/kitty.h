#ifndef KITTY_H
#define KITTY_H
#include <math.h>
#include <sys/types.h>
#include <process.h>
#include <time.h>

/*
 * Menu command ids live in ONE place: windows/kitty_rc_additions.h, which the
 * resource compiler already reads through windows/putty-rc.h and which is
 * therefore preprocessor-only. This header used to carry its own copies of 25
 * of them behind #ifndef guards, so whichever header a translation unit
 * happened to include first decided the value - and three of them disagreed
 * (IDM_XYZSTART/UPLOAD/ABORT were 0xA810/20/30 here and 0xB150/60/70 there).
 * Nothing outside window.c used those three, so the build was consistent by
 * luck; a ZModem entry added to kitty_shortcuts.c would have sent an id the
 * menu was not listening for and done nothing at all.
 */
#include "kitty_rc_additions.h"

// Handle sur la fenetre principale
//extern HWND MainHwnd ;
HWND GetMainHwnd(void) ;


/*****************************************************
** DEFINITION DES VARIABLES STATIQUE DE kitty.c
** ET DE LEUR FONCTIONS D'ACCES ET DE MODIFICATION
*****************************************************/
// [ConfigBox] noexit: respawn the config box when a session window closes
int GetConfigBoxNoExitFlag(void) ;
void SetConfigBoxNoExitFlag( const int flag ) ;

// Flag pour inhiber la gestion du CTRL+TAB
int GetCtrlTabFlag(void) ;
void SetCtrlTabFlag( const int flag ) ;

// Flag pour afficher l'image de fond
//extern int BackgroundImageFlag ;
int GetBackgroundImageFlag(void) ;
void SetBackgroundImageFlag( const int flag ) ;

#ifdef MOD_RECONNECT
// Flag pour inhiber le mécanisme de reconnexion automatique
int GetAutoreconnectFlag( void ) ;
void SetAutoreconnectFlag( const int flag ) ;
// Delai avant de tenter une reconnexion automatique
int GetReconnectDelay(void) ;
#endif

// Delai avant d'envoyer le password et d'envoyer vers le tray (automatiquement à la connexion) (en milliseconde)
extern int init_delay ;

// Delai entre chaque ligne de la commande automatique (en milliseconde)
extern int autocommand_delay ;

// Delai avant l envoi de la commande automatique sur drag-and-drop (en millisecondes)
extern int dnd_delay ;

// Delai entre chaque caracteres d'une commande (en millisecondes)
extern int between_char_delay ;

// Delai entre deux lignes d'une meme commande et entre deux raccourcis \x \k
extern int internal_delay ;

// Nom de la classe de l'application
extern char KiTTYClassName[128] ;

// [KiTTY] size: append the live [rows x cols] to the window title
int GetSizeFlag(void) ;
void SetSizeFlag( const int flag ) ;

// [KiTTY] wintitle: enable the title decorations (size suffix, PROTECTED/ONTOP markers)
int GetTitleBarFlag(void) ;
void SetTitleBarFlag( const int flag ) ;

// Reapplique les decorations de titre apres un changement d'etat (windows/window.c)
void kitty_refresh_title(void) ;

// KiTTY: expand window-title placeholders (%%h, %%s, %%u, %%p, %%P, %%f, %%l, %%d)
char *kitty_expand_wintitle(const char *title, const char *hostname, Conf *conf) ;

// Flag pour passer en mode visualiseur d'images
// extern int ImageViewerFlag ;
int GetImageViewerFlag(void) ;
void SetImageViewerFlag( const int flag ) ;

#ifdef MOD_PROXY
// Flag pour ajouter la fonction Proxy Selector
// extern int ProxySelectionFlag ;
int GetProxySelectionFlag() ;
void SetProxySelectionFlag( const int flag ) ;
#endif

// Duree (en secondes) pour switcher l'image de fond d'ecran (<=0 pas de slide)
extern int ImageSlideDelay ;

// Flag pour la protection contre les saisies malheureuses
// extern int ProtectFlag ; 
int GetProtectFlag(void) ;
void SetProtectFlag( const int flag ) ;

// Flag de definition de la visibilite d'une fenetres
// extern int VisibleFlag ;
int GetVisibleFlag(void) ;
void SetVisibleFlag( const int flag ) ;

// Gestion du script file au lancement
extern char * ScriptFileContent ;

// Flag pour inhiber les raccourcis clavier
// extern int ShortcutsFlag ;
int GetShortcutsFlag(void) ;
void SetShortcutsFlag( const int flag ) ;

// Flag pour inhiber les raccourcis souris
// extern int MouseShortcutsFlag ;
int GetMouseShortcutsFlag(void) ;
void SetMouseShortcutsFlag( const int flag ) ;

// Stuff for drag-n-drop
#ifndef TIMER_DND
#define TIMER_DND 8777
#endif
extern HDROP hDropInf;
void recupNomFichierDragDrop(HWND hwnd, HDROP* leDrop) ;

// Pointeur sur la commande autocommand
extern char * AutoCommand ;

// Contenu d'un script a envoyer à l'ecran
extern char * ScriptCommand ;

// paste size limit (number of characters). Above the limit a confirmation is requested. (0 means unlimited)
int GetPasteSize(void) ;
void SetPasteSize( const int size ) ;

// Max chained SSH proxies before refusing; kitty.ini [KiTTY] proxychainmax (default 5)
int GetProxyChainMax(void) ;
void SetProxyChainMax( const int n ) ;

// Flag de gestion de la fonction hyperlink
extern int HyperlinkFlag ;
int GetHyperlinkFlag(void) ;
void SetHyperlinkFlag( const int flag ) ;

// Broadcast gate: [KiTTY] sendcmdmode=yes|no starts windows armed or not, and
// [KiTTY] sendcmdgroup (derived when unset) decides which KiTTYs hear each
// other. Accident prevention, not a security boundary - see kitty.c.
void kitty_broadcast_set_enabled( int on ) ;
int  kitty_broadcast_default( void ) ;
const char *kitty_broadcast_group( void ) ;
int kitty_broadcast_group_from_ini( void ) ;   // key came from kitty.ini, not derived
void kitty_broadcast_set_send_key( const char *k ) ;   // -sendcmdkey override
const char *kitty_broadcast_send_key( void ) ;         // key a broadcast is SENT with
// RuTTY script engine master switch: [KiTTY] scriptmode=yes|no (kitty_rutty.c)
int kitty_script_enabled(void) ;
void kitty_script_set_enabled( int on ) ;

// Flag pour le fonctionnement en mode "portable" (gestion par fichiers), defini dans kitty_commun.c
extern int IniFileFlag ;
int GetIniFileFlag(void) ;
void SetIniFileFlag( const int flag ) ;
void SwitchIniFileFlag(void) ;

// Flag permettant la gestion de l'arborscence (dossier=folder) dans le cas d'un savemode=dir, defini dans kitty_commun.c
//extern int DirectoryBrowseFlag ;
int GetDirectoryBrowseFlag(void) ;
void SetDirectoryBrowseFlag( const int flag ) ;

// Renvoi automatiquement dans le tray (pour les tunnel), fonctionne avec le l'option -send-to-tray
//extern int AutoSendToTray ;
int GetAutoSendToTray( void ) ;
void SetAutoSendToTray( const int flag ) ;

// Flag de gestion de la Transparence
// extern int TransparencyFlag ;
int GetTransparencyFlag(void) ;
void SetTransparencyFlag( const int flag ) ;

#ifdef MOD_ZMODEM
// Flag pour inhiber les fonctions ZMODEM
// extern int ZModemFlag ;
int GetZModemFlag(void) ;
void SetZModemFlag( const int flag ) ;
#endif

// Flag pour ne pas creer les fichiers kitty.ini et kitty.sav
// extern int NoKittyFileFlag ;
int GetNoKittyFileFlag(void) ;
void SetNoKittyFileFlag( const int flag ) ;

// Hauteur de la boite de configuration
// extern int ConfigBoxHeight ;
int GetConfigBoxHeight(void) ;
void SetConfigBoxHeight( const int num ) ;

// Hauteur de la fenetre de la boite de configuration (0=valeur par defaut)
// static int ConfigBoxWindowHeight = 0 ;
int GetConfigBoxWindowHeight(void) ;
void SetConfigBoxWindowHeight( const int num ) ;

// Hauteur de la fenetre pour la fonction winrol
// extern int WinHeight ;
int GetWinHeight(void) ;
void SetWinHeight( const int num ) ;
// Flag pour inhiber le Winrol
// extern int WinrolFlag = 1 
int GetWinrolFlag(void) ;
void SetWinrolFlag( const int num ) ;

// Flag permettant de desactiver la sauvegarde automatique des informations de connexion (user/password) à la connexion SSH
// extern int UserPassSSHNoSave ; ==> Defini dans kitty_commun.c
int GetUserPassSSHNoSave(void) ;
void SetUserPassSSHNoSave( const int flag ) ;

// Flag pour inhiber le filtre sur la liste des sessions de la boite de configuration
// extern int SessionFilterFlag ;
// [ConfigBox] filter=yes
int GetSessionFilterFlag(void) ;
void SetSessionFilterFlag( const int flag ) ;

// Flag pour inhiber la création automatique de la session Default Settings
// [ConfigBox] defaultsettings=yes
int GetDefaultSettingsFlag(void) ;
void SetDefaultSettingsFlag( const int flag ) ;

// Browse folders as rows of the saved-session list instead of via the combo
// [ConfigBox] foldernavigation=no
int GetFolderNavigationFlag(void) ;
void SetFolderNavigationFlag( const int flag ) ;

// Quick connect: no = start from Default Settings with the caret in Host Name
// [ConfigBox] loadlastsession=yes
int GetLoadLastSessionFlag(void) ;
void SetLoadLastSessionFlag( const int flag ) ;

// Quick connect armed for this run (loadlastsession=no, or last session = the defaults)
int GetQuickConnectMode(void) ;
void SetQuickConnectMode( const int flag ) ;

// Double-click action on a saved session: 0 = open here, 1 = start in a new window
// [ConfigBox] dblclick=open|start
int GetDblClickFlag(void) ;
void SetDblClickFlag( const int flag ) ;

// Chemin vers le programme cthelper.exe
extern char * CtHelperPath ;

// Chemin vers le programme WinSCP
extern char * WinSCPPath ;

// Chemin vers le programme pscp.exe
extern char * PSCPPath  ;

// Repertoire de lancement
extern char InitialDirectory[4096] ;

// Extention pour les fichiers de session en mode portable (peut être ktx)
extern char FileExtension[15] ;

// Répertoire de sauvegarde de la configuration (savemode=dir)
extern char * ConfigDirectory ;

// Positionne un flag permettant de determiner si on est connecte
extern int is_backend_connected ;

#ifdef MOD_RECONNECT
/* Variable permettant de savoir qu'on a deja ete connecte */
extern int is_backend_first_connected ; 
#endif

/* Flag pour interdire l'ouverture de boite configuration */
extern int force_reconf ; 

// Compteur pour l'envoi de anti-idle
extern int AntiIdleSeconds ;   /* KiTTY: keepalive interval, in seconds */
extern char AntiIdleStr[128] ;

NOTIFYICONDATA TrayIcone ;
#ifndef MYWM_NOTIFYICON
#define MYWM_NOTIFYICON		(WM_USER+3)
#endif

// La librairie dans laquelle chercher les icones (fichier defini dans kitty.ini, sinon kitty.dll s'il existe, sinon kitty.exe)
// extern HINSTANCE hInstIcons ;
HINSTANCE GethInstIcons(void) ;
void SethInstIcons( const HINSTANCE h ) ;

extern int debug_flag ;

extern int PORT ;

// Declaration de prototypes de fonction
void InitFolderList( void ) ;
void SaveFolderList( void ) ;
void InfoBoxSetText( HWND hwnd, char * st ) ;
void InfoBoxClose( HWND hwnd ); 
void routine_server( void * st ) ;
void SetNewIcon( HWND hwnd, char * iconefile, int icone, const int mode ) ;
int WINAPI Notepad_WinMain(HINSTANCE hinstance, HINSTANCE hPrevInstance,LPSTR lpCmdLine, int nCmdShow) ;
void InitWinMain( void ) ;
char * getcwd (char * buf, int size);
int chdir(const char *path); 
void ShowInputBox( HINSTANCE hInstance, HWND hwnd ) ;   /* modeless single-line box */
char * InputBoxMultiline( HINSTANCE hInstance, HWND hwnd ) ;
char * InputBoxPassword( HINSTANCE hInstance, HWND hwnd ) ;
char * GetInputBoxResult( void ) ;
void GetAndSendLine( HWND hwnd ) ;
void GetAndSendMultiLine( HWND hwnd ) ;
void routine_inputbox( void * phwnd ) ;
void routine_inputbox_multiline( void * phwnd ) ;
void routine_inputbox_password( void * phwnd ) ;
char *itoa(int value, char *string, int radix);
void GetAndSendLinePassword( HWND hwnd ) ;
int unlink(const char *pathname);
void RunScriptFile( HWND hwnd, const char * filename ) ;
void InfoBoxSetText( HWND hwnd, char * st ) ;
void ReadInitScript( const char * filename ) ;
int ReadParameterN( const char * key, const char * name, char * value, size_t size ) ;
int ReadParameter( const char * key, const char * name, char * value ) ; /* compat: value >= 4096 octets; preferer ReadParameterN */
int WriteParameter( const char * key, const char * name, char * value ) ;
int DelParameter( const char * key, const char * name ) ;
void GetSessionFolderName( const char * session_in, char * folder ) ;
int ManageShortcuts( Terminal *term, Conf *conf, HWND hwnd, const int* clips_system, int key_num, int shift_flag, int control_flag, int alt_flag, int altgr_flag, int win_flag ) ;
void print_log( const char *fmt, ...) ;
char * SetInitialSessPath( void ) ;
char * SetSessPath( const char * dec ) ;
void CleanFolderName( char * folder ) ;
void SetInitCurrentFolder( const char * name ) ;
void set_sshver( const char * vers ) ;
int ResizeWinList( HWND hwnd, int width, int height ) ;
int SendCommandAllWindows( HWND hwnd, char * cmd ) ;
void RunCommand( HWND hwnd, const char * cmd ) ;
void timestamp_change_filename( void ) ;
int InternalCommand( HWND hwnd, char * st ) ;
void load_open_settings_forced(char *filename, Conf *conf) ;
void save_open_settings_forced(char *filename, Conf *conf) ;
int SwitchCryptFlag( void ) ;
void CreateDefaultIniFile( void ) ;
void InitSpecialMenu( HMENU m, const char * folder, const char * sessionname ) ;
void InitSpecialMenuTab( void ) ;
#define NB_MENU_MAX 1024
extern char *SpecialMenu[NB_MENU_MAX] ;   /* User-Command / launcher entry payloads (kitty_specialmenu.c) */
int ReadSpecialMenu( HMENU menu, char * KeyName, int * nbitem, int separator ) ;

/* keyboard-shortcut types, tables + entry points; all defined in
 * kitty_shortcuts.c. */
struct TShortcuts {
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
	int openlogfile ;
	int closerestart ;
	int eventlog ;
	int fullscreen ;
	int fontup ;
	int fontdown ;
	int copyall ;
	int fontnegative ;
	int fontblackandwhite ;
	int keyexchange ;
	} ;
extern struct TShortcuts shortcuts_tab ;
struct TShortcuts2 { int num ; char * st ; } ;
extern struct TShortcuts2 shortcuts_tab2[512] ;
extern int NbShortCuts ;
int DefineShortcuts( char * buf ) ;
void TranslateShortcuts( char * st ) ;
void InitShortcuts( void ) ;
int ManageShortcuts( Terminal *term, Conf *conf, HWND hwnd, const int* clips_system, int key_num, int shift_flag, int control_flag, int alt_flag, int altgr_flag, int win_flag ) ;
char * GetKittyIniFile(void) ;
char * GetKittySavFile(void) ;
// Recupere une entree d'une session ( retourne 1 si existe )
int GetSessionField( const char * session_in, const char * folder_in, const char * field, char * result ) ;
// Sauve les coordonnees de la fenetre
void SaveWindowCoord( Conf * conf ) ;
// Decompte le nombre de fenetre de la meme classe que KiTTY
int WindowsCount( HWND hwnd ) ;
HWND InfoBox( HINSTANCE hInstance, HWND hwnd ) ;
// Renomme une Cle de registre
void RegRenameTree( HWND hdlg, HKEY hMainKey, LPCTSTR lpSubKey, LPCTSTR lpDestKey ) ;
void DelRegistryKey( void ) ;
void RenewPassword( Conf *conf ) ;
// Gere l'envoi dans le System Tray
int ManageToTray( HWND hwnd ) ;
void RefreshBackground( HWND hwnd ) ;
void SendAutoCommand( HWND hwnd, const char * cmd ) ;
int NextBgImage( HWND hwnd ) ;
int PreviousBgImage( HWND hwnd ) ;
void ManageSpecialCommand( HWND hwnd, int menunum ) ;
int fileno(FILE *stream) ;
// Sauvegarde de la cle de registre
void SaveRegistryKeyEx( HKEY hMainKey, LPCTSTR lpSubKey, const char * filename ) ;
void ManageProtect( HWND hwnd, TermWin *tw, char * title ) ;
void ManagePrint( HWND hwnd ) ;
// Gere l'option always visible
void ManageVisible( HWND hwnd, TermWin *tw, char * title ) ;
// Sauvegarde de la cle de registre
void SaveRegistryKeyEx( HKEY hMainKey, LPCTSTR lpSubKey, const char * filename ) ;
void SaveRegistryKey( void ) ;
void SaveRegistryKeyNow( void ) ;
void ManageWinrol( HWND hwnd, int resize_action ) ;
void resize( int height, int width ) ;
void OpenAndSendScriptFile( HWND hwnd ) ;
void SaveCurrentSetting( HWND hwnd ) ;
void SendFile( HWND hwnd ) ;
void StartWinSCP( HWND hwnd, char * directory, char * host, char * user ) ;

/* The proxy the current connection actually went through, when a named proxy or
 * workplace proxy mode overrode the session's own settings. Recorded by
 * kitty_proxy_record_connection() from start_backend(), because that override
 * lives on a throwaway Conf copy the session never sees; kitty_proxy_connection()
 * returns NULL when the session's own proxy fields are the truth. Implemented in
 * kitty_bridge.c. */
struct kitty_proxy_snapshot {
    int type ;                 /* PROXY_* as in putty.h */
    int port ;
    char * host ;
    char * username ;
    char * password ;
    char * telnet_command ;
} ;
void kitty_proxy_record_connection( Conf * resolved ) ;
const struct kitty_proxy_snapshot * kitty_proxy_connection( void ) ;

void SendOneFile( HWND hwnd, char * directory, char * filename, char * distantdir) ;
void SendFileList( HWND hwnd, char * filelist ) ;
void GetOneFile( HWND hwnd, char * directory, const char * filename ) ;
void GetFile( HWND hwnd ) ;
void RunCmd( HWND hwnd ) ;
int SearchCtHelper( void ) ;
int SearchWinSCP( void ) ;
int SearchPSCP( void ) ;
void StartNewSession( HWND hwnd, char * directory, char * host, char * user ) ;
void urlhack_launch_url(const char* app, const char *url) ;
int GetPortFwdState( const int port, const DWORD pid ) ;
int ShowPortfwd( HWND hwnd, Conf * conf ) ;
void OnDropFiles(HWND hwnd, HDROP hDropInfo) ;
// Affiche un menu dans le systeme Tray
void DisplaySystemTrayMenu( HWND hwnd ) ;
// Recupere les coordonnees de la fenetre
void GetWindowCoord( HWND hwnd ) ;
// Gestion du script au lancement
void ManageInitScript( const char * input_str, const int len ) ;
void SetNewIcon( HWND hwnd, char * iconefile, int icone, const int mode ) ;
void GotoInitialDirectory( void ) ;
void GotoConfigDirectory( void ) ;

char * get_param_str( const char * val ) ;

// Fonctions permettant de formatter les chaînes de caractères avec %XY	
void mungestr( const char *in, char *out ) ;
void unmungestr( const char *in, char *out, int outlen ) ;
	
void NegativeColours(HWND hwnd) ;
void BlackOnWhiteColours(HWND hwnd) ;
void ChangeFontSize(Terminal *term, Conf *conf,HWND hwnd, int dec) ;
void ChangeSettings(HWND hwnd) ;
int ManageViewer( HWND hwnd, WORD wParam ) ;

void create_settings( const char * name ) ;

char * GetHelpMessage(void) ;
void CreateIniFile( const char * filename ) ;
void SendKeyboard( HWND hwnd, const char * buffer ) ;
void ManageShortcutsFlag( HWND hwnd ) ;

#ifdef MOD_LAUNCHER
void InitLauncherRegistry( void ) ;
#endif

int getpid(void) ;

// Definition de la section du fichier de configuration
#ifdef MOD_PERSO

#ifndef INIT_SECTION
#define INIT_SECTION "KiTTY"
#endif
#ifndef DEFAULT_INIT_FILE
#define DEFAULT_INIT_FILE "kitty.ini"
#endif
#ifndef DEFAULT_SAV_FILE
/* kittynew.sav (timestamped copies: kittynew-YYYYMMDD-HHMMSS.sav), NOT
 * kitty.sav, so we never overwrite the registry backup of an old (0.76)
 * KiTTY installed side by side. THIS is the definition that takes effect:
 * kitty.c includes this header before its own #ifndef block, so a second
 * definition there is dead code - which is exactly how this default was
 * silently kitty.sav until 2026-07-26. Keep it here, and only here. */
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

#ifndef SI_INIT
#define SI_INIT 0
#endif
#ifndef SI_NEXT
#define SI_NEXT 1
#endif
#ifndef SI_RANDOM
#define SI_RANDOM 2
#endif


#ifndef TIMER_INIT
#define TIMER_INIT 8701
#endif

#ifndef TIMER_AUTOCOMMAND
#define TIMER_AUTOCOMMAND 8702
#endif

#ifndef TIMER_SLIDEBG
#ifdef MOD_BACKGROUNDIMAGE
#define TIMER_SLIDEBG 8703
#endif
#endif
#ifndef TIMER_REDRAW
#define TIMER_REDRAW 8704
#endif
#ifndef TIMER_BLINKTRAYICON
#define TIMER_BLINKTRAYICON 8706
#endif
#ifndef TIMER_LOGROTATION
#define TIMER_LOGROTATION 8707
#endif
#ifndef TIMER_ANTIIDLE
#define TIMER_ANTIIDLE 8708
#endif
#ifndef TIMER_RECONNECT
#define TIMER_RECONNECT 8709
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

/*************************************************
** DEFINITION DES DEFINES
*************************************************/
#ifdef MOD_ZMODEM
int xyz_Process(Backend *back, void *backhandle, Terminal *term) ;
void xyz_ReceiveInit(Terminal *term) ;
void xyz_StartSending(Terminal *term) ;
void xyz_Cancel(Terminal *term) ;
void xyz_updateMenuItems(Terminal *term) ;
#endif

#ifndef IDM_FROMTRAY
#define IDM_FROMTRAY   0xA940
#endif
#ifndef IDM_HIDE
#define IDM_HIDE	0xA960
#endif
#ifndef IDM_UNHIDE
#define IDM_UNHIDE	0xA970
#endif
#ifndef IDM_SWITCH_HIDE
#define IDM_SWITCH_HIDE 0xA980
#endif
#ifndef IDM_GONEXT
#define IDM_GONEXT	0xA990
#endif
#ifndef IDM_GOPREVIOUS
#define IDM_GOPREVIOUS	0xB000
#endif
#ifndef IDM_SCRIPTFILE
#define IDM_SCRIPTFILE 0xB010
#endif


#ifndef IDM_PORTKNOCK
#define IDM_PORTKNOCK	0xB090
#endif

#ifdef MOD_RECONNECT
#endif


/* [Shortcuts] keyexchange: window.c resolves this to the SSH "Repeat key
 * exchange" special by its SS_REKEY code (the specials menu is built
 * dynamically in 0.84, so a fixed menu index cannot be used). */
#ifndef IDM_REKEY
#define IDM_REKEY 0xB200
#endif

// Doit etre le dernier
#ifndef IDM_LAUNCHER
#define IDM_LAUNCHER	0xB130
#endif

// USERCMD doit etre la plus grande valeur pour permettre d'avoir autant de raccourcis qu'on le souhaite
#ifndef IDM_USERCMD
#define IDM_USERCMD   0x8000
#endif

// Idem USERCMD
#ifndef IDM_GOHIDE
#define IDM_GOHIDE    0x9000
#endif

/* Launcher tray menu, workplace proxy mode: the base id switches the mode OFF,
 * base+1+i switches it on with named proxy i. A range, like GOHIDE above, and
 * clear of it (GOHIDE holds at most 100 open windows). */
#ifndef IDM_WORKPLACE
#define IDM_WORKPLACE 0x9800
#endif

/*************************************************
** FIN DE DEFINITION DES DEFINES
*************************************************/



#ifndef IDB_OK
#define IDB_OK	1098
#endif


#ifndef SAVEMODE_REG
#define SAVEMODE_REG 0
#endif
#ifndef SAVEMODE_FILE
#define SAVEMODE_FILE 1
#endif
#ifndef SAVEMODE_DIR
#define SAVEMODE_DIR 2
#endif

#ifndef NB_MENU_MAX
#define NB_MENU_MAX 1024
#endif



// Liste de define recupere de WINDOW.C necessaires a kitty.c
#ifndef IDM_ABOUT
#define IDM_ABOUT     0x0150
#endif
#ifndef IDM_RESET
#define IDM_RESET     0x0070
#endif
#ifndef IDM_COPYALL
#define IDM_COPYALL   0x0170
#endif
#ifndef IDM_SHOWLOG
#define IDM_SHOWLOG   0x0010
#endif
#ifndef IDM_RESTART
#define IDM_RESTART   0x0040
#endif
#ifndef IDM_NEWSESS
#define IDM_NEWSESS   0x0020
#endif
#ifndef IDM_DUPSESS
#define IDM_DUPSESS   0x0030
#endif
#ifndef IDM_NEWDUPSESS
#define IDM_NEWDUPSESS  0xB1B0  /* "Inherit New Session..." - must match window.c */
#endif
#ifndef IDM_RECONF
#define IDM_RECONF   0x0050
#endif
#ifndef IDM_CLRSB
#define IDM_CLRSB     0x0060
#endif
#ifndef IDM_FULLSCREEN
#define IDM_FULLSCREEN	0x0180
#endif
/* IDM_COPYALL was defined a second time here, with the same value as the copy
 * above. Harmless, but it is how the ids that DID disagree got started. */

/* Launcher global-hotkey helpers (kitty_bridge.c), shared by the launcher's
 * registration loop, the config box and session import. */
int kitty_parse_hotkey_spec( const char * spec, unsigned int * mods, unsigned int * vk ) ;
int kitty_hotkey_conflict_scan( unsigned int mods, unsigned int vk,
                                const char * exclude, char * names, int nameslen ) ;
int kitty_hotkey_enabled_count( const char * exclude ) ;
int kitty_hotkey_conflict_report( char * buf, int buflen ) ;


#endif // KITTY_H
