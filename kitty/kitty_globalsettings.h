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
 * nowhere. */
char *kitty_settings_read_dup(const struct kitty_setting *s);

/* Into a caller's buffer of len bytes: "" when it is set nowhere. */
void kitty_settings_read(const struct kitty_setting *s, char *value, int len);

/* To the store in charge only. readonly=yes: not stored, KITTY_RO_REFUSED.
 * 1 when stored, 0 with the reason in GetLastError(). */
int kitty_settings_write(const struct kitty_setting *s, const char *value);

/* The global [KiTTY] keys the console tools and kittygen read where
 * kitty.exe keeps them (KS_HIVE_RUNTIME, KS_RULE_REG_OVER_INI). */
void kitty_console_global_read(const char *key, char *value, int len);

#endif
