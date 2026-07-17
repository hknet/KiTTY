/*
 * kitty_pageant.c: the KiTTY additions to pageant (kageant), split out of
 * windows/pageant.c to keep that file textually close to upstream PuTTY.
 * Bodies are moved verbatim from pageant.c (cross-TU functions de-static'd);
 * see kitty_pageant.h for the interface and the banner comments below for
 * what each group does.
 */

#include <stdio.h>
#include <stdlib.h>

#include "putty.h"
#include "pageant.h"

#include <shellapi.h>

#include "kitty_pageant.h"
#include "kitty_inilight.h"
#include "ssh.h"

/* Shim so the moved kageant_do_notify body below stays textually identical
 * to its pageant.c original: reach pageant.c's static tray-window handle
 * through the accessor it exports for us. */
#define traywindow (kageant_traywindow())

/* ------------------------------------------------------------------ *
 * KiTTY: optional integration with the Windows OpenSSH client.        *
 *                                                                     *
 * OFF BY DEFAULT. When the user ticks the tray item "Register as      *
 * Windows OpenSSH agent" (persisted in the registry), kageant:        *
 *   - writes %USERPROFILE%\.ssh\kageant.conf with an IdentityAgent    *
 *     line pointing at its named pipe, and                            *
 *   - adds a marker-delimited managed block to %USERPROFILE%\.ssh\    *
 *     config that `Include`s kageant.conf,                            *
 * so the Windows ssh.exe uses kageant as its agent. Disabling removes *
 * the managed block and empties kageant.conf. Only the marker block   *
 * is ever touched - the rest of the user's config is preserved        *
 * byte-for-byte, written atomically (temp + rename), and a one-time   *
 * config.kageant.bak backup is taken before the first edit.           *
 * ------------------------------------------------------------------ */
#define KAGEANT_REG_BASE   "Software\\kapper.net\\KiTTY"  /* consolidated KiTTY hive */
#define KAGEANT_REG_VALUE  "OpenSSHIntegration"
#define KAGEANT_SSH_BEGIN  "# >>> kageant managed (KiTTY) >>>"
#define KAGEANT_SSH_END    "# <<< kageant managed (KiTTY) <<<"

int kageant_openssh_get(void)
{
    DWORD val = 0, sz = sizeof(val);
    if (RegGetValueA(HKEY_CURRENT_USER, KAGEANT_REG_BASE, KAGEANT_REG_VALUE,
                     RRF_RT_REG_DWORD, NULL, &val, &sz) != ERROR_SUCCESS)
        return 0;
    return val ? 1 : 0;
}

void kageant_openssh_set(int on)
{
    HKEY hk;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, KAGEANT_REG_BASE, 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &hk, NULL) == ERROR_SUCCESS) {
        DWORD val = on ? 1 : 0;
        RegSetValueExA(hk, KAGEANT_REG_VALUE, 0, REG_DWORD,
                       (const BYTE *)&val, sizeof(val));
        RegCloseKey(hk);
    }
}

/* Return malloc'd "%USERPROFILE%\.ssh\<leaf>", or NULL if no USERPROFILE. */
char *kageant_ssh_path(const char *leaf)
{
    const char *home = getenv("USERPROFILE");
    if (!home || !*home)
        return NULL;
    return dupprintf("%s\\.ssh\\%s", home, leaf);
}

/* Write the IdentityAgent line for `pipename` (with / separators) to fp.
 * Shared by the -openssh-config flag path and the tray-toggle path. */
void kageant_write_identityagent(FILE *fp, const char *pipename)
{
    fputs("IdentityAgent \"", fp);
    /* Some Windows OpenSSH versions prefer / to \; none object to /. */
    for (const char *p = pipename; *p; p++)
        fputc(*p == '\\' ? '/' : *p, fp);
    fputs("\"\n", fp);
}

/* Read a whole file into a malloc'd NUL-terminated buffer (binary), or NULL. */
static char *kageant_read_file(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return NULL;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    if (n < 0) { fclose(fp); return NULL; }
    fseek(fp, 0, SEEK_SET);
    char *buf = snewn(n + 1, char);
    size_t got = fread(buf, 1, n, fp);
    fclose(fp);
    buf[got] = '\0';
    return buf;
}

