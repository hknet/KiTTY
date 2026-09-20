/*
 * kitty.h - the shared declaration header of the KiTTY fork.
 * Every module of the fork includes it. It declares the [KiTTY] and
 * [ConfigBox] settings flags with their Get/Set accessors, the delays,
 * paths and buffers kitty.c defines, the shortcut
 * tables and their entry points, the window, tray, icon and script
 * helpers, and the kitty.ini/registry parameter functions. It also
 * pulls in kitty_rc_additions.h so the menu command ids have exactly
 * one definition.
 */
#ifndef KITTY_H
#define KITTY_H
#include "putty.h"   /* Conf, Terminal, TermWin, Backend, and windows.h via platform.h */
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

// Handle to the main window
//extern HWND MainHwnd ;
HWND GetMainHwnd(void) ;

// Flag to show the background image
//extern int BackgroundImageFlag ;
int GetBackgroundImageFlag(void) ;
void SetBackgroundImageFlag( const int flag ) ;


// Name of the application window class
extern char KiTTYClassName[128] ;

// Reapply the title decorations after a state change (windows/window.c)
void kitty_refresh_title(void) ;

// KiTTY: expand window-title placeholders (%%h, %%s, %%u, %%p, %%P, %%f, %%l, %%d)
char *kitty_expand_wintitle(const char *title, const char *hostname, Conf *conf) ;

#ifdef MOD_PROXY
// Flag adding the Proxy Selector feature
// extern int ProxySelectionFlag ;
int GetProxySelectionFlag() ;
void SetProxySelectionFlag( const int flag ) ;
#endif

// Stuff for drag-n-drop
#ifndef TIMER_DND
#define TIMER_DND 8777
#endif
void recupNomFichierDragDrop(HWND hwnd, HDROP* leDrop) ;

// RuTTY script engine master switch: [KiTTY] scriptmode=yes|no (kitty_rutty.c)
int kitty_script_enabled(void) ;
void kitty_script_set_enabled( int on ) ;

// Flag for "portable" mode (settings kept in files), defined in
// kitty_commun.c
extern int IniFileFlag ;
int GetIniFileFlag(void) ;

#ifdef MOD_ZMODEM
// Flag to disable the ZMODEM functions
// extern int ZModemFlag ;
int GetZModemFlag(void) ;
void SetZModemFlag( const int flag ) ;
#endif

// Flag disabling the automatic saving of the login details (user/password)
// on an SSH connection
// extern int UserPassSSHNoSave ; ==> defined in kitty_commun.c
int GetUserPassSSHNoSave(void) ;
void SetUserPassSSHNoSave( const int flag ) ;

// Extension of the session files in portable mode (may be ktx)
extern char FileExtension[15] ;

// Directory the configuration is saved in (savemode=dir)
extern char * ConfigDirectory ;

// Flag telling whether we are connected
extern int is_backend_connected ;

#ifdef MOD_RECONNECT
/* Variable telling that we have already been connected once */
extern int is_backend_first_connected ; 
#endif

/* Flag forbidding the configuration box from being opened */
extern int force_reconf ; 

extern NOTIFYICONDATA TrayIcone ;   /* defined in kitty.c */
#ifndef MYWM_NOTIFYICON
#define MYWM_NOTIFYICON		(WM_USER+3)
#endif

// The library to look the icons up in (the file named in kitty.ini, else
// kitty.dll if it exists, else kitty.exe)
// extern HINSTANCE hInstIcons ;

extern int debug_flag ;

