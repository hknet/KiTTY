/*
 * kitty_updater.c - the in-app updater: the GitHub release query, the cached
 * "newer build available" state and its session-start notice, the asset
 * download, the Authenticode gate and the MSI run, the modeless update popup
 * and the transient title notice.
 */
#include "kitty_updater.h"
#include "kitty_win.h"
#include "kitty_authenticode.h"   /* shared Authenticode trust + CN gate */
#include "kitty_notice.h"          /* near-the-clock warning window */
#include "kitty_rc_additions.h"   /* IDD_UPDATEBOX, IDC_UPD_TEXT, IDC_UPD_UPDATE */
#include "kitty_theme.h"           /* the app-wide colour theme */
#include "../windows/putty-rc.h"   /* -demo-templates: the shared dialog ids */
#include "kitty_oldwin.h"   /* APIs newer than the oldest Windows we load on */
#include "kitty_oldwin_reg.h"   /* XP: RegDeleteTree/RegGetValue via oldwin - a STATIC RegGetValueA import kills the loader there */
#include "kitty_text.h"     /* shared captions */
#include "kitty_inikeys.h"  /* KI_*: the kitty.ini key names */
#include "kitty_buildlabel.h"   /* the test build's label, if this is one */
#include <wininet.h>   /* CheckVersionFromWebSite: GitHub releases query */
#include <wintrust.h>  /* in-app updater: Authenticode trust verification */
#include <softpub.h>   /* WINTRUST_ACTION_GENERIC_VERIFY_V2 */
#include <msi.h>       /* in-app updater: install-type detection by UpgradeCode */
#include "kitty_gui.h"
#include "kitty.h"
#include "kitty_storage.h"
#include "kitty_auxpos.h"
#include "kitty_dlgbox.h"   /* the themed boxes, CenterDlgInParent */

// Check whether an update is available (GitHub repository hknet/KiTTY)

/* Parse a dotted version "0.84.0.15" into 4 comparable integers. */
static void kitty_parse_version( const char *s, int v[4] ) {
	v[0]=v[1]=v[2]=v[3]=0 ;
	sscanf( s, "%d.%d.%d.%d", &v[0], &v[1], &v[2], &v[3] ) ;
}
/* Return <0 if a<b, 0 if equal, >0 if a>b. */
static int kitty_version_cmp( const int a[4], const int b[4] ) {
	int i ;
	for( i=0 ; i<4 ; i++ ) { if( a[i]!=b[i] ) return (a[i]<b[i]) ? -1 : 1 ; }
	return 0 ;
}

/* The page a user lands on to download a new build, and the JSON API we query.
 * We use the /releases list (newest first) rather than /releases/latest, because
 * /releases/latest skips pre-releases and every KiTTY build is a -beta prerelease,
 * so /latest would 404. The first "tag_name" in the array is the newest release. */
#define KITTY_RELEASES_URL "https://github.com/hknet/KiTTY/releases"
#define KITTY_RELEASES_API "https://api.github.com/repos/hknet/KiTTY/releases?per_page=1"

/* ===================== In-app updater =====================
 * When "Check for updates" finds a newer release, we can download the correct
 * installer asset and run it - but ONLY after verifying it is a genuine,
 * KAPPER-signed artifact. The Authenticode gate (kitty_verify_signature) is the
 * security control here: it is fail-closed (any error rejects), it requires both
 * a valid trust chain (WinVerifyTrust) AND an exact publisher-CN match, and the
 * downloaded file is deleted if it does not pass. */

typedef enum { KITTY_INST_PERUSER, KITTY_INST_SYSTEM, KITTY_INST_PORTABLE } kitty_install_t ;

/* Our MSI UpgradeCodes, stable across versions - they must match the ones in
 * windows/installer/*.wxs, which is where they are defined.
 * MsiEnumRelatedProducts takes the braced GUID form. */
#define KITTY_UPGRADE_SYSTEM  "{69EA2DD5-EF19-4811-B324-EF34CAA6942C}"
#define KITTY_UPGRADE_PERUSER "{578952A6-AA7F-4146-918B-47803234700B}"

static int kitty_msi_installed( const char *upgradecode ) {
	char prodbuf[40] = "" ;   /* a ProductCode GUID is 38 chars + NUL */
	return MsiEnumRelatedProductsA( upgradecode, 0, 0, prodbuf ) == ERROR_SUCCESS ;
}

/* How was this copy installed? Decides which asset to fetch and how to run it.
 * Primary, robust signal: ask Windows Installer whether OUR product (by its
 * stable UpgradeCode) is installed, and which kind - this is independent of the
 * install path, locale, or whether the exe was copied elsewhere. The path sniff
 * is only a fallback. The portable build (MOD_PORTABLE) is always download-only. */
static kitty_install_t kitty_detect_install_type( void ) {
#ifdef MOD_PORTABLE
	return KITTY_INST_PORTABLE ;
#else
	if( kitty_msi_installed( KITTY_UPGRADE_SYSTEM ) )  return KITTY_INST_SYSTEM ;
	if( kitty_msi_installed( KITTY_UPGRADE_PERUSER ) ) return KITTY_INST_PERUSER ;

	/* Fallback: path sniff (older installs / unusual setups). */
	char exe[MAX_PATH]="", env[MAX_PATH]="" ;
	if( GetModuleFileNameA( NULL, exe, sizeof(exe)-1 ) ) {
		if( GetEnvironmentVariableA("ProgramFiles", env, sizeof(env)-1) && env[0]
		    && _strnicmp(exe, env, strlen(env))==0 ) return KITTY_INST_SYSTEM ;
		if( GetEnvironmentVariableA("ProgramW6432", env, sizeof(env)-1) && env[0]
		    && _strnicmp(exe, env, strlen(env))==0 ) return KITTY_INST_SYSTEM ;
		if( GetEnvironmentVariableA("ProgramFiles(x86)", env, sizeof(env)-1) && env[0]
		    && _strnicmp(exe, env, strlen(env))==0 ) return KITTY_INST_SYSTEM ;
		if( GetEnvironmentVariableA("LOCALAPPDATA", env, sizeof(env)-1) && env[0]
		    && _strnicmp(exe, env, strlen(env))==0 ) return KITTY_INST_PERUSER ;
	}
	/* Unknown location (loose exe): treat as portable -> download-only, no auto-run. */
	return KITTY_INST_PORTABLE ;
#endif
}

