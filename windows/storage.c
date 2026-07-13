/*
 * storage.c: Windows-specific implementation of the interface
 * defined in storage.h.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <limits.h>
#include <assert.h>
#include "putty.h"
#include "storage.h"
#include "../kitty/kitty_defs.h"   /* KITTY_DEFAULT_SESSION (dependency-free) */
#include "../kitty/kitty_b64.h"    /* ksec_b64_encode/decode (at-rest secret codec) */

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
static int store_is_file(void);   /* portable file-mode backend, defined below */
int kitty_portable_store_state_string(const char *key, const char *value);
int kitty_portable_load_state_string(const char *key, char *buf, int buflen);
int kitty_portable_store_state_dword(const char *key, DWORD value);
int kitty_portable_load_state_dword(const char *key, DWORD *value);

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
            if (strcmp(sb->s, KITTY_DEFAULT_SESSION) != 0)
                n++;
            strbuf_free(sb);
            sfree(name);
        }
        close_regkey(key);
    }
    return n;
}

/* True if the old 9bis-KiTTY or stock-PuTTY hive holds at least one real session
 * (excluding "Default Settings"). Lets the config box hide the "show old
 * putty/kitty sessions" option entirely when there is nothing to reveal. */
int kitty_has_foreign_sessions(void)
{
    static const char *hives[2] = { OLD_KITTY_HIVE_SESSIONS, PUTTY_HIVE_SESSIONS };
    for (int h = 0; h < 2; h++) {
        HKEY key = open_regkey_ro(HKEY_CURRENT_USER, hives[h]);
        if (!key)
            continue;
        char *name;
        int idx = 0, found = 0;
        while (!found && (name = enum_regkey(key, idx)) != NULL) {
            idx++;
            strbuf *sb = strbuf_new();
            unescape_registry_key(name, sb);
            if (strcmp(sb->s, KITTY_DEFAULT_SESSION) != 0)
                found = 1;
            strbuf_free(sb);
            sfree(name);
        }
        close_regkey(key);
        if (found)
            return 1;
    }
    return 0;
}

