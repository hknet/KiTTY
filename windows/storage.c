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

#include <shlobj.h>
#ifndef CSIDL_APPDATA
#define CSIDL_APPDATA 0x001a
#endif
#ifndef CSIDL_LOCAL_APPDATA
#define CSIDL_LOCAL_APPDATA 0x001c
#endif

/*
 * KiTTY: the runtime registry root, the portable file backend and the
 * at-rest credential crypto live in kitty/kitty_storage.c (split out of
 * this file to shrink its divergence from upstream). The macros below
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
        const char *keep = (value && value[0]) ? NULL : ksec_orig_get(slot);
        if (keep) {
            ksf_or_reg_put(handle, key, keep);
        } else {
            /* Portable saves need consent before converting an old-format
             * (unmarked) stored password to a protected one; declining keeps
             * the pre-loaded stored value in handle->items untouched. */
            if (handle->is_file && value && value[0] &&
                !kitty_portable_password_legacy() &&
                ksec_stored_is_legacy(ksf_list_get(handle->items, key))) {
                if (handle->mig_answer < 0)
                    handle->mig_answer = ksec_migrate_warn_ask();
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
    /* A session was written - the store differs from the last backup. */
    kitty_store_mark_dirty();
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
                          kitty_storage_mode(), handle->is_file, raw ? raw : "(null)",
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
    kitty_store_mark_dirty();
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
    /* The trust store changed - worth a backup. */
    kitty_store_mark_dirty();
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
