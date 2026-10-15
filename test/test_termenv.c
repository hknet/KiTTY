/*
 * test_termenv - the merge rule in kitty/kitty_termenv.c: which environment
 * variables a connection sends from the session's own list and the
 * [KiTTY] sendtermenv switch (COLORTERM=truecolor, TERM_PROGRAM=KiTTY++);
 * and, when a server refuses those defaults, the host the one NOTE is
 * remembered for and the once-per-host rule (on an in-memory store); and
 * the line naming the session's own refused variables.
 *
 * Returns non-zero on any failure.
 */
#include <stdio.h>
#include <string.h>

#include "../kitty/kitty_termenv.h"
#include "../kitty/kitty_text.h"

static int failures = 0;

/* The session's ANSI colour && 24-bit colour, as kitty_termenv_list_new
 * passes it; on unless a case says otherwise. */
static bool truecolour = true;

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
    count = kitty_termenv_merge(names, values, n, mode, truecolour, out);
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

static void check_id(const char *host, int port, size_t size,
                     const char *want)
{
    char buf[KITTY_TERMENV_HOST_ID_MAX];
    size_t len;

    memset(buf, 'x', sizeof(buf));
    len = kitty_termenv_host_id(host, port, buf, size);
    if (!want) {
        if (len) {
            printf("FAIL [host id %s:%d in %u]: %s, want none\n",
                   host ? host : "(null)", port, (unsigned)size, buf);
            failures++;
        }
        return;
    }
    if (len != strlen(want) || strcmp(buf, want)) {
        printf("FAIL [host id %s:%d]: %s (%u), want %s\n",
               host ? host : "(null)", port, len ? buf : "(none)",
               (unsigned)len, want);
        failures++;
    }
}

#define NOTE_HEAD "\r\n\x1b[1;33mNOTE:\x1b[0m "

/* want NULL: nothing of the session's own refused (length 0). */
static void check_own(const char *what, const char *const *names,
                      const bool *added, const bool *refused, size_t n,
                      const char *want)
{
    char buf[512], small[24];
    size_t len, len0;

    memset(buf, 'x', sizeof(buf));
    len = kitty_termenv_refused_own_note(names, added, refused, n, true, buf,
                                         sizeof(buf));
    len0 = kitty_termenv_refused_own_note(names, added, refused, n, true,
                                          NULL, 0);
    if (len0 != len) {
        printf("FAIL [%s]: length %u without a buffer, %u with\n", what,
               (unsigned)len0, (unsigned)len);
        failures++;
    }
    if (!want) {
        if (len || buf[0]) {
            printf("FAIL [%s]: a line, want none\n", what);
            failures++;
        }
        return;
    }
    if (len != strlen(want) || strcmp(buf, want)) {
        printf("FAIL [%s]: got \"%s\" (%u), want \"%s\"\n", what, buf,
               (unsigned)len, want);
        failures++;
    }
    /* a short buffer: cut, terminated, the full length still returned */
    memset(small, 'x', sizeof(small));
    if (kitty_termenv_refused_own_note(names, added, refused, n, true, small,
                                       sizeof(small)) != len ||
        strlen(small) != sizeof(small) - 1 ||
        strncmp(small, want, sizeof(small) - 1)) {
        printf("FAIL [%s]: short buffer not cut cleanly\n", what);
        failures++;
    }
    /* the plain form (klink / kscp / ksftp): the same line, no colour
     * sequences, "NOTE:" right after the line break */
    {
        char plain_want[512], plain[512];
        size_t plen;
        if (strncmp(want, NOTE_HEAD, strlen(NOTE_HEAD))) {
            printf("FAIL [%s]: want does not start with the NOTE head\n",
                   what);
            failures++;
            return;
        }
        snprintf(plain_want, sizeof(plain_want), "\r\nNOTE: %s",
                 want + strlen(NOTE_HEAD));
        plen = kitty_termenv_refused_own_note(names, added, refused, n,
                                              false, plain, sizeof(plain));
        if (plen != strlen(plain_want) || strcmp(plain, plain_want) ||
            strchr(plain, '\x1b')) {
            printf("FAIL [%s] plain: got \"%s\" (%u), want \"%s\"\n", what,
                   plain, (unsigned)plen, plain_want);
            failures++;
        }
    }
}

