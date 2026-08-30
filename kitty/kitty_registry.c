#include "kitty_registry.h"

#include "kitty_oldwin.h"   /* APIs newer than the oldest Windows we load on */
char * itoa (int __val, char *__s, int __radix) ;
/* kitty_tools.c; declared locally because this file deliberately includes
 * only kitty_registry.h (see the MigrateOldKittyHive rationale below). */
char * str_rtrim( char * s, const char * set ) ;
/* kitty.c, same reason: CreateSSHHandler() needs to know whether a portable
 * copy was configured to use the registry (kitty_store.h's SAVEMODE_REG). */
int GetIniFileFlag( void ) ;
#ifndef SAVEMODE_REG
#define SAVEMODE_REG 0
#endif
// Variante bornee: n'ecrit jamais plus de `rsize` octets (NUL final compris)
// dans rValue; une valeur trop longue est tronquee au lieu de deborder.
char * GetValueDataN(HKEY hkTopKey, char * lpSubKey, const char * lpValueName, char * rValue, size_t rsize){
    HKEY hkKey;
    DWORD lpType, dwDataSize = cstMaxRegLength;

  //Receptionne la valeur de réception lecture clé registre
	unsigned char * lpData ;
	if( rValue == NULL || rsize == 0 ) { return NULL ; }
	lpData = (unsigned char*) malloc( cstMaxRegLength + 1 ); // +1 for forced NUL
	if( lpData == NULL ) { return NULL ; }

    rValue[0] = '\0';
  //Lecture de la clé registre si ok passe à la suite...
    if (RegOpenKeyEx(hkTopKey,lpSubKey,0,KEY_READ,&hkKey) == ERROR_SUCCESS){

      if (RegQueryValueEx(hkKey,lpValueName,NULL,&lpType,lpData,&dwDataSize) == ERROR_SUCCESS){
      // RegQueryValueEx does NOT guarantee NUL-termination for the *_SZ types and a
      // value can fill the whole buffer; force a terminator so the copies below
      // cannot over-read past the data.
        if( dwDataSize > cstMaxRegLength ) dwDataSize = cstMaxRegLength ;
        lpData[dwDataSize] = '\0' ;
      //déchiffrage des différents type de clé dans registry
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
      else { RegCloseKey(hkKey); free(lpData); return NULL ; }
       free(lpData); // libère la mémoire
       RegCloseKey(hkKey);

    }//end if
    else { free(lpData); return NULL ; }
    return rValue;
  }//end function

/* Compat: ancienne signature non bornee -- le buffer destinataire DOIT faire
 * au moins cstMaxRegLength+2 octets. Preferer GetValueDataN( ..., sizeof(buf) ). */
char * GetValueData(HKEY hkTopKey, char * lpSubKey, const char * lpValueName, char * rValue){
    return GetValueDataN( hkTopKey, lpSubKey, lpValueName, rValue, cstMaxRegLength+2 ) ;
}

// Teste l'existance d'une clé
int RegTestKey( HKEY hMainKey, LPCTSTR lpSubKey ) {
	HKEY hKey ;
	if( lpSubKey == NULL ) return 1 ;
	if( strlen( lpSubKey ) == 0 ) return 1 ;
	if( RegOpenKeyEx( hMainKey, TEXT(lpSubKey), 0, KEY_WRITE, &hKey) != ERROR_SUCCESS ) return 0 ;
	RegCloseKey( hKey ) ;
	return 1 ;
	}
	
// Retourne le nombre de sous-keys
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

	// Teste l'existance d'une clé ou bien d'une valeur et la crée sinon
	// KiTTY: returns 1 on success, 0 if the key could not be opened or created
	// or the value not written. Writing under HKEY_CLASSES_ROOT lands in
	// HKEY_LOCAL_MACHINE and needs elevation, so failure here is ordinary and
	// callers that register file/URL associations must be able to say so.
	// (Both functions also used to carry on with an UNINITIALISED handle when
	// the create failed, which is what made the failure silent.)
int RegTestOrCreate( HKEY hMainKey, LPCTSTR lpSubKey, LPCTSTR name, LPCTSTR value ) {
	HKEY hKey = NULL ;
	int ok ;
	if( lpSubKey == NULL ) return 0 ;
	if( strlen( lpSubKey ) == 0 ) return 0 ;
	if( RegOpenKeyEx( hMainKey, TEXT(lpSubKey), 0, KEY_WRITE, &hKey) != ERROR_SUCCESS ) {
		if( RegCreateKey( hMainKey, lpSubKey, &hKey ) != ERROR_SUCCESS ) return 0 ;
		}
	ok = 1 ;
	if( name != NULL ) {
		if( RegSetValueEx( hKey, TEXT( name ), 0, REG_SZ, (const BYTE *)value, strlen(value)+1 ) != ERROR_SUCCESS )
			ok = 0 ;
		}
	RegCloseKey( hKey ) ;
	return ok ;
	}

// Test l'existance d'une clé ou bien d'une valeur DWORD et la crée sinon
int RegTestOrCreateDWORD( HKEY hMainKey, LPCTSTR lpSubKey, LPCTSTR name, DWORD value ) {
	HKEY hKey = NULL ;
	int ok ;
	if( lpSubKey == NULL ) return 0 ;
	if( strlen( lpSubKey ) == 0 ) return 0 ;
	if( RegOpenKeyEx( hMainKey, TEXT(lpSubKey), 0, KEY_WRITE, &hKey) != ERROR_SUCCESS ) {
		if( RegCreateKey( hMainKey, lpSubKey, &hKey ) != ERROR_SUCCESS ) return 0 ;
		}
	ok = 1 ;
	if( name != NULL ) {
		if( RegSetValueEx( hKey, TEXT( name ), 0, REG_DWORD, (LPBYTE)&value, sizeof(DWORD) ) != ERROR_SUCCESS )
			ok = 0 ;
		}
	RegCloseKey( hKey ) ;
	return ok ;
	}
	

// Initialise toutes les sessions avec une valeur (si oldvalue==NULL) ou uniquement celles qui ont la valeur oldvalue
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
					MessageBox(NULL,achKey,"Info",MB_OK);
					//RegTestOrCreate( hMainKey, buffer, name, value ) ;
			}
		}
	}
}
	
