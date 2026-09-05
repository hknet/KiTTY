/*
 * kitty_migrate.c: importing sessions from an older KiTTY or from stock PuTTY.
 *
 * The import is a LOAD followed by a SAVE, never a copy of the registry key.
 * That matters for more than tidiness:
 *
 *  - an old-KiTTY password is encrypted in the old KiTTY's own format, and
 *    only the read path knows that (it decides from the hive the handle came
 *    from). Copied verbatim into our hive the blob would be read as one of
 *    ours, and the session would fail to log in with no explanation.
 *  - settings this base no longer has are not carried over, which a copy
 *    cannot express: it would plant values nothing reads and nothing removes.
 *
 * Going through the ordinary load/save also means an imported session is
 * indistinguishable from one saved here, which is the point of importing it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "putty.h"
#include "storage.h"
#include "kitty_defs.h"
#include "kitty_storage.h"
#include "kitty_migrate.h"

/* ------------------------------------------------------------------ */
/* name sets                                                          */

void kitty_namelist_add(struct kitty_namelist *l, const char *name)
{
    if (!l || !name || !*name || kitty_namelist_has(l, name))
        return;
    if (l->n >= l->alloc) {
        l->alloc = l->alloc ? l->alloc * 2 : 16;
        l->names = sresize(l->names, l->alloc, char *);
    }
    l->names[l->n++] = dupstr(name);
}

bool kitty_namelist_has(const struct kitty_namelist *l, const char *name)
{
    if (!l || !name)
        return false;
    for (int i = 0; i < l->n; i++)
        if (!_stricmp(l->names[i], name))     /* the registry is case-blind */
            return true;
    return false;
}

void kitty_namelist_clear(struct kitty_namelist *l)
{
    if (!l)
        return;
    for (int i = 0; i < l->n; i++)
        sfree(l->names[i]);
    sfree(l->names);
    l->names = NULL;
    l->n = l->alloc = 0;
}

char *kitty_namelist_join(const struct kitty_namelist *l, const char *sep)
{
    strbuf *sb = strbuf_new();
    for (int i = 0; l && i < l->n; i++) {
        if (i)
            put_dataz(sb, sep);
        put_dataz(sb, l->names[i]);
    }
    return strbuf_to_str(sb);
}

/* ------------------------------------------------------------------ */
/* the read watch: which setting names the loader actually asked for   */

static struct kitty_namelist *watch_target = NULL;
static bool retired_and_migrates(const char *key);   /* defined with the file import */

static void watch_note(const char *key)
{
    kitty_namelist_add(watch_target, key);
}

static void watch_begin(struct kitty_namelist *l)
{
    watch_target = l;
    kitty_set_read_watch(watch_note);
}

static void watch_end(void)
{
    kitty_set_read_watch(NULL);
    watch_target = NULL;
}

/* ------------------------------------------------------------------ */
/* the foreign hives                                                   */

const char *kitty_foreign_hive_label(int hive)
{
    return hive == KSEC_HIVE_OLDKITTY ? "old KiTTY" :
           hive == KSEC_HIVE_PUTTY    ? "PuTTY" : "";
}

static const char *foreign_hive_path(int hive)
{
    return hive == KSEC_HIVE_OLDKITTY ? OLD_KITTY_HIVE_SESSIONS :
           hive == KSEC_HIVE_PUTTY    ? PUTTY_HIVE_SESSIONS : NULL;
}

/* The session's key in a given hive, or NULL. */
static HKEY foreign_session_key(const char *name, int hive)
{
    const char *path = foreign_hive_path(hive);
    if (!path)
        return NULL;
    strbuf *sb = strbuf_new();
    escape_registry_key(name, sb);
    HKEY key = open_regkey_ro(HKEY_CURRENT_USER, path, sb->s);
    strbuf_free(sb);
    return key;
}

struct kitty_foreign_list *kitty_foreign_sessions(void)
{
    static const int hives[2] = { KSEC_HIVE_OLDKITTY, KSEC_HIVE_PUTTY };
    struct kitty_foreign_list *l = snew(struct kitty_foreign_list);
    int alloc = 0;

    l->items = NULL;
    l->n = 0;
    if (store_is_file() || kitty_root_is_putty())
        return l;              /* nothing foreign to import from */

