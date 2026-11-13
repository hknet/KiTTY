/*
 * sk-keys.c - KiTTY: FIDO security keys in SSH (OpenSSH's
 * sk-ssh-ed25519@openssh.com and sk-ecdsa-sha2-nistp256@openssh.com,
 * PROTOCOL.u2f).
 *
 * Such a key is an ordinary Ed25519 or NIST p256 public key plus an
 * application string ("ssh:"); the "private key" holds no secret, only
 * the flags and the key handle the device needs to find its own secret.
 * The device signs
 *
 *     SHA256(application) || flags || counter || SHA256(message)
 *
 * which is exactly a WebAuthn authenticatorData of 37 bytes followed by a
 * client-data hash, when the client data handed to the authenticator is
 * the message itself. Each key here wraps the plain key of its curve for
 * the public half and for verification; signing goes to the provider the
 * program holding the key has set (kageant: webauthn.dll). Without one,
 * signing fails with a reason (sk_sign_error).
 */

#include <string.h>

#include "ssh.h"
#include "putty.h"

struct sk_extra {
    const ssh_keyalg *base;     /* the plain key type of the same curve */
    bool ecdsa;
    const char *desc;
};

typedef struct sk_key {
    ssh_key *base;              /* the public key as its plain type */
    strbuf *application;
    unsigned char flags;
    strbuf *key_handle;         /* NULL on a public-only key */
    strbuf *reserved;
    ssh_key sshk;
} sk_key;

sk_assert_fn sk_assert_provider = NULL;

static char *sk_last_error = NULL;

const char *sk_sign_error(void)
{
    return sk_last_error ? sk_last_error : "";
}

static void sk_set_error(const char *text)
{
    sfree(sk_last_error);
    sk_last_error = text ? dupstr(text) : NULL;
}

bool ssh_keyalg_is_sk(const ssh_keyalg *alg)
{
    return alg == &ssh_sk_ed25519 || alg == &ssh_sk_ecdsa_nistp256;
}

static const struct sk_extra *sk_extra_of(const ssh_keyalg *alg)
{
    return (const struct sk_extra *)alg->extra;
}

/*
 * Read the public half that follows the type string: the curve's own
 * fields (ed25519: the point; ecdsa: the curve name and the point), then
 * the application. Returns a key with no handle, or NULL.
 */
static sk_key *sk_read_public(const ssh_keyalg *alg, BinarySource *src)
{
    const struct sk_extra *extra = sk_extra_of(alg);
    strbuf *baseblob = strbuf_new();

    put_stringz(baseblob, extra->base->ssh_id);
    if (extra->ecdsa) {
        ptrlen curve = get_string(src);
        ptrlen point = get_string(src);
        put_stringpl(baseblob, curve);
        put_stringpl(baseblob, point);
    } else {
        ptrlen point = get_string(src);
        put_stringpl(baseblob, point);
    }
    ptrlen application = get_string(src);
    if (get_err(src)) {
        strbuf_free(baseblob);
        return NULL;
    }

    ssh_key *base = ssh_key_new_pub(extra->base,
                                    ptrlen_from_strbuf(baseblob));
    strbuf_free(baseblob);
    if (!base)
        return NULL;

    sk_key *sk = snew(sk_key);
    sk->sshk.vt = alg;
    sk->base = base;
    sk->application = strbuf_dup(application);
    sk->flags = 0;
    sk->key_handle = NULL;
    sk->reserved = NULL;
    return sk;
}

static void sk_freekey(ssh_key *key)
{
    sk_key *sk = container_of(key, sk_key, sshk);
    if (sk->base)
        ssh_key_free(sk->base);
    strbuf_free(sk->application);
    if (sk->key_handle)
        strbuf_free(sk->key_handle);
    if (sk->reserved)
        strbuf_free(sk->reserved);
    sfree(sk);
}

static ssh_key *sk_new_pub(const ssh_keyalg *alg, ptrlen data)
{
    BinarySource src[1];
    BinarySource_BARE_INIT_PL(src, data);
    if (!ptrlen_eq_string(get_string(src), alg->ssh_id))
        return NULL;
    sk_key *sk = sk_read_public(alg, src);
    if (!sk)
        return NULL;
    if (get_avail(src)) {
        sk_freekey(&sk->sshk);
        return NULL;
    }
    return &sk->sshk;
}

/* The PPK form (KiTTY's, PuTTY has none): the public blob as for every
 * key, the private blob holds what OpenSSH's private form adds after the
 * application - flags, key handle, reserved. No secret in either. */
