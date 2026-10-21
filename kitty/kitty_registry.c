/*
 * kitty_registry.c - the registry helpers the KiTTY additions are built on.
 * Three layers live here: small wrappers around the Win32 registry API
 * (read a value, test or create a key, delete a value or a whole tree, copy
 * a tree, export a key through reg.exe), the one-time hive migrations and
 * repairs run at startup (the old KiTTY namespace, the ShiftedArrowKeys
 * default, the retired SCPAutoPwd option) plus the PuTTY hive clean-up, and
 * the shell integration: registering and removing the URL protocol handlers
 * and the file association, with their console-or-message-box reports.
 */
#include <winsock2.h>   /* before kitty_registry.h's windows.h: putty.h pulls it in further down */
#include "kitty_registry.h"

#include "kitty_oldwin.h"   /* APIs newer than the oldest Windows we load on */
#include "kitty_msgbox.h"   /* themed MessageBox routing */
#include "kitty_oldwin_reg.h"   /* XP: RegDeleteTree/RegGetValue via oldwin */
#include "kitty_text.h"     /* shared captions */
#include "kitty_winerr.h"   /* kitty_win_reason: Windows' reason, for the reports */
#include "kitty_dlgbox.h"   /* kitty_info_modeless: an outcome beside a live session */
#include "kitty_inikeys.h"  /* KI_* and KR_*: the stored value names */
#include "kitty_store.h"
#include "kitty_tools.h"
#include "kitty.h"
#include "kitty_inifile.h"  /* kitty_store_ro_regpath: readonly=yes */
#include "kitty_buildlabel.h"   /* KITTY_TEST_BUILD_LABEL: KITTY_NO_SETTINGS_LAUNCH */
#include <objbase.h>        /* CoInitializeEx: the ms-settings: launch */
#include <stdarg.h>
/* kitty_tools.c; declared locally because this file deliberately includes
 * only kitty_registry.h (see the MigrateOldKittyHive rationale below). */
/* kitty.c, same reason: CreateSSHHandler() needs to know whether a portable
 * copy was configured to use the registry (kitty_store.h's SAVEMODE_REG). */
#ifndef SAVEMODE_REG
#define SAVEMODE_REG 0
#endif
// Bounded variant: never writes more than `rsize` bytes (final NUL included)
// into rValue; a value that is too long is truncated instead of overflowing.
char * GetValueDataN(HKEY hkTopKey, const char * lpSubKey, const char * lpValueName, char * rValue, size_t rsize){
    HKEY hkKey;
    DWORD lpType, dwDataSize = cstMaxRegLength;

  //Buffer receiving the value read from the registry key
	/* On the stack: this is the read behind every ReadParameter, and a
	 * malloc/free pair per call added up on the paths that read often. */
	unsigned char lpData[ cstMaxRegLength + 1 ] ; // +1 for forced NUL
	if( rValue == NULL || rsize == 0 ) { return NULL ; }

    rValue[0] = '\0';
  //Read the registry key; if that works, carry on...
    if (RegOpenKeyEx(hkTopKey,lpSubKey,0,KEY_READ,&hkKey) == ERROR_SUCCESS){

      if (RegQueryValueEx(hkKey,lpValueName,NULL,&lpType,lpData,&dwDataSize) == ERROR_SUCCESS){
      // RegQueryValueEx does NOT guarantee NUL-termination for the *_SZ types and a
      // value can fill the whole buffer; force a terminator so the copies below
      // cannot over-read past the data.
        if( dwDataSize > cstMaxRegLength ) dwDataSize = cstMaxRegLength ;
        lpData[dwDataSize] = '\0' ;
      //decode the different registry value types
        switch ((int)lpType){

          case REG_BINARY:
               if( dwDataSize >= 4 ) {   // a.b.c.d needs 4 bytes; don't over-read short values
               snprintf( rValue, rsize, "%u.%u.%u.%u",
                         (unsigned)lpData[0], (unsigned)lpData[1],
                         (unsigned)lpData[2], (unsigned)lpData[3] ) ;
               }
               break;

          case REG_DWORD:
               snprintf( rValue, rsize, "%d", *(int*)(lpData) ) ;
               break;

          case REG_EXPAND_SZ:
          case REG_MULTI_SZ:
          case REG_SZ: {
               size_t n = strlen( (char*)lpData ) ;
               if( n >= rsize ) n = rsize - 1 ;
               memcpy( rValue, lpData, n ) ;
               rValue[n] = '\0' ;
               break;
          }
        }//end switch
      }//end if
      else { RegCloseKey(hkKey); return NULL ; }
       RegCloseKey(hkKey);

    }//end if
    else { return NULL ; }
    return rValue;
  }//end function

/* Compat: the old unbounded signature -- the destination buffer MUST be at
 * least cstMaxRegLength+2 bytes. Prefer GetValueDataN( ..., sizeof(buf) ). */
char * GetValueData(HKEY hkTopKey, const char * lpSubKey, const char * lpValueName, char * rValue){
    return GetValueDataN( hkTopKey, lpSubKey, lpValueName, rValue, cstMaxRegLength+2 ) ;
}

// Test whether a key exists
int RegTestKey( HKEY hMainKey, LPCTSTR lpSubKey ) {
	HKEY hKey ;
	if( lpSubKey == NULL ) return 1 ;
	if( strlen( lpSubKey ) == 0 ) return 1 ;
	if( RegOpenKeyEx( hMainKey, TEXT(lpSubKey), 0, KEY_WRITE, &hKey) != ERROR_SUCCESS ) return 0 ;
	RegCloseKey( hKey ) ;
	return 1 ;
	}
	
// Return the number of subkeys
int RegCountKey( HKEY hMainKey, LPCTSTR lpSubKey ) {
	HKEY hKey ;
	TCHAR    achClass[MAX_PATH] = TEXT("");
	DWORD    cchClassName = MAX_PATH, cSubKeys=0, cbMaxSubKey, cchMaxClass, cValues, cchMaxValue, cbMaxValueData, cbSecurityDescriptor ;
	FILETIME ftLastWriteTime;

	int nb = 0 ;
	if( RegOpenKeyEx( hMainKey, TEXT(lpSubKey), 0, KEY_READ, &hKey) != ERROR_SUCCESS ) return 0 ;
	
	RegQueryInfoKey( hKey, achClass, &cchClassName, NULL, &cSubKeys, &cbMaxSubKey, &cchMaxClass
		, &cValues, &cchMaxValue, &cbMaxValueData, &cbSecurityDescriptor, &ftLastWriteTime) ;
	nb = cSubKeys ;
	RegCloseKey( hKey ) ;
	return nb ;
	}

	// Test whether a key or a value exists and create it otherwise
	// KiTTY: returns 1 on success, 0 if the key could not be opened or created
	// or the value not written. Writing under HKEY_CLASSES_ROOT lands in
	// HKEY_LOCAL_MACHINE and needs elevation, so failure here is ordinary and
	// callers that register file/URL associations must be able to report it.
	// (Both functions also used to carry on with an UNINITIALISED handle when
	// the create failed, which is what made the failure silent.)
	// A key of the settings store under readonly=yes: nothing is written,
	// KITTY_RO_REFUSED (non-zero: a caller testing for failure stays silent).
	// The same holds for every writer below (RegTestOrCreateDWORD,
	// RegDelValue, RegDelTree, kitty_RegCopyTree's destination).
int RegTestOrCreate( HKEY hMainKey, LPCTSTR lpSubKey, LPCTSTR name, LPCTSTR value ) {
	HKEY hKey = NULL ;
	LONG rc ;
	if( lpSubKey == NULL ) return 0 ;
	if( strlen( lpSubKey ) == 0 ) return 0 ;
	if( kitty_store_ro_regpath( hMainKey == HKEY_CURRENT_USER, lpSubKey ) ) return KITTY_RO_REFUSED ;
	if( RegOpenKeyEx( hMainKey, TEXT(lpSubKey), 0, KEY_WRITE, &hKey) != ERROR_SUCCESS ) {
		/* the reason stays in GetLastError() for the caller's report */
		if( ( rc = RegCreateKey( hMainKey, lpSubKey, &hKey ) ) != ERROR_SUCCESS ) { SetLastError( (DWORD)rc ) ; return 0 ; }
		}
	rc = ERROR_SUCCESS ;
	if( name != NULL )
		rc = RegSetValueEx( hKey, TEXT( name ), 0, REG_SZ, (const BYTE *)value, strlen(value)+1 ) ;
	RegCloseKey( hKey ) ;
	if( rc != ERROR_SUCCESS ) { SetLastError( (DWORD)rc ) ; return 0 ; }
	return 1 ;
	}

// Test whether a key or a DWORD value exists and create it otherwise
int RegTestOrCreateDWORD( HKEY hMainKey, LPCTSTR lpSubKey, LPCTSTR name, DWORD value ) {
	HKEY hKey = NULL ;
	LONG rc ;
	if( lpSubKey == NULL ) return 0 ;
	if( strlen( lpSubKey ) == 0 ) return 0 ;
	if( kitty_store_ro_regpath( hMainKey == HKEY_CURRENT_USER, lpSubKey ) ) return KITTY_RO_REFUSED ;
	if( RegOpenKeyEx( hMainKey, TEXT(lpSubKey), 0, KEY_WRITE, &hKey) != ERROR_SUCCESS ) {
		/* the reason stays in GetLastError() for the caller's report */
		if( ( rc = RegCreateKey( hMainKey, lpSubKey, &hKey ) ) != ERROR_SUCCESS ) { SetLastError( (DWORD)rc ) ; return 0 ; }
		}
	rc = ERROR_SUCCESS ;
	if( name != NULL )
		rc = RegSetValueEx( hKey, TEXT( name ), 0, REG_DWORD, (LPBYTE)&value, sizeof(DWORD) ) ;
	RegCloseKey( hKey ) ;
	if( rc != ERROR_SUCCESS ) { SetLastError( (DWORD)rc ) ; return 0 ; }
	return 1 ;
	}
	

// Set a value in every session (oldvalue==NULL) or only where it holds oldvalue
void RegUpdateAllSessions( HKEY hMainKey, LPCTSTR lpSubKey, LPCTSTR name, LPCTSTR oldvalue, LPCTSTR value  ) {
	HKEY hKey ;
	TCHAR    achClass[MAX_PATH] = TEXT(""), achKey[MAX_KEY_LENGTH]; 
	DWORD    cchClassName = MAX_PATH, cSubKeys=0, cbMaxSubKey, cbName=MAX_KEY_LENGTH, cchMaxClass, cValues, cchMaxValue, cbMaxValueData, cbSecurityDescriptor ;
	FILETIME ftLastWriteTime;
	
	int i, retCode ;
	if( RegOpenKeyEx( hMainKey, TEXT(lpSubKey), 0, KEY_READ, &hKey) != ERROR_SUCCESS ) return ;
	RegQueryInfoKey( hKey, achClass, &cchClassName, NULL, &cSubKeys, &cbMaxSubKey, &cchMaxClass
		, &cValues, &cchMaxValue, &cbMaxValueData, &cbSecurityDescriptor, &ftLastWriteTime) ;

	if (cSubKeys) {
		for (i=0; i<cSubKeys; i++) {
			retCode = RegEnumKeyEx(hKey, i, achKey, &cbName, NULL, NULL, NULL, &ftLastWriteTime); 
			if (retCode == ERROR_SUCCESS) {
				char buffer[MAX_KEY_LENGTH] ;
				char previousvalue[cstMaxRegLength+2] ;
				snprintf( buffer, sizeof(buffer), "%s\\%s", lpSubKey, achKey ) ;
				GetValueDataN( hMainKey, buffer, name, previousvalue, sizeof(previousvalue) ) ;
				if( (oldvalue==NULL) || ( !strcmp(previousvalue,oldvalue)) )
					MessageBox(NULL,achKey,KT_CAP_INFO,MB_OK);
					//RegTestOrCreate( hMainKey, buffer, name, value ) ;
			}
		}
	}
}
	
// Export a whole registry key
void QuerySubKey( HKEY hMainKey, LPCTSTR lpSubKey, FILE * fp_out, char * text  ) { 
	HKEY hKey ;
    TCHAR    achKey[MAX_KEY_LENGTH];   // buffer for subkey name
    DWORD    cbName;                   // size of name string 
    TCHAR    achClass[MAX_PATH] = TEXT("");  // buffer for class name 
    DWORD    cchClassName = MAX_PATH;  // size of class string 
    DWORD    cSubKeys=0;               // number of subkeys 
    DWORD    cbMaxSubKey;              // longest subkey size 
    DWORD    cchMaxClass;              // longest class string 
    DWORD    cValues;              // number of values for key 
    DWORD    cchMaxValue;          // longest value name 
    DWORD    cbMaxValueData;       // longest value data 
    DWORD    cbSecurityDescriptor; // size of security descriptor 
    FILETIME ftLastWriteTime;      // last write time 
    DWORD i, retCode; 
	
	char * buffer = NULL ;

	// Open the key
	if( RegOpenKeyEx( hMainKey, TEXT(lpSubKey), 0, KEY_READ, &hKey) != ERROR_SUCCESS ) return ;

    // Get the class name and the value count.
    retCode = RegQueryInfoKey(
        hKey,                    // key handle
        achClass,                // buffer for class name
        &cchClassName,           // size of class string
        NULL,                    // reserved
        &cSubKeys,               // number of subkeys
        &cbMaxSubKey,            // longest subkey size
        &cchMaxClass,            // longest class string
        &cValues,                // number of values for this key
        &cchMaxValue,            // longest value name
        &cbMaxValueData,         // longest value data
        &cbSecurityDescriptor,   // security descriptor
        &ftLastWriteTime);       // last write time

	// Enumerate the subkeys, until RegEnumKeyEx fails.
	if (cSubKeys) {
		for (i=0; i<cSubKeys; i++) { 
			cbName = MAX_KEY_LENGTH;
			retCode = RegEnumKeyEx(hKey, i, achKey, &cbName, NULL, NULL, NULL, &ftLastWriteTime); 
			if (retCode == ERROR_SUCCESS) {
				buffer = (char*) malloc( strlen( TEXT(lpSubKey) ) + strlen( achKey ) + 100 ) ;
				sprintf( buffer, "[HKEY_CURRENT_USER\\%s\\%s]", TEXT(lpSubKey), achKey ) ;
				fprintf( fp_out, "\r\n%s\r\n", buffer ) ;
				if( text!=NULL ) 
					if( strlen( text ) > 0 ) fprintf( fp_out, "%s\r\n", text ) ;
				free( buffer );				
				}
			}
		} 
	RegCloseKey( hKey ) ;
	}