    for (int h = 0; h < 2; h++) {
        HKEY key = open_regkey_ro(HKEY_CURRENT_USER, foreign_hive_path(hives[h]));
        if (!key)
            continue;
        char *escaped;
        int idx = 0;
        while ((escaped = enum_regkey(key, idx)) != NULL) {
            idx++;
            strbuf *sb = strbuf_new();
            unescape_registry_key(escaped, sb);
            sfree(escaped);
            if (!strcmp(sb->s, KITTY_DEFAULT_SESSION)) {
                strbuf_free(sb);
                continue;      /* the defaults are not a session to import */
            }
            if (l->n >= alloc) {
                alloc = alloc ? alloc * 2 : 16;
                l->items = sresize(l->items, alloc,
                                   struct kitty_foreign_session);
            }
            l->items[l->n].name = dupstr(sb->s);
            l->items[l->n].hive = hives[h];
            l->n++;
            strbuf_free(sb);
        }
        close_regkey(key);
    }
    return l;
}

void kitty_foreign_list_free(struct kitty_foreign_list *l)
{
    if (!l)
        return;
    for (int i = 0; i < l->n; i++)
        sfree(l->items[i].name);
    sfree(l->items);
    sfree(l);
}

/* ------------------------------------------------------------------ */
/* the import                                                          */

/* Is there already a session of this name in OUR store? */
static bool own_session_exists(const char *name)
{
    strbuf *sb = strbuf_new();
    escape_registry_key(name, sb);
    HKEY key = open_regkey_ro(HKEY_CURRENT_USER, kitty_reg_sessions(), sb->s);
    strbuf_free(sb);
    if (!key)
        return false;
    close_regkey(key);
    return true;
}

/*
 * The name the copy gets. An existing session is never overwritten and the
 * import is never silently skipped, so a taken name is suffixed with where the
 * session came from - "work (PuTTY)" - and, if that is taken as well, counted
 * up until something is free.
 */
static char *import_target_name(const char *name, int hive)
{
    if (!own_session_exists(name))
        return dupstr(name);

    char *cand = dupcat(name, " (", kitty_foreign_hive_label(hive), ")");
    if (!own_session_exists(cand))
        return cand;

    for (int n = 2; n < 1000; n++) {
        char *numbered = dupprintf("%s %d", cand, n);
        if (!own_session_exists(numbered)) {
            sfree(cand);
            return numbered;
        }
        sfree(numbered);
    }
    sfree(cand);
    return NULL;
}

char *kitty_import_foreign_session(const char *name, int hive,
                                   struct kitty_namelist *dropped)
{
    if (!name || !*name || store_is_file())
        return NULL;

    settings_r *src = kitty_open_settings_r_hive(name, hive);
    if (!src)
        return NULL;

    /*
     * Read it with the watch on, so that afterwards we can say which of the
     * session's values this KiTTY has no home for: they are the ones the
     * loader never asked about.
     */
    struct kitty_namelist seen;
    memset(&seen, 0, sizeof(seen));
    Conf *conf = conf_new();
    watch_begin(&seen);
    load_open_settings(src, conf);
    watch_end();
    close_settings_r(src);

    char *target = import_target_name(name, hive);
    if (!target) {
        conf_free(conf);
        kitty_namelist_clear(&seen);
        return NULL;
    }

    /* An imported session is a session of ours from here on: it is written by
     * the ordinary save path, into our hive, in our formats. The never-lose
     * originals are dropped first, as in kitty_import_file_session: a blob the
     * source hive holds and we could not decrypt must not be written into our
     * hive verbatim. */
    ksec_after_load(0, NULL, 0);
    ksec_after_load(1, NULL, 0);
    char *err = save_settings(target, conf);
    conf_free(conf);
    if (err) {
        sfree(err);
        sfree(target);
        kitty_namelist_clear(&seen);
        return NULL;
    }

    /* What was left behind. Read from the SOURCE key rather than compared
     * against the saved one: a value we still read but store under another
     * name has not been lost, and would show up in that comparison. */
    HKEY srckey = foreign_session_key(name, hive);
    if (srckey) {
        char valname[16384];          /* the registry's own limit for a name */
        DWORD idx = 0, len;
        while (1) {
            len = lenof(valname);
            if (RegEnumValueA(srckey, idx, valname, &len, NULL, NULL,
                              NULL, NULL) != ERROR_SUCCESS)
                break;
            idx++;
            if (!valname[0])
                continue;             /* the key's default value has no name */
            if (!kitty_namelist_has(&seen, valname) &&
                !retired_and_migrates(valname))
                kitty_namelist_add(dropped, valname);
        }
        close_regkey(srckey);
    }

