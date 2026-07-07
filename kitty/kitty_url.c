/*
 * KiTTY URL hyperlinks integration for PuTTY 0.84 (no-global, window.c-side).
 *
 * Original feature: PuttyTray / Nutty hyperlink hack, carried by KiTTY as an
 * old MOD-gated terminal patch. In KiTTY 0.76b the screen-scan + region
 * detection + launch lived inside terminal.c's do_paint / term_mouse.
 * PuTTY 0.84's terminal.c is
 * heavily refactored, but terminal.h now exposes the full Terminal struct plus
 * the public term_get_line()/term_release_line() accessors, so the whole thing
 * can be driven from windows/window.c instead, with NO terminal.c edits.
 *
 * This file provides:
 *   - kitty_url_init()                    one-time urlhack_init()
 *   - kitty_url_config(Conf*)             (re)compile the regex from conf
 *   - kitty_url_rescan(Terminal*)         scrape visible screen -> urlhack
 *   - kitty_url_hover(Terminal*,hwnd,x,y) hand cursor when over a link region
 *   - kitty_url_click(Terminal*,Conf*,x,y,ctrl)  ctrl+click -> launch browser
 *
 * Underline RENDERING is provided by kitty_url_cell_underline() below, called
 * per-cell from windows/window.c do_text_internal(); regions are kept current
 * by a rescan in wintw_setup_draw_ctx().  Detection + hover-cursor +
 * click-to-open + underline are all functional.
 */
#include "putty.h"
#include <windows.h>
#include "terminal.h"
#include "urlhack.h"

/* KiTTY url_underline modes (were in 0.76b putty.h) */
enum {
    URLHACK_UNDERLINE_ALWAYS = 0,
    URLHACK_UNDERLINE_HOVER,
    URLHACK_UNDERLINE_NEVER
};

static int kitty_url_inited = 0;
static int kitty_url_cursor_is_hand = 0;

/* Per-cell link membership from the last two scans, so a rescan can report
 * exactly which rows changed underline state; the window layer then repaints
 * only those rows instead of the whole window (which flickered on live output,
 * because the coarse "any screen content changed" signal fired every frame). */
static unsigned char *kitty_url_mask = NULL;
static unsigned char *kitty_url_prevmask = NULL;
static unsigned char *kitty_url_dirtyrow = NULL;
static int kitty_url_mask_rows = 0, kitty_url_mask_cols = 0;

void kitty_url_init(void)
{
    if (!kitty_url_inited) {
        urlhack_init();
        kitty_url_inited = 1;
    }
}

/* (Re)compile the active regular expression from the session conf. */
void kitty_url_config(Conf *conf)
{
    const char *re;
    if (!kitty_url_inited)
        return;
    re = conf_get_str(conf, CONF_url_regex);
    if (re == NULL || strlen(re) == 0)
        re = "@" "NO REGEX--"; /* harmless placeholder, matches nothing */
    if (conf_get_int(conf, CONF_url_defregex) != 0)
        urlhack_set_regular_expression(URLHACK_REGEX_CLASSIC, re);
    else
        urlhack_set_regular_expression(URLHACK_REGEX_CUSTOM, re);
}

/*
 * Scrape the visible terminal screen into urlhack and (re)scan for links.
 * Mirrors the term->url_update branch in 0.76b terminal.c do_paint, using the
 * 0.84 public term_get_line()/term_release_line() accessors.
 */
int kitty_url_rescan(Terminal *term)
{
    int i, j, any = 0;
    if (!kitty_url_inited || term == NULL)
        return 0;
    urlhack_reset();
    for (i = 0; i < term->rows; i++) {
        termline *lp = term_get_line(term, term->disptop + i);
        if (!lp)
            continue;
        for (j = 0; j < term->cols; j++) {
            unsigned long c = lp->chars[j].chr & 0xFF;
            /* UCSWIDE / control chars -> treat as blank for URL scanning */
            if (c < 0x20 || c == 0x7F)
                c = ' ';
            urlhack_putchar((char)c);
        }
        term_release_line(lp);
    }
    urlhack_go_find_me_some_hyperlinks(term->cols);

    /*
     * Diff per-cell link membership against the previous scan so the caller can
     * repaint only the rows whose underline state actually changed, rather than
     * invalidating the whole window on every content change.  urlhack_is_in_
     * link_region() uses the same 0-based visible frame as kitty_url_cell_
     * underline(), so the mask lines up with what gets drawn.
     */
    if (kitty_url_mask_rows != term->rows || kitty_url_mask_cols != term->cols) {
        sfree(kitty_url_mask);
        sfree(kitty_url_prevmask);
        sfree(kitty_url_dirtyrow);
        kitty_url_mask_rows = term->rows;
        kitty_url_mask_cols = term->cols;
        kitty_url_mask     = snewn(term->rows * term->cols, unsigned char);
        kitty_url_prevmask = snewn(term->rows * term->cols, unsigned char);
        kitty_url_dirtyrow = snewn(term->rows, unsigned char);
        memset(kitty_url_prevmask, 0, term->rows * term->cols);
    }
    for (i = 0; i < term->rows; i++) {
        int rowchanged = 0;
        for (j = 0; j < term->cols; j++) {
            unsigned char m = urlhack_is_in_link_region(j, i) ? 1 : 0;
            kitty_url_mask[i * term->cols + j] = m;
            if (m != kitty_url_prevmask[i * term->cols + j])
                rowchanged = 1;
        }
        kitty_url_dirtyrow[i] = (unsigned char)rowchanged;
        if (rowchanged)
            any = 1;
    }
    {   /* current scan becomes the baseline for the next diff */
        unsigned char *t = kitty_url_prevmask;
        kitty_url_prevmask = kitty_url_mask;
        kitty_url_mask = t;
    }
    return any;
}