static ssh_key *sk_new_priv(const ssh_keyalg *alg, ptrlen pub, ptrlen priv)
{
    BinarySource src[1];
    BinarySource_BARE_INIT_PL(src, pub);
    if (!ptrlen_eq_string(get_string(src), alg->ssh_id))
        return NULL;
    sk_key *sk = sk_read_public(alg, src);
    if (!sk)
        return NULL;
    if (get_avail(src)) {
        sk_freekey(&sk->sshk);
        return NULL;
    }
    BinarySource_BARE_INIT_PL(src, priv);
    unsigned char flags = get_byte(src);
    ptrlen handle = get_string(src);
    ptrlen reserved = get_string(src);
    /* trailing bytes allowed: an encrypted PPK pads this to the cipher's
     * block size */
    if (get_err(src) || handle.len == 0) {
        sk_freekey(&sk->sshk);
        return NULL;
    }
    sk->flags = flags;
    sk->key_handle = strbuf_dup(handle);
    sk->reserved = strbuf_dup(reserved);
    return &sk->sshk;
}

/* OpenSSH's private form, after the type string: the public fields, the
 * application, flags, key handle, reserved (PROTOCOL.u2f). This is also
 * the body of an agent ADD_IDENTITY. */
static ssh_key *sk_new_priv_openssh(const ssh_keyalg *alg, BinarySource *src)
{
    sk_key *sk = sk_read_public(alg, src);
    if (!sk)
        return NULL;
    unsigned char flags = get_byte(src);
    ptrlen handle = get_string(src);
    ptrlen reserved = get_string(src);
    if (get_err(src) || handle.len == 0) {
        sk_freekey(&sk->sshk);
        return NULL;
    }
    sk->flags = flags;
    sk->key_handle = strbuf_dup(handle);
    sk->reserved = strbuf_dup(reserved);
    return &sk->sshk;
}

static char *sk_invalid(ssh_key *key, unsigned flags)
{
    (void)key; (void)flags;
    return NULL;
}

/* The base key's public blob without its type string: the fields both
 * forms share. */
static void sk_put_base_fields(sk_key *sk, BinarySink *bs)
{
    strbuf *blob = strbuf_new();
    ssh_key_public_blob(sk->base, BinarySink_UPCAST(blob));
    BinarySource src[1];
    BinarySource_BARE_INIT_PL(src, ptrlen_from_strbuf(blob));
    get_string(src);
    put_datapl(bs, get_data(src, get_avail(src)));
    strbuf_free(blob);
}

static void sk_public_blob(ssh_key *key, BinarySink *bs)
{
    sk_key *sk = container_of(key, sk_key, sshk);
    put_stringz(bs, sk->sshk.vt->ssh_id);
    sk_put_base_fields(sk, bs);
    put_stringpl(bs, ptrlen_from_strbuf(sk->application));
}

static void sk_private_blob(ssh_key *key, BinarySink *bs)
{
    sk_key *sk = container_of(key, sk_key, sshk);
    put_byte(bs, sk->flags);
    put_stringpl(bs, sk->key_handle ? ptrlen_from_strbuf(sk->key_handle) :
                 PTRLEN_LITERAL(""));
    put_stringpl(bs, sk->reserved ? ptrlen_from_strbuf(sk->reserved) :
                 PTRLEN_LITERAL(""));
}

static void sk_openssh_blob(ssh_key *key, BinarySink *bs)
{
    sk_key *sk = container_of(key, sk_key, sshk);
    sk_put_base_fields(sk, bs);
    put_stringpl(bs, ptrlen_from_strbuf(sk->application));
    put_byte(bs, sk->flags);
    put_stringpl(bs, sk->key_handle ? ptrlen_from_strbuf(sk->key_handle) :
                 PTRLEN_LITERAL(""));
    put_stringpl(bs, sk->reserved ? ptrlen_from_strbuf(sk->reserved) :
                 PTRLEN_LITERAL(""));
}

static bool sk_has_private(ssh_key *key)
{
    sk_key *sk = container_of(key, sk_key, sshk);
    return sk->key_handle != NULL;
}

static char *sk_cache_str(ssh_key *key)
{
    sk_key *sk = container_of(key, sk_key, sshk);
    return ssh_key_cache_str(sk->base);
}

static key_components *sk_components(ssh_key *key)
{
    sk_key *sk = container_of(key, sk_key, sshk);
    key_components *kc = ssh_key_components(sk->base);
    key_components_add_text_pl(kc, "application",
                               ptrlen_from_strbuf(sk->application));
    key_components_add_uint(kc, "flags", sk->flags);
    return kc;
}

