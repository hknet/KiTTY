/*
 * kitty_hello_container.c - the wrapped-secret container, on its own.
 *
 * Pure crypto over in-tree primitives: no WinRT, no UI, no window
 * handles. That is exactly why it is a file of its own - it is the half
 * of the Hello work that a unit test can exercise completely
 * (test/test_hello_container.c drives it with fixed KEKs) and that
 * kittygen-cli uses headless, while everything around it needs a real
 * desktop and a real gesture.
 *
 * The format itself, the doors it can hold and the reasoning behind the
 * crypto choices are documented at the top of the container section
 * below, which moved here verbatim from kitty_hello.c.
 */

#include <winsock2.h>   /* putty.h insists on preceding windows.h */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "putty.h"
#include "ssh.h"
#include "mpint.h"
#include "misc.h"
#include "kitty_hello.h"

/* ====================================================================
 * The wrapped-secret container. Pure crypto on in-tree primitives - no
 * WinRT, no UI - so this whole layer is exercised by the unit test
 * (test_hello_container.c) with fixed KEKs and by kittygen-cli headless.
 *
 *   HELLOK1:H<b64 hello-blob>.R<mem>,<passes>,<parallel>,<b64 salt>,<b64 blob>
 *
 * Either field may be absent (never both). Unknown fields are ignored so
 * HELLOK1 can grow. Each blob is nonce(12) || AES-256-GCM ciphertext of
 * the 32-byte secret || tag(16), with the format marker as associated
 * data; the recovery key is Argon2id of the passphrase with its cost
 * RECORDED in the container - the MPW lesson: a cost baked only into the
 * binary cannot ever be tuned without locking old data out. Recorded
 * parameters are validated (argon2_params_bad) before EVER reaching the
 * KDF. GCM is safe here where the MPW envelope chose CBC: every wrap is
 * a fresh random nonce under a key that wraps only this one secret.
 */

#define HELLO_MARK          "HELLOK1:"
#define HELLO_AAD           HELLO_MARK      /* binds blobs to the format */
#define HELLO_NONCE_LEN     12
#define HELLO_TAG_LEN       16
#define HELLO_BLOB_LEN      (HELLO_NONCE_LEN + KITTY_HELLO_SECRET_LEN + \
                             HELLO_TAG_LEN)
#define HELLO_SALT_LEN      16
#define HELLO_KEK_LEN       32

/* Recovery-wrap Argon2id cost written by THIS build (RFC 9106's second
 * recommended parameter set). Recorded in the container, so raising it
 * later affects only newly written containers. */
#define HELLO_ARGON_MEM       65536     /* KiB */
#define HELLO_ARGON_PASSES    3
#define HELLO_ARGON_PARALLEL  4

/* System CSPRNG for nonces, salts and generated secrets. PuTTY's own pool
 * is not usable here: pageant.c deliberately stubs random_read() with a
 * fatal error, and this file runs inside kageant. RtlGenRandom is the
 * documented-stable export underneath CryptGenRandom. */
BOOLEAN NTAPI SystemFunction036(PVOID, ULONG);
static bool hello_random(void *buf, size_t len)
{
    return SystemFunction036(buf, (ULONG)len);
}

/* One-shot AES-256-GCM. blob = nonce || ct || tag; call order per
 * test/cryptsuite.py: cipher setkey, setiv(nonce || 4 zero bytes), mac
 * setkey (derives its mask from the cipher), prefix lengths, encrypt,
 * MAC over aad || ciphertext. */
static bool hello_gcm_wrap(const unsigned char kek[HELLO_KEK_LEN],
                           const unsigned char secret[KITTY_HELLO_SECRET_LEN],
                           unsigned char blob[HELLO_BLOB_LEN])
{
    ssh_cipher *c = ssh_cipher_new(&ssh_aes256_gcm);
    ssh2_mac *m;
    unsigned char iv[16];
    unsigned char *ct = blob + HELLO_NONCE_LEN;

    if (!c)
        return false;
    m = ssh2_mac_new(&ssh2_aesgcm_mac, c);
    if (!m) {
        ssh_cipher_free(c);
        return false;
    }
    if (!hello_random(blob, HELLO_NONCE_LEN)) {
        ssh2_mac_free(m);
        ssh_cipher_free(c);
        return false;
    }
    memcpy(iv, blob, HELLO_NONCE_LEN);
    memset(iv + HELLO_NONCE_LEN, 0, 4);
    ssh_cipher_setkey(c, kek);
    ssh_cipher_setiv(c, iv);
    ssh2_mac_setkey(m, PTRLEN_LITERAL(""));
    aesgcm_set_prefix_lengths(m, 0, sizeof(HELLO_AAD) - 1);

    memcpy(ct, secret, KITTY_HELLO_SECRET_LEN);
    ssh_cipher_encrypt(c, ct, KITTY_HELLO_SECRET_LEN);
    ssh2_mac_start(m);
    put_data(m, HELLO_AAD, sizeof(HELLO_AAD) - 1);
    put_data(m, ct, KITTY_HELLO_SECRET_LEN);
    ssh2_mac_genresult(m, ct + KITTY_HELLO_SECRET_LEN);

    ssh2_mac_free(m);
    ssh_cipher_free(c);
    smemclr(iv, sizeof(iv));
    return true;
}