void InitRegistryAllSessions( HKEY hMainKey, LPCTSTR lpSubKey, char * SubKeyName, char * filename, char * text ) {
	FILE * fp;
	char buf[1024] = "" ;
	if( (fp=fopen( filename, "wb" )) != NULL ) {
		fprintf( fp, "Windows Registry Editor Version 5.00\r\n" ) ;
		snprintf( buf, sizeof(buf), "%s\\%s", lpSubKey, SubKeyName );
		QuerySubKey( hMainKey, (LPCTSTR)buf, fp, text ) ;
		fclose( fp ) ;
		}
	}
	
void InitAllSessions( HKEY hMainKey, LPCTSTR lpSubKey, char * SubKeyName, char * filename ) {
	char text[4096], f[1024] ;
	FILE * fp ;
	int len ;
	if( (fp=fopen(filename, "rb")) != NULL ) {
		len = fread( text, 1, 4096, fp ) ;
		fclose( fp ) ;
		text[4095]='\0'; text[len] = '\0' ;
		str_rtrim( text, "\n\r" ) ;
		snprintf( f, sizeof(f), "%s.reg", filename ) ;
		InitRegistryAllSessions( hMainKey, lpSubKey, SubKeyName, f, text ) ;
		unlink(filename);
		}
	}
	
// Delete a registry key value. TRUE when it is gone - deleted, or never
// there; FALSE with the reason in GetLastError() when it is still there.
// It returned TRUE whatever happened.
BOOL RegDelValue (HKEY hKeyRoot, LPCTSTR lpSubKey, LPCTSTR lpValue ) {
	HKEY hKey;
	LONG lResult;
	if( kitty_store_ro_regpath( hKeyRoot == HKEY_CURRENT_USER, lpSubKey ) ) return KITTY_RO_REFUSED ;
	if( (lResult = RegOpenKeyEx (hKeyRoot, lpSubKey, 0, KEY_WRITE, &hKey)) == ERROR_SUCCESS ) {
		lResult = RegDeleteValue( hKey, lpValue ) ;
		RegCloseKey(hKey) ;
		}
	if( lResult == ERROR_SUCCESS || lResult == ERROR_FILE_NOT_FOUND ) return TRUE ;
	SetLastError( (DWORD)lResult ) ;
	return FALSE ;
	}

// Delete a registry key and its subkeys
BOOL RegDelTree (HKEY hKeyRoot, LPCTSTR lpSubKey) {
    TCHAR lpEnd[MAX_PATH];
    LONG lResult;
    DWORD dwSize;
    TCHAR szName[MAX_PATH];
    HKEY hKey;
    FILETIME ftWrite;

    if( kitty_store_ro_regpath( hKeyRoot == HKEY_CURRENT_USER, lpSubKey ) ) return KITTY_RO_REFUSED ;

    // First, see if we can delete the key without having
    // to recurse.
    lResult = RegDeleteKey(hKeyRoot, lpSubKey);
    if (lResult == ERROR_SUCCESS) return TRUE;

    lResult = RegOpenKeyEx (hKeyRoot, lpSubKey, 0, KEY_READ, &hKey) ;

    if (lResult != ERROR_SUCCESS) {
        if (lResult == ERROR_FILE_NOT_FOUND) { 
		//printf("Key not found.\n") ;
		return TRUE ; 
	} else {
		//printf("Error opening key.\n") ;
		SetLastError( (DWORD)lResult ) ;   /* the caller reports the reason */
		return FALSE ;
	}
    }

    // Enumerate the keys
    dwSize = MAX_PATH;
    lResult = RegEnumKeyEx(hKey, 0, szName, &dwSize, NULL, NULL, NULL, &ftWrite) ;

    if (lResult == ERROR_SUCCESS) 
    {
        do {
            //StringCchCopy (lpEnd, MAX_PATH*2, szName);
            snprintf(lpEnd, sizeof(lpEnd), "%s\\%s", lpSubKey, szName);

            //if( !RegDelTree( hKeyRoot, lpSubKey ) ) { break ; }
            if( !RegDelTree( hKeyRoot, lpEnd ) ) { break ; }
            dwSize = MAX_PATH;
            lResult = RegEnumKeyEx(hKey, 0, szName, &dwSize, NULL, NULL, NULL, &ftWrite) ;
        } while ( lResult == ERROR_SUCCESS ) ;
    }

	RegCloseKey(hKey) ;

	// Try again to delete the key.
	lResult = RegDeleteKey(hKeyRoot, lpSubKey);

	if (lResult == ERROR_SUCCESS) return TRUE;
	SetLastError( (DWORD)lResult ) ;   /* the caller reports the reason */
	return FALSE;
	}

/* KiTTY 0.84: one-time migration of the registry hive from the old KiTTY namespace
 * (Software\9bis.com\KiTTY) to the current one (Software\kapper.net\KiTTY). Idempotent
 * -- copies only when the destination is absent and the source exists; non-destructive
 * (the old hive is left intact and also serves as a read-only fallback in storage.c).
 * Call it from every kitty.exe entry path (terminal init AND -launcher) so the launcher
 * sees migrated sessions even on a boot where it runs before the main terminal.
 *
 * Hive paths are spelled out as string literals on purpose (not PUTTY_REG_POS): this is
 * a one-off migration between two FIXED endpoints -- the legacy 9bis hive and the current
 * kapper.net hive -- so we specifically do NOT want the runtime-flippable base; and the
 * 9bis side has no global macro anyway. Literals also keep this helper's include surface
 * minimal (kitty_registry.h only, not platform.h/putty.h). */
void MigrateOldKittyHive( void ) {
	if( !RegTestKey( HKEY_CURRENT_USER, "Software\\kapper.net\\KiTTY" )
	    && RegTestKey( HKEY_CURRENT_USER, "Software\\9bis.com\\KiTTY" ) ) {
		/* a partial copy must not stay: the key's presence is what marks the
		 * migration done, so it would never be tried again */
		if( !kitty_RegCopyTree( HKEY_CURRENT_USER, "Software\\9bis.com\\KiTTY", "Software\\kapper.net\\KiTTY" ) )
			RegDelTree( HKEY_CURRENT_USER, "Software\\kapper.net\\KiTTY" ) ;
	}
}

/* The one-shot repairs below address the kapper.net\\KiTTY hive by literal
 * path (see the note above RepairSharrowDefaults). They run only when that is
 * the hive in use, else a run under another hive would create kapper.net\\KiTTY
 * just to leave a marker in it. */
extern const char *kitty_registry_base( void ) ;
static int kitty_repairs_own_hive( void ) {
	return strcmp( kitty_registry_base(), "Software\\kapper.net\\KiTTY" ) == 0 ;
}

/* KiTTY 0.84: one-time repair of the ShiftedArrowKeys default regression.
 * The 0.84 port inherited PuTTY's SHARROW_APPLICATION(0) compiled default instead
 * of KiTTY's historical SHARROW_BITMAP(1). A session migrated from the old 9bis
 * hive that relied on that old default, once loaded and *re-saved* under the buggy
 * build, had ShiftedArrowKeys=0 persisted into the kapper.net hive -- silently
 * losing Ctrl+Left/Right word navigation. conf.h now restores BITMAP as the
 * default, which auto-heals every session that has NO explicit value; this repair
 * mops up the ones that persisted an explicit 0.
 *
 * We reset ONLY where we can prove the 0 was written by us and not chosen by the
 * user: the corresponding session must still exist in the untouched old 9bis hive
 * AND have no explicit ShiftedArrowKeys there. Deleting the kapper.net value then
 * lets the session fall back to the new BITMAP default. Any deliberate choice
 * (an explicit value already in 9bis, or a session absent from 9bis so we cannot
 * know) is left untouched. Idempotent via a marker value -- runs at most once.
 *
 * Registry paths are string literals, not PUTTY_REG_POS -- same rationale as
 * MigrateOldKittyHive above: fixed 9bis<->kapper.net migration endpoints (not the
 * runtime-flippable base), and this file intentionally includes only kitty_registry.h. */
void RepairSharrowDefaults( void ) {
	HKEY hSess ;
	char cur[cstMaxRegLength+2], old[cstMaxRegLength+2] ;
	char name[MAX_KEY_LENGTH+1], kpath[512], oldsess[512] ;
	DWORD idx, len ;
	int failed = 0 ;

	/* Only when kapper.net\\KiTTY is the hive this run uses (KiClassName can
	 * point the store at another hive): the repair reads and marks THAT hive
	 * by literal path, and must not create it as a side effect under any other. */
	if( !kitty_repairs_own_hive() ) return ;

	/* one-time guard: skip if we've already run */
	if( GetValueDataN( HKEY_CURRENT_USER, "Software\\kapper.net\\KiTTY", KR_SHARROWREPAIRDONE, cur, sizeof(cur) ) != NULL )
		return ;

	if( RegOpenKeyEx( HKEY_CURRENT_USER, "Software\\kapper.net\\KiTTY\\Sessions", 0, KEY_READ, &hSess ) == ERROR_SUCCESS ) {
		for( idx = 0 ; ; idx++ ) {
			len = sizeof(name) ;
			if( RegEnumKeyEx( hSess, idx, name, &len, NULL, NULL, NULL, NULL ) != ERROR_SUCCESS ) break ;
			snprintf( kpath, sizeof(kpath), "Software\\kapper.net\\KiTTY\\Sessions\\%s", name ) ;
			/* only sessions with an explicit APPLICATION(0) are candidates */
			if( GetValueDataN( HKEY_CURRENT_USER, kpath, "ShiftedArrowKeys", cur, sizeof(cur) ) == NULL ) continue ;
			if( strcmp( cur, "0" ) != 0 ) continue ;
			snprintf( oldsess, sizeof(oldsess), "Software\\9bis.com\\KiTTY\\Sessions\\%s", name ) ;
			/* repair only if the 9bis original exists and had NO explicit value:
			 * that proves the 0 is our persisted default, not the user's choice. */
			if( RegTestKey( HKEY_CURRENT_USER, oldsess )
			    && GetValueDataN( HKEY_CURRENT_USER, oldsess, "ShiftedArrowKeys", old, sizeof(old) ) == NULL ) {
				if( !RegDelValue( HKEY_CURRENT_USER, kpath, "ShiftedArrowKeys" ) ) failed = 1 ;
			}
		}
		RegCloseKey( hSess ) ;
	}

	/* set the marker, so we don't rescan every boot (even if the old 9bis hive
	 * is gone and nothing could be repaired) - but not after a repair that
	 * failed: the next start tries it again. RegTestOrCreateDWORD hides the
	 * RegSetValueEx byte-buffer boilerplate and creates the base key if needed. */
	if( !failed ) RegTestOrCreateDWORD( HKEY_CURRENT_USER, "Software\\kapper.net\\KiTTY", KR_SHARROWREPAIRDONE, 1 ) ;
}

/* One-time migration: the per-session "Send file in current
 * directory" option (SCPAutoPwd) was retired -- it drove uploads off the removed
 * __pw title-scan (CVE-2024-23749) and is superseded by opt-in OSC 7 cwd
 * tracking. For every registry session that had SCPAutoPwd=1, enable
 * OSC7CwdTracking (the safe way to get the "upload into the current remote dir"
 * behaviour the user had asked for); then delete the retired SCPAutoPwd value
 * from every session so it does not linger. Idempotent via the ScpAutoPwdMigrated
 * marker -- runs at most once. Registry mode only (like RepairSharrowDefaults);
 * portable sessions simply stop persisting the key once it is no longer saved. */
void MigrateScpAutoPwd( void ) {
	HKEY hSess ;
	int failed = 0 ;
	char cur[cstMaxRegLength+2] ;
	char name[MAX_KEY_LENGTH+1], kpath[512] ;
	DWORD idx, len ;

	/* Only when kapper.net\\KiTTY is the hive this run uses (KiClassName can
	 * point the store at another hive): the repair reads and marks THAT hive
	 * by literal path, and must not create it as a side effect under any other. */
	if( !kitty_repairs_own_hive() ) return ;

	/* one-time guard: skip if we've already run */
	if( GetValueDataN( HKEY_CURRENT_USER, "Software\\kapper.net\\KiTTY", KR_SCPAUTOPWDMIGRATED, cur, sizeof(cur) ) != NULL )
		return ;

	if( RegOpenKeyEx( HKEY_CURRENT_USER, "Software\\kapper.net\\KiTTY\\Sessions", 0, KEY_READ, &hSess ) == ERROR_SUCCESS ) {
		for( idx = 0 ; ; idx++ ) {
			len = sizeof(name) ;
			if( RegEnumKeyEx( hSess, idx, name, &len, NULL, NULL, NULL, NULL ) != ERROR_SUCCESS ) break ;
			snprintf( kpath, sizeof(kpath), "Software\\kapper.net\\KiTTY\\Sessions\\%s", name ) ;
			if( GetValueDataN( HKEY_CURRENT_USER, kpath, "SCPAutoPwd", cur, sizeof(cur) ) == NULL ) continue ;
			/* the old option was on -> turn on its safe replacement */
			if( strcmp( cur, "1" ) == 0
			    && !RegTestOrCreateDWORD( HKEY_CURRENT_USER, kpath, "OSC7CwdTracking", 1 ) ) {
				/* the replacement is not stored: keep the old value, the
				 * next start tries again - deleting it lost the setting */
				failed = 1 ;
				continue ;
			}
			/* drop the retired key regardless of its value */
			if( !RegDelValue( HKEY_CURRENT_USER, kpath, "SCPAutoPwd" ) ) failed = 1 ;
		}
		RegCloseKey( hSess ) ;
	}

	/* set the marker, so we don't rescan every boot - unless a session was
	 * left unmigrated, which the next start tries again */
	if( !failed )
		RegTestOrCreateDWORD( HKEY_CURRENT_USER, "Software\\kapper.net\\KiTTY", KR_SCPAUTOPWDMIGRATED, 1 ) ;
}

