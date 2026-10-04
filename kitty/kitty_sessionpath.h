/*
 * kitty_sessionpath.h - a saved session's name as a PATH (hknet/KiTTY#55) and
 * the session file suffix (hknet/KiTTY#56).
 *
 * A session's identity is its folder path plus its name, joined with '\':
 * "Linux\web\srv01". A root session is its bare name, as before. The same
 * identity names the session in both stores:
 *
 *   registry      one flat key per session, PuTTY's escape of the whole path
 *                 (Sessions\Linux%5Cweb%5Csrv01) - no subkeys;
 *   folder store  nested directories, every component escaped on its own
 *                 (Sessions\Linux\web\srv01<suffix>).
 *
 * The pure helpers (ksp_*) touch no store and no global, so the unit test
 * drives them directly. kitty_session_resolve() is the one store-aware entry
 * point: it lives here, in the settings library, so the console tools and the
 * SSH proxy code get the same answer as the GUI.
 */
#ifndef KITTY_SESSIONPATH_H
#define KITTY_SESSIONPATH_H

#include <stddef.h>
#include <stdbool.h>

/* ---- one path component <-> one file or directory name ----
 * The ksf_munge escape (%xx for \ / : * ? " < > | % and control characters),
 * plus what Windows cannot store as a name: a leading dot (a dot-file is never
 * listed), a trailing dot or space, and the reserved device names (CON, PRN,
 * AUX, NUL, COM1-COM9, LPT1-LPT9, with or without an extension). All of them
 * are %xx escapes, so ksp_component_unmunge() - plain ksf_unmunge - reverses
 * every one. snewn'd. */
char *ksp_component_munge(const char *component);
char *ksp_component_unmunge(const char *filename);

/* A path's folder ("Linux\web" of "Linux\web\srv01"), snewn'd, or NULL for a
 * root session. ksp_leaf() points into the argument. */
char *ksp_folder_of(const char *path);
const char *ksp_leaf(const char *path);

/* The path with empty components dropped (leading, trailing and doubled '\'),
 * snewn'd. "" stays "". */
char *ksp_normalise(const char *path);

/* The file name of a session RELATIVE to the session directory: every
 * component escaped, joined with '\', the suffix appended to the last one.
 * snewn'd. */
char *ksp_path_to_relfile(const char *path, const char *suffix);

/* Laid out as session files with `suffix`, would one of these sessions'
 * file stand where another needs a folder (`foo` and `foo\bar` with no
 * suffix)? Returns that session's name (snewn'd), or NULL when the layout
 * has no such clash. */
char *ksp_layout_blocker(const char *const *names, int n, const char *suffix);

/* Strip `suffix` from a file name when it ends with it (case-insensitive) and
 * something is left in front of it. Returns 1 if stripped. */
int ksp_strip_suffix(char *filename, const char *suffix);

/* ---- folders as paths ----
 * A folder is a path too ("Linux\web"); "Default" (or empty) is the root.
 * Both stores are case-insensitive, so folders compare that way. */
int ksp_folder_is_root(const char *folder);
int ksp_folder_same(const char *a, const char *b);
/* Is `folder` the folder `top` or one below it? Everything is within root. */
int ksp_folder_within(const char *folder, const char *top);
/* The folder above `folder`, "Default" above a top-level one. snewn'd. */
char *ksp_folder_parent(const char *folder);
/* The row `folder` puts at `level`: the full path of the child of `level`
 * that leads to it, or NULL when it is not below `level`. snewn'd. */
char *ksp_folder_child_row(const char *level, const char *folder);
/* Where `path` (a session or a folder below `from`) lands when the folder
 * `from` moves to `to`: its subtree keeps its shape below the new place,
 * "a\b\srv" from "a" to the root is "b\srv", to "x" it is "x\b\srv".
 * "Default" when the result is the root itself. snewn'd. */
char *ksp_folder_moved_path(const char *path, const char *from, const char *to);

/* ---- what counts as a session file (hknet/KiTTY#56) ----
 * A file is a session when it holds a HostName or Protocol key, in either
 * on-disk form: "Key=value" (KiTTY++) or "Key\value\" (classic KiTTY, .ktx).
 * Known limit: any text file with a line starting "HostName=" counts. */
int ksp_text_is_session(const char *buf, size_t len);
int ksp_file_is_session(const char *path);

/* The same verdict through the per-process cache, for a directory walk:
 * `size` and `mtime` (FILETIME as one number) are what the walk's find data
 * says of the file. It is read only when new, changed in size or time, or
 * written in the last few seconds. A full walk of the store brackets its
 * lookups with walk_begin/walk_end, which drops the entries of files it no
 * longer met; ksp_verdict_forget drops one (a file this process wrote).
 * ksp_verdict_reads counts the files read so far, ksp_verdict_count the
 * entries held (both for the unit test). Thread-safe. */
int ksp_file_verdict(const char *path, unsigned long long size,
                     unsigned long long mtime);
void ksp_verdict_forget(const char *path);
unsigned long ksp_verdict_walk_begin(void);
void ksp_verdict_walk_end(unsigned long gen);
unsigned long ksp_verdict_reads(void);
int ksp_verdict_count(void);

