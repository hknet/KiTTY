/*
 * kitty_sessionrekey.c - move or rename saved sessions together with what
 * hangs off their names (hknet/KiTTY#55). The contract is in
 * kitty_sessionrekey.h.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "putty.h"
#include "storage.h"
#include "kitty.h"
#include "kitty_defs.h"
#include "kitty_storage.h"
#include "kitty_sessionpath.h"
#include "kitty_sessionrekey.h"
#include "kitty_registry.h"
#include "kitty_migrate.h"
#include "kitty_commun.h"
#include "kitty_text.h"

bool kitty_session_exists_own(const char *name)
{
    return kitty_own_session_exists(name);
}

/* The registry key of a session, below the sessions key in use. */
static char *rk_regpath(const char *name)
{
    strbuf *sb = strbuf_new();
    char *p;
    escape_registry_key(name, sb);
    p = dupprintf("%s\\%s", kitty_reg_sessions(), sb->s);
    strbuf_free(sb);
    return p;
}

/* Move one session's key or file. Returns NULL or an error text (snewn'd). */
static char *rk_move_one(const char *from, const char *to)
{
    if (store_is_file()) {
        char *src = ksf_session_find(from);
        char *dst = ksf_session_target_path(to);
        char *err = NULL;
        if (!src) {
            err = dupprintf(KT_CFG_SESSION_UPDATE_FAILED, from, KT_MSG_UNKNOWN_ERROR);
        } else if (strcmp(src, dst)) {
            ksf_make_parent_dirs(dst);
            if (!MoveFileA(src, dst))
                err = dupprintf(KT_CFG_SESSION_UPDATE_FAILED, from,
                                win_strerror(GetLastError()));
            else
                ksf_prune_empty_dirs(src);
        }
        sfree(src);
        sfree(dst);

        /* The per-session commands of the folder store live beside it, under
         * the name munged the way kitty_specialmenu.c looks them up. */
        if (!err) {
            char *base = portable_subdir_path("Sessions_Commands");
            char *mf = snewn(strlen(from) * 3 + 1, char);
            char *mt = snewn(strlen(to) * 3 + 1, char);
            mungestr(from, mf);
            mungestr(to, mt);
            if (base && strcmp(mf, mt)) {
                char *a = dupprintf("%s\\%s", base, mf);
                char *b = dupprintf("%s\\%s", base, mt);
                if (GetFileAttributesA(a) != INVALID_FILE_ATTRIBUTES)
                    MoveFileA(a, b);
                sfree(a);
                sfree(b);
            }
            sfree(base);
            sfree(mf);
            sfree(mt);
        }
        return err;
    }

    {
        /* Copy every value whatever its size, prove the copy equal, and only
         * then delete the source (ksp_reg_move_tree). A failed copy is
         * removed again and the source stays as it was. */
        char *src = rk_regpath(from), *dst = rk_regpath(to), *err = NULL;
        char *leftover = NULL;
        if (!ksp_reg_move_tree(HKEY_CURRENT_USER, src, dst, &leftover)) {
            if (leftover) {
                char *why = dupprintf(KT_SP_REKEY_LEFTOVER, leftover);
                err = dupprintf(KT_CFG_SESSION_UPDATE_FAILED, from, why);
                sfree(why);
            } else {
                err = dupprintf(KT_CFG_SESSION_UPDATE_FAILED, from,
                                KT_SP_REKEY_COPY_FAILED);
            }
        }
        sfree(leftover);
        sfree(src);
        sfree(dst);
        return err;
    }
}

/* The Folder value follows the path. */
static void rk_write_folder(const char *to)
{
    char *errmsg = NULL;
    char *fld = ksp_folder_of(to);
    settings_w *w = open_settings_w(to, &errmsg);
    if (w) {
        write_setting_s(w, "Folder", fld ? fld : "Default");
        close_settings_w(w);
    }
    sfree(errmsg);
    sfree(fld);
}

