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
#include "kitty_notice.h"   /* KiTTY: near-the-clock notice window (not a balloon) */
#include "kitty_inilight.h"

/* KiTTY: notice accents. Kitty's own notices are green (workplace proxy), so
 * kageant uses two distinct colours: an amber WARNING for key-load problems,
 * and a blue INFO for key use - recognisably kageant, not a stray kitty
 * notice, and not confusable with a warning. */
#define KAGEANT_NOTICE_WARN RGB(190, 110, 0)
#define KAGEANT_NOTICE_INFO RGB(40, 70, 170)
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
    int  confirm;     /* stored ,confirm marker (per-key confirm-on-use) */
    int  failed;      /* the file was there and would not load: stop retrying */
    int  slot;        /* where it sat in the startup list - see below */
    char fp[160];     /* stored fingerprint, "" if the entry had none */
} KageantPendingKey;
static KageantPendingKey g_pending[64];
static int g_npending = 0;
static int g_fp_adopted = 0;   /* a changed key was accepted: re-save after load */
static char *kageant_fp_of_blob(strbuf *blob);   /* defined with the identity code */

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

/* KiTTY: -noload (clean slate). One switch turns the whole startup-keys
 * mechanism off for this run: kageant_startup_get() reports it disabled
 * (so nothing loads and the implicit save-on-add/remove snapshots skip),
 * and kageant_save_startup_keys() refuses outright as a belt-and-braces
 * guard - a clean-slate run must never rewrite the stored list with its
 * own (empty) key set. */
static int g_noload = 0;
void kageant_noload_set(void) { g_noload = 1; }
int kageant_noload(void) { return g_noload; }

int kageant_startup_get(void)
{
    char buf[8];
    if (g_noload)
        return 0;
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
#define KAGEANT_REG_NOTICESECS "NoticeTimeout"  /* notice display seconds */

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


/*
 * KiTTY: string settings with the same store precedence as the toggles
 * above - the authoritative store wins, the other is a first-run fallback,
 * and writes go to the authoritative store only (a portable install never
 * touches the registry). Used for the key-list window geometry and column
 * widths, which want to persist in portable mode too - which is why the
 * AuxWinPos mechanism (registry-only, persistence off in portable) is not
 * used for them.
 */
static int kageant_reg_read_str(const char *name, char *buf, size_t len)
{
    DWORD sz = (DWORD)len;
    if (RegGetValueA(HKEY_CURRENT_USER, KAGEANT_REG_BASE, name,
                     RRF_RT_REG_SZ, NULL, buf, &sz) != ERROR_SUCCESS)
        return 0;
    return buf[0] != '\0';
}

int kageant_setting_str_get(const char *inikey, const char *regname,
                            char *buf, size_t len)
{
    char ini[256];
    int have_ini = kitty_inilight_read("Agent", inikey, ini, sizeof(ini)) &&
        ini[0];
    if (kitty_inilight_registry_authoritative()) {
        if (kageant_reg_read_str(regname, buf, len))
            return 1;
    } else if (have_ini) {
        snprintf(buf, len, "%s", ini);
        return 1;
    } else {
        return kageant_reg_read_str(regname, buf, len);
    }
    if (have_ini) {
        snprintf(buf, len, "%s", ini);
        return 1;
    }
    return 0;
}

void kageant_setting_str_set(const char *inikey, const char *regname,
                             const char *value)
{
    if (!kitty_inilight_registry_authoritative() &&
        kitty_inilight_write("Agent", inikey, value))
        return;
    HKEY hk;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, KAGEANT_REG_BASE, 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &hk, NULL) == ERROR_SUCCESS) {
        RegSetValueExA(hk, regname, 0, REG_SZ, (const BYTE *)value,
                       (DWORD)(strlen(value) + 1));
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

/* KiTTY: how long a notice stays up, [Agent] noticetimeout (seconds). One
 * knob for all of them; absent/0 keeps each notice's own default. Clamped to
 * a sane 2..120s. The user can still HOVER to hold any notice open longer. */
int kageant_notice_seconds(int fallback)
{
    char buf[16];
    int ini_v = -1, reg_v, v;
    if (kitty_inilight_read("Agent", "noticetimeout", buf, sizeof(buf)))
        ini_v = atoi(buf);
    if (kitty_inilight_registry_authoritative())
        v = kageant_reg_read(KAGEANT_REG_NOTICESECS, &reg_v) ? reg_v :
            (ini_v > 0 ? ini_v : 0);
    else
        v = (ini_v >= 0) ? ini_v :
            (kageant_reg_read(KAGEANT_REG_NOTICESECS, &reg_v) ? reg_v : 0);
    if (v <= 0)
        return fallback;
    if (v < 2) v = 2;
    if (v > 120) v = 120;
    return v;
}

/* KiTTY: key-use confirmation mode (classic [Agent] askconfirmation).
 * KAGEANT_CONFIRM_NO silences even the per-key comment prompts (automation)
 * and is expressible only in the ini; the registry DWORD and the two-state
 * tray toggle keep their historical 0=auto / 1=yes meaning. Default AUTO. */
/* Registry <-> mode mapping. Historically the DWORD was two-state
 * (0 = auto, 1 = yes); we KEEP that and add 2 = no, so an existing install
 * is read exactly as before and only the new "Never" needs the new value. */
static int kageant_reg_to_mode(int r)
{
    return r == 1 ? KAGEANT_CONFIRM_YES :
           r == 2 ? KAGEANT_CONFIRM_NO  : KAGEANT_CONFIRM_AUTO;
}
static int kageant_mode_to_reg(int m)
{
    return m == KAGEANT_CONFIRM_YES ? 1 :
           m == KAGEANT_CONFIRM_NO  ? 2 : 0;
}

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
               kageant_reg_to_mode(reg_val) :
               (ini_mode >= 0 ? ini_mode : KAGEANT_CONFIRM_AUTO);
    if (ini_mode >= 0)
        return ini_mode;
    return kageant_reg_read(KAGEANT_REG_CONFIRM, &reg_val) ?
           kageant_reg_to_mode(reg_val) : KAGEANT_CONFIRM_AUTO;
}

