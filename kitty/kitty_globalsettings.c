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

void *kitty_settings_reg_value_dup(int hive, const char *name, DWORD *type,
                                   DWORD *len)
{
    const char *h = kitty_settings_hive(hive);
    const struct ks_ro_value *ro = ks_ro_find(h, name);
    HKEY k;
    DWORD t = 0, n = 0;
    unsigned char *v = NULL;
    LONG rc;
    if (ro) {
        if ((v = malloc(ro->len + 2)) == NULL)
            return NULL;
        memcpy(v, ro->data, ro->len);
        v[ro->len] = v[ro->len + 1] = '\0';
        if (type) *type = ro->type;
        if (len) *len = ro->len;
        return v;
    }
    if (RegOpenKeyExA(HKEY_CURRENT_USER, h, 0, KEY_QUERY_VALUE, &k) !=
            ERROR_SUCCESS)
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

int kitty_settings_reg_del(int hive, const char *name)
{
    const char *h = kitty_settings_hive(hive);
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

char *kitty_profile_string_dup(const char *section, const char *key,
                               const char *def, const char *file);   /* kitty_oldwin.c */

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
    char *v;
    if (dir_mode)
        /* Portable directory mode is registry-independent: a stale HKCU
         * value of an installed copy is never read. */
        return kitty_settings_file_read_dup(ini_path, section, key);
    v = ks_reg_any_dup(kitty_settings_hive(KS_HIVE_RUNTIME), key);
    if (!v)
        v = kitty_settings_file_read_dup(ini_path, section, key);
    return v;
}

int kitty_settings_kitty_write(const char *ini_path, int dir_mode,
                               const char *section, const char *key,
                               const char *value)
{
    const char *hive;
    HKEY k = NULL;
    LONG rc;
    if (dir_mode)
        return kitty_settings_file_write(ini_path, section, key, value);
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

char *ksf_munge(const char *in);     /* kitty_sessionpath.c, malloc'd */
char *ksf_unmunge(const char *in);   /* kitty_sessionpath.c, malloc'd */
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

struct ks_get_ctx { const char *key; char *out; int found; };
static int ks_get_line(char *line, int crlf, void *vctx)
{
    struct ks_get_ctx *c = vctx;
    char *k, *raw;
    (void)crlf;
    ks_state_line(line, &k, &raw);
    if (k && !strcmp(k, c->key)) {
        c->out = ksf_unmunge(raw);
        c->found = 1;
        return 0;                          /* the first line of the key */
    }
    return 1;
}

char *kitty_settings_state_file_get_dup(const char *path, const char *key)
{
    struct ks_get_ctx c;
    size_t len = 0;
    char *buf;
    if (!path || !path[0] || !key || (buf = ks_file_dup(path, &len)) == NULL)
        return NULL;
    c.key = key;
    c.out = NULL;
    c.found = 0;
    ks_each_line(buf, len, ks_get_line, &c);
    free(buf);
    if (c.out && !c.out[0]) {
        free(c.out);
        c.out = NULL;
    }
    return c.out;
}

struct ks_set_ctx {
    const char *key, *value;
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
    if (!c->done && (copy = _strdup(line)) != NULL) {
        ks_state_line(copy, &k, &raw);
        if (k && !strcmp(k, c->key)) {
            char *old = ksf_unmunge(raw);
            int same = old && !strcmp(old, c->value);
            free(old);
            free(copy);
            c->done = 1;
            if (!same) {                   /* the line takes the new value */
                ks_put_pair(c, crlf);
                return 1;
            }
        } else {
            free(copy);
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
    c.cap = len + strlen(key) + strlen(c.value) * 3 + 64;
    c.out = malloc(c.cap);
    c.n = 0;
    c.done = 0;
    c.first_crlf = -1;
    if (buf) {
        ks_each_line(buf, len, ks_set_line, &c);
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
