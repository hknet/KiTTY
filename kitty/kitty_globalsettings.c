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

/* A value of root\path whole, followed by two NULs, malloc'd: NULL when the
 * key or the value is absent. */
static void *ks_key_value_dup(HKEY root, const char *path, const char *name,
                              REGSAM sam,
                              DWORD *type, DWORD *len)
{
    HKEY k;
    DWORD t = 0, n = 0;
    unsigned char *v = NULL;
    LONG rc;
    if (RegOpenKeyExA(root, path, 0, sam, &k) != ERROR_SUCCESS)
        return NULL;
    for (;;) {
        rc = RegQueryValueExA(k, name, NULL, &t, NULL, &n);
        if (rc != ERROR_SUCCESS)
            break;
        if ((v = malloc(n + 2)) == NULL)
            break;
        rc = RegQueryValueExA(k, name, NULL, &t, v, &n);
        if (rc == ERROR_MORE_DATA) {        /* grew in between: again */
            free(v);
            v = NULL;
            continue;
        }
        if (rc != ERROR_SUCCESS) {
            free(v);
            v = NULL;
            break;
        }
        v[n] = v[n + 1] = '\0';
        if (type) *type = t;
        if (len) *len = n;
        break;
    }
    RegCloseKey(k);
    return v;
}

/* The policy keys: the machine's, then the user's. Software\Policies is one
 * key for 32-bit and 64-bit programs (not redirected). */
#define KS_POLICY_PATH "Software\\Policies\\kapper.net\\KiTTY++"

const char *kitty_settings_section_subkey(const char *section)
{
    if (!section)
        return NULL;
    if (!_stricmp(section, KI_SECTION_PRINT))
        return KI_SECTION_PRINT;
    if (!_stricmp(section, KI_SECTION_FONTFALLBACK))
        return KI_SECTION_FONTFALLBACK;
    return NULL;
}

/* base, or base\sub (malloc'd, free() it), NULL without memory */
static char *ks_path_dup(const char *base, const char *sub)
{
    size_t n = strlen(base) + (sub ? strlen(sub) + 1 : 0) + 1;
    char *p = malloc(n);
    if (p)
        snprintf(p, n, sub ? "%s\\%s" : "%s", base, sub);
    return p;
}


/* One reading of a policy value for every program: a REG_SZ not empty or a
 * REG_DWORD, as text (a DWORD as its decimal number) or as a number (a
 * REG_SZ that is a decimal number whole). Any other kind, an empty REG_SZ
 * or a text that is not a number for a number counts as unset in that key:
 * the next key is read. */
enum { KS_POL_TEXT, KS_POL_DWORD };

/* The text whole (free() it) or, KS_POL_DWORD, a malloc'd DWORD. sub: the
 * section's subkey of the policy key (kitty_settings_section_subkey), NULL
 * for the key itself. */
static void *ks_policy_dup_in(const char *sub, const char *name, int want,
                              int *src)
{
    static const HKEY roots[2] = { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER };
    int i;
    char *path;
    if (!name || !name[0])
        return NULL;
    if ((path = ks_path_dup(KS_POLICY_PATH, sub)) == NULL)
        return NULL;
    for (i = 0; i < 2; i++) {
        DWORD t = 0, n = 0, num = 0;
        int have = 0;
        void *out = NULL;
        unsigned char *v = ks_key_value_dup(roots[i], path, name,
                                            KEY_QUERY_VALUE, &t, &n);
        if (!v)
            continue;
        if (t == REG_DWORD && n == 4) {
            memcpy(&num, v, sizeof(num));
            have = 1;
        } else if (t == REG_SZ && v[0]) {
            char *end;
            long l = strtol((const char *)v, &end, 10);
            if (want == KS_POL_TEXT)
                out = _strdup((const char *)v);
            else if (!*end && end != (char *)v) {
                num = (DWORD)l;
                have = 1;
            }
        }
        if (have) {
            if (want == KS_POL_TEXT) {
                char txt[16];
                snprintf(txt, sizeof(txt), "%d", (int)num);
                out = _strdup(txt);
            } else if ((out = malloc(sizeof(num))) != NULL) {
                memcpy(out, &num, sizeof(num));
            }
        }
        free(v);
        if (out) {
            if (src) *src = i ? KS_SOURCE_USER_POLICY : KS_SOURCE_MACHINE_POLICY;
            free(path);
            return out;
        }
    }
    free(path);
    return NULL;
}

