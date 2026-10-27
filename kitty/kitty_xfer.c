/*
 * KiTTY file-transfer and external-tool integration, moved verbatim out of
 * kitty.c to shrink that monolith: the kscp transfer window (live output
 * capture, cancel button, success tray balloon), the injection-hardened
 * kscp command builders (SendOneFile/SendFileList/SendFile,
 * GetOneFile/GetFile, RunCmd), external-tool path discovery
 * (SearchWinSCP/SearchPSCP), StartWinSCP, and the
 * kscp-upload drag-and-drop handlers. Compiled into the same targets as
 * kitty.c (kitty + kitty_portable), so behaviour is unchanged.
 *
 * This is the helper-program mechanism (kscp, WinSCP, FileZilla run as
 * separate processes); the unrelated in-terminal OSC 5113 file-transfer
 * protocol, one letter away in kitty_transfer.c, is a different mechanism.
 */
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "putty.h"

#include <windows.h>
#include <shellapi.h>        /* SHFileOperation: remove a leftover staging tree */

#include "kitty.h"
#include "kitty_params.h"
#include "kitty_common.h"    /* MASKPASS */

#include "kitty_tools.h"     /* existfile/existdirectory, poss, set_env */
#include "kitty_winutil.h"   /* the pickers, RunCommand */
#include "kitty_oldwin.h"    /* paths read in full: kitty_*_path_dup */
#include "kitty_dlgbox.h"
#include "kitty_hello_keys.h" /* kageant_hello_has_sidecar */
#include "ssh.h"             /* the agent protocol, for the "is it loaded?" check */
#include "kitty_msgbox.h"   /* themed MessageBox routing */
#include "kitty_text.h"     /* shared captions and wordings */
#include "kitty_inikeys.h"  /* KI_*: the kitty.ini key names */
#include "kitty_pwmem.h"    /* passwords wrapped in memory */
#include "kitty_droppath.h" /* the remote paths typed after a drop upload */
#include "kitty_getfile_parse.h" /* Get File's check: the clipboard text, kscp's reply */
#include "kitty_notice.h"   /* Get File's "found" notice */
#include "kitty_buildlabel.h" /* KITTY_TEST_BUILD_LABEL: the notice trace below */

/* A user action that cannot go on (no SSH, a tool not found, kscp not
 * started) is reported in the notice window near the clock, which never takes
 * the focus: a box here stood in front of a live session and stopped it -
 * no output, no keepalive - until it was answered. Amber, the suite's
 * warning accent. */
#define KX_ERR_ACCENT RGB( 190, 110, 0 )
static void kx_report( const char *caption, const char *text ) {
	kitty_notice_show( caption, text, KX_ERR_ACCENT, GetNoticeSeconds(), NULL, 0 ) ;
}

/* The port inside a target override ([user@]hostname[:port]), 0 if it gives
 * none. Defined beside kitty_xfer_default_port(), used by the builders above
 * it as well. */
static int kx_override_port( const char * ov ) ;

/*
 * KiTTY: log a command line that had a password built into it.
 *
 * The command itself needs the secret - that is how kscp and WinSCP are
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
 * deliberately. Neither kscp nor WinSCP knows anything about the
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
 * showing rather than letting kscp or WinSCP fail with a bare
 * "server refused our key" that gives no cause.
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
          resp = NULL ; resplen = 0 ;   /* async: not what we requested */
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
 * nothing to report. Caller sfrees.
 */
static const char *kx_keyfile_path(Conf *c) ;
static char *kx_hello_agent_note(Conf *c)
{
    const char *path = kx_keyfile_path( c ) ;
    if( path==NULL || strlen(path)==0 ) return NULL ;
    if( !kageant_hello_has_sidecar( path ) ) return NULL ;
    if( kx_agent_holds( path ) ) return NULL ;
    return dupprintf(
        KT_XFER_HELLO_NOTE, path ) ;
}

/* The session's key file as it is on disk: %VAR% expanded, so the Hello
 * sidecar is found next to it (hknet/KiTTY#70 sweep). In full; held until
 * the next call, so it is used before that. */
static const char *kx_keyfile_path(Conf *c)
{
    static char *path = NULL ;
    char *e = filename_expand_str( conf_get_filename( c, CONF_keyfile ) ) ;
    sfree( path ) ;
    path = e ? e : dupstr( "" ) ;
    return path ;
}

static const char *kx_helper_keyfile(Conf *c)
{
    const char *path = kx_keyfile_path( c ) ;
    if( path==NULL || strlen(path)==0 ) return NULL ;
    if( kageant_hello_has_sidecar(path) ) {
        debug_logevent("transfer: key file is Hello-protected, left to the agent: %s", path) ;
        return NULL ;
    }
    return path ;
}

/* Provided elsewhere in the KiTTY tree (not in a header). */

/* KiTTY security: the helpers are started WITHOUT a shell (CreateProcess, no
 * cmd.exe), so session fields spliced into a command line cannot inject shell
 * commands - & | > ` and friends are taken literally. The quoting layers
 * below are what keeps each value one argument: bcat appends to a strbuf
 * that grows (nothing cut, no overflow), qcat applies Win32 argv quoting to each single
 * value (no injected extra arguments), rawcat applies WinSCP's rawsettings
 * rule, urlcat percent-encodes URL userinfo and pathcat a URL's folder. What
 * is still appended RAW is deliberate and documented at each site:
 * pscpoptions, winscpoptions and winscprawsettings are the user writing
 * command line on purpose, and quoting them would break the feature. */

/*
 * HELPER PROGRAMS: the one place a helper is started, and the signature check
 * in front of it.
 *
 * kscp/pscp, rz/sz and klink are programs KiTTY++ finds by a search or a
 * configured path and starts with the session's login - host, user, a
 * password file. Anything that can put a program there would be handed live
 * credentials by a program the user trusts. So every one of them starts
 * through kitty_helper_start(), and that runs a program only when
 *
 *  1. it is one of our tools (kscp.exe, klink.exe) BESIDE kitty.exe and
 *     kitty_verify_sibling() accepts it: our signature (when kitty.exe is
 *     signed) and exactly our version. Otherwise it is refused;
 *  2. anywhere else: it carries our signature (kitty_authenticode_verify), it
 *     is a Windows component (listed in a Windows catalog this machine
 *     trusts: kh_catalog_signer), or the user allowed exactly this file -
 *     path and SHA-256 - before. A broken signature (the file changed after
 *     it was signed) is refused.
 *
 * Allowing happens one step earlier, where a user action finds its helper
 * (kitty_helper_ready): before any password file exists, a modeless box shows
 * the file and its signer; Yes remembers it (TrustedHelpers: a registry key,
 * or [TrustedHelpers] in kitty.ini in portable mode) and runs the action
 * again. The file is held open, writes and deletion refused, from the check
 * until the process has started, so what was checked is what runs.
 */
#include "kitty_authenticode.h"   /* kitty_verify_sibling, kitty_authenticode_verify */
#include "kitty_renameguard.h"    /* kitty_signature_reading, kitty_eventlog_line */
#include "mini/mini.h"            /* readINI / writeINI: portable mode's store */
#include "kitty_storage.h"        /* kitty_registry_base: the registry store */

#define KH_STORE_SECTION "TrustedHelpers"

enum { KHJ_RUN, KHJ_CONFIRM_SIGNED, KHJ_CONFIRM_UNSIGNED, KHJ_CONFIRM_CANNOT,
       KHJ_REFUSE_SIBLING, KHJ_REFUSE_BROKEN, KHJ_MISSING };

/* The full long path, so one file has one name in the store; in full
 * (sfree it), NULL when Windows cannot give it. No long form (the file is
 * not there): the full path. */
static char *kh_full_path_dup( const char *in ) {
	char *full = kitty_full_path_dup( in ), *lp ;
	if( full == NULL ) return NULL ;
	if( ( lp = kitty_long_path_dup( full ) ) != NULL ) { sfree( full ) ; return lp ; }
	return full ;
}

/* Our own tool, beside the running kitty.exe? Rule 1 applies to these only. */
static int kh_is_our_sibling( const char *full ) {
	char *self = kitty_module_path_dup( NULL ), *selffull, *s ;
	const char *base ;
	int ours = 0 ;
	selffull = self ? kh_full_path_dup( self ) : NULL ;
	sfree( self ) ;
	if( selffull == NULL ) return 0 ;
	s = strrchr( selffull, '\\' ) ; base = strrchr( full, '\\' ) ;
	if( s && base ) {
		base++ ;
		if( ( !stricmp( base, "kscp.exe" ) || !stricmp( base, "klink.exe" ) )
		    && (size_t)( base - full ) == (size_t)( s + 1 - selffull ) )
			ours = strnicmp( full, selffull, (size_t)( s + 1 - selffull ) ) == 0 ;
	}
	sfree( selffull ) ;
	return ours ;
}

/* SHA-256 of the open file, as 64 hex digits. */
static int kh_sha256( HANDLE h, char hex[65] ) {
	unsigned char buf[65536], digest[32] ;
	DWORD got ;
	ssh_hash *sh = ssh_hash_new( &ssh_sha256 ) ;
	LARGE_INTEGER zero ; zero.QuadPart = 0 ;
	if( !SetFilePointerEx( h, zero, NULL, FILE_BEGIN ) ) { ssh_hash_free( sh ) ; return 0 ; }
	for(;;) {
		if( !ReadFile( h, buf, sizeof(buf), &got, NULL ) ) { ssh_hash_free( sh ) ; return 0 ; }
		if( got == 0 ) break ;
		put_data( sh, buf, got ) ;
	}
	ssh_hash_final( sh, digest ) ;
	for( int i = 0 ; i < 32 ; i++ ) sprintf( hex + 2*i, "%02x", digest[i] ) ;
	return 1 ;
}

/* The store: value name = the lower-case full path, data = the SHA-256. */
static char *kh_store_name_dup( const char *full ) {
	char *name = dupstr( full ) ;
	size_t i ;
	for( i = 0 ; name[i] ; i++ ) name[i] = (char)tolower( (unsigned char)name[i] ) ;
	return name ;
}
/* The stored SHA-256 for this file (sfree it), NULL = none. */
static char *kh_store_get_dup( const char *full ) {
	char *name = kh_store_name_dup( full ), *hex = NULL ;
	if( IniFileFlag == SAVEMODE_DIR ) {
		hex = kitty_read_ini_dup( GetKittyIniFile(), KH_STORE_SECTION, name ) ;
	} else {
		char *key = dupprintf( "%s\\%s", kitty_registry_base(), KH_STORE_SECTION ) ;
		HKEY hk ; DWORD type = 0, len = 0 ;
		if( RegOpenKeyExA( HKEY_CURRENT_USER, key, 0, KEY_READ, &hk ) == ERROR_SUCCESS ) {
			unsigned char *d = kitty_reg_query_dup( hk, name, &type, &len ) ;
			if( d != NULL && type == REG_SZ ) hex = (char *)d ;
			else sfree( d ) ;
			RegCloseKey( hk ) ;
		}
		sfree( key ) ;
	}
	sfree( name ) ;
	if( hex != NULL && !hex[0] ) { sfree( hex ) ; hex = NULL ; }
	return hex ;
}
static void kh_store_put( const char *full, const char *hex ) {
	char *name ;
	if( GetReadOnlyFlag() ) return ;   /* readonly=yes: confirmed again next time */
	name = kh_store_name_dup( full ) ;
	if( strchr( name, '=' ) ) { sfree( name ) ; return ; }   /* not a kitty.ini key: confirmed again next time */
	if( IniFileFlag == SAVEMODE_DIR ) {
		if( !GetReadOnlyFlag() ) writeINI( GetKittyIniFile(), KH_STORE_SECTION, name, hex ) ;
	} else {
		char *key = dupprintf( "%s\\%s", kitty_registry_base(), KH_STORE_SECTION ) ;
		HKEY hk ;
		if( RegCreateKeyExA( HKEY_CURRENT_USER, key, 0, NULL, 0, KEY_WRITE, NULL, &hk, NULL ) == ERROR_SUCCESS ) {
			RegSetValueExA( hk, name, 0, REG_SZ, (const BYTE *)hex, (DWORD)strlen( hex ) + 1 ) ;
			RegCloseKey( hk ) ;
		}
		sfree( key ) ;
	}
	sfree( name ) ;
}

/* Open the file for the check and the start: reading shared, writing and
 * deleting refused until it is closed. */
static HANDLE kh_hold( const char *full ) {
	return CreateFileA( full, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
	                    FILE_ATTRIBUTE_NORMAL, NULL ) ;
}

/* Files this process found to be ours, or the user allowed, by path and
 * SHA-256: the signature check goes online for revocation, which can take
 * seconds on a machine without a network, and every action checks twice
 * (ready, then start). An allowed file the store cannot keep (a read-only
 * kitty.ini) is still allowed for the action that wanted it. */
#define KH_OURS_CACHE 8
static struct { char *full ; char hex[65] ; } kh_ours[KH_OURS_CACHE] ;
static int kh_ours_next = 0 ;
static volatile LONG kh_ours_lock = 0 ;   /* klink starts from a worker thread too */
static void kh_ours_enter( void ) { while( InterlockedExchange( &kh_ours_lock, 1 ) ) Sleep( 0 ) ; }
static void kh_ours_leave( void ) { InterlockedExchange( &kh_ours_lock, 0 ) ; }
static int kh_ours_known( const char *full, const char *hex ) {
	int found = 0 ;
	kh_ours_enter() ;
	for( int i = 0 ; i < KH_OURS_CACHE && !found ; i++ )
		if( kh_ours[i].full && kh_ours[i].hex[0] && !strcmp( kh_ours[i].hex, hex ) && !stricmp( kh_ours[i].full, full ) )
			found = 1 ;
	kh_ours_leave() ;
	return found ;
}
static void kh_ours_add( const char *full, const char *hex ) {
	char *copy = dupstr( full ), *old ;
	kh_ours_enter() ;
	old = kh_ours[kh_ours_next].full ;
	kh_ours[kh_ours_next].full = copy ;
	snprintf( kh_ours[kh_ours_next].hex, sizeof(kh_ours[0].hex), "%s", hex ) ;
	kh_ours_next = ( kh_ours_next + 1 ) % KH_OURS_CACHE ;
	kh_ours_leave() ;
	sfree( old ) ;
}

/*
 * A file Windows signs in a CATALOG rather than in the file - its own
 * programs (wsl.exe, where.exe ...) - reads as unsigned to the in-file check.
 * Its catalog is looked up here: 1 and the catalog's signer when the file is
 * listed in a catalog this machine trusts; such a file runs without a
 * confirmation (kh_judge). The catalog's trust comes from what the machine
 * holds - revocation is not fetched, as for any Windows component.
 * SHA-256 lookup (Windows 8+, loaded at run time: XP has no such entry
 * points), else the SHA-1 one.
 */
#include <mscat.h>
#include <softpub.h>
typedef BOOL (WINAPI *kh_acquire2_fn)( HCATADMIN *, const GUID *, const WCHAR *,
                                       const CERT_STRONG_SIGN_PARA *, DWORD ) ;
typedef BOOL (WINAPI *kh_hash2_fn)( HCATADMIN, HANDLE, DWORD *, BYTE *, DWORD ) ;
static int kh_catalog_signer( const char *full, HANDLE held, char *signer, size_t n ) {
	HMODULE wt = GetModuleHandleA( "wintrust.dll" ) ;
	kh_acquire2_fn acquire2 = wt ? (kh_acquire2_fn)GetProcAddress( wt, "CryptCATAdminAcquireContext2" ) : NULL ;
	kh_hash2_fn hash2 = wt ? (kh_hash2_fn)GetProcAddress( wt, "CryptCATAdminCalcHashFromFileHandle2" ) : NULL ;
	WCHAR *wfull ;
	GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2 ;
	int ok = 0, pass ;
	LARGE_INTEGER zero ; zero.QuadPart = 0 ;

	signer[0] = '\0' ;
	if( ( wfull = kitty_acp_to_wide_dup( full ) ) == NULL ) return 0 ;
	/* pass 0: SHA-256, pass 1: SHA-1. A catalog lists a file under one of
	 * them - many Windows catalogs still under SHA-1 - so a hash no catalog
	 * knows is tried the other way before the file counts as unsigned. */
	for( pass = 0 ; pass < 2 && !ok ; pass++ ) {
		HCATADMIN cat = NULL ;
		HCATINFO info = NULL ;
		BYTE hash[64] ;
		DWORD hsize = sizeof(hash) ;
		WCHAR tag[129] ;
		CATALOG_INFO ci ;
		SetFilePointerEx( held, zero, NULL, FILE_BEGIN ) ;
		if( pass == 0 ) {
			if( !acquire2 || !hash2 || !acquire2( &cat, NULL, L"SHA256", NULL, 0 ) ) continue ;
			if( !hash2( cat, held, &hsize, hash, 0 ) ) { CryptCATAdminReleaseContext( cat, 0 ) ; continue ; }
		} else {
			if( !CryptCATAdminAcquireContext( &cat, NULL, 0 ) ) continue ;
			if( !CryptCATAdminCalcHashFromFileHandle( held, &hsize, hash, 0 ) ) { CryptCATAdminReleaseContext( cat, 0 ) ; continue ; }
		}
		info = CryptCATAdminEnumCatalogFromHash( cat, hash, hsize, 0, NULL ) ;
		memset( &ci, 0, sizeof(ci) ) ; ci.cbStruct = sizeof(ci) ;
		if( info && CryptCATCatalogInfoFromContext( info, &ci, 0 ) ) {
			WINTRUST_CATALOG_INFO wci ;
			WINTRUST_DATA wd ;
			{   /* the member tag: the hash as upper-case hex */
				static const WCHAR hx[] = L"0123456789ABCDEF" ;
				DWORD i ;
				for( i = 0 ; i < hsize && i < 64 ; i++ ) { tag[2*i] = hx[hash[i] >> 4] ; tag[2*i+1] = hx[hash[i] & 15] ; }
				tag[2*i] = 0 ;
			}
			memset( &wci, 0, sizeof(wci) ) ; wci.cbStruct = sizeof(wci) ;
			wci.pcwszCatalogFilePath = ci.wszCatalogFile ;
			wci.pcwszMemberFilePath = wfull ;
			wci.pcwszMemberTag = tag ;
			wci.pbCalculatedFileHash = hash ;
			wci.cbCalculatedFileHash = hsize ;
			wci.hMemberFile = held ;
			memset( &wd, 0, sizeof(wd) ) ; wd.cbStruct = sizeof(wd) ;
			wd.dwUIChoice = WTD_UI_NONE ;
			wd.fdwRevocationChecks = WTD_REVOKE_NONE ;
			wd.dwUnionChoice = WTD_CHOICE_CATALOG ;
			wd.pCatalog = &wci ;
			wd.dwStateAction = WTD_STATEACTION_VERIFY ;
			wd.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL ;
			if( WinVerifyTrust( NULL, &action, &wd ) == ERROR_SUCCESS ) {
				CRYPT_PROVIDER_DATA *pd = WTHelperProvDataFromStateData( wd.hWVTStateData ) ;
				CRYPT_PROVIDER_SGNR *sg = pd ? WTHelperGetProvSignerFromChain( pd, 0, FALSE, 0 ) : NULL ;
				ok = 1 ;
				if( sg && sg->csCertChain > 0 && sg->pasCertChain && sg->pasCertChain[0].pCert )
					CertGetNameStringA( sg->pasCertChain[0].pCert, CERT_NAME_ATTR_TYPE, 0,
					                    (void *)szOID_COMMON_NAME, signer, (DWORD)n ) ;
			}
			wd.dwStateAction = WTD_STATEACTION_CLOSE ;
			WinVerifyTrust( NULL, &action, &wd ) ;
		}
		if( info ) CryptCATAdminReleaseCatalogContext( cat, info, 0 ) ;
		CryptCATAdminReleaseContext( cat, 0 ) ;
	}
	sfree( wfull ) ;
	return ok ;
}

/* What may happen with the held file: run, confirm first (signer filled in), refuse.
 * hex: the file's SHA-256, for the store. */
static int kh_judge( const char *full, HANDLE held, char *signer, size_t signersz, char hex[65] ) {
	char *known ;
	int sig ;
	signer[0] = '\0' ; hex[0] = '\0' ;
	if( held == INVALID_HANDLE_VALUE || !kh_sha256( held, hex ) ) return KHJ_MISSING ;
	if( kh_ours_known( full, hex ) ) return KHJ_RUN ;
	if( kh_is_our_sibling( full ) ) {
		if( !kitty_verify_sibling( full ) ) return KHJ_REFUSE_SIBLING ;
		kh_ours_add( full, hex ) ;
		return KHJ_RUN ;
	}
	if( kitty_authenticode_verify( full ) ) { kh_ours_add( full, hex ) ; return KHJ_RUN ; }
	sig = kitty_signature_reading( full, signer, signersz ) ;
	if( sig == KG_SIG_MODIFIED ) return KHJ_REFUSE_BROKEN ;
	if( ( known = kh_store_get_dup( full ) ) != NULL ) {
		int same = !stricmp( known, hex ) ;
		sfree( known ) ;
		if( same ) return KHJ_RUN ;
	}
	if( sig == KG_SIG_OURS || sig == KG_SIG_OTHER ) return KHJ_CONFIRM_SIGNED ;
	/* No signature in the file, but listed in a Windows catalog this machine
	 * trusts: a component of Windows (wsl.exe for rz/sz through WSL), which
	 * only an administrator can replace and Windows Update replaces often - a
	 * confirmation pinned to its SHA-256 would come back after every update.
	 * It runs (his word). A changed or planted copy matches no catalog and
	 * falls through to the unsigned confirmation below. */
	if( ( sig == KG_SIG_UNSIGNED || sig == KG_SIG_CANNOT ) &&
	    kh_catalog_signer( full, held, signer, signersz ) ) {
		kh_ours_add( full, hex ) ;
		return KHJ_RUN ;
	}
	if( sig == KG_SIG_CANNOT ) return KHJ_CONFIRM_CANNOT ;
	return KHJ_CONFIRM_UNSIGNED ;
}

static const char *kh_base( const char *full ) {
	const char *b = strrchr( full, '\\' ) ;
	return b ? b + 1 : full ;
}

static void kh_log( unsigned type, const char *fmt, const char *full, const char *why ) {
	char *m = dupprintf( fmt, full, why ) ;
	kitty_eventlog_line( type, m ) ;
	sfree( m ) ;
}

/* One open box per file; every action that wanted the file while it was open
 * runs on its Yes, in the order they came. */
typedef struct kh_retry { void (*fn)( void * ) ; void *ctx ; void (*ctxfree)( void * ) ; struct kh_retry *next ; } kh_retry ;
typedef struct kh_pending { char *full ; char hex[65] ; char *why ; HWND box ;
                            kh_retry *retries ; struct kh_pending *next ; } kh_pending ;
static kh_pending *kh_pendings = NULL ;

static void kh_answer( int yes, void *vp ) {
	kh_pending *p = (kh_pending *)vp, **pp ;
	kh_retry *r, *rn ;
	for( pp = &kh_pendings ; *pp ; pp = &(*pp)->next )
		if( *pp == p ) { *pp = p->next ; break ; }
	kh_log( yes ? EVENTLOG_INFORMATION_TYPE : EVENTLOG_WARNING_TYPE,
	        yes ? KT_HELPER_LOG_ALLOWED : KT_HELPER_LOG_DECLINED, p->full, p->why ) ;
	if( yes ) { kh_store_put( p->full, p->hex ) ; kh_ours_add( p->full, p->hex ) ; }
	for( r = p->retries ; r ; r = rn ) {
		rn = r->next ;
		if( yes ) r->fn( r->ctx ) ;
		if( r->ctxfree ) r->ctxfree( r->ctx ) ;
		sfree( r ) ;
	}
	sfree( p->full ) ; sfree( p->why ) ;
	sfree( p ) ;
}

/*
 * A user action found its helper at `path`: may it go on now?
 * 1 = yes (ctx is freed here, nothing waits). 0 = not now: the file was
 * refused (a box gives the reason), or a confirmation box is open and fn(ctx) runs on its Yes;
 * ctx is freed either way. password: whether the helper receives one, for the
 * warning's wording. zmodem: the box adds that the transfer may need starting
 * again.
 */
int kitty_helper_ready( HWND owner, const char *path, int password, int zmodem,
                        void (*fn)( void * ), void *ctx, void (*ctxfree)( void * ) ) {
	/* signer: a certificate's common name, at most 64 characters (X.520) */
	char *full, signer[256], hex[65], *detail, *text ;
	const char *warn = NULL ;
	char *sigline ;
	HANDLE held ;
	int j ;
	kh_pending *p ;
	kh_retry *r ;

	if( !path || ( full = kh_full_path_dup( path ) ) == NULL ) {
		if( ctxfree ) ctxfree( ctx ) ;
		return 1 ;   /* nothing there: the action reports its own "not found" */
	}
	held = kh_hold( full ) ;
	j = kh_judge( full, held, signer, sizeof(signer), hex ) ;
	if( held != INVALID_HANDLE_VALUE ) CloseHandle( held ) ;
	if( j == KHJ_RUN || j == KHJ_MISSING ) {
		sfree( full ) ;
		if( ctxfree ) ctxfree( ctx ) ;
		return 1 ;   /* missing: the start fails and reports it as before */
	}
	if( j == KHJ_REFUSE_SIBLING || j == KHJ_REFUSE_BROKEN ) {
		text = dupprintf( j == KHJ_REFUSE_SIBLING ? KT_HELPER_REFUSE_SIBLING : KT_HELPER_REFUSE_BROKEN, kh_base( full ) ) ;
		kitty_info_modeless( owner, KT_HELPER_CAPTION, text, full,
		                     j == KHJ_REFUSE_SIBLING ? KT_HELPER_REFUSE_SIBLING_WARN : KT_HELPER_REFUSE_BROKEN_WARN ) ;
		kh_log( EVENTLOG_WARNING_TYPE, KT_HELPER_LOG_REFUSED, full,
		        j == KHJ_REFUSE_SIBLING ? KT_HELPER_WHY_SIBLING : KT_HELPER_WHY_BROKEN ) ;
		sfree( text ) ;
		sfree( full ) ;
		if( ctxfree ) ctxfree( ctx ) ;
		return 0 ;
	}
	/* a box for this file is open already: this action waits on it too */
	r = snew( kh_retry ) ; r->fn = fn ; r->ctx = ctx ; r->ctxfree = ctxfree ; r->next = NULL ;
	for( p = kh_pendings ; p ; p = p->next )
		if( !stricmp( p->full, full ) ) {
			kh_retry **t = &p->retries ;
			while( *t ) t = &(*t)->next ;
			*t = r ;
			if( p->box ) SetForegroundWindow( p->box ) ;
			sfree( full ) ;
			return 0 ;
		}
	if( j == KHJ_CONFIRM_SIGNED ) {
		sigline = dupprintf( KT_HELPER_SIGNED_BY, signer[0] ? signer : "?" ) ;
	} else if( j == KHJ_CONFIRM_CANNOT ) {
		sigline = dupstr( KT_HELPER_NOT_CHECKABLE ) ; warn = KT_HELPER_WARN_CANNOT ;
	} else {
		sigline = dupstr( KT_HELPER_NOT_SIGNED ) ; warn = password ? KT_HELPER_WARN_UNSIGNED_PW : KT_HELPER_WARN_UNSIGNED ;
	}
	p = snew( kh_pending ) ; memset( p, 0, sizeof(*p) ) ;
	p->full = full ;                   /* the pending entry's from here on */
	strcpy( p->hex, hex ) ;
	p->why = sigline ;
	p->retries = r ;
	p->next = kh_pendings ; kh_pendings = p ;
	detail = dupprintf( "%s\r\n%s", full, sigline ) ;
	p->box = kitty_confirm_modeless( owner, KT_HELPER_CAPTION,
	                                 zmodem ? KT_HELPER_CONFIRM_ZMODEM : KT_HELPER_CONFIRM_TEXT,
	                                 detail, warn, kh_answer, p ) ;
	sfree( detail ) ;
	if( !p->box ) kh_answer( 0, p ) ;   /* no box: that is a No */
	return 0 ;
}

