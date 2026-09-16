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

#ifndef SAVEMODE_REG
#define SAVEMODE_REG 0
#endif
#ifndef SAVEMODE_FILE
#define SAVEMODE_FILE 1
#endif
#ifndef SAVEMODE_DIR
#define SAVEMODE_DIR 2
#endif

// Flag for "portable" mode (settings kept in files), defined in
// kitty_commun.c
extern int IniFileFlag ;

// Flag enabling the folder tree when savemode=dir, defined in
// kitty_commun.c
int GetDirectoryBrowseFlag(void) { return DirectoryBrowseFlag ; }


#define SI_INIT 0
#define SI_NEXT 1
#define SI_RANDOM 2

// Stuff for drag-n-drop transfers
#define TIMER_DND 8777
int dnd_delay = 250;
HDROP hDropInf = NULL;

// Delay before sending the password and before sending the window to the
// tray automatically on connection (in milliseconds)
int init_delay = 2000 ;
// Delay between each line of the automatic command (in milliseconds)
int autocommand_delay = 5 ;
// Delay between each character of a command (in milliseconds)
int between_char_delay = 0 ;
// Delay between two lines of one command and between two \x \k shortcuts
int internal_delay = 10 ;

// Pointer to the automatic command
char * AutoCommand = NULL ;

// Content of a script to send to the screen
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
/* What a NEW session's Keyboard panel starts with. Xterm 216+ is the mode
 * every modern host expects (Shift+F1..F12 = F13..F24); PuTTY's ESC[n~ is
 * still one choice away, and a saved session always keeps its own mode. */
static int FunkeysDefault = FUNKY_XTERM_216 ;
int GetFunkeysDefault( void ) { return FunkeysDefault ; }
void SetFunkeysDefault( const int t ) { FunkeysDefault = t ; }

// Flag controlling the hyperlink feature. In 0.84 hyperlinks are provided by
// kitty_url.c/window.c, not the historical terminal.c hyperlink patch, so keep
// the feature available by default and let kitty.ini "hyperlink" disable it.
int HyperlinkFlag = 1 ;
int GetHyperlinkFlag(void) { return HyperlinkFlag ; }
void SetHyperlinkFlag( const int flag ) { HyperlinkFlag = flag ; }

// Flag controlling transparency.
// The feature stays available (so transparency is configurable PER SESSION via the
// Window > Transparency panel), but it is OFF by default because the per-session
// TransparencyValue defaults to 0 = fully opaque / non-layered (see conf.h):
// kitty_apply_transparency() applies nothing until a session sets a value > 0.
// kitty.ini [KiTTY] transparency=no remains the master switch that removes the
// whole feature (config panel + system-menu adjust items).
/* TransparencyFlag is the LIVE state, which the /transparency command flips.
 * TransparencyAllowed remembers what kitty.ini said and is never flipped, so
 * transparency=no cannot be worked around from the console. */
int TransparencyFlag = 1 ;
static int TransparencyAllowed = 1 ;
int GetTransparencyFlag(void) { return TransparencyFlag ; }
int GetTransparencyAllowed(void) { return TransparencyAllowed ; }
static void SetTransparencyIni( const int flag ) {
	TransparencyFlag = flag ; TransparencyAllowed = flag ;
}

// Script file handling at startup
char * ScriptFileContent = NULL ;

// Flag protecting the window against accidental keyboard input
static int ProtectFlag = 0 ; 
int GetProtectFlag(void) { return ProtectFlag ; }

// Flags defining the save mode
#ifndef SAVEMODE_REG
#define SAVEMODE_REG 0
#endif
#ifndef SAVEMODE_FILE
#define SAVEMODE_FILE 1
#endif
#ifndef SAVEMODE_DIR
#define SAVEMODE_DIR 2
#endif

// Section name of the configuration file
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

// Flag defining the visibility of a window
static int VisibleFlag = VISIBLE_YES ;
int GetVisibleFlag(void) { return VisibleFlag ; }
void SetVisibleFlag( const int flag ) { VisibleFlag = flag ; }

// Flag to disable the keyboard shortcuts
int ShortcutsFlag = 1 ;
int GetShortcutsFlag(void) { return ShortcutsFlag ; }
void SetShortcutsFlag( const int flag ) { ShortcutsFlag = flag ; }

// Flag to disable the mouse shortcuts
int MouseShortcutsFlag = 1 ;
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

// The library to look the icons up in (the file named in kitty.ini, else
// kitty.dll if it exists, else kitty.exe)
HINSTANCE hInstIcons =  NULL ;

// File holding the icons to load
char * IconFile = NULL ;

