/*
 * kitty_migrate.h: importing sessions that belong to an older KiTTY or to
 * stock PuTTY into this KiTTY's own store.
 *
 * The read-only fallback hives make such sessions VISIBLE (kitty_storage.c);
 * this is the other half - taking a copy that is ours, in our format, without
 * touching the hive it came from.
 *
 * Registry stores only: a portable store has no foreign hive to import from.
 * Windows-only; include after putty.h and kitty_storage.h.
 */
#ifndef KITTY_MIGRATE_H
#define KITTY_MIGRATE_H

/* A set of setting names: insertion-ordered, duplicate-free. Small enough that
 * a linear scan is the right lookup. */
struct kitty_namelist {
    char **names;
    int n, alloc;
};
void kitty_namelist_add(struct kitty_namelist *l, const char *name);
bool kitty_namelist_has(const struct kitty_namelist *l, const char *name);
void kitty_namelist_clear(struct kitty_namelist *l);
char *kitty_namelist_join(const struct kitty_namelist *l, const char *sep);

/* One foreign session: its name as the user sees it, and which hive it is in
 * (KSEC_HIVE_OLDKITTY / KSEC_HIVE_PUTTY, kitty_storage.h). */
struct kitty_foreign_session {
    char *name;
    int hive;
};
struct kitty_foreign_list {
    struct kitty_foreign_session *items;
    int n;
};

/*
 * Every session in either foreign hive, "Default Settings" excluded.
 *
 * Deliberately NOT enum_settings_start(): that answers "what should the saved
 * session list show", so it hides a foreign session behind a same-named one of
 * ours and disappears entirely when the show-foreign setting is off. Both are
 * wrong here - a shadowed session is exactly the one the suffix rule exists
 * for, and a session that can be imported can be imported whether or not it is
 * currently on show.
 */
struct kitty_foreign_list *kitty_foreign_sessions(void);
void kitty_foreign_list_free(struct kitty_foreign_list *l);
const char *kitty_foreign_hive_label(int hive);   /* "old KiTTY" / "PuTTY" */

/*
 * Import one session into our own store. Returns the name it was saved under
 * (the caller frees it) or NULL if the session could not be read.
 *
 * The source hive is left exactly as it was. An existing name is never
 * overwritten: the copy arrives as "work (PuTTY)", and a counter is added if
 * that is taken too. Setting names this KiTTY no longer reads are added to
 * *dropped, which the caller reports once for the whole import.
 */
char *kitty_import_foreign_session(const char *name, int hive,
                                   struct kitty_namelist *dropped);

/* The import NAMES what it left behind; why a particular setting is not
 * carried over is in the manual, not in a string here. */

/*
 * Sessions in FILES: a folder tree holding an old KiTTY's (or a copied)
 * Sessions directory - design/TASK_old_kitty_folders_import.md. Works in
 * either store mode: the copy is saved by the ordinary save path.
 */
enum {
    KFS_READY = 0,        /* imports as it is */
    KFS_PASSWORD,         /* imports, but the stored password cannot be decoded here */
    KFS_PASSWORD_MPW,     /* imports; the password is under a master password
                             (that store's, not asked for - see the design note) */
    KFS_UNREADABLE        /* the file did not parse as a session */
};
struct kitty_folder_scan_item {
    char *name;           /* the session name (the file name, unmunged) */
    char *path;           /* full path of the file */
    char *folder;         /* target folder the user assigned (owned) */
    int state;            /* KFS_* */
};
struct kitty_folder_scan {
    struct kitty_folder_scan_item *items;
    int n;
    size_t alloc;         /* size_t: sgrowarray takes its address */
    int files_seen, dirs_seen;
    bool hit_depth, hit_count;   /* which limit stopped the walk, if any */
};
#define KFS_MAX_DEPTH 8
#define KFS_MAX_FILES 5000
struct kitty_folder_scan *kitty_scan_folder_store(const char *root,
                                                  const char *default_folder);
void kitty_folder_scan_free(struct kitty_folder_scan *s);
/* Does a session of this name exist in OUR store (registry key or file,
 * whichever this KiTTY runs)? */
bool kitty_own_session_exists(const char *name);
/* The name an import into `folder` gets: the name itself if free, else
 * "name (folder)", else "name (folder 2)" and up. `taken` are names this run
 * has already handed out (so two files of one name do not collide on disk).
 * Caller frees; NULL if nothing is free. */
char *kitty_import_folder_target_name(const char *name, const char *folder,
                                      const struct kitty_namelist *taken);
/* Import one session file into our store under `target` in `folder`.
 * *password_lost is set when the source had a password this KiTTY could not
 * decode (the session is saved without it). Returns false if the file could
 * not be read or the save failed. */
bool kitty_import_file_session(const char *path, const char *target,
                               const char *folder, const char *store_pass,
                               struct kitty_namelist *dropped,
                               bool *password_lost);
/* Does `pass` open the master-password value in this session file? The
 * import's own check, before it asks again; false when the file has no MPW2
 * value at all. */
bool kitty_import_store_pass_fits(const char *path, const char *pass);

#endif /* KITTY_MIGRATE_H */
