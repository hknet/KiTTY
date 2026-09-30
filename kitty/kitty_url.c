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
 * Underline RENDERING is provided by kitty_url_cell_in_link() below, called
 * per-cell from windows/window.c do_text_internal(); regions are kept current
 * by a rescan in wintw_setup_draw_ctx().  Detection + hover-cursor +
 * click-to-open + underline are all functional.
 */
#include "putty.h"
#include <windows.h>
#include <ctype.h>
#include "terminal.h"
#include "urlhack.h"
#include "kitty_url.h"
#include "kitty_text.h"
#include "kitty_dlgbox.h"

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

/* OSC 8: the link handle of every visible cell as the last scan saw it (0 =
 * none, or a handle the terminal no longer resolves), so hover and click
 * know a declared link from a detected one. Same frame as the masks. */
static unsigned int *kitty_url_linkv = NULL;

/* A fingerprint of the text the last scan saw. A repaint whose text is the
 * same as last time - a focus change, another window uncovering ours, a
 * forced redraw - keeps the regions it already has instead of scanning
 * again; the regex and the per-cell diff are the expensive part of a paint
 * on a link-rich screen. Cleared when the regular expression changes. */
static unsigned long long kitty_url_last_hash = 0;
static int kitty_url_last_valid = 0;

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
    kitty_url_last_valid = 0;          /* a new expression must rescan */
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
 *
 * Two sources, each switched per session: the regular expression over the
 * text (CONF_url_scan) and the links the host declared with OSC 8
 * (CONF_url_osc8). A cell an OSC 8 link owns is a link whatever the
 * expression thinks, and is a blank to the expression, so a detected URL
 * never runs into or across a declared one.
 */
