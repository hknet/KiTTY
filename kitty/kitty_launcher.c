/*
 * kitty_launcher.c - the KiTTY launcher: a tray-icon process whose menu lists
 * the saved sessions (built from the Launcher key or directory tree), starts
 * them, and offers the configuration box, the session editor and the list of
 * open KiTTY windows (hide / unhide / switch).
 * It also owns the launcher-side extras: global per-session hotkeys, the
 * "update available" tray balloon, workplace proxy mode arming with its
 * notices and timers, and the optional Startup-folder shortcut.
 * The tray part is compiled only when MOD_LAUNCHER is defined; the spawning
 * helpers after it (RunConfig, RunPuTTY, RunSession) are always built and
 * assemble the child command lines, passing on the restricted ACL and the
 * shared master-password unlock.
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
#include "kitty_params.h"
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
#include "kitty_theme.h"            /* kitty_theme_app_mode: the menus' theme, re-read per menu */
#include "kitty_updater.h"
#include "kitty_winutil.h"
#include "kitty_dlgbox.h"
#include "kitty_launcher.h"
#include "winfont_fallback.h"
#include "kitty_msgbox.h"   /* themed MessageBox routing */
#include "kitty_oldwin_reg.h"   /* XP: RegDeleteTree/RegGetValue via oldwin */
#include "kitty_text.h"   /* shared captions and wordings (also for the .c files included below) */
#include "kitty_inikeys.h"   /* KI_*: the kitty.ini key names */
#include "kitty_notes.h"   /* the application notification, marked owed at startup */
#include "kitty_pwmem.h"   /* passwords wrapped in memory (kitty_commands.c too) */
#include "kitty_storage.h"
#include "kitty_secretstore.h"
#include "kitty_gui.h"
#include "kitty_bridge.h"
#include "kitty_exportbundle.h"
#include "mini/mini.h"
#include "kitty_buildlabel.h"   /* the test build's label, if this is one */
#ifdef MOD_PROXY
#include "kitty_proxy.h"   /* kitty.c includes it mid-file, fenced the same way */
#endif

#ifdef MOD_LAUNCHER

/* IDI_PUTTY_LAUNCH / IDI_BLACKBALL come from kitty_rc_additions.h (via
 * kitty.h); both are the application icon, IDI_MAINICON. */
#ifndef IDI_PUTTY_LAUNCH
#error "kitty_launcher.c needs kitty_rc_additions.h for IDI_PUTTY_LAUNCH"
#endif

#define KLWM_NOTIFYICON		(WM_USER+2)
#define KLWM_UPDATECHECKDONE	(WM_USER+12)
/* Posted from WM_CREATE: the "workplace proxy mode is off, click to switch it
 * on" offer. Posted rather than called, for the reason the update balloon is -
 * a balloon raised from inside window creation shows, but a click on it never
 * comes back. */
#define KLWM_WORKPLACEOFFER	(WM_USER+13)
/* Posted by the "mode is OFF" notice window when it is clicked. */
#define KLWM_WORKPLACEREARM	(WM_USER+14)
/* Posted by LauncherRegisterHotkeys when session hotkeys collided: the balloon
 * naming winners and losers. Posted rather than shown in place, for the reason
 * the update balloon is - registration also runs from WM_CREATE, and a balloon
 * raised inside window creation shows but its click never comes back. */
#define KLWM_HOTKEYBALLOON	(WM_USER+15)
/* Posted from WM_CREATE: the application notification, if this process owes
 * it and the launcher's window is the first one it opens. Posted for the
 * reason the two above are - a window raised from inside window creation is
 * shown, but the clicks on it do not come back. */
#define KLWM_NOTESPENDING	(WM_USER+16)
/* (WM_USER+17 is KLWM_OPENFOLDER, defined with the folder context menu.)
 * Refresh was chosen in the OPEN tray menu: the menu filter swallowed the
 * click, so the menu is still on the screen, and the refresh runs behind it. */
#define KLWM_REFRESHINPLACE	(WM_USER+18)
/* The spinner beside "Refresh", and the end of its minimum showing time. */
#define LAUNCHER_REFRESHSPIN_TIMER	104
#define LAUNCHER_REFRESHSPIN_STEP_MS	80
#define LAUNCHER_REFRESHSPIN_MIN_MS	300
/* KiTTY: timer id for the delayed single-left-click tray menu (so a double
 * click - new default window - doesn't pop the menu up first) */
#define LAUNCHER_TRAYCLICK_TIMER	100
/* Workplace proxy mode: notices the arming's time running out, so the mode ends
 * visibly rather than only when the next connection is made. */
#define LAUNCHER_WORKPLACE_TIMER	101
/* A launcher the mode started is closing, but not before the notice saying the
 * mode is off has been readable - and not at all if the user takes that
 * notice's offer to switch the mode back on. */
#define LAUNCHER_EXITAFTERNOTICE_TIMER	102
#define LAUNCHER_HOTKEY_BASE	0x4B00
/* Slot count lives in kitty_defs.h: the config box enforces the same limit
 * when a hotkey is enabled, and the two sides must not drift apart. */
#define LAUNCHER_HOTKEY_MAX	KITTY_LAUNCHER_HOTKEY_MAX
#define KITTY_LAUNCHER_REFRESH_MESSAGE "KiTTYLauncherRefreshSessionsAndHotkeys"
#ifndef MOD_NOREPEAT
#define MOD_NOREPEAT 0x4000
#endif
int RunSession( HWND hwnd, const char * folder_in, char * session_in ) ;
static void RunPuTTYAtPanel( HWND hwnd, const char * panel, int mark_loaded ) ;

/* KiTTY: runtime registry base (windows/storage.c). The launcher must read
 * KiTTY's own hive (Software\9bis.com\KiTTY) -- where sessions actually live --
 * not the compile-time PUTTY_REG_POS macro (stock PuTTY's SimonTatham hive).
 * In the -launcher process nothing calls kitty_set_registry_root(), so this
 * returns the default KiTTY base; the launcher therefore does NOT follow
 * KiClassName=PuTTY mode -- deliberate, acceptable for the launcher. */

#include "kitty_startup_shortcut.h"   /* KiTTY: on-request Startup-folder shortcut */
#include "kitty_workplace.h"          /* KiTTY: workplace proxy mode arming */
#include "kitty_notice.h"           /* KiTTY: our own arm/disarm notice window */

static HMENU MenuLauncher = NULL ;
static HMENU HideMenu ;
/* A Refresh chosen in the open tray menu (see LauncherRefreshInPlace). */
static int LauncherRefreshBusy = 0 ;
static HMENU LauncherPendingMenu = NULL ;
static int LauncherConfReload = 1 ;
static POINT LauncherMenuPoint ;
static int LauncherMenuPointValid = 0 ;
/* KiTTY: cursor position of the single left click, used when the delayed
 * tray-menu timer fires */
static POINT LauncherClickPoint ;
/* KiTTY: a double click arrives as down/up/dblclk/up - swallow the trailing
 * button-up so it doesn't re-arm the single-click timer */
static int LauncherIgnoreUp = 0 ;
static int LauncherUpdateKnown = 0 ;
static char LauncherUpdateLatest[64] = "" ;
static int LauncherUpdateBeta = 0 ;
/* The update balloon is shown at most once per launcher run: the check runs
 * twice (the cached answer at startup, then the async fetch) and each used to
 * raise its own balloon. */
static int LauncherUpdateBalloonShown = 0 ;

/* The proxy the "mode is OFF" notice offered to switch back on, so a click on
 * that notice knows which one it meant. */
static char LauncherRearmProxy[256] = "" ;

/* Workplace proxy mode: the proxy this launcher is arming with while the mode
 * is on, empty while it is off. Declared here because the tray menu is built
 * further up this file than the workplace helpers are defined. */
static char LauncherWorkplaceProxy[256] = "" ;
static int LauncherRememberedWorkplaceProxy( char *out, int len ) ;

struct LauncherHotkey {
	int id ;
	UINT modifiers ;
	UINT vk ;
	char folder[1024] ;
	char session[1024] ;
} ;
static struct LauncherHotkey LauncherHotkeys[LAUNCHER_HOTKEY_MAX] ;
static int LauncherHotkeyCount = 0 ;
static UINT LauncherRefreshMessage = 0 ;
/* Hotkey-conflict balloon state. The report is remembered so re-registration
 * (every saved-session refresh re-runs it) balloons only when the conflicts
 * CHANGED, not on every refresh of an unchanged store. The winner of the first
 * conflict is kept so a click on the balloon can open the config box on it. */
static char LauncherHotkeyReport[256] = "" ;
static char LauncherHotkeyWinner[256] = "" ;
static int LauncherHotkeyBalloonArmed = 0 ;

// Hide/UnHide all handling
static struct THWin { HWND hwnd ; char name[128] ; } TabWin[100] ;
static int oldIconFlag = 0 ;
static int NbWin = 0 ;
static int IsUnique = 0 ;
int RefreshWinList( HWND hwnd ) ;

// Build a menu from a registry key
static HMENU InitLauncherMenu( char * Key ) {
	HMENU menu ;
	menu = CreatePopupMenu() ;
	char KeyName[1024] ;
	int nbitem = 0,i ;
	
	if( (IniFileFlag == SAVEMODE_REG)||(IniFileFlag == SAVEMODE_FILE) ) {
		snprintf( KeyName, sizeof(KeyName), "%s\\%s", kitty_registry_base(), Key ) ;
		ReadSpecialMenu( menu, KeyName, &nbitem, 0 ) ;
	} else if( IniFileFlag == SAVEMODE_DIR ) {
		ReadSpecialMenu( menu, Key, &nbitem, 0 ) ;
	}

	if( GetMenuItemCount( menu ) > 0 )
		AppendMenu( menu, MF_SEPARATOR, 0, 0 ) ;

	if( LauncherUpdateKnown && LauncherUpdateLatest[0] ) {
		char upmsg[160] ;
		/* KiTTY: clickable, like the balloon. This was a greyed label stating
		 * that an update exists and leaving the user to find the terminal's
		 * "Check for updates" themselves; it now opens that same updater. */
		snprintf( upmsg, sizeof(upmsg), KT_MENU_UPDATE_AVAILABLE,
		          LauncherUpdateLatest, LauncherUpdateBeta ? KT_UPD_BETA_SUFFIX : "" ) ;
		AppendMenu( menu, MF_ENABLED, IDM_LAUNCHER+9, upmsg ) ;
		AppendMenu( menu, MF_SEPARATOR, 0, 0 ) ;
	}

	// Build the left-button menu
	/* Not while a Refresh builds the NEXT menu behind the one on the screen:
	 * the old "Opened sessions" popup is a submenu of that open menu and goes
	 * with it when it is destroyed. */
	if( !LauncherRefreshBusy ) DestroyMenu( HideMenu ) ;
	HideMenu = CreatePopupMenu() ;
	if( !IsUnique ) {
		AppendMenu( HideMenu, MF_ENABLED, IDM_LAUNCHER+3, KT_MENU_HIDE_ALL ) ;
		AppendMenu( HideMenu, MF_ENABLED, IDM_LAUNCHER+4, KT_MENU_UNHIDE_ALL ) ;
		//AppendMenu( HideMenu, MF_ENABLED, IDM_LAUNCHER+5, "&Refresh list" ) ;
		AppendMenu( HideMenu, MF_ENABLED, IDM_LAUNCHER+6, KT_MENU_WINDOW_UNIQUE ) ;
		CheckMenuItem( HideMenu, IDM_LAUNCHER+6, MF_BYCOMMAND | MF_UNCHECKED) ;
	} else {
		AppendMenu( HideMenu, MF_ENABLED, IDM_LAUNCHER+6, KT_MENU_WINDOW_UNIQUE ) ;
		CheckMenuItem( HideMenu, IDM_LAUNCHER+6, MF_BYCOMMAND | MF_CHECKED) ;
	}
	//AppendMenu( HideMenu, MF_ENABLED, IDM_GONEXT, "&Next" ) ;
	//AppendMenu( HideMenu, MF_ENABLED, IDM_GOPREVIOUS, "&Previous" ) ;
	if( RefreshWinList( MainHwnd ) > 0 ) {
		AppendMenu( HideMenu, MF_SEPARATOR, 0, 0 ) ;
		for( i=0 ; i<NbWin ; i++ ) {
			AppendMenu( HideMenu, MF_ENABLED, IDM_GOHIDE+i, TabWin[i].name ) ;
			if( IsWindowVisible( TabWin[i].hwnd ) ) 
				CheckMenuItem( HideMenu, IDM_GOHIDE+i, MF_BYCOMMAND | MF_CHECKED) ;
			else 
				CheckMenuItem( HideMenu, IDM_GOHIDE+i, MF_BYCOMMAND | MF_UNCHECKED) ;
		}
	}

	
	AppendMenu( menu, MF_POPUP, (UINT_PTR)HideMenu, KT_MENU_OPENED_SESSIONS ) ;
	AppendMenu( menu, MF_SEPARATOR, 0, 0 ) ;

	/* A blank right-hand column beside "Refresh": the spinner of a running
	 * refresh is written into it, and an open menu cannot grow. */
	AppendMenu( menu, MF_ENABLED, IDM_LAUNCHER+7, KT_MENU_REFRESH "\t   " ) ;
	AppendMenu( menu, MF_ENABLED, IDM_LAUNCHER+1, KT_MENU_CONFIGURATION ) ;
	AppendMenu( menu, MF_ENABLED, IDM_LAUNCHER+2, KT_MENU_TTYED ) ;
	/* KiTTY: workplace proxy mode. While it is on, ONE item that says which
	 * proxy everything is going through and switches it off; while it is off, a
	 * submenu of the named proxies to switch it on with, the remembered one
	 * ticked. The wording says "every connection" both ways round because that
	 * is the whole point of the mode, and because a proxy override left on by
	 * accident is the risk this feature has to keep visible. */
	{
		AppendMenu( menu, MF_SEPARATOR, 0, 0 ) ;
		if( kitty_workplace_holding() ) {
			char item[400], left[64] ;
			kitty_workplace_left_text( left, sizeof(left) ) ;
			if( left[0] )
				snprintf( item, sizeof(item),
					KT_MENU_WORKPLACE_ON_LEFT,
					LauncherWorkplaceProxy, left ) ;
			else
				snprintf( item, sizeof(item),
					KT_MENU_WORKPLACE_ON,
					LauncherWorkplaceProxy ) ;
			AppendMenu( menu, MF_ENABLED, IDM_WORKPLACE, item ) ;
		} else {
			HMENU wpmenu = CreatePopupMenu() ;
			char remembered[256] ;
			int have_remembered = LauncherRememberedWorkplaceProxy( remembered, sizeof(remembered) ) ;
			int i, n = 0 ;
			InitProxyList() ;
			for( i = 2 ; i < MAX_PROXY && proxies[i].name ; i++ ) {
				/* The last-used one is LABELLED, not ticked: a tick on a menu
				 * item reads as something you can turn off, and there is
				 * nothing here to turn off - every item is "switch the mode on
				 * with this proxy". */
				char item[320] ;
				if( have_remembered && !strcmp( remembered, proxies[i].name ) )
					snprintf( item, sizeof(item), KT_MENU_WORKPLACE_LAST_USED, proxies[i].name ) ;
				else
					snprintf( item, sizeof(item), "%.250s", proxies[i].name ) ;
				AppendMenu( wpmenu, MF_ENABLED, IDM_WORKPLACE+1+(i-2), item ) ;
				n++ ;
			}
			if( n > 0 ) {
				AppendMenu( menu, MF_POPUP, (UINT_PTR)wpmenu,
					KT_MENU_WORKPLACE_OFF ) ;
			} else {
				/* No named proxies: say why rather than offer an empty submenu. */
				DestroyMenu( wpmenu ) ;
				AppendMenu( menu, MF_GRAYED, 0,
					KT_MENU_WORKPLACE_NEEDS_PROXY ) ;
			}
		}
		AppendMenu( menu, MF_SEPARATOR, 0, 0 ) ;
	}
	/* KiTTY: user-Startup-folder shortcut for the launcher, on request.
	 * Checked only when a "KiTTY Launcher" shortcut pointing at THIS exe
	 * exists (user or all-users) - a same-named shortcut for a different
	 * KiTTY (e.g. the installer's, targeting the installed kitty.exe) is not
	 * this launcher's autostart. */
	{ char mx[MAX_PATH] ; DWORD mn = GetModuleFileNameA( NULL, mx, sizeof(mx) ) ;
	  int on = mn && mn < sizeof(mx) &&
	           ( kitty_startup_shortcut_points_to("KiTTY Launcher", 0, mx)
	             || kitty_startup_shortcut_points_to("KiTTY Launcher", 1, mx) ) ;
	  AppendMenu( menu, MF_ENABLED | (on ? MF_CHECKED : MF_UNCHECKED),
	              IDM_LAUNCHER+8, KT_MENU_START_AT_LOGIN ) ; }
	AppendMenu( menu, MF_SEPARATOR, 0, 0 ) ;
	AppendMenu( menu, MF_ENABLED, IDM_ABOUT, KT_MENU_ABOUT ) ;
	AppendMenu( menu, MF_ENABLED, IDM_QUIT, KT_MENU_EXIT ) ;

	return menu ;
}

