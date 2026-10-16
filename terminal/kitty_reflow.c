/*
 * kitty_reflow.c - KiTTY++: re-wrap the main screen and the scrollback when
 * the width changes (RewrapOnResize, on by default). term_size calls
 * kitty_reflow before it moves rows for the new height.
 *
 * Rows linked by LATTR_WRAPPED are one line: joined, then cut again at the
 * new width. A wrapped row gives its cells up to the width it was cut at -
 * its own column count (on the screen at most the old width, a row there
 * may have been widened since) - one less with LATTR_WRAPPED2, where a wide
 * character did not fit; the line's last row gives its cells up to the last
 * one that shows something. A wide character never splits: it starts the
 * next row and leaves LATTR_WRAPPED2, as when it is written. A row that is
 * not wrapped and fits the new width stays as it is - a scrollback entry is
 * not even unpacked. Double-width and double-height rows stay as they are,
 * and so do trusted rows (the local prompts before a session, which keep
 * room for the trust sigil), and - while the shell sits at its prompt
 * (OSC 133) - the prompt and the line typed after it, which the shell
 * itself draws again.
 *
 * Each row costs a few microseconds, so a resize re-wraps the screen and
 * the newest RF_SYNC_ROWS rows of the scrollback at once, and the older
 * rows afterwards from a timer, RF_SLICE_ROWS at a time, newest first
 * (kitty_reflow_step; term->rf_pend is where the rows still to do end).
 * While the window frame is dragged (term_set_dragging) a resize at a rest
 * takes only the screen and two screens above it, and the timer waits for
 * the end of the drag. A view scrolled back further up is re-wrapped with
 * the resize as well, with a screen above and two below it.
 * Since every row knows the width it was cut at, rows from an earlier
 * width that a new resize finds there are re-wrapped just the same.
 *
 * Everything tied to a row moves with its text through rf_map: the cursor
 * and the saved cursor, the screen's top row, the rows that come back when
 * the window grows (tempsblines), the view's top line and the pictures'
 * anchors. The mark bits (OSC 133) go on the row that holds the start of
 * the row they were on. Narrower, the screen keeps its top row and the
 * cursor moves down (rows go up into the scrollback only when the screen
 * is full); wider, the cursor keeps its row and earlier rows come down
 * above it - a prompt at the bottom stays at the bottom, a prompt at the
 * top of a cleared screen stays at the top.
 */
#ifdef MOD_PERSO

#include <stdlib.h>
#include <string.h>

#include "putty.h"
#include "terminal.h"
#include "kitty_term_int.h"
#include "../kitty/kitty_gfx_term.h"

/* how far above the cursor a prompt mark still counts as the prompt being
 * typed at */
#define RF_PROMPT_ROWS 1000
/* scrollback rows re-wrapped with the resize itself, and per timer tick
 * after it (about 6 us a row: some 12 ms each) */
#define RF_SYNC_ROWS   2000
#define RF_SLICE_ROWS  2000
#define RF_STEP_MS     10
#ifdef KITTY_TEST_HOOKS
int kitty_rf_sync_rows = RF_SYNC_ROWS;   /* test/test_reflow.c makes them small */
int kitty_rf_slice_rows = RF_SLICE_ROWS;
#else
#define kitty_rf_sync_rows RF_SYNC_ROWS
#define kitty_rf_slice_rows RF_SLICE_ROWS
#endif

typedef struct {
    void *ent;                  /* a scrollback entry, or */
    termline *line;             /* a line */
    int off;                    /* new rows: where they start in their line */
} rf_item;

typedef struct {
    int gfirst, gcount;         /* the new rows of the row's line */
    int startoff;               /* where its column 0 went in the line;
                                 * -1: the row was kept, gfirst is its row */
} rf_old;

/* One pass over the rows lo.. (scrollback + screen counted from the top of
 * the scrollback); the arrays are indexed from lo. */
