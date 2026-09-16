/*
 * kitty_commands.h - the entry point of kitty_commands.c, the send-text-box
 * internal commands ("/commands"): the dispatcher the input box hands a line
 * to. 1 = it was a command and has been handled, 0 = plain text for the host.
 */
#ifndef KITTY_COMMANDS_H
#define KITTY_COMMANDS_H

#include <windows.h>

int InternalCommand( HWND hwnd, char * st ) ;

#endif /* KITTY_COMMANDS_H */
