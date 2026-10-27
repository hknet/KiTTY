/*
 * test_osc7 - regression tests for the OSC 7 remote-cwd path validation
 * (kitty/kitty_osc7_parse.h).
 *
 * This is the data-plane guard that makes OSC 7 cwd tracking a safe replacement
 * for the removed __pw title-scan (CVE-2024-23749): a shell-reported
 * file://host/path is %-decoded and whitelist-checked before it may be used as
 * a kscp/WinSCP upload target. These cases lock in that a malicious or malformed
 * path is REJECTED (KiTTY then falls back to the remote home) while a legitimate
 * absolute path is accepted. Returns non-zero on any failure.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../kitty/kitty_osc7_parse.h"

static int failures = 0 ;

/* A path is "accepted" iff urldecode reports no embedded NUL AND the whitelist
 * passes - exactly mirroring kitty_set_remote_cwd() in kitty.c. */
static int osc7_accepts( const char * raw ) {
	static char buf[16384] ;   /* the terminal's OSC 7 buffer */
	strncpy( buf, raw, sizeof(buf)-1 ) ; buf[sizeof(buf)-1] = '\0' ;
	if( !osc7_urldecode( buf ) ) return 0 ;   /* embedded %00 -> reject */
	if( strlen( buf ) > OSC7_PATH_MAX ) return 0 ;   /* decoded length */
	return osc7_path_ok( buf ) ;
}

/* "/" + n copies of unit: a folder of a given length, plain or %-encoded */
static char * longpath( int n, const char * unit ) {
	size_t u = strlen( unit ) ;
	char * s = malloc( 2 + (size_t)n * u ), * p = s ;
	*p++ = '/' ;
	for( int i = 0 ; i < n ; i++ ) { memcpy( p, unit, u ) ; p += u ; }
	*p = '\0' ;
	return s ;
}

static void expect( const char * raw, int want, const char * why ) {
	int got = osc7_accepts( raw ) ;
	if( got != want ) {
		printf( "FAIL [%s]: \"%s\" -> %s (wanted %s)\n",
			why, raw, got?"accept":"reject", want?"accept":"reject" ) ;
		failures++ ;
	}
}

static void expect_decode( const char * raw, const char * want ) {
	char buf[2048] ;
	strncpy( buf, raw, sizeof(buf)-1 ) ; buf[sizeof(buf)-1] = '\0' ;
	osc7_urldecode( buf ) ;
	if( strcmp( buf, want ) != 0 ) {
		printf( "FAIL decode: \"%s\" -> \"%s\" (wanted \"%s\")\n", raw, buf, want ) ;
		failures++ ;
	}
}

