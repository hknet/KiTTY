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
#include <commctrl.h>                  /* the OSC 8 preview tooltip */
#include <ctype.h>
#include "terminal.h"
#include "urlhack.h"
#include "kitty_url.h"
#include "kitty_text.h"
#include "kitty_dlgbox.h"
#include "kitty_theme.h"               /* kitty_theme_tooltip */
#include "kitty_win.h"                 /* kitty_theme_app_dark */

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
 * separate an OSC 8 hyperlink from a detected one. Same frame as the masks. */
static unsigned int *kitty_url_linkv = NULL;

/* A fingerprint of the text the last scan saw. A repaint whose text is the
 * same as last time - a focus change, another window uncovering ours, a
 * forced redraw - keeps the regions it already has instead of scanning
 * again; the regex and the per-cell diff are the expensive part of a paint
 * on a link-rich screen. Cleared when the regular expression changes. */
static unsigned long long kitty_url_last_hash = 0;
static int kitty_url_last_valid = 0;

/*
 * The expression runs per chunk of the screen, not over the whole screen,
 * and a chunk whose text the last scan saw takes its matches from it - so
 * output that scrolls scans only the lines that came in.
 *
 * A chunk is a run of rows the expression could match across: the screen
 * text has no separators between rows, so a link continues on the next row
 * whenever a row is full. A row whose last cell is a blank (OSC 8 cells
 * count as blanks) ends its chunk. Keyed by its text and length, never by
 * position: a chunk that only moved is found again. Matches are kept as
 * offsets from the chunk's start. Only the last scan's chunks are kept.
 */
typedef struct kitty_url_chunk {
    unsigned long long hash;
    int len;                           /* bytes of screen text */
    int first, n;                      /* its matches in the offset array */
} kitty_url_chunk;
typedef struct kitty_url_chunks {
    kitty_url_chunk *c;
    int nc, cc;
    int *off;                          /* start, end pairs */
    int noff, coff;
} kitty_url_chunks;
static kitty_url_chunks kitty_url_cache[2];
static int kitty_url_cache_cur = 0;    /* the one being filled */

/* After a scan, until the frame is drawn: the old mask is still in
 * kitty_url_mask. The rows whose pixels the window moved meanwhile
 * (kitty_url_note_shift) decide which old row each new one is compared
 * with (kitty_url_frame_done). */
static int kitty_url_diff_pending = 0;
static int kitty_url_shift_top, kitty_url_shift_bot, kitty_url_shift_n;

/* Per row of the last scan: the hash of its text, and whether its last
 * cell was a blank (the end of a chunk). */
static unsigned long long *kitty_url_rowhash = NULL;
static unsigned char *kitty_url_rowend = NULL;

static void kitty_url_cache_clear(void)
{
    kitty_url_cache[0].nc = kitty_url_cache[0].noff = 0;
    kitty_url_cache[1].nc = kitty_url_cache[1].noff = 0;
}

static void kitty_url_cache_match(void *ctx, int s, int e)
{
    kitty_url_chunks *k = (kitty_url_chunks *)ctx;
    if (k->noff + 2 > k->coff) {
        k->coff = k->coff ? 2 * k->coff : 64;
        k->off = sresize(k->off, k->coff, int);
    }
    k->off[k->noff++] = s;
    k->off[k->noff++] = e;
}

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
    kitty_url_cache_clear();           /* and its matches are not the old ones */
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
 * text (CONF_url_scan) and the OSC 8 hyperlinks the host sends
 * (CONF_url_osc8). A cell an OSC 8 link owns is a link whatever the
 * expression thinks, and is a blank to the expression, so a detected URL
 * never runs into or across an OSC 8 one.
 *
 * Runs before the frame is drawn, which draws with the new scan. Which rows
 * changed their underline is settled after it (kitty_url_frame_done), once
 * it is known whether the frame moved rows. True when the text changed.
 */
