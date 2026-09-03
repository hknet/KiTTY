/*
 * kitty_storemove.c - Application > Migration > KiTTY storage
 *
 * Two actions that move a whole store between the registry and a folder:
 *
 *   kitty_make_portable_copy   the store in use -> a folder the user picks:
 *                              kitty.exe and its companions, a kitty.ini with
 *                              every global and savemode=dir, sessions, named
 *                              proxies, host keys. The store in use is left
 *                              untouched.
 *   kitty_take_folder_store    a folder store -> the store in use: sessions,
 *                              named proxies, host keys and the globals of
 *                              its kitty.ini. The folder is left as it is.
 *
 * Sessions and proxies travel through the export bundle machinery
 * (kitty_bridge.c). A bundle file is written by the same save_open_settings
 * as a folder-store session file, so on the way out the bundle is written
 * straight into <folder>\Sessions and its files lose the bundle extension;
 * wrapped with the copy's master password they ARE the copy's session files,
 * unlockable by the salt+verifier minted here (or DPAPI when the user chose
 * "this PC only"). On the way in the folder's files are staged as a bundle in
 * a temporary folder, the store's master password opens them, and the store
 * in use re-protects them.
 *
 * The no-UI cores also serve the -portablecopy / -takefolder command-line
 * switches (windows/putty.c), which is how the harness drives them.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "putty.h"
#include "kitty.h"
#include "kitty_win.h"
#include "kitty_storage.h"
#include "kitty_storemove.h"
#include "kitty_mpw.h"
#include "kitty_b64.h"
#include "kitty_proxy.h"
#include "kitty_text.h"     /* the words the boxes show */

/* kitty.c */
int WriteParameter(const char *key, const char *name, char *value);
int GetIniFileFlag(void);
/* kitty_bridge.c */
int kitty_export_all_to_dir(const char *dir, int *failOut);
int kitty_import_dir(const char *dir, int *failOut, int *proxyOut,
                     int *skippedOut, int overwrite);
int kitty_ask_store_password(HWND hwnd, char **pwOut, int *dpapiOut);
int kitty_unlock_import_bundle(HWND hwnd, const char *dir, char **pwOut);
int kitty_bundle_needs_password(const char *dir);
/* kitty_storage.c */
void kitty_set_bundle_import(int on);
void kitty_set_bundle_passphrase(const char *pass);
void kitty_set_bundle_dpapi_only(int on);
void kitty_clear_bundle_context(void);
int  kitty_bundle_wrap_failed(void);
const char *kitty_mpw_verify_token(void);

#define KSM_INI       "kitty.ini"
/* the bundle extension follows the store's fileextension setting */
#define KSM_BUNDLE_EXT (FileExtension[0] ? FileExtension : ".ktx")

/* Keys that describe WHERE a store lives, never what is in it: they are
 * generated for the copy (savemode=dir) or belong to the store in use. */
static int ksm_layout_key(const char *name)
{
    static const char *const skip[] = {
        "savemode", "configdir", "browsedirectory", "KiClassName", "sav",
        "conf", "readonly", "PortablePasswordProtection", NULL
    };
    int i;
    for (i = 0; skip[i]; i++)
        if (!_stricmp(name, skip[i])) return 1;
    return 0;
}

/* kitty.ini sections that live in the file only, whatever the save mode. */
static const char *const ksm_file_sections[] = {
    "ConfigBox", "Shortcuts", "Print", "Launcher", "Agent", "FontFallback", NULL
};

static void ksm_wipe(char **p)
{
    if (*p) { SecureZeroMemory(*p, strlen(*p)); free(*p); *p = NULL; }
}

