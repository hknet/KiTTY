/*
 * kitty_kageant_settings.c - kageant's settings facade: the registry/kitty.ini
 * reads and writes behind every [Agent] option (confirm mode, auto-encrypt,
 * lifetimes, the lockdown policy, notices) and the small helpers they share.
 * Split from kitty_pageant.c, which keeps the key-loading engine; what the two
 * share is declared in kitty_pageant_int.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#include "putty.h"
#include "pageant.h"

#include <shellapi.h>

#include "kitty_pageant.h"
#include "kitty_pageant_int.h"      /* what kitty_pageant.c and its split-off files share */
#include "kitty_kageant_openssh.h"  /* the Windows OpenSSH client integration */
#include "kitty_notice.h"   /* KiTTY: near-the-clock notice window (not a balloon) */
#include "kitty_inifile.h"

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

#include "kitty_oldwin.h"   /* APIs newer than the oldest Windows we load on */
#include "kitty_oldwin_reg.h"   /* XP: RegDeleteTree/RegGetValue via oldwin */
#include "kitty_text.h"     /* shared captions */
#include "kitty_inikeys.h"  /* KI_*: the kitty.ini key names */
#include "kitty_gui.h"
#include "kitty_pageant_int.h"
#include "kitty_winerr.h"   /* kitty_win_reason: a setting not stored */

/*
 * KiTTY: the notify/confirm settings live in the registry by default, but
 * honour the classic kitty.ini [Agent] keys (hknet/KiTTY#14). When the ini
 * resolved by kitty_inifile is authoritative (found, with an explicit
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

/* One value under the agent's registry key: 1 when stored, 0 with the reason
 * in GetLastError(). */
static int kageant_reg_set(const char *name, DWORD type, const void *data,
                           DWORD len)
{
    HKEY hk;
    LONG rc = RegCreateKeyExA(HKEY_CURRENT_USER, KAGEANT_REG_BASE, 0, NULL, 0,
                              KEY_SET_VALUE, NULL, &hk, NULL);
    if (rc == ERROR_SUCCESS) {
        rc = RegSetValueExA(hk, name, 0, type, (const BYTE *)data, len);
        RegCloseKey(hk);
    }
    if (rc != ERROR_SUCCESS) {
        SetLastError((DWORD)rc);
        return 0;
    }
    return 1;
}

int kageant_reg_write_dword(const char *name, int val)
{
    DWORD v = (DWORD)val;
    return kageant_reg_set(name, REG_DWORD, &v, sizeof(v));
}

/*
 * The first setting that could not be stored since the last report, with the
 * reason. The settings dialog writes some twenty values on OK; one box with
 * the first failure shows that the settings did not stick. Twenty would not
 * show more.
 */
static char kageant_set_fail_key[64];
static DWORD kageant_set_fail_err;

int kageant_set_note(int ok, const char *key)
{
    if (!ok && !kageant_set_fail_key[0]) {
        kageant_set_fail_err = GetLastError();
        snprintf(kageant_set_fail_key, sizeof(kageant_set_fail_key), "%s", key);
    }
    return ok;
}

void kageant_setting_failure_box(HWND parent)
{
    const char *ini = kageant_ini_status();
    char *msg;
    if (!kageant_set_fail_key[0])
        return;
    msg = dupprintf(KT_KA_SETTING_NOT_STORED_FMT, kageant_set_fail_key,
                    ini ? ini : "HKCU\\" KAGEANT_REG_BASE,
                    kitty_win_reason(kageant_set_fail_err));
    kageant_set_fail_key[0] = '\0';
    MessageBox(parent, msg, KT_CAP_KA_ERROR, MB_OK | MB_ICONERROR);
    sfree(msg);
}

/*
 * THE writer of an [Agent] setting: the store in charge and only that one -
 * kitty.ini when it is authoritative, the registry otherwise. Settings used
 * to be written through to both, so a portable install wrote the registry
 * and a registry install wrote any kitty.ini lying beside it; the reads
 * already fall back to the other store when the one in charge has no value,
 * so nothing set before is lost. Returns 1 when stored, 0 (recorded for
 * kageant_setting_failure_box) when not.
 */