/* ---- name lookup over a list of session identities ----
 * Path exact (case-insensitive) wins. A bare name (no '\') that is not an
 * exact match is looked up by its last component: one match is that session,
 * more than one is AMBIGUOUS. A path that does not match exactly is NONE: a
 * folder path is never guessed. `matches` receives up to `maxmatches` indexes
 * into names[]; *nmatches the full count. */
#define KSP_NONE       0
#define KSP_EXACT      1
#define KSP_UNIQUE     2
#define KSP_AMBIGUOUS  3
int ksp_lookup(char *const *names, int n, const char *wanted,
               int *matches, int maxmatches, int *nmatches);

/* The -load text for an ambiguous bare name, snewn'd. `list` holds the
 * matching identities. with_load_hint 0 leaves out the "-load" sentence (the
 * SSH proxy's form). */
char *ksp_ambiguous_text(const char *wanted, char *const *list, int nlist,
                         int with_load_hint);

/* ---- against the store in use ----
 * Resolve a TYPED session name. Returns KSP_NONE (no such session - the
 * caller does what it did before), KSP_EXACT / KSP_UNIQUE with *resolved set
 * (snewn'd identity to load), or KSP_AMBIGUOUS with *errtext set (snewn'd,
 * the -load text; with_load_hint as above). Either out pointer may be NULL. */
int kitty_session_resolve(const char *wanted, char **resolved, char **errtext,
                          int with_load_hint);

/* Every saved session identity in the store in use, "Default Settings"
 * included when stored. *n gets the count; free with
 * kitty_session_names_free. */
char **kitty_session_names(int *n);
void kitty_session_names_free(char **names, int n);

/* ---- planning a move (no store touched: the unit test drives these) ----
 * A plan is the re-keys a move needs (from[i] -> to[i], identities), the
 * Folder values to rewrite for sessions stored by their bare name
 * (fname[i] gets Folder=fvalue[i], "Default" = the root), and the targets
 * that are taken (clash[]: an identity another session already has, or one
 * two moves of the plan would both use). A plan with clashes is carried out
 * not at all (Arrange aside, see there). `names` is every identity in the store, `folders[i]` the
 * Folder value of names[i] (read only for bare names; may be NULL). */
struct ksp_plan {
    int n;
    char **from, **to;
    int nfolder;
    char **fname, **fvalue;
    int nclash;
    char **clash;
};
void ksp_plan_init(struct ksp_plan *p);
void ksp_plan_free(struct ksp_plan *p);
/* Arrange: every session stored by its bare name (Default Settings aside)
 * whose Folder value is not the root moves to <Folder>\<name>. Unlike the
 * other plans, a taken path does not stop it: that session is only listed in
 * clash[] and stays where it is, every free one is planned. p->n + p->nclash
 * is the number of sessions to arrange. */
void ksp_plan_arrange(char *const *names, char *const *folders, int n,
                      struct ksp_plan *p);
/* Is the first-start Arrange offer due for this Arrange plan? Only when an
 * unarranged session's folder path is already taken by another session (the
 * list would show two sessions of one name in one folder). Sessions filed by
 * their Folder value with a free path are left to Arrange on demand. */
int ksp_plan_arrange_offer_due(const struct ksp_plan *p);
/* A folder and everything below it moves from `from` to `to` (a rename, or
 * the delete of `from` into `to`): path sessions are re-keyed keeping their
 * shape below it, bare sessions filed there get their Folder value moved. */
void ksp_plan_folder_move(char *const *names, char *const *folders, int n,
                          const char *from, const char *to,
                          struct ksp_plan *p);
/* The sessions sel[] move into folder `dest` under their own names. */
void ksp_plan_sessions_move(char *const *names, int n, char *const *sel,
                            int nsel, const char *dest, struct ksp_plan *p);

/* The identity a session file of another store gets on import, from its path
 * below that store's Sessions folder ("Linux\web\srv01.ktx"): every
 * component unescaped, `suffix` (may be empty) taken off the file name.
 * snewn'd. */
char *ksp_import_name(const char *relpath, const char *suffix);

/* ---- moving a registry key tree without losing anything ----
 * `root` is any open key (HKEY_CURRENT_USER in use; a private hive in the
 * unit test). ksp_reg_copy_verified copies src to dst - values of any size,
 * subkeys all the way down - and then compares the two value by value; on
 * any difference it deletes what it wrote and returns 0. dst must not exist.
 * ksp_reg_move_tree is copy, prove, then delete the source; a change of case
 * only goes through a temporary key and is proven at each step. On failure
 * nothing is lost: the source is left (or rebuilt) as it was, except when it
 * cannot be written back - *leftover (snewn'd) then names the key (the
 * temporary key, or dst) that holds the only complete copy. */
#ifdef _WIN32
#include <windows.h>
int ksp_reg_copy_verified(HKEY root, const char *src, const char *dst);
int ksp_reg_move_tree(HKEY root, const char *src, const char *dst,
                      char **leftover);
bool ksp_reg_delete_tree(HKEY root, const char *path);
#endif

#endif /* KITTY_SESSIONPATH_H */
