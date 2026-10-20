/*
 * kitty_regbackup.c - the registry store's safety copies: the .sav export
 * of the hive in use (Windows' own registry tool, hidden; multi-line values
 * repaired), its timestamped rotation and the restore of the newest copy,
 * the portable folder store's backup, and the leftovers of retired
 * features cleaned out of the hive.
 */
#include <dirent.h>
#include <io.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/locking.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

// PuTTY includes
#include "putty.h"
#include "terminal.h"
#include "putty-rc.h"

// Windows-specific includes (windows.h must come first)
#include <windows.h>
#include "kitty_oldwin.h"   /* record what an older Windows does not have */
#include <psapi.h>
#include <iphlpapi.h>

// KiTTY includes
#include "kitty.h"
#include "kitty_params.h"
#include "kitty_broadcast.h"   /* the other KiTTY windows: count, broadcast, resize */
#include "kitty_portfwd.h"     /* the port-forward display */
#include "kitty_defs.h"     /* KITTY_DEFAULT_SESSION */
#include "kitty_common.h"
#include "kitty_image.h"
#include "kitty_crypt.h"
#include "kitty_registry.h"
#include "kitty_tools.h"
#include "kitty_win.h"
#include "kitty_updater.h"
#include "kitty_winutil.h"
#include "kitty_dlgbox.h"
#include "kitty_launcher.h"
#include "winfont_fallback.h"
#include "kitty_msgbox.h"   /* themed MessageBox routing */
#include "kitty_oldwin_reg.h"   /* XP: RegDeleteTree/RegGetValue via oldwin */
#include "kitty_inifile.h"   /* kitty_store_ro_regpath: readonly=yes */
#include "kitty_text.h"   /* shared captions and wordings (also for the .c files included below) */
#include "kitty_inikeys.h"   /* KI_*: the kitty.ini key names */
#include "kitty_notes.h"   /* the application notification, marked owed at startup */
#include "kitty_pwmem.h"   /* passwords wrapped in memory (kitty_commands.c too) */
#include "kitty_storage.h"
#include "kitty_gui.h"
#include "kitty_bridge.h"
#include "kitty_exportbundle.h"
#include "mini/mini.h"
#include "kitty_b64.h"
#include "kitty_store.h"
#include "kitty_regbackup.h"
#include "kitty_int.h"   /* the paths and flags kitty.c owns */
#include "kitty_winerr.h"   /* kitty_win_reason: Windows' reason, for the reports */

/* Run Windows' own registry tool, hidden, and report whether it succeeded.
 * `hive` is NULL for verbs that take only a file (import). */
/* Why the last kitty_reg_tool call failed, for the caller's report: reg.exe's
 * exit code, or Windows' error when it could not even be started. */
static DWORD kitty_reg_tool_exit = 0, kitty_reg_tool_winerr = 0 ;
static int kitty_reg_tool( const char *verb, const char *hive, const char *path ) {
	char sysdir[MAX_PATH], cmd[8192] ;
	STARTUPINFOA si ; PROCESS_INFORMATION pi ; DWORD rc = 1 ;
	kitty_reg_tool_exit = 0 ; kitty_reg_tool_winerr = 0 ;
	if( GetSystemDirectoryA( sysdir, sizeof(sysdir) ) == 0 ) { kitty_reg_tool_winerr = GetLastError() ; return 0 ; }
	if( hive != NULL )
		snprintf( cmd, sizeof(cmd), "\"%s\\reg.exe\" %s \"%s\" \"%s\" /y", sysdir, verb, hive, path ) ;
	else
		snprintf( cmd, sizeof(cmd), "\"%s\\reg.exe\" %s \"%s\"", sysdir, verb, path ) ;
	memset( &si, 0, sizeof(si) ) ; si.cb = sizeof(si) ;
	si.dwFlags = STARTF_USESHOWWINDOW ; si.wShowWindow = SW_HIDE ;
	if( !CreateProcessA( NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi ) ) {
		kitty_reg_tool_winerr = GetLastError() ; return 0 ; }
	WaitForSingleObject( pi.hProcess, 60000 ) ;
	if( !GetExitCodeProcess( pi.hProcess, &rc ) ) rc = 1 ;   /* STILL_ACTIVE after 60 s: a failure too */
	CloseHandle( pi.hThread ) ; CloseHandle( pi.hProcess ) ;
	kitty_reg_tool_exit = rc ;
	return ( rc == 0 ) ;
	}

/* Import a .reg file produced by SaveRegistryKeyEx(). */
static int kitty_reg_import( const char *filename ) {
	return kitty_reg_tool( "import", NULL, filename ) ;
	}

/* Why the last LoadRegistryKey failed: the file and the reason, for the
 * caller's report (/loadreg's result line, the start-up messages). */
static char kitty_sav_load_why[1024] = "" ;
const char *kitty_sav_load_reason( void ) { return kitty_sav_load_why ; }
static int sav_load_fail_win( const char *path, DWORD err ) {
	snprintf( kitty_sav_load_why, sizeof(kitty_sav_load_why), "%s: %s", path, kitty_win_reason( (int)( err ? err : ERROR_READ_FAULT ) ) ) ;
	return 0 ;
	}
static int sav_load_fail_regexe( const char *path ) {
	if( kitty_reg_tool_winerr ) return sav_load_fail_win( path, kitty_reg_tool_winerr ) ;
	snprintf( kitty_sav_load_why, sizeof(kitty_sav_load_why), KT_SAV_IMPORT_FAILED_FMT, path, (unsigned long)kitty_reg_tool_exit ) ;
	return 0 ;
	}

// Backup of the registry key
/* The backup is produced by Windows' own exporter rather than a hand-rolled
 * serialiser. The QueryKey() this replaces did not write REG_BINARY values at
 * all (and, reusing its line buffer, emitted the previous line again in their
 * place), wrote REG_MULTI_SZ and REG_EXPAND_SZ as plain strings so they came
 * back with the wrong type, dropped anything past a fixed 1 KB, and could
 * overflow that buffer on a long value name. reg.exe gets all of it right and
 * produces a genuine .reg the user can read or import by hand. */
/* reg.exe writes a REG_SZ that holds a line break as a quoted string broken
 * across several lines, and reg.exe import then SKIPS that value: a two-line
 * session Comment vanished from every backup, and restoring an untouched
 * hive deleted it. Rewrite such values as hex(1) - the UTF-16LE bytes, the
 * form reg import restores exactly. The file is UTF-16LE with a BOM; every
 * other line is copied through untouched. */
