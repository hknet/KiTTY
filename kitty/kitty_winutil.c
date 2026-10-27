/*
 * kitty_winutil.c - small Win32 helpers shared by the KiTTY additions:
 * window transparency and the OS version, the file and folder selection
 * dialogs (open, save, folder picker), printing a block of text, the
 * clipboard write helper, launching a process (RunCommand, the -ed editor),
 * the event-log printf and the absolute-path test.
 */
#include "kitty_winutil.h"
#include "kitty_win.h"
#include "kitty_authenticode.h"   /* shared Authenticode trust + CN gate */
#include "kitty_notice.h"          /* near-the-clock warning window */
#include "kitty_dlgbox.h"          /* kitty_info_modeless: the print report */
#include "kitty_rc_additions.h"   /* IDD_UPDATEBOX, IDC_UPD_TEXT, IDC_UPD_UPDATE */
#include "kitty_theme.h"           /* the app-wide colour theme */
#include "../windows/putty-rc.h"   /* -demo-templates: the shared dialog ids */
#include "kitty_oldwin.h"   /* APIs newer than the oldest Windows we load on */
#include "kitty_text.h"     /* shared captions */
#include "kitty_inikeys.h"  /* KI_*: the kitty.ini key names */
#include <process.h>   /* _beginthreadex: kitty_pick_async */
#include <wininet.h>   /* CheckVersionFromWebSite: GitHub releases query */
#include <wintrust.h>  /* in-app updater: Authenticode trust verification */
#include <softpub.h>   /* WINTRUST_ACTION_GENERIC_VERIFY_V2 */
#include <msi.h>       /* in-app updater: install-type detection by UpgradeCode */
#include "kitty_gui.h"
#include "kitty.h"
#include "kitty_storage.h"
#include "kitty_auxpos.h"
#include "kitty_winerr.h"   /* kitty_win_reason: Windows' reason, for the reports */
#include "kitty_params.h"   /* GetNoticeSeconds: RunCommand's start-failure notice */
#include <shellapi.h>       /* ShellExecuteExA: RunCommand's fallback */
#include "kitty_renameguard.h"   /* kitty_eventlog_line: -edit's start failure */
/* wincrypt.h (CryptQueryObject / signer cert) comes in via windows.h */

/* MOD_PERSO event-log wrapper, defined in windows/window.c */

// Change the window transparency
void SetTransparency( HWND hwnd, int value ) {
#ifndef MOD_NOTRANSPARENCY
	SetLayeredWindowAttributes( hwnd, 0, value, LWA_ALPHA ) ;
#endif
	}


// Operating system version number
void GetOSInfo( char * version ) { // ==> Deprecated with version >= Windows 8.1
	OSVERSIONINFO osvi;
	ZeroMemory(&osvi, sizeof(OSVERSIONINFO));
	osvi.dwOSVersionInfoSize = sizeof(OSVERSIONINFO);
	GetVersionEx(&osvi);
	sprintf( version, "%ld.%ld %ld %ld %s %dx%d", osvi.dwMajorVersion, osvi.dwMinorVersion, osvi.dwBuildNumber, osvi.dwPlatformId, osvi.szCSDVersion, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) ) ;
}
/*
http://msdn.microsoft.com/en-us/library/windows/desktop/ms724832%28v=vs.85%29.aspx
Operating system 			Version number
Windows 10 Insider Preview		10.0*
Windows Server Technical Preview	10.0*
Windows Server 2019 			10.0*
Windows Server 2016 			10.0*
Windows 8.1				6.3*
Windows Server 2012 R2			6.3*
Windows 8				6.2
Windows Server 2012			6.2
Windows 7				6.1
Windows Server 2008 R2			6.1
Windows Server 2008			6.0
Windows Vista				6.0
Windows Server 2003 R2			5.2
Windows Server 2003			5.2
Windows XP 64-Bit Edition		5.2
Windows XP				5.1
Windows 2000				5.0
*/

/*
https://msdn.microsoft.com/en-us/library/aa383745%28v=vs.85%29.aspx#faster_builds_with_smaller_header_files
Minimum system required					Minimum value for _WIN32_WINNT and WINVER
Windows 8.1						_WIN32_WINNT_WINBLUE (0x0602)
Windows 8						_WIN32_WINNT_WIN8 (0x0602)
Windows 7						_WIN32_WINNT_WIN7 (0x0601)
Windows Server 2008					_WIN32_WINNT_WS08 (0x0600)
Windows Vista						_WIN32_WINNT_VISTA (0x0600)
Windows Server 2003 with SP1, Windows XP with SP2	_WIN32_WINNT_WS03 (0x0502)
Windows Server 2003, Windows XP				_WIN32_WINNT_WINXP (0x0501)

Minimum version required		Minimum value of _WIN32_IE
Internet Explorer 10.0			_WIN32_IE_IE100 (0x0A00)
Internet Explorer 9.0			_WIN32_IE_IE90 (0x0900)
Internet Explorer 8.0			_WIN32_IE_IE80 (0x0800)
Internet Explorer 7.0			_WIN32_IE_IE70 (0x0700)
Internet Explorer 6.0 SP2		_WIN32_IE_IE60SP2 (0x0603)
Internet Explorer 6.0 SP1		_WIN32_IE_IE60SP1 (0x0601)
Internet Explorer 6.0			_WIN32_IE_IE60 (0x0600)
Internet Explorer 5.5			_WIN32_IE_IE55 (0x0550)
Internet Explorer 5.01			_WIN32_IE_IE501 (0x0501)
Internet Explorer 5.0, 5.0a, 5.0b	_WIN32_IE_IE50 (0x0500)
*/

typedef BOOL (WINAPI *LPFN_ISWOW64PROCESS) (HANDLE, PBOOL);
LPFN_ISWOW64PROCESS fnIsWow64Process;
BOOL IsWow64(void) {
    BOOL bIsWow64 = FALSE;
    //IsWow64Process is not available on all supported versions of Windows.
    //Use GetModuleHandle to get a handle to the DLL that contains the function
    //and GetProcAddress to get a pointer to the function if available.
    fnIsWow64Process = (LPFN_ISWOW64PROCESS) GetProcAddress(
        GetModuleHandle(TEXT("kernel32")),"IsWow64Process");
    if(NULL != fnIsWow64Process) {
        if (!fnIsWow64Process(GetCurrentProcess(),&bIsWow64)) {
            //handle error
        }
    }
    return bIsWow64 ;
}

/* The pickers' own work; the public names below run them through
 * kitty_sync_pick (see there). Each answer is held whole (sfree it); NULL =
 * cancelled, or the answer could not be read whole. */
static char * open_file_name_from( HWND hFrame, const char * Title, const char * Filter, const char * initialdir ) ;
static char * save_file_name( HWND hFrame, const char * Title, const char * Filter ) ;
static char * save_file_name_from( HWND hFrame, const char * prefill, const char * Title, const char * Filter, const char * initialdir ) ;
static char * open_dir_name_from( HWND hFrame, const char * initial, const char * title ) ;

