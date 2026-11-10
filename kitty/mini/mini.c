/*
 * The kitty.ini helpers: readINI, writeINI and delINI read or change one key
 * by file, section and key name, through Windows' own ini functions.
 */
#include "mini.h"
#include <windows.h>
#include "../kitty_inifile.h"   /* kitty_inifile_write_blocked: readonly=yes */

/*
 * kitty.ini is read and written through Windows' own ini functions, the ones
 * kageant, kittygen and the console tools have always used (kitty_inifile.c):
 * one reader and one writer for the file across the suite. A write changes
 * its one line and leaves the rest of the file - the template's comments,
 * the order, the layout - as it was; the whole-file rewrite this replaced
 * dropped every comment line at the first save, and its swap (ReplaceFile)
 * was refused while anything held the file open, Windows' own ini cache in
 * this very process included (error 1175). Windows reads a section written
 * twice only where it first appears; kitty.exe merges such files once at
 * start (kitty_startup.c ConsolidateKittyIni), keeping what was read here.
 */

/* A value GetPrivateProfileString returns when the key is not there - the
 * call cannot distinguish "absent" from "empty" otherwise. */
static const char mini_absent[] = "\x01kitty.ini:absent\x01" ;

int readINI( const char * filename, const char * section, const char * key, char * pStr, size_t pStrSize) {
	char * buf ;
	DWORD cap ;
	if( filename == NULL || !filename[0] || section == NULL || !section[0] || key == NULL ) return 0 ;
	if( pStr == NULL || pStrSize == 0 ) return 0 ;
	cap = pStrSize < 65536 ? (DWORD)pStrSize : 65536 ;
	if( cap < sizeof(mini_absent) ) cap = sizeof(mini_absent) ;
	if( ( buf = (char*)malloc( cap ) ) == NULL ) return 0 ;
	GetPrivateProfileStringA( section, key, mini_absent, buf, cap, filename ) ;
	if( !strcmp( buf, mini_absent ) ) { free( buf ) ; return 0 ; }
	{ size_t n = strlen( buf ) ; if( n >= pStrSize ) n = pStrSize - 1 ; memcpy( pStr, buf, n ) ; pStr[n] = '\0' ; }
	free( buf ) ;
	return 1 ;
	}

/* 1 written; 0 not, Windows' reason in GetLastError(). A NULL key makes sure
 * the section exists (an empty one is written only where there is none).
 * The kitty.ini in charge under readonly=yes: nothing is written,
 * KITTY_RO_REFUSED (delINI too); any other file is written as ever. */
int writeINI( const char * filename, const char * section, const char * key, const char * value ) {
	if( filename == NULL || !filename[0] || section == NULL || !section[0] ) return 0 ;
	if( kitty_inifile_write_blocked( filename ) ) return KITTY_RO_REFUSED ;
	if( key == NULL || !key[0] ) {
		char probe[4] ;
		if( GetPrivateProfileSectionA( section, probe, sizeof(probe), filename ) > 0 ) return 1 ;
		return WritePrivateProfileSectionA( section, "\0", filename ) ? 1 : 0 ;
		}
	return WritePrivateProfileStringA( section, key, value ? value : "", filename ) ? 1 : 0 ;
	}

/* One key, or with a NULL key the whole section. */
int delINI( const char * filename, const char * section, const char * key ) {
	if( filename == NULL || !filename[0] || section == NULL || !section[0] ) return 0 ;
	if( kitty_inifile_write_blocked( filename ) ) return KITTY_RO_REFUSED ;
	return WritePrivateProfileStringA( section, ( key && key[0] ) ? key : NULL, NULL, filename ) ? 1 : 0 ;
	}