static int kitty_reg_line_is_value( const wchar_t *l, size_t n, size_t *valstart ) {
	size_t i ;
	if( n < 3 ) return 0 ;
	if( l[0] == L'@' ) { if( n >= 3 && l[1] == L'=' && l[2] == L'"' ) { *valstart = 3 ; return 1 ; } return 0 ; }
	if( l[0] != L'"' ) return 0 ;
	for( i = 1 ; i + 2 < n ; i++ ) {
		if( l[i] == L'\\' ) { i++ ; continue ; }            /* escaped char in the name */
		if( l[i] == L'"' && l[i+1] == L'=' && l[i+2] == L'"' ) { *valstart = i + 3 ; return 1 ; }
		if( l[i] == L'"' ) return 0 ;                        /* "name"=dword:... */
	}
	return 0 ;
}
/* Does this line END the quoted value (last char an unescaped quote)? */
static int kitty_reg_line_closes( const wchar_t *l, size_t n, size_t from ) {
	size_t bs = 0, i ;
	if( n <= from || l[n-1] != L'"' ) return 0 ;
	for( i = n - 1 ; i > from && l[i-1] == L'\\' ; i-- ) bs++ ;
	return ( bs % 2 ) == 0 ;
}
/* 1 repaired or nothing to repair; 0 the repaired file could not be written
 * (the export left as reg.exe wrote it, GetLastError) */
static int kitty_reg_fix_multiline( const char *filename ) {
	FILE *fp ; long size ; unsigned char *buf ; wchar_t *w, *out ; size_t n, pos, o = 0, cap ; int changed = 0 ;
	if( ( fp = fopen( filename, "rb" ) ) == NULL ) return 0 ;
	fseek( fp, 0, SEEK_END ) ; size = ftell( fp ) ; fseek( fp, 0, SEEK_SET ) ;
	if( size < 4 ) { fclose( fp ) ; return 1 ; }
	buf = (unsigned char *)malloc( size + 2 ) ;
	if( buf == NULL ) { fclose( fp ) ; SetLastError( ERROR_NOT_ENOUGH_MEMORY ) ; return 0 ; }
	if( fread( buf, 1, size, fp ) != (size_t)size ) { fclose( fp ) ; free( buf ) ; SetLastError( ERROR_READ_FAULT ) ; return 0 ; }
	fclose( fp ) ;
	if( buf[0] != 0xFF || buf[1] != 0xFE ) { free( buf ) ; return 1 ; }   /* not reg.exe's Unicode file */
	w = (wchar_t *)( buf + 2 ) ; n = ( size - 2 ) / 2 ;
	cap = n * 6 + 64 ; out = (wchar_t *)malloc( cap * sizeof(wchar_t) ) ;
	if( out == NULL ) { free( buf ) ; SetLastError( ERROR_NOT_ENOUGH_MEMORY ) ; return 0 ; }
	pos = 0 ;
	while( pos < n ) {
		size_t ls = pos, le, vs ;
		while( pos < n && w[pos] != L'\n' ) pos++ ;
		le = pos ; if( pos < n ) pos++ ;                     /* past the \n */
		while( le > ls && ( w[le-1] == L'\r' ) ) le-- ;      /* line without its ending */
		if( kitty_reg_line_is_value( w + ls, le - ls, &vs ) && !kitty_reg_line_closes( w + ls, le - ls, vs ) ) {
			/* The value runs on: collect its lines until one closes it. */
			size_t vcap = 1024, vlen = 0, i ; wchar_t *val = (wchar_t *)malloc( vcap * sizeof(wchar_t) ) ;
			size_t seg_s = ls + vs, seg_e = le ; int closed = 0 ;
			if( val == NULL ) break ;
			for( ; ; ) {
				for( i = seg_s ; i < seg_e ; i++ ) {
					if( vlen + 4 > vcap ) { vcap *= 2 ; val = (wchar_t *)realloc( val, vcap * sizeof(wchar_t) ) ; }
					if( w[i] == L'\\' && i + 1 < seg_e ) { val[vlen++] = w[i+1] ; i++ ; }   /* \\ and \" */
					else val[vlen++] = w[i] ;
				}
				if( closed || pos >= n ) break ;
				/* the original line break belongs to the value */
				if( vlen + 4 > vcap ) { vcap *= 2 ; val = (wchar_t *)realloc( val, vcap * sizeof(wchar_t) ) ; }
				val[vlen++] = L'\r' ; val[vlen++] = L'\n' ;
				seg_s = pos ;
				while( pos < n && w[pos] != L'\n' ) pos++ ;
				seg_e = pos ; if( pos < n ) pos++ ;
				while( seg_e > seg_s && w[seg_e-1] == L'\r' ) seg_e-- ;
				if( kitty_reg_line_closes( w + seg_s, seg_e - seg_s, 0 ) ) { closed = 1 ; seg_e-- ; }
			}
			if( closed ) {
				/* "name"=hex(1):xx,00,...,00,00 with reg's own line continuation */
				static const wchar_t hexd[] = L"0123456789abcdef" ;
				size_t need = vs + 8 + ( vlen + 1 ) * 2 * 3 + ( ( vlen + 1 ) / 8 ) * 4 + 16 ;
				int col = 0 ;
				if( o + need > cap ) { cap = ( o + need ) * 2 ; out = (wchar_t *)realloc( out, cap * sizeof(wchar_t) ) ; }
				memcpy( out + o, w + ls, ( vs - 1 ) * sizeof(wchar_t) ) ; o += vs - 1 ;   /* "name"= */
				memcpy( out + o, L"hex(1):", 7 * sizeof(wchar_t) ) ; o += 7 ;
				for( i = 0 ; i <= vlen ; i++ ) {
					unsigned c = ( i < vlen ) ? (unsigned)val[i] : 0u ;
					unsigned bytes[2] = { c & 0xFF, ( c >> 8 ) & 0xFF } ;
					int k ;
					for( k = 0 ; k < 2 ; k++ ) {
						if( i > 0 || k > 0 ) { out[o++] = L',' ; if( ++col == 25 ) { out[o++] = L'\\' ; out[o++] = L'\r' ; out[o++] = L'\n' ; out[o++] = L' ' ; out[o++] = L' ' ; col = 0 ; } }
						out[o++] = hexd[ bytes[k] >> 4 ] ; out[o++] = hexd[ bytes[k] & 15 ] ;
					}
				}
				out[o++] = L'\r' ; out[o++] = L'\n' ;
				changed = 1 ;
				free( val ) ;
				continue ;
			}
			free( val ) ;
			/* never closed: copy the rest through untouched */
			if( o + ( n - ls ) > cap ) { cap = o + ( n - ls ) + 16 ; out = (wchar_t *)realloc( out, cap * sizeof(wchar_t) ) ; }
			memcpy( out + o, w + ls, ( n - ls ) * sizeof(wchar_t) ) ; o += n - ls ; pos = n ;
			continue ;
		}
		if( o + ( pos - ls ) > cap ) { cap = ( o + ( pos - ls ) ) * 2 ; out = (wchar_t *)realloc( out, cap * sizeof(wchar_t) ) ; }
		memcpy( out + o, w + ls, ( pos - ls ) * sizeof(wchar_t) ) ; o += pos - ls ;
	}
	free( buf ) ;
	/* the repaired file goes in only when whole: it was rewritten in place
	 * with fopen "wb", unchecked, so a failed write left a truncated backup
	 * that was then reported as saved */
	if( changed ) {
		char *tmp = dupprintf( "%s.%lu.tmp", filename, (unsigned long)GetCurrentProcessId() ) ;
		int ok = 0 ;
		DWORD err = 0 ;
		if( ( fp = fopen( tmp, "wb" ) ) != NULL ) {
			ok = fwrite( "\xFF\xFE", 1, 2, fp ) == 2 && fwrite( out, sizeof(wchar_t), o, fp ) == o ;
			if( fclose( fp ) != 0 ) ok = 0 ;
			if( !ok ) err = GetLastError() ;
			if( ok && !MoveFileExA( tmp, filename, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH ) ) { ok = 0 ; err = GetLastError() ; }
			if( !ok ) DeleteFileA( tmp ) ;
		} else err = GetLastError() ;
		sfree( tmp ) ;
		free( out ) ;
		if( !ok ) SetLastError( err ? err : ERROR_WRITE_FAULT ) ;
		return ok ;
	}
	free( out ) ;
	return 1 ;
}
/* Export the key to filename with reg.exe, multi-line values repaired. 1 the
 * file is a whole export; 0 not (no file, or not repaired), with the reason
 * for the caller in kitty_sav_save_reason(). */