/* The classic dialogs' answer buffer: the longest path Windows has, in
 * bytes of the ANSI code page. */
#define KITTY_PICK_FILE_MAX 32768

/*
 * KiTTY: a picker called from inside a pumped box - a "Browse..." button of
 * the mid-session Change Settings box - runs on a worker thread while this
 * thread serves the session (ShinyPumpCall, windows/utils/shinydialogbox.c):
 * the common dialogs' own loop serves no network events, and the session
 * stood still until the picker closed. Anywhere else the picker runs here,
 * as before. Either way the caller gets the answer when the picker is
 * closed.
 */
enum { KITTY_SYNC_OPEN, KITTY_SYNC_SAVE, KITTY_SYNC_SAVE_FROM, KITTY_SYNC_DIR } ;
struct kitty_sync_pick {
	int kind ;
	HWND owner ;
	const char *prefill, *title, *filter, *initial ;
	char *result ;
} ;

static void kitty_sync_pick_run( void *v ) {
	struct kitty_sync_pick *p = (struct kitty_sync_pick *)v ;
	switch( p->kind ) {
	  case KITTY_SYNC_OPEN:
		p->result = open_file_name_from( p->owner, p->title, p->filter, p->initial ) ; break ;
	  case KITTY_SYNC_SAVE:
		p->result = save_file_name( p->owner, p->title, p->filter ) ; break ;
	  case KITTY_SYNC_SAVE_FROM:
		p->result = save_file_name_from( p->owner, p->prefill, p->title, p->filter, p->initial ) ; break ;
	  default:
		p->result = open_dir_name_from( p->owner, p->initial, p->title ) ; break ;
	}
}

static char *kitty_sync_pick( int kind, HWND owner, const char *prefill, const char *title,
                              const char *filter, const char *initial ) {
	struct kitty_sync_pick p ;
	p.kind = kind ; p.owner = owner ; p.prefill = prefill ; p.title = title ;
	p.filter = filter ; p.initial = initial ; p.result = NULL ;
	ShinyPumpCall( kitty_sync_pick_run, &p ) ;
	return p.result ;
}

char *kitty_pick_open_dup( HWND hFrame, const char *title, const char *filter, const char *initialdir ) {
	return kitty_sync_pick( KITTY_SYNC_OPEN, hFrame, NULL, title, filter, initialdir ) ;
}
char *kitty_pick_save_dup( HWND hFrame, const char *title, const char *filter,
                           const char *prefill, const char *initialdir ) {
	if( initialdir && initialdir[0] )
		return kitty_sync_pick( KITTY_SYNC_SAVE_FROM, hFrame, prefill, title, filter, initialdir ) ;
	return kitty_sync_pick( KITTY_SYNC_SAVE, hFrame, NULL, title, filter, NULL ) ;
}
char *kitty_pick_dir_dup( HWND hFrame, const char *initial, const char *title ) {
	return kitty_sync_pick( KITTY_SYNC_DIR, hFrame, NULL, title, NULL, initial ) ;
}

/* "Name|pattern|Name|pattern|" as the classic dialogs take it: every '|' a
 * NUL, the list ended by two NULs (sfree it). */
static char *pick_filter_dup( const char *Filter ) {
	size_t n = Filter ? strlen( Filter ) : 0, i ;
	char *f = snewn( n + 2, char ) ;
	if( n ) memcpy( f, Filter, n ) ;
	f[n] = f[n+1] = '\0' ;
	for( i = 0 ; i < n ; i++ ) if( f[i] == '|' ) f[i] = '\0' ;
	return f ;
}

/* GetOpenFileName / GetSaveFileName into a buffer of the longest path, the
 * answer handed back whole; a buffer that was still too small is a failure
 * (FNERR_BUFFERTOOSMALL), never a cut name. `prefill` NULL = empty field. */
static char *pick_classic( int save, HWND hFrame, const char *Title, const char *Filter,
                           const char *initialdir, const char *prefill, const char *defext, DWORD flags ) {
	char *filter = pick_filter_dup( Filter ) ;
	char *buf = snewn( KITTY_PICK_FILE_MAX, char ), *res = NULL ;
	OPENFILENAME ofn = {0} ;
	BOOL ok ;
	buf[0] = '\0' ;
	if( prefill && strlen( prefill ) < KITTY_PICK_FILE_MAX ) strcpy( buf, prefill ) ;
	ofn.lStructSize     = sizeof(OPENFILENAME) ;
	ofn.hwndOwner       = hFrame ;
	ofn.lpstrFilter     = filter ;
	ofn.nFilterIndex    = 1 ;
	ofn.lpstrFile       = buf ;
	ofn.nMaxFile        = KITTY_PICK_FILE_MAX ;
	ofn.lpstrTitle      = Title ;
	ofn.lpstrInitialDir = ( initialdir && initialdir[0] ) ? initialdir : NULL ;
	ofn.lpstrDefExt     = defext ;
	ofn.Flags           = flags ;
	ok = save ? GetSaveFileName( &ofn ) : GetOpenFileName( &ofn ) ;
	if( ok && buf[0] ) {
		buf[KITTY_PICK_FILE_MAX - 1] = '\0' ;
		res = dupstr( buf ) ;
	}
	sfree( buf ) ;
	sfree( filter ) ;
	return res ;
}

static char * open_file_name_from( HWND hFrame, const char * Title, const char * Filter, const char * initialdir ) {
	return pick_classic( 0, hFrame, Title, Filter, initialdir, NULL, NULL,
		OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_LONGNAMES
		| OFN_EXPLORER | OFN_EXTENSIONDIFFERENT | OFN_DONTADDTORECENT ) ;
}

static char * save_file_name( HWND hFrame, const char * Title, const char * Filter ) {
	return pick_classic( 1, hFrame, Title, Filter, NULL, NULL, ".ktx",
		OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_LONGNAMES | OFN_OVERWRITEPROMPT
		| OFN_EXPLORER | OFN_EXTENSIONDIFFERENT | OFN_DONTADDTORECENT ) ;
}

/* The classic Save-As (GetSaveFileName): SaveFileNameFrom's fallback where the
 * Common Item Dialog cannot be created (XP). On Windows 11 the same call was
 * seen opening in the folder Windows remembers for the program, or in
 * Documents, ignoring both the initial folder and the path in the prefilled
 * name - which is why it is only the fallback now. */
static char * save_file_name_classic( HWND hFrame, const char * prefill, const char * Title, const char * Filter, const char * initialdir ) {
	/* kept as prefilled: the remote base name */
	return pick_classic( 1, hFrame, Title, Filter, initialdir, prefill, NULL,
		OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_LONGNAMES
		| OFN_OVERWRITEPROMPT | OFN_EXPLORER | OFN_NOCHANGEDIR
		| OFN_DONTADDTORECENT ) ;
}