/* Atomically replace `path` contents with `data` (temp file + rename). */
static int kageant_write_file_atomic(const char *path, const char *data)
{
    char *tmp = dupprintf("%s.kageant.tmp", path);
    int ok = 0;
    FILE *fp = fopen(tmp, "wb");
    if (fp) {
        ok = (fputs(data, fp) >= 0);
        if (fclose(fp) != 0) ok = 0;
    }
    if (ok)
        ok = MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING) ? 1 : 0;
    if (!ok)
        remove(tmp);
    sfree(tmp);
    return ok;
}

/* One-time backup of config before kageant first edits it (never clobbers). */
static void kageant_backup_config_once(const char *cfgpath)
{
    char *bak = dupprintf("%s.kageant.bak", cfgpath);
    CopyFileA(cfgpath, bak, TRUE);   /* bFailIfExists = TRUE */
    sfree(bak);
}

/* Apply (on=1) or remove (on=0) the OpenSSH integration. Touches only our
 * managed block in ~/.ssh/config; the rest is preserved byte-for-byte. */
void kageant_openssh_apply(int on)
{
    char *cfgpath  = kageant_ssh_path("config");
    char *confpath = kageant_ssh_path("kageant.conf");
    char *sshdir   = kageant_ssh_path("");   /* "...\.ssh\" */
    if (!cfgpath || !confpath || !sshdir) {
        sfree(cfgpath); sfree(confpath); sfree(sshdir);
        return;
    }
    { size_t L = strlen(sshdir); if (L && sshdir[L-1] == '\\') sshdir[L-1] = '\0'; }
    CreateDirectoryA(sshdir, NULL);   /* harmless if it already exists */

    if (on) {
        /* (1) (re)write kageant.conf with the current pipe name */
        char *pipename = agent_named_pipe_name();
        FILE *fp = fopen(confpath, "wb");
        if (fp) { kageant_write_identityagent(fp, pipename); fclose(fp); }
        sfree(pipename);

        /* (2) ensure ~/.ssh/config Include's it via our managed block, but
         * never add a duplicate if a block or a manual Include already exists */
        char *cfg = kageant_read_file(cfgpath);
        int present = cfg && (strstr(cfg, KAGEANT_SSH_BEGIN) ||
                              strstr(cfg, "Include kageant.conf"));
        if (!present) {
            kageant_backup_config_once(cfgpath);
            /* block ends in \n; placed before existing content with no extra
             * separator, so a later remove restores the file byte-for-byte */
            char *block = dupprintf(
                "%s\n"
                "# kageant rewrites kageant.conf on each launch; do not edit by hand.\n"
                "Include kageant.conf\n"
                "%s\n",
                KAGEANT_SSH_BEGIN, KAGEANT_SSH_END);
            char *newcfg = dupprintf("%s%s", block, (cfg && *cfg) ? cfg : "");
            kageant_write_file_atomic(cfgpath, newcfg);
            sfree(block); sfree(newcfg);
        }
        sfree(cfg);
    } else {
        /* (1) remove ONLY our managed block from ~/.ssh/config */
        char *cfg = kageant_read_file(cfgpath);
        if (cfg) {
            char *b = strstr(cfg, KAGEANT_SSH_BEGIN);
            char *e = b ? strstr(b, KAGEANT_SSH_END) : NULL;
            if (b && e) {
                kageant_backup_config_once(cfgpath);
                e += strlen(KAGEANT_SSH_END);
                if (*e == '\r') e++;
                if (*e == '\n') e++;
                size_t headlen = (size_t)(b - cfg);
                char *newcfg = snewn(headlen + strlen(e) + 1, char);
                memcpy(newcfg, cfg, headlen);
                strcpy(newcfg + headlen, e);
                kageant_write_file_atomic(cfgpath, newcfg);
                sfree(newcfg);
            }
            sfree(cfg);
        }
        /* (2) empty kageant.conf so any lingering Include is inert */
        FILE *fp = fopen(confpath, "wb");
        if (fp) fclose(fp);
    }
    sfree(cfgpath); sfree(confpath); sfree(sshdir);
}

