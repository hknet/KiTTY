/*
 * kitty_inifile.c: the suite's kitty.ini resolver - which file, and which
 * store it puts in charge. Dependency-free
 * (the Win32 profile API directly), so every program links it: kageant,
 * kittygen, klink/kscp/ksftp (kitty_console_store.c), the theme preference.
 * Formerly kitty_inilight.c: no longer a "light" reader beside kitty.exe's,
 * but the one the other programs take their store from.
 *
 * Search order - kitty.exe's own (kitty_startup.c), the EXE's directory,
 * never the process cwd (kageant is commonly autostarted with cwd=system32):
 *   1. %KITTY_INI_FILE%
 *   2. <exedir>\kitty.ini
 *   3. %APPDATA%\KiTTY\kitty.ini
 * No putty.ini anywhere: kitty.exe read only [KiTTY] from one, so a classic
 * [PuTTY] file did nothing there, and the name is retired for the suite.
 * The main section is always [KiTTY] (KI_SECTION_KITTY).
 *
 * The resolved file is the authoritative settings store when its main
 * section says savemode=file or savemode=dir, or - with no savemode key at
 * all - when a portable layout sits beside it (a Sessions\ store or a
 * KiTTYState file). The portable build writes savemode=dir on its first
 * start; the layout covers a copy whose kitty.ini has no key yet (kageant
 * started at logon before that first start) or cannot be written.
 * Otherwise (savemode=registry, or absent with no such layout, matching
 * kitty.exe's default) the registry stays authoritative and ini keys serve
 * as first-run defaults only (see kitty_inifile_registry_authoritative()).
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kitty_inifile.h"
#include "kitty_inikeys.h"  /* KI_*: the kitty.ini key names */

static char inifile_path[MAX_PATH + 1];
static int inifile_state = 0;          /* 0 = unresolved, 1 = found, -1 = none */