/* The jump list keeps names; a moved session keeps its place in it. */
static void rk_jumplist(const char *from, const char *to)
{
    char *list = get_jumplist_registry_entries();
    const char *p;
    bool had = false;
    for (p = list; p && *p; p += strlen(p) + 1)
        if (!strcmp(p, from))
            had = true;
    sfree(list);
    if (had) {
        remove_session_from_jumplist(from);
        add_session_to_jumplist(to);
    }
}

/* What the launcher shows for a session: its leaf inside a folder path. */
static const char *rk_launcher_label(const char *name)
{
    return strchr(name, '\\') ? ksp_leaf(name) : name;
}

/* The launcher's copy of the session list in the registry: values whose
 * data names the session. */
static void rk_launcher_reg_walk(const char *keypath, const char *from,
                                 const char *to)
{
    HKEY k;
    DWORD i;
    char **sub = NULL;
    int nsub = 0;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, keypath, 0,
                      KEY_READ | KEY_SET_VALUE, &k) != ERROR_SUCCESS)
        return;
    for (i = 0;; i++) {
        char name[1024];
        DWORD nl = sizeof(name);
        if (RegEnumKeyExA(k, i, name, &nl, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;
        sub = sresize(sub, nsub + 1, char *);
        sub[nsub++] = dupprintf("%s\\%s", keypath, name);
    }
    /* Collect first, rewrite after: changing values while enumerating them
     * shifts the indexes, and a rename that only changes case would match
     * its own rewritten entry again. */
    {
        char **hits = NULL;
        int nh = 0, h;
        for (i = 0;; i++) {
            char name[1024];
            BYTE data[4096];
            DWORD nl = sizeof(name), dl = sizeof(data) - 1, type = 0;
            LONG rc = RegEnumValueA(k, i, name, &nl, NULL, &type, data, &dl);
            if (rc == ERROR_MORE_DATA)
                continue;      /* longer than any session name: not ours */
            if (rc != ERROR_SUCCESS)
                break;
            data[dl] = '\0';
            if (type == REG_SZ && !stricmp((char *)data, from)) {
                hits = sresize(hits, nh + 1, char *);
                hits[nh++] = dupstr(name);
            }
        }
        for (h = 0; h < nh; h++) {
            RegDeleteValueA(k, hits[h]);
            RegSetValueExA(k, rk_launcher_label(to), 0, REG_SZ,
                           (const BYTE *)to, (DWORD)strlen(to) + 1);
            sfree(hits[h]);
        }
        sfree(hits);
    }
    RegCloseKey(k);
    for (int j = 0; j < nsub; j++) {
        rk_launcher_reg_walk(sub[j], from, to);
        sfree(sub[j]);
    }
    sfree(sub);
}

/* The folder store's launcher copy: one file per entry, "label\session\". */
static void rk_launcher_dir_walk(const char *dir, const char *from,
                                 const char *to)
{
    char *pat = dupprintf("%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    sfree(pat);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        char *full;
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, ".."))
            continue;
        full = dupprintf("%s\\%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            rk_launcher_dir_walk(full, from, to);
        } else {
            FILE *fp = fopen(full, "rb");
            char line[4096];
            bool hit = false;
            if (fp) {
                if (fgets(line, sizeof(line), fp)) {
                    char *bs, *e;
                    size_t n = strlen(line);
                    while (n && (line[n-1] == '\n' || line[n-1] == '\r'))
                        line[--n] = '\0';
                    if (n && line[n-1] == '\\')
                        line[--n] = '\0';
                    bs = strchr(line, '\\');
                    e = bs ? bs + 1 : NULL;
                    hit = e && !stricmp(e, from);
                }
                fclose(fp);
            }
            if (hit && (fp = fopen(full, "wb")) != NULL) {
                fprintf(fp, "%s\\%s\\", rk_launcher_label(to), to);
                fclose(fp);
            }
        }
        sfree(full);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static void rk_launcher(const char *from, const char *to)
{
    if (store_is_file()) {
        char *dir = portable_subdir_path("Launcher");
        if (dir)
            rk_launcher_dir_walk(dir, from, to);
        sfree(dir);
    } else {
        char *key = dupprintf("%s\\Launcher", kitty_registry_base());
        rk_launcher_reg_walk(key, from, to);
        sfree(key);
    }
}

/* The proxy-type map of the session store (utils/conf_data.c), declared the
 * way settings.c declares it. */
extern const ConfSaveEnumType conf_enum_proxy_type;

static bool rk_proxy_is_ssh(int storageval)
{
    int confval;
    if (!conf_enum_map_from_storage(&conf_enum_proxy_type, storageval, &confval))
        return false;
    return confval == PROXY_SSH_TCPIP || confval == PROXY_SSH_EXEC ||
        confval == PROXY_SSH_SUBSYSTEM;
}

/*
 * Every session that names a moved one as its SSH jump host. The reference
 * may be the exact identity, or a bare name that resolved to it BEFORE the
 * move (pre_names: the store's names before anything moved) - both now
 * point at nothing, or at something else, and are rewritten to the new path.
 */
static void rk_jump_refs(char *const *from, char *const *to, int n,
                         char **pre_names, int npre, strbuf *rewritten)
{
    int nnow, i, j;
    char **now = kitty_session_names(&nnow);
    int *m = snewn(npre > 0 ? npre : 1, int);
    for (i = 0; i < nnow; i++) {
        settings_r *r = open_settings_r(now[i]);
        char *host;
        int method, target = -1, nm = 0;
        if (!r)
            continue;
        method = read_setting_i(r, "ProxyMethod", -1);
        host = read_setting_s(r, "ProxyHost");
        close_settings_r(r);
        if (!host || !*host || !rk_proxy_is_ssh(method)) {
            sfree(host);
            continue;
        }
        for (j = 0; j < n && target < 0; j++)
            if (!stricmp(host, from[j]))
                target = j;
        if (target < 0 && !strchr(host, '\\')) {
            int kind = ksp_lookup(pre_names, npre, host, m, npre, &nm);
            if (kind == KSP_UNIQUE)
                for (j = 0; j < n && target < 0; j++)
                    if (!stricmp(pre_names[m[0]], from[j]))
                        target = j;
        }
        if (target >= 0 && stricmp(host, to[target])) {
            char *errmsg = NULL;
            settings_w *w = open_settings_w(now[i], &errmsg);
            if (w) {
                write_setting_s(w, "ProxyHost", to[target]);
                close_settings_w(w);
                if (rewritten) {
                    if (rewritten->len)
                        put_byte(rewritten, '\n');
                    put_dataz(rewritten, now[i]);
                }
            }
            sfree(errmsg);
        }
        sfree(host);
    }
    sfree(m);
    kitty_session_names_free(now, nnow);
}

static void rk_note(strbuf *sb, const char *name)
{
    if (!sb)
        return;
    if (sb->len)
        put_byte(sb, '\n');
    put_dataz(sb, name);
}

int kitty_session_rekey_many(char *const *from, char *const *to, int n,
                             strbuf *rewritten, strbuf *clashes,
                             char **errmsg)
{
    int i, j, npre = 0;
    bool clash = false;
    char **pre = NULL;
    char last[1024];

    if (errmsg)
        *errmsg = NULL;
    if (n <= 0)
        return KITTY_REKEY_OK;

    /* Everything is checked before anything moves. */
    for (i = 0; i < n; i++) {
        bool same = !stricmp(from[i], to[i]);
        if (!from[i][0] || !to[i][0] ||
            !strcmp(from[i], KITTY_DEFAULT_SESSION) ||
            !strcmp(to[i], KITTY_DEFAULT_SESSION) ||
            !kitty_session_exists_own(from[i])) {
            rk_note(clashes, from[i]);
            clash = true;
            continue;
        }
        if (!same && kitty_session_exists_own(to[i])) {
            rk_note(clashes, to[i]);
            clash = true;
            continue;
        }
        for (j = 0; j < i; j++)
            if (!stricmp(to[j], to[i])) {
                rk_note(clashes, to[i]);
                clash = true;
                break;
            }
    }
    if (clash)
        return KITTY_REKEY_CLASH;

    /* Folder store: what the moves would run into on disk is checked before
     * the first one, so a batch is refused whole rather than stopped
     * half-way - a folder where a session file goes, a session file where a
     * folder goes (no suffix: "Linux" the session and "Linux" the folder are
     * one name), or a source that is not a file that can be read. A target
     * whose blocker is one of the batch's own sources may still be fine once
     * that source has moved, so it is not counted. */
    if (store_is_file()) {
        strbuf *why = strbuf_new();
        for (i = 0; i < n; i++) {
            char *blk, *src;
            if (!stricmp(from[i], to[i]))
                continue;
            src = ksf_session_find(from[i]);
            if (!src) {
                rk_note(why, from[i]);
            } else {
                HANDLE fh = CreateFileA(src, GENERIC_READ, FILE_SHARE_READ, NULL,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
                if (fh == INVALID_HANDLE_VALUE)
                    rk_note(why, from[i]);
                else
                    CloseHandle(fh);
            }
            sfree(src);
            blk = ksf_path_blocker(to[i]);
            if (blk) {
                bool own = false;
                for (j = 0; j < n && !own; j++) {
                    char *rel = ksp_path_to_relfile(from[j], kitty_session_suffix());
                    own = !stricmp(rel, blk);
                    sfree(rel);
                }
                if (!own) {
                    char *t = dupprintf(KT_SP_PATH_BLOCKED, blk);
                    rk_note(why, t);
                    sfree(t);
                }
                sfree(blk);
            }
        }
        if (why->len) {
            if (errmsg)
                *errmsg = dupprintf(KT_SP_REKEY_REFUSED, why->s);
            strbuf_free(why);
            return KITTY_REKEY_FAILED;
        }
        strbuf_free(why);
    }

    pre = kitty_session_names(&npre);
    if (!kitty_get_last_session(last, sizeof(last)))
        last[0] = '\0';

    for (i = 0; i < n; i++) {
        char *err;
        if (!strcmp(from[i], to[i]))
            continue;
        err = rk_move_one(from[i], to[i]);
        if (err) {
            /* Name what did move before the failure: those are re-keyed and
             * stay where they went. */
            strbuf *done = strbuf_new();
            for (j = 0; j < i; j++)
                if (strcmp(from[j], to[j])) {
                    if (done->len)
                        put_byte(done, '\n');
                    put_fmt(done, KT_SP_REKEY_MOVED_PAIR, from[j], to[j]);
                }
            if (done->len) {
                char *more = dupprintf("%s\n\n" KT_SP_REKEY_PARTIAL, err, done->s);
                sfree(err);
                err = more;
            }
            strbuf_free(done);
            if (errmsg)
                *errmsg = err;
            else
                sfree(err);
            break;
        }
        rk_write_folder(to[i]);
        rk_jumplist(from[i], to[i]);
        rk_launcher(from[i], to[i]);
        if (last[0] && !stricmp(last, from[i]))
            kitty_set_last_session(to[i]);
    }
    kitty_store_mark_dirty();
    kitty_session_folder_cache_clear();

    rk_jump_refs(from, to, i, pre, npre, rewritten);
    kitty_session_names_free(pre, npre);
    return i < n ? KITTY_REKEY_FAILED : KITTY_REKEY_OK;
}

int kitty_session_rekey(const char *from, const char *to,
                        strbuf *rewritten, strbuf *clashes, char **errmsg)
{
    char *f = (char *)from, *t = (char *)to;
    return kitty_session_rekey_many(&f, &t, 1, rewritten, clashes, errmsg);
}