/* ------------------------------------------------------------------ *
 * KiTTY: optional "load keys on startup" (added encrypted/deferred).   *
 *                                                                      *
 * OFF BY DEFAULT. kageant auto-tracks the file paths of keys you load  *
 * (kageant_track_keypath, from win_add_keyfile); enabling the tray     *
 * item snapshots them to the registry (REG_MULTI_SZ StartupKeys) and   *
 * installs an HKCU ...\Run entry so kageant autostarts at login -      *
 * replacing the need for a manual Startup shortcut. At startup the keys *
 * are re-added with deferred decryption (passphrase only on first use).*
 * Only key-file PATHS are stored (never passphrases/key material).     *
 * ------------------------------------------------------------------ */
#define KAGEANT_REG_STARTUP "LoadKeysOnStartup"
#define KAGEANT_REG_KEYS    "StartupKeys"
#define KAGEANT_REG_ORDER   "KeyOrder"   /* SHA256 fingerprints in offer order */
#define KAGEANT_RUN_KEY     "Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define KAGEANT_RUN_NAME    "KiTTY-kageant"

static char **g_loaded_keypaths = NULL;   /* key paths added this session */
static int   *g_loaded_encrypted = NULL;   /* per key: added encrypted/deferred */
static int    g_nloaded = 0;
static int    g_startup_loading = 0;       /* suppress re-save during startup load */
static int    g_startup_missing = 0;       /* startup keys not found at last load */

/* defined in the notify/confirm block below; used by the ini-aware
 * startup getters/setters here. */
static int kageant_reg_read(const char *name, int *val_out);
static void kageant_reg_write(const char *name, int on);

int kageant_startup_get(void)
{
    char buf[8];
    int ini_val = -1, reg_val;
    if (kitty_inilight_read("Agent", "loadonstartup", buf, sizeof(buf))) {
        if (!stricmp(buf, "yes")) ini_val = 1;
        else if (!stricmp(buf, "no")) ini_val = 0;
    }
    if (kitty_inilight_registry_authoritative())
        return kageant_reg_read(KAGEANT_REG_STARTUP, &reg_val) ? reg_val :
               (ini_val >= 0 ? ini_val : 0);
    if (ini_val >= 0)
        return ini_val;
    return kageant_reg_read(KAGEANT_REG_STARTUP, &reg_val) ? reg_val : 0;
}

void kageant_startup_set(int on)
{
    if (!kitty_inilight_registry_authoritative() &&
        kitty_inilight_write("Agent", "loadonstartup", on ? "yes" : "no"))
        return;
    kageant_reg_write(KAGEANT_REG_STARTUP, on);
}

#define KAGEANT_REG_NOTIFY "NotifyOnKeyUse"
#define KAGEANT_REG_CONFIRM "ConfirmKeyUse"

/*
 * KiTTY: the notify/confirm settings live in the registry by default, but
 * honour the classic kitty.ini [Agent] keys (hknet/KiTTY#14). When the ini
 * resolved by kitty_inilight is authoritative (found, with an explicit
 * savemode=file/dir), a present [Agent] key wins and the tray toggles write
 * back to the ini, so a portable install never touches the registry. When
 * the registry is authoritative, ini keys act as first-run defaults only.
 */

static int kageant_reg_read(const char *name, int *val_out)
{
    DWORD val, sz = sizeof(val);
    if (RegGetValueA(HKEY_CURRENT_USER, KAGEANT_REG_BASE, name,
                     RRF_RT_REG_DWORD, NULL, &val, &sz) != ERROR_SUCCESS)
        return 0;
    *val_out = val ? 1 : 0;
    return 1;
}

static void kageant_reg_write(const char *name, int on)
{
    HKEY hk;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, KAGEANT_REG_BASE, 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &hk, NULL) == ERROR_SUCCESS) {
        DWORD val = on ? 1 : 0;
        RegSetValueExA(hk, name, 0, REG_DWORD,
                       (const BYTE *)&val, sizeof(val));
        RegCloseKey(hk);
    }
}

/* KiTTY: "notify on key use" tray-balloon toggle ([Agent] messageonkeyusage).
 * Default ON (absent everywhere => on). */
