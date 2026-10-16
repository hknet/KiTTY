/*
 * kitty_search.c - KiTTY++: search the scrollback (Ctrl+Shift+F), the
 * terminal's half. The front end (windows/kitty_searchbar.c) owns the find
 * bar and calls the term_search_* functions below; terminal.c calls the
 * kitty_search_* hooks where lines leave the scrollback, output arrives,
 * the size changes and each row is painted.
 *
 * Hits are stored by ABSOLUTE line number - the number of scrollback entry
 * 0 is `base`, which grows by one each time a line is dropped from the top -
 * so a hit stays on its text while output scrolls. A soft-wrapped line is
 * one logical line (a hit may span the wrap); the matcher is
 * kitty/kitty_search_match.h (smart case, unit-tested).
 *
 * The search runs 150 ms after the last change of the text: the rows in
 * view first (their hits are shown at once, kept apart in `vis` until the
 * full scan reaches them), then from the top of the scrollback down, a
 * slice of rows per timer tick so the terminal stays responsive; output arriving meanwhile is picked up by rescanning
 * from the logical line the screen started on at the last scan (scrollback
 * lines do not change, screen rows do).
 *
 * While the bar is open the view stays where it is: "Reset scrollback on
 * display activity" is set aside and restored on close. Closing keeps the
 * current hit highlighted until the next key or output, then everything
 * goes.
 */
#ifdef MOD_PERSO

#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

#include "putty.h"
#include "terminal.h"
#include "kitty_term_int.h"
#include "../kitty/kitty_search_match.h"

#define KS_DEBOUNCE_MS   150
#define KS_TAIL_MS       200
#define KS_SLICE_ROWS    3000          /* rows per timer tick */
#define KS_MAX_HITS      1000000       /* stored (16 MB); the count goes on */
#define KS_SPAN_MAX      64            /* rows one hit may span, at most */

typedef struct { int line, col, eline, ecol; } ks_hit;

struct kitty_search {
    Terminal *term;
    ksm_char *needle;
    int nn, cs;
    ks_hit *hits;
    int nhits;
    size_t hsize;
    ks_hit *vis;                   /* the view's hits, ahead of the full scan */
    int nvis;
    size_t vsize;
    bool vis_due;                  /* the view is scanned first */
    int total;                     /* counted, may exceed nhits */
    int cur;                       /* current hit, -1 = none */
    int base;                      /* absolute number of scrollback entry 0 */
    int scan_next;                 /* absolute line the scan goes on at; -1 = done */
    int tail;                      /* absolute line the tail rescan starts at */
    bool scanned_once;
    bool tail_dirty;
    bool open, lingering;          /* bar open / closed but the hit still shown */
    bool saved_sod;                /* scroll_on_disp while pinned */
    unsigned long due;             /* the one timer that does the work */
    int maxspan;                   /* rows the widest stored hit spans */
    ksm_line line;                 /* scratch */
};

static ksm_char ks_lower(ksm_char c)
{
    if (c < 0x10000)
        return (ksm_char)towlower((wint_t)c);
    return c;
}

/* A cell as a code point, the way copying reads it (clipme). */
static ksm_char ks_cell(Terminal *term, unsigned long uc)
{
    if (uc == UCSWIDE)
        return KSM_WIDE_CONT;
    switch (uc & CSET_MASK) {
      case CSET_LINEDRW:
        uc = term->ucsdata->unitab_xterm[uc & 0xFF];
        break;
      case CSET_ASCII:
        uc = term->ucsdata->unitab_line[uc & 0xFF];
        break;
      case CSET_SCOACS:
        uc = term->ucsdata->unitab_scoacs[uc & 0xFF];
        break;
    }
    switch (uc & CSET_MASK) {
      case CSET_ACP:
        uc = term->ucsdata->unitab_font[uc & 0xFF];
        break;
      case CSET_OEMCP:
        uc = term->ucsdata->unitab_oemcp[uc & 0xFF];
        break;
    }
    if (DIRECT_FONT(uc))
        uc &= 0xFF;
    return (ksm_char)(uc & ~CSET_MASK);
}

