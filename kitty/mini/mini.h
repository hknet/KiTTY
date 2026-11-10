
/*
 * The kitty.ini helpers (mini.c): readINI / writeINI / delINI read or change
 * one key by file, section and key name.
 */
#ifndef __MINI
#define __MINI

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>

#ifdef WIN32
#include <io.h>
#include <sys/locking.h>
#else
#include <sys/file.h>
#define _locking lockf
#define LK_LOCK F_LOCK
#define LK_UNLCK F_ULOCK
#endif


int readINI( const char * filename, const char * section, const char * key, char * pStr, size_t pStrSize) ;

int writeINI( const char * filename, const char * section, const char * key, const char * value ) ;

int delINI( const char * filename, const char * section, const char * key ) ;

#endif
