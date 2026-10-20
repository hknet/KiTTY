/*
 * test_reflow - a new width re-wraps the main screen and the scrollback
 * (terminal/kitty_reflow.c, RewrapOnResize).
 *
 * The main check: text written at one width and then resized to another
 * must read exactly as the same text written at the other width - every
 * cell (character, attributes, colour, combining chain), the wrap marks
 * between rows and the cursor's place in its line. It runs a seeded mix
 * of words, colours, wide and combining characters and line ends through
 * a series of widths, with the scrollback kept uncompressed, compressed
 * and compressed while waiting, and with small slices so most rows are
 * re-wrapped by the timer's steps - also when a second resize comes
 * before they are done.
 *
 * Then what follows the text: the view scrolled back keeps its top line
 * (also inside the rows the timer still has to do),
 * the prompt marks (OSC 133) stay on a line's first row, rows pushed up by
 * a shorter window still come back after a width change, the main screen
 * is re-wrapped while the alternate one is shown, a full scrollback drops
 * from the top, double-width rows stay as they are, and the switch off
 * leaves the rows as PuTTY does. The pictures' anchors are in
 * test/test_osc52.c (test_kitty_graphics), which has the picture harness.
 *
 * Built against terminal.c compiled WITH MOD_PERSO (and MOD_FAR2L, like the
 * kitty target), as test_textrun. Returns non-zero on any failure.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "putty.h"
#include "terminal.h"
#include "kitty_term_int.h"

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

const char *appname = "test_reflow";


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
void kitty_term_send_reply(Terminal *term, const char *data, size_t len) { }
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
void kitty_osc52_read_confirm(Terminal *term, const wchar_t *clip, int clip_len,
                              const char *claim) { }
void kitty_osc52_read_confirm_end(Terminal *term) { }
bool kitty_osc52_save_deny_for_host(Terminal *term) { return true; }
bool kitty_osc52_store_readonly(void) { return false; }
bool kitty_osc52_title_visible(void) { return true; }
void kitty_osc52_notify(Terminal *term, const char *t, const char *m, int a) { }
void kitty_osc52_state_changed(Terminal *term) { }
void kitty_ctx3008_user_changed(Terminal *term) { }
void kitty_ctx3008_chain_changed(Terminal *term) { }
void kitty_osc52_write_confirm(Terminal *term) { }
void kitty_osc52_write_confirm_end(Terminal *term) { }
void kitty_transfer_osc(Terminal *term) { (void)term; }
void kitty_transfer_free(Terminal *term) { (void)term; }
/* The far2l platform seams (kitty/kitty_far2l.h): not reached by plain text. */
uint32_t kitty_far2l_clip_register(const char *name, size_t len) { return 0; }
bool kitty_far2l_clip_available(uint32_t fmt) { return false; }
bool kitty_far2l_clip_empty(void) { return false; }
bool kitty_far2l_clip_set(uint32_t fmt, const unsigned char *data, size_t len,
                          bool empty_first) { return false; }
unsigned char *kitty_far2l_clip_get(uint32_t fmt, size_t *len)
{ *len = 0; return NULL; }
void kitty_far2l_confirm(Terminal *term, bool offer_always) { }
void kitty_far2l_confirm_end(Terminal *term) { }
bool kitty_far2l_save_client_ids(Terminal *term) { return false; }
bool kitty_far2l_max_cells(Terminal *term, int *rows, int *cols) { return false; }
void kitty_far2l_events_changed(Terminal *term) { }
void kitty_hostnotify_osc(Terminal *term, unsigned osc, const char *s,
                          size_t len, bool overflow)
{ (void)term; (void)osc; (void)s; (void)len; (void)overflow; }
void kitty_hostnotify_term_free(Terminal *term) { (void)term; }
void kitty_host_notice(Terminal *term, const char *title, const char *body)
{ (void)term; (void)title; (void)body; }

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


static Mock *mock_new(int cols, int rows, int savelines)
{
    Mock *mk = snew(Mock);
    memset(mk, 0, sizeof(*mk));
    mk->conf = conf_new();
    do_defaults(NULL, mk->conf);
    init_ucs_generic(mk->conf, mk->ucsdata);
    mk->ucsdata->line_codepage = CP_UTF8;
    mk->tw.vt = &mock_termwin_vt;
    mk->term = term_init(mk->conf, mk->ucsdata, &mk->tw);
    term_pwron(mk->term, true);
    /* as in a running session: before one, the lines are the local
     * prompts, trusted, and keep room for the trust sigil (test_trusted) */
    term_set_trust_status(mk->term, false);
    term_size(mk->term, rows, cols, savelines);
    return mk;
}