/*
 * THE start of a helper. The file at `path` is held open, checked (rule 1 or
 * rule 2 above - no box here: allowing is kitty_helper_ready's) and started
 * as lpApplicationName, so exactly the checked file runs. The other
 * arguments are CreateProcessA's. FALSE with ERROR_ACCESS_DENIED when the
 * check failed (one event-log line).
 */
static BOOL kh_start( const char *path, char *cmdline, BOOL inherit, DWORD flags,
                      const char *workdir, STARTUPINFOA *si, PROCESS_INFORMATION *pi,
                      const HANDLE *h, int nh ) ;
BOOL kitty_helper_start( const char *path, char *cmdline, BOOL inherit, DWORD flags,
                         const char *workdir, STARTUPINFOA *si, PROCESS_INFORMATION *pi ) {
	return kh_start( path, cmdline, inherit, flags, workdir, si, pi, NULL, 0 ) ;
}

/* The same check, the child inheriting exactly h[0..nh-1] and no other
 * handle (kitty_create_process_with_handles): kscp's output pipe and the
 * mapping that holds its password. */
static BOOL kitty_helper_start_handles( const char *path, char *cmdline, DWORD flags,
                                        const char *workdir, STARTUPINFOA *si,
                                        PROCESS_INFORMATION *pi, const HANDLE *h, int nh ) {
	return kh_start( path, cmdline, TRUE, flags, workdir, si, pi, h, nh ) ;
}

static BOOL kh_start( const char *path, char *cmdline, BOOL inherit, DWORD flags,
                      const char *workdir, STARTUPINFOA *si, PROCESS_INFORMATION *pi,
                      const HANDLE *h, int nh ) {
	char *full, signer[256], hex[65] ;
	HANDLE held ;
	int j ;
	BOOL ok ;
	if( !path || ( full = kh_full_path_dup( path ) ) == NULL ) { SetLastError( ERROR_FILE_NOT_FOUND ) ; return FALSE ; }
	held = kh_hold( full ) ;
	if( held == INVALID_HANDLE_VALUE ) { DWORD e = GetLastError() ; sfree( full ) ; SetLastError( e ) ; return FALSE ; }
	j = kh_judge( full, held, signer, sizeof(signer), hex ) ;
	if( j != KHJ_RUN ) {
		CloseHandle( held ) ;
		kh_log( EVENTLOG_WARNING_TYPE, KT_HELPER_LOG_REFUSED, full,
		        j == KHJ_REFUSE_SIBLING ? KT_HELPER_WHY_SIBLING :
		        j == KHJ_REFUSE_BROKEN ? KT_HELPER_WHY_BROKEN : KT_HELPER_WHY_UNCONFIRMED ) ;
		sfree( full ) ;
		SetLastError( ERROR_ACCESS_DENIED ) ;
		return FALSE ;
	}
	if( nh > 0 )
		ok = kitty_create_process_with_handles( full, cmdline, flags,
		                                        ( workdir && workdir[0] ) ? workdir : NULL,
		                                        si, pi, h, nh ) ;
	else
		ok = CreateProcessA( full, cmdline, NULL, NULL, inherit, flags, NULL,
		                     ( workdir && workdir[0] ) ? workdir : NULL, si, pi ) ;
	{ DWORD e = GetLastError() ; CloseHandle( held ) ; sfree( full ) ; SetLastError( e ) ; }
	return ok ;
}

/*
 * WinSCP and FileZilla are NOT checked: switched off here, kept for
 * reference. They update themselves, and a check that remembers a file by its
 * SHA-256 would put up the confirmation again after every update - a regular
 * alert for something the user is supposed to do. They start through
 * RunCommand as before. Setting this to 1 builds the check for them as it
 * was written: kitty_helper_ready in StartWinSCP / StartFileZilla, the start
 * through kitty_helper_run below.
 */
#define KX_CHECK_SELF_UPDATING_TOOLS 0

#if KX_CHECK_SELF_UPDATING_TOOLS
/* A helper with a window of its own (WinSCP, FileZilla): cmdline starts with
 * its quoted path, as the builders write it. Started detached; a path
 * CreateProcess cannot start itself (WinSCPPath may name a .bat) goes to
 * ShellExecute, after the same check. Returns 1 if started. */
static int kitty_helper_run( HWND hwnd, const char *path, char *cmdline ) {
	STARTUPINFOA si ; PROCESS_INFORMATION pi ;
	const char *args = cmdline ;
	memset( &si, 0, sizeof(si) ) ; si.cb = sizeof(si) ;
	memset( &pi, 0, sizeof(pi) ) ;
	if( kitty_helper_start( path, cmdline, FALSE, 0, NULL, &si, &pi ) ) {
		CloseHandle( pi.hThread ) ; CloseHandle( pi.hProcess ) ;
		return 1 ;
	}
	if( GetLastError() == ERROR_ACCESS_DENIED ) return 0 ;
	/* the arguments: everything after the program's own (quoted) name */
	if( *args == '"' ) { args = strchr( args + 1, '"' ) ; args = args ? args + 1 : "" ; }
	else { while( *args && *args != ' ' ) args++ ; }
	while( *args == ' ' ) args++ ;
	{
		char *full, signer[256], hex[65] ;
		HANDLE held ;
		int j ;
		if( ( full = kh_full_path_dup( path ) ) == NULL ) return 0 ;
		held = kh_hold( full ) ;
		j = kh_judge( full, held, signer, sizeof(signer), hex ) ;
		if( j == KHJ_RUN )
			ShellExecuteA( hwnd, "open", full, args, NULL, SW_SHOWNORMAL ) ;
		if( held != INVALID_HANDLE_VALUE ) CloseHandle( held ) ;
		sfree( full ) ;
		return j == KHJ_RUN ;
	}
}
#endif

/* KiTTY: one drop whose remote paths are typed into the terminal once its
 * uploads are over. Each dropped file is a kscp run of its own (its own
 * transfer window); a run that succeeds keeps its file's path, and when the
 * last run of the drop is over the kept paths are typed - nothing when none
 * succeeded. Settled on the GUI thread only: at the start of a run that
 * cannot start, and in the transfer window. */
typedef struct { HWND hwnd ; int n, pending ; char **remote ; char *ok ; } kx_drop_t ;
static kx_drop_t *kx_drop_new( HWND hwnd, int n ) {
	kx_drop_t *d ;
	if( n <= 0 ) return NULL ;
	d = snew( kx_drop_t ) ;
	d->hwnd = hwnd ; d->n = n ; d->pending = n ;
	d->remote = snewn( n, char * ) ; d->ok = snewn( n, char ) ;
	memset( d->remote, 0, n * sizeof(char *) ) ; memset( d->ok, 0, n ) ;
	return d ;
}
static void kx_drop_settle( kx_drop_t *d, int idx, int ok ) {
	int i ;
	if( !d ) return ;
	if( idx >= 0 && idx < d->n && ok ) d->ok[idx] = 1 ;
	if( --d->pending > 0 ) return ;
	{	const char **typed = snewn( d->n, const char * ) ;
		char *text ;
		for( i = 0 ; i < d->n ; i++ ) typed[i] = d->ok[i] ? d->remote[i] : NULL ;
		/* Plain text: term_do_paste brackets it when the program enabled
		 * bracketed paste (mode 2004). No newline: Enter is the user's. */
		text = kitty_droppath_text( typed, d->n, 0 ) ;
		if( text ) { kitty_paste_text( d->hwnd, text ) ; free( text ) ; }
		sfree( typed ) ;
	}
	for( i = 0 ; i < d->n ; i++ ) free( d->remote[i] ) ;
	sfree( d->remote ) ; sfree( d->ok ) ; sfree( d ) ;
}
static void kx_send_one( HWND hwnd, char * directory, char * filename, char * distantdir,
                         kx_drop_t *drop, int drop_idx ) ;
/* The dropped files uploaded, one kscp run each; typepath: type their
 * remote paths once the runs are over. */
static void kx_drop_upload( HWND hwnd, char **files, int nfiles, int typepath ) {
	kx_drop_t *d = typepath ? kx_drop_new( hwnd, nfiles ) : NULL ;
	for( int i = 0 ; i < nfiles ; i++ ) kx_send_one( hwnd, "", files[i], NULL, d, i ) ;
}

/* What kitty_helper_ready runs on a Yes: the user action again, which finds
 * its helper allowed this time. */
typedef struct { HWND hwnd ; char *a, *b, *c ; char **files ; int nfiles ; int typepath ; } kx_again_t ;
static kx_again_t *kx_again( HWND hwnd ) {
	kx_again_t *g = snew( kx_again_t ) ;
	memset( g, 0, sizeof(*g) ) ; g->hwnd = hwnd ;
	return g ;
}
static void kx_again_free( void *v ) {
	kx_again_t *g = (kx_again_t *)v ;
	sfree( g->a ) ; sfree( g->b ) ; sfree( g->c ) ;
	for( int i = 0 ; i < g->nfiles ; i++ ) sfree( g->files[i] ) ;
	sfree( g->files ) ; sfree( g ) ;
}
static void kx_again_sendfile( void *v ) { SendFile( ((kx_again_t *)v)->hwnd ) ; }
static void kx_again_getfile( void *v ) { GetFile( ((kx_again_t *)v)->hwnd ) ; }
#if KX_CHECK_SELF_UPDATING_TOOLS
static void kx_again_filezilla( void *v ) { StartFileZilla( ((kx_again_t *)v)->hwnd ) ; }
static void kx_again_winscp( void *v ) {
	kx_again_t *g = (kx_again_t *)v ;
	StartWinSCP( g->hwnd, g->a, g->b, g->c ) ;
}
#endif
static void kx_again_drop( void *v ) {
	kx_again_t *g = (kx_again_t *)v ;
	kx_drop_upload( g->hwnd, g->files, g->nfiles, g->typepath ) ;
}

/* Watch a launched transfer process (kscp/klink) on a background thread: wait
 * for it, and on a NON-zero exit pop a dialog with the exit code + a hint. Runs
 * off the GUI thread so a long transfer never freezes KiTTY, and the visible
 * console (CREATE_NEW_CONSOLE below) still shows live progress. Never shows the
 * command line (it carries -pw). */
/* Transient system-tray balloon (non-modal, auto-dismiss), clickable: the
 * balloon thread creates a hidden message-only window that owns the notify
 * icon, fires the balloon, pumps messages until Windows reports the balloon
 * gone (NIN_BALLOONTIMEOUT / NIN_BALLOONHIDE) or clicked
 * (NIN_BALLOONUSERCLICK), or until the 8-second display is over, then removes
 * the icon and the window - on every exit path, click or not. A click opens
 * `path`: a file is shown selected in Explorer, a folder is opened; NULL =
 * a plain balloon that does nothing on a click (the clipboard command's
 * notice). Used for the file-transfer SUCCESS notice. Runs on its own thread
 * (the pump must NOT sit on the GUI thread). */
#define KTX_WM_BALLOON (WM_APP+13)     /* the notify icon's callback message */
#define KTX_BALLOON_MS 8000
#ifndef NIN_BALLOONSHOW
#define NIN_BALLOONSHOW      (WM_USER + 2)
#endif
#ifndef NIN_BALLOONHIDE
#define NIN_BALLOONHIDE      (WM_USER + 3)
#endif
#ifndef NIN_BALLOONTIMEOUT
#define NIN_BALLOONTIMEOUT   (WM_USER + 4)
#endif
#ifndef NIN_BALLOONUSERCLICK
#define NIN_BALLOONUSERCLICK (WM_USER + 5)
#endif
struct ktx_balloon { char *title ; char *msg ; char *path ; int done ; } ;

/* What a click opens: the file selected in its folder, or the folder itself.
 * A path that is gone by the time of the click opens nothing. */
static void ktx_balloon_open( const char *path ) {
	if( !path || !path[0] ) return ;
	if( existdirectory( path ) ) {
		ShellExecuteA( NULL, "open", path, NULL, NULL, SW_SHOWNORMAL ) ;
	} else if( existfile( path ) ) {
		char *args = dupprintf( "/select,\"%s\"", path ) ;
		ShellExecuteA( NULL, "open", "explorer.exe", args, NULL, SW_SHOWNORMAL ) ;
		sfree( args ) ;
	}
}

static LRESULT CALLBACK ktx_balloon_wndproc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp ) {
	struct ktx_balloon *b = (struct ktx_balloon *)GetWindowLongPtr( hwnd, GWLP_USERDATA ) ;
	if( msg == WM_CREATE ) {
		SetWindowLongPtr( hwnd, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCT *)lp)->lpCreateParams ) ;
		return 0 ;
	}
	if( msg == KTX_WM_BALLOON && b ) {
		/* Version-0 icon: lParam is the notification code, as in window.c.
		 * A left click on the icon during its short life counts as a click. */
		if( lp == NIN_BALLOONUSERCLICK || lp == WM_LBUTTONUP ) {
			ktx_balloon_open( b->path ) ;
			b->done = 1 ;
		} else if( lp == NIN_BALLOONTIMEOUT || lp == NIN_BALLOONHIDE ) {
			b->done = 1 ;
		}
		return 0 ;
	}
	return DefWindowProc( hwnd, msg, wp, lp ) ;
}

static void kitty_tray_balloon( struct ktx_balloon *b ) {
	static volatile LONG s_uid = 0xC000 ;
	static const char *cls = "KiTTYballoonwin" ;
	NOTIFYICONDATA nid ;
	HINSTANCE hi = GetModuleHandle( NULL ) ;
	HWND hwnd ;
	HRESULT hr ;
	{	/* one class for every balloon; a second registration fails with
		 * ERROR_CLASS_ALREADY_EXISTS, which is fine */
		WNDCLASSA wc ; memset( &wc, 0, sizeof(wc) ) ;
		wc.lpfnWndProc = ktx_balloon_wndproc ;
		wc.hInstance = hi ;
		wc.lpszClassName = cls ;
		RegisterClassA( &wc ) ;
	}
	hr = CoInitializeEx( NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE ) ;
	hwnd = CreateWindowExA( 0, cls, "", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, hi, b ) ;
	if( hwnd ) {
		memset( &nid, 0, sizeof(nid) ) ;
		nid.cbSize = sizeof(nid) ;
		nid.hWnd = hwnd ;
		nid.uID = (UINT)InterlockedIncrement( &s_uid ) ;
		nid.uFlags = NIF_ICON | NIF_INFO | NIF_MESSAGE ;
		nid.uCallbackMessage = KTX_WM_BALLOON ;
		nid.hIcon = LoadIcon( NULL, IDI_INFORMATION ) ;
		nid.dwInfoFlags = NIIF_INFO ;
		strncpy( nid.szInfoTitle, b->title, sizeof(nid.szInfoTitle)-1 ) ;
		strncpy( nid.szInfo,      b->msg,   sizeof(nid.szInfo)-1 ) ;
		if( Shell_NotifyIcon( NIM_ADD, &nid ) ) {
			/* Keep the icon while the balloon shows: pump until Windows reports
			 * it gone or clicked, or until the display time is over. */
			DWORD start = GetTickCount() ;
			while( !b->done ) {
				DWORD spent = GetTickCount() - start ;
				MSG m ;
				if( spent >= KTX_BALLOON_MS ) break ;
				MsgWaitForMultipleObjects( 0, NULL, FALSE, KTX_BALLOON_MS - spent, QS_ALLINPUT ) ;
				while( PeekMessage( &m, NULL, 0, 0, PM_REMOVE ) ) {
					TranslateMessage( &m ) ;
					DispatchMessage( &m ) ;
				}
			}
			Shell_NotifyIcon( NIM_DELETE, &nid ) ;
		}
		DestroyWindow( hwnd ) ;
	}
	if( SUCCEEDED( hr ) ) CoUninitialize() ;
}

/* Fire the success tray balloon on its own short-lived thread. */
static DWORD WINAPI ktx_balloon_thread( LPVOID p ) {
	struct ktx_balloon *b = (struct ktx_balloon *)p ;
	kitty_tray_balloon( b ) ;
	sfree( b->title ) ; sfree( b->msg ) ; sfree( b->path ) ; free( b ) ;
	return 0 ;
}
/* path: what a click opens (a file or a folder), NULL = nothing. */
static void kitty_tray_balloon_async( const char *title, const char *msg, const char *path ) {
	struct ktx_balloon *b = (struct ktx_balloon *)malloc( sizeof(*b) ) ;
	if( !b ) return ;
	memset( b, 0, sizeof(*b) ) ;
	b->title = dupstr(title) ; b->msg = dupstr(msg) ;
	b->path = ( path && path[0] ) ? dupstr(path) : NULL ;
	HANDLE t = CreateThread( NULL, 0, ktx_balloon_thread, b, 0, NULL ) ;
	if( t ) CloseHandle( t ) ; else { sfree(b->title) ; sfree(b->msg) ; sfree(b->path) ; free(b) ; }
}

/* ---- KiTTY file-transfer window -----------------------------------------
 * Runs kscp with its output captured to a pipe (no shell -> injection
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
/* KiTTY: a run on the session's shared connection (-share) for which the
 * server refused another channel (OpenSSH MaxSessions, "Server refused to
 * open main channel") is started once more on kscp's own login. The starter
 * leaves here what starting it again takes; kitty_run_xfer hands it to the
 * transfer window, whose end starts it. kx_noshare: the builder
 * (kx_kscp_line) leaves -share out, for that one run. */
#define KX_REFUSED_CHANNEL "Server refused to open"
/* kscp -batch met a prompt it may not show (a one-time code, a password it
 * was not given) - windows/console.c */
#define KX_BATCH_PROMPT    "Cannot answer interactive prompts"
typedef struct {
	int kind ;                     /* 0 none, 1 Send (kx_send_one), 2 Get (GetOneFileStaged) */
	char *dir, *file, *dist, *localdir, *localfile, *final ;
	kx_drop_t *drop ;
	int drop_idx ;
	HANDLE lock ;
} kx_rerun_t ;
static kx_rerun_t kx_rerun_next ;      /* for the next kitty_run_xfer */
static int kx_noshare ;
static void kx_rerun_free( kx_rerun_t *r ) {
	sfree( r->dir ) ; sfree( r->file ) ; sfree( r->dist ) ;
	sfree( r->localdir ) ; sfree( r->localfile ) ; sfree( r->final ) ;
	memset( r, 0, sizeof(*r) ) ;
}

struct ktx_win {
	HWND hwnd, edit, closebtn, parent ;
	HANDLE proc, rd, thread ;
	HFONT font, uifont ;   /* DPI-scaled: monospace output + UI-font button */
	char *what ;
	char *open_file, *open_dir ;   /* what the success balloon opens on a click:
	                                * the file if it exists when done, else the folder */
	int arriving ;                 /* 1 = a Get (files arriving), 0 = a Send */
	int done ;
	int cancelled ;
	/* Wildcard/folder Get File staging (never overwrite silently): kscp writes
	 * into stage_dir; on success each file is moved to final_dir, a name clash
	 * renamed to "name (1)". lock is the held-open marker so a sweep leaves a
	 * live download alone. NULL / INVALID when this transfer does not stage. */
	char *stage_dir, *final_dir ;
	HANDLE lock ;
	int renamed ;                  /* files that had to be renamed on the move */
	int cleanup_failed ;           /* the staging folder could not be removed */
	kx_drop_t *drop ;              /* the drop whose paths are typed, NULL = none */
	int drop_idx ;                 /* this run's file in it */
	kx_rerun_t rerun ;             /* how to start it again without -share */
	/* mini line-discipline so kscp's \r progress meter overwrites the current
	 * line in place (terminal-style) instead of stacking new lines: */
	char *curline ;        /* current uncommitted line (grows as it needs) */
	size_t cursize ;       /* bytes allocated for curline */
	int  curcol ;          /* write cursor within curline (\r resets to 0) */
	int  curlen ;          /* length of curline */
	int  committed ;       /* edit-control char index where curline begins */
} ;

/* Make the edit-control tail (from w->committed to end) equal curline[0..curlen]. */
static void ktx_set_curline( struct ktx_win *w ) {
	char save ;
	sgrowarray( w->curline, w->cursize, (size_t)w->curlen ) ;
	save = w->curline[w->curlen] ; w->curline[w->curlen] = 0 ;
	SendMessageA( w->edit, EM_SETSEL, w->committed, -1 ) ;
	SendMessageA( w->edit, EM_REPLACESEL, FALSE, (LPARAM)w->curline ) ;
	w->curline[w->curlen] = save ;
}

/* Feed captured kscp bytes through a tiny line-discipline: '\r' returns the
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
				sgrowarray( w->curline, w->cursize, (size_t)w->curcol ) ;
				w->curline[w->curcol++] = ' ' ;
				if( w->curcol > w->curlen ) w->curlen = w->curcol ;
			} while( w->curcol % 8 ) ;
		} else if( (unsigned char)c >= 0x20 ) {
			sgrowarray( w->curline, w->cursize, (size_t)w->curcol ) ;
			w->curline[w->curcol++] = c ;
			if( w->curcol > w->curlen ) w->curlen = w->curcol ;
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
 * monospace font for the kscp output - the old ANSI_FIXED_FONT stock font was a
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

/* ---- Get File overwrite protection: never replace a local file silently ----
 *
 * Single named file: a Save-As dialog (Windows' own replace prompt) - see
 * GetFile. Several named files: the caller tests each name and, on "Keep both",
 * calls here for a free "name (1)" path. Wildcard/folder: kscp writes into a
 * VISIBLE staging folder with a held-open .lock; on success each file is moved
 * out, a clash renamed, and the folder removed. Stale staging folders (a crash
 * left one) are swept when their .lock can be taken. */

/* "<dir>\<stem> (n)<ext>" for the first n>=1 whose file does not exist, in
 * full (sfree it). ext is the last '.' of the base name (none -> whole name
 * is the stem). */
static char *kitty_xfer_free_name_dup( const char *dir, const char *base ) {
	const char *dot = strrchr( base, '.' ) ;
	int stemlen = ( dot && dot != base ) ? (int)( dot - base ) : (int)strlen( base ) ;
	const char *ext = ( dot && dot != base ) ? dot : "" ;
	char *out ;
	int n ;
	for( n = 1 ; ; n++ ) {
		out = dupprintf( "%s\\%.*s (%d)%s", dir, stemlen, base, n, ext ) ;
		if( n >= 100000 ) return out ;   /* astronomically unreachable */
		if( !existfile( out ) && !existdirectory( out ) ) return out ;
		sfree( out ) ;
	}
}

/* Create the staging folder inside `dir` ("KiTTY++ download in progress", then
 * "(2)","(3)"...) and hold its .lock open. Returns the folder path in full
 * (sfree it) and the lock handle, or NULL on failure. */
static char *kitty_xfer_make_stage_dup( const char *dir, HANDLE *lock ) {
	char *stage = NULL, *lp ;
	int n ;
	for( n = 1 ; n < 100000 ; n++ ) {
		if( n == 1 ) stage = dupprintf( "%s\\%s", dir, KT_XFER_STAGING_DIR ) ;
		else { char *nm = dupprintf( KT_XFER_STAGING_DIR_N, n ) ; stage = dupprintf( "%s\\%s", dir, nm ) ; sfree( nm ) ; }
		if( CreateDirectoryA( stage, NULL ) ) break ;
		if( GetLastError() != ERROR_ALREADY_EXISTS ) { sfree( stage ) ; return NULL ; }
		sfree( stage ) ; stage = NULL ;
	}
	if( stage == NULL ) return NULL ;
	lp = dupprintf( "%s\\%s", stage, KT_XFER_STAGING_LOCK ) ;
	*lock = CreateFileA( lp, GENERIC_WRITE, 0 /* no share: the sweep's open fails while we hold it */,
	                     NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN|FILE_FLAG_DELETE_ON_CLOSE, NULL ) ;
	sfree( lp ) ;
	if( *lock == INVALID_HANDLE_VALUE ) { RemoveDirectoryA( stage ) ; sfree( stage ) ; return NULL ; }
	return stage ;
}

/* Move everything from w->stage_dir into w->final_dir, renaming a name that is
 * already taken to "name (1)"; count the renames, feed a line per rename; then
 * release the lock and remove the staging folder. Called from KTX_WM_DONE on
 * the window thread AFTER kscp has exited. */
static void kitty_xfer_finish_stage( struct ktx_win *w ) {
	if( !w->stage_dir || !w->final_dir ) return ;
	char *pat = dupprintf( "%s\\*", w->stage_dir ) ;
	WIN32_FIND_DATAA fd ; HANDLE h = FindFirstFileA( pat, &fd ) ;
	sfree( pat ) ;
	if( h != INVALID_HANDLE_VALUE ) {
		do {
			if( !strcmp( fd.cFileName, "." ) || !strcmp( fd.cFileName, ".." ) ) continue ;
			if( !strcmp( fd.cFileName, KT_XFER_STAGING_LOCK ) ) continue ;   /* our marker */
			char *src = dupprintf( "%s\\%s", w->stage_dir, fd.cFileName ) ;
			char *dst = dupprintf( "%s\\%s", w->final_dir, fd.cFileName ) ;
			if( existfile( dst ) || existdirectory( dst ) ) {
				char *freed = kitty_xfer_free_name_dup( w->final_dir, fd.cFileName ) ;
				if( MoveFileExA( src, freed, MOVEFILE_COPY_ALLOWED ) ) {
					const char *fb = strrchr( freed, '\\' ) ; fb = fb ? fb+1 : freed ;
					char *ln = dupprintf( KT_XFER_RENAMED_LINE, fd.cFileName, fb ) ;
					ktx_feed( w, ln, (int)strlen(ln) ) ; sfree( ln ) ;
					w->renamed++ ;
				}
				sfree( freed ) ;
			} else {
				MoveFileExA( src, dst, MOVEFILE_COPY_ALLOWED ) ;
			}
			sfree( src ) ; sfree( dst ) ;
		} while( FindNextFileA( h, &fd ) ) ;
		FindClose( h ) ;
	}
	if( w->lock && w->lock != INVALID_HANDLE_VALUE ) { CloseHandle( w->lock ) ; w->lock = NULL ; }  /* FILE_FLAG_DELETE_ON_CLOSE removes the marker */
	if( !RemoveDirectoryA( w->stage_dir ) ) {
		char *ln = dupprintf( KT_XFER_STAGING_KEEP_LINE, w->stage_dir ) ;
		ktx_feed( w, ln, (int)strlen(ln) ) ; sfree( ln ) ;
		w->cleanup_failed = 1 ;
	}
}

/* Sweep leftover staging folders under `dir`: remove only ours, and only when
 * the .lock opens (a live download holds it, so its open fails and it is left).
 * Called at startup for the download dir + Downloads, and before a wildcard
 * download into a folder. */
static void kitty_xfer_sweep_dir( const char *dir ) {
	if( !dir || !dir[0] || !existdirectory( dir ) ) return ;
	char *pat = dupprintf( "%s\\%s*", dir, KT_XFER_STAGING_DIR ) ;
	WIN32_FIND_DATAA fd ; HANDLE h = FindFirstFileA( pat, &fd ) ;
	sfree( pat ) ;
	if( h == INVALID_HANDLE_VALUE ) return ;
	do {
		if( !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ) continue ;
		char *sd = dupprintf( "%s\\%s", dir, fd.cFileName ) ;
		char *lp = dupprintf( "%s\\%s", sd, KT_XFER_STAGING_LOCK ) ;
		int live = 0 ;
		if( existfile( lp ) ) {
			/* try to take the marker: a running download holds it (no share) */
			HANDLE t = CreateFileA( lp, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_DELETE_ON_CLOSE, NULL ) ;
			if( t == INVALID_HANDLE_VALUE ) live = 1 ;   /* still live: leave it */
			else CloseHandle( t ) ;   /* deletes the marker */
		}
		if( !live ) {
			/* remove the folder tree (any leftover files too); pFrom ends
			 * with two NULs */
			size_t sl = strlen( sd ) ;
			char *op = snewn( sl + 2, char ) ;
			SHFILEOPSTRUCTA fo ; memset( &fo, 0, sizeof(fo) ) ;
			memcpy( op, sd, sl ) ; op[sl] = op[sl+1] = '\0' ;
			fo.wFunc = FO_DELETE ; fo.pFrom = op ;
			fo.fFlags = FOF_NO_UI | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT ;
			SHFileOperationA( &fo ) ;
			sfree( op ) ;
		}
		sfree( sd ) ; sfree( lp ) ;
	} while( FindNextFileA( h, &fd ) ) ;
	FindClose( h ) ;
}

