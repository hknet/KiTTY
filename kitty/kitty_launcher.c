#ifdef MOD_LAUNCHER

// define recupere de WIN_RES.H
#ifndef IDI_PUTTY_LAUNCH
#define IDI_PUTTY_LAUNCH 9901
#endif
#ifndef IDI_BLACKBALL
#define IDI_BLACKBALL 9902
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
#define LAUNCHER_HOTKEY_MAX	32
#define KITTY_LAUNCHER_REFRESH_MESSAGE "KiTTYLauncherRefreshSessionsAndHotkeys"
#ifndef MOD_NOREPEAT
#define MOD_NOREPEAT 0x4000
#endif
int RunSession( HWND hwnd, const char * folder_in, char * session_in ) ;

/* KiTTY: runtime registry base (windows/storage.c). The launcher must read
 * KiTTY's own hive (Software\9bis.com\KiTTY) -- where sessions actually live --
 * not the compile-time PUTTY_REG_POS macro (stock PuTTY's SimonTatham hive).
 * In the -launcher process nothing calls kitty_set_registry_root(), so this
 * returns the default KiTTY base; the launcher therefore does NOT follow
 * KiClassName=PuTTY mode -- deliberate, acceptable for the launcher. */
extern const char *kitty_registry_base( void ) ;

#include "kitty_startup_shortcut.h"   /* KiTTY: on-request Startup-folder shortcut */
#include "kitty_workplace.h"          /* KiTTY: workplace proxy mode arming */
#include "kitty_notice.h"           /* KiTTY: our own arm/disarm notice window */

static HMENU MenuLauncher = NULL ;
static HMENU HideMenu ;
static int LauncherConfReload = 1 ;
static HBITMAP bmpCheck, bmpUnCheck ;
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

// Gestion Hide/UnHide all
static struct THWin { HWND hwnd ; char name[128] ; } TabWin[100] ;
static int oldIconFlag = 0 ;
static int NbWin = 0 ;
static int IsUnique = 0 ;
int RefreshWinList( HWND hwnd ) ;

#ifndef OBM_CHECKBOXES
#define OBM_CHECKBOXES 32759
#endif

// Creation de bitmap coche
HBITMAP GetMyCheckBitmaps(UINT fuCheck) 
{ 
    COLORREF crBackground;  // background color                  
    HBRUSH hbrBackground;   // background brush                  
    HBRUSH hbrTargetOld;    // original background brush         
    HDC hdcSource;          // source device context             
    HDC hdcTarget;          // target device context             
    HBITMAP hbmpCheckboxes; // handle to check-box bitmap        
    BITMAP bmCheckbox;      // structure for bitmap data         
    HBITMAP hbmpSourceOld;  // handle to original source bitmap  
    HBITMAP hbmpTargetOld;  // handle to original target bitmap  
    HBITMAP hbmpCheck;      // handle to check-mark bitmap       
    RECT rc;                // rectangle for check-box bitmap    
    WORD wBitmapX;          // width of check-mark bitmap        
    WORD wBitmapY;          // height of check-mark bitmap       
 
    // Get the menu background color and create a solid brush 
    // with that color. 
 
    crBackground = GetSysColor(COLOR_MENU); 
    hbrBackground = CreateSolidBrush(crBackground); 
 
    // Create memory device contexts for the source and 
    // destination bitmaps. 
 
    hdcSource = CreateCompatibleDC((HDC) NULL); 
    hdcTarget = CreateCompatibleDC(hdcSource); 
 
    // Get the size of the system default check-mark bitmap and 
    // create a compatible bitmap of the same size. 
 
    wBitmapX = GetSystemMetrics(SM_CXMENUCHECK); 
    wBitmapY = GetSystemMetrics(SM_CYMENUCHECK); 
 
    hbmpCheck = CreateCompatibleBitmap(hdcSource, wBitmapX, 
        wBitmapY); 
 
    // Select the background brush and bitmap into the target DC. 
 
    hbrTargetOld = SelectObject(hdcTarget, hbrBackground); 
    hbmpTargetOld = SelectObject(hdcTarget, hbmpCheck); 
 
    // Use the selected brush to initialize the background color 
    // of the bitmap in the target device context. 
 
    PatBlt(hdcTarget, 0, 0, wBitmapX, wBitmapY, PATCOPY); 
 
    // Load the predefined check box bitmaps and select it 
    // into the source DC. 
 
    hbmpCheckboxes = LoadBitmap((HINSTANCE) NULL, 
        (LPTSTR) OBM_CHECKBOXES); 
 
    hbmpSourceOld = SelectObject(hdcSource, hbmpCheckboxes); 
 
    // Fill a BITMAP structure with information about the 
    // check box bitmaps, and then find the upper-left corner of 
    // the unchecked check box or the checked check box. 
 
    GetObject(hbmpCheckboxes, sizeof(BITMAP), &bmCheckbox); 
 
    if (fuCheck == 2 /*UNCHECK*/) 
    { 
        rc.left = 0; 
        rc.right = (bmCheckbox.bmWidth / 4); 
    } 
    else 
    { 
        rc.left = (bmCheckbox.bmWidth / 4); 
        rc.right = (bmCheckbox.bmWidth / 4) * 2; 
    } 
 
    rc.top = 0; 
    rc.bottom = (bmCheckbox.bmHeight / 3); 
 
    // Copy the appropriate bitmap into the target DC. If the 
    // check-box bitmap is larger than the default check-mark 
    // bitmap, use StretchBlt to make it fit; otherwise, just 
    // copy it. 
 
    if (((rc.right - rc.left) > (int) wBitmapX) || 
            ((rc.bottom - rc.top) > (int) wBitmapY)) 
    {
        StretchBlt(hdcTarget, 0, 0, wBitmapX, wBitmapY, 
            hdcSource, rc.left, rc.top, rc.right - rc.left, 
            rc.bottom - rc.top, SRCCOPY); 
    }
 
    else 
    {
        BitBlt(hdcTarget, 0, 0, rc.right - rc.left, 
            rc.bottom - rc.top, 
            hdcSource, rc.left, rc.top, SRCCOPY); 
    }
 
    // Select the old source and destination bitmaps into the 
    // source and destination DCs, and then delete the DCs and 
    // the background brush. 
 
    SelectObject(hdcSource, hbmpSourceOld); 
    SelectObject(hdcTarget, hbrTargetOld); 
    hbmpCheck = SelectObject(hdcTarget, hbmpTargetOld); 
 
    DeleteObject(hbrBackground); 
    DeleteObject(hdcSource); 
    DeleteObject(hdcTarget); 
 
    // Return a handle to the new check-mark bitmap.  
 
    return hbmpCheck; 
} 

