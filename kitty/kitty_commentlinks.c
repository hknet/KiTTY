/*
 * kitty_commentlinks.c - the links in a session comment and the comment
 * wrapped to a width (kitty_commentlinks.h). No window, no GDI: the box
 * that draws it is kitty_commentview.c, the unit test
 * test/test_commentlinks.c.
 */
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "kitty_commentlinks.h"
#include "kitty_url_regex.h"
#include "url/re_lib/regexp.h"

/*
 * The terminal's default expression, compiled once into a regexp of our
 * own: the one a running session scans with (url/urlhack.c) is never
 * touched. The engine keeps its state in globals - UI thread only, as
 * the terminal's scan.
 */
static regexp *kcl_rx;
static int kcl_rx_tried;

static regexp *kcl_regex(void)
{
    if (!kcl_rx_tried) {
        kcl_rx_tried = 1;
        kcl_rx = regcomp((char *)KITTY_URL_REGEX_DEFAULT);
    }
    return kcl_rx;
}

static int kcl_is_blank(wchar_t c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == 0x0B ||
           c == 0x0C || c == 0xA0 || c == 0x3000 ||
           (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029;
}

/* A letter or digit right before "file://" or "\\" makes it part of a word,
 * not the start of a link. Anything outside ASCII counts as a letter. */
static int kcl_is_word(wchar_t c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') || c == '_' || c >= 0x80;
}

/* Where an unquoted file:// or UNC link stops. */
static int kcl_is_stop(wchar_t c)
{
    return kcl_is_blank(c) || c == '"' || c == '<' || c == '>';
}

/* The trailing characters that end a sentence rather than a path. */
static int kcl_is_trail(wchar_t c)
{
    return c == '.' || c == ',' || c == ';' || c == ':' || c == ')';
}

static int kcl_starts(const wchar_t *t, int i, int n, const char *lit, int nocase)
{
    int k;
    for (k = 0; lit[k]; k++) {
        wchar_t c;
        if (i + k >= n)
            return 0;
        c = t[i + k];
        if (nocase && c >= 'A' && c <= 'Z')
            c = (wchar_t)(c - 'A' + 'a');
        if (c != (wchar_t)(unsigned char)lit[k])
            return 0;
    }
    return 1;
}

/* The end of a file:// link starting at i, -1 for none. Quoted: it runs to
 * n (the closing quote); otherwise to the next stop character, trailing
 * punctuation left out. Something other than '/' must follow "file://". */
static int kcl_file_end(const wchar_t *t, int i, int n, int quoted)
{
    int j = i + 7, e, k;
    if (!kcl_starts(t, i, n, "file://", 1))
        return -1;
    e = j;
    while (e < n && (quoted ? (t[e] != '\r' && t[e] != '\n') : !kcl_is_stop(t[e])))
        e++;
    if (!quoted)
        while (e > j && kcl_is_trail(t[e - 1]))
            e--;
    for (k = j; k < e; k++)
        if (t[k] != '/')
            return e;
    return -1;
}

/* The end of a UNC path \\server\share... starting at i, -1 for none. */
static int kcl_unc_end(const wchar_t *t, int i, int n, int quoted)
{
    int j = i + 2, s, share, e;
#define KCL_UNC_STOP(c) ((quoted ? ((c) == '\r' || (c) == '\n') : kcl_is_stop(c)) || \
                         (c) == '\\' || (c) == '/')
    if (!kcl_starts(t, i, n, "\\\\", 0))
        return -1;
    s = j;
    while (j < n && !KCL_UNC_STOP(t[j]))
        j++;
    if (j == s || j >= n || t[j] != '\\')
        return -1;                       /* no server, or no share after it */
    share = ++j;
    while (j < n && !KCL_UNC_STOP(t[j]))
        j++;
    if (j == share)
        return -1;
#undef KCL_UNC_STOP
    e = j;
    while (e < n && (quoted ? (t[e] != '\r' && t[e] != '\n') : !kcl_is_stop(t[e])))
        e++;
    if (!quoted)
        while (e > share && kcl_is_trail(t[e - 1]))
            e--;
    return e > share ? e : -1;
}

/*
 * The text as the expression's engine reads it: one byte per UTF-16 unit,
 * so a match's byte offsets are the text's offsets. White space and
 * control characters become a space (a line break ends a link, as the
 * blank does in the terminal); anything outside ASCII becomes 0x7F, which
 * none of the expression's classes names - it is matched where the
 * terminal matches "any character but ...", in a path, never in a host.
 */
static char *kcl_narrow(const wchar_t *t, int n)
{
    char *a = malloc((size_t)n + 1);
    int i;
    if (!a)
        return NULL;
    for (i = 0; i < n; i++) {
        wchar_t c = t[i];
        a[i] = (c < 0x20 || kcl_is_blank(c)) ? ' ' : c < 0x80 ? (char)c : 0x7F;
    }
    a[n] = '\0';
    return a;
}

