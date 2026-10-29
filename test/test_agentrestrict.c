/*
 * test_agentrestrict - regression tests for kitty/kitty_agentrestrict.c:
 * the restrict-destination-v00@openssh.com constraint (ssh-add -h) and
 * OpenSSH's checks on it, the walk of a connection's session-binds (local
 * use, a failed bind, the destination, the user, forwarding hops, the
 * hostbound method, a host certificate against a CA entry), the sign
 * check's userauth data, the rule store (ssh-add -h replacing a key's
 * links, forwarding allowed, tidying) and its bytes: a round trip, and a
 * file cut short, changed or of another kind refused as damaged with the
 * store left as it was.
 *
 * The keys are fixed ed25519 public keys and one host certificate (key
 * web02, principal web03, signed by the key "ca"), made once with
 * ssh-keygen. Returns non-zero on any failure.
 */
#ifdef _WIN32
#include <winsock2.h>   /* before windows.h: putty.h pulls it in further down */
#include <windows.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "putty.h"
#include "ssh.h"
#include "kitty/kitty_agentrestrict.h"

const char *appname = "test_agentrestrict";

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

/* sshpubk.c and the key code are linked for the key types; nothing here
 * writes a key or makes a key exchange, so neither is ever called. */
void old_keyfile_warning(void) {}
void random_read(void *buf, size_t size)
{
    (void)buf;
    (void)size;
    modalfatalbox("random_read called in test_agentrestrict");
}

static const char *const B64_USER =
    "AAAAC3NzaC1lZDI1NTE5AAAAIExom5kayN8hDg6F/tWETm7ZB/pxT5AnD9miVHHxyWjz";
static const char *const B64_WEB01 =
    "AAAAC3NzaC1lZDI1NTE5AAAAIImVTyHeP0g80hvhVutDgpt/BmSweAALeKsSF+sDBdLM";
static const char *const B64_WEB02 =
    "AAAAC3NzaC1lZDI1NTE5AAAAIJaznXTFVKK9hf8gjn9npSgOtWGq4+e5pk/IiBv732ih";
static const char *const B64_JUMP01 =
    "AAAAC3NzaC1lZDI1NTE5AAAAID885J4o8Corh5ACQFwkbyLGaoTr0esG53fPHVYFiYzj";
static const char *const B64_CA =
    "AAAAC3NzaC1lZDI1NTE5AAAAIH/vQofRFaKADuSxqWxImjxsSufce7bDr+P9IP0aILxu";
static const char *const B64_CERT_WEB03 =
    "AAAAIHNzaC1lZDI1NTE5LWNlcnQtdjAxQG9wZW5zc2guY29tAAAAICjzrenu4ZJHysOR"
    "ewRdsVgGFNO/Q58ENeTjOYxNB/pHAAAAIJaznXTFVKK9hf8gjn9npSgOtWGq4+e5pk/I"
    "iBv732ihAAAAAAAAAAAAAAACAAAABXdlYjAzAAAACQAAAAV3ZWIwMwAAAAAAAAAA////"
    "//////8AAAAAAAAAAAAAAAAAAAAzAAAAC3NzaC1lZDI1NTE5AAAAIH/vQofRFaKADuSx"
    "qWxImjxsSufce7bDr+P9IP0aILxuAAAAUwAAAAtzc2gtZWQyNTUxOQAAAEDoPDJ4IUDE"
    "QNtlyA9VXjibmOF1q2Y23hr5pncLlW3+uQTCBVsiTML3PSotVwGMqBNP4vZm2eTiVrnY"
    "ZOihv7QL";

static int failures = 0;

static void expect(const char *what, bool got, bool want)
{
    if (got != want) {
        printf("FAIL [%s]: got %d, want %d\n", what, (int)got, (int)want);
        failures++;
    }
}

static void expect_why(const char *what, const char *got, const char *want)
{
    if (!got || strcmp(got, want)) {
        printf("FAIL [%s]: reason %s, want %s\n", what, got ? got : "(none)",
               want);
        failures++;
    }
}

