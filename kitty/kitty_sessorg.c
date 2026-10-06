/*
 * kitty_sessorg.c - Organize sessions, Arrange, and the box that deletes a
 * folder holding sessions (hknet/KiTTY#55). The contract is in
 * kitty_sessorg.h.
 *
 * A session's identity is its folder path plus its name (kitty_sessionpath.h)
 * and moving it is a re-key (kitty_sessionrekey.h): key or file, Folder
 * value, jump list, launcher, last session and the jump-host references to
 * it, in one step. What to move is PLANNED first (ksp_plan_*, unit-tested):
 * a plan with a clash is not carried out at all.
 *
 * Every window here is a dialog template of windows/kitty.rc, created
 * modeless (CreateDialogParam + ShinyAddAuxDialog) and owned by the
 * configuration window, so it never holds that window up. Nothing here
 * paints: the theme engine dresses each window on its activation
 * (kitty_theme.c handles the tree view, the list view, the combo box and
 * the buttons), buttons are as wide as their captions
 * (kitty_theme_button_width), the label of the name row is lined up by the
 * shared row aligner, and the resizable window moves its controls with the
 * shared edge anchoring (kitty_anchor.h).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "putty.h"
#include "storage.h"

#include <windows.h>
#include <commctrl.h>

#include "kitty.h"               /* FolderList, InitFolderList, SaveFolderList */
#include "kitty_commun.h"        /* GetReadOnlyFlag */
#include "kitty_tools.h"         /* StringList_Add / _Del */
#include "kitty_storage.h"
#include "kitty_sessionpath.h"
#include "kitty_sessionrekey.h"
#include "kitty_regbackup.h"     /* SaveRegistryKeyNow: the regbackup before a registry move */
#include "kitty_dlgbox.h"        /* the shared confirm / info boxes, kitty_fit_text */
#include "kitty_theme.h"         /* button widths, ink marks, the row aligner */
#include "kitty_anchor.h"        /* the shared resize */
#include "kitty_text.h"
#include "kitty_sessorg.h"
#include "kitty_auxpos.h"        /* the window's own place and size, per monitor layout */
#include "kitty_inikeys.h"       /* KR_DLGPOS_MANAGE: its name there */
#include "kitty_launcher.h"      /* RunConfig: -manage's Edit starts a session in a new process */

/* windows/utils/shinydialogbox.c: Esc and Tab for modeless dialogs */
void ShinyAddAuxDialog(HWND hwnd);
void ShinyRemoveAuxDialog(HWND hwnd);

/* A clash list or a list of rewritten sessions can run to hundreds of lines;
 * a box must stay on the screen. The first lines, then how many more. */
#define SO_LIST_MAX 15
static char *so_cap_list(const char *list)
{
    strbuf *sb = strbuf_new();
    const char *p = list ? list : "";
    int lines = 0, more = 0;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (lines < SO_LIST_MAX) {
            if (lines)
                put_byte(sb, '\n');
            put_data(sb, p, n);
            lines++;
        } else {
            more++;
        }
        p += n;
        if (*p == '\n')
            p++;
    }
    if (more)
        put_fmt(sb, KT_SP_ORG_LIST_MORE, more);
    return strbuf_to_str(sb);
}

static char *so_join(char **list, int n)
{
    strbuf *sb = strbuf_new();
    int i;
    for (i = 0; i < n; i++) {
        if (i)
            put_byte(sb, '\n');
        put_dataz(sb, list[i]);
    }
    return strbuf_to_str(sb);
}

static const char *so_folder_label(const char *f)
{
    return ksp_folder_is_root(f) ? KT_SP_ORG_ROOT : f;
}

/* Tell the user the jump-host references that followed a move. */
static void so_report_rewritten(HWND owner, const char *rewritten)
{
    char *list, *msg;
    if (!rewritten || !*rewritten)
        return;
    list = so_cap_list(rewritten);
    msg = dupprintf(KT_SP_JUMP_REWRITTEN, list);
    kitty_info_modeless(owner, KT_CAP_KITTYPP, msg, NULL, NULL);
    sfree(msg);
    sfree(list);
}

/* Every session identity and, for one stored by its bare name, its Folder
 * value (NULL for a path session: its path says where it is). */
struct so_names {
    char **names;
    char **folders;
    int n;
};
static void so_names_load(struct so_names *s)
{
    int i;
    kitty_session_folder_cache_clear();
    s->names = kitty_session_names(&s->n);
    s->folders = snewn(s->n > 0 ? s->n : 1, char *);
    for (i = 0; i < s->n; i++)
        s->folders[i] = strchr(s->names[i], '\\') ? NULL :
            kitty_read_session_folder(s->names[i]);
}
static void so_names_free(struct so_names *s)
{
    int i;
    for (i = 0; i < s->n; i++)
        sfree(s->folders[i]);
    sfree(s->folders);
    kitty_session_names_free(s->names, s->n);
    memset(s, 0, sizeof(*s));
}

/* The folder a session is in: its path's, or its Folder value; "" = root. */
static char *so_effective_folder(const char *name, const char *folder_value)
{
    char *f;
    if (strchr(name, '\\'))
        return ksp_folder_of(name);
    if (ksp_folder_is_root(folder_value))
        return dupstr("");
    f = ksp_normalise(folder_value);
    return f;
}

static void so_write_folder(const char *name, const char *folder)
{
    char *errmsg = NULL;
    settings_w *w = open_settings_w(name, &errmsg);
    if (w) {
        write_setting_s(w, "Folder", ksp_folder_is_root(folder) ? "Default" : folder);
        close_settings_w(w);
    }
    sfree(errmsg);
}

/* "Default Settings" belongs to no folder, but an older version may have
 * left a Folder value in it - invisible in the list, yet enough for the
 * folder list to rebuild a moved or deleted folder straight back. */
static void so_clear_default_folder(const char *from)
{
    char *f = kitty_read_session_folder("Default Settings");
    if (f && !ksp_folder_is_root(f) && ksp_folder_within(f, from))
        so_write_folder("Default Settings", "Default");
    sfree(f);
}

/* The stored folder list follows a folder that moved, with its subtree. */
static void so_folderlist_move(const char *from, const char *to)
{
    so_clear_default_folder(from);
    int i, nm = 0;
    char **moved = NULL;
    InitFolderList();
    for (i = 0; FolderList && FolderList[i] != NULL; i++)
        if (FolderList[i][0] && ksp_folder_within(FolderList[i], from)) {
            moved = sresize(moved, nm + 1, char *);
            moved[nm++] = dupstr(FolderList[i]);
        }
    for (i = 0; i < nm; i++) {
        StringList_Del(FolderList, moved[i]);
        if (to) {
            char *np = ksp_folder_moved_path(moved[i], from, to);
            if (!ksp_folder_is_root(np))
                StringList_Add(FolderList, np);
            sfree(np);
        }
        sfree(moved[i]);
    }
    sfree(moved);
    if (to && !ksp_folder_is_root(to))
        StringList_Add(FolderList, to);
    SaveFolderList();
}

int kitty_sessorg_folder_move(const char *from, const char *to,
                              const char *clash_fmt, char **msg,
                              char **rewritten)
{
    struct so_names s;
    struct ksp_plan p;
    strbuf *rw, *cl;
    char *err = NULL;
    int i, r;

    *msg = NULL;
    if (rewritten)
        *rewritten = NULL;
    if (ksp_folder_is_root(from))
        return 0;
    if (!ksp_folder_is_root(to) && ksp_folder_within(to, from) &&
        stricmp(to, from)) {
        *msg = dupstr(KT_SP_ORG_NOT_EMPTY_INTO_SELF);
        return 0;
    }
    if (store_is_file()) {
        char *blk = ksf_folder_blocker(to);
        if (blk) {
            *msg = dupprintf(KT_SP_PATH_BLOCKED, blk);
            sfree(blk);
            return 0;
        }
    }
    so_names_load(&s);
    ksp_plan_folder_move(s.names, s.folders, s.n, from, to, &p);
    if (p.nclash) {
        char *list = so_join(p.clash, p.nclash), *capped = so_cap_list(list);
        *msg = dupprintf(clash_fmt, so_folder_label(to), capped);
        sfree(capped);
        sfree(list);
        ksp_plan_free(&p);
        so_names_free(&s);
        return 0;
    }
    if (!store_is_file())
        SaveRegistryKeyNow();           /* the regbackup, before anything moves */
    rw = strbuf_new();
    cl = strbuf_new();
    r = kitty_session_rekey_many(p.from, p.to, p.n, rw, cl, &err);
    if (r == KITTY_REKEY_OK) {
        for (i = 0; i < p.nfolder; i++)
            so_write_folder(p.fname[i], p.fvalue[i]);
        so_folderlist_move(from, to);
        if (rewritten && rw->len)
            *rewritten = dupstr(rw->s);
    } else if (r == KITTY_REKEY_CLASH) {
        char *capped = so_cap_list(cl->s);
        *msg = dupprintf(clash_fmt, so_folder_label(to), capped);
        sfree(capped);
    } else {
        *msg = err ? err : dupstr(KT_MSG_UNKNOWN_ERROR);
        err = NULL;
    }
    sfree(err);
    strbuf_free(rw);
    strbuf_free(cl);
    ksp_plan_free(&p);
    so_names_free(&s);
    return r == KITTY_REKEY_OK;
}

/* Move sessions into a folder under their own names. Returns 1 done, 0 with
 * *msg set (snewn'd). */
static int so_move_sessions(HWND owner, char **sel, int nsel, const char *dest,
                            char **msg)
{
    struct so_names s;
    struct ksp_plan p;
    strbuf *rw, *cl;
    char **mf = NULL, **mt = NULL, *err = NULL;
    int i, nm = 0, r;

    *msg = NULL;
    if (store_is_file() && !ksp_folder_is_root(dest)) {
        char *blk = ksf_folder_blocker(dest);
        if (blk) {
            *msg = dupprintf(KT_SP_PATH_BLOCKED, blk);
            sfree(blk);
            return 0;
        }
    }
    so_names_load(&s);
    ksp_plan_sessions_move(s.names, s.n, sel, nsel, dest, &p);
    if (p.nclash) {
        char *list = so_join(p.clash, p.nclash), *capped = so_cap_list(list);
        *msg = dupprintf(KT_SP_MOVE_CLASH, so_folder_label(dest), capped);
        sfree(capped);
        sfree(list);
        ksp_plan_free(&p);
        so_names_free(&s);
        return 0;
    }
    /* A re-key for every session whose identity changes. One that keeps it
     * (a session filed by its Folder value only, moved to the root) has only
     * that value to change. */
    for (i = 0; i < p.n; i++)
        if (strcmp(p.from[i], p.to[i])) {
            mf = sresize(mf, nm + 1, char *);
            mt = sresize(mt, nm + 1, char *);
            mf[nm] = p.from[i];
            mt[nm] = p.to[i];
            nm++;
        }
    if (!store_is_file())
        SaveRegistryKeyNow();
    rw = strbuf_new();
    cl = strbuf_new();
    r = kitty_session_rekey_many(mf, mt, nm, rw, cl, &err);
    if (r == KITTY_REKEY_OK) {
        for (i = 0; i < p.n; i++)
            if (!strcmp(p.from[i], p.to[i]))
                so_write_folder(p.to[i], dest);
        so_report_rewritten(owner, rw->s);
    } else if (r == KITTY_REKEY_CLASH) {
        char *capped = so_cap_list(cl->s);
        *msg = dupprintf(KT_SP_MOVE_CLASH, so_folder_label(dest), capped);
        sfree(capped);
    } else {
        *msg = err ? err : dupstr(KT_MSG_UNKNOWN_ERROR);
        err = NULL;
    }
    sfree(err);
    sfree(mf);
    sfree(mt);
    strbuf_free(rw);
    strbuf_free(cl);
    ksp_plan_free(&p);
    so_names_free(&s);
    kitty_config_session_store_changed();
    return r == KITTY_REKEY_OK;
}

/* ---- Clone and Copy to... -----------------------------------------------
 * A session copied whole under a new identity: its settings loaded and saved
 * under the new name, as a Save under another name in the configuration box
 * does. Clone keeps the folder and adds "-1", "-2", ...; Copy to... keeps
 * the name in the destination folder and adds a number only when the name
 * is taken there. */
static int so_id_taken(const struct so_names *s, char **made, int nmade,
                       const char *id)
{
    int i;
    for (i = 0; i < s->n; i++)
        if (!stricmp(s->names[i], id))
            return 1;
    for (i = 0; i < nmade; i++)
        if (!stricmp(made[i], id))
            return 1;
    if (store_is_file()) {
        char *blk = ksf_path_blocker(id);
        if (blk) {
            sfree(blk);
            return 1;
        }
    }
    return 0;
}

/* The first free identity for a copy of `src` ("" dest = the root; NULL =
 * src's own folder, a clone). snewn'd. */
