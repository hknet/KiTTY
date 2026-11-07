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
#include "kitty_sendesc.h"   /* the two hex digits of \k and \x */
#include "kitty_params.h"
#include "kitty_defs.h"     /* KITTY_DEFAULT_SESSION */
#include "kitty_common.h"
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
// The windows the last broadcast gave up on (SendCommandAllWindowsEx)
static int NbSkipped = 0 ;


//extern const int Default_Port ;
//void server_run_routine( const int port, const int timeout ) ;

//extern int PORT ; int main_m1( void ) ;
//void routine_server( void * st ) { main_m1() ; }

typedef void (CALLBACK* LPFNDLLFUNC1)( void ) ;
void routine_server( void * st ) {
	HMODULE lphDLL ;               // Handle to DLL
	LPFNDLLFUNC1 lpfnDllFunc1 ;    // Function pointer
	
	/* the name in full: a cut one loaded another DLL (kcha.dll, or <dir>.dll
	 * in the parent folder); one too long for Windows fails to load */
	char * buffer = dupprintf( "%s\\kchat.dll", InitialDirectory ) ;
	lphDLL = LoadLibrary( TEXT( buffer ) ) ;
	sfree( buffer ) ;
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


/*
 * The typing queue. SendKeyboard and SendKeyboardPlus append steps here and
 * the terminal window's TIMER_SENDQ types them in order (kitty_sendq_tick,
 * window.c). A pause - \p, \sNN, the delay between characters - is the
 * timer's interval, never a Sleep on the window's thread: the window keeps
 * painting and reading the network while a long text, a broadcast with \s30
 * or an AutoCommand is typed. The order of everything typed stays the order
 * of the calls, because every caller appends to the same queue.
 */
enum { KQ_CHAR, KQ_ENTER, KQ_WAIT, KQ_TERM, KQ_VKEY, KQ_BREAK, KQ_RESET } ;
typedef struct { unsigned char kind ; unsigned int val ; HWND hwnd ; } kq_op ;
static kq_op *kq = NULL ;
static size_t kq_head = 0, kq_len = 0, kq_cap = 0 ;
static int kq_armed = 0 ;

static void kq_push( HWND hwnd, int kind, unsigned int val ) {
	if( kq_head + kq_len == kq_cap ) {
		if( kq_head > 0 ) {
			memmove( kq, kq + kq_head, kq_len * sizeof(*kq) ) ;
			kq_head = 0 ;
		}
		if( kq_len == kq_cap ) {
			size_t nc = kq_cap ? 2 * kq_cap : 256 ;
			kq_op *n = (kq_op *)realloc( kq, nc * sizeof(*kq) ) ;
			if( n == NULL ) return ;
			kq = n ; kq_cap = nc ;
		}
	}
	kq[kq_head + kq_len].kind = (unsigned char)kind ;
	kq[kq_head + kq_len].val = val ;
	kq[kq_head + kq_len].hwnd = hwnd ;
	kq_len++ ;
}

/* The timer starts the queue, unless it already runs (a pause waits on it) */
static void kq_kick( HWND hwnd ) {
	if( !kq_armed && kq_len ) {
		kq_armed = 1 ;
		SetTimer( hwnd, TIMER_SENDQ, 1, NULL ) ;
	}
}

/* The modifiers \kXX switched on, for the keys after them in one send
 * (KITTY_VKEY_*). Reset at the end of every send (KQ_RESET): a send that
 * leaves one on does not carry it into the next, or into a login-script
 * reply. The Win key is no modifier here: a terminal has no use for it,
 * and with it \k5B\k52 was Win+R. */
static int keyb_mods = 0 ;

/* One \kXX: a modifier toggles its bit, any other key is typed into this
 * window's terminal with the modifiers that are on (kitty_type_vkey,
 * window.c: TranslateKey with that modifier state). Nothing goes through
 * Windows' keyboard, so the window need not have the focus and no other
 * window can receive the key. */
static void kq_vkey( HWND hwnd, int j ) {
	if( j==VK_CONTROL || j==VK_LCONTROL || j==VK_RCONTROL ) keyb_mods ^= KITTY_VKEY_CTRL ;
	else if( j==VK_SHIFT || j==VK_LSHIFT || j==VK_RSHIFT ) keyb_mods ^= KITTY_VKEY_SHIFT ;
	else if( j==VK_MENU || j==VK_LMENU ) keyb_mods ^= KITTY_VKEY_ALT ;
	else if( j==VK_RMENU ) keyb_mods ^= KITTY_VKEY_ALTGR ;
	else if( j==VK_LWIN || j==VK_RWIN || j==VK_APPS ) return ;   /* not sent */
	else if( keyb_mods || j==VK_ESCAPE || j==VK_HOME || j==VK_END )
		kitty_type_vkey( hwnd, j, keyb_mods ) ;
	else {
		/* no modifier: the key code itself as a character, as \kXX always
		 * typed it (\k41 is "A") */
		char stb[2] ;
		stb[0] = (char)j ; stb[1] = '\0' ;
		SendStrToTerminal( stb, 1 ) ;
	}
}

static void kq_do( const kq_op *op ) {
	char stb[2] ;
	switch( op->kind ) {
	  case KQ_CHAR:
		SendMessage( op->hwnd, WM_CHAR, (WPARAM)(char)op->val, 0 ) ;
		break ;
	  case KQ_ENTER:
		SendMessage( op->hwnd, WM_KEYDOWN, VK_RETURN, 0 ) ;
		break ;
	  case KQ_TERM:
		stb[0] = (char)op->val ; stb[1] = '\0' ;
		SendStrToTerminal( stb, 1 ) ;
		break ;
	  case KQ_VKEY:
		kq_vkey( op->hwnd, (int)op->val ) ;
		break ;
	  case KQ_BREAK:                     /* \c: the break, to the backend */
		kitty_send_break( op->hwnd ) ;
		break ;
	  case KQ_RESET:                     /* the end of a send */
		keyb_mods = 0 ;
		break ;
	}
}

/* TIMER_SENDQ: the steps up to the next pause (or a round's budget, so a
 * long text gives the message pump its turn), then the timer again. A step
 * may type into this window and start new typing: it is appended, the
 * order holds. */
void kitty_sendq_tick( HWND hwnd ) {
	int budget = 4096 ;
	KillTimer( hwnd, TIMER_SENDQ ) ;
	kq_armed = 0 ;
	while( kq_len && budget-- > 0 ) {
		kq_op op = kq[kq_head] ;
		kq_head++ ; kq_len-- ;
		if( op.kind == KQ_WAIT ) {
			if( op.val > 0 && kq_len ) {
				kq_armed = 1 ;
				SetTimer( hwnd, TIMER_SENDQ, op.val, NULL ) ;
				return ;
			}
			continue ;
		}
		kq_do( &op ) ;
	}
	if( kq_len == 0 ) { kq_head = 0 ; return ; }
	kq_armed = 1 ;
	SetTimer( hwnd, TIMER_SENDQ, 1, NULL ) ;
}

void SendKeyboard( HWND hwnd, const char * buffer ) {
	size_t i, n ;
	if( buffer == NULL || ( n = strlen( buffer ) ) == 0 ) return ;
	for( i = 0 ; i < n ; i++ ) {
		if( buffer[i] == '\n' ) kq_push( hwnd, KQ_ENTER, 0 ) ;
		else if( buffer[i] != '\r' ) kq_push( hwnd, KQ_CHAR, (unsigned char)buffer[i] ) ;
		if( between_char_delay > 0 ) kq_push( hwnd, KQ_WAIT, (unsigned int)between_char_delay ) ;
	}
	kq_kick( hwnd ) ;
}

/*
SetForegroundWindow(hwnd);
keybd_event(VK_CONTROL, 0x1D, 0, 0);
keybd_event(0x58, 0x47, 0, 0);
keybd_event(0x58, 0x47, KEYEVENTF_KEYUP, 0);
keybd_event(VK_CONTROL, 0x1D, KEYEVENTF_KEYUP, 0);
*/

void SendKeyboardPlus( HWND hwnd, const char * st ) {
	if( strlen(st) <= 0 ) return ;
	int i=0, j=0;
	unsigned hexv = 0 ;
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
				kq_push( hwnd, KQ_WAIT, (unsigned int)internal_delay ) ;
				buffer[0] = '\0' ; j = 0 ;
				i++ ;
			} else if( st[i+1] == 'p' ) { 			// \p pauses one second
				SendKeyboard( hwnd, buffer ) ;
				kq_push( hwnd, KQ_WAIT, 1000 ) ;
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
				kq_push( hwnd, KQ_WAIT, (unsigned int)j * 1000 ) ;
				buffer[0] = '\0' ; j = 0 ;
				i++ ;
			} else if( st[i+1] == 'c' ) {
				SendKeyboard( hwnd, buffer ) ;
				kq_push( hwnd, KQ_BREAK, 0 ) ;
				buffer[0] = '\0' ; j = 0 ;
				i++ ;
			/* \k and \x take the two hex digits after the letter. Without
			 * both, the backslash is typed as it is: the escape skips four
			 * characters, and a text ending in "\k" or "\kA" stepped over
			 * its NUL and copied the bytes behind it. */
			} else if( (st[i+1] == 'k') && kitty_sendesc_hex2( st+i+2, &hexv ) ) {
				SendKeyboard( hwnd, buffer ) ;
				kq_push( hwnd, KQ_VKEY, hexv ) ;
				kq_push( hwnd, KQ_WAIT, (unsigned int)internal_delay ) ;
				buffer[0] = '\0' ; j = 0 ;
				i++ ; i++ ; i++ ;
			} else if( (st[i+1] == 'x') && kitty_sendesc_hex2( st+i+2, &hexv ) ) {
				SendKeyboard( hwnd, buffer ) ;
				kq_push( hwnd, KQ_TERM, hexv ) ;
				kq_push( hwnd, KQ_WAIT, (unsigned int)internal_delay ) ;
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
		kq_push( hwnd, KQ_RESET, 0 ) ;   /* no modifier carries into the next send */
		free( buffer ) ;
		kq_kick( hwnd ) ;
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
		/* The command can hold a password: the line gives its length only,
		 * never the text, and never as the format of the log call */
		if( debug_flag )
			debug_logevent( "Send automatic command (%u characters)", (unsigned)strlen(cmd) ) ;
		else
			debug_logevent( "%s", "Send automatic command" ) ;
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
 * Return means it RUNS on the far end. Useful (one line to twenty servers)
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
 * shows which, and "generated" versus "you set this in the ini" are different
 * facts to the person reading it. */
static int group_from_ini = 0 ;
int kitty_broadcast_group_from_ini( void ) { (void)kitty_broadcast_group() ; return group_from_ini ; }

/* The cached install key, so the config box's edit of sendcmdgroup shows in
 * THIS process at once (the resolved key, the provenance line). The store is
 * written by the config box itself; this only follows it. An empty key throws
 * the cache away, so the next kitty_broadcast_group() derives again. Windows
 * of OTHER processes read the store when they start: the change reaches them
 * with their next start, not before. */
/* In full (sfree'd memory): a key set in kitty.ini was cut at 79 bytes, so
 * two installs with different long keys could meet as one group. The copy
 * is made before the old one goes: `k` may be that one itself. */
static char *broadcast_group_cache = NULL ;
void kitty_broadcast_set_group( const char *k )
{
	char *n = ( k == NULL || k[0] == '\0' ) ? NULL : dupstr( k ) ;
	sfree( broadcast_group_cache ) ;
	broadcast_group_cache = n ;
	group_from_ini = n != NULL ;
}

const char *kitty_broadcast_group( void )
{
	char *buf, *exe ;
	unsigned long long h = 1469598103934665603ULL ;     /* FNV-1a, 64-bit */
	const char *p ;

	if( broadcast_group_cache ) return broadcast_group_cache ;

	buf = ReadParameterDup( INIT_SECTION, KI_SENDCMDGROUP ) ;
	if( buf[0] ) {
		broadcast_group_cache = buf ;                   /* explicit override */
		group_from_ini = 1 ;
		return broadcast_group_cache ;
	}
	sfree( buf ) ;
	/* the exe path in full: a cut one gave two installs in long folders
	 * one group */
	exe = kitty_module_path_dup( NULL ) ;
	for( p = exe ? exe : "" ; *p ; p++ ) {                 /* case-insensitive: */
		h ^= (unsigned char)( (*p >= 'A' && *p <= 'Z') ? *p + 32 : *p ) ;
		h *= 1099511628211ULL ;
	}
	sfree( exe ) ;
	for( p = kitty_registry_base() ; p && *p ; p++ ) {
		h ^= (unsigned char)( (*p >= 'A' && *p <= 'Z') ? *p + 32 : *p ) ;
		h *= 1099511628211ULL ;
	}
	broadcast_group_cache = dupprintf( "auto-%016llx", h ) ;
	return broadcast_group_cache ;
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
			 * text with no group, so a receiver cannot see which install it
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
			/* One window after the other: WM_COPYDATA only goes with a call
			 * that waits. A window that does not answer (hung, or held by a
			 * modal box) is given up after KITTY_BROADCAST_TIMEOUT_MS - a
			 * hung one at once - and the next one still gets the text. */
			{
				DWORD_PTR res = 0 ;
				if( SendMessageTimeout( hwnd, WM_COPYDATA, (WPARAM)(HWND)MainHwnd,
				                        (LPARAM)(LPVOID)&data,
				                        SMTO_NORMAL | SMTO_ABORTIFHUNG,
				                        KITTY_BROADCAST_TIMEOUT_MS, &res ) ) {
					NbWindows++ ;
				} else {
					char title[128] = "" ;
					DWORD pid = 0 ;
					GetWindowThreadProcessId( hwnd, &pid ) ;
					GetWindowText( hwnd, title, sizeof(title) ) ;
					debug_logevent( KT_BROADCAST_SKIPPED_FMT, title, (unsigned long)pid,
					                KITTY_BROADCAST_TIMEOUT_MS / 1000 ) ;
					NbSkipped++ ;
				}
			}
			free( payload ) ;
		}
	}
	return TRUE ;
}

