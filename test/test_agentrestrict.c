/*
 * test_agentrestrict - regression tests for kitty/kitty_agentrestrict.c:
 * the restrict-destination-v00@openssh.com constraint (ssh-add -h) and
 * OpenSSH's checks on it, the walk of a connection's session-binds (local
 * use, a failed bind, the destination, the user, forwarding hops, the
 * hostbound method, a host certificate against a CA entry), the sign
 * check's userauth data, the rule store (ssh-add -h replacing a key's
 * links, forwarding allowed, tidying) and its bytes: a round trip, and a
 * file cut short, changed or of another kind refused as damaged with the
 * store left as it was. kageant's own extensions
 * (docs/PROTOCOL-AGENT-KAPPER.md): both in KNOWN_EXTENSIONS (the "query"
 * answer), session-bind-host-v00's data checked, the names of a host key
 * as shown (the client's first, five in all, "+N"), and Key details' lines
 * of a key's destinations. Pattern destinations (the rule editor's two
 * choices: fixed list, checked at each login; the dropped "Host CA only" that
 * matches no server), the key list's groups and order, the rule file's
 * format 3 and the formats 2 and 1 it still reads. The rule editor's limit
 * on a pattern host (kar_pattern_narrow_enough). "All servers of a host CA"
 * (KAR_MATCH_CA_STORE): the CA store read at login, its Valid hosts
 * expression with a port condition checked against the port the client
 * sent (session-bind-host-v00), never met without one, dropped for such a
 * client when the destination ignores the port (KAR_HOP_IGNORE_PORT, the
 * reason no-port when only the port was missing, the flag in the rule
 * file of format 4 and not in format 3), a certificate without names
 * refused unless the CA allows it (as in an ssh-add -h rule, OpenSSH
 * 10.3). A rule's origin (D9): stored, the old
 * formats without it, the texts, the comparison with known_hosts. OpenSSH's
 * known_hosts read only while kageant's setting is on (D8,
 * kitty_knownhosts.c): nothing opened with it off, names, look-ups, the
 * "(known_hosts)" mark and a match through such a name reported.
 * key-state-v00@kapper.net's data (every key, one blob, refused when cut
 * or followed by a byte), PuTTY's list-extended / reencrypt /
 * reencrypt-all kept from programs (kar_ext_local_only, add-ppk and query
 * not), and the reason of an empty key list (kar_nokey_reason: none
 * unbound, bind-failed, destination, forwarding-hop, store-damaged).
 * kageant's -h destinations (kar_dest_spec_parse, kar_dest_list_parse,
 * with the refusals), the startup entry's tokens (kar_entry_parse: the
 * forms as before, a dest= token left with the path as before) and a host
 * kept with its port (the rule editor): the store round trip, the port
 * split off for the look-up, the name match and a certificate.
 *
 * The keys are fixed ed25519 public keys and host certificates made once
 * with ssh-keygen: key web02 with principal web03 signed by the key "ca";
 * key web04 signed by "ca2" without names and with the names
 * web05.example.com and web06.example.org, both valid forever. Returns
 * non-zero on any failure.
 */
#ifdef _WIN32
#include <winsock2.h>   /* before windows.h: putty.h pulls it in further down */
#include <windows.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "putty.h"
#include "ssh.h"
#include "pageant.h"                    /* KNOWN_EXTENSIONS */
#include "kitty/kitty_agentrestrict.h"
#include "kitty/kitty_knownhosts.h"

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
/* D10/D12: the CA "ca2", the host key web04, its certificate without names
 * and the one for web05.example.com and web06.example.org */
static const char *const B64_CA2 =
    "AAAAC3NzaC1lZDI1NTE5AAAAIAySji2Ngt1Z0yGgoMSl/lW6MATXjGUx4N8E0qpJG1M5";
static const char *const B64_WEB04 =
    "AAAAC3NzaC1lZDI1NTE5AAAAIA5+tNpLbd/Au0Me89vlq1H17/tZdxqsio1mifT0IiEi";
static const char *const B64_CERT_NONAME =
    "AAAAIHNzaC1lZDI1NTE5LWNlcnQtdjAxQG9wZW5zc2guY29tAAAAINJi9ySJguMhLeLD"
    "JssuxHUtKP4mpjE1l894NhxOpW2gAAAAIA5+tNpLbd/Au0Me89vlq1H17/tZdxqsio1m"
    "ifT0IiEiAAAAAAAAAAAAAAACAAAACG5hbWVsZXNzAAAAAAAAAAAAAAAA//////////8A"
    "AAAAAAAAAAAAAAAAAAAzAAAAC3NzaC1lZDI1NTE5AAAAIAySji2Ngt1Z0yGgoMSl/lW6"
    "MATXjGUx4N8E0qpJG1M5AAAAUwAAAAtzc2gtZWQyNTUxOQAAAEDcGcR45/pCzWeEg2Dr"
    "3BxuoZrXsfiCUNaUUedfbJSSw64FdX3YeNRv4yNAOVx+CYEi18QEq0wfSQRmPIJJxVII";
