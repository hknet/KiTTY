/*
 * kitty_portfwd.h - the declarations for kitty_portfwd.c, the port-forward
 * display: the listening state of one port and the whole list.
 */
#ifndef KITTY_PORTFWD_H
#define KITTY_PORTFWD_H

#include "putty.h"
#include <windows.h>

int GetPortFwdState( const int port, const DWORD pid ) ;
int ShowPortfwd( HWND hwnd, Conf * conf ) ;

#endif /* KITTY_PORTFWD_H */
