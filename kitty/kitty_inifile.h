/*
 * kitty_inifile.h: interface of the kitty.ini/putty.ini resolver for
 * satellite binaries (see kitty_inifile.c for the search order and the
 * authoritative-store rule).
 */

#ifndef KITTY_INIFILE_H
#define KITTY_INIFILE_H

const char *kitty_inifile_file(void);
int kitty_inifile_portable(void);   /* portable storage layout beside the exe? */
int kitty_inifile_registry_authoritative(void);
int kitty_inifile_read(const char *section, const char *key,
                        char *value, int len);
int kitty_inifile_write(const char *section, const char *key,
                         const char *value);
/* 1 when the sessions live in the folder store (savemode=dir), with its
 * directory and session file suffix; 0 when they are in the registry. */
int kitty_inifile_folder_store(char *dir, int dirlen, char *suffix, int suflen);
/* That store's host-key folder (empty = its own SshHostKeys) and ending. */
int kitty_inifile_hostkey_store(char *dir, int dirlen, char *suffix, int suflen);

#endif