#ifdef KITTY_TEST_BUILD_LABEL
/*
 * TEST BUILDS ONLY: how long the two halves of a menu refresh take - the copy
 * of the store into Launcher\ (InitLauncherRegistry) and the menu read from
 * that copy (InitLauncherMenu). Written only when KITTY_LAUNCHER_TIMING is set
 * in the environment, one line per call, to launcher_timing.log beside the
 * executable. A release knows neither the variable nor the code.
 */
static double LauncherTimingNow( void ) {
	LARGE_INTEGER f, c ;
	QueryPerformanceFrequency( &f ) ;
	QueryPerformanceCounter( &c ) ;
	return 1000.0 * (double)c.QuadPart / (double)f.QuadPart ;
}
static void LauncherDebugLine( const char * line ) {
	char path[MAX_PATH], * slash ;
	FILE * fp ;
	if( getenv( "KITTY_LAUNCHER_TIMING" ) == NULL ) return ;
	if( !GetModuleFileNameA( NULL, path, sizeof(path) - 32 ) ) return ;
	if( (slash = strrchr( path, '\\' )) == NULL ) return ;
	strcpy( slash + 1, "launcher_timing.log" ) ;
	if( (fp = fopen( path, "a" )) != NULL ) {
		fprintf( fp, "%s\n", line ) ;
		fclose( fp ) ;
	}
}
static void LauncherTimingLog( const char * what, double started ) {
	char line[128] ;
	snprintf( line, sizeof(line), "%s %.3f", what, LauncherTimingNow() - started ) ;
	LauncherDebugLine( line ) ;
}
#define LAUNCHER_DEBUG_LINE(l)	LauncherDebugLine( l )
#define LAUNCHER_TIMING_START	double launcher_t0 = LauncherTimingNow()
#define LAUNCHER_TIMING_END(w)	LauncherTimingLog( (w), launcher_t0 )
#else
#define LAUNCHER_TIMING_START	((void)0)
#define LAUNCHER_TIMING_END(w)	((void)0)
#define LAUNCHER_DEBUG_LINE(l)	((void)0)
#endif

static void RefreshMenuLauncher( void ) {
	LAUNCHER_TIMING_START ;
	DestroyMenu( MenuLauncher ) ;
	MenuLauncher = NULL ;
	MenuLauncher = InitLauncherMenu( "Launcher" ) ;
	LAUNCHER_TIMING_END( "menu" ) ;
}
	
// Delete a directory tree   ==> moved to kitty_commun.c
/*
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
*/

// Build the Launcher directory tree in savemode=dir with folder browsing
static void InitLauncherDir( const char * directory ) {
	char fullpath[MAX_VALUE_NAME], buffer[MAX_VALUE_NAME] ;
	DIR * dir ;
	struct dirent * de ;
	FILE * fp ;
	
	if( strlen(directory)>0 ) {
		snprintf( fullpath, sizeof(fullpath), "%s\\Sessions\\%s", ConfigDirectory, directory ) ;
		snprintf( buffer, sizeof(buffer), "%s\\Launcher\\%s", ConfigDirectory, directory ) ;
	} else {
		snprintf( fullpath, sizeof(fullpath), "%s\\Sessions", ConfigDirectory ) ;
		snprintf( buffer, sizeof(buffer), "%s\\Launcher", ConfigDirectory ) ;
	}
	if( !MakeDir( buffer ) ) { 
		//MessageBox(NULL,buffer,"Error",MB_OK|MB_ICONERROR); 
		MessageBox(NULL,KT_MSG_LAUNCHER_DIR_FAILED,KT_CAP_ERROR,MB_OK|MB_ICONERROR); 
	}
	if( (dir=opendir(fullpath)) != NULL ) {
		while( (de=readdir(dir)) != NULL ) 
		if( strcmp(de->d_name,".") && strcmp(de->d_name,"..") )	{
			snprintf( fullpath, sizeof(fullpath), "%s\\Sessions\\%s\\%s", ConfigDirectory, directory, de->d_name ) ;
			if( !(GetFileAttributes( fullpath ) & FILE_ATTRIBUTE_DIRECTORY) ) {
				snprintf( buffer, sizeof(buffer), "%s\\Launcher\\%s\\%s", ConfigDirectory, directory, de->d_name ) ;
				if( (fp=fopen(buffer,"wb")) != NULL ) {
					unmungestr( de->d_name, buffer, MAX_VALUE_NAME) ;
					fprintf( fp, "%s\\%s\\", buffer, directory ) ;
					fclose( fp ) ; 
					}
				}
			else if( (GetFileAttributes( fullpath ) & FILE_ATTRIBUTE_DIRECTORY) ) {
				snprintf( buffer, sizeof(buffer), "%s\\%s", directory, de->d_name ) ;
				if( buffer[0]=='\\' ) InitLauncherDir( buffer+1 ) ;
				else InitLauncherDir( buffer ) ;
				}

			}
		}
	closedir( dir ) ;
	}

// Fill the Launcher registry key with the saved sessions
void InitLauncherRegistry( void ) {
	HKEY hKey ;
	char buffer[MAX_VALUE_NAME] ;
	int i;

	if( (IniFileFlag == SAVEMODE_REG)||(IniFileFlag == SAVEMODE_FILE) ) {
		TCHAR folder[MAX_VALUE_NAME], achClass[MAX_PATH] = TEXT("");
		DWORD   cchClassName=MAX_PATH,cSubKeys=0,cbMaxSubKey,cchMaxClass;
		DWORD	cValues,cchMaxValue,cbMaxValueData,cbSecurityDescriptor;
		FILETIME ftLastWriteTime;

		snprintf( buffer, sizeof(buffer), "%s\\Launcher", kitty_registry_base() ) ;
		RegDelTree (HKEY_CURRENT_USER, buffer ) ;
		RegTestOrCreate( HKEY_CURRENT_USER, buffer, NULL, NULL ) ;
		snprintf( buffer, sizeof(buffer), "%s\\Sessions", kitty_registry_base() ) ;
		if( RegOpenKeyEx( HKEY_CURRENT_USER, buffer, 0, KEY_READ, &hKey) != ERROR_SUCCESS ) return ;

		RegQueryInfoKey(hKey,achClass,&cchClassName,NULL,&cSubKeys,&cbMaxSubKey,&cchMaxClass,&cValues,&cchMaxValue,&cbMaxValueData,&cbSecurityDescriptor,&ftLastWriteTime);

		if( cSubKeys>0 )
			for (i=0; i<cSubKeys; i++) {
				DWORD cchValue = MAX_VALUE_NAME;
				char lpData[4096] ;
				if( RegEnumKeyEx(hKey, i, lpData, &cchValue, NULL, NULL, NULL, &ftLastWriteTime) == ERROR_SUCCESS ) {
					/* KiTTY: skip sessions marked LauncherHide (excluded from the launcher). */
					{
						DWORD hide = 0, hsz = sizeof(hide) ;
						char skey[4096] ;
						snprintf( skey, sizeof(skey), "%s\\Sessions\\%s", kitty_registry_base(), lpData ) ;
						if( RegGetValueA( HKEY_CURRENT_USER, skey, KR_LAUNCHERHIDE, RRF_RT_REG_DWORD, NULL, &hide, &hsz ) == ERROR_SUCCESS && hide )
							continue ;
					}
					snprintf( buffer, sizeof(buffer),"%s\\Sessions\\%s", kitty_registry_base(), lpData ) ;
					if( !GetValueDataN(HKEY_CURRENT_USER, buffer, KR_FOLDER, folder, sizeof(folder) ) )
						{ strcpy( folder, "Default" ) ; }
					CleanFolderName( folder ) ;
					if( !strcmp( folder, "Default" ) || (strlen(folder)<=0) )
						snprintf( buffer, sizeof(buffer), "%s\\Launcher", kitty_registry_base() ) ;
					else
						snprintf( buffer, sizeof(buffer), "%s\\Launcher\\%s", kitty_registry_base(), folder ) ;
					strcpy( folder, "" ) ;
					unmungestr( lpData, folder, MAX_VALUE_NAME ) ;
					if( strlen(folder) > 0 )
						RegTestOrCreate( HKEY_CURRENT_USER, buffer, folder, folder ) ;
				}
			}
		RegCloseKey( hKey ) ;
	} else if( (IniFileFlag == SAVEMODE_DIR)&&(DirectoryBrowseFlag==0) ) {
		char fullpath[MAX_VALUE_NAME], folder[MAX_VALUE_NAME] ;
		DIR * dir ;
		struct dirent * de ;
		FILE * fp ;
		snprintf( fullpath, sizeof(fullpath), "%s\\Launcher", ConfigDirectory ) ;
		DelDir( fullpath ) ;
		if(!MakeDir( fullpath ) ) { MessageBox(NULL,KT_MSG_LAUNCHER_DIR_FAILED,KT_CAP_ERROR,MB_OK|MB_ICONERROR); }
		snprintf( fullpath, sizeof(fullpath), "%s\\Sessions", ConfigDirectory ) ;
		if( (dir=opendir(fullpath)) != NULL ) {
			while( (de=readdir(dir)) != NULL ) 
			if( strcmp(de->d_name,".") && strcmp(de->d_name,"..") )	{
				snprintf( fullpath, sizeof(fullpath), "%s\\Sessions\\%s", ConfigDirectory, de->d_name ) ;
				if( !(GetFileAttributes( fullpath ) & FILE_ATTRIBUTE_DIRECTORY) ) {
					strcpy( folder, "" ) ;
					unmungestr( de->d_name, buffer, MAX_VALUE_NAME) ;
					GetSessionFolderName( buffer, folder ) ;
					CleanFolderName( folder ) ;
					snprintf( buffer, sizeof(buffer), "%s\\Launcher\\%s", ConfigDirectory, folder ) ;
					if( strcmp(folder,"Default") ) {
						MakeDir( buffer ) ;
						snprintf( buffer, sizeof(buffer), "%s\\Launcher\\%s\\%s", ConfigDirectory, folder, de->d_name ) ;
					} else sprintf( buffer, "%s\\Launcher\\%s", ConfigDirectory, de->d_name ) ;
					if( (fp=fopen(buffer,"wb")) != NULL ) {
						unmungestr( de->d_name, buffer, MAX_VALUE_NAME) ;
						fprintf( fp, "%s\\%s\\", buffer, buffer ) ;
						fclose( fp ) ; 
					}
				}
			}
			closedir(dir) ;
		}
	} else if( (IniFileFlag == SAVEMODE_DIR)&&DirectoryBrowseFlag ) {
		char fullpath[MAX_VALUE_NAME] ;
		snprintf( fullpath, sizeof(fullpath), "%s\\Launcher", ConfigDirectory ) ;
		DelDir( fullpath ) ;
		if( !MakeDir( fullpath ) ) { MessageBox(NULL,KT_MSG_LAUNCHER_DIR_FAILED,KT_CAP_ERROR,MB_OK|MB_ICONERROR); }
		InitLauncherDir( "" ) ;
	}
}

/* KiTTY #544: the launcher menu shows "Ctrl+Shift+<letter>" accelerators for the
 * predefined-command/session items, but a Win32 popup menu only displays that
 * text -- it never acts on it (so the keystroke just dings). While the menu is
 * open we install a WH_MSGFILTER hook: on Ctrl+Shift+<letter>, if a command is
 * defined at that index, dismiss the menu and fire its WM_COMMAND (the same path
 * a mouse click takes). Scoped to the open menu only (no global hotkey grab). */
static HHOOK g_launcher_menu_hook = NULL ;

/* The id of the menu item under a screen point, 0 when there is none (or a
 * submenu item, a separator): the popup window under the point names its
 * menu, the menu names the item. */
static UINT LauncherMenuIdAt( POINT pt ) {
	HWND under = WindowFromPoint( pt ) ;
	char cls[16] = "" ;
	HMENU hm ;
	int pos ;
	UINT id ;
	if( !under || !GetClassNameA( under, cls, sizeof(cls) ) || strcmp( cls, "#32768" ) )
		return 0 ;
	hm = (HMENU)SendMessage( under, 0x01E1 /* MN_GETHMENU */, 0, 0 ) ;
	pos = hm ? MenuItemFromPoint( NULL, hm, pt ) : -1 ;
	if( pos < 0 ) return 0 ;
	id = GetMenuItemID( hm, pos ) ;
	return ( id == (UINT)-1 ) ? 0 : id ;
}

