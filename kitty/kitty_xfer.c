/*
 * KiTTY file-transfer and external-tool integration, moved verbatim out of
 * kitty.c to shrink that monolith: the pscp transfer window (live output
 * capture, cancel button, success tray balloon), the injection-hardened
 * pscp command builders (SendOneFile/SendFileList/SendFile,
 * GetOneFile/GetFile, RunCmd), external-tool path discovery
 * (SearchCtHelper/SearchWinSCP/SearchPSCP), StartWinSCP, and the
 * pscp-upload drag-and-drop handlers. Compiled into the same targets as
 * kitty.c (kitty + kitty_portable), so behaviour is unchanged.
 */
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "putty.h"

#include <windows.h>

#include "kitty.h"
#include "kitty_commun.h"    /* MASKPASS */

#include "kitty_tools.h"     /* existfile/existdirectory, poss, set_env */
#include "kitty_win.h"       /* OpenFileName */
#include "kitty_hello_keys.h" /* kageant_hello_has_sidecar */
#include "ssh.h"             /* the agent protocol, for the "is it loaded?" check */
/*
 * KiTTY: log a command line that had a password built into it.
 *
 * The command itself needs the secret - that is how pscp and WinSCP are
 * driven - but the Event Log copy does not, and the Event Log is scrollable,
 * copyable and saveable. Callers record where the secret landed (the buffer
 * length either side of the insertion) and this blanks exactly that span, so
 * it works whether the password was qcat-escaped, percent-encoded or plain.
 * A zero-length span means there was no password to hide.
 */
static void debug_logevent_redacted2(const char *what, const char *cmd,
                                     size_t secret_at, size_t secret_len,
                                     size_t secret2_at, size_t secret2_len)
{
    char *safe;
    size_t n;
    if (!debug_flag)
        return;
    if (secret_len == 0 && secret2_len == 0) {
        debug_logevent("%s: %s", what, cmd);
        return;
    }
    safe = dupstr(cmd);
    n = strlen(safe);
    if (secret_len > 0 && secret_at < n) {
        size_t end = secret_at + secret_len;
        if (end > n)
            end = n;
        memset(safe + secret_at, '*', end - secret_at);
    }
    if (secret2_len > 0 && secret2_at < n) {
        size_t end = secret2_at + secret2_len;
        if (end > n)
            end = n;
        memset(safe + secret2_at, '*', end - secret2_at);
    }
    debug_logevent("%s: %s", what, safe);
    smemclr(safe, strlen(safe));
    sfree(safe);
}

static void debug_logevent_redacted(const char *what, const char *cmd,
                                    size_t secret_at, size_t secret_len)
{
    debug_logevent_redacted2(what, cmd, secret_at, secret_len, 0, 0);
}


extern Conf *conf ;          /* the live session configuration (windows/window.c) */

/*
 * KiTTY: the key file to hand a transfer helper - or NULL.
 *
 * A Hello-protected key (a .hello sidecar beside the PPK) is withheld
 * deliberately. Neither pscp/psftp nor WinSCP knows anything about the
 * sidecar or its recovery doors, so all a -i / /privatekey= switch buys
 * is a passphrase prompt that only the never-shown secret would answer -
 * exactly the text that must not be typed into another program. kageant
 * unlocks such a key once and serves it over the agent pipe, which every
 * one of these helpers speaks, so withholding the path is what makes the
 * transfer work rather than what stops it.
 */
/*
 * Does the agent hold the key in this file? Compares PUBLIC halves, so
 * it works on a protected key without opening anything: the public half
 * of a PPK is plaintext even when the private half is encrypted.
 *
 * A "no" here is not an error - it is the one thing the user has to fix
 * before a transfer helper can use a Hello-protected key, so it is worth
 * saying out loud rather than letting pscp or WinSCP fail with a bare
 * "server refused our key" that names no cause.
 */
static bool kx_agent_holds(const char *keypath)
{
    Filename *fn ;
    strbuf *blob ;
    char *algorithm = NULL, *comment = NULL ;
    const char *error = NULL ;
    bool loaded, found = false ;
    void *resp = NULL ;
    int resplen = 0 ;

    if( !agent_exists() ) return false ;

    fn = filename_from_str( keypath ) ;
    blob = strbuf_new() ;
    loaded = ppk_loadpub_f( fn, &algorithm, BinarySink_UPCAST(blob),
                            &comment, &error ) ;
    filename_free( fn ) ;
    sfree( algorithm ) ; sfree( comment ) ;
    if( !loaded ) { strbuf_free( blob ) ; return false ; }

    /* agent_query with a NULL callback is the synchronous form (it reads
     * the pipe itself and returns no pending query) - aqsync.c's wrapper
     * would do the same, but it lives in a library kitty.exe does not
     * link, and its assert would be a crash rather than a failed check. */
    { strbuf *req = strbuf_new_for_agent_query() ;
      put_byte( req, SSH2_AGENTC_REQUEST_IDENTITIES ) ;
      if( agent_query( req, &resp, &resplen, NULL, 0 ) != NULL ) {
          resp = NULL ; resplen = 0 ;   /* async: not what we asked for */
      }
      strbuf_free( req ) ; }

    if( resp != NULL ) {
        BinarySource src[1] ;
        BinarySource_BARE_INIT( src, resp, resplen ) ;
        get_uint32( src ) ;                    /* length field */
        if( get_byte( src ) == SSH2_AGENT_IDENTITIES_ANSWER ) {
            size_t nkeys = get_uint32( src ), i ;
            for( i = 0 ; i < nkeys && !found ; i++ ) {
                ptrlen b = get_string( src ) ;
                get_string( src ) ;            /* comment */
                if( get_err( src ) ) break ;
                if( ptrlen_eq_ptrlen( b, ptrlen_from_strbuf( blob ) ) )
                    found = true ;
            }
        }
        sfree( resp ) ;
    }
    strbuf_free( blob ) ;
    return found ;
}

/*
 * The line to put in front of a transfer when the session's key is
 * Hello-protected and the agent does not hold it - NULL when there is
 * nothing to say. Caller sfrees.
 */
static char *kx_hello_agent_note(Conf *c)
{
    const char *path = filename_to_str( conf_get_filename( c, CONF_keyfile ) ) ;
    if( path==NULL || strlen(path)==0 ) return NULL ;
    if( !kageant_hello_has_sidecar( path ) ) return NULL ;
    if( kx_agent_holds( path ) ) return NULL ;
    return dupprintf(
        "This session's key is protected by Windows Hello, and the agent "
        "does not hold it.\r\n"
        "A transfer client cannot open a protected key file itself. Load "
        "the key in kageant (one Hello) and start the transfer again.\r\n"
        "Key: %s\r\n\r\n", path ) ;
}

static const char *kx_helper_keyfile(Conf *c)
{
    const char *path = filename_to_str(conf_get_filename(c, CONF_keyfile)) ;
    if( path==NULL || strlen(path)==0 ) return NULL ;
    if( kageant_hello_has_sidecar(path) ) {
        debug_logevent("transfer: key file is Hello-protected, left to the agent: %s", path) ;
        return NULL ;
    }
    return path ;
}

/* Provided elsewhere in the KiTTY tree (not in a header). */
char * kitty_current_dir() ;                            /* kitty.c */

// Envoi d'un fichier par SCP vers la racine du compte
int SearchPSCP( void ) ;
/* KiTTY security: launch a console command line WITHOUT a shell. Replaces
 * system()/"start" for the pscp/plink command builders below, so session fields
 * spliced into the command line cannot inject shell commands - CreateProcess does
 * NOT run cmd.exe, so & | > ` and friends are taken literally (kills the shell
 * command-injection class). A new console is created (pscp/plink are console
 * tools); `wait` blocks until exit (the old inline system() behaviour) or returns
 * immediately (the old "start" new-window behaviour). Returns 0 on success.
 * That staged follow-up is DONE, and this note is kept because it says what
 * the three quoting layers below are for: bcat bounds every append (no
 * overflow of buffer[4096]), qcat applies Win32 argv quoting to each single
 * value (no injected extra arguments), rawcat applies WinSCP's rawsettings
 * rule and urlcat percent-encodes URL userinfo. What is still appended RAW is
 * deliberate and documented at each site: pscpoptions, winscpoptions and
 * winscprawsettings are the user writing command line on purpose, and
 * quoting them would break the feature. */
static int kitty_run_noshell( char *cmdline, int wait ) {
	STARTUPINFOA si ; PROCESS_INFORMATION pi ;
	memset( &si, 0, sizeof(si) ) ; si.cb = sizeof(si) ;
	memset( &pi, 0, sizeof(pi) ) ;
	if( !CreateProcessA( NULL, cmdline, NULL, NULL, FALSE,
	                     CREATE_NEW_CONSOLE, NULL, NULL, &si, &pi ) )
		return -1 ;
	if( wait ) WaitForSingleObject( pi.hProcess, INFINITE ) ;
	CloseHandle( pi.hThread ) ; CloseHandle( pi.hProcess ) ;
	return 0 ;
}

/* Watch a launched transfer process (pscp/plink) on a background thread: wait
 * for it, and on a NON-zero exit pop a dialog with the exit code + a hint. Runs
 * off the GUI thread so a long transfer never freezes KiTTY, and the visible
 * console (CREATE_NEW_CONSOLE below) still shows live progress. Never shows the
 * command line (it carries -pw). */
/* Transient system-tray balloon (non-modal, auto-dismiss): add a short-lived
 * notify icon on `hwnd`, fire the balloon, keep it alive briefly, then remove
 * it. Used for the file-transfer SUCCESS notice. Safe if hwnd is gone (the
 * Shell_NotifyIcon calls just fail). Runs on the watcher thread (the Sleep is
 * off the GUI thread). */
static void kitty_tray_balloon( HWND hwnd, const char *title, const char *msg ) {
	static volatile LONG s_uid = 0xC000 ;
	NOTIFYICONDATA nid ;
	memset( &nid, 0, sizeof(nid) ) ;
	nid.cbSize = sizeof(nid) ;
	nid.hWnd = hwnd ;
	nid.uID = (UINT)InterlockedIncrement( &s_uid ) ;
	nid.uFlags = NIF_ICON | NIF_INFO ;
	nid.hIcon = LoadIcon( NULL, IDI_INFORMATION ) ;
	nid.dwInfoFlags = NIIF_INFO ;
	strncpy( nid.szInfoTitle, title, sizeof(nid.szInfoTitle)-1 ) ;
	strncpy( nid.szInfo,      msg,   sizeof(nid.szInfo)-1 ) ;
	if( Shell_NotifyIcon( NIM_ADD, &nid ) ) {
		Sleep( 8000 ) ;   /* keep the icon present while the balloon is shown */
		Shell_NotifyIcon( NIM_DELETE, &nid ) ;
	}
}