// [KiTTY] size=yes: append the live terminal size [cols x rows] to the window
// title (not while maximized). Applied by the title decorator in
// windows/window.c; needs wintitle=yes (TitleBarFlag) like classic KiTTY.
int SizeFlag = 0 ;
int GetSizeFlag(void) { return SizeFlag ; }
void SetSizeFlag( const int flag ) { SizeFlag = flag ; }

// [KiTTY] wintitle=yes (default): enable KiTTY's title decorations - the size
// suffix (size=yes) and the (PROTECTED)/(ONTOP) status markers. wintitle=no =
// plain stock titles. SECURITY: unlike classic KiTTY the title text is never
// PARSED (the __xy title-scan dispatcher stays dead) - decoration is strictly
// one-way output in windows/window.c wintw_set_title.
int TitleBarFlag = 1 ;
int GetTitleBarFlag(void) { return TitleBarFlag ; }
void SetTitleBarFlag( const int flag ) { TitleBarFlag = flag ; }

// Window height used by the WinHeight function
static int WinHeight = -1 ;
int GetWinHeight(void) { return WinHeight ; }
// Flag to disable the Winrol (window rollup)
int WinrolFlag = 1 ;
int GetWinrolFlag(void) { return WinrolFlag ; }
void SetWinrolFlag( const int num ) { WinrolFlag  = num ; }

// Password protecting the configuration (registry)
/* "the configuration store changed" flag - implemented in kitty_storage.c so
 * that windows/storage.c can set it too. Declared up here because SaveFolderList
 * below is the first user. */
/* Startup cleanup of the master password the old export behaviour created as a
 * side effect - implemented in kitty_storage.c, which owns the store scan. */

char PasswordConf[cstMaxRegLength+2] = "" ; /* filled from the registry "password" value via GetValueData, which writes up to cstMaxRegLength data bytes + NUL */

// Send the window to the tray automatically (for tunnels); goes with the
// -send-to-tray option
static int AutoSendToTray = 0 ;
int GetAutoSendToTray( void ) { return AutoSendToTray ; }
void SetAutoSendToTray( const int flag ) { AutoSendToTray = flag ; }

// Flag to avoid creating the kitty.ini and kitty.sav files
int NoKittyFileFlag = 0 ;
int GetNoKittyFileFlag(void) { return NoKittyFileFlag ; }

// Height of the configuration box (visible saved-session rows; 16 = stock fit)
static int ConfigBoxHeight = 16 ;
int GetConfigBoxHeight(void) { return ConfigBoxHeight ; }
void SetConfigBoxHeight( const int num ) { ConfigBoxHeight = num ; }

// Height of the configuration box window (0 = default value)
static int ConfigBoxWindowHeight = 0 ;
int GetConfigBoxWindowHeight(void) { return ConfigBoxWindowHeight ; }
void SetConfigBoxWindowHeight( const int num ) { ConfigBoxWindowHeight = num ; }

// Width of the configuration box window (0 = the template's width).
// Written by dragging the box's own edge as well as by the field on
// Application > Config Window: the drag and the field are one setting, so the
// field cannot come to disagree with the window it describes.
static int ConfigBoxWindowWidth = 0 ;
int GetConfigBoxWindowWidth(void) { return ConfigBoxWindowWidth ; }
void SetConfigBoxWindowWidth( const int num ) { ConfigBoxWindowWidth = num ; }

// [ConfigBox] noexit=yes: when a window that ran a connected session closes,
// spawn a fresh instance (= the config box) so you land back in the session
// picker. Gated on is_backend_first_connected at exit (windows/window.c), so a
// config-box-only process never respawns - classic KiTTY's version fired on
// config-box exit too, which is the bug that kept it broken there.
static int ConfigBoxNoExitFlag = 0 ;
int GetConfigBoxNoExitFlag(void) { return ConfigBoxNoExitFlag ; }
void SetConfigBoxNoExitFlag( const int flag ) { ConfigBoxNoExitFlag = flag ; }

/* [ConfigBox] fixedsizewindow=yes: the configuration window keeps the size it
 * has - no resize frame, and the two size fields on
 * Application > Config Window refuse edits - so a size chosen for a kiosk or
 * a shared machine stays chosen. */
static int ConfigBoxFixedSizeFlag = 0 ;
int GetConfigBoxFixedSizeFlag(void) { return ConfigBoxFixedSizeFlag ; }
void SetConfigBoxFixedSizeFlag( const int flag ) { ConfigBoxFixedSizeFlag = flag ; }

/* [ConfigBox] applicationsettings=no: the configuration box has no
 * Application tab - none of its panels is built, and every jump to one lands
 * on the Session tab. kitty.ini only, no control: it exists so an
 * administrator can keep users out of the application-wide settings, which
 * is exactly what a control on one of those panels could not do. */