static int ks_sb(Terminal *term) { return count234(term->scrollback); }
/* absolute line <-> screen y (negative = scrollback) */
static int ks_y(struct kitty_search *s, int abs) { return abs - s->base - ks_sb(s->term); }
static int ks_abs(struct kitty_search *s, int y) { return y + ks_sb(s->term) + s->base; }

static unsigned ks_lattr(Terminal *term, int y)
{
    termline *l = kitty_term_lineptr(term, y);
    unsigned a = l ? l->lattr : 0;
    if (l) term_release_line(l);
    return a;
}

/* The first row of the logical line row y belongs to. */
static int ks_logical_start(Terminal *term, int y)
{
    int top = -ks_sb(term), k;
    for (k = 0; k < KS_SPAN_MAX && y > top; k++) {
        if (!(ks_lattr(term, y - 1) & LATTR_WRAPPED))
            break;
        y--;
    }
    return y;
}

static void ks_add_hit(struct kitty_search *s, int line, int col, int eline, int ecol)
{
    s->total++;
    if (s->nhits >= KS_MAX_HITS)
        return;
    sgrowarray(s->hits, s->hsize, s->nhits);
    s->hits[s->nhits].line = line;
    s->hits[s->nhits].col = col;
    s->hits[s->nhits].eline = eline;
    s->hits[s->nhits].ecol = ecol;
    s->nhits++;
    if (eline - line + 1 > s->maxspan)
        s->maxspan = eline - line + 1;
}

/* Scan the logical line starting at row y; returns the row after it. */
static int ks_scan_line(struct kitty_search *s, int y)
{
    Terminal *term = s->term;
    int rows = term->rows, k, n, cols;
    int pos[256];
    ksm_char cells[1024];

    s->line.n = 0;
    for (k = 0; k < KS_SPAN_MAX && y < rows; k++, y++) {
        termline *l = kitty_term_lineptr(term, y);
        unsigned lattr;
        int x;
        if (!l) { y++; break; }
        cols = l->cols < (int)lenof(cells) ? l->cols : (int)lenof(cells);
        for (x = 0; x < cols; x++)
            cells[x] = ks_cell(term, l->chars[x].chr);
        lattr = l->lattr;
        term_release_line(l);
        ksm_line_add_row(&s->line, cells, cols, ks_abs(s, y),
                         (lattr & LATTR_WRAPPED) != 0,
                         (lattr & LATTR_WRAPPED2) != 0);
        if (!(lattr & LATTR_WRAPPED)) { y++; break; }
    }
    if (s->nn > 0 && s->line.n >= s->nn) {
        n = ksm_find(&s->line, s->needle, s->nn, s->cs, ks_lower, pos, lenof(pos));
        if (n > (int)lenof(pos)) {
            /* more hits on one logical line than the buffer holds: count
             * them, keep the first ones */
            s->total += n - (int)lenof(pos);
            n = lenof(pos);
        }
        for (k = 0; k < n; k++) {
            int r, c, er, ec;
            ksm_span(&s->line, pos[k], s->nn, &r, &c, &er, &ec);
            ks_add_hit(s, r, c, er, ec);
        }
    }
    return y;
}

/* Drop the hits at or below absolute line `from` (they are scanned again). */
static void ks_truncate(struct kitty_search *s, int from)
{
    int keep = s->nhits;
    while (keep > 0 && s->hits[keep - 1].line >= from)
        keep--;
    s->total -= s->nhits - keep;
    s->nhits = keep;
    if (s->cur >= s->nhits)
        s->cur = -1;
}

static void ks_timer(void *ctx, unsigned long now);

/* The work happens in ms from now: the latest request wins, a timer set
 * earlier finds it is no longer due and does nothing. Never
 * expire_timer_context + schedule_timer here: a timer set again for the
 * same context and the same tick merges into the expired one and never
 * runs (timing.c), and the scan stood still for good - a resize restarts
 * the search three times within one tick. */
static void ks_schedule(struct kitty_search *s, int ms)
{
    s->due = schedule_timer(ms * TICKSPERSEC / 1000 + 1, ks_timer, s);
}

/* One step of the work: the debounce, a slice of the full scan, or a tail
 * rescan. */