int kageant_notify_get(void)
{
    char buf[32];
    int ini_val = -1, reg_val;
    if (kitty_inilight_read("Agent", "messageonkeyusage", buf, sizeof(buf))) {
        if (!stricmp(buf, "yes")) ini_val = 1;
        else if (!stricmp(buf, "no")) ini_val = 0;
    }
    if (kitty_inilight_registry_authoritative())
        return kageant_reg_read(KAGEANT_REG_NOTIFY, &reg_val) ? reg_val :
               (ini_val >= 0 ? ini_val : 1);
    if (ini_val >= 0)
        return ini_val;
    return kageant_reg_read(KAGEANT_REG_NOTIFY, &reg_val) ? reg_val : 1;
}

void kageant_notify_set(int on)
{
    if (!kitty_inilight_registry_authoritative() &&
        kitty_inilight_write("Agent", "messageonkeyusage", on ? "yes" : "no"))
        return;
    kageant_reg_write(KAGEANT_REG_NOTIFY, on);
}

/* KiTTY: key-use confirmation mode (classic [Agent] askconfirmation).
 * KAGEANT_CONFIRM_NO silences even the per-key comment prompts (automation)
 * and is expressible only in the ini; the registry DWORD and the two-state
 * tray toggle keep their historical 0=auto / 1=yes meaning. Default AUTO. */
int kageant_confirm_mode(void)
{
    char buf[32];
    int ini_mode = -1, reg_val;
    if (kitty_inilight_read("Agent", "askconfirmation", buf, sizeof(buf))) {
        if (!stricmp(buf, "yes")) ini_mode = KAGEANT_CONFIRM_YES;
        else if (!stricmp(buf, "no")) ini_mode = KAGEANT_CONFIRM_NO;
        else if (!stricmp(buf, "auto")) ini_mode = KAGEANT_CONFIRM_AUTO;
    }
    if (kitty_inilight_registry_authoritative())
        return kageant_reg_read(KAGEANT_REG_CONFIRM, &reg_val) ?
               (reg_val ? KAGEANT_CONFIRM_YES : KAGEANT_CONFIRM_AUTO) :
               (ini_mode >= 0 ? ini_mode : KAGEANT_CONFIRM_AUTO);
    if (ini_mode >= 0)
        return ini_mode;
    return kageant_reg_read(KAGEANT_REG_CONFIRM, &reg_val) ?
           (reg_val ? KAGEANT_CONFIRM_YES : KAGEANT_CONFIRM_AUTO) :
           KAGEANT_CONFIRM_AUTO;
}

/* The tray checkbox is two-state: checked = YES, unchecked = AUTO (or NO). */
int kageant_confirm_get(void)
{
    return kageant_confirm_mode() == KAGEANT_CONFIRM_YES;
}

void kageant_confirm_set(int on)
{
    if (!kitty_inilight_registry_authoritative() &&
        kitty_inilight_write("Agent", "askconfirmation", on ? "yes" : "auto"))
        return;
    kageant_reg_write(KAGEANT_REG_CONFIRM, on);
}

/* KiTTY: "kitty.ini mode" indicator for the key-list window and the tray
 * tooltip: the resolved ini path when it is the authoritative settings
 * store, NULL when the registry is. */
const char *kageant_ini_status(void)
{
    return kitty_inilight_registry_authoritative() ? NULL
                                                   : kitty_inilight_file();
}

/* Directory holding the resolved authoritative ini (no trailing separator);
 * 0 when there is none. */
static int kageant_inidir(char *out, size_t outlen)
{
    const char *f = kitty_inilight_file();
    char *slash, *s2;
    if (!f || strlen(f) >= outlen)
        return 0;
    strcpy(out, f);
    slash = strrchr(out, '\\');
    s2 = strrchr(out, '/');
    if (s2 > slash) slash = s2;
    if (!slash)
        return 0;
    *slash = '\0';
    return 1;
}

static int kageant_path_under(const char *dir, const char *path)
{
    size_t dl = strlen(dir);
    if (_strnicmp(path, dir, dl) != 0)
        return 0;
    return path[dl] == '\\' || path[dl] == '/';
}

/* On-disk form: relative to the ini folder when the key sits under it,
 * else the absolute path unchanged. */
static void kageant_store_form(const char *abspath, char *out, size_t outlen)
{
    char dir[MAX_PATH + 1];
    if (kageant_inidir(dir, sizeof(dir)) && kageant_path_under(dir, abspath)) {
        const char *rel = abspath + strlen(dir);
        while (*rel == '\\' || *rel == '/') rel++;
        snprintf(out, outlen, "%s", rel);
    } else {
        snprintf(out, outlen, "%s", abspath);
    }
}