static char *so_copy_target(const struct so_names *s, char **made, int nmade,
                            const char *src, const char *dest, int clone)
{
    const char *leaf = ksp_leaf(src);
    char *fld = dest ? (ksp_folder_is_root(dest) ? NULL : dupstr(dest))
                     : ksp_folder_of(src);
    int n;
    for (n = clone ? 1 : 0; ; n++) {
        char *nm = n ? dupprintf("%s-%d", leaf, n) : dupstr(leaf);
        char *id = fld ? dupprintf("%s\\%s", fld, nm) : dupstr(nm);
        sfree(nm);
        if (!so_id_taken(s, made, nmade, id)) {
            sfree(fld);
            return id;
        }
        sfree(id);
    }
}

/* Returns 1 done; 0 with *msg (snewn'd) naming the session that failed -
 * the ones before it are copied. */
static int so_copy_sessions(char **sel, int nsel, const char *dest, int clone,
                            char **msg)
{
    struct so_names s;
    char **made = snewn(nsel > 0 ? nsel : 1, char *);
    int i, nmade = 0, ok = 1;
    *msg = NULL;
    if (!store_is_file())
        SaveRegistryKeyNow();
    so_names_load(&s);
    for (i = 0; i < nsel && ok; i++) {
        Conf *c = conf_new();
        char *to, *err;
        if (!load_settings(sel[i], c)) {
            conf_free(c);
            *msg = dupprintf(KT_SP_ORG_COPY_FAILED, sel[i], KT_MSG_UNKNOWN_ERROR);
            ok = 0;
            break;
        }
        to = so_copy_target(&s, made, nmade, sel[i], dest, clone);
        if (dest)
            conf_set_str(c, CONF_folder, ksp_folder_is_root(dest) ? "Default" : dest);
        err = save_settings(to, c);
        conf_free(c);
        if (err) {
            *msg = dupprintf(KT_SP_ORG_COPY_FAILED, sel[i], err);
            sfree(err);
            sfree(to);
            ok = 0;
            break;
        }
        made[nmade++] = to;
    }
    for (i = 0; i < nmade; i++)
        sfree(made[i]);
    sfree(made);
    so_names_free(&s);
    kitty_store_mark_dirty();
    kitty_config_session_store_changed();
    return ok;
}

/* ------------------------------------------------------------------------
 * Small pieces every box here shares
 * ------------------------------------------------------------------------ */

static void so_rect(HWND h, int id, RECT *r)
{
    GetWindowRect(GetDlgItem(h, id), r);
    MapWindowPoints(NULL, h, (POINT *)r, 2);
}

/* A box's red line takes `text` (or goes empty); what sits below it and the
 * window itself move by the height it gained or lost. */
static void so_set_warn(HWND h, int warn_id, const char *text, const int *below)
{
    int dh, i;
    HWND w = GetDlgItem(h, warn_id);
    SetWindowTextA(w, text ? text : "");
    ShowWindow(w, SW_SHOW);
    dh = kitty_fit_text(h, warn_id, (text && *text) ? text : " ", 0);
    if (!dh)
        return;
    for (i = 0; below[i]; i++) {
        RECT r;
        so_rect(h, below[i], &r);
        MoveWindow(GetDlgItem(h, below[i]), r.left, r.top + dh,
                   r.right - r.left, r.bottom - r.top, TRUE);
    }
    {
        RECT wr;
        GetWindowRect(h, &wr);
        SetWindowPos(h, NULL, 0, 0, wr.right - wr.left,
                     (wr.bottom - wr.top) + dh, SWP_NOMOVE | SWP_NOZORDER);
    }
    InvalidateRect(h, NULL, TRUE);
}

/* The buttons of a box's foot, each as wide as its caption, right-aligned
 * (ids in order, left to right). The box widens rather than clip one. */
static void so_fit_buttons_right(HWND h, const int *ids)
{
    int n, i, gap, margin, total = 0, x, w[8];
    RECT first, second, cr;
    for (n = 0; ids[n] && n < 8; n++)
        ;
    if (!n)
        return;
    so_rect(h, ids[0], &first);
    GetClientRect(h, &cr);
    margin = cr.right - (so_rect(h, ids[n - 1], &second), second.right);
    if (margin < 4)
        margin = 4;
    gap = 4;
    if (n > 1) {
        so_rect(h, ids[1], &second);
        gap = second.left - first.right;
        if (gap < 4)
            gap = 4;
    }
    for (i = 0; i < n; i++) {
        RECT r;
        so_rect(h, ids[i], &r);
        w[i] = kitty_theme_button_width(GetDlgItem(h, ids[i]), r.right - r.left);
        total += w[i] + (i ? gap : 0);
    }
    if (total + 2 * margin > cr.right) {
        RECT wr;
        GetWindowRect(h, &wr);
        SetWindowPos(h, NULL, 0, 0, (wr.right - wr.left) + total + 2 * margin - cr.right,
                     wr.bottom - wr.top, SWP_NOMOVE | SWP_NOZORDER);
        GetClientRect(h, &cr);
    }
    x = cr.right - margin - total;
    for (i = 0; i < n; i++) {
        RECT r;
        so_rect(h, ids[i], &r);
        MoveWindow(GetDlgItem(h, ids[i]), x, r.top, w[i], r.bottom - r.top, TRUE);
        x += w[i] + gap;
    }
}

static HWND so_dialog(int tpl, HWND owner, DLGPROC proc, void *ctx)
{
    HWND h = CreateDialogParamA(GetModuleHandle(NULL), MAKEINTRESOURCEA(tpl),
                                owner, proc, (LPARAM)ctx);
    if (!h)
        return NULL;
    ShinyAddAuxDialog(h);
    ShowWindow(h, SW_SHOW);
    SetForegroundWindow(h);             /* the theme dresses it on activation */
    return h;
}

/* A sorted list of every folder: the stored folder list, the folders the
 * sessions are in, and every folder above one of those. */
struct so_folders {
    char **f;
    int n;
};
static void so_folders_add(struct so_folders *fs, const char *f)
{
    int i;
    char *norm;
    if (ksp_folder_is_root(f))
        return;
    norm = ksp_normalise(f);
    if (!*norm) {
        sfree(norm);
        return;
    }
    for (i = 0; i < fs->n; i++)
        if (!stricmp(fs->f[i], norm)) {
            sfree(norm);
            return;
        }
    fs->f = sresize(fs->f, fs->n + 1, char *);
    fs->f[fs->n++] = norm;
    {
        char *up = ksp_folder_of(fs->f[fs->n - 1]);
        if (up) {
            so_folders_add(fs, up);
            sfree(up);
        }
    }
}
/* Siblings share their parent's path, so this orders each level by the name
 * its rows show, digit runs by value; a parent still sorts before its children,
 * which so_fill_tree_open's parent lookup relies on. */
static int so_folders_cmp(const void *a, const void *b)
{
    const char *x = *(char *const *)a, *y = *(char *const *)b;
    int c = ksp_natcasecmp(x, y);
    return c ? c : strcmp(x, y);
}
static void so_folders_load(struct so_folders *fs, const struct so_names *s)
{
    int i;
    memset(fs, 0, sizeof(*fs));
    InitFolderList();
    for (i = 0; FolderList && FolderList[i] != NULL; i++)
        if (FolderList[i][0])
            so_folders_add(fs, FolderList[i]);
    for (i = 0; i < s->n; i++) {
        char *f = so_effective_folder(s->names[i], s->folders[i]);
        so_folders_add(fs, f);
        sfree(f);
    }
    if (fs->n)
        qsort(fs->f, fs->n, sizeof(char *), so_folders_cmp);
}
static void so_folders_free(struct so_folders *fs)
{
    int i;
    for (i = 0; i < fs->n; i++)
        sfree(fs->f[i]);
    sfree(fs->f);
    memset(fs, 0, sizeof(*fs));
}

/* The folders whose rows are expanded now (snewn'd copies, *n of them), so a
 * rebuild of the tree can open them again: a re-read after every move must
 * not fold the folders being worked in. */
static char **so_tree_expanded(HWND tree, const struct so_folders *fs, int *n)
{
    char **out = snewn(fs->n + 1, char *);
    HTREEITEM it = TreeView_GetRoot(tree);
    *n = 0;
    while (it) {
        TVITEMA tv;
        HTREEITEM next;
        memset(&tv, 0, sizeof(tv));
        tv.mask = TVIF_PARAM | TVIF_STATE;
        tv.stateMask = TVIS_EXPANDED;
        tv.hItem = it;
        if (SendMessageA(tree, TVM_GETITEMA, 0, (LPARAM)&tv) &&
            (tv.state & TVIS_EXPANDED) && tv.lParam >= 0 &&
            tv.lParam < fs->n && *n < fs->n)
            out[(*n)++] = dupstr(fs->f[tv.lParam]);
        /* depth-first walk: child, else next sibling, else an ancestor's */
        next = TreeView_GetChild(tree, it);
        while (!next && it) {
            next = TreeView_GetNextSibling(tree, it);
            if (!next)
                it = TreeView_GetParent(tree, it);
        }
        it = next;
    }
    return out;
}

/* Fill a tree view: the root row (lParam -1), then each folder under its
 * parent (lParam = index into fs). Selects `select` ("" = root) and expands
 * the folders named in `open` (n of them) that still exist. */
static void so_fill_tree_open(HWND tree, const struct so_folders *fs,
                              const char *select, char **open, int nopen);