#include <shlobj.h>
#include <shobjidl.h>   /* IFileOpenDialog (Common Item Dialog folder picker) */
#include "kitty_oldwin_reg.h"   /* XP: RegDeleteTree/RegGetValue via oldwin */
#include "kitty_tools.h"        /* existdirectory */

/* Save-As opening in a given folder with a name PREFILLED by the caller (the
 * remote base name, the last component of `prefill`). Used by Get File for a
 * single named file; FOS_OVERWRITEPROMPT is Windows' own replace question, so
 * a file is never overwritten silently. The chosen full path, or NULL on a
 * cancel.
 *
 * KiTTY: the Common Item Dialog (Vista+) with SetFolder, which FORCES the
 * folder: GetSaveFileName opened in the folder Windows remembers for the
 * program (or Documents), so Get File's Save-As never showed the download
 * folder it was given. GetSaveFileName stays as the fallback (XP). */
static char * save_file_name_from( HWND hFrame, const char * prefill, const char * Title, const char * Filter, const char * initialdir ) {
	static const GUID clsid_fsd = {0xC0B4E2F3,0xBA21,0x4773,{0x8D,0xBA,0x33,0x5E,0xC9,0x46,0xEB,0x8B}} ;   /* FileSaveDialog */
	static const GUID iid_fsd   = {0x84bccd23,0x5fde,0x4cdb,{0xae,0xa4,0xaf,0x64,0xb8,0x3d,0x78,0xab}} ;   /* IFileSaveDialog */
	static const GUID iid_si    = {0x43826d1e,0xe718,0x42ee,{0xbc,0x55,0xa1,0xe2,0x61,0xc3,0x7b,0xfe}} ;   /* IShellItem */
	IFileSaveDialog *pfd = NULL ;
	char *ret = NULL ;
	HRESULT hrInit = CoInitializeEx( NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE ) ;

	if( !prefill ) prefill = "" ;
	if( FAILED( CoCreateInstance( &clsid_fsd, NULL, CLSCTX_INPROC_SERVER, &iid_fsd, (void**)&pfd ) ) || !pfd ) {
		if( SUCCEEDED( hrInit ) ) CoUninitialize() ;
		return save_file_name_classic( hFrame, prefill, Title, Filter, initialdir ) ;
	}
	{
		DWORD opts = 0 ;
		pfd->lpVtbl->GetOptions( pfd, &opts ) ;
		pfd->lpVtbl->SetOptions( pfd, opts | FOS_OVERWRITEPROMPT | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST
		                               | FOS_NOCHANGEDIR | FOS_DONTADDTORECENT ) ;
	}
	if( Title ) {
		wchar_t *wt = kitty_acp_to_wide_dup( Title ) ;
		if( wt ) { pfd->lpVtbl->SetTitle( pfd, wt ) ; sfree( wt ) ; }
	}
	/* The filter: "Name|pattern|Name|pattern|" as the classic dialog takes it. */
	if( Filter && Filter[0] ) {
		COMDLG_FILTERSPEC spec[8] ; wchar_t *wf[8][2] ; UINT n = 0, k ;
		char *f = dupstr( Filter ) ; char *p, *name, *pat ;
		for( p = f ; n < 8 && *p ; ) {
			name = p ; if( !(p = strchr( p, '|' )) ) break ; *p++ = '\0' ;
			pat = p ; if( (p = strchr( p, '|' )) ) *p++ = '\0' ; else p = pat + strlen( pat ) ;
			wf[n][0] = kitty_acp_to_wide_dup( name ) ;
			wf[n][1] = kitty_acp_to_wide_dup( pat ) ;
			if( !wf[n][0] || !wf[n][1] ) { sfree( wf[n][0] ) ; sfree( wf[n][1] ) ; continue ; }
			spec[n].pszName = wf[n][0] ; spec[n].pszSpec = wf[n][1] ;
			n++ ;
		}
		if( n ) pfd->lpVtbl->SetFileTypes( pfd, n, spec ) ;
		for( k = 0 ; k < n ; k++ ) { sfree( wf[k][0] ) ; sfree( wf[k][1] ) ; }
		sfree( f ) ;
	}
	if( initialdir && initialdir[0] && existdirectory( initialdir ) ) {
		/* SHCreateItemFromParsingName is Vista+: resolved at run time, as in
		 * OpenDirNameFrom, so the XP build still loads. */
		typedef HRESULT (WINAPI *pfn_scifpn)( PCWSTR, IBindCtx *, REFIID, void ** ) ;
		HMODULE sh = GetModuleHandleA( "shell32.dll" ) ;
		pfn_scifpn scifpn = sh ? (pfn_scifpn)GetProcAddress( sh, "SHCreateItemFromParsingName" ) : NULL ;
		wchar_t *wi = scifpn ? kitty_acp_to_wide_dup( initialdir ) : NULL ;
		if( wi ) {
			IShellItem *psi0 = NULL ;
			if( SUCCEEDED( scifpn( wi, NULL, &iid_si, (void**)&psi0 ) ) && psi0 ) {
				pfd->lpVtbl->SetFolder( pfd, psi0 ) ;
				psi0->lpVtbl->Release( psi0 ) ;
			}
			sfree( wi ) ;
		}
	}
	{
		/* the prefilled name without its folder: the folder is set above */
		const char *base = strrchr( prefill, '\\' ) ;
		wchar_t *wn = kitty_acp_to_wide_dup( base ? base + 1 : prefill ) ;
		if( wn && wn[0] ) pfd->lpVtbl->SetFileName( pfd, wn ) ;
		sfree( wn ) ;
	}
	if( SUCCEEDED( pfd->lpVtbl->Show( pfd, hFrame ) ) ) {
		IShellItem *psi = NULL ;
		if( SUCCEEDED( pfd->lpVtbl->GetResult( pfd, &psi ) ) && psi ) {
			PWSTR wpath = NULL ;
			if( SUCCEEDED( psi->lpVtbl->GetDisplayName( psi, SIGDN_FILESYSPATH, &wpath ) ) && wpath ) {
				ret = kitty_wide_to_acp_dup( wpath ) ;
				CoTaskMemFree( wpath ) ;
			}
			psi->lpVtbl->Release( psi ) ;
		}
	}
	pfd->lpVtbl->Release( pfd ) ;
	if( SUCCEEDED( hrInit ) ) CoUninitialize() ;
	return ret ;   /* a cancel is a cancel: no fallback to the classic box */
}
/* The tree picker's initial selection: SHBrowseForFolder takes it through a
 * callback, not a field. lParam carries the ANSI path. */
static int CALLBACK browse_start_folder( HWND hwnd, UINT msg, LPARAM lParam, LPARAM data ) {
	(void)lParam ;
	if( msg == BFFM_INITIALIZED && data )
		SendMessage( hwnd, BFFM_SETSELECTIONA, TRUE, data ) ;
	return 0 ;
}