/* Inverse: verify the tag FIRST, then decrypt. false = refused. */
static bool hello_gcm_unwrap(const unsigned char kek[HELLO_KEK_LEN],
                             const unsigned char blob[HELLO_BLOB_LEN],
                             unsigned char secret[KITTY_HELLO_SECRET_LEN])
{
    ssh_cipher *c = ssh_cipher_new(&ssh_aes256_gcm);
    ssh2_mac *m;
    unsigned char iv[16], tag[HELLO_TAG_LEN];
    const unsigned char *ct = blob + HELLO_NONCE_LEN;
    bool ok;

    if (!c)
        return false;
    m = ssh2_mac_new(&ssh2_aesgcm_mac, c);
    if (!m) {
        ssh_cipher_free(c);
        return false;
    }
    memcpy(iv, blob, HELLO_NONCE_LEN);
    memset(iv + HELLO_NONCE_LEN, 0, 4);
    ssh_cipher_setkey(c, kek);
    ssh_cipher_setiv(c, iv);
    ssh2_mac_setkey(m, PTRLEN_LITERAL(""));
    aesgcm_set_prefix_lengths(m, 0, sizeof(HELLO_AAD) - 1);

    ssh2_mac_start(m);
    put_data(m, HELLO_AAD, sizeof(HELLO_AAD) - 1);
    put_data(m, ct, KITTY_HELLO_SECRET_LEN);
    ssh2_mac_genresult(m, tag);
    ok = smemeq(tag, ct + KITTY_HELLO_SECRET_LEN, HELLO_TAG_LEN);
    if (ok) {
        memcpy(secret, ct, KITTY_HELLO_SECRET_LEN);
        ssh_cipher_decrypt(c, secret, KITTY_HELLO_SECRET_LEN);
    }

    ssh2_mac_free(m);
    ssh_cipher_free(c);
    smemclr(iv, sizeof(iv));
    smemclr(tag, sizeof(tag));
    return ok;
}

/* Derive the recovery KEK. Validates the (recorded or compiled-in) cost
 * before running the KDF; false = illegal parameters, refused. */
static bool hello_recovery_kek(const char *passphrase,
                               const unsigned char salt[HELLO_SALT_LEN],
                               uint32_t mem, uint32_t passes,
                               uint32_t parallel,
                               unsigned char kek[HELLO_KEK_LEN])
{
    char *bad = argon2_params_bad(mem, passes, parallel, HELLO_KEK_LEN,
                                  strlen(passphrase), HELLO_SALT_LEN, 0, 0);
    strbuf *sb;
    ptrlen empty = PTRLEN_LITERAL("");

    if (bad) {
        sfree(bad);
        return false;
    }
    sb = strbuf_new_nm();
    argon2(Argon2id, mem, passes, parallel, HELLO_KEK_LEN,
           ptrlen_from_asciz(passphrase),
           make_ptrlen(salt, HELLO_SALT_LEN), empty, empty, sb);
    memcpy(kek, sb->u, HELLO_KEK_LEN);
    strbuf_free(sb);
    return true;
}

static char *hello_w_field(const unsigned char prf_kek[32],
                           const unsigned char *prf_credid,
                           size_t prf_credidlen, const char *owner,
                           const unsigned char secret[KITTY_HELLO_SECRET_LEN]);

char *kitty_hello_container_create(const unsigned char hello_kek[32],
                                   const char *recovery_passphrase,
                                   const unsigned char
                                       secret[KITTY_HELLO_SECRET_LEN])
{
    return kitty_hello_container_create_ex(hello_kek, NULL, NULL, 0,
                                           NULL, recovery_passphrase, secret);
}

