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
int kitty_inilight_registry_authoritative(void)
{
    char buf[32];
    if (!kitty_inilight_file())
        return 1;
    GetPrivateProfileStringA(inilight_mainsection, "savemode", "",
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