/* Copy one registry key onto another, every value at its full size and every
 * subkey. 1 the whole tree was copied; 0 not (the source key missing, or a
 * read or write failed), with Windows' reason in GetLastError() - a caller
 * that deletes the source afterwards (RegRenameTree) must stop on 0.
 *
 * Values used to be read into a 1024-byte buffer and ANY longer one was
 * skipped without a word (a guard against overrunning that buffer): a long
 * PortForwardings or a multi-line comment did not reach the copy and the
 * rename that deleted the source then lost it. Each value is now read into
 * a buffer of its own size. */
int kitty_RegCopyTree( HKEY hMainKey, LPCTSTR lpSubKey, LPCTSTR lpDestKey ) {
	HKEY hKey, hDestKey ;
	TCHAR achKey[MAX_KEY_LENGTH], achValue[MAX_VALUE_NAME] ;
	DWORD cbName, cchValue, cSubKeys = 0, cValues = 0, cbMaxValueData = 0 ;
	DWORD i, type, size ;
	LONG rc ;
	unsigned char * data = NULL ;
	int ok = 1 ;

	/* a copy INTO the read-only store; out of it is a read */
	if( kitty_store_ro_regpath( hMainKey == HKEY_CURRENT_USER, lpDestKey ) ) return KITTY_RO_REFUSED ;
	if( ( rc = RegOpenKeyEx( hMainKey, TEXT(lpSubKey), 0, KEY_READ, &hKey ) ) != ERROR_SUCCESS ) {
		SetLastError( (DWORD)rc ) ; return 0 ; }
	if( ( rc = RegCreateKeyEx( hMainKey, TEXT(lpDestKey), 0, NULL, 0, KEY_WRITE, NULL, &hDestKey, NULL ) ) != ERROR_SUCCESS ) {
		RegCloseKey( hKey ) ; SetLastError( (DWORD)rc ) ; return 0 ; }
	if( ( rc = RegQueryInfoKey( hKey, NULL, NULL, NULL, &cSubKeys, NULL, NULL,
	                            &cValues, NULL, &cbMaxValueData, NULL, NULL ) ) != ERROR_SUCCESS ) {
		ok = 0 ; goto done ; }
	data = (unsigned char *) malloc( cbMaxValueData ? cbMaxValueData : 1 ) ;
	if( data == NULL ) { rc = ERROR_NOT_ENOUGH_MEMORY ; ok = 0 ; goto done ; }

	for( i = 0 ; i < cValues ; i++ ) {
		cchValue = MAX_VALUE_NAME ;
		size = cbMaxValueData ;
		rc = RegEnumValue( hKey, i, achValue, &cchValue, NULL, &type, data, &size ) ;
		if( rc == ERROR_MORE_DATA ) {
			/* grew since the query: read it again at its size */
			unsigned char * bigger = (unsigned char *) realloc( data, size ) ;
			if( bigger == NULL ) { rc = ERROR_NOT_ENOUGH_MEMORY ; ok = 0 ; goto done ; }
			data = bigger ; cbMaxValueData = size ;
			cchValue = MAX_VALUE_NAME ;
			rc = RegEnumValue( hKey, i, achValue, &cchValue, NULL, &type, data, &size ) ;
		}
		if( rc == ERROR_NO_MORE_ITEMS ) break ;
		if( rc != ERROR_SUCCESS ) { ok = 0 ; goto done ; }
		if( ( rc = RegSetValueEx( hDestKey, achValue, 0, type, data, size ) ) != ERROR_SUCCESS ) { ok = 0 ; goto done ; }
	}

	for( i = 0 ; i < cSubKeys ; i++ ) {
		char * src, * dst ;
		cbName = MAX_KEY_LENGTH ;
		rc = RegEnumKeyEx( hKey, i, achKey, &cbName, NULL, NULL, NULL, NULL ) ;
		if( rc == ERROR_NO_MORE_ITEMS ) break ;
		if( rc != ERROR_SUCCESS ) { ok = 0 ; goto done ; }
		src = dupprintf( "%s\\%s", TEXT(lpSubKey), achKey ) ;
		dst = dupprintf( "%s\\%s", TEXT(lpDestKey), achKey ) ;
		ok = kitty_RegCopyTree( hMainKey, src, dst ) ;
		rc = ok ? ERROR_SUCCESS : (LONG)GetLastError() ;
		sfree( src ) ; sfree( dst ) ;
		if( !ok ) goto done ;
	}
  done:
	free( data ) ;
	RegCloseKey( hDestKey ) ;
	RegCloseKey( hKey ) ;
	if( !ok ) SetLastError( rc != ERROR_SUCCESS ? (DWORD)rc : ERROR_WRITE_FAULT ) ;
	return ok ;
}

// Clean the PuTTY key: remove the keys and values specific to KiTTY
BOOL RegCleanPuTTY( void ) {
	HKEY hKey, hSubKey ;
	DWORD retCode, i;
	TCHAR    achKey[MAX_KEY_LENGTH];   // buffer for subkey name
	DWORD    cbName;                   // size of name string 
	TCHAR    achClass[MAX_PATH] = TEXT("");  // buffer for class name 
	DWORD    cchClassName = MAX_PATH;  // size of class string 
	DWORD    cSubKeys=0;               // number of subkeys 
	DWORD    cbMaxSubKey;              // longest subkey size 
	DWORD    cchMaxClass;              // longest class string 
	DWORD    cValues;              // number of values for key 
	DWORD    cchMaxValue;          // longest value name 
	DWORD    cbMaxValueData;       // longest value data 
	DWORD    cbSecurityDescriptor; // size of security descriptor 
	FILETIME ftLastWriteTime;      // last write time 
	char *buffer = NULL ;
	/* PuTTY's hive is the store with KiClassName=PuTTY */
	if( kitty_store_ro_regpath( 1, "Software\\SimonTatham\\PuTTY" ) ) return KITTY_RO_REFUSED ;
	if( (retCode = RegOpenKeyEx ( HKEY_CURRENT_USER, "Software\\SimonTatham\\PuTTY", 0, KEY_WRITE, &hSubKey)) == ERROR_SUCCESS ) {
		RegDeleteValue( hSubKey, KI_BUILD ) ;
		RegDeleteValue( hSubKey, KI_FOLDERS ) ;
		RegDeleteValue( hSubKey, KI_KICOUNT ) ;
		RegDeleteValue( hSubKey, KR_KILASTSE ) ;
		RegDeleteValue( hSubKey, KR_KILASTUH ) ;
		RegDeleteValue( hSubKey, KR_KILASTUP ) ;
		RegDeleteValue( hSubKey, KR_KIPATH ) ;
		RegDeleteValue( hSubKey, KR_KISESS ) ;
		RegDeleteValue( hSubKey, KR_KIVERS ) ;
		RegDeleteValue( hSubKey, KR_CTHELPERPATH ) ;
		RegDeleteValue( hSubKey, KI_PSCPPATH ) ;
		RegDeleteValue( hSubKey, KI_WINSCPPATH ) ;
		RegDeleteValue( hSubKey, KI_KICLASSNAME ) ;
		RegCloseKey(hSubKey) ;
		}
	
	RegDelTree (HKEY_CURRENT_USER, "Software\\SimonTatham\\PuTTY\\Commands" ) ;
	RegDelTree (HKEY_CURRENT_USER, "Software\\SimonTatham\\PuTTY\\Folders" ) ;
	RegDelTree (HKEY_CURRENT_USER, "Software\\SimonTatham\\PuTTY\\Launcher" ) ;
	RegDelTree (HKEY_CURRENT_USER, "Software\\SimonTatham\\PuTTY\\TermEnvHint" ) ;   /* kitty_termenv_hint.c */
	
	// Open the key
	if( RegOpenKeyEx( HKEY_CURRENT_USER, "Software\\SimonTatham\\PuTTY\\Sessions", 0, KEY_READ|KEY_WRITE, &hKey) != ERROR_SUCCESS ) return 0;
	
	retCode = RegQueryInfoKey(
        hKey,                    // key handle 
        achClass,                // buffer for class name 
        &cchClassName,           // size of class string 
        NULL,                    // reserved 
        &cSubKeys,               // number of subkeys 
        &cbMaxSubKey,            // longest subkey size 
        &cchMaxClass,            // longest class string 
        &cValues,                // number of values for this key 
        &cchMaxValue,            // longest value name 
        &cbMaxValueData,         // longest value data 
        &cbSecurityDescriptor,   // security descriptor 
        &ftLastWriteTime);
	
	// Enumerate the subkeys, until RegEnumKeyEx fails.
	if (cSubKeys) {  //printf( "\nNumber of subkeys: %d\n", cSubKeys);
		for (i=0; i<cSubKeys; i++) { 
			cbName = MAX_KEY_LENGTH;
			if( ( retCode = RegEnumKeyEx(hKey, i, achKey, &cbName,NULL,NULL,NULL, &ftLastWriteTime) ) == ERROR_SUCCESS ) {
				buffer = (char*) malloc( strlen( achKey ) + 50 ) ;
				sprintf( buffer, "Software\\SimonTatham\\PuTTY\\Sessions\\%s\\Commands", achKey ) ;
				RegDelTree( HKEY_CURRENT_USER, buffer );
				sprintf( buffer, "Software\\SimonTatham\\PuTTY\\Sessions\\%s", achKey ) ;
				if( (retCode = RegOpenKeyEx ( HKEY_CURRENT_USER, buffer, 0, KEY_WRITE, &hSubKey)) == ERROR_SUCCESS ) {
					RegDeleteValue( hSubKey, "BCDelay" ) ;
					RegDeleteValue( hSubKey, "BgOpacity" ) ;
					RegDeleteValue( hSubKey, "BgSlideshow" ) ;
					RegDeleteValue( hSubKey, "BgType" ) ;
					RegDeleteValue( hSubKey, "BgImageFile" ) ;
					RegDeleteValue( hSubKey, "BgImageStyle" ) ;
					RegDeleteValue( hSubKey, "BgImageAbsoluteX" ) ;
					RegDeleteValue( hSubKey, "BgImageAbsoluteY" ) ;
					RegDeleteValue( hSubKey, "BgImagePlacement" ) ;
					RegDeleteValue( hSubKey, "Fullscreen" ) ;
					RegDeleteValue( hSubKey, "Maximize" ) ;
					RegDeleteValue( hSubKey, "SendToTray" ) ;
					RegDeleteValue( hSubKey, "SaveOnExit" ) ;
					RegDeleteValue( hSubKey, KR_FOLDER ) ;
					RegDeleteValue( hSubKey, "Icone" ) ;
					RegDeleteValue( hSubKey, "IconeFile" ) ;
					RegDeleteValue( hSubKey, "WinSCPProtocol" ) ;
					RegDeleteValue( hSubKey, "SFTPConnect" ) ;
    					RegDeleteValue( hSubKey, "PSCPOptions" ) ;
					RegDeleteValue( hSubKey, "PSCPShell" ) ;
					RegDeleteValue( hSubKey, "PSCPRemoteDir" ) ;
					RegDeleteValue( hSubKey, "WinSCPOptions" ) ;
					RegDeleteValue( hSubKey, "WinSCPRawSettings" ) ;
					RegDeleteValue( hSubKey, "InitDelay" ) ;
					RegDeleteValue( hSubKey, KR_PASSWORD ) ;
					RegDeleteValue( hSubKey, "Autocommand" ) ;
					RegDeleteValue( hSubKey, "AutocommandOut" ) ;
					RegDeleteValue( hSubKey, "AntiIdle" ) ;
					RegDeleteValue( hSubKey, "LogTimestamp" ) ;
					RegDeleteValue( hSubKey, KR_NOTES ) ;
					RegDeleteValue( hSubKey, "CygtermCommand" ) ;
					RegDeleteValue( hSubKey, "CygtermAltMetabit" ) ;
					RegDeleteValue( hSubKey, "CygtermAutoPath" ) ;
					RegDeleteValue( hSubKey, "Cygterm64" ) ;
					RegDeleteValue( hSubKey, "WakeupReconnect" ) ;
					RegDeleteValue( hSubKey, "FailureReconnect" ) ;
					RegDeleteValue( hSubKey, "ReconnectLogin" ) ;
					RegDeleteValue( hSubKey, "Scriptfile" ) ;
					RegDeleteValue( hSubKey, "ScriptfileContent" ) ;
					RegDeleteValue( hSubKey, "HostAlt" ) ;
					RegDeleteValue( hSubKey, "TransparencyValue" ) ;
					RegDeleteValue( hSubKey, "TermXPos" ) ;
					RegDeleteValue( hSubKey, "TermYPos" ) ;
					RegDeleteValue( hSubKey, "AuthPKCS11" ) ;
					RegDeleteValue( hSubKey, "PKCS11LibFile" ) ;
					RegDeleteValue( hSubKey, "PKCS11TokenLabel" ) ;
					RegDeleteValue( hSubKey, "PKCS11CertLabel" ) ;
					RegDeleteValue( hSubKey, "CopyURLDetection" ) ;
					RegDeleteValue( hSubKey, "HyperlinkUnderline" ) ;
					RegDeleteValue( hSubKey, "HyperlinkUseCtrlClick" ) ;
					RegDeleteValue( hSubKey, "HyperlinkBrowserUseDefault" ) ;
					RegDeleteValue( hSubKey, "HyperlinkBrowser" ) ;
					RegDeleteValue( hSubKey, "HyperlinkRegularExpressionUseDefault" ) ;
					RegDeleteValue( hSubKey, "HyperlinkRegularExpression" ) ;
					RegDeleteValue( hSubKey, "HyperlinkScan" ) ;
					RegDeleteValue( hSubKey, "HyperlinkOSC8" ) ;
					RegDeleteValue( hSubKey, "HyperlinkPreview" ) ;
					RegDeleteValue( hSubKey, "HyperlinkConfirm" ) ;
					RegDeleteValue( hSubKey, "rzCommand" ) ;
					RegDeleteValue( hSubKey, "rzOptions" ) ;
					RegDeleteValue( hSubKey, "szCommand" ) ;
					RegDeleteValue( hSubKey, "szOptions" ) ;
					RegDeleteValue( hSubKey, "zDownloadDir" ) ;
					RegDeleteValue( hSubKey, "SaveWindowPos" ) ;
					RegDeleteValue( hSubKey, "WindowState" ) ;
					RegDeleteValue( hSubKey, "ForegroundOnBell" ) ;
					RegDeleteValue( hSubKey, "CtrlTabSwitch" ) ;
					RegDeleteValue( hSubKey, KR_COMMENT ) ;
					RegDeleteValue( hSubKey, "WindowName" ) ;
					RegDeleteValue( hSubKey, "LogTimeRotation" ) ;
					RegDeleteValue( hSubKey, "LogKeepDays" ) ;
					RegDeleteValue( hSubKey, "LogKeepMB" ) ;
					RegDeleteValue( hSubKey, "PortKnocking" ) ;
					RegDeleteValue( hSubKey, "WindowClosable" ) ;
					RegDeleteValue( hSubKey, "WindowMinimizable" ) ;
					RegDeleteValue( hSubKey, "WindowMaximizable" ) ;
					RegDeleteValue( hSubKey, "WindowHasSysMenu" ) ;
					RegDeleteValue( hSubKey, "DisableBottomButtons" ) ;
					RegDeleteValue( hSubKey, "BoldAsColour" ) ;
					RegDeleteValue( hSubKey, "UnderlinedAsColour" ) ;
					RegDeleteValue( hSubKey, "SelectedAsColour" ) ;
					RegDeleteValue( hSubKey, "ScriptFileName" ) ;
					RegDeleteValue( hSubKey, "ScriptMode" ) ;
					RegDeleteValue( hSubKey, "ScriptLineDelay" ) ;
					RegDeleteValue( hSubKey, "ScriptCharDelay" ) ;
					RegDeleteValue( hSubKey, "ScriptCondLine" ) ;
					RegDeleteValue( hSubKey, "ScriptCondUse" ) ;
					RegDeleteValue( hSubKey, "ScriptCRLF" ) ;
					RegDeleteValue( hSubKey, "ScriptEnable" ) ;
					RegDeleteValue( hSubKey, "ScriptExcept" ) ;
					RegDeleteValue( hSubKey, "ScriptTimeout" ) ;
					RegDeleteValue( hSubKey, "ScriptWait" ) ;
					RegDeleteValue( hSubKey, "ScriptHalt" ) ;
					RegDeleteValue( hSubKey, "SSHTunnelInTitle" ) ;
					RegDeleteValue( hSubKey, "SCPAutoPwd" ) ;
					RegDeleteValue( hSubKey, "NoFocusReporting" ) ;
					RegDeleteValue( hSubKey, "SyncOutputHoldMs" ) ;
					RegDeleteValue( hSubKey, "HostNotify" ) ;
					RegDeleteValue( hSubKey, "TaskbarProgress" ) ;
					RegDeleteValue( hSubKey, "FailedCommandMarks" ) ;
					RegDeleteValue( hSubKey, "ContextFailNotice" ) ;
					RegDeleteValue( hSubKey, "ContextRootFrame" ) ;
					RegDeleteValue( hSubKey, "ContextRootTint" ) ;
					RegDeleteValue( hSubKey, "ContextBreadcrumb" ) ;
					RegDeleteValue( hSubKey, "KittyGraphics" ) ;
					RegDeleteValue( hSubKey, "ItermImages" ) ;
					RegDeleteValue( hSubKey, "SixelImages" ) ;
					RegDeleteValue( hSubKey, "ReportVT220" ) ;
					RegDeleteValue( hSubKey, "KscpDropTypePath" ) ;
					RegDeleteValue( hSubKey, "LinesAtAScroll" ) ;
					RegDeleteValue( hSubKey, "DisableAltGr" ) ;
					RegDeleteValue( hSubKey, "ProxySelection" ) ;
					//RegDeleteValue( hSubKey, "" ) ;
 					RegCloseKey(hSubKey) ;
					}
				free( buffer );
				}
			}
		} 
 
	RegCloseKey( hKey ) ;
	
	return 1;
	}

