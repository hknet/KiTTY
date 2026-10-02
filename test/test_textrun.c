/*
 * test_textrun - KiTTY's two speed-ups of terminal output must leave the
 * terminal EXACTLY as upstream's path does: the plain-text fast path in
 * term_out() (term_text_run) and the deferred scrollback compression
 * (term_sb_compact), both in terminal/terminal.c.
 *
 * Every case feeds the same bytes, in the same chunk splits, into two
 * terminals: one with both, one with term_textrun_off and term_sbdefer_off
 * set; it compares them, then compresses what the first one deferred and
 * compares again. Each comparison covers every cell of the screen and the
 * scrollback - character, attributes, true colour, combining chain - each
 * line's attributes and trust flag, the cursor, the pending wrap, the last
 * graphic character (REP repeats it) and the selection state. A difference
 * names the case, the line and the column.
 *
 * The cases are the edges of both: runs that end at the margin, overflow
 * it, wrap and scroll inside a scroll region, cross a wide or combining
 * character, auto-wrap off, insert mode, line drawing, trusted lines, a
 * selection being overwritten, UTF-8 split across chunks, malformed UTF-8,
 * a scrollback overflowing while lines wait uncompressed, resizes pulling
 * them back onto the screen, clearing the scrollback, and a seeded random
 * mix of all of it.
 *
 * Not covered here: the session log (the terminals have no log context);
 * the log call in the fast path mirrors the ordinary one line for line.
 *
 * Built against terminal.c compiled WITH MOD_PERSO (and MOD_FAR2L, like the
 * kitty target), because the fast path lives behind that define and the
 * guiterminal library is built without it. Returns non-zero on any failure.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "putty.h"
#include "terminal.h"

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

const char *appname = "test_textrun";

char *platform_default_s(const char *name)
{ return NULL; }
bool platform_default_b(const char *name, bool def)
{ return def; }
int platform_default_i(const char *name, int def)
{ return def; }
FontSpec *platform_default_fontspec(const char *name)
{ return fontspec_new_default(); }
Filename *platform_default_filename(const char *name)
{ return filename_from_str(""); }

const struct BackendVtable *const backends[] = { NULL };

/* The KiTTY entry points terminal.c calls when built with MOD_PERSO. None of
 * them is reached by plain text; they exist so the terminal links, and do
 * nothing (test_osc52.c has recording versions for the clipboard tests). */
char *kitty_expand_wintitle(const char *title, const char *hostname, Conf *conf)
{ return dupstr(title ? title : ""); }
void kitty_set_remote_cwd(const char *osc7) { }
void kitty_osc52_send_raw(Terminal *term, const char *data, size_t len) { }
wchar_t *kitty_osc52_get_clipboard_ex(int *len, bool *unavailable)
{ if (len) *len = 0; if (unavailable) *unavailable = false; return NULL; }
wchar_t *kitty_osc52_get_clipboard(int *len)
{ if (len) *len = 0; return NULL; }
unsigned char *kitty_osc52_get_clipboard_png(size_t *len, bool *unavailable)
{ *len = 0; if (unavailable) *unavailable = false; return NULL; }
bool kitty_osc52_clipboard_has_image(void) { return false; }
void kitty_osc52_random(unsigned char *buf, size_t len) { memset(buf, 1, len); }
bool kitty_osc52_set_clipboard_formats(const KittyClipFormat *fmts, int n)
{ return false; }
bool kitty_osc52_read_dialog(Terminal *term, const wchar_t *clip, int clip_len,
                             const char *claim, int *grant, bool *always_deny)
{ if (grant) *grant = 0; if (always_deny) *always_deny = false; return false; }
bool kitty_osc52_save_deny_for_host(Terminal *term) { return true; }
bool kitty_osc52_title_visible(void) { return true; }
void kitty_osc52_notify(Terminal *term, const char *t, const char *m, int a) { }
void kitty_osc52_state_changed(Terminal *term) { }
void kitty_osc52_write_confirm(Terminal *term) { }
void kitty_osc52_write_confirm_end(Terminal *term) { }
void kitty_transfer_osc(Terminal *term) { (void)term; }
void kitty_transfer_free(Terminal *term) { (void)term; }