// Function prototype declarations
void InitFolderList( void ) ;
void SaveFolderList( void ) ;
void InfoBoxSetText( HWND hwnd, char * st ) ;
void InfoBoxClose( HWND hwnd ); 
void SetNewIcon( HWND hwnd, char * iconefile, int icone, const int mode ) ;
int WINAPI Notepad_WinMain(HINSTANCE hinstance, HINSTANCE hPrevInstance,LPSTR lpCmdLine, int nCmdShow) ;
void ShowInputBox( HINSTANCE hInstance, HWND hwnd ) ;   /* modeless single-line box */
char * InputBoxMultiline( HINSTANCE hInstance, HWND hwnd ) ;
char * InputBoxPassword( HINSTANCE hInstance, HWND hwnd ) ;
char * GetInputBoxResult( void ) ;
void GetAndSendLine( HWND hwnd ) ;
void GetAndSendMultiLine( HWND hwnd ) ;
void routine_inputbox( void * phwnd ) ;
void routine_inputbox_multiline( void * phwnd ) ;
void GetAndSendLinePassword( HWND hwnd ) ;
void RunScriptFile( HWND hwnd, const char * filename ) ;
void ReadInitScript( const char * filename ) ;
void GetSessionFolderName( const char * session_in, char * folder ) ;
char * SetSessPath( const char * dec ) ;
void CleanFolderName( char * folder ) ;
void set_sshver( const char * vers ) ;
void RunCommand( HWND hwnd, const char * cmd ) ;
void load_open_settings_forced(char *filename, Conf *conf) ;
void save_open_settings_forced(char *filename, Conf *conf) ;
int SwitchCryptFlag( void ) ;
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
	int filezilla ;
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
	int fontreset ;
	int transparencyup ;
	int transparencydown ;
	} ;
extern struct TShortcuts shortcuts_tab ;
struct TShortcuts2 { int num ; char * st ; } ;
extern struct TShortcuts2 shortcuts_tab2[512] ;
extern int NbShortCuts ;
int DefineShortcuts( char * buf ) ;
void TranslateShortcuts( char * st ) ;
void InitShortcuts( void ) ;
int ManageShortcuts( Terminal *term, Conf *conf, HWND hwnd, const int* clips_system, int key_num, int shift_flag, int control_flag, int alt_flag, int altgr_flag, int win_flag ) ;
/* Shortcut code -> "Ctrl+F3" (the inverse of DefineShortcuts); the key bound
 * to a menu command id; "<menu text>\t<key text>" for a menu item. */
int ShortcutKeyText( int key, char * buf, size_t size ) ;
int GetShortcutKey( int idm ) ;
const char * ShortcutMenuText( const char * text, int key, char * buf, size_t size ) ;
/* The action table behind the Shortcuts panel: one row per [Shortcuts]
 * action - its ini key, its display name, the key in force, its default. */
int ShortcutActionCount( void ) ;
const char * ShortcutActionKey( int i ) ;
const char * ShortcutActionName( int i ) ;
int ShortcutActionValue( int i ) ;
int ShortcutActionDefault( int i ) ;
/* A shortcut code from a virtual key and the modifier flags, as
 * ManageShortcuts composes it; the code written back in the kitty.ini
 * syntax ({CONTROL}{SHIFT}{F4}, the inverse of DefineShortcuts); the
 * user-command slot (1..26) a Ctrl+Shift+letter code shares, 0 for any other;
 * whether a code is one KiTTY keeps for itself (1 = a fixed alias, 2 = the
 * window-closing Alt+F4). */
int ShortcutKeyCode( int vk, int shift, int control, int alt, int altgr, int win ) ;
int ShortcutKeySyntax( int key, char * buf, size_t size ) ;
int ShortcutKeyUserCommand( int key ) ;
int ShortcutKeyReserved( int key ) ;
// Get one entry of a session (returns 1 if it exists)
int GetSessionField( const char * session_in, const char * folder_in, const char * field, char * result ) ;
// Save the window coordinates
// Count the windows of the same class as KiTTY
HWND InfoBox( HINSTANCE hInstance, HWND hwnd ) ;
// Rename a registry key
void RegRenameTree( HWND hdlg, HKEY hMainKey, LPCTSTR lpSubKey, LPCTSTR lpDestKey ) ;
void RenewPassword( Conf *conf ) ;
// Put a login typed at the SSH prompt into the running session (hknet/KiTTY#50)
void SetPasswordInConfig( const char * password ) ;
void SetUsernameInConfig( const char * username ) ;
void kitty_userauth_credentials( Seat * seat, const char * username, const char * password ) ;
// Handles sending the window to the system tray
int ManageToTray( HWND hwnd ) ;
void RefreshBackground( HWND hwnd ) ;
int NextBgImage( HWND hwnd ) ;
int PreviousBgImage( HWND hwnd ) ;
void ManageSpecialCommand( HWND hwnd, int menunum ) ;
// Backup of the registry key
void ManageProtect( HWND hwnd, TermWin *tw, char * title ) ;
void ManagePrint( HWND hwnd ) ;
// Handles the always visible option
void ManageVisible( HWND hwnd, TermWin *tw, char * title ) ;
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
void GetOneFileTo( HWND hwnd, char * directory, const char * filename, const char * localdir ) ;
/* localfile != NULL: kscp writes to that exact local path (a single named
 * file, the Save-As case) instead of into localdir. */
