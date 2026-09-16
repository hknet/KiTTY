/*
 * kitty_storage.h: interface between windows/storage.c (upstream PuTTY's
 * storage API, kept textually close to upstream) and kitty_storage.c (the
 * fork's store state: runtime registry root, fallback hives, portable file
 * backend). The at-rest credential crypto is declared in kitty_secretstore.h.
 *
 * Windows-only; include after putty.h/windows.h.
 * Returned char* ownership follows the comments in kitty_storage.c.
 */
#ifndef KITTY_STORAGE_H
#define KITTY_STORAGE_H
#include "putty.h"
#include "storage.h"   /* settings_r, settings_w */

/* Read-only fallback hives (precedence: our base > old KiTTY hive > stock
 * PuTTY). Sessions present only in an older hive stay loadable; edits write
 * to our base. */
#define OLD_KITTY_HIVE_SESSIONS "Software\\9bis.com\\KiTTY\\Sessions"
#define PUTTY_HIVE_SESSIONS     "Software\\SimonTatham\\PuTTY\\Sessions"

/* Which of those a session was read from. Recorded on the open handle, because
 * the at-rest format differs per hive: an old-KiTTY password is legacy-encrypted
 * and nothing else is. */
#define KSEC_HIVE_PRIMARY  0   /* our own kapper.net hive (or PuTTY base if KiClassName=PuTTY) */
#define KSEC_HIVE_OLDKITTY 1   /* read-only fallback: old 9bis KiTTY (legacy-encrypted passwords) */
#define KSEC_HIVE_PUTTY    2   /* read-only fallback: stock PuTTY (only our own cleartext can live here) */

/* Marker prefix of a DPAPI-wrapped stored secret (see ksec_protect_*). */
#define KITTY_SECRET_DPAPI_MARK "DPAPI1:"

/* Forget the cached session -> folder answers (kitty_read_session_folder).
 * Call after re-reading the session list from the store or writing a
 * session's Folder value; the cache also expires by itself after 2 s. */
void kitty_session_folder_cache_clear(void);

/* Marker prefix of a password deliberately supplied in the CLEAR by an external
 * provisioning script, e.g. Password\PLAIN:hunter2\ in a rolled-out .ktx. It is
 * an input format only: KiTTY recognises it on import and never writes it, and
 * the value is re-protected by the destination backend on the next save. It
 * exists because an unmarked value cannot be told apart from an old-KiTTY
 * encrypted one with certainty (see kitty_secret_decode_imported). */
#define KITTY_SECRET_PLAIN_MARK "PLAIN:"

/* portable-store key=value list node (definition private to kitty_storage.c) */
struct ksf_item;

/* ---- runtime registry root + derived key names ---- */
int kitty_root_is_putty(void);
int kitty_get_show_foreign_sessions(void);  /* fallback hives visible? */
void kitty_set_show_foreign_sessions(int on);

/*
 * The one-time notice that "auto" has answered the old-sessions question.
 * Two places show it and neither waits for the other: the box at startup and
 * the line under the saved-session list. Each clears its OWN bit once it has
 * been seen, so whichever comes first does not rob the other.
 */
#define KITTY_FOREIGN_NOTICE_STARTUP 1
#define KITTY_FOREIGN_NOTICE_LIST    2
int kitty_foreign_notice_pending(int bits);
void kitty_foreign_notice_clear(int bits);
const char *kitty_registry_base(void);   /* base hive, no suffix */
const char *kitty_reg_sessions(void);    /* <base>\Sessions */
const char *kitty_reg_jumplist(void);    /* <base>\Jumplist */
const char *kitty_reg_hostcas(void);     /* <base>\SshHostCAs */
const char *kitty_reg_hostkeys(void);    /* <base>\SshHostKeys */

/* ---- the read watch (session importer) ----
 * While a callback is set, every setting name read from a session is passed to
 * it. Set it around one load and clear it again; it is not re-entrant and is
 * meant for a single-threaded import, not for general instrumentation. */
void kitty_set_read_watch(void (*cb)(const char *key));
void kitty_read_watch_note(const char *key);   /* called by the read path */

/* Open a session from ONE hive, ignoring the precedence chain (windows/storage.c).
 * hive is KSEC_HIVE_OLDKITTY / KSEC_HIVE_PUTTY as kitty_migrate.h re-exports them. */
settings_r *kitty_open_settings_r_hive(const char *sessionname, int hive);
/* 1 when a read handle came from the store this KiTTY writes (own hive or a
 * portable file), 0 when it came from a read-only fallback hive. */
int kitty_settings_r_is_own(settings_r *handle);
/* A read handle over ONE session file at any path - a folder-store import's
 * way in. Both on-disk formats, legacy password conversion included, via
 * ksf_load. NULL if the file cannot be read or parsed. */