/* What the device signed: SHA256(application) || flags || counter ||
 * SHA256(data). */
static void sk_signed_data(BinarySink *bs, ptrlen application,
                           unsigned char flags, uint32_t counter, ptrlen data)
{
    unsigned char h[32];
    hash_simple(&ssh_sha256, application, h);
    put_data(bs, h, 32);
    put_byte(bs, flags);
    put_uint32(bs, counter);
    hash_simple(&ssh_sha256, data, h);
    put_data(bs, h, 32);
    smemclr(h, sizeof(h));
}

static bool sk_verify(ssh_key *key, ptrlen sig, ptrlen data)
{
    sk_key *sk = container_of(key, sk_key, sshk);
    const struct sk_extra *extra = sk_extra_of(sk->sshk.vt);

    BinarySource src[1];
    BinarySource_BARE_INIT_PL(src, sig);
    if (!ptrlen_eq_string(get_string(src), sk->sshk.vt->ssh_id))
        return false;
    ptrlen inner = get_string(src);
    unsigned char flags = get_byte(src);
    uint32_t counter = get_uint32(src);
    if (get_err(src) || get_avail(src))
        return false;
    if (!(flags & SSH_SK_USER_PRESENCE_REQD) &&
        (sk->flags & SSH_SK_USER_PRESENCE_REQD))
        return false;          /* no touch, though the key demands one */
    if (!(flags & SSH_SK_USER_VERIFICATION_REQD) &&
        (sk->flags & SSH_SK_USER_VERIFICATION_REQD))
        return false;          /* no PIN, though the key demands one */

    strbuf *msg = strbuf_new();
    sk_signed_data(BinarySink_UPCAST(msg), ptrlen_from_strbuf(sk->application),
                   flags, counter, data);
    strbuf *basesig = strbuf_new();
    put_stringz(basesig, extra->base->ssh_id);
    put_stringpl(basesig, inner);
    bool ok = ssh_key_verify(sk->base, ptrlen_from_strbuf(basesig),
                             ptrlen_from_strbuf(msg));
    strbuf_free(basesig);
    strbuf_free(msg);
    return ok;
}

/* One DER INTEGER, minimal and positive: its content bytes are already an
 * SSH mpint's (RFC 4251 5). */
static bool sk_der_integer(BinarySource *src, ptrlen *out)
{
    unsigned char tag = get_byte(src);
    unsigned char len = get_byte(src);
    if (get_err(src) || tag != 0x02 || len == 0 || len > 33)
        return false;
    *out = get_data(src, len);
    if (get_err(src) || (((const unsigned char *)out->ptr)[0] & 0x80))
        return false;          /* short, or negative */
    return true;
}

static bool sk_der_to_mpints(ptrlen der, BinarySink *bs)
{
    BinarySource src[1];
    BinarySource_BARE_INIT_PL(src, der);
    unsigned char tag = get_byte(src);
    unsigned char len = get_byte(src);
    ptrlen r, s;
    if (get_err(src) || tag != 0x30 || len != get_avail(src))
        return false;
    if (!sk_der_integer(src, &r) || !sk_der_integer(src, &s) ||
        get_avail(src))
        return false;
    put_stringpl(bs, r);
    put_stringpl(bs, s);
    return true;
}

/*
 * Build the SSH signature from what the device returned. Shared with the
 * test so the parsing of authenticatorData and the DER signature is the
 * one in use. NULL on success, else the reason.
 */
const char *sk_signature_from_assertion(const ssh_keyalg *alg,
                                        ptrlen application,
                                        ptrlen auth_data, ptrlen dev_sig,
                                        BinarySink *bs)
{
    const struct sk_extra *extra = sk_extra_of(alg);
    unsigned char h[32];

    /* 32 bytes RP ID hash, flags, counter - and nothing more: extensions
     * would sit between counter and the data hash, outside this format */
    if (auth_data.len != 37)
        return "unexpected response from the security key";
    const unsigned char *ad = auth_data.ptr;
    hash_simple(&ssh_sha256, application, h);
    if (memcmp(ad, h, 32))
        return "security key response for another application";
    unsigned char flags = ad[32];
    if (flags & 0x80)
        return "unexpected response from the security key";

    strbuf *inner = strbuf_new();
    if (extra->ecdsa) {
        if (!sk_der_to_mpints(dev_sig, BinarySink_UPCAST(inner))) {
            strbuf_free(inner);
            return "malformed signature from the security key";
        }
    } else {
        if (dev_sig.len != 64) {
            strbuf_free(inner);
            return "malformed signature from the security key";
        }
        put_datapl(inner, dev_sig);
    }

    put_stringz(bs, alg->ssh_id);
    put_stringsb(bs, inner);
    put_byte(bs, flags);
    put_data(bs, ad + 33, 4);   /* the counter, big-endian as stored */
    return NULL;
}