typedef struct {
    Terminal *term;
    int newcols, oldcols;
    int lo, n;                  /* the rows taken: lo .. lo+n-1 */
    int scr;                    /* the first screen row, from lo (n: none) */
    rf_item *olds;
    int *cols;
    unsigned *lattr;
    bool *trusted;
    rf_old *old;
    rf_item *items;             /* the new rows */
    int m;
    size_t size;
    long base;                  /* gfx_sb_base */
} Reflow;

static int rf_push(Reflow *rf, void *ent, termline *line, int off)
{
    sgrowarray(rf->items, rf->size, rf->m);
    rf->items[rf->m].ent = ent;
    rf->items[rf->m].line = line;
    rf->items[rf->m].off = off;
    return rf->m++;
}

/* Taken row r (from lo), column c: the new row (from lo) and column of that
 * cell. Rows past the end keep their distance to it. */
static void rf_map_local(Reflow *rf, int r, int c, int *nr, int *nc)
{
    const rf_old *o;
    int k, end, off;

    if (r >= rf->n) {
        *nr = rf->m + (r - rf->n);
        *nc = c;
        return;
    }
    o = &rf->old[r];
    if (o->startoff < 0) {
        *nr = o->gfirst;
        *nc = c;
        return;
    }
    off = o->startoff + c;
    end = o->gfirst + o->gcount;
    for (k = o->gfirst; k + 1 < end && rf->items[k + 1].off <= off; k++)
        ;
    *nr = k;
    *nc = off - rf->items[k].off;
    if (*nc >= rf->newcols)
        *nc = rf->newcols - 1;
}

/* The same counted from the top of the scrollback: rows above lo stay. */
static void rf_map(Reflow *rf, int r, int c, int *nr, int *nc)
{
    if (r < rf->lo) {
        *nr = r;
        *nc = c;
        return;
    }
    rf_map_local(rf, r - rf->lo, c, nr, nc);
    *nr += rf->lo;
}

static void rf_gfx_map(void *ctx, long *abs_line, int *col)
{
    Reflow *rf = (Reflow *)ctx;
    long r = *abs_line - rf->base;
    int nr, nc;

    if (r < rf->lo)
        return;
    rf_map(rf, (int)r, *col, &nr, &nc);
    *abs_line = rf->base + nr;
    *col = nc;
}

/* A blank that shows nothing: none of these on a line's end is text. Only
 * what makes a blank visible counts (a background, underline, reverse,
 * strike-through); the blanks a scroll brings in carry the text colour
 * (BCE), and a colour on nothing is not text. */
#define RF_SHOWN (ATTR_BGMASK | ATTR_UNDER | ATTR_REVERSE | ATTR_STRIKE)
static bool rf_blank(Terminal *term, const termchar *c)
{
    return (c->chr == term->basic_erase_char.chr || c->chr == ' ') &&
        !((c->attr ^ term->basic_erase_char.attr) & RF_SHOWN) &&
        !c->cc_next && !c->truecolour.bg.enabled && !c->link;
}

static int rf_len(Terminal *term, termline *line)
{
    int n = line->cols;
    while (n > 0 && rf_blank(term, &line->chars[n - 1]))
        n--;
    return n;
}

static termline *rf_view(rf_item *it)
{
    return it->ent ? kitty_sb_view(it->ent) : it->line;
}

static void rf_release(rf_item *it, termline *line)
{
    if (it->ent)
        term_release_line(line);
}

static void rf_free(Terminal *term, rf_item *it)
{
    if (it->ent)
        kitty_sb_drop(term, it->ent);
    else
        kitty_tl_free(it->line);
}

/* Rows r..e (from lo) are one line: joined and cut at the new width. curc:
 * the cursor's column when it is on row curr (else curr is -1); the line
 * reaches at least one cell past it, so the cursor has a row. */