/* KiTTY: report back from a command-line switch. kitty.exe is a
 * GUI-subsystem program and so starts with no console of its own; attaching to
 * the console of the shell that launched it puts the text where the user is
 * actually looking. A message box is the fallback for when there is no console
 * at all - started from Explorer, a shortcut or the installer - and not the
 * normal path: a switch typed at a prompt should answer at that prompt. */
/* KiTTY: set while the configuration box drives a registration - the report
 * and the question then always come as boxes, never on whatever console the
 * terminal happens to have been started from. */
static int g_report_gui = 0 ;

/* The console half on its own: 1 when the text went to the console of the
 * shell that started us, 0 when there is none. For a caller with a better
 * fallback than a message box - the option list of -h is too long for one. */
int KittyCliPrint( const char *text ) {
	HANDLE h ;
	DWORD written ;
	int attached = kitty_attach_parent_console() ? 1 : 0 ;

	h = CreateFileA( "CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE|FILE_SHARE_READ,
			 NULL, OPEN_EXISTING, 0, NULL ) ;
	if( h == INVALID_HANDLE_VALUE ) {
		if( attached ) FreeConsole() ;
		return 0 ;
	}
	/* the line breaks: see KittyCliReport below */
	WriteFile( h, "\r\n", 2, &written, NULL ) ;
	WriteFile( h, text, (DWORD)strlen(text), &written, NULL ) ;
	WriteFile( h, "\r\n", 2, &written, NULL ) ;
	CloseHandle( h ) ;
	if( attached ) FreeConsole() ;
	return 1 ;
}

/* A standard handle the caller redirected to a file or a pipe (a script
 * capturing our output): the text written there, 1. 0 when there is none -
 * a GUI program started normally has no standard handles at all. */
static int cli_std_write( DWORD which, const char *text ) {
	HANDLE h = GetStdHandle( which ) ;
	DWORD type, written ;
	if( h == NULL || h == INVALID_HANDLE_VALUE ) return 0 ;
	type = GetFileType( h ) ;
	if( type != FILE_TYPE_DISK && type != FILE_TYPE_PIPE ) return 0 ;
	WriteFile( h, text, (DWORD)strlen( text ), &written, NULL ) ;
	WriteFile( h, "\r\n", 2, &written, NULL ) ;
	return 1 ;
}
static int reach_desktop_visible( void ) ;   /* below */

/* The result of a command-line run. A script reads it, and its exit code
 * carries the verdict, so a box nobody may click must never hold it up:
 * redirected output first (stderr for a failure, stdout otherwise), then
 * the console of the shell that started us, then a box - only where a
 * desktop can be seen. A service or a task run whether the user is logged
 * on or not gets the exit code alone. */
void KittyCliReport( const char *title, const char *text, int warn ) {
	if( g_report_gui ) {
		MessageBoxA( NULL, text, title,
			     MB_OK | (warn ? MB_ICONWARNING : MB_ICONINFORMATION) ) ;
		return ;
	}
	if( KittyCliOutput( text, warn ) ) return ;
	if( !reach_desktop_visible() ) return ;
	/* in front: no click of the user's raised it, and a box behind other
	 * windows holds the run up unseen */
	MessageBoxA( NULL, text, title, MB_OK | MB_SETFOREGROUND |
		     (warn ? MB_ICONWARNING : MB_ICONINFORMATION) ) ;
}

/* KittyCliReport without the box: redirected output, then the console of the
 * shell that started us. 1 = written there, 0 = neither is there. */
int KittyCliOutput( const char *text, int warn ) {
	if( cli_std_write( warn ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE, text ) ||
	    cli_std_write( warn ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE, text ) )
		return 1 ;
	HANDLE h ;
	DWORD written ;
	int attached = kitty_attach_parent_console() ? 1 : 0 ;

	h = CreateFileA( "CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE|FILE_SHARE_READ,
			 NULL, OPEN_EXISTING, 0, NULL ) ;
	if( h != INVALID_HANDLE_VALUE ) {
		/* Start on a line of our own. A GUI-subsystem program returns to the
		 * shell the moment it starts, so the prompt has already been drawn by
		 * the time this text arrives - without the leading break it lands on
		 * top of it. Same reason for the trailing one: leave the cursor at the
		 * start of a clean line for the prompt that follows. */
		WriteFile( h, "\r\n", 2, &written, NULL ) ;
		WriteFile( h, text, (DWORD)strlen(text), &written, NULL ) ;
		WriteFile( h, "\r\n", 2, &written, NULL ) ;
		CloseHandle( h ) ;
		if( attached ) FreeConsole() ;
		return 1 ;
	}
	if( attached ) FreeConsole() ;
	return 0 ;
}

/* KiTTY: put up a yes/no question the same way KittyCliReport() answers one -
 * at the prompt when there is one, in a box when there is not. Returns 1 only
 * for an explicit yes; anything else, including a closed console or an
 * unreadable input, is a no, because the caller is about to write to the
 * registry. */
static int CliConfirm( const char *title, const char *text ) {
	if( g_report_gui )
		return MessageBoxA( NULL, text, title,
				    MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2 ) == IDYES ;
	HANDLE hout, hin ;
	DWORD written, nread = 0 ;
	char answer[16] ;
	int attached = kitty_attach_parent_console() ? 1 : 0 ;
	int yes = 0 ;

	hout = CreateFileA( "CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE|FILE_SHARE_READ,
			    NULL, OPEN_EXISTING, 0, NULL ) ;
	hin  = CreateFileA( "CONIN$", GENERIC_READ|GENERIC_WRITE, FILE_SHARE_READ|FILE_SHARE_WRITE,
			    NULL, OPEN_EXISTING, 0, NULL ) ;
	if( hout != INVALID_HANDLE_VALUE && hin != INVALID_HANDLE_VALUE ) {
		WriteFile( hout, "\r\n", 2, &written, NULL ) ;   /* clear of the prompt */
		WriteFile( hout, text, (DWORD)strlen(text), &written, NULL ) ;
		WriteFile( hout, KT_REG_CLI_CONFIRM_PROMPT, 38, &written, NULL ) ;
		if( ReadFile( hin, answer, sizeof(answer)-1, &nread, NULL ) && nread > 0 ) {
			answer[nread] = '\0' ;
			yes = ( answer[0] == 'y' || answer[0] == 'Y' ) ;
		}
		WriteFile( hout, "\r\n", 2, &written, NULL ) ;
	} else if( !reach_desktop_visible() ||
		   GetFileType( GetStdHandle( STD_ERROR_HANDLE ) ) == FILE_TYPE_DISK ||
		   GetFileType( GetStdHandle( STD_ERROR_HANDLE ) ) == FILE_TYPE_PIPE ) {
		/* nobody to prompt, or a script that captured our output: the safe
		 * answer, and how to give the other one */
		char *msg = dupprintf( "%s\r\n%s", text, KT_REACH_GIVE_YES ) ;
		cli_std_write( STD_ERROR_HANDLE, msg ) ;
		sfree( msg ) ;
	} else {
		yes = ( MessageBoxA( NULL, text, title,
				     MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2 ) == IDYES ) ;
	}
	if( hout != INVALID_HANDLE_VALUE ) CloseHandle( hout ) ;
	if( hin != INVALID_HANDLE_VALUE ) CloseHandle( hin ) ;
	if( attached ) FreeConsole() ;
	return yes ;
}

/* ---- where a report or a question goes ------------------------------- */

/* A terminal window on this thread: a session runs here. A modal box
 * would stop it - PuTTY handles the network in run_toplevel_callbacks, which
 * a modal box's own message loop never calls. The terminal's window class is
 * appname. */
static BOOL CALLBACK reach_find_terminal( HWND h, LPARAM lp ) {
	char cls[128] ;
	if( GetClassNameA( h, cls, sizeof(cls) ) && !strcmp( cls, appname ) ) {
		*(int *)lp = 1 ;
		return FALSE ;
	}
	return TRUE ;
}

/* Can anybody see a window from this process? Not in a service, nor in a
 * scheduled task that runs whether a user is logged on or not: their window
 * station is invisible. When that is unknown, someone is assumed there. */
static int reach_desktop_visible( void ) {
	USEROBJECTFLAGS uof ;
	DWORD need = 0 ;
	HWINSTA ws = GetProcessWindowStation() ;
	if( ws == NULL || !GetUserObjectInformationA( ws, UOI_FLAGS, &uof, sizeof(uof), &need ) )
		return 1 ;
	return ( uof.dwFlags & WSF_VISIBLE ) != 0 ;
}

/* The console of the shell that started this run, if there is one. */
static int reach_shell( void ) {
	int attached = kitty_attach_parent_console() ? 1 : 0 ;
	HANDLE h = CreateFileA( "CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE|FILE_SHARE_READ,
				NULL, OPEN_EXISTING, 0, NULL ) ;
	int ok = ( h != INVALID_HANDLE_VALUE ) ;
	if( ok ) CloseHandle( h ) ;
	if( attached ) FreeConsole() ;
	return ok ;
}

int kitty_reach( void ) {
	int term = 0 ;
	EnumThreadWindows( GetCurrentThreadId(), reach_find_terminal, (LPARAM)&term ) ;
	if( term ) return KITTY_REACH_SESSION ;
	/* the shell only for a do-and-exit run: a terminal started from a prompt
	 * still reports in windows, as does the configuration box driving a
	 * registration */
	if( !g_report_gui && kitty_cli_do_and_exit() && reach_shell() ) return KITTY_REACH_SHELL ;
	if( !reach_desktop_visible() ) return KITTY_REACH_NOBODY ;
	return KITTY_REACH_DESKTOP ;
}

/* stderr, when the caller redirected it to a file or a pipe */
static void reach_stderr( const char *text ) {
	HANDLE h = GetStdHandle( STD_ERROR_HANDLE ) ;
	DWORD written ;
	if( h == NULL || h == INVALID_HANDLE_VALUE ) return ;
	WriteFile( h, text, (DWORD)strlen( text ), &written, NULL ) ;
	WriteFile( h, "\r\n", 2, &written, NULL ) ;
}

