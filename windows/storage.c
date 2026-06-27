/*
 * storage.c: Windows-specific implementation of the interface
 * defined in storage.h.
 */

#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <assert.h>
#include "putty.h"
#include "storage.h"

#include <shlobj.h>
#ifndef CSIDL_APPDATA
#define CSIDL_APPDATA 0x001a
#endif
#ifndef CSIDL_LOCAL_APPDATA
#define CSIDL_LOCAL_APPDATA 0x001c
#endif

/*
 * KiTTY: the registry root is chosen at RUNTIME (kitty.ini KiClassName).
 * Default to KiTTY's own hive (Software\kapper.net\KiTTY) so existing KiTTY
 * sessions are picked up and PuTTY's settings aren't touched; kitty.c calls
 * kitty_set_registry_root() to flip it to PuTTY's hive when KiClassName=PuTTY.
 * For convenience we additionally READ (never write) sessions from the old KiTTY hive
 * (9bis.com\KiTTY) and PuTTY's hive, in that precedence order -- see open_settings_r()
 * and enum_settings_start().  The pointers below stay
 * fixed at their buffers; only the buffer contents change.
 */
static char reg_base_buf[256]     = "Software\\kapper.net\\KiTTY";
static char reg_sessions_buf[300] = "Software\\kapper.net\\KiTTY\\Sessions";
static char reg_jumplist_buf[300] = "Software\\kapper.net\\KiTTY\\Jumplist";
static char reg_hostca_buf[300]   = "Software\\kapper.net\\KiTTY\\SshHostCAs";
static char reg_hostkeys_buf[300] = "Software\\kapper.net\\KiTTY\\SshHostKeys";
static const char *const reg_jumplist_key = reg_jumplist_buf;
static const char *const reg_jumplist_value = "Recent sessions";
static const char *const puttystr = reg_sessions_buf;
static const char *const host_ca_key = reg_hostca_buf;
/* Read-only fallback hives (precedence: our base > old KiTTY hive > stock PuTTY).
 * Sessions present only in an older hive stay loadable; edits write to our base. */
#define OLD_KITTY_HIVE_SESSIONS "Software\\9bis.com\\KiTTY\\Sessions"
#define PUTTY_HIVE_SESSIONS     "Software\\SimonTatham\\PuTTY\\Sessions"

void kitty_set_registry_root(int use_putty)
{
    const char *base = use_putty ? "Software\\SimonTatham\\PuTTY"
                                 : "Software\\kapper.net\\KiTTY";
    strncpy(reg_base_buf, base, sizeof(reg_base_buf)-1);
    reg_base_buf[sizeof(reg_base_buf)-1] = '\0';
    sprintf(reg_sessions_buf, "%s\\Sessions",    reg_base_buf);
    sprintf(reg_jumplist_buf, "%s\\Jumplist",    reg_base_buf);
    sprintf(reg_hostca_buf,   "%s\\SshHostCAs",  reg_base_buf);
    sprintf(reg_hostkeys_buf, "%s\\SshHostKeys", reg_base_buf);
}
static int kitty_root_is_putty(void)
{ return strstr(reg_base_buf, "SimonTatham") != NULL; }

/* KiTTY: whether the saved-session list also shows (and lets you delete)
 * sessions from the read-only fallback hives (old 9bis KiTTY + stock PuTTY).
 * Default OFF, so by default KiTTY only shows/deletes its own hive and can never
 * touch a stock-PuTTY session without the user opting in. Persisted as a DWORD
 * under the base hive; toggled by a checkbox in the config dialog. */
static int kitty_show_foreign = -1;   /* -1 = not yet read */

/* Count real sessions in the primary hive (excluding "Default Settings"), so we
 * can decide the adaptive default for ShowForeignSessions. */
static int kitty_primary_session_count(void)
{
    int n = 0;
    HKEY key = open_regkey_ro(HKEY_CURRENT_USER, puttystr);
    if (key) {
        char *name;
        int idx = 0;
        while ((name = enum_regkey(key, idx)) != NULL) {
            idx++;
            strbuf *sb = strbuf_new();
            unescape_registry_key(name, sb);
            if (strcmp(sb->s, "Default Settings") != 0)
                n++;
            strbuf_free(sb);
            sfree(name);
        }
        close_regkey(key);
    }
    return n;
}

int kitty_get_show_foreign_sessions(void)
{
    if (kitty_show_foreign < 0) {
        DWORD v = 0, sz = sizeof(v);
        if (RegGetValueA(HKEY_CURRENT_USER, reg_base_buf, "ShowForeignSessions",
                         RRF_RT_REG_DWORD, NULL, &v, &sz) == ERROR_SUCCESS) {
            /* User has made an explicit choice: honour it. */
            kitty_show_foreign = v ? 1 : 0;
        } else {
            /* No explicit choice yet: adaptive default.  If the primary hive
             * has no real sessions of its own, the user almost certainly still
             * keeps everything in the old 9bis / PuTTY hive, so show those
             * (otherwise the session list would appear empty).  Once the
             * primary hive holds real sessions, default to a clean own-hive
             * view.  Not persisted, so it keeps adapting until the user
             * toggles the checkbox explicitly. */
            kitty_show_foreign =
                (!kitty_root_is_putty() && kitty_primary_session_count() == 0)
                ? 1 : 0;
        }
    }
    return kitty_show_foreign;
}
void kitty_set_show_foreign_sessions(int on)
{
    kitty_show_foreign = on ? 1 : 0;
    HKEY hk;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, reg_base_buf, 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &hk, NULL) == ERROR_SUCCESS) {
        DWORD v = (DWORD)kitty_show_foreign;
        RegSetValueExA(hk, "ShowForeignSessions", 0, REG_DWORD,
                       (const BYTE *)&v, sizeof(v));
        RegCloseKey(hk);
    }
}

/* KiTTY: remember the last session loaded in the config box, so it can be
 * re-selected and re-loaded the next time the box opens. Stored as a string
 * value "LastSession" under the base hive. */
void kitty_set_last_session(const char *sessionname)
{
    HKEY hk;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, reg_base_buf, 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &hk, NULL) == ERROR_SUCCESS) {
        const char *v = sessionname ? sessionname : "";
        RegSetValueExA(hk, "LastSession", 0, REG_SZ,
                       (const BYTE *)v, (DWORD)strlen(v) + 1);
        RegCloseKey(hk);
    }
}
int kitty_get_last_session(char *buf, int buflen)
{
    DWORD sz = (DWORD)buflen;
    if (!buf || buflen <= 0) return 0;
    buf[0] = '\0';
    if (RegGetValueA(HKEY_CURRENT_USER, reg_base_buf, "LastSession",
                     RRF_RT_REG_SZ, NULL, buf, &sz) != ERROR_SUCCESS)
        return 0;
    buf[buflen-1] = '\0';
    return buf[0] ? 1 : 0;
}

/*
 * KiTTY: expose the runtime registry base (e.g. "Software\9bis.com\KiTTY")
 * so legacy modules -- notably the tray launcher in kitty_launcher.c -- read
 * the SAME hive that session storage uses, instead of the compile-time
 * PUTTY_REG_POS macro (which is stock PuTTY's "Software\SimonTatham\PuTTY"
 * and does not hold KiTTY's sessions).  Returns the base WITHOUT any
 * "\Sessions" / "\Launcher" suffix; callers append their own.
 */
const char *kitty_registry_base(void) { return reg_base_buf; }