// Exporte toute une cle de registre
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

	// On ouvre la clé
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
	
// Détruit une valeur de clé de registre 
BOOL RegDelValue (HKEY hKeyRoot, LPTSTR lpSubKey, LPTSTR lpValue ) {
	HKEY hKey;
	LONG lResult;
	if( (lResult = RegOpenKeyEx (hKeyRoot, lpSubKey, 0, KEY_WRITE, &hKey)) == ERROR_SUCCESS ) {
		RegDeleteValue( hKey, lpValue ) ;
		RegCloseKey(hKey) ;
		}
	return TRUE;   
	}

// Detruit une clé de registre et ses sous-clé
BOOL RegDelTree (HKEY hKeyRoot, LPCTSTR lpSubKey) {
    TCHAR lpEnd[MAX_PATH];
    LONG lResult;
    DWORD dwSize;
    TCHAR szName[MAX_PATH];
    HKEY hKey;
    FILETIME ftWrite;

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
		kitty_RegCopyTree( HKEY_CURRENT_USER, "Software\\9bis.com\\KiTTY", "Software\\kapper.net\\KiTTY" ) ;
	}
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
 * tell) is left untouched. Idempotent via a marker value -- runs at most once.
 *
 * Registry paths are string literals, not PUTTY_REG_POS -- same rationale as
 * MigrateOldKittyHive above: fixed 9bis<->kapper.net migration endpoints (not the
 * runtime-flippable base), and this file intentionally includes only kitty_registry.h. */
void RepairSharrowDefaults( void ) {
	HKEY hSess ;
	char cur[cstMaxRegLength+2], old[cstMaxRegLength+2] ;
	char name[MAX_KEY_LENGTH+1], kpath[512], oldsess[512] ;
	DWORD idx, len ;

	/* one-time guard: skip if we've already run */
	if( GetValueDataN( HKEY_CURRENT_USER, "Software\\kapper.net\\KiTTY", "SharrowRepairDone", cur, sizeof(cur) ) != NULL )
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
				RegDelValue( HKEY_CURRENT_USER, kpath, "ShiftedArrowKeys" ) ;
			}
		}
		RegCloseKey( hSess ) ;
	}

	/* set the marker regardless, so we don't rescan every boot (even if the old
	 * 9bis hive is gone and nothing could be repaired). RegTestOrCreateDWORD hides
	 * the RegSetValueEx byte-buffer boilerplate and creates the base key if needed. */
	RegTestOrCreateDWORD( HKEY_CURRENT_USER, "Software\\kapper.net\\KiTTY", "SharrowRepairDone", 1 ) ;
}

/* One-time migration (2026-07-21): the per-session "Send file in current
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
	char cur[cstMaxRegLength+2] ;
	char name[MAX_KEY_LENGTH+1], kpath[512] ;
	DWORD idx, len ;

	/* one-time guard: skip if we've already run */
	if( GetValueDataN( HKEY_CURRENT_USER, "Software\\kapper.net\\KiTTY", "ScpAutoPwdMigrated", cur, sizeof(cur) ) != NULL )
		return ;

	if( RegOpenKeyEx( HKEY_CURRENT_USER, "Software\\kapper.net\\KiTTY\\Sessions", 0, KEY_READ, &hSess ) == ERROR_SUCCESS ) {
		for( idx = 0 ; ; idx++ ) {
			len = sizeof(name) ;
			if( RegEnumKeyEx( hSess, idx, name, &len, NULL, NULL, NULL, NULL ) != ERROR_SUCCESS ) break ;
			snprintf( kpath, sizeof(kpath), "Software\\kapper.net\\KiTTY\\Sessions\\%s", name ) ;
			if( GetValueDataN( HKEY_CURRENT_USER, kpath, "SCPAutoPwd", cur, sizeof(cur) ) == NULL ) continue ;
			/* the old option was on -> turn on its safe replacement */
			if( strcmp( cur, "1" ) == 0 )
				RegTestOrCreateDWORD( HKEY_CURRENT_USER, kpath, "OSC7CwdTracking", 1 ) ;
			/* drop the retired key regardless of its value */
			RegDelValue( HKEY_CURRENT_USER, kpath, "SCPAutoPwd" ) ;
		}
		RegCloseKey( hSess ) ;
	}

	/* set the marker regardless, so we don't rescan every boot */
	RegTestOrCreateDWORD( HKEY_CURRENT_USER, "Software\\kapper.net\\KiTTY", "ScpAutoPwdMigrated", 1 ) ;
}

