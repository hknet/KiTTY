/*
 * test_certinfo.c - what kageant shows of a certificate
 * (kitty/kitty_certinfo.c): the fields read from the blob, the state at a
 * given time in local calendar days, the key list's Certificate column.
 */
#include <winsock2.h>   /* before windows.h: putty.h pulls it in further down */
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "putty.h"
#include "ssh.h"
#include "kitty/kitty_certinfo.h"

const char *appname = "test_certinfo";

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
    modalfatalbox("random_read called in test_certinfo");
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

/* an ed25519 key and a CA key (fixtures of test_agentrestrict.c) */
static const char *const B64_KEY =
    "AAAAC3NzaC1lZDI1NTE5AAAAIJaznXTFVKK9hf8gjn9npSgOtWGq4+e5pk/IiBv732ih";
static const char *const B64_CA =
    "AAAAC3NzaC1lZDI1NTE5AAAAIH/vQofRFaKADuSxqWxImjxsSufce7bDr+P9IP0aILxu";

/* A user certificate of B64_KEY signed by B64_CA, laid out as OpenSSH
 * does. The signature is not checked by anything here: filler. */
static strbuf *make_cert(uint64_t after, uint64_t before,
                         const char *const *principals,
                         const char *force_command)
{
    strbuf *key = base64_decode_sb(ptrlen_from_asciz(B64_KEY));
    strbuf *ca = base64_decode_sb(ptrlen_from_asciz(B64_CA));
    strbuf *c = strbuf_new(), *list = strbuf_new(), *opts = strbuf_new();
    BinarySource src[1];

    BinarySource_BARE_INIT_PL(src, ptrlen_from_strbuf(key));
    get_string(src);                                  /* ssh-ed25519 */
    put_stringz(c, "ssh-ed25519-cert-v01@openssh.com");
    put_stringz(c, "0123456789abcdef0123456789abcdef"); /* nonce */
    put_datapl(c, get_data(src, get_avail(src)));     /* the key's field */
    put_uint64(c, 7);                                 /* serial */
    put_uint32(c, 1);                                 /* user */
    put_stringz(c, "qa-key-id");
    for (; principals && *principals; principals++)
        put_stringz(list, *principals);
    put_stringsb(c, list);
    put_uint64(c, after);
    put_uint64(c, before);
    if (force_command) {
        strbuf *v = strbuf_new();
        put_stringz(v, force_command);
        put_stringz(opts, "force-command");
        put_stringsb(opts, v);
    }
    put_stringsb(c, opts);
    put_stringz(c, "");                               /* extensions */
    put_stringz(c, "");                               /* reserved */
    put_stringsb(c, ca);
    put_stringz(c, "signature");
    strbuf_free(key);
    return c;
}

static char *date_stub(uint64_t t, void *ctx)
{
    (void)ctx;
    return dupprintf("D%llu", (unsigned long long)t);
}

#define DAY 86400ULL
/* a time at noon UTC: whole days from it stay at noon */
#define NOW 1793793600ULL

static void test_fields(void)
{
    static const char *const two[] = { "alice", "deploy", NULL };
    strbuf *c = make_cert(NOW - DAY, NOW + 30 * DAY, two,
                          "/usr/bin/backup");
    strbuf *key = base64_decode_sb(ptrlen_from_asciz(B64_KEY));
    kcert_info ci;
    memset(&ci, 0, sizeof(ci));

    expect("a certificate is read", kcert_info_get(ptrlen_from_strbuf(c), &ci),
           true);
    expect("valid from", ci.valid_after == NOW - DAY, true);
    expect("valid to", ci.valid_before == NOW + 30 * DAY, true);
    expect_str("principals", ci.principals, "alice, deploy");
    expect_str("critical options", ci.critical,
               "force-command=/usr/bin/backup");
    expect_str("key id", ci.key_id, "qa-key-id");
    expect("the CA as SHA256:...", ci.ca_fp && !strncmp(ci.ca_fp, "SHA256:", 7)
           && !strchr(ci.ca_fp, ' '), true);
    kcert_info_free(&ci);

    memset(&ci, 0, sizeof(ci));
    expect("a plain key is no certificate",
           kcert_info_get(ptrlen_from_strbuf(key), &ci), false);
    strbuf_free(c);

    c = make_cert(0, UINT64_MAX, NULL, NULL);
    expect("no principals, no options: read",
           kcert_info_get(ptrlen_from_strbuf(c), &ci), true);
    expect_str("no principals: empty", ci.principals, "");
    expect_str("no options: empty", ci.critical, "");
    kcert_info_free(&ci);
    strbuf_free(c);

    c = make_cert(0, UINT64_MAX, NULL, NULL);
    {
        strbuf *b = kcert_base_blob(ptrlen_from_strbuf(c));
        strbuf *b2 = kcert_base_blob(ptrlen_from_strbuf(key));
        expect("a certificate's base blob is its key's",
               ptrlen_eq_ptrlen(ptrlen_from_strbuf(b),
                                ptrlen_from_strbuf(key)), true);
        expect("a plain key's base blob is itself",
               ptrlen_eq_ptrlen(ptrlen_from_strbuf(b2),
                                ptrlen_from_strbuf(key)), true);
        strbuf_free(b);
        strbuf_free(b2);
    }
    strbuf_shrink_to(c, c->len - 5);
    expect("a cut certificate is not read",
           kcert_info_get(ptrlen_from_strbuf(c), &ci), false);
    strbuf_free(c);
    strbuf_free(key);
}

