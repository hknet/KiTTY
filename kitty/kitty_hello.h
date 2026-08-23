/*
 * KiTTY: Windows Hello presence check, for gating the agent's approval
 * moments (confirm-on-use, Accept-this-key; later the agent unlock).
 *
 * A Hello verification asks the SYSTEM to prove a human is present -
 * biometrics or the device PIN, in Windows' own protected UI. A MessageBox
 * click can be synthesized by any same-user process; this cannot. It is a
 * raised bar, not a full boundary: an approval covers one request, and an
 * admin-level attacker is out of scope.
 */

#ifndef KITTY_HELLO_H
#define KITTY_HELLO_H

#include <windows.h>

/* kitty_hello_verify outcomes. Callers FAIL CLOSED on everything except
 * VERIFIED; UNAVAILABLE vs DENIED matter only for what the user is told. */
enum {
    KITTY_HELLO_VERIFIED = 0,    /* the human is present and approved */
    KITTY_HELLO_DENIED,          /* declined, cancelled, retries exhausted */
    KITTY_HELLO_UNAVAILABLE,     /* no Hello credential / policy / no device
                                  * (notably: RDP sessions) */
    KITTY_HELLO_ERROR            /* plumbing failed; treated as a denial */
};

/* Is Hello usable right now? 1 = available, 0 = not, -1 = could not tell.
 * Runs the system availability check (no UI). Used to grey the setting. */
int kitty_hello_available(void);

/* Show the Hello prompt (parented to owner) with the given UTF-8 message
 * and wait for the outcome, pumping messages meanwhile. Returns one of the
 * KITTY_HELLO_* values above; never blocks longer than its own deadline. */
int kitty_hello_verify(HWND owner, const char *message_utf8);

/*
 * Windows Hello as a KEY PROTECTOR (KeyCredentialManager).
 *
 * A KeyCredential is a Hello-gated, TPM-backed-where-available RSA key
 * whose private half never leaves the system: every signature costs one
 * Hello verification. RSA PKCS#1 v1.5 signatures are DETERMINISTIC, so
 * signing a fixed challenge yields a stable secret only this machine and
 * account can reproduce; SHA-256 of that signature is the key-encryption
 * key (KEK) under which a random per-key secret is wrapped.
 *
 * The container is a one-line string carrying up to two independent wraps
 * of the same 32-byte secret: the Hello KEK wrap and a recovery-passphrase
 * wrap (Argon2id), each AES-256-GCM. The recovery wrap is what survives
 * machine loss, Hello re-enrolment or TPM reset - and it makes the whole
 * container testable with no Hello UI. The Argon2 cost is RECORDED in the
 * container, so it can be tuned later without locking old containers out.
 * The container holds no plaintext: without the TPM credential or the
 * recovery passphrase it is AES-GCM of random bytes.
 */

#define KITTY_HELLO_SECRET_LEN 32

/* The WebAuthn RP id our credentials live under - the ONE place it is
 * spelled. User-visible in Windows' passkey list and on every Hello
 * prompt; effectively PERMANENT once real keys are wrapped (changing it
 * orphans existing wraps onto the recovery path), free to change until
 * then. Live-verified accepted by the platform authenticator. */
#define KITTY_HELLO_RP_ID L"kapper.net kitty++"

/* Is a Hello key credential usable (KeyCredentialManager.IsSupported)?
 * 1 = yes, 0 = no, -1 = could not tell. No UI. */
int kitty_hello_key_available(void);

/* Derive the Hello KEK: open (or, if create_if_missing, create) the
 * credential and sign the fixed challenge - one Hello prompt. Returns a
 * KITTY_HELLO_* code; kek is filled only on VERIFIED. Never destroys an
 * existing credential. */
int kitty_hello_kek(unsigned char kek[32], int create_if_missing);

/* Build a container wrapping secret. hello_kek and/or recovery_passphrase
 * may be NULL, but not both; skipping the recovery wrap is the caller's
 * (warned) choice. Returns the container string (caller sfree) or NULL. */
char *kitty_hello_container_create(const unsigned char hello_kek[32],
                                   const char *recovery_passphrase,
                                   const unsigned char
                                       secret[KITTY_HELLO_SECRET_LEN]);

/* Open a container with the Hello KEK / the recovery passphrase.
 * 1 = secret_out filled, 0 = that wrap absent, -1 = refused (bad KEK or
 * passphrase, tamper, or illegal recorded KDF cost - never fed to the
 * KDF in that case). */
int kitty_hello_container_open_kek(const char *container,
                                   const unsigned char hello_kek[32],
                                   unsigned char
                                       secret_out[KITTY_HELLO_SECRET_LEN]);