static char kitty_sav_save_why[1024] = "" ;
const char *kitty_sav_save_reason( void ) { return kitty_sav_save_why ; }
int SaveRegistryKeyEx( HKEY hMainKey, LPCTSTR lpSubKey, const char * filename ) {
	char hive[4096] ;
	const char * root ;
	kitty_sav_save_why[0] = '\0' ;
	if( hMainKey == HKEY_CURRENT_USER ) root = "HKCU" ;
	else if( hMainKey == HKEY_LOCAL_MACHINE ) root = "HKLM" ;
	else return 0 ;
	snprintf( hive, sizeof(hive), "%s\\%s", root, TEXT(lpSubKey) ) ;
	/* reg.exe /y overwrites, but a stale file must not survive a failed export. */
	unlink( filename ) ;
	if( !kitty_reg_tool( "export", hive, filename ) ) {
		if( kitty_reg_tool_winerr )
			snprintf( kitty_sav_save_why, sizeof(kitty_sav_save_why), "%s: %s", filename, kitty_win_reason( (int)kitty_reg_tool_winerr ) ) ;
		else
			snprintf( kitty_sav_save_why, sizeof(kitty_sav_save_why), KT_SAV_EXPORT_FAILED_FMT, filename, (unsigned long)kitty_reg_tool_exit ) ;
		return 0 ;
		}
	if( !kitty_reg_fix_multiline( filename ) ) {
		snprintf( kitty_sav_save_why, sizeof(kitty_sav_save_why), "%s: %s", filename, kitty_win_reason( (int)GetLastError() ) ) ;
		return 0 ;
		}
	return 1 ;
	}

/* a\b into out (n wide characters); 0 when it does not fit */
static int pb_wjoin( wchar_t *out, size_t n, const wchar_t *a, const wchar_t *b ) {
	size_t la = wcslen( a ), lb = wcslen( b ) ;
	if( la + 1 + lb + 1 > n ) return 0 ;
	wcscpy( out, a ) ;
	out[la] = L'\\' ;
	wcscpy( out + la + 1, b ) ;
	return 1 ;
}

/* The copy works in wide characters: the ANSI functions hand back '?' for
 * every character outside the code page, a name with '?' cannot be copied,
 * and so one session named in Cyrillic failed the whole backup - and with it
 * every delete, move and overwrite that waits for one. */
static int portable_backup_copy_tree( const wchar_t *src, const wchar_t *dst ) {
	wchar_t pattern[4096], s[4096], d[4096] ;
	WIN32_FIND_DATAW fd ;
	HANDLE h ;
	DWORD attr = GetFileAttributesW( src ) ;
	if( attr == INVALID_FILE_ATTRIBUTES ) return 1 ;
	if( !(attr & FILE_ATTRIBUTE_DIRECTORY) ) return CopyFileW( src, dst, FALSE ) ? 1 : 0 ;
	CreateDirectoryW( dst, NULL ) ;
	if( !pb_wjoin( pattern, 4096, src, L"*" ) ) { SetLastError( ERROR_FILENAME_EXCED_RANGE ) ; return 0 ; }
	h = FindFirstFileW( pattern, &fd ) ;
	if( h == INVALID_HANDLE_VALUE ) return 1 ;
	do {
		if( !wcscmp(fd.cFileName,L".") || !wcscmp(fd.cFileName,L"..") ) continue ;
		if( !pb_wjoin( s, 4096, src, fd.cFileName ) || !pb_wjoin( d, 4096, dst, fd.cFileName ) ) {
			FindClose(h) ; SetLastError( ERROR_FILENAME_EXCED_RANGE ) ; return 0 ;
		}
		if( fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) {
			if( !portable_backup_copy_tree( s, d ) ) { FindClose(h) ; return 0 ; }
		} else if( !CopyFileW( s, d, FALSE ) ) { FindClose(h) ; return 0 ; }
	} while( FindNextFileW( h, &fd ) ) ;
	FindClose( h ) ;
	return 1 ;
}

struct portable_backup_name { char name[MAX_PATH] ; } ;

static int portable_backup_name_cmp_desc( const void *a, const void *b ) {
	const struct portable_backup_name *aa = (const struct portable_backup_name *)a ;
	const struct portable_backup_name *bb = (const struct portable_backup_name *)b ;
	return strcmp( bb->name, aa->name ) ;
}

static void portable_backup_prune( const char *root, int keep ) {
	char pattern[4096], path[4096] ;
	WIN32_FIND_DATAA fd ;
	HANDLE h ;
	struct portable_backup_name backups[128] ;
	int n = 0, i ;
	if( keep < 1 ) keep = 1 ;
	/* FindFirstFile understands * and ? only - it has no character classes, so
	 * the "[0-9]" this used to carry was matched LITERALLY, nothing was ever
	 * found, and pruning silently never happened (portablebackupcount was
	 * therefore ignored and the backup folders grew without bound). The
	 * "-latest" directory is filtered out by name just below. */
	snprintf( pattern, sizeof(pattern), "%s\\kitty-portable-*", root ) ;
	h = FindFirstFileA( pattern, &fd ) ;
	if( h == INVALID_HANDLE_VALUE ) return ;
	do {
		if( !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ) continue ;
		if( !strcmp(fd.cFileName,"kitty-portable-latest") ) continue ;
		if( n < (int)(sizeof(backups)/sizeof(backups[0])) ) {
			snprintf( backups[n].name, sizeof(backups[n].name), "%s", fd.cFileName ) ;
			n++ ;
		}
	} while( FindNextFileA( h, &fd ) ) ;
	FindClose( h ) ;
	qsort( backups, n, sizeof(backups[0]), portable_backup_name_cmp_desc ) ;
	for( i = keep ; i < n ; i++ ) {
		snprintf( path, sizeof(path), "%s\\%s", root, backups[i].name ) ;
		DelDir( path ) ;
	}
}

/* What must NOT go into a backup. Everything else in the portable directory is
 * treated as configuration and copied, so a store that gains a new folder or
 * file is covered without anyone having to remember a list - the allowlist this
 * replaces had silently omitted "Launcher" ever since the feature shipped.
 *
 * Left out: the Backups tree itself (it holds the older copies, and the
 * destination lives inside it, so copying it would nest backups inside
 * backups); the programs; and the bulky by-products that are not configuration
 * - session logs, and any leftover .dmp diagnostic dumps from the removed
 * savedump feature, which are large and hold secrets. */