/*
 * A registration (WebAuthn MakeCredential). Its authenticatorData is the
 * RP ID hash, flags, counter, then the attested credential data: AAGUID,
 * credential id length and id, then the public key as a COSE key - a CBOR
 * map. Only the CBOR a COSE key uses is read: integers, byte strings, one
 * map.
 */
static bool sk_cbor_head(BinarySource *src, unsigned *major, uint64_t *val)
{
    unsigned char b = get_byte(src);
    unsigned ai = b & 31;
    *major = b >> 5;
    if (ai < 24)
        *val = ai;
    else if (ai == 24)
        *val = get_byte(src);
    else if (ai == 25)
        *val = get_uint16(src);
    else if (ai == 26)
        *val = get_uint32(src);
    else if (ai == 27)
        *val = get_uint64(src);
    else
        return false;
    return !get_err(src);
}

/* A CBOR integer (major type 0 or 1) as a signed value. */
static bool sk_cbor_int(unsigned major, uint64_t val, int64_t *out)
{
    if (val > 0x7FFFFFFF)
        return false;
    if (major == 0)
        *out = (int64_t)val;
    else if (major == 1)
        *out = -1 - (int64_t)val;
    else
        return false;
    return true;
}

#define SK_COSE_KTY  1
#define SK_COSE_ALG  3
#define SK_COSE_CRV  (-1)
#define SK_COSE_X    (-2)
#define SK_COSE_Y    (-3)

ssh_key *sk_key_from_registration(const ssh_keyalg *alg, ptrlen application,
                                  unsigned char flags, ptrlen auth_data,
                                  ptrlen cred_id, const char **err)
{
    const struct sk_extra *extra = sk_extra_of(alg);
    BinarySource src[1];
    unsigned char h[32];
    int64_t kty = 0, calg = 0, crv = 0;
    ptrlen x = PTRLEN_LITERAL(""), y = PTRLEN_LITERAL("");
    unsigned major;
    uint64_t n, i;

    *err = "unexpected response from the security key";
    BinarySource_BARE_INIT_PL(src, auth_data);
    ptrlen rphash = get_data(src, 32);
    unsigned char adflags = get_byte(src);
    get_uint32(src);                         /* counter */
    get_data(src, 16);                       /* AAGUID */
    ptrlen id = get_data(src, get_uint16(src));
    if (get_err(src) || !(adflags & 0x40))   /* AT: credential data present */
        return NULL;
    hash_simple(&ssh_sha256, application, h);
    if (memcmp(rphash.ptr, h, 32)) {
        *err = "security key response for another application";
        return NULL;
    }
    if (cred_id.len == 0)
        cred_id = id;                        /* the id is in both places */
    if (cred_id.len == 0)
        return NULL;

    if (!sk_cbor_head(src, &major, &n) || major != 5 || n > 16)
        return NULL;
    for (i = 0; i < n; i++) {
        int64_t key, ival;
        uint64_t v;
        if (!sk_cbor_head(src, &major, &v) || !sk_cbor_int(major, v, &key))
            return NULL;
        if (!sk_cbor_head(src, &major, &v))
            return NULL;
        if (major == 2) {
            ptrlen bytes = get_data(src, (size_t)v);
            if (get_err(src))
                return NULL;
            if (key == SK_COSE_X)
                x = bytes;
            else if (key == SK_COSE_Y)
                y = bytes;
        } else if (sk_cbor_int(major, v, &ival)) {
            if (key == SK_COSE_KTY)
                kty = ival;
            else if (key == SK_COSE_ALG)
                calg = ival;
            else if (key == SK_COSE_CRV)
                crv = ival;
        } else {
            return NULL;
        }
    }

    /* after the type string: OpenSSH's private form (sk_new_priv_openssh) */
    strbuf *priv = strbuf_new();
    if (extra->ecdsa) {
        /* EC2, ES256, P-256 */
        if (kty != 2 || calg != -7 || crv != 1 || x.len != 32 ||
            y.len != 32) {
            strbuf_free(priv);
            return NULL;
        }
        strbuf *point = strbuf_new();
        put_byte(point, 0x04);
        put_datapl(point, x);
        put_datapl(point, y);
        put_stringz(priv, "nistp256");
        put_stringsb(priv, point);
    } else {
        /* OKP, EdDSA, Ed25519 */
        if (kty != 1 || calg != -8 || crv != 6 || x.len != 32) {
            strbuf_free(priv);
            return NULL;
        }
        put_stringpl(priv, x);
    }
    put_stringpl(priv, application);
    put_byte(priv, flags);
    put_stringpl(priv, cred_id);
    put_stringz(priv, "");                   /* reserved */

    BinarySource_BARE_INIT_PL(src, ptrlen_from_strbuf(priv));
    ssh_key *key = ssh_key_new_priv_openssh(alg, src);
    strbuf_free(priv);
    if (key)
        *err = NULL;
    return key;
}