// Procedure de creation de menu à partir d'une clé de registre
HMENU InitLauncherMenu( char * Key ) {
	HMENU menu ;
	menu = CreatePopupMenu() ;
	char KeyName[1024] ;
	int nbitem = 0,i ;
	
	DeleteObject( bmpCheck ) ; bmpCheck = GetMyCheckBitmaps( 1 ) ;
	DeleteObject( bmpUnCheck ) ; bmpUnCheck = GetMyCheckBitmaps( 2 ) ;
	
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
		snprintf( upmsg, sizeof(upmsg), "Update available: KiTTY %s%s - install...",
		          LauncherUpdateLatest, LauncherUpdateBeta ? " (beta)" : "" ) ;
		AppendMenu( menu, MF_ENABLED, IDM_LAUNCHER+9, upmsg ) ;
		AppendMenu( menu, MF_SEPARATOR, 0, 0 ) ;
	}

	// Creation du menu bouton gauche
	DestroyMenu( HideMenu ) ;
	HideMenu = CreatePopupMenu() ;
	if( !IsUnique ) {
		AppendMenu( HideMenu, MF_ENABLED, IDM_LAUNCHER+3, "&Hide all" ) ;
		AppendMenu( HideMenu, MF_ENABLED, IDM_LAUNCHER+4, "&Unhide all" ) ;
		//AppendMenu( HideMenu, MF_ENABLED, IDM_LAUNCHER+5, "&Refresh list" ) ;
		AppendMenu( HideMenu, MF_ENABLED, IDM_LAUNCHER+6, "&Window unique" ) ;
		CheckMenuItem( HideMenu, IDM_LAUNCHER+6, MF_BYCOMMAND | MF_UNCHECKED) ;
	} else {
		AppendMenu( HideMenu, MF_ENABLED, IDM_LAUNCHER+6, "&Window unique" ) ;
		CheckMenuItem( HideMenu, IDM_LAUNCHER+6, MF_BYCOMMAND | MF_CHECKED) ;
	}
	//AppendMenu( HideMenu, MF_ENABLED, IDM_GONEXT, "&Next" ) ;
	//AppendMenu( HideMenu, MF_ENABLED, IDM_GOPREVIOUS, "&Previous" ) ;
	if( RefreshWinList( MainHwnd ) > 0 ) {
		AppendMenu( HideMenu, MF_SEPARATOR, 0, 0 ) ;
		for( i=0 ; i<NbWin ; i++ ) {
			AppendMenu( HideMenu, MF_ENABLED, IDM_GOHIDE+i, TabWin[i].name ) ;
			SetMenuItemBitmaps ( HideMenu, IDM_GOHIDE+i, MF_BYCOMMAND, bmpUnCheck, bmpCheck ) ;
			if( IsWindowVisible( TabWin[i].hwnd ) ) 
				CheckMenuItem( HideMenu, IDM_GOHIDE+i, MF_BYCOMMAND | MF_CHECKED) ;
			else 
				CheckMenuItem( HideMenu, IDM_GOHIDE+i, MF_BYCOMMAND | MF_UNCHECKED) ;
		}
	}

	
	AppendMenu( menu, MF_POPUP, (UINT_PTR)HideMenu, "&Opened sessions" ) ;
	AppendMenu( menu, MF_SEPARATOR, 0, 0 ) ;
	
	AppendMenu( menu, MF_ENABLED, IDM_LAUNCHER+7, "&Refresh" ) ;
	AppendMenu( menu, MF_ENABLED, IDM_LAUNCHER+1, "&Configuration" ) ;
	AppendMenu( menu, MF_ENABLED, IDM_LAUNCHER+2, "&TTY-ed" ) ;
	/* KiTTY: workplace proxy mode. While it is on, ONE item that says which
	 * proxy everything is going through and switches it off; while it is off, a
	 * submenu of the named proxies to switch it on with, the remembered one
	 * ticked. The wording says "every connection" both ways round because that
	 * is the whole point of the mode, and because a proxy override left on by
	 * accident is the risk this feature has to keep visible
	 * (design/TASK_workplace_proxy.md §6). */
	{
		AppendMenu( menu, MF_SEPARATOR, 0, 0 ) ;
		if( kitty_workplace_holding() ) {
			char item[400], left[64] ;
			kitty_workplace_left_text( left, sizeof(left) ) ;
			if( left[0] )
				snprintf( item, sizeof(item),
					"&Workplace proxy ON: \"%.200s\" for another %s - switch off now",
					LauncherWorkplaceProxy, left ) ;
			else
				snprintf( item, sizeof(item),
					"&Workplace proxy ON: every connection uses \"%.200s\" - switch off",
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
					snprintf( item, sizeof(item), "%.250s (last used)", proxies[i].name ) ;
				else
					snprintf( item, sizeof(item), "%.250s", proxies[i].name ) ;
				AppendMenu( wpmenu, MF_ENABLED, IDM_WORKPLACE+1+(i-2), item ) ;
				n++ ;
			}
			if( n > 0 ) {
				AppendMenu( menu, MF_POPUP, (UINT_PTR)wpmenu,
					"&Workplace proxy mode (off) - use one proxy for everything" ) ;
			} else {
				/* No named proxies: say why rather than offer an empty submenu. */
				DestroyMenu( wpmenu ) ;
				AppendMenu( menu, MF_GRAYED, 0,
					"Workplace proxy mode (needs a named proxy)" ) ;
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
	              IDM_LAUNCHER+8, "Start &at login" ) ; }
	AppendMenu( menu, MF_SEPARATOR, 0, 0 ) ;
	AppendMenu( menu, MF_ENABLED, IDM_ABOUT, "&About" ) ;
	AppendMenu( menu, MF_ENABLED, IDM_QUIT, "E&xit" ) ;

	return menu ;
}