static LRESULT CALLBACK LauncherMenuMsgFilter( int code, WPARAM wParam, LPARAM lParam )
{
	if( code == MSGF_MENU ) {
		MSG *m = (MSG *)lParam ;
		/*
		 * Refresh. A menu closes the moment one of its items is chosen, and
		 * the refresh used to run AFTER that: no menu on the screen for as long
		 * as the rebuild took, then the menu again - a flicker. So the choice
		 * never reaches the menu loop: the button (down, up and the double
		 * click, all three, or the loop would be left believing a button is
		 * held) and Enter on the highlighted item are taken here, the menu
		 * stays where it is, and the launcher is told to refresh behind it.
		 * While that runs every click is taken too: the session ids of the
		 * menu still on the screen belong to the list being replaced.
		 */
		if( m && ( m->message == WM_LBUTTONDOWN || m->message == WM_LBUTTONUP ||
		           m->message == WM_LBUTTONDBLCLK || m->message == WM_NCLBUTTONDOWN ||
		           m->message == WM_NCLBUTTONUP || m->message == WM_NCLBUTTONDBLCLK ) ) {
			int down = ( m->message == WM_LBUTTONDOWN || m->message == WM_NCLBUTTONDOWN ) ;
			if( LauncherRefreshBusy )
				return 1 ;
			if( LauncherMenuIdAt( m->pt ) == IDM_LAUNCHER+7 ) {
				if( down ) PostMessage( MainHwnd, KLWM_REFRESHINPLACE, 0, 0 ) ;
				return 1 ;
			}
		}
		if( m && m->message == WM_KEYDOWN && m->wParam == VK_RETURN && MenuLauncher != NULL ) {
			int i, n = GetMenuItemCount( MenuLauncher ) ;
			for( i = 0 ; i < n ; i++ )
				if( GetMenuItemID( MenuLauncher, i ) == IDM_LAUNCHER+7 &&
				    ( GetMenuState( MenuLauncher, i, MF_BYPOSITION ) & MF_HILITE ) ) {
					if( !LauncherRefreshBusy ) PostMessage( MainHwnd, KLWM_REFRESHINPLACE, 0, 0 ) ;
					return 1 ;
				}
		}
		if( m && m->message == WM_KEYDOWN ) {
			int vk = (int)m->wParam ;
			if( vk >= 'A' && vk <= 'Z'
			    && (GetKeyState(VK_CONTROL) & 0x8000)
			    && (GetKeyState(VK_SHIFT) & 0x8000) ) {
				int nb = vk - 'A' ;
				if( nb >= 0 && nb < NB_MENU_MAX && SpecialMenu[nb] != NULL ) {
					EndMenu() ;   /* close the popup */
					PostMessage( MainHwnd, WM_COMMAND, IDM_USERCMD + nb, 0 ) ;
					return 1 ;    /* consume -> no ding */
				}
			}
		}
		/*
		 * A right click on a session FOLDER. The menu loop sends its owner
		 * WM_MENURBUTTONUP for a plain item and NOTHING for an item that opens
		 * a submenu (measured: the message arrives for a session entry and
		 * never for a folder) - and a folder is the one place the folder
		 * context menu is for. So the raw button release is looked at here:
		 * the popup window under the cursor names its menu, the menu names the
		 * item under the point, and when that item has a submenu the launcher
		 * is handed the same message the menu loop would have sent. Posted,
		 * not sent: the context menu is then raised from the launcher's own
		 * message handling, not from inside this filter. A plain item is left
		 * to the menu loop, which reports it itself.
		 */
		else if( m && ( m->message == WM_RBUTTONUP || m->message == WM_NCRBUTTONUP ) ) {
			HWND under = WindowFromPoint( m->pt ) ;
			char cls[16] = "" ;
			if( under && GetClassNameA( under, cls, sizeof(cls) ) && !strcmp( cls, "#32768" ) ) {
				HMENU hm = (HMENU)SendMessage( under, 0x01E1 /* MN_GETHMENU */, 0, 0 ) ;
				int pos = hm ? MenuItemFromPoint( NULL, hm, m->pt ) : -1 ;
				if( pos >= 0 && GetSubMenu( hm, pos ) != NULL )
					PostMessage( MainHwnd, WM_MENURBUTTONUP, (WPARAM)pos, (LPARAM)hm ) ;
			}
		}
	}
	return CallNextHookEx( g_launcher_menu_hook, code, wParam, lParam ) ;
}

/*
 * KiTTY: a right click on a session FOLDER in the open tray menu offers to
 * open every session directly in that folder (not the ones in its subfolders:
 * a folder of folders would otherwise start the whole store with one click).
 *
 * The sessions are COPIED out of the menu when the entry is chosen: every
 * RunSession() is followed by a menu rebuild, which replaces the SpecialMenu[]
 * payloads the menu items point at. The start itself happens after the menu
 * loop has ended (a posted message), so the confirmation box is not raised
 * under an open menu, and one session per timer tick, so a large folder does
 * not start all its processes in the same instant and the launcher's message
 * loop stays alive in between.
 */
#define KLWM_OPENFOLDER			(WM_USER+17)
#define LAUNCHER_OPENFOLDER_TIMER	103
#define LAUNCHER_OPENFOLDER_GAP_MS	300	/* between two session starts */
#define LAUNCHER_OPENFOLDER_ASK_ABOVE	8	/* more sessions than this: ask first */

struct LauncherFolderItem { char payload[1024] ; char label[1024] ; } ;
static struct LauncherFolderItem * LauncherFolderItems = NULL ;
static int LauncherFolderCount = 0 ;
static int LauncherFolderNext = 0 ;
static char LauncherFolderName[256] = "" ;

/* The sessions directly in a folder's submenu: the items whose id is a
 * session's. 0 for a submenu that is not a session folder (Opened sessions,
 * the workplace proxies) - their items carry other ids. */
static int LauncherFolderSessions( HMENU sub, struct LauncherFolderItem * out, int max ) {
	int i, n = 0, count = GetMenuItemCount( sub ) ;
	for( i = 0 ; i < count ; i++ ) {
		UINT id = GetMenuItemID( sub, i ) ;   /* (UINT)-1 for a submenu */
		int nb = (int)id - IDM_USERCMD ;
		if( id == (UINT)-1 || nb < 0 || nb >= NB_MENU_MAX || SpecialMenu[nb] == NULL )
			continue ;
		if( out != NULL && n < max ) {
			char * tab ;
			snprintf( out[n].payload, sizeof(out[n].payload), "%s", SpecialMenu[nb] ) ;
			out[n].label[0] = '\0' ;
			GetMenuString( sub, i, out[n].label, sizeof(out[n].label), MF_BYPOSITION ) ;
			/* the "\tCtrl+Shift+A" accelerator text is not part of the name */
			if( (tab = strchr( out[n].label, '\t' )) != NULL ) *tab = '\0' ;
		}
		n++ ;
	}
	return n ;
}

/* WM_MENURBUTTONUP: item `pos` of `parent` was right-clicked. */
static void LauncherFolderContextMenu( HWND hwnd, HMENU parent, int pos ) {
	HMENU sub = GetSubMenu( parent, pos ), ctx ;
	char name[256] = "", item[512] ;
	POINT pt ;
	int n ;

#ifdef KITTY_TEST_BUILD_LABEL
	{	/* test builds, KITTY_LAUNCHER_TIMING set: did the message arrive, and with what */
		char dbg[160] ;
		snprintf( dbg, sizeof(dbg), "rclick pos=%d parent=%p sub=%p hidemenu=%d sessions=%d",
		          pos, (void *)parent, (void *)sub, sub == HideMenu,
		          sub ? LauncherFolderSessions( sub, NULL, 0 ) : -1 ) ;
		LAUNCHER_DEBUG_LINE( dbg ) ;
	}
#endif
	if( sub == NULL || sub == HideMenu )
		return ;
	n = LauncherFolderSessions( sub, NULL, 0 ) ;
	if( n < 2 )
		return ;                 /* one session is what a left click is for */
	GetMenuString( parent, pos, name, sizeof(name), MF_BYPOSITION ) ;
	snprintf( item, sizeof(item), KT_MENU_OPEN_FOLDER_ALL, n, name ) ;
	if( (ctx = CreatePopupMenu()) == NULL )
		return ;
	AppendMenu( ctx, MF_ENABLED, 1, item ) ;
	GetCursorPos( &pt ) ;
	/* TPM_RECURSE: this popup is raised while the tray menu is still open. */
	if( TrackPopupMenuEx( ctx, TPM_RECURSE | TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN,
	                      pt.x, pt.y, hwnd, NULL ) == 1 ) {
		free( LauncherFolderItems ) ;
		LauncherFolderItems = (struct LauncherFolderItem *)calloc( n, sizeof(struct LauncherFolderItem) ) ;
		LauncherFolderCount = LauncherFolderItems ? LauncherFolderSessions( sub, LauncherFolderItems, n ) : 0 ;
		LauncherFolderNext = 0 ;
		snprintf( LauncherFolderName, sizeof(LauncherFolderName), "%s", name ) ;
		EndMenu() ;              /* the tray menu: its job is done */
		if( LauncherFolderCount > 0 )
			PostMessage( hwnd, KLWM_OPENFOLDER, 0, 0 ) ;
	}
	DestroyMenu( ctx ) ;
}

/* One session of the pending folder; the timer brings the next. */
static void LauncherFolderOpenNext( HWND hwnd ) {
	if( LauncherFolderItems != NULL && LauncherFolderNext < LauncherFolderCount ) {
		struct LauncherFolderItem * it = &LauncherFolderItems[LauncherFolderNext++] ;
		/* The same two forms the menu's own click uses. */
		if( DirectoryBrowseFlag ) RunSession( hwnd, it->payload, it->label ) ;
		else RunSession( hwnd, it->payload, it->payload ) ;
	}
	if( LauncherFolderItems == NULL || LauncherFolderNext >= LauncherFolderCount ) {
		KillTimer( hwnd, LAUNCHER_OPENFOLDER_TIMER ) ;
		free( LauncherFolderItems ) ;
		LauncherFolderItems = NULL ;
		LauncherFolderCount = LauncherFolderNext = 0 ;
	}
}

/*
 * KiTTY: Refresh, chosen in the OPEN tray menu (the menu filter above took the
 * click, so the menu is still up).
 *
 * The next menu is built completely BEHIND the open one, which shows a spinner
 * beside "Refresh" meanwhile - for at least LAUNCHER_REFRESHSPIN_MIN_MS, so a
 * refresh that takes twenty milliseconds is still seen to have happened. Then
 * the open menu is ended and DisplayContextMenuAt, which is waiting in
 * TrackPopupMenu further up the stack, shows the finished one at the same
 * point without the opening animation. An open menu cannot take new items or
 * change its size, so one swap is unavoidable; what made it a flicker was the
 * time between the two menus, and there is none left.
 *
 * The spinner is plain text ("-\|/") in the item's right-hand column: no glyph
 * font to be missing on an old Windows, and it takes the menu's own ink in
 * either theme.
 */
static void LauncherRegisterHotkeys( HWND hwnd, int notify ) ;
static DWORD LauncherRefreshTick = 0 ;
static int LauncherRefreshSpin = 0 ;
static int LauncherRefreshSwap = 0 ;     /* the refresh ended the menu, not the user */

/* The open tray menu's own window: the popup of this thread that shows `menu`. */
static BOOL CALLBACK LauncherMenuWindowProc( HWND w, LPARAM lParam ) {
	HWND * found = (HWND *)lParam ;
	char cls[16] = "" ;
	if( GetClassNameA( w, cls, sizeof(cls) ) && !strcmp( cls, "#32768" ) &&
	    (HMENU)SendMessage( w, 0x01E1 /* MN_GETHMENU */, 0, 0 ) == MenuLauncher ) {
		*found = w ;
		return FALSE ;
	}
	return TRUE ;
}

/* Write `c` (or a blank) beside "Refresh" and have that one item repainted. */
static void LauncherRefreshSpinPaint( char c ) {
	MENUITEMINFOA mii ;
	char text[64] ;
	HWND popup = NULL ;
	RECT rc ;
	int i, n, pos = -1 ;

	if( MenuLauncher == NULL ) return ;
	n = GetMenuItemCount( MenuLauncher ) ;
	for( i = 0 ; i < n ; i++ )
		if( GetMenuItemID( MenuLauncher, i ) == IDM_LAUNCHER+7 ) { pos = i ; break ; }
	if( pos < 0 ) return ;
	snprintf( text, sizeof(text), "%s\t %c ", KT_MENU_REFRESH, c ) ;
	memset( &mii, 0, sizeof(mii) ) ;
	mii.cbSize = sizeof(mii) ;
	mii.fMask = MIIM_STRING ;
	mii.dwTypeData = text ;
	SetMenuItemInfoA( MenuLauncher, pos, TRUE, &mii ) ;
	EnumThreadWindows( GetCurrentThreadId(), LauncherMenuWindowProc, (LPARAM)&popup ) ;
	if( popup == NULL ) return ;
	if( GetMenuItemRect( NULL, MenuLauncher, pos, &rc ) ) {     /* screen coordinates */
		MapWindowPoints( NULL, popup, (LPPOINT)&rc, 2 ) ;
		RedrawWindow( popup, &rc, NULL, RDW_INVALIDATE | RDW_UPDATENOW ) ;
	} else
		RedrawWindow( popup, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW ) ;
}

static void LauncherRefreshInPlace( HWND hwnd ) {
	if( LauncherRefreshBusy ) return ;
	LauncherRefreshBusy = 1 ;
	LauncherRefreshTick = GetTickCount() ;
	LauncherRefreshSpin = 0 ;
	LauncherRefreshSpinPaint( '-' ) ;          /* seen before the work starts */
	if( LauncherConfReload ) InitLauncherRegistry() ;
	/* NOT RefreshMenuLauncher(): that destroys the menu that is on the screen. */
	LauncherPendingMenu = InitLauncherMenu( "Launcher" ) ;
	/* An explicit request for the current state, so a changed hotkey-conflict
	 * report balloons here, as it does for the Refresh of a closed menu. */
	LauncherRegisterHotkeys( hwnd, 1 ) ;
	SetTimer( hwnd, LAUNCHER_REFRESHSPIN_TIMER, LAUNCHER_REFRESHSPIN_STEP_MS, NULL ) ;
}

/*
 * The swap of the two menus, without the desktop showing through in between.
 * One menu has to close before the next can open at the same point, and for
 * that instant there is no menu: the background flashes through, which reads
 * as a flicker however short it is. So a PICTURE of the open menu is put up
 * first, in a plain topmost window on exactly the menu's rectangle: the old
 * menu closes behind it, the new one opens over it (a menu is topmost too, and
 * the later topmost window is in front), and the picture goes a moment later.
 * Never activated - an activation would end the menu mode it is there to cover.
 */
#define LAUNCHER_REFRESHCOVER_TIMER	105
#define LAUNCHER_REFRESHCOVER_MS	200
static HWND LauncherCoverWnd = NULL ;
static HBITMAP LauncherCoverBmp = NULL ;

static void LauncherCoverRemove( HWND hwnd ) {
	KillTimer( hwnd, LAUNCHER_REFRESHCOVER_TIMER ) ;
	if( LauncherCoverWnd ) { DestroyWindow( LauncherCoverWnd ) ; LauncherCoverWnd = NULL ; }
	if( LauncherCoverBmp ) { DeleteObject( LauncherCoverBmp ) ; LauncherCoverBmp = NULL ; }
}