static int portable_backup_skip( const WIN32_FIND_DATAW *fd ) {
	static const wchar_t *skipdirs[] = { L"Backups", NULL } ;
	static const wchar_t *skipexts[] = { L".exe", L".dll", L".log", L".dmp", NULL } ;
	const wchar_t *dot ;
	int i ;
	if( !wcscmp(fd->cFileName,L".") || !wcscmp(fd->cFileName,L"..") ) return 1 ;
	if( fd->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) {
		for( i=0 ; skipdirs[i]!=NULL ; i++ )
			if( !_wcsicmp( fd->cFileName, skipdirs[i] ) ) return 1 ;
		return 0 ;
	}
	dot = wcsrchr( fd->cFileName, L'.' ) ;
	if( dot != NULL )
		for( i=0 ; skipexts[i]!=NULL ; i++ )
			if( !_wcsicmp( dot, skipexts[i] ) ) return 1 ;
	return 0 ;
}

/* 1 the whole folder store copied to dst; 0 a copy failed (GetLastError) -
 * the CopyFile results were not looked at, so /savereg called a backup
 * "Saved" whenever the dated folder merely existed. */
static int portable_backup_write_one( const char *dst ) {
	char d[4096] ;
	wchar_t wroot[4096], wdst[4096], pattern[4096], s[4096], wd[4096] ;
	WIN32_FIND_DATAW fd ;
	HANDLE h ;
	int ok = 1 ;
	DWORD err = 0 ;
	DelDir( dst ) ;
	if( !CreateDirectoryA( dst, NULL ) && GetLastError() != ERROR_ALREADY_EXISTS ) return 0 ;
	/* kitty.ini can be resolved from outside the portable directory, so it is
	 * copied by its resolved path and under its canonical name. When it does
	 * live here, the sweep below simply copies it again over the same file. */
	if( KittyIniFile != NULL && strlen(KittyIniFile)>0 && existfile(KittyIniFile) ) {
		snprintf( d, sizeof(d), "%s\\kitty.ini", dst ) ;
		if( !CopyFileA( KittyIniFile, d, FALSE ) ) { ok = 0 ; err = GetLastError() ; }
	}
	if( ConfigDirectory == NULL || strlen(ConfigDirectory) == 0 ) { if( !ok ) SetLastError( err ) ; return ok ; }
	if( !MultiByteToWideChar( CP_ACP, 0, ConfigDirectory, -1, wroot, 4096 ) ||
	    !MultiByteToWideChar( CP_ACP, 0, dst, -1, wdst, 4096 ) ||
	    !pb_wjoin( pattern, 4096, wroot, L"*" ) ) { SetLastError( ERROR_FILENAME_EXCED_RANGE ) ; return 0 ; }
	h = FindFirstFileW( pattern, &fd ) ;
	if( h == INVALID_HANDLE_VALUE ) { if( !ok ) SetLastError( err ) ; return ok ; }
	do {
		if( portable_backup_skip( &fd ) ) continue ;
		if( !pb_wjoin( s, 4096, wroot, fd.cFileName ) || !pb_wjoin( wd, 4096, wdst, fd.cFileName ) ) {
			if( ok ) { ok = 0 ; err = ERROR_FILENAME_EXCED_RANGE ; }
			continue ;
		}
		if( !( fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ? portable_backup_copy_tree( s, wd )
		                                                      : CopyFileW( s, wd, FALSE ) ) && ok ) {
			ok = 0 ; err = GetLastError() ;
		}
	} while( FindNextFileW( h, &fd ) ) ;
	FindClose( h ) ;
	if( !ok ) SetLastError( err ? err : ERROR_WRITE_FAULT ) ;
	return ok ;
}

/* out (may be NULL) gets the dated folder written. Returns as
 * kitty_sav_backup_report. */
static int SavePortableDirBackupEx( char *out, size_t outlen ) {
	char root[4096], latest[4096], stampdir[4096], buffer[64] ;
	int keep = 5 ;
	time_t now ;
	struct tm *tmnow ;
	if( NoKittyFileFlag || ConfigDirectory == NULL || strlen(ConfigDirectory)==0 ) return -2 ;
	/* readonly=yes writes nothing, a backup included - as the registry
	 * paths below and the manual have it. The check was missing here. */
	if( GetReadOnlyFlag() ) return -2 ;
	if( ReadParameterN( INIT_SECTION, KI_PORTABLEBACKUPCOUNT, buffer, sizeof(buffer) ) ) keep = atoi( buffer ) ;
	if( keep <= 0 ) return -1 ;
	if( keep > 50 ) keep = 50 ;
	portable_config_path( root, sizeof(root), "Backups" ) ;
	if( !CreateDirectoryA( root, NULL ) && GetLastError() != ERROR_ALREADY_EXISTS ) {
		if( out ) snprintf( out, outlen, "%s", root ) ;
		return 0 ;
		}
	now = time(NULL) ;
	tmnow = localtime( &now ) ;
	if( tmnow != NULL )
		snprintf( buffer, sizeof(buffer), "%04d%02d%02d-%02d%02d%02d",
			1900+tmnow->tm_year, 1+tmnow->tm_mon, tmnow->tm_mday,
			tmnow->tm_hour, tmnow->tm_min, tmnow->tm_sec ) ;
	else snprintf( buffer, sizeof(buffer), "%ld", (long)now ) ;
	snprintf( stampdir, sizeof(stampdir), "%s\\kitty-portable-%s", root, buffer ) ;
	snprintf( latest, sizeof(latest), "%s\\kitty-portable-latest", root ) ;
	if( out ) snprintf( out, outlen, "%s", stampdir ) ;
	if( !portable_backup_write_one( stampdir ) ) {
		/* a partial dated copy must not pose as a backup, nor push a whole one
		 * out of the retention window */
		DWORD err = GetLastError() ;
		DelDir( stampdir ) ;
		SetLastError( err ) ;
		return 0 ;
		}
	portable_backup_write_one( latest ) ;   /* the convenience copy; the dated one is the backup */
	portable_backup_prune( root, keep ) ;
	return 1 ;
}

static void SavePortableDirBackup( void ) { SavePortableDirBackupEx( NULL, 0 ) ; }

/* Split a sav path into dir / base / ext (e.g. "C:\x\kittynew.sav" ->
 * "C:\x", "kittynew", ".sav"). */
static void sav_split( const char *savfile, char *dir, size_t dl, char *base, size_t bl, char *ext, size_t el ) {
	const char *slash = strrchr( savfile, '\\' ) ;
	const char *fname = slash ? slash + 1 : savfile ;
	const char *dot = strrchr( fname, '.' ) ;
	snprintf( dir, dl, "%.*s", slash ? (int)(slash - savfile) : 1, slash ? savfile : "." ) ;
	if( dot ) { snprintf( base, bl, "%.*s", (int)(dot - fname), fname ) ; snprintf( ext, el, "%s", dot ) ; }
	else { snprintf( base, bl, "%s", fname ) ; if( el ) ext[0] = '\0' ; }
}