static void mock_free(Mock *mk)
{
    term_free(mk->term);
    conf_free(mk->conf);
    sfree(mk);
}

static int failures = 0, cases = 0;

static void check(bool ok, const char *what, const char *fmt, ...)
{
    cases++;
    if (!ok) {
        va_list ap;
        failures++;
        printf("FAIL %s: ", what);
        va_start(ap, fmt);
        vprintf(fmt, ap);
        va_end(ap);
        putchar('\n');
    }
}

static void put(Mock *mk, const char *s)
{
    term_data(mk->term, s, strlen(s));
}

/* A seeded generator, so a failure reproduces. */
static unsigned long rng_state;
static unsigned rng(void)
{
    rng_state = rng_state * 1103515245UL + 12345UL;
    return (unsigned)((rng_state >> 16) & 0x7FFF);
}

/* As kitty_reflow.c counts a line's end: blanks that show nothing (a text
 * colour on a blank shows nothing) are not text. */
static bool cell_blank(Terminal *t, const termchar *c)
{
    unsigned long shown = ATTR_BGMASK | ATTR_UNDER | ATTR_REVERSE |
        ATTR_STRIKE;
    return (c->chr == t->basic_erase_char.chr || c->chr == ' ') &&
        !((c->attr ^ t->basic_erase_char.attr) & shown) && !c->cc_next &&
        !c->truecolour.bg.enabled && !c->link;
}

/* One row as text: each cell's character, attributes, colour and
 * combining chain, up to its last cell that is not a plain blank. */
static strbuf *row_dump(Terminal *t, termline *l)
{
    strbuf *s = strbuf_new();
    int x, len = l->cols;
    while (len > 0 && cell_blank(t, &l->chars[len - 1]))
        len--;
    for (x = 0; x < len; x++) {
        termchar *c = &l->chars[x];
        put_fmt(s, "%lx/%lx", c->chr, c->attr);
        if (c->truecolour.fg.enabled)
            put_fmt(s, "/%d.%d.%d", c->truecolour.fg.r, c->truecolour.fg.g,
                    c->truecolour.fg.b);
        while (c->cc_next) {
            c += c->cc_next;
            put_fmt(s, "+%lx", c->chr);
        }
        put_byte(s, ' ');
    }
    return s;
}

/* The scrollback and the screen, a row a line; '+' after a row that wraps
 * into a following row (also '2' when a wide character did not fit). The
 * empty rows after the last text are left out, and so is the wrap mark of
 * the last row with text: the cursor's own empty row decides it. */
static strbuf *dump(Terminal *t)
{
    int sb = count234(t->scrollback), total = sb + t->rows, y, last;
    strbuf **rs = snewn(total, strbuf *), *out = strbuf_new();
    unsigned *la = snewn(total, unsigned);

    for (y = -sb; y < t->rows; y++) {
        termline *l = term_get_line(t, y);
        rs[y + sb] = row_dump(t, l);
        la[y + sb] = l->lattr;
        term_release_line(l);
    }
    for (last = total - 1; last >= 0 && rs[last]->len == 0; last--)
        ;
    for (y = 0; y < total; y++) {
        if (y <= last) {
            put_datapl(out, ptrlen_from_strbuf(rs[y]));
            if (y < last && (la[y] & LATTR_WRAPPED))
                put_byte(out, (la[y] & LATTR_WRAPPED2) ? '2' : '+');
            put_byte(out, '\n');
        }
        strbuf_free(rs[y]);
    }
    sfree(rs);
    sfree(la);
    return out;
}

/* The cursor's place counted from the top of the scrollback, a pending
 * wrap as the next row's column 0. */
static void cursor_at(Terminal *t, int *row, int *col)
{
    *row = count234(t->scrollback) + t->curs.y;
    *col = t->curs.x + (t->wrapnext ? 1 : 0);
    if (*col >= t->cols) {
        (*row)++;
        *col -= t->cols;
    }
}

