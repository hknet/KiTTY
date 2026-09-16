/*
 * kitty_secretstore.h - the at-rest credential crypto of the settings store
 * (kitty_secretstore.c): the DPAPI1 / MPW2 wrap and unwrap behind
 * windows/storage.c's read and write of a password, the master-password
 * unlock state and its prompt hooks, the export-bundle passphrase, the
 * legacy (<=0.76 old-KiTTY) password decrypt and the .ktx forced-export
 * glue. The marker macros (KITTY_SECRET_*_MARK) and the hive ids stay in
 * kitty_storage.h, which both halves' callers already include.
 *
 * Windows-only (HKEY, HANDLE in prototypes); include after putty.h.
 * Returned char* ownership follows the comments in kitty_secretstore.c.
 */
#ifndef KITTY_SECRETSTORE_H
#define KITTY_SECRETSTORE_H
#include "putty.h"
#include "kitty_storage.h"   /* KITTY_SECRET_*_MARK, KSEC_HIVE_* */

const char *kitty_mpw_verify_token(void); /* master-password verifier plaintext */
/* Import from another store: open one MPW2 value with a given passphrase.
 * Pure - this store's unlock state is not consulted or changed. */
char *kitty_mpw2_unprotect_with_passphrase(const char *stored, const char *passphrase);
int kitty_secret_is_mpw(const char *stored);     /* 2 = MPW2, 1 = MPW1, 0 = no */

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
/* kitty.ini PortablePasswordProtection=dpapi: portable secrets are protected
 * with DPAPI and no master password is created or asked for. Mutually exclusive
 * with -masterpwfile, which cmdline.c refuses in this mode. */
int kitty_portable_password_dpapi(void);
/* One-shot at startup, portable stores only: copy the master-password state out
 * of the registry into the store's own Security\ folder, but only when this
 * store really has values wrapped with it. Returns 1 if it copied, so the caller
 * can tell the user to carry Security\ to any OTHER portable KiTTY of theirs. */
int kitty_migrate_portable_mpw_state(void);
/* Delete MasterPwSalt/MasterPwVerifier when a scan of the active store finds
 * nothing wrapped with them - the leftovers of the old export behaviour, which
 * created a master password as a side effect. Silent; keeps them when the
 * master password is genuinely in use. Call once at startup. */
void kitty_retire_orphan_master_password(void);
const char *kitty_secret_strip_plain(const char *stored);  /* borrowed */

/* ---- export-bundle passphrase (transport protection) ----
 * An export bundle carries its OWN password, unrelated to the store's master
 * password. Set the context around an export/import run and clear it after
 * (kitty_set_bundle_passphrase(NULL) also wipes the copy); while it is set, the
 * master password is never created, read, prompted for or written. Nothing is
 * persisted by any of this. */
void kitty_set_bundle_passphrase(const char *pass);
void kitty_set_bundle_dpapi_only(int on);  /* "this PC + this account only" */
/* Import direction: the passphrase opens the bundle but never re-protects what
 * is saved - imported secrets take the DESTINATION store's protection. */
void kitty_set_bundle_import(int on);
void kitty_clear_bundle_context(void);     /* always call when the run ends */
int  kitty_bundle_wrap_failed(void);   /* a wrap fell back to DPAPI: this-PC-only */
/* Wrap/unwrap under an explicit passphrase as a self-contained MPW2 value
 * (fresh salt per wrap, embedded), touching no store state. */
char *ksec_wrap_with_passphrase(const char *plaintext, const char *passphrase);
int   ksec_unwrap_with_passphrase(const char *stored, const char *passphrase,
                                  char **out);

/* ---- legacy (<=0.76 old-KiTTY) password decrypt ---- */
char *ksec_legacy_decrypt(const char *stored, HKEY sesskey);  /* malloc/NULL */
/* Decode a password read from an imported .ktx (unknown provenance). malloc'd
 * or NULL for empty input; try_legacy=0 for fields old KiTTY never encrypted. */
char *kitty_secret_decode_imported(const char *stored, const char *host,
                                   const char *term, int try_legacy);
char *ksec_to_utf8(char *s);

/* ---- exported from kitty/kitty_secretstore.c ---- */
char *kitty_loginscript_blob_to_lines(const unsigned char *blob, int len);
unsigned char *kitty_loginscript_lines_to_blob(const char *text, int *outlen);
void kitty_mpw_consume_handle_str(const char *s);
HANDLE kitty_mpw_export_inherit_blob(const char *prefix, char *tok, size_t toklen);
char *kitty_mpw_import_inherit_blob(char *p);
int kitty_mpw_startup_unlock(void);
void kitty_register_mpw_crypto( void (*derive)(const char *, const unsigned char *, int, unsigned char *), char *(*protect)(const char *, const unsigned char *), int (*unprotect)(const char *, const unsigned char *, char **), void (*randsalt)(unsigned char *, int));
int kitty_secret_is_marked(const char *stored);
int kitty_secret_unwrap(const char *stored, char **out);
char *kitty_secret_wrap_current_backend(const char *plaintext);
char *kitty_secret_wrap_portable(const char *plaintext);
char *kitty_session_fname_munge(const char *name);
char *kitty_session_fname_unmunge(const char *name);
void kitty_set_defer_mpw_prompt(int on);
void kitty_set_legacy_migrate_warn(int (*fn)(void));
void kitty_set_master_passphrase(const char *pass);
void kitty_set_master_pw_prompt(char *(*fn)(int creating));
void kitty_set_portable_password_protection(const char *mode);

#endif /* KITTY_SECRETSTORE_H */