/* The store under kitty_termenv_hint_due: a list of marked hosts. */
typedef struct MemStore {
    char marked[8][64];
    int n;
    int marks;       /* mark() calls */
    bool mark_fails; /* a store that cannot be written */
} MemStore;

static bool mem_said(void *ctx, const char *id)
{
    MemStore *m = (MemStore *)ctx;
    int i;
    for (i = 0; i < m->n; i++)
        if (!strcmp(m->marked[i], id))
            return true;
    return false;
}

static bool mem_mark(void *ctx, const char *id)
{
    MemStore *m = (MemStore *)ctx;
    m->marks++;
    if (m->mark_fails || m->n >= 8)
        return false;
    snprintf(m->marked[m->n++], sizeof(m->marked[0]), "%s", id);
    return true;
}

static void check_due(const char *what, const KittyTermEnvHintStore *st,
                      const char *id, bool want)
{
    bool got = kitty_termenv_hint_due(st, id);
    if (got != want) {
        printf("FAIL [%s]: due is %d, want %d\n", what, got, want);
        failures++;
    }
}

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

    /* 24-bit colour (or ANSI colour) off: no COLORTERM default, the
     * session's own COLORTERM still goes, TERM_PROGRAM is unaffected */
    truecolour = false;
    {
        const char *const *none = NULL;
        const char *const want[] = { "+TERM_PROGRAM=KiTTY++", NULL };
        const char *const want_empty[] = { NULL };
        check("no true colour, switch on", none, none, 0,
              KITTY_TERMENV_DEFAULTS, want);
        check("no true colour, switch off", none, none, 0,
              KITTY_TERMENV_NO_DEFAULTS, want_empty);
    }
    {
        const char *const names[] = { "COLORTERM", "LANG" };
        const char *const values[] = { "24bit", "C" };
        const char *const want[] = { "COLORTERM=24bit", "LANG=C",
                                     "+TERM_PROGRAM=KiTTY++", NULL };
        check("no true colour, own COLORTERM", names, values, 2,
              KITTY_TERMENV_DEFAULTS, want);
    }
    {
        const char *const names[] = { "LANG" };
        const char *const values[] = { "C" };
        const char *const want[] = { "LANG=C", NULL };
        check("no true colour, not KiTTY++", names, values, 1,
              KITTY_TERMENV_AS_IS, want);
    }
    truecolour = true;

    /* the terminal type sent: "-256color" cut only in a KiTTY++ terminal
     * with 256 colours off, only that exact ending, no '.' in the name */
    {
        static const struct {
            const char *stored;
            KittyTermEnvMode mode;
            bool c256;
            const char *want;
        } tt[] = {
            { "xterm-256color", KITTY_TERMENV_DEFAULTS, false, "xterm" },
            { "xterm-256color", KITTY_TERMENV_NO_DEFAULTS, false, "xterm" },
            { "xterm-256color", KITTY_TERMENV_DEFAULTS, true, "xterm-256color" },
            { "xterm-256color", KITTY_TERMENV_AS_IS, false, "xterm-256color" },
            { "screen-256color", KITTY_TERMENV_DEFAULTS, false, "screen" },
            { "rxvt-unicode-256color", KITTY_TERMENV_DEFAULTS, false, "rxvt-unicode" },
            { "screen.xterm-256color", KITTY_TERMENV_DEFAULTS, false, "screen.xterm-256color" },
            { "screen-256color-bce", KITTY_TERMENV_DEFAULTS, false, "screen-256color-bce" },
            { "-256color", KITTY_TERMENV_DEFAULTS, false, "-256color" },
            { "xterm-256COLOR", KITTY_TERMENV_DEFAULTS, false, "xterm-256COLOR" },
            { "xterm", KITTY_TERMENV_DEFAULTS, false, "xterm" },
            { "", KITTY_TERMENV_DEFAULTS, false, "" },
        };
        size_t k;
        for (k = 0; k < sizeof(tt) / sizeof(tt[0]); k++) {
            size_t len = kitty_termtype_len(tt[k].stored, tt[k].mode,
                                            tt[k].c256);
            if (len != strlen(tt[k].want) ||
                strncmp(tt[k].stored, tt[k].want, len)) {
                printf("FAIL [termtype %s, mode %d, 256 %d]: %.*s, want %s\n",
                       tt[k].stored, (int)tt[k].mode, (int)tt[k].c256,
                       (int)len, tt[k].stored, tt[k].want);
                failures++;
            }
        }
        if (kitty_termtype_len(NULL, KITTY_TERMENV_DEFAULTS, false) != 0) {
            printf("FAIL [termtype NULL]: not 0\n");
            failures++;
        }
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

    /* the host a refused default is remembered for: the host-key cache's
     * host, lower case, ":port" unless 22, IPv6 then in brackets */
    check_id("Example.COM", 22, KITTY_TERMENV_HOST_ID_MAX, "example.com");
    check_id("example.com", 2222, KITTY_TERMENV_HOST_ID_MAX,
             "example.com:2222");
    check_id("127.0.0.1", 2222, KITTY_TERMENV_HOST_ID_MAX, "127.0.0.1:2222");
    check_id("::1", 22, KITTY_TERMENV_HOST_ID_MAX, "::1");
    check_id("FE80::1", 2222, KITTY_TERMENV_HOST_ID_MAX, "[fe80::1]:2222");
    check_id("", 22, KITTY_TERMENV_HOST_ID_MAX, NULL);
    check_id(NULL, 22, KITTY_TERMENV_HOST_ID_MAX, NULL);
    check_id("abcd", 22, 5, "abcd");          /* exactly fits */
    check_id("abcde", 22, 5, NULL);           /* one short: none, not cut */
    check_id("abcd", 2222, 9, NULL);

    /* the NOTE once per host */
    {
        MemStore m;
        KittyTermEnvHintStore st = { mem_said, mem_mark, &m };
        memset(&m, 0, sizeof(m));
        check_due("first time", &st, "example.com", true);
        check_due("second time", &st, "example.com", false);
        check_due("third time", &st, "example.com", false);
        check_due("another port is another host", &st, "example.com:2222",
                  true);
        check_due("and then known", &st, "example.com:2222", false);
        check_due("empty id", &st, "", false);
        check_due("no id", &st, NULL, false);
        check_due("no store", NULL, "other.example", false);
        if (m.marks != 2) {
            printf("FAIL [marks]: %d marks written, want 2\n", m.marks);
            failures++;
        }
    }
    {
        MemStore m;
        KittyTermEnvHintStore st = { mem_said, mem_mark, &m };
        memset(&m, 0, sizeof(m));
        m.mark_fails = true;
        check_due("unwritable store: still said", &st, "example.com", true);
        check_due("unwritable store: said again", &st, "example.com", true);
    }

    /* the session's own refused variables: one line, every connection,
     * in the order sent; refused defaults are not in it */
    {
        const char *const names[] = { "LANG", "EDITOR", "COLORTERM",
                                      "PAGER", "TERM_PROGRAM" };
        const bool added[] = { false, false, true, false, true };
        const bool none[] = { false, false, false, false, false };
        const bool two[] = { true, true, false, false, false };
        const bool one[] = { false, false, false, true, false };
        const bool mixed[] = { true, false, true, true, true };
        const bool dflt[] = { false, false, true, false, true };
        check_own("nothing refused", names, added, none, 5, NULL);
        check_own("only defaults refused", names, added, dflt, 5, NULL);
        check_own("two own", names, added, two, 5,
                  NOTE_HEAD "the server refused LANG, EDITOR; its "
                  "sshd_config needs \"AcceptEnv LANG EDITOR\".\r\n");
        check_own("one own", names, added, one, 5,
                  NOTE_HEAD "the server refused PAGER; its sshd_config "
                  "needs \"AcceptEnv PAGER\".\r\n");
        check_own("own and defaults", names, added, mixed, 5,
                  NOTE_HEAD "the server refused LANG, PAGER; its "
                  "sshd_config needs \"AcceptEnv LANG PAGER\".\r\n");
        check_own("empty list", names, added, none, 0, NULL);
    }
    /* the defaults' NOTE shows only the refused ones (a Debian
     * sshd accepts COLORTERM and refused TERM_PROGRAM, while the fixed text
     * still showed both); "for true colour" only with COLORTERM */
    {
        const char *const names[] = { "LANG", "COLORTERM", "TERM_PROGRAM" };
        const bool added[] = { false, true, true };
        const bool both[] = { false, true, true };
        const bool tp[] = { false, false, true };
        const bool ct[] = { true, true, false };
        const bool own[] = { true, false, false };
        const struct { const char *what; const bool *refused; const char *want; } c[] = {
            { "both refused", both, NOTE_HEAD "this server drops COLORTERM and "
              "TERM_PROGRAM; add \"AcceptEnv COLORTERM TERM_PROGRAM\" to its "
              "sshd_config and reload sshd for true colour.\r\n" },
            { "TERM_PROGRAM only", tp, NOTE_HEAD "this server drops "
              "TERM_PROGRAM; add \"AcceptEnv TERM_PROGRAM\" to its sshd_config "
              "and reload sshd.\r\n" },
            { "COLORTERM only (and an own one)", ct, NOTE_HEAD "this server "
              "drops COLORTERM; add \"AcceptEnv COLORTERM\" to its sshd_config "
              "and reload sshd for true colour.\r\n" },
            { "no default refused", own, NULL },
        };
        size_t k;
        for (k = 0; k < sizeof(c) / sizeof(c[0]); k++) {
            char buf[512];
            size_t len = kitty_termenv_refused_default_note(
                names, added, c[k].refused, 3, buf, sizeof(buf));
            size_t len0 = kitty_termenv_refused_default_note(
                names, added, c[k].refused, 3, NULL, 0);
            const char *want = c[k].want ? c[k].want : "";
            if (len != len0 || len != strlen(want) || strcmp(buf, want)) {
                printf("FAIL [defaults note, %s]: got \"%s\" (%u/%u), want \"%s\"\n",
                       c[k].what, buf, (unsigned)len, (unsigned)len0, want);
                failures++;
            }
        }
    }
    {
        char line[200];
        snprintf(line, sizeof(line), KT_TERMENV_REFUSED_OWN_LOG, "LANG",
                 "LANG");
        if (strcmp(line, "Server refused to set environment variable LANG - "
                         "its sshd_config needs \"AcceptEnv LANG\"")) {
            printf("FAIL [own log line]: %s\n", line);
            failures++;
        }
        snprintf(line, sizeof(line), KT_TERMENV_REFUSED_DEFAULT_LOG,
                 "TERM_PROGRAM", "TERM_PROGRAM");
        if (strcmp(line, "Server refused to set environment variable "
                         "TERM_PROGRAM - its sshd_config needs \"AcceptEnv "
                         "TERM_PROGRAM\"")) {
            printf("FAIL [default log line]: %s\n", line);
            failures++;
        }
    }

    /* no hook (every program but the KiTTY++ terminals): no NOTE */
    if (kitty_termenv_hint_hook != NULL) {
        printf("FAIL [hint hook]: set without a terminal\n");
        failures++;
    }

    if (failures) {
        printf("test_termenv: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_termenv: all passed\n");
    return 0;
}