int kitty_get_show_foreign_sessions(void)
{
    if (kitty_show_foreign < 0) {
        DWORD v = 0, sz = sizeof(v);
        if (store_is_file() && kitty_portable_load_state_dword("ShowForeignSessions", &v)) {
            kitty_show_foreign = v ? 1 : 0;
        } else if (RegGetValueA(HKEY_CURRENT_USER, reg_base_buf, "ShowForeignSessions",
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
    if (store_is_file()) {
        kitty_portable_store_state_dword("ShowForeignSessions", (DWORD)kitty_show_foreign);
        return;
    }
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
    if (store_is_file()) {
        kitty_portable_store_state_string("LastSession", sessionname ? sessionname : "");
        return;
    }
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
    if (store_is_file())
        return kitty_portable_load_state_string("LastSession", buf, buflen);
    if (RegGetValueA(HKEY_CURRENT_USER, reg_base_buf, "LastSession",
                     RRF_RT_REG_SZ, NULL, buf, &sz) != ERROR_SUCCESS)
        return 0;
    buf[buflen-1] = '\0';
    return buf[0] ? 1 : 0;
}

void kitty_set_last_folder(const char *folder)
{
    if (!folder || !*folder) folder = "Default";
    if (store_is_file()) {
        kitty_portable_store_state_string("LastFolder", folder);
        return;
    }
    HKEY hk;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, reg_base_buf, 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &hk, NULL) == ERROR_SUCCESS) {
        RegSetValueExA(hk, "LastFolder", 0, REG_SZ,
                       (const BYTE *)folder, (DWORD)strlen(folder) + 1);
        RegCloseKey(hk);
    }
}
int kitty_get_last_folder(char *buf, int buflen)
{
    DWORD sz = (DWORD)buflen;
    if (!buf || buflen <= 0) return 0;
    buf[0] = '\0';
    if (store_is_file())
        return kitty_portable_load_state_string("LastFolder", buf, buflen);
    if (RegGetValueA(HKEY_CURRENT_USER, reg_base_buf, "LastFolder",
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
static char *kitty_read_session_value_direct(const char *sessionname,
                                             const char *valuename,
                                             int fallback_nonempty)
{
    static const char *const fallback_hives[] = {
        OLD_KITTY_HIVE_SESSIONS, PUTTY_HIVE_SESSIONS };
    char *result = NULL;
    int i;

    if (!sessionname || !*sessionname)
        sessionname = KITTY_DEFAULT_SESSION;

    /* Portable/file mode: read through the active storage backend. */
    if (store_is_file()) {
        settings_r *r = open_settings_r(sessionname);
        char *c = r ? read_setting_s(r, valuename) : NULL;
        if (r) close_settings_r(r);
        return c;
    }

    strbuf *sb = strbuf_new();
    escape_registry_key(sessionname, sb);

    /* primary (runtime) hive first */
    HKEY k = open_regkey_ro(HKEY_CURRENT_USER, puttystr, sb->s);
    if (k) {
        result = get_reg_sz(k, valuename);
        close_regkey(k);
    }
    /* then the read-only fallback hives, unless we're in PuTTY-root mode */
    if (fallback_nonempty && (!result || !*result) && !kitty_root_is_putty()) {
        for (i = 0; i < (int)lenof(fallback_hives); i++) {
            k = open_regkey_ro(HKEY_CURRENT_USER, fallback_hives[i], sb->s);
            if (!k)
                continue;
            sfree(result);
            result = get_reg_sz(k, valuename);
            close_regkey(k);
            if (result && *result)
                break;
        }
    }
    strbuf_free(sb);
    return result;
}

char *kitty_read_session_comment(const char *sessionname)
{
    /* If a session exists in the primary hive with an intentionally empty
     * Comment, keep it empty. Falling back to old hives here made the config
     * dialog show stale comments from migrated/legacy sessions with the same
     * name (e.g. an old 9bis entry overwriting an empty kapper.net comment). */
    return kitty_read_session_value_direct(sessionname, "Comment", 0);
}

char *kitty_read_session_folder(const char *sessionname)
{
    return kitty_read_session_value_direct(sessionname, "Folder", 0);
}

/* KiTTY: which hive does a session live in? 0 = our (primary kapper.net) hive,
 * 1 = old 9bis KiTTY hive, 2 = stock PuTTY hive. Used to tag foreign sessions in
 * the saved-sessions list. */
int kitty_session_origin(const char *sessionname)
{
    if (!sessionname || !*sessionname) return 0;
    if (!strcmp(sessionname, KITTY_DEFAULT_SESSION)) return 0;   /* never tag the default */
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

/* ===================================================================== *
 * KiTTY portable storage backend (fork-native flat .ini/dir format).
 *
 * When portable mode is active, each saved session is ONE file
 *   <session-dir>\<munged-sessionname>
 * holding lines  Key=munged-value  (value %HH-escaped so newlines/specials
 * round-trip). Folders are just a normal "Folder" value inside the file,
 * exactly as the registry treats them (no folder-nested subdirectories).
 *
 * Self-contained (no kitty_store.c / kitty_commun.c dependency) so libsettings
 * still links into plink/pscp/puttygen, which never call the setters below and
 * therefore stay registry-only (g_store_mode == 0). The dispatch sits BELOW the
 * credential-crypto hooks in read/write_setting_s, so portable passwords are
 * encrypted at rest exactly like registry ones.
 * ===================================================================== */
static int  g_store_mode = 0;        /* 0 = registry; nonzero = portable file mode */
static char g_sess_dir[1024] = "";   /* directory holding per-session files */

/* Lightweight diagnostic log (no plaintext: lengths + a weak checksum only).
 * Writes to %TEMP%\kitty_pwdebug.log when env KITTY_PWDEBUG is set. Shared with
 * window.c (auth-send) via the exported kitty_pwdebug(). */
static unsigned ksec_cksum(const char *s)
{
    unsigned h = 0;
    if (s) for (; *s; s++) h = h * 131 + (unsigned char)*s;
    return h & 0xffff;
}
void kitty_pwdebug(const char *fmt, ...)
{
    /* Off by default (release): set env KITTY_PWDEBUG=1 to trace the password
     * flow to kitty_pwdebug.log next to the running exe. Logs only lengths +
     * a weak checksum + storage mode, never plaintext. */
    if (!GetEnvironmentVariableA("KITTY_PWDEBUG", NULL, 0)) return;
    char path[1024], *bs;
    DWORD n = GetModuleFileNameA(NULL, path, sizeof(path) - 24);
    if (n == 0 || n >= sizeof(path) - 24) { strcpy(path, "kitty_pwdebug.log"); }
    else { bs = strrchr(path, '\\'); strcpy(bs ? bs + 1 : path, "kitty_pwdebug.log"); }
    FILE *f = fopen(path, "a");
    if (!f) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}
void kitty_set_storage_mode(int mode) { g_store_mode = mode; }
void kitty_set_session_dir(const char *dir)
{
    if (dir && dir[0]) {
        strncpy(g_sess_dir, dir, sizeof(g_sess_dir) - 1);
        g_sess_dir[sizeof(g_sess_dir) - 1] = '\0';
    } else {
        g_sess_dir[0] = '\0';
    }
}
static int store_is_file(void) { return g_store_mode != 0 && g_sess_dir[0] != '\0'; }
/* Public query for UI surfaces (e.g. the config-box "(portable)" title tag). */
int kitty_storage_is_portable(void) { return store_is_file(); }

static const char ksf_hex[] = "0123456789ABCDEF";
static int ksf_special(unsigned char c)
{
    return c < 0x20 || c == 0x7f || c == '%' || c == '\\' || c == '/' ||
           c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
           c == '>' || c == '|';
}
static char *ksf_munge(const char *in)            /* snewn'd */
{
    char *out = snewn(strlen(in) * 3 + 1, char), *o = out;
    for (; *in; in++) {
        unsigned char c = (unsigned char)*in;
        if (ksf_special(c)) { *o++ = '%'; *o++ = ksf_hex[c >> 4]; *o++ = ksf_hex[c & 15]; }
        else *o++ = (char)c;
    }
    *o = '\0';
    return out;
}
static int ksf_hexv(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}
static char *ksf_unmunge(const char *in)          /* snewn'd */
{
    char *out = snewn(strlen(in) + 1, char), *o = out;
    while (*in) {
        if (*in == '%' && in[1] && in[2]) {
            int hi = ksf_hexv(in[1]), lo = ksf_hexv(in[2]);
            if (hi >= 0 && lo >= 0) { *o++ = (char)((hi << 4) | lo); in += 3; continue; }
        }
        *o++ = *in++;
    }
    *o = '\0';
    return out;
}

struct ksf_item { char *key; char *val; struct ksf_item *next; };
static char *ksf_list_get(struct ksf_item *h, const char *key)   /* borrowed or NULL */
{
    for (; h; h = h->next) if (!strcmp(h->key, key)) return h->val;
    return NULL;
}
static void ksf_list_set(struct ksf_item **h, const char *key, const char *val)
{
    struct ksf_item *it;
    for (it = *h; it; it = it->next)
        if (!strcmp(it->key, key)) {
            char *nv = dupstr(val ? val : "");
            if (it->val) { memset(it->val, 0, strlen(it->val)); sfree(it->val); }
            it->val = nv;
            return;
        }
    it = snew(struct ksf_item);
    it->key = dupstr(key);
    it->val = dupstr(val ? val : "");
    it->next = *h;
    *h = it;
}
static void ksf_list_free(struct ksf_item *h)
{
    while (h) {
        struct ksf_item *n = h->next;
        sfree(h->key);
        if (h->val) { memset(h->val, 0, strlen(h->val)); sfree(h->val); }
        sfree(h);
        h = n;
    }
}
static char *ksf_session_path(const char *sessionname)   /* snewn'd or NULL */
{
    char *m = ksf_munge(sessionname);
    char *p = dupprintf("%s\\%s", g_sess_dir, m);
    sfree(m);
    return p;
}
/* A cyd01-syntax file was written by old (<=0.76) KiTTY, whose portable saves
 * stored "Password" bcrypt-encrypted, same scheme as its old registry hive
 * (old KiTTY never encrypted ProxyPassword). Decode it as part of the format
 * conversion, so everything downstream — reads, the migration-consent gate,
 * the next save — sees the plaintext-unmarked semantics of our own format. If
 * it does not decode, the stored bytes stay untouched (never-lose); crucially,
 * decoding must happen HERE because once the file is rewritten in our syntax
 * the "this value is legacy-encrypted" context is gone for good. */
static char *ksec_legacy_decrypt_hostterm(const char *stored, const char *host,
                                          const char *term);
static void ksf_convert_legacy_password(struct ksf_item **head)
{
    const char *pw = ksf_list_get(*head, "Password");
    if (!pw || !pw[0]) return;
    char *pt = ksec_legacy_decrypt_hostterm(pw,
        ksf_list_get(*head, "HostName"), ksf_list_get(*head, "TerminalType"));
    if (pt) {
        ksf_list_set(head, "Password", pt);
        memset(pt, 0, strlen(pt));
        free(pt);
    }
}

static struct ksf_item *ksf_load(const char *path)       /* parsed list (may be NULL) */
{
    FILE *fp = fopen(path, "rb");
    struct ksf_item *head = NULL;
    char line[8192];
    int cyd01 = 0;
    if (!fp) return NULL;
    while (fgets(line, sizeof(line), fp)) {
        size_t l = strlen(line);
        char *eq, *bs, *val;
        while (l && (line[l-1] == '\n' || line[l-1] == '\r')) line[--l] = '\0';
        /* Two on-disk line formats are accepted:
         *   key=munged-value     - our fork-native format (written by ksf_save)
         *   key\munged-value\    - legacy cyd01-KiTTY portable format
         * Discriminate by whichever delimiter follows the key first: a setting
         * key never contains '=' or '\\', and in our format values escape '\\'
         * (so a real '\\' only appears as the cyd01 delimiter), while values may
         * legitimately contain '='. Both formats use the same %HH value munging,
         * so reading cyd01 files lets existing portable sessions load; the next
         * Save rewrites them in our format. */
        eq = strchr(line, '=');
        bs = strchr(line, '\\');
        if (eq && (!bs || eq < bs)) {
            *eq = '\0';
            val = ksf_unmunge(eq + 1);
        } else if (bs) {
            *bs = '\0';
            char *raw = bs + 1;
            size_t rl = strlen(raw);
            if (rl && raw[rl - 1] == '\\') raw[rl - 1] = '\0';  /* drop trailing delim */
            val = ksf_unmunge(raw);
            cyd01 = 1;
        } else {
            continue;   /* no delimiter -> not a setting line */
        }
        ksf_list_set(&head, line, val ? val : "");
        if (val) sfree(val);
    }
    fclose(fp);
    if (cyd01)
        ksf_convert_legacy_password(&head);
    return head;
}
static void ksf_save(const char *path, struct ksf_item *h)
{
    FILE *fp;
    CreateDirectoryA(g_sess_dir, NULL);   /* harmless if it already exists */
    fp = fopen(path, "wb");
    if (!fp) return;
    for (; h; h = h->next) {
        char *mv = ksf_munge(h->val ? h->val : "");
        fprintf(fp, "%s=%s\n", h->key, mv ? mv : "");
        if (mv) sfree(mv);
    }
    fclose(fp);
}

static char *portable_root_dir(void)           /* snewn'd or NULL */
{
    char *root, *bs;
    if (!store_is_file()) return NULL;
    root = dupstr(g_sess_dir);
    bs = strrchr(root, '\\');
    if (bs && !_stricmp(bs + 1, "Sessions"))
        *bs = '\0';
    return root;
}

static char *portable_subdir_path(const char *subdir)     /* snewn'd */
{
    char *root = portable_root_dir();
    char *path = root ? dupprintf("%s\\%s", root, subdir) : NULL;
    sfree(root);
    return path;
}

static char *portable_item_path(const char *subdir, const char *name)
{
    char *dir = portable_subdir_path(subdir);
    char *m = ksf_munge(name ? name : "");
    char *path = (dir && m) ? dupprintf("%s\\%s", dir, m) : NULL;
    sfree(dir);
    sfree(m);
    return path;
}

static int portable_write_text_file(const char *subdir, const char *name,
                                    const char *value)
{
    char *dir = portable_subdir_path(subdir);
    char *path = portable_item_path(subdir, name);
    FILE *fp;
    int ok = 0;
    if (!dir || !path) goto out;
    CreateDirectoryA(dir, NULL);
    fp = fopen(path, "wb");
    if (!fp) goto out;
    fputs(value ? value : "", fp);
    fputc('\n', fp);
    ok = (fclose(fp) == 0);
    fp = NULL;
out:
    sfree(dir);
    sfree(path);
    return ok;
}

static char *portable_read_text_file(const char *subdir, const char *name)
{
    char *path = portable_item_path(subdir, name);
    FILE *fp;
    long len;
    char *buf = NULL;
    if (!path) return NULL;
    fp = fopen(path, "rb");
    sfree(path);
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) || (len = ftell(fp)) < 0 || fseek(fp, 0, SEEK_SET)) {
        fclose(fp); return NULL;
    }
    buf = snewn(len + 1, char);
    if (fread(buf, 1, len, fp) != (size_t)len) { sfree(buf); buf = NULL; }
    else {
        while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r')) len--;
        buf[len] = '\0';
    }
    fclose(fp);
    return buf;
}

static char *portable_state_path(void)          /* snewn'd or NULL */
{
    char *root = portable_root_dir();
    char *path = root ? dupprintf("%s\\KiTTYState", root) : NULL;
    sfree(root);
    return path;
}

int kitty_portable_store_state_string(const char *key, const char *value)
{
    char *path;
    struct ksf_item *items;
    if (!store_is_file()) return 0;
    path = portable_state_path();
    if (!path) return 0;
    items = ksf_load(path);
    ksf_list_set(&items, key, value ? value : "");
    ksf_save(path, items);
    ksf_list_free(items);
    sfree(path);
    return 1;
}

int kitty_portable_load_state_string(const char *key, char *buf, int buflen)
{
    char *path, *v;
    struct ksf_item *items;
    int ok = 0;
    if (!store_is_file() || !buf || buflen <= 0) return 0;
    buf[0] = '\0';
    path = portable_state_path();
    if (!path) return 0;
    items = ksf_load(path);
    v = ksf_list_get(items, key);
    if (v) {
        strncpy(buf, v, buflen - 1);
        buf[buflen - 1] = '\0';
        ok = buf[0] ? 1 : 0;
    }
    ksf_list_free(items);
    sfree(path);
    return ok;
}

int kitty_portable_store_state_dword(const char *key, DWORD value)
{
    char tmp[32];
    sprintf(tmp, "%lu", (unsigned long)value);
    return kitty_portable_store_state_string(key, tmp);
}

int kitty_portable_load_state_dword(const char *key, DWORD *value)
{
    char tmp[32];
    char *end;
    unsigned long v;
    if (!value || !kitty_portable_load_state_string(key, tmp, sizeof(tmp))) return 0;
    v = strtoul(tmp, &end, 10);
    if (end == tmp) return 0;
    *value = (DWORD)v;
    return 1;
}

struct settings_w {
    HKEY sesskey;
    int is_file;
    char *fpath;
    struct ksf_item *items;
    int mig_answer;   /* legacy->protected consent for THIS save: -1 unasked */
};

settings_w *open_settings_w(const char *sessionname, char **errmsg)
{
    *errmsg = NULL;

    if (!sessionname || !*sessionname)
        sessionname = KITTY_DEFAULT_SESSION;

    if (store_is_file()) {
        settings_w *handle = snew(settings_w);
        handle->sesskey = NULL;
        handle->is_file = 1;
        handle->items = NULL;
        handle->mig_answer = -1;
        handle->fpath = ksf_session_path(sessionname);
        if (!handle->fpath) {
            sfree(handle);
            *errmsg = dupstr("Unable to build portable session path");
            return NULL;
        }
        /* Pre-load any existing file so a save overwrites/adds in place and never
         * drops keys it didn't rewrite (matches the registry open-then-overwrite
         * semantics; the full conf is normally rewritten on each save anyway). */
        handle->items = ksf_load(handle->fpath);
        return handle;
    }

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
    handle->is_file = 0;
    handle->fpath = NULL;
    handle->items = NULL;
    handle->mig_answer = -1;
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
 * Write protection is BACKEND-scoped (TASK_dpapi_mpw_backend_policy.md):
 * registry hive -> DPAPI1 always; portable session files -> MPW1 (master
 * password) or the explicit-compat escape hatch. Reads dispatch on the stored
 * marker regardless of backend.
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

/* Base64 codec (ksec_b64_encode / ksec_b64_val / ksec_b64_decode) moved to
 * kitty/kitty_b64.c to shrink this file's divergence from upstream. */

/* Portable-context write policy (kitty.ini [KiTTY] PortablePasswordProtection):
 * "master" (default) -> portable secrets are written MPW1; "legacy" -> unmarked
 * plaintext, the explicit compatibility escape hatch for automation/audit
 * setups. Set from kitty.c when the portable (savemode=dir) backend is
 * activated; registry-backed stores never consult it. This replaces the
 * retired registry-global "PasswordScheme" DWORD, which is no longer read at
 * all — a leftover value of any kind is ignored, so it can no longer make the
 * hive plaintext or master-password (TASK_dpapi_mpw_backend_policy.md). */
static int g_portable_pw_legacy = 0;
void kitty_set_portable_password_protection(const char *mode)
{
    g_portable_pw_legacy = (mode && !_stricmp(mode, "legacy"));
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
/* MPW2 = the same AES-256-CBC+HMAC envelope with the Argon2id salt embedded:
 * "MPW2:<b64 salt>.<MPW1 payload>". Self-contained, so a value can be
 * unlocked on another machine from the master password alone — required for
 * exported .ktx files, which carry no Security\ salt store. MPW1 (store-salt
 * only) stays read-compatible; all new writes are MPW2. */
#define KSEC_MPW2_MARK   "MPW2:"
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
static int   g_mpw_declined = 0;                 /* user cancelled -> stop prompting */
static char *g_mpw_passphrase = NULL;            /* from -masterpwfile / API */
static char *(*g_mpw_prompt)(int creating) = NULL; /* GUI/console prompt */
/* Defer the interactive master-password prompt: when set, a locked MPW value
 * reads back empty (blob preserved by never-wipe) instead of popping the unlock
 * dialog. Used around the startup auto-load of the last session so the config
 * box opens WITHOUT a premature prompt; an explicit Load / "show password" /
 * connect clears it and prompts at the real point of use. A supplied
 * passphrase (-masterpwfile) or an already-unlocked store still works. */
static int   g_mpw_defer = 0;
void kitty_set_defer_mpw_prompt(int on) { g_mpw_defer = on; }
static unsigned char g_mpw_salt[KSEC_MPW_SALTLEN]; /* store salt, cached at unlock */
static int   g_mpw_salt_valid = 0;
/* one-slot cache for a FOREIGN salt (imported MPW2 from another store), so a
 * bulk import derives/prompts once, not per session */
static unsigned char g_mpw_fkey[KSEC_MPW_KEYLEN];
static unsigned char g_mpw_fsalt[KSEC_MPW_SALTLEN];
static int   g_mpw_f_valid = 0;

void kitty_set_master_passphrase(const char *pass)
{
    if (g_mpw_passphrase) { memset(g_mpw_passphrase, 0, strlen(g_mpw_passphrase)); free(g_mpw_passphrase); }
    g_mpw_passphrase = (pass && pass[0]) ? ksec_dup(pass) : NULL;
    g_mpw_unlocked = 0;                           /* re-derive with the new passphrase */
    g_mpw_declined = 0;                           /* fresh passphrase -> allow another try */
}
void kitty_set_master_pw_prompt(char *(*fn)(int creating)) { g_mpw_prompt = fn; }

/* Backend-scoped MPW store state (salt + verifier). The registry hive is only
 * right for registry-backed stores; in portable mode these MUST live next to
 * the session files (Security\ subdir at the portable root) so that "copy the
 * files + know the master password" actually unlocks on another machine, and
 * portable mode stays registry-free. Portable reads fall back to the registry
 * read-only, so a dev-era store (salt minted in the hive) keeps unlocking on
 * the same machine; the next state write lands in the portable store. */
#define KSEC_MPW_SUBDIR "Security"
static char *mpw_state_get(const char *name)    /* malloc'd (ksec_dup) or NULL */
{
    if (store_is_file()) {
        char *v = portable_read_text_file(KSEC_MPW_SUBDIR, name);   /* snewn'd */
        if (v) { char *res = ksec_dup(v); sfree(v); return res; }
        /* fall through: dev-era same-machine migration */
    }
    char b[2048]; DWORD sz = sizeof(b);
    if (RegGetValueA(HKEY_CURRENT_USER, reg_base_buf, name, RRF_RT_REG_SZ, NULL, b, &sz)
        != ERROR_SUCCESS) return NULL;
    return ksec_dup(b);
}
static void mpw_state_set(const char *name, const char *val)
{
    if (store_is_file()) {
        portable_write_text_file(KSEC_MPW_SUBDIR, name, val);
        return;
    }
    HKEY hk;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, reg_base_buf, 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &hk, NULL) == ERROR_SUCCESS) {
        RegSetValueExA(hk, name, 0, REG_SZ, (const BYTE *)val, (DWORD)strlen(val) + 1);
        RegCloseKey(hk);
    }
}
static int mpw_load_salt(unsigned char salt[KSEC_MPW_SALTLEN])
{
    char *b = mpw_state_get("MasterPwSalt");
    if (!b) return 0;
    int n = 0; unsigned char *d = ksec_b64_decode(b, &n);
    free(b);
    if (!d || n != KSEC_MPW_SALTLEN) { if (d) free(d); return 0; }
    memcpy(salt, d, KSEC_MPW_SALTLEN); free(d); return 1;
}

/* Derive (and cache) the master key. creating=1 (a save) may generate the salt +
 * verifier; creating=0 (a load) requires an existing store. Returns 1 if unlocked. */
static int mpw_ensure_unlocked(int creating)
{
    if (g_mpw_unlocked) return 1;
    if (g_mpw_declined)  return 0;               /* user cancelled earlier this run */
    /* Deferred (startup auto-load): don't prompt now unless a passphrase was
     * supplied non-interactively. Leaves the value locked; the next explicit
     * load/show/connect runs with defer cleared and prompts then. */
    if (g_mpw_defer && !g_mpw_passphrase) return 0;
    if (!g_mpw_derive || !g_mpw_protect || !g_mpw_unprotect || !g_mpw_randsalt)
        return 0;                                /* MPW crypto not linked in this tool */

    unsigned char salt[KSEC_MPW_SALTLEN];
    int have_salt = mpw_load_salt(salt);
    char *ver = mpw_state_get("MasterPwVerifier");    /* malloc'd (ksec_dup) or NULL */
    int first_time = (!have_salt || !ver);

    /* A load (creating==0) needs an existing store; nothing to unlock otherwise. */
    if (!creating && first_time) { if (ver) free(ver); return 0; }

    /* Ask at most ONCE per process: if a preceding decrypt already derived the
     * key for THIS store's salt (foreign-salt cache), and it validates against
     * the stored verifier, promote it to the unlocked key instead of prompting
     * again. This is what makes open-proxy -> save -> connect (all MPW2 in the
     * same store) prompt only on the first step. Salt-matched + verifier-checked,
     * so a genuinely foreign (imported, different-password) key can't slip in. */
    if (ver && have_salt && g_mpw_f_valid &&
        !memcmp(g_mpw_fsalt, salt, KSEC_MPW_SALTLEN)) {
        char *vpt = NULL; int rv = g_mpw_unprotect(ver, g_mpw_fkey, &vpt);
        int good = (rv == 1 && vpt && !strcmp(vpt, KSEC_MPW_VERIFY));
        if (vpt) { memset(vpt, 0, strlen(vpt)); sfree(vpt); }
        if (good) {
            memcpy(g_mpw_key, g_mpw_fkey, sizeof(g_mpw_key));
            memcpy(g_mpw_salt, salt, KSEC_MPW_SALTLEN);
            g_mpw_salt_valid = 1; g_mpw_unlocked = 1;
            free(ver); return 1;
        }
    }

    if (!have_salt)                              /* first-time setup: mint a salt */
        g_mpw_randsalt(salt, KSEC_MPW_SALTLEN);
    /* NOTE: the minted salt is NOT persisted here. It is written together
     * with the verifier below, only once the user has actually set a master
     * password — cancelling the setup prompt must leave no state behind
     * (no Security\ dir in a portable tree the user said no to). */

    /* A non-interactive passphrase (-masterpwfile/API) gets a single try; an
     * interactive prompt gets a few, re-prompting on a wrong master password. */
    int from_supplied = (g_mpw_passphrase != NULL);
    int tries = from_supplied ? 1 : 3;
    int ok = 0;

    while (tries-- > 0 && !ok) {
        char *pass = from_supplied ? ksec_dup(g_mpw_passphrase)
                   : (g_mpw_prompt ? g_mpw_prompt(first_time) : NULL);
        if (!pass || !pass[0]) {                 /* cancelled / no prompt available */
            if (pass) { memset(pass, 0, strlen(pass)); free(pass); }
            if (!from_supplied && g_mpw_prompt) g_mpw_declined = 1;
            break;
        }
        g_mpw_derive(pass, salt, KSEC_MPW_SALTLEN, g_mpw_key);
        memset(pass, 0, strlen(pass)); free(pass);

        if (ver) {                               /* verify against the stored token */
            char *vpt = NULL; int rv = g_mpw_unprotect(ver, g_mpw_key, &vpt);
            ok = (rv == 1 && vpt && !strcmp(vpt, KSEC_MPW_VERIFY));
            if (vpt) { memset(vpt, 0, strlen(vpt)); sfree(vpt); } /* snew'd by kitty_mpw */
            if (!ok) SecureZeroMemory(g_mpw_key, sizeof(g_mpw_key)); /* wrong: re-prompt */
        } else {                                 /* first time: persist salt + verifier */
            if (!have_salt) {
                char *sb = ksec_b64_encode(salt, KSEC_MPW_SALTLEN);
                if (sb) { mpw_state_set("MasterPwSalt", sb); free(sb); }
                have_salt = 1;
            }
            char *vb = g_mpw_protect(KSEC_MPW_VERIFY, g_mpw_key); /* snew'd by kitty_mpw */
            if (vb) { mpw_state_set("MasterPwVerifier", vb); sfree(vb); }
            ok = 1;
        }
    }

    if (ver) free(ver);
    if (ok) {
        memcpy(g_mpw_salt, salt, KSEC_MPW_SALTLEN);
        g_mpw_salt_valid = 1;
        g_mpw_unlocked = 1;
        return 1;
    }
    SecureZeroMemory(g_mpw_key, sizeof(g_mpw_key));
    return 0;
}

/* Unprotect an "MPW1:..." payload whose Argon2id salt is KNOWN (embedded in an
 * MPW2 value). The derived key is salt-specific, so this works for values from
 * ANY store — the unlocked store key and one foreign key are cached; otherwise
 * the passphrase (from -masterpwfile/API, else a bounded prompt) is derived
 * against the given salt and validated by the envelope's own HMAC.
 * Returns 1 + snew'd plaintext in *outp, else 0. */
static int mpw_unprotect_with_salt(const char *m1blob,
                                   const unsigned char salt[KSEC_MPW_SALTLEN],
                                   char **outp)
{
    *outp = NULL;
    if (!g_mpw_derive || !g_mpw_unprotect) return 0;
    if (g_mpw_unlocked && g_mpw_salt_valid &&
        !memcmp(salt, g_mpw_salt, KSEC_MPW_SALTLEN)) {
        if (g_mpw_unprotect(m1blob, g_mpw_key, outp) == 1 && *outp) return 1;
        if (*outp) { sfree(*outp); *outp = NULL; }
    }
    if (g_mpw_f_valid && !memcmp(salt, g_mpw_fsalt, KSEC_MPW_SALTLEN)) {
        if (g_mpw_unprotect(m1blob, g_mpw_fkey, outp) == 1 && *outp) return 1;
        if (*outp) { sfree(*outp); *outp = NULL; }
    }
    int from_supplied = (g_mpw_passphrase != NULL);
    int tries = from_supplied ? 1 : 3;
    /* Deferred startup load or no interactive prompt available -> stay locked
     * (the cached-key fast paths above already ran). */
    if (!from_supplied && (g_mpw_declined || !g_mpw_prompt || g_mpw_defer)) return 0;
    while (tries-- > 0) {
        char *pass = from_supplied ? ksec_dup(g_mpw_passphrase)
                                   : g_mpw_prompt(0);
        if (!pass || !pass[0]) {
            if (pass) { memset(pass, 0, strlen(pass)); free(pass); }
            if (!from_supplied) g_mpw_declined = 1;
            return 0;
        }
        unsigned char key[KSEC_MPW_KEYLEN];
        g_mpw_derive(pass, salt, KSEC_MPW_SALTLEN, key);
        memset(pass, 0, strlen(pass)); free(pass);
        if (g_mpw_unprotect(m1blob, key, outp) == 1 && *outp) {
            memcpy(g_mpw_fkey, key, KSEC_MPW_KEYLEN);
            memcpy(g_mpw_fsalt, salt, KSEC_MPW_SALTLEN);
            g_mpw_f_valid = 1;
            SecureZeroMemory(key, sizeof(key));
            return 1;
        }
        if (*outp) { sfree(*outp); *outp = NULL; }
        SecureZeroMemory(key, sizeof(key));
    }
    return 0;
}

/* DPAPI1 wrap: malloc'd "DPAPI1:<b64>" or NULL on failure. */
static char *ksec_dpapi_protect(const char *plaintext)
{
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
    return NULL;
}

/* ---- Cross-process master-password sharing (launcher-mpw-sharing) ----------
 * Hand the unlocked master key to a spawned child KiTTY through an INHERITABLE
 * file mapping - child-only, mirroring the existing Duplicate-Session conf
 * hand-off - with the key wrapped by CryptProtectMemory(SAME_LOGON) so the
 * shared section never holds it in the clear and only this logon can unwrap it.
 * Fail-safe throughout: any failure just leaves the child to prompt as before. */
#ifndef CRYPTPROTECTMEMORY_BLOCK_SIZE
#define CRYPTPROTECTMEMORY_BLOCK_SIZE 16
#endif
#ifndef CRYPTPROTECTMEMORY_SAME_LOGON
#define CRYPTPROTECTMEMORY_SAME_LOGON 0x02
#endif
#define KMPW_INHERIT_MAGIC 0x57504D4Bu   /* 'KMPW' */

struct kmpw_inherit_blob {
    unsigned magic ;
    unsigned version ;
    unsigned wrapped ;                       /* 1 = key is CryptProtectMemory'd */
    unsigned char salt[KSEC_MPW_SALTLEN] ;
    unsigned char key[KSEC_MPW_KEYLEN] ;     /* KEYLEN is a multiple of the 16-byte block */
} ;

static int kmpw_mem(const char *fn, void *p, DWORD n) {
    HMODULE c = GetModuleHandleA("crypt32.dll") ; if(!c) c = LoadLibraryA("crypt32.dll") ;
    if(!c) return 0 ;
    BOOL (WINAPI *pf)(LPVOID,DWORD,DWORD) = (BOOL(WINAPI*)(LPVOID,DWORD,DWORD))GetProcAddress(c,fn) ;
    return pf ? (pf(p,n,CRYPTPROTECTMEMORY_SAME_LOGON) ? 1 : 0) : 0 ;
}

/* Core: map the inherited mapping `fm` (sz bytes), validate + unlock, close it.
 * Verifies the key against this store's verifier when the store is readable;
 * otherwise trusts the inherited-handle authenticity (only our parent could have
 * created it, and CryptUnprotectMemory only unwraps in this logon). */
static void kmpw_consume(HANDLE fm, unsigned sz) {
    if (!fm || sz != sizeof(struct kmpw_inherit_blob)) { if(fm) CloseHandle(fm) ; return ; }
    struct kmpw_inherit_blob *b = (struct kmpw_inherit_blob*)MapViewOfFile(fm, FILE_MAP_READ, 0, 0, sz) ;
    if (b) {
        struct kmpw_inherit_blob loc ; memcpy(&loc, b, sizeof(loc)) ; UnmapViewOfFile(b) ;
        int ok = (loc.magic==KMPW_INHERIT_MAGIC && loc.version==1) ;
        if (ok && loc.wrapped) ok = kmpw_mem("CryptUnprotectMemory", loc.key, KSEC_MPW_KEYLEN) ;
        if (ok) {
            unsigned char salt[KSEC_MPW_SALTLEN] ;
            char *ver = mpw_state_get("MasterPwVerifier") ;
            int trust = 1 ;
            if (mpw_load_salt(salt) && ver && g_mpw_unprotect) {   /* store readable -> verify */
                trust = 0 ;
                if (!memcmp(salt, loc.salt, KSEC_MPW_SALTLEN)) {
                    char *vpt=NULL ;
                    if (g_mpw_unprotect(ver, loc.key, &vpt)==1 && vpt && !strcmp(vpt,KSEC_MPW_VERIFY)) trust = 1 ;
                    if (vpt) { memset(vpt,0,strlen(vpt)) ; sfree(vpt) ; }
                }
            }
            if (ver) free(ver) ;
            if (trust) {
                memcpy(g_mpw_key, loc.key, KSEC_MPW_KEYLEN) ;
                memcpy(g_mpw_salt, loc.salt, KSEC_MPW_SALTLEN) ;
                g_mpw_salt_valid = 1 ; g_mpw_unlocked = 1 ;
            }
        }
        SecureZeroMemory(&loc, sizeof(loc)) ;
    }
    CloseHandle(fm) ;
}

/* Parent: if the portable master password is unlocked, publish the key into a
 * fresh inheritable mapping and write "<prefix><hex>:<size>" into tok for the
 * child command line (prefix e.g. "&K" for the @/& dispatch, " -mpwkey " for the
 * argument parser). Returns the mapping HANDLE (close it AFTER CreateProcess), or
 * NULL (and tok emptied) when there is nothing to share. */
HANDLE kitty_mpw_export_inherit_blob(const char *prefix, char *tok, size_t toklen) {
    if (tok && toklen) tok[0] = '\0' ;
    if (!store_is_file() || !g_mpw_unlocked || !g_mpw_salt_valid) return NULL ;
    struct kmpw_inherit_blob blob ; memset(&blob, 0, sizeof(blob)) ;
    blob.magic = KMPW_INHERIT_MAGIC ; blob.version = 1 ;
    memcpy(blob.salt, g_mpw_salt, KSEC_MPW_SALTLEN) ;
    memcpy(blob.key, g_mpw_key, KSEC_MPW_KEYLEN) ;
    blob.wrapped = kmpw_mem("CryptProtectMemory", blob.key, KSEC_MPW_KEYLEN) ;
    SECURITY_ATTRIBUTES sa ; memset(&sa,0,sizeof(sa)) ; sa.nLength=sizeof(sa) ; sa.bInheritHandle=TRUE ;
    HANDLE fm = CreateFileMappingA(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, sizeof(blob), NULL) ;
    if (!fm || fm==INVALID_HANDLE_VALUE) { SecureZeroMemory(&blob,sizeof(blob)) ; return NULL ; }
    void *p = MapViewOfFile(fm, FILE_MAP_WRITE, 0, 0, sizeof(blob)) ;
    if (!p) { CloseHandle(fm) ; SecureZeroMemory(&blob,sizeof(blob)) ; return NULL ; }
    memcpy(p, &blob, sizeof(blob)) ; UnmapViewOfFile(p) ;
    SecureZeroMemory(&blob, sizeof(blob)) ;
    if (tok && toklen) snprintf(tok, toklen, "%s%p:%u", prefix?prefix:"", fm, (unsigned)sizeof(blob)) ;
    return fm ;
}

/* Child (@/& dispatch): consume a leading "&K<hex>:<size>" token from `p` and
 * return `p` advanced past it (or unchanged if no token). */
char *kitty_mpw_import_inherit_blob(char *p) {
    while (*p && isspace((unsigned char)*p)) p++ ;
    if (!(p[0]=='&' && p[1]=='K')) return p ;
    HANDLE fm=NULL ; unsigned sz=0 ; int n=0 ;
    if (sscanf(p+2, "%p:%u%n", &fm, &sz, &n) < 2 || n<=0) return p ;
    kmpw_consume(fm, sz) ;
    return p + 2 + n ;
}

/* Child (argument parser, launcher -load path): consume a "<hex>:<size>" string
 * from the -mpwkey option value. */
void kitty_mpw_consume_handle_str(const char *s) {
    HANDLE fm=NULL ; unsigned sz=0 ;
    if (s && sscanf(s, "%p:%u", &fm, &sz) == 2) kmpw_consume(fm, sz) ;
}

/* Root process: prompt once at startup to unlock the portable master password so
 * the whole run - and every child it spawns - is silent afterwards. No-op in
 * registry mode, when already unlocked, or when no master password is set. */
int kitty_mpw_startup_unlock(void) {
    if (g_mpw_unlocked) return 1 ;
    if (!store_is_file()) return 0 ;
    char *ver = mpw_state_get("MasterPwVerifier") ;
    if (!ver) return 0 ;
    free(ver) ;
    return mpw_ensure_unlocked(0) ;
}

/* plaintext -> stored form (malloc'd), backend-scoped policy
 * (TASK_dpapi_mpw_backend_policy.md): the protection is chosen by WHERE the
 * value is stored, not by a global user scheme.
 *
 * Registry backend: always DPAPI1. A leftover PasswordScheme=1/2 DWORD must
 * not make the hive plaintext or master-password, and a master-password
 * prompt must never appear for a registry save (this helper never calls
 * mpw_ensure_unlocked). DPAPI failure -> plaintext verbatim, kept only as the
 * exceptional never-lose fallback. */
static char *ksec_protect_registry(const char *plaintext)
{
    if (!plaintext || !plaintext[0]) return ksec_dup("");
    char *res = ksec_dpapi_protect(plaintext);
    return res ? res : ksec_dup(plaintext);
}

/* Portable-file backend: MPW1 when the master password is set/unlockable (the
 * first non-empty secret save may prompt to create it; a cancel stops further
 * prompts this run). kitty.ini PortablePasswordProtection=legacy selects the
 * explicit-compat plaintext escape hatch instead. MPW declined/unavailable ->
 * DPAPI1, so the secret never lands plaintext unintentionally: still readable
 * on this machine, and rewritten as MPW1 on a later protected save (read
 * policy). */
static char *ksec_protect_portable(const char *plaintext)
{
    if (!plaintext || !plaintext[0]) return ksec_dup("");
    if (g_portable_pw_legacy)
        return ksec_dup(plaintext);
    if (mpw_ensure_unlocked(1)) {
        char *mb = g_mpw_protect(plaintext, g_mpw_key);   /* "MPW1:..." (snew'd) */
        if (mb) {
            /* Wrap as self-contained MPW2 (salt embedded) so the value stays
             * unlockable away from this store's Security\/registry salt. */
            char *res = NULL;
            char *sb = g_mpw_salt_valid
                ? ksec_b64_encode(g_mpw_salt, KSEC_MPW_SALTLEN) : NULL;
            if (sb) {
                const char *payload = mb + strlen(KSEC_MPW_MARK);
                size_t n = strlen(KSEC_MPW2_MARK) + strlen(sb) + 1 +
                           strlen(payload) + 1;
                res = malloc(n);
                if (res)
                    snprintf(res, n, "%s%s.%s", KSEC_MPW2_MARK, sb, payload);
                free(sb);
            }
            sfree(mb);
            if (res) return res;
            /* MPW2 build failed (salt b64 OOM) -> do NOT persist bare MPW1: it
             * is the least portable form and would make MPW1 a live write format
             * again. Fall through to DPAPI below instead -- still encrypted at
             * rest, needs no store salt, and a later protected save re-wraps as
             * MPW2 (read policy). */
            kitty_pwdebug("MPW2 wrap failed (salt b64 OOM) -> DPAPI fallback");
        }
        /* else fall through to DPAPI so the secret is never lost */
    }
    char *res = ksec_dpapi_protect(plaintext);
    return res ? res : ksec_dup(plaintext);
}

/* stored -> plaintext in *out (malloc'd). 1=ok, 0=absent, -1=undecryptable DPAPI blob. */
static int ksec_unprotect(const char *stored, char **out)
{
    size_t marklen = strlen(KITTY_SECRET_DPAPI_MARK);
    *out = NULL;
    if (!stored || !stored[0]) { *out = ksec_dup(""); return 0; }
    if (!strncmp(stored, KSEC_MPW2_MARK, strlen(KSEC_MPW2_MARK))) {
        /* Self-contained: split "<b64 salt>.<payload>", derive with the
         * embedded salt (works for values from ANY store, e.g. imported .ktx). */
        const char *p = stored + strlen(KSEC_MPW2_MARK);
        const char *dot = strchr(p, '.');
        if (dot && dot > p) {
            char *sb = malloc((size_t)(dot - p) + 1);
            unsigned char *salt = NULL;
            int sn = 0;
            if (sb) {
                memcpy(sb, p, dot - p); sb[dot - p] = '\0';
                salt = ksec_b64_decode(sb, &sn);
                free(sb);
            }
            if (salt && sn == KSEC_MPW_SALTLEN) {
                size_t mn = strlen(KSEC_MPW_MARK) + strlen(dot + 1) + 1;
                char *m1 = malloc(mn);
                if (m1) {
                    snprintf(m1, mn, "%s%s", KSEC_MPW_MARK, dot + 1);
                    char *pt = NULL;
                    int rv = mpw_unprotect_with_salt(m1, salt, &pt); /* snew'd */
                    free(m1);
                    if (rv == 1 && pt) {
                        char *res = ksec_dup(pt);
                        memset(pt, 0, strlen(pt)); sfree(pt);
                        free(salt);
                        *out = res; return 1;
                    }
                    if (pt) sfree(pt);
                }
            }
            if (salt) free(salt);
        }
        *out = ksec_dup(""); return -1;   /* locked/wrong/malformed -> never-wipe */
    }
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

/* ---- .ktx forced-export glue (kitty_settings_forced.c / kitty_settings_load.c
 * and the bulk export/import in kitty_bridge.c). The forced serializer is a
 * SECOND portable-file write path that bypasses the write_setting_s chokepoint,
 * so it must apply the same backend password policy: wrap = portable policy
 * (MPW1 / DPAPI1 fallback / explicit-legacy plain), unwrap = stored-marker
 * dispatch. Session-name <-> filename munging is exposed alongside so bulk
 * export/import can round-trip arbitrary session names. Wrap/unwrap results
 * are malloc'd (free()); the munge results are snewn'd (sfree()). */
static int ksec_stored_is_legacy(const char *stored);
char *kitty_secret_wrap_portable(const char *plaintext)
{
    return ksec_protect_portable(plaintext ? plaintext : "");
}
/* Wrap for whatever backend is active now (registry -> DPAPI1, portable ->
 * MPW/legacy), for stores that live outside the write_setting_s chokepoint
 * (e.g. named proxies). Malloc'd; free() the result. */
char *kitty_secret_wrap_current_backend(const char *plaintext)
{
    return store_is_file() ? ksec_protect_portable(plaintext ? plaintext : "")
                           : ksec_protect_registry(plaintext ? plaintext : "");
}
/* True when portable at-rest protection is the explicit legacy/plaintext hatch
 * (kitty.ini PortablePasswordProtection=legacy): callers then keep passwords
 * plaintext deliberately and must NOT nag about it. */
int kitty_portable_password_legacy(void)
{
    return g_portable_pw_legacy;
}
int kitty_secret_is_marked(const char *stored)
{
    return stored && !ksec_stored_is_legacy(stored) && stored[0];
}
int kitty_secret_unwrap(const char *stored, char **out)
{
    return ksec_unprotect(stored, out);
}
char *kitty_session_fname_munge(const char *name)
{
    return ksf_munge(name ? name : "");
}
char *kitty_session_fname_unmunge(const char *name)
{
    return ksf_unmunge(name ? name : "");
}

/* never-wipe guard: keep an undecryptable-here blob so the next save re-persists it. */
static char *g_ksec_orig[2] = { NULL, NULL };
static void ksec_after_load(int slot, const char *stored, int rv)
{
    if (slot < 0 || slot > 1) return;
    if (g_ksec_orig[slot]) { free(g_ksec_orig[slot]); g_ksec_orig[slot] = NULL; }
    if (rv < 0 && stored && stored[0]) g_ksec_orig[slot] = ksec_dup(stored);
}

/* Legacy->protected migration consent (portable files only; user decision
 * 2026-07). Before a portable save rewrites a password stored in the old
 * unprotected (unmarked) form into a protected one, ask once per save via the
 * GUI-registered warner: 1 = re-encrypt, 0 = keep the stored value verbatim.
 * CLI tools never register a warner, so batch saves keep the old form. */
static int (*g_ksec_migrate_warn)(void) = NULL;
void kitty_set_legacy_migrate_warn(int (*fn)(void)) { g_ksec_migrate_warn = fn; }
static int ksec_stored_is_legacy(const char *stored)
{
    return stored && stored[0] &&
        strncmp(stored, KITTY_SECRET_DPAPI_MARK,
                strlen(KITTY_SECRET_DPAPI_MARK)) != 0 &&
        strncmp(stored, KSEC_MPW_MARK, strlen(KSEC_MPW_MARK)) != 0 &&
        strncmp(stored, KSEC_MPW2_MARK, strlen(KSEC_MPW2_MARK)) != 0;
}

/* Backend dispatch: store a string value either in the file-mode item list or
 * the registry. Sits below the credential-crypto hooks, so both backends get
 * the same at-rest encryption. */
static void ksf_or_reg_put(settings_w *handle, const char *key, const char *value)
{
    if (handle->is_file)
        ksf_list_set(&handle->items, key, value ? value : "");
    else
        put_reg_sz(handle->sesskey, key, value);
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
            ksf_or_reg_put(handle, key, keep);
        } else {
            /* Portable saves need consent before converting an old-format
             * (unmarked) stored password to a protected one; declining keeps
             * the pre-loaded stored value in handle->items untouched. */
            if (handle->is_file && value && value[0] && !g_portable_pw_legacy &&
                ksec_stored_is_legacy(ksf_list_get(handle->items, key))) {
                if (handle->mig_answer < 0)
                    handle->mig_answer =
                        g_ksec_migrate_warn ? g_ksec_migrate_warn() : 0;
                if (handle->mig_answer == 0)
                    return;
            }
            char *blob = handle->is_file
                ? ksec_protect_portable(value ? value : "")
                : ksec_protect_registry(value ? value : "");
            ksf_or_reg_put(handle, key, blob ? blob : "");
            if (blob) { memset(blob, 0, strlen(blob)); free(blob); }
        }
        return;
    }
    ksf_or_reg_put(handle, key, value);
}

void write_setting_i(settings_w *handle, const char *key, int value)
{
    if (!handle)
        return;
    if (handle->is_file) {
        char buf[32];
        sprintf(buf, "%d", value);
        ksf_list_set(&handle->items, key, buf);
    } else {
        put_reg_dword(handle->sesskey, key, value);
    }
}

void close_settings_w(settings_w *handle)
{
    if (!handle)
        return;
    if (handle->is_file) {
        if (handle->fpath) { ksf_save(handle->fpath, handle->items); sfree(handle->fpath); }
        ksf_list_free(handle->items);
    } else {
        close_regkey(handle->sesskey);
    }
    sfree(handle);
}

#define KSEC_HIVE_PRIMARY  0   /* our own kapper.net hive (or PuTTY base if KiClassName=PuTTY) */
#define KSEC_HIVE_OLDKITTY 1   /* read-only fallback: old 9bis KiTTY (legacy-encrypted passwords) */
#define KSEC_HIVE_PUTTY    2   /* read-only fallback: stock PuTTY (only our own cleartext can live here) */
struct settings_r {
    HKEY sesskey;
    int src_hive;
    int is_file;
    struct ksf_item *items;
};

settings_r *open_settings_r(const char *sessionname)
{
    if (!sessionname || !*sessionname)
        sessionname = KITTY_DEFAULT_SESSION;

    if (store_is_file()) {
        char *path = ksf_session_path(sessionname);
        if (!path) return NULL;
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) { sfree(path); return NULL; }
        settings_r *handle = snew(settings_r);
        handle->sesskey = NULL;
        handle->src_hive = KSEC_HIVE_PRIMARY;
        handle->is_file = 1;
        handle->items = ksf_load(path);
        sfree(path);
        return handle;
    }

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
    handle->is_file = 0;
    handle->items = NULL;
    return handle;
}

/* ---- legacy (<=0.76 old-KiTTY) password decrypt. Applied ONLY where a value
 * is guaranteed to be old-KiTTY-written: the old 9bis hive (read_setting_s)
 * and cyd01-syntax portable session files (ksf_load format conversion). Old
 * KiTTY stored "Password" as bcrypt_base64(plaintext) — plus a MASKPASS XOR
 * layer in some configurations — keyed on host+termtype+"KiTTY" (dopasskey
 * mode 0) or the fixed key "KiTTY" (mode >0). bcrypt is unauthenticated so we
 * must NOT apply this anywhere we might have written cleartext (our own hive,
 * our own ksf files, or the PuTTY hive via KiClassName=PuTTY). ---- */
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
/* True only if every byte is printable ASCII (0x20..0x7e). Used to tell a clean
 * plaintext password from MASKPASS XOR output, which leaves high-bit/control
 * bytes. */
static int ksec_all_printable(const char *s)
{
    if (!s || !*s) return 0;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x20 || c > 0x7e) return 0;
    }
    return 1;
}
/* Weaker sanity check for the last-resort path: no control bytes, but high-bit
 * (ANSI/UTF-8) bytes allowed — a legacy NON-ASCII password decodes to these. */
