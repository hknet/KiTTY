/*
 * kitty_inilight.c: minimal, dependency-free resolver for the suite's
 * kitty.ini / putty.ini, for satellite binaries (kageant today; the
 * klink/kscp/ksftp tools are candidates later) that must not link the full
 * kitty.exe ini machinery. Uses the Win32 profile API directly.
 *
 * Search order (the suite's canonical one, with one deliberate difference:
 * "running directory" means the EXE's directory, not the process cwd -
 * kageant is commonly autostarted with cwd=system32):
 *   1. %KITTY_INI_FILE%
 *   2. <exedir>\kitty.ini
 *   3. <exedir>\putty.ini
 *   4. %APPDATA%\KiTTY\kitty.ini
 *   5. %APPDATA%\PuTTY\putty.ini
 *
 * The resolved file is the authoritative settings store when its main
 * section says savemode=file or savemode=dir, or - with no savemode key at
 * all - when a portable layout sits beside it (a Sessions\ store or a
 * KiTTYState file; portable builds force dir mode without writing the key).
 * Otherwise (savemode=registry, or absent with no such layout, matching
 * kitty.exe's default) the registry stays authoritative and ini keys serve
 * as first-run defaults only (see kitty_inilight_registry_authoritative()).
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kitty_inilight.h"
#include "kitty_inikeys.h"  /* KI_*: the kitty.ini key names */

static char inilight_path[MAX_PATH + 1];
static char inilight_mainsection[8];    /* "KiTTY" or "PuTTY" */
static int inilight_state = 0;          /* 0 = unresolved, 1 = found, -1 = none */

static int inilight_isfile(const char *p)
{
    DWORD a = GetFileAttributesA(p);
    return (a != INVALID_FILE_ATTRIBUTES) && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static void inilight_resolve(void)
{
    char exedir[MAX_PATH + 1], cand[MAX_PATH + 32];
    const char *env;
    char *slash;
    DWORD n;

    if (inilight_state)
        return;
    inilight_state = -1;
    inilight_path[0] = '\0';
    strcpy(inilight_mainsection, "KiTTY");

    env = getenv("KITTY_INI_FILE");
    if (env && *env && strlen(env) <= MAX_PATH && inilight_isfile(env)) {
        strcpy(inilight_path, env);
        inilight_state = 1;
        return;
    }

    n = GetModuleFileNameA(NULL, exedir, MAX_PATH);
    if (n > 0 && n < MAX_PATH && (slash = strrchr(exedir, '\\')) != NULL) {
        *slash = '\0';
        snprintf(cand, sizeof(cand), "%s\\kitty.ini", exedir);
        if (inilight_isfile(cand) && strlen(cand) <= MAX_PATH) {
            strcpy(inilight_path, cand);
            inilight_state = 1;
            return;
        }
        snprintf(cand, sizeof(cand), "%s\\putty.ini", exedir);
        if (inilight_isfile(cand) && strlen(cand) <= MAX_PATH) {
            strcpy(inilight_path, cand);
            strcpy(inilight_mainsection, "PuTTY");
            inilight_state = 1;
            return;
        }
    }

    env = getenv("APPDATA");
    if (env && *env) {
        snprintf(cand, sizeof(cand), "%s\\KiTTY\\kitty.ini", env);
        if (inilight_isfile(cand) && strlen(cand) <= MAX_PATH) {
            strcpy(inilight_path, cand);
            inilight_state = 1;
            return;
        }
        snprintf(cand, sizeof(cand), "%s\\PuTTY\\putty.ini", env);
        if (inilight_isfile(cand) && strlen(cand) <= MAX_PATH) {
            strcpy(inilight_path, cand);
            strcpy(inilight_mainsection, "PuTTY");
            inilight_state = 1;
            return;
        }
    }
}

/* Absolute path of the resolved ini, or NULL when none was found. */
const char *kitty_inilight_file(void)
{
    inilight_resolve();
    return (inilight_state == 1) ? inilight_path : NULL;
}

/* An unambiguous file/dir-backed layout beside the resolved ini: the
 * dir-mode Sessions\ store or the portable build's KiTTYState file.
 * kitty.sav is deliberately NOT a signal - /savereg drops one next to the
 * exe as a backup while staying in registry mode. */
static int inilight_portable_layout(void)
{
    char dir[MAX_PATH + 1], cand[MAX_PATH + 32];
    char *slash, *s2;
    DWORD a;
    strcpy(dir, inilight_path);
    slash = strrchr(dir, '\\');
    s2 = strrchr(dir, '/');
    if (s2 > slash)
        slash = s2;
    if (!slash)
        return 0;
    *slash = '\0';
    snprintf(cand, sizeof(cand), "%s\\Sessions", dir);
    a = GetFileAttributesA(cand);
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY))
        return 1;
    snprintf(cand, sizeof(cand), "%s\\KiTTYState", dir);
    a = GetFileAttributesA(cand);
    if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY))
        return 1;
    return 0;
}

/* 1 when the registry must stay the authoritative settings store: no ini
 * was found, or the resolved ini neither says savemode=file/dir nor sits
 * in a recognisable portable layout. An absent savemode key means registry
 * mode, exactly as in kitty.exe - registry-mode installs routinely carry an
 * auto-created kitty.ini whose savemode key was deleted when the mode was
 * selected - except that portable builds force dir mode without ever
 * writing a savemode key, so their on-disk layout counts as evidence. */