/* Fire the success tray balloon on its own short-lived thread (kitty_tray_balloon
 * Sleeps to keep the icon alive, so it must NOT run on the GUI thread). */
struct ktx_balloon { HWND hwnd ; char *title ; char *msg ; } ;
static DWORD WINAPI ktx_balloon_thread( LPVOID p ) {
	struct ktx_balloon *b = (struct ktx_balloon *)p ;
	kitty_tray_balloon( b->hwnd, b->title, b->msg ) ;
	sfree( b->title ) ; sfree( b->msg ) ; free( b ) ;
	return 0 ;
}
static void kitty_tray_balloon_async( HWND hwnd, const char *title, const char *msg ) {
	struct ktx_balloon *b = (struct ktx_balloon *)malloc( sizeof(*b) ) ;
	if( !b ) return ;
	b->hwnd = hwnd ; b->title = dupstr(title) ; b->msg = dupstr(msg) ;
	HANDLE t = CreateThread( NULL, 0, ktx_balloon_thread, b, 0, NULL ) ;
	if( t ) CloseHandle( t ) ; else { sfree(b->title) ; sfree(b->msg) ; free(b) ; }
}

/* ---- KiTTY file-transfer window -----------------------------------------
 * Runs pscp with its output captured to a pipe (no shell -> injection
 * hardening preserved) and streams it LIVE into a scrollable window. On
 * SUCCESS the window auto-closes and a tray balloon pops; on FAILURE the
 * window STAYS OPEN showing the full error context (so the user can read /
 * copy it), with a Close button. Never displays the command line (it carries
 * -pw). The window runs on the GUI thread; a reader thread pumps output to it
 * via posted messages, so KiTTY never blocks during a transfer. */
#define KTX_WM_APPEND (WM_APP+11)
#define KTX_WM_DONE   (WM_APP+12)
#define KTX_ID_EDIT   2001
#define KTX_ID_CLOSE  2002
struct ktx_win {
	HWND hwnd, edit, closebtn, parent ;
	HANDLE proc, rd, thread ;
	HFONT font, uifont ;   /* DPI-scaled: monospace output + UI-font button */
	char *what ;
	int done ;
	int cancelled ;
	/* mini line-discipline so pscp's \r progress meter overwrites the current
	 * line in place (terminal-style) instead of stacking new lines: */
	char curline[2048] ;   /* current uncommitted line */
	int  curcol ;          /* write cursor within curline (\r resets to 0) */
	int  curlen ;          /* length of curline */
	int  committed ;       /* edit-control char index where curline begins */
} ;

/* Make the edit-control tail (from w->committed to end) equal curline[0..curlen]. */
static void ktx_set_curline( struct ktx_win *w ) {
	char save = w->curline[w->curlen] ; w->curline[w->curlen] = 0 ;
	SendMessageA( w->edit, EM_SETSEL, w->committed, -1 ) ;
	SendMessageA( w->edit, EM_REPLACESEL, FALSE, (LPARAM)w->curline ) ;
	w->curline[w->curlen] = save ;
}

/* Feed captured pscp bytes through a tiny line-discipline: '\r' returns the
 * cursor to column 0 (so the progress meter overwrites its line in place),
 * '\n' commits the line and starts a new one, tabs expand, other control
 * chars are dropped. Keeps the live transfer readable as one updating line. */
static void ktx_feed( struct ktx_win *w, const char *s, int len ) {
	int i ;
	for( i=0 ; i<len ; i++ ) {
		char c = s[i] ;
		if( c=='\r' ) {
			w->curcol = 0 ;
		} else if( c=='\n' ) {
			ktx_set_curline( w ) ;
			int n = GetWindowTextLength( w->edit ) ;
			SendMessageA( w->edit, EM_SETSEL, n, n ) ;
			SendMessageA( w->edit, EM_REPLACESEL, FALSE, (LPARAM)"\r\n" ) ;
			w->committed = n + 2 ;
			w->curcol = 0 ; w->curlen = 0 ; w->curline[0] = 0 ;
		} else if( c=='\b' ) {
			if( w->curcol>0 ) w->curcol-- ;
		} else if( c=='\t' ) {
			do {
				if( w->curcol < (int)sizeof(w->curline)-1 ) {
					w->curline[w->curcol++] = ' ' ;
					if( w->curcol > w->curlen ) w->curlen = w->curcol ;
				}
			} while( (w->curcol % 8) && w->curcol < (int)sizeof(w->curline)-1 ) ;
		} else if( (unsigned char)c >= 0x20 ) {
			if( w->curcol < (int)sizeof(w->curline)-1 ) {
				w->curline[w->curcol++] = c ;
				if( w->curcol > w->curlen ) w->curlen = w->curcol ;
			}
		}
	}
	ktx_set_curline( w ) ;
	SendMessageA( w->edit, EM_SCROLLCARET, 0, 0 ) ;
}

static DWORD WINAPI ktx_reader_thread( LPVOID param ) {
	struct ktx_win *w = (struct ktx_win *)param ;
	char buf[4096] ; DWORD nrd ; DWORD code = (DWORD)-1 ;
	while( ReadFile( w->rd, buf, sizeof(buf), &nrd, NULL ) && nrd>0 ) {
		char *chunk = (char *)malloc( nrd ) ;
		if( chunk ) { memcpy( chunk, buf, nrd ) ;
			PostMessage( w->hwnd, KTX_WM_APPEND, (WPARAM)nrd, (LPARAM)chunk ) ; }
	}
	CloseHandle( w->rd ) ; w->rd = NULL ;
	WaitForSingleObject( w->proc, INFINITE ) ;
	GetExitCodeProcess( w->proc, &code ) ;
	/* leave w->proc open: the GUI thread owns it (for Cancel/TerminateProcess)
	 * and closes it in WM_DESTROY. We don't touch it again after this. */
	PostMessage( w->hwnd, KTX_WM_DONE, (WPARAM)code, 0 ) ;
	return 0 ;
}

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

/* (Re)create the transfer window's DPI-scaled fonts and apply them: a scalable
 * monospace font for the pscp output - the old ANSI_FIXED_FONT stock font was a
 * fixed 96-dpi bitmap font that rendered tiny on high-DPI displays - and a
 * scalable UI font for the Close/Cancel button. Any previous fonts are freed. */
static void ktx_apply_fonts( struct ktx_win *w, int dpi ) {
	HFONT of = w->font, ou = w->uifont ;
	w->font = CreateFont( -MulDiv(10, dpi, 72), 0,0,0, FW_NORMAL, 0,0,0,
		DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
		DEFAULT_QUALITY, FIXED_PITCH|FF_MODERN, "Consolas" ) ;
	w->uifont = CreateFont( -MulDiv(9, dpi, 72), 0,0,0, FW_NORMAL, 0,0,0,
		DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
		DEFAULT_QUALITY, DEFAULT_PITCH|FF_SWISS, "Segoe UI" ) ;
	SendMessage( w->edit, WM_SETFONT,
		(WPARAM)( w->font ? w->font : GetStockObject(ANSI_FIXED_FONT) ), TRUE ) ;
	if( w->uifont ) SendMessage( w->closebtn, WM_SETFONT, (WPARAM)w->uifont, TRUE ) ;
	if( of ) DeleteObject( of ) ;
	if( ou ) DeleteObject( ou ) ;
}

/* Esc closes (or cancels) the transfer window. Key events go to the focused
 * child control - the read-only edit or the Close button - whose default procs
 * ignore Esc, so we subclass both to forward Esc as a WM_CLOSE to the parent
 * (which then either closes a finished transfer or cancels a running one). */
static LRESULT CALLBACK ktx_child_subclass( HWND h, UINT msg, WPARAM wp, LPARAM lp ) {
	WNDPROC old = (WNDPROC)GetProp( h, "ktxoldproc" ) ;
	if( msg == WM_KEYDOWN && wp == VK_ESCAPE ) {
		SendMessage( GetParent(h), WM_CLOSE, 0, 0 ) ;
		return 0 ;
	}
	if( msg == WM_NCDESTROY ) {
		LRESULT r = old ? CallWindowProc( old, h, msg, wp, lp ) : DefWindowProc( h, msg, wp, lp ) ;
		RemoveProp( h, "ktxoldproc" ) ;
		return r ;
	}
	return old ? CallWindowProc( old, h, msg, wp, lp ) : DefWindowProc( h, msg, wp, lp ) ;
}
static void ktx_subclass_child( HWND h ) {
	WNDPROC old = (WNDPROC)(LONG_PTR)SetWindowLongPtr( h, GWLP_WNDPROC, (LONG_PTR)ktx_child_subclass ) ;
	SetProp( h, "ktxoldproc", (HANDLE)old ) ;
}

