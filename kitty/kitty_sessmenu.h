/*
 * kitty_sessmenu.h - a saved-session menu laid out as the folder tree
 * (hknet/KiTTY#55): kageant's tray "Saved Sessions" submenu.
 *
 * Pure: no store and no window. The caller lists the session identities and,
 * for each one stored by its bare name, its Folder value; ksm_build returns
 * the menu as a flat pre-order list - open a folder submenu, a session, close
 * the submenu - which the caller turns into menus with a stack of handles.
 *
 * The layout is the launcher's: in every menu the folder submenus come first,
 * then the sessions. A session stored as a path ("Linux\web\srv01") is in
 * that path's folder, whatever its Folder value says; one stored by its bare
 * name is in its Folder value's folder ("Default" or empty = the top). Every
 * folder above a session is a submenu of its own, nested as the tree is.
 * Folders and sessions sort by the name they show, case-insensitively and
 * numbers by value (ksp_natcasecmp); "Default Settings" is left out.
 */
#ifndef KITTY_SESSMENU_H
#define KITTY_SESSMENU_H

#include <stddef.h>

#define KSM_FOLDER_OPEN   1     /* a submenu starts: label = folder name */
#define KSM_SESSION       2     /* label = shown name, idx = into ids[] */
#define KSM_FOLDER_CLOSE  3     /* the submenu ends */

struct ksm_entry {
    int kind;
    char *label;                /* snewn'd; NULL for KSM_FOLDER_CLOSE */
    int idx;                    /* KSM_SESSION: the caller's index */
};

/* `folders` may be NULL (no session filed by a Folder value); folders[i] is
 * read only when ids[i] holds no '\'. */
struct ksm_entry *ksm_build(char *const *ids, char *const *folders, int n,
                            int *nentries);
void ksm_free(struct ksm_entry *e, int n);

/* The Folder value of a session file of the folder store, either on-disk form
 * ("Folder=value" or classic "Folder\value\"), unescaped; NULL when the file
 * has none. snewn'd. */
char *ksm_text_folder(const char *buf, size_t len);
char *ksm_file_folder(const char *path);

/* The same through a cache keyed by the file's path, size and time (what the
 * store walk reports): a menu built again reads only the files that changed.
 * A full walk brackets its lookups with begin/end, which drops the entries of
 * files it no longer met. ksm_folder_reads counts the files read (unit
 * test). */
char *ksm_file_folder_cached(const char *path, unsigned long long size,
                             unsigned long long mtime);
void ksm_folder_cache_begin(void);
void ksm_folder_cache_end(void);
unsigned long ksm_folder_reads(void);

/* Every session of the folder store below `dir` (ksp_walk_store: the same
 * rules as the terminal's list) with the Folder value of each one stored by
 * its bare name (NULL for the others), ready for ksm_build. Returns the
 * count; free with ksm_rows_free. */
int ksm_folder_store_rows(const char *dir, const char *suffix,
                          char ***ids, char ***folders);
void ksm_rows_free(char **ids, char **folders, int n);

#endif /* KITTY_SESSMENU_H */