int kitty_reach_confirm( HWND owner, const char *title, const char *question,
                         const char *notdone ) {
	if( kitty_cli_answer_yes() ) return 1 ;
	/* a caller without a window of its own (kitty_sessorg_folder_move): the
	 * one in front on this thread, so the question is not left behind it */
	if( owner == NULL ) owner = GetActiveWindow() ;
	switch( kitty_reach() ) {
	  case KITTY_REACH_DESKTOP:
		return MessageBox( owner, question, title,
				   MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 ) == IDYES ;
	  case KITTY_REACH_SESSION:
		/* a modal question would stop the session; a modeless one cannot
		 * hold the step back: not done and reported */
		if( notdone ) kitty_info_modeless( owner, title, notdone, NULL, NULL ) ;
		return 0 ;
	  case KITTY_REACH_SHELL:
		/* no question at the prompt yet (an interactive prompt does not wait
		 * for this GUI program and can take the answer): the safe answer */
		if( notdone ) {
			char *msg = dupprintf( "%s\r\n%s", notdone, KT_REACH_GIVE_YES ) ;
			KittyCliPrint( msg ) ;
			sfree( msg ) ;
		}
		return 0 ;
	  default:   /* KITTY_REACH_NOBODY: a box nobody answers hangs the caller */
		if( notdone ) {
			char *msg = dupprintf( "%s\r\n%s", notdone, KT_REACH_GIVE_YES ) ;
			reach_stderr( msg ) ;
			sfree( msg ) ;
		}
		return 0 ;
	}
}

/* KiTTY: is this exe the machine-wide installation? The system MSI installs
 * into %ProgramFiles%\KiTTY (kitty-system.wxs uses ProgramFiles64Folder); the
 * per-user MSI and every portable copy live elsewhere. Used to decide whether
 * registering URL handlers should request administrator rights: a program
 * installed for everyone should register for everyone. */
static int RunningFromProgramFiles( const char *path ) {
	static const char *vars[] = { "ProgramW6432", "ProgramFiles", "ProgramFiles(x86)" } ;
	char pf[MAX_PATH] ;
	int i ;
	for( i = 0 ; i < (int)(sizeof(vars)/sizeof(vars[0])) ; i++ ) {
		DWORD n = GetEnvironmentVariableA( vars[i], pf, sizeof(pf) ) ;
		if( n == 0 || n >= sizeof(pf) ) continue ;
		if( !strnicmp( path, pf, strlen(pf) ) ) return 1 ;
	}
	return 0 ;
}

/* KiTTY: the command the shell currently runs for <proto>://, read from the
 * merged HKEY_CLASSES_ROOT view - that is what actually handles a link today,
 * wherever it was registered. Returns 1 if there is one. */
static int UrlHandlerCurrentCommand( const char *proto, char *out, DWORD outlen ) {
	char key[256] ;
	DWORD len = outlen ;
	snprintf( key, sizeof(key), "%s\\shell\\open\\command", proto ) ;
	out[0] = '\0' ;
	if( RegGetValueA( HKEY_CLASSES_ROOT, key, NULL, RRF_RT_REG_SZ, NULL,
			  out, &len ) != ERROR_SUCCESS ) return 0 ;
	return out[0] != '\0' ;
}

/* KiTTY: the program behind a registered command line, for a report a person
 * can act on - "rundll32.exe" shows more at a glance than the full command with
 * its DLL entry point. Writes the bare file name into `out`. */
static void UrlHandlerProgram( const char *command, char *out, size_t outlen ) {
	const char *p = command, *end, *slash ;
	size_t n ;
	out[0] = '\0' ;
	while( *p == ' ' ) p++ ;
	if( *p == '"' ) { p++ ; end = strchr( p, '"' ) ; }
	else            { end = strchr( p, ' ' ) ; }
	if( !end ) end = p + strlen(p) ;
	for( slash = end ; slash > p ; slash-- )
		if( slash[-1] == '\\' || slash[-1] == '/' ) break ;
	n = (size_t)(end - slash) ;
	if( n == 0 || n >= outlen ) return ;
	memcpy( out, slash, n ) ;
	out[n] = '\0' ;
}

/* KiTTY: does this exact key exist in this exact hive? Not the same question
 * as "does HKEY_CLASSES_ROOT have one": HKCR is a merged view of HKLM and
 * HKCU, so a handler can be visible there while the hive we are about to write
 * holds nothing at all. That difference decides what "undo" means. */
static int ClassKeyExists( HKEY root, const char *subkey ) {
	HKEY hk ;
	if( RegOpenKeyExA( root, subkey, 0, KEY_READ, &hk ) != ERROR_SUCCESS )
		return 0 ;
	RegCloseKey( hk ) ;
	return 1 ;
}

/* KiTTY: save one key, in one hive, to a .reg file before we change it, so
 * "put it back the way it was" is a command the user can run rather than a
 * registry key to reconstruct by hand. Returns 1 and fills `file` on success.
 * Uses Windows' own reg.exe, which exports the whole subtree - values we never
 * touched included. The hive has to be given explicitly: an HKCR export is an
 * HKLM export, and importing one of those needs elevation the user may not
 * have (and would not restore anything, if what shadowed it was in HKCU). */
static int ClassKeyBackup( HKEY root, const char *subkey, const char *tag,
			   char *file, size_t filelen ) {
	char dir[MAX_PATH], sysdir[MAX_PATH], cmd[2048] ;
	const char *hive = ( root == HKEY_LOCAL_MACHINE ) ? "HKLM" : "HKCU" ;
	SYSTEMTIME st ;
	STARTUPINFOA si ;
	PROCESS_INFORMATION pi ;
	DWORD rc = 1, n ;

	/* `file` is empty on every failure: it was filled in before reg.exe ran,
	 * so a failed backup still offered "undo: import <file>" for a file that
	 * was never written (the callers' no-backup wording keys off it). */
	if( filelen ) file[0] = '\0' ;
	n = GetEnvironmentVariableA( "LOCALAPPDATA", dir, sizeof(dir) ) ;
	if( n == 0 || n >= sizeof(dir) ) return 0 ;
	strncat( dir, "\\KiTTY", sizeof(dir)-strlen(dir)-1 ) ;
	if( !CreateDirectoryA( dir, NULL ) && GetLastError() != ERROR_ALREADY_EXISTS ) return 0 ;

	GetLocalTime( &st ) ;
	snprintf( file, filelen, "%s\\%s-%s-%04d%02d%02d-%02d%02d%02d.reg",
		  dir, hive, tag, st.wYear, st.wMonth, st.wDay,
		  st.wHour, st.wMinute, st.wSecond ) ;

	if( GetSystemDirectoryA( sysdir, sizeof(sysdir) ) == 0 ) { file[0] = '\0' ; return 0 ; }
	snprintf( cmd, sizeof(cmd), "\"%s\\reg.exe\" export \"%s\\%s\" \"%s\" /y",
		  sysdir, hive, subkey, file ) ;
	memset( &si, 0, sizeof(si) ) ; si.cb = sizeof(si) ;
	si.dwFlags = STARTF_USESHOWWINDOW ; si.wShowWindow = SW_HIDE ;
	if( !CreateProcessA( NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW,
			     NULL, NULL, &si, &pi ) ) { file[0] = '\0' ; return 0 ; }
	WaitForSingleObject( pi.hProcess, 30000 ) ;
	if( !GetExitCodeProcess( pi.hProcess, &rc ) ) rc = 1 ;
	CloseHandle( pi.hThread ) ; CloseHandle( pi.hProcess ) ;
	if( rc != 0 || GetFileAttributesA( file ) == INVALID_FILE_ATTRIBUTES ) {
		DeleteFileA( file ) ;   /* a part-written export is no backup */
		file[0] = '\0' ;
		return 0 ;
	}
	return 1 ;
}

/* KiTTY: where should a class registration (URL handler, file association) be
 * written, and may we write it at all? Shared by -sshhandler and -fileassoc so
 * that the two behave the same.
 *
 * HKLM when this process can write there. Otherwise, if this is the
 * machine-wide installation, request elevation and let the elevated
 * copy do the whole job (`relaunch` + `relaunch_extra` is the command line it
 * gets); -user (peruser) means "my account is what I meant". A portable copy
 * requests a confirmation first, since what it writes outlives it.
 *
 * Returns 1 with *root set when the caller should proceed, 0 when it should
 * return quietly - the user has been shown why. */
static int ClassRegTarget( const char *path, const char *relaunch,
			   const char *relaunch_extra,
			   int peruser, int assume_yes,
			   const char *what, const char *title, HKEY *root ) {
	HKEY classes ;

#ifdef MOD_PORTABLE
	/* A portable KiTTY is expected to leave the machine as it found it, and
	 * registering is the opposite of that: the entry outlives the USB stick
	 * and then points at a path that has gone. Still the user's call - so a
	 * plain question comes first, with the path in front of them.
	 * savemode=registry already means "this copy uses the registry"; -yes
	 * answers for scripts. */
	if( !assume_yes && GetIniFileFlag() != SAVEMODE_REG ) {
		char question[2048] ;
		snprintf( question, sizeof(question),
			KT_REG_PORTABLE_QUESTION, what, path ) ;
		if( !CliConfirm( title, question ) ) {
			KittyCliReport( title, KT_REG_NOTHING_REGISTERED, 0 ) ;
			return 0 ;
		}
	}
#else
	(void)assume_yes ; (void)what ;
#endif

	/* Opening the parent for KEY_CREATE_SUB_KEY answers "may we write to
	 * HKLM?" without creating anything. */
	if( RegOpenKeyExA( HKEY_LOCAL_MACHINE, "Software\\Classes", 0,
			   KEY_WRITE|KEY_CREATE_SUB_KEY, &classes ) == ERROR_SUCCESS ) {
		RegCloseKey( classes ) ;
		*root = HKEY_LOCAL_MACHINE ;
		return 1 ;
	}

	if( RunningFromProgramFiles( path ) && !peruser ) {
		SHELLEXECUTEINFOA sei ;
		char params[128] ;
		snprintf( params, sizeof(params), "%s%s", relaunch,
			  relaunch_extra ? relaunch_extra : "" ) ;
		memset( &sei, 0, sizeof(sei) ) ;
		sei.cbSize = sizeof(sei) ;
		sei.fMask = SEE_MASK_NOCLOSEPROCESS ;
		sei.lpVerb = "runas" ;
		sei.lpFile = path ;
		sei.lpParameters = params ;
		sei.nShow = SW_SHOWNORMAL ;
		if( ShellExecuteExA( &sei ) ) {
			if( sei.hProcess ) {
				WaitForSingleObject( sei.hProcess, INFINITE ) ;
				CloseHandle( sei.hProcess ) ;
			}
			return 0 ;   /* the elevated copy did the work and reported */
		}
		/* a cancelled UAC prompt is the user's answer, anything else is
		 * a failure with a reason: a missing file, a policy, a denial */
		{
			DWORD err = GetLastError() ;
			if( err == ERROR_CANCELLED )
				KittyCliReport( title, KT_REG_ELEVATION_REFUSED, 1 ) ;
			else {
				char *msg = dupprintf( KT_REG_ELEVATION_FAILED_FMT, kitty_win_reason( err ) ) ;
				KittyCliReport( title, msg, 1 ) ;
				sfree( msg ) ;
			}
		}
		return 0 ;
	}
	*root = HKEY_CURRENT_USER ;
	return 1 ;
}

/* KiTTY: write one URL protocol registration below `root`\`prefix`. Returns 1
 * only if the shell\open\command value - the one that matters - was written. */
static int UrlHandlerWrite( HKEY root, const char *prefix, const char *proto,
			    const char *friendly, const char *path,
			    const char *command ) {
	char key[512], buffer[1024] ;

	snprintf( key, sizeof(key), "%s%s", prefix, proto ) ;
	if( !RegTestOrCreate( root, key, "", friendly ) ) return 0 ;
	RegTestOrCreateDWORD( root, key, "EditFlags", 2 ) ;
	RegTestOrCreate( root, key, "FriendlyTypeName", "@ieframe.dll,-907" ) ;
	RegTestOrCreate( root, key, "URL Protocol", "" ) ;
	RegTestOrCreateDWORD( root, key, "BrowserFlags", 8 ) ;

	snprintf( buffer, sizeof(buffer), "%s,0", path ) ;
	snprintf( key, sizeof(key), "%s%s\\DefaultIcon", prefix, proto ) ;
	RegTestOrCreate( root, key, "", buffer ) ;
	snprintf( key, sizeof(key), "%s%s\\shell", prefix, proto ) ;
	RegTestOrCreate( root, key, "", "" ) ;

	snprintf( key, sizeof(key), "%s%s\\shell\\open\\command", prefix, proto ) ;
	return RegTestOrCreate( root, key, "", command ) ;
}

/* ---- Windows' Default apps ---------------------------------------------
 *
 * Windows' shell does not hand a link only to the HKCR\<scheme> handler: an
 * application listed in Software\RegisteredApplications whose Capabilities\
 * URLAssociations names the scheme is offered in Settings > Default apps,
 * and with no UserChoice set for the scheme the shell may send the link to it
 * although HKCR\<scheme> names this program. Registering therefore also lists
 * KiTTY++ there, with ProgIds of its own carrying the same open command as
 * the protocol keys, and then names every other application that claims one
 * of the same schemes. The choice itself (UserChoice) is hash-protected and
 * only the user can make it in Settings; it is never written here.
 *
 * The RegisteredApplications value and the Capabilities key are written in
 * the 64-bit registry view (KEY_WOW64_64KEY; ignored on 32-bit Windows), so
 * the 32-bit build writes the same keys as the 64-bit one. The Capabilities
 * key lives in a key of its own, Software\kapper.net\KiTTY++, outside the
 * settings store (Software\kapper.net\KiTTY, or wherever kitty_registry_base()
 * points): operations that delete, park or move the store must not take it
 * along and leave the RegisteredApplications value pointing at nothing. That
 * key holds nothing else, so removal deletes it whole. ProgIds go to
 * Software\Classes, which is shared between the views. */
