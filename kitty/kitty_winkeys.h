/*
 * kitty_winkeys.h - Windows' own keys, read and written in one place.
 *
 * What KiTTY++ keeps in the registry goes through the settings module
 * (kitty_globalsettings.h) and the session store. The keys here belong to
 * Windows or to other programs: file and URL associations, Default Apps,
 * the Run entry, HKCU\Environment, the light or dark theme, Event Log
 * sources, serial devices, the Attachments policy, the wallpaper. Every
 * raw registry call for them is in kitty_winkeys.c (tools/raw-registry.allow
 * lists that file alone).
 *
 * root: HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE, HKEY_CLASSES_ROOT. view:
 * 0, or KEY_WOW64_64KEY / KEY_WOW64_32KEY for a 32-bit program that must
 * see the 64-bit keys. Strings are malloc'd (free() them).
 */
#ifndef KITTY_WINKEYS_H
#define KITTY_WINKEYS_H

#include <windows.h>

/* A REG_SZ or REG_EXPAND_SZ value in full (not expanded), NULL when the key
 * or the value is absent or of another kind. type: its kind (may be NULL). */
char *kitty_winkey_str_dup(HKEY root, const char *path, const char *name,
                           REGSAM view, DWORD *type);
/* A REG_DWORD value: 1 and *out, 0 when absent or of another kind. */
int kitty_winkey_dword(HKEY root, const char *path, const char *name,
                       REGSAM view, DWORD *out);
/* Any value in full (free() it) with its kind and length. NULL when absent. */
void *kitty_winkey_value_dup(HKEY root, const char *path, const char *name,
                             REGSAM view, DWORD *type, DWORD *len);
/* The same for a key handle another API opened (SetupDiOpenDevRegKey). */
void *kitty_winkey_handle_value_dup(HKEY key, const char *name, DWORD *type,
                                    DWORD *len);
/* 1 when the key exists. */
int kitty_winkey_exists(HKEY root, const char *path, REGSAM view);
/* A value written (the key created when missing): 1 done, 0 not, the
 * reason in GetLastError(). */
int kitty_winkey_set(HKEY root, const char *path, const char *name,
                     REGSAM view, DWORD type, const void *data, DWORD len);
int kitty_winkey_set_str(HKEY root, const char *path, const char *name,
                         REGSAM view, const char *value);
int kitty_winkey_set_dword(HKEY root, const char *path, const char *name,
                           REGSAM view, DWORD value);
/* A value removed: 1 when it is gone (removed or never there), 0 with the
 * reason in GetLastError(). */
int kitty_winkey_delete_value(HKEY root, const char *path, const char *name,
                              REGSAM view);
/* A key removed with everything below it: 1 removed, -1 there was none,
 * 0 not removed (the reason in GetLastError()). */
int kitty_winkey_delete_tree(HKEY root, const char *path, REGSAM view);
/* The key created when missing, no value written: 1 when it exists after. */
int kitty_winkey_create(HKEY root, const char *path, REGSAM view);
/* 1 when this process may create keys below path (nothing is created):
 * "may we write to HKLM?" */
int kitty_winkey_may_create(HKEY root, const char *path, REGSAM view);
/* The counts of a key's subkeys and values: 1 when the key was opened. */
int kitty_winkey_counts(HKEY root, const char *path, REGSAM view,
                        DWORD *subkeys, DWORD *values);
/* Every value name of the key through fn (stop with 0), or every subkey
 * name. 1 when the key was opened. */
int kitty_winkey_value_names(HKEY root, const char *path, REGSAM view,
                             int (*fn)(const char *name, void *ctx), void *ctx);
int kitty_winkey_subkey_names(HKEY root, const char *path, REGSAM view,
                              int (*fn)(const char *name, void *ctx), void *ctx);

#endif