/* Build a fresh timestamped backup path (<dir>\<base>-YYYYMMDD-HHMMSS<ext>), so
 * each backup's FILENAME reflects when it was actually written. */
static void sav_timestamped_path( const char *savfile, char *out, size_t outlen ) {
	char dir[4096], base[256], ext[64], stamp[64] ;
	time_t now ; struct tm *tmnow ;
	sav_split( savfile, dir, sizeof(dir), base, sizeof(base), ext, sizeof(ext) ) ;
	now = time(NULL) ; tmnow = localtime( &now ) ;
	if( tmnow != NULL )
		snprintf( stamp, sizeof(stamp), "%04d%02d%02d-%02d%02d%02d",
			1900+tmnow->tm_year, 1+tmnow->tm_mon, tmnow->tm_mday, tmnow->tm_hour, tmnow->tm_min, tmnow->tm_sec ) ;
	else snprintf( stamp, sizeof(stamp), "%ld", (long)now ) ;
	snprintf( out, outlen, "%s\\%s-%s%s", dir, base, stamp, ext ) ;
}

/* Which hive a backup holds, from its first key line
 * "[HKEY_CURRENT_USER\<base>]" or "...\<base>\...": 1 = the hive this run
 * uses, 0 = another one, -1 = unknown (an encrypted backup of the retired
 * format, an unreadable file). A KiClassName=PuTTY copy and a normal one keep
 * their backups in the same folder; reg import writes a backup back to the
 * hive it came from, so restoring the other one's file filled the hive NOT in
 * use and left the one in use empty. */
static int sav_hive_of_file( const char *path ) {
	unsigned char raw[8192] ; char text[8192] ;
	size_t n, i, t = 0, bl ;
	int wide ;
	const char *base = kitty_registry_base(), *p ;
	FILE *fp = fopen( path, "rb" ) ;
	if( fp == NULL ) return -1 ;
	n = fread( raw, 1, sizeof(raw), fp ) ;
	fclose( fp ) ;
	/* reg.exe writes UTF-16LE with a BOM, the retired format ASCII; the key
	 * names compared are ASCII, anything wider becomes '?' */
	wide = ( n >= 2 && raw[0] == 0xFF && raw[1] == 0xFE ) ;
	for( i = wide ? 2 : 0 ; i < n && t < sizeof(text) - 1 ; i += wide ? 2 : 1 )
		text[t++] = ( wide && i + 1 < n && raw[i+1] ) ? '?' : (char)raw[i] ;
	text[t] = '\0' ;
	p = strstr( text, "[HKEY_CURRENT_USER\\" ) ;
	if( p == NULL ) return -1 ;
	p += strlen( "[HKEY_CURRENT_USER\\" ) ;
	bl = strlen( base ) ;
	return ( _strnicmp( p, base, bl ) == 0 && ( p[bl] == ']' || p[bl] == '\\' ) ) ? 1 : 0 ;
}

/* A backup of the hive in use. One whose hive cannot be read counts only for
 * the default hive, which is all the retired format ever held. */
static int sav_is_ours( const char *path ) {
	int h = sav_hive_of_file( path ) ;
	return h == 1 || ( h < 0 && !_stricmp( kitty_registry_base(), PUTTY_REG_POS ) ) ;
}

/* The <base>-*<ext> timestamped backups of the hive in use next to savfile,
 * newest name first, into backups[]; returns how many. */
static int sav_list_ours( const char *savfile, char *dir, size_t dl,
                          struct portable_backup_name *backups, int max ) {
	char base[256], ext[64], pattern[4096], path[4096] ;
	WIN32_FIND_DATAA fd ; HANDLE h ; int n = 0 ;
	sav_split( savfile, dir, dl, base, sizeof(base), ext, sizeof(ext) ) ;
	snprintf( pattern, sizeof(pattern), "%s\\%s-*%s", dir, base, ext ) ;
	h = FindFirstFileA( pattern, &fd ) ;
	if( h == INVALID_HANDLE_VALUE ) return 0 ;
	do {
		if( fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) continue ;
		if( n >= max ) break ;
		snprintf( path, sizeof(path), "%s\\%s", dir, fd.cFileName ) ;
		if( !sav_is_ours( path ) ) continue ;
		snprintf( backups[n].name, sizeof(backups[n].name), "%s", fd.cFileName ) ;
		n++ ;
	} while( FindNextFileA( h, &fd ) ) ;
	FindClose( h ) ;
	qsort( backups, n, sizeof(backups[0]), portable_backup_name_cmp_desc ) ;
	return n ;
}

/* Keep only the `keep` newest timestamped backups of the hive in use next to
 * savfile; the other hive's backups are not counted and not removed. */
static void sav_prune( const char *savfile, int keep ) {
	char dir[4096], path[4096] ;
	struct portable_backup_name backups[256] ;
	int n, i ;
	if( keep < 0 ) keep = 0 ;
	n = sav_list_ours( savfile, dir, sizeof(dir), backups, (int)(sizeof(backups)/sizeof(backups[0])) ) ;
	for( i = keep ; i < n ; i++ ) { snprintf( path, sizeof(path), "%s\\%s", dir, backups[i].name ) ; unlink( path ) ; }
}

/* Newest timestamped backup of the hive in use next to savfile (for the
 * first-run restore and /loadreg). Returns 1 + path in out, else 0. */
static int sav_find_newest( const char *savfile, char *out, size_t outlen ) {
	char dir[4096] ;
	struct portable_backup_name backups[256] ;
	if( sav_list_ours( savfile, dir, sizeof(dir), backups, (int)(sizeof(backups)/sizeof(backups[0])) ) == 0 ) return 0 ;
	snprintf( out, outlen, "%s\\%s", dir, backups[0].name ) ;
	return 1 ;
}

/* Backups written before the DEFAULT_SAV_FILE shadowing was fixed
 * are named kitty-YYYYMMDD-HHMMSS.sav, because the intended kittynew.sav default
 * never took effect. Once it does, sav_find_newest() no longer sees them, so a
 * machine upgrading from such a build would silently skip its first-run restore.
 * Resolve the legacy name too - for READING ONLY; nothing is ever written back
 * under it, and only when the sav path is still the default (a user-configured
 * sav= is taken literally). Returns 1 + path in out. */
#define LEGACY_SAV_FILE "kitty.sav"
static int sav_find_newest_legacy( const char *savfile, char *out, size_t outlen ) {
	char dir[4096], base[256], ext[64], legacy[4096] ;
	char ddir[16], dbase[256], dext[64] ;
	sav_split( savfile, dir, sizeof(dir), base, sizeof(base), ext, sizeof(ext) ) ;
	sav_split( DEFAULT_SAV_FILE, ddir, sizeof(ddir), dbase, sizeof(dbase), dext, sizeof(dext) ) ;
	if( strcmp( base, dbase ) ) return 0 ;              /* custom sav=: no fallback */
	snprintf( legacy, sizeof(legacy), "%s\\%s", dir, LEGACY_SAV_FILE ) ;
	if( sav_find_newest( legacy, out, outlen ) ) return 1 ;
	if( existfile( legacy ) && sav_is_ours( legacy ) ) { snprintf( out, outlen, "%s", legacy ) ; return 1 ; }
	return 0 ;
}