/*
 * Did row `row` (0-based, top visible line) change hyperlink-underline
 * membership in the most recent kitty_url_rescan()?  Lets the window layer
 * repaint only the affected rows.
 */
int kitty_url_row_dirty(int row)
{
    if (!kitty_url_dirtyrow || row < 0 || row >= kitty_url_mask_rows)
        return 0;
    return kitty_url_dirtyrow[row];
}

/*
 * Update the mouse-hover state: show a hand cursor when over a link region.
 * cx/cy are character coordinates.  Returns 1 if currently over a link.
 */
int kitty_url_hover(Terminal *term, HWND hwnd, int cx, int cy, int hover_cursor)
{
    int over;
    if (!kitty_url_inited)
        return 0;
    urlhack_mouse_old_x = cx;
    urlhack_mouse_old_y = cy;
    over = hover_cursor && urlhack_is_in_link_region(cx, cy);
    if (over) {
        if (!kitty_url_cursor_is_hand) {
            SetClassLongPtr(hwnd, GCLP_HCURSOR,
                            (LONG_PTR)LoadCursor(NULL, IDC_HAND));
            kitty_url_cursor_is_hand = 1;
        }
    } else if (kitty_url_cursor_is_hand) {
        SetClassLongPtr(hwnd, GCLP_HCURSOR,
                        (LONG_PTR)LoadCursor(NULL, IDC_IBEAM));
        kitty_url_cursor_is_hand = 0;
    }
    return over;
}

/*
 * Handle a (ctrl+)click at character coordinates x,y.  If it falls inside a
 * detected link region, extract the URL text and launch it.  Returns 1 if a
 * URL was launched.  Mirrors the term_mouse launch branch in 0.76b terminal.c.
 */
int kitty_url_click(Terminal *term, Conf *conf, int x, int y, int ctrl_down)
{
    text_region region;
    char *linkbuf = NULL;
    int i;
    int ctrl_required;

    if (!kitty_url_inited || term == NULL)
        return 0;

    ctrl_required = conf_get_int(conf, CONF_url_ctrl_click);
    if (ctrl_required && !ctrl_down)
        return 0;
    if (!urlhack_is_in_link_region(x, y))
        return 0;

    region = urlhack_get_link_bounds(x, y);

    if (region.y0 == region.y1) {
        termline *lp = term_get_line(term, region.y0 + term->disptop);
        if (!lp)
            return 0;
        linkbuf = snewn(region.x1 - region.x0 + 2, char);
        for (i = region.x0; i < region.x1; i++)
            linkbuf[i - region.x0] = (char)(lp->chars[i].chr & 0xFF);
        linkbuf[i - region.x0] = '\0';
        term_release_line(lp);
    } else {
        int row = region.y0 + term->disptop;
        termline *lp = term_get_line(term, row);
        int linklen;
        if (!lp)
            return 0;
        linklen = (term->cols - region.x0) +
                  ((region.y1 - region.y0 - 1) * term->cols) + region.x1 + 1;
        linkbuf = snewn(linklen, char);
        for (i = region.x0; i < linklen + region.x0; i++) {
            linkbuf[i - region.x0] = (char)(lp->chars[i % term->cols].chr & 0xFF);
            if (((i + 1) % term->cols) == 0) {
                row++;
                term_release_line(lp);
                lp = term_get_line(term, row);
                if (!lp) { linkbuf[i - region.x0 + 1] = '\0'; break; }
            }
        }
        linkbuf[linklen - 1] = '\0';
        if (lp)
            term_release_line(lp);
    }

    if (linkbuf) {
        const char *browser = NULL;
        if (!conf_get_int(conf, CONF_url_defbrowser))
            browser = filename_to_str(conf_get_filename(conf, CONF_url_browser));
        urlhack_launch_url(browser, linkbuf);
        sfree(linkbuf);
        return 1;
    }
    return 0;
}

/*
 * Paint-time per-cell underline test, called from window.c do_text_internal().
 * Boolean semantics, matching the "Underline hyperlinks" checkbox
 * (CONF_url_underline is written as 0/1 by the config dialog): when enabled,
 * underline every cell that lies inside a detected link region.  col/row are
 * screen-relative character coordinates (row 0 = top visible line), the same
 * frame kitty_url_rescan() scans, so region lookups line up.  Returns 1 if the
 * cell should be underlined.
 */
int kitty_url_cell_underline(Conf *conf, int col, int row)
{
    if (!kitty_url_inited)
        return 0;
    if (!conf_get_int(conf, CONF_url_underline))
        return 0;
    return urlhack_is_in_link_region(col, row);
}