int kitty_hello_container_open_recovery(const char *container,
                                        const char *passphrase,
                                        unsigned char
                                            secret_out[KITTY_HELLO_SECRET_LEN]);

/* Container queries: is this string a container at all; does it carry a
 * KCM-hello / PRF / recovery wrap? 1 / 0. */
int kitty_hello_container_valid(const char *container);
int kitty_hello_container_has_hello(const char *container);
int kitty_hello_container_has_prf(const char *container);
int kitty_hello_container_has_recovery(const char *container);

/*
 * The WebAuthn-PRF KEK source (the passkey machinery; preferred - it has
 * no WHfB gate). owner MUST be the app's real window: a synthetic host
 * window breaks the platform save flow.
 */

/* Is a PRF-capable platform authenticator present? 1/0/-1. No UI. */
int kitty_hello_prf_available(void);

/* Get (find, or create when asked - find-first avoids the replace-and-
 * orphan trap) the KiTTY PRF credential's id. KITTY_HELLO_* code; on
 * VERIFIED credid_out is filled (caller sfree). Creation shows Windows'
 * passkey save flow. */
int kitty_hello_prf_credential(HWND owner, int create_if_missing,
                               unsigned char **credid_out,
                               size_t *credidlen_out);

/* Derive the PRF KEK for that credential - one Hello prompt. */
int kitty_hello_prf_kek(HWND owner, const unsigned char *credid,
                        size_t credidlen, unsigned char kek[32]);

/* Full-control container creation: any combination of the KCM wrap (H),
 * the PRF wrap (W, needs kek + credential id together) and the recovery
 * wrap (R); at least one. */
char *kitty_hello_container_create_ex(const unsigned char hello_kek[32],
                                      const unsigned char prf_kek[32],
                                      const unsigned char *prf_credid,
                                      size_t prf_credidlen,
                                      const char *prf_owner,
                                      const char *recovery_passphrase,
                                      const unsigned char
                                          secret[KITTY_HELLO_SECRET_LEN]);

/* W-field access. A container carries ONE W PER ENROLLED ACCOUNT+MACHINE
 * (credentials are per Windows account by construction; the id inside
 * each W says whose it is, an optional owner tag says so in words).
 * prf_credid = the FIRST W's id (1/0/-1; caller sfree); open_prf tries
 * every W with the given KEK (1 = secret out, 0 = none, -1 = refused). */
int kitty_hello_container_prf_credid(const char *container,
                                     unsigned char **credid_out,
                                     size_t *credidlen_out);
int kitty_hello_container_open_prf(const char *container,
                                   const unsigned char prf_kek[32],
                                   unsigned char
                                       secret_out[KITTY_HELLO_SECRET_LEN]);

/* Multi-account W access: count; the index-th W's owner tag (caller
 * sfree, NULL if untagged/absent) and credential id (1/0/-1); the index
 * of the W carrying a given credential id (-1 = none); all owners as one
 * readable line for the "protected elsewhere" message (caller sfree).
 * append_prf adds a W for another account WITHOUT touching the existing
 * text (each wrap is an independent GCM blob); returns the new container
 * (caller sfree) or NULL. */
int kitty_hello_container_w_count(const char *container);
char *kitty_hello_container_w_owner(const char *container, int index);
int kitty_hello_container_w_credid(const char *container, int index,
                                   unsigned char **credid_out,
                                   size_t *credidlen_out);
int kitty_hello_container_find_w(const char *container,
                                 const unsigned char *credid,
                                 size_t credidlen);
char *kitty_hello_container_owners_text(const char *container);
char *kitty_hello_container_append_prf(const char *container,
                                       const unsigned char prf_kek[32],
                                       const unsigned char *prf_credid,
                                       size_t prf_credidlen,
                                       const char *owner,
                                       const unsigned char
                                           secret[KITTY_HELLO_SECRET_LEN]);

/* This account's PRF credential id from the platform store, no UI
 * (1 = found, caller sfree; 0 = none; -1 = cannot tell), and the index
 * of the W in a container that belongs to it (-1 = none of ours). */
int kitty_hello_prf_my_credid(unsigned char **credid_out,
                              size_t *credidlen_out);
int kitty_hello_container_my_w(const char *container);

/* DOMAIN\user@MACHINE - the owner tag this account writes. Caller
 * sfree. */
char *kitty_hello_owner_tag(void);

/* A fresh 32-byte secret from the OS CSPRNG (1 = ok, 0 = failed). */
int kitty_hello_new_secret(unsigned char secret[KITTY_HELLO_SECRET_LEN]);

/* Stage + HRESULT of the last PRF operation (find/create/assert), for the
 * apps' audit lines. No secrets. Static text, never NULL. */
const char *kitty_hello_last_detail(void);