static void LauncherCoverShow( HWND hwnd ) {
	HWND popup = NULL ;
	RECT rc ;
	HDC screen, mem ;
	HGDIOBJ old ;
	int w, h ;

	LauncherCoverRemove( hwnd ) ;
	EnumThreadWindows( GetCurrentThreadId(), LauncherMenuWindowProc, (LPARAM)&popup ) ;
	if( popup == NULL || !GetWindowRect( popup, &rc ) ) return ;
	w = rc.right - rc.left ; h = rc.bottom - rc.top ;
	if( w <= 0 || h <= 0 ) return ;
	if( (screen = GetDC( NULL )) == NULL ) return ;
	mem = CreateCompatibleDC( screen ) ;
	LauncherCoverBmp = CreateCompatibleBitmap( screen, w, h ) ;
	if( mem && LauncherCoverBmp ) {
		old = SelectObject( mem, LauncherCoverBmp ) ;
		BitBlt( mem, 0, 0, w, h, screen, rc.left, rc.top, SRCCOPY ) ;
		SelectObject( mem, old ) ;
	}
	if( mem ) DeleteDC( mem ) ;
	ReleaseDC( NULL, screen ) ;
	if( LauncherCoverBmp == NULL ) return ;
	LauncherCoverWnd = CreateWindowExA( WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
		"STATIC", "", WS_POPUP | SS_BITMAP, rc.left, rc.top, w, h,
		NULL, NULL, GetModuleHandle( NULL ), NULL ) ;
	if( LauncherCoverWnd == NULL ) { LauncherCoverRemove( hwnd ) ; return ; }
	SendMessage( LauncherCoverWnd, STM_SETIMAGE, IMAGE_BITMAP, (LPARAM)LauncherCoverBmp ) ;
	SetWindowPos( LauncherCoverWnd, HWND_TOPMOST, rc.left, rc.top, w, h,
	              SWP_NOACTIVATE | SWP_SHOWWINDOW ) ;
	UpdateWindow( LauncherCoverWnd ) ;
	SetTimer( hwnd, LAUNCHER_REFRESHCOVER_TIMER, LAUNCHER_REFRESHCOVER_MS, NULL ) ;
}

/*
 * The refreshed menu, put INTO the menu that is on the screen - no window
 * closes, so nothing can flicker. Possible whenever the top level reads the
 * same as before, which is the usual refresh: an open menu cannot take new
 * items or change its size, but an item's submenu and its command id are not
 * part of what is painted. A folder's submenu is closed at that moment (Refresh
 * is a top-level item), so the refreshed one is hung in its place and lays
 * itself out afresh when it is next opened; a session's id is replaced because
 * the ids index the list that was just rebuilt.
 */
static BOOL CALLBACK LauncherMenuCountProc( HWND w, LPARAM lParam ) {
	char cls[16] = "" ;
	if( IsWindowVisible( w ) && GetClassNameA( w, cls, sizeof(cls) ) && !strcmp( cls, "#32768" ) )
		(*(int *)lParam)++ ;
	return TRUE ;
}

/* Same top level? Item for item: the same text, and a submenu on both or on neither. */
static int LauncherMenuSameTopLevel( HMENU a, HMENU b ) {
	int i, n = GetMenuItemCount( a ) ;
	if( n < 0 || n != GetMenuItemCount( b ) ) return 0 ;
	for( i = 0 ; i < n ; i++ ) {
		char ta[1024] = "", tb[1024] = "" ;
		GetMenuStringA( a, i, ta, sizeof(ta), MF_BYPOSITION ) ;
		GetMenuStringA( b, i, tb, sizeof(tb), MF_BYPOSITION ) ;
		if( strcmp( ta, tb ) ) return 0 ;
		if( ( GetSubMenu( a, i ) != NULL ) != ( GetSubMenu( b, i ) != NULL ) ) return 0 ;
	}
	return 1 ;
}

/* Move every submenu, id and tick of `fresh` into `shown`, then drop `fresh`. */
static void LauncherMenuGraft( HMENU shown, HMENU fresh ) {
	int i ;
	for( i = GetMenuItemCount( fresh ) - 1 ; i >= 0 ; i-- ) {
		MENUITEMINFOA mii ;
		HMENU subnew = GetSubMenu( fresh, i ) ;
		memset( &mii, 0, sizeof(mii) ) ;
		mii.cbSize = sizeof(mii) ;
		if( subnew != NULL ) {
			HMENU subold = GetSubMenu( shown, i ) ;
			RemoveMenu( fresh, i, MF_BYPOSITION ) ;     /* detached, not destroyed */
			mii.fMask = MIIM_SUBMENU ;
			mii.hSubMenu = subnew ;
			SetMenuItemInfoA( shown, i, TRUE, &mii ) ;
			if( subold != NULL && subold != subnew && IsMenu( subold ) )
				DestroyMenu( subold ) ;
		} else {
			UINT id = GetMenuItemID( fresh, i ) ;
			UINT st = GetMenuState( fresh, i, MF_BYPOSITION ) ;
			if( id == (UINT)-1 || id == 0 || st == (UINT)-1 ) continue ;   /* a separator */
			mii.fMask = MIIM_ID | MIIM_STATE ;
			mii.wID = id ;
			mii.fState = st & ( MFS_CHECKED | MFS_GRAYED | MFS_DISABLED ) ;
			SetMenuItemInfoA( shown, i, TRUE, &mii ) ;
		}
	}
	DestroyMenu( fresh ) ;
}

/* LAUNCHER_REFRESHSPIN_TIMER: the next spinner frame, or the end. */
static void LauncherRefreshSpinStep( HWND hwnd ) {
	static const char frames[] = "-\\|/" ;
	if( !LauncherRefreshBusy ) { KillTimer( hwnd, LAUNCHER_REFRESHSPIN_TIMER ) ; return ; }
	if( GetTickCount() - LauncherRefreshTick < LAUNCHER_REFRESHSPIN_MIN_MS ) {
		LauncherRefreshSpin = ( LauncherRefreshSpin + 1 ) % 4 ;
		LauncherRefreshSpinPaint( frames[LauncherRefreshSpin] ) ;
		return ;
	}
	KillTimer( hwnd, LAUNCHER_REFRESHSPIN_TIMER ) ;
	LauncherRefreshSpinPaint( ' ' ) ;          /* before the texts are compared */
	/*
	 * The usual refresh: the top level reads as before. The menu on the screen
	 * STAYS, and takes the refreshed submenus and ids. Only with exactly one
	 * menu window up - an open submenu cannot have its menu replaced under it.
	 */
	if( g_launcher_menu_hook != NULL && LauncherPendingMenu != NULL && MenuLauncher != NULL ) {
		int popups = 0 ;
		EnumThreadWindows( GetCurrentThreadId(), LauncherMenuCountProc, (LPARAM)&popups ) ;
		if( popups == 1 && LauncherMenuSameTopLevel( MenuLauncher, LauncherPendingMenu ) ) {
			HWND popup = NULL ;
			LauncherMenuGraft( MenuLauncher, LauncherPendingMenu ) ;
			LauncherPendingMenu = NULL ;
			LauncherRefreshBusy = 0 ;
			/* a tick may have changed (Start at login): repaint the menu once */
			EnumThreadWindows( GetCurrentThreadId(), LauncherMenuWindowProc, (LPARAM)&popup ) ;
			if( popup ) RedrawWindow( popup, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW ) ;
			return ;
		}
	}
	/* The top level changed: an open menu cannot show that, so the two menus
	 * are swapped. End the open menu. DisplayContextMenuAt finds LauncherPendingMenu when
	 * its TrackPopupMenu returns and shows it; without an open menu (it was
	 * dismissed meanwhile) the finished menu simply becomes the current one. */
	LauncherRefreshSwap = 1 ;
	if( g_launcher_menu_hook != NULL ) {
		LauncherRefreshSpinPaint( ' ' ) ;      /* the picture shows a plain "Refresh" */
		LauncherCoverShow( hwnd ) ;
	}
	EndMenu() ;
	if( g_launcher_menu_hook == NULL && LauncherPendingMenu != NULL ) {
		LauncherRefreshSwap = 0 ;
		DestroyMenu( MenuLauncher ) ;
		MenuLauncher = LauncherPendingMenu ;
		LauncherPendingMenu = NULL ;
		LauncherRefreshBusy = 0 ;
	}
}

static void DisplayContextMenuAt( HWND hwnd, HMENU menu, POINT pt ) {
	HMENU hMenuPopup = menu ;
	UINT flags = TPM_LEFTALIGN ;

	/* The colour theme is read when the process starts, and the launcher runs
	 * for days: a change made in the configuration box reached it only after
	 * a restart. Asked again before every menu - the preference is cached for
	 * two seconds and the call does nothing while the mode is unchanged. */
	kitty_theme_app_mode( kitty_theme_app_pref() ) ;

	SetForegroundWindow( hwnd ) ;
	g_launcher_menu_hook = SetWindowsHookEx( WH_MSGFILTER, LauncherMenuMsgFilter,
	                                         NULL, GetCurrentThreadId() ) ;
	for( ;; ) {
		TrackPopupMenu (hMenuPopup, flags, pt.x, pt.y, 0, hwnd, NULL);
		/* A Refresh ended this menu to have the rebuilt one shown in its
		 * place: same point, and no opening animation, so the swap is one
		 * redraw and not a menu fading in a second time. */
		if( LauncherPendingMenu == NULL )
			break ;
		{
			/* Ended by the refresh, or dismissed by the user while it ran?
			 * The finished menu becomes the current one either way; it is
			 * SHOWN only when the refresh ended the old one. */
			int show = LauncherRefreshSwap && ( hMenuPopup == MenuLauncher ) ;
			KillTimer( hwnd, LAUNCHER_REFRESHSPIN_TIMER ) ;
			DestroyMenu( MenuLauncher ) ;
			MenuLauncher = LauncherPendingMenu ;
			LauncherPendingMenu = NULL ;
			LauncherRefreshBusy = 0 ;
			LauncherRefreshSwap = 0 ;
			if( !show )
				break ;
			hMenuPopup = MenuLauncher ;
			flags |= TPM_NOANIMATION ;
		}
	}
	if( g_launcher_menu_hook ) {
		UnhookWindowsHookEx( g_launcher_menu_hook ) ;
		g_launcher_menu_hook = NULL ;
	}
}

static void DisplayContextMenu( HWND hwnd, HMENU menu ) {
	GetCursorPos (&LauncherMenuPoint);
	LauncherMenuPointValid = 1 ;
	DisplayContextMenuAt( hwnd, menu, LauncherMenuPoint ) ;
}
	
// Hide/UnHide all handling
static int CurrentVisibleWin = -1 ; /* -1 = all visible */

void ManageHideOne( HWND hwnd ) { PostMessage( hwnd, WM_COMMAND, IDM_HIDE, 0 ) ; }
void ManageUnHideOne( HWND hwnd ) { PostMessage( hwnd, WM_COMMAND, IDM_UNHIDE, 0 ) ; }

static BOOL CALLBACK RefreshWinListProc( HWND hwnd, LPARAM lParam ) {
	char buffer[256] ;
	GetClassName( hwnd, buffer, 256 ) ;
	
	if( !strcmp( buffer, KiTTYClassName ) )
	if( hwnd != MainHwnd ) {
		TabWin[NbWin].hwnd=hwnd ;
		GetWindowText( hwnd, TabWin[NbWin].name, 127 ) ;
		NbWin++ ;
	}

	return TRUE ;
}

int RefreshWinList( HWND hwnd ) {
	NbWin=0 ;
	EnumWindows( RefreshWinListProc, 0 ) ;
	return NbWin ;
}
	
static void ManageHideAll( HWND hwnd ) {
	int i ;
	if( RefreshWinList( hwnd ) > 0 ) {
		for( i=0 ; i<NbWin ; i++ ) {
			ManageHideOne( TabWin[i].hwnd ) ;
		}
	}
	CurrentVisibleWin = 0 ;
}

static void ManageUnHideAll( HWND hwnd ) {
	int i ;
	if( RefreshWinList( hwnd ) > 0 ) {
		for( i=0 ; i<NbWin ; i++ ) ManageUnHideOne( TabWin[i].hwnd ) ;
	}
	CurrentVisibleWin = -1 ;
}
	
static void ManageGoNext( HWND hwnd ) {
	if( CurrentVisibleWin == -1 ) return ;
	ManageHideOne( TabWin[CurrentVisibleWin].hwnd ) ;
	CurrentVisibleWin++ ;
	if( CurrentVisibleWin>=NbWin ) CurrentVisibleWin=0 ;
	ManageUnHideOne( TabWin[CurrentVisibleWin].hwnd ) ;
}

static void ManageGoPrevious( HWND hwnd ) {
	if( CurrentVisibleWin == -1 ) return ;
	ManageHideOne( TabWin[CurrentVisibleWin].hwnd ) ;
	CurrentVisibleWin-- ;
	if( CurrentVisibleWin<0 ) CurrentVisibleWin=NbWin-1 ;
	ManageUnHideOne( TabWin[CurrentVisibleWin].hwnd ) ;
}
	
void ManageSwitch( const int n ) { 
	SendMessage( TabWin[n].hwnd, WM_COMMAND, IDM_SWITCH_HIDE, 0 ) ; 
	SetForegroundWindow( TabWin[n].hwnd ) ;
	SetFocus( TabWin[n].hwnd ) ;
}

static void ShowLauncherUpdateBalloon( void ) {
	char ulatest[64]="" ; int ubeta=0 ;
	if( kitty_update_available( ulatest, sizeof(ulatest), NULL, 0, &ubeta ) ) {
		char umsg[256] ;
		LauncherUpdateKnown = 1 ;
		LauncherUpdateBeta = ubeta ;
		strncpy( LauncherUpdateLatest, ulatest, sizeof(LauncherUpdateLatest)-1 ) ;
		LauncherUpdateLatest[sizeof(LauncherUpdateLatest)-1] = '\0' ;
		snprintf( umsg, sizeof(umsg),
			KT_LAUNCHER_UPDATE_BALLOON,
			ulatest, ubeta ? KT_UPD_BETA_SUFFIX : "" ) ;
		/* Tooltip and menu entry are refreshed on every call; the BALLOON is
		 * raised once. This function runs twice per launcher run - once on the
		 * cached answer, once when the async check returns - and used to pop a
		 * second balloon for the same news. */
		if( LauncherUpdateBalloonShown ) {
			snprintf( TrayIcone.szTip, sizeof(TrayIcone.szTip),
			          KT_LAUNCHER_TIP_UPDATE,
			          ulatest, ubeta ? KT_UPD_BETA_WORD : "" ) ;
			TrayIcone.uFlags = NIF_TIP ;
			Shell_NotifyIcon( NIM_MODIFY, &TrayIcone ) ;
			TrayIcone.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE ;
			return ;
		}
		LauncherUpdateBalloonShown = 1 ;
		/* This balloon replaces whatever balloon was showing, so a click from
		 * now on means the update - not a hotkey-conflict notice. */
		LauncherHotkeyBalloonArmed = 0 ;
		TrayIcone.uFlags = NIF_INFO | NIF_TIP ;
		TrayIcone.dwInfoFlags = NIIF_INFO ;
		TrayIcone.uTimeout = 10000 ;
		snprintf( TrayIcone.szTip, sizeof(TrayIcone.szTip),
		          KT_LAUNCHER_TIP_UPDATE,
		          ulatest, ubeta ? KT_UPD_BETA_WORD : "" ) ;
		strncpy( TrayIcone.szInfoTitle, KT_CAP_UPDATE_AVAILABLE, sizeof(TrayIcone.szInfoTitle) ) ;
		TrayIcone.szInfoTitle[sizeof(TrayIcone.szInfoTitle)-1] = '\0' ;
		strncpy( TrayIcone.szInfo, umsg, sizeof(TrayIcone.szInfo) ) ;
		TrayIcone.szInfo[sizeof(TrayIcone.szInfo)-1] = '\0' ;
		Shell_NotifyIcon( NIM_MODIFY, &TrayIcone ) ;
		TrayIcone.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE ;
	}
}

static void LauncherUnregisterHotkeys( HWND hwnd ) {
	int i ;
	for( i=0 ; i<LauncherHotkeyCount ; i++ )
		UnregisterHotKey( hwnd, LauncherHotkeys[i].id ) ;
	LauncherHotkeyCount = 0 ;
}

