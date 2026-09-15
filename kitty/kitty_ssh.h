/*
 * kitty_ssh.h - the declarations for kitty_ssh.c: port knocking before a
 * connection (MOD_PORTKNOCKING). knock() hits one TCP or UDP port;
 * ManagePortKnocking() walks a whole sequence written as a string.
 */
#ifndef KITTY_SSH
#define KITTY_SSH

#ifdef MOD_PORTKNOCKING
#define PROTO_TCP 1
#define PROTO_UDP 2
int knock( char *hostname, unsigned short port, unsigned short proto) ;
int ManagePortKnocking( char* host, char *portstr ) ;
#endif

#endif
