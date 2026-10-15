/*
 * kitty_osc.c - KiTTY++: the OSC and DCS strings terminal.c's do_osc hands
 * over (kitty_osc_dispatch, kitty_osc_dcs), and the handlers that lived in
 * terminal/terminal.c: OSC 8 hyperlinks and their link table, OSC 1337
 * Capabilities and ReportCellSize, XTGETTCAP (DCS +q), the DCS form of
 * synchronized output, OSC 133's prompt marks with the command columns.
 * Moved as they were; the clipboard protocols are in kitty_clipboard.c.
 */

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <limits.h>
#include <wchar.h>

#include <time.h>
#include <assert.h>
#include "putty.h"
#include "terminal.h"
#include "kitty_perf.h"
#ifdef MOD_PERSO
#include "../kitty/kitty_text.h"
#include "../kitty/kitty_termenv.h"
#include "unicode/version.h"
#include "../kitty/kitty_transfer.h"
#include "../kitty/kitty_hostnotify.h"
#include "../kitty/kitty_progstatus.h"
#include "../kitty/kitty_gfx_term.h"
#include "kitty/kitty.h"
#include "kitty/kitty_osc52.h"
#include "kitty/kitty_reply.h"
#include "windows/kitty_gui.h"
#include "kitty_term_int.h"
#ifdef MOD_FAR2L
#include "../kitty/kitty_far2l_image_term.h"
#endif


/*
 * KiTTY: synchronized output in the DCS form iTerm2 proposed first, which
 * kitty's terminfo names (Sync=\EP=%p1%ds\E\\) and tmux therefore sends
 * under TERM=xterm-kitty: DCS = 1 s starts the hold of mode 2026, DCS = 2 s
 * ends it; mintty's DCS = 1 ; N s holds for at most N ms (within the
 * session's limit). Returns whether the DCS was one of these.
 */
static bool term_sync_dcs(Terminal *term)
{
    const char *s = term->osc_string;
    int len = term->osc_strlen, i;
    long n = -1;

    if (len < 3 || s[0] != '=' || s[len - 1] != 's')
        return false;
    if (len == 3 && s[1] == '2') {
        term_sync_release(term);
        return true;
    }
    if (s[1] != '1')
        return false;
    if (len > 3) {
        if (s[2] != ';' || len == 4 || len > 4 + 9)
            return false;
        for (n = 0, i = 3; i < len - 1; i++) {
            if (s[i] < '0' || s[i] > '9')
                return false;
            n = n * 10 + (s[i] - '0');
        }
    }
    term_sync_begin(term, n);
    return true;
}
#ifdef MOD_PERSO
/*
 * KiTTY OSC 8 hyperlinks. ESC ] 8 ; params ; URI ST opens a link, an empty
 * URI closes it, and every cell written in between carries the link's
 * handle (termchar.link).
 *
 * The table is a ring of KITTY_LINK_SLOTS slots. A handle is the slot number
 * plus the generation the slot was filled with, so a cell that outlives its
 * slot - the ring came round, or the byte budget pushed the slot out - reads
 * as no link, never as the link that took the slot over. A link already in
 * the table (same id and URI) gets its handle again instead of a new slot,
 * so a long listing of the same few targets does not churn the ring. Once
 * the generations run out (a million new links in one session) no further
 * link is made rather than risk an old cell resolving to a new target.
 */
#define KITTY_LINK_SLOT_BITS 12
#define KITTY_LINK_SLOTS (1u << KITTY_LINK_SLOT_BITS)
#define KITTY_LINK_GEN_MAX ((1u << (32 - KITTY_LINK_SLOT_BITS)) - 1)
#define KITTY_LINK_BYTES_MAX (1024 * 1024)
#define KITTY_LINK_ID_MAX 250

struct kitty_link {
    char *uri, *id;                    /* uri NULL: a free slot */
    unsigned int gen;
    unsigned long hash;
};

