/*
 * kitty_int.h - what kitty.c and the files split off it share and nothing
 * else needs. kitty.h is the public face of the module; this header carries
 * the file-scope state the split files still reach into directly.
 */
#ifndef KITTY_INT_H
#define KITTY_INT_H

#include <windows.h>

/* The terminal window of this process (kitty.c); GetMainHwnd() is the
 * accessor the rest of the suite uses. */
extern HWND MainHwnd ;

/* The store's file paths and flags (kitty.c): kitty.ini and kitty.sav as
 * resolved at startup, "conf=no" (create neither), and the registry's
 * configuration-password value the backup retires. */
extern char * KittyIniFile ;
extern char * KittySavFile ;
extern int NoKittyFileFlag ;
extern char PasswordConf[] ;

/* kitty_regbackup.c, for the startup sequence: the newest .sav to offer at a
 * first start, and the two clean-outs of retired features. */
int sav_find_for_restore( const char *savfile, char *out, size_t outlen ) ;
void RetireConfigPasswordLeftovers( void ) ;
void RetireCountUpLeftovers( void ) ;

/* The icon library and the icon file kitty.ini names (kitty.c), resolved
 * by the startup sequence. */
extern HINSTANCE hInstIcons ;
extern char * IconFile ;

/* kitty.c helpers the startup sequence calls once. */
void CountUp( void ) ;
void GetSaveMode( void ) ;
void SetConfigDirectory( const char * Directory ) ;
void GetInitialDirectory( char * InitialDirectory ) ;

/* The window flags kitty.c owns and the /commands console switches at run
 * time (kitty.c); the Get/Set accessors in kitty.h are the public way. */
extern int TransparencyFlag ;
extern int ShortcutsFlag ;
extern int MouseShortcutsFlag ;
extern int SizeFlag ;
extern int TitleBarFlag ;
extern int WinrolFlag ;
/* Defined in kitty_commun.c; kitty.h leaves it undeclared on purpose
 * (GetDirectoryBrowseFlag() is the accessor). */
extern int DirectoryBrowseFlag ;

#endif /* KITTY_INT_H */