/* The text of row y (characters only, ASCII), for the targeted checks. */
static void row_text(Terminal *t, int y, char *buf, int size)
{
    termline *l = term_get_line(t, y);
    int x, n = 0;
    for (x = 0; x < l->cols && n < size - 1; x++) {
        unsigned long c = l->chars[x].chr;
        if ((c & ~0xFFUL) == CSET_ASCII)
            c &= 0xFF;
        buf[n++] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
    }
    while (n > 0 && buf[n - 1] == ' ')
        n--;
    buf[n] = '\0';
    term_release_line(l);
}

/* Words, colours, wide and combining characters, long runs and line ends;
 * never a blank before a line end (a line's trailing blanks are not text,
 * as in every terminal that re-wraps). */
static char *gen(unsigned long seed, size_t target)
{
    strbuf *b = strbuf_new();
    bool linestart = true;
    rng_state = seed;
    while (b->len < target) {
        unsigned k = rng() % 22, i, n;
        if (k < 12 || k == 21) {
            n = (k == 21) ? 30 + rng() % 90 : 1 + rng() % 12;
            if (!linestart)
                put_byte(b, ' ');
            for (i = 0; i < n; i++)
                put_byte(b, 'a' + rng() % 26);
            linestart = false;
        } else if (k < 15) {
            put_datapl(b, PTRLEN_LITERAL("\r\n"));
            linestart = true;
        } else if (k == 15) {
            put_datapl(b, PTRLEN_LITERAL("\xE4\xB8\xAD"));   /* wide */
            linestart = false;
        } else if (k == 16) {
            put_datapl(b, PTRLEN_LITERAL("e\xCC\x81"));      /* e + acute */
            linestart = false;
        } else if (k == 17) {
            put_fmt(b, "\033[3%dm", 1 + rng() % 7);
        } else if (k == 18) {
            put_datapl(b, PTRLEN_LITERAL("\033[0m"));
        } else if (k == 19) {
            put_datapl(b, PTRLEN_LITERAL("\033[1m"));
        } else {
            put_datapl(b, PTRLEN_LITERAL("\033[38;2;10;200;30m"));
        }
    }
    return strbuf_to_str(b);
}

static void first_diff(const char *a, const char *b, int *line)
{
    int l = 0;
    while (*a && *a == *b) {
        if (*a == '\n')
            l++;
        a++, b++;
    }
    *line = l;
}

/* The older rows a width change leaves for the timer: all of them now (the
 * test build has no timer). */
static void settle(Terminal *t)
{
    while (t->rf_pend)
        kitty_reflow_step(t);
}

/* Written at one width and resized through others must read as written at
 * each of them. mode 0: lines wait uncompressed; 1: compressed as they go;
 * 2: the waiting ones compressed before each resize; 3: small slices, so
 * most rows are done by the timer's steps; 4: the same, and each resize
 * goes first to another width, whose steps stop after one - the rows left
 * from then are cut at a width that is neither the old nor the new one;
 * 5: a frame drag resting at two widths, the default sizes - only the rows
 * near the screen are done until the drag ends. */
