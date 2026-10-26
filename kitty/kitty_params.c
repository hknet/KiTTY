/*
 * kitty_params.c - the application-wide settings of kitty.c: the flags
 * kitty.ini and the registry switch (each a variable with its Get/Set
 * accessor), the delays and paths, the parameter reader and writer that
 * pick the ini file or the hive in use, and LoadParameters with the
 * ini_params table it applies at startup. Moved out of kitty.c as it was;
 * what the two still share is declared in kitty_int.h.
 */

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
#include "kitty_common.h"
#include "kitty_image.h"
#include "kitty_crypt.h"
#include "kitty_registry.h"
#include "kitty_tools.h"
#include "kitty_win.h"
#include "kitty_updater.h"
#include "kitty_dlgbox.h"     /* kitty_info_modeless */
#include "kitty_winerr.h"     /* kitty_win_reason */
#include "kitty_winutil.h"
#include "kitty_dlgbox.h"
#include "kitty_launcher.h"
#include "winfont_fallback.h"
#include "kitty_msgbox.h"   /* themed MessageBox routing */
#include "kitty_oldwin_reg.h"   /* XP: RegDeleteTree/RegGetValue via oldwin */
#include "kitty_text.h"   /* shared captions and wordings */
#include "kitty_inikeys.h"   /* KI_*: the kitty.ini key names */
#include "kitty_storage.h"   /* kitty_set_session_save_failed_report */
#include "kitty_inifile.h"   /* kitty_inifile_write_blocked: readonly=yes */
#include "kitty_notes.h"   /* the application notification, marked owed at startup */
#include "kitty_termenv.h"   /* the sendtermenv switch, read by the backends */
#include "kitty_pwmem.h"   /* passwords wrapped in memory */
#include "kitty_storage.h"
#include "kitty_secretstore.h"
#include "kitty_gui.h"
#include "kitty_bridge.h"
#include "kitty_exportbundle.h"
#include "mini/mini.h"
#include "kitty_params.h"
#include "kitty_hostkey_acceptnew.h"   /* autostoresshkey */



// Stuff for drag-n-drop transfers
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

/* How a named proxy's Host is read when the proxy itself does not set it
 * (kitty.ini [KiTTY] namedproxy):
 *   sessionorhostname  the title of a saved session first, then a hostname -
 *                      what PuTTY has always done, and the DEFAULT, because
 *                      existing configurations may rely on it;
 *   hostname           a hostname, full stop.
 * A proxy that DOES set it overrides this, per definition. The silent
 * substitution - a jump host that happens to share a name with a saved session
 * quietly dragging that session's whole config in - is the reason a proxy can
 * now set it. */
static int NamedProxyHostnameOnly = 0 ;
int kitty_named_proxy_default_hostname( void ) { return NamedProxyHostnameOnly ; }
void SetNamedProxyHostnameOnly( const int flag ) { NamedProxyHostnameOnly = flag ? 1 : 0 ; }

/* [KiTTY] funkeys: the function-key mode a session that has none of its own
 * starts with. -1 means "not set", which is the default and leaves PuTTY's
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
 * TransparencyAllowed remembers what kitty.ini set and is never flipped, so
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
int ProtectFlag = 0 ; 
int GetProtectFlag(void) { return ProtectFlag ; }

// Flag defining the visibility of a window
int VisibleFlag = VISIBLE_YES ;
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
 * ever made SetNewIcon cycle or randomise (every caller passes SI_INIT),
 * and the ini key could only switch the flag on, so the feature did nothing.
 * The 50 embedded icons remain and are still selectable per session, by
 * number or as an external file (CONF_icone / CONF_iconefile). */
#ifndef MOD_PERSO
#define IDI_MAINICON_0 1
#define IDC_RESULT 1008
#endif

// The library to look the icons up in (the file set in kitty.ini, else
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