char *kitty_hello_container_create_ex(const unsigned char hello_kek[32],
                                      const unsigned char prf_kek[32],
                                      const unsigned char *prf_credid,
                                      size_t prf_credidlen,
                                      const char *prf_owner,
                                      const char *recovery_passphrase,
                                      const unsigned char
                                          secret[KITTY_HELLO_SECRET_LEN])
{
    strbuf *out;
    bool first = true;

    if (!hello_kek && !prf_kek && !recovery_passphrase)
        return NULL;
    if ((prf_kek != NULL) != (prf_credid != NULL && prf_credidlen > 0))
        return NULL;      /* the W wrap needs BOTH the KEK and the id */

    out = strbuf_new_nm();
    put_dataz(out, HELLO_MARK);

    if (hello_kek) {
        unsigned char blob[HELLO_BLOB_LEN];
        strbuf *b64;
        if (!hello_gcm_wrap(hello_kek, secret, blob)) {
            strbuf_free(out);
            return NULL;
        }
        b64 = base64_encode_sb(make_ptrlen(blob, HELLO_BLOB_LEN), 0);
        put_dataz(out, "H");
        put_dataz(out, b64->s);
        strbuf_free(b64);
        smemclr(blob, sizeof(blob));
        first = false;
    }

    if (prf_kek) {
        /* W<b64 credential id>,<b64 blob>[,<b64 owner>]: the WebAuthn-PRF
         * wrap. The credential id travels IN the container so unwrap can
         * pick the field that belongs to this account; the owner tag is
         * informational text for the "wrapped elsewhere" message. */
        char *w = hello_w_field(prf_kek, prf_credid, prf_credidlen,
                                prf_owner, secret);
        if (!w) {
            strbuf_free(out);
            return NULL;
        }
        if (!first)
            put_dataz(out, ".");
        put_dataz(out, w);
        sfree(w);
        first = false;
    }

    if (recovery_passphrase) {
        unsigned char salt[HELLO_SALT_LEN], kek[HELLO_KEK_LEN];
        unsigned char blob[HELLO_BLOB_LEN];
        strbuf *b64salt, *b64blob;
        if (!hello_random(salt, sizeof(salt)) ||
            !hello_recovery_kek(recovery_passphrase, salt,
                                HELLO_ARGON_MEM, HELLO_ARGON_PASSES,
                                HELLO_ARGON_PARALLEL, kek) ||
            !hello_gcm_wrap(kek, secret, blob)) {
            smemclr(kek, sizeof(kek));
            strbuf_free(out);
            return NULL;
        }
        smemclr(kek, sizeof(kek));
        b64salt = base64_encode_sb(make_ptrlen(salt, sizeof(salt)), 0);
        b64blob = base64_encode_sb(make_ptrlen(blob, HELLO_BLOB_LEN), 0);
        if (!first)
            put_dataz(out, ".");
        put_fmt(out, "R%u,%u,%u,%s,%s",
                    (unsigned)HELLO_ARGON_MEM, (unsigned)HELLO_ARGON_PASSES,
                    (unsigned)HELLO_ARGON_PARALLEL, b64salt->s, b64blob->s);
        strbuf_free(b64salt);
        strbuf_free(b64blob);
        smemclr(blob, sizeof(blob));
    }

    return strbuf_to_str(out);
}

/* Find the body of the index-th field (0-based) starting with the given
 * letter (the text up to the next '.' or end). NULL if absent. A letter
 * may repeat: every enrolled account+machine adds its own W field. */
static const char *hello_container_field_n(const char *container,
                                           char letter, int index,
                                           size_t *len_out)
{
    const char *p;
    if (!container ||
        strncmp(container, HELLO_MARK, sizeof(HELLO_MARK) - 1) != 0)
        return NULL;
    p = container + sizeof(HELLO_MARK) - 1;
    while (*p) {
        const char *end = strchr(p, '.');
        size_t flen = end ? (size_t)(end - p) : strlen(p);
        if (flen > 0 && *p == letter && index-- == 0) {
            *len_out = flen - 1;
            return p + 1;
        }
        p += flen + (end ? 1 : 0);
    }
    return NULL;
}

static const char *hello_container_field(const char *container, char letter,
                                         size_t *len_out)
{
    return hello_container_field_n(container, letter, 0, len_out);
}

/* Split one W field body into its comma parts: credential id, blob and
 * the optional owner tag (all base64). false = malformed. */
