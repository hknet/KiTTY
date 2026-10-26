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
/* malloc'd; NULL when the wrap was refused (see the _kept forms) */
char *ksec_protect_registry(const char *plaintext);
char *ksec_protect_portable(const char *plaintext);
/* A save's wrap: NULL and *locked = 1 when it is refused - the master-password
 * policy holds and the master password is locked, or Windows' DPAPI failed.
 * The caller keeps what is stored (never DPAPI or the plaintext in its place)
 * and reports it with ksec_report_locked_save, which words the reason. */
char *ksec_protect_registry_kept(const char *plaintext, int *locked);
char *ksec_protect_portable_kept(const char *plaintext, int *locked);
#ifdef KITTY_SELFTEST_HOOKS
/* test_storage_roundtrip only: every DPAPI wrap fails with err (0 = off) */
void kitty_selftest_dpapi_fail(DWORD err);
#endif
/* what a save kept (ksec_report_locked_save) */
#define KSEC_KEPT_SESSION 0   /* a session's password; where = its file */
#define KSEC_KEPT_PROXY   1   /* a named proxy's password; where = its name */
#define KSEC_KEPT_SCRIPT  2   /* a session's login script; where = the session */
#define KSEC_KEPT_EXPORT  3   /* a .ktx export; where = the file */
void ksec_report_locked_save(int what, const char *where);
int  kitty_mpw_kept_count(void);   /* reports so far: -importdir's exit code */
int  kitty_mpw_kept_nosetup_count(void);   /* of those: no master password set up */
int  kitty_mpw_kept_dpapi_count(void);     /* of those: DPAPI failed */
/* a do-and-exit run's report lines that no output took (kitty_mpw_gui.c),
 * kept for its result; *more = the lines past the limit */
void ksec_kept_hold(const char *line);
const char *kitty_mpw_kept_held(int *more);
int ksec_unprotect(const char *stored, char **out);   /* 1/0/-1; *out malloc'd */
void ksec_after_load(int slot, const char *stored, int rv);
const char *ksec_orig_get(int slot);     /* never-wipe original blob or NULL */
int ksec_stored_is_legacy(const char *stored);
/* 1 = keep `stored` on a save of `plaintext` (same password, current form) */
int ksec_stored_keeps(const char *stored, const char *plaintext, int portable);
int kitty_portable_password_legacy(void);
/* kitty.ini PortablePasswordProtection=dpapi: portable secrets are protected
 * with DPAPI and no master password is created or prompted for. Mutually
 * exclusive with -masterpwfile, which cmdline.c refuses in this mode. */
int kitty_portable_password_dpapi(void);
/* One-shot at startup, portable stores only: copy the master-password state out
 * of the registry into the store's own Security\ folder, but only when this
 * store really has values wrapped with it. Returns 1 if it copied, so the
 * caller can advise the user to carry Security\ to any OTHER portable KiTTY
 * of theirs. */
int kitty_migrate_portable_mpw_state(void);
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
/* The portable copy: session files in the folder store's "key=value" lines,
 * not the .ktx "key\value\" ones. */
void kitty_set_bundle_store_form(int on);
int  kitty_bundle_store_form(void);
void kitty_clear_bundle_context(void);     /* always call when the run ends */
/* Passwords the bundle left out (their wrap failed; never DPAPI in their
 * place): how many, and the summary's lines for them - snewn'd, or NULL for
 * none. Both before kitty_clear_bundle_context. */
int  kitty_bundle_wrap_failed(void);
char *kitty_bundle_wrap_report(void);
/* what an export wraps (kitty_secret_wrap_export) */
#define KSEC_EXPORT_SESSION_PW   0   /* a session's password; where = the session */
#define KSEC_EXPORT_SESSION_PXPW 1   /* a session's proxy password */
#define KSEC_EXPORT_PROXY_PW     2   /* a named proxy's password; where = its name */
/* An export's wrap (kitty_secretstore.c): in a bundle, NULL and *skipped = 1
 * when the wrap failed (nothing to write, the summary lists it); outside one,
 * NULL and *locked = 1 when it is refused (the master password locked, DPAPI
 * failing). malloc'd. */
char *kitty_secret_wrap_export(const char *plaintext, int what, const char *where,
                               int *locked, int *skipped);
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
/* The login script in memory (kitty.c ScriptFileContent): held wrapped for
 * this process, opened only for one compare. _wrap: the blob's first `len`
 * bytes held (NULL for no script or one used up); _open: a plain copy, *len
 * bytes plus two zero bytes, which the caller smemclr()s (*len + 2) and
 * sfree()s, NULL when it cannot be opened; _next: the script after its first
 * expect/send pair, held (NULL when none is left), from an open copy; _free:
 * wiped and freed. */
char *kitty_loginscript_mem_wrap(const unsigned char *blob, size_t len);
unsigned char *kitty_loginscript_mem_open(const char *held, size_t *len);
char *kitty_loginscript_mem_next(const unsigned char *blob, size_t len);
void kitty_loginscript_mem_free(char *held);
void kitty_mpw_consume_handle_str(const char *s);
void kitty_mpw_consume_cmdline_switch(char *cl);   /* -launcher / -manage: "-mpwkey x" consumed + blanked */
HANDLE kitty_mpw_export_inherit_blob(const char *prefix, char *tok, size_t toklen);
char *kitty_mpw_import_inherit_blob(char *p);
int kitty_mpw_startup_unlock(void);
void kitty_register_mpw_crypto( void (*derive)(const char *, const unsigned char *, int, unsigned char *), char *(*protect)(const char *, const unsigned char *), int (*unprotect)(const char *, const unsigned char *, char **), void (*randsalt)(unsigned char *, int));
int kitty_secret_is_marked(const char *stored);
int kitty_secret_unwrap(const char *stored, char **out);
char *kitty_secret_wrap_current_backend(const char *plaintext);
char *kitty_secret_wrap_current_backend_kept(const char *plaintext, int *locked);
char *kitty_session_fname_munge(const char *name);
char *kitty_session_fname_unmunge(const char *name);
void kitty_set_defer_mpw_prompt(int on);
void kitty_set_master_passphrase(const char *pass);
void kitty_set_master_pw_prompt(char *(*fn)(int creating));
void kitty_set_master_pw_wrong(void (*fn)(int supplied));
void kitty_set_master_pw_store_failed(void (*fn)(const char *reason));   /* salt/verifier not stored */
void kitty_set_master_pw_locked_save(void (*fn)(const char *cap, const char *msg));    /* a save kept a stored secret */
/* A user-started action (Save, /save, Export, Export All, Make a portable
 * copy): the master password is prompted for even after an earlier cancel. */
void kitty_mpw_user_action_begin(void);
void kitty_mpw_user_action_end(void);
/* The prompt being shown: try no of max (no = 0 outside a prompt). */
void kitty_mpw_prompt_try(int *no, int *max);
/* -masterpwfile, applied (cmdline.c): from the option, or by the console tools
 * before any argument (kitty_console_store.c). fn NULL = "-", standard input. */
extern bool kitty_masterpwfile_done;
void kitty_masterpwfile_apply(const char *shown, Filename *fn);
/* ...the same, found on the command line before any argument is processed */
void kitty_masterpwfile_prescan(void);
void kitty_set_portable_password_protection(const char *mode);

/* "Comment out defaults" in one registry session (windows/storage.c): each
 * value equal to defkey's is deleted; the count, or -1 when one could not
 * be. Windows-only (HKEY), so here rather than in kitty_storage.h. */
int kitty_reg_delete_defaults(HKEY k, HKEY defkey);

#endif /* KITTY_SECRETSTORE_H */