/* The one place that decides WHICH backup to restore from: newest timestamped,
 * else the fixed-name file, else the legacy name above - each only a backup of
 * the hive in use. */
int sav_find_for_restore( const char *savfile, char *out, size_t outlen ) {
	if( sav_find_newest( savfile, out, outlen ) ) return 1 ;
	if( existfile( savfile ) && sav_is_ours( savfile ) ) { snprintf( out, outlen, "%s", savfile ) ; return 1 ; }
	return sav_find_newest_legacy( savfile, out, outlen ) ;
}

/* The configuration password (/configpassword) is retired: it encrypted the
 * .sav while storing its own key in the CLEAR in the hive, and the kitty.ini
 * copy was only obfuscated with a constant compiled into every build. Nothing
 * reads either value any more, so remove them rather than leave a plaintext
 * secret lying about. Self-limiting: both deletes are skipped when absent. */
void RetireConfigPasswordLeftovers( void ) {
	char buf[4096] ;
	/* the registry only for a copy that keeps its settings there: a portable
	 * copy (folder store) leaves HKCU alone - an installed copy cleans its
	 * own hive at its next start */
	if( IniFileFlag != SAVEMODE_DIR
	    && GetValueDataN( HKEY_CURRENT_USER, kitty_registry_base(), KI_PASSWORD, buf, sizeof(buf) ) != NULL )
		RegDelValue( HKEY_CURRENT_USER, kitty_registry_base(), KI_PASSWORD ) ;
	if( ( KittyIniFile != NULL ) && !GetReadOnlyFlag()
	    && readINI( KittyIniFile, INIT_SECTION, KI_PASSWORD, buf, sizeof(buf) ) )
		delINI( KittyIniFile, INIT_SECTION, KI_PASSWORD ) ;
	memset( buf, 0, sizeof(buf) ) ;
	}

/*
 * CountUp() used to write five bookkeeping values on every run - KiLastUp,
 * KiLastUH, KiSess, KiVers, KiPath - that nothing ever read back. The writes are
 * gone; this removes them from stores that already have them, because stopping
 * the writes on its own would just freeze stale values in place for ever.
 *
 * Two of them mattered more than the rest. KiLastUH held your Windows username
 * and computer name, and KiVers your OS version, both scrambled with the
 * compiled-in public constant - which is obfuscation, not encryption, and which
 * made the leak invisible to the one person who would care: kitty.ini gets
 * shared, and nobody reading their own file could tell those two lines were
 * their username and machine.
 *
 * Self-limiting, like RetireConfigPasswordLeftovers above: every delete is
 * skipped when the value is absent, so this costs nothing on a clean store and
 * runs at most once usefully.
 */
void RetireCountUpLeftovers( void ) {
	/* char* rather than const char*: RegDelValue takes LPTSTR. */
	static char *const dead[] = {
		KR_KILASTUP, KR_KILASTUH, KR_KISESS, KR_KISESS_COMMENTED, KR_KIVERS, KR_KIPATH } ;
	char buf[4096] ;
	size_t i ;
	for( i = 0 ; i < lenof(dead) ; i++ ) {
		if( IniFileFlag != SAVEMODE_DIR   /* a portable copy leaves HKCU alone (above) */
		    && GetValueDataN( HKEY_CURRENT_USER, kitty_registry_base(), dead[i], buf, sizeof(buf) ) != NULL )
			RegDelValue( HKEY_CURRENT_USER, kitty_registry_base(), dead[i] ) ;
		if( ( KittyIniFile != NULL ) && !GetReadOnlyFlag()
		    && readINI( KittyIniFile, INIT_SECTION, dead[i], buf, sizeof(buf) ) )
			delINI( KittyIniFile, INIT_SECTION, dead[i] ) ;
	}
	memset( buf, 0, sizeof(buf) ) ;
	}

/* The export spawns reg.exe, so it runs on a worker thread rather than making
 * the config box wait for it - upstream did the same with _beginthread. One at
 * a time: a request arriving while one is in flight is dropped, since it would
 * only write a near-identical snapshot a moment later. */
static LONG sav_worker_busy = 0 ;

struct sav_job { char dated[4096] ; char base[4096] ; int keep ; } ;

static DWORD WINAPI sav_worker( LPVOID param ) {
	struct sav_job * j = (struct sav_job *)param ;
	/* The hive this run USES, not the compile-time one: with KiClassName=PuTTY
	 * the sessions live in PuTTY's hive, and exporting kapper.net\KiTTY produced
	 * a "backup" containing nothing but our migration markers. Worse than
	 * useless - it looks like a backup right up to the day someone needs it. */
	{ extern const char *kitty_registry_base( void ) ;
	  /* never prune for a backup that failed - as sav_backup's own path */
	  if( SaveRegistryKeyEx( HKEY_CURRENT_USER, kitty_registry_base(), j->dated ) )
		sav_prune( j->base, j->keep ) ; }
	free( j ) ;
	InterlockedExchange( &sav_worker_busy, 0 ) ;
	return 0 ;
	}

/* async=0 blocks until the backup is on disk. Callers that are about to DESTROY
 * something - delete a session or a folder, overwrite a saved session - must use
 * that: the whole point of those backups is to capture the store as it was
 * BEFORE the change, and a worker thread could just as easily snapshot it after.
 * Portable (dir) mode is always synchronous; it copies files rather than
 * spawning a process, and its backup has the same ordering requirement. */
/* 1 the backup is written (or there is nothing to do: unchanged store,
 * backups off, no backup file, a worker started); 0 the synchronous one
 * failed - its reason in kitty_sav_save_reason(). */