/* Does a web link the expression accepts start right at t[i] (up to n)? */
static int kcl_web_at(const wchar_t *t, int i, int n)
{
    regexp *rx = kcl_regex();
    char *a;
    int ok = 0;
    if (!rx || n <= i)
        return 0;
    a = kcl_narrow(t + i, n - i);
    if (!a)
        return 0;
    if (regexec(rx, a) == 1 && rx->startp[0] == a && rx->endp[0] > a)
        ok = 1;
    free(a);
    return ok;
}

typedef struct { int start, end, prio; } kcl_cand;
typedef struct { kcl_cand *v; int n, cap; } kcl_cands;

static void kcl_add(kcl_cands *c, int start, int end, int prio)
{
    if (end <= start)
        return;
    if (c->n == c->cap) {
        int cap = c->cap ? c->cap * 2 : 16;
        kcl_cand *v = realloc(c->v, (size_t)cap * sizeof(*v));
        if (!v)
            return;
        c->v = v;
        c->cap = cap;
    }
    c->v[c->n].start = start;
    c->v[c->n].end = end;
    c->v[c->n].prio = prio;
    c->n++;
}

/* Earlier first; at the same start the quoted form, then file/UNC, then the
 * expression; then the longer. */
static int kcl_cmp(const void *pa, const void *pb)
{
    const kcl_cand *a = pa, *b = pb;
    if (a->start != b->start)
        return a->start < b->start ? -1 : 1;
    if (a->prio != b->prio)
        return a->prio < b->prio ? -1 : 1;
    if (a->end != b->end)
        return a->end > b->end ? -1 : 1;
    return 0;
}

int kcl_find_links(const wchar_t *text, int n, kcl_span **out)
{
    kcl_cands c = { NULL, 0, 0 };
    kcl_span *res = NULL;
    regexp *rx;
    int i, nres = 0, last;

    *out = NULL;
    if (!text || n <= 0)
        return 0;

    /* 1. Inside double quotes or <...>: everything up to the closer on the
     *    same line, when it starts as a link. */
    for (i = 0; i < n; i++) {
        wchar_t open = text[i], close;
        int j, s, e = -1;
        if (open != '"' && open != '<')
            continue;
        close = open == '"' ? '"' : '>';
        s = i + 1;
        for (j = s; j < n && text[j] != close && text[j] != '\r' && text[j] != '\n'; j++)
            ;
        if (j >= n || text[j] != close || j == s)
            continue;
        if (kcl_starts(text, s, j, "file://", 1))
            e = kcl_file_end(text, s, j, 1);
        else if (kcl_starts(text, s, j, "\\\\", 0))
            e = kcl_unc_end(text, s, j, 1);
        else if ((kcl_starts(text, s, j, "http://", 0) ||
                  kcl_starts(text, s, j, "https://", 0) ||
                  kcl_starts(text, s, j, "ftp://", 0) ||
                  kcl_starts(text, s, j, "www.", 0)) && kcl_web_at(text, s, j))
            e = j;
        if (e == j) {
            kcl_add(&c, s, j, 0);
            i = j;                          /* the closer opens nothing */
        }
    }

    /* 2. file:// and UNC paths, up to white space. */
    for (i = 0; i < n; i++) {
        int e;
        if (i > 0 && (kcl_is_word(text[i - 1]) || text[i - 1] == '\\'))
            continue;
        if (text[i] == 'f' || text[i] == 'F')
            e = kcl_file_end(text, i, n, 0);
        else if (text[i] == '\\')
            e = kcl_unc_end(text, i, n, 0);
        else
            continue;
        if (e > i)
            kcl_add(&c, i, e, 1);
    }

    /* 3. What the terminal's default expression finds, the same way it
     *    scans: a match starting with a blank starts after it, the next
     *    search starts at the end of a match. */
    rx = kcl_regex();
    if (rx) {
        char *a = kcl_narrow(text, n), *p = a;
        while (p && *p && regexec(rx, p) == 1) {
            char *s = rx->startp[0], *e = rx->endp[0];
            if (s < e && *s == ' ')
                s++;
            if (e > s)
                kcl_add(&c, (int)(s - a), (int)(e - a), 2);
            p = e > p ? e : p + 1;
        }
        free(a);
    }

    if (!c.n)
        return 0;
    qsort(c.v, (size_t)c.n, sizeof(*c.v), kcl_cmp);
    res = malloc((size_t)c.n * sizeof(*res));
    if (res) {
        for (i = 0, last = 0; i < c.n; i++) {
            if (c.v[i].start < last)
                continue;                   /* inside a link already taken */
            res[nres].start = c.v[i].start;
            res[nres].end = c.v[i].end;
            nres++;
            last = c.v[i].end;
        }
    }
    free(c.v);
    if (!nres) {
        free(res);
        return 0;
    }
    *out = res;
    return nres;
}