// [KiTTY] psnotify: a notice when a program in a background window becomes
// blocked, fails or is done (OSC 7501, kitty_hostnotify.c).
int GetNoticeSeconds( void ) {
	char buffer[32] ;
	int v = 15 ;
	if( ReadParameterN( INIT_SECTION, KI_NOTICESECONDS, buffer, sizeof(buffer) ) ) v = atoi( buffer ) ;
	if( v < 1 ) v = 15 ;
	if( v > 600 ) v = 600 ;
	return v ;
}

// [KiTTY] psoverlay: the program status overlay while two or more jobs report
// (kitty_psoverlay.c).
int PsOverlayFlag = 1 ;
int GetPsOverlayFlag(void) { return PsOverlayFlag ; }
void SetPsOverlayFlag( const int flag ) { PsOverlayFlag = flag ; }

// Window height used by the WinHeight function
int WinHeight = -1 ;
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

// Send the window to the tray automatically (for tunnels); set by the
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

/* [KiTTY] sendtermenv: a new connection also sends COLORTERM=truecolor and
 * TERM_PROGRAM=KiTTY++ unless the session sets them (kitty_termenv.h). Read
 * by the SSH and Telnet backends through kitty_termenv_switch_hook. */
static int SendTermEnvFlag = 1 ;
int GetSendTermEnvFlag(void) { return SendTermEnvFlag ; }
void SetSendTermEnvFlag( const int flag ) { SendTermEnvFlag = flag ; }

#ifdef MOD_RECONNECT
// Flag to disable the automatic reconnection mechanism
static int AutoreconnectFlag = 1 ;
int GetAutoreconnectFlag( void ) { return AutoreconnectFlag ; }
void SetAutoreconnectFlag( const int flag ) { AutoreconnectFlag = flag ; }
// Delay before attempting an automatic reconnection
static int ReconnectDelay = 5 ;
int GetReconnectDelay(void) { return ReconnectDelay ; }
void SetReconnectDelay( const int flag ) { ReconnectDelay = flag ; }
// The cap of the reconnect backoff: the wait doubles per attempt up to this
static int ReconnectMaxDelay = 300 ;
int GetReconnectMaxDelay(void) { return ReconnectMaxDelay ; }
void SetReconnectMaxDelay( const int flag ) { ReconnectMaxDelay = flag ; }
// The backoff factor: each wait is the last one times this (1 = fixed, 1..10)
static int ReconnectBackoff = 2 ;
int GetReconnectBackoff(void) { return ReconnectBackoff ; }
void SetReconnectBackoff( const int flag ) { ReconnectBackoff = flag ; }
#endif

// Where the session list shows "Default Settings":
// [ConfigBox] defaultsettings=no (0, never) | yes (1, always, the default) |
// root (2, only at the top level of the list) - Application > Config Window >
// Session Panel, "Show "Default Settings":"
static int DefaultSettingsFlag = 1 ;
int GetDefaultSettingsFlag(void) { return DefaultSettingsFlag ; }
void SetDefaultSettingsFlag( const int flag ) { DefaultSettingsFlag = flag ; }

/* [ConfigBox] supdup=yes / rlogin=yes: show the Connection/SUPDUP and
 * Connection/Rlogin leaves in the configuration window's tree. Off hides the
 * leaf, except while the session in the box uses that protocol - the tree
 * filter in windows/dialog.c decides. */
static int ConfigBoxSupdupFlag = 1 ;
int GetConfigBoxSupdupFlag(void) { return ConfigBoxSupdupFlag ; }
void SetConfigBoxSupdupFlag( const int flag ) { ConfigBoxSupdupFlag = flag ; }
static int ConfigBoxRloginFlag = 1 ;
int GetConfigBoxRloginFlag(void) { return ConfigBoxRloginFlag ; }
void SetConfigBoxRloginFlag( const int flag ) { ConfigBoxRloginFlag = flag ; }

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
 * made the ini value mean three times what it stated. */
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
	/* the panel hands over the text as stored; the runtime copy is the
	 * expanded path, as at start-up (%VAR%, cyd01/KiTTY#472) */
	if( path && path[0] ) PSCPPath = kitty_expand_env_dup( path ) ;
}
void SetTransparencyEnabled( const int flag ) { SetTransparencyIni( flag ) ; }

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