typedef struct Mock {
    Terminal *term;
    Conf *conf;
    struct unicode_data ucsdata[1];
    TermWin tw;
} Mock;

static bool mock_setup_draw_ctx(TermWin *win) { return false; }
static void mock_draw_text(TermWin *win, int x, int y, wchar_t *text, int len,
                           unsigned long attrs, int lattrs, truecolour tc) {}
static void mock_draw_cursor(TermWin *win, int x, int y, wchar_t *text,
                             int len, unsigned long attrs, int lattrs,
                             truecolour tc) {}
static void mock_set_raw_mouse_mode(TermWin *win, bool enable) {}
static void mock_set_raw_mouse_mode_pointer(TermWin *win, bool enable) {}
static void mock_palette_set(TermWin *win, unsigned start, unsigned ncolours,
                             const rgb *colours) {}
static void mock_palette_get_overrides(TermWin *tw, Terminal *term) {}
static void mock_set_title(TermWin *win, const char *title, int codepage) {}
static void mock_set_icon_title(TermWin *win, const char *title, int cp) {}

static const TermWinVtable mock_termwin_vt = {
    .setup_draw_ctx = mock_setup_draw_ctx,
    .draw_text = mock_draw_text,
    .draw_cursor = mock_draw_cursor,
    .set_title = mock_set_title,
    .set_icon_title = mock_set_icon_title,
    .set_raw_mouse_mode = mock_set_raw_mouse_mode,
    .set_raw_mouse_mode_pointer = mock_set_raw_mouse_mode_pointer,
    .palette_set = mock_palette_set,
    .palette_get_overrides = mock_palette_get_overrides,
};

static Mock *mock_new(int cols, int rows, bool cjk_ambig_wide)
{
    Mock *mk = snew(Mock);
    memset(mk, 0, sizeof(*mk));
    mk->conf = conf_new();
    do_defaults(NULL, mk->conf);
    conf_set_bool(mk->conf, CONF_cjk_ambig_wide, cjk_ambig_wide);
    init_ucs_generic(mk->conf, mk->ucsdata);
    mk->ucsdata->line_codepage = CP_UTF8;
    mk->tw.vt = &mock_termwin_vt;
    mk->term = term_init(mk->conf, mk->ucsdata, &mk->tw);
    term_pwron(mk->term, true);
    term_size(mk->term, rows, cols, 200);
    return mk;
}

static void mock_free(Mock *mk)
{
    term_free(mk->term);
    conf_free(mk->conf);
    sfree(mk);
}

static int failures = 0, cases = 0;

/* A seeded generator, so a failure reproduces and both terminals get the
 * same chunk splits. */
static unsigned long rng_state;
static unsigned rng(void)
{
    rng_state = rng_state * 1103515245UL + 12345UL;
    return (unsigned)((rng_state >> 16) & 0x7FFF);
}

/* Feed data in chunks: whole when maxchunk is 0, else 1..maxchunk bytes at a
 * time from the given seed. */
static void feed(Terminal *term, const char *data, size_t len,
                 int maxchunk, unsigned long seed)
{
    size_t off = 0;
    rng_state = seed;
    while (off < len) {
        size_t n = maxchunk ? 1 + rng() % maxchunk : len;
        if (n > len - off)
            n = len - off;
        term_data(term, data + off, n);
        off += n;
    }
}

static bool same_termchar_colour(const termchar *a, const termchar *b)
{
    return a->truecolour.fg.enabled == b->truecolour.fg.enabled &&
        a->truecolour.fg.r == b->truecolour.fg.r &&
        a->truecolour.fg.g == b->truecolour.fg.g &&
        a->truecolour.fg.b == b->truecolour.fg.b &&
        a->truecolour.bg.enabled == b->truecolour.bg.enabled &&
        a->truecolour.bg.r == b->truecolour.bg.r &&
        a->truecolour.bg.g == b->truecolour.bg.g &&
        a->truecolour.bg.b == b->truecolour.bg.b;
}