static unsigned long kitty_link_hash(const char *id, const char *uri)
{
    unsigned long h = 2166136261UL;    /* FNV-1a, id then a 0 then the URI */
    for (; *id; id++)
        h = ((h ^ (unsigned char)*id) * 16777619UL) & 0xFFFFFFFFUL;
    h = (h * 16777619UL) & 0xFFFFFFFFUL;
    for (; *uri; uri++)
        h = ((h ^ (unsigned char)*uri) * 16777619UL) & 0xFFFFFFFFUL;
    return h;
}

static void kitty_link_drop(Terminal *term, struct kitty_link *l)
{
    if (!l->uri)
        return;
    term->link_bytes -= strlen(l->uri) + strlen(l->id);
    sfree(l->uri);
    sfree(l->id);
    l->uri = l->id = NULL;
}

void kitty_links_free(Terminal *term)
{
    unsigned int i;
    if (!term->links)
        return;
    for (i = 0; i < KITTY_LINK_SLOTS; i++)
        kitty_link_drop(term, &term->links[i]);
    sfree(term->links);
    term->links = NULL;
}

static unsigned int kitty_link_intern(Terminal *term, const char *id,
                                      const char *uri)
{
    size_t need = strlen(uri) + strlen(id);
    unsigned long h = kitty_link_hash(id, uri);
    unsigned int i, slot;
    struct kitty_link *l;

    if (!term->links) {
        term->links = snewn(KITTY_LINK_SLOTS, struct kitty_link);
        memset(term->links, 0, KITTY_LINK_SLOTS * sizeof(struct kitty_link));
    }
    for (i = 0; i < KITTY_LINK_SLOTS; i++) {
        l = &term->links[i];
        if (l->uri && l->hash == h && !strcmp(l->uri, uri) &&
            !strcmp(l->id, id))
            return (l->gen << KITTY_LINK_SLOT_BITS) | i;
    }
    /* The next slot round the ring, and as many after it as the byte
     * budget needs. */
    slot = term->link_next;
    kitty_link_drop(term, &term->links[slot]);
    for (i = 1; term->link_bytes + need > KITTY_LINK_BYTES_MAX &&
             i < KITTY_LINK_SLOTS; i++)
        kitty_link_drop(term, &term->links[(slot + i) % KITTY_LINK_SLOTS]);
    term->link_next = (slot + 1) % KITTY_LINK_SLOTS;
    l = &term->links[slot];
    l->uri = dupstr(uri);
    l->id = dupstr(id);
    l->gen = ++term->link_gen;
    l->hash = h;
    term->link_bytes += need;
    return (l->gen << KITTY_LINK_SLOT_BITS) | slot;
}

const char *term_link_uri(Terminal *term, unsigned int link)
{
    struct kitty_link *l;
    if (!link || !term->links)
        return NULL;
    l = &term->links[link & (KITTY_LINK_SLOTS - 1)];
    if (!l->uri || l->gen != link >> KITTY_LINK_SLOT_BITS)
        return NULL;
    return l->uri;
}

/* do_osc, OSC 8. Every OSC 8 ends the link before it; a well-formed one with
 * a URI starts the next. Refused, so that the text after it is plain: the
 * session's switch off, a sequence cut at the OSC ceiling, a URI with bytes
 * outside printable ASCII (the specification's own rule), an id over
 * KITTY_LINK_ID_MAX. What a link may OPEN is decided at the click, not here
 * (windows side). */
static void kitty_osc8(Terminal *term)
{
    char *s = term->osc_string, *uri, *p;
    char id[KITTY_LINK_ID_MAX + 1];

    term->curr_link = 0;
    if (!term->conf || !conf_get_int(term->conf, CONF_url_osc8) ||
        term->osc_str_overflow)
        return;
    uri = strchr(s, ';');
    if (!uri)
        return;                        /* no URI field at all */
    *uri++ = '\0';
    if (!*uri)
        return;                        /* the close */
    for (p = uri; *p; p++)
        if ((unsigned char)*p < 0x21 || (unsigned char)*p > 0x7E)
            return;
    id[0] = '\0';
    for (p = s; *p; ) {                /* params: key=value:key=value */
        char *end = strchr(p, ':');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        if (n > 3 && !strncmp(p, "id=", 3)) {
            if (n - 3 > KITTY_LINK_ID_MAX)
                return;
            memcpy(id, p + 3, n - 3);
            id[n - 3] = '\0';
        }
        p += n + (end ? 1 : 0);
    }
    if (term->link_gen >= KITTY_LINK_GEN_MAX)
        return;                        /* generations spent: see above */
    term->curr_link = kitty_link_intern(term, id, uri);
}
#endif