static void *ks_policy_dup(const char *name, int want, int *src)
{
    return ks_policy_dup_in(NULL, name, want, src);
}

static int ks_policy_has_in(const char *sub, const char *name, int want)
{
    int src = 0;
    void *v = ks_policy_dup_in(sub, name, want, &src);
    if (!v)
        return 0;
    free(v);
    return src;
}

static int ks_policy_has(const char *name, int want)
{
    return ks_policy_has_in(NULL, name, want);
}

char *kitty_settings_policy_text_dup(const char *name)
{
    return ks_policy_dup(name, KS_POL_TEXT, NULL);
}

int kitty_settings_policy_source(const char *name, int dword)
{
    return ks_policy_has(name, dword > 0 ? KS_POL_DWORD : KS_POL_TEXT);
}

char *kitty_settings_hardening_dup(const char *name, int *src)
{
    DWORD t = 0, n = 0;
    unsigned char *v;
    char *out = ks_policy_dup(name, KS_POL_TEXT, src);
    if (out)
        return out;
    v = ks_key_value_dup(HKEY_LOCAL_MACHINE, KS_MACHINE_PATH, name,
                         KEY_QUERY_VALUE | KEY_WOW64_64KEY, &t, &n);
    if (!v)
        return NULL;
    if (t == REG_SZ && v[0])
        out = _strdup((const char *)v);
    else if (t == REG_DWORD && n == 4) {
        DWORD num;
        char txt[16];
        memcpy(&num, v, sizeof(num));
        snprintf(txt, sizeof(txt), "%s", num ? "yes" : "no");
        out = _strdup(txt);
    }
    free(v);
    if (out && src)
        *src = KS_SOURCE_MACHINE_KEY;
    return out;
}

int kitty_settings_machine_set(const char *name, const char *value)
{
    HKEY k;
    LONG rc;
    if (value)
        rc = RegCreateKeyExA(HKEY_LOCAL_MACHINE, KS_MACHINE_PATH, 0, NULL, 0,
                             KEY_SET_VALUE | KEY_WOW64_64KEY, NULL, &k, NULL);
    else
        rc = RegOpenKeyExA(HKEY_LOCAL_MACHINE, KS_MACHINE_PATH, 0,
                           KEY_SET_VALUE | KEY_WOW64_64KEY, &k);
    if (rc != ERROR_SUCCESS) {
        if (!value && rc == ERROR_FILE_NOT_FOUND)
            return 1;                      /* nothing there to remove */
        SetLastError((DWORD)rc);
        return 0;
    }
    if (value)
        rc = RegSetValueExA(k, name, 0, REG_SZ, (const BYTE *)value,
                            (DWORD)(strlen(value) + 1));
    else {
        rc = RegDeleteValueA(k, name);
        if (rc == ERROR_FILE_NOT_FOUND)
            rc = ERROR_SUCCESS;
    }
    RegCloseKey(k);
    if (rc != ERROR_SUCCESS) {
        SetLastError((DWORD)rc);
        return 0;
    }
    return 1;
}

/* the policy key's Recommended subkey (and its section subkey) */
static void *ks_recommended_dup(const char *sub, const char *name, int want)
{
    char path[64];
    snprintf(path, sizeof(path), sub ? "Recommended\\%s" : "Recommended", sub);
    return ks_policy_dup_in(path, name, want, NULL);
}

char *kitty_settings_recommended_text_dup(const char *section, const char *name)
{
    return ks_recommended_dup(kitty_settings_section_subkey(section), name,
                              KS_POL_TEXT);
}