static int ksec_no_ctrl(const char *s)
{
    if (!s || !*s) return 0;
    for (; *s; s++)
        if ((unsigned char)*s < 0x20) return 0;
    return 1;
}
/* One buncrypt attempt with one key. cyd01 saved-session passwords are
 * bcrypt(plaintext): buncrypt ALONE yields the plaintext (verified against
 * real cyd01 0.76 registry AND portable session files). Only the rarer cyd01
 * 'cryptsalt' configuration adds the MASKPASS XOR layer, which leaves
 * high-bit/non-printable bytes. So apply MASKPASS ONLY when it actually turns
 * the result printable. (The pre-0.84.1.44 code MASKPASSed unconditionally,
 * corrupting every legacy password it touched.) Returns a malloc'd result
 * that passed the printable test, or NULL; the raw buncrypt output is handed
 * to *raw0 (once) so the caller can fall back to it. */
static char *ksec_try_legacy_key(const char *stored, const char *passkey,
                                 char **raw0)
{
    char *out = malloc(strlen(stored) + 16);
    int r;
    if (!out) return NULL;
    r = buncrypt_string_base64(stored, out, (unsigned)strlen(stored), passkey);
    if (r <= 0) { free(out); return NULL; }
    out[r] = '\0';            /* buncrypt returns the decoded length */
    if (ksec_all_printable(out)) return out;
    char *u = malloc(strlen(out) + 1);
    if (u) {
        strcpy(u, out);
        ksec_maskpass(u);
        if (ksec_all_printable(u)) {
            memset(out, 0, strlen(out)); free(out);
            return u;
        }
        free(u);
    }
    if (raw0 && !*raw0)
        *raw0 = out;          /* keep for the caller's last-resort fallback */
    else {
        memset(out, 0, strlen(out)); free(out);
    }
    return NULL;
}
/* Backend-neutral decoder core (host/term supplied by the caller: registry
 * wrapper below, cyd01 file conversion in ksf_load). Key order = most-specific
 * first; returns malloc'd plaintext or NULL (caller preserves stored bytes). */
