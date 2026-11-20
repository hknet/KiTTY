/*
 * kitty_globalsettings.h - the one reader and writer of the global settings,
 * linked by every program.
 *
 * Global settings live in kitty.ini (the file kitty_inifile.c resolves) or
 * in a registry key, by a rule that depends on the setting and the program.
 * Each rule is one function here, so every program that reads a setting by
 * that rule reads it the same way. The rules keep the behaviour each
 * program had before they moved here (the settings matrix records it).
 */
#ifndef KITTY_GLOBALSETTINGS_H
#define KITTY_GLOBALSETTINGS_H

#include <windows.h>   /* DWORD */

/* The policy keys: HKLM\Software\Policies\kapper.net\KiTTY++, then the
 * same path under HKCU, read before every store and in every save mode. A
 * value there holds the setting of the same name as the registry store
 * names it, read the same way by every program: a REG_SZ not empty or a
 * REG_DWORD, as text (a DWORD as its decimal number) or as a number (a
 * REG_SZ that is a decimal number whole). Another kind counts as unset
 * there. KiClassName, savemode and readonly too: kitty_inifile.c
 * reads them from a policy first, so a policy decides the store. Without a
 * value there nothing changes.
 * The setters do not store a value a policy holds: KITTY_POLICY_REFUSED,
 * non-zero, so a caller testing for failure stays silent. It is not
 * KITTY_RO_REFUSED, which keeps the value for the run. */
#define KITTY_POLICY_REFUSED (-3)
enum {
    KS_SOURCE_NONE,
    KS_SOURCE_MACHINE_POLICY,  /* HKLM\Software\Policies\... */
    KS_SOURCE_USER_POLICY      /* HKCU\Software\Policies\... */
};
/* Which policy key holds the registry name: KS_SOURCE_*. dword 1: for a
 * reader of numbers, else for a reader of text. */
int kitty_settings_policy_source(const char *name, int dword);
/* A policy's value of the registry name as text (free() it), NULL when no
 * policy key holds one. */
char *kitty_settings_policy_text_dup(const char *name);

/* Which registry key holds the settings - the programs differ. */
enum {
    KS_HIVE_RUNTIME,   /* kitty_registry_base(): the key this process uses */
    KS_HIVE_INICLASS,  /* by kitty.ini's KiClassName (kitty_inifile_registry_base) */
    KS_HIVE_FIXED      /* Software\kapper.net\KiTTY, KiClassName ignored */
};

/* How the stores combine for a read. */
enum {
    /* the registry in charge: its REG_SZ value, else kitty.ini. kitty.ini in
     * charge: kitty.ini only */
    KS_RULE_REG_OVER_INI,
    /* the store in charge first, the other one as the fallback */
    KS_RULE_CHARGE_FIRST
};

struct kitty_setting {
    const char *section;   /* kitty.ini section */
    const char *key;       /* kitty.ini key */
    const char *regname;   /* registry value name (REG_SZ) */
    int hive;              /* KS_HIVE_* */
    int rule;              /* KS_RULE_* */
    /* NULL, or: is this text a value of the setting? A value that is not
     * counts as unset in the store it came from. */
    int (*valid)(const char *value);
};

/* The registry key a KS_HIVE_* names. */
const char *kitty_settings_hive(int hive);

/* The setting's value by its rule, whole (free() it), NULL when it is set
 * nowhere. A policy's value of regname first. */
char *kitty_settings_read_dup(const struct kitty_setting *s);

/* Into a caller's buffer of len bytes: "" when it is set nowhere. */
void kitty_settings_read(const struct kitty_setting *s, char *value, int len);

/* To the store in charge only. readonly=yes: not stored, KITTY_RO_REFUSED.
 * A policy holds it: not stored, KITTY_POLICY_REFUSED. 1 when stored, 0 with
 * the reason in GetLastError(). */
int kitty_settings_write(const struct kitty_setting *s, const char *value);

/* The pieces the rules are built from, for the programs whose settings
 * still parse their own values (kageant's [Agent] keys). */