/*
 * KiTTY: OSC 1337 ; Capabilities, iTerm2's "Terminal Feature Reporting"
 * (iterm2.com/feature-reporting): answered OSC 1337 ; Capabilities=<tags>
 * BEL, the tags in the order and with the terminator of iTerm2's own
 * encoder. Every tag from this session's state at the time of the query:
 *   T3  24-bit colour, both the 38;2;r;g;b form (1) and the colon forms
 *       38:2::r:g:b / 38:2:r:g:b (2) - true colour and ANSI colour on
 *   Cw  OSC 52 writes not set to Deny (Ask counts, as the spec allows)
 *   M   mouse reporting 1000/1002/1003/1006 - not with it disabled
 *   Sc7 DECSCUSR: Ps 1-4 (1), 5-6 (2) and 0 back to the default (4)
 *   U, Aw, Uw<n>  UTF-8 as the line character set; ambiguous-width
 *       characters wide; the Unicode version of the width tables
 *   Ts2 OSC 0/1/2 title setting (no title stack) - not with remote
 *       retitling disabled
 *   B   bracketed paste - not with it disabled
 *   F   focus reporting (DECSET 1004)
 *   Gs  strikethrough (SGR 9 / 29)
 *   Sy  synchronized output - a hold limit above 0
 *   H   OSC 8 hyperlinks - not with them off
 *   No  OSC 9 notifications - not with host notifications off
 *   Sx  Sixel - SixelImages
 *   F   iTerm2's OSC 1337 File= images - ItermImages. The spec gives FILE
 *       the same letter as FOCUS_REPORTING; iTerm2 and mintty both send it
 *       twice like this.
 *   P   OSC 9;4 progress - TaskbarProgress
 * Never: Lr (no left/right margins), Go (no overline).
 */
static void term_capabilities_report(Terminal *term)
{
    bool utf8 = term->ucsdata && term->ucsdata->line_codepage == CP_UTF8;
    strbuf *sb = strbuf_new();

    put_dataz(sb, "\033]1337;Capabilities=");
    if (term->ansi_colour && term->true_colour)
        put_dataz(sb, "T3");
    if (term->osc52_allowed != OSC52_CLIPBOARD_DENY)
        put_dataz(sb, "Cw");
    if (!term->xterm_mouse_forbidden)
        put_dataz(sb, "M");
    put_dataz(sb, "Sc7");
    if (utf8) {
        put_dataz(sb, "U");
        if (term->cjk_ambig_wide)
            put_dataz(sb, "Aw");
        put_fmt(sb, "Uw%d", atoi(UNICODE_VERSION_SHORT));
    }
    if (!term->no_remote_wintitle)
        put_dataz(sb, "Ts2");
    if (!term->no_bracketed_paste)
        put_dataz(sb, "B");
    if (!conf_get_bool(term->conf, CONF_no_focus_rep))
        put_dataz(sb, "F");
    put_dataz(sb, "Gs");
    if (term_sync_hold_ms(term))
        put_dataz(sb, "Sy");
    if (conf_get_int(term->conf, CONF_url_osc8))
        put_dataz(sb, "H");
    if (conf_get_int(term->conf, CONF_host_notify))
        put_dataz(sb, "No");
#ifdef KITTY_SIXEL
    if (conf_get_bool(term->conf, CONF_sixel_images))
        put_dataz(sb, "Sx");
#endif
    if (conf_get_bool(term->conf, CONF_iterm_images))
        put_dataz(sb, "F");
    if (conf_get_bool(term->conf, CONF_taskbar_progress))
        put_dataz(sb, "P");
    put_byte(sb, '\007');
    kitty_term_send_reply(term, sb->s, sb->len);
    strbuf_free(sb);
}