int kitty_url_rescan(Terminal *term, Conf *conf)
{
    int i, j;
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
        sfree(kitty_url_rowhash);
        sfree(kitty_url_rowend);
        kitty_url_mask_rows = term->rows;
        kitty_url_mask_cols = term->cols;
        kitty_url_mask     = snewn(term->rows * term->cols, unsigned char);
        kitty_url_prevmask = snewn(term->rows * term->cols, unsigned char);
        kitty_url_dirtyrow = snewn(term->rows, unsigned char);
        kitty_url_linkv    = snewn(term->rows * term->cols, unsigned int);
        kitty_url_rowhash  = snewn(term->rows, unsigned long long);
        kitty_url_rowend   = snewn(term->rows, unsigned char);
        memset(kitty_url_prevmask, 0, term->rows * term->cols);
        memset(kitty_url_dirtyrow, 0, term->rows);
        kitty_url_last_valid = 0;
    }
    kitty_url_diff_pending = 0;        /* a new frame: no diff, no shift yet */
    kitty_url_shift_n = 0;
    urlhack_reset();
    hash = (hash ^ (unsigned long long)term->rows) * 1099511628211ULL;
    hash = (hash ^ (unsigned long long)term->cols) * 1099511628211ULL;
    hash = (hash ^ (unsigned long long)(scan | osc8 << 1)) * 1099511628211ULL;
    for (i = 0; i < term->rows; i++) {
        termline *lp = term_get_line(term, term->disptop + i);
        unsigned int *lv = kitty_url_linkv + i * term->cols;
        unsigned long long rh = 1469598103934665603ULL;
        char fed = ' ';
        if (!lp) {
            /* a blank row, as the scan sees it */
            memset(lv, 0, term->cols * sizeof(*lv));
            for (j = 0; j < term->cols; j++) {
                urlhack_putchar(' ');
                rh = (rh ^ ' ') * 1099511628211ULL;
            }
            hash = (hash ^ rh) * 1099511628211ULL;
            kitty_url_rowhash[i] = rh;
            kitty_url_rowend[i] = 1;
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
            fed = link ? ' ' : (char)c;
            rh = (rh ^ (unsigned char)fed) * 1099511628211ULL;
            hash = (hash ^ c) * 1099511628211ULL;
            hash = (hash ^ link) * 1099511628211ULL;
            urlhack_putchar(fed);
        }
        term_release_line(lp);
        kitty_url_rowhash[i] = rh;
        kitty_url_rowend[i] = fed == ' ';
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

    /* The new mask: OSC 8 cells, then the regions of the expression. */
    for (i = 0; i < term->rows * term->cols; i++)
        kitty_url_mask[i] = kitty_url_linkv[i] ? 1 : 0;
    /* Scan off: urlhack keeps the regions of its last scan, so every reader
     * of them in the click checks `scan` first. */
    if (scan) {
        kitty_url_chunks *cur = &kitty_url_cache[kitty_url_cache_cur];
        kitty_url_chunks *old = &kitty_url_cache[!kitty_url_cache_cur];
        int r0 = 0, r1, cols = term->cols, total = term->rows * term->cols;
        cur->nc = cur->noff = 0;
        urlhack_clear_regions();
        while (r0 < term->rows) {
            unsigned long long ch = 1469598103934665603ULL;
            int start = r0 * cols, len, k, found = -1, ok = 1;
            for (r1 = r0; r1 < term->rows - 1 && !kitty_url_rowend[r1]; r1++)
                ;
            len = (r1 - r0 + 1) * cols;
            for (k = r0; k <= r1; k++)
                ch = (ch ^ kitty_url_rowhash[k]) * 1099511628211ULL;
            ch = (ch ^ (unsigned long long)len) * 1099511628211ULL;
            for (k = 0; k < old->nc; k++)
                if (old->c[k].hash == ch && old->c[k].len == len) {
                    found = k;
                    break;
                }
            if (cur->nc == cur->cc) {
                cur->cc = cur->cc ? 2 * cur->cc : 32;
                cur->c = sresize(cur->c, cur->cc, kitty_url_chunk);
            }
            cur->c[cur->nc].hash = ch;
            cur->c[cur->nc].len = len;
            cur->c[cur->nc].first = cur->noff;
            if (found >= 0) {
                /* seen before: its matches, moved to where it is now */
                for (k = 0; k < old->c[found].n; k++) {
                    int o = old->off[old->c[found].first + 2 * k];
                    int e = old->off[old->c[found].first + 2 * k + 1];
                    kitty_url_cache_match(cur, o, e);
                }
            } else {
                int m0 = cur->noff;
                ok = urlhack_scan_range(start, start + len,
                                        kitty_url_cache_match, cur);
                for (k = m0; k < cur->noff; k++)
                    cur->off[k] -= start;   /* offsets from the chunk start */
            }
            cur->c[cur->nc].n = (cur->noff - cur->c[cur->nc].first) / 2;
            cur->nc++;
            if (!ok)
                break;                 /* no expression: no links at all */
            /* the regions, and their cells in the mask */
            for (k = cur->c[cur->nc - 1].first; k < cur->noff; k += 2) {
                int s = start + cur->off[k], e = start + cur->off[k + 1], p;
                urlhack_add_link_region(s % cols, s / cols, e % cols, e / cols);
                for (p = s < 0 ? 0 : s; p < e && p < total; p++)
                    kitty_url_mask[p] = 1;
            }
            r0 = r1 + 1;
        }
        kitty_url_cache_cur = !kitty_url_cache_cur;
    }
    {   /* the new scan is what is drawn; the old one waits for the diff */
        unsigned char *t = kitty_url_prevmask;
        kitty_url_prevmask = kitty_url_mask;
        kitty_url_mask = t;
    }
    kitty_url_diff_pending = 1;
    return 1;
}

/* The window moved the pixels of rows top..bot by n (positive = up) in the
 * frame being drawn (window.c kitty_win_scroll_rows). */
void kitty_url_note_shift(int top, int bot, int n)
{
    kitty_url_shift_top = top;
    kitty_url_shift_bot = bot;
    kitty_url_shift_n = n;
}

/*
 * After the frame is drawn: which rows changed hyperlink-underline
 * membership against the last scan, for the window to repaint
 * (kitty_url_row_dirty). A row whose pixels the frame moved is compared with
 * the row they came from - a link that only scrolled keeps its underline
 * with its pixels. A row that came in was drawn whole. True when a row is
 * dirty.
 */