static int ksm_dir_exists(const char *path)
{
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static int ksm_file_exists(const char *path)
{
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static void ksm_trim_slashes(char *dir)
{
    while (strlen(dir) > 3 && dir[strlen(dir) - 1] == '\\')
        dir[strlen(dir) - 1] = '\0';
}

/* Writability is PROBED with a real file, never judged by the path: a
 * per-user install under Program Files is writable, a synced folder with a
 * deny ACL is not, and only the file system knows which is which. */
static int ksm_probe_writable(const char *dir)
{
    char *path = dupprintf("%s\\kitty-write-probe-%lu.tmp", dir,
                           (unsigned long)GetCurrentProcessId());
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                           NULL);
    sfree(path);
    if (h == INVALID_HANDLE_VALUE) return 0;
    CloseHandle(h);
    return 1;
}

/* One text value as a store file: value + newline, the format
 * portable_write_text_file uses for the store in use. */
static int ksm_write_text(const char *dir, const char *name, const char *val)
{
    char *m = ksf_munge(name);
    char *path = dupprintf("%s\\%s", dir, m);
    FILE *fp = fopen(path, "wb");
    int ok = 0;
    if (fp) {
        fputs(val ? val : "", fp);
        fputc('\n', fp);
        ok = (fclose(fp) == 0);
    }
    sfree(path);
    sfree(m);
    return ok;
}

static char *ksm_read_text(const char *path)          /* snewn'd or NULL */
{
    FILE *fp = fopen(path, "rb");
    long len;
    char *buf;
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) || (len = ftell(fp)) < 0 ||
        fseek(fp, 0, SEEK_SET)) { fclose(fp); return NULL; }
    buf = snewn(len + 1, char);
    if (fread(buf, 1, len, fp) != (size_t)len) { sfree(buf); buf = NULL; }
    else {
        while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r')) len--;
        buf[len] = '\0';
    }
    fclose(fp);
    return buf;
}

/* Every plain file of a folder, through a callback. */
typedef void (*ksm_file_fn)(const char *dir, const char *name, void *ctx);
static void ksm_each_file(const char *dir, ksm_file_fn fn, void *ctx)
{
    char *pat = dupprintf("%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            fn(dir, fd.cFileName, ctx);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    sfree(pat);
}

static void ksm_delete_file_cb(const char *dir, const char *name, void *ctx)
{
    char *p = dupprintf("%s\\%s", dir, name);
    (void)ctx;
    DeleteFileA(p);
    sfree(p);
}

/* ---- host keys ------------------------------------------------------- */

/* Registry hive in use -> <dst>\SshHostKeys\<munged name>. */
static int ksm_hostkeys_to_dir(const char *dst, int *fail)
{
    HKEY k;
    char *dir = dupprintf("%s\\SshHostKeys", dst);
    int n = 0;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, kitty_reg_hostkeys(), 0,
                      KEY_READ, &k) == ERROR_SUCCESS) {
        DWORD i = 0;
        CreateDirectoryA(dir, NULL);
        for (;;) {
            char name[16384];
            DWORD nlen = sizeof(name), type, vlen = 0;
            LONG r = RegEnumValueA(k, i, name, &nlen, NULL, &type, NULL, &vlen);
            if (r == ERROR_NO_MORE_ITEMS) break;
            i++;
            if (r != ERROR_SUCCESS || type != REG_SZ) continue;
            {
                char *val = snewn(vlen + 1, char);
                DWORD vl2 = vlen;
                if (RegQueryValueExA(k, name, NULL, NULL, (BYTE *)val, &vl2)
                        == ERROR_SUCCESS) {
                    val[vl2] = '\0';
                    if (ksm_write_text(dir, name, val)) n++; else (*fail)++;
                } else (*fail)++;
                sfree(val);
            }
        }
        RegCloseKey(k);
    }
    sfree(dir);
    return n;
}

/* Folder store in use: <root>\SshHostKeys\ files copied as they are. */
static void ksm_copy_file_cb(const char *dir, const char *name, void *ctx)
{
    struct { const char *to; int *n; int *fail; } *c = ctx;
    char *a = dupprintf("%s\\%s", dir, name);
    char *b = dupprintf("%s\\%s", c->to, name);
    if (CopyFileA(a, b, FALSE)) (*c->n)++; else (*c->fail)++;
    sfree(a);
    sfree(b);
}
static int ksm_copy_dir_files(const char *from, const char *to, int *fail)
{
    struct { const char *to; int *n; int *fail; } c;
    int n = 0;
    c.to = to; c.n = &n; c.fail = fail;
    if (ksm_dir_exists(from)) {
        CreateDirectoryA(to, NULL);
        ksm_each_file(from, ksm_copy_file_cb, &c);
    }
    return n;
}

