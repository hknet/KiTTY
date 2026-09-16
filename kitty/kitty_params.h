/*
 * kitty_params.h - the declarations for kitty_params.c: the application-wide
 * flags' Get/Set accessors, the exported delays and paths, and the
 * parameter reader and writer (ReadParameterN, WriteParameter, ...).
 * Include after kitty.h.
 */
#ifndef KITTY_PARAMS_H
#define KITTY_PARAMS_H

#include <windows.h>



/*****************************************************
** STATIC VARIABLES OF kitty.c
** AND THEIR ACCESSOR AND MODIFIER FUNCTIONS
*****************************************************/
// [ConfigBox] noexit: respawn the config box when a session window closes
int GetConfigBoxNoExitFlag(void) ;
void SetConfigBoxNoExitFlag( const int flag ) ;

// Flag to disable CTRL+TAB handling
int GetCtrlTabFlag(void) ;
void SetCtrlTabFlag( const int flag ) ;
#ifdef MOD_RECONNECT
// Flag to disable the automatic reconnection mechanism
int GetAutoreconnectFlag( void ) ;
void SetAutoreconnectFlag( const int flag ) ;
// Delay before attempting an automatic reconnection
int GetReconnectDelay(void) ;
#endif

// Delay before sending the password and before sending the window to the
// tray automatically on connection (in milliseconds)
extern int init_delay ;

// Delay between each line of the automatic command (in milliseconds)
extern int autocommand_delay ;

// Delay before sending the automatic command on drag-and-drop (in
// milliseconds)
extern int dnd_delay ;

// Delay between each character of a command (in milliseconds)
extern int between_char_delay ;

// Delay between two lines of one command and between two \x \k shortcuts
extern int internal_delay ;

// [KiTTY] size: append the live [cols x rows] to the window title
int GetSizeFlag(void) ;
void SetSizeFlag( const int flag ) ;

// [KiTTY] wintitle: enable the title decorations (size suffix, PROTECTED/ONTOP markers)
int GetTitleBarFlag(void) ;
void SetTitleBarFlag( const int flag ) ;

// Flag to switch to image-viewer mode
// extern int ImageViewerFlag ;
int GetImageViewerFlag(void) ;
void SetImageViewerFlag( const int flag ) ;

// Time (in seconds) between background image switches (<=0 = no slideshow)
extern int ImageSlideDelay ;

// Flag protecting the window against accidental keyboard input
// extern int ProtectFlag ;
int GetProtectFlag(void) ;

// Flag defining the visibility of a window
// extern int VisibleFlag ;
int GetVisibleFlag(void) ;
void SetVisibleFlag( const int flag ) ;

// Script file handling at startup
extern char * ScriptFileContent ;

// Flag to disable the keyboard shortcuts
// extern int ShortcutsFlag ;
int GetShortcutsFlag(void) ;
void SetShortcutsFlag( const int flag ) ;

// Flag to disable the mouse shortcuts
// extern int MouseShortcutsFlag ;
int GetMouseShortcutsFlag(void) ;
void SetMouseShortcutsFlag( const int flag ) ;
extern HDROP hDropInf;

// Pointer to the automatic command
extern char * AutoCommand ;

// Content of a script to send to the screen
extern char * ScriptCommand ;

// paste size limit (number of characters). Above the limit a confirmation is requested. (0 means unlimited)
int GetPasteSize(void) ;
void SetPasteSize( const int size ) ;

// Max chained SSH proxies before refusing; kitty.ini [KiTTY] proxychainmax (default 5)
int GetProxyChainMax(void) ;
void SetProxyChainMax( const int n ) ;

// Flag controlling the hyperlink feature
extern int HyperlinkFlag ;
int GetHyperlinkFlag(void) ;
void SetHyperlinkFlag( const int flag ) ;

// Flag enabling the folder tree when savemode=dir, defined in
// kitty_commun.c
//extern int DirectoryBrowseFlag ;
int GetDirectoryBrowseFlag(void) ;

// Send the window to the tray automatically (for tunnels); goes with the
// -send-to-tray option
//extern int AutoSendToTray ;
int GetAutoSendToTray( void ) ;
void SetAutoSendToTray( const int flag ) ;

// Flag controlling transparency
// extern int TransparencyFlag ;
int GetTransparencyFlag(void) ;

// Flag to avoid creating the kitty.ini and kitty.sav files
// extern int NoKittyFileFlag ;
int GetNoKittyFileFlag(void) ;

// Height of the configuration box
// extern int ConfigBoxHeight ;
int GetConfigBoxHeight(void) ;
void SetConfigBoxHeight( const int num ) ;

// Height of the configuration box window (0 = default value)
// static int ConfigBoxWindowHeight = 0 ;
int GetConfigBoxWindowHeight(void) ;
void SetConfigBoxWindowHeight( const int num ) ;

// Window height used by the winrol function
// extern int WinHeight ;
int GetWinHeight(void) ;
// Flag to disable the Winrol (window rollup)
// extern int WinrolFlag = 1
int GetWinrolFlag(void) ;
void SetWinrolFlag( const int num ) ;

// Flag to disable the filter on the configuration box session list
// extern int SessionFilterFlag ;
// [ConfigBox] filter=yes
int GetSessionFilterFlag(void) ;
void SetSessionFilterFlag( const int flag ) ;

// Flag to disable the automatic creation of the Default Settings session
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


// Path to the WinSCP program
extern char * WinSCPPath ;

/* path to the file-copy helper: kscp.exe, or PuTTY's pscp.exe */
extern char * PSCPPath  ;

// Startup directory
extern char InitialDirectory[4096] ;

// Counter for sending the anti-idle string
extern int AntiIdleSeconds ;   /* KiTTY: keepalive interval, in seconds */
extern char AntiIdleStr[128] ;
int ReadParameterN( const char * key, const char * name, char * value, size_t size ) ;
int ReadParameter( const char * key, const char * name, char * value ) ; /* compat: value >= 4096 bytes; prefer ReadParameterN */
int WriteParameter( const char * key, const char * name, char * value ) ;
int DelParameter( const char * key, const char * name ) ;
void CreateDefaultIniFile( void ) ;
char * GetKittyIniFile(void) ;
char * GetKittySavFile(void) ;



/* ---- exported from kitty/kitty.c ---- */
int GetConfigBoxApplicationSettingsFlag(void);
int GetConfigBoxFixedSizeFlag(void);
int GetConfigBoxWindowWidth(void);
int GetFunkeysDefault( void );
char * GetIconFile(void);
int GetTransparencyAllowed(void);
void LoadParameters( void );
void SetConfigBoxFixedSizeFlag( const int flag );
void SetConfigBoxWindowWidth( const int num );
void SetFunkeysDefault( const int t );
void SetIconFile( const char * path );
void SetNamedProxyHostnameOnly( const int flag );
void SetPSCPPath( const char * path );
void SetReconnectDelay( const int flag );
void SetTransparencyEnabled( const int flag );
int kitty_named_proxy_default_hostname( void );

#endif /* KITTY_PARAMS_H */
