/*
 * kitty_storage.c: the KiTTY half of windows/storage.c, split out to shrink
 * that file's divergence from upstream PuTTY.
 *
 * Everything here is fork-added state and helpers: the runtime-selected
 * registry root (kitty.ini KiClassName) with its read-only fallback hives,
 * the portable flat-file session store (ksf_*), the at-rest credential
 * crypto (DPAPI1/MPW2 + the master-password unlock state), and the legacy
 * (<=0.76 old-KiTTY) password decrypt. storage.c's upstream interface
 * functions dispatch into this file through kitty_storage.h; the bodies
 * here were moved VERBATIM (only `static` dropped on the cross-file
 * helpers), so diffing against the pre-split storage.c reviews the move.
 *
 * Like storage.c, this compiles into the settings lib AND standalone into
 * puttygen/kittygen_cli (windows/CMakeLists.txt): keep it free of
 * MOD_PERSO-style guards and GUI dependencies.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <limits.h>
#include <assert.h>
#include "putty.h"
#include "storage.h"
#include "kitty_defs.h"   /* KITTY_DEFAULT_SESSION (dependency-free) */
#include "kitty_b64.h"    /* ksec_b64_encode/decode (at-rest secret codec) */
#include "kitty_storage.h"

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
static const char *const puttystr = reg_sessions_buf;

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
int kitty_root_is_putty(void)
{ return strstr(reg_base_buf, "SimonTatham") != NULL; }

/* KiTTY: whether the saved-session list also shows (and lets you delete)
 * sessions from the read-only fallback hives (old 9bis KiTTY + stock PuTTY).
 * Default OFF, so by default KiTTY only shows/deletes its own hive and can never
 * touch a stock-PuTTY session without the user opting in. Persisted as a DWORD
 * under the base hive; toggled by a checkbox in the config dialog. */
static int kitty_show_foreign = -1;   /* -1 = not yet read */
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

/* See kitty_storage.h. Interlocked because the backup that consumes it runs on
 * a worker thread. */
static LONG kitty_store_dirty = 0;
void kitty_store_mark_dirty(void) { InterlockedExchange(&kitty_store_dirty, 1); }
int kitty_store_take_dirty(void) { return InterlockedExchange(&kitty_store_dirty, 0) != 0; }

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
unsigned ksec_cksum(const char *s)
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
int store_is_file(void) { return g_store_mode != 0 && g_sess_dir[0] != '\0'; }
/* Public query for UI surfaces (e.g. the config-box "(portable)" title tag). */
int kitty_storage_is_portable(void) { return store_is_file(); }

static const char ksf_hex[] = "0123456789ABCDEF";
static int ksf_special(unsigned char c)
{
    return c < 0x20 || c == 0x7f || c == '%' || c == '\\' || c == '/' ||
           c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
           c == '>' || c == '|';
}
char *ksf_munge(const char *in)            /* snewn'd */
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
char *ksf_unmunge(const char *in)          /* snewn'd */
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
char *ksf_list_get(struct ksf_item *h, const char *key)   /* borrowed or NULL */
{
    for (; h; h = h->next) if (!strcmp(h->key, key)) return h->val;
    return NULL;
}
void ksf_list_set(struct ksf_item **h, const char *key, const char *val)
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
void ksf_list_free(struct ksf_item *h)
{
    while (h) {
        struct ksf_item *n = h->next;
        sfree(h->key);
        if (h->val) { memset(h->val, 0, strlen(h->val)); sfree(h->val); }
        sfree(h);
        h = n;
    }
}
char *ksf_session_path(const char *sessionname)   /* snewn'd or NULL */
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

struct ksf_item *ksf_load(const char *path)       /* parsed list (may be NULL) */
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
void ksf_save(const char *path, struct ksf_item *h)
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

char *portable_root_dir(void)           /* snewn'd or NULL */
{
    char *root, *bs;
    if (!store_is_file()) return NULL;
    root = dupstr(g_sess_dir);
    bs = strrchr(root, '\\');
    if (bs && !_stricmp(bs + 1, "Sessions"))
        *bs = '\0';
    return root;
}