static void so_fill_tree(HWND tree, const struct so_folders *fs,
                         const char *select)
{
    so_fill_tree_open(tree, fs, select, NULL, 0);
}
static void so_fill_tree_open(HWND tree, const struct so_folders *fs,
                              const char *select, char **open, int nopen)
{
    HTREEITEM *items = snewn(fs->n + 1, HTREEITEM), root, want;
    TVINSERTSTRUCTA ins;
    int i, j;
    SendMessage(tree, WM_SETREDRAW, FALSE, 0);
    TreeView_DeleteAllItems(tree);
    memset(&ins, 0, sizeof(ins));
    ins.hParent = TVI_ROOT;
    ins.hInsertAfter = TVI_LAST;
    ins.item.mask = TVIF_TEXT | TVIF_PARAM;
    ins.item.pszText = (char *)KT_SP_ORG_ROOT;
    ins.item.lParam = -1;
    root = (HTREEITEM)SendMessageA(tree, TVM_INSERTITEMA, 0, (LPARAM)&ins);
    want = root;
    for (i = 0; i < fs->n; i++) {
        char *parent = ksp_folder_of(fs->f[i]);
        HTREEITEM ph = root;
        if (parent)
            for (j = 0; j < i; j++)
                if (!stricmp(fs->f[j], parent)) {
                    ph = items[j];
                    break;
                }
        sfree(parent);
        ins.hParent = ph;
        ins.item.pszText = (char *)ksp_leaf(fs->f[i]);
        ins.item.lParam = i;
        items[i] = (HTREEITEM)SendMessageA(tree, TVM_INSERTITEMA, 0, (LPARAM)&ins);
        if (select && *select && !stricmp(fs->f[i], select))
            want = items[i];
    }
    TreeView_Expand(tree, root, TVE_EXPAND);
    for (j = 0; j < nopen; j++)
        for (i = 0; i < fs->n; i++)
            if (!stricmp(fs->f[i], open[j])) {
                TreeView_Expand(tree, items[i], TVE_EXPAND);
                break;
            }
    TreeView_SelectItem(tree, want);
    TreeView_EnsureVisible(tree, want);
    SendMessage(tree, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(tree, NULL, TRUE);
    sfree(items);
}

/* The folder a tree row stands for ("" = root), snewn'd; NULL = none. */
static char *so_tree_folder(HWND tree, HTREEITEM it, const struct so_folders *fs)
{
    TVITEMA tv;
    if (!it)
        return NULL;
    memset(&tv, 0, sizeof(tv));
    tv.mask = TVIF_PARAM;
    tv.hItem = it;
    if (!SendMessageA(tree, TVM_GETITEMA, 0, (LPARAM)&tv))
        return NULL;
    if (tv.lParam < 0 || tv.lParam >= fs->n)
        return dupstr("");
    return dupstr(fs->f[tv.lParam]);
}

/* ------------------------------------------------------------------------
 * The Organize sessions window
 * ------------------------------------------------------------------------ */

static const struct kl_anchor so_anchors[] = {
    {IDC_SO_TREE,      KL_ANCH_LEFT | KL_ANCH_TOP | KL_ANCH_BOTTOM},
    {IDC_SO_COUNT,     KL_ANCH_LEFT | KL_ANCH_TOP | KL_ANCH_RIGHT},
    {IDC_SO_LIST,      KL_ANCH_LEFT | KL_ANCH_TOP | KL_ANCH_RIGHT | KL_ANCH_BOTTOM},
    {IDC_SO_MOVE,      KL_ANCH_LEFT | KL_ANCH_BOTTOM},
    {IDC_SO_RENAME,    KL_ANCH_LEFT | KL_ANCH_BOTTOM},
    {IDC_SO_NEWFOLDER, KL_ANCH_LEFT | KL_ANCH_BOTTOM},
    {IDC_SO_DELFOLDER, KL_ANCH_LEFT | KL_ANCH_BOTTOM},
    {IDC_SO_ARRANGE,   KL_ANCH_LEFT | KL_ANCH_BOTTOM},
    {IDC_SO_EDIT,      KL_ANCH_RIGHT | KL_ANCH_BOTTOM},
    {IDCANCEL,         KL_ANCH_RIGHT | KL_ANCH_BOTTOM},
};

struct so_state {
    HWND h, tree, list;
    struct so_names s;
    struct so_folders fs;
    char *cur;                      /* the folder shown, "" = root */
    HWND sub;                       /* the Move / name / delete box, if open */
    int pane;                       /* 0 tree, 1 list: what Rename acts on */
    int dragging;                   /* SO_DRAG_SESSIONS / SO_DRAG_FOLDER, 0 = none */
    char *dragfolder;               /* the folder being dragged */
    HIMAGELIST dragimg;             /* what the pointer carries */
    HTREEITEM hover;                /* the folder under the pointer, and since when */
    DWORD hover_since;
    int ready;
    long gen;                       /* kitty_store_generation() when read */
    RECT rects[lenof(so_anchors)];
    SIZE basesize, minsize;
};

static struct so_state *so_window = NULL;   /* one Organize window at a time */

/* kitty.exe -manage: this window is the program (kitty_manage_WinMain), and
 * its Edit opens the configuration window in this process. While that one is
 * open, so_editing is set: the configuration window's Exit then reads Close
 * and closes only itself, and its Open starts the session in a new process. */
static int so_manage_mode = 0;
static int so_editing = 0;
static int so_quit_after_edit = 0;          /* closed while the edit stood */
int kitty_manage_editing(void) { return so_editing; }

static void so_fill_list(struct so_state *st)
{
    LVITEMA it;
    int i, k, nrows = 0, shown = 0;
    char *count;
    /* The folder's sessions, in the order of the name each row shows: a
     * session stored by its bare name and one stored as "folder\name" sit
     * side by side here, and their identities do not sort together. The
     * control has no LVS_SORTASCENDING (kitty.rc), so this order is kept. */
    struct ksp_shown_row *rows = snewn(st->s.n > 0 ? st->s.n : 1,
                                       struct ksp_shown_row);
    for (i = 0; i < st->s.n; i++) {
        char *f;
        if (!strcmp(st->s.names[i], "Default Settings"))
            continue;
        f = so_effective_folder(st->s.names[i], st->s.folders[i]);
        if (ksp_folder_same(*f ? f : NULL, *st->cur ? st->cur : NULL)) {
            rows[nrows].id = st->s.names[i];
            rows[nrows].folder = NULL;      /* all in the folder shown */
            rows[nrows].idx = i;
            nrows++;
        }
        sfree(f);
    }
    ksp_sort_shown(rows, nrows);
    SendMessage(st->list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(st->list);
    memset(&it, 0, sizeof(it));
    it.mask = LVIF_TEXT | LVIF_PARAM;
    for (k = 0; k < nrows; k++) {
        it.iItem = shown;
        it.pszText = (char *)ksp_leaf(rows[k].id);
        it.lParam = rows[k].idx;
        if (SendMessageA(st->list, LVM_INSERTITEMA, 0, (LPARAM)&it) >= 0)
            shown++;
    }
    sfree(rows);
    ListView_SetColumnWidth(st->list, 0, LVSCW_AUTOSIZE_USEHEADER);
    SendMessage(st->list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(st->list, NULL, TRUE);
    count = shown == 1 ? dupstr(KT_SP_ORG_COUNT_ONE) : dupprintf(KT_SP_ORG_COUNT, shown);
    SetDlgItemTextA(st->h, IDC_SO_COUNT, count);
    sfree(count);
    EnableWindow(GetDlgItem(st->h, IDC_SO_DELFOLDER), *st->cur != '\0');
}

/* Everything from the store again, the same folder shown if it still is. */
static void so_select_session_add(struct so_state *st, const char *name);

static void so_reload(struct so_state *st)
{
    char *keep = dupstr(st->cur);
    int i, found = !*keep, nopen = 0, nkeep = 0;
    char **keepsel = NULL;
    /* the selection by name, before the names are read again */
    {
        int j = -1;
        while ((j = ListView_GetNextItem(st->list, j, LVNI_SELECTED)) >= 0) {
            LVITEMA it;
            memset(&it, 0, sizeof(it));
            it.mask = LVIF_PARAM;
            it.iItem = j;
            if (SendMessageA(st->list, LVM_GETITEMA, 0, (LPARAM)&it) &&
                it.lParam >= 0 && it.lParam < st->s.n) {
                keepsel = sresize(keepsel, nkeep + 1, char *);
                keepsel[nkeep++] = dupstr(st->s.names[it.lParam]);
            }
        }
    }
    /* what was open, and how far the tree was scrolled, before the re-read */
    char **open = so_tree_expanded(st->tree, &st->fs, &nopen);
    HTREEITEM top = TreeView_GetFirstVisible(st->tree);
    char *topf = so_tree_folder(st->tree, top, &st->fs);
    so_folders_free(&st->fs);
    so_names_free(&st->s);
    st->gen = kitty_store_generation();
    so_names_load(&st->s);
    so_folders_load(&st->fs, &st->s);
    for (i = 0; i < st->fs.n && !found; i++)
        if (!stricmp(st->fs.f[i], keep))
            found = 1;
    sfree(st->cur);
    st->cur = found ? keep : dupstr("");
    if (!found)
        sfree(keep);
    so_fill_tree_open(st->tree, &st->fs, st->cur, open, nopen);
    /* the same first visible row, when that folder still exists */
    if (topf) {
        HTREEITEM it = TreeView_GetRoot(st->tree);
        while (it) {
            char *f = so_tree_folder(st->tree, it, &st->fs);
            int same = f && !stricmp(f, topf);
            HTREEITEM next;
            sfree(f);
            if (same) {
                TreeView_Select(st->tree, it, TVGN_FIRSTVISIBLE);
                break;
            }
            next = TreeView_GetChild(st->tree, it);
            while (!next && it) {
                next = TreeView_GetNextSibling(st->tree, it);
                if (!next)
                    it = TreeView_GetParent(st->tree, it);
            }
            it = next;
        }
        sfree(topf);
    }
    for (i = 0; i < nopen; i++)
        sfree(open[i]);
    sfree(open);
    so_fill_list(st);
    /* the sessions that were selected stay selected (a re-read on coming
     * back from the configuration window must not drop them) */
    for (i = 0; i < nkeep; i++) {
        so_select_session_add(st, keepsel[i]);
        sfree(keepsel[i]);
    }
    sfree(keepsel);
}

/* Add the session `name` to the list's selection, if it is there. */
static void so_select_session_add(struct so_state *st, const char *name)
{
    int i, n = ListView_GetItemCount(st->list);
    for (i = 0; i < n; i++) {
        LVITEMA it;
        memset(&it, 0, sizeof(it));
        it.mask = LVIF_PARAM;
        it.iItem = i;
        if (SendMessageA(st->list, LVM_GETITEMA, 0, (LPARAM)&it) &&
            it.lParam >= 0 && it.lParam < st->s.n &&
            !strcmp(st->s.names[it.lParam], name)) {
            ListView_SetItemState(st->list, i, LVIS_SELECTED, LVIS_SELECTED);
            return;
        }
    }
}

/* Select, focus and show the session `name` in the list, if it is there. */
static void so_select_session(struct so_state *st, const char *name)
{
    int i, n = ListView_GetItemCount(st->list);
    for (i = 0; i < n; i++) {
        LVITEMA it;
        memset(&it, 0, sizeof(it));
        it.mask = LVIF_PARAM;
        it.iItem = i;
        if (SendMessageA(st->list, LVM_GETITEMA, 0, (LPARAM)&it) &&
            it.lParam >= 0 && it.lParam < st->s.n &&
            !strcmp(st->s.names[it.lParam], name)) {
            ListView_SetItemState(st->list, i, LVIS_SELECTED | LVIS_FOCUSED,
                                  LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(st->list, i, FALSE);
            return;
        }
    }
}

/* The selected sessions' identities (snewn'd array of borrowed strings). */
static char **so_selected(struct so_state *st, int *n)
{
    int i = -1;
    char **sel = NULL;
    *n = 0;
    while ((i = ListView_GetNextItem(st->list, i, LVNI_SELECTED)) >= 0) {
        LVITEMA it;
        memset(&it, 0, sizeof(it));
        it.mask = LVIF_PARAM;
        it.iItem = i;
        if (SendMessageA(st->list, LVM_GETITEMA, 0, (LPARAM)&it) &&
            it.lParam >= 0 && it.lParam < st->s.n) {
            sel = sresize(sel, *n + 1, char *);
            sel[(*n)++] = st->s.names[it.lParam];
        }
    }
    return sel;
}

static void so_changed(struct so_state *st)
{
    kitty_config_session_store_changed();
    if (st)
        so_reload(st);
}

/* ---- Move to... ------------------------------------------------------- */

struct so_move {
    struct so_state *st;        /* NULL once the window is gone */
    struct so_folders fs;
    char **sel;                 /* owned copies */
    int nsel;
    int copy;                   /* Copy to...: the same box, copying */
};

static void so_move_free(struct so_move *m)
{
    int i;
    for (i = 0; i < m->nsel; i++)
        sfree(m->sel[i]);
    sfree(m->sel);
    so_folders_free(&m->fs);
    sfree(m);
}

static INT_PTR CALLBACK so_move_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    struct so_move *m = (struct so_move *)GetWindowLongPtr(h, GWLP_USERDATA);
    static const int below[] = { IDOK, IDCANCEL, 0 };
    static const int btns[] = { IDOK, IDCANCEL, 0 };
    switch (msg) {
      case WM_INITDIALOG: {
        char *q;
        m = (struct so_move *)lp;
        SetWindowLongPtr(h, GWLP_USERDATA, lp);
        SetWindowTextA(h, KT_SP_ORG_TITLE);
        if (m->copy)
            q = m->nsel == 1 ? dupstr(KT_SP_ORG_COPY_ONE_Q)
                             : dupprintf(KT_SP_ORG_COPY_Q, m->nsel);
        else
            q = m->nsel == 1 ? dupstr(KT_SP_ORG_MOVE_ONE_Q)
                             : dupprintf(KT_SP_ORG_MOVE_Q, m->nsel);
        SetDlgItemTextA(h, IDC_SOM_TEXT, q);
        sfree(q);
        SetDlgItemTextA(h, IDOK, m->copy ? KT_SP_ORG_COPY_BTN : KT_SP_ORG_MOVE_BTN);
        SetDlgItemTextA(h, IDCANCEL, KT_SP_ORG_CANCEL);
        kitty_theme_mark_ink(GetDlgItem(h, IDC_SOM_WARN), KITTY_INK_BAD);
        so_fit_buttons_right(h, btns);
        so_fill_tree(GetDlgItem(h, IDC_SOM_TREE), &m->fs,
                     m->st ? m->st->cur : "");
        kitty_dialog_icon(h, NULL);
        kitty_centre_on_owner(h);
        SetFocus(GetDlgItem(h, IDC_SOM_TREE));
        return FALSE;
      }
      case WM_COMMAND:
        if (!m)
            return FALSE;
        if (LOWORD(wp) == IDOK) {
            HWND tree = GetDlgItem(h, IDC_SOM_TREE);
            char *dest = so_tree_folder(tree, TreeView_GetSelection(tree), &m->fs);
            char *err = NULL;
            if (!dest) {
                MessageBeep(MB_ICONWARNING);
                return TRUE;
            }
            /* Any box the move raises (the jump-host result) belongs to the
             * Organize window: this one is destroyed right after. */
            if (m->copy ? so_copy_sessions(m->sel, m->nsel, dest, 0, &err) :
                so_move_sessions(m->st ? m->st->h : GetWindow(h, GW_OWNER),
                                 m->sel, m->nsel, dest, &err)) {
                if (m->st)
                    so_reload(m->st);
                DestroyWindow(h);
            } else {
                so_set_warn(h, IDC_SOM_WARN, err, below);
                if (m->st)
                    so_reload(m->st);
                /* Back to the tree for another folder. The click on Move
                 * gave that button the focus and, with it, the default
                 * border; WM_NEXTDLGCTL hands the border back to Cancel,
                 * the template's default. Posted: this runs inside the
                 * button's own click. */
                PostMessage(h, WM_NEXTDLGCTL,
                            (WPARAM)GetDlgItem(h, IDC_SOM_TREE), TRUE);
            }
            sfree(err);
            sfree(dest);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) {
            DestroyWindow(h);
            return TRUE;
        }
        return FALSE;
      case WM_CLOSE:
        DestroyWindow(h);
        return TRUE;
      case WM_DESTROY:
        ShinyRemoveAuxDialog(h);
        if (m) {
            if (m->st && m->st->sub == h)
                m->st->sub = NULL;
            so_move_free(m);
            SetWindowLongPtr(h, GWLP_USERDATA, 0);
        }
        return FALSE;
    }
    return FALSE;
}

/* ---- Rename / New folder ---------------------------------------------- */

#define SO_NAME_SESSION 1
#define SO_NAME_FOLDER  2
#define SO_NAME_NEW     3

struct so_name {
    struct so_state *st;
    int kind;
    char *target;               /* session identity or folder path; NULL for New */
    char *folder_value;         /* a bare session's Folder value */
};

static bool so_name_ok(const char *v)
{
    return v && *v && !strchr(v, '\\') && strcmp(v, "Default Settings") &&
        stricmp(v, "Default");
}

/* Returns NULL when done, else the line to show in the box. */
static char *so_name_apply(HWND h, struct so_name *nm, const char *v)
{
    char *err = NULL;
    if (nm->kind == SO_NAME_SESSION) {
        char *fld = ksp_folder_of(nm->target);
        char *to = fld ? dupprintf("%s\\%s", fld, v) : dupstr(v);
        strbuf *rw = strbuf_new(), *cl = strbuf_new();
        int r;
        if (store_is_file()) {
            char *blk = ksf_path_blocker(to);
            if (blk) {
                err = dupprintf(KT_SP_PATH_BLOCKED, blk);
                sfree(blk);
            }
        }
        if (!err) {
            if (!store_is_file())
                SaveRegistryKeyNow();
            r = kitty_session_rekey(nm->target, to, rw, cl, &err);
            if (r == KITTY_REKEY_OK) {
                /* A session filed by its Folder value only keeps it: a
                 * rename is not an Arrange. */
                if (!fld && !ksp_folder_is_root(nm->folder_value))
                    so_write_folder(to, nm->folder_value);
                so_report_rewritten(nm->st ? nm->st->h : GetParent(h), rw->s);
            } else if (r == KITTY_REKEY_CLASH) {
                sfree(err);
                err = dupprintf(KT_SP_MOVE_CLASH, so_folder_label(fld), cl->s);
            } else if (!err) {
                err = dupstr(KT_MSG_UNKNOWN_ERROR);
            }
        }
        strbuf_free(rw);
        strbuf_free(cl);
        sfree(to);
        sfree(fld);
    } else if (nm->kind == SO_NAME_FOLDER) {
        char *parent = ksp_folder_of(nm->target);
        char *to = parent ? dupprintf("%s\\%s", parent, v) : dupstr(v);
        char *rewritten = NULL;
        int i, exists = 0;
        if (nm->st) {
            for (i = 0; i < nm->st->fs.n; i++)
                if (!stricmp(nm->st->fs.f[i], to) && stricmp(to, nm->target))
                    exists = 1;
        } else {
            /* opened from the session list (kitty_sessorg_rename): the
             * folders as the store has them now */
            struct so_names s;
            struct so_folders fs;
            so_names_load(&s);
            so_folders_load(&fs, &s);
            for (i = 0; i < fs.n; i++)
                if (!stricmp(fs.f[i], to) && stricmp(to, nm->target))
                    exists = 1;
            so_folders_free(&fs);
            so_names_free(&s);
        }
        if (exists) {
            err = dupprintf(KT_SP_ORG_FOLDER_EXISTS, to);
        } else if (kitty_sessorg_folder_move(nm->target, to, KT_SP_MOVE_CLASH,
                                             &err, &rewritten)) {
            kitty_config_session_folder_moved(nm->target, to, 0);
            if (nm->st) {
                sfree(nm->st->cur);
                nm->st->cur = dupstr(to);
            }
            so_report_rewritten(nm->st ? nm->st->h : GetParent(h), rewritten);
        }
        sfree(rewritten);
        sfree(to);
        sfree(parent);
    } else {
        const char *cur = nm->st ? nm->st->cur : "";
        char *to = *cur ? dupprintf("%s\\%s", cur, v) : dupstr(v);
        int i, exists = 0;
        if (nm->st)
            for (i = 0; i < nm->st->fs.n; i++)
                if (!stricmp(nm->st->fs.f[i], to))
                    exists = 1;
        if (exists) {
            err = dupprintf(KT_SP_ORG_FOLDER_EXISTS, to);
        } else {
            char *blk = store_is_file() ? ksf_folder_blocker(to) : NULL;
            if (blk) {
                err = dupprintf(KT_SP_PATH_BLOCKED, blk);
                sfree(blk);
            } else {
                InitFolderList();
                StringList_Add(FolderList, to);
                SaveFolderList();
                if (nm->st) {
                    sfree(nm->st->cur);
                    nm->st->cur = dupstr(to);
                }
            }
        }
        sfree(to);
    }
    return err;
}

static INT_PTR CALLBACK so_name_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    struct so_name *nm = (struct so_name *)GetWindowLongPtr(h, GWLP_USERDATA);
    static const int below[] = { IDOK, IDCANCEL, 0 };
    static const int btns[] = { IDOK, IDCANCEL, 0 };
    static const int row[] = { IDC_SON_LABEL, 0 };
    switch (msg) {
      case WM_INITDIALOG:
        nm = (struct so_name *)lp;
        SetWindowLongPtr(h, GWLP_USERDATA, lp);
        SetWindowTextA(h, nm->kind == SO_NAME_SESSION ? KT_SP_ORG_RENAME_SESSION_CAP :
                          nm->kind == SO_NAME_FOLDER ? KT_SP_ORG_RENAME_FOLDER_CAP :
                          KT_SP_ORG_NEW_FOLDER);
        SetDlgItemTextA(h, IDC_SON_LABEL, KT_SP_ORG_NAME_LABEL);
        SetDlgItemTextA(h, IDOK, KT_SP_ORG_OK);
        SetDlgItemTextA(h, IDCANCEL, KT_SP_ORG_CANCEL);
        if (nm->target)
            SetDlgItemTextA(h, IDC_SON_EDIT, ksp_leaf(nm->target));
        kitty_theme_mark_ink(GetDlgItem(h, IDC_SON_WARN), KITTY_INK_BAD);
        kitty_theme_align_row(h, IDC_SON_EDIT, row);
        so_fit_buttons_right(h, btns);
        kitty_dialog_icon(h, NULL);
        kitty_centre_on_owner(h);
        SetFocus(GetDlgItem(h, IDC_SON_EDIT));
        SendDlgItemMessage(h, IDC_SON_EDIT, EM_SETSEL, 0, -1);
        return FALSE;
      case WM_COMMAND:
        if (!nm)
            return FALSE;
        if (LOWORD(wp) == IDOK) {
            char v[1024], *err;
            GetDlgItemTextA(h, IDC_SON_EDIT, v, sizeof(v));
            str_rtrim(v, " \t");
            if (!so_name_ok(v)) {
                so_set_warn(h, IDC_SON_WARN, KT_SP_ORG_NAME_BAD, below);
                return TRUE;
            }
            if (nm->target && !strcmp(ksp_leaf(nm->target), v)) {
                DestroyWindow(h);       /* nothing changed */
                return TRUE;
            }
            err = so_name_apply(h, nm, v);
            if (nm->st)
                so_changed(nm->st);
            else
                kitty_config_session_store_changed();
            if (err) {
                so_set_warn(h, IDC_SON_WARN, err, below);
                sfree(err);
            } else {
                DestroyWindow(h);
            }
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) {
            DestroyWindow(h);
            return TRUE;
        }
        return FALSE;
      case WM_CLOSE:
        DestroyWindow(h);
        return TRUE;
      case WM_DESTROY:
        ShinyRemoveAuxDialog(h);
        if (nm) {
            if (nm->st && nm->st->sub == h)
                nm->st->sub = NULL;
            sfree(nm->target);
            sfree(nm->folder_value);
            sfree(nm);
            SetWindowLongPtr(h, GWLP_USERDATA, 0);
        }
        return FALSE;
    }
    return FALSE;
}

/* ---- Delete a folder that holds sessions ------------------------------ */

struct so_del {
    char *folder;
    struct so_folders dests;        /* the folders offered: not this one or below */
    void (*done)(int deleted, void *ctx);
    void *ctx;
    int deleted;
    int answered;                   /* Delete or Cancel pressed, or closed */
};

static INT_PTR CALLBACK so_del_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    struct so_del *d = (struct so_del *)GetWindowLongPtr(h, GWLP_USERDATA);
    static const int below[] = { IDOK, IDCANCEL, 0 };
    static const int btns[] = { IDOK, IDCANCEL, 0 };
    switch (msg) {
      case WM_INITDIALOG: {
        struct so_names s;
        int i, nsess = 0, nfold = 1, dh;
        char *head;
        HWND combo = GetDlgItem(h, IDC_SOD_DEST);
        d = (struct so_del *)lp;
        SetWindowLongPtr(h, GWLP_USERDATA, lp);
        SetWindowTextA(h, KT_CAP_KITTYPP);
        so_names_load(&s);
        {
            struct so_folders all;
            so_folders_load(&all, &s);
            memset(&d->dests, 0, sizeof(d->dests));
            for (i = 0; i < all.n; i++) {
                if (ksp_folder_within(all.f[i], d->folder)) {
                    if (stricmp(all.f[i], d->folder))
                        nfold++;
                    continue;
                }
                d->dests.f = sresize(d->dests.f, d->dests.n + 1, char *);
                d->dests.f[d->dests.n++] = dupstr(all.f[i]);
            }
            so_folders_free(&all);
        }
        for (i = 0; i < s.n; i++) {
            char *f = so_effective_folder(s.names[i], s.folders[i]);
            if (*f && ksp_folder_within(f, d->folder) &&
                strcmp(s.names[i], "Default Settings"))
                nsess++;
            sfree(f);
        }
        so_names_free(&s);
        head = nsess == 1 ? dupprintf(KT_SP_DEL_HEAD_ONE, d->folder) :
               nfold > 1 ? dupprintf(KT_SP_DEL_HEAD_SUB, d->folder, nsess, nfold) :
                           dupprintf(KT_SP_DEL_HEAD_MANY, d->folder, nsess);
        SetDlgItemTextA(h, IDC_SOD_HEAD, head);
        SetDlgItemTextA(h, IDC_SOD_DESTLBL, nsess == 1 ? KT_SP_DEL_DEST_ONE : KT_SP_DEL_DEST_MANY);
        SetDlgItemTextA(h, IDC_SOD_KEPT, nsess == 1 ? KT_SP_DEL_KEPT_ONE : KT_SP_DEL_KEPT_MANY);
        SetDlgItemTextA(h, IDOK, KT_SP_DEL_BTN);
        SetDlgItemTextA(h, IDCANCEL, KT_SP_ORG_CANCEL);
        /* The heading may wrap (a long folder name): what is below moves. */
        dh = kitty_fit_text(h, IDC_SOD_HEAD, head, 0);
        sfree(head);
        if (dh) {
            static const int moved[] = { IDC_SOD_DESTLBL, IDC_SOD_DEST, IDC_SOD_KEPT,
                                         IDC_SOD_WARN, IDOK, IDCANCEL, 0 };
            RECT wr;
            for (i = 0; moved[i]; i++) {
                RECT r;
                so_rect(h, moved[i], &r);
                MoveWindow(GetDlgItem(h, moved[i]), r.left, r.top + dh,
                           r.right - r.left, r.bottom - r.top, TRUE);
            }
            GetWindowRect(h, &wr);
            SetWindowPos(h, NULL, 0, 0, wr.right - wr.left, (wr.bottom - wr.top) + dh,
                         SWP_NOMOVE | SWP_NOZORDER);
        }
        SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM)KT_SP_ORG_ROOT);
        for (i = 0; i < d->dests.n; i++)
            SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM)d->dests.f[i]);
        SendMessageA(combo, CB_SETCURSEL, 0, 0);       /* the root, preselected */
        kitty_theme_mark_ink(GetDlgItem(h, IDC_SOD_WARN), KITTY_INK_BAD);
        so_fit_buttons_right(h, btns);
        kitty_dialog_icon(h, NULL);
        kitty_centre_on_owner(h);
        SetFocus(GetDlgItem(h, IDCANCEL));
        return FALSE;
      }
      case WM_COMMAND:
        if (!d)
            return FALSE;
        if (LOWORD(wp) == IDOK) {
            int sel = (int)SendDlgItemMessageA(h, IDC_SOD_DEST, CB_GETCURSEL, 0, 0);
            const char *dest = (sel <= 0 || sel > d->dests.n) ? "Default"
                                                             : d->dests.f[sel - 1];
            char *err = NULL, *rewritten = NULL;
            if (kitty_sessorg_folder_move(d->folder, dest, KT_SP_DEL_CLASH, &err,
                                          &rewritten)) {
                /* The folder itself is gone with its subtree: what was moved
                 * into the destination stays listed there. */
                kitty_config_session_folder_moved(d->folder, dest, 1);
                d->deleted = 1;
                d->answered = 1;
                so_report_rewritten(GetWindow(h, GW_OWNER), rewritten);
                sfree(rewritten);
                kitty_config_session_store_changed();
                if (so_window)
                    so_reload(so_window);
                DestroyWindow(h);
            } else {
                so_set_warn(h, IDC_SOD_WARN, err, below);
                /* Back to the destination list, Cancel the default again
                 * (as in the Move box). */
                PostMessage(h, WM_NEXTDLGCTL,
                            (WPARAM)GetDlgItem(h, IDC_SOD_DEST), TRUE);
            }
            sfree(err);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) {
            d->answered = 1;
            DestroyWindow(h);
            return TRUE;
        }
        return FALSE;
      case WM_CLOSE:
        if (d)
            d->answered = 1;
        DestroyWindow(h);
        return TRUE;
      case WM_DESTROY:
        ShinyRemoveAuxDialog(h);
        if (d) {
            if (so_window && so_window->sub == h)
                so_window->sub = NULL;
            /* Answered only: a box that goes with its owner (the
             * configuration box closing) has no one left to tell. */
            if (d->done && d->answered)
                d->done(d->deleted, d->ctx);
            so_folders_free(&d->dests);
            sfree(d->folder);
            sfree(d);
            SetWindowLongPtr(h, GWLP_USERDATA, 0);
        }
        return FALSE;
    }
    return FALSE;
}