static void test_roundtrip(unsigned long seed, int mode)
{
    static const int widths[] = { 80, 37, 120, 13, 80, 2, 61, 3, 80 };
    char what[64], *text = gen(seed, 6000);
    Mock *a = mock_new(widths[0], 24, 5000);
    size_t i;

    term_sbdefer_off = (mode == 1);
    kitty_rf_sync_rows = mode >= 3 ? 30 : 5000;
    kitty_rf_slice_rows = mode >= 3 ? 23 : 5000;
    put(a, text);
    for (i = 1; i < lenof(widths); i++) {
        Mock *b = mock_new(widths[i], 24, 5000);
        strbuf *da, *db;
        int ra, ca, rb, cb, line;
        if (mode == 2)
            term_sb_compact_now(a->term);
        if (mode >= 3)                 /* the view half way up */
            a->term->disptop = -(int)count234(a->term->scrollback) / 2;
        if (mode == 4) {
            term_size(a->term, 24, widths[i] + 17, 5000);
            kitty_reflow_step(a->term);
        }
        if (mode == 5) {
            /* a frame drag: rests at another width and at this one; a
             * step meanwhile does nothing, the rest comes at the end */
            long pend;
            term_set_dragging(a->term, true);
            term_size(a->term, 24, widths[i] + 17, 5000);
            pend = a->term->rf_pend;
            kitty_reflow_step(a->term);
            snprintf(what, sizeof(what), "drag seed %lu width %d", seed,
                     widths[i]);
            check(a->term->rf_pend == pend, what,
                  "a step ran during the drag (pending %ld -> %ld)", pend,
                  a->term->rf_pend);
            term_size(a->term, 24, widths[i], 5000);
            term_set_dragging(a->term, false);
        } else
            term_size(a->term, 24, widths[i], 5000);
        settle(a->term);
        put(b, text);
        da = dump(a->term);
        db = dump(b->term);
        cursor_at(a->term, &ra, &ca);
        cursor_at(b->term, &rb, &cb);
        snprintf(what, sizeof(what), "roundtrip seed %lu mode %d width %d",
                 seed, mode, widths[i]);
        first_diff(da->s, db->s, &line);
        check(!strcmp(da->s, db->s), what,
              "the text differs from row %d on (%d rows resized, %d written)",
              line, (int)count234(a->term->scrollback) + a->term->rows,
              (int)count234(b->term->scrollback) + b->term->rows);
        check(ra == rb && ca == cb, what, "cursor %d,%d, written %d,%d",
              ra, ca, rb, cb);
        strbuf_free(da);
        strbuf_free(db);
        mock_free(b);
    }
    term_sbdefer_off = false;
    kitty_rf_sync_rows = kitty_rf_slice_rows = 5000;
    mock_free(a);
    sfree(text);
}

static void long_line(Mock *mk, char c, int n);

/* A view scrolled back into the rows the timer still has to do keeps its
 * top line through every step. */
static void test_view_pending(void)
{
    Mock *a = mock_new(80, 24, 5000);
    Terminal *t = a->term;
    char before[128], after[128], buf[16];
    int i, steps = 0;
    bool same = true;

    kitty_rf_sync_rows = 40;
    kitty_rf_slice_rows = 25;
    for (i = 0; i < 120; i++) {
        snprintf(buf, sizeof(buf), "A%03d", i);
        put(a, buf);
        long_line(a, 'a', 96);
        put(a, "\r\n");
    }
    t->disptop = -150;                 /* far above the newest 40 rows */
    row_text(t, t->disptop, before, sizeof(before));
    if (before[0] != 'A') {            /* the second row of a line */
        t->disptop--;
        row_text(t, t->disptop, before, sizeof(before));
    }
    term_size(t, 24, 30, 5000);        /* 2 rows a line become 4 */
    row_text(t, t->disptop, after, sizeof(after));
    if (strncmp(before, after, 4))
        same = false;
    /* the rows in view are re-wrapped with the resize, not by the timer */
    check(strlen(after) == 30 && t->rf_pend, "view re-wrapped at once",
          "the top row has %d characters, want 30 (the new width)%s",
          (int)strlen(after), t->rf_pend ? "" : "; nothing left for the timer");
    while (t->rf_pend) {
        kitty_reflow_step(t);
        steps++;
        row_text(t, t->disptop, after, sizeof(after));
        if (strncmp(before, after, 4))
            same = false;
    }
    check(steps > 1 && same && before[0] == 'A', "view in the rows left",
          "%d steps, top line '%.8s', was '%.8s'", steps, after, before);
    kitty_rf_sync_rows = kitty_rf_slice_rows = 5000;
    mock_free(a);
}

static void long_line(Mock *mk, char c, int n)
{
    char *s = snewn(n + 1, char);
    memset(s, c, n);
    s[n] = '\0';
    put(mk, s);
    sfree(s);
}

/* A view scrolled back keeps its top line while the rows above it change. */
static void test_view(void)
{
    Mock *a = mock_new(80, 24, 5000);
    Terminal *t = a->term;
    char before[128], after[128], buf[16];
    int i;

    for (i = 0; i < 50; i++) {
        snprintf(buf, sizeof(buf), "A%02d", i);
        put(a, buf);
        long_line(a, 'a', 97);
        put(a, "\r\n");
    }
    for (i = 0; i < 40; i++) {
        snprintf(buf, sizeof(buf), "S%02d\r\n", i);
        put(a, buf);
    }
    t->disptop = -10;
    row_text(t, t->disptop, before, sizeof(before));
    term_size(t, 24, 40, 5000);
    row_text(t, t->disptop, after, sizeof(after));
    check(!strcmp(before, after) && before[0] == 'S', "view narrower",
          "top line '%s', was '%s'", after, before);
    term_size(t, 24, 120, 5000);
    row_text(t, t->disptop, after, sizeof(after));
    check(!strcmp(before, after), "view wider", "top line '%s', was '%s'",
          after, before);
    mock_free(a);
}