static int inifile_isfile(const char *p)
{
    DWORD a = GetFileAttributesA(p);
    return (a != INVALID_FILE_ATTRIBUTES) && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static void inifile_resolve(void)
{
    char exedir[MAX_PATH + 1], cand[MAX_PATH + 32];
    const char *env;
    char *slash;
    DWORD n;

    if (inifile_state)
        return;
    inifile_state = -1;
    inifile_path[0] = '\0';

    env = getenv("KITTY_INI_FILE");
    if (env && *env && strlen(env) <= MAX_PATH && inifile_isfile(env)) {
        strcpy(inifile_path, env);
        inifile_state = 1;
        return;
    }

    n = GetModuleFileNameA(NULL, exedir, MAX_PATH);
    if (n > 0 && n < MAX_PATH && (slash = strrchr(exedir, '\\')) != NULL) {
        *slash = '\0';
        snprintf(cand, sizeof(cand), "%s\\kitty.ini", exedir);
        if (inifile_isfile(cand) && strlen(cand) <= MAX_PATH) {
            strcpy(inifile_path, cand);
            inifile_state = 1;
            return;
        }
    }

    env = getenv("APPDATA");
    if (env && *env) {
        snprintf(cand, sizeof(cand), "%s\\KiTTY\\kitty.ini", env);
        if (inifile_isfile(cand) && strlen(cand) <= MAX_PATH) {
            strcpy(inifile_path, cand);
            inifile_state = 1;
            return;
        }
        /* No %APPDATA%\PuTTY\putty.ini: kitty.exe never read one there, and
         * a helper that did found a file its kitty.exe ignores. */
    }
}

/* Absolute path of the resolved ini, or NULL when none was found. */
const char *kitty_inifile_file(void)
{
    inifile_resolve();
    return (inifile_state == 1) ? inifile_path : NULL;
}

/* An unambiguous file/dir-backed layout beside the resolved ini: the
 * dir-mode Sessions\ store or the portable build's KiTTYState file.
 * kitty.sav is deliberately NOT a signal - /savereg drops one next to the
 * exe as a backup while staying in registry mode. */
static int inifile_portable_layout(void)
{
    char dir[MAX_PATH + 1], cand[MAX_PATH + 32];
    char *slash, *s2;
    DWORD a;
    strcpy(dir, inifile_path);
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
 * selected - except beside a portable layout: a portable copy whose
 * kitty.exe has not written its savemode=dir yet, or cannot. */
int kitty_inifile_portable(void)
{
    return inifile_portable_layout();
}

int kitty_inifile_registry_authoritative(void)
{
    char buf[32];
    if (!kitty_inifile_file())
        return 1;
    GetPrivateProfileStringA(KI_SECTION_KITTY, KI_SAVEMODE, "",
                             buf, sizeof(buf), inifile_path);
    if (!stricmp(buf, "file") || !stricmp(buf, "dir"))
        return 0;
    if (!stricmp(buf, "registry"))
        return 1;
    return !inifile_portable_layout();
}

/* Read [section] key. Returns 1 with the value copied when the key is
 * present and non-empty, 0 otherwise (an empty value counts as absent). */
int kitty_inifile_read(const char *section, const char *key,
                        char *value, int len)
{
    const char *f = kitty_inifile_file();
    if (!f)
        return 0;
    value[0] = '\0';
    GetPrivateProfileStringA(section, key, "", value, len, f);
    return value[0] != '\0';
}

/* Write [section] key=value. Returns 1 on success, 0 when no ini was
 * resolved or the file is not writable (caller falls back to the registry). */
int kitty_inifile_write(const char *section, const char *key,
                         const char *value)
{
    const char *f = kitty_inifile_file();
    if (!f)
        return 0;
    return WritePrivateProfileStringA(section, key, value, f) ? 1 : 0;
}

/* The folder store, as kitty.exe finds it (kitty_store.c loadPath and
 * kitty_store_ini_takeover). It is in use when the ini says savemode=dir, or
 * says no savemode at all beside a portable layout (a portable copy before
 * kitty.exe wrote the key); savemode=file keeps its sessions in the
 * registry. The session directory is sessions= (environment strings
 * expanded; a leading '\' or '/' is below the program folder, "X:..." is
 * absolute, anything else is relative to the program folder), else
 * <configdir>\Sessions, else <program folder>\Sessions. The suffix is
 * fileextension= (a dot put in front if missing, as old KiTTY read it), else
 * 0.85.1.13's sessionsuffix= not yet copied over by kitty.exe, unless it
 * holds a character a file name cannot end in.
 * Classic KiTTY's putty.conf is not read: kitty.exe moves its keys into
 * kitty.ini on its first folder-store start. Returns 1 with dir and suffix
 * filled, 0 when the sessions are in the registry. */
static void inifile_rtrim(char *s, const char *set)
{
    size_t n = strlen(s);
    while (n > 0 && strchr(set, s[n - 1]))
        s[--n] = '\0';
}

static int inifile_folder_store(char *dir, int dirlen, char *suffix, int suflen)
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
    if (!kitty_inifile_file())
        return 0;
    GetPrivateProfileStringA(KI_SECTION_KITTY, KI_SAVEMODE, "",
                             mode, sizeof(mode), inifile_path);
    inifile_rtrim(mode, " \t\r\n");
    if (stricmp(mode, "dir") && (mode[0] || !inifile_portable_layout()))
        return 0;

    n = GetModuleFileNameA(NULL, exedir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH || !(slash = strrchr(exedir, '\\')))
        return 0;
    *slash = '\0';

    GetPrivateProfileStringA(KI_SECTION_KITTY, KI_SESSIONS, "",
                             v, sizeof(v), inifile_path);
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
        inifile_rtrim(path, " \n\r\t\\");
        if (strlen(path) == 2 && path[1] == ':')
            strcat(path, "\\");          /* "C:" alone is drive C's cwd */
    } else {
        GetPrivateProfileStringA(KI_SECTION_KITTY, KI_CONFIGDIR, "",
                                 v, sizeof(v), inifile_path);
        inifile_rtrim(v, " \n\r\t\\");
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
    GetPrivateProfileStringA(KI_SECTION_KITTY, KI_FILEEXTENSION, "",
                             v, sizeof(v), inifile_path);
    inifile_rtrim(v, " \n\r\t");
    if (!v[0])
        GetPrivateProfileStringA(KI_SECTION_KITTY, KI_SESSIONSUFFIX, "",
                                 v, sizeof(v), inifile_path);
    kitty_ext_dot(v, sizeof(v));
    for (p = v; *p; p++)
        if ((unsigned char)*p < 0x20 || strchr("\\/:*?\"<>|", *p))
            return 1;
    if (strlen(v) > 63)
        v[63] = '\0';
    if ((int)strlen(v) < suflen)
        strcpy(suffix, v);
    return 1;
}

/* The folder store's host keys, as kitty.exe sets them up
 * (kitty_store_ini_takeover): sshhostkeys= resolved like sessions= above
 * (empty = the store's own SshHostKeys folder, which the storage layer
 * finds by itself), and hostkeyextension= - else 0.85.1.13's keysuffix= -
 * with a dot put in front. Meaningful only after inifile_folder_store()
 * returned 1; returns 0 when no ini was resolved. */
static int inifile_hostkey_store(char *dir, int dirlen, char *suffix, int suflen)
{
    char v[2 * MAX_PATH], x[2 * MAX_PATH], exedir[MAX_PATH + 1];
    char path[4 * MAX_PATH + 8];
    char *slash;
    DWORD n;

    if (dirlen > 0)
        dir[0] = '\0';
    if (suflen > 0)
        suffix[0] = '\0';
    if (!kitty_inifile_file())
        return 0;

    GetPrivateProfileStringA(KI_SECTION_KITTY, KI_SSHHOSTKEYS, "",
                             v, sizeof(v), inifile_path);
    inifile_rtrim(v, " \n\r\t");
    n = GetModuleFileNameA(NULL, exedir, MAX_PATH);
    if (v[0] && n > 0 && n < MAX_PATH && (slash = strrchr(exedir, '\\'))) {
        DWORD xn;
        *slash = '\0';
        xn = ExpandEnvironmentStringsA(v, x, sizeof(x));
        if (xn == 0 || xn > sizeof(x))
            snprintf(x, sizeof(x), "%s", v);
        if (x[0] == '\\' || x[0] == '/')
            snprintf(path, sizeof(path), "%s%s", exedir, x);
        else if (x[0] && x[1] == ':')
            snprintf(path, sizeof(path), "%s", x);
        else
            snprintf(path, sizeof(path), "%s\\%s", exedir, x);
        inifile_rtrim(path, " \n\r\t\\");
        if ((int)strlen(path) < dirlen)
            strcpy(dir, path);
    }

    GetPrivateProfileStringA(KI_SECTION_KITTY, KI_HOSTKEYEXTENSION, "",
                             v, sizeof(v), inifile_path);
    inifile_rtrim(v, " \n\r\t");
    if (!v[0])
        GetPrivateProfileStringA(KI_SECTION_KITTY, KI_KEYSUFFIX, "",
                                 v, sizeof(v), inifile_path);
    kitty_ext_dot(v, sizeof(v));
    if ((int)strlen(v) < suflen)
        strcpy(suffix, v);     /* kitty_set_hostkey_suffix refuses a bad one */
    return 1;
}

void kitty_inifile_startup(struct kitty_ini_startup *s)
{
    memset(s, 0, sizeof(*s));
    s->registry_authoritative = kitty_inifile_registry_authoritative();
    s->folder_store = inifile_folder_store(s->session_dir, sizeof(s->session_dir),
                                           s->session_suffix,
                                           sizeof(s->session_suffix));
    if (!s->folder_store)
        return;
    inifile_hostkey_store(s->hostkey_dir, sizeof(s->hostkey_dir),
                          s->hostkey_suffix, sizeof(s->hostkey_suffix));
    kitty_inifile_read(KI_SECTION_KITTY, KI_PORTABLEPASSWORDPROTECTION,
                       s->portable_password_protection,
                       sizeof(s->portable_password_protection));
}