/* The Organize window's rename prompt, opened from the session list (its
 * right-click menu and F2): `target` is a session identity, or a folder path
 * when `folder` is set. The same checks and the same re-keying as there; the
 * open configuration box follows through kitty_config_session_store_changed. */
void kitty_sessorg_rename(HWND owner, int folder, const char *target)
{
    struct so_name *nm;
    if (!target || !*target || GetReadOnlyFlag()) {
        MessageBeep(MB_ICONWARNING);
        return;
    }
    nm = snew(struct so_name);
    memset(nm, 0, sizeof(*nm));
    nm->kind = folder ? SO_NAME_FOLDER : SO_NAME_SESSION;
    nm->target = dupstr(target);
    if (!folder && !strchr(target, '\\'))
        nm->folder_value = kitty_read_session_folder(target);   /* a bare session's */
    if (!so_dialog(IDD_SESSORG_NAME, owner, so_name_proc, nm)) {
        sfree(nm->target);
        sfree(nm->folder_value);
        sfree(nm);
    }
}

void kitty_sessorg_delete_folder(HWND owner, const char *folder,
                                 void (*done)(int deleted, void *ctx),
                                 void *ctx)
{
    struct so_del *d = snew(struct so_del);
    HWND h;
    memset(d, 0, sizeof(*d));
    d->folder = dupstr(folder);
    d->done = done;
    d->ctx = ctx;
    h = so_dialog(IDD_SESSORG_DEL, owner, so_del_proc, d);
    if (!h) {
        sfree(d->folder);
        sfree(d);
        if (done)
            done(0, ctx);
        return;
    }
    if (so_window && owner == so_window->h)
        so_window->sub = h;
}