int kitty_url_frame_done(void)
{
    int i, j, any = 0, rows = kitty_url_mask_rows, cols = kitty_url_mask_cols;
    int top = kitty_url_shift_top, bot = kitty_url_shift_bot;
    int n = kitty_url_shift_n;
    kitty_url_shift_n = 0;
    if (!kitty_url_diff_pending || !kitty_url_mask || !kitty_url_prevmask)
        return 0;
    kitty_url_diff_pending = 0;
    if (n && (top < 0 || bot >= rows || top > bot))
        n = 0;
    for (i = 0; i < rows; i++) {
        int src = i;
        if (n && i >= top && i <= bot) {
            src = i + n;
            if (src < top || src > bot) {
                kitty_url_dirtyrow[i] = 0;     /* came in: drawn whole */
                continue;
            }
        }
        kitty_url_dirtyrow[i] = 0;
        for (j = 0; j < cols; j++)
            if (kitty_url_prevmask[i * cols + j] != kitty_url_mask[src * cols + j]) {
                kitty_url_dirtyrow[i] = 1;
                any = 1;
                break;
            }
    }
    return any;
}

/*
 * Did row `row` (0-based, top visible line) change hyperlink-underline
 * membership in the most recent frame (kitty_url_frame_done)?  Lets the
 * window layer repaint only the affected rows.
 */
int kitty_url_row_dirty(int row)
{
    if (!kitty_url_dirtyrow || row < 0 || row >= kitty_url_mask_rows)
        return 0;
    return kitty_url_dirtyrow[row];
}

/*
 * The host of an OSC 8 hyperlink's target, for the look-alike checks and for the
 * "Host:" line of the confirmation and of a long target's preview. Targets
 * are printable ASCII (terminal.c refuses anything else), so every trick left
 * is one of spelling:
 *   userinfo   https://mybank.example@evil.test/ - the host is evil.test;
 *              https://user:pass@host/ logs in with a password (kept here
 *              so the box can name the user and mask the password)
 *   numeric    http://3232235777/, 0x7f.1, 0177.0.0.1, 127.1 - an address in
 *              disguise. A plain dotted quad (192.168.1.1) is not one
 *   encoded    %2e and the like inside the host
 *   punycode   a label starting xn-- (a name imitating another)
 */
typedef struct kitty_url_hostinfo {
    char host[256];                    /* lower case; "" = no authority */
    char user[128];                    /* the user name before @, as written */
    int userinfo, numeric, encoded, punycode;
} kitty_url_hostinfo;

static int kitty_url_label_numeric(const char *l, size_t len)
{
    size_t i = 0;
    if (len > 2 && l[0] == '0' && (l[1] == 'x' || l[1] == 'X')) {
        for (i = 2; i < len; i++)
            if (!isxdigit((unsigned char)l[i]))
                return 0;
        return 1;
    }
    for (i = 0; i < len; i++)
        if (!isdigit((unsigned char)l[i]))
            return 0;
    return len > 0;
}

static void kitty_url_hostinfo_of(const char *uri, kitty_url_hostinfo *hi)
{
    const char *a = strchr(uri, ':'), *end, *h, *p;
    size_t n, i;
    int labels = 0, numlabels = 0, canonical = 1;

    memset(hi, 0, sizeof(*hi));
    if (!a || strncmp(a + 1, "//", 2) != 0)
        return;                        /* mailto: and the like: no host */
    a += 3;
    end = a + strcspn(a, "/?#");
    h = a;
    for (p = a; p < end; p++)
        if (*p == '@') {
            hi->userinfo = 1;
            h = p + 1;                 /* the host is after the LAST @ */
        }
    if (hi->userinfo) {                /* the user name: up to ':' or the @ */
        n = strcspn(a, ":@");
        if (n >= sizeof(hi->user))
            n = sizeof(hi->user) - 1;
        memcpy(hi->user, a, n);
        hi->user[n] = '\0';
    }
    if (*h == '[') {                   /* an IPv6 literal, kept as it is */
        p = memchr(h, ']', end - h);
        n = p ? (size_t)(p + 1 - h) : (size_t)(end - h);
    } else {
        for (p = h; p < end && *p != ':'; p++)
            ;
        n = p - h;                     /* without the port */
    }
    if (n >= sizeof(hi->host))
        n = sizeof(hi->host) - 1;
    for (i = 0; i < n; i++)
        hi->host[i] = (char)tolower((unsigned char)h[i]);
    hi->host[n] = '\0';
    hi->encoded = strchr(hi->host, '%') != NULL;
    if (!hi->host[0] || hi->host[0] == '[')
        return;
    for (p = hi->host; *p; ) {
        size_t len = strcspn(p, ".");
        if (len) {
            labels++;
            if (len >= 4 && !strncmp(p, "xn--", 4))
                hi->punycode = 1;
            if (kitty_url_label_numeric(p, len)) {
                numlabels++;
                /* a dotted quad's part: decimal, no leading zero, <= 255 */
                if ((len > 1 && p[0] == '0') || len > 3 ||
                    !isdigit((unsigned char)p[0]) || atoi(p) > 255)
                    canonical = 0;
            }
        }
        p += len + (p[len] == '.');
    }
    if (numlabels && numlabels == labels && !(labels == 4 && canonical))
        hi->numeric = 1;
}

/* The target as it is SHOWN (tip, box): a password before the host
 * (user:password@host) is masked as ****. What opens is the target as
 * written. */