/* Register every enabled session hotkey (parser shared with the config box:
 * kitty_parse_hotkey_spec, kitty_bridge.c). A combination two sessions claim
 * goes to the FIRST one in enumeration order; that used to happen in silence,
 * now every session that did NOT get its key - duplicate, slot limit, or the
 * key being held by another application - lands in a report ballooned from the
 * tray icon. The report is compared against the previous run's, so a refresh
 * of an unchanged store stays quiet.
 *
 * `notify` says whether a NEW report may balloon: launcher startup and the
 * tray menu's manual Refresh do, the config box's save/import broadcast does
 * NOT - the box already put the same fact in front of the user in a
 * MessageBox, and a balloon seconds later would say it twice. A silent run
 * leaves the remembered report alone, so the next notifying run still sees
 * the change and balloons it. */
static void LauncherRegisterHotkeys( HWND hwnd, int notify ) {
	int i, j ; UINT mods, vk ;
	char report[256] = "" ;
	char winner[256] = "" ;
	int overflow = 0 ;
	LauncherUnregisterHotkeys( hwnd ) ;
	for( i=0 ; i<NB_MENU_MAX ; i++ ) {
		/* The session's two hotkey settings, read alone. This loop used to
		 * load every session's WHOLE configuration to look at those two - at
		 * 200 sessions in a portable store that took about twelve seconds,
		 * at every start and every refresh of the store, and the launcher
		 * answered no tray click while it ran. */
		char spec[256] ;
		if( SpecialMenu[i] == NULL || SpecialMenu[i][0] == '\0' ) continue ;
		if( kitty_hotkey_of_session( SpecialMenu[i], &mods, &vk, spec, sizeof(spec) ) ) {
			int dup = -1 ;
			for( j=0 ; j<LauncherHotkeyCount ; j++ )
				if( LauncherHotkeys[j].modifiers == (mods|MOD_NOREPEAT) && LauncherHotkeys[j].vk == vk ) { dup = j ; break ; }
			if( dup >= 0 ) {
				char line[240] ;
				snprintf( line, sizeof(line), KT_LAUNCHER_HOTKEY_DUP,
				          report[0] ? "\n" : "",
				          spec,
				          LauncherHotkeys[dup].session, SpecialMenu[i] ) ;
				strncat( report, line, sizeof(report)-strlen(report)-1 ) ;
				if( winner[0] == '\0' ) {
					strncpy( winner, LauncherHotkeys[dup].session, sizeof(winner)-1 ) ;
					winner[sizeof(winner)-1] = '\0' ;
				}
			} else if( LauncherHotkeyCount >= LAUNCHER_HOTKEY_MAX ) {
				overflow++ ;
			} else {
				LauncherHotkeys[LauncherHotkeyCount].id = LAUNCHER_HOTKEY_BASE + LauncherHotkeyCount ;
				LauncherHotkeys[LauncherHotkeyCount].modifiers = mods | MOD_NOREPEAT ;
				LauncherHotkeys[LauncherHotkeyCount].vk = vk ;
				strncpy( LauncherHotkeys[LauncherHotkeyCount].folder, SpecialMenu[i], sizeof(LauncherHotkeys[LauncherHotkeyCount].folder)-1 ) ;
				LauncherHotkeys[LauncherHotkeyCount].folder[sizeof(LauncherHotkeys[LauncherHotkeyCount].folder)-1] = '\0' ;
				strncpy( LauncherHotkeys[LauncherHotkeyCount].session, SpecialMenu[i], sizeof(LauncherHotkeys[LauncherHotkeyCount].session)-1 ) ;
				LauncherHotkeys[LauncherHotkeyCount].session[sizeof(LauncherHotkeys[LauncherHotkeyCount].session)-1] = '\0' ;
				if( RegisterHotKey( hwnd, LauncherHotkeys[LauncherHotkeyCount].id,
					LauncherHotkeys[LauncherHotkeyCount].modifiers, LauncherHotkeys[LauncherHotkeyCount].vk ) )
					LauncherHotkeyCount++ ;
				else {
					char line[240] ;
					snprintf( line, sizeof(line), KT_LAUNCHER_HOTKEY_HELD,
					          report[0] ? "\n" : "",
					          spec,
					          SpecialMenu[i] ) ;
					strncat( report, line, sizeof(report)-strlen(report)-1 ) ;
				}
			}
		}
	}
	if( overflow ) {
		char line[120] ;
		snprintf( line, sizeof(line), KT_LAUNCHER_HOTKEY_OVERFLOW,
		          report[0] ? "\n" : "", overflow, overflow==1 ? "" : "s", LAUNCHER_HOTKEY_MAX ) ;
		strncat( report, line, sizeof(report)-strlen(report)-1 ) ;
	}
	if( report[0] ) {
		if( notify && strcmp( report, LauncherHotkeyReport ) != 0 ) {
			strncpy( LauncherHotkeyReport, report, sizeof(LauncherHotkeyReport)-1 ) ;
			LauncherHotkeyReport[sizeof(LauncherHotkeyReport)-1] = '\0' ;
			strncpy( LauncherHotkeyWinner, winner, sizeof(LauncherHotkeyWinner)-1 ) ;
			LauncherHotkeyWinner[sizeof(LauncherHotkeyWinner)-1] = '\0' ;
			PostMessage( hwnd, KLWM_HOTKEYBALLOON, 0, 0 ) ;
		}
	} else {
		/* Conflict-free run: forget the old report so the SAME conflict
		 * re-created later balloons again, and disarm the balloon click. */
		LauncherHotkeyReport[0] = '\0' ;
		LauncherHotkeyWinner[0] = '\0' ;
		LauncherHotkeyBalloonArmed = 0 ;
	}
}

static void LauncherRefreshSessionsAndHotkeys( HWND hwnd ) {
	if( LauncherConfReload ) {
		LAUNCHER_TIMING_START ;
		InitLauncherRegistry() ;
		LAUNCHER_TIMING_END( "store" ) ;
	}
	RefreshMenuLauncher() ;
	/* Broadcast from the config box after a save or import: whoever caused
	 * the change was warned in place, so re-register WITHOUT ballooning. */
	{
		LAUNCHER_TIMING_START ;
		LauncherRegisterHotkeys( hwnd, 0 ) ;
		LAUNCHER_TIMING_END( "hotkeys" ) ;
	}
}
	
/* KiTTY: the tray tooltip, built in one place because it now has a part that
 * changes at runtime - workplace proxy mode names the proxy every connection is
 * going through while this launcher holds the arming. Safe to call before
 * the icon exists; NIM_MODIFY on an unregistered icon simply fails. */
static void LauncherSetTrayTip( void ) {
#ifdef MOD_PORTABLE
	strcpy( TrayIcone.szTip, KT_LAUNCHER_TIP_PORTABLE ) ;
#else
	strcpy( TrayIcone.szTip, KT_CAP_LAUNCHER ) ;
#endif
	/* KiTTY: say so when this launcher - and so every session it starts, via
	 * the "&R" prefix - runs with the restricted ACL. */
	if( restricted_acl() ) strcat( TrayIcone.szTip, KT_LAUNCHER_TIP_RESTRICTED ) ;
	if( kitty_workplace_holding() && LauncherWorkplaceProxy[0] ) {
		char line[220], left[64] ;
		kitty_workplace_left_text( left, sizeof(left) ) ;
		snprintf( line, sizeof(line), KT_LAUNCHER_TIP_WORKPLACE,
			LauncherWorkplaceProxy, left[0] ? KT_LAUNCHER_TIP_SWITCHES_OFF : "", left ) ;
		if( strlen(TrayIcone.szTip) + strlen(line) < sizeof(TrayIcone.szTip) )
			strcat( TrayIcone.szTip, line ) ;
	}
}

/* WARNING: the SELECTION is remembered, the ARMED state never is.
 * Which proxy was last chosen is written
 * here so a later launcher start can offer to switch the mode back on; "armed"
 * exists only as this process holding the arming, and switching the mode off
 * deliberately KEEPS the selection - it is what you would want back tomorrow
 * morning. Key spelling matters: mini.c matches ini keys case-sensitively. */

static void LauncherRememberWorkplaceProxy( const char *proxyname, unsigned int minutes ) {
	char m[32] ;
	WriteParameter( INIT_SECTION, KI_WORKPLACEPROXY, (char*)proxyname ) ;
	snprintf( m, sizeof(m), "%u", minutes ) ;
	WriteParameter( INIT_SECTION, KI_WORKPLACEMINUTES, m ) ;
}

/* How long the mode was last switched on for; 0 = until it is switched off or
 * the launcher exits. Remembered with the proxy, so switching it on from the
 * tray repeats the choice made in the config box. */
static unsigned int LauncherRememberedWorkplaceMinutes( void ) {
	char buffer[32] = "" ;
	if( !ReadParameterN( INIT_SECTION, KI_WORKPLACEMINUTES, buffer, sizeof(buffer) ) ) return 0 ;
	if( atoi(buffer) <= 0 ) return 0 ;
	return (unsigned int)atoi(buffer) ;
}

static int LauncherRememberedWorkplaceProxy( char *out, int len ) {
	char buffer[512] = "" ;
	out[0] = '\0' ;
	if( !ReadParameterN( INIT_SECTION, KI_WORKPLACEPROXY, buffer, sizeof(buffer) ) ) return 0 ;
	if( !buffer[0] || (int)strlen(buffer) >= len ) return 0 ;
	strcpy( out, buffer ) ;
	return 1 ;
}

/* How long the workplace notice stays up: [Launcher] noticeseconds, default 15.
 * Ours to choose, which is half the reason the notice is our own window - the
 * shell has ignored the requested balloon duration since Vista. */
static int LauncherNoticeSeconds( void ) {
	char buffer[32] ;
	if( ReadParameterN( KI_SECTION_LAUNCHER, KI_LAUNCHER_NOTICESECONDS, buffer, sizeof(buffer) )
	    && atoi(buffer) > 0 ) return atoi(buffer) ;
	return 15 ;
}

/* Dark green, the same colour the terminal frame uses while a connection is
 * going through the mode's proxy, so the notice and the window read as one
 * thing. */
#define WORKPLACE_GREEN RGB(0,100,0)

static void LauncherWorkplaceBalloon( int on, int by_timeout ) {
	char msg[512] ;
	if( on ) {
		char left[64] ;
		kitty_workplace_left_text( left, sizeof(left) ) ;
		if( left[0] )
			snprintf( msg, sizeof(msg),
				KT_LAUNCHER_WP_ON_LEFT,
				LauncherWorkplaceProxy, left ) ;
		else
			snprintf( msg, sizeof(msg),
				KT_LAUNCHER_WP_ON, LauncherWorkplaceProxy ) ;
	}
	else if( by_timeout )
		snprintf( msg, sizeof(msg),
			KT_LAUNCHER_WP_TIMEOUT, LauncherRearmProxy ) ;
	else
		snprintf( msg, sizeof(msg),
			KT_LAUNCHER_WP_OFF ) ;
	/* Our own window, not a tray balloon: it carries the mode's colour and a
	 * duration we choose, and it is not silently swallowed by focus assist the
	 * way balloons are. The tray tooltip and menu still say the same thing, so
	 * a notice that is missed is never the only record. */
	LauncherSetTrayTip() ;
	Shell_NotifyIcon( NIM_MODIFY, &TrayIcone ) ;
	/* WARNING: only the TIMEOUT notice offers to switch the mode back on. When the
	 * user switched it off themselves they have said what they want, and a
	 * one-click undo in front of them invites the opposite; a timeout is the
	 * case where the mode ended without them deciding anything. */
	kitty_notice_show( on ? KT_CAP_WORKPLACE_ON
	                      : (by_timeout ? KT_CAP_WORKPLACE_TIMEOUT
	                                    : KT_CAP_WORKPLACE_OFF),
	                   msg, WORKPLACE_GREEN, LauncherNoticeSeconds(),
	                   (!on && by_timeout) ? MainHwnd : NULL,
	                   (!on && by_timeout) ? KLWM_WORKPLACEREARM : 0 ) ;
	/* The user has now been told, whichever way it ended, so no later start owes
	 * them the "it is not active" notice. */
	if( !on ) kitty_workplace_notice_settled() ;
}

/* Take the arming for workplace proxy mode. From here on every connection this
 * install starts asks us, and gets this proxy until we let go or die. */
static int LauncherArmWorkplace( const char *proxyname, unsigned int minutes ) {
	if( !kitty_workplace_arm( proxyname, minutes ) ) return 0 ;
	/* Check often enough that "switch off after 4 hours" is not visibly late,
	 * rarely enough to be free. The readers honour the expiry too, so a missed
	 * tick can never route a connection through a proxy whose time is up. */
	if( minutes ) SetTimer( MainHwnd, LAUNCHER_WORKPLACE_TIMER, 30000, NULL ) ;
	else KillTimer( MainHwnd, LAUNCHER_WORKPLACE_TIMER ) ;
	strncpy( LauncherWorkplaceProxy, proxyname, sizeof(LauncherWorkplaceProxy)-1 ) ;
	LauncherWorkplaceProxy[sizeof(LauncherWorkplaceProxy)-1] = '\0' ;
	LauncherRememberWorkplaceProxy( LauncherWorkplaceProxy, minutes ) ;
	/* The breadcrumb: if this arming ends without anybody being told - the
	 * launcher killed, logged off, rebooted - the next start owes one notice. */
	kitty_workplace_mark_armed() ;
	LauncherSetTrayTip() ;
	Shell_NotifyIcon( NIM_MODIFY, &TrayIcone ) ;
	return 1 ;
}

/* What a launcher start says about workplace proxy mode - which, most of the
 * time, is NOTHING.
 *
 * It used to offer to switch the mode back on at every single start, because a
 * remembered selection existed. People run this launcher all day for the session
 * list, so that turned into a notice to be dismissed each time and then ignored
 * - and a notice that is ignored is worse than none, because the one that
 * matters is ignored with it.
 *
 * So: say something only when there is news. The mode having ended without
 * anybody being told IS news, and is said once. Everything else is silence.
 * Switching the mode on lives in the tray menu, where somebody who wants it can
 * find it. */
static void LauncherOfferWorkplaceRearm( void ) {
	/* Already armed - this launcher was started BY the mode being switched on,
	 * and took the arming from its command line before it had a tray icon to
	 * raise a notice from. Say it now, or switching the mode on from the config
	 * box is the one route that announces nothing. */
	if( kitty_workplace_holding() ) { LauncherWorkplaceBalloon( 1, 0 ) ; return ; }
	kitty_workplace_show_pending_notice() ;
}

static void LauncherDisarmWorkplace( void ) {
	KillTimer( MainHwnd, LAUNCHER_WORKPLACE_TIMER ) ;
	kitty_workplace_disarm() ;
	LauncherWorkplaceProxy[0] = '\0' ;   /* the selection stays remembered */
	LauncherSetTrayTip() ;
	Shell_NotifyIcon( NIM_MODIFY, &TrayIcone ) ;
}

/* A launcher that the MODE started goes away again when the mode is switched
 * off: it was started to hold the arming, and leaving a tray icon behind that
 * nobody asked for reads as a bug. [Launcher]
 * exitwithworkplace=no keeps it running instead, for anyone who would rather
 * gain the session list and hotkeys from it.
 *
 * WARNING: it applies ONLY to a launcher the mode itself started. A launcher the
 * user was already running must never be closed by switching a proxy mode off -
 * that would take their session list away as a side effect. */