/* The tray checkbox is two-state: checked = YES, unchecked = AUTO (or NO). */
int kageant_confirm_get(void)
{
    return kageant_confirm_mode() == KAGEANT_CONFIRM_YES;
}

void kageant_confirm_set(int on)
{
    /* The two-state tray toggle: on = confirm every use, off = by comment. */
    kageant_confirm_set_mode(on ? KAGEANT_CONFIRM_YES : KAGEANT_CONFIRM_AUTO);
}

/* Set the full three-state mode (the key-list radio buttons), written
 * THROUGH to both stores so an export/import lands the same value whichever
 * store the other machine reads. The registry now holds all three states
 * (0 = auto, 1 = yes, 2 = no; the first two unchanged from the old form). */
void kageant_confirm_set_mode(int mode)
{
    const char *s = (mode == KAGEANT_CONFIRM_YES) ? "yes" :
                    (mode == KAGEANT_CONFIRM_NO)  ? "no"  : "auto";
    kitty_inilight_write("Agent", "askconfirmation", s);
    kageant_reg_write(KAGEANT_REG_CONFIRM, kageant_mode_to_reg(mode));
}

/* KiTTY: "kitty.ini mode" indicator for the key-list window and the tray
 * tooltip: the resolved ini path when it is the authoritative settings
 * store, NULL when the registry is. */
const char *kageant_ini_status(void)
{
    return kitty_inilight_registry_authoritative() ? NULL
                                                   : kitty_inilight_file();
}

/* A kitty.ini exists somewhere the resolver can reach (next to the exe,
 * KITTY_INI_FILE, or %APPDATA%\KiTTY) - so there is a place to store the
 * ini-only [Agent] options, whatever the session's mode. NOT the same as
 * kageant_ini_status(), which is non-NULL only when the ini is ALSO the
 * authoritative store; here we just need a file to write to. */
