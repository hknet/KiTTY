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
#include "kitty_startup_shortcut.h"
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

/*
 * Startup keys whose FILE was not there when we tried to load them, kept so
 * they can be tried again when it appears - a key on a USB stick or a network
 * share is absent at login and present later, and asking the user to re-add it
 * by hand every time is the whole problem.
 *
 * Only absent files go here. A file that exists and fails to PARSE is a
 * permanent error; retrying it on every device event would just re-run a
 * failure. That case gets the named-path box instead.
 */
typedef struct {
    char path[MAX_PATH + 1];
    int  encrypted;
    int  failed;      /* the file was there and would not load: stop retrying */
    int  slot;        /* where it sat in the startup list - see below */
} KageantPendingKey;
static KageantPendingKey g_pending[64];
static int g_npending = 0;

/* The public blob of each loaded key, alongside its path, so a key can be
 * identified later without its file - which is exactly the situation when the
 * media it came from has gone. Deletion by blob, never by list position: a
 * client can add or remove keys behind our back, and position N in our list is
 * then a different key from position N in the agent's. */
static strbuf **g_loaded_blobs = NULL;    /* public blob per loaded key, or NULL */
static int      g_nblobs = 0;

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

/* Set the full three-state mode (the key-list radio buttons). The ini can
 * store all three; the registry DWORD only yes/auto, so "no" folds to auto
 * there - but the radios are only offered in ini mode anyway. */
