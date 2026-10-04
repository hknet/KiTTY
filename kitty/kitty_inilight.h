/*
 * kitty_inilight.h: interface of the light kitty.ini/putty.ini resolver for
 * satellite binaries (see kitty_inilight.c for the search order and the
 * authoritative-store rule).
 */

#ifndef KITTY_INILIGHT_H
#define KITTY_INILIGHT_H

const char *kitty_inilight_file(void);
int kitty_inilight_portable(void);   /* portable storage layout beside the exe? */
int kitty_inilight_registry_authoritative(void);
int kitty_inilight_read(const char *section, const char *key,
                        char *value, int len);
int kitty_inilight_write(const char *section, const char *key,
                         const char *value);
/* 1 when the sessions live in the folder store (savemode=dir), with its
 * directory and session file suffix; 0 when they are in the registry. */
int kitty_inilight_folder_store(char *dir, int dirlen, char *suffix, int suflen);

#endif