static char *ksec_legacy_decrypt_hostterm(const char *stored, const char *host,
                                          const char *term)
{
    static int inited = 0;
    char passkey[1100];
    char *pt, *raw0 = NULL;
    if (!stored || !stored[0]) return NULL;
    if (!inited) { bcrypt_init(0); inited = 1; }
    /* dopasskey() mode 0: host + termtype + "KiTTY" (termtype default "xterm"). */
    snprintf(passkey, sizeof(passkey), "%s%sKiTTY",
             host ? host : "", (term && term[0]) ? term : "xterm");
    pt = ksec_try_legacy_key(stored, passkey, &raw0);
    /* dopasskey() mode >0 fallback: the fixed key "KiTTY" (alternate old
     * configurations; TASK_dpapi_passwords.md legacy-import fallback). */
    if (!pt)
        pt = ksec_try_legacy_key(stored, "KiTTY", NULL);
    /* Last resort: a legacy non-ASCII password fails the strict printable
     * test; accept the host-keyed output if it at least has no control bytes,
     * else give up so the caller preserves the stored value verbatim. */
    if (!pt && raw0 && ksec_no_ctrl(raw0)) {
        pt = raw0;
        raw0 = NULL;
    }
    if (raw0) { memset(raw0, 0, strlen(raw0)); free(raw0); }
    return pt;
}
static char *ksec_legacy_decrypt(const char *stored, HKEY sesskey)
{
    char *host = get_reg_sz(sesskey, "HostName");
    char *term = get_reg_sz(sesskey, "TerminalType");
    char *pt = ksec_legacy_decrypt_hostterm(stored, host, term);
    sfree(host);
    sfree(term);
    return pt;
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
    char *raw;
    if (handle->is_file) {
        const char *v = ksf_list_get(handle->items, key);
        raw = v ? dupstr(v) : NULL;
    } else {
        raw = get_reg_sz(handle->sesskey, key);
    }
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
        /* Migrate the auto-login password to UTF-8 (slot 0) so a legacy ANSI value
         * works at the UTF-8 prompt without re-entry; re-saved UTF-8 thereafter. */
        if (slot == 0) pt = ksec_to_utf8(pt);
        if (slot == 0)
            kitty_pwdebug("LOAD pw: mode=%d filemode=%d rawmark=%.7s declen=%d cksum=%04x",
                          g_store_mode, handle->is_file, raw ? raw : "(null)",
                          pt ? (int)strlen(pt) : -1, ksec_cksum(pt));
        sfree(raw);
        char *ret = dupstr(pt ? pt : "");
        if (pt) { memset(pt, 0, strlen(pt)); free(pt); }
        return ret;
    }
    return raw;
}