static int state_of(uint64_t after, uint64_t before, uint64_t now,
                    int64_t tz, int *days)
{
    kcert_info ci;
    memset(&ci, 0, sizeof(ci));
    ci.valid_after = after;
    ci.valid_before = before;
    return kcert_state(&ci, now, tz, days);
}

static void test_states(void)
{
    int d;
    expect("before valid_after: not yet valid",
           state_of(NOW + 1, UINT64_MAX, NOW, 0, &d) == KCERT_NOT_YET, true);
    expect("at valid_after: valid",
           state_of(NOW, UINT64_MAX, NOW, 0, &d) == KCERT_VALID, true);
    expect("at valid_before: expired",
           state_of(0, NOW, NOW, 0, &d) == KCERT_EXPIRED, true);
    expect("later the same day: expires today",
           state_of(0, NOW + 3600, NOW, 0, &d) == KCERT_EXPIRES_TODAY, true);
    expect("in 3 days: expiring, 3",
           state_of(0, NOW + 3 * DAY, NOW, 0, &d) == KCERT_EXPIRING && d == 3,
           true);
    expect("in 14 days: expiring, 14",
           state_of(0, NOW + 14 * DAY, NOW, 0, &d) == KCERT_EXPIRING && d == 14,
           true);
    expect("in 15 days: valid",
           state_of(0, NOW + 15 * DAY, NOW, 0, &d) == KCERT_VALID, true);
    expect("valid for ever: valid",
           state_of(0, UINT64_MAX, NOW, 0, &d) == KCERT_VALID, true);
    /* 23:00 UTC, the end at 00:30 UTC: the next day in UTC, the same day
     * an hour east of it */
    expect("local days: tomorrow in UTC",
           state_of(0, NOW + 12 * 3600 + 1800, NOW + 11 * 3600, 0, &d) ==
           KCERT_EXPIRING && d == 1, true);
    expect("local days: today an hour east",
           state_of(0, NOW + 12 * 3600 + 1800, NOW + 11 * 3600, 3600, &d) ==
           KCERT_EXPIRES_TODAY, true);
    expect("levels: valid 0, expiring 1, today 1, expired 2, not yet 2",
           kcert_state_level(KCERT_VALID) == 0 &&
           kcert_state_level(KCERT_EXPIRING) == 1 &&
           kcert_state_level(KCERT_EXPIRES_TODAY) == 1 &&
           kcert_state_level(KCERT_EXPIRED) == 2 &&
           kcert_state_level(KCERT_NOT_YET) == 2, true);
}

static void column(const char *what, kcert_info *c, size_t n,
                   const char *want, int wantlevel)
{
    int level = -1;
    char *t = kcert_column_text(c, n, NOW, 0, date_stub, NULL, &level);
    expect_str(what, t, want);
    expect(what, level == wantlevel, true);
    sfree(t);
}

static void test_column(void)
{
    kcert_info c[3];
    char want[64];
    memset(c, 0, sizeof(c));
    column("no certificate: empty", c, 0, "", 0);

    c[0].valid_before = UINT64_MAX;
    c[1].valid_before = UINT64_MAX;
    column("one valid", c, 1, "certificate", 0);
    column("two valid", c, 2, "2 certificates", 0);

    c[1].valid_before = NOW - DAY;
    snprintf(want, sizeof(want), "EXPIRED since D%llu (+1)",
             (unsigned long long)(NOW - DAY));
    column("one expired of two: the expired one first", c, 2, want, 2);

    c[0].valid_before = NOW + 10 * DAY;
    c[1].valid_before = NOW + 3 * DAY;
    c[2].valid_before = UINT64_MAX;
    column("expiring: the nearest first", c, 3, "expires in 3 days (+2)", 1);

    c[1].valid_before = NOW + DAY;
    column("one day", c, 2, "expires in 1 day (+1)", 1);

    c[0].valid_before = UINT64_MAX;
    c[0].valid_after = NOW + 2 * DAY;
    snprintf(want, sizeof(want), "valid from D%llu",
             (unsigned long long)(NOW + 2 * DAY));
    column("not yet valid", c, 1, want, 2);

    c[0].valid_after = 0;
    c[0].valid_before = NOW + 60;
    column("expires today", c, 1, "expires today", 1);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("certificate fields\n");
    test_fields();
    printf("states\n");
    test_states();
    printf("the Certificate column\n");
    test_column();
    if (fails) {
        printf("%d FAILED\n", fails);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
