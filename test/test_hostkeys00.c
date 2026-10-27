/*
 * test_hostkeys00 - regression tests for kitty/kitty_hostkeys00.c, OpenSSH's
 * host key update: which connections may update the store (the switch,
 * readonly=yes, Connect Once, a manual host key, a certificate), the data a
 * proof signs, what each key of the server's list leads to (equal, a new
 * type, the same type with another key, a type gone, a partial answer), and
 * the record texts.
 *
 * Returns non-zero on any failure.
 */
#include <stdio.h>
#include <string.h>

#include "../kitty/kitty_hostkeys00.h"
#include "../kitty/kitty_text.h"

static int failures = 0;

static void expect(const char *what, bool got, bool want)
{
    if (got != want) {
        printf("FAIL [%s]: got %d, want %d\n", what, (int)got, (int)want);
        failures++;
    }
}

static void expect_int(const char *what, int got, int want)
{
    if (got != want) {
        printf("FAIL [%s]: got %d, want %d\n", what, got, want);
        failures++;
    }
}

static void expect_text(const char *what, const char *got, const char *want)
{
    if (strcmp(got, want)) {
        printf("FAIL [%s]: <%s>\n", what, got);
        failures++;
    }
}

static void offer(struct kitty_hk00_offer *o, const char *type, int status)
{
    o->type = type;
    o->status = status;
    o->proof = 0;
    o->action = -1;
}