int read_setting_i(settings_r *handle, const char *key, int defvalue)
{
    if (!handle)
        return defvalue;
    if (handle->is_file) {
        const char *v = ksf_list_get(handle->items, key);
        return v ? atoi(v) : defvalue;
    }
    DWORD val;
    if (!get_reg_dword(handle->sesskey, key, &val))
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
        if (handle->is_file)
            ksf_list_free(handle->items);
        else
            close_regkey(handle->sesskey);
        sfree(handle);
    }
}

void del_settings(const char *sessionname)
{
    if (store_is_file()) {
        char *path = ksf_session_path(sessionname);
        if (path) { DeleteFileA(path); sfree(path); }
        remove_session_from_jumplist(sessionname);
        return;
    }

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
    int is_file;        /* names are plain (already-unmunged) session names */
};

settings_e *enum_settings_start(void)
{
    settings_e *e = snew(settings_e);
    e->names = NULL;
    e->count = 0;
    e->i = 0;
    e->is_file = 0;

    if (store_is_file()) {
        e->is_file = 1;
        int alloc = 0;
        char pat[1100];
        WIN32_FIND_DATAA fd;
        HANDLE hf;
        snprintf(pat, sizeof(pat), "%s\\*", g_sess_dir);
        hf = FindFirstFileA(pat, &fd);
        if (hf != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                char *nm = ksf_unmunge(fd.cFileName);   /* file name -> session name */
                if (!nm) continue;
                if (e->count >= alloc) {
                    alloc = alloc ? alloc * 2 : 16;
                    e->names = sresize(e->names, alloc, char *);
                }
                e->names[e->count++] = nm;
            } while (FindNextFileA(hf, &fd));
            FindClose(hf);
        }
        return e;
    }

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
    if (e->is_file)
        put_dataz(sb, e->names[e->i]);       /* already a plain session name */
    else
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

    if (store_is_file()) {
        char *otherstr = portable_read_text_file("SshHostKeys", regname->s);
        int exists = (otherstr != NULL);
        int compare = exists ? strcmp(otherstr, key) : -1;
        sfree(otherstr);
        strbuf_free(regname);
        if (!exists)
            return 1;                  /* key does not exist in portable store */
        else if (compare)
            return 2;                  /* key is different in portable store */
        else
            return 0;                  /* key matched OK in portable store */
    }

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

    int exists = (otherstr != NULL);
    int compare = exists ? strcmp(otherstr, key) : -1;

    sfree(otherstr);
    strbuf_free(regname);

    if (!exists)
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

    if (store_is_file()) {
        portable_write_text_file("SshHostKeys", regname->s, key);
        strbuf_free(regname);
        return;
    }

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
    int is_file;
    char **names;
    int count;
};