/* Delete any folder, as Organize's Del folder does: one holding sessions
 * gets the question where they go (kitty_sessorg_delete_folder); an empty one (subfolders
 * included) simply goes, and done(1, ctx) runs at once. */
void kitty_sessorg_delete_any_folder(HWND owner, const char *folder,
                                     void (*done)(int deleted, void *ctx),
                                     void *ctx)
{
    struct so_names s;
    int i, members = 0;
    if (!folder || !*folder || ksp_folder_is_root(folder) || GetReadOnlyFlag()) {
        MessageBeep(MB_ICONWARNING);
        return;
    }
    so_names_load(&s);
    for (i = 0; i < s.n; i++) {
        char *f = so_effective_folder(s.names[i], s.folders[i]);
        if (*f && ksp_folder_within(f, folder) &&
            strcmp(s.names[i], "Default Settings"))
            members++;
        sfree(f);
    }
    so_names_free(&s);
    if (members) {
        kitty_sessorg_delete_folder(owner, folder, done, ctx);
    } else {
        char *gone = dupstr(folder);   /* `folder` may live in a list rebuilt below */
        so_folderlist_move(gone, NULL);
        kitty_config_session_folder_moved(gone, NULL, 1);
        sfree(gone);
        if (so_window)
            so_changed(so_window);
        if (done)
            done(1, ctx);
    }
}

/* ---- Arrange ------------------------------------------------------------ */

struct so_arrange {
    HWND owner;
    int first_start;
};

static void so_arrange_run(HWND owner)
{
    struct so_names s;
    struct ksp_plan p;
    strbuf *rw, *cl;
    char *err = NULL, *msg = NULL;
    int r, n;

    so_names_load(&s);
    ksp_plan_arrange(s.names, s.folders, s.n, &p);
    n = p.n;
    /* The plan holds the free moves only; a session whose path is taken
     * stays where it is and is named in the result. Nothing free: nothing
     * moves. */
    if (!p.n && p.nclash) {
        char *list = so_join(p.clash, p.nclash), *capped = so_cap_list(list);
        msg = dupprintf(KT_SP_ARRANGE_CLASH, capped);
        kitty_info_modeless(owner, KT_CAP_KITTYPP, msg, NULL, NULL);
        sfree(msg);
        sfree(capped);
        sfree(list);
        ksp_plan_free(&p);
        so_names_free(&s);
        return;
    }
    if (!store_is_file())
        SaveRegistryKeyNow();           /* "The registry sessions are backed up first." */
    rw = strbuf_new();
    cl = strbuf_new();
    r = kitty_session_rekey_many(p.from, p.to, p.n, rw, cl, &err);
    if (r == KITTY_REKEY_OK) {
        strbuf *sb = strbuf_new();
        if (n == 1)
            put_dataz(sb, KT_SP_ARRANGE_DONE_ONE);
        else
            put_fmt(sb, KT_SP_ARRANGE_DONE, n);
        if (p.nclash) {
            char *list = so_join(p.clash, p.nclash), *capped = so_cap_list(list);
            put_fmt(sb, KT_SP_ARRANGE_LEFT, capped);
            sfree(capped);
            sfree(list);
        }
        if (rw->len) {
            char *list = so_cap_list(rw->s);
            put_datapl(sb, PTRLEN_LITERAL("\n\n"));
            put_fmt(sb, KT_SP_JUMP_REWRITTEN, list);
            sfree(list);
        }
        kitty_info_modeless(owner, KT_CAP_KITTYPP, sb->s, NULL, NULL);
        strbuf_free(sb);
    } else if (r == KITTY_REKEY_CLASH) {
        char *capped = so_cap_list(cl->s);
        msg = dupprintf(KT_SP_ARRANGE_CLASH, capped);
        kitty_info_modeless(owner, KT_CAP_KITTYPP, msg, NULL, NULL);
        sfree(msg);
        sfree(capped);
    } else {
        kitty_info_modeless(owner, KT_CAP_KITTYPP, err ? err : KT_MSG_UNKNOWN_ERROR,
                            NULL, NULL);
    }
    sfree(err);
    strbuf_free(rw);
    strbuf_free(cl);
    ksp_plan_free(&p);
    so_names_free(&s);
    so_changed(so_window);
}

static void so_arrange_answer(int yes, void *vctx)
{
    struct so_arrange *a = (struct so_arrange *)vctx;
    HWND owner = (a->owner && IsWindow(a->owner)) ? a->owner : NULL;
    if (yes && owner)
        so_arrange_run(owner);
    else if (!yes && a->first_start && owner && IsWindowVisible(owner))
        kitty_info_modeless(owner, KT_CAP_KITTYPP, KT_SP_ARRANGE_LATER_NOTE, NULL, NULL);
    sfree(a);
}

int kitty_sessorg_arrange_pending(void)
{
    return !GetReadOnlyFlag() && kitty_arrange_offer_pending();
}

void kitty_sessorg_arrange(HWND owner, int first_start)
{
    struct so_names s;
    struct ksp_plan p;
    struct so_arrange *a;
    char *q;
    int n, due;

    if (GetReadOnlyFlag()) {
        if (!first_start)
            MessageBeep(MB_ICONWARNING);
        return;
    }
    so_names_load(&s);
    ksp_plan_arrange(s.names, s.folders, s.n, &p);
    n = p.n + p.nclash;                 /* the free ones and the taken ones */
    due = ksp_plan_arrange_offer_due(&p);
    ksp_plan_free(&p);
    so_names_free(&s);
    /* At first start the offer is made only for a taken folder path, and the
     * marker is left unset otherwise: a clash that appears later (a session
     * copied in) is still offered once. */
    if (first_start && !due)
        return;
    if (!n) {
        kitty_info_modeless(owner, KT_CAP_KITTYPP, KT_SP_ARRANGE_NONE, NULL, NULL);
        return;
    }
    if (n == 1)
        q = dupprintf(KT_SP_ARRANGE_Q_ONE "\n\n%s",
                      store_is_file() ? KT_SP_ARRANGE_DIR : KT_SP_ARRANGE_REG);
    else
        q = dupprintf(KT_SP_ARRANGE_Q "\n\n%s", n,
                      store_is_file() ? KT_SP_ARRANGE_DIR : KT_SP_ARRANGE_REG);
    a = snew(struct so_arrange);
    a->owner = owner;
    a->first_start = first_start;
    if (!kitty_confirm_modeless_words(owner, KT_CAP_KITTYPP, q, NULL,
                                      KT_SP_ARRANGE_NOW, KT_SP_ARRANGE_LATER,
                                      so_arrange_answer, a))
        sfree(a);                       /* not shown: offered again next time */
    else if (first_start)
        kitty_arrange_offer_made();     /* offered once per store */
    sfree(q);
}

/* ---- the window itself -------------------------------------------------- */

static void so_drop_on(struct so_state *st, HTREEITEM target)
{
    char *dest = so_tree_folder(st->tree, target, &st->fs);
    char **sel, *err = NULL;
    int nsel, i;
    if (!dest)
        return;
    sel = so_selected(st, &nsel);
    if (nsel && !ksp_folder_same(*dest ? dest : NULL, *st->cur ? st->cur : NULL)) {
        char **copy = snewn(nsel, char *);
        for (i = 0; i < nsel; i++)
            copy[i] = dupstr(sel[i]);
        if (!so_move_sessions(st->h, copy, nsel, dest, &err) && err)
            kitty_info_modeless(st->h, KT_CAP_KITTYPP, err, NULL, NULL);
        for (i = 0; i < nsel; i++)
            sfree(copy[i]);
        sfree(copy);
        so_reload(st);
    }
    sfree(err);
    sfree(sel);
    sfree(dest);
}

/* ---- the right-click menus' own actions --------------------------------
 * The menus carry the buttons' actions (Move to..., Rename..., New folder,
 * Delete folder) and these, which have no button: Start Sessions, Copy
 * to..., Clone, Delete for sessions, Select All. */
#define SO_CMD_START        0x7101
#define SO_CMD_COPY         0x7102
#define SO_CMD_CLONE        0x7103
#define SO_CMD_DELSESS      0x7104
#define SO_CMD_SELALL       0x7105
#define SO_CMD_RENAME_SESS  0x7106
#define SO_CMD_RENAME_FOLD  0x7107

/* Sessions in their own terminals: kitty.exe -load "<name>" each. */
static void so_start_named(char **names, int n_names)
{
    char exe[MAX_PATH];
    int i;
    DWORD n = GetModuleFileNameA(NULL, exe, sizeof(exe));
    if (!n_names || !n || n >= sizeof(exe)) {
        MessageBeep(MB_ICONWARNING);
        return;
    }
    for (i = 0; i < n_names; i++) {
        char *cmd = dupprintf("\"%s\" -load \"%s\"", exe, names[i]);
        STARTUPINFOA si;
        PROCESS_INFORMATION pi;
        memset(&si, 0, sizeof(si));
        si.cb = sizeof(si);
        memset(&pi, 0, sizeof(pi));
        if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        sfree(cmd);
    }
}

/* Start Sessions and Enter: every selected session. */
static void so_start_sessions(struct so_state *st)
{
    int nsel;
    char **sel = so_selected(st, &nsel);
    so_start_named(sel, nsel);
    sfree(sel);
}

static void so_clone(struct so_state *st)
{
    char **sel, *err = NULL;
    int nsel;
    sel = so_selected(st, &nsel);
    if (!nsel || GetReadOnlyFlag()) {
        MessageBeep(MB_ICONWARNING);
        sfree(sel);
        return;
    }
    {
        /* copies: the reload below frees the names `sel` borrows */
        char **copy = snewn(nsel, char *);
        int i;
        for (i = 0; i < nsel; i++)
            copy[i] = dupstr(sel[i]);
        if (!so_copy_sessions(copy, nsel, NULL, 1, &err) && err)
            kitty_info_modeless(st->h, KT_CAP_KITTYPP, err, NULL, NULL);
        for (i = 0; i < nsel; i++)
            sfree(copy[i]);
        sfree(copy);
    }
    sfree(err);
    sfree(sel);
    so_reload(st);
}

