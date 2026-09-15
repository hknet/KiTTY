/*
 * kitty_broadcast.h - the declarations for kitty_broadcast.c: the count of
 * KiTTY windows, sending a command or keystrokes to every one, the broadcast
 * gate with its group and send key, the auto-command, the "same size" resize.
 */
#ifndef KITTY_BROADCAST_H
#define KITTY_BROADCAST_H

#include "putty.h"
#include <windows.h>

// Broadcast gate: [KiTTY] sendcmdmode=yes|no starts windows armed or not, and
// [KiTTY] sendcmdgroup (derived when unset) decides which KiTTYs hear each
// other. Accident prevention, not a security boundary - see kitty_broadcast.c.
void kitty_broadcast_set_enabled( int on ) ;
int  kitty_broadcast_default( void ) ;
const char *kitty_broadcast_group( void ) ;
int kitty_broadcast_group_from_ini( void ) ;   // key came from kitty.ini, not derived
void kitty_broadcast_set_group( const char *k ) ;      // the config box wrote sendcmdgroup; "" = derive again
void kitty_broadcast_set_send_key( const char *k ) ;   // -sendcmdkey override
const char *kitty_broadcast_send_key_override( void ) ; // the raw override, "" when none
const char *kitty_broadcast_send_key( void ) ;         // key a broadcast is SENT with

void routine_server( void * st ) ;
void SendKeyboard( HWND hwnd, const char * buffer ) ;
void SendKeyboardPlus( HWND hwnd, const char * st );
void SendAutoCommand( HWND hwnd, const char * cmd ) ;
int SendCommandAllWindows( HWND hwnd, char * cmd ) ;
// The send console's form: include_self=1 also reaches the terminal this
// process owns (MainHwnd), which /command deliberately skips.
int SendCommandAllWindowsEx( HWND hwnd, char * cmd, int include_self ) ;
int ResizeWinList( HWND hwnd, int width, int height ) ;
// How many KiTTY windows (terminals and configuration boxes) are open.
int WindowsCount( HWND hwnd ) ;

#endif /* KITTY_BROADCAST_H */