static char *kitty_url_masked(const char *uri)
{
    const char *a = strchr(uri, ':'), *end, *at = NULL, *colon, *p;
    if (!a || strncmp(a + 1, "//", 2) != 0)
        return dupstr(uri);
    a += 3;
    end = a + strcspn(a, "/?#");
    for (p = a; p < end; p++)
        if (*p == '@')
            at = p;
    if (!at)
        return dupstr(uri);
    colon = memchr(a, ':', at - a);
    if (!colon)
        return dupstr(uri);            /* a user name, no password */
    return dupprintf("%.*s****%s", (int)(colon + 1 - uri), uri, at);
}

/* The target with its user name and password taken out, lower case: what
 * the bare-text check searches, so "mybank.com" in
 * https://mybank.com@evil.test/ does not count as found in the target. */
static char *kitty_url_without_userinfo(const char *uri)
{
    const char *a = strchr(uri, ':'), *end, *at = NULL, *p;
    char *s, *q;
    if (a && !strncmp(a + 1, "//", 2)) {
        a += 3;
        end = a + strcspn(a, "/?#");
        for (p = a; p < end; p++)
            if (*p == '@')
                at = p;
    }
    s = at ? dupprintf("%.*s%s", (int)(a - uri), uri, at + 1) : dupstr(uri);
    for (q = s; *q; q++)
        *q = (char)tolower((unsigned char)*q);
    return s;
}

/* The host a link's TEXT names, when the text is written as an address:
 * "https://..." or "www...." at its start. A bare name ("mybank.com") is
 * kitty_url_text_bare's. */
static int kitty_url_text_host(const char *text, char *out, size_t outlen)
{
    const char *s = text, *p, *e, *q;
    size_t n = 0;
    while (*s == ' ')
        s++;
    if ((p = strstr(s, "://")) != NULL && p - s < 16)
        s = p + 3;
    else if (_strnicmp(s, "www.", 4) != 0)
        return 0;
    e = s + strcspn(s, "/?# ");
    for (q = s; q < e; q++)
        if (*q == '@')
            s = q + 1;
    while (s[n] && (isalnum((unsigned char)s[n]) || s[n] == '-' ||
                    s[n] == '.') && n + 1 < outlen) {
        out[n] = (char)tolower((unsigned char)s[n]);
        n++;
    }
    out[n] = '\0';
    while (n && out[n - 1] == '.')
        out[--n] = '\0';
    return n > 0 && strchr(out, '.') != NULL;
}

/* The text as a single bare name.name word ("mybank.com", "report.pdf"),
 * lower case: a host or a file name, which its shape cannot tell apart.
 * kitty_url_open_osc8 settles it against the target instead. */
static int kitty_url_text_bare(const char *text, char *out, size_t outlen)
{
    const char *s = text, *last;
    size_t n, len;
    int dots = 0, letter = 0;
    while (*s == ' ')
        s++;
    len = strlen(s);
    while (len && (s[len - 1] == ' ' || s[len - 1] == '/'))
        len--;
    if (!len || len + 1 > outlen)
        return 0;
    for (n = 0; n < len; n++) {
        char c = s[n];
        if (c == '.') {
            if (!n || s[n - 1] == '.' || n + 1 == len)
                return 0;              /* no empty label */
            dots++;
        } else if (!isalnum((unsigned char)c) && c != '-')
            return 0;
        out[n] = (char)tolower((unsigned char)c);
    }
    out[n] = '\0';
    last = strrchr(out, '.');
    if (!dots || !last)
        return 0;
    for (last++; *last; last++)        /* 1.2.3 is a version, not a name */
        if (isalpha((unsigned char)*last))
            letter = 1;
    return letter;
}

/* One host is the other, or a name under it (docs.example.org is under
 * example.org): the same owner. */
static int kitty_url_same_owner(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    if (!strcmp(a, b))
        return 1;
    if (la > lb && a[la - lb - 1] == '.' && !strcmp(a + la - lb, b))
        return 1;
    if (lb > la && b[lb - la - 1] == '.' && !strcmp(b + lb - la, a))
        return 1;
    return 0;
}

/* A second address carried in the target's query - "?url=https://...",
 * "&next=https%3A%2F%2F...", "?to=//host/..." - whose host has another
 * owner than the target's: where a redirecting page really sends the user.
 * The value is percent-decoded once. Returns that host in out, or 0. */
static int kitty_url_redirect_host(const char *uri, const char *host,
                                   char *out, size_t outlen)
{
    const char *q = strchr(uri, '?'), *p;
    if (!q || !host[0])
        return 0;
    for (p = q + 1; *p && *p != '#'; ) {
        size_t len = strcspn(p, "&#");
        const char *eq = memchr(p, '=', len);
        if (eq) {
            size_t vlen = len - (size_t)(eq + 1 - p), i, n = 0;
            char *v = snewn(vlen + 1, char);
            for (i = 0; i < vlen; i++) {
                const char *c = eq + 1 + i;
                if (c[0] == '%' && i + 2 < vlen &&
                    isxdigit((unsigned char)c[1]) &&
                    isxdigit((unsigned char)c[2])) {
                    char hx[3];
                    hx[0] = c[1]; hx[1] = c[2]; hx[2] = '\0';
                    v[n++] = (char)strtol(hx, NULL, 16);
                    i += 2;
                } else
                    v[n++] = *c;
            }
            v[n] = '\0';
            if (!_strnicmp(v, "http://", 7) || !_strnicmp(v, "https://", 8) ||
                !strncmp(v, "//", 2)) {
                kitty_url_hostinfo ri;
                char *full = v[0] == '/' ? dupprintf("https:%s", v) : dupstr(v);
                kitty_url_hostinfo_of(full, &ri);
                sfree(full);
                if (ri.host[0] && !kitty_url_same_owner(ri.host, host) &&
                    strlen(ri.host) < outlen) {
                    strcpy(out, ri.host);
                    sfree(v);
                    return 1;
                }
            }
            sfree(v);
        }
        p += len;
        if (*p == '&')
            p++;
    }
    return 0;
}

