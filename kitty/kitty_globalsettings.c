/*
 * kitty_globalsettings.c - the one reader and writer of the global settings,
 * linked by every program. See kitty_globalsettings.h.
 */

#include <stdio.h>
#include <string.h>

#include <windows.h>
#include <stdlib.h>
#include "kitty_globalsettings.h"
#include "kitty_inifile.h"
#include "kitty_inikeys.h"

/* kitty_storage.c, in the programs that link it (kageant does not: it never
 * reads KS_HIVE_RUNTIME, the fixed key stands in) */
const char *kitty_registry_base(void) __attribute__((weak));

const char *kitty_settings_hive(int hive)
{
    switch (hive) {
      case KS_HIVE_RUNTIME:
        if (kitty_registry_base)
            return kitty_registry_base();
        return "Software\\kapper.net\\KiTTY";
      case KS_HIVE_INICLASS:
        return kitty_inifile_registry_base();
      default:
        return "Software\\kapper.net\\KiTTY";
    }
}

/* A REG_SZ value whole (free() it), NULL when it is absent, empty or of
 * another type. */
static char *ks_reg_sz_dup(const char *hive, const char *name)
{
    HKEY k;
    DWORD type = 0, n = 0;
    char *v = NULL;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, hive, 0, KEY_QUERY_VALUE, &k) !=
            ERROR_SUCCESS)
        return NULL;
    if (RegQueryValueExA(k, name, NULL, &type, NULL, &n) == ERROR_SUCCESS &&
        type == REG_SZ && n > 0) {
        v = malloc(n + 1);
        if (RegQueryValueExA(k, name, NULL, &type, (LPBYTE)v, &n) ==
                ERROR_SUCCESS && type == REG_SZ) {
            v[n] = '\0';
        } else {
            free(v);
            v = NULL;
        }
    }
    RegCloseKey(k);
    if (v && !v[0]) {
        free(v);
        v = NULL;
    }
    return v;
}

/* kitty.ini's value whole (free() it), NULL when absent or empty. */
static char *ks_ini_dup(const char *section, const char *key)
{
    char *m = kitty_inifile_read_dup(section, key);   /* malloc'd */
    char *v = (m && m[0]) ? _strdup(m) : NULL;
    free(m);
    return v;
}

/* NULL when the text is not a value of the setting. */
static char *ks_checked(const struct kitty_setting *s, char *v)
{
    if (v && s->valid && !s->valid(v)) {
        free(v);
        v = NULL;
    }
    return v;
}

char *kitty_settings_read_dup(const struct kitty_setting *s)
{
    const char *hive = kitty_settings_hive(s->hive);
    char *v = NULL;
    if (kitty_inifile_registry_authoritative()) {
        v = ks_checked(s, ks_reg_sz_dup(hive, s->regname));
        if (!v)
            v = ks_checked(s, ks_ini_dup(s->section, s->key));
    } else {
        v = ks_checked(s, ks_ini_dup(s->section, s->key));
        if (!v && s->rule == KS_RULE_CHARGE_FIRST)
            v = ks_checked(s, ks_reg_sz_dup(hive, s->regname));
    }
    return v;
}

void kitty_settings_read(const struct kitty_setting *s, char *value, int len)
{
    char *v;
    if (len < 1)
        return;
    value[0] = '\0';
    v = kitty_settings_read_dup(s);
    if (v) {
        strncpy(value, v, (size_t)len - 1);
        value[len - 1] = '\0';
        free(v);
    }
}

int kitty_settings_write(const struct kitty_setting *s, const char *value)
{
    const char *hive;
    HKEY hk;
    LONG rc;
    if (!kitty_inifile_registry_authoritative())
        /* kitty.ini in charge (readonly=yes: kitty_inifile_write leaves it) */
        return kitty_inifile_write(s->section, s->key, value);
    hive = kitty_settings_hive(s->hive);
    if (kitty_store_ro_regpath(1, hive))
        return KITTY_RO_REFUSED;
    rc = RegCreateKeyExA(HKEY_CURRENT_USER, hive, 0, NULL, 0, KEY_SET_VALUE,
                         NULL, &hk, NULL);
    if (rc == ERROR_SUCCESS) {
        rc = RegSetValueExA(hk, s->regname, 0, REG_SZ, (const BYTE *)value,
                            (DWORD)(strlen(value) + 1));
        RegCloseKey(hk);
    }
    if (rc != ERROR_SUCCESS) {
        SetLastError((DWORD)rc);
        return 0;
    }
    return 1;
}

/* A global [KiTTY] key read where kitty.exe keeps it: the console tools'
 * and kittygen's reader (the Event Log switches, verifyagent,
 * autostoresshkey, inheritdefaultsettings). "" when it is set nowhere. */
void kitty_console_global_read(const char *key, char *value, int len)
{
    struct kitty_setting s;
    s.section = KI_SECTION_KITTY;
    s.key = key;
    s.regname = key;
    s.hive = KS_HIVE_RUNTIME;
    s.rule = KS_RULE_REG_OVER_INI;
    s.valid = NULL;
    kitty_settings_read(&s, value, len);
}