static void rf_line(Reflow *rf, int r, int e, int curr, int curc)
{
    Terminal *term = rf->term;
    int newcols = rf->newcols;
    int i, x, k0, len, pos, L = 0, cap = rf->oldcols + 2;
    termline *big, *src;

    for (i = r; i <= e; i++)
        cap += rf->cols[i];
    big = kitty_tl_new(term, cap);
    for (i = r; i <= e; i++) {
        src = rf_view(&rf->olds[i]);
        if (rf->lattr[i] & LATTR_WRAPPED) {
            len = src->cols;
            if (i >= rf->scr && len > rf->oldcols)
                len = rf->oldcols;
            if ((rf->lattr[i] & LATTR_WRAPPED2) && len > 0)
                len--;
        } else
            len = rf_len(term, src);
        for (x = 0; x < len; x++)
            kitty_tl_copychar(big, L + x, &src->chars[x]);
        rf->old[i].startoff = L;
        L += len;
        rf_release(&rf->olds[i], src);
        rf_free(term, &rf->olds[i]);
    }
    if (curr >= 0 && rf->old[curr].startoff + curc + 1 > L)
        L = rf->old[curr].startoff + curc + 1;

    k0 = rf->m;
    pos = 0;
    do {
        int cnt = L - pos;
        bool w2 = false;
        termline *nl;
        if (cnt > newcols) {
            cnt = newcols;
            if (cnt > 1 && big->chars[pos + cnt].chr == UCSWIDE) {
                cnt--;                 /* the wide character starts the next row */
                w2 = true;
            }
        }
        nl = kitty_tl_new(term, newcols);
        for (x = 0; x < cnt; x++)
            kitty_tl_copychar(nl, x, &big->chars[pos + x]);
        if (pos + cnt < L)
            nl->lattr |= LATTR_WRAPPED | (w2 ? LATTR_WRAPPED2 : 0);
        rf_push(rf, NULL, nl, pos);
        pos += cnt;
    } while (pos < L);
    kitty_tl_free(big);
    if (rf->lattr[e] & LATTR_WRAPPED)  /* the last row of all, still open */
        rf->items[rf->m - 1].line->lattr |= LATTR_WRAPPED;

    for (i = r; i <= e; i++) {
        rf->old[i].gfirst = k0;
        rf->old[i].gcount = rf->m - k0;
    }
    for (i = r; i <= e; i++) {
        int nr, nc;
        rf_map_local(rf, i, 0, &nr, &nc);
        rf->items[nr].line->lattr |= rf->lattr[i] & LATTR_MARKS;
    }
}

/* The taken rows, line by line. curr/curc: the cursor (from lo; -1: not
 * among them); plo: the prompt being typed at (from lo; -1: none). */
static void rf_run(Reflow *rf, int curr, int curc, int plo)
{
    Terminal *term = rf->term;
    int r, e, k, n = rf->n;

    for (r = 0; r < n; r = e + 1) {
        bool keep = false;
        for (e = r;; e++) {
            if ((rf->lattr[e] & LATTR_MODE) != LATTR_NORM || rf->trusted[e])
                keep = true;           /* double size, or trusted: as it is */
            if (!(rf->lattr[e] & LATTR_WRAPPED) || e == n - 1)
                break;
        }
        if (plo >= 0 && e >= plo && r <= curr)
            keep = true;               /* the prompt being typed at */
        if (keep) {
            for (k = r; k <= e; k++) {
                rf->old[k].gfirst = rf_push(rf, rf->olds[k].ent,
                                            rf->olds[k].line, 0);
                rf->old[k].gcount = 1;
                rf->old[k].startoff = -1;
            }
            continue;
        }
        if (r == e && !(rf->lattr[r] & LATTR_WRAPPED)) {
            /* a row of its own: kept when it fits */
            int need = (curr == r) ? curc + 1 : 0, len = rf->cols[r];
            if (len > rf->newcols) {
                termline *v = rf_view(&rf->olds[r]);
                len = rf_len(term, v);
                rf_release(&rf->olds[r], v);
            }
            if (len <= rf->newcols && need <= rf->newcols) {
                rf->old[r].gfirst = rf_push(rf, rf->olds[r].ent,
                                            rf->olds[r].line, 0);
                rf->old[r].gcount = 1;
                rf->old[r].startoff = 0;
                continue;
            }
        }
        rf_line(rf, r, e, (curr >= r && curr <= e) ? curr : -1, curc);
    }
}

