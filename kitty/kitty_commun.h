/*
 * kitty_commun.h - the declarations for kitty_commun.c: the global flags and
 * helpers shared by every program in the suite (kitty, pscp, psftp, plink,
 * pageant). Save mode and configuration directory, the feature and
 * compatibility flags read from kitty.ini, folder-name cleaning, the
 * light parameter reader used before the full store is up, the %XY name
 * escaping, and access to the session password.
 */
#ifndef KITTY_COMMUN
#define KITTY_COMMUN

#include <stdlib.h>
#include <stdio.h>
#include "putty.h"

#include "platform.h"
#include <windows.h>

// Flag enabling extra code paths that put more information into kitty.dmp
extern int debug_flag ;

// Flag to fall back to plain PuTTY mode
int GetPuttyFlag(void) ;
void SetPuttyFlag( const int flag ) ;

// Flag: show connection errors as modal boxes instead of inline in the terminal (cyd01/KiTTY #548)
int GetModalErrorsFlag(void) ;
void SetModalErrorsFlag( const int flag ) ;

// Inline-first security prompts (successor to cyd01/KiTTY #548): each defaults to
// 1 = classic modal box; set the [KiTTY] key to no for an OpenSSH-style in-terminal prompt.
int GetModalNewHostKeyConfirmationFlag(void) ;
void SetModalNewHostKeyConfirmationFlag( const int flag ) ;
int GetModalChangedHostKeyConfirmationFlag(void) ;
void SetModalChangedHostKeyConfirmationFlag( const int flag ) ;
int GetModalWeakKeyConfirmationFlag(void) ;
void SetModalWeakKeyConfirmationFlag( const int flag ) ;

// Flag for running in "portable" mode (settings kept in files)
int GetIniFileFlag(void) ;

// Flag disabling the automatic saving of the login details (user/password)
// on SSH connection
// extern int UserPassSSHNoSave ;
int GetUserPassSSHNoSave(void) ;
void SetUserPassSSHNoSave( const int flag ) ;

// Flag preventing files from being written (default settings, jump list ...)
// [KiTTY] readonly=no
int GetReadOnlyFlag(void) ;
void SetReadOnlyFlag( const int flag ) ;

// Flag to display the background image
//extern int BackgroundImageFlag ;
int GetBackgroundImageFlag(void) ;
void SetBackgroundImageFlag( const int flag ) ;

// To remove the salt from the password encryption
int GetCryptSaltFlag() ;
void SetCryptSaltFlag( int flag ) ;

#ifdef MOD_ZMODEM
// Flag to disable the ZMODEM functions
int GetZModemFlag(void) ;
void SetZModemFlag( const int flag ) ;
#endif

// Directory the configuration is saved into (savemode=dir)
extern char * ConfigDirectory ;

char * GetConfigDirectory( void ) ;

#ifndef stricmp	/* platform.h may #define stricmp _stricmp (CRT); don't redeclare */
int stricmp(const char *s1, const char *s2) ;
#endif
char * GetValueDataN(HKEY hkTopKey, char * lpSubKey, const char * lpValueName, char * rValue, size_t rsize) ;
char * GetValueData(HKEY hkTopKey, char * lpSubKey, const char * lpValueName, char * rValue) ; /* compat: rValue >= cstMaxRegLength+2 bytes; prefer GetValueDataN */
int readINI( const char * filename, const char * section, const char * key, char * pStr, size_t pStrSize) ;
char * SetSessPath( const char * dec ) ;

// Clean folder names: replace "/" with "\" and " \ " with " \"
void CleanFolderName( char * folder ) ;

// Delete a directory tree
void DelDir( const char * directory ) ;

// Read a parameter either from the configuration file or from the registry
int ReadParameterLightN( const char * key, const char * name, char * value, size_t size ) ;
int ReadParameterLight( const char * key, const char * name, char * value ) ; /* compat: value >= 4096 bytes; prefer ReadParameterLightN */

/* test if we are in portable mode by looking for putty.ini or kitty.ini in running directory */
int LoadParametersLight( void ) ;

// Flag saying whether we are connected
extern int is_backend_connected ;
#ifdef MOD_RECONNECT
extern int is_backend_first_connected ;
#endif

void SetSSHConnected( int flag ) ;

//PVOID WINAPI SecureZeroMemory( PVOID ptr, SIZE_T cnt) ;

// Functions escaping strings with %XY sequences
void mungestr( const char *in, char *out ) ;
void unmungestr( const char *in, char *out, int outlen ) ;

/* The session password. GetPasswordInConfig fills the caller's buffer, which
 * must hold KITTY_PW_MAX+1 bytes; IsPasswordInConf answers its length. Both
 * read through kitty_pwmem.c - the running value is wrapped in memory. */
void MASKPASS( const int mode, char * password ) ;
void GetPasswordInConfig( char * p ) ;
int IsPasswordInConf(void) ;

int _rmdir(const char *) ;

// Extension for session files in portable mode (may be ktx)
extern char FileExtension[15] ;

// Current folder for portable mode
extern char CurrentFolder[1024] ;

//int DebugAddPassword( const char*fct, const char*pwd ) ;
#endif
