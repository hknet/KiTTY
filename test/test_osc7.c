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
#include <string.h>

#include "../kitty/kitty_osc7_parse.h"

static int failures = 0 ;

/* A path is "accepted" iff urldecode reports no embedded NUL AND the whitelist
 * passes - exactly mirroring kitty_set_remote_cwd() in kitty.c. */
static int osc7_accepts( const char * raw ) {
	char buf[2048] ;
	strncpy( buf, raw, sizeof(buf)-1 ) ; buf[sizeof(buf)-1] = '\0' ;
	if( !osc7_urldecode( buf ) ) return 0 ;   /* embedded %00 -> reject */
	return osc7_path_ok( buf ) ;
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
	/* --- accepted: legitimate absolute paths --- */
	expect( "/home/user",            1, "simple" ) ;
	expect( "/tmp",                  1, "short" ) ;
	expect( "/var/log/syslog",       1, "nested" ) ;
	expect( "/home/user/.config",    1, "leading dot" ) ;
	expect( "/opt/my-app_v2.1",      1, "dash underscore dot" ) ;
	expect( "/~backups",             1, "tilde" ) ;
	expect( "/srv/data/%C3%A9t%C3%A9", 1, "%-encoded UTF-8 (accented)" ) ;
	expect( "/",                     1, "root" ) ;

	/* --- rejected: not an absolute path --- */
	expect( "home/user",             0, "relative" ) ;
	expect( "",                      0, "empty" ) ;
	expect( "file:relative",         0, "no leading slash" ) ;

	/* --- rejected: shell metacharacters (the injection guard) --- */
	expect( "/tmp;rm -rf ~",         0, "semicolon + space" ) ;
	expect( "/tmp;id",               0, "semicolon" ) ;
	expect( "/tmp&whoami",           0, "ampersand" ) ;
	expect( "/tmp|cat",              0, "pipe" ) ;
	expect( "/tmp`id`",              0, "backtick" ) ;
	expect( "/tmp$(id)",             0, "dollar-paren" ) ;
	expect( "/tmp/${HOME}",          0, "dollar-brace" ) ;
	expect( "/a<b",                  0, "redirect <" ) ;
	expect( "/a>b",                  0, "redirect >" ) ;
	expect( "/a*b",                  0, "glob *" ) ;
	expect( "/a?b",                  0, "glob ?" ) ;
	expect( "/a'b",                  0, "single quote" ) ;
	expect( "/a\"b",                 0, "double quote" ) ;
	expect( "/a b",                  0, "space" ) ;
	expect( "/tab\tb",               0, "tab (control char)" ) ;

	/* --- %-encoded metacharacters must ALSO be rejected (decode, then check) --- */
	expect( "/tmp%20x",              0, "%20 space decodes then rejected" ) ;
	expect( "/tmp%3Bid",             0, "%3B semicolon decodes then rejected" ) ;
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