/* Scan the release JSON for a "browser_download_url" whose value ends with the
 * given suffix (e.g. "-x64-system.msi"). Robust to the exact version string.
 * Copies the URL into out and returns 1; returns 0 if no asset matches. */
static int kitty_find_asset_url( const char *body, const char *suffix, char *out, size_t outsz ) {
	const char *p = body ;
	size_t slen = strlen(suffix) ;
	while( (p = strstr(p, "\"browser_download_url\"")) != NULL ) {
		p += strlen("\"browser_download_url\"") ;
		const char *q = strchr(p, ':') ;
		if( q==NULL ) break ;
		q++ ;
		while( *q==' ' || *q=='\"' ) q++ ;
		char url[1024] ; int j=0 ;
		while( *q && *q!='\"' && j<(int)sizeof(url)-1 ) url[j++]=*q++ ;
		url[j]='\0' ;
		p = q ;
		if( (size_t)j>=slen && _stricmp(url + j - slen, suffix)==0 ) {
			strncpy(out, url, outsz-1) ; out[outsz-1]='\0' ;
			return 1 ;
		}
	}
	return 0 ;
}

/* Download a URL to a local file. Uses generous timeouts (multi-MB installer,
 * not the JSON probe) and follows GitHub's redirect to the CDN. Returns 1 on
 * success; on any failure the partial file is removed. */
static int kitty_download_to_file( const char *url, const char *path ) {
	int ok = 0 ;
	HINTERNET hi = InternetOpenA( "KiTTY-UpdateDownload", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0 ) ;
	if( hi==NULL ) return 0 ;
	DWORD tmo = 30000 ;
	InternetSetOption( hi, INTERNET_OPTION_CONNECT_TIMEOUT, &tmo, sizeof(tmo) ) ;
	InternetSetOption( hi, INTERNET_OPTION_RECEIVE_TIMEOUT, &tmo, sizeof(tmo) ) ;
	HINTERNET hu = InternetOpenUrlA( hi, url, NULL, (DWORD)-1,
		INTERNET_FLAG_RELOAD|INTERNET_FLAG_NO_CACHE_WRITE|INTERNET_FLAG_SECURE, 0 ) ;
	if( hu!=NULL ) {
		/* CREATE_NEW avoids clobbering or following an attacker-precreated file in
		 * %TEMP%. The caller supplies a fresh random-ish path and failure deletes
		 * partial output. */
		HANDLE hf = CreateFileA( path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL ) ;
		if( hf!=INVALID_HANDLE_VALUE ) {
			char buf[16384] ; DWORD nread=0 ; ok=1 ;
			for( ;; ) {
				if( !InternetReadFile( hu, buf, sizeof(buf), &nread ) ) { ok=0 ; break ; }
				if( nread==0 ) break ;
				DWORD nwr=0 ;
				if( !WriteFile( hf, buf, nread, &nwr, NULL ) || nwr!=nread ) { ok=0 ; break ; }
			}
			CloseHandle( hf ) ;
		}
		InternetCloseHandle( hu ) ;
	}
	InternetCloseHandle( hi ) ;
	if( !ok ) DeleteFileA( path ) ;
	return ok ;
}

/* SECURITY GATE for the downloaded installer: a valid Authenticode trust
 * chain AND an exact publisher-CN match. The implementation now lives in
 * kitty_authenticode.c so the kageant "New key" launcher shares the exact
 * same gate - see kitty_authenticode.h. The installer is a NEWER version than
 * the running kitty, so only trust+CN is checked here, never a version match. */

/* Launch the (already verified) MSI. System installs need elevation (runas);
 * per-user installs run unelevated. Restart Manager inside msiexec will close
 * the running kitty.exe to perform the in-place upgrade. */
static int kitty_run_installer( HWND hwnd, kitty_install_t type, const char *path ) {
	char args[MAX_PATH+32] ;
	snprintf( args, sizeof(args), "/i \"%s\"", path ) ;
	HINSTANCE r = ShellExecuteA( hwnd, (type==KITTY_INST_SYSTEM) ? "runas" : "open",
		"msiexec.exe", args, NULL, SW_SHOWNORMAL ) ;
	return ((INT_PTR)r > 32) ;
}

/* ---- KiTTY: background "update available" check (cached; shown at session start) ----
 * The blocking GitHub query runs on a worker thread and ONLY refreshes a cached
 * "latest version" in the registry. The in-terminal notice is rendered later,
 * synchronously, at the clean top of a session (window.c) -- never injected
 * mid-session, which would corrupt a full-screen TUI. So the notice can be at
 * most one launch behind for a brand-new release, which is fine for a nudge. */

/* Fetch the newest release's numeric version + prerelease flag from GitHub.
 * Returns 1 on success. Leaner sibling of CheckVersionFromWebSite's fetch
 * (no asset URLs needed). */
