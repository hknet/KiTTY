/*
 * kitty_tools.h - the declarations for kitty_tools.c, the small
 * general-purpose helpers the KiTTY additions reuse: string editing, tests
 * and sizes for files and directories, a NULL-terminated string list,
 * setting an environment variable, and creating a directory path.
 */
#ifndef KITTY_TOOLS
#define KITTY_TOOLS

#include <stdlib.h>
#include <stdio.h>
#include <windows.h>
#include <sys/stat.h>
#include <ctype.h>

#ifndef MAX_VALUE_NAME
#define MAX_VALUE_NAME 16383
#endif

// String-handling procedures
#ifndef stricmp	/* platform.h may #define stricmp _stricmp (CRT); don't redeclare */
int stricmp(const char *s1, const char *s2) ;
#endif

/* Remove, in place, the trailing characters belonging to `set` (right-trim).
   Replaces the while(strlen...) loops copied all over the place; safe on an
   empty string (never indexes s[-1]). Returns s. */
char *str_rtrim( char *s, const char *set ) ;

/* Insert one string into another */
int insert( char * ch, const char * c, const int ipos ) ;

/* Delete part of a string */
int del( char * ch, const int start, const int length ) ;

/* Find the position of one string inside another string */
int poss( const char * c, const char * ch ) ;

// Test whether a file exists
int existfile( const char * filename ) ;

// Test whether a directory exists
int existdirectory( const char * filename ) ;

/* Return the size of a file */
long filesize( const char * filename ) ;

// Remove doubled backslashes
void DelDoubleBackSlash( char * st ) ;
	
// Add a string to a list of strings
int StringList_Add( char **list, const char *str ) ;

// Remove a string from a list of strings
void StringList_Del( char **list, const char * name ) ;

// Reorder a list of strings by moving the selected one up one place
void StringList_Up( char **list, const char * name ) ;

// Set an environment variable
int putenv (const char *string) ;
int set_env( char * name, char * value ) ;

// Create a directory path recursively (dir1 / dir2 / ...)
int MakeDir( const char * directory ) ;

#endif