/* <src>\SshHostKeys\* -> values under the hive in use. */
static int ksm_hostkeys_from_dir(const char *src, int *fail)
{
    char *dir = dupprintf("%s\\SshHostKeys", src);
    char *pat = dupprintf("%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    HKEY k = NULL;
    int n = 0;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            char *path, *val, *name;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            path = dupprintf("%s\\%s", dir, fd.cFileName);
            val = ksm_read_text(path);
            sfree(path);
            if (!val) { (*fail)++; continue; }
            if (!k && RegCreateKeyExA(HKEY_CURRENT_USER, kitty_reg_hostkeys(),
                                      0, NULL, 0, KEY_WRITE, NULL, &k, NULL)
                          != ERROR_SUCCESS) {
                k = NULL; (*fail)++; sfree(val); continue;
            }
            name = ksf_unmunge(fd.cFileName);
            if (RegSetValueExA(k, name, 0, REG_SZ, (const BYTE *)val,
                               (DWORD)strlen(val) + 1) == ERROR_SUCCESS) n++;
            else (*fail)++;
            sfree(name);
            sfree(val);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    if (k) RegCloseKey(k);
    sfree(pat);
    sfree(dir);
    return n;
}

/* ---- globals --------------------------------------------------------- */

/* Every "key=value" pair of one section of an ini file, through a callback.
 * GetPrivateProfileSection hands the section over as a double-NUL list. */
typedef void (*ksm_pair_fn)(const char *key, const char *val, void *ctx);
static void ksm_walk_section(const char *ini, const char *section,
                             ksm_pair_fn fn, void *ctx)
{
    DWORD cap = 65536;
    char *buf = snewn(cap, char), *p;
    DWORD got = GetPrivateProfileSectionA(section, buf, cap, ini);
    for (p = buf; got && *p; p += strlen(p) + 1) {
        char *eq = strchr(p, '=');
        if (!eq || p[0] == ';' || p[0] == '#') continue;
        *eq = '\0';
        fn(p, eq + 1, ctx);
        *eq = '=';
    }
    sfree(buf);
}

struct ksm_ini_copy {
    const char *dst, *section;
    int *n, *fail, skip_layout, only_missing;
};
static void ksm_copy_pair(const char *key, const char *val, void *vctx)
{
    struct ksm_ini_copy *c = (struct ksm_ini_copy *)vctx;
    char have[4];
    if (c->skip_layout && ksm_layout_key(key)) return;
    if (c->only_missing &&
        GetPrivateProfileStringA(c->section, key, "", have, sizeof(have),
                                 c->dst) > 0)
        return;                                 /* the registry value won */
    if (WritePrivateProfileStringA(c->section, key, val, c->dst)) (*c->n)++;
    else (*c->fail)++;
}

/* Registry hive in use + kitty.ini in use -> <dst ini>. The registry wins
 * where both hold a key, as it does for the running copy (ReadParameterN).
 * In folder mode the store in use IS a kitty.ini, so its [KiTTY] section is
 * copied as a whole. */
static int ksm_globals_to_ini(const char *dst, int *fail)
{
    int n = 0, i;
    const char *cur = GetKittyIniFile();
    struct ksm_ini_copy c = { dst, INIT_SECTION, &n, fail, 1, 0 };
    if (GetIniFileFlag() != SAVEMODE_DIR) {
        HKEY k;
        if (RegOpenKeyExA(HKEY_CURRENT_USER, kitty_registry_base(), 0,
                          KEY_READ, &k) == ERROR_SUCCESS) {
            DWORD i2 = 0;
            for (;;) {
                char name[1024], val[8192];
                DWORD nlen = sizeof(name), vlen = sizeof(val), type;
                LONG r = RegEnumValueA(k, i2, name, &nlen, NULL, &type,
                                       (BYTE *)val, &vlen);
                if (r == ERROR_NO_MORE_ITEMS) break;
                i2++;
                if (r != ERROR_SUCCESS || type != REG_SZ) continue;
                val[vlen < sizeof(val) ? vlen : sizeof(val) - 1] = '\0';
                if (ksm_layout_key(name)) continue;
                if (WritePrivateProfileStringA(INIT_SECTION, name, val, dst)) n++;
                else (*fail)++;
            }
            RegCloseKey(k);
        }
        c.only_missing = 1;
    }
    if (cur && cur[0] && ksm_file_exists(cur)) {
        ksm_walk_section(cur, INIT_SECTION, ksm_copy_pair, &c);
        c.only_missing = 0;
        c.skip_layout = 0;
        for (i = 0; ksm_file_sections[i]; i++) {
            c.section = ksm_file_sections[i];
            ksm_walk_section(cur, c.section, ksm_copy_pair, &c);
        }
    }
    return n;
}

struct ksm_param_copy { int *n; int *fail; };
static void ksm_param_pair(const char *key, const char *val, void *vctx)
{
    struct ksm_param_copy *c = (struct ksm_param_copy *)vctx;
    char v[8192];
    if (ksm_layout_key(key)) return;
    strncpy(v, val, sizeof(v) - 1);
    v[sizeof(v) - 1] = '\0';
    if (WriteParameter(INIT_SECTION, key, v)) (*c->n)++;
    else (*c->fail)++;
}

/* <src ini> -> the store in use: [KiTTY] via WriteParameter (the hive in
 * registry mode), the file-only sections into the kitty.ini in use. */
static int ksm_globals_from_ini(const char *src, int *fail)
{
    int n = 0, i;
    const char *cur = GetKittyIniFile();
    struct ksm_param_copy pc = { &n, fail };
    struct ksm_ini_copy fc = { cur, NULL, &n, fail, 0, 0 };
    ksm_walk_section(src, INIT_SECTION, ksm_param_pair, &pc);
    if (cur && cur[0]) {
        for (i = 0; ksm_file_sections[i]; i++) {
            fc.section = ksm_file_sections[i];
            ksm_walk_section(src, fc.section, ksm_copy_pair, &fc);
        }
    }
    return n;
}

/* ---- program files --------------------------------------------------- */

static int ksm_copy_programs(const char *dst, int *fail)
{
    static const char *const companions[] = {
        "kageant.exe", "kittygen.exe", "kittygen-cli.exe", "klink.exe",
        "kscp.exe", "ksftp.exe", "kitty.chm", NULL
    };
    char exe[MAX_PATH], *bs;
    int n = 0, i;
    if (!GetModuleFileNameA(NULL, exe, sizeof(exe))) { (*fail)++; return 0; }
    bs = strrchr(exe, '\\');
    if (!bs) { (*fail)++; return 0; }
    {
        char *to = dupprintf("%s\\%s", dst, bs + 1);
        if (CopyFileA(exe, to, FALSE)) n++; else (*fail)++;
        sfree(to);
    }
    *bs = '\0';
    for (i = 0; companions[i]; i++) {
        char *from = dupprintf("%s\\%s", exe, companions[i]);
        char *to = dupprintf("%s\\%s", dst, companions[i]);
        if (ksm_file_exists(from)) {
            if (CopyFileA(from, to, FALSE)) n++; else (*fail)++;
        }
        sfree(from);
        sfree(to);
    }
    return n;
}

/* ---- bundle <-> folder-store layout ---------------------------------- */

/* The bundle just written into <dir>\Sessions: strip the bundle extension so
 * each file is a session of the copy, and lift Proxies\ one level up, where
 * the folder store keeps them. */
static void ksm_strip_ext_cb(const char *dir, const char *name, void *ctx)
{
    size_t l = strlen(name), e = strlen(KSM_BUNDLE_EXT);
    (void)ctx;
    if (l > e && !_stricmp(name + l - e, KSM_BUNDLE_EXT)) {
        char *a = dupprintf("%s\\%s", dir, name);
        char *b = dupprintf("%s\\%.*s", dir, (int)(l - e), name);
        MoveFileExA(a, b, MOVEFILE_REPLACE_EXISTING);
        sfree(a);
        sfree(b);
    }
}
static void ksm_move_file_cb(const char *dir, const char *name, void *ctx)
{
    const char *to = (const char *)ctx;
    char *a = dupprintf("%s\\%s", dir, name);
    char *b = dupprintf("%s\\%s", to, name);
    MoveFileExA(a, b, MOVEFILE_REPLACE_EXISTING);
    sfree(a);
    sfree(b);
}
static void ksm_bundle_to_store_layout(const char *dir)
{
    char *sess = dupprintf("%s\\Sessions", dir);
    char *from = dupprintf("%s\\Sessions\\Proxies", dir);
    char *to = dupprintf("%s\\Proxies", dir);
    ksm_each_file(sess, ksm_strip_ext_cb, NULL);
    if (ksm_dir_exists(from)) {
        CreateDirectoryA(to, NULL);
        ksm_each_file(from, ksm_move_file_cb, to);
        RemoveDirectoryA(from);
    }
    sfree(to);
    sfree(from);
    sfree(sess);
}

/* The reverse: stage the folder store as a bundle in a temporary folder -
 * sessions with the bundle extension, proxies beneath them. */
static void ksm_stage_session_cb(const char *dir, const char *name, void *ctx)
{
    const char *to = (const char *)ctx;
    char *a = dupprintf("%s\\%s", dir, name);
    char *b = dupprintf("%s\\%s%s", to, name, KSM_BUNDLE_EXT);
    CopyFileA(a, b, FALSE);
    sfree(a);
    sfree(b);
}
static char *ksm_stage_folder_store(const char *dir)      /* snewn'd or NULL */
{
    char tmp[MAX_PATH];
    char *stage, *sess, *prox, *sprox;
    if (!GetTempPathA(sizeof(tmp), tmp)) return NULL;
    ksm_trim_slashes(tmp);
    stage = dupprintf("%s\\kitty-take-%lu", tmp,
                      (unsigned long)GetCurrentProcessId());
    sprox = dupprintf("%s\\Proxies", stage);
    if (!CreateDirectoryA(stage, NULL) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        sfree(sprox); sfree(stage); return NULL;
    }
    sess = dupprintf("%s\\Sessions", dir);
    prox = dupprintf("%s\\Proxies", dir);
    if (ksm_dir_exists(sess)) ksm_each_file(sess, ksm_stage_session_cb, stage);
    if (ksm_dir_exists(prox)) {
        int f = 0;
        ksm_copy_dir_files(prox, sprox, &f);
    }
    sfree(prox);
    sfree(sess);
    sfree(sprox);
    return stage;
}
static void ksm_unstage(char *stage)
{
    char *sprox;
    if (!stage) return;
    sprox = dupprintf("%s\\Proxies", stage);
    ksm_each_file(sprox, ksm_delete_file_cb, NULL);
    RemoveDirectoryA(sprox);
    ksm_each_file(stage, ksm_delete_file_cb, NULL);
    RemoveDirectoryA(stage);
    sfree(sprox);
    sfree(stage);
}

/* Salt + verifier for the copy, so that it unlocks with the password its
 * sessions were wrapped with - and asks for it only from then on. */
static int ksm_mint_master_state(const char *dir, const char *pass)
{
    unsigned char salt[KITTY_MPW_SALT_LEN], key[KITTY_MPW_DERIVED_LEN];
    char *sec = dupprintf("%s\\Security", dir);
    char *sb, *vb;
    int ok = 0;
    CreateDirectoryA(sec, NULL);
    kitty_mpw_random_salt(salt, sizeof(salt));
    kitty_mpw_derive(pass, salt, sizeof(salt), key);
    sb = ksec_b64_encode(salt, sizeof(salt));
    vb = kitty_mpw_protect(kitty_mpw_verify_token(), key);
    if (sb && vb)
        ok = ksm_write_text(sec, "MasterPwSalt", sb) &&
             ksm_write_text(sec, "MasterPwVerifier", vb);
    if (sb) free(sb);
    if (vb) sfree(vb);
    SecureZeroMemory(key, sizeof(key));
    sfree(sec);
    return ok;
}

/* ---- the cores ------------------------------------------------------- */

static void ksm_cat(char *msg, size_t len, const char *s)
{
    strncat(msg, s, len - strlen(msg) - 1);
}

int kitty_portable_copy_core(const char *dir, const char *pw, int dpapi,
                             struct ksm_result *r, char *msg, size_t msglen)
{
    char *sessdir = dupprintf("%s\\Sessions", dir);
    char *ini = dupprintf("%s\\" KSM_INI, dir);
    char t[256];
    memset(r, 0, sizeof(*r));
    msg[0] = '\0';

    /* sessions + named proxies, re-protected for the copy */
    CreateDirectoryA(sessdir, NULL);
    if (dpapi) kitty_set_bundle_dpapi_only(1);
    else kitty_set_bundle_passphrase(pw);
    r->sessions = kitty_export_all_to_dir(sessdir, &r->fail);
    if (kitty_bundle_wrap_failed()) r->fail++;
    kitty_clear_bundle_context();
    ksm_bundle_to_store_layout(dir);
    {
        char *prox = dupprintf("%s\\Proxies", dir);
        struct { int n; } c = { 0 };
        WIN32_FIND_DATAA fd;
        char *pat = dupprintf("%s\\*", prox);
        HANDLE h = FindFirstFileA(pat, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) c.n++;
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        r->proxies = c.n;
        sfree(pat);
        sfree(prox);
    }

    /* host keys: registry values, or the folder store's files */
    if (GetIniFileFlag() != SAVEMODE_DIR) {
        r->hostkeys = ksm_hostkeys_to_dir(dir, &r->fail);
    } else {
        char *root = portable_root_dir();
        if (root) {
            char *from = dupprintf("%s\\SshHostKeys", root);
            char *to = dupprintf("%s\\SshHostKeys", dir);
            r->hostkeys = ksm_copy_dir_files(from, to, &r->fail);
            sfree(to);
            sfree(from);
            sfree(root);
        }
    }

    /* the copy's own master-password state */
    if (!dpapi && !ksm_mint_master_state(dir, pw)) r->fail++;

    /* kitty.ini: every global, then the layout of the copy */
    r->settings = ksm_globals_to_ini(ini, &r->fail);
    WritePrivateProfileStringA(INIT_SECTION, "savemode", "dir", ini);
    WritePrivateProfileStringA(INIT_SECTION, "PortablePasswordProtection",
                               dpapi ? "dpapi" : "master", ini);

    r->programs = ksm_copy_programs(dir, &r->fail);

    snprintf(msg, msglen,
             KT_STOREMOVE_COPY_DONE,
             dir, r->sessions, r->sessions == 1 ? "" : "s",
             r->proxies, r->proxies == 1 ? "y" : "ies",
             r->hostkeys, r->hostkeys == 1 ? "" : "s",
             r->settings, r->settings == 1 ? "" : "s",
             r->programs, r->programs == 1 ? "" : "s");
    if (!dpapi)
        ksm_cat(msg, msglen, KT_STOREMOVE_COPY_MPW);
    else
        ksm_cat(msg, msglen, KT_STOREMOVE_COPY_DPAPI);
    if (r->fail > 0) {
        snprintf(t, sizeof(t), KT_STOREMOVE_COPY_FAILED,
                 r->fail, r->fail == 1 ? "" : "s");
        ksm_cat(msg, msglen, t);
    }
    sfree(sessdir);
    sfree(ini);
    return r->fail;
}

int kitty_take_folder_needs_password(const char *dir)
{
    char *stage = ksm_stage_folder_store(dir);
    int need = stage ? kitty_bundle_needs_password(stage) : 0;
    ksm_unstage(stage);
    return need;
}

int kitty_take_folder_core(const char *dir, const char *pw, int overwrite,
                           struct ksm_result *r, char *msg, size_t msglen)
{
    char *ini = dupprintf("%s\\" KSM_INI, dir);
    char *stage;
    char t[256];
    memset(r, 0, sizeof(*r));
    msg[0] = '\0';

    stage = ksm_stage_folder_store(dir);
    if (!stage) {
        snprintf(msg, msglen, KT_STOREMOVE_NO_TEMP);
        sfree(ini);
        return ++r->fail;
    }
    /* Import direction: the password OPENS the files, the store in use
     * re-protects what it saves - the pattern of every other importer. */
    if (pw) {
        kitty_set_bundle_import(1);
        kitty_set_bundle_passphrase(pw);
    }
    r->sessions = kitty_import_dir(stage, &r->fail, &r->proxies,
                                   &r->skipped, overwrite);
    kitty_clear_bundle_context();
    ksm_unstage(stage);

    r->hostkeys = ksm_hostkeys_from_dir(dir, &r->fail);
    if (ksm_file_exists(ini)) r->settings = ksm_globals_from_ini(ini, &r->fail);

    snprintf(msg, msglen,
             KT_STOREMOVE_TAKEN,
             dir, r->sessions, r->sessions == 1 ? "" : "s",
             r->proxies, r->proxies == 1 ? "y" : "ies",
             r->hostkeys, r->hostkeys == 1 ? "" : "s",
             r->settings, r->settings == 1 ? "" : "s");
    if (r->skipped > 0) {
        snprintf(t, sizeof(t), KT_STOREMOVE_TAKEN_KEPT, r->skipped);
        ksm_cat(msg, msglen, t);
    }
    if (r->fail > 0) {
        snprintf(t, sizeof(t), KT_STOREMOVE_TAKEN_FAILED,
                 r->fail, r->fail == 1 ? "" : "s");
        ksm_cat(msg, msglen, t);
    }
    ksm_cat(msg, msglen, KT_STOREMOVE_TAKEN_REPROTECTED);
    sfree(ini);
    return r->fail;
}

/* ---- the two buttons ------------------------------------------------- */

void kitty_make_portable_copy(HWND hwnd)
{
    char dir[MAX_PATH];
    char *sessdir, *ini, *pw = NULL;
    int dpapi = 0;
    struct ksm_result r;
    char msg[2048];

    strcpy(dir, "");
    if (!OpenDirName(hwnd, dir) || !dir[0]) return;
    ksm_trim_slashes(dir);

    if (!ksm_probe_writable(dir)) {
        snprintf(msg, sizeof(msg),
                 KT_STOREMOVE_NOT_WRITABLE, dir);
        MessageBoxA(hwnd, msg, KT_STOREMOVE_TITLE_OUT, MB_OK | MB_ICONWARNING);
        return;
    }
    sessdir = dupprintf("%s\\Sessions", dir);
    ini = dupprintf("%s\\" KSM_INI, dir);
    if (ksm_dir_exists(sessdir) || ksm_file_exists(ini)) {
        snprintf(msg, sizeof(msg),
                 KT_STOREMOVE_ALREADY_STORE_Q, dir);
        if (MessageBoxA(hwnd, msg, KT_STOREMOVE_TITLE_OUT,
                        MB_YESNO | MB_ICONQUESTION) != IDYES) {
            sfree(sessdir); sfree(ini); return;
        }
    }
    sfree(sessdir);
    sfree(ini);
    if (!kitty_ask_store_password(hwnd, &pw, &dpapi)) return;

    kitty_portable_copy_core(dir, pw, dpapi, &r, msg, sizeof(msg));
    ksm_wipe(&pw);
    MessageBoxA(hwnd, msg, KT_STOREMOVE_TITLE_OUT,
                MB_OK | (r.fail ? MB_ICONWARNING : MB_ICONINFORMATION));
}

void kitty_take_folder_store(HWND hwnd)
{
    char dir[MAX_PATH];
    char *sessdir, *ini, *stage, *pw = NULL;
    int overwrite;
    struct ksm_result r;
    char msg[2048];

    strcpy(dir, "");
    if (!OpenDirName(hwnd, dir) || !dir[0]) return;
    ksm_trim_slashes(dir);

    sessdir = dupprintf("%s\\Sessions", dir);
    ini = dupprintf("%s\\" KSM_INI, dir);
    if (!ksm_dir_exists(sessdir) && !ksm_file_exists(ini)) {
        snprintf(msg, sizeof(msg),
                 KT_STOREMOVE_NO_STORE_PREFIX KSM_INI ".", dir);
        MessageBoxA(hwnd, msg, KT_STOREMOVE_TITLE_IN, MB_OK | MB_ICONWARNING);
        sfree(sessdir); sfree(ini); return;
    }
    sfree(sessdir);
    sfree(ini);
    {
        int a = MessageBoxA(hwnd,
            KT_STOREMOVE_TAKE_Q,
            KT_STOREMOVE_TITLE_IN, MB_YESNOCANCEL | MB_ICONQUESTION);
        if (a == IDCANCEL) return;
        overwrite = (a == IDYES);
    }

    /* The folder's master password, asked once - the bundle unlock scans the
     * staged copy, which is the shape it knows. */
    stage = ksm_stage_folder_store(dir);
    if (!stage || !kitty_unlock_import_bundle(hwnd, stage, &pw)) {
        ksm_unstage(stage);
        return;
    }
    ksm_unstage(stage);

    kitty_take_folder_core(dir, pw, overwrite, &r, msg, sizeof(msg));
    ksm_wipe(&pw);
    MessageBoxA(hwnd, msg, KT_STOREMOVE_TITLE_IN,
                MB_OK | (r.fail ? MB_ICONWARNING : MB_ICONINFORMATION));
}