static int sav_backup( int async ) {
	int keep = 5 ; char kb[64] ;
	/* Routine snapshots happen only when the store actually changed since the
	 * last one. The path that triggers them is also what plain Enter in the
	 * session list goes through, so without this a handful of opens would push
	 * every interesting backup out of the retention window. The blocking variant
	 * deliberately ignores the flag: it runs BEFORE a destructive edit, where
	 * capturing the prior state is the whole point whether or not anything was
	 * edited first. */
	if( async && !kitty_store_take_dirty() ) return 1 ;
	/* readonly=yes: no backup is written in any mode - savemode=file's
	 * write-back to its .sav included. KITTY_RO_REFUSED counts as "go on":
	 * the change it would precede is refused by the store itself. */
	if( GetReadOnlyFlag() ) return KITTY_RO_REFUSED ;
	if( IniFileFlag == SAVEMODE_DIR ) {
		char where[MAX_PATH * 2] ;
		int r = SavePortableDirBackupEx( where, sizeof(where) ) ;
		if( r == 0 ) {
			snprintf( kitty_sav_save_why, sizeof(kitty_sav_save_why), "%s: %s", where, kitty_win_reason( (int)GetLastError() ) ) ;
			return 0 ;
			}
		return 1 ;
		}
	if( NoKittyFileFlag || (KittySavFile==NULL) ) return 1 ;
	if( strlen(KittySavFile)==0 ) return 1 ;

	if( ReadParameterN( INIT_SECTION, KI_SAVBACKUPCOUNT, kb, sizeof(kb) ) ) keep = atoi( kb ) ;
	if( keep <= 0 ) return 1 ;            /* savbackupcount=0 disables the backup */
	if( keep > 50 ) keep = 50 ;
	/* Write a FRESH timestamped file now (kittynew-YYYYMMDD-HHMMSS.sav) so its
	 * name matches its content's save time, then keep only the newest `keep`.
	 * (The old scheme wrote a fixed kittynew.sav and renamed the PREVIOUS one to
	 * a now-stamped name - the timestamp then lied about the content's age.) */
	{ char dated[4096] ;
	  sav_timestamped_path( KittySavFile, dated, sizeof(dated) ) ;
	  if( !async ) {
		int ok = SaveRegistryKeyEx( HKEY_CURRENT_USER, kitty_registry_base(), dated ) ;
		if( ok ) sav_prune( KittySavFile, keep ) ;   /* never prune for a backup that failed */
		return ok ;
		}
	  if( InterlockedCompareExchange( &sav_worker_busy, 1, 0 ) != 0 ) return 1 ;
	  { struct sav_job * j = (struct sav_job *)malloc( sizeof(*j) ) ;
	    HANDLE th ;
	    if( j == NULL ) { InterlockedExchange( &sav_worker_busy, 0 ) ; return 1 ; }
	    snprintf( j->dated, sizeof(j->dated), "%s", dated ) ;
	    snprintf( j->base, sizeof(j->base), "%s", KittySavFile ) ;
	    j->keep = keep ;
	    th = CreateThread( NULL, 0, sav_worker, j, 0, NULL ) ;
	    if( th == NULL ) { free( j ) ; InterlockedExchange( &sav_worker_busy, 0 ) ; return 1 ; }
	    CloseHandle( th ) ;
	  }
	}
	return 1 ;
	}

/* Routine snapshot after a deliberate config change - never blocks the UI. */
void SaveRegistryKey( void ) { sav_backup( 1 ) ; }

/* Snapshot that must be on disk before the caller changes anything: 1 it is
 * (or backups are off), 0 it failed - kitty_sav_save_reason() holds the reason. */
int SaveRegistryKeyNow( void ) { return sav_backup( 0 ) ; }

/* The backup before a delete, a move or an overwrite of saved sessions. 1 go
 * on: it is written, backups are off, or the user chose to go on without
 * one; 0 stop - the change is not made. A failed backup used to pass
 * unnoticed and the change ran with nothing to come back to. The question
 * goes where kitty_reach() puts it, No being the default. Where nobody can
 * answer it (beside a live session, nobody there) the change is not made
 * and the stop is reported - unless the caller reports it itself
 * (own_report: its result line takes kitty_backup_not_done()), so that it
 * is not reported twice. */
int kitty_backup_before( HWND owner, int own_report ) {
	char *q, *nd ;
	int go ;
	if( SaveRegistryKeyNow() ) return 1 ;
	q = dupprintf( KT_BACKUP_BEFORE_Q_FMT, kitty_sav_save_reason() ) ;
	nd = own_report ? NULL : dupprintf( KT_BACKUP_BEFORE_NOT_DONE_FMT, kitty_sav_save_reason() ) ;
	go = kitty_reach_confirm( owner, KT_CAP_BACKUP, q, nd ) ;
	sfree( q ) ;
	sfree( nd ) ;
	return go ;
	}
/* The text a caller with its own result line shows after a stop. snewn'd. */
char *kitty_backup_not_done( void ) {
	return dupprintf( KT_BACKUP_BEFORE_NOT_DONE_FMT, kitty_sav_save_reason() ) ;
	}

/* /savereg. The routine snapshot (SaveRegistryKey) writes only when the store
 * changed since the last one, and on a worker - right for the background,
 * wrong for a command typed to get a backup: it often wrote nothing, and reported
 * nothing. This one always writes, waits, and reports the file. */
int kitty_sav_backup_report( char * path, size_t n ) {
	int keep = 5 ; char kb[64] ;
	path[0] = '\0' ;
	kitty_sav_save_why[0] = '\0' ;
	if( IniFileFlag == SAVEMODE_DIR ) {
		int r = SavePortableDirBackupEx( path, n ) ;
		if( r == 0 )
			snprintf( kitty_sav_save_why, sizeof(kitty_sav_save_why), "%s: %s", path, kitty_win_reason( (int)GetLastError() ) ) ;
		return r ;
		}
	if( NoKittyFileFlag || (KittySavFile==NULL) || !KittySavFile[0] ) return -2 ;
	if( GetReadOnlyFlag() ) return -2 ;   /* readonly=yes, as the folder store's above */
	if( ReadParameterN( INIT_SECTION, KI_SAVBACKUPCOUNT, kb, sizeof(kb) ) ) keep = atoi( kb ) ;
	if( keep <= 0 ) return -1 ;
	if( keep > 50 ) keep = 50 ;
	sav_timestamped_path( KittySavFile, path, n ) ;
	if( !SaveRegistryKeyEx( HKEY_CURRENT_USER, kitty_registry_base(), path ) ) return 0 ;
	sav_prune( KittySavFile, keep ) ;
	return 1 ;
	}

void kitty_sav_dir( char * out, size_t n ) {
	char base[256], ext[64] ;
	if( (KittySavFile==NULL) || !KittySavFile[0] ) { snprintf( out, n, "%s", InitialDirectory ) ; return ; }
	sav_split( KittySavFile, out, n, base, sizeof(base), ext, sizeof(ext) ) ;
	}

/* Load the registry key. 1 loaded; 0 not, the file and the reason in
 * kitty_sav_load_reason() - it returned nothing and threw the import's result
 * away, so /loadreg showed "Loaded:" and a start-up restore went on as if the
 * sessions were there, whatever reg.exe had done. A file that is not there is
 * a failure too (the caller decides whether that is news). */
/* Set by LoadRegistryKeyLive: a load from a live session reports and returns
 * where the start-up load ends the process. */
static int sav_load_live = 0 ;
static int sav_load_fail_text( FILE *fp, HKEY hKey, const char *path, const char *why ) {
	if( hKey != NULL ) RegCloseKey( hKey ) ;
	if( fp != NULL ) fclose( fp ) ;
	snprintf( kitty_sav_load_why, sizeof(kitty_sav_load_why), "%s: %s", path, why ) ;
	return 0 ;
	}

