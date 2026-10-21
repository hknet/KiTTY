/*
 * test_auditparse - regression tests for kitty/kitty_auditparse.c: which
 * agent-log lines are read as the old backslash-escaped form, the
 * conversion of an old line, and the field reader on current lines (paths
 * with single backslashes, UNC paths, doubled quotes).
 *
 * Returns non-zero on any failure.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../kitty/kitty_auditparse.h"

static int failures = 0;

static void check_old(const char *line, int want)
{
    int got = kitty_auditparse_is_old(line);
    if (got != want) {
        printf("FAIL [is_old]: got %d, want %d for <%s>\n", got, want, line);
        failures++;
    }
}

static void check_field(const char *line, const char *key, const char *want)
{
    char buf[512];
    int have = kitty_auditparse_field(line, key, buf, sizeof(buf));
    if (!want) {
        if (have) {
            printf("FAIL [field %s]: present <%s>, want absent in <%s>\n",
                   key, buf, line);
            failures++;
        }
        return;
    }
    if (!have || strcmp(buf, want)) {
        printf("FAIL [field %s]: got <%s>, want <%s> in <%s>\n",
               key, have ? buf : "(absent)", want, line);
        failures++;
    }
}

/* An old line: recognised, converted, and its fields read back. */
static void check_old_line(const char *line, const char *want_conv,
                           const char *key, const char *want)
{
    char *conv;
    check_old(line, 1);
    conv = kitty_auditparse_from_old(line);
    if (!conv || strcmp(conv, want_conv)) {
        printf("FAIL [from_old]: got <%s>, want <%s>\n",
               conv ? conv : "(null)", want_conv);
        failures++;
    }
    if (conv) {
        check_old(conv, 0);
        check_field(conv, key, want);
    }
    free(conv);
}

int main(void)
{
    /* Current lines: a backslash is itself. */
    static const char cur_path[] =
        "ts=2026-10-21T10:00:00Z ev=\"loadkey\" "
        "path=\"C:\\new\\keys.ppk\" result=\"loaded\"";
    static const char cur_unc[] =
        "ts=2026-10-21T10:00:00Z ev=\"loadkey\" "
        "path=\"\\\\server\\share\\k.ppk\" result=\"loaded\"";
    static const char cur_quote[] =
        "ts=2026-10-21T10:00:00Z ev=\"add\" "
        "comment=\"x\"\" result=\"\"allowed\"\" forged=\"\"yes\\\" "
        "result=\"done\"";
    static const char cur_nl[] =
        "ts=2026-10-21T10:00:00Z ev=\"sign\" "
        "reqpath=\"C:\\Windows\\System32\\OpenSSH\\ssh-keygen.exe\" "
        "pid=\"42\"";

    check_old(cur_path, 0);
    check_field(cur_path, "path", "C:\\new\\keys.ppk");
    check_field(cur_path, "result", "loaded");
    check_field(cur_path, "ts", "2026-10-21T10:00:00Z");

    check_old(cur_unc, 0);
    check_field(cur_unc, "path", "\\\\server\\share\\k.ppk");

    check_old(cur_quote, 0);
    check_field(cur_quote, "comment", "x\" result=\"allowed\" forged=\"yes\\");
    check_field(cur_quote, "forged", NULL);
    check_field(cur_quote, "result", "done");

    check_old(cur_nl, 0);
    check_field(cur_nl, "reqpath",
                "C:\\Windows\\System32\\OpenSSH\\ssh-keygen.exe");
    check_field(cur_nl, "pid", "42");
    check_field(cur_nl, "comment", NULL);

    /* Old lines: doubled backslashes, \" and \n escapes. */
    check_old_line(
        "ts=2026-08-22T12:00:00Z ev=\"sign\" "
        "reqpath=\"C:\\\\Windows\\\\System32\\\\OpenSSH\\\\ssh-add.exe\" "
        "comment=\"a\\\"b\"",
        "ts=2026-08-22T12:00:00Z ev=\"sign\" "
        "reqpath=\"C:\\Windows\\System32\\OpenSSH\\ssh-add.exe\" "
        "comment=\"a\"\"b\"",
        "reqpath", "C:\\Windows\\System32\\OpenSSH\\ssh-add.exe");
    check_old_line(
        "ts=2026-08-22T12:00:00Z ev=\"loadkey\" "
        "path=\"\\\\\\\\srv\\\\share\\\\k.ppk\" result=\"loaded\"",
        "ts=2026-08-22T12:00:00Z ev=\"loadkey\" "
        "path=\"\\\\srv\\share\\k.ppk\" result=\"loaded\"",
        "path", "\\\\srv\\share\\k.ppk");
    check_old_line(
        "ts=2026-08-22T12:00:00Z ev=\"add\" "
        "path=\"C:\\\\keys\\\\a.ppk\" comment=\"line1\\nline2\"",
        "ts=2026-08-22T12:00:00Z ev=\"add\" "
        "path=\"C:\\keys\\a.ppk\" comment=\"line1?line2\"",
        "comment", "line1?line2");

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("test_auditparse: all passed\n");
    return 0;
}