/* Delete for sessions: a confirmation first, then each one deleted. */
struct so_delsess {
    char **names;
    int n;
};
static void so_delete_sessions_answer(int yes, void *ctx)
{
    struct so_delsess *d = (struct so_delsess *)ctx;
    int i;
    if (yes && !GetReadOnlyFlag()) {
        SaveRegistryKeyNow();           /* the backup, before the delete */
        for (i = 0; i < d->n; i++)
            del_settings(d->names[i]);
        kitty_store_mark_dirty();
        so_changed(so_window);
    }
    for (i = 0; i < d->n; i++)
        sfree(d->names[i]);
    sfree(d->names);
    sfree(d);
}
static void so_delete_sessions(struct so_state *st)
{
    char **sel, *q;
    int nsel, i;
    struct so_delsess *d;
    sel = so_selected(st, &nsel);
    if (!nsel || GetReadOnlyFlag()) {
        MessageBeep(MB_ICONWARNING);
        sfree(sel);
        return;
    }
    d = snew(struct so_delsess);
    d->n = nsel;
    d->names = snewn(nsel, char *);
    for (i = 0; i < nsel; i++)
        d->names[i] = dupstr(sel[i]);
    q = nsel == 1 ? dupprintf(KT_SP_DEL_SESSION_Q, ksp_leaf(sel[0]))
                  : dupprintf(KT_SP_ORG_DEL_Q, nsel);
    if (!kitty_confirm_modeless_words(st->h, KT_CAP_KITTYPP, q, NULL,
                                      KT_SP_ORG_M_DELETE, KT_SP_ORG_CANCEL,
                                      so_delete_sessions_answer, d)) {
        for (i = 0; i < nsel; i++)
            sfree(d->names[i]);
        sfree(d->names);
        sfree(d);
    }
    sfree(q);
    sfree(sel);
}

/* ---- dragging: what the pointer carries, where it would drop -----------
 * A session drag starts in the list, a folder drag in the tree; both drop
 * on a folder of the tree. The pointer carries an image of what is dragged
 * (the session's name, or "N sessions", or the folder's name); the folder it
 * would land in is highlighted, a closed one opens after a short hover, and
 * the tree scrolls at its top and bottom edge. Esc ends a drag. */
#define SO_DRAG_SESSIONS    1
#define SO_DRAG_FOLDER      2
#define SO_TIMER_DRAG       0x5D01
#define SO_HOVER_OPEN_MS    700

static HIMAGELIST so_drag_image(HWND from, const char *text)
{
    HDC sdc = GetDC(from), mdc;
    HFONT f = (HFONT)SendMessage(from, WM_GETFONT, 0, 0), of = NULL;
    SIZE sz;
    HBITMAP bm, obm;
    HIMAGELIST il;
    RECT r;
    bool dark = kitty_theme_window_dark(GetAncestor(from, GA_ROOT));
    COLORREF back = dark ? kitty_theme_row_colour(true, false) : GetSysColor(COLOR_HIGHLIGHT);
    COLORREF ink = dark ? kitty_theme_text_colour(true) : GetSysColor(COLOR_HIGHLIGHTTEXT);
    if (!sdc)
        return NULL;
    mdc = CreateCompatibleDC(sdc);
    if (f)
        of = SelectObject(mdc, f);
    GetTextExtentPoint32A(mdc, text, (int)strlen(text), &sz);
    r.left = r.top = 0;
    r.right = sz.cx + 12;
    r.bottom = sz.cy + 6;
    bm = CreateCompatibleBitmap(sdc, r.right, r.bottom);
    obm = SelectObject(mdc, bm);
    {
        HBRUSH b = CreateSolidBrush(back);
        FillRect(mdc, &r, b);
        DeleteObject(b);
    }
    SetBkMode(mdc, TRANSPARENT);
    SetTextColor(mdc, ink);
    TextOutA(mdc, 6, 3, text, (int)strlen(text));
    SelectObject(mdc, obm);
    if (of)
        SelectObject(mdc, of);
    DeleteDC(mdc);
    ReleaseDC(from, sdc);
    il = ImageList_Create(r.right, r.bottom, ILC_COLOR32, 1, 0);
    if (il)
        ImageList_Add(il, bm, NULL);
    DeleteObject(bm);
    return il;
}

static void so_drag_begin(struct so_state *st, int kind, const char *text)
{
    POINT pt;
    st->dragging = kind;
    st->hover = NULL;
    st->hover_since = 0;
    st->dragimg = so_drag_image(kind == SO_DRAG_FOLDER ? st->tree : st->list, text);
    SetCapture(st->h);
    if (st->dragimg && ImageList_BeginDrag(st->dragimg, 0, -12, -8)) {
        GetCursorPos(&pt);
        ImageList_DragEnter(NULL, pt.x, pt.y);
    }
    SetTimer(st->h, SO_TIMER_DRAG, 100, NULL);
}

/* May `target` take what is dragged? Not the folder the sessions are in,
 * not a dragged folder itself, its own subfolders or its present parent. */
static int so_drop_ok(struct so_state *st, HTREEITEM target)
{
    char *dest;
    int ok;
    if (!target)
        return 0;
    dest = so_tree_folder(st->tree, target, &st->fs);
    if (!dest)
        return 0;
    if (st->dragging == SO_DRAG_FOLDER) {
        char *parent = ksp_folder_parent(st->dragfolder);
        ok = !(*dest && ksp_folder_within(dest, st->dragfolder)) &&
             !ksp_folder_same(*dest ? dest : NULL,
                              ksp_folder_is_root(parent) ? NULL : parent);
        sfree(parent);
    } else {
        ok = !ksp_folder_same(*dest ? dest : NULL, *st->cur ? st->cur : NULL);
    }
    sfree(dest);
    return ok;
}

static void so_drag_track(struct so_state *st)
{
    TVHITTESTINFO ht;
    POINT pt, sp;
    HTREEITEM over;
    RECT tr;
    int ok;
    GetCursorPos(&sp);
    pt = sp;
    ScreenToClient(st->tree, &pt);
    memset(&ht, 0, sizeof(ht));
    ht.pt = pt;
    over = TreeView_HitTest(st->tree, &ht);
    if (!(ht.flags & (TVHT_ONITEM | TVHT_ONITEMBUTTON | TVHT_ONITEMINDENT |
                      TVHT_ONITEMRIGHT)))
        over = NULL;
    ok = so_drop_ok(st, over);
    if (over != st->hover) {
        st->hover = over;
        st->hover_since = GetTickCount();
    }
    ImageList_DragShowNolock(FALSE);
    TreeView_SelectDropTarget(st->tree, ok ? over : NULL);
    UpdateWindow(st->tree);
    ImageList_DragShowNolock(TRUE);
    ImageList_DragMove(sp.x, sp.y);
    SetCursor(LoadCursor(NULL, ok ? IDC_ARROW : IDC_NO));
    /* at the tree's top or bottom edge it scrolls */
    GetClientRect(st->tree, &tr);
    if (PtInRect(&tr, pt) || (pt.x >= tr.left && pt.x < tr.right)) {
        int edge = GetSystemMetrics(SM_CYVSCROLL);
        if (pt.y < tr.top + edge || pt.y >= tr.bottom - edge) {
            ImageList_DragShowNolock(FALSE);
            SendMessage(st->tree, WM_VSCROLL,
                        pt.y < tr.top + edge ? SB_LINEUP : SB_LINEDOWN, 0);
            UpdateWindow(st->tree);
            ImageList_DragShowNolock(TRUE);
        }
    }
}

/* A dropped folder moves only after a confirmation (a drop is easily made by
 * accident); the answer does the move, from and to as they were then. */
struct so_folderdrop {
    char *from, *to;
};
static void so_folder_drop_answer(int yes, void *ctx)
{
    struct so_folderdrop *d = (struct so_folderdrop *)ctx;
    struct so_state *st = so_window;
    if (yes && !GetReadOnlyFlag()) {
        char *err = NULL, *rewritten = NULL;
        if (kitty_sessorg_folder_move(d->from, d->to, KT_SP_MOVE_CLASH,
                                      &err, &rewritten)) {
            kitty_config_session_folder_moved(d->from, d->to, 0);
            if (st && ksp_folder_within(st->cur, d->from)) {
                char *np = ksp_folder_moved_path(st->cur, d->from, d->to);
                sfree(st->cur);
                st->cur = np;
            }
            so_report_rewritten(st ? st->h : NULL, rewritten);
        } else if (err) {
            kitty_info_modeless(st ? st->h : NULL, KT_CAP_KITTYPP, err, NULL, NULL);
        }
        sfree(err);
        sfree(rewritten);
        so_changed(st);
    }
    sfree(d->from);
    sfree(d->to);
    sfree(d);
}

static void so_drag_end(struct so_state *st, int drop)
{
    HTREEITEM target = TreeView_GetDropHilight(st->tree);
    int kind = st->dragging;
    st->dragging = 0;
    KillTimer(st->h, SO_TIMER_DRAG);
    if (st->dragimg) {
        ImageList_DragLeave(NULL);
        ImageList_EndDrag();
        ImageList_Destroy(st->dragimg);
        st->dragimg = NULL;
    }
    TreeView_SelectDropTarget(st->tree, NULL);
    if (GetCapture() == st->h)
        ReleaseCapture();
    if (drop && target) {
        if (kind == SO_DRAG_FOLDER) {
            char *dest = so_tree_folder(st->tree, target, &st->fs);
            if (dest && st->dragfolder) {
                struct so_folderdrop *d = snew(struct so_folderdrop);
                char *q;
                d->from = dupstr(st->dragfolder);
                d->to = *dest ? dupprintf("%s\\%s", dest, ksp_leaf(st->dragfolder))
                              : dupstr(ksp_leaf(st->dragfolder));
                q = dupprintf(KT_SP_ORG_FOLDER_DROP_Q, st->dragfolder,
                              so_folder_label(dest));
                if (!kitty_confirm_modeless_words(st->h, KT_CAP_KITTYPP, q, NULL,
                                                  KT_SP_ORG_FOLDER_DROP_BTN,
                                                  KT_SP_ORG_CANCEL,
                                                  so_folder_drop_answer, d)) {
                    sfree(d->from);
                    sfree(d->to);
                    sfree(d);
                }
                sfree(q);
            }
            sfree(dest);
        } else {
            so_drop_on(st, target);
        }
    }
    sfree(st->dragfolder);
    st->dragfolder = NULL;
}

/* the drag timer: a closed folder opens after a hover; Esc ends the drag */
static void so_drag_tick(struct so_state *st)
{
    if (!st->dragging)
        return;
    if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
        so_drag_end(st, 0);
        return;
    }
    so_drag_track(st);
    if (st->hover && GetTickCount() - st->hover_since >= SO_HOVER_OPEN_MS &&
        TreeView_GetChild(st->tree, st->hover)) {
        TVITEMA tv;
        memset(&tv, 0, sizeof(tv));
        tv.mask = TVIF_STATE;
        tv.stateMask = TVIS_EXPANDED;
        tv.hItem = st->hover;
        if (SendMessageA(st->tree, TVM_GETITEMA, 0, (LPARAM)&tv) &&
            !(tv.state & TVIS_EXPANDED)) {
            ImageList_DragShowNolock(FALSE);
            TreeView_Expand(st->tree, st->hover, TVE_EXPAND);
            UpdateWindow(st->tree);
            ImageList_DragShowNolock(TRUE);
        }
    }
}

/* ---- Edit (kitty.exe -manage) -------------------------------------------
 * The configuration window, in this process, on the selected session (loaded,
 * in its folder), else on the selected folder, else on the root: it opens
 * there because the last folder and session are set first, as a Load there
 * would have left them. Open starts the session in a new process (RunConfig,
 * the hand-off Start uses) and comes back here. */
static void so_edit(struct so_state *st)
{
    Conf *conf;
    char **sel, *name = NULL, *folder = NULL;
    int nsel, i, open, focus_list;
    if (so_editing) {
        MessageBeep(MB_ICONWARNING);    /* the one already open stays in front */
        return;
    }
    sel = so_selected(st, &nsel);
    if (nsel >= 1) {
        name = dupstr(sel[0]);
        for (i = 0; i < st->s.n; i++)
            if (!strcmp(st->s.names[i], name)) {
                folder = so_effective_folder(name, st->s.folders[i]);
                break;
            }
    }
    sfree(sel);
    if (!folder)
        folder = dupstr(st->cur ? st->cur : "");
    kitty_set_last_folder(*folder ? folder : "Default");
    kitty_set_last_session(name ? name : "");
    conf = conf_new();
    do_defaults(name, conf);
    if (name) {
        /* loaded, not only selected: the box shows the name, the session
         * label reads it as loaded, and Save writes it back without the
         * overwrite warning - as the hotkey balloon's -cfgloaded does */
        extern void kitty_cfgbox_open_loaded(void);
        conf_set_str(conf, CONF_sessionname, name);
        kitty_cfgbox_open_loaded();
    }
    so_editing = 1;
    /* afterwards the focus goes to the edited session in the list (or back
     * to the list or the tree, wherever it was when no session was chosen) */
    focus_list = name != NULL || st->pane == 1;
    EnableWindow(GetDlgItem(st->h, IDC_SO_EDIT), FALSE);
    open = do_config(conf);             /* modal; this window keeps working */
    so_editing = 0;
    if (open && conf_launchable(conf))
        RunConfig(conf);
    conf_free(conf);
    if (so_window) {
        struct so_state *w = so_window;
        EnableWindow(GetDlgItem(w->h, IDC_SO_EDIT), TRUE);
        so_reload(w);                   /* a save or a delete there */
        if (name)
            so_select_session(w, name); /* the reload cleared the selection */
        SetForegroundWindow(w->h);
        /* WM_NEXTDLGCTL, not SetFocus: the dialog manager then keeps the
         * default button and its own focus memory right */
        PostMessage(w->h, WM_NEXTDLGCTL,
                    (WPARAM)(focus_list ? w->list : w->tree), TRUE);
    }
    sfree(name);
    sfree(folder);
    if (!so_window && so_quit_after_edit) {
        PostQuitMessage(0);
    }
}

