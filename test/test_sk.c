/*
 * test_sk.c - FIDO security keys in SSH (crypto/sk-keys.c): OpenSSH's own
 * test vectors (regress/unittests/sshkey and sshsig, openssh-portable)
 * parse, keep their fingerprints and verify; private key files load; a
 * signature is built from what a device returns, through the provider,
 * and every refusal path gives its reason. No device is needed.
 */
#include <winsock2.h>   /* before windows.h: putty.h pulls it in further down */
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "putty.h"
#include "ssh.h"
#include "mpint.h"

const char *appname = "test_sk";

void modalfatalbox(const char *p, ...)
{
    va_list ap;
    fprintf(stderr, "FATAL ERROR: ");
    va_start(ap, p);
    vfprintf(stderr, p, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

/* Linked for the key code; nothing here makes a key or a random number:
 * Ed25519 and ECDSA (RFC 6979) sign deterministically. */
void old_keyfile_warning(void) {}
void random_read(void *buf, size_t size)
{
    (void)buf;
    (void)size;
    modalfatalbox("random_read called in test_sk");
}

static int fails = 0;

static void expect(const char *what, bool got, bool want)
{
    if (got != want) {
        printf("FAIL %s\n", what);
        fails++;
    } else {
        printf("ok   %s\n", what);
    }
}

static void expect_str(const char *what, const char *got, const char *want)
{
    if (!got || strcmp(got, want)) {
        printf("FAIL %s: got \"%s\", want \"%s\"\n", what,
               got ? got : "(null)", want);
        fails++;
    } else {
        printf("ok   %s\n", what);
    }
}

static strbuf *b64(const char *s)
{
    return base64_decode_sb(ptrlen_from_asciz(s));
}

/* ---- OpenSSH's vectors (regress/unittests/sshkey/testdata) ---- */

static const char ED_PUB[] =
    "AAAAGnNrLXNzaC1lZDI1NTE5QG9wZW5zc2guY29tAAAAICFo/k5LU8863u66YC9eUO2170Q"
    "duohPURkQnbLa/dczAAAABHNzaDo=";
static const char ED_FP[] = "SHA256:6WZVJ44bqhAWLVP4Ns0TDkoSQSsZo/h2K+mEvOaNFbw";
static const char EC_PUB[] =
    "AAAAInNrLWVjZHNhLXNoYTItbmlzdHAyNTZAb3BlbnNzaC5jb20AAAAIbmlzdHAyNTYAAAB"
    "BBGdVPkKOLUMZ7b6rZfS8BOHotic4NX4MmuyEmiuvkaeKlbb1F+5oNezRYHpcF6P104trNV"
    "EONCu1XX1CA7px+QUAAAAEc3NoOg==";
static const char EC_FP[] = "SHA256:Go7HO0CVPYG+BSDSk9ZUJBKGSrtBExp6obTa9iqzIUo";

static const char ED_FILE[] =
    "-----BEGIN OPENSSH PRIVATE KEY-----\n"
    "b3BlbnNzaC1rZXktdjEAAAAABG5vbmUAAAAEbm9uZQAAAAAAAAABAAAASgAAABpzay1zc2\n"
    "gtZWQyNTUxOUBvcGVuc3NoLmNvbQAAACAhaP5OS1PPOt7uumAvXlDtte9EHbqIT1EZEJ2y\n"
    "2v3XMwAAAARzc2g6AAAAuBocY6UaHGOlAAAAGnNrLXNzaC1lZDI1NTE5QG9wZW5zc2guY2\n"
    "9tAAAAICFo/k5LU8863u66YC9eUO2170QduohPURkQnbLa/dczAAAABHNzaDoBAAAAQJYq\n"
    "lGHhFoA25/q8X/rdTqDAb7dhqs4ehhd/w8x99CwiIWj+TktTzzre7rpgL15Q7bXvRB26iE\n"
    "9RGRCdstr91zMAAAAAAAAAFkVEMjU1MTktU0sgdGVzdCBrZXkgIzEBAgM=\n"
    "-----END OPENSSH PRIVATE KEY-----\n";
static const char EC_FILE[] =
    "-----BEGIN OPENSSH PRIVATE KEY-----\n"
    "b3BlbnNzaC1rZXktdjEAAAAABG5vbmUAAAAEbm9uZQAAAAAAAAABAAAAfwAAACJzay1lY2\n"
    "RzYS1zaGEyLW5pc3RwMjU2QG9wZW5zc2guY29tAAAACG5pc3RwMjU2AAAAQQRnVT5Cji1D\n"
    "Ge2+q2X0vATh6LYnODV+DJrshJorr5GnipW29RfuaDXs0WB6XBej9dOLazVRDjQrtV19Qg\n"
    "O6cfkFAAAABHNzaDoAAAGQuPdnP7j3Zz8AAAAic2stZWNkc2Etc2hhMi1uaXN0cDI1NkBv\n"
    "cGVuc3NoLmNvbQAAAAhuaXN0cDI1NgAAAEEEZ1U+Qo4tQxntvqtl9LwE4ei2Jzg1fgya7I\n"
    "SaK6+Rp4qVtvUX7mg17NFgelwXo/XTi2s1UQ40K7VdfUIDunH5BQAAAARzc2g6AQAAAOMt\n"
    "LS0tLUJFR0lOIEVDIFBSSVZBVEUgS0VZLS0tLS0KTUhjQ0FRRUVJRURmVFB4YzA0alN5Zk\n"
    "Z5NlhoV1pTVlpzcnU5ZFlaSVpTOWhjeVFhcDlVT29Bb0dDQ3FHU000OQpBd0VIb1VRRFFn\n"
    "QUVaMVUrUW80dFF4bnR2cXRsOUx3RTRlaTJKemcxZmd5YTdJU2FLNitScDRxVnR2VVg3bW\n"
    "cxCjdORmdlbHdYby9YVGkyczFVUTQwSzdWZGZVSUR1bkg1QlE9PQotLS0tLUVORCBFQyBQ\n"
    "UklWQVRFIEtFWS0tLS0tCgAAAAAAAAAURUNEU0EtU0sgdGVzdCBrZXkgIzEBAgMEBQ==\n"
    "-----END OPENSSH PRIVATE KEY-----\n";

/* ---- regress/unittests/sshsig/testdata: signatures over signed-data in
 * namespace "unittest", made by real security-key code ---- */

static const char SIGNED_DATA[] = "This is a test, this is only a test";
static const char ED_SSHSIG[] =
    "U1NIU0lHAAAAAQAAAEoAAAAac2stc3NoLWVkMjU1MTlAb3BlbnNzaC5jb20AAAAgmxoNhd"
    "Biu5zpuKUJgMgrhhI54b7OatE7Yz23Tf//sscAAAAEc3NoOgAAAAh1bml0dGVzdAAAAAAA"
    "AAAGc2hhNTEyAAAAZwAAABpzay1zc2gtZWQyNTUxOUBvcGVuc3NoLmNvbQAAAEAi+7eTjW"
    "/+LQ2M+sCD+KFtH1n7VFFJon/SZFsxODyV8cWTlFKj617Ys1Ur5TV6uaEXQhck8rBA2oQI"
    "HTPANLIPARI0Vng=";
static const char EC_SSHSIG[] =
    "U1NIU0lHAAAAAQAAAH8AAAAic2stZWNkc2Etc2hhMi1uaXN0cDI1NkBvcGVuc3NoLmNvbQ"
    "AAAAhuaXN0cDI1NgAAAEEEoNVrmNFxPlXsTrKybBWLsclaHunkP6F5ss9qRBymcPZqMlno"
    "b/hbO6GVhadyVZjrcmykOnb31GWGrkBPzr+DwAAAAARzc2g6AAAACHVuaXR0ZXN0AAAAAA"
    "AAAAZzaGE1MTIAAAB3AAAAInNrLWVjZHNhLXNoYTItbmlzdHAyNTZAb3BlbnNzaC5jb20A"
    "AABIAAAAIHohGwyy8iKT3zwd1TYA9V/Ioo7h/3zCJUtyq/Qigt/HAAAAIGzidTwq7D/kFa"
    "7Xjcp/KkdbIs4MfQpfAW/0OciajlpzARI0Vng=";

static void test_public(const char *name, const char *pub,
                        const ssh_keyalg *alg, const char *fp)
{
    char what[128];
    strbuf *blob = b64(pub);
    ssh_key *k = ssh_key_new_pub(alg, ptrlen_from_strbuf(blob));

    snprintf(what, sizeof(what), "%s: the public key parses", name);
    expect(what, k != NULL, true);
    snprintf(what, sizeof(what), "%s: its type is found by name", name);
    expect(what, pubkey_blob_to_alg(ptrlen_from_strbuf(blob)) == alg, true);
    if (k) {
        strbuf *again = strbuf_new();
        ssh_key_public_blob(k, BinarySink_UPCAST(again));
        snprintf(what, sizeof(what), "%s: the public blob comes back as read",
                 name);
        expect(what, ptrlen_eq_ptrlen(ptrlen_from_strbuf(again),
                                      ptrlen_from_strbuf(blob)), true);
        strbuf_free(again);
        snprintf(what, sizeof(what), "%s: a public key cannot sign", name);
        expect(what, ssh_key_has_private(k), false);
        ssh_key_free(k);
    }
    char *f = ssh2_fingerprint_blob(ptrlen_from_strbuf(blob),
                                    SSH_FPTYPE_SHA256);
    const char *tail = strrchr(f, ' ');
    snprintf(what, sizeof(what), "%s: OpenSSH's fingerprint", name);
    expect_str(what, tail ? tail + 1 : f, fp);
    sfree(f);

    /* the same blob with one byte too many is not a key */
    put_byte(blob, 0);
    k = ssh_key_new_pub(alg, ptrlen_from_strbuf(blob));
    snprintf(what, sizeof(what), "%s: trailing bytes are refused", name);
    expect(what, k == NULL, true);
    if (k)
        ssh_key_free(k);
    strbuf_free(blob);
}

static void test_file(const char *name, const char *file, const char *pub,
                      const ssh_keyalg *alg, const char *comment,
                      unsigned char flags)
{
    char what[128];
    const char *err = NULL;
    BinarySource src[1];
    BinarySource_BARE_INIT(src, file, strlen(file));
    ssh2_userkey *uk = import_ssh2_s(src, SSH_KEYTYPE_OPENSSH_NEW, "", &err);

    snprintf(what, sizeof(what), "%s: the private key file loads", name);
    expect(what, uk && uk != SSH2_WRONG_PASSPHRASE && uk->key, true);
    if (!uk || uk == SSH2_WRONG_PASSPHRASE || !uk->key) {
        printf("     (%s)\n", err ? err : "no reason");
        return;
    }
    snprintf(what, sizeof(what), "%s: as a security key", name);
    expect(what, ssh_key_alg(uk->key) == alg && ssh_keyalg_is_sk(alg), true);
    snprintf(what, sizeof(what), "%s: with its comment", name);
    expect_str(what, uk->comment, comment);
    snprintf(what, sizeof(what), "%s: holding a key handle", name);
    expect(what, ssh_key_has_private(uk->key), true);

    strbuf *pb = strbuf_new(), *want = b64(pub);
    ssh_key_public_blob(uk->key, BinarySink_UPCAST(pb));
    snprintf(what, sizeof(what), "%s: its public half is the .pub file's",
             name);
    expect(what, ptrlen_eq_ptrlen(ptrlen_from_strbuf(pb),
                                  ptrlen_from_strbuf(want)), true);

    /* the agent form (ADD_IDENTITY body, kageant's protected copy): out
     * and in again, the same key with the same flags */
    strbuf *ob = strbuf_new();
    ssh_key_openssh_blob(uk->key, BinarySink_UPCAST(ob));
    BinarySource osrc[1];
    BinarySource_BARE_INIT_PL(osrc, ptrlen_from_strbuf(ob));
    ssh_key *k2 = ssh_key_new_priv_openssh(alg, osrc);
    snprintf(what, sizeof(what), "%s: the agent form reads back", name);
    expect(what, k2 && !get_avail(osrc), true);
    if (k2) {
        strbuf *ob2 = strbuf_new();
        ssh_key_openssh_blob(k2, BinarySink_UPCAST(ob2));
        snprintf(what, sizeof(what), "%s: ... unchanged", name);
        expect(what, ptrlen_eq_ptrlen(ptrlen_from_strbuf(ob),
                                      ptrlen_from_strbuf(ob2)), true);
        key_components *kc = ssh_key_components(k2);
        bool found = false;
        for (size_t i = 0; i < kc->ncomponents; i++)
            if (!strcmp(kc->components[i].name, "flags") &&
                kc->components[i].type == KCT_MPINT &&
                mp_get_integer(kc->components[i].mp) == flags)
                found = true;
        snprintf(what, sizeof(what), "%s: flags 0x%02x kept", name, flags);
        expect(what, found, true);
        key_components_free(kc);
        strbuf_free(ob2);
        ssh_key_free(k2);
    }
    strbuf_free(ob);
    strbuf_free(pb);
    strbuf_free(want);
    ssh_key_free(uk->key);
    sfree(uk->comment);
    sfree(uk);
}

/* An SSHSIG (PROTOCOL.sshsig) carries the key and its signature over
 * "SSHSIG" || namespace || reserved || hash algorithm || H(message). */
static void test_sshsig(const char *name, const char *armour,
                        const ssh_keyalg *alg)
{
    char what[128];
    strbuf *sig = b64(armour);
    BinarySource src[1];
    BinarySource_BARE_INIT_PL(src, ptrlen_from_strbuf(sig));
    ptrlen magic = get_data(src, 6);
    get_uint32(src);
    ptrlen pub = get_string(src), ns = get_string(src);
    ptrlen reserved = get_string(src), hashalg = get_string(src);
    ptrlen sigblob = get_string(src);
    snprintf(what, sizeof(what), "%s: the SSHSIG fixture reads", name);
    expect(what, !get_err(src) && ptrlen_eq_string(magic, "SSHSIG") &&
           ptrlen_eq_string(hashalg, "sha512"), true);

    ssh_key *k = ssh_key_new_pub(alg, pub);
    snprintf(what, sizeof(what), "%s: its key parses", name);
    expect(what, k != NULL, true);
    if (!k) {
        strbuf_free(sig);
        return;
    }

    unsigned char h[64];
    hash_simple(&ssh_sha512, ptrlen_from_asciz(SIGNED_DATA), h);
    strbuf *signed_data = strbuf_new();
    put_data(signed_data, "SSHSIG", 6);
    put_stringpl(signed_data, ns);
    put_stringpl(signed_data, reserved);
    put_stringpl(signed_data, hashalg);
    put_string(signed_data, h, 64);
    snprintf(what, sizeof(what), "%s: OpenSSH's signature verifies", name);
    expect(what, ssh_key_verify(k, sigblob, ptrlen_from_strbuf(signed_data)),
           true);

    /* one bit of the data, the counter, or the flags changed: refused */
    strbuf *bad = strbuf_dup(ptrlen_from_strbuf(signed_data));
    bad->u[bad->len - 1] ^= 1;
    snprintf(what, sizeof(what), "%s: changed data is refused", name);
    expect(what, ssh_key_verify(k, sigblob, ptrlen_from_strbuf(bad)), false);
    strbuf_free(bad);
    strbuf *badsig = strbuf_dup(sigblob);
    badsig->u[badsig->len - 1] ^= 1;             /* the counter's last byte */
    snprintf(what, sizeof(what), "%s: a changed counter is refused", name);
    expect(what, ssh_key_verify(k, ptrlen_from_strbuf(badsig),
                                ptrlen_from_strbuf(signed_data)), false);
    strbuf_free(badsig);

    strbuf_free(signed_data);
    ssh_key_free(k);
    strbuf_free(sig);
}

/* ---- a device stand-in: the plain key of the same curve signs what the
 * device would, and the answer comes back as webauthn.dll gives it ---- */

static ssh_key *dev_key;            /* the device's secret */
static unsigned char dev_flags;     /* what the device reports */
static size_t dev_authlen = 37;     /* 38: an extension byte appended */
static const char *dev_app = NULL;  /* answers for another application */
static const char *dev_refuse = NULL;

static void put_der_int(strbuf *out, ptrlen v)
{
    put_byte(out, 0x02);
    put_byte(out, (unsigned char)v.len);
    put_datapl(out, v);
}

static const char *fake_device(ptrlen application, ptrlen key_handle,
                               unsigned key_flags, bool ecdsa, ptrlen data,
                               SkAssertion *out)
{
    unsigned char h[32];
    (void)key_handle;
    (void)key_flags;
    if (dev_refuse)
        return dev_refuse;

    strbuf *ad = strbuf_new();
    hash_simple(&ssh_sha256, dev_app ? ptrlen_from_asciz(dev_app)
                                     : application, h);
    put_data(ad, h, 32);
    put_byte(ad, dev_flags);
    put_uint32(ad, 7);                       /* the counter */
    while (ad->len < dev_authlen)
        put_byte(ad, 0xA0);

    strbuf *msg = strbuf_dup(ptrlen_from_strbuf(ad));
    hash_simple(&ssh_sha256, data, h);
    put_data(msg, h, 32);
    strbuf *bsig = strbuf_new();
    ssh_key_sign(dev_key, ptrlen_from_strbuf(msg), 0,
                 BinarySink_UPCAST(bsig));
    BinarySource src[1];
    BinarySource_BARE_INIT_PL(src, ptrlen_from_strbuf(bsig));
    get_string(src);                         /* the plain type */
    ptrlen inner = get_string(src);
    if (ecdsa) {
        BinarySource isrc[1];
        BinarySource_BARE_INIT_PL(isrc, inner);
        ptrlen r = get_string(isrc), s = get_string(isrc);
        strbuf *seq = strbuf_new();
        put_der_int(seq, r);
        put_der_int(seq, s);
        put_byte(out->signature, 0x30);
        put_byte(out->signature, (unsigned char)seq->len);
        put_datapl(out->signature, ptrlen_from_strbuf(seq));
        strbuf_free(seq);
    } else {
        put_datapl(out->signature, inner);
    }
    put_datapl(out->auth_data, ptrlen_from_strbuf(ad));
    strbuf_free(bsig);
    strbuf_free(msg);
    strbuf_free(ad);
    return NULL;
}

/* RFC 8032 7.1 test 1 */
static const unsigned char ED_SECRET[32] = {
    0x9d,0x61,0xb1,0x9d,0xef,0xfd,0x5a,0x60,0xba,0x84,0x4a,0xf4,0x92,0xec,
    0x2c,0xc4,0x44,0x49,0xc5,0x69,0x7b,0x32,0x69,0x19,0x70,0x3b,0xac,0x03,
    0x1c,0xae,0x7f,0x60 };
static const unsigned char ED_PUBLIC[32] = {
    0xd7,0x5a,0x98,0x01,0x82,0xb1,0x0a,0xb7,0xd5,0x4b,0xfe,0xd3,0xc9,0x64,
    0x07,0x3a,0x0e,0xe1,0x72,0xf3,0xda,0xa6,0x23,0x25,0xaf,0x02,0x1a,0x68,
    0xf7,0x07,0x51,0x1a };
/* RFC 6979 A.2.5 (P-256) */
static const char P256_X[] =
    "C9AFA9D845BA75166B5C215767B1D6934E50C3DB36E89B127B8A622B120F6721";
static const char P256_UX[] =
    "60FED4BA255A9D31C961EB74C6356D68C049B8923B61FA6CE669622E60F29FB6";
static const char P256_UY[] =
    "7903FE1008B8BC99A41AE9E95628BC64F2F1B20C2D7E9F5177A3C294D4462299";

static void put_hex(strbuf *sb, const char *hex)
{
    for (size_t i = 0; hex[i] && hex[i+1]; i += 2) {
        unsigned v;
        sscanf(hex + i, "%2x", &v);
        put_byte(sb, (unsigned char)v);
    }
}

static void test_device(const char *name, bool ecdsa)
{
    char what[160];
    const ssh_keyalg *alg = ecdsa ? &ssh_sk_ecdsa_nistp256 : &ssh_sk_ed25519;
    strbuf *point = strbuf_new(), *priv = strbuf_new();

    if (ecdsa) {
        put_byte(point, 0x04);
        put_hex(point, P256_UX);
        put_hex(point, P256_UY);
        put_stringz(priv, "nistp256");
        put_stringpl(priv, ptrlen_from_strbuf(point));
        strbuf *x = strbuf_new();
        put_byte(x, 0);                       /* positive mpint */
        put_hex(x, P256_X);
        put_stringpl(priv, ptrlen_from_strbuf(x));
        strbuf_free(x);
        BinarySource s[1];
        BinarySource_BARE_INIT_PL(s, ptrlen_from_strbuf(priv));
        dev_key = ssh_key_new_priv_openssh(&ssh_ecdsa_nistp256, s);
    } else {
        put_data(point, ED_PUBLIC, 32);
        put_string(priv, ED_PUBLIC, 32);
        put_uint32(priv, 64);
        put_data(priv, ED_SECRET, 32);
        put_data(priv, ED_PUBLIC, 32);
        BinarySource s[1];
        BinarySource_BARE_INIT_PL(s, ptrlen_from_strbuf(priv));
        dev_key = ssh_key_new_priv_openssh(&ssh_ecdsa_ed25519, s);
    }
    snprintf(what, sizeof(what), "%s: the device stand-in's key", name);
    expect(what, dev_key != NULL, true);
    if (!dev_key)
        return;

    /* the agent's copy: public fields, "ssh:", flags touch + PIN, handle */
    strbuf *body = strbuf_new();
    if (ecdsa)
        put_stringz(body, "nistp256");
    put_stringpl(body, ptrlen_from_strbuf(point));
    put_stringz(body, "ssh:");
    put_byte(body, SSH_SK_USER_PRESENCE_REQD | SSH_SK_USER_VERIFICATION_REQD);
    put_stringz(body, "key-handle");
    put_stringz(body, "");
    BinarySource bs[1];
    BinarySource_BARE_INIT_PL(bs, ptrlen_from_strbuf(body));
    ssh_key *k = ssh_key_new_priv_openssh(alg, bs);
    snprintf(what, sizeof(what), "%s: the agent form of a key loads", name);
    expect(what, k != NULL, true);
    if (!k)
        return;

    ptrlen data = PTRLEN_LITERAL("session id and userauth request");
    strbuf *sig = strbuf_new();

    sk_assert_provider = NULL;
    ssh_key_sign(k, data, 0, BinarySink_UPCAST(sig));
    snprintf(what, sizeof(what), "%s: no provider - no signature", name);
    expect(what, sig->len == 0, true);
    snprintf(what, sizeof(what), "%s: ... and the reason", name);
    expect_str(what, sk_sign_error(), "security keys not supported by this program");

    sk_assert_provider = fake_device;
    dev_flags = SSH_SK_USER_PRESENCE_REQD | SSH_SK_USER_VERIFICATION_REQD;
    ssh_key_sign(k, data, 0, BinarySink_UPCAST(sig));
    snprintf(what, sizeof(what), "%s: the device's answer becomes a "
             "signature", name);
    expect(what, sig->len > 0 && !*sk_sign_error(), true);
    snprintf(what, sizeof(what), "%s: ... that verifies", name);
    expect(what, ssh_key_verify(k, ptrlen_from_strbuf(sig), data), true);
    snprintf(what, sizeof(what), "%s: ... over this data only", name);
    expect(what, ssh_key_verify(k, ptrlen_from_strbuf(sig),
                                PTRLEN_LITERAL("other data")), false);
    {
        BinarySource ss[1];
        BinarySource_BARE_INIT_PL(ss, ptrlen_from_strbuf(sig));
        ptrlen t = get_string(ss);
        get_string(ss);
        unsigned char f = get_byte(ss);
        uint32_t c = get_uint32(ss);
        snprintf(what, sizeof(what), "%s: ... with type, flags and counter",
                 name);
        expect(what, ptrlen_eq_string(t, alg->ssh_id) && f == dev_flags &&
               c == 7 && !get_avail(ss) && !get_err(ss), true);
    }

    /* the device reports no PIN for a key that demands one */
    strbuf_clear(sig);
    dev_flags = SSH_SK_USER_PRESENCE_REQD;
    ssh_key_sign(k, data, 0, BinarySink_UPCAST(sig));
    snprintf(what, sizeof(what), "%s: no PIN reported - the signature does "
             "not verify", name);
    expect(what, sig->len > 0 &&
           !ssh_key_verify(k, ptrlen_from_strbuf(sig), data), true);
    dev_flags = SSH_SK_USER_PRESENCE_REQD | SSH_SK_USER_VERIFICATION_REQD;

    strbuf_clear(sig);
    dev_authlen = 38;
    ssh_key_sign(k, data, 0, BinarySink_UPCAST(sig));
    snprintf(what, sizeof(what), "%s: authenticatorData with extensions "
             "refused", name);
    expect(what, sig->len == 0, true);
    expect_str("     reason", sk_sign_error(),
               "unexpected response from the security key");
    dev_authlen = 37;

    dev_app = "other:";
    ssh_key_sign(k, data, 0, BinarySink_UPCAST(sig));
    snprintf(what, sizeof(what), "%s: an answer for another application "
             "refused", name);
    expect(what, sig->len == 0, true);
    expect_str("     reason", sk_sign_error(),
               "security key response for another application");
    dev_app = NULL;

    dev_refuse = "cancelled or timed out";
    ssh_key_sign(k, data, 0, BinarySink_UPCAST(sig));
    snprintf(what, sizeof(what), "%s: the provider's refusal passes through",
             name);
    expect(what, sig->len == 0, true);
    expect_str("     reason", sk_sign_error(), dev_refuse);
    dev_refuse = NULL;

    sk_assert_provider = NULL;
    strbuf_free(sig);
    strbuf_free(body);
    strbuf_free(point);
    strbuf_free(priv);
    ssh_key_free(k);
    ssh_key_free(dev_key);
    dev_key = NULL;
}

static void test_der(void)
{
    unsigned char ad[37];
    strbuf *out = strbuf_new();
    memset(ad, 0, sizeof(ad));
    hash_simple(&ssh_sha256, PTRLEN_LITERAL("ssh:"), ad);
    ad[32] = 1;
    /* SEQUENCE { INTEGER 0x80 (negative), INTEGER 1 } */
    static const unsigned char neg[] = { 0x30, 6, 2, 1, 0x80, 2, 1, 1 };
    const char *err = sk_signature_from_assertion(
        &ssh_sk_ecdsa_nistp256, PTRLEN_LITERAL("ssh:"),
        make_ptrlen(ad, 37), make_ptrlen(neg, sizeof(neg)),
        BinarySink_UPCAST(out));
    expect_str("p256: a negative DER integer is a malformed signature",
               err, "malformed signature from the security key");
    static const unsigned char shortseq[] = { 0x30, 7, 2, 1, 0x7f, 2, 1, 1 };
    err = sk_signature_from_assertion(
        &ssh_sk_ecdsa_nistp256, PTRLEN_LITERAL("ssh:"),
        make_ptrlen(ad, 37), make_ptrlen(shortseq, sizeof(shortseq)),
        BinarySink_UPCAST(out));
    expect_str("p256: a short DER sequence is a malformed signature",
               err, "malformed signature from the security key");
    static const unsigned char ok[] = { 0x30, 6, 2, 1, 0x7f, 2, 1, 1 };
    err = sk_signature_from_assertion(
        &ssh_sk_ecdsa_nistp256, PTRLEN_LITERAL("ssh:"),
        make_ptrlen(ad, 37), make_ptrlen(ok, sizeof(ok)),
        BinarySink_UPCAST(out));
    expect("p256: a well-formed DER signature is taken", err == NULL, true);
    err = sk_signature_from_assertion(
        &ssh_sk_ed25519, PTRLEN_LITERAL("ssh:"),
        make_ptrlen(ad, 37), make_ptrlen(ok, sizeof(ok)),
        BinarySink_UPCAST(out));
    expect_str("ed25519: a signature of the wrong length is damaged",
               err, "malformed signature from the security key");
    strbuf_free(out);
}

int main(void)
{
    test_public("ed25519-sk", ED_PUB, &ssh_sk_ed25519, ED_FP);
    test_public("ecdsa-sk", EC_PUB, &ssh_sk_ecdsa_nistp256, EC_FP);
    test_file("ed25519-sk", ED_FILE, ED_PUB, &ssh_sk_ed25519,
              "ED25519-SK test key #1", 1);
    test_file("ecdsa-sk", EC_FILE, EC_PUB, &ssh_sk_ecdsa_nistp256,
              "ECDSA-SK test key #1", 1);
    test_sshsig("ed25519-sk", ED_SSHSIG, &ssh_sk_ed25519);
    test_sshsig("ecdsa-sk", EC_SSHSIG, &ssh_sk_ecdsa_nistp256);
    test_device("ed25519-sk", false);
    test_device("ecdsa-sk", true);
    test_der();
    expect("the sk types are not plain-key types",
           !ssh_keyalg_is_sk(&ssh_ecdsa_ed25519) &&
           !ssh_keyalg_is_sk(&ssh_ecdsa_nistp256), true);

    printf("%s: %d failure(s)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