int kitty_inilight_portable(void)
{
    return inilight_portable_layout();
}

int kitty_inilight_registry_authoritative(void)
{
    char buf[32];
    if (!kitty_inilight_file())
        return 1;
    GetPrivateProfileStringA(inilight_mainsection, KI_SAVEMODE, "",
                             buf, sizeof(buf), inilight_path);
    if (!stricmp(buf, "file") || !stricmp(buf, "dir"))
        return 0;
    if (!stricmp(buf, "registry"))
        return 1;
    return !inilight_portable_layout();
}

/* Read [section] key. Returns 1 with the value copied when the key is
 * present and non-empty, 0 otherwise (an empty value counts as absent). */
int kitty_inilight_read(const char *section, const char *key,
                        char *value, int len)
{
    const char *f = kitty_inilight_file();
    if (!f)
        return 0;
    value[0] = '\0';
    GetPrivateProfileStringA(section, key, "", value, len, f);
    return value[0] != '\0';
}

/* Write [section] key=value. Returns 1 on success, 0 when no ini was
 * resolved or the file is not writable (caller falls back to the registry). */
int kitty_inilight_write(const char *section, const char *key,
                         const char *value)
{
    const char *f = kitty_inilight_file();
    if (!f)
        return 0;
    return WritePrivateProfileStringA(section, key, value, f) ? 1 : 0;
}

/* The folder store, as kitty.exe finds it (kitty_store.c loadPath and
 * kitty_store_ini_takeover). It is in use when the ini says savemode=dir, or
 * says no savemode at all beside a portable layout (the portable build forces
 * dir mode without the key); savemode=file keeps its sessions in the
 * registry. The session directory is sessions= (environment strings
 * expanded; a leading '\' or '/' is below the program folder, "X:..." is
 * absolute, anything else is relative to the program folder), else
 * <configdir>\Sessions, else <program folder>\Sessions. The suffix is
 * sessionsuffix= unless it holds a character a file name cannot end in.
 * Classic KiTTY's putty.conf is not read: kitty.exe moves its keys into
 * kitty.ini on its first folder-store start. Returns 1 with dir and suffix
 * filled, 0 when the sessions are in the registry. */
static void inilight_rtrim(char *s, const char *set)
{
    size_t n = strlen(s);
    while (n > 0 && strchr(set, s[n - 1]))
        s[--n] = '\0';
}

int kitty_inilight_folder_store(char *dir, int dirlen, char *suffix, int suflen)
{
    char mode[32], v[2 * MAX_PATH], x[2 * MAX_PATH], exedir[MAX_PATH + 1];
    char path[4 * MAX_PATH + 8];
    char *slash;
    DWORD n;
    const char *p;

    if (dirlen > 0)
        dir[0] = '\0';
    if (suflen > 0)
        suffix[0] = '\0';
    if (!kitty_inilight_file())
        return 0;
    GetPrivateProfileStringA(inilight_mainsection, KI_SAVEMODE, "",
                             mode, sizeof(mode), inilight_path);
    inilight_rtrim(mode, " \t\r\n");
    if (stricmp(mode, "dir") && (mode[0] || !inilight_portable_layout()))
        return 0;

    n = GetModuleFileNameA(NULL, exedir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH || !(slash = strrchr(exedir, '\\')))
        return 0;
    *slash = '\0';

    GetPrivateProfileStringA(inilight_mainsection, KI_SESSIONS, "",
                             v, sizeof(v), inilight_path);
    if (v[0]) {
        DWORD xn = ExpandEnvironmentStringsA(v, x, sizeof(x));
        if (xn == 0 || xn > sizeof(x))
            snprintf(x, sizeof(x), "%s", v);
        if (x[0] == '\\' || x[0] == '/')
            snprintf(path, sizeof(path), "%s%s", exedir, x);
        else if (x[0] && x[1] == ':')
            snprintf(path, sizeof(path), "%s", x);
        else
            snprintf(path, sizeof(path), "%s\\%s", exedir, x);
        inilight_rtrim(path, " \n\r\t\\");
        if (strlen(path) == 2 && path[1] == ':')
            strcat(path, "\\");          /* "C:" alone is drive C's cwd */
    } else {
        GetPrivateProfileStringA(inilight_mainsection, KI_CONFIGDIR, "",
                                 v, sizeof(v), inilight_path);
        inilight_rtrim(v, " \n\r\t\\");
        if (!v[0])
            snprintf(path, sizeof(path), "%s\\Sessions", exedir);
        else if (v[0] == '\\' || v[1] == ':')
            snprintf(path, sizeof(path), "%s\\Sessions", v);
        else
            snprintf(path, sizeof(path), "%s\\%s\\Sessions", exedir, v);
    }
    if ((int)strlen(path) >= dirlen)
        return 0;
    strcpy(dir, path);

    /* kitty.exe keeps at most 63 characters and refuses a suffix with a
     * character its escape would rewrite (kitty_set_session_suffix). */
    GetPrivateProfileStringA(inilight_mainsection, KI_SESSIONSUFFIX, "",
                             v, sizeof(v), inilight_path);
    inilight_rtrim(v, " \n\r\t");
    for (p = v; *p; p++)
        if ((unsigned char)*p < 0x20 || strchr("\\/:*?\"<>|", *p))
            return 1;
    if (strlen(v) > 63)
        v[63] = '\0';
    if ((int)strlen(v) < suflen)
        strcpy(suffix, v);
    return 1;
}