int main(void)
{
    struct kitty_hk00_offer o[4];

    /* which connections: the switch x readonly x how the key was accepted */
    expect_int("stored before or now: by the store",
               kitty_hk00_how(false, false, 0), KHK00_BY_STORE);
    expect_int("not in the store after the question: Connect Once",
               kitty_hk00_how(false, false, 1), KHK00_BY_ONCE);
    expect_int("changed key used once: Connect Once",
               kitty_hk00_how(false, false, 2), KHK00_BY_ONCE);
    expect_int("manual host key, even when also stored",
               kitty_hk00_how(false, true, 0), KHK00_BY_MANUAL);
    expect_int("certificate, even when also stored",
               kitty_hk00_how(true, false, 0), KHK00_BY_CERT);
    expect("on, verified by the store: updates",
           kitty_hk00_eligible(true, false, KHK00_BY_STORE), true);
    expect("switch off: nothing",
           kitty_hk00_eligible(false, false, KHK00_BY_STORE), false);
    expect("readonly=yes: nothing",
           kitty_hk00_eligible(true, true, KHK00_BY_STORE), false);
    expect("Connect Once: nothing",
           kitty_hk00_eligible(true, false, KHK00_BY_ONCE), false);
    expect("manual host key: nothing",
           kitty_hk00_eligible(true, false, KHK00_BY_MANUAL), false);
    expect("certificate: nothing",
           kitty_hk00_eligible(true, false, KHK00_BY_CERT), false);
    expect("no host key verified (GSSAPI): nothing",
           kitty_hk00_eligible(true, false, KHK00_BY_NONE), false);

    /* the switch: on until set off */
    expect("updatehostkeys on by default", kitty_hk00_enabled(), true);
    kitty_hk00_set_enabled(0);
    expect("updatehostkeys=no", kitty_hk00_enabled(), false);
    kitty_hk00_set_enabled(1);
    expect("updatehostkeys=yes", kitty_hk00_enabled(), true);
    expect("no store hook until a program sets one", kitty_hk00_get_store() == NULL, true);

    /* equal: nothing to prove, nothing to store */
    offer(&o[0], "rsa2", 0);
    offer(&o[1], "ssh-ed25519", 0);
    expect("equal: plan", kitty_hk00_plan(o, 2, "rsa2"), true);
    expect_int("equal: the verified type", o[0].action, KHK00_NOTHING);
    expect_int("equal: the other type", o[1].action, KHK00_NOTHING);
    expect("equal: all stored", kitty_hk00_decide(o, 2), true);
    expect_int("equal: still nothing", o[1].action, KHK00_NOTHING);

    /* a new type, proven: stored */
    offer(&o[0], "rsa2", 0);
    offer(&o[1], "ssh-ed25519", 1);
    expect("new type: plan", kitty_hk00_plan(o, 2, "rsa2"), true);
    expect_int("new type: to be proven", o[1].action, KHK00_PROVE);
    o[1].proof = 1;
    expect("new type proven: complete", kitty_hk00_decide(o, 2), true);
    expect_int("new type proven: stored", o[1].action, KHK00_STORE);

    /* the same type with another key, proven: replaced */
    offer(&o[0], "rsa2", 0);
    offer(&o[1], "ssh-ed25519", 2);
    expect("rotated: plan", kitty_hk00_plan(o, 2, "rsa2"), true);
    expect_int("rotated: to be proven", o[1].action, KHK00_PROVE);
    o[1].proof = 1;
    expect("rotated proven: complete", kitty_hk00_decide(o, 2), true);
    expect_int("rotated proven: replaced", o[1].action, KHK00_REPLACE);

    /* a type gone: removed, never the verified type */
    offer(&o[0], "rsa2", 0);
    expect("type gone: plan", kitty_hk00_plan(o, 1, "rsa2"), true);
    expect("type gone: complete", kitty_hk00_decide(o, 1), true);
    expect("type gone: the stored ed25519 key is removed",
           kitty_hk00_removes(o, 1, true, "ssh-ed25519", "rsa2"), true);
    expect("type gone: an offered type stays",
           kitty_hk00_removes(o, 1, true, "rsa2", "ssh-ed25519"), false);
    expect("the verified type is never removed",
           kitty_hk00_removes(o, 0, true, "rsa2", "rsa2"), false);

    /* a failed proof: nothing stored, nothing replaced, nothing removed -
     * only the failed key gets its line */
    offer(&o[0], "rsa2", 0);
    offer(&o[1], "ssh-ed25519", 1);
    offer(&o[2], "ecdsa-sha2-nistp256", 2);
    expect("partial: plan", kitty_hk00_plan(o, 3, "rsa2"), true);
    o[1].proof = 1;
    o[2].proof = -1;
    expect("partial: not complete", kitty_hk00_decide(o, 3), false);
    expect_int("partial: the proven key not stored either", o[1].action, KHK00_NOTHING);
    expect_int("partial: the failed key reported", o[2].action, KHK00_FAILED);
    /* the same with the proven key a rotation: not replaced */
    offer(&o[0], "rsa2", 0);
    offer(&o[1], "ssh-ed25519", 2);
    offer(&o[2], "ecdsa-sha2-nistp256", 1);
    expect("failed proof, rotation: plan", kitty_hk00_plan(o, 3, "rsa2"), true);
    o[1].proof = 1;
    o[2].proof = -1;
    expect("failed proof, rotation: not complete", kitty_hk00_decide(o, 3), false);
    expect_int("failed proof, rotation: not replaced", o[1].action, KHK00_NOTHING);
    expect_int("failed proof, rotation: the failed key reported", o[2].action, KHK00_FAILED);
    expect("partial: nothing removed",
           kitty_hk00_removes(o, 3, false, "dss", "rsa2"), false);

    /* a proof the answer did not carry fails its key */
    offer(&o[0], "rsa2", 0);
    offer(&o[1], "ssh-ed25519", 1);
    expect("missing proof: plan", kitty_hk00_plan(o, 2, "rsa2"), true);
    expect("missing proof: not complete", kitty_hk00_decide(o, 2), false);
    expect_int("missing proof: failed", o[1].action, KHK00_FAILED);

    /* the verified type offered with another key: left alone, nothing
     * removed */
    offer(&o[0], "rsa2", 2);
    offer(&o[1], "ssh-ed25519", 0);
    expect("verified type, other key: plan", kitty_hk00_plan(o, 2, "rsa2"), true);
    expect_int("verified type, other key: skipped", o[0].action, KHK00_SKIP);
    expect("verified type, other key: not complete", kitty_hk00_decide(o, 2), false);

    /* lists not used at all */
    expect("empty list: not used", kitty_hk00_plan(o, 0, "rsa2"), false);
    offer(&o[0], "ssh-ed25519", 1);
    offer(&o[1], "ssh-ed25519", 2);
    expect("a type twice: not used", kitty_hk00_plan(o, 2, "rsa2"), false);

    /* the data a proof signs */
    {
        static const unsigned char sid[] = { 1, 2, 3 };
        static const unsigned char blob[] = { 'A', 'B' };
        static const unsigned char want[] = {
            0, 0, 0, 29,
            'h','o','s','t','k','e','y','s','-','p','r','o','v','e','-','0',
            '0','@','o','p','e','n','s','s','h','.','c','o','m',
            0, 0, 0, 3, 1, 2, 3,
            0, 0, 0, 2, 'A', 'B',
        };
        unsigned char out[64];
        memset(out, 0xEE, sizeof(out));
        expect_int("sigdata: length asked", (int)kitty_hk00_sigdata(NULL, 0, sid, 3, blob, 2),
                   (int)sizeof(want));
        expect_int("sigdata: too small writes nothing",
                   (int)kitty_hk00_sigdata(out, 10, sid, 3, blob, 2), (int)sizeof(want));
        expect("sigdata: too small left the buffer alone", out[0] == 0xEE, true);
        expect_int("sigdata: written", (int)kitty_hk00_sigdata(out, sizeof(out), sid, 3, blob, 2),
                   (int)sizeof(want));
        expect("sigdata: the bytes", memcmp(out, want, sizeof(want)) == 0, true);
    }

    /* the record texts */
    {
        char line[200];
        snprintf(line, sizeof(line), KT_HOSTKEY_UPDATE_STORED,
                 "ssh-ed25519 255 SHA256:abc", "srv.example", 22);
        expect_text("stored text", line,
                    "Host key stored (proven by the server): ssh-ed25519 255 "
                    "SHA256:abc, srv.example port 22");
        snprintf(line, sizeof(line), KT_HOSTKEY_UPDATE_REPLACED,
                 "ssh-ed25519 255 SHA256:new", "ssh-ed25519 255 SHA256:old",
                 "srv.example", 22);
        expect_text("replaced text", line,
                    "Host key replaced (proven by the server): ssh-ed25519 255 "
                    "SHA256:new, was ssh-ed25519 255 SHA256:old, srv.example port 22");
        snprintf(line, sizeof(line), KT_HOSTKEY_UPDATE_REMOVED,
                 "ssh-ed25519 255 SHA256:abc", "srv.example", 22);
        expect_text("removed text", line,
                    "Host key removed (no longer offered by the server): "
                    "ssh-ed25519 255 SHA256:abc, srv.example port 22");
        snprintf(line, sizeof(line), KT_HOSTKEY_UPDATE_FAILED,
                 "ssh-ed25519 255 SHA256:abc", "srv.example", 22);
        expect_text("failed text", line,
                    "Host key not stored (the server's proof failed): "
                    "ssh-ed25519 255 SHA256:abc, srv.example port 22");
        snprintf(line, sizeof(line), KT_HOSTKEY_UPDATE_REFUSED, "srv.example", 22);
        expect_text("refused text", line,
                    "Host keys not updated (the server refused the proof "
                    "request): srv.example port 22");
    }

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("test_hostkeys00: all passed\n");
    return 0;
}