/*
 * KiTTY: read a session's "Comment" value, scanning the read hives in
 * precedence order (our base -> old 9bis -> stock PuTTY) and returning the
 * FIRST NON-EMPTY value found. The config dialog's read-only comment display
 * uses this instead of load_settings(): open_settings_r() is first-hive-wins
 * with no per-value merge, so a session that also exists in the new hive
 * WITHOUT a comment would otherwise mask a comment still held in the old hive.
 * Reading the value directly also sidesteps the full load path. Caller frees.
 */
char *kitty_read_session_comment(const char *sessionname)
{
    static const char *const fallback_hives[] = {
        OLD_KITTY_HIVE_SESSIONS, PUTTY_HIVE_SESSIONS };
    char *result = NULL;
    int i;

    if (!sessionname || !*sessionname)
        sessionname = "Default Settings";

    strbuf *sb = strbuf_new();
    escape_registry_key(sessionname, sb);

    /* primary (runtime) hive first */
    HKEY k = open_regkey_ro(HKEY_CURRENT_USER, puttystr, sb->s);
    if (k) {
        result = get_reg_sz(k, "Comment");
        close_regkey(k);
    }
    /* then the read-only fallback hives, unless we're in PuTTY-root mode */
    if ((!result || !*result) && !kitty_root_is_putty()) {
        for (i = 0; i < (int)lenof(fallback_hives); i++) {
            k = open_regkey_ro(HKEY_CURRENT_USER, fallback_hives[i], sb->s);
            if (!k)
                continue;
            sfree(result);
            result = get_reg_sz(k, "Comment");
            close_regkey(k);
            if (result && *result)
                break;
        }
    }
    strbuf_free(sb);
    return result;
}

/* KiTTY: which hive does a session live in? 0 = our (primary kapper.net) hive,
 * 1 = old 9bis KiTTY hive, 2 = stock PuTTY hive. Used to tag foreign sessions in
 * the saved-sessions list. */
int kitty_session_origin(const char *sessionname)
{
    if (!sessionname || !*sessionname) return 0;
    if (!strcmp(sessionname, "Default Settings")) return 0;   /* never tag the default */
    strbuf *sb = strbuf_new();
    escape_registry_key(sessionname, sb);
    int origin = 0;
    HKEY k = open_regkey_ro(HKEY_CURRENT_USER, puttystr, sb->s);
    if (k) {
        close_regkey(k);                       /* present in primary -> native */
    } else if (!kitty_root_is_putty()) {
        k = open_regkey_ro(HKEY_CURRENT_USER, OLD_KITTY_HIVE_SESSIONS, sb->s);
        if (k) { close_regkey(k); origin = 1; }
        else {
            k = open_regkey_ro(HKEY_CURRENT_USER, PUTTY_HIVE_SESSIONS, sb->s);
            if (k) { close_regkey(k); origin = 2; }
        }
    }
    strbuf_free(sb);
    return origin;
}

static bool tried_shgetfolderpath = false;
static HMODULE shell32_module = NULL;
DECL_WINDOWS_FUNCTION(static, HRESULT, SHGetFolderPathA,
                      (HWND, int, HANDLE, DWORD, LPSTR));

struct settings_w {
    HKEY sesskey;
};

settings_w *open_settings_w(const char *sessionname, char **errmsg)
{
    *errmsg = NULL;

    if (!sessionname || !*sessionname)
        sessionname = "Default Settings";

    strbuf *sb = strbuf_new();
    escape_registry_key(sessionname, sb);

    HKEY sesskey = create_regkey(HKEY_CURRENT_USER, puttystr, sb->s);
    if (!sesskey) {
        *errmsg = dupprintf("Unable to create registry key\n"
                            "HKEY_CURRENT_USER\\%s\\%s", puttystr, sb->s);
        strbuf_free(sb);
        return NULL;
    }
    strbuf_free(sb);

    settings_w *handle = snew(settings_w);
    handle->sesskey = sesskey;
    return handle;
}

/* ===================================================================== *
 * KiTTY 0.84.1.38+: encrypt credential fields at rest (Windows DPAPI).
 *
 * write_setting_s/read_setting_s is the generic registry/ini session save+load
 * chokepoint (the conf SAVE_KEYWORD path used by GUI Save/Load AND the CLI tools
 * klink/kscp/ksftp), so hooking here protects "Password"/"ProxyPassword"
 * everywhere while leaving the portable .ktx forced-file export on its own
 * legacy format. Runtime conf stays PLAINTEXT; only the stored form changes.
 *
 * Self-contained (DPAPI + base64, no bcrypt) so it links into every tool that
 * uses libsettings. The "legacy"/unmarked stored value is returned VERBATIM:
 * a registry session password was always stored plaintext (the generic save
 * wrote conf verbatim), so existing sessions behave exactly as before and only
 * new saves convert to DPAPI1:. See TASK_dpapi_passwords.md.
 * ===================================================================== */
#include <wincrypt.h>

#define KITTY_SECRET_DPAPI_MARK "DPAPI1:"

static int kitty_secret_slot(const char *key)
{
    if (!key) return -1;
    if (!strcmp(key, "Password")) return 0;
    if (!strcmp(key, "ProxyPassword")) return 1;
    return -1;
}

static char *ksec_dup(const char *s) { size_t n = strlen(s) + 1; char *d = malloc(n); if (d) memcpy(d, s, n); return d; }