/*
 * KiTTY: OSC 1337 ; ReportCellSize - iTerm2's answer, OSC 1337 ;
 * ReportCellSize=<height>;<width>;<scale> BEL: the cell in points and the
 * pixels per point. On Windows a point is 1/96 inch, so the scale is
 * dpi/96 and the cell in points is the cell in pixels divided by it. The
 * scale has two decimals: Windows' 125 % and 175 % are 1.25 and 1.75, and
 * one decimal made a client that multiplies back get the cell wrong.
 * Nothing before the window has reported the cell size.
 */
static void term_report_cell_size(Terminal *term)
{
    double scale = (term->dpi > 0 ? term->dpi : 96) / 96.0;
    char *s;

    if (term->cellpix_x <= 0 || term->cellpix_y <= 0)
        return;
    s = dupprintf("\033]1337;ReportCellSize=%.2f;%.2f;%.2f\007",
                  term->cellpix_y / scale, term->cellpix_x / scale, scale);
    kitty_term_send_reply(term, s, strlen(s));
    sfree(s);
}
#if defined(MOD_PERSO) && defined(MOD_FAR2L)
/*
 * KiTTY: XTGETTCAP, DCS + q <names> ST, each name hex-encoded, the names
 * separated by ';'. Each name gets its own reply, DCS 1 + r <name> = <value>
 * ST (the value hex-encoded; a boolean capability has no "= value") or
 * DCS 0 + r <name> ST for a name not answered here - the form kitty, foot
 * and WezTerm send and neovim's parser expects (one reply per name). xterm
 * puts every pair in one reply and stops at the first unknown name, which
 * neovim would not read. Only capabilities this session really has right
 * now are answered; any other name, key names included, is unknown.
 * Returns -1 unknown, 0 a boolean, 1 a value in *val.
 */
static int xtgettcap_value(Terminal *term, const char *name, const char **val)
{
    bool rgb = term->ansi_colour && term->true_colour;
    bool paste = !term->no_bracketed_paste;

    *val = NULL;
    if (!strcmp(name, "TN") || !strcmp(name, "name")) {
        /* the type the connection sent, "-256color" cut as there; kept
         * until the next query - the caller copies it at once */
        static char *sent = NULL;
        sfree(sent);
        sent = kitty_termtype_new(term->conf);
        *val = sent;
        return **val ? 1 : -1;
    }
    if (!strcmp(name, "Co") || !strcmp(name, "colors")) {
        if (!term->ansi_colour)
            return -1;
        *val = term->xterm_256_colour ? "256" : "16";
        return 1;
    }
    if (!strcmp(name, "RGB") && rgb) {
        *val = "8";                    /* bits per channel, as xterm */
        return 1;
    }
    if (!strcmp(name, "Tc") && rgb)
        return 0;
    if (!strcmp(name, "setrgbf") && rgb) {
        *val = "\\E[38;2;%p1%d;%p2%d;%p3%dm";
        return 1;
    }
    if (!strcmp(name, "setrgbb") && rgb) {
        *val = "\\E[48;2;%p1%d;%p2%d;%p3%dm";
        return 1;
    }
    if (!strcmp(name, "Ms") &&
        term->osc52_allowed != OSC52_CLIPBOARD_DENY) {
        *val = "\\E]52;%p1%s;%p2%s\\E\\\\";
        return 1;
    }
    /* styled underlines (SGR 4:n) and the underline colour (SGR 58), the
     * extended names of kitty's and ncurses' terminfo */
    if (!strcmp(name, "Smulx")) { *val = "\\E[4:%p1%dm"; return 1; }
    if (!strcmp(name, "Setulc") && term->ansi_colour) {
        *val = "\\E[58:2::%p1%{65536}%/%d:%p1%{256}%/%{255}%&%d:%p1%{255}%&%dm";
        return 1;
    }
    /* DECSCUSR (term_set_cursor_style); Se as ncurses' xterm+tmux and
     * xterm-kitty: a steady block. CSI 0 SP q would give the session's
     * cursor back, but terminfo's convention is 2. */
    if (!strcmp(name, "Ss")) { *val = "\\E[%p1%d q"; return 1; }
    if (!strcmp(name, "Se")) { *val = "\\E[2 q"; return 1; }
    if (!strcmp(name, "Sync") && term_sync_hold_ms(term)) {
        *val = "\\EP=%p1%ds\\E\\\\";
        return 1;
    }
    /* ncurses' xterm+bracketed and xterm+focus extended names */
    if (!strcmp(name, "BE") && paste) { *val = "\\E[?2004h"; return 1; }
    if (!strcmp(name, "BD") && paste) { *val = "\\E[?2004l"; return 1; }
    if (!strcmp(name, "PS") && paste) { *val = "\\E[200~"; return 1; }
    if (!strcmp(name, "PE") && paste) { *val = "\\E[201~"; return 1; }
    if (!conf_get_bool(term->conf, CONF_no_focus_rep)) {
        if (!strcmp(name, "fe")) { *val = "\\E[?1004h"; return 1; }
        if (!strcmp(name, "fd")) { *val = "\\E[?1004l"; return 1; }
        if (!strcmp(name, "kxIN")) { *val = "\\E[I"; return 1; }
        if (!strcmp(name, "kxOUT")) { *val = "\\E[O"; return 1; }
    }
    return -1;
}

