/*
 * kitty_commands.h - the entry point of kitty_commands.c, the send-text-box
 * internal commands ("/commands"): the dispatcher the input box hands a line
 * to. 1 = it was a command and has been handled, 0 = plain text for the host.
 */
#ifndef KITTY_COMMANDS_H
#define KITTY_COMMANDS_H

#include <windows.h>

int InternalCommand( HWND hwnd, char * st ) ;

/* What the last command did, for the command box's result line: a full
 * sentence, a file always with its full path ("" before any command). */
const char * kitty_cmd_result( void ) ;

/* "kitty.exe -h" without a console: the option list in the scrolling
 * command-list window. 0 = the window could not be made. */
int kitty_cli_help_box( const char * caption, const char * text ) ;

#endif /* KITTY_COMMANDS_H */
