#ifndef KITTY_MPW_H
#define KITTY_MPW_H

/* KiTTY master-password (MPW1) secret envelope - see kitty_mpw.c. */

#define KITTY_MPW_DERIVED_LEN 64   /* AES-256 key (32) || HMAC-SHA-256 key (32) */
#define KITTY_MPW_SALT_LEN    16

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
