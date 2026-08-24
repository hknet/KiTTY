/*
 * The Hello-wrapped-secret container (kitty/kitty_hello.c) - the whole
 * layer below the Hello UI, driven with fixed KEKs so no Hello prompt or
 * TPM is involved: round trips through both wraps, refusal of a wrong
 * KEK / wrong passphrase / tampered blob, and refusal of a container
 * whose recorded Argon2 cost is illegal (which must never reach the KDF).
 * A parsed-but-refused open and an absent field are distinct results, so
 * callers can tell "no recovery wrap" from "wrong recovery passphrase".
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "putty.h"
#include "ssh.h"
#include "kitty/kitty_hello.h"
#include "kitty/kitty_hello_keys.h"

const char *appname = "test_hello_container";

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

/* sshpubk.c (linked for the protect path) calls this on an old-format
 * key; the test never loads one. */
void old_keyfile_warning(void) {}

static int fails = 0;

/* The PPK writer (sshpubk.c, linked for the protect path) draws its salt
 * from random_read; this binary has no PuTTY pool, so: the OS CSPRNG. */
void random_read(void *buf, size_t size)
{
    unsigned char *p = (unsigned char *)buf;
    while (size > 0) {
        unsigned char chunk[KITTY_HELLO_SECRET_LEN];
        size_t n = size < sizeof(chunk) ? size : sizeof(chunk);
        if (!kitty_hello_new_secret(chunk))
            modalfatalbox("random generator failed");
        memcpy(p, chunk, n);
        p += n;
        size -= n;
    }
}

static void check(bool cond, const char *what)
{
    printf("%-58s %s\n", what, cond ? "ok" : "FAILED");
    if (!cond)
        fails++;
}

/*
 * A container written by an EARLIER build (same fixed KEK/passphrase/
 * secret as below). Every build must keep opening it: this is the
 * "nobody loses access by upgrading" guarantee as a test - the recorded
 * Argon2 cost means even a future cost change must not break it.
 * Refresh (only alongside a deliberate format bump): run with argument
 * "emit" and paste the output here.
 */
static const char FIXTURE[] =
    "HELLOK1:HMKTN3WDPmEN2o0O0cFDHid+nKBn+4uE6JLkpFocDbj49uFM715HIG/nYL3fl"
    "ocotE/IrmPPfovo9Rb31.R65536,3,4,SrcwMuQvEmNw/8YDI+heFg==,pq1//CKI84hy"
    "RMgWrZox/PwTW5mcK60hzodztuYFHVQtLzwOKdHf4zGvpPyVLxc4vWtaRBTrl4jE31ot";

