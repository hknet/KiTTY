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
     * the ordinary save path, into our hive, in our formats. */
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
            if (!kitty_namelist_has(&seen, valname))
                kitty_namelist_add(dropped, valname);
        }
        close_regkey(srckey);
    }

    kitty_namelist_clear(&seen);
    return target;
}
