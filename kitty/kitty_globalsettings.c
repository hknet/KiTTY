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
#include "kitty_oldwin_reg.h"   /* XP: RegGetValue via oldwin */

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

/* readonly=yes: a value kitty_settings_reg_set did not store, held for the
 * running program - the readers answer from it first, so a change made in a
 * settings dialog applies until the program exits. */
struct ks_ro_value {
    char *hive, *name;
    DWORD type, len;
    unsigned char *data;
    struct ks_ro_value *next;
};
static struct ks_ro_value *ks_ro_values = NULL;

static struct ks_ro_value *ks_ro_find(const char *hive, const char *name)
{
    struct ks_ro_value *v;
    for (v = ks_ro_values; v; v = v->next)
        if (!_stricmp(v->hive, hive) && !_stricmp(v->name, name))
            return v;
    return NULL;
}

static void ks_ro_keep(const char *hive, const char *name, DWORD type,
                       const void *data, DWORD len)
{
    struct ks_ro_value *v = ks_ro_find(hive, name);
    unsigned char *d = malloc(len ? len : 1);
    if (!d)
        return;
    if (len)
        memcpy(d, data, len);
    if (!v) {
        v = malloc(sizeof(*v));
        if (!v) {
            free(d);
            return;
        }
        v->hive = _strdup(hive);
        v->name = _strdup(name);
        v->data = NULL;
        v->next = ks_ro_values;
        ks_ro_values = v;
    }
    free(v->data);
    v->type = type;
    v->len = len;
    v->data = d;
}

int kitty_settings_reg_dword(int hive, const char *name, int *value)
{
    const char *h = kitty_settings_hive(hive);
    const struct ks_ro_value *ro = ks_ro_find(h, name);
    DWORD v, sz = sizeof(v);
    if (ro) {
        if (ro->type != REG_DWORD || ro->len != sizeof(DWORD))
            return 0;
        memcpy(&v, ro->data, sizeof(v));
        *value = (int)v;
        return 1;
    }
    if (RegGetValueA(HKEY_CURRENT_USER, h, name, RRF_RT_REG_DWORD, NULL, &v,
                     &sz) != ERROR_SUCCESS)
        return 0;
    *value = (int)v;
    return 1;
}

char *kitty_settings_reg_str_dup(int hive, const char *name)
{
    const char *h = kitty_settings_hive(hive);
    const struct ks_ro_value *ro = ks_ro_find(h, name);
    char *buf;
    int tries;
    if (ro) {                          /* readonly=yes: this run's value */
        if (ro->type != REG_SZ || !ro->len)
            return NULL;
        buf = malloc(ro->len + 1);
        if (!buf)
            return NULL;
        memcpy(buf, ro->data, ro->len);
        buf[ro->len] = '\0';
        if (!buf[0]) {
            free(buf);
            return NULL;
        }
        return buf;
    }
    /* the size first, read again if the value grew in between */
    for (tries = 0; tries < 4; tries++) {
        DWORD sz = 0;
        LONG rc = RegGetValueA(HKEY_CURRENT_USER, h, name, RRF_RT_REG_SZ,
                               NULL, NULL, &sz);
        if (rc != ERROR_SUCCESS || !sz)
            return NULL;
        /* one byte more: room for the NUL of a value stored without one
         * (Windows XP's stand-in for RegGetValue adds it in place) */
        buf = malloc(sz + 1);
        if (!buf)
            return NULL;
        sz++;
        rc = RegGetValueA(HKEY_CURRENT_USER, h, name, RRF_RT_REG_SZ, NULL,
                          buf, &sz);
        if (rc == ERROR_SUCCESS && buf[0])
            return buf;
        free(buf);
        if (rc != ERROR_MORE_DATA)
            return NULL;
    }
    return NULL;
}

int kitty_settings_reg_set(int hive, const char *name, DWORD type,
                           const void *data, DWORD len)
{
    const char *h = kitty_settings_hive(hive);
    HKEY hk;
    LONG rc;
    if (kitty_store_ro_regpath(1, h)) {
        ks_ro_keep(h, name, type, data, len);
        return KITTY_RO_REFUSED;
    }
    rc = RegCreateKeyExA(HKEY_CURRENT_USER, h, 0, NULL, 0, KEY_SET_VALUE,
                         NULL, &hk, NULL);
    if (rc == ERROR_SUCCESS) {
        rc = RegSetValueExA(hk, name, 0, type, (const BYTE *)data, len);
        RegCloseKey(hk);
    }
    if (rc != ERROR_SUCCESS) {
        SetLastError((DWORD)rc);
        return 0;
    }
    return 1;
}

char *kitty_settings_ini_dup(const char *section, const char *key)
{
    char *m = kitty_inifile_read_dup(section, key);   /* malloc'd */
    if (m && !m[0]) {
        free(m);
        m = NULL;
    }
    return m;
}