int kitty_settings_policy_source_in(const char *section, const char *name,
                                    int dword)
{
    return ks_policy_has_in(kitty_settings_section_subkey(section), name,
                            dword > 0 ? KS_POL_DWORD : KS_POL_TEXT);
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

int kitty_settings_reg_dword_at(const char *h, const char *name, int *value)
{
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

char *kitty_settings_reg_str_dup_at(const char *h, const char *name)
{
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

int kitty_settings_reg_set_at(const char *h, const char *name, DWORD type, const void *data, DWORD len)
{
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

void *kitty_settings_reg_value_dup_at(const char *h, const char *name, DWORD *type, DWORD *len)
{
    const struct ks_ro_value *ro = ks_ro_find(h, name);
    unsigned char *v;
    if (ro) {
        if ((v = malloc(ro->len + 2)) == NULL)
            return NULL;
        memcpy(v, ro->data, ro->len);
        v[ro->len] = v[ro->len + 1] = '\0';
        if (type) *type = ro->type;
        if (len) *len = ro->len;
        return v;
    }
    return kitty_settings_reg_stored_dup_at(h, name, type, len);
}
void *kitty_settings_reg_stored_dup_at(const char *h, const char *name, DWORD *type, DWORD *len)
{
    return ks_key_value_dup(HKEY_CURRENT_USER, h, name, KEY_QUERY_VALUE,
                            type, len);
}

int kitty_settings_reg_del_at(const char *h, const char *name)
{
    HKEY hk;
    LONG rc;
    if (kitty_store_ro_regpath(1, h))
        return KITTY_RO_REFUSED;
    rc = RegOpenKeyExA(HKEY_CURRENT_USER, h, 0, KEY_SET_VALUE, &hk);
    if (rc == ERROR_FILE_NOT_FOUND)
        return 1;
    if (rc == ERROR_SUCCESS) {
        rc = RegDeleteValueA(hk, name);
        RegCloseKey(hk);
        if (rc == ERROR_FILE_NOT_FOUND)
            rc = ERROR_SUCCESS;
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

/* The text of a value of any kind (data followed by two NULs). */
static char *ks_any_text(DWORD type, const unsigned char *data, DWORD n)
{
    char num[48];
    switch (type) {
      case REG_BINARY:
        if (n >= 4)
            snprintf(num, sizeof(num), "%u.%u.%u.%u",
                     (unsigned)data[0], (unsigned)data[1],
                     (unsigned)data[2], (unsigned)data[3]);
        else
            num[0] = '\0';
        return _strdup(num);
      case REG_DWORD: {
        int v = 0;
        memcpy(&v, data, n < sizeof(v) ? n : sizeof(v));
        snprintf(num, sizeof(num), "%d", v);
        return _strdup(num);
      }
      case REG_EXPAND_SZ:
      case REG_MULTI_SZ:
      case REG_SZ:
        return _strdup((const char *)data);
      default:
        return _strdup("");
    }
}

/* A value of any type as kitty.exe has read it (GetValueDataDup): text
 * whole, a DWORD as its number, a BINARY of 4 bytes or more as a.b.c.d,
 * anything else as "". NULL when the key or the value is absent. */
static char *ks_reg_any_dup(const char *hive, const char *name)
{
    DWORD type = 0, n = 0;
    unsigned char *data = ks_key_value_dup(HKEY_CURRENT_USER, hive, name,
                                           KEY_READ, &type, &n);
    char *out = data ? ks_any_text(type, data, n) : NULL;
    free(data);
    return out;
}

char *kitty_profile_string_dup(const char *section, const char *key,
                               const char *def, const char *file);   /* kitty_oldwin.c */

DWORD kitty_settings_file_get(const char *file, const char *section,
                              const char *key, char *buf, DWORD len)
{
    return GetPrivateProfileStringA(section, key, "", buf, len, file);
}
int kitty_settings_file_put(const char *file, const char *section,
                            const char *key, const char *value)
{
    return WritePrivateProfileStringA(section, key, value, file) ? 1 : 0;
}
char *kitty_settings_file_read_dup(const char *file, const char *section,
                                   const char *key)
{
    /* the value GetPrivateProfileString returns when the key is not there:
     * it cannot distinguish "absent" from "empty" otherwise */
    static const char absent[] = "\x01kitty.ini:absent\x01";
    char *v;
    if (!file || !file[0] || !section || !section[0] || !key)
        return NULL;
    v = kitty_profile_string_dup(section, key, absent, file);
    if (v && !strcmp(v, absent)) {
        free(v);
        v = NULL;
    }
    return v;
}

int kitty_settings_file_write(const char *file, const char *section,
                              const char *key, const char *value)
{
    if (!file || !file[0] || !section || !section[0])
        return 0;
    if (kitty_inifile_write_blocked(file))
        return KITTY_RO_REFUSED;
    if (!key || !key[0]) {
        char probe[4];
        if (GetPrivateProfileSectionA(section, probe, sizeof(probe), file) > 0)
            return 1;
        return WritePrivateProfileSectionA(section, "\0", file) ? 1 : 0;
    }
    return WritePrivateProfileStringA(section, key, value ? value : "",
                                      file) ? 1 : 0;
}

int kitty_settings_file_del(const char *file, const char *section,
                            const char *key)
{
    if (!file || !file[0] || !section || !section[0])
        return 0;
    if (kitty_inifile_write_blocked(file))
        return KITTY_RO_REFUSED;
    return WritePrivateProfileStringA(section, (key && key[0]) ? key : NULL,
                                      NULL, file) ? 1 : 0;
}

char *kitty_settings_kitty_read_dup(const char *ini_path, int dir_mode,
                                    const char *section, const char *key)
{
    const char *sub = kitty_settings_section_subkey(section);
    char *v = ks_policy_dup_in(sub, key, KS_POL_TEXT, NULL), *path;
    if (v)                           /* a policy holds it: every save mode */
        return v;
    if (dir_mode) {
        /* Portable directory mode is registry-independent: a stale HKCU
         * value of an installed copy is never read. */
        v = kitty_settings_file_read_dup(ini_path, section, key);
    } else {
        /* a flat section: the hive's own value, a sectioned one: its subkey's */
        if ((path = ks_path_dup(kitty_settings_hive(KS_HIVE_RUNTIME), sub)) != NULL) {
            v = ks_reg_any_dup(path, key);
            free(path);
        }
        if (!v)
            v = kitty_settings_file_read_dup(ini_path, section, key);
    }
    /* the user's own value absent: an administrator's recommended one */
    if (!v)
        v = ks_recommended_dup(sub, key, KS_POL_TEXT);
    return v;
}

int kitty_settings_kitty_write(const char *ini_path, int dir_mode,
                               const char *section, const char *key,
                               const char *value)
{
    const char *sub = kitty_settings_section_subkey(section);
    char *hive;
    HKEY k = NULL;
    LONG rc;
    if (ks_policy_has_in(sub, key, KS_POL_TEXT))
        return KITTY_POLICY_REFUSED;
    if (dir_mode)
        return kitty_settings_file_write(ini_path, section, key, value);
    /* The hive in use, the one the read side looks at: the section dropped
     * (a write to the vendor key plus the section name, read from somewhere
     * else, made a setting come back with its old value for ever), but a
     * sectioned section's own subkey (kitty_settings_section_subkey). */
    if (!kitty_settings_hive(KS_HIVE_RUNTIME)[0])
        return 0;
    if ((hive = ks_path_dup(kitty_settings_hive(KS_HIVE_RUNTIME), sub)) == NULL)
        return 0;
    if (kitty_store_ro_regpath(1, hive)) {
        free(hive);
        return KITTY_RO_REFUSED;
    }
    if (RegOpenKeyExA(HKEY_CURRENT_USER, hive, 0, KEY_WRITE, &k) !=
            ERROR_SUCCESS) {
        /* the reason stays in GetLastError() for the caller's report */
        if ((rc = RegCreateKeyA(HKEY_CURRENT_USER, hive, &k)) !=
                ERROR_SUCCESS) {
            free(hive);
            SetLastError((DWORD)rc);
            return 0;
        }
    }
    free(hive);
    rc = RegSetValueExA(k, key, 0, REG_SZ, (const BYTE *)value,
                        (DWORD)(strlen(value) + 1));
    RegCloseKey(k);
    if (rc != ERROR_SUCCESS) {
        SetLastError((DWORD)rc);
        return 0;
    }
    return 1;
}

char *ksf_munge(const char *in);     /* kitty_sessionpath.c, malloc'd */
char *ksf_unmunge(const char *in);   /* kitty_sessionpath.c, malloc'd */
char *ksf_text_value(const char *buf, size_t len, const char *key);   /* same */
int kitty_write_file_whole(const char *path, const void *data, size_t len);
void smemclr(void *b, size_t len);   /* utils: a wipe the compiler keeps */
/* kitty_store_acl.c, in the programs that write the store (kageant only
 * reads KiTTYState) */
int kitty_store_acl_new_file(const char *path) __attribute__((weak));

/* A file whole (free() it) with its length, NULL when it cannot be read. */
static char *ks_file_dup(const char *path, size_t *len)
{
    FILE *fp = fopen(path, "rb");
    char *buf;
    long n;
    if (!fp)
        return NULL;
    if (fseek(fp, 0, SEEK_END) || (n = ftell(fp)) < 0 ||
        fseek(fp, 0, SEEK_SET) || (buf = malloc((size_t)n + 1)) == NULL) {
        fclose(fp);
        return NULL;
    }
    if (fread(buf, 1, (size_t)n, fp) != (size_t)n) {
        fclose(fp);
        free(buf);
        return NULL;
    }
    fclose(fp);
    buf[n] = '\0';
    *len = (size_t)n;
    return buf;
}

/* One line of the folder store's format: its key (NULL for a comment or a
 * line without one) and its raw value, both pointing into the line, which
 * is cut in place. */
static void ks_state_line(char *line, char **key, char **raw)
{
    const char *f = line;
    char *eq, *bs;
    *key = *raw = NULL;
    while (*f == ' ' || *f == '\t')
        f++;
    if (*f == '#' || *f == ';')
        return;
    eq = strchr(line, '=');
    bs = strchr(line, '\\');
    if (eq && (!bs || eq < bs)) {          /* key=value */
        *eq = '\0';
        *key = line;
        *raw = eq + 1;
    } else if (bs) {                       /* classic KiTTY: key\value\ */
        size_t rl;
        *bs = '\0';
        *key = line;
        *raw = bs + 1;
        rl = strlen(*raw);
        if (rl && (*raw)[rl - 1] == '\\')
            (*raw)[rl - 1] = '\0';
    }
}

/* Calls back with each line (its text cut out, without the line break) and
 * whether it ended in CRLF. The last line without a break takes the file's
 * first line break. */
typedef int (*ks_line_fn)(char *line, int crlf, void *ctx);
static void ks_each_line(char *buf, size_t len, ks_line_fn fn, void *ctx)
{
    size_t i = 0;
    int file_crlf = -1;
    while (i < len) {
        size_t start = i, end;
        int crlf = -1;
        while (i < len && buf[i] != '\n')
            i++;
        end = i;
        if (i < len) {                     /* a line break */
            crlf = (end > start && buf[end - 1] == '\r');
            if (crlf)
                end--;
            i++;
            if (file_crlf < 0)
                file_crlf = crlf;
        }
        if (crlf < 0)
            crlf = file_crlf > 0;
        buf[end] = '\0';
        if (!fn(buf + start, crlf, ctx))
            return;
    }
}

/* A key on more than one line: the last line's value counts, written back
 * as one line at the first line's place (ksf_load, ksf_save). */
struct ks_get_ctx { const char *key; char *out, *last_line; int found; };
static int ks_get_line(char *line, int crlf, void *vctx)
{
    struct ks_get_ctx *c = vctx;
    char *copy, *k, *raw;
    (void)crlf;
    if ((copy = _strdup(line)) == NULL)
        return 1;
    ks_state_line(copy, &k, &raw);
    if (k && !strcmp(k, c->key)) {
        free(c->out);
        free(c->last_line);
        c->out = ksf_unmunge(raw);
        c->last_line = _strdup(line);      /* its text as read */
        c->found = 1;
    }
    free(copy);
    return 1;
}

char *kitty_settings_state_file_get_dup(const char *path, const char *key)
{
    struct ks_get_ctx c;
    size_t len = 0;
    char *buf;
    if (!path || !path[0] || !key || (buf = ks_file_dup(path, &len)) == NULL)
        return NULL;
    /* the folder store's one parser (kitty_sessionpath.c): the last line of
     * the key wins */
    c.out = ksf_text_value(buf, len, key);
    free(buf);
    if (c.out && !c.out[0]) {
        free(c.out);
        c.out = NULL;
    }
    return c.out;
}

struct ks_set_ctx {
    const char *key, *value;
    const char *last_line;     /* the key's last line as read, NULL = none */
    int same;                  /* the new value is the one read */
    char *out;
    size_t n, cap;
    int done, first_crlf;
};
static void ks_put(struct ks_set_ctx *c, const char *s, int crlf)
{
    size_t need = strlen(s) + 3;
    if (!c->out)
        return;
    if (c->n + need > c->cap) {
        char *nb;
        while (c->n + need > c->cap)
            c->cap *= 2;
        if ((nb = realloc(c->out, c->cap)) == NULL) {
            free(c->out);
            c->out = NULL;
            return;
        }
        c->out = nb;
    }
    memcpy(c->out + c->n, s, strlen(s));
    c->n += strlen(s);
    if (crlf)
        c->out[c->n++] = '\r';
    c->out[c->n++] = '\n';
}
static void ks_put_pair(struct ks_set_ctx *c, int crlf)
{
    char *mv = ksf_munge(c->value), *line;
    if (!mv) {
        free(c->out);
        c->out = NULL;
        return;
    }
    line = malloc(strlen(c->key) + strlen(mv) + 2);
    if (line) {
        sprintf(line, "%s=%s", c->key, mv);
        ks_put(c, line, crlf);
        free(line);
    } else {
        free(c->out);
        c->out = NULL;
    }
    free(mv);
}
static int ks_set_line(char *line, int crlf, void *vctx)
{
    struct ks_set_ctx *c = vctx;
    char *copy, *k, *raw;
    if (c->first_crlf < 0)
        c->first_crlf = crlf;
    if (c->last_line && (copy = _strdup(line)) != NULL) {
        int mine;
        ks_state_line(copy, &k, &raw);
        mine = k && !strcmp(k, c->key);
        free(copy);
        if (mine) {
            if (c->done)
                return 1;                  /* a later line of the key: gone */
            c->done = 1;
            if (c->same)                   /* the last line's text, here */
                ks_put(c, c->last_line, crlf);
            else                           /* the line takes the new value */
                ks_put_pair(c, crlf);
            return 1;
        }
    }
    ks_put(c, line, crlf);                 /* as it was read */
    return 1;
}

int kitty_settings_state_file_set(const char *path, const char *key,
                                  const char *value)
{
    struct ks_set_ctx c;
    size_t len = 0;
    char *buf;
    int ok;
    if (!path || !path[0] || !key || !key[0]) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (kitty_store_acl_new_file)
        kitty_store_acl_new_file(path);   /* a new file: the store's rights */
    buf = ks_file_dup(path, &len);
    c.key = key;
    c.value = value ? value : "";
    c.last_line = NULL;
    c.same = 0;
    c.cap = len + strlen(key) + strlen(c.value) * 3 + 64;
    c.out = malloc(c.cap);
    c.n = 0;
    c.done = 0;
    c.first_crlf = -1;
    if (buf) {
        /* first the key's last line and value (a copy: the walk cuts the
         * lines in place) */
        struct ks_get_ctx g;
        char *scan = malloc(len + 1);
        g.key = key;
        g.out = g.last_line = NULL;
        g.found = 0;
        if (scan) {
            memcpy(scan, buf, len + 1);
            ks_each_line(scan, len, ks_get_line, &g);
            free(scan);
        }
        c.last_line = g.last_line;
        c.same = g.out && !strcmp(g.out, c.value);
        ks_each_line(buf, len, ks_set_line, &c);
        free(g.out);
        free(g.last_line);
        free(buf);
    }
    if (!c.done)                           /* a new key: at the end */
        ks_put_pair(&c, c.first_crlf > 0);
    if (!c.out) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    ok = kitty_write_file_whole(path, c.out, c.n) != 0;
    {
        DWORD err = GetLastError();
        smemclr(c.out, c.n);
        free(c.out);
        SetLastError(err);
    }
    return ok;
}

int kitty_settings_reg_dword(int hive, const char *name, int *value)
{
    void *p = ks_policy_dup(name, KS_POL_DWORD, NULL);
    if (p) {
        DWORD v;
        memcpy(&v, p, sizeof(v));
        free(p);
        *value = (int)v;
        return 1;
    }
    if (kitty_settings_reg_dword_at(kitty_settings_hive(hive), name, value))
        return 1;
    /* the user's own value absent: an administrator's recommended one */
    if ((p = ks_recommended_dup(NULL, name, KS_POL_DWORD)) != NULL) {
        DWORD v;
        memcpy(&v, p, sizeof(v));
        free(p);
        *value = (int)v;
        return 1;
    }
    return 0;
}

char *kitty_settings_reg_str_dup(int hive, const char *name)
{
    char *p = ks_policy_dup(name, KS_POL_TEXT, NULL);
    if (p)
        return p;
    if ((p = kitty_settings_reg_str_dup_at(kitty_settings_hive(hive), name)) != NULL)
        return p;
    /* the user's own value absent: an administrator's recommended one */
    return ks_recommended_dup(NULL, name, KS_POL_TEXT);
}

int kitty_settings_reg_set(int hive, const char *name, DWORD type, const void *data, DWORD len)
{
    if ((type == REG_DWORD && ks_policy_has(name, KS_POL_DWORD)) ||
        (type == REG_SZ && ks_policy_has(name, KS_POL_TEXT)))
        return KITTY_POLICY_REFUSED;
    return kitty_settings_reg_set_at(kitty_settings_hive(hive), name, type, data, len);
}

void *kitty_settings_reg_value_dup(int hive, const char *name, DWORD *type, DWORD *len)
{
    return kitty_settings_reg_value_dup_at(kitty_settings_hive(hive), name, type, len);
}

int kitty_settings_reg_del(int hive, const char *name)
{
    if (ks_policy_has(name, KS_POL_TEXT))
        return KITTY_POLICY_REFUSED;
    return kitty_settings_reg_del_at(kitty_settings_hive(hive), name);
}

int kitty_settings_reg_names_at(const char *path, int (*fn)(const char *name,
                                                            void *ctx),
                                void *ctx)
{
    HKEY k;
    DWORD i, nlen, max = 0;
    char *name;
    int n = 0;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, path, 0, KEY_QUERY_VALUE, &k) !=
            ERROR_SUCCESS)
        return 0;
    if (RegQueryInfoKeyA(k, NULL, NULL, NULL, NULL, NULL, NULL, NULL, &max,
                         NULL, NULL, NULL) != ERROR_SUCCESS ||
        (name = malloc(max + 2)) == NULL) {
        RegCloseKey(k);
        return 0;
    }
    for (i = 0; ; i++) {
        nlen = max + 1;
        if (RegEnumValueA(k, i, name, &nlen, NULL, NULL, NULL, NULL) !=
                ERROR_SUCCESS)
            break;
        n++;
        if (!fn(name, ctx))
            break;
    }
    free(name);
    RegCloseKey(k);
    return n;
}

LONG kitty_settings_reg_stored_set_at(const char *path, const char *name,
                                      DWORD type, const void *data, DWORD len)
{
    HKEY k;
    LONG rc = RegOpenKeyExA(HKEY_CURRENT_USER, path, 0, KEY_WRITE, &k);
    if (rc != ERROR_SUCCESS &&
        (rc = RegCreateKeyA(HKEY_CURRENT_USER, path, &k)) != ERROR_SUCCESS)
        return rc;
    rc = ERROR_SUCCESS;
    if (name != NULL)
        rc = RegSetValueExA(k, name, 0, type, (const BYTE *)data, len);
    RegCloseKey(k);
    return rc;
}
LONG kitty_settings_reg_stored_del_at(const char *path, const char *name)
{
    HKEY k;
    LONG rc = RegOpenKeyExA(HKEY_CURRENT_USER, path, 0, KEY_WRITE, &k);
    if (rc == ERROR_SUCCESS) {
        rc = RegDeleteValueA(k, name);
        RegCloseKey(k);
    }
    return rc;
}
LONG kitty_settings_reg_get_stored_at(const char *path, const char *name,
                                      DWORD flags, void *buf, DWORD *size)
{
    return RegGetValueA(HKEY_CURRENT_USER, path, name, flags, NULL, buf, size);
}
LONG kitty_settings_reg_key_check(const char *path)
{
    HKEY k;
    LONG rc = RegOpenKeyExA(HKEY_CURRENT_USER, path, 0, KEY_READ, &k);
    if (rc == ERROR_SUCCESS)
        RegCloseKey(k);
    return rc;
}
LONG kitty_settings_reg_key_del_at(const char *path)
{
    return RegDeleteKeyA(HKEY_CURRENT_USER, path);
}

int kitty_settings_reg_subkeys_at(const char *path,
                                  int (*fn)(const char *name, void *ctx),
                                  void *ctx)
{
    HKEY k;
    DWORD i, nlen, max = 0;
    char *name;
    int n = 0;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, path, 0, KEY_READ, &k) !=
            ERROR_SUCCESS)
        return -1;
    if (RegQueryInfoKeyA(k, NULL, NULL, NULL, NULL, &max, NULL, NULL, NULL,
                         NULL, NULL, NULL) != ERROR_SUCCESS ||
        (name = malloc(max + 2)) == NULL) {
        RegCloseKey(k);
        return 0;
    }
    for (i = 0; ; i++) {
        nlen = max + 1;
        if (RegEnumKeyExA(k, i, name, &nlen, NULL, NULL, NULL, NULL) !=
                ERROR_SUCCESS)
            break;
        n++;
        if (!fn(name, ctx))
            break;
    }
    free(name);
    RegCloseKey(k);
    return n;
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

/* The policy's value of the setting, NULL when no policy holds one it
 * takes. */
static char *ks_policy_setting_dup(const struct kitty_setting *s)
{
    return ks_checked(s, ks_policy_dup(s->regname, KS_POL_TEXT, NULL));
}

char *kitty_settings_read_dup(const struct kitty_setting *s)
{
    const char *hive = kitty_settings_hive(s->hive);
    char *v = ks_policy_setting_dup(s);
    if (v)
        return v;
    if (kitty_inifile_registry_authoritative()) {
        v = ks_checked(s, ks_reg_sz_dup(hive, s->regname));
        if (!v)
            v = ks_checked(s, ks_ini_dup(s->section, s->key));
    } else {
        v = ks_checked(s, ks_ini_dup(s->section, s->key));
        if (!v && s->rule == KS_RULE_CHARGE_FIRST)
            v = ks_checked(s, ks_reg_sz_dup(hive, s->regname));
    }
    /* the user's own value absent: an administrator's recommended one */
    if (!v)
        v = ks_checked(s, ks_recommended_dup(NULL, s->regname, KS_POL_TEXT));
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
    char *pv = ks_policy_setting_dup(s);
    if (pv) {
        free(pv);
        return KITTY_POLICY_REFUSED;
    }
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
