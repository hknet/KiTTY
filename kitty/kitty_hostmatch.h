/*
 * kitty_hostmatch.h - the platform-free half of opening an OSC 8
 * file://host/path link (GNU ls --hyperlink writes one for every name):
 * the link's host and path, the host names the shell's own signals carry
 * (a window title "user@host: ~/dir", iTerm2's OSC 1337 RemoteHost=, the
 * answer of "hostname; hostname -f"), and the rule that two names are the
 * same machine. Dependency-free and static, shared verbatim with
 * test/test_hostmatch.c, so every rule here is unit-tested without a window
 * or a server. The path is decoded by the OSC 7 rules (kitty_osc7_parse.h):
 * percent-encoding decoded, %00 and control characters refused, nothing
 * else expanded.
 */
#ifndef KITTY_HOSTMATCH_H
#define KITTY_HOSTMATCH_H

#include <ctype.h>
#include <stddef.h>
#include <string.h>

#include "kitty_osc7_parse.h"

#define HM_HOST_MAX 255      /* a DNS name, less its NUL */

/* A character of a host name, and of a user name, as the signals carry
 * them. */
static inline int hm_host_char( char c ) {
    return isalnum( (unsigned char)c ) || c == '-' || c == '.' || c == '_' ;
}
static inline int hm_user_char( char c ) {
    return isalnum( (unsigned char)c ) || c == '-' || c == '.' || c == '_' || c == '$' ;
}

/* s[0..n) is a host name: 1 to HM_HOST_MAX host characters, not starting
 * with '.' or '-'. */
static inline int hm_is_host( const char *s, size_t n ) {
    size_t i ;
    if( n == 0 || n > HM_HOST_MAX || s[0] == '.' || s[0] == '-' ) return 0 ;
    for( i = 0 ; i < n ; i++ ) if( !hm_host_char( s[i] ) ) return 0 ;
    return 1 ;
}

/* s[0..n) into out, when it fits with its NUL. */
static inline int hm_put( char *out, size_t outlen, const char *s, size_t n ) {
    if( !out || n + 1 > outlen ) return 0 ;
    memcpy( out, s, n ) ;
    out[n] = '\0' ;
    return 1 ;
}

/* a[0..la) and b[0..lb) equal without regard to case */
static inline int hm_eq( const char *a, size_t la, const char *b, size_t lb ) {
    size_t i ;
    if( la != lb ) return 0 ;
    for( i = 0 ; i < la ; i++ )
        if( tolower( (unsigned char)a[i] ) != tolower( (unsigned char)b[i] ) ) return 0 ;
    return 1 ;
}

/* s[0..n) is written as an IPv4 address: digits and dots only. */
static inline int hm_is_ipv4( const char *s, size_t n ) {
    size_t i ;
    for( i = 0 ; i < n ; i++ )
        if( !isdigit( (unsigned char)s[i] ) && s[i] != '.' ) return 0 ;
    return n > 0 ;
}

/* Two host names are the same machine: equal without regard to case (a
 * trailing root dot ignored), or a short name equal to the first label of
 * the other, full name - "server01" and "server01.lab.example.org", either
 * way round. Two full names compare whole. An address (IPv4 digits and
 * dots, anything with ':') compares whole, never by a first label. An
 * empty name is no machine. */
static inline int hm_host_same( const char *a, const char *b ) {
    size_t la, lb ;
    const char *da, *db ;
    if( !a || !b ) return 0 ;
    la = strlen( a ) ; lb = strlen( b ) ;
    while( la && a[la-1] == '.' ) la-- ;
    while( lb && b[lb-1] == '.' ) lb-- ;
    if( !la || !lb ) return 0 ;
    if( hm_eq( a, la, b, lb ) ) return 1 ;
    if( memchr( a, ':', la ) || memchr( b, ':', lb ) ) return 0 ;
    if( hm_is_ipv4( a, la ) || hm_is_ipv4( b, lb ) ) return 0 ;
    da = memchr( a, '.', la ) ; db = memchr( b, '.', lb ) ;
    if( da && db ) return 0 ;
    if( da ) la = (size_t)( da - a ) ;
    if( db ) lb = (size_t)( db - b ) ;
    return hm_eq( a, la, b, lb ) ;
}

/* The host of a window title that starts with user@host followed by ':',
 * a space or the end: the defaults of bash on Debian/Ubuntu
 * ("user@host: ~/dir") and Fedora/RHEL ("user@host:~/dir"), Oh My Zsh,
 * grml's zsh ("user@host cmd"). Any other title: 0. */
static inline int hm_title_host( const char *t, char *out, size_t outlen ) {
    const char *p = t, *h ;
    if( !t ) return 0 ;
    while( hm_user_char( *p ) ) p++ ;
    if( p == t || *p != '@' ) return 0 ;
    h = ++p ;
    while( hm_host_char( *p ) ) p++ ;
    if( *p && *p != ':' && *p != ' ' ) return 0 ;
    if( !hm_is_host( h, (size_t)( p - h ) ) ) return 0 ;
    return hm_put( out, outlen, h, (size_t)( p - h ) ) ;
}