/*
 * KiTTY: the file and folder pickers without stopping the session. The
 * common dialogs run a message loop of their own that serves no network
 * events, so a picker opened from the terminal's thread stood the session
 * still - keepalives unanswered, a server's close unseen. Here the picker
 * runs on a thread of its own (COM is set up per thread by the pickers
 * themselves); the owner is disabled by the dialog as usual, and the
 * terminal's thread goes on. The answer comes back to the calling thread,
 * through a message-only window it owns: done(ctx, path or NULL) runs there.
 * kitty_pick_cancel: done is not called (the caller went); the thread ends
 * with its dialog.
 */
#define WM_KITTY_PICK_DONE ( WM_APP + 0x3A2 )
struct kitty_pick {
	HWND owner ;
	int folder ;                        /* KITTY_PICK_FOLDER / _OPEN / _SAVE */
	/* each held whole (NULL = none): a script filter from kitty.ini
	 * (scriptfilefilter) may run past 1 KB, a folder past MAX_PATH */
	char *initial, *title, *filter ;
	char *result ;                      /* NULL = cancelled */
	volatile LONG cancelled ;
	void (*done)( void *ctx, const char *path ) ;
	void *ctx ;
} ;

static void kitty_pick_free( struct kitty_pick *p ) {
	sfree( p->initial ) ; sfree( p->title ) ; sfree( p->filter ) ; sfree( p->result ) ;
	free( p ) ;
}
static HWND kitty_pick_sink = NULL ;

/* kitty_call_async's call: fn on the worker, done back on the calling thread */
#define WM_KITTY_CALL_DONE ( WM_APP + 0x3A3 )
struct kitty_call {
	void (*fn)( void *ctx ) ;
	void (*done)( void *ctx ) ;
	void *ctx ;
} ;

static LRESULT CALLBACK kitty_pick_sink_proc( HWND h, UINT msg, WPARAM wp, LPARAM lp ) {
	if( msg == WM_KITTY_PICK_DONE ) {
		struct kitty_pick *p = (struct kitty_pick *)lp ;
		if( !p->cancelled ) p->done( p->ctx, p->result && p->result[0] ? p->result : NULL ) ;
		kitty_pick_free( p ) ;
		return 0 ;
	}
	if( msg == WM_KITTY_CALL_DONE ) {
		struct kitty_call *c = (struct kitty_call *)lp ;
		c->done( c->ctx ) ;
		free( c ) ;
		return 0 ;
	}
	return DefWindowProc( h, msg, wp, lp ) ;
}

/* The message-only window the answers come back through, made on first use
 * by the calling thread (the terminal's); NULL = none could be made. */
static HWND kitty_pick_sink_get( void ) {
	if( !kitty_pick_sink ) {
		WNDCLASSA wc ;
		memset( &wc, 0, sizeof(wc) ) ;
		wc.lpfnWndProc = kitty_pick_sink_proc ;
		wc.hInstance = GetModuleHandle( NULL ) ;
		wc.lpszClassName = "KiTTYPickSink" ;
		RegisterClassA( &wc ) ;
		kitty_pick_sink = CreateWindowExA( 0, "KiTTYPickSink", "", 0, 0, 0, 0, 0,
			HWND_MESSAGE, NULL, GetModuleHandle( NULL ), NULL ) ;
	}
	return kitty_pick_sink ;
}

/*
 * A blocking call made beside the session: fn(ctx) on a thread of its own,
 * then done(ctx) on the calling thread, through the same message-only
 * window as the pickers. For what kitty_pick_async does not cover - the
 * ZModem upload's multi-select picker, Get File's Save-As with the remote
 * name filled in, PrintDlg and the printing behind it: each runs a message
 * loop of its own that serves no network events, so on the terminal's thread
 * the session stood still until it was closed. COM is set up for the worker
 * (the file dialogs are shell windows). 1 = started; 0 = no thread or no
 * window to answer through (the caller makes the call itself, as before).
 * done always runs; ctx belongs to the caller throughout.
 */
static unsigned __stdcall kitty_call_thread( void *v ) {
	struct kitty_call *c = (struct kitty_call *)v ;
	HRESULT hr = CoInitializeEx( NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE ) ;
	c->fn( c->ctx ) ;
	if( SUCCEEDED( hr ) ) CoUninitialize() ;
	PostMessage( kitty_pick_sink, WM_KITTY_CALL_DONE, 0, (LPARAM)c ) ;
	return 0 ;
}

int kitty_call_async( void (*fn)( void *ctx ), void (*done)( void *ctx ), void *ctx ) {
	struct kitty_call *c ;
	uintptr_t th ;
	if( !kitty_pick_sink_get() ) return 0 ;
	c = calloc( 1, sizeof(*c) ) ;
	if( !c ) return 0 ;
	c->fn = fn ; c->done = done ; c->ctx = ctx ;
	th = _beginthreadex( NULL, 0, kitty_call_thread, c, 0, NULL ) ;
	if( !th ) { free( c ) ; return 0 ; }
	CloseHandle( (HANDLE)th ) ;
	return 1 ;
}

static unsigned __stdcall kitty_pick_thread( void *v ) {
	struct kitty_pick *p = (struct kitty_pick *)v ;
	if( p->folder == KITTY_PICK_SAVE ) {
		/* The classic Save-As keeps its default extension (.ktx) for a name
		 * typed without one; given a folder, the Common Item Dialog, which
		 * really opens there (SaveFileNameFrom). COM for this thread: the
		 * Save-As dialogs are shell windows. */
		HRESULT hr = CoInitializeEx( NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE ) ;
		p->result = kitty_pick_save_dup( p->owner, p->title, p->filter, "", p->initial ) ;
		if( SUCCEEDED( hr ) ) CoUninitialize() ;
	} else if( p->folder )
		p->result = kitty_pick_dir_dup( p->owner, p->initial, p->title ) ;
	else
		p->result = kitty_pick_open_dup( p->owner, p->title, p->filter, p->initial ) ;
	PostMessage( kitty_pick_sink, WM_KITTY_PICK_DONE, 0, (LPARAM)p ) ;
	return 0 ;
}

/* Start the picker; the token for kitty_pick_cancel, or NULL when no thread
 * could be started (the caller then uses OpenDirNameFrom /
 * OpenFileNameFrom as before). Call from the terminal's thread. */
void *kitty_pick_async( HWND owner, int folder, const char *initial, const char *title,
                        const char *filter, void (*done)( void *ctx, const char *path ), void *ctx ) {
	struct kitty_pick *p ;
	uintptr_t th ;
	if( !kitty_pick_sink_get() ) return NULL ;
	p = calloc( 1, sizeof(*p) ) ;
	if( !p ) return NULL ;
	p->owner = owner ;
	p->folder = folder ;
	if( initial && initial[0] ) p->initial = dupstr( initial ) ;
	if( title && title[0] ) p->title = dupstr( title ) ;
	if( filter ) p->filter = dupstr( filter ) ;
	p->done = done ;
	p->ctx = ctx ;
	th = _beginthreadex( NULL, 0, kitty_pick_thread, p, 0, NULL ) ;
	if( !th ) { kitty_pick_free( p ) ; return NULL ; }
	CloseHandle( (HANDLE)th ) ;
	return p ;
}