static int LauncherStartedForWorkplace = 0 ;

static void LauncherExitIfStartedForWorkplace( HWND hwnd ) {
	char buffer[32] ;
	if( !LauncherStartedForWorkplace ) return ;
	if( ReadParameterN( KI_SECTION_LAUNCHER, KI_LAUNCHER_EXITWITHWORKPLACE, buffer, sizeof(buffer) )
	    && !stricmp( buffer, "no" ) ) return ;   /* absent = yes */
	/* WARNING: NOT straight away. The notice saying the mode is off is a window of
	 * OURS, so quitting here would take it off the screen the instant it
	 * appeared - and on the timeout path it is the notice that offers the mode
	 * back with a click, so quitting would remove the offer as well as the news.
	 * Wait for the notice to have had its time, then go. */
	SetTimer( hwnd, LAUNCHER_EXITAFTERNOTICE_TIMER,
	          (UINT)(LauncherNoticeSeconds()*1000 + 1000), NULL ) ;
}

// Main launcher procedures

static LRESULT CALLBACK Launcher_WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
	int ResShell ;
	static UINT s_uTaskbarRestart;

	if( LauncherRefreshMessage != 0 && uMsg == LauncherRefreshMessage ) {
		LauncherRefreshSessionsAndHotkeys( hwnd ) ;
		return 0 ;
	}

	/* KiTTY: somebody clicked the application notification away and [KiTTY]
	 * notesonce is on. Take a share of the "seen" mark, so it lasts as long
	 * as this launcher does rather than only as long as the window that was
	 * clicked. wParam names the note that was on screen (0 = whatever the
	 * store holds now), so a note edited while its notice was still up is not
	 * marked read. Install-keyed, so another install's launcher never answers
	 * it. */
	if( uMsg != 0 && uMsg == kitty_notes_seen_message() ) {
		kitty_notes_seen_hold( (unsigned int)wParam ) ;
		return 0 ;
	}

	/* KiTTY: the config box asking for workplace proxy mode on (wParam 1) or off
	 * (wParam 0). The proxy is not in the message - the requester has just
	 * written the remembered selection, and reading it here keeps one source of
	 * truth for which proxy the mode uses. Install-keyed message, so another
	 * install's launcher never answers this. */
	if( uMsg != 0 && uMsg == kitty_workplace_message() ) {
		if( wParam ) {
			char proxy[256] ;
			if( LauncherRememberedWorkplaceProxy( proxy, sizeof(proxy) )
			    && LauncherArmWorkplace( proxy, (unsigned int)lParam ) ) {
				RefreshMenuLauncher() ;
				LauncherWorkplaceBalloon( 1, 0 ) ;
			}
		} else if( kitty_workplace_holding() ) {
			LauncherDisarmWorkplace() ;
			RefreshMenuLauncher() ;
			LauncherWorkplaceBalloon( 0, 0 ) ;
			LauncherExitIfStartedForWorkplace( hwnd ) ;
		}
		return 0 ;
	}
	
	switch( uMsg ) {
		case WM_CREATE:
		s_uTaskbarRestart = RegisterWindowMessage(TEXT("TaskbarCreated"));
		MenuLauncher = InitLauncherMenu( "Launcher" ) ;
		/* The application notification, if this launcher is the first window
		 * this process opens. Not inside the tray-icon block below: the note
		 * has nothing to do with the tray, and is owed whether or not the
		 * shell accepted the icon. */
		PostMessage( hwnd, KLWM_NOTESPENDING, 0, 0 ) ;

	// Set up the NOTIFYICONDATA structure
	TrayIcone.cbSize = sizeof(TrayIcone);	// give the structure the size it needs
	if( oldIconFlag ) {
		TrayIcone.uID = IDI_BLACKBALL ;	// give it an ID
		TrayIcone.hIcon = LoadIcon((HINSTANCE) GetModuleHandle (NULL), MAKEINTRESOURCE(IDI_BLACKBALL));
	} else {
		TrayIcone.uID = IDI_PUTTY_LAUNCH ;	// give it an ID
		TrayIcone.hIcon = LoadIcon((HINSTANCE) GetModuleHandle (NULL), MAKEINTRESOURCE(IDI_PUTTY_LAUNCH));
	}
	TrayIcone.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;	// say which fields are valid
	// tell it to "listen" to its surroundings (mouse clicks and so on)
	TrayIcone.uCallbackMessage = KLWM_NOTIFYICON;
	//TrayIcone.szTip[1024] = "KiTTY That\'s all folks!\0" ;			// the default tooltip, i.e. nothing
	LauncherSetTrayTip() ;
	TrayIcone.hWnd = hwnd ;
	ResShell = Shell_NotifyIcon(NIM_ADD, &TrayIcone);
	if( ResShell ) {
		LauncherSetTrayTip() ;
		ResShell = Shell_NotifyIcon(NIM_MODIFY, &TrayIcone);
		/* KiTTY: refresh the cached latest version async. The notify variant posts
		 * back when the fetch finishes, so the launcher balloon can appear on the
		 * first run after a new release instead of only after a previous process has
		 * populated the cache. Also check the existing cache immediately. */
		{
			kitty_start_update_check_notify( hwnd, KLWM_UPDATECHECKDONE ) ;
			/* POSTED, not called: this is still WM_CREATE. A balloon raised
			 * from inside window creation is displayed, but a click on it does
			 * not come back to us - the icon's callback only reaches a window
			 * that has finished being created. That is why clicking the first
			 * of the two balloons did nothing while the second one worked.
			 * Handling it through the message loop is the same path the async
			 * result takes, and it is known to work. */
			PostMessage( hwnd, KLWM_UPDATECHECKDONE, 0, 0 ) ;
			PostMessage( hwnd, KLWM_WORKPLACEOFFER, 0, 0 ) ;
		}
		/* An arming taken from the command line was taken before this window
		 * existed, so its expiry timer could not be set against it then. */
		if( kitty_workplace_holding() && kitty_workplace_minutes_left() )
			SetTimer( hwnd, LAUNCHER_WORKPLACE_TIMER, 30000, NULL ) ;
		LauncherRegisterHotkeys( hwnd, 1 ) ;	/* startup: conflicts balloon */
		if (IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_HIDE);
		//SendMessage(hwnd, WM_SYSCOMMAND, SC_MINIMIZE, 0);
		return 1 ;
	} else 
		return 0 ;
			break ;
	
		case KLWM_NOTESPENDING :
			kitty_notes_show_pending( hwnd ) ;
			break ;

		case KLWM_WORKPLACEOFFER :
			LauncherOfferWorkplaceRearm() ;
			break ;

		case KLWM_WORKPLACEREARM :
			/* Taking the offer cancels the pending close outright, rather than
			 * relying on the check in the timer: this launcher is wanted. */
			KillTimer( hwnd, LAUNCHER_EXITAFTERNOTICE_TIMER ) ;
			/* The "mode is OFF" notice was clicked: switch it on with the
			 * proxy the notice named. */
			if( LauncherRearmProxy[0]
			    && LauncherArmWorkplace( LauncherRearmProxy,
			                             LauncherRememberedWorkplaceMinutes() ) ) {
				RefreshMenuLauncher() ;
				LauncherWorkplaceBalloon( 1, 0 ) ;
			}
			break ;

		case KLWM_HOTKEYBALLOON :
			/* Posted by LauncherRegisterHotkeys: session hotkeys collided.
			 * Balloon the report; a click on it opens the config box on the
			 * session that DID get the contested key (see the click below). */
			if( LauncherHotkeyReport[0] ) {
				TrayIcone.uFlags = NIF_INFO | NIF_TIP ;
				TrayIcone.dwInfoFlags = NIIF_WARNING ;
				TrayIcone.uTimeout = 10000 ;
				strncpy( TrayIcone.szInfoTitle, KT_CAP_HOTKEY_CONFLICT, sizeof(TrayIcone.szInfoTitle) ) ;
				TrayIcone.szInfoTitle[sizeof(TrayIcone.szInfoTitle)-1] = '\0' ;
				strncpy( TrayIcone.szInfo, LauncherHotkeyReport, sizeof(TrayIcone.szInfo) ) ;
				TrayIcone.szInfo[sizeof(TrayIcone.szInfo)-1] = '\0' ;
				Shell_NotifyIcon( NIM_MODIFY, &TrayIcone ) ;
				TrayIcone.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE ;
				LauncherHotkeyBalloonArmed = 1 ;
			}
			break ;

		case KLWM_NOTIFYICON :
			switch (lParam)	{
				/* KiTTY: the "update available" balloon is clickable - clicking
				 * it opens the updater (the same dialog as the terminal's
				 * "Check for updates"), instead of merely dismissing a notice
				 * that told you to go and find that menu item yourself. Guarded
				 * on LauncherUpdateKnown so a click on any OTHER balloon this
				 * icon may show does not start an update check. The icon is
				 * registered without NIM_SETVERSION, so the notification code
				 * arrives in lParam like the mouse messages below. */
				case NIN_BALLOONUSERCLICK :
					/* Hotkey-conflict balloon first: it arms itself when shown
					 * and whichever balloon was raised LAST disarmed the other,
					 * so the flags cannot both claim this click. Clicking it
					 * opens the config box on the session that kept the key,
					 * for inspection - via the same last-session remembering
					 * the box's startup pre-fill already reads. */
					if( LauncherHotkeyBalloonArmed ) {
						LauncherHotkeyBalloonArmed = 0 ;
						if( LauncherHotkeyWinner[0] ) {
							Conf * wc = conf_new() ;
							if( wc != NULL ) {
								if( do_defaults( LauncherHotkeyWinner, wc ) )
									kitty_set_last_folder( conf_get_str( wc, CONF_folder ) ) ;
								conf_free( wc ) ;
							}
							kitty_set_last_session( LauncherHotkeyWinner ) ;
							RunPuTTYAtPanel( hwnd, "Session/Startup", 1 ) ;
						}
					}
					/* The update balloon is the only balloon left: the workplace
					 * notices are our own window, which handles its own click. */
					else if( LauncherUpdateKnown ) {
						CheckVersionFromWebSite( hwnd, 0 ) ;
					}
				break ;
				case WM_LBUTTONDBLCLK :
					/* KiTTY: double click opens a new default KiTTY window
					 * (the configuration box); cancel the pending
					 * single-click menu first */
					if ( (wParam == IDI_PUTTY_LAUNCH) || (wParam == IDI_BLACKBALL) ) {
						KillTimer( hwnd, LAUNCHER_TRAYCLICK_TIMER ) ;
						LauncherIgnoreUp = 1 ;
						RunPuTTY( hwnd, "" ) ;
						}
				break ;
				case WM_RBUTTONUP:
					{
					if ( (wParam == IDI_PUTTY_LAUNCH) || (wParam == IDI_BLACKBALL) ) {
						RefreshMenuLauncher() ;
						DisplayContextMenu( hwnd, HideMenu ) ;
						}
					}
				break ;
				case WM_LBUTTONUP:
					{
					/* KiTTY: delay the menu by the double-click time so the
					 * first click of a double click doesn't pop it up under
					 * the second click; remember where the click happened */
					if ( (wParam == IDI_PUTTY_LAUNCH) || (wParam == IDI_BLACKBALL) ) {
						if( LauncherIgnoreUp ) {
							LauncherIgnoreUp = 0 ;
						} else {
							GetCursorPos( &LauncherClickPoint ) ;
							SetTimer( hwnd, LAUNCHER_TRAYCLICK_TIMER, GetDoubleClickTime(), NULL ) ;
						}
						}
					}
				break ;
				}
			break ;
	
		case WM_TIMER:
			/* KiTTY: no double click arrived - deliver the left-click menu
			 * at the position of the original click */
			if( wParam == LAUNCHER_EXITAFTERNOTICE_TIMER ) {
				KillTimer( hwnd, LAUNCHER_EXITAFTERNOTICE_TIMER ) ;
				/* Unless the mode is back on - the user clicked the timeout
				 * notice, and closing now would take away what they just
				 * asked for. */
				if( !kitty_workplace_holding() ) {
					Shell_NotifyIcon( NIM_DELETE, &TrayIcone ) ;
					PostQuitMessage( 0 ) ;
				}
				break ;
			}
			if( wParam == LAUNCHER_WORKPLACE_TIMER ) {
				/* The arming's time is up: end the mode where the user can see
				 * it, rather than leaving the tray saying it is on until the
				 * next connection quietly disagrees. */
				if( kitty_workplace_holding() && !kitty_workplace_minutes_left() ) {
					/* Remember what it was using, so the timeout notice can
					 * offer that same proxy back with one click. */
					LauncherRememberedWorkplaceProxy( LauncherRearmProxy,
					                                  sizeof(LauncherRearmProxy) ) ;
					LauncherDisarmWorkplace() ;
					RefreshMenuLauncher() ;
					LauncherWorkplaceBalloon( 0, 1 ) ;   /* by timeout */
					LauncherExitIfStartedForWorkplace( hwnd ) ;
				}
				break ;
			}
			if( wParam == LAUNCHER_OPENFOLDER_TIMER ) {
				LauncherFolderOpenNext( hwnd ) ;
				break ;
			}
			if( wParam == LAUNCHER_REFRESHSPIN_TIMER ) {
				LauncherRefreshSpinStep( hwnd ) ;
				break ;
			}
			if( wParam == LAUNCHER_REFRESHCOVER_TIMER ) {
				LauncherCoverRemove( hwnd ) ;   /* the new menu is up: the picture goes */
				break ;
			}
			if( wParam == LAUNCHER_TRAYCLICK_TIMER ) {
				KillTimer( hwnd, LAUNCHER_TRAYCLICK_TIMER ) ;
				RefreshMenuLauncher() ;
				LauncherMenuPoint = LauncherClickPoint ;
				LauncherMenuPointValid = 1 ;
				DisplayContextMenuAt( hwnd, MenuLauncher, LauncherMenuPoint ) ;
			}
			break ;
		case WM_SETTINGCHANGE:
			/* The SYSTEM switched between light and dark (by hand, or by the
			 * time of day). A launcher set to follow the system keeps the same
			 * app mode across that, so nothing else would drop the theme its
			 * menus cached when they were first opened. */
			if( lParam && ( IsWindowUnicode( hwnd )
			        ? !wcscmp( (const wchar_t *)lParam, L"ImmersiveColorSet" )
			        : !strcmp( (const char *)lParam, "ImmersiveColorSet" ) ) )
				kitty_theme_system_changed() ;
			break ;
		case WM_MENURBUTTONUP:
			/* A right click inside the open tray menu: on a session folder it
			 * offers to open the folder's sessions, anywhere else it is nothing. */
			LauncherFolderContextMenu( hwnd, (HMENU)lParam, (int)wParam ) ;
			break ;
		case KLWM_REFRESHINPLACE:
			/* Refresh was chosen in the open tray menu; the menu is still up. */
			LauncherRefreshInPlace( hwnd ) ;
			break ;
		case KLWM_OPENFOLDER:
			/* The folder entry was chosen and the menu loop has ended. */
			if( LauncherFolderCount > LAUNCHER_OPENFOLDER_ASK_ABOVE ) {
				char q[512] ;
				snprintf( q, sizeof(q), KT_LAUNCHER_OPEN_FOLDER_CONFIRM,
				          LauncherFolderCount, LauncherFolderName ) ;
				if( MessageBox( hwnd, q, KT_CAP_LAUNCHER,
				                MB_YESNO | MB_ICONQUESTION ) != IDYES ) {
					LauncherFolderNext = LauncherFolderCount ;   /* drop the list */
					LauncherFolderOpenNext( hwnd ) ;
					break ;
				}
			}
			LauncherFolderOpenNext( hwnd ) ;                 /* the first one now */
			if( LauncherFolderItems != NULL )
				SetTimer( hwnd, LAUNCHER_OPENFOLDER_TIMER, LAUNCHER_OPENFOLDER_GAP_MS, NULL ) ;
			break ;
		case KLWM_UPDATECHECKDONE:
			ShowLauncherUpdateBalloon() ;
			break ;
		case WM_HOTKEY:
			{
				int i ;
				for( i=0 ; i<LauncherHotkeyCount ; i++ )
					if( (int)wParam == LauncherHotkeys[i].id ) {
						RunSession( hwnd, LauncherHotkeys[i].folder, LauncherHotkeys[i].session ) ;
						break ;
					}
			}
			break ;
		case WM_DESTROY: 
			LauncherUnregisterHotkeys( hwnd ) ;
			ManageUnHideAll( hwnd ) ;
			PostQuitMessage( 0 ) ;
			break ;
		case WM_CLOSE:
			PostMessage(hwnd, WM_DESTROY,0,0) ;
			break ;
		case WM_COMMAND: {//Menu commands
			switch( LOWORD(wParam) ) {
				case IDM_ABOUT: {
					/* UTF-8 source (real "(c)" and em-dash); MessageBoxW renders it
					 * as Unicode regardless of the system ANSI codepage, so the
					 * earlier mojibake (A© / "a\200\224") cannot recur. */
					const char *ab =
						KT_LAUNCHER_ABOUT_PREFIX BUILD_VERSION "\r\n"
#ifdef KITTY_TEST_BUILD_LABEL
						KT_LAUNCHER_ABOUT_TESTBUILD KITTY_TEST_BUILD_LABEL "\r\n"
#endif
						KT_LAUNCHER_ABOUT_BODY ;
					WCHAR wab[512] ;
					MultiByteToWideChar( CP_UTF8, 0, ab, -1, wab, 512 ) ;
					/* Modal, no sound (plain MB_OK, no MB_ICON* asterisk). MessageBoxW
					 * renders the title and Unicode text correctly at any DPI. */
					MessageBoxW( hwnd, wab, L"About KiTTY Launcher", MB_OK ) ;
					break ; }
				case IDM_QUIT:
					ResShell = Shell_NotifyIcon(NIM_DELETE, &TrayIcone) ;
					ManageUnHideAll( hwnd ) ;
					PostQuitMessage( 0 ) ;
					break ;
				case IDM_LAUNCHER:
					DestroyMenu( MenuLauncher ) ; 
					MenuLauncher = NULL ;
					MenuLauncher = InitLauncherMenu( "Launcher" ) ;
					Shell_NotifyIcon(NIM_DELETE, &TrayIcone);
					Shell_NotifyIcon(NIM_ADD, &TrayIcone);
					Shell_NotifyIcon(NIM_MODIFY, &TrayIcone);
					break ;
				case IDM_LAUNCHER+1:
					RunPuTTY( hwnd, "" ) ;
					break ;
				case IDM_LAUNCHER+2:
					RunPuTTY( hwnd, "-ed" ) ;
					break ;
				case IDM_LAUNCHER+3:
					ManageHideAll( hwnd ) ;
					RefreshMenuLauncher() ;
					break ;
				case IDM_LAUNCHER+4:
					ManageUnHideAll( hwnd ) ;
					RefreshMenuLauncher() ;
					break ;
				case IDM_LAUNCHER+5:
					RefreshMenuLauncher() ;
					break ;
				case IDM_LAUNCHER+6:
					IsUnique = abs( IsUnique -1 ) ;
					RefreshMenuLauncher() ;
					break ;
				case IDM_LAUNCHER+8: {
					/* KiTTY: toggle a "KiTTY Launcher" shortcut (kitty*.exe
					 * -launcher) in the USER Startup folder. Everything keys off
					 * whether a shortcut targeting THIS exe exists, so the
					 * generic name never makes us act on another KiTTY's entry:
					 *  - our own per-user shortcut     -> remove it (turn off);
					 *  - an all-users shortcut for us  -> installer-managed,
					 *    cannot remove without elevation, so just explain;
					 *  - none for us, but a per-user shortcut for a DIFFERENT
					 *    KiTTY exists                  -> leave it, explain;
					 *  - nothing                       -> create ours. */
					char exe[MAX_PATH], dir[MAX_PATH], *slash ;
					DWORD n = GetModuleFileNameA( NULL, exe, sizeof(exe) ) ;
					if( n && n < sizeof(exe) ) {
						if( kitty_startup_shortcut_points_to("KiTTY Launcher", 0, exe) ) {
							kitty_startup_shortcut_set("KiTTY Launcher", NULL, NULL, NULL, NULL, 0) ;
						} else if( kitty_startup_shortcut_points_to("KiTTY Launcher", 1, exe) ) {
							MessageBox( hwnd,
							    KT_LAUNCHER_STARTUP_ALLUSERS,
							    KT_CAP_LAUNCHER, MB_ICONINFORMATION | MB_OK ) ;
						} else if( kitty_startup_shortcut_exists("KiTTY Launcher") ) {
							MessageBox( hwnd,
							    KT_LAUNCHER_STARTUP_OTHER_KITTY,
							    KT_CAP_LAUNCHER, MB_ICONINFORMATION | MB_OK ) ;
						} else {
							snprintf( dir, sizeof(dir), "%s", exe ) ;
							slash = strrchr( dir, '\\' ) ; if( slash ) *slash = '\0' ;
							kitty_startup_shortcut_set("KiTTY Launcher", exe, "-launcher", dir, exe, 1) ;
						}
					}
					RefreshMenuLauncher() ;
					break ; }
				case IDM_LAUNCHER+9:
					/* KiTTY: "Update available ... install" - same updater the
					 * balloon click and the terminal's system menu open. */
					{ extern void CheckVersionFromWebSite( HWND hwnd, int is_terminal ) ;
					  CheckVersionFromWebSite( hwnd, 0 ) ; }
					break ;
				case IDM_LAUNCHER+7:
					if( LauncherConfReload ) InitLauncherRegistry() ;
					RefreshMenuLauncher() ;
					/* Manual Refresh is an explicit request for the current
					 * state, so a changed conflict report balloons here too. */
					LauncherRegisterHotkeys( hwnd, 1 ) ;
					/* Keep the launcher visible after an explicit Refresh: users expect
					 * to continue choosing from the freshly rebuilt session tree rather
					 * than having the tray menu vanish. TrackPopupMenu has already
					 * returned before WM_COMMAND is delivered, so re-open the rebuilt
					 * menu at the same anchor point as the pre-refresh menu, not at the
					 * current cursor position over the Refresh item. */
					if( LauncherMenuPointValid ) DisplayContextMenuAt( hwnd, MenuLauncher, LauncherMenuPoint ) ;
					else DisplayContextMenu( hwnd, MenuLauncher ) ;
					break ;
				case IDM_GONEXT:
					ManageGoNext( hwnd ) ;
					break ;
				case IDM_GOPREVIOUS:
					ManageGoPrevious( hwnd ) ;
					break ;
				case IDM_WORKPLACE:
					/* Switch workplace proxy mode off. The selection stays
					 * remembered so the next start can offer it back.
					 *
					 * WARNING: this launcher STAYS, even when the mode started it
					 * and exitwithworkplace is on: the user is standing in this
					 * menu right now, so the thing they just clicked vanishing
					 * under them reads as a crash. The exit
					 * is for disarms the user did not perform HERE - from the
					 * config box, or the timeout running out - where nobody is
					 * looking at the tray and a leftover icon is the surprise
					 * instead. */
					LauncherDisarmWorkplace() ;
					RefreshMenuLauncher() ;
					LauncherWorkplaceBalloon( 0, 0 ) ;
					break ;
				}
				/* Workplace proxy mode ON, with named proxy (id - base - 1).
				 * A range, so it cannot be a switch case above. */
				{
					int wp = LOWORD(wParam) - (IDM_WORKPLACE+1) ;
					if( wp >= 0 && wp < MAX_PROXY-2 ) {
						InitProxyList() ;
						if( proxies[wp+2].name ) {
							if( LauncherArmWorkplace( proxies[wp+2].name,
						                          LauncherRememberedWorkplaceMinutes() ) )
								LauncherWorkplaceBalloon( 1, 0 ) ;
							RefreshMenuLauncher() ;
						}
					}
				}
				int nb ;
				nb = LOWORD(wParam)-IDM_USERCMD ;
				if( ( nb >= 0 ) && ( nb<NB_MENU_MAX ) ) {
					if( SpecialMenu[nb]!= NULL )
					//if( strlen( SpecialMenu[nb] ) > 0 ) 
						{
						if( DirectoryBrowseFlag ) {
							char buffer[1024]="" ;
							GetMenuString( MenuLauncher, nb+IDM_USERCMD, buffer, 1024, MF_BYCOMMAND ) ;
							RunSession( hwnd, SpecialMenu[nb], buffer ) ;
							}
						else RunSession( hwnd, SpecialMenu[nb], SpecialMenu[nb] ) ;
						RefreshMenuLauncher() ;
						}
					break ;
					}
				nb = LOWORD(wParam)-IDM_GOHIDE ;
				if( ( nb >= 0 ) && ( nb<100 ) ) {
					if( !IsUnique )	ManageSwitch( nb ) ;
					else { 
						ManageHideAll( hwnd ) ; 
						ManageUnHideOne( TabWin[nb].hwnd ) ;
						}
					RefreshMenuLauncher() ;
					break ;
					}
				}
			break ;
		default: // Default message
			if( uMsg == s_uTaskbarRestart ) { // show the icon again after a Windows Explorer crash
				Shell_NotifyIcon(NIM_DELETE, &TrayIcone);
				Shell_NotifyIcon(NIM_ADD, &TrayIcone);
				Shell_NotifyIcon(NIM_MODIFY, &TrayIcone);
			}
			return DefWindowProc(hwnd, uMsg, wParam, lParam);
	}
	return -1 ;
}
	