    kitty_namelist_clear(&seen);
    return target;
}

/* ------------------------------------------------------------------ */
/* sessions in files: a folder tree                                    */

bool kitty_own_session_exists(const char *name)
{
    if (!name || !*name)
        return false;
    if (store_is_file()) {
        char *p = ksf_session_path(name);
        bool yes = p && GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
        sfree(p);
        return yes;
    }
    return own_session_exists(name);
}

char *kitty_import_folder_target_name(const char *name, const char *folder,
                                      const struct kitty_namelist *taken)
{
    char *cand;
    /* "Default Settings" ALWAYS gets the suffix: under its own name it would
     * become this KiTTY's template, not a session. */
    if (strcmp(name, KITTY_DEFAULT_SESSION) &&
        !kitty_own_session_exists(name) && !kitty_namelist_has(taken, name))
        return dupstr(name);
    cand = dupcat(name, " (", folder, ")");
    if (!kitty_own_session_exists(cand) && !kitty_namelist_has(taken, cand))
        return cand;
    for (int n = 2; n < 1000; n++) {
        char *numbered = dupprintf("%s (%s %d)", name, folder, n);
        if (!kitty_own_session_exists(numbered) &&
            !kitty_namelist_has(taken, numbered)) {
            sfree(cand);
            return numbered;
        }
        sfree(numbered);
    }
    sfree(cand);
    return NULL;
}

/* Does the stored Password decode here? The loader's own decoder answers:
 * a marked value (ours, PLAIN:, or a legacy header) that fails is NULL; an
 * unmarked value falls back to itself, i.e. a cleartext password imports as
 * it is. */
static int classify_password(struct ksf_item *items)
{
    extern char *kitty_secret_decode_imported(const char *, const char *,
                                              const char *, int);
    extern int kitty_secret_is_mpw(const char *stored);       /* kitty_storage.c */
    const char *pw = ksf_list_get(items, "Password");
    char *pt;
    if (!pw || !*pw)
        return KFS_READY;
    /* A master-password value is not tried during the SCAN - asking file
     * after file is what a scan of a copied store turned into. An MPW2 value
     * carries its salt, so THAT store's master password unlocks it: the
     * import asks once and the answer is cached for the rest of the run. An
     * MPW1 value needs its store's salt, which a foreign folder does not
     * give us: it imports without the password. */
    switch (kitty_secret_is_mpw(pw)) {
      case 2: return KFS_PASSWORD_MPW;
      case 1: return KFS_PASSWORD;
      default: break;
    }
    pt = kitty_secret_decode_imported(pw, ksf_list_get(items, "HostName"),
                                      ksf_list_get(items, "TerminalType"), 1);
    /* A marked value that fails comes back as "" (the loader then saves the
     * session without a password) - that, or NULL, is the undecipherable
     * case. */
    if (!pt || !*pt) {
        if (pt) free(pt);
        return KFS_PASSWORD;
    }
    memset(pt, 0, strlen(pt));
    free(pt);
    return KFS_READY;
}

static void scan_add(struct kitty_folder_scan *s, const char *name,
                     const char *path, const char *folder, int state)
{
    struct kitty_folder_scan_item *it;
    sgrowarray(s->items, s->alloc, s->n);
    it = &s->items[s->n++];
    it->name = dupstr(name);
    it->path = dupstr(path);
    it->folder = dupstr(folder);
    it->state = state;
}

/* One directory level. A file counts as a session when it sits in a directory
 * named Sessions, or when it parses to a list naming a host or a protocol -
 * so a copied Sessions folder and a bare heap of session files both work.
 * Hidden and system directories, and reparse points, are not entered. */
/* `sess_rel` is the directory path below the nearest Sessions directory
 * ("" directly in it, "prod\db" two levels down), or NULL outside one. An old
 * KiTTY kept its session FOLDERS as subdirectories of Sessions, and our
 * folder names take the same backslash-separated shape (CleanFolderName), so
 * that path is the natural default target for a file that carries no Folder
 * key of its own. */