/* Resolve a stored path (relative -> against the ini folder) to absolute. */
static void kageant_resolve_form(const char *stored, char *out, size_t outlen)
{
    char dir[MAX_PATH + 1];
    int isabs = stored[0] &&
                (stored[1] == ':' || stored[0] == '\\' || stored[0] == '/');
    if (!isabs && kageant_inidir(dir, sizeof(dir)))
        snprintf(out, outlen, "%s\\%s", dir, stored);
    else
        snprintf(out, outlen, "%s", stored);
}

/* Persist the tracked key set. Portable (ini authoritative): numbered
 * [Agent] startupkeyN entries, relative where possible, with a trailing
 * ,encrypted marker. Otherwise: the StartupKeys REG_MULTI_SZ value, each
 * entry "path,encrypted" or "path,plain". */
void kageant_save_startup_keys(void)
{
    const char *f;
    int i;

    if (!kitty_inilight_registry_authoritative() &&
        (f = kitty_inilight_file()) != NULL) {
        char key[32], val[MAX_PATH + 32], store[MAX_PATH + 1], probe[MAX_PATH + 32];
        int gap;
        for (i = 1, gap = 0; gap < 8; i++) {          /* clear the old list */
            snprintf(key, sizeof(key), "startupkey%d", i);
            GetPrivateProfileStringA("Agent", key, "", probe, sizeof(probe), f);
            if (probe[0]) { WritePrivateProfileStringA("Agent", key, NULL, f); gap = 0; }
            else gap++;
        }
        for (i = 0; i < g_nloaded; i++) {
            kageant_store_form(g_loaded_keypaths[i], store, sizeof(store));
            snprintf(key, sizeof(key), "startupkey%d", i + 1);
            snprintf(val, sizeof(val), "%s%s", store,
                     g_loaded_encrypted[i] ? ",encrypted" : "");
            WritePrivateProfileStringA("Agent", key, val, f);
        }
        return;
    }

    {
        size_t total = 1;
        char **entries = (g_nloaded ? snewn(g_nloaded, char *) : NULL);
        char *buf, *p;
        HKEY hk;
        for (i = 0; i < g_nloaded; i++) {
            entries[i] = dupprintf("%s,%s", g_loaded_keypaths[i],
                                   g_loaded_encrypted[i] ? "encrypted" : "plain");
            total += strlen(entries[i]) + 1;
        }
        buf = snewn(total, char); p = buf;
        for (i = 0; i < g_nloaded; i++) {
            size_t L = strlen(entries[i]) + 1;
            memcpy(p, entries[i], L); p += L; sfree(entries[i]);
        }
        *p = '\0';
        sfree(entries);
        if (RegCreateKeyExA(HKEY_CURRENT_USER, KAGEANT_REG_BASE, 0, NULL, 0,
                            KEY_SET_VALUE, NULL, &hk, NULL) == ERROR_SUCCESS) {
            RegSetValueExA(hk, KAGEANT_REG_KEYS, 0, REG_MULTI_SZ,
                           (const BYTE *)buf, (DWORD)total);
            RegCloseKey(hk);
        }
        sfree(buf);
    }
}

/* Best-effort "does this key need a passphrase" for the copy warning. */
static int kageant_key_needs_pass(const char *abspath)
{
    Filename *pf = filename_from_str(abspath);
    char *cmt = NULL;
    int kt = key_type(pf), needs = 1;
    if (kt == SSH_KEYTYPE_SSH2) needs = ppk_encrypted_f(pf, &cmt);
    else if (kt == SSH_KEYTYPE_SSH1) needs = rsa1_encrypted_f(pf, &cmt);
    sfree(cmt);
    filename_free(pf);
    return needs;
}

/* Auto-track a key path added this session (dedup, case-insensitive). In a
 * portable install with startup-load on, a key from outside the install
 * folder prompts to be copied in (so it travels) or referenced in place.
 * When the feature is on, persist the updated set (unless mid startup load). */