/* The rows carrying a mark bit, and whether each starts its line. */
static int marked(Terminal *t, unsigned bit, bool *starts)
{
    int sb = count234(t->scrollback), y, n = 0;
    bool prev_wraps = false;
    *starts = true;
    for (y = -sb; y < t->rows; y++) {
        termline *l = term_get_line(t, y);
        if (l->lattr & bit) {
            n++;
            if (prev_wraps)
                *starts = false;
        }
        prev_wraps = (l->lattr & LATTR_WRAPPED) != 0;
        term_release_line(l);
    }
    return n;
}

/* A prompt mark stays on its line's first row. */
static void test_marks(void)
{
    Mock *a = mock_new(80, 24, 1000);
    bool starts;
    int n;

    put(a, "before\r\n");
    long_line(a, 'x', 70);
    put(a, "\033]133;A\007");      /* mid-row: the row is marked */
    long_line(a, 'p', 100);
    put(a, "\r\n");
    term_size(a->term, 24, 30, 1000);
    n = marked(a->term, LATTR_PROMPT, &starts);
    check(n == 1 && starts, "marks narrower", "%d marked rows, first %d",
          n, starts);
    term_size(a->term, 24, 120, 1000);
    n = marked(a->term, LATTR_PROMPT, &starts);
    check(n == 1 && starts, "marks wider", "%d marked rows, first %d", n,
          starts);
    mock_free(a);
}

/* Rows a shorter window pushed up come back when it grows again, also
 * after a width change - in between or in the same resize. */
static void test_pushed_rows(int how)
{
    Mock *a = mock_new(80, 24, 1000);
    Terminal *t = a->term;
    char before[64], after[64], buf[16], what[48];
    int i, cy;

    for (i = 0; i < 30; i++) {
        snprintf(buf, sizeof(buf), "T%02d\r\n", i);
        put(a, buf);
    }
    row_text(t, 0, before, sizeof(before));
    cy = t->curs.y;
    term_size(t, 10, 80, 1000);
    if (how == 0) {
        term_size(t, 10, 60, 1000);
        term_size(t, 24, 60, 1000);
    } else
        term_size(t, 24, 50, 1000);
    row_text(t, 0, after, sizeof(after));
    snprintf(what, sizeof(what), "pushed rows back (%s)",
             how == 0 ? "narrower, then taller" : "both at once");
    check(!strcmp(before, after) && t->curs.y == cy, what,
          "top row '%s' (was '%s'), cursor row %d (was %d)", after, before,
          t->curs.y, cy);
    mock_free(a);
}

/* The cursor keeps its row on the screen: at the bottom, a wider window
 * brings the earlier rows down above it; at the top of a cleared screen,
 * the prompt stays at the top and the cleared text does not come back. */
static void test_screen_rows(void)
{
    Mock *a = mock_new(80, 24, 1000);
    Terminal *t = a->term;
    char buf[160];
    int i;

    for (i = 0; i < 40; i++) {
        snprintf(buf, sizeof(buf), "B%02d\r\n", i);
        put(a, buf);
    }
    for (i = 0; i < 10; i++) {
        snprintf(buf, sizeof(buf), "L%02d", i);
        put(a, buf);
        long_line(a, 'l', 117);
        put(a, "\r\n");
    }
    term_size(t, 24, 130, 1000);
    row_text(t, 22, buf, sizeof(buf));
    check(t->curs.y == 23 && !strncmp(buf, "L09", 3), "cursor at the bottom",
          "cursor row %d, the row above '%.8s'", t->curs.y, buf);
    row_text(t, 0, buf, sizeof(buf));
    check(!strcmp(buf, "B27"), "cursor at the bottom",
          "the top row '%s', want B27 brought down", buf);
    mock_free(a);

    a = mock_new(80, 24, 1000);
    t = a->term;
    for (i = 0; i < 40; i++) {
        snprintf(buf, sizeof(buf), "C%02d\r\n", i);
        put(a, buf);
    }
    put(a, "\033[2J\033[H$ ");
    term_size(t, 24, 60, 1000);
    term_size(t, 24, 100, 1000);
    row_text(t, 0, buf, sizeof(buf));
    check(t->curs.y == 0 && t->curs.x == 2 && !strcmp(buf, "$"),
          "cleared screen", "cursor %d,%d, the top row '%s'", t->curs.x,
          t->curs.y, buf);
    row_text(t, 1, buf, sizeof(buf));
    check(!buf[0], "cleared screen", "row 1 '%s', want empty", buf);
    mock_free(a);
}

