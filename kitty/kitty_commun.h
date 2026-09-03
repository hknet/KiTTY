#ifndef KITTY_COMMUN
#define KITTY_COMMUN

#include <stdlib.h>
#include <stdio.h>
#include "putty.h"

#include "platform.h"
#include <windows.h>

// Flag permettant d'activer l'acces a du code particulier permettant d'avoir plus d'info dans le kitty.dmp
extern int debug_flag ;

// Flag pour repasser en mode Putty basic
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

// Flag pour le fonctionnement en mode "portable" (gestion par fichiers)
int GetIniFileFlag(void) ;

// Flag permettant de desactiver la sauvegarde automatique des informations de connexion (user/password) Ã  la connexion SSH
// extern int UserPassSSHNoSave ;
int GetUserPassSSHNoSave(void) ;
void SetUserPassSSHNoSave( const int flag ) ;

// Flag pour empêcher l'écriture des fichiers (default settings, jump file list ...)
// [KiTTY] readonly=no
int GetReadOnlyFlag(void) ;
void SetReadOnlyFlag( const int flag ) ;

// Flag pour afficher l'image de fond
//extern int BackgroundImageFlag ;
int GetBackgroundImageFlag(void) ;
void SetBackgroundImageFlag( const int flag ) ;

// Pour supprimer le sel dans le cryptage du mot de passe
int GetCryptSaltFlag() ;
void SetCryptSaltFlag( int flag ) ;

#ifdef MOD_ZMODEM
// Flag pour inhiber les fonctions ZMODEM
int GetZModemFlag(void) ;
void SetZModemFlag( const int flag ) ;
#endif

// Répertoire de sauvegarde de la configuration (savemode=dir)
extern char * ConfigDirectory ;

char * GetConfigDirectory( void ) ;

#ifndef stricmp	/* platform.h may #define stricmp _stricmp (CRT); don't redeclare */
int stricmp(const char *s1, const char *s2) ;
#endif
char * GetValueDataN(HKEY hkTopKey, char * lpSubKey, const char * lpValueName, char * rValue, size_t rsize) ;
char * GetValueData(HKEY hkTopKey, char * lpSubKey, const char * lpValueName, char * rValue) ; /* compat: rValue >= cstMaxRegLength+2 octets; preferer GetValueDataN */
int readINI( const char * filename, const char * section, const char * key, char * pStr, size_t pStrSize) ;
char * SetSessPath( const char * dec ) ;

// Nettoie les noms de folder en remplaçant les "/" par des "\" et les " \ " par des " \"
void CleanFolderName( char * folder ) ;

// Supprime une arborescence
void DelDir( const char * directory ) ;

// Lit un parametre soit dans le fichier de configuration, soit dans le registre
int ReadParameterLightN( const char * key, const char * name, char * value, size_t size ) ;
int ReadParameterLight( const char * key, const char * name, char * value ) ; /* compat: value >= 4096 octets; preferer ReadParameterLightN */

/* test if we are in portable mode by looking for putty.ini or kitty.ini in running directory */
int LoadParametersLight( void ) ;

// Positionne un flag permettant de determiner si on est connecte
extern int is_backend_connected ;
#ifdef MOD_RECONNECT
extern int is_backend_first_connected ;
#endif

void SetSSHConnected( int flag ) ;

//PVOID WINAPI SecureZeroMemory( PVOID ptr, SIZE_T cnt) ;

// Fonctions permettant de formatter les chaînes de caractères avec %XY
void mungestr( const char *in, char *out ) ;
void unmungestr( const char *in, char *out, int outlen ) ;

// Fonctions de gestion du mot de passe
void MASKPASS( const int mode, char * password ) ;
void GetPasswordInConfig( char * p ) ;
int IsPasswordInConf(void) ;

int _rmdir(const char *) ;

// Extention pour les fichiers de session en mode portable (peut être ktx)
extern char FileExtension[15] ;

// Répertoire courant pourle mode portable
extern char CurrentFolder[1024] ;

//int DebugAddPassword( const char*fct, const char*pwd ) ;
#endif