void kageant_track_keypath(const char *path, int encrypted)
{
    char abspath[MAX_PATH + 1];
    int i;
    if (!path || !*path)
        return;
    if (!_fullpath(abspath, path, sizeof(abspath)))
        snprintf(abspath, sizeof(abspath), "%s", path);
    for (i = 0; i < g_nloaded; i++)
        if (!stricmp(g_loaded_keypaths[i], abspath))
            return;

    if (!g_startup_loading && kageant_startup_get() &&
        !kitty_inilight_registry_authoritative()) {
        char dir[MAX_PATH + 1];
        if (kageant_inidir(dir, sizeof(dir)) &&
            !kageant_path_under(dir, abspath)) {
            int needs_pass = kageant_key_needs_pass(abspath);
            char *prompt = dupprintf(
                "This key is outside the portable install folder:\n\n"
                "    %s\n\n"
                "Copy it into the portable keys folder so it travels with this "
                "install, or reference it where it is (it will then load only on "
                "this machine)?%s\n\n"
                "Yes = Copy into %s\\keys\n"
                "No = Reference where it is\n"
                "Cancel = Do not add it to the startup list",
                abspath,
                needs_pass ? "" :
                "\n\nWARNING: this key has no passphrase - copying it onto "
                "portable media lets anyone holding the media use it.",
                dir);
            int choice = MessageBox(NULL, prompt,
                "kageant - add key to startup",
                MB_ICONQUESTION | MB_YESNOCANCEL | MB_DEFBUTTON1);
            sfree(prompt);
            if (choice == IDCANCEL)
                return;               /* loaded this session, not persisted */
            if (choice == IDYES) {
                char keysdir[MAX_PATH + 8], dest[MAX_PATH + 72];
                const char *base = abspath, *q;
                for (q = abspath; *q; q++)
                    if (*q == '\\' || *q == '/') base = q + 1;
                snprintf(keysdir, sizeof(keysdir), "%s\\keys", dir);
                CreateDirectoryA(keysdir, NULL);
                snprintf(dest, sizeof(dest), "%s\\%s", keysdir, base);
                if (strlen(dest) <= MAX_PATH &&
                    (CopyFileA(abspath, dest, TRUE) ||
                     GetLastError() == ERROR_FILE_EXISTS)) {
                    snprintf(abspath, sizeof(abspath), "%s", dest);
                    for (i = 0; i < g_nloaded; i++)   /* re-dedup on the copy */
                        if (!stricmp(g_loaded_keypaths[i], abspath))
                            return;
                } else {
                    MessageBox(NULL, "Could not copy the key into the portable "
                        "folder; it will be referenced at its current location "
                        "instead.", "kageant", MB_ICONWARNING | MB_OK);
                }
            }
        }
    }

    g_loaded_keypaths = sresize(g_loaded_keypaths, g_nloaded + 1, char *);
    g_loaded_encrypted = sresize(g_loaded_encrypted, g_nloaded + 1, int);
    g_loaded_keypaths[g_nloaded] = dupstr(abspath);
    g_loaded_encrypted[g_nloaded] = encrypted ? 1 : 0;
    g_nloaded++;
    if (!g_startup_loading && kageant_startup_get())
        kageant_save_startup_keys();
}

/* Add/remove the HKCU ...\Run entry that autostarts kageant at login. */
void kageant_set_run_entry(int on)
{
    HKEY hk;
    if (on) {
        char exe[MAX_PATH + 3];
        DWORD n = GetModuleFileNameA(NULL, exe + 1, MAX_PATH);
        if (!n || n >= MAX_PATH)
            return;
        exe[0] = '"'; exe[n + 1] = '"'; exe[n + 2] = '\0';
        if (RegCreateKeyExA(HKEY_CURRENT_USER, KAGEANT_RUN_KEY, 0, NULL, 0,
                            KEY_SET_VALUE, NULL, &hk, NULL) == ERROR_SUCCESS) {
            RegSetValueExA(hk, KAGEANT_RUN_NAME, 0, REG_SZ,
                           (const BYTE *)exe, (DWORD)strlen(exe) + 1);
            RegCloseKey(hk);
        }
    } else {
        if (RegOpenKeyExA(HKEY_CURRENT_USER, KAGEANT_RUN_KEY, 0,
                          KEY_SET_VALUE, &hk) == ERROR_SUCCESS) {
            RegDeleteValueA(hk, KAGEANT_RUN_NAME);
            RegCloseKey(hk);
        }
    }
}