static LRESULT CALLBACK ktx_wndproc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp ) {
	struct ktx_win *w = (struct ktx_win *)GetWindowLongPtr( hwnd, GWLP_USERDATA ) ;
	switch( msg ) {
	  case WM_CREATE: {
		CREATESTRUCT *cs = (CREATESTRUCT *)lp ;
		w = (struct ktx_win *)cs->lpCreateParams ;
		SetWindowLongPtr( hwnd, GWLP_USERDATA, (LONG_PTR)w ) ;
		w->hwnd = hwnd ;
		w->edit = CreateWindowEx( WS_EX_CLIENTEDGE, "EDIT", "",
			WS_CHILD|WS_VISIBLE|WS_VSCROLL|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL,
			0,0,0,0, hwnd, (HMENU)(UINT_PTR)KTX_ID_EDIT, GetModuleHandle(NULL), NULL ) ;
		/* DPI-aware fonts are applied below, once both controls exist. */
		SendMessage( w->edit, EM_LIMITTEXT, (WPARAM)0x200000, 0 ) ;
		w->closebtn = CreateWindow( "BUTTON", "&Cancel",
			WS_CHILD|WS_VISIBLE|BS_DEFPUSHBUTTON,
			0,0,0,0, hwnd, (HMENU)(UINT_PTR)KTX_ID_CLOSE, GetModuleHandle(NULL), NULL ) ;
			{ HDC hdc = GetDC( hwnd ) ; int dpi = GetDeviceCaps( hdc, LOGPIXELSX ) ;
			  ReleaseDC( hwnd, hdc ) ; ktx_apply_fonts( w, dpi ) ; }
			ktx_subclass_child( w->edit ) ; ktx_subclass_child( w->closebtn ) ;
		return 0 ;
	  }
	  case WM_SIZE: {
		RECT rc ; GetClientRect( hwnd, &rc ) ;
		int pad, bh, bw ;
			{ HDC hdc = GetDC( hwnd ) ; int dpi = GetDeviceCaps( hdc, LOGPIXELSX ) ; ReleaseDC( hwnd, hdc ) ;
			  pad=MulDiv(8,dpi,96) ; bh=MulDiv(26,dpi,96) ; bw=MulDiv(90,dpi,96) ; }
		MoveWindow( w->edit, pad, pad, rc.right-2*pad, rc.bottom-bh-3*pad, TRUE ) ;
		MoveWindow( w->closebtn, rc.right-bw-pad, rc.bottom-bh-pad, bw, bh, TRUE ) ;
		return 0 ;
	  }
	  case WM_DPICHANGED: {
			if( w ) {
				ktx_apply_fonts( w, HIWORD(wp) ) ;
				RECT *r = (RECT *)lp ;
				SetWindowPos( hwnd, NULL, r->left, r->top,
					r->right-r->left, r->bottom-r->top,
					SWP_NOZORDER|SWP_NOACTIVATE ) ;   /* re-lays out via WM_SIZE */
			}
			return 0 ;
		  }
		  case KTX_WM_APPEND: {
		char *chunk = (char *)lp ;
		if( w && chunk ) ktx_feed( w, chunk, (int)wp ) ;
		if( chunk ) free( chunk ) ;
		return 0 ;
	  }
	  case KTX_WM_DONE: {
		DWORD code = (DWORD)wp ;
		if( !w ) return 0 ;
		w->done = 1 ;
		const char *what = w->what ? w->what : "Transfer" ;
		if( code == 0 && !w->cancelled ) {
			char *m = dupprintf( "%s complete.", what ) ;
			kitty_tray_balloon_async( w->parent, "KiTTY transfer", m ) ;
			sfree( m ) ;
			if( conf && conf_get_bool( conf, CONF_pscp_keep_window ) ) {
				char *t = dupprintf( "\r\n==== %s complete ====\r\n", what ) ;
				ktx_feed( w, t, (int)strlen(t) ) ; sfree( t ) ;
				SetWindowTextA( w->closebtn, "&Close" ) ;
				EnableWindow( w->closebtn, TRUE ) ;
				SetForegroundWindow( hwnd ) ; SetFocus( w->closebtn ) ;
			} else {
				DestroyWindow( hwnd ) ;   /* default: auto-close, balloon confirms */
			}
		} else {
			char *m ;
			if( w->cancelled ) {
				m = dupprintf( "\r\n==== %s cancelled ====\r\n", what ) ;
				SetWindowTextA( hwnd, "KiTTY transfer - cancelled" ) ;
			} else {
				const char *hint = ( code==127 )
					? "\r\n\r\nExit 127 = the server could not start the SCP/SFTP "
					  "subsystem (command not found). Try switching the transfer "
					  "protocol (Connection -> SSH -> WinSCP) between SCP "
					  "and SFTP, or check the server's sftp-server/scp."
					: "" ;
				m = dupprintf( "\r\n==== %s FAILED  (pscp exit code %lu) ====%s\r\n",
				               what, (unsigned long)code, hint ) ;
				char *t = dupprintf( "KiTTY transfer - FAILED (exit %lu)", (unsigned long)code ) ;
				SetWindowTextA( hwnd, t ) ; sfree( t ) ;
			}
			ktx_feed( w, m, (int)strlen(m) ) ; sfree( m ) ;
			SetWindowTextA( w->closebtn, "&Close" ) ;
			EnableWindow( w->closebtn, TRUE ) ;
			SetForegroundWindow( hwnd ) ;
			SetFocus( w->closebtn ) ;
		}
		return 0 ;
	  }
	  case WM_COMMAND:
		if( LOWORD(wp)==KTX_ID_CLOSE && w ) {
			if( w->done ) {
				DestroyWindow( hwnd ) ;          /* finished -> button is "Close" */
			} else if( !w->cancelled ) {
				w->cancelled = 1 ;               /* running -> button is "Cancel": kill pscp */
				if( w->proc ) TerminateProcess( w->proc, 2 ) ;
				SetWindowTextA( w->closebtn, "Stopping..." ) ;
				EnableWindow( w->closebtn, FALSE ) ;
			}
			return 0 ;
		}
		break ;
	  case WM_CLOSE:
		if( w && !w->done ) {                     /* X mid-transfer: cancel, wait for DONE */
			if( !w->cancelled ) {
				w->cancelled = 1 ;
				if( w->proc ) TerminateProcess( w->proc, 2 ) ;
				SetWindowTextA( w->closebtn, "Stopping..." ) ;
				EnableWindow( w->closebtn, FALSE ) ;
			}
			return 0 ;
		}
		DestroyWindow( hwnd ) ;
		return 0 ;
	  case WM_DESTROY:
		if( w ) {
			if( w->font ) DeleteObject( w->font ) ;
			if( w->uifont ) DeleteObject( w->uifont ) ;
			if( w->proc ) CloseHandle( w->proc ) ;
			if( w->thread ) CloseHandle( w->thread ) ;
			if( w->what ) sfree( w->what ) ;
			free( w ) ;
			SetWindowLongPtr( hwnd, GWLP_USERDATA, 0 ) ;
		}
		return 0 ;
	}
	return DefWindowProc( hwnd, msg, wp, lp ) ;
}

/* Launch pscp into a transfer window (above). No shell. Returns 0 if launched. */
static int kitty_run_xfer( HWND parent, char *cmdline, const char *what, const char *intro ) {
	static int registered = 0 ;
	HINSTANCE hi = GetModuleHandle( NULL ) ;
	if( !registered ) {
		WNDCLASS wc ; memset( &wc, 0, sizeof(wc) ) ;
		wc.lpfnWndProc = ktx_wndproc ;
		wc.hInstance = hi ;
		wc.hCursor = LoadCursor( NULL, IDC_ARROW ) ;
		wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE+1) ;
		wc.hIcon = LoadIcon( NULL, IDI_APPLICATION ) ;
		wc.lpszClassName = "KiTTYxferwin" ;
		RegisterClass( &wc ) ;
		registered = 1 ;
	}
	SECURITY_ATTRIBUTES sa ; HANDLE rd=NULL, wr=NULL ;
	memset( &sa, 0, sizeof(sa) ) ; sa.nLength = sizeof(sa) ; sa.bInheritHandle = TRUE ;
	if( !CreatePipe( &rd, &wr, &sa, 0 ) ) return -1 ;
	SetHandleInformation( rd, HANDLE_FLAG_INHERIT, 0 ) ;
	STARTUPINFOA si ; PROCESS_INFORMATION pi ;
	memset( &si, 0, sizeof(si) ) ; si.cb = sizeof(si) ;
	si.dwFlags = STARTF_USESTDHANDLES ;
	si.hStdOutput = wr ; si.hStdError = wr ; si.hStdInput = GetStdHandle( STD_INPUT_HANDLE ) ;
	memset( &pi, 0, sizeof(pi) ) ;
	if( !CreateProcessA( NULL, cmdline, NULL, NULL, TRUE,
	                     CREATE_NO_WINDOW, NULL, NULL, &si, &pi ) ) {
		CloseHandle( rd ) ; CloseHandle( wr ) ;
		MessageBox( NULL, "Could not launch the transfer client (pscp).",
		            "KiTTY transfer", MB_OK|MB_ICONERROR ) ;
		return -1 ;
	}
	CloseHandle( wr ) ; CloseHandle( pi.hThread ) ;
	struct ktx_win *w = (struct ktx_win *)malloc( sizeof(*w) ) ;
	if( !w ) { CloseHandle( pi.hProcess ) ; CloseHandle( rd ) ; return -1 ; }
	memset( w, 0, sizeof(*w) ) ;
	w->parent = parent ; w->proc = pi.hProcess ; w->rd = rd ;
	w->what = dupstr( what ? what : "Transfer" ) ;
	char *title = dupprintf( "KiTTY transfer - %s", w->what ) ;
	int dpi0 = 96 ;
	{ HDC pdc = GetDC( parent ) ; if( pdc ) { dpi0 = GetDeviceCaps( pdc, LOGPIXELSX ) ; ReleaseDC( parent, pdc ) ; } }
	HWND hwnd = CreateWindow( "KiTTYxferwin", title,
		WS_OVERLAPPEDWINDOW|WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
		MulDiv(680,dpi0,96), MulDiv(420,dpi0,96),
		parent, NULL, hi, w ) ;
	sfree( title ) ;
	if( !hwnd ) { CloseHandle( pi.hProcess ) ; CloseHandle( rd ) ; sfree( w->what ) ; free( w ) ; return -1 ; }
	SetForegroundWindow( hwnd ) ;          /* bring the transfer window to the front */
	BringWindowToTop( hwnd ) ;
	if( intro && *intro ) {          /* show the target (user@host:dir) up top */
		int n = GetWindowTextLength( w->edit ) ;
		SendMessageA( w->edit, EM_SETSEL, n, n ) ;
		SendMessageA( w->edit, EM_REPLACESEL, FALSE, (LPARAM)intro ) ;
		w->committed = GetWindowTextLength( w->edit ) ;
	}
	w->thread = CreateThread( NULL, 0, ktx_reader_thread, w, 0, NULL ) ;
	return 0 ;
}

/* Bounded string append: never writes past dst[cap-1], always NUL-terminates,
 * silently truncates rather than overflowing. Replaces the unbounded strcat()s
 * in the pscp/plink command builders. */
static void bcat( char *dst, size_t cap, const char *s ) {
	if( !s || cap==0 ) return ;
	size_t dl = strlen(dst) ;
	if( dl >= cap-1 ) return ;
	size_t room = cap-1-dl, sl = strlen(s) ;
	if( sl > room ) sl = room ;
	memcpy( dst+dl, s, sl ) ; dst[dl+sl] = '\0' ;
}

/* Append ONE argument, wrapped in double-quotes and escaped per Windows
 * CommandLineToArgvW rules, so a value containing a quote or space cannot inject
 * extra command-line arguments/switches. Bounded via bcat. Use for single-value
 * fields (password, key/file path, source path, the user@host:dir target); do NOT
 * use for fields that are intentionally raw option strings (pscpoptions etc.). */
static void qcat( char *dst, size_t cap, const char *s ) {
	char one[2] = {0,0} ;
	bcat( dst, cap, "\"" ) ;
	if( s ) {
		size_t nbs = 0 ;   /* run of pending backslashes */
		for( const char *p = s ; *p ; p++ ) {
			if( *p == '\\' ) { nbs++ ; }
			else if( *p == '"' ) {
				for( size_t k=0 ; k<2*nbs+1 ; k++ ) bcat(dst,cap,"\\") ;
				bcat( dst, cap, "\"" ) ; nbs = 0 ;
			} else {
				for( size_t k=0 ; k<nbs ; k++ ) bcat(dst,cap,"\\") ;
				nbs = 0 ; one[0]=*p ; bcat( dst, cap, one ) ;
			}
		}
		for( size_t k=0 ; k<2*nbs ; k++ ) bcat(dst,cap,"\\") ;   /* trailing backslashes before closing quote */
	}
	bcat( dst, cap, "\"" ) ;
}