#define PROMPT "\033]133;A\007user@host:~$ \033]133;B\007"
#define TYPED "echo xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"

/* How many rows start with s, and the row before the first of them. */
static int rows_starting(Terminal *t, const char *s, char *before, int size)
{
    int sb = count234(t->scrollback), y, n = 0;
    char buf[256], prev[256] = "";
    for (y = -sb; y < t->rows; y++) {
        row_text(t, y, buf, sizeof(buf));
        if (!strncmp(buf, s, strlen(s))) {
            if (!n++)
                snprintf(before, size, "%s", prev);
        }
        strcpy(prev, buf);
    }
    return n;
}

/* At its prompt (OSC 133), the shell draws the prompt again on a new size:
 * it moves up by the rows it drew it on, clears and draws. The bytes are
 * what bash 5.3 and zsh sent in a pseudo-terminal: the
 * prompt and the typed line must come out once, the output above intact. */
static void shell_redraw(const char *what, int from, int to,
                         const char *redraw, bool marks)
{
    Mock *a = mock_new(from, 24, 1000);
    char before[256] = "";
    int n;

    put(a, "out one\r\nout two\r\n");
    if (marks)
        put(a, PROMPT TYPED);
    else
        put(a, "user@host:~$ " TYPED);
    term_size(a->term, 24, to, 1000);
    put(a, redraw);
    n = rows_starting(a->term, "user@host", before, sizeof(before));
    if (marks)
        check(n == 1 && !strcmp(before, "out two"), what,
              "%d prompt rows, the row above '%s'", n, before);
    else
        check(n != 1 || strcmp(before, "out two"), what,
              "without the marks the shell's redraw went right: the case "
              "the prompt line is kept for no longer shows");
    mock_free(a);
}

static void test_shell_redraw(void)
{
    static const char bash_wider[] =
        "\r\033[K\r\033[A\033[K\r\033[A\033[K\r" PROMPT TYPED;
    static const char bash_narrower[] = "\r\033[K\r\033[A\033[K\r" PROMPT TYPED;
    static const char zsh_narrower[] =
        "\033[A\r\r\033[0m\033[27m\033[24m\033[J" PROMPT TYPED "\033[K";

    shell_redraw("bash at its prompt, wider", 40, 80, bash_wider, true);
    shell_redraw("bash at its prompt, narrower", 80, 40, bash_narrower, true);
    shell_redraw("zsh at its prompt, narrower", 80, 40, zsh_narrower, true);
    shell_redraw("bash without marks, wider", 40, 80, bash_wider, false);
    shell_redraw("bash without marks, narrower", 80, 40, bash_narrower,
                 false);

    /* a command running (133;C): its output line is re-wrapped */
    {
        Mock *a = mock_new(80, 24, 1000);
        char r0[128];
        put(a, PROMPT "make\r\n\033]133;C\007");
        long_line(a, 'o', 100);
        term_size(a->term, 24, 120, 1000);
        row_text(a->term, 1, r0, sizeof(r0));
        check(strlen(r0) == 100, "a command running",
              "its output row has %d characters, want 100 (re-wrapped)",
              (int)strlen(r0));
        mock_free(a);
    }
}

/* The main screen is re-wrapped while the alternate screen is shown; the
 * cursor saved on entering comes back in its new place. */
static void test_alt(void)
{
    Mock *a = mock_new(80, 24, 1000);
    Terminal *t = a->term;
    char r0[128], r2[128];

    long_line(a, 'm', 100);
    put(a, "\033[?1049h");
    put(a, "ALT");
    term_size(t, 24, 40, 1000);
    put(a, "\033[?1049l");
    row_text(t, 0, r0, sizeof(r0));
    row_text(t, 2, r2, sizeof(r2));
    check(strlen(r0) == 40 && strlen(r2) == 20 && t->curs.y == 2 &&
          t->curs.x == 20, "alternate screen",
          "rows of %d and %d, cursor %d,%d", (int)strlen(r0),
          (int)strlen(r2), t->curs.x, t->curs.y);
    mock_free(a);
}