/* A value of any type as kitty.exe has read it (GetValueDataDup): text
 * whole, a DWORD as its number, a BINARY of 4 bytes or more as a.b.c.d,
 * anything else as "". NULL when the key or the value is absent. */
static char *ks_reg_any_dup(const char *hive, const char *name)
{
    HKEY k;
    DWORD type = 0, n = 0;
    unsigned char *data;
    char *out = NULL;
    LONG rc;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, hive, 0, KEY_READ, &k) !=
            ERROR_SUCCESS)
        return NULL;
    for (;;) {
        rc = RegQueryValueExA(k, name, NULL, &type, NULL, &n);
        if (rc != ERROR_SUCCESS)
            break;
        data = malloc(n + 2);
        if (!data)
            break;
        rc = RegQueryValueExA(k, name, NULL, &type, data, &n);
        if (rc == ERROR_MORE_DATA) {        /* grew in between: again */
            free(data);
            continue;
        }
        if (rc == ERROR_SUCCESS) {
            char num[48];
            data[n] = data[n + 1] = '\0';
            switch (type) {
              case REG_BINARY:
                if (n >= 4)
                    snprintf(num, sizeof(num), "%u.%u.%u.%u",
                             (unsigned)data[0], (unsigned)data[1],
                             (unsigned)data[2], (unsigned)data[3]);
                else
                    num[0] = '\0';
                out = _strdup(num);
                break;
              case REG_DWORD: {
                int v = 0;
                memcpy(&v, data, n < sizeof(v) ? n : sizeof(v));
                snprintf(num, sizeof(num), "%d", v);
                out = _strdup(num);
                break;
              }
              case REG_EXPAND_SZ:
              case REG_MULTI_SZ:
              case REG_SZ:
                out = _strdup((char *)data);
                break;
              default:
                out = _strdup("");
                break;
            }
        }
        free(data);
        break;
    }
    RegCloseKey(k);
    return out;
}

/* A kitty.ini value whole, an empty one included (free() it), NULL when the
 * file or the key is absent. */
static char *ks_ini_file_dup(const char *file, const char *section,
                             const char *key)
{
    static const char absent[] = "\x01kitty.ini:absent\x01";
    DWORD len = 512, got;
    char *buf;
    if (!file || !file[0] || !section || !section[0] || !key)
        return NULL;
    for (;;) {
        buf = malloc(len);
        if (!buf)
            return NULL;
        got = GetPrivateProfileStringA(section, key, absent, buf, len, file);
        if (got < len - 2 || len >= (1u << 24))
            break;
        free(buf);
        len *= 2;
    }
    if (!strcmp(buf, absent)) {
        free(buf);
        return NULL;
    }
    return buf;
}

char *kitty_settings_kitty_read_dup(const char *ini_path, int dir_mode,
                                    const char *section, const char *key)
{
    char *v;
    if (dir_mode)
        /* Portable directory mode is registry-independent: a stale HKCU
         * value of an installed copy is never read. */
        return ks_ini_file_dup(ini_path, section, key);
    v = ks_reg_any_dup(kitty_settings_hive(KS_HIVE_RUNTIME), key);
    if (!v)
        v = ks_ini_file_dup(ini_path, section, key);
    return v;
}

int kitty_settings_kitty_write(const char *ini_path, int dir_mode,
                               const char *section, const char *key,
                               const char *value)
{
    const char *hive;
    HKEY k = NULL;
    LONG rc;
    if (dir_mode) {
        if (!ini_path || !ini_path[0] || !section || !section[0])
            return 0;
        if (kitty_inifile_write_blocked(ini_path))
            return KITTY_RO_REFUSED;
        return WritePrivateProfileStringA(section, key, value ? value : "",
                                          ini_path) ? 1 : 0;
    }
    /* The hive in use, the one the read side looks at, the section dropped
     * (a write to the vendor key plus the section name, read from somewhere
     * else, made a setting come back with its old value for ever). */
    hive = kitty_settings_hive(KS_HIVE_RUNTIME);
    if (!hive[0])
        return 0;
    if (kitty_store_ro_regpath(1, hive))
        return KITTY_RO_REFUSED;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, hive, 0, KEY_WRITE, &k) !=
            ERROR_SUCCESS) {
        /* the reason stays in GetLastError() for the caller's report */
        if ((rc = RegCreateKeyA(HKEY_CURRENT_USER, hive, &k)) !=
                ERROR_SUCCESS) {
            SetLastError((DWORD)rc);
            return 0;
        }
    }
    rc = RegSetValueExA(k, key, 0, REG_SZ, (const BYTE *)value,
                        (DWORD)(strlen(value) + 1));
    RegCloseKey(k);
    if (rc != ERROR_SUCCESS) {
        SetLastError((DWORD)rc);
        return 0;
    }
    return 1;
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