static int kitty_fetch_latest_version( char *ver, int verlen, int *is_beta ) {
	char *body = NULL ; DWORD bodylen = 0 ; int ok = 0 ;
#ifdef KITTY_TEST_BUILD_LABEL
	/* A TEST BUILD answers from a file beside the program when there is one
	 * (update_test_latest.txt, one line: the version), so a harness can prove
	 * the re-check of a long-running launcher without a release to find and
	 * without a network. A release build has no such door. */
	{
		char path[MAX_PATH+40], *slash ; FILE *fp ;
		if( GetModuleFileNameA( NULL, path, MAX_PATH ) && ( slash = strrchr( path, '\\' ) ) != NULL ) {
			strcpy( slash+1, "update_test_latest.txt" ) ;
			if( ( fp = fopen( path, "r" ) ) != NULL ) {
				char line[64] = "" ; int k = 0 ; const char *d ;
				if( fgets( line, sizeof(line), fp ) == NULL ) line[0] = '\0' ;
				fclose( fp ) ;
				for( d = line ; *d && ( ( *d>='0' && *d<='9' ) || *d=='.' ) && k < verlen-1 ; d++ ) ver[k++] = *d ;
				ver[k] = '\0' ;
				if( is_beta != NULL ) *is_beta = 0 ;
				return ( ver[0] != '\0' ) ;
			}
		}
	}
#endif
	HINTERNET hi = InternetOpenA( "KiTTY-UpdateCheck",
	                              INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0 ) ;
	if( hi == NULL ) return 0 ;
	DWORD tmo = 8000 ;
	InternetSetOption( hi, INTERNET_OPTION_CONNECT_TIMEOUT, &tmo, sizeof(tmo) ) ;
	InternetSetOption( hi, INTERNET_OPTION_SEND_TIMEOUT,    &tmo, sizeof(tmo) ) ;
	InternetSetOption( hi, INTERNET_OPTION_RECEIVE_TIMEOUT, &tmo, sizeof(tmo) ) ;
	HINTERNET hu = InternetOpenUrlA( hi, KITTY_RELEASES_API,
	                                 "Accept: application/vnd.github+json\r\n", (DWORD)-1,
	                                 INTERNET_FLAG_RELOAD|INTERNET_FLAG_NO_CACHE_WRITE|INTERNET_FLAG_SECURE, 0 ) ;
	if( hu != NULL ) {
		DWORD cap = 65536 ; body = (char*)malloc( cap ) ; bodylen = 0 ;
		if( body != NULL ) {
			DWORD nread = 0 ;
			for( ;; ) {
				if( cap - bodylen < 4096 ) { char *nb=(char*)realloc(body,cap*2); if(nb==NULL) break; body=nb; cap*=2; }
				if( !InternetReadFile( hu, body+bodylen, cap-bodylen-1, &nread ) || nread==0 ) break ;
				bodylen += nread ;
				}
			body[bodylen] = '\0' ; ok = (bodylen>0) ;
			}
		InternetCloseHandle( hu ) ;
		}
	InternetCloseHandle( hi ) ;

	int got = 0 ;
	if( ok && body!=NULL ) {
		if( is_beta != NULL ) {
			char *pr = strstr( body, "\"prerelease\"" ) ; *is_beta = 0 ;
			if( pr!=NULL ) { pr=strchr(pr,':'); if(pr!=NULL) pr++; while(pr!=NULL&&(*pr==' '||*pr=='\t'))pr++;
				*is_beta = ( pr!=NULL && strncmp(pr,"true",4)==0 ) ; }
			}
		char *p = strstr( body, "\"tag_name\"" ) ;
		if( p!=NULL ) {
			p=strchr(p,':'); if(p!=NULL)p++; while(p!=NULL&&(*p==' '||*p=='\"'))p++;
			char tag[128]=""; int j=0; while(p!=NULL&&*p&&(*p!='\"')&&(j<(int)sizeof(tag)-1)){tag[j++]=*p++;} tag[j]='\0';
			char *d=tag; while(*d&&!((*d>='0')&&(*d<='9')))d++;
			int k=0; while(*d&&(((*d>='0')&&(*d<='9'))||(*d=='.'))&&(k<verlen-1)){ver[k++]=*d++;} ver[k]='\0';
			got = (ver[0]!='\0') ;
			}
		}
	if( body!=NULL ) free( body ) ;
	return got ;
}

struct kitty_update_notify {
	HWND hwnd ;
	UINT msg ;
} ;
/* One notifying check at a time: set by kitty_start_update_check_notify,
 * cleared by the worker when it has posted its answer. */
static volatile LONG kitty_update_notify_running = 0 ;

static DWORD WINAPI kitty_update_worker( LPVOID param ) {
	struct kitty_update_notify *notify = (struct kitty_update_notify *)param ;
	char ver[64]="" ; int is_beta=0 ;
	if( kitty_fetch_latest_version( ver, sizeof(ver), &is_beta ) ) {
		DWORD b = is_beta ? 1 : 0 ;
		if( !kitty_portable_store_state_string( "UpdateLatest", ver ) ||
		    !kitty_portable_store_state_dword( "UpdateLatestBeta", b ) ) {
			HKEY hk ; char base[512] ;
			snprintf( base, sizeof(base), "%s", kitty_registry_base() ) ;
			if( RegCreateKeyExA( HKEY_CURRENT_USER, base, 0, NULL, 0, KEY_SET_VALUE, NULL, &hk, NULL ) == ERROR_SUCCESS ) {
				RegSetValueExA( hk, "UpdateLatest", 0, REG_SZ, (const BYTE*)ver, (DWORD)strlen(ver)+1 ) ;
				RegSetValueExA( hk, "UpdateLatestBeta", 0, REG_DWORD, (const BYTE*)&b, sizeof(b) ) ;
				RegCloseKey( hk ) ;
				}
			}
		}
	if( notify != NULL ) {
		if( notify->hwnd != NULL && notify->msg != 0 )
			PostMessage( notify->hwnd, notify->msg, 0, 0 ) ;
		free( notify ) ;
		InterlockedExchange( &kitty_update_notify_running, 0 ) ;
		}
	return 0 ;
}

/* Launch the background update check once per process (fire-and-forget). */
void kitty_start_update_check( void ) {
	static int started = 0 ;
	if( started ) return ; started = 1 ;
	HANDLE th = CreateThread( NULL, 0, kitty_update_worker, NULL, 0, NULL ) ;
	if( th != NULL ) CloseHandle( th ) ;
}

/* Launcher variant: notify a window after the async cache refresh, so the tray
 * balloon can appear on the first launcher run after a new release instead of
 * only after a previous process has already populated the cache.
 * Callable again: a launcher sits in the tray for days and asks once a day.
 * One check at a time - a call made while one is running does nothing, and a
 * check that fails is simply not heard from. */