int kageant_ini_present(void)
{
    return kitty_inilight_file() != NULL;
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

/* KiTTY: per-key confirm helpers for the startup list. The flag itself
 * lives in the agent core (pageant_get/set_key_confirm); these map it onto
 * the tracked key paths for the ,confirm token. */
static int kageant_confirm_of_loaded(int li)
{
    if (li < g_nblobs && g_loaded_blobs[li])
        return pageant_get_key_confirm(ptrlen_from_strbuf(g_loaded_blobs[li]));
    return 0;
}

static void kageant_apply_confirm_by_path(const char *abspath)
{
    int i;
    for (i = g_nloaded - 1; i >= 0; i--)
        if (!stricmp(g_loaded_keypaths[i], abspath)) {
            if (i < g_nblobs && g_loaded_blobs[i])
                pageant_set_key_confirm(
                    ptrlen_from_strbuf(g_loaded_blobs[i]), true);
            return;
        }
}

/* Persist the tracked key set. Portable (ini authoritative): numbered
 * [Agent] startupkeyN entries, relative where possible, with a trailing
 * ,encrypted marker. Otherwise: the StartupKeys REG_MULTI_SZ value, each
 * entry "path,encrypted" or "path,plain". */
void kageant_save_startup_keys(void)
{
    const char *f;
    int i;

    if (g_noload)   /* clean-slate run: never touch the stored list */
        return;

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
                const char *path, *fp = NULL;
                char *fp_owned = NULL;
                int enc, conf;
                if (p < g_npending && g_pending[p].slot <= n) {
                    path = g_pending[p].path;
                    enc  = g_pending[p].encrypted;
                    conf = g_pending[p].confirm;
                    fp   = g_pending[p].fp[0] ? g_pending[p].fp : NULL;
                    p++;
                } else if (li < g_nloaded) {
                    path = g_loaded_keypaths[li];
                    enc  = g_loaded_encrypted[li];
                    conf = kageant_confirm_of_loaded(li);
                    /* Computed from the blob captured when the key loaded, so
                     * it costs no file access and cannot disagree with what is
                     * actually in the agent. */
                    if (li < g_nblobs)
                        fp = fp_owned = kageant_fp_of_blob(g_loaded_blobs[li]);
                    li++;
                } else {
                    path = g_pending[p].path;
                    enc  = g_pending[p].encrypted;
                    conf = g_pending[p].confirm;
                    fp   = g_pending[p].fp[0] ? g_pending[p].fp : NULL;
                    p++;
                }
                kageant_store_form(path, store, sizeof(store));
                snprintf(key, sizeof(key), "startupkey%d", ++n);
                snprintf(val, sizeof(val), "%s%s%s%s%s", store,
                         enc ? ",encrypted" : "",
                         conf ? ",confirm" : "",
                         fp ? "," : "", fp ? fp : "");
                WritePrivateProfileStringA("Agent", key, val, f);
                sfree(fp_owned);
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
            char *fp = (i < g_nblobs) ? kageant_fp_of_blob(g_loaded_blobs[i])
                                      : NULL;
            entries[i] = dupprintf("%s,%s%s%s%s", g_loaded_keypaths[i],
                                   g_loaded_encrypted[i] ? "encrypted" : "plain",
                                   kageant_confirm_of_loaded(i) ? ",confirm" : "",
                                   fp ? "," : "", fp ? fp : "");
            sfree(fp);
            total += strlen(entries[i]) + 1;
        }
        for (i = 0; i < g_npending; i++) {
            entries[g_nloaded + i] = dupprintf(
                "%s,%s%s%s%s", g_pending[i].path,
                g_pending[i].encrypted ? "encrypted" : "plain",
                g_pending[i].confirm ? ",confirm" : "",
                g_pending[i].fp[0] ? "," : "",
                g_pending[i].fp[0] ? g_pending[i].fp : "");
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
 * Written THROUGH to both stores and read from the authoritative one (user,
 * 2026-08-10, unifying with the other agent settings): a registry-mode user
 * with keys on removable media wants these too, and keeping both stores equal
 * removes the "which store?" ambiguity the earlier ini-only rule was meant to
 * avoid.
 */
static int kageant_bool_get(const char *inikey, const char *regname, int def)
{
    char buf[8];
    int ini_v = -1, reg_v;
    if (kitty_inilight_read("Agent", inikey, buf, sizeof(buf))) {
        if (!stricmp(buf, "yes")) ini_v = 1;
        else if (!stricmp(buf, "no")) ini_v = 0;
    }
    if (kitty_inilight_registry_authoritative())
        return kageant_reg_read(regname, &reg_v) ? reg_v :
               (ini_v >= 0 ? ini_v : def);
    if (ini_v >= 0)
        return ini_v;
    return kageant_reg_read(regname, &reg_v) ? reg_v : def;
}
static int kageant_clamp_ttl(int v)
{
    if (v < 0) return 0;
    return v > KAGEANT_TTL_MAX ? KAGEANT_TTL_MAX : v;
}

int kageant_quiet_missing(void)
{
    return kageant_bool_get("quietmissingkeys", "QuietMissingKeys", 0);
}

/* [Agent] retrykeys: when a drive appears, try the startup keys that were not
 * there at login. Default ON - it does nothing at all unless a startup key is
 * actually missing, so there is no cost to anyone else. */
int kageant_retry_keys(void)
{
    return kageant_bool_get("retrykeys", "RetryKeys", 1);
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
    return kageant_bool_get("unloadonremove", "UnloadOnRemove", 0);
}

/* Seconds a typed passphrase is cached (encrypted) during a batch add.
 * Clamped to a sane range: 0 (do not cache) .. KAGEANT_TTL_MAX. */
int kageant_passphrase_ttl(void)
{
    char buf[16];
    int ini_v = -1, reg_v;
    if (kitty_inilight_read("Agent", "passphrasecacheseconds",
                            buf, sizeof(buf))) {
        int v = atoi(buf);
        if (v >= 0)
            ini_v = kageant_clamp_ttl(v);
    }
    if (kitty_inilight_registry_authoritative())
        return kageant_reg_read("PassphraseCacheSeconds", &reg_v) ?
               kageant_clamp_ttl(reg_v) : (ini_v >= 0 ? ini_v : 60);
    if (ini_v >= 0)
        return ini_v;
    return kageant_reg_read("PassphraseCacheSeconds", &reg_v) ?
           kageant_clamp_ttl(reg_v) : 60;
}

/* Setters - write THROUGH to both stores so the value is consistent whichever
 * is authoritative and survives export/import. */
int kageant_quiet_missing_set(int on)
{
    kitty_inilight_write("Agent", "quietmissingkeys", on ? "yes" : "no");
    kageant_reg_write("QuietMissingKeys", on ? 1 : 0);
    return 1;
}
int kageant_retry_keys_set(int on)
{
    kitty_inilight_write("Agent", "retrykeys", on ? "yes" : "no");
    kageant_reg_write("RetryKeys", on ? 1 : 0);
    return 1;
}
int kageant_unload_on_remove_set(int on)
{
    kitty_inilight_write("Agent", "unloadonremove", on ? "yes" : "no");
    kageant_reg_write("UnloadOnRemove", on ? 1 : 0);
    return 1;
}
int kageant_passphrase_ttl_set(int seconds)
{
    char buf[16];
    HKEY hk;
    seconds = kageant_clamp_ttl(seconds);
    snprintf(buf, sizeof(buf), "%d", seconds);
    kitty_inilight_write("Agent", "passphrasecacheseconds", buf);
    /* kageant_reg_write only stores 0/1, so write this DWORD directly. */
    if (RegCreateKeyExA(HKEY_CURRENT_USER, KAGEANT_REG_BASE, 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &hk, NULL) == ERROR_SUCCESS) {
        DWORD v = (DWORD)seconds;
        RegSetValueExA(hk, "PassphraseCacheSeconds", 0, REG_DWORD,
                       (const BYTE *)&v, sizeof(v));
        RegCloseKey(hk);
    }
    return 1;
}

/*
 * ---- key identity: the fingerprint stored beside each startup entry --------
 *
 * A startup entry is a PATH, and a path says nothing about what is at the end
 * of it. Storing the key's fingerprint alongside buys three things:
 *
 *  - a key file that has been REPLACED is noticed. Swapped on a stick,
 *    overwritten by an old backup, a copy that is not yours: without this it
 *    loads silently and you go on believing you are using the key you put
 *    there.
 *  - a key whose file is currently unreachable can still be identified, so
 *    removing it in View Keys can drop those entries too.
 *  - messages can name a key rather than only a path.
 *
 * Stored as an extra comma-separated token: "path[,encrypted][,SHA256:...]".
 * Entries written by older versions simply have none, and adopt one the first
 * time they load successfully.
 */
static char *kageant_fp_of_blob(strbuf *blob)
{
    char *full, *bare;
    if (!blob || !blob->len)
        return NULL;
    full = ssh2_fingerprint_blob(ptrlen_from_strbuf(blob), SSH_FPTYPE_SHA256);
    if (!full)
        return NULL;
    /* ssh2_fingerprint_blob gives "ssh-rsa 4096 SHA256:...." - algorithm and
     * bit count included. Only the hash goes into the stored entry: the rest is
     * derivable from the key and, being full of spaces, makes the entry harder
     * to read and to parse back. */
    bare = strstr(full, "SHA256:");
    if (!bare)
        return full;
    bare = dupstr(bare);
    sfree(full);
    return bare;
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
            if (g_npending > 0)
                g_pending[g_npending - 1].confirm = kageant_confirm_of_loaded(i);

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

    /*
     * Pending entries too, matched on the stored fingerprint.
     *
     * These are paths whose file is not readable right now, so there is nothing
     * to compute a blob from - but the entry remembers which key it is, which
     * is exactly what that fingerprint is for. Without this, removing a key
     * that also lives on a stick meant plugging the stick in loaded it straight
     * back.
     */
    {
        char *fp = NULL;
        for (i = 0; i < g_npending; i++) {
            if (!g_pending[i].fp[0])
                continue;               /* entry predates fingerprints */
            if (!fp) {
                strbuf *b = strbuf_new();
                put_datapl(b, blob);
                fp = kageant_fp_of_blob(b);
                strbuf_free(b);
                if (!fp)
                    break;
            }
            if (strcmp(g_pending[i].fp, fp))
                continue;
            for (j = i; j < g_npending - 1; j++)
                g_pending[j] = g_pending[j + 1];
            g_npending--;
            removed = 1;
            i--;
        }
        sfree(fp);
    }

    if (removed && kageant_startup_get())
        kageant_save_startup_keys();

    /*
     * Prune the saved offer order too. It is rebuilt from the keys the agent
     * holds now, and the key has already been deleted by the caller, so this
     * drops its fingerprint.
     *
     * Unconditionally, not only when an entry was removed: the agent's key list
     * has changed either way. A stale fingerprint left in the order is not
     * inert - if that key is ever loaded again it takes the old position back,
     * which is a surprise, and position is what decides the sequence keys are
     * offered to a server in.
     */
    kageant_save_key_order();
}

/*
 * Does the key at `path` still match the fingerprint stored for it?
 *
 * Returns 1 to go ahead (matched, or nothing stored to compare, or the user
 * accepted the new key), 0 to skip this entry. On acceptance *adopt is set, and
 * the caller records the new fingerprint.
 *
 * The prompt has to offer accepting PERMANENTLY, because a legitimate key
 * rotation looks exactly like a swapped file. A warning that can only be
 * dismissed and will return at every start is one people learn to click
 * through, which is worse than not warning at all.
 */
static int kageant_fp_ok(const char *path, const char *stored, int *adopt)
{
    strbuf *blob;
    char *actual;
    char *msg;
    int r;

    *adopt = 0;
    if (!stored || !*stored)
        return 1;                       /* nothing to compare against yet */

    blob = kageant_pubblob(path);
    if (!blob)
        return 1;                       /* unreadable: the loader reports it */
    actual = kageant_fp_of_blob(blob);
    strbuf_free(blob);
    if (!actual)
        return 1;

    if (!strcmp(actual, stored)) {
        sfree(actual);
        return 1;
    }

    msg = dupprintf(
        "The key file loaded at startup is NOT the key that was there before.\n\n"
        "    %s\n\n"
        "Stored:  %s\n"
        "Found:   %s\n\n"
        "If you replaced this key yourself, this is expected - answer Yes and "
        "the new key is remembered, and you will not be asked again.\n\n"
        "If you did not, the file has been changed by something else. Answer No "
        "to leave it out until you have looked at it.\n\n"
        "Load this key and remember it?",
        path, stored, actual);
    r = MessageBox(NULL, msg, "kageant - this key has changed",
                   MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2);
    sfree(msg);
    sfree(actual);

    if (r == IDYES) {
        *adopt = 1;
        return 1;
    }
    return 0;
}

/*
 * Which file did this key come from? Answered by the public blob, so it works
 * for a key however it was added, and returns NULL when the key was added by
 * some other client and we never saw a file for it.
 *
 * The returned string belongs to the tracking list - copy it if you need to
 * keep it.
 */
char *kageant_paths_of_blob(ptrlen blob)
{
    strbuf *out = strbuf_new();
    char *fp = NULL;
    int i, n = 0;

    /* Every file we loaded this key from - ALL of them, because the same key
     * in two places is exactly the case where "which file?" is worth asking:
     * a copy on a stick and a copy on the disk are one key to the agent. */
    for (i = 0; i < g_nloaded && i < g_nblobs; i++) {
        if (!g_loaded_blobs[i])
            continue;
        if (g_loaded_blobs[i]->len == blob.len &&
            !memcmp(g_loaded_blobs[i]->s, blob.ptr, blob.len)) {
            if (n++) put_dataz(out, "\n    ");
            put_dataz(out, g_loaded_keypaths[i]);
        }
    }

    /*
     * And the entries whose file is not reachable, matched on the fingerprint
     * they remember - the stick is out, the key is still in the agent, and
     * naming the file it belongs to is the whole point of the question.
     */
    {
        strbuf *b = strbuf_new();
        put_datapl(b, blob);
        fp = kageant_fp_of_blob(b);
        strbuf_free(b);
    }
    if (fp) {
        for (i = 0; i < g_npending; i++) {
            if (g_pending[i].fp[0] && !strcmp(g_pending[i].fp, fp)) {
                if (n++) put_dataz(out, "\n    ");
                put_dataz(out, g_pending[i].path);
                put_dataz(out, "   (not reachable right now)");
            }
        }
        sfree(fp);
    }

    if (!n) {
        strbuf_free(out);
        return NULL;
    }
    return strbuf_to_str(out);
}

/* KiTTY: the first tracked file path for this blob, as a plain path with no
 * annotations - for feeding back into win_add_keyfile ("Load key now" on a
 * deferred key). NULL when the key was added by another program. */
char *kageant_file_of_blob(ptrlen blob)
{
    for (int i = 0; i < g_nloaded && i < g_nblobs; i++) {
        if (!g_loaded_blobs[i])
            continue;
        if (g_loaded_blobs[i]->len == blob.len &&
            !memcmp(g_loaded_blobs[i]->s, blob.ptr, blob.len))
            return dupstr(g_loaded_keypaths[i]);
    }
    return NULL;
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
    g_pending[g_npending].confirm = 0;   /* callers set it when known */
    g_pending[g_npending].failed = 0;
    g_pending[g_npending].slot = slot;
    g_pending[g_npending].fp[0] = '\0';  /* the array slot may be reused */
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
            if (g_pending[i].confirm)
                kageant_apply_confirm_by_path(g_pending[i].path);
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

/* ------------------------------------------------------------------ *
 * KiTTY: ssh-add -t key lifetimes.                                    *
 *                                                                     *
 * A key added over the agent with a lifetime constraint must be       *
 * removed when it expires. The agent core (pageant.c) hands us the    *
 * key's public blob and the seconds via kageant_key_lifetime_hook; we *
 * remember (blob, expiry) and a 1-second frontend timer (TrayWndProc) *
 * calls kageant_expire_due_keys() to drop the ones whose time is up.  *
 * ------------------------------------------------------------------ */
typedef struct {
    strbuf *blob;
    ULONGLONG expiry;
    unsigned set_seconds;   /* the lifetime as requested, for display */
} KageantLifetime;
static KageantLifetime *g_lifetimes = NULL;
static int g_nlifetimes = 0, g_lifetimes_cap = 0;

static void kageant_lifetime_drop(int i)
{
    strbuf_free(g_lifetimes[i].blob);
    memmove(&g_lifetimes[i], &g_lifetimes[i + 1],
            (g_nlifetimes - i - 1) * sizeof(*g_lifetimes));
    g_nlifetimes--;
}

void kageant_key_set_lifetime(ptrlen pubblob, unsigned seconds)
{
    int i;
    /* NULL blob: a remove-all - forget every pending lifetime. */
    if (!pubblob.ptr) {
        while (g_nlifetimes > 0)
            kageant_lifetime_drop(0);
        return;
    }
    for (i = 0; i < g_nlifetimes; i++)
        if (g_lifetimes[i].blob->len == pubblob.len &&
            !memcmp(g_lifetimes[i].blob->s, pubblob.ptr, pubblob.len))
            break;
    /* 0 seconds: the key was removed, or re-added without -t - either way
     * a stale pending removal must not survive to kill its successor. */
    if (seconds == 0) {
        if (i < g_nlifetimes)
            kageant_lifetime_drop(i);
        return;
    }
    /* If this key already has a lifetime pending, replace it (a re-add with a
     * new -t resets the clock) rather than stacking two removals. */
    if (i == g_nlifetimes) {
        if (g_nlifetimes >= g_lifetimes_cap) {
            g_lifetimes_cap = g_lifetimes_cap ? g_lifetimes_cap * 2 : 8;
            g_lifetimes = sresize(g_lifetimes, g_lifetimes_cap, KageantLifetime);
        }
        g_lifetimes[g_nlifetimes++].blob = strbuf_dup(pubblob);
    }
    g_lifetimes[i].expiry = GetTickCount64() + (ULONGLONG)seconds * 1000;
    g_lifetimes[i].set_seconds = seconds;
}

int kageant_expire_due_keys(void)
{
    ULONGLONG now = GetTickCount64();
    int w = 0, i, ndue = 0;
    strbuf **due = NULL;
    /* Pop every due entry off the table FIRST: deleting the key calls back
     * into this table (clear-on-remove), which must not find the entry
     * mid-compaction. */
    for (i = 0; i < g_nlifetimes; i++) {
        if (g_lifetimes[i].expiry <= now) {
            due = sresize(due, ndue + 1, strbuf *);
            due[ndue++] = g_lifetimes[i].blob;
        } else {
            if (w != i) g_lifetimes[w] = g_lifetimes[i];
            w++;
        }
    }
    g_nlifetimes = w;
    for (i = 0; i < ndue; i++) {
        /* Just unload it from the agent - a lifetime is about in-memory
         * exposure, not the startup configuration, so leave any startup
         * entry alone (it can load again next time). */
        pageant_delete_ssh2_key_by_blob(ptrlen_from_strbuf(due[i]));
        strbuf_free(due[i]);
    }
    sfree(due);
    return ndue;
}

int kageant_key_lifetime_get(ptrlen pubblob, unsigned *set_seconds,
                             unsigned *remaining_seconds)
{
    ULONGLONG now = GetTickCount64();
    for (int i = 0; i < g_nlifetimes; i++) {
        if (g_lifetimes[i].blob->len == pubblob.len &&
            !memcmp(g_lifetimes[i].blob->s, pubblob.ptr, pubblob.len)) {
            if (set_seconds)
                *set_seconds = g_lifetimes[i].set_seconds;
            if (remaining_seconds)   /* round up: show 20 at t=0, 0 only when gone */
                *remaining_seconds = g_lifetimes[i].expiry > now ?
                    (unsigned)((g_lifetimes[i].expiry - now + 999) / 1000) : 0;
            return 1;
        }
    }
    return 0;
}

int kageant_lifetime_count(void)
{
    return g_nlifetimes;
}

/* ------------------------------------------------------------------ *
 * KiTTY: the pending (not-loaded) startup entries, exposed for the    *
 * key list window - so "6 keys could not be loaded" is answerable     *
 * without digging in the registry, and a dead entry can be removed.   *
 * ------------------------------------------------------------------ */
int kageant_pending_count(void) { return g_npending; }

int kageant_pending_get(int i, const char **path, int *encrypted,
                        const char **fp, int *failed)
{
    if (i < 0 || i >= g_npending)
        return 0;
    *path = g_pending[i].path;
    *encrypted = g_pending[i].encrypted;
    *fp = g_pending[i].fp;
    *failed = g_pending[i].failed;
    return 1;
}

/* Remove one pending entry: from memory (or the next save would write it
 * right back) AND from the stored list. */
void kageant_drop_pending(const char *path)
{
    int i, j;
    if (!path || !*path)
        return;
    for (i = 0; i < g_npending; i++) {
        if (!stricmp(g_pending[i].path, path)) {
            for (j = i; j < g_npending - 1; j++)
                g_pending[j] = g_pending[j + 1];
            g_npending--;
            break;
        }
    }
    kageant_forget_startup_key(path);
}

/* ------------------------------------------------------------------ *
 * KiTTY: per-key load mode - how this key will be added the NEXT time *
 * the startup list loads it: deferred (,encrypted) or decrypted at    *
 * load (,plain). Switchable from the key-details dialog; changing it  *
 * does not touch the key's current state in the agent.                *
 * ------------------------------------------------------------------ */
int kageant_startup_mode_get(const char *path)
{
    int i;
    if (!path || !*path)
        return -1;
    for (i = 0; i < g_nloaded; i++)
        if (!stricmp(g_loaded_keypaths[i], path))
            return g_loaded_encrypted[i] ? 1 : 0;
    for (i = 0; i < g_npending; i++)
        if (!stricmp(g_pending[i].path, path))
            return g_pending[i].encrypted ? 1 : 0;
    return -1;
}

void kageant_startup_mode_set(const char *path, int encrypted)
{
    int i, hit = 0;
    if (!path || !*path)
        return;
    for (i = 0; i < g_nloaded; i++)
        if (!stricmp(g_loaded_keypaths[i], path)) {
            g_loaded_encrypted[i] = encrypted ? 1 : 0;
            hit = 1;
        }
    for (i = 0; i < g_npending; i++)
        if (!stricmp(g_pending[i].path, path)) {
            g_pending[i].encrypted = encrypted ? 1 : 0;
            hit = 1;
        }
    if (hit)
        kageant_save_startup_keys();
}

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
/* Strip an entry's trailing ",encrypted"/",plain"/",SHA256:..." tokens in
 * place, leaving the stored path. Mirrors the loader's parser: one token per
 * pass from the right, an unknown token belongs to the path. The old
 * single-token strip here missed the fingerprint that .71 entries carry, so
 * removing a fingerprinted entry from the stored list silently failed and
 * the key came back at the next start. */
static void kageant_entry_strip(char *entry)
{
    for (;;) {
        char *c = strrchr(entry, ',');
        if (!c)
            return;
        if (!stricmp(c + 1, "encrypted") || !stricmp(c + 1, "plain") ||
            !stricmp(c + 1, "confirm") || strstr(c + 1, "SHA256:"))
            *c = '\0';
        else
            return;
    }
}

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
            char raw[MAX_PATH + 32];
            snprintf(key, sizeof(key), "startupkey%d", i);
            GetPrivateProfileStringA("Agent", key, "", val, sizeof(val), f);
            if (!val[0]) { gap++; continue; }
            gap = 0;
            snprintf(raw, sizeof(raw), "%s", val);      /* keep the ,markers */
            kageant_entry_strip(val);
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
                    char entry[MAX_PATH + 32];
                    snprintf(entry, sizeof(entry), "%s", p);
                    kageant_entry_strip(entry);
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
            int enc = 0, conf = 0, adopt = 0;
            char *c;
            char fp[160];
            fp[0] = '\0';
            snprintf(key, sizeof(key), "startupkey%d", i);
            GetPrivateProfileStringA("Agent", key, "", val, sizeof(val), f);
            if (!val[0]) { gap++; continue; }
            gap = 0;
            /* Trailing tokens, in any order and any of them absent:
             * ",encrypted"/",plain" and ",SHA256:..." (the fingerprint). */
            for (;;) {
                c = strrchr(val, ',');
                if (!c) break;
                if (!stricmp(c + 1, "encrypted")) { enc = 1; *c = '\0'; }
                else if (!stricmp(c + 1, "plain")) { enc = 0; *c = '\0'; }
                else if (!stricmp(c + 1, "confirm")) { conf = 1; *c = '\0'; }
                else if (strstr(c + 1, "SHA256:")) {
                    /* Bare "SHA256:..." as written now, and the longer
                     * "alg bits SHA256:..." that a build in between wrote -
                     * take the hash out of either. */
                    snprintf(fp, sizeof(fp), "%s", strstr(c + 1, "SHA256:"));
                    *c = '\0';
                } else break;           /* part of the path: leave it alone */
            }
            kageant_resolve_form(val, abspath, sizeof(abspath));
            if (GetFileAttributesA(abspath) == INVALID_FILE_ATTRIBUTES) {
                g_startup_missing++;
                /* seen-so-far count is this entry's place in the offer order */
                kageant_note_pending(abspath, enc, g_nloaded + g_npending);
                if (g_npending > 0) {
                    g_pending[g_npending - 1].confirm = conf;
                    if (fp[0])
                        snprintf(g_pending[g_npending - 1].fp,
                                 sizeof(g_pending[0].fp), "%s", fp);
                }
                continue;
            }
            /* Is it still the key that was here? */
            if (!kageant_fp_ok(abspath, fp, &adopt))
                continue;               /* user said no: leave it out */
            {
                Filename *fn = filename_from_str(abspath);
                win_add_keyfile(fn, enc ? true : false);
                filename_free(fn);
                if (conf)
                    kageant_apply_confirm_by_path(abspath);
            }
            /* Write the list out afterwards if anything changed: a key the user
             * accepted as replaced, or - the common case on the first run after
             * an upgrade - an entry that had no fingerprint yet and has now
             * been identified.
             *
             * AFTER the loop, never inside it: the list is saved from the keys
             * loaded so far, so saving half way through would truncate it to
             * whatever had loaded by then. */
            if (adopt || !fp[0])
                g_fp_adopted = 1;
        }
        g_startup_loading = 0;
        if (g_fp_adopted) {
            g_fp_adopted = 0;
            kageant_save_startup_keys();   /* now the list is complete */
        }
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
                    int conf = 0;
                    int adopt = 0;
                    char fp[160];
                    char *c;
                    /* Parse a COPY: stripping the trailing tokens in place
                     * would shorten *p, and the walk's `p += strlen(p) + 1`
                     * would then re-enter the middle of this same record and
                     * read its leftover token bytes as bogus extra entries -
                     * which is exactly what inflated the "keys not loaded"
                     * count (each marked+fingerprinted entry spawned one or
                     * two phantom missing keys). */
                    char entry[MAX_PATH + 32];
                    snprintf(entry, sizeof(entry), "%s", p);
                    fp[0] = '\0';
                    /* Same trailing tokens as the ini form: ",encrypted" /
                     * ",plain" and ",SHA256:..." - see the ini branch above. */
                    for (;;) {
                        c = strrchr(entry, ',');
                        if (!c) break;
                        if (!stricmp(c + 1, "encrypted")) { enc = 1; *c = '\0'; }
                        else if (!stricmp(c + 1, "plain")) { enc = 0; *c = '\0'; }
                        else if (!stricmp(c + 1, "confirm")) { conf = 1; *c = '\0'; }
                        else if (strstr(c + 1, "SHA256:")) {
                            snprintf(fp, sizeof(fp), "%s",
                                     strstr(c + 1, "SHA256:"));
                            *c = '\0';
                        } else break;
                    }
                    if (GetFileAttributesA(entry) == INVALID_FILE_ATTRIBUTES) {
                        g_startup_missing++;
                        kageant_note_pending(entry, enc,
                                             g_nloaded + g_npending);
                        if (g_npending > 0) {
                            g_pending[g_npending - 1].confirm = conf;
                            if (fp[0])
                                snprintf(g_pending[g_npending - 1].fp,
                                         sizeof(g_pending[0].fp), "%s", fp);
                        }
                        continue;
                    }
                    if (!kageant_fp_ok(entry, fp, &adopt))
                        continue;
                    {
                        Filename *fn = filename_from_str(entry);
                        win_add_keyfile(fn, enc ? true : false);
                        filename_free(fn);
                        if (conf)
                            kageant_apply_confirm_by_path(entry);
                    }
                    if (adopt || !fp[0])
                        g_fp_adopted = 1;   /* see the ini branch above */
                }
                g_startup_loading = 0;
                if (g_fp_adopted) {
                    g_fp_adopted = 0;
                    kageant_save_startup_keys();
                }
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
    if (g_startup_missing <= 0 || !traywindow)
        return;
    /* [Agent] quietmissingkeys: the user has said that keys being absent is
     * expected here - media that is not always plugged in - so counting them at
     * every start is noise. A key file that EXISTS and will not load is still
     * reported: that is a broken key, not an absent one, and staying quiet
     * about it would hide a real problem. */
    if (kageant_quiet_missing())
        return;
    /* Say what happens NEXT, not just what did not happen: with retrying on,
     * these keys are waiting rather than lost, and they load by themselves when
     * the drive comes back. Read as a plain failure, the old wording sent
     * people looking for something to fix. */
    char text[256];
    if (kageant_retry_keys())
        snprintf(text, sizeof(text),
                 "%d startup key%s not reachable right now. They will be "
                 "loaded as soon as the drive they are on is back.",
                 g_startup_missing, g_startup_missing == 1 ? " is" : "s are");
    else
        snprintf(text, sizeof(text),
                 "%d startup key%s could not be found and %s skipped.",
                 g_startup_missing, g_startup_missing == 1 ? "" : "s",
                 g_startup_missing == 1 ? "was" : "were");
    /* KiTTY: our own notice window instead of a tray balloon. Amber = a
     * warning (a key did not load); 10s - longer than key-use info, since a
     * missing key is something to act on; click opens View Keys. */
    kitty_notice_show("kageant: startup keys", text, KAGEANT_NOTICE_WARN,
                      kageant_notice_seconds(10), traywindow,
                      KAGEANT_WM_NOTICE_CLICK);
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
extern int (*kageant_confirm_hook)(const char *comment, int key_confirm);

/* KiTTY: notice for a key-set mutation that arrived over an external
 * transport (WM_COPYDATA or the pipe): name the key and, best effort, the
 * requesting process. Deliberately a NOTICE and not a prompt - a prompt
 * would break every scripted ssh-add. Shares the "Notify key usage"
 * setting with the signature notices. */
/* KiTTY: IPC access-control policy. All default OFF. lockdownmode blocks
 * both add and remove; blockipcadd / blockipcremove block one direction.
 * Read from the ini where authoritative, else the registry, same shape as
 * the other kageant settings. Enforced only for EXTERNAL requests. */
static int kageant_policy_get(const char *inikey, const char *regname)
{
    char buf[32];
    int ini_val = -1, reg_val;
    if (kitty_inilight_read("Agent", inikey, buf, sizeof(buf))) {
        if (!stricmp(buf, "yes")) ini_val = 1;
        else if (!stricmp(buf, "no")) ini_val = 0;
    }
    if (kitty_inilight_registry_authoritative())
        return kageant_reg_read(regname, &reg_val) ? reg_val :
               (ini_val >= 0 ? ini_val : 0);
    if (ini_val >= 0)
        return ini_val;
    return kageant_reg_read(regname, &reg_val) ? reg_val : 0;
}

int kageant_lockdown_get(void)
{
    return kageant_policy_get("lockdownmode", "LockdownMode");
}

/* Write a yes/no policy through to BOTH stores, like the confirm mode, so
 * the value is consistent in either mode and survives export/import. */
static void kageant_policy_set(const char *inikey, const char *regname, int on)
{
    kitty_inilight_write("Agent", inikey, on ? "yes" : "no");
    kageant_reg_write(regname, on ? 1 : 0);
}
void kageant_lockdown_set(int on)
{
    kageant_policy_set("lockdownmode", "LockdownMode", on);
}
int  kageant_blockadd_get(void)
{
    return kageant_policy_get("blockipcadd", "BlockIpcAdd");
}
void kageant_blockadd_set(int on)
{
    kageant_policy_set("blockipcadd", "BlockIpcAdd", on);
}
int  kageant_blockremove_get(void)
{
    return kageant_policy_get("blockipcremove", "BlockIpcRemove");
}
void kageant_blockremove_set(int on)
{
    kageant_policy_set("blockipcremove", "BlockIpcRemove", on);
}

/* The raw configured notice display time (0 = unset = per-notice default). */
int kageant_notice_timeout_get(void)
{
    char buf[16];
    int ini_v = -1, reg_v;
    if (kitty_inilight_read("Agent", "noticetimeout", buf, sizeof(buf)))
        ini_v = atoi(buf);
    if (kitty_inilight_registry_authoritative())
        return kageant_reg_read(KAGEANT_REG_NOTICESECS, &reg_v) ? reg_v :
               (ini_v > 0 ? ini_v : 0);
    if (ini_v >= 0)
        return ini_v;
    return kageant_reg_read(KAGEANT_REG_NOTICESECS, &reg_v) ? reg_v : 0;
}
void kageant_notice_timeout_set(int seconds)
{
    char buf[16];
    HKEY hk;
    if (seconds < 0) seconds = 0;
    if (seconds > 120) seconds = 120;
    snprintf(buf, sizeof(buf), "%d", seconds);
    kitty_inilight_write("Agent", "noticetimeout", buf);
    /* kageant_reg_write only stores 0/1, so write this DWORD directly. */
    if (RegCreateKeyExA(HKEY_CURRENT_USER, KAGEANT_REG_BASE, 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &hk, NULL) == ERROR_SUCCESS) {
        DWORD v = (DWORD)seconds;
        RegSetValueExA(hk, KAGEANT_REG_NOTICESECS, 0, REG_DWORD,
                       (const BYTE *)&v, sizeof(v));
        RegCloseKey(hk);
    }
}

int kageant_ipc_blocked(int op)
{
    if (kageant_lockdown_get())
        return 1;   /* add + remove + remove-all all blocked */
    if (op == KAGEANT_MUT_ADD)
        return kageant_policy_get("blockipcadd", "BlockIpcAdd");
    /* remove and remove-all share the one switch */
    return kageant_policy_get("blockipcremove", "BlockIpcRemove");
}

void kageant_do_mutation_notice(int op, const char *comment)
{
    char proc[MAX_PATH + 32];
    const char *title;
    char *text;

    if (!kageant_notify_get() || !traywindow)
        return;

    proc[0] = '\0';
    if (pageant_external_pid) {
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                               (DWORD)pageant_external_pid);
        char path[MAX_PATH];
        DWORD sz = sizeof(path);
        if (h && QueryFullProcessImageNameA(h, 0, path, &sz)) {
            const char *base = strrchr(path, '\\');
            snprintf(proc, sizeof(proc), " by %s (pid %lu)",
                     base ? base + 1 : path, pageant_external_pid);
        } else {
            snprintf(proc, sizeof(proc), " by pid %lu", pageant_external_pid);
        }
        if (h)
            CloseHandle(h);
    }

    title = op == KAGEANT_MUT_ADD    ? "kageant - key added" :
            op == KAGEANT_MUT_REMOVE ? "kageant - key removed" :
                                       "kageant - ALL keys removed";
    if (op == KAGEANT_MUT_REMOVE_ALL)
        text = dupprintf("All keys were removed from the agent%s.", proc);
    else
        text = dupprintf("%s%s:\n%s",
                         op == KAGEANT_MUT_ADD
                             ? "A key was added to the agent"
                             : "A key was removed from the agent",
                         proc, comment && *comment ? comment : "(no comment)");
    kitty_notice_show(title, text,
                      op == KAGEANT_MUT_ADD ? KAGEANT_NOTICE_INFO
                                            : KAGEANT_NOTICE_WARN,
                      kageant_notice_seconds(8), traywindow,
                      KAGEANT_WM_NOTICE_CLICK);
    sfree(text);
}

