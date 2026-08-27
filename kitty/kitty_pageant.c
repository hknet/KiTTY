/*
 * kitty_pageant.c: the KiTTY additions to pageant (kageant), split out of
 * windows/pageant.c to keep that file textually close to upstream PuTTY.
 * Bodies are moved verbatim from pageant.c (cross-TU functions de-static'd);
 * see kitty_pageant.h for the interface and the banner comments below for
 * what each group does.
 */

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

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
#include "kitty_startup_shortcut.h"
#include "kitty_protkey.h"  /* kitty_protkey_available: the unprotected-memory warning */
#include "kitty_hello.h"    /* Windows Hello presence check (confirm gating) */
#include "kitty_hello_keys.h"  /* Hello-protected keys: the sidecar test */
#include "kitty_auditlog.h" /* the audit log's file sink */
#include "kitty_theme.h"    /* KITTY_THEME_* preference values */
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
    /*
     * The file at this path is NOT the key we recorded. Set when a device
     * arrives carrying a different key: the key is refused and the user is
     * told, rather than a dialog being raised at the moment something was
     * plugged in. The stored fingerprint above is deliberately left ALONE -
     * adopting the new one here would ratify the swap silently.
     */
    int  mismatch;
    /*
     * The path (minus its drive letter) turned up on a NEWLY ARRIVED drive,
     * but this entry has no recorded fingerprint, so there is nothing to
     * admit the file by and it was refused (see the anydrive block in the
     * retry pass). Set so the refusal is announced once, not on every one of
     * the several device-change messages Windows sends per insertion.
     */
    int  nofp_refused;
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

/* KiTTY: the ini key is loadkeysonstartup. It was loadonstartup, which read
 * as "start the agent" - the thing the tray item next to it actually does -
 * while this one only decides whether the REMEMBERED KEYS are re-added. The
 * old name is still read once and rewritten under the new one; there is no
 * delete in the inilight API, so the old key is blanked rather than removed,
 * which is enough to stop it being authoritative. */
static int kageant_startup_read_ini(void)
{
    char buf[8];
    int val = -1;
    if (kitty_inilight_read("Agent", "loadkeysonstartup", buf, sizeof(buf))) {
        if (!stricmp(buf, "yes")) val = 1;
        else if (!stricmp(buf, "no")) val = 0;
        if (val >= 0)
            return val;
    }
    if (kitty_inilight_read("Agent", "loadonstartup", buf, sizeof(buf))) {
        if (!stricmp(buf, "yes")) val = 1;
        else if (!stricmp(buf, "no")) val = 0;
        if (val >= 0) {
            /* migrate: write the new name, retire the old one */
            if (kitty_inilight_write("Agent", "loadkeysonstartup",
                                     val ? "yes" : "no"))
                kitty_inilight_write("Agent", "loadonstartup", "");
        }
    }
    return val;
}

int kageant_startup_get(void)
{
    if (g_noload)
        return 0;
    int ini_val = kageant_startup_read_ini(), reg_val;
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
        kitty_inilight_write("Agent", "loadkeysonstartup", on ? "yes" : "no"))
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

/*
 * Value-preserving registry access. A setting with more than two states MUST
 * use these directly: the boolean pair below deliberately collapses everything
 * to 0/1 at BOTH ends, so a multi-valued setting routed through it silently
 * loses every state above 1.
 */
static int kageant_reg_read_dword(const char *name, int *val_out)
{
    DWORD val, sz = sizeof(val);
    if (RegGetValueA(HKEY_CURRENT_USER, KAGEANT_REG_BASE, name,
                     RRF_RT_REG_DWORD, NULL, &val, &sz) != ERROR_SUCCESS)
        return 0;
    *val_out = (int)val;
    return 1;
}

static void kageant_reg_write_dword(const char *name, int val)
{
    HKEY hk;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, KAGEANT_REG_BASE, 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &hk, NULL) == ERROR_SUCCESS) {
        DWORD v = (DWORD)val;
        RegSetValueExA(hk, name, 0, REG_DWORD,
                       (const BYTE *)&v, sizeof(v));
        RegCloseKey(hk);
    }
}

/*
 * The on/off toggles. Booleans ONLY - anything with a third state belongs on
 * the pair above. The tri-state confirm mode was stored through here, so
 * "Never" (2) was written as 1 and read back as "confirm every use": the
 * setting did the exact opposite of what it said.
 */
static int kageant_reg_read(const char *name, int *val_out)
{
    int v;
    if (!kageant_reg_read_dword(name, &v))
        return 0;
    *val_out = v ? 1 : 0;
    return 1;
}