static const char ksec_b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static char *ksec_b64_encode(const unsigned char *in, int len)
{
    int olen = ((len + 2) / 3) * 4, i, o = 0;
    char *out = malloc(olen + 1);
    if (!out) return NULL;
    for (i = 0; i < len; i += 3) {
        int n = len - i;
        unsigned a = in[i], b = n > 1 ? in[i+1] : 0, c = n > 2 ? in[i+2] : 0;
        out[o++] = ksec_b64[a >> 2];
        out[o++] = ksec_b64[((a & 3) << 4) | (b >> 4)];
        out[o++] = n > 1 ? ksec_b64[((b & 15) << 2) | (c >> 6)] : '=';
        out[o++] = n > 2 ? ksec_b64[c & 63] : '=';
    }
    out[o] = '\0';
    return out;
}
static int ksec_b64_val(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
static unsigned char *ksec_b64_decode(const char *in, int *outlen)
{
    int len = (int)strlen(in), pad = 0, i, o = 0, olen;
    unsigned char *out;
    if (len < 4 || (len % 4) != 0) return NULL;
    if (in[len-1] == '=') pad++;
    if (in[len-2] == '=') pad++;
    olen = (len / 4) * 3 - pad;
    out = malloc(olen > 0 ? olen : 1);
    if (!out) return NULL;
    for (i = 0; i < len; i += 4) {
        int v0 = ksec_b64_val(in[i]), v1 = ksec_b64_val(in[i+1]);
        int c2 = in[i+2], c3 = in[i+3];
        int v2 = (c2 == '=') ? 0 : ksec_b64_val(c2);
        int v3 = (c3 == '=') ? 0 : ksec_b64_val(c3);
        unsigned trip;
        if (v0 < 0 || v1 < 0 || (c2 != '=' && v2 < 0) || (c3 != '=' && v3 < 0)) { free(out); return NULL; }
        trip = ((unsigned)v0 << 18) | ((unsigned)v1 << 12) | ((unsigned)v2 << 6) | (unsigned)v3;
        if (o < olen) out[o++] = (trip >> 16) & 0xff;
        if (o < olen) out[o++] = (trip >>  8) & 0xff;
        if (o < olen) out[o++] =  trip        & 0xff;
    }
    *outlen = olen;
    return out;
}

/* at-rest scheme: "PasswordScheme" DWORD under the active hive.
 * 0 = DPAPI (default), 1 = legacy plaintext (automation escape hatch),
 * 2 = master-password (MPW1, machine-independent). */
#define KSEC_SCHEME_DPAPI  0
#define KSEC_SCHEME_LEGACY 1
#define KSEC_SCHEME_MPW    2
static int ksec_scheme(void)
{
    static int cached = -1;
    if (cached < 0) {
        DWORD v = 0, sz = sizeof(v);
        if (RegGetValueA(HKEY_CURRENT_USER, reg_base_buf, "PasswordScheme",
                         RRF_RT_REG_DWORD, NULL, &v, &sz) == ERROR_SUCCESS)
            cached = (v == 1) ? 1 : (v == 2) ? 2 : 0;
        else
            cached = 0;
    }
    return cached;
}

/* ---- master-password (MPW1) glue ----------------------------------------
 * The crypto lives in kitty/kitty_mpw.c (Argon2id + AES-CBC/HMAC), which needs
 * the crypto lib + CSPRNG and is linked only into tools that have them; it
 * self-registers these ops via a constructor. Tools without it (puttytel/pterm)
 * leave the pointers NULL, so MPW is unavailable and we fall back to DPAPI.
 * The unlock state, per-store salt and verifier (registry) live here. */
#define KSEC_MPW_KEYLEN  64    /* must match KITTY_MPW_DERIVED_LEN */
#define KSEC_MPW_SALTLEN 16    /* must match KITTY_MPW_SALT_LEN */
#define KSEC_MPW_MARK    "MPW1:"
#define KSEC_MPW_VERIFY  "KiTTY-MPW-verify"
static void (*g_mpw_derive)(const char *, const unsigned char *, int, unsigned char *) = NULL;
static char *(*g_mpw_protect)(const char *, const unsigned char *) = NULL;
static int  (*g_mpw_unprotect)(const char *, const unsigned char *, char **) = NULL;
static void (*g_mpw_randsalt)(unsigned char *, int) = NULL;
void kitty_register_mpw_crypto(
    void (*derive)(const char *, const unsigned char *, int, unsigned char *),
    char *(*protect)(const char *, const unsigned char *),
    int  (*unprotect)(const char *, const unsigned char *, char **),
    void (*randsalt)(unsigned char *, int))
{
    g_mpw_derive = derive; g_mpw_protect = protect;
    g_mpw_unprotect = unprotect; g_mpw_randsalt = randsalt;
}

static unsigned char g_mpw_key[KSEC_MPW_KEYLEN];
static int   g_mpw_unlocked = 0;
static char *g_mpw_passphrase = NULL;            /* from -masterpwfile / API */
static char *(*g_mpw_prompt)(int creating) = NULL; /* GUI/console prompt (Phase 3) */

void kitty_set_master_passphrase(const char *pass)
{
    if (g_mpw_passphrase) { memset(g_mpw_passphrase, 0, strlen(g_mpw_passphrase)); free(g_mpw_passphrase); }
    g_mpw_passphrase = (pass && pass[0]) ? ksec_dup(pass) : NULL;
    g_mpw_unlocked = 0;                           /* re-derive with the new passphrase */
}
void kitty_set_master_pw_prompt(char *(*fn)(int creating)) { g_mpw_prompt = fn; }

static int mpw_load_salt(unsigned char salt[KSEC_MPW_SALTLEN])
{
    char b[256]; DWORD sz = sizeof(b);
    if (RegGetValueA(HKEY_CURRENT_USER, reg_base_buf, "MasterPwSalt",
                     RRF_RT_REG_SZ, NULL, b, &sz) != ERROR_SUCCESS) return 0;
    int n = 0; unsigned char *d = ksec_b64_decode(b, &n);
    if (!d || n != KSEC_MPW_SALTLEN) { if (d) free(d); return 0; }
    memcpy(salt, d, KSEC_MPW_SALTLEN); free(d); return 1;
}
static void mpw_reg_set_sz(const char *name, const char *val)
{
    HKEY hk;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, reg_base_buf, 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &hk, NULL) == ERROR_SUCCESS) {
        RegSetValueExA(hk, name, 0, REG_SZ, (const BYTE *)val, (DWORD)strlen(val) + 1);
        RegCloseKey(hk);
    }
}
static char *mpw_reg_get_sz(const char *name)   /* malloc'd or NULL */
{
    char b[2048]; DWORD sz = sizeof(b);
    if (RegGetValueA(HKEY_CURRENT_USER, reg_base_buf, name, RRF_RT_REG_SZ, NULL, b, &sz)
        != ERROR_SUCCESS) return NULL;
    return ksec_dup(b);
}

/* Derive (and cache) the master key. creating=1 (a save) may generate the salt +
 * verifier; creating=0 (a load) requires an existing store. Returns 1 if unlocked. */
static int mpw_ensure_unlocked(int creating)
{
    if (g_mpw_unlocked) return 1;
    if (!g_mpw_derive || !g_mpw_protect || !g_mpw_unprotect || !g_mpw_randsalt)
        return 0;                                /* MPW crypto not linked in this tool */

    unsigned char salt[KSEC_MPW_SALTLEN];
    if (!mpw_load_salt(salt)) {
        if (!creating) return 0;
        g_mpw_randsalt(salt, KSEC_MPW_SALTLEN);
        char *sb = ksec_b64_encode(salt, KSEC_MPW_SALTLEN);
        if (sb) { mpw_reg_set_sz("MasterPwSalt", sb); free(sb); }
    }

    char *pass = NULL;
    if (g_mpw_passphrase) pass = ksec_dup(g_mpw_passphrase);
    else if (g_mpw_prompt) pass = g_mpw_prompt(creating);
    if (!pass || !pass[0]) { if (pass) { memset(pass, 0, strlen(pass)); free(pass); } return 0; }
    g_mpw_derive(pass, salt, KSEC_MPW_SALTLEN, g_mpw_key);
    memset(pass, 0, strlen(pass)); free(pass);

    char *ver = mpw_reg_get_sz("MasterPwVerifier");   /* malloc'd (ksec_dup) */
    if (ver) {
        char *vpt = NULL; int rv = g_mpw_unprotect(ver, g_mpw_key, &vpt);
        int okv = (rv == 1 && vpt && !strcmp(vpt, KSEC_MPW_VERIFY));
        if (vpt) { memset(vpt, 0, strlen(vpt)); sfree(vpt); }  /* snew'd by kitty_mpw */
        free(ver);
        if (!okv) { SecureZeroMemory(g_mpw_key, sizeof(g_mpw_key)); return 0; } /* wrong passphrase */
    } else if (creating) {
        char *vb = g_mpw_protect(KSEC_MPW_VERIFY, g_mpw_key);  /* snew'd by kitty_mpw */
        if (vb) { mpw_reg_set_sz("MasterPwVerifier", vb); sfree(vb); }
    } else {
        SecureZeroMemory(g_mpw_key, sizeof(g_mpw_key)); return 0;  /* no verifier to check against */
    }
    g_mpw_unlocked = 1;
    return 1;
}

/* plaintext -> stored form (malloc'd). Empty -> "". DPAPI by default; legacy
 * scheme (or DPAPI failure) -> plaintext verbatim (registry legacy == plaintext). */