static strbuf *blob(const char *b64)
{
    return base64_decode_sb(ptrlen_from_asciz(b64));
}

/* One destination: [from host+key >] [user@]to host + key. */
static void add_rule(kar_rules *r, const char *fromhost, strbuf *fromkey,
                     const char *user, const char *tohost, strbuf *tokey,
                     bool to_is_ca)
{
    kar_rule d;
    memset(&d, 0, sizeof(d));
    if (fromhost) {
        d.from.host = dupstr(fromhost);
        kar_hop_add_key(&d.from, ptrlen_from_strbuf(fromkey), false);
    }
    d.to.user = user ? dupstr(user) : NULL;
    d.to.host = dupstr(tohost);
    kar_hop_add_key(&d.to, ptrlen_from_strbuf(tokey), to_is_ca);
    kar_rules_add(r, &d);
    kar_hop_free(&d.from);
    kar_hop_free(&d.to);
}

/* The constraint's data on the wire, then parsed back. */
static kar_rules *round_trip(const kar_rules *r, const char **err)
{
    strbuf *sb = strbuf_new();
    BinarySource src[1];
    kar_rules *out;
    kar_encode_constraint(BinarySink_UPCAST(sb), r);
    BinarySource_BARE_INIT_PL(src, ptrlen_from_strbuf(sb));
    out = kar_parse_constraint(src, err);
    strbuf_free(sb);
    return out;
}

/* A constraint built by hand: one destination whose hops are given as
 * already-encoded strings. */
static kar_rules *parse_raw(ptrlen from, ptrlen to, ptrlen reserved,
                            const char **err)
{
    strbuf *one = strbuf_new(), *all = strbuf_new(), *msg = strbuf_new();
    BinarySource src[1];
    kar_rules *out;
    put_stringpl(one, from);
    put_stringpl(one, to);
    put_stringpl(one, reserved);
    put_stringsb(all, one);
    put_stringsb(msg, all);
    BinarySource_BARE_INIT_PL(src, ptrlen_from_strbuf(msg));
    out = kar_parse_constraint(src, err);
    strbuf_free(msg);
    return out;
}

static strbuf *hop(const char *user, const char *host, const char *reserved,
                   strbuf *key, int is_ca)
{
    strbuf *h = strbuf_new();
    put_stringz(h, user);
    put_stringz(h, host);
    put_stringz(h, reserved);
    if (key) {
        put_stringpl(h, ptrlen_from_strbuf(key));
        put_byte(h, is_ca);
    }
    return h;
}

static strbuf *userauth(ptrlen sid, const char *user, const char *method,
                        ptrlen key, ptrlen hostkey)
{
    strbuf *sb = strbuf_new();
    put_stringpl(sb, sid);
    put_byte(sb, SSH2_MSG_USERAUTH_REQUEST);
    put_stringz(sb, user);
    put_stringz(sb, "ssh-connection");
    put_stringz(sb, method);
    put_bool(sb, true);
    put_stringz(sb, "ssh-ed25519");
    put_stringpl(sb, key);
    if (hostkey.len)
        put_stringpl(sb, hostkey);
    return sb;
}