/* The asker is going: its done() must not run. The picker stays up until it
 * is answered or its owner goes; the token is freed then. */
void kitty_pick_cancel( void *token ) {
	if( token ) InterlockedExchange( &((struct kitty_pick *)token)->cancelled, 1 ) ;
}

static char * open_dir_name_from( HWND hFrame, const char * initial, const char * title ) {
	char * dirname = NULL ;
	if( initial && (!initial[0] || !existdirectory( initial )) ) initial = NULL ;
	/* Modern Common Item Dialog folder picker (Vista+): the full Explorer window
	 * with an address bar you can paste a path into, type-ahead and favourites -
	 * not the old tree-only SHBrowseForFolder. Falls back to the tree picker (with
	 * a New Folder button) if COM or the dialog is unavailable. */
	static const GUID clsid_fod = {0xDC1C5A9C,0xE88A,0x4dde,{0xA5,0xA1,0x60,0xF8,0x2A,0x20,0xAE,0xF7}} ;
	static const GUID iid_fod   = {0xd57c7288,0xd4ad,0x4768,{0xbe,0x02,0x9d,0x96,0x95,0x32,0xd9,0x60}} ;
	static const GUID iid_si    = {0x43826d1e,0xe718,0x42ee,{0xbc,0x55,0xa1,0xe2,0x61,0xc3,0x7b,0xfe}} ;   /* IShellItem */
	IFileOpenDialog *pfd = NULL ;
	HRESULT hrInit = CoInitializeEx( NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE ) ;
	if( SUCCEEDED( CoCreateInstance( &clsid_fod, NULL, CLSCTX_INPROC_SERVER,
	                                 &iid_fod, (void**)&pfd ) ) && pfd ) {
		DWORD opts = 0 ;
		pfd->lpVtbl->GetOptions( pfd, &opts ) ;
		pfd->lpVtbl->SetOptions( pfd, opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST ) ;
		if( title ) {
			wchar_t *wt = kitty_acp_to_wide_dup( title ) ;
			if( wt ) { pfd->lpVtbl->SetTitle( pfd, wt ) ; sfree( wt ) ; }
		} else {
			pfd->lpVtbl->SetTitle( pfd, L"Select a folder..." ) ;
		}
		if( initial ) {
			/* SHCreateItemFromParsingName is Vista+: resolved at run time so
			 * the XP build still loads (a static import the OS lacks kills the
			 * process in the loader). */
			typedef HRESULT (WINAPI *pfn_scifpn)( PCWSTR, IBindCtx *, REFIID, void ** ) ;
			HMODULE sh = GetModuleHandleA( "shell32.dll" ) ;
			pfn_scifpn scifpn = sh ? (pfn_scifpn)GetProcAddress( sh, "SHCreateItemFromParsingName" ) : NULL ;
			wchar_t *wi = scifpn ? kitty_acp_to_wide_dup( initial ) : NULL ;
			if( wi ) {
				IShellItem *psi0 = NULL ;
				if( SUCCEEDED( scifpn( wi, NULL, &iid_si, (void**)&psi0 ) ) && psi0 ) {
					pfd->lpVtbl->SetFolder( pfd, psi0 ) ;
					psi0->lpVtbl->Release( psi0 ) ;
				}
				sfree( wi ) ;
			}
		}
		if( SUCCEEDED( pfd->lpVtbl->Show( pfd, hFrame ) ) ) {
			IShellItem *psi = NULL ;
			if( SUCCEEDED( pfd->lpVtbl->GetResult( pfd, &psi ) ) && psi ) {
				PWSTR wpath = NULL ;
				if( SUCCEEDED( psi->lpVtbl->GetDisplayName( psi, SIGDN_FILESYSPATH, &wpath ) ) && wpath ) {
					/* whole, whatever its length in the code page: it was
					 * written unchecked into the caller's buffer */
					dirname = kitty_wide_to_acp_dup( wpath ) ;
					CoTaskMemFree( wpath ) ;
				}
				psi->lpVtbl->Release( psi ) ;
			}
		}
		pfd->lpVtbl->Release( pfd ) ;
		if( SUCCEEDED( hrInit ) ) CoUninitialize() ;
		if( dirname && !dirname[0] ) { sfree( dirname ) ; dirname = NULL ; }
		return dirname ;
	}
	if( SUCCEEDED( hrInit ) ) CoUninitialize() ;

	/* Fallback: classic tree picker (with the New Folder button). The path
	 * comes back wide, in the MAX_PATH characters the call is made for, and
	 * is converted whole. */
	{
		BROWSEINFO bi ; LPITEMIDLIST il ; char Buffer[4096] ; wchar_t Result[MAX_PATH + 1] ;
		memset( &bi, 0, sizeof(bi) ) ;
		bi.hwndOwner = hFrame ;
		bi.pszDisplayName = Buffer ;
		bi.lpszTitle = title ? title : KT_CAP_SELECT_FOLDER ;
		bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE ;
		if( initial ) { bi.lpfn = browse_start_folder ; bi.lParam = (LPARAM)initial ; }
		if( (il = SHBrowseForFolder( &bi )) != NULL ) {
			Result[0] = L'\0' ;
			if( !SHGetPathFromIDListW( il, Result ) ) Result[0] = L'\0' ;
			Result[MAX_PATH] = L'\0' ;
			GlobalFree( il ) ;
			if( Result[0] ) dirname = kitty_wide_to_acp_dup( Result ) ;
		}
	}
	return dirname ;
	}


//
// Send to the printer
//
// Printing parameters
int PrintCharSize = 100 ;
int PrintMaxLinePerPage = 60 ;
int PrintMaxCharPerLine = 85 ;

/* The print dialog and the printing; *report = the line for the user, or
 * NULL (cancelled, nothing to print). */
