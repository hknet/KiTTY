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
#include "../kitty/kitty_storage.h"  /* the KiTTY half of this file (registry root, portable store, at-rest crypto) */
#include "../kitty/kitty_secretstore.h"
#include "../kitty/kitty_text.h"     /* KT_SESSION_LOCKED_FMT */
#include "../kitty/kitty_pwmem.h"    /* passwords wrapped in memory: unwrap before protecting at rest */
#include "../kitty/kitty_text.h"     /* KT_SP_SAVE_CLASH: the folder store's save clash */
#include "../kitty/kitty_inifile.h"  /* kitty_store_readonly: readonly=yes */

#include <shlobj.h>
#ifndef CSIDL_APPDATA
#define CSIDL_APPDATA 0x001a
#endif
#ifndef CSIDL_LOCAL_APPDATA
#define CSIDL_LOCAL_APPDATA 0x001c
#endif

/*
 * KiTTY: the runtime registry root and the portable file backend live in
 * kitty/kitty_storage.c, the at-rest credential crypto in
 * kitty/kitty_secretstore.c (both split out of this file to shrink its
 * divergence from upstream). The macros below
 * repoint upstream's registry-path identifiers at that file's runtime
 * accessors, so the function bodies in this file stay textually unchanged.
 */
#define reg_base_buf     (kitty_registry_base())
#define puttystr         (kitty_reg_sessions())
#define host_ca_key      (kitty_reg_hostcas())
#define reg_hostkeys_buf (kitty_reg_hostkeys())
#define reg_jumplist_key (kitty_reg_jumplist())
#define g_sess_dir       (kitty_session_dir())
static const char *const reg_jumplist_value = "Recent sessions";


static bool tried_shgetfolderpath = false;
static HMODULE shell32_module = NULL;
DECL_WINDOWS_FUNCTION(static, HRESULT, SHGetFolderPathA,
                      (HWND, int, HANDLE, DWORD, LPSTR));


struct settings_w {
    HKEY sesskey;
    int is_file;
    char *fpath;
    struct ksf_item *items;
    int mig_answer;   /* legacy->protected consent for THIS save: -1 unasked */
    char *legacy;     /* KiTTY: older file of this session, removed once saved */
    int raw_secrets;  /* KiTTY: .ktx export - passwords are the caller's (no protection here) */
    /* KiTTY: save only non-default settings - Default Settings' stored
     * values (defs: folder store, defkey: registry) */
    int nd;
    struct ksf_item *defs;
    HKEY defkey;
    char *sessname;   /* KiTTY: registry - the session, for a refused save's report */
};