void RefreshMenuLauncher( void ) {
	DestroyMenu( MenuLauncher ) ; 
	MenuLauncher = NULL ;
	MenuLauncher = InitLauncherMenu( "Launcher" ) ;
}
	
// Nettoie les noms de folder en remplaçant les "/" par des "\" et les " \ " par des " \"
// Deplace dans kitty_commun.c
/*
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
*/

// Supprime une arborescence   ==> deplace dans kitty_commun.c
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

// Initialise l'arborescence Launcher en mode savemode=dir avec arborescence
void InitLauncherDir( const char * directory ) {
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
		MessageBox(NULL,"Unable to create the menu launcher directory","Error",MB_OK|MB_ICONERROR); 
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

// Inititalise la clé de registre Launcher avec les sessions enregistrées
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
						if( RegGetValueA( HKEY_CURRENT_USER, skey, "LauncherHide", RRF_RT_REG_DWORD, NULL, &hide, &hsz ) == ERROR_SUCCESS && hide )
							continue ;
					}
					snprintf( buffer, sizeof(buffer),"%s\\Sessions\\%s", kitty_registry_base(), lpData ) ;
					if( !GetValueDataN(HKEY_CURRENT_USER, buffer, "Folder", folder, sizeof(folder) ) )
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
		if(!MakeDir( fullpath ) ) { MessageBox(NULL,"Unable to create the menu launcher directory","Error",MB_OK|MB_ICONERROR); }
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
		if( !MakeDir( fullpath ) ) { MessageBox(NULL,"Unable to create the menu launcher directory","Error",MB_OK|MB_ICONERROR); }
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
static LRESULT CALLBACK LauncherMenuMsgFilter( int code, WPARAM wParam, LPARAM lParam )
{
	if( code == MSGF_MENU ) {
		MSG *m = (MSG *)lParam ;
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
	}
	return CallNextHookEx( g_launcher_menu_hook, code, wParam, lParam ) ;
}

static void DisplayContextMenuAt( HWND hwnd, HMENU menu, POINT pt ) {
	HMENU hMenuPopup = menu ;

	SetForegroundWindow( hwnd ) ;
	g_launcher_menu_hook = SetWindowsHookEx( WH_MSGFILTER, LauncherMenuMsgFilter,
	                                         NULL, GetCurrentThreadId() ) ;
	TrackPopupMenu (hMenuPopup, TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, NULL);
	if( g_launcher_menu_hook ) {
		UnhookWindowsHookEx( g_launcher_menu_hook ) ;
		g_launcher_menu_hook = NULL ;
	}
}

