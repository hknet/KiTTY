/*
 * kitty_winkeys.c - Windows' own keys, read and written in one place
 * (kitty_winkeys.h). The only file outside the settings module and the
 * session store that calls the registry API (tools/raw-registry.allow).
 */
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include "kitty_winkeys.h"

static LONG wk_open(HKEY root, const char *path, REGSAM access, REGSAM view,
                    HKEY *k)
{
    return RegOpenKeyExA(root, path, 0, access | view, k);
}

void *kitty_winkey_handle_value_dup(HKEY k, const char *name, DWORD *type,
                                    DWORD *len)
{
    DWORD t = 0, n = 0;
    unsigned char *v = NULL;
    LONG rc;
    /* the value in full: its size queried, the value read, queried again while it grows */
    for (;;) {
        rc = RegQueryValueExA(k, name, NULL, &t, NULL, &n);
        if (rc != ERROR_SUCCESS)
            break;
        free(v);
        if ((v = malloc(n + 2)) == NULL)
            break;
        rc = RegQueryValueExA(k, name, NULL, &t, v, &n);
        if (rc == ERROR_MORE_DATA)
            continue;
        if (rc != ERROR_SUCCESS) {
            free(v);
            v = NULL;
        } else {
            v[n] = v[n + 1] = '\0';     /* a string is terminated whatever it holds */
        }
        break;
    }
    if (v) {
        if (type) *type = t;
        if (len) *len = n;
    }
    return v;
}

void *kitty_winkey_value_dup(HKEY root, const char *path, const char *name,
                             REGSAM view, DWORD *type, DWORD *len)
{
    HKEY k;
    void *v;
    if (wk_open(root, path, KEY_QUERY_VALUE, view, &k) != ERROR_SUCCESS)
        return NULL;
    v = kitty_winkey_handle_value_dup(k, name, type, len);
    RegCloseKey(k);
    return v;
}

char *kitty_winkey_str_dup(HKEY root, const char *path, const char *name,
                           REGSAM view, DWORD *type)
{
    DWORD t = 0;
    char *v = kitty_winkey_value_dup(root, path, name, view, &t, NULL);
    if (v && t != REG_SZ && t != REG_EXPAND_SZ) {
        free(v);
        v = NULL;
    }
    if (v && type)
        *type = t;
    return v;
}

int kitty_winkey_dword(HKEY root, const char *path, const char *name,
                       REGSAM view, DWORD *out)
{
    DWORD t = 0, n = 0;
    unsigned char *v = kitty_winkey_value_dup(root, path, name, view, &t, &n);
    int ok = v && t == REG_DWORD && n == sizeof(DWORD);
    if (ok)
        memcpy(out, v, sizeof(DWORD));
    free(v);
    return ok;
}

int kitty_winkey_exists(HKEY root, const char *path, REGSAM view)
{
    HKEY k;
    if (wk_open(root, path, KEY_READ, view, &k) != ERROR_SUCCESS)
        return 0;
    RegCloseKey(k);
    return 1;
}

int kitty_winkey_set(HKEY root, const char *path, const char *name,
                     REGSAM view, DWORD type, const void *data, DWORD len)
{
    HKEY k;
    LONG rc = RegCreateKeyExA(root, path, 0, NULL, 0, KEY_SET_VALUE | view,
                              NULL, &k, NULL);
    if (rc == ERROR_SUCCESS) {
        rc = RegSetValueExA(k, name, 0, type, (const BYTE *)data, len);
        RegCloseKey(k);
    }
    if (rc != ERROR_SUCCESS) {
        SetLastError((DWORD)rc);
        return 0;
    }
    return 1;
}

int kitty_winkey_set_str(HKEY root, const char *path, const char *name,
                         REGSAM view, const char *value)
{
    return kitty_winkey_set(root, path, name, view, REG_SZ, value,
                            (DWORD)strlen(value) + 1);
}

int kitty_winkey_set_dword(HKEY root, const char *path, const char *name,
                           REGSAM view, DWORD value)
{
    return kitty_winkey_set(root, path, name, view, REG_DWORD, &value,
                            sizeof(value));
}

int kitty_winkey_delete_value(HKEY root, const char *path, const char *name,
                              REGSAM view)
{
    HKEY k;
    LONG rc = wk_open(root, path, KEY_SET_VALUE, view, &k);
    if (rc == ERROR_FILE_NOT_FOUND)
        return 1;
    if (rc == ERROR_SUCCESS) {
        rc = RegDeleteValueA(k, name);
        RegCloseKey(k);
    }
    if (rc != ERROR_SUCCESS && rc != ERROR_FILE_NOT_FOUND) {
        SetLastError((DWORD)rc);
        return 0;
    }
    return 1;
}