settings_w *open_settings_w(const char *sessionname, char **errmsg)
{
    *errmsg = NULL;

    /* KiTTY: readonly=yes - no session is written. NULL with no message:
     * an automatic save stays silent, a caller that shows a refusal checks
     * kitty_store_readonly() (kitty_inifile.h). */
    if (kitty_store_readonly())
        return NULL;

    if (!sessionname || !*sessionname)
        sessionname = KITTY_DEFAULT_SESSION;

    if (store_is_file()) {
        settings_w *handle = snew(settings_w);
        handle->raw_secrets = 0;
        handle->sesskey = NULL;
        handle->is_file = 1;
        handle->items = NULL;
        handle->mig_answer = -1;
        handle->nd = 0;
        handle->defs = NULL;
        handle->defkey = NULL;
        handle->sessname = NULL;
        /* KiTTY (hknet/KiTTY#55, #56): the session is written to its path
         * with the suffix; an older file of it (no suffix, or the flat
         * pre-path name) is carried over and removed once the save is done.
         * When BOTH exist they are two sessions sharing one name, and the
         * save is refused rather than overwrite one of them. */
        handle->fpath = ksf_session_target_path(sessionname);
        handle->legacy = ksf_session_legacy_path(sessionname);
        /* KiTTY (hknet/KiTTY#59): neither the file under today's name nor an
         * older layout, but one of another escape the list shows as this
         * session (old KiTTY's "Web%20Servers\srv%20(web).ktx"): saved IN
         * PLACE, under its own name. */
        if (!handle->legacy) {
            DWORD ta = GetFileAttributesA(handle->fpath);
            if (ta == INVALID_FILE_ATTRIBUTES) {
                char *inplace = ksf_session_inplace_path(sessionname);
                if (inplace) {
                    sfree(handle->fpath);
                    handle->fpath = inplace;
                    handle->items = ksf_load(handle->fpath);
                    return handle;
                }
            }
        }
        if (handle->legacy) {
            DWORD a = GetFileAttributesA(handle->fpath);
            if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) {
                const char *rel = handle->fpath + strlen(g_sess_dir);
                while (*rel == '\\')
                    rel++;
                *errmsg = dupprintf(KT_SP_SAVE_CLASH, rel);
                sfree(handle->fpath);
                sfree(handle->legacy);
                sfree(handle);
                return NULL;
            }
        }
        /* KiTTY: a folder where the file goes, or a session file where one of
         * its folders goes (no suffix: "Linux" the session and "Linux" the
         * folder are one name on disk). Refused with a reason - the write
         * would otherwise fail without a word. */
        {
            char *blk = ksf_path_blocker(sessionname);
            if (blk) {
                *errmsg = dupprintf(KT_SP_PATH_BLOCKED, blk);
                sfree(blk);
                sfree(handle->fpath);
                sfree(handle->legacy);
                sfree(handle);
                return NULL;
            }
        }
        /* Pre-load any existing file so a save overwrites/adds in place and never
         * drops keys it didn't rewrite (matches the registry open-then-overwrite
         * semantics; the full conf is normally rewritten on each save anyway). */
        handle->items = ksf_load(handle->legacy ? handle->legacy : handle->fpath);
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
    handle->raw_secrets = 0;
    handle->sesskey = sesskey;
    handle->is_file = 0;
    handle->fpath = NULL;
    handle->items = NULL;
    handle->mig_answer = -1;
    handle->legacy = NULL;
    handle->nd = 0;
    handle->defs = NULL;
    handle->defkey = NULL;
    handle->sessname = dupstr(sessionname);
    return handle;
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

/* KiTTY: save only non-default settings. Folder and SessionLockdown belong
 * to the session, not to its settings; HostName and Protocol mark a file
 * as a session (ksp_file_is_session): written as always. */
static int kitty_nd_exempt(const char *key)
{
    return !strcmp(key, "Folder") || !strcmp(key, "SessionLockdown") ||
        !strcmp(key, "HostName") || !strcmp(key, "Protocol");
}

/* The write of key under handle->nd: 1 done here, 0 written as always. The
 * default is Default Settings' stored value; a key it lacks has none. The
 * folder store keeps every line (ksf_list_set_nondefault); the registry has
 * no comments: a value set back to the default is deleted, a default never
 * written. only_default: a value other than the default is left to the
 * caller. */
static int kitty_nd_put(settings_w *handle, const char *key, const char *value,
                        int is_int, int ival, int only_default)
{
    if (!handle->nd || kitty_nd_exempt(key))
        return 0;
    if (handle->is_file) {
        const char *def = ksf_list_get(handle->defs, key);
        if (!def || (only_default && strcmp(value, def)))
            return 0;
        ksf_list_set_nondefault(&handle->items, key, value, def);
        return 1;
    }
    if (is_int) {
        DWORD d, cur;
        if (!get_reg_dword(handle->defkey, key, &d))
            return 0;
        if (get_reg_dword(handle->sesskey, key, &cur) && (int)cur == ival)
            return 1;                   /* unchanged */
        if ((int)d != ival)
            return 0;
    } else {
        char *d = get_reg_sz(handle->defkey, key), *cur;
        int same, isdef;
        if (!d)
            return 0;
        cur = get_reg_sz(handle->sesskey, key);
        same = cur && !strcmp(cur, value);
        isdef = !strcmp(value, d);
        sfree(d);
        sfree(cur);
        if (same)
            return 1;
        if (!isdef)
            return 0;
    }
    RegDeleteValueA(handle->sesskey, key);
    return 1;
}

void write_setting_s(settings_w *handle, const char *key, const char *value)
{
    if (!handle)
        return;
    int slot = kitty_secret_slot(key);
    if (slot >= 0 && handle->raw_secrets)
        return;                        /* the .ktx writer sets them itself */
    if (slot >= 0) {
        /* KiTTY: what the caller hands over is the wrapped in-memory form
         * (kitty_pwmem.c). Unwrap it here - this is the one place every writer
         * to the store passes through - so the at-rest protection below sees
         * the plaintext it expects. An unwrapped buffer exists only for the
         * length of this call. */
        char plain[KITTY_PW_MAX + 1];
        plain[0] = '\0';
        if (kitty_pw_is_wrapped(value)) {
            /* An unreadable blob unwraps to "", and the never-wipe guard
             * below then keeps whatever is already stored - the same answer
             * it gives for a value that failed to decrypt on load. */
            kitty_pw_unwrap_str(value, plain, sizeof(plain));
            value = plain;
        }
        /* Never-wipe: a blob that failed to decrypt this session leaves the
         * in-memory value ""; re-persist the original verbatim instead of
         * clobbering it. Otherwise encrypt the plaintext at rest. */
        const char *keep = (value && value[0]) ? NULL : ksec_orig_get(slot);
        if (keep) {
            ksf_or_reg_put(handle, key, keep);
        } else {
            /* Save only non-default settings: an empty password is the
             * default where Default Settings stores an empty one. */
            if (handle->nd && !(value && value[0]) &&
                kitty_nd_put(handle, key, "", 0, 0, 1)) {
                smemclr(plain, sizeof(plain));
                return;
            }
            /* Portable saves need consent before converting an old-format
             * (unmarked) stored password to a protected one; declining keeps
             * the pre-loaded stored value in handle->items untouched. */
            if (handle->is_file && value && value[0] &&
                !kitty_portable_password_legacy() &&
                ksec_stored_is_legacy(ksf_list_get(handle->items, key))) {
                if (handle->mig_answer < 0)
                    handle->mig_answer = ksec_migrate_warn_ask();
                if (handle->mig_answer == 0) {
                    smemclr(plain, sizeof(plain));
                    return;
                }
            }
            /* The same password as the one stored, in the form this save
             * would write: the stored value stays as it is. Wrapping it again
             * draws a fresh salt, and the line would change on every save. */
            if (value && value[0]) {
                char *stored = handle->is_file
                    ? dupstr(ksf_list_get(handle->items, key))
                    : get_reg_sz(handle->sesskey, key);
                int keeps = ksec_stored_keeps(stored, value, handle->is_file);
                if (keeps)
                    ksf_or_reg_put(handle, key, stored);
                if (stored) { smemclr(stored, strlen(stored)); sfree(stored); }
                if (keeps) {
                    smemclr(plain, sizeof(plain));
                    return;
                }
            }
            int locked = 0;
            char *blob = handle->is_file
                ? ksec_protect_portable_kept(value ? value : "", &locked)
                : ksec_protect_registry_kept(value ? value : "", &locked);
            if (locked) {
                /* KiTTY: the wrap was refused - the master password is
                 * locked (cancelled, wrong, not set up) or DPAPI failed:
                 * what is stored stays - the pre-loaded line or registry
                 * value, or nothing for a key that had none - never DPAPI
                 * or the plaintext in its place, and the user learns it */
                const char *rel = handle->fpath ? handle->fpath
                                : handle->sessname ? handle->sessname : "";
                size_t dl = strlen(g_sess_dir);
                if (!_strnicmp(rel, g_sess_dir, dl))
                    rel += dl;
                while (*rel == '\\')
                    rel++;
                ksec_report_locked_save(KSEC_KEPT_SESSION, rel);
                smemclr(plain, sizeof(plain));
                return;
            }
            ksf_or_reg_put(handle, key, blob ? blob : "");
            if (blob) { memset(blob, 0, strlen(blob)); free(blob); }
        }
        smemclr(plain, sizeof(plain));
        return;
    }
    if (!kitty_nd_put(handle, key, value ? value : "", 0, 0, 0))
        ksf_or_reg_put(handle, key, value);
}

void write_setting_i(settings_w *handle, const char *key, int value)
{
    char buf[32];
    if (!handle)
        return;
    sprintf(buf, "%d", value);
    if (kitty_nd_put(handle, key, buf, 1, value, 0))
        return;
    if (handle->is_file)
        ksf_list_set(&handle->items, key, buf);
    else
        put_reg_dword(handle->sesskey, key, value);
}

/* KiTTY: settings that have been RENAMED or replaced. The new name is written
 * by the ordinary save; the old one is removed there, so a session stops
 * carrying both. Where `now` names a setting AND the value still means the same
 * thing, the old name is READ as a fallback (kitty_setting_legacy_name below) -
 * that is what makes the removal safe: a session written by an older KiTTY, or
 * by classic KiTTY, keeps its value, and migrates the moment it is saved.
 *
 * A replacement whose value does NOT mean the same thing has `migrates` false:
 * the old key is still dropped on save, but nothing reads it, and the session
 * importer names it as a value it could not carry over.
 *
 * Retiring is deliberately NOT done while LOADING a session: a read path that
 * writes fails on read-only media or a locked-down hive, and would fire for
 * "Default Settings" and for plink/pscp merely loading a session in passing. */
const struct kitty_retired_key kitty_retired_keys[] = {
    /* A rename only: it pins a position, it saves none. An out-of-reach or
     * unset coordinate is already handled where the pin is applied
     * (kitty_apply_window_pos: a pin needs x >= 0 and y >= 0, and is clamped
     * onto the nearest monitor), so carrying the flag across cannot put a
     * window where its owner cannot get at it. */
    { "SaveWindowPos", "SetWindowPos", true },
    /* A REPLACEMENT, not a rename. The old BOOL meant "warn before syncing",
     * so its default of false meant "sync silently", and honouring it would
     * quietly switch remote clipboard writes on for every session imported
     * from classic KiTTY - see conf.h. */
    { "OSC52WarnBeforeClipboardSync", "OSC52Clipboard", false,
      "off there meant hosts could write to your clipboard without asking" },
    /* Whether KiTTY looks for a new release is a property of the installation,
     * not of a connection, so it is [KiTTY] checkupdate in kitty.ini now. There
     * is nothing sensible to migrate FROM: each session carried its own answer
     * and the first session opened won. */
    { "CheckUpdateStartup", "kitty.ini [KiTTY] checkupdate", false,
      "it belongs to the installation now, not to a session" },
    /* Classic KiTTY's name for the RuTTY script file; the value means the
     * same, so it is read under the old name and migrates on save. The .ktx
     * loader had this fallback by hand; the folder import showed the ordinary
     * loader lacked it. */
    { "ScriptFileName", "Scriptfile", true },
    /* Classic wrote this first in every session file as a "this is a session"
     * marker (value 1). It is not a setting; nothing reads it here, and an
     * empty `now` tells the importers not to report it either. */
    { "Present", "", false, "a marker classic KiTTY wrote into every session file, not a setting" },
    /* TuTTY's single switch for the window's caption buttons. Not ported as
     * such: the Window buttons group on Window > Behaviour has one option per
     * button instead, and the old value's meaning cannot be mapped onto them
     * one to one. */
    { "DisableBottomButtons", "Window buttons (Window > Behaviour)", false,
      "replaced by the four Window-buttons options, which it does not map onto" },
    /* Classic KiTTY's SECOND free-text field per session, written by the
     * send-text box's Shift+F2 / Shift+F3 and shown in a modal box when the
     * session opened. There is one note field now, the Comment, so the value
     * still means what it meant: it is read under the old name and dropped
     * here on the next save. The carry-over is richer than a rename - an
     * existing Comment keeps its text and the note is appended after a blank
     * line, and the Comment panel's "Notify the user at login" is switched on
     * so the note is still shown - so the merge itself lives in
     * kitty_merge_legacy_note() (kitty/kitty_storage.c); `migrates` here gets
     * the value across when a session has no Comment at all, and tells the
     * session importers that nothing was lost. */
    { "Notes", "Comment", true },
};

const struct kitty_retired_key *kitty_retired_key_table(size_t *n)
{
    *n = lenof(kitty_retired_keys);
    return kitty_retired_keys;
}

/* The name a setting used to have, if reading it under that name still means
 * what it means now; NULL otherwise. */
static const char *kitty_setting_legacy_name(const char *key)
{
    size_t i;
    for (i = 0; i < lenof(kitty_retired_keys); i++)
        if (kitty_retired_keys[i].migrates &&
            !strcmp(kitty_retired_keys[i].now, key))
            return kitty_retired_keys[i].was;
    return NULL;
}

static void kitty_retire_renamed_keys(settings_w *handle)
{
    size_t i;
    for (i = 0; i < lenof(kitty_retired_keys); i++) {
        if (handle->is_file)
            ksf_list_del(&handle->items, kitty_retired_keys[i].was);
        else if (handle->sesskey)
            RegDeleteValueA(handle->sesskey, kitty_retired_keys[i].was);
    }
}

void close_settings_w(settings_w *handle)
{
    if (!handle)
        return;
    /* A session was written - the store differs from the last backup. */
    kitty_store_mark_dirty();
    kitty_retire_renamed_keys(handle);
    if (handle->is_file) {
        if (handle->fpath) {
            bool saved;
            DWORD a;
            ksf_make_parent_dirs(handle->fpath);   /* KiTTY: the folder path */
            saved = ksf_save(handle->fpath, handle->items);
            /* KiTTY: the session now lives at its path with the suffix; the
             * older file goes, and so does its folder if that left it empty -
             * but only once the new file is written whole and is a file. */
            a = GetFileAttributesA(handle->fpath);
            if (handle->legacy && saved && a != INVALID_FILE_ATTRIBUTES &&
                !(a & FILE_ATTRIBUTE_DIRECTORY)) {
                DeleteFileA(handle->legacy);
                ksf_prune_empty_dirs(handle->legacy);
            }
            sfree(handle->fpath);
        }
        sfree(handle->legacy);
        ksf_list_free(handle->items);
    } else {
        close_regkey(handle->sesskey);
    }
    sfree(handle->sessname);
    ksf_list_free(handle->defs);
    if (handle->defkey)
        close_regkey(handle->defkey);
    sfree(handle);
}

/* KSEC_HIVE_*: kitty_storage.h, so the importer names the same hives. */
struct settings_r {
    HKEY sesskey;
    int src_hive;
    int is_file;
    struct ksf_item *items;
    int raw_secrets;  /* KiTTY: .ktx read - passwords come back as stored */
    settings_r *defaults;  /* KiTTY (#61): Default Settings, for what this one leaves out */
};

/*
 * KiTTY (hknet/KiTTY#61): kitty.ini [KiTTY] inheritdefaultsettings=yes - a
 * session that leaves a setting out takes it from Default Settings (the
 * top-level one) instead of the built-in default. Every key, passwords and
 * commands included: a user who turns it on may keep a user name and a
 * password there on purpose, so that stub session files are enough; the
 * panel warns about exactly that when it is ticked. kitty.exe sets it from
 * kitty.ini at start and from the panel, klink/kscp/ksftp from the resolver.
 */
static int g_inherit_defaults = 0;
void kitty_set_inherit_default_settings(int on) { g_inherit_defaults = on ? 1 : 0; }
int kitty_inherit_default_settings(void) { return g_inherit_defaults; }

static settings_r *open_settings_r_own(const char *sessionname);

/* A session of the store, with Default Settings behind it when the switch is
 * on - not for Default Settings itself (asked for by name or as NULL). */
settings_r *open_settings_r(const char *sessionname)
{
    settings_r *h = open_settings_r_own(sessionname);
    if (sessionname && *sessionname)    /* NULL / "" is Default Settings itself */
        kitty_settings_r_inherit(h, sessionname);
    return h;
}

/* hknet/KiTTY#61, Session Lockdown: the session's OWN SessionLockdown - never
 * inherited from Default Settings, or one lock there would lock every stub -
 * and Default Settings itself is never locked (every new session is made
 * from it). */
int kitty_session_locked(const char *sessionname)
{
    settings_r *h;
    int on;
    if (!sessionname || !*sessionname || !strcmp(sessionname, KITTY_DEFAULT_SESSION))
        return 0;
    h = open_settings_r_own(sessionname);
    if (!h)
        return 0;
    on = read_setting_i(h, "SessionLockdown", 0) != 0;
    close_settings_r(h);
    return on;
}

/* The switch alone into the stored session: unlocking must not need a Save,
 * which the lock refuses. 1 written. */
int kitty_session_set_lockdown(const char *sessionname, int on)
{
    char *errmsg = NULL;
    settings_w *w;
    if (!sessionname || !*sessionname || !strcmp(sessionname, KITTY_DEFAULT_SESSION))
        return 0;
    w = open_settings_w(sessionname, &errmsg);
    if (!w) {
        sfree(errmsg);
        return 0;
    }
    write_setting_i(w, "SessionLockdown", on ? 1 : 0);
    close_settings_w(w);
    return 1;
}

/* save_settings' refusal for a locked session, NULL when it is not locked.
 * snewn'd. */
char *kitty_session_locked_msg(const char *sessionname)
{
    return kitty_session_locked(sessionname)
        ? dupprintf(KT_SESSION_LOCKED_FMT, sessionname) : NULL;
}

/* Default Settings behind a handle when the switch is on: a stored session's
 * (sessionname, never Default Settings itself), or one made elsewhere - a
 * .ktx file's (sessionname NULL). */
void kitty_settings_r_inherit(settings_r *handle, const char *sessionname)
{
    if (!handle || !g_inherit_defaults || handle->defaults)
        return;
    if (sessionname && !strcmp(sessionname, KITTY_DEFAULT_SESSION))
        return;
    handle->defaults = open_settings_r_own(KITTY_DEFAULT_SESSION);
}

/*
 * KiTTY: kitty.ini [KiTTY] saveonlynondefaultsettings=yes - with Inherit
 * Default Settings on, a session save writes only what differs from Default
 * Settings (kitty_nd_put); Default Settings itself, and the .ktx writer, are
 * always written whole. Without Inherit a key left out reads as the built-in
 * default, so the switch is in force only with it.
 */
static int g_save_nondefault = 0;
void kitty_set_save_only_nondefault(int on) { g_save_nondefault = on ? 1 : 0; }
int kitty_save_only_nondefault(void) { return g_save_nondefault; }

/* The rule on a write handle, defsession's stored values as the defaults.
 * No such session: nothing to compare with, written whole. */
void kitty_settings_w_nondefault(settings_w *handle, const char *defsession)
{
    if (!handle || handle->nd || handle->raw_secrets || !defsession)
        return;
    if (handle->is_file) {
        char *p = ksf_session_find(defsession);
        handle->defs = p ? ksf_load(p) : NULL;
        sfree(p);
        handle->nd = handle->defs != NULL;
    } else if (handle->sesskey) {
        strbuf *sb = strbuf_new();
        escape_registry_key(defsession, sb);
        handle->defkey = open_regkey_ro(HKEY_CURRENT_USER, puttystr, sb->s);
        strbuf_free(sb);
        handle->nd = handle->defkey != NULL;
    }
}

/* save_settings: the rule when both switches are on, never for Default
 * Settings. */
void kitty_settings_w_save_rules(settings_w *handle, const char *section)
{
    if (!g_save_nondefault || !g_inherit_defaults || !section || !*section ||
        !strcmp(section, KITTY_DEFAULT_SESSION))
        return;
    kitty_settings_w_nondefault(handle, KITTY_DEFAULT_SESSION);
}

/* "Comment out defaults" leaves the passwords too: stored protected, they
 * cannot be compared. */
static int kitty_cod_skip(const char *key)
{
    return kitty_nd_exempt(key) || kitty_secret_slot(key) >= 0;
}

static BYTE *kitty_reg_value_get(HKEY k, const char *name, DWORD *type,
                                 DWORD *size)
{
    BYTE *d;
    if (RegQueryValueExA(k, name, NULL, type, NULL, size) != ERROR_SUCCESS)
        return NULL;
    d = snewn(*size + 1, BYTE);
    if (RegQueryValueExA(k, name, NULL, type, d, size) != ERROR_SUCCESS) {
        sfree(d);
        return NULL;
    }
    return d;
}

/* One registry session: each value equal to defkey's (type and data) is
 * deleted. The values deleted, or -1 when one could not be. */
int kitty_reg_delete_defaults(HKEY k, HKEY defkey)
{
    char name[16384];
    char **names = NULL;
    size_t n = 0, alloc = 0, i;
    int done = 0, bad = 0;
    DWORD idx;
    if (kitty_store_readonly())
        return 0;                       /* readonly=yes: nothing deleted */
    for (idx = 0; ; idx++) {
        DWORD nl = sizeof(name), t1, t2, s1, s2;
        BYTE *a, *b;
        if (RegEnumValueA(k, idx, name, &nl, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;
        if (!name[0] || kitty_cod_skip(name))
            continue;
        a = kitty_reg_value_get(k, name, &t1, &s1);
        b = a ? kitty_reg_value_get(defkey, name, &t2, &s2) : NULL;
        if (b && t1 == t2 && s1 == s2 && !memcmp(a, b, s1)) {
            sgrowarray(names, alloc, n);
            names[n++] = dupstr(name);
        }
        sfree(a);
        sfree(b);
    }
    for (i = 0; i < n; i++) {
        if (RegDeleteValueA(k, names[i]) == ERROR_SUCCESS)
            done++;
        else
            bad = 1;
        sfree(names[i]);
    }
    sfree(names);
    return bad ? -1 : done;
}

/* Every session of the store but defsession, once: a folder-store line
 * equal to Default Settings becomes "# <line>", a registry value equal to it
 * is deleted. A locked session is left (Session Lockdown). Returns the
 * sessions changed; *nfailed those that could not be written. */
int kitty_store_comment_out_defaults(const char *defsession, int *nfailed)
{
    int changed = 0, failed = 0;
    if (kitty_store_readonly()) {       /* readonly=yes: no session changed */
        if (nfailed)
            *nfailed = 0;
        return 0;
    }
    if (!defsession || !*defsession)
        defsession = KITTY_DEFAULT_SESSION;
    if (store_is_file()) {
        char *dp = ksf_session_find(defsession);
        struct ksf_item *defs = dp ? ksf_load(dp) : NULL;
        sfree(dp);
        if (defs) {
            settings_e *e = enum_settings_start();
            strbuf *sb = strbuf_new();
            while (enum_settings_next(e, sb)) {
                char *path = NULL;
                struct ksf_item *items = NULL;
                if (strcmp(sb->s, defsession) && !kitty_session_locked(sb->s) &&
                    (path = ksf_session_find(sb->s)) != NULL &&
                    (items = ksf_load(path)) != NULL &&
                    ksf_list_comment_defaults(items, defs, kitty_cod_skip) > 0) {
                    if (ksf_save(path, items))
                        changed++;
                    else
                        failed++;
                }
                ksf_list_free(items);
                sfree(path);
                strbuf_clear(sb);
            }
            strbuf_free(sb);
            enum_settings_finish(e);
            ksf_list_free(defs);
        }
    } else {
        HKEY root = open_regkey_ro(HKEY_CURRENT_USER, puttystr);
        strbuf *esc = strbuf_new();
        HKEY defkey;
        escape_registry_key(defsession, esc);
        defkey = root ? open_regkey_ro(root, esc->s) : NULL;
        if (defkey) {
            char *name;
            int idx;
            for (idx = 0; (name = enum_regkey(root, idx)) != NULL; idx++) {
                strbuf *un = strbuf_new();
                HKEY k;
                unescape_registry_key(name, un);
                if (strcmp(un->s, defsession) && !kitty_session_locked(un->s) &&
                    (k = open_regkey_rw(root, name)) != NULL) {
                    int r = kitty_reg_delete_defaults(k, defkey);
                    if (r > 0)
                        changed++;
                    else if (r < 0)
                        failed++;
                    close_regkey(k);
                }
                strbuf_free(un);
                sfree(name);
            }
            close_regkey(defkey);
        }
        strbuf_free(esc);
        if (root)
            close_regkey(root);
    }
    if (changed)
        kitty_store_mark_dirty();
    if (nfailed)
        *nfailed = failed;
    return changed;
}

static settings_r *open_settings_r_own(const char *sessionname)
{
    if (!sessionname || !*sessionname)
        sessionname = KITTY_DEFAULT_SESSION;

    if (store_is_file()) {
        char *path = ksf_session_find(sessionname);   /* KiTTY: path, suffix, legacy */
        if (!path) return NULL;
        settings_r *handle = snew(settings_r);
        handle->raw_secrets = 0;
        handle->defaults = NULL;
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
    if (!sesskey && !kitty_root_is_putty() && kitty_get_show_foreign_sessions()) {
        /* KiTTY: fall back to the old KiTTY hive, then stock PuTTY's, so older and
         * PuTTY sessions stay loadable (precedence: our base > old KiTTY > PuTTY).
         *
         * ONLY while the old stores are shown: a name the saved-session list
         * hides is not found by -load, @name, the launcher or the console tools
         * either, so one switch has one meaning. The importer opens a hive by
         * name through kitty_open_settings_r_hive() and is not affected. */
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
    handle->raw_secrets = 0;
    handle->defaults = NULL;
    handle->sesskey = sesskey;
    handle->src_hive = src;
    handle->is_file = 0;
    handle->items = NULL;
    return handle;
}


/*
 * KiTTY: open a session from ONE named hive, bypassing the precedence chain.
 *
 * open_settings_r() answers "which session does this name mean", and for the
 * session importer that is the wrong question twice over: a foreign session
 * shadowed by a same-named session of our own is exactly the case the importer
 * has to handle, and the src_hive it records is what makes an old-KiTTY
 * password decrypt on the way in.
 */
settings_r *kitty_open_settings_r_hive(const char *sessionname, int hive)
{
    const char *path;
    if (!sessionname || !*sessionname || store_is_file())
        return NULL;
    if (hive == KSEC_HIVE_OLDKITTY)
        path = OLD_KITTY_HIVE_SESSIONS;
    else if (hive == KSEC_HIVE_PUTTY)
        path = PUTTY_HIVE_SESSIONS;
    else
        return NULL;

    strbuf *sb = strbuf_new();
    escape_registry_key(sessionname, sb);
    HKEY sesskey = open_regkey_ro(HKEY_CURRENT_USER, path, sb->s);
    strbuf_free(sb);
    if (!sesskey)
        return NULL;

    settings_r *handle = snew(settings_r);
    handle->raw_secrets = 0;
    handle->defaults = NULL;
    handle->sesskey = sesskey;
    handle->src_hive = hive;
    handle->is_file = 0;
    handle->items = NULL;
    return handle;
}

settings_r *kitty_open_settings_r_items(struct ksf_item *items)
{
    if (!items)
        return NULL;
    settings_r *handle = snew(settings_r);
    handle->raw_secrets = 0;
    handle->defaults = NULL;
    handle->sesskey = NULL;
    handle->src_hive = KSEC_HIVE_PRIMARY;
    handle->is_file = 1;
    handle->items = items;             /* owned from here; freed on close */
    return handle;
}

settings_r *kitty_open_settings_r_file(const char *path)
{
    struct ksf_item *items;
    if (!path || !*path)
        return NULL;
    items = ksf_load(path);
    if (!items)
        return NULL;
    settings_r *handle = snew(settings_r);
    handle->raw_secrets = 0;
    handle->defaults = NULL;
    handle->sesskey = NULL;
    handle->src_hive = KSEC_HIVE_PRIMARY;
    handle->is_file = 1;
    handle->items = items;
    return handle;
}

/* KiTTY: a .ktx file read through the ordinary loader. Its passwords come
 * back as the file holds them - the .ktx reader decodes them itself
 * (kitty_secret_decode_imported) - and never reach the never-wipe guard,
 * which belongs to sessions of this store. */
void kitty_settings_r_raw_secrets(settings_r *handle)
{
    if (handle)
        handle->raw_secrets = 1;
}

/* KiTTY: a write handle over a list in memory - the .ktx export runs the
 * ordinary save_open_settings into it and writes the file itself. Nothing is
 * written to the store, and passwords are left out (the export wraps them in
 * the bundle's envelope). Collect the list with kitty_settings_w_take_items,
 * never close_settings_w. */
settings_w *kitty_open_settings_w_items(void)
{
    settings_w *handle = snew(settings_w);
    handle->sesskey = NULL;
    handle->is_file = 1;
    handle->fpath = NULL;
    handle->items = NULL;
    handle->mig_answer = 1;
    handle->legacy = NULL;
    handle->raw_secrets = 1;
    handle->nd = 0;
    handle->defs = NULL;
    handle->defkey = NULL;
    handle->sessname = NULL;
    return handle;
}

struct ksf_item *kitty_settings_w_take_items(settings_w *handle)
{
    struct ksf_item *items;
    if (!handle)
        return NULL;
    items = handle->items;
    sfree(handle->fpath);
    sfree(handle->legacy);
    sfree(handle->sessname);
    sfree(handle);
    return items;
}

char *read_setting_s(settings_r *handle, const char *key)
{
    kitty_read_watch_note(key);
    if (!handle)
        return NULL;
    char *raw;
    if (handle->is_file) {
        const char *v = ksf_list_get(handle->items, key);
        raw = v ? dupstr(v) : NULL;
    } else {
        raw = get_reg_sz(handle->sesskey, key);
    }
    if (!raw) {
        /* Not there under the name it has now: a session written before the
         * rename still has it under the old one. */
        const char *was = kitty_setting_legacy_name(key);
        if (was) {
            kitty_read_watch_note(was);
            if (handle->is_file) {
                const char *v = ksf_list_get(handle->items, was);
                raw = v ? dupstr(v) : NULL;
            } else {
                raw = get_reg_sz(handle->sesskey, was);
            }
        }
    }
    if (!raw && handle->defaults)
        return read_setting_s(handle->defaults, key);   /* #61: Default Settings' */
    int slot = kitty_secret_slot(key);
    if (slot >= 0 && raw && !handle->raw_secrets) {
        char *pt = NULL;
        /* Old-KiTTY hive "Password" (slot 0): always legacy-encrypted there (we
         * never wrote to that hive). Decrypt it; the next Save re-stores it
         * DPAPI-encrypted in our own hive. ProxyPassword was never encrypted, and
         * the PuTTY/primary hives only hold our cleartext or DPAPI blobs. */
        if (handle->src_hive == KSEC_HIVE_OLDKITTY && slot == 0 &&
            strncmp(raw, KITTY_SECRET_DPAPI_MARK, strlen(KITTY_SECRET_DPAPI_MARK)) != 0)
            pt = ksec_legacy_decrypt(raw, handle->sesskey);   /* malloc or NULL */
        int rv_dbg = -99;                     /* -99 = the decrypt path never ran */
        if (!pt) {
            /* DPAPI blob -> plaintext; unmarked value passes through. Record an
             * undecryptable-here blob for the never-wipe guard on next save. */
            int rv = ksec_unprotect(raw, &pt);
            rv_dbg = rv;
            ksec_after_load(slot, raw, rv);
        }
        /* Migrate the auto-login password to UTF-8 (slot 0) so a legacy ANSI value
         * works at the UTF-8 prompt without re-entry; re-saved UTF-8 thereafter. */
        if (slot == 0) pt = ksec_to_utf8(pt);
        if (slot == 0)
            /* rv matters: declen=0 against an MPW2 raw value means the unwrap
             * handed back an EMPTY string instead of failing, so the session
             * authenticates with a blank password and says nothing. That is
             * exactly how the SAVEABLE -masterpwfile defect presented. */
            kitty_pwdebug("LOAD pw: mode=%d filemode=%d rawmark=%.7s declen=%d cksum=%04x rv=%d",
                          kitty_storage_mode(), handle->is_file, raw ? raw : "(null)",
                          pt ? (int)strlen(pt) : -1, ksec_cksum(pt), rv_dbg);
        sfree(raw);
        char *ret = dupstr(pt ? pt : "");
        if (pt) { memset(pt, 0, strlen(pt)); free(pt); }
        return ret;
    }
    return raw;
}

int read_setting_i(settings_r *handle, const char *key, int defvalue)
{
    kitty_read_watch_note(key);
    if (!handle)
        return defvalue;
    /* Missing under the current name, the old one is tried: see
     * kitty_retired_keys. */
    const char *was = kitty_setting_legacy_name(key);
    if (handle->is_file) {
        const char *v = ksf_list_get(handle->items, key);
        if (!v && was) {
            kitty_read_watch_note(was);
            v = ksf_list_get(handle->items, was);
        }
        if (v)
            return atoi(v);
        return handle->defaults ? read_setting_i(handle->defaults, key, defvalue) : defvalue;
    }
    DWORD val;
    if (get_reg_dword(handle->sesskey, key, &val))
        return val;
    if (was) {
        kitty_read_watch_note(was);
        if (get_reg_dword(handle->sesskey, was, &val))
            return val;
    }
    return handle->defaults ? read_setting_i(handle->defaults, key, defvalue) : defvalue;
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
        if (handle->defaults)
            close_settings_r(handle->defaults);
        if (handle->is_file)
            ksf_list_free(handle->items);
        else
            close_regkey(handle->sesskey);
        sfree(handle);
    }
}

/* KiTTY: did this read handle come from the store this KiTTY writes to (its
 * own hive, or a portable file), rather than a read-only fallback hive (old
 * 9bis KiTTY, stock PuTTY)? A write aimed at a name that only exists in a
 * fallback hive would land in the own hive as a new session - see
 * kitty_winpos_session_set. */
int kitty_settings_r_is_own(settings_r *handle)
{
    return handle && handle->src_hive == KSEC_HIVE_PRIMARY;
}

void del_settings(const char *sessionname)
{
    if (kitty_store_readonly())
        return;                         /* KiTTY: readonly=yes, nothing deleted */
    kitty_store_mark_dirty();
    if (store_is_file()) {
        char *path = ksf_session_find(sessionname);
        if (path) {
            DeleteFileA(path);
            ksf_prune_empty_dirs(path);   /* KiTTY: an emptied folder goes too */
            sfree(path);
        }
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
        /* KiTTY (hknet/KiTTY#55, #56): the whole folder tree, as path names;
         * dot-files and files that are not sessions are left out. */
        e->is_file = 1;
        e->names = ksf_enum_sessions(&e->count);
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
                if (!kitty_store_readonly())   /* KiTTY: readonly=yes */
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
    /* KiTTY: readonly=yes - the key is not stored; it holds for this
     * connection only (the caller's verification already accepted it). */
    if (kitty_store_readonly())
        return;
    /* The trust store changed - worth a backup. */
    kitty_store_mark_dirty();
    strbuf *regname = strbuf_new();
    hostkey_regname(regname, hostname, port, keytype);

    /* KiTTY: two stamps beside the key, for the Host keys leaf and klink:
     * "<name>:first" written once, "<name>:when" on every write - so a row
     * reads "known since X, key last written Y". Information for the human;
     * nothing decides on them. ISO local time. */
    char stamp[32];
    {
        SYSTEMTIME st;
        GetLocalTime(&st);
        snprintf(stamp, sizeof(stamp), "%04d-%02d-%02dT%02d:%02d:%02d",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    }
    char *name_first = dupcat(regname->s, ":first");
    char *name_when = dupcat(regname->s, ":when");

    if (store_is_file()) {
        char *had;
        portable_write_text_file("SshHostKeys", regname->s, key);
        had = portable_read_text_file("SshHostKeys", name_first);
        if (!had)
            portable_write_text_file("SshHostKeys", name_first, stamp);
        sfree(had);
        portable_write_text_file("SshHostKeys", name_when, stamp);
        sfree(name_first); sfree(name_when);
        strbuf_free(regname);
        return;
    }

    HKEY rkey = create_regkey(HKEY_CURRENT_USER,
                              reg_hostkeys_buf);
    if (rkey) {
        char *had;
        put_reg_sz(rkey, regname->s, key);
        had = get_reg_sz(rkey, name_first);
        if (!had)
            put_reg_sz(rkey, name_first, stamp);
        sfree(had);
        put_reg_sz(rkey, name_when, stamp);
        close_regkey(rkey);
    } /* else key does not exist in registry */

    sfree(name_first); sfree(name_when);
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
    if (kitty_store_readonly())
        return NULL;                    /* KiTTY: readonly=yes, not stored */

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
    if (kitty_store_readonly())
        return NULL;                    /* KiTTY: readonly=yes, not deleted */
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
            /* KiTTY: the seed file is the folder store's - under readonly=yes
             * read only (write_random_seed, cleanup_all stop first) */
            if (!kitty_store_readonly())
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
    /* KiTTY: the folder store's PUTTY.RND is not rewritten under
     * readonly=yes; in registry mode the seed lives outside the store
     * (%LOCALAPPDATA%) and is written as ever */
    if (store_is_file() && kitty_store_readonly())
        return;
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
    /* KiTTY: readonly=yes - the stored list is read, never changed (and
     * the registry key never created); the caller rebuilds the Windows
     * jump list from it as after a write */
    int ro = kitty_store_readonly();
    if (ro)
        add = rem = NULL;
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

    HKEY rkey = ro ? open_regkey_ro(HKEY_CURRENT_USER, reg_jumplist_key)
                   : create_regkey(HKEY_CURRENT_USER, reg_jumplist_key);
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
    /* KiTTY: readonly=yes - the store is not wiped (-cleanup refused) */
    if (kitty_store_readonly())
        return;

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