char *portable_subdir_path(const char *subdir)     /* snewn'd */
{
    char *root = portable_root_dir();
    char *path = root ? dupprintf("%s\\%s", root, subdir) : NULL;
    sfree(root);
    return path;
}

char *portable_item_path(const char *subdir, const char *name)
{
    char *dir = portable_subdir_path(subdir);
    char *m = ksf_munge(name ? name : "");
    char *path = (dir && m) ? dupprintf("%s\\%s", dir, m) : NULL;
    sfree(dir);
    sfree(m);
    return path;
}

int portable_write_text_file(const char *subdir, const char *name,
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

char *portable_read_text_file(const char *subdir, const char *name)
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


int kitty_secret_slot(const char *key)
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

/* ---- export-bundle passphrase (TASK_export_password.md) -------------------
 * An export bundle is a TRANSPORT artifact and carries its own password, which
 * is a different concept from the store's master password. While this context
 * is set, secrets written by the export path are wrapped with THIS passphrase
 * and the master password is neither read, created, prompted for nor written;
 * reads of a self-contained MPW2 value try it before falling back to the normal
 * master-password route.
 *
 * Process-scoped, following the established decoupling pattern
 * (kitty_set_storage_mode / kitty_set_session_dir / kitty_set_defer_mpw_prompt)
 * so libsettings stays standalone: the export/import loop sets it, runs, and
 * clears it. Nothing here is ever persisted - no salt, no verifier, no writes
 * to the g_mpw_* store state. */
static char *g_bundle_pass = NULL;
/* "This PC and this account only": wrap the bundle with DPAPI deliberately.
 * A distinct flag rather than merely "no passphrase", because the no-context
 * default must keep behaving exactly as it always has - and because in this
 * mode a master-password prompt must never appear either, which is precisely
 * what the ordinary portable policy would do. */
static int   g_bundle_dpapi = 0;
/* Set if any wrap during this bundle fell back to DPAPI (see
 * kitty_secret_wrap_portable): such a bundle imports only on this PC/account,
 * which the export summary must state rather than claim a password protects it. */
static int   g_bundle_wrap_failed = 0;
/* The bundle context is direction-aware. On IMPORT the passphrase must open the
 * bundle's values but must NOT be used to re-protect them: what gets saved
 * belongs to the destination store and has to carry the destination's
 * protection (registry -> DPAPI, portable -> master password). Without this
 * flag an import would rewrite every imported password under the transport
 * password, which nothing on that machine would know to ask for. */
static int   g_bundle_import = 0;
void kitty_set_bundle_import(int on) { g_bundle_import = (on != 0); }
void kitty_set_bundle_passphrase(const char *pass)
{
    if (g_bundle_pass) {
        SecureZeroMemory(g_bundle_pass, strlen(g_bundle_pass));
        free(g_bundle_pass);
    }
    g_bundle_pass = (pass && pass[0]) ? ksec_dup(pass) : NULL;
    g_bundle_wrap_failed = 0;
}
void kitty_set_bundle_dpapi_only(int on) { g_bundle_dpapi = (on != 0); }
void kitty_clear_bundle_context(void)
{
    kitty_set_bundle_passphrase(NULL);
    g_bundle_dpapi = 0;
    g_bundle_wrap_failed = 0;
    g_bundle_import = 0;
}
int kitty_bundle_passphrase_active(void) { return g_bundle_pass != NULL; }
int kitty_bundle_wrap_failed(void) { return g_bundle_wrap_failed; }

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
    if (!g_mpw_derive || !g_mpw_protect || !g_mpw_unprotect || !g_mpw_randsalt) {
        kitty_pwdebug("mpw unlock: crypto not linked (d=%d p=%d u=%d r=%d)",
                      g_mpw_derive != NULL, g_mpw_protect != NULL,
                      g_mpw_unprotect != NULL, g_mpw_randsalt != NULL);
        return 0;                                /* MPW crypto not linked in this tool */
    }

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
    kitty_pwdebug("mpw unlock FAILED: first_time=%d supplied=%d prompt=%d declined=%d",
                  first_time, from_supplied, g_mpw_prompt != NULL, g_mpw_declined);
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
    /* Import of an export bundle in progress: the bundle's own passphrase is
     * the right key, and it must be tried BEFORE anything that could prompt for
     * the master password - importing must never raise a master-password
     * dialog. The envelope's HMAC authenticates the attempt, so a wrong guess
     * simply fails through to the normal route. */
    if (g_bundle_pass) {
        unsigned char bkey[KSEC_MPW_KEYLEN];
        g_mpw_derive(g_bundle_pass, salt, KSEC_MPW_SALTLEN, bkey);
        int bok = (g_mpw_unprotect(m1blob, bkey, outp) == 1 && *outp);
        SecureZeroMemory(bkey, sizeof(bkey));
        if (bok) return 1;
        if (*outp) { sfree(*outp); *outp = NULL; }
    }
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

/* Compose a self-contained "MPW2:<b64 salt>.<payload>" from an "MPW1:..." blob
 * and the salt it was derived with. malloc'd, or NULL. */
static char *mpw2_compose(const char *m1blob, const unsigned char salt[KSEC_MPW_SALTLEN])
{
    char *sb = ksec_b64_encode(salt, KSEC_MPW_SALTLEN);
    char *res = NULL;
    if (sb) {
        const char *payload = m1blob + strlen(KSEC_MPW_MARK);
        size_t n = strlen(KSEC_MPW2_MARK) + strlen(sb) + 1 + strlen(payload) + 1;
        res = malloc(n);
        if (res) snprintf(res, n, "%s%s.%s", KSEC_MPW2_MARK, sb, payload);
        free(sb);
    }
    return res;
}

/* Wrap `plaintext` under an EXPLICIT passphrase, as a self-contained MPW2 value
 * (fresh salt minted per call and embedded in the result), so it unlocks
 * anywhere from that passphrase alone. This is what an export bundle needs.
 *
 * Persists NOTHING: no MasterPwSalt, no MasterPwVerifier, no writes to the
 * g_mpw_* store state and no touching of the key caches. That is the entire
 * point - exporting must not create a master password as a side effect.
 *
 * malloc'd result, or NULL if the MPW crypto is not linked into this tool or
 * the wrap fails. */
char *ksec_wrap_with_passphrase(const char *plaintext, const char *passphrase)
{
    unsigned char salt[KSEC_MPW_SALTLEN], key[KSEC_MPW_KEYLEN];
    char *m1, *res = NULL;
    if (!plaintext || !passphrase || !passphrase[0]) return NULL;
    if (!g_mpw_derive || !g_mpw_protect || !g_mpw_randsalt) return NULL;
    g_mpw_randsalt(salt, KSEC_MPW_SALTLEN);
    g_mpw_derive(passphrase, salt, KSEC_MPW_SALTLEN, key);
    m1 = g_mpw_protect(plaintext, key);          /* "MPW1:..." (snew'd) */
    if (m1) {
        res = mpw2_compose(m1, salt);
        memset(m1, 0, strlen(m1));
        sfree(m1);
    }
    SecureZeroMemory(key, sizeof(key));
    SecureZeroMemory(salt, sizeof(salt));
    return res;
}

/* Unwrap a self-contained MPW2 value with an EXPLICIT passphrase. Touches no
 * cached key and no store state, so a wrong passphrase costs nothing but the
 * derivation. Returns 1 and a malloc'd plaintext in *out, else 0. */
int ksec_unwrap_with_passphrase(const char *stored, const char *passphrase,
                                char **out)
{
    if (out) *out = NULL;
    if (!stored || !out || !passphrase || !passphrase[0]) return 0;
    if (!g_mpw_derive || !g_mpw_unprotect) return 0;
    if (strncmp(stored, KSEC_MPW2_MARK, strlen(KSEC_MPW2_MARK)) != 0) return 0;

    const char *p = stored + strlen(KSEC_MPW2_MARK);
    const char *dot = strchr(p, '.');
    if (!dot || dot <= p) return 0;

    char *sb = malloc((size_t)(dot - p) + 1);
    if (!sb) return 0;
    memcpy(sb, p, dot - p); sb[dot - p] = '\0';
    int sn = 0;
    unsigned char *salt = ksec_b64_decode(sb, &sn);
    free(sb);
    if (!salt || sn != KSEC_MPW_SALTLEN) { if (salt) free(salt); return 0; }

    size_t mn = strlen(KSEC_MPW_MARK) + strlen(dot + 1) + 1;
    char *m1 = malloc(mn);
    int ok = 0;
    if (m1) {
        unsigned char key[KSEC_MPW_KEYLEN];
        char *pt = NULL;
        snprintf(m1, mn, "%s%s", KSEC_MPW_MARK, dot + 1);
        g_mpw_derive(passphrase, salt, KSEC_MPW_SALTLEN, key);
        if (g_mpw_unprotect(m1, key, &pt) == 1 && pt) {   /* HMAC authenticates it */
            *out = ksec_dup(pt);
            ok = (*out != NULL);
        }
        if (pt) { memset(pt, 0, strlen(pt)); sfree(pt); }
        SecureZeroMemory(key, sizeof(key));
        free(m1);
    }
    free(salt);
    return ok;
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
char *ksec_protect_registry(const char *plaintext)
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
char *ksec_protect_portable(const char *plaintext)
{
    if (!plaintext || !plaintext[0]) return ksec_dup("");
    /* Which branch this takes is otherwise invisible, and "why did I get DPAPI
     * when I expected MPW2?" is the question that actually gets asked. No
     * plaintext, just the decision inputs. */
    kitty_pwdebug("protect portable: legacy=%d crypto=%d supplied=%d unlocked=%d declined=%d defer=%d",
                  g_portable_pw_legacy, g_mpw_derive != NULL,
                  g_mpw_passphrase != NULL, g_mpw_unlocked, g_mpw_declined,
                  g_mpw_defer);
    if (g_portable_pw_legacy)
        return ksec_dup(plaintext);
    if (mpw_ensure_unlocked(1)) {
        char *mb = g_mpw_protect(plaintext, g_mpw_key);   /* "MPW1:..." (snew'd) */
        if (mb) {
            /* Wrap as self-contained MPW2 (salt embedded) so the value stays
             * unlockable away from this store's Security\/registry salt. */
            char *res = g_mpw_salt_valid ? mpw2_compose(mb, g_mpw_salt) : NULL;
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
int ksec_unprotect(const char *stored, char **out)
{
    size_t marklen = strlen(KITTY_SECRET_DPAPI_MARK);
    *out = NULL;
    if (!stored || !stored[0]) { *out = ksec_dup(""); return 0; }
    /* PLAIN: - a password deliberately provisioned in the clear by an external
     * script. Handled here, at the shared chokepoint, so it works wherever a
     * stored value is read: an imported .ktx, a portable session file rolled
     * out by that script, or a registry value written by one. Strip the marker
     * and report success; the value is then re-protected by the destination
     * backend on the next save, so the cleartext lives only in the rollout
     * file. KiTTY never writes this marker itself. */
    if (!strncmp(stored, KITTY_SECRET_PLAIN_MARK,
                 strlen(KITTY_SECRET_PLAIN_MARK))) {
        *out = ksec_dup(stored + strlen(KITTY_SECRET_PLAIN_MARK));
        return 1;
    }
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
int ksec_stored_is_legacy(const char *stored);
char *kitty_secret_wrap_portable(const char *plaintext)
{
    if (!plaintext) plaintext = "";
    /* Export bundle in progress: protect with the bundle's OWN passphrase and
     * leave the master password alone entirely - no setup dialog, no
     * MasterPwSalt/MasterPwVerifier written, no Security\ folder created in a
     * portable tree. This is the whole point of TASK_export_password.md, and it
     * is why the hook sits here: both the session export
     * (save_open_settings_forced) and the named-proxy export
     * (kitty_export_proxies_to_dir) come through this one function, so a single
     * bundle password covers both.
     *
     * If the wrap itself fails (MPW crypto absent, or a CSPRNG/OOM failure) we
     * fall back to DPAPI rather than to ksec_protect_portable: the secret stays
     * encrypted and is never lost, and crucially we still do not prompt for a
     * master password. Such a bundle is then this-PC-only, so the caller must
     * say so - kitty_bundle_wrap_failed() reports it. */
    if (!g_bundle_import && (g_bundle_pass || g_bundle_dpapi)) {
        char *res;
        if (!plaintext[0]) return ksec_dup("");
        if (g_bundle_pass) {
            res = ksec_wrap_with_passphrase(plaintext, g_bundle_pass);
            if (res) return res;
            g_bundle_wrap_failed = 1;
            kitty_pwdebug("bundle wrap failed -> DPAPI fallback (bundle is this-PC-only)");
        }
        /* Deliberate "this PC only", or the never-lose fallback above. Either
         * way: DPAPI directly, never the portable policy, so no
         * master-password prompt can appear on an export. */
        res = ksec_dpapi_protect(plaintext);
        return res ? res : ksec_dup(plaintext);
    }
    return ksec_protect_portable(plaintext);
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
/* Past the PLAIN: provisioning marker if the value carries one, else the value
 * itself. Borrowed pointer, never NULL for a non-NULL argument. For the few
 * write-side callers that re-protect a raw stored value without reading it
 * through ksec_unprotect() first, so the marker is not wrapped up as part of
 * the password. */
const char *kitty_secret_strip_plain(const char *stored)
{
    if (stored && !strncmp(stored, KITTY_SECRET_PLAIN_MARK,
                           strlen(KITTY_SECRET_PLAIN_MARK)))
        return stored + strlen(KITTY_SECRET_PLAIN_MARK);
    return stored;
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
void ksec_after_load(int slot, const char *stored, int rv)
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
int ksec_stored_is_legacy(const char *stored)
{
    return stored && stored[0] &&
        strncmp(stored, KITTY_SECRET_DPAPI_MARK,
                strlen(KITTY_SECRET_DPAPI_MARK)) != 0 &&
        strncmp(stored, KSEC_MPW_MARK, strlen(KSEC_MPW_MARK)) != 0 &&
        strncmp(stored, KSEC_MPW2_MARK, strlen(KSEC_MPW2_MARK)) != 0;
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
/* Does `stored` carry the legacy format's header?
 *
 * bcrypt_string_base64 emits a 5-character header before the payload, and its
 * characters come from a tiny fixed alphabet - measured 2026-07-27 over 480,000
 * ciphertexts spanning 4 keys, 4 bcrypt_init seeds and plaintext lengths 0-200:
 * positions 0-2 are always one of "0123456bnv" and positions 3-4 one of
 * "0123bnpvx", with NOT ONE exception. The header does not depend on the key,
 * which is what makes it usable as a format test.
 *
 * This matters because buncrypt is unauthenticated: it cheerfully "decrypts"
 * arbitrary text into short garbage, and roughly 6% of ordinary alphanumeric
 * passwords decode to something that passes the printable test and would be
 * silently accepted as a decoded legacy value. Gating on the header cuts that
 * to 2 in 500,000 (measured over the same corpus) while rejecting none of the
 * 480,000 genuine values - i.e. it costs no read compatibility at all.
 *
 * A too-short value cannot be legacy either: the header alone is 5 characters
 * (an empty plaintext encodes to exactly 5). */
static int ksec_has_legacy_header(const char *stored)
{
    static const char *hdr012 = "0123456bnv";
    static const char *hdr34  = "0123bnpvx";
    int i;
    if (!stored || strlen(stored) < 5) return 0;
    for (i = 0; i < 3; i++) if (!strchr(hdr012, stored[i])) return 0;
    for (i = 3; i < 5; i++) if (!strchr(hdr34,  stored[i])) return 0;
    return 1;
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
    if (!ksec_has_legacy_header(stored)) return NULL;   /* not the legacy format */
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
/* Decode a password read from an IMPORTED .ktx, where the value's provenance is
 * unknown. Four cases, in this order because the deterministic ones must win:
 *
 *   PLAIN:<pw>   provisioning marker - taken literally, never guessed at. This
 *                is the supported way for a rollout script to ship a password.
 *   DPAPI1: /    our own protection - unwrapped; if it cannot be unwrapped here
 *   MPW1: MPW2:  (wrong PC/account, no master password) the runtime password is
 *                empty, and the .ktx is not rewritten, so nothing is lost.
 *   unmarked     could be old-KiTTY bcrypt+base64 OR simply cleartext. Attempt
 *                the legacy decode and accept it only if it yields a clean
 *                printable plaintext; otherwise take the value literally.
 *
 * That last fallback is the fix: the .ktx loader used to run every unmarked
 * value through decryptpassword + an UNCONDITIONAL MASKPASS, which silently
 * mangled cleartext into garbage (and, per ksec_try_legacy_key above, corrupted
 * genuine legacy values too in the common non-cryptsalt configuration).
 *
 * The unmarked case is a guess, but a well-constrained one: it requires the
 * legacy header (ksec_has_legacy_header) AND a successful decode AND printable
 * output. Measured 2026-07-27 against the real bcrypt library, that mistakes
 * cleartext for legacy about twice per 500,000 passwords, against roughly 6%
 * for the decode+printable test alone, and it rejects none of 480,000 genuine
 * legacy values. PLAIN: remains the deterministic answer for anyone who needs
 * certainty rather than very good odds.
 *
 * try_legacy=0 suppresses the guess entirely, for fields old KiTTY never
 * encrypted (ProxyPassword): there is no legacy form to find, so guessing could
 * only corrupt a good value.
 *
 * Returns malloc'd plaintext (caller frees), or NULL for empty input. */
char *kitty_secret_decode_imported(const char *stored, const char *host,
                                   const char *term, int try_legacy)
{
    char *pt;
    if (!stored || !stored[0]) return NULL;
    if (!strncmp(stored, KITTY_SECRET_PLAIN_MARK,
                 strlen(KITTY_SECRET_PLAIN_MARK)))
        return ksec_dup(stored + strlen(KITTY_SECRET_PLAIN_MARK));
    if (kitty_secret_is_marked(stored)) {
        pt = NULL;
        if (ksec_unprotect(stored, &pt) > 0 && pt) return pt;
        if (pt) { memset(pt, 0, strlen(pt)); free(pt); }
        return ksec_dup("");
    }
    if (!try_legacy) return ksec_dup(stored);
    pt = ksec_legacy_decrypt_hostterm(stored, host, term);
    return pt ? pt : ksec_dup(stored);
}

char *ksec_legacy_decrypt(const char *stored, HKEY sesskey)
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
char *ksec_to_utf8(char *s)
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

/* --------------------------------------------------------------------- *
 * Extraction-seam accessors: storage.c's upstream function bodies reach
 * the runtime state above through these (via the macro shims at its top),
 * so the state itself stays file-local here.
 */
const char *kitty_reg_sessions(void) { return reg_sessions_buf; }
const char *kitty_reg_jumplist(void) { return reg_jumplist_buf; }
const char *kitty_reg_hostcas(void)  { return reg_hostca_buf; }
const char *kitty_reg_hostkeys(void) { return reg_hostkeys_buf; }
const char *kitty_session_dir(void)  { return g_sess_dir; }
int kitty_storage_mode(void)         { return g_store_mode; }

/* never-wipe guard: the original stored blob for write_setting_s to
 * re-persist when the in-memory secret is empty because THIS session
 * could not decrypt it (NULL when there is nothing to preserve). */
const char *ksec_orig_get(int slot)
{
    return (slot >= 0 && slot <= 1) ? g_ksec_orig[slot] : NULL;
}

/* legacy->protected migration consent (0 = keep stored form; CLI tools
 * never register a warner, so batch saves keep the old form). */
int ksec_migrate_warn_ask(void)
{
    return g_ksec_migrate_warn ? g_ksec_migrate_warn() : 0;
}
