/*
 * test_knock.c - the port-knocking sequence as read by
 * kitty/kitty_knockparse.c: the entry forms, the separators, the port range,
 * the pause and its limit, the entry shown when one is not valid.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "putty.h"
#include "kitty/kitty_knock.h"

const char *appname = "test_knock";

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

static void fail(const char *text, const char *what)
{
    printf("FAIL  [%s]  %s\n", text, what);
    fails++;
}

/* expect: steps written as "T2001 U2002 P1000", or "!entry" for a bad one */
static void check(const char *text, const char *expect, const char *cut)
{
    KnockList l;
    bool ok = knock_parse(text, &l);
    strbuf *got = strbuf_new();
    size_t i;

    if (!ok) {
        if (l.too_many)
            put_fmt(got, "!too many");
        else
            put_fmt(got, "!%s", l.bad ? l.bad : "(null)");
    } else {
        for (i = 0; i < l.n; i++) {
            const KnockStep *s = &l.steps[i];
            if (s->kind == KNOCK_PAUSE)
                put_fmt(got, "%sP%u", i ? " " : "", s->ms);
            else
                put_fmt(got, "%s%c%u", i ? " " : "",
                            s->kind == KNOCK_TCP ? 'T' : 'U', s->port);
        }
    }
    if (strcmp(got->s, expect)) {
        char *msg = dupprintf("got \"%s\", expected \"%s\"", got->s, expect);
        fail(text, msg);
        sfree(msg);
    }
    strbuf_free(got);
    if ((cut == NULL) != (l.cut == NULL) ||
        (cut && l.cut && strcmp(cut, l.cut)))
        fail(text, "the cut pause is not the one expected");
    knock_list_free(&l);
}

int main(void)
{
    /* the documented example and the plain forms */
    check("2001:tcp, 1:s, 2002:udp", "T2001 P1000 U2002", NULL);
    check("7000 8000 9000", "T7000 T8000 T9000", NULL);
    check("", "", NULL);
    check("  ,; ", "", NULL);
    /* every separator, and the spaces around ':' */
    check("1;2,3\t4", "T1 T2 T3 T4", NULL);
    /* '-' is no separator: a range-like entry is refused */
    check("2001-2005", "!2001-2005", NULL);
    check("2001 : UDP , 0.5 : s", "U2001 P500", NULL);
    check("2001:TCP", "T2001", NULL);
    /* the port range: 1-65535, nothing else */
    check("65535", "T65535", NULL);
    check("40000:udp", "U40000", NULL);
    check("0", "!0", NULL);
    check("65536", "!65536", NULL);
    check("70000:tcp", "!70000:tcp", NULL);
    check("123456", "!123456", NULL);
    check("20x1", "!20x1", NULL);
    /* the first bad entry stops the whole sequence */
    check("2001, 2002:sctp, 2003", "!2002:sctp", NULL);
    check("2001:", "!2001:", NULL);
    check(":tcp", "!:tcp", NULL);
    check("2001:tcp:", "!2001:tcp:", NULL);
    check("1:s:", "!1:s:", NULL);
    /* pauses: decimals, the 60 s limit, not valid ones */
    check("0:s", "P0", NULL);
    check("2.25:s", "P2250", NULL);
    check("60:s", "P60000", NULL);
    check("61:s, 2001", "P60000 T2001", "61:s");
    check("99999999999:s", "P60000", "99999999999:s");
    check("1:s 90:s 120:s", "P1000 P60000 P60000", "90:s");
    check(".5:s", "P500", NULL);
    check("x:s", "!x:s", NULL);
    check("1.2.3:s", "!1.2.3:s", NULL);
    check(":s", "!:s", NULL);
    /* a bad entry is shown with its control bytes as \xNN */
    check("2001, 20\x1b[2J01", "!20\\x1B[2J01", NULL);
    check("2001:udp\x7f", "!2001:udp\\x7F", NULL);
    check("2001:\xc3\xa4", "!2001:\\xC3\\xA4", NULL);
    /* at most KNOCK_MAX_STEPS entries */
    {
        strbuf *sb = strbuf_new();
        unsigned j;
        for (j = 0; j < KNOCK_MAX_STEPS; j++)
            put_fmt(sb, "%u,", 1000 + j);
        {
            KnockList l;
            if (!knock_parse(sb->s, &l) || l.n != KNOCK_MAX_STEPS)
                fail("64 entries", "not taken");
            knock_list_free(&l);
        }
        put_fmt(sb, "2000");
        check(sb->s, "!too many", NULL);
        strbuf_free(sb);
    }

    if (fails) {
        printf("%d check(s) failed\n", fails);
        return 1;
    }
    printf("All tests passed\n");
    return 0;
}
