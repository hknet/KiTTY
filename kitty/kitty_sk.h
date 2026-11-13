/*
 * kitty_sk.h - FIDO security keys (sk-*@openssh.com) through webauthn.dll,
 * for the program that holds such keys (kageant). The SSH side is
 * crypto/sk-keys.c; the WebAuthn call lives in kitty_hello.c beside the
 * other webauthn.dll calls and their pump.
 */
#ifndef KITTY_SK_H
#define KITTY_SK_H

#include <windows.h>
/* needs ssh.h first (SkAssertion, ptrlen): it has no include guard */

/* How long Windows waits for the device and the touch. */
#ifndef KITTY_SK_TIMEOUT_MS
#define KITTY_SK_TIMEOUT_MS 60000
#endif

/* The wait in force (a test build may shorten it). */
extern int kitty_sk_timeout_ms;

/* webauthn.dll present with what one signature needs (1), or not (0). */
int kitty_sk_available(void);

/* One signature from the security key holding key_handle; data goes to
 * the device as the client data. Pumps the UI thread like every Hello
 * call. NULL on success, else the reason. */
const char *kitty_sk_assert(HWND owner, ptrlen application,
                            ptrlen key_handle, unsigned key_flags,
                            ptrlen data, SkAssertion *out);

/* A new key on the security key (kittygen): Ed25519, or NIST p256 when
 * ecdsa. uv requires the PIN at every use. Hands back the registration's
 * authenticatorData and the credential id (sk_key_from_registration makes
 * the SSH key of them). Windows prompts for the touch and the PIN. NULL on
 * success, else the reason. */
const char *kitty_sk_make(HWND owner, bool ecdsa, bool uv,
                          ptrlen application, strbuf *auth_data,
                          strbuf *cred_id);

#endif