static bool hello_w_parts(const char *f, size_t flen,
                          ptrlen *id, ptrlen *blob, ptrlen *owner)
{
    const char *c1 = memchr(f, ',', flen), *c2;
    if (!c1 || c1 == f)
        return false;
    *id = make_ptrlen(f, c1 - f);
    c2 = memchr(c1 + 1, ',', flen - (c1 + 1 - f));
    if (c2) {
        *blob = make_ptrlen(c1 + 1, c2 - (c1 + 1));
        *owner = make_ptrlen(c2 + 1, flen - (c2 + 1 - f));
    } else {
        *blob = make_ptrlen(c1 + 1, flen - (c1 + 1 - f));
        *owner = make_ptrlen("", 0);
    }
    return blob->len > 0;
}

/* One W field as text: W<b64 id>,<b64 blob>[,<b64 owner>]. NULL on a
 * wrap failure. */
static char *hello_w_field(const unsigned char prf_kek[32],
                           const unsigned char *prf_credid,
                           size_t prf_credidlen, const char *owner,
                           const unsigned char secret[KITTY_HELLO_SECRET_LEN])
{
    unsigned char blob[HELLO_BLOB_LEN];
    strbuf *b64id, *b64blob, *out;
    if (!hello_gcm_wrap(prf_kek, secret, blob))
        return NULL;
    b64id = base64_encode_sb(make_ptrlen(prf_credid, prf_credidlen), 0);
    b64blob = base64_encode_sb(make_ptrlen(blob, HELLO_BLOB_LEN), 0);
    out = strbuf_new();
    put_fmt(out, "W%s,%s", b64id->s, b64blob->s);
    if (owner && *owner) {
        strbuf *b64own = base64_encode_sb(ptrlen_from_asciz(owner), 0);
        put_fmt(out, ",%s", b64own->s);
        strbuf_free(b64own);
    }
    strbuf_free(b64id);
    strbuf_free(b64blob);
    smemclr(blob, sizeof(blob));
    return strbuf_to_str(out);
}

int kitty_hello_container_w_count(const char *container)
{
    size_t len;
    int n = 0;
    while (hello_container_field_n(container, 'W', n, &len))
        n++;
    return n;
}

char *kitty_hello_container_w_owner(const char *container, int index)
{
    size_t flen;
    const char *f = hello_container_field_n(container, 'W', index, &flen);
    ptrlen id, blob, owner;
    strbuf *raw;
    char *s;
    if (!f || !hello_w_parts(f, flen, &id, &blob, &owner) || !owner.len)
        return NULL;
    raw = base64_decode_sb(owner);
    if (!raw)
        return NULL;
    s = dupprintf("%.*s", (int)raw->len, (const char *)raw->u);
    strbuf_free(raw);
    return s;
}

int kitty_hello_container_w_credid(const char *container, int index,
                                   unsigned char **credid_out,
                                   size_t *credidlen_out)
{
    size_t flen;
    const char *f = hello_container_field_n(container, 'W', index, &flen);
    ptrlen id, blob, owner;
    strbuf *raw;

    *credid_out = NULL;
    if (!f)
        return 0;
    if (!hello_w_parts(f, flen, &id, &blob, &owner))
        return -1;
    raw = base64_decode_sb(id);
    if (!raw || raw->len == 0) {
        if (raw) strbuf_free(raw);
        return -1;
    }
    *credidlen_out = raw->len;
    *credid_out = snewn(raw->len, unsigned char);
    memcpy(*credid_out, raw->u, raw->len);
    strbuf_free(raw);
    return 1;
}

int kitty_hello_container_find_w(const char *container,
                                 const unsigned char *credid,
                                 size_t credidlen)
{
    int i, n = kitty_hello_container_w_count(container);
    for (i = 0; i < n; i++) {
        unsigned char *id = NULL;
        size_t idlen = 0;
        bool match;
        if (kitty_hello_container_w_credid(container, i, &id, &idlen) != 1)
            continue;
        match = idlen == credidlen && !memcmp(id, credid, credidlen);
        sfree(id);
        if (match)
            return i;
    }
    return -1;
}

/*
 * Remove the index-th W field - pure string surgery, no secret needed:
 * taking a door AWAY must not require opening one. Refuses to remove
 * the last door of the container (a sidecar with no doors is a lie
 * beside the key file - delete the file instead). Caller sfree; NULL =
 * refused or no such field.
 */