int main(int argc, char **argv)
{
    unsigned char kek[32], wrong_kek[32];
    unsigned char secret[KITTY_HELLO_SECRET_LEN];
    unsigned char out[KITTY_HELLO_SECRET_LEN];
    const char *pass = "correct horse battery staple";
    char *c;
    size_t i;

    for (i = 0; i < sizeof(kek); i++) kek[i] = (unsigned char)(i * 7 + 1);
    for (i = 0; i < sizeof(wrong_kek); i++) wrong_kek[i] = (unsigned char)i;
    for (i = 0; i < sizeof(secret); i++) secret[i] = (unsigned char)(255 - i);

    if (argc > 1 && !strcmp(argv[1], "emit")) {
        c = kitty_hello_container_create(kek, pass, secret);
        printf("%s\n", c ? c : "(failed)");
        sfree(c);
        return 0;
    }

    /* HANDS-ON: the persistence proof, via the PRODUCT policy layer.
     * "persist1" wraps a fixed secret with kitty_hello_wrap_auto (real
     * foreground window; PRF preferred) and writes the container beside
     * the exe; after a REBOOT or sign-out, "persist2" unwraps it with
     * kitty_hello_unwrap_auto and compares. persist2 leaves the (real,
     * reusable) credential alone and deletes only the container file. */
    if (argc > 1 && (!strcmp(argv[1], "persist1") ||
                     !strcmp(argv[1], "persist2"))) {
        const char *path = "persist_test.hello";
        HWND owner = GetForegroundWindow();
        kitty_hello_trace_enable();
        if (!strcmp(argv[1], "persist1")) {
            char *cont = NULL;
            int source = 0;
            int r = kitty_hello_wrap_auto(owner, secret,
                                          "persist-test-recovery", &cont,
                                          &source);
            if (r != 0 || !cont) {
                printf("persist1: wrap FAILED (code %d)\n", r);
                return 1;
            }
            {
                FILE *fp = fopen(path, "wb");
                if (!fp) { printf("persist1: cannot write %s\n", path);
                           sfree(cont); return 1; }
                fputs(cont, fp);
                fclose(fp);
            }
            {
                char *t = kitty_hello_secret_text(secret);
                printf("persist1: wrapped OK via %s; container in %s\n"
                       "printed secret (for the printout test): %s\n"
                       "NOW REBOOT (or sign out/in), then run: "
                       "test_hello_container.exe persist2\n",
                       source == KITTY_HELLO_SOURCE_PRF ? "PRF" : "KCM",
                       path, t);
                sfree(t);
            }
            sfree(cont);
            return 0;
        } else {
            char buf[4096];
            size_t n;
            unsigned char got[KITTY_HELLO_SECRET_LEN];
            int r;
            FILE *fp = fopen(path, "rb");
            if (!fp) { printf("persist2: %s missing - run persist1 "
                              "first\n", path); return 1; }
            n = fread(buf, 1, sizeof(buf) - 1, fp);
            fclose(fp);
            buf[n] = '\0';
            r = kitty_hello_unwrap_auto(owner, buf, got);
            if (r == 0 && memcmp(got, secret, sizeof(secret)) == 0) {
                printf("persist2: PASS - the secret survived the "
                       "reboot/sign-out; PRF is stable on a persisted "
                       "credential\n");
                remove(path);
                return 0;
            }
            printf("persist2: FAIL - unwrap code %d%s\n", r,
                   r == 0 ? " (secret MISMATCH!)" : "");
            return 1;
        }
    }

    /* No UI, no prompt: list what the platform (NGC) store holds. */
    if (argc > 1 && !strcmp(argv[1], "list")) {
        char *out = NULL;
        int r = kitty_hello_webauthn_list(&out);
        printf("%s\n", out ? out : "(no output)");
        sfree(out);
        return r >= 0 ? 0 : 1;
    }

    /* HANDS-ON ONLY, never in the gate: the WebAuthn/PRF probe - creates
     * a throwaway platform passkey, two PRF assertions (a Hello prompt
     * each), deletes it, prints the verdict. Optional extra args, any
     * order: "noprf" / "hmacext" (how PRF is requested; default the
     * WebAuthn-level enable) and "rp=<id>" (default kitty.kapper.net). */
    if (argc > 1 && !strcmp(argv[1], "webauthn")) {
        char *msg = NULL;
        int r, i;
        int prf_mode = KITTY_HELLO_PRF_ENABLE;
        int flags = 0;
        WCHAR rpbuf[128];
        const WCHAR *rp = NULL;
        kitty_hello_trace_enable();
        for (i = 2; i < argc; i++) {
            if (!strcmp(argv[i], "noprf")) {
                prf_mode = KITTY_HELLO_PRF_NONE;
            } else if (!strcmp(argv[i], "hmacext")) {
                prf_mode = KITTY_HELLO_PRF_HMAC_EXT;
            } else if (!strcmp(argv[i], "fgwnd")) {
                flags |= KITTY_HELLO_WA_FGWND;
            } else if (!strcmp(argv[i], "uvpref")) {
                flags |= KITTY_HELLO_WA_UVPREF;
            } else if (!strncmp(argv[i], "rp=", 3) && argv[i][3]) {
                if (MultiByteToWideChar(CP_UTF8, 0, argv[i] + 3, -1,
                                        rpbuf, 128) > 0)
                    rp = rpbuf;
            } else {
                printf("unknown argument: %s\n", argv[i]);
                return 2;
            }
        }
        r = kitty_hello_webauthn_probe_ex(rp ? rp : KITTY_HELLO_RP_ID,
                                          prf_mode, flags, &msg);
        printf("webauthn probe: %s (code %d)\n",
               msg ? msg : "(no message)", r);
        sfree(msg);
        return r == 0 ? 0 : 1;
    }

    /* HANDS-ON ONLY, never in the gate: the live determinism proof. Two
     * Hello prompts (the first run also creates the kapper.net.KiTTY
     * credential, with its own enrolment UI); prints the verdict. */
    if (argc > 1 && !strcmp(argv[1], "selftest")) {
        char *msg = NULL;
        int r;
        int nohost = (argc > 2 && !strcmp(argv[2], "nohost"));
        kitty_hello_trace_enable();
        r = kitty_hello_kek_selftest_ex(nohost, &msg);
        printf("selftest: %s (code %d)\n", msg ? msg : "(no message)", r);
        sfree(msg);
        return r == 0 ? 0 : 1;
    }

    memset(out, 0, sizeof(out));
    check(kitty_hello_container_open_kek(FIXTURE, kek, out) == 1 &&
          memcmp(out, secret, sizeof(secret)) == 0,
          "fixture from an earlier build opens via hello KEK");
    memset(out, 0, sizeof(out));
    check(kitty_hello_container_open_recovery(FIXTURE, pass, out) == 1 &&
          memcmp(out, secret, sizeof(secret)) == 0,
          "fixture from an earlier build opens via recovery");

    /* Both wraps present. */
    c = kitty_hello_container_create(kek, pass, secret);
    check(c != NULL, "dual-wrap container created");
    if (c) {
        check(strncmp(c, "HELLOK1:", 8) == 0, "container carries the marker");
        check(kitty_hello_container_valid(c), "container is valid");
        check(kitty_hello_container_has_hello(c), "hello wrap present");
        check(kitty_hello_container_has_recovery(c), "recovery wrap present");

        memset(out, 0, sizeof(out));
        check(kitty_hello_container_open_kek(c, kek, out) == 1 &&
              memcmp(out, secret, sizeof(secret)) == 0,
              "hello KEK opens it, secret round-trips");
        memset(out, 0, sizeof(out));
        check(kitty_hello_container_open_recovery(c, pass, out) == 1 &&
              memcmp(out, secret, sizeof(secret)) == 0,
              "recovery passphrase opens it, secret round-trips");

        check(kitty_hello_container_open_kek(c, wrong_kek, out) == -1,
              "wrong KEK is refused");
        check(kitty_hello_container_open_recovery(c, "wrong", out) == -1,
              "wrong recovery passphrase is refused");

        /* Tamper with one byte inside the hello blob's b64 (the field
         * body starts right after "HELLOK1:H"; this lands in the nonce,
         * which feeds the tag just as the ciphertext does). */
        {
            char *t = dupstr(c);
            char *p = t + 9 + 4;
            *p = (*p == 'A') ? 'B' : 'A';
            check(kitty_hello_container_open_kek(t, kek, out) == -1,
                  "tampered hello blob is refused");
            sfree(t);
        }
        sfree(c);
    }

    /* Recovery-only (the kittygen-cli / headless shape). */
    c = kitty_hello_container_create(NULL, pass, secret);
    check(c != NULL, "recovery-only container created");
    if (c) {
        check(!kitty_hello_container_has_hello(c), "no hello wrap in it");
        check(kitty_hello_container_open_kek(c, kek, out) == 0,
              "hello open reports the wrap ABSENT (0), not refused");
        memset(out, 0, sizeof(out));
        check(kitty_hello_container_open_recovery(c, pass, out) == 1 &&
              memcmp(out, secret, sizeof(secret)) == 0,
              "recovery-only round-trips");
        sfree(c);
    }

    /* Hello-only (the deliberate, warned no-recovery choice). */
    c = kitty_hello_container_create(kek, NULL, secret);
    check(c != NULL, "hello-only container created");
    if (c) {
        check(!kitty_hello_container_has_recovery(c), "no recovery wrap in it");
        check(kitty_hello_container_open_recovery(c, pass, out) == 0,
              "recovery open reports the wrap ABSENT (0), not refused");
        memset(out, 0, sizeof(out));
        check(kitty_hello_container_open_kek(c, kek, out) == 1 &&
              memcmp(out, secret, sizeof(secret)) == 0,
              "hello-only round-trips");
        sfree(c);
    }

    /* No wrap at all is not a container. */
    check(kitty_hello_container_create(NULL, NULL, secret) == NULL,
          "refuses to create a container with no wrap");
    check(!kitty_hello_container_valid("HELLOK1:"), "empty container invalid");
    check(!kitty_hello_container_valid("MPW2:abc"), "foreign marker invalid");
    check(!kitty_hello_container_valid(NULL), "NULL invalid");

    /* A recorded Argon2 cost the validator rejects must be refused before
     * the KDF runs - same rule PuTTY 0.85 applies to PPK parameters. A
     * memory cost below 8x parallelism is illegal; splice one in. */
    {
        unsigned char salt[16] = {0};
        strbuf *b64salt = base64_encode_sb(make_ptrlen(salt, 16), 0);
        unsigned char blob[12 + KITTY_HELLO_SECRET_LEN + 16] = {0};
        strbuf *b64blob = base64_encode_sb(make_ptrlen(blob, sizeof(blob)), 0);
        char *bad = dupprintf("HELLOK1:R8,1,4,%s,%s", b64salt->s, b64blob->s);
        check(kitty_hello_container_open_recovery(bad, pass, out) == -1,
              "illegal recorded Argon2 cost is refused");
        sfree(bad);
        strbuf_free(b64salt);
        strbuf_free(b64blob);
    }

    /* Unknown extra fields are ignored (room for HELLOK1 to grow). */
    c = kitty_hello_container_create(kek, NULL, secret);
    if (c) {
        char *ext = dupcat(c, ".Xfuture");
        memset(out, 0, sizeof(out));
        check(kitty_hello_container_open_kek(ext, kek, out) == 1 &&
              memcmp(out, secret, sizeof(secret)) == 0,
              "unknown extra field is ignored");
        sfree(ext);
        sfree(c);
    }

    /* The PRF (W) wrap: fixed KEK + a dummy credential id. */
    {
        unsigned char credid[48];
        unsigned char *gotid = NULL;
        size_t gotlen = 0;
        for (i = 0; i < sizeof(credid); i++)
            credid[i] = (unsigned char)(i * 3 + 5);

        c = kitty_hello_container_create_ex(NULL, kek, credid,
                                            sizeof(credid), "DOM\\alice@PC1",
                                            pass, secret);
        check(c != NULL, "PRF+recovery container created");
        if (c) {
            check(kitty_hello_container_has_prf(c), "W wrap present");
            check(!kitty_hello_container_has_hello(c), "no H wrap in it");
            check(kitty_hello_container_valid(c), "W-carrying container valid");
            check(kitty_hello_container_prf_credid(c, &gotid, &gotlen) == 1 &&
                  gotlen == sizeof(credid) &&
                  memcmp(gotid, credid, gotlen) == 0,
                  "credential id round-trips through the W field");
            sfree(gotid);
            memset(out, 0, sizeof(out));
            check(kitty_hello_container_open_prf(c, kek, out) == 1 &&
                  memcmp(out, secret, sizeof(secret)) == 0,
                  "PRF KEK opens the W wrap, secret round-trips");
            check(kitty_hello_container_open_prf(c, wrong_kek, out) == -1,
                  "wrong PRF KEK is refused");
            check(kitty_hello_container_open_kek(c, kek, out) == 0,
                  "H open reports absent on a W-only container");
            memset(out, 0, sizeof(out));
            check(kitty_hello_container_open_recovery(c, pass, out) == 1 &&
                  memcmp(out, secret, sizeof(secret)) == 0,
                  "recovery still opens beside the W wrap");
            sfree(c);
        }

        /* All three wraps in one container. */
        c = kitty_hello_container_create_ex(kek, wrong_kek, credid,
                                            sizeof(credid), NULL, pass, secret);
        check(c != NULL, "H+W+R container created");
        if (c) {
            check(kitty_hello_container_has_hello(c) &&
                  kitty_hello_container_has_prf(c) &&
                  kitty_hello_container_has_recovery(c),
                  "all three wraps present");
            memset(out, 0, sizeof(out));
            check(kitty_hello_container_open_kek(c, kek, out) == 1 &&
                  kitty_hello_container_open_prf(c, wrong_kek, out) == 1 &&
                  kitty_hello_container_open_recovery(c, pass, out) == 1,
                  "each wrap opens with its own key");
            sfree(c);
        }

        /* A KEK without a credential id (or the reverse) is a caller
         * bug, refused at create. */
        check(kitty_hello_container_create_ex(NULL, kek, NULL, 0, NULL, pass,
                                              secret) == NULL,
              "PRF KEK without credential id refused");

        /* Several accounts, one sidecar: a second W appended for another
         * credential id under its own KEK, the first left byte-identical. */
        c = kitty_hello_container_create_ex(NULL, kek, credid,
                                            sizeof(credid), "DOM\\alice@PC1",
                                            pass, secret);
        if (c) {
            unsigned char credid2[32];
            char *c2, *o, *all;
            for (i = 0; i < sizeof(credid2); i++)
                credid2[i] = (unsigned char)(200 - i);
            check(kitty_hello_container_w_count(c) == 1, "one W to start");
            o = kitty_hello_container_w_owner(c, 0);
            check(o && !strcmp(o, "DOM\\alice@PC1"), "owner tag round-trips");
            sfree(o);
            c2 = kitty_hello_container_append_prf(c, wrong_kek, credid2,
                                                  sizeof(credid2),
                                                  "DOM\\bob@PC2", secret);
            check(c2 != NULL, "second account appended");
            if (c2) {
                check(!strncmp(c2, c, strlen(c)) && c2[strlen(c)] == '.',
                      "existing text kept byte for byte, new W after it");
                check(kitty_hello_container_w_count(c2) == 2, "two W now");
                check(kitty_hello_container_find_w(c2, credid,
                                                   sizeof(credid)) == 0 &&
                      kitty_hello_container_find_w(c2, credid2,
                                                   sizeof(credid2)) == 1 &&
                      kitty_hello_container_find_w(c2, credid2,
                                                   sizeof(credid2) - 1) == -1,
                      "find_w locates each id, refuses a near miss");
                memset(out, 0, sizeof(out));
                check(kitty_hello_container_open_prf(c2, kek, out) == 1 &&
                      memcmp(out, secret, sizeof(secret)) == 0,
                      "first account's KEK still opens");
                memset(out, 0, sizeof(out));
                check(kitty_hello_container_open_prf(c2, wrong_kek, out) == 1 &&
                      memcmp(out, secret, sizeof(secret)) == 0,
                      "second account's KEK opens its own W");
                {
                    unsigned char third[32];
                    memset(third, 0x42, sizeof(third));
                    check(kitty_hello_container_open_prf(c2, third, out) == -1,
                          "a KEK of neither account is refused");
                }
                memset(out, 0, sizeof(out));
                check(kitty_hello_container_open_recovery(c2, pass, out) == 1 &&
                      memcmp(out, secret, sizeof(secret)) == 0,
                      "recovery untouched by the append");
                all = kitty_hello_container_owners_text(c2);
                check(all && !strcmp(all, "DOM\\alice@PC1, DOM\\bob@PC2"),
                      "owners line names both accounts");
                sfree(all);
                check(kitty_hello_container_append_prf("garbage", kek, credid,
                                                       sizeof(credid), NULL,
                                                       secret) == NULL,
                      "append onto a non-container refused");

                /* remove_w: door surgery for the sidecar editor */
                {
                    char *r0 = kitty_hello_container_remove_w(c2, 0);
                    check(r0 != NULL &&
                          kitty_hello_container_w_count(r0) == 1 &&
                          kitty_hello_container_find_w(r0, credid2,
                                                       sizeof(credid2)) == 0 &&
                          kitty_hello_container_has_recovery(r0),
                          "removing W0 keeps W1 and the recovery door");
                    if (r0) {
                        memset(out, 0, sizeof(out));
                        check(kitty_hello_container_open_prf(r0, wrong_kek,
                                                             out) == 1 &&
                              memcmp(out, secret, sizeof(secret)) == 0,
                              "the surviving W still opens after surgery");
                        check(kitty_hello_container_open_prf(r0, kek,
                                                             out) == -1,
                              "the removed W's KEK no longer opens");
                        sfree(r0);
                    }
                    check(kitty_hello_container_remove_w(c2, 5) == NULL,
                          "removing a W that is not there refused");
                }
                sfree(c2);
            }
            sfree(c);
        }

        /* The 1c-era two-part W (no owner tag) still parses. */
        {
            char *c3 = kitty_hello_container_create_ex(NULL, kek, credid,
                                                       sizeof(credid), NULL,
                                                       pass, secret);
            if (c3) {
                check(kitty_hello_container_w_owner(c3, 0) == NULL &&
                      kitty_hello_container_find_w(c3, credid,
                                                   sizeof(credid)) == 0 &&
                      kitty_hello_container_open_prf(c3, kek, out) == 1,
                      "untagged W: no owner, id and open still work");
                {
                    char *wonly = kitty_hello_container_create_ex(
                        NULL, kek, credid, sizeof(credid), NULL, NULL,
                        secret);
                    check(wonly != NULL &&
                          kitty_hello_container_remove_w(wonly, 0) == NULL,
                          "the LAST door can never be removed");
                    sfree(wonly);
                }
                sfree(c3);
            }
        }
    }

    /* The printed secret (fixed encoding: it doubles as the literal PPK
     * passphrase). */
    {
        char *t = kitty_hello_secret_text(secret);
        char *lower, *stripped;
        unsigned char back[KITTY_HELLO_SECRET_LEN];
        size_t j, k;
        check(t != NULL && strlen(t) == 64 + 4 + 8,
              "printed secret has the fixed length (8+1 groups, dashes)");
        check(kitty_hello_secret_from_text(t, back) == 1 &&
              memcmp(back, secret, sizeof(secret)) == 0,
              "printed secret parses back to the same bytes");
        lower = dupstr(t);
        for (j = 0; lower[j]; j++)
            if (lower[j] >= 'A' && lower[j] <= 'F')
                lower[j] += 'a' - 'A';
        check(kitty_hello_secret_from_text(lower, back) == 1,
              "lower-case printed secret accepted");
        stripped = dupstr(t);
        for (j = 0, k = 0; stripped[j]; j++)
            if (stripped[j] != '-')
                stripped[k++] = stripped[j];
        stripped[k] = '\0';
        check(kitty_hello_secret_from_text(stripped, back) == 1,
              "dash-free printed secret accepted");
        t[0] = (t[0] == '0') ? '1' : '0';
        check(kitty_hello_secret_from_text(t, back) == 0,
              "a typo fails the check group");
        check(kitty_hello_secret_from_text("nonsense", back) == 0,
              "garbage refused");
        sfree(t);
        sfree(lower);
        sfree(stripped);
    }


    /* The kageant-side worker, headless parts: sidecar naming and I/O,
     * the protected copy's default name, door translation, and the
     * refusals protect must make before any Hello prompt. */
    {
        const char *keyfile = "hello_worker_test.ppk";
        char *sc = kageant_hello_sidecar_path(keyfile);
        char *d1 = kageant_hello_default_destpath("C:\\keys\\id_ed25519.ppk");
        char *d2 = kageant_hello_default_destpath("C:\\keys\\noext");
        char *d3 = kageant_hello_default_destpath("C:\\my.keys\\id.ppk");
        char *got, *err = NULL, *printed = NULL;
        int via = -1;
        unsigned char credid_dummy[16];
        memset(credid_dummy, 7, sizeof(credid_dummy));

        check(sc && !strcmp(sc, "hello_worker_test.ppk.hello"),
              "sidecar is <keyfile>.hello");
        check(d1 && !strcmp(d1, "C:\\keys\\id_ed25519-hello.ppk"),
              "protected copy defaults to <name>-hello.<ext>");
        check(d2 && !strcmp(d2, "C:\\keys\\noext-hello.ppk"),
              "no extension: -hello.ppk appended");
        check(d3 && !strcmp(d3, "C:\\my.keys\\id-hello.ppk"),
              "a dot in the folder name is not the extension");
        sfree(d1); sfree(d2); sfree(d3);

        DeleteFileA(sc);
        check(!kageant_hello_has_sidecar(keyfile), "no sidecar: not protected");
        check(kageant_hello_read_sidecar(keyfile) == NULL,
              "reading an absent sidecar yields NULL");

        c = kitty_hello_container_create_ex(NULL, kek, credid_dummy,
                                            sizeof(credid_dummy),
                                            "DOM\\me@HERE", pass, secret);
        check(c && kageant_hello_write_sidecar(keyfile, c),
              "sidecar written");
        check(kageant_hello_has_sidecar(keyfile), "sidecar detected");
        got = kageant_hello_read_sidecar(keyfile);
        check(got && !strcmp(got, c), "sidecar reads back byte for byte");
        sfree(got);

        /* translate: recovery passphrase -> printed text; printed text
         * (any form) -> canonical; anything else -> NULL */
        {
            char *canon = kitty_hello_secret_text(secret);
            char *lower = dupstr(canon), *q;
            for (q = lower; *q; q++) {
                if (*q >= 'A' && *q <= 'F') *q += 'a' - 'A';
                if (*q == '-') *q = ' ';
            }
            got = kageant_hello_translate(keyfile, pass, &via);
            check(got && !strcmp(got, canon) && via == 1,
                  "recovery passphrase opens R and yields the printed text");
            burnstr(got);
            got = kageant_hello_translate(keyfile, lower, &via);
            check(got && !strcmp(got, canon) && via == 0,
                  "printed secret typed loosely is canonicalised");
            burnstr(got);
            got = kageant_hello_translate(keyfile, "not a door", &via);
            check(got == NULL, "an unrelated passphrase translates to nothing");
            got = kageant_hello_translate("no_such_key.ppk", pass, &via);
            check(got == NULL, "no sidecar: nothing to translate");
            burnstr(canon);
            sfree(lower);
        }

        /* protect refusals that need no Hello: empty recovery passphrase
         * with an empty source passphrase; destination exists */
        {
            FILE *fp = fopen(keyfile, "wb");   /* a stand-in "key" on disk */
            if (fp) { fputs("not a key", fp); fclose(fp); }
        }
        check(kageant_hello_protect(NULL, keyfile, "", NULL,
                                    "hello_worker_test-hello.ppk",
                                    &printed, &err) == KAGEANT_HELLO_ERROR &&
              err && strstr(err, "recovery passphrase") && !printed,
              "protect refuses an empty recovery passphrase");
        sfree(err); err = NULL;
        check(kageant_hello_protect(NULL, keyfile, NULL, "rec", keyfile,
                                    &printed, &err) == KAGEANT_HELLO_ERROR &&
              err && strstr(err, "already exists") && !printed,
              "protect never overwrites an existing file");
        sfree(err); err = NULL;

        /* enrol: the text must be this key's secret */
        check(kageant_hello_enrol(NULL, keyfile, "wrong", &err) ==
                  KAGEANT_HELLO_ERROR && err,
              "enrol refuses a non-secret");
        sfree(err); err = NULL;

        DeleteFileA(sc);
        DeleteFileA(keyfile);
        sfree(sc);
        sfree(c);
    }
    if (fails) {
        printf("\n%d check(s) failed\n", fails);
        return 1;
    }
    printf("\nTest suite passed\n");
    return 0;
}