int main( void ) {
	/* --- the Linux limit: 4095 bytes after decoding --- */
	{ char * a = longpath( OSC7_PATH_MAX - 1, "a" ) ;     /* 4095 */
	  char * b = longpath( OSC7_PATH_MAX, "a" ) ;         /* 4096 */
	  char * c = longpath( OSC7_PATH_MAX - 1, "%61" ) ;   /* 4095 decoded, 12 KB sent */
	  char * d = longpath( OSC7_PATH_MAX, "%61" ) ;       /* 4096 decoded */
	  expect( a, 1, "4095 bytes: the Linux maximum" ) ;
	  expect( b, 0, "4096 bytes: one over" ) ;
	  expect( c, 1, "4095 bytes %-encoded: the decoded length counts" ) ;
	  expect( d, 0, "4096 bytes %-encoded: one over" ) ;
	  free( a ) ; free( b ) ; free( c ) ; free( d ) ; }
	/* --- accepted: legitimate absolute paths --- */
	expect( "/home/user",            1, "simple" ) ;
	expect( "/tmp",                  1, "short" ) ;
	expect( "/var/log/syslog",       1, "nested" ) ;
	expect( "/home/user/.config",    1, "leading dot" ) ;
	expect( "/opt/my-app_v2.1",      1, "dash underscore dot" ) ;
	expect( "/~backups",             1, "tilde" ) ;
	expect( "/srv/data/%C3%A9t%C3%A9", 1, "%-encoded UTF-8 (accented)" ) ;
	expect( "/tmp/a b",              1, "space" ) ;
	expect( "/tmp/a%20b",            1, "%20 space decodes and is accepted" ) ;
	expect( "/tmp/a%25b",            1, "%25 decodes to a % and is accepted" ) ;
	expect( "/tmp/%",                1, "a folder named %" ) ;
	expect( "/tmp/%25%25",           1, "a folder named %%" ) ;
	expect( "/home/u/My Documents/x y", 1, "several spaces" ) ;
	expect( "/",                     1, "root" ) ;

	/* --- rejected: not an absolute path --- */
	expect( "home/user",             0, "relative" ) ;
	expect( "",                      0, "empty" ) ;
	expect( "file:relative",         0, "no leading slash" ) ;

	/* --- accepted: every printable byte is a legal Linux folder name; the
	 * consumers keep it data (qcat, pathcat, kitty_droppath_quote) --- */
	expect( "/tmp;rm -rf ~",         1, "semicolon + space" ) ;
	expect( "/tmp;id",               1, "semicolon" ) ;
	expect( "/tmp&whoami",           1, "ampersand" ) ;
	expect( "/tmp|cat",              1, "pipe" ) ;
	expect( "/tmp`id`",              1, "backtick" ) ;
	expect( "/tmp$(id)",             1, "dollar-paren" ) ;
	expect( "/tmp/${HOME}",          1, "dollar-brace" ) ;
	expect( "/a<b",                  1, "redirect <" ) ;
	expect( "/a>b",                  1, "redirect >" ) ;
	expect( "/a*b",                  1, "glob *" ) ;
	expect( "/a?b",                  1, "glob ?" ) ;
	expect( "/a'b",                  1, "single quote" ) ;
	expect( "/a\"b",                 1, "double quote" ) ;
	expect( "/a\\b",                1, "backslash" ) ;
	expect( "/backup (1)",           1, "brackets" ) ;
	expect( "/a@b+c,d=e!f#g:h^i",    1, "punctuation" ) ;
	expect( "/a[b]{c}",              1, "square and curly brackets" ) ;

	/* --- rejected: control characters --- */
	expect( "/tab\tb",               0, "tab (control char)" ) ;
	expect( "/a\nb",                 0, "newline (control char)" ) ;
	expect( "/a\rb",                 0, "carriage return (control char)" ) ;
	expect( "/a\x7f" "b",             0, "DEL (control char)" ) ;
	expect( "/a\x1b" "b",             0, "ESC (control char)" ) ;

	/* --- %-encoded control characters are rejected too (decode, then check) --- */
	expect( "/tmp/a%09b",            0, "%09 tab decodes then rejected" ) ;
	expect( "/tmp/a%0Ab",            0, "%0A newline decodes then rejected" ) ;
	expect( "/tmp/a%7Fb",            0, "%7F DEL decodes then rejected" ) ;
	expect( "/tmp/a%01b",            0, "%01 decodes then rejected" ) ;
	expect( "/tmp/a%1Bb",            0, "%1B ESC decodes then rejected" ) ;
	expect( "/tmp/a b;id",           1, "space then semicolon: taken, data only" ) ;
	expect( "/tmp/a%20b%27c",        1, "%27 single quote decodes and is taken" ) ;
	expect( "/tmp%3Bid",             1, "%3B semicolon decodes and is taken" ) ;
	expect( "/tmp/%22q%22",          1, "%22 double quotes decode and are taken" ) ;
	expect( "/tmp%2Fok",             1, "%2F decodes to a legitimate slash" ) ;

	/* --- embedded NUL (%00) must reject outright (truncation guard) --- */
	expect( "/tmp%00/etc/passwd",    0, "%00 embedded NUL rejected" ) ;

	/* --- decode correctness --- */
	expect_decode( "/a%2Fb",     "/a/b" ) ;
	expect_decode( "/a%20b",     "/a b" ) ;
	expect_decode( "/plain",     "/plain" ) ;
	expect_decode( "/bad%ZZ",    "/bad%ZZ" ) ;     /* malformed %XX left literal */
	expect_decode( "/trailing%", "/trailing%" ) ;  /* lone trailing % left literal */

	if( failures == 0 ) { printf( "test_osc7: all cases passed\n" ) ; return 0 ; }
	printf( "test_osc7: %d FAILURE(S)\n", failures ) ;
	return 1 ;
}
