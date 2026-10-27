/*
 * kitty_osc7_parse.h - pure validation for OSC 7 remote-working-directory
 * payloads (ESC ] 7 ; file://host/path BEL).
 *
 * This is the data-plane-only guard that makes OSC 7 cwd tracking a SAFE
 * replacement for the removed __pw/__ws title-scan dispatcher (CVE-2024-23749):
 * a shell-reported path is %-decoded and whitelist-checked before it may be
 * spliced into a kscp/WinSCP command line - NOTHING is ever executed. Kept in
 * this dependency-free header (only these static functions, no includes needed)
 * so kitty.c and the regression test test/test_osc7.c compile the EXACT same
 * code. If you change the whitelist or the decoder, update test/test_osc7.c.
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
 * (Every other control char survives as a byte and is caught by the whitelist.) */
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

/* The first byte of p the whitelist below refuses, or -1 when every byte
 * passes ('/' itself when the path is not absolute). */
static int osc7_path_bad_char( const char * p ) {
	if( p[0] != '/' ) return (unsigned char)p[0] ;
	for( ; *p ; p++ ) {
		unsigned char c = (unsigned char)*p ;
		if( c >= 0x80 ) continue ;
		if( (c>='0'&&c<='9') || (c>='a'&&c<='z') || (c>='A'&&c<='Z') ) continue ;
		if( c=='/' || c=='.' || c=='-' || c=='_' || c=='~' ) continue ;
		return c ;
	}
	return -1 ;
}
/* Whitelist: a path we are willing to splice into the kscp/WinSCP command
 * lines.  Must be absolute; ASCII limited to alphanumerics and /._-~ ; raw
 * UTF-8 bytes (>=0x80, never a shell metacharacter) allowed.  Anything else -
 * space, quotes, ;|&$`<>*?()[]{} , controls - rejects the whole path, so we
 * simply fall back to today's behaviour (upload to the remote HOME). */
static inline int osc7_path_ok( const char * p ) {   /* inline: kitty.c uses the char form */
	return osc7_path_bad_char( p ) < 0 ;
}

#endif /* KITTY_OSC7_PARSE_H */
