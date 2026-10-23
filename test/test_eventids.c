/*
 * test_eventids - checks the suite's event ID table (kitty/kitty_eventids.h):
 * every ID inside its block and listed once, in order; each block's own
 * source; every record but ID 0 with a kind and fields; and the 2xx/3xx
 * pairing - an offset below KITTY_EVO_SHARED_END is the same kind with the
 * same fields in both blocks, and exists in both. (From 30 on the blocks
 * differ: 230 is kitty.exe's alone, 330/331 the command-line tools'.)
 * tools/check-kitty-events.py checks the table against the .mc.
 *
 * Returns non-zero on any failure.
 */
#include <stdio.h>
#include <string.h>

#define KITTY_EVENTIDS_TABLE
#define KITTY_EVENTFIELDS_TABLE
#include "../kitty/kitty_eventids.h"

static int failures = 0;

#define WORDS_MAX_CHECK 16   /* fields per record; kitty_winevent.c takes 16 */
#define NIDS (sizeof(kitty_eventids) / sizeof(kitty_eventids[0]))

static void fail(const char *what, unsigned id)
{
    printf("FAIL [%u]: %s\n", id, what);
    failures++;
}

static const char *block_source(unsigned block)
{
    switch (block) {
      case KITTY_EVB_TEXT:     return KITTY_EVSRC_SUITE;
      case KITTY_EVB_AGENT:    return KITTY_EVSRC_AGENT;
      case KITTY_EVB_KITTY:    return KITTY_EVSRC_KITTY;
      case KITTY_EVB_CLI:      return KITTY_EVSRC_CLI;
      case KITTY_EVB_KITTYGEN: return KITTY_EVSRC_KITTYGEN;
      case KITTY_EVB_SUITE:    return KITTY_EVSRC_SUITE;
    }
    return NULL;
}

static const struct kitty_eventid *find(unsigned id)
{
    size_t i;
    for (i = 0; i < NIDS; i++)
        if (kitty_eventids[i].id == id)
            return &kitty_eventids[i];
    return NULL;
}

/* "a b c": every name [a-z] only, once, single spaces, no "pid" twice */
static int fields_ok(const char *f)
{
    char seen[WORDS_MAX_CHECK][32];
    int n = 0, i;
    while (*f) {
        char name[32];
        int len = 0;
        while (*f && *f != ' ') {
            if (*f < 'a' || *f > 'z' || len + 1 >= (int)sizeof(name))
                return 0;
            name[len++] = *f++;
        }
        name[len] = '\0';
        if (!len)
            return 0;
        for (i = 0; i < n; i++)
            if (!strcmp(seen[i], name))
                return 0;
        if (n >= WORDS_MAX_CHECK)
            return 0;
        strcpy(seen[n++], name);
        if (*f == ' ') {
            f++;
            if (!*f || *f == ' ')
                return 0;
        }
    }
    return n > 0;
}

