/*
 * kitty_settings_load.c - load a ".ktx" session file into a Conf.
 *
 * A .ktx is read the way a session of the folder store is: ksf_load parses
 * it (both line formats, "key=value" and classic KiTTY's "key\value\", with
 * classic KiTTY's password form converted) and KiTTY++'s session loader,
 * load_open_settings in settings.c, takes every key from it. This file only
 * adds what a .ktx needs on top: the deprecated encrypted lines, the
 * passwords in the forms an export or an older KiTTY wrote them, and the
 * KiTTY++ fix-ups the load hook gives a stored session. It used to be a
 * hand-written copy of the loader, one line per key, and had fallen 33 keys
 * behind it: those were never read, so a double-clicked .ktx took them from
 * Default Settings whatever the file said.
 *
 * kitty_xfer_migrate_protocol lives here too: both load paths call it.
 */
#include "putty.h"         /* first: pulls winsock2.h before windows.h on Windows */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <limits.h>
#include "kitty.h"
#include "kitty_defs.h"     /* KITTY_DEFAULT_SESSION */
#include "kitty_commun.h"  /* GetCryptSaltFlag */
#include "kitty_crypt.h"   /* decryptstring */
#include "kitty_text.h"    /* shared captions */
#include "kitty_pwmem.h"   /* passwords wrapped in memory */
#include "kitty_bridge.h"
#include "kitty_settings.h"
#include "kitty_registry.h"
#include "kitty_storage.h"
#include "kitty_secretstore.h"

/* CryptFileFlag lives in kitty_bridge.c (same as the write side). */
/* unmungestr is declared in kitty_commun.h (const char *in). */

/* Constants that were file-local in the original KiTTY tree. */
#ifndef PRINT_TO_CLIPBOARD_STRING
#define PRINT_TO_CLIPBOARD_STRING "Windows clipboard"  /* KiTTY 0.76b putty.h:33 */
#endif
#ifndef GSS_DEF_REKEY_MINS
#define GSS_DEF_REKEY_MINS 2     /* ssh/gss.h: default minutes between GSS cache checks */
#endif

/*
 * Sessions written before the file-transfer protocol was split per tool carry
 * WinSCPProtocol alone. kscp speaks only SCP and SFTP, FileZilla only SFTP and
 * the FTP family, so the old value maps onto each tool's own list; anything a
 * tool has no scheme for becomes SFTP, the default. WinSCPProtocol itself is
 * unchanged. Both load paths call this - the .ktx reader below and the
 * registry/file load hook in windows/putty.c - and only when the two new keys
 * are absent from the stored session, which is what makes it idempotent.
 *
 * It lives here rather than with the transfer code because this file is the
 * one both callers link: windows/test/test_storage_roundtrip compiles the .ktx
 * reader without any of kitty_xfer.c.
 */
void kitty_xfer_migrate_protocol(Conf *conf, int oldprot) {
    if (conf == NULL) return;
    conf_set_int(conf, CONF_kscp_protocol, oldprot == 0 ? 0 : 1);
    conf_set_int(conf, CONF_filezilla_protocol,
                 (oldprot >= 1 && oldprot <= 4) ? oldprot : 1);
}

/* ---- the deprecated encrypted .ktx (still read, never written) ---- */

static int g_ktx_encrypted_seen = 0 ;
static void ktx_note_encrypted( void ) { g_ktx_encrypted_seen = 1 ; }
static int ktx_encrypted_was_seen( void ) { int n = g_ktx_encrypted_seen ; g_ktx_encrypted_seen = 0 ; return n ; }

/* Told once per KiTTY run, however many encrypted files are loaded: importing a
 * folder of them must not produce one box per file. */
static int g_ktx_encrypted_told = 0 ;

static void ktx_warn_encrypted_once( void ) {
	if( g_ktx_encrypted_told ) return ;
	if( !ktx_encrypted_was_seen() ) return ;
	g_ktx_encrypted_told = 1 ;
	/*
	 * KittyCliReport, not MessageBox: this path is reached from -kload / -loadfile
	 * as well as from the GUI, and a modal box nobody can click is a hang for
	 * anyone importing from a script. The helper writes to the console of the
	 * shell that launched us and falls back to a box only when there is no console
	 * at all - started from Explorer, a shortcut or the installer.
	 */
	KittyCliReport(
		KT_CAP_KTX_ENCRYPTED_DEPRECATED,
		KT_SETTINGS_KTX_ENCRYPTED_DEPRECATED, 0 ) ;
}


/* One line of any length, newline (and a CR before it) stripped; NULL at the
 * end. Caller frees. */
static char *ktx_read_line( FILE *fp ) {
	size_t cap = 512, len = 0 ;
	char *buf = snewn( cap, char ) ;
	int c = EOF ;
	while( ( c = fgetc( fp ) ) != EOF && c != '\n' ) {
		if( len + 2 > cap ) { cap *= 2 ; buf = sresize( buf, cap, char ) ; }
		buf[len++] = (char)c ;
	}
	if( c == EOF && len == 0 ) { sfree( buf ) ; return NULL ; }
	if( len && buf[len-1] == '\r' ) len-- ;
	buf[len] = '\0' ;
	return buf ;
}

/* A line that is neither "key=value" nor "key\value\" is the deprecated
 * encrypted form: whole lines run through the compiled-in key. */
static int ktx_line_encrypted( const char *l ) {
	const char *eq = strchr( l, '=' ), *bs = strchr( l, '\\' ) ;
	size_t n = strlen( l ) ;
	if( !n ) return 0 ;
	if( eq && ( !bs || eq < bs ) ) return 0 ;
	return l[n-1] != '\\' ;
}