void kageant_confirm_set_mode(int mode)
{
    const char *s = (mode == KAGEANT_CONFIRM_YES) ? "yes" :
                    (mode == KAGEANT_CONFIRM_NO)  ? "no"  : "auto";
    if (!kitty_inilight_registry_authoritative() &&
        kitty_inilight_write("Agent", "askconfirmation", s))
        return;
    kageant_reg_write(KAGEANT_REG_CONFIRM, mode == KAGEANT_CONFIRM_YES ? 1 : 0);
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
        /*
         * Loaded keys AND the ones waiting for their drive, IN ORDER.
         *
         * Two things this has to get right:
         *
         * - a key whose file was not reachable must stay in the list. It is
         *   rebuilt from the keys currently loaded, so anything that did not
         *   load was dropped permanently: start with the USB drive unplugged,
         *   add any other key later, and the entry was gone for good with
         *   nothing said.
         *
         * - and it must stay where it WAS. The list is the order in which keys
         *   are offered to a server, and every key offered that the server does
         *   not want costs one of the attempts it allows before locking the
         *   account out. Appending recovered keys at the end quietly changes
         *   which keys are tried first.
         *
         * Pending entries carry the slot they came from and are woven back in
         * there; anything beyond the end simply lands at the end.
         */
        {
            int n = 0, p = 0, li = 0;
            while (li < g_nloaded || p < g_npending) {
                const char *path;
                int enc;
                if (p < g_npending && g_pending[p].slot <= n) {
                    path = g_pending[p].path;
                    enc  = g_pending[p].encrypted;
                    p++;
                } else if (li < g_nloaded) {
                    path = g_loaded_keypaths[li];
                    enc  = g_loaded_encrypted[li];
                    li++;
                } else {
                    path = g_pending[p].path;
                    enc  = g_pending[p].encrypted;
                    p++;
                }
                kageant_store_form(path, store, sizeof(store));
                snprintf(key, sizeof(key), "startupkey%d", ++n);
                snprintf(val, sizeof(val), "%s%s", store,
                         enc ? ",encrypted" : "");
                WritePrivateProfileStringA("Agent", key, val, f);
            }
        }
        return;
    }

    {
        /* Loaded keys AND the ones waiting for their drive - see the ini branch
         * above for why the waiting ones must not be dropped. */
        int nent = g_nloaded + g_npending;
        size_t total = 1;
        char **entries = (nent ? snewn(nent, char *) : NULL);
        char *buf, *p;
        HKEY hk;
        for (i = 0; i < g_nloaded; i++) {
            entries[i] = dupprintf("%s,%s", g_loaded_keypaths[i],
                                   g_loaded_encrypted[i] ? "encrypted" : "plain");
            total += strlen(entries[i]) + 1;
        }
        for (i = 0; i < g_npending; i++) {
            entries[g_nloaded + i] = dupprintf(
                "%s,%s", g_pending[i].path,
                g_pending[i].encrypted ? "encrypted" : "plain");
            total += strlen(entries[g_nloaded + i]) + 1;
        }
        buf = snewn(total, char); p = buf;
        for (i = 0; i < nent; i++) {
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
/*
 * How long typed passphrases may sit in the cache, in seconds.
 *
 * They are kept so that adding several keys at once asks once, and are scrubbed
 * when the add finishes - but an add that is abandoned never finishes, and they
 * would then stay until kageant exits. [Agent] passphrasecacheseconds bounds
 * that; 0 turns the backstop off, which is the old behaviour and not advised.
 *
 * ini-only, like restrictacl: it is a policy for this installation, and a
 * registry copy would be one more place for the two to disagree.
 */
/*
 * [Agent] quietmissingkeys: do not announce startup keys whose FILE is not
 * there. For installations where keys live on media that is not always plugged
 * in, where their absence is the normal state and not news.
 *
 * Deliberately about MISSING keys only. A key file that exists and will not
 * load is a broken key - a truncated file, a wrong format, a corrupted copy -
 * and that is reported whatever this is set to. Silencing it would hide the one
 * case the user genuinely needs to act on, which is why this is not the
 * "quietkeyfailures" it started out as.
 *
 * ini-only, like the TTL below.
 */
int kageant_quiet_missing(void)
{
    char buf[8];
    if (kitty_inilight_read("Agent", "quietmissingkeys", buf, sizeof(buf)))
        return !stricmp(buf, "yes");
    return 0;
}

/* [Agent] retrykeys: when a drive appears, try the startup keys that were not
 * there at login. Default ON - it does nothing at all unless a startup key is
 * actually missing, so there is no cost to anyone else. */
int kageant_retry_keys(void)
{
    char buf[8];
    if (kitty_inilight_read("Agent", "retrykeys", buf, sizeof(buf)))
        return !stricmp(buf, "yes");
    return 1;
}

/* [Agent] unloadonremove: when the media a key came from goes away, drop that
 * key from the agent. Default OFF, deliberately: pulling a stick should not
 * break the session someone is authenticating right now, and a loaded key is
 * already protected in memory. For people who want "this key exists only while
 * its media is present", which is a real requirement and not the common one.
 * NOTHING is deleted from disk - the key is unloaded from the agent, and put
 * back on the pending list so it returns if the media does. */
int kageant_unload_on_remove(void)
{
    char buf[8];
    if (kitty_inilight_read("Agent", "unloadonremove", buf, sizeof(buf)))
        return !stricmp(buf, "yes");
    return 0;
}

int kageant_passphrase_ttl(void)
{
    char buf[16];
    if (kitty_inilight_read("Agent", "passphrasecacheseconds", buf, sizeof(buf))) {
        int v = atoi(buf);
        if (v >= 0)
            return v;
    }
    return 60;
}

/*
 * Read a key file's PUBLIC blob. No passphrase involved - the public half of a
 * .ppk is not encrypted - and it is the only thing that identifies the key to
 * the agent afterwards, when the file may be gone.
 *
 * Returned as a malloc'd strbuf the caller frees, or NULL if the file cannot be
 * read as a key.
 */
static strbuf *kageant_pubblob(const char *path)
{
    Filename *fn = filename_from_str(path);
    strbuf *blob = strbuf_new();
    char *alg = NULL, *comment = NULL;
    const char *error = NULL;
    bool ok = ppk_loadpub_f(fn, &alg, BinarySink_UPCAST(blob), &comment, &error);
    filename_free(fn);
    sfree(alg);
    sfree(comment);
    if (!ok) {
        strbuf_free(blob);
        return NULL;
    }
    return blob;
}

/*
 * Drop from the agent every key that came from media which is no longer there.
 *
 * NOTHING is deleted from disk, and nothing is deleted by list position: each
 * key is identified by the public blob captured when it loaded, so a key list
 * that has been changed by a client in the meantime cannot make us unload the
 * wrong one. Each unloaded key goes back on the pending list, so plugging the
 * media in again brings it back.
 */
void kageant_media_gone(void)
{
    int i;
    if (!kageant_unload_on_remove())
        return;

    for (i = 0; i < g_nloaded; i++) {
        int j, still_backed = 0;

        if (GetFileAttributesA(g_loaded_keypaths[i]) != INVALID_FILE_ATTRIBUTES)
            continue;                          /* its file is still reachable */
        if (i >= g_nblobs || !g_loaded_blobs[i])
            continue;                          /* never identified: leave alone */

        /*
         * The AGENT holds one key, however many files it came from. If the same
         * key is also loaded from a file that is still reachable - the same key
         * kept on a stick AND on the local disk - then unloading it because the
         * stick has gone takes away a key the user still has. Measured
         * 2026-08-08: unplugging cost a key that was equally available locally.
         */
        for (j = 0; j < g_nloaded && !still_backed; j++) {
            if (j == i || j >= g_nblobs || !g_loaded_blobs[j])
                continue;
            if (g_loaded_blobs[j]->len == g_loaded_blobs[i]->len &&
                !memcmp(g_loaded_blobs[j]->s, g_loaded_blobs[i]->s,
                        g_loaded_blobs[i]->len) &&
                GetFileAttributesA(g_loaded_keypaths[j]) !=
                    INVALID_FILE_ATTRIBUTES)
                still_backed = 1;
        }
        if (still_backed)
            continue;

        if (pageant_delete_ssh2_key_by_blob(
                ptrlen_from_strbuf(g_loaded_blobs[i]))) {
            /* Back onto the pending list, at the position it held, so its place
             * in the offer order survives the round trip - the order decides
             * which keys a server is asked to try first, and a wrong key costs
             * one of the attempts before a lockout. */
            kageant_note_pending(g_loaded_keypaths[i], g_loaded_encrypted[i], i);

            /* And out of the loaded list, or the next save would write it twice
             * - once as loaded, once as pending. */
            sfree(g_loaded_keypaths[i]);
            if (g_loaded_blobs[i]) strbuf_free(g_loaded_blobs[i]);
            for (j = i; j < g_nloaded - 1; j++) {
                g_loaded_keypaths[j] = g_loaded_keypaths[j + 1];
                g_loaded_encrypted[j] = g_loaded_encrypted[j + 1];
                g_loaded_blobs[j] = g_loaded_blobs[j + 1];
            }
            g_nloaded--;
            g_nblobs = g_nloaded;
            i--;                               /* re-examine this slot */
        }
    }
}

/*
 * A key has been removed from the agent by hand: forget every file that
 * provided it, and write the startup list out.
 *
 * Removing a key in the View Keys window used to leave the startup list alone,
 * which is only ever saved when a key is ADDED. So the key came back at the
 * next start, and nothing in the UI could stop it - the list is not editable
 * anywhere else. Measured 2026-08-08.
 *
 * By blob, and ALL matching entries: the same key can be loaded from more than
 * one file (a copy on a stick and a copy on the disk), the agent holds it once,
 * and leaving either path behind would bring it back.
 */
void kageant_forget_loaded_by_blob(ptrlen blob)
{
    int i, j, removed = 0;

    for (i = 0; i < g_nloaded; i++) {
        if (i >= g_nblobs || !g_loaded_blobs[i])
            continue;
        if (g_loaded_blobs[i]->len != blob.len ||
            memcmp(g_loaded_blobs[i]->s, blob.ptr, blob.len))
            continue;

        sfree(g_loaded_keypaths[i]);
        strbuf_free(g_loaded_blobs[i]);
        for (j = i; j < g_nloaded - 1; j++) {
            g_loaded_keypaths[j] = g_loaded_keypaths[j + 1];
            g_loaded_encrypted[j] = g_loaded_encrypted[j + 1];
            g_loaded_blobs[j] = g_loaded_blobs[j + 1];
        }
        g_nloaded--;
        g_nblobs = g_nloaded;
        removed = 1;
        i--;
    }

    /* ⚠️ Pending entries are left alone. They are paths whose file was never
     * readable, so there is no blob to compare and no way to tell that one of
     * them is this same key. If a removed key also sits on media that is
     * currently absent, plugging that media in loads it again. Rare, visible
     * when it happens, and the alternative is guessing by filename. */

    if (removed && kageant_startup_get())
        kageant_save_startup_keys();
}

/* Remember a startup key whose file was not there, so a device event can try it
 * again. Silently full at 64: past that, something is wrong with the list
 * rather than with the media. */
void kageant_note_pending(const char *path, int encrypted, int slot)
{
    int i;
    if (!path || !*path || g_npending >= (int)lenof(g_pending))
        return;
    for (i = 0; i < g_npending; i++)
        if (!stricmp(g_pending[i].path, path))
            return;
    snprintf(g_pending[g_npending].path, sizeof(g_pending[0].path), "%s", path);
    g_pending[g_npending].encrypted = encrypted;
    g_pending[g_npending].failed = 0;
    g_pending[g_npending].slot = slot;
    g_npending++;
}

/*
 * Try the pending keys again. Called when a device arrives.
 *
 * Loaded DEFERRED whatever the entry said: a deferred load never asks for a
 * passphrase - it is wanted at first use - and a device event is no moment to
 * put a modal prompt in front of someone. The key coming back is silent; using
 * it asks, as it would for any deferred key.
 */
void kageant_retry_pending_keys(void)
{
    int i, w = 0, loaded_any = 0;
    if (!g_npending || !kageant_retry_keys())
        return;

    for (i = 0; i < g_npending; i++) {
        int before;

        if (g_pending[i].failed ||
            GetFileAttributesA(g_pending[i].path) == INVALID_FILE_ATTRIBUTES) {
            /* Still not there, or there and broken. Either way it stays on the
             * list: the list is also what keeps the entry in the saved startup
             * keys, and a key must not vanish from a user's configuration
             * because one load went wrong. */
            if (w != i) g_pending[w] = g_pending[i];
            w++;
            continue;
        }
        {
            Filename *fn = filename_from_str(g_pending[i].path);
            int j;
            before = g_nloaded;
            g_startup_loading = 1;             /* a failure now is a startup one */
            win_add_keyfile(fn, true);
            g_startup_loading = 0;
            filename_free(fn);

            if (g_nloaded == before) {
                /* Did not load. Keep the entry, but stop trying it: the file is
                 * present and broken, the user has already been told, and
                 * repeating the box at every device event would be its own
                 * annoyance. Restarting kageant tries again from scratch. */
                g_pending[i].failed = 1;
                if (w != i) g_pending[w] = g_pending[i];
                w++;
                continue;
            }

            /*
             * Loading deferred is a decision about THIS moment - a device
             * arriving is no time to raise a passphrase prompt - not about how
             * the user wants this key added in future. Put the recorded
             * preference back to what the stored entry said.
             *
             * Without this the deferred flag became permanent by accident: the
             * startup list is saved from the in-memory state, so the next time
             * any key was added by hand the whole list was written out with
             * this key now marked ",encrypted". Measured 2026-08-08 - keys the
             * user had deliberately added un-deferred came back deferred a
             * session later.
             */
            for (j = 0; j < g_nloaded; j++)
                if (!stricmp(g_loaded_keypaths[j], g_pending[i].path))
                    g_loaded_encrypted[j] = g_pending[i].encrypted;
            loaded_any = 1;
        }
        /* Dropped from the list whether or not it loaded: if the file is there
         * and will not parse, retrying on every future device event only
         * repeats the failure. */
    }
    g_npending = w;

    /*
     * A key that has just been re-added sits at the END of the agent's list,
     * which is the order keys are offered in - so a key the user had put first
     * comes back last, and the rest shift. Re-apply the saved offer order, the
     * same call the startup path makes once its keys are in.
     *
     * This matters beyond tidiness: each key offered that a server does not
     * want spends one of the attempts it allows before locking the account.
     */
    if (loaded_any)
        kageant_apply_saved_order();
}

/* Is a startup-list load in progress? Lets the failure path tell "this key
 * could not be loaded" from "this key you just picked could not be loaded",
 * which are different problems needing different words. */
int kageant_startup_loading(void) { return g_startup_loading; }

/*
 * Drop one entry from the persisted startup list, by path.
 *
 * NOT a matter of calling kageant_save_startup_keys(): that rebuilds the list
 * from the keys currently LOADED, and a key that failed to load was never in
 * that array - saving would either leave the entry untouched or wipe every
 * other startup key with it. So the stored list is edited where it lives.
 *
 * Matching is on the resolved absolute path, because entries are stored
 * relative when they sit inside a portable install.
 */
void kageant_forget_startup_key(const char *path)
{
    const char *f;
    char want[MAX_PATH + 1];

    if (!path || !*path)
        return;
    if (!_fullpath(want, path, sizeof(want)))
        snprintf(want, sizeof(want), "%s", path);

    if (!kitty_inilight_registry_authoritative() &&
        (f = kitty_inilight_file()) != NULL) {
        typedef char kageant_entry[MAX_PATH + 32];   /* snewn casts to (T *) */
        kageant_entry *keep;
        char key[32], val[MAX_PATH + 32], abspath[MAX_PATH + 1];
        int i, gap, n = 0, cap = 64;

        keep = snewn(cap, kageant_entry);
        for (i = 1, gap = 0; gap < 8 && n < cap; i++) {
            char raw[MAX_PATH + 32], *c;
            snprintf(key, sizeof(key), "startupkey%d", i);
            GetPrivateProfileStringA("Agent", key, "", val, sizeof(val), f);
            if (!val[0]) { gap++; continue; }
            gap = 0;
            snprintf(raw, sizeof(raw), "%s", val);      /* keep the ,marker */
            c = strrchr(val, ',');
            if (c && (!stricmp(c + 1, "encrypted") || !stricmp(c + 1, "plain")))
                *c = '\0';
            kageant_resolve_form(val, abspath, sizeof(abspath));
            if (!stricmp(abspath, want))
                continue;                               /* the one being dropped */
            snprintf(keep[n++], MAX_PATH + 32, "%s", raw);
        }
        /* Clear the old numbering before writing the survivors back: the list
         * is positional, so a shorter list must not leave a stale tail. */
        for (i = 1, gap = 0; gap < 8; i++) {
            snprintf(key, sizeof(key), "startupkey%d", i);
            GetPrivateProfileStringA("Agent", key, "", val, sizeof(val), f);
            if (val[0]) { WritePrivateProfileStringA("Agent", key, NULL, f); gap = 0; }
            else gap++;
        }
        for (i = 0; i < n; i++) {
            snprintf(key, sizeof(key), "startupkey%d", i + 1);
            WritePrivateProfileStringA("Agent", key, keep[i], f);
        }
        sfree(keep);
        return;
    }

    {
        HKEY hk;
        DWORD type = 0, sz = 0;
        if (RegOpenKeyExA(HKEY_CURRENT_USER, KAGEANT_REG_BASE, 0,
                          KEY_QUERY_VALUE | KEY_SET_VALUE, &hk) != ERROR_SUCCESS)
            return;
        if (RegQueryValueExA(hk, KAGEANT_REG_KEYS, NULL, &type, NULL, &sz)
                == ERROR_SUCCESS && type == REG_MULTI_SZ && sz > 0) {
            char *buf = snewn(sz + 2, char);
            if (RegQueryValueExA(hk, KAGEANT_REG_KEYS, NULL, NULL,
                                 (BYTE *)buf, &sz) == ERROR_SUCCESS) {
                char *out = snewn(sz + 2, char), *o = out, *p;
                buf[sz] = '\0'; buf[sz + 1] = '\0';
                for (p = buf; *p; p += strlen(p) + 1) {
                    char entry[MAX_PATH + 32], *c;
                    snprintf(entry, sizeof(entry), "%s", p);
                    c = strrchr(entry, ',');
                    if (c && (!stricmp(c + 1, "encrypted") ||
                              !stricmp(c + 1, "plain")))
                        *c = '\0';
                    if (!stricmp(entry, want))
                        continue;
                    memcpy(o, p, strlen(p) + 1); o += strlen(p) + 1;
                }
                *o++ = '\0';
                RegSetValueExA(hk, KAGEANT_REG_KEYS, 0, REG_MULTI_SZ,
                               (const BYTE *)out, (DWORD)(o - out));
                sfree(out);
            }
            sfree(buf);
        }
        RegCloseKey(hk);
    }
}

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
            /* Default NO - reference the key where it is. Copying a private key
             * is the answer that cannot be undone by changing your mind later,
             * and it is the wrong one for the case this prompt fires on most:
             * a key deliberately kept on a stick, which the user does not want
             * duplicated onto every machine they plug into. */
            int choice = MessageBox(NULL, prompt,
                "kageant - add key to startup",
                MB_ICONQUESTION | MB_YESNOCANCEL | MB_DEFBUTTON2);
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
    g_loaded_blobs = sresize(g_loaded_blobs, g_nloaded + 1, strbuf *);
    g_loaded_keypaths[g_nloaded] = dupstr(abspath);
    g_loaded_encrypted[g_nloaded] = encrypted ? 1 : 0;
    /* The key's public blob, captured NOW while the file is readable. It is
     * what identifies this key to the agent later, when the media it came from
     * may be gone - see kageant_media_gone(). NULL is fine: that key simply is
     * not eligible to be unloaded automatically. */
    g_loaded_blobs[g_nloaded] = kageant_pubblob(abspath);
    g_nloaded++;
    g_nblobs = g_nloaded;
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

/* ---- autostart: registry-free Startup shortcut (portable) + a warn-only
 * conflict scan so two agents don't silently block each other ---- */
#define KAGEANT_SHORTCUT_NAME "KiTTY kageant"

static const char *kageant_basename(const char *p)
{
    const char *b = p, *q;
    for (q = p; *q; q++)
        if (*q == '\\' || *q == '/') b = q + 1;
    return b;
}

static int kageant_is_agent_exe(const char *path)
{
    const char *b = kageant_basename(path);
    return !stricmp(b, "kageant.exe") || !stricmp(b, "pageant.exe");
}

/* Extract the exe from a Run command line (strip a leading quote/args). */
static void kageant_cmd_to_exe(const char *cmd, char *out, size_t len)
{
    const char *s = cmd, *e;
    size_t n;
    while (*s == ' ') s++;
    if (*s == '"') { s++; e = strchr(s, '"'); }
    else e = strchr(s, ' ');
    n = e ? (size_t)(e - s) : strlen(s);
    if (n >= len) n = len - 1;
    memcpy(out, s, n);
    out[n] = '\0';
}

static int kageant_scan_run(HKEY root, const char *myexe, char *desc, size_t len)
{
    HKEY hk;
    char name[256], data[MAX_PATH + 8], exe[MAX_PATH];
    DWORD idx = 0, nlen, dlen, type;
    int found = 0;
    if (RegOpenKeyExA(root, KAGEANT_RUN_KEY, 0, KEY_READ, &hk) != ERROR_SUCCESS)
        return 0;
    for (;;) {
        LONG r;
        nlen = sizeof(name); dlen = sizeof(data);
        r = RegEnumValueA(hk, idx++, name, &nlen, NULL, &type,
                          (BYTE *)data, &dlen);
        if (r == ERROR_NO_MORE_ITEMS) break;
        if (r != ERROR_SUCCESS) continue;
        if (type != REG_SZ && type != REG_EXPAND_SZ) continue;
        /* Exclude our own entry by EXE PATH, not by value name: the
         * KAGEANT_RUN_NAME name is shared by every kageant/pageant install,
         * so a same-named entry pointing at a different exe is a real
         * conflict (e.g. a system-installed kageant vs this portable one). */
        kageant_cmd_to_exe(data, exe, sizeof(exe));
        if (kageant_is_agent_exe(exe) && stricmp(exe, myexe) != 0) {
            snprintf(desc, len, "%s  ->  %s\n(%s\\...\\CurrentVersion\\Run)",
                     name, exe, root == HKEY_CURRENT_USER ? "HKCU" : "HKLM");
            found = 1;
            break;
        }
    }
    RegCloseKey(hk);
    return found;
}

static int kageant_scan_startup(int common, const char *myexe,
                                char *desc, size_t len)
{
    char dir[MAX_PATH], glob[MAX_PATH + 8], lnk[MAX_PATH], target[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int found = 0;
    if (!kitty_startup_dir(dir, sizeof(dir), common))
        return 0;
    snprintf(glob, sizeof(glob), "%s\\*.lnk", dir);
    h = FindFirstFileA(glob, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    do {
        snprintf(lnk, sizeof(lnk), "%s\\%s", dir, fd.cFileName);
        if (kitty_startup_shortcut_target(lnk, target, sizeof(target)) &&
            kageant_is_agent_exe(target) && stricmp(target, myexe) != 0) {
            snprintf(desc, len, "%s  ->  %s\n(%s Startup folder)",
                     fd.cFileName, target, common ? "all-users" : "your");
            found = 1;
            break;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return found;
}

/* 1 + a description when another kageant/pageant is set to autostart from a
 * different exe (so the single-instance agent would block one of them). */
int kageant_autostart_conflict(char *desc, size_t len)
{
    char myexe[MAX_PATH];
    if (!GetModuleFileNameA(NULL, myexe, sizeof(myexe)))
        return 0;
    return kageant_scan_run(HKEY_CURRENT_USER, myexe, desc, len)
        || kageant_scan_run(HKEY_LOCAL_MACHINE, myexe, desc, len)
        || kageant_scan_startup(0, myexe, desc, len)
        || kageant_scan_startup(1, myexe, desc, len);
}

/* Remove our HKCU Run entry ONLY when it points at this very exe. The value
 * name is shared by every kageant/pageant install, so deleting it blindly
 * would clobber another (e.g. system-installed) kageant's autostart. */
static void kageant_clear_own_run_entry(void)
{
    char myexe[MAX_PATH], data[MAX_PATH + 8], exe[MAX_PATH];
    HKEY hk;
    DWORD type = 0, sz = sizeof(data);
    if (!GetModuleFileNameA(NULL, myexe, sizeof(myexe)))
        return;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, KAGEANT_RUN_KEY, 0,
                      KEY_QUERY_VALUE | KEY_SET_VALUE, &hk) != ERROR_SUCCESS)
        return;
    if (RegQueryValueExA(hk, KAGEANT_RUN_NAME, NULL, &type,
                         (BYTE *)data, &sz) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ)) {
        kageant_cmd_to_exe(data, exe, sizeof(exe));
        if (!stricmp(exe, myexe))
            RegDeleteValueA(hk, KAGEANT_RUN_NAME);
    }
    RegCloseKey(hk);
}

/* 1 when kageant's own autostart is actually in place (so the tray checkmark
 * reflects reality, e.g. after the user deletes the shortcut by hand): the
 * Startup shortcut in portable mode, our own Run entry in registry mode. */
int kageant_autostart_active(void)
{
    if (!kitty_inilight_registry_authoritative())
        return kitty_startup_shortcut_exists(KAGEANT_SHORTCUT_NAME);
    {
        char myexe[MAX_PATH], data[MAX_PATH + 8], exe[MAX_PATH];
        HKEY hk;
        DWORD type = 0, sz = sizeof(data);
        int active = 0;
        if (!GetModuleFileNameA(NULL, myexe, sizeof(myexe)))
            return 0;
        if (RegOpenKeyExA(HKEY_CURRENT_USER, KAGEANT_RUN_KEY, 0,
                          KEY_QUERY_VALUE, &hk) != ERROR_SUCCESS)
            return 0;
        if (RegQueryValueExA(hk, KAGEANT_RUN_NAME, NULL, &type,
                             (BYTE *)data, &sz) == ERROR_SUCCESS &&
            (type == REG_SZ || type == REG_EXPAND_SZ)) {
            kageant_cmd_to_exe(data, exe, sizeof(exe));
            active = !stricmp(exe, myexe);
        }
        RegCloseKey(hk);
        return active;
    }
}

/* Install/remove kageant's login autostart: a Startup-folder shortcut in
 * portable mode (registry-free), the HKCU Run entry otherwise. */
void kageant_set_autostart(int on)
{
    if (!kitty_inilight_registry_authoritative()) {
        char exe[MAX_PATH], dir[MAX_PATH];
        char *slash;
        DWORD n;
        /* Clear only OUR OWN leftover Run entry (from a prior registry-mode
         * enable); never touch another kageant's same-named entry. */
        kageant_clear_own_run_entry();
        if (!on) {
            kitty_startup_shortcut_set(KAGEANT_SHORTCUT_NAME, NULL, NULL,
                                       NULL, NULL, 0);
            return;
        }
        n = GetModuleFileNameA(NULL, exe, sizeof(exe));
        if (!n || n >= sizeof(exe))
            return;
        snprintf(dir, sizeof(dir), "%s", exe);
        slash = strrchr(dir, '\\');
        if (slash) *slash = '\0';
        kitty_startup_shortcut_set(KAGEANT_SHORTCUT_NAME, exe, "", dir, exe, 1);
        return;
    }
    kageant_set_run_entry(on);
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
        int i, gap;
        g_startup_loading = 1;
        /* Tolerate gaps in the numbering: a hand-edit that deletes one
         * startupkeyN line must not truncate the rest of the list. Stop only
         * after a run of empty slots (matching the save-side clear scan). */
        for (i = 1, gap = 0; gap < 8; i++) {
            int enc = 0;
            char *c;
            snprintf(key, sizeof(key), "startupkey%d", i);
            GetPrivateProfileStringA("Agent", key, "", val, sizeof(val), f);
            if (!val[0]) { gap++; continue; }
            gap = 0;
            c = strrchr(val, ',');
            if (c && !stricmp(c + 1, "encrypted")) { *c = '\0'; enc = 1; }
            else if (c && !stricmp(c + 1, "plain")) { *c = '\0'; enc = 0; }
            kageant_resolve_form(val, abspath, sizeof(abspath));
            if (GetFileAttributesA(abspath) == INVALID_FILE_ATTRIBUTES) {
                g_startup_missing++;
                /* seen-so-far count is this entry's place in the offer order */
                kageant_note_pending(abspath, enc, g_nloaded + g_npending);
                continue;
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
                        g_startup_missing++;
                        kageant_note_pending(p, enc, g_nloaded + g_npending);
                        continue;
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
    /* [Agent] quietmissingkeys: the user has said that keys being absent is
     * expected here - media that is not always plugged in - so counting them at
     * every start is noise. A key file that EXISTS and will not load is still
     * reported: that is a broken key, not an absent one, and staying quiet
     * about it would hide a real problem. */
    if (kageant_quiet_missing())
        return;
    memset(&nid, 0, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = traywindow;
    nid.uID = 1;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_WARNING;
    nid.uTimeout = 5000;
    snprintf(nid.szInfoTitle, sizeof(nid.szInfoTitle), "kageant: startup keys");
    /* Say what happens NEXT, not just what did not happen: with retrying on,
     * these keys are waiting rather than lost, and they load by themselves when
     * the drive comes back. Read as a plain failure, the old wording sent
     * people looking for something to fix. */
    if (kageant_retry_keys())
        snprintf(nid.szInfo, sizeof(nid.szInfo),
                 "%d startup key%s not reachable right now. They will be "
                 "loaded as soon as the drive they are on is back.",
                 g_startup_missing, g_startup_missing == 1 ? " is" : "s are");
    else
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

    /*
     * In a portable install this belongs in the ini, like everything else.
     *
     * It used to go to the registry unconditionally - so the offer order did
     * not travel with a portable install, and writing it left a trace on every
     * machine the stick was plugged into. The order is not cosmetic: it decides
     * which keys a server is offered first, and each key it does not want costs
     * one of the attempts before a lockout.
     */
    if (!kitty_inilight_registry_authoritative() && kitty_inilight_file()) {
        const char *f = kitty_inilight_file();
        char key[32];
        int i, gap;
        /* clear the old numbering first - a shorter list must leave no tail */
        for (i = 1, gap = 0; gap < 8; i++) {
            char probe[512];
            snprintf(key, sizeof(key), "keyorder%d", i);
            GetPrivateProfileStringA("Agent", key, "", probe, sizeof(probe), f);
            if (probe[0]) { WritePrivateProfileStringA("Agent", key, NULL, f); gap = 0; }
            else gap++;
        }
        i = 0;
        for (char *q = buf; *q; q += strlen(q) + 1) {
            snprintf(key, sizeof(key), "keyorder%d", ++i);
            WritePrivateProfileStringA("Agent", key, q, f);
        }
        sfree(buf);
        return;
    }

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

    /* The ini first, where it is authoritative - see kageant_save_key_order. */
    if (!kitty_inilight_registry_authoritative() && kitty_inilight_file()) {
        const char *f = kitty_inilight_file();
        char key[32], val[512];
        char **fps = NULL;
        int n = 0, i, gap;
        for (i = 1, gap = 0; gap < 8; i++) {
            snprintf(key, sizeof(key), "keyorder%d", i);
            GetPrivateProfileStringA("Agent", key, "", val, sizeof(val), f);
            if (!val[0]) { gap++; continue; }
            gap = 0;
            fps = sresize(fps, n + 1, char *);
            fps[n++] = dupstr(val);
        }
        if (n) {
            pageant_apply_key_order(fps, n);
            for (i = 0; i < n; i++)
                sfree(fps[i]);
        }
        sfree(fps);
        return;
    }

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
