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
