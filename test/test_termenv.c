/*
 * test_termenv - the merge rule in kitty/kitty_termenv.c: which environment
 * variables a connection sends from the session's own list and the
 * [KiTTY] sendtermenv switch (COLORTERM=truecolor, TERM_PROGRAM=KiTTY++).
 *
 * Returns non-zero on any failure.
 */
#include <stdio.h>
#include <string.h>

#include "../kitty/kitty_termenv.h"

static int failures = 0;

/* want: "NAME=value" entries in order, a leading '+' marking an added
 * default; NULL-terminated. */
static void check(const char *what, const char *const *names,
                  const char *const *values, size_t n,
                  KittyTermEnvMode mode, const char *const *want)
{
    KittyTermEnvVar out[16];
    char got[64];
    size_t count, i, nwant = 0;

    while (want[nwant])
        nwant++;
    memset(out, 0, sizeof(out));
    count = kitty_termenv_merge(names, values, n, mode, out);
    if (count > n + KITTY_TERMENV_EXTRA) {
        printf("FAIL [%s]: %u entries, room for %u\n", what,
               (unsigned)count, (unsigned)(n + KITTY_TERMENV_EXTRA));
        failures++;
        return;
    }
    if (count != nwant) {
        printf("FAIL [%s]: %u entries, want %u\n", what,
               (unsigned)count, (unsigned)nwant);
        failures++;
    }
    for (i = 0; i < count && i < nwant; i++) {
        snprintf(got, sizeof(got), "%s%s=%s", out[i].added ? "+" : "",
                 out[i].name, out[i].value);
        if (strcmp(got, want[i])) {
            printf("FAIL [%s]: entry %u is %s, want %s\n", what,
                   (unsigned)i, got, want[i]);
            failures++;
        }
    }
}

static int switch_on(void) { return 1; }
static int switch_off(void) { return 0; }

int main(void)
{
    /* no session variables */
    {
        const char *const *none = NULL;
        const char *const want_on[] = { "+COLORTERM=truecolor",
                                        "+TERM_PROGRAM=KiTTY++", NULL };
        const char *const want_empty[] = { NULL };
        check("empty, switch on", none, none, 0, KITTY_TERMENV_DEFAULTS,
              want_on);
        check("empty, switch off", none, none, 0, KITTY_TERMENV_NO_DEFAULTS,
              want_empty);
        check("empty, not KiTTY++", none, none, 0, KITTY_TERMENV_AS_IS,
              want_empty);
    }

    /* other variables are untouched, the defaults go after them */
    {
        const char *const names[] = { "LANG", "EMPTY", "KITTY_WINDOW" };
        const char *const values[] = { "C.UTF-8", "", "db-2" };
        const char *const want_on[] = { "LANG=C.UTF-8", "EMPTY=",
                                        "KITTY_WINDOW=db-2",
                                        "+COLORTERM=truecolor",
                                        "+TERM_PROGRAM=KiTTY++", NULL };
        const char *const want_off[] = { "LANG=C.UTF-8", "EMPTY=",
                                         "KITTY_WINDOW=db-2", NULL };
        check("others, switch on", names, values, 3, KITTY_TERMENV_DEFAULTS,
              want_on);
        check("others, switch off", names, values, 3,
              KITTY_TERMENV_NO_DEFAULTS, want_off);
        check("others, not KiTTY++", names, values, 3, KITTY_TERMENV_AS_IS,
              want_off);
    }

    /* the session's own value wins, and is not marked as added */
    {
        const char *const names[] = { "TERM_PROGRAM", "LANG", "COLORTERM" };
        const char *const values[] = { "tmux", "C", "24bit" };
        const char *const want[] = { "TERM_PROGRAM=tmux", "LANG=C",
                                     "COLORTERM=24bit", NULL };
        check("override both, switch on", names, values, 3,
              KITTY_TERMENV_DEFAULTS, want);
        check("override both, switch off", names, values, 3,
              KITTY_TERMENV_NO_DEFAULTS, want);
    }
    {
        const char *const names[] = { "COLORTERM" };
        const char *const values[] = { "256color" };
        const char *const want[] = { "COLORTERM=256color",
                                     "+TERM_PROGRAM=KiTTY++", NULL };
        check("override one", names, values, 1, KITTY_TERMENV_DEFAULTS,
              want);
    }

    /* an empty value suppresses that variable, default or not */
    {
        const char *const names[] = { "COLORTERM", "LANG" };
        const char *const values[] = { "", "C" };
        const char *const want_on[] = { "LANG=C", "+TERM_PROGRAM=KiTTY++",
                                        NULL };
        const char *const want_off[] = { "LANG=C", NULL };
        const char *const want_asis[] = { "COLORTERM=", "LANG=C", NULL };
        check("empty COLORTERM, switch on", names, values, 2,
              KITTY_TERMENV_DEFAULTS, want_on);
        check("empty COLORTERM, switch off", names, values, 2,
              KITTY_TERMENV_NO_DEFAULTS, want_off);
        check("empty COLORTERM, not KiTTY++", names, values, 2,
              KITTY_TERMENV_AS_IS, want_asis);
    }
    {
        const char *const names[] = { "TERM_PROGRAM", "COLORTERM" };
        const char *const values[] = { "", "" };
        const char *const want[] = { NULL };
        check("both empty, switch on", names, values, 2,
              KITTY_TERMENV_DEFAULTS, want);
    }

    /* names match exactly: a differently cased name is another variable */
    {
        const char *const names[] = { "colorterm" };
        const char *const values[] = { "" };
        const char *const want[] = { "colorterm=", "+COLORTERM=truecolor",
                                     "+TERM_PROGRAM=KiTTY++", NULL };
        check("lower-case name", names, values, 1, KITTY_TERMENV_DEFAULTS,
              want);
    }

    /* the switch hook */
    if (kitty_termenv_mode() != KITTY_TERMENV_AS_IS) {
        printf("FAIL [mode]: no hook should be AS_IS\n");
        failures++;
    }
    kitty_termenv_switch_hook = switch_on;
    if (kitty_termenv_mode() != KITTY_TERMENV_DEFAULTS) {
        printf("FAIL [mode]: switch on should be DEFAULTS\n");
        failures++;
    }
    kitty_termenv_switch_hook = switch_off;
    if (kitty_termenv_mode() != KITTY_TERMENV_NO_DEFAULTS) {
        printf("FAIL [mode]: switch off should be NO_DEFAULTS\n");
        failures++;
    }
    kitty_termenv_switch_hook = NULL;

    if (failures) {
        printf("test_termenv: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_termenv: all passed\n");
    return 0;
}