/* Re-add remembered startup keys. A ,encrypted entry loads deferred
 * (passphrase on first use); ,plain loads immediately. Missing files are
 * counted (kageant_startup_missing) and skipped, not purged. */
void kageant_load_startup_keys(void)
{
    const char *f;
    g_startup_missing = 0;

    if (!kitty_inilight_registry_authoritative() &&
        (f = kitty_inilight_file()) != NULL) {
        char key[32], val[MAX_PATH + 32], abspath[MAX_PATH + 1];
        int i;
        g_startup_loading = 1;
        for (i = 1; ; i++) {
            int enc = 0;
            char *c;
            snprintf(key, sizeof(key), "startupkey%d", i);
            GetPrivateProfileStringA("Agent", key, "", val, sizeof(val), f);
            if (!val[0])
                break;
            c = strrchr(val, ',');
            if (c && !stricmp(c + 1, "encrypted")) { *c = '\0'; enc = 1; }
            else if (c && !stricmp(c + 1, "plain")) { *c = '\0'; enc = 0; }
            kageant_resolve_form(val, abspath, sizeof(abspath));
            if (GetFileAttributesA(abspath) == INVALID_FILE_ATTRIBUTES) {
                g_startup_missing++; continue;
            }
            Filename *fn = filename_from_str(abspath);
            win_add_keyfile(fn, enc ? true : false);
            filename_free(fn);
        }
        g_startup_loading = 0;
        return;
    }

    {
        HKEY hk;
        DWORD type = 0, sz = 0;
        if (RegOpenKeyExA(HKEY_CURRENT_USER, KAGEANT_REG_BASE, 0,
                          KEY_QUERY_VALUE, &hk) != ERROR_SUCCESS)
            return;
        if (RegQueryValueExA(hk, KAGEANT_REG_KEYS, NULL, &type, NULL, &sz)
                == ERROR_SUCCESS && type == REG_MULTI_SZ && sz > 0) {
            char *buf = snewn(sz + 1, char);
            if (RegQueryValueExA(hk, KAGEANT_REG_KEYS, NULL, NULL,
                                 (BYTE *)buf, &sz) == ERROR_SUCCESS) {
                buf[sz] = '\0';
                g_startup_loading = 1;
                for (char *p = buf; *p; p += strlen(p) + 1) {
                    int enc = 1;   /* legacy entries had no marker: deferred */
                    char *c = strrchr(p, ',');
                    if (c && !stricmp(c + 1, "encrypted")) { *c = '\0'; enc = 1; }
                    else if (c && !stricmp(c + 1, "plain")) { *c = '\0'; enc = 0; }
                    if (GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES) {
                        g_startup_missing++; continue;
                    }
                    Filename *fn = filename_from_str(p);
                    win_add_keyfile(fn, enc ? true : false);
                    filename_free(fn);
                }
                g_startup_loading = 0;
            }
            sfree(buf);
        }
        RegCloseKey(hk);
    }
}

/* Tray balloon for startup keys that could not be found (called once the
 * tray icon exists). Silent when none were missing. */
void kageant_notify_startup_missing(void)
{
    NOTIFYICONDATA nid;
    if (g_startup_missing <= 0 || !traywindow)
        return;
    memset(&nid, 0, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = traywindow;
    nid.uID = 1;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_WARNING;
    nid.uTimeout = 5000;
    snprintf(nid.szInfoTitle, sizeof(nid.szInfoTitle), "kageant: startup keys");
    snprintf(nid.szInfo, sizeof(nid.szInfo),
             "%d startup key%s could not be found and %s skipped.",
             g_startup_missing, g_startup_missing == 1 ? "" : "s",
             g_startup_missing == 1 ? "was" : "were");
    Shell_NotifyIcon(NIM_MODIFY, &nid);
}

/* KiTTY: persist the current key offer order (SHA256 fingerprints, REG_MULTI_SZ).
 * Called after the user moves a key up/down in the list window. */
void kageant_save_key_order(void)
{
    int n = 0;
    char **fps = pageant_get_order_fps(&n);
    size_t total = 1;                       /* trailing double-NUL */
    for (int i = 0; i < n; i++)
        total += strlen(fps[i]) + 1;
    char *buf = snewn(total, char), *p = buf;
    for (int i = 0; i < n; i++) {
        size_t L = strlen(fps[i]) + 1;
        memcpy(p, fps[i], L);
        p += L;
        sfree(fps[i]);
    }
    sfree(fps);
    *p = '\0';
    HKEY hk;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, KAGEANT_REG_BASE, 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &hk, NULL) == ERROR_SUCCESS) {
        RegSetValueExA(hk, KAGEANT_REG_ORDER, 0, REG_MULTI_SZ,
                       (const BYTE *)buf, (DWORD)total);
        RegCloseKey(hk);
    }
    sfree(buf);
}

