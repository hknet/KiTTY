/*
 * kitty_osc7_parse.h - pure validation for OSC 7 remote-working-directory
 * payloads (ESC ] 7 ; file://host/path BEL).
 *
 * This is the data-plane-only guard that makes OSC 7 cwd tracking a SAFE
 * replacement for the removed __pw/__ws title-scan dispatcher (CVE-2024-23749):
 * a shell-reported path is %-decoded and checked before it is handed, as
 * data, to kscp, WinSCP or FileZilla - NOTHING is ever executed. Kept in
 * this dependency-free header (only these static functions, no includes needed)
 * so kitty.c and the regression test test/test_osc7.c compile the EXACT same
 * code. If you change the check or the decoder, update test/test_osc7.c.
 */
#ifndef KITTY_OSC7_PARSE_H
#define KITTY_OSC7_PARSE_H

static int osc7_ishex( char c ) {
	return (c>='0'&&c<='9') || (c>='a'&&c<='f') || (c>='A'&&c<='F') ;
}
static int osc7_hexval( char c ) {
	if( c>='0'&&c<='9' ) return c-'0' ;
	if( c>='a'&&c<='f' ) return c-'a'+10 ;
	return c-'A'+10 ;
}
/* Percent-decode in place (OSC 7 paths are %-encoded UTF-8).  Malformed %XX is
 * left literal.  Returns 0 if a %00 was decoded: an embedded NUL would
 * terminate the C string before osc7_path_ok() could judge the rest, silently
 * truncating the path, so the caller must reject such a payload outright.
 * (Every other control char survives as a byte and is caught by the check.) */
static int osc7_urldecode( char * s ) {
	char * r = s, * w = s ;
	int ok = 1 ;
	while( *r ) {
		if( r[0]=='%' && osc7_ishex(r[1]) && osc7_ishex(r[2]) ) {
			char v = (char)( (osc7_hexval(r[1])<<4) | osc7_hexval(r[2]) ) ;
			if( v == 0 ) ok = 0 ;
			*w++ = v ; r += 3 ;
		} else { *w++ = *r++ ; }
	}
	*w = '\0' ;
	return ok ;
}
/* The longest folder taken, after %-decoding: Linux PATH_MAX (4096) less its
 * NUL. The terminal receives OSC 7 in a 16 KB buffer, so even a folder of this
 * length with every byte %-encoded arrives whole. */
#define OSC7_PATH_MAX 4095

/* The first byte of p the check below refuses, or -1 when every byte
 * passes ('/' itself when the path is not absolute). */
static int osc7_path_bad_char( const char * p ) {
	if( p[0] != '/' ) return (unsigned char)p[0] ;
	for( ; *p ; p++ ) {
		unsigned char c = (unsigned char)*p ;
		if( c < 0x20 || c == 0x7F ) return c ;
	}
	return -1 ;
}
/* A path we are willing to hand to kscp, WinSCP and FileZilla: absolute,
 * every printable byte allowed (a Linux folder name may hold any of them),
 * UTF-8 bytes (>=0x80) too; a control byte (below 0x20, or DEL) rejects the
 * whole path and the previous folder stays.  Each consumer keeps the folder
 * ONE argument and data only: Win32 argv quoting (qcat) for kscp and the
 * FileZilla URL (a '"' as \"), every byte but letters, digits and /._-~
 * %-encoded in the WinSCP URL (pathcat), single quotes in the typed path
 * (kitty_droppath_quote, a ' as '\'').  FileZilla does not decode its URL's
 * folder, so it gets the bytes as they are. */
static inline int osc7_path_ok( const char * p ) {   /* inline: kitty.c uses the char form */
	return osc7_path_bad_char( p ) < 0 ;
}

#endif /* KITTY_OSC7_PARSE_H */