static int ConfigBoxApplicationSettingsFlag = 1 ;
int GetConfigBoxApplicationSettingsFlag(void) { return ConfigBoxApplicationSettingsFlag ; }

// Flag to disable CTRL+TAB handling
static int CtrlTabFlag = 1 ;
int GetCtrlTabFlag(void) { return CtrlTabFlag  ; }
void SetCtrlTabFlag( const int flag ) { CtrlTabFlag  = flag ; }

#ifdef MOD_RECONNECT
// Flag to disable the automatic reconnection mechanism
static int AutoreconnectFlag = 1 ;
int GetAutoreconnectFlag( void ) { return AutoreconnectFlag ; }
void SetAutoreconnectFlag( const int flag ) { AutoreconnectFlag = flag ; }
// Delay before attempting an automatic reconnection
static int ReconnectDelay = 5 ;
int GetReconnectDelay(void) { return ReconnectDelay ; }
void SetReconnectDelay( const int flag ) { ReconnectDelay = flag ; }
#endif

// Flag to disable the automatic creation of the Default Settings session
// [ConfigBox] defaultsettings=yes
static int DefaultSettingsFlag = 1 ;
int GetDefaultSettingsFlag(void) { return DefaultSettingsFlag ; }
void SetDefaultSettingsFlag( const int flag ) { DefaultSettingsFlag = flag ; }

/* KiTTY (hknet/KiTTY#26) [ConfigBox] foldernavigation=yes: browse session
 * folders as ROWS of the saved-session list - folders first, then the sessions
 * at that level, with ".." to go back up - instead of picking a folder from a
 * combo box. Opt-in, and OFF by default: it changes what the root list shows
 * (only unfiled sessions, rather than every session annotated with its folder),
 * which would be a surprise on upgrade. Storage is untouched either way; a
 * folder remains an attribute of a session, and this is only a view over it. */
static int FolderNavigationFlag = 0 ;
int GetFolderNavigationFlag(void) { return FolderNavigationFlag ; }
void SetFolderNavigationFlag( const int flag ) { FolderNavigationFlag = flag ; }

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

// Flag to disable the filter on the configuration box session list
static int SessionFilterFlag = 1 ;
int GetSessionFilterFlag(void) { return SessionFilterFlag ; }
void SetSessionFilterFlag( const int flag ) { SessionFilterFlag = flag ; }

// Flag to switch to image-viewer mode
static int ImageViewerFlag = 0 ;
int GetImageViewerFlag(void) { return ImageViewerFlag  ; }
void SetImageViewerFlag( const int flag ) { ImageViewerFlag = flag ; }

// Time (in seconds) between background image switches (<=0 = no slideshow)
int ImageSlideDelay = - 1 ;

// Counter for sending the anti-idle string
/* KiTTY: seconds between keepalives. Was a count of 30-second ticks, which
 * made the ini value mean three times what it said. */
int AntiIdleSeconds = 180 ;
char AntiIdleStr[128] = "" ;  // e.g. " \x08": type a space and erase it at once


// Path to the WinSCP program
char * WinSCPPath = NULL ;

/* path to the file-copy helper: kscp.exe, or PuTTY's pscp.exe */
char * PSCPPath = NULL ;

// Startup directory
char InitialDirectory[4096]="" ;

// Full paths of the kitty.ini and kitty.sav configuration files
char * KittyIniFile = NULL ;
char * GetKittyIniFile(void) { return KittyIniFile ; }
char * KittySavFile = NULL ;
char * GetKittySavFile(void) { return KittySavFile ; }

/* Running values the settings tree (Application > KiTTY Settings) reads and
 * writes beside the store, so a change shows again when the panel is
 * revisited and takes effect where the program reads the value live. */
char * GetIconFile(void) { return IconFile ; }
void SetIconFile( const char * path ) {
	if( IconFile != NULL ) { free( IconFile ) ; IconFile = NULL ; }
	if( path && path[0] ) { IconFile = (char*) malloc( strlen(path)+1 ) ; strcpy( IconFile, path ) ; }
}
void SetPSCPPath( const char * path ) {
	if( PSCPPath != NULL ) { free( PSCPPath ) ; PSCPPath = NULL ; }
	if( path && path[0] ) { PSCPPath = (char*) malloc( strlen(path)+1 ) ; strcpy( PSCPPath, path ) ; }
}
void SetTransparencyEnabled( const int flag ) { SetTransparencyIni( flag ) ; }

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
void CreateDefaultIniFile( void ) {
	if( !NoKittyFileFlag ) if( !GetReadOnlyFlag() ) {
		if( KittyIniFile==NULL ) return ;
		if( strlen(KittyIniFile)==0 ) return ;
		if( !existfile( KittyIniFile ) ) {
			CreateIniFile( KittyIniFile ) ;
		}
		if( !existfile( KittyIniFile ) ) { MessageBox( NULL, KT_MAIN_INI_CREATE_FAILED, KT_CAP_ERROR, MB_OK|MB_ICONERROR ) ; }
	}
}