/* kitty.exe -manage: Manage Sessions alone, without a configuration window
 * behind it; closing it ends the process. */
int kitty_manage_WinMain(void)
{
    MSG msg;
    so_manage_mode = 1;
    kitty_sessorg_open(NULL);
    if (!so_window)
        return 1;
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        if (!ShinyAuxDialogMessage(&msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }
    return 0;
}

/* ---- the right-click menus --------------------------------------------- */

static void so_menu_add(HMENU m, UINT id, const char *text, int enabled)
{
    AppendMenuA(m, MF_STRING | (enabled ? 0 : MF_GRAYED), id, text);
}

static void so_command(struct so_state *st, int id);

static void so_list_menu(struct so_state *st, int x, int y)
{
    HMENU m = CreatePopupMenu();
    int nsel = ListView_GetSelectedCount(st->list), cmd;
    int rw = !GetReadOnlyFlag();
    if (!m)
        return;
    if (x == -1 && y == -1) {           /* the menu key: at the focused row */
        RECT r;
        int i = ListView_GetNextItem(st->list, -1, LVNI_FOCUSED);
        POINT p = {8, 8};
        if (i >= 0 && ListView_GetItemRect(st->list, i, &r, LVIR_LABEL)) {
            p.x = r.left + 8;
            p.y = r.bottom;
        }
        ClientToScreen(st->list, &p);
        x = p.x;
        y = p.y;
    }
    so_menu_add(m, SO_CMD_START, KT_SP_ORG_M_START, nsel > 0);
    AppendMenuA(m, MF_SEPARATOR, 0, NULL);
    so_menu_add(m, IDC_SO_MOVE, KT_SP_ORG_M_MOVE, nsel > 0 && rw);
    so_menu_add(m, SO_CMD_COPY, KT_SP_ORG_M_COPY, nsel > 0 && rw);
    so_menu_add(m, SO_CMD_RENAME_SESS, KT_SP_ORG_M_RENAME, nsel == 1 && rw);
    so_menu_add(m, SO_CMD_CLONE, KT_SP_ORG_M_CLONE, nsel > 0 && rw);
    so_menu_add(m, SO_CMD_DELSESS, KT_SP_ORG_M_DELETE, nsel > 0 && rw);
    AppendMenuA(m, MF_SEPARATOR, 0, NULL);
    so_menu_add(m, SO_CMD_SELALL, KT_SP_ORG_M_SELALL, ListView_GetItemCount(st->list) > 0);
    cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, x, y, 0, st->h, NULL);
    DestroyMenu(m);
    if (cmd)
        so_command(st, cmd);
}

static void so_tree_menu(struct so_state *st, int x, int y)
{
    HMENU m;
    int cmd, rw = !GetReadOnlyFlag();
    if (x == -1 && y == -1) {
        RECT r;
        HTREEITEM it = TreeView_GetSelection(st->tree);
        POINT p = {8, 8};
        if (it && TreeView_GetItemRect(st->tree, it, &r, TRUE)) {
            p.x = r.left + 8;
            p.y = r.bottom;
        }
        ClientToScreen(st->tree, &p);
        x = p.x;
        y = p.y;
    } else {
        /* the folder under the pointer becomes the one acted on */
        TVHITTESTINFO ht;
        HTREEITEM it;
        memset(&ht, 0, sizeof(ht));
        ht.pt.x = x;
        ht.pt.y = y;
        ScreenToClient(st->tree, &ht.pt);
        it = TreeView_HitTest(st->tree, &ht);
        if (!it)
            return;
        TreeView_SelectItem(st->tree, it);
    }
    m = CreatePopupMenu();
    if (!m)
        return;
    so_menu_add(m, IDC_SO_NEWFOLDER, KT_SP_ORG_M_NEWFOLDER, rw);
    so_menu_add(m, SO_CMD_RENAME_FOLD, KT_SP_ORG_M_RENAME, *st->cur && rw);
    so_menu_add(m, IDC_SO_DELFOLDER, KT_SP_ORG_M_DELETE, *st->cur && rw);
    cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, x, y, 0, st->h, NULL);
    DestroyMenu(m);
    if (cmd)
        so_command(st, cmd);
}

/* The list gets the focus with a session to act on: the first one selected
 * and focused when none is selected yet (a Tab in, or Enter in the tree). */