static void test_parser(strbuf *web01, strbuf *jump01)
{
    kar_rules *r = kar_rules_new(), *p;
    const char *err = NULL;
    strbuf *f, *t, *bad;

    add_rule(r, NULL, NULL, NULL, "web01", web01, false);
    add_rule(r, "jump01", jump01, "alice", "web01", web01, false);
    p = round_trip(r, &err);
    expect("round trip: parsed", p != NULL, true);
    if (p) {
        expect("round trip: two destinations", p->n == 2, true);
        expect("round trip: first equal", kar_rule_equal(&p->v[0], &r->v[0]),
               true);
        expect("round trip: second equal",
               kar_rule_equal(&p->v[1], &r->v[1]), true);
        expect("round trip: from hop of the first is this computer",
               p->v[0].from.host == NULL && p->v[0].from.nkeys == 0, true);
        kar_rules_free(p);
    }
    kar_rules_free(r);

    /* an empty list would lift a key's rules from a client */
    r = kar_rules_new();
    p = round_trip(r, &err);
    expect("no destination: refused", p == NULL, true);
    kar_rules_free(r);

    /* OpenSSH's consistency checks */
    f = hop("", "", "", NULL, 0);
    t = hop("", "web01", "", NULL, 0);
    p = parse_raw(ptrlen_from_strbuf(f), ptrlen_from_strbuf(t),
                  PTRLEN_LITERAL(""), &err);
    expect("a \"to\" hop without a key: refused", p == NULL, true);
    kar_rules_free(p);
    strbuf_free(t);
    t = hop("", "", "", web01, 0);
    p = parse_raw(ptrlen_from_strbuf(f), ptrlen_from_strbuf(t),
                  PTRLEN_LITERAL(""), &err);
    expect("a \"to\" hop without a host name: refused", p == NULL, true);
    kar_rules_free(p);
    strbuf_free(t);
    strbuf_free(f);

    t = hop("", "web01", "", web01, 0);
    f = hop("", "jump01", "", NULL, 0);
    p = parse_raw(ptrlen_from_strbuf(f), ptrlen_from_strbuf(t),
                  PTRLEN_LITERAL(""), &err);
    expect("a \"from\" host without keys: refused", p == NULL, true);
    kar_rules_free(p);
    strbuf_free(f);
    f = hop("", "", "", jump01, 0);
    p = parse_raw(ptrlen_from_strbuf(f), ptrlen_from_strbuf(t),
                  PTRLEN_LITERAL(""), &err);
    expect("\"from\" keys without a host: refused", p == NULL, true);
    kar_rules_free(p);
    strbuf_free(f);
    f = hop("bob", "jump01", "", jump01, 0);
    p = parse_raw(ptrlen_from_strbuf(f), ptrlen_from_strbuf(t),
                  PTRLEN_LITERAL(""), &err);
    expect("a \"from\" user: refused", p == NULL, true);
    kar_rules_free(p);
    strbuf_free(f);
    f = hop("", "", "", NULL, 0);
    p = parse_raw(ptrlen_from_strbuf(f), ptrlen_from_strbuf(t),
                  PTRLEN_LITERAL("x"), &err);
    expect("a reserved field with data: refused", p == NULL, true);
    kar_rules_free(p);
    p = parse_raw(ptrlen_from_strbuf(f), ptrlen_from_strbuf(t),
                  PTRLEN_LITERAL(""), &err);
    expect("the plain destination: accepted", p != NULL, true);
    kar_rules_free(p);
    strbuf_free(t);
    t = hop("", "web01", "x", web01, 0);
    p = parse_raw(ptrlen_from_strbuf(f), ptrlen_from_strbuf(t),
                  PTRLEN_LITERAL(""), &err);
    expect("a hop's reserved field with data: refused", p == NULL, true);
    kar_rules_free(p);
    strbuf_free(t);

    bad = strbuf_new();
    put_stringz(bad, "ssh-ed25519");
    put_stringz(bad, "short");
    t = hop("", "web01", "", bad, 0);
    p = parse_raw(ptrlen_from_strbuf(f), ptrlen_from_strbuf(t),
                  PTRLEN_LITERAL(""), &err);
    expect("an ed25519 host key of 5 bytes: refused", p == NULL, true);
    kar_rules_free(p);
    strbuf_free(t);
    strbuf_free(bad);

    {
        char *user = snewn(KAR_MAX_USER + 2, char);
        memset(user, 'u', KAR_MAX_USER + 2);
        user[KAR_MAX_USER] = '\0';
        t = hop(user, "web01", "", web01, 0);
        p = parse_raw(ptrlen_from_strbuf(f), ptrlen_from_strbuf(t),
                      PTRLEN_LITERAL(""), &err);
        expect("a user of 256 bytes: accepted", p != NULL, true);
        kar_rules_free(p);
        strbuf_free(t);
        user[KAR_MAX_USER] = 'u';
        user[KAR_MAX_USER + 1] = '\0';
        t = hop(user, "web01", "", web01, 0);
        p = parse_raw(ptrlen_from_strbuf(f), ptrlen_from_strbuf(t),
                      PTRLEN_LITERAL(""), &err);
        expect("a user of 257 bytes: refused", p == NULL, true);
        kar_rules_free(p);
        strbuf_free(t);
        sfree(user);
    }
    strbuf_free(f);

    /* OpenSSH's 1024 destinations per key */
    r = kar_rules_new();
    for (int i = 0; i < KAR_MAX_RULES; i++)
        add_rule(r, NULL, NULL, NULL, "web01", web01, false);
    p = round_trip(r, &err);
    expect("1024 destinations: accepted", p != NULL, true);
    kar_rules_free(p);
    expect("a 1025th is not added to a set", kar_rules_add(r, &r->v[0]),
           false);
    {
        /* 1025 on the wire, written by hand */
        strbuf *all = strbuf_new(), *msg = strbuf_new(), *one;
        BinarySource src[1];
        for (int i = 0; i <= KAR_MAX_RULES; i++) {
            strbuf *fh = hop("", "", "", NULL, 0);
            strbuf *th = hop("", "web01", "", web01, 0);
            one = strbuf_new();
            put_stringsb(one, fh);
            put_stringsb(one, th);
            put_stringz(one, "");
            put_stringsb(all, one);
        }
        put_stringsb(msg, all);
        BinarySource_BARE_INIT_PL(src, ptrlen_from_strbuf(msg));
        p = kar_parse_constraint(src, &err);
        expect("1025 destinations: refused", p == NULL, true);
        kar_rules_free(p);
        strbuf_free(msg);
    }
    kar_rules_free(r);
}

