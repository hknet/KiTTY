/*
 * kitty_storage.h: interface between windows/storage.c (upstream PuTTY's
 * storage API, kept textually close to upstream) and kitty_storage.c (the
 * fork's storage state: runtime registry root, portable file backend,
 * at-rest credential crypto, legacy password decrypt).
 *
 * Windows-only (HKEY in a prototype); include after putty.h/windows.h.
 * Returned char* ownership follows the comments in kitty_storage.c.
 */
#ifndef KITTY_STORAGE_H
#define KITTY_STORAGE_H

/* Read-only fallback hives (precedence: our base > old KiTTY hive > stock
 * PuTTY). Sessions present only in an older hive stay loadable; edits write
 * to our base. */
#define OLD_KITTY_HIVE_SESSIONS "Software\\9bis.com\\KiTTY\\Sessions"
#define PUTTY_HIVE_SESSIONS     "Software\\SimonTatham\\PuTTY\\Sessions"

/* Marker prefix of a DPAPI-wrapped stored secret (see ksec_protect_*). */
#define KITTY_SECRET_DPAPI_MARK "DPAPI1:"

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
const char *kitty_registry_base(void);   /* base hive, no suffix */
const char *kitty_reg_sessions(void);    /* <base>\Sessions */
const char *kitty_reg_jumplist(void);    /* <base>\Jumplist */
const char *kitty_reg_hostcas(void);     /* <base>\SshHostCAs */
const char *kitty_reg_hostkeys(void);    /* <base>\SshHostKeys */

/* ---- portable file backend (fork-native flat .ini/dir format) ---- */
int store_is_file(void);                 /* portable mode active? */
const char *kitty_session_dir(void);     /* per-session files directory */
int kitty_storage_mode(void);            /* raw g_store_mode (diagnostics) */
char *ksf_munge(const char *in);                  /* snewn'd */
char *ksf_unmunge(const char *in);                /* snewn'd */
char *ksf_list_get(struct ksf_item *h, const char *key);  /* borrowed/NULL */
void ksf_list_set(struct ksf_item **h, const char *key, const char *val);
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

/* ---- at-rest credential crypto (DPAPI1 / MPW2 + master-password state) ---- */
int kitty_secret_slot(const char *key);  /* Password=0, ProxyPassword=1, else -1 */
char *ksec_protect_registry(const char *plaintext);   /* malloc'd */
char *ksec_protect_portable(const char *plaintext);   /* malloc'd */
int ksec_unprotect(const char *stored, char **out);   /* 1/0/-1; *out malloc'd */
void ksec_after_load(int slot, const char *stored, int rv);
const char *ksec_orig_get(int slot);     /* never-wipe original blob or NULL */
int ksec_stored_is_legacy(const char *stored);
int ksec_migrate_warn_ask(void);         /* legacy->protected save consent */
int kitty_portable_password_legacy(void);
const char *kitty_secret_strip_plain(const char *stored);  /* borrowed */

/* ---- export-bundle passphrase (transport protection) ----
 * An export bundle carries its OWN password, unrelated to the store's master
 * password. Set the context around an export/import run and clear it after
 * (kitty_set_bundle_passphrase(NULL) also wipes the copy); while it is set, the
 * master password is never created, read, prompted for or written. Nothing is
 * persisted by any of this. */
void kitty_set_bundle_passphrase(const char *pass);
int  kitty_bundle_passphrase_active(void);
int  kitty_bundle_wrap_failed(void);   /* a wrap fell back to DPAPI: this-PC-only */
/* Wrap/unwrap under an explicit passphrase as a self-contained MPW2 value
 * (fresh salt per wrap, embedded), touching no store state. */
char *ksec_wrap_with_passphrase(const char *plaintext, const char *passphrase);
int   ksec_unwrap_with_passphrase(const char *stored, const char *passphrase,
                                  char **out);
unsigned ksec_cksum(const char *s);
void kitty_pwdebug(const char *fmt, ...);

/* ---- legacy (<=0.76 old-KiTTY) password decrypt ---- */
char *ksec_legacy_decrypt(const char *stored, HKEY sesskey);  /* malloc/NULL */
/* Decode a password read from an imported .ktx (unknown provenance). malloc'd
 * or NULL for empty input; try_legacy=0 for fields old KiTTY never encrypted. */
char *kitty_secret_decode_imported(const char *stored, const char *host,
                                   const char *term, int try_legacy);
char *ksec_to_utf8(char *s);

/* ---- "the configuration store changed" flag ----
 * Set by every path that writes the store (session save/delete, host keys,
 * folders, proxies); consumed by the routine .sav backup so that merely OPENING
 * a session does not write one. Lives here because windows/storage.c is
 * compiled into every variant and already includes this header. */
void kitty_store_mark_dirty(void);
int  kitty_store_take_dirty(void);   /* 1 if dirty; clears the flag */

#endif /* KITTY_STORAGE_H */
