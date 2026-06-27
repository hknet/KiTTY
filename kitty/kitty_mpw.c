/*
 * KiTTY master-password (MPW1) secret envelope — DPAPI Phase 2.
 *
 * Machine-independent at-rest encryption of a stored secret under a user
 * passphrase, so secrets survive a move to another PC (unlike DPAPI, which is
 * machine-bound). Crypto mirrors PuTTY's vetted PPK v3 at-rest format and reuses
 * its primitives (no new crypto):
 *
 *   key material = Argon2id(passphrase, master-salt) -> AESkey(32) || MACkey(32)
 *                  derived ONCE per unlock from a single per-store master salt
 *                  (NOT per-secret: that would mean an Argon2 hash per password).
 *   per secret   = random IV(16); AES-256-CBC(AESkey, IV) over PKCS#7-padded
 *                  plaintext; HMAC-SHA-256(MACkey) over IV||ciphertext.
 *   stored blob  = "MPW1:" + base64( IV(16) || ciphertext || MAC(32) )
 *
 * A wrong passphrase derives the wrong MACkey, so unprotect() fails the HMAC
 * check cleanly (== "wrong master password"), distinct from "no secret".
 * AES-CBC (not GCM) is deliberate: under a SHARED key, GCM is catastrophic on IV
 * reuse; CBC's IV-reuse failure is far milder.
 */
#include <string.h>
#include "putty.h"
#include "ssh.h"
#include "kitty_mpw.h"

#define MPW_MARK        "MPW1:"
#define MPW_IV_LEN      16
#define MPW_MAC_LEN     32      /* ssh_hmac_sha256 output */
#define MPW_AESKEY_LEN  32      /* AES-256 */
#define MPW_MACKEY_LEN  32
/* Argon2id cost: interactive-friendly, mirrors PPK v3 defaults (~tenths of a sec). */
#define MPW_ARGON_MEM       8192    /* KiB */
#define MPW_ARGON_PASSES    13
#define MPW_ARGON_PARALLEL  1

void kitty_mpw_derive(const char *passphrase,
                      const unsigned char *salt, int saltlen,
                      unsigned char derived[KITTY_MPW_DERIVED_LEN])
{
    strbuf *sb = strbuf_new_nm();
    ptrlen empty = PTRLEN_LITERAL("");
    argon2(Argon2id, MPW_ARGON_MEM, MPW_ARGON_PASSES, MPW_ARGON_PARALLEL,
           KITTY_MPW_DERIVED_LEN,
           make_ptrlen(passphrase, strlen(passphrase)),
           make_ptrlen(salt, saltlen), empty, empty, sb);
    memcpy(derived, sb->u, KITTY_MPW_DERIVED_LEN);
    strbuf_free(sb);
}

void kitty_mpw_random_salt(unsigned char *salt, int len)
{
    random_read(salt, len);
}

char *kitty_mpw_protect(const char *plaintext,
                        const unsigned char derived[KITTY_MPW_DERIVED_LEN])
{
    if (!plaintext || !plaintext[0]) return dupstr("");   /* empty == no secret */
    int ptlen = (int)strlen(plaintext);
    int pad = MPW_IV_LEN - (ptlen % MPW_IV_LEN);   /* PKCS#7: always 1..16 */
    int ctlen = ptlen + pad;
    int blen = MPW_IV_LEN + ctlen + MPW_MAC_LEN;
    unsigned char *buf = snewn(blen, unsigned char);

    random_read(buf, MPW_IV_LEN);                  /* fresh IV */
    unsigned char *ct = buf + MPW_IV_LEN;
    memcpy(ct, plaintext, ptlen);
    memset(ct + ptlen, pad, pad);                  /* PKCS#7 pad bytes */
    aes256_encrypt_pubkey(derived, buf, ct, ctlen);

    ssh2_mac *mac = ssh2_mac_new(&ssh_hmac_sha256, NULL);
    ssh2_mac_setkey(mac, make_ptrlen(derived + MPW_AESKEY_LEN, MPW_MACKEY_LEN));
    ssh2_mac_start(mac);
    put_data(mac, buf, MPW_IV_LEN + ctlen);        /* MAC over IV||ciphertext */
    ssh2_mac_genresult(mac, ct + ctlen);
    ssh2_mac_free(mac);

    strbuf *b64 = base64_encode_sb(make_ptrlen(buf, blen), 0);
    char *res = dupcat(MPW_MARK, b64->s);
    strbuf_free(b64);
    smemclr(buf, blen);
    sfree(buf);
    return res;
}

