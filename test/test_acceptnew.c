/*
 * test_acceptnew - regression tests for kitty/kitty_hostkey_acceptnew.c:
 * which host keys accept-new stores without a question (an unknown plain key
 * only - never a changed key, never a certificate, never while off), the
 * kitty.ini switch, the record's second destination, and the record's text.
 *
 * Returns non-zero on any failure.
 */
#include <stdio.h>
#include <string.h>

#include "../kitty/kitty_hostkey_acceptnew.h"
#include "../kitty/kitty_text.h"

static int failures = 0;

static void expect(const char *what, bool got, bool want)
{
    if (got != want) {
        printf("FAIL [%s]: got %d, want %d\n", what, (int)got, (int)want);
        failures++;
    }
}

static char reported[512];
static int nreports = 0;
static void capture(const char *line)
{
    snprintf(reported, sizeof(reported), "%s", line);
    nreports++;
}

int main(void)
{
    /* the decision: enabled x storage status x certificate */
    expect("on, unknown key: stored", kitty_hostkey_accept_new_applies(true, 1, false), true);
    expect("on, stored key: nothing to do", kitty_hostkey_accept_new_applies(true, 0, false), false);
    expect("on, CHANGED key: never", kitty_hostkey_accept_new_applies(true, 2, false), false);
    expect("on, unknown certificate: never", kitty_hostkey_accept_new_applies(true, 1, true), false);
    expect("on, changed certificate: never", kitty_hostkey_accept_new_applies(true, 2, true), false);
    expect("off, unknown key: the question", kitty_hostkey_accept_new_applies(false, 1, false), false);
    expect("off, changed key: the question", kitty_hostkey_accept_new_applies(false, 2, false), false);
    expect("off, stored key", kitty_hostkey_accept_new_applies(false, 0, false), false);

    /* the kitty.ini switch: off until set, any non-zero is on */
    expect("autostoresshkey off by default", kitty_hostkey_accept_new_all(), false);
    kitty_hostkey_accept_new_set_all(1);
    expect("autostoresshkey=yes", kitty_hostkey_accept_new_all(), true);
    kitty_hostkey_accept_new_set_all(0);
    expect("autostoresshkey=no", kitty_hostkey_accept_new_all(), false);

    /* the record: nowhere but the Event Log until a tool sets a destination */
    kitty_hostkey_accept_new_report("unseen");
    expect("no destination: nothing reported", nreports == 0, true);
    kitty_hostkey_accept_new_set_report(capture);
    {
        char line[512];
        snprintf(line, sizeof(line), KT_HOSTKEY_ACCEPTNEW_STORED,
                 "ssh-ed25519 255 SHA256:abcdefghijklmnopqrstuvwxyz0123456789ABCDEFG",
                 "srv.example", 2222);
        kitty_hostkey_accept_new_report(line);
    }
    expect("the destination gets the line once", nreports == 1, true);
    if (strcmp(reported, "Host key stored (accept-new): ssh-ed25519 255 "
               "SHA256:abcdefghijklmnopqrstuvwxyz0123456789ABCDEFG, "
               "srv.example port 2222")) {
        printf("FAIL [record text]: <%s>\n", reported);
        failures++;
    }
    kitty_hostkey_accept_new_set_report(NULL);
    kitty_hostkey_accept_new_report("unseen");
    expect("destination removed: nothing more", nreports == 1, true);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("test_acceptnew: all passed\n");
    return 0;
}