/*
 * OSC 8 link preview: hovering an OSC 8 hyperlink shows its target in a tooltip
 * (CONF_url_preview), because its text need not be its target. A detected
 * link gets none - its text IS the target.
 *
 * One tracking tooltip per window, placed under the pointer when the pointer
 * reaches a link and kept there while it stays on that link (it does not
 * chase the pointer across the link's cells). It goes when the pointer
 * leaves the link or the window (WM_MOUSELEAVE), the window loses the focus,
 * the link is clicked, or the output replaces or scrolls the link away
 * (kitty_url_preview_refresh after each rescan). A target has no spaces to
 * wrap at, so it is broken every KITTY_URL_TIP_LINE characters. Past
 * KITTY_URL_TIP_MAX it ends in "..." - the whole target is in the
 * confirmation box's field when it matters.
 */
#define KITTY_URL_TIP_LINE 80
#define KITTY_URL_TIP_MAX 1024
/* A target the preview showed WHOLE on one line (KITTY_URL_TIP_LINE) for at
 * least this long before the click opens without the confirmation (the
 * other conditions are in kitty_url_open_osc8). */
#define KITTY_URL_TIP_READ_MS 400
static HWND kitty_url_tip = NULL, kitty_url_tip_owner = NULL;
static unsigned int kitty_url_tip_link = 0;    /* the link shown, 0 = none */
static DWORD kitty_url_tip_since = 0;          /* when it came up */
static char *kitty_url_tip_text = NULL;
static int kitty_url_tip_added = 0;

/* A target longer than one line starts with its host on a line of its own,
 * so a long name cannot hide where it really goes. A password in it is
 * shown masked (kitty_url_masked). */
static char *kitty_url_tip_format(const char *target)
{
    char *uri = kitty_url_masked(target);
    size_t len = strlen(uri), i, n = 0, hl = 0;
    kitty_url_hostinfo hi;
    char *s, *hostline = NULL;
    kitty_url_hostinfo_of(uri, &hi);
    if (len > KITTY_URL_TIP_LINE && hi.host[0]) {
        hostline = dupprintf(KT_OSC8_HOST_LINE, hi.host);
        hl = strlen(hostline);
    }
    s = snewn(hl + 1 + KITTY_URL_TIP_MAX + KITTY_URL_TIP_MAX / KITTY_URL_TIP_LINE + 8, char);
    if (hostline) {
        memcpy(s, hostline, hl);
        n = hl;
        s[n++] = '\n';
        sfree(hostline);
    }
    for (i = 0; i < len && i < KITTY_URL_TIP_MAX; i++) {
        if (i && i % KITTY_URL_TIP_LINE == 0)
            s[n++] = '\n';
        s[n++] = uri[i];
    }
    if (len > KITTY_URL_TIP_MAX) {
        memcpy(s + n, "...", 3);
        n += 3;
    }
    s[n] = '\0';
    sfree(uri);
    return s;
}

static void kitty_url_tip_info(TOOLINFOA *ti)
{
    memset(ti, 0, sizeof(*ti));
    ti->cbSize = TTTOOLINFOA_V1_SIZE;
    ti->uFlags = TTF_TRACK | TTF_ABSOLUTE;
    ti->hwnd = kitty_url_tip_owner;
    ti->uId = 0;
    ti->lpszText = kitty_url_tip_text;
}

void kitty_url_preview_hide(void)
{
    TOOLINFOA ti;
    if (kitty_url_tip && kitty_url_tip_added && kitty_url_tip_link) {
        kitty_url_tip_info(&ti);
        SendMessage(kitty_url_tip, TTM_TRACKACTIVATE, FALSE, (LPARAM)&ti);
    }
    kitty_url_tip_link = 0;
}