static void ks_timer(void *ctx, unsigned long now)
{
    struct kitty_search *s = (struct kitty_search *)ctx;
    Terminal *term = s->term;
    if (now != s->due)
        return;                    /* superseded by a later request */
    if (!s->open || term->alt_which)
        return;
    if (s->scan_next >= 0 && s->vis_due) {
        int y = ks_logical_start(term, term->disptop);
        int end = term->disptop + term->rows;
        s->vis_due = false;
        s->nhits = s->total = 0;
        while (y < end)
            y = ks_scan_line(s, y);
        sgrowarray(s->vis, s->vsize, s->nhits);
        if (s->nhits)
            memcpy(s->vis, s->hits, s->nhits * sizeof(*s->hits));
        s->nvis = s->nhits;
        s->nhits = s->total = 0;
        ks_schedule(s, 0);
        term_invalidate(term);
        return;
    }
    if (s->scan_next >= 0) {
        int y = ks_y(s, s->scan_next), budget = KS_SLICE_ROWS;
        if (y < -ks_sb(term))
            y = -ks_sb(term);       /* lines left the top meanwhile */
        while (y < term->rows && budget > 0) {
            int ny = ks_scan_line(s, y);
            budget -= ny - y;
            y = ny;
        }
        if (y >= term->rows) {
            s->scan_next = -1;
            s->nvis = 0;               /* all in the full list now */
            s->scanned_once = true;
            s->tail = ks_abs(s, ks_logical_start(term, 0));
        } else {
            s->scan_next = ks_abs(s, y);
            ks_schedule(s, 10);
        }
        term_invalidate(term);
        return;
    }
    if (s->tail_dirty && s->scanned_once) {
        int y, i, cl = -1, cc = -1;
        s->tail_dirty = false;
        /* the current hit, when it is among the lines scanned again, stays
         * current if its text is still there */
        if (s->cur >= 0 && s->hits[s->cur].line >= s->tail) {
            cl = s->hits[s->cur].line;
            cc = s->hits[s->cur].col;
        }
        ks_truncate(s, s->tail);
        y = ks_y(s, s->tail);
        if (y < -ks_sb(term))
            y = -ks_sb(term);
        while (y < term->rows)
            y = ks_scan_line(s, y);
        for (i = s->nhits - 1; cl >= 0 && i >= 0 && s->hits[i].line >= cl; i--)
            if (s->hits[i].line == cl && s->hits[i].col == cc) {
                s->cur = i;
                break;
            }
        s->tail = ks_abs(s, ks_logical_start(term, 0));
        term_invalidate(term);
    }
}

/* Start over: the text changed, or the lines moved under the hits. */
static void ks_restart(struct kitty_search *s, int delay_ms)
{
    s->nhits = s->total = 0;
    s->nvis = 0;
    s->vis_due = s->nn > 0;
    s->cur = -1;
    s->maxspan = 1;
    s->scanned_once = false;
    s->tail_dirty = false;
    s->scan_next = s->nn > 0 ? s->base : -1;
    if (s->nn > 0)
        ks_schedule(s, delay_ms);
    term_invalidate(s->term);
}

/* ---- the API for the front end --------------------------------------- */

void term_search_open(Terminal *term)
{
    struct kitty_search *s = term->search;
    if (!s) {
        s = snew(struct kitty_search);
        memset(s, 0, sizeof(*s));
        s->term = term;
        s->cur = -1;
        s->scan_next = -1;
        s->maxspan = 1;
        term->search = s;
    }
    if (!s->open) {
        s->open = true;
        s->saved_sod = term->scroll_on_disp;
        term->scroll_on_disp = false;      /* the view stays put */
    }
    if (s->lingering) {                    /* reopened: search everything again */
        s->lingering = false;
        ks_restart(s, 0);
    }
}

/* The bar closes: the view is free again, the current hit stays lit until
 * the next key or output (kitty_search_forget). */
void term_search_close(Terminal *term)
{
    struct kitty_search *s = term->search;
    if (!s || !s->open)
        return;
    s->open = false;
    term->scroll_on_disp = s->saved_sod;
    if (s->cur >= 0) {
        s->hits[0] = s->hits[s->cur];
        s->nhits = s->total = 1;
        s->nvis = 0;
        s->cur = 0;
        s->lingering = true;
        term_invalidate(term);
    } else {
        kitty_search_free(term);
    }
}