static void kageant_reg_write(const char *name, int on)
{
    kageant_reg_write_dword(name, on ? 1 : 0);
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
        v = kageant_reg_read_dword(KAGEANT_REG_NOTICESECS, &reg_v) ? reg_v :
            (ini_v > 0 ? ini_v : 0);
    else
        v = (ini_v >= 0) ? ini_v :
            (kageant_reg_read_dword(KAGEANT_REG_NOTICESECS, &reg_v) ? reg_v : 0);
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
    /* _dword, not the boolean reader: this setting has three states and the
     * boolean one would fold "no" (2) into "yes" (1). */
    if (kitty_inilight_registry_authoritative())
        return kageant_reg_read_dword(KAGEANT_REG_CONFIRM, &reg_val) ?
               kageant_reg_to_mode(reg_val) :
               (ini_mode >= 0 ? ini_mode : KAGEANT_CONFIRM_AUTO);
    if (ini_mode >= 0)
        return ini_mode;
    return kageant_reg_read_dword(KAGEANT_REG_CONFIRM, &reg_val) ?
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
    /* _dword, not the boolean writer: it would store "no" (2) as 1. */
    kageant_reg_write_dword(KAGEANT_REG_CONFIRM, kageant_mode_to_reg(mode));
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

static void kageant_apply_confirm_by_path(const char *abspath, int mode)
{
    int i;
    for (i = g_nloaded - 1; i >= 0; i--)
        if (!stricmp(g_loaded_keypaths[i], abspath)) {
            if (i < g_nblobs && g_loaded_blobs[i])
                pageant_set_key_confirm(
                    ptrlen_from_strbuf(g_loaded_blobs[i]), mode);
            return;
        }
}

/* The stored-entry marker for a confirm mode - the ONLY writer of these
 * tokens, so mode 2 cannot flatten to ",confirm" at one site and survive at
 * another. */
static const char *kageant_confirm_token(int mode)
{
    return mode == 2 ? ",helloconfirm" : mode ? ",confirm" : "";
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
                         kageant_confirm_token(conf),
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
                                   kageant_confirm_token(
                                       kageant_confirm_of_loaded(i)),
                                   fp ? "," : "", fp ? fp : "");
            sfree(fp);
            total += strlen(entries[i]) + 1;
        }
        for (i = 0; i < g_npending; i++) {
            entries[g_nloaded + i] = dupprintf(
                "%s,%s%s%s%s", g_pending[i].path,
                g_pending[i].encrypted ? "encrypted" : "plain",
                kageant_confirm_token(g_pending[i].confirm),
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
 * Written THROUGH to both stores and read from the authoritative one, the
 * same rule as every other agent setting: a registry-mode user
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

/* ---- the AGENT LOG: settings, path resolution and setup ----
 * User-facing name "agent log", deliberately NOT "audit log" - a local
 * file is evidence, never proof, and the name must not claim more than
 * that (the internal kitty_audit_* names describe the purpose, not a
 * promise). [Agent] agentlog (default ON), agentlogmaxkb / agentlogkeep /
 * agentlogexpiredays for the file sink's rotation, agentlogpath to
 * override the location entirely (also what makes the harness hermetic). */
static int kageant_int_setting(const char *inikey, const char *regname,
                               int def, int lo, int hi)
{
    char buf[16];
    int ini_v = -1, reg_v;
    if (kitty_inilight_read("Agent", inikey, buf, sizeof(buf))) {
        int v = atoi(buf);
        if (v >= lo && v <= hi)
            ini_v = v;
    }
    if (kitty_inilight_registry_authoritative()) {
        if (kageant_reg_read_dword(regname, &reg_v) &&
            reg_v >= lo && reg_v <= hi)
            return reg_v;
        return ini_v >= 0 ? ini_v : def;
    }
    if (ini_v >= 0)
        return ini_v;
    if (kageant_reg_read_dword(regname, &reg_v) && reg_v >= lo && reg_v <= hi)
        return reg_v;
    return def;
}

int kageant_audit_get(void)
{
    return kageant_bool_get("agentlog", "AgentLog", 1);
}
int kageant_audit_set(int on)
{
    kitty_inilight_write("Agent", "agentlog", on ? "yes" : "no");
    kageant_reg_write("AgentLog", on ? 1 : 0);
    kageant_audit_setup();
    return 1;
}

/* The knobs, for the settings dialog. Same defaults and clamps as the
 * setup below - one source of truth for the ranges would be nicer, but
 * these two sites are three lines apart and say the same numbers. */
int kageant_audit_maxkb_get(void)
{
    return kageant_int_setting("agentlogmaxkb", "AgentLogMaxKB",
                               KAGEANT_AGENTLOG_KB_DEFAULT, 16, 1048576);
}
int kageant_audit_keep_get(void)
{
    return kageant_int_setting("agentlogkeep", "AgentLogKeep",
                               KAGEANT_AGENTLOG_KEEP_DEFAULT, 1, 99);
}
int kageant_audit_expire_get(void)
{
    return kageant_int_setting("agentlogexpiredays", "AgentLogExpireDays",
                               KAGEANT_AGENTLOG_DAYS_DEFAULT, 0, 3650);
}
int kageant_audit_pathsetting_get(char *buf, size_t len)
{
    return kageant_setting_str_get("agentlogpath", "AgentLogPath",
                                   buf, len);
}

void kageant_audit_cfg_set(const char *path, int maxkb, int keep,
                           int expiredays)
{
    char num[16];
    if (maxkb < 16) maxkb = 16;
    if (maxkb > 1048576) maxkb = 1048576;
    if (keep < 1) keep = 1;
    if (keep > 99) keep = 99;
    if (expiredays < 0) expiredays = 0;
    if (expiredays > 3650) expiredays = 3650;
    kageant_setting_str_set("agentlogpath", "AgentLogPath",
                            path ? path : "");
    snprintf(num, sizeof(num), "%d", maxkb);
    kitty_inilight_write("Agent", "agentlogmaxkb", num);
    kageant_reg_write_dword("AgentLogMaxKB", maxkb);
    snprintf(num, sizeof(num), "%d", keep);
    kitty_inilight_write("Agent", "agentlogkeep", num);
    kageant_reg_write_dword("AgentLogKeep", keep);
    snprintf(num, sizeof(num), "%d", expiredays);
    kitty_inilight_write("Agent", "agentlogexpiredays", num);
    kageant_reg_write_dword("AgentLogExpireDays", expiredays);
    kageant_audit_setup();
}

/* Resolve the log path and (re)configure the sink. Portable installs log
 * beside their kitty.ini; registry-mode installs under
 * %LOCALAPPDATA%\kapper.net\KiTTY; [Agent] auditlogpath overrides both. */
/*
 * Where the log goes when [Agent] agentlogpath is not set. Split out of
 * kageant_audit_setup() so the settings dialog can SHOW this path: the File
 * box is empty in the default case, and an empty box that says "blank =
 * default" tells nobody where the file actually is.
 *
 * `create` is what separates the two callers. The setup path wants the
 * directory to exist because it is about to write there; the dialog only
 * wants the string, and must not create directories as a side effect of
 * being opened.
 */
int kageant_audit_default_path(char *buf, size_t len, int create)
{
    const char *ini;

    if (!buf || len == 0)
        return 0;
    buf[0] = '\0';

    /* A portable install logs beside its kitty.ini, so the whole install
     * stays on the stick. */
    if (!kitty_inilight_registry_authoritative() &&
        (ini = kitty_inilight_file()) != NULL) {
        const char *sl = strrchr(ini, '\\');
        if (sl)
            snprintf(buf, len, "%.*s\\kageant.log", (int)(sl - ini), ini);
    }
    if (!buf[0]) {
        char base[MAX_PATH + 1];
        DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", base, sizeof(base));
        if (n > 0 && n < sizeof(base)) {
            char dir[MAX_PATH + 1];
            snprintf(dir, sizeof(dir), "%s\\kapper.net", base);
            if (create)
                CreateDirectoryA(dir, NULL);
            snprintf(dir, sizeof(dir), "%s\\kapper.net\\KiTTY", base);
            if (create)
                CreateDirectoryA(dir, NULL);
            snprintf(buf, len, "%s\\kageant.log", dir);
        }
    }
    return buf[0] != '\0';
}

void kageant_audit_setup(void)
{
    char path[MAX_PATH + 1];
    path[0] = '\0';
    if (!kageant_setting_str_get("agentlogpath", "AgentLogPath",
                                 path, sizeof(path)) || !path[0])
        kageant_audit_default_path(path, sizeof(path), 1);
    kitty_audit_configure(
        path[0] ? path : NULL, path[0] ? kageant_audit_get() : 0,
        kageant_audit_maxkb_get(), kageant_audit_keep_get(),
        kageant_audit_expire_get());
}

/* Best-effort requester identity for the log: the exe base name AND its
 * full path behind a pid - the base name is what the first-level view
 * shows, the full path is the audit-trail fact ("which ssh.exe?"). */
static void kageant_req_name(unsigned long pid, char *base_out, size_t bsz,
                             char *path_out, size_t psz)
{
    HANDLE h;
    base_out[0] = path_out[0] = '\0';
    if (!pid)
        return;
    h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (h) {
        char path[MAX_PATH + 1];
        DWORD n = sizeof(path);
        if (QueryFullProcessImageNameA(h, 0, path, &n) && path[0]) {
            const char *base = strrchr(path, '\\');
            snprintf(base_out, bsz, "%s", base ? base + 1 : path);
            snprintf(path_out, psz, "%s", path);
        }
        CloseHandle(h);
    }
}

/* One sign/confirm-shaped audit line; pid 0 drops the requester fields. */
static void kageant_audit_use(const char *ev, const char *fp,
                              const char *comment, const char *result,
                              const char *reason, unsigned long pid)
{
    char req[80], reqpath[MAX_PATH + 1], pidbuf[16];
    kageant_req_name(pid, req, sizeof(req), reqpath, sizeof(reqpath));
    snprintf(pidbuf, sizeof(pidbuf), "%lu", pid);
    kitty_audit(ev, "fp", fp, "comment", comment, "result", result,
                "reason", reason, "req", req, "reqpath", reqpath,
                "pid", pid ? pidbuf : NULL, (const char *)NULL);
}

/* [Agent] helloconfirm: every confirmation prompt demands a Windows Hello
 * presence check instead of a button. Default OFF; a single key can demand
 * it via its per-key confirm mode without this. Genuinely boolean. */
int kageant_hello_get(void)
{
    return kageant_bool_get("helloconfirm", "HelloConfirm", 0);
}
int kageant_hello_set(int on)
{
    kitty_inilight_write("Agent", "helloconfirm", on ? "yes" : "no");
    kageant_reg_write("HelloConfirm", on ? 1 : 0);
    return 1;
}

/* [Agent] retrykeys: when a drive appears, try the startup keys that were not
 * there at login. Default ON - it does nothing at all unless a startup key is
 * actually missing, so there is no cost to anyone else.
 *
 * THREE-valued, so it must not go through the boolean helpers (which collapse
 * to 0/1 at both ends - see the note on kageant_reg_read):
 *   0 = never, 1 = from the stored drive+path, 2 = from the stored path on
 *   whichever drive just arrived (KAGEANT_RETRY_ANYDRIVE). The ini spellings
 *   are no / yes / ignoredriveletter, so old files keep their meaning. */
int kageant_retry_keys(void)
{
    char buf[24];
    int ini_v = -1, reg_v;
    if (kitty_inilight_read("Agent", "retrykeys", buf, sizeof(buf))) {
        if (!stricmp(buf, "yes")) ini_v = 1;
        else if (!stricmp(buf, "no")) ini_v = 0;
        else if (!stricmp(buf, "ignoredriveletter")) ini_v = KAGEANT_RETRY_ANYDRIVE;
    }
    if (kitty_inilight_registry_authoritative()) {
        if (kageant_reg_read_dword("RetryKeys", &reg_v))
            return (reg_v >= 0 && reg_v <= KAGEANT_RETRY_ANYDRIVE) ? reg_v : 1;
        return ini_v >= 0 ? ini_v : 1;
    }
    if (ini_v >= 0)
        return ini_v;
    if (kageant_reg_read_dword("RetryKeys", &reg_v))
        return (reg_v >= 0 && reg_v <= KAGEANT_RETRY_ANYDRIVE) ? reg_v : 1;
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
        return kageant_reg_read_dword("PassphraseCacheSeconds", &reg_v) ?
               kageant_clamp_ttl(reg_v) : (ini_v >= 0 ? ini_v : KAGEANT_TTL_DEFAULT);
    if (ini_v >= 0)
        return ini_v;
    return kageant_reg_read_dword("PassphraseCacheSeconds", &reg_v) ?
           kageant_clamp_ttl(reg_v) : KAGEANT_TTL_DEFAULT;
}

/* Seconds the Windows Hello KEK cache stays valid after a gesture, so a
 * batch of protected keys and quick successive unlocks need ONE face.
 * Same range and default as the passphrase cache (Chromium's device
 * reauth uses the same 60 s validity window). 0 = every unlock asks. */
int kageant_hello_ttl(void)
{
    char buf[16];
    int ini_v = -1, reg_v;
    if (kitty_inilight_read("Agent", "hellocacheseconds",
                            buf, sizeof(buf))) {
        int v = atoi(buf);
        if (v >= 0)
            ini_v = kageant_clamp_ttl(v);
    }
    if (kitty_inilight_registry_authoritative())
        return kageant_reg_read_dword("HelloCacheSeconds", &reg_v) ?
               kageant_clamp_ttl(reg_v) : (ini_v >= 0 ? ini_v : KAGEANT_TTL_DEFAULT);
    if (ini_v >= 0)
        return ini_v;
    return kageant_reg_read_dword("HelloCacheSeconds", &reg_v) ?
           kageant_clamp_ttl(reg_v) : KAGEANT_TTL_DEFAULT;
}

/* Setters - write THROUGH to both stores so the value is consistent whichever
 * is authoritative and survives export/import. */
int kageant_quiet_missing_set(int on)
{
    kitty_inilight_write("Agent", "quietmissingkeys", on ? "yes" : "no");
    kageant_reg_write("QuietMissingKeys", on ? 1 : 0);
    return 1;
}
int kageant_retry_keys_set(int mode)
{
    if (mode < 0 || mode > KAGEANT_RETRY_ANYDRIVE)
        mode = 1;
    /* Value-preserving on BOTH ends - routed through the boolean pair this
     * would write mode 2 as 1 and the third state could never exist. */
    kitty_inilight_write("Agent", "retrykeys",
                         mode == KAGEANT_RETRY_ANYDRIVE ? "ignoredriveletter" :
                         mode ? "yes" : "no");
    kageant_reg_write_dword("RetryKeys", mode);
    return 1;
}
/* [Agent] theme - THREE-valued, so it takes the same value-preserving DWORD
 * route as retrykeys above rather than the boolean helpers. Default 0
 * (follow the system), which is also what every Windows too old to have a
 * system preference resolves to. */
int kageant_theme_get(void)
{
    char buf[16];
    int ini_v = -1, reg_v;
    if (kitty_inilight_read("Agent", "theme", buf, sizeof(buf))) {
        if (!stricmp(buf, "system")) ini_v = KITTY_THEME_SYSTEM;
        else if (!stricmp(buf, "light")) ini_v = KITTY_THEME_LIGHT;
        else if (!stricmp(buf, "dark")) ini_v = KITTY_THEME_DARK;
    }
    if (kitty_inilight_registry_authoritative()) {
        if (kageant_reg_read_dword("Theme", &reg_v))
            return (reg_v >= KITTY_THEME_SYSTEM && reg_v <= KITTY_THEME_DARK)
                ? reg_v : KITTY_THEME_SYSTEM;
        return ini_v >= 0 ? ini_v : KITTY_THEME_SYSTEM;
    }
    if (ini_v >= 0)
        return ini_v;
    if (kageant_reg_read_dword("Theme", &reg_v))
        return (reg_v >= KITTY_THEME_SYSTEM && reg_v <= KITTY_THEME_DARK)
            ? reg_v : KITTY_THEME_SYSTEM;
    return KITTY_THEME_SYSTEM;
}
int kageant_theme_set(int pref)
{
    if (pref < KITTY_THEME_SYSTEM || pref > KITTY_THEME_DARK)
        pref = KITTY_THEME_SYSTEM;
    kitty_inilight_write("Agent", "theme",
                         pref == KITTY_THEME_DARK ? "dark" :
                         pref == KITTY_THEME_LIGHT ? "light" : "system");
    kageant_reg_write_dword("Theme", pref);
    return 1;
}
/* The settings dialog's last page. Window state, so registry only - a
 * portable install that never writes the registry simply always opens on the
 * first page, which is what the window geometry already does. */
int kageant_settings_tab_get(void)
{
    int v;
    if (!kageant_reg_read_dword("SettingsTab", &v) || v < 0 || v > 15)
        return 0;
    return v;
}
void kageant_settings_tab_set(int page)
{
    if (page < 0 || page > 15)
        page = 0;
    kageant_reg_write_dword("SettingsTab", page);
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
int kageant_hello_ttl_set(int seconds)
{
    char buf[16];
    HKEY hk;
    seconds = kageant_clamp_ttl(seconds);
    snprintf(buf, sizeof(buf), "%d", seconds);
    kitty_inilight_write("Agent", "hellocacheseconds", buf);
    if (RegCreateKeyExA(HKEY_CURRENT_USER, KAGEANT_REG_BASE, 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &hk, NULL) == ERROR_SUCCESS) {
        DWORD v = (DWORD)seconds;
        RegSetValueExA(hk, "HelloCacheSeconds", 0, REG_DWORD,
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
            if (g_npending > 0) {
                g_pending[g_npending - 1].confirm = kageant_confirm_of_loaded(i);
                /*
                 * And the fingerprint of the key we are unloading - WE KNOW IT,
                 * it is the blob we just deleted by. Without this the entry goes
                 * back with nothing recorded, and when the media returns the
                 * retry takes the "nothing to compare against" branch and loads
                 * whatever is at that path: unplug, swap the file, plug in, and
                 * the swap is accepted and adopted as the new baseline.
                 *
                 * Reported from a real stick 2026-08-13. It survived testing
                 * because pending entries have two origins and only the other
                 * one - the stored startup list - was ever exercised; that one
                 * carries a fingerprint, so the check looked complete.
                 */
                char *fp = kageant_fp_of_blob(g_loaded_blobs[i]);
                if (fp) {
                    snprintf(g_pending[g_npending - 1].fp,
                             sizeof(g_pending[0].fp), "%s", fp);
                    sfree(fp);
                }
            }

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
    /* KiTTY: removing a key can take a HELD-BACK entry with it (they are
     * matched on the stored fingerprint above), so the tooltip's warning line
     * may no longer be true. */
    if (removed)
        kageant_refresh_tray_tip();

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
/*
 * KiTTY: does the file at `path` still hold the key `stored` describes?
 *
 *   1  = yes
 *   0  = no, it is a different key
 *  -1  = cannot tell (unreadable, or no fingerprint could be computed)
 *
 * The comparison lives here alone so the two callers cannot drift: the startup
 * loader, which may ask the user about a mismatch, and the device-arrival
 * retry, which must never ask and simply refuses.
 */
int kageant_fp_matches(const char *path, const char *stored)
{
    strbuf *blob;
    char *actual;
    int same;

    if (!stored || !*stored)
        return -1;                      /* nothing to compare against */
    blob = kageant_pubblob(path);
    if (!blob)
        return -1;
    actual = kageant_fp_of_blob(blob);
    strbuf_free(blob);
    if (!actual)
        return -1;
    same = !strcmp(actual, stored);
    sfree(actual);
    return same ? 1 : 0;
}

/*
 * Is the key we are now holding for `path` the one recorded for it?
 *
 * This is the ONLY fingerprint comparison on the load paths, and it is made
 * against what the agent actually ended up with rather than against the file.
 *
 * It used to be the other way round - inspect the file, then hand the same
 * PATH to the loader, which opened it a second time. That was not a design;
 * it fell out of bolting a check onto PuTTY's loader, whose entry point takes
 * a filename (pageant_add_keyfile) and not a key we already parsed. Two reads
 * of a file whose contents are not ours between them is exactly the hole this
 * check exists to close: read once as the right key, once as another.
 *
 * Loading before checking is safe here because agent requests are answered on
 * this same thread - the WM_COPYDATA subthread only signals an event, and both
 * it and the named pipe are dispatched from the main event loop - so nothing
 * can ask the agent to sign anything while this function is running.
 *
 * Returns 1 if the key matches, or if there is nothing recorded to compare
 * against; 0 if it does not, in which case the key has been taken back out of
 * the agent before returning. The stored ENTRY is deliberately left alone: the
 * user's configuration is not something a swapped file gets to edit.
 */
static int kageant_verify_loaded(const char *path, const char *stored)
{
    int i, j, same, still_backed = 0;
    char *fp;

    if (!stored || !*stored)
        return 1;                     /* nothing recorded to compare against */

    for (i = g_nloaded - 1; i >= 0; i--)          /* the newest one wins */
        if (!stricmp(g_loaded_keypaths[i], path))
            break;
    if (i < 0 || i >= g_nblobs || !g_loaded_blobs[i])
        return 1;                     /* never identified: nothing to check */

    fp = kageant_fp_of_blob(g_loaded_blobs[i]);
    if (!fp)
        return 1;
    same = !strcmp(fp, stored);
    sfree(fp);
    if (same)
        return 1;

    /*
     * Does another tracked file provide this same key? Then the agent's copy is
     * not ours to remove: a file that has been swapped for a copy of some OTHER
     * key the user legitimately loaded would otherwise take that key away - a
     * swap turning into a way to unload keys. Drop our own tracking of this
     * path only, and let the entry be marked instead.
     */
    for (j = 0; j < g_nloaded && !still_backed; j++) {
        if (j == i || j >= g_nblobs || !g_loaded_blobs[j])
            continue;
        if (g_loaded_blobs[j]->len == g_loaded_blobs[i]->len &&
            !memcmp(g_loaded_blobs[j]->s, g_loaded_blobs[i]->s,
                    g_loaded_blobs[i]->len))
            still_backed = 1;
    }

    /*
     * Out of the agent first, then out of our own lists - in that order,
     * because the delete reads the blob we are about to free.
     */
    if (!still_backed)
        pageant_delete_ssh2_key_by_blob(ptrlen_from_strbuf(g_loaded_blobs[i]));

    sfree(g_loaded_keypaths[i]);
    strbuf_free(g_loaded_blobs[i]);
    for (j = i; j < g_nloaded - 1; j++) {
        g_loaded_keypaths[j] = g_loaded_keypaths[j + 1];
        g_loaded_encrypted[j] = g_loaded_encrypted[j + 1];
        g_loaded_blobs[j] = g_loaded_blobs[j + 1];
    }
    g_nloaded--;
    g_nblobs = g_nloaded;
    return 0;
}

/*
 * Can this key be added in deferred (still-encrypted) form?
 *
 * SSH-1 keys cannot: the agent's add-encrypted extension is SSH-2 only, and
 * asking for it fails the whole add with "Can't add SSH-1 keys in encrypted
 * form" (pageant.c). Anything that loads deferred-by-default therefore has to
 * ask first, or an SSH-1 key never loads at all and the user gets an error box
 * naming a file that is perfectly fine.
 */
static int kageant_can_defer(const char *path)
{
    Filename *fn = filename_from_str(path);
    int type = key_type(fn);
    filename_free(fn);
    return type != SSH_KEYTYPE_SSH1;
}

/*
 * Load one startup entry and keep it only if it is the key we recorded.
 *
 * Deferred FIRST, always, whatever the entry says: a deferred add asks for
 * nothing, so the fingerprint is checked before anyone is invited to type a
 * passphrase at a file that may have been swapped. Only once it is the right
 * key does an entry stored as "decrypt at load" get decrypted - which re-reads
 * the file, so the result is checked again.
 *
 * There is no dialog on this path any more. It used to raise a modal yes/no at
 * startup where "Yes" adopted the new fingerprint - one click, made while
 * logging in and looking at something else, permanently ratified a key swap.
 * Consent for a changed key now happens in the key list, deliberately, on a row
 * the user went and opened.
 *
 * Returns 1 loaded and verified, 0 the file held a DIFFERENT key, -1 nothing
 * could be loaded from it at all.
 */
/* Audit one on-disk key-load outcome. enc reports what protects the FILE
 * (password/blank) - the field the Hello/smartcard wraps will extend. */
static void kageant_audit_load(const char *ev, const char *path,
                               const char *result)
{
    Filename *fn = filename_from_str(path);
    char *cmt = NULL;
    int enc = ppk_encrypted_f(fn, &cmt);
    filename_free(fn);
    sfree(cmt);
    kitty_audit(ev, "path", path, "result", result,
                "enc", enc ? "password" : "blank", (const char *)NULL);
}

static int kageant_load_startup_entry(const char *path, int encrypted,
                                      const char *fp)
{
    Filename *fn;
    int before = g_nloaded;
    int defer = kageant_can_defer(path);

    fn = filename_from_str(path);
    win_add_keyfile(fn, defer ? true : false);  /* deferred asks nothing */
    filename_free(fn);
    if (g_nloaded == before) {
        kageant_audit_load("loadkey", path, "failed");
        return -1;                             /* would not load at all */
    }
    if (!kageant_verify_loaded(path, fp)) {
        kageant_audit_load("loadkey", path, "fp-refused");
        return 0;                              /* not our key; already removed */
    }

    if (!encrypted && defer) {
        /* The entry wants it decrypted now. That goes through the file again
         * (the same path the key list's Decrypt button uses), so check what we
         * are holding once more afterwards. */
        fn = filename_from_str(path);
        win_add_keyfile(fn, false);
        filename_free(fn);
        if (!kageant_verify_loaded(path, fp)) {
            kageant_audit_load("loadkey", path, "fp-refused");
            return 0;
        }
    }
    kageant_audit_load("loadkey", path, "loaded");

    /*
     * Put the recorded load mode back. The deferred add above tracked this key
     * as ",encrypted", and the startup list is saved from that in-memory state
     * - so without this, loading a ",plain" entry rewrote it as deferred, and
     * the next save made that permanent. The retry path has the same guard for
     * the same reason (measured 2026-08-08; caught again here 2026-08-13, by
     * the harness asserting what ended up in the registry).
     */
    {
        int j;
        for (j = 0; j < g_nloaded; j++)
            if (!stricmp(g_loaded_keypaths[j], path))
                g_loaded_encrypted[j] = encrypted;
    }
    return 1;
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

/*
 * Remember a startup key whose file was not there, so a device event can try it
 * again. Silently full at 64: past that, something is wrong with the list
 * rather than with the media.
 *
 * ⚠️ THE FINGERPRINT IS CLEARED HERE and every caller must set it unless it
 * genuinely has none. An entry with no fingerprint is loaded WITHOUT being
 * checked - that carve-out exists only for entries written by a version that
 * did not record one, so that upgrading does not stop keys loading.
 *
 * Pending entries have two origins:
 *   - the stored startup list, where an old entry may legitimately have none;
 *   - a key being UNLOADED because its media went away (kageant_media_gone),
 *     where the fingerprint is known and MUST be carried over.
 * The second one shipped without it, which turned unplug-swap-replug into an
 * accepted swap. If a third producer appears, it belongs in this list.
 */
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
    g_pending[g_npending].mismatch = 0;  /* ditto - do not inherit a refusal */
    g_pending[g_npending].nofp_refused = 0;   /* ditto */
    g_npending++;
}

/*
 * Park a startup entry that was NOT loaded, with the reason.
 *
 * The entry stays in the user's configuration and gains a row in the key list:
 * a key that is refused has to be visible somewhere, or the only symptom is an
 * SSH login that stopped working.
 *
 * The two reasons are kept apart, because only one of them is an accusation.
 * `mismatch` means the file held a DIFFERENT key and needs the user's consent
 * to resolve; otherwise the file simply would not load - the ordinary `failed`
 * state, which must NOT be reported as "your key was replaced". A stick still
 * being scanned by a virus checker would otherwise raise a swap alarm.
 */
static void kageant_park_unloaded(const char *path, int encrypted, int confirm,
                                  const char *fp, int mismatch)
{
    int n;
    kageant_note_pending(path, encrypted, g_nloaded + g_npending);
    if (g_npending <= 0)
        return;
    n = g_npending - 1;
    g_pending[n].confirm = confirm;
    if (fp && *fp)
        snprintf(g_pending[n].fp, sizeof(g_pending[0].fp), "%s", fp);
    g_pending[n].mismatch = mismatch ? 1 : 0;
    g_pending[n].failed = mismatch ? 0 : 1;
}

/*
 * Try the pending keys again. Called when a device arrives, and from the key
 * list's "Retry unavailable keys" button.
 *
 * Loaded DEFERRED whatever the entry said: a deferred load never asks for a
 * passphrase - it is wanted at first use - and a device event is no moment to
 * put a modal prompt in front of someone. The key coming back is silent; using
 * it asks, as it would for any deferred key.
 *
 * manual says the user pressed the button, which changes three things:
 *
 *  - the [Agent] retrykeys setting no longer applies. That setting governs
 *    retrying by itself, and this is not by itself.
 *  - entries parked as "failed" are tried once more. Having just fixed the
 *    file is the reason to press the button.
 *  - the outcome is always reported, "nothing happened" included - see
 *    kageant_note_retry_result(). The automatic path deliberately reports only
 *    news; a button that can be pressed to no visible effect is a bug.
 *
 * The load stays deferred either way: a passphrase prompt raised out of a
 * button press is still a modal box appearing where nobody asked for one.
 */
/*
 * Retry mode (c), "from their stored path on any drive": the stored path with
 * its drive letter replaced by a letter that JUST ARRIVED, if a file actually
 * exists there. One GetFileAttributes per pending key per arrived letter and
 * no enumeration - drives are never scanned, only the letter(s) the device
 * broadcast named are tried. A UNC or relative path has no drive letter, so
 * the mode does not apply to it; the stored letter itself is skipped because
 * the stored-path probe already covers it.
 *
 * Returns buf (the candidate path) or NULL when nothing was found.
 */
static char *kageant_anydrive_probe(const KageantPendingKey *k,
                                    unsigned long arrived_mask,
                                    char *buf, size_t bufsz)
{
    int bit;
    if (!(isalpha((unsigned char)k->path[0]) && k->path[1] == ':'))
        return NULL;
    for (bit = 0; bit < 26; bit++) {
        if (!(arrived_mask & (1UL << bit)))
            continue;
        if (toupper((unsigned char)k->path[0]) == 'A' + bit)
            continue;
        snprintf(buf, bufsz, "%c%s", 'A' + bit, k->path + 1);
        if (GetFileAttributesA(buf) != INVALID_FILE_ATTRIBUTES)
            return buf;
    }
    return NULL;
}

static void kageant_retry_pending_pass(int manual, unsigned long arrived_mask)
{
    int i, w = 0, loaded_any = 0, rewrote_any = 0;
    int mismatch_new = 0, unchecked = 0, nofp_new = 0;
    int loaded_n = 0, refused_n = 0, absent_n = 0, broken_n = 0;
    int mode = kageant_retry_keys();

    if (!mode && !manual)
        return;
    if (!g_npending) {
        if (manual)
            kageant_note_retry_result(0, 0, 0, 0, 0);
        return;
    }
    if (manual)
        for (i = 0; i < g_npending; i++)
            g_pending[i].failed = 0;

    for (i = 0; i < g_npending; i++) {
        int before;
        const char *loadpath = g_pending[i].path;
        int on_new_drive = 0;
        char altbuf[MAX_PATH + 1];

        if (g_pending[i].failed ||
            GetFileAttributesA(g_pending[i].path) == INVALID_FILE_ATTRIBUTES) {
            int absent = GetFileAttributesA(g_pending[i].path) ==
                         INVALID_FILE_ATTRIBUTES;
            /*
             * Not at its stored path. Mode (c): the same path on the drive
             * that just arrived. Only the AUTOMATIC pass gets here with a
             * mask - the manual button carries none, deliberately: a button
             * press supplies no evidence about which volume is which, so
             * manual retry stays a stored-path affair.
             */
            if (absent && !g_pending[i].failed &&
                mode == KAGEANT_RETRY_ANYDRIVE && arrived_mask &&
                kageant_anydrive_probe(&g_pending[i], arrived_mask,
                                       altbuf, sizeof(altbuf))) {
                if (!g_pending[i].fp[0]) {
                    /*
                     * No recorded fingerprint, so nothing to admit this file
                     * by - REFUSED, never adopted. Loading it would mean
                     * auto-loading a key we never recorded from media we
                     * picked ourselves. (An fp-less entry still loads from
                     * its RECORDED path - the user chose that path.)
                     */
                    if (!g_pending[i].nofp_refused) {
                        g_pending[i].nofp_refused = 1;
                        nofp_new++;        /* announced once, not per message */
                    }
                    if (w != i) g_pending[w] = g_pending[i];
                    w++;
                    continue;
                }
                loadpath = altbuf;
                on_new_drive = 1;
                /* fall through into the load below */
            } else {
                /* Still not there, or there and broken. Either way it stays
                 * on the list: the list is also what keeps the entry in the
                 * saved startup keys, and a key must not vanish from a user's
                 * configuration because one load went wrong. */
                if (absent)
                    absent_n++;
                else
                    broken_n++;
                if (w != i) g_pending[w] = g_pending[i];
                w++;
                continue;
            }
        }
        /*
         * Nothing recorded to check against: it loads, and the fingerprint of
         * whatever loaded becomes the baseline. Counted so the user is told it
         * went unverified this once.
         */
        if (!g_pending[i].fp[0])
            unchecked++;
        {
            Filename *fn = filename_from_str(loadpath);
            int j;
            before = g_nloaded;
            g_startup_loading = 1;             /* a failure now is a startup one */
            kitty_hello_batch_begin();  /* one gesture covers the batch */
            /* Deferred where that is possible - see kageant_can_defer. An
             * SSH-1 key asked for deferred is refused outright, so this path
             * used to fail every SSH-1 key on every device arrival. */
            win_add_keyfile(fn, kageant_can_defer(loadpath) ?
                                true : false);
            g_startup_loading = 0;
            kitty_hello_batch_end();
            filename_free(fn);

            /*
             * Is what we are now holding the key we recorded?
             *
             * This check used to run only at startup, which left the ONE place
             * a swap is easiest - removable media - as the one place nothing
             * was checked: a stick absent at login lands here, and whatever
             * file appeared at that path was loaded unverified.
             *
             * A mismatch does NOT prompt. A device arriving is no moment for a
             * dialog, and a fingerprint change is not something to wave away
             * with one click either. The key is put back out of the agent, the
             * entry is marked, and the user is told - see
             * kageant_note_verify_problem().
             */
            if (g_nloaded != before &&
                !kageant_verify_loaded(loadpath, g_pending[i].fp)) {
                if (!g_pending[i].mismatch) {
                    g_pending[i].mismatch = 1;
                    mismatch_new++;        /* only a CHANGE is worth a notice */
                }
                refused_n++;               /* every pass, for the manual reply */
                if (w != i) g_pending[w] = g_pending[i];
                w++;
                continue;
            }
            g_pending[i].mismatch = 0;     /* it is the right key again */

            if (g_nloaded == before) {
                /* Did not load. Keep the entry, but stop trying it: the file is
                 * present and broken, the user has already been told, and
                 * repeating the box at every device event would be its own
                 * annoyance. Restarting kageant tries again from scratch, and
                 * so does the key list's "Retry unavailable keys".
                 *
                 * Unless the broken file sat on a NEW drive: parking the entry
                 * then would also stop its stored path from being retried, over
                 * a file that is not even the recorded one. */
                if (!on_new_drive)
                    g_pending[i].failed = 1;
                broken_n++;
                if (w != i) g_pending[w] = g_pending[i];
                w++;
                continue;
            }

            /*
             * Only now, AFTER the fingerprint matched, does the entry's stored
             * path move to the new drive - never before, so a wrong-but-
             * plausible path can not end up written into the configuration.
             * The re-save below makes it stick, and the next start needs no
             * retry at all.
             */
            if (on_new_drive) {
                snprintf(g_pending[i].path, sizeof(g_pending[i].path),
                         "%s", loadpath);
                rewrote_any = 1;
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
                kageant_apply_confirm_by_path(g_pending[i].path,
                                              g_pending[i].confirm);
            loaded_any = 1;
            loaded_n++;
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

    /* A path moved to a new drive: write the startup list out with the new
     * letter, so the next start finds the key without any retry. Only reached
     * after a fingerprint match - see above. */
    if (rewrote_any)
        kageant_save_startup_keys();

    /* One summary line per pass with anything to say - quiet passes on an
     * unrelated device arrival stay out of the log. */
    if (loaded_n || refused_n || broken_n || nofp_new || (manual && absent_n)) {
        char nums[4][12];
        snprintf(nums[0], sizeof(nums[0]), "%d", loaded_n);
        snprintf(nums[1], sizeof(nums[1]), "%d", refused_n);
        snprintf(nums[2], sizeof(nums[2]), "%d", absent_n);
        snprintf(nums[3], sizeof(nums[3]), "%d", broken_n);
        kitty_audit("retry", "trigger", manual ? "button" : "device-arrival",
                    "loaded", nums[0], "refused", nums[1],
                    "absent", nums[2], "broken", nums[3], (const char *)NULL);
    }

    if (manual)
        kageant_note_retry_result(loaded_n, refused_n, absent_n, broken_n,
                                  unchecked);
    else
        kageant_note_verify_problem(mismatch_new, unchecked, nofp_new);

    kageant_refresh_tray_tip();    /* the held-back set may have changed */
}

/*
 * The fingerprint of the file sitting at `path` right now, or NULL. Free it.
 * The key list uses it to show the user what a changed file actually holds
 * before they decide whether to accept it.
 */
char *kageant_fp_of_file(const char *path)
{
    strbuf *blob = kageant_pubblob(path);
    char *fp;
    if (!blob)
        return NULL;
    fp = kageant_fp_of_blob(blob);
    strbuf_free(blob);
    return fp;
}

/*
 * The user has looked at a changed key in the key list and said yes to it.
 *
 * This is the ONLY way a new fingerprint is ever adopted. It replaces the
 * modal that used to appear during startup, where a single click - made while
 * logging in, with the dialog in front of whatever else was happening -
 * permanently accepted a swapped key file.
 *
 * The entry's stored fingerprint is only rewritten AFTER the key has loaded and
 * been verified against what was actually accepted, so a file that changes
 * again between the click and the load does not get ratified by it.
 *
 * Returns 1 if the key is now loaded and the entry updated, 0 otherwise.
 */
int kageant_accept_pending_key(const char *path)
{
    int i, v;
    char *actual;

    for (i = 0; i < g_npending; i++)
        if (!stricmp(g_pending[i].path, path))
            break;
    if (i >= g_npending || !g_pending[i].mismatch)
        return 0;

    actual = kageant_fp_of_file(path);
    if (!actual)
        return 0;                     /* cannot read it now: nothing to accept */

    /* Load it and check we are holding what the user was shown. */
    v = kageant_load_startup_entry(path, g_pending[i].encrypted, actual);
    sfree(actual);
    if (v != 1)
        return 0;

    if (g_pending[i].confirm)
        kageant_apply_confirm_by_path(path, g_pending[i].confirm);

    /*
     * Off the pending list - the key is loaded now, so it belongs to the loaded
     * list instead, and saving writes its NEW fingerprint out from the key the
     * agent is holding. Spliced here rather than through kageant_drop_pending(),
     * which also deletes the stored entry: that would take the key out of the
     * user's startup list for the moment between the two writes, and this is an
     * acceptance, not a removal.
     */
    {
        int j;
        for (j = i; j < g_npending - 1; j++)
            g_pending[j] = g_pending[j + 1];
        g_npending--;
    }
    kageant_save_startup_keys();
    kageant_apply_saved_order();
    kageant_refresh_tray_tip();     /* one fewer key held back */
    return 1;
}

/*
 * The user has pointed a not-loaded startup entry at a file they chose - the
 * key details' "Locate..." button, offered only on a row whose recorded file
 * is ABSENT or UNPARSEABLE. Never on a mismatch: "Accept this key" owns that
 * case and is deliberately the only place a changed key file can be ratified.
 *
 * The entry is rewritten IN PLACE - it keeps its slot in the offer order and
 * its confirm marker, which Remove + Add Key would both lose. An entry with a
 * recorded fingerprint accepts only a file holding THAT key; an entry with
 * none adopts the chosen file's fingerprint - the user picked this very file,
 * which is the same consent Add Key rests on.
 *
 * If the same key is recorded at several locations and ALL are unreachable,
 * they collapse into the one just chosen: there is no way to tell a
 * deliberate second location from the same key added off three different
 * sticks. Consequence accepted: a temporarily-down network share among them
 * is forgotten and would need a fresh Add Key.
 *
 * Returns 1 loaded and re-pointed, 0 the chosen file holds a DIFFERENT key
 * (refused, nothing changed), -1 nothing could be loaded from it at all.
 */
int kageant_locate_pending_key(const char *oldpath, const char *newpath)
{
    int i, j, v;
    char fp[160];

    for (i = 0; i < g_npending; i++)
        if (!stricmp(g_pending[i].path, oldpath))
            break;
    if (i >= g_npending || g_pending[i].mismatch)
        return -1;

    if (g_pending[i].fp[0]) {
        snprintf(fp, sizeof(fp), "%s", g_pending[i].fp);
    } else {
        /* Adopt what the chosen file holds; passing it through the loader's
         * verify step still catches the file changing between this read and
         * the load. */
        char *actual = kageant_fp_of_file(newpath);
        if (!actual)
            return -1;
        snprintf(fp, sizeof(fp), "%s", actual);
        sfree(actual);
    }

    v = kageant_load_startup_entry(newpath, g_pending[i].encrypted, fp);
    if (v != 1)
        return v;          /* 0 = a different key; -1 = would not load */

    if (g_pending[i].confirm)
        kageant_apply_confirm_by_path(newpath, g_pending[i].confirm);

    /*
     * Off the pending list - and every other UNREACHABLE entry recording the
     * same key goes with it: the chosen location replaces them all. Spliced
     * by hand rather than through kageant_drop_pending() for the same reason
     * as the accept path - this is a re-pointing, not a removal, and the
     * stored entry must never blink out between two writes.
     */
    {
        int w = 0;
        for (j = 0; j < g_npending; j++) {
            int drop = (j == i) ||
                (g_pending[j].fp[0] && !strcmp(g_pending[j].fp, fp) &&
                 GetFileAttributesA(g_pending[j].path) ==
                     INVALID_FILE_ATTRIBUTES);
            if (!drop) {
                if (w != j) g_pending[w] = g_pending[j];
                w++;
            }
        }
        g_npending = w;
    }
    kageant_save_startup_keys();
    kageant_apply_saved_order();
    kageant_refresh_tray_tip();
    return 1;
}

/*
 * KiTTY: somebody just asked for our keys while we are holding one back.
 *
 * This is the moment a refused key actually costs something - the login that
 * is about to fail - rather than back when the stick was plugged in and the
 * balloon may have been missed. It is also the ONLY trigger available: a
 * refused key is not in the identity list, so nothing ever asks to sign with
 * it.
 *
 * Rate-limited hard, because every SSH connection asks this, as do scp, git and
 * any scripted plink - firing per request would be a balloon storm, and a
 * warning that appears constantly is one people learn to dismiss:
 *
 *  - once per held-back SET. The count is the set's identity; it changes when a
 *    key is refused or resolved, and only then does this re-arm.
 *  - never while the key list is open. The user is already looking at the
 *    answer.
 */
void kageant_do_identities_asked(unsigned long pid)
{
    static int announced_for = -1;      /* mismatch count already announced */
    int held = kageant_mismatch_count();
    char text[512];
    char who[80];

    if (held <= 0) {
        announced_for = -1;             /* nothing held back: re-arm */
        return;
    }
    if (held == announced_for)
        return;                         /* same set, already said once */
    if (!traywindow || kageant_keylist_open())
        return;

    who[0] = '\0';
    if (pid) {
        char path[MAX_PATH + 1];
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                               (DWORD)pid);
        if (h) {
            DWORD n = sizeof(path);
            if (QueryFullProcessImageNameA(h, 0, path, &n) && path[0]) {
                const char *base = strrchr(path, '\\');
                snprintf(who, sizeof(who), " %s", base ? base + 1 : path);
            }
            CloseHandle(h);
        }
        if (!who[0])
            snprintf(who, sizeof(who), " a program (pid %lu)", pid);
    }

    snprintf(text, sizeof(text),
             held == 1 ?
             "Something%s just asked for your keys, and one of them is NOT "
             "loaded: the file is not the key recorded for it. If that login "
             "fails, this is why. Click to see it." :
             "Something%s just asked for your keys, and %d of them are NOT "
             "loaded: the files are not the keys recorded for them. If a login "
             "fails, this is why. Click to see them.",
             who, held);

    kitty_notice_show("kageant: a key is being held back", text,
                      KAGEANT_NOTICE_WARN, kageant_notice_seconds(12),
                      traywindow, KAGEANT_WM_NOTICE_CLICK);
    announced_for = held;
}

/* On a device arriving. The mask names the letter(s) that arrived (bit 0 =
 * A:), or 0 when the broadcast did not name a volume - retry mode (c) then
 * probes nothing beyond the stored paths. */
void kageant_retry_pending_keys(unsigned long arrived_mask)
{
    kageant_retry_pending_pass(0, arrived_mask);
}

/* On the key list's "Retry unavailable keys". Also the only way to drive the retry
 * path without hardware: Windows will not let one process send another a
 * WM_DEVICECHANGE (SendMessageTimeout fails with 87, PostMessage with 1159),
 * so this button is what the fingerprint-refusal test can reach. */
void kageant_retry_pending_keys_now(void) { kageant_retry_pending_pass(1, 0); }

/*
 * KiTTY: say what verification did, without becoming noise.
 *
 * Called after a load pass (device arrival today). Two different things to
 * report, and only when they are NEWS:
 *
 *  - mismatch_new: keys refused this pass because the file is not the key we
 *    recorded. Counted per TRANSITION, not per pass: Windows sends several
 *    WM_DEVICECHANGE messages for one insertion, and a refused entry stays on
 *    the pending list, so reporting per pass would also announce it when an
 *    unrelated mouse or phone was plugged in.
 *
 *  - unchecked: keys loaded with no recorded fingerprint to compare against.
 *    That is one unverified load each, after which the fingerprint of whatever
 *    loaded becomes the baseline - worth saying once.
 *
 *  - nofp_newdrive: a file matching a startup key's path (minus its drive)
 *    appeared on a drive that just arrived, but the entry has no recorded
 *    fingerprint, so there is nothing to admit the file by and it was refused.
 *    Also per transition, via the entry's nofp_refused flag. The wording must
 *    NOT send the user at the Retry button: manual retry only ever loads from
 *    the RECORDED path, so pressing it would do nothing here.
 *
 * A notice, never a prompt: this fires when hardware appeared, which is not a
 * moment to demand an answer. Clicking it opens the key list, where the State
 * column says which key - and that list IS the persistent record, because the
 * agent has no event log of its own (debug_logevent lives in kitty_win.c and
 * is not linked here). A balloon is easily missed, so the refused entry must
 * stay visible there until it is dealt with.
 */
void kageant_note_verify_problem(int mismatch_new, int unchecked,
                                 int nofp_newdrive)
{
    char text[512];

    if (mismatch_new > 0) {
        snprintf(text, sizeof(text),
                 mismatch_new == 1 ?
                 "A key file was NOT loaded: it is not the key recorded for "
                 "that path. Click to see which - its State in the key list "
                 "reads \"mismatch\"." :
                 "%d key files were NOT loaded: they are not the keys recorded "
                 "for those paths. Click to see which - their State in the key "
                 "list reads \"mismatch\".",
                 mismatch_new);
        if (traywindow)
            kitty_notice_show("kageant: key file changed", text,
                              KAGEANT_NOTICE_WARN, kageant_notice_seconds(12),
                              traywindow, KAGEANT_WM_NOTICE_CLICK);
    }

    if (unchecked > 0) {
        snprintf(text, sizeof(text),
                 unchecked == 1 ?
                 "A key was loaded with no fingerprint on record, so nothing "
                 "could be checked. Its fingerprint is recorded now and it "
                 "will be checked from here on." :
                 "%d keys were loaded with no fingerprint on record, so "
                 "nothing could be checked. Their fingerprints are recorded "
                 "now and they will be checked from here on.",
                 unchecked);
        if (traywindow)
            kitty_notice_show("kageant: keys loaded unverified", text,
                              KAGEANT_NOTICE_INFO, kageant_notice_seconds(10),
                              traywindow, KAGEANT_WM_NOTICE_CLICK);
    }

    if (nofp_newdrive > 0) {
        snprintf(text, sizeof(text),
                 nofp_newdrive == 1 ?
                 "A file matching a startup key's path appeared on the new "
                 "drive, but that key has no fingerprint on record to check "
                 "it against, so it was NOT loaded. Load the key once from "
                 "its recorded path (or Add Key) to record one." :
                 "%d files matching startup keys' paths appeared on the new "
                 "drive, but those keys have no fingerprints on record to "
                 "check them against, so they were NOT loaded. Load each key "
                 "once from its recorded path (or Add Key) to record one.",
                 nofp_newdrive);
        if (traywindow)
            kitty_notice_show("kageant: key on a new drive not loaded", text,
                              KAGEANT_NOTICE_WARN, kageant_notice_seconds(12),
                              traywindow, KAGEANT_WM_NOTICE_CLICK);
    }
}

/*
 * KiTTY: answer a "Retry unavailable keys".
 *
 * Always exactly one notice, whatever happened - including nothing. The
 * automatic path reports only news, on purpose: it fires on hardware events
 * and a refused entry stays refused across every later one. Reused here that
 * would leave the button silent precisely when it is pressed a second time on
 * a key that is still wrong, which reads as "the button is broken".
 *
 * Counts, not names: several entries can be in different states in one pass,
 * and the key list is where a per-key answer belongs. Clicking the notice
 * opens it.
 */
void kageant_note_retry_result(int loaded, int refused, int absent,
                               int broken, int unchecked)
{
    char text[512];
    int n = 0;
    int warn = (refused > 0 || broken > 0);

    if (!loaded && !refused && !absent && !broken) {
        if (traywindow)
            kitty_notice_show("kageant: nothing to retry",
                              "No key is waiting to be loaded. Every "
                              "remembered key is either loaded already or "
                              "not in the startup list.",
                              KAGEANT_NOTICE_INFO, kageant_notice_seconds(8),
                              traywindow, KAGEANT_WM_NOTICE_CLICK);
        return;
    }

    if (loaded > 0)
        n += snprintf(text + n, sizeof(text) - n,
                      "%d key%s loaded.%s ", loaded, loaded == 1 ? "" : "s",
                      unchecked > 0 ?
                      " There was no fingerprint on record for some of them,"
                      " so nothing could be checked this once - what loaded"
                      " is the baseline from here on." : "");
    if (refused > 0)
        n += snprintf(text + n, sizeof(text) - n,
                      "%d refused: the file is not the key recorded for that "
                      "path. ", refused);
    if (absent > 0)
        n += snprintf(text + n, sizeof(text) - n,
                      "%d still not there. ", absent);
    if (broken > 0)
        n += snprintf(text + n, sizeof(text) - n,
                      "%d could not be read as a key. ", broken);
    /* All four clauses together come to well under sizeof(text), but snprintf
     * returns what it WANTED to write, so an offset walked past the end would
     * turn the size argument negative and enormous. */
    if (n < 0 || n > (int)sizeof(text) - 1)
        n = (int)sizeof(text) - 1;
    snprintf(text + n, sizeof(text) - n, "Click to see which.");

    if (traywindow)
        kitty_notice_show(warn ? "kageant: keys not loaded"
                               : "kageant: retry finished", text,
                          warn ? KAGEANT_NOTICE_WARN : KAGEANT_NOTICE_INFO,
                          kageant_notice_seconds(warn ? 12 : 8),
                          traywindow, KAGEANT_WM_NOTICE_CLICK);
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

/* KiTTY: was this entry refused because the file is not the key we recorded?
 * Separate accessor so the older one keeps its signature and its callers. */
int kageant_pending_mismatch(int i)
{
    return (i >= 0 && i < g_npending) ? g_pending[i].mismatch : 0;
}

/* KiTTY: how many entries are currently refused for a fingerprint mismatch -
 * the number the tray tooltip and the key list want. */
int kageant_mismatch_count(void)
{
    int i, n = 0;
    for (i = 0; i < g_npending; i++)
        if (g_pending[i].mismatch)
            n++;
    return n;
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
    /* the entry just dropped may have been one of the held-back ones */
    kageant_refresh_tray_tip();
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
            !stricmp(c + 1, "confirm") || !stricmp(c + 1, "helloconfirm") ||
            strstr(c + 1, "SHA256:"))
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
                    /* KiTTY: a Hello-protected key travels WITH its
                     * sidecar, or the copy could only be opened by the
                     * printed secret. */
                    if (kageant_hello_has_sidecar(abspath)) {
                        char *s1 = kageant_hello_sidecar_path(abspath);
                        char *s2 = kageant_hello_sidecar_path(dest);
                        CopyFileA(s1, s2, TRUE);
                        sfree(s1);
                        sfree(s2);
                    }
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
    if (!kitty_inilight_registry_authoritative()) {
        /* KiTTY: a shortcut of that NAME existing is not the question - it may
         * point at another kageant entirely, in which case "starts at login"
         * would be a claim about somebody else's agent. Check the target, as
         * the registry branch below already does. */
        char myexe[MAX_PATH];
        if (!GetModuleFileNameA(NULL, myexe, sizeof(myexe)))
            return 0;
        return kitty_startup_shortcut_points_to(KAGEANT_SHORTCUT_NAME, 0, myexe);
    }
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

/* A stored "path" that is really a stray format token. 0.84.1.71's registry
 * loader stripped an entry's trailing ,plain/,encrypted/,SHA256:... tokens
 * IN PLACE in the REG_MULTI_SZ buffer, so its walk re-entered the record and
 * read each token as one more entry - and the next save persisted those
 * phantoms as startup keys (",encrypted" appended, the legacy default). The
 * parser bug is fixed (both branches below parse a copy), but every .71
 * install with fingerprinted entries still carries the phantoms it wrote:
 * recognise them so the loaders can scrub them. Only ever consulted for an
 * entry whose file does NOT exist, so a real key file that happened to have
 * such a name is never touched. */
static int kageant_entry_is_phantom(const char *path)
{
    return !stricmp(path, "plain") || !stricmp(path, "encrypted") ||
           !stricmp(path, "confirm") || !stricmp(path, "helloconfirm") ||
           !strnicmp(path, "SHA256:", 7);
}

/* Re-add remembered startup keys. A ,encrypted entry loads deferred
 * (passphrase on first use); ,plain loads immediately. Missing files are
 * counted (kageant_startup_missing) and skipped, not purged. */
void kageant_load_startup_keys(void)
{
    const char *f;
    /* Keys loaded with no fingerprint on record - reported once at the end, so
     * the first start after an upgrade says what it did rather than doing it
     * silently. */
    int startup_unchecked = 0, startup_mismatch = 0;
    g_startup_missing = 0;

    if (!kitty_inilight_registry_authoritative() &&
        (f = kitty_inilight_file()) != NULL) {
        char key[32], val[MAX_PATH + 32], abspath[MAX_PATH + 1];
        int i, gap;
        g_startup_loading = 1;
        kitty_hello_batch_begin();  /* one gesture covers the startup load */
        /* Tolerate gaps in the numbering: a hand-edit that deletes one
         * startupkeyN line must not truncate the rest of the list. Stop only
         * after a run of empty slots (matching the save-side clear scan). */
        for (i = 1, gap = 0; gap < 8; i++) {
            int enc = 0, conf = 0;
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
                else if (!stricmp(c + 1, "helloconfirm")) { conf = 2; *c = '\0'; }
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
                if (kageant_entry_is_phantom(val)) {
                    /* a .71 parser phantom: drop it, and rewrite the stored
                     * list after the loop so it stays gone */
                    g_fp_adopted = 1;
                    continue;
                }
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
            /* Load it, and keep it only if it is the key we recorded. A
             * different key, or one that will not load, is parked with the
             * reason instead - no dialog at login, and the key list is where it
             * gets resolved. */
            {
                int v = kageant_load_startup_entry(abspath, enc, fp);
                if (v != 1) {
                    kageant_park_unloaded(abspath, enc, conf, fp, v == 0);
                    if (v == 0) startup_mismatch++;
                    continue;
                }
                if (conf)
                    kageant_apply_confirm_by_path(abspath, conf);
            }
            /* Write the list out afterwards if anything changed - the common
             * case on the first run after an upgrade is an entry that had no
             * fingerprint yet and has now been identified.
             *
             * AFTER the loop, never inside it: the list is saved from the keys
             * loaded so far, so saving half way through would truncate it to
             * whatever had loaded by then. */
            if (!fp[0])
                g_fp_adopted = 1;
            if (!fp[0])
                startup_unchecked++;   /* loaded with nothing to compare */
        }
        g_startup_loading = 0;
        kitty_hello_batch_end();
        if (g_fp_adopted) {
            g_fp_adopted = 0;
            kageant_save_startup_keys();   /* now the list is complete */
        }
        /* Say so once, after the list is complete - not per key. */
        kageant_note_verify_problem(startup_mismatch, startup_unchecked, 0);
        kageant_refresh_tray_tip();
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
                kitty_hello_batch_begin();
                for (char *p = buf; *p; p += strlen(p) + 1) {
                    int enc = 1;   /* legacy entries had no marker: deferred */
                    int conf = 0;

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
                        else if (!stricmp(c + 1, "helloconfirm")) { conf = 2; *c = '\0'; }
                        else if (strstr(c + 1, "SHA256:")) {
                            snprintf(fp, sizeof(fp), "%s",
                                     strstr(c + 1, "SHA256:"));
                            *c = '\0';
                        } else break;
                    }
                    if (GetFileAttributesA(entry) == INVALID_FILE_ATTRIBUTES) {
                        if (kageant_entry_is_phantom(entry)) {
                            /* a .71 parser phantom - see the helper above */
                            g_fp_adopted = 1;
                            continue;
                        }
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
                    {   /* see the ini branch above for all of this */
                        int v = kageant_load_startup_entry(entry, enc, fp);
                        if (v != 1) {
                            kageant_park_unloaded(entry, enc, conf, fp, v == 0);
                            if (v == 0) startup_mismatch++;
                            continue;
                        }
                        if (conf)
                            kageant_apply_confirm_by_path(entry, conf);
                    }
                    if (!fp[0])
                        g_fp_adopted = 1;   /* see the ini branch above */
                    if (!fp[0])
                        startup_unchecked++;
                }
                g_startup_loading = 0;
                kitty_hello_batch_end();
                if (g_fp_adopted) {
                    g_fp_adopted = 0;
                    kageant_save_startup_keys();
                }
                kageant_note_verify_problem(startup_mismatch, startup_unchecked, 0);
                kageant_refresh_tray_tip();
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
        return kageant_reg_read_dword(KAGEANT_REG_NOTICESECS, &reg_v) ? reg_v :
               (ini_v > 0 ? ini_v : 0);
    if (ini_v >= 0)
        return ini_v;
    return kageant_reg_read_dword(KAGEANT_REG_NOTICESECS, &reg_v) ? reg_v : 0;
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
    int blocked;
    if (kageant_lockdown_get())
        blocked = 1;   /* add + remove + remove-all all blocked */
    else if (op == KAGEANT_MUT_ADD)
        blocked = kageant_policy_get("blockipcadd", "BlockIpcAdd");
    else   /* remove and remove-all share the one switch */
        blocked = kageant_policy_get("blockipcremove", "BlockIpcRemove");
    if (blocked)
        kageant_audit_use(op == KAGEANT_MUT_ADD    ? "add" :
                          op == KAGEANT_MUT_REMOVE ? "remove" : "remove-all",
                          NULL, NULL, "blocked", "ipc-policy",
                          pageant_external_pid);
    return blocked;
}

void kageant_do_mutation_notice(int op, const char *comment)
{
    char proc[MAX_PATH + 32];
    const char *title;
    char *text;

    /* Audited BEFORE the notify gate: turning notices off must never turn
     * the audit trail off with it. */
    kageant_audit_use(op == KAGEANT_MUT_ADD    ? "add" :
                      op == KAGEANT_MUT_REMOVE ? "remove" : "remove-all",
                      NULL, comment, "done", "ipc", pageant_external_pid);

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
 * kageant_comment_confirm_hook) on EVERY add path, turning the convention
 * into the real, sticky per-key flag. There is deliberately no sign-time
 * re-check of the comment: it could not tell a flag the user cleared in Key
 * details from one that was never set, so it would silently override that
 * "No". The per-key flag is the single source of truth at sign time. */
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
    if (g_confirm_suppress) {
        kageant_audit_use("confirm", NULL, comment, "denied",
                          "suppress-latch", 0);
        return 0;
    }

    /* One box at a time. A hostile client can fire many sign requests; the
     * MessageBox modal loop pumps messages, so a second request can arrive
     * and stack another box on top. Deny the pile-up rather than let a bad
     * client fill the screen while the user is trying to say no. */
    if (g_confirm_active) {
        kageant_audit_use("confirm", NULL, comment, "denied", "pileup", 0);
        return 0;
    }

    /*
     * Windows Hello instead of a button - when this key's own mode says so,
     * or the global [Agent] helloconfirm toggle upgrades every confirmation.
     * A click can be synthesized by same-user code; the Hello prompt cannot.
     *
     * FAIL CLOSED, never a Yes/No fallback: falling back to the box would
     * convert the gate back to exactly what it replaced, precisely in the
     * remote-driving scenario where Hello is unavailable. The refusal is
     * announced, because the remote symptom is a bare "Permission denied".
     */
    if (key_confirm == 2 || kageant_hello_get()) {
        char *msg;
        int hr, allowed;

        g_confirm_active = 1;
        msg = dupprintf("Allow this use of the SSH key \"%s\"?",
                        comment && *comment ? comment : "(unnamed key)");
        hr = kitty_hello_verify(traywindow, msg);
        sfree(msg);
        g_confirm_active = 0;

        allowed = (hr == KITTY_HELLO_VERIFIED);
        kageant_audit_use("confirm", NULL, comment,
                          allowed ? "allowed" : "denied",
                          hr == KITTY_HELLO_VERIFIED    ? "hello-verified" :
                          hr == KITTY_HELLO_DENIED      ? "hello-denied" :
                          hr == KITTY_HELLO_UNAVAILABLE ? "hello-unavailable" :
                                                          "hello-error", 0);
        if (!allowed && hr != KITTY_HELLO_DENIED && traywindow)
            kitty_notice_show(
                "kageant: key use DENIED",
                hr == KITTY_HELLO_UNAVAILABLE ?
                "A key use was denied: it requires a Windows Hello check, "
                "and Hello is not available in this session (no Hello "
                "credential, policy, or a remote desktop). The request was "
                "REFUSED - it is never downgraded to a plain click." :
                "A key use was denied: the Windows Hello check could not "
                "be carried out.",
                KAGEANT_NOTICE_WARN, kageant_notice_seconds(12),
                traywindow, KAGEANT_WM_NOTICE_CLICK);
        return allowed;
    }

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

        kageant_audit_use("confirm", NULL, comment,
                          r == IDYES ? "allowed" : "denied",
                          r == IDYES    ? "click-allow" :
                          r == IDCANCEL ? "click-cancel-latch" : "click-deny",
                          0);
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
extern void (*kageant_notify_hook)(const char *comment,
                                   const char *fingerprint);
/* KiTTY: the key list tints a row for a moment when that key is used, so a
 * signature you did not expect is visible while it happens. Amber = refused,
 * blue = signed, matching the notice windows. Keyed by public blob: the list
 * is keyed that way too, and comments repeat or are empty. A handful of slots
 * is plenty - the tint lasts a second. */
#define KAGEANT_FLASH_SLOTS 8
#define KAGEANT_FLASH_MS    1200
static struct kageant_flash {
    char *fp;            /* SHA256 fingerprint string, as the rows carry */
    DWORD until;
    int allowed;
} g_flash[KAGEANT_FLASH_SLOTS];

/* 1 and the verdict if this key was used within the last KAGEANT_FLASH_MS. */
int kageant_flash_get(const char *fingerprint, int *allowed)
{
    DWORD now = GetTickCount();
    int i;
    if (!fingerprint)
        return 0;
    for (i = 0; i < KAGEANT_FLASH_SLOTS; i++) {
        if (!g_flash[i].fp || strcmp(g_flash[i].fp, fingerprint))
            continue;
        if ((int)(g_flash[i].until - now) <= 0)
            return 0;
        if (allowed)
            *allowed = g_flash[i].allowed;
        return 1;
    }
    return 0;
}

/* Any tint still live? The key list keeps its repaint timer only while so. */
int kageant_flash_any(void)
{
    DWORD now = GetTickCount();
    int i;
    for (i = 0; i < KAGEANT_FLASH_SLOTS; i++)
        if (g_flash[i].fp && (int)(g_flash[i].until - now) > 0)
            return 1;
    return 0;
}

void kageant_note_keyuse(const char *fingerprint, const char *comment,
                         int allowed, unsigned long req_pid)
{
    DWORD now = GetTickCount();
    int i, use = -1;
    if (!fingerprint || !*fingerprint)
        return;
    /* THE audit line of the whole feature: what signed (or was refused),
     * and by which process. The COMMENT rides along because that is the
     * name users know a key by - nobody deals in SHA256 strings. */
    kageant_audit_use("sign", fingerprint, comment,
                      allowed ? "allowed" : "denied", NULL, req_pid);
    for (i = 0; i < KAGEANT_FLASH_SLOTS; i++)            /* same key again? */
        if (g_flash[i].fp && !strcmp(g_flash[i].fp, fingerprint)) {
            use = i;
            break;
        }
    if (use < 0)
        for (i = 0; i < KAGEANT_FLASH_SLOTS; i++)        /* a free/expired one */
            if (!g_flash[i].fp || (int)(g_flash[i].until - now) <= 0) {
                use = i;
                break;
            }
    if (use < 0)
        use = 0;                                          /* all live: recycle */
    sfree(g_flash[use].fp);
    g_flash[use].fp = dupstr(fingerprint);
    g_flash[use].until = now + KAGEANT_FLASH_MS;
    g_flash[use].allowed = allowed;
    kageant_keylist_flash_changed();     /* windows/pageant.c: repaint + timer */
}

/* KiTTY: warn ONCE when the in-memory key protection is not working -
 * kageant then holds every key in plain process memory, and a user counting
 * on the CryptProtectMemory design deserves to hear that it is off rather
 * than find out from a dump. On every supported Windows the probe passes, so
 * this notice firing at all means a stripped/emulated system or something
 * hooking the crypt API - itself worth a look. Called from windows/pageant.c
 * once the tray exists (the notice needs a window to click through to); the
 * durable half is the tray-tip line kageant_refresh_tray_tip() adds while
 * the condition holds. The "refuse to hold keys instead" question is policy
 * and stays open in the TODO - this only ends the silence. */
void kageant_warn_unprotected_memory(void)
{
    static int warned = 0;
    if (warned || kitty_protkey_available())
        return;
    warned = 1;
    kitty_notice_show(
        "kageant: keys are NOT memory-protected",
        "Windows' CryptProtectMemory is not working in this process, so "
        "private keys are held in PLAIN memory while loaded. On a normal "
        "Windows this never happens - something is stripping or hooking the "
        "crypt API, which is itself worth investigating.",
        KAGEANT_NOTICE_WARN, kageant_notice_seconds(15),
        traywindow, KAGEANT_WM_NOTICE_CLICK);
}

void kageant_do_notify(const char *comment, const char *fingerprint)
{
    if (!kageant_notify_get() || !traywindow)
        return;
    /* KiTTY: our own notice window, not a tray balloon (the shell ignores
     * balloon durations and often suppresses them). Blue = kageant info; 5s;
     * click opens View Keys. */
    char text[512];
    snprintf(text, sizeof(text), "A key was used to authenticate:\n%s%s%s",
             (comment && *comment) ? comment : "(unnamed key)",
             (fingerprint && *fingerprint) ? "\n" : "",
             (fingerprint && *fingerprint) ? fingerprint : "");
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

/* ------------------------------------------------------------------ *
 * KiTTY: Hello-protected keys - the startup list's side of it.        *
 * ------------------------------------------------------------------ */

/*
 * Swap one tracked path for another (the protected copy replacing the
 * plaintext original it was made from): the entry keeps its slot, its
 * blob (same key) and, unless told otherwise, its load mode. Returns 1
 * replaced, 0 oldpath was not a tracked loaded path.
 */
int kageant_startup_replace_path(const char *oldpath, const char *newpath,
                                 int encrypted)
{
    char want[MAX_PATH + 1], abspath[MAX_PATH + 1];
    int i;
    if (!_fullpath(want, oldpath, sizeof(want)))
        snprintf(want, sizeof(want), "%s", oldpath);
    if (!_fullpath(abspath, newpath, sizeof(abspath)))
        snprintf(abspath, sizeof(abspath), "%s", newpath);
    for (i = 0; i < g_nloaded; i++) {
        if (stricmp(g_loaded_keypaths[i], want))
            continue;
        sfree(g_loaded_keypaths[i]);
        g_loaded_keypaths[i] = dupstr(abspath);
        if (encrypted >= 0)
            g_loaded_encrypted[i] = encrypted ? 1 : 0;
        if (kageant_startup_get())
            kageant_save_startup_keys();
        return 1;
    }
    return 0;
}

/*
 * Forget ONE path of a loaded key - the key stays in the agent, only the
 * startup list stops naming that file (a deleted plaintext original, a
 * retired stick). A pending entry of that path is dropped the same way.
 * Returns 1 if something was forgotten.
 */
int kageant_startup_forget_path(const char *path)
{
    char want[MAX_PATH + 1];
    int i, j, done = 0;
    if (!_fullpath(want, path, sizeof(want)))
        snprintf(want, sizeof(want), "%s", path);
    for (i = 0; i < g_nloaded; i++) {
        if (stricmp(g_loaded_keypaths[i], want))
            continue;
        sfree(g_loaded_keypaths[i]);
        if (i < g_nblobs && g_loaded_blobs[i])
            strbuf_free(g_loaded_blobs[i]);
        for (j = i; j < g_nloaded - 1; j++) {
            g_loaded_keypaths[j] = g_loaded_keypaths[j + 1];
            g_loaded_encrypted[j] = g_loaded_encrypted[j + 1];
            g_loaded_blobs[j] = g_loaded_blobs[j + 1];
        }
        g_nloaded--;
        g_nblobs = g_nloaded;
        done = 1;
        break;
    }
    for (i = 0; i < g_npending; i++) {
        if (stricmp(g_pending[i].path, want))
            continue;
        for (j = i; j < g_npending - 1; j++)
            g_pending[j] = g_pending[j + 1];
        g_npending--;
        done = 1;
        break;
    }
    if (done) {
        if (kageant_startup_get())
            kageant_save_startup_keys();
        kageant_refresh_tray_tip();
    }
    return done;
}

/* The load mode recorded for a tracked loaded path: 1 deferred, 0 plain,
 * -1 not tracked. */
int kageant_keypath_encrypted(const char *path)
{
    char want[MAX_PATH + 1];
    int i;
    if (!_fullpath(want, path, sizeof(want)))
        snprintf(want, sizeof(want), "%s", path);
    for (i = 0; i < g_nloaded; i++)
        if (!stricmp(g_loaded_keypaths[i], want))
            return g_loaded_encrypted[i] ? 1 : 0;
    return -1;
}

/* Of the files a key was loaded from, the first with a .hello sidecar -
 * the one a deferred-decryption prompt can open through Windows Hello.
 * Caller sfree; NULL = this key has no protected file. */
char *kageant_hello_file_of_blob(ptrlen blob)
{
    int i;
    for (i = 0; i < g_nloaded && i < g_nblobs; i++) {
        if (!g_loaded_blobs[i])
            continue;
        if (g_loaded_blobs[i]->len == blob.len &&
            !memcmp(g_loaded_blobs[i]->s, blob.ptr, blob.len) &&
            kageant_hello_has_sidecar(g_loaded_keypaths[i]))
            return dupstr(g_loaded_keypaths[i]);
    }
    return NULL;
}

/* All files of a key, one per line, each annotated with how it is
 * protected on disk: the key details' "Loaded from:" text. Caller sfree;
 * NULL when nothing is tracked. Lines are separated by "\n    " like
 * kageant_paths_of_blob's. */
char *kageant_paths_of_blob_annotated(ptrlen blob)
{
    strbuf *out = strbuf_new();
    int i, n = 0;
    for (i = 0; i < g_nloaded && i < g_nblobs; i++) {
        const char *how;
        if (!g_loaded_blobs[i])
            continue;
        if (g_loaded_blobs[i]->len != blob.len ||
            memcmp(g_loaded_blobs[i]->s, blob.ptr, blob.len))
            continue;
        if (kageant_hello_has_sidecar(g_loaded_keypaths[i])) {
            how = "Windows Hello protected";
        } else {
            Filename *fn = filename_from_str(g_loaded_keypaths[i]);
            char *cmt = NULL;
            how = ppk_encrypted_f(fn, &cmt) ? "passphrase" :
                  "UNPROTECTED - no passphrase";
            filename_free(fn);
            sfree(cmt);
        }
        if (n++)
            put_dataz(out, "\n    ");
        put_fmt(out, "%s  -  %s", g_loaded_keypaths[i], how);
    }
    if (!n) {
        strbuf_free(out);
        return NULL;
    }
    return strbuf_to_str(out);
}