static void rf_init(Reflow *rf, Terminal *term, int newcols, int lo, int n)
{
    memset(rf, 0, sizeof(*rf));
    rf->term = term;
    rf->newcols = newcols;
    rf->oldcols = term->cols;
    rf->lo = lo;
    rf->n = n;
    rf->scr = n;
    rf->base = term->gfx_sb_base;
    rf->olds = snewn(n, rf_item);
    rf->cols = snewn(n, int);
    rf->lattr = snewn(n, unsigned);
    rf->trusted = snewn(n, bool);
    rf->old = snewn(n, rf_old);
}

static void rf_done(Reflow *rf)
{
    sfree(rf->items);
    sfree(rf->old);
    sfree(rf->olds);
    sfree(rf->cols);
    sfree(rf->lattr);
    sfree(rf->trusted);
}

/* Takes scrollback entries lo..hi-1 out of the tree into rf (from 0). */
static void rf_take_sb(Reflow *rf, int lo, int hi)
{
    int r;
    for (r = 0; r < hi - lo; r++) {
        rf->olds[r].ent = delpos234(rf->term->scrollback, lo);
        rf->olds[r].line = NULL;
        kitty_sb_peek(rf->olds[r].ent, &rf->cols[r], &rf->lattr[r],
                      &rf->trusted[r]);
    }
}

/* Where a line starts at or above scrollback entry r. */
static int rf_line_start(Terminal *term, int r)
{
    while (r > 0) {
        int c;
        unsigned la;
        bool tr;
        kitty_sb_peek(index234(term->scrollback, r - 1), &c, &la, &tr);
        if (!(la & LATTR_WRAPPED))
            break;
        r--;
    }
    return r;
}

/* Where the next line starts at or below scrollback entry r. */
static int rf_line_end(Terminal *term, int r)
{
    int sblen = count234(term->scrollback);
    if (r > sblen)
        r = sblen;
    while (r > 0 && r < sblen) {
        int c;
        unsigned la;
        bool tr;
        kitty_sb_peek(index234(term->scrollback, r - 1), &c, &la, &tr);
        if (!(la & LATTR_WRAPPED))
            break;
        r++;
    }
    return r;
}

static void rf_schedule(Terminal *term, int ms);

static void rf_timer(void *ctx, unsigned long now)
{
    Terminal *term = (Terminal *)ctx;
    if (now != term->rf_due)
        return;                        /* a later request took over */
    kitty_reflow_step(term);
}

static void rf_schedule(Terminal *term, int ms)
{
    term->rf_due = schedule_timer(ms * TICKSPERSEC / 1000, rf_timer, term);
}

/* Scrollback rows lo..hi-1 (both at a line's start) re-wrapped to newcols;
 * what points into them or below moves with them: *idx (an index counted
 * from the top of the scrollback, rows below hi keep their distance to
 * the end), the rows that come back when the window grows (counted from
 * the bottom), the pictures' anchors. Returns the change in the number of
 * scrollback rows. */
static int rf_sb_range(Terminal *term, int lo, int hi, int newcols, int *idx)
{
    Reflow rf[1];
    int sblen = count234(term->scrollback), k, nr, nc, delta;

    rf_init(rf, term, newcols, lo, hi - lo);
    rf_take_sb(rf, lo, hi);
    rf_run(rf, -1, 0, -1);
    delta = rf->m - rf->n;
    if (*idx < hi) {
        rf_map(rf, *idx, 0, &nr, &nc);
        *idx = nr;
    } else
        *idx += delta;
    if (sblen - term->tempsblines < hi) {
        rf_map(rf, sblen - term->tempsblines, 0, &nr, &nc);
        term->tempsblines = sblen + delta - nr;
        if (term->tempsblines < 0)
            term->tempsblines = 0;
    }
    kitty_gfx_reflowed(term, rf_gfx_map, rf);
    for (k = 0; k < rf->m; k++) {
        rf_item *it = &rf->items[k];
        addpos234(term->scrollback,
                  it->ent ? it->ent : kitty_sb_make(term, it->line), lo + k);
    }
    rf_done(rf);
    return delta;
}