#define DEFAPPS_NAME        "KiTTY++"   /* RegisteredApplications value name; the ms-settings: parameter */
#define DEFAPPS_KEYNAME     "KiTTY++"   /* below Software\kapper.net */
#define DEFAPPS_PARENT      "Software\\kapper.net\\" DEFAPPS_KEYNAME
#define DEFAPPS_CAPS        DEFAPPS_PARENT "\\Capabilities"
#define DEFAPPS_REGAPPS     "Software\\RegisteredApplications"
#define DEFAPPS_PROGID_FMT  "KiTTYpp.%s"   /* a ProgId takes no '+' */
#define DEFAPPS_NSCHEMES    4
/* the order of CreateSSHHandler's protos[] */
static const char *const defapps_schemes[DEFAPPS_NSCHEMES] = { "telnet", "ssh", "kitty", "putty" } ;

/* Append to a report buffer; a full buffer stays full (len >= size). */
static size_t rep_add( char *report, size_t len, size_t size, const char *fmt, ... ) {
	va_list ap ;
	int n ;
	if( len >= size ) return len ;
	va_start( ap, fmt ) ;
	n = vsnprintf( report+len, size-len, fmt, ap ) ;
	va_end( ap ) ;
	return n < 0 ? len : len + (size_t)n ;
}

/* One REG_SZ value in the 64-bit view, key created as needed. */
static int defapps_set( HKEY root, const char *subkey, const char *name, const char *data ) {
	HKEY hk ;
	LONG rc ;
	if( RegCreateKeyExA( root, subkey, 0, NULL, 0, KEY_SET_VALUE|KEY_WOW64_64KEY,
			     NULL, &hk, NULL ) != ERROR_SUCCESS ) return 0 ;
	rc = RegSetValueExA( hk, name, 0, REG_SZ, (const BYTE *)data, (DWORD)strlen(data)+1 ) ;
	RegCloseKey( hk ) ;
	return rc == ERROR_SUCCESS ;
}

/* RegisteredApplications + Capabilities in `root`, for the schemes whose
 * ProgId was written (progid_ok[i]). offered[i] = 1 for each scheme listed;
 * adds the "Default apps:" line when anything was. */
static size_t defapps_register( HKEY root, const char *path, const int *progid_ok,
				int *offered, char *report, size_t len, size_t size ) {
	char key[300], progid[64], icon[1100], list[128] ;
	int i ;
	list[0] = '\0' ;
	for( i = 0 ; i < DEFAPPS_NSCHEMES ; i++ ) offered[i] = 0 ;
	snprintf( icon, sizeof(icon), "%s,0", path ) ;
	if( !defapps_set( root, DEFAPPS_CAPS, "ApplicationName", DEFAPPS_NAME ) ||
	    !defapps_set( root, DEFAPPS_CAPS, "ApplicationDescription", KT_REG_DEFAPPS_DESCRIPTION ) ||
	    !defapps_set( root, DEFAPPS_CAPS, "ApplicationIcon", icon ) )
		return len ;
	snprintf( key, sizeof(key), "%s\\URLAssociations", DEFAPPS_CAPS ) ;
	for( i = 0 ; i < DEFAPPS_NSCHEMES ; i++ ) {
		if( !progid_ok[i] ) continue ;
		snprintf( progid, sizeof(progid), DEFAPPS_PROGID_FMT, defapps_schemes[i] ) ;
		if( !defapps_set( root, key, defapps_schemes[i], progid ) ) continue ;
		offered[i] = 1 ;
		snprintf( list+strlen(list), sizeof(list)-strlen(list), "%s%s://",
			  list[0] ? ", " : "", defapps_schemes[i] ) ;
	}
	if( !list[0] || !defapps_set( root, DEFAPPS_REGAPPS, DEFAPPS_NAME, DEFAPPS_CAPS ) ) {
		for( i = 0 ; i < DEFAPPS_NSCHEMES ; i++ ) offered[i] = 0 ;
		return len ;
	}
	return rep_add( report, len, size, KT_REG_DEFAPPS_OFFERED, list ) ;
}

/* Every other application in Default apps that claims one of the schemes
 * offered: RegisteredApplications of HKLM and HKCU, each in both registry
 * views - a 32-bit installer's Capabilities path lands under WOW6432Node,
 * and Windows reads it there. One report line per scheme and application;
 * *claimed counts them. */
static size_t defapps_competitors( const int *offered, char *report, size_t len,
				   size_t size, int *claimed ) {
	static const HKEY hives[2] = { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER } ;
	static const REGSAM views[2] = { KEY_WOW64_64KEY, KEY_WOW64_32KEY } ;
	char seen[32][300] ;
	int nseen = 0, h, v, i, k ;
	DWORD idx ;

	*claimed = 0 ;
	for( h = 0 ; h < 2 ; h++ ) for( v = 0 ; v < 2 ; v++ ) {
		HKEY hr ;
		if( RegOpenKeyExA( hives[h], DEFAPPS_REGAPPS, 0, KEY_QUERY_VALUE|views[v], &hr ) != ERROR_SUCCESS )
			continue ;
		for( idx = 0 ; ; idx++ ) {
			char name[256], caps[512], app[256] ;
			DWORD nlen = sizeof(name), dlen = sizeof(caps)-1, alen, type ;
			HKEY hc, hu ;
			LONG rc = RegEnumValueA( hr, idx, name, &nlen, NULL, &type, (BYTE *)caps, &dlen ) ;
			if( rc == ERROR_NO_MORE_ITEMS ) break ;
			if( rc != ERROR_SUCCESS || ( type != REG_SZ && type != REG_EXPAND_SZ ) ) continue ;
			caps[dlen] = '\0' ;
			if( !stricmp( name, DEFAPPS_NAME ) || !caps[0] ) continue ;
			/* the path in the view it was listed in first, then the other */
			if( RegOpenKeyExA( hives[h], caps, 0, KEY_READ|views[v], &hc ) != ERROR_SUCCESS &&
			    RegOpenKeyExA( hives[h], caps, 0, KEY_READ|views[1-v], &hc ) != ERROR_SUCCESS )
				continue ;
			/* the name Settings shows; an indirect "@dll,-id" one is not
			 * resolved here - the value name stands in for it */
			alen = sizeof(app)-1 ;
			if( RegQueryValueExA( hc, "ApplicationName", NULL, &type, (BYTE *)app, &alen ) == ERROR_SUCCESS &&
			    ( type == REG_SZ || type == REG_EXPAND_SZ ) ) app[alen] = '\0' ;
			else app[0] = '\0' ;
			if( !app[0] || app[0] == '@' ) snprintf( app, sizeof(app), "%s", name ) ;
			if( RegOpenKeyExA( hc, "URLAssociations", 0, KEY_QUERY_VALUE, &hu ) == ERROR_SUCCESS ) {
				for( i = 0 ; i < DEFAPPS_NSCHEMES ; i++ ) {
					char pv[256], tag[300] ;
					DWORD plen = sizeof(pv)-1 ;
					int dup = 0 ;
					if( !offered[i] ) continue ;
					if( RegQueryValueExA( hu, defapps_schemes[i], NULL, &type, (BYTE *)pv, &plen ) != ERROR_SUCCESS )
						continue ;
					pv[plen] = '\0' ;
					if( !pv[0] ) continue ;
					/* listed in both views (a shared key, or 32-bit Windows),
					 * or in both hives: one line */
					snprintf( tag, sizeof(tag), "%s|%s", defapps_schemes[i], app ) ;
					for( k = 0 ; k < nseen ; k++ ) if( !stricmp( seen[k], tag ) ) { dup = 1 ; break ; }
					if( dup ) continue ;
					if( nseen < (int)(sizeof(seen)/sizeof(seen[0])) ) {
						snprintf( seen[nseen], sizeof(seen[0]), "%s", tag ) ;
						nseen++ ;
					}
					len = rep_add( report, len, size, KT_REG_DEFAPPS_CLAIMED,
						       defapps_schemes[i], app, app ) ;
					(*claimed)++ ;
				}
				RegCloseKey( hu ) ;
			}
			RegCloseKey( hc ) ;
		}
		RegCloseKey( hr ) ;
	}
	return len ;
}

/* Settings > Default apps exists from Windows 10 on. The version comes from
 * ntdll: GetVersionEx answers 6.2 to a process whose manifest does not claim
 * the newer system. */
static int defapps_windows10( void ) {
	typedef LONG (WINAPI *fn_RtlGetVersion)( PRTL_OSVERSIONINFOW ) ;
	HMODULE nt = GetModuleHandleA( "ntdll.dll" ) ;
	fn_RtlGetVersion p = nt ? (fn_RtlGetVersion)(void *)GetProcAddress( nt, "RtlGetVersion" ) : NULL ;
	RTL_OSVERSIONINFOW vi ;
	if( !p ) return 0 ;
	memset( &vi, 0, sizeof(vi) ) ;
	vi.dwOSVersionInfoSize = sizeof(vi) ;
	if( p( &vi ) != 0 ) return 0 ;
	return vi.dwMajorVersion >= 10 ;
}

/* Whether Settings may be opened after a registration another application
 * contests. Not for a scripted run (-yes: report only). Test builds only:
 * the environment variable KITTY_NO_SETTINGS_LAUNCH=1 suppresses it as
 * well, so a harness can drive the configuration box's Register button
 * without a Settings window appearing. */
static int defapps_may_open_settings( int assume_yes ) {
	if( assume_yes || !defapps_windows10() ) return 0 ;
#ifdef KITTY_TEST_BUILD_LABEL
	{
		char v[8] ;
		DWORD n = GetEnvironmentVariableA( "KITTY_NO_SETTINGS_LAUNCH", v, sizeof(v) ) ;
		if( n > 0 && n < sizeof(v) && !strcmp( v, "1" ) ) return 0 ;
	}
#endif
	return 1 ;
}

/* Settings > Default apps, on KiTTY++'s own page where Windows 11 supports
 * the parameter (registeredAppMachine for an HKLM registration,
 * registeredAppUser for HKCU); Windows 10 ignores it and shows Default apps. */
static void defapps_open_settings( HKEY root ) {
	SHELLEXECUTEINFOA sei ;
	HRESULT co = CoInitializeEx( NULL, COINIT_APARTMENTTHREADED|COINIT_DISABLE_OLE1DDE ) ;
	memset( &sei, 0, sizeof(sei) ) ;
	sei.cbSize = sizeof(sei) ;
	sei.fMask = SEE_MASK_FLAG_NO_UI ;
	sei.lpVerb = "open" ;
	sei.lpFile = root == HKEY_LOCAL_MACHINE ?
		"ms-settings:defaultapps?registeredAppMachine=KiTTY%2B%2B" :
		"ms-settings:defaultapps?registeredAppUser=KiTTY%2B%2B" ;
	sei.nShow = SW_SHOWNORMAL ;
	ShellExecuteExA( &sei ) ;
	if( SUCCEEDED( co ) ) CoUninitialize() ;
}

/* Remove what defapps_register() wrote in `root`: the RegisteredApplications
 * value, Software\kapper.net\KiTTY++ with its Capabilities, the KiTTYpp.*
 * ProgIds. In HKLM, Software\kapper.net goes too when nothing else is left
 * in it. The settings store (kapper.net\KiTTY) and the installer's key are
 * never touched. Returns 1 when anything was removed. */
static int defapps_remove( HKEY root ) {
	HKEY hk ;
	char key[128] ;
	int any = 0, i ;
	if( RegOpenKeyExA( root, DEFAPPS_REGAPPS, 0, KEY_QUERY_VALUE|KEY_SET_VALUE|KEY_WOW64_64KEY,
			   &hk ) == ERROR_SUCCESS ) {
		if( RegQueryValueExA( hk, DEFAPPS_NAME, NULL, NULL, NULL, NULL ) == ERROR_SUCCESS &&
		    RegDeleteValueA( hk, DEFAPPS_NAME ) == ERROR_SUCCESS ) any = 1 ;
		RegCloseKey( hk ) ;
	}
	/* relative to a handle opened in the 64-bit view: the subkey is not
	 * redirected again */
	if( RegOpenKeyExA( root, "Software\\kapper.net", 0, KEY_READ|KEY_WRITE|DELETE|KEY_WOW64_64KEY,
			   &hk ) == ERROR_SUCCESS ) {
		HKEY hc ;
		DWORD nsub = 1, nval = 1 ;
		if( RegOpenKeyExA( hk, DEFAPPS_KEYNAME, 0, KEY_READ, &hc ) == ERROR_SUCCESS ) {
			RegCloseKey( hc ) ;
			if( RegDeleteTreeA( hk, DEFAPPS_KEYNAME ) == ERROR_SUCCESS ) any = 1 ;
		}
		if( root != HKEY_LOCAL_MACHINE ||
		    RegQueryInfoKeyA( hk, NULL, NULL, NULL, &nsub, NULL, NULL, &nval,
				      NULL, NULL, NULL, NULL ) != ERROR_SUCCESS ) nsub = nval = 1 ;
		RegCloseKey( hk ) ;
		if( nsub == 0 && nval == 0 &&
		    RegOpenKeyExA( root, "Software", 0, KEY_READ|KEY_WRITE|DELETE|KEY_WOW64_64KEY,
				   &hk ) == ERROR_SUCCESS ) {
			RegDeleteKeyA( hk, "kapper.net" ) ;
			RegCloseKey( hk ) ;
		}
	}
	for( i = 0 ; i < DEFAPPS_NSCHEMES ; i++ ) {
		snprintf( key, sizeof(key), "Software\\Classes\\" DEFAPPS_PROGID_FMT, defapps_schemes[i] ) ;
		if( ClassKeyExists( root, key ) && RegDeleteTreeA( root, key ) == ERROR_SUCCESS ) any = 1 ;
	}
	return any ;
}

// Create the SSH handler
/* KiTTY, rewritten. What it used to do: write telnet/ssh/putty
 * straight into HKEY_CLASSES_ROOT - i.e. HKEY_LOCAL_MACHINE - overwriting
 * whatever handled those links before, and reporting success even when every
 * write had been refused for lack of elevation (RegTestOrCreate ignored its
 * return codes, and Windows' own telnet handler survived only by accident).
 *
 * Now:
 *  - per-user when it has to be. HKLM if this process may write there,
 *    otherwise HKCU\Software\Classes, which needs no elevation and takes
 *    precedence for this user anyway. Registering for yourself is the normal
 *    case; needing the whole machine is the exception.
 *  - non-destructive by default. A protocol already pointing somewhere else is
 *    left alone and reported; `force` is what replaces it, and then the report
 *    lists what was replaced, so it can be put back.
 *  - a portable KiTTY requests a confirmation before writing anything, and
 *    shows the path that would be left behind in the registry once the stick
 *    is gone.
 *  - our own session-URL scheme is kitty://. putty:// is another project's
 *    name and is registered only when asked for (`withputty`); both forms are
 *    understood on the command line either way.
 */