void GetOneFileToPath( HWND hwnd, char * directory, const char * filename, const char * localdir, const char * localfile ) ;
/* final_dir != NULL: a staged wildcard/folder download - localdir is the
 * staging folder, final_dir where files move on success, lock the marker. */
void GetOneFileStaged( HWND hwnd, char * directory, const char * filename, const char * localdir, const char * localfile, const char * final_dir, HANDLE lock ) ;
void kitty_xfer_sweep_downloads( void ) ;   /* remove stale staging folders at startup */
void GetFile( HWND hwnd ) ;
/* The session's download folder, resolved (Connection > File-Transfer-Settings, else the
 * global one, else Downloads). kitty_xfer.c */
char * kitty_xfer_download_dir( Conf * cf, char * out, size_t outlen ) ;
/* The session's upload folder, resolved (Connection > File-Transfer-Settings, else the
 * global Default Upload Folder, else Documents): where Send File opens and
 * where a plain name the far end asks to read is looked up. kitty_xfer.c */
char * kitty_xfer_upload_dir( Conf * cf, char * out, size_t outlen ) ;
/* The tray balloon after a finished transfer, gated by [KiTTY]
 * transfernotification; `what` fills KT_XFER_COMPLETE. A click on the
 * balloon opens `path` (a file: selected in Explorer; a folder: opened):
 * files arriving - the one file, or the folder they landed in; files
 * leaving - the local upload folder. arriving adds the "saved to" line
 * (nfiles: 1, the count, or 0 = not counted). kitty_xfer.c */
void kitty_xfer_notify( const char * what, int arriving, int nfiles, const char * path ) ;
/* ... with the count of files a Get File saved under a new name (0 = none);
 * the balloon then says so. kitty_xfer.c */
void kitty_xfer_notify_ex( const char * what, int arriving, int nfiles, const char * path, int renamed ) ;
int kitty_xfer_notify_enabled( void ) ;
/* Is the helper there? 0 = kscp, 1 = WinSCP, 2 = FileZilla. kitty_xfer.c */
int kitty_xfer_tool_ready( int which ) ;
/* Does the session show the Tools menu entry? 0 = Send File, 1 = WinSCP,
 * 2 = FileZilla, 3 = Get File (Connection > File-Transfer-Settings). kitty_xfer.c */
int kitty_xfer_tool_shown( Conf * cf, int which ) ;
/* The port a transfer tool WILL use when its Port field is empty, so that a
 * panel can show it as a hint and the command builders can use it as the
 * fallback. tool: 0 = kscp, 1 = WinSCP, 2 = FileZilla. protocol: that tool's
 * own protocol value (CONF_kscp_protocol / CONF_winscpprot /
 * CONF_filezilla_protocol; 0=scp 1=sftp 2=ftp 3=ftps 4=ftpes 5=http 6=https).
 *   - kscp: the global kscp port ([KiTTY] pscpport, "*" = the session's port),
 *     else the target override's ":port", else the session's port.
 *   - WinSCP and FileZilla: a port written into the target override
 *     (CONF_sftpconnect, "[user@]hostname[:port]") wins over everything, the
 *     tool's own Port field included - the override names the machine those two
 *     are to reach, port and all. An override that names no port supplies user
 *     and host only, and then the rules below decide.
 *   - scp and sftp: the session's port for an SSH session, 22 otherwise - a
 *     telnet or raw port is not an SFTP port.
 *   - ftp and ftpes: 21. ftps (implicit TLS): 990. http: 80. https: 443.
 * kitty_xfer.c */