static char *ksec_protect(const char *plaintext)
{
    if (!plaintext || !plaintext[0]) return ksec_dup("");
    int scheme = ksec_scheme();
    if (scheme == KSEC_SCHEME_MPW && mpw_ensure_unlocked(1)) {
        char *mb = g_mpw_protect(plaintext, g_mpw_key);   /* "MPW1:..." (snew'd) */
        if (mb) { char *res = ksec_dup(mb); sfree(mb); return res; }
        /* else fall through to DPAPI so the secret is never lost */
    }
    if (scheme != KSEC_SCHEME_LEGACY) {   /* DPAPI (default), or MPW-unavailable fallback */
        DATA_BLOB in, out;
        in.pbData = (BYTE *)plaintext; in.cbData = (DWORD)strlen(plaintext);
        out.pbData = NULL; out.cbData = 0;
        if (CryptProtectData(&in, L"KiTTY stored credential", NULL, NULL, NULL,
                             CRYPTPROTECT_UI_FORBIDDEN, &out)) {
            char *b64 = ksec_b64_encode(out.pbData, (int)out.cbData);
            if (out.pbData) LocalFree(out.pbData);
            if (b64) {
                size_t n = sizeof(KITTY_SECRET_DPAPI_MARK) + strlen(b64);
                char *res = malloc(n);
                if (res) { strcpy(res, KITTY_SECRET_DPAPI_MARK); strcat(res, b64); }
                free(b64);
                if (res) return res;
            }
        }
        /* fall through: never lose the secret if DPAPI is unavailable */
    }
    return ksec_dup(plaintext);
}

/* stored -> plaintext in *out (malloc'd). 1=ok, 0=absent, -1=undecryptable DPAPI blob. */
static int ksec_unprotect(const char *stored, char **out)
{
    size_t marklen = strlen(KITTY_SECRET_DPAPI_MARK);
    *out = NULL;
    if (!stored || !stored[0]) { *out = ksec_dup(""); return 0; }
    if (!strncmp(stored, KSEC_MPW_MARK, strlen(KSEC_MPW_MARK))) {
        if (mpw_ensure_unlocked(0)) {
            char *pt = NULL; int rv = g_mpw_unprotect(stored, g_mpw_key, &pt); /* snew'd */
            if (rv == 1 && pt) { char *res = ksec_dup(pt); sfree(pt); *out = res; return 1; }
            if (pt) sfree(pt);
        }
        *out = ksec_dup(""); return -1;   /* MPW locked/wrong -> undecryptable (never-wipe) */
    }
    if (!strncmp(stored, KITTY_SECRET_DPAPI_MARK, marklen)) {
        int blen = 0;
        unsigned char *blob = ksec_b64_decode(stored + marklen, &blen);
        if (blob) {
            DATA_BLOB in, dec;
            in.pbData = blob; in.cbData = (DWORD)blen;
            dec.pbData = NULL; dec.cbData = 0;
            if (CryptUnprotectData(&in, NULL, NULL, NULL, NULL,
                                   CRYPTPROTECT_UI_FORBIDDEN, &dec)) {
                char *pt = malloc(dec.cbData + 1);
                if (pt) { memcpy(pt, dec.pbData, dec.cbData); pt[dec.cbData] = '\0'; }
                if (dec.pbData) { SecureZeroMemory(dec.pbData, dec.cbData); LocalFree(dec.pbData); }
                free(blob);
                if (pt) { *out = pt; return 1; }
                *out = ksec_dup(""); return -1;
            }
            free(blob);
        }
        *out = ksec_dup(""); return -1;   /* present blob, could not decrypt */
    }
    *out = ksec_dup(stored); return 1;     /* unmarked legacy == plaintext */
}

/* never-wipe guard: keep an undecryptable-here blob so the next save re-persists it. */
static char *g_ksec_orig[2] = { NULL, NULL };
static void ksec_after_load(int slot, const char *stored, int rv)
{
    if (slot < 0 || slot > 1) return;
    if (g_ksec_orig[slot]) { free(g_ksec_orig[slot]); g_ksec_orig[slot] = NULL; }
    if (rv < 0 && stored && stored[0]) g_ksec_orig[slot] = ksec_dup(stored);
}

void write_setting_s(settings_w *handle, const char *key, const char *value)
{
    if (!handle)
        return;
    int slot = kitty_secret_slot(key);
    if (slot >= 0) {
        /* Never-wipe: a blob that failed to decrypt this session leaves the
         * in-memory value ""; re-persist the original verbatim instead of
         * clobbering it. Otherwise encrypt the plaintext at rest. */
        const char *keep = (value && value[0]) ? NULL : g_ksec_orig[slot];
        if (keep) {
            put_reg_sz(handle->sesskey, key, keep);
        } else {
            char *blob = ksec_protect(value ? value : "");
            put_reg_sz(handle->sesskey, key, blob ? blob : "");
            if (blob) { memset(blob, 0, strlen(blob)); free(blob); }
        }
        return;
    }
    put_reg_sz(handle->sesskey, key, value);
}

void write_setting_i(settings_w *handle, const char *key, int value)
{
    if (handle)
        put_reg_dword(handle->sesskey, key, value);
}

void close_settings_w(settings_w *handle)
{
    close_regkey(handle->sesskey);
    sfree(handle);
}

#define KSEC_HIVE_PRIMARY  0   /* our own kapper.net hive (or PuTTY base if KiClassName=PuTTY) */
#define KSEC_HIVE_OLDKITTY 1   /* read-only fallback: old 9bis KiTTY (legacy-encrypted passwords) */
#define KSEC_HIVE_PUTTY    2   /* read-only fallback: stock PuTTY (only our own cleartext can live here) */
struct settings_r {
    HKEY sesskey;
    int src_hive;
};

settings_r *open_settings_r(const char *sessionname)
{
    if (!sessionname || !*sessionname)
        sessionname = "Default Settings";

    strbuf *sb = strbuf_new();
    escape_registry_key(sessionname, sb);
    int src = KSEC_HIVE_PRIMARY;
    HKEY sesskey = open_regkey_ro(HKEY_CURRENT_USER, puttystr, sb->s);
    if (!sesskey && !kitty_root_is_putty()) {
        /* KiTTY: fall back to the old KiTTY hive, then stock PuTTY's, so older and
         * PuTTY sessions stay loadable (precedence: our base > old KiTTY > PuTTY). */
        sesskey = open_regkey_ro(HKEY_CURRENT_USER, OLD_KITTY_HIVE_SESSIONS, sb->s);
        if (sesskey) {
            src = KSEC_HIVE_OLDKITTY;
        } else {
            sesskey = open_regkey_ro(HKEY_CURRENT_USER, PUTTY_HIVE_SESSIONS, sb->s);
            if (sesskey) src = KSEC_HIVE_PUTTY;
        }
    }
    strbuf_free(sb);

    if (!sesskey)
        return NULL;

    settings_r *handle = snew(settings_r);
    handle->sesskey = sesskey;
    handle->src_hive = src;
    return handle;
}

/* ---- legacy (<=0.76 old-KiTTY) password decrypt, applied ONLY to the old 9bis
 * hive (see read_setting_s). Old-KiTTY stored "Password" as
 * bcrypt_base64(MASKPASS(plaintext)) keyed on host+termtype+"KiTTY". We never
 * wrote to that hive, so a value there is always this format; bcrypt is
 * unauthenticated so we must NOT apply this anywhere we might have written
 * cleartext (our own hive, or the PuTTY hive via KiClassName=PuTTY). ---- */