int SendCommandAllWindowsEx( HWND hwnd, char * cmd, int include_self ) {
	struct kitty_sendcmd_job job ;
	NbWindows=0 ;
	NbSkipped=0 ;
	if( cmd==NULL ) return 0 ;
	if( strlen(cmd) > 0 ) {
		job.cmd = cmd ;
		job.include_self = include_self ;
		EnumWindows( SendCommandProc, (LPARAM)&job ) ;
	}
	return NbWindows ;
}

int kitty_broadcast_skipped( void ) {
	return NbSkipped ;
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

/* Is the program behind `pid` a KiTTY++? The window classes are not ours
 * alone - old KiTTY's terminal is "KiTTY" as well, and every PuTTY's
 * configuration box is "PuTTYConfigBox" - so the program's own version
 * resource decides: its ProductVersion contains KiTTY++ ("0.85.1.15-beta
 * (KiTTY++, PuTTY 0.85 base)"). This process always is; one that cannot be
 * opened or read is not, which for both callers (the "first window" tests in
 * kitty_startup.c) means acting as the first. Answers are kept per process
 * for one count. */
#define WINCOUNT_CACHE 32
static struct { DWORD pid ; int ours ; } wincount_cache[WINCOUNT_CACHE] ;
static int wincount_n = 0 ;

static int kitty_process_is_kittypp( DWORD pid ) {
	char *path = NULL ;
	HANDLE proc ;
	DWORD dummy, sz ;
	void *buf ;
	int i, ours = 0 ;

	if( pid == GetCurrentProcessId() ) return 1 ;
	for( i = 0 ; i < wincount_n ; i++ )
		if( wincount_cache[i].pid == pid ) return wincount_cache[i].ours ;
	/* PROCESS_QUERY_LIMITED_INFORMATION is Vista and later; the older right
	 * pair is what kitty_process_image_path's XP way needs */
	proc = OpenProcess( 0x1000, FALSE, pid ) ;
	if( !proc ) proc = OpenProcess( PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid ) ;
	if( proc ) {
		if( ( path = kitty_process_image_path_dup( proc ) ) != NULL
		    && ( sz = GetFileVersionInfoSizeA( path, &dummy ) ) > 0
		    && ( buf = malloc( sz ) ) != NULL ) {
			if( GetFileVersionInfoA( path, 0, sz, buf ) ) {
				struct { WORD lang, cp ; } *xlat = NULL ;
				UINT n = 0, j ;
				if( VerQueryValueA( buf, "\\VarFileInfo\\Translation", (void **)&xlat, &n ) && xlat != NULL )
					for( j = 0 ; j < n / sizeof(*xlat) && !ours ; j++ ) {
						char sub[64] ; char *val = NULL ; UINT vlen = 0 ;
						snprintf( sub, sizeof(sub), "\\StringFileInfo\\%04x%04x\\ProductVersion",
						          xlat[j].lang, xlat[j].cp ) ;
						if( VerQueryValueA( buf, sub, (void **)&val, &vlen ) && val != NULL && vlen > 0
						    && strstr( val, "KiTTY++" ) != NULL )
							ours = 1 ;
					}
			}
			free( buf ) ;
		}
		sfree( path ) ;
		CloseHandle( proc ) ;
	}
	if( wincount_n < WINCOUNT_CACHE ) {
		wincount_cache[wincount_n].pid = pid ;
		wincount_cache[wincount_n].ours = ours ;
		wincount_n++ ;
	}
	return ours ;
}

static BOOL CALLBACK EnumWindowsProc( HWND hwnd, LPARAM lParam ) {
	char buffer[256] ;
	DWORD pid = 0 ;
	GetClassName( hwnd, buffer, 256 ) ;
	
	if( (!strcmp( buffer, appname )) || (!strcmp( buffer, "PuTTYConfigBox" )) ) {
		GetWindowThreadProcessId( hwnd, &pid ) ;
		if( kitty_process_is_kittypp( pid ) )
			NbWindows++ ;
	}
	
	return TRUE ;
}

// Count the KiTTY++ windows: terminals and configuration boxes of KiTTY++
// programs, this one's included - not old KiTTY's, not stock PuTTY's
int WindowsCount( HWND hwnd ) {
	char buffer[256] ;
	NbWindows = 0 ;
	wincount_n = 0 ;
	
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