/* The comment convention. Checked once at ADD time (via
 * kageant_comment_confirm_hook) to set the real per-key flag; the sign-time
 * AUTO-mode check below stays as a safety net. */
int kageant_comment_wants_confirm(const char *comment)
{
    return comment &&
        (strstr(comment, "confirmation") ||
         strstr(comment, "need confirm") ||
         strstr(comment, "needs confirm"));
}

/* Storm protection: at most one confirm box on screen, and a user-set
 * "deny everything from now on" latch. See kageant_do_confirm. */
static int g_confirm_active = 0;    /* a confirm box is up right now */
static int g_confirm_suppress = 0;  /* user chose "deny & stop asking" */

/* Called when the user opens the key list - a deliberate "I am dealing with
 * this now" action, so lift any confirm-suppress latch they set during a
 * storm. */
void kageant_confirm_resume(void)
{
    g_confirm_suppress = 0;
}

int kageant_confirm_suppressed(void)
{
    return g_confirm_suppress;
}

int kageant_do_confirm(const char *comment, int key_confirm)
{
    int mode = kageant_confirm_mode();
    /* Every use -> always; By comment -> only if this key's own flag is set
     * (the comment merely seeds that flag when the key is added, so a per-key
     * "No" set in Key details wins); Never -> never, even a flagged key. */
    int needs = (mode == KAGEANT_CONFIRM_YES) ||
                (mode == KAGEANT_CONFIRM_AUTO && key_confirm);
    (void)comment;
    if (!needs)
        return 1;   /* this key does not require usage confirmation */

    /* The user hit "deny & stop asking" during a storm: keep denying, no box. */
    if (g_confirm_suppress)
        return 0;

    /* One box at a time. A hostile client can fire many sign requests; the
     * MessageBox modal loop pumps messages, so a second request can arrive
     * and stack another box on top. Deny the pile-up rather than let a bad
     * client fill the screen while the user is trying to say no. */
    if (g_confirm_active)
        return 0;

    g_confirm_active = 1;
    {
        char *msg = dupprintf(
            "A remote session is requesting to authenticate with the SSH key:"
            "\n\n    %s\n\n"
            "Yes - allow this one use.\n"
            "No - deny this one use.\n"
            "Cancel - deny this AND stop asking: all further requests are "
            "denied silently until you open the kageant key list.",
            comment && *comment ? comment : "(unnamed key)");
        int r = MessageBox(NULL, msg, "Confirm SSH key usage",
                           MB_ICONQUESTION | MB_YESNOCANCEL | MB_SYSTEMMODAL |
                           MB_DEFBUTTON2);   /* default No */
        sfree(msg);
        g_confirm_active = 0;

        if (r == IDCANCEL) {
            g_confirm_suppress = 1;
            if (kageant_notify_get() && traywindow)
                kitty_notice_show(
                    "kageant: confirmations blocked",
                    "Key-use confirmations are now being denied silently. "
                    "Click this notice, the tray \"Resume\" item, or the "
                    "key list's Resume button to allow them again.",
                    KAGEANT_NOTICE_WARN, kageant_notice_seconds(10),
                    traywindow, KAGEANT_WM_NOTICE_CLICK);
            return 0;
        }
        return (r == IDYES);
    }
}

/* KiTTY: show a short tray balloon when a key is used to authenticate. Installed
 * into the agent core via kageant_notify_hook; gated by the "Notify when a key
 * is used" toggle (default on). Non-blocking (no Sleep). */
extern void (*kageant_notify_hook)(const char *comment);
void kageant_do_notify(const char *comment)
{
    if (!kageant_notify_get() || !traywindow)
        return;
    /* KiTTY: our own notice window, not a tray balloon (the shell ignores
     * balloon durations and often suppresses them). Blue = kageant info; 5s;
     * click opens View Keys. */
    char text[256];
    snprintf(text, sizeof(text), "A key was used to authenticate:\n%s",
             (comment && *comment) ? comment : "(unnamed key)");
    kitty_notice_show("kageant: SSH key used", text, KAGEANT_NOTICE_INFO,
                      kageant_notice_seconds(5), traywindow,
                      KAGEANT_WM_NOTICE_CLICK);
}

/* Seam accessor: the "Load keys on startup" tray handler (windows/pageant.c)
 * reports how many key paths were tracked this session. */
int kageant_nloaded(void)
{
    return g_nloaded;
}
