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
/* The folder picker (folder = KITTY_PICK_FOLDER), the open-file picker
 * (KITTY_PICK_OPEN) or the Save-As picker (KITTY_PICK_SAVE: `initial` a
 * folder, or NULL for the classic box with its .ktx default extension) on a
 * thread of its own, so the session keeps running: done(ctx, path or NULL)
 * on the calling thread. A token, or NULL = not started (pick synchronously
 * instead). kitty_pick_cancel(token): done is not called any more. */
#define KITTY_PICK_OPEN    0
#define KITTY_PICK_FOLDER  1
#define KITTY_PICK_SAVE    2
void *kitty_pick_async( HWND owner, int folder, const char *initial, const char *title,
                        const char *filter, void (*done)( void *ctx, const char *path ), void *ctx ) ;
void kitty_pick_cancel( void *token ) ;
/* fn(ctx) on a thread of its own (COM set up for it) - a blocking call such
 * as a common dialog, so the session keeps running - then done(ctx) on the
 * calling thread. 1 = started (done always runs); 0 = not started (make the
 * call here instead). Call from the terminal's thread. */
int kitty_call_async( void (*fn)( void *ctx ), void (*done)( void *ctx ), void *ctx ) ;
int SaveFileName( HWND hFrame, char * filename, char * Title, char * Filter ) ;
int SaveFileNameFrom( HWND hFrame, char * filename, char * Title, char * Filter, const char * initialdir ) ;

// Send to the printer
int PrintText( const char * Text ) ;

// Print the text in the notepad
void ManagePrint( HWND hwnd ) ;

// Put a text into the clipboard
int SetTextToClipboard( const char * buf ) ;

// Run a command
int RunCommand( HWND hwnd, const char * cmd ) ;   /* 1 started, 0 not (the reason was shown) */

/* Start a process that inherits exactly h[0..nh-1] (hand-over mappings) and no
 * other handle; CreateProcessA's result and GetLastError(). */
BOOL kitty_create_process_with_handles( const char * app, char * cmdline, DWORD flags,
                                        const char * dir, STARTUPINFOA * si,
                                        PROCESS_INFORMATION * pi,
                                        const HANDLE * h, int nh ) ;

// Start the built-in editor
void RunPuttyEd( HWND hwnd, char * filename ) ;

// Display a message in the event log
void debug_logevent( const char *fmt, ... ) ;

// Test whether a path is absolute
bool IsPathAbsolute( const char * path ) ;

/* Expand %VAR% Windows environment variables in a stored plain-string path
 * (download/upload folder, PSCPPath/WinSCPPath/FileZillaPath, rzcommand/
 * szcommand, sav=), right
 * before it is used as a real Win32 path (cyd01/KiTTY#472). See the
 * implementation comment in kitty_winutil.c. Caller frees with free(). */
char *kitty_expand_env_dup(const char *in);

extern int PrintCharSize;
extern int PrintMaxCharPerLine;
extern int PrintMaxLinePerPage;

#endif