/* The PRINTED form of the secret (fixed for ever): 8 hex groups + a
 * check group. This string IS the PPK's literal passphrase, so a
 * printout opens the key in any PuTTY-compatible tool. from_text is
 * tolerant of case/dashes/spaces and verifies the check group. */
char *kitty_hello_secret_text(const unsigned char
                                  secret[KITTY_HELLO_SECRET_LEN]);
int kitty_hello_secret_from_text(const char *text,
                                 unsigned char
                                     secret_out[KITTY_HELLO_SECRET_LEN]);

/* Which Hello mechanism a wrap used. */
enum {
    KITTY_HELLO_SOURCE_NONE = 0,
    KITTY_HELLO_SOURCE_PRF,      /* WebAuthn platform hmac-secret */
    KITTY_HELLO_SOURCE_KCM       /* KeyCredentialManager signature */
};

/* The POLICY layer apps call: PRF where available, else KCM, else
 * refuse; unwrap dispatches on what the container carries. owner = the
 * app's real window. KITTY_HELLO_* codes; wrap yields the container
 * (caller sfree) and, optionally, which source was used. */
int kitty_hello_wrap_auto(HWND owner,
                          const unsigned char secret[KITTY_HELLO_SECRET_LEN],
                          const char *recovery_passphrase,
                          char **container_out, int *source_out);
int kitty_hello_unwrap_auto(HWND owner, const char *container,
                            unsigned char
                                secret_out[KITTY_HELLO_SECRET_LEN]);

/* Add THIS account+machine as a door to an existing container, given
 * the secret (opened through any other door first). PRF only; finds or
 * creates this account's credential, one Hello prompt. Yields the new
 * container (caller sfree); VERIFIED with the unchanged text when this
 * account is already enrolled. */
int kitty_hello_enrol_auto(HWND owner, const char *container,
                           const unsigned char secret[KITTY_HELLO_SECRET_LEN],
                           char **container_out);

/*
 * Hands-on diagnostics - available only to targets that define
 * KITTY_HELLO_DIAG (the test binary); shipping apps compile none of it.
 */
#ifdef KITTY_HELLO_DIAG

/* Sign the fixed challenge twice and compare: proves this machine's
 * credential signs deterministically (two Hello prompts). Returns a
 * KITTY_HELLO_* code; VERIFIED means byte-identical signatures, DENIED
 * covers both refusal and a non-deterministic signer (msg says which;
 * caller sfree). */
int kitty_hello_kek_selftest(char **msg);

/* Diagnostic variant: nohost skips the transient host window so the
 * caller's real foreground window stays in charge (the WebAuthn probe
 * proved the synthetic 1x1 window is what breaks creation UI). */
int kitty_hello_kek_selftest_ex(int nohost, char **msg);

/* Turn on stderr stage tracing for the hands-on diagnostics (the test
 * binary's selftest mode). Never enabled inside the agent. */
void kitty_hello_trace_enable(void);

/* Probe the OTHER Hello-gated KEK source: the platform authenticator's
 * WebAuthn hmac-secret/PRF extension (the passkey path, which has no
 * WHfB gate). Creates a throwaway platform credential, runs two PRF
 * assertions with a fixed salt (a Hello prompt each), compares the
 * secrets and DELETES the credential. VERIFIED = deterministic 32-byte
 * secret, viable as a KEK. msg says what happened (caller sfree). */
int kitty_hello_webauthn_probe(char **msg);

/* Diagnostic variants of the probe: choose the RP id and how (whether)
 * the PRF capability is requested at creation. PRF_NONE creates and
 * deletes a credential without any PRF request - it discriminates
 * "creation is broken" from "the PRF request breaks creation". */
enum {
    KITTY_HELLO_PRF_NONE = 0,
    KITTY_HELLO_PRF_ENABLE,      /* WebAuthn-level bEnablePrf */
    KITTY_HELLO_PRF_HMAC_EXT     /* CTAP "hmac-secret" BOOL extension */
};
/* OR-able diagnostic flags for probe_ex's flags argument. */
#define KITTY_HELLO_WA_FGWND   1  /* owner = the real foreground window,
                                   * not the transient 1x1 host */
#define KITTY_HELLO_WA_UVPREF  2  /* user verification "preferred" (the
                                   * browser default) instead of required */
int kitty_hello_webauthn_probe_ex(const WCHAR *rp_id, int prf_mode,
                                  int flags, char **msg);

/* List what the PLATFORM (NGC) store holds - no UI, no prompt. Returns
 * 1 = listing in *out, 0 = store empty, -1 = failed (caller sfree). */
int kitty_hello_webauthn_list(char **out);

#endif /* KITTY_HELLO_DIAG */

#endif /* KITTY_HELLO_H */
