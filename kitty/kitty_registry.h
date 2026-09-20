/*
 * kitty_registry.h - the declarations for kitty_registry.c: the registry
 * wrappers (read a value, test or create a key, delete a value or a tree,
 * copy or export a tree), the one-time hive migrations and repairs, and the
 * shell integration (URL protocol handlers and the file association) with
 * the console-or-box reporting helper they share.
 */
#ifndef KITTY_REGISTRY
#define KITTY_REGISTRY

#include <stdlib.h>
#include <stdio.h>
#include <windows.h>

#ifndef MAX_KEY_LENGTH 
#define MAX_KEY_LENGTH 255
#endif
#ifndef MAX_VALUE_NAME
#define MAX_VALUE_NAME 16383
#endif

/* Longest registry value GetValueData reads. It can write cstMaxRegLength
 * data bytes plus a forced NUL into rValue, so destination buffers must be
 * at least cstMaxRegLength+2 bytes -- a bare [cstMaxRegLength] is one short. */
#define cstMaxRegLength 1024

char * GetValueDataN(HKEY hkTopKey, const char * lpSubKey, const char * lpValueName, char * rValue, size_t rsize) ;
char * GetValueData(HKEY hkTopKey, const char * lpSubKey, const char * lpValueName, char * rValue) ; /* compat: rValue >= cstMaxRegLength+2 bytes; prefer GetValueDataN */

// Extension for session files in portable mode (may be ktx)
extern char FileExtension[15] ;

// Test whether a key exists
int RegTestKey( HKEY hMainKey, LPCTSTR lpSubKey ) ;

// Return the number of subkeys
int RegCountKey( HKEY hMainKey, LPCTSTR lpSubKey ) ;

// Test whether a key or a value exists and create it otherwise
// KiTTY: 1 = written, 0 = key/value could not be written (HKCR needs elevation)
int RegTestOrCreate( HKEY hMainKey, LPCTSTR lpSubKey, LPCTSTR name, LPCTSTR value ) ;

// Test whether a key or a DWORD value exists and create it otherwise
int RegTestOrCreateDWORD( HKEY hMainKey, LPCTSTR lpSubKey, LPCTSTR name, DWORD value ) ;

// Set a value in every session (oldvalue==NULL) or only where it holds oldvalue
void RegUpdateAllSessions( HKEY hMainKey, LPCTSTR lpSubKey, LPCTSTR name, LPCTSTR oldvalue, LPCTSTR value  ) ;

// Export a whole registry key
void QuerySubKey( HKEY hMainKey, LPCTSTR lpSubKey, FILE * fp_out, char * text  ) ;

// Delete a registry key value
BOOL RegDelValue (HKEY hKeyRoot, LPCTSTR lpSubKey, LPCTSTR lpValue ) ;

// Delete a registry key and its subkeys
BOOL RegDelTree (HKEY hKeyRoot, LPCTSTR lpSubKey) ;

// Copy one registry key onto another
void kitty_RegCopyTree( HKEY hMainKey, LPCTSTR lpSubKey, LPCTSTR lpDestKey ) ;
// One-time migration of the old hive (9bis.com\KiTTY) to kapper.net\KiTTY
void MigrateOldKittyHive( void ) ;

// One-time repair of the ShiftedArrowKeys default (SHARROW_APPLICATION was
// saved by mistake)
void RepairSharrowDefaults( void ) ;

// One-time migration: SCPAutoPwd (retired) -> OSC7CwdTracking, then the
// value is deleted
void MigrateScpAutoPwd( void ) ;

// Clean the PuTTY key: remove the keys and values specific to KiTTY
BOOL RegCleanPuTTY( void ) ;

// KiTTY: answer a command-line switch at the prompt that issued it (attaching
// to the parent console); a message box only when there is no console at all
void KittyCliReport( const char *title, const char *text, int warn ) ;

// Create the SSH handler
// KiTTY: force = also take over protocols another program already handles;
// peruser = register under HKCU even when this is the machine-wide install;
// assume_yes = skip the portable build's "write to the registry?" question;
// withputty = also register putty://, which is another project's name
void CreateSSHHandler( int force, int peruser, int assume_yes, int withputty ) ;

// KiTTY: -sshhandler -uninstall. Removes only handlers pointing at a KiTTY,
// exporting each key first so the removal can be undone
void RemoveSSHHandler( void ) ;

// Create the *.ktx file association
// KiTTY: same options as CreateSSHHandler - force = take the extension over
// from whatever opens it now (exported first), peruser = HKCU without asking
// for administrator rights, assume_yes = skip the portable copy's question
void CreateFileAssoc( int force, int peruser, int assume_yes ) ;

// KiTTY++ Settings > System: five state lines (telnet://, ssh://, kitty://,
// putty://, the session-file extension) - "this KiTTY++", another KiTTY, the
// program that has it, or "not registered". Returns how many of the four
// required ones point at this exe.
int kitty_shell_integration_state( char lines[5][256] ) ;
// The leaf's buttons: register (leave other programs' entries alone) or take
// over (force); reports in boxes.
void kitty_shell_integration_register( int force ) ;
void kitty_shell_integration_unregister( void ) ;

// KiTTY: -fileassoc -uninstall. Removes the association only while it is ours
void RemoveFileAssoc( void ) ;

// Check that the KiTTY key exists, otherwise copy it from PuTTY
void TestRegKeyOrCopyFromPuTTY( HKEY hMainKey, char * KeyName ) ;

void InitRegistryAllSessions( HKEY hMainKey, LPCTSTR lpSubKey, char * SubKeyName, char * filename, char * text ) ;

// Set values in every session from the contents of a kitty.ses.updt file
void InitAllSessions( HKEY hMainKey, LPCTSTR lpSubKey, char * SubKeyName, char * filename ) ;

void mungestr( const char *in, char *out ) ;

#endif
