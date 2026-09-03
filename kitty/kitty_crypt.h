#ifndef KITTYCRYPT_H
#define KITTYCRYPT_H

#include "nbcrypt.h"
#include <windows.h>

int cryptstring( const int mode, char * st, const char * key ) ;
int decryptstring( const int mode, char * st, const char * key ) ;
int cryptpassword( const int mode, char * password, const char * host, const char * termtype ) ;
int decryptpassword( const int mode, char * password, const char * host, const char * termtype ) ;

void MASKPASS( const int mode, char * password ) ;

int GetUserPassSSHNoSave(void) ;

#endif