int kageant_setting_write(const char *inikey, const char *ini_text,
                          const char *regname, int regval)
{
    if (!kitty_inifile_registry_authoritative())
        return kageant_set_note(
            kitty_inifile_write(KI_SECTION_AGENT, inikey, ini_text), inikey);
    return kageant_set_note(kageant_reg_write_dword(regname, regval), inikey);
}

/*
 * The on/off toggles. Booleans ONLY - anything with a third state belongs on
 * the pair above. The tri-state confirm mode was stored through here, so
 * "Never" (2) was written as 1 and read back as "confirm every use": the
 * setting did the exact opposite of what it said.
 */
int kageant_reg_read(const char *name, int *val_out)
{
    int v;
    if (!kageant_reg_read_dword(name, &v))
        return 0;
    *val_out = v ? 1 : 0;
    return 1;
}

int kageant_reg_write(const char *name, int on)
{
    return kageant_reg_write_dword(name, on ? 1 : 0);
}


/*
 * KiTTY: string settings with the same store precedence as the toggles
 * above - the authoritative store wins, the other is a first-run fallback,
 * and writes go to the authoritative store only (a portable install never
 * touches the registry). Used for the list column widths. The windows' place
 * and size go through kitty_auxpos.c (registry, or kitty_windowpos.ini beside
 * a portable exe); the geometry values kept here before 0.85.1.14 are read
 * once to carry them over (windows/pageant.c kageant_geometry_carry_over).
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
    int have_ini = kitty_inifile_read(KI_SECTION_AGENT, inikey, ini, sizeof(ini)) &&
        ini[0];
    if (kitty_inifile_registry_authoritative()) {
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

/* A failed kitty.ini write no longer falls back to the registry: that wrote
 * a value the ini-mode reads never look at first. */
int kageant_setting_str_set(const char *inikey, const char *regname,
                            const char *value)
{
    if (!kitty_inifile_registry_authoritative())
        return kageant_set_note(
            kitty_inifile_write(KI_SECTION_AGENT, inikey, value), inikey);
    return kageant_set_note(
        kageant_reg_set(regname, REG_SZ, value, (DWORD)(strlen(value) + 1)),
        inikey);
}

/* KiTTY: "notify on key use" tray-balloon toggle ([Agent] messageonkeyusage).
 * Default ON (absent everywhere => on). */
int kageant_notify_get(void)
{
    char buf[32];
    int ini_val = -1, reg_val;
    if (kitty_inifile_read(KI_SECTION_AGENT, KI_AGENT_MESSAGEONKEYUSAGE, buf, sizeof(buf))) {
        if (!stricmp(buf, "yes")) ini_val = 1;
        else if (!stricmp(buf, "no")) ini_val = 0;
    }
    if (kitty_inifile_registry_authoritative())
        return kageant_reg_read(KAGEANT_REG_NOTIFY, &reg_val) ? reg_val :
               (ini_val >= 0 ? ini_val : 1);
    if (ini_val >= 0)
        return ini_val;
    return kageant_reg_read(KAGEANT_REG_NOTIFY, &reg_val) ? reg_val : 1;
}

int kageant_notify_set(int on)
{
    return kageant_setting_write(KI_AGENT_MESSAGEONKEYUSAGE, on ? "yes" : "no",
                                 KAGEANT_REG_NOTIFY, on ? 1 : 0);
}

/* KiTTY: how long a notice stays up, [Agent] noticetimeout (seconds). One
 * knob for all of them; absent/0 keeps each notice's own default. Clamped to
 * a sane 2..120s. The user can still HOVER to hold any notice open longer. */