/* Append a WinSCP /rawsettings VALUE, quoted and escaped.
 *
 * A third quoting layer, and not the same as either of the other two: rawsettings
 * are whitespace-separated, so a value with a space runs into the next setting,
 * and WinSCP's own rule for a literal quote inside a quoted parameter is to write
 * it TWICE ("Name=""a b""" - see its command-line documentation). qcat's
 * backslash escaping is Win32 argv grammar and means nothing here; urlcat's %XX
 * is URL grammar and would be stored literally.
 *
 * Use for every value we compose (proxy/tunnel host, user, password, commands,
 * Shell). Do NOT use for CONF_winscpoptions / CONF_winscprawsettings: those are
 * documented raw passthrough, where the user is writing command line on purpose.
 */
static void rawcat( char *dst, size_t cap, const char *s ) {
	char one[2] = {0,0} ;
	bcat( dst, cap, "\"" ) ;
	if( s ) for( const char *p = s ; *p ; p++ ) {
		if( *p == '"' ) bcat( dst, cap, "\"\"" ) ;
		else { one[0] = *p ; bcat( dst, cap, one ) ; }
	}
	bcat( dst, cap, "\"" ) ;
}

/* #535: percent-encode and append, for the userinfo (user/password) of a
 * WinSCP "proto://user:pass@host:port/dir" URL. Without this, an '@' '/' ':' or
 * '?' in the password or username is parsed as URL grammar by WinSCP -- e.g. a
 * password "x@evil.host/" redirects the connection to an attacker-chosen host.
 * Everything outside the RFC 3986 "unreserved" set is escaped as %XX. Bounded
 * via bcat. (qcat is Win32 argv quoting -- a different layer -- so it does NOT
 * cover this URL-grammar injection.) */
static void urlcat( char *dst, size_t cap, const char *s ) {
	static const char hexd[] = "0123456789ABCDEF" ;
	char esc[4] = {0,0,0,0} ;
	if( !s ) return ;
	for( const unsigned char *p = (const unsigned char *)s ; *p ; p++ ) {
		unsigned char c = *p ;
		if( (c>='A'&&c<='Z') || (c>='a'&&c<='z') || (c>='0'&&c<='9')
		    || c=='-' || c=='.' || c=='_' || c=='~' ) {
			esc[0]=(char)c ; esc[1]='\0' ; bcat( dst, cap, esc ) ;
		} else {
			esc[0]='%' ; esc[1]=hexd[(c>>4)&0xF] ; esc[2]=hexd[c&0xF] ; esc[3]='\0' ;
			bcat( dst, cap, esc ) ;
		}
	}
}

void SendOneFile( HWND hwnd, char * directory, char * filename, char * distantdir) {
	char buffer[4096], pscppath[4096]="", pscpport[4096]="22", remotedir[4096]=".",dir[4096], b1[256], tgt[4096] ;
	size_t pw_at = 0, pw_len = 0 ;   /* KiTTY: where the password lands in buffer */
	int p ;
	
	if( distantdir == NULL ) { distantdir = kitty_current_dir() ; } 
	if( PSCPPath==NULL ) {
		if( IniFileFlag == SAVEMODE_REG ) return ;
		else if( !SearchPSCP() ) return ;
	}
	if( !existfile( PSCPPath ) ) {
		if( IniFileFlag == SAVEMODE_REG ) return ;
		else if( !SearchPSCP() ) return ;
	}
		
	if( !GetShortPathName( PSCPPath, pscppath, 4095 ) ) return ;
	
	if( ReadParameterN( INIT_SECTION, "uploaddir", dir, sizeof(dir) ) ) {
		if( !existdirectory( dir ) ) 
			strcpy( dir, InitialDirectory ) ;
	}
	if (strlen( dir ) == 0) strcpy( dir, InitialDirectory ) ;

	if( (distantdir != NULL ) && ( strlen(distantdir)>0 ) ) {
		strcpy( remotedir, distantdir ) ;
	} else if( strlen(conf_get_str(conf,CONF_pscpremotedir))>0 ) {
		/* fixed remote dir sanity check: qcat below already quotes/escapes the
		 * whole user@host:dir argument (and pscp runs via CreateProcess, no
		 * shell), so injection is handled - but reject control characters that a
		 * quoted argument can't sensibly carry, falling back to the remote home. */
		const char * rd = conf_get_str(conf,CONF_pscpremotedir) ; const char * q ; int ok = 1 ;
		for( q = rd ; *q ; q++ ) if( (unsigned char)*q < 0x20 ) { ok = 0 ; break ; }
		strcpy( remotedir, ok ? rd : "." ) ;
	} else { strcpy( remotedir, "." ) ;
	}
	if( strlen( remotedir ) == 0 ) strcpy( remotedir, "." ) ;

	buffer[0] = '\0' ;
	const size_t BC = sizeof(buffer) ;

	bcat( buffer, BC, pscppath ) ; bcat( buffer, BC, " " ) ;   /* exe: 8.3 path, no spaces/quotes */

	if( strlen(conf_get_str(conf, CONF_pscpoptions))>0 ) {     /* raw user options - intentionally unquoted */
		bcat( buffer, BC, conf_get_str(conf, CONF_pscpoptions) ) ; bcat( buffer, BC, " " ) ;
	}
	bcat( buffer, BC, conf_get_int(conf, CONF_winscpprot)==0 ? "-scp " : "-sftp " ) ;

	if( ReadParameterN( INIT_SECTION, "pscpport", pscpport, sizeof(pscpport) ) ) {
		pscpport[17]='\0';
		if( !strcmp( pscpport,"*" ) ) snprintf( pscpport, sizeof(pscpport), "%d", conf_get_int(conf, CONF_port) ) ;
		bcat( buffer, BC, "-P " ) ; bcat( buffer, BC, pscpport ) ; bcat( buffer, BC, " " ) ;
	} else {
		if( (p=poss(":",conf_get_str(conf, CONF_sftpconnect) )) > 0 ) {
			snprintf( b1, sizeof(b1), "-P %d ", atoi(conf_get_str(conf, CONF_sftpconnect)+p) ) ;
		} else {
			snprintf( b1, sizeof(b1), "-P %d ", conf_get_int(conf, CONF_port) ) ;
		}
		bcat( buffer, BC, b1 ) ;
	}

	if( conf_get_int(conf, CONF_sshprot) == 3 ) { bcat( buffer, BC, "-2 " ) ; }   // SSH-2 Only

	if( strlen( conf_get_str(conf,CONF_password)) > 0 ) {
		/* CONF_password is plaintext at runtime; do NOT MASKPASS. qcat escapes any
		 * quote so the password can't inject an extra switch. The span is noted so
		 * the debug log can blank it - see debug_logevent_redacted(). */
		bcat( buffer, BC, "-pw " ) ;
		pw_at = strlen( buffer ) ;
		qcat( buffer, BC, conf_get_str(conf,CONF_password) ) ;
		pw_len = strlen( buffer ) - pw_at ;
		bcat( buffer, BC, " " ) ;
	}
	if( strlen( conf_get_str(conf,CONF_portknockingoptions)) > 0 ) {
		bcat( buffer, BC, "-knock " ) ; qcat( buffer, BC, conf_get_str(conf,CONF_portknockingoptions) ) ; bcat( buffer, BC, " " ) ;
	}
	{ const char *kf = kx_helper_keyfile(conf) ;
	  if( kf != NULL ) { bcat( buffer, BC, "-i " ) ; qcat( buffer, BC, kf ) ; bcat( buffer, BC, " " ) ; }
	}

	/* source path (single quoted argument) */
	{
		char src[4096] ; src[0]='\0' ;
		if( (strlen(directory)>0) && (strlen(filename)>0) ) {
			bcat(src,sizeof(src),directory) ; bcat(src,sizeof(src),"\\") ; bcat(src,sizeof(src),filename) ;
		} else if( (directory!=NULL)&&(strlen(directory)>0) ) {
			bcat(src,sizeof(src),directory) ;
		} else {
			bcat(src,sizeof(src),filename) ;
		}
		qcat( buffer, BC, src ) ; bcat( buffer, BC, " " ) ;
	}

	/* destination user@host:remotedir (single quoted argument) */
	{
		tgt[0]='\0' ;
		if( strlen( conf_get_str(conf, CONF_sftpconnect) ) > 0 ) {
			snprintf( b1, sizeof(b1), "%s", conf_get_str(conf, CONF_sftpconnect) ) ;
			if( (p=poss(":",b1)) > 0 ) { b1[p-1]='\0'; }
			bcat( tgt, sizeof(tgt), b1 ) ;
		} else {
			bcat( tgt, sizeof(tgt), conf_get_str_ambi(conf,CONF_username,NULL) ) ; bcat( tgt, sizeof(tgt), "@" ) ;
			if( poss( ":", conf_get_str(conf,CONF_host))>0 ) { bcat(tgt,sizeof(tgt),"[") ; bcat(tgt,sizeof(tgt),conf_get_str(conf,CONF_host)) ; bcat(tgt,sizeof(tgt),"]") ; }
			else { bcat( tgt, sizeof(tgt), conf_get_str(conf,CONF_host) ) ; }
		}
		bcat( tgt, sizeof(tgt), ":" ) ; bcat( tgt, sizeof(tgt), remotedir ) ;
		qcat( buffer, BC, tgt ) ;
	}

	chdir( InitialDirectory ) ;
	debug_logevent_redacted( "Run", buffer, pw_at, pw_len ) ;
	/* Capture output + show it on failure, instead of flashing a console shut
	 * (so e.g. a server's exit-127 "Cannot initialize SFTP" is readable). */
	{ char whatbuf[600] ; snprintf( whatbuf, sizeof(whatbuf), "Upload of \"%s\"", filename ? filename : "file" ) ;
	  char *note = kx_hello_agent_note( conf ) ;
	  char *intro = dupprintf( "%sUploading  %s  ->  %s\r\n\r\n",
	                           note ? note : "", filename ? filename : "file", tgt ) ;
	  kitty_run_xfer( hwnd, buffer, whatbuf, intro ) ;
	  sfree( intro ) ; sfree( note ) ; }

	//debug_log("%s\n",buffer);MessageBox( NULL, buffer, "Info",MB_OK );
	
	memset(buffer,0,strlen(buffer));
	}