// Copie une clé de registre vers une autre
void kitty_RegCopyTree( HKEY hMainKey, LPCTSTR lpSubKey, LPCTSTR lpDestKey ) {
	HKEY hKey, hDestKey ;
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
 
    TCHAR  achValue[MAX_VALUE_NAME]; 
    DWORD cchValue = MAX_VALUE_NAME; 
	
	DWORD lpType, dwDataSize = 1024 ;
	char * buffer = NULL, * destbuffer = NULL ;
	
	// On ouvre la clé
	if( RegOpenKeyEx( hMainKey, TEXT(lpSubKey), 0, KEY_READ, &hKey) != ERROR_SUCCESS ) return ;
	if( RegCreateKey( hMainKey, TEXT(lpDestKey), &hDestKey ) == ERROR_SUCCESS )
					RegCloseKey( hDestKey ) ;

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
 
    // Enumerate the key values. 
    if (cValues) 
    {
        //printf( "\nNumber of values: %d\n", cValues);

        for (i=0, retCode=ERROR_SUCCESS; i<cValues; i++) 
        { 
            cchValue = MAX_VALUE_NAME; 
            achValue[0] = '\0'; 
            retCode = RegEnumValue(hKey, i, 
                achValue, 
                &cchValue, 
                NULL, 
                NULL,
                NULL,
                NULL);
 
            if (retCode == ERROR_SUCCESS ) 
            { 
				unsigned char lpData[1024] ;
				dwDataSize = 1024 ;
				/* SECURITY: a value >1024 bytes returns ERROR_MORE_DATA and sets
				 * dwDataSize to the full length while lpData holds only 1024 bytes;
				 * copying dwDataSize bytes then reads off the end of the stack
				 * buffer. Only propagate values that fit. */
				if( RegQueryValueEx( hKey, TEXT( achValue ), 0, &lpType, lpData, &dwDataSize ) == ERROR_SUCCESS ) {
					if( RegOpenKeyEx( hMainKey, TEXT(lpDestKey), 0, KEY_WRITE, &hDestKey) != ERROR_SUCCESS ) return ;
					RegSetValueEx( hDestKey, TEXT( achValue ), 0, lpType, lpData, dwDataSize );
					RegCloseKey( hDestKey ) ;
				}
            } 
        }
    }
	
    // Enumerate the subkeys, until RegEnumKeyEx fails.
    if (cSubKeys)
    {
        //printf( "\nNumber of subkeys: %d\n", cSubKeys);

        for (i=0; i<cSubKeys; i++) 
        { 
            cbName = MAX_KEY_LENGTH;
            retCode = RegEnumKeyEx(hKey, i,
                     achKey, 
                     &cbName, 
                     NULL, 
                     NULL, 
                     NULL, 
                     &ftLastWriteTime); 
            if (retCode == ERROR_SUCCESS) 
            {
				buffer = (char*) malloc( strlen( TEXT(lpSubKey) ) + strlen( achKey ) + 3 ) ;
				sprintf( buffer, "%s\\%s", TEXT(lpSubKey), achKey ) ;
				destbuffer = (char*) malloc( strlen( TEXT(lpDestKey) ) + strlen( achKey ) + 3 ) ;
				sprintf( destbuffer, "%s\\%s", TEXT(lpDestKey), achKey ) ;
				if( RegCreateKey( hMainKey, destbuffer, &hDestKey ) == ERROR_SUCCESS )
					RegCloseKey( hDestKey ) ;
					
				kitty_RegCopyTree( hMainKey, buffer, destbuffer ) ;
				free( buffer );
				free( destbuffer );
            }
        }
    } 
 
	RegCloseKey( hKey ) ;
}

// Nettoie la clé de PuTTY pour enlever les clés et valeurs spécifique à KiTTY
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
	if( (retCode = RegOpenKeyEx ( HKEY_CURRENT_USER, "Software\\SimonTatham\\PuTTY", 0, KEY_WRITE, &hSubKey)) == ERROR_SUCCESS ) {
		RegDeleteValue( hSubKey, "Build" ) ;
		RegDeleteValue( hSubKey, "Folders" ) ;
		RegDeleteValue( hSubKey, "KiCount" ) ;
		RegDeleteValue( hSubKey, "KiLastSe" ) ;
		RegDeleteValue( hSubKey, "KiLastUH" ) ;
		RegDeleteValue( hSubKey, "KiLastUp" ) ;
		RegDeleteValue( hSubKey, "KiPath" ) ;
		RegDeleteValue( hSubKey, "KiSess" ) ;
		RegDeleteValue( hSubKey, "KiVers" ) ;
		RegDeleteValue( hSubKey, "CtHelperPath" ) ;
		RegDeleteValue( hSubKey, "PSCPPath" ) ;
		RegDeleteValue( hSubKey, "WinSCPPath" ) ;
		RegDeleteValue( hSubKey, "KiClassName" ) ;
		RegCloseKey(hSubKey) ;
		}
	
	RegDelTree (HKEY_CURRENT_USER, "Software\\SimonTatham\\PuTTY\\Commands" ) ;
	RegDelTree (HKEY_CURRENT_USER, "Software\\SimonTatham\\PuTTY\\Folders" ) ;
	RegDelTree (HKEY_CURRENT_USER, "Software\\SimonTatham\\PuTTY\\Launcher" ) ;
	
	// On ouvre la clé
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
					RegDeleteValue( hSubKey, "Folder" ) ;
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
					RegDeleteValue( hSubKey, "Password" ) ;
					RegDeleteValue( hSubKey, "Autocommand" ) ;
					RegDeleteValue( hSubKey, "AutocommandOut" ) ;
					RegDeleteValue( hSubKey, "AntiIdle" ) ;
					RegDeleteValue( hSubKey, "LogTimestamp" ) ;
					RegDeleteValue( hSubKey, "Notes" ) ;
					RegDeleteValue( hSubKey, "CygtermCommand" ) ;
					RegDeleteValue( hSubKey, "CygtermAltMetabit" ) ;
					RegDeleteValue( hSubKey, "CygtermAutoPath" ) ;
					RegDeleteValue( hSubKey, "Cygterm64" ) ;
					RegDeleteValue( hSubKey, "WakeupReconnect" ) ;
					RegDeleteValue( hSubKey, "FailureReconnect" ) ;
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
					RegDeleteValue( hSubKey, "rzCommand" ) ;
					RegDeleteValue( hSubKey, "rzOptions" ) ;
					RegDeleteValue( hSubKey, "szCommand" ) ;
					RegDeleteValue( hSubKey, "szOptions" ) ;
					RegDeleteValue( hSubKey, "zDownloadDir" ) ;
					RegDeleteValue( hSubKey, "SaveWindowPos" ) ;
					RegDeleteValue( hSubKey, "WindowState" ) ;
					RegDeleteValue( hSubKey, "ForegroundOnBell" ) ;
					RegDeleteValue( hSubKey, "CtrlTabSwitch" ) ;
					RegDeleteValue( hSubKey, "Comment" ) ;
					RegDeleteValue( hSubKey, "LogTimeRotation" ) ;
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

