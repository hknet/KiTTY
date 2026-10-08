/*
 * kitty_storage.c: the KiTTY half of windows/storage.c, split out to shrink
 * that file's divergence from upstream PuTTY.
 *
 * Here is the fork's STORE state and its helpers: the runtime-selected
 * registry root (kitty.ini KiClassName) with its read-only fallback hives
 * and the show-foreign-sessions switch, the last session and folder, the
 * session comment and folder readers, and the portable flat-file session
 * store (ksf_*) with its state files. The at-rest credential crypto - the
 * DPAPI1/MPW2 wrapping, the master-password unlock state, the export-bundle
 * passphrase, the -pwfile forms and the legacy (<=0.76 old-KiTTY) password
 * decrypt - is in kitty_secretstore.c; what the two halves share is
 * declared in kitty_storage_int.h. storage.c's upstream interface functions
 * dispatch into both through kitty_storage.h and kitty_secretstore.h.
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
#include "kitty_oldwin_reg.h"   /* XP: RegDeleteTree/RegGetValue via oldwin */
#include "kitty_oldwin.h"   /* kitty_api_record: what this Windows cannot do */
#include "kitty_text.h"     /* KT_WINFEAT_*: the feature names that report names */
#include "kitty_pwmem.h"    /* the -pwfile helpers, defined in kitty_secretstore.c */
#include "kitty_inikeys.h"  /* KI_*: the kitty.ini key names */
#include "kitty.h"
#include "kitty_params.h"     /* ReadParameterN: the weak stub below is its fallback */
#include "kitty_storage_int.h"
#include "kitty_sessionpath.h" /* session names as folder paths */

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
char reg_base_buf[256]     = "Software\\kapper.net\\KiTTY";
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
 * Persisted as a DWORD under the base hive, written either by the checkbox in
 * the configuration box or - once, on the start that first meets an old hive -
 * by the adaptive rule below. */
static int kitty_show_foreign = -1;   /* -1 = not yet read */
int kitty_portable_store_state_string(const char *key, const char *value);
int kitty_portable_load_state_string(const char *key, char *buf, int buflen);
int kitty_portable_store_state_dword(const char *key, DWORD value);
int kitty_portable_load_state_dword(const char *key, DWORD *value);

/*
 * The read watch: while it is armed, every setting name the loader asks a
 * session for is reported to the watcher.
 *
 * It exists for the session importer, which has to say which values it could
 * NOT carry over. Asking the loader what it read is the only answer that
 * cannot go stale: the alternative is a hand-kept list of settings this base
 * no longer has, which is wrong the first time one is added or renamed.
 *
 * The callback lives behind a setter because storage.c is compiled into the
 * settings library that every binary links, while the importer is in the GUI
 * targets only - a direct call would not link for puttygen.
 */
static void (*kitty_read_watch_cb)(const char *key) = NULL;

void kitty_set_read_watch(void (*cb)(const char *key))
{
    kitty_read_watch_cb = cb;
}

void kitty_read_watch_note(const char *key)
{
    if (kitty_read_watch_cb && key)
        kitty_read_watch_cb(key);
}

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

/* Write the answer where the checkbox writes it, so the two are one setting
 * and not two that happen to agree. */
static void kitty_dword_store(const char *name, DWORD v)
{
    HKEY hk;
    if (store_is_file()) {
        kitty_portable_store_state_dword(name, v);
        return;
    }
    if (RegCreateKeyExA(HKEY_CURRENT_USER, reg_base_buf, 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &hk, NULL) == ERROR_SUCCESS) {
        RegSetValueExA(hk, name, 0, REG_DWORD, (const BYTE *)&v, sizeof(v));
        RegCloseKey(hk);
    }
}

static DWORD kitty_dword_load(const char *name)
{
    DWORD v = 0, sz = sizeof(v);
    if (store_is_file()) {
        if (!kitty_portable_load_state_dword(name, &v))
            v = 0;
        return v;
    }
    if (RegGetValueA(HKEY_CURRENT_USER, reg_base_buf, name,
                     RRF_RT_REG_DWORD, NULL, &v, &sz) != ERROR_SUCCESS)
        v = 0;
    return v;
}

static void kitty_persist_show_foreign(int on)
{
    kitty_dword_store(KR_SHOWFOREIGNSESSIONS, (DWORD)(on ? 1 : 0));
}

/*
 * The one-time notice. Held as BITS rather than a flag because the two places
 * that show it are shown at different moments and often on different runs: a
 * user who never opens the configuration box should still be told, and one who
 * dismissed the startup box should still find the line where the sessions are.
 */
#define KITTY_FOREIGN_NOTICE_VALUE "ForeignSessionsNotice"

static void kitty_foreign_notice_write(DWORD bits)
{
    kitty_dword_store(KITTY_FOREIGN_NOTICE_VALUE, bits);
}

int kitty_foreign_notice_pending(int bits)
{
    return (kitty_dword_load(KITTY_FOREIGN_NOTICE_VALUE) & (DWORD)bits) ? 1 : 0;
}

void kitty_foreign_notice_clear(int bits)
{
    DWORD v = kitty_dword_load(KITTY_FOREIGN_NOTICE_VALUE);
    if (!(v & (DWORD)bits))
        return;
    kitty_dword_store(KITTY_FOREIGN_NOTICE_VALUE, v & ~(DWORD)bits);
}

/* KiTTY (hknet/KiTTY#55): the one-time Arrange offer, kept in the store it
 * is about (the registry hive, or the folder store's state) so a store taken
 * to another PC or another copy carries its answer with it. */
#define KITTY_ARRANGE_OFFER_VALUE "SessionArrangeOffered"
int kitty_arrange_offer_pending(void)
{
    return kitty_dword_load(KITTY_ARRANGE_OFFER_VALUE) == 0;
}
void kitty_arrange_offer_made(void)
{
    kitty_dword_store(KITTY_ARRANGE_OFFER_VALUE, 1);
}

/* kitty.c: the kitty.ini reader, and the name of its main section. */
/* See the note above kitty_get_show_foreign_sessions: libsettings is linked by
 * the CLI tools, which have no kitty.c and no kitty.ini. */
__attribute__((weak))
int ReadParameterN(const char *key, const char *name, char *value, size_t size)
{
    (void)key; (void)name; (void)size;
    if (value) value[0] = '\0';
    return 0;
}
/* The console tools' way to the ONE ini key this file needs: the strong
 * definition lives in kitty_showforeign_ini.c, linked into klink, kscp and
 * ksftp only, and answers through the light kitty.ini resolver. Everywhere
 * else this stub says "not set" and ReadParameterN above has already
 * answered - the GUI's reading does not change. */
__attribute__((weak))
int kitty_showforeign_ini_read(char *value, size_t size)
{
    (void)size;
    if (value) value[0] = '\0';
    return 0;
}
/* May this process WRITE the one-time "auto" answer (ShowForeignSessions=1
 * and the foreign-sessions notice)? Here, in the GUI, yes - the historical
 * behaviour, byte for byte. The console-side unit above overrides it to 0:
 * klink, kscp and ksftp evaluate "auto" and persist nothing. */
__attribute__((weak))
int kitty_showforeign_may_persist(void)
{
    return 1;
}

int kitty_get_show_foreign_sessions(void)
{
    if (kitty_show_foreign < 0) {
        DWORD v = 0, sz = sizeof(v);
        if (store_is_file() && kitty_portable_load_state_dword(KR_SHOWFOREIGNSESSIONS, &v)) {
            kitty_show_foreign = v ? 1 : 0;
        } else if (RegGetValueA(HKEY_CURRENT_USER, reg_base_buf, KR_SHOWFOREIGNSESSIONS,
                         RRF_RT_REG_DWORD, NULL, &v, &sz) == ERROR_SUCCESS) {
            /* User has made an explicit choice: honour it. */
            kitty_show_foreign = v ? 1 : 0;
        } else {
            /*
             * No stored choice. kitty.ini decides: [KiTTY] showforeignsessions
             * = auto (the DEFAULT), yes, or no.
             *
             * "auto" is the behaviour KiTTY has always had: show the old 9bis
             * or PuTTY sessions only while this KiTTY has none of its own, so
             * that someone upgrading does not open onto an apparently empty
             * list, and stop showing them once there are real sessions here.
             * The key exists so the answer can be PINNED either way - it is
             * not there to change what happens by default.
             */
            char ini[32];
            ini[0] = '\0';
            /* The GUI reads it through kitty.c; a console tool through the
             * light resolver (the second call, a stub everywhere else). */
            /* The [KiTTY] section by NAME. This file is compiled into the
             * settings library, without the fork's define, and kitty.h then
             * spells INIT_SECTION "PuTTY": a read through that macro looked
             * in the wrong section and never saw the line. */
            if ((ReadParameterN(KI_SECTION_KITTY, KI_SHOWFOREIGNSESSIONS,
                                ini, sizeof(ini)) && ini[0]) ||
                (kitty_showforeign_ini_read(ini, sizeof(ini)) && ini[0])) {
                if (!_stricmp(ini, "auto"))
                    kitty_show_foreign =
                        (!kitty_root_is_putty() &&
                         kitty_primary_session_count() == 0) ? 1 : 0;
                else
                    kitty_show_foreign =
                        (!_stricmp(ini, "yes") || !_stricmp(ini, "1") ||
                         !_stricmp(ini, "true") || !_stricmp(ini, "on"))
                        ? 1 : 0;
            } else {
                /*
                 * auto: the historical adaptive rule - but ANSWERED ONCE
                 * rather than asked again on every start. Re-deciding meant
                 * the session list could change on its own: save your first
                 * session here and the old hive's sessions were gone at the
                 * next launch, with nothing to connect the two events.
                 *
                 * Only when there is actually an old hive with sessions in it
                 * is anything written down. On a machine that never ran PuTTY
                 * there is no question to answer, and the answer would be a
                 * registry value recording a decision about nothing.
                 */
                kitty_show_foreign =
                    (!kitty_root_is_putty() &&
                     kitty_primary_session_count() == 0) ? 1 : 0;
                /* Only KiTTY itself writes the answer down and arms the
                 * notice. A console tool (klink -load on a fresh install)
                 * evaluates "auto" read-only: it must not decide, on the
                 * GUI's behalf, what the GUI's next start shows. */
                if (kitty_show_foreign && kitty_has_foreign_sessions() &&
                    kitty_showforeign_may_persist()) {
                    kitty_persist_show_foreign(1);
                    kitty_foreign_notice_write(KITTY_FOREIGN_NOTICE_STARTUP |
                                               KITTY_FOREIGN_NOTICE_LIST);
                }
            }
        }
    }
    return kitty_show_foreign;
}
void kitty_set_show_foreign_sessions(int on)
{
    kitty_show_foreign = on ? 1 : 0;
    kitty_persist_show_foreign(kitty_show_foreign);
}