void SendFileList( HWND hwnd, char * filelist ) {
	char *pname=NULL,dir[4096] ;
	int i;

	if( filelist==NULL ) return ;
	if( strlen( filelist ) == 0 ) return ;

	if( (filelist[strlen(filelist)]=='\0') && (filelist[strlen(filelist)+1]=='\0') ) {
		i=strlen(filelist) ;
		while( i>0 ) {
			i--;
			if( (filelist[i]=='/')||(filelist[i]=='\\') ) { filelist[i]='\0' ; break ; }
			}
		}
	snprintf( dir, sizeof(dir), "%s", filelist ) ;

	pname=filelist+strlen(filelist)+1;
	
	i = 0 ;
	while( pname[i] != '\0' ){
			
		while( (pname[i] != '\0') && (pname[i] != '\n') && (pname[i] != '\r') ) { i++ ; }
		pname[i]='\0';
		SendOneFile( hwnd, dir, pname, NULL ) ;
		pname=pname+i+1 ; i=0;
		}
		
	}

void SendFile( HWND hwnd ) {
	char filename[32768] ;

	if( conf_get_int(conf,CONF_protocol) != PROT_SSH ) {
		MessageBox( hwnd, "This function is only available with SSH connections.", "Error", MB_OK|MB_ICONERROR ) ;
		return ;
		}

	if( OpenFileName( hwnd, filename, "Send file...", "All files (*.*)|*.*|" ) ) 
		if( strlen( filename ) > 0 ) {
			SendFileList( hwnd, filename ) ;
		}
	}


// Get a remote file throught scp
/*
get()
{
echo "\033]0;__pw:"`pwd`"\007"
for file in ${*} ; do echo "\033]0;__rv:"${file}"\007" ; done
}
*/
void GetOneFile( HWND hwnd, char * directory, const char * filename ) {
    char buffer[4096], pscppath[4096]="", pscpport[4096]="22", dir[4096]=".", b1[256] ;
    int p;

    if( PSCPPath==NULL ) {
        if( IniFileFlag == SAVEMODE_REG ) return ;
        else if( !SearchPSCP() ) return ;
    }
    if( !existfile( PSCPPath ) ) {
        if( IniFileFlag == SAVEMODE_REG ) return ;
        else if( !SearchPSCP() ) return ;
    }

    if( !GetShortPathName( PSCPPath, pscppath, 4095 ) ) return ;

    if( ReadParameterN( INIT_SECTION, "downloaddir", dir, sizeof(dir) ) ) {
        if( !existdirectory( dir ) ) { strcpy( dir, InitialDirectory ) ; }
    }

    if(strlen( dir ) == 0) { strcpy( dir, InitialDirectory ) ; }

    buffer[0]='\0' ;
    const size_t BC = sizeof(buffer) ;

    bcat( buffer, BC, pscppath ) ; bcat( buffer, BC, " " ) ;

    if( strlen(conf_get_str(conf, CONF_pscpoptions))>0 ) {     /* raw user options - unquoted */
        bcat( buffer, BC, conf_get_str(conf, CONF_pscpoptions) ) ; bcat( buffer, BC, " " ) ;
    }
    bcat( buffer, BC, conf_get_int(conf, CONF_winscpprot)==0 ? "-scp " : "-sftp " ) ;

    if( ReadParameterN( INIT_SECTION, "pscpport", pscpport, sizeof(pscpport) ) ) {
        pscpport[17]='\0';
        if( !strcmp( pscpport,"*" ) ) snprintf( pscpport, sizeof(pscpport), "%d", conf_get_int(conf,CONF_port) ) ;
        bcat( buffer, BC, "-P " ) ; bcat( buffer, BC, pscpport ) ; bcat( buffer, BC, " " ) ;
    } else {
        if( (p=poss(":",conf_get_str(conf, CONF_sftpconnect) )) > 0 ) snprintf( b1, sizeof(b1), "-P %d ", atoi(conf_get_str(conf, CONF_sftpconnect)+p) ) ;
        else sprintf( b1, "-P %d ", conf_get_int(conf, CONF_port) ) ;
        bcat( buffer, BC, b1 ) ;
    }
    if( conf_get_int(conf,CONF_sshprot) == 3 ) { bcat( buffer, BC, "-2 " ) ; }   // SSH-2 Only

    if( strlen( conf_get_str(conf,CONF_password) ) > 0 ) {
        bcat( buffer, BC, "-pw " ) ; qcat( buffer, BC, conf_get_str(conf,CONF_password) ) ; bcat( buffer, BC, " " ) ;
    }
    if( strlen( conf_get_str(conf,CONF_portknockingoptions)) > 0 ) {
        bcat( buffer, BC, "-knock " ) ; qcat( buffer, BC, conf_get_str(conf,CONF_portknockingoptions) ) ; bcat( buffer, BC, " " ) ;
    }
    { const char *kf = kx_helper_keyfile(conf) ;
      if( kf != NULL ) { bcat( buffer, BC, "-i " ) ; qcat( buffer, BC, kf ) ; bcat( buffer, BC, " " ) ; }
    }

    /* remote source user@host:path (single quoted argument) */
    {
        char src[4096] ; src[0]='\0' ;
        if( strlen( conf_get_str(conf, CONF_sftpconnect) ) > 0 ) {
            snprintf( b1, sizeof(b1), "%s", conf_get_str(conf, CONF_sftpconnect) ) ;
            if( (p=poss(":",b1)) > 0 ) { b1[p-1]='\0'; }
            bcat( src, sizeof(src), b1 ) ;
        } else {
            bcat( src, sizeof(src), conf_get_str_ambi(conf,CONF_username,NULL) ) ; bcat( src, sizeof(src), "@" ) ;
            if( poss( ":", conf_get_str(conf,CONF_host) )>0 ) { bcat(src,sizeof(src),"[") ; bcat(src,sizeof(src),conf_get_str(conf,CONF_host)) ; bcat(src,sizeof(src),"]") ; }
            else { bcat( src, sizeof(src), conf_get_str(conf,CONF_host) ) ; }
        }
        bcat( src, sizeof(src), ":" ) ;
        if( filename[0]=='/' ) {
            bcat( src, sizeof(src), filename ) ;
        } else if( (directory!=NULL) && (strlen(directory)>0) && (strlen(filename)>0) ) {
            bcat(src,sizeof(src),directory) ; bcat(src,sizeof(src),"/") ; bcat(src,sizeof(src),filename) ;
        } else if( (directory!=NULL) && (strlen(directory)>0) ) {
            bcat(src,sizeof(src),directory) ; bcat(src,sizeof(src),"/*") ;
        } else {
            bcat(src,sizeof(src),filename) ;
        }
        qcat( buffer, BC, src ) ; bcat( buffer, BC, " " ) ;
    }
    qcat( buffer, BC, dir ) ;   /* local destination dir (single quoted argument) */
    //strcat( buffer, " > kitty.log 2>&1" ) ; //if( !system( buffer ) ) unlink( "kitty.log" ) ;

    chdir( InitialDirectory ) ;

    if( debug_flag ) { debug_logevent( "Get on file: %s", buffer) ; }
    /* Capture output + show on failure (no vanishing console). */
    { char whatbuf[600] ; snprintf( whatbuf, sizeof(whatbuf), "Download of \"%s\"", filename ? filename : "file" ) ;
      char *note = kx_hello_agent_note( conf ) ;
      kitty_run_xfer( hwnd, buffer, whatbuf, note ) ;    /* no target line, but say it if the key needs loading */
      sfree( note ) ; }

    //debug_log("%s\n",buffer);//MessageBox( NULL, buffer, "Info",MB_OK );

    memset(buffer,0,strlen(buffer));
}

// Get a remote file throught SCP
void GetFile( HWND hwnd ) {
    char buffer[4096]="", b1[256], *pst ;
    char dir[4096], pscppath[4096]="", pscpport[4096]="22" ;
    int p;

    if( conf_get_int(conf,CONF_protocol) != PROT_SSH ) {
        MessageBox( hwnd, "This function is only available with SSH connections.", "Error", MB_OK|MB_ICONERROR ) ;
        return ;
    }

    if( PSCPPath==NULL ) {
        if( IniFileFlag == SAVEMODE_REG ) return ;
        else if( !SearchPSCP() ) return ;
    }

    if( !existfile( PSCPPath ) ) {
        if( IniFileFlag == SAVEMODE_REG ) return ;
        else if( !SearchPSCP() ) return ;
    }

    if( !GetShortPathName( PSCPPath, pscppath, 4095 ) ) return ;

    if (!IsClipboardFormatAvailable(CF_TEXT)) return ;

    if( OpenClipboard(NULL) ) {
        HGLOBAL hglb ;

        if( (hglb = GetClipboardData( CF_TEXT ) ) != NULL ) {
            if( ( pst = GlobalLock( hglb ) ) != NULL ) {
//sprintf(buffer,"#%s#%d",pst,strlen(pst));MessageBox(hwnd,buffer,"Info",MB_OK);
                str_rtrim( pst, "\n\r \t" ) ;
//sprintf(buffer,"#%s#%d",pst,strlen(pst));MessageBox(hwnd,buffer,"Info",MB_OK);
                strcpy( buffer, "" ) ;
                if( strlen( pst ) > 0 ) {
                    if( ReadParameterN( INIT_SECTION, "downloaddir", dir, sizeof(dir) ) ) {
                        if( !existdirectory( dir ) ) {
                            strcpy( dir, InitialDirectory ) ;
                        }
                    } else if( OpenDirName( hwnd, dir ) ) {
                        if( !existdirectory( dir ) ) { GlobalUnlock( hglb ) ; CloseClipboard(); return ; }
                        //strcpy( dir, InitialDirectory ) ;
                    } else { 
                        return ; 
                    }
                    //else { strcpy( dir, InitialDirectory ) ; }

                    buffer[0]='\0' ;
                    bcat( buffer, sizeof(buffer), pscppath ) ; bcat( buffer, sizeof(buffer), " " ) ;
                    if( strlen(conf_get_str(conf, CONF_pscpoptions))>0 ) {   /* raw user options */
                        bcat( buffer, sizeof(buffer), conf_get_str(conf, CONF_pscpoptions) ) ; bcat( buffer, sizeof(buffer), " " ) ;
                    }
                    bcat( buffer, sizeof(buffer), conf_get_int(conf, CONF_winscpprot)==0 ? "-scp " : "-sftp " ) ;
                    if( conf_get_int(conf,CONF_sshprot) == 3 ) { bcat( buffer, sizeof(buffer), "-2 " ) ; }   // SSH-2 Only
                    if( ReadParameterN( INIT_SECTION, "pscpport", pscpport, sizeof(pscpport) ) ) {
                        pscpport[17]='\0';
                        if( !strcmp( pscpport,"*" ) ) { snprintf( pscpport, sizeof(pscpport), "%d", conf_get_int(conf,CONF_port) ) ; }
                        bcat( buffer, sizeof(buffer), "-P " ) ; bcat( buffer, sizeof(buffer), pscpport ) ; bcat( buffer, sizeof(buffer), " " ) ;
                    } else {
                        if( (p=poss(":",conf_get_str(conf, CONF_sftpconnect) )) > 0 ) snprintf( b1, sizeof(b1), "-P %d ", atoi(conf_get_str(conf, CONF_sftpconnect)+p) ) ;
                        else sprintf( b1, "-P %d ", conf_get_int(conf, CONF_port) ) ;
                        bcat( buffer, sizeof(buffer), b1 ) ;
                    }
                    if( strlen( conf_get_str(conf,CONF_password) ) > 0 ) {
                        bcat( buffer, sizeof(buffer), "-pw " ) ; qcat( buffer, sizeof(buffer), conf_get_str(conf,CONF_password) ) ; bcat( buffer, sizeof(buffer), " " ) ;
                    }
                    { const char *kf = kx_helper_keyfile(conf) ;
                      if( kf != NULL ) { bcat( buffer, sizeof(buffer), "-i " ) ; qcat( buffer, sizeof(buffer), kf ) ; bcat( buffer, sizeof(buffer), " " ) ; }
                    }
                    /* remote source user@host:path (single quoted argument) */
                    {
                        char src[4096] ; src[0]='\0' ;
                        if( strlen( conf_get_str(conf, CONF_sftpconnect) ) > 0 ) {
                            snprintf( b1, sizeof(b1), "%s", conf_get_str(conf, CONF_sftpconnect) ) ;
                            if( (p=poss(":",b1)) > 0 ) { b1[p-1]='\0'; }
                            bcat( src, sizeof(src), b1 ) ;
                        } else {
                            bcat( src, sizeof(src), conf_get_str_ambi(conf,CONF_username,NULL) ) ; bcat( src, sizeof(src), "@" ) ;
                            if( poss( ":", conf_get_str(conf,CONF_host))>0 ) { bcat(src,sizeof(src),"[") ; bcat(src,sizeof(src),conf_get_str(conf,CONF_host)) ; bcat(src,sizeof(src),"]") ; }
                            else { bcat( src, sizeof(src), conf_get_str(conf,CONF_host) ) ; }
                        }
                        bcat( src, sizeof(src), ":" ) ; bcat( src, sizeof(src), pst ) ;
                        qcat( buffer, sizeof(buffer), src ) ; bcat( buffer, sizeof(buffer), " " ) ;
                    }
                    qcat( buffer, sizeof(buffer), dir ) ;   /* local destination (single quoted argument) */
                }
                GlobalUnlock( hglb ) ;
            }
        }
        CloseClipboard();
    }
    if( strlen( buffer ) > 0 ) {
        chdir( InitialDirectory ) ;
        if( debug_flag ) { debug_logevent("Get file: %s", buffer) ; }
        if( kitty_run_noshell( buffer, 0 ) ) { MessageBox( NULL, buffer, "Transfer problem", MB_OK|MB_ICONERROR  ) ; }
        //if( !system( buffer ) ) unlink( "kitty.log" ) ;
    }
}