/* A DWORD value under the key: 1 with it in *value, 0 when absent or of
 * another type. A policy's value as a number first, then a value
 * readonly=yes refused this run. */
int kitty_settings_reg_dword(int hive, const char *name, int *value);

/* A REG_SZ value whole (free() it), NULL when absent, empty or of another
 * type. A policy's value as text first, then a value readonly=yes refused
 * this run. */
char *kitty_settings_reg_str_dup(int hive, const char *name);

/* One value under the key: 1 when stored, 0 with the reason in
 * GetLastError(). readonly=yes: not stored, KITTY_RO_REFUSED. The value
 * holds for this run (the two readers above answer from it). A REG_DWORD or
 * REG_SZ of a name a policy holds (the readers above answer with the
 * policy's): KITTY_POLICY_REFUSED. */
int kitty_settings_reg_set(int hive, const char *name, DWORD type,
                           const void *data, DWORD len);

/* A value of any type whole, followed by two NULs (a REG_MULTI_SZ list ends
 * safely), free() it, NULL when the key or the value is absent. *type and
 * *len (the stored bytes) when not NULL. A value readonly=yes refused this
 * run answers first. */
void *kitty_settings_reg_value_dup(int hive, const char *name, DWORD *type,
                                   DWORD *len);

/* The same pieces under a key given by its path below HKEY_CURRENT_USER
 * (a subkey of the settings: window positions, the helpers' hashes). */
int kitty_settings_reg_dword_at(const char *path, const char *name,
                                int *value);
char *kitty_settings_reg_str_dup_at(const char *path, const char *name);
int kitty_settings_reg_set_at(const char *path, const char *name, DWORD type,
                              const void *data, DWORD len);
void *kitty_settings_reg_value_dup_at(const char *path, const char *name,
                                      DWORD *type, DWORD *len);
int kitty_settings_reg_del_at(const char *path, const char *name);
/* Calls fn with each value name under the key until it returns 0, the
 * number of names seen. Removing values while it runs moves the rest:
 * collect first. */
int kitty_settings_reg_names_at(const char *path,
                                int (*fn)(const char *name, void *ctx),
                                void *ctx);
/* A value of HKCU\path as the hive holds it, the readonly overlay left out
 * (the readers that never took it: a session's or a named proxy's values,
 * the special menu). malloc'd, NULL when absent. */
void *kitty_settings_reg_stored_dup_at(const char *path, const char *name,
                                       DWORD *type, DWORD *len);
/* A value written to HKCU\path as it is, the key made when missing (name
 * NULL: the key only), no readonly overlay: Windows' answer. For the
 * writers that check readonly=yes themselves. */
LONG kitty_settings_reg_stored_set_at(const char *path, const char *name,
                                      DWORD type, const void *data, DWORD len);
/* A value of HKCU\path removed: Windows' answer (the key's when it does not
 * open, ERROR_FILE_NOT_FOUND when the value is not there). */
LONG kitty_settings_reg_stored_del_at(const char *path, const char *name);
/* RegGetValue of HKCU\path as the hive holds it (flags RRF_RT_*, the
 * buffer's size in *size), no readonly overlay: Windows' answer. */
LONG kitty_settings_reg_get_stored_at(const char *path, const char *name,
                                      DWORD flags, void *buf, DWORD *size);
/* Whether HKCU\path opens for reading: Windows' answer (ERROR_SUCCESS). */
LONG kitty_settings_reg_key_check(const char *path);
/* HKCU\path removed, a key without subkeys: Windows' answer. */
LONG kitty_settings_reg_key_del_at(const char *path);
/* Each subkey name of HKCU\path in turn, fn returning 0 stops: the number of
 * names seen, -1 when the key cannot be opened. */
int kitty_settings_reg_subkeys_at(const char *path,
                                  int (*fn)(const char *name, void *ctx),
                                  void *ctx);

