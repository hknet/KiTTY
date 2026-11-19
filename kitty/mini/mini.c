/*
 * The kitty.ini helpers: readINI, writeINI and delINI read or change one key
 * by file, section and key name, through Windows' own ini functions.
 */
#include "mini.h"
#include <windows.h>
#include "../kitty_globalsettings.h"   /* the reads and writes */

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

/* kitty_globalsettings.c holds the reads and writes, for kitty.exe's
 * parameters and the other programs alike. */

int readINI( const char * filename, const char * section, const char * key, char * pStr, size_t pStrSize) {
	char * v ;
	if( pStr == NULL || pStrSize == 0 ) return 0 ;
	if( ( v = kitty_settings_file_read_dup( filename, section, key ) ) == NULL ) return 0 ;
	{ size_t n = strlen( v ) ; if( n >= pStrSize ) n = pStrSize - 1 ; memcpy( pStr, v, n ) ; pStr[n] = '\0' ; }
	free( v ) ;
	return 1 ;
	}

/* 1 written; 0 not, Windows' reason in GetLastError(). A NULL key makes sure
 * the section exists (an empty one is written only where there is none).
 * The kitty.ini in charge under readonly=yes: nothing is written,
 * KITTY_RO_REFUSED (delINI too); any other file is written as ever. */
int writeINI( const char * filename, const char * section, const char * key, const char * value ) {
	return kitty_settings_file_write( filename, section, key, value ) ;
	}

/* One key, or with a NULL key the whole section. */
int delINI( const char * filename, const char * section, const char * key ) {
	return kitty_settings_file_del( filename, section, key ) ;
	}