// Start a locale commande (Internet Explorer for example)
/* #9 hardening: RunCmd() runs text taken straight from the Windows clipboard as
 * a local command (default Ctrl+F5), so attacker-planted clipboard content could
 * execute on a single keypress. Two independent, PER-SESSION safeguards, both ON
 * by default and each toggled in the config dialog (Window/Selection): a
 * confirmation prompt (CONF_runcmdconfirm) and a post-launch tray balloon
 * (CONF_runcmdnotify, mirroring kageant's key-use balloon). */
void RunCmd( HWND hwnd ) {
    char buffer[4096]="", * pst = NULL ;
    if (!IsClipboardFormatAvailable(CF_TEXT)) return ;
    if( OpenClipboard(NULL) ) {
        HGLOBAL hglb ;

        if( (hglb = GetClipboardData( CF_TEXT ) ) != NULL ) {
            if( ( pst = GlobalLock( hglb ) ) != NULL ) {
                snprintf( buffer, sizeof(buffer), "%s", pst ) ;
                GlobalUnlock( hglb ) ;
            }
        }
        CloseClipboard();
    }
    if( strlen( buffer ) > 0 ) {
        if( conf_get_bool( conf, CONF_runcmdconfirm ) ) {
            char prompt[4096+160] ;
            snprintf( prompt, sizeof(prompt),
                "Run this command from the clipboard?\n\n%s", buffer ) ;
            if( MessageBox( hwnd, prompt, "KiTTY - run clipboard command",
                    MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON2 ) != IDYES ) return ;
        }
        chdir( InitialDirectory ) ;
        //system( buffer ) ;
        STARTUPINFO si ;
        PROCESS_INFORMATION pi ;
        ZeroMemory( &si, sizeof(si) );
        si.cb = sizeof(si);
        ZeroMemory( &pi, sizeof(pi) );
        if( !CreateProcess(NULL, buffer, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi) ) {
            ShellExecute(hwnd, "open", buffer,0, 0, SW_SHOWDEFAULT);
        }
        if( conf_get_bool( conf, CONF_runcmdnotify ) ) {
            char note[4096+64] ;
            snprintf( note, sizeof(note), "Ran clipboard command:\n%s", buffer ) ;
            kitty_tray_balloon_async( hwnd, "KiTTY", note ) ;
        }
    }
}

/* Execute a local command
cmd()
{
if [ $# -eq 0 ] ; then echo "Usage: cmd command" ; return 0 ; fi
printf "\033]0;__cm:"$@"\007"
}
*/
/* Send a file that is present locally
scriptfile()
{
if [ $# -eq 0 ] ; then echo "Usage: scriptfile filename" ; return 0 ; fi
printf "\033]0;__ls:"$@"\007"
}
*/
/* Start default browser
ie()
{
if [ $# -eq 0 ] ; then echo "Usage: ie url" ; return 0 ; fi
printf "\033]0;__ie:"$@"\007"
}
*/
/* Copy stdin into clipboard
function wcl {
  echo -ne '\e''[5i'
  cat $*
  echo -ne '\e''[4i'
  echo "Copied to Windows clipboard" 1>&2
}
*/
/* Start WinSCP pointing into the same directory
winscp() { printf "\033]0;__ws:"`pwd`"\007" ; printf "\033]0;__ti\007" ; }
winscp() { echo -ne "\033];__ws:${PWD}\007" ; }
*/
/* Start WinSCP with specific user, host and directory
wt() { printf "\033]0;__wt:"$(hostname)":"${USER}":"`pwd`"\007" ; printf "\033]0;__ti\007" ; }
*/
/* Start a duplicated session into the same directory
ds() { printf "\033]0;__ds:`pwd`\007" ; }
# Start a duplicated session with specific user, host and directory
dt() { printf "\033]0;__dt:"$(hostname)":"${USER}":"`pwd`"\007" ; }
*/

/* SECURITY (0.84.1.37): the ManageLocalCmd "__xy" remote-escape metacommand
 * dispatcher has been REMOVED. It was dead code in 0.84 (the OSC hook that fed
 * it was never forward-ported, so it had no caller) and carried a latent
 * command-injection / RCE surface (__cm/__pl/__ie/__ds, cf. CVE-2024-23749) if
 * ever re-wired. If a future feature needs remote metacommands, re-introduce a
 * hardened, opt-in implementation rather than restoring this. Its last
 * remnants (the RemotePath store fed by __pw, and the #525/CVE-2024-25003-
 * hardened host:user:path splitter for __dt/__wt) went in the ini hygiene
 * sweep: nothing set or consumed them anymore. */

// Recherche le chemin vers le programme cthelper.exe
/* If `candidate` names an existing file, adopt it as the tool path *out
 * (freeing any previous value), optionally export it to environment `envname`,
 * optionally persist it under kitty.ini/registry key `param`, and return 1;
 * otherwise leave *out untouched and return 0. Pass param=NULL when the value
 * came from `param` itself (no need to write it back) and envname=NULL when the
 * tool has no associated environment variable. Generalises the former
 * per-tool malloc+strcpy+WriteParameter tails in the Search* helpers. */
static int adopt_tool_path_if_exists( char **out, const char *candidate,
                                      const char *param, const char *envname ) {
	if( candidate == NULL || !existfile( candidate ) ) return 0 ;
	if( *out != NULL ) free( *out ) ;
	*out = (char*) malloc( strlen(candidate) + 1 ) ;
	strcpy( *out, candidate ) ;
	if( envname != NULL ) set_env( (char*)envname, *out ) ;
	if( param != NULL ) WriteParameter( INIT_SECTION, (char*)param, *out ) ;
	return 1 ;
}

int SearchCtHelper( void ) {
	char buffer[4096] ;
	if( CtHelperPath!=NULL ) {
		free(CtHelperPath) ;
		CtHelperPath=NULL ;
	}
	if( ReadParameterN( INIT_SECTION, "CtHelperPath", buffer, sizeof(buffer) ) != 0 ) {
		if( adopt_tool_path_if_exists( &CtHelperPath, buffer, NULL, "CTHELPER_PATH" ) ) return 1 ;
		else { DelParameter( INIT_SECTION, "CtHelperPath" ) ; }
	}
	snprintf( buffer, sizeof(buffer), "%s\\cthelper.exe", InitialDirectory ) ;
	if( adopt_tool_path_if_exists( &CtHelperPath, buffer, "CtHelperPath", "CTHELPER_PATH" ) ) return 1 ;
	return 0 ;
}
	
static int set_winscp_path_if_exists(const char *path)
{
	if( path != NULL && path[0] && existfile(path) ) {
		WinSCPPath = (char*) malloc( strlen(path) + 1 ) ;
		strcpy( WinSCPPath, path ) ;
		WriteParameter( INIT_SECTION, "WinSCPPath", WinSCPPath ) ;
		return 1 ;
	}
	return 0 ;
}

static int probe_winscp_env_dir(const char *envname, const char *subpath, char *buffer, size_t buflen)
{
	const char *base = getenv(envname) ;
	if( base == NULL || base[0] == '\0' ) return 0 ;
	snprintf( buffer, buflen, "%s\\%s", base, subpath ) ;
	return set_winscp_path_if_exists(buffer) ;
}