/* The file's keys. A file with encrypted lines is decoded into a scratch copy
 * first, which ksf_load then parses like any other; the copy is removed. */
static struct ksf_item *ktx_load_items( const char *filename ) {
	FILE *in = fopen( filename, "rb" ) ;
	char *line, tmpdir[MAX_PATH], tmp[MAX_PATH] ;
	int encrypted = 0 ;
	FILE *out ;
	struct ksf_item *items ;
	if( !in ) return NULL ;
	while( ( line = ktx_read_line( in ) ) != NULL ) {
		if( ktx_line_encrypted( line ) ) encrypted = 1 ;
		sfree( line ) ;
		if( encrypted ) break ;
	}
	if( !encrypted ) { fclose( in ) ; return ksf_load( filename ) ; }

	ktx_note_encrypted() ;
	if( !GetTempPathA( sizeof(tmpdir), tmpdir ) || !GetTempFileNameA( tmpdir, "ktx", 0, tmp ) ) {
		fclose( in ) ; return NULL ;
	}
	if( ( out = fopen( tmp, "wb" ) ) == NULL ) { fclose( in ) ; DeleteFileA( tmp ) ; return NULL ; }
	rewind( in ) ;
	while( ( line = ktx_read_line( in ) ) != NULL ) {
		if( ktx_line_encrypted( line ) ) {
			/* decoded in place; the plain form is never longer */
			char *dec = snewn( strlen( line ) + 64, char ) ;
			strcpy( dec, line ) ;
			decryptstring( GetCryptSaltFlag(), dec, MASTER_PASSWORD ) ;
			fprintf( out, "%s\n", dec ) ;
			smemclr( dec, strlen( dec ) ) ;
			sfree( dec ) ;
		} else
			fprintf( out, "%s\n", line ) ;
		smemclr( line, strlen( line ) ) ;
		sfree( line ) ;
	}
	fclose( in ) ;
	fclose( out ) ;
	items = ksf_load( tmp ) ;
	DeleteFileA( tmp ) ;
	return items ;
}

/* A password the file carries: our own envelope (an export, the bundle's
 * passphrase or this PC's protection), PLAIN:, classic KiTTY's encrypted form,
 * or cleartext. Whatever cannot be decoded here loads as empty. */
static void ktx_decode_password( Conf *conf, int key, int try_legacy ) {
	char raw[KITTY_PW_MAX + 1], *pt ;
	kitty_pw_get( conf, key, raw, sizeof(raw) ) ;
	if( !raw[0] ) return ;
	pt = kitty_secret_decode_imported( raw,
		try_legacy ? conf_get_str( conf, CONF_host ) : NULL,
		try_legacy ? conf_get_str( conf, CONF_termtype ) : NULL, try_legacy ) ;
	smemclr( raw, sizeof(raw) ) ;
	kitty_pw_set( conf, key, pt ? pt : "" ) ;
	if( pt ) { memset( pt, 0, strlen( pt ) ) ; free( pt ) ; }
}

void load_open_settings_forced(char *filename, Conf *conf) {
	struct ksf_item *items = ktx_load_items( filename ) ;
	settings_r *src ;
	if( items == NULL ) {
		char buffer[1024] ;
		snprintf( buffer, sizeof(buffer), KT_SETTINGS_FILE_NOT_FOUND, filename ) ;
		MessageBox( NULL, buffer, KT_CAP_ERROR, MB_OK|MB_ICONERROR ) ;
		return ;
	}
	src = kitty_open_settings_r_items( items ) ;   /* owns items now */
	kitty_settings_r_raw_secrets( src ) ;

	/* every key, from the file or its built-in default - the loader a stored
	 * session goes through */
	load_open_settings( src, conf ) ;

	/* what the load hook (windows/putty.c) gives a stored session: an older
	 * KiTTY's Notes folded into the Comment, the transfer protocol split */
	{
		const int absent = -32768 ;   /* no protocol value is negative */
		char *note = read_setting_s( src, "Notes" ) ;
		int oldprot = read_setting_i( src, "WinSCPProtocol", absent ) ;
		if( note ) { kitty_merge_legacy_note( conf, note ) ; sfree( note ) ; }
		if( oldprot != absent &&
		    read_setting_i( src, "KscpProtocol", absent ) == absent &&
		    read_setting_i( src, "FileZillaProtocol", absent ) == absent )
			kitty_xfer_migrate_protocol( conf, oldprot ) ;
		/* SCPAutoPwd retired: its sessions get OSC 7 cwd tracking instead */
		if( read_setting_i( src, "SCPAutoPwd", 0 ) )
			conf_set_bool( conf, CONF_osc7_cwd_tracking, true ) ;
	}
	close_settings_r( src ) ;

	/* the passwords as the file holds them (the reader left them alone) */
	ktx_decode_password( conf, CONF_password, 1 ) ;
	/* old KiTTY stored ProxyPassword in the clear: no legacy decode */
	ktx_decode_password( conf, CONF_proxy_password, 0 ) ;

	if( conf_get_int( conf, CONF_transparencynumber ) < -1 ) conf_set_int( conf, CONF_transparencynumber, -1 ) ;
	if( conf_get_int( conf, CONF_transparencynumber ) > 255 ) conf_set_int( conf, CONF_transparencynumber, 255 ) ;

	/* after the load: once per run, however many files */
	ktx_warn_encrypted_once() ;
	kitty_pw_seal_all( conf ) ;
}