void CreateSSHHandler( int force, int peruser, int assume_yes, int withputty ) {
	char path[1024], report[4096], cmd[1200], prev[1024], backup[MAX_PATH] ;
	char key[512] ;
	const char *prefix ;
	HKEY root ;
	int n = 0, written = 0, kept = 0, replaced = 0 ;
	size_t len ;
	int i, mine ;
	char progid[64] ;
	int progid_ok[DEFAPPS_NSCHEMES] = { 0 }, offered[DEFAPPS_NSCHEMES], claimed = 0, open_settings ;
	/* session = the URL refers to a saved session, so the command is -load;
	 * optional = registered only when explicitly asked for. */
	static const struct { const char *proto, *friendly ; int session, optional ; } protos[] = {
		{ "telnet", "URL:Telnet Protocol",  0, 0 },
		{ "ssh",    "URL:SSH Protocol",     0, 0 },
		{ "kitty",  "URL:KiTTY++ Session",    1, 0 },
		{ "putty",  "URL:PuTTY Session",    1, 1 },
	} ;

	GetModuleFileName( NULL, (LPTSTR)path, 1024 ) ;

	if( !ClassRegTarget( path, "-sshhandler",
			     force ? ( assume_yes ? " -force -yes" : " -force" ) :
				     ( assume_yes ? " -yes" : "" ),
			     peruser, assume_yes,
			     KT_REG_URL_WHAT,
			     KT_CAP_URL_HANDLERS, &root ) )
		return ;
	prefix = "Software\\Classes\\" ;

	len = snprintf( report, sizeof(report), KT_REG_URL_REPORT_HEAD,
			root == HKEY_LOCAL_MACHINE ?
			KT_REG_FOR_ALL_USERS :
			KT_REG_FOR_YOUR_ACCOUNT , path ) ;

	for( i = 0 ; i < (int)(sizeof(protos)/sizeof(protos[0])) ; i++ ) {
		int had ;
		if( protos[i].optional && !withputty ) continue ;
		had = UrlHandlerCurrentCommand( protos[i].proto, prev, sizeof(prev) ) ;
		n++ ;
		if( protos[i].session )
			snprintf( cmd, sizeof(cmd), "\"%s\" -load \"%%1\"", path ) ;
		else
			/* "%1" quoted: unquoted, a URL containing a space arrives split
			 * across arguments and only its first word reaches KiTTY. */
			snprintf( cmd, sizeof(cmd), "\"%s\" \"%%1\"", path ) ;

		/* Default apps: this scheme's own ProgId, with the same command.
		 * Written also when the protocol key below stays another
		 * program's - Default apps is where that is settled. */
		snprintf( progid, sizeof(progid), DEFAPPS_PROGID_FMT, protos[i].proto ) ;
		progid_ok[i] = UrlHandlerWrite( root, prefix, progid, protos[i].friendly, path, cmd ) ;

		if( had && !strcmp( prev, cmd ) ) {
			len += snprintf( report+len, sizeof(report)-len,
					 KT_REG_URL_ALREADY,
					 protos[i].proto ) ;
			written++ ;
			continue ;
		}
		if( had && !force ) {
			char prog[MAX_PATH] ;
			UrlHandlerProgram( prev, prog, sizeof(prog) ) ;
			len += snprintf( report+len, sizeof(report)-len,
					 KT_REG_URL_LEFT_ALONE,
					 protos[i].proto, prog[0] ? prog : KT_REG_ANOTHER_PROGRAM, prev ) ;
			kept++ ;
			continue ;
		}
		/* About to take a protocol over. What "undo" means depends on where
		 * the handler being displaced actually lives: if the hive we write to
		 * already holds this key, we are overwriting it and the way back is to
		 * import the copy taken here; if it does not, we are shadowing a
		 * registration in the other hive, and the way back is to delete what
		 * we are about to create - Windows then falls through to it again. */
		snprintf( key, sizeof(key), "%s%s", prefix, protos[i].proto ) ;
		mine = ClassKeyExists( root, key ) ;
		backup[0] = '\0' ;
		if( mine )
			ClassKeyBackup( root, key, protos[i].proto, backup, sizeof(backup) ) ;
		if( !UrlHandlerWrite( root, prefix, protos[i].proto,
				      protos[i].friendly, path, cmd ) ) {
			len += snprintf( report+len, sizeof(report)-len,
					 KT_REG_URL_NOT_WRITTEN, protos[i].proto ) ;
			continue ;
		}
		written++ ;
		if( had ) {
			char prog[MAX_PATH] ;
			UrlHandlerProgram( prev, prog, sizeof(prog) ) ;
			replaced++ ;
			len += snprintf( report+len, sizeof(report)-len,
					 KT_REG_URL_TAKEN_OVER,
					 protos[i].proto, prog[0] ? prog : KT_REG_ANOTHER_PROGRAM, prev ) ;
			if( mine && backup[0] )
				len += snprintf( report+len, sizeof(report)-len,
					 KT_REG_URL_UNDO_IMPORT, backup ) ;
			else if( mine )
				len += snprintf( report+len, sizeof(report)-len,
					 KT_REG_URL_NO_BACKUP ) ;
			else
				len += snprintf( report+len, sizeof(report)-len,
					 KT_REG_URL_UNDO_DELETE,
					 root == HKEY_LOCAL_MACHINE ? "HKLM" : "HKCU",
					 prefix, protos[i].proto,
					 root == HKEY_LOCAL_MACHINE ?
						KT_REG_URL_REG_YOUR_ACCOUNT :
						KT_REG_URL_REG_ALL_USERS,
					 prog[0] ? prog : KT_REG_PREVIOUS_PROGRAM, protos[i].proto ) ;
		} else {
			len += snprintf( report+len, sizeof(report)-len,
					 KT_REG_URL_REGISTERED, protos[i].proto ) ;
		}
		if( len >= sizeof(report) ) break ;   /* report full; stop appending */
	}

	/* Default apps: offered, then who else claims those schemes there. The
	 * report shows first (a modal box from the configuration box), Settings
	 * opens after it. */
	len = defapps_register( root, path, progid_ok, offered, report, len, sizeof(report) ) ;
	len = defapps_competitors( offered, report, len, sizeof(report), &claimed ) ;
	open_settings = claimed && defapps_may_open_settings( assume_yes ) ;
	if( open_settings )
		len = rep_add( report, len, sizeof(report), "%s", KT_REG_DEFAPPS_OPEN_SETTINGS ) ;

	if( kept && len < sizeof(report) )
		snprintf( report+len, sizeof(report)-len,
			  KT_REG_URL_KEPT_NOTE,
			  kept, kept == 1 ? "it is" : "they are",
			  kept == 1 ? "it" : "them" ) ;

	KittyCliReport( KT_CAP_URL_HANDLERS, report, written == n ? 0 : 1 ) ;
	if( open_settings ) defapps_open_settings( root ) ;
	(void)replaced ;
}

/* KiTTY (-sshhandler -uninstall): remove the URL handlers again.
 *
 * Only ones that point at a KiTTY are removed - a handler belonging to some
 * other program is left exactly where it is, even if it sits under a protocol
 * we can register. Each key is exported before it is deleted, so this is
 * undoable too. HKCU first (that is where an unelevated registration went),
 * then HKLM if this process may write there. */
void RemoveSSHHandler( void ) {
	static const char *protos[] = { "telnet", "ssh", "kitty", "putty" } ;
	static const struct { HKEY root ; const char *label ; } hives[] = {
		{ HKEY_CURRENT_USER,  KT_REG_YOUR_ACCOUNT },
		{ HKEY_LOCAL_MACHINE, KT_REG_ALL_USERS },
	} ;
	char report[4096], key[512], cur[1024], backup[MAX_PATH], prog[MAX_PATH] ;
	size_t len ;
	int i, h, removed = 0, kept = 0, defapps = 0 ;

	len = snprintf( report, sizeof(report),
			KT_REG_URL_REMOVING ) ;

	for( h = 0 ; h < (int)(sizeof(hives)/sizeof(hives[0])) ; h++ ) {
		for( i = 0 ; i < (int)(sizeof(protos)/sizeof(protos[0])) ; i++ ) {
			HKEY hk ;
			DWORD sz = sizeof(cur) ;
			snprintf( key, sizeof(key), "Software\\Classes\\%s", protos[i] ) ;
			if( RegOpenKeyExA( hives[h].root, key, 0, KEY_READ, &hk ) != ERROR_SUCCESS )
				continue ;
			RegCloseKey( hk ) ;

			snprintf( key, sizeof(key),
				  "Software\\Classes\\%s\\shell\\open\\command", protos[i] ) ;
			cur[0] = '\0' ;
			if( RegGetValueA( hives[h].root, key, NULL, RRF_RT_REG_SZ, NULL,
					  cur, &sz ) != ERROR_SUCCESS ) cur[0] = '\0' ;

			/* Ours? Anything else stays. */
			UrlHandlerProgram( cur, prog, sizeof(prog) ) ;
			if( strnicmp( prog, "kitty", 5 ) != 0 ) {
				if( cur[0] ) {
					len += snprintf( report+len, sizeof(report)-len,
						 KT_REG_URL_RM_LEFT_ALONE,
						 protos[i], hives[h].label,
						 prog[0] ? prog : KT_REG_ANOTHER_PROGRAM ) ;
					kept++ ;
				}
				continue ;
			}

			backup[0] = '\0' ;
			snprintf( key, sizeof(key), "Software\\Classes\\%s", protos[i] ) ;
			ClassKeyBackup( hives[h].root, key, protos[i], backup, sizeof(backup) ) ;
			if( RegDeleteTreeA( hives[h].root, key ) == ERROR_SUCCESS ) {
				removed++ ;
				len += snprintf( report+len, sizeof(report)-len,
					 KT_REG_URL_REMOVED, protos[i], hives[h].label ) ;
				if( backup[0] )
					len += snprintf( report+len, sizeof(report)-len,
						 KT_REG_URL_UNDO_IMPORT, backup ) ;
				else
					len += snprintf( report+len, sizeof(report)-len,
						 KT_REG_URL_NO_BACKUP ) ;
			} else {
				len += snprintf( report+len, sizeof(report)-len,
					 KT_REG_URL_NOT_REMOVED,
					 protos[i], hives[h].label,
					 hives[h].root == HKEY_LOCAL_MACHINE ?
					 KT_REG_NEEDS_ADMIN : "" ) ;
			}
			if( len >= sizeof(report) ) break ;
		}
	}

	/* Default apps: our RegisteredApplications value, Capabilities and
	 * ProgIds, in each hive that holds them (HKLM only when this process
	 * may write there - otherwise the opens fail and it stays). */
	for( h = 0 ; h < (int)(sizeof(hives)/sizeof(hives[0])) ; h++ )
		if( defapps_remove( hives[h].root ) ) defapps = 1 ;
	if( defapps ) {
		removed++ ;
		len = rep_add( report, len, sizeof(report), "%s", KT_REG_DEFAPPS_REMOVED ) ;
	}

	if( !removed && len < sizeof(report) )
		snprintf( report+len, sizeof(report)-len,
			  KT_REG_URL_NOTHING_REGISTERED,
			  kept ? KT_REG_URL_OTHERS : "" ) ;

	KittyCliReport( KT_CAP_URL_HANDLERS, report, removed ? 0 : 1 ) ;
}

// Create the *.ktx file association
/* KiTTY, rewritten alongside CreateSSHHandler(), which had the same
 * three faults: it wrote to HKEY_CLASSES_ROOT (= HKLM) and so did nothing at
 * all from an unelevated prompt while reporting nothing either; it claimed the
 * extension even when another program owned it; and it never reported what it
 * had done. Same treatment - HKLM if we may, elevation for the machine-wide
 * install, HKCU otherwise, the existing owner left alone unless `force` (and
 * then exported first), a confirmation first for a portable copy, and the
 * outcome reported to the console it was run from. */