void term_set_dragging(Terminal *term, bool on)
{
    term->rf_dragging = on;
    if (!on && term->rf_pend)
        rf_schedule(term, RF_STEP_MS);
}

void kitty_reflow(Terminal *term, int newcols, int *viewtop)
{
    Reflow rf[1];
    int sblen = count234(term->scrollback), rows = term->rows;
    int lo, r, k, top, nr, nc, nb, lastrow = -1, lastcol = 0, plo = -1, want;
    int around;
    int curr, curc = term->curs.x + (term->wrapnext ? 1 : 0);

    /* the newest rows of the scrollback now, from where a line starts -
     * at least enough to fill the screen again after a wider window joined
     * them (rows from further up would come down with their old cut) */
    around = 2 * rows * ((newcols + term->cols - 1) / term->cols) + rows;
    want = around;
    if (want < kitty_rf_sync_rows && !term->rf_dragging)
        want = kitty_rf_sync_rows;
    /* a view scrolled back above them: its rows now too, from a screen
     * above its top to `around` below it - what is looked at is re-wrapped
     * at once wherever it is (the timer passes these rows again later and
     * finds them cut at this width already) */
    if (viewtop && *viewtop + around < sblen - want) {
        int vlo = rf_line_start(term, *viewtop > rows ? *viewtop - rows : 0);
        int vhi = rf_line_end(term, *viewtop + around);
        sblen += rf_sb_range(term, vlo, vhi, newcols, viewtop);
    }
    lo = rf_line_start(term, sblen > want ? sblen - want : 0);
    rf_init(rf, term, newcols, lo, sblen - lo + rows);
    rf->scr = sblen - lo;
    curr = rf->scr + term->curs.y;
    rf_take_sb(rf, lo, sblen);
    for (r = 0; r < rows; r++) {
        termline *line = index234(term->screen, r);
        int len = rf_len(term, line), i = rf->scr + r;
        rf->olds[i].ent = NULL;
        rf->olds[i].line = line;
        rf->cols[i] = line->cols;
        rf->lattr[i] = line->lattr;
        rf->trusted[i] = line->trusted;
        if (len > 0 || (line->lattr & LATTR_MARKS)) {
            lastrow = i;
            lastcol = len > 0 ? len - 1 : 0;
        }
    }
    freetree234(term->screen);         /* the nodes; the rows are in olds */

    /* The shell at its prompt (OSC 133): the prompt and what is typed after
     * it stay as the shell drew them. On the new size the shell moves up
     * by the rows it drew them on, clears and draws them again (bash,
     * zsh); re-wrapped, they would sit on other rows and it would land a
     * row off - a doubled prompt, or the line above wiped. */
    if (term->at_prompt && term->marks_seen)
        for (r = curr; r >= 0 && r > curr - RF_PROMPT_ROWS; r--)
            if (rf->lattr[r] & LATTR_PROMPT) {
                plo = r;
                break;
            }

    rf_run(rf, curr, curc, plo);

    /* the screen: where the lines above the cursor got longer it keeps its
     * top row and the cursor moves down; where they got shorter the cursor
     * keeps its row and earlier rows come down - only those a taller
     * window would bring back (tempsblines). The last text stays on the
     * screen, the cursor too. Counted from lo. */
    rf_map(rf, sblen - term->tempsblines, 0, &nb, &nc);
    nb -= lo;
    rf_map_local(rf, rf->scr, 0, &top, &nc);
    rf_map_local(rf, curr, curc, &nr, &nc);
    term->curs.x = nc;
    if (nr - top <= term->curs.y) {
        top = nr - term->curs.y;
        if (top < nb)
            top = nb;
    } else if (nr - top + 1 > rows)
        top = nr - rows + 1;
    if (lastrow >= 0) {
        int lr, lc;
        rf_map_local(rf, lastrow, lastcol, &lr, &lc);
        if (lr - top + 1 > rows)
            top = lr - rows + 1;
    }
    if (top > nr)
        top = nr;
    if (top < -lo)
        top = -lo;
    term->curs.y = nr - top;
    if (term->savecurs.y >= 0 && term->savecurs.y < rows) {
        int sy, sx;
        rf_map_local(rf, rf->scr + term->savecurs.y, term->savecurs.x,
                     &sy, &sx);
        sy -= top;
        term->savecurs.y = sy < 0 ? 0 : sy >= rows ? rows - 1 : sy;
        term->savecurs.x = sx;
    }
    /* the rows that come back when the window grows: from where the first
     * of them went (the old screen's top when there were none); the rows
     * the narrower screen pushed up count too */
    term->tempsblines = top - nb;
    if (viewtop) {
        rf_map(rf, *viewtop, 0, &nr, &nc);
        *viewtop = nr;
    }
    kitty_gfx_reflowed(term, rf_gfx_map, rf);

    /* the new rows: above top into the scrollback after the rows not taken,
     * then the screen (top may be above lo when rows came down) */
    term->screen = newtree234(NULL);
    for (k = top; k < 0; k++) {        /* each from the same place */
        void *ent = delpos234(term->scrollback, lo + top);
        addpos234(term->screen, kitty_sb_take(term, ent), k - top);
    }
    for (k = 0; k < rf->m; k++) {
        rf_item *it = &rf->items[k];
        if (k < top)
            addpos234(term->scrollback,
                      it->ent ? it->ent : kitty_sb_make(term, it->line),
                      count234(term->scrollback));
        else if (k < top + rows)
            addpos234(term->screen,
                      it->ent ? kitty_sb_take(term, it->ent) : it->line,
                      count234(term->screen));
        else
            rf_free(term, it);         /* blank rows below the screen */
    }
    while (count234(term->screen) < rows)
        addpos234(term->screen, kitty_tl_new(term, newcols),
                  count234(term->screen));
    if (rf->m > top + rows)            /* pictures on the rows let go */
        kitty_gfx_row_deleted(term, rf->base + lo + top + rows);
    rf_done(rf);

    /* the older rows: afterwards, from a timer */
    term->rf_pend = lo > 0 ? term->gfx_sb_base + lo : 0;
    if (term->rf_pend)
        rf_schedule(term, RF_STEP_MS);
}