void kitty_start_update_check_notify( HWND hwnd, UINT msg ) {
	struct kitty_update_notify *notify ;
	if( InterlockedCompareExchange( &kitty_update_notify_running, 1, 0 ) != 0 ) return ;
	notify = (struct kitty_update_notify *)malloc( sizeof(*notify) ) ;
	if( notify == NULL ) { InterlockedExchange( &kitty_update_notify_running, 0 ) ; return ; }
	notify->hwnd = hwnd ;
	notify->msg = msg ;
	HANDLE th = CreateThread( NULL, 0, kitty_update_worker, notify, 0, NULL ) ;
	if( th != NULL ) CloseHandle( th ) ;
	else { free( notify ) ; InterlockedExchange( &kitty_update_notify_running, 0 ) ; }
}

/* If the cached latest version is newer than this build and the channel rule
 * allows surfacing it, fill buf with a one-line ASCII notice and return 1. */
/* Shared "is a newer build available?" check, used by the terminal notice and
 * the launcher tray balloon. Reads the cached latest version (refreshed async by
 * the worker), applies the channel rule (a stable build ignores betas), and on a
 * positive result fills the caller's buffers. Returns 1 if an update should be
 * surfaced, else 0. Any out pointer may be NULL. */
int kitty_update_available( char *latest_out, int latest_n,
                            char *cur_out, int cur_n, int *beta_out ) {
	char curnum[64]="" ; int i ;
	strncpy( curnum, BuildVersionTime, sizeof(curnum)-1 ) ; curnum[sizeof(curnum)-1]='\0' ;
	for( i=0 ; i<(int)strlen(curnum) ; i++ )
		if( !(((curnum[i]>='0')&&(curnum[i]<='9'))||(curnum[i]=='.')) ) { curnum[i]='\0'; break; }
	/* This build's channel: BUILD_VERSION carries no "-beta" suffix, so detect
	 * from the version scheme (KiTTY stable = x.y.M.0, beta = x.y.M.P, P>0); also
	 * honour an explicit "beta" in the build string if one is ever added. */
	int cur_is_beta ;
	{ int cvb[4] ; kitty_parse_version( curnum, cvb ) ;
	  cur_is_beta = ( cvb[3] != 0 ) || ( strstr(BuildVersionTime,"beta")!=NULL )
	                                || ( strstr(BuildVersionTime,"BETA")!=NULL ) ; }

	char base[512], latest[64]="" ; DWORD sz=sizeof(latest), beta=0, bsz=sizeof(beta) ;
	if( !kitty_portable_load_state_string( "UpdateLatest", latest, sizeof(latest) ) ) {
		snprintf( base, sizeof(base), "%s", kitty_registry_base() ) ;
		if( RegGetValueA( HKEY_CURRENT_USER, base, "UpdateLatest", RRF_RT_REG_SZ, NULL, latest, &sz ) != ERROR_SUCCESS ) return 0 ;
		RegGetValueA( HKEY_CURRENT_USER, base, "UpdateLatestBeta", RRF_RT_REG_DWORD, NULL, &beta, &bsz ) ;
	} else {
		kitty_portable_load_state_dword( "UpdateLatestBeta", &beta ) ;
	}
	if( latest[0]=='\0' ) return 0 ;

	int cv[4], lv[4] ;
	kitty_parse_version( curnum, cv ) ;
	kitty_parse_version( latest, lv ) ;
	if( kitty_version_cmp( cv, lv ) >= 0 ) return 0 ;   /* not newer */
	if( !cur_is_beta && beta ) return 0 ;               /* stable build ignores betas */

	if( latest_out && latest_n>0 ) { strncpy( latest_out, latest, latest_n-1 ) ; latest_out[latest_n-1]='\0' ; }
	if( cur_out && cur_n>0 ) { strncpy( cur_out, curnum, cur_n-1 ) ; cur_out[cur_n-1]='\0' ; }
	if( beta_out ) *beta_out = (int)beta ;
	return 1 ;
}

int kitty_update_notice( char *buf, int n ) {
	char latest[64]="", curnum[64]="" ; int beta=0 ;
	if( !kitty_update_available( latest, sizeof(latest), curnum, sizeof(curnum), &beta ) ) return 0 ;
	/* UTF-8 source text (incl. a real "->" arrow); window.c renders it via
	 * term_data_wide(), which encodes to the terminal's charset (no mojibake). */
	snprintf( buf, n,
		KT_UPD_TERM_NOTICE,
		latest, curnum, beta ? KT_UPD_BETA_SUFFIX : "" ) ;
	return 1 ;
}

/* ============================================================================
 * KiTTY: non-modal, self-dismissing "update" popup + a transient title notice.
 * Replaces the modal, sound-playing MessageBox result boxes for the
 * update-available / no-update cases. Error boxes (download/signature failures)
 * stay modal WITH their sound on purpose - a failed install must not slip by.
 * ==========================================================================*/
#define KUP_ACT_NONE       0
#define KUP_ACT_OPENPAGE   1
#define KUP_ACT_MSI        2
/* Button IDs come from the IDD_UPDATEBOX template: IDC_UPD_UPDATE ("Update
 * now"), IDCANCEL ("Later"), IDOK ("OK"). */
#define KUP_TIMER_ID       1
#define KUP_AUTODISMISS_MS 5000

typedef struct {
	HWND  owner ;
	int   action ;
	kitty_install_t itype ;
	char  asseturl[1024] ;
	char  notesurl[512] ; /* KiTTY: that release's GitHub page; "" => no button */
	char  text[2048] ;    /* message; the dialog is grown to fit it */
	int   has_update ;    /* update available (Update now/Later) vs info (OK) */
} kitty_upd_ctx ;

/* Download+verify+install the MSI (unchanged flow, just factored out so the
 * non-modal popup's "Update" button can call it). Keeps its modal MB_ICONERROR
 * boxes (with the system sound) so failures are impossible to miss. */