static int print_text( const char * Text, const char ** report ) {
	int return_code = 0 ;
	PRINTDLG	pd;
	DOCINFO		di;
	int i, TextLen = 0, Index1 = 0, Index2 = 2;
	//int Exit = 0 ;
	char*		LinePrint = NULL ;
	const char*	szMessage = NULL ;

	*report = NULL ;
	if( Text == NULL ) return 1 ;
	if( strlen( Text ) == 0 ) return 1 ;

	memset (&pd, 0, sizeof(PRINTDLG));
	memset (&di, 0, sizeof(DOCINFO));

	di.cbSize = sizeof(DOCINFO);
	di.lpszDocName = "Test";

	pd.lStructSize = sizeof(PRINTDLG);
	pd.Flags = PD_PAGENUMS | PD_RETURNDC;
	pd.nFromPage = 1;
	pd.nToPage = 1;
	pd.nMinPage = 1;
	pd.nMaxPage = 1;
	szMessage = 0;

	if( PrintDlg( &pd ) ) {
		if( pd.hDC ) {
			if (StartDoc (pd.hDC, &di) != SP_ERROR)	{
				TextLen = strlen( Text ) ;
				if( TextLen > 0 ) {
					LinePrint = (char*) malloc( TextLen + 2 ) ;
					Index1 = 0 ; Index2 = 2 ; 
					//Exit = 0 ;
					for( i = 0 ; i < TextLen ; i++ ) {
						if( Text[i]=='\r' ) i++;
                    				LinePrint[Index1] = Text[i] ;
                    				if( Text[i] == '\n' ) {
                      					Index2++ ;
							LinePrint[Index1] = '\0' ;
                      					TextOut(pd.hDC,100, Index2*PrintCharSize, LinePrint, strlen(LinePrint) ) ;
							Index1 = 0 ;
                    					}
						else if( Index1>=PrintMaxCharPerLine ) {
							Index2++ ;
							LinePrint[Index1+1] = '\0' ;
                      					TextOut(pd.hDC,100, Index2*PrintCharSize, LinePrint, strlen(LinePrint) ) ;
							Index1 = 0 ;
							}
                    				else { Index1++ ; }
                    				if( Index2 >= PrintMaxLinePerPage ) {
                  	   				EndPage( pd.hDC ) ;
                       					//EndDoc(pd.hDC) ;
                       					//StartDoc(pd.hDC, &di) ;
							StartPage( pd.hDC ) ;
                       					Index2 = 2 ;
                       					}
                  				}
                  			Index2++ ; 
                  			LinePrint[Index1] = '\0'; // Print the last page
                  			TextOut(pd.hDC,100, Index2*PrintCharSize, LinePrint, strlen(LinePrint)) ;
               	  			EndPage(pd.hDC) ;
                  			EndDoc(pd.hDC) ;
                  			szMessage = KT_WIN_PRINT_OK;
					free( LinePrint ) ;
              				}
              			else { return_code = 1 ;  /* Empty string */ }
				}
			else { // StartDoc problem
				szMessage = KT_WIN_PRINT_ERR1 ;
				return_code = 2 ;
				}
			}
		else { // pd.hDC problem
			szMessage = KT_WIN_PRINT_ERR2 ;
			return_code = 3 ;
			}
		}
	else { // PrintDlg problem
		//szMessage = "Impression annulee par l'utilisateur" ;
		return_code = 4 ;
		}
	*report = szMessage ;
	return return_code ;
	}

int PrintText( const char * Text ) {
	const char * report ;
	int rc = print_text( Text, &report ) ;
	if( report ) { MessageBox( NULL, report, KT_CAP_PRINT_REPORT, MB_OK ) ; }
	return rc ;
	}

/*
 * Print the clipboard (Tools > Print clipboard). PrintDlg and the printing
 * behind it run beside the session (kitty_call_async): PrintDlg's own loop
 * serves no network events, so the session stood still while it was open.
 * The text is a copy taken first, so the clipboard is not held open while
 * the dialog waits; the report comes back to the terminal's thread and goes
 * to a modeless box. One print at a time: a second request while one is
 * under way does nothing.
 */
struct kitty_print_job {
	HWND hwnd ;
	char * text ;
	const char * report ;
} ;
static int kitty_print_busy = 0 ;

static void kitty_print_run( void * ctx ) {
	struct kitty_print_job * j = (struct kitty_print_job *)ctx ;
	print_text( j->text, &j->report ) ;
}

static void kitty_print_done( void * ctx ) {
	struct kitty_print_job * j = (struct kitty_print_job *)ctx ;
	kitty_print_busy = 0 ;
	if( j->report ) {
		HWND owner = IsWindow( j->hwnd ) ? j->hwnd : NULL ;
		if( !kitty_info_modeless( owner, KT_CAP_PRINT_REPORT, j->report, NULL, NULL ) )
			MessageBox( owner, j->report, KT_CAP_PRINT_REPORT, MB_OK ) ;
	}
	free( j->text ) ;
	free( j ) ;
}

void ManagePrint( HWND hwnd ) {
	char *pst = NULL, *text = NULL ;
	struct kitty_print_job * j ;
	if( kitty_print_busy ) return ;
	if( OpenClipboard(NULL) ) {
		HGLOBAL hglb ;
		if( (hglb = GetClipboardData( CF_TEXT ) ) != NULL ) {
			if( ( pst = GlobalLock( hglb ) ) != NULL ) {
				text = _strdup( pst ) ;
				GlobalUnlock( hglb ) ;
			}
		}
		CloseClipboard();
	}
	if( text == NULL ) return ;
	j = calloc( 1, sizeof(*j) ) ;
	if( j != NULL ) {
		j->hwnd = hwnd ;
		j->text = text ;
		if( kitty_call_async( kitty_print_run, kitty_print_done, j ) ) {
			kitty_print_busy = 1 ;
			return ;
		}
		free( j ) ;
	}
	PrintText( text ) ;           /* no thread for it: here, as before */
	free( text ) ;
}

// Put a text into the clipboard
int SetTextToClipboard( const char * buf ) {
	HGLOBAL hglbCopy ;
	LPTSTR lptstrCopy ;
	int attempt ;
	/* NO IsClipboardFormatAvailable(CF_TEXT) TEST HERE. It used to guard this
	 * function, which made putting text on the clipboard depend on text already
	 * being on it: with an image copied, or nothing copied since login, every
	 * Copy in KiTTY silently did nothing and returned 0. That test belongs to
	 * READING the clipboard, not writing it.
	 *
	 * The clipboard is a single system-wide resource and one process holds it at
	 * a time, so OpenClipboard failing is normal and transient - it is worth
	 * waiting for rather than silently dropping the text to be copied. */
	for( attempt = 0 ; attempt < 10 ; attempt++ ) {
		if( OpenClipboard(NULL) ) break ;
		Sleep( 20 ) ;
	}
	if( attempt >= 10 ) return 0 ;
	EmptyClipboard() ; 
	if( (hglbCopy= GlobalAlloc(GMEM_MOVEABLE, (strlen(buf)+1) * sizeof(TCHAR)) ) == NULL ) {
		CloseClipboard() ; 
		return 0 ;	
	}
	lptstrCopy = GlobalLock( hglbCopy ) ; 
	memcpy( lptstrCopy, buf, (strlen(buf)+1) * sizeof(TCHAR) ) ;
	GlobalUnlock( hglbCopy ) ; 
	if( SetClipboardData(CF_TEXT, hglbCopy) == NULL ) {
		CloseClipboard() ;
		return 0 ; 
	}
	CloseClipboard() ;
	return 1 ;
}