/* Every observable piece of state the text path writes, fast vs ordinary. */
static void compare(const char *what, Terminal *f, Terminal *o)
{
    char msg[256];
    int y, x, sbf = count234(f->scrollback), sbo = count234(o->scrollback);
    bool bad = false;

#define DIFF(...) do { snprintf(msg, sizeof(msg), __VA_ARGS__); \
        printf("FAIL %s: %s\n", what, msg); bad = true; } while (0)

    if (f->curs.x != o->curs.x || f->curs.y != o->curs.y)
        DIFF("cursor %d,%d vs %d,%d", f->curs.x, f->curs.y, o->curs.x, o->curs.y);
    if (f->wrapnext != o->wrapnext)
        DIFF("wrapnext %d vs %d", f->wrapnext, o->wrapnext);
    if (f->last_graphic_char != o->last_graphic_char)
        DIFF("last graphic char %#lx vs %#lx",
             f->last_graphic_char, o->last_graphic_char);
    if (f->selstate != o->selstate)
        DIFF("selection state %d vs %d", (int)f->selstate, (int)o->selstate);
    if (f->utf8.state != o->utf8.state)
        DIFF("utf8 decoder state %d vs %d", f->utf8.state, o->utf8.state);
    if (sbf != sbo)
        DIFF("scrollback %d vs %d lines", sbf, sbo);

    for (y = -(sbf < sbo ? sbf : sbo); y < f->rows && !bad; y++) {
        termline *lf = term_get_line(f, y), *lo = term_get_line(o, y);
        if (lf->cols != lo->cols || lf->lattr != lo->lattr ||
            lf->trusted != lo->trusted) {
            DIFF("line %d: cols %d/%d lattr %#x/%#x trusted %d/%d", y,
                 lf->cols, lo->cols, lf->lattr, lo->lattr,
                 lf->trusted, lo->trusted);
        } else {
            /* The columns, then each cell's combining chain by walking it:
             * the storage behind the columns holds the chains in whatever
             * order they were built, and a free slot's other fields are
             * never initialised, so only the chain's characters count. */
            for (x = 0; x < lf->cols && !bad; x++) {
                const termchar *cf = &lf->chars[x], *co = &lo->chars[x];
                const char *uf = term_link_uri(f, cf->link);
                const char *uo = term_link_uri(o, co->link);
                if (cf->chr != co->chr || cf->attr != co->attr ||
                    !same_termchar_colour(cf, co))
                    DIFF("line %d col %d: chr %#lx/%#lx attr %#lx/%#lx",
                         y, x, cf->chr, co->chr, cf->attr, co->attr);
                else if (cf->link != co->link || !uf != !uo ||
                         (uf && strcmp(uf, uo)))
                    DIFF("line %d col %d: link %#x/%#x (%s / %s)", y, x,
                         cf->link, co->link, uf ? uf : "none",
                         uo ? uo : "none");
                while (!bad && (cf->cc_next || co->cc_next)) {
                    if (!cf->cc_next || !co->cc_next) {
                        DIFF("line %d col %d: combining chain lengths differ",
                             y, x);
                        break;
                    }
                    cf += cf->cc_next;
                    co += co->cc_next;
                    if (cf->chr != co->chr)
                        DIFF("line %d col %d: combining %#lx vs %#lx",
                             y, x, cf->chr, co->chr);
                }
            }
        }
        term_release_line(lf);
        term_release_line(lo);
    }
#undef DIFF
    cases++;
    if (bad)
        failures++;
}

/* The same input into both: f with KiTTY's text path (the plain-text fast
 * path, scrollback compressed later), o with upstream's (per character,
 * every scrolled-off line compressed at once). */
static void feed_both(Terminal *f, Terminal *o, const char *data, size_t len,
                      int maxchunk, unsigned long seed)
{
    term_textrun_off = false;
    term_sbdefer_off = false;
    feed(f, data, len, maxchunk, seed);
    term_textrun_off = true;
    term_sbdefer_off = true;
    feed(o, data, len, maxchunk, seed);
    term_textrun_off = false;
    term_sbdefer_off = false;
}

/* One case: fresh terminals, optional set-up, the input three ways (whole,
 * in chunks of up to 3 bytes, of up to 17) - each way compared, before and
 * after the deferred scrollback compression. */
typedef void (*setup_fn)(Terminal *term);