static const char *const B64_CERT_NAMED =
    "AAAAIHNzaC1lZDI1NTE5LWNlcnQtdjAxQG9wZW5zc2guY29tAAAAIBJbPq5r47lTxGh1"
    "W6xw8CMpLsGGg8/rTNsOhw1Sak0MAAAAIA5+tNpLbd/Au0Me89vlq1H17/tZdxqsio1m"
    "ifT0IiEiAAAAAAAAAAAAAAACAAAABW5hbWVkAAAAKgAAABF3ZWIwNS5leGFtcGxlLmNv"
    "bQAAABF3ZWIwNi5leGFtcGxlLm9yZwAAAAAAAAAA//////////8AAAAAAAAAAAAAAAAA"
    "AAAzAAAAC3NzaC1lZDI1NTE5AAAAIAySji2Ngt1Z0yGgoMSl/lW6MATXjGUx4N8E0qpJ"
    "G1M5AAAAUwAAAAtzc2gtZWQyNTUxOQAAAEDA+16PahbT6kzqiZ+YJOh/W9I4uwUji+2l"
    "ts08DsqK6TjQTSmQ7BM6IP3Wx6AZITG/h6o9SILSV2kFYg40fuIA";

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

    memset(b, 0, sizeof(b));            /* no port sent */
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
    kar_store_set_key_rules(s, fp, "user key", ptrlen_from_strbuf(user), r,
                            NULL);
    expect("-h: the key is restricted", kar_store_restricted(s, fp), true);
    got = kar_store_rules(s, fp);
    expect("-h: two rules", got && got->n == 2, true);
    kar_rules_free(got);
    expect("-h: the key's row", kar_store_key(s, fp) != NULL, true);

    /* a re-add with -h replaces the links (decision D8) */
    kar_rules_free(r);
    r = kar_rules_new();
    add_rule(r, NULL, NULL, NULL, "web01", web01, false);
    kar_store_set_key_rules(s, fp, NULL, ptrlen_from_strbuf(user), r, NULL);
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
        put_stringz(body, "kageant-rules-9");
        put_uint32(body, 1);
        put_uint32(body, 0);
        put_uint32(body, 0);
        put_uint32(body, 0);
        h = ssh_hash_new(&ssh_sha256);
        put_datapl(h, ptrlen_from_strbuf(body));
        ssh_hash_final(h, hash);
        put_datapl(file, ptrlen_from_strbuf(body));
        put_string(file, hash, sizeof(hash));
        expect("decode: an unknown version (format 9) is damaged",
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

/* session-bind-host-v00's data: string host, uint32 port */
static bool bind_host(const char *host, size_t hlen, unsigned long port,
                      bool trailing, bool cut, char **out, const char **why)
{
    strbuf *sb = strbuf_new();
    BinarySource src[1];
    bool ok;
    put_stringpl(sb, make_ptrlen(host, hlen));
    put_uint32(sb, port);
    if (trailing)
        put_byte(sb, 0);
    BinarySource_BARE_INIT_PL(src, make_ptrlen(sb->s, sb->len - (cut ? 1 : 0)));
    ok = kar_parse_bind_host(src, out, why);
    strbuf_free(sb);
    return ok;
}

/* key-state-v00@kapper.net's data, PuTTY's extensions kept from programs,
 * and the reason of an empty key list */
static bool key_state(const unsigned char *data, size_t len, bool *one,
                      ptrlen *kblob, const char **why)
{
    BinarySource src[1];
    BinarySource_BARE_INIT_PL(src, make_ptrlen(data, len));
    return kar_parse_key_state(src, one, kblob, why);
}

static void test_keystate_local_nokey(void)
{
    static const unsigned char blob3[] = { 0, 0, 0, 3, 0xaa, 0xbb, 0xcc };
    static const unsigned char blob3x[] = { 0, 0, 0, 3, 0xaa, 0xbb, 0xcc, 0 };
    static const unsigned char cut[] = { 0, 0, 0, 3, 0xaa };
    bool one = true;
    ptrlen kb;
    const char *why = NULL;
    kar_bind b[2];

    expect("key-state: no data - every key",
           key_state(NULL, 0, &one, &kb, &why) && !one, true);
    expect("key-state: a blob - that key",
           key_state(blob3, sizeof(blob3), &one, &kb, &why) && one &&
           kb.len == 3 && ((const unsigned char *)kb.ptr)[0] == 0xaa,
           true);
    expect("key-state: a byte after the blob is refused",
           key_state(blob3x, sizeof(blob3x), &one, &kb, &why), false);
    expect_why("key-state: why for a trailing byte", why,
               "trailing data after the request");
    expect("key-state: a cut blob is refused",
           key_state(cut, sizeof(cut), &one, &kb, &why), false);
    expect_why("key-state: why for a cut blob", why,
               "unable to decode request");

    expect("local only: list-extended",
           kar_ext_local_only(PTRLEN_LITERAL(
               "list-extended@putty.projects.tartarus.org")), true);
    expect("local only: reencrypt",
           kar_ext_local_only(PTRLEN_LITERAL(
               "reencrypt@putty.projects.tartarus.org")), true);
    expect("local only: reencrypt-all",
           kar_ext_local_only(PTRLEN_LITERAL(
               "reencrypt-all@putty.projects.tartarus.org")), true);
    expect("local only: add-ppk goes to programs",
           kar_ext_local_only(PTRLEN_LITERAL(
               "add-ppk@putty.projects.tartarus.org")), false);
    expect("local only: query goes to programs",
           kar_ext_local_only(PTRLEN_LITERAL("query")), false);
    expect("local only: key-state-v00 goes to programs",
           kar_ext_local_only(PTRLEN_LITERAL(KAR_EXT_KEY_STATE)), false);

    memset(b, 0, sizeof(b));
    expect("no key: never bound - no report",
           kar_nokey_reason(false, b, 0, false) == NULL, true);
    expect("no key: never bound, rule file damaged - no report",
           kar_nokey_reason(true, b, 0, false) == NULL, true);
    expect_why("no key: a refused bind, none kept",
               kar_nokey_reason(false, b, 0, true), KAR_WHY_BIND_FAILED);
    expect_why("no key: bound for a login",
               kar_nokey_reason(false, b, 1, false), KAR_WHY_DESTINATION);
    expect_why("no key: bound for a login, a later bind refused",
               kar_nokey_reason(false, b, 1, true), KAR_WHY_DESTINATION);
    b[1].forwarding = true;
    expect_why("no key: the last binding for forwarding",
               kar_nokey_reason(false, b, 2, false), KAR_WHY_FWD_HOP);
    expect_why("no key: rule file damaged on a bound connection",
               kar_nokey_reason(true, b, 2, false), KAR_WHY_STORE_DAMAGED);
    expect_why("no key: rule file damaged after a refused bind",
               kar_nokey_reason(true, b, 0, true), KAR_WHY_STORE_DAMAGED);
}

static void test_ext(strbuf *user, strbuf *web01, strbuf *web02,
                     strbuf *jump01)
{
#define TEST_EXT_NAME(id, name) name,
    static const char *const known[] = { KNOWN_EXTENSIONS(TEST_EXT_NAME) };
#undef TEST_EXT_NAME
    bool wk = false, bh = false, ks = false;
    char *out = NULL, *t;
    const char *why = NULL;
    char *longhost = snewn(256, char);

    /* the "query" answer lists every name of KNOWN_EXTENSIONS (to a
     * program all but the ones kar_ext_local_only keeps from it) */
    for (size_t i = 0; i < lenof(known); i++) {
        if (!strcmp(known[i], KAR_EXT_WITHHELD))
            wk = true;
        if (!strcmp(known[i], KAR_EXT_BIND_HOST))
            bh = true;
        if (!strcmp(known[i], KAR_EXT_KEY_STATE))
            ks = true;
    }
    expect("query lists withheld-keys-v00@kapper.net", wk, true);
    expect("query lists session-bind-host-v00@kapper.net", bh, true);
    expect("query lists key-state-v00@kapper.net", ks, true);
    {
        /* kageant -l and -R: rules-list-v00 / rules-remove-v00 */
        bool rl = false, rr = false;
        for (size_t i = 0; i < lenof(known); i++) {
            if (!strcmp(known[i], "rules-list-v00@kapper.net"))
                rl = true;
            if (!strcmp(known[i], "rules-remove-v00@kapper.net"))
                rr = true;
        }
        expect("query lists rules-list-v00@kapper.net", rl, true);
        expect("query lists rules-remove-v00@kapper.net", rr, true);
    }
    expect("the names carry their version",
           !strcmp(KAR_EXT_WITHHELD, "withheld-keys-v00@kapper.net") &&
           !strcmp(KAR_EXT_BIND_HOST, "session-bind-host-v00@kapper.net") &&
           !strcmp(KAR_EXT_KEY_STATE, "key-state-v00@kapper.net"),
           true);

    /* session-bind-host-v00's data */
    expect("bind-host: web01 port 22",
           bind_host("web01", 5, 22, false, false, &out, &why) &&
           !strcmp(out, "web01"), true);
    sfree(out);
    out = NULL;
    expect("bind-host: web01 port 2222 is web01:2222",
           bind_host("web01", 5, 2222, false, false, &out, &why) &&
           !strcmp(out, "web01:2222"), true);
    sfree(out);
    out = NULL;
    expect("bind-host: an ESC in the name is refused",
           bind_host("web\x1b" "01", 6, 22, false, false, &out, &why), false);
    expect("bind-host: no host on a refusal", out == NULL, true);
    expect_why("bind-host: why for a control character", why,
               "a control character in the host name");
    expect("bind-host: a newline is refused",
           bind_host("web01\n", 6, 22, false, false, &out, &why), false);
    expect("bind-host: DEL is refused",
           bind_host("web\x7f", 4, 22, false, false, &out, &why), false);
    expect("bind-host: an empty name is refused",
           bind_host("", 0, 22, false, false, &out, &why), false);
    memset(longhost, 'a', 256);
    expect("bind-host: 255 bytes are taken",
           bind_host(longhost, 255, 22, false, false, &out, &why) &&
           strlen(out) == 255, true);
    sfree(out);
    out = NULL;
    expect("bind-host: 256 bytes are refused",
           bind_host(longhost, 256, 22, false, false, &out, &why), false);
    sfree(longhost);
    expect("bind-host: port 0 is refused",
           bind_host("web01", 5, 0, false, false, &out, &why), false);
    expect("bind-host: port 65536 is refused",
           bind_host("web01", 5, 65536, false, false, &out, &why), false);
    expect("bind-host: a trailing byte is refused",
           bind_host("web01", 5, 22, true, false, &out, &why), false);
    expect_why("bind-host: why for a trailing byte", why,
               "trailing data after the request");
    expect("bind-host: a short message is refused",
           bind_host("web01", 5, 22, false, true, &out, &why), false);

    /* the names of a host key as shown */
    {
        static const char *const five_src[] = {
            "10.0.0.1", "WEB01", "b.example", "c.example", "d.example",
            "e.example", "f.example" };
        char *five[lenof(five_src)], *one[1];
        for (size_t i = 0; i < lenof(five_src); i++)
            five[i] = dupstr(five_src[i]);
        one[0] = dupstr("web01");
        t = kar_dest_names_text("web01", five, lenof(five));
        expect("names: the client's first, the same name once, five in all",
               t && !strcmp(t, "web01 (client) 10.0.0.1 b.example c.example "
                            "d.example +2"), true);
        sfree(t);
        t = kar_dest_names_text(NULL, five, lenof(five));
        expect("names: without the client, five names then +2",
               t && !strcmp(t, "10.0.0.1 WEB01 b.example c.example "
                            "d.example +2"), true);
        sfree(t);
        t = kar_dest_names_text("web01", NULL, 0);
        expect("names: the client's alone", t && !strcmp(t, "web01 (client)"),
               true);
        sfree(t);
        t = kar_dest_names_text("web01", one, 1);
        expect("names: a lookup name equal to the client's is not repeated",
               t && !strcmp(t, "web01 (client)"), true);
        sfree(t);
        t = kar_dest_names_text(NULL, one, 1);
        expect("names: one lookup name, no +N", t && !strcmp(t, "web01"),
               true);
        sfree(t);
        expect("names: none at all is NULL",
               kar_dest_names_text(NULL, NULL, 0) == NULL, true);
        for (size_t i = 0; i < lenof(five); i++)
            sfree(five[i]);
        sfree(one[0]);
    }

    /* Key details' lines */
    {
        kar_store *s = kar_store_new();
        char *fp = kar_key_fp(ptrlen_from_strbuf(user));
        size_t n = 99;
        kar_rule d;
        unsigned id;

        expect("lines: a key without links has none",
               kar_store_dest_lines(s, fp, &n) == NULL && n == 0, true);
        memset(&d, 0, sizeof(d));
        d.to.host = dupstr("web01.example.com");
        d.to.user = dupstr("deploy");
        d.from.host = dupstr("jump.example.com");
        kar_hop_add_key(&d.from, ptrlen_from_strbuf(jump01), false);
        kar_hop_add_key(&d.to, ptrlen_from_strbuf(web01), false);
        id = kar_store_dest_for(s, &d);
        kar_hop_free(&d.from);
        kar_hop_free(&d.to);
        kar_store_link(s, fp, id, KAR_LINK_FORWARD);
        memset(&d, 0, sizeof(d));
        d.to.host = dupstr("web02");
        kar_hop_add_key(&d.to, ptrlen_from_strbuf(web02), false);
        id = kar_store_dest_for(s, &d);
        kar_hop_free(&d.to);
        kar_store_link(s, fp, id, 0);
        t = kar_store_dest_lines(s, fp, &n);
        expect("lines: two destinations", n == 2, true);
        expect("lines: host, user, via, forwarding; then the plain one",
               t && !strcmp(t, "web01.example.com  user deploy  via "
                            "jump.example.com  forwarding\r\n"
                            "    Origin not recorded\r\n"
                            "web02\r\n    Origin not recorded"), true);
        sfree(t);
        sfree(fp);
        kar_store_free(s);
    }
}

/* kar_hostkey_names for the tests: the names "stored" for one host key;
 * fake_kh: fake_name is read from known_hosts; fake_name2 a second name
 * from the KiTTY++ store */
static ptrlen fake_key;
static const char *fake_name = NULL, *fake_name2 = NULL;
static bool fake_kh = false;
static size_t fake_names(ptrlen hostkey, char ***names, bool **kh)
{
    size_t n = 0;
    *names = NULL;
    *kh = NULL;
    if (!fake_name || !ptrlen_eq_ptrlen(hostkey, fake_key))
        return 0;
    *names = snewn(3, char *);
    *kh = snewn(3, bool);
    (*names)[n] = dupstr("10.0.0.7");
    (*kh)[n++] = false;
    (*names)[n] = dupstr(fake_name);
    (*kh)[n++] = fake_kh;
    if (fake_name2) {
        (*names)[n] = dupstr(fake_name2);
        (*kh)[n++] = false;
    }
    return n;
}

/* One destination from this computer: host, how it is matched, one key
 * (NULL for none). */
static kar_rules *one_rule(const char *host, unsigned match, strbuf *key,
                           bool is_ca)
{
    kar_rules *r = kar_rules_new();
    kar_rule d;
    memset(&d, 0, sizeof(d));
    d.to.host = dupstr(host);
    d.to.match = match;
    if (key)
        kar_hop_add_key(&d.to, ptrlen_from_strbuf(key), is_ca);
    kar_rules_add(r, &d);
    kar_hop_free(&d.to);
    return r;
}

static bool permitted_on(const kar_rules *r, strbuf *hostkey, bool fwd)
{
    kar_bind b;
    const char *why;
    memset(&b, 0, sizeof(b));           /* no port sent */
    b.hostkey = ptrlen_from_strbuf(hostkey);
    b.sid = PTRLEN_LITERAL("sid");
    b.forwarding = fwd;
    return kar_permitted(r, &b, 1, true, NULL, &why);
}

static int group_on(const kar_rules *r, strbuf *hostkey, bool fwd)
{
    kar_bind b;
    memset(&b, 0, sizeof(b));
    b.hostkey = ptrlen_from_strbuf(hostkey);
    b.sid = PTRLEN_LITERAL("sid");
    b.forwarding = fwd;
    return kar_list_group(r, &b, 1);
}

/* The rule editor's limit on a pattern host: two fixed labels at the end,
 * the wildcard in a label before them. */
static void test_pattern_limit(void)
{
    static const struct { const char *host; bool ok; } cases[] = {
        {"*.customer.com", true},
        {"web*.customer.com", true},
        {"*.a.customer.com", true},
        {"*.customer.com:2222", true},
        {"web01.customer.com", true},         /* not a pattern */
        {"*", false},
        {"*.*", false},
        {"*.com", false},
        {"?*", false},
        {"*customer.com", false},
        {"customer.*", false},
        {"*.customer.*", false},
        {"*.cust?mer.com", false},
        {"*..com", false},
        {"*.com:2222", false},
    };
    for (size_t i = 0; i < lenof(cases); i++) {
        char *what = dupprintf("pattern limit: %s %s", cases[i].host,
                               cases[i].ok ? "passes" : "is refused");
        expect(what, kar_pattern_narrow_enough(cases[i].host), cases[i].ok);
        sfree(what);
    }
}

static void test_patterns(strbuf *user, strbuf *web01, strbuf *web02,
                          strbuf *ca, strbuf *cert)
{
    kar_rules *r;

    kar_hostkey_names = fake_names;
    fake_key = ptrlen_from_strbuf(web01);

    /* 3: checked at each login - the names stored for the bound key */
    r = one_rule("*.customer.com", KAR_MATCH_STORED_LATER, NULL, false);
    fake_name = "WEB07.Customer.com:2222";
    expect("later: a key stored under a matching name (case, port ignored)",
           permitted_on(r, web01, false), true);
    expect("later: its group is the pattern group",
           group_on(r, web01, false) == KAR_GROUP_PATTERN, true);
    fake_name = "web07.customer.com.evil.net";
    expect("later: a name the pattern does not cover is refused",
           permitted_on(r, web01, false), false);
    fake_name = "a.b.customer.com";
    expect("later: * covers dots too", permitted_on(r, web01, false), true);
    expect("later: another key, no name stored for it, is refused",
           permitted_on(r, web02, false), false);
    kar_hostkey_names = NULL;
    expect("later: without the names lookup nothing matches",
           permitted_on(r, web01, false), false);
    kar_hostkey_names = fake_names;
    kar_rules_free(r);

    /* 2: fixed list - the snapshot only, never the names */
    r = one_rule("*.customer.com", KAR_MATCH_STORED_NOW, web02, false);
    fake_name = "web08.customer.com";
    expect("now: a key stored later under a matching name is refused",
           permitted_on(r, web01, false), false);
    expect("now: the key of the snapshot is permitted",
           permitted_on(r, web02, false), true);
    expect("now: its group is the pattern group",
           group_on(r, web02, false) == KAR_GROUP_PATTERN, true);
    kar_rules_free(r);

    /* 1: the dropped "Host CA only" matches no server, not even the
     * certificate it was made for */
    r = one_rule("WEB0?", KAR_MATCH_CA_DROPPED, ca, true);
    expect("dropped Host CA only: a certificate it once matched is refused",
           permitted_on(r, cert, false), false);
    expect("dropped Host CA only: no group",
           group_on(r, cert, false) == KAR_GROUP_NONE, true);
    kar_rules_free(r);
    r = one_rule("*.customer.com", KAR_MATCH_CA_DROPPED, web01, false);
    expect("dropped Host CA only: a plain key in it is refused",
           permitted_on(r, web01, false), false);
    kar_rules_free(r);

    /* OpenSSH's own (ssh-add -h): the exact group */
    r = one_rule("web03", KAR_MATCH_EXACT, ca, true);
    expect("exact: a CA certificate for the name as written",
           group_on(r, cert, false) == KAR_GROUP_EXACT, true);
    kar_rules_free(r);
    r = one_rule("web01", KAR_MATCH_EXACT, web01, false);
    expect("exact: a plain key", group_on(r, web01, false) ==
           KAR_GROUP_EXACT, true);
    expect("group: a binding for forwarding has no group",
           group_on(r, web01, true) == KAR_GROUP_NONE, true);
    expect("group: another server has no group",
           group_on(r, web02, false) == KAR_GROUP_NONE, true);
    {
        /* an exact rule and a pattern rule for the same server: exact */
        kar_rules *p = one_rule("*.customer.com", KAR_MATCH_STORED_LATER,
                                NULL, false);
        kar_rules_add(r, &p->v[0]);
        fake_name = "web07.customer.com";
        expect("group: exact wins over a pattern for the same server",
               group_on(r, web01, false) == KAR_GROUP_EXACT, true);
        kar_rules_free(p);
    }
    kar_rules_free(r);

    /* the order of the list */
    {
        static const int g[] = { 3, 1, 2, 1, 3, 2 };
        static const size_t want[] = { 1, 3, 2, 5, 0, 4 };
        size_t o[6];
        kar_list_order(g, 6, o);
        expect("order: exact, pattern, unrestricted, each as given",
               !memcmp(o, want, sizeof(o)), true);
    }

    /* the file: format 2 keeps the choice, format 1 still loads */
    {
        kar_store *s = kar_store_new(), *t = kar_store_new();
        char *fp = kar_key_fp(ptrlen_from_strbuf(user));
        const char *err = NULL;
        strbuf *bytes = strbuf_new();
        size_t n;
        char *lines;
        unsigned id;

        r = one_rule("*.customer.com", KAR_MATCH_STORED_LATER, NULL, false);
        id = kar_store_dest_for(s, &r->v[0]);
        kar_store_note_key(s, fp, "user key", ptrlen_from_strbuf(user));
        kar_store_link(s, fp, id, 0);
        kar_rules_free(r);
        kar_store_encode(s, bytes);
        expect("format 2: a pattern destination without keys loads",
               kar_store_decode(t, ptrlen_from_strbuf(bytes), &err), true);
        expect("format 2: its choice is kept",
               t->ndest == 1 && t->dest[0].rule.to.match ==
               KAR_MATCH_STORED_LATER, true);
        lines = kar_store_dest_lines(t, fp, &n);
        expect("lines: the choice is shown",
               lines && !strcmp(lines, "*.customer.com  checked at each login"
                                "\r\n    Origin not recorded"),
               true);
        sfree(lines);
        strbuf_clear(bytes);
        kar_store_encode_format(s, bytes, 2);
        expect("format 2: still loads",
               kar_store_decode(t, ptrlen_from_strbuf(bytes), &err), true);
        strbuf_clear(bytes);
        kar_store_encode_format(s, bytes, 1);
        expect("format 1: a destination without keys is damaged there",
               kar_store_decode(t, ptrlen_from_strbuf(bytes), &err), false);
        kar_store_free(s);
        s = kar_store_new();
        r = one_rule("web01", KAR_MATCH_EXACT, web01, false);
        id = kar_store_dest_for(s, &r->v[0]);
        kar_store_note_key(s, fp, "user key", ptrlen_from_strbuf(user));
        kar_store_link(s, fp, id, 0);
        kar_rules_free(r);
        strbuf_clear(bytes);
        kar_store_encode_format(s, bytes, 1);
        expect("format 1: an older file still loads",
               kar_store_decode(t, ptrlen_from_strbuf(bytes), &err), true);
        expect("format 1: its destinations are exact",
               t->ndest == 1 && t->dest[0].rule.to.match == KAR_MATCH_EXACT,
               true);
        r = one_rule("web01", KAR_MATCH_EXACT, NULL, false);
        kar_store_free(s);
        s = kar_store_new();
        id = kar_store_dest_for(s, &r->v[0]);
        kar_store_note_key(s, fp, "user key", ptrlen_from_strbuf(user));
        kar_store_link(s, fp, id, 0);
        kar_rules_free(r);
        strbuf_clear(bytes);
        kar_store_encode(s, bytes);
        expect("format 3: an exact destination without keys is damaged",
               kar_store_decode(t, ptrlen_from_strbuf(bytes), &err), false);
        strbuf_free(bytes);
        sfree(fp);
        kar_store_free(s);
        kar_store_free(t);
    }
    kar_hostkey_names = NULL;
    fake_name = NULL;
}

/* kar_ca_lookup for the tests: one CA, "qa-ca", with the key, expression
 * and exception set below; fake_ca_none: the store holds no CA */
static strbuf *fake_ca_key;
static const char *fake_ca_expr = "*.example.com";
static bool fake_ca_nameless = false, fake_ca_none = false;
static bool fake_ca_lookup(const char *name, strbuf *pubkey, char **expr,
                           bool *nameless)
{
    *expr = NULL;
    *nameless = false;
    if (fake_ca_none || strcmp(name, "qa-ca"))
        return false;
    put_datapl(pubkey, ptrlen_from_strbuf(fake_ca_key));
    *expr = dupstr(fake_ca_expr);
    *nameless = fake_ca_nameless;
    return true;
}

/* One login bound to hostkey, the client's port sent with
 * session-bind-host-v00 (0 = none): permitted? *why on a refusal. */
static bool permitted_port(const kar_rules *r, strbuf *hostkey,
                           unsigned port, const char **why)
{
    kar_bind b;
    memset(&b, 0, sizeof(b));
    b.hostkey = ptrlen_from_strbuf(hostkey);
    b.sid = PTRLEN_LITERAL("sid");
    b.port = port;
    *why = NULL;
    return kar_permitted(r, &b, 1, true, NULL, why);
}

/* A CA destination's port conditions: the client's port, none sent (never
 * met), none sent with KAR_HOP_IGNORE_PORT (dropped). The certificate
 * "named" holds web05.example.com and web06.example.org. */
static void test_ca_ports(strbuf *user, strbuf *ca2, strbuf *named)
{
    static const struct {
        const char *expr;
        unsigned port;                  /* 0 = none sent */
        bool ignore;                    /* KAR_HOP_IGNORE_PORT */
        bool ok;
        const char *why;                /* the reason of a refusal */
    } cases[] = {
        /* the client's port decides a port condition */
        {"*.example.com && port:2222", 2222, false, true, NULL},
        {"*.example.com && port:2222", 22, false, false, KAR_WHY_DESTINATION},
        {"*.example.com && port:2222", 22, true, false, KAR_WHY_DESTINATION},
        {"*.example.com && !port:22", 2222, false, true, NULL},
        {"*.example.com && !port:22", 22, false, false, KAR_WHY_DESTINATION},
        {"port:2222", 2222, false, true, NULL},
        {"*.example.net || port:22", 22, false, true, NULL},
        {"*.example.net || port:22", 23, false, false, KAR_WHY_DESTINATION},
        /* no port sent, the flag off: a port condition is never met */
        {"*.example.com && port:2222", 0, false, false, KAR_WHY_NO_PORT},
        {"*.example.com && !port:22", 0, false, false, KAR_WHY_NO_PORT},
        {"!port:22", 0, false, false, KAR_WHY_DESTINATION},
        {"port:2222", 0, false, false, KAR_WHY_DESTINATION},
        {"*.example.com || port:22", 0, false, true, NULL},
        {"*.example.net && port:22", 0, false, false, KAR_WHY_DESTINATION},
        /* no port sent, the flag on: the port conditions dropped */
        {"*.example.com && port:2222", 0, true, true, NULL},
        {"*.example.com || port:22", 0, true, true, NULL},
        {"*.example.com && !port:22", 0, true, true, NULL},
        {"*.example.net || port:22", 0, true, false, KAR_WHY_DESTINATION},
        {"*.example.net || !port:22", 0, true, false, KAR_WHY_DESTINATION},
        {"!port:22", 0, true, false, KAR_WHY_DESTINATION},
        {"port:22 && port:2222", 0, true, false, KAR_WHY_DESTINATION},
        /* dropped: !*.example.com - web06.example.org passes */
        {"!(*.example.com && port:22)", 0, true, true, NULL},
        /* Valid hosts without a port condition: as before */
        {"*.example.com", 0, false, true, NULL},
        {"*.example.com", 2222, false, true, NULL},
        {"*.example.com", 0, true, true, NULL},
        {"*.example.net", 0, false, false, KAR_WHY_DESTINATION},
    };
    kar_rules *r;
    const char *why;

    kar_ca_lookup = fake_ca_lookup;
    fake_ca_key = ca2;
    fake_ca_nameless = false;
    fake_ca_none = false;
    for (size_t i = 0; i < lenof(cases); i++) {
        char *what = dupprintf("CA port: \"%s\", port %u%s: %s",
                               cases[i].expr, cases[i].port,
                               cases[i].ignore ? ", port ignored" : "",
                               cases[i].ok ? "admitted" : "refused");
        r = one_rule("qa-ca", KAR_MATCH_CA_STORE, NULL, false);
        if (cases[i].ignore)
            r->v[0].to.flags = KAR_HOP_IGNORE_PORT;
        fake_ca_expr = cases[i].expr;
        expect(what, permitted_port(r, named, cases[i].port, &why),
               cases[i].ok);
        if (!cases[i].ok)
            expect_why(what, why, cases[i].why);
        sfree(what);
        kar_rules_free(r);
    }

    /* the reason on the sign path, the flag after a forwarding hop */
    {
        kar_bind b;
        strbuf *data;
        ptrlen ukey = ptrlen_from_strbuf(user);
        r = one_rule("qa-ca", KAR_MATCH_CA_STORE, NULL, false);
        fake_ca_expr = "*.example.com && port:2222";
        memset(&b, 0, sizeof(b));
        b.hostkey = ptrlen_from_strbuf(named);
        b.sid = PTRLEN_LITERAL("sid");
        data = userauth(PTRLEN_LITERAL("sid"), "alice", "publickey", ukey,
                        make_ptrlen(NULL, 0));
        expect("CA port: no port sent - no signature",
               kar_sign_permitted(r, &b, 1, true, ukey,
                                  ptrlen_from_strbuf(data), &why), false);
        expect_why("CA port: no port sent - signing: the reason", why,
                   KAR_WHY_NO_PORT);
        b.port = 2222;
        expect("CA port: port 2222 sent - signed",
               kar_sign_permitted(r, &b, 1, true, ukey,
                                  ptrlen_from_strbuf(data), &why), true);
        strbuf_free(data);
        kar_rules_free(r);
    }

    /* the port of session-bind-host-v00 as the binding keeps it */
    {
        strbuf *sb = strbuf_new();
        BinarySource src[1];
        char *out = NULL;
        unsigned port = 99;
        put_stringz(sb, "web05.example.com");
        put_uint32(sb, 2222);
        BinarySource_BARE_INIT_PL(src, ptrlen_from_strbuf(sb));
        expect("bind host: the port is returned",
               kar_parse_bind_host_port(src, &out, &port, &why) &&
               port == 2222 && out && !strcmp(out, "web05.example.com:2222"),
               true);
        sfree(out);
        strbuf_free(sb);
    }

    /* the rule file: format 4 keeps the flag, format 3 loads it off */
    {
        kar_store *s = kar_store_new(), *t = kar_store_new();
        char *fp = kar_key_fp(ptrlen_from_strbuf(user)), *lines;
        const char *err;
        strbuf *bytes = strbuf_new();
        unsigned id, id_off;
        size_t n;
        fake_ca_expr = "*.example.com && port:2222";
        r = one_rule("qa-ca", KAR_MATCH_CA_STORE, NULL, false);
        id_off = kar_store_dest_for(s, &r->v[0]);
        r->v[0].to.flags = KAR_HOP_IGNORE_PORT;
        id = kar_store_dest_for(s, &r->v[0]);
        expect("format 4: the flag makes another destination",
               id && id_off && id != id_off && s->ndest == 2, true);
        kar_rules_free(r);
        kar_store_note_key(s, fp, "user key", ptrlen_from_strbuf(user));
        kar_store_link(s, fp, id, 0);
        kar_store_tidy(s);
        kar_store_encode(s, bytes);
        expect("format 4: written by kar_store_encode",
               bytes->len > 19 && !memcmp(bytes->s + 4, "kageant-rules-4", 15),
               true);
        expect("format 4: a CA destination with the flag loads",
               kar_store_decode(t, ptrlen_from_strbuf(bytes), &err), true);
        expect("format 4: the flag is kept",
               t->ndest == 1 &&
               t->dest[0].rule.to.match == KAR_MATCH_CA_STORE &&
               t->dest[0].rule.to.flags == KAR_HOP_IGNORE_PORT, true);
        r = kar_store_rules(t, fp);
        expect("format 4: no port sent, the port ignored - admitted",
               r && permitted_port(r, named, 0, &why), true);
        kar_rules_free(r);
        lines = kar_store_dest_lines(t, fp, &n);
        expect("lines: CA: name (Valid hosts: expression), port ignored",
               lines && !strncmp(lines, "CA: qa-ca (Valid hosts: *.example.com"
                                 " && port:2222), port ignored for clients "
                                 "that send none\r\n", 94), true);
        sfree(lines);
        strbuf_clear(bytes);
        kar_store_encode_format(s, bytes, 3);
        expect("format 3: loads", kar_store_decode(t, ptrlen_from_strbuf(bytes),
                                                  &err), true);
        expect("format 3: the flag is off",
               t->ndest == 1 && t->dest[0].rule.to.flags == 0, true);
        r = kar_store_rules(t, fp);
        expect("format 3: no port sent - refused, reason no-port",
               r && !permitted_port(r, named, 0, &why) && why &&
               !strcmp(why, KAR_WHY_NO_PORT), true);
        kar_rules_free(r);
        lines = kar_store_dest_lines(t, fp, &n);
        expect("lines: without the flag no port text",
               lines && !strstr(lines, "port ignored"), true);
        sfree(lines);
        /* an unknown flag bit, the flag on another kind: damaged */
        s->dest[0].rule.to.flags = 2;
        strbuf_clear(bytes);
        kar_store_encode(s, bytes);
        expect("format 4: an unknown flag bit is damaged",
               kar_store_decode(t, ptrlen_from_strbuf(bytes), &err), false);
        s->dest[0].rule.to.flags = KAR_HOP_IGNORE_PORT;
        s->dest[0].rule.to.match = KAR_MATCH_STORED_LATER;
        strbuf_clear(bytes);
        kar_store_encode(s, bytes);
        expect("format 4: the flag on another kind is damaged",
               kar_store_decode(t, ptrlen_from_strbuf(bytes), &err), false);
        strbuf_free(bytes);
        sfree(fp);
        kar_store_free(s);
        kar_store_free(t);
    }
    fake_ca_expr = "*.example.com";
    kar_ca_lookup = NULL;
}

/* D10: "All servers of a host CA", D12: certificates without names */
static void test_ca_store(strbuf *user, strbuf *ca, strbuf *ca2,
                          strbuf *web04, strbuf *noname, strbuf *named)
{
    static const struct {
        const char *expr;
        bool ok;
    } exprs[] = {
        {"*.example.com", true},              /* web05.example.com */
        {"*.example.org", true},              /* web06.example.org */
        {"*.example.net", false},             /* no name admitted */
        {"port:22", false},
        {"*.example.com && port:22", false},  /* a port is never met */
        {"*.example.com || port:22", true},
        {"!port:22", false},
        {"*.example.com && !port:22", false}, /* not even negated */
        {"*.example.net || !port:22", false},
        {"web05.example.com", true},
        {"*.example.com &&", false},          /* no expression at all */
    };
    kar_rules *r = one_rule("qa-ca", KAR_MATCH_CA_STORE, NULL, false);

    kar_ca_lookup = fake_ca_lookup;
    fake_ca_key = ca2;
    fake_ca_nameless = false;
    fake_ca_none = false;
    for (size_t i = 0; i < lenof(exprs); i++) {
        char *what = dupprintf("CA store: Valid hosts \"%s\" %s", exprs[i].expr,
                               exprs[i].ok ? "admits the certificate" :
                                             "refuses it");
        fake_ca_expr = exprs[i].expr;
        expect(what, permitted_on(r, named, false), exprs[i].ok);
        sfree(what);
    }
    fake_ca_expr = "*.example.com";
    expect("CA store: its group is the pattern group",
           group_on(r, named, false) == KAR_GROUP_PATTERN, true);
    expect("CA store: the plain host key of the certificate is refused",
           permitted_on(r, web04, false), false);
    fake_ca_key = ca;
    expect("CA store: another key under the CA's name is refused",
           permitted_on(r, named, false), false);
    fake_ca_key = ca2;
    fake_ca_none = true;
    expect("CA store: a CA no longer in the store is refused",
           permitted_on(r, named, false), false);
    fake_ca_none = false;
    expect("CA store: no certificate without names (OpenSSH 10.3)",
           permitted_on(r, noname, false), false);
    fake_ca_nameless = true;
    expect("CA store: a certificate without names, the CA allows it",
           permitted_on(r, noname, false), true);
    fake_ca_nameless = false;
    kar_ca_lookup = NULL;
    expect("CA store: without the CA store nothing matches",
           permitted_on(r, named, false), false);
    kar_ca_lookup = fake_ca_lookup;
    kar_rules_free(r);

    /* D12 in an ssh-add -h rule: a CA entry and the name as written */
    r = one_rule("web05.example.com", KAR_MATCH_EXACT, ca2, true);
    expect("-h: a certificate with the name, from the CA",
           permitted_on(r, named, false), true);
    expect("-h: a certificate without names is valid for no host",
           permitted_on(r, noname, false), false);
    kar_rules_free(r);

    /* texts: Key details, the agent log */
    {
        kar_store *s = kar_store_new(), *t = kar_store_new();
        char *fp = kar_key_fp(ptrlen_from_strbuf(user)), *lines, *txt;
        const char *err;
        strbuf *bytes = strbuf_new();
        size_t n;
        unsigned id;
        r = one_rule("qa-ca", KAR_MATCH_CA_STORE, NULL, false);
        txt = kar_rules_text(r);
        expect("text: a CA destination in the agent log",
               !strcmp(txt, "CA: qa-ca"), true);
        sfree(txt);
        id = kar_store_dest_for(s, &r->v[0]);
        kar_rules_free(r);
        kar_store_note_key(s, fp, "user key", ptrlen_from_strbuf(user));
        kar_store_link(s, fp, id, 0);
        lines = kar_store_dest_lines(s, fp, &n);
        expect("lines: CA: name (Valid hosts: expression)",
               lines && !strncmp(lines, "CA: qa-ca (Valid hosts: "
                                 "*.example.com)\r\n", 40), true);
        sfree(lines);
        fake_ca_none = true;
        lines = kar_store_dest_lines(s, fp, &n);
        expect("lines: a CA gone from the store",
               lines && !strncmp(lines, "CA: qa-ca (not in the CA store: "
                                 "matches no server)", 50), true);
        sfree(lines);
        fake_ca_none = false;
        kar_store_encode(s, bytes);
        expect("format 3: a CA destination without keys loads",
               kar_store_decode(t, ptrlen_from_strbuf(bytes), &err), true);
        expect("format 3: its kind is kept",
               t->ndest == 1 &&
               t->dest[0].rule.to.match == KAR_MATCH_CA_STORE &&
               !strcmp(t->dest[0].rule.to.host, "qa-ca"), true);
        strbuf_clear(bytes);
        kar_store_encode_format(s, bytes, 2);
        expect("format 2 cannot hold a CA destination: damaged",
               kar_store_decode(t, ptrlen_from_strbuf(bytes), &err), false);
        /* an old rule file with "Host CA only": it loads, matches nothing,
         * Key details marks it */
        kar_store_free(s);
        s = kar_store_new();
        r = one_rule("*.example.com", KAR_MATCH_CA_DROPPED, ca2, true);
        id = kar_store_dest_for(s, &r->v[0]);
        kar_rules_free(r);
        kar_store_note_key(s, fp, "user key", ptrlen_from_strbuf(user));
        kar_store_link(s, fp, id, 0);
        strbuf_clear(bytes);
        kar_store_encode_format(s, bytes, 2);
        expect("format 2 with Host CA only: loads",
               kar_store_decode(t, ptrlen_from_strbuf(bytes), &err), true);
        expect("format 2 with Host CA only: kept as the dropped choice",
               t->ndest == 1 &&
               t->dest[0].rule.to.match == KAR_MATCH_CA_DROPPED, true);
        r = kar_store_rules(t, fp);
        expect("format 2 with Host CA only: matches no server",
               r && !permitted_on(r, named, false), true);
        kar_rules_free(r);
        lines = kar_store_dest_lines(t, fp, &n);
        expect("lines: the dropped choice is marked",
               lines && strstr(lines, "Host CA only: no longer offered, "
                               "matches no server") != NULL, true);
        sfree(lines);
        strbuf_free(bytes);
        sfree(fp);
        kar_store_free(s);
        kar_store_free(t);
    }
    kar_ca_lookup = NULL;
}

/* The temporary known_hosts of the D8 tests */
static char *kh_path = NULL;
static bool kh_on = false;
static char *kh_hook(void)
{
    return kh_on ? dupstr(kh_path) : NULL;
}
static void kh_name_add(void *ctx, const char *name)
{
    strbuf *sb = (strbuf *)ctx;
    put_fmt(sb, "%s;", name);
}
static void kh_key_add(void *ctx, ptrlen blob, bool is_ca)
{
    strbuf *sb = (strbuf *)ctx;
    char *fp = ssh2_fingerprint_blob(blob, SSH_FPTYPE_SHA256);
    put_fmt(sb, "%s%s;", is_ca ? "CA " : "", fp);
    sfree(fp);
}
static char *kh_fp(strbuf *blob, bool is_ca)
{
    char *fp = ssh2_fingerprint_blob(ptrlen_from_strbuf(blob),
                                     SSH_FPTYPE_SHA256);
    char *out = dupprintf("%s%s;", is_ca ? "CA " : "", fp);
    sfree(fp);
    return out;
}

/* D8: known_hosts read only with the setting on; D9: the origin texts */
static void test_known_hosts(strbuf *user, strbuf *web01, strbuf *web02,
                             strbuf *ca2)
{
    const char *tmp = getenv("TEMP");
    strbuf *b64_01, *b64_02, *b64_ca, *salt, *hashed, *got = strbuf_new();
    unsigned char mac[20];
    ssh2_mac *m;
    char *want, *t;
    unsigned long opened;
    FILE *fp;

    kh_path = dupprintf("%s%sqa_kar_known_hosts_%lu", tmp ? tmp : ".",
                        tmp ? "\\" : "/", (unsigned long)time(NULL));
    b64_01 = base64_encode_sb(ptrlen_from_strbuf(web01), 0);
    b64_02 = base64_encode_sb(ptrlen_from_strbuf(web02), 0);
    b64_ca = base64_encode_sb(ptrlen_from_strbuf(ca2), 0);
    /* a hashed entry for hashed.example.com, salt 20 x 0x11 */
    salt = strbuf_new();
    for (int i = 0; i < 20; i++)
        put_byte(salt, 0x11);
    m = ssh2_mac_new(&ssh_hmac_sha1, NULL);
    ssh2_mac_setkey(m, ptrlen_from_strbuf(salt));
    ssh2_mac_start(m);
    put_dataz(m, "hashed.example.com");
    ssh2_mac_genresult(m, mac);
    ssh2_mac_free(m);
    hashed = base64_encode_sb(make_ptrlen(mac, 20), 0);
    {
        strbuf *s64 = base64_encode_sb(ptrlen_from_strbuf(salt), 0);
        fp = fopen(kh_path, "w");
        expect("known_hosts: the test file is written", fp != NULL, true);
        if (!fp) {
            strbuf_free(s64);
            return;
        }
        fprintf(fp, "# a comment\n");
        fprintf(fp, "kh-web.example.com,10.9.9.9 ssh-ed25519 %s\n",
                b64_01->s);
        fprintf(fp, "[kh-two.example.com]:2200 ssh-ed25519 %s\n", b64_01->s);
        fprintf(fp, "|1|%s|%s ssh-ed25519 %s\n", s64->s, hashed->s,
                b64_02->s);
        fprintf(fp, "@cert-authority *.example.com ssh-ed25519 %s\n",
                b64_ca->s);
        fprintf(fp, "*.wild.example.com,!neg.example.com ssh-ed25519 %s\n",
                b64_02->s);
        fclose(fp);
        strbuf_free(s64);
    }

    /* the setting off: nothing is opened */
    kkh_path_hook = NULL;
    opened = kkh_opened;
    kkh_names(ptrlen_from_strbuf(web01), kh_name_add, got);
    kkh_lookup("kh-web.example.com", 22, false, kh_key_add, got);
    expect("D8 off (no hook): no name, no key", got->len == 0, true);
    expect("D8 off (no hook): the file is not opened", kkh_opened == opened,
           true);
    kkh_path_hook = kh_hook;
    kh_on = false;
    kkh_names(ptrlen_from_strbuf(web01), kh_name_add, got);
    kkh_lookup("kh-web.example.com", 22, false, kh_key_add, got);
    kkh_lookup("*.example.com", 22, true, kh_key_add, got);
    expect("D8 off (setting): no name, no key", got->len == 0, true);
    expect("D8 off (setting): the file is not opened", kkh_opened == opened,
           true);

    /* the setting on */
    kh_on = true;
    kkh_names(ptrlen_from_strbuf(web01), kh_name_add, got);
    expect("D8 on: the names of web01 (a [name]:port as name:port)",
           !strcmp(got->s, "kh-web.example.com;10.9.9.9;"
                   "kh-two.example.com:2200;"), true);
    expect("D8 on: the file was opened", kkh_opened == opened + 1, true);
    strbuf_clear(got);
    kkh_names(ptrlen_from_strbuf(web02), kh_name_add, got);
    expect("D8 on: hashed, wildcard and negated entries give no name",
           got->len == 0, true);
    {
        /* the @cert-authority line for *.example.com takes these too */
        char *a = kh_fp(web01, false), *c = kh_fp(ca2, true);
        want = dupprintf("%s%s", a, c);
        kkh_lookup("KH-WEB.example.com", 22, false, kh_key_add, got);
        expect("D8 on: look-up of a plain name (case ignored), the CA line",
               !strcmp(got->s, want), true);
        sfree(want);
        strbuf_clear(got);
        kkh_lookup("kh-two.example.com", 2200, false, kh_key_add, got);
        expect("D8 on: look-up of [name]:port", !strcmp(got->s, a), true);
        strbuf_clear(got);
        sfree(a);
        a = kh_fp(web02, false);
        want = dupprintf("%s%s", a, c);
        kkh_lookup("hashed.example.com", 22, false, kh_key_add, got);
        expect("D8 on: look-up through a hashed entry",
               !strcmp(got->s, want), true);
        sfree(want);
        sfree(a);
        sfree(c);
    }
    strbuf_clear(got);
    kkh_lookup("neg.example.com", 22, false, kh_key_add, got);
    want = kh_fp(ca2, true);
    expect("D8 on: a negated name refused, the CA line kept",
           !strcmp(got->s, want), true);
    sfree(want);
    strbuf_clear(got);
    kkh_lookup("*.example.com", 22, true, kh_key_add, got);
    want = kh_fp(web01, false);
    expect("D8 on: a pattern takes plain names on its port only",
           !strcmp(got->s, want), true);
    sfree(want);
    strbuf_clear(got);

    /* the names as shown: marked */
    {
        char *names[2] = { dupstr("web07.example.com"),
                           dupstr("10.0.0.7") };
        bool kh[2] = { true, false };
        t = kar_dest_names_text_kh(NULL, names, kh, 2);
        expect("names: a known_hosts name marked",
               t && !strcmp(t, "web07.example.com (known_hosts) 10.0.0.7"),
               true);
        sfree(t);
        t = kar_dest_names_text_kh("web07.example.com", names, kh, 2);
        expect("names: compared with the client's without the mark",
               t && !strcmp(t, "web07.example.com (client) 10.0.0.7"), true);
        sfree(t);
        sfree(names[0]);
        sfree(names[1]);
    }

    /* "Checked at each login: any matching host" through a known_hosts name */
    {
        kar_rules *r = one_rule("*.customer.com", KAR_MATCH_STORED_LATER,
                                NULL, false);
        kar_bind b;
        const char *why;
        unsigned via = 99;
        kar_hostkey_names = fake_names;
        fake_key = ptrlen_from_strbuf(web01);
        memset(&b, 0, sizeof(b));
        b.hostkey = ptrlen_from_strbuf(web01);
        b.sid = PTRLEN_LITERAL("sid");
        b.forwarding = false;
        fake_name = "web07.customer.com";
        fake_name2 = NULL;
        fake_kh = true;
        expect("later via known_hosts: permitted",
               kar_permitted_ex(r, &b, 1, true, NULL, &why, &via), true);
        expect("later via known_hosts: reported", via == KAR_VIA_KNOWN_HOSTS,
               true);
        fake_kh = false;
        expect("later via the KiTTY++ store: permitted",
               kar_permitted_ex(r, &b, 1, true, NULL, &why, &via), true);
        expect("later via the KiTTY++ store: not reported", via == 0, true);
        fake_kh = true;
        fake_name2 = "web08.customer.com";
        expect("later: a store name and a known_hosts name, the store's wins",
               kar_permitted_ex(r, &b, 1, true, NULL, &why, &via) && via == 0,
               true);
        fake_name2 = NULL;
        fake_kh = false;
        fake_name = NULL;
        kar_hostkey_names = NULL;
        kar_rules_free(r);
    }

    /* D9: the origin texts, a program's rule against known_hosts */
    {
        kar_origin o;
        kar_rules *r = one_rule("kh-web.example.com", KAR_MATCH_EXACT, web01,
                                false);
        memset(&o, 0, sizeof(o));
        o.kind = KAR_ORIGIN_PROGRAM;
        o.program = dupstr("C:\\Windows\\System32\\OpenSSH\\ssh-add.exe");
        o.when = 1761818400;            /* the day and hour vary with the local time zone */
        t = kar_origin_text(&o, &r->v[0]);
        expect("origin: Set by the program, the time, keys as in known_hosts",
               !strncmp(t, "Set by C:\\Windows\\System32\\OpenSSH\\"
                        "ssh-add.exe, 2025-10-", 52) &&
               strstr(t, " (host keys looked up by that program), host keys "
                      "as in known_hosts") != NULL, true);
        sfree(t);
        kar_hop_add_key(&r->v[0].to, ptrlen_from_strbuf(web02), false);
        t = kar_origin_text(&o, &r->v[0]);
        expect("origin: host keys not in known_hosts: 1 of 2",
               strstr(t, ", host keys not in known_hosts: 1 of 2") != NULL,
               true);
        sfree(t);
        kh_on = false;
        t = kar_origin_text(&o, &r->v[0]);
        expect("origin: no comparison while known_hosts is not read",
               strstr(t, "known_hosts") == NULL, true);
        sfree(t);
        sfree(o.program);
        o.program = NULL;
        o.when = 0;
        t = kar_origin_text(&o, NULL);
        expect("origin: a program kageant could not find, no time",
               !strcmp(t, "Set by an unknown program, time not recorded "
                       "(host keys looked up by that program)"), true);
        sfree(t);
        o.kind = KAR_ORIGIN_EDITOR;
        t = kar_origin_text(&o, NULL);
        expect("origin: the editor", !strcmp(t, "Set in the rule editor"),
               true);
        sfree(t);
        t = kar_origin_text(NULL, NULL);
        expect("origin: not recorded", !strcmp(t, "Origin not recorded"),
               true);
        sfree(t);
        kar_rules_free(r);
    }

    /* D9: the origin in the file - format 3 keeps it, format 2 has none */
    {
        kar_store *s = kar_store_new(), *u = kar_store_new();
        char *fp = kar_key_fp(ptrlen_from_strbuf(user)), *lines;
        kar_rules *r = one_rule("kh-web.example.com", KAR_MATCH_EXACT, web01,
                                false);
        kar_origin o;
        const char *err;
        strbuf *bytes = strbuf_new();
        size_t n;
        memset(&o, 0, sizeof(o));
        o.kind = KAR_ORIGIN_PROGRAM;
        o.program = dupstr("C:\\qa\\ssh-add.exe");
        o.when = 1761818400;
        kar_store_set_key_rules(s, fp, "user key", ptrlen_from_strbuf(user),
                                r, &o);
        kar_store_encode(s, bytes);
        expect("format 3: the origin round trip loads",
               kar_store_decode(u, ptrlen_from_strbuf(bytes), &err), true);
        expect("format 3: the origin is kept",
               u->nlinks == 1 && u->links[0].origin.kind == KAR_ORIGIN_PROGRAM
               && u->links[0].origin.program &&
               !strcmp(u->links[0].origin.program, "C:\\qa\\ssh-add.exe") &&
               u->links[0].origin.when == 1761818400, true);
        lines = kar_store_dest_lines(u, fp, &n);
        expect("lines: the origin under the destination",
               lines && strstr(lines, "kh-web.example.com\r\n    Set by "
                               "C:\\qa\\ssh-add.exe, ") != NULL, true);
        sfree(lines);
        strbuf_clear(bytes);
        kar_store_encode_format(s, bytes, 2);
        expect("format 2: loads", kar_store_decode(u, ptrlen_from_strbuf(bytes),
                                                  &err), true);
        expect("format 2: the origin is not recorded",
               u->nlinks == 1 && u->links[0].origin.kind == KAR_ORIGIN_UNKNOWN
               && !u->links[0].origin.program, true);
        lines = kar_store_dest_lines(u, fp, &n);
        expect("lines: Origin not recorded",
               lines && strstr(lines, "\r\n    Origin not recorded") != NULL,
               true);
        sfree(lines);
        {
            /* an origin kind no version writes is damaged */
            strbuf *mod;
            s->links[0].origin.kind = 7;
            strbuf_clear(bytes);
            kar_store_encode(s, bytes);
            mod = strbuf_dup(ptrlen_from_strbuf(bytes));
            expect("format 3: an unknown origin kind is damaged",
                   kar_store_decode(u, ptrlen_from_strbuf(mod), &err), false);
            strbuf_free(mod);
            s->links[0].origin.kind = KAR_ORIGIN_PROGRAM;
        }
        kar_origin_free(&o);
        kar_rules_free(r);
        strbuf_free(bytes);
        sfree(fp);
        kar_store_free(s);
        kar_store_free(u);
    }

    kh_on = false;
    kkh_path_hook = NULL;
    remove(kh_path);
    sfree(kh_path);
    kh_path = NULL;
    strbuf_free(got);
    strbuf_free(b64_01);
    strbuf_free(b64_02);
    strbuf_free(b64_ca);
    strbuf_free(salt);
    strbuf_free(hashed);
}

/* kkh_count: the host entries of known_hosts and the hashed ones among
 * them, counted when the setting is turned on. A file of its own. */
static void test_kh_count(strbuf *web01, strbuf *web02, strbuf *ca2)
{
    const char *tmp = getenv("TEMP");
    strbuf *b64_01 = base64_encode_sb(ptrlen_from_strbuf(web01), 0);
    strbuf *b64_02 = base64_encode_sb(ptrlen_from_strbuf(web02), 0);
    strbuf *b64_ca = base64_encode_sb(ptrlen_from_strbuf(ca2), 0);
    size_t total = 99, hashed = 99;
    unsigned long opened;
    bool ok;
    FILE *fp;

    kh_path = dupprintf("%s%sqa_kar_kh_count_%lu", tmp ? tmp : ".",
                        tmp ? "\\" : "/", (unsigned long)time(NULL));
    fp = fopen(kh_path, "w");
    expect("count: the test file is written", fp != NULL, true);
    if (!fp)
        goto out;
    fprintf(fp, "# a comment\n");
    fprintf(fp, "\n");
    fprintf(fp, "plain1.example.com,10.1.1.1 ssh-ed25519 %s\n", b64_01->s);
    fprintf(fp, "[plain2.example.com]:2200 ssh-ed25519 %s\n", b64_02->s);
    fprintf(fp, "|1|ERERERERERERERERERERERERERE=|AAAAAAAAAAAAAAAAAAAAAAAAAAA= "
            "ssh-ed25519 %s\n", b64_01->s);
    fprintf(fp, "|1|IiIiIiIiIiIiIiIiIiIiIiIiIiI=|BBBBBBBBBBBBBBBBBBBBBBBBBBB= "
            "ssh-ed25519 %s\n", b64_02->s);
    fprintf(fp, "  |1|MzMzMzMzMzMzMzMzMzMzMzMzMzM=|CCCCCCCCCCCCCCCCCCCCCCCCCCC= "
            "ssh-ed25519 %s\n", b64_02->s);
    fprintf(fp, "@cert-authority *.example.com ssh-ed25519 %s\n", b64_ca->s);
    fprintf(fp, "@revoked |1|RERERERERERERERERERERERERERE=|DDDD ssh-ed25519 %s\n",
            b64_01->s);
    fprintf(fp, "nokey.example.com ssh-ed25519\n");
    fprintf(fp, "# |1|ZmFrZQ==|ZmFrZQ== ssh-ed25519 %s\n", b64_01->s);
    fclose(fp);

    /* the setting off: nothing opened, nothing counted */
    kkh_path_hook = NULL;
    opened = kkh_opened;
    ok = kkh_count(&total, &hashed);
    expect("count off (no hook): false, 0 and 0, the file not opened",
           !ok && total == 0 && hashed == 0 && kkh_opened == opened, true);
    kkh_path_hook = kh_hook;
    kh_on = false;
    total = hashed = 99;
    ok = kkh_count(&total, &hashed);
    expect("count off (setting): false, 0 and 0, the file not opened",
           !ok && total == 0 && hashed == 0 && kkh_opened == opened, true);

    /* on: plain and hashed host lines count. Comments, empty lines, marker
     * lines and a line without a key do not. */
    kh_on = true;
    ok = kkh_count(&total, &hashed);
    expect("count on: the file read", ok && kkh_opened == opened + 1, true);
    expect("count on: 5 host entries", total == 5, true);
    expect("count on: 3 of them hashed", hashed == 3, true);

    /* a file that is not there: false, nothing counted */
    remove(kh_path);
    total = hashed = 99;
    ok = kkh_count(&total, &hashed);
    expect("count: a missing file gives false, 0 and 0",
           !ok && total == 0 && hashed == 0, true);

  out:
    kh_on = false;
    kkh_path_hook = NULL;
    remove(kh_path);
    sfree(kh_path);
    kh_path = NULL;
    strbuf_free(b64_01);
    strbuf_free(b64_02);
    strbuf_free(b64_ca);
}

/* kar_hop_add_key_new: a "fixed list" destination collected again keeps
 * its keys and takes the new ones only */
static void test_hop_merge(strbuf *web01, strbuf *web02, strbuf *ca2)
{
    kar_hop h;
    size_t added = 0;

    memset(&h, 0, sizeof(h));
    h.host = dupstr("*.customer.com");
    h.match = KAR_MATCH_STORED_NOW;
    kar_hop_add_key(&h, ptrlen_from_strbuf(web01), false);
    expect("merge: a key the hop holds is not added again",
           !kar_hop_add_key_new(&h, ptrlen_from_strbuf(web01), false) &&
           h.nkeys == 1, true);
    added += kar_hop_add_key_new(&h, ptrlen_from_strbuf(web02), false);
    added += kar_hop_add_key_new(&h, ptrlen_from_strbuf(web02), false);
    expect("merge: a new key is added once", added == 1 && h.nkeys == 2,
           true);
    expect("merge: the same blob as a CA key is another key",
           kar_hop_add_key_new(&h, ptrlen_from_strbuf(web01), true) &&
           h.nkeys == 3, true);
    expect("merge: the keys held before stay first, in order",
           ptrlen_eq_ptrlen(ptrlen_from_strbuf(h.keys[0].blob),
                            ptrlen_from_strbuf(web01)) && !h.keys[0].is_ca &&
           ptrlen_eq_ptrlen(ptrlen_from_strbuf(h.keys[1].blob),
                            ptrlen_from_strbuf(web02)), true);
    expect("merge: host and match unchanged",
           !strcmp(h.host, "*.customer.com") &&
           h.match == KAR_MATCH_STORED_NOW, true);
    /* full: no key past KAR_MAX_HOPKEYS */
    while (h.nkeys < KAR_MAX_HOPKEYS)
        kar_hop_add_key(&h, ptrlen_from_strbuf(ca2), false);
    expect("merge: a full hop takes no new key",
           !kar_hop_add_key_new(&h, ptrlen_from_strbuf(ca2), true) &&
           h.nkeys == KAR_MAX_HOPKEYS, true);
    kar_hop_free(&h);
}

/* kageant -h: one destination */
static void spec_case(const char *spec, bool ok, const char *via,
                      const char *user, const char *host)
{
    char *v = NULL, *u = NULL, *h = NULL;
    const char *err = NULL;
    char what[160];
    bool got = kar_dest_spec_parse(spec, &v, &u, &h, &err);
    snprintf(what, sizeof(what), "dest spec '%s'", spec);
    expect(what, got, ok);
    if (got && ok) {
        bool same = ((!via && !v) || (via && v && !strcmp(via, v))) &&
            ((!user && !u) || (user && u && !strcmp(user, u))) &&
            h && !strcmp(host, h);
        if (!same) {
            printf("FAIL [%s]: via %s user %s host %s\n", what,
                   v ? v : "(none)", u ? u : "(none)", h ? h : "(none)");
            failures++;
        }
    }
    if (!got && !ok && (!err || !*err)) {
        printf("FAIL [%s]: refused without a reason\n", what);
        failures++;
    }
    sfree(v);
    sfree(u);
    sfree(h);
}

static void test_dest_specs(void)
{
    char **items;
    size_t n;
    char *bad = NULL, *j;
    const char *err = NULL;
    int port;
    char *name;

    spec_case("web01", true, NULL, NULL, "web01");
    spec_case("  alice@web01 ", true, NULL, "alice", "web01");
    spec_case("jump01>web01", true, "jump01", NULL, "web01");
    spec_case("jump01>alice@web01", true, "jump01", "alice", "web01");
    spec_case("web01:2222", true, NULL, NULL, "web01:2222");
    spec_case("[web01]:2222", true, NULL, NULL, "[web01]:2222");
    spec_case("jump01:2200>web01", true, "jump01:2200", NULL, "web01");
    spec_case("*.customer.com", true, NULL, NULL, "*.customer.com");
    spec_case("deploy@*.customer.com", true, NULL, "deploy", "*.customer.com");
    spec_case("", false, NULL, NULL, NULL);
    spec_case("   ", false, NULL, NULL, NULL);
    spec_case("a>b>c", false, NULL, NULL, NULL);
    spec_case("bob@jump01>web01", false, NULL, NULL, NULL);
    spec_case("@web01", false, NULL, NULL, NULL);
    spec_case("alice@", false, NULL, NULL, NULL);
    spec_case("a@b@web01", false, NULL, NULL, NULL);
    spec_case("*.jump.com>web01", false, NULL, NULL, NULL);
    spec_case("*", false, NULL, NULL, NULL);
    spec_case("*.com", false, NULL, NULL, NULL);
    spec_case("web*.customer.com", true, NULL, NULL, "web*.customer.com");
    spec_case("customer.*", false, NULL, NULL, NULL);
    spec_case("web01:0", false, NULL, NULL, NULL);
    spec_case("web01:70000", false, NULL, NULL, NULL);
    spec_case("web01:22x", false, NULL, NULL, NULL);
    spec_case("[web01]x", false, NULL, NULL, NULL);
    spec_case("web 01", false, NULL, NULL, NULL);
    spec_case(">web01", false, NULL, NULL, NULL);
    spec_case("jump01>", false, NULL, NULL, NULL);

    expect("list: three destinations",
           kar_dest_list_parse(" web01, alice@web02 ,jump01>web03", &items,
                               &n, &bad, &err) && n == 3, true);
    if (bad) { sfree(bad); bad = NULL; }
    if (n == 3) {
        expect("list: the second without blanks",
               !strcmp(items[1], "alice@web02"), true);
        j = kar_dest_list_join(items, n);
        expect("list: joined with \", \"",
               !strcmp(j, "web01, alice@web02, jump01>web03"), true);
        sfree(j);
        kar_names_free(items, n);
    }
    expect("list: empty is no destination",
           kar_dest_list_parse("  ", &items, &n, &bad, &err) && n == 0, true);
    expect("list: an empty item between commas is refused",
           kar_dest_list_parse("web01,,web02", &items, &n, &bad, &err), false);
    sfree(bad); bad = NULL;
    expect("list: a refused item is returned",
           !kar_dest_list_parse("web01, *.com", &items, &n, &bad, &err) &&
           bad && !strcmp(bad, "*.com"), true);
    sfree(bad); bad = NULL;

    /* the port split for the look-up */
    expect("split: host:2222",
           kar_split_host_port("web01:2222", &name, &port) &&
           !strcmp(name, "web01") && port == 2222, true);
    sfree(name);
    expect("split: [host]:2200",
           kar_split_host_port("[qa-kh-two]:2200", &name, &port) &&
           !strcmp(name, "qa-kh-two") && port == 2200, true);
    sfree(name);
    expect("split: no port is 22",
           kar_split_host_port("web01", &name, &port) && port == 22, true);
    sfree(name);
    expect("split: an IPv6 address is all name",
           kar_split_host_port("fe80::1", &name, &port) &&
           !strcmp(name, "fe80::1") && port == 22, true);
    sfree(name);
    {
        ptrlen p = kar_host_name_part("web01:2222");
        expect("name part: host:port", ptrlen_eq_string(p, "web01"), true);
        p = kar_host_name_part("[web01]:2222");
        expect("name part: [host]:port", ptrlen_eq_string(p, "web01"), true);
        p = kar_host_name_part("web01");
        expect("name part: plain", ptrlen_eq_string(p, "web01"), true);
    }
}

/* The startup entry's tokens: the forms the inline parsers took, and a
 * token it does not know (dest=, written by no release) left with the path */
static void test_entry_tokens(void)
{
    kar_entry_tokens t;
    char *e;

    e = dupstr("C:\\keys\\a.ppk,encrypted,confirm,autoencrypt=600,"
               "SHA256:abc");
    kar_entry_parse(e, &t);
    expect("old entry: path", !strcmp(e, "C:\\keys\\a.ppk"), true);
    expect("old entry: encrypted", t.enc == 1, true);
    expect("old entry: confirm", t.confirm == 1, true);
    expect("old entry: autoencrypt", t.autoenc && !strcmp(t.autoenc, "600"),
           true);
    expect("old entry: fingerprint", !strcmp(t.fp, "SHA256:abc"), true);
    sfree(e);

    e = dupstr("keys\\b.ppk,plain,helloconfirm,ssh-ed25519 255 SHA256:x");
    kar_entry_parse(e, &t);
    expect("older fingerprint form", !strcmp(t.fp, "SHA256:x") &&
           t.enc == 0 && t.confirm == 2, true);
    sfree(e);

    e = dupstr("D:\\odd,name.ppk");
    kar_entry_parse(e, &t);
    expect("a comma in the path stays", !strcmp(e, "D:\\odd,name.ppk") &&
           t.enc == -1, true);
    sfree(e);

    e = dupstr("C:\\k.ppk,dest=web01,plain,SHA256:abc");
    kar_entry_parse(e, &t);
    expect("an unknown token stops the parse: it stays with the path",
           !strcmp(e, "C:\\k.ppk,dest=web01") && t.enc == 0 &&
           !strcmp(t.fp, "SHA256:abc"), true);
    sfree(e);

    e = dupstr("C:\\k.ppk,ENCRYPTED,HelloConfirm,AUTOENCRYPT=off");
    kar_entry_parse(e, &t);
    expect("tokens without regard to case", !strcmp(e, "C:\\k.ppk") &&
           t.enc == 1 && t.confirm == 2 && t.autoenc &&
           !strcmp(t.autoenc, "off"), true);
    sfree(e);
}

/* A host kept with its port (the rule editor): through the store's bytes,
 * the name match without the port, a certificate for the name */
static void test_host_port(strbuf *user, strbuf *web01, strbuf *ca,
                           strbuf *cert)
{
    kar_store *s = kar_store_new(), *t = kar_store_new();
    kar_rules *r = kar_rules_new(), *back;
    kar_rule d;
    strbuf *bytes = strbuf_new();
    const char *err = NULL;
    char *fp = kar_key_fp(ptrlen_from_strbuf(user));

    memset(&d, 0, sizeof(d));
    d.to.host = dupstr("web01:2222");
    kar_hop_add_key(&d.to, ptrlen_from_strbuf(web01), false);
    d.from.host = dupstr("[jump01]:2200");
    kar_hop_add_key(&d.from, ptrlen_from_strbuf(web01), false);
    kar_rules_add(r, &d);
    kar_hop_free(&d.from);
    kar_hop_free(&d.to);
    kar_store_set_key_rules(s, fp, "c", ptrlen_from_strbuf(user), r, NULL);
    kar_store_encode(s, bytes);
    expect("port: the store decodes",
           kar_store_decode(t, ptrlen_from_strbuf(bytes), &err), true);
    back = kar_store_rules(t, fp);
    expect("port: the host keeps its port", back && back->n == 1 &&
           !strcmp(back->v[0].to.host, "web01:2222") &&
           !strcmp(back->v[0].from.host, "[jump01]:2200"), true);
    if (back && back->n == 1) {
        char *name;
        int port;
        expect("port: the look-up gets port 2222",
               kar_split_host_port(back->v[0].to.host, &name, &port) &&
               port == 2222 && !strcmp(name, "web01"), true);
        sfree(name);
    }
    kar_rules_free(back);
    kar_rules_free(r);
    {
        /* the hints' short text: destination lines only, the first 3 */
        char *sum = kar_store_dest_summary(t, fp, 3);
        expect("summary: the host with its port, no origin line",
               sum && !strcmp(sum, "web01:2222  via [jump01]:2200"), true);
        if (sum && strcmp(sum, "web01:2222  via [jump01]:2200"))
            printf("    summary: %s\n", sum);
        sfree(sum);
        expect("summary: a key without rules has none",
               kar_store_dest_summary(t, "SHA256:nothing", 3) == NULL, true);
    }
    {
        /* five destinations: three shown, then +2 */
        kar_store *m = kar_store_new();
        kar_rules *five = kar_rules_new();
        char *sum;
        static const char *const hosts[] = { "a1", "a2", "a3", "a4", "a5" };
        for (size_t hi = 0; hi < 5; hi++)
            add_rule(five, NULL, NULL, NULL, hosts[hi], web01, false);
        kar_store_set_key_rules(m, fp, "c", ptrlen_from_strbuf(user), five,
                                NULL);
        sum = kar_store_dest_summary(m, fp, 3);
        expect("summary: the first three, then +2",
               sum && !strcmp(sum, "a1, a2, a3 +2"), true);
        if (sum && strcmp(sum, "a1, a2, a3 +2"))
            printf("    summary: %s\n", sum);
        sfree(sum);
        kar_rules_free(five);
        kar_store_free(m);
    }
    strbuf_free(bytes);
    kar_store_free(s);
    kar_store_free(t);
    sfree(fp);

    /* checked at each login: the names match without the rule's port */
    kar_hostkey_names = fake_names;
    fake_key = ptrlen_from_strbuf(web01);
    fake_name = "web07.customer.com:2222";
    r = one_rule("*.customer.com:2222", KAR_MATCH_STORED_LATER, NULL, false);
    expect("port: a pattern with a port matches the stored name",
           permitted_on(r, web01, false), true);
    kar_rules_free(r);
    kar_hostkey_names = NULL;

    /* a host certificate is checked for the name without the port */
    r = one_rule("web03:2222", KAR_MATCH_EXACT, ca, true);
    expect("port: a CA certificate for the name, the rule with a port",
           permitted_on(r, cert, false), true);
    kar_rules_free(r);
}

int main(void)
{
    strbuf *user, *web01, *web02, *jump01, *ca, *cert;
    strbuf *ca2, *web04, *noname, *named;

    /* unbuffered: an abort still shows how far the run got */
    setvbuf(stdout, NULL, _IONBF, 0);
    user = blob(B64_USER);
    web01 = blob(B64_WEB01);
    web02 = blob(B64_WEB02);
    jump01 = blob(B64_JUMP01);
    ca = blob(B64_CA);
    cert = blob(B64_CERT_WEB03);
    ca2 = blob(B64_CA2);
    web04 = blob(B64_WEB04);
    noname = blob(B64_CERT_NONAME);
    named = blob(B64_CERT_NAMED);

    expect("fixture keys decode",
           kar_blob_valid(ptrlen_from_strbuf(user)) &&
           kar_blob_valid(ptrlen_from_strbuf(web01)) &&
           kar_blob_valid(ptrlen_from_strbuf(web02)) &&
           kar_blob_valid(ptrlen_from_strbuf(jump01)) &&
           kar_blob_valid(ptrlen_from_strbuf(ca)) &&
           kar_blob_valid(ptrlen_from_strbuf(cert)) &&
           kar_blob_valid(ptrlen_from_strbuf(ca2)) &&
           kar_blob_valid(ptrlen_from_strbuf(web04)) &&
           kar_blob_valid(ptrlen_from_strbuf(noname)) &&
           kar_blob_valid(ptrlen_from_strbuf(named)), true);

    printf("parser\n");
    test_parser(web01, jump01);
    printf("walk\n");
    test_walk(user, web01, web02, jump01, ca, cert);
    printf("store\n");
    test_store(user, web01, web02, jump01);
    printf("texts\n");
    test_texts(web01, jump01);
    printf("kapper.net extensions\n");
    test_ext(user, web01, web02, jump01);
    test_keystate_local_nokey();
    printf("patterns and order\n");
    test_patterns(user, web01, web02, ca, cert);
    printf("pattern limit\n");
    test_pattern_limit();
    printf("CA store destinations, certificates without names\n");
    test_ca_store(user, ca, ca2, web04, noname, named);
    printf("CA destinations and the client's port\n");
    test_ca_ports(user, ca2, named);
    printf("known_hosts on opt-in, rule origins\n");
    test_known_hosts(user, web01, web02, ca2);
    printf("known_hosts entries counted, stored-now keys merged\n");
    test_kh_count(web01, web02, ca2);
    test_hop_merge(web01, web02, ca2);
    printf("-h destinations, startup entry tokens, hosts with a port\n");
    test_dest_specs();
    test_entry_tokens();
    test_host_port(user, web01, ca, cert);

    strbuf_free(ca2);
    strbuf_free(web04);
    strbuf_free(noname);
    strbuf_free(named);
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
