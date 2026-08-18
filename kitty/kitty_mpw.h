#ifndef KITTY_MPW_H
#define KITTY_MPW_H

/* KiTTY master-password (MPW1) secret envelope - see kitty_mpw.c. */

#define KITTY_MPW_DERIVED_LEN 64   /* AES-256 key (32) || HMAC-SHA-256 key (32) */
#define KITTY_MPW_SALT_LEN    16

/*
 * Argon2id cost for the master password: interactive-friendly, mirroring the
 * PPK v3 defaults (~tenths of a second).
 *
 * Declared HERE, rather than privately in kitty_mpw.c, so a test can check them.
 * PuTTY 0.85 added argon2_params_bad() and now validates PPK save/load
 * parameters against it. Ours are compile-time constants, so nothing
 * user-supplied reaches the KDF and there is nothing to validate at runtime -
 * but a later tuning pass (less memory, more parallelism) could still land on a
 * combination that function rejects, and the first symptom would be master
 * passwords that stop working. test/test_mpw_params.c asserts they stay legal.
 *
 * They must ALSO never simply be changed: neither the MPW1 nor the MPW2
 * envelope records the cost, so a new value would lock out every secret already
 * stored - unprotect would just fail to verify, indistinguishable from a wrong
 * passphrase. Raising the cost needs a versioned envelope; the plan is in
 * design/TASK_mpw_cost_migration.md.
 */
#define KITTY_MPW_ARGON_MEM       8192    /* KiB */
#define KITTY_MPW_ARGON_PASSES    13
#define KITTY_MPW_ARGON_PARALLEL  1

/* Argon2id(passphrase, salt) -> 64-byte key material. Slow by design; call once
 * per unlock and cache the result. */
void  kitty_mpw_derive(const char *passphrase,
                       const unsigned char *salt, int saltlen,
                       unsigned char derived[KITTY_MPW_DERIVED_LEN]);
/* CSPRNG bytes (for the per-store master salt). */
void  kitty_mpw_random_salt(unsigned char *salt, int len);
/* plaintext -> malloc'd "MPW1:<base64>" (sfree). Empty plaintext still encrypts. */
char *kitty_mpw_protect(const char *plaintext,
                        const unsigned char derived[KITTY_MPW_DERIVED_LEN]);
/* stored -> *out (sfree'd by caller). 1=ok, 0=absent/empty, -1=wrong key/corrupt.
 * A value without the MPW1: marker is passed through unchanged (rv 1). */
int   kitty_mpw_unprotect(const char *stored,
                          const unsigned char derived[KITTY_MPW_DERIVED_LEN],
                          char **out);

#endif