static void run_case(const char *what, const char *data, size_t len,
                     int cols, int rows, bool ambig, setup_fn setup)
{
    static const int chunkings[] = { 0, 3, 17 };
    size_t i;
    char name[160];
    for (i = 0; i < lenof(chunkings); i++) {
        Mock *f = mock_new(cols, rows, ambig), *o = mock_new(cols, rows, ambig);
        if (setup) { setup(f->term); setup(o->term); }
        feed_both(f->term, o->term, data, len, chunkings[i], 4242 + i);
        snprintf(name, sizeof(name), "%s [chunks %d]", what, chunkings[i]);
        compare(name, f->term, o->term);
        /* the scrollback that waited is compressed now: still the same */
        term_sb_compact_now(f->term);
        snprintf(name, sizeof(name), "%s [chunks %d, compressed]", what,
                 chunkings[i]);
        compare(name, f->term, o->term);
        mock_free(f);
        mock_free(o);
    }
}

#define CASE(what, str) run_case(what, str, sizeof(str) - 1, 80, 24, false, NULL)

static void setup_trusted(Terminal *term) { term_set_trust_status(term, true); }

/* A selection over row 2, columns 10-29, as a mouse drag would leave it. */
static void setup_selection(Terminal *term)
{
    term->selstart.x = 10; term->selstart.y = 2;
    term->selend.x = 30; term->selend.y = 2;
    term->selstate = SELECTED;
}

/* One cell selected: row 3, column 12 - the right half of a double-width
 * character written at column 11. The ordinary path never checks that cell
 * on its own, so the selection must survive the write there too. */
static void setup_selection_half(Terminal *term)
{
    term->selstart.x = 11; term->selstart.y = 2;
    term->selend.x = 12; term->selend.y = 2;
    term->selstate = SELECTED;
}

/* One cell selected just past what is written ("ab" at columns 11-12 leaves
 * the cursor on column 13): only the check of the cursor's cell after each
 * move reaches it, so it tells a missing one of those apart. */
static void setup_selection_after(Terminal *term)
{
    term->selstart.x = 12; term->selstart.y = 2;
    term->selend.x = 13; term->selend.y = 2;
    term->selstate = SELECTED;
}