#include "../kitty/bcrypt/nbcrypt.h"   /* buncrypt_string_base64, bcrypt_init (relative: storage.c is built standalone in some targets) */
/* exact bytes of kitty_crypt.c's MASKKEY ("\xc2\xa4..\xc2\xbe", UTF-8, 16 bytes) */
static const unsigned char ksec_maskkey[16] = {
    0xC2,0xA4,0xC2,0xA5,0xC2,0xA9,0xC2,0xAA,
    0xC2,0xB3,0xC2,0xBC,0xC2,0xBD,0xC2,0xBE
};
static void ksec_maskpass(char *s)   /* exact replica of kitty_crypt.c MASKPASS */
{
    int i, j = 0, len = (int)strlen(s);
    char *buf = malloc(len + 1), c;
    if (!buf) return;
    buf[0] = '\0';
    for (i = 0; i < len; i++) {
        c = s[i] ^ (char)ksec_maskkey[j];
        if (c == 0) { free(buf); return; }     /* original aborts (leaves s unchanged) */
        buf[i] = c; buf[i+1] = '\0';
        j++; if (j >= 16) j = 0;
    }
    strcpy(s, buf);
    memset(buf, 0, strlen(s));
    free(buf);
}
/* returns malloc'd plaintext, or NULL if the value did not decode. */
static char *ksec_legacy_decrypt(const char *stored, HKEY sesskey)
{
    static int inited = 0;
    char passkey[1100], *host, *term, *out;
    int r;
    if (!stored || !stored[0]) return NULL;
    if (!inited) { bcrypt_init(0); inited = 1; }
    host = get_reg_sz(sesskey, "HostName");
    term = get_reg_sz(sesskey, "TerminalType");
    /* dopasskey() mode 0: host + termtype + "KiTTY" (termtype default "xterm"). */
    snprintf(passkey, sizeof(passkey), "%s%sKiTTY",
             host ? host : "", (term && term[0]) ? term : "xterm");
    sfree(host); sfree(term);
    out = malloc(strlen(stored) + 16);
    if (!out) return NULL;
    strcpy(out, stored);
    r = buncrypt_string_base64(stored, out, (unsigned)strlen(stored), passkey);
    if (r <= 0) { free(out); return NULL; }
    out[r] = '\0';            /* buncrypt returns the decoded length */
    ksec_maskpass(out);       /* undo the MASKPASS layer */
    return out;
}

/* Normalise the auto-login password to UTF-8 (the SSH password prompt is UTF-8).
 * A value that's already valid UTF-8 (ASCII included) is left untouched; a legacy
 * value in the system codepage (older KiTTY stored it via GetWindowTextA) is
 * converted CP_ACP -> UTF-8 so it matches what typing it would send. Takes
 * ownership of s (malloc'd); returns a malloc'd result. Idempotent. */
static char *ksec_to_utf8(char *s)
{
    if (!s || !s[0]) return s;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0) > 0)
        return s;                                  /* already valid UTF-8 */
    int wn = MultiByteToWideChar(CP_ACP, 0, s, -1, NULL, 0);
    if (wn <= 0) return s;
    wchar_t *w = (wchar_t *)malloc(wn * sizeof(wchar_t));
    if (!w) return s;
    MultiByteToWideChar(CP_ACP, 0, s, -1, w, wn);
    int un = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *u = (char *)malloc(un > 0 ? un : 1);
    if (u) WideCharToMultiByte(CP_UTF8, 0, w, -1, u, un, NULL, NULL);
    free(w);
    if (!u) return s;
    memset(s, 0, strlen(s)); free(s);
    return u;
}

char *read_setting_s(settings_r *handle, const char *key)
{
    if (!handle)
        return NULL;
    char *raw = get_reg_sz(handle->sesskey, key);
    int slot = kitty_secret_slot(key);
    if (slot >= 0 && raw) {
        char *pt = NULL;
        /* Old-KiTTY hive "Password" (slot 0): always legacy-encrypted there (we
         * never wrote to that hive). Decrypt it; the next Save re-stores it
         * DPAPI-encrypted in our own hive. ProxyPassword was never encrypted, and
         * the PuTTY/primary hives only hold our cleartext or DPAPI blobs. */
        if (handle->src_hive == KSEC_HIVE_OLDKITTY && slot == 0 &&
            strncmp(raw, KITTY_SECRET_DPAPI_MARK, strlen(KITTY_SECRET_DPAPI_MARK)) != 0)
            pt = ksec_legacy_decrypt(raw, handle->sesskey);   /* malloc or NULL */
        if (!pt) {
            /* DPAPI blob -> plaintext; unmarked value passes through. Record an
             * undecryptable-here blob for the never-wipe guard on next save. */
            int rv = ksec_unprotect(raw, &pt);
            ksec_after_load(slot, raw, rv);
        }
        sfree(raw);
        /* Migrate the auto-login password to UTF-8 (slot 0) so a legacy ANSI value
         * works at the UTF-8 prompt without re-entry; re-saved UTF-8 thereafter. */
        if (slot == 0) pt = ksec_to_utf8(pt);
        char *ret = dupstr(pt ? pt : "");
        if (pt) { memset(pt, 0, strlen(pt)); free(pt); }
        return ret;
    }
    return raw;
}

int read_setting_i(settings_r *handle, const char *key, int defvalue)
{
    DWORD val;
    if (!handle || !get_reg_dword(handle->sesskey, key, &val))
        return defvalue;
    else
        return val;
}

FontSpec *read_setting_fontspec(settings_r *handle, const char *name)
{
    char *settingname;
    char *fontname;
    FontSpec *ret;
    int isbold, height, charset;

    fontname = read_setting_s(handle, name);
    if (!fontname)
        return NULL;

    settingname = dupcat(name, "IsBold");
    isbold = read_setting_i(handle, settingname, -1);
    sfree(settingname);
    if (isbold == -1) {
        sfree(fontname);
        return NULL;
    }

    settingname = dupcat(name, "CharSet");
    charset = read_setting_i(handle, settingname, -1);
    sfree(settingname);
    if (charset == -1) {
        sfree(fontname);
        return NULL;
    }

    settingname = dupcat(name, "Height");
    height = read_setting_i(handle, settingname, INT_MIN);
    sfree(settingname);
    if (height == INT_MIN) {
        sfree(fontname);
        return NULL;
    }

    ret = fontspec_new(fontname, isbold, height, charset);
    sfree(fontname);
    return ret;
}

void write_setting_fontspec(settings_w *handle,
                            const char *name, FontSpec *font)
{
    char *settingname;

    write_setting_s(handle, name, font->name);
    settingname = dupcat(name, "IsBold");
    write_setting_i(handle, settingname, font->isbold);
    sfree(settingname);
    settingname = dupcat(name, "CharSet");
    write_setting_i(handle, settingname, font->charset);
    sfree(settingname);
    settingname = dupcat(name, "Height");
    write_setting_i(handle, settingname, font->height);
    sfree(settingname);
}

Filename *read_setting_filename(settings_r *handle, const char *name)
{
    char *tmp = read_setting_s(handle, name);
    if (tmp) {
        Filename *ret = filename_from_str(tmp);
        sfree(tmp);
        return ret;
    } else
        return NULL;
}

void write_setting_filename(settings_w *handle,
                            const char *name, Filename *result)
{
    /*
     * When saving a session involving a Filename, we use the 'cpath'
     * member of the Filename structure, because otherwise we break
     * backwards compatibility with existing saved sessions.
     *
     * This means that 'exotic' filenames - those including Unicode
     * characters outside the host system's CP_ACP default code page -
     * cannot be represented faithfully, and saving and reloading a
     * Conf including one will break it.
     *
     * This can't be fixed without breaking backwards compatibility,
     * and if we're going to break compatibility then we should break
     * it good and hard (the Nanny Ogg principle), and devise a
     * completely fresh storage representation that fixes as many
     * other legacy problems as possible at the same time.
     */
    write_setting_s(handle, name, result->cpath); /* FIXME */
}

