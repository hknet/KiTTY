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
 * kitty.exe takes the same walk (kitty_inifile_find, without step 3 in the
 * portable build) and then pins what it decided for its own process
 * (kitty_inifile_pin, kitty_inifile_pin_authority): the file it chose even
 * before it exists, and its savemode - the key, else its build default,
 * never the layout guess below.
 *
 * The resolved file is the authoritative settings store when its main
 * section sets savemode=file or savemode=dir, or - with no savemode key at
 * all - when a portable layout sits beside it (a Sessions\ store or a
 * KiTTYState file). The portable build writes savemode=dir on its first
 * start; the layout covers a copy whose kitty.ini has no key yet (kageant
 * started at logon before that first start) or cannot be written.
 * Otherwise (savemode=registry, or absent with no such layout, matching
 * kitty.exe's default) the registry stays authoritative and ini keys serve
 * as first-run defaults only (see kitty_inifile_registry_authoritative()).
 *
 * [KiTTY] readonly=yes in the resolved file stops every write to it from
 * here (kitty_inifile_write): kageant's settings, its start-up keys and the
 * theme preference keep a change for the running program only. kitty.exe's
 * own writers name the file themselves and check
 * kitty_inifile_write_blocked() first. Other
 * ini files - a store-move destination, kitty_windowpos.ini - are not this
 * file and are never held back.
 *
 * readonly=yes covers the whole settings store, not only this file: the
 * registry hive, the folder store and kitty.sav. kitty_store_readonly() is
 * the one check every write primitive of every program makes (the
 * registry helpers, windows/storage.c, kitty_storage.c, the window memory,
 * kageant's settings); under it a primitive does nothing and answers
 * KITTY_RO_REFUSED, or the success its silent callers expect.
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kitty_inifile.h"
#include "kitty_inikeys.h"  /* KI_*: the kitty.ini key names */

static char inifile_path[MAX_PATH + 1];
static int inifile_state = 0;          /* 0 = unresolved, 1 = found, -1 = none,
                                        * 2 = pinned by kitty.exe */
static int inifile_pinned_registry = -1;   /* -1 = not pinned */

/* The readonly answer, kept with the file it was read from and that file's
 * size and last write: checked before every store write, it costs one
 * GetFileAttributesEx while the file is unchanged, and an edited file (a
 * running kageant, kitty.exe) is read again. */
static int ro_cached = -1;             /* -1 = not read */
static char ro_path[MAX_PATH + 1];
static FILETIME ro_time;
static DWORD ro_size_lo, ro_size_hi;
static int ro_forced = 0;              /* kitty_inifile_readonly_force */
static LONG ro_allow = 0;              /* kitty_store_ro_allow nesting */
static char ro_regbase[300];           /* kitty_store_ro_set_regbase */

/* readonly=yes: what kitty_inifile_write would have written, held for the
 * running program - kitty_inifile_read answers from it first, so a change
 * made in a settings dialog (kageant's, the theme) applies until the
 * program exits. value NULL: the key was removed. */
struct inifile_ro_value {
    char *section, *key, *value;
    struct inifile_ro_value *next;
};
static struct inifile_ro_value *inifile_ro_values = NULL;

static struct inifile_ro_value *inifile_ro_find(const char *section,
                                                const char *key)
{
    struct inifile_ro_value *v;
    for (v = inifile_ro_values; v; v = v->next)
        if (!stricmp(v->section, section) && !stricmp(v->key, key))
            return v;
    return NULL;
}

static void inifile_ro_keep(const char *section, const char *key,
                            const char *value)
{
    struct inifile_ro_value *v = inifile_ro_find(section, key);
    char *copy = value ? _strdup(value) : NULL;
    if (value && !copy)
        return;
    if (!v) {
        v = calloc(1, sizeof(*v));
        if (v) {
            v->section = _strdup(section);
            v->key = _strdup(key);
        }
        if (!v || !v->section || !v->key) {
            if (v) {
                free(v->section);
                free(v->key);
                free(v);
            }
            free(copy);
            return;
        }
        v->next = inifile_ro_values;
        inifile_ro_values = v;
    }
    free(v->value);
    v->value = copy;
}

static int inifile_isfile(const char *p)
{
    DWORD a = GetFileAttributesA(p);
    return (a != INVALID_FILE_ATTRIBUTES) && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/* The search, the one list of places: the first existing file into out.
 * skip_appdata leaves %APPDATA%\KiTTY\kitty.ini out (the portable kitty.exe
 * never reads an installed copy's file). 1 = found, 0 = none. */
int kitty_inifile_find(int skip_appdata, char *out, int outlen)
{
    char exedir[MAX_PATH + 1], cand[MAX_PATH + 32];
    const char *env;
    char *slash;
    DWORD n;

    if (outlen > 0)
        out[0] = '\0';
    env = getenv("KITTY_INI_FILE");
    if (env && *env && strlen(env) <= MAX_PATH && inifile_isfile(env)) {
        snprintf(out, outlen, "%s", env);
        return 1;
    }

    n = GetModuleFileNameA(NULL, exedir, MAX_PATH);
    if (n > 0 && n < MAX_PATH && (slash = strrchr(exedir, '\\')) != NULL) {
        *slash = '\0';
        snprintf(cand, sizeof(cand), "%s\\kitty.ini", exedir);
        if (inifile_isfile(cand) && strlen(cand) <= MAX_PATH) {
            snprintf(out, outlen, "%s", cand);
            return 1;
        }
    }

    env = getenv("APPDATA");
    if (!skip_appdata && env && *env) {
        snprintf(cand, sizeof(cand), "%s\\KiTTY\\kitty.ini", env);
        if (inifile_isfile(cand) && strlen(cand) <= MAX_PATH) {
            snprintf(out, outlen, "%s", cand);
            return 1;
        }
        /* No %APPDATA%\PuTTY\putty.ini: kitty.exe never read one there, and
         * a helper that did found a file its kitty.exe ignores. */
    }
    return 0;
}

static void inifile_resolve(void)
{
    if (inifile_state)
        return;
    inifile_state = kitty_inifile_find(0, inifile_path, sizeof(inifile_path)) ? 1 : -1;
}

/* kitty.exe: the file it reads and writes, chosen before it may exist (a
 * kitty.ini it creates later - the template, the portable savemode key - is
 * then found by every reader in the process). A path longer than MAX_PATH is
 * not pinned: the resolver's own answer stays. */
void kitty_inifile_pin(const char *path)
{
    if (!path || !path[0] || strlen(path) > MAX_PATH)
        return;
    strcpy(inifile_path, path);
    inifile_state = 2;
    ro_cached = -1;                    /* another file: read it again */
    while (inifile_ro_values) {        /* ...and the changes kept from it go */
        struct inifile_ro_value *v = inifile_ro_values;
        inifile_ro_values = v->next;
        free(v->section);
        free(v->key);
        free(v->value);
        free(v);
    }
}

/* kitty.exe: whether its own store is the registry (savemode=registry, or
 * no key in the registry build) - the key and the build default, without
 * the layout guess the helpers need. */
void kitty_inifile_pin_authority(int registry)
{
    inifile_pinned_registry = registry ? 1 : 0;
}

/* Absolute path of the resolved ini, or NULL when none was found. A pinned
 * path counts while the file exists. */
const char *kitty_inifile_file(void)
{
    inifile_resolve();
    if (inifile_state == 2)
        return inifile_isfile(inifile_path) ? inifile_path : NULL;
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
 * was found, or the resolved ini neither sets savemode=file/dir nor sits
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
    if (inifile_pinned_registry >= 0)
        return inifile_pinned_registry;
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
    const struct inifile_ro_value *ro;
    if (!f || len <= 0)
        return 0;
    value[0] = '\0';
    /* a change readonly=yes kept from the file (kitty_inifile_write), while
     * the file still sets it */
    if (inifile_ro_values && (ro = inifile_ro_find(section, key)) != NULL &&
        kitty_inifile_readonly()) {
        snprintf(value, len, "%s", ro->value ? ro->value : "");
        return value[0] != '\0';
    }
    GetPrivateProfileStringA(section, key, "", value, len, f);
    return value[0] != '\0';
}

/* [KiTTY] readonly=yes in the resolved kitty.ini, matched as kitty.exe
 * matches it (kitty_params.c load_ini_params: the value "yes", case aside).
 * Read again whenever the file changed (its size or last write), so a
 * running kageant sees an edited file; kitty.exe's GetReadOnlyFlag() is
 * this answer (kitty_common.c). */
static int inifile_readonly_read(void)
{
    char buf[16];
    const char *f = kitty_inifile_file();
    /* from the file itself, past the changes readonly=yes keeps
     * (kitty_inifile_read reaches this function for those) */
    buf[0] = '\0';
    if (f)
        GetPrivateProfileStringA(KI_SECTION_KITTY, KI_READONLY, "", buf,
                                 sizeof(buf), f);
    return !stricmp(buf, "YES");
}

int kitty_inifile_readonly(void)
{
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (ro_forced)
        return 1;
    /* kitty_inifile_file()'s answer, with the one stat the cache needs */
    inifile_resolve();
    if (inifile_state != 1 && inifile_state != 2)
        return 0;
    if (!GetFileAttributesExA(inifile_path, GetFileExInfoStandard, &fa) ||
        (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return 0;                       /* no file: nothing reads readonly=yes */
    if (ro_cached >= 0 && !stricmp(ro_path, inifile_path) &&
        CompareFileTime(&fa.ftLastWriteTime, &ro_time) == 0 &&
        fa.nFileSizeLow == ro_size_lo && fa.nFileSizeHigh == ro_size_hi)
        return ro_cached;
    ro_cached = inifile_readonly_read();
    snprintf(ro_path, sizeof(ro_path), "%s", inifile_path);
    ro_time = fa.ftLastWriteTime;
    ro_size_lo = fa.nFileSizeLow;
    ro_size_hi = fa.nFileSizeHigh;
    return ro_cached;
}

/* The cached answer is dropped: the next question reads the file. */
void kitty_inifile_readonly_reread(void)
{
    ro_cached = -1;
}

/* kitty.exe: readonly set by another source than the file (a readonly
 * value its ReadParameter finds in the hive). Only ever switched on. */
void kitty_inifile_readonly_force(int on)
{
    if (on)
        ro_forced = 1;
}

/* Is a store write refused now? readonly=yes, outside a
 * kitty_store_ro_allow() scope. */
int kitty_store_readonly(void)
{
    return ro_allow <= 0 && kitty_inifile_readonly();
}

/* savemode=file only: kitty.sav is loaded into the registry working copy
 * (the hive parked first) under readonly too - that copy is what the
 * program reads. Nested calls; 1 opens a scope, 0 closes it. */
void kitty_store_ro_allow(int on)
{
    if (on)
        InterlockedIncrement(&ro_allow);
    else
        InterlockedDecrement(&ro_allow);
}

/* The hive in use when it is not KiTTY++'s own (KiClassName=PuTTY):
 * kitty_set_registry_root hands it over. */
void kitty_store_ro_set_regbase(const char *base)
{
    snprintf(ro_regbase, sizeof(ro_regbase), "%s", base ? base : "");
}

/* subkey inside base, or a key above it (a tree delete there takes the
 * store with it) */
static int regpath_touches(const char *subkey, const char *base)
{
    size_t nb = strlen(base), ns = strlen(subkey);
    if (!nb || !ns)
        return 0;
    if (!strnicmp(subkey, base, nb))
        return 1;
    return ns < nb && !strnicmp(subkey, base, ns) && base[ns] == '\\';
}

/* 1 when a registry write to HKEY_CURRENT_USER\subkey must be refused:
 * subkey is in the settings store - KiTTY++'s own hive (sessions, kageant,
 * window memory, theme; the parked "<base>_save" included) or the hive in
 * use, or a key above one of them - and the store is read-only. Any other
 * key (the Run key, file associations, PuTTY's hive when it is not in use)
 * is never held back. */
int kitty_store_ro_regpath(int hkcu, const char *subkey)
{
    if (!hkcu || !subkey)
        return 0;
    while (*subkey == '\\')
        subkey++;
    if (!regpath_touches(subkey, "Software\\kapper.net\\KiTTY") &&
        !regpath_touches(subkey, ro_regbase))
        return 0;
    return kitty_store_readonly();
}

/* The kitty.ini a refusal under readonly=yes names: the resolved file, else
 * the plain name (readonly forced from the hive, no file found). */
const char *kitty_store_ro_inifile(void)
{
    const char *f = kitty_inifile_file();
    return f ? f : "kitty.ini";
}

/* Two names of one file: the full paths compared, case aside. */
static int inifile_same_file(const char *a, const char *b)
{
    char fa[MAX_PATH + 1], fb[MAX_PATH + 1];
    DWORD na = GetFullPathNameA(a, sizeof(fa), fa, NULL);
    DWORD nb = GetFullPathNameA(b, sizeof(fb), fb, NULL);
    if (na == 0 || na >= sizeof(fa) || nb == 0 || nb >= sizeof(fb))
        return !stricmp(a, b);
    return !stricmp(fa, fb);
}

/* 1 when a write to path must be skipped: path is the resolved kitty.ini
 * and that sets readonly=yes. For the writers that name the file
 * themselves (kitty.exe's writeINI / delINI); any other file is never
 * held back by this. */
int kitty_inifile_write_blocked(const char *path)
{
    const char *f;
    if (!path || !path[0])
        return 0;
    f = kitty_inifile_file();
    return f && inifile_same_file(path, f) && kitty_inifile_readonly();
}

/* Write [section] key=value; a NULL value removes the key. Returns 1 on
 * success, 0 when no ini was resolved or the file is not writable (the
 * reason in GetLastError()). With readonly=yes the file is left as it is
 * and 1 is returned, as kitty.exe's write_parameter skips it: the change
 * holds for the running program only (kitty_inifile_read answers it), and
 * no "not stored" report follows. */
int kitty_inifile_write(const char *section, const char *key,
                         const char *value)
{
    const char *f = kitty_inifile_file();
    if (!f)
        return 0;
    if (kitty_inifile_readonly()) {
        inifile_ro_keep(section, key, value);
        return 1;
    }
    return WritePrivateProfileStringA(section, key, value, f) ? 1 : 0;
}

/* The folder store, as kitty.exe finds it (kitty_store.c loadPath and
 * kitty_store_ini_takeover). It is in use when the ini sets savemode=dir, or
 * sets no savemode at all beside a portable layout (a portable copy before
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

/* NOTE: this is a SECOND copy of kitty.exe's rules (kitty_store.c loadPath,
 * kitty_store_ini_takeover, kitty.c SetConfigDirectory, kitty_storage.c
 * portable_root_dir) for the programs that do not link the store - kageant
 * and the command-line tools. A change to where kitty.exe puts the session
 * folder, the configuration directory, the host keys or KiTTYState has to
 * be made here too. */
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
        if (v[0] && v[0] != '\\' && v[0] != '/' && v[1] != ':') {
            snprintf(x, sizeof(x), "%s\\%s", exedir, v);   /* below the exe's folder */
            snprintf(v, sizeof(v), "%s", x);
        }
        /* A configdir that is not there counts as not set, as in kitty.exe
         * once its question is answered (kitty_params.c): the sessions
         * beside the exe, not a folder of nothing. */
        if (v[0]) {
            DWORD a = GetFileAttributesA(v);
            if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY))
                v[0] = '\0';
        }
        if (!v[0])
            snprintf(path, sizeof(path), "%s\\Sessions", exedir);
        else
            snprintf(path, sizeof(path), "%s\\Sessions", v);
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

/* The folder store's state file KiTTYState, where kitty.exe keeps it
 * (kitty_storage.c portable_root_dir): beside the session folder when that
 * is called Sessions, else in the session folder itself. 0 when the
 * sessions are not in the folder store - savemode=file included, whose
 * state values are in the registry. */
int kitty_inifile_state_file(char *out, int outlen)
{
    char dir[KITTY_INIFILE_PATHLEN], suffix[80];
    char *bs;
    if (!inifile_folder_store(dir, sizeof(dir), suffix, sizeof(suffix)))
        return 0;
    bs = strrchr(dir, '\\');
    if (bs && !stricmp(bs + 1, "Sessions"))
        *bs = '\0';
    if ((int)strlen(dir) + 12 > outlen)
        return 0;
    snprintf(out, outlen, "%s\\KiTTYState", dir);
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