// Write a parameter either to the registry or to the configuration file
int WriteParameter( const char * key, const char * name, char * value ) {
	int ret = 1 ;
	char buffer[4096] ;
	if( IniFileFlag == SAVEMODE_DIR ) { 
		if( !GetReadOnlyFlag() ) {
			ret = writeINI( KittyIniFile, key, name, value ) ; 
		}
	} else { 
		/* The hive in use, matching ReadParameterN.
		 *
		 * This used to write to PUTTY_REG_PARENT\\<key> - "Software\\kapper.net"
		 * plus the INI SECTION name - while the read side looked at the base
		 * hive and ignored the section entirely. The two agreed only by
		 * coincidence, when that section happened to be "KiTTY". With
		 * KiClassName=PuTTY the section becomes "PuTTY" and the sessions move
		 * to SimonTatham\\PuTTY, so a global setting was WRITTEN to
		 * kapper.net\\PuTTY and READ from somewhere else entirely: saving one
		 * appeared to work, and it came back with the old value for ever.
		 * Read and write now name the same key - the one this process uses. */
		strcpy( buffer, kitty_registry_base() ) ;
		RegTestOrCreate( HKEY_CURRENT_USER, buffer, name, value ) ; 
	}
	return ret ;
}

// Read a parameter either from the configuration file or from the registry.
// Bounded variant: never writes more than `size` bytes (final NUL included)
// into `value`; a value that is too long is truncated instead of overflowing.
int ReadParameterN( const char * key, const char * name, char * value, size_t size ) {
	char buffer[4096] ;
	strcpy( buffer, "" ) ;
	if( IniFileFlag == SAVEMODE_DIR ) {
		/* Portable directory mode must be registry-independent: global KiTTY
		 * parameters such as Folders are read from kitty.ini, not from a stale
		 * HKCU value left by an installed/registry-mode copy. */
		if( !readINI( KittyIniFile, key, name, buffer, sizeof(buffer) ) ) strcpy( buffer, "" ) ;
	} else if( GetValueData( HKEY_CURRENT_USER, kitty_registry_base(), name, buffer ) == NULL ) {
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

// Compatibility: the old unbounded signature - the destination buffer MUST be
// at least 4096 bytes. Prefer ReadParameterN( ..., sizeof(buf) ).
int ReadParameter( const char * key, const char * name, char * value ) {
	return ReadParameterN( key, name, value, 4096 ) ;
	}
	
// Delete a parameter
int DelParameter( const char * key, const char * name ) {
	char buffer[4096] ;
	if( !GetReadOnlyFlag() ) { delINI( KittyIniFile, key, name ) ; }
	/* The same key WriteParameter uses: a delete aimed at a different hive
	 * removes nothing and still reports success. */
	strcpy( buffer, kitty_registry_base() ) ;
	RegDelValue( HKEY_CURRENT_USER, buffer, (char*)name ) ;
	return 1 ;
	}
	
// Check the configuration (file or registry mode) and load kitty.sav if needed
void GetSaveMode( void ) {
	char buffer[256] ;
	if( readINI( KittyIniFile, INIT_SECTION, KI_SAVEMODE, buffer, sizeof(buffer) ) ) {
		str_rtrim( buffer, "\n\r \t" ) ;
		if( !stricmp( buffer, "registry" ) ) IniFileFlag = SAVEMODE_REG ;
		else if( !stricmp( buffer, "file" ) ) IniFileFlag = SAVEMODE_FILE ;
		/* savemode=dir no longer implies browsedirectory: the sessions it
		 * writes are flat files carrying Folder=, so the subdirectory lookup
		 * this used to switch on could never find anything. browsedirectory=yes
		 * still selects it explicitly. */
		else if( !stricmp( buffer, "dir" ) ) { IniFileFlag = SAVEMODE_DIR ; }
	}
	if( IniFileFlag!=SAVEMODE_DIR ) DirectoryBrowseFlag = 0 ;
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

// Initialise the parameters from the kitty.ini file
#ifdef MOD_BACKGROUNDIMAGE
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
 * no such value (see ReadParameterN above; the registry silently beats
 * kitty.ini). For a security switch that ordering
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
	/* "debug" stays first (historically "read this one first"). */
	INIP_KW( INIT_SECTION, 0, KI_DEBUG,		1, IGN, IGN,	&debug_flag, NULL ),
#ifdef MOD_BACKGROUNDIMAGE
	INIP_KW( INIT_SECTION, 0, KI_BGIMAGE,		1, 0, IGN,	NULL, SetBackgroundImageFlag ),
#endif
	INIP_NUM( INIT_SECTION, 0, KI_BCDELAY,		IGN,		&between_char_delay, NULL ),
	/* conf=no: do NOT auto-create kitty.ini/kitty.sav */
	INIP_KW( INIT_SECTION, 0, KI_CONF,		IGN, 1, IGN,	&NoKittyFileFlag, NULL ),
	INIP_NUM( INIT_SECTION, 0, KI_CRYPTSALT,		IGN,		NULL, SetCryptSaltFlag ),
	INIP_KW( INIT_SECTION, 0, KI_CTRLTAB,		1, 0, IGN,	NULL, SetCtrlTabFlag ),   /* symmetrical: a checkbox */
	INIP_KW( INIT_SECTION, 0, KI_HYPERLINK,		1, 0, IGN,	&HyperlinkFlag, NULL ),
	INIP_NUM( INIT_SECTION, 0, KI_INTERNALDELAY,	1,		&internal_delay, NULL ),
	INIP_KW( INIT_SECTION, 0, KI_MOUSESHORTCUTS,	1, 0, IGN,	&MouseShortcutsFlag, NULL ),
	/* cyd01/KiTTY #548: force classic modal error boxes instead of inline terminal errors */
	INIP_KW( INIT_SECTION, 0, KI_MODALERRORS,	1, 0, IGN,	NULL, SetModalErrorsFlag ),
	/* Inline-first security prompts (#548 successor): default yes = classic modal
	 * box; no = OpenSSH-style in-terminal prompt (typed "yes"). */
	INIP_KW( INIT_SECTION, 0, KI_MODALNEWHOSTKEYCONFIRMATION,	1, 0, IGN,	NULL, SetModalNewHostKeyConfirmationFlag ),
	INIP_KW( INIT_SECTION, 0, KI_MODALCHANGEDHOSTKEYCONFIRMATION,	1, 0, IGN,	NULL, SetModalChangedHostKeyConfirmationFlag ),
	INIP_KW( INIT_SECTION, 0, KI_MODALWEAKKEYCONFIRMATION,	1, 0, IGN,	NULL, SetModalWeakKeyConfirmationFlag ),
	INIP_KW( INIT_SECTION, 0, KI_READONLY,		1, IGN, IGN,	NULL, SetReadOnlyFlag ),
	/* restrictacl=yes: -restrict-acl for every process; no way back off.
	 * use_readini=1 (kitty.ini ONLY) is deliberate and unlike its [KiTTY]
	 * neighbours - see the comment on SetRestrictAclFlag. */
	INIP_KW( INIT_SECTION, 1, KI_RESTRICTACL,	1, IGN, IGN,	NULL, SetRestrictAclFlag ),
	INIP_KW( INIT_SECTION, 0, KI_SHORTCUTS,		1, 0, IGN,	&ShortcutsFlag, NULL ),
	INIP_KW( INIT_SECTION, 0, KI_SIZE,		1, 0, IGN,	&SizeFlag, NULL ),      /* symmetrical: a checkbox */
	INIP_NUM( INIT_SECTION, 0, KI_SLIDEDELAY,	IGN,		&ImageSlideDelay, NULL ),
	INIP_KW( INIT_SECTION, 0, KI_USERPASSSSHNOSAVE,	1, 0, IGN,	NULL, SetUserPassSSHNoSave ),
	INIP_KW( INIT_SECTION, 0, KI_WINROLL,		1, 0, IGN,	&WinrolFlag, NULL ),
	/* wintitle: symmetrical since the settings tree offers it as a checkbox */
	INIP_KW( INIT_SECTION, 0, KI_WINTITLE,		1, 0, IGN,	&TitleBarFlag, NULL ),
#ifdef MOD_PROXY
	/* proxyselection: yes = always, no = never, auto (or anything else) = when defined */
	INIP_KW( KI_SECTION_CONFIGBOX, 0, KI_CONFIGBOX_PROXYSELECTION,	1, -1, 0,	NULL, SetProxySelectionFlag ),
#endif
#ifdef MOD_ZMODEM
	INIP_KW( INIT_SECTION, 0, KI_ZMODEM,		1, 0, IGN,	NULL, SetZModemFlag ),
#endif
#ifdef MOD_RECONNECT
	/* SYMMETRICAL since the settings tree offers it as a checkbox: yes states
	 * the default rather than meaning nothing (same as the ConfigBox keys). */
	INIP_KW( INIT_SECTION, 0, KI_AUTORECONNECT,	1, 0, IGN,	&AutoreconnectFlag, NULL ),
	INIP_NUM( INIT_SECTION, 0, KI_RECONNECTDELAY,	1,		&ReconnectDelay, NULL ),
#endif
	INIP_KW( INIT_SECTION, 0, KI_SCRIPTMODE,		1, 0, IGN,	NULL, kitty_script_set_enabled ),
	INIP_KW( INIT_SECTION, 0, KI_SENDCMDMODE,		1, 0, IGN,	NULL, kitty_broadcast_set_enabled ),
#ifndef MOD_NOTRANSPARENCY
	/* transparency: anything but an explicit yes disables */
	INIP_KW( INIT_SECTION, 0, KI_TRANSPARENCY,	1, 0, 0,	NULL, SetTransparencyIni ),
#endif
#ifdef MOD_BACKGROUNDIMAGE
	INIP_KW( INIT_SECTION, 0, KI_SHRINKBITMAP,	1, 0, 0,	NULL, SetShrinkBitmapEnable ),
#endif
	/* Symmetrical and registry-aware for the same reason as the four below:
	 * Application > Config Window offers it as a checkbox now. */
	INIP_KW( KI_SECTION_CONFIGBOX, 0, KI_CONFIGBOX_NOEXIT,		1, 0, IGN,	&ConfigBoxNoExitFlag, NULL ),
	INIP_KW( KI_SECTION_CONFIGBOX, 0, KI_CONFIGBOX_FIXEDSIZEWINDOW,	1, 0, IGN,	&ConfigBoxFixedSizeFlag, NULL ),
	INIP_KW( KI_SECTION_CONFIGBOX, 0, KI_CONFIGBOX_APPLICATIONSETTINGS,	1, 0, IGN,	&ConfigBoxApplicationSettingsFlag, NULL ),
	/*
	 * The Session-panel group on Application > Config Window edits these
	 * four, which forces two things on them.
	 *
	 * SYMMETRICAL. "filter" and "defaultsettings" used to be one-way (yes =
	 * IGN, i.e. leave the current value alone), because only "no" was ever
	 * useful from a hand-written file. A checkbox that can be cleared and
	 * not set again is a lie, so "yes" now states the default rather than
	 * meaning nothing. No existing file changes behaviour: the value "yes"
	 * writes the same 1 those flags already start at.
	 *
	 * READ THE WAY THEY ARE WRITTEN (0 = ReadParameterN). A panel writes
	 * through WriteParameter, which in registry mode stores under the hive
	 * in use; reading them with readINI would put the value somewhere it is
	 * never looked for. See the note on the three size keys below.
	 */
	INIP_KW( KI_SECTION_CONFIGBOX, 0, KI_CONFIGBOX_FILTER,		1, 0, IGN,	&SessionFilterFlag, NULL ),
	INIP_KW( KI_SECTION_CONFIGBOX, 0, KI_CONFIGBOX_DEFAULTSETTINGS,	1, 0, IGN,	&DefaultSettingsFlag, NULL ),
	INIP_KW( KI_SECTION_CONFIGBOX, 0, KI_CONFIGBOX_FOLDERNAVIGATION,	1, 0, IGN,	&FolderNavigationFlag, NULL ),
	INIP_KW( KI_SECTION_CONFIGBOX, 0, KI_CONFIGBOX_LOADLASTSESSION,	1, 0, IGN,	&LoadLastSessionFlag, NULL ),
	/*
	 * READ THE WAY THEY ARE WRITTEN (0 = ReadParameterN, registry then ini).
	 *
	 * These three are the only kitty.ini keys the configuration box writes
	 * back by itself, through WriteParameter - which in registry mode stores
	 * them under the hive in use, not in kitty.ini. Reading them with
	 * readINI meant an installed copy wrote the number to the registry and
	 * then looked for it in a file that never had it, so the field took the
	 * value, the panel redisplayed it from the running program, and the next
	 * start came up at the default again with nothing to show for it.
	 * Portable mode is unaffected: with savemode=dir, ReadParameterN reads
	 * kitty.ini and nothing else.
	 */
	INIP_NUM( KI_SECTION_CONFIGBOX, 0, KI_CONFIGBOX_HEIGHT,		IGN,		&ConfigBoxHeight, NULL ),
	INIP_NUM( KI_SECTION_CONFIGBOX, 0, KI_CONFIGBOX_WINDOWHEIGHT,	IGN,		&ConfigBoxWindowHeight, NULL ),
	INIP_NUM( KI_SECTION_CONFIGBOX, 0, KI_CONFIGBOX_WINDOWWIDTH,	IGN,		&ConfigBoxWindowWidth, NULL ),
	INIP_NUM( KI_SECTION_PRINT, 1, KI_PRINT_HEIGHT,			IGN,		&PrintCharSize, NULL ),
	INIP_NUM( KI_SECTION_PRINT, 1, KI_PRINT_MAXLINE,		IGN,		&PrintMaxLinePerPage, NULL ),
	INIP_NUM( KI_SECTION_PRINT, 1, KI_PRINT_MAXCHAR,		IGN,		&PrintMaxCharPerLine, NULL ),
	INIP_KW( KI_SECTION_FONTFALLBACK, 1, KI_FONTFALLBACK_ACTIVE,		1, 0, IGN,	NULL, SetFontFallbackFlag ),
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
	if( ReadParameterN( INIT_SECTION, KI_ANTIIDLE, buffer, sizeof(buffer) ) ) { buffer[127]='\0'; strcpy( AntiIdleStr, buffer ) ; }
	if( ReadParameterN( INIT_SECTION, KI_ANTIIDLEDELAY, buffer, sizeof(buffer) ) ) {
		/* Plain seconds, floored at 5 AND capped at a day. 0 or nonsense
		 * leaves the default.
		 *
		 * The cap is not tidiness: window.c turns this into milliseconds for
		 * SetTimer, and that multiplication is done in 32 bits. Without an
		 * upper bound, antiidledelay=4294968 becomes 4,294,968,000 ms, wraps,
		 * and arms a 704 ms timer - a keepalive flood reached from the
		 * opposite end of the range the floor above guards. */
		int secs = atoi( buffer ) ;
		if( secs > 86400 ) secs = 86400 ;
		if( secs > 0 ) AntiIdleSeconds = secs < 5 ? 5 : secs ;
	}
	if( ReadParameterN( INIT_SECTION, KI_BROWSEDIRECTORY, buffer, sizeof(buffer) ) ) { 
		if( !stricmp( buffer, "NO" ) ) { DirectoryBrowseFlag = 0 ; }
		else if( (!stricmp( buffer, "YES" )) && (IniFileFlag==SAVEMODE_DIR) ) DirectoryBrowseFlag = 1 ;
	}
	if( ReadParameterN( INIT_SECTION, KI_COMMANDDELAY, buffer, sizeof(buffer) ) ) {
		autocommand_delay = (int)(1000*atof( buffer )) ;
		if(autocommand_delay<5) autocommand_delay = 5 ; 
	}
	if( ReadParameterN( INIT_SECTION, KI_CONFIGDIR, buffer, sizeof(buffer) ) ) {
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
				 * WARNING: It says "as if configdir had not been set" rather than
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
							KT_MAIN_CFGDIR_DIAG_DRIVE, buffer[0] ) ;
					else if( parent[0] && existdirectory( parent ) )
						snprintf( diag, sizeof(diag),
							KT_MAIN_CFGDIR_DIAG_PARENT ) ;
					else
						snprintf( diag, sizeof(diag),
							KT_MAIN_CFGDIR_DIAG_NEITHER ) ;
				} else {
					snprintf( diag, sizeof(diag),
						KT_MAIN_CFGDIR_DIAG_LOOKED ) ;
				}
				snprintf( msg, sizeof(msg),
					KT_MAIN_CFGDIR_MISSING,
					buffer, diag ) ;
				if( !kitty_confirm_box( NULL, KT_CAP_CFGDIR_NOT_FOUND, msg,
					KT_MAIN_CFGDIR_WARN ) ) {
					exit( 0 ) ;
				}
			}
		}
	}
	if( ReadParameterN( INIT_SECTION, KI_ICONFILE, buffer, sizeof(buffer) ) ) {
		if( existfile( buffer ) ) {
			if( IconFile != NULL ) free( IconFile ) ;
			IconFile = (char*) malloc( strlen(buffer)+1 ) ;
			strcpy( IconFile, buffer ) ;
		}
	}
	if( ReadParameterN( INIT_SECTION, KI_INITDELAY, buffer, sizeof(buffer) ) ) { 
		init_delay = (int)(1000*atof( buffer )) ;
		if( init_delay < 0 ) init_delay = 2000 ; 
	}
	if( ReadParameterN( INIT_SECTION, KI_FILEEXTENSION, buffer, sizeof(buffer) ) ) {
		if( strlen(buffer) > 0 ) {
			snprintf( FileExtension, sizeof(FileExtension), "%s%s", (buffer[0]!='.')?".":"", buffer ) ;
			str_rtrim( FileExtension, " " ) ;
		}				
	}
	if( ReadParameterN( INIT_SECTION, KI_PASTESIZE, buffer, sizeof(buffer) ) ) {
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
	if( ReadParameterN( INIT_SECTION, KI_PROXYCHAINMAX, buffer, sizeof(buffer) ) ) { if( atoi(buffer)>0 ) SetProxyChainMax( atoi(buffer) ) ; }
	/* [KiTTY] funkeys=<mode>: the function-key mode for sessions that do not
	 * carry one. Spelled as the Keyboard panel spells the modes; "xterm216" is
	 * the one worth setting, because it is the only mode in which Shift+F1..F12
	 * mean F13..F24 the way terminfo and every modern host expect. */
	if( ReadParameterN( INIT_SECTION, KI_FUNKEYS, buffer, sizeof(buffer) ) ) {
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
	if( ReadParameterN( INIT_SECTION, KI_NAMEDPROXY, buffer, sizeof(buffer) ) ) {
		str_rtrim( buffer, "\n\r \t" ) ;
		if( !stricmp( buffer, "hostname" ) ) SetNamedProxyHostnameOnly( 1 ) ;
		else if( !stricmp( buffer, "sessionorhostname" ) ) SetNamedProxyHostnameOnly( 0 ) ;
	}
	if( ReadParameterN( INIT_SECTION, KI_PSCPPATH, buffer, sizeof(buffer) ) ) {
		if( existfile( buffer ) ) { 
			if( PSCPPath!=NULL) { free(PSCPPath) ; PSCPPath = NULL ; }
			PSCPPath = (char*) malloc( strlen(buffer) + 1 ) ; strcpy( PSCPPath, buffer ) ;
		}
	}
	if( ReadParameterN( INIT_SECTION, KI_SAV, buffer, sizeof(buffer) ) ) {
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
	if( ReadParameterN( INIT_SECTION, KI_SSHVERSION, buffer, sizeof(buffer) ) ) { set_sshver( buffer ) ; }
	if( ReadParameterN( INIT_SECTION, KI_WINSCPPATH, buffer, sizeof(buffer) ) ) {
		if( existfile( buffer ) ) { 
			if( WinSCPPath!=NULL) { free(WinSCPPath) ; WinSCPPath = NULL ; }
			WinSCPPath = (char*) malloc( strlen(buffer) + 1 ) ; strcpy( WinSCPPath, buffer ) ;
		}
	}
	/* ReadParameter, not readINI: the Session-panel group on Application >
	 * Config Window edits this, and a panel writes through WriteParameter -
	 * which in registry mode does not write the file. */
	if( ReadParameter( KI_SECTION_CONFIGBOX, KI_CONFIGBOX_DBLCLICK, buffer ) ) {
		if( !stricmp(buffer,"open") ) { SetDblClickFlag(0) ; }
		if( !stricmp(buffer,"start") ) { SetDblClickFlag(1) ; }
	}
	/* How many levels of the config-box Category tree to auto-expand. Default
	 * (unset / all / full) = fully expanded; a number 1..N expands only that
	 * deep (1 = top categories only, like stock PuTTY). */
	/* ReadParameter, not readINI: Application > Config Window edits it, and a
	 * panel writes through WriteParameter - the registry in registry mode. */
	if( ReadParameter( KI_SECTION_CONFIGBOX, KI_CONFIGBOX_CATEGORYEXPAND, buffer ) ) {
		if( strlen(buffer)==0 || !stricmp(buffer,"all") || !stricmp(buffer,"full") || !stricmp(buffer,"max") || !stricmp(buffer,"yes") )
			kitty_category_expand_depth = 99 ;
		else { int d = atoi(buffer) ; kitty_category_expand_depth = (d >= 1) ? d : 99 ; }
	}
	if( readINI( KittyIniFile, KI_SECTION_FOLDER, KI_FOLDER_DEL, buffer, sizeof(buffer) ) ) {
		StringList_Del( FolderList, buffer ) ;
		delINI( KittyIniFile, KI_SECTION_FOLDER, KI_FOLDER_DEL ) ;
	}
	/* [FontFallback] string settings (kitty/winfont_fallback.c). The
	 * "active" master switch is handled by the ini_params table above;
	 * the free-form keys are read here and handed over in one shot.
	 * NB the mini ini parser matches section/key names case-SENSITIVELY. */
	{
	char fbList[1024]="", fbOvr[2048]="", fbLog[64]="", fbLogFile[MAX_PATH]="" ;
	readINI( KittyIniFile, KI_SECTION_FONTFALLBACK, KI_FONTFALLBACK_FALLBACK, fbList, sizeof(fbList) ) ;
	readINI( KittyIniFile, KI_SECTION_FONTFALLBACK, KI_FONTFALLBACK_OVERRIDE, fbOvr, sizeof(fbOvr) ) ;
	readINI( KittyIniFile, KI_SECTION_FONTFALLBACK, KI_FONTFALLBACK_LOG, fbLog, sizeof(fbLog) ) ;
	readINI( KittyIniFile, KI_SECTION_FONTFALLBACK, KI_FONTFALLBACK_LOGFILE, fbLogFile, sizeof(fbLogFile) ) ;
	winfb_config_set( fbList, fbOvr, fbLog, fbLogFile ) ;
	}
}

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