int WINAPI Launcher_WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show) {
	hinst = inst ;
	WNDCLASS wndclass ;
	MSG msg;
	char buffer[4096] ;
	char className[1024] = "KiTTYLauncher" ;
	
	if( strcmp(KiTTYClassName,appname) ) { strcpy(className,KiTTYClassName) ; }
	else if( strcmp(KiTTYClassName,"KiTTY") ) { strcpy(className,KiTTYClassName) ; }
	if( ReadParameterN( KI_SECTION_LAUNCHER, KI_LAUNCHER_CLASSNAME, buffer, sizeof(buffer) ) ) {
		buffer[1023]='\0' ;
		if( strlen(buffer)>0 ) { strcpy(className,buffer) ; }
	}
	
	if( strstr( cmdline, "-putty" ) != NULL ) SetPuttyFlag(1) ;

	/* KiTTY: -workplace <named proxy> switches workplace proxy mode ON and
	 * hands this launcher the arming. The
	 * arming lives and dies with this process: every connection asks whether one
	 * is held right now, so killing the launcher, logging off or rebooting
	 * switches the mode off with nothing to clean up.
	 *
	 * Taken BEFORE the window exists, so the tray tooltip built in WM_CREATE
	 * already names the proxy - and BEFORE the already-running check below,
	 * which is the subtle part: see the comment on that check. */
	{
		char *w = strstr( cmdline, "-workplace" ) ;
		unsigned int minutes = 0 ;
		char *m = strstr( cmdline, "-workplaceminutes" ) ;
		if( m != NULL ) {
			m += strlen("-workplaceminutes") ;
			while( *m==' ' || *m=='\t' ) m++ ;
			if( atoi(m) > 0 ) minutes = (unsigned int)atoi(m) ;
		}
		/* "-workplace" must be followed by whitespace or a quote, or
		 * "-workplaceminutes 240" would be read as a proxy named "minutes". */
		if( w != NULL && (w[strlen("-workplace")]==' ' || w[strlen("-workplace")]=='\t'
		                  || w[strlen("-workplace")]=='"') ) {
			char proxy[256] = "" ;
			w += strlen("-workplace") ;
			while( *w==' ' || *w=='\t' ) w++ ;
			if( *w=='"' ) {
				char *end = strchr( ++w, '"' ) ;
				if( end && (end-w) < (int)sizeof(proxy) ) {
					memcpy( proxy, w, end-w ) ; proxy[end-w] = '\0' ;
				}
			} else {
				int i = 0 ;
				while( *w && *w!=' ' && *w!='\t' && i < (int)sizeof(proxy)-1 ) proxy[i++] = *w++ ;
				proxy[i] = '\0' ;
			}
			if( proxy[0] && LauncherArmWorkplace( proxy, minutes ) )
				/* Started BY the mode: only such a launcher may be closed again
				 * when the mode goes off, and only if asked to be. */
				LauncherStartedForWorkplace = 1 ;
		}
	}

	/*
	 * Only ONE launcher, normally - but the window class is shared by every
	 * KiTTY on the machine, while workplace proxy mode is per INSTALL.
	 *
	 * So a portable copy on a stick, or a second installation in another
	 * directory, owns this class as soon as its launcher is in the tray, and a
	 * launcher started to hold OUR install's arming would exit here without
	 * arming anything - the mode would simply refuse to switch on, with a
	 * launcher visibly running. That is why the arming is taken above and why
	 * holding one exempts this launcher from the check.
	 *
	 * Two launchers of the SAME install still cannot both arm: the arming
	 * itself refuses a second holder (CreateFileMapping ERROR_ALREADY_EXISTS),
	 * so the second one fails to arm and exits here like any other duplicate.
	 */
	if( !kitty_workplace_holding() && FindWindow(className,className) ) {
		if( ReadParameterN( KI_SECTION_LAUNCHER, KI_LAUNCHER_ALREADYRUNCHECK, buffer, sizeof(buffer) ) ) {
			if( !stricmp( buffer, "yes" ) ) return 0 ;
		} else {
			return 0 ;
		}
	}

	LauncherRefreshMessage = RegisterWindowMessageA(KITTY_LAUNCHER_REFRESH_MESSAGE) ;

	wndclass.style = 0;
	wndclass.lpfnWndProc = Launcher_WndProc;
	wndclass.cbClsExtra = 0;
	wndclass.cbWndExtra = 0;
	wndclass.hInstance = inst;
	if( strstr( cmdline, "-oldicon" ) != NULL ) { oldIconFlag = 1 ; } 
	if( oldIconFlag ) { wndclass.hIcon = LoadIcon(inst, MAKEINTRESOURCE(IDI_BLACKBALL) ); }
	else { wndclass.hIcon = LoadIcon(inst, MAKEINTRESOURCE(IDI_PUTTY_LAUNCH) ); }
	wndclass.hCursor = LoadCursor(NULL, IDC_IBEAM) ;
	wndclass.hbrBackground = NULL;
	wndclass.lpszMenuName = NULL;
	wndclass.lpszClassName = className ;

	if( !RegisterClass(&wndclass) ) return 1 ;

	if( ReadParameterN( KI_SECTION_LAUNCHER, KI_LAUNCHER_RELOAD, buffer, sizeof(buffer) ) ) {
		if( !stricmp( buffer, "NO" ) ) LauncherConfReload = 0 ;
	}
	/* KiTTY 0.84: the launcher can run before the main terminal (e.g. the boot Startup
	 * shortcut), so migrate the old 9bis.com\KiTTY hive here too before reading sessions. */
	MigrateOldKittyHive() ;
	if( LauncherConfReload ) InitLauncherRegistry() ;
		
	MainHwnd = CreateWindowEx(0, className, "KiTTYLauncher",
				0,//WS_OVERLAPPEDWINDOW,
				CW_USEDEFAULT, CW_USEDEFAULT,
				CW_USEDEFAULT, CW_USEDEFAULT,
				NULL, NULL, inst, NULL);
	
	//ShowWindow(hwnd, show) ; UpdateWindow(hwnd) ;

	while (GetMessage(&msg, NULL, 0, 0)) {
		//if(!TranslateAccelerator(hwnd, hAccel, &msg)){
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		//	}
	}
	return msg.wParam;
}