/* Run a command: 1 started; 0 neither CreateProcess nor the ShellExecute
 * fallback could start it, with the command and Windows' reason shown to
 * the user. It closed the ADDRESSES of its two structs (leaking both handles of
 * every program it started) and never looked at the fallback's result, so
 * WinSCP, FileZilla, the ini editor and the launcher's starts failed silently. */
/* report 0: nothing shown or logged on a failure, Windows' reason left in
 * GetLastError() for the caller (kitty_edit_file_cli). */
static int run_command_ex( HWND hwnd, const char * cmd, int report ) {
	PROCESS_INFORMATION ProcessInformation ;
	ZeroMemory( &ProcessInformation, sizeof(ProcessInformation) );
	
	STARTUPINFO StartUpInfo ;
	ZeroMemory( &StartUpInfo, sizeof(StartUpInfo) );
	StartUpInfo.cb=sizeof(STARTUPINFO);
	StartUpInfo.lpReserved=0;
	StartUpInfo.lpDesktop=0;
	StartUpInfo.lpTitle=0;
	StartUpInfo.dwX=0;
	StartUpInfo.dwY=0;
	StartUpInfo.dwXSize=0;
	StartUpInfo.dwYSize=0;
	StartUpInfo.dwXCountChars=0;
	StartUpInfo.dwYCountChars=0;
	StartUpInfo.dwFillAttribute=0;
	StartUpInfo.dwFlags=0;
	StartUpInfo.wShowWindow=0;
	StartUpInfo.cbReserved2=0;
	StartUpInfo.lpReserved2=0;
	StartUpInfo.hStdInput=0;
	StartUpInfo.hStdOutput=0;
	StartUpInfo.hStdError=0;
//MessageBox(hwnd,cmd,"Info",MB_OK);

	/* SEM_FAILCRITICALERRORS for the start: a path to a file that is no
	 * program makes Windows put up its own modal "Unsupported 16-Bit
	 * Application" box, which stood in front of the session. SetErrorMode,
	 * not SetThreadErrorMode: the latter does not exist on XP. */
	UINT olderr = SetErrorMode( 0 ) ;
	BOOL started ;
	SetErrorMode( olderr | SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX ) ;
	started = CreateProcess(NULL,(CHAR*)cmd,NULL,NULL,FALSE,NORMAL_PRIORITY_CLASS,NULL,NULL,&StartUpInfo,&ProcessInformation) ;
	if( !started ) {
		DWORD err = GetLastError() ;
		/* The shell's fallback with NO_UI: plain ShellExecute puts up its
		 * own modal "cannot find" box over the terminal on a failure. */
		SHELLEXECUTEINFOA sei ;
		ZeroMemory( &sei, sizeof(sei) ) ;
		sei.cbSize = sizeof(sei) ;
		sei.fMask = SEE_MASK_FLAG_NO_UI ;
		sei.hwnd = hwnd ;
		sei.lpVerb = "open" ;
		sei.lpFile = cmd ;
		sei.nShow = SW_SHOWDEFAULT ;
		if( !ShellExecuteExA( &sei ) ) {
			DWORD e2 = GetLastError() ;
			SetErrorMode( olderr ) ;
			if( report ) {
				/* Reported in the notice window (it never takes the focus) and
				 * the Event Log: a modal box here stood in front of the live
				 * session and stopped it until it was answered. */
				const char *why = kitty_win_reason( err ? err : e2 ) ;
				char *msg = dupprintf( KT_START_FAILED_FMT, cmd, why ) ;
				debug_logevent( KT_START_FAILED_EVENT_FMT, cmd, why ) ;
				kitty_notice_show( KT_CAP_START_FAILED, msg, RGB( 190, 110, 0 ),
				                   GetNoticeSeconds(), NULL, 0 ) ;
				sfree( msg ) ;
			}
			SetLastError( err ? err : e2 ) ;
			return 0 ;
		}
	}
	SetErrorMode( olderr ) ;
	if( started ) {
		/* Grant the spawned session the right to bring its window to the
		 * foreground. Without this, the new KiTTY window's SetForegroundWindow()
		 * (window.c) is blocked by Windows' foreground lock when we launch from
		 * the tray launcher, so the window opens behind and never gets focus.
		 * No WaitForInputIdle: with INFINITE it held the terminal's thread -
		 * no output, no keepalive - until the program had loaded, and nothing
		 * here uses the program's window. The right granted above does not
		 * depend on the wait. */
		AllowSetForegroundWindow( ProcessInformation.dwProcessId ) ;
		CloseHandle( ProcessInformation.hThread );
		CloseHandle( ProcessInformation.hProcess );
	}
	return 1 ;
}

/* STARTUPINFOEXA and the handle-list attribute, spelt out here: the headers
 * declare them only for a Vista target. This build also runs on XP, so the
 * functions are looked up at run time instead. */
typedef struct { STARTUPINFOA StartupInfo ; void * lpAttributeList ; } kitty_startupinfoex ;
#define KITTY_PTA_HANDLE_LIST              0x00020002
#define KITTY_EXTENDED_STARTUPINFO_PRESENT 0x00080000
#define KITTY_SPAWN_MAX_HANDLES            4

/* Start a process that receives exactly the handles in h[] - the settings and
 * master-key mappings of a hand-over - and no other handle of this process.
 * The callers create those mappings NOT inheritable; they are inheritable only
 * for this call. Where Windows has PROC_THREAD_ATTRIBUTE_HANDLE_LIST (Vista
 * and later) the child gets these and nothing else, so a program started from
 * another thread at the same moment cannot pick up the master key. On XP it
 * falls back to plain inheritance for that moment. No usable handle: an
 * ordinary start without inheritance. Returns CreateProcessA's result, with
 * its GetLastError() kept for the caller. */