/* KiTTY: say something back from a command-line switch. kitty.exe is a
 * GUI-subsystem program and so starts with no console of its own; attaching to
 * the console of the shell that launched it puts the text where the user is
 * actually looking. A message box is the fallback for when there is no console
 * at all - started from Explorer, a shortcut or the installer - and not the
 * normal path: a switch typed at a prompt should answer at that prompt. */
void KittyCliReport( const char *title, const char *text, int warn ) {
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
		return ;
	}
	if( attached ) FreeConsole() ;
	MessageBoxA( NULL, text, title,
		     MB_OK | (warn ? MB_ICONWARNING : MB_ICONINFORMATION) ) ;
}

/* KiTTY: ask a yes/no question the same way KittyCliReport() answers one - at the
 * prompt when there is one, in a box when there is not. Returns 1 only for an
 * explicit yes; anything else, including a closed console or an unreadable
 * input, is a no, because the caller is about to write to the registry. */
static int CliConfirm( const char *title, const char *text ) {
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
		WriteFile( hout, "\r\nWrite these registry entries? [y/N] ", 38, &written, NULL ) ;
		if( ReadFile( hin, answer, sizeof(answer)-1, &nread, NULL ) && nread > 0 ) {
			answer[nread] = '\0' ;
			yes = ( answer[0] == 'y' || answer[0] == 'Y' ) ;
		}
		WriteFile( hout, "\r\n", 2, &written, NULL ) ;
	} else {
		yes = ( MessageBoxA( NULL, text, title,
				     MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2 ) == IDYES ) ;
	}
	if( hout != INVALID_HANDLE_VALUE ) CloseHandle( hout ) ;
	if( hin != INVALID_HANDLE_VALUE ) CloseHandle( hin ) ;
	if( attached ) FreeConsole() ;
	return yes ;
}

/* KiTTY: is this exe the machine-wide installation? The system MSI installs
 * into %ProgramFiles%\KiTTY (kitty-system.wxs uses ProgramFiles64Folder); the
 * per-user MSI and every portable copy live elsewhere. Used to decide whether
 * registering URL handlers should ask for administrator rights: a program
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
 * can act on - "rundll32.exe" says more at a glance than the full command with
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
 * touched included. The hive has to be named explicitly: an HKCR export is an
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

	n = GetEnvironmentVariableA( "LOCALAPPDATA", dir, sizeof(dir) ) ;
	if( n == 0 || n >= sizeof(dir) ) return 0 ;
	strncat( dir, "\\KiTTY", sizeof(dir)-strlen(dir)-1 ) ;
	CreateDirectoryA( dir, NULL ) ;   /* fine if it is already there */

	GetLocalTime( &st ) ;
	snprintf( file, filelen, "%s\\%s-%s-%04d%02d%02d-%02d%02d%02d.reg",
		  dir, hive, tag, st.wYear, st.wMonth, st.wDay,
		  st.wHour, st.wMinute, st.wSecond ) ;

	if( GetSystemDirectoryA( sysdir, sizeof(sysdir) ) == 0 ) return 0 ;
	snprintf( cmd, sizeof(cmd), "\"%s\\reg.exe\" export \"%s\\%s\" \"%s\" /y",
		  sysdir, hive, subkey, file ) ;
	memset( &si, 0, sizeof(si) ) ; si.cb = sizeof(si) ;
	si.dwFlags = STARTF_USESHOWWINDOW ; si.wShowWindow = SW_HIDE ;
	if( !CreateProcessA( NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW,
			     NULL, NULL, &si, &pi ) ) return 0 ;
	WaitForSingleObject( pi.hProcess, 30000 ) ;
	if( !GetExitCodeProcess( pi.hProcess, &rc ) ) rc = 1 ;
	CloseHandle( pi.hThread ) ; CloseHandle( pi.hProcess ) ;
	return rc == 0 ;
}