void close_settings_r(settings_r *handle)
{
    if (handle) {
        close_regkey(handle->sesskey);
        sfree(handle);
    }
}

void del_settings(const char *sessionname)
{
    strbuf *sb = strbuf_new();
    escape_registry_key(sessionname, sb);

    /* Delete from the primary hive AND (KiTTY) the read-only fallback hives that
     * enum_settings_start lists from, so a session that only exists in an
     * imported/older hive can actually be removed instead of reappearing in the
     * list after a "delete". Mirrors the enumeration's hive set. */
    HKEY rkey = open_regkey_rw(HKEY_CURRENT_USER, puttystr);
    if (rkey) { del_regkey(rkey, sb->s); close_regkey(rkey); }
    /* Only reach the fallback hives when the user has opted to show them (so a
     * stock-PuTTY session is never deleted unless it's deliberately visible). */
    if (!kitty_root_is_putty() && kitty_get_show_foreign_sessions()) {
        HKEY ok = open_regkey_rw(HKEY_CURRENT_USER, OLD_KITTY_HIVE_SESSIONS);
        if (ok) { del_regkey(ok, sb->s); close_regkey(ok); }
        HKEY pk = open_regkey_rw(HKEY_CURRENT_USER, PUTTY_HIVE_SESSIONS);
        if (pk) { del_regkey(pk, sb->s); close_regkey(pk); }
    }

    strbuf_free(sb);

    remove_session_from_jumplist(sessionname);
}

struct settings_e {
    char **names;       /* merged, deduped, still-escaped key names */
    int count;
    int i;
};

settings_e *enum_settings_start(void)
{
    settings_e *e = snew(settings_e);
    e->names = NULL;
    e->count = 0;
    e->i = 0;

    /* KiTTY: enumerate the active hive first, then PuTTY's hive (deduped), so
     * KiTTY sessions and (for convenience) PuTTY sessions both show up. */
    const char *hives[3];
    int nhives = 1;
    hives[0] = puttystr;
    if (!kitty_root_is_putty() && kitty_get_show_foreign_sessions()) {
        hives[nhives++] = OLD_KITTY_HIVE_SESSIONS;
        hives[nhives++] = PUTTY_HIVE_SESSIONS;
    }

    int alloc = 0;
    for (int h = 0; h < nhives; h++) {
        HKEY key = open_regkey_ro(HKEY_CURRENT_USER, hives[h]);
        if (!key)
            continue;
        char *name;
        int idx = 0;
        while ((name = enum_regkey(key, idx)) != NULL) {
            idx++;
            bool dup = false;
            for (int j = 0; j < e->count; j++)
                if (!strcmp(e->names[j], name)) { dup = true; break; }
            if (dup) { sfree(name); continue; }
            if (e->count >= alloc) {
                alloc = alloc ? alloc * 2 : 16;
                e->names = sresize(e->names, alloc, char *);
            }
            e->names[e->count++] = name;   /* take ownership */
        }
        close_regkey(key);
    }
    return e;
}

bool enum_settings_next(settings_e *e, strbuf *sb)
{
    if (e->i >= e->count)
        return false;
    unescape_registry_key(e->names[e->i], sb);
    e->i++;
    return true;
}

void enum_settings_finish(settings_e *e)
{
    for (int j = 0; j < e->count; j++)
        sfree(e->names[j]);
    sfree(e->names);
    sfree(e);
}

static void hostkey_regname(strbuf *sb, const char *hostname,
                            int port, const char *keytype)
{
    put_fmt(sb, "%s@%d:", keytype, port);
    escape_registry_key(hostname, sb);
}

int check_stored_host_key(const char *hostname, int port,
                          const char *keytype, const char *key)
{
    /*
     * Read a saved key in from the registry and see what it says.
     */
    strbuf *regname = strbuf_new();
    hostkey_regname(regname, hostname, port, keytype);

    HKEY rkey = open_regkey_ro(HKEY_CURRENT_USER,
                               reg_hostkeys_buf);
    if (!rkey) {
        strbuf_free(regname);
        return 1;                      /* key does not exist in registry */
    }

    char *otherstr = get_reg_sz(rkey, regname->s);
    if (!otherstr && !strcmp(keytype, "rsa")) {
        /*
         * Key didn't exist. If the key type is RSA, we'll try
         * another trick, which is to look up the _old_ key format
         * under just the hostname and translate that.
         */
        char *justhost = regname->s + 1 + strcspn(regname->s, ":");
        char *oldstyle = get_reg_sz(rkey, justhost);

        if (oldstyle) {
            /*
             * The old format is two old-style bignums separated by
             * a slash. An old-style bignum is made of groups of
             * four hex digits: digits are ordered in sensible
             * (most to least significant) order within each group,
             * but groups are ordered in silly (least to most)
             * order within the bignum. The new format is two
             * ordinary C-format hex numbers (0xABCDEFG...XYZ, with
             * A nonzero except in the special case 0x0, which
             * doesn't appear anyway in RSA keys) separated by a
             * comma. All hex digits are lowercase in both formats.
             */
            strbuf *new = strbuf_new();
            const char *q = oldstyle;
            int i, j;

            for (i = 0; i < 2; i++) {
                int ndigits, nwords;
                put_datapl(new, PTRLEN_LITERAL("0x"));
                ndigits = strcspn(q, "/");      /* find / or end of string */
                nwords = ndigits / 4;
                /* now trim ndigits to remove leading zeros */
                while (q[(ndigits - 1) ^ 3] == '0' && ndigits > 1)
                    ndigits--;
                /* now move digits over to new string */
                for (j = ndigits; j-- > 0 ;)
                    put_byte(new, q[j ^ 3]);
                q += nwords * 4;
                if (*q) {
                    q++;                 /* eat the slash */
                    put_byte(new, ',');  /* add a comma */
                }
            }

            /*
             * Now _if_ this key matches, we'll enter it in the new
             * format. If not, we'll assume something odd went
             * wrong, and hyper-cautiously do nothing.
             */
            if (!strcmp(new->s, key)) {
                put_reg_sz(rkey, regname->s, new->s);
                otherstr = strbuf_to_str(new);
            } else {
                strbuf_free(new);
            }
        }

        sfree(oldstyle);
    }

    close_regkey(rkey);

    int compare = otherstr ? strcmp(otherstr, key) : -1;

    sfree(otherstr);
    strbuf_free(regname);

    if (!otherstr)
        return 1;                      /* key does not exist in registry */
    else if (compare)
        return 2;                      /* key is different in registry */
    else
        return 0;                      /* key matched OK in registry */
}

bool have_ssh_host_key(const char *hostname, int port,
                       const char *keytype)
{
    /*
     * If we have a host key, check_stored_host_key will return 0 or 2.
     * If we don't have one, it'll return 1.
     */
    return check_stored_host_key(hostname, port, keytype, "") != 1;
}

void store_host_key(Seat *seat, const char *hostname, int port,
                    const char *keytype, const char *key)
{
    strbuf *regname = strbuf_new();
    hostkey_regname(regname, hostname, port, keytype);

    HKEY rkey = create_regkey(HKEY_CURRENT_USER,
                              reg_hostkeys_buf);
    if (rkey) {
        put_reg_sz(rkey, regname->s, key);
        close_regkey(rkey);
    } /* else key does not exist in registry */

    strbuf_free(regname);
}