#endif
void RunConfig( Conf * conf ) {
	char b[2048];
	//char c[180];
	//int freecl = FALSE;
	char *cl;
	const char *argprefix;
	BOOL inherit_handles;
	STARTUPINFO si;
	PROCESS_INFORMATION pi;
	HANDLE filemap = NULL;
	
	/* Pass the session conf (the auto-login password included) to the child
	 * putty process through the anonymous, inherit-only file mapping below -
	 * exactly as the Duplicate-Session hand-off (window.c IDM_DUPSESS) does,
	 * and with the same protection: a COPY is serialised, with its password
	 * fields wrapped for the LOGON (kitty_pwmem.c), and the child re-wraps them
	 * for itself right after conf_deserialise. So the shared section never
	 * holds a password in the clear.
	 * The old code MASKPASS-obfuscated the password here on the assumption the
	 * child would un-mask it; the 0.84 child does not, so it sent the masked
	 * bytes and auto-login and WinSCP launches failed for every stored
	 * password. */

	/* restricted_acl is a FUNCTION in the 0.84 core (it was a variable in the
	 * 0.76-era tree this file came from) - testing the bare identifier was
	 * always true, so every session spawned here ran with the restricted
	 * process ACL ("&R") regardless of how this process was started. */
	if (restricted_acl()) {
		argprefix = "&R";
	} else {
		argprefix = "";
	}
	/*
	 * Allocate a file-mapping memory chunk for the
	 * config structure.
	 */
	SECURITY_ATTRIBUTES sa;
	strbuf *serbuf;
	void *p;
	int size;

	{
		Conf *wire = conf_copy(conf);
		kitty_pw_seal_for_handoff(wire);
		serbuf = strbuf_new_nm();
		conf_serialise(BinarySink_UPCAST(serbuf), wire);
		kitty_pw_wipe(wire);   /* conf_free does not clear what it frees */
		conf_free(wire);
	}
	size = serbuf->len;

	sa.nLength = sizeof(sa);
	sa.lpSecurityDescriptor = NULL;
	sa.bInheritHandle = TRUE;
	filemap = CreateFileMapping(INVALID_HANDLE_VALUE,
		&sa,
		PAGE_READWRITE,
		0, size, NULL);
	if (filemap && filemap != INVALID_HANDLE_VALUE) {
		p = MapViewOfFile(filemap, FILE_MAP_WRITE, 0, 0, size);
		if (p) {
			memcpy(p, serbuf->s, size);
			UnmapViewOfFile(p);
		}
	}
	strbuf_free(serbuf);
	inherit_handles = true;
	/* KiTTY: unlock the portable master password once for this launcher run and
	 * pass it to the spawned session so it doesn't re-prompt (launcher-mpw-
	 * sharing). The first launch prompts; later ones reuse the unlock. */
	HANDLE mpwmap = NULL; char mpwtok[64] = "";
	{ extern int kitty_mpw_startup_unlock(void);
	  kitty_mpw_startup_unlock();
	  mpwmap = kitty_mpw_export_inherit_blob("&K", mpwtok, sizeof(mpwtok)); }
	cl = dupprintf("putty %s%s&%p:%u", argprefix, mpwtok,
		filemap, (unsigned)size);

	GetModuleFileName(NULL, b, sizeof(b) - 1);
	si.cb = sizeof(si);
	si.lpReserved = NULL;
	si.lpDesktop = NULL;
	si.lpTitle = NULL;
	si.dwFlags = 0;
	si.cbReserved2 = 0;
	si.lpReserved2 = NULL;
	if( CreateProcess(b, cl, NULL, NULL, inherit_handles,
		NORMAL_PRIORITY_CLASS, NULL, NULL, &si, &pi) ) {
		/* let the spawned KiTTY window come to the foreground (see RunCommand) */
		AllowSetForegroundWindow( pi.dwProcessId ) ;
		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
	}

	if (filemap)
		CloseHandle(filemap);
	if (mpwmap)
		CloseHandle(mpwmap);
	sfree(cl);
}

static void launcher_run_session_cmd( HWND hwnd, char * cmd, HANDLE inheritmap ) ;

void RunPuTTY( HWND hwnd, char * param ) {
	char buffer[4096]="",shortname[1024]="" ; ;
	if( GetModuleFileName( NULL, (LPTSTR)buffer, 1023 ) )
		if( GetShortPathName( buffer, shortname, 1023 ) ) {
			if( strlen(param) > 0 ) {
				/* A prefix-dispatched mode (e.g. "-ed" editor): don't inject
				 * "-mpwkey" - it would break the "-ed" cmdline detection. */
				snprintf( buffer, sizeof(buffer), "%s %s", shortname, param ) ;
				RunCommand( hwnd, buffer ) ;
			} else {
				/* New configuration box: share the master-password unlock so the
				 * config box - and the session it opens - doesn't re-prompt
				 * (launcher-mpw-sharing). Unlocks once if not already unlocked. */
				HANDLE mpwmap = NULL ; char mpwtok[80] = "" ;
				/* KiTTY: and pass on the restricted ACL, exactly as RunSession()
				 * does. Without this, double-clicking the tray icon of a
				 * RESTRICTED launcher opened an UNrestricted configuration box -
				 * and every session started from it was unrestricted too, with
				 * nothing to say so.
				 * NOT the "&R" prefix RunSession() uses: that form is only
				 * recognised when it is followed by end-of-line, "@" or "&"
				 * (handle_restrict_acl_cmdline_prefix), and this command line
				 * continues with " -mpwkey ...". The ordinary switch is
				 * whitespace-safe and the child's parser applies it before the
				 * window exists. */
				const char * aclprefix = restricted_acl() ? " -restrict-acl" : "" ;
				{ extern int kitty_mpw_startup_unlock(void);
				  kitty_mpw_startup_unlock();
				  mpwmap = kitty_mpw_export_inherit_blob(" -mpwkey ", mpwtok, sizeof(mpwtok)); }
				snprintf( buffer, sizeof(buffer), "%s%s%s", shortname, aclprefix, mpwtok ) ;
				launcher_run_session_cmd( hwnd, buffer, mpwmap ) ;
				if( mpwmap ) CloseHandle( mpwmap ) ;
			}
		}
}

/* Open a new configuration box exactly like RunPuTTY(hwnd,"") - same restricted
 * ACL and master-password sharing - but landed on a named panel via -cfgpanel,
 * the switch the workplace-proxy notice already opens Connection/Proxy with.
 * The hotkey-conflict balloon uses it for Session/Startup, where the hotkey
 * controls are. Quote the path if it contains spaces (window.c's -cfgpanel
 * scan understands quotes). */
static void RunPuTTYAtPanel( HWND hwnd, const char * panel, int mark_loaded ) {
	char buffer[4096]="",shortname[1024]="" ;
	if( GetModuleFileName( NULL, (LPTSTR)buffer, 1023 ) )
		if( GetShortPathName( buffer, shortname, 1023 ) ) {
			HANDLE mpwmap = NULL ; char mpwtok[80] = "" ;
			const char * aclprefix = restricted_acl() ? " -restrict-acl" : "" ;
			{ extern int kitty_mpw_startup_unlock(void);
			  kitty_mpw_startup_unlock();
			  mpwmap = kitty_mpw_export_inherit_blob(" -mpwkey ", mpwtok, sizeof(mpwtok)); }
			/* mark_loaded: the balloon named a session and pre-set it as the
			 * last one, so the box treats the restore as a deliberate load
			 * and Save does not raise the overwrite warning about it. */
			snprintf( buffer, sizeof(buffer), "%s%s%s -cfgpanel %s%s", shortname, aclprefix, mpwtok, panel,
			          mark_loaded ? " -cfgloaded" : "" ) ;
			launcher_run_session_cmd( hwnd, buffer, mpwmap ) ;
			if( mpwmap ) CloseHandle( mpwmap ) ;
		}
}

/* Spawn a launcher session command line. When `inheritmap` is non-NULL the child
 * must inherit it (the shared master-key mapping, launcher-mpw-sharing), so spawn
 * with handle inheritance; otherwise use the plain RunCommand path. */
static void launcher_run_session_cmd( HWND hwnd, char * cmd, HANDLE inheritmap ) {
	if( inheritmap == NULL ) { RunCommand( hwnd, cmd ) ; return ; }
	STARTUPINFO si ; PROCESS_INFORMATION pi ;
	ZeroMemory( &si, sizeof(si) ) ; si.cb = sizeof(si) ;
	ZeroMemory( &pi, sizeof(pi) ) ;
	if( CreateProcess( NULL, cmd, NULL, NULL, TRUE, NORMAL_PRIORITY_CLASS, NULL, NULL, &si, &pi ) ) {
		AllowSetForegroundWindow( pi.dwProcessId ) ;
		WaitForInputIdle( pi.hProcess, INFINITE ) ;
		CloseHandle( pi.hProcess ) ; CloseHandle( pi.hThread ) ;
	} else {
		RunCommand( hwnd, cmd ) ;   /* fallback: still launch (child will prompt) */
	}
}

int RunSession( HWND hwnd, const char * folder_in, char * session_in ) {
	char buffer[4096]="", shortname[1024]="" ;
	char *session=NULL ;
	int return_code=0 ;
	HANDLE mpwmap = NULL ; char mpwtok[80] = "" ;

	if( session_in==NULL ) return 0 ;
	if( strlen(session_in) <= 0 ) return 0 ;

	if( !GetModuleFileName( NULL, (LPTSTR)buffer, 1023 ) ) return 0 ;
	if( !GetShortPathName( buffer, shortname, 1023 ) ) return 0 ;

	/* KiTTY: unlock the portable master password once for this launcher run and
	 * hand it to the spawned session (inherited handle + "-mpwkey" argument,
	 * launcher-mpw-sharing) so the session doesn't re-prompt. Only prompts when a
	 * master password is actually configured; "-mpwkey" is placed before "-load"
	 * so the stored password decrypts as the session loads. */
	{ extern int kitty_mpw_startup_unlock(void);
	  kitty_mpw_startup_unlock();
	  mpwmap = kitty_mpw_export_inherit_blob(" -mpwkey ", mpwtok, sizeof(mpwtok)); }
	/* KiTTY: a session started from a RESTRICTED launcher must be restricted
	 * too. This path builds a plain "-load <session>" command line and passed
	 * nothing on, so every session opened from the launcher menu ran without
	 * the ACL while the launcher itself had it - silently, which is the whole
	 * problem this marker work exists to solve. The switch rather than the "&R"
	 * prefix, because what follows here is " -mpwkey ..." / " -load ..." and &R
	 * is only recognised before end-of-line, "@" or "&". Appended to shortname,
	 * so both the registry/file and the savemode=dir branches below inherit it. */
	if( restricted_acl() ) { size_t _sl=strlen(shortname); snprintf( shortname+_sl, sizeof(shortname)-_sl, " -restrict-acl" ) ; }
	if( mpwtok[0] ) { size_t _sl=strlen(shortname); snprintf( shortname+_sl, sizeof(shortname)-_sl, "%s", mpwtok ) ; }

	session = (char*)malloc(strlen(session_in)+100) ;
	
	if( (IniFileFlag==SAVEMODE_REG)||(IniFileFlag==SAVEMODE_FILE) ) {
		mungestr(session_in, session) ;
		snprintf( buffer, sizeof(buffer), "%s\\Sessions\\%s", kitty_registry_base(), session ) ;
		if( RegTestKey(HKEY_CURRENT_USER, buffer) ) {
			strcpy( session, session_in ) ;
			if( strlen(session)>0 && session[strlen(session)-1] == '&' ) {
				session[strlen(session)-1]='\0' ;
				str_rtrim( session, " \t" ) ;
				if( GetPuttyFlag() )	snprintf( buffer, sizeof(buffer), "%s -putty -load \"%s\" -send-to-tray", shortname, session ) ;
				else sprintf( buffer, "%s -load \"%s\" -send-to-tray", shortname, session ) ;
			} else {
				if( GetPuttyFlag() )	snprintf( buffer, sizeof(buffer), "%s -putty -load \"%s\"", shortname, session ) ;
				else sprintf( buffer, "%s -load \"%s\"", shortname, session ) ;
			}
			launcher_run_session_cmd( hwnd, buffer, mpwmap ) ;
			return_code = 1 ;
		} else {
			RunCommand( hwnd, session_in ) ;
		}
	} else if( IniFileFlag==SAVEMODE_DIR ) {
		if( DirectoryBrowseFlag ) {
			if( (folder_in!=NULL)&&strcmp(folder_in,"")&&strcmp(folder_in,"Default") ) {
				{ size_t _sl=strlen(shortname); snprintf( shortname+_sl, sizeof(shortname)-_sl, " -folder \"%s\"", folder_in ) ; }
			}
		}
		strcpy( session, session_in ) ;
		if( strlen(session)>0 && session[strlen(session)-1] == '&' ) {
			session[strlen(session)-1]='\0' ;
			str_rtrim( session, " \t" ) ;
			if( GetPuttyFlag() )	snprintf( buffer, sizeof(buffer), "%s -putty -load \"%s\" -send-to-tray", shortname, session ) ;
			else sprintf( buffer, "%s -load \"%s\" -send-to-tray", shortname, session ) ;
		} else {
			if( GetPuttyFlag() )	snprintf( buffer, sizeof(buffer), "%s -putty -load \"%s\"", shortname, session ) ;
			else sprintf( buffer, "%s -load \"%s\"", shortname, session ) ;
			//else sprintf( buffer, "%s @%s", shortname, session ) ;
		}
/*		if( DirectoryBrowseFlag ) {
			if( strcmp(folder_in,"")&&strcmp(folder_in,"Default") ) {
				strcat( buffer, " -folder \"" ) ;
				strcat( buffer, folder_in ) ;
				strcat( buffer, "\"" ) ;
				}
			}*/
//MessageBox( hwnd, buffer, "Info", MB_OK ) ;
		launcher_run_session_cmd( hwnd, buffer, mpwmap ) ;
		return_code = 1 ;
	}

	if( mpwmap ) CloseHandle( mpwmap ) ;
	free( session ) ;
	return return_code ;
}
