/*
 * test_hostmatch - the host matching of OSC 8 file://host/path links
 * (kitty/kitty_hostmatch.h): the same-machine rule, the window title
 * pattern, OSC 1337 RemoteHost, the first/now tracking, the link's host and
 * decoded path, and the answer of the klink probe. Returns non-zero on any
 * failure.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../kitty/kitty_hostmatch.h"

static int failures = 0 ;

static void same( const char *a, const char *b, int want ) {
	if( hm_host_same( a, b ) != want ) {
		printf( "FAIL host_same \"%s\" \"%s\" (wanted %d)\n", a ? a : "(null)", b ? b : "(null)", want ) ;
		failures++ ;
	}
}

static void title( const char *t, const char *want ) {
	char out[HM_HOST_MAX + 1] ;
	int got = hm_title_host( t, out, sizeof(out) ) ;
	if( want ? ( !got || strcmp( out, want ) ) : got ) {
		printf( "FAIL title \"%s\": %d \"%s\" (wanted %s)\n", t ? t : "(null)", got, got ? out : "",
			want ? want : "none" ) ;
		failures++ ;
	}
}

static void rhost( const char *s, const char *want ) {
	char out[HM_HOST_MAX + 1] ;
	int got = hm_remotehost( s, out, sizeof(out) ) ;
	if( want ? ( !got || strcmp( out, want ) ) : got ) {
		printf( "FAIL remotehost \"%s\": %d \"%s\" (wanted %s)\n", s ? s : "(null)", got, got ? out : "",
			want ? want : "none" ) ;
		failures++ ;
	}
}

static void flink( const char *uri, int want, const char *want_host, const char *want_path ) {
	char host[HM_HOST_MAX + 1] ;
	size_t n = strlen( uri ) + 1 ;
	char *path = malloc( n ) ;
	int got ;
	host[0] = '\0' ; path[0] = '\0' ;
	got = hm_file_link( uri, host, sizeof(host), path, n ) ;
	if( got != want || ( want == HM_LINK_OK && ( strcmp( host, want_host ) || strcmp( path, want_path ) ) ) ) {
		printf( "FAIL file_link \"%.80s\": %d \"%s\" \"%.80s\" (wanted %d \"%s\" \"%s\")\n", uri, got, host, path,
			want, want_host ? want_host : "", want_path ? want_path : "" ) ;
		failures++ ;
	}
	free( path ) ;
}

static void probe( const char *out, int want, const char *wa, const char *wb ) {
	char a[64], b[64] ;
	int got = hm_probe_names( out, a, sizeof(a), b, sizeof(b) ) ;
	if( got != want || strcmp( a, wa ) || strcmp( b, wb ) ) {
		printf( "FAIL probe \"%s\": %d \"%s\" \"%s\" (wanted %d \"%s\" \"%s\")\n", out, got, a, b, want, wa, wb ) ;
		failures++ ;
	}
}

int main( void ) {
	char first[HM_HOST_MAX + 1] = "", now[HM_HOST_MAX + 1] = "" ;
	char big[5000] ;
	size_t i ;

	/* the same machine: case, the short name = the first label, either way */
	same( "server01", "server01", 1 ) ;
	same( "Server01", "SERVER01", 1 ) ;
	same( "server01", "server01.lab.example.org", 1 ) ;
	same( "server01.lab.example.org", "server01", 1 ) ;
	same( "SERVER01.lab.example.org", "server01", 1 ) ;
	same( "server01.lab.example.org", "server01.lab.example.org.", 1 ) ;
	same( "server01.lab.example.org", "server01.other.example.org", 0 ) ;   /* two full names: whole */
	same( "server01", "server02", 0 ) ;
	same( "server0", "server01.lab.example.org", 0 ) ;
	same( "server01.lab", "server01", 1 ) ;
	same( "10.0.0.1", "10.0.0.1", 1 ) ;
	same( "10.0.0.1", "10", 0 ) ;          /* an address never by its first label */
	same( "10", "10.0.0.1", 0 ) ;
	same( "10.0.0.1", "10.0.0.2", 0 ) ;
	same( "fe80::1", "fe80", 0 ) ;
	same( "", "server01", 0 ) ;
	same( "server01", "", 0 ) ;
	same( NULL, "server01", 0 ) ;
	same( ".", ".", 0 ) ;

	/* the window title: user@host followed by ':', a space or the end */
	title( "alice@server01: ~/src", "server01" ) ;         /* Debian, Ubuntu, Oh My Zsh */
	title( "alice@server01:~/src", "server01" ) ;          /* Fedora, RHEL */
	title( "root@db1.example.org: /etc", "db1.example.org" ) ;
	title( "alice@server01 vim notes.txt", "server01" ) ;  /* grml zsh */
	title( "alice@server01", "server01" ) ;
	title( "svc_backup$@host-2: /", "host-2" ) ;
	title( "vim notes.txt", NULL ) ;
	title( "~/src", NULL ) ;
	title( "(chroot)alice@server01: ~", NULL ) ;
	title( "@server01: ~", NULL ) ;
	title( "alice@: ~", NULL ) ;
	title( "alice@server01/x", NULL ) ;
	title( "alice@server01;rm", NULL ) ;
	title( "alice@-bad: ~", NULL ) ;
	title( " alice@server01: ~", NULL ) ;
	title( "", NULL ) ;
	title( NULL, NULL ) ;
	/* a host name over 255 characters is no host name */
	memcpy( big, "u@", 2 ) ;
	for( i = 2 ; i < 2 + 300 ; i++ ) big[i] = 'h' ;
	big[i] = '\0' ;
	title( big, NULL ) ;

	/* OSC 1337 RemoteHost (iTerm2 shell integration) */
	rhost( "RemoteHost=alice@server01", "server01" ) ;
	rhost( "RemoteHost=alice@server01.lab.example.org", "server01.lab.example.org" ) ;
	rhost( "RemoteHost=server01", "server01" ) ;
	rhost( "RemoteHost=a@b@server01", NULL ) ;              /* '@' is no user character */
	rhost( "RemoteHost=alice@", NULL ) ;
	rhost( "RemoteHost=alice@server01;x", NULL ) ;
	rhost( "RemoteHost=alice@server 01", NULL ) ;
	rhost( "CurrentDir=/home/alice", NULL ) ;
	rhost( "remotehost=alice@server01", NULL ) ;
	rhost( NULL, NULL ) ;

	/* first and now: the first signal is the server, a later other host
	 * means the shell moved on, and back again */
	hm_track( first, now, sizeof(first), "server01" ) ;
	if( strcmp( first, "server01" ) || strcmp( now, "server01" ) || hm_moved( first, now ) ) {
		printf( "FAIL track: first signal\n" ) ; failures++ ;
	}
	hm_track( first, now, sizeof(first), "server01.lab.example.org" ) ;
	if( hm_moved( first, now ) ) { printf( "FAIL track: the full name of the same machine moved\n" ) ; failures++ ; }
	hm_track( first, now, sizeof(first), "jumphost2" ) ;
	if( strcmp( first, "server01" ) || !hm_moved( first, now ) ) {
		printf( "FAIL track: another host not seen as moved\n" ) ; failures++ ;
	}
	hm_track( first, now, sizeof(first), "SERVER01" ) ;
	if( hm_moved( first, now ) ) { printf( "FAIL track: back on the server still moved\n" ) ; failures++ ; }
	if( hm_moved( "", "x" ) || hm_moved( "x", "" ) ) { printf( "FAIL moved: an empty name\n" ) ; failures++ ; }

	/* the link: host and path, percent-encoding decoded, never expanded */
	flink( "file://server01/home/alice/notes.txt", HM_LINK_OK, "server01", "/home/alice/notes.txt" ) ;
	flink( "FILE://server01/etc/", HM_LINK_OK, "server01", "/etc/" ) ;
	flink( "file://server01/home/alice/my%20file.txt", HM_LINK_OK, "server01", "/home/alice/my file.txt" ) ;
	flink( "file://server01/tmp/100%25%20done", HM_LINK_OK, "server01", "/tmp/100% done" ) ;
	flink( "file://server01/tmp/%USERPROFILE%", HM_LINK_OK, "server01", "/tmp/%USERPROFILE%" ) ;
	flink( "file://server01/tmp/a%3Fb%23c", HM_LINK_OK, "server01", "/tmp/a?b#c" ) ;
	flink( "file://server01/tmp/a?query#frag", HM_LINK_OK, "server01", "/tmp/a" ) ;
	flink( "file://server01/tmp/%E6%97%A5%E6%9C%AC", HM_LINK_OK, "server01", "/tmp/\xE6\x97\xA5\xE6\x9C\xAC" ) ;
	flink( "file://db1.example.org/", HM_LINK_OK, "db1.example.org", "/" ) ;
	flink( "file://server01/tmp/a%00b", HM_LINK_BAD, NULL, NULL ) ;
	flink( "file://server01/tmp/a%0Ab", HM_LINK_BAD, NULL, NULL ) ;
	flink( "file://server01/tmp/a%09b", HM_LINK_BAD, NULL, NULL ) ;
	flink( "file://server01/tmp/a%7Fb", HM_LINK_BAD, NULL, NULL ) ;
	flink( "file://server01", HM_LINK_BAD, NULL, NULL ) ;
	flink( "file://bad%20host/x", HM_LINK_BAD, NULL, NULL ) ;
	flink( "file://bad%00host/x", HM_LINK_BAD, NULL, NULL ) ;
	flink( "file://[::1]/x", HM_LINK_BAD, NULL, NULL ) ;
	flink( "file:///etc/passwd", HM_LINK_NOHOST, NULL, NULL ) ;
	flink( "file:/etc/passwd", HM_LINK_NOHOST, NULL, NULL ) ;
	flink( "https://server01/x", HM_LINK_NOTFILE, NULL, NULL ) ;
	flink( "fil", HM_LINK_NOTFILE, NULL, NULL ) ;
	/* a path over 4095 bytes once decoded */
	strcpy( big, "file://server01/" ) ;
	for( i = strlen( big ) ; i < 16 + 4100 ; i++ ) big[i] = 'a' ;
	big[i] = '\0' ;
	flink( big, HM_LINK_BAD, NULL, NULL ) ;
	if( !hm_ascii( "/home/alice/x.txt" ) || hm_ascii( "/tmp/\xE6\x97\xA5" ) ) {
		printf( "FAIL ascii\n" ) ; failures++ ;
	}

	/* the probe's answer: hostname, then hostname -f */
	probe( "server01\nserver01.lab.example.org\n", 2, "server01", "server01.lab.example.org" ) ;
	probe( "server01\r\nserver01\r\n", 2, "server01", "server01" ) ;
	probe( "  server01  \n", 1, "server01", "" ) ;
	probe( "hostname: Name or service not known\nserver01\n", 1, "server01", "" ) ;
	probe( "", 0, "", "" ) ;
	probe( "\n\n", 0, "", "" ) ;

	if( failures ) { printf( "%d failure(s)\n", failures ) ; return 1 ; }
	printf( "test_hostmatch: all passed\n" ) ;
	return 0 ;
}
