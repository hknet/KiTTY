/*
 * kitty_broadcast.c - the other KiTTY windows: the count of them, sending a
 * command or keystrokes to every one (the broadcast gate, its group and its
 * send key), the auto-command after login and the "same size" resize of
 * every window. Also the RuTTY server DLL hook.
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
#include "kitty_text.h"   /* shared captions and wordings (also for the .c files included below) */
#include "kitty_inikeys.h"   /* KI_*: the kitty.ini key names */
#include "kitty_notes.h"   /* the application notification, marked owed at startup */
#include "kitty_pwmem.h"   /* passwords wrapped in memory (kitty_commands.c too) */
#include "kitty_storage.h"
#include "kitty_gui.h"
#include "kitty_bridge.h"
#include "kitty_exportbundle.h"
#include "mini/mini.h"
#include "kitty_b64.h"
#include "kitty_store.h"
#include "kitty_broadcast.h"
#include "kitty_int.h"   /* MainHwnd */
// Count of the KiTTY windows currently open
static int NbWindows = 0 ;


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
		MessageBox( MainHwnd, KT_MAIN_KCHAT_LIB_FAILED, KT_CAP_ERROR, MB_OK|MB_ICONERROR ) ;
		return ;
		}
	if( !( lpfnDllFunc1 = (LPFNDLLFUNC1) GetProcAddress( lphDLL, TEXT("main_m1") ) ) ) {
		MessageBox( NULL, KT_MAIN_KCHAT_FUNC_FAILED, KT_CAP_ERROR, MB_OK|MB_ICONERROR  );
		FreeLibrary( lphDLL ) ;
		return ;
		}
	(lpfnDllFunc1) () ;
	FreeLibrary( lphDLL ) ;
	return ;
}


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
			} else if( st[i+1] == 'p' ) { 			// \p pauses one second
				SendKeyboard( hwnd, buffer ) ;
				Sleep(1000);
				buffer[0] = '\0' ; j = 0 ;
				i++ ; 
			} else if( st[i+1] == 's' ) { 			// \s03 pauses 3 seconds
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
			if( buffer[strlen(buffer)-1]=='\\' ) { // a command ending in \ gets no carriage return
				buffer[strlen(buffer)-1]='\0' ;
				SendKeyboard( hwnd, buffer ) ;
			} else {
				SendKeyboard( hwnd, buffer ) ;
				if( buffer[strlen(buffer)-1] != '\n' ) // add a carriage return if needed
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
		if( conf_get_int(conf,CONF_protocol) != PROT_TELNET ) debug_logevent( buf ) ; // log only outside telnet (the password goes in clear there)
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
/* ===================== broadcast gate (/command, -sendcmd) =================
 *
 * The broadcast TYPES text into every KiTTY window of this class, and a trailing
 * Return means it RUNS on the far end. Useful (say one thing to twenty servers)
 * and dangerous for the same reason: the twenty include whatever production
 * session happens to be open. So a window accepts it only when armed -
 * [KiTTY] sendcmdmode (default no) sets the starting state, Tools > "Accept
 * broadcast" flips the current window.
 *
 * NEITHER this nor the group below is a security boundary. Anything running as
 * you can post the same message, and could type into your session by other means
 * anyway. This decides when your terminals accept it - it does not keep an
 * attacker out.
 */
static int broadcast_default_on = 0;            /* [KiTTY] sendcmdmode */
void kitty_broadcast_set_enabled( int on ) { broadcast_default_on = (on != 0) ; }
int  kitty_broadcast_default( void ) { return broadcast_default_on ; }

/*
 * WHICH KiTTYs hear each other. An installed copy and a portable one on a USB
 * stick are both "KiTTY" windows on the same desktop, so without this a
 * broadcast from the stick would type into the laptop's admin sessions.
 *
 * DERIVED, not stored: the key is a hash of the executable path and the registry
 * hive in use, so two installs differ by construction and every window of one
 * install agrees - with nothing written anywhere. Generating a key on first run
 * and saving it would do the same until the media is READ-ONLY, where each
 * process would invent its own and the broadcast would quietly stop working
 * inside that copy.
 *
 * [KiTTY] sendcmdgroup overrides it, for deliberately joining two installs into
 * one group or splitting one into several.
 */
/* The key a BROADCAST IS SENT WITH. Normally this install's own, but
 * `-sendcmdkey <key>` overrides it so a script can aim at the sessions carrying
 * that key rather than at every window of the install. Separate flag rather than
 * more syntax inside -sendcmd: the key must not be confusable with the text. */
static char broadcast_send_key[80] = "" ;
void kitty_broadcast_set_send_key( const char *k )
{
	if( k == NULL ) { broadcast_send_key[0] = '\0' ; return ; }
	snprintf( broadcast_send_key, sizeof(broadcast_send_key), "%s", k ) ;
}
const char *kitty_broadcast_send_key( void )
{
	if( broadcast_send_key[0] ) return broadcast_send_key ;
	return kitty_broadcast_group() ;
}
/* The override itself, so the send console can put back exactly what it
 * found (an empty string means "none set"), rather than the resolved key. */
const char *kitty_broadcast_send_key_override( void )
{
	return broadcast_send_key ;
}

/* Did the install key come from kitty.ini, or was it derived? The config box
 * says which, and "generated" versus "you set this in the ini" are different
 * facts to the person reading it. */
static int group_from_ini = 0 ;
int kitty_broadcast_group_from_ini( void ) { (void)kitty_broadcast_group() ; return group_from_ini ; }

/* The cached install key, so the config box's edit of sendcmdgroup shows in
 * THIS process at once (the resolved key, the provenance line). The store is
 * written by the config box itself; this only follows it. An empty key throws
 * the cache away, so the next kitty_broadcast_group() derives again. Windows
 * of OTHER processes read the store when they start: the change reaches them
 * with their next start, not before. */
static char broadcast_group_cache[80] = "" ;
void kitty_broadcast_set_group( const char *k )
{
	if( k == NULL || k[0] == '\0' ) {
		broadcast_group_cache[0] = '\0' ;
		group_from_ini = 0 ;
		return ;
	}
	snprintf( broadcast_group_cache, sizeof(broadcast_group_cache), "%s", k ) ;
	group_from_ini = 1 ;
}

const char *kitty_broadcast_group( void )
{
	char *group = broadcast_group_cache ;
	const size_t group_size = sizeof(broadcast_group_cache) ;
	char buf[4096] ;
	char exe[MAX_PATH+1] = "" ;
	unsigned long long h = 1469598103934665603ULL ;     /* FNV-1a, 64-bit */
	const char *p ;

	if( group[0] ) return group ;

	if( ReadParameterN( INIT_SECTION, KI_SENDCMDGROUP, buf, sizeof(buf) ) && buf[0] ) {
		snprintf( group, group_size, "%s", buf ) ;      /* explicit override */
		group_from_ini = 1 ;
		return group ;
	}
	if( GetModuleFileNameA( NULL, exe, MAX_PATH ) == 0 ) exe[0] = '\0' ;
	for( p = exe ; *p ; p++ ) {                            /* case-insensitive: */
		h ^= (unsigned char)( (*p >= 'A' && *p <= 'Z') ? *p + 32 : *p ) ;
		h *= 1099511628211ULL ;
	}
	for( p = kitty_registry_base() ; p && *p ; p++ ) {
		h ^= (unsigned char)( (*p >= 'A' && *p <= 'Z') ? *p + 32 : *p ) ;
		h *= 1099511628211ULL ;
	}
	snprintf( group, group_size, "auto-%016llx", h ) ;
	return group ;
}

/* What one broadcast carries through EnumWindows: the text, and whether the
 * terminal of THIS process is a target too. /command skips it - the user typed
 * the line there and does not want it typed back - but the send console on the
 * configuration window is a deliberate act aimed at every accepting terminal,
 * that one included. */
struct kitty_sendcmd_job {
	const char *cmd ;
	int include_self ;
} ;

static BOOL CALLBACK SendCommandProc( HWND hwnd, LPARAM lParam ) {
	char buffer[256] ;
	const struct kitty_sendcmd_job *job = (const struct kitty_sendcmd_job *)lParam ;
	GetClassName( hwnd, buffer, 256 ) ;
	if( !strcmp( buffer, KiTTYClassName ) ) {
		if( hwnd != MainHwnd || job->include_self ) {
			COPYDATASTRUCT data;
			/* dwData 2 carries "<group>\0<text>". The old dwData 1 was bare
			 * text with no group, so a receiver cannot tell which install it
			 * came from; it is refused (and logged) rather than obeyed. */
			const char *grp = kitty_broadcast_send_key() ;
			size_t glen = strlen( grp ) , tlen = strlen( job->cmd ) ;
			char *payload = (char*)malloc( glen + 1 + tlen + 1 ) ;
			memcpy( payload, grp, glen ) ; payload[glen] = '\0' ;
			memcpy( payload + glen + 1, job->cmd, tlen ) ;
			payload[glen + 1 + tlen] = '\0' ;
			data.dwData = 2 ;
			data.cbData = (DWORD)( glen + 1 + tlen + 1 ) ;
			data.lpData = payload ;
			SendMessage( hwnd, WM_COPYDATA, (WPARAM)(HWND)MainHwnd, (LPARAM) (LPVOID)&data ) ;
			free( payload ) ;
			NbWindows++ ;
		}
	}
	return TRUE ;
}

int SendCommandAllWindowsEx( HWND hwnd, char * cmd, int include_self ) {
	struct kitty_sendcmd_job job ;
	NbWindows=0 ;
	if( cmd==NULL ) return 0 ;
	if( strlen(cmd) > 0 ) {
		job.cmd = cmd ;
		job.include_self = include_self ;
		EnumWindows( SendCommandProc, (LPARAM)&job ) ;
	}
	return NbWindows ;
}

int SendCommandAllWindows( HWND hwnd, char * cmd ) {
	return SendCommandAllWindowsEx( hwnd, cmd, 0 ) ;
}
	
// Resizing of the windows of the same class
static BOOL CALLBACK ResizeWinListProc( HWND hwnd, LPARAM lParam ) {
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

static BOOL CALLBACK EnumWindowsProc( HWND hwnd, LPARAM lParam ) {
	char buffer[256] ;
	GetClassName( hwnd, buffer, 256 ) ;
	
	if( (!strcmp( buffer, appname )) || (!strcmp( buffer, "PuTTYConfigBox" )) ) {
		NbWindows++ ;
	}
	
	return TRUE ;
}

// Count the windows of the same class as KiTTY
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