static void kitty_url_preview_show(HWND hwnd, unsigned int link,
                                   const char *uri)
{
    TOOLINFOA ti;
    POINT pt;
    RECT tr;
    MONITORINFO mi;
    TRACKMOUSEEVENT tme;
    int x, y;

    if (link == kitty_url_tip_link && hwnd == kitty_url_tip_owner)
        return;                        /* this one is up already */
    if (!kitty_url_tip || kitty_url_tip_owner != hwnd) {
        if (kitty_url_tip)
            DestroyWindow(kitty_url_tip);
        kitty_url_tip = CreateWindowEx(WS_EX_TOPMOST, TOOLTIPS_CLASS, NULL,
                                       WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                                       CW_USEDEFAULT, CW_USEDEFAULT,
                                       CW_USEDEFAULT, CW_USEDEFAULT,
                                       hwnd, NULL, GetModuleHandle(NULL), NULL);
        kitty_url_tip_owner = hwnd;
        kitty_url_tip_added = 0;
        if (!kitty_url_tip)
            return;
        /* any width: lines break only where the text has them */
        SendMessage(kitty_url_tip, TTM_SETMAXTIPWIDTH, 0, 32767);
    }
    sfree(kitty_url_tip_text);
    kitty_url_tip_text = kitty_url_tip_format(uri);
    kitty_url_tip_info(&ti);
    if (!kitty_url_tip_added) {
        if (!SendMessage(kitty_url_tip, TTM_ADDTOOL, 0, (LPARAM)&ti))
            return;
        kitty_url_tip_added = 1;
    } else {
        SendMessage(kitty_url_tip, TTM_UPDATETIPTEXT, 0, (LPARAM)&ti);
    }
    /* the application's theme, as the terminal's own frame takes it */
    kitty_theme_tooltip(kitty_url_tip, kitty_theme_app_dark());

    /* under the pointer, kept on the pointer's monitor: above it when there
     * is no room below, pulled left when there is none to the right */
    GetCursorPos(&pt);
    x = pt.x;
    y = pt.y + GetSystemMetrics(SM_CYCURSOR) * 2 / 3;
    SendMessage(kitty_url_tip, TTM_TRACKPOSITION, 0, MAKELPARAM(x, y));
    SendMessage(kitty_url_tip, TTM_TRACKACTIVATE, TRUE, (LPARAM)&ti);
    mi.cbSize = sizeof(mi);
    if (GetWindowRect(kitty_url_tip, &tr) &&
        GetMonitorInfo(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi)) {
        int w = tr.right - tr.left, h = tr.bottom - tr.top;
        if (y + h > mi.rcWork.bottom)
            y = pt.y - h - 4;
        if (x + w > mi.rcWork.right)
            x = mi.rcWork.right - w;
        if (x < mi.rcWork.left)
            x = mi.rcWork.left;
        if (y < mi.rcWork.top)
            y = mi.rcWork.top;
        SendMessage(kitty_url_tip, TTM_TRACKPOSITION, 0, MAKELPARAM(x, y));
    }
    kitty_url_tip_link = link;
    kitty_url_tip_since = GetTickCount();

    /* WM_MOUSELEAVE when the pointer leaves the window, to take it down */
    tme.cbSize = sizeof(tme);
    tme.dwFlags = TME_LEAVE;
    tme.hwndTrack = hwnd;
    tme.dwHoverTime = 0;
    TrackMouseEvent(&tme);
}

/* The preview for the cell (cx, cy): the OSC 8 hyperlink's target, or none. */
static void kitty_url_preview_at(Terminal *term, Conf *conf, HWND hwnd,
                                 int cx, int cy)
{
    unsigned int link = 0;
    const char *uri = NULL;
    if (conf_get_int(conf, CONF_url_preview) &&
        conf_get_int(conf, CONF_url_osc8) && kitty_url_linkv &&
        cx >= 0 && cx < kitty_url_mask_cols &&
        cy >= 0 && cy < kitty_url_mask_rows)
        link = kitty_url_linkv[cy * kitty_url_mask_cols + cx];
    if (link)
        uri = term_link_uri(term, link);
    if (!uri)
        kitty_url_preview_hide();
    else
        kitty_url_preview_show(hwnd, link, uri);
}

/* After a rescan: the output may have replaced or moved the link under a
 * preview that is up. Only then - a preview appears on a pointer move. */
void kitty_url_preview_refresh(Terminal *term, Conf *conf, HWND hwnd)
{
    if (kitty_url_tip_link)
        kitty_url_preview_at(term, conf, hwnd,
                             urlhack_mouse_old_x, urlhack_mouse_old_y);
}

/*
 * Update the mouse-hover state: show a hand cursor when over a link region,
 * and the target of an OSC 8 hyperlink (the preview above).
 * cx/cy are character coordinates.  Returns 1 if currently over a link.
 */
