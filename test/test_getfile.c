/*
 * test_getfile - Get File's clipboard check (kitty/kitty_getfile_parse.h,
 * hknet/KiTTY#71): the class of the clipboard text, the path kscp -stat is
 * asked about, kscp's reply, the size text and the OSC 7 baseline. Returns
 * non-zero on any failure.
 */
#include <stdio.h>
#include <string.h>

#include "../kitty/kitty_getfile_parse.h"

static int failures = 0 ;

static void cls( const char *text, int want, const char *want_out ) {
	char out[256] ;
	int got = gf_classify( text, out, sizeof(out) ) ;
	if( got != want || ( want_out && strcmp( out, want_out ) ) ) {
		printf( "FAIL classify \"%s\": %d \"%s\" (wanted %d \"%s\")\n",
			text ? text : "(null)", got, out, want, want_out ? want_out : "" ) ;
		failures++ ;
	}
}

/* The same with the target's host and account: the scp form. */
static void cat( const char *text, const char *host, const char *user, int want, const char *want_out ) {
	char out[8192] ;
	int got = gf_classify_at( text, out, sizeof(out), host, user ) ;
	if( got != want || ( want_out && strcmp( out, want_out ) ) ) {
		printf( "FAIL classify_at \"%.60s\" (%s@%s): %d \"%.60s\" (wanted %d \"%s\")\n",
			text, user ? user : "", host ? host : "", got, out, want, want_out ? want_out : "" ) ;
		failures++ ;
	}
}

static void sp( const char *in, const char *want ) {
	char out[256] ;
	gf_stat_path( in, out, sizeof(out) ) ;
	if( strcmp( out, want ) ) { printf( "FAIL stat_path \"%s\": \"%s\" (wanted \"%s\")\n", in, out, want ) ; failures++ ; }
}

static void st( const char *reply, int want, unsigned long long want_size, const char *want_err ) {
	unsigned long long size ; char err[64] ;
	int got = gf_parse_stat( reply, &size, err, sizeof(err) ) ;
	if( got != want || size != want_size || strcmp( err, want_err ) ) {
		printf( "FAIL parse_stat \"%s\": %d %llu \"%s\" (wanted %d %llu \"%s\")\n",
			reply, got, size, err, want, want_size, want_err ) ;
		failures++ ;
	}
}

static void sz( unsigned long long b, const char *want ) {
	char out[32] ;
	gf_size_text( b, out, sizeof(out) ) ;
	if( strcmp( out, want ) ) { printf( "FAIL size %llu: \"%s\" (wanted \"%s\")\n", b, out, want ) ; failures++ ; }
}

static void mv( const char *first, const char *now, int want ) {
	if( gf_osc7_moved( first, now ) != want ) {
		printf( "FAIL osc7_moved \"%s\" \"%s\" (wanted %d)\n", first ? first : "(null)", now ? now : "(null)", want ) ;
		failures++ ;
	}
}