/* Startup sweep: the global download folder and the user's Downloads. */
void kitty_xfer_sweep_downloads( void ) {
	char *dir ;
	/* At startup `conf` is still NULL; kitty_xfer_download_dir_dup() then skips
	 * the session folder and answers the GLOBAL download folder (Transfers &
	 * Tools), else Downloads - which is exactly the startup sweep the design
	 * requires. The folder a download is aimed at is swept again when a
	 * wildcard download starts into it. */
	dir = kitty_xfer_download_dir_dup( conf ) ;
	kitty_xfer_sweep_dir( dir ) ;
	{ const char *dl = getenv( "USERPROFILE" ) ;
	  if( dl && dl[0] ) {
	      char *dd = dupprintf( "%s\\Downloads", dl ) ;
	      if( _stricmp( dd, dir ) != 0 ) kitty_xfer_sweep_dir( dd ) ;
	      sfree( dd ) ; }
	}
	sfree( dir ) ;
}

/* Esc closes (or cancels) the transfer window. Key events go to the focused
 * child control - the read-only edit or the Close button - whose default procs
 * ignore Esc, so we subclass both to forward Esc as a WM_CLOSE to the parent
 * (which then either closes a finished transfer or cancels a running one).
 * The frame itself holds the focus until a child is clicked (a plain window
 * gets no focus hand-off at creation, nor when activated again), so the
 * frame's own procedure handles WM_KEYDOWN the same way and passes the focus
 * to the button whenever it receives it. */
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

/* This run is over for its drop (ok: kscp succeeded); once only. */
/* Does kscp's output contain s? */
static int ktx_said( struct ktx_win *w, const char *s ) {
	int n = GetWindowTextLengthA( w->edit ), hit ;
	char *t = snewn( n + 1, char ) ;
	t[0] = '\0' ;
	GetWindowTextA( w->edit, t, n + 1 ) ;
	hit = strstr( t, s ) != NULL ;
	sfree( t ) ;
	return hit ;
}

static void ktx_drop_done( struct ktx_win *w, int ok ) {
	kx_drop_t *d = w->drop ;
	w->drop = NULL ;
	kx_drop_settle( d, w->drop_idx, ok ) ;
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
		w->closebtn = CreateWindow( "BUTTON", KT_XFER_BTN_CANCEL,
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
		const char *what = w->what ? w->what : KT_XFER_TRANSFER ;
		if( code == 0 && !w->cancelled ) {
			/* Wildcard/folder: move out of the staging folder now (kscp has
			 * exited), renaming any clash to "name (1)" and counting it. */
			kitty_xfer_finish_stage( w ) ;
			/* KiTTY: a drop upload that types its remote paths: this file
			 * landed; the paths are typed when the drop's last run is over. */
			ktx_drop_done( w, 1 ) ;
			/* [KiTTY] transfernotification: a Get File that produced the one
			 * file opens that file, anything else opens its folder */
			if( w->open_file && existfile( w->open_file ) )
				kitty_xfer_notify_ex( what, 1, 1, w->open_file, w->renamed ) ;
			else
				kitty_xfer_notify_ex( what, w->arriving, 0, w->open_dir, w->renamed ) ;
			if( w->renamed > 0 ) {          /* report it in the window, which then stays open */
				char *t = dupprintf( "\r\n" KT_XFER_RENAMED_NOTE "\r\n", w->renamed ) ;
				ktx_feed( w, t, (int)strlen(t) ) ; sfree( t ) ;
			}
			/* Auto-close ONLY when everything went as planned: keep it open
			 * (Close focused) on the keep-window setting, on ANY rename, and
			 * when the staging folder could not be removed. */
			if( ( conf && conf_get_bool( conf, CONF_pscp_keep_window ) )
			    || w->renamed > 0 || w->cleanup_failed ) {
				char *t = dupprintf( KT_XFER_COMPLETE_LINE, what ) ;
				ktx_feed( w, t, (int)strlen(t) ) ; sfree( t ) ;
				SetWindowTextA( w->closebtn, KT_XFER_BTN_CLOSE ) ;
				EnableWindow( w->closebtn, TRUE ) ;
				SetForegroundWindow( hwnd ) ; SetFocus( w->closebtn ) ;
			} else {
				DestroyWindow( hwnd ) ;   /* default: auto-close, balloon confirms */
			}
		} else {
			char *m ;
			if( !w->cancelled && w->rerun.kind && ktx_said( w, KX_REFUSED_CHANNEL ) ) {
				/* The shared connection would not carry this run: once more
				 * on kscp's own login, in a window of its own that shows why.
				 * That run takes over the drop and the staging folder. */
				kx_rerun_t r = w->rerun ;
				memset( &w->rerun, 0, sizeof(w->rerun) ) ;
				w->drop = NULL ;
				if( r.kind == 2 ) {
					w->lock = NULL ;
					sfree( w->stage_dir ) ; w->stage_dir = NULL ;
				}
				kx_noshare = 1 ;
				if( r.kind == 1 ) kx_send_one( w->parent, r.dir, r.file, r.dist, r.drop, r.drop_idx ) ;
				else GetOneFileStaged( w->parent, r.dir, r.file, r.localdir, r.localfile, r.final, r.lock ) ;
				kx_noshare = 0 ;
				kx_rerun_free( &r ) ;
				DestroyWindow( hwnd ) ;
				return 0 ;
			}
			ktx_drop_done( w, 0 ) ;   /* a failed upload types nothing */
			/* Cancel or failure: drop the staging folder too (design: removed
			 * after success, cancel or failure), without moving anything out. */
			if( w->stage_dir ) {
				if( w->lock && w->lock != INVALID_HANDLE_VALUE ) { CloseHandle( w->lock ) ; w->lock = NULL ; }
				kitty_xfer_sweep_dir( w->final_dir ? w->final_dir : w->stage_dir ) ;
			}
			if( w->cancelled ) {
				m = dupprintf( KT_XFER_CANCELLED_LINE, what ) ;
				SetWindowTextA( hwnd, KT_CAP_XFER_CANCELLED ) ;
			} else {
				const char *hint = ( code==127 )
					? KT_XFER_HINT_127
					: ktx_said( w, KX_BATCH_PROMPT ) ? KT_XFER_HINT_PROMPT
					: "" ;
				m = dupprintf( KT_XFER_FAILED_LINE,
				               what, (unsigned long)code, hint ) ;
				char *t = dupprintf( KT_XFER_FAILED_TITLE, (unsigned long)code ) ;
				SetWindowTextA( hwnd, t ) ; sfree( t ) ;
			}
			ktx_feed( w, m, (int)strlen(m) ) ; sfree( m ) ;
			SetWindowTextA( w->closebtn, KT_XFER_BTN_CLOSE ) ;
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
				w->cancelled = 1 ;               /* running -> button is "Cancel": kill kscp */
				if( w->proc ) TerminateProcess( w->proc, 2 ) ;
				SetWindowTextA( w->closebtn, KT_XFER_BTN_STOPPING ) ;
				EnableWindow( w->closebtn, FALSE ) ;
			}
			return 0 ;
		}
		break ;
	  case WM_SETFOCUS:                          /* the frame never keeps the focus */
		if( w && w->closebtn ) SetFocus( w->closebtn ) ;
		return 0 ;
	  case WM_KEYDOWN:                           /* Esc on the frame = Close / Cancel */
		if( wp == VK_ESCAPE ) { SendMessage( hwnd, WM_CLOSE, 0, 0 ) ; return 0 ; }
		break ;
	  case WM_CLOSE:
		if( w && !w->done ) {                     /* X mid-transfer: cancel, wait for DONE */
			if( !w->cancelled ) {
				w->cancelled = 1 ;
				if( w->proc ) TerminateProcess( w->proc, 2 ) ;
				SetWindowTextA( w->closebtn, KT_XFER_BTN_STOPPING ) ;
				EnableWindow( w->closebtn, FALSE ) ;
			}
			return 0 ;
		}
		DestroyWindow( hwnd ) ;
		return 0 ;
	  case WM_DESTROY:
		if( w ) {
			ktx_drop_done( w, 0 ) ;   /* gone before kscp was done */
			if( w->font ) DeleteObject( w->font ) ;
			if( w->uifont ) DeleteObject( w->uifont ) ;
			if( w->proc ) CloseHandle( w->proc ) ;
			if( w->thread ) CloseHandle( w->thread ) ;
			if( w->what ) sfree( w->what ) ;
			sfree( w->open_file ) ; sfree( w->open_dir ) ;
			if( w->lock && w->lock != INVALID_HANDLE_VALUE ) CloseHandle( w->lock ) ;
			sfree( w->stage_dir ) ; sfree( w->final_dir ) ;
			sfree( w->curline ) ;
			kx_rerun_free( &w->rerun ) ;
			free( w ) ;
			SetWindowLongPtr( hwnd, GWLP_USERDATA, 0 ) ;
		}
		return 0 ;
	}
	return DefWindowProc( hwnd, msg, wp, lp ) ;
}

/* Launch kscp into a transfer window (above). No shell. Returns 0 if launched.
 * arriving: 1 = a Get (files arriving), 0 = a Send. open_file / open_dir:
 * what the success balloon opens on a click - the file when it exists once
 * kscp is done (a Get File of one file), else the folder (the download
 * folder for a Get, the upload folder for a Send). drop / drop_idx: the drop
 * upload this run belongs to, settled when it is over - left to the caller
 * when the run does not start (-1). */
static int kitty_run_xfer( HWND parent, char *cmdline, const char *what, const char *intro,
                           int arriving, const char *open_file, const char *open_dir,
                           const char *stage_dir, const char *final_dir, HANDLE lock,
                           kx_drop_t *drop, int drop_idx, HANDLE pwmap ) {
	static int registered = 0 ;
	HINSTANCE hi = GetModuleHandle( NULL ) ;
	if( !registered ) {
		WNDCLASS wc ; memset( &wc, 0, sizeof(wc) ) ;
		wc.lpfnWndProc = ktx_wndproc ;
		wc.hInstance = hi ;
		wc.hCursor = LoadCursor( NULL, IDC_ARROW ) ;
		wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE+1) ;
		/* The window takes the session's icon after creation
		 * (kitty_dialog_icon); this is only what the class starts with. */
		wc.hIcon = LoadIcon( hi, MAKEINTRESOURCE(IDI_MAINICON) ) ;
		wc.lpszClassName = "KiTTYxferwin" ;
		RegisterClass( &wc ) ;
		registered = 1 ;
	}
	SECURITY_ATTRIBUTES sa ; HANDLE rd=NULL, wr=NULL ;
	memset( &sa, 0, sizeof(sa) ) ; sa.nLength = sizeof(sa) ; sa.bInheritHandle = TRUE ;
	if( !CreatePipe( &rd, &wr, &sa, 0 ) ) { kx_rerun_free( &kx_rerun_next ) ; return -1 ; }
	SetHandleInformation( rd, HANDLE_FLAG_INHERIT, 0 ) ;
	STARTUPINFOA si ; PROCESS_INFORMATION pi ;
	memset( &si, 0, sizeof(si) ) ; si.cb = sizeof(si) ;
	si.dwFlags = STARTF_USESTDHANDLES ;
	si.hStdOutput = wr ; si.hStdError = wr ; si.hStdInput = GetStdHandle( STD_INPUT_HANDLE ) ;
	memset( &pi, 0, sizeof(pi) ) ;
	/* kscp is PSCPPath: started only through the signature check. With its
	 * password in a mapping (-pwhandle), it inherits that and its output pipe
	 * and nothing else; the caller closes the mapping after the start. */
	if( pwmap ? !kitty_helper_start_handles( PSCPPath, cmdline, CREATE_NO_WINDOW, NULL, &si, &pi,
	                                         (HANDLE[]){ wr, pwmap }, 2 )
	          : !kitty_helper_start( PSCPPath, cmdline, TRUE, CREATE_NO_WINDOW, NULL, &si, &pi ) ) {
		CloseHandle( rd ) ; CloseHandle( wr ) ;
		kx_rerun_free( &kx_rerun_next ) ;
		kx_report( KT_CAP_XFER, KT_XFER_LAUNCH_FAILED ) ;
		return -1 ;
	}
	CloseHandle( wr ) ; CloseHandle( pi.hThread ) ;
	struct ktx_win *w = (struct ktx_win *)malloc( sizeof(*w) ) ;
	if( !w ) { CloseHandle( pi.hProcess ) ; CloseHandle( rd ) ; kx_rerun_free( &kx_rerun_next ) ; return -1 ; }
	memset( w, 0, sizeof(*w) ) ;
	w->rerun = kx_rerun_next ;              /* the window owns it now */
	memset( &kx_rerun_next, 0, sizeof(kx_rerun_next) ) ;
	w->parent = parent ; w->proc = pi.hProcess ; w->rd = rd ;
	w->what = dupstr( what ? what : KT_XFER_TRANSFER ) ;
	w->open_file = ( open_file && open_file[0] ) ? dupstr( open_file ) : NULL ;
	w->open_dir = ( open_dir && open_dir[0] ) ? dupstr( open_dir ) : NULL ;
	w->stage_dir = ( stage_dir && stage_dir[0] ) ? dupstr( stage_dir ) : NULL ;
	w->final_dir = ( final_dir && final_dir[0] ) ? dupstr( final_dir ) : NULL ;
	w->lock = lock ;
	w->arriving = arriving ;
	char *title = dupprintf( KT_XFER_WINDOW_TITLE, w->what ) ;
	int dpi0 = 96 ;
	{ HDC pdc = GetDC( parent ) ; if( pdc ) { dpi0 = GetDeviceCaps( pdc, LOGPIXELSX ) ; ReleaseDC( parent, pdc ) ; } }
	HWND hwnd = CreateWindow( "KiTTYxferwin", title,
		WS_OVERLAPPEDWINDOW|WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
		MulDiv(680,dpi0,96), MulDiv(420,dpi0,96),
		parent, NULL, hi, w ) ;
	sfree( title ) ;
	if( !hwnd ) { CloseHandle( pi.hProcess ) ; CloseHandle( rd ) ; sfree( w->what ) ;
	              sfree( w->open_file ) ; sfree( w->open_dir ) ; free( w ) ; return -1 ; }
	/* Taken only now that the window exists; kscp's end is posted to it. */
	w->drop = drop ; w->drop_idx = drop_idx ;
	/* The session's own icon, big and small, as every other window of ours
	 * takes it - the class was registered with IDI_APPLICATION, which is the
	 * generic Windows icon. */
	kitty_dialog_icon( hwnd, parent ) ;
	SetForegroundWindow( hwnd ) ;          /* bring the transfer window to the front */
	BringWindowToTop( hwnd ) ;
	SetFocus( w->closebtn ) ;              /* Esc reaches the button, not the bare frame */
	{ /* a re-run without -share shows why first */
	  char *again = kx_noshare ? dupprintf( "%s\r\n\r\n%s", KT_XFER_SHARE_REFUSED, intro ? intro : "" ) : NULL ;
	  const char *top = again ? again : intro ;
	  if( top && *top ) {            /* show the target (user@host:dir) up top */
		int n = GetWindowTextLength( w->edit ) ;
		SendMessageA( w->edit, EM_SETSEL, n, n ) ;
		SendMessageA( w->edit, EM_REPLACESEL, FALSE, (LPARAM)top ) ;
		w->committed = GetWindowTextLength( w->edit ) ;
	  }
	  sfree( again ) ; }
	w->thread = CreateThread( NULL, 0, ktx_reader_thread, w, 0, NULL ) ;
	return 0 ;
}

/* Append to a command line held in a strbuf: it grows as it needs, so
 * nothing is cut. A line longer than Windows takes (32767 characters) makes
 * the start fail, and the starter reports that as before. Replaces the
 * bounded appends into fixed buffers, which cut the line - the last argument
 * first - without a word. */
static void bcat( strbuf *dst, const char *s ) {
	if( s ) put_dataz( dst, s ) ;
}

/* Append ONE argument, wrapped in double-quotes and escaped per Windows
 * CommandLineToArgvW rules, so a value containing a quote or space cannot inject
 * extra command-line arguments/switches. Use for single-value fields (password,
 * key/file path, source path, the user@host:dir target); do NOT use for fields
 * that are intentionally raw option strings (pscpoptions etc.). */