int kitty_url_hover(Terminal *term, Conf *conf, HWND hwnd, int cx, int cy,
                    int hover_cursor)
{
    int over;
    if (!kitty_url_inited)
        return 0;
    urlhack_mouse_old_x = cx;
    urlhack_mouse_old_y = cy;
    kitty_url_preview_at(term, conf, hwnd, cx, cy);
    /* the mask: detected and OSC 8 links alike */
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
 * What an OSC 8 hyperlink may open and when it needs a confirmation.
 *
 * The host names the target, and its text need not be that target, so the
 * scheme is the boundary the regular expression used to be by accident: the
 * expression only ever matched http, https, ftp and www. What opens at once,
 * what needs the confirmation and what is refused: kitty_url_open_osc8.
 * A drive letter is no scheme. A double quote would end the argument a
 * configured browser gets.
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

/* The confirmation - modeless, so the terminal's output keeps arriving while
 * the box waits. The host on a line of its own, then the whole target: the
 * field wraps and scrolls when long, so every character can be inspected.
 * One box at a time: a click on another link while one is open brings that
 * one forward instead. Yes opens what the box showed, copied now, whatever
 * has become of the link meanwhile - a password in it shown masked, opened
 * as written. warn: NULL or the red lines. */
static void kitty_url_ask(HWND hwnd, const char *uri, const char *browser,
                          const char *host, const char *warn)
{
    char *detail, *shown;
    kitty_url_pending *p;
    if (kitty_url_box && IsWindow(kitty_url_box)) {
        SetForegroundWindow(kitty_url_box);
        return;
    }
    shown = kitty_url_masked(uri);
    if (host && host[0]) {
        char *hostline = dupprintf(KT_OSC8_HOST_LINE, host);
        detail = dupprintf("%s\r\n%s", hostline, shown);
        sfree(hostline);
    } else
        detail = dupstr(shown);
    sfree(shown);
    p = snew(kitty_url_pending);
    p->uri = dupstr(uri);
    p->browser = browser ? dupstr(browser) : NULL;
    /* the click held the mouse: let it go, or the box gets no clicks */
    ReleaseCapture();
    kitty_url_box = kitty_confirm_modeless(hwnd, KT_OSC8_CONFIRM_CAPTION,
                                           KT_OSC8_CONFIRM_TEXT, detail,
                                           warn, kitty_url_answer, p);
    if (!kitty_url_box) {              /* not made: that is a No */
        sfree(p->uri);
        sfree(p->browser);
        sfree(p);
    }
    sfree(detail);
}

/* Add one red line to the confirmation's warning (lines joined by \n). */
static void kitty_url_warn_add(char **warn, char *line)
{
    if (!*warn) {
        *warn = line;
    } else {
        char *both = dupprintf("%s\n%s", *warn, line);
        sfree(*warn);
        sfree(line);
        *warn = both;
    }
}

/*
 * previewed: the link whose target the preview showed for at least
 * KITTY_URL_TIP_READ_MS before this click, else 0 (kitty_url_click).
 *
 * Refused (event log, no box): no valid scheme, a double quote, file: on
 * another computer. For http, https and ftp also: a user name before the host
 * or a host written as a number or encoded.
 * Opened at once: http, https, ftp or mailto with no red flag below, when either
 * the text IS the target, or the preview showed this target whole (at most
 * KITTY_URL_TIP_LINE characters, one line of the tip) before the click.
 * Everything else: the confirmation, with the host on its own line and a
 * red line per flag - not a browser's scheme, a text naming another host
 * than the target's, a punycode host.
 */
static void kitty_url_open_osc8(Terminal *term, Conf *conf, HWND hwnd,
                                    LogContext *logctx, int x, int y,
                                    unsigned int link, unsigned int previewed)
{
    static int logged_bad;                 /* once per window */
    const char *uri = term_link_uri(term, link);
    char scheme[33], *text, *warn = NULL, texthost[256];
    char redirhost[256];
    int level;
    const char *why = NULL;
    int web, trusted, same, mismatch = 0, redirect = 0;
    size_t tmax;
    const char *browser = NULL;
    kitty_url_hostinfo hi;

    if (!uri)
        return;
    kitty_url_hostinfo_of(uri, &hi);
    if (!kitty_url_scheme(uri, scheme, sizeof(scheme)) || strchr(uri, '"'))
        why = KT_OSC8_REFUSED_MALFORMED;
    else if (!strcmp(scheme, "file") && !kitty_url_file_is_local(uri))
        why = KT_OSC8_REFUSED_FILEHOST;
    web = !why && (!strcmp(scheme, "http") || !strcmp(scheme, "https") ||
                   !strcmp(scheme, "ftp"));
    if (!why && web && (hi.numeric || hi.encoded))
        why = KT_OSC8_REFUSED_NUMERIC;
    if (why) {
        if (!logged_bad && logctx) {
            char *m = dupprintf(KT_OSC8_LOG_REFUSED, why);
            logevent(logctx, m);
            sfree(m);
            logged_bad = 1;
        }
        return;
    }
    trusted = web || !strcmp(scheme, "mailto");
    tmax = strlen(uri) + 1;
    if (tmax < 300)
        tmax = 300;                    /* enough for the host in the text */
    text = kitty_url_link_text(term, x, y, link, tmax);
    same = !strcmp(text, uri);
    if (!same && hi.host[0] &&
        kitty_url_text_host(text, texthost, sizeof(texthost)) &&
        !kitty_url_same_owner(texthost, hi.host))
        mismatch = 1;
    /* A bare name.name text of a web link ("mybank.com"): a host or a file
     * name. Harmless when it is the target's host (or above or below it)
     * or appears in the target itself (report.pdf -> .../report.pdf) - NOT
     * counting a user name before the host, where https://mybank.com@evil/
     * would hide it. Otherwise the text claims something the target is
     * not. */
    if (!same && !mismatch && web && hi.host[0] &&
        kitty_url_text_bare(text, texthost, sizeof(texthost)) &&
        !kitty_url_same_owner(texthost, hi.host)) {
        char *lower = kitty_url_without_userinfo(uri);
        if (!strstr(lower, texthost))
            mismatch = 1;
        sfree(lower);
    }
    sfree(text);
    /* a second address in the query, with another owner: a redirect */
    if (web)
        redirect = kitty_url_redirect_host(uri, hi.host, redirhost,
                                           sizeof(redirhost));

    /* a configured browser gets web targets only; mailto: and the rest go to
     * whatever Windows has registered for them */
    if (web && !conf_get_int(conf, CONF_url_defbrowser))
        browser = filename_to_str(conf_get_filename(conf, CONF_url_browser));

    /* HyperlinkConfirm: 0 always puts up the box, 2 lets every web target through,
     * 1 (default) is the rule above */
    level = conf_get_int(conf, CONF_url_confirm);
    if ((level == 2 && trusted) ||
        (level == 1 && trusted && !mismatch && !redirect && !hi.punycode &&
         !hi.userinfo &&
         (same || (previewed == link && strlen(uri) <= KITTY_URL_TIP_LINE)))) {
        urlhack_launch_url(browser, uri);
        return;
    }

    if (!trusted)
        kitty_url_warn_add(&warn,
            !strcmp(scheme, "ssh") ? dupstr(KT_OSC8_CONFIRM_WARN_SSH) :
            !strcmp(scheme, "file") ? dupstr(KT_OSC8_CONFIRM_WARN_FILE) :
            dupprintf(KT_OSC8_CONFIRM_WARN_SCHEME, scheme));
    if (mismatch)
        kitty_url_warn_add(&warn, dupprintf(KT_OSC8_CONFIRM_WARN_TEXTHOST,
                                            texthost, hi.host));
    if (redirect)
        kitty_url_warn_add(&warn, dupprintf(KT_OSC8_CONFIRM_WARN_REDIRECT,
                                            redirhost));
    if (hi.punycode)
        kitty_url_warn_add(&warn, dupstr(KT_OSC8_CONFIRM_WARN_PUNYCODE));
    if (hi.userinfo)
        kitty_url_warn_add(&warn, dupprintf(KT_OSC8_CONFIRM_WARN_USERINFO,
                                            hi.host, hi.user));
    kitty_url_ask(hwnd, uri, browser, hi.host, warn);
    sfree(warn);
}

/*
 * Handle a (ctrl+)click at character coordinates x,y.  If it falls inside a
 * detected link region, extract the URL text and launch it.  Returns 1 if a
 * URL was launched.  Mirrors the term_mouse launch branch in 0.76b terminal.c.
 * A cell of an OSC 8 link goes through kitty_url_open_osc8 instead, and
 * the click is taken (1) whatever that decides.
 */
int kitty_url_click(Terminal *term, Conf *conf, HWND hwnd, LogContext *logctx,
                    int x, int y, int ctrl_down)
{
    text_region region;
    char *linkbuf = NULL;
    int i;
    int ctrl_required;

    unsigned int previewed;

    if (!kitty_url_inited || term == NULL)
        return 0;
    /* which target the preview had shown long enough to be read - taken
     * before the click takes the preview down */
    previewed = (kitty_url_tip_link &&
                 GetTickCount() - kitty_url_tip_since >= KITTY_URL_TIP_READ_MS)
                ? kitty_url_tip_link : 0;
    kitty_url_preview_hide();

    ctrl_required = conf_get_int(conf, CONF_url_ctrl_click);
    if (ctrl_required && !ctrl_down)
        return 0;
    if (!kitty_url_cell_in_link(x, y))
        return 0;
    if (kitty_url_linkv && x >= 0 && x < kitty_url_mask_cols &&
        y >= 0 && y < kitty_url_mask_rows &&
        kitty_url_linkv[y * kitty_url_mask_cols + x]) {
        kitty_url_open_osc8(term, conf, hwnd, logctx, x, y,
                                kitty_url_linkv[y * kitty_url_mask_cols + x],
                                previewed);
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
        char scheme[33];
        kitty_url_hostinfo hi;
        int web, level = conf_get_int(conf, CONF_url_confirm);
        /* The URL the expression found is its own text, so only the tricks
         * of spelling apply: a web address with a host written as a number
         * or encoded is not opened. One with a user name before the host
         * needs the confirmation, with the red line, except in "only for
         * non-web links". (KiTTY's default expression matches neither. A
         * custom one can.) */
        kitty_url_hostinfo_of(linkbuf, &hi);
        web = kitty_url_scheme(linkbuf, scheme, sizeof(scheme)) &&
              (!strcmp(scheme, "http") || !strcmp(scheme, "https") ||
               !strcmp(scheme, "ftp"));
        if (web && (hi.numeric || hi.encoded)) {
            static int logged_scan;        /* once per window */
            if (!logged_scan && logctx) {
                char *m = dupprintf(KT_URL_LOG_REFUSED, KT_OSC8_REFUSED_NUMERIC);
                logevent(logctx, m);
                sfree(m);
                logged_scan = 1;
            }
            sfree(linkbuf);
            return 1;
        }
        if (!conf_get_int(conf, CONF_url_defbrowser))
            browser = filename_to_str(conf_get_filename(conf, CONF_url_browser));
        if (level == 0 || (level == 1 && web && hi.userinfo)) {
            char *warn = (web && hi.userinfo) ?
                dupprintf(KT_OSC8_CONFIRM_WARN_USERINFO, hi.host, hi.user) : NULL;
            kitty_url_ask(hwnd, linkbuf, browser, hi.host, warn);
            sfree(warn);
        } else
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