static void sk_sign(ssh_key *key, ptrlen data, unsigned flags, BinarySink *bs)
{
    sk_key *sk = container_of(key, sk_key, sshk);
    const struct sk_extra *extra = sk_extra_of(sk->sshk.vt);
    (void)flags;

    if (!sk->key_handle) {
        sk_set_error("key file without key handle");
        return;
    }
    if (!sk_assert_provider) {
        sk_set_error("security keys not supported by this program");
        return;
    }

    SkAssertion as;
    as.auth_data = strbuf_new();
    as.signature = strbuf_new();
    const char *err = sk_assert_provider(
        ptrlen_from_strbuf(sk->application),
        ptrlen_from_strbuf(sk->key_handle), sk->flags, extra->ecdsa,
        data, &as);
    if (!err)
        err = sk_signature_from_assertion(
            sk->sshk.vt, ptrlen_from_strbuf(sk->application),
            ptrlen_from_strbuf(as.auth_data),
            ptrlen_from_strbuf(as.signature), bs);
    sk_set_error(err);
    strbuf_free(as.auth_data);
    strbuf_free(as.signature);
}

static int sk_pubkey_bits(const ssh_keyalg *alg, ptrlen blob)
{
    (void)blob;
    return sk_extra_of(alg)->ecdsa ? 256 : 255;
}

static char *sk_alg_desc(const ssh_keyalg *alg)
{
    return dupstr(sk_extra_of(alg)->desc);
}

static const struct sk_extra sk_extra_ed25519 = {
    &ssh_ecdsa_ed25519, false, "Ed25519 security key",
};
const ssh_keyalg ssh_sk_ed25519 = {
    .new_pub = sk_new_pub,
    .new_priv = sk_new_priv,
    .new_priv_openssh = sk_new_priv_openssh,
    .freekey = sk_freekey,
    .invalid = sk_invalid,
    .sign = sk_sign,
    .verify = sk_verify,
    .public_blob = sk_public_blob,
    .private_blob = sk_private_blob,
    .openssh_blob = sk_openssh_blob,
    .has_private = sk_has_private,
    .cache_str = sk_cache_str,
    .components = sk_components,
    .base_key = nullkey_base_key,
    .pubkey_bits = sk_pubkey_bits,
    .supported_flags = nullkey_supported_flags,
    .alternate_ssh_id = nullkey_alternate_ssh_id,
    .alg_desc = sk_alg_desc,
    .variable_size = nullkey_variable_size_no,
    .ssh_id = "sk-ssh-ed25519@openssh.com",
    .cache_id = "sk-ssh-ed25519@openssh.com",
    .extra = &sk_extra_ed25519,
};

static const struct sk_extra sk_extra_nistp256 = {
    &ssh_ecdsa_nistp256, true, "NIST p256 security key",
};
const ssh_keyalg ssh_sk_ecdsa_nistp256 = {
    .new_pub = sk_new_pub,
    .new_priv = sk_new_priv,
    .new_priv_openssh = sk_new_priv_openssh,
    .freekey = sk_freekey,
    .invalid = sk_invalid,
    .sign = sk_sign,
    .verify = sk_verify,
    .public_blob = sk_public_blob,
    .private_blob = sk_private_blob,
    .openssh_blob = sk_openssh_blob,
    .has_private = sk_has_private,
    .cache_str = sk_cache_str,
    .components = sk_components,
    .base_key = nullkey_base_key,
    .pubkey_bits = sk_pubkey_bits,
    .supported_flags = nullkey_supported_flags,
    .alternate_ssh_id = nullkey_alternate_ssh_id,
    .alg_desc = sk_alg_desc,
    .variable_size = nullkey_variable_size_no,
    .ssh_id = "sk-ecdsa-sha2-nistp256@openssh.com",
    .cache_id = "sk-ecdsa-sha2-nistp256@openssh.com",
    .extra = &sk_extra_nistp256,
};