/* KiTTY: where should a class registration (URL handler, file association) be
 * written, and may we write it at all? Shared by -sshhandler and -fileassoc so
 * that the two behave the same.
 *
 * HKLM when this process can write there. Otherwise, if this is the
 * machine-wide installation, ask Windows for the rights and let the elevated
 * copy do the whole job (`relaunch` + `relaunch_extra` is the command line it
 * gets); -user (peruser) says "my account is what I meant". A portable copy is
 * asked for permission first, since what it writes outlives it.
 *
 * Returns 1 with *root set when the caller should proceed, 0 when it should
 * return quietly - the user has been told why. */
static int ClassRegTarget( const char *path, const char *relaunch,
			   const char *relaunch_extra,
			   int peruser, int assume_yes,
			   const char *what, const char *title, HKEY *root ) {
	HKEY classes ;

#ifdef MOD_PORTABLE
	/* A portable KiTTY is expected to leave the machine as it found it, and
	 * registering is the opposite of that: the entry outlives the USB stick
	 * and then points at a path that has gone. Still the user's call - so ask,
	 * plainly, with the path in front of them. savemode=registry already means
	 * "this copy uses the registry"; -yes answers for scripts. */
	if( !assume_yes && GetIniFileFlag() != SAVEMODE_REG ) {
		char question[2048] ;
		snprintf( question, sizeof(question),
			"This is a portable KiTTY. Registering %s writes to this "
			"machine's registry, pointing at:\r\n\r\n    %s\r\n\r\n"
			"Those entries stay behind when this copy is removed, and then "
			"point at nothing.", what, path ) ;
		if( !CliConfirm( title, question ) ) {
			KittyCliReport( title, "Nothing was registered.", 0 ) ;
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
		KittyCliReport( title,
			"This is the machine-wide installation of KiTTY, so this belongs "
			"to the whole machine - and that was refused or cancelled.\r\n"
			"Re-run from an administrator prompt, or add -user to register "
			"for your account only.", 1 ) ;
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

// Creation du SSH Handler
/* KiTTY, rewritten 2026-07-31. What it used to do: write telnet/ssh/putty
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
 *    says what was replaced, so it can be put back.
 *  - a portable KiTTY asks before writing anything, naming the path that would
 *    be left behind in the registry once the stick is gone.
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
	/* session = the URL names a saved session, so the command is -load;
	 * optional = registered only when explicitly asked for. */
	static const struct { const char *proto, *friendly ; int session, optional ; } protos[] = {
		{ "telnet", "URL:Telnet Protocol",  0, 0 },
		{ "ssh",    "URL:SSH Protocol",     0, 0 },
		{ "kitty",  "URL:KiTTY Session",    1, 0 },
		{ "putty",  "URL:PuTTY Session",    1, 1 },
	} ;

	GetModuleFileName( NULL, (LPTSTR)path, 1024 ) ;

	if( !ClassRegTarget( path, "-sshhandler", force ? " -force" : "",
			     peruser, assume_yes,
			     "the telnet://, ssh:// and kitty:// handlers",
			     "KiTTY URL handlers", &root ) )
		return ;
	prefix = "Software\\Classes\\" ;

	len = snprintf( report, sizeof(report), "%s\r\nRegistering: %s\r\n\r\n",
			root == HKEY_LOCAL_MACHINE ?
			"For all users of this machine (HKEY_LOCAL_MACHINE)." :
			"For your account only (HKEY_CURRENT_USER)." , path ) ;

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

		if( had && !strcmp( prev, cmd ) ) {
			len += snprintf( report+len, sizeof(report)-len,
					 "%s://  already registered for this KiTTY\r\n",
					 protos[i].proto ) ;
			written++ ;
			continue ;
		}
		if( had && !force ) {
			char prog[MAX_PATH] ;
			UrlHandlerProgram( prev, prog, sizeof(prog) ) ;
			len += snprintf( report+len, sizeof(report)-len,
					 "%s://  LEFT ALONE, currently opened by %s\r\n"
					 "           %s\r\n",
					 protos[i].proto, prog[0] ? prog : "another program", prev ) ;
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
					 "%s://  COULD NOT BE WRITTEN\r\n", protos[i].proto ) ;
			continue ;
		}
		written++ ;
		if( had ) {
			char prog[MAX_PATH] ;
			UrlHandlerProgram( prev, prog, sizeof(prog) ) ;
			replaced++ ;
			len += snprintf( report+len, sizeof(report)-len,
					 "%s://  taken over from %s\r\n           %s\r\n",
					 protos[i].proto, prog[0] ? prog : "another program", prev ) ;
			if( mine && backup[0] )
				len += snprintf( report+len, sizeof(report)-len,
					 "           to undo:  reg import \"%s\"\r\n", backup ) ;
			else if( mine )
				len += snprintf( report+len, sizeof(report)-len,
					 "           (the old setting could NOT be backed up)\r\n" ) ;
			else
				len += snprintf( report+len, sizeof(report)-len,
					 "           to undo:  reg delete \"%s\\%s%s\" /f\r\n"
					 "           (%s was not changed - deleting the entry above "
					 "lets %s open %s:// links again)\r\n",
					 root == HKEY_LOCAL_MACHINE ? "HKLM" : "HKCU",
					 prefix, protos[i].proto,
					 root == HKEY_LOCAL_MACHINE ?
						"the registration for your account" :
						"the registration for all users of this machine",
					 prog[0] ? prog : "the previous program", protos[i].proto ) ;
		} else {
			len += snprintf( report+len, sizeof(report)-len,
					 "%s://  registered\r\n", protos[i].proto ) ;
		}
		if( len >= sizeof(report) ) break ;   /* report full; stop appending */
	}

	if( kept && len < sizeof(report) )
		snprintf( report+len, sizeof(report)-len,
			  "\r\n%d left untouched because %s already opened by another "
			  "program. Add -force to take %s over as well; the setting "
			  "replaced is exported to a .reg file first, and the report then "
			  "names the command that puts it back.",
			  kept, kept == 1 ? "it is" : "they are",
			  kept == 1 ? "it" : "them" ) ;

	KittyCliReport( "KiTTY URL handlers", report, written == n ? 0 : 1 ) ;
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
		{ HKEY_CURRENT_USER,  "your account" },
		{ HKEY_LOCAL_MACHINE, "all users" },
	} ;
	char report[4096], key[512], cur[1024], backup[MAX_PATH], prog[MAX_PATH] ;
	size_t len ;
	int i, h, removed = 0, kept = 0 ;

	len = snprintf( report, sizeof(report),
			"Removing KiTTY's telnet://, ssh://, kitty:// and putty:// "
			"handlers.\r\n\r\n" ) ;

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
						 "%s:// (%s)  LEFT ALONE, opened by %s\r\n",
						 protos[i], hives[h].label,
						 prog[0] ? prog : "another program" ) ;
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
					 "%s:// (%s)  removed\r\n", protos[i], hives[h].label ) ;
				if( backup[0] )
					len += snprintf( report+len, sizeof(report)-len,
						 "           to undo:  reg import \"%s\"\r\n", backup ) ;
			} else {
				len += snprintf( report+len, sizeof(report)-len,
					 "%s:// (%s)  could not be removed%s\r\n",
					 protos[i], hives[h].label,
					 hives[h].root == HKEY_LOCAL_MACHINE ?
					 " - needs administrator rights" : "" ) ;
			}
			if( len >= sizeof(report) ) break ;
		}
	}

	if( !removed && len < sizeof(report) )
		snprintf( report+len, sizeof(report)-len,
			  "Nothing of KiTTY's was registered%s.",
			  kept ? " (the handlers above belong to other programs)" : "" ) ;

	KittyCliReport( "KiTTY URL handlers", report, removed ? 0 : 1 ) ;
}