static void scan_dir(struct kitty_folder_scan *s, const char *dir, int depth,
                     const char *default_folder, const char *sess_rel)
{
    WIN32_FIND_DATAA fd;
    HANDLE h;
    char *pattern;
    const char *leaf;
    bool in_sessions;

    if (depth > KFS_MAX_DEPTH) { s->hit_depth = true; return; }
    s->dirs_seen++;
    leaf = strrchr(dir, '\\');
    leaf = leaf ? leaf + 1 : dir;
    in_sessions = (sess_rel != NULL) || !_stricmp(leaf, "Sessions");

    pattern = dupcat(dir, "\\*");
    h = FindFirstFileA(pattern, &fd);
    sfree(pattern);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        char *full;
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, ".."))
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))
                continue;
            full = dupcat(dir, "\\", fd.cFileName);
            {
                /* Into a Sessions directory: its subdirectories are folders. */
                char *sub = NULL;
                if (sess_rel)
                    sub = *sess_rel ? dupcat(sess_rel, "\\", fd.cFileName)
                                    : dupstr(fd.cFileName);
                else if (!_stricmp(leaf, "Sessions"))
                    sub = dupstr(fd.cFileName);
                else if (!_stricmp(fd.cFileName, "Sessions"))
                    sub = dupstr("");
                scan_dir(s, full, depth + 1, default_folder, sub);
                sfree(sub);
            }
            sfree(full);
            if (s->hit_count)
                break;
            continue;
        }
        if (s->files_seen >= KFS_MAX_FILES) { s->hit_count = true; break; }
        s->files_seen++;
        full = dupcat(dir, "\\", fd.cFileName);
        {
            struct ksf_item *items = ksf_load(full);
            bool session = items && (ksf_list_get(items, "HostName") ||
                                     ksf_list_get(items, "Protocol"));
            if (in_sessions || session) {
                char *name = ksf_unmunge(fd.cFileName);
                int state = !items ? KFS_UNREADABLE : classify_password(items);
                /* The row's default folder: the file's own Folder key, else
                 * the old store's directory below Sessions, else the panel's
                 * default. "Default" in the file means "no folder" there. */
                const char *fkey = items ? ksf_list_get(items, "Folder") : NULL;
                const char *folder =
                    (fkey && *fkey && strcmp(fkey, "Default")) ? fkey :
                    (sess_rel && *sess_rel) ? sess_rel : default_folder;
                scan_add(s, name, full, folder, state);
                sfree(name);
            }
            ksf_list_free(items);
        }
        sfree(full);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

struct kitty_folder_scan *kitty_scan_folder_store(const char *root,
                                                  const char *default_folder)
{
    struct kitty_folder_scan *s = snew(struct kitty_folder_scan);
    memset(s, 0, sizeof(*s));
    if (root && *root) {
        /* Pointed straight at a Sessions directory: its subdirectories are
         * folders from the start. */
        const char *leaf = strrchr(root, '\\');
        leaf = leaf ? leaf + 1 : root;
        scan_dir(s, root, 0, default_folder ? default_folder : "",
                 !_stricmp(leaf, "Sessions") ? "" : NULL);
    }
    return s;
}

void kitty_folder_scan_free(struct kitty_folder_scan *s)
{
    if (!s) return;
    for (int i = 0; i < s->n; i++) {
        sfree(s->items[i].name);
        sfree(s->items[i].path);
        sfree(s->items[i].folder);
    }
    sfree(s->items);
    sfree(s);
}

/* Which retired keys are NOT worth naming as "not carried over": one whose
 * value MIGRATES (read under its new name - a file written after the rename
 * carries both - or under the old one as the fallback), and one that names
 * no replacement at all (a marker the old program wrote, never a setting the
 * user set - telling them it was left behind tells them nothing). What
 * remains is a real setting with no counterpart, and the manual says why. */
static bool retired_and_migrates(const char *key)
{
    size_t n, i;
    const struct kitty_retired_key *t = kitty_retired_key_table(&n);
    for (i = 0; i < n; i++)
        if (!strcmp(t[i].was, key) && (t[i].migrates || !t[i].now || !t[i].now[0]))
            return true;
    return false;
}

struct dropped_ctx { const struct kitty_namelist *seen; struct kitty_namelist *dropped; };
static void note_unread(const char *key, const char *val, void *vctx)
{
    struct dropped_ctx *c = (struct dropped_ctx *)vctx;
    (void)val;
    if (!kitty_namelist_has(c->seen, key) && !retired_and_migrates(key))
        kitty_namelist_add(c->dropped, key);
}