int kageant_notice_seconds(int fallback)
{
    char buf[16];
    int ini_v = -1, reg_v, v;
    if (kitty_inifile_read(KI_SECTION_AGENT, KI_AGENT_NOTICETIMEOUT, buf, sizeof(buf)))
        ini_v = atoi(buf);
    if (kitty_inifile_registry_authoritative())
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

/*
 * The confirm mode is asked for on EVERY signature request, and reading it
 * is an ini parse (GetPrivateProfileString, twice with the store-mode probe)
 * plus a registry query. The answer is remembered for two seconds: a
 * forwarding storm or an ssh-add loop asks hundreds of times a second and
 * gets one read; a change through the settings dialog invalidates at once
 * (kageant_confirm_set_mode), and an edit of the file by hand is seen within
 * two seconds.
 */
static int kageant_confirm_mode_cached = -1;
static DWORD kageant_confirm_mode_stamp = 0;

static int kageant_confirm_mode_read(void)
{
    char buf[32];
    int ini_mode = -1, reg_val;
    if (kitty_inifile_read(KI_SECTION_AGENT, KI_AGENT_ASKCONFIRMATION, buf, sizeof(buf))) {
        if (!stricmp(buf, "yes")) ini_mode = KAGEANT_CONFIRM_YES;
        else if (!stricmp(buf, "no")) ini_mode = KAGEANT_CONFIRM_NO;
        else if (!stricmp(buf, "auto")) ini_mode = KAGEANT_CONFIRM_AUTO;
    }
    /* _dword, not the boolean reader: this setting has three states and the
     * boolean one would fold "no" (2) into "yes" (1). */
    if (kitty_inifile_registry_authoritative())
        return kageant_reg_read_dword(KAGEANT_REG_CONFIRM, &reg_val) ?
               kageant_reg_to_mode(reg_val) :
               (ini_mode >= 0 ? ini_mode : KAGEANT_CONFIRM_AUTO);
    if (ini_mode >= 0)
        return ini_mode;
    return kageant_reg_read_dword(KAGEANT_REG_CONFIRM, &reg_val) ?
           kageant_reg_to_mode(reg_val) : KAGEANT_CONFIRM_AUTO;
}

int kageant_confirm_mode(void)
{
    DWORD now = GetTickCount();
    if (kageant_confirm_mode_cached >= 0 &&
        now - kageant_confirm_mode_stamp < 2000)
        return kageant_confirm_mode_cached;
    kageant_confirm_mode_cached = kageant_confirm_mode_read();
    kageant_confirm_mode_stamp = now;
    return kageant_confirm_mode_cached;
}

/* The tray checkbox is two-state: checked = YES, unchecked = AUTO (or NO). */
int kageant_confirm_get(void)
{
    return kageant_confirm_mode() == KAGEANT_CONFIRM_YES;
}

int kageant_confirm_set(int on)
{
    /* The two-state tray toggle: on = confirm every use, off = by comment. */
    return kageant_confirm_set_mode(on ? KAGEANT_CONFIRM_YES : KAGEANT_CONFIRM_AUTO);
}

/* Set the full three-state mode (the key-list radio buttons). The registry
 * holds all three states (0 = auto, 1 = yes, 2 = no; the first two unchanged
 * from the old form) - the DWORD writer, not the boolean one, which would
 * store "no" (2) as 1. */
int kageant_confirm_set_mode(int mode)
{
    const char *s = (mode == KAGEANT_CONFIRM_YES) ? "yes" :
                    (mode == KAGEANT_CONFIRM_NO)  ? "no"  : "auto";
    int ok = kageant_setting_write(KI_AGENT_ASKCONFIRMATION, s,
                                   KAGEANT_REG_CONFIRM, kageant_mode_to_reg(mode));
    kageant_confirm_mode_cached = -1;      /* the next request re-reads */
    return ok;
}

/* KiTTY: "kitty.ini mode" indicator for the key-list window and the tray
 * tooltip: the resolved ini path when it is the authoritative settings
 * store, NULL when the registry is. */
const char *kageant_ini_status(void)
{
    return kitty_inifile_registry_authoritative() ? NULL
                                                   : kitty_inifile_file();
}

/* Directory holding the resolved authoritative ini (no trailing separator);
 * 0 when there is none. */
int kageant_inidir(char *out, size_t outlen)
{
    const char *f = kitty_inifile_file();
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

int kageant_path_under(const char *dir, const char *path)
{
    size_t dl = strlen(dir);
    if (_strnicmp(path, dir, dl) != 0)
        return 0;
    return path[dl] == '\\' || path[dl] == '/';
}

/* On-disk form: relative to the ini folder when the key sits under it,
 * else the absolute path unchanged. */
void kageant_store_form(const char *abspath, char *out, size_t outlen)
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
void kageant_resolve_form(const char *stored, char *out, size_t outlen)
{
    char dir[MAX_PATH + 1];
    int isabs = stored[0] &&
                (stored[1] == ':' || stored[0] == '\\' || stored[0] == '/');
    if (!isabs && kageant_inidir(dir, sizeof(dir)))
        snprintf(out, outlen, "%s\\%s", dir, stored);
    else
        snprintf(out, outlen, "%s", stored);
}

/* The stored-entry marker for a confirm mode - the ONLY writer of these
 * tokens, so mode 2 cannot flatten to ",confirm" at one site and survive at
 * another. */
const char *kageant_confirm_token(int mode)
{
    return mode == 2 ? ",helloconfirm" : mode ? ",confirm" : "";
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
 * Stored in the store in charge, the same rule as every other agent setting
 * (kageant_setting_write): a registry-mode user with keys on removable media
 * wants these too.
 */
int kageant_bool_get(const char *inikey, const char *regname, int def)
{
    char buf[8];
    int ini_v = -1, reg_v;
    if (kitty_inifile_read(KI_SECTION_AGENT, inikey, buf, sizeof(buf))) {
        if (!stricmp(buf, "yes")) ini_v = 1;
        else if (!stricmp(buf, "no")) ini_v = 0;
    }
    if (kitty_inifile_registry_authoritative())
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
    return kageant_bool_get(KI_AGENT_QUIETMISSINGKEYS, "QuietMissingKeys", 0);
}

/* ---- the AGENT LOG: settings, path resolution and setup ----
 * User-facing name "agent log", deliberately NOT "audit log" - a local
 * file is evidence, never proof, and the name must not claim more than
 * that (the internal kitty_audit_* names describe the purpose, not a
 * promise). [Agent] agentlog (default ON), agentlogmaxkb / agentlogkeep /
 * agentlogexpiredays for the file sink's rotation, agentlogpath to
 * override the location entirely (also what makes the harness hermetic). */
int kageant_int_setting(const char *inikey, const char *regname,
                               int def, int lo, int hi)
{
    char buf[16];
    int ini_v = -1, reg_v;
    if (kitty_inifile_read(KI_SECTION_AGENT, inikey, buf, sizeof(buf))) {
        int v = atoi(buf);
        if (v >= lo && v <= hi)
            ini_v = v;
    }
    if (kitty_inifile_registry_authoritative()) {
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


/* [Agent] helloconfirm: every confirmation prompt demands a Windows Hello
 * presence check instead of a button. Default OFF; a single key can demand
 * it via its per-key confirm mode without this. Genuinely boolean. */
int kageant_hello_get(void)
{
    return kageant_bool_get(KI_AGENT_HELLOCONFIRM, "HelloConfirm", 0);
}
int kageant_hello_set(int on)
{
    return kageant_setting_write(KI_AGENT_HELLOCONFIRM, on ? "yes" : "no",
                                 "HelloConfirm", on ? 1 : 0);
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
    if (kitty_inifile_read(KI_SECTION_AGENT, KI_AGENT_RETRYKEYS, buf, sizeof(buf))) {
        if (!stricmp(buf, "yes")) ini_v = 1;
        else if (!stricmp(buf, "no")) ini_v = 0;
        else if (!stricmp(buf, "ignoredriveletter")) ini_v = KAGEANT_RETRY_ANYDRIVE;
    }
    if (kitty_inifile_registry_authoritative()) {
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
    return kageant_bool_get(KI_AGENT_UNLOADONREMOVE, "UnloadOnRemove", 0);
}

/* Seconds a typed passphrase is cached (encrypted) during a batch add.
 * Clamped to a sane range: 0 (do not cache) .. KAGEANT_TTL_MAX. */
int kageant_passphrase_ttl(void)
{
    char buf[16];
    int ini_v = -1, reg_v;
    if (kitty_inifile_read(KI_SECTION_AGENT, KI_AGENT_PASSPHRASECACHESECONDS,
                            buf, sizeof(buf))) {
        int v = atoi(buf);
        if (v >= 0)
            ini_v = kageant_clamp_ttl(v);
    }
    if (kitty_inifile_registry_authoritative())
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
    if (kitty_inifile_read(KI_SECTION_AGENT, KI_AGENT_HELLOCACHESECONDS,
                            buf, sizeof(buf))) {
        int v = atoi(buf);
        if (v >= 0)
            ini_v = kageant_clamp_ttl(v);
    }
    if (kitty_inifile_registry_authoritative())
        return kageant_reg_read_dword("HelloCacheSeconds", &reg_v) ?
               kageant_clamp_ttl(reg_v) : (ini_v >= 0 ? ini_v : KAGEANT_TTL_DEFAULT);
    if (ini_v >= 0)
        return ini_v;
    return kageant_reg_read_dword("HelloCacheSeconds", &reg_v) ?
           kageant_clamp_ttl(reg_v) : KAGEANT_TTL_DEFAULT;
}

/* Setters - to the store in charge (kageant_setting_write). */
int kageant_quiet_missing_set(int on)
{
    return kageant_setting_write(KI_AGENT_QUIETMISSINGKEYS, on ? "yes" : "no",
                                 "QuietMissingKeys", on ? 1 : 0);
}
int kageant_retry_keys_set(int mode)
{
    if (mode < 0 || mode > KAGEANT_RETRY_ANYDRIVE)
        mode = 1;
    /* Value-preserving on BOTH ends - routed through the boolean pair this
     * would write mode 2 as 1 and the third state could never exist. */
    return kageant_setting_write(KI_AGENT_RETRYKEYS,
                                 mode == KAGEANT_RETRY_ANYDRIVE ? "ignoredriveletter" :
                                 mode ? "yes" : "no",
                                 "RetryKeys", mode);
}
/* The colour theme is application-wide and lives in kitty_theme_pref.c, not
 * here: kittygen and kitty read the same setting. */
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
int kageant_settings_tab_set(int page)
{
    if (page < 0 || page > 15)
        page = 0;
    return kageant_reg_write_dword("SettingsTab", page);
}
int kageant_unload_on_remove_set(int on)
{
    return kageant_setting_write(KI_AGENT_UNLOADONREMOVE, on ? "yes" : "no",
                                 "UnloadOnRemove", on ? 1 : 0);
}
int kageant_passphrase_ttl_set(int seconds)
{
    char buf[16];
    seconds = kageant_clamp_ttl(seconds);
    snprintf(buf, sizeof(buf), "%d", seconds);
    return kageant_setting_write(KI_AGENT_PASSPHRASECACHESECONDS, buf,
                                 "PassphraseCacheSeconds", seconds);
}
int kageant_hello_ttl_set(int seconds)
{
    char buf[16];
    seconds = kageant_clamp_ttl(seconds);
    snprintf(buf, sizeof(buf), "%d", seconds);
    return kageant_setting_write(KI_AGENT_HELLOCACHESECONDS, buf,
                                 "HelloCacheSeconds", seconds);
}

/* ---- re-encrypt keys after idle (see kitty_pageant.h) ------------------ */

int kageant_autoenc_clamp(int v)
{
    if (v == KAGEANT_AUTOENC_USE || v <= 0) return v <= 0 ? 0 : v;
    if (v < KAGEANT_AUTOENC_MIN) return KAGEANT_AUTOENC_MIN;
    if (v > KAGEANT_AUTOENC_MAX) return KAGEANT_AUTOENC_MAX;
    return v;
}

int kageant_autoenc_parse(const char *text)
{
    char *end;
    long v;
    int unit = 1;
    while (text && (*text == ' ' || *text == '\t')) text++;
    if (!text || !*text) return -1;
    /* "immediately" is the word the settings show since 0.85.1.15; "use" is
     * what every version stores and reads, and what is still written, so a
     * kitty.ini shared with an older kageant keeps its meaning. A later
     * release switches the writing to "immediately". */
    if (!stricmp(text, "use") || !stricmp(text, "immediately"))
        return KAGEANT_AUTOENC_USE;
    if (!stricmp(text, "off") || !stricmp(text, "no") || !stricmp(text, "0")) return 0;
    v = strtol(text, &end, 10);
    if (end == text || v < 0) return -1;
    while (*end == ' ') end++;
    switch (tolower((unsigned char)*end)) {
      case '\0': case 's': unit = 1; break;
      case 'm': unit = 60; break;
      case 'h': unit = 3600; break;
      case 'd': unit = 86400; break;
      case 'w': unit = 7 * 86400; break;
      default: return -1;
    }
    if (v > KAGEANT_AUTOENC_MAX / unit) v = KAGEANT_AUTOENC_MAX / unit;
    return kageant_autoenc_clamp((int)(v * unit));
}

void kageant_autoenc_format(int seconds, char *buf, size_t len)
{
    /* display only - the stored word is still "use" (see the parser) */
    if (seconds == KAGEANT_AUTOENC_USE) snprintf(buf, len, "immediately");
    else if (seconds <= 0) snprintf(buf, len, "off");
    else if (seconds % 86400 == 0) snprintf(buf, len, "%d d", seconds / 86400);
    else if (seconds % 3600 == 0) snprintf(buf, len, "%d h", seconds / 3600);
    else if (seconds % 60 == 0) snprintf(buf, len, "%d m", seconds / 60);
    else snprintf(buf, len, "%d s", seconds);
}

/* Stored as a MODE WORD + a value, never a boolean pair (the confirm-mode
 * tri-state trap): [Agent] autoencryptmode = off | default | enforce,
 * autoencryptseconds = number | use; registry AutoEncryptMode /
 * AutoEncryptSeconds DWORDs. Read: registry first where it is authoritative,
 * else the ini, as every other agent setting does. */
static int kageant_autoenc_mode_parse(const char *t)
{
    if (!stricmp(t, "enforce") || !stricmp(t, "enforced")) return 2;
    if (!stricmp(t, "default") || !stricmp(t, "yes")) return 1;
    return 0;
}
/* Remembered for two seconds like the confirm mode: the heartbeat asks it
 * to decide whether it is still needed, and the setter forgets it. */
static int kageant_autoenc_mode_cached = -1;
static DWORD kageant_autoenc_mode_stamp = 0;

int kageant_autoenc_mode(void)
{
    DWORD now = GetTickCount();
    if (kageant_autoenc_mode_cached >= 0 &&
        now - kageant_autoenc_mode_stamp < 2000)
        return kageant_autoenc_mode_cached;
    kageant_autoenc_mode_cached = kageant_autoenc_mode_read();
    kageant_autoenc_mode_stamp = now;
    return kageant_autoenc_mode_cached;
}

int kageant_autoenc_mode_read(void)
{
    char buf[16];
    int ini_v = -1, reg_v;
    if (kitty_inifile_read(KI_SECTION_AGENT, KI_AGENT_AUTOENCRYPTMODE, buf, sizeof(buf)))
        ini_v = kageant_autoenc_mode_parse(buf);
    if (kitty_inifile_registry_authoritative())
        return kageant_reg_read_dword("AutoEncryptMode", &reg_v) ?
               (reg_v >= 0 && reg_v <= 2 ? reg_v : 0) : (ini_v >= 0 ? ini_v : 0);
    if (ini_v >= 0) return ini_v;
    return kageant_reg_read_dword("AutoEncryptMode", &reg_v) ?
           (reg_v >= 0 && reg_v <= 2 ? reg_v : 0) : 0;
}
int kageant_autoenc_mode_set(int mode)
{
    static const char *const words[] = { "off", "default", "enforce" };
    int ok;
    if (mode < 0 || mode > 2) mode = 0;
    ok = kageant_setting_write(KI_AGENT_AUTOENCRYPTMODE, words[mode],
                               "AutoEncryptMode", mode);
    kageant_autoenc_mode_cached = -1;
    if (kageant_tick_arm_hook)
        kageant_tick_arm_hook();           /* the heartbeat may be needed now */
    return ok;
}
int kageant_autoenc_seconds(void)
{
    char buf[32];
    int ini_v = -1, reg_v;
    if (kitty_inifile_read(KI_SECTION_AGENT, KI_AGENT_AUTOENCRYPTSECONDS, buf, sizeof(buf)))
        ini_v = kageant_autoenc_parse(buf);
    if (kitty_inifile_registry_authoritative())
        return kageant_reg_read_dword("AutoEncryptSeconds", &reg_v) ?
               kageant_autoenc_clamp(reg_v) : (ini_v >= 0 ? ini_v : 600);
    if (ini_v >= 0) return ini_v;
    return kageant_reg_read_dword("AutoEncryptSeconds", &reg_v) ?
           kageant_autoenc_clamp(reg_v) : 600;
}
int kageant_autoenc_seconds_set(int seconds)
{
    char buf[32];
    seconds = kageant_autoenc_clamp(seconds);
    if (seconds == KAGEANT_AUTOENC_USE) snprintf(buf, sizeof(buf), "use");
    else snprintf(buf, sizeof(buf), "%d", seconds);
    return kageant_setting_write(KI_AGENT_AUTOENCRYPTSECONDS, buf,
                                 "AutoEncryptSeconds", seconds);
}

/* KiTTY: notice for a key-set mutation that arrived over an external
 * transport (WM_COPYDATA or the pipe): name the key and, best effort, the
 * requesting process. Deliberately a NOTICE and not a prompt - a prompt
 * would break every scripted ssh-add. Shares the "Notify key usage"
 * setting with the signature notices. */
/* KiTTY: IPC access-control policy. All default OFF. lockdownmode blocks
 * both add and remove; blockipcadd / blockipcremove block one direction.
 * Read from the ini where authoritative, else the registry, same shape as
 * the other kageant settings. Enforced only for EXTERNAL requests. */
static int kageant_policy_read(const char *inikey, const char *regname)
{
    char buf[32];
    int ini_val = -1, reg_val;
    if (kitty_inifile_read(KI_SECTION_AGENT, inikey, buf, sizeof(buf))) {
        if (!stricmp(buf, "yes")) ini_val = 1;
        else if (!stricmp(buf, "no")) ini_val = 0;
    }
    if (kitty_inifile_registry_authoritative())
        return kageant_reg_read(regname, &reg_val) ? reg_val :
               (ini_val >= 0 ? ini_val : 0);
    if (ini_val >= 0)
        return ini_val;
    return kageant_reg_read(regname, &reg_val) ? reg_val : 0;
}

/* Remembered for two seconds per policy, like the confirm mode: these are
 * asked on every external add/remove (and lock) request. One slot per
 * policy - a fifth policy needs a fifth slot, or it is re-read every time.
 * A write through kageant_policy_set forgets them all at once. */
#define KAGEANT_POLICY_SLOTS 4
static struct { const char *inikey; int value; DWORD stamp; } kageant_policy_cache[KAGEANT_POLICY_SLOTS];

int kageant_policy_get(const char *inikey, const char *regname)
{
    DWORD now = GetTickCount();
    int i, slot = -1;
    for (i = 0; i < KAGEANT_POLICY_SLOTS; i++) {
        if (kageant_policy_cache[i].inikey &&
            !strcmp(kageant_policy_cache[i].inikey, inikey)) {
            if (now - kageant_policy_cache[i].stamp < 2000)
                return kageant_policy_cache[i].value;
            slot = i;
            break;
        }
        if (!kageant_policy_cache[i].inikey && slot < 0)
            slot = i;
    }
    {
        int v = kageant_policy_read(inikey, regname);
        if (slot >= 0) {
            kageant_policy_cache[slot].inikey = inikey;
            kageant_policy_cache[slot].value = v;
            kageant_policy_cache[slot].stamp = now;
        }
        return v;
    }
}

int kageant_lockdown_get(void)
{
    return kageant_policy_get(KI_AGENT_LOCKDOWNMODE, "LockdownMode");
}

/* A yes/no policy, to the store in charge like every agent setting. */
static int kageant_policy_set(const char *inikey, const char *regname, int on)
{
    int ok = kageant_setting_write(inikey, on ? "yes" : "no", regname, on ? 1 : 0);
    memset(kageant_policy_cache, 0, sizeof(kageant_policy_cache));
    return ok;
}
int kageant_lockdown_set(int on)
{
    return kageant_policy_set(KI_AGENT_LOCKDOWNMODE, "LockdownMode", on);
}
int  kageant_blockadd_get(void)
{
    return kageant_policy_get(KI_AGENT_BLOCKIPCADD, "BlockIpcAdd");
}
int kageant_blockadd_set(int on)
{
    return kageant_policy_set(KI_AGENT_BLOCKIPCADD, "BlockIpcAdd", on);
}
int  kageant_blockremove_get(void)
{
    return kageant_policy_get(KI_AGENT_BLOCKIPCREMOVE, "BlockIpcRemove");
}
int kageant_blockremove_set(int on)
{
    return kageant_policy_set(KI_AGENT_BLOCKIPCREMOVE, "BlockIpcRemove", on);
}
/* "Allow locking the agent over IPC (ssh-add -x / -X)" - default OFF: the
 * agent never supported the lock before, so nothing depends on it, and any
 * program could otherwise lock the agent with a passphrase nobody knows. */
int  kageant_allowipclock_get(void)
{
    return kageant_policy_get(KI_AGENT_ALLOWIPCLOCK, "AllowIpcLock");
}
int kageant_allowipclock_set(int on)
{
    return kageant_policy_set(KI_AGENT_ALLOWIPCLOCK, "AllowIpcLock", on);
}

/* "When Windows locks": 0 keep the agent working (default), 1 lock the
 * agent, 2 re-encrypt the keys. [Agent] lockwithwindows=keep|lock|reencrypt,
 * registry LockWithWindows (DWORD 0-2). Read on each session event, which is
 * rare enough to need no cache. */
int kageant_winlock_mode(void)
{
    char buf[16];
    int ini_v = -1, reg_v;
    if (kitty_inifile_read(KI_SECTION_AGENT, KI_AGENT_LOCKWITHWINDOWS, buf, sizeof(buf))) {
        if (!stricmp(buf, "keep")) ini_v = 0;
        else if (!stricmp(buf, "lock")) ini_v = 1;
        else if (!stricmp(buf, "reencrypt")) ini_v = 2;
    }
    if (kitty_inifile_registry_authoritative())
        return kageant_reg_read_dword("LockWithWindows", &reg_v) ?
               (reg_v >= 0 && reg_v <= 2 ? reg_v : 0) : (ini_v >= 0 ? ini_v : 0);
    if (ini_v >= 0) return ini_v;
    return kageant_reg_read_dword("LockWithWindows", &reg_v) ?
           (reg_v >= 0 && reg_v <= 2 ? reg_v : 0) : 0;
}
int kageant_winlock_mode_set(int mode)
{
    static const char *const words[] = { "keep", "lock", "reencrypt" };
    if (mode < 0 || mode > 2) mode = 0;
    return kageant_setting_write(KI_AGENT_LOCKWITHWINDOWS, words[mode],
                                 "LockWithWindows", mode);
}

/* The raw configured notice display time (0 = unset = per-notice default). */
int kageant_notice_timeout_get(void)
{
    char buf[16];
    int ini_v = -1, reg_v;
    if (kitty_inifile_read(KI_SECTION_AGENT, KI_AGENT_NOTICETIMEOUT, buf, sizeof(buf)))
        ini_v = atoi(buf);
    if (kitty_inifile_registry_authoritative())
        return kageant_reg_read_dword(KAGEANT_REG_NOTICESECS, &reg_v) ? reg_v :
               (ini_v > 0 ? ini_v : 0);
    if (ini_v >= 0)
        return ini_v;
    return kageant_reg_read_dword(KAGEANT_REG_NOTICESECS, &reg_v) ? reg_v : 0;
}
int kageant_notice_timeout_set(int seconds)
{
    char buf[16];
    if (seconds < 0) seconds = 0;
    if (seconds > 120) seconds = 120;
    snprintf(buf, sizeof(buf), "%d", seconds);
    return kageant_setting_write(KI_AGENT_NOTICETIMEOUT, buf,
                                 KAGEANT_REG_NOTICESECS, seconds);
}