static void kitty_do_msi_update( HWND owner, const char *asseturl, kitty_install_t itype ) {
	char tmpdir[MAX_PATH]="", tmpbase[MAX_PATH]="", tmpfile[MAX_PATH]="" ;
	if( !GetTempPathA( sizeof(tmpdir), tmpdir ) ||
	    !GetTempFileNameA( tmpdir, "kty", 0, tmpbase ) ) {
		kitty_message_box( owner, KT_UPD_TMP_FAILED,
			KT_CAP_UPDATE, MB_OK|MB_ICONERROR ) ;
		return ;
	}
	DeleteFileA( tmpbase ) ;
	snprintf( tmpfile, sizeof(tmpfile), "%s.msi", tmpbase ) ;

	HCURSOR oldc = SetCursor( LoadCursor(NULL, IDC_WAIT) ) ;
	int dok = kitty_download_to_file( asseturl, tmpfile ) ;
	SetCursor( oldc ) ;
	if( !dok ) {
		kitty_message_box( owner, KT_UPD_DOWNLOAD_FAILED,
			KT_CAP_UPDATE, MB_OK|MB_ICONERROR ) ;
		ShellExecute( owner, "open", KITTY_RELEASES_URL, 0, 0, SW_SHOWDEFAULT ) ;
		return ;
	}
	/* TOCTOU guard: hold the downloaded file open denying write/delete for the
	 * rest of the flow so it cannot be swapped between verify and launch. */
	HANDLE updguard = CreateFileA( tmpfile, GENERIC_READ, FILE_SHARE_READ,
		NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL ) ;
	if( updguard == INVALID_HANDLE_VALUE ) {
		DeleteFileA( tmpfile ) ;
		kitty_message_box( owner, KT_UPD_SECURE_FAILED,
			KT_CAP_UPDATE, MB_OK|MB_ICONERROR ) ;
		return ;
	}
	/* SECURITY GATE: reject anything not genuinely KAPPER-signed. */
	if( !kitty_authenticode_verify( tmpfile ) ) {
		CloseHandle( updguard ) ;
		DeleteFileA( tmpfile ) ;
		kitty_message_box( owner, KT_UPD_SIG_REJECTED,
			KT_CAP_UPDATE_SIG_REJECTED, MB_OK|MB_ICONERROR ) ;
		return ;
	}
	if( !kitty_run_installer( owner, itype, tmpfile ) ) {
		CloseHandle( updguard ) ;
		DeleteFileA( tmpfile ) ;
		kitty_message_box( owner, KT_UPD_INSTALLER_FAILED,
			KT_CAP_UPDATE, MB_OK|MB_ICONERROR ) ;
		return ;
	}
	/* keep the verified bytes locked while msiexec reads them (released on exit). */
}

/* Grow the message control + the whole dialog to fit `text` at the dialog's
 * (DPI-correct) font, and slide the buttons down by the same amount. Keeps the
 * "sized to the text" look now that the dialog manager owns the font. */
static void kitty_upd_fit_to_text( HWND h, const char *text ) {
	HWND txt = GetDlgItem( h, IDC_UPD_TEXT ) ;
	HFONT f = (HFONT)SendMessage( h, WM_GETFONT, 0, 0 ) ;
	if( !txt ) return ;
	RECT tr ; GetWindowRect( txt, &tr ) ; MapWindowPoints( NULL, h, (POINT*)&tr, 2 ) ;
	int tw = tr.right - tr.left, cur_th = tr.bottom - tr.top ;
	HDC dc = GetDC( txt ) ; HFONT of = (HFONT)SelectObject( dc, f ) ;
	RECT mr = { 0, 0, tw, 0 } ;
	DrawText( dc, text, -1, &mr, DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX ) ;
	int new_th = mr.bottom ;
	SelectObject( dc, of ) ; ReleaseDC( txt, dc ) ;
	int dh = new_th - cur_th ;
	if( dh == 0 ) return ;
	MoveWindow( txt, tr.left, tr.top, tw, new_th, TRUE ) ;
	int ids[] = { IDOK, IDCANCEL, IDC_UPD_UPDATE, IDC_UPD_NOTES } ;
	for( int i=0 ; i<(int)(sizeof(ids)/sizeof(ids[0])) ; i++ ) {
		HWND b = GetDlgItem( h, ids[i] ) ; if( !b ) continue ;
		RECT br ; GetWindowRect( b, &br ) ; MapWindowPoints( NULL, h, (POINT*)&br, 2 ) ;
		MoveWindow( b, br.left, br.top + dh, br.right-br.left, br.bottom-br.top, TRUE ) ;
	}
	RECT wr ; GetWindowRect( h, &wr ) ;
	SetWindowPos( h, NULL, 0, 0, wr.right-wr.left, (wr.bottom-wr.top)+dh, SWP_NOMOVE|SWP_NOZORDER ) ;
}

