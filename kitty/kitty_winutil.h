/*
 * kitty_winutil.h - the declarations for kitty_winutil.c: window
 * transparency and the OS version, the file and folder pickers, printing,
 * the clipboard, launching a process, the event-log printf and the
 * absolute-path test.
 */
#ifndef KITTY_WINUTIL_H
#define KITTY_WINUTIL_H

#include <stdlib.h>
#include <stdio.h>
#include "putty.h"
#include <windows.h>

void SetTransparency( HWND hwnd, int value ) ;
void GetOSInfo( char * version ) ;
BOOL IsWow64(void) ; // Test whether we are on 64-bit Windows
int OpenFileName( HWND hFrame, char * filename, char * Title, char * Filter ) ;
/* The same picker opened in a given folder (NULL or empty = wherever
 * Windows would open it). */
int OpenFileNameFrom( HWND hFrame, char * filename, char * Title, char * Filter, const char * initialdir ) ;
int OpenDirName( HWND hFrame, char * dirname ) ;
/* Same picker, opened on `initial` (when that folder exists) with `title`
 * (NULL = the default caption). */
int OpenDirNameFrom( HWND hFrame, char * dirname, const char * initial, const char * title ) ;
int SaveFileName( HWND hFrame, char * filename, char * Title, char * Filter ) ;
int SaveFileNameFrom( HWND hFrame, char * filename, char * Title, char * Filter, const char * initialdir ) ;

// Send to the printer
int PrintText( const char * Text ) ;

// Print the text in the notepad
void ManagePrint( HWND hwnd ) ;

// Put a text into the clipboard
int SetTextToClipboard( const char * buf ) ;

// Run a command
void RunCommand( HWND hwnd, const char * cmd ) ;

// Start the built-in editor
void RunPuttyEd( HWND hwnd, char * filename ) ;

// Display a message in the event log
void debug_logevent( const char *fmt, ... ) ;

// Test whether a path is absolute
bool IsPathAbsolute( const char * path ) ;

extern int PrintCharSize;
extern int PrintMaxCharPerLine;
extern int PrintMaxLinePerPage;

#endif