int kitty_mpw_unprotect(const char *stored,
                        const unsigned char derived[KITTY_MPW_DERIVED_LEN],
                        char **out)
{
    size_t marklen = strlen(MPW_MARK);
    *out = NULL;
    if (!stored || !stored[0]) { *out = dupstr(""); return 0; }
    if (strncmp(stored, MPW_MARK, marklen) != 0) { *out = dupstr(stored); return 1; }

    strbuf *raw = base64_decode_sb(ptrlen_from_asciz(stored + marklen));
    if (!raw) { *out = dupstr(""); return -1; }
    int ctlen = (int)raw->len - MPW_IV_LEN - MPW_MAC_LEN;
    if (ctlen <= 0 || (ctlen % MPW_IV_LEN) != 0) {
        strbuf_free(raw); *out = dupstr(""); return -1;
    }
    unsigned char *iv  = raw->u;
    unsigned char *ct  = raw->u + MPW_IV_LEN;
    unsigned char *macs = raw->u + MPW_IV_LEN + ctlen;

    unsigned char macc[MPW_MAC_LEN];
    ssh2_mac *mac = ssh2_mac_new(&ssh_hmac_sha256, NULL);
    ssh2_mac_setkey(mac, make_ptrlen(derived + MPW_AESKEY_LEN, MPW_MACKEY_LEN));
    ssh2_mac_start(mac);
    put_data(mac, raw->u, MPW_IV_LEN + ctlen);
    ssh2_mac_genresult(mac, macc);
    ssh2_mac_free(mac);
    if (!smemeq(macc, macs, MPW_MAC_LEN)) {        /* wrong passphrase / tampered */
        smemclr(raw->u, raw->len); strbuf_free(raw); *out = dupstr(""); return -1;
    }

    aes256_decrypt_pubkey(derived, iv, ct, ctlen);
    int pad = ct[ctlen - 1];
    if (pad < 1 || pad > MPW_IV_LEN || pad > ctlen) {
        smemclr(raw->u, raw->len); strbuf_free(raw); *out = dupstr(""); return -1;
    }
    for (int i = 0; i < pad; i++)
        if (ct[ctlen - 1 - i] != pad) {
            smemclr(raw->u, raw->len); strbuf_free(raw); *out = dupstr(""); return -1;
        }
    int ptlen = ctlen - pad;
    char *pt = snewn(ptlen + 1, char);
    memcpy(pt, ct, ptlen); pt[ptlen] = '\0';
    smemclr(raw->u, raw->len); strbuf_free(raw);
    *out = pt;
    return 1;
}

/* Self-register with the storage layer (windows/storage.c) at startup, so any
 * tool that LINKS this file gets master-password support without touching its
 * main(); tools that don't link it leave MPW unavailable (-> DPAPI fallback). */
extern void kitty_register_mpw_crypto(
    void (*)(const char *, const unsigned char *, int, unsigned char *),
    char *(*)(const char *, const unsigned char *),
    int  (*)(const char *, const unsigned char *, char **),
    void (*)(unsigned char *, int));
static void __attribute__((constructor)) kitty_mpw_autoreg(void)
{
    kitty_register_mpw_crypto(kitty_mpw_derive, kitty_mpw_protect,
                              kitty_mpw_unprotect, kitty_mpw_random_salt);
}