static INT_PTR CALLBACK kitty_upd_dlgproc( HWND h, UINT msg, WPARAM wp, LPARAM lp ) {
	kitty_upd_ctx *c = (kitty_upd_ctx*)GetWindowLongPtr( h, DWLP_USER ) ;
	switch( msg ) {
	  case WM_INITDIALOG:
		c = (kitty_upd_ctx*)lp ;
		SetWindowLongPtr( h, DWLP_USER, (LONG_PTR)c ) ;
		SetDlgItemTextA( h, IDC_UPD_TEXT, c ? c->text : "" ) ;
		if( c && c->has_update ) {
			ShowWindow( GetDlgItem( h, IDOK ), SW_HIDE ) ;   /* Update now + Later */
		} else {
			ShowWindow( GetDlgItem( h, IDC_UPD_UPDATE ), SW_HIDE ) ;   /* just OK */
			ShowWindow( GetDlgItem( h, IDCANCEL ), SW_HIDE ) ;
		}
		/* KiTTY: the "View release notes" button only when we have an update
		 * and a release URL to point at. */
		if( !( c && c->has_update && c->notesurl[0] ) )
			ShowWindow( GetDlgItem( h, IDC_UPD_NOTES ), SW_HIDE ) ;
		if( c ) kitty_upd_fit_to_text( h, c->text ) ;
		CenterDlgInParent( h ) ;
		SetWindowPos( h, HWND_TOP, 0,0,0,0, SWP_NOMOVE|SWP_NOSIZE|SWP_SHOWWINDOW ) ;
		SetForegroundWindow( h ) ;
		SetFocus( GetDlgItem( h, (c && c->has_update) ? IDC_UPD_UPDATE : IDOK ) ) ;
		/* Only the info box may dismiss itself; an available update stays until
		 * the user picks Update/Later so the offer can't silently vanish. */
		if( !(c && c->has_update) ) SetTimer( h, KUP_TIMER_ID, KUP_AUTODISMISS_MS, NULL ) ;
		return FALSE ;   /* focus set ourselves */
	  case WM_TIMER:
		if( wp==KUP_TIMER_ID ) DestroyWindow( h ) ;   /* auto-dismiss */
		return TRUE ;
	  case WM_COMMAND:
		if( LOWORD(wp)==IDC_UPD_UPDATE ) {
			KillTimer( h, KUP_TIMER_ID ) ;
			HWND owner = c ? c->owner : NULL ;
			int  action = c ? c->action : KUP_ACT_NONE ;
			kitty_install_t itype = c ? c->itype : KITTY_INST_PORTABLE ;
			char url[1024] ; url[0]='\0' ;
			if( c ) { strncpy(url, c->asseturl, sizeof(url)-1) ; url[sizeof(url)-1]='\0' ; }
			DestroyWindow( h ) ;   /* close the popup, then act */
			if( action==KUP_ACT_OPENPAGE ) ShellExecute( owner, "open", KITTY_RELEASES_URL, 0, 0, SW_SHOWDEFAULT ) ;
			else if( action==KUP_ACT_MSI ) kitty_do_msi_update( owner, url, itype ) ;
			return TRUE ;
		}
		if( LOWORD(wp)==IDC_UPD_NOTES ) {
			/* KiTTY: open the release page in the browser; leave the popup up
			 * so the user can still choose Update now / Later afterwards. */
			if( c && c->notesurl[0] )
				ShellExecute( c->owner, "open", c->notesurl, 0, 0, SW_SHOWDEFAULT ) ;
			return TRUE ;
		}
		if( LOWORD(wp)==IDOK || LOWORD(wp)==IDCANCEL ) { DestroyWindow( h ) ; return TRUE ; }
		return FALSE ;
	  case WM_CLOSE: DestroyWindow( h ) ; return TRUE ;
	  case WM_NCDESTROY:
		if( c ) { free(c) ; SetWindowLongPtr( h, DWLP_USER, 0 ) ; }
		return FALSE ;
	}
	return FALSE ;
}

/* Show the modeless "update available / up to date" popup over `owner`. It is a
 * real dialog (IDD_UPDATEBOX) so the dialog manager gives it the shell font at
 * the correct DPI, exactly like every other KiTTY window - no hand-rolled DPI
 * scaling. action==KUP_ACT_NONE => info only (single "OK", auto-dismisses in
 * 5s); otherwise "Update now" runs the action and "Later" dismisses, and it
 * stays up until the user decides so the offer can't silently vanish. */
static void kitty_show_update_popup( HWND owner, const char *text, int action,
                                     const char *asseturl, kitty_install_t itype,
                                     const char *notesurl ) {
	kitty_upd_ctx *c = (kitty_upd_ctx*)calloc( 1, sizeof(kitty_upd_ctx) ) ;
	if( !c ) return ;
	c->owner = owner ; c->action = action ; c->itype = itype ;
	c->has_update = ( action != KUP_ACT_NONE ) ;
	if( text ) { strncpy( c->text, text, sizeof(c->text)-1 ) ; }
	if( asseturl ) { strncpy( c->asseturl, asseturl, sizeof(c->asseturl)-1 ) ; }
	if( notesurl ) { strncpy( c->notesurl, notesurl, sizeof(c->notesurl)-1 ) ; }
	/* Modeless: don't block the session that's starting up. The dialog frees c
	 * on WM_NCDESTROY. */
	HWND h = CreateDialogParamA( GetModuleHandle(NULL), MAKEINTRESOURCEA(IDD_UPDATEBOX),
		owner, kitty_upd_dlgproc, (LPARAM)c ) ;
	if( !h ) free( c ) ;
}

/*
 * "kitty.exe -update": the updater as a run of its own, for a program that
 * has no updater and must not get one - the agent holds the private keys and
 * stays free of network code, so its "Update available" entry starts this.
 * The same dialog the launcher and the terminal's menu open, then a message
 * loop for as long as this thread shows a window (the popup is modeless, and
 * "Update now" downloads and verifies before it starts the installer); then
 * the caller exits.
 */
static BOOL CALLBACK kitty_upd_first_visible( HWND h, LPARAM lp ) {
	if( IsWindowVisible( h ) ) { *(HWND *)lp = h ; return FALSE ; }
	return TRUE ;
}

void kitty_update_run_standalone( void ) {
	CheckVersionFromWebSite( NULL, 0 ) ;
	for( ;; ) {
		HWND w = NULL ; MSG m ;
		EnumThreadWindows( GetCurrentThreadId(), kitty_upd_first_visible, (LPARAM)&w ) ;
		if( w == NULL ) break ;
		if( GetMessage( &m, NULL, 0, 0 ) <= 0 ) break ;
		if( !IsDialogMessage( w, &m ) ) { TranslateMessage( &m ) ; DispatchMessage( &m ) ; }
	}
}

/* Transient terminal-title notice: show `text` for `ms`, then restore the title.
 * Runs on a short detached thread so it never blocks; guarded by IsWindow. */
typedef struct { HWND hwnd ; int ms ; char text[512] ; char saved[512] ; } kitty_titlenotice_t ;

