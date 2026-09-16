/*
 * kitty_startup.h - the declarations for kitty_startup.c: the KiTTY-specific
 * initialisation WinMain runs first, and the netdebug build's startup
 * checkpoint logger.
 */
#ifndef KITTY_STARTUP_H
#define KITTY_STARTUP_H

void InitWinMain( void ) ;
#ifdef MOD_NETDEBUG
void kitty_netdbg_ts( const char *msg ) ;
#endif

#endif /* KITTY_STARTUP_H */