wchar_t *kcl_link_target(const wchar_t *text, const kcl_span *link)
{
    int len = link->end - link->start, pre = 0;
    wchar_t *t;
    if (len < 0)
        len = 0;
    if (kcl_starts(text, link->start, link->end, "www.", 0))
        pre = 7;                            /* "http://" */
    t = malloc(((size_t)len + pre + 1) * sizeof(wchar_t));
    if (!t)
        return NULL;
    if (pre)
        memcpy(t, L"http://", 7 * sizeof(wchar_t));
    memcpy(t + pre, text + link->start, (size_t)len * sizeof(wchar_t));
    t[pre + len] = 0;
    return t;
}

/* ---- wrapping ---------------------------------------------------------- */

static void kcl_run_add(kcl_layout *l, int *cap, int line, int start, int end,
                        int x, int w, int link)
{
    if (l->nruns == *cap) {
        int nc = *cap ? *cap * 2 : 16;
        kcl_run *r = realloc(l->runs, (size_t)nc * sizeof(*r));
        if (!r)
            return;
        l->runs = r;
        *cap = nc;
    }
    l->runs[l->nruns].line = line;
    l->runs[l->nruns].start = start;
    l->runs[l->nruns].end = end;
    l->runs[l->nruns].x = x;
    l->runs[l->nruns].w = w;
    l->runs[l->nruns].link = link;
    l->nruns++;
}

/* One line, text[ls .. le), cut into runs at the link boundaries. */
static void kcl_line(kcl_layout *l, int *cap, const wchar_t *text, int ls, int le,
                     const kcl_span *links, int nlinks, kcl_width_fn width, void *ctx)
{
    int line = l->nlines++, pos = ls, k = 0;
    while (pos < le) {
        int end = le, link = -1;
        while (k < nlinks && links[k].end <= pos)
            k++;
        if (k < nlinks && links[k].start <= pos) {
            link = k;
            if (links[k].end < end)
                end = links[k].end;
        } else if (k < nlinks && links[k].start < end) {
            end = links[k].start;
        }
        kcl_run_add(l, cap, line, pos, end,
                    pos > ls ? width(ctx, text + ls, pos - ls) : 0,
                    width(ctx, text + pos, end - pos), link);
        pos = end;
    }
}

/* How many of s[0 .. n) fit in maxw - at least one, never half of a
 * surrogate pair. */
static int kcl_fit(const wchar_t *s, int n, int maxw, kcl_width_fn width, void *ctx)
{
    int lo = 1, hi = n;
    if (width(ctx, s, n) <= maxw)
        return n;
    if (width(ctx, s, 1) > maxw)
        lo = 1;
    else {
        while (hi - lo > 1) {
            int mid = lo + (hi - lo) / 2;
            if (width(ctx, s, mid) <= maxw)
                lo = mid;
            else
                hi = mid;
        }
    }
    if (lo < n && lo > 1 && s[lo] >= 0xDC00 && s[lo] <= 0xDFFF)
        lo--;
    else if (lo == 1 && n > 1 && s[0] >= 0xD800 && s[0] <= 0xDBFF)
        lo = 2;
    return lo;
}

void kcl_layout_build(kcl_layout *out, const wchar_t *text, int n,
                      const kcl_span *links, int nlinks, int maxw,
                      kcl_width_fn width, void *ctx)
{
    int cap = 0, ps = 0;
    out->runs = NULL;
    out->nruns = 0;
    out->nlines = 0;
    if (!text)
        n = 0;
    if (maxw < 1)
        maxw = 1;
    for (;;) {
        int pe = ps, p;
        while (pe < n && text[pe] != '\r' && text[pe] != '\n')
            pe++;
        if (ps == pe)
            kcl_line(out, &cap, text, ps, ps, links, nlinks, width, ctx);
        for (p = ps; p < pe;) {
            int rem = pe - p, k = kcl_fit(text + p, rem, maxw, width, ctx);
            int le, next;
            if (k >= rem) {
                le = next = pe;
            } else {
                int i;
                if (kcl_is_blank(text[p + k])) {
                    le = next = p + k;
                } else {
                    for (i = p + k - 1; i > p && !kcl_is_blank(text[i]); i--)
                        ;
                    le = next = (i > p) ? i : p + k;
                }
                while (le > p && kcl_is_blank(text[le - 1]))
                    le--;
                while (next < pe && kcl_is_blank(text[next]))
                    next++;
            }
            kcl_line(out, &cap, text, p, le, links, nlinks, width, ctx);
            p = next;
        }
        if (pe >= n)
            break;
        ps = (text[pe] == '\r' && pe + 1 < n && text[pe + 1] == '\n') ? pe + 2 : pe + 1;
    }
}

void kcl_layout_free(kcl_layout *l)
{
    free(l->runs);
    l->runs = NULL;
    l->nruns = l->nlines = 0;
}