/* Remove one value under the key: 1 when gone (or never there), 0 with the
 * reason in GetLastError(). readonly=yes: KITTY_RO_REFUSED. A policy holds
 * it: KITTY_POLICY_REFUSED. */
int kitty_settings_reg_del(int hive, const char *name);

/* A kitty.ini value whole (free() it), NULL when absent or empty. */
char *kitty_settings_ini_dup(const char *section, const char *key);

/* One key of a kitty.ini by file, section and key, through Windows' own ini
 * functions: a write changes its one line and leaves the rest of the file -
 * comments, order, layout - as it was.
 *   read: the value whole, an empty one included (free() it), NULL when the
 *   file, the section or the key is absent.
 *   write: 1 written, 0 not with Windows' reason in GetLastError(). A NULL
 *   key makes sure the section exists. The kitty.ini in charge under
 *   readonly=yes: nothing written, KITTY_RO_REFUSED (del too).
 *   del: one key, or with a NULL key the whole section. */
/* GetPrivateProfileString of file into buf (len bytes, "" when absent, a
 * longer value cut as Windows cuts it): the characters copied. */
DWORD kitty_settings_file_get(const char *file, const char *section,
                              const char *key, char *buf, DWORD len);
/* WritePrivateProfileString of file as it is (value NULL: the key removed),
 * no readonly check: 1 written, 0 not (Windows' reason in GetLastError()).
 * For the writers that handle readonly=yes themselves. */
int kitty_settings_file_put(const char *file, const char *section,
                            const char *key, const char *value);
char *kitty_settings_file_read_dup(const char *file, const char *section,
                                   const char *key);
int kitty_settings_file_write(const char *file, const char *section,
                              const char *key, const char *value);
int kitty_settings_file_del(const char *file, const char *section,
                            const char *key);

/* The KiTTYState file of a folder store (the remembered values: the last
 * session, Manage Sessions' state, the update check's answer), in the folder
 * store's line format: key=value with the value %HH-escaped (ksf_munge),
 * classic KiTTY's key\value\ read too, comments and every line's own line
 * ending kept.
 *   get: the first line of the key, decoded, whole (free() it), NULL when
 *   the file or the key is absent or the value empty.
 *   set: the key's line takes the value (a new key goes at the end, with the
 *   file's line ending, LF in a new file), the file written whole or not at
 *   all. 1 written, 0 not with the reason in GetLastError(). readonly=yes is
 *   the caller's to check. */
char *kitty_settings_state_file_get_dup(const char *path, const char *key);
int kitty_settings_state_file_set(const char *path, const char *key,
                                  const char *value);

/* kitty.exe's rule (ReadParameter*, WriteParameter). ini_path is the
 * kitty.ini kitty.exe resolved (NULL or "" before it is known: no kitty.ini
 * value), dir_mode its savemode=dir.
 *   read: a policy's value of key first. dir_mode - kitty.ini only.
 *   Otherwise the registry value of that name under KS_HIVE_RUNTIME, the
 *   section ignored, of any type (a DWORD as its decimal number, a BINARY of
 *   4 bytes or more as a.b.c.d, another type as ""), else kitty.ini. An
 *   empty value counts as set in both stores. Whole (free() it), NULL when
 *   set nowhere.
 *   write: dir_mode - kitty.ini, else a REG_SZ under KS_HIVE_RUNTIME. The
 *   store in charge under readonly=yes: KITTY_RO_REFUSED. A policy value of
 *   key: KITTY_POLICY_REFUSED. 1 when stored, 0 with the reason in
 *   GetLastError(). */
char *kitty_settings_kitty_read_dup(const char *ini_path, int dir_mode,
                                    const char *section, const char *key);
int kitty_settings_kitty_write(const char *ini_path, int dir_mode,
                               const char *section, const char *key,
                               const char *value);

/* The global [KiTTY] keys the console tools and kittygen read where
 * kitty.exe keeps them (KS_HIVE_RUNTIME, KS_RULE_REG_OVER_INI). */
void kitty_console_global_read(const char *key, char *value, int len);

#endif
