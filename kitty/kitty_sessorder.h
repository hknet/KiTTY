/*
 * kitty_sessorder.h - the manual order of folders and sessions (Manage
 * Sessions' drag and drop): a moved item is anchored to its neighbour.
 *
 * Per sibling level (the subfolders of one folder, the sessions of one
 * folder), separately for folders and for sessions, the items without a
 * stored place keep the automatic order; an item with a stored place comes
 * directly after its anchor (the item above it when it was dropped), or
 * first. Chains resolve in order; an anchor that no longer exists, a cycle
 * or a damaged entry leaves the item in its automatic place. Names compare
 * without case: an entry stands for the item as the list shows it.
 *
 * Where it is kept, per folder:
 *   folder store  <session dir>\<folder>\.kittyorder, a public text format:
 *                 "s" or "f", a TAB, the name as shown, optionally a TAB and
 *                 the anchor; without an anchor the item follows the line
 *                 before of the same kind ("first" for the first one). '#'
 *                 lines and empty lines are comments. Manage Sessions edits
 *                 the file in place (the lines whose meaning changes, no
 *                 more) and writes it whole or not at all.
 *   registry      <base>\SessionOrder\<folder escaped as Sessions escapes a
 *                 name>, the root as "%5C" (the escape of "\"): one REG_SZ
 *                 value per item, "s:<name>" / "f:<name>", data = the anchor,
 *                 empty = first.
 *
 * The pure part (lists, the resolver, the planner, the line model) touches
 * no store; the store part needs no settings library, so kageant links it
 * for its Saved Sessions menu. kitty.exe fills its store description with
 * ksp_order_store_current() (kitty_storage.c).
 */
#ifndef KITTY_SESSORDER_H
#define KITTY_SESSORDER_H

#include <stddef.h>

#define KSP_ORDER_SESSION 's'
#define KSP_ORDER_FOLDER  'f'

/* The file of the folder store and the registry key, by name. */
#define KSP_ORDER_FILE    ".kittyorder"
#define KSP_ORDER_REGKEY  "SessionOrder"
#define KSP_ORDER_REGROOT "%5C"

/* ---- stored places ---- */
struct ksp_order_entry {
    char kind;          /* KSP_ORDER_SESSION / KSP_ORDER_FOLDER */
    char *name;
    char *anchor;       /* NULL = first */
};
struct ksp_order_list {
    struct ksp_order_entry *e;
    int n;
    size_t cap;
};
void ksp_order_list_init(struct ksp_order_list *l);
void ksp_order_list_free(struct ksp_order_list *l);
void ksp_order_list_add(struct ksp_order_list *l, char kind, const char *name,
                        const char *anchor);
/* The first entry of `kind` for `name` (case-insensitive), or NULL. */
const struct ksp_order_entry *ksp_order_find(const struct ksp_order_list *l,
                                             char kind, const char *name);

/* ---- the resolver ----
 * names[] are the n items of one kind of one level, already in the automatic
 * order (several items of one shown name are one entry: they move together,
 * in their automatic order). perm[] receives the indexes into names[] in the
 * order shown; pinned[] (may be NULL) is set to 1 for each item whose stored
 * place took effect. */
void ksp_order_apply(const struct ksp_order_list *l, char kind,
                     char *const *names, int n, int *perm,
                     unsigned char *pinned);

/* ---- the planner ----
 * What a drop or a removal changes. `autonames` are the items of the level
 * after the change, in the automatic order; `desired` the order the list is
 * to show afterwards (the same items, one per shown name). `moved` (in the
 * order shown, may be 0) get the place of the drop: the first one after the
 * item above it in `desired`, each next one after the one before. `gone`
 * names (may be 0) lose their place. An item anchored to a moved or gone
 * item, or to the row the block was dropped below, is anchored again so it
 * stays where it was shown; any other entry is kept, and an item whose place
 * the result would still differ from `desired` gets one too. *changes
 * receives one change per name: the anchor set, or the place forgotten.
 * Returns the number of changes. */
struct ksp_order_change {
    char kind;
    char *name;
    char *anchor;       /* NULL = first */
    int remove;         /* 1 = the place is forgotten */
    int moved;          /* 1 = one of `moved` (its line moves in a file) */
};
struct ksp_order_changes {
    struct ksp_order_change *c;
    int n;
    size_t cap;
};
void ksp_order_changes_free(struct ksp_order_changes *ch);
int ksp_order_plan(const struct ksp_order_list *old, char kind,
                   char *const *autonames, int nauto,
                   char *const *desired, int ndesired,
                   char *const *moved, int nmoved,
                   char *const *gone, int ngone,
                   struct ksp_order_changes *changes);

/* ---- one level's stored places as a document ----
 * A file is kept as its lines (comments, empty lines, line ends, the BOM
 * and the lines of names not in the folder stay byte for byte); the registry
 * as its values. ksp_order_doc_text parses a file's bytes (len 0 = no file);
 * ksp_order_doc_bytes serialises it (snewn'd, *len set; the bytes of the
 * file as it would be written). */
struct ksp_order_doc;
struct ksp_order_doc *ksp_order_doc_text(const char *buf, size_t len);
struct ksp_order_doc *ksp_order_doc_values(const struct ksp_order_list *values);
void ksp_order_doc_free(struct ksp_order_doc *d);
/* The places the document stores, in its order (a file's two-field lines
 * resolved to the line before). */