/* The cursor in the middle of a wrapped line stays on its character. */
static void test_cursor_mid(void)
{
    Mock *a = mock_new(80, 24, 1000);
    Terminal *t = a->term;

    put(a, "\033[1;1H");
    long_line(a, 'c', 100);
    put(a, "\033[1;71H");          /* row 0, column 70 */
    term_size(t, 24, 40, 1000);
    check(t->curs.y == 1 && t->curs.x == 30, "cursor mid-line narrower",
          "cursor %d,%d", t->curs.x, t->curs.y);
    term_size(t, 24, 120, 1000);
    check(t->curs.y == 0 && t->curs.x == 70, "cursor mid-line wider",
          "cursor %d,%d", t->curs.x, t->curs.y);
    mock_free(a);
}

/* A narrower width with a full scrollback: the top goes, the end reads as
 * written at that width. */
static void test_full_pending(void)
{
    Mock *a = mock_new(80, 24, 100), *b = mock_new(30, 24, 100);
    strbuf *da, *db;
    int i;

    kitty_rf_sync_rows = 30;
    kitty_rf_slice_rows = 23;
    for (i = 0; i < 80; i++) {
        long_line(a, 'a' + i % 26, 100);
        put(a, "\r\n");
        long_line(b, 'a' + i % 26, 100);
        put(b, "\r\n");
    }
    term_size(a->term, 24, 30, 100);
    settle(a->term);
    check(count234(a->term->scrollback) == 100, "full scrollback, timer",
          "%d rows kept, want the 100 it holds",
          (int)count234(a->term->scrollback));
    da = dump(a->term);
    db = dump(b->term);
    {
        size_t n = da->len < db->len ? da->len : db->len;
        check(n > 0 && !strcmp(da->s + (da->len - n), db->s + (db->len - n)),
              "full scrollback, timer", "the newest rows differ");
    }
    strbuf_free(da);
    strbuf_free(db);
    kitty_rf_sync_rows = kitty_rf_slice_rows = 5000;
    mock_free(a);
    mock_free(b);
}

static void test_full(void)
{
    Mock *a = mock_new(80, 24, 10), *b = mock_new(30, 24, 10);
    strbuf *da, *db;
    size_t la, lb;
    int i;

    for (i = 0; i < 30; i++) {
        long_line(a, 'a' + i % 26, 100);
        put(a, "\r\n");
        long_line(b, 'a' + i % 26, 100);
        put(b, "\r\n");
    }
    term_size(a->term, 24, 30, 10);
    check(count234(a->term->scrollback) <= 10, "full scrollback",
          "%d rows kept", (int)count234(a->term->scrollback));
    da = dump(a->term);
    db = dump(b->term);
    la = da->len;
    lb = db->len;
    /* the newest rows read the same */
    {
        size_t n = la < lb ? la : lb;
        check(n > 0 && !strcmp(da->s + (la - n), db->s + (lb - n)),
              "full scrollback", "the newest rows differ");
    }
    strbuf_free(da);
    strbuf_free(db);
    mock_free(a);
    mock_free(b);
}

/* Double-width rows stay as they are. */
static void test_double_width(void)
{
    Mock *a = mock_new(80, 24, 1000);
    Terminal *t = a->term;
    termline *l;
    char r0[128];

    put(a, "\033#6wide\r\n");
    long_line(a, 'z', 100);
    term_size(t, 24, 40, 1000);
    l = term_get_line(t, 0);
    check((l->lattr & LATTR_MODE) == LATTR_WIDE, "double width",
          "lattr %#x", l->lattr);
    term_release_line(l);
    row_text(t, 0, r0, sizeof(r0));
    check(!strcmp(r0, "wide"), "double width", "row 0 '%s'", r0);
    mock_free(a);
}

