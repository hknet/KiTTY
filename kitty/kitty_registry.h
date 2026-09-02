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

char * GetValueDataN(HKEY hkTopKey, char * lpSubKey, const char * lpValueName, char * rValue, size_t rsize) ;
char * GetValueData(HKEY hkTopKey, char * lpSubKey, const char * lpValueName, char * rValue) ; /* compat: rValue >= cstMaxRegLength+2 octets; preferer GetValueDataN */

// Extention pour les fichiers de session en mode portable (peut être ktx)
extern char FileExtension[15] ;

// Teste l'existance d'une clé
int RegTestKey( HKEY hMainKey, LPCTSTR lpSubKey ) ;

// Retourne le nombre de sous-keys
int RegCountKey( HKEY hMainKey, LPCTSTR lpSubKey ) ;

// Teste l'existance d'une clé ou bien d'une valeur et la crée sinon
// KiTTY: 1 = written, 0 = key/value could not be written (HKCR needs elevation)
int RegTestOrCreate( HKEY hMainKey, LPCTSTR lpSubKey, LPCTSTR name, LPCTSTR value ) ;

// Test l'existance d'une clé ou bien d'une valeur DWORD et la crée sinon
int RegTestOrCreateDWORD( HKEY hMainKey, LPCTSTR lpSubKey, LPCTSTR name, DWORD value ) ;

// Initialise toutes les sessions avec une valeur (si oldvalue==NULL) ou uniquement celles qui ont la valeur oldvalue
void RegUpdateAllSessions( HKEY hMainKey, LPCTSTR lpSubKey, LPCTSTR name, LPCTSTR oldvalue, LPCTSTR value  ) ;

// Exporte toute une cle de registre
void QuerySubKey( HKEY hMainKey, LPCTSTR lpSubKey, FILE * fp_out, char * text  ) ;

// Détruit une valeur de clé de registre 
BOOL RegDelValue (HKEY hKeyRoot, LPTSTR lpSubKey, LPTSTR lpValue ) ;

// Detruit une clé de registre et ses sous-clé
BOOL RegDelTree (HKEY hKeyRoot, LPCTSTR lpSubKey) ;

// Copie une clé de registre vers une autre
void kitty_RegCopyTree( HKEY hMainKey, LPCTSTR lpSubKey, LPCTSTR lpDestKey ) ;
// Migration ponctuelle de l'ancienne ruche (9bis.com\KiTTY) vers kapper.net\KiTTY
void MigrateOldKittyHive( void ) ;

// Réparation ponctuelle du défaut ShiftedArrowKeys (SHARROW_APPLICATION persisté par erreur)
void RepairSharrowDefaults( void ) ;

// Migration ponctuelle: SCPAutoPwd (retiré) -> OSC7CwdTracking, puis suppression de la clé
void MigrateScpAutoPwd( void ) ;

// Nettoie la clé de PuTTY pour enlever les clés et valeurs spécifique à KiTTY
BOOL RegCleanPuTTY( void ) ;

// KiTTY: answer a command-line switch at the prompt that issued it (attaching
// to the parent console); a message box only when there is no console at all
void KittyCliReport( const char *title, const char *text, int warn ) ;

// Creation du SSH Handler
// KiTTY: force = also take over protocols another program already handles;
// peruser = register under HKCU even when this is the machine-wide install;
// assume_yes = skip the portable build's "write to the registry?" question;
// withputty = also register putty://, which is another project's name
void CreateSSHHandler( int force, int peruser, int assume_yes, int withputty ) ;

// KiTTY: -sshhandler -uninstall. Removes only handlers pointing at a KiTTY,
// exporting each key first so the removal can be undone
void RemoveSSHHandler( void ) ;

// Creation de l'association de fichiers *.ktx
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

// Vérifie l'existance de la clé de KiTTY sinon la copie depuis PuTTY
void TestRegKeyOrCopyFromPuTTY( HKEY hMainKey, char * KeyName ) ;

void InitRegistryAllSessions( HKEY hMainKey, LPCTSTR lpSubKey, char * SubKeyName, char * filename, char * text ) ;

// Permet d'initialiser toutes les sessions avec des valeurs contenu dans un fichier kitty.ses.updt
void InitAllSessions( HKEY hMainKey, LPCTSTR lpSubKey, char * SubKeyName, char * filename ) ;

// Exporte une clé de registre dans un fichier au format PuTTY (  name\value\ )
void mungestr( const char *in, char *out ) ;
int ExportSubKeyToFile( HKEY hkey, const char *subkey, const char *keyname, const char *maindir, const char *subdir ) ;

#endif