/* KiTTY: remember the last session loaded in the config box, so it can be
 * re-selected and re-loaded the next time the box opens. Stored as a string
 * value "LastSession" under the base hive. */
void kitty_set_last_session(const char *sessionname)
{
    if (store_is_file()) {
        kitty_portable_store_state_string(KR_LASTSESSION, sessionname ? sessionname : "");
        return;
    }
    HKEY hk;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, reg_base_buf, 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &hk, NULL) == ERROR_SUCCESS) {
        const char *v = sessionname ? sessionname : "";
        RegSetValueExA(hk, KR_LASTSESSION, 0, REG_SZ,
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
        return kitty_portable_load_state_string(KR_LASTSESSION, buf, buflen);
    if (RegGetValueA(HKEY_CURRENT_USER, reg_base_buf, KR_LASTSESSION,
                     RRF_RT_REG_SZ, NULL, buf, &sz) != ERROR_SUCCESS)
        return 0;
    buf[buflen-1] = '\0';
    return buf[0] ? 1 : 0;
}

/* Any other remembered string of the same kind (Manage Sessions' folder and
 * session): the portable state file, or a value of the hive in use. "" or
 * NULL removes it. */
void kitty_state_set_string(const char *key, const char *value)
{
    HKEY hk;
    if (store_is_file()) {
        kitty_portable_store_state_string(key, value ? value : "");
        return;
    }
    if (RegCreateKeyExA(HKEY_CURRENT_USER, reg_base_buf, 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &hk, NULL) == ERROR_SUCCESS) {
        if (value && *value)
            RegSetValueExA(hk, key, 0, REG_SZ, (const BYTE *)value,
                           (DWORD)strlen(value) + 1);
        else
            RegDeleteValueA(hk, key);
        RegCloseKey(hk);
    }
}
int kitty_state_get_string(const char *key, char *buf, int buflen)
{
    DWORD sz = (DWORD)buflen;
    if (!buf || buflen <= 0) return 0;
    buf[0] = '\0';
    if (store_is_file())
        return kitty_portable_load_state_string(key, buf, buflen);
    if (RegGetValueA(HKEY_CURRENT_USER, reg_base_buf, key,
                     RRF_RT_REG_SZ, NULL, buf, &sz) != ERROR_SUCCESS)
        return 0;
    buf[buflen-1] = '\0';
    return buf[0] ? 1 : 0;
}

void kitty_set_last_folder(const char *folder)
{
    if (!folder || !*folder) folder = "Default";
    if (store_is_file()) {
        kitty_portable_store_state_string(KR_LASTFOLDER, folder);
        return;
    }
    HKEY hk;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, reg_base_buf, 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &hk, NULL) == ERROR_SUCCESS) {
        RegSetValueExA(hk, KR_LASTFOLDER, 0, REG_SZ,
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
        return kitty_portable_load_state_string(KR_LASTFOLDER, buf, buflen);
    if (RegGetValueA(HKEY_CURRENT_USER, reg_base_buf, KR_LASTFOLDER,
                     RRF_RT_REG_SZ, NULL, buf, &sz) != ERROR_SUCCESS)
        return 0;
    buf[buflen-1] = '\0';
    return buf[0] ? 1 : 0;
}

/*
 * KiTTY: the registry hive this process is ACTUALLY using, so that every
 * module reads and writes the same one.
 *
 * Two things are easy to get backwards here, and both have caused real
 * defects:
 *
 *   PUTTY_REG_POS is OURS, not PuTTY's. It is "Software\kapper.net\KiTTY"
 *   (windows/platform.h) - the hive we use BY DEFAULT. Code that deliberately
 *   wants stock PuTTY's hive spells "Software\SimonTatham\PuTTY" out in full.
 *
 *   So the split is not "ours vs PuTTY's" but COMPILE-TIME vs RUNTIME. With
 *   kitty.ini's KiClassName=PuTTY, kitty_set_registry_root() points this base
 *   at PuTTY's hive and the sessions live there - while PUTTY_REG_POS still
 *   says kapper.net\KiTTY. Code holding the macro then touches a hive this
 *   process is not using: named proxies stored passwords in one hive while
 *   their sessions went to another, sav_backup() exported the wrong hive,
 *   savemode=file parked the wrong one, and /delreg deleted it.
 *
 * Use this for anything belonging to the store IN USE. Keep the macro only
 * where the default hive is genuinely meant whatever the current root is -
 * migration and adoption paths, which must name one specific hive.
 *
 * Returns the base WITHOUT any "\Sessions" / "\Launcher" suffix; callers
 * append their own, or use kitty_reg_sessions() and friends.
 */
const char *kitty_registry_base(void) { return reg_base_buf; }

/* See kitty_storage.h. Interlocked because the backup that consumes it runs on
 * a worker thread. */
static LONG kitty_store_dirty = 0;
/* KiTTY: a count of writes, for a window that shows the store and must know
 * whether to read it again (Organize sessions); never reset. */
static LONG kitty_store_writes = 0;
void kitty_store_mark_dirty(void)
{
    InterlockedExchange(&kitty_store_dirty, 1);
    InterlockedIncrement(&kitty_store_writes);
}
long kitty_store_generation(void) { return (long)kitty_store_writes; }
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
    /* then the read-only fallback hives, unless we're in PuTTY-root mode or
     * the old stores are hidden (the same rule as open_settings_r) */
    if (fallback_nonempty && (!result || !*result) && !kitty_root_is_putty() &&
        kitty_get_show_foreign_sessions()) {
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

/*
 * KiTTY: fold a session's legacy "Notes" value into its Comment.
 *
 * Classic KiTTY kept a second free-text field per session, written by the
 * send-text box's Shift+F2 / Shift+F3 keys and shown in a modal box when the
 * session opened. There is one note field now - the Comment - so a note that
 * arrives under the old name is merged into it and the Comment panel's
 * "Notify the user at login" is switched on, which is what the old field did.
 *
 * Empty Comment: the note becomes the Comment. Non-empty: the note is appended
 * after a blank line. CRLF throughout, because that is what the Comment's
 * multi-line edit box and REG_SZ round-trip (see scb_panel_comment).
 *
 * IDEMPOTENT, by WHOLE PARAGRAPHS. The note counts as already merged only when
 * the comment - split at blank lines - has a paragraph that IS the note. A
 * substring match would be wrong in both directions: a note of "db" would count
 * as merged into a comment reading "dbserver" and would then be lost when the
 * old value is dropped, while a comment that merely quotes a line of the note
 * would block the carry-over of the rest.
 *
 * The comparison normalises line endings (CRLF, CR and LF all read as one line
 * break) and ignores trailing spaces and tabs on every line, because the note
 * and the comment were typed into two different edit boxes by two different
 * versions. Only the COMPARISON is normalised: the text that is stored is the
 * comment and the note exactly as they were written.
 *
 * Nothing is written to the store here. The old value is dropped from the
 * session the next time it is saved, by the retired-key rule in
 * windows/storage.c - see kitty_retired_keys, which explains why retiring is
 * not done on a read path.
 */

/* Line endings to LF, trailing spaces/tabs off every line. Caller frees. */
static char *kitty_note_normalise(const char *s)
{
    size_t n = s ? strlen(s) : 0;
    char *out = snewn(n + 1, char);
    size_t o = 0, blanks = 0;     /* blanks: the run of spaces/tabs just written */
    const char *p = s ? s : "";

    while (*p) {
        if (*p == '\r' || *p == '\n') {
            if (*p == '\r' && p[1] == '\n')
                p++;
            p++;
            o -= blanks;          /* that run sat at the end of a line */
            blanks = 0;
            out[o++] = '\n';
        } else {
            if (*p == ' ' || *p == '\t')
                blanks++;
            else
                blanks = 0;
            out[o++] = *p++;
        }
    }
    o -= blanks;                  /* and at the end of the last line */
    out[o] = '\0';
    return out;
}

/* Does `nc` (normalised) have a paragraph equal to `nn` (normalised, with no
 * leading or trailing blank lines)? Paragraphs are separated by blank lines. */
static bool kitty_note_is_paragraph_of(const char *nc, const char *nn)
{
    size_t ln = strlen(nn);
    const char *p = nc;

    if (!ln)
        return false;
    while (*p) {
        const char *e;
        while (*p == '\n')        /* the blank lines between paragraphs */
            p++;
        if (!*p)
            break;
        for (e = p; *e; e++)
            if (e[0] == '\n' && e[1] == '\n')
                break;
        if ((size_t)(e - p) == ln && !memcmp(p, nn, ln))
            return true;
        p = e;
    }
    return false;
}

/*
 * The merge itself, on plain strings: the comment a session should show once
 * its legacy note is folded in, or NULL when there is nothing to change (no
 * note, or the note is already a paragraph of the comment). Caller frees.
 */
static char *kitty_note_merged_text(const char *comment, const char *note)
{
    char *nc, *nn, *start, *end;
    bool have;

    if (!note || !*note)
        return NULL;
    if (!comment)
        comment = "";

    nc = kitty_note_normalise(comment);
    nn = kitty_note_normalise(note);
    start = nn;
    while (*start == '\n')             /* the note's own leading blank lines */
        start++;
    end = start + strlen(start);
    while (end > start && end[-1] == '\n')
        end--;
    *end = '\0';
    have = !*start || kitty_note_is_paragraph_of(nc, start);
    sfree(nc);
    sfree(nn);
    if (have)
        return NULL;

    return *comment ? dupcat(comment, "\r\n\r\n", note) : dupstr(note);
}

void kitty_merge_legacy_note(Conf *conf, const char *note)
{
    const char *comment;
    char *merged;

    if (!conf || !note || !*note)
        return;
    comment = conf_get_str(conf, CONF_comment);
    merged = kitty_note_merged_text(comment, note);
    if (merged) {
        conf_set_str(conf, CONF_comment, merged);
        sfree(merged);
    }
    /* Owed whether or not the text changed: the old field always spoke up when
     * the session opened, so a session that carries one asks to be notified. */
    conf_set_bool(conf, CONF_comment_notify, true);
}

char *kitty_read_session_comment(const char *sessionname)
{
    /* If a session exists in the primary hive with an intentionally empty
     * Comment, keep it empty. Falling back to old hives here made the config
     * dialog show stale comments from migrated/legacy sessions with the same
     * name (e.g. an old 9bis entry overwriting an empty kapper.net comment). */
    char *comment = kitty_read_session_value_direct(sessionname, KR_COMMENT, 0);
    /*
     * A session written by an older KiTTY may still carry its note under the
     * retired name. The session list's read-only preview has to show it BEFORE
     * that session is ever loaded, so the same merge the load path does is done
     * here on the text - read from the same hive, nothing written back.
     */
    char *note = kitty_read_session_value_direct(sessionname, KR_NOTES, 0);
    if (note) {
        char *merged = kitty_note_merged_text(comment, note);
        if (merged) {
            sfree(comment);
            comment = merged;
        }
        sfree(note);
    }
    return comment;
}

/*
 * The folder of a session, cached. The session list asks for it several
 * times per refresh for EVERY saved session (the level filter, the folder
 * rows and the selection's position each ask), and a refresh happens on
 * every keystroke in the filter box - with a few hundred sessions that was
 * hundreds of registry opens per keystroke. The cache answers the repeats.
 * It is cleared wherever the list is re-read from the store or a folder
 * value is written (kitty_config.c), and it expires by itself after two
 * seconds, so nothing writing behind our back is shown stale for longer.
 */
static struct {
    char **names, **folders;           /* folders[i] NULL: no folder */
    int n, cap;
    DWORD stamp;                       /* when the oldest entry went in */
} kitty_folder_cache;

void kitty_session_folder_cache_clear(void)
{
    int i;
    for (i = 0; i < kitty_folder_cache.n; i++) {
        sfree(kitty_folder_cache.names[i]);
        sfree(kitty_folder_cache.folders[i]);
    }
    kitty_folder_cache.n = 0;
}

/* The direct read: what the mid-session save asks, because a session may
 * have been moved from another window since this one launched, and the
 * store is the truth. */
char *kitty_read_session_folder(const char *sessionname)
{
    if (sessionname && !strcmp(sessionname, KITTY_DEFAULT_SESSION))
        return NULL;
    /* A session stored under a path is in that path's folder, whatever its
     * Folder value says (hknet/KiTTY#55): the path wins, and the next save
     * writes the matching Folder value back. */
    if (sessionname && strchr(sessionname, '\\'))
        return ksp_folder_of(sessionname);
    return kitty_read_session_value_direct(sessionname, KR_FOLDER, 0);
}

/* The cached read, for the list refresh loops only (see above). */
char *kitty_read_session_folder_cached(const char *sessionname)
{
    int i;
    char *v;
    DWORD now;
    /* Default Settings is in no folder - it is shown at every level - so a
     * stored value on it is answered as "none" rather than passed on. Without
     * this it reaches the folder-list rebuild, which resurrects folders that
     * were deleted or renamed, and the mid-session save, which reads the folder
     * back from storage. The loader clears it on the way in (settings.c); this
     * is the same rule for the paths that read the store directly. */
    if (sessionname && !strcmp(sessionname, KITTY_DEFAULT_SESSION))
        return NULL;
    if (!sessionname)
        return kitty_read_session_value_direct(sessionname, KR_FOLDER, 0);
    if (strchr(sessionname, '\\'))
        return ksp_folder_of(sessionname);   /* the path wins: no store read */

    now = GetTickCount();
    if (kitty_folder_cache.n && now - kitty_folder_cache.stamp > 2000)
        kitty_session_folder_cache_clear();
    for (i = 0; i < kitty_folder_cache.n; i++)
        if (!strcmp(kitty_folder_cache.names[i], sessionname))
            return kitty_folder_cache.folders[i] ?
                dupstr(kitty_folder_cache.folders[i]) : NULL;

    v = kitty_read_session_value_direct(sessionname, KR_FOLDER, 0);
    if (kitty_folder_cache.n == kitty_folder_cache.cap) {
        kitty_folder_cache.cap = kitty_folder_cache.cap ? kitty_folder_cache.cap * 2 : 64;
        kitty_folder_cache.names = sresize(kitty_folder_cache.names, kitty_folder_cache.cap, char *);
        kitty_folder_cache.folders = sresize(kitty_folder_cache.folders, kitty_folder_cache.cap, char *);
    }
    if (kitty_folder_cache.n == 0)
        kitty_folder_cache.stamp = now;
    kitty_folder_cache.names[kitty_folder_cache.n] = dupstr(sessionname);
    kitty_folder_cache.folders[kitty_folder_cache.n] = v ? dupstr(v) : NULL;
    kitty_folder_cache.n++;
    return v;
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

/* ksf_munge / ksf_unmunge: kitty_sessionpath.c, beside the path rules. */

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
/* KiTTY: drop a key from a loaded session list. Used to retire a renamed
 * setting when the session is next saved - open_settings_w pre-loads the file,
 * so a key nobody writes any more would otherwise be carried forever. */
void ksf_list_del(struct ksf_item **h, const char *key)
{
    struct ksf_item **pp = h;
    while (*pp) {
        struct ksf_item *it = *pp;
        if (!strcmp(it->key, key)) {
            *pp = it->next;
            sfree(it->key);
            if (it->val) { memset(it->val, 0, strlen(it->val)); sfree(it->val); }
            sfree(it);
            return;
        }
        pp = &it->next;
    }
}
void ksf_list_foreach(struct ksf_item *h,
                      void (*fn)(const char *key, const char *val, void *ctx),
                      void *ctx)
{
    for (; h; h = h->next)
        fn(h->key, h->val, ctx);
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
/* ===================================================================== *
 * KiTTY: session names as folder paths (hknet/KiTTY#55) and the session
 * file suffix (hknet/KiTTY#56).
 *
 * A session "Linux\web\srv01" is the file Sessions\Linux\web\srv01<suffix>,
 * every component escaped by ksp_component_munge (kitty_sessionpath.c). Two
 * older layouts are still READ, and a save moves such a session to the
 * current one:
 *   - the same path without the suffix (a store from before the suffix was
 *     set, or a classic store whose putty.conf named one);
 *   - the flat file Sessions\<ksf_munge(whole name)> this backend wrote before
 *     names became paths. For a plain name that IS the current file; it only
 *     differs for a name with a reserved word, a leading or trailing dot or
 *     space, or a '\' in it.
 * The legacy file wins while it exists: it is the one on screen, and saving
 * it either migrates it or - when the current file is ALSO there, as another
 * session - is refused with the save-clash text (KT_SP_SAVE_CLASH).
 * ===================================================================== */
static char g_sess_suffix[64] = "";

void kitty_set_session_suffix(const char *suffix)
{
    /* A suffix is a file-name ending, so the characters the escape would
     * rewrite have no place in it; refuse rather than half-apply. */
    const char *p;
    g_sess_suffix[0] = '\0';
    if (!suffix)
        return;
    for (p = suffix; *p; p++)
        if ((unsigned char)*p < 0x20 || strchr("\\/:*?\"<>|", *p))
            return;
    strncpy(g_sess_suffix, suffix, sizeof(g_sess_suffix) - 1);
    g_sess_suffix[sizeof(g_sess_suffix) - 1] = '\0';
}
const char *kitty_session_suffix(void) { return g_sess_suffix; }

/* Where a save writes the session. */
char *ksf_session_target_path(const char *sessionname)   /* snewn'd */
{
    char *rel = ksp_path_to_relfile(sessionname ? sessionname : "", g_sess_suffix);
    char *p = dupprintf("%s\\%s", g_sess_dir, rel);
    sfree(rel);
    return p;
}

static bool ksf_exists_file(const char *p)
{
    DWORD a = p ? GetFileAttributesA(p) : INVALID_FILE_ATTRIBUTES;
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/*
 * Without a suffix a session and a folder can want the same name on disk: the
 * root session "Linux" is the FILE Sessions\Linux, the folder "Linux" the
 * DIRECTORY Sessions\Linux. Only one of them can exist. These name what is in
 * the way (relative to the session directory, snewn'd), or return NULL:
 *   ksf_path_blocker   - for a session: a directory where its file goes, or
 *                        a file where one of its folders goes;
 *   ksf_folder_blocker - for a folder: a file where it or a folder above it
 *                        goes.
 * A save, a move or a new folder that meets one is refused, never done
 * half-way (fopen on a directory fails without a word).
 */
static bool ksf_exists_dir(const char *p)
{
    DWORD a = p ? GetFileAttributesA(p) : INVALID_FILE_ATTRIBUTES;
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
static char *ksf_parent_file_blocker(const char *rel)
{
    /* every proper prefix of rel ending before a '\' */
    char *buf = dupstr(rel), *q, *hit = NULL;
    for (q = buf; !hit && (q = strchr(q, '\\')) != NULL; q++) {
        char *full;
        *q = '\0';
        full = dupprintf("%s\\%s", g_sess_dir, buf);
        if (ksf_exists_file(full))
            hit = dupstr(buf);
        sfree(full);
        *q = '\\';
    }
    sfree(buf);
    return hit;
}
char *ksf_path_blocker(const char *sessionname)
{
    char *rel, *full, *hit;
    if (!g_sess_dir[0] || !sessionname || !*sessionname)
        return NULL;
    rel = ksp_path_to_relfile(sessionname, g_sess_suffix);
    full = dupprintf("%s\\%s", g_sess_dir, rel);
    hit = ksf_exists_dir(full) ? dupstr(rel) : ksf_parent_file_blocker(rel);
    sfree(full);
    sfree(rel);
    return hit;
}
char *ksf_folder_blocker(const char *folder)
{
    char *rel, *withleaf, *hit;
    if (!g_sess_dir[0] || ksp_folder_is_root(folder))
        return NULL;
    rel = ksp_path_to_relfile(folder, "");
    /* the folder itself counts too: test it as the parent of a leaf */
    withleaf = dupcat(rel, "\\x");
    hit = ksf_parent_file_blocker(withleaf);
    sfree(withleaf);
    sfree(rel);
    return hit;
}

/* The legacy file holding this session, if there is one and it differs from
 * the target; NULL otherwise. snewn'd. */
char *ksf_session_legacy_path(const char *sessionname)
{
    char *target = ksf_session_target_path(sessionname);
    char *cand[2];
    char *found = NULL;
    int i;
    {
        char *rel = ksp_path_to_relfile(sessionname ? sessionname : "", "");
        cand[0] = dupprintf("%s\\%s", g_sess_dir, rel);
        sfree(rel);
    }
    {
        char *m = ksf_munge(sessionname ? sessionname : "");
        cand[1] = dupprintf("%s\\%s", g_sess_dir, m);
        sfree(m);
    }
    for (i = 0; i < 2 && !found; i++)
        if (stricmp(cand[i], target) && ksf_exists_file(cand[i]) &&
            ksp_file_is_session(cand[i]))
            found = dupstr(cand[i]);
    sfree(cand[0]);
    sfree(cand[1]);
    sfree(target);
    return found;
}

/*
 * KiTTY (hknet/KiTTY#59): a session file under a name of another escape -
 * old KiTTY's and PuTTY's (a space as %20, a byte above '~' as %E4), a '%'
 * written by hand ("100%"), any %xx this store's escape would not write. The
 * list shows such a file under its DECODED name (ksp_component_unmunge, the
 * ending stripped), so it is found the same way. Such a file is read and
 * SAVED IN PLACE (windows/storage.c): renaming it would split an old folder
 * like "Web%20Servers" from its new twin and take it from an old KiTTY
 * reading the same folder - a rename is the user's (Rename, Organize).
 *
 * A name without a file under today's spelling - each new session name, the
 * window-position and import exists checks - must not walk the folders each
 * time. So the store keeps a table of the session files whose path is NOT
 * today's spelling of the name it decodes to (ksp_is_today_spelling; a file
 * in "Web%20Servers" is one too), with the folders it was read from and
 * their last-write times, and a lookup looks only there. A store KiTTY++
 * wrote has an empty table. Several files for one name: the one
 * ksp_reach_cmp puts first, the file the list shows; the others are the red
 * rows of ksf_enum_twins. The list's full walk (ksf_enum_names) builds the
 * table; the first lookup before any list builds it by the names alone (no
 * file read: the few a name finds are judged then).
 *
 * Before each lookup (ksf_alts_check) the folders the name could sit in -
 * the session directory and every folder whose list path is a folder of the
 * name - are asked for their time. A file or folder added, removed or renamed
 * in one changes it, and that folder alone is listed again (ksf_alts_relist),
 * with any folder new in it; a folder gone, or another session directory or
 * suffix, builds the table anew. A time less than KSF_ALTS_SETTLE old when it
 * was read is not trusted (a second change within the clock tick would not
 * move it): such a folder is listed again at each lookup until it settles. KiTTY++'s own saves, deletes,
 * renames and moves change their folders' times like any other writer's, so
 * the store's write count (kitty_store_generation) is not asked: it also
 * counts host keys, proxies and tags, which change no session file name.
 * Not seen until a list: a file whose CONTENT turns it into a session (no
 * name changes), and changes on a file system that keeps no folder times.
 */
struct ksf_alt {
    char *id;                           /* the name the file decodes to */
    char *path;
};
static struct {
    int built;
    char *root, *suffix;
    struct ksf_alt *e;                  /* by id without case, then reach */
    int n;
    size_t ae;
    struct ksp_store_dir *dirs;         /* every folder of the store */
    int ndirs;
    size_t adirs;
    struct ksp_store_file *sh;          /* the list walk's shadowed files */
    int nsh;
    int sh_judged;                      /* sh is from the list's walk, and
                                         * nothing was listed again since */
} ksf_alts;
static CRITICAL_SECTION ksf_alts_cs;
static volatile LONG ksf_alts_cs_state;  /* 0 none, 1 being set up, 2 up */
#define KSF_ALTS_SETTLE (3ULL * 10000000ULL)   /* 100 ns units */

/* Lists run on whatever thread asks, so the table is under a lock, set up
 * lazily as the verdict cache's is (no Vista+ import: the 32-bit build loads
 * on XP). The list's walk reads files outside it; a relist by names holds
 * it (it reads no file). */
static void ksf_alts_lock(void)
{
    if (ksf_alts_cs_state != 2) {
        if (InterlockedCompareExchange(&ksf_alts_cs_state, 1, 0) == 0) {
            InitializeCriticalSection(&ksf_alts_cs);
            InterlockedExchange(&ksf_alts_cs_state, 2);
        } else {
            while (ksf_alts_cs_state != 2)
                Sleep(0);
        }
    }
    EnterCriticalSection(&ksf_alts_cs);
}
static void ksf_alts_unlock(void)
{
    LeaveCriticalSection(&ksf_alts_cs);
}

static int ksf_alt_cmp(const void *av, const void *bv)
{
    const struct ksf_alt *a = (const struct ksf_alt *)av;
    const struct ksf_alt *b = (const struct ksf_alt *)bv;
    size_t base = strlen(g_sess_dir) + 1;
    int c = stricmp(a->id, b->id);
    if (c)
        return c;
    return ksp_reach_cmp(a->id, a->path + base, b->path + base, g_sess_suffix);
}

/* Is `path` directly in the folder `dir` (both full paths)? Below it? */
static bool ksf_path_in(const char *path, const char *dir)
{
    size_t dl = strlen(dir);
    return !strnicmp(path, dir, dl) && path[dl] == '\\' &&
        !strchr(path + dl + 1, '\\');
}
static bool ksf_path_below(const char *path, const char *dir)
{
    size_t dl = strlen(dir);
    return !strnicmp(path, dir, dl) && path[dl] == '\\';
}

/* Under the lock: the table emptied; files added (those not of today's
 * spelling); folders added (copied, the unread ones left out). */
static void ksf_alts_clear(void)
{
    int i;
    for (i = 0; i < ksf_alts.n; i++) {
        sfree(ksf_alts.e[i].id);
        sfree(ksf_alts.e[i].path);
    }
    sfree(ksf_alts.e);
    ksf_alts.e = NULL;
    ksf_alts.n = 0;
    ksf_alts.ae = 0;
    ksp_walk_dirs_free(ksf_alts.dirs, ksf_alts.ndirs);
    ksf_alts.dirs = NULL;
    ksf_alts.ndirs = 0;
    ksf_alts.adirs = 0;
    ksp_walk_store_free(ksf_alts.sh, ksf_alts.nsh);
    ksf_alts.sh = NULL;
    ksf_alts.nsh = 0;
    ksf_alts.sh_judged = 0;
    sfree(ksf_alts.root);
    sfree(ksf_alts.suffix);
    ksf_alts.root = dupstr(g_sess_dir);
    ksf_alts.suffix = dupstr(g_sess_suffix);
    ksf_alts.built = 1;
}
static void ksf_alts_add_files(const struct ksp_store_file *f, int n)
{
    size_t base = strlen(g_sess_dir) + 1;
    int i;
    for (i = 0; i < n; i++) {
        if (ksp_is_today_spelling(f[i].id, f[i].path + base, g_sess_suffix))
            continue;
        sgrowarray(ksf_alts.e, ksf_alts.ae, ksf_alts.n);
        ksf_alts.e[ksf_alts.n].id = dupstr(f[i].id);
        ksf_alts.e[ksf_alts.n].path = dupstr(f[i].path);
        ksf_alts.n++;
    }
}
static void ksf_alts_add_dirs(const struct ksp_store_dir *d, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        if (!d[i].seen)
            continue;
        sgrowarray(ksf_alts.dirs, ksf_alts.adirs, ksf_alts.ndirs);
        ksf_alts.dirs[ksf_alts.ndirs] = d[i];
        ksf_alts.dirs[ksf_alts.ndirs].folder = dupstr(d[i].folder);
        ksf_alts.dirs[ksf_alts.ndirs].path = dupstr(d[i].path);
        ksf_alts.ndirs++;
    }
}
static void ksf_alts_sort(void)
{
    if (ksf_alts.n > 1)
        qsort(ksf_alts.e, ksf_alts.n, sizeof(*ksf_alts.e), ksf_alt_cmp);
}

/* The list's full walk becomes the table (`sh` and `dirs` handed over, `v`
 * only read). */
static void ksf_alts_take(const struct ksp_store_file *v, int n,
                          struct ksp_store_file *sh, int nsh,
                          struct ksp_store_dir *dirs, int ndirs)
{
    ksf_alts_lock();
    ksf_alts_clear();
    ksf_alts_add_files(v, n);
    ksf_alts_add_files(sh, nsh);
    ksf_alts_add_dirs(dirs, ndirs);
    ksf_alts_sort();
    ksf_alts.sh = sh;
    ksf_alts.nsh = nsh;
    ksf_alts.sh_judged = 1;
    ksf_alts_unlock();
    ksp_walk_dirs_free(dirs, ndirs);
}

/* The list's full walk, with the table built from it. Returns the walk's
 * files (free with ksp_walk_store_free). */
static struct ksp_store_file *ksf_walk_full(int *count)
{
    struct ksp_store_file *v, *sh;
    struct ksp_store_dir *dirs;
    int nsh, ndirs;
    v = ksp_walk_store_ex(g_sess_dir, g_sess_suffix, count, &sh, &nsh,
                          &dirs, &ndirs);
    ksf_alts_take(v, *count, sh, nsh, dirs, ndirs);
    return v;
}

/* Under the lock: the table built anew by the names alone. */
static void ksf_alts_rebuild(void)
{
    struct ksp_store_file *f;
    struct ksp_store_dir *d;
    int n, nd;
    f = ksp_walk_names(g_sess_dir, g_sess_dir, "", 1, g_sess_suffix, &n, &d, &nd);
    ksf_alts_clear();
    ksf_alts_add_files(f, n);
    ksf_alts_add_dirs(d, nd);
    ksf_alts_sort();
    ksp_walk_store_free(f, n);
    ksp_walk_dirs_free(d, nd);
}

/* Under the lock: the folder ksf_alts.dirs[k] listed again - its own files
 * replaced, a subfolder gone dropped with all below it, a new one walked
 * whole. False when it cannot be listed (the caller rebuilds). */
static bool ksf_alts_relist(int k)
{
    char *dpath = dupstr(ksf_alts.dirs[k].path);
    struct ksp_store_file *f;
    struct ksp_store_dir *d;
    int n, nd, i, j, keep;
    bool ok;
    f = ksp_walk_names(g_sess_dir, dpath, ksf_alts.dirs[k].folder, 0,
                       g_sess_suffix, &n, &d, &nd);
    ok = nd > 0 && d[0].seen && !stricmp(d[0].path, dpath);
    if (ok) {
        /* its own files */
        for (i = keep = 0; i < ksf_alts.n; i++) {
            if (ksf_path_in(ksf_alts.e[i].path, dpath)) {
                sfree(ksf_alts.e[i].id);
                sfree(ksf_alts.e[i].path);
            } else {
                ksf_alts.e[keep++] = ksf_alts.e[i];
            }
        }
        ksf_alts.n = keep;
        ksf_alts_add_files(f, n);
        /* its time; the subfolders it no longer has, and all below them */
        for (i = 0; i < ksf_alts.ndirs; i++) {
            struct ksp_store_dir *r = &ksf_alts.dirs[i];
            bool gone;
            if (!stricmp(r->path, dpath)) {
                r->mtime = d[0].mtime;
                r->seen = d[0].seen;
                continue;
            }
            if (!ksf_path_in(r->path, dpath))
                continue;
            gone = true;
            for (j = 1; j < nd && gone; j++)
                if (!stricmp(d[j].path, r->path))
                    gone = false;
            if (gone) {
                char *sub = dupstr(r->path);
                int e2;
                for (j = keep = 0; j < ksf_alts.n; j++) {
                    if (ksf_path_below(ksf_alts.e[j].path, sub)) {
                        sfree(ksf_alts.e[j].id);
                        sfree(ksf_alts.e[j].path);
                    } else {
                        ksf_alts.e[keep++] = ksf_alts.e[j];
                    }
                }
                ksf_alts.n = keep;
                for (j = e2 = 0; j < ksf_alts.ndirs; j++) {
                    struct ksp_store_dir *q = &ksf_alts.dirs[j];
                    if (!stricmp(q->path, sub) || ksf_path_below(q->path, sub)) {
                        sfree(q->folder);
                        sfree(q->path);
                    } else {
                        ksf_alts.dirs[e2++] = *q;
                    }
                }
                ksf_alts.ndirs = e2;
                sfree(sub);
                i = -1;                 /* the array moved: from the start */
            }
        }
        /* the subfolders new in it, walked whole */
        for (j = 1; j < nd; j++) {
            bool known = false;
            for (i = 0; i < ksf_alts.ndirs && !known; i++)
                if (!stricmp(ksf_alts.dirs[i].path, d[j].path))
                    known = true;
            if (!known) {
                struct ksp_store_file *f2;
                struct ksp_store_dir *d2;
                int n2, nd2;
                f2 = ksp_walk_names(g_sess_dir, d[j].path, d[j].folder, 1,
                                    g_sess_suffix, &n2, &d2, &nd2);
                ksf_alts_add_files(f2, n2);
                ksf_alts_add_dirs(d2, nd2);
                ksp_walk_store_free(f2, n2);
                ksp_walk_dirs_free(d2, nd2);
            }
        }
        ksf_alts_sort();
        ksf_alts.sh_judged = 0;
    }
    ksp_walk_store_free(f, n);
    ksp_walk_dirs_free(d, nd);
    sfree(dpath);
    return ok;
}

/* Under the lock: is the folder's time still what was read (and was it old
 * enough then to be trusted)? 0 yes, 1 changed, 2 gone. */
static int ksf_alts_dir_state(const struct ksp_store_dir *d)
{
    WIN32_FILE_ATTRIBUTE_DATA ad;
    unsigned long long mt;
    if (!GetFileAttributesExA(d->path, GetFileExInfoStandard, &ad) ||
        !(ad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return 2;
    mt = ((unsigned long long)ad.ftLastWriteTime.dwHighDateTime << 32) |
        ad.ftLastWriteTime.dwLowDateTime;
    if (mt != d->mtime || d->mtime + KSF_ALTS_SETTLE > d->seen)
        return 1;
    return 0;
}

/* Under the lock: is the folder one `norm` (a normalised name) could sit in
 * - the session directory, or a folder of the name? */
static bool ksf_alts_dir_matters(const struct ksp_store_dir *d, const char *norm)
{
    size_t fl = strlen(d->folder);
    return !fl || (!strnicmp(norm, d->folder, fl) && norm[fl] == '\\');
}

/* Under the lock: the table made good for `norm` - built when it is not, or
 * is of another store; the folders that matter and changed listed again,
 * the session directory's change or a folder gone rebuilding it. */
static void ksf_alts_check(const char *norm)
{
    char **stale = NULL;
    int nstale = 0, i, k;
    size_t astale = 0;
    bool rebuild = !ksf_alts.built || stricmp(ksf_alts.root, g_sess_dir) ||
        strcmp(ksf_alts.suffix, g_sess_suffix);
    for (i = 0; !rebuild && i < ksf_alts.ndirs; i++) {
        const struct ksp_store_dir *d = &ksf_alts.dirs[i];
        int st;
        if (!ksf_alts_dir_matters(d, norm))
            continue;
        st = ksf_alts_dir_state(d);
        if (st == 2) {
            rebuild = true;
        } else if (st == 1) {
            sgrowarray(stale, astale, nstale);
            stale[nstale++] = dupstr(d->path);
        }
    }
    if (!rebuild) {
        bool root = false;
        for (i = 0; i < ksf_alts.ndirs && !root; i++)
            root = !ksf_alts.dirs[i].folder[0];
        rebuild = !root;                /* the session directory not walked */
    }
    /* parents first: the list is in walk order, a folder before its own */
    for (i = 0; !rebuild && i < nstale; i++) {
        for (k = 0; k < ksf_alts.ndirs; k++)
            if (!stricmp(ksf_alts.dirs[k].path, stale[i]))
                break;
        if (k < ksf_alts.ndirs && !ksf_alts_relist(k))
            rebuild = true;
    }
    if (rebuild)
        ksf_alts_rebuild();
    for (i = 0; i < nstale; i++)
        sfree(stale[i]);
    sfree(stale);
}

/* Under the lock: has nothing changed in any folder of the store? */
static bool ksf_alts_unchanged(void)
{
    int i;
    bool root = false;
    if (!ksf_alts.built || stricmp(ksf_alts.root, g_sess_dir) ||
        strcmp(ksf_alts.suffix, g_sess_suffix))
        return false;
    for (i = 0; i < ksf_alts.ndirs; i++) {
        if (!ksf_alts.dirs[i].folder[0])
            root = true;
        if (ksf_alts_dir_state(&ksf_alts.dirs[i]))
            return false;
    }
    return root;
}

char *ksf_session_inplace_path(const char *sessionname)
{
    char *norm, *found = NULL, **cand = NULL;
    int ncand = 0, i, lo, hi;
    if (!g_sess_dir[0] || !sessionname || !*sessionname)
        return NULL;
    norm = ksp_normalise(sessionname);
    if (!norm[0]) {
        sfree(norm);
        return NULL;
    }
    ksf_alts_lock();
    ksf_alts_check(norm);
    /* the first entry of the name, then each of it in reach order */
    lo = 0;
    hi = ksf_alts.n;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (stricmp(ksf_alts.e[mid].id, norm) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    for (i = lo; i < ksf_alts.n && !stricmp(ksf_alts.e[i].id, norm); i++)
        ncand++;
    if (ncand) {
        cand = snewn(ncand, char *);
        for (i = 0; i < ncand; i++)
            cand[i] = dupstr(ksf_alts.e[lo + i].path);
    }
    ksf_alts_unlock();
    for (i = 0; i < ncand; i++) {
        if (!found && ksp_file_is_session(cand[i]))
            found = cand[i];
        else
            sfree(cand[i]);
    }
    sfree(cand);
    sfree(norm);
    return found;
}

/* The red rows of the list for files no name reaches: each file the walk
 * shadowed (another file of its name is reached, ksp_reach_cmp), unless the
 * lookup reaches it after all. From the last full walk while that still holds
 * for the whole store, else from a new one. */
struct ksp_bad_file *ksf_enum_twins(int *count)
{
    struct ksp_store_file *sh = NULL;
    struct ksp_bad_file *out = NULL;
    int nsh = 0, nout = 0, i;
    size_t aout = 0;
    size_t base = strlen(g_sess_dir) + 1;
    *count = 0;
    if (!g_sess_dir[0])
        return NULL;
    ksf_alts_lock();
    if (!ksf_alts.sh_judged || !ksf_alts_unchanged()) {
        struct ksp_store_file *v;
        int n;
        ksf_alts_unlock();
        v = ksf_walk_full(&n);
        ksp_walk_store_free(v, n);
        ksf_alts_lock();
    }
    if (ksf_alts.nsh) {
        nsh = ksf_alts.nsh;
        sh = snewn(nsh, struct ksp_store_file);
        for (i = 0; i < nsh; i++) {
            sh[i] = ksf_alts.sh[i];
            sh[i].id = dupstr(ksf_alts.sh[i].id);
            sh[i].path = dupstr(ksf_alts.sh[i].path);
        }
    }
    ksf_alts_unlock();
    for (i = 0; i < nsh; i++) {
        char *reach = ksf_session_find(sh[i].id);
        bool twin = !reach || stricmp(reach, sh[i].path);
        sfree(reach);
        if (twin) {
            const char *rel = sh[i].path + base;
            const char *slash = strrchr(sh[i].id, '\\');
            const char *fname = strrchr(sh[i].path, '\\');
            int wn;
            sgrowarray(out, aout, nout);
            /* listed at the level of the folder it sits in; a flat file at
             * the top (a whole path in one name) at the top */
            out[nout].folder = (strchr(rel, '\\') && slash) ?
                dupprintf("%.*s", (int)(slash - sh[i].id), sh[i].id) : dupstr("");
            /* its file name as on disk, the ending kept: never mistaken
             * for the session name beside it ("both" and "both.ktx") */
            out[nout].shown = dupstr(fname ? fname + 1 : sh[i].path);
            wn = MultiByteToWideChar(CP_ACP, 0, sh[i].path, -1, NULL, 0);
            out[nout].wpath = snewn(wn > 0 ? wn : 1, wchar_t);
            if (wn <= 0 ||
                !MultiByteToWideChar(CP_ACP, 0, sh[i].path, -1, out[nout].wpath, wn))
                out[nout].wpath[0] = L'\0';
            out[nout].isdir = 0;
            out[nout].twin = 1;
            nout++;
        }
    }
    ksp_walk_store_free(sh, nsh);
    *count = nout;
    return out;
}

/* The file the session is read from: the legacy one while it exists, else the
 * target, else a file of another escape (ksf_session_inplace_path). NULL when
 * none exists. */
char *ksf_session_find(const char *sessionname)
{
    char *p = ksf_session_legacy_path(sessionname);
    if (p)
        return p;
    p = ksf_session_target_path(sessionname);
    if (ksf_exists_file(p))
        return p;
    sfree(p);
    return ksf_session_inplace_path(sessionname);
}

/* The files besides the reached one that decode to this session name (the
 * red rows of the list), for the terminal's notice when such a session is
 * opened - at every start of a saved session, so it must cost little and
 * never walks the store: only the folders on the name's own path are listed,
 * each entry decoded and compared without case (a session file at the end,
 * every folder that decodes to the component on the way; the flat pre-path
 * file at the top too). A name with one file lists the session directory and
 * one folder per level. */
struct ksf_others {
    char **v;
    int n;
    size_t a;
};
static void ksf_others_scan(const char *dir, char **comps, int ncomp,
                            const char *whole, struct ksf_others *o)
{
    char *pat = dupprintf("%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    sfree(pat);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        int isdir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        char *name, *dec, *full;
        int hit;
        if (fd.cFileName[0] == '.')
            continue;                   /* as the list skips them */
        if (isdir && (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            continue;                   /* not followed, as the list does not */
        name = dupstr(fd.cFileName);
        if (!isdir)
            ksp_strip_suffix(name, g_sess_suffix);
        dec = ksp_component_unmunge(name);
        sfree(name);
        if (isdir)
            hit = ncomp > 1 && !stricmp(dec, comps[0]);
        else
            hit = ncomp == 1 ? !stricmp(dec, comps[0]) :
                (whole && !stricmp(dec, whole));     /* the flat older file */
        sfree(dec);
        if (!hit)
            continue;
        full = dupprintf("%s\\%s", dir, fd.cFileName);
        if (isdir) {
            ksf_others_scan(full, comps + 1, ncomp - 1, NULL, o);
            sfree(full);
        } else if (ksp_file_is_session(full)) {
            sgrowarray(o->v, o->a, o->n);
            o->v[o->n++] = full;
        } else {
            sfree(full);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static const char *ksf_others_id;
static int ksf_others_cmp(const void *av, const void *bv)
{
    return ksp_reach_cmp(ksf_others_id, *(char *const *)av, *(char *const *)bv,
                         g_sess_suffix);
}

char **ksf_session_others(const char *sessionname, char **reached, int *count)
{
    char *found, *norm, *p, **comps = NULL;
    size_t n = 0, cap = 0, base = strlen(g_sess_dir) + 1;
    struct ksf_others o = { NULL, 0, 0 };
    int i, keep;
    *count = 0;
    if (reached)
        *reached = NULL;
    if (!g_sess_dir[0] || !sessionname || !*sessionname)
        return NULL;
    found = ksf_session_find(sessionname);
    if (!found)
        return NULL;
    norm = ksp_normalise(sessionname);
    for (p = norm; *p; ) {
        char *e = strchr(p, '\\');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        sgrowarray(comps, cap, n);
        comps[n++] = dupprintf("%.*s", (int)len, p);
        p += len;
        if (*p == '\\')
            p++;
    }
    if (n > 0 && comps[n - 1][0])
        ksf_others_scan(g_sess_dir, comps, (int)n, n > 1 ? norm : NULL, &o);
    /* the files below the session folder, the opened one left out, in the
     * order the name would reach them */
    for (i = keep = 0; i < o.n; i++) {
        if (stricmp(o.v[i], found)) {
            char *rel = dupstr(o.v[i] + base);
            sfree(o.v[i]);
            o.v[keep++] = rel;
        } else {
            sfree(o.v[i]);
        }
    }
    o.n = keep;
    if (o.n > 1) {
        ksf_others_id = norm;
        qsort(o.v, o.n, sizeof(*o.v), ksf_others_cmp);
        ksf_others_id = NULL;
    }
    if (o.n && reached)
        *reached = dupstr(found + base);
    while (n > 0)
        sfree(comps[--n]);
    sfree(comps);
    sfree(norm);
    sfree(found);
    if (!o.n) {
        sfree(o.v);
        return NULL;
    }
    *count = o.n;
    return o.v;
}

char *ksf_session_path(const char *sessionname)   /* snewn'd or NULL */
{
    /* Existing file if any, else where a save would put it - what the callers
     * outside the backend (window positions, the import's exists check) ask. */
    char *p = ksf_session_find(sessionname);
    return p ? p : ksf_session_target_path(sessionname);
}

/* Create the directories above a session file (mkdir -p below g_sess_dir). */
void ksf_make_parent_dirs(const char *path)
{
    size_t base = strlen(g_sess_dir);
    char *p = dupstr(path), *q;
    CreateDirectoryA(g_sess_dir, NULL);
    if (strnicmp(p, g_sess_dir, base) || p[base] != '\\') {
        sfree(p);
        return;
    }
    for (q = p + base + 1; (q = strchr(q, '\\')) != NULL; q++) {
        *q = '\0';
        CreateDirectoryA(p, NULL);
        *q = '\\';
    }
    sfree(p);
}

/* Remove the directories above a removed session file while they are empty,
 * stopping at g_sess_dir. RemoveDirectory refuses a directory with anything
 * in it, so a folder that still holds a session - or a .gitkeep - stays. */
void ksf_prune_empty_dirs(const char *path)
{
    size_t base = strlen(g_sess_dir);
    char *p = dupstr(path), *bs;
    if (strnicmp(p, g_sess_dir, base) || p[base] != '\\') {
        sfree(p);
        return;
    }
    while ((bs = strrchr(p, '\\')) != NULL && (size_t)(bs - p) > base) {
        *bs = '\0';
        if (!RemoveDirectoryA(p))
            break;
    }
    sfree(p);
}

/* Enumerate the folder store: every session file below g_sess_dir, as path
 * identities. The walk itself - dot-files, links, the depth cap, the suffix,
 * the content check - is ksp_walk_store (kitty_sessionpath.c), which kageant's
 * tray menu walks too. */
#define KSF_ENUM_MAXDEPTH KSP_WALK_MAXDEPTH
/*
 * What the listing costs: one directory walk, plus a read of the files whose
 * verdict is not known yet. Every file is judged by its content, the
 * configured suffix included (a note named like a session is not one); a
 * file is read only until a HostName or Protocol line turns up - and KiTTY++
 * writes those near the top. The verdict cache (ksp_file_verdict) keeps each
 * file's verdict against the size and time the walk reports, so a listing
 * reads again only the files that are new or changed. `leaf` (NULL = all)
 * narrows the walk to the sessions of that name: the bare-name lookup behind
 * -load, a jump host or a host argument then opens at most the files that
 * share the name, never the whole store.
 */
static char **ksf_enum_names(const char *leaf, int *count)
{
    struct ksp_store_file *v;
    char **names = NULL;
    int i, n = 0;
    *count = 0;
    if (!g_sess_dir[0])
        return NULL;
    /* the whole store also builds the table of files of another escape */
    v = leaf ? ksp_walk_store(g_sess_dir, g_sess_suffix, leaf, &n) :
        ksf_walk_full(&n);
    if (n > 0) {
        names = snewn(n, char *);
        for (i = 0; i < n; i++) {
            names[i] = v[i].id;          /* handed over */
            v[i].id = NULL;
        }
    }
    *count = n;
    ksp_walk_store_free(v, n);
    return names;
}

char **ksf_enum_sessions(int *count)
{
    return ksf_enum_names(NULL, count);
}

char **ksf_enum_sessions_leaf(const char *leaf, int *count)
{
    *count = 0;
    if (!leaf || !*leaf)
        return NULL;
    return ksf_enum_names(leaf, count);
}

/* The session names of the store in use, and a typed name resolved against
 * them (kitty_sessionpath.h). Store-aware, so here with the store rather
 * than beside the pure path rules. */
char **kitty_session_names(int *n)
{
    settings_e *e = enum_settings_start();
    char **names = NULL;
    int count = 0, alloc = 0;
    strbuf *sb = strbuf_new();
    if (e) {
        while (enum_settings_next(e, sb)) {
            if (count >= alloc) {
                alloc = alloc ? alloc * 2 : 32;
                names = sresize(names, alloc, char *);
            }
            names[count++] = dupstr(sb->s);
            strbuf_clear(sb);
        }
        enum_settings_finish(e);
    }
    strbuf_free(sb);
    *n = count;
    return names;
}

void kitty_session_names_free(char **names, int n)
{
    int i;
    for (i = 0; i < n; i++)
        sfree(names[i]);
    sfree(names);
}

int kitty_session_resolve(const char *wanted, char **resolved, char **errtext,
                          int with_load_hint)
{
    char **names;
    int n, nm = 0, kind, *idx;
    settings_r *r;

    if (resolved)
        *resolved = NULL;
    if (errtext)
        *errtext = NULL;
    if (!wanted || !*wanted)
        return KSP_NONE;

    /* The exact name first: it costs one open, and it is the answer for every
     * root session and every unarranged one - the store is case-insensitive in
     * both backends, so this is the case-insensitive exact match too. */
    r = open_settings_r(wanted);
    if (r) {
        close_settings_r(r);
        if (resolved)
            *resolved = dupstr(wanted);
        return KSP_EXACT;
    }
    if (strchr(wanted, '\\'))
        return KSP_NONE;

    /* Folder store: only the files that carry this name are looked at (a
     * proxy connection or a host on the command line asks this for every
     * name that is not a session, so a whole-store read here would be paid
     * on every connection). The registry lists key names only, which is
     * cheap as it is. */
    if (store_is_file())
        names = ksf_enum_sessions_leaf(wanted, &n);
    else
        names = kitty_session_names(&n);
    idx = snewn(n > 0 ? n : 1, int);
    kind = ksp_lookup(names, n, wanted, idx, n, &nm);
    if ((kind == KSP_EXACT || kind == KSP_UNIQUE) && resolved)
        *resolved = dupstr(names[idx[0]]);
    if (kind == KSP_AMBIGUOUS && errtext) {
        char **list = snewn(nm, char *);
        int i;
        for (i = 0; i < nm; i++)
            list[i] = names[idx[i]];
        *errtext = ksp_ambiguous_text(wanted, list, nm, with_load_hint);
        sfree(list);
    }
    sfree(idx);
    kitty_session_names_free(names, n);
    return kind;
}

/*
 * Change the session file suffix on disk, once: every session file ending in
 * `oldsuf` (or with no suffix, the legacy form) is renamed to end in `newsuf`.
 * A file whose new name already exists is left alone and named in `clashes`
 * (one relative path per line). With dry_run nothing moves and the return is
 * the number of files that would be renamed. Returns the number renamed.
 */
static int ksf_suffix_walk(const char *dir, const char *rel, int depth,
                           const char *oldsuf, const char *newsuf,
                           int dry_run, strbuf *clashes)
{
    char *pat = dupprintf("%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE hf = FindFirstFileA(pat, &fd);
    int n = 0;
    sfree(pat);
    if (hf == INVALID_HANDLE_VALUE)
        return 0;
    do {
        char *full;
        if (fd.cFileName[0] == '.' ||
            ((fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
             (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)))
            continue;
        full = dupprintf("%s\\%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth < KSF_ENUM_MAXDEPTH) {
                char *subrel = dupcat(rel, fd.cFileName, "\\");
                n += ksf_suffix_walk(full, subrel, depth + 1, oldsuf, newsuf,
                                     dry_run, clashes);
                sfree(subrel);
            }
        } else if (ksp_file_is_session(full)) {
            char *base = dupstr(fd.cFileName);
            char *to;
            int had_old = ksp_strip_suffix(base, oldsuf);
            to = dupprintf("%s\\%s%s", dir, base, newsuf ? newsuf : "");
            /* A file already ending in the new suffix is listed under the
             * stripped name already: appending it a second time would rename
             * the session, not migrate it. */
            if (!had_old && newsuf && *newsuf) {
                char *chk = dupstr(fd.cFileName);
                if (ksp_strip_suffix(chk, newsuf)) {
                    sfree(to);
                    to = dupstr(full);
                }
                sfree(chk);
            }
            if (stricmp(to, full)) {
                if (ksf_exists_file(to)) {
                    if (clashes) {
                        if (clashes->len)
                            put_byte(clashes, '\n');
                        put_fmt(clashes, "%s%s", rel, fd.cFileName);
                    }
                } else if (dry_run) {
                    n++;
                } else if (MoveFileA(full, to)) {
                    n++;
                }
            }
            sfree(to);
            sfree(base);
        }
        sfree(full);
    } while (FindNextFileA(hf, &fd));
    FindClose(hf);
    return n;
}

int kitty_session_suffix_rename(const char *oldsuf, const char *newsuf,
                                int dry_run, strbuf *clashes)
{
    int n;
    if (!store_is_file())
        return 0;
    n = ksf_suffix_walk(g_sess_dir, "", 0, oldsuf ? oldsuf : "",
                        newsuf ? newsuf : "", dry_run, clashes);
    if (n && !dry_run)
        kitty_store_mark_dirty();
    return n;
}
/* A cyd01-syntax file was written by old (<=0.76) KiTTY, whose portable saves
 * stored "Password" bcrypt-encrypted, same scheme as its old registry hive
 * (old KiTTY never encrypted ProxyPassword). Decode it as part of the format
 * conversion, so everything downstream - reads, the migration-consent gate,
 * the next save - sees the plaintext-unmarked semantics of our own format. If
 * it does not decode, the stored bytes stay untouched (never-lose); crucially,
 * decoding must happen HERE because once the file is rewritten in our syntax
 * the "this value is legacy-encrypted" context is gone for good. */
static void ksf_convert_legacy_password(struct ksf_item **head)
{
    const char *pw = ksf_list_get(*head, KR_PASSWORD);
    if (!pw || !pw[0]) return;
    char *pt = ksec_legacy_decrypt_hostterm(pw,
        ksf_list_get(*head, "HostName"), ksf_list_get(*head, "TerminalType"));
    if (pt) {
        ksf_list_set(head, KR_PASSWORD, pt);
        memset(pt, 0, strlen(pt));
        free(pt);
    }
}

/* Read one line of any length, newline stripped; NULL at end of file. Caller
 * frees. This used to be a fixed 8 KB buffer, and a setting longer than it was
 * silently cut: the head parsed as a truncated value and the tail, having no
 * delimiter, was dropped. PortForwardings is a single value holding the WHOLE
 * tunnel list - a /24 of ssh+rdp+vnc is ~19 KB - so a large Tunnels list came
 * back partial from a portable store, which to the user is the list being
 * empty (cf. cyd01/KiTTY#541). Nothing else caps a setting's length; the
 * registry backend stores and returns the same value whole. */
static char *ksf_read_line(FILE *fp)
{
    size_t cap = 512, len = 0;
    char *buf = snewn(cap, char);
    int c = EOF;
    while ((c = fgetc(fp)) != EOF && c != '\n') {
        if (len + 2 > cap) {
            cap *= 2;
            buf = sresize(buf, cap, char);
        }
        buf[len++] = (char)c;
    }
    if (c == EOF && len == 0) {
        sfree(buf);
        return NULL;
    }
    while (len && (buf[len-1] == '\r' || buf[len-1] == '\n'))
        len--;
    buf[len] = '\0';
    return buf;
}

struct ksf_item *ksf_load(const char *path)       /* parsed list (may be NULL) */
{
    FILE *fp = fopen(path, "rb");
    struct ksf_item *head = NULL;
    char *line;
    int cyd01 = 0;
    if (!fp) return NULL;
    while ((line = ksf_read_line(fp)) != NULL) {
        char *eq, *bs, *val;
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
            sfree(line);
            continue;   /* no delimiter -> not a setting line */
        }
        ksf_list_set(&head, line, val ? val : "");
        if (val) sfree(val);
        sfree(line);
    }
    fclose(fp);
    if (cyd01)
        ksf_convert_legacy_password(&head);
    return head;
}
/* Returns true only when the whole file was written and closed: the caller
 * removes an older copy of the session on that, and on nothing less. */
bool ksf_save(const char *path, struct ksf_item *h)
{
    FILE *fp;
    bool ok;
    CreateDirectoryA(g_sess_dir, NULL);   /* harmless if it already exists */
    fp = fopen(path, "wb");
    if (!fp) return false;
    for (; h; h = h->next) {
        char *mv = ksf_munge(h->val ? h->val : "");
        fprintf(fp, "%s=%s\n", h->key, mv ? mv : "");
        if (mv) sfree(mv);
    }
    ok = !ferror(fp);
    if (fclose(fp) != 0)
        ok = false;
    ksp_verdict_forget(path);   /* judged afresh at the next listing */
    return ok;
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

/* KiTTY: the host-key folder of the folder store, and an ending on host-key
 * file names - kitty.ini [KiTTY] sshhostkeys / keysuffix, set at startup. An
 * empty folder means the SshHostKeys folder beside Sessions, as before. */
static char g_hostkey_dir[1024] = "";
static char g_hostkey_suffix[64] = "";
void kitty_set_hostkey_dir(const char *dir)
{
    g_hostkey_dir[0] = '\0';
    if (dir && *dir) {
        strncpy(g_hostkey_dir, dir, sizeof(g_hostkey_dir) - 1);
        g_hostkey_dir[sizeof(g_hostkey_dir) - 1] = '\0';
        while (g_hostkey_dir[0] &&
               g_hostkey_dir[strlen(g_hostkey_dir) - 1] == '\\')
            g_hostkey_dir[strlen(g_hostkey_dir) - 1] = '\0';
    }
}
const char *kitty_hostkey_dir(void) { return g_hostkey_dir; }
void kitty_set_hostkey_suffix(const char *suffix)
{
    const char *p;
    g_hostkey_suffix[0] = '\0';
    if (!suffix)
        return;
    for (p = suffix; *p; p++)
        if ((unsigned char)*p < 0x20 || strchr("\\/:*?\"<>|", *p))
            return;
    strncpy(g_hostkey_suffix, suffix, sizeof(g_hostkey_suffix) - 1);
    g_hostkey_suffix[sizeof(g_hostkey_suffix) - 1] = '\0';
}
const char *kitty_hostkey_suffix(void) { return g_hostkey_suffix; }

char *portable_subdir_path(const char *subdir)     /* snewn'd */
{
    char *root;
    if (g_hostkey_dir[0] && !strcmp(subdir, "SshHostKeys") && store_is_file())
        return dupstr(g_hostkey_dir);
    root = portable_root_dir();
    char *path = root ? dupprintf("%s\\%s", root, subdir) : NULL;
    sfree(root);
    return path;
}

char *portable_item_path(const char *subdir, const char *name)
{
    char *dir = portable_subdir_path(subdir);
    char *m = ksf_munge(name ? name : "");
    /* KiTTY: keysuffix ends every host-key file name (never the folder). */
    const char *suf = (name && *name && !strcmp(subdir, "SshHostKeys")) ?
        g_hostkey_suffix : "";
    char *path = (dir && m) ? dupprintf("%s\\%s%s", dir, m, suf) : NULL;
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
    /* KiTTY (hknet/KiTTY#56): a host-key file without the ending in force
     * (one written before hostkeyextension was set, or copied in later) is
     * renamed to carry it and read; a file WITH the ending always wins. */
    if (!fp && g_hostkey_suffix[0] && name && *name &&
        !strcmp(subdir, "SshHostKeys")) {
        size_t sl = strlen(g_hostkey_suffix), pl = strlen(path);
        if (pl > sl) {
            char *plain = dupstr(path);
            plain[pl - sl] = '\0';
            if (GetFileAttributesA(plain) != INVALID_FILE_ATTRIBUTES)
                fp = fopen(MoveFileA(plain, path) ? path : plain, "rb");
            sfree(plain);
        }
    }
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