char *kitty_hello_container_remove_w(const char *container, int index)
{
    const char *mark_end;
    const char *p;
    strbuf *out;
    int wi = 0, doors = 0, removed = 0;

    if (!kitty_hello_container_valid(container))
        return NULL;
    doors = kitty_hello_container_w_count(container) +
            (kitty_hello_container_has_hello(container) ? 1 : 0) +
            (kitty_hello_container_has_recovery(container) ? 1 : 0);
    if (doors <= 1)
        return NULL;            /* never leave a doorless sidecar */
    if (index < 0 || index >= kitty_hello_container_w_count(container))
        return NULL;

    mark_end = container + sizeof(HELLO_MARK) - 1;
    out = strbuf_new();
    put_data(out, container, mark_end - container);
    p = mark_end;
    while (*p) {
        const char *end = strchr(p, '.');
        size_t flen = end ? (size_t)(end - p) : strlen(p);
        int skip = 0;
        if (flen > 0 && *p == 'W') {
            if (wi == index)
                skip = 1;
            wi++;
        }
        if (!skip) {
            if (out->len > (size_t)(mark_end - container))
                put_dataz(out, ".");
            put_data(out, p, flen);
        } else {
            removed = 1;
        }
        p += flen + (end ? 1 : 0);
    }
    if (!removed) {
        strbuf_free(out);
        return NULL;
    }
    return strbuf_to_str(out);
}

char *kitty_hello_container_append_prf(const char *container,
                                       const unsigned char prf_kek[32],
                                       const unsigned char *prf_credid,
                                       size_t prf_credidlen,
                                       const char *owner,
                                       const unsigned char
                                           secret[KITTY_HELLO_SECRET_LEN])
{
    char *w, *out;
    if (!kitty_hello_container_valid(container) || !prf_credid ||
        !prf_credidlen)
        return NULL;
    /* Every wrap is an independent GCM blob under the marker as AAD, so
     * the existing text is kept byte for byte: other accounts' doors are
     * never re-encoded, only ours is added. */
    w = hello_w_field(prf_kek, prf_credid, prf_credidlen, owner, secret);
    if (!w)
        return NULL;
    out = dupprintf("%s%s%s", container,
                    container[strlen(container) - 1] == ':' ? "" : ".", w);
    sfree(w);
    return out;
}

int kitty_hello_container_valid(const char *container)
{
    return kitty_hello_container_has_hello(container) ||
           kitty_hello_container_has_prf(container) ||
           kitty_hello_container_has_recovery(container);
}

int kitty_hello_container_has_hello(const char *container)
{
    size_t len;
    return hello_container_field(container, 'H', &len) != NULL;
}

int kitty_hello_container_has_recovery(const char *container)
{
    size_t len;
    return hello_container_field(container, 'R', &len) != NULL;
}

int kitty_hello_container_has_prf(const char *container)
{
    size_t len;
    return hello_container_field(container, 'W', &len) != NULL;
}

/* Decode a b64 field expected to be exactly wantlen bytes. */
static bool hello_b64_fixed(const char *s, size_t slen, unsigned char *out,
                            size_t wantlen)
{
    strbuf *raw = base64_decode_sb(make_ptrlen(s, slen));
    bool ok = raw && raw->len == wantlen;
    if (ok)
        memcpy(out, raw->u, wantlen);
    if (raw) {
        smemclr(raw->u, raw->len);
        strbuf_free(raw);
    }
    return ok;
}

int kitty_hello_container_open_kek(const char *container,
                                   const unsigned char hello_kek[32],
                                   unsigned char
                                       secret_out[KITTY_HELLO_SECRET_LEN])
{
    size_t flen;
    const char *f = hello_container_field(container, 'H', &flen);
    unsigned char blob[HELLO_BLOB_LEN];
    int ret;

    if (!f)
        return 0;
    if (!hello_b64_fixed(f, flen, blob, HELLO_BLOB_LEN))
        return -1;
    ret = hello_gcm_unwrap(hello_kek, blob, secret_out) ? 1 : -1;
    smemclr(blob, sizeof(blob));
    return ret;
}

/* Open ONE R field body with the given passphrase. 1/-1. */
static int hello_open_recovery_field(const char *f, size_t flen,
                                     const char *passphrase,
                                     unsigned char
                                         secret_out[KITTY_HELLO_SECRET_LEN])
{
    unsigned mem, passes, parallel;
    int consumed = 0;
    unsigned char salt[HELLO_SALT_LEN], kek[HELLO_KEK_LEN];
    unsigned char blob[HELLO_BLOB_LEN];
    const char *b64salt, *b64blob, *comma;
    int ret = -1;

    /* R<mem>,<passes>,<parallel>,<b64 salt>,<b64 blob> - the numbers are
     * whatever the WRITING build used; they go through argon2_params_bad
     * before the KDF ever sees them. */
    if (sscanf(f, "%u,%u,%u,%n", &mem, &passes, &parallel, &consumed) != 3 ||
        consumed <= 0 || (size_t)consumed >= flen)
        return -1;
    b64salt = f + consumed;
    comma = memchr(b64salt, ',', flen - consumed);
    if (!comma)
        return -1;
    b64blob = comma + 1;

    {
        /* An optional trailing ",c" tags a CODE door (the sidecar-bound
         * printout); the blob ends at that comma. Unknown tags are
         * ignored the same way. */
        const char *bend = memchr(b64blob, ',', flen - (b64blob - f));
        size_t bloblen = bend ? (size_t)(bend - b64blob)
                              : flen - (b64blob - f);
        if (!hello_b64_fixed(b64salt, comma - b64salt, salt,
                             HELLO_SALT_LEN) ||
            !hello_b64_fixed(b64blob, bloblen, blob, HELLO_BLOB_LEN))
            return -1;
    }

    if (hello_recovery_kek(passphrase, salt, mem, passes, parallel, kek))
        ret = hello_gcm_unwrap(kek, blob, secret_out) ? 1 : -1;

    smemclr(kek, sizeof(kek));
    smemclr(blob, sizeof(blob));
    return ret;
}