static void test_walk(strbuf *user, strbuf *web01, strbuf *web02,
                      strbuf *jump01, strbuf *ca, strbuf *cert)
{
    kar_rules *r = kar_rules_new();
    kar_bind b[2];
    const char *why;
    static const unsigned char sid1[] = { 1, 2, 3, 4 };
    static const unsigned char sid2[] = { 5, 6, 7, 8 };
    ptrlen s1 = make_ptrlen(sid1, sizeof(sid1));
    ptrlen s2 = make_ptrlen(sid2, sizeof(sid2));
    ptrlen ukey = ptrlen_from_strbuf(user), none = make_ptrlen(NULL, 0);
    strbuf *data;

    add_rule(r, NULL, NULL, NULL, "web01", web01, false);

    /* no bind: local use */
    why = NULL;
    expect("no bind: listed", kar_permitted(r, b, 0, false, NULL, &why),
           true);
    expect("a failed bind: hidden", kar_permitted(r, b, 0, true, NULL, &why),
           false);
    expect_why("a failed bind: the reason", why, KAR_WHY_BIND_FAILED);
    data = userauth(s1, "alice", "publickey", ukey, none);
    expect("no bind: no signature",
           kar_sign_permitted(r, b, 0, false, ukey,
                              ptrlen_from_strbuf(data), &why), false);
    expect_why("no bind: the reason", why, KAR_WHY_UNBOUND);
    expect("a failed bind: no signature",
           kar_sign_permitted(r, b, 0, true, ukey,
                              ptrlen_from_strbuf(data), &why), false);
    expect_why("a failed bind, signing: the reason", why,
               KAR_WHY_BIND_FAILED);

    /* bound to the permitted server */
    b[0].hostkey = ptrlen_from_strbuf(web01);
    b[0].sid = s1;
    b[0].forwarding = false;
    expect("bound to web01: listed", kar_permitted(r, b, 1, true, NULL, &why),
           true);
    expect("bound to web01: signs",
           kar_sign_permitted(r, b, 1, true, ukey, ptrlen_from_strbuf(data),
                              &why), true);
    strbuf_free(data);
    data = userauth(s2, "alice", "publickey", ukey, none);
    expect("another session id: refused",
           kar_sign_permitted(r, b, 1, true, ukey, ptrlen_from_strbuf(data),
                              &why), false);
    expect_why("another session id: the reason", why, KAR_WHY_SESSION_ID);
    strbuf_free(data);
    data = userauth(s1, "alice", "password", ukey, none);
    expect("no publickey request: refused",
           kar_sign_permitted(r, b, 1, true, ukey, ptrlen_from_strbuf(data),
                              &why), false);
    expect_why("no publickey request: the reason", why,
               KAR_WHY_NOT_USERAUTH);
    strbuf_free(data);
    data = userauth(s1, "alice", "publickey", ptrlen_from_strbuf(web02),
                    none);
    expect("another key in the data: refused",
           kar_sign_permitted(r, b, 1, true, ukey, ptrlen_from_strbuf(data),
                              &why), false);
    strbuf_free(data);
    data = userauth(s1, "alice", "publickey", ukey, none);
    put_byte(data, 0);
    expect("data after the request: refused",
           kar_sign_permitted(r, b, 1, true, ukey, ptrlen_from_strbuf(data),
                              &why), false);
    strbuf_free(data);
    expect("data that is no request: refused",
           kar_sign_permitted(r, b, 1, true, ukey,
                              PTRLEN_LITERAL("random bytes"), &why), false);

    /* bound to another server */
    b[0].hostkey = ptrlen_from_strbuf(web02);
    expect("bound to web02: hidden", kar_permitted(r, b, 1, true, NULL, &why),
           false);
    expect_why("bound to web02: the reason", why, KAR_WHY_DESTINATION);
    data = userauth(s1, "alice", "publickey", ukey, none);
    expect("bound to web02: no signature",
           kar_sign_permitted(r, b, 1, true, ukey, ptrlen_from_strbuf(data),
                              &why), false);
    strbuf_free(data);
    kar_rules_free(r);

    /* the user on the last hop */
    r = kar_rules_new();
    add_rule(r, NULL, NULL, "a*", "web01", web01, false);
    b[0].hostkey = ptrlen_from_strbuf(web01);
    data = userauth(s1, "alice", "publickey", ukey, none);
    expect("user alice for a*: signs",
           kar_sign_permitted(r, b, 1, true, ukey, ptrlen_from_strbuf(data),
                              &why), true);
    strbuf_free(data);
    data = userauth(s1, "bob", "publickey", ukey, none);
    expect("user bob for a*: refused",
           kar_sign_permitted(r, b, 1, true, ukey, ptrlen_from_strbuf(data),
                              &why), false);
    expect_why("user bob: the reason", why, KAR_WHY_DESTINATION);
    strbuf_free(data);
    expect("the user is not checked for a listing",
           kar_permitted(r, b, 1, true, NULL, &why), true);
    kar_rules_free(r);

    /* through a jump host with a forwarded agent */
    r = kar_rules_new();
    add_rule(r, NULL, NULL, NULL, "jump01", jump01, false);
    b[0].hostkey = ptrlen_from_strbuf(jump01);
    b[0].sid = s1;
    b[0].forwarding = true;
    b[1].hostkey = ptrlen_from_strbuf(web01);
    b[1].sid = s2;
    b[1].forwarding = false;
    expect("forwarded to jump01, no rule beyond it: hidden there",
           kar_permitted(r, b, 1, true, NULL, &why), false);
    data = userauth(s2, "alice", "publickey-hostbound-v00@openssh.com", ukey,
                    ptrlen_from_strbuf(web01));
    expect("jump01 > web01 without that rule: refused",
           kar_sign_permitted(r, b, 2, true, ukey, ptrlen_from_strbuf(data),
                              &why), false);
    expect_why("jump01 > web01 without that rule: the reason", why,
               KAR_WHY_DESTINATION);
    add_rule(r, "jump01", jump01, NULL, "web01", web01, false);
    expect("forwarded to jump01, a rule beyond it: listed there",
           kar_permitted(r, b, 1, true, NULL, &why), true);
    expect("jump01 > web01: signs",
           kar_sign_permitted(r, b, 2, true, ukey, ptrlen_from_strbuf(data),
                              &why), true);
    strbuf_free(data);
    data = userauth(s2, "alice", "publickey", ukey, none);
    expect("two binds without the hostbound method: refused",
           kar_sign_permitted(r, b, 2, true, ukey, ptrlen_from_strbuf(data),
                              &why), false);
    expect_why("two binds without the hostbound method: the reason", why,
               KAR_WHY_HOSTKEY);
    strbuf_free(data);
    data = userauth(s2, "alice", "publickey-hostbound-v00@openssh.com", ukey,
                    ptrlen_from_strbuf(web02));
    expect("the hostbound key not the last bind's: refused",
           kar_sign_permitted(r, b, 2, true, ukey, ptrlen_from_strbuf(data),
                              &why), false);
    expect_why("the hostbound key not the last bind's: the reason", why,
               KAR_WHY_HOSTKEY);
    strbuf_free(data);
    data = userauth(s1, "alice", "publickey", ukey, none);
    expect("signing on the forwarding bind: refused",
           kar_sign_permitted(r, b, 1, true, ukey, ptrlen_from_strbuf(data),
                              &why), false);
    expect_why("signing on the forwarding bind: the reason", why,
               KAR_WHY_FWD_HOP);
    strbuf_free(data);
    kar_rules_free(r);

    /* a host certificate against a CA entry */
    r = kar_rules_new();
    add_rule(r, NULL, NULL, NULL, "web03", ca, true);
    b[0].hostkey = ptrlen_from_strbuf(cert);
    b[0].sid = s1;
    b[0].forwarding = false;
    expect("a certificate for web03 from the CA: listed",
           kar_permitted(r, b, 1, true, NULL, &why), true);
    kar_rules_free(r);
    r = kar_rules_new();
    add_rule(r, NULL, NULL, NULL, "web04", ca, true);
    expect("the same certificate for web04: hidden",
           kar_permitted(r, b, 1, true, NULL, &why), false);
    kar_rules_free(r);
    r = kar_rules_new();
    add_rule(r, NULL, NULL, NULL, "web03", web02, false);
    expect("a certificate against its key as a plain entry: hidden",
           kar_permitted(r, b, 1, true, NULL, &why), false);
    kar_rules_free(r);
    r = kar_rules_new();
    add_rule(r, NULL, NULL, NULL, "web03", ca, false);
    expect("a certificate against the CA key not marked as CA: hidden",
           kar_permitted(r, b, 1, true, NULL, &why), false);
    kar_rules_free(r);
    r = kar_rules_new();
    add_rule(r, NULL, NULL, NULL, "web02", web02, true);
    b[0].hostkey = ptrlen_from_strbuf(web02);
    expect("a plain key against a CA entry: hidden",
           kar_permitted(r, b, 1, true, NULL, &why), false);
    kar_rules_free(r);
}