int main( void ) {
	/* the class, and the text without its outer blanks */
	cls( NULL,                       GF_EMPTY,   "" ) ;
	cls( "",                         GF_EMPTY,   "" ) ;
	cls( " \r\n\t \n",               GF_EMPTY,   "" ) ;
	cls( "app.log",                  GF_ONE,     "app.log" ) ;
	cls( "  /var/log/app.log \r\n",  GF_ONE,     "/var/log/app.log" ) ;
	cls( "\n\n~/notes.txt\n",        GF_ONE,     "~/notes.txt" ) ;
	cls( "my file.txt",              GF_ONE,     "my file.txt" ) ;
	cls( "/var/log/",                GF_FOLDER,  "/var/log/" ) ;
	cls( "/var/log/*.log",           GF_SEVERAL, NULL ) ;
	cls( "file?.txt",                GF_SEVERAL, NULL ) ;
	cls( "a.txt\nb.txt",             GF_SEVERAL, "a.txt\nb.txt" ) ;
	cls( "a.txt\r\n\r\nb.txt\r\n",   GF_SEVERAL, "a.txt\r\n\r\nb.txt" ) ;
	cls( "C:\\temp\\x.txt",          GF_WINPATH, NULL ) ;
	cls( "d:/temp/x.txt",            GF_WINPATH, NULL ) ;
	cls( "dir\\file",                GF_WINPATH, NULL ) ;
	cls( "/ok\nC:\\later",           GF_SEVERAL, NULL ) ;   /* the first line decides */

	/* text that cannot be a path: no check, no login */
	cls( "https://example.com/x.tgz",  GF_URL,     NULL ) ;
	cls( "sftp://host/etc/hosts",      GF_URL,     NULL ) ;
	cls( "git+ssh://h/r.git",          GF_URL,     NULL ) ;
	cls( "a\tb.txt",                   GF_CONTROL, NULL ) ;   /* a tab from a table */
	cls( "x\x7f.txt",                  GF_CONTROL, NULL ) ;
	cls( "x\x01y",                     GF_CONTROL, NULL ) ;
	cls( "/var/log/app.log",           GF_ONE,     NULL ) ;   /* not a URL: no "://" */
	cls( "3dfiles://x",                GF_ONE,     NULL ) ;   /* a scheme starts with a letter */
	{	char name[300], path[4200] ;
		memset( name, 'n', 256 ) ; name[256] = '\0' ;
		cls( name, GF_TOOLONG, NULL ) ;                     /* a part over 255 bytes */
		name[255] = '\0' ;
		cls( name, GF_ONE, NULL ) ;                         /* 255 is still a name */
		memset( path, 'p', sizeof(path) - 1 ) ; path[sizeof(path) - 1] = '\0' ;
		for( int i = 100 ; i < (int)sizeof(path) - 1 ; i += 100 ) path[i] = '/' ;
		cls( path, GF_TOOLONG, NULL ) ;                     /* over 4095 in all */
		path[4095] = '\0' ;
		cls( path, GF_ONE, NULL ) ;                         /* 4095 is still a path */
	}

	/* the scp form: this host's path is taken, another host is not */
	cat( "srv:/var/log/app.log",           "srv", "bob", GF_ONE,       "/var/log/app.log" ) ;
	cat( "bob@srv:/var/log/app.log",       "srv", "bob", GF_ONE,       "/var/log/app.log" ) ;
	cat( "bob@SRV:~/notes.txt",            "srv", "bob", GF_ONE,       "~/notes.txt" ) ;   /* host names ignore case */
	cat( "bob@srv:notes.txt",              "srv", "bob", GF_ONE,       "notes.txt" ) ;     /* a user: the form */
	cat( "srv:/var/log/",                  "srv", "bob", GF_FOLDER,    "/var/log/" ) ;
	cat( "srv:",                           "srv", "bob", GF_FOLDER,    "~/" ) ;            /* the login dir */
	cat( "bob@[::1]:/x.txt",               "::1", "bob", GF_ONE,       "/x.txt" ) ;
	cat( "other:/var/log/app.log",         "srv", "bob", GF_ELSEWHERE, "other:/var/log/app.log" ) ;
	cat( "eve@srv:/var/log/app.log",       "srv", "bob", GF_ELSEWHERE, NULL ) ;   /* another account */
	cat( "eve@srv:/x",                     "srv", "",    GF_ONE,       "/x" ) ;   /* no account known: the host decides */
	cat( "notes:2026.txt",                 "srv", "bob", GF_ONE,       "notes:2026.txt" ) ;   /* a file name */
	cat( "dir/a:/b",                       "srv", "bob", GF_ONE,       "dir/a:/b" ) ;   /* '/' before ':': a path */
	cat( "srv:/x\ty",                      "srv", "bob", GF_CONTROL,   NULL ) ;
	cat( "srv:/a/*.log",                   "srv", "bob", GF_SEVERAL,   "/a/*.log" ) ;
	cls( "srv:/var/log/app.log",           GF_ONE, "srv:/var/log/app.log" ) ;   /* no session: literal */

	/* SFTP has no ~ */
	sp( "~",              "." ) ;
	sp( "~/",             "." ) ;
	sp( "~/a/b.txt",      "a/b.txt" ) ;
	sp( "~bob/x",         "~bob/x" ) ;
	sp( "/etc/hosts",     "/etc/hosts" ) ;
	sp( "rel/x",          "rel/x" ) ;

	/* kscp's reply */
	st( "file 12345\n",                    GFS_FILE,    12345, "" ) ;
	st( "file 0\r\n",                      GFS_FILE,    0,     "" ) ;
	st( "dir\n",                           GFS_DIR,     0,     "" ) ;
	st( "other\n",                         GFS_OTHER,   0,     "" ) ;
	st( "missing\n",                       GFS_MISSING, 0,     "" ) ;
	st( "error permission denied\r\n",     GFS_ERROR,   0,     "permission denied" ) ;
	st( "Welcome to host\nfile 7\n",       GFS_FILE,    7,     "" ) ;   /* a banner first */
	st( "files 3\ndirectory\n",            GFS_NONE,    0,     "" ) ;   /* whole words only */
	st( "",                                GFS_NONE,    0,     "" ) ;
	st( "error\n",                         GFS_ERROR,   0,     "" ) ;

	/* sizes */
	sz( 0,                 "0 bytes" ) ;
	sz( 1,                 "1 byte" ) ;
	sz( 512,               "512 bytes" ) ;
	sz( 12345,             "12 KB" ) ;
	sz( 5242880,           "5.0 MB" ) ;
	sz( 3221225472ULL,     "3.0 GB" ) ;

	/* OSC 7 baseline */
	mv( "server01", "server01",                  0 ) ;
	mv( "server01", "SERVER01",                  0 ) ;
	mv( "server01", "server02",                  1 ) ;
	mv( "server01", "server01.lab.hq.domain.at", 0 ) ;   /* short and long form of one host */
	mv( "server01.lab.at", "server01", 0 ) ;
	mv( "server01.lab.at", "server01.other.at", 1 ) ;
	mv( "server01", "server011.lab.at", 1 ) ;
	mv( "localhost", "server01", 1 ) ;
	mv( "", "server02",                          0 ) ;
	mv( "server01", "",                          0 ) ;
	mv( NULL, "x",                               0 ) ;

	if( failures ) { printf( "test_getfile: %d FAILED\n", failures ) ; return 1 ; }
	printf( "test_getfile: all passed\n" ) ;
	return 0 ;
}