host_ca_enum *enum_host_ca_start(void)
{
    host_ca_enum *e;
    HKEY key;

    if (store_is_file()) {
        char *dir = portable_subdir_path("SshHostCAs");
        char pattern[MAX_PATH];
        WIN32_FIND_DATAA fd;
        HANDLE h;
        e = snew(host_ca_enum);
        e->key = NULL; e->i = 0; e->is_file = 1; e->names = NULL; e->count = 0;
        if (!dir) return e;
        snprintf(pattern, sizeof(pattern), "%s\\*", dir);
        h = FindFirstFileA(pattern, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    char *u = ksf_unmunge(fd.cFileName);
                    e->names = sresize(e->names, e->count + 1, char *);
                    e->names[e->count++] = u;
                }
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        sfree(dir);
        return e;
    }

    if (!(key = open_regkey_ro(HKEY_CURRENT_USER, host_ca_key)))
        return NULL;

    e = snew(host_ca_enum);
    e->key = key;
    e->i = 0;
    e->is_file = 0;
    e->names = NULL;
    e->count = 0;

    return e;
}

bool enum_host_ca_next(host_ca_enum *e, strbuf *sb)
{
    if (e->is_file) {
        if (e->i >= e->count)
            return false;
        put_dataz(sb, e->names[e->i++]);
        return true;
    }

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
    if (e->is_file) {
        int i;
        for (i = 0; i < e->count; i++)
            sfree(e->names[i]);
        sfree(e->names);
    } else {
        close_regkey(e->key);
    }
    sfree(e);
}

