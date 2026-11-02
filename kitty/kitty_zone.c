/*
 * kitty_zone.c - Windows' zone policy for a file. See kitty_zone.h.
 */
#include "putty.h"          /* first: pulls winsock2.h before windows.h */
#include "storage.h"
#include <windows.h>
#include <urlmon.h>
#include <stdlib.h>

#include "kitty.h"            /* kitty_ktx_gate */
#include "kitty_text.h"
#include "kitty_storage.h"    /* kitty_open_settings_r_items */
#include "kitty_dlgbox.h"     /* kitty_confirm_box, kitty_info_box */
#include "kitty_zone.h"


int kitty_zone_file_policy(const char *path)
{
    IInternetSecurityManager *ism = NULL;
    WCHAR *wpath = NULL, *full = NULL;
    DWORD fn;
    DWORD policy = URLPOLICY_QUERY;
    int result = KITTY_ZONE_ASK, wn;
    HRESULT init, hr;

    if (!path || !*path)
        return KITTY_ZONE_ASK;
    wn = MultiByteToWideChar(CP_ACP, 0, path, -1, NULL, 0);
    if (wn <= 0 || (wpath = malloc((size_t)wn * sizeof(WCHAR))) == NULL)
        return KITTY_ZONE_ASK;
    MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, wn);
    /* the full path, held in memory sized to it */
    fn = GetFullPathNameW(wpath, 0, NULL, NULL);
    if (fn == 0 || (full = malloc((size_t)fn * sizeof(WCHAR))) == NULL ||
        GetFullPathNameW(wpath, fn, full, NULL) == 0) {
        free(full);
        free(wpath);
        return KITTY_ZONE_ASK;
    }
    free(wpath);

    init = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    hr = CoCreateInstance(&CLSID_InternetSecurityManager, NULL,
                          CLSCTX_INPROC_SERVER, &IID_IInternetSecurityManager,
                          (void **)&ism);
    if (SUCCEEDED(hr) && ism) {
        hr = ism->lpVtbl->ProcessUrlAction(
            ism, full, URLACTION_SHELL_EXECUTE_HIGHRISK, (BYTE *)&policy,
            sizeof(policy), NULL, 0,
            PUAF_NOUI | PUAF_ISFILE | PUAF_REQUIRESAVEDFILECHECK, 0);
        if (SUCCEEDED(hr)) {
            switch (policy & URLPOLICY_MASK_PERMISSIONS) {
              case URLPOLICY_ALLOW:    result = KITTY_ZONE_ALLOW; break;
              case URLPOLICY_DISALLOW: result = KITTY_ZONE_REFUSE; break;
              default:                 result = KITTY_ZONE_ASK; break;
            }
        }
        ism->lpVtbl->Release(ism);
    }
    if (init == S_OK || init == S_FALSE)
        CoUninitialize();
    free(full);
    return result;
}

static int policy_dword(HKEY root, const char *name)
{
    DWORD v = 0, sz = sizeof(v), type = 0;
    HKEY k;
    int set = 0;
    if (RegOpenKeyExA(root,
            "Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Attachments",
            0, KEY_READ, &k) != ERROR_SUCCESS)
        return 0;
    if (RegQueryValueExA(k, name, NULL, &type, (BYTE *)&v, &sz) == ERROR_SUCCESS &&
        type == REG_DWORD && v == 1)
        set = 1;
    RegCloseKey(k);
    return set;
}

int kitty_zone_unblock_hidden(void)
{
    return policy_dword(HKEY_LOCAL_MACHINE, "HideZoneInfoOnProperties") ||
           policy_dword(HKEY_CURRENT_USER, "HideZoneInfoOnProperties");
}

/* The entries of a .ktx file that run a program or write a file on this PC,
 * one "Name: value" line each (a long value cut), or NULL when there is
 * none. Only the file's own entries count: Default Settings behind it are
 * the user's own. */
static void ktx_risky_add( strbuf *b, const char *name, const char *value ) {
	char cut[204] ;
	size_t n = strlen( value ) ;
	if( n > 200 ) { memcpy( cut, value, 197 ) ; strcpy( cut + 197, "..." ) ; value = cut ; }
	put_fmt( b, "    %s: %s\n", name, value ) ;
}
static char *ktx_risky_entries( settings_r *src ) {
	static const char *const runs[] = { "PreConnectCommand", "Autocommand", "AutocommandOut",
		"ScriptfileContent", "Scriptfile", "rzCommand", "szCommand", "FileZillaOptions" } ;
	strbuf *b = strbuf_new() ;
	char *v ;
	for( size_t i = 0 ; i < sizeof(runs)/sizeof(runs[0]) ; i++ ) {
		if( ( v = read_setting_s( src, runs[i] ) ) != NULL ) {
			if( *v ) ktx_risky_add( b, runs[i], v ) ;
			sfree( v ) ;
		}
	}
	if( read_setting_i( src, "ProxyMethod", -1 ) == PROXY_CMD
	    && ( v = read_setting_s( src, "ProxyTelnetCommand" ) ) != NULL ) {
		if( *v ) ktx_risky_add( b, "ProxyTelnetCommand (local proxy command)", v ) ;
		sfree( v ) ;
	}
	if( read_setting_i( src, "LogType", 0 ) != 0
	    && ( v = read_setting_s( src, "LogFileName" ) ) != NULL ) {
		if( *v ) ktx_risky_add( b, "LogFileName (session log)", v ) ;
		sfree( v ) ;
	}
	if( read_setting_i( src, "RunCmdConfirm", -1 ) == 0 )
		ktx_risky_add( b, "RunCmdConfirm", "0 (programs run without a question)" ) ;
	if( b->len == 0 ) { strbuf_free( b ) ; return NULL ; }
	return strbuf_to_str( b ) ;
}

/* A .ktx file opened to connect (-kload): when it carries an entry that runs
 * a program or writes a file, Windows' zone policy for the file decides
 * (kitty_zone.c): allowed - as it is; to be asked - one question listing
 * the entries, the whole file or nothing; refused - no connection, the
 * reason and the remedy. 1 = go on, 0 = do not connect. */
int kitty_ktx_gate( const char *filename ) {
	struct ksf_item *items = ktx_load_items( filename ) ;
	settings_r *src ;
	char *list, *text ;
	int pol, go ;
	if( items == NULL ) return 1 ;   /* the load reports a missing file itself */
	src = kitty_open_settings_r_items( items ) ;   /* owns items now */
	list = ktx_risky_entries( src ) ;
	close_settings_r( src ) ;
	if( list == NULL ) return 1 ;
	pol = kitty_zone_file_policy( filename ) ;
	if( pol == KITTY_ZONE_ALLOW ) { sfree( list ) ; return 1 ; }
	if( pol == KITTY_ZONE_ASK ) {
		text = dupprintf( KT_KTX_ZONE_ASK_FMT, filename, list ) ;
		go = kitty_confirm_box( NULL, KT_CAP_KTX_ZONE, text, KT_KTX_ZONE_ASK_WARN ) ;
	} else {
		text = dupprintf( kitty_zone_unblock_hidden() ? KT_KTX_ZONE_REFUSED_ADMIN_FMT
		                                              : KT_KTX_ZONE_REFUSED_FMT, filename, list ) ;
		kitty_info_box( NULL, KT_CAP_KTX_ZONE, text, NULL ) ;
		go = 0 ;
	}
	sfree( text ) ;
	sfree( list ) ;
	return go ;
}
