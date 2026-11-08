/*
 * test_hostkeys00 - regression tests for kitty/kitty_hostkeys00.c, OpenSSH's
 * host key update: which connections may update the store (the switch,
 * readonly=yes, Connect Once, a manual host key, a certificate), the data a
 * proof signs, what each key of the server's list leads to (equal, a new
 * type, the same type with another key, a type gone, a partial answer), the
 * keys per type of hostkeyspertype (a second key stored beside the first,
 * the limit, the verifying KEY protected rather than its type, a rotation
 * over two connections), and the record texts.
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

/* One key of the server's list. status: 0 equal to a stored key of its
 * type, 1 none of its type stored, 2 others of its type stored. nstored:
 * keys of its type stored. */
static void offer(struct kitty_hk00_offer *o, const char *type, const char *key,
                  int status, int nstored)
{
    o->type = type;
    o->key = key;
    o->status = status;
    o->nstored = nstored;
    o->proof = 0;
    o->action = -1;
    o->over = 0;
}

int main(void)
{
    struct kitty_hk00_offer o[6];
    size_t at = 99;
    int over = 0;

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

    /* the switch: yes until set otherwise. updatehostkeys=ask is on too. */
    expect_int("updatehostkeys yes by default", kitty_hk00_mode(), KHK00_MODE_YES);
    expect("updatehostkeys on by default", kitty_hk00_enabled(), true);
    kitty_hk00_set_mode(KHK00_MODE_NO);
    expect("updatehostkeys=no", kitty_hk00_enabled(), false);
    kitty_hk00_set_mode(KHK00_MODE_ASK);
    expect("updatehostkeys=ask updates", kitty_hk00_enabled(), true);
    expect_int("updatehostkeys=ask kept", kitty_hk00_mode(), KHK00_MODE_ASK);
    kitty_hk00_set_mode(7);
    expect_int("an unknown mode: yes", kitty_hk00_mode(), KHK00_MODE_YES);
    expect_int("word no", kitty_hk00_mode_from_word("No"), KHK00_MODE_NO);
    expect_int("word ask", kitty_hk00_mode_from_word("ASK"), KHK00_MODE_ASK);
    expect_int("word yes", kitty_hk00_mode_from_word("yes"), KHK00_MODE_YES);
    expect_int("word empty: yes", kitty_hk00_mode_from_word(""), KHK00_MODE_YES);
    expect_int("word other: yes", kitty_hk00_mode_from_word("asked"), KHK00_MODE_YES);
    expect_int("no word: yes", kitty_hk00_mode_from_word(NULL), KHK00_MODE_YES);
    expect("no store hook until a program sets one", kitty_hk00_get_store() == NULL, true);
    expect("no UI hook until a program sets one", kitty_hk00_get_ui() == NULL, true);

    /* hostkeyspertype: 2 by default, 1 to 32 */
    expect_int("hostkeyspertype default", kitty_hk00_pertype(), 2);
    kitty_hk00_set_pertype(0);
    expect_int("hostkeyspertype below 1", kitty_hk00_pertype(), 1);
    kitty_hk00_set_pertype(33);
    expect_int("hostkeyspertype above 32", kitty_hk00_pertype(), 32);
    kitty_hk00_set_pertype(2);

    /* ---- hostkeyspertype=1: one key per type, as before ---- */

    /* equal: nothing to prove, nothing to store */
    offer(&o[0], "rsa2", "R", 0, 1);
    offer(&o[1], "ssh-ed25519", "E", 0, 1);
    expect_int("1: equal: plan", kitty_hk00_plan(o, 2, "rsa2", "R", 1, NULL, NULL),
               KHK00_PLAN_OK);
    expect_int("1: equal: the verified type", o[0].action, KHK00_NOTHING);
    expect_int("1: equal: the other type", o[1].action, KHK00_NOTHING);
    expect("1: equal: all stored", kitty_hk00_decide(o, 2, "rsa2", "R", 1), true);
    expect_int("1: equal: still nothing", o[1].action, KHK00_NOTHING);

    /* a new type, proven: stored */
    offer(&o[0], "rsa2", "R", 0, 1);
    offer(&o[1], "ssh-ed25519", "E", 1, 0);
    expect_int("1: new type: plan", kitty_hk00_plan(o, 2, "rsa2", "R", 1, NULL, NULL),
               KHK00_PLAN_OK);
    expect_int("1: new type: to be proven", o[1].action, KHK00_PROVE);
    o[1].proof = 1;
    expect("1: new type proven: complete", kitty_hk00_decide(o, 2, "rsa2", "R", 1), true);
    expect_int("1: new type proven: stored", o[1].action, KHK00_STORE);

    /* the same type with another key, proven: replaced */
    offer(&o[0], "rsa2", "R", 0, 1);
    offer(&o[1], "ssh-ed25519", "E2", 2, 1);
    expect_int("1: rotated: plan", kitty_hk00_plan(o, 2, "rsa2", "R", 1, NULL, NULL),
               KHK00_PLAN_OK);
    expect_int("1: rotated: to be proven", o[1].action, KHK00_PROVE);
    o[1].proof = 1;
    expect("1: rotated proven: complete", kitty_hk00_decide(o, 2, "rsa2", "R", 1), true);
    expect_int("1: rotated proven: replaced", o[1].action, KHK00_REPLACE);
    expect("1: the replaced key is not also removed",
           kitty_hk00_removes(o, 2, true, "ssh-ed25519", "E1", "rsa2", "R"), false);

    /* the verified type offered with another key: left alone, nothing
     * removed */
    offer(&o[0], "rsa2", "R2", 2, 1);
    offer(&o[1], "ssh-ed25519", "E", 0, 1);
    expect_int("1: verified type, other key: plan",
               kitty_hk00_plan(o, 2, "rsa2", "R", 1, NULL, NULL), KHK00_PLAN_OK);
    expect_int("1: verified type, other key: skipped", o[0].action, KHK00_SKIP);
    expect("1: verified type, other key: not complete",
           kitty_hk00_decide(o, 2, "rsa2", "R", 1), false);

    /* a type twice: over the limit, the list is not used */
    offer(&o[0], "ssh-ed25519", "E1", 0, 1);
    offer(&o[1], "ssh-ed25519", "E2", 2, 1);
    expect_int("1: a type twice: over the limit",
               kitty_hk00_plan(o, 2, "ssh-ed25519", "E1", 1, &at, &over),
               KHK00_PLAN_OVERLIMIT);
    expect_int("1: a type twice: its first offer", (int)at, 0);
    expect_int("1: a type twice: how many", over, 2);
    /* proven first, so the limit's line lists a proven key: the new key is
     * PROVE with over set, the stored one NOTHING; proven, it is SKIP and
     * nothing is stored */
    expect_int("1: a type twice: the new key to be proven", o[1].action, KHK00_PROVE);
    expect_int("1: a type twice: for the limit only", o[1].over, 1);
    expect_int("1: a type twice: the stored key", o[0].action, KHK00_NOTHING);
    o[1].proof = 1;
    expect("1: a type twice: not complete", kitty_hk00_decide(o, 2, "ssh-ed25519", "E1", 1), false);
    expect_int("1: a type twice: proven, not stored", o[1].action, KHK00_SKIP);
    /* its proof failed: FAILED, not the limit */
    offer(&o[0], "ssh-ed25519", "E1", 0, 1);
    offer(&o[1], "ssh-ed25519", "E2", 2, 1);
    kitty_hk00_plan(o, 2, "ssh-ed25519", "E1", 1, NULL, NULL);
    o[1].proof = -1;
    kitty_hk00_decide(o, 2, "ssh-ed25519", "E1", 1);
    expect_int("1: a type twice, proof failed: FAILED", o[1].action, KHK00_FAILED);
    /* over the limit with stored keys only (the limit lowered): nothing to
     * prove, the type's first offer SKIP for the limit's line */
    offer(&o[0], "ssh-ed25519", "E1", 0, 2);
    offer(&o[1], "ssh-ed25519", "E2", 0, 2);
    expect_int("1: two stored, limit 1: over the limit",
               kitty_hk00_plan(o, 2, "ssh-ed25519", "E1", 1, NULL, NULL), KHK00_PLAN_OVERLIMIT);
    expect_int("1: two stored, limit 1: the first offer SKIP", o[0].action, KHK00_SKIP);
    expect_int("1: two stored, limit 1: for the limit", o[0].over, 1);
    expect_int("1: two stored, limit 1: nothing to prove", o[1].action, KHK00_NOTHING);

    /* ---- the default, hostkeyspertype=2 ---- */

    /* a new type, proven: stored as key 1 */
    offer(&o[0], "rsa2", "R", 0, 1);
    offer(&o[1], "ssh-ed25519", "E", 1, 0);
    expect_int("2: new type: plan", kitty_hk00_plan(o, 2, "rsa2", "R", 2, NULL, NULL),
               KHK00_PLAN_OK);
    o[1].proof = 1;
    expect("2: new type proven: complete", kitty_hk00_decide(o, 2, "rsa2", "R", 2), true);
    expect_int("2: new type proven: stored as key 1", o[1].action, KHK00_STORE);

    /* two new keys of a type nothing of is stored: key 1 and key 2 */
    offer(&o[0], "rsa2", "R", 0, 1);
    offer(&o[1], "ssh-ed25519", "E1", 1, 0);
    offer(&o[2], "ssh-ed25519", "E2", 1, 0);
    expect_int("2: two new of a type: plan",
               kitty_hk00_plan(o, 3, "rsa2", "R", 2, NULL, NULL), KHK00_PLAN_OK);
    o[1].proof = 1;
    o[2].proof = 1;
    expect("2: two new of a type: complete", kitty_hk00_decide(o, 3, "rsa2", "R", 2), true);
    expect_int("2: two new of a type: the first stored", o[1].action, KHK00_STORE);
    expect_int("2: two new of a type: the second beside it", o[2].action, KHK00_ADD);

    /* the rotation, connection A: the server signs with the old key and
     * lists old + new. The new key is proven and stored beside the old one.
     * The old key verified A and is listed, so it stays. */
    offer(&o[0], "ssh-ed25519", "OLD", 0, 1);
    offer(&o[1], "ssh-ed25519", "NEW", 2, 1);
    expect_int("rotation A: plan",
               kitty_hk00_plan(o, 2, "ssh-ed25519", "OLD", 2, NULL, NULL), KHK00_PLAN_OK);
    expect_int("rotation A: the old key", o[0].action, KHK00_NOTHING);
    expect_int("rotation A: the new key to be proven", o[1].action, KHK00_PROVE);
    o[1].proof = 1;
    expect("rotation A: complete", kitty_hk00_decide(o, 2, "ssh-ed25519", "OLD", 2), true);
    expect_int("rotation A: the new key stored as key 2", o[1].action, KHK00_ADD);
    expect("rotation A: the old key stays",
           kitty_hk00_removes(o, 2, true, "ssh-ed25519", "OLD", "ssh-ed25519", "OLD"), false);

    /* the rotation, connection B (after the admin's swap): the server signs
     * with the new key and lists it alone. The old key did not verify B and
     * is not listed: removed. The protection is the KEY, not its type. */
    offer(&o[0], "ssh-ed25519", "NEW", 0, 2);
    expect_int("rotation B: plan",
               kitty_hk00_plan(o, 1, "ssh-ed25519", "NEW", 2, NULL, NULL), KHK00_PLAN_OK);
    expect_int("rotation B: nothing to prove", o[0].action, KHK00_NOTHING);
    expect("rotation B: complete", kitty_hk00_decide(o, 1, "ssh-ed25519", "NEW", 2), true);
    expect("rotation B: the old key of the verifying type is removed",
           kitty_hk00_removes(o, 1, true, "ssh-ed25519", "OLD", "ssh-ed25519", "NEW"), true);
    expect("rotation B: the verifying key stays",
           kitty_hk00_removes(o, 1, true, "ssh-ed25519", "NEW", "ssh-ed25519", "NEW"), false);

    /* the verifying key is never removed, even when the server leaves it
     * out (OpenSSH removes it) */
    offer(&o[0], "ssh-ed25519", "E", 0, 1);
    expect_int("verifying key unlisted: plan",
               kitty_hk00_plan(o, 1, "rsa2", "R", 2, NULL, NULL), KHK00_PLAN_OK);
    expect("verifying key unlisted: complete", kitty_hk00_decide(o, 1, "rsa2", "R", 2), true);
    expect("verifying key unlisted: kept",
           kitty_hk00_removes(o, 1, true, "rsa2", "R", "rsa2", "R"), false);
    expect("a type gone: its key removed",
           kitty_hk00_removes(o, 1, true, "dss", "D", "rsa2", "R"), true);

    /* the verifying type with another key, the verifying key unlisted: the
     * new key is stored beside it (the verifying key takes one place) */
    offer(&o[0], "ssh-ed25519", "E2", 2, 1);
    expect_int("verifying type, other key: plan",
               kitty_hk00_plan(o, 1, "ssh-ed25519", "E1", 2, NULL, NULL), KHK00_PLAN_OK);
    expect_int("verifying type, other key: to be proven", o[0].action, KHK00_PROVE);
    o[0].proof = 1;
    expect("verifying type, other key: complete",
           kitty_hk00_decide(o, 1, "ssh-ed25519", "E1", 2), true);
    expect_int("verifying type, other key: stored as key 2", o[0].action, KHK00_ADD);

    /* two keys stored, one listed with a new one: the unlisted stored key
     * goes, the new key takes its number */
    offer(&o[0], "ssh-ed25519", "A", 0, 2);
    offer(&o[1], "ssh-ed25519", "C", 2, 2);
    expect_int("swap the second key: plan",
               kitty_hk00_plan(o, 2, "ssh-ed25519", "A", 2, NULL, NULL), KHK00_PLAN_OK);
    o[1].proof = 1;
    expect("swap the second key: complete",
           kitty_hk00_decide(o, 2, "ssh-ed25519", "A", 2), true);
    expect_int("swap the second key: the new key as key 2", o[1].action, KHK00_ADD);
    expect("swap the second key: B removed",
           kitty_hk00_removes(o, 2, true, "ssh-ed25519", "B", "ssh-ed25519", "A"), true);

    /* no room: the verifying key is unlisted and stays, two other keys of
     * its type listed - three would be stored. Nothing of that type. */
    offer(&o[0], "ssh-ed25519", "B", 0, 2);
    offer(&o[1], "ssh-ed25519", "C", 2, 2);
    expect_int("no room: plan",
               kitty_hk00_plan(o, 2, "ssh-ed25519", "A", 2, NULL, NULL), KHK00_PLAN_OK);
    expect_int("no room: proven for the limit", o[1].action, KHK00_PROVE);
    expect_int("no room: for the limit only", o[1].over, 1);
    o[1].proof = 1;
    expect("no room: not complete", kitty_hk00_decide(o, 2, "ssh-ed25519", "A", 2), false);
    expect_int("no room: proven, skipped", o[1].action, KHK00_SKIP);
    expect("no room: nothing removed",
           kitty_hk00_removes(o, 2, false, "ssh-ed25519", "B", "ssh-ed25519", "A"), false);
    /* the same list with room for three: C is proven, nothing skipped */
    offer(&o[0], "ssh-ed25519", "B", 0, 2);
    offer(&o[1], "ssh-ed25519", "C", 2, 2);
    expect_int("room for three: plan",
               kitty_hk00_plan(o, 2, "ssh-ed25519", "A", 3, NULL, NULL), KHK00_PLAN_OK);
    expect_int("room for three: C to be proven", o[1].action, KHK00_PROVE);

    /* the limit: three keys of one type with hostkeyspertype=2 - the list
     * is not used at all */
    offer(&o[0], "rsa2", "R", 0, 1);
    offer(&o[1], "ssh-ed25519", "E1", 1, 0);
    offer(&o[2], "ssh-ed25519", "E2", 1, 0);
    offer(&o[3], "ssh-ed25519", "E3", 1, 0);
    at = 99; over = 0;
    expect_int("over the limit: not used",
               kitty_hk00_plan(o, 4, "rsa2", "R", 2, &at, &over), KHK00_PLAN_OVERLIMIT);
    expect_int("over the limit: the type's first offer", (int)at, 1);
    expect_int("over the limit: how many", over, 3);
    expect_int("over the limit: E1..E3 proven for the limit",
               (o[1].action == KHK00_PROVE && o[1].over && o[2].action == KHK00_PROVE &&
                o[2].over && o[3].action == KHK00_PROVE && o[3].over), 1);
    expect_int("over the limit: the other type not touched", o[0].action, KHK00_NOTHING);
    expect_int("the same list with room: used",
               kitty_hk00_plan(o, 4, "rsa2", "R", 3, NULL, NULL), KHK00_PLAN_OK);

    /* a key listed twice: not used (OpenSSH) */
    offer(&o[0], "ssh-ed25519", "E", 0, 1);
    offer(&o[1], "ssh-ed25519", "E", 0, 1);
    expect_int("a key twice: not used",
               kitty_hk00_plan(o, 2, "rsa2", "R", 2, NULL, NULL), KHK00_PLAN_UNUSED);
    expect_int("empty list: not used",
               kitty_hk00_plan(o, 0, "rsa2", "R", 2, NULL, NULL), KHK00_PLAN_UNUSED);

    /* a failed proof: nothing stored, nothing added, nothing removed - only
     * the failed key gets its line */
    offer(&o[0], "rsa2", "R", 0, 1);
    offer(&o[1], "ssh-ed25519", "E", 1, 0);
    offer(&o[2], "ecdsa-sha2-nistp256", "P2", 2, 1);
    expect_int("failed proof: plan", kitty_hk00_plan(o, 3, "rsa2", "R", 2, NULL, NULL),
               KHK00_PLAN_OK);
    o[1].proof = 1;
    o[2].proof = -1;
    expect("failed proof: not complete", kitty_hk00_decide(o, 3, "rsa2", "R", 2), false);
    expect_int("failed proof: the proven key not stored either", o[1].action, KHK00_NOTHING);
    expect_int("failed proof: the failed key reported", o[2].action, KHK00_FAILED);
    expect("failed proof: nothing removed",
           kitty_hk00_removes(o, 3, false, "dss", "D", "rsa2", "R"), false);
    /* the same with the proven key a second key of its type: not added */
    offer(&o[0], "rsa2", "R", 0, 1);
    offer(&o[1], "ssh-ed25519", "E2", 2, 1);
    offer(&o[2], "ecdsa-sha2-nistp256", "P", 1, 0);
    expect_int("failed proof, rotation: plan",
               kitty_hk00_plan(o, 3, "rsa2", "R", 2, NULL, NULL), KHK00_PLAN_OK);
    o[1].proof = 1;
    o[2].proof = -1;
    expect("failed proof, rotation: not complete", kitty_hk00_decide(o, 3, "rsa2", "R", 2), false);
    expect_int("failed proof, rotation: not added", o[1].action, KHK00_NOTHING);
    expect_int("failed proof, rotation: the failed key reported", o[2].action, KHK00_FAILED);

    /* a proof the answer did not carry fails its key */
    offer(&o[0], "rsa2", "R", 0, 1);
    offer(&o[1], "ssh-ed25519", "E", 1, 0);
    expect_int("missing proof: plan", kitty_hk00_plan(o, 2, "rsa2", "R", 2, NULL, NULL),
               KHK00_PLAN_OK);
    expect("missing proof: not complete", kitty_hk00_decide(o, 2, "rsa2", "R", 2), false);
    expect_int("missing proof: failed", o[1].action, KHK00_FAILED);

    /* a partial answer (a key skipped): the proven keys are stored, but the
     * list is not complete, so nothing is removed and the stored keys of a
     * type keep their places */
    offer(&o[0], "rsa2", "R", 0, 1);
    offer(&o[1], "ssh-ed25519", "B", 0, 2);
    offer(&o[2], "ssh-ed25519", "C", 2, 2);
    offer(&o[3], "ecdsa-sha2-nistp256", "P", 1, 0);
    expect_int("partial: plan",
               kitty_hk00_plan(o, 4, "ssh-ed25519", "A", 2, NULL, NULL), KHK00_PLAN_OK);
    expect_int("partial: no room for C, proven for the limit", o[2].action, KHK00_PROVE);
    expect_int("partial: C for the limit only", o[2].over, 1);
    /* C's proof failed: its line, and it holds back nothing - it was
     * proven only for the limit's line */
    o[2].proof = -1;
    o[3].proof = 1;
    expect("partial: not complete", kitty_hk00_decide(o, 4, "ssh-ed25519", "A", 2), false);
    expect_int("partial: C's failed proof reported", o[2].action, KHK00_FAILED);
    expect_int("partial: the new type stored", o[3].action, KHK00_STORE);
    expect("partial: nothing removed",
           kitty_hk00_removes(o, 4, false, "dss", "D", "ssh-ed25519", "A"), false);
    /* ... and with room only while the stored keys go, a partial list adds
     * nothing: two stored, no room for a third */
    offer(&o[0], "ssh-ed25519", "A", 0, 2);
    offer(&o[1], "ssh-ed25519", "C", 2, 2);
    offer(&o[2], "ecdsa-sha2-nistp256", "P2", 2, 1);
    expect_int("partial, full type: plan",
               kitty_hk00_plan(o, 3, "rsa2", "R", 1, NULL, NULL), KHK00_PLAN_OVERLIMIT);
    expect_int("partial, full type: plan with 2",
               kitty_hk00_plan(o, 3, "ecdsa-sha2-nistp256", "P1", 2, NULL, NULL),
               KHK00_PLAN_OK);
    o[1].proof = 1;
    o[2].proof = 1;
    /* P1 verified and is unlisted: P2 gets key 2 of its type. A's type is
     * complete, so B (stored, unlisted) goes and C takes key 2. */
    expect("partial, full type: complete",
           kitty_hk00_decide(o, 3, "ecdsa-sha2-nistp256", "P1", 2), true);
    expect_int("partial, full type: C beside A", o[1].action, KHK00_ADD);
    expect_int("partial, full type: P2 beside P1", o[2].action, KHK00_ADD);

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
        expect_int("sigdata: length requested", (int)kitty_hk00_sigdata(NULL, 0, sid, 3, blob, 2),
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
        char line[240];
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
        snprintf(line, sizeof(line), KT_HOSTKEY_UPDATE_ADDED, 2,
                 "ssh-ed25519 255 SHA256:new", "ssh-ed25519 255 SHA256:old",
                 "srv.example", 22);
        expect_text("added text", line,
                    "Host key stored as key 2 of its type (proven by the server): "
                    "ssh-ed25519 255 SHA256:new, beside ssh-ed25519 255 SHA256:old, "
                    "srv.example port 22");
        snprintf(line, sizeof(line), KT_HOSTKEY_UPDATE_PRIMARY,
                 "ssh-ed25519 255 SHA256:new", "srv.example", 22);
        expect_text("made primary text", line,
                    "Host key made primary (the old key is no longer offered): "
                    "ssh-ed25519 255 SHA256:new, srv.example port 22");
        snprintf(line, sizeof(line), KT_HOSTKEY_UPDATE_OVERLIMIT, "srv.example", 22, 2,
                 "ssh-ed25519");
        expect_text("over the limit text", line,
                    "Host keys not updated for srv.example port 22: the limit "
                    "\"Keys per type and host\" (2) blocks the change of the "
                    "ssh-ed25519 keys.");
        snprintf(line, sizeof(line), KT_HOSTKEY_UPDATE_OVERLIMIT_STORED,
                 "ssh-ed25519 255 SHA256:a");
        expect_text("over the limit: a stored key", line, "  Stored: ssh-ed25519 255 SHA256:a");
        snprintf(line, sizeof(line), KT_HOSTKEY_UPDATE_OVERLIMIT_OFFERED,
                 "ssh-ed25519 255 SHA256:c");
        expect_text("over the limit: an offered key", line, "  Offered: ssh-ed25519 255 SHA256:c");
        snprintf(line, sizeof(line), KT_HK00_NOTICE_TEXT_REPLACED, "srv.example", 22,
                 "ssh-ed25519 255 SHA256:new", "ssh-ed25519 255 SHA256:old");
        expect_text("notice: a replaced key", line,
                    "srv.example port 22\nNew: ssh-ed25519 255 SHA256:new\n"
                    "Replaced: ssh-ed25519 255 SHA256:old\n\"Keys per type and host\" "
                    "is 1: the new key replaces the stored one.");
        snprintf(line, sizeof(line), KT_HOSTKEY_UPDATE_UNANSWERED,
                 "ssh-ed25519 255 SHA256:abc", "srv.example", 22);
        expect_text("unanswered text", line,
                    "Host key not stored (updatehostkeys=ask, not answered): "
                    "ssh-ed25519 255 SHA256:abc, srv.example port 22");
        snprintf(line, sizeof(line), KT_HOSTKEY_UPDATE_KEPT_UNANSWERED,
                 "ssh-ed25519 255 SHA256:abc", "srv.example", 22);
        expect_text("kept unanswered text", line,
                    "Host key kept (updatehostkeys=ask, not answered): "
                    "ssh-ed25519 255 SHA256:abc, srv.example port 22");
        snprintf(line, sizeof(line), KT_HOSTKEY_UPDATE_DECLINED,
                 "ssh-ed25519 255 SHA256:abc", "srv.example", 22);
        expect_text("declined text", line,
                    "Host key not stored (declined): ssh-ed25519 255 SHA256:abc, "
                    "srv.example port 22");
        snprintf(line, sizeof(line), KT_HOSTKEY_UPDATE_KEPT,
                 "ssh-ed25519 255 SHA256:abc", "srv.example", 22);
        expect_text("kept text", line,
                    "Host key kept (declined): ssh-ed25519 255 SHA256:abc, "
                    "srv.example port 22");
    }

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("test_hostkeys00: all passed\n");
    return 0;
}