int kitty_hello_container_open_recovery(const char *container,
                                        const char *passphrase,
                                        unsigned char
                                            secret_out[KITTY_HELLO_SECRET_LEN])
{
    /* A container may carry SEVERAL R doors (a typed recovery passphrase
     * and a sidecar-bound recovery code are both R wraps); the given
     * text opens exactly the one keyed on it - the KDF+GCM refuse the
     * others - so trying each in turn is correct and cheap enough (one
     * Argon2 run per R). */
    int i = 0, ret = 0;
    size_t flen;
    const char *f;
    while ((f = hello_container_field_n(container, 'R', i++, &flen))
               != NULL) {
        int r = hello_open_recovery_field(f, flen, passphrase, secret_out);
        if (r == 1)
            return 1;
        ret = -1;
    }
    return ret;
}

/*
 * The RECOVERY CODE's printed form - deliberately unmistakable for the
 * printed passphrase (his report: the two looked identical and were
 * mixed up): "KRC1-" prefix, 8 groups of 4 hex (16 random bytes - a
 * KDF-protected door does not need 256 bits), and a 2-hex check group.
 * The parser is as tolerant as the passphrase one (case, dashes,
 * spaces) but REQUIRES the prefix.
 */
char *kitty_hello_code_text(const unsigned char code[16])
{
    static const char hexd[] = "0123456789ABCDEF";
    unsigned char chk[32];
    strbuf *sb = strbuf_new_nm();
    int i;
    ssh_hash *h = ssh_hash_new(&ssh_sha256);
    put_data(h, code, 16);
    ssh_hash_final(h, chk);
    put_dataz(sb, "KRC1");
    for (i = 0; i < 16; i++) {
        if ((i & 1) == 0)
            put_byte(sb, '-');
        put_byte(sb, hexd[code[i] >> 4]);
        put_byte(sb, hexd[code[i] & 15]);
    }
    put_fmt(sb, "-%c%c", hexd[chk[0] >> 4], hexd[chk[0] & 15]);
    return strbuf_to_str(sb);
}

int kitty_hello_code_from_text(const char *text, unsigned char code_out[16])
{
    unsigned char digits[34];
    unsigned char chk[32];
    int nd = 0, i;
    const char *p = text;

    if (!text)
        return 0;
    while (*p == ' ' || *p == '\t')
        p++;
    if ((p[0] != 'K' && p[0] != 'k') || (p[1] != 'R' && p[1] != 'r') ||
        (p[2] != 'C' && p[2] != 'c') || p[3] != '1')
        return 0;
    p += 4;
    for (; *p; p++) {
        int v;
        if (*p == '-' || *p == ' ' || *p == '\t')
            continue;
        if (*p >= '0' && *p <= '9') v = *p - '0';
        else if (*p >= 'a' && *p <= 'f') v = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F') v = *p - 'A' + 10;
        else return 0;
        if (nd >= 34)
            return 0;
        digits[nd++] = (unsigned char)v;
    }
    if (nd != 34)
        return 0;
    for (i = 0; i < 16; i++)
        code_out[i] = (unsigned char)((digits[i * 2] << 4) | digits[i * 2 + 1]);
    {
        ssh_hash *h = ssh_hash_new(&ssh_sha256);
        put_data(h, code_out, 16);
        ssh_hash_final(h, chk);
    }
    if (((digits[32] << 4) | digits[33]) != chk[0]) {
        smemclr(code_out, 16);
        return 0;
    }
    return 1;
}

int kitty_hello_container_r_count(const char *container)
{
    size_t len;
    int n = 0;
    while (hello_container_field_n(container, 'R', n, &len))
        n++;
    return n;
}