struct host_ca_enum {
    HKEY key;
    int i;
};

host_ca_enum *enum_host_ca_start(void)
{
    host_ca_enum *e;
    HKEY key;

    if (!(key = open_regkey_ro(HKEY_CURRENT_USER, host_ca_key)))
        return NULL;

    e = snew(host_ca_enum);
    e->key = key;
    e->i = 0;

    return e;
}

bool enum_host_ca_next(host_ca_enum *e, strbuf *sb)
{
    char *regbuf = enum_regkey(e->key, e->i);
    if (!regbuf)
        return false;

    unescape_registry_key(regbuf, sb);
    sfree(regbuf);
    e->i++;
    return true;
}

void enum_host_ca_finish(host_ca_enum *e)
{
    close_regkey(e->key);
    sfree(e);
}

host_ca *host_ca_load(const char *name)
{
    strbuf *sb;
    const char *s;

    sb = strbuf_new();
    escape_registry_key(name, sb);
    HKEY rkey = open_regkey_ro(HKEY_CURRENT_USER, host_ca_key, sb->s);
    strbuf_free(sb);

    if (!rkey)
        return NULL;

    host_ca *hca = host_ca_new();
    hca->name = dupstr(name);

    DWORD val;

    if ((s = get_reg_sz(rkey, "PublicKey")) != NULL)
        hca->ca_public_key = base64_decode_sb(ptrlen_from_asciz(s));

    if ((s = get_reg_sz(rkey, "Validity")) != NULL) {
        hca->validity_expression = strbuf_to_str(
            percent_decode_sb(ptrlen_from_asciz(s)));
    } else if ((sb = get_reg_multi_sz(rkey, "MatchHosts")) != NULL) {
        BinarySource src[1];
        BinarySource_BARE_INIT_PL(src, ptrlen_from_strbuf(sb));
        CertExprBuilder *eb = cert_expr_builder_new();

        const char *wc;
        while (wc = get_asciz(src), !get_err(src))
            cert_expr_builder_add(eb, wc);

        hca->validity_expression = cert_expr_expression(eb);
        cert_expr_builder_free(eb);
    }

    if (get_reg_dword(rkey, "PermitRSASHA1", &val))
        hca->opts.permit_rsa_sha1 = val;
    if (get_reg_dword(rkey, "PermitRSASHA256", &val))
        hca->opts.permit_rsa_sha256 = val;
    if (get_reg_dword(rkey, "PermitRSASHA512", &val))
        hca->opts.permit_rsa_sha512 = val;

    close_regkey(rkey);
    return hca;
}

char *host_ca_save(host_ca *hca)
{
    if (!*hca->name)
        return dupstr("CA record must have a name");

    strbuf *sb = strbuf_new();
    escape_registry_key(hca->name, sb);
    HKEY rkey = create_regkey(HKEY_CURRENT_USER, host_ca_key, sb->s);
    if (!rkey) {
        char *err = dupprintf("Unable to create registry key\n"
                              "HKEY_CURRENT_USER\\%s\\%s", host_ca_key, sb->s);
        strbuf_free(sb);
        return err;
    }
    strbuf_free(sb);

    strbuf *base64_pubkey = base64_encode_sb(
        ptrlen_from_strbuf(hca->ca_public_key), 0);
    put_reg_sz(rkey, "PublicKey", base64_pubkey->s);
    strbuf_free(base64_pubkey);

    strbuf *validity = percent_encode_sb(
        ptrlen_from_asciz(hca->validity_expression), NULL);
    put_reg_sz(rkey, "Validity", validity->s);
    strbuf_free(validity);

    put_reg_dword(rkey, "PermitRSASHA1", hca->opts.permit_rsa_sha1);
    put_reg_dword(rkey, "PermitRSASHA256", hca->opts.permit_rsa_sha256);
    put_reg_dword(rkey, "PermitRSASHA512", hca->opts.permit_rsa_sha512);

    close_regkey(rkey);
    return NULL;
}

char *host_ca_delete(const char *name)
{
    HKEY rkey = open_regkey_rw(HKEY_CURRENT_USER, host_ca_key);
    if (!rkey)
        return NULL;

    strbuf *sb = strbuf_new();
    escape_registry_key(name, sb);
    del_regkey(rkey, sb->s);
    strbuf_free(sb);

    return NULL;
}

/*
 * Open (or delete) the random seed file.
 */
enum { DEL, OPEN_R, OPEN_W };
static bool try_random_seed(char const *path, int action, HANDLE *ret)
{
    if (action == DEL) {
        if (!DeleteFile(path) && GetLastError() != ERROR_FILE_NOT_FOUND) {
            nonfatal("Unable to delete '%s': %s", path,
                     win_strerror(GetLastError()));
        }
        *ret = INVALID_HANDLE_VALUE;
        return false;                  /* so we'll do the next ones too */
    }

    *ret = CreateFile(path,
                      action == OPEN_W ? GENERIC_WRITE : GENERIC_READ,
                      action == OPEN_W ? 0 : (FILE_SHARE_READ |
                                              FILE_SHARE_WRITE),
                      NULL,
                      action == OPEN_W ? CREATE_ALWAYS : OPEN_EXISTING,
                      action == OPEN_W ? FILE_ATTRIBUTE_NORMAL : 0,
                      NULL);

    return (*ret != INVALID_HANDLE_VALUE);
}

static bool try_random_seed_and_free(char *path, int action, HANDLE *hout)
{
    bool retd = try_random_seed(path, action, hout);
    sfree(path);
    return retd;
}

static HANDLE access_random_seed(int action)
{
    HANDLE rethandle;

    /*
     * Iterate over a selection of possible random seed paths until
     * we find one that works.
     *
     * We do this iteration separately for reading and writing,
     * meaning that we will automatically migrate random seed files
     * if a better location becomes available (by reading from the
     * best location in which we actually find one, and then
     * writing to the best location in which we can _create_ one).
     */

    /*
     * First, try the location specified by the user in the
     * Registry, if any.
     */
    {
        HKEY rkey = open_regkey_ro(HKEY_CURRENT_USER, reg_base_buf);
        if (rkey) {
            char *regpath = get_reg_sz(rkey, "RandSeedFile");
            close_regkey(rkey);
            if (regpath) {
                bool success = try_random_seed(regpath, action, &rethandle);
                sfree(regpath);
                if (success)
                    return rethandle;
            }
        }
    }

    /*
     * Next, try the user's local Application Data directory,
     * followed by their non-local one. This is found using the
     * SHGetFolderPath function, which won't be present on all
     * versions of Windows.
     */
    if (!tried_shgetfolderpath) {
        /* This is likely only to bear fruit on systems with IE5+
         * installed, or WinMe/2K+. There is some faffing with
         * SHFOLDER.DLL we could do to try to find an equivalent
         * on older versions of Windows if we cared enough.
         * However, the invocation below requires IE5+ anyway,
         * so stuff that. */
        shell32_module = load_system32_dll("shell32.dll");
        GET_WINDOWS_FUNCTION(shell32_module, SHGetFolderPathA);
        tried_shgetfolderpath = true;
    }
    if (p_SHGetFolderPathA) {
        char profile[MAX_PATH + 1];
        if (SUCCEEDED(p_SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA,
                                         NULL, SHGFP_TYPE_CURRENT, profile)) &&
            try_random_seed_and_free(dupcat(profile, "\\PUTTY.RND"),
                                     action, &rethandle))
            return rethandle;

        if (SUCCEEDED(p_SHGetFolderPathA(NULL, CSIDL_APPDATA,
                                         NULL, SHGFP_TYPE_CURRENT, profile)) &&
            try_random_seed_and_free(dupcat(profile, "\\PUTTY.RND"),
                                     action, &rethandle))
            return rethandle;
    }

    /*
     * Failing that, try %HOMEDRIVE%%HOMEPATH% as a guess at the
     * user's home directory.
     */
    {
        char drv[MAX_PATH], path[MAX_PATH];

        DWORD drvlen = GetEnvironmentVariable("HOMEDRIVE", drv, sizeof(drv));
        DWORD pathlen = GetEnvironmentVariable("HOMEPATH", path, sizeof(path));

        /* We permit %HOMEDRIVE% to expand to an empty string, but if
         * %HOMEPATH% does that, we abort the attempt. Same if either
         * variable overflows its buffer. */
        if (drvlen == 0)
            drv[0] = '\0';

        if (drvlen < lenof(drv) && pathlen < lenof(path) && pathlen > 0 &&
            try_random_seed_and_free(
                dupcat(drv, path, "\\PUTTY.RND"), action, &rethandle))
            return rethandle;
    }

    /*
     * And finally, fall back to C:\WINDOWS.
     */
    {
        char windir[MAX_PATH];
        DWORD len = GetWindowsDirectory(windir, sizeof(windir));
        if (len < lenof(windir) &&
            try_random_seed_and_free(
                dupcat(windir, "\\PUTTY.RND"), action, &rethandle))
            return rethandle;
    }

    /*
     * If even that failed, give up.
     */
    return INVALID_HANDLE_VALUE;
}

