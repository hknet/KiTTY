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

#endif /* KITTY_MIGRATE_H */