settings_r *kitty_open_settings_r_file(const char *path);
/* The same over an already-parsed (and possibly rewritten) list; the list is
 * owned by the handle from then on. */
settings_r *kitty_open_settings_r_items(struct ksf_item *items);
/* Walk a parsed file's keys, in file order (the import asks which keys the
 * loader never read). */
void ksf_list_foreach(struct ksf_item *h,
                      void (*fn)(const char *key, const char *val, void *ctx),
                      void *ctx);

/*
 * Settings that were renamed or replaced. `was` is dropped from a session the
 * next time it is saved; `now` names what took its place. When `migrates` is
 * true the value still means the same thing, so reading `now` falls back to
 * `was` and an old session keeps its setting. When it is false the value is
 * deliberately not carried over, and the session importer says so.
 */
struct kitty_retired_key {
    const char *was;
    const char *now;
    bool migrates;
    const char *why;      /* for a non-migrating key: the one-line reason (may be NULL) */
};
const struct kitty_retired_key *kitty_retired_key_table(size_t *n);

/*
 * Fold a session's legacy "Notes" value into its Comment and switch the
 * Comment panel's "Notify the user at login" on. `note` may be NULL or empty,
 * in which case nothing happens. Idempotent: a Comment that already contains
 * the note is left as it is. Nothing is written to the store - the old value
 * is dropped by the retired-key rule the next time the session is saved.
 */
void kitty_merge_legacy_note(Conf *conf, const char *note);

/* ---- portable file backend (fork-native flat .ini/dir format) ---- */
int store_is_file(void);                 /* portable mode active? */
const char *kitty_session_dir(void);     /* per-session files directory */
int kitty_storage_mode(void);            /* raw g_store_mode (diagnostics) */
char *ksf_munge(const char *in);                  /* snewn'd */
char *ksf_unmunge(const char *in);                /* snewn'd */
char *ksf_list_get(struct ksf_item *h, const char *key);  /* borrowed/NULL */
void ksf_list_set(struct ksf_item **h, const char *key, const char *val);
void ksf_list_del(struct ksf_item **h, const char *key);  /* retire a renamed key */
void ksf_list_free(struct ksf_item *h);
char *ksf_session_path(const char *sessionname);  /* snewn'd or NULL */
struct ksf_item *ksf_load(const char *path);      /* parsed list (may be NULL) */
void ksf_save(const char *path, struct ksf_item *h);
char *portable_root_dir(void);                    /* snewn'd or NULL */
char *portable_subdir_path(const char *subdir);   /* snewn'd */
char *portable_item_path(const char *subdir, const char *name);
int portable_write_text_file(const char *subdir, const char *name,
                             const char *value);
char *portable_read_text_file(const char *subdir, const char *name);

/* ---- the password diagnostic log (%TEMP%\kitty_pwdebug.log while
 * KITTY_PWDEBUG is set; lengths and a weak checksum, never plaintext) ---- */
unsigned ksec_cksum(const char *s);
void kitty_pwdebug(const char *fmt, ...);

/* ---- "the configuration store changed" flag ----
 * Set by every path that writes the store (session save/delete, host keys,
 * folders, proxies); consumed by the routine .sav backup so that merely OPENING
 * a session does not write one. Lives here because windows/storage.c is
 * compiled into every variant and already includes this header. */
void kitty_store_mark_dirty(void);
int  kitty_store_take_dirty(void);   /* 1 if dirty; clears the flag */

/* ---- exported from kitty/kitty_showforeign_ini.c ---- */
int kitty_showforeign_ini_read(char *value, size_t size);
int kitty_showforeign_may_persist(void);

/* ---- exported from kitty/kitty_storage.c ---- */
int kitty_get_last_folder(char *buf, int buflen);
int kitty_get_last_session(char *buf, int buflen);
int kitty_has_foreign_sessions(void);
int kitty_portable_load_state_dword(const char *key, DWORD *value);
int kitty_portable_load_state_string(const char *key, char *buf, int buflen);
int kitty_portable_store_state_dword(const char *key, DWORD value);
int kitty_portable_store_state_string(const char *key, const char *value);
char *kitty_read_session_comment(const char *sessionname);
char *kitty_read_session_folder(const char *sessionname);
char *kitty_read_session_folder_cached(const char *sessionname);
int kitty_session_origin(const char *sessionname);
void kitty_set_last_folder(const char *folder);
void kitty_set_last_session(const char *sessionname);
void kitty_set_registry_root(int use_putty);
void kitty_set_session_dir(const char *dir);
void kitty_set_storage_mode(int mode);
int kitty_storage_is_portable(void);

#endif /* KITTY_STORAGE_H */