/* RegDeleteTree is Vista's: the keys below, one by one, then the key */
static LONG wk_delete_tree(HKEY root, const char *path, REGSAM view)
{
    HKEY k;
    char *name;
    DWORD n, maxlen = 0;
    LONG rc = wk_open(root, path, KEY_READ | DELETE, view, &k);
    if (rc != ERROR_SUCCESS)
        return rc;
    if (RegQueryInfoKeyA(k, NULL, NULL, NULL, NULL, &maxlen, NULL, NULL, NULL,
                         NULL, NULL, NULL) != ERROR_SUCCESS)
        maxlen = 16383;                 /* a key name's limit */
    if ((name = malloc(maxlen + 2)) == NULL) {
        RegCloseKey(k);
        return ERROR_NOT_ENOUGH_MEMORY;
    }
    for (;;) {
        n = maxlen + 1;
        if (RegEnumKeyExA(k, 0, name, &n, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;
        {
            size_t pl = strlen(path) + 1 + strlen(name) + 1;
            char *sub = malloc(pl);
            if (!sub)
                break;
            memcpy(sub, path, strlen(path));
            sub[strlen(path)] = '\\';
            strcpy(sub + strlen(path) + 1, name);
            rc = wk_delete_tree(root, sub, view);
            free(sub);
            if (rc != ERROR_SUCCESS)
                break;
        }
    }
    free(name);
    RegCloseKey(k);
    if (view) {
        /* RegDeleteKeyEx (a view's key) is XP x64's and newer: looked up,
         * never imported, so the program still loads on 32-bit XP (where
         * there is only one view) */
        typedef LONG (WINAPI *delkeyex_fn)(HKEY, LPCSTR, REGSAM, DWORD);
        static delkeyex_fn dke = NULL;
        static int looked = 0;
        if (!looked) {
            HMODULE a = GetModuleHandleA("advapi32.dll");
            dke = a ? (delkeyex_fn)(void *)GetProcAddress(a, "RegDeleteKeyExA") : NULL;
            looked = 1;
        }
        if (dke)
            return dke(root, path, view, 0);
    }
    return RegDeleteKeyA(root, path);
}

int kitty_winkey_delete_tree(HKEY root, const char *path, REGSAM view)
{
    LONG rc = wk_delete_tree(root, path, view);
    if (rc == ERROR_FILE_NOT_FOUND)
        return -1;
    if (rc != ERROR_SUCCESS) {
        SetLastError((DWORD)rc);
        return 0;
    }
    return 1;
}

int kitty_winkey_create(HKEY root, const char *path, REGSAM view)
{
    HKEY k;
    LONG rc = RegCreateKeyExA(root, path, 0, NULL, 0, KEY_QUERY_VALUE | view,
                              NULL, &k, NULL);
    if (rc != ERROR_SUCCESS) {
        SetLastError((DWORD)rc);
        return 0;
    }
    RegCloseKey(k);
    return 1;
}

int kitty_winkey_may_create(HKEY root, const char *path, REGSAM view)
{
    HKEY k;
    if (wk_open(root, path, KEY_WRITE | KEY_CREATE_SUB_KEY, view, &k) !=
        ERROR_SUCCESS)
        return 0;
    RegCloseKey(k);
    return 1;
}

int kitty_winkey_counts(HKEY root, const char *path, REGSAM view,
                        DWORD *subkeys, DWORD *values)
{
    HKEY k;
    LONG rc;
    if (wk_open(root, path, KEY_QUERY_VALUE | KEY_ENUMERATE_SUB_KEYS, view,
                &k) != ERROR_SUCCESS)
        return 0;
    rc = RegQueryInfoKeyA(k, NULL, NULL, NULL, subkeys, NULL, NULL, values,
                          NULL, NULL, NULL, NULL);
    RegCloseKey(k);
    return rc == ERROR_SUCCESS;
}

static int wk_names(HKEY root, const char *path, REGSAM view, int values,
                    int (*fn)(const char *name, void *ctx), void *ctx)
{
    HKEY k;
    DWORD maxlen = 0, i;
    char *name;
    if (wk_open(root, path, KEY_READ, view, &k) != ERROR_SUCCESS)
        return 0;
    if (RegQueryInfoKeyA(k, NULL, NULL, NULL, NULL, values ? NULL : &maxlen,
                         NULL, NULL, values ? &maxlen : NULL, NULL, NULL,
                         NULL) != ERROR_SUCCESS)
        maxlen = 16383;
    if ((name = malloc(maxlen + 2)) == NULL) {
        RegCloseKey(k);
        return 0;
    }
    for (i = 0;; i++) {
        DWORD n = maxlen + 1;
        LONG rc = values ? RegEnumValueA(k, i, name, &n, NULL, NULL, NULL, NULL)
                         : RegEnumKeyExA(k, i, name, &n, NULL, NULL, NULL, NULL);
        if (rc != ERROR_SUCCESS)
            break;
        if (!fn(name, ctx))
            break;
    }
    free(name);
    RegCloseKey(k);
    return 1;
}

int kitty_winkey_value_names(HKEY root, const char *path, REGSAM view,
                             int (*fn)(const char *name, void *ctx), void *ctx)
{
    return wk_names(root, path, view, 1, fn, ctx);
}

int kitty_winkey_subkey_names(HKEY root, const char *path, REGSAM view,
                              int (*fn)(const char *name, void *ctx), void *ctx)
{
    return wk_names(root, path, view, 0, fn, ctx);
}