int LoadRegistryKey( HWND hdlg ) { // hdlg = progress dialog (NULL = none)
	FILE *fp ;
	HKEY hKey = NULL ;
	char buffer[4096], KeyName[1024] = "", ValueName[1024], *Value ;
	char savpath[4096] ;
	int nb=0, ok = 1 ;
	LONG rc ;
	DWORD err = 0 ;

	kitty_sav_load_why[0] = '\0' ;
	if( KittySavFile==NULL || strlen(KittySavFile)==0 )
		return sav_load_fail_win( "(no backup file set)", ERROR_FILE_NOT_FOUND ) ;
	/* readonly=yes: a backup is not loaded into the hive (/loadreg, the
	 * first-run restore) - except savemode=file's start, which loads its
	 * kitty.sav inside kitty_store_ro_allow() */
	if( kitty_store_ro_regpath( 1, kitty_registry_base() ) ) return KITTY_RO_REFUSED ;
	snprintf( savpath, sizeof(savpath), "%s", KittySavFile ) ;

	if( ( fp = fopen( savpath, "rb" ) ) == NULL ) {
		err = GetLastError() ;
		/* Nothing under the current name: a store written by a pre-fix build is
		 * still called kitty*.sav. Read it rather than come up empty - this path
		 * matters most in savemode=file, where the .sav IS the session store. */
		if( !sav_find_newest_legacy( KittySavFile, savpath, sizeof(savpath) ) ) return sav_load_fail_win( KittySavFile, err ) ;
		if( ( fp = fopen( savpath, "rb" ) ) == NULL ) return sav_load_fail_win( savpath, GetLastError() ) ;
		}

	/* Current backups are what reg.exe writes: UTF-16LE with a BOM. Let Windows
	 * import those, so every value type is restored exactly as exported. Older
	 * backups are our own ASCII format (optionally encrypted with the retired
	 * configuration password) and are still parsed below. */
	{	unsigned char bom[2] ;
		if( ( fread( bom, 1, 2, fp ) == 2 ) && ( bom[0] == 0xFF ) && ( bom[1] == 0xFE ) ) {
			fclose( fp ) ;
			if( hdlg != NULL ) InfoBoxSetText( hdlg, KT_MAIN_INFO_LOADING_SESSIONS ) ;
			if( !kitty_reg_import( savpath ) ) return sav_load_fail_regexe( savpath ) ;
			return 1 ;
			}
		rewind( fp ) ;
		}
	while( fgets( buffer, 4096, fp ) != NULL ) {
		str_rtrim( buffer, "\n\r \t" ) ;
		
		// Check whether the file is encrypted
		if( nb == 0 ) {
			if( strcmp( buffer, "Windows Registry Editor Version 5.00" ) ) {
				/* A live session gets no password prompt: the box is modal
				 * and would stop the session while it is open. The result
				 * line reports why nothing was loaded. */
				if( sav_load_live ) return sav_load_fail_text( fp, hKey, savpath, KT_MAIN_SAV_ENCRYPTED_LIVE ) ;
				GetAndSendLinePassword( NULL ) ;
				if( GetInputBoxResult() == NULL || strlen( GetInputBoxResult() ) == 0 ) {
					exit(0) ;
					}
				strcpy( PasswordConf, GetInputBoxResult() ) ;
				decryptstring( GetCryptSaltFlag(), buffer, PasswordConf ) ;
				if( strcmp( buffer, "Windows Registry Editor Version 5.00" ) ) {
					PasswordConf[0] = '\0' ;
					MessageBox( NULL, KT_MAIN_WRONG_PASSWORD, KT_CAP_ERROR, MB_OK|MB_ICONERROR ) ;
					exit(1) ;
					}
				/* Decrypt-only: the configuration password is retired, so the
				 * value is never written back to the hive or kitty.ini. */
				}
			}
		nb++ ;
		if( strlen( PasswordConf ) > 0 ) {
			decryptstring( GetCryptSaltFlag(), buffer, PasswordConf ) ;
			}
			
		if( strlen( buffer ) == 0 ) ;
		if( (buffer[0]=='[') && (buffer[strlen(buffer)-1]==']') ) {
			snprintf( KeyName, sizeof(KeyName), "%s", buffer+19 ) ; // +19 to strip [HKEY_CURRENT_USER
			{ size_t _kl=strlen(KeyName); if(_kl>0) KeyName[_kl-1] = '\0' ; }
			if( hKey != NULL ) { RegCloseKey( hKey ) ; hKey = NULL ; }
			if( RegOpenKeyEx( HKEY_CURRENT_USER, TEXT(KeyName), 0, KEY_WRITE, &hKey) != ERROR_SUCCESS )
				{
					if( hdlg != NULL ) {
						snprintf( buffer, sizeof(buffer), KT_MAIN_INFO_LOADING_KEY, KeyName ) ;
						InfoBoxSetText( hdlg, buffer ) ;
						}
					if( ( rc = RegCreateKey( HKEY_CURRENT_USER, TEXT(KeyName), &hKey ) ) != ERROR_SUCCESS ) {
						hKey = NULL ;
						if( ok ) { ok = 0 ; err = (DWORD)rc ; }
						}
					}
			}
		else {
			if( ( Value = strstr( buffer, "=" ) ) != NULL ) {
				snprintf( ValueName, sizeof(ValueName), "%s", buffer+1 ) ;
				{ int _vni = (int)(Value-buffer-2); if( _vni >= 0 && _vni < (int)sizeof(ValueName) ) ValueName[ _vni ] = '\0' ; }
				Value++;
			if( Value[0] == '\"' ) { // REG_SZ
			  	Value++;
			  	{ size_t _vl=strlen(Value); if(_vl>0) Value[_vl-1] = '\0' ; }
				DelDoubleBackSlash( Value ) ;
			  	rc = hKey ? RegSetValueEx( hKey, TEXT( ValueName ), 0, REG_SZ, (LPBYTE)Value, strlen(Value)+1 ) : ERROR_INVALID_HANDLE ;
				if( rc != ERROR_SUCCESS && ok ) { ok = 0 ; err = (DWORD)rc ; }
			  	}
			else if( strstr(Value,"dword:") == Value ) { //REG_DWORD
				int dwData = 0 ;
				Value = Value + 6 ;
				sscanf( Value, "%08x", (int*)&dwData ) ;
				rc = hKey ? RegSetValueEx( hKey, TEXT( ValueName ), 0, REG_DWORD, (LPBYTE)&dwData, sizeof(DWORD) ) : ERROR_INVALID_HANDLE ;
				if( rc != ERROR_SUCCESS && ok ) { ok = 0 ; err = (DWORD)rc ; }
				}
			else { // error
				if( sav_load_live ) {
					PasswordConf[0] = '\0' ;
					return sav_load_fail_text( fp, hKey, savpath, KT_MAIN_SAV_UNKNOWN_TYPE ) ;
					}
				MessageBox( NULL, KT_MAIN_SAV_UNKNOWN_TYPE, KT_CAP_ERROR, MB_OK|MB_ICONERROR );
				exit( 1 ) ;
				}
			}
			}
		}
	if( hKey != NULL ) { RegCloseKey( hKey ) ; hKey = NULL ; }
	fclose( fp ) ;
	if( !ok ) return sav_load_fail_win( savpath, err ) ;
	return 1 ;
	}

int LoadRegistryKeyLive( void ) {
	int r ;
	sav_load_live = 1 ;
	r = LoadRegistryKey( NULL ) ;
	sav_load_live = 0 ;
	return r ;
	}

void DelRegistryKey( void ) {
	/* readonly=yes: RegDelTree refuses the vendor key above the store */
	RegDelTree( HKEY_CURRENT_USER, TEXT(PUTTY_REG_PARENT) ) ;
	}