void CreateFileAssoc( int force, int peruser, int assume_yes ) {
	char path[1024], buffer[1024], report[2048], cur[512], backup[MAX_PATH] ;
	char ext[15], key[256] ;
	HKEY root ;
	DWORD sz ;
	size_t len ;
	int had = 0, mine = 0 ;

	if( strlen( FileExtension ) > 0 ) { snprintf( ext, sizeof(ext), "%s", FileExtension ) ; } else { snprintf( ext, sizeof(ext), "%s", ".ktx") ; }

	GetModuleFileName( NULL, (LPTSTR)path, 1024 ) ;

	snprintf( buffer, sizeof(buffer), KT_REG_ASSOC_WHAT, ext ) ;
	if( !ClassRegTarget( path, "-fileassoc", force ? " -force" : "",
			     peruser, assume_yes, buffer,
			     KT_CAP_FILE_ASSOC, &root ) )
		return ;

	/* Who owns the extension today? Its default value is the ProgID that
	 * opens it - ours is kitty.connect.1. */
	sz = sizeof(cur) ; cur[0] = '\0' ;
	if( RegGetValueA( HKEY_CLASSES_ROOT, ext, NULL, RRF_RT_REG_SZ, NULL,
			  cur, &sz ) == ERROR_SUCCESS && cur[0] )
		had = 1 ;

	len = snprintf( report, sizeof(report), KT_REG_ASSOC_REPORT_HEAD,
			root == HKEY_LOCAL_MACHINE ?
			KT_REG_FOR_ALL_USERS :
			KT_REG_FOR_YOUR_ACCOUNT, ext, path ) ;

	if( had && strcmp( cur, "kitty.connect.1" ) != 0 && !force ) {
		snprintf( report+len, sizeof(report)-len,
			  KT_REG_ASSOC_LEFT_ALONE, ext, cur ) ;
		KittyCliReport( KT_CAP_FILE_ASSOC, report, 1 ) ;
		return ;
	}

	/* Same question as for a URL handler: are we overwriting this hive's own
	 * key, or shadowing one in the other hive? The undo differs. */
	snprintf( key, sizeof(key), "Software\\Classes\\%s", ext ) ;
	mine = ClassKeyExists( root, key ) ;
	backup[0] = '\0' ;
	if( mine )
		ClassKeyBackup( root, key, ext[0] == '.' ? ext+1 : ext,
				backup, sizeof(backup) ) ;

	// Associate .ktx files with the KiTTY application
	// Create the application entry
	snprintf( key, sizeof(key), "Software\\Classes\\kitty.connect.1" ) ;
	/* The type Explorer shows for a .ktx file ("Type" column, Properties).
	 * Classic KiTTY also wrote FriendlyTypeName "@KiTTY, -120" - an indirect
	 * string pointing at no module and no string resource, so Windows always fell
	 * back to this default value; a registration left by it is cleaned up. */
	RegTestOrCreate( root, key, "", "KiTTY++ session file") ;
	{
		HKEY hk ;
		if( RegOpenKeyExA( root, key, 0, KEY_SET_VALUE, &hk ) == ERROR_SUCCESS ) {
			RegDeleteValueA( hk, "FriendlyTypeName" ) ;
			RegCloseKey( hk ) ;
		}
	}
	snprintf( key, sizeof(key), "Software\\Classes\\kitty.connect.1\\CurVer" ) ;
	RegTestOrCreate( root, key, "", "kitty.connect.1") ;
	snprintf( buffer, sizeof(buffer), "%s", path ) ;
	snprintf( key, sizeof(key), "Software\\Classes\\kitty.connect.1\\DefaultIcon" ) ;
	RegTestOrCreate( root, key, "", buffer);
	snprintf( buffer, sizeof(buffer), "\"%s\" -kload \"%%1\"", path ) ;
	snprintf( key, sizeof(key), "Software\\Classes\\kitty.connect.1\\shell\\open\\command" ) ;
	if( !RegTestOrCreate( root, key, "", buffer) ) {
		snprintf( report+len, sizeof(report)-len,
			  KT_REG_ASSOC_NOT_WRITTEN ) ;
		KittyCliReport( KT_CAP_FILE_ASSOC, report, 1 ) ;
		return ;
	}
	// Create the file association
	snprintf( key, sizeof(key), "Software\\Classes\\%s", ext ) ;
	/* the one value that IS the association: it went unchecked and a failed
	 * write still ended in "done" */
	if( !RegTestOrCreate( root, key, "", "kitty.connect.1") ) {
		snprintf( report+len, sizeof(report)-len, KT_REG_ASSOC_NOT_WRITTEN ) ;
		KittyCliReport( KT_CAP_FILE_ASSOC, report, 1 ) ;
		return ;
	}
	RegTestOrCreate( root, key, "PerceivedType", "Connection") ;
	RegTestOrCreate( root, key, "Content Type", "connection/ssh") ;
	/* "Open with": Windows reads a SUBKEY OpenWithProgids whose value NAMES
	 * the ProgID (empty data). Classic KiTTY wrote a VALUE of that name
	 * instead, which Windows ignores; a registration left by it, or by an
	 * earlier KiTTY++, loses that value here. */
	{
		HKEY hk ;
		char owp[256] ;
		if( RegOpenKeyExA( root, key, 0, KEY_SET_VALUE, &hk ) == ERROR_SUCCESS ) {
			RegDeleteValueA( hk, "OpenWithProgids" ) ;
			RegCloseKey( hk ) ;
		}
		snprintf( owp, sizeof(owp), "%s\\OpenWithProgids", key ) ;
		RegTestOrCreate( root, owp, "kitty.connect.1", "" ) ;
	}

	if( had && strcmp( cur, "kitty.connect.1" ) != 0 ) {
		len += snprintf( report+len, sizeof(report)-len,
				 KT_REG_ASSOC_TAKEN_OVER, ext, cur ) ;
		if( mine && backup[0] )
			snprintf( report+len, sizeof(report)-len,
				  KT_REG_ASSOC_UNDO_IMPORT, backup ) ;
		else if( mine )
			snprintf( report+len, sizeof(report)-len,
				  KT_REG_ASSOC_NO_BACKUP ) ;
		else
			snprintf( report+len, sizeof(report)-len,
				  KT_REG_ASSOC_UNDO_DELETE,
				  root == HKEY_LOCAL_MACHINE ? "HKLM" : "HKCU", ext,
				  root == HKEY_LOCAL_MACHINE ?
					KT_REG_ASSOC_YOUR_ACCOUNT :
					KT_REG_ASSOC_ALL_USERS,
				  cur, ext ) ;
	} else {
		snprintf( report+len, sizeof(report)-len, KT_REG_ASSOC_DONE, ext ) ;
	}
	KittyCliReport( KT_CAP_FILE_ASSOC, report, 0 ) ;
}

/* KiTTY (-fileassoc -uninstall): give the extension back. Only removed when it
 * is still ours, and the ProgID with it. Exported first, like everything else
 * here, so the removal can be undone. */
void RemoveFileAssoc( void ) {
	static const struct { HKEY root ; const char *label ; } hives[] = {
		{ HKEY_CURRENT_USER,  KT_REG_YOUR_ACCOUNT },
		{ HKEY_LOCAL_MACHINE, KT_REG_ALL_USERS },
	} ;
	char ext[15], key[256], cur[512], backup[MAX_PATH], report[2048] ;
	size_t len ;
	int h, removed = 0 ;
	DWORD sz ;

	if( strlen( FileExtension ) > 0 ) { snprintf( ext, sizeof(ext), "%s", FileExtension ) ; } else { snprintf( ext, sizeof(ext), "%s", ".ktx") ; }

	len = snprintf( report, sizeof(report),
			KT_REG_ASSOC_REMOVING, ext ) ;

	for( h = 0 ; h < (int)(sizeof(hives)/sizeof(hives[0])) ; h++ ) {
		snprintf( key, sizeof(key), "Software\\Classes\\%s", ext ) ;
		sz = sizeof(cur) ; cur[0] = '\0' ;
		if( RegGetValueA( hives[h].root, key, NULL, RRF_RT_REG_SZ, NULL,
				  cur, &sz ) == ERROR_SUCCESS && cur[0] ) {
			if( strcmp( cur, "kitty.connect.1" ) != 0 ) {
				len += snprintf( report+len, sizeof(report)-len,
					 KT_REG_ASSOC_RM_LEFT_ALONE,
					 ext, hives[h].label, cur ) ;
				continue ;
			}
			backup[0] = '\0' ;
			ClassKeyBackup( hives[h].root, key, ext[0] == '.' ? ext+1 : ext,
					backup, sizeof(backup) ) ;
			if( RegDeleteTreeA( hives[h].root, key ) == ERROR_SUCCESS ) {
				removed++ ;
				len += snprintf( report+len, sizeof(report)-len,
					 KT_REG_ASSOC_REMOVED, ext, hives[h].label ) ;
				if( backup[0] )
					len += snprintf( report+len, sizeof(report)-len,
						 KT_REG_ASSOC_UNDO_IMPORT, backup ) ;
				else
					len += snprintf( report+len, sizeof(report)-len,
						 KT_REG_ASSOC_NO_BACKUP ) ;
			} else {
				len += snprintf( report+len, sizeof(report)-len,
					 KT_REG_ASSOC_NOT_REMOVED, ext, hives[h].label,
					 hives[h].root == HKEY_LOCAL_MACHINE ?
					 KT_REG_NEEDS_ADMIN : "" ) ;
			}
		}
		snprintf( key, sizeof(key), "Software\\Classes\\kitty.connect.1" ) ;
		if( RegDeleteTreeA( hives[h].root, key ) == ERROR_SUCCESS && len < sizeof(report) )
			len += snprintf( report+len, sizeof(report)-len,
				 KT_REG_PROGID_REMOVED, hives[h].label ) ;
	}

	if( !removed && len < sizeof(report) )
		snprintf( report+len, sizeof(report)-len,
			  KT_REG_ASSOC_NOTHING ) ;

	KittyCliReport( KT_CAP_FILE_ASSOC, report, removed ? 0 : 1 ) ;
}
	
// Check for KiTTY registry key. If not, copy from PuTTY one
void TestRegKeyOrCopyFromPuTTY( HKEY hMainKey, char * KeyName ) { 
	HKEY hKey ;
	if( RegOpenKeyEx( hMainKey, TEXT(KeyName), 0, KEY_READ, &hKey) == ERROR_SUCCESS ) {
		RegCloseKey( hKey ) ;
	} else {
		RegCreateKey( hMainKey, TEXT(KeyName), &hKey ) ;
		RegCloseKey( hKey ) ;
		kitty_RegCopyTree( hMainKey, "Software\\SimonTatham\\PuTTY", TEXT(KeyName) ) ;
	}
}


/******
Remove every trace of KiTTY from the registry.
Write and run a utf-8 .reg file containing the lines:

Windows Registry Editor Version 5.00

[-HKEY_CURRENT_USER\Software\9bis.com\KiTTY]

[-HKEY_CLASSES_ROOT\telnet]

[-HKEY_CLASSES_ROOT\ssh]

[-HKEY_CLASSES_ROOT\putty]

[-HKEY_CLASSES_ROOT\kitty.connect.1]

[-HKEY_CLASSES_ROOT\.ktx]

******/

/* ---- KiTTY++ Settings > System: the shell integration's state ----------- */

/* The program a "shell\open\command" line runs: its full path, unquoted. */
static void CommandProgramPath( const char *command, char *out, size_t outlen ) {
	const char *p = command, *end ;
	size_t n ;
	out[0] = '\0' ;
	while( *p == ' ' ) p++ ;
	if( *p == '"' ) { p++ ; end = strchr( p, '"' ) ; }
	else            { end = strchr( p, ' ' ) ; }
	if( !end ) end = p + strlen(p) ;
	n = (size_t)(end - p) ;
	if( n == 0 || n >= outlen ) return ;
	memcpy( out, p, n ) ;
	out[n] = '\0' ;
}

/* One state line for a handler command: what the System leaf shows. */
static void IntegrationLine( const char *what, int had, const char *command,
			     const char *self, char *out, size_t outlen, int *ours ) {
	char prog[MAX_PATH], base[MAX_PATH] ;
	if( !had ) { snprintf( out, outlen, KT_REG_STATE_NOT_REGISTERED, what ) ; return ; }
	CommandProgramPath( command, prog, sizeof(prog) ) ;
	UrlHandlerProgram( command, base, sizeof(base) ) ;
	if( prog[0] && !stricmp( prog, self ) ) {
		snprintf( out, outlen, "%s  this KiTTY++", what ) ;
		(*ours)++ ;
	} else if( base[0] && !stricmp( base, "kitty.exe" ) ) {
		snprintf( out, outlen, KT_REG_STATE_OTHER_KITTY, what, prog ) ;
	} else {
		snprintf( out, outlen, "%s  %s", what, base[0] ? base : command ) ;
	}
}

int kitty_shell_integration_state( char lines[5][256] ) {
	static const char *const protos[] = { "telnet", "ssh", "kitty", "putty" } ;
	char self[1024], cmd[1024], progid[256], ext[15] ;
	DWORD sz ;
	int i, had, ours = 0 ;
	GetModuleFileName( NULL, (LPTSTR)self, sizeof(self) ) ;
	for( i = 0 ; i < 4 ; i++ ) {
		char what[24] ;
		snprintf( what, sizeof(what), "%s://", protos[i] ) ;
		had = UrlHandlerCurrentCommand( protos[i], cmd, sizeof(cmd) ) ;
		IntegrationLine( what, had, cmd, self, lines[i], 256, &ours ) ;
	}
	if( strlen( FileExtension ) > 0 ) snprintf( ext, sizeof(ext), "%s", FileExtension ) ;
	else snprintf( ext, sizeof(ext), ".ktx" ) ;
	sz = sizeof(progid) ; progid[0] = '\0' ;
	had = ( RegGetValueA( HKEY_CLASSES_ROOT, ext, NULL, RRF_RT_REG_SZ, NULL,
			      progid, &sz ) == ERROR_SUCCESS && progid[0] ) ;
	if( had && !stricmp( progid, "kitty.connect.1" ) ) {
		char key[300] ;
		snprintf( key, sizeof(key), "kitty.connect.1\\shell\\open\\command" ) ;
		sz = sizeof(cmd) ; cmd[0] = '\0' ;
		had = ( RegGetValueA( HKEY_CLASSES_ROOT, key, NULL, RRF_RT_REG_SZ, NULL,
				      cmd, &sz ) == ERROR_SUCCESS && cmd[0] ) ;
		IntegrationLine( ext, had, cmd, self, lines[4], 256, &ours ) ;
	} else if( had ) {
		snprintf( lines[4], 256, "%s  %s", ext, progid ) ;
	} else {
		snprintf( lines[4], 256, KT_REG_STATE_NOT_REGISTERED, ext ) ;
	}
	/* putty:// is optional; the count shows how many of the four that matter
	 * point at this exe. */
	{
		int required = 0 ;
		for( i = 0 ; i < 5 ; i++ )
			if( i != 3 && strstr( lines[i], "  this KiTTY++" ) ) required++ ;
		return required ;
	}
}

/* The System leaf's buttons: the same work as -sshhandler / -fileassoc, with
 * boxes instead of console output. force = take protocols and the extension
 * over from other programs (their settings are exported first, the report
 * gives the undo command). */
void kitty_shell_integration_register( int force ) {
	g_report_gui = 1 ;
	CreateSSHHandler( force, 0, 0, 0 ) ;
	CreateFileAssoc( force, 0, 0 ) ;
	g_report_gui = 0 ;
}

/* The leaf's third button: -sshhandler -uninstall + -fileassoc -uninstall,
 * reports in boxes. Only entries pointing at a KiTTY are removed. */
void kitty_shell_integration_unregister( void ) {
	g_report_gui = 1 ;
	RemoveSSHHandler( ) ;
	RemoveFileAssoc( ) ;
	g_report_gui = 0 ;
}