void kitty_reflow_step(Terminal *term)
{
    int sblen, hi, lo, view, delta;

    if (!term->rf_pend || term->rf_dragging)
        return;                        /* the end of the drag goes on */
    if (!term->rewrap) {
        term->rf_pend = 0;
        return;
    }
    if (term->alt_which) {             /* later: the main screen is away */
        rf_schedule(term, 200);
        return;
    }
    sblen = count234(term->scrollback);
    hi = (int)(term->rf_pend - term->gfx_sb_base);
    if (hi > sblen)
        hi = sblen;
    if (hi <= 0) {                     /* the rest has left the scrollback */
        term->rf_pend = 0;
        return;
    }
    lo = rf_line_start(term, hi > kitty_rf_slice_rows ?
                       hi - kitty_rf_slice_rows : 0);
    /* the view: its top line, where it is among or above these rows (below
     * them it keeps its distance to the bottom) */
    view = sblen + term->disptop;
    delta = rf_sb_range(term, lo, hi, term->cols, &view);
    term->disptop = view - (sblen + delta);
    kitty_term_rows_moved(term, hi - sblen);

    term->rf_pend = lo > 0 ? term->gfx_sb_base + lo : 0;
    if (term->rf_pend)
        rf_schedule(term, RF_STEP_MS);
}

#endif /* MOD_PERSO */