static void test_fixed_cases(void)
{
    char buf[8192];
    size_t n, i;

    CASE("plain text", "hello, world");
    CASE("lines with CR LF", "one\r\ntwo\r\nthree\r\n");
    CASE("exactly the width, then more",
         "0123456789012345678901234567890123456789"
         "0123456789012345678901234567890123456789" "X\r\nY");
    CASE("width minus one, then a newline",
         "012345678901234567890123456789012345678901234567890123456789"
         "0123456789012345678\r\n");
    CASE("colours: SGR, 256, true colour",
         "\033[1;31mred\033[0m \033[38;5;42mgreen\033[48;2;10;20;30mtc\033[0m end");
    CASE("erase in line mid-run", "abcdef\033[3D\033[Kxyz");
    CASE("UTF-8 width 1", "h\xc3\xa9llo w\xc3\xb6rld \xc3\x9f \xe2\x82\xac \xd0\x96");
    CASE("wide characters", "ab\xe4\xb8\xad\xe6\x96\x87" "cd");
    CASE("combining acute after e", "e\xcc\x81 and a\xcc\x88\xcc\x81 x");
    CASE("emoji (4-byte)", "go \xf0\x9f\x9a\x80 now");
    CASE("REP repeats the last character", "ab\033[5bc");
    CASE("tab and backspace", "a\tb\bX\t\tc");
    CASE("line drawing", "\033(0lqqqk\033(B plain");
    CASE("SO/SI", "a\016bc\017d");
    CASE("insert mode", "abcdefgh\033[4D\033[4hXY\033[4lZ");
    CASE("auto-wrap off",
         "\033[?7l0123456789012345678901234567890123456789"
         "0123456789012345678901234567890123456789OVERFLOWING\033[?7h");
    CASE("VT52 mode", "\033[?2lAB\033<cd");
    CASE("scroll region",
         "\033[5;10r\033[10;1Hline1\r\nline2\r\nline3\r\nline4\r\nline5\r\n"
         "line6\r\nline7 which is long enough to wrap past the right margin "
         "of an eighty column terminal window\033[r");
    CASE("UTF-8 separators and BOM", "a\xe2\x80\xa8" "b\xef\xbb\xbf" "c");
    CASE("malformed UTF-8",
         "x\xc0\x80y\x80z\xed\xa0\x80w\xf8\x88\x80\x80\x80v\xe4\xb8u\xef\xbf\xbe.");
    CASE("C1 in UTF-8", "a\xc2\x9b" "31mb\xc2\x85" "c");
    /* OSC 8: the link rides on every cell written while it is open, wide
     * and combined cells and a wrap included; erasing takes it away */
    CASE("OSC 8 link, closed with ST and BEL",
         "see \033]8;;https://example.org/a\033\\the docs\033]8;;\033\\ now, "
         "\033]8;id=x;http://example.org/b\007two\033]8;;\007 done");
    CASE("OSC 8 link over wide and combining text",
         "\033]8;;https://example.org/w\033\\\xe4\xb8\xad" "e\xcc\x81" "x"
         "\033]8;;\033\\y");
    CASE("OSC 8 link erased in part",
         "\033]8;;https://example.org/e\033\\abcdefgh\033]8;;\033\\\033[4D\033[K");

    /* an OSC 8 link open across many wraps and scrolls, into the scrollback */
    n = 0;
    n += snprintf(buf + n, sizeof(buf) - n, "\033]8;;https://example.org/long\033\\");
    for (i = 0; i < 3000; i++) buf[n++] = 'a' + (i % 26);
    n += snprintf(buf + n, sizeof(buf) - n, "\033]8;;\033\\tail");
    run_case("OSC 8 link through the scrollback", buf, n, 40, 6, false, NULL);

    /* a wide character that lands on the last column */
    n = 0;
    for (i = 0; i < 79; i++) buf[n++] = 'a' + (i % 26);
    memcpy(buf + n, "\xe4\xb8\xad" "b", 4); n += 4;
    run_case("wide character at the margin", buf, n, 80, 24, false, NULL);

    /* overwriting wide and combining cells, splitting them */
    CASE("overwrite a wide character's halves",
         "\xe4\xb8\xad\xe6\x96\x87\xe4\xb8\xad\r" "a\r\n"
         "\xe4\xb8\xad\xe6\x96\x87\033[1;2Hb");
    CASE("overwrite combined cells", "e\xcc\x81" "e\xcc\x81" "e\xcc\x81\rxy");

    /* long unbroken text: many wraps and scrolls, into the scrollback */
    n = 0;
    for (i = 0; i < 5000; i++) buf[n++] = ' ' + (i * 7) % 95;
    run_case("5000 characters, wrapping", buf, n, 80, 24, false, NULL);
    run_case("5000 characters, 33 columns", buf, n, 33, 7, false, NULL);

    run_case("trusted lines", "a trusted line of text that is long enough "
             "to reach the end of the line and wrap", 86, 60, 10, false,
             setup_trusted);
    {
        static const char s[] = "\033[3;1Hoverwriting the selected part of row 3";
        run_case("a selection overwritten", s, sizeof(s) - 1, 80, 24, false,
                 setup_selection);
    }
    {
        static const char s[] = "\033[10;1Htext far away from the selection";
        run_case("a selection left alone", s, sizeof(s) - 1, 80, 24, false,
                 setup_selection);
    }
    {
        static const char s[] = "\033[3;11H\xe4\xb8\xad" "x";
        run_case("a selection on a wide character's right half", s,
                 sizeof(s) - 1, 80, 24, false, setup_selection_half);
    }
    {
        static const char s[] = "\033[3;11Hab";
        run_case("a selection on the cell after the text", s, sizeof(s) - 1,
                 80, 24, false, setup_selection_after);
    }
    /* long Chinese text: every margin meets a double-width character at an
     * odd width, and a mixed line of CJK, emoji and ASCII */
    n = 0;
    for (i = 0; i < 700 && n < sizeof(buf) - 8; i++) {
        memcpy(buf + n, (i % 3) ? "\xe4\xb8\xad" : "\xe6\x96\x87", 3);
        n += 3;
        if (i % 17 == 0) buf[n++] = 'a';
        if (i % 29 == 0) { memcpy(buf + n, "\xf0\x9f\x98\x80", 4); n += 4; }
    }
    run_case("Chinese text, 80 columns", buf, n, 80, 24, false, NULL);
    run_case("Chinese text, 33 columns", buf, n, 33, 7, false, NULL);
    run_case("Chinese text, 2 columns", buf, n, 2, 5, false, NULL);
    run_case("Chinese text, trusted lines", buf, n, 40, 9, false, setup_trusted);
    {
        /* ambiguous-width characters: width 2 when cjk_ambig_wide is on */
        static const char s[] = "a\xc2\xb1" "b\xc2\xa7" "c\xce\xb1" "d";
        run_case("ambiguous width, narrow", s, sizeof(s) - 1, 80, 24, false, NULL);
        run_case("ambiguous width, wide", s, sizeof(s) - 1, 80, 24, true, NULL);
    }
}