int kitty_xfer_default_port( Conf * cf, int tool, int protocol ) ;
/* A Port field's value: the number it holds when that is a plain decimal number
 * in 1..65535, else 0 = not set (an empty field, and anything unusable). The
 * panels use the same test to decide whether to show the default-port hint.
 * kitty_xfer.c */
int kitty_xfer_port_field( const char * s ) ;
/* A session saved before the per-tool protocol split carries WinSCPProtocol
 * alone: derive KscpProtocol and FileZillaProtocol from it. The load paths call
 * this only when the two new keys are absent from the stored session, so it
 * never overwrites a value that was chosen. A load-path rule, so it lives with
 * the loader in kitty_settings_load.c and not with the transfer code: the .ktx
 * reader is linked into targets that have no transfer code at all. */
void kitty_xfer_migrate_protocol( Conf * cf, int oldprot ) ;
/* FileZilla hand-off (kitty_xfer.c): the executable path is an application
 * setting ([KiTTY] FileZillaPath), located like WinSCP's. */
extern char * FileZillaPath ;
int SearchFileZilla( void ) ;
void StartFileZilla( HWND hwnd ) ;
void RunCmd( HWND hwnd ) ;
int SearchWinSCP( void ) ;
int SearchPSCP( void ) ;
void urlhack_launch_url(const char* app, const char *url) ;
void OnDropFiles(HWND hwnd, HDROP hDropInfo) ;
// Show a menu in the system tray
// Get the window coordinates
// Startup script handling
void ManageInitScript( const char * input_str, const int len ) ;

char * get_param_str( const char * val ) ;

// Functions encoding and decoding strings with the %XY escape form
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
void ManageShortcutsFlag( HWND hwnd ) ;

#ifdef MOD_LAUNCHER
void InitLauncherRegistry( void ) ;
#endif

// Section name of the configuration file
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
 * silently kitty.sav for a long time. Keep it here, and only here. */
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
#endif
#ifndef TIMER_ANTIIDLE
#endif
#ifndef TIMER_RECONNECT
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
** DEFINES
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

// Must be the last one
#ifndef IDM_LAUNCHER
#define IDM_LAUNCHER	0xB130
#endif

// USERCMD must be the largest value, so that there can be as many shortcuts
// as wanted
#ifndef IDM_USERCMD
#define IDM_USERCMD   0x8000
#endif

// Same as USERCMD
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
** END OF DEFINES
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



// Defines taken from WINDOW.C that kitty.c needs
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
int kitty_hotkey_of_session( const char * name, unsigned int * mods, unsigned int * vk,
                             char * spec_out, int speclen ) ;
int kitty_hotkey_conflict_scan( unsigned int mods, unsigned int vk,
                                const char * exclude, char * names, int nameslen ) ;
int kitty_hotkey_enabled_count( const char * exclude ) ;
int kitty_hotkey_conflict_report( char * buf, int buflen ) ;
int RestoreFromTray( HWND hwnd );
void SetConnBreakIcon( HWND hwnd );
void debug_log( const char *fmt, ... );
int get_param( const char * val );
char * kitty_current_dir(void);
void kitty_fontfallback_apply_list( const char * list );
char *kitty_loginscript_from_text( const char *text );
char *kitty_loginscript_to_text( const char *stored );
void kitty_netdbg_ts( const char *msg );
void kitty_set_remote_cwd( const char * osc7 );
void set_title( TermWin *tw, const char *title );
extern char BuildVersionTime[256];
extern char **FolderList;

#endif // KITTY_H