host_ca *host_ca_load(const char *name)
{
    strbuf *sb;
    const char *s;
    HKEY rkey = NULL;
    struct ksf_item *items = NULL;
    char *fpath = NULL;

    if (store_is_file()) {
        fpath = portable_item_path("SshHostCAs", name);
        if (!fpath) return NULL;
        items = ksf_load(fpath);
        sfree(fpath);
        if (!items) return NULL;
    } else {
        sb = strbuf_new();
        escape_registry_key(name, sb);
        rkey = open_regkey_ro(HKEY_CURRENT_USER, host_ca_key, sb->s);
        strbuf_free(sb);

        if (!rkey)
            return NULL;
    }

    host_ca *hca = host_ca_new();
    hca->name = dupstr(name);

    DWORD val;

    if ((s = store_is_file() ? ksf_list_get(items, "PublicKey") : get_reg_sz(rkey, "PublicKey")) != NULL)
        hca->ca_public_key = base64_decode_sb(ptrlen_from_asciz(s));

    if ((s = store_is_file() ? ksf_list_get(items, "Validity") : get_reg_sz(rkey, "Validity")) != NULL) {
        hca->validity_expression = strbuf_to_str(
            percent_decode_sb(ptrlen_from_asciz(s)));
    } else if (!store_is_file() && (sb = get_reg_multi_sz(rkey, "MatchHosts")) != NULL) {
        BinarySource src[1];
        BinarySource_BARE_INIT_PL(src, ptrlen_from_strbuf(sb));
        CertExprBuilder *eb = cert_expr_builder_new();

        const char *wc;
        while (wc = get_asciz(src), !get_err(src))
            cert_expr_builder_add(eb, wc);

        hca->validity_expression = cert_expr_expression(eb);
        cert_expr_builder_free(eb);
    }

    if (store_is_file()) {
        s = ksf_list_get(items, "PermitRSASHA1"); if (s) hca->opts.permit_rsa_sha1 = atoi(s);
        s = ksf_list_get(items, "PermitRSASHA256"); if (s) hca->opts.permit_rsa_sha256 = atoi(s);
        s = ksf_list_get(items, "PermitRSASHA512"); if (s) hca->opts.permit_rsa_sha512 = atoi(s);
        ksf_list_free(items);
    } else {
        if (get_reg_dword(rkey, "PermitRSASHA1", &val))
            hca->opts.permit_rsa_sha1 = val;
        if (get_reg_dword(rkey, "PermitRSASHA256", &val))
            hca->opts.permit_rsa_sha256 = val;
        if (get_reg_dword(rkey, "PermitRSASHA512", &val))
            hca->opts.permit_rsa_sha512 = val;

        close_regkey(rkey);
    }
    return hca;
}