/* The scrollback while lines wait uncompressed: overflowing it (the
 * mock's scrollback is 200 lines), and a resize pulling lines back onto the
 * screen and pushing them out again, with text in between. */
static void test_scrollback(void)
{
    static char buf[40000];
    size_t n = 0;
    int i, step;
    char name[96];

    for (i = 0; i < 1500 && n < sizeof(buf) - 64; i++)
        n += snprintf(buf + n, sizeof(buf) - n, "line %d %s\r\n", i,
                      (i % 7) ? "plain" : "\033[1;32mcoloured\033[0m e\xcc\x81 "
                      "\xe4\xb8\xad");
    run_case("1500 lines through a 200-line scrollback", buf, n, 80, 24,
             false, NULL);

    for (step = 0; step < 2; step++) {
        Mock *f = mock_new(80, 24, false), *o = mock_new(80, 24, false);
        feed_both(f->term, o->term, buf, 20000, 0, 1);
        if (step)
            term_sb_compact_now(f->term);
        /* taller: lines come back from the scrollback; shorter: they go
         * back; narrower and wider on top */
        term_size(f->term, 40, 80, 200);  term_size(o->term, 40, 80, 200);
        feed_both(f->term, o->term, buf + 20000, 3000, 0, 2);
        term_size(f->term, 10, 50, 200);  term_size(o->term, 10, 50, 200);
        feed_both(f->term, o->term, buf + 23000, 3000, 0, 3);
        term_size(f->term, 30, 120, 150); term_size(o->term, 30, 120, 150);
        snprintf(name, sizeof(name), "resizes with the scrollback %s",
                 step ? "compressed first" : "still waiting");
        compare(name, f->term, o->term);
        term_sb_compact_now(f->term);
        compare(name, f->term, o->term);
        /* clearing the scrollback frees both kinds */
        term_clrsb(f->term); term_clrsb(o->term);
        feed_both(f->term, o->term, buf, 5000, 0, 4);
        compare("after clearing the scrollback", f->term, o->term);
        mock_free(f);
        mock_free(o);
    }
}

/* Seeded random mixes of everything above. */
static void test_random(void)
{
    static const char *const pieces[] = {
        "a", "Z", " ", "~", "hello", "\r\n", "\r", "\n", "\t", "\b", "\033[K",
        "\033[2J", "\033[H", "\033[5;3H", "\033[1;31m", "\033[0m", "\033[4h",
        "\033[4l", "\033[?7l", "\033[?7h", "\033[3b", "\033(0q\033(B",
        "\xc3\xa9", "\xe2\x82\xac", "\xe4\xb8\xad", "\xcc\x81", "\xf0\x9f\x98\x80",
        "\xc0\x80", "\x80", "\xe4\xb8", "\xef\xbb\xbf", "\xe2\x80\xa8", "\033c",
        "\033[3;8r", "\033[r", "\033M", "\033D", "\033[2L", "\033[3P",
        "\xe4\xb8\xad\xe6\x96\x87\xe5\xad\x97", "\xe4\xb8\xad" "a\xe6\x96\x87",
        "\xf0\x9f\x98\x80\xf0\x9f\x98\x9b",
    };
    char buf[4096];
    int round;
    char name[64];
    for (round = 0; round < 300; round++) {
        size_t n = 0;
        rng_state = 1000 + round;
        while (n < sizeof(buf) - 16) {
            const char *p = pieces[rng() % lenof(pieces)];
            size_t l = strlen(p);
            if (rng() % 3 == 0) {
                /* a run of plain text between the pieces */
                size_t r = rng() % 90, j;
                for (j = 0; j < r && n < sizeof(buf) - 16; j++)
                    buf[n++] = ' ' + rng() % 95;
            }
            if (n + l >= sizeof(buf))
                break;
            memcpy(buf + n, p, l);
            n += l;
        }
        snprintf(name, sizeof(name), "random mix %d", round);
        run_case(name, buf, n, 20 + round % 70, 5 + round % 20,
                 round % 4 == 0, round % 5 == 0 ? setup_trusted : NULL);
    }
}