/* KiTTY: apply the saved offer order to the currently loaded keys. Called at
 * startup once StartupKeys have been re-added. */
void kageant_apply_saved_order(void)
{
    HKEY hk;
    DWORD type = 0, sz = 0;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, KAGEANT_REG_BASE, 0,
                      KEY_QUERY_VALUE, &hk) != ERROR_SUCCESS)
        return;
    if (RegQueryValueExA(hk, KAGEANT_REG_ORDER, NULL, &type, NULL, &sz)
            == ERROR_SUCCESS && type == REG_MULTI_SZ && sz > 0) {
        char *buf = snewn(sz + 1, char);
        if (RegQueryValueExA(hk, KAGEANT_REG_ORDER, NULL, NULL,
                             (BYTE *)buf, &sz) == ERROR_SUCCESS) {
            buf[sz] = '\0';
            int n = 0;
            for (char *p = buf; *p; p += strlen(p) + 1)
                n++;
            char **fps = snewn(n ? n : 1, char *);
            int i = 0;
            for (char *p = buf; *p; p += strlen(p) + 1)
                fps[i++] = p;
            pageant_apply_key_order(fps, n);
            sfree(fps);
        }
        sfree(buf);
    }
    RegCloseKey(hk);
}

/* KiTTY (Patrick Cernko) private-key usage confirmation. Installed into the
 * agent core via kageant_confirm_hook below. Ask the user before allowing the
 * key to sign when the global "Confirm every key use" toggle is on, or when
 * the key's comment requests confirmation for just that key. Returns 0 to
 * refuse, nonzero to allow. */
extern int (*kageant_confirm_hook)(const char *comment);
int kageant_do_confirm(const char *comment)
{
    int mode = kageant_confirm_mode();
    if (mode == KAGEANT_CONFIRM_YES ||
        (mode == KAGEANT_CONFIRM_AUTO && comment &&
         (strstr(comment, "confirmation") ||
          strstr(comment, "need confirm") ||
          strstr(comment, "needs confirm")))) {
        char *msg = dupprintf(
            "A remote session is requesting to authenticate with the SSH key:"
            "\n\n    %s\n\nAllow this key to be used?", comment);
        int r = MessageBox(NULL, msg, "Confirm SSH key usage",
                           MB_ICONQUESTION | MB_YESNO | MB_SYSTEMMODAL);
        sfree(msg);
        return (r == IDYES);
    }
    return 1;   /* this key does not require usage confirmation */
}

/* KiTTY: show a short tray balloon when a key is used to authenticate. Installed
 * into the agent core via kageant_notify_hook; gated by the "Notify when a key
 * is used" toggle (default on). Non-blocking (no Sleep). */
extern void (*kageant_notify_hook)(const char *comment);
void kageant_do_notify(const char *comment)
{
    if (!kageant_notify_get() || !traywindow)
        return;
    NOTIFYICONDATA nid;
    memset(&nid, 0, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = traywindow;
    nid.uID = 1;                       /* same icon AddTrayIcon registered */
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO;
    nid.uTimeout = 5000;
    snprintf(nid.szInfoTitle, sizeof(nid.szInfoTitle), "kageant: SSH key used");
    snprintf(nid.szInfo, sizeof(nid.szInfo),
             "A key was used to authenticate:\n%s",
             (comment && *comment) ? comment : "(unnamed key)");
    Shell_NotifyIcon(NIM_MODIFY, &nid);
}

/* Seam accessor: the "Load keys on startup" tray handler (windows/pageant.c)
 * reports how many key paths were tracked this session. */
int kageant_nloaded(void)
{
    return g_nloaded;
}