/* Is the index-th R door a CODE door (",c" tag)? 1/0; -1 = no such R. */
int kitty_hello_container_r_is_code(const char *container, int index)
{
    size_t flen;
    const char *f = hello_container_field_n(container, 'R', index, &flen);
    if (!f)
        return -1;
    return (flen >= 2 && f[flen - 2] == ',' && f[flen - 1] == 'c') ? 1 : 0;
}

/* Append one more R door to an existing container - the existing text is
 * kept byte for byte (the sidecar-bound recovery code is such a door:
 * an R keyed on a random printed code instead of a typed passphrase).
 * Caller sfree; NULL on failure. */
char *kitty_hello_container_append_recovery(
    const char *container, const char *passphrase,
    const unsigned char secret[KITTY_HELLO_SECRET_LEN])
{
    unsigned char salt[HELLO_SALT_LEN], kek[HELLO_KEK_LEN];
    unsigned char blob[HELLO_BLOB_LEN];
    strbuf *b64salt, *b64blob;
    char *out;

    if (!kitty_hello_container_valid(container) || !passphrase ||
        !*passphrase)
        return NULL;
    if (!hello_random(salt, sizeof(salt)) ||
        !hello_recovery_kek(passphrase, salt, HELLO_ARGON_MEM,
                            HELLO_ARGON_PASSES, HELLO_ARGON_PARALLEL, kek) ||
        !hello_gcm_wrap(kek, secret, blob)) {
        smemclr(kek, sizeof(kek));
        return NULL;
    }
    smemclr(kek, sizeof(kek));
    b64salt = base64_encode_sb(make_ptrlen(salt, sizeof(salt)), 0);
    b64blob = base64_encode_sb(make_ptrlen(blob, HELLO_BLOB_LEN), 0);
    /* ",c": this R is a CODE door (the sidecar-bound printout) - the
     * label distinction the door list needs; crypto-wise identical. */
    out = dupprintf("%s.R%u,%u,%u,%s,%s,c", container,
                    (unsigned)HELLO_ARGON_MEM, (unsigned)HELLO_ARGON_PASSES,
                    (unsigned)HELLO_ARGON_PARALLEL, b64salt->s, b64blob->s);
    strbuf_free(b64salt);
    strbuf_free(b64blob);
    smemclr(blob, sizeof(blob));
    return out;
}

/* Pull the PRF credential id out of the W field (caller sfree).
 * 1 = filled, 0 = no W field, -1 = malformed. */
int kitty_hello_container_prf_credid(const char *container,
                                     unsigned char **credid_out,
                                     size_t *credidlen_out)
{
    return kitty_hello_container_w_credid(container, 0, credid_out,
                                          credidlen_out);
}

int kitty_hello_container_open_prf(const char *container,
                                   const unsigned char prf_kek[32],
                                   unsigned char
                                       secret_out[KITTY_HELLO_SECRET_LEN])
{
    int i, n = kitty_hello_container_w_count(container);
    int ret = 0;

    /* Try every W: a KEK opens exactly the blob wrapped under it (the GCM
     * tag refuses the others), so no bookkeeping beyond the loop. */
    for (i = 0; i < n; i++) {
        size_t flen;
        const char *f = hello_container_field_n(container, 'W', i, &flen);
        ptrlen id, blob, owner;
        unsigned char raw[HELLO_BLOB_LEN];
        if (!f || !hello_w_parts(f, flen, &id, &blob, &owner) ||
            !hello_b64_fixed(blob.ptr, blob.len, raw, HELLO_BLOB_LEN)) {
            ret = -1;
            continue;
        }
        if (hello_gcm_unwrap(prf_kek, raw, secret_out)) {
            smemclr(raw, sizeof(raw));
            return 1;
        }
        smemclr(raw, sizeof(raw));
        ret = -1;
    }
    return ret;
}

/* The informational owner tag for a W written by this account:
 * DOMAIN\user@MACHINE. Caller sfree; never NULL. */
char *kitty_hello_owner_tag(void)
{
    char user[256 + 1], dom[256 + 1], host[256 + 1];
    DWORD ulen = sizeof(user), dlen = sizeof(dom), hlen = sizeof(host);
    if (!GetUserNameA(user, &ulen))
        strcpy(user, "?");
    if (!GetEnvironmentVariableA("USERDOMAIN", dom, dlen))
        strcpy(dom, "?");
    if (!GetComputerNameA(host, &hlen))
        strcpy(host, "?");
    return dupprintf("%s\%s@%s", dom, user, host);
}