static int xtgettcap_hex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void term_xtgettcap(Terminal *term)
{
    const char *s = term->osc_string + 2;
    const char *end = term->osc_string + term->osc_strlen;

    while (1) {
        const char *seg = s, *val = NULL;
        char name[33];
        int n = 0, kind = -1;
        bool ok;
        strbuf *sb;

        while (s < end && *s != ';')
            s++;
        ok = s > seg && (s - seg) % 2 == 0 &&
            s - seg <= 2 * (int)(sizeof(name) - 1);
        for (const char *q = seg; ok && q < s; q += 2) {
            int hi = xtgettcap_hex(q[0]), lo = xtgettcap_hex(q[1]);
            if (hi < 0 || lo < 0 || !(hi | lo))
                ok = false;
            else
                name[n++] = (char)(hi * 16 + lo);
        }
        name[n] = '\0';
        if (ok)
            kind = xtgettcap_value(term, name, &val);
        sb = strbuf_new();
        put_fmt(sb, "\033P%c+r", kind >= 0 ? '1' : '0');
        put_data(sb, seg, s - seg);
        if (kind > 0) {
            put_byte(sb, '=');
            for (const char *v = val; *v; v++)
                put_fmt(sb, "%02X", (unsigned)(unsigned char)*v);
        }
        put_dataz(sb, "\033\\");
        kitty_term_send_reply(term, sb->s, sb->len);
        strbuf_free(sb);
        if (s >= end)
            break;
        s++;                           /* the ';' */
    }
}
#endif

/*
 * The columns the typed commands start at (133;B), by absolute line number
 * in the fm_* numbering (terminal.h im_*). At most IM_MAX entries, the
 * oldest going first; entries whose lines were evicted are dropped on the
 * way in and out.
 */
#define IM_MAX 4096

static int im_abs(Terminal *term, int y)
{
    return term->fm_base + count234(term->scrollback) + y;
}

static void im_prune(Terminal *term)
{
    int n = 0;
    while (n < term->im_count && term->im_marks[n].line < term->fm_base)
        n++;
    if (n) {
        term->im_count -= n;
        memmove(term->im_marks, term->im_marks + n,
                term->im_count * sizeof(*term->im_marks));
    }
}

