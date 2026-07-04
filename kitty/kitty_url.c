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
static unsigned long kitty_url_last_screen_hash = 0;

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
    int i, j;
    unsigned long hash = 2166136261UL;
    int changed;
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
            hash ^= (unsigned char)c;
            hash *= 16777619UL;
            urlhack_putchar((char)c);
        }
        term_release_line(lp);
        hash ^= '\n';
        hash *= 16777619UL;
    }
    hash ^= (unsigned long)term->cols;
    hash *= 16777619UL;
    hash ^= (unsigned long)term->rows;
    hash *= 16777619UL;
    changed = (hash != kitty_url_last_screen_hash);
    kitty_url_last_screen_hash = hash;
    urlhack_go_find_me_some_hyperlinks(term->cols);
    return changed;
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