char *host_ca_save(host_ca *hca)
{
    if (!*hca->name)
        return dupstr("CA record must have a name");

    if (store_is_file()) {
        char *dir = portable_subdir_path("SshHostCAs");
        char *path = portable_item_path("SshHostCAs", hca->name);
        struct ksf_item *items = NULL;
        char tmp[32];
        if (!dir || !path) { sfree(dir); sfree(path); return dupstr("Unable to build portable host CA path"); }
        CreateDirectoryA(dir, NULL);
        strbuf *base64_pubkey = base64_encode_sb(ptrlen_from_strbuf(hca->ca_public_key), 0);
        ksf_list_set(&items, "PublicKey", base64_pubkey->s);
        strbuf_free(base64_pubkey);
        strbuf *validity = percent_encode_sb(ptrlen_from_asciz(hca->validity_expression), NULL);
        ksf_list_set(&items, "Validity", validity->s);
        strbuf_free(validity);
        sprintf(tmp, "%u", (unsigned)hca->opts.permit_rsa_sha1); ksf_list_set(&items, "PermitRSASHA1", tmp);
        sprintf(tmp, "%u", (unsigned)hca->opts.permit_rsa_sha256); ksf_list_set(&items, "PermitRSASHA256", tmp);
        sprintf(tmp, "%u", (unsigned)hca->opts.permit_rsa_sha512); ksf_list_set(&items, "PermitRSASHA512", tmp);
        ksf_save(path, items);
        ksf_list_free(items);
        sfree(dir); sfree(path);
        return NULL;
    }

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
    if (store_is_file()) {
        char *path = portable_item_path("SshHostCAs", name);
        if (path) { DeleteFileA(path); sfree(path); }
        return NULL;
    }

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

    if (store_is_file()) {
        char *root = portable_root_dir();
        if (root) {
            char *path = dupprintf("%s\\PUTTY.RND", root);
            CreateDirectoryA(root, NULL);
            sfree(root);
            if (try_random_seed_and_free(path, action, &rethandle))
                return rethandle;
            if (action != DEL)
                return INVALID_HANDLE_VALUE;
        }
    }

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
    if (store_is_file()) {
        char *root = portable_root_dir();
        char *path = root ? dupprintf("%s\\Jumplist", root) : NULL;
        strbuf *oldlist = strbuf_new();
        FILE *fp;
        put_data(oldlist, "\0\0", 2);
        if (path && (fp = fopen(path, "rb")) != NULL) {
            char line[1024];
            oldlist->len = 0;
            while (fgets(line, sizeof(line), fp)) {
                size_t l = strlen(line);
                char *u;
                while (l && (line[l-1] == '\n' || line[l-1] == '\r')) line[--l] = '\0';
                u = ksf_unmunge(line);
                put_asciz(oldlist, u);
                sfree(u);
            }
            put_byte(oldlist, '\0');
            fclose(fp);
        }
        bool write_failure = false;
        if (add || rem) {
            BinarySource src[1];
            BinarySource_BARE_INIT_PL(src, ptrlen_from_strbuf(oldlist));
            strbuf *newlist = strbuf_new();
            if (add) put_asciz(newlist, add);
            while (true) {
                const char *olditem = get_asciz(src);
                if (get_err(src)) break;
                if (!rem || strcmp(olditem, rem) != 0) {
                    settings_r *psettings_tmp = open_settings_r(olditem);
                    if (psettings_tmp != NULL) {
                        close_settings_r(psettings_tmp);
                        put_asciz(newlist, olditem);
                    }
                }
            }
            if (path && root) {
                CreateDirectoryA(root, NULL);
                fp = fopen(path, "wb");
                if (fp) {
                    BinarySource outsrc[1];
                    BinarySource_BARE_INIT_PL(outsrc, ptrlen_from_strbuf(newlist));
                    while (true) {
                        const char *item = get_asciz(outsrc);
                        if (get_err(outsrc)) break;
                        char *m = ksf_munge(item);
                        fprintf(fp, "%s\n", m);
                        sfree(m);
                    }
                    write_failure = (fclose(fp) != 0);
                } else write_failure = true;
            } else write_failure = true;
            strbuf_free(oldlist);
            oldlist = newlist;
        }
        sfree(path); sfree(root);
        if (out && !write_failure)
            *out = strbuf_to_str(oldlist);
        else
            strbuf_free(oldlist);
        return write_failure ? JUMPLISTREG_ERROR_VALUEWRITE_FAILURE : JUMPLISTREG_OK;
    }

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