int kitty_url_rescan(Terminal *term, Conf *conf)
{
    int i, j, any = 0;
    int scan = conf_get_int(conf, CONF_url_scan) != 0;
    int osc8 = conf_get_int(conf, CONF_url_osc8) != 0;
    unsigned long long hash = 1469598103934665603ULL;   /* FNV-1a */
    if (!kitty_url_inited || term == NULL)
        return 0;
    if (kitty_url_mask_rows != term->rows || kitty_url_mask_cols != term->cols) {
        sfree(kitty_url_mask);
        sfree(kitty_url_prevmask);
        sfree(kitty_url_dirtyrow);
        sfree(kitty_url_linkv);
        kitty_url_mask_rows = term->rows;
        kitty_url_mask_cols = term->cols;
        kitty_url_mask     = snewn(term->rows * term->cols, unsigned char);
        kitty_url_prevmask = snewn(term->rows * term->cols, unsigned char);
        kitty_url_dirtyrow = snewn(term->rows, unsigned char);
        kitty_url_linkv    = snewn(term->rows * term->cols, unsigned int);
        memset(kitty_url_prevmask, 0, term->rows * term->cols);
        kitty_url_last_valid = 0;
    }
    urlhack_reset();
    hash = (hash ^ (unsigned long long)term->rows) * 1099511628211ULL;
    hash = (hash ^ (unsigned long long)term->cols) * 1099511628211ULL;
    hash = (hash ^ (unsigned long long)(scan | osc8 << 1)) * 1099511628211ULL;
    for (i = 0; i < term->rows; i++) {
        termline *lp = term_get_line(term, term->disptop + i);
        unsigned int *lv = kitty_url_linkv + i * term->cols;
        if (!lp) {
            memset(lv, 0, term->cols * sizeof(*lv));
            continue;
        }
        for (j = 0; j < term->cols; j++) {
            unsigned long c = lp->chars[j].chr & 0xFF;
            unsigned int link = osc8 ? lp->chars[j].link : 0;
            if (link && !term_link_uri(term, link))
                link = 0;              /* its slot has been reused */
            lv[j] = link;
            /* UCSWIDE / control chars -> treat as blank for URL scanning */
            if (c < 0x20 || c == 0x7F)
                c = ' ';
            hash = (hash ^ c) * 1099511628211ULL;
            hash = (hash ^ link) * 1099511628211ULL;
            urlhack_putchar(link ? ' ' : (char)c);
        }
        term_release_line(lp);
    }
    urlhack_putchar('\0');             /* the scan reads up to this */

    if (kitty_url_last_valid && hash == kitty_url_last_hash) {
        /* Same text as the last scan: the regions and the mask still hold. */
        if (kitty_url_dirtyrow)
            memset(kitty_url_dirtyrow, 0, term->rows);
        return 0;
    }
    kitty_url_last_hash = hash;
    kitty_url_last_valid = 1;
    /* Scan off: urlhack keeps the regions of its last scan, so every reader
     * of them below and in the click checks `scan` first. */
    if (scan)
        urlhack_go_find_me_some_hyperlinks(term->cols);

    /*
     * Diff per-cell link membership against the previous scan so the caller can
     * repaint only the rows whose underline state actually changed, rather than
     * invalidating the whole window on every content change.  urlhack_is_in_
     * link_region() uses the same 0-based visible frame as kitty_url_cell_
     * underline(), so the mask lines up with what gets drawn.
     */
    for (i = 0; i < term->rows; i++) {
        int rowchanged = 0;
        for (j = 0; j < term->cols; j++) {
            unsigned char m = (kitty_url_linkv[i * term->cols + j] ||
                               (scan && urlhack_is_in_link_region(j, i))) ? 1 : 0;
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
    /* the mask: detected and declared links alike */
    over = hover_cursor && kitty_url_cell_in_link(cx, cy);
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
 * OSC 8: what a declared link may open, and when it needs a confirmation.
 *
 * The host names the target, and its text need not be that target, so the
 * scheme is the boundary the regular expression used to be by accident: the
 * expression only ever matched http, https, ftp and www. A target opens at
 * once only when its scheme is http, https, ftp or mailto AND the link's own
 * text is the target itself; anything else shows the target for a confirmation, with
 * a red line when the scheme is not a browser's (ssh: reaches our own URL
 * handler). Refused outright: no valid scheme (a drive letter is none), a
 * double quote (it would end the argument a configured browser gets), a
 * file: target on another host.
 */
static int kitty_url_scheme(const char *uri, char *out, size_t outlen)
{
    size_t n = 0;
    if (!isalpha((unsigned char)uri[0]))
        return 0;
    while (uri[n] && uri[n] != ':') {
        char c = uri[n];
        if (!isalnum((unsigned char)c) && c != '+' && c != '-' && c != '.')
            return 0;
        if (n + 1 >= outlen)
            return 0;
        out[n] = (char)tolower((unsigned char)c);
        n++;
    }
    /* one letter is a drive (c:\...), never a scheme */
    if (uri[n] != ':' || n < 2)
        return 0;
    out[n] = '\0';
    return 1;
}

/* file://host/path: only an empty host, localhost or this computer's name */
static int kitty_url_file_is_local(const char *uri)
{
    const char *h = uri + 5, *end;         /* past "file:" */
    char me[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD melen = sizeof(me);
    size_t n;
    if (strncmp(h, "//", 2) != 0)
        return 1;                          /* file:/path, no authority */
    h += 2;
    end = strchr(h, '/');
    n = end ? (size_t)(end - h) : strlen(h);
    if (n == 0 || (n == 9 && !_strnicmp(h, "localhost", 9)))
        return 1;
    if (GetComputerNameA(me, &melen) && n == melen && !_strnicmp(h, me, n))
        return 1;
    return 0;
}

/* The link's own text: the run of cells around (x,y) with the same handle,
 * read on across line ends, ASCII only - enough to decide whether it is the
 * target. At most `max` characters. */
static char *kitty_url_link_text(Terminal *term, int x, int y,
                                 unsigned int link, size_t max)
{
    int cols = kitty_url_mask_cols, rows = kitty_url_mask_rows;
    int p = y * cols + x;
    size_t n = 0;
    char *s = snewn(max + 1, char);
    termline *lp = NULL;
    int lprow = -1;
    while (p > 0 && kitty_url_linkv[p - 1] == link)
        p--;
    for (; p < rows * cols && kitty_url_linkv[p] == link && n < max; p++) {
        unsigned long c;
        if (p / cols != lprow) {
            if (lp)
                term_release_line(lp);
            lprow = p / cols;
            lp = term_get_line(term, term->disptop + lprow);
            if (!lp)
                break;
        }
        c = lp->chars[p % cols].chr;
        if (c == UCSWIDE)
            continue;
        s[n++] = (char)(c & 0xFF);
    }
    if (lp)
        term_release_line(lp);
    s[n] = '\0';
    return s;
}

/* The open "Open link" box and what its Yes opens. */
typedef struct kitty_url_pending {
    char *uri, *browser;
} kitty_url_pending;
static HWND kitty_url_box = NULL;

static void kitty_url_answer(int yes, void *ctx)
{
    kitty_url_pending *p = (kitty_url_pending *)ctx;
    kitty_url_box = NULL;
    if (yes)
        urlhack_launch_url(p->browser, p->uri);
    sfree(p->uri);
    sfree(p->browser);
    sfree(p);
}

static void kitty_url_open_declared(Terminal *term, Conf *conf, HWND hwnd,
                                    LogContext *logctx, int x, int y,
                                    unsigned int link)
{
    static int logged_bad;                 /* once per window */
    const char *uri = term_link_uri(term, link);
    char scheme[33], *text, *warn = NULL;
    int web, trusted, same;
    const char *browser = NULL;

    if (!uri)
        return;
    if (!kitty_url_scheme(uri, scheme, sizeof(scheme)) || strchr(uri, '"') ||
        (!strcmp(scheme, "file") && !kitty_url_file_is_local(uri))) {
        if (!logged_bad && logctx) {
            logevent(logctx, KT_OSC8_LOG_REFUSED);
            logged_bad = 1;
        }
        return;
    }
    web = !strcmp(scheme, "http") || !strcmp(scheme, "https") ||
          !strcmp(scheme, "ftp");
    trusted = web || !strcmp(scheme, "mailto");
    text = kitty_url_link_text(term, x, y, link, strlen(uri) + 1);
    same = !strcmp(text, uri);
    sfree(text);

    /* a configured browser gets web targets only; mailto: and the rest go to
     * whatever Windows has registered for them */
    if (web && !conf_get_int(conf, CONF_url_defbrowser))
        browser = filename_to_str(conf_get_filename(conf, CONF_url_browser));

    if (trusted && same) {
        urlhack_launch_url(browser, uri);
        return;
    }

    /* The confirmation - modeless, so the terminal's output keeps arriving while the
     * box waits. One box at a time: a click on another link while one is
     * open brings that one forward instead. Yes opens what the box showed,
     * copied now, whatever has become of the link meanwhile. */
    if (kitty_url_box && IsWindow(kitty_url_box)) {
        SetForegroundWindow(kitty_url_box);
        return;
    }
    if (!trusted)
        warn = !strcmp(scheme, "ssh") ? dupstr(KT_OSC8_CONFIRM_WARN_SSH) :
               !strcmp(scheme, "file") ? dupstr(KT_OSC8_CONFIRM_WARN_FILE) :
               dupprintf(KT_OSC8_CONFIRM_WARN_SCHEME, scheme);
    {
        /* the whole target, in the box's read-only field: it wraps, and
         * scrolls when long, so every character can be inspected */
        kitty_url_pending *p = snew(kitty_url_pending);
        p->uri = dupstr(uri);
        p->browser = browser ? dupstr(browser) : NULL;
        /* the click held the mouse: let it go, or the box gets no clicks */
        ReleaseCapture();
        kitty_url_box = kitty_confirm_modeless(hwnd, KT_OSC8_CONFIRM_CAPTION,
                                               KT_OSC8_CONFIRM_TEXT, uri, warn,
                                               kitty_url_answer, p);
        if (!kitty_url_box) {          /* not made: that is a No */
            sfree(p->uri);
            sfree(p->browser);
            sfree(p);
        }
    }
    sfree(warn);
}

/*
 * Handle a (ctrl+)click at character coordinates x,y.  If it falls inside a
 * detected link region, extract the URL text and launch it.  Returns 1 if a
 * URL was launched.  Mirrors the term_mouse launch branch in 0.76b terminal.c.
 * A cell of an OSC 8 link goes through kitty_url_open_declared instead, and
 * the click is taken (1) whatever that decides.
 */
int kitty_url_click(Terminal *term, Conf *conf, HWND hwnd, LogContext *logctx,
                    int x, int y, int ctrl_down)
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
    if (!kitty_url_cell_in_link(x, y))
        return 0;
    if (kitty_url_linkv && x >= 0 && x < kitty_url_mask_cols &&
        y >= 0 && y < kitty_url_mask_rows &&
        kitty_url_linkv[y * kitty_url_mask_cols + x]) {
        kitty_url_open_declared(term, conf, hwnd, logctx, x, y,
                                kitty_url_linkv[y * kitty_url_mask_cols + x]);
        return 1;
    }
    if (!conf_get_int(conf, CONF_url_scan) || !urlhack_is_in_link_region(x, y))
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
 * Paint-time per-cell link test, called from window.c do_text_internal().
 * col/row are screen-relative character coordinates (row 0 = top visible
 * line), the same frame kitty_url_rescan() scans, so region lookups line up.
 * Returns 1 if the cell lies inside a detected link region.  Whether
 * underlining is enabled (CONF_url_underline) is the caller's check, made
 * once per text run instead of once per cell here.
 */
int kitty_url_cell_in_link(int col, int row)
{
    if (!kitty_url_inited)
        return 0;
    /* The per-cell mask of the last scan (after the swap in kitty_url_rescan
     * the current scan sits in prevmask). Asked for every cell of every text
     * run on every paint, so this is a lookup, not a walk over the regions. */
    if (kitty_url_prevmask && row >= 0 && row < kitty_url_mask_rows &&
        col >= 0 && col < kitty_url_mask_cols)
        return kitty_url_prevmask[row * kitty_url_mask_cols + col] ? 1 : 0;
    return urlhack_is_in_link_region(col, row) ? 1 : 0;
}