/* A setting kitty.ini did not take is reported once per run, with Windows'
 * reason and the file. The save swaps a full copy in (mini.c storeINI,
 * ReplaceFile), which another program holding the file open or mapped
 * refuses; the panel then just kept the old value at the next start, and
 * nobody knew why. Where the report goes follows kitty_reach(): a box on the
 * desktop, a modeless one beside a live session, the prompt for a scripted
 * run, nothing where nobody can see it. */
static void ini_write_failed_once( DWORD err ) {
	static int told = 0 ;
	char *msg ;
	if( told ) return ;
	told = 1 ;
	msg = dupprintf( KT_INI_WRITE_FAILED_FMT, KittyIniFile ? KittyIniFile : "kitty.ini",
	                 kitty_win_reason( err ) ) ;
	switch( kitty_reach() ) {
	  case KITTY_REACH_DESKTOP:
		kitty_info_box( GetActiveWindow(), KT_CAP_KITTYPP, msg, NULL ) ;   /* themed, as every KiTTY++ box */
		break ;
	  case KITTY_REACH_SESSION:
		kitty_info_modeless( GetActiveWindow(), KT_CAP_KITTYPP, msg, NULL, NULL ) ;
		break ;
	  case KITTY_REACH_SHELL:
		KittyCliPrint( msg ) ;
		break ;
	  default:
		break ;
	}
	sfree( msg ) ;
}

/* A session save to the folder store that failed after its retries
 * (windows/storage.c, which writes the Event Log record): reported where a
 * failed kitty.ini write is, each time - a box on the desktop, a modeless one
 * beside a live session, the prompt of a scripted run, nothing where nobody
 * can see it. */
static void session_save_failed_report( const char *msg ) {
	switch( kitty_reach() ) {
	  case KITTY_REACH_DESKTOP:
		kitty_info_box( GetActiveWindow(), KT_CAP_KITTYPP, msg, NULL ) ;   /* themed, as every KiTTY++ box */
		break ;
	  case KITTY_REACH_SESSION:
		kitty_info_modeless( GetActiveWindow(), KT_CAP_KITTYPP, msg, NULL, NULL ) ;
		break ;
	  case KITTY_REACH_SHELL:
		KittyCliPrint( msg ) ;
		break ;
	  default:
		break ;
	}
}
static void __attribute__((constructor)) kitty_params_autoreg( void ) {
	kitty_set_session_save_failed_report( session_save_failed_report ) ;
}