/* The slot of absno, or where it would go. */
static int im_slot(Terminal *term, int absno)
{
    int lo = 0, hi = term->im_count;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (term->im_marks[mid].line < absno)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

static void im_add(Terminal *term, int y, int col)
{
    int absno = im_abs(term, y), lo;
    im_prune(term);
    lo = im_slot(term, absno);
    if (lo < term->im_count && term->im_marks[lo].line == absno) {
        term->im_marks[lo].col = col;  /* the prompt was redrawn: the new one */
        return;
    }
    if (term->im_count >= IM_MAX) {
        term->im_count--;
        memmove(term->im_marks, term->im_marks + 1,
                term->im_count * sizeof(*term->im_marks));
        lo = im_slot(term, absno);
    }
    sgrowarray(term->im_marks, term->im_size, term->im_count);
    memmove(term->im_marks + lo + 1, term->im_marks + lo,
            (term->im_count - lo) * sizeof(*term->im_marks));
    term->im_marks[lo].line = absno;
    term->im_marks[lo].col = col;
    term->im_count++;
}

/* The column the command on line y starts at; 0 when unknown. */
int im_col(Terminal *term, int y)
{
    int absno = im_abs(term, y), lo;
    im_prune(term);
    lo = im_slot(term, absno);
    if (lo < term->im_count && term->im_marks[lo].line == absno)
        return term->im_marks[lo].col < term->cols ?
               term->im_marks[lo].col : term->cols;
    return 0;
}

/* do_osc: OSC 133. */
static void term_osc133(Terminal *term)
{
    int code, y;

    if (term->alt_which)
        return;
    switch (ps_osc133(term->osc_string, term->osc_strlen, &code)) {
      case PS133_PROMPT:
        mark_set(term, term->curs.y, LATTR_PROMPT, true);
        term->marks_seen = true;
        break;
      case PS133_INPUT:
        mark_set(term, term->curs.y, LATTR_INPUT, true);
        im_add(term, term->curs.y, term->curs.x);
        break;
      case PS133_OUTPUT:
        mark_set(term, term->curs.y, LATTR_OUTPUT, true);
        break;
      case PS133_END:
        if (code > 0) {
            y = mark_find_up(term, term->curs.y, LATTR_PROMPT);
            if (y != INT_MIN)
                mark_set(term, y, LATTR_FAILED, true);
        }
        break;
      default:
        break;
    }
}


/*
 * do_osc's OSC switch for the numbers KiTTY++ adds or answers. The string
 * is NUL-terminated. True when the number is one of them (handled, or
 * ignored on purpose); false leaves it to do_osc's own switch (window
 * titles, and every number nobody handles).
 */
bool kitty_osc_dispatch(Terminal *term)
{
    switch (term->esc_args[0]) {
      case 4:
      case 10:
      case 11:
      case 12: {
        /*
         * xterm colour queries. OSC 4;n;? asks for palette entry n,
         * OSC 10;? / 11;? / 12;? for the default foreground, the default
         * background and the cursor colour. The reply repeats the
         * request with the colour in place of the "?", as
         * rgb:RRRR/GGGG/BBBB, which is what vim and neovim read to
         * choose a light or dark colour scheme.
         *
         * Only the query is served. The same sequences with a colour
         * instead of "?" would SET that colour under remote control, and
         * are ignored here like every other unknown OSC.
         *
         * The reply goes straight to the backend, not through
         * ldisc_send(): with local line editing on the line discipline
         * would append it to the line being typed, and with local echo
         * on it would paint it on this screen first. See
         * kitty_term_send_reply() for the full account.
         */
        if (strcmp(term->osc_string, "?") != 0)
            break;
        unsigned which = term->esc_args[0];
        unsigned index =
            which == 4 ? term->esc_args[1] :
            which == 10 ? OSC4_COLOUR_fg :
            which == 11 ? OSC4_COLOUR_bg : OSC4_COLOUR_cursor_bg;
        if (index >= OSC4_NCOLOURS)
            break;
        rgb colour = term->palette[index];
        char *reply_buf = which == 4 ?
            dupprintf("\033]4;%u;rgb:%04x/%04x/%04x\007", index,
                      (unsigned)colour.r * 0x0101,
                      (unsigned)colour.g * 0x0101,
                      (unsigned)colour.b * 0x0101) :
            dupprintf("\033]%u;rgb:%04x/%04x/%04x\007", which,
                      (unsigned)colour.r * 0x0101,
                      (unsigned)colour.g * 0x0101,
                      (unsigned)colour.b * 0x0101);
        kitty_term_send_reply(term, reply_buf, strlen(reply_buf));
        sfree(reply_buf);
        break;
      }
      case 7:
        /* OSC 7: shell reports its working directory as
         * file://hostname/path.  Data-plane only -- the kitty layer
         * validates and stores it (opt-in, CONF_osc7_cwd_tracking,
         * default off) for use as the default remote target of
         * pscp/WinSCP transfers.  Nothing is ever executed; this is
         * the safe replacement for the removed __pw/__ws title-scan
         * dispatcher (CVE-2024-23749), which stays dead. */
        kitty_set_remote_cwd(term->osc_string);
        break;
      case 8:
        /* OSC 8: a hyperlink the host sends (kitty_osc8). */
        kitty_osc8(term);
        break;
      case 52:
        /* OSC 52: the host wants to set the local clipboard, or (with Pd
         * "?") to read it. Text only - the protocol carries nothing else. */
        osc52_set_clipboard(term);
        break;
      case 5522:
        /* OSC 5522: the kitty clipboard protocol. Reads and writes both go
         * through the OSC 52 permission gates; writes may carry any MIME
         * type. */
        osc5522_process(term);
        break;
      case 1337:
        /* OSC 1337: iTerm2 inline images (File=, MultipartFile=), into
         * the kitty graphics store, under their own setting. The other
         * 1337 commands stay ignored, except the Capabilities query,
         * answered whatever the settings (term_capabilities_report). */
        if (!term->osc_str_overflow &&
            !strcmp(term->osc_string, "Capabilities")) {
            term_capabilities_report(term);
            break;
        }
        /* KiTTY: iTerm2's ReportCellSize - a query like CSI 16 t,
         * answered whatever the settings */
        if (!term->osc_str_overflow &&
            !strcmp(term->osc_string, "ReportCellSize")) {
            term_report_cell_size(term);
            break;
        }
        if (conf_get_bool(term->conf, CONF_iterm_images)) {
            int dx, dy;
            kitty_gfx_iterm(term, term->osc_string, term->osc_strlen,
                            term->osc_str_overflow, &dx, &dy);
            gfx_cursor_advance(term, dx, dy);
        }
        break;
      case 5113:
        /* OSC 5113: kitty's file-transfer protocol (`kitten transfer` on
         * the far end). Sessions, files and the permission dialog live
         * in kitty/kitty_transfer.c. */
        kitty_transfer_osc(term);
        break;
      case 133:
        /* The prompt marks on the lines (term_osc133), then the
         * program status they end (kitty_hostnotify.c). */
        term_osc133(term);
        /* fall through */
      case 9:
      case 777:
      case 99:
      case 7501:
        /* Desktop notifications from the host (OSC 9, OSC 777, OSC 99),
         * taskbar progress (OSC 9;4), program status (OSC 7501) and the
         * shell's prompt marks (OSC 133, which end a running status).
         * The ConEmu numbers of OSC 9 arrive here as the start of the
         * string (the numeric parser stops at the first ';'), so they are
         * told apart there. The per-session setting, the limits and the
         * flood rule live in kitty/kitty_hostnotify.c. */
        kitty_hostnotify_osc(term, term->esc_args[0], term->osc_string,
                             term->osc_strlen, term->osc_str_overflow);
        break;
      default:
        return false;
    }
    return true;
}

/*
 * do_osc's front for the DCS strings KiTTY++ answers. True when the
 * string was one of them and is handled.
 */
bool kitty_osc_dcs(Terminal *term)
{
    /* KiTTY: the DCS form of synchronized output (term_sync_dcs) */
    if (term->osc_type == OSCLIKE_DCS && !term->osc_str_overflow &&
        term_sync_dcs(term))
        return true;
#ifdef MOD_FAR2L
    /* KiTTY: XTGETTCAP (term_xtgettcap); the Sixel detection never takes
     * a "+q" string */
    if (term->osc_type == OSCLIKE_DCS && !term->osc_str_overflow &&
        term->osc_strlen >= 2 && term->osc_string[0] == '+' &&
        term->osc_string[1] == 'q') {
        term_xtgettcap(term);
        return true;
    }
#endif
    return false;
}
#endif /* MOD_PERSO */
