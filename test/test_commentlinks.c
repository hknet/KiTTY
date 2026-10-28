/*
 * test_commentlinks - the links in a session comment and the comment
 * wrapped to a width (kitty/kitty_commentlinks.c), as the Session panel's
 * comment box (kitty/kitty_commentview.c) draws it: web links by the
 * terminal's default expression, file:// links, UNC paths, quoted and
 * <...> links with spaces, trailing punctuation, line breaks, a link
 * wrapped over two lines, text outside ASCII. Widths come from a fixed
 * 10 units per UTF-16 unit. Returns non-zero on any failure.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "../kitty/kitty_commentlinks.h"
#include "../kitty/kitty_text.h"   /* the two placeholders the box shows */

static int failures = 0, checks = 0;
#define CHECK(c, what) do { checks++; if (!(c)) { printf("FAIL %s (line %d)\n", what, __LINE__); failures++; } \
                            else printf("ok   %s\n", what); } while (0)

#define CW 10   /* the width of one UTF-16 unit */
static int mono(void *ctx, const wchar_t *s, int n) { (void)ctx; (void)s; return n * CW; }

/* UTF-8 (the source's spelling) to UTF-16, malloc'd; *n its length. */
static wchar_t *wide(const char *u8, int *n)
{
    int len = MultiByteToWideChar(CP_UTF8, 0, u8, -1, NULL, 0);
    wchar_t *w = malloc((size_t)(len > 0 ? len : 1) * sizeof(wchar_t));
    if (len > 0)
        MultiByteToWideChar(CP_UTF8, 0, u8, -1, w, len);
    else
        w[0] = 0;
    *n = len > 0 ? len - 1 : 0;
    return w;
}

/* A piece of wide text as UTF-8, in a static buffer. */
static const char *narrow(const wchar_t *w, int n)
{
    static char buf[240];
    int len = WideCharToMultiByte(CP_UTF8, 0, w, n, buf, (int)sizeof(buf) - 1, NULL, NULL);
    buf[len > 0 ? len : 0] = 0;
    return buf;
}

/* The links of `text` must be exactly the `nexp` strings in `exp`. */
static void links_are(const char *text, int nexp, const char *const *exp, const char *what)
{
    int n, i, nl;
    wchar_t *w = wide(text, &n);
    kcl_span *sp;
    nl = kcl_find_links(w, n, &sp);
    if (nl != nexp) {
        printf("     found %d link(s) in \"%s\":", nl, text);
        for (i = 0; i < nl; i++)
            printf(" [%s]", narrow(w + sp[i].start, sp[i].end - sp[i].start));
        printf("\n");
    }
    CHECK(nl == nexp, what);
    for (i = 0; i < nl && i < nexp; i++) {
        const char *got = narrow(w + sp[i].start, sp[i].end - sp[i].start);
        if (strcmp(got, exp[i]))
            printf("     link %d is [%s], expected [%s]\n", i, got, exp[i]);
        CHECK(!strcmp(got, exp[i]), what);
    }
    free(sp);
    free(w);
}
#define LINKS0(text, what) links_are(text, 0, NULL, what)
#define LINKS1(text, a, what) do { const char *e_[] = { a }; links_are(text, 1, e_, what); } while (0)
#define LINKS2(text, a, b, what) do { const char *e_[] = { a, b }; links_are(text, 2, e_, what); } while (0)

/* The target opening the first link starts. */
static void target_is(const char *text, const char *exp, const char *what)
{
    int n;
    wchar_t *w = wide(text, &n), *t;
    kcl_span *sp;
    int nl = kcl_find_links(w, n, &sp);
    CHECK(nl >= 1, what);
    if (nl >= 1) {
        t = kcl_link_target(w, &sp[0]);
        CHECK(t && !strcmp(narrow(t, (int)wcslen(t)), exp), what);
        free(t);
    }
    free(sp);
    free(w);
}