/* OSC 8 on its own terms, not fast path against ordinary: which cells carry
 * which target, and which sequences make no link at all. */
#define FEED(t, s) feed((t), (s), strlen(s), 0, 1)
static const char *cell_uri(Terminal *term, int y, int x)
{
    termline *l = term_get_line(term, y);
    const char *u = term_link_uri(term, l->chars[x].link);
    term_release_line(l);
    return u;
}

static void expect_uri(const char *what, Terminal *term, int y, int x,
                       const char *want)
{
    const char *got = cell_uri(term, y, x);
    cases++;
    if (!want ? got != NULL : (!got || strcmp(got, want))) {
        printf("FAIL OSC 8 %s: row %d col %d links to %s, expected %s\n",
               what, y, x, got ? got : "nothing", want ? want : "nothing");
        failures++;
    }
}

static void test_osc8(void)
{
    static char big[4096];
    Mock *mk = mock_new(80, 24, false);
    Terminal *t = mk->term;
    size_t n;

    FEED(t, "a\033]8;;https://x.test/1\033\\bc\033]8;;\033\\d");
    expect_uri("plain: before", t, 0, 0, NULL);
    expect_uri("plain: first cell", t, 0, 1, "https://x.test/1");
    expect_uri("plain: last cell", t, 0, 2, "https://x.test/1");
    expect_uri("plain: after the close", t, 0, 3, NULL);

    /* the same id and target again: the same handle */
    FEED(t, "\r\n\033]8;id=k;https://x.test/2\033\\e\033]8;;\033\\"
            "\033]8;id=k;https://x.test/2\033\\f\033]8;;\033\\");
    {
        termline *l = term_get_line(t, 1);
        cases++;
        if (!l->chars[0].link || l->chars[0].link != l->chars[1].link) {
            printf("FAIL OSC 8 same id and target: handles %#x and %#x\n",
                   l->chars[0].link, l->chars[1].link);
            failures++;
        }
        term_release_line(l);
    }

    /* no link: a space in the target, bytes above ASCII, no URI field */
    FEED(t, "\r\n\033]8;;http://a b\033\\g\033]8;;\033\\");
    expect_uri("space in the target", t, 2, 0, NULL);
    FEED(t, "\r\n\033]8;;http://\xc3\xa9\033\\h\033]8;;\033\\");
    expect_uri("non-ASCII target", t, 3, 0, NULL);
    FEED(t, "\r\n\033]8;id=q\033\\i");
    expect_uri("no URI field", t, 4, 0, NULL);

    /* a target longer than the OSC ceiling: cut, so no link */
    n = 0;
    n += snprintf(big + n, sizeof(big) - n, "\r\n\033]8;;https://x.test/");
    while (n < 3000) big[n++] = 'z';
    n += snprintf(big + n, sizeof(big) - n, "\033\\j\033]8;;\033\\");
    feed(t, big, n, 0, 1);
    expect_uri("over the OSC ceiling", t, 5, 0, NULL);

    /* a reset ends an open link */
    FEED(t, "\r\n\033]8;;https://x.test/3\033\\k\033c");
    FEED(t, "l");
    expect_uri("after a reset", t, 0, 0, NULL);

    /* the switch off: the sequence is taken, no link is made */
    conf_set_int(t->conf, CONF_url_osc8, 0);   /* the terminal's own copy */
    FEED(t, "\033]8;;https://x.test/4\033\\m\033]8;;\033\\");
    expect_uri("switched off", t, 0, 1, NULL);
    mock_free(mk);
}

int main(void)
{
    test_fixed_cases();
    test_osc8();
    test_scrollback();
    test_random();
    if (failures) {
        printf("Test suite FAILED (%d of %d comparisons)\n", failures, cases);
        return 1;
    }
    printf("Test suite passed (%d comparisons)\n", cases);
    return 0;
}