void term_search_set(Terminal *term, const wchar_t *text)
{
    struct kitty_search *s = term->search;
    size_t i, n = 0;
    if (!s)
        return;
    sfree(s->needle);
    s->needle = snewn(wcslen(text) + 1, ksm_char);
    for (i = 0; text[i]; i++) {
        unsigned c = text[i];
        if (c >= 0xD800 && c < 0xDC00 && text[i + 1] >= 0xDC00 && text[i + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (text[i + 1] - 0xDC00);
            i++;
        }
        s->needle[n++] = c;
    }
    s->nn = (int)n;
    s->cs = ksm_case_sensitive(s->needle, s->nn, ks_lower);
    ks_restart(s, KS_DEBOUNCE_MS);
}

/* Up (dir < 0, older output) or Down (dir > 0). The first press starts
 * from the view: Up takes the last hit above the view's bottom, Down the
 * first below its top. The hit is brought into the view, in its middle
 * when it was outside. Returns 1 when a hit became current. */
int term_search_move(Terminal *term, int dir)
{
    struct kitty_search *s = term->search;
    int i, y, top, bottom;
    if (!s || !s->open || s->nhits == 0 || term->alt_which)
        return 0;
    top = ks_abs(s, term->disptop);
    bottom = ks_abs(s, term->disptop + term->rows - 1);
    if (s->cur < 0) {
        if (dir < 0) {
            for (i = s->nhits - 1; i >= 0 && s->hits[i].line > bottom; i--);
            if (i < 0) return 0;
        } else {
            for (i = 0; i < s->nhits && s->hits[i].line < top; i++);
            if (i >= s->nhits) return 0;
        }
    } else {
        i = s->cur + (dir < 0 ? -1 : 1);
        if (i < 0 || i >= s->nhits)
            return 0;              /* the end: no wrap-around */
    }
    s->cur = i;
    y = ks_y(s, s->hits[i].line);
    if (y < term->disptop || ks_y(s, s->hits[i].eline) > term->disptop + term->rows - 1)
        term_scroll(term, 0, (y - term->rows / 2) - term->disptop);
    term_invalidate(term);
    return 1;
}

/* For the bar's counter: the current hit (1-based, 0 = none), how many,
 * and the state - KS_STATE_*. */
int term_search_status(Terminal *term, int *cur, int *total)
{
    struct kitty_search *s = term->search;
    *cur = 0; *total = 0;
    if (!s || !s->open)
        return KS_STATE_IDLE;
    if (term->alt_which)
        return KS_STATE_NOT_AVAILABLE;
    if (s->nn == 0)
        return KS_STATE_IDLE;
    *cur = s->cur + 1;
    *total = s->total;
    return s->scan_next >= 0 ? KS_STATE_SEARCHING : KS_STATE_DONE;
}

/* The hits for the strip beside the scrollbar: line numbers counted from
 * the top of the scrollback, like term_failed_marks; *cur_out = the current
 * one's, or -1. */
int term_search_marks(Terminal *term, int *out, int max, int *cur_out)
{
    struct kitty_search *s = term->search;
    int i, n = 0, sb = ks_sb(term), last = -1;
    *cur_out = -1;
    if (!s || term->alt_which)
        return 0;
    for (i = 0; i < s->nhits && n < max; i++) {
        int rel = s->hits[i].line - s->base;
        if (rel < 0 || rel >= sb + term->rows || rel == last)
            continue;
        out[n++] = last = rel;
    }
    for (i = 0; i < s->nvis && n < max; i++) {
        int rel = s->vis[i].line - s->base;
        if (s->vis[i].line < s->scan_next || rel < 0 || rel >= sb + term->rows)
            continue;
        out[n++] = rel;
    }
    if (s->cur >= 0)
        *cur_out = s->hits[s->cur].line - s->base;
    return n;
}

bool term_search_active(Terminal *term)
{
    return term->search && term->search->open;
}

/* ---- the hooks for terminal.c ----------------------------------------- */

/* Everything goes; term_free calls it too, so nothing here may schedule
 * work on the terminal. */
void kitty_search_destroy(Terminal *term)
{
    struct kitty_search *s = term->search;
    if (!s)
        return;
    if (s->open)
        term->scroll_on_disp = s->saved_sod;
    expire_timer_context(s);
    sfree(s->needle);
    sfree(s->hits);
    sfree(s->vis);
    ksm_line_free(&s->line);
    sfree(s);
    term->search = NULL;
}

void kitty_search_free(Terminal *term)
{
    if (!term->search)
        return;
    kitty_search_destroy(term);
    term_invalidate(term);
}

/* term_reconfig read "Reset scrollback on display activity" again: while
 * the bar is open that is the value to restore, the view stays pinned. */
void kitty_search_reconfig(Terminal *term)
{
    struct kitty_search *s = term->search;
    if (s && s->open) {
        s->saved_sod = term->scroll_on_disp;
        term->scroll_on_disp = false;
    }
}

/* A key or new output after the bar closed: the lingering hit goes. */
void kitty_search_forget(Terminal *term)
{
    if (term->search && term->search->lingering)
        kitty_search_free(term);
}

/* Scrollback entry 0 was dropped. */
void kitty_search_sb_evicted(Terminal *term)
{
    struct kitty_search *s = term->search;
    int drop = 0;
    if (!s)
        return;
    s->base++;
    if (s->nvis) {
        int i, k = 0;
        for (i = 0; i < s->nvis; i++)
            if (s->vis[i].line >= s->base)
                s->vis[k++] = s->vis[i];
        s->nvis = k;
    }
    while (drop < s->nhits && s->hits[drop].line < s->base)
        drop++;
    if (drop) {
        memmove(s->hits, s->hits + drop, (s->nhits - drop) * sizeof(*s->hits));
        s->nhits -= drop;
        s->total -= drop;
        if (s->cur >= 0) {
            s->cur -= drop;
            if (s->cur < 0)
                s->cur = s->nhits ? 0 : -1;   /* it left: the next one */
        }
    }
}

/* Output changed the screen. */
void kitty_search_output(Terminal *term)
{
    struct kitty_search *s = term->search;
    if (!s)
        return;
    if (s->lingering) {
        kitty_search_free(term);
        return;
    }
    if (s->open && s->nn > 0 && s->scanned_once && !s->tail_dirty) {
        s->tail_dirty = true;
        ks_schedule(s, KS_TAIL_MS);
    }
}

/* The lines were re-laid (a resize, the scrollback cleared, back from the
 * alternate screen): every hit is searched again. */
void kitty_search_relaid(Terminal *term)
{
    struct kitty_search *s = term->search;
    if (!s)
        return;
    if (s->lingering) {
        kitty_search_free(term);
        return;
    }
    s->base = 0;
    ks_restart(s, 0);
}

/* The hit spans on screen row y (negative = scrollback): columns
 * [x0, x1) with cur = 1 for the current hit. Returns how many. */
int kitty_search_row(Terminal *term, int y, kitty_search_span *sp, int max)
{
    struct kitty_search *s = term->search;
    int abs, lo, hi, n = 0, i;
    if (!s || (s->nhits == 0 && s->nvis == 0) || term->alt_which)
        return 0;
    abs = ks_abs(s, y);
    /* the first hit that could still reach row abs */
    lo = 0; hi = s->nhits;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (s->hits[mid].line < abs - s->maxspan + 1) lo = mid + 1; else hi = mid;
    }
    for (i = lo; i < s->nhits && s->hits[i].line <= abs && n < max; i++) {
        ks_hit *h = &s->hits[i];
        if (h->eline < abs)
            continue;
        sp[n].x0 = h->line == abs ? h->col : 0;
        sp[n].x1 = h->eline == abs ? h->ecol : term->cols;
        sp[n].cur = (i == s->cur);
        n++;
    }
    /* the view's hits the full scan has not reached yet */
    for (i = 0; i < s->nvis && n < max; i++) {
        ks_hit *h = &s->vis[i];
        if (h->line < s->scan_next || h->line > abs || h->eline < abs)
            continue;
        sp[n].x0 = h->line == abs ? h->col : 0;
        sp[n].x1 = h->eline == abs ? h->ecol : term->cols;
        sp[n].cur = 0;
        n++;
    }
    return n;
}

#endif /* MOD_PERSO */