void DisplayContextMenu( HWND hwnd, HMENU menu ) {
	GetCursorPos (&LauncherMenuPoint);
	LauncherMenuPointValid = 1 ;
	DisplayContextMenuAt( hwnd, menu, LauncherMenuPoint ) ;
}
	
// Gestion Hide/UnHide all
static int CurrentVisibleWin = -1 ; /* -1 = toutes visibles */

void ManageHideOne( HWND hwnd ) { PostMessage( hwnd, WM_COMMAND, IDM_HIDE, 0 ) ; }
void ManageUnHideOne( HWND hwnd ) { PostMessage( hwnd, WM_COMMAND, IDM_UNHIDE, 0 ) ; }

BOOL CALLBACK RefreshWinListProc( HWND hwnd, LPARAM lParam ) {
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
	
void GoNext( HWND hwnd ) {
	int i ;
	if( RefreshWinList( hwnd ) > 1 ) 
	for( i=0 ; i<NbWin ; i++ ) {
		if( hwnd == TabWin[i].hwnd ) {
			ManageHideOne( hwnd ) ;
			if( i == (NbWin-1) ) {
				ManageUnHideOne( TabWin[0].hwnd ) ;
				SetFocus( TabWin[0].hwnd ) ;
			} else {
				ManageUnHideOne( TabWin[i+1].hwnd ) ;
				SetFocus( TabWin[i+1].hwnd ) ;
			}
			break ;
		}
	}
}

void GoPrevious( HWND hwnd ) {
	int i ;
	if( RefreshWinList( hwnd ) > 1 ) 
	for( i=0 ; i<NbWin ; i++ ) {
		if( hwnd == TabWin[i].hwnd ) {
			ManageHideOne( hwnd ) ;
			if( i == 0 ) {
				ManageUnHideOne( TabWin[NbWin-1].hwnd ) ;
				SetFocus( TabWin[NbWin-1].hwnd ) ;
			} else {
				ManageUnHideOne( TabWin[i-1].hwnd ) ;
				SetFocus( TabWin[i-1].hwnd ) ;
			}
			break ;
		}
	}
}

void ManageHideAll( HWND hwnd ) {
	int i ;
	if( RefreshWinList( hwnd ) > 0 ) {
		for( i=0 ; i<NbWin ; i++ ) {
			ManageHideOne( TabWin[i].hwnd ) ;
		}
	}
	CurrentVisibleWin = 0 ;
}

void ManageUnHideAll( HWND hwnd ) {
	int i ;
	if( RefreshWinList( hwnd ) > 0 ) {
		for( i=0 ; i<NbWin ; i++ ) ManageUnHideOne( TabWin[i].hwnd ) ;
	}
	CurrentVisibleWin = -1 ;
}
	
void ManageGoNext( HWND hwnd ) {
	if( CurrentVisibleWin == -1 ) return ;
	ManageHideOne( TabWin[CurrentVisibleWin].hwnd ) ;
	CurrentVisibleWin++ ;
	if( CurrentVisibleWin>=NbWin ) CurrentVisibleWin=0 ;
	ManageUnHideOne( TabWin[CurrentVisibleWin].hwnd ) ;
}

void ManageGoPrevious( HWND hwnd ) {
	if( CurrentVisibleWin == -1 ) return ;
	ManageHideOne( TabWin[CurrentVisibleWin].hwnd ) ;
	CurrentVisibleWin-- ;
	if( CurrentVisibleWin<0 ) CurrentVisibleWin=NbWin-1 ;
	ManageUnHideOne( TabWin[CurrentVisibleWin].hwnd ) ;
}
	
void ManageGo( const int n ) {
	if( CurrentVisibleWin == -1 ) return ;
	if( (n<0)||(n>=100) ) return ;
	ManageHideOne( TabWin[CurrentVisibleWin].hwnd ) ;
	CurrentVisibleWin = n ;
	ManageUnHideOne( TabWin[CurrentVisibleWin].hwnd ) ;
}
	
void ManageSwitch( const int n ) { 
	SendMessage( TabWin[n].hwnd, WM_COMMAND, IDM_SWITCH_HIDE, 0 ) ; 
	SetForegroundWindow( TabWin[n].hwnd ) ;
	SetFocus( TabWin[n].hwnd ) ;
}

static void ShowLauncherUpdateBalloon( void ) {
	extern int kitty_update_available(char*,int,char*,int,int*) ;
	char ulatest[64]="" ; int ubeta=0 ;
	if( kitty_update_available( ulatest, sizeof(ulatest), NULL, 0, &ubeta ) ) {
		char umsg[256] ;
		LauncherUpdateKnown = 1 ;
		LauncherUpdateBeta = ubeta ;
		strncpy( LauncherUpdateLatest, ulatest, sizeof(LauncherUpdateLatest)-1 ) ;
		LauncherUpdateLatest[sizeof(LauncherUpdateLatest)-1] = '\0' ;
		snprintf( umsg, sizeof(umsg),
			"KiTTY %s is available%s.\nClick here to install it.",
			ulatest, ubeta ? " (beta)" : "" ) ;
		/* Tooltip and menu entry are refreshed on every call; the BALLOON is
		 * raised once. This function runs twice per launcher run - once on the
		 * cached answer, once when the async check returns - and used to pop a
		 * second balloon for the same news. */
		if( LauncherUpdateBalloonShown ) {
			snprintf( TrayIcone.szTip, sizeof(TrayIcone.szTip),
			          "KiTTY Launcher - update %s%s available",
			          ulatest, ubeta ? " beta" : "" ) ;
			TrayIcone.uFlags = NIF_TIP ;
			Shell_NotifyIcon( NIM_MODIFY, &TrayIcone ) ;
			TrayIcone.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE ;
			return ;
		}
		LauncherUpdateBalloonShown = 1 ;
		TrayIcone.uFlags = NIF_INFO | NIF_TIP ;
		TrayIcone.dwInfoFlags = NIIF_INFO ;
		TrayIcone.uTimeout = 10000 ;
		snprintf( TrayIcone.szTip, sizeof(TrayIcone.szTip),
		          "KiTTY Launcher - update %s%s available",
		          ulatest, ubeta ? " beta" : "" ) ;
		strncpy( TrayIcone.szInfoTitle, "KiTTY update available", sizeof(TrayIcone.szInfoTitle) ) ;
		TrayIcone.szInfoTitle[sizeof(TrayIcone.szInfoTitle)-1] = '\0' ;
		strncpy( TrayIcone.szInfo, umsg, sizeof(TrayIcone.szInfo) ) ;
		TrayIcone.szInfo[sizeof(TrayIcone.szInfo)-1] = '\0' ;
		Shell_NotifyIcon( NIM_MODIFY, &TrayIcone ) ;
		TrayIcone.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE ;
	}
}

static char *launcher_trim( char *s ) {
	char *e ;
	while( *s==' ' || *s=='\t' ) s++ ;
	e = s + strlen(s) ;
	while( e>s && (e[-1]==' ' || e[-1]=='\t' || e[-1]=='\r' || e[-1]=='\n') ) *--e='\0' ;
	return s ;
}

static int launcher_parse_hotkey( char *spec, UINT *mods, UINT *vk ) {
	char *hotkey, *tok, *keytok = NULL ;
	*mods = 0 ; *vk = 0 ;
	hotkey = launcher_trim( spec ) ;
	if( hotkey[0]=='\0' ) return 0 ;
	for( tok = strtok( hotkey, "+" ) ; tok != NULL ; tok = strtok( NULL, "+" ) ) {
		tok = launcher_trim( tok ) ;
		if( !stricmp(tok,"Ctrl") || !stricmp(tok,"Control") ) *mods |= MOD_CONTROL ;
		else if( !stricmp(tok,"Shift") ) *mods |= MOD_SHIFT ;
		else if( !stricmp(tok,"Alt") ) *mods |= MOD_ALT ;
		else if( !stricmp(tok,"Win") || !stricmp(tok,"Windows") ) *mods |= MOD_WIN ;
		else keytok = tok ;
	}
	if( keytok == NULL ) return 0 ;
	if( strlen(keytok)==1 ) {
		char c = keytok[0] ;
		if( c>='a' && c<='z' ) c = (char)(c-'a'+'A') ;
		if( (c>='A'&&c<='Z') || (c>='0'&&c<='9') ) *vk = (UINT)c ;
	} else if( (keytok[0]=='F' || keytok[0]=='f') && keytok[1]>='1' && keytok[1]<='9' ) {
		int n = atoi( keytok+1 ) ;
		if( n>=1 && n<=24 ) *vk = VK_F1 + n - 1 ;
	}
	return (*mods != 0 && *vk != 0) ;
}

static void LauncherUnregisterHotkeys( HWND hwnd ) {
	int i ;
	for( i=0 ; i<LauncherHotkeyCount ; i++ )
		UnregisterHotKey( hwnd, LauncherHotkeys[i].id ) ;
	LauncherHotkeyCount = 0 ;
}

static void LauncherRegisterHotkeys( HWND hwnd ) {
	char work[256] ;
	int i ; UINT mods, vk ;
	LauncherUnregisterHotkeys( hwnd ) ;
	for( i=0 ; i<NB_MENU_MAX && LauncherHotkeyCount<LAUNCHER_HOTKEY_MAX ; i++ ) {
		Conf *c ;
		const char *hotkey ;
		if( SpecialMenu[i] == NULL || SpecialMenu[i][0] == '\0' ) continue ;
		c = conf_new() ;
		if( c == NULL ) continue ;
		if( do_defaults( SpecialMenu[i], c ) &&
		    conf_get_bool( c, CONF_launcher_global_hotkey_enabled ) ) {
			hotkey = conf_get_str( c, CONF_launcher_global_hotkey ) ;
			strncpy( work, hotkey, sizeof(work)-1 ) ; work[sizeof(work)-1]='\0' ;
			if( launcher_parse_hotkey( work, &mods, &vk ) ) {
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
			}
		}
		conf_free( c ) ;
	}
}

static void LauncherRefreshSessionsAndHotkeys( HWND hwnd ) {
	if( LauncherConfReload ) InitLauncherRegistry() ;
	RefreshMenuLauncher() ;
	LauncherRegisterHotkeys( hwnd ) ;
}
	
/* KiTTY: the tray tooltip, built in one place because it now has a part that
 * changes at runtime - workplace proxy mode names the proxy every connection is
 * going through while this launcher holds the arming
 * (design/TASK_workplace_proxy.md §6). Safe to call before the icon exists;
 * NIM_MODIFY on an unregistered icon simply fails. */
static void LauncherSetTrayTip( void ) {
#ifdef MOD_PORTABLE
	strcpy( TrayIcone.szTip, "KiTTY Launcher\r\n(portable)" ) ;
#else
	strcpy( TrayIcone.szTip, "KiTTY Launcher" ) ;
#endif
	/* KiTTY: say so when this launcher - and so every session it starts, via
	 * the "&R" prefix - runs with the restricted ACL. */
	if( restricted_acl() ) strcat( TrayIcone.szTip, "\r\n(RESTRICTED)" ) ;
	if( kitty_workplace_holding() && LauncherWorkplaceProxy[0] ) {
		char line[220], left[64] ;
		kitty_workplace_left_text( left, sizeof(left) ) ;
		snprintf( line, sizeof(line), "\r\nWorkplace proxy: %.100s%s%s",
			LauncherWorkplaceProxy, left[0] ? "\r\nSwitches off in " : "", left ) ;
		if( strlen(TrayIcone.szTip) + strlen(line) < sizeof(TrayIcone.szTip) )
			strcat( TrayIcone.szTip, line ) ;
	}
}

/* ⚠️ The SELECTION is remembered, the ARMED state never is
 * (design/TASK_workplace_proxy.md §3). Which proxy was last chosen is written
 * here so a later launcher start can offer to switch the mode back on; "armed"
 * exists only as this process holding the arming, and switching the mode off
 * deliberately KEEPS the selection - it is what you would want back tomorrow
 * morning. Key spelling matters: mini.c matches ini keys case-sensitively. */
#define WORKPLACE_PROXY_KEY   "WorkplaceProxy"
#define WORKPLACE_MINUTES_KEY "WorkplaceMinutes"

static void LauncherRememberWorkplaceProxy( const char *proxyname, unsigned int minutes ) {
	char m[32] ;
	WriteParameter( INIT_SECTION, WORKPLACE_PROXY_KEY, (char*)proxyname ) ;
	snprintf( m, sizeof(m), "%u", minutes ) ;
	WriteParameter( INIT_SECTION, WORKPLACE_MINUTES_KEY, m ) ;
}

/* How long the mode was last switched on for; 0 = until it is switched off or
 * the launcher exits. Remembered with the proxy, so switching it on from the
 * tray repeats the choice made in the config box. */
static unsigned int LauncherRememberedWorkplaceMinutes( void ) {
	char buffer[32] = "" ;
	if( !ReadParameterN( INIT_SECTION, WORKPLACE_MINUTES_KEY, buffer, sizeof(buffer) ) ) return 0 ;
	if( atoi(buffer) <= 0 ) return 0 ;
	return (unsigned int)atoi(buffer) ;
}

static int LauncherRememberedWorkplaceProxy( char *out, int len ) {
	char buffer[512] = "" ;
	out[0] = '\0' ;
	if( !ReadParameterN( INIT_SECTION, WORKPLACE_PROXY_KEY, buffer, sizeof(buffer) ) ) return 0 ;
	if( !buffer[0] || (int)strlen(buffer) >= len ) return 0 ;
	strcpy( out, buffer ) ;
	return 1 ;
}

/* How long the workplace notice stays up: [Launcher] noticeseconds, default 15.
 * Ours to choose, which is half the reason the notice is our own window - the
 * shell has ignored the requested balloon duration since Vista. */
static int LauncherNoticeSeconds( void ) {
	char buffer[32] ;
	if( ReadParameterN( "Launcher", "noticeseconds", buffer, sizeof(buffer) )
	    && atoi(buffer) > 0 ) return atoi(buffer) ;
	return 15 ;
}

/* Dark green, the same colour the terminal frame uses while a connection is
 * going through the mode's proxy, so the notice and the window read as one
 * thing (design §7a). */
#define WORKPLACE_GREEN RGB(0,100,0)

static void LauncherWorkplaceBalloon( int on, int by_timeout ) {
	char msg[512] ;
	if( on ) {
		char left[64] ;
		kitty_workplace_left_text( left, sizeof(left) ) ;
		if( left[0] )
			snprintf( msg, sizeof(msg),
				"Every connection now uses the proxy \"%.200s\", whatever each "
				"session says. Switches off in %s, or when this launcher stops.",
				LauncherWorkplaceProxy, left ) ;
		else
			snprintf( msg, sizeof(msg),
				"Every connection now uses the proxy \"%.200s\", whatever each "
				"session says. It stays on until you switch it off or this "
				"launcher stops.", LauncherWorkplaceProxy ) ;
	}
	else if( by_timeout )
		snprintf( msg, sizeof(msg),
			"The time you set for workplace proxy mode has run out, so it is off. "
			"New connections use each session's own proxy settings again. Click "
			"here to switch it on again with \"%.200s\".", LauncherRearmProxy ) ;
	else
		snprintf( msg, sizeof(msg),
			"Workplace proxy mode is off. New connections use each session's own "
			"proxy settings again; connections already open keep the proxy they "
			"connected through." ) ;
	/* Our own window, not a tray balloon: it carries the mode's colour and a
	 * duration we choose, and it is not silently swallowed by focus assist the
	 * way balloons are. The tray tooltip and menu still say the same thing, so
	 * a notice that is missed is never the only record. */
	LauncherSetTrayTip() ;
	Shell_NotifyIcon( NIM_MODIFY, &TrayIcone ) ;
	/* ⚠️ Only the TIMEOUT notice offers to switch the mode back on. When the
	 * user switched it off themselves they have said what they want, and a
	 * one-click undo in front of them invites the opposite; a timeout is the
	 * case where the mode ended without them deciding anything. */
	kitty_notice_show( on ? "Workplace proxy mode is ON"
	                      : (by_timeout ? "Workplace proxy mode has timed out"
	                                    : "Workplace proxy mode is OFF"),
	                   msg, WORKPLACE_GREEN, LauncherNoticeSeconds(),
	                   (!on && by_timeout) ? MainHwnd : NULL,
	                   (!on && by_timeout) ? KLWM_WORKPLACEREARM : 0 ) ;
	/* The user has now been told, whichever way it ended, so no later start owes
	 * them the "it is not active" notice. */
	if( !on ) kitty_workplace_notice_settled() ;
}

/* Take the arming for workplace proxy mode. From here on every connection this
 * install starts asks us, and gets this proxy until we let go or die. */
int LauncherArmWorkplace( const char *proxyname, unsigned int minutes ) {
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

void LauncherDisarmWorkplace( void ) {
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
 * ⚠️ It applies ONLY to a launcher that the mode itself started. A launcher the
 * user was already running must never be closed by switching a proxy mode off -
 * that would take their session list away as a side effect. */
static int LauncherStartedForWorkplace = 0 ;

static void LauncherExitIfStartedForWorkplace( HWND hwnd ) {
	char buffer[32] ;
	if( !LauncherStartedForWorkplace ) return ;
	if( ReadParameterN( "Launcher", "exitwithworkplace", buffer, sizeof(buffer) )
	    && !stricmp( buffer, "no" ) ) return ;   /* absent = yes */
	/* ⚠️ NOT straight away. The notice saying the mode is off is a window of
	 * OURS, so quitting here would take it off the screen the instant it
	 * appeared - and on the timeout path it is the notice that offers the mode
	 * back with a click, so quitting would remove the offer as well as the news.
	 * Wait for the notice to have had its time, then go. */
	SetTimer( hwnd, LAUNCHER_EXITAFTERNOTICE_TIMER,
	          (UINT)(LauncherNoticeSeconds()*1000 + 1000), NULL ) ;
}

// Procedures principales du launcher

LRESULT CALLBACK Launcher_WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
	int ResShell ;
	static UINT s_uTaskbarRestart;

	if( LauncherRefreshMessage != 0 && uMsg == LauncherRefreshMessage ) {
		LauncherRefreshSessionsAndHotkeys( hwnd ) ;
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
        
	// Initialisation de la structure NOTIFYICONDATA
	TrayIcone.cbSize = sizeof(TrayIcone);	// On alloue la taille nécessaire pour la structure
	if( oldIconFlag ) {
		TrayIcone.uID = IDI_BLACKBALL ;	// On lui donne un ID
		TrayIcone.hIcon = LoadIcon((HINSTANCE) GetModuleHandle (NULL), MAKEINTRESOURCE(IDI_BLACKBALL));
	} else {
		TrayIcone.uID = IDI_PUTTY_LAUNCH ;	// On lui donne un ID
		TrayIcone.hIcon = LoadIcon((HINSTANCE) GetModuleHandle (NULL), MAKEINTRESOURCE(IDI_PUTTY_LAUNCH));
	}
	TrayIcone.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;	// On lui indique les champs valables
	// On lui dit qu'il devra "écouter" son environement (clique de souris, etc)
	TrayIcone.uCallbackMessage = KLWM_NOTIFYICON;
	//TrayIcone.szTip[1024] = "KiTTY That\'s all folks!\0" ;			// Le tooltip par défaut, soit rien
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
			extern void kitty_start_update_check_notify(HWND,UINT) ;
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
		LauncherRegisterHotkeys( hwnd ) ;
		if (IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_HIDE);
		//SendMessage(hwnd, WM_SYSCOMMAND, SC_MINIMIZE, 0);
		return 1 ;
	} else 
		return 0 ;
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
					/* The update balloon is the only balloon left: the workplace
					 * notices are our own window, which handles its own click. */
					if( LauncherUpdateKnown ) {
						extern void CheckVersionFromWebSite( HWND hwnd, int is_terminal ) ;
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
			if( wParam == LAUNCHER_TRAYCLICK_TIMER ) {
				KillTimer( hwnd, LAUNCHER_TRAYCLICK_TIMER ) ;
				RefreshMenuLauncher() ;
				LauncherMenuPoint = LauncherClickPoint ;
				LauncherMenuPointValid = 1 ;
				DisplayContextMenuAt( hwnd, MenuLauncher, LauncherMenuPoint ) ;
			}
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
		case WM_COMMAND: {//Commandes du menu
			switch( LOWORD(wParam) ) {
				case IDM_ABOUT: {
					/* UTF-8 source (real "(c)" and em-dash); MessageBoxW renders it
					 * as Unicode regardless of the system ANSI codepage, so the
					 * earlier mojibake (Â© / "a\200\224") cannot recur. */
					const char *ab =
						"KiTTY Launcher " BUILD_VERSION "\r\n"
#ifdef KITTY_TEST_BUILD_LABEL
						"TEST BUILD: " KITTY_TEST_BUILD_LABEL "\r\n"
#endif
						"\r\nQuick-launch for your saved KiTTY sessions, from the system tray.\r\n"
						"Part of the KiTTY suite \xe2\x80\x94 a fork of PuTTY 0.84.\r\n\r\n"
						"\xc2\xa9 KAPPER NETWORK-COMMUNICATIONS GmbH\r\n"
						"Based on KiTTY by Cyril Dupont and PuTTY by Simon Tatham." ;
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
							    "This KiTTY already starts at login for all users "
							    "(an all-users Startup shortcut, usually placed by "
							    "the installer). To stop it, disable it in Settings "
									    "> Apps > Startup (that leaves the shortcut in place "
									    "but prevents it running); deleting the shortcut "
									    "itself needs administrator access to the all-users "
									    "Startup folder.",
							    "KiTTY Launcher", MB_ICONINFORMATION | MB_OK ) ;
						} else if( kitty_startup_shortcut_exists("KiTTY Launcher") ) {
							MessageBox( hwnd,
							    "A \"KiTTY Launcher\" startup shortcut for a different "
							    "KiTTY already exists in your Startup folder, so none "
							    "was added.\n\nDelete it from your Startup folder "
									    "(open shell:startup) first if you want THIS KiTTY to "
									    "start at login - disabling it in Settings does not "
									    "remove the file, and its name would still clash.",
							    "KiTTY Launcher", MB_ICONINFORMATION | MB_OK ) ;
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
					LauncherRegisterHotkeys( hwnd ) ;
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
					 * ⚠️ And this launcher STAYS, even when the mode started it
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
		default: // Message par défaut
			if( uMsg == s_uTaskbarRestart ) { // On reaffiche l'icone après un crash de l'explorateur windows
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
	if( ReadParameterN( "Launcher", "classname", buffer, sizeof(buffer) ) ) {
		buffer[1023]='\0' ;
		if( strlen(buffer)>0 ) { strcpy(className,buffer) ; }
	}
	
	if( strstr( cmdline, "-putty" ) != NULL ) SetPuttyFlag(1) ;

	/* KiTTY: -workplace <named proxy> switches workplace proxy mode ON and
	 * hands this launcher the arming (design/TASK_workplace_proxy.md §3). The
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
		if( ReadParameterN( "Launcher", "alreadyRunCheck", buffer, sizeof(buffer) ) ) {
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

	if( ReadParameterN( "Launcher", "reload", buffer, sizeof(buffer) ) ) {
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
	
	/* Pass the session conf (incl. the auto-login password) to the child putty
	 * process PLAINTEXT through the anonymous, inherit-only file-mapping below -
	 * exactly as the Duplicate-Session handoff (window.c IDM_DUPSESS) already does.
	 * The old code MASKPASS-obfuscated the password here on the assumption the
	 * child would un-mask it, but the 0.84 child reads CONF_password raw
	 * (window.c get_userpass_input) -> it sent the masked bytes -> auto-login and
	 * WinSCP launches failed for every stored password. The mapping is anonymous
	 * and only shared by handle-inheritance with our own child, so plaintext here
	 * is no weaker than the password already being plaintext in process memory. */

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

	serbuf = strbuf_new();
	conf_serialise(BinarySink_UPCAST(serbuf), conf);
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
	  extern HANDLE kitty_mpw_export_inherit_blob(const char*, char*, size_t);
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
				  extern HANDLE kitty_mpw_export_inherit_blob(const char*, char*, size_t);
				  kitty_mpw_startup_unlock();
				  mpwmap = kitty_mpw_export_inherit_blob(" -mpwkey ", mpwtok, sizeof(mpwtok)); }
				snprintf( buffer, sizeof(buffer), "%s%s%s", shortname, aclprefix, mpwtok ) ;
				launcher_run_session_cmd( hwnd, buffer, mpwmap ) ;
				if( mpwmap ) CloseHandle( mpwmap ) ;
			}
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
	  extern HANDLE kitty_mpw_export_inherit_blob(const char*, char*, size_t);
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