static void qcat( strbuf *dst, const char *s ) {
	put_byte( dst, '"' ) ;
	if( s ) {
		size_t nbs = 0 ;   /* run of pending backslashes */
		for( const char *p = s ; *p ; p++ ) {
			if( *p == '\\' ) { nbs++ ; }
			else if( *p == '"' ) {
				for( size_t k=0 ; k<2*nbs+1 ; k++ ) put_byte( dst, '\\' ) ;
				put_byte( dst, '"' ) ; nbs = 0 ;
			} else {
				for( size_t k=0 ; k<nbs ; k++ ) put_byte( dst, '\\' ) ;
				nbs = 0 ; put_byte( dst, *p ) ;
			}
		}
		for( size_t k=0 ; k<2*nbs ; k++ ) put_byte( dst, '\\' ) ;   /* trailing backslashes before closing quote */
	}
	put_byte( dst, '"' ) ;
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
static void rawcat( strbuf *dst, const char *s ) {
	put_byte( dst, '"' ) ;
	if( s ) for( const char *p = s ; *p ; p++ ) {
		if( *p == '"' ) put_dataz( dst, "\"\"" ) ;
		else put_byte( dst, *p ) ;
	}
	put_byte( dst, '"' ) ;
}

/* #535: percent-encode and append, for the userinfo (user/password) of a
 * WinSCP "proto://user:pass@host:port/dir" URL. Without this, an '@' '/' ':' or
 * '?' in the password or username is parsed as URL grammar by WinSCP -- e.g. a
 * password "x@evil.host/" redirects the connection to an attacker-chosen host.
 * Everything outside the RFC 3986 "unreserved" set is escaped as %XX. Bounded
 * the same way. (qcat is Win32 argv quoting -- a different layer -- so it does NOT
 * cover this URL-grammar injection.) */
static void urlcat( strbuf *dst, const char *s ) {
	static const char hexd[] = "0123456789ABCDEF" ;
	if( !s ) return ;
	for( const unsigned char *p = (const unsigned char *)s ; *p ; p++ ) {
		unsigned char c = *p ;
		if( (c>='A'&&c<='Z') || (c>='a'&&c<='z') || (c>='0'&&c<='9')
		    || c=='-' || c=='.' || c=='_' || c=='~' ) {
			put_byte( dst, (char)c ) ;
		} else {
			put_byte( dst, '%' ) ; put_byte( dst, hexd[(c>>4)&0xF] ) ; put_byte( dst, hexd[c&0xF] ) ;
		}
	}
}

/* Append a folder for the PATH part of a WinSCP URL
 * (proto://user@host:port/path/) for WinSCP. '/' stays as it is - urlcat would make it
 * %2F, one segment. A space is %20, so the URL stays one argument on the
 * command line, a '%' is %25. Every ASCII byte but letters, digits and
 * / . _ - ~ is %-encoded, so no folder (kitty_osc7_parse.h takes every
 * printable byte) breaks the URL. Bytes from 0x80 on (UTF-8) go as they are, as
 * they always did. */
static void pathcat( strbuf *dst, const char *s ) {
	static const char hexd[] = "0123456789ABCDEF" ;
	if( !s ) return ;
	for( const unsigned char *p = (const unsigned char *)s ; *p ; p++ ) {
		unsigned char c = *p ;
		if( c >= 0x80 || (c>='A'&&c<='Z') || (c>='a'&&c<='z') || (c>='0'&&c<='9')
		    || c=='/' || c=='-' || c=='.' || c=='_' || c=='~' ) {
			put_byte( dst, (char)c ) ;
		} else {
			put_byte( dst, '%' ) ; put_byte( dst, hexd[(c>>4)&0xF] ) ; put_byte( dst, hexd[c&0xF] ) ;
		}
	}
}

/* ---- passwords for WinSCP travel in FILES, never on its command line -------
 *
 * A child's command line is readable by every process of the same user for as
 * long as the child runs (Task Manager, WMI, ToolHelp), and it lands in
 * process-audit logs. WinSCP's /passwordsfromfiles switch makes it read the
 * value of /password, /passphrase "and in general all passwords from all
 * sources" as the PATH of a file holding the real password - so the session
 * password in the URL and the proxy/tunnel password in /rawsettings become
 * paths to files this process writes just before the start and deletes again
 * after WinSCP has had time to read them (a minute; a stale file from a crash
 * is swept at the next start). The files live in %TEMP%, which Windows keeps
 * private to the user - the same protection our own stores have. */
#define KX_PWFILES_MAX 4
/* The paths in full: a %TEMP% Windows cannot give whole (GetTempPath's
 * answer longer than its buffer) is no folder for a password file - the file
 * is then not made and the caller's fallback applies. */
struct kx_pwfiles { int n ; char *path[KX_PWFILES_MAX] ; } ;
static struct kx_pwfiles kx_pw = { 0 } ;

static void kx_pwfiles_sweep( void ) {
	char *tmp, *pat, *full ;
	WIN32_FIND_DATA fd ; HANDLE h ;
	if( ( tmp = kitty_temp_path_dup() ) == NULL ) return ;
	pat = dupprintf( "%skitty_pw_*.tmp", tmp ) ;
	h = FindFirstFile( pat, &fd ) ;
	sfree( pat ) ;
	if( h == INVALID_HANDLE_VALUE ) { sfree( tmp ) ; return ; }
	do {
		full = dupprintf( "%s%s", tmp, fd.cFileName ) ;
		SetFileAttributes( full, FILE_ATTRIBUTE_NORMAL ) ;
		DeleteFile( full ) ;
		sfree( full ) ;
	} while( FindNextFile( h, &fd ) ) ;
	FindClose( h ) ;
	sfree( tmp ) ;
}

/* How the password reached the helper, for the line the transfer window shows.
 * Set from the SAME variables the builder decided on - never worked out a
 * second time - so what the window shows is what the code did. */
#define KX_PW_NONE        0   /* key or agent login: nothing to hand over */
#define KX_PW_FILE_ENC    1
#define KX_PW_FILE_PLAIN  2
#define KX_PW_CMD_ENC     3
#define KX_PW_CMD_PLAIN   4
#define KX_PW_HANDLE      5   /* in memory handed to the helper alone (-pwhandle) */

static const char *kx_pw_handover_line( int mode ) {
	switch( mode ) {
		case KX_PW_FILE_ENC:   return KT_XFER_PW_FILE_ENCRYPTED ;
		case KX_PW_FILE_PLAIN: return KT_XFER_PW_FILE_PLAIN ;
		case KX_PW_CMD_ENC:    return KT_XFER_PW_CMDLINE_ENCRYPTED ;
		case KX_PW_CMD_PLAIN:  return KT_XFER_PW_CMDLINE_PLAIN ;
		case KX_PW_HANDLE:     return KT_XFER_PW_HANDLE ;
		default:               return NULL ;
	}
}

/* Can the helper at `path` read a PROTECTED password file, or only a plain one?
 *
 * The password file replaces "-pw <password>" on the command line, where every
 * process lister and audit log on the machine could read it. What may go IN the
 * file depends entirely on the helper: a password file is read VERBATIM by every
 * RELEASED helper - ours through 0.85.1.8-beta and stock pscp/psftp alike, where
 * cmdline.c is `chomp(fgetline(fp))` and nothing more - so handing one of those
 * the protected form makes the marker and the base64 the password, and the login
 * fails with "Access denied". That is not hypothetical; it is what happened.
 *
 * A NAME test cannot answer this, and that was the bug: "kscp.exe" shows nothing
 * about which kscp it is, and `PSCPPath` commonly resolves to an INSTALLED KiTTY
 * rather than the build the running window came from. So the helper is queried
 * about ITSELF, through its own version resource:
 *
 *   - "KiTTYPasswordFile" >= 1 in its StringFileInfo (windows/version.rc2) - the
 *     capability flag every binary built from this change onward carries; or
 *   - a numeric file version >= 0.85.1.9, so a future binary that somehow lost
 *     the string still qualifies.
 *
 * Anything else - a missing field, an unreadable resource, any error at all - is
 * "plain only". Every released helper and every stock PuTTY lands there, which
 * is the safe answer: the plain file still keeps the password off the command
 * line, and the file is private to this user and deleted after the start.
 *
 * The flag is in the BINARY, so an installed newer helper is recognised whatever
 * file name it has been given. */
#define KX_HELPER_PROTECTED_MS  ((0UL << 16) | 85UL)        /* 0.85 */
#define KX_HELPER_PROTECTED_LS  ((1UL << 16) |  9UL)        /* .1.9 */

static int kx_helper_reads_protected( const char *path ) {
	DWORD dummy, sz ;
	void *buf ;
	int ok = 0 ;

	if( path == NULL || path[0] == '\0' ) return 0 ;
	sz = GetFileVersionInfoSizeA( path, &dummy ) ;
	if( sz == 0 ) return 0 ;
	if( (buf = malloc( sz )) == NULL ) return 0 ;
	if( GetFileVersionInfoA( path, 0, sz, buf ) ) {
		/* The capability flag, in whichever translation the file declares -
		 * ours is 0x0809/1200, but a rebuild elsewhere need not be. */
		struct { WORD lang, cp ; } *xlat = NULL ;
		UINT n = 0 ;
		if( VerQueryValueA( buf, "\\VarFileInfo\\Translation", (void **)&xlat, &n )
		    && xlat != NULL ) {
			UINT i ;
			for( i = 0 ; i < n / sizeof(*xlat) && !ok ; i++ ) {
				char sub[64] ; char *val = NULL ; UINT vlen = 0 ;
				snprintf( sub, sizeof(sub),
				          "\\StringFileInfo\\%04x%04x\\KiTTYPasswordFile",
				          xlat[i].lang, xlat[i].cp ) ;
				if( VerQueryValueA( buf, sub, (void **)&val, &vlen )
				    && val != NULL && vlen > 0 && atoi( val ) >= 1 )
					ok = 1 ;
			}
		}
		if( !ok ) {
			/* No flag: a build new enough by its NUMBER still qualifies. */
			VS_FIXEDFILEINFO *ffi = NULL ; UINT flen = 0 ;
			if( VerQueryValueA( buf, "\\", (void **)&ffi, &flen )
			    && ffi != NULL && flen >= sizeof(*ffi) ) {
				if( ffi->dwFileVersionMS > KX_HELPER_PROTECTED_MS
				    || ( ffi->dwFileVersionMS == KX_HELPER_PROTECTED_MS
				         && ffi->dwFileVersionLS >= KX_HELPER_PROTECTED_LS ) )
					ok = 1 ;
			}
		}
	}
	free( buf ) ;
	return ok ;
}

/* Write `secret` to a fresh private temp file; the path (static storage,
 * valid until the release) or NULL when no file could be made - the caller
 * then falls back to the plain value, so a transfer never fails for this. */
static const char *kx_password_file( const char *secret ) {
	char *tmp ; HANDLE h = INVALID_HANDLE_VALUE ; DWORD written ; int i ;
	static unsigned serial = 0 ;
	if( kx_pw.n >= KX_PWFILES_MAX ) return NULL ;
	if( ( tmp = kitty_temp_path_dup() ) == NULL ) return NULL ;
	for( i = 0 ; i < 16 ; i++ ) {
		char *p = dupprintf( "%skitty_pw_%08lx_%u.tmp", tmp,
		                     (unsigned long)GetTickCount() ^ (unsigned long)GetCurrentProcessId(), ++serial ) ;
		h = CreateFile( p, GENERIC_WRITE, 0, NULL, CREATE_NEW,
		                FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_TEMPORARY, NULL ) ;
		if( h != INVALID_HANDLE_VALUE ) { kx_pw.path[kx_pw.n] = p ; break ; }
		sfree( p ) ;
	}
	sfree( tmp ) ;
	if( h == INVALID_HANDLE_VALUE ) return NULL ;
	if( !WriteFile( h, secret, (DWORD)strlen(secret), &written, NULL ) || written != strlen(secret) ) {
		CloseHandle( h ) ; DeleteFile( kx_pw.path[kx_pw.n] ) ;
		sfree( kx_pw.path[kx_pw.n] ) ; kx_pw.path[kx_pw.n] = NULL ;
		return NULL ;
	}
	CloseHandle( h ) ;
	return kx_pw.path[kx_pw.n++] ;
}

static void kx_pwfiles_delete( struct kx_pwfiles *f ) {
	int i ;
	for( i = 0 ; i < f->n ; i++ ) {
		SetFileAttributes( f->path[i], FILE_ATTRIBUTE_NORMAL ) ; DeleteFile( f->path[i] ) ;
		sfree( f->path[i] ) ; f->path[i] = NULL ;
	}
	f->n = 0 ;
}

static DWORD WINAPI kx_pwfiles_delete_thread( LPVOID p ) {
	struct kx_pwfiles *f = (struct kx_pwfiles *)p ;
	Sleep( 60000 ) ;
	kx_pwfiles_delete( f ) ;
	free( f ) ;
	return 0 ;
}

/* After the start: delete the files in a minute (WinSCP reads them at
 * startup). `now` deletes at once - the start was cancelled. The delayed
 * delete owns the paths from here on. */
static void kx_pwfiles_release( int now ) {
	struct kx_pwfiles *copy ; HANDLE t ;
	if( kx_pw.n == 0 ) return ;
	if( now ) {
		kx_pwfiles_delete( &kx_pw ) ;
	} else if( (copy = (struct kx_pwfiles *)malloc( sizeof(*copy) )) != NULL ) {
		*copy = kx_pw ;
		memset( &kx_pw, 0, sizeof(kx_pw) ) ;
		t = CreateThread( NULL, 0, kx_pwfiles_delete_thread, copy, 0, NULL ) ;
		if( t ) CloseHandle( t ) ; else { kx_pwfiles_delete_thread( copy ) ; }
	} else {
		kx_pwfiles_delete( &kx_pw ) ;
	}
	memset( &kx_pw, 0, sizeof(kx_pw) ) ;
}

/* The proxy of the CONNECTION, for a kscp the terminal starts. Send File and
 * Get File used to hand kscp the host, the port and the login and nothing
 * about the way there, so behind an SSH jump host, a SOCKS/HTTP proxy, a
 * named proxy or workplace proxy mode the transfer dialled the host directly
 * and failed wherever only the proxy reaches it. The WinSCP hand-off has
 * taken the connection's proxy since it existed; this is the same snapshot
 * (kitty_proxy_connection(): named and workplace proxies amend a throwaway
 * Conf the session never sees) written as kscp's -proxy* options.
 *
 * Only for a helper that KNOWS the options: an older kscp - and stock pscp -
 * rejects an unknown option outright, so the capability is read off the
 * binary itself (KiTTYProxyOptions in its version resource, like the
 * password-file flag), and an older helper gets the line it always got. The
 * proxy password travels in a private file (-proxypwfile), as the session's
 * does; a helper that reads the protected form gets it protected.
 * Appends to `buffer`; the password's span is noted for the redacted log. */
static int kx_helper_has_flag( const char *path, const char *flag ) ;
static void kx_proxy_args( strbuf *buffer, int *pwfiles,
                           size_t *px_at, size_t *px_len ) {
	const struct kitty_proxy_snapshot *px = kitty_proxy_connection() ;
	int          px_type = px ? px->type           : conf_get_int(conf,CONF_proxy_type) ;
	int          px_port = px ? px->port           : conf_get_int(conf,CONF_proxy_port) ;
	const char * px_host = px ? px->host           : conf_get_str(conf,CONF_proxy_host) ;
	const char * px_user = px ? px->username       : conf_get_str(conf,CONF_proxy_username) ;
	const char * px_tcmd = px ? px->telnet_command : conf_get_str(conf,CONF_proxy_telnet_command) ;
	const char * tname = NULL ;
	char px_pass[KITTY_PW_MAX+1] ;

	*px_at = *px_len = 0 ;
	if( px_type == PROXY_NONE ) return ;
	/* an explicit -sftpconnect sets its own route to the host */
	if( strlen( conf_get_str(conf, CONF_sftpconnect) ) > 0 ) return ;
	if( !kx_helper_has_flag( PSCPPath, "KiTTYProxyOptions" ) ) return ;

	switch( px_type ) {
		case PROXY_SOCKS4:        tname = "socks4" ; break ;
		case PROXY_SOCKS5:        tname = "socks5" ; break ;
		case PROXY_HTTP:          tname = "http" ; break ;
		case PROXY_TELNET:        tname = "telnet" ; break ;
		case PROXY_CMD:           tname = "cmd" ; break ;
		case PROXY_SSH_TCPIP:     tname = "ssh" ; break ;
		case PROXY_SSH_EXEC:      tname = "sshexec" ; break ;
		case PROXY_SSH_SUBSYSTEM: tname = "sshsubsys" ; break ;
		default: return ;
	}
	bcat( buffer, "-proxytype " ) ; bcat( buffer, tname ) ; bcat( buffer, " " ) ;
	if( px_type == PROXY_CMD ) {
		/* the local command IS the proxy; -proxycmd carries it (and sets the type) */
		if( px_tcmd && px_tcmd[0] ) { bcat( buffer, "-proxycmd " ) ; qcat( buffer, px_tcmd ) ; bcat( buffer, " " ) ; }
	} else {
		if( px_host && px_host[0] ) { bcat( buffer, "-proxyhost " ) ; qcat( buffer, px_host ) ; bcat( buffer, " " ) ; }
		if( px_port > 0 ) put_fmt( buffer, "-proxyport %d ", px_port ) ;
	}
	if( conf_get_bool(conf, CONF_even_proxy_localhost) ) bcat( buffer, "-proxylocalhost " ) ;
	if( px_user && px_user[0] ) { bcat( buffer, "-proxyuser " ) ; qcat( buffer, px_user ) ; bcat( buffer, " " ) ; }

	/* Both sources hold the password WRAPPED (kitty_pwmem.c); unwrapped once
	 * here, the buffer burned before this returns. Into a private file, the
	 * protected form when the helper reads it - exactly as the session's
	 * password goes (see the -pwfile block in kx_kscp_line). No file at all:
	 * the option is left out rather than put a password on the command line;
	 * the jump host then prompts for it, which a transfer window can show. */
	kitty_pw_unwrap_str( px ? px->password : conf_get_str(conf,CONF_proxy_password),
	                     px_pass, sizeof(px_pass) ) ;
	if( px_pass[0] ) {
		char *line = NULL ; const char *pf ;
		if( kx_helper_reads_protected( PSCPPath ) ) line = kitty_pwfile_line( px_pass ) ;
		pf = kx_password_file( line ? line : px_pass ) ;
		if( pf ) {
			bcat( buffer, "-proxypwfile " ) ;
			*px_at = buffer->len ;
			qcat( buffer, pf ) ;
			*px_len = buffer->len - *px_at ;
			bcat( buffer, " " ) ;
			(*pwfiles)++ ;
		}
		if( line ) { smemclr( line, strlen(line) ) ; sfree( line ) ; }
	}
	smemclr( px_pass, sizeof(px_pass) ) ;
}

/* A capability flag in the helper's own version resource, in whichever
 * translation the file declares; 0 on any doubt. kx_helper_reads_protected
 * above is the same test for the password-file flag, kept as it is. */
static int kx_helper_has_flag( const char *path, const char *flag ) {
	DWORD dummy, sz ; void *buf ; int ok = 0 ;
	if( path == NULL || path[0] == '\0' ) return 0 ;
	sz = GetFileVersionInfoSizeA( path, &dummy ) ;
	if( sz == 0 ) return 0 ;
	if( (buf = malloc( sz )) == NULL ) return 0 ;
	if( GetFileVersionInfoA( path, 0, sz, buf ) ) {
		struct { WORD lang, cp ; } *xlat = NULL ; UINT n = 0 ;
		if( VerQueryValueA( buf, "\\VarFileInfo\\Translation", (void **)&xlat, &n ) && xlat != NULL ) {
			UINT i ;
			for( i = 0 ; i < n / sizeof(*xlat) && !ok ; i++ ) {
				char sub[96] ; char *val = NULL ; UINT vlen = 0 ;
				snprintf( sub, sizeof(sub), "\\StringFileInfo\\%04x%04x\\%s", xlat[i].lang, xlat[i].cp, flag ) ;
				if( VerQueryValueA( buf, sub, (void **)&val, &vlen ) && val != NULL && vlen > 0 && atoi( val ) >= 1 )
					ok = 1 ;
			}
		}
	}
	free( buf ) ;
	return ok ;
}

/*
 * KiTTY: the kscp command line both directions build - Send File (SendOneFile)
 * and Get File (GetOneFileStaged) - up to the two paths: the helper, the
 * user's raw options, the protocol, the port, the login (password, port
 * knocking, key file) and the connection's proxy. One builder, so the two
 * directions cannot drift apart. Each caller appends its source and target.
 */
typedef struct {
	int pwfiles ;              /* password files to delete once kscp has them */
	HANDLE pwmap ;             /* the password's mapping (-pwhandle), closed after the start */
	int pw_mode ;              /* how the password travelled (kx_pw_handover_line) */
	size_t pw_at, pw_len ;     /* where the password landed, for the redacted log */
	size_t px_at, px_len ;     /* ... and the proxy password file's path */
	int shared ;               /* -share went on the line */
} kx_kscp_line_t ;

/* GetShortPathName in full (sfree it); NULL when Windows cannot give it. */
static char *kx_short_path_dup( const char *path ) {
	DWORD n = GetShortPathNameA( path, NULL, 0 ), got ;
	int tries ;
	for( tries = 0 ; n > 0 && tries < 4 ; tries++ ) {
		char *s = snewn( n, char ) ;
		got = GetShortPathNameA( path, s, n ) ;
		if( got > 0 && got < n ) return s ;
		sfree( s ) ;
		if( got == 0 ) return NULL ;
		n = got ;                  /* it grew meanwhile: the size it needs now */
	}
	return NULL ;
}

/* SearchPath for a program on the PATH, in full (sfree it); NULL = none. */
static char *kx_search_path_dup( const char *name ) {
	DWORD n = SearchPathA( NULL, name, NULL, 0, NULL, NULL ), got ;
	int tries ;
	for( tries = 0 ; n > 0 && tries < 4 ; tries++ ) {
		char *s = snewn( n, char ) ;
		got = SearchPathA( NULL, name, NULL, n, s, NULL ) ;
		if( got > 0 && got < n ) return s ;
		sfree( s ) ;
		if( got == 0 ) return NULL ;
		n = got ;
	}
	return NULL ;
}

/* A remote folder the SCP protocol cannot carry: SCP hands it to the
 * server's shell unquoted (scp -t / scp -f), where anything but letters,
 * digits, UTF-8 and / . _ - ~ % could split it or run as a command. SFTP
 * takes it as data. */
static int kx_folder_needs_sftp( const char *f ) {
	if( f == NULL ) return 0 ;
	for( const unsigned char *p = (const unsigned char *)f ; *p ; p++ ) {
		unsigned char c = *p ;
		if( c >= 0x80 || (c>='A'&&c<='Z') || (c>='a'&&c<='z') || (c>='0'&&c<='9')
		    || c=='/' || c=='.' || c=='_' || c=='-' || c=='~' || c=='%' ) continue ;
		return 1 ;
	}
	return 0 ;
}

/* The folder of a remote path (everything before its last '/'), in full
 * (sfree it); "" when it has none. */
static char *kx_folder_of( const char *path ) {
	char *f = dupstr( path ? path : "" ), *sl = strrchr( f, '/' ) ;
	if( sl ) *sl = '\0' ; else f[0] = '\0' ;
	return f ;
}

/* Starts the line in `buffer`; 0 when there is no helper to start. folder:
 * the remote folder this run aims at (NULL = none known); one SCP cannot
 * carry runs kscp in SFTP mode whatever the Protocol setting. */
static int kx_kscp_line( strbuf *buffer, kx_kscp_line_t *k, const char *folder ) {
	char *pscppath, *pscpport ;
	int p ;

	memset( k, 0, sizeof(*k) ) ;
	k->pw_mode = KX_PW_NONE ;
	if( PSCPPath==NULL ) {
		if( IniFileFlag == SAVEMODE_REG ) return 0 ;
		else if( !SearchPSCP() ) return 0 ;
	}
	if( !existfile( PSCPPath ) ) {
		if( IniFileFlag == SAVEMODE_REG ) return 0 ;
		else if( !SearchPSCP() ) return 0 ;
	}
	if( ( pscppath = kx_short_path_dup( PSCPPath ) ) == NULL ) return 0 ;

	strbuf_clear( buffer ) ;
	bcat( buffer, pscppath ) ; bcat( buffer, " " ) ;   /* exe: 8.3 path, no spaces/quotes */
	sfree( pscppath ) ;

	if( strlen(conf_get_str(conf, CONF_pscpoptions))>0 ) {     /* raw user options - intentionally unquoted */
		bcat( buffer, conf_get_str(conf, CONF_pscpoptions) ) ; bcat( buffer, " " ) ;
	}
	bcat( buffer, ( conf_get_int(conf, CONF_kscp_protocol)==0 && !kx_folder_needs_sftp( folder ) )
	              ? "-scp " : "-sftp " ) ;
	/* The session shares its connection (Connection > SSH > "Share SSH
	 * connections if possible"): kscp rides on the terminal's connection
	 * and does not log in again - no second password, no second 2FA. Only
	 * then: switching sharing on for a session that has it off is not
	 * kscp's to decide. */
	if( conf_get_bool(conf, CONF_ssh_connection_sharing) && !kx_noshare ) {
		bcat( buffer, "-share " ) ;
		k->shared = 1 ;
	}

	/* The session's own kscp port first; an empty field falls through to the
	 * global kscp port and then to the session's port (kitty_xfer_default_port). */
	pscpport = ReadParameterDup( INIT_SECTION, KI_PSCPPORT ) ;
	if( (p = kitty_xfer_port_field( conf_get_str(conf, CONF_kscp_port) )) > 0 ) {
		put_fmt( buffer, "-P %d ", p ) ;
	} else if( pscpport[0] ) {
		if( !strcmp( pscpport,"*" ) ) put_fmt( buffer, "-P %d ", conf_get_int(conf, CONF_port) ) ;
		else { bcat( buffer, "-P " ) ; bcat( buffer, pscpport ) ; bcat( buffer, " " ) ; }
	} else {
		if( (p = kx_override_port( conf_get_str(conf, CONF_sftpconnect) )) > 0 ) {
			put_fmt( buffer, "-P %d ", p ) ;
		} else {
			put_fmt( buffer, "-P %d ", conf_get_int(conf, CONF_port) ) ;
		}
	}
	sfree( pscpport ) ;

	if( conf_get_int(conf, CONF_sshprot) == 3 ) { bcat( buffer, "-2 " ) ; }   // SSH-2 Only

	if( !kitty_pw_empty(conf,CONF_password) ) {
		/* The password goes over in a PRIVATE FILE, not on the command line,
		 * where every process lister and audit log on the machine can read it.
		 * The file holds one line: the DPAPI-protected form when this Windows
		 * can produce it, else the password itself - the plain format -pwfile
		 * has always accepted. It is deleted after the start
		 * (kx_pwfiles_release below). Only if no file can be made at all does
		 * the password go on the command line, so a transfer never fails for
		 * this; qcat escapes any quote, so it cannot inject a switch. The span
		 * is noted either way, so the debug log can blank it - see
		 * debug_logevent_redacted().
		 * What goes IN the file, and what goes after -pw when no file could
		 * be made, is decided by the HELPER and not by us:
		 * kx_helper_reads_protected() tests whether the binary itself reads
		 * the protected form. A capable helper gets that form in either place,
		 * so even the command-line fallback carries no readable password;
		 * every released helper, and stock pscp/psftp, gets the plain value.
		 * The window shows which of the four it was -
		 * kx_pw_handover_line(). */
		/* A helper of ours that declares it in its version resource
		 * ("KiTTYPasswordHandle") gets it in MEMORY instead: a mapping only
		 * it inherits (kitty_pwhandle_make), "-pwhandle <handle>:<size>" on
		 * its command line - no file on disk at all. The file stays for the
		 * others, and for this one if no mapping can be made. */
		char pw[KITTY_PW_MAX+1] ; const char *pf ; char *line = NULL ; int enc ;
		char tok[64] ;
		kitty_pw_get( conf, CONF_password, pw, sizeof(pw) ) ;
		if( kx_helper_has_flag( PSCPPath, "KiTTYPasswordHandle" )
		    && ( k->pwmap = kitty_pwhandle_make( pw, tok, sizeof(tok) ) ) != NULL ) {
			bcat( buffer, "-pwhandle " ) ;
			k->pw_at = buffer->len ;
			bcat( buffer, tok ) ;
			k->pw_len = buffer->len - k->pw_at ;
			bcat( buffer, " " ) ;
			k->pw_mode = KX_PW_HANDLE ;
		} else {
			if( kx_helper_reads_protected( PSCPPath ) ) line = kitty_pwfile_line( pw ) ;
			enc = kitty_pwfile_line_is_protected( line ) ;
			pf = kx_password_file( line ? line : pw ) ;
			bcat( buffer, pf ? "-pwfile " : "-pw " ) ;
			k->pw_at = buffer->len ;
			qcat( buffer, pf ? pf : ( enc ? line : pw ) ) ;
			k->pw_len = buffer->len - k->pw_at ;   /* the span covers whatever was written */
			bcat( buffer, " " ) ;
			if( pf ) k->pwfiles++ ;
			k->pw_mode = pf ? ( enc ? KX_PW_FILE_ENC : KX_PW_FILE_PLAIN )
			                : ( enc ? KX_PW_CMD_ENC  : KX_PW_CMD_PLAIN ) ;
			if( line ) { smemclr( line, strlen(line) ) ; sfree( line ) ; }
		}
		smemclr( pw, sizeof(pw) ) ;
	}
	if( strlen( conf_get_str(conf,CONF_portknockingoptions)) > 0 ) {
		bcat( buffer, "-knock " ) ; qcat( buffer, conf_get_str(conf,CONF_portknockingoptions) ) ; bcat( buffer, " " ) ;
	}
	{ const char *kf = kx_helper_keyfile(conf) ;
	  if( kf != NULL ) { bcat( buffer, "-i " ) ; qcat( buffer, kf ) ; bcat( buffer, " " ) ; }
	}
	kx_proxy_args( buffer, &k->pwfiles, &k->px_at, &k->px_len ) ;   /* the connection's proxy, if any */
	return 1 ;
}

/* The remote end of the line, up to and including the ':' before the path:
 * user@host (user@[addr] for an IPv6 address), or the user@host part of an
 * explicit -sftpconnect target. Appended to `out`. */
static void kx_kscp_remote( strbuf *out ) {
	const char *o = conf_get_str(conf, CONF_sftpconnect) ;
	int p ;
	if( strlen( o ) > 0 ) {
		if( (p=poss(":",o)) > 0 ) put_data( out, o, (size_t)( p - 1 ) ) ;
		else bcat( out, o ) ;
	} else {
		bcat( out, conf_get_str_ambi(conf,CONF_username,NULL) ) ; bcat( out, "@" ) ;
		if( poss( ":", conf_get_str(conf,CONF_host))>0 ) { bcat(out,"[") ; bcat(out,conf_get_str(conf,CONF_host)) ; bcat(out,"]") ; }
		else { bcat( out, conf_get_str(conf,CONF_host) ) ; }
	}
	bcat( out, ":" ) ;
}

/* The host and account Get File fetches from, as kx_kscp_remote: the target
 * override's ([user@]host[:port], the port dropped) when set, else the
 * session's. An override without a user leaves user empty. Both in full
 * (sfree them). */
static void kx_gf_target( char **host, char **user ) {
	const char *o = conf_get_str( conf, CONF_sftpconnect ), *at ;
	if( o && o[0] ) {
		char *c, *h ;
		at = strchr( o, '@' ) ;
		*user = at ? dupprintf( "%.*s", (int)( at - o ), o ) : dupstr( "" ) ;
		h = dupstr( at ? at + 1 : o ) ;
		if( h[0] == '[' ) {                    /* [v6]:port */
			if( ( c = strchr( h, ']' ) ) != NULL ) *c = '\0' ;
			memmove( h, h + 1, strlen( h ) ) ;
		} else if( ( c = strchr( h, ':' ) ) != NULL && !strchr( c + 1, ':' ) )
			*c = '\0' ;                           /* host:port */
		*host = h ;
	} else {
		*user = dupstr( conf_get_str_ambi( conf, CONF_username, NULL ) ) ;
		*host = dupstr( conf_get_str( conf, CONF_host ) ) ;
	}
}

/* The reason for a clipboard text that cannot be a path on the target: the
 * class's text, NULL for the classes that go on. text: the trimmed text. */
static char *kx_gf_why_not_path( int cls, const char *text, const char *th, const char *tu ) {
	switch( cls ) {
	  case GF_URL:     return dupstr( KT_GETFILE_WHY_URL ) ;
	  case GF_CONTROL: return dupstr( KT_GETFILE_WHY_CONTROL ) ;
	  case GF_TOOLONG: return dupstr( KT_GETFILE_WHY_TOOLONG ) ;
	  case GF_ELSEWHERE: {
		/* user and host are parts of the text, so its length holds them */
		size_t n = strlen( text ) + 1 ;
		char *su = snewn( n, char ), *sh = snewn( n, char ), *whom, *there, *why ;
		const char *rest ;
		if( !gf_scp_form( text, su, n, sh, n, &rest ) ) su[0] = sh[0] = '\0' ;
		whom = dupprintf( "%s%s%s", su, su[0] ? "@" : "", sh ) ;
		there = dupprintf( "%s%s%s", tu, tu[0] ? "@" : "", th ) ;
		why = dupprintf( KT_GETFILE_WHY_ELSEWHERE, whom, there ) ;
		sfree( su ) ; sfree( sh ) ; sfree( whom ) ; sfree( there ) ;
		return why ;
	  }
	  default:         return NULL ;
	}
}

void SendOneFile( HWND hwnd, char * directory, char * filename, char * distantdir) {
	kx_send_one( hwnd, directory, filename, distantdir, NULL, 0 ) ;
}

/* drop / drop_idx: the drop upload this file belongs to (NULL = none); its
 * remote path is noted here, and the run is settled whatever happens. */
static void kx_send_one( HWND hwnd, char * directory, char * filename, char * distantdir,
                         kx_drop_t *drop, int drop_idx ) {
	char *remotedir ;
	strbuf *buffer, *tgt ;
	kx_kscp_line_t k ;               /* KiTTY: what the shared builder handed over, and where */

	if( distantdir == NULL ) { distantdir = kitty_current_dir() ; }

	/* The local upload folder ([KiTTY] uploaddir, the session's zUploadDir)
	 * is where the Send File picker opens (SendFile below); the file to send
	 * arrives here already chosen, as directory + filename. */

	if( (distantdir != NULL ) && ( strlen(distantdir)>0 ) ) {
		remotedir = dupstr( distantdir ) ;
	} else if( strlen(conf_get_str(conf,CONF_pscpremotedir))>0 ) {
		/* fixed remote dir sanity check: qcat below already quotes/escapes the
		 * whole user@host:dir argument (and kscp runs via CreateProcess, no
		 * shell), so injection is handled - but reject control characters that a
		 * quoted argument can't sensibly carry, falling back to the remote home. */
		const char * rd = conf_get_str(conf,CONF_pscpremotedir) ; const char * q ; int ok = 1 ;
		for( q = rd ; *q ; q++ ) if( (unsigned char)*q < 0x20 ) { ok = 0 ; break ; }
		remotedir = dupstr( ok ? rd : "." ) ;
	} else { remotedir = dupstr( "." ) ;
	}
	if( strlen( remotedir ) == 0 ) { sfree( remotedir ) ; remotedir = dupstr( "." ) ; }

	/* The whole line in a strbuf: the remote target dir is its LAST argument,
	 * and a fixed buffer cut exactly that first. strbuf_new_nm: the line can
	 * carry a password, and its memory is cleared when it grows or goes. */
	buffer = strbuf_new_nm() ;
	if( !kx_kscp_line( buffer, &k, remotedir ) ) {
		strbuf_free( buffer ) ; sfree( remotedir ) ;
		kx_drop_settle( drop, drop_idx, 0 ) ; return ;
	}
	/* kscp runs without a console anybody sees: a question (a one-time
	 * code) would wait there unseen. -batch ends the run with the reason,
	 * and the window shows the cure (KT_XFER_HINT_PROMPT). */
	bcat( buffer, "-batch " ) ;

	/* source path (single quoted argument) */
	{
		char *src ;
		if( (strlen(directory)>0) && (strlen(filename)>0) ) {
			src = dupprintf( "%s\\%s", directory, filename ) ;
		} else if( (directory!=NULL)&&(strlen(directory)>0) ) {
			src = dupstr( directory ) ;
		} else {
			src = dupstr( filename ) ;
		}
		qcat( buffer, src ) ; bcat( buffer, " " ) ;
		/* KiTTY: where it lands, as kscp resolves remotedir ("." = home,
		 * a relative dir = under home): typed if the upload succeeds. */
		if( drop && drop_idx >= 0 && drop_idx < drop->n && !drop->remote[drop_idx] )
			drop->remote[drop_idx] = kitty_droppath_remote( remotedir, src ) ;
		sfree( src ) ;
	}

	/* destination user@host:remotedir (single quoted argument) */
	tgt = strbuf_new() ;
	kx_kscp_remote( tgt ) ;
	bcat( tgt, remotedir ) ;
	qcat( buffer, tgt->s ) ;

	chdir( InitialDirectory ) ;
	debug_logevent_redacted2( "Run", buffer->s, k.pw_at, k.pw_len, k.px_at, k.px_len ) ;
	/* Capture output + show it on failure, instead of flashing a console shut
	 * (so e.g. a server's exit-127 "Cannot initialize SFTP" is readable). */
	{ char *whatbuf = dupprintf( KT_XFER_UPLOAD_OF, filename ? filename : KT_XFER_FILE ), *updir ;
	  char *note = kx_hello_agent_note( conf ) ;
	  char *intro = dupprintf( KT_XFER_UPLOADING,
	                           note ? note : "", filename ? filename : KT_XFER_FILE, tgt->s ) ;
	  /* One line on how the password travelled, from what the builder
	   * above actually did. Nothing when the login uses a key or the agent. */
	  { const char *hand = kx_pw_handover_line( k.pw_mode ) ;
	    if( hand != NULL ) {
		char *t = dupprintf( "%s%s\r\n\r\n", intro, hand ) ;
		sfree( intro ) ; intro = t ;
	    } }
	  /* files leaving: the balloon's click opens the local upload folder */
	  updir = kitty_xfer_upload_dir_dup( conf ) ;
	  if( k.shared ) {              /* a refused channel: again on its own login */
		kx_rerun_next.kind = 1 ;
		kx_rerun_next.dir = dupstr( directory ) ;
		kx_rerun_next.file = dupstr( filename ) ;
		kx_rerun_next.dist = dupstr( distantdir ) ;
		kx_rerun_next.drop = drop ;
		kx_rerun_next.drop_idx = drop_idx ;
	  }
	  if( kitty_run_xfer( hwnd, buffer->s, whatbuf, intro, 0, NULL, updir, NULL, NULL, INVALID_HANDLE_VALUE,
	                      drop, drop_idx, k.pwmap ) != 0 )
		kx_drop_settle( drop, drop_idx, 0 ) ;   /* not started: the window never took it */
	  sfree( intro ) ; sfree( note ) ; sfree( whatbuf ) ; sfree( updir ) ; }

	strbuf_free( buffer ) ;                     /* cleared: it may hold the password */
	strbuf_free( tgt ) ;
	sfree( remotedir ) ;
	if( k.pwfiles ) kx_pwfiles_release( 0 ) ;   /* the password file, once kscp has had it */
	if( k.pwmap ) CloseHandle( k.pwmap ) ;      /* kscp holds its own handle to it */
	}

void SendFileList( HWND hwnd, char * filelist ) {
	char *pname=NULL, *dir ;
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
	dir = dupstr( filelist ) ;

	pname=filelist+strlen(filelist)+1;

	i = 0 ;
	while( pname[i] != '\0' ){

		while( (pname[i] != '\0') && (pname[i] != '\n') && (pname[i] != '\r') ) { i++ ; }
		pname[i]='\0';
		SendOneFile( hwnd, dir, pname, NULL ) ;
		pname=pname+i+1 ; i=0;
		}
	sfree( dir ) ;
	}

/* Send File's picker runs beside the session (kitty_pick_async): the
 * common dialog's own loop serves no network events, and the session stood
 * still while it was open. The upload starts when the file has been picked.
 * One picker at a time: the token is set while it is open. */
static void * kx_send_pick = NULL ;

static void kx_send_picked( void * ctx, const char * path ) {
	HWND hwnd = (HWND)ctx ;
	char * list ;
	kx_send_pick = NULL ;
	if( path == NULL || !path[0] || !IsWindow( hwnd ) ) return ;
	/* two NULs at the end: SendFileList reads the byte after the first */
	list = calloc( strlen( path ) + 2, 1 ) ;
	if( list == NULL ) return ;
	strcpy( list, path ) ;
	SendFileList( hwnd, list ) ;
	free( list ) ;
	}

void SendFile( HWND hwnd ) {
	char *updir, *picked ;

	if( conf_get_int(conf,CONF_protocol) != PROT_SSH ) {
		kx_report( KT_CAP_XFER, KT_MSG_SSH_ONLY ) ;
		return ;
		}
	if( kx_send_pick != NULL ) return ;   /* its picker is open already */

	/* kscp not there: reported before the picker, as Get File does - the
	 * picker opened, and the files picked then went nowhere, silently */
	if( !kitty_xfer_tool_ready( 0 ) ) {
		kx_report( KT_CAP_XFER, KT_XFER_KSCP_NOT_FOUND ) ;
		debug_logevent( "%s", KT_XFER_KSCP_NOT_FOUND ) ;
		return ;
	}
	/* kscp checked before the picker: a Yes in its confirmation box re-opens Send File */
	if( !kitty_helper_ready( hwnd, PSCPPath, 1, 0, kx_again_sendfile, kx_again( hwnd ), kx_again_free ) )
		return ;

	/* The picker opens in the upload folder (Connection > File-Transfer-Settings, else
	 * the global one, else Documents). */
	updir = kitty_xfer_upload_dir_dup( conf ) ;
	kx_send_pick = kitty_pick_async( hwnd, KITTY_PICK_OPEN, updir, KT_CAP_SEND_FILE,
	                                 "All files (*.*)|*.*|", kx_send_picked, (void *)hwnd ) ;
	if( kx_send_pick != NULL ) { sfree( updir ) ; return ; }
	/* no thread for the picker: on this one, as before; the path in full,
	 * handed over as the threaded picker hands it */
	picked = kitty_pick_open_dup( hwnd, KT_CAP_SEND_FILE, "All files (*.*)|*.*|", updir ) ;
	sfree( updir ) ;
	if( picked != NULL && picked[0] ) kx_send_picked( (void *)hwnd, picked ) ;
	sfree( picked ) ;
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
    GetOneFileToPath( hwnd, directory, filename, NULL, NULL ) ;
}

/* localdir NULL = the download folder (kitty_xfer_download_dir). */
void GetOneFileTo( HWND hwnd, char * directory, const char * filename, const char * localdir ) {
    GetOneFileToPath( hwnd, directory, filename, localdir, NULL ) ;
}

/* localfile != NULL: kscp writes to that exact local PATH (a single named file
 * chosen through Save-As, so Windows' own replace prompt has already handled an
 * overwrite) instead of into the local folder. */
void GetOneFileToPath( HWND hwnd, char * directory, const char * filename, const char * localdir, const char * localfile ) {
    GetOneFileStaged( hwnd, directory, filename, localdir, localfile, NULL, INVALID_HANDLE_VALUE ) ;
}

/* The full form. final_dir != NULL: this is a staged wildcard/folder download -
 * localdir is the staging folder kscp writes into, final_dir the folder the
 * files are moved to on success, lock the held-open marker. */
void GetOneFileStaged( HWND hwnd, char * directory, const char * filename, const char * localdir, const char * localfile, const char * final_dir, HANDLE lock ) {
    char *dir ;
    strbuf *buffer ;
    kx_kscp_line_t k ;   /* what the shared builder handed over, and where */

    if( localdir != NULL && localdir[0] && existdirectory( localdir ) ) dir = dupstr( localdir ) ;
    else dir = kitty_xfer_download_dir_dup( conf ) ;

    /* In a strbuf: the LOCAL destination is the last argument, and a long
     * remote source pushed it off the end of a fixed buffer - kscp then wrote
     * to a cut path. */
    buffer = strbuf_new_nm() ;
    {   /* the remote file's folder decides SCP or SFTP (kx_folder_needs_sftp) */
        char *rpath = ( filename[0]=='/' || directory==NULL || !directory[0] )
                      ? dupstr( filename ) : dupprintf( "%s/%s", directory, filename ) ;
        char *rfolder = kx_folder_of( rpath ) ;
        int ok = kx_kscp_line( buffer, &k, rfolder ) ;
        sfree( rfolder ) ; sfree( rpath ) ;
        if( !ok ) { strbuf_free( buffer ) ; sfree( dir ) ; return ; }
    }
    bcat( buffer, "-batch " ) ;    /* no unseen questions: see kx_send_one */

    /* remote source user@host:path (single quoted argument) */
    {
        strbuf *src = strbuf_new() ;
        kx_kscp_remote( src ) ;
        if( filename[0]=='/' ) {
            bcat( src, filename ) ;
        } else if( (directory!=NULL) && (strlen(directory)>0) && (strlen(filename)>0) ) {
            bcat( src, directory ) ; bcat( src, "/" ) ; bcat( src, filename ) ;
        } else if( (directory!=NULL) && (strlen(directory)>0) ) {
            bcat( src, directory ) ; bcat( src, "/*" ) ;
        } else {
            bcat( src, filename ) ;
        }
        qcat( buffer, src->s ) ; bcat( buffer, " " ) ;
        strbuf_free( src ) ;
    }
    /* Local destination: an exact file path (Save-As, single named file) when
     * given, else the folder. Both single-quoted so a space cannot split it. */
    qcat( buffer, ( localfile && localfile[0] ) ? localfile : dir ) ;

    chdir( InitialDirectory ) ;

    /* KiTTY: the password's span blanked, as Send File always did - the plain
     * line put it in the debug log on the no-file fallback (-pw) */
    debug_logevent_redacted2( "Get on file", buffer->s, k.pw_at, k.pw_len, k.px_at, k.px_len ) ;
    /* Capture output + show on failure (no vanishing console). */
    { char *whatbuf = dupprintf( KT_XFER_DOWNLOAD_OF, filename ? filename : KT_XFER_FILE ) ;
      char *note = kx_hello_agent_note( conf ) ;
      /* The balloon's click: the file it will have produced (dir + the remote
       * name's last component) when that exists once kscp is done, else the
       * folder. A wildcard or a trailing slash means a folder's contents. */
      char *one = NULL ;
      if( localfile && localfile[0] ) {
          one = dupstr( localfile ) ;   /* Save-As: the exact file it will produce */
      } else if( filename && filename[0] && !strpbrk( filename, "*?" ) && filename[strlen(filename)-1] != '/' ) {
          const char *base = strrchr( filename, '/' ) ;
          one = dupprintf( "%s\\%s", dir, base ? base + 1 : filename ) ;
      }
      /* No target line, but the note if the key needs loading - and the one
       * line on how the password travelled, from what the builder above
       * actually did. Nothing of it when the login uses a key or the agent. */
      const char *hand = kx_pw_handover_line( k.pw_mode ) ;
      char *intro = hand ? dupprintf( "%s%s\r\n\r\n", note ? note : "", hand ) : NULL ;
      /* Staged (wildcard/folder): kscp wrote into `dir` (the staging folder);
       * on success the files move to final_dir. The balloon opens final_dir. */
      if( k.shared ) {              /* a refused channel: again on its own login */
          kx_rerun_next.kind = 2 ;
          kx_rerun_next.dir = dupstr( directory ) ;
          kx_rerun_next.file = dupstr( filename ) ;
          kx_rerun_next.localdir = dupstr( localdir ) ;
          kx_rerun_next.localfile = dupstr( localfile ) ;
          kx_rerun_next.final = dupstr( final_dir ) ;
          kx_rerun_next.lock = lock ;
      }
      kitty_run_xfer( hwnd, buffer->s, whatbuf, intro ? intro : note, 1, one,
                      final_dir ? final_dir : dir,
                      final_dir ? dir : NULL, final_dir, lock, NULL, 0, k.pwmap ) ;
      sfree( intro ) ; sfree( one ) ; sfree( note ) ; sfree( whatbuf ) ; }

    strbuf_free( buffer ) ;                     /* cleared: it may hold the password */
    sfree( dir ) ;
    if( k.pwfiles ) kx_pwfiles_release( 0 ) ;   /* the password file, once kscp has had it */
    if( k.pwmap ) CloseHandle( k.pwmap ) ;      /* kscp holds its own handle to it */
}

/* --- Where received files go ------------------------------------------------
 * ONE answer for kscp Get file, ZModem receives and file transfers over the
 * session: the session's local download folder (Connection > File-Transfer-Settings), else
 * the global Download folder (Transfers & Tools), else the user's Downloads
 * folder, else the folder KiTTY started in. A configured folder that does not
 * exist is skipped, not created. In full (sfree it), never NULL. */

/* `stored` %VAR%-expanded (cyd01/KiTTY#472) - the stored value stays
 * literal; only this copy, tested and returned, is expanded - when that is an
 * existing folder, in memory of its own (sfree it); else NULL. */
static char *kx_existing_dir_dup( const char *stored ) {
    char *expanded, *out = NULL ;
    if( stored == NULL || !stored[0] ) return NULL ;
    expanded = kitty_expand_env_dup( stored ) ;
    if( expanded != NULL && existdirectory( expanded ) ) out = dupstr( expanded ) ;
    free( expanded ) ;
    return out ;
}

/* <USERPROFILE>\<sub> when that folder exists (sfree it); else NULL. */
static char *kx_profile_dir_dup( const char *sub ) {
    const char * prof = getenv( "USERPROFILE" ) ;
    char *out ;
    if( prof == NULL || !prof[0] ) return NULL ;
    out = dupprintf( "%s\\%s", prof, sub ) ;
    if( existdirectory( out ) ) return out ;
    sfree( out ) ;
    return NULL ;
}

/* The two folder answers below: the session's field, the [KiTTY] key,
 * <USERPROFILE>\<sub>, else the folder KiTTY started in. */
static char *kx_xfer_dir_dup( Conf * cf, int field, const char * key, const char * sub ) {
    char *out = NULL, *v ;
    if( cf != NULL && ( out = kx_existing_dir_dup( conf_get_str( cf, field ) ) ) != NULL ) return out ;
    v = ReadParameterDup( INIT_SECTION, key ) ;
    out = kx_existing_dir_dup( v ) ;
    sfree( v ) ;
    if( out != NULL ) return out ;
    if( ( out = kx_profile_dir_dup( sub ) ) != NULL ) return out ;
    return dupstr( InitialDirectory ) ;
}

char * kitty_xfer_download_dir_dup( Conf * cf ) {
    return kx_xfer_dir_dup( cf, CONF_zdownloaddir, KI_DOWNLOADDIR, "Downloads" ) ;
}

/* The mirror of kitty_xfer_download_dir for files LEAVING this PC: the
 * session's upload folder (Connection > File-Transfer-Settings), else the global Default
 * Upload Folder (Transfers & Tools, [KiTTY] uploaddir), else the user's
 * Documents folder, else the folder KiTTY started in. Where the Send File
 * picker opens, and where a plain name the far end requests to read (kitten
 * transfer) is looked up. Never the remote target of an upload. In full
 * (sfree it), never NULL. */
char * kitty_xfer_upload_dir_dup( Conf * cf ) {
    return kx_xfer_dir_dup( cf, CONF_zuploaddir, KI_UPLOADDIR, "Documents" ) ;
}

/* [KiTTY] transfernotification (default yes): the tray balloon after a
 * finished transfer - kscp (Send File, Get File, drag-and-drop), ZModem
 * receive and upload, kitten transfer arriving and leaving. `what` is the
 * "%s" of KT_XFER_COMPLETE. A click on the balloon opens `path`: for files
 * arriving the one file (shown selected in Explorer) or the folder they
 * landed in; for files leaving the local upload folder. For files arriving
 * the text gets a second line showing the folder (nfiles: 1 = one file, more
 * = the count, 0 = not counted); a path that is a file yields its folder. */
int kitty_xfer_notify_enabled( void ) {
    char v[64] = "" ;
    if( ReadParameterN( INIT_SECTION, KI_TRANSFERNOTIFICATION, v, sizeof(v) ) && v[0] )
        return stricmp( v, "no" ) != 0 && stricmp( v, "0" ) != 0 ;
    return 1 ;
}
/* `renamed` > 0: a Get File saved that many files under a new name because the
 * name was taken; the balloon reports it, as the approved wording requires,
 * beside the line the transfer window already carries. */
void kitty_xfer_notify_ex( const char * what, int arriving, int nfiles, const char * path, int renamed ) {
    char * m, * done ;
    if( !kitty_xfer_notify_enabled() ) return ;
    done = dupprintf( KT_XFER_COMPLETE, what ? what : KT_XFER_TRANSFER ) ;
    if( arriving && path && path[0] ) {
        char *folder = dupstr( path ) ;
        if( !existdirectory( folder ) ) {          /* a file: its folder */
            char * bs = strrchr( folder, '\\' ) ;
            if( bs && bs > folder ) *bs = '\0' ;
        }
        if( nfiles == 1 ) m = dupprintf( "%s\r\n" KT_XFER_SAVED_ONE, done, folder ) ;
        else if( nfiles > 1 ) m = dupprintf( "%s\r\n" KT_XFER_SAVED_MANY, done, nfiles, folder ) ;
        else m = dupprintf( "%s\r\n" KT_XFER_SAVED_TO, done, folder ) ;
        sfree( folder ) ;
    } else {
        m = dupstr( done ) ;
    }
    if( renamed > 0 ) {
        char * t = dupprintf( "%s\r\n" KT_XFER_RENAMED_NOTE, m, renamed ) ;
        sfree( m ) ; m = t ;
    }
    kitty_tray_balloon_async( KT_CAP_XFER, m, path ) ;   /* szInfo truncates a long path */
    sfree( m ) ; sfree( done ) ;
}
void kitty_xfer_notify( const char * what, int arriving, int nfiles, const char * path ) {
    kitty_xfer_notify_ex( what, arriving, nfiles, path, 0 ) ;
}

/* Does this session show the Tools menu entry? 0 = Send File (kscp),
 * 1 = Start WinSCP, 2 = Start FileZilla, 3 = Get File (kscp) - the same
 * numbering as kitty_xfer_tool_ready for the first three. Off = the entry is
 * not built and its [Shortcuts] key falls through to the terminal. */
int kitty_xfer_tool_shown( Conf * cf, int which ) {
    if( cf == NULL ) return 1 ;
    switch( which ) {
      case 0: return conf_get_bool( cf, CONF_tools_sendfile ) ;
      case 1: return conf_get_bool( cf, CONF_tools_winscp ) ;
      case 2: return conf_get_bool( cf, CONF_tools_filezilla ) ;
      case 3: return conf_get_bool( cf, CONF_tools_getfile ) ;
    }
    return 1 ;
}

/* Is the helper behind a Tools-menu entry there? 0 = kscp, 1 = WinSCP. The
 * menu greys an entry whose tool is missing instead of offering a click that
 * ends in nothing (registry mode) or in a "not found" notice. A stored path
 * that is gone runs the search again; what it finds is used for this run. */
int kitty_xfer_tool_ready( int which ) {
    if( which == 0 ) {
        if( PSCPPath == NULL || !existfile( PSCPPath ) ) SearchPSCP() ;
        return PSCPPath != NULL && existfile( PSCPPath ) ;
    } else if( which == 1 ) {
        if( WinSCPPath == NULL || !existfile( WinSCPPath ) ) SearchWinSCP() ;
        return WinSCPPath != NULL && existfile( WinSCPPath ) ;
    } else {
        if( FileZillaPath == NULL || !existfile( FileZillaPath ) ) SearchFileZilla() ;
        return FileZillaPath != NULL && existfile( FileZillaPath ) ;
    }
}

/* A port field: used as it is when it holds a plain number in range, ignored
 * otherwise (an empty field is the normal case, and a field holding anything
 * else must not silently become port 0). Returns 0 = not set. */
int kitty_xfer_port_field( const char * s ) {
    long v ; const char * p ;
    if( s == NULL || *s == '\0' ) return 0 ;
    for( p = s ; *p ; p++ ) if( *p < '0' || *p > '9' ) return 0 ;
    v = atol( s ) ;
    if( v < 1 || v > 65535 ) return 0 ;
    return (int)v ;
}

/* The port written inside a target override ([user@]hostname[:port]), else 0.
 * A bracketed IPv6 literal keeps its own colons - only a ':' after the closing
 * bracket is a port - and a bare IPv6 address has none at all. */
static int kx_override_port( const char * ov ) {
    const char * h ; const char * c ;
    if( ov == NULL || *ov == '\0' ) return 0 ;
    h = strrchr( ov, '@' ) ; h = ( h != NULL ) ? h + 1 : ov ;
    if( *h == '[' ) {
        const char * b = strchr( h, ']' ) ;
        if( b == NULL ) return 0 ;
        c = strchr( b, ':' ) ;
    } else {
        c = strchr( h, ':' ) ;
        if( c != NULL && strchr( c + 1, ':' ) != NULL ) return 0 ;   /* bare IPv6 */
    }
    if( c == NULL ) return 0 ;
    return kitty_xfer_port_field( c + 1 ) ;
}

/* The port a tool WILL use when its Port field is empty - the fallback the
 * builders below apply, and the hint the panels show. kitty.h has the rules. */
int kitty_xfer_default_port( Conf * cf, int tool, int protocol ) {
    int p ;
    if( cf == NULL ) return 22 ;

    if( tool == 0 ) {                     /* kscp: the global port comes first */
        char *pscpport = ReadParameterDup( INIT_SECTION, KI_PSCPPORT ) ;
        if( pscpport[0] ) {
            int gp = strcmp( pscpport, "*" ) != 0 ? kitty_xfer_port_field( pscpport ) : 0 ;
            sfree( pscpport ) ;
            if( gp > 0 ) return gp ;
            return conf_get_int( cf, CONF_port ) ;   /* "*" = the session's port */
        }
        sfree( pscpport ) ;
        if( (p = kx_override_port( conf_get_str( cf, CONF_sftpconnect ) )) > 0 )
            return p ;
        return conf_get_int( cf, CONF_port ) ;
    }

    /* WinSCP and FileZilla: a port written into the target override wins over
     * everything, the tool's own Port field included - the override gives the
     * machine those two are to reach, port and all. Without a port it supplies
     * user and host only, and the rules below decide the port. */
    if( (p = kx_override_port( conf_get_str( cf, CONF_sftpconnect ) )) > 0 )
        return p ;

    switch( protocol ) {
      case 2: return 21 ;                 /* ftp */
      case 3: return 990 ;                /* ftps, implicit TLS */
      case 4: return 21 ;                 /* ftpes, explicit TLS (AUTH TLS) */
      case 5: return 80 ;                 /* http  (WebDAV) */
      case 6: return 443 ;                /* https (WebDAV) */
      default: break ;                    /* scp and sftp */
    }
    /* SFTP/SCP over the session's own connection: its port, but only when that
     * port is an SSH port. A telnet or raw session's port is not. */
    if( conf_get_int( cf, CONF_protocol ) == PROT_SSH )
        return conf_get_int( cf, CONF_port ) ;
    return 22 ;
}

/* kitty_xfer_migrate_protocol() is a LOAD-path rule and lives in
 * kitty/kitty_settings_load.c, not here: the .ktx reader is linked into targets
 * that have no transfer code at all (windows/test/test_storage_roundtrip). */

/* Get File's transfers, once the remote paths are settled - by the check
 * below or by the user in the Get File box. `clip` is the text, trimmed
 * (dupstr'd; freed here): one path per line. An absolute path (or ~) is
 * taken as it is; a bare name is joined to the OSC 7 tracked directory when
 * tracking is on, else kscp resolves it against the login directory. Several
 * lines mean several files, fetched one after the other, into a folder the
 * picker returns (it opens on kitty_xfer_download_dir); one named file goes
 * through a Save-As instead. The transfer runs through GetOneFileTo() and its
 * relatives - so the port, protocol, key-file and password handling are the
 * ones every kscp download uses, and "Keep the transfer window open after
 * success" applies.
 *
 * Every step that waits for the user runs beside the session: the Save-As
 * and the folder picker on a thread of their own (kitty_call_async,
 * kitty_pick_async), the "already exist" question modeless. Each of them
 * stood the session still while it was open - no output, no keepalive, a
 * server's close unseen. The steps share one context, kx_getgo_t, and one
 * Get File at a time goes through them. */
typedef struct {
    HWND hwnd ;
    char * clip ;             /* the paths, one per line (freed with this) */
    char * defdir ;           /* the download folder the pickers open on */
    char * save ;             /* Save-As: the remote name in, the chosen path out */
    char * dir ;              /* the folder picked */
    int ok ;                  /* Save-As: a file was chosen */
    int keepboth ;
} kx_getgo_t ;
static kx_getgo_t * kx_getgo_cur = NULL ;   /* a Get File waits for the user */
static HWND kx_getgo_box = NULL ;           /* its "already exist" question */

static void kx_getgo_free( kx_getgo_t * g ) {
    if( kx_getgo_cur == g ) kx_getgo_cur = NULL ;
    sfree( g->clip ) ;
    sfree( g->defdir ) ; sfree( g->save ) ; sfree( g->dir ) ;
    sfree( g ) ;
}

/* The remote path of one line: as it is (absolute, ~, or no OSC 7
 * directory known), else joined to the tracked directory - in full (sfree
 * it). A fixed buffer cut it, and a different file was fetched. */
static char *kx_getgo_remote( const char * line ) {
    if( line[0] == '/' || line[0] == '~' || kitty_current_dir() == NULL ) return dupstr( line ) ;
    return dupprintf( "%s/%s", kitty_current_dir(), line ) ;
}

/* The transfers: every line of g->clip into g->dir. */
static void kx_getgo_run( kx_getgo_t * g ) {
    HWND hwnd = g->hwnd ;
    char * line = g->clip, * next ;
    char * dir = g->dir ;
    char * remote ;
    int keepboth = g->keepboth ;

    for( ; line != NULL && *line ; line = next ) {
        next = strpbrk( line, "\r\n" ) ;
        if( next ) { *next = '\0' ; next++ ; while( *next == '\r' || *next == '\n' ) next++ ; }
        str_rtrim( line, " \t" ) ;
        while( *line == ' ' || *line == '\t' ) line++ ;
        if( *line == '\0' ) continue ;
        remote = kx_getgo_remote( line ) ;
        int is_glob = ( strpbrk( line, "*?" ) || line[strlen(line)-1]=='/' ) ;
        if( is_glob ) {
            /* CASE 3: into a staging folder, moved out (with rename) on success. */
            char *stage ; HANDLE lock = INVALID_HANDLE_VALUE ;
            kitty_xfer_sweep_dir( dir ) ;   /* clear any stale staging first */
            if( ( stage = kitty_xfer_make_stage_dup( dir, &lock ) ) != NULL )
                GetOneFileStaged( hwnd, NULL, remote, stage, NULL, dir, lock ) ;
            else
                GetOneFileTo( hwnd, NULL, remote, dir ) ;   /* fallback: straight in */
            sfree( stage ) ;
        } else if( keepboth ) {
            /* Keep both: a colliding name is saved as "name (1)". */
            const char *base = strrchr( remote, '/' ) ; base = base ? base+1 : remote ;
            char *cand = dupprintf( "%s\\%s", dir, base ) ;
            if( existfile( cand ) ) {
                char *freed = kitty_xfer_free_name_dup( dir, base ) ;
                GetOneFileToPath( hwnd, NULL, remote, dir, freed ) ;
                sfree( freed ) ;
            } else GetOneFileTo( hwnd, NULL, remote, dir ) ;
            sfree( cand ) ;
        } else {
            GetOneFileTo( hwnd, NULL, remote, dir ) ;   /* Overwrite / no clash */
        }
        sfree( remote ) ;
    }
    kx_getgo_free( g ) ;
}

/* ONE named file: the Save-As, on the worker thread (kitty_call_async). The
 * answer in full replaces the prefilled name; a cancel leaves that. */
static void kx_getgo_saveas_run( void * v ) {
    kx_getgo_t * g = (kx_getgo_t *)v ;
    char *picked = kitty_pick_save_dup( g->hwnd, KT_XFER_SAVE_AS_TITLE, "All files (*.*)|*.*|", g->save, g->defdir ) ;
    g->ok = picked != NULL ;
    if( picked != NULL ) { sfree( g->save ) ; g->save = picked ; }
}

/* ...and its answer, back on the terminal's thread: kscp writes to that path. */
static void kx_getgo_saved( void * v ) {
    kx_getgo_t * g = (kx_getgo_t *)v ;
    if( g->ok && IsWindow( g->hwnd ) ) {
        char *remote = kx_getgo_remote( g->clip ) ;
        GetOneFileToPath( g->hwnd, NULL, remote, NULL, g->save ) ;
        sfree( remote ) ;
    }
    kx_getgo_free( g ) ;
}

/* The "already exist" question answered: 1 Overwrite, 2 Keep both; Cancel,
 * Escape or the window going (0, 3) fetch nothing. */
static void kx_getgo_exist_done( int answer, void * v ) {
    kx_getgo_t * g = (kx_getgo_t *)v ;
    kx_getgo_box = NULL ;
    if( ( answer == 1 || answer == 2 ) && IsWindow( g->hwnd ) ) {
        g->keepboth = ( answer == 2 ) ;
        kx_getgo_run( g ) ;
    } else {
        kx_getgo_free( g ) ;
    }
}

/* The folder picked (path NULL = cancelled): the clash check, then the
 * transfers. Called on the terminal's thread (kitty_pick_async's done). */
static void kx_getgo_folder( void * v, const char * path ) {
    kx_getgo_t * g = (kx_getgo_t *)v ;
    const char * line = g->clip ;
    char * dir ;
    if( path != g->dir ) { char *d = dupstr( path ? path : "" ) ; sfree( g->dir ) ; g->dir = d ; }
    dir = g->dir ;
    if( dir == NULL || !dir[0] || !existdirectory( dir ) || !IsWindow( g->hwnd ) ) {
        kx_getgo_free( g ) ; return ;
    }

    /* Are any lines a wildcard (*?) or a folder (trailing '/')? Those download
     * through a staging folder; a selection of named files instead gets the
     * "already exists" box below. Scans on a copy, so `line` stays intact. */
    int any_glob = 0 ;
    { char *scan = dupstr( line ) ; char *l = scan, *nx ;
      for( ; l && *l ; l = nx ) { nx = strpbrk( l, "\r\n" ) ; if( nx ){ *nx='\0'; nx++; while(*nx=='\r'||*nx=='\n') nx++; }
        str_rtrim( l, " \t" ) ; while( *l==' '||*l=='\t' ) l++ ; if( !*l ) continue ;
        if( strpbrk( l, "*?" ) || l[strlen(l)-1]=='/' ) { any_glob = 1 ; break ; } }
      sfree( scan ) ; }

    /* CASE 2: several NAMED files. If any already exist in dir, prompt
     * once - Overwrite / Keep both / Cancel (the shared themed 3-way box,
     * modeless; the transfers follow its answer, kx_getgo_exist_done). */
    if( !any_glob ) {
        strbuf *existing = strbuf_new() ; int nexist = 0 ;   /* every name, in full */
        char *scan = dupstr( line ) ; char *l = scan, *nx ;
        for( ; l && *l ; l = nx ) { nx = strpbrk( l, "\r\n" ) ; if( nx ){ *nx='\0'; nx++; while(*nx=='\r'||*nx=='\n') nx++; }
            str_rtrim( l, " \t" ) ; while( *l==' '||*l=='\t' ) l++ ; if( !*l ) continue ;
            const char *base = strrchr( l, '/' ) ; base = base ? base+1 : l ;
            char *cand = dupprintf( "%s\\%s", dir, base ) ;
            if( existfile( cand ) ) {
                put_fmt( existing, "%s\r\n", base ) ;
                nexist++ ;
            }
            sfree( cand ) ; }
        sfree( scan ) ;
        if( nexist > 0 ) {
            char *body = dupprintf( KT_XFER_FILES_EXIST_TEXT, dir, existing->s ) ;
            int ch ;
            strbuf_free( existing ) ; existing = NULL ;
            kx_getgo_box = kitty_confirm_modeless3( g->hwnd, KT_CAP_XFER_FILES_EXIST, body, NULL,
                        KT_XFER_BTN_OVERWRITE, KT_XFER_BTN_KEEPBOTH, KT_XFER_BTN_CANCEL_PLAIN,
                        kx_getgo_exist_done, g ) ;
            if( kx_getgo_box != NULL ) { sfree( body ) ; return ; }
            /* no modeless box here: the box as before */
            ch = kitty_confirm_box3( g->hwnd, KT_CAP_XFER_FILES_EXIST, body,
                        KT_XFER_BTN_OVERWRITE, KT_XFER_BTN_KEEPBOTH, KT_XFER_BTN_CANCEL_PLAIN ) ;
            sfree( body ) ;
            if( ch == 0 ) { kx_getgo_free( g ) ; return ; }   /* Cancel: nothing */
            g->keepboth = ( ch == 2 ) ;
        }
        if( existing ) strbuf_free( existing ) ;
    }
    kx_getgo_run( g ) ;
}

static void kx_getfile_go( HWND hwnd, char * clip ) {
    const char * line = clip ;
    kx_getgo_t * g ;

    if( kx_getgo_cur != NULL ) {
        /* one Get File at a time: its picker or question is open */
        if( kx_getgo_box != NULL && IsWindow( kx_getgo_box ) ) SetForegroundWindow( kx_getgo_box ) ;
        sfree( clip ) ; return ;
    }
    g = snew( kx_getgo_t ) ;
    memset( g, 0, sizeof(*g) ) ;
    g->hwnd = hwnd ;
    g->clip = clip ;
    g->defdir = kitty_xfer_download_dir_dup( conf ) ;
    kx_getgo_cur = g ;

    /* ONE named file (no interior newline, no wildcard, not a trailing-slash
     * folder): a Save-As dialog with the remote name filled in, so Windows' own
     * replace prompt guards an overwrite; kscp then writes to that exact path.
     * Several files, wildcards and folders keep the folder picker below. */
    if( !strpbrk( line, "\r\n" ) && !strpbrk( line, "*?" ) && line[0] && line[strlen(line)-1] != '/' ) {
        const char *base = strrchr( line, '/' ) ; base = base ? base + 1 : line ;
        g->save = dupprintf( "%s\\%s", g->defdir, base ) ;
        if( kitty_call_async( kx_getgo_saveas_run, kx_getgo_saved, g ) ) return ;
        kx_getgo_saveas_run( g ) ;    /* no thread for it: here, as before */
        kx_getgo_saved( g ) ;
        return ;
    }

    if( kitty_pick_async( hwnd, KITTY_PICK_FOLDER, g->defdir, KT_XFER_GETFILE_PICK_TITLE, NULL,
                          kx_getgo_folder, g ) != NULL )
        return ;
    /* no thread for the picker: on this one, as before */
    g->dir = kitty_pick_dir_dup( hwnd, g->defdir, KT_XFER_GETFILE_PICK_TITLE ) ;
    kx_getgo_folder( g, g->dir ) ;
}

/* ---- Get File: the clipboard check and the box (hknet/KiTTY#71) ----------
 *
 * Tools > Get File (kscp)..., and the [Shortcuts] getfile key (Ctrl+F4).
 *
 * ONE remote path on the clipboard is checked first: kscp -stat in the
 * background (no window; -batch, so it never waits for an answer; 3 s at
 * most). A regular file on the connection's host: a notice reports it and the
 * Save-As opens as Get File always did. Anything else opens the Get File box
 * with the reason on top and the paths to edit - nothing or several paths on
 * the clipboard, a local path, a folder, not found, the shell on another host,
 * no silent login, a kscp without -stat, no answer. Its OK takes the routes of
 * kx_getfile_go.
 *
 * MODELESS throughout: kscp runs in a worker thread whose result is posted to
 * a message-only window, and the box is modeless, so the session keeps
 * running while either waits. A modal loop would stall it (no output, no
 * keepalives).
 *
 * The check runs only when kscp can log in without a prompt: a shared
 * connection, a password the session holds (stored, or typed and accepted),
 * a key file or an agent. Otherwise there is no attempt: a -batch login that
 * fails still lands in the server's auth log, and fail2ban counts those. */
#define KX_GF_TIMEOUT_MS 3000
#define KX_GF_WM_DONE    ( WM_APP + 1 )
#define KX_GF_ACCENT     RGB( 25, 110, 45 )

typedef struct {
	HWND owner ;                  /* the terminal window that requested it */
	char *text ;                  /* the clipboard path, trimmed */
	char *remote ;                /* the same, joined to the OSC 7 directory */
	char *host ;                  /* the connection's host, for the texts */
	HANDLE proc, rd_out, rd_err ;
	int result ;                  /* GFS_*, or -1 = no answer in time */
	unsigned long long size ;
	char *err ;                   /* kscp's reason, NULL = none */
	int shared, retried ;         /* ran with -share; already ran again without */
} kx_gf_check_t ;

static HWND kx_gf_msgwin = NULL ;     /* takes the worker's result */
/* The "Checking ..." notice: shown while kscp runs - the sign that something
 * is happening (the terminal resets a busy cursor on every mouse move). The
 * "Found" notice replaces it; before the box it is closed. Its context is
 * this token, so only that notice is closed, never another one. */
static int kx_gf_checking_token ;
static void kx_gf_checking_closed( void *ctx, int clicked ) { (void)ctx ; (void)clicked ; }

/* Test builds with KITTY_NOTICE_TRACE=<file>: the notice's text, one line
 * appended in the format kitty_hostnotify.c writes - for a test harness, as
 * a notice's text is painted and cannot be read from outside. */
static void kx_gf_trace_notice( const char *text ) {
#ifdef KITTY_TEST_BUILD_LABEL
	const char *path = getenv( "KITTY_NOTICE_TRACE" ) ;
	FILE *f ;
	if( !path || !*path || ( f = fopen( path, "a" ) ) == NULL ) return ;
	fprintf( f, "notice accent=%06X text=%s\n", (unsigned)KX_GF_ACCENT, text ) ;
	fclose( f ) ;
#else
	(void)text ;
#endif
}
static HWND kx_gf_box = NULL ;        /* the Get File box, while it is open */
static int kx_gf_checking = 0 ;       /* a check is running */

static void kx_gf_check_free( kx_gf_check_t *c ) {
	if( !c ) return ;
	if( c->proc ) CloseHandle( c->proc ) ;
	if( c->rd_out ) CloseHandle( c->rd_out ) ;
	if( c->rd_err ) CloseHandle( c->rd_err ) ;
	sfree( c->text ) ; sfree( c->remote ) ;
	sfree( c->host ) ; sfree( c->err ) ;
	sfree( c ) ;
}

/* Whatever the pipe holds now, appended to buf, so kscp never blocks on a
 * full pipe. The reply is a line or two: past KX_GF_KEEP bytes the rest is
 * read and dropped, a bound on memory against a flood. */
#define KX_GF_KEEP ( 64 * 1024 )
static void kx_gf_drain( HANDLE rd, strbuf *buf ) {
	DWORD avail = 0, got = 0 ;
	char tmp[1024] ;
	while( PeekNamedPipe( rd, NULL, 0, NULL, &avail, NULL ) && avail > 0 ) {
		if( !ReadFile( rd, tmp, avail < sizeof(tmp) ? avail : (DWORD)sizeof(tmp), &got, NULL ) || got == 0 )
			break ;
		if( buf->len < KX_GF_KEEP ) put_data( buf, tmp, got ) ;
	}
}

/* The last line with text in it (kscp's reason on stderr), in full (sfree
 * it); NULL when there is none. */
static char *kx_gf_last_line_dup( const char *s ) {
	const char *e = s + strlen( s ), *b ;
	while( e > s && ( e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' ) ) e-- ;
	b = e ;
	while( b > s && b[-1] != '\n' && b[-1] != '\r' ) b-- ;
	return e > b ? dupprintf( "%.*s", (int)( e - b ), b ) : NULL ;
}

static DWORD WINAPI kx_gf_thread( LPVOID p ) {
	kx_gf_check_t *c = (kx_gf_check_t *)p ;
	strbuf *out = strbuf_new(), *err = strbuf_new() ;
	DWORD start = GetTickCount() ;
	for( ;; ) {
		DWORD w ;
		kx_gf_drain( c->rd_out, out ) ;
		kx_gf_drain( c->rd_err, err ) ;
		if( GetTickCount() - start >= KX_GF_TIMEOUT_MS ) {
			TerminateProcess( c->proc, 1 ) ;
			c->result = -1 ;
			break ;
		}
		w = WaitForSingleObject( c->proc, 50 ) ;
		if( w == WAIT_OBJECT_0 ) {
			kx_gf_drain( c->rd_out, out ) ;
			kx_gf_drain( c->rd_err, err ) ;
			/* the reason is part of the reply, so the reply's length holds it */
			sfree( c->err ) ;
			c->err = snewn( out->len + 1, char ) ;
			c->result = gf_parse_stat( out->s, &c->size, c->err, out->len + 1 ) ;
			if( !c->err[0] ) { sfree( c->err ) ; c->err = NULL ; }
			if( c->result == GFS_NONE ) { sfree( c->err ) ; c->err = kx_gf_last_line_dup( err->s ) ; }
			break ;
		}
		if( w == WAIT_FAILED ) { c->result = GFS_NONE ; break ; }
	}
	strbuf_free( out ) ; strbuf_free( err ) ;
	if( !kx_gf_msgwin || !PostMessage( kx_gf_msgwin, KX_GF_WM_DONE, 0, (LPARAM)c ) ) {
		kx_gf_checking = 0 ;
		kx_gf_check_free( c ) ;
	}
	return 0 ;
}

/* Can kscp log in on its own without a prompt: a password, a key file
 * or an agent? */
static int kx_gf_own_login( void ) {
	if( !kitty_pw_empty( conf, CONF_password ) ) return 1 ;
	if( kx_helper_keyfile( conf ) != NULL ) return 1 ;
	if( conf_get_bool( conf, CONF_tryagent ) && agent_exists() ) return 1 ;
	return 0 ;
}

/* Can kscp log in without prompting anybody? See the head comment. */
static int kx_gf_silent_login( void ) {
	if( conf_get_bool( conf, CONF_ssh_connection_sharing ) ) return 1 ;
	if( !kitty_pw_empty( conf, CONF_password ) ) return 1 ;
	if( kx_helper_keyfile( conf ) != NULL ) return 1 ;
	if( conf_get_bool( conf, CONF_tryagent ) && agent_exists() ) return 1 ;
	return 0 ;
}

/* Start kscp -stat for c->remote; 1 = running (the worker owns c), 0 = not
 * started (c->err gives the reason when there is one). */
static int kx_gf_start_check( kx_gf_check_t *c ) {
	strbuf *buffer, *tgt ;
	kx_kscp_line_t k ;
	SECURITY_ATTRIBUTES sa ;
	HANDLE wr_out = NULL, wr_err = NULL, hs[3], t ;
	STARTUPINFOA si ; PROCESS_INFORMATION pi ;
	int nh = 0, ok ;

	buffer = strbuf_new_nm() ;
	{ char *rfolder = kx_folder_of( c->remote ) ;
	  int ok = kx_kscp_line( buffer, &k, rfolder ) ;
	  sfree( rfolder ) ;
	  if( !ok ) { strbuf_free( buffer ) ; return 0 ; } }
	bcat( buffer, "-batch -stat " ) ;
	tgt = strbuf_new() ;
	kx_kscp_remote( tgt ) ;
	bcat( tgt, gf_stat_path_of( c->remote ) ) ;
	qcat( buffer, tgt->s ) ;
	strbuf_free( tgt ) ;
	debug_logevent_redacted2( "Run", buffer->s, k.pw_at, k.pw_len, k.px_at, k.px_len ) ;

	memset( &sa, 0, sizeof(sa) ) ; sa.nLength = sizeof(sa) ; sa.bInheritHandle = TRUE ;
	if( !CreatePipe( &c->rd_out, &wr_out, &sa, 0 ) || !CreatePipe( &c->rd_err, &wr_err, &sa, 0 ) ) {
		if( wr_out ) CloseHandle( wr_out ) ;
		if( k.pwmap ) CloseHandle( k.pwmap ) ;
		if( k.pwfiles ) kx_pwfiles_release( 1 ) ;
		strbuf_free( buffer ) ;
		return 0 ;
	}
	SetHandleInformation( c->rd_out, HANDLE_FLAG_INHERIT, 0 ) ;
	SetHandleInformation( c->rd_err, HANDLE_FLAG_INHERIT, 0 ) ;
	memset( &si, 0, sizeof(si) ) ; si.cb = sizeof(si) ;
	si.dwFlags = STARTF_USESTDHANDLES ;
	si.hStdOutput = wr_out ; si.hStdError = wr_err ; si.hStdInput = NULL ;
	memset( &pi, 0, sizeof(pi) ) ;
	hs[nh++] = wr_out ; hs[nh++] = wr_err ;
	if( k.pwmap ) hs[nh++] = k.pwmap ;
	chdir( InitialDirectory ) ;
	ok = kitty_helper_start_handles( PSCPPath, buffer->s, CREATE_NO_WINDOW, NULL, &si, &pi, hs, nh ) ;
	strbuf_free( buffer ) ;                     /* cleared: it may hold the password */
	CloseHandle( wr_out ) ; CloseHandle( wr_err ) ;
	if( k.pwmap ) CloseHandle( k.pwmap ) ;
	if( k.pwfiles ) kx_pwfiles_release( ok ? 0 : 1 ) ;   /* once kscp has had it */
	if( !ok ) return 0 ;
	CloseHandle( pi.hThread ) ;
	c->proc = pi.hProcess ;
	c->shared = k.shared ;
	t = CreateThread( NULL, 0, kx_gf_thread, c, 0, NULL ) ;
	if( !t ) {
		TerminateProcess( c->proc, 1 ) ;
		return 0 ;
	}
	CloseHandle( t ) ;
	return 1 ;
}

/* ---- the box ---- */
typedef struct {
	HWND owner ;
	char *why ;
	char *text ;
} kx_gf_box_t ;

/* Move the controls by dy and the window's foot with them. */
static void kx_gf_shift( HWND h, const int *ids, int n, int dy ) {
	RECT r, wr ;
	int i ;
	for( i = 0 ; i < n ; i++ ) {
		HWND c = GetDlgItem( h, ids[i] ) ;
		if( !c ) continue ;
		GetWindowRect( c, &r ) ; MapWindowPoints( NULL, h, (POINT*)&r, 2 ) ;
		MoveWindow( c, r.left, r.top + dy, r.right - r.left, r.bottom - r.top, TRUE ) ;
	}
	GetWindowRect( h, &wr ) ;
	SetWindowPos( h, NULL, 0, 0, wr.right - wr.left, ( wr.bottom - wr.top ) + dy,
	              SWP_NOMOVE | SWP_NOZORDER ) ;
}

/* The reason line: its text, its height, what is below moved along. */
static void kx_gf_set_why( HWND h, const char *why ) {
	static const int below[] = { IDC_GF_LABEL, IDC_GF_EDIT, IDC_GF_FROM, IDC_GF_FOLDERS,
	                             IDC_GF_TIP, IDOK, IDCANCEL } ;
	int dy ;
	SetDlgItemTextA( h, IDC_GF_WHY, why ? why : "" ) ;
	dy = kitty_fit_text( h, IDC_GF_WHY, why, 0 ) ;
	if( why && *why ) ShowWindow( GetDlgItem( h, IDC_GF_WHY ), SW_SHOW ) ;
	if( dy ) kx_gf_shift( h, below, sizeof(below)/sizeof(below[0]), dy ) ;
}

/* "\n" -> "\r\n" for the edit control; the caller frees. */
static char *kx_gf_crlf( const char *s ) {
	size_t n = 0 ; const char *p ; char *o, *q ;
	for( p = s ; *p ; p++ ) n += ( *p == '\n' && ( p == s || p[-1] != '\r' ) ) ? 2 : 1 ;
	o = snewn( n + 1, char ) ;
	for( p = s, q = o ; *p ; p++ ) {
		if( *p == '\n' && ( p == s || p[-1] != '\r' ) ) *q++ = '\r' ;
		*q++ = *p ;
	}
	*q = '\0' ;
	return o ;
}

static INT_PTR CALLBACK kx_gf_box_proc( HWND h, UINT msg, WPARAM wp, LPARAM lp ) {
	kx_gf_box_t *b = (kx_gf_box_t *)GetWindowLongPtr( h, DWLP_USER ) ;
	switch( msg ) {
	  case WM_INITDIALOG: {
		static const int below_from[] = { IDC_GF_FOLDERS, IDC_GF_TIP, IDOK, IDCANCEL } ;
		const char *cwd = kitty_current_dir() ;
		char *from = dupprintf( KT_GETFILE_BOX_FROM, ( cwd && cwd[0] ) ? cwd : KT_GETFILE_BOX_LOGINDIR ) ;
		char *text ;
		int dy ;
		b = (kx_gf_box_t *)lp ;
		SetWindowLongPtr( h, DWLP_USER, (LONG_PTR)b ) ;
		SetDlgItemTextA( h, IDC_GF_FROM, from ) ;
		dy = kitty_fit_text( h, IDC_GF_FROM, from, 0 ) ;
		if( dy ) kx_gf_shift( h, below_from, sizeof(below_from)/sizeof(below_from[0]), dy ) ;
		sfree( from ) ;
		kx_gf_set_why( h, b->why ) ;
		text = kx_gf_crlf( b->text ? b->text : "" ) ;
		SetDlgItemTextA( h, IDC_GF_EDIT, text ) ;
		sfree( text ) ;
		kitty_dialog_icon( h, b->owner ) ;
		kitty_centre_on_owner( h ) ;
		SetFocus( GetDlgItem( h, IDC_GF_EDIT ) ) ;
		SendDlgItemMessage( h, IDC_GF_EDIT, EM_SETSEL, 0, -1 ) ;
		return FALSE ;                 /* focus set here */
	  }
	  case WM_COMMAND:
		if( !b ) return FALSE ;
		if( LOWORD(wp) == IDOK ) {
			HWND e = GetDlgItem( h, IDC_GF_EDIT ) ;
			int n = GetWindowTextLengthA( e ) ;
			char *t = snewn( n + 1, char ), *trim = snewn( n + 1, char ) ;
			int cls ;
			char *th, *tu, *nw ;
			GetWindowTextA( e, t, n + 1 ) ;
			kx_gf_target( &th, &tu ) ;
			cls = gf_classify_at( t, trim, n + 1, th, tu ) ;
			sfree( t ) ;
			nw = kx_gf_why_not_path( cls, trim, th, tu ) ;
			sfree( th ) ; sfree( tu ) ;
			if( nw != NULL ) {
				/* not a path on the target: the reason, the box stays */
				kx_gf_set_why( h, nw ) ;
				sfree( nw ) ; sfree( trim ) ;
				return TRUE ;
			}
			if( cls == GF_WINPATH ) {
				/* a local path typed or left in: report it, the box stays */
				kx_gf_set_why( h, KT_GETFILE_WHY_WINPATH ) ;
				sfree( trim ) ;
				return TRUE ;
			}
			{	HWND owner = b->owner ;
				DestroyWindow( h ) ;           /* frees b */
				if( cls != GF_EMPTY && IsWindow( owner ) ) kx_getfile_go( owner, trim ) ;
				else sfree( trim ) ;
			}
			return TRUE ;
		}
		if( LOWORD(wp) == IDCANCEL ) { DestroyWindow( h ) ; return TRUE ; }
		return FALSE ;
	  case WM_CLOSE:
		DestroyWindow( h ) ;
		return TRUE ;
	  case WM_DESTROY:
		ShinyRemoveAuxDialog( h ) ;
		if( b ) {
			SetWindowLongPtr( h, DWLP_USER, 0 ) ;
			sfree( b->why ) ; sfree( b->text ) ; sfree( b ) ;
		}
		if( kx_gf_box == h ) kx_gf_box = NULL ;
		return FALSE ;
	}
	return FALSE ;
}

/* why and text: dupstr'd, the box's from here on. */
static void kx_gf_open_box( HWND owner, char *why, char *text ) {
	kx_gf_box_t *b = snew( kx_gf_box_t ) ;
	HWND h ;
	b->owner = owner ; b->why = why ; b->text = text ;
	h = CreateDialogParamA( GetModuleHandle( NULL ), MAKEINTRESOURCEA( IDD_GETFILEBOX ), owner,
	                        kx_gf_box_proc, (LPARAM)b ) ;
	if( !h ) {
		/* no box: the reason at least, never nothing */
		MessageBox( owner, why, KT_GETFILE_BOX_CAP, MB_OK|MB_ICONINFORMATION ) ;
		sfree( why ) ; sfree( text ) ; sfree( b ) ;
		return ;
	}
	kx_gf_box = h ;
	ShinyAddAuxDialog( h ) ;
	ShowWindow( h, SW_SHOW ) ;
	SetForegroundWindow( h ) ;
}

/* The worker's result, on the window's thread. */
static void kx_gf_checked( kx_gf_check_t *c ) {
	char *why = NULL, *msg ;
	char size[32] ;
	/* the shared connection refused the check another channel: once more on
	 * kscp's own login, where it needs nobody to answer anything */
	if( c->shared && !c->retried && c->result == GFS_NONE && c->err && strstr( c->err, KX_REFUSED_CHANNEL )
	    && kx_gf_own_login() && IsWindow( c->owner ) ) {
		int ok ;
		c->retried = 1 ;
		if( c->proc ) { CloseHandle( c->proc ) ; c->proc = NULL ; }
		if( c->rd_out ) { CloseHandle( c->rd_out ) ; c->rd_out = NULL ; }
		if( c->rd_err ) { CloseHandle( c->rd_err ) ; c->rd_err = NULL ; }
		c->result = GFS_NONE ; c->size = 0 ; sfree( c->err ) ; c->err = NULL ;
		kx_noshare = 1 ;
		ok = kx_gf_start_check( c ) ;
		kx_noshare = 0 ;
		if( ok ) return ;          /* the worker has it again; still checking */
	}
	kx_gf_checking = 0 ;
	if( c->result != GFS_FILE ) kitty_notice_close_ctx( &kx_gf_checking_token ) ;
	if( !IsWindow( c->owner ) ) { kx_gf_check_free( c ) ; return ; }
	switch( c->result ) {
	  case GFS_FILE:
		gf_size_text( c->size, size, sizeof(size) ) ;
		msg = dupprintf( KT_GETFILE_FOUND, c->host, c->remote, size ) ;
		kitty_notice_show( KT_GETFILE_FOUND_CAP, msg, KX_GF_ACCENT, 0, NULL, 0 ) ;
		kx_gf_trace_notice( msg ) ;
		sfree( msg ) ;
		kx_getfile_go( c->owner, dupstr( c->text ) ) ;
		kx_gf_check_free( c ) ;
		return ;
	  case GFS_DIR:
		why = dupprintf( KT_GETFILE_WHY_FOLDER, c->remote ) ;
		break ;
	  case GFS_MISSING:
		why = dupprintf( KT_GETFILE_WHY_MISSING, c->host, c->remote ) ;
		break ;
	  case GFS_OTHER:
		msg = dupprintf( KT_GETFILE_NOT_REGULAR, c->remote ) ;
		why = dupprintf( KT_GETFILE_WHY_ERROR, msg ) ;
		sfree( msg ) ;
		break ;
	  case -1:
		why = dupprintf( KT_GETFILE_WHY_TIMEOUT, c->host ) ;
		break ;
	  default:   /* GFS_ERROR, GFS_NONE */
		why = dupprintf( KT_GETFILE_WHY_ERROR, ( c->err && c->err[0] ) ? c->err : KT_GETFILE_NO_ANSWER ) ;
		break ;
	}
	kx_gf_open_box( c->owner, why, dupstr( c->text ) ) ;
	kx_gf_check_free( c ) ;
}

static LRESULT CALLBACK kx_gf_wndproc( HWND h, UINT msg, WPARAM wp, LPARAM lp ) {
	if( msg == KX_GF_WM_DONE ) { kx_gf_checked( (kx_gf_check_t *)lp ) ; return 0 ; }
	return DefWindowProc( h, msg, wp, lp ) ;
}

static HWND kx_gf_window( void ) {
	if( !kx_gf_msgwin ) {
		WNDCLASSA wc ;
		memset( &wc, 0, sizeof(wc) ) ;
		wc.lpfnWndProc = kx_gf_wndproc ;
		wc.hInstance = GetModuleHandle( NULL ) ;
		wc.lpszClassName = "KiTTYgetfilemsg" ;
		RegisterClassA( &wc ) ;   /* already registered = fine */
		kx_gf_msgwin = CreateWindowExA( 0, wc.lpszClassName, "", 0, 0, 0, 0, 0,
		                                HWND_MESSAGE, NULL, wc.hInstance, NULL ) ;
	}
	return kx_gf_msgwin ;
}

void GetFile( HWND hwnd ) {
	char * clip = NULL, * text, * why = NULL ;
	const char * where = NULL ;
	int cls ;

	if( conf_get_int(conf,CONF_protocol) != PROT_SSH ) {
		kx_report( KT_GETFILE_BOX_CAP, KT_MSG_SSH_ONLY ) ;
		return ;
	}
	if( !kitty_xfer_tool_ready( 0 ) ) {
		kx_report( KT_GETFILE_BOX_CAP, KT_XFER_KSCP_NOT_FOUND ) ;
		debug_logevent( "%s", KT_XFER_KSCP_NOT_FOUND ) ;
		return ;
	}
	if( !kitty_helper_ready( hwnd, PSCPPath, 1, 0, kx_again_getfile, kx_again( hwnd ), kx_again_free ) )
		return ;
	/* one at a time: a second Ctrl+F4 brings the box back, or waits for the check */
	if( kx_gf_box && IsWindow( kx_gf_box ) ) { SetForegroundWindow( kx_gf_box ) ; return ; }
	if( kx_gf_checking ) return ;

	/* Take a private copy of the clipboard text: nothing below may hold the
	 * clipboard open. */
	if( IsClipboardFormatAvailable(CF_TEXT) && OpenClipboard(NULL) ) {
		HGLOBAL hglb = GetClipboardData( CF_TEXT ) ;
		if( hglb != NULL ) {
			char * pst = GlobalLock( hglb ) ;
			if( pst != NULL ) { clip = dupstr( pst ) ; GlobalUnlock( hglb ) ; }
		}
		CloseClipboard() ;
	}
	if( clip == NULL ) clip = dupstr( "" ) ;
	text = snewn( strlen( clip ) + 1, char ) ;
	{	char *th, *tu ;
		kx_gf_target( &th, &tu ) ;
		cls = gf_classify_at( clip, text, strlen( clip ) + 1, th, tu ) ;
		why = kx_gf_why_not_path( cls, text, th, tu ) ;
		sfree( th ) ; sfree( tu ) ;
	}
	sfree( clip ) ;

	if( !why ) switch( cls ) {
	  case GF_EMPTY:
		why = dupstr( KT_GETFILE_WHY_EMPTY ) ;
		break ;
	  case GF_WINPATH:
		why = dupstr( KT_GETFILE_WHY_WINPATH ) ;
		text[0] = '\0' ;               /* a local path is no start for a remote one */
		break ;
	  case GF_SEVERAL:
		why = dupstr( KT_GETFILE_WHY_SEVERAL ) ;
		break ;
	  case GF_FOLDER:
		why = dupprintf( KT_GETFILE_WHY_FOLDER, text ) ;
		break ;
	  default: {   /* GF_ONE: query the server */
		const char * host = conf_get_str( conf, CONF_host ) ;
		if( kitty_shell_moved( hwnd, &where ) ) {
			why = where ? dupprintf( KT_GETFILE_WHY_MOVED, where, host )
			            : dupprintf( KT_GETFILE_WHY_MOVED_UNNAMED, host ) ;
		} else if( !kx_helper_has_flag( PSCPPath, "KiTTYStat" ) ) {
			why = dupstr( KT_GETFILE_WHY_OLDKSCP ) ;
		} else if( !kx_gf_silent_login() ) {
			why = dupstr( KT_GETFILE_WHY_NOLOGIN ) ;
		} else if( kx_gf_window() ) {
			kx_gf_check_t * c = snew( kx_gf_check_t ) ;
			const char * cwd = kitty_current_dir() ;
			memset( c, 0, sizeof(*c) ) ;
			c->owner = hwnd ;
			c->text = text ;
			c->remote = ( text[0] == '/' || text[0] == '~' || cwd == NULL )
			            ? dupstr( text ) : dupprintf( "%s/%s", cwd, text ) ;
			c->host = dupstr( host ) ;
			kx_gf_checking = 1 ;
			char * msg = dupprintf( KT_GETFILE_CHECKING, host, c->remote ) ;   /* before c is the worker's */
			if( kx_gf_start_check( c ) ) {          /* the worker takes it from here */
				kitty_notice_show_ex( KT_GETFILE_FOUND_CAP, msg, KX_GF_ACCENT,
				                      ( KX_GF_TIMEOUT_MS / 1000 ) + 2, NULL, 0,
				                      kx_gf_checking_closed, &kx_gf_checking_token ) ;
				kx_gf_trace_notice( msg ) ;
				sfree( msg ) ;
				return ;
			}
			sfree( msg ) ;
			kx_gf_checking = 0 ;
			why = dupprintf( KT_GETFILE_WHY_ERROR, ( c->err && c->err[0] ) ? c->err : KT_GETFILE_NO_ANSWER ) ;
			c->text = NULL ;               /* still ours, for the box */
			kx_gf_check_free( c ) ;
		} else {
			why = dupprintf( KT_GETFILE_WHY_ERROR, KT_GETFILE_NO_ANSWER ) ;
		}
		break ;
	  }
	}
	kx_gf_open_box( hwnd, why, text ) ;
}

// Start a local command (Internet Explorer for example)
/* #9 hardening: RunCmd() runs text taken straight from the Windows clipboard as
 * a local command (default Ctrl+F5), so attacker-planted clipboard content could
 * execute on a single keypress. Two independent, PER-SESSION safeguards, both ON
 * by default and each toggled in the config dialog (Window/Copy & Paste): a
 * confirmation prompt (CONF_runcmdconfirm) and a post-launch tray balloon
 * (CONF_runcmdnotify, mirroring kageant's key-use balloon). */
typedef struct { HWND hwnd ; char *cmd ; } kx_runcmd_t ;   /* the command in full */
static HWND kx_runcmd_box = NULL ;   /* one confirmation at a time */

/* Run the confirmed (or unconfirmed) clipboard command. */
static void kx_runcmd_go( HWND hwnd, char *cmd ) {
    STARTUPINFO si ;
    PROCESS_INFORMATION pi ;
    chdir( InitialDirectory ) ;
    ZeroMemory( &si, sizeof(si) ) ;
    si.cb = sizeof(si) ;
    ZeroMemory( &pi, sizeof(pi) ) ;
    if( CreateProcess( NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi ) ) {
        CloseHandle( pi.hThread ) ; CloseHandle( pi.hProcess ) ;
    } else
        ShellExecute( hwnd, "open", cmd, 0, 0, SW_SHOWDEFAULT ) ;
    if( conf_get_bool( conf, CONF_runcmdnotify ) ) {
        char *note = dupprintf( KT_XFER_RAN_CLIP, cmd ) ;
        kitty_tray_balloon_async( KT_CAP_KITTY, note, NULL ) ;
        sfree( note ) ;
    }
}
static void kx_runcmd_answer( int yes, void *vp ) {
    kx_runcmd_t *r = (kx_runcmd_t *)vp ;
    kx_runcmd_box = NULL ;
    if( yes ) kx_runcmd_go( r->hwnd, r->cmd ) ;
    smemclr( r->cmd, strlen( r->cmd ) ) ;
    sfree( r->cmd ) ;
    sfree( r ) ;
}

/* The clipboard text is taken in full: a cut copy ran a different command
 * than the one on the clipboard. One longer than Windows starts fails to
 * start. */
void RunCmd( HWND hwnd ) {
    char *buffer = NULL, * pst = NULL ;
    if (!IsClipboardFormatAvailable(CF_TEXT)) return ;
    if( OpenClipboard(NULL) ) {
        HGLOBAL hglb ;

        if( (hglb = GetClipboardData( CF_TEXT ) ) != NULL ) {
            if( ( pst = GlobalLock( hglb ) ) != NULL ) {
                buffer = dupstr( pst ) ;
                GlobalUnlock( hglb ) ;
            }
        }
        CloseClipboard();
    }
    if( buffer != NULL && strlen( buffer ) > 0 ) {
        if( conf_get_bool( conf, CONF_runcmdconfirm ) ) {
            /* modeless: the terminal's output keeps arriving while it waits;
             * the command is in the read-only field, the run in the answer */
            kx_runcmd_t *r ;
            if( kx_runcmd_box && IsWindow( kx_runcmd_box ) ) {
                SetForegroundWindow( kx_runcmd_box ) ;
                smemclr( buffer, strlen( buffer ) ) ; sfree( buffer ) ;
                return ;
            }
            r = snew( kx_runcmd_t ) ;
            r->hwnd = hwnd ;
            r->cmd = buffer ;                  /* the box's from here on */
            kx_runcmd_box = kitty_confirm_modeless( hwnd, KT_CAP_RUN_CLIP_CMD,
                KT_XFER_RUN_CLIP_PROMPT, r->cmd, NULL, kx_runcmd_answer, r ) ;
            if( !kx_runcmd_box ) { smemclr( r->cmd, strlen( r->cmd ) ) ; sfree( r->cmd ) ; sfree( r ) ; }   /* not made: a No */
            return ;
        }
        kx_runcmd_go( hwnd, buffer ) ;
    }
    if( buffer != NULL ) { smemclr( buffer, strlen( buffer ) ) ; sfree( buffer ) ; }
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

/* If `candidate` refers to an existing file, adopt it as the tool path *out
 * (freeing any previous value), optionally export it to environment `envname`,
 * optionally persist it under kitty.ini/registry key `param`, and return 1;
 * otherwise leave *out untouched and return 0. Pass param=NULL when the value
 * came from `param` itself (no need to write it back) and envname=NULL when the
 * tool has no associated environment variable. Generalises the former
 * per-tool malloc+strcpy+WriteParameter tails in the Search* helpers. */
/* KiTTY: %VAR% expansion (cyd01/KiTTY#472) - `candidate` may be the literal
 * text stored in kitty.ini (e.g. "%OneDrive%\pscp.exe"); expand it before
 * the existfile() check so a %VAR% helper path is actually found, and adopt
 * the EXPANDED path as the running value. None of this function's callers
 * pass a non-NULL `param`, so the ini/registry text itself is never
 * overwritten with the expansion. */
static int adopt_tool_path_if_exists( char **out, const char *candidate,
                                      const char *param, const char *envname ) {
	char *expanded ;
	if( candidate == NULL ) return 0 ;
	expanded = kitty_expand_env_dup( candidate ) ;
	if( !existfile( expanded ) ) { free( expanded ) ; return 0 ; }
	if( *out != NULL ) free( *out ) ;
	*out = expanded ;
	if( envname != NULL ) set_env( (char*)envname, *out ) ;
	if( param != NULL ) WriteParameter( INIT_SECTION, (char*)param, *out ) ;
	return 1 ;
}

static int set_winscp_path_if_exists(const char *path)
{
	/* KiTTY: %VAR% expansion (cyd01/KiTTY#472) - see
	 * adopt_tool_path_if_exists() above. A path found here is used for this
	 * run and NOT written to the store, as for FileZilla and kscp: WinSCP
	 * moved, or a drive not there today, must not rewrite the user's
	 * setting. Application > KiTTY++ Settings > Transfers & Tools > WinSCP
	 * shows the found path as a hint while nothing is stored
	 * (kitty_config_app.c kitty_toolpath_hint). */
	char *expanded ;
	if( path == NULL || !path[0] ) return 0 ;
	expanded = kitty_expand_env_dup( path ) ;
	if( existfile(expanded) ) {
		if( WinSCPPath != NULL ) free( WinSCPPath ) ;
		WinSCPPath = expanded ;
		return 1 ;
	}
	free( expanded ) ;
	return 0 ;
}

static int probe_winscp_env_dir(const char *envname, const char *subpath)
{
	const char *base = getenv(envname) ;
	char *path ;
	int found ;
	if( base == NULL || base[0] == '\0' ) return 0 ;
	path = dupprintf( "%s\\%s", base, subpath ) ;
	found = set_winscp_path_if_exists( path ) ;
	sfree( path ) ;
	return found ;
}

/* A stored tool setting tried with `try` (1 = found and adopted): the
 * value as it is, or with `name` appended when it is a folder. In full. */
static int kx_try_stored( const char *key, const char *name, int (*try)( const char *path ) ) {
	char *v = ReadParameterDup( INIT_SECTION, key ), *path ;
	int found = 0 ;
	if( v[0] ) {
		path = name ? dupprintf( "%s\\%s", v, name ) : dupstr( v ) ;
		found = try( path ) ;
		sfree( path ) ;
	}
	sfree( v ) ;
	return found ;
}

/* <folder>\<name> tried with `try`. */
static int kx_try_in( const char *folder, const char *name, int (*try)( const char *path ) ) {
	char *path = dupprintf( "%s\\%s", folder, name ) ;
	int found = try( path ) ;
	sfree( path ) ;
	return found ;
}

static int kx_adopt_winscp( const char *path ) {
	return adopt_tool_path_if_exists( &WinSCPPath, path, NULL, NULL ) ;
}

// Look for the path to the WinSCP program
int SearchWinSCP( void ) {
	char *found ;
	if( WinSCPPath!=NULL) { free(WinSCPPath) ; WinSCPPath = NULL ; }
	/* A stored path whose file is missing stays stored (here and for FileZilla
	 * and kscp): the drive may only be unmounted today, and the setting is
	 * the user's - the search below covers this run only. */
	if( kx_try_stored( KI_WINSCPPATH, NULL, kx_adopt_winscp ) ) return 1 ;
	/* ProgramW6432 as well: a 32-bit process gets ProgramFiles = the x86
	 * folder, and a 64-bit WinSCP is under ProgramW6432 (in a 64-bit process
	 * Windows sets ProgramFiles from it, so the probe repeats harmlessly). */
	if( probe_winscp_env_dir("ProgramFiles", "WinSCP\\WinSCP.exe") ) return 1 ;
	if( probe_winscp_env_dir("ProgramW6432", "WinSCP\\WinSCP.exe") ) return 1 ;
	if( probe_winscp_env_dir("ProgramFiles(x86)", "WinSCP\\WinSCP.exe") ) return 1 ;
	if( probe_winscp_env_dir("LOCALAPPDATA", "Programs\\WinSCP\\WinSCP.exe") ) return 1 ;
	if( probe_winscp_env_dir("ProgramFiles", "WinSCP3\\WinSCP3.exe") ) return 1 ;
	if( probe_winscp_env_dir("ProgramFiles(x86)", "WinSCP3\\WinSCP3.exe") ) return 1 ;
	if( kx_try_in( InitialDirectory, "WinSCP.exe", set_winscp_path_if_exists ) ) return 1 ;
	if( kx_try_stored( KI_WINSCPDIR, "WinSCP.exe", set_winscp_path_if_exists ) ) return 1 ;
	if( ( found = kx_search_path_dup( "WinSCP.exe" ) ) != NULL ) {
		int ok = set_winscp_path_if_exists( found ) ;
		sfree( found ) ;
		if( ok ) return 1 ;
	}
	return 0 ;
}

/*
 * "Start ... anyway?" for WinSCP and FileZilla: the session's key is
 * Hello-protected and the agent does not hold it, so the tool can only fail
 * to log in with it. The question is a modeless box - a modal one stopped the
 * session behind it until it was answered - and its Yes starts the tool again
 * past the question. It comes before the command line is built: no password
 * file, no temporary FileZilla directory and no password in memory waits on
 * the answer. One question at a time; a second start brings it forward.
 */
static HWND kx_anyway_box = NULL ;
static int kx_anyway_yes = 0 ;     /* set for the one start a Yes makes */

/* 1 = start now; 0 = not now: a question stands (its Yes starts again) or
 * the answer was No. g (what the start needs again) is freed here or by the
 * question. */
static int kx_anyway( HWND hwnd, const char *fmt, void (*done)( int yes, void *ctx ), kx_again_t *g ) {
	char *note, *text ;
	int go ;
	if( kx_anyway_yes ) { kx_anyway_yes = 0 ; kx_again_free( g ) ; return 1 ; }
	if( kx_anyway_box && IsWindow( kx_anyway_box ) ) {
		SetForegroundWindow( kx_anyway_box ) ;
		kx_again_free( g ) ;
		return 0 ;
	}
	note = kx_hello_agent_note( conf ) ;
	if( note == NULL ) { kx_again_free( g ) ; return 1 ; }
	text = dupprintf( fmt, note ) ;
	sfree( note ) ;
	kx_anyway_box = kitty_confirm_modeless( hwnd, KT_CAP_KEY_NOT_IN_AGENT, text, NULL, NULL, done, g ) ;
	if( kx_anyway_box ) { sfree( text ) ; return 0 ; }
	kx_again_free( g ) ;               /* no modeless box here: the box as before */
	go = MessageBox( hwnd, text, KT_CAP_KEY_NOT_IN_AGENT, MB_OKCANCEL|MB_ICONWARNING ) ;
	sfree( text ) ;
	return go == IDOK ;
}

static void kx_anyway_winscp_done( int yes, void *v ) {
	kx_again_t *g = (kx_again_t *)v ;
	kx_anyway_box = NULL ;
	if( yes && IsWindow( g->hwnd ) ) {
		kx_anyway_yes = 1 ;
		StartWinSCP( g->hwnd, g->a, g->b, g->c ) ;
		kx_anyway_yes = 0 ;        /* a start that returned before the question */
	}
	kx_again_free( g ) ;
}

static void kx_anyway_filezilla_done( int yes, void *v ) {
	kx_again_t *g = (kx_again_t *)v ;
	kx_anyway_box = NULL ;
	if( yes && IsWindow( g->hwnd ) ) {
		kx_anyway_yes = 1 ;
		StartFileZilla( g->hwnd ) ;
		kx_anyway_yes = 0 ;
	}
	kx_again_free( g ) ;
}

// Start WinSCP from the current session, optionally in the current directory
/* UNIX ALIAS TO DEFINE FOR STARTING WINSCP in the current directory
winscp()
{
echo "\033]0;__ws:"`pwd`"\007"
}
Then it is enough to type: winscp
(history: once handled by ManageLocalCmd, removed in 0.84.1.37 - see the
security note above)

The path to the WinSCP executable is defined in the WInSCPPath variable. It
can point at a .BAT file, to pass extra options.
@ECHO OFF
start "C:\Program Files\WinSCP\WinSCP.exe" "%1" "%2" "%3" "%4" "%5" "%6" "%7" "%8" "%9"
*/	
// winscp.exe [(sftp|ftp|scp)://][user[:password]@]host[:port][/path/[file]] [/privatekey=key_file] [/rawsettings (ProxyMethod=1) (Compression=1)]
void StartWinSCP( HWND hwnd, char * directory, char * host, char * user ) {
	size_t pw_at = 0, pw_len = 0 ;   /* KiTTY: where the password lands in cmd */
	size_t proxy_pw_at = 0, proxy_pw_len = 0 ;  /* ... and the proxy/tunnel one */
	/* The line in a strbuf: a fixed buffer cut it, and what came after the
	 * cut - the proxy's /rawsettings, /passwordsfromfiles - was lost. */
	strbuf *cmd ;
	char *shortpath ;
	const char *proto ;
	int winscpport ;
	int raw = 0;
	int pwfiles = 0 ;                /* passwords handed over as files (/passwordsfromfiles) */
	const char *pf ;

	kx_pwfiles_sweep() ;             /* anything an earlier start left behind */
	if( directory == NULL ) { directory = kitty_current_dir(); }
	/* The stored path gone (WinSCP uninstalled, moved, another drive): the
	 * search runs again, and what it finds is used for this run. Nothing
	 * found: reported in the notice window and the Event Log, as for
	 * FileZilla and kscp - this returned silently before. */
	if( !kitty_xfer_tool_ready( 1 ) ) {
		kx_report( KT_BEHAVIOUR_TOOLS_WINSCP, KT_XFER_WINSCP_NOT_FOUND ) ;
		debug_logevent( "%s", KT_XFER_WINSCP_NOT_FOUND ) ;
		return ;
	}
#if KX_CHECK_SELF_UPDATING_TOOLS
	/* checked before any password file exists; a Yes starts WinSCP again */
	{ kx_again_t *g = kx_again( hwnd ) ;
	  g->a = directory ? dupstr( directory ) : NULL ;
	  g->b = host ? dupstr( host ) : NULL ;
	  g->c = user ? dupstr( user ) : NULL ;
	  if( !kitty_helper_ready( hwnd, WinSCPPath, 1, 0, kx_again_winscp, g, kx_again_free ) ) return ;
	}
#endif
	/* the key the agent does not hold: "Start WinSCP anyway?" first */
	{ kx_again_t *g = kx_again( hwnd ) ;
	  g->a = directory ? dupstr( directory ) : NULL ;
	  g->b = host ? dupstr( host ) : NULL ;
	  g->c = user ? dupstr( user ) : NULL ;
	  if( !kx_anyway( hwnd, KT_XFER_START_WINSCP_ANYWAY, kx_anyway_winscp_done, g ) ) return ;
	}

	if( ( shortpath = kx_short_path_dup( WinSCPPath ) ) == NULL ) return ;

	switch( conf_get_int(conf, CONF_winscpprot) ) {
		case 0: proto = "scp" ; break ;
		case 2: proto = "ftp" ; break ;
		case 3: proto = "ftps" ; break ;
		case 4: proto = "ftpes" ; break ;
		case 5: proto = "http" ; break ;
		case 6: proto = "https" ; break ;
		default: proto = "sftp" ;
	}

	/* A port written into the target override wins; then the WinSCP panel's own
	 * Port field; then the protocol's standard port. An override without a port
	 * supplies user and host only. kitty_xfer_default_port() gives the same, so
	 * the panel's hint and this agree. */
	{ int wport = kx_override_port( conf_get_str(conf, CONF_sftpconnect) ) ;
	  if( wport <= 0 ) wport = kitty_xfer_port_field( conf_get_str(conf, CONF_winscp_port) ) ;
	  if( wport <= 0 ) wport = kitty_xfer_default_port( conf, 1, conf_get_int(conf, CONF_winscpprot) ) ;
	  winscpport = wport ;
	}

	cmd = strbuf_new_nm() ;                /* cleared when it goes: it may hold a password */
	if( conf_get_int(conf,CONF_protocol) == PROT_SSH ) {
		put_fmt( cmd, "\"%s\" %s://", shortpath, proto ) ;

		if( strlen( conf_get_str(conf, CONF_sftpconnect) ) > 0 ) {
			bcat( cmd, conf_get_str(conf, CONF_sftpconnect) ) ;
			/* The override gives user and host; when it gives no port, the
			 * Port field (or the standard port) supplies one. */
			if( kx_override_port( conf_get_str(conf, CONF_sftpconnect) ) <= 0 ) {
				put_fmt( cmd, ":%d", winscpport ) ;
			}
		} else {
			urlcat( cmd, user!=NULL ? user : conf_get_str_ambi(conf,CONF_username,NULL) ) ;
			if( !kitty_pw_empty(conf,CONF_password) ) {
				/* The URL's password field carries the PATH of a private temp
				 * file holding the password (/passwordsfromfiles, see above);
				 * only if no file could be made does the value itself go
				 * (#535: percent-encoded so '@' '/' etc. can't redirect the host).
				 * WinSCP's file format is its own - a plain password - so this
				 * one is NOT the protected form the kscp file uses. */
				char pw[KITTY_PW_MAX+1] ;
				kitty_pw_get( conf, CONF_password, pw, sizeof(pw) ) ;
				pf = kx_password_file( pw ) ;
				bcat( cmd, ":" ) ;
				pw_at = cmd->len ;
				if( pf ) { urlcat( cmd, pf ) ; pwfiles++ ; }
				else urlcat( cmd, pw ) ;
				pw_len = cmd->len - pw_at ;
				smemclr( pw, sizeof(pw) ) ;
			}
			bcat( cmd, "@" ) ;
			if( poss( ":", host!=NULL ? host : conf_get_str(conf,CONF_host) )>0 ) { bcat(cmd,"[") ; bcat(cmd, host!=NULL ? host : conf_get_str(conf,CONF_host)) ; bcat(cmd,"]") ; }
			else { bcat( cmd, host!=NULL ? host : conf_get_str(conf,CONF_host) ) ; }
			put_fmt( cmd, ":%d", winscpport ) ;
		}

		if( directory!=NULL ) if( strlen(directory)>0 ) {
			pathcat( cmd, directory ) ;   /* a space as %20: one argument */
			if( directory[strlen(directory)-1]!='/' ) bcat( cmd, "/" ) ;
		}
		{ const char *kf = kx_helper_keyfile(conf) ;
		  if( kf != NULL ) {
			char *kshort = kx_short_path_dup( kf ) ;
			if( kshort != NULL ) {
				bcat( cmd, " \"/privatekey=" ) ;
				bcat( cmd, kshort ) ;
				bcat( cmd, "\"" ) ;
				sfree( kshort ) ;
			}
		  }
		}
	} else {
		put_fmt( cmd, "\"%s\" %s://", shortpath, proto ) ;
		urlcat( cmd, conf_get_str_ambi(conf,CONF_username,NULL) ) ; /* #535: percent-encode userinfo */
		if( !kitty_pw_empty(conf,CONF_password) ) {
			char pw[KITTY_PW_MAX+1] ;
			kitty_pw_get( conf, CONF_password, pw, sizeof(pw) ) ;
			bcat( cmd, ":" ) ;
			pw_at = cmd->len ;
			pf = kx_password_file( pw ) ;   /* a file, see above */
			if( pf ) { urlcat( cmd, pf ) ; pwfiles++ ; }
			else urlcat( cmd, pw ) ;
			pw_len = cmd->len - pw_at ;
			smemclr( pw, sizeof(pw) ) ;
		}
		bcat( cmd, "@" ) ;
		if( poss( ":", conf_get_str(conf,CONF_host) )>0 ) { bcat( cmd, "[" ) ; bcat( cmd, conf_get_str(conf,CONF_host) ) ; bcat( cmd, "]" ) ; }
		else { bcat( cmd, conf_get_str(conf,CONF_host) ) ; }
		put_fmt( cmd, ":%d", winscpport ) ;
		if( directory!=NULL ) if( strlen(directory)>0 ) {
			pathcat( cmd, directory ) ;   /* a space as %20: one argument */
			if( directory[strlen(directory)-1]!='/' ) bcat( cmd, "/" ) ;
		}
	}
	sfree( shortpath ) ;

	if( strlen(conf_get_str(conf, CONF_winscpoptions))>0 ) {
		bcat( cmd, " " ) ; bcat( cmd, conf_get_str(conf, CONF_winscpoptions) ) ;
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
	 * whatever the session holds - usually no proxy at all, and then WinSCP tries
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
	const char * px_tcmd = px ? px->telnet_command : conf_get_str(conf,CONF_proxy_telnet_command) ;
	/* Both sources hold the password WRAPPED (kitty_pwmem.c) - the snapshot
	 * keeps whatever the Conf held - so it is unwrapped once here and this
	 * buffer is burned at the end of the block. */
	char px_pass[KITTY_PW_MAX+1] ;
	kitty_pw_unwrap_str( px ? px->password : conf_get_str(conf,CONF_proxy_password),
	                     px_pass, sizeof(px_pass) ) ;

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
			if( raw == 0 ) { bcat( cmd, " /rawsettings" ) ; raw++ ; }
			bcat( cmd, " Tunnel=1" ) ;
			if( px_host && strlen(px_host)>0 ) { bcat( cmd, " TunnelHostName=" ) ; rawcat( cmd, px_host ) ; }
			put_fmt( cmd, " TunnelPortNumber=%d", px_port ) ;
			if( px_user && strlen(px_user)>0 ) { bcat( cmd, " TunnelUserName=" ) ; rawcat( cmd, px_user ) ; }
			if( px_pass[0] ) {
				/* TunnelPassword expects WinSCP's own encrypted form;
				 * TunnelPasswordPlain is the cleartext one. The span covers
				 * the quotes rawcat adds, so the Event Log blanks the whole
				 * value however it was escaped. */
				bcat( cmd, " TunnelPasswordPlain=" ) ;
				proxy_pw_at = cmd->len ;
				pf = kx_password_file( px_pass ) ;      /* a file - /passwordsfromfiles covers rawsettings too */
				if( pf ) { rawcat( cmd, pf ) ; pwfiles++ ; }
				else rawcat( cmd, px_pass ) ;
				proxy_pw_len = cmd->len - proxy_pw_at ;
			}
		} else if( method > 0 ) {
			if( raw == 0 ) { bcat( cmd, " /rawsettings" ) ; raw++ ; }
			put_fmt( cmd, " ProxyMethod=%d", method ) ;
			if( px_host && strlen(px_host)>0 ) { bcat( cmd, " ProxyHost=" ) ; rawcat( cmd, px_host ) ; }
			put_fmt( cmd, " ProxyPort=%d", px_port ) ;
			if( px_user && strlen(px_user)>0 ) { bcat( cmd, " ProxyUsername=" ) ; rawcat( cmd, px_user ) ; }
			if( px_pass[0] ) {
				bcat( cmd, " ProxyPassword=" ) ;
				proxy_pw_at = cmd->len ;
				pf = kx_password_file( px_pass ) ;      /* a file, see above */
				if( pf ) { rawcat( cmd, pf ) ; pwfiles++ ; }
				else rawcat( cmd, px_pass ) ;
				proxy_pw_len = cmd->len - proxy_pw_at ;
			}
			/* One PuTTY field, two WinSCP ones: CONF_proxy_telnet_command holds
			 * the Telnet negotiation string for PROXY_TELNET and the local
			 * command line for PROXY_CMD, and WinSCP keeps those apart. Sending
			 * a local command as ProxyTelnetCommand left WinSCP with nothing to
			 * run for method 5. */
			if( px_tcmd && strlen(px_tcmd)>0 ) {
				bcat( cmd, method==5 ? " ProxyLocalCommand=" : " ProxyTelnetCommand=" ) ;
				rawcat( cmd, px_tcmd ) ;
			}
		}
	}
	smemclr( px_pass, sizeof(px_pass) ) ;
	}

	if( conf_get_bool(conf,CONF_compression) ) {
		if( raw == 0 ) { bcat( cmd, " /rawsettings" ) ; raw++ ; }
		bcat( cmd, " Compression=1" ) ;
	}

	if( conf_get_bool(conf, CONF_agentfwd) ) {
		if( raw == 0 ) { bcat( cmd, " /rawsettings" ) ; raw++ ; }
		bcat( cmd, " AgentFwd=1" ) ;
	}

	if( strlen(conf_get_str(conf, CONF_winscprawsettings))>0 ) {
		if( raw == 0 ) { bcat( cmd, " /rawsettings" ) ; raw++ ; }
		bcat( cmd, " " ) ; bcat( cmd, conf_get_str(conf, CONF_winscprawsettings) ) ;
	}

	if( !strcmp(proto,"scp") && (strlen(conf_get_str(conf, CONF_pscpshell))>0) ) {
		if( raw == 0 ) { bcat( cmd, " /rawsettings" ) ; raw++ ; }
		bcat( cmd, " Shell=" ) ; rawcat( cmd, conf_get_str(conf, CONF_pscpshell) ) ;
	}

	/* Every password above went into a file: add the WinSCP switch. Without a
	 * password there is no switch and the command line is what it always was
	 * (key files and the agent are untouched by this). */
	if( pwfiles ) bcat( cmd, " /passwordsfromfiles" ) ;
	debug_logevent_redacted2( "Start WinSCP", cmd->s, pw_at, pw_len, proxy_pw_at, proxy_pw_len ) ;
#if KX_CHECK_SELF_UPDATING_TOOLS
	kitty_helper_run( hwnd, WinSCPPath, cmd->s ) ;
#else
	RunCommand( hwnd, cmd->s ) ;       /* not checked: KX_CHECK_SELF_UPDATING_TOOLS */
#endif
	strbuf_free( cmd ) ;
	kx_pwfiles_release( 0 ) ;
}

	

/* ---- FileZilla ---------------------------------------------------------------
 *
 * The same hand-off as WinSCP's, for FileZilla (the request behind it is
 * cyd01/KiTTY#501). FileZilla takes a URL on its command line -
 * [proto://][user[:password]@]host[:port][/path/] (the path: the shell's
 * folder from OSC 7 when tracking is on, as for WinSCP) - plus
 * --logontype=ask (it prompts for the password itself) and --site=<path> (a
 * Site Manager entry). It has no
 * /passwordsfromfiles, so the password question is the user's explicit choice
 * (CONF_filezilla_pwmode), each option with its consequence stated on the panel:
 *
 *   0  ask       - user@host:port and --logontype=ask; no secret leaves this
 *                  process. The default, and what a key-authenticated session
 *                  gets regardless (no password to hand over).
 *   1  temp cfg  - a private temporary directory holding an fzdefaults.xml
 *                  whose <Servers> block carries the credentials (the password
 *                  base64, which is how FileZilla stores it without its master
 *                  password). FileZilla finds that file through FZ_DATADIR (its
 *                  data directory, searched for fzdefaults.xml before the
 *                  program directory) and lists the entry among the predefined
 *                  sites, so it is started with --site="1/<name>" ("1/" =
 *                  predefined, "0/" = the user's own Site Manager). FileZilla's
 *                  own settings stay in force - fzdefaults.xml only ADDS. The
 *                  directory is deleted a minute after the start, like the
 *                  WinSCP password files. (FZ_DATADIR does NOT relocate the
 *                  settings directory; a sitemanager.xml placed there is never
 *                  read - "Site does not exist".)
 *   2  cmd line  - user:password@host:port, percent-encoded. Readable by every
 *                  process of the same user while FileZilla runs; offered
 *                  because it is the simplest, and the panel states it.
 *
 * A proxy is not handed over: FileZilla has no per-connection proxy setting,
 * only its global one. Protocol: the FileZilla panel's own Protocol setting
 * (CONF_filezilla_protocol), which offers only what FileZilla speaks - SFTP,
 * FTP, and the two FTPS forms. A session whose SFTP choice has no SSH
 * connection under it falls back to plain FTP. */
char * FileZillaPath = NULL ;

static int set_filezilla_path_if_exists( const char *path ) {
	/* KiTTY: %VAR% expansion (cyd01/KiTTY#472) - see
	 * adopt_tool_path_if_exists() above; this path is never written back to
	 * the store, so the ini text (literal or not) is untouched either way. */
	char *expanded ;
	if( path == NULL || !path[0] ) return 0 ;
	expanded = kitty_expand_env_dup( path ) ;
	if( existfile( expanded ) ) {
		if( FileZillaPath != NULL ) free( FileZillaPath ) ;
		FileZillaPath = expanded ;
		return 1 ;
	}
	free( expanded ) ;
	return 0 ;
}

static int probe_filezilla_env_dir( const char *envname, const char *subpath ) {
	const char *base = getenv( envname ) ;
	char *path ;
	int found ;
	if( base == NULL || base[0] == '\0' ) return 0 ;
	path = dupprintf( "%s\\%s", base, subpath ) ;
	found = set_filezilla_path_if_exists( path ) ;
	sfree( path ) ;
	return found ;
}

/* [KiTTY] FileZillaPath first, then the installer's default locations. A stored
 * path that no longer exists is dropped, as SearchWinSCP does. */
int SearchFileZilla( void ) {
	if( FileZillaPath != NULL ) { free( FileZillaPath ) ; FileZillaPath = NULL ; }
	if( kx_try_stored( KI_FILEZILLAPATH, NULL, set_filezilla_path_if_exists ) ) return 1 ;
	if( probe_filezilla_env_dir( "ProgramFiles", "FileZilla FTP Client\\filezilla.exe" ) ) return 1 ;
	/* a 64-bit FileZilla seen from a 32-bit process: see SearchWinSCP */
	if( probe_filezilla_env_dir( "ProgramW6432", "FileZilla FTP Client\\filezilla.exe" ) ) return 1 ;
	if( probe_filezilla_env_dir( "ProgramFiles(x86)", "FileZilla FTP Client\\filezilla.exe" ) ) return 1 ;
	if( probe_filezilla_env_dir( "LOCALAPPDATA", "Programs\\FileZilla FTP Client\\filezilla.exe" ) ) return 1 ;
	if( kx_try_in( InitialDirectory, "filezilla.exe", set_filezilla_path_if_exists ) ) return 1 ;
	return 0 ;
}

/* XML text: the five characters that break an element or an attribute. */
static void xmlcat( strbuf *dst, const char *s ) {
	if( !s ) return ;
	for( ; *s ; s++ ) {
		switch( *s ) {
			case '&':  bcat( dst, "&amp;" ) ; break ;
			case '<':  bcat( dst, "&lt;" ) ; break ;
			case '>':  bcat( dst, "&gt;" ) ; break ;
			case '"':  bcat( dst, "&quot;" ) ; break ;
			case '\'': bcat( dst, "&apos;" ) ; break ;
			default:   put_byte( dst, *s ) ;
		}
	}
}

static void b64cat( strbuf *dst, const char *s ) {
	char atom[5] = { 0, 0, 0, 0, 0 } ;
	size_t n = strlen( s ), i ;
	for( i = 0 ; i < n ; i += 3 ) {
		base64_encode_atom( (const unsigned char *)s + i, (int)( n - i >= 3 ? 3 : n - i ), atom ) ;
		bcat( dst, atom ) ;
	}
}

/* The temporary data directory of mode 1: made just before the start, removed
 * a minute later (FileZilla reads fzdefaults.xml at startup). In full; NULL
 * = none. */
static char *kx_fzdir = NULL ;

static void kx_fzdir_remove( const char *dir ) {
	static const char *const names[] = { "fzdefaults.xml", "sitemanager.xml", "filezilla.xml",
	                                     "recentservers.xml", "lockfile" } ;
	size_t i ;
	for( i = 0 ; i < sizeof(names)/sizeof(names[0]) ; i++ ) {
		char *full = dupprintf( "%s\\%s", dir, names[i] ) ;
		SetFileAttributes( full, FILE_ATTRIBUTE_NORMAL ) ; DeleteFile( full ) ;
		sfree( full ) ;
	}
	RemoveDirectory( dir ) ;
}

static DWORD WINAPI kx_fzdir_delete_thread( LPVOID p ) {
	char *dir = (char *)p ;
	Sleep( 60000 ) ;
	kx_fzdir_remove( dir ) ;
	sfree( dir ) ;
	return 0 ;
}

static void kx_fzdir_sweep( void ) {
	char *tmp, *pat, *full ;
	WIN32_FIND_DATA fd ; HANDLE h ;
	if( ( tmp = kitty_temp_path_dup() ) == NULL ) return ;
	pat = dupprintf( "%skitty_fz_*", tmp ) ;
	h = FindFirstFile( pat, &fd ) ;
	sfree( pat ) ;
	if( h == INVALID_HANDLE_VALUE ) { sfree( tmp ) ; return ; }
	do {
		if( fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) {
			full = dupprintf( "%s%s", tmp, fd.cFileName ) ;
			kx_fzdir_remove( full ) ;
			sfree( full ) ;
		}
	} while( FindNextFile( h, &fd ) ) ;
	FindClose( h ) ;
	sfree( tmp ) ;
}

/* Write the Site Manager entry; returns the directory (kx_fzdir, until the
 * release) or NULL. */
static const char *kx_fzdir_make( const char *host, int port, int fzproto,
                                  const char *user, const char *password ) {
	char *tmp, *file ;
	strbuf *xml ;
	HANDLE h ; DWORD written ; int i ;
	static unsigned serial = 0 ;
	if( ( tmp = kitty_temp_path_dup() ) == NULL ) return NULL ;
	for( i = 0 ; i < 16 ; i++ ) {
		kx_fzdir = dupprintf( "%skitty_fz_%08lx_%u", tmp,
		                      (unsigned long)GetTickCount() ^ (unsigned long)GetCurrentProcessId(), ++serial ) ;
		if( CreateDirectory( kx_fzdir, NULL ) ) break ;
		sfree( kx_fzdir ) ; kx_fzdir = NULL ;
	}
	sfree( tmp ) ;
	if( i == 16 ) return NULL ;
	xml = strbuf_new_nm() ;                /* it holds the password: cleared when it goes */
	bcat( xml, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
	           "<FileZilla3>\n"
	           "\t<Servers>\n\t\t<Server>\n\t\t\t<Host>" ) ;
	xmlcat( xml, host ) ;
	put_fmt( xml, "</Host>\n\t\t\t<Port>%d</Port>\n\t\t\t<Protocol>%d</Protocol>\n", port, fzproto ) ;
	bcat( xml, "\t\t\t<Type>0</Type>\n\t\t\t<User>" ) ;
	xmlcat( xml, user ) ;
	bcat( xml, "</User>\n\t\t\t<Pass encoding=\"base64\">" ) ;
	b64cat( xml, password ) ;
	bcat( xml, "</Pass>\n\t\t\t<Logontype>1</Logontype>\n"
	           "\t\t\t<EncodingType>Auto</EncodingType>\n"
	           "\t\t\t<BypassProxy>0</BypassProxy>\n\t\t\t<Name>" ) ;
	xmlcat( xml, KT_FZ_SITE_NAME ) ;
	bcat( xml, "</Name>\n\t\t\t<SyncBrowsing>0</SyncBrowsing>\n"
	           "\t\t\t<DirectoryComparison>0</DirectoryComparison>\n"
	           "\t\t</Server>\n\t</Servers>\n</FileZilla3>\n" ) ;
	file = dupprintf( "%s\\fzdefaults.xml", kx_fzdir ) ;
	h = CreateFile( file, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_HIDDEN, NULL ) ;
	sfree( file ) ;
	if( h == INVALID_HANDLE_VALUE ) {
		RemoveDirectory( kx_fzdir ) ; sfree( kx_fzdir ) ; kx_fzdir = NULL ;
		strbuf_free( xml ) ; return NULL ;
	}
	if( !WriteFile( h, xml->s, (DWORD)xml->len, &written, NULL ) || written != xml->len ) {
		CloseHandle( h ) ; kx_fzdir_remove( kx_fzdir ) ; sfree( kx_fzdir ) ; kx_fzdir = NULL ;
		strbuf_free( xml ) ; return NULL ;
	}
	CloseHandle( h ) ;
	strbuf_free( xml ) ;
	return kx_fzdir ;
}

/* The delayed delete owns the directory's name from here on. */
static void kx_fzdir_release( int now ) {
	HANDLE t ;
	char *dir = kx_fzdir ;
	if( dir == NULL ) return ;
	kx_fzdir = NULL ;
	if( now ) {
		kx_fzdir_remove( dir ) ;
		sfree( dir ) ;
	} else {
		t = CreateThread( NULL, 0, kx_fzdir_delete_thread, dir, 0, NULL ) ;
		if( t ) CloseHandle( t ) ; else kx_fzdir_delete_thread( dir ) ;
	}
}

/* Tools > Start FileZilla. */
void StartFileZilla( HWND hwnd ) {
	strbuf *cmd ;
	char *user, *host ;
	const char *proto ; int fzproto, fzprot, port, p ;
	size_t pw_at = 0, pw_len = 0 ;
	int mode = conf_get_int( conf, CONF_filezilla_pwmode ) ;
	/* The Conf holds the password wrapped (kitty_pwmem.c); FileZilla's own
	 * modes need the value, so it is unwrapped into this buffer, which is
	 * burned on every way out of this function. */
	char pw[KITTY_PW_MAX+1] ;
	int havepw = ( kitty_pw_get( conf, CONF_password, pw, sizeof(pw) ) > 0 ) ;
	const char *fzdir = NULL ;

	kx_fzdir_sweep() ;
	if( !kitty_xfer_tool_ready( 2 ) ) {
		kx_report( KT_BEHAVIOUR_TOOLS_FILEZILLA, KT_XFER_FILEZILLA_NOT_FOUND ) ;
		debug_logevent( "%s", KT_XFER_FILEZILLA_NOT_FOUND ) ;
		smemclr( pw, sizeof(pw) ) ;
		return ;
	}
#if KX_CHECK_SELF_UPDATING_TOOLS
	/* FileZilla receives the password in modes 1 and 2 only (0 = it prompts for it) */
	if( !kitty_helper_ready( hwnd, FileZillaPath, havepw && mode != 0, 0,
	                         kx_again_filezilla, kx_again( hwnd ), kx_again_free ) ) {
		smemclr( pw, sizeof(pw) ) ;
		return ;
	}
#endif
	/* A Hello-protected key the agent is not holding: FileZilla has no way to
	 * take a key file from us, so it can only fail - "Start FileZilla
	 * anyway?" first, as for WinSCP. */
	if( !kx_anyway( hwnd, KT_XFER_START_FILEZILLA_ANYWAY, kx_anyway_filezilla_done, kx_again( hwnd ) ) ) {
		smemclr( pw, sizeof(pw) ) ;
		return ;
	}
	/* The REAL path, quoted - not GetShortPathName(): started through its 8.3
	 * name (C:\PROGRA~1\FILEZI~1\FILEZI~1.EXE) FileZilla exits at once, before
	 * any window, because it locates its resources from its own module path. */

	/* protocol: FileZilla's names and its Site Manager codes. The two
	 * numberings differ - CONF_filezilla_protocol follows WinSCPProtocol
	 * (1 sftp, 2 ftp, 3 ftps, 4 ftpes), FileZilla's site codes do not - so the
	 * switch maps them one by one rather than arithmetically. */
	fzprot = conf_get_int( conf, CONF_filezilla_protocol ) ;
	if( fzprot < 1 || fzprot > 4 ) fzprot = 1 ;   /* FileZilla has no scheme for the rest */
	/* SFTP needs an SSH session to mean anything; on a telnet or raw session it
	 * becomes plain FTP. An explicitly chosen FTP variant is kept as it is. */
	if( fzprot == 1 && conf_get_int( conf, CONF_protocol ) != PROT_SSH ) fzprot = 2 ;
	switch( fzprot ) {
		case 2:  proto = "ftp" ;   fzproto = 0 ; break ;
		case 3:  proto = "ftps" ;  fzproto = 3 ; break ;
		case 4:  proto = "ftpes" ; fzproto = 4 ; break ;
		default: proto = "sftp" ;  fzproto = 1 ; break ;
	}
	/* A port written into the target override wins (applied with the override
	 * below); else the FileZilla panel's Port field, else the standard port. */
	port = kitty_xfer_port_field( conf_get_str( conf, CONF_filezilla_port ) ) ;
	if( port <= 0 ) port = kitty_xfer_default_port( conf, 2, fzprot ) ;

	/* target: the override (Connection > File-Transfer-Settings), else the session */
	if( strlen( conf_get_str( conf, CONF_sftpconnect ) ) > 0 ) {
		const char *b1 = conf_get_str( conf, CONF_sftpconnect ) ;
		if( (p = poss( "@", b1 )) > 0 ) { user = dupprintf( "%.*s", p - 1, b1 ) ; host = dupstr( b1 + p ) ; }
		else { user = dupstr( "" ) ; host = dupstr( b1 ) ; }
		/* A port in the override wins over the Port field; without one the
		 * override gives user and host only. A bare IPv6 address keeps its
		 * colons (kx_override_port answers 0 for it, and the host stands). */
		{ int ovp = kx_override_port( conf_get_str( conf, CONF_sftpconnect ) ) ;
		  if( ovp > 0 ) {
			char * c = strrchr( host, ':' ) ;
			if( c != NULL ) *c = '\0' ;
			port = ovp ;
		  }
		}
		if( user[0] == '\0' ) { sfree( user ) ; user = dupstr( conf_get_str_ambi( conf, CONF_username, NULL ) ) ; }
	} else {
		user = dupstr( conf_get_str_ambi( conf, CONF_username, NULL ) ) ;
		host = dupstr( conf_get_str( conf, CONF_host ) ) ;
	}

	cmd = strbuf_new_nm() ;                /* cleared when it goes: mode 2 puts the password in */
	put_fmt( cmd, "\"%s\"", FileZillaPath ) ;
	if( havepw && mode == 1 ) {
		fzdir = kx_fzdir_make( host, port, fzproto, user, pw ) ;
		if( fzdir == NULL ) mode = 0 ;      /* no directory: fall back to mode 0, never to the command line */
	}
	if( fzdir != NULL ) {
		bcat( cmd, " --site=\"1/" ) ; bcat( cmd, KT_FZ_SITE_NAME ) ; bcat( cmd, "\"" ) ;
	} else {
		/* The URL as ONE quoted argument (qcat): FileZilla takes the folder
		 * as written - a %20 stays "%20" there, WinSCP decodes it - so the
		 * folder goes as it is, a '"' in it as \". The password is urlcat'ed:
		 * qcat leaves it as it is, so its place moves by the opening quote. */
		strbuf *url = strbuf_new_nm() ;
		size_t at ;
		bcat( url, proto ) ; bcat( url, "://" ) ;
		urlcat( url, user ) ;
		if( havepw && mode == 2 ) {
			bcat( url, ":" ) ;
			pw_at = url->len ;
			urlcat( url, pw ) ;
			pw_len = url->len - pw_at ;
		}
		bcat( url, "@" ) ;
		if( poss( ":", host ) > 0 ) { bcat( url, "[" ) ; bcat( url, host ) ; bcat( url, "]" ) ; }
		else bcat( url, host ) ;
		put_fmt( url, ":%d", port ) ;
		/* The shell's folder (OSC 7 tracking on), as StartWinSCP hands it
		 * over: FileZilla opens there. Tracking off or no folder: none, as
		 * before. The Site Manager entry of mode 1 carries no folder. */
		{ const char *fdir = kitty_current_dir() ;
		  if( fdir != NULL && fdir[0] ) {
			bcat( url, fdir ) ;
			if( fdir[strlen(fdir)-1] != '/' ) bcat( url, "/" ) ;
		  }
		}
		bcat( cmd, " " ) ;
		at = cmd->len + 1 ;                 /* past qcat's opening quote */
		qcat( cmd, url->s ) ;
		if( pw_len ) pw_at += at ;
		strbuf_free( url ) ;                /* cleared: it may hold the password */
		/* With a password: FileZilla prompts for it before connecting (ask).
		 * Without one: interactive - FileZilla connects at once, its SFTP
		 * engine tries the agent's keys, and a password is requested only if
		 * the server still wants one. A bare user@host URL would instead
		 * stop at an "Enter password" box BEFORE connecting, agent or not. */
		if( havepw && mode == 0 ) bcat( cmd, " --logontype=ask" ) ;
		else if( !havepw ) bcat( cmd, " --logontype=interactive" ) ;
	}
	if( strlen( conf_get_str( conf, CONF_filezilla_options ) ) > 0 ) {
		bcat( cmd, " " ) ; bcat( cmd, conf_get_str( conf, CONF_filezilla_options ) ) ;
	}

	debug_logevent_redacted( "Start FileZilla", cmd->s, pw_at, pw_len ) ;
	if( fzdir != NULL ) SetEnvironmentVariable( "FZ_DATADIR", fzdir ) ;
#if KX_CHECK_SELF_UPDATING_TOOLS
	kitty_helper_run( hwnd, FileZillaPath, cmd->s ) ;
#else
	RunCommand( hwnd, cmd->s ) ;       /* not checked: KX_CHECK_SELF_UPDATING_TOOLS */
#endif
	if( fzdir != NULL ) SetEnvironmentVariable( "FZ_DATADIR", NULL ) ;
	strbuf_free( cmd ) ;
	sfree( user ) ; sfree( host ) ;
	smemclr( pw, sizeof(pw) ) ;
	kx_fzdir_release( 0 ) ;
}


/* Find the file-copy helper: kscp.exe, else PuTTY's pscp.exe - the PSCPPath
 * key, the pscpdir folder (kscp.exe then pscp.exe), kscp.exe beside kitty.exe,
 * pscp.exe in PuTTY's folder, pscp.exe beside kitty.exe. */
static int kx_adopt_pscp( const char *path ) {
	return adopt_tool_path_if_exists( &PSCPPath, path, NULL, NULL ) ;
}

int SearchPSCP( void ) {
	static const char ki[] = "kscp.exe", pu[] = "pscp.exe" ;

	if( PSCPPath!=NULL ) { free(PSCPPath) ; PSCPPath = NULL ; }
	// In the registry
	if( kx_try_stored( KI_PSCPPATH, NULL, kx_adopt_pscp ) ) return 1 ;

	// In the ini file: the folder, kscp.exe then pscp.exe in it
	if( kx_try_stored( KI_PSCPDIR, ki, kx_adopt_pscp ) ) return 1 ;
	if( kx_try_stored( KI_PSCPDIR, pu, kx_adopt_pscp ) ) return 1 ;
	/* kscp.exe beside kitty.exe - the usual answer */
	if( kx_try_in( InitialDirectory, ki, kx_adopt_pscp ) ) return 1 ;
	/* PuTTY's pscp.exe in PuTTY's usual folder */
	{ const char *pf = getenv( "ProgramFiles" ) ;
	  if( pf != NULL && pf[0] ) {
		char *dir = dupprintf( "%s\\PuTTY", pf ) ;
		int found = kx_try_in( dir, pu, kx_adopt_pscp ) ;
		sfree( dir ) ;
		if( found ) return 1 ;
	  }
	}

	/* PuTTY's pscp.exe beside kitty.exe */
	if( kx_try_in( InitialDirectory, pu, kx_adopt_pscp ) ) return 1 ;

	return 0 ;
}

// Drag and drop handling
void recupNomFichierDragDrop(HWND hwnd, HDROP* leDrop, int typepath ) {
        HDROP hDropInfo = *leDrop ;
        int nb,taille,i;
        taille=0;
        nb=0;
	if( leDrop==NULL ) return ;
        nb=DragQueryFile( hDropInfo, 0xFFFFFFFF, NULL, 0 ) ;
        char *fic ;
	/* Files to upload need kscp, checked first: while its confirmation box is open, the
	 * names wait in the retry and are sent on its Yes. kitty.ini (the editor
	 * below) is not a kscp job and goes on either way. */
	int upload = 1, nup = 0, iup = 0 ;
	kx_drop_t *drop = NULL ;         /* KiTTY: the paths to type, typepath only */
	{ kx_again_t *g = kx_again( hwnd ) ;
	  g->typepath = typepath ;
	  g->files = snewn( nb > 0 ? nb : 1, char * ) ;
	  for( i = 0 ; i < nb ; i++ ) {
		UINT n = DragQueryFile( hDropInfo, i, NULL, 0 ) ;
		char *f = snewn( n + 2, char ) ;
		UINT got = DragQueryFile( hDropInfo, i, f, n + 1 ) ; f[got] = '\0' ;
		if( strlen( f ) >= 10 && !strcmp( f+strlen(f)-10, "\\kitty.ini" ) ) sfree( f ) ;
		else g->files[g->nfiles++] = f ;
	  }
	  nup = g->nfiles ;
	  if( g->nfiles > 0 && kitty_xfer_tool_ready( 0 ) )
		upload = kitty_helper_ready( hwnd, PSCPPath, 1, 0, kx_again_drop, g, kx_again_free ) ;
	  else kx_again_free( g ) ;
	}
	if( upload && typepath ) drop = kx_drop_new( hwnd, nup ) ;
	if( nb>0 ) for( i = 0; i < nb; i++ ) {
                taille = DragQueryFile(hDropInfo, i, NULL, 0 ) ;   /* length, excluding NUL */
		fic = (char*)malloc(taille+2) ;
		if( !fic ) continue ;
                { UINT _g = DragQueryFile( hDropInfo, i, fic, taille+1 ) ; fic[_g] = '\0' ; }  /* force-terminate: DragQueryFile doesn't always NUL-terminate -> a stray byte was reaching kscp ("...pdf\0") */
		if( strlen(fic) >= 10 && !strcmp( fic+strlen(fic)-10,"\\kitty.ini" ) ) { // load the config file in the internal editor
			char *self = kitty_module_path_dup( NULL ), *shortname = self ? kx_short_path_dup( self ) : NULL ;
			if( shortname != NULL ) {
				char *line = dupprintf( "\"%s\" -ed %s", shortname, fic ) ;
				RunCommand( hwnd, line ) ;
				sfree( line ) ;
			}
			sfree( self ) ; sfree( shortname ) ;
		} else if( upload ) {
			/* NULL target dir either way: the RemotePath store the auto-pwd
			 * branch used to pass was never written on the 0.84 core (the
			 * __pw title-scan was not forward-ported), so it was always NULL. */
			if( iup < nup ) kx_send_one( hwnd, "", fic, NULL, drop, iup ) ;
			else SendOneFile( hwnd, "", fic, NULL ) ;
			iup++ ;
		}
		free(fic);
	}
	/* KiTTY: a file the loop never sent counts as not uploaded, so the
	 * drop still ends and types what did land. */
	if( drop ) for( ; iup < nup ; iup++ ) kx_drop_settle( drop, iup, 0 ) ;
	DragFinish(hDropInfo) ;  //free the memory...
        *leDrop = hDropInfo ;  //TOCHECK: parameter passing...
}

void OnDropFiles(HWND hwnd, HDROP hDropInfo, int shift) {
	/* Upload on drop is a per-session switch. The window is unregistered for
	 * drops when it is off (windows/window.c), so this message should not
	 * arrive at all; refuse it here too rather than rely on that. */
	if( !conf_get_bool(conf,CONF_kscp_dragdrop) ) { DragFinish(hDropInfo) ; return ; }
	if( conf_get_int(conf,CONF_protocol) != PROT_SSH ) {
		DragFinish( hDropInfo ) ;   /* the drop is over either way */
		kx_report( KT_CAP_XFER, KT_MSG_SSH_ONLY ) ;
		return ;
	}
	/* Drag-drop always uses the normal path now. The former "Send file in
	 * current directory" (CONF_scp_auto_pwd) option injected a
	 * `printf "...__pw:$(pwd)..."` probe into the shell and scraped the reply via
	 * the __pw OSC-title dispatcher - removed in 0.84 as the CVE-2024-23749 RCE -
	 * so it no longer captured anything. Its safe, opt-in replacement is OSC 7
	 * cwd tracking, which SendOneFile already consults via kitty_current_dir(). */
	/* KiTTY: the remote paths are typed after the upload when the session
	 * enables it, or for this drop when Shift was held at it. */
	recupNomFichierDragDrop(hwnd, &hDropInfo,
	                        conf_get_bool(conf,CONF_kscp_drop_typepath) || shift) ;
}