static void so_list_take_focus(struct so_state *st, int move_focus)
{
    if (ListView_GetItemCount(st->list) > 0 &&
        ListView_GetSelectedCount(st->list) == 0) {
        ListView_SetItemState(st->list, 0, LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(st->list, 0, FALSE);
    }
    if (move_focus)
        PostMessage(st->h, WM_NEXTDLGCTL, (WPARAM)st->list, TRUE);
}

static void so_command(struct so_state *st, int id)
{
    switch (id) {                       /* the menu actions with no box */
      case IDOK:
        /* Enter (no button is the default: Close was, and Enter closed the
         * window): in the list it starts the selected sessions, in the tree
         * it moves to the folder's first session */
        if (GetFocus() == st->list)
            so_start_sessions(st);
        else if (GetFocus() == st->tree)
            so_list_take_focus(st, 1);
        return;
      case SO_CMD_START:
        so_start_sessions(st);
        return;
      case SO_CMD_SELALL:
        ListView_SetItemState(st->list, -1, LVIS_SELECTED, LVIS_SELECTED);
        return;
      case SO_CMD_RENAME_SESS:
        st->pane = 1;
        id = IDC_SO_RENAME;
        break;
      case SO_CMD_RENAME_FOLD:
        st->pane = 0;
        id = IDC_SO_RENAME;
        break;
    }
    if (st->sub && IsWindow(st->sub) && id != IDCANCEL) {
        /* One box at a time: the one already open comes to the front. */
        SetForegroundWindow(st->sub);
        return;
    }
    switch (id) {
      case SO_CMD_CLONE:
        so_clone(st);
        return;
      case SO_CMD_DELSESS:
        so_delete_sessions(st);
        return;
      case IDC_SO_MOVE:
      case SO_CMD_COPY: {
        int nsel, i;
        char **sel = so_selected(st, &nsel);
        struct so_move *m;
        if (!nsel) {
            MessageBeep(MB_ICONWARNING);
            sfree(sel);
            return;
        }
        m = snew(struct so_move);
        memset(m, 0, sizeof(*m));
        m->st = st;
        m->copy = (id == SO_CMD_COPY);
        m->nsel = nsel;
        m->sel = snewn(nsel, char *);
        for (i = 0; i < nsel; i++)
            m->sel[i] = dupstr(sel[i]);
        sfree(sel);
        so_folders_load(&m->fs, &st->s);
        st->sub = so_dialog(IDD_SESSORG_MOVE, st->h, so_move_proc, m);
        if (!st->sub)
            so_move_free(m);
        return;
      }
      case IDC_SO_RENAME:
      case IDC_SO_NEWFOLDER: {
        struct so_name *nm = snew(struct so_name);
        memset(nm, 0, sizeof(*nm));
        nm->st = st;
        if (id == IDC_SO_NEWFOLDER) {
            nm->kind = SO_NAME_NEW;
        } else {
            int nsel, i;
            char **sel = so_selected(st, &nsel);
            if (st->pane == 1 && nsel == 1) {
                nm->kind = SO_NAME_SESSION;
                nm->target = dupstr(sel[0]);
                for (i = 0; i < st->s.n; i++)
                    if (!strcmp(st->s.names[i], sel[0]) && st->s.folders[i])
                        nm->folder_value = dupstr(st->s.folders[i]);
            } else if (st->pane == 0 && *st->cur) {
                nm->kind = SO_NAME_FOLDER;
                nm->target = dupstr(st->cur);
            }
            sfree(sel);
            if (!nm->kind) {
                MessageBeep(MB_ICONWARNING);
                sfree(nm);
                return;
            }
        }
        st->sub = so_dialog(IDD_SESSORG_NAME, st->h, so_name_proc, nm);
        if (!st->sub) {
            sfree(nm->target);
            sfree(nm->folder_value);
            sfree(nm);
        }
        return;
      }
      case IDC_SO_DELFOLDER: {
        int i, members = 0;
        if (!*st->cur) {
            MessageBeep(MB_ICONWARNING);
            return;
        }
        for (i = 0; i < st->s.n; i++) {
            char *f = so_effective_folder(st->s.names[i], st->s.folders[i]);
            if (*f && ksp_folder_within(f, st->cur) &&
                strcmp(st->s.names[i], "Default Settings"))
                members++;
            sfree(f);
        }
        if (members) {
            kitty_sessorg_delete_folder(st->h, st->cur, NULL, NULL);
        } else {
            /* Empty (subfolders included): it simply goes, as the session
             * list's Del folder does it. */
            char *up = ksp_folder_parent(st->cur);
            so_folderlist_move(st->cur, NULL);
            kitty_config_session_folder_moved(st->cur, NULL, 1);
            sfree(st->cur);
            st->cur = ksp_folder_is_root(up) ? dupstr("") : dupstr(up);
            sfree(up);
            so_changed(st);
        }
        return;
      }
      case IDC_SO_ARRANGE:
        kitty_sessorg_arrange(st->h, 0);
        return;
      case IDC_SO_EDIT:
        if (so_manage_mode)
            so_edit(st);
        return;
      case IDCANCEL:
        DestroyWindow(st->h);
        return;
    }
}

static INT_PTR CALLBACK so_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    struct so_state *st = (struct so_state *)GetWindowLongPtr(h, GWLP_USERDATA);
    switch (msg) {
      case WM_INITDIALOG: {
        static const int btns[] = { IDC_SO_MOVE, IDC_SO_RENAME, IDC_SO_NEWFOLDER,
                                    IDC_SO_DELFOLDER, IDC_SO_ARRANGE, 0 };
        LVCOLUMNA col;
        RECT a, b, cr, wr, r;
        int i, x, gap, w;
        HDWP dwp;
        st = (struct so_state *)lp;
        SetWindowLongPtr(h, GWLP_USERDATA, lp);
        st->h = h;
        st->tree = GetDlgItem(h, IDC_SO_TREE);
        st->list = GetDlgItem(h, IDC_SO_LIST);
        SetWindowTextA(h, KT_SP_ORG_TITLE);
        SetDlgItemTextA(h, IDC_SO_MOVE, KT_SP_ORG_BTN_MOVE);
        SetDlgItemTextA(h, IDC_SO_RENAME, KT_SP_ORG_BTN_RENAME);
        SetDlgItemTextA(h, IDC_SO_NEWFOLDER, KT_SP_ORG_BTN_NEW_FOLDER);
        SetDlgItemTextA(h, IDC_SO_DELFOLDER, KT_SP_ORG_BTN_DEL_FOLDER);
        SetDlgItemTextA(h, IDC_SO_ARRANGE, KT_SP_ORG_BTN_ARRANGE);
        SetDlgItemTextA(h, IDCANCEL, KT_SP_ORG_BTN_CLOSE);
        /* its own icon: the KiTTY++ icon with a gear in front (kitty.rc) */
        {
            HINSTANCE inst = GetModuleHandle(NULL);
            HICON big = (HICON)LoadImage(inst, MAKEINTRESOURCE(IDI_MANAGEICON), IMAGE_ICON,
                                         GetSystemMetrics(SM_CXICON),
                                         GetSystemMetrics(SM_CYICON), LR_SHARED);
            HICON sm = (HICON)LoadImage(inst, MAKEINTRESOURCE(IDI_MANAGEICON), IMAGE_ICON,
                                        GetSystemMetrics(SM_CXSMICON),
                                        GetSystemMetrics(SM_CYSMICON), LR_SHARED);
            if (big && sm) {
                SendMessage(h, WM_SETICON, ICON_BIG, (LPARAM)big);
                SendMessage(h, WM_SETICON, ICON_SMALL, (LPARAM)sm);
            } else {
                kitty_dialog_icon(h, NULL);
            }
        }

        /* The foot: the five actions from the left, Close at the right,
         * every button as wide as its caption, in one transaction. */
        so_rect(h, IDC_SO_MOVE, &a);
        so_rect(h, IDC_SO_RENAME, &b);
        gap = b.left - a.right;
        GetClientRect(h, &cr);
        dwp = BeginDeferWindowPos(6);
        for (i = 0, x = a.left; btns[i]; i++) {
            so_rect(h, btns[i], &r);
            w = kitty_theme_button_width(GetDlgItem(h, btns[i]), r.right - r.left);
            if (dwp)
                dwp = DeferWindowPos(dwp, GetDlgItem(h, btns[i]), NULL, x, r.top,
                                     w, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
            x += w + gap;
        }
        so_rect(h, IDCANCEL, &r);
        w = kitty_theme_button_width(GetDlgItem(h, IDCANCEL), r.right - r.left);
        GetWindowRect(h, &wr);
        if (x + w + a.left > cr.right) {
            /* wider than the template allowed for: the window grows */
            SetWindowPos(h, NULL, 0, 0, (wr.right - wr.left) + (x + w + a.left - cr.right),
                         wr.bottom - wr.top, SWP_NOMOVE | SWP_NOZORDER);
            GetClientRect(h, &cr);
        }
        if (dwp)
            dwp = DeferWindowPos(dwp, GetDlgItem(h, IDCANCEL), NULL,
                                 cr.right - a.left - w, r.top, w, r.bottom - r.top,
                                 SWP_NOZORDER | SWP_NOACTIVATE);
        if (dwp)
            EndDeferWindowPos(dwp);
        /* -manage: Edit, left of Close (hidden otherwise: there the
         * configuration window is the one this window was opened from) */
        if (so_manage_mode) {
            HWND eb = GetDlgItem(h, IDC_SO_EDIT);
            RECT er;
            int we;
            SetWindowTextA(eb, KT_SP_ORG_BTN_EDIT);
            so_rect(h, IDC_SO_EDIT, &er);
            we = kitty_theme_button_width(eb, er.right - er.left);
            MoveWindow(eb, cr.right - a.left - w - gap - we, r.top, we,
                       r.bottom - r.top, TRUE);
            ShowWindow(eb, SW_SHOW);
        }

        ListView_SetExtendedListViewStyle(st->list, LVS_EX_FULLROWSELECT);
        memset(&col, 0, sizeof(col));
        col.mask = LVCF_WIDTH;
        col.cx = 100;
        SendMessageA(st->list, LVM_INSERTCOLUMNA, 0, (LPARAM)&col);

        /* the folder and the session it was left on (a folder gone since
         * falls back to the root in so_reload) */
        {
            char f[1024];
            st->cur = dupstr(kitty_state_get_string(KR_MANAGE_FOLDER, f, sizeof(f)) &&
                             !ksp_folder_is_root(f) ? f : "");
        }
        so_reload(st);
        {
            char sn[1024];
            if (kitty_state_get_string(KR_MANAGE_SESSION, sn, sizeof(sn)))
                so_select_session(st, sn);
        }

        {
            SIZE ignore;
            anchored_capture(h, so_anchors, lenof(so_anchors), st->rects,
                             &st->basesize, &ignore);
            GetWindowRect(h, &wr);
            st->minsize.cx = wr.right - wr.left;
            st->minsize.cy = (wr.bottom - wr.top) * 2 / 3;
            st->ready = 1;
        }
        /* where it was on this monitor layout, fully visible; the first
         * time, over its owner */
        if (!kitty_auxpos_restore(h, KR_DLGPOS_MANAGE, 1, st->minsize.cx,
                                  st->minsize.cy))
            kitty_centre_on_owner(h);
        SetFocus(st->tree);
        return FALSE;
      }
      case WM_SIZE:
        if (st && st->ready && wp != SIZE_MINIMIZED) {
            anchored_relayout(h, so_anchors, lenof(so_anchors), st->rects,
                              st->basesize);
            ListView_SetColumnWidth(st->list, 0, LVSCW_AUTOSIZE_USEHEADER);
        }
        return TRUE;
      case WM_GETMINMAXINFO:
        if (st && st->ready) {
            MINMAXINFO *mmi = (MINMAXINFO *)lp;
            mmi->ptMinTrackSize.x = st->minsize.cx;
            mmi->ptMinTrackSize.y = st->minsize.cy;
        }
        return TRUE;
      case WM_ACTIVATE:
        /* Back from the configuration box (a save, a delete there): the
         * store may have changed under this window. */
        if (st && st->ready && LOWORD(wp) != WA_INACTIVE && !st->dragging &&
            !(st->sub && IsWindow(st->sub)) &&
            kitty_store_generation() != st->gen)
            so_reload(st);
        return FALSE;
      case WM_NOTIFY: {
        NMHDR *nm = (NMHDR *)lp;
        if (!st || !nm)
            return FALSE;
        if (nm->idFrom == IDC_SO_TREE) {
            if (nm->code == TVN_SELCHANGEDA || nm->code == TVN_SELCHANGEDW) {
                NMTREEVIEWA *tv = (NMTREEVIEWA *)lp;
                char *f = so_tree_folder(st->tree, tv->itemNew.hItem, &st->fs);
                if (f) {
                    sfree(st->cur);
                    st->cur = f;
                    so_fill_list(st);
                }
            } else if (nm->code == NM_SETFOCUS) {
                st->pane = 0;
            } else if ((nm->code == TVN_BEGINDRAGA || nm->code == TVN_BEGINDRAGW) &&
                       !GetReadOnlyFlag()) {
                /* Drag a folder into another one (not the root row). */
                NMTREEVIEWA *tv = (NMTREEVIEWA *)lp;
                char *f = so_tree_folder(st->tree, tv->itemNew.hItem, &st->fs);
                if (f && *f) {
                    st->dragfolder = f;
                    so_drag_begin(st, SO_DRAG_FOLDER, ksp_leaf(f));
                } else {
                    sfree(f);
                }
            }
        } else if (nm->idFrom == IDC_SO_LIST) {
            if (nm->code == NM_SETFOCUS) {
                /* Tab into the list: the first session is selected and
                 * focused when none is selected yet - a focus frame alone
                 * left Edit and the menus with nothing to act on */
                st->pane = 1;
                so_list_take_focus(st, 0);
                InvalidateRect(st->list, NULL, TRUE);
            } else if (nm->code == NM_KILLFOCUS) {
                if (ListView_GetItemCount(st->list) == 0)
                    InvalidateRect(st->list, NULL, TRUE);
            } else if (nm->code == NM_CUSTOMDRAW &&
                       ListView_GetItemCount(st->list) == 0) {
                /* an empty list shows a line stating it, and the focus frame */
                NMLVCUSTOMDRAW *cd = (NMLVCUSTOMDRAW *)lp;
                if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) {
                    SetWindowLongPtr(h, DWLP_MSGRESULT, CDRF_NOTIFYPOSTPAINT);
                    return TRUE;
                }
                if (cd->nmcd.dwDrawStage == CDDS_POSTPAINT) {
                    RECT r;
                    HFONT f = (HFONT)SendMessage(st->list, WM_GETFONT, 0, 0), of = NULL;
                    bool dark = kitty_theme_window_dark(h);
                    GetClientRect(st->list, &r);
                    InflateRect(&r, -2, -2);
                    if (f)
                        of = SelectObject(cd->nmcd.hdc, f);
                    SetBkMode(cd->nmcd.hdc, TRANSPARENT);
                    SetTextColor(cd->nmcd.hdc, dark ? kitty_theme_text_colour(true)
                                                    : GetSysColor(COLOR_GRAYTEXT));
                    {
                        RECT t = r;
                        t.top += 4;
                        DrawTextA(cd->nmcd.hdc, KT_SP_ORG_EMPTY, -1, &t,
                                  DT_CENTER | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
                    }
                    if (GetFocus() == st->list)
                        DrawFocusRect(cd->nmcd.hdc, &r);
                    if (of)
                        SelectObject(cd->nmcd.hdc, of);
                    SetWindowLongPtr(h, DWLP_MSGRESULT, CDRF_DODEFAULT);
                    return TRUE;
                }
            } else if (nm->code == NM_DBLCLK) {
                /* a double click starts the session clicked - only that
                 * one, whatever else is selected, as Explorer opens the
                 * item clicked; Enter starts the whole selection */
                NMITEMACTIVATE *ia = (NMITEMACTIVATE *)lp;
                LVITEMA it;
                memset(&it, 0, sizeof(it));
                it.mask = LVIF_PARAM;
                it.iItem = ia->iItem;
                if (ia->iItem >= 0 &&
                    SendMessageA(st->list, LVM_GETITEMA, 0, (LPARAM)&it) &&
                    it.lParam >= 0 && it.lParam < st->s.n) {
                    char *one = st->s.names[it.lParam];
                    so_start_named(&one, 1);
                }
            } else if (nm->code == LVN_KEYDOWN) {
                NMLVKEYDOWN *kd = (NMLVKEYDOWN *)lp;
                if (kd->wVKey == 'A' && (GetKeyState(VK_CONTROL) & 0x8000))
                    ListView_SetItemState(st->list, -1, LVIS_SELECTED, LVIS_SELECTED);
            } else if (nm->code == LVN_BEGINDRAG && !GetReadOnlyFlag()) {
                /* Drag sessions onto a folder of the tree: the pointer
                 * carries the session's name, or "N sessions". */
                int nsel = ListView_GetSelectedCount(st->list);
                char *text;
                if (nsel == 1) {
                    char **sel;
                    int n;
                    sel = so_selected(st, &n);
                    text = dupstr(n ? ksp_leaf(sel[0]) : "");
                    sfree(sel);
                } else {
                    text = dupprintf(KT_SP_ORG_COUNT, nsel);
                }
                so_drag_begin(st, SO_DRAG_SESSIONS, text);
                sfree(text);
            }
        }
        return FALSE;
      }
      case WM_CONTEXTMENU:
        if (!st || st->dragging)
            return FALSE;
        if ((HWND)wp == st->list) {
            so_list_menu(st, (short)LOWORD(lp), (short)HIWORD(lp));
            return TRUE;
        }
        if ((HWND)wp == st->tree) {
            so_tree_menu(st, (short)LOWORD(lp), (short)HIWORD(lp));
            return TRUE;
        }
        return FALSE;
      case WM_TIMER:
        if (st && wp == SO_TIMER_DRAG) {
            so_drag_tick(st);
            return TRUE;
        }
        return FALSE;
      case WM_MOUSEMOVE:
        if (st && st->dragging)
            so_drag_track(st);
        return FALSE;
      case WM_LBUTTONUP:
        if (st && st->dragging)
            so_drag_end(st, 1);
        return FALSE;
      case WM_CAPTURECHANGED:
        /* the capture taken away (a box, Alt+Tab): the drag ends, no drop */
        if (st && st->dragging && (HWND)lp != h)
            so_drag_end(st, 0);
        return FALSE;
      case WM_COMMAND:
        if (st && HIWORD(wp) == BN_CLICKED)
            so_command(st, LOWORD(wp));
        return TRUE;
      case WM_CLOSE:
        DestroyWindow(h);
        return TRUE;
      case WM_DESTROY:
        ShinyRemoveAuxDialog(h);
        if (st && st->ready)
            kitty_auxpos_save(h, KR_DLGPOS_MANAGE);
        if (st) {
            if (st->dragging)
                so_drag_end(st, 0);
            if (st->ready && !GetReadOnlyFlag()) {
                /* where it was left: the folder, and the session in focus */
                int i = ListView_GetNextItem(st->list, -1, LVNI_FOCUSED);
                const char *sn = "";
                if (i < 0)
                    i = ListView_GetNextItem(st->list, -1, LVNI_SELECTED);
                if (i >= 0) {
                    LVITEMA it;
                    memset(&it, 0, sizeof(it));
                    it.mask = LVIF_PARAM;
                    it.iItem = i;
                    if (SendMessageA(st->list, LVM_GETITEMA, 0, (LPARAM)&it) &&
                        it.lParam >= 0 && it.lParam < st->s.n)
                        sn = st->s.names[it.lParam];
                }
                kitty_state_set_string(KR_MANAGE_FOLDER, st->cur ? st->cur : "");
                kitty_state_set_string(KR_MANAGE_SESSION, sn);
            }
            sfree(st->dragfolder);
            so_folders_free(&st->fs);
            so_names_free(&st->s);
            sfree(st->cur);
            if (so_window == st)
                so_window = NULL;
            sfree(st);
            SetWindowLongPtr(h, GWLP_USERDATA, 0);
            /* -manage: the window is the program. While Edit's configuration
             * window stands, the end waits for it (so_edit). */
            if (so_manage_mode) {
                if (so_editing)
                    so_quit_after_edit = 1;
                else
                    PostQuitMessage(0);
            }
        }
        return FALSE;
    }
    return FALSE;
}

void kitty_sessorg_open(HWND owner)
{
    struct so_state *st;
    if (so_window && IsWindow(so_window->h)) {
        if (IsIconic(so_window->h))
            ShowWindow(so_window->h, SW_RESTORE);
        SetForegroundWindow(so_window->h);
        return;
    }
    st = snew(struct so_state);
    memset(st, 0, sizeof(*st));
    so_window = st;
    if (!so_dialog(IDD_SESSORG, owner, so_proc, st) && so_window == st) {
        /* the template did not load: no WM_DESTROY freed it */
        so_window = NULL;
        sfree(st);
    }
}