/* Trusted rows (the local prompts before a session) stay as they are. */
static void test_trusted(void)
{
    Mock *a = mock_new(80, 24, 1000);
    Terminal *t = a->term;
    termline *l;
    char r0[128];

    term_set_trust_status(t, true);
    long_line(a, 't', 100);
    term_size(t, 24, 40, 1000);
    l = term_get_line(t, 0);
    check((l->lattr & LATTR_WRAPPED) && l->trusted && l->cols >= 80,
          "trusted rows", "row 0 lattr %#x, trusted %d, %d columns",
          l->lattr, l->trusted, l->cols);
    term_release_line(l);
    row_text(t, 0, r0, sizeof(r0));
    check(strlen(r0) == 80 - 3, "trusted rows", "row 0 has %d characters",
          (int)strlen(r0));
    mock_free(a);
}

/* Switched off: the rows keep their cut, as in PuTTY. */
static void test_off(void)
{
    Mock *a = mock_new(80, 24, 1000);
    Terminal *t = a->term;
    termline *l;
    char r1[128];

    conf_set_bool(a->conf, CONF_rewrap_on_resize, false);
    term_reconfig(t, a->conf);
    long_line(a, 'o', 100);
    term_size(t, 24, 40, 1000);
    term_size(t, 24, 120, 1000);
    l = term_get_line(t, 0);
    check((l->lattr & LATTR_WRAPPED) && l->cols >= 80, "switched off",
          "row 0 lattr %#x, %d columns", l->lattr, l->cols);
    term_release_line(l);
    row_text(t, 1, r1, sizeof(r1));
    check(strlen(r1) == 20, "switched off", "row 1 has %d characters",
          (int)strlen(r1));
    mock_free(a);
}

/* test_reflow bench [lines]: how long one width change takes with a big
 * scrollback - a third of the lines
 * longer than any width here, the rest short, the scrollback compressed
 * as after quiet output. Not part of the test run. */
static double now_ms(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

static int bench(int lines)
{
    static const int widths[] = { 80, 60, 100, 120, 79, 81, 80 };
    Mock *a = mock_new(120, 50, lines + 100);
    strbuf *b = strbuf_new();
    double t0;
    size_t i;
    int k;

    for (k = 0; k < lines; k++) {
        if (k % 3 == 0)
            put_fmt(b, "%07d %0190d\r\n", k, 0);   /* 198 characters */
        else
            put_fmt(b, "%07d short line of output\r\n", k);
        if (b->len > 1 << 20) {
            term_data(a->term, b->s, b->len);
            strbuf_clear(b);
        }
    }
    term_data(a->term, b->s, b->len);
    strbuf_free(b);
    term_sb_compact_now(a->term);
    printf("%d lines, %d rows in the scrollback at 120 columns\n", lines,
           (int)count234(a->term->scrollback));
    for (i = 0; i < lenof(widths); i++) {
        int before = (int)count234(a->term->scrollback);
        t0 = now_ms();
        double t1, worst = 0;
        int steps = 0;
        term_size(a->term, 50, widths[i], lines + 100);
        t1 = now_ms();
        while (a->term->rf_pend) {
            double s = now_ms();
            kitty_reflow_step(a->term);
            if (now_ms() - s > worst)
                worst = now_ms() - s;
            steps++;
        }
        printf("  %3d -> %3d columns: resize %6.1f ms, then %3d steps "
               "(longest %5.1f ms, %7.1f ms in all)  (%d -> %d rows)\n",
               i ? widths[i - 1] : 120, widths[i], t1 - t0, steps, worst,
               now_ms() - t1, before, (int)count234(a->term->scrollback));
    }
    mock_free(a);
    return 0;
}

int main(int argc, char **argv)
{
    unsigned long seed;
    int mode;

    if (argc > 1 && !strcmp(argv[1], "bench"))
        return bench(argc > 2 ? atoi(argv[2]) : 200000);

    for (mode = 0; mode < 6; mode++)
        for (seed = 1; seed <= 6; seed++)
            test_roundtrip(seed * 7919 + mode, mode);
    test_view();
    test_view_pending();
    test_marks();
    test_pushed_rows(0);
    test_pushed_rows(1);
    test_screen_rows();
    test_shell_redraw();
    test_alt();
    test_cursor_mid();
    test_full();
    test_full_pending();
    test_double_width();
    test_trusted();
    test_off();

    printf("%s: %d cases, %d failures\n", failures ? "FAIL" : "PASS", cases,
           failures);
    return failures ? 1 : 0;
}