/* The text of the runs of one line, links in [], separated by |. */
static const char *line_text(const wchar_t *w, const kcl_layout *l, int line)
{
    static char out[240];
    int i;
    out[0] = 0;
    for (i = 0; i < l->nruns; i++) {
        const kcl_run *r = &l->runs[i];
        if (r->line != line)
            continue;
        if (out[0])
            strcat(out, "|");
        if (r->link >= 0) strcat(out, "[");
        strcat(out, narrow(w + r->start, r->end - r->start));
        if (r->link >= 0) strcat(out, "]");
    }
    return out;
}

static void layout_of(const char *text, int maxw, kcl_layout *l, wchar_t **wout)
{
    int n;
    wchar_t *w = wide(text, &n);
    kcl_span *sp;
    int nl = kcl_find_links(w, n, &sp);
    kcl_layout_build(l, w, n, sp, nl, maxw, mono, NULL);
    free(sp);
    *wout = w;
}

int main(void)
{
    kcl_layout l;
    wchar_t *w;
    int i, n;

    /* ---- finding ---- */
    LINKS0("A plain comment, owned by the ops team.", "plain comment: no link");
    LINKS1("https://wiki.example.com/runbook is the runbook",
           "https://wiki.example.com/runbook", "link at the start");
    LINKS1("the runbook: https://wiki.example.com/runbook",
           "https://wiki.example.com/runbook", "link at the end");
    LINKS2("see https://a.example.com/1 and ftp://files.example.org/pub/x.tar today",
           "https://a.example.com/1", "ftp://files.example.org/pub/x.tar", "two links");
    LINKS1("docs on www.example.com/docs now", "www.example.com/docs", "www. link");
    target_is("docs on www.example.com/docs now", "http://www.example.com/docs",
              "www. opens as http://");
    target_is("https://a.example.com/x", "https://a.example.com/x", "https link opens as written");
    LINKS1("see https://a.example.com/page.", "https://a.example.com/page",
           "a trailing . is not part of the link");
    LINKS1("(see https://a.example.com/page)", "https://a.example.com/page",
           "a trailing ) is not part of the link");
    LINKS1("Pr\xC3\xBC" "fung: https://a.example.com/M\xC3\xBCller ok",
           "https://a.example.com/M\xC3\xBCller", "umlauts before and inside a link");
    LINKS0("", "empty text: no link");
    LINKS0(KT_CFG_COMMENT_SELECT, "placeholder 'select a session': no link");
    LINKS0(KT_CFG_COMMENT_NONE, "placeholder 'no comment': no link");
    LINKS1("file:///C:/x.pdf", "file:///C:/x.pdf", "file:///C:/x.pdf");
    LINKS1("open file:///C:/x.pdf.", "file:///C:/x.pdf", "file:// without the trailing .");
    LINKS1("file://srv/share/x", "file://srv/share/x", "file://srv/share/x");
    LINKS0("file:// alone", "file:// alone: no link");
    LINKS0("xfile://srv/share/x", "file:// inside a word: no link");
    LINKS1("\\\\srv\\share\\dir\\x.txt", "\\\\srv\\share\\dir\\x.txt", "UNC path");
    target_is("\\\\srv\\share\\dir\\x.txt", "\\\\srv\\share\\dir\\x.txt", "UNC path opens as written");
    LINKS1("on \\\\srv\\share\\dir\\x.txt, then", "\\\\srv\\share\\dir\\x.txt",
           "UNC path without the trailing ,");
    LINKS0("\\\\srv", "\\\\srv alone: no link (no share)");
    LINKS0("\\\\srv\\ next", "\\\\srv\\ with nothing after: no link");
    LINKS1("the runbook \"\\\\srv\\docs\\my runbook.pdf\" here",
           "\\\\srv\\docs\\my runbook.pdf", "quoted UNC path with a space");
    LINKS1("<file://srv/share/a b.pdf>", "file://srv/share/a b.pdf", "<file:// link with a space>");
    LINKS1("\"https://a.example.com/my file\"", "https://a.example.com/my file",
           "quoted web link with a space");
    LINKS1("\"not a link\" \\\\srv\\share", "\\\\srv\\share", "a quote that opens no link");
    LINKS2("https://a.example.com/x\r\nwww.example.org", "https://a.example.com/x",
           "www.example.org", "a line break ends a link");
    LINKS1("file://www.example.com/x", "file://www.example.com/x",
           "a file:// link is one link, not a www. one inside it");

    /* ---- wrapping ---- */
    layout_of("aaa bbb ccc", 7 * CW, &l, &w);
    CHECK(l.nlines == 2, "wrap: 'aaa bbb ccc' at 7 is two lines");
    CHECK(!strcmp(line_text(w, &l, 0), "aaa bbb"), "wrap: first line 'aaa bbb'");
    CHECK(!strcmp(line_text(w, &l, 1), "ccc"), "wrap: second line 'ccc'");
    kcl_layout_free(&l); free(w);

    layout_of("see https://a.example.com/x now", 1000, &l, &w);
    CHECK(l.nlines == 1 && l.nruns == 3, "a link in a line: three runs");
    CHECK(!strcmp(line_text(w, &l, 0), "see |[https://a.example.com/x]| now"),
          "a link in a line: text, link, text");
    CHECK(l.nruns == 3 && l.runs[1].x == 4 * CW && l.runs[1].w == 23 * CW,
          "a link in a line: its place and width");
    kcl_layout_free(&l); free(w);

    layout_of("x https://a.example.com/averyveryverylongpath/abcdefghij", 20 * CW, &l, &w);
    {
        int linklines = 0, covered = 0, last = -1;
        for (i = 0; i < l.nruns; i++)
            if (l.runs[i].link == 0) {
                if (l.runs[i].line != last) linklines++;
                last = l.runs[i].line;
                covered += l.runs[i].end - l.runs[i].start;
            }
        CHECK(linklines >= 2, "a link longer than a line wraps over lines");
        CHECK(covered == 54, "the wrapped link's runs cover the link whole");
        for (i = 0; i < l.nruns; i++)
            if (l.runs[i].x + l.runs[i].w > 20 * CW)
                break;
        CHECK(i == l.nruns, "no run reaches past the width");
    }
    kcl_layout_free(&l); free(w);

    layout_of("", 100, &l, &w);
    CHECK(l.nlines == 1 && l.nruns == 0, "empty text: one empty line");
    kcl_layout_free(&l); free(w);

    layout_of("one\r\n\r\nthree\nfour", 1000, &l, &w);
    CHECK(l.nlines == 4, "CR LF, an empty line and LF: four lines");
    CHECK(!strcmp(line_text(w, &l, 2), "three"), "the third line after an empty one");
    kcl_layout_free(&l); free(w);

    /* six bytes of UTF-8 each side: a wrap by byte would cut after two */
    layout_of("\xC3\xA4\xC3\xB6\xC3\xBC \xC3\xA4\xC3\xB6\xC3\xBC", 4 * CW, &l, &w);
    CHECK(l.nlines == 2 && !strcmp(line_text(w, &l, 0), "\xC3\xA4\xC3\xB6\xC3\xBC") &&
          !strcmp(line_text(w, &l, 1), "\xC3\xA4\xC3\xB6\xC3\xBC"),
          "umlauts wrap by character, not by byte");
    kcl_layout_free(&l); free(w);

    w = wide(KT_CFG_COMMENT_NONE, &n);
    kcl_layout_build(&l, w, n, NULL, 0, 1000, mono, NULL);
    CHECK(l.nruns == 1 && l.runs[0].link < 0, "a placeholder draws as one plain run");
    kcl_layout_free(&l); free(w);

    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