void read_random_seed(noise_consumer_t consumer)
{
    HANDLE seedf = access_random_seed(OPEN_R);

    if (seedf != INVALID_HANDLE_VALUE) {
        while (1) {
            char buf[1024];
            DWORD len;

            if (ReadFile(seedf, buf, sizeof(buf), &len, NULL) && len)
                consumer(buf, len);
            else
                break;
        }
        CloseHandle(seedf);
    }
}

void write_random_seed(void *data, int len)
{
    HANDLE seedf = access_random_seed(OPEN_W);

    if (seedf != INVALID_HANDLE_VALUE) {
        DWORD lenwritten;

        WriteFile(seedf, data, len, &lenwritten, NULL);
        CloseHandle(seedf);
    }
}

/*
 * Internal function supporting the jump list registry code. All the
 * functions to add, remove and read the list have substantially
 * similar content, so this is a generalisation of all of them which
 * transforms the list in the registry by prepending 'add' (if
 * non-null), removing 'rem' from what's left (if non-null), and
 * returning the resulting concatenated list of strings in 'out' (if
 * non-null).
 */
static int transform_jumplist_registry(
    const char *add, const char *rem, char **out)
{
    HKEY rkey = create_regkey(HKEY_CURRENT_USER, reg_jumplist_key);
    if (!rkey)
        return JUMPLISTREG_ERROR_KEYOPENCREATE_FAILURE;

    /* Get current list of saved sessions in the registry. */
    strbuf *oldlist = get_reg_multi_sz(rkey, reg_jumplist_value);
    if (!oldlist) {
        /* Start again with the empty list. */
        oldlist = strbuf_new();
        put_data(oldlist, "\0\0", 2);
    }

    /*
     * Modify the list, if we're modifying.
     */
    bool write_failure = false;
    if (add || rem) {
        BinarySource src[1];
        BinarySource_BARE_INIT_PL(src, ptrlen_from_strbuf(oldlist));
        strbuf *newlist = strbuf_new();

        /* First add the new item to the beginning of the list. */
        if (add)
            put_asciz(newlist, add);

        /* Now add the existing list, taking care to leave out the removed
         * item, if it was already in the existing list. */
        while (true) {
            const char *olditem = get_asciz(src);
            if (get_err(src))
                break;

            if (!rem || strcmp(olditem, rem) != 0) {
                /* Check if this is a valid session, otherwise don't add. */
                settings_r *psettings_tmp = open_settings_r(olditem);
                if (psettings_tmp != NULL) {
                    close_settings_r(psettings_tmp);
                    put_asciz(newlist, olditem);
                }
            }
        }

        /* Save the new list to the registry. */
        write_failure = !put_reg_multi_sz(rkey, reg_jumplist_value, newlist);

        strbuf_free(oldlist);
        oldlist = newlist;
    }

    close_regkey(rkey);

    if (out && !write_failure)
        *out = strbuf_to_str(oldlist);
    else
        strbuf_free(oldlist);

    if (write_failure)
        return JUMPLISTREG_ERROR_VALUEWRITE_FAILURE;
    else
        return JUMPLISTREG_OK;
}

/* Adds a new entry to the jumplist entries in the registry. */
int add_to_jumplist_registry(const char *item)
{
    return transform_jumplist_registry(item, item, NULL);
}

/* Removes an item from the jumplist entries in the registry. */
int remove_from_jumplist_registry(const char *item)
{
    return transform_jumplist_registry(NULL, item, NULL);
}

/* Returns the jumplist entries from the registry. Caller must free
 * the returned pointer. */
char *get_jumplist_registry_entries (void)
{
    char *list_value;

    if (transform_jumplist_registry(NULL,NULL,&list_value) != JUMPLISTREG_OK) {
        list_value = snewn(2, char);
        *list_value = '\0';
        *(list_value + 1) = '\0';
    }
    return list_value;
}

/*
 * Recursively delete a registry key and everything under it.
 */
static void registry_recursive_remove(HKEY key)
{
    char *name;

    DWORD i = 0;
    while ((name = enum_regkey(key, i)) != NULL) {
        HKEY subkey = open_regkey_rw(key, name);
        if (subkey) {
            registry_recursive_remove(subkey);
            close_regkey(subkey);
        }
        del_regkey(key, name);
        sfree(name);
    }
}

void cleanup_all(void)
{
    /* ------------------------------------------------------------
     * Wipe out the random seed file, in all of its possible
     * locations.
     */
    access_random_seed(DEL);

    /* ------------------------------------------------------------
     * Ask Windows to delete any jump list information associated
     * with this installation of PuTTY.
     */
    clear_jumplist();

    /* ------------------------------------------------------------
     * Destroy all registry information associated with PuTTY.
     */

    /*
     * Open the main PuTTY registry key and remove everything in it.
     */
    HKEY key = open_regkey_rw(HKEY_CURRENT_USER, reg_base_buf);
    if (key) {
        registry_recursive_remove(key);
        close_regkey(key);
    }
    /*
     * Now open the parent key and remove the PuTTY main key. Once
     * we've done that, see if the parent key has any other
     * children.
     */
    if ((key = open_regkey_rw(HKEY_CURRENT_USER, PUTTY_REG_PARENT)) != NULL) {
        del_regkey(key, PUTTY_REG_PARENT_CHILD);
        char *name = enum_regkey(key, 0);
        close_regkey(key);

        /*
         * If the parent key had no other children, we must delete
         * it in its turn. That means opening the _grandparent_
         * key.
         */
        if (name) {
            sfree(name);
        } else {
            if ((key = open_regkey_rw(HKEY_CURRENT_USER,
                                      PUTTY_REG_GPARENT)) != NULL) {
                del_regkey(key, PUTTY_REG_GPARENT_CHILD);
                close_regkey(key);
            }
        }
    }
    /*
     * Now we're done.
     */
}