// Write a parameter either to the registry or to the configuration file.
// report: show a failed kitty.ini write (once per run); 0 = the caller
// reports it with the setting's name, Windows' reason still in GetLastError().
// readonly=yes: neither is written, KITTY_RO_REFUSED (non-zero, so a caller
// testing for a failed write stays silent).
static int write_parameter( const char * key, const char * name, char * value, int report ) {
	int ret = 1 ;
	char buffer[4096] ;
	if( GetReadOnlyFlag() ) return KITTY_RO_REFUSED ;
	if( IniFileFlag == SAVEMODE_DIR ) {
		ret = writeINI( KittyIniFile, key, name, value ) ;
		if( !ret && report ) {
			DWORD err = GetLastError() ;
			ini_write_failed_once( err ) ;
			SetLastError( err ) ;
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
		/* its result: a failed write returned 1 like a stored one */
		ret = RegTestOrCreate( HKEY_CURRENT_USER, buffer, name, value ) ;
	}
	return ret ;
}
int WriteParameter( const char * key, const char * name, char * value ) {
	return write_parameter( key, name, value, 1 ) ;
}
/* The Application tab (kset_write) reports a failed write itself, giving the
 * setting: one box, not this file's generic one first. */
int WriteParameterNoReport( const char * key, const char * name, char * value ) {
	return write_parameter( key, name, value, 0 ) ;
}

/* A kitty.ini value whole (sfree it); NULL = the key is not there. */
char * kitty_read_ini_dup( const char * file, const char * section, const char * key ) {
	static const char absent[] = "\x01kitty.ini:absent\x01" ;
	char * v ;
	if( file == NULL || !file[0] || section == NULL || !section[0] || key == NULL ) return NULL ;
	v = kitty_profile_string_dup( section, key, absent, file ) ;
	if( v && !strcmp( v, absent ) ) { sfree( v ) ; v = NULL ; }
	return v ;
}

// Read a parameter either from the configuration file or from the registry,
// whole (sfree it; "" when it is not set). A registry value was read into
// 1024 bytes and a longer one counted as absent, so the kitty.ini value - or
// nothing - took its place: a long Folders list saved to the registry never
// came back.
char * ReadParameterDup( const char * key, const char * name ) {
	char * v = NULL ;
	if( IniFileFlag == SAVEMODE_DIR ) {
		/* Portable directory mode must be registry-independent: global KiTTY
		 * parameters such as Folders are read from kitty.ini, not from a stale
		 * HKCU value left by an installed/registry-mode copy. */
		v = kitty_read_ini_dup( KittyIniFile, key, name ) ;
	} else if( ( v = GetValueDataDup( HKEY_CURRENT_USER, kitty_registry_base(), name ) ) == NULL ) {
		v = kitty_read_ini_dup( KittyIniFile, key, name ) ;
		}
	return v ? v : dupstr( "" ) ;
	}

// Into a caller's buffer of `size` bytes (final NUL included): the value
// read whole by ReadParameterDup, cut to that size - for short values
// (flags, numbers, keywords); a path or a list is read with ReadParameterDup.
int ReadParameterN( const char * key, const char * name, char * value, size_t size ) {
	char * v = ReadParameterDup( key, name ) ;
	int set = v[0] != '\0' ;
	if( size == 0 ) { sfree( v ) ; return 0 ; }
	snprintf( value, size, "%s", v ) ;
	sfree( v ) ;
	return set ;
	}
	
// Delete a parameter (readonly=yes: nothing, KITTY_RO_REFUSED)
int DelParameter( const char * key, const char * name ) {
	char buffer[4096] ;
	if( GetReadOnlyFlag() ) return KITTY_RO_REFUSED ;
	delINI( KittyIniFile, key, name ) ;
	/* The same key WriteParameter uses: a delete aimed at a different hive
	 * removes nothing and still reports success. */
	strcpy( buffer, kitty_registry_base() ) ;
	RegDelValue( HKEY_CURRENT_USER, buffer, (char*)name ) ;
	return 1 ;
	}
	
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
	/* hknet/KiTTY#61: a setting a session leaves out comes from Default Settings */
	INIP_KW( INIT_SECTION, 0, KI_INHERITDEFAULTSETTINGS,	1, 0, IGN,	NULL, kitty_set_inherit_default_settings ),
	/* with it: a session save writes only what differs from Default Settings */
	INIP_KW( INIT_SECTION, 0, KI_SAVEONLYNONDEFAULTSETTINGS,	1, 0, IGN,	NULL, kitty_set_save_only_nondefault ),
	/* restrictacl=yes: -restrict-acl for every process; no way back off.
	 * use_readini=1 (kitty.ini ONLY) is deliberate and unlike its [KiTTY]
	 * neighbours - see the comment on SetRestrictAclFlag. */
	INIP_KW( INIT_SECTION, 1, KI_RESTRICTACL,	1, IGN, IGN,	NULL, SetRestrictAclFlag ),
	/* autostoresshkey=yes: accept-new for every session - a NEW host key is
	 * stored without a question, a changed one never. kitty.ini ONLY, as
	 * restrictacl: nothing writes it to the registry, and a value found there
	 * would switch the question off unseen. */
	INIP_KW( INIT_SECTION, 1, KI_AUTOSTORESSHKEY,	1, 0, IGN,	NULL, kitty_hostkey_accept_new_set_all ),
	INIP_KW( INIT_SECTION, 0, KI_SHORTCUTS,		1, 0, IGN,	&ShortcutsFlag, NULL ),
	INIP_KW( INIT_SECTION, 0, KI_SIZE,		1, 0, IGN,	&SizeFlag, NULL ),      /* symmetrical: a checkbox */
	INIP_NUM( INIT_SECTION, 0, KI_SLIDEDELAY,	IGN,		&ImageSlideDelay, NULL ),
	INIP_KW( INIT_SECTION, 0, KI_USERPASSSSHNOSAVE,	1, 0, IGN,	NULL, SetUserPassSSHNoSave ),
	INIP_KW( INIT_SECTION, 0, KI_WINROLL,		1, 0, IGN,	&WinrolFlag, NULL ),
	/* wintitle: symmetrical since the settings tree offers it as a checkbox */
	INIP_KW( INIT_SECTION, 0, KI_WINTITLE,		1, 0, IGN,	&TitleBarFlag, NULL ),
	INIP_KW( INIT_SECTION, 0, KI_PSOVERLAY,		1, 0, IGN,	&PsOverlayFlag, NULL ),
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
	INIP_NUM( INIT_SECTION, 0, KI_RECONNECTMAXDELAY,	1,		&ReconnectMaxDelay, NULL ),
	INIP_NUM( INIT_SECTION, 0, KI_RECONNECTBACKOFF,	1,		&ReconnectBackoff, NULL ),
#endif
	INIP_KW( INIT_SECTION, 0, KI_SCRIPTMODE,		1, 0, IGN,	NULL, kitty_script_set_enabled ),
	INIP_KW( INIT_SECTION, 0, KI_SENDCMDMODE,		1, 0, IGN,	NULL, kitty_broadcast_set_enabled ),
	INIP_KW( INIT_SECTION, 0, KI_SENDTERMENV,		1, 0, IGN,	NULL, SetSendTermEnvFlag ),   /* symmetrical: a checkbox */
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
	/* Same shape: the two Tree Navigation checkboxes on Config Window. */
	INIP_KW( KI_SECTION_CONFIGBOX, 0, KI_CONFIGBOX_SUPDUP,		1, 0, IGN,	&ConfigBoxSupdupFlag, NULL ),
	INIP_KW( KI_SECTION_CONFIGBOX, 0, KI_CONFIGBOX_RLOGIN,		1, 0, IGN,	&ConfigBoxRloginFlag, NULL ),
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
	/* [ConfigBox] defaultsettings=root: the third value the yes/no table row
	 * leaves alone (its "other" is ignored). Through ReadParameterN like the
	 * row itself - the panel writes it with WriteParameter, which in registry
	 * mode stores in the hive. */
	if( ReadParameterN( KI_SECTION_CONFIGBOX, KI_CONFIGBOX_DEFAULTSETTINGS, buffer, sizeof(buffer) )
	    && !stricmp( buffer, "root" ) )
		DefaultSettingsFlag = 2 ;
	/* The backends query this terminal for the sendtermenv switch, and SSH
	 * whether a refused default is still news for that host. */
	kitty_termenv_switch_hook = GetSendTermEnvFlag ;
	kitty_termenv_hint_hook = kitty_termenv_hint_first ;

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
	/* browsedirectory: classic KiTTY's switch to believe folders as
	 * subdirectories of the session folder. Retired: the folder store reads a
	 * session's folder from its path whatever it holds. Still read, so a
	 * kitty.ini that sets it holds a known key; nothing is done with it. */
	(void)ReadParameterN( INIT_SECTION, KI_BROWSEDIRECTORY, buffer, sizeof(buffer) ) ;
	if( ReadParameterN( INIT_SECTION, KI_COMMANDDELAY, buffer, sizeof(buffer) ) ) {
		autocommand_delay = (int)(1000*atof( buffer )) ;
		if(autocommand_delay<5) autocommand_delay = 5 ; 
	}
	{
		/* the folder whole (a relative one joined to the exe's): a value
		 * past 4 KB was cut to another folder */
		char * cfgdir = ReadParameterDup( INIT_SECTION, KI_CONFIGDIR ) ;
		if( cfgdir[0] ) {
			/* A relative configdir is below the exe's folder, as sessions= is
			 * and as kageant and the console tools take it (kitty_inifile.c).
			 * It was tested against the working directory, so a shortcut with
			 * another "Start in" found another folder, or none. */
			if( cfgdir[0] != '\\' && cfgdir[0] != '/' && cfgdir[1] != ':' ) {
				char * abs = dupprintf( "%s\\%s", InitialDirectory, cfgdir ) ;
				sfree( cfgdir ) ;
				cfgdir = abs ;
			}
			if( existdirectory(cfgdir) ) { SetConfigDirectory( cfgdir ) ; }
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
				 * WARNING: It reads "as if configdir had not been set" rather than
				 * stating the fallback store, because at this point in startup we
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
				char * msg ;
				/*
				 * Report what we actually KNOW, and no more. All this code has
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
				char diag[512], * parent = dupstr( cfgdir ) ;
				{
					char *slash = strrchr( parent, '\\' ) ;
					char *fwd   = strrchr( parent, '/' ) ;
					if( fwd > slash ) slash = fwd ;
					if( slash && slash != parent ) *slash = '\0' ; else parent[0] = '\0' ;
				}
				if( cfgdir[0] && cfgdir[1] == ':' ) {
					char root[8] ; snprintf( root, sizeof(root), "%c:\\", cfgdir[0] ) ;
					if( GetDriveType( root ) <= DRIVE_NO_ROOT_DIR )
						snprintf( diag, sizeof(diag),
							KT_MAIN_CFGDIR_DIAG_DRIVE, cfgdir[0] ) ;
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
				sfree( parent ) ;
				msg = dupprintf( KT_MAIN_CFGDIR_MISSING, cfgdir, diag ) ;
				if( !kitty_confirm_box( NULL, KT_CAP_CFGDIR_NOT_FOUND, msg,
					KT_MAIN_CFGDIR_WARN ) ) {
					exit( 0 ) ;
				}
				sfree( msg ) ;
			}
		}
		sfree( cfgdir ) ;
	}
	{
		char * icon = ReadParameterDup( INIT_SECTION, KI_ICONFILE ) ;
		if( icon[0] && existfile( icon ) ) {
			if( IconFile != NULL ) free( IconFile ) ;
			IconFile = (char*) malloc( strlen(icon)+1 ) ;
			strcpy( IconFile, icon ) ;
		}
		sfree( icon ) ;
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
	{
		/* the three paths below are read whole */
		char * pscp = ReadParameterDup( INIT_SECTION, KI_PSCPPATH ) ;
		if( pscp[0] ) {
			/* KiTTY: %VAR% expansion (cyd01/KiTTY#472) - expand the stored text
			 * before checking/adopting it; kitty.ini keeps the literal form. */
			char *expanded = kitty_expand_env_dup( pscp ) ;
			if( existfile( expanded ) ) {
				if( PSCPPath!=NULL) { free(PSCPPath) ; PSCPPath = NULL ; }
				PSCPPath = expanded ;
			} else {
				free( expanded ) ;
			}
		}
		sfree( pscp ) ;
	}
	{
		char * sav = ReadParameterDup( INIT_SECTION, KI_SAV ) ;
		if( sav[0] ) {
			/* Ignore an inherited legacy default (kitty.sav / kitty084.sav) written
			 * by an older KiTTY, so the current default (kittynew.sav) takes over
			 * without the user having to edit kitty.ini. A genuinely custom path is
			 * still honoured. The legacy-name check is against the STORED text,
			 * before %VAR% expansion (cyd01/KiTTY#472): a legacy filename never
			 * contains a variable, and checking the raw text keeps this test
			 * exactly as it always was. */
			const char *bn = strrchr( sav, '\\' ) ; bn = bn ? bn+1 : sav ;
			if( stricmp( bn, "kitty.sav" ) && stricmp( bn, "kitty084.sav" ) ) {
				char *expanded = kitty_expand_env_dup( sav ) ;
				if( KittySavFile!=NULL ) free( KittySavFile ) ;
				KittySavFile = expanded ;
			}
		}
		sfree( sav ) ;
	}
	if( ReadParameterN( INIT_SECTION, KI_SSHVERSION, buffer, sizeof(buffer) ) ) { set_sshver( buffer ) ; }
	{
		char * winscp = ReadParameterDup( INIT_SECTION, KI_WINSCPPATH ) ;
		if( winscp[0] ) {
			/* KiTTY: %VAR% expansion (cyd01/KiTTY#472) - see the KI_PSCPPATH
			 * load above. */
			char *expanded = kitty_expand_env_dup( winscp ) ;
			if( existfile( expanded ) ) {
				if( WinSCPPath!=NULL) { free(WinSCPPath) ; WinSCPPath = NULL ; }
				WinSCPPath = expanded ;
			} else {
				free( expanded ) ;
			}
		}
		sfree( winscp ) ;
	}
	/* ReadParameter, not readINI: the Session-panel group on Application >
	 * Config Window edits this, and a panel writes through WriteParameter -
	 * which in registry mode does not write the file. */
	if( ReadParameterN( KI_SECTION_CONFIGBOX, KI_CONFIGBOX_DBLCLICK, buffer, sizeof(buffer) ) ) {
		if( !stricmp(buffer,"open") ) { SetDblClickFlag(0) ; }
		if( !stricmp(buffer,"start") ) { SetDblClickFlag(1) ; }
	}
	/* How many levels of the config-box Category tree to auto-expand. Default
	 * (unset / all / full) = fully expanded; a number 1..N expands only that
	 * deep (1 = top categories only, like stock PuTTY). */
	/* ReadParameter, not readINI: Application > Config Window edits it, and a
	 * panel writes through WriteParameter - the registry in registry mode. */
	if( ReadParameterN( KI_SECTION_CONFIGBOX, KI_CONFIGBOX_CATEGORYEXPAND, buffer, sizeof(buffer) ) ) {
		if( strlen(buffer)==0 || !stricmp(buffer,"all") || !stricmp(buffer,"full") || !stricmp(buffer,"max") || !stricmp(buffer,"yes") )
			kitty_category_expand_depth = 99 ;
		else { int d = atoi(buffer) ; kitty_category_expand_depth = (d >= 1) ? d : 99 ; }
	}
	{
		/* the folder whole: a cut name removed another folder */
		char * del = kitty_read_ini_dup( KittyIniFile, KI_SECTION_FOLDER, KI_FOLDER_DEL ) ;
		if( del ) {
			StringList_Del( FolderList, del ) ;
			/* readonly=yes: the key stays, and is applied again next start */
			if( !kitty_inifile_write_blocked( KittyIniFile ) )
				delINI( KittyIniFile, KI_SECTION_FOLDER, KI_FOLDER_DEL ) ;
			sfree( del ) ;
		}
	}
	/* [FontFallback] string settings (kitty/winfont_fallback.c). The
	 * "active" master switch is handled by the ini_params table above;
	 * the free-form keys are read here and handed over in one shot.
	 * NB the mini ini parser matches section/key names case-SENSITIVELY. */
	{
	char fbLog[64]="", *fbList, *fbOvr, *fbLogFile ;
	/* the lists and the log file in full: cut, a list lost its last
	 * entries and the log file named another file */
	fbList = kitty_read_ini_dup( KittyIniFile, KI_SECTION_FONTFALLBACK, KI_FONTFALLBACK_FALLBACK ) ;
	fbOvr = kitty_read_ini_dup( KittyIniFile, KI_SECTION_FONTFALLBACK, KI_FONTFALLBACK_OVERRIDE ) ;
	readINI( KittyIniFile, KI_SECTION_FONTFALLBACK, KI_FONTFALLBACK_LOG, fbLog, sizeof(fbLog) ) ;
	fbLogFile = kitty_read_ini_dup( KittyIniFile, KI_SECTION_FONTFALLBACK, KI_FONTFALLBACK_LOGFILE ) ;
	winfb_config_set( fbList ? fbList : "", fbOvr ? fbOvr : "", fbLog, fbLogFile ? fbLogFile : "" ) ;
	sfree( fbList ) ; sfree( fbOvr ) ; sfree( fbLogFile ) ;
	}
}