// Creation de l'association de fichiers *.ktx
/* KiTTY, rewritten 2026-08-01 alongside CreateSSHHandler(), which had the same
 * three faults: it wrote to HKEY_CLASSES_ROOT (= HKLM) and so did nothing at
 * all from an unelevated prompt while reporting nothing either; it claimed the
 * extension even when another program owned it; and it never said what it had
 * done. Same treatment - HKLM if we may, elevation for the machine-wide
 * install, HKCU otherwise, the existing owner left alone unless `force` (and
 * then exported first), a portable copy asked, and the outcome reported to the
 * console that asked for it. */
void CreateFileAssoc( int force, int peruser, int assume_yes ) {
	char path[1024], buffer[1024], report[2048], cur[512], backup[MAX_PATH] ;
	char ext[15], key[256] ;
	HKEY root ;
	DWORD sz ;
	size_t len ;
	int had = 0, mine = 0 ;

	if( strlen( FileExtension ) > 0 ) { snprintf( ext, sizeof(ext), "%s", FileExtension ) ; } else { snprintf( ext, sizeof(ext), "%s", ".ktx") ; }

	GetModuleFileName( NULL, (LPTSTR)path, 1024 ) ;

	snprintf( buffer, sizeof(buffer), "the %s file association", ext ) ;
	if( !ClassRegTarget( path, "-fileassoc", force ? " -force" : "",
			     peruser, assume_yes, buffer,
			     "KiTTY file association", &root ) )
		return ;

	/* Who owns the extension today? Its default value is the ProgID that
	 * opens it - ours is kitty.connect.1. */
	sz = sizeof(cur) ; cur[0] = '\0' ;
	if( RegGetValueA( HKEY_CLASSES_ROOT, ext, NULL, RRF_RT_REG_SZ, NULL,
			  cur, &sz ) == ERROR_SUCCESS && cur[0] )
		had = 1 ;

	len = snprintf( report, sizeof(report), "%s\r\nAssociating %s with: %s\r\n\r\n",
			root == HKEY_LOCAL_MACHINE ?
			"For all users of this machine (HKEY_LOCAL_MACHINE)." :
			"For your account only (HKEY_CURRENT_USER).", ext, path ) ;

	if( had && strcmp( cur, "kitty.connect.1" ) != 0 && !force ) {
		snprintf( report+len, sizeof(report)-len,
			  "%s  LEFT ALONE, currently opened by \"%s\"\r\n\r\n"
			  "Add -force to take it over; the current setting is exported to "
			  "a .reg file first and this report then names the command that "
			  "restores it.", ext, cur ) ;
		KittyCliReport( "KiTTY file association", report, 1 ) ;
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

	// Association des fichers .ktx avec l'application KiTTY
	// Création d l'application
	snprintf( key, sizeof(key), "Software\\Classes\\kitty.connect.1" ) ;
	RegTestOrCreate( root, key, "", "KiTTY connection manager") ;
	RegTestOrCreate( root, key, "FriendlyTypeName", "@KiTTY, -120") ;
	snprintf( key, sizeof(key), "Software\\Classes\\kitty.connect.1\\CurVer" ) ;
	RegTestOrCreate( root, key, "", "kitty.connect.1") ;
	snprintf( buffer, sizeof(buffer), "%s", path ) ;
	snprintf( key, sizeof(key), "Software\\Classes\\kitty.connect.1\\DefaultIcon" ) ;
	RegTestOrCreate( root, key, "", buffer);
	snprintf( buffer, sizeof(buffer), "\"%s\" -kload \"%%1\"", path ) ;
	snprintf( key, sizeof(key), "Software\\Classes\\kitty.connect.1\\shell\\open\\command" ) ;
	if( !RegTestOrCreate( root, key, "", buffer) ) {
		snprintf( report+len, sizeof(report)-len,
			  "The registration could NOT be written." ) ;
		KittyCliReport( "KiTTY file association", report, 1 ) ;
		return ;
	}
	// Création de l'association de fichiers
	snprintf( key, sizeof(key), "Software\\Classes\\%s", ext ) ;
	RegTestOrCreate( root, key, "", "kitty.connect.1") ;
	RegTestOrCreate( root, key, "PerceivedType", "Connection") ;
	RegTestOrCreate( root, key, "Content Type", "connection/ssh") ;
	RegTestOrCreate( root, key, "OpenWithProgids", "kitty.connect.1") ;

	if( had && strcmp( cur, "kitty.connect.1" ) != 0 ) {
		len += snprintf( report+len, sizeof(report)-len,
				 "%s  taken over from \"%s\"\r\n", ext, cur ) ;
		if( mine && backup[0] )
			snprintf( report+len, sizeof(report)-len,
				  "     to undo:  reg import \"%s\"\r\n", backup ) ;
		else if( mine )
			snprintf( report+len, sizeof(report)-len,
				  "     (the old setting could NOT be backed up)\r\n" ) ;
		else
			snprintf( report+len, sizeof(report)-len,
				  "     to undo:  reg delete \"%s\\Software\\Classes\\%s\" /f\r\n"
				  "     (%s was not changed - deleting the entry above lets "
				  "\"%s\" open %s files again)\r\n",
				  root == HKEY_LOCAL_MACHINE ? "HKLM" : "HKCU", ext,
				  root == HKEY_LOCAL_MACHINE ?
					"the association for your account" :
					"the association for all users of this machine",
				  cur, ext ) ;
	} else {
		snprintf( report+len, sizeof(report)-len, "%s  associated\r\n", ext ) ;
	}
	KittyCliReport( "KiTTY file association", report, 0 ) ;
}

/* KiTTY (-fileassoc -uninstall): give the extension back. Only removed when it
 * is still ours; the ProgID goes with it. Exported first, like everything else
 * here, so the removal can be undone. */
void RemoveFileAssoc( void ) {
	static const struct { HKEY root ; const char *label ; } hives[] = {
		{ HKEY_CURRENT_USER,  "your account" },
		{ HKEY_LOCAL_MACHINE, "all users" },
	} ;
	char ext[15], key[256], cur[512], backup[MAX_PATH], report[2048] ;
	size_t len ;
	int h, removed = 0 ;
	DWORD sz ;

	if( strlen( FileExtension ) > 0 ) { snprintf( ext, sizeof(ext), "%s", FileExtension ) ; } else { snprintf( ext, sizeof(ext), "%s", ".ktx") ; }

	len = snprintf( report, sizeof(report),
			"Removing KiTTY's %s file association.\r\n\r\n", ext ) ;

	for( h = 0 ; h < (int)(sizeof(hives)/sizeof(hives[0])) ; h++ ) {
		snprintf( key, sizeof(key), "Software\\Classes\\%s", ext ) ;
		sz = sizeof(cur) ; cur[0] = '\0' ;
		if( RegGetValueA( hives[h].root, key, NULL, RRF_RT_REG_SZ, NULL,
				  cur, &sz ) == ERROR_SUCCESS && cur[0] ) {
			if( strcmp( cur, "kitty.connect.1" ) != 0 ) {
				len += snprintf( report+len, sizeof(report)-len,
					 "%s (%s)  LEFT ALONE, opened by \"%s\"\r\n",
					 ext, hives[h].label, cur ) ;
				continue ;
			}
			backup[0] = '\0' ;
			ClassKeyBackup( hives[h].root, key, ext[0] == '.' ? ext+1 : ext,
					backup, sizeof(backup) ) ;
			if( RegDeleteTreeA( hives[h].root, key ) == ERROR_SUCCESS ) {
				removed++ ;
				len += snprintf( report+len, sizeof(report)-len,
					 "%s (%s)  removed\r\n", ext, hives[h].label ) ;
				if( backup[0] )
					len += snprintf( report+len, sizeof(report)-len,
						 "     to undo:  reg import \"%s\"\r\n", backup ) ;
			} else {
				len += snprintf( report+len, sizeof(report)-len,
					 "%s (%s)  could not be removed%s\r\n", ext, hives[h].label,
					 hives[h].root == HKEY_LOCAL_MACHINE ?
					 " - needs administrator rights" : "" ) ;
			}
		}
		snprintf( key, sizeof(key), "Software\\Classes\\kitty.connect.1" ) ;
		if( RegDeleteTreeA( hives[h].root, key ) == ERROR_SUCCESS && len < sizeof(report) )
			len += snprintf( report+len, sizeof(report)-len,
				 "kitty.connect.1 (%s)  removed\r\n", hives[h].label ) ;
	}

	if( !removed && len < sizeof(report) )
		snprintf( report+len, sizeof(report)-len,
			  "Nothing of KiTTY's was associated." ) ;

	KittyCliReport( "KiTTY file association", report, removed ? 0 : 1 ) ;
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

// Exporte une clé de registre dans un fichier au format PuTTY (  name\value\ )
int ExportSubKeyToFile( HKEY hkey, const char *subkey, const char *keyname, const char *maindir, const char *subdir ) {
	char *fullkey, *fullpath ;
	FILE *fp;
	HKEY hKey;
	
	int retCode, i;
	unsigned char lpData[1024], unlpData[1024] ;
	TCHAR achClass[MAX_PATH] = TEXT(""), achValue[MAX_VALUE_NAME] ; 
	DWORD cchClassName = MAX_PATH, cSubKeys=0, cbMaxSubKey, cchMaxClass, cValues, cchMaxValue, cbMaxValueData, cbSecurityDescriptor, cchValue = MAX_VALUE_NAME , dwDataSize, lpType;
	FILETIME ftLastWriteTime; 
	
	if( subkey!=NULL ) {
		if( keyname!=NULL ) {
			fullkey = (char*) malloc( strlen(subkey)+strlen(keyname)+2 ) ;
			sprintf( fullkey, "%s\\%s", subkey, keyname ) ;
		} else {
			fullkey = (char*) malloc( strlen(subkey)+1 ) ;
			sprintf( fullkey, "%s", subkey ) ;
		}
	} else {
		fullkey = (char*) malloc( strlen(keyname)+1 ) ;
		sprintf( fullkey, "%s", keyname ) ;
	}
	
	if( maindir!=NULL ) {
		if( subdir!=NULL ) {
			fullpath = (char*) malloc( strlen(maindir)+strlen(subdir)+strlen(keyname)+3 ) ;
			sprintf( fullpath, "%s\\%s\\%s", maindir, subdir, keyname ) ;
		} else {
			fullpath = (char*) malloc( strlen(maindir)+strlen(keyname)+2 ) ;
			sprintf( fullpath, "%s\\%s", maindir, keyname ) ;
		}
	} else {
		fullpath = (char*) malloc( strlen(subdir)+strlen(keyname)+2 ) ;
		sprintf( fullpath, "%s\\%s", subdir, keyname ) ;
	}
	if( (fp=fopen(fullpath,"wb"))==NULL ) { free( fullpath ) ; free( fullkey ) ; return 1 ; }
	if( RegOpenKeyEx( HKEY_CURRENT_USER, TEXT(fullkey), 0, KEY_READ, &hKey) != ERROR_SUCCESS ) { free( fullpath ) ; free( fullkey ) ; return 2 ; }
	
	if( RegQueryInfoKey(hKey,achClass,&cchClassName,NULL,&cSubKeys,&cbMaxSubKey
		,&cchMaxClass,&cValues,&cchMaxValue,&cbMaxValueData,&cbSecurityDescriptor,&ftLastWriteTime) == ERROR_SUCCESS ) {
		retCode = ERROR_SUCCESS ;
		if(cValues) for (i=0, retCode=ERROR_SUCCESS; i<cValues; i++) {
			cchValue = MAX_VALUE_NAME; 
			achValue[0] = '\0';
			if( (retCode = RegEnumValue(hKey, i, achValue, &cchValue, NULL, NULL,NULL,NULL) ) == ERROR_SUCCESS ) {
				char *buffer=NULL;
				dwDataSize = 1024 ;
				RegQueryValueEx( hKey, TEXT( achValue ), 0, &lpType, lpData, &dwDataSize ) ;
				switch ((int)lpType) {
					case REG_BINARY:
						buffer = (char*) malloc( strlen( achValue ) + 50 ) ;
						sprintf( buffer, "%s\\", achValue ) ;
						itoa((u_int)(lpData[0]),buffer+strlen(buffer), 10);
						strcat(buffer,".");
						itoa((u_int)(lpData[1]),buffer+strlen(buffer),10);
						strcat(buffer,".");
						itoa((u_int)(lpData[2]),buffer+strlen(buffer),10);
						strcat(buffer,".");
						itoa((u_int)(lpData[3]),buffer+strlen(buffer),10);
						strcat( buffer, "\\" ) ;
						break;
  
					case REG_DWORD:
						buffer = (char*) malloc( strlen( achValue ) + 13 ) ;
						sprintf( buffer, "%s\\", achValue ) ;
						itoa(*(int*)(lpData),buffer+strlen(buffer),10) ;
						strcat( buffer, "\\" ) ;
						break;
					
					case REG_EXPAND_SZ:
					case REG_MULTI_SZ:
					case REG_SZ:
						mungestr( (const char*)lpData, (char*)unlpData ) ;
						buffer = (char*) malloc( strlen( achValue ) + strlen( (char*)unlpData ) + 3 ) ;
						sprintf( buffer, "%s\\%s\\", achValue, unlpData ) ;
						break;
				}
				if( buffer!=NULL ) { fprintf(fp,"%s\r\n",buffer) ; free( buffer ) ; }
			}
		}
	}
	fclose( fp );
	free( fullpath ) ;
	free( fullkey ) ;
	return 0 ;
}

	
/******
Supprimer toute trace de KiTTY dans le registre.
Ecrire et exécuter un fichier utf-8 .reg contenant les lignes:

Windows Registry Editor Version 5.00

[-HKEY_CURRENT_USER\Software\9bis.com\KiTTY]

[-HKEY_CLASSES_ROOT\telnet]

[-HKEY_CLASSES_ROOT\ssh]

[-HKEY_CLASSES_ROOT\putty]

[-HKEY_CLASSES_ROOT\kitty.connect.1]

[-HKEY_CLASSES_ROOT\.ktx]

******/
