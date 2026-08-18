/*
 * The master password's Argon2id cost must stay inside the limits PuTTY
 * enforces for PPK files.
 *
 * KiTTY derives the master-password key with fixed, compile-time Argon2
 * parameters (kitty/kitty_mpw.h). Nothing user-supplied reaches the KDF, so
 * unlike a PPK save there is nothing to validate at run time - which is exactly
 * why a bad combination would go unnoticed until master passwords stopped
 * working. PuTTY 0.85 added argon2_params_bad(); this asks it about OUR numbers,
 * so tuning them (less memory, more parallelism) fails the build's test run
 * rather than the user's next unlock.
 *
 * It also checks the two nearby assumptions the envelope relies on: the derived
 * material is big enough to be split into an AES-256 key and an HMAC-SHA-256
 * key, and the salt is a sane length.
 */
#include <stdio.h>
#include <string.h>
#include "putty.h"
#include "ssh.h"
#include "kitty/kitty_mpw.h"

const char *appname = "test_mpw_params";

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

static int fails = 0;

static void check(bool cond, const char *what)
{
    printf("%-58s %s\n", what, cond ? "ok" : "FAILED");
    if (!cond)
        fails++;
}

int main(void)
{
    /*
     * The tag length argon2 is asked for is the whole derived block, and the
     * passphrase/salt lengths are the ones the envelope actually uses: a
     * realistic passphrase and KITTY_MPW_SALT_LEN of salt. The zeroes are the
     * optional secret and associated data, which the master password does not
     * use.
     */
    char *err = argon2_params_bad(KITTY_MPW_ARGON_MEM,
                                  KITTY_MPW_ARGON_PASSES,
                                  KITTY_MPW_ARGON_PARALLEL,
                                  KITTY_MPW_DERIVED_LEN,
                                  /* passphrase */ 16,
                                  KITTY_MPW_SALT_LEN, 0, 0);
    if (err) {
        printf("%-58s FAILED: %s\n",
               "master-password Argon2 parameters are legal", err);
        sfree(err);
        fails++;
    } else {
        printf("%-58s ok\n", "master-password Argon2 parameters are legal");
    }

    /* The same numbers, spelled out, so a failure says WHICH rule broke rather
     * than only that something did. */
    check(KITTY_MPW_ARGON_PARALLEL >= 1, "parallelism is at least 1");
    check(KITTY_MPW_ARGON_PASSES >= 1, "passes is at least 1");
    check(KITTY_MPW_ARGON_MEM >= 8 * KITTY_MPW_ARGON_PARALLEL,
          "memory is at least 8 x parallelism");

    /* What the envelope splits the output into (kitty_mpw.c): AES-256 key and
     * an HMAC-SHA-256 key. */
    check(KITTY_MPW_DERIVED_LEN >= 32 + 32,
          "derived material covers an AES-256 key and a MAC key");
    check(KITTY_MPW_SALT_LEN >= 16, "salt is at least 16 bytes");

    if (fails) {
        printf("\n%d check(s) failed\n", fails);
        return 1;
    }
    printf("\nTest suite passed\n");
    return 0;
}