char *kitty_hello_container_owners_text(const char *container)
{
    strbuf *sb = strbuf_new();
    int i, n = kitty_hello_container_w_count(container);
    for (i = 0; i < n; i++) {
        char *o = kitty_hello_container_w_owner(container, i);
        if (i)
            put_dataz(sb, ", ");
        put_dataz(sb, o ? o : "(untagged)");
        sfree(o);
    }
    if (kitty_hello_container_has_hello(container)) {
        if (n)
            put_dataz(sb, ", ");
        put_dataz(sb, "(Windows Hello key credential)");
    }
    return strbuf_to_str(sb);
}


int kitty_hello_container_my_w(const char *container)
{
    unsigned char *mine = NULL;
    size_t minelen = 0;
    int idx;
    if (kitty_hello_prf_my_credid(&mine, &minelen) != 1)
        return -1;
    idx = kitty_hello_container_find_w(container, mine, minelen);
    sfree(mine);
    return idx;
}

/*
 * The printed form of the secret (the BitLocker-recovery-key pattern):
 * 64 hex digits in 8 groups plus a 4-digit check group (the first two
 * bytes of SHA-256 over the secret), dash-separated. This same string
 * IS the PPK's literal passphrase, so a printout opens the key in ANY
 * PuTTY-compatible tool with no KiTTY code involved - which is why the
 * encoding is fixed for ever alongside the container marker.
 */
char *kitty_hello_secret_text(const unsigned char
                                  secret[KITTY_HELLO_SECRET_LEN])
{
    static const char hex[] = "0123456789ABCDEF";
    unsigned char check[32];
    strbuf *sb = strbuf_new_nm();
    int i;
    ssh_hash *h = ssh_hash_new(&ssh_sha256);

    put_data(h, secret, KITTY_HELLO_SECRET_LEN);
    ssh_hash_final(h, check);

    for (i = 0; i < KITTY_HELLO_SECRET_LEN; i++) {
        if (i > 0 && i % 4 == 0)
            put_byte(sb, '-');
        put_byte(sb, hex[secret[i] >> 4]);
        put_byte(sb, hex[secret[i] & 15]);
    }
    put_fmt(sb, "-%c%c%c%c", hex[check[0] >> 4], hex[check[0] & 15],
            hex[check[1] >> 4], hex[check[1] & 15]);
    smemclr(check, sizeof(check));
    return strbuf_to_str(sb);
}

/* Parse the printed form back: case-insensitive, dashes/spaces ignored,
 * check group verified. 1 = secret_out filled, 0 = not a valid printed
 * secret (wrong length, non-hex, or failed check). */
int kitty_hello_secret_from_text(const char *text,
                                 unsigned char
                                     secret_out[KITTY_HELLO_SECRET_LEN])
{
    unsigned char bytes[KITTY_HELLO_SECRET_LEN + 2];
    unsigned char check[32];
    int nibbles = 0;
    const char *p;
    bool ok;
    ssh_hash *h;

    for (p = text; *p; p++) {
        int v;
        if (*p == '-' || *p == ' ' || *p == '\t')
            continue;
        if (*p >= '0' && *p <= '9') v = *p - '0';
        else if (*p >= 'a' && *p <= 'f') v = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F') v = *p - 'A' + 10;
        else return 0;
        if (nibbles >= 2 * (int)sizeof(bytes))
            return 0;
        if (nibbles % 2 == 0)
            bytes[nibbles / 2] = (unsigned char)(v << 4);
        else
            bytes[nibbles / 2] |= (unsigned char)v;
        nibbles++;
    }
    if (nibbles != 2 * (int)sizeof(bytes))
        return 0;

    h = ssh_hash_new(&ssh_sha256);
    put_data(h, bytes, KITTY_HELLO_SECRET_LEN);
    ssh_hash_final(h, check);
    ok = (bytes[KITTY_HELLO_SECRET_LEN] == check[0] &&
          bytes[KITTY_HELLO_SECRET_LEN + 1] == check[1]);
    if (ok)
        memcpy(secret_out, bytes, KITTY_HELLO_SECRET_LEN);
    smemclr(bytes, sizeof(bytes));
    smemclr(check, sizeof(check));
    return ok ? 1 : 0;
}



/* A fresh secret from the OS CSPRNG. 1 = filled, 0 = the generator
 * failed (never use the buffer then). */
int kitty_hello_new_secret(unsigned char secret[KITTY_HELLO_SECRET_LEN])
{
    return hello_random(secret, KITTY_HELLO_SECRET_LEN) ? 1 : 0;
}