void ksp_order_doc_entries(const struct ksp_order_doc *d,
                           struct ksp_order_list *out);
/* Carry out a plan. `level` lists every item of the level of `kind` (the
 * known names): only their lines are rewritten; NULL = every name of the
 * document counts as known. */
void ksp_order_doc_change(struct ksp_order_doc *d, char kind,
                          char *const *level, int nlevel,
                          const struct ksp_order_changes *ch);
/* An item renamed: the lines and values that carry the name. */
void ksp_order_doc_rename(struct ksp_order_doc *d, char kind,
                          const char *from, const char *to);
/* Reset Order: every item line or value goes, comments stay. */
void ksp_order_doc_reset(struct ksp_order_doc *d);
/* Did the meaning or the bytes change since it was read? Nothing to write
 * when not. ksp_order_doc_empty: no item and no comment left (a file is then
 * deleted, a key removed). */
int ksp_order_doc_changed(const struct ksp_order_doc *d);
int ksp_order_doc_empty(const struct ksp_order_doc *d);
char *ksp_order_doc_bytes(const struct ksp_order_doc *d, size_t *len);

/* ---- the store ---- */
struct ksp_order_store {
    int folder_store;       /* 1 = the folder store, 0 = the registry */
    char *dir;              /* folder store: the session directory */
    char *regbase;          /* registry: the base key below the root */
    void *regroot;          /* registry: an HKEY; NULL = HKEY_CURRENT_USER
                             * (the unit test's private hive) */
    /* folder store: makes the directories above a file about to be written
     * (kitty.exe: with the store's rights); NULL = CreateDirectory */
    void (*make_dirs)(const char *filepath);
};
/* kitty.exe: the store in use (kitty_storage.c, settings library). */
void ksp_order_store_current(struct ksp_order_store *st);
void ksp_order_store_free(struct ksp_order_store *st);

/* Where a folder's places are: the file's path, or the key below HKCU.
 * `folder` NULL, "" or "Default" = the root. snewn'd. */
char *ksp_order_location(const struct ksp_order_store *st, const char *folder);

/* The places of `folder`, through a per-process cache that reads the file
 * or key again only when its last-write time changed. Never fails: no file,
 * no key or a damaged file is an empty list. The list belongs to the cache:
 * valid until the next call. */
const struct ksp_order_list *ksp_order_load(const struct ksp_order_store *st,
                                            const char *folder);
void ksp_order_cache_clear(void);

/* Read a folder's document, and write it back. ksp_order_commit writes
 * nothing when nothing changed; a file left with no item and no comment is
 * deleted, a key with no value removed. 1 done (or nothing to do); 0 failed,
 * Windows' reason in GetLastError(). readonly=yes: nothing written, 1. */
struct ksp_order_doc *ksp_order_open(const struct ksp_order_store *st,
                                     const char *folder);
int ksp_order_commit(const struct ksp_order_store *st, const char *folder,
                     struct ksp_order_doc *d);

/* A level in the stored order, in one step: load + apply. st NULL = the
 * automatic order (perm the identity). */
void ksp_order_sort_level(const struct ksp_order_store *st, const char *folder,
                          char kind, char *const *names, int n, int *perm,
                          unsigned char *pinned);

/* Session rows already sorted by ksp_sort_shown / _by_folder (a list that
 * may hold several folders): the rows of each folder take that folder's
 * stored order among the positions they hold; rows of other folders and
 * "Default Settings" stay where they are. A row's folder is its `folder`
 * (NULL, "" or "Default" = the root); a row's name is the leaf of its id.
 * pinned (may be NULL) gets 1 per row position with a stored place. */
struct ksp_shown_row;
void ksp_order_shown_rows(const struct ksp_order_store *st,
                          struct ksp_shown_row *rows, int n,
                          unsigned char *pinned);

/* ---- the store changes under the order (each returns 1, or 0 when a
 * write failed) ----
 * An item of `folder` left it (moved away or deleted): its place goes, the
 * items anchored to it take its anchor (an item that had no place of its own
 * leaves them anchored to a name that is gone: their automatic place). */
int ksp_order_forget(const struct ksp_order_store *st, const char *folder,
                     char kind, const char *name);
/* An item renamed in its folder. */
int ksp_order_rename(const struct ksp_order_store *st, const char *folder,
                     char kind, const char *from, const char *to);
/* A folder moved from `from` to `to` (NULL = deleted with everything in
 * it): the places of it and of every folder below it follow the path; where
 * the new path already has places of its own, those stay and the moved ones
 * are dropped. `merged`: `from` was emptied into the existing folder `to`
 * (a folder deleted with its sessions moved elsewhere) - its own level's
 * places go, the folders below it keep theirs. Its own entry in the folder
 * above it is renamed (same parent) or forgotten. */
int ksp_order_folder_moved(const struct ksp_order_store *st, const char *from,
                           const char *to, int merged);
/* Reset Order: forget every place of `folder`'s level (its subfolders and
 * its sessions). */
int ksp_order_reset(const struct ksp_order_store *st, const char *folder);

/* Every level's places of `from` carried into `to` (Take folder store, the
 * portable copy): a level with no places in `to` takes them all; in a level
 * with places of its own a place is added only for an item without one,
 * an existing place is never overwritten. Returns the number of levels
 * that could not be written. */
int ksp_order_copy_all(const struct ksp_order_store *from,
                       const struct ksp_order_store *to);

#endif /* KITTY_SESSORDER_H */
