/*
 * kitty_userpath.c: the System panel's "Add this KiTTY++ folder to the user
 * PATH" checkbox. The user's PATH lives in HKCU\Environment\Path; the exe's
 * folder is appended or removed there and running shells are told
 * (WM_SETTINGCHANGE "Environment"), which is what a newly opened shell
 * reads. Nothing is stored anywhere else: the checkbox shows the state of
 * that value, so a folder that moved shows as unchecked from its new place.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "putty.h"

bool kitty_userpath_contains_exe_dir(void);
bool kitty_userpath_set_exe_dir(bool on, char **err);

static void exe_dir(char *buf, size_t size)
{
    char *slash;
    buf[0] = '\0';
    if (!GetModuleFileNameA(NULL, buf, (DWORD)size))
        return;
    slash = strrchr(buf, '\\');
    if (slash && slash != buf)
        *slash = '\0';
}

/* Trailing backslashes off, for the comparison. */
static void trim_dir(char *s)
{
    size_t n = strlen(s);
    while (n > 3 && s[n - 1] == '\\')
        s[--n] = '\0';
}

/* The raw (unexpanded) user PATH and its registry type; "" when unset. */
static char *read_user_path(DWORD *type)
{
    HKEY key;
    char *value = NULL;
    *type = REG_EXPAND_SZ;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Environment", 0, KEY_READ, &key) == ERROR_SUCCESS) {
        DWORD len = 0;
        if (RegQueryValueExA(key, "Path", NULL, type, NULL, &len) == ERROR_SUCCESS && len > 0) {
            value = snewn(len + 1, char);
            if (RegQueryValueExA(key, "Path", NULL, type, (BYTE *)value, &len) == ERROR_SUCCESS)
                value[len] = '\0';
            else { sfree(value); value = NULL; }
        }
        RegCloseKey(key);
    }
    return value ? value : dupstr("");
}

/* Does one PATH entry name the exe's folder? */
static bool entry_is_dir(const char *entry, size_t len, const char *dir)
{
    char *e = snewn(len + 1, char);
    bool same;
    memcpy(e, entry, len); e[len] = '\0';
    while (*e == ' ') memmove(e, e + 1, strlen(e));
    trim_dir(e);
    same = !stricmp(e, dir);
    sfree(e);
    return same;
}

bool kitty_userpath_contains_exe_dir(void)
{
    char dir[MAX_PATH];
    DWORD type;
    char *path, *p;
    bool found = false;
    exe_dir(dir, sizeof(dir));
    trim_dir(dir);
    if (!dir[0]) return false;
    path = read_user_path(&type);
    for (p = path; *p; ) {
        char *semi = strchr(p, ';');
        size_t len = semi ? (size_t)(semi - p) : strlen(p);
        if (len && entry_is_dir(p, len, dir)) { found = true; break; }
        p += len + (semi ? 1 : 0);
    }
    sfree(path);
    return found;
}

bool kitty_userpath_set_exe_dir(bool on, char **err)
{
    char dir[MAX_PATH];
    DWORD type;
    char *path, *p;
    strbuf *out;
    HKEY key;
    LONG rc;

    *err = NULL;
    exe_dir(dir, sizeof(dir));
    trim_dir(dir);
    if (!dir[0]) { *err = dupstr("the program's own folder could not be determined"); return false; }
    path = read_user_path(&type);
    out = strbuf_new();
    for (p = path; *p; ) {
        char *semi = strchr(p, ';');
        size_t len = semi ? (size_t)(semi - p) : strlen(p);
        if (len && !entry_is_dir(p, len, dir)) {    /* keep everything but us */
            if (out->len) put_byte(out, ';');
            put_data(out, p, len);
        }
        p += len + (semi ? 1 : 0);
    }
    if (on) {
        if (out->len) put_byte(out, ';');
        put_dataz(out, dir);
    }
    sfree(path);
    if (type != REG_SZ && type != REG_EXPAND_SZ)
        type = REG_EXPAND_SZ;
    rc = RegCreateKeyExA(HKEY_CURRENT_USER, "Environment", 0, NULL, 0,
                         KEY_SET_VALUE, NULL, &key, NULL);
    if (rc == ERROR_SUCCESS) {
        rc = RegSetValueExA(key, "Path", 0, type, (const BYTE *)out->s, (DWORD)out->len + 1);
        RegCloseKey(key);
    }
    strbuf_free(out);
    if (rc != ERROR_SUCCESS) {
        *err = dupprintf("HKEY_CURRENT_USER\\Environment\\Path could not be written (error %ld)", rc);
        return false;
    }
    /* Tell the shells and Explorer; a hung window must not hold us. */
    SendMessageTimeoutA(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)"Environment",
                        SMTO_ABORTIFHUNG, 2000, NULL);
    return true;
}