static void test_store(strbuf *user, strbuf *web01, strbuf *web02,
                       strbuf *jump01)
{
    kar_store *s = kar_store_new(), *t = kar_store_new();
    kar_rules *r = kar_rules_new(), *got;
    char *fp = kar_key_fp(ptrlen_from_strbuf(user));
    const char *err;
    strbuf *bytes;
    unsigned id1, id2;
    kar_rule d;

    expect("an empty store restricts nothing",
           kar_store_restricted(s, fp), false);
    expect("an empty store has no rules", kar_store_rules(s, fp) == NULL,
           true);

    /* ssh-add -h */
    add_rule(r, NULL, NULL, NULL, "web01", web01, false);
    add_rule(r, NULL, NULL, "alice", "web02", web02, false);
    kar_store_set_key_rules(s, fp, "user key", ptrlen_from_strbuf(user), r);
    expect("-h: the key is restricted", kar_store_restricted(s, fp), true);
    got = kar_store_rules(s, fp);
    expect("-h: two rules", got && got->n == 2, true);
    kar_rules_free(got);
    expect("-h: the key's row", kar_store_key(s, fp) != NULL, true);

    /* a re-add with -h replaces the links (decision D8) */
    kar_rules_free(r);
    r = kar_rules_new();
    add_rule(r, NULL, NULL, NULL, "web01", web01, false);
    kar_store_set_key_rules(s, fp, NULL, ptrlen_from_strbuf(user), r);
    got = kar_store_rules(s, fp);
    expect("-h again: one rule left", got && got->n == 1, true);
    kar_rules_free(got);
    expect("-h again: the comment kept",
           !strcmp(kar_store_key(s, fp)->comment, "user key"), true);
    expect("-h again: the old destination stays in the table",
           s->ndest == 2, true);
    kar_store_tidy(s);
    expect("tidy: the destination no link uses is gone", s->ndest == 1,
           true);

    /* forwarding allowed: from jump01 to the key's other destinations */
    memset(&d, 0, sizeof(d));
    d.to.host = dupstr("jump01");
    kar_hop_add_key(&d.to, ptrlen_from_strbuf(jump01), false);
    id1 = kar_store_dest_for(s, &d);
    id2 = kar_store_dest_for(s, &d);
    expect("an equal destination is found, not added", id1 == id2, true);
    kar_hop_free(&d.to);
    kar_store_link(s, fp, id1, KAR_LINK_FORWARD);
    got = kar_store_rules(s, fp);
    expect("forwarding allowed: web01, jump01, jump01 > web01",
           got && got->n == 3, true);
    if (got && got->n == 3) {
        bool found = false;
        for (size_t i = 0; i < got->n; i++)
            if (got->v[i].from.host && !strcmp(got->v[i].from.host, "jump01")
                && !strcmp(got->v[i].to.host, "web01"))
                found = true;
        expect("forwarding allowed: the jump01 > web01 rule", found, true);
    }
    kar_rules_free(got);

    /* the bytes of the file */
    bytes = strbuf_new();
    kar_store_encode(s, bytes);
    expect("decode: the store's own bytes",
           kar_store_decode(t, ptrlen_from_strbuf(bytes), &err), true);
    {
        kar_rules *a = kar_store_rules(s, fp), *b2 = kar_store_rules(t, fp);
        char *ta = kar_rules_text(a), *tb = kar_rules_text(b2);
        expect("decode: the same rules", !strcmp(ta, tb), true);
        expect("decode: the same links", t->nlinks == s->nlinks, true);
        {
            bool fwd = false;
            for (size_t i = 0; i < t->nlinks; i++)
                if (t->links[i].flags & KAR_LINK_FORWARD)
                    fwd = true;
            expect("decode: the forwarding flag kept", fwd, true);
        }
        sfree(ta);
        sfree(tb);
        kar_rules_free(a);
        kar_rules_free(b2);
    }
    {
        size_t keep = t->nlinks;
        strbuf *mod = strbuf_dup(ptrlen_from_strbuf(bytes));
        mod->u[mod->len / 2] ^= 0x40;
        expect("decode: one byte changed is damaged",
               kar_store_decode(t, ptrlen_from_strbuf(mod), &err), false);
        expect("decode: a damaged file leaves the store as it was",
               t->nlinks == keep, true);
        strbuf_free(mod);
        expect("decode: cut short is damaged",
               kar_store_decode(t, make_ptrlen(bytes->u, bytes->len - 10),
                                &err), false);
        expect("decode: nothing is damaged",
               kar_store_decode(t, make_ptrlen(NULL, 0), &err), false);
        expect("decode: another file is damaged",
               kar_store_decode(t, PTRLEN_LITERAL(
                                    "a text file of a reasonable length, "
                                    "with nothing of a rule file in it"),
                                &err), false);
    }
    {
        /* a valid checksum over a body of another kind */
        strbuf *body = strbuf_new(), *file = strbuf_new();
        unsigned char hash[32];
        ssh_hash *h;
        put_stringz(body, "kageant-rules-2");
        put_uint32(body, 1);
        put_uint32(body, 0);
        put_uint32(body, 0);
        put_uint32(body, 0);
        h = ssh_hash_new(&ssh_sha256);
        put_datapl(h, ptrlen_from_strbuf(body));
        ssh_hash_final(h, hash);
        put_datapl(file, ptrlen_from_strbuf(body));
        put_string(file, hash, sizeof(hash));
        expect("decode: another version is damaged",
               kar_store_decode(t, ptrlen_from_strbuf(file), &err), false);
        strbuf_free(body);
        strbuf_free(file);
    }
    {
        /* the empty store's own bytes are a valid file */
        kar_store *e = kar_store_new();
        strbuf *eb = strbuf_new();
        kar_store_encode(e, eb);
        expect("decode: an empty store",
               kar_store_decode(t, ptrlen_from_strbuf(eb), &err), true);
        expect("decode: an empty store restricts nothing",
               kar_store_restricted(t, fp), false);
        strbuf_free(eb);
        kar_store_free(e);
    }
    strbuf_free(bytes);

    /* copy keeps the damaged mark; unlinking lifts the restriction */
    s->damaged = true;
    kar_store_copy(t, s);
    expect("copy: damaged kept", t->damaged, true);
    kar_store_unlink_key(s, fp);
    expect("unlinked: not restricted", kar_store_restricted(s, fp), false);

    sfree(fp);
    kar_rules_free(r);
    kar_store_free(s);
    kar_store_free(t);
}

