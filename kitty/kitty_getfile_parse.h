/*
 * kitty_getfile_parse.h - the platform-free half of Get File's clipboard
 * check (hknet/KiTTY#71): what the clipboard text is, the path kscp -stat
 * is asked about, its one-line reply, a size as people read it, and the
 * OSC 7 baseline. Dependency-free and static, shared verbatim with
 * test/test_getfile.c, so every rule here is unit-tested without a window
 * or a server.
 */
#ifndef KITTY_GETFILE_PARSE_H
#define KITTY_GETFILE_PARSE_H

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* What the clipboard text is. Leading and trailing blanks and blank lines
 * are noise from the selection and do not count. */
enum {
    GF_EMPTY,        /* nothing but blanks */
    GF_WINPATH,      /* the first line is a Windows path ('\' or "C:") */
    GF_ONE,          /* one line, no wildcard, no trailing '/': the checked case */
    GF_FOLDER,       /* one line ending in '/' */
    GF_SEVERAL       /* several lines, or a wildcard (* ?) */
};

static inline int gf_is_blank( char c ) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' ; }

static inline int gf_is_windows_path( const char *s ) {
    if( strchr( s, '\\' ) != NULL ) return 1 ;
    if( isalpha( (unsigned char)s[0] ) && s[1] == ':' ) return 1 ;
    return 0 ;
}

/* Copies the text without its outer blanks into out (n bytes); returns
 * the class. The first line alone decides GF_WINPATH, as Get File always
 * has. */
static inline int gf_classify( const char *text, char *out, size_t n ) {
    const char *b = text ? text : "", *e, *nl ;
    size_t len, fl ;
    char first[4096] ;
    while( *b && gf_is_blank( *b ) ) b++ ;
    e = b + strlen( b ) ;
    while( e > b && gf_is_blank( e[-1] ) ) e-- ;
    len = (size_t)( e - b ) ;
    if( n ) {
        size_t c = len < n - 1 ? len : n - 1 ;
        memcpy( out, b, c ) ; out[c] = '\0' ;
    }
    if( len == 0 ) return GF_EMPTY ;
    nl = b ;
    while( nl < e && *nl != '\r' && *nl != '\n' ) nl++ ;
    fl = (size_t)( nl - b ) ;
    if( fl >= sizeof(first) ) fl = sizeof(first) - 1 ;
    memcpy( first, b, fl ) ; first[fl] = '\0' ;
    if( gf_is_windows_path( first ) ) return GF_WINPATH ;
    if( nl < e ) return GF_SEVERAL ;
    if( strpbrk( first, "*?" ) ) return GF_SEVERAL ;
    if( first[strlen(first)-1] == '/' ) return GF_FOLDER ;
    return GF_ONE ;
}

/* The path as SFTP takes it: SFTP knows no "~", and its relative paths are
 * relative to the login directory already. "~" -> ".", "~/x" -> "x";
 * "~user/x" and everything else unchanged. */
static inline void gf_stat_path( const char *remote, char *out, size_t n ) {
    const char *p = remote ;
    if( !n ) return ;
    if( p[0] == '~' && p[1] == '\0' ) p = "." ;
    else if( p[0] == '~' && p[1] == '/' ) { p += 2 ; if( !*p ) p = "." ; }
    snprintf( out, n, "%s", p ) ;
}

/* kscp -stat's reply: the first line that starts with one of its words.
 * Anything else on stdout (a banner, a server message) is skipped. */
enum { GFS_NONE, GFS_FILE, GFS_DIR, GFS_OTHER, GFS_MISSING, GFS_ERROR } ;

static inline int gf_word( const char *line, const char *w, const char **rest ) {
    size_t l = strlen( w ) ;
    if( strncmp( line, w, l ) != 0 ) return 0 ;
    if( line[l] != '\0' && line[l] != ' ' && line[l] != '\r' && line[l] != '\n' ) return 0 ;
    *rest = line[l] == ' ' ? line + l + 1 : line + l ;
    return 1 ;
}

static inline int gf_parse_stat( const char *out, unsigned long long *size, char *err, size_t errlen ) {
    const char *line = out ? out : "", *rest ;
    if( size ) *size = 0 ;
    if( err && errlen ) err[0] = '\0' ;
    while( *line ) {
        int r = GFS_NONE ;
        if( gf_word( line, "file", &rest ) ) {
            if( size ) *size = strtoull( rest, NULL, 10 ) ;
            r = GFS_FILE ;
        } else if( gf_word( line, "dir", &rest ) ) r = GFS_DIR ;
        else if( gf_word( line, "other", &rest ) ) r = GFS_OTHER ;
        else if( gf_word( line, "missing", &rest ) ) r = GFS_MISSING ;
        else if( gf_word( line, "error", &rest ) ) {
            if( err && errlen ) {
                size_t i = 0 ;
                while( rest[i] && rest[i] != '\r' && rest[i] != '\n' && i < errlen - 1 ) { err[i] = rest[i] ; i++ ; }
                err[i] = '\0' ;
            }
            r = GFS_ERROR ;
        }
        if( r != GFS_NONE ) return r ;
        line = strchr( line, '\n' ) ;
        if( !line ) break ;
        line++ ;
    }
    return GFS_NONE ;
}

/* 512 -> "512 bytes", 12345 -> "12 KB", 5242880 -> "5.0 MB". */
static inline void gf_size_text( unsigned long long b, char *out, size_t n ) {
    if( b < 1024ULL ) snprintf( out, n, "%llu byte%s", b, b == 1 ? "" : "s" ) ;
    else if( b < 1024ULL * 1024 ) snprintf( out, n, "%llu KB", ( b + 512 ) / 1024 ) ;
    else if( b < 1024ULL * 1024 * 1024 ) snprintf( out, n, "%.1f MB", (double)b / ( 1024.0 * 1024 ) ) ;
    else snprintf( out, n, "%.1f GB", (double)b / ( 1024.0 * 1024 * 1024 ) ) ;
}

/* The OSC 7 baseline: the host of the first OSC 7 after connecting is the
 * session's machine, whatever it calls itself; a later one naming another
 * host means the shell moved on. Host names compare without case, and a
 * short name matches an FQDN that starts with it (one shell sends
 * "server01", root's may send "server01.lab.hq.domain.at" on the same
 * machine). An empty name never counts as moved. */
static inline int gf_osc7_moved( const char *first, const char *now ) {
    size_t lf, ln, n ;
    if( !first || !now || !first[0] || !now[0] ) return 0 ;
    lf = strlen( first ) ; ln = strlen( now ) ;
    if( !strchr( first, '.' ) || !strchr( now, '.' ) ) {
        lf = strcspn( first, "." ) ; ln = strcspn( now, "." ) ;
    }
    if( lf != ln ) return 1 ;
    for( n = 0 ; n < lf ; n++ )
        if( tolower( (unsigned char)first[n] ) != tolower( (unsigned char)now[n] ) ) return 1 ;
    return 0 ;
}

#endif
