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