int main(void)
{
    size_t i;
    unsigned prev = 0;

    if (NIDS < 2)
        fail("table empty", 0);
    for (i = 0; i < NIDS; i++) {
        const struct kitty_eventid *e = &kitty_eventids[i];
        const char *src = block_source(e->block);
        if (i && e->id <= prev)
            fail("IDs not ascending (or listed twice)", e->id);
        prev = e->id;
        if (!src)
            fail("unknown block", e->id);
        else if (strcmp(e->source, src))
            fail("source is not its block's", e->id);
        if (e->id < e->block || e->id > e->block + 99)
            fail("ID outside its block", e->id);
        if (!e->level || !e->level[0])
            fail("no level (the manual's appendix row)", e->id);
        if (e->id == 0) {
            if (e->kind || e->fields[0])
                fail("ID 0 carries a kind or fields", e->id);
            continue;
        }
        if (e->block == KITTY_EVB_TEXT)
            fail("only ID 0 is text only", e->id);
        if (!e->kind || !e->kind[0])
            fail("no kind", e->id);
        if (!fields_ok(e->fields))
            fail("field list malformed", e->id);
        /* the 2xx/3xx pairing */
        if ((e->block == KITTY_EVB_KITTY || e->block == KITTY_EVB_CLI) &&
            e->id - e->block < KITTY_EVO_SHARED_END) {
            unsigned other = e->block == KITTY_EVB_KITTY ?
                e->id + (KITTY_EVB_CLI - KITTY_EVB_KITTY) :
                e->id - (KITTY_EVB_CLI - KITTY_EVB_KITTY);
            const struct kitty_eventid *o = find(other);
            if (!o)
                fail("shared offset missing in the other session block",
                     e->id);
            else if (!e->kind || !o->kind || strcmp(e->kind, o->kind) ||
                     strcmp(e->fields, o->fields))
                fail("shared offset differs from the other session block",
                     e->id);
        }
    }

    /* the IDs the code reports by name are in the table */
    {
        static const unsigned named[] = {
            KITTY_EVB_KITTY + KITTY_EVO_SESSION_OPEN,
            KITTY_EVB_KITTY + KITTY_EVO_SESSION_CLOSE,
            KITTY_EVB_KITTY + KITTY_EVO_HOSTKEY_KNOWN,
            KITTY_EVB_KITTY + KITTY_EVO_HOSTKEY_NEW,
            KITTY_EVB_KITTY + KITTY_EVO_HOSTKEY_MISMATCH,
            KITTY_EVB_KITTY + KITTY_EVO_AUTH_OK,
            KITTY_EVB_KITTY + KITTY_EVO_AUTH_FAILED,
            KITTY_EVB_KITTY + KITTY_EVO_PROXY_USED,
            KITTY_EVB_KITTY + KITTY_EVO_FAREND_REFUSED,
            KITTY_EVB_KITTY + KITTY_EVO_TERM_TRANSFER_DONE,
            KITTY_EVB_KITTY + KITTY_EVO_TERM_TRANSFER_FAILED,
            KITTY_EVB_CLI + KITTY_EVO_SESSION_OPEN,
            KITTY_EVB_CLI + KITTY_EVO_SESSION_CLOSE,
            KITTY_EVB_CLI + KITTY_EVO_HOSTKEY_KNOWN,
            KITTY_EVB_CLI + KITTY_EVO_HOSTKEY_NEW,
            KITTY_EVB_CLI + KITTY_EVO_HOSTKEY_MISMATCH,
            KITTY_EVB_CLI + KITTY_EVO_AUTH_OK,
            KITTY_EVB_CLI + KITTY_EVO_AUTH_FAILED,
            KITTY_EVB_CLI + KITTY_EVO_PROXY_USED,
            KITTY_EVB_CLI + KITTY_EVO_TRANSFER_DONE,
            KITTY_EVB_CLI + KITTY_EVO_TRANSFER_FAILED,
            KITTY_EVB_KITTYGEN + KITTY_EVO_KEY_GENERATED,
            KITTY_EVB_KITTYGEN + KITTY_EVO_PRIVATE_SAVED,
            KITTY_EVB_KITTYGEN + KITTY_EVO_PUBLIC_SAVED,
            KITTY_EVB_KITTYGEN + KITTY_EVO_PASSPHRASE_CHANGED,
            KITTY_EV_PROGRAM_REFUSED,
            KITTY_EV_LOG_RETENTION,
            KITTY_EV_MISSING_FEATURES,
        };
        for (i = 0; i < sizeof(named) / sizeof(named[0]); i++)
            if (!find(named[i]))
                fail("named in the header, missing in the table", named[i]);
    }

    /* the 9xx records carry no pid: they are written before a session or
     * by a guard that names the program itself */
    for (i = 0; i < NIDS; i++)
        if (kitty_eventids[i].block == KITTY_EVB_SUITE &&
            strstr(kitty_eventids[i].fields, "pid"))
            fail("a suite record lists pid", kitty_eventids[i].id);

    /* the field glossary: one entry per field an ID uses, none unused,
     * in name order, each with a text */
    {
        size_t f, nf = sizeof(kitty_eventfields) / sizeof(kitty_eventfields[0]);
        for (f = 0; f < nf; f++) {
            const char *name = kitty_eventfields[f].name;
            size_t len = strlen(name);
            int used = 0;
            if (!kitty_eventfields[f].text || !kitty_eventfields[f].text[0])
                fail("a glossary entry without text", (unsigned)f);
            if (f && strcmp(kitty_eventfields[f - 1].name, name) >= 0)
                fail("glossary not in name order (or a name twice)", (unsigned)f);
            for (i = 0; i < NIDS && !used; i++) {
                const char *p = kitty_eventids[i].fields;
                while (*p) {
                    const char *e = strchr(p, ' ');
                    size_t l = e ? (size_t)(e - p) : strlen(p);
                    if (l == len && !strncmp(p, name, len)) {
                        used = 1;
                        break;
                    }
                    p += l;
                    while (*p == ' ')
                        p++;
                }
            }
            if (!used)
                fail("a glossary entry no ID uses", (unsigned)f);
        }
        for (i = 0; i < NIDS; i++) {
            const char *p = kitty_eventids[i].fields;
            while (*p) {
                const char *e = strchr(p, ' ');
                size_t l = e ? (size_t)(e - p) : strlen(p), k;
                int found = 0;
                for (k = 0; k < nf && !found; k++)
                    found = strlen(kitty_eventfields[k].name) == l &&
                        !strncmp(kitty_eventfields[k].name, p, l);
                if (!found)
                    fail("a field without a glossary entry", kitty_eventids[i].id);
                p += l;
                while (*p == ' ')
                    p++;
            }
        }
    }

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("test_eventids: all passed (%u IDs)\n", (unsigned)NIDS);
    return 0;
}