/* Tint the title bar green while the notice shows so it is actually noticed
 * (Windows 11 DWMWA_CAPTION_COLOR/DWMWA_TEXT_COLOR; silent no-op on older
 * Windows). dwmapi.dll is loaded on demand so nothing new is linked. */
static void kitty_caption_tint( HWND hwnd, int on ) {
	typedef HRESULT (WINAPI *dwmswa_t)( HWND, DWORD, LPCVOID, DWORD ) ;
	static dwmswa_t dwmswa = NULL ;
	static int inited = 0 ;
	if( !inited ) {
		HMODULE dwm = LoadLibraryA( "dwmapi.dll" ) ;
		if( dwm ) dwmswa = (dwmswa_t)kitty_api_from(dwm, "dwmapi.dll", "DwmSetWindowAttribute", KITTY_API_OPTIONAL,
                                  KT_WINFEAT_DARK_TITLEBARS) ;
		inited = 1 ;
	}
	if( dwmswa ) {
		/* 35/36 = DWMWA_CAPTION_COLOR/DWMWA_TEXT_COLOR (absent from older
		 * MinGW headers); 0xFFFFFFFF = DWMWA_COLOR_DEFAULT. */
		DWORD caption = on ? (DWORD)RGB(16,124,16)    : 0xFFFFFFFFu ;
		DWORD text    = on ? (DWORD)RGB(255,255,255)  : 0xFFFFFFFFu ;
		dwmswa( hwnd, 35, &caption, sizeof(caption) ) ;
		dwmswa( hwnd, 36, &text, sizeof(text) ) ;
	}
}
static DWORD WINAPI kitty_titlenotice_thread( LPVOID p ) {
	kitty_titlenotice_t *t = (kitty_titlenotice_t*)p ;
	if( IsWindow(t->hwnd) ) { kitty_caption_tint( t->hwnd, 1 ) ; SetWindowTextA( t->hwnd, t->text ) ; }
	Sleep( t->ms ) ;
	if( IsWindow(t->hwnd) ) { kitty_caption_tint( t->hwnd, 0 ) ; SetWindowTextA( t->hwnd, t->saved ) ; }
	free( t ) ;
	return 0 ;
}
static void kitty_title_notice( HWND hwnd, const char *text, int ms ) {
	if( !hwnd || !IsWindow(hwnd) ) return ;
	kitty_titlenotice_t *t = (kitty_titlenotice_t*)calloc( 1, sizeof(*t) ) ;
	if( !t ) return ;
	t->hwnd = hwnd ; t->ms = ms ;
	GetWindowTextA( hwnd, t->saved, sizeof(t->saved)-1 ) ;
	strncpy( t->text, text, sizeof(t->text)-1 ) ;
	HANDLE th = CreateThread( NULL, 0, kitty_titlenotice_thread, t, 0, NULL ) ;
	if( th ) CloseHandle( th ) ; else free( t ) ;
}