/* The host of iTerm2's shell integration report, the text of
 * OSC 1337 ; RemoteHost=user@host ST (the user may be missing). Any other
 * text: 0. */
static inline int hm_remotehost( const char *s, char *out, size_t outlen ) {
    static const char key[] = "RemoteHost=" ;
    const char *v, *at, *p ;
    size_t n ;
    if( !s || strncmp( s, key, sizeof(key) - 1 ) != 0 ) return 0 ;
    v = s + sizeof(key) - 1 ;
    at = strrchr( v, '@' ) ;
    if( at ) {
        for( p = v ; p < at ; p++ ) if( !hm_user_char( *p ) ) return 0 ;
        v = at + 1 ;
    }
    n = strlen( v ) ;
    if( !hm_is_host( v, n ) ) return 0 ;
    return hm_put( out, outlen, v, n ) ;
}

/* A host signal arrived: the first one is kept as the session's server
 * (first), the latest as where the shell is now (now). Both buffers of
 * size n. */
static inline void hm_track( char *first, char *now, size_t n, const char *host ) {
    size_t l = strlen( host ) ;
    if( !first[0] ) hm_put( first, n, host, l ) ;
    hm_put( now, n, host, l ) ;
}

/* The shell is on another machine than the first signal named. */
static inline int hm_moved( const char *first, const char *now ) {
    return first && now && first[0] && now[0] && !hm_host_same( first, now ) ;
}

/* What hm_file_link found. */
enum {
    HM_LINK_OK,        /* host and path set */
    HM_LINK_NOTFILE,   /* not a file: link */
    HM_LINK_NOHOST,    /* file:/path or file:///path - no host part */
    HM_LINK_BAD        /* no path, a host that is no host name, a path with
                        * %00 or a control character, or one too long */
};

/* A file://host/path link: the host and the path, both percent-decoded by
 * the OSC 7 rules. A '?' or '#' ends the path (a query, a fragment); a
 * file name holding one arrives encoded. path must hold strlen(uri) + 1
 * bytes: it is decoded in place. */
static inline int hm_file_link( const char *uri, char *host, size_t hostlen,
                                char *path, size_t pathlen ) {
    const char *h, *s, *e ;
    size_t n ;
    if( !uri || !hm_eq( uri, 5, "file:", 5 ) ) return HM_LINK_NOTFILE ;   /* stops at a shorter uri's NUL */
    h = uri + 5 ;
    if( h[0] != '/' || h[1] != '/' ) return HM_LINK_NOHOST ;
    h += 2 ;
    s = strchr( h, '/' ) ;
    if( !s ) return HM_LINK_BAD ;
    if( s == h ) return HM_LINK_NOHOST ;
    /* the host, decoded: a NUL or anything but a host name is refused */
    n = (size_t)( s - h ) ;
    if( n + 1 > pathlen || n + 1 > hostlen ) return HM_LINK_BAD ;
    memcpy( path, h, n ) ; path[n] = '\0' ;
    if( !osc7_urldecode( path ) || !hm_is_host( path, strlen( path ) ) ) return HM_LINK_BAD ;
    hm_put( host, hostlen, path, strlen( path ) ) ;
    /* the path, up to a query or a fragment */
    e = s + strcspn( s, "?#" ) ;
    n = (size_t)( e - s ) ;
    if( n + 1 > pathlen ) return HM_LINK_BAD ;
    memcpy( path, s, n ) ; path[n] = '\0' ;
    if( !osc7_urldecode( path ) ) return HM_LINK_BAD ;
    if( strlen( path ) > OSC7_PATH_MAX || osc7_path_bad_char( path ) >= 0 ) return HM_LINK_BAD ;
    return HM_LINK_OK ;
}

/* Every byte of s is ASCII. */
static inline int hm_ascii( const char *s ) {
    for( ; *s ; s++ ) if( (unsigned char)*s >= 0x80 ) return 0 ;
    return 1 ;
}

/* The answer of "hostname; hostname -f": the first two lines that are a
 * host name, trimmed, into a and b (other lines - a warning, a banner -
 * are passed over). Returns how many were found (0, 1 or 2). */
static inline int hm_probe_names( const char *out, char *a, size_t al, char *b, size_t bl ) {
    const char *p = out ;
    int got = 0 ;
    if( al ) a[0] = '\0' ;
    if( bl ) b[0] = '\0' ;
    while( p && *p && got < 2 ) {
        const char *e = p + strcspn( p, "\r\n" ), *s = p, *t = e ;
        while( s < t && ( *s == ' ' || *s == '\t' ) ) s++ ;
        while( t > s && ( t[-1] == ' ' || t[-1] == '\t' ) ) t-- ;
        if( hm_is_host( s, (size_t)( t - s ) ) ) {
            if( got == 0 ) got += hm_put( a, al, s, (size_t)( t - s ) ) ;
            else got += hm_put( b, bl, s, (size_t)( t - s ) ) ;
        }
        p = e ;
        while( *p == '\r' || *p == '\n' ) p++ ;
    }
    return got ;
}

#endif