// Recherche le chemin vers le programme WinSCP
int SearchWinSCP( void ) {
	char buffer[4096] ;
	if( WinSCPPath!=NULL) { free(WinSCPPath) ; WinSCPPath = NULL ; }
	if( ReadParameterN( INIT_SECTION, "WinSCPPath", buffer, sizeof(buffer) ) != 0 ) {
		if( adopt_tool_path_if_exists( &WinSCPPath, buffer, NULL, NULL ) ) return 1 ;
		else { DelParameter( INIT_SECTION, "WinSCPPath" ) ; }
	}
	if( probe_winscp_env_dir("ProgramFiles", "WinSCP\\WinSCP.exe", buffer, sizeof(buffer)) ) return 1 ;
	if( probe_winscp_env_dir("ProgramFiles(x86)", "WinSCP\\WinSCP.exe", buffer, sizeof(buffer)) ) return 1 ;
	if( probe_winscp_env_dir("LOCALAPPDATA", "Programs\\WinSCP\\WinSCP.exe", buffer, sizeof(buffer)) ) return 1 ;
	if( probe_winscp_env_dir("ProgramFiles", "WinSCP3\\WinSCP3.exe", buffer, sizeof(buffer)) ) return 1 ;
	if( probe_winscp_env_dir("ProgramFiles(x86)", "WinSCP3\\WinSCP3.exe", buffer, sizeof(buffer)) ) return 1 ;
	snprintf( buffer, sizeof(buffer), "%s\\WinSCP.exe", InitialDirectory ) ;
	if( set_winscp_path_if_exists(buffer) ) return 1 ;
	if( ReadParameterN( INIT_SECTION, "winscpdir", buffer, sizeof(buffer) ) ) {
		buffer[4076]='\0';
		strcat( buffer, "\\" ) ; strcat( buffer, "WinSCP.exe" ) ;
		if( set_winscp_path_if_exists(buffer) ) return 1 ;
	}
	if( SearchPathA(NULL, "WinSCP.exe", NULL, sizeof(buffer), buffer, NULL) > 0 ) {
		if( set_winscp_path_if_exists(buffer) ) return 1 ;
	}
	return 0 ;
}

// Lance WinSCP à partir de la sesson courante eventuellement dans le repertoire courant
/* ALIAS UNIX A DEFINIR POUR DEMARRER WINSCP Dans le repertoire courant
winscp()
{
echo "\033]0;__ws:"`pwd`"\007"
}
Il faut ensuite simplement taper: winscp
(historique: jadis traite par ManageLocalCmd, supprime en 0.84.1.37 - voir la note securite plus haut)

Le chemin vers l'exécutable WinSCP est défini dans la variable WInSCPPath. Elle peut pointer sur un fichier .BAT pour passer des options supplémentaires.
@ECHO OFF
start "C:\Program Files\WinSCP\WinSCP.exe" "%1" "%2" "%3" "%4" "%5" "%6" "%7" "%8" "%9"
*/	
// winscp.exe [(sftp|ftp|scp)://][user[:password]@]host[:port][/path/[file]] [/privatekey=key_file] [/rawsettings (ProxyMethod=1) (Compression=1)]
void StartWinSCP( HWND hwnd, char * directory, char * host, char * user ) {
	size_t pw_at = 0, pw_len = 0 ;   /* KiTTY: where the password lands in cmd */
	size_t proxy_pw_at = 0, proxy_pw_len = 0 ;  /* ... and the proxy/tunnel one */
	char cmd[4096], shortpath[1024], buffer[4096], proto[10] ;
	int raw = 0;
	
	if( directory == NULL ) { directory = kitty_current_dir(); } 
	if( WinSCPPath==NULL ) {
		if( !SearchWinSCP() ) return ;
	}
	if( !existfile( WinSCPPath ) ) {
		if( !SearchWinSCP() ) return ;
	}
		
	if( !GetShortPathName( WinSCPPath, shortpath, 4095 ) ) return ;

	switch( conf_get_int(conf, CONF_winscpprot) ) {
		case 0: strcpy( proto, "scp" ) ; break ;
		case 2: strcpy( proto, "ftp" ) ; break ;
		case 3: strcpy( proto, "ftps" ) ; break ;
		case 4: strcpy( proto, "ftpes" ) ; break ;
		case 5: strcpy( proto, "http" ) ; break ;
		case 6: strcpy( proto, "https" ) ; break ;
		default: strcpy( proto, "sftp" ) ;
	}
	
	if( conf_get_int(conf,CONF_protocol) == PROT_SSH ) {
		snprintf( cmd, sizeof(cmd), "\"%s\" %s://", shortpath, proto ) ;
			
		if( strlen( conf_get_str(conf, CONF_sftpconnect) ) > 0 ) {
			bcat( cmd, sizeof(cmd), conf_get_str(conf, CONF_sftpconnect) ) ;
		} else {
			urlcat( cmd, sizeof(cmd), user!=NULL ? user : conf_get_str_ambi(conf,CONF_username,NULL) ) ;
			if( strlen( conf_get_str(conf,CONF_password) ) > 0 ) {
				/* plaintext at runtime; do NOT MASKPASS. (Goes into the WinSCP URL -- #535: percent-encode so '@' '/' etc. can't redirect the host.) */
				bcat( cmd, sizeof(cmd), ":" ) ;
				pw_at = strlen( cmd ) ;
				urlcat( cmd, sizeof(cmd), conf_get_str(conf,CONF_password) ) ;
				pw_len = strlen( cmd ) - pw_at ;
			}
			bcat( cmd, sizeof(cmd), "@" ) ;
			if( poss( ":", host!=NULL ? host : conf_get_str(conf,CONF_host) )>0 ) { bcat(cmd,sizeof(cmd),"[") ; bcat(cmd,sizeof(cmd), host!=NULL ? host : conf_get_str(conf,CONF_host)) ; bcat(cmd,sizeof(cmd),"]") ; }
			else { bcat( cmd, sizeof(cmd), host!=NULL ? host : conf_get_str(conf,CONF_host) ) ; }
			bcat( cmd, sizeof(cmd), ":" ) ; snprintf( buffer, sizeof(buffer), "%d", conf_get_int(conf,CONF_port) ) ; bcat( cmd, sizeof(cmd), buffer ) ;
		}

		if( directory!=NULL ) if( strlen(directory)>0 ) {
			bcat( cmd, sizeof(cmd), directory ) ;
			if( directory[strlen(directory)-1]!='/' ) bcat( cmd, sizeof(cmd), "/" ) ;
		}
		{ const char *kf = kx_helper_keyfile(conf) ;
		  if( kf != NULL ) {
			if( GetShortPathName( kf, shortpath, 4095 ) ) {
				bcat( cmd, sizeof(cmd), " \"/privatekey=" ) ;
				bcat( cmd, sizeof(cmd), shortpath ) ;
				bcat( cmd, sizeof(cmd), "\"" ) ;
			}
		  }
		}
	} else {
		snprintf( cmd, sizeof(cmd), "\"%s\" %s://", shortpath, proto ) ;
		urlcat( cmd, sizeof(cmd), conf_get_str_ambi(conf,CONF_username,NULL) ) ; /* #535: percent-encode userinfo */
		if( strlen( conf_get_str(conf,CONF_password) ) > 0 ) {
			char bufpass[1024] ;
			bcat( cmd, sizeof(cmd), ":" ) ;
			snprintf(bufpass,sizeof(bufpass),"%s",conf_get_str(conf,CONF_password)); /* bounded */
			/* plaintext at runtime; do NOT MASKPASS */
			urlcat( cmd, sizeof(cmd), bufpass ) ;
			memset(bufpass,0,strlen(bufpass));
		}
		bcat( cmd, sizeof(cmd), "@" ) ;
		if( poss( ":", conf_get_str(conf,CONF_host) )>0 ) { bcat( cmd, sizeof(cmd), "[" ) ; bcat( cmd, sizeof(cmd), conf_get_str(conf,CONF_host) ) ; bcat( cmd, sizeof(cmd), "]" ) ; }
		else { bcat( cmd, sizeof(cmd), conf_get_str(conf,CONF_host) ) ; }
		bcat( cmd, sizeof(cmd), ":21" ) ;
		if( directory!=NULL ) if( strlen(directory)>0 ) {
			bcat( cmd, sizeof(cmd), directory ) ;
			if( directory[strlen(directory)-1]!='/' ) bcat( cmd, sizeof(cmd), "/" ) ;
		}
	}
	
	if( strlen(conf_get_str(conf, CONF_winscpoptions))>0 ) {
		bcat( cmd, sizeof(cmd), " " ) ; bcat( cmd, sizeof(cmd), conf_get_str(conf, CONF_winscpoptions) ) ;
	}

	/* Proxy -> WinSCP.  Two different WinSCP features hide behind PuTTY's one
	 * "Proxy type" list, and they are not interchangeable:
	 *
	 *   - SOCKS4/SOCKS5/HTTP/Telnet/Local command are WinSCP's ProxyMethod,
	 *     and PuTTY's PROXY_SOCKS4..PROXY_CMD happen to share WinSCP's 1..5.
	 *     Match on the enum names anyway, so a future PuTTY renumbering is a
	 *     compile-time question rather than a silent wrong proxy.
	 *   - "SSH to proxy and use port forwarding" is not a proxy to WinSCP at
	 *     all, it is a tunnel: Tunnel=on plus the TunnelXxx settings.  Sending
	 *     it as a ProxyMethod made WinSCP open a SOCKS handshake against an SSH
	 *     daemon, which answers with its "SSH-2.0-..." banner - the reported
	 *     "SOCKS proxy response contained reply version numbers 83" ('S' = 83).
	 *
	 * PROXY_SSH_EXEC and PROXY_SSH_SUBSYSTEM have no WinSCP equivalent; their
	 * host/user/password mean the same SSH proxy host, so they get the tunnel
	 * too - the transport differs, the destination does not.  Anything else
	 * (PROXY_FUZZ) gets nothing rather than a wrong guess.
	 *
	 * Values are quoted: rawsettings are whitespace-separated, so a password or
	 * a hostname with a space would otherwise be read as the next setting.
	 *
	 * The fields come from the CONNECTION, not from the session: a named proxy
	 * or workplace proxy mode amends a throwaway Conf copy and deliberately
	 * leaves the stored session alone, so reading the session here handed WinSCP
	 * whatever the session says - usually no proxy at all, and then WinSCP tries
	 * to reach a host only the proxy can see. kitty_proxy_connection() is that
	 * copy's answer, kept by start_backend(); NULL means we never connected, and
	 * then the session's own fields are the best guess available.
	 */
	{
	const struct kitty_proxy_snapshot *px = kitty_proxy_connection() ;
	int          px_type = px ? px->type           : conf_get_int(conf,CONF_proxy_type) ;
	int          px_port = px ? px->port           : conf_get_int(conf,CONF_proxy_port) ;
	const char * px_host = px ? px->host           : conf_get_str(conf,CONF_proxy_host) ;
	const char * px_user = px ? px->username       : conf_get_str(conf,CONF_proxy_username) ;
	const char * px_pass = px ? px->password       : conf_get_str(conf,CONF_proxy_password) ;
	const char * px_tcmd = px ? px->telnet_command : conf_get_str(conf,CONF_proxy_telnet_command) ;

	if( (px_type != PROXY_NONE) && (strlen( conf_get_str(conf, CONF_sftpconnect) )==0) ) {
		int ptype = px_type ;
		int tunnel = (ptype==PROXY_SSH_TCPIP || ptype==PROXY_SSH_EXEC || ptype==PROXY_SSH_SUBSYSTEM) ;
		int method = -1 ;

		switch( ptype ) {
			case PROXY_SOCKS4: method = 1 ; break ;
			case PROXY_SOCKS5: method = 2 ; break ;
			case PROXY_HTTP:   method = 3 ; break ;
			case PROXY_TELNET: method = 4 ; break ;
			case PROXY_CMD:    method = 5 ; break ;
			default: break ;
		}

		if( tunnel ) {
			if( raw == 0 ) { bcat( cmd, sizeof(cmd), " /rawsettings" ) ; raw++ ; }
			bcat( cmd, sizeof(cmd), " Tunnel=1" ) ;
			if( px_host && strlen(px_host)>0 ) { bcat( cmd, sizeof(cmd), " TunnelHostName=" ) ; rawcat( cmd, sizeof(cmd), px_host ) ; }
			snprintf( buffer, sizeof(buffer), " TunnelPortNumber=%d", px_port ) ; bcat( cmd, sizeof(cmd), buffer ) ;
			if( px_user && strlen(px_user)>0 ) { bcat( cmd, sizeof(cmd), " TunnelUserName=" ) ; rawcat( cmd, sizeof(cmd), px_user ) ; }
			if( px_pass && strlen(px_pass)>0 ) {
				/* plaintext at runtime; TunnelPassword expects WinSCP's own
				 * encrypted form, TunnelPasswordPlain is the cleartext one.
				 * The span covers the quotes rawcat adds, so the Event Log
				 * blanks the whole value however it was escaped. */
				bcat( cmd, sizeof(cmd), " TunnelPasswordPlain=" ) ;
				proxy_pw_at = strlen( cmd ) ;
				rawcat( cmd, sizeof(cmd), px_pass ) ;
				proxy_pw_len = strlen( cmd ) - proxy_pw_at ;
			}
		} else if( method > 0 ) {
			if( raw == 0 ) { bcat( cmd, sizeof(cmd), " /rawsettings" ) ; raw++ ; }
			snprintf( buffer, sizeof(buffer), " ProxyMethod=%d", method ) ; bcat( cmd, sizeof(cmd), buffer ) ;
			if( px_host && strlen(px_host)>0 ) { bcat( cmd, sizeof(cmd), " ProxyHost=" ) ; rawcat( cmd, sizeof(cmd), px_host ) ; }
			snprintf( buffer, sizeof(buffer), " ProxyPort=%d", px_port ) ; bcat( cmd, sizeof(cmd), buffer ) ;
			if( px_user && strlen(px_user)>0 ) { bcat( cmd, sizeof(cmd), " ProxyUsername=" ) ; rawcat( cmd, sizeof(cmd), px_user ) ; }
			if( px_pass && strlen(px_pass)>0 ) {
				bcat( cmd, sizeof(cmd), " ProxyPassword=" ) ;   /* plaintext at runtime */
				proxy_pw_at = strlen( cmd ) ;
				rawcat( cmd, sizeof(cmd), px_pass ) ;
				proxy_pw_len = strlen( cmd ) - proxy_pw_at ;
			}
			/* One PuTTY field, two WinSCP ones: CONF_proxy_telnet_command holds
			 * the Telnet negotiation string for PROXY_TELNET and the local
			 * command line for PROXY_CMD, and WinSCP keeps those apart. Sending
			 * a local command as ProxyTelnetCommand left WinSCP with nothing to
			 * run for method 5. */
			if( px_tcmd && strlen(px_tcmd)>0 ) {
				bcat( cmd, sizeof(cmd), method==5 ? " ProxyLocalCommand=" : " ProxyTelnetCommand=" ) ;
				rawcat( cmd, sizeof(cmd), px_tcmd ) ;
			}
		}
	}
	}

	if( conf_get_bool(conf,CONF_compression) ) {
		if( raw == 0 ) { bcat( cmd, sizeof(cmd), " /rawsettings" ) ; raw++ ; }
		bcat( cmd, sizeof(cmd), " Compression=1" ) ;
	}

	if( conf_get_bool(conf, CONF_agentfwd) ) {
		if( raw == 0 ) { bcat( cmd, sizeof(cmd), " /rawsettings" ) ; raw++ ; }
		bcat( cmd, sizeof(cmd), " AgentFwd=1" ) ;
	}

	if( strlen(conf_get_str(conf, CONF_winscprawsettings))>0 ) {
		if( raw == 0 ) { bcat( cmd, sizeof(cmd), " /rawsettings" ) ; raw++ ; }
		bcat( cmd, sizeof(cmd), " " ) ; bcat( cmd, sizeof(cmd), conf_get_str(conf, CONF_winscprawsettings) ) ;
	}

	if( !strcmp(proto,"scp") && (strlen(conf_get_str(conf, CONF_pscpshell))>0) ) {
		if( raw == 0 ) { bcat( cmd, sizeof(cmd), " /rawsettings" ) ; raw++ ; }
		bcat( cmd, sizeof(cmd), " Shell=" ) ; rawcat( cmd, sizeof(cmd), conf_get_str(conf, CONF_pscpshell) ) ;
	}
	
	/* WinSCP is a separate program with a window of its own, so there is
	 * nowhere of ours to write a note into - ask before launching it. */
	{ char *note = kx_hello_agent_note( conf ) ;
	  if( note != NULL ) {
		char *text = dupprintf( "%sStart WinSCP anyway?", note ) ;
		int go = MessageBox( hwnd, text, "KiTTY - the key is not in the agent",
		                     MB_OKCANCEL|MB_ICONWARNING ) ;
		sfree( text ) ; sfree( note ) ;
		if( go != IDOK ) { memset( cmd, 0, strlen(cmd) ) ; return ; }
	  }
	}
	debug_logevent_redacted2( "Start WinSCP", cmd, pw_at, pw_len, proxy_pw_at, proxy_pw_len ) ;
	RunCommand( hwnd, cmd ) ;
	memset(cmd,0,strlen(cmd));
}

	
// Recherche le chemin vers le programme PSCP
int SearchPSCP( void ) {
	char buffer[4096], ki[10]="kscp.exe", pu[10]="pscp.exe" ;

	if( PSCPPath!=NULL ) { free(PSCPPath) ; PSCPPath = NULL ; }
	// Dans la base de registre
	if( ReadParameterN( INIT_SECTION, "PSCPPath", buffer, sizeof(buffer) ) != 0 ) {
		if( adopt_tool_path_if_exists( &PSCPPath, buffer, NULL, NULL ) ) return 1 ;
		else { DelParameter( INIT_SECTION, "PSCPPath" ) ; }
	}

	// Dans le fichier ini
	if( ReadParameterN( INIT_SECTION, "pscpdir", buffer, sizeof(buffer) ) ) {
		buffer[4076]='\0';
		strcat( buffer, "\\" ) ; strcat( buffer, ki ) ;
		if( adopt_tool_path_if_exists( &PSCPPath, buffer, "PSCPPath", NULL ) ) return 1 ;
		else {
			ReadParameterN( INIT_SECTION, "pscpdir", buffer, sizeof(buffer) ) ;
			buffer[4076]='\0';
			strcat( buffer, "\\" ) ; strcat( buffer, pu ) ;
			if( adopt_tool_path_if_exists( &PSCPPath, buffer, "PSCPPath", NULL ) ) return 1 ;
		}
	}
	// kscp dans le meme repertoire
	snprintf( buffer, sizeof(buffer), "%s\\%s", InitialDirectory, ki ) ;
	if( adopt_tool_path_if_exists( &PSCPPath, buffer, "PSCPPath", NULL ) ) return 1 ;
	// pscp dans le repertoire normal de PuTTY
	snprintf( buffer, sizeof(buffer), "%s\\PuTTY\\%s", getenv("ProgramFiles"), pu ) ;
	if( adopt_tool_path_if_exists( &PSCPPath, buffer, "PSCPPath", NULL ) ) return 1 ;

	// pscp dans le meme repertoire
	snprintf( buffer, sizeof(buffer), "%s\\%s", InitialDirectory, pu ) ;
	if( adopt_tool_path_if_exists( &PSCPPath, buffer, "PSCPPath", NULL ) ) return 1 ;

	return 0 ;
}