void CheckVersionFromWebSite( HWND hwnd, int is_terminal ) {
	char curnum[64]="" ;
	int i ;

	/* Reduce the build string ("0.84.0.15-beta @ ...") to its numeric prefix. */
	strncpy( curnum, BuildVersionTime, sizeof(curnum)-1 ) ; curnum[sizeof(curnum)-1]='\0' ;
	for( i=0 ; i<(int)strlen(curnum) ; i++ ) {
		if( !(((curnum[i]>='0')&&(curnum[i]<='9'))||(curnum[i]=='.')) ) { curnum[i]='\0' ; break ; }
		}

	/* KiTTY: is THIS build a beta? (stable builds don't silently take betas) */
	/* Channel of THIS build (see kitty_update_notice): version scheme + string. */
	int cur_is_beta ;
	{ int cvb[4] ; kitty_parse_version( curnum, cvb ) ;
	  cur_is_beta = ( cvb[3] != 0 ) || ( strstr(BuildVersionTime,"beta")!=NULL )
	                                || ( strstr(BuildVersionTime,"BETA")!=NULL ) ; }

	/* Fetch the latest release JSON from GitHub. GitHub requires a User-Agent
	 * (set via InternetOpen); PRECONFIG honours the system/IE proxy settings. */
	char *body = NULL ; DWORD bodylen = 0 ; int ok = 0 ;
	HINTERNET hi = InternetOpenA( "KiTTY-UpdateCheck",
	                              INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0 ) ;
	if( hi != NULL ) {
		/* Bound the synchronous request so a blackholed network falls back to
		 * the browser in seconds instead of freezing the UI on the default timeout. */
		DWORD tmo = 8000 ;
		InternetSetOption( hi, INTERNET_OPTION_CONNECT_TIMEOUT, &tmo, sizeof(tmo) ) ;
		InternetSetOption( hi, INTERNET_OPTION_SEND_TIMEOUT,    &tmo, sizeof(tmo) ) ;
		InternetSetOption( hi, INTERNET_OPTION_RECEIVE_TIMEOUT, &tmo, sizeof(tmo) ) ;
		HINTERNET hu = InternetOpenUrlA( hi, KITTY_RELEASES_API,
		                                 "Accept: application/vnd.github+json\r\n",
		                                 (DWORD)-1,
		                                 INTERNET_FLAG_RELOAD|INTERNET_FLAG_NO_CACHE_WRITE|INTERNET_FLAG_SECURE,
		                                 0 ) ;
		if( hu != NULL ) {
			DWORD cap = 65536 ; body = (char*)malloc( cap ) ; bodylen = 0 ;
			if( body != NULL ) {
				DWORD nread = 0 ;
				for( ;; ) {
					if( cap - bodylen < 4096 ) {
						char *nb = (char*)realloc( body, cap*2 ) ;
						if( nb==NULL ) break ; body = nb ; cap *= 2 ;
						}
					if( !InternetReadFile( hu, body+bodylen, cap-bodylen-1, &nread ) || nread==0 ) break ;
					bodylen += nread ;
					}
				body[bodylen] = '\0' ;
				ok = (bodylen>0) ;
				}
			InternetCloseHandle( hu ) ;
			}
		InternetCloseHandle( hi ) ;
		}

	/* Extract "tag_name":"kitty-0.84.0.16-beta" and compare. */
	if( ok && (body!=NULL) ) {
		char *p = strstr( body, "\"tag_name\"" ) ;
		char latestnum[64]="" ;
		char tag[128]="" ;   /* KiTTY: the full tag, e.g. "kitty-0.84.1.72-beta",
		                      * kept for the "View release notes" URL */
		int latest_is_beta = 0 ;
		/* Channel of the newest release from GitHub's own "prerelease" flag
		 * (betas are published with "prerelease":true) rather than the tag text;
		 * per_page=1 so the first occurrence is this newest release. */
		{
			char *pr = strstr( body, "\"prerelease\"" ) ;
			if( pr != NULL ) {
				pr = strchr( pr, ':' ) ; if( pr!=NULL ) pr++ ;
				while( (pr!=NULL) && (*pr==' '||*pr=='\t') ) pr++ ;
				latest_is_beta = ( (pr!=NULL) && (strncmp(pr,"true",4)==0) ) ;
				}
			}
		if( p != NULL ) {
			p = strchr( p, ':' ) ; if( p!=NULL ) p++ ;
			while( (p!=NULL) && (*p==' '||*p=='\"') ) p++ ;
			int j=0 ;
			while( (p!=NULL) && *p && (*p!='\"') && (j<(int)sizeof(tag)-1) ) { tag[j++]=*p++ ; }
			tag[j]='\0' ;
			/* tag is e.g. "kitty-0.84.0.16-beta": skip to the first digit, keep digits/dots. */
			char *d = tag ; while( *d && !((*d>='0')&&(*d<='9')) ) d++ ;
			int k=0 ; while( *d && (((*d>='0')&&(*d<='9'))||(*d=='.')) && (k<(int)sizeof(latestnum)-1) ) { latestnum[k++]=*d++ ; }
			latestnum[k]='\0' ;
			}
		if( latestnum[0] ) {
			int cv[4], lv[4] ; char msg[512] ;
			kitty_parse_version( curnum, cv ) ;
			kitty_parse_version( latestnum, lv ) ;
			if( kitty_version_cmp( cv, lv ) < 0 ) {
				/* An update is available. Compose the message + pick the action,
				 * then show the NON-modal, no-sound popup over the caller window
				 * (it stays up until the user picks Update/Later). The download/
				 * verify/install runs from its "Update" button
				 * (kitty_do_msi_update), which keeps modal error boxes. */
				int stable_taking_beta = ( !cur_is_beta && latest_is_beta ) ;
				kitty_install_t itype = kitty_detect_install_type() ;
				/* KiTTY: that release's GitHub page, for "View release notes". */
				char notesurl[512]="" ;
				if( tag[0] )
					snprintf( notesurl, sizeof(notesurl), "%s/tag/%s",
						KITTY_RELEASES_URL, tag ) ;
				char asseturl[1024]="" ; int haveasset = 0 ;
				if( itype != KITTY_INST_PORTABLE ) {
					const char *suffix = (itype==KITTY_INST_SYSTEM)
						? "-x64-system.msi" : "-x64-peruser.msi" ;
					haveasset = kitty_find_asset_url( body, suffix, asseturl, sizeof(asseturl) ) ;
				}
				free( body ) ; body = NULL ;
				/* Refuse a non-HTTPS asset URL (never auto-download+run over plain http). */
				if( haveasset && strncmp( asseturl, "https://", 8 )!=0 ) haveasset = 0 ;

				if( itype==KITTY_INST_PORTABLE || !haveasset ) {
					/* Portable copy or no matching asset: offer the download page. */
					snprintf( msg, sizeof(msg),
						KT_UPD_AVAILABLE_HEAD,
						curnum, latestnum, stable_taking_beta ? KT_UPD_BETA_MARK : "",
						(itype==KITTY_INST_PORTABLE)
						  ? KT_UPD_PORTABLE_OPEN_PAGE
						  : KT_UPD_NO_ASSET_OPEN_PAGE ) ;
					kitty_show_update_popup( hwnd, msg, KUP_ACT_OPENPAGE, KITTY_RELEASES_URL, itype, notesurl ) ;
					return ;
				}
				/* MSI install path. If a stable build is offered a beta, say so in
				 * the text (the "Update now" button is the explicit opt-in). */
				snprintf( msg, sizeof(msg),
					KT_UPD_AVAILABLE_HEAD
					KT_UPD_AVAILABLE_MSI_TAIL,
					curnum, latestnum, stable_taking_beta ? KT_UPD_BETA_MARK : "",
					stable_taking_beta
					  ? KT_UPD_STABLE_TAKING_BETA
					  : "" ) ;
				kitty_show_update_popup( hwnd, msg, KUP_ACT_MSI, asseturl, itype, notesurl ) ;
				return ;
			} else {
				/* No update. In a live terminal, state it in the title for 3s (no
				 * box at all). Elsewhere (config box), a non-modal auto-dismiss box. */
				if( is_terminal ) {
					char note[256] ;
					snprintf( note, sizeof(note), KT_UPD_UP_TO_DATE_TITLE,
						curnum, cur_is_beta ? "-beta" : "" ) ;
					kitty_title_notice( hwnd, note, 5000 ) ;
				} else {
					snprintf( msg, sizeof(msg),
						KT_UPD_UP_TO_DATE,
						curnum, latestnum ) ;
					kitty_show_update_popup( hwnd, msg, KUP_ACT_NONE, NULL, 0, NULL ) ;
				}
				free( body ) ;
				return ;
			}
		}

		}

	/* Fallback (offline / proxy / TLS / parse failure): open the releases page. */
	if( body!=NULL ) free( body ) ;
	ShellExecute( hwnd, "open", KITTY_RELEASES_URL, 0, 0, SW_SHOWDEFAULT ) ;
}
