/*
 * kitty_testdoor_settings.c - test builds only: what a program reads for its
 * global settings and where its writers put them.
 *
 * The settings move into one reader and writer:
 * before the move, each program's own readers are recorded per save mode
 * and KiClassName, after it the same again. The two must agree. Each
 * program hands its table (a name, its reader, its writer) to
 * kitty_testdoor_settings() at the point where its settings are readable.
 *
 *   KITTY_TEST_SETTINGS_SET   name=value;name=value - each setter called first
 *   KITTY_TEST_SETTINGS_DUMP  a file: one "name<TAB>value" line per reader,
 *                             "set<TAB>name=value<TAB>rc" per setter call
 *
 * With either variable set the program exits after it (exit code 0): no
 * window, no agent, no connection. A release build compiles none of it.
 */

#include <stdio.h>
#include <string.h>

#include "putty.h"
#include "kitty_buildlabel.h"
#include "kitty_testdoor_settings.h"

#ifdef KITTY_TEST_BUILD_LABEL
/* The variable whole (sfree it), NULL when it is not set. */
static char *kts_env_dup(const char *name)
{
    DWORD n = GetEnvironmentVariableA(name, NULL, 0), got;
    char *v;
    if (n == 0)
        return NULL;
    v = snewn(n + 1, char);
    got = GetEnvironmentVariableA(name, v, n + 1);
    if (got == 0 || got > n) {
        sfree(v);
        return NULL;
    }
    v[got] = '\0';
    return v;
}

static const struct kts_probe *kts_find(const struct kts_probe *p, int n,
                                        const char *name)
{
    int i;
    for (i = 0; i < n; i++)
        if (!_stricmp(p[i].name, name))
            return &p[i];
    return NULL;
}
#endif

void kitty_testdoor_settings(const struct kts_probe *p, int n)
{
#ifdef KITTY_TEST_BUILD_LABEL
    char *sets = kts_env_dup("KITTY_TEST_SETTINGS_SET");
    char *dump = kts_env_dup("KITTY_TEST_SETTINGS_DUMP");
    strbuf *out;
    int i;
    if (!sets && !dump)
        return;
    out = strbuf_new();
    if (sets) {
        char *item = sets;
        while (item && *item) {
            char *next = strchr(item, ';'), *eq;
            const struct kts_probe *pr;
            if (next)
                *next++ = '\0';
            eq = strchr(item, '=');
            if (eq) {
                *eq = '\0';
                pr = kts_find(p, n, item);
                if (pr && pr->set)
                    put_fmt(out, "set\t%s=%s\t%d\n", item, eq + 1,
                                pr->set(eq + 1));
                else
                    put_fmt(out, "set\t%s=%s\tno-setter\n", item, eq + 1);
            }
            item = next;
        }
    }
    for (i = 0; i < n; i++) {
        char *v = p[i].get ? p[i].get() : NULL;
        put_fmt(out, "%s\t%s\n", p[i].name, v ? v : "<null>");
        sfree(v);
    }
    if (dump) {
        FILE *f = fopen(dump, "wb");
        if (f) {
            fwrite(out->s, 1, out->len, f);
            fclose(f);
        }
    }
    strbuf_free(out);
    sfree(sets);
    sfree(dump);
    ExitProcess(0);
#else
    (void)p;
    (void)n;
#endif
}

/* A number as a string to sfree - for the readers that return an int. */
char *kitty_testdoor_int_dup(int v)
{
    return dupprintf("%d", v);
}
