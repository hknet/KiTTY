/*
 * kitty_hello_keys.h - kageant's side of Hello-protected keys: the sidecar
 * beside a PPK, the protect/unlock/enrol operations built on the shared
 * container layer in kitty_hello.h. No UI in here beyond the Hello prompts
 * themselves; the dialogs live in windows/pageant.c.
 */
#ifndef KITTY_HELLO_KEYS_H
#define KITTY_HELLO_KEYS_H

#include <windows.h>

/* The sidecar: "<keyfile>.hello" beside the protected PPK. Caller sfree.
 * has_sidecar is a plain existence test (no read). */
char *kageant_hello_sidecar_path(const char *keypath);
int kageant_hello_has_sidecar(const char *keypath);

/* Read / write the sidecar's container text (caller sfree / 1 = ok). */
char *kageant_hello_read_sidecar(const char *keypath);
int kageant_hello_write_sidecar(const char *keypath, const char *container);

/* The protected copy's default path: "<name>-hello.ppk" beside the
 * source (any other extension is kept as "<name>-hello.<ext>"). */
char *kageant_hello_default_destpath(const char *srcpath);

/* Outcomes of unlock / translate. */
enum {
    KAGEANT_HELLO_OK = 0,      /* passphrase_out filled */
    KAGEANT_HELLO_NOSIDECAR,   /* not a Hello-protected key */
    KAGEANT_HELLO_DENIED,      /* the Hello prompt was refused: final */
    KAGEANT_HELLO_NODOOR,      /* no door of this account in the sidecar:
                                * owners_out names who wrapped it */
    KAGEANT_HELLO_ERROR        /* unreadable sidecar, plumbing failure */
};

/* Open the key's sidecar through this account's Hello door. On OK,
 * passphrase_out is the PPK's literal passphrase (the printed-secret
 * text; caller burnstr). On NODOOR, owners_out (caller sfree) is the
 * readable owner line. owner = a REAL window of the app. */
int kageant_hello_unlock(const char *keypath, HWND owner,
                         char **passphrase_out, char **owners_out);

/* Turn what the user typed at a passphrase prompt for a Hello-protected
 * key into the PPK's literal passphrase: the recovery/original
 * passphrase opens the R wrap; a printed secret (any case, dashes or
 * spaces) is canonicalised. NULL when neither applies (the caller may
 * still try the text as-is). via_recovery_out = 1 when the R door was
 * the one that opened. Caller burnstr. */
char *kageant_hello_translate(const char *keypath, const char *typed,
                              int *via_recovery_out);

/* Protect a key file: write the protected copy at destpath (the SAME key,
 * passphrase = a fresh printed secret), write its sidecar (this
 * account's Hello door + the R door keyed on recovery_pass), never touch
 * srcpath. srcpass = the source's passphrase (NULL/"" = none).
 * recovery_pass NULL = "use srcpass"; "" = NO recovery wrap at all
 * (Windows Hello + printed secret only - warn first); with NULL and an
 * empty srcpass the call is refused (it would protect nothing). On success printed_out (caller
 * burnstr) is the secret's printed form - show it ONCE. err_out (caller
 * sfree) explains a failure. Returns a KAGEANT_HELLO_* code. */
/* As kageant_hello_protect, plus the sidecar-bound choice: with
 * sidebound != 0 the printout is a recovery CODE (opens the key only
 * together with the .hello file, in KiTTY tools) - the file's literal
 * passphrase is then written nowhere. */
int kageant_hello_protect_ex(HWND owner, const char *srcpath,
                             const char *srcpass, const char *recovery_pass,
                             const char *destpath, int sidebound,
                             char **printed_out, char **err_out);

int kageant_hello_protect(HWND owner, const char *srcpath,
                          const char *srcpass, const char *recovery_pass,
                          const char *destpath, char **printed_out,
                          char **err_out);

/* Add this account+machine's Hello door to an existing sidecar, given
 * the PPK's literal passphrase (obtained through another door).
 * Returns a KAGEANT_HELLO_* code; err_out on failure. */
int kageant_hello_enrol(HWND owner, const char *keypath,
                        const char *passphrase, char **err_out);

/* Is THIS account already enrolled in the key's sidecar? 1/0, -1 = no
 * sidecar or unreadable. No UI. */
int kageant_hello_enrolled_here(const char *keypath);

/* Can this machine offer Hello protection at all (PRF or KCM)? 1/0.
 * No UI. */
int kageant_hello_offerable(void);

#endif /* KITTY_HELLO_KEYS_H */