BOOL kitty_create_process_with_handles( const char * app, char * cmdline, DWORD flags,
                                        const char * dir, STARTUPINFOA * si,
                                        PROCESS_INFORMATION * pi,
                                        const HANDLE * h, int nh ) {
	typedef BOOL (WINAPI *init_fn)( void *, DWORD, DWORD, SIZE_T * ) ;
	typedef BOOL (WINAPI *update_fn)( void *, DWORD, DWORD_PTR, void *, SIZE_T, void *, SIZE_T * ) ;
	typedef void (WINAPI *delete_fn)( void * ) ;
	HANDLE list[KITTY_SPAWN_MAX_HANDLES] ;
	HMODULE k32 ;
	init_fn pinit = NULL ; update_fn pupd = NULL ; delete_fn pdel = NULL ;
	void * attrs = NULL ;
	SIZE_T sz = 0 ;
	int n = 0, i ;
	BOOL ok ;
	DWORD err ;

	for( i = 0 ; i < nh && n < KITTY_SPAWN_MAX_HANDLES ; i++ )
		if( h[i] != NULL && h[i] != INVALID_HANDLE_VALUE ) list[n++] = h[i] ;
	if( n == 0 )
		return CreateProcessA( app, cmdline, NULL, NULL, FALSE, flags, NULL, dir, si, pi ) ;

	for( i = 0 ; i < n ; i++ ) SetHandleInformation( list[i], HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT ) ;
	if( ( k32 = GetModuleHandleA( "kernel32.dll" ) ) != NULL ) {
		pinit = (init_fn)GetProcAddress( k32, "InitializeProcThreadAttributeList" ) ;
		pupd = (update_fn)GetProcAddress( k32, "UpdateProcThreadAttribute" ) ;
		pdel = (delete_fn)GetProcAddress( k32, "DeleteProcThreadAttributeList" ) ;
	}
	if( pinit && pupd && pdel ) {
		pinit( NULL, 1, 0, &sz ) ;   /* fails by design: it returns the size */
		if( sz && ( attrs = malloc( sz ) ) != NULL && !pinit( attrs, 1, 0, &sz ) ) { free( attrs ) ; attrs = NULL ; }
		if( attrs && !pupd( attrs, 0, KITTY_PTA_HANDLE_LIST, list, n * sizeof(HANDLE), NULL, NULL ) ) {
			pdel( attrs ) ; free( attrs ) ; attrs = NULL ;
		}
	}
	if( attrs ) {
		kitty_startupinfoex six ;
		memset( &six, 0, sizeof(six) ) ;
		six.StartupInfo = *si ;
		six.StartupInfo.cb = sizeof(six) ;
		six.lpAttributeList = attrs ;
		ok = CreateProcessA( app, cmdline, NULL, NULL, TRUE, flags | KITTY_EXTENDED_STARTUPINFO_PRESENT,
		                     NULL, dir, &six.StartupInfo, pi ) ;
		err = GetLastError() ;
		pdel( attrs ) ; free( attrs ) ;
	} else {
		ok = CreateProcessA( app, cmdline, NULL, NULL, TRUE, flags, NULL, dir, si, pi ) ;
		err = GetLastError() ;
	}
	for( i = 0 ; i < n ; i++ ) SetHandleInformation( list[i], HANDLE_FLAG_INHERIT, 0 ) ;
	SetLastError( err ) ;
	return ok ;
}

int RunCommand( HWND hwnd, const char * cmd ) {
	return run_command_ex( hwnd, cmd, 1 ) ;
}

/* kitty.exe -edit <file>: the editor started on the file, in a process of
 * its own. This process has no window and ends right after, so a failed
 * start is a modal box (it ends only after OK) and a line in Windows'
 * Application log, source KiTTY++, written whatever happens to the box -
 * as the startup guards write theirs (kitty_eventlog_line). */
void kitty_edit_file_cli( char * filename ) {
	char *module = kitty_module_path_dup( NULL ), *cmd = NULL ;
	char *msg, *line ;
	const char *why ;
	DWORD err ;
	if( module ) {
		cmd = dupprintf( "\"%s\" -edb %s", module, filename ) ;
		if( run_command_ex( NULL, cmd, 0 ) ) {
			sfree( cmd ) ;
			sfree( module ) ;
			return ;
		}
	}
	err = GetLastError() ;
	why = kitty_win_reason( err ) ;
	msg = dupprintf( KT_START_FAILED_FMT, cmd ? cmd : filename, why ) ;
	line = dupprintf( KT_START_FAILED_EVENT_FMT, cmd ? cmd : filename, why ) ;
	kitty_eventlog_line( EVENTLOG_ERROR_TYPE, line ) ;
	kitty_info_box( NULL, KT_CAP_START_FAILED, msg, NULL ) ;
	sfree( line ) ;
	sfree( msg ) ;
	sfree( cmd ) ;
	sfree( module ) ;
}

void RunPuttyEd( HWND hwnd, char * filename ) {
	/* Do not rely on GetShortPathName(): 8.3 short names can be disabled on
	 * modern Windows volumes, which made Shift+F2/Ctrl+Shift+F2 silently do
	 * nothing. Quote the real module path instead - whole: a cut one started
	 * whatever program that shorter path named. */
	char *module = kitty_module_path_dup( NULL ), *cmd ;
	if( module ) {
		if( filename != NULL && filename[0] )
			cmd = dupprintf( "\"%s\" -edb %s", module, filename ) ;
		else
			cmd = dupprintf( "\"%s\" -ed", module ) ;
		debug_logevent( "%s", cmd ) ;
		RunCommand( hwnd, cmd ) ;
		sfree( cmd ) ;
		sfree( module ) ;
	}
}

// Display a message in the event log
void debug_logevent( const char *fmt, ... ) {
	va_list ap;
	char *buf;
	va_start(ap, fmt);
	buf = dupvprintf(fmt, ap) ;
	va_end(ap);
	do_eventlog(buf) ;
	free(buf);
}

// Test whether a path is absolute
bool IsPathAbsolute( const char * path ) {
	bool test = false ;
	if( path == NULL ) { return false ; }
	if( strlen( path ) < 3 ) { return false ; }
	if( ((path[0]>='a') && (path[0]<='z')) || ((path[0]>='A') && (path[0]<='Z')) )
		if( path[1]==':' )
			if( (path[2]=='/') || (path[2]=='\\') ) test = true ;
	return test ;
}

/*
 * KiTTY: %VAR% expansion (cyd01/KiTTY#472) for the plain-string (non-
 * Filename) settings that hold a path: the download/upload folders, the
 * PSCPPath/WinSCPPath/FileZillaPath helper-program paths, the rzcommand/
 * szcommand ZModem programs (kitty_zmodem.c), and the sav= override. These never round-trip through the Filename type - each is a
 * char* read straight out of kitty.ini/the registry (ReadParameterN) into a
 * running global or a local buffer.
 *
 * Called ONLY at the point the stored text is turned into a path a real
 * Win32 API is about to use (existfile/existdirectory, GetShortPathName, the
 * actual open) - never where it is loaded for display or written back, so
 * the config box and the store keep showing/saving exactly what was typed,
 * e.g. "%OneDrive%\Kitty-Logs". An unknown %name% is left literal, which is
 * what ExpandEnvironmentStrings already does on its own.
 *
 * Returns a newly malloc'd string the caller frees with free() (matching
 * the malloc/strcpy idiom every call site here already uses for these
 * globals); never NULL, even if `in` is NULL (an empty string comes back).
 */
char *kitty_expand_env_dup(const char *in)
{
	DWORD need;
	char *out;

	if (in == NULL)
		in = "";

	need = ExpandEnvironmentStrings(in, NULL, 0);
	if (!need) {
		/* Expansion failed: fall back to the literal text, unchanged. */
		out = (char*) malloc(strlen(in) + 1);
		strcpy(out, in);
		return out;
	}

	out = (char*) malloc(need);
	if (!ExpandEnvironmentStrings(in, out, need)) {
		free(out);
		out = (char*) malloc(strlen(in) + 1);
		strcpy(out, in);
	}
	return out;
}