/* The import's OWN decryption: a master-password value of the source store is
 * opened with the passphrase the import asked for, and handed to the loader
 * as "PLAIN:<text>" - the marker the loader already strips. Our store's
 * unlock is never involved: not consulted, not prompted, not changed. */
static void import_open_mpw_values(struct ksf_item **items, const char *store_pass)
{
    static const char *const keys[] = { "Password", "ProxyPassword" };
    for (size_t k = 0; k < lenof(keys); k++) {
        const char *v = ksf_list_get(*items, keys[k]);
        char *pt;
        if (!v || kitty_secret_is_mpw(v) != 2 || !store_pass)
            continue;
        pt = kitty_mpw2_unprotect_with_passphrase(v, store_pass);
        if (pt) {
            char *marked = dupcat(KITTY_SECRET_PLAIN_MARK, pt);
            ksf_list_set(items, keys[k], marked);
            smemclr(marked, strlen(marked));
            sfree(marked);
            memset(pt, 0, strlen(pt));
            free(pt);
        }
    }
}

bool kitty_import_store_pass_fits(const char *path, const char *pass)
{
    struct ksf_item *items = ksf_load(path);
    const char *v = ksf_list_get(items, "Password");
    bool ok = false;
    if (v && kitty_secret_is_mpw(v) == 2 && pass && *pass) {
        char *pt = kitty_mpw2_unprotect_with_passphrase(v, pass);
        if (pt) { ok = true; memset(pt, 0, strlen(pt)); free(pt); }
    }
    ksf_list_free(items);
    return ok;
}

bool kitty_import_file_session(const char *path, const char *target,
                               const char *folder, const char *store_pass,
                               struct kitty_namelist *dropped,
                               bool *password_lost)
{
    extern void kitty_set_defer_mpw_prompt(int on);           /* kitty_storage.c */
    settings_r *src;
    struct ksf_item *items, *report;
    struct kitty_namelist seen;
    Conf *conf;
    char *err;
    const char *srcpw;

    if (password_lost) *password_lost = false;
    if (!path || !target || !*target)
        return false;
    items = ksf_load(path);
    if (!items)
        return false;
    report = ksf_load(path);           /* the untouched source keys, for the report */
    srcpw = ksf_list_get(report, "Password");

    /* The import decrypts what it can with ITS passphrase, then the loader
     * runs with our store's prompt held off: whatever is still wrapped after
     * this step (another machine's DPAPI blob, an MPW1 value without its
     * salt, a wrong or absent store passphrase) loads as empty and the
     * session is imported without it - the summary says how many. */
    import_open_mpw_values(&items, store_pass);
    src = kitty_open_settings_r_items(items);   /* owns items now */

    /* The same road as the hive import: read with the watch on, save through
     * the ordinary path. A file copy would keep the old password blob, which
     * our reader would then take for one of ours. */
    memset(&seen, 0, sizeof(seen));
    conf = conf_new();
    kitty_set_defer_mpw_prompt(1);
    watch_begin(&seen);
    load_open_settings(src, conf);
    watch_end();
    kitty_set_defer_mpw_prompt(0);

    if (password_lost && srcpw && *srcpw && !*conf_get_str(conf, CONF_password))
        *password_lost = true;
    if (dropped) {
        struct dropped_ctx c = { &seen, dropped };
        ksf_list_foreach(report, note_unread, &c);
    }
    ksf_list_free(report);
    close_settings_r(src);

    conf_set_str(conf, CONF_folder, folder && *folder ? folder : "Default");
    /* The store's never-lose rule keeps a password blob it could not decrypt
     * and writes it back verbatim when the in-memory password is empty. Right
     * for a session of ours whose key is missing today; WRONG for an import:
     * it would plant a foreign blob in our store, which our reader would then
     * take for one of ours - the very thing this load-then-save exists to
     * avoid. Drop both originals (Password, ProxyPassword) before saving. */
    ksec_after_load(0, NULL, 0);
    ksec_after_load(1, NULL, 0);
    err = save_settings(target, conf);
    conf_free(conf);
    kitty_namelist_clear(&seen);
    if (err) {
        sfree(err);
        return false;
    }
    return true;
}