// Gestion du drap and drop
void recupNomFichierDragDrop(HWND hwnd, HDROP* leDrop ) {
        HDROP hDropInfo = *leDrop ;
        int nb,taille,i;
        taille=0;
        nb=0;
	if( leDrop==NULL ) return ;
        nb=DragQueryFile( hDropInfo, 0xFFFFFFFF, NULL, 0 ) ;
        char *fic ;
	if( nb>0 ) for( i = 0; i < nb; i++ ) {
                taille = DragQueryFile(hDropInfo, i, NULL, 0 ) ;   /* length, excluding NUL */
		fic = (char*)malloc(taille+2) ;
                { UINT _g = DragQueryFile( hDropInfo, i, fic, taille+1 ) ; fic[_g] = '\0' ; }  /* force-terminate: DragQueryFile doesn't always NUL-terminate -> a stray byte was reaching pscp ("...pdf\0") */
		if( !strcmp( fic+strlen(fic)-10,"\\kitty.ini" ) ) { // On charge le fichier de config dans l'editeur interne
			char buffer[1024]="", shortname[1024]="" ;
			if( GetModuleFileName( NULL, (LPTSTR)buffer, 1023 ) ) 
				if( GetShortPathName( buffer, shortname, 1023 ) ) {
					snprintf( buffer, sizeof(buffer), "\"%s\" -ed %s", shortname, fic ) ;
					RunCommand( hwnd, buffer ) ;
				}
		} else {
			/* NULL target dir either way: the RemotePath store the auto-pwd
			 * branch used to pass was never written on the 0.84 core (the
			 * __pw title-scan was not forward-ported), so it was always NULL. */
			SendOneFile( hwnd, "", fic, NULL ) ;
		}
		free(fic);
	}
	DragFinish(hDropInfo) ;  //vidage de la mem...
        *leDrop = hDropInfo ;  //TOCHECK : transmistion de param...
}

void OnDropFiles(HWND hwnd, HDROP hDropInfo) {
	if( conf_get_int(conf,CONF_protocol) != PROT_SSH ) {
		MessageBox( hwnd, "This function is only available with SSH connections.", "Error", MB_OK|MB_ICONERROR ) ;
		return ;
	}
	/* Drag-drop always uses the normal path now. The former "Send file in
	 * current directory" (CONF_scp_auto_pwd) option injected a
	 * `printf "...__pw:$(pwd)..."` probe into the shell and scraped the reply via
	 * the __pw OSC-title dispatcher - removed in 0.84 as the CVE-2024-23749 RCE -
	 * so it no longer captured anything. Its safe, opt-in replacement is OSC 7
	 * cwd tracking, which SendOneFile already consults via kitty_current_dir(). */
	recupNomFichierDragDrop(hwnd, &hDropInfo) ;
}