static void test_texts(strbuf *web01, strbuf *jump01)
{
    kar_rules *r = kar_rules_new();
    char *t;
    add_rule(r, NULL, NULL, "alice", "web01", web01, false);
    add_rule(r, "jump01", jump01, NULL, "web01", web01, false);
    t = kar_rules_text(r);
    expect("text: user@host first",
           !strncmp(t, "alice@web01 (SHA256:", 20), true);
    expect("text: from > to second", strstr(t, ", jump01 > web01 (SHA256:")
           != NULL, true);
    sfree(t);
    kar_rules_free(r);
    expect("wildcard: a* takes alice", kar_wildcard("alice", "a*"), true);
    expect("wildcard: a?ice takes alice", kar_wildcard("alice", "a?ice"),
           true);
    expect("wildcard: b* refuses alice", kar_wildcard("alice", "b*"), false);
}

int main(void)
{
    strbuf *user, *web01, *web02, *jump01, *ca, *cert;

    /* unbuffered: an abort still shows how far the run got */
    setvbuf(stdout, NULL, _IONBF, 0);
    user = blob(B64_USER);
    web01 = blob(B64_WEB01);
    web02 = blob(B64_WEB02);
    jump01 = blob(B64_JUMP01);
    ca = blob(B64_CA);
    cert = blob(B64_CERT_WEB03);

    expect("fixture keys decode",
           kar_blob_valid(ptrlen_from_strbuf(user)) &&
           kar_blob_valid(ptrlen_from_strbuf(web01)) &&
           kar_blob_valid(ptrlen_from_strbuf(web02)) &&
           kar_blob_valid(ptrlen_from_strbuf(jump01)) &&
           kar_blob_valid(ptrlen_from_strbuf(ca)) &&
           kar_blob_valid(ptrlen_from_strbuf(cert)), true);

    printf("parser\n");
    test_parser(web01, jump01);
    printf("walk\n");
    test_walk(user, web01, web02, jump01, ca, cert);
    printf("store\n");
    test_store(user, web01, web02, jump01);
    printf("texts\n");
    test_texts(web01, jump01);

    strbuf_free(user);
    strbuf_free(web01);
    strbuf_free(web02);
    strbuf_free(jump01);
    strbuf_free(ca);
    strbuf_free(cert);

    if (failures) {
        printf("test_agentrestrict: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_agentrestrict: all passed\n");
    return 0;
}
