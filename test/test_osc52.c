/*
 * test_osc52 - regression tests for OSC 52 (a remote host putting text on the
 * local clipboard) and for the OSC accumulation buffer that grows to hold it.
 *
 * These lock in behaviour that is easy to break and expensive to get wrong:
 *
 *  - the READ direction (Pd == "?", where the host asks for YOUR clipboard) is
 *    refused. That is an exfiltration channel and the reason upstream PuTTY
 *    leaves OSC 52 out altogether;
 *  - a malformed or truncated payload is refused WHOLE, never handed over in
 *    part, because half a clipboard looks like success and pasting half a
 *    command line is how that becomes somebody's bad day;
 *  - the policy gate is honoured (CONF_osc52_clipboard: deny/allow/ask). "Ask"
 *    is a modeless box, so the write waits for its answer: the box is a stub
 *    here and the tests answer it (test_write_confirm);
 *  - a real clipboard-sized payload survives, i.e. the buffer actually grows;
 *  - and every OTHER OSC sequence still stops at the size it always did, so the
 *    clipboard feature cannot be used to hand us a multi-megabyte window title.
 *
 * Built against terminal.c compiled WITH MOD_PERSO: the OSC 52 handler lives
 * behind that define, and the guiterminal library is built without it, so
 * linking the library instead would silently test nothing at all.
 *
 * Returns non-zero on any failure.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "putty.h"
#include "terminal.h"
#include "../kitty/kitty_far2l_image.h"        /* KiTTY: far2l images */
#include "../kitty/kitty_far2l_image_term.h"
#include "../kitty/kitty_gfx.h"                /* KiTTY: kitty graphics (APC _G) */
#include "../kitty/kitty_gfx_term.h"
#include <objbase.h>                    /* CoInitialize: WIC for far2l PNG */

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

const char *appname = "test_osc52";

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

/* The two KiTTY entry points terminal.c calls when built with MOD_PERSO. */
char *kitty_expand_wintitle(const char *title, const char *hostname, Conf *conf)
{ return dupstr(title ? title : ""); }
void kitty_set_remote_cwd(const char *osc7) { }

/*
 * The OSC 52 clipboard-READ seams. The real ones live in kitty/kitty_osc52.c and
 * need a window, the Windows clipboard and a modal dialog; these stubs are how
 * the permission engine gets tested without any of that, which is the whole
 * reason the engine and the platform half are separate files.
 *
 * Three separate counters, because three different things can happen and two of
 * them look alike from the outside:
 *  - osc52_sends is the one that matters. It counts the clipboard actually
 *    LEAVING the machine, which is the event the whole design exists to control;
 *  - osc52_dialogs counts times the engine tried to ASK, so "refused without
 *    prompting" and "prompted, and the user said no" can be told apart;
 *  - osc52_gets counts clipboard fetches, which happens on the ask path too
 *    (the dialog shows a masked summary), so it is NOT a proxy for a send.
 */
static const wchar_t *stub_clip = L"secret";
static int osc52_gets;             /* times the clipboard was fetched */
static int osc52_sends;            /* times it was actually sent to the host */
static int osc52_dialogs;          /* times the engine tried to ask */
static bool osc52_dialog_answer;   /* what the fake user says */
static int osc52_dialog_grant;     /* and for how long */
static int osc52_state_changes;

/* Captures the last sequence sent to the host, so the OSC 5522 tests can check
 * the wire format and not merely that something went out. */
static char osc52_last_send[4096];
static char osc52_all[262144];     /* every reply of the current feed, concatenated */
static size_t osc52_all_len;
static int osc52_last_len;

void kitty_osc52_send_raw(Terminal *term, const char *data, size_t len)
{
    osc52_sends++;
    if (osc52_all_len + len < sizeof(osc52_all) - 1) {
        memcpy(osc52_all + osc52_all_len, data, len);
        osc52_all_len += len;
        osc52_all[osc52_all_len] = '\0';
    }
    osc52_last_len = (int)(len < sizeof(osc52_last_send) - 1
                           ? len : sizeof(osc52_last_send) - 1);
    memcpy(osc52_last_send, data, osc52_last_len);
    osc52_last_send[osc52_last_len] = '\0';
}

/* stub_clip_busy models the clipboard being HELD by another program - a state
 * the real one reports separately from "empty", because the read path treats
 * empty as a decision and refuses without prompting. */
static bool stub_clip_busy = false;
static void counters_reset(void);


wchar_t *kitty_osc52_get_clipboard_ex(int *len, bool *unavailable)
{
    size_t n;
    wchar_t *out;
    osc52_gets++;
    if (unavailable) *unavailable = false;
    if (stub_clip_busy) {
        if (len) *len = 0;
        if (unavailable) *unavailable = true;
        return NULL;
    }
    if (!stub_clip) {
        if (len) *len = 0;
        return NULL;
    }
    n = wcslen(stub_clip);
    out = snewn(n + 1, wchar_t);
    memcpy(out, stub_clip, (n + 1) * sizeof(wchar_t));
    if (len) *len = (int)n;
    return out;
}

wchar_t *kitty_osc52_get_clipboard(int *len)
{
    return kitty_osc52_get_clipboard_ex(len, NULL);
}

/* The image on the stub clipboard, if any. */
static const unsigned char *stub_png = NULL;
static size_t stub_png_len = 0;

unsigned char *kitty_osc52_get_clipboard_png(size_t *len, bool *unavailable)
{
    unsigned char *out;
    if (unavailable) *unavailable = false;
    *len = 0;
    if (stub_clip_busy) {
        if (unavailable) *unavailable = true;
        return NULL;
    }
    if (!stub_png)
        return NULL;
    out = snewn(stub_png_len, unsigned char);
    memcpy(out, stub_png, stub_png_len);
    *len = stub_png_len;
    return out;
}

bool kitty_osc52_clipboard_has_image(void) { return stub_png != NULL; }

/* Deterministic "random" for the paste-event token: a counter, so two events
 * get two different tokens and a test can tell them apart. */
static unsigned char stub_random_counter = 1;
void kitty_osc52_random(unsigned char *buf, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++)
        buf[i] = (unsigned char)(stub_random_counter * 7 + i);
    stub_random_counter++;
}

/* The OSC 5522 WRITE seam: what the terminal asked to have put on the clipboard,
 * all formats of one transaction in one call. Recorded, never applied. */
static int osc52_set_calls;
static int osc52_set_nformats;
static char osc52_set_mimes[1024];       /* the format names, comma-joined */
static char osc52_set_text[4096];        /* the bytes of the FIRST format */
static size_t osc52_set_text_len;
static size_t osc52_set_first_len;       /* its full length, uncut */
static bool osc52_set_fail;              /* models "the clipboard could not be set" */

bool kitty_osc52_set_clipboard_formats(const KittyClipFormat *fmts, int n)
{
    int i;
    osc52_set_calls++;
    if (osc52_set_fail)
        return false;
    osc52_set_nformats = n;
    osc52_set_mimes[0] = '\0';
    for (i = 0; i < n; i++) {
        if (i)
            strcat(osc52_set_mimes, ",");
        strncat(osc52_set_mimes, fmts[i].mime,
                sizeof(osc52_set_mimes) - strlen(osc52_set_mimes) - 2);
    }
    osc52_set_first_len = n ? fmts[0].len : 0;
    osc52_set_text_len = osc52_set_first_len < sizeof(osc52_set_text) - 1
                       ? osc52_set_first_len : sizeof(osc52_set_text) - 1;
    if (n)
        memcpy(osc52_set_text, fmts[0].data, osc52_set_text_len);
    osc52_set_text[osc52_set_text_len] = '\0';
    return true;
}

/* The read box is modeless: opening it only records that it is open, and the
 * fake user answers it once the data that raised it has been fed (term_data
 * below) - the platform side answers from a callback the same way, after the
 * parser has returned. */
static bool osc52_read_box_open;
static bool osc52_read_box_manual;  /* a test answers the box itself */
void kitty_osc52_read_confirm(Terminal *term, const wchar_t *clip, int clip_len,
                              const char *claim)
{
    osc52_dialogs++;
    osc52_read_box_open = true;
}
void kitty_osc52_read_confirm_end(Terminal *term) { osc52_read_box_open = false; }

/* Every feed in this file goes through here: the real term_data, then the
 * answer to a read box it opened. */
static size_t test_term_data(Terminal *term, const void *data, size_t len)
{
    size_t r = term_data(term, data, len);
    if (osc52_read_box_open && !osc52_read_box_manual) {
        osc52_read_box_open = false;
        term_osc52_read_answer(term, osc52_dialog_answer, osc52_dialog_grant,
                               false);
    }
    return r;
}
#define term_data test_term_data

bool kitty_osc52_save_deny_for_host(Terminal *term) { return true; }
/* true: the tests model a normal window with a title bar, so the full-screen
 * balloon fallback stays out of the way of the counters. */
bool kitty_osc52_title_visible(void) { return true; }
/* Counted, because "the ordinary case is silent" is a promise worth keeping: a
 * notification that fires on a normal clipboard write would be worse than none,
 * since the first thing anybody does with a noisy notifier is switch it off - and
 * then the one that matters never arrives either. */
static int osc52_notifies;
static int osc52_notify_action;

void kitty_osc52_notify(Terminal *term, const char *t, const char *m, int a)
{
    osc52_notifies++;
    osc52_notify_action = a;
}
void kitty_osc52_state_changed(Terminal *term) { osc52_state_changes++; }
/* The write confirmation box: opened, never answered here - the tests answer
 * through term_osc52_write_answer, as the platform side does once it closes. */
static int osc52_wconfirms;
static int osc52_wconfirm_ends;
void kitty_osc52_write_confirm(Terminal *term) { osc52_wconfirms++; }
void kitty_osc52_write_confirm_end(Terminal *term) { osc52_wconfirm_ends++; }
/* OSC 5113 (kitty/kitty_transfer.c) is not under test here: the terminal
 * dispatches to these two, so they exist and do nothing. */
void kitty_transfer_osc(Terminal *term) { (void)term; }
void kitty_transfer_free(Terminal *term) { (void)term; }
/* Notifications from the host (kitty/kitty_hostnotify.c): the stub records
 * what the terminal handed over, so test_hostnotify_dispatch can check the
 * dispatch and the OSC 99 ceiling. */
static int hn_calls;
static unsigned hn_last_osc;
static size_t hn_last_len;
static bool hn_last_overflow;
static char hn_last[64];
void kitty_hostnotify_osc(Terminal *term, unsigned osc, const char *s,
                          size_t len, bool overflow)
{
    (void)term;
    hn_calls++;
    hn_last_osc = osc;
    hn_last_len = len;
    hn_last_overflow = overflow;
    snprintf(hn_last, sizeof(hn_last), "%.*s",
             (int)(len < sizeof(hn_last) - 1 ? len : sizeof(hn_last) - 1), s);
}
void kitty_hostnotify_term_free(Terminal *term) { (void)term; }
/* far2l's notification request reaches the same notice through this one;
 * the stub records the title and text it was handed. */
static int hn_notice_calls;
static char hn_notice_title[64], hn_notice_body[64];
void kitty_host_notice(Terminal *term, const char *title, const char *body)
{
    (void)term;
    hn_notice_calls++;
    snprintf(hn_notice_title, sizeof(hn_notice_title), "%s", title ? title : "");
    snprintf(hn_notice_body, sizeof(hn_notice_body), "%s", body ? body : "");
}

/*
 * The far2l platform seams (kitty/kitty_far2l.h). A fake clipboard of a few
 * formats, holding the wire bytes as they came, so a set and the read back can
 * be compared; the box is recorded and answered by the tests through
 * term_far2l_open_answer, as the platform side does once it closes.
 */
#define F2L_FAKE_FMTS 8
static struct { uint32_t fmt; unsigned char *data; size_t len; } f2l_clip[F2L_FAKE_FMTS];
static int f2l_sets, f2l_empties, f2l_set_emptied, f2l_confirms, f2l_confirm_ends;
static int f2l_saves;
static bool f2l_offer_always;
static bool f2l_last_set_empty_first;

static void f2l_fake_empty(void)
{
    int i;
    for (i = 0; i < F2L_FAKE_FMTS; i++) {
        sfree(f2l_clip[i].data);
        f2l_clip[i].data = NULL;
        f2l_clip[i].fmt = 0;
        f2l_clip[i].len = 0;
    }
}
uint32_t kitty_far2l_clip_register(const char *name, size_t len)
{ return len ? 0xC0DE : 0; }
bool kitty_far2l_clip_available(uint32_t fmt)
{
    int i;
    for (i = 0; i < F2L_FAKE_FMTS; i++)
        if (f2l_clip[i].data && f2l_clip[i].fmt == fmt)
            return true;
    return false;
}
bool kitty_far2l_clip_empty(void)
{
    f2l_empties++;
    f2l_fake_empty();
    return true;
}
bool kitty_far2l_clip_set(uint32_t fmt, const unsigned char *data, size_t len,
                          bool empty_first)
{
    int i, slot = -1;
    f2l_sets++;
    f2l_last_set_empty_first = empty_first;
    if (empty_first) {
        f2l_set_emptied++;
        f2l_fake_empty();
    }
    for (i = 0; i < F2L_FAKE_FMTS && slot < 0; i++)
        if (f2l_clip[i].data && f2l_clip[i].fmt == fmt)
            slot = i;
    for (i = 0; i < F2L_FAKE_FMTS && slot < 0; i++)
        if (!f2l_clip[i].data)
            slot = i;
    if (slot < 0)
        return false;
    sfree(f2l_clip[slot].data);
    f2l_clip[slot].fmt = fmt;
    f2l_clip[slot].data = snewn(len ? len : 1, unsigned char);
    memcpy(f2l_clip[slot].data, data, len);
    f2l_clip[slot].len = len;
    return true;
}
unsigned char *kitty_far2l_clip_get(uint32_t fmt, size_t *len)
{
    int i;
    *len = 0;
    for (i = 0; i < F2L_FAKE_FMTS; i++)
        if (f2l_clip[i].data && f2l_clip[i].fmt == fmt && f2l_clip[i].len) {
            unsigned char *out = snewn(f2l_clip[i].len, unsigned char);
            memcpy(out, f2l_clip[i].data, f2l_clip[i].len);
            *len = f2l_clip[i].len;
            return out;
        }
    return NULL;
}
void kitty_far2l_confirm(Terminal *term, bool offer_always)
{
    f2l_confirms++;
    f2l_offer_always = offer_always;
}
void kitty_far2l_confirm_end(Terminal *term) { f2l_confirm_ends++; }
bool kitty_far2l_save_client_ids(Terminal *term) { f2l_saves++; return true; }
static int f2l_max_rows = 0, f2l_max_cols = 0;
/* every step of far2l_input_gen tells the window (title suffix, notice) */
static int f2l_events_changes = 0;
void kitty_far2l_events_changed(Terminal *term) { (void)term; f2l_events_changes++; }
bool kitty_far2l_max_cells(Terminal *term, int *rows, int *cols)
{
    if (f2l_max_rows <= 0)
        return false;
    *rows = f2l_max_rows;
    *cols = f2l_max_cols;
    return true;
}

typedef struct Mock {
    Terminal *term;
    Conf *conf;
    struct unicode_data ucsdata[1];
    strbuf *title;

    /* what OSC 52 last wrote to the clipboard, and how many times */
    wchar_t *clip;
    int clip_len;
    int clip_writes;

    bool any_test_failed;

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
static void mock_set_icon_title(TermWin *win, const char *title, int cp) {}
/* far2l's maximise/restore requests arrive here: recorded. */
static int mock_maximise_calls;
static bool mock_maximised;
static void mock_set_maximised(TermWin *win, bool maximised)
{
    mock_maximise_calls++;
    mock_maximised = maximised;
}

static void mock_set_title(TermWin *win, const char *title, int codepage)
{
    Mock *mk = container_of(win, Mock, tw);
    strbuf_clear(mk->title);
    put_dataz(mk->title, title);
}

/* An ordinary paste request reaching the platform: counted, nothing pasted. */
static int mock_paste_requests;
static void mock_clip_request_paste(TermWin *win, int clipboard)
{
    mock_paste_requests++;
}

static void mock_clip_write(TermWin *win, int clipboard, wchar_t *text,
                            int *attrs, truecolour *colours, int len,
                            bool deselect)
{
    Mock *mk = container_of(win, Mock, tw);
    sfree(mk->clip);
    mk->clip = snewn(len + 1, wchar_t);
    memcpy(mk->clip, text, len * sizeof(wchar_t));
    mk->clip[len] = L'\0';
    mk->clip_len = len;
    mk->clip_writes++;
}

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
    .clip_write = mock_clip_write,
    .clip_request_paste = mock_clip_request_paste,
    .set_maximised = mock_set_maximised,
};

static Mock *mock_new(void)
{
    Mock *mk = snew(Mock);
    memset(mk, 0, sizeof(*mk));
    mk->conf = conf_new();
    do_defaults(NULL, mk->conf);
    init_ucs_generic(mk->conf, mk->ucsdata);
    mk->ucsdata->line_codepage = CP_UTF8;
    mk->title = strbuf_new();
    mk->tw.vt = &mock_termwin_vt;
    return mk;
}

static void mock_free(Mock *mk)
{
    conf_free(mk->conf);
    term_free(mk->term);
    strbuf_free(mk->title);
    sfree(mk->clip);
    sfree(mk);
}

static int failures = 0;

static void fail(const char *what, const char *detail)
{
    printf("FAIL %s: %s\n", what, detail);
    failures++;
}

/* Feed a sequence with the clipboard policy forced to 'policy', and report what
 * reached the clipboard. Returns the number of clipboard writes it caused. */
static int feed(Mock *mk, int policy, const char *data, size_t len)
{
    mk->term->osc52_allowed = policy;
    /* These cases fire many writes in the same second, which is exactly what the
     * write rate cap exists to stop. Reset the window before each so the cap is
     * not what is under test here - it has its own cases in
     * test_clipboard_write_rate(). */
    mk->term->clip_write_second = 0;
    mk->term->clip_write_count = 0;
    mk->clip_writes = 0;
    term_data(mk->term, data, len);
    term_update(mk->term);
    return mk->clip_writes;
}

static void expect_clip(Mock *mk, const char *what, const char *seq,
                        const wchar_t *want)
{
    if (feed(mk, OSC52_CLIPBOARD_ALLOW, seq, strlen(seq)) != 1) {
        fail(what, "nothing was written to the clipboard");
        return;
    }
    if (wcscmp(mk->clip, want) != 0)
        fail(what, "clipboard content differs from what was sent");
}

static void expect_refused(Mock *mk, const char *what, const char *seq)
{
    if (feed(mk, OSC52_CLIPBOARD_ALLOW, seq, strlen(seq)) != 0)
        fail(what, "the clipboard was written when it should not have been");
}

/* ---------------------------------------------------------------------------
 * The READ direction
 * ------------------------------------------------------------------------- */

/* Mirrors the grant kinds in terminal.c. Four values that the design settled;
 * kept in step by hand rather than exported, for the same reason the platform
 * half does it. */
enum {
    GRANT_ONCE, GRANT_MINUTES, GRANT_REQUESTS, GRANT_SESSION,
};

#define READ_SEQ "\033]52;c;?\007"

/* Feed one clipboard-read request and report what the gate did. */
static void feed_read(Mock *mk)
{
    osc52_gets = 0;
    osc52_sends = 0;
    osc52_dialogs = 0;
    term_data(mk->term, READ_SEQ, strlen(READ_SEQ));
    term_update(mk->term);
}

/* Assert on both halves of the outcome: whether the clipboard actually went to
 * the host, and whether the user was asked. Refusing silently and refusing after
 * a prompt are different behaviours and the tests have to tell them apart. */
static void expect_read(Mock *mk, const char *what, int want_sends,
                        int want_dialogs)
{
    feed_read(mk);
    if (osc52_sends != want_sends)
        fail(what, osc52_sends > want_sends
             ? "the clipboard was SENT when it should not have been"
             : "the clipboard was not sent when it should have been");
    if (osc52_dialogs != want_dialogs)
        fail(what, osc52_dialogs > want_dialogs
             ? "the user was asked when they should not have been"
             : "the user was not asked when they should have been");
}

/* Put the terminal in a known state: reads allowed to ask, focused, no standing
 * decision, no history. Each test starts from here so an earlier grant or an
 * earlier refusal cannot make the next one pass for the wrong reason. */
static void read_reset(Mock *mk)
{
    /* NOTE: term->conf, not mk->conf. term_init() takes a COPY of the Conf it is
     * given, so settings poked into the mock's own Conf after that point are read
     * by nobody - which makes every test pass or fail for the wrong reason. */
    Conf *c = mk->term->conf;
    conf_set_int(c, CONF_osc52_clipboard_read, OSC52_READ_ASK);
    conf_set_bool(c, CONF_clipboard_require_focus, true);
    conf_set_int(c, CONF_osc52_read_interval, 0);
    conf_set_int(c, CONF_osc52_read_max, 0);
    conf_set_int(c, CONF_osc52_read_dialogs, 3);
    conf_set_int(c, CONF_osc52_read_minutes, 10);
    conf_set_int(c, CONF_osc52_read_requests, 25);
    mk->term->has_focus = true;
    mk->term->osc52_read_decision = 0;
    mk->term->osc52_read_until = 0;
    mk->term->osc52_read_remaining = 0;
    mk->term->osc52_read_served = 0;
    mk->term->osc52_read_last_served = 0;
    mk->term->osc52_read_prompts = 0;
    mk->term->osc52_read_prompt_window = 0;
    mk->term->osc52_read_asking = false;
    mk->term->osc52_read_refused_quiet = 0;
    mk->term->osc52_read_refused_logged = 0;
    /* Clear the OSC 5522 approvals too. Without this an approval granted by one
     * test silently satisfies the next one, which is how "a one-off answer was
     * remembered" showed up as a code bug when it was a harness bug. */
    {
        int i;
        for (i = 0; i < OSC5522_MAX_APPROVALS; i++) {
            sfree(mk->term->osc5522_pw[i]);
            sfree(mk->term->osc5522_pw_name[i]);
            mk->term->osc5522_pw[i] = NULL;
            mk->term->osc5522_pw_name[i] = NULL;
            mk->term->osc5522_pw_until[i] = 0;
        }
        mk->term->osc5522_pw_count = 0;
    }
    stub_clip = L"secret";
    osc52_dialog_answer = true;
    osc52_dialog_grant = GRANT_ONCE;
}

static void test_read_direction(Mock *mk)
{
    /*
     * The default. This is the one that matters most: a host that asks a KiTTY
     * nobody has configured must get silence, and must not even cause a prompt.
     */
    read_reset(mk);
    conf_set_int(mk->term->conf, CONF_osc52_clipboard_read, OSC52_READ_DENY);
    expect_read(mk, "reads default to Deny", 0, 0);

    /*
     * No focus, nothing happens - and this is checked BEFORE any stored
     * permission, so a grant cannot be spent while the user is working
     * elsewhere. The grant must survive: it is suspended, not cancelled.
     */
    read_reset(mk);
    mk->term->osc52_read_decision = 1;
    mk->term->osc52_read_remaining = -1;
    mk->term->has_focus = false;
    expect_read(mk, "unfocused window serves nothing", 0, 0);
    if (mk->term->osc52_read_decision != 1)
        fail("unfocused window", "the grant was thrown away instead of paused");
    /* and it resumes on its own when focus comes back, without asking again */
    mk->term->has_focus = true;
    expect_read(mk, "grant resumes when focus returns", 1, 0);

    /* The focus rule can be switched off, because it changes write behaviour
     * that shipped working; when it is off, an unfocused read still serves. */
    read_reset(mk);
    conf_set_bool(mk->term->conf, CONF_clipboard_require_focus, false);
    mk->term->osc52_read_decision = 1;
    mk->term->osc52_read_remaining = -1;
    mk->term->has_focus = false;
    expect_read(mk, "focus rule off", 1, 0);

    /* "Just this request" remembers nothing: the next request asks again. */
    read_reset(mk);
    osc52_dialog_grant = GRANT_ONCE;
    expect_read(mk, "allow once, first request", 1, 1);
    if (mk->term->osc52_read_decision != 0)
        fail("allow once", "a one-off answer was remembered");
    expect_read(mk, "allow once, second request asks again", 1, 1);

    /* A refusal, on the other hand, applies for as long as it was given for,
     * and does so without prompting again. That is what makes "deny for ten
     * minutes" a usable way to get rid of a host that will not stop asking. */
    read_reset(mk);
    osc52_dialog_answer = false;
    osc52_dialog_grant = GRANT_SESSION;
    expect_read(mk, "deny for the session, first request", 0, 1);
    expect_read(mk, "deny for the session holds", 0, 0);
    expect_read(mk, "deny for the session still holds", 0, 0);

    /* A request-counted grant covers exactly that many, then asks again. */
    read_reset(mk);
    mk->term->osc52_read_decision = 1;
    mk->term->osc52_read_remaining = 2;
    expect_read(mk, "counted grant, 1 of 2", 1, 0);
    expect_read(mk, "counted grant, 2 of 2", 1, 0);
    expect_read(mk, "counted grant is spent", 1, 1);

    /* A time-limited grant that has run out asks again rather than serving. */
    read_reset(mk);
    mk->term->osc52_read_decision = 1;
    mk->term->osc52_read_remaining = -1;
    mk->term->osc52_read_until = (unsigned long)time(NULL) - 1;
    osc52_dialog_answer = false;
    expect_read(mk, "expired grant", 0, 1);

    /*
     * The hand-over rate limit is PACING, and pacing refuses the request without
     * touching the grant or asking anything.
     *
     * It used to withdraw the permission and ask again, and hands-on testing
     * killed that: a host asking every second produced a DIALOG every second,
     * which is the prompt storm the whole design exists to prevent, and it
     * overrode a decision the user had explicitly made - "ten minutes" has to
     * mean ten minutes, not "until the host gets impatient". Hence the two
     * assertions below: nothing sent, and NO dialog, and the grant still standing.
     */
    read_reset(mk);
    conf_set_int(mk->term->conf, CONF_osc52_read_interval, 3600);
    mk->term->osc52_read_decision = 1;
    mk->term->osc52_read_remaining = -1;
    mk->term->osc52_read_last_served = (unsigned long)time(NULL);
    osc52_dialog_answer = false;
    expect_read(mk, "too fast: refused, no dialog", 0, 0);
    if (mk->term->osc52_read_decision <= 0)
        fail("hand-over rate limit", "pacing must not cost the user their grant");

    /* Same for the whole-window ceiling. */
    read_reset(mk);
    conf_set_int(mk->term->conf, CONF_osc52_read_max, 2);
    mk->term->osc52_read_decision = 1;
    mk->term->osc52_read_remaining = -1;
    mk->term->osc52_read_served = 2;
    osc52_dialog_answer = false;
    expect_read(mk, "window ceiling reached", 0, 1);

    /*
     * A BUSY clipboard is not an empty one. When another program holds the
     * clipboard open, the fetch fails - and that used to be indistinguishable
     * from "there is nothing on the clipboard", a state this code answers by
     * refusing WITHOUT asking. So a read request that landed at the wrong moment
     * disappeared: nothing sent, no dialog, and nothing in the Event Log either.
     * Fail-closed, but silent, and the user had no way to know their request had
     * been dropped rather than denied.
     *
     * Nothing sent and no dialog is still the right OUTCOME, and that is all
     * this level can check: the mock has no LogContext, and logevent() returns
     * early without one, so the refusal REASON is invisible here. The Event Log
     * line is asserted by the clipboard harness instead - it can hold the real
     * Windows clipboard, and it is mutation-tested against exactly this branch.
     * What is proved here is that a busy clipboard is never SERVED and never
     * prompts about a clipboard nobody can read.
     */
    read_reset(mk);
    stub_clip_busy = true;
    osc52_dialog_answer = true;          /* would allow, if it were ever asked */
    expect_read(mk, "clipboard held by another program", 0, 0);
    stub_clip_busy = false;
    /* ...and the very next request, with the clipboard free again, behaves
     * normally: a busy moment must not leave the session unable to ask. */
    expect_read(mk, "recovers once the clipboard is free", 1, 1);
    if (mk->term->osc52_read_decision > 0)
        fail("window ceiling", "the grant survived the ceiling");

    /* The prompt itself is rationed, so a host cannot use the dialog as the
     * attack. Past the cap, requests are refused WITHOUT asking. */
    read_reset(mk);
    conf_set_int(mk->term->conf, CONF_osc52_read_dialogs, 2);
    osc52_dialog_answer = false;
    osc52_dialog_grant = GRANT_ONCE;
    expect_read(mk, "prompt 1 of 2", 0, 1);
    expect_read(mk, "prompt 2 of 2", 0, 1);
    expect_read(mk, "prompt cap reached", 0, 0);

    /* An empty clipboard never raises a dialog. A prompt about nothing is a
     * prompt that teaches people to click Allow. */
    read_reset(mk);
    stub_clip = NULL;
    expect_read(mk, "empty clipboard", 0, 0);

    /* Reads and writes are separate permissions: allowing writes must not
     * permit a read. */
    read_reset(mk);
    conf_set_int(mk->term->conf, CONF_osc52_clipboard_read, OSC52_READ_DENY);
    mk->term->osc52_allowed = OSC52_CLIPBOARD_ALLOW;
    expect_read(mk, "write permission does not grant a read", 0, 0);
}

/* ---------------------------------------------------------------------------
 * OSC 5522 - the kitty clipboard protocol
 * ------------------------------------------------------------------------- */

/* base64 of a plain string, for building request payloads. */
static char *b64_bytes(const unsigned char *p, size_t n)
{
    strbuf *sb = strbuf_new();
    base64_encode_bs(BinarySink_UPCAST(sb), make_ptrlen(p, n), 0);
    return strbuf_to_str(sb);
}

static char *b64(const char *s)
{
    strbuf *sb = strbuf_new();
    char *r;
    base64_encode_bs(BinarySink_UPCAST(sb), ptrlen_from_asciz(s), 0);
    r = strbuf_to_str(sb);
    return r;
}

/* Feed one OSC 5522 sequence: OSC 5522 ; <meta> ; <base64 payload> ST */
static void feed_5522(Mock *mk, const char *meta, const char *plain_payload)
{
    char *p = plain_payload ? b64(plain_payload) : NULL;
    char *seq = p ? dupprintf("\033]5522;%s;%s\033\\", meta, p)
                  : dupprintf("\033]5522;%s\033\\", meta);
    counters_reset();
    term_data(mk->term, seq, strlen(seq));
    term_update(mk->term);
    sfree(seq);
    sfree(p);
}

/* Like feed_5522, but the payload goes on the wire AS GIVEN - for the base64
 * strictness tests, whose whole point is what arrives - and the counters are
 * NOT reset, so a multi-packet write transaction can be judged as a whole. */
static void feed_5522_raw(Mock *mk, const char *meta, const char *raw_payload)
{
    char *seq = raw_payload ? dupprintf("\033]5522;%s;%s\033\\", meta, raw_payload)
                            : dupprintf("\033]5522;%s\033\\", meta);
    term_data(mk->term, seq, strlen(seq));
    term_update(mk->term);
    sfree(seq);
}

static void counters_reset(void)
{
    osc52_gets = 0;
    osc52_sends = 0;
    osc52_all_len = 0;
    osc52_all[0] = '\0';
    osc52_dialogs = 0;
    osc52_last_send[0] = '\0';
    osc52_set_calls = 0;
    osc52_set_nformats = 0;
    osc52_set_mimes[0] = '\0';
    osc52_set_text[0] = '\0';
    osc52_set_text_len = 0;
}

/* Did any reply contain this substring? Only the LAST is captured, so this is
 * used for the final packet of a transaction. */
static void expect_last(Mock *mk, const char *what, const char *want)
{
    if (!strstr(osc52_last_send, want)) {
        printf("   last reply was: %s\n", osc52_last_send);
        fail(what, "the last reply did not contain what it should have");
    }
}

static void test_osc5522(Mock *mk)
{
    /*
     * The whole reason OSC 5522 is worth having: it can be REFUSED OUT LOUD. OSC
     * 52 must stay silent because any reply confirms the feature exists, but this
     * protocol has real error codes, so a well-behaved program learns to stop
     * asking instead of retrying for ever.
     */
    read_reset(mk);
    conf_set_int(mk->term->conf, CONF_osc52_clipboard_read, OSC52_READ_DENY);
    feed_5522(mk, "type=read", "text/plain");
    expect_last(mk, "5522 read when set to Deny", "status=EPERM");
    if (osc52_dialogs != 0)
        fail("5522 read when set to Deny", "it asked the user anyway");

    /* No focus, nothing happens - EPERM, and still no prompt. */
    read_reset(mk);
    mk->term->has_focus = false;
    feed_5522(mk, "type=read", "text/plain");
    expect_last(mk, "5522 read with no focus", "status=EPERM");
    if (osc52_dialogs != 0)
        fail("5522 read with no focus", "it asked the user anyway");

    /* The ordinary allowed case: OK, one DATA packet, DONE. */
    read_reset(mk);
    osc52_dialog_answer = true;
    osc52_dialog_grant = GRANT_ONCE;
    feed_5522(mk, "type=read", "text/plain");
    if (osc52_dialogs != 1)
        fail("5522 read, allowed", "the user was not asked");
    if (osc52_sends != 3)
        fail("5522 read, allowed", "expected exactly OK, one DATA, then DONE");
    expect_last(mk, "5522 read, allowed", "type=read:status=DONE");

    /* A type we cannot produce is NOT an error: OK then DONE, no data. That is
     * what kitty does, and an error would describe it worse. */
    read_reset(mk);
    feed_5522(mk, "type=read", "image/png");
    if (osc52_sends != 2)
        fail("5522 read, unavailable type", "expected OK then DONE and nothing else");
    if (osc52_dialogs != 0)
        fail("5522 read, unavailable type",
             "the user was asked about a type we cannot supply");
    expect_last(mk, "5522 read, unavailable type", "status=DONE");

    /* Listing the available types must NOT prompt - the spec requires that, so an
     * application is not asked twice for one paste. */
    read_reset(mk);
    feed_5522(mk, "type=read", ".");
    if (osc52_dialogs != 0)
        fail("5522 type list", "listing the available types asked the user");
    if (osc52_sends != 3)
        fail("5522 type list", "expected OK, the list, then DONE");
    expect_last(mk, "5522 type list", "status=DONE");

    /* ...but it is still refused when reads are Deny, because the answer says
     * whether there is text on the clipboard. */
    read_reset(mk);
    conf_set_int(mk->term->conf, CONF_osc52_clipboard_read, OSC52_READ_DENY);
    feed_5522(mk, "type=read", ".");
    expect_last(mk, "5522 type list when set to Deny", "status=EPERM");

    /* primary selection does not exist on Windows: ENOSYS rather than quietly
     * serving the clipboard, which would be a worse answer than a refusal. */
    read_reset(mk);
    feed_5522(mk, "type=read:loc=primary", "text/plain");
    expect_last(mk, "5522 primary selection", "status=ENOSYS");

    /* The write direction has its own tests: test_osc5522_write(). */

    /* A request with no type= gets NO reply. Every reply must carry a type=, so
     * answering one whose type we could not read would mean inventing it. */
    read_reset(mk);
    feed_5522(mk, "nonsense=1", NULL);
    if (osc52_sends != 0)
        fail("5522 no type key", "an unidentifiable request was answered anyway");

    /* A type we do not implement is ENOSYS, against the type the host named. */
    read_reset(mk);
    feed_5522(mk, "type=frobnicate", NULL);
    expect_last(mk, "5522 unknown type", "type=frobnicate:status=ENOSYS");

    /* id is echoed back so a multiplexer can match replies to requests. */
    read_reset(mk);
    feed_5522(mk, "type=read:id=ab-1_2+x.y", "image/png");
    expect_last(mk, "5522 id echoed", "id=ab-1_2+x.y");

    /* An id outside the character set the spec allows is NOT echoed: whatever we
     * echo ends up inside sequences we generate, so it has to be known-safe.
     * (A ';' cannot be tested here - it separates metadata from payload, so it
     * never reaches the id in the first place.) */
    read_reset(mk);
    feed_5522(mk, "type=read:id=ba*d", "image/png");
    if (strstr(osc52_last_send, "ba*d"))
        fail("5522 bad id", "an id with illegal characters was echoed back");

    /*
     * The fatigue fix, and the one thing OSC 52 cannot do. A program that was
     * approved once, by password AND name, is not asked again.
     */
    read_reset(mk);
    osc52_dialog_answer = true;
    osc52_dialog_grant = GRANT_SESSION;
    feed_5522(mk, "type=read:pw=cHc=:name=bnZpbQ==", "text/plain");
    if (osc52_dialogs != 1)
        fail("5522 approve by password", "the first request did not ask");
    feed_5522(mk, "type=read:pw=cHc=:name=bnZpbQ==", "text/plain");
    if (osc52_dialogs != 0)
        fail("5522 approve by password", "the SAME program was asked again");
    if (osc52_sends != 3)
        fail("5522 approve by password", "the approved program was not served");

    /* The same password under a different name is not the same program. The
     * password alone would let anything reuse a token it overheard. */
    feed_5522(mk, "type=read:pw=cHc=:name=ZXZpbA==", "text/plain");
    if (osc52_dialogs != 1)
        fail("5522 approval is per name too",
             "a different name reused the approval without being asked");

    /* And a one-off answer is never remembered, whatever the program calls
     * itself. */
    read_reset(mk);
    osc52_dialog_answer = true;
    osc52_dialog_grant = GRANT_ONCE;
    feed_5522(mk, "type=read:pw=cHc=:name=bnZpbQ==", "text/plain");
    feed_5522(mk, "type=read:pw=cHc=:name=bnZpbQ==", "text/plain");
    if (osc52_dialogs != 1)
        fail("5522 one-off answer", "a one-off answer was remembered as an approval");

    /*
     * --- strict base64 on reads ---
     *
     * The specification's "Encoding of payloads" section (added upstream 2026-09):
     * standard alphabet, padding REQUIRED, and a terminal must not silently drop
     * invalid characters - line breaks included. An invalid READ is simply
     * ignored: no reply, no prompt. These replace the earlier tolerance tests,
     * which pinned the opposite (unpadded accepted) when kitty's decoder was the
     * only guide; the spec has since said which of the two it means.
     */
    read_reset(mk);
    osc52_dialog_answer = true;
    osc52_dialog_grant = GRANT_ONCE;

    /* unpadded type list ("text/plain" is dGV4dC9wbGFpbg==): ignored */
    counters_reset();
    feed_5522_raw(mk, "type=read", "dGV4dC9wbGFpbg");
    if (osc52_sends != 0 || osc52_dialogs != 0)
        fail("5522 unpadded read", "an unpadded type list was not ignored");

    /* characters outside the alphabet: ignored, and nobody asked */
    counters_reset();
    feed_5522_raw(mk, "type=read", "!!!not-base64!!!");
    if (osc52_sends != 0 || osc52_dialogs != 0)
        fail("5522 malformed payload", "a malformed request was answered or prompted");

    /* a line break inside the payload: the parser keeps it for 5522 (PuTTY
     * aborts every other OSC on CR/LF) so the validator can refuse it */
    counters_reset();
    feed_5522_raw(mk, "type=read", "dGV4dC9w\nbGFpbg==");
    if (osc52_sends != 0 || osc52_dialogs != 0)
        fail("5522 newline in payload", "a payload with a line break was not ignored");
    /* ...and the sequence was consumed whole: the next request works normally */
    counters_reset();
    feed_5522(mk, "type=read", "image/png");
    if (osc52_sends != 2)
        fail("5522 newline in payload", "the terminal did not recover after it");

    /* an unpadded PASSWORD is not a password we compare: ignored. (The earlier
     * padding-stripping compare is gone with it - two spellings can no longer
     * both arrive, so there is nothing to normalise.) */
    counters_reset();
    feed_5522_raw(mk, "type=read:pw=cHc:name=bnZpbQ==", "dGV4dC9wbGFpbg==");
    if (osc52_sends != 0 || osc52_dialogs != 0)
        fail("5522 unpadded password", "a request with an unpadded password was not ignored");

    /* padding alone is not base64 either */
    counters_reset();
    feed_5522_raw(mk, "type=read:pw==:name=bnZpbQ==", "dGV4dC9wbGFpbg==");
    if (osc52_sends != 0 || osc52_dialogs != 0)
        fail("5522 empty password", "an all-padding password was not ignored");

    /* an invalid name: ignored */
    counters_reset();
    feed_5522_raw(mk, "type=read:pw=cHc=:name=bnZpb!==", "dGV4dC9wbGFpbg==");
    if (osc52_sends != 0 || osc52_dialogs != 0)
        fail("5522 invalid name", "a request with an invalid name was not ignored");

    /* and the padded spelling, twice, is still the same program (approval by
     * password is tested above; this pins that strictness did not break it) */
    read_reset(mk);
    osc52_dialog_answer = true;
    osc52_dialog_grant = GRANT_SESSION;
    feed_5522(mk, "type=read:pw=cHc=:name=bnZpbQ==", "text/plain");
    feed_5522(mk, "type=read:pw=cHc=:name=bnZpbQ==", "text/plain");
    if (osc52_dialogs != 0 || osc52_sends != 3)
        fail("5522 strict password compare", "the same padded password was not recognised");

    /* A large clipboard is chunked at the size the specification requires, so the
     * transaction is OK + ceil(n/4096) DATA packets + DONE. */
    read_reset(mk);
    {
        static wchar_t big[10000];
        int i;
        for (i = 0; i < 9999; i++)
            big[i] = L'x';
        big[9999] = L'\0';
        stub_clip = big;
        osc52_dialog_answer = true;
        osc52_dialog_grant = GRANT_ONCE;
        feed_5522(mk, "type=read", "text/plain");
        /* 9999 bytes of UTF-8 -> 3 chunks at 4096 -> OK + 3 DATA + DONE */
        if (osc52_sends != 5)
            fail("5522 chunking", "a 9999-character clipboard was not sent in 3 chunks");
        expect_last(mk, "5522 chunking", "status=DONE");
    }
    stub_clip = L"secret";

    /*
     * --- images: the read direction serves image/png ---
     * A PNG on the clipboard goes out as it is; the type list names it; text
     * and image in one request go out one after the other; a text request
     * against an image-only clipboard is OK+DONE without a prompt.
     */
    {
        static const unsigned char png[] = {
            0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a, 1, 2, 3, 4, 5, 6 };
        char *pngb64 = b64_bytes(png, sizeof(png));
        stub_png = png;
        stub_png_len = sizeof(png);

        read_reset(mk);
        osc52_dialog_answer = true;
        osc52_dialog_grant = GRANT_ONCE;
        feed_5522(mk, "type=read", "image/png");
        if (osc52_dialogs != 1)
            fail("5522 image read", "the user was not asked");
        if (osc52_sends != 3)
            fail("5522 image read", "expected OK, one DATA, DONE");
        if (!strstr(osc52_all, "status=DATA:mime=aW1hZ2UvcG5n;"))
            fail("5522 image read", "the DATA packet did not carry mime=image/png");
        if (!strstr(osc52_all, pngb64))
            fail("5522 image read", "the DATA packet did not carry the PNG bytes");

        /* the type list names the image */
        read_reset(mk);
        feed_5522(mk, "type=read", ".");
        {
            char *want = b64("text/plain text/plain;charset=utf-8 image/png\n");
            if (!strstr(osc52_all, want))
                fail("5522 type list with image", "image/png missing from the type list");
            sfree(want);
        }

        /* text and image in one request: OK, DATA text, DATA image, DONE */
        read_reset(mk);
        osc52_dialog_answer = true;
        osc52_dialog_grant = GRANT_ONCE;
        feed_5522(mk, "type=read", "text/plain image/png");
        if (osc52_sends != 4)
            fail("5522 text+image read", "expected OK, DATA text, DATA image, DONE");
        if (osc52_dialogs != 1)
            fail("5522 text+image read", "one request must be one prompt");

        /* image-only clipboard: a text request is OK+DONE without a prompt, an
         * image request is served */
        read_reset(mk);              /* read_reset restores stub_clip: clear it AFTER */
        stub_clip = NULL;
        feed_5522(mk, "type=read", "text/plain");
        if (osc52_sends != 2 || osc52_dialogs != 0)
            fail("5522 text read, image-only clipboard", "expected OK+DONE and no prompt");
        feed_5522(mk, "type=read", "image/png");
        if (osc52_sends != 3 || osc52_dialogs != 1)
            fail("5522 image read, image-only clipboard", "the image was not served");
        /* the type list then names only the image */
        feed_5522(mk, "type=read", ".");
        {
            char *want = b64("image/png\n");
            if (!strstr(osc52_all, want))
                fail("5522 type list, image only", "expected exactly image/png");
            sfree(want);
        }
        /* and OSC 52, which carries text only, sends nothing for an image */
        read_reset(mk);
        stub_clip = NULL;
        osc52_dialog_answer = true;
        {
            const char *seq = "\033]52;c;?\007";
            counters_reset();
            term_data(mk->term, seq, strlen(seq));
            term_update(mk->term);
            if (osc52_sends != 0 || osc52_dialogs != 0)
                fail("OSC 52 read, image-only clipboard", "text-only protocol acted on an image");
        }
        stub_png = NULL;
        stub_png_len = 0;
        stub_clip = L"secret";
        sfree(pngb64);
    }
}

/* Pull pw=<token> out of the captured replies. Returns a fresh string or NULL. */
static char *captured_pw(void)
{
    const char *p = strstr(osc52_all, ":pw=");
    const char *e;
    if (!p)
        return NULL;
    p += 4;
    e = p;
    while (*e && *e != ':' && *e != ';' && *e != '\033')
        e++;
    return dupprintf("%.*s", (int)(e - p), p);
}

/*
 * OSC 5522 paste events (private mode 5522): DECRQM, the event on Paste, the
 * one-time token, the auto-disarm, the resets.
 */
static void test_osc5522_paste_events(Mock *mk)
{
    static const unsigned char png[] = {
        0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a, 9, 9, 9, 9 };
    char *pw, *meta;
    Terminal *term = mk->term;

    read_reset(mk);
    stub_clip = L"pasted text";
    stub_png = png;
    stub_png_len = sizeof(png);
    conf_set_int(term->conf, CONF_osc5522_paste_minutes, 30);

    /* DECRQM while off: reset (2) */
    counters_reset();
    term_data(term, "\033[?5522$p", 9);
    term_update(term);
    expect_last(mk, "DECRQM 5522 off", "\033[?5522;2$y");
    if (osc52_sends != 1)
        fail("DECRQM 5522 off", "expected exactly one reply");

    /* DECRQM for another mode stays silent, as before */
    counters_reset();
    term_data(term, "\033[?2004$p", 9);
    term_update(term);
    if (osc52_sends != 0)
        fail("DECRQM other mode", "a mode this build does not report was answered");

    /* set the mode: DECRQM says set (1), the marker function agrees */
    counters_reset();
    term_data(term, "\033[?5522h", 8);
    term_update(term);
    if (!term_osc5522_paste_events(term))
        fail("mode 5522 set", "CSI ? 5522 h did not arm paste events");
    if (!term->osc5522_paste_tokens_until || !term_osc5522_paste_tokens(term))
        fail("mode 5522 set", "no token clock was armed with it");
    counters_reset();
    term_data(term, "\033[?5522$p", 9);
    term_update(term);
    expect_last(mk, "DECRQM 5522 on", "\033[?5522;1$y");

    /* Paste: no text goes to the host, the event goes instead - OK with a token,
     * the type list naming text and image, DONE */
    counters_reset();
    mock_paste_requests = 0;
    term_request_paste(term, CLIP_SYSTEM);
    if (mock_paste_requests != 0)
        fail("paste event", "Paste still pasted");
    if (osc52_sends != 3)
        fail("paste event", "expected OK, the type list, DONE");
    if (!strstr(osc52_all, "type=read:status=OK:pw="))
        fail("paste event", "the OK packet carries no token");
    if (!strstr(osc52_all, "status=DATA:mime=Lg==:pw="))
        fail("paste event", "the DATA packet is not the '.' list with the token");
    {
        char *want = b64("text/plain text/plain;charset=utf-8 image/png\n");
        if (!strstr(osc52_all, want))
            fail("paste event", "the type list does not name text and image");
        sfree(want);
    }
    pw = captured_pw();
    if (!pw || !*pw)
        fail("paste event", "no token could be read back");

    /* the application reads the image with the token: served, NO dialog */
    meta = dupprintf("type=read:pw=%s:name=UGFzdGUgZXZlbnQ=", pw ? pw : "");
    feed_5522(mk, meta, "image/png");
    if (osc52_dialogs != 0)
        fail("token read", "a paste-event token still raised the dialog");
    if (osc52_sends != 3 || !strstr(osc52_all, "mime=aW1hZ2UvcG5n"))
        fail("token read", "the image was not served on the token");

    /* the same token again: single use, so this one asks */
    osc52_dialog_answer = false;
    feed_5522(mk, meta, "image/png");
    if (osc52_dialogs != 1)
        fail("token reuse", "a used token was accepted a second time");
    expect_last(mk, "token reuse", "status=EPERM");
    sfree(meta);
    sfree(pw);

    /* the wrong name with the right token is no token */
    counters_reset();
    term_request_paste(term, CLIP_SYSTEM);
    pw = captured_pw();
    meta = dupprintf("type=read:pw=%s:name=ZXZpbA==", pw);   /* "evil" */
    osc52_dialog_answer = false;
    feed_5522(mk, meta, "image/png");
    if (osc52_dialogs != 1)
        fail("token with another name", "the token worked under a different name");
    sfree(meta);
    sfree(pw);

    /* an expired token asks */
    counters_reset();
    term_request_paste(term, CLIP_SYSTEM);
    pw = captured_pw();
    term->osc5522_paste_pw_until = (unsigned long)time(NULL) - 1;
    meta = dupprintf("type=read:pw=%s:name=UGFzdGUgZXZlbnQ=", pw);
    osc52_dialog_answer = false;
    feed_5522(mk, meta, "image/png");
    if (osc52_dialogs != 1)
        fail("expired token", "an expired token was still accepted");
    sfree(meta);
    sfree(pw);

    /* two events, two different tokens */
    counters_reset();
    term_request_paste(term, CLIP_SYSTEM);
    pw = captured_pw();
    counters_reset();
    term_request_paste(term, CLIP_SYSTEM);
    meta = captured_pw();
    if (!pw || !meta || !strcmp(pw, meta))
        fail("token uniqueness", "two paste events carried the same token");
    sfree(pw);
    sfree(meta);

    /* reads set to Deny: no event, an ordinary paste instead */
    conf_set_int(term->conf, CONF_osc52_clipboard_read, OSC52_READ_DENY);
    counters_reset();
    mock_paste_requests = 0;
    term_request_paste(term, CLIP_SYSTEM);
    if (osc52_sends != 0 || mock_paste_requests != 1)
        fail("paste events under Deny", "the event was sent, or the paste did not happen");
    conf_set_int(term->conf, CONF_osc52_clipboard_read, OSC52_READ_ASK);

    /* the clock runs out: the MODE stays (DECRQM still says set), a paste still
     * sends the event but WITHOUT a token, and the application's read then
     * meets the dialog */
    term->osc5522_paste_tokens_until = (unsigned long)time(NULL) - 1;
    if (term_osc5522_paste_tokens(term))
        fail("token clock", "tokens survived their deadline");
    if (!term_osc5522_paste_events(term))
        fail("token clock", "the deadline switched the mode off - the application would never recover");
    counters_reset();
    term_data(term, "\033[?5522$p", 9);
    term_update(term);
    expect_last(mk, "token clock", "\033[?5522;1$y");
    counters_reset();
    mock_paste_requests = 0;
    term_request_paste(term, CLIP_SYSTEM);
    if (osc52_sends != 3 || mock_paste_requests != 0)
        fail("token clock", "a paste after the deadline did not send the event");
    if (strstr(osc52_all, "pw="))
        fail("token clock", "a paste after the deadline still carried a token");
    osc52_dialog_answer = true;
    osc52_dialog_grant = GRANT_ONCE;
    /* three dialogs were shown above within ten seconds, which is the prompt
     * ration; this one must not be refused for that reason */
    term->osc52_read_prompts = 0;
    term->osc52_read_prompt_window = 0;
    feed_5522(mk, "type=read:name=UGFzdGUgZXZlbnQ=", "image/png");
    if (osc52_dialogs != 1 || osc52_sends != 3)
        fail("token clock", "the read after the deadline was not put to the user, or not served on yes");
    /* setting the mode again restarts the clock */
    term_data(term, "\033[?5522h", 8);
    term_update(term);
    if (!term_osc5522_paste_tokens(term))
        fail("token clock", "re-setting the mode did not restart the clock");

    /* 0 minutes = tokens always */
    conf_set_int(term->conf, CONF_osc5522_paste_minutes, 0);
    term_data(term, "\033[?5522h", 8);
    term_update(term);
    if (!term->osc5522_paste_events || term->osc5522_paste_tokens_until != 0 ||
        !term_osc5522_paste_tokens(term))
        fail("no deadline", "0 minutes armed a clock anyway");

    /* the application clears it */
    term_data(term, "\033[?5522l", 8);
    term_update(term);
    if (term->osc5522_paste_events)
        fail("mode 5522 reset", "CSI ? 5522 l did not clear paste events");

    /* a terminal reset clears it too, and its token */
    term_data(term, "\033[?5522h", 8);
    term_update(term);
    term_request_paste(term, CLIP_SYSTEM);
    if (!term->osc5522_paste_pw)
        fail("reset clears mode", "no token to clear (test setup)");
    term_pwron(term, true);
    if (term->osc5522_paste_events || term->osc5522_paste_pw)
        fail("reset clears mode", "RIS left paste events or a token behind");

    conf_set_int(term->conf, CONF_osc5522_paste_minutes, 30);
    stub_png = NULL;
    stub_png_len = 0;
    stub_clip = L"secret";
}

/* One OSC 5522 write of the given raw base64 chunks for text/plain, judged by its
 * single reply. Mirrors kitty's `t(*payloads, expected_status)`. */
static void w_chunks(Mock *mk, const char *what, const char *const *payloads,
                     int n, const char *expected)
{
    int i;
    char want[64];
    counters_reset();
    feed_5522_raw(mk, "type=write:id=w1", NULL);
    for (i = 0; i < n; i++)
        feed_5522_raw(mk, "type=wdata:mime=dGV4dC9wbGFpbg==", payloads[i]);
    feed_5522_raw(mk, "type=wdata", NULL);
    if (osc52_sends != 1)
        fail(what, "expected exactly one reply for the whole transaction");
    snprintf(want, sizeof(want), "type=write:status=%s:id=w1", expected);
    expect_last(mk, what, want);
    if (mk->term->osc5522_w_active)
        fail(what, "the transaction was left open after its reply");
}

/* A write with one good chunk in, then one malformed packet: EINVAL against the
 * write's id, and everything after it ignored. Mirrors kitty's
 * test_clipboard_malformed_write_packets. */
static void w_malformed(Mock *mk, const char *what, const char *meta,
                        const char *raw_payload)
{
    counters_reset();
    feed_5522_raw(mk, "type=write:id=w1", NULL);
    feed_5522_raw(mk, "type=wdata:mime=dGV4dC9wbGFpbg==", "eHh4");     /* xxx */
    counters_reset();
    feed_5522_raw(mk, meta, raw_payload);
    if (osc52_sends != 1)
        fail(what, "expected exactly one reply to the malformed packet");
    expect_last(mk, what, "type=write:status=EINVAL:id=w1");
    if (mk->term->osc5522_w_active)
        fail(what, "the transaction survived a malformed packet");
    counters_reset();
    feed_5522_raw(mk, "type=wdata:mime=dGV4dC9wbGFpbg==", "eHh4");
    feed_5522_raw(mk, "type=wdata", NULL);
    if (osc52_sends != 0 || osc52_set_calls != 0)
        fail(what, "packets of the aborted write were not ignored");
}

/*
 * The OSC 5522 WRITE direction. The protocol cases mirror kitty's own tests
 * (kitty_tests/clipboard.py: write_too_much_data, invalid_base64,
 * malformed_write_packets); the gate cases are ours.
 */
static void test_osc5522_write(Mock *mk)
{
    static const char *const M = "type=wdata:mime=dGV4dC9wbGFpbg==";  /* text/plain */

    read_reset(mk);
    mk->term->osc52_allowed = OSC52_CLIPBOARD_ALLOW;
    conf_set_int(mk->term->conf, CONF_clipboard_writes_per_sec, 0);
    mk->term->has_focus = true;
    mk->term->osc5522_w_limit_override = 0;
    osc52_set_fail = false;

    /* The ordinary write: silent until the end, then ONE reply - DONE, status
     * before id as kitty lays it out - and the clipboard set exactly once. */
    counters_reset();
    feed_5522_raw(mk, "type=write:id=w1", NULL);
    feed_5522_raw(mk, M, "SGVsbG8=");                                /* Hello */
    if (osc52_sends != 0)
        fail("5522 write", "a write in progress was answered before its end");
    feed_5522_raw(mk, "type=wdata", NULL);
    if (osc52_sends != 1)
        fail("5522 write", "expected exactly one reply, DONE");
    expect_last(mk, "5522 write", "type=write:status=DONE:id=w1");
    if (osc52_set_calls != 1 || osc52_set_nformats != 1)
        fail("5522 write", "the clipboard was not set exactly once with one format");
    if (strcmp(osc52_set_text, "Hello") || strcmp(osc52_set_mimes, "text/plain"))
        fail("5522 write", "what landed was not the payload under its type");

    /* Two types and an alias: all three reach the clipboard in ONE set, the alias
     * carrying its target's bytes; no id sent, no id echoed. */
    counters_reset();
    feed_5522_raw(mk, "type=write", NULL);
    feed_5522_raw(mk, M, "SGVsbG8=");
    feed_5522_raw(mk, "type=wdata:mime=aW1hZ2UvcG5n", "iVBORw0KGgo=");  /* image/png */
    feed_5522_raw(mk, "type=walias:mime=dGV4dC9wbGFpbg==", "dGV4dC9ydGY=");  /* text/rtf -> text/plain */
    feed_5522_raw(mk, "type=wdata", NULL);
    expect_last(mk, "5522 write, several types", "type=write:status=DONE");
    if (strstr(osc52_last_send, "id="))
        fail("5522 write, several types", "an id was echoed that was never sent");
    if (osc52_set_calls != 1)
        fail("5522 write, several types", "the formats did not reach the clipboard in one set");
    if (strcmp(osc52_set_mimes, "text/plain,image/png,text/rtf")) {
        printf("   formats set: %s\n", osc52_set_mimes);
        fail("5522 write, several types", "the types and the alias did not all land");
    }

    /* Odd chunk boundaries are fine: padding is judged on the concatenation.
     * "some data" is c29tZSBkYXRh, split as kitty splits it. */
    {
        static const char *const three[] = { "c29", "tZSB", "kYXRh" };
        static const char *const one[] = { "c", "2", "9", "t", "Z", "S", "B", "k", "Y", "X", "R", "h" };
        w_chunks(mk, "5522 odd chunk boundaries", three, 3, "DONE");
        if (strcmp(osc52_set_text, "some data"))
            fail("5522 odd chunk boundaries", "the chunks did not reassemble");
        w_chunks(mk, "5522 one character per chunk", one, 12, "DONE");
        if (strcmp(osc52_set_text, "some data"))
            fail("5522 one character per chunk", "the chunks did not reassemble");
    }

    /* --- kitty: invalid base64 is REJECTED, not ignored --- */
    {
        static const char *const a[] = { "!!!" };
        static const char *const b[] = { "SGVs!!!bG8=" };
        static const char *const c[] = { "Z29vZA==", "SGVs!!!bG8=" };       /* good, then bad */
        static const char *const d[] = { "\nZGF0YSB3aXRoIGEgbmV3bGluZQ==" }; /* a line break */
        static const char *const e[] = { "SGVsbG8" };                      /* unpadded */
        static const char *const f[] = { "Z29vZA==", "SGVsbG8" };
        static const char *const g[] = { "SGVsbG8=", "QQ==" };             /* padded chunk, then more */
        w_chunks(mk, "5522 invalid characters", a, 1, "EINVAL");
        w_chunks(mk, "5522 invalid characters mid-chunk", b, 1, "EINVAL");
        w_chunks(mk, "5522 invalid characters in a later chunk", c, 2, "EINVAL");
        w_chunks(mk, "5522 line break in a chunk", d, 1, "EINVAL");
        w_chunks(mk, "5522 unpadded data", e, 1, "EINVAL");
        w_chunks(mk, "5522 unpadded final chunk", f, 2, "EINVAL");
        if (osc52_set_calls != 0)
            fail("5522 invalid base64", "a rejected write reached the clipboard anyway");
        /* a chunk padded on its own, then more data for the same type, is
         * accepted - kitty's own EFBIG test sends that shape, and clients built
         * against a terminal that pads every read chunk pad their writes too */
        w_chunks(mk, "5522 padded chunk then more", g, 2, "DONE");
        if (strcmp(osc52_set_text, "HelloA"))
            fail("5522 padded chunk then more", "the two padded chunks did not concatenate");
    }

    /* invalid base64 in a METADATA value */
    counters_reset();
    feed_5522_raw(mk, "type=write:id=w2", NULL);
    feed_5522_raw(mk, "type=wdata:mime=dGV4dC9wbGFpbg=!", "eHh4");
    if (osc52_sends != 1)
        fail("5522 invalid metadata base64", "expected exactly one reply");
    expect_last(mk, "5522 invalid metadata base64", "type=write:status=EINVAL:id=w2");
    if (mk->term->osc5522_w_active)
        fail("5522 invalid metadata base64", "the transaction was left open");

    /* --- kitty: malformed write packets --- */
    w_malformed(mk, "5522 alias not base64", "type=walias:mime=dGV4dC9wbGFpbg==", "AAA");
    w_malformed(mk, "5522 alias not UTF-8", "type=walias:mime=dGV4dC9wbGFpbg==", "/w==");
    w_malformed(mk, "5522 alias without a type", "type=walias", "dGV4dC9ydGY=");
    w_malformed(mk, "5522 alias type not base64", "type=walias:mime=AAA", "dGV4dC9ydGY=");
    w_malformed(mk, "5522 data type not base64", "type=wdata:mime=AAA", "eHh4");
    w_malformed(mk, "5522 data without a type", "type=wdata", "eHh4");

    /* a malformed READ must not abort the write, and gets no reply itself */
    counters_reset();
    feed_5522_raw(mk, "type=write:id=w2", NULL);
    feed_5522_raw(mk, "type=read:id=r1", "AAA");
    if (osc52_sends != 0)
        fail("5522 malformed read during a write", "the malformed read was answered");
    if (!mk->term->osc5522_w_active)
        fail("5522 malformed read during a write", "the write was aborted by it");
    feed_5522_raw(mk, "type=wdata", NULL);
    expect_last(mk, "5522 malformed read during a write", "type=write:status=DONE:id=w2");

    /* --- kitty: too much data is EFBIG, incrementally, and the rest ignored --- */
    mk->term->osc5522_w_limit_override = 16;
    counters_reset();
    feed_5522_raw(mk, "type=write", NULL);
    feed_5522_raw(mk, M, "YWFhYWFhYWFhYWFhYWFhYQ==");                  /* 16 x a */
    if (osc52_sends != 0 || !mk->term->osc5522_w_active)
        fail("5522 EFBIG", "a write within the limit was answered or dropped");
    feed_5522_raw(mk, M, "YWFhYQ==");                                  /* 4 more */
    if (osc52_sends != 1)
        fail("5522 EFBIG", "expected exactly one reply at the moment the limit passed");
    expect_last(mk, "5522 EFBIG", "type=write:status=EFBIG");
    if (mk->term->osc5522_w_active)
        fail("5522 EFBIG", "the transaction survived EFBIG");
    counters_reset();
    feed_5522_raw(mk, M, "YWFhYQ==");
    feed_5522_raw(mk, "type=wdata", NULL);
    if (osc52_sends != 0 || osc52_set_calls != 0)
        fail("5522 EFBIG", "packets of the aborted write were not ignored");
    mk->term->osc5522_w_limit_override = 0;

    /* The 64 MB floor: ClipboardMaxMB at its smallest (1 MB) must NOT refuse a
     * 5522 write above 1 MB - the spec makes 64 MB the least a terminal may
     * accept. Just over 1 MB of 'A's, in 4 KB chunks as the protocol prescribes.
     * (The setting still bounds OSC 52 exactly as before: test_far2l_ceiling and
     * the 256 KB case above cover that.) */
    conf_set_int(mk->term->conf, CONF_clipboard_max_mb, 1);
    {
        const size_t total_quads = 349526;        /* 1048578 bytes > 1 MB */
        const size_t per_packet = 1365;           /* 4095 bytes a chunk */
        char *chunk = snewn(per_packet * 4 + 1, char);
        size_t sent = 0;
        counters_reset();
        feed_5522_raw(mk, "type=write:id=big", NULL);
        while (sent < total_quads) {
            size_t n = total_quads - sent;
            size_t i;
            if (n > per_packet)
                n = per_packet;
            for (i = 0; i < n; i++)
                memcpy(chunk + i * 4, "QUFB", 4);
            chunk[n * 4] = '\0';
            feed_5522_raw(mk, M, chunk);
            sent += n;
        }
        feed_5522_raw(mk, "type=wdata", NULL);
        sfree(chunk);
        if (osc52_sends != 1)
            fail("5522 64 MB floor", "expected exactly one reply");
        expect_last(mk, "5522 64 MB floor", "type=write:status=DONE:id=big");
        if (osc52_set_calls != 1 || osc52_set_first_len != total_quads * 3)
            fail("5522 64 MB floor", "a 1 MB write was refused or cut although the spec floor is 64 MB");
    }
    conf_set_int(mk->term->conf, CONF_clipboard_max_mb, 16);

    /* --- the gate: it is the OSC 52 write gate, answered out loud --- */

    /* Deny: EPERM at the START, and the rest of the transaction ignored */
    mk->term->osc52_allowed = OSC52_CLIPBOARD_DENY;
    counters_reset();
    feed_5522_raw(mk, "type=write:id=w3", NULL);
    if (osc52_sends != 1)
        fail("5522 write when set to Deny", "expected an immediate refusal");
    expect_last(mk, "5522 write when set to Deny", "type=write:status=EPERM:id=w3");
    counters_reset();
    feed_5522_raw(mk, M, "SGVsbG8=");
    feed_5522_raw(mk, "type=wdata", NULL);
    if (osc52_sends != 0 || osc52_set_calls != 0)
        fail("5522 write when set to Deny", "a refused write's packets were not ignored");
    mk->term->osc52_allowed = OSC52_CLIPBOARD_ALLOW;

    /* no focus: EPERM */
    mk->term->has_focus = false;
    counters_reset();
    feed_5522_raw(mk, "type=write", NULL);
    expect_last(mk, "5522 write with no focus", "type=write:status=EPERM");
    mk->term->has_focus = true;

    /* the rate cap: the second write within the second is EBUSY */
    conf_set_int(mk->term->conf, CONF_clipboard_writes_per_sec, 1);
    mk->term->clip_write_second = 0;
    mk->term->clip_write_count = 0;
    counters_reset();
    feed_5522_raw(mk, "type=write:id=a", NULL);
    feed_5522_raw(mk, M, "SGVsbG8=");
    feed_5522_raw(mk, "type=wdata", NULL);
    expect_last(mk, "5522 write rate cap", "type=write:status=DONE:id=a");
    counters_reset();
    feed_5522_raw(mk, "type=write:id=b", NULL);
    expect_last(mk, "5522 write rate cap", "type=write:status=EBUSY:id=b");
    conf_set_int(mk->term->conf, CONF_clipboard_writes_per_sec, 0);

    /* primary selection: ENOSYS */
    counters_reset();
    feed_5522_raw(mk, "type=write:loc=primary:id=p", NULL);
    expect_last(mk, "5522 write to primary", "type=write:status=ENOSYS:id=p");

    /* a packet that does not fit the per-sequence ceiling is EINVAL */
    {
        char *huge = snewn(OSC_STR_MAX_5522 + 1024, char);
        memset(huge, 'A', OSC_STR_MAX_5522 + 1000);
        huge[OSC_STR_MAX_5522 + 1000] = '\0';
        counters_reset();
        feed_5522_raw(mk, "type=write:id=o", NULL);
        feed_5522_raw(mk, M, huge);
        expect_last(mk, "5522 oversize packet", "type=write:status=EINVAL:id=o");
        sfree(huge);
    }

    /* the clipboard could not be set: EIO, and the host is told */
    osc52_set_fail = true;
    counters_reset();
    feed_5522_raw(mk, "type=write:id=e", NULL);
    feed_5522_raw(mk, M, "SGVsbG8=");
    feed_5522_raw(mk, "type=wdata", NULL);
    expect_last(mk, "5522 clipboard failure", "type=write:status=EIO:id=e");
    osc52_set_fail = false;

    /* a write that carried nothing completes (DONE) and sets nothing - the same
     * rule OSC 52 has for an empty payload */
    counters_reset();
    feed_5522_raw(mk, "type=write:id=n", NULL);
    feed_5522_raw(mk, "type=wdata", NULL);
    expect_last(mk, "5522 empty write", "type=write:status=DONE:id=n");
    if (osc52_set_calls != 0)
        fail("5522 empty write", "an empty write touched the clipboard");

    /* a new type=write replaces one in flight, silently */
    counters_reset();
    feed_5522_raw(mk, "type=write:id=first", NULL);
    feed_5522_raw(mk, M, "SGVsbG8=");
    feed_5522_raw(mk, "type=write:id=second", NULL);
    if (osc52_sends != 0)
        fail("5522 write replaces write", "the replaced write was answered");
    feed_5522_raw(mk, "type=wdata", NULL);
    expect_last(mk, "5522 write replaces write", "type=write:status=DONE:id=second");
    if (osc52_set_calls != 0)
        fail("5522 write replaces write", "the replaced write's data was set anyway");
}

/*
 * The remote clipboard WRITE rate cap. This is the direction that is ON by
 * default, so it is the one an ordinary user is exposed to.
 */
static void test_clipboard_write_rate(Mock *mk)
{
    const char *seq = "\033]52;c;SGVsbG8=\007";
    int i, applied;

    conf_set_bool(mk->term->conf, CONF_clipboard_require_focus, true);
    mk->term->has_focus = true;
    mk->term->osc52_allowed = OSC52_CLIPBOARD_ALLOW;

    /*
     * FIRST, and most important: an ordinary clipboard write is SILENT. No
     * balloon, no Event Log line, nothing. A notifier that speaks up when a
     * feature is working normally gets switched off within a day, and then the
     * one message that mattered never arrives either.
     */
    conf_set_int(mk->term->conf, CONF_clipboard_writes_per_sec, 10);
    mk->term->clip_write_second = 0;
    mk->term->clip_write_count = 0;
    mk->term->clip_notified_last = 0;
    osc52_notifies = 0;
    mk->clip_writes = 0;
    term_data(mk->term, seq, strlen(seq));
    term_update(mk->term);
    if (mk->clip_writes != 1)
        fail("single clipboard write", "an ordinary write did not reach the clipboard");
    if (osc52_notifies != 0)
        fail("single clipboard write",
             "an ordinary write raised a notification");

    /* and a handful under the cap stays silent too - the cap must clear a
     * realistic burst without complaining about it */
    osc52_notifies = 0;
    mk->clip_writes = 0;
    for (i = 0; i < 5; i++)
        term_data(mk->term, seq, strlen(seq));
    term_update(mk->term);
    if (mk->clip_writes != 5)
        fail("burst under the cap", "a burst within the allowance was throttled");
    if (osc52_notifies != 0)
        fail("burst under the cap", "a burst within the allowance raised a notification");

    /* Five writes with a cap of three: three land, two are dropped. */
    conf_set_int(mk->term->conf, CONF_clipboard_writes_per_sec, 3);
    mk->term->clip_write_second = 0;
    mk->term->clip_write_count = 0;
    mk->term->clip_notified_last = 0;
    osc52_notifies = 0;
    mk->clip_writes = 0;
    for (i = 0; i < 5; i++)
        term_data(mk->term, seq, strlen(seq));
    term_update(mk->term);
    applied = mk->clip_writes;
    if (applied != 3)
        fail("clipboard write rate cap",
             "the cap did not limit a burst to exactly its allowance");
    /* ...and THAT is worth a word, exactly once however many were dropped, and it
     * has to be the notification that offers to block rather than one that merely
     * points at the log. */
    if (osc52_notifies != 1)
        fail("clipboard write rate cap",
             osc52_notifies == 0 ? "exceeding the cap said nothing at all"
                                 : "exceeding the cap raised one notice per drop");
    if (osc52_notify_action != CLIP_BALLOON_BLOCK_WRITES)
        fail("clipboard write rate cap",
             "the notice did not offer to block the server");

    /* The window moves on: a later second gets a fresh allowance. Simulated by
     * ageing the recorded second rather than sleeping through a test run. */
    mk->term->clip_write_second = 0;
    mk->term->clip_write_count = 0;
    mk->clip_writes = 0;
    term_data(mk->term, seq, strlen(seq));
    term_update(mk->term);
    if (mk->clip_writes != 1)
        fail("clipboard write rate cap",
             "a new second did not restore the allowance");

    /* Zero means no limit, for anyone who wants the old behaviour back. */
    conf_set_int(mk->term->conf, CONF_clipboard_writes_per_sec, 0);
    mk->term->clip_write_second = 0;
    mk->term->clip_write_count = 0;
    mk->clip_writes = 0;
    for (i = 0; i < 20; i++)
        term_data(mk->term, seq, strlen(seq));
    term_update(mk->term);
    if (mk->clip_writes != 20)
        fail("clipboard write rate cap", "zero did not mean unlimited");

    /*
     * A REFUSED write must not cost anyone their allowance - otherwise a host that
     * is already denied could exhaust the budget and stop a legitimate one landing.
     */
    conf_set_int(mk->term->conf, CONF_clipboard_writes_per_sec, 2);
    mk->term->clip_write_second = 0;
    mk->term->clip_write_count = 0;
    mk->term->has_focus = false;                /* so these are all refused */
    for (i = 0; i < 10; i++)
        term_data(mk->term, seq, strlen(seq));
    term_update(mk->term);
    mk->term->has_focus = true;
    mk->clip_writes = 0;
    for (i = 0; i < 2; i++)
        term_data(mk->term, seq, strlen(seq));
    term_update(mk->term);
    if (mk->clip_writes != 2)
        fail("clipboard write rate cap",
             "refused writes consumed the allowance for allowed ones");

    conf_set_int(mk->term->conf, CONF_clipboard_writes_per_sec, 10);
    mk->term->clip_write_second = 0;
    mk->term->clip_write_count = 0;
}

/* ---------------------------------------------------------------------------
 * far2l: the accumulation-buffer ceiling
 *
 * far2l clipboard payloads arrive as ONE APC sequence and share the OSC
 * accumulation buffer. That buffer's APC ceiling was OSC_STR_MAX (2 KB), so any
 * real copied selection was truncated by the parser and then dropped outright -
 * silently. These tests are about the CEILING, not about the clipboard: they
 * check how much the parser was willing to hold, which is where the bug was.
 * ------------------------------------------------------------------------- */

/* Feed an APC sequence and report how much of it the parser kept. clip_allowed is
 * forced to 0 (deny) so nothing touches the real Windows clipboard and no dialog
 * can appear - we are measuring the buffer, not the feature. */
static int feed_apc(Mock *mk, const char *body, size_t len)
{
    char *seq = snewn(len + 8, char);
    size_t n = 0;
    int kept;
    memcpy(seq + n, "\033_", 2); n += 2;
    memcpy(seq + n, body, len);   n += len;
    seq[n++] = '\007';
    term_data(mk->term, seq, n);
    /* read the parser's state BEFORE term_update, which starts the next sequence */
    kept = mk->term->osc_strlen;
    term_update(mk->term);
    sfree(seq);
    return kept;
}

static void test_far2l_ceiling(Mock *mk)
{
    const size_t big = 100000;         /* far past the old 2 KB ceiling */
    char *body = snewn(big + 16, char);
    int kept;

    /* deny, so nothing here can reach the real Windows clipboard or raise a
     * dialog: these cases are about the parser's ceiling, not the feature */
    mk->term->clip_allowed = 0;

    /* A far2l DATA payload is held whole. Before the fix this stopped at 2048. */
    memcpy(body, "far2l:", 6);
    memset(body + 6, 'A', big);
    kept = feed_apc(mk, body, big + 6);
    if (kept != (int)(big + 6))
        fail("far2l payload ceiling",
             "a large far2l payload was still truncated by the parser");
    if (mk->term->osc_str_overflow)
        fail("far2l payload ceiling", "a large far2l payload was marked overflowed");

    /*
     * ...but nothing else gets that headroom. An APC that is NOT a far2l data
     * payload still stops where it always did, so announcing far2l support - or
     * simply sending an APC - cannot buy a host a 64 MB buffer.
     */
    memcpy(body, "notfar2l:", 9);
    memset(body + 9, 'A', big);
    kept = feed_apc(mk, body, big + 9);
    if (kept != OSC_STR_MAX)
        fail("non-far2l APC ceiling",
             "an unrelated APC sequence was allowed past the ordinary ceiling");
    if (!mk->term->osc_str_overflow)
        fail("non-far2l APC ceiling", "the overflow flag was not set");

    /* The handshake is a few bytes and keeps the ordinary ceiling: only the
     * "far2l:" DATA prefix grows the buffer. */
    memcpy(body, "far2l1", 6);
    memset(body + 6, 'A', big);
    kept = feed_apc(mk, body, big + 6);
    if (kept != OSC_STR_MAX)
        fail("far2l handshake ceiling",
             "the handshake prefix bought a large buffer");

    /*
     * The ceiling is a setting, and it is CLAMPED. Lowering it must work - that is
     * the whole point of exposing it - and raising it beyond the cap must not,
     * because this bounds memory a remote host can make us hold with no user
     * interaction.
     */
    /*
     * The ceiling is a setting, and it is CLAMPED.
     *
     * These assert the LIMIT ITSELF rather than "a payload fitted", because those
     * are not the same claim: 100 KB fits under 1 MB, under 64 MB and under no
     * limit at all, so a fitting payload cannot tell an honoured setting from an
     * ignored one, nor a fallback-to-default from a fallback-to-unlimited - which
     * is the failure that actually matters here, since the unlimited reading is
     * remote memory exhaustion.
     */
    memcpy(body, "far2l:", 6);
    memset(body + 6, 'A', big);

    conf_set_int(mk->term->conf, CONF_clipboard_max_mb, 1);
    feed_apc(mk, body, 16);            /* short: we only want the limit chosen */
    if (mk->term->osc_str_limit != (size_t)1 * 1024 * 1024)
        fail("clipboard ceiling setting", "a 1 MB setting was not honoured");

    /* ...and lowering it actually bites. The smallest settable ceiling is 1 MB (the
     * unit is megabytes), so this needs a payload comfortably over that: 1.5 MB
     * must be truncated at exactly the ceiling and flagged as overflowed, which is
     * what makes the whole payload get refused rather than half-pasted. */
    {
        const size_t over = 1536 * 1024;
        char *big_body = snewn(over + 16, char);
        int got;
        memcpy(big_body, "far2l:", 6);
        memset(big_body + 6, 'A', over);
        conf_set_int(mk->term->conf, CONF_clipboard_max_mb, 1);
        got = feed_apc(mk, big_body, over + 6);
        if ((size_t)got != (size_t)1 * 1024 * 1024)
            fail("clipboard ceiling setting",
                 "a payload over a lowered ceiling was not cut at that ceiling");
        if (!mk->term->osc_str_overflow)
            fail("clipboard ceiling setting",
                 "a payload over the ceiling was not flagged as overflowed");
        sfree(big_body);
    }

    /* Zero is nonsense, and must fall back to the DEFAULT - not to "no limit",
     * which is the reading whose failure mode is memory exhaustion. */
    conf_set_int(mk->term->conf, CONF_clipboard_max_mb, 0);
    feed_apc(mk, body, 16);
    if (mk->term->osc_str_limit != (size_t)CLIP_MAX_MB_DEFAULT * 1024 * 1024)
        fail("clipboard ceiling setting",
             "a zero setting did not fall back to exactly the default");

    /* A huge number is clamped, not honoured. Checked on the limit rather than by
     * actually feeding gigabytes. */
    conf_set_int(mk->term->conf, CONF_clipboard_max_mb, 100000);
    feed_apc(mk, body, 16);
    if (mk->term->osc_str_limit != (size_t)CLIP_MAX_MB_CAP * 1024 * 1024)
        fail("clipboard ceiling clamp",
             "a huge ClipboardMaxMB was not clamped to exactly the cap");

    conf_set_int(mk->term->conf, CONF_clipboard_max_mb, CLIP_MAX_MB_DEFAULT);
    sfree(body);
}

/*
 * far2l obeys the focus rule too. It was the one clipboard path that ignored it,
 * which made the rule untrue as stated - and "KiTTY never touches your clipboard
 * unless you are looking at that window" is worth having precisely because it has
 * no exceptions.
 *
 * The far2l clipboard subcommands are Win32-only, so what is checked here is the
 * decision the gate makes, not the clipboard itself: an unfocused window must
 * behave exactly as though the policy were Deny, and must still SEND a deny reply
 * rather than going quiet, or the remote far2l waits for an answer that never
 * comes.
 */
static void test_far2l_focus(Mock *mk)
{
    /*
     * far2l's clipboard OPEN subcommand ('o'), which is its permission gate: its
     * reply is a plain yes(1)/no(-1) byte, so it reads our gate directly and the
     * answer is observable. Payload is base64 of the bytes { 'o', 'c', id } -
     * far2l reads its command bytes from the END of the decoded buffer, so the
     * subcommand comes first in the array.
     */
    static const char ask[] = "far2l:b2MB";     /* base64 of 6F 63 01 = 'o','c',1 */
    char allowed[sizeof(osc52_last_send)];

    conf_set_bool(mk->term->conf, CONF_clipboard_require_focus, true);
    /* requests count only after the handshake, which seeds the policy from
     * the setting - so the policy is forced after it */
    feed_apc(mk, "far2l1", 6);
    mk->term->clip_allowed = 1;                 /* policy says allow */

    mk->term->has_focus = true;
    osc52_sends = 0;
    osc52_last_send[0] = '\0';
    feed_apc(mk, ask, strlen(ask));
    if (osc52_sends == 0) {
        fail("far2l focus rule", "no reply at all when focused");
        return;
    }
    strcpy(allowed, osc52_last_send);

    mk->term->has_focus = false;
    osc52_sends = 0;
    osc52_last_send[0] = '\0';
    feed_apc(mk, ask, strlen(ask));

    /*
     * It must still ANSWER - silence would leave the remote far2l waiting for a
     * reply that never comes, which is why the gate zeroes the permission rather
     * than returning early - and the answer must be a different one, because
     * unfocused has to mean refused.
     */
    if (osc52_sends == 0)
        fail("far2l focus rule",
             "far2l stopped replying when unfocused; the remote would hang");
    else if (!strcmp(allowed, osc52_last_send))
        fail("far2l focus rule",
             "an unfocused window gave the same answer as a focused one");

    /* and with the rule switched off, focus stops mattering again */
    conf_set_bool(mk->term->conf, CONF_clipboard_require_focus, false);
    osc52_last_send[0] = '\0';
    feed_apc(mk, ask, strlen(ask));
    if (strcmp(allowed, osc52_last_send))
        fail("far2l focus rule off",
             "focus still mattered after the rule was switched off");

    conf_set_bool(mk->term->conf, CONF_clipboard_require_focus, true);
    mk->term->has_focus = true;
    mk->term->clip_allowed = 0;
}

/*
 * far2l requests end to end through terminal.c, against the fake clipboard
 * above: the ID-0 rule, the bare acknowledgement, the open and its held
 * reply, "always allow" by client ID, the read gate, one empty per
 * transaction, chunked upload, data IDs, the window size and the focus
 * reports far2l asks for.
 */
#define F2L_MAXREP 8
static unsigned char f2l_rep[F2L_MAXREP][8192];
static size_t f2l_rep_len[F2L_MAXREP];
static int f2l_nrep;

/* Split what was sent to the host into far2l replies, decoded. The handshake
 * acknowledgement is not a reply and is skipped. */
static void f2l_collect(void)
{
    const char *p = osc52_all, *end = osc52_all + osc52_all_len;
    f2l_nrep = 0;
    while (p < end && f2l_nrep < F2L_MAXREP) {
        const char *s = strstr(p, "\033_far2l"), *bel;
        strbuf *dec;
        if (!s || s >= end)
            break;
        s += 7;
        bel = memchr(s, '\007', end - s);
        if (!bel)
            break;
        if (!(bel - s == 2 && !memcmp(s, "ok", 2))) {
            dec = base64_decode_sb(make_ptrlen(s, bel - s));
            f2l_rep_len[f2l_nrep] = dec->len < sizeof(f2l_rep[0]) ? dec->len
                                                                   : sizeof(f2l_rep[0]);
            memcpy(f2l_rep[f2l_nrep], dec->u, f2l_rep_len[f2l_nrep]);
            f2l_nrep++;
            strbuf_free(dec);
        }
        p = bel + 1;
    }
}

/* Send one request (the stack in wire order); returns the number of replies. */
static int f2l_send(Mock *mk, const F2lOut *req)
{
    strbuf *sb = strbuf_new();
    put_dataz(sb, "\033_far2l:");
    base64_encode_bs(BinarySink_UPCAST(sb), make_ptrlen(req->data, req->len), 0);
    put_byte(sb, '\007');
    osc52_all_len = 0;
    osc52_all[0] = '\0';
    term_data(mk->term, sb->s, sb->len);
    term_update(mk->term);
    strbuf_free(sb);
    f2l_collect();
    return f2l_nrep;
}

/* Build-and-send helpers: arguments are pushed bottom first, then the
 * command letter(s), then the ID on top. */
static int f2l_cmd(Mock *mk, char cmd, uint8_t id)
{
    F2lOut o;
    int n;
    f2l_out_init(&o);
    f2l_push_u8(&o, (uint8_t)cmd);
    f2l_push_u8(&o, id);
    n = f2l_send(mk, &o);
    f2l_out_free(&o);
    return n;
}

static int f2l_open(Mock *mk, const char *cid, uint8_t id)
{
    F2lOut o;
    int n;
    f2l_out_init(&o);
    if (cid) {
        f2l_push_bytes(&o, cid, strlen(cid));
        f2l_push_u32(&o, (uint32_t)strlen(cid));
    }
    f2l_push_u8(&o, 'o');
    f2l_push_u8(&o, 'c');
    f2l_push_u8(&o, id);
    n = f2l_send(mk, &o);
    f2l_out_free(&o);
    return n;
}

static int f2l_clip_simple(Mock *mk, char sub, uint8_t id)
{
    F2lOut o;
    int n;
    f2l_out_init(&o);
    f2l_push_u8(&o, (uint8_t)sub);
    f2l_push_u8(&o, 'c');
    f2l_push_u8(&o, id);
    n = f2l_send(mk, &o);
    f2l_out_free(&o);
    return n;
}

static int f2l_set(Mock *mk, uint32_t fmt, const void *data, size_t len, uint8_t id)
{
    F2lOut o;
    int n;
    f2l_out_init(&o);
    f2l_push_bytes(&o, data, len);
    f2l_push_u32(&o, (uint32_t)len);
    f2l_push_u32(&o, fmt);
    f2l_push_u8(&o, 's');
    f2l_push_u8(&o, 'c');
    f2l_push_u8(&o, id);
    /* the write rate cap has its own tests; not what is under test here */
    mk->term->clip_write_second = 0;
    mk->term->clip_write_count = 0;
    n = f2l_send(mk, &o);
    f2l_out_free(&o);
    return n;
}

static int f2l_get(Mock *mk, char sub, uint32_t fmt, uint8_t id)
{
    F2lOut o;
    int n;
    f2l_out_init(&o);
    f2l_push_u32(&o, fmt);
    f2l_push_u8(&o, (uint8_t)sub);
    f2l_push_u8(&o, 'c');
    f2l_push_u8(&o, id);
    n = f2l_send(mk, &o);
    f2l_out_free(&o);
    return n;
}

static int f2l_chunk(Mock *mk, uint16_t enc, unsigned char fill, uint8_t id)
{
    F2lOut o;
    unsigned char *buf;
    size_t n = (size_t)enc << 8;
    int r;
    f2l_out_init(&o);
    buf = snewn(n ? n : 1, unsigned char);
    memset(buf, fill, n);
    f2l_push_bytes(&o, buf, n);
    f2l_push_u16(&o, enc);
    f2l_push_u8(&o, 'S');
    f2l_push_u8(&o, 'c');
    f2l_push_u8(&o, id);
    r = f2l_send(mk, &o);
    f2l_out_free(&o);
    sfree(buf);
    return r;
}

static uint64_t f2l_le(const unsigned char *p, int n)
{
    uint64_t v = 0;
    while (n-- > 0)
        v = (v << 8) | p[n];
    return v;
}

static void test_far2l_protocol(Mock *mk)
{
    static const char cid[] = "testhost-0123456789abcdefghijklmnopqrstuv";
    uint64_t set_id;

    counters_reset();
    f2l_fake_empty();
    conf_set_bool(mk->term->conf, CONF_clipboard_require_focus, true);
    conf_set_int(mk->term->conf, CONF_shared_clipboard, 2);     /* Ask */
    conf_set_str(mk->term->conf, CONF_far2l_client_ids, "");
    mk->term->has_focus = true;

    /* --- nothing before the handshake --- */
    feed_apc(mk, "far2l0", 6);
    if (f2l_cmd(mk, 'p', 9) != 0)
        fail("far2l before handshake", "a request was answered before far2l1");
    osc52_all_len = 0;
    feed_apc(mk, "far2l1", 6);
    if (!strstr(osc52_all, "\033_far2lok\007"))
        fail("far2l handshake", "no far2lok");

    /* --- ID 0: never a reply; otherwise always one --- */
    if (f2l_cmd(mk, 'p', 0) != 0)
        fail("far2l ID 0", "a request with ID 0 was answered");
    if (f2l_cmd(mk, 'p', 5) != 1 || f2l_rep_len[0] != 3 ||
        f2l_rep[0][0] != 0 || f2l_rep[0][1] != 24 || f2l_rep[0][2] != 5)
        fail("far2l palette", "the 24-bit reply is not [0, 24, id]");
    if (f2l_cmd(mk, 'Z', 7) != 1 || f2l_rep_len[0] != 1 || f2l_rep[0][0] != 7)
        fail("far2l unknown command", "not answered with the ID alone");
    if (f2l_cmd(mk, 'f', 8) != 1 || f2l_rep_len[0] != 2 || f2l_rep[0][0] != 0)
        fail("far2l F-key titles", "not answered 'not supported'");
    if (f2l_cmd(mk, 'n', 0) != 0)
        fail("far2l notification", "a notification with ID 0 was answered");

    /* --- the window size: never 0 x 0 --- */
    f2l_max_rows = 0;
    if (f2l_cmd(mk, 'w', 6) != 1 || f2l_rep_len[0] != 5 ||
        f2l_le(f2l_rep[0], 2) != 80 || f2l_le(f2l_rep[0] + 2, 2) != 24)
        fail("far2l max size fallback", "not the current 80 x 24");
    f2l_max_rows = 50; f2l_max_cols = 200;
    if (f2l_cmd(mk, 'w', 6) != 1 || f2l_rep_len[0] != 5 ||
        f2l_le(f2l_rep[0], 2) != 200 || f2l_le(f2l_rep[0] + 2, 2) != 50)
        fail("far2l max size", "the width/height order or values are wrong");
    f2l_max_rows = 0;
    mock_maximise_calls = 0;
    f2l_cmd(mk, 'M', 0);
    if (mock_maximise_calls != 1 || !mock_maximised)
        fail("far2l maximise", "M did not maximise the window");
    f2l_cmd(mk, 'm', 0);
    if (mock_maximise_calls != 2 || mock_maximised)
        fail("far2l restore", "m did not restore the window");

    /* --- a malformed client ID is refused outright --- */
    if (f2l_open(mk, "SHORT", 10) != 1 || f2l_rep_len[0] != 10 ||
        (int8_t)f2l_rep[0][8] != 0)
        fail("far2l bad client ID", "not answered status 0");
    if (f2l_confirms != 0)
        fail("far2l bad client ID", "the box was raised for a malformed ID");

    /* --- Ask: the box, the held reply, and what waits behind it --- */
    if (f2l_open(mk, cid, 11) != 0)
        fail("far2l ask", "the open was answered before the box");
    if (f2l_confirms != 1 || !f2l_offer_always)
        fail("far2l ask", "no box, or no 'always allow' on it");
    if (f2l_get(mk, 'a', F2L_CF_TEXT, 12) != 0)
        fail("far2l ask", "a request behind the box was answered early");
    osc52_all_len = 0;
    osc52_all[0] = '\0';
    term_far2l_open_answer(mk->term, true, true);
    f2l_collect();
    if (f2l_nrep != 2)
        fail("far2l ask answer", "the open and the held request were not both answered");
    else {
        if (f2l_rep_len[0] != 10 || f2l_rep[0][9] != 11 || f2l_rep[0][8] != 1 ||
            f2l_le(f2l_rep[0], 8) != (F2L_FEATCLIP_DATA_ID | F2L_FEATCLIP_CHUNKED_SET))
            fail("far2l ask answer", "the open reply is not [features, 1, id]");
        if (f2l_rep_len[1] != 2 || f2l_rep[1][1] != 12)
            fail("far2l ask answer", "the held request was not answered after it");
    }
    if (!f2l_client_ids_contains(conf_get_str(mk->term->conf, CONF_far2l_client_ids),
                                 (const unsigned char *)cid, strlen(cid)) ||
        f2l_saves != 1)
        fail("far2l always allow", "the client ID was not remembered and saved");

    /* Allow on a box the host raised without a paste opens the clipboard,
     * never the read gate */
    kitty_far2l_clip_set(F2L_CF_TEXT, (const unsigned char *)"pre", 3, true);
    if (f2l_get(mk, 'g', F2L_CF_TEXT, 30) != 1 || f2l_le(f2l_rep[0] + 8, 4) != 0)
        fail("far2l ask answer", "Allow without a paste opened the read gate");

    /* --- one empty per transaction; the formats of one copy stay --- */
    f2l_sets = f2l_set_emptied = 0;
    if (f2l_set(mk, F2L_CF_TEXT, "hello", 5, 13) != 1 || f2l_rep_len[0] != 10 ||
        f2l_rep[0][8] != 1)
        fail("far2l set", "not answered [data ID, 1, id]");
    set_id = f2l_le(f2l_rep[0], 8);
    if (set_id == 0)
        fail("far2l set", "data ID 0 on success");
    f2l_set(mk, 0xC0DE, "\0\0\0\0", 4, 14);
    if (f2l_sets != 2 || f2l_set_emptied != 1 || f2l_last_set_empty_first)
        fail("far2l transaction", "the clipboard was emptied per format, not once");
    if (!kitty_far2l_clip_available(F2L_CF_TEXT))
        fail("far2l transaction", "the text did not survive the second format");
    f2l_clip_simple(mk, 'e', 0);
    f2l_set(mk, F2L_CF_TEXT, "hello", 5, 0);
    if (f2l_set_emptied != 1)
        fail("far2l transaction", "a set after CLIP_EMPTY emptied again");

    /* --- the read gate: nothing without a paste gesture --- */
    if (f2l_get(mk, 'g', F2L_CF_TEXT, 15) != 1 || f2l_rep_len[0] != 13 ||
        f2l_le(f2l_rep[0] + 8, 4) != 0 || f2l_le(f2l_rep[0], 8) != 0)
        fail("far2l read gate", "a read without a paste was served");
    if (f2l_get(mk, 'i', F2L_CF_TEXT, 16) != 1 || f2l_le(f2l_rep[0], 8) != 0)
        fail("far2l read gate", "a data ID without a paste was served");
    term_far2l_paste_gesture(mk->term);
    if (f2l_get(mk, 'g', F2L_CF_TEXT, 17) != 1 || f2l_rep_len[0] != 8 + 5 + 4 + 1 ||
        f2l_le(f2l_rep[0] + 13, 4) != 5 || memcmp(f2l_rep[0] + 8, "hello", 5))
        fail("far2l read", "the clipboard was not served after a paste");
    else if (f2l_le(f2l_rep[0], 8) != set_id)
        fail("far2l data ID", "the read's data ID differs from the set's");
    if (f2l_get(mk, 'i', F2L_CF_TEXT, 18) != 1 || f2l_le(f2l_rep[0], 8) != set_id)
        fail("far2l data ID", "GETDATAID does not match the data");

    /* --- chunked upload --- */
    {
        size_t got;
        unsigned char *all;
        f2l_chunk(mk, 1, 'A', 0);
        if (f2l_chunk(mk, 1, 'A', 19) != 1 || f2l_rep_len[0] != 1)
            fail("far2l chunk", "a chunk with an ID was not acknowledged alone");
        f2l_set(mk, F2L_CF_TEXT, "BC", 2, 20);
        all = kitty_far2l_clip_get(F2L_CF_TEXT, &got);
        if (!all || got != 514 || all[0] != 'A' || all[511] != 'A' ||
            memcmp(all + 512, "BC", 2))
            fail("far2l chunk", "the chunks were not put in front of the data");
        sfree(all);
        /* a cancelled upload leaves nothing behind */
        f2l_chunk(mk, 1, 'X', 0);
        f2l_chunk(mk, 0, 0, 0);
        f2l_set(mk, F2L_CF_TEXT, "Z", 1, 21);
        all = kitty_far2l_clip_get(F2L_CF_TEXT, &got);
        if (!all || got != 1 || all[0] != 'Z')
            fail("far2l chunk cancel", "cancelled chunks reached the clipboard");
        sfree(all);
    }

    /* --- close: the clipboard is no longer open for reads --- */
    f2l_clip_simple(mk, 'c', 0);
    if (f2l_get(mk, 'g', F2L_CF_TEXT, 22) != 1 || f2l_rep_len[0] != 5 ||
        f2l_le(f2l_rep[0], 4) != 0xFFFFFFFFU)
        fail("far2l closed", "a read after CLIP_CLOSE was not 'not open'");

    /* --- a listed client opens without the box; the gate still applies --- */
    feed_apc(mk, "far2l1", 6);
    f2l_confirms = 0;
    if (f2l_open(mk, cid, 23) != 1 || f2l_rep[0][8] != 1 || f2l_confirms != 0)
        fail("far2l always allow", "a listed client was asked again");
    if (f2l_get(mk, 'g', F2L_CF_TEXT, 24) != 1 || f2l_le(f2l_rep[0] + 8, 4) != 0)
        fail("far2l always allow", "a listed client read without a paste");

    /* --- far2l leaving while the box is open --- */
    feed_apc(mk, "far2l1", 6);
    conf_set_str(mk->term->conf, CONF_far2l_client_ids, "");
    f2l_confirms = 0;
    if (f2l_open(mk, cid, 25) != 1 || (int8_t)f2l_rep[0][8] != 0 || f2l_confirms != 0)
        fail("far2l box rule", "a second box came without a paste");
    term_far2l_paste_gesture(mk->term);
    f2l_confirm_ends = 0;
    f2l_open(mk, cid, 25);
    if (f2l_confirms != 1)
        fail("far2l box rule", "no box after a paste");
    feed_apc(mk, "far2l0", 6);
    if (f2l_confirm_ends != 1)
        fail("far2l leave", "the box was not closed when far2l left");
    osc52_all_len = 0;
    osc52_all[0] = '\0';
    term_far2l_open_answer(mk->term, true, false);
    f2l_collect();
    if (f2l_nrep != 0)
        fail("far2l leave", "a late answer was still sent");

    /* --- the host cannot raise boxes at will: closed unanswered counts as a
     * refusal, and far2l1 + open loops get -1 without a box --- */
    {
        int k;
        f2l_confirms = 0;
        for (k = 0; k < 5; k++) {
            feed_apc(mk, "far2l1", 6);
            if (f2l_open(mk, cid, 26) != 1 || (int8_t)f2l_rep[0][8] != -1)
                fail("far2l box loop", "not refused while the refusal stands");
        }
        if (f2l_confirms != 0)
            fail("far2l box loop", "a re-handshake raised the box again");
    }

    /* --- a real paste: one box; Allow on its back serves the held read --- */
    feed_apc(mk, "far2l1", 6);
    term_far2l_paste_gesture(mk->term);
    f2l_confirms = 0;
    f2l_open(mk, cid, 27);
    f2l_get(mk, 'g', F2L_CF_TEXT, 28);        /* held behind the box */
    if (f2l_confirms != 1)
        fail("far2l paste box", "no box after a paste");
    osc52_all_len = 0;
    osc52_all[0] = '\0';
    term_far2l_open_answer(mk->term, true, false);
    f2l_collect();
    if (f2l_nrep != 2 || f2l_rep[0][8] != 1 || f2l_le(f2l_rep[1] + f2l_rep_len[1] - 5, 4) == 0)
        fail("far2l paste box", "Allow after a paste did not serve the held read");
    /* that answer is not a new gesture: no further box without a paste */
    feed_apc(mk, "far2l1", 6);
    f2l_confirms = 0;
    if (f2l_open(mk, cid, 29) != 1 || (int8_t)f2l_rep[0][8] != 0 || f2l_confirms != 0)
        fail("far2l paste box", "a box came again without a new paste");

    /* --- Deny latches across far2l1 until a paste --- */
    term_far2l_paste_gesture(mk->term);
    f2l_open(mk, cid, 31);
    term_far2l_open_answer(mk->term, false, false);
    feed_apc(mk, "far2l1", 6);
    f2l_confirms = 0;
    if (f2l_open(mk, cid, 32) != 1 || (int8_t)f2l_rep[0][8] != -1 || f2l_confirms != 0)
        fail("far2l deny latch", "a re-handshake undid the refusal");

    /* --- register: only for an authorised open, capped per activation --- */
    feed_apc(mk, "far2l1", 6);
    mk->term->clip_allowed = 1;
    {
        F2lOut o;
        int k, got = 0;
        for (k = 0; k < 70; k++) {
            f2l_out_init(&o);
            f2l_push_bytes(&o, "FMT", 3);
            f2l_push_u32(&o, 3);
            f2l_push_u8(&o, 'r');
            f2l_push_u8(&o, 'c');
            f2l_push_u8(&o, 33);
            f2l_send(mk, &o);
            f2l_out_free(&o);
            if (k == 0) {
                if (f2l_le(f2l_rep[0], 4) != 0)
                    fail("far2l register", "a format was registered before an open");
                f2l_open(mk, NULL, 34);
            } else if (f2l_le(f2l_rep[0], 4) != 0) {
                got++;
            }
        }
        if (got != 64)
            fail("far2l register", "registrations are not capped at 64");
    }

    /* --- a chunk refused while unfocused fails the closing set --- */
    f2l_chunk(mk, 1, 'A', 0);
    mk->term->has_focus = false;
    f2l_chunk(mk, 1, 'B', 0);
    mk->term->has_focus = true;
    if (f2l_set(mk, F2L_CF_TEXT, "C", 1, 35) != 1 ||
        (int8_t)f2l_rep[0][f2l_rep_len[0] - 2] != 0)
        fail("far2l chunk refused", "a set with a refused chunk succeeded");

    /* --- the data-ID query counts like a read --- */
    term_far2l_paste_gesture(mk->term);
    f2l_get(mk, 'i', F2L_CF_TEXT, 36);
    if (mk->term->far2l_gate.prolongs != 1)
        fail("far2l data ID", "the query did not use an extension of the gate");

    /* --- blocking writes from the balloon closes an open box as refused --- */
    feed_apc(mk, "far2l1", 6);
    term_far2l_paste_gesture(mk->term);
    f2l_confirm_ends = 0;
    f2l_confirms = 0;
    f2l_open(mk, cid, 37);
    if (f2l_confirms != 1)
        fail("far2l block", "no box to block");
    osc52_all_len = 0;
    osc52_all[0] = '\0';
    term_far2l_block(mk->term);
    f2l_collect();
    if (f2l_confirm_ends != 1 || f2l_nrep != 1 || (int8_t)f2l_rep[0][8] != -1)
        fail("far2l block", "the box was not closed and the open refused");
    term_far2l_open_answer(mk->term, true, false);
    if (mk->term->clip_allowed != 0 ||
        conf_get_int(mk->term->conf, CONF_shared_clipboard) != 0)
        fail("far2l block", "a late Allow gave the permission back");
    conf_set_int(mk->term->conf, CONF_shared_clipboard, 2);

    /* --- the focus reports far2l asks for (DECSET 1004) --- */
    osc52_all_len = 0;
    osc52_all[0] = '\0';
    term_set_focus(mk->term, false);
    term_set_focus(mk->term, true);
    if (osc52_all_len != 0)
        fail("focus reports", "sent without DECSET 1004");
    term_data(mk->term, "\033[?1004h", 8);
    term_set_focus(mk->term, false);
    term_set_focus(mk->term, true);
    if (osc52_all_len != 6 || memcmp(osc52_all, "\033[O\033[I", 6))
        fail("focus reports", "not ESC [ O then ESC [ I");
    term_data(mk->term, "\033[?1004l", 8);

    f2l_fake_empty();
    conf_set_str(mk->term->conf, CONF_far2l_client_ids, "");
    mk->term->clip_allowed = 0;
}

/*
 * far2l key and mouse events are armed by the client's own feature request
 * ('x') after the handshake, not by far2l1 alone: a far2l1 in a file being
 * cat'ed must not turn the keyboard into events. And far2l images (request
 * 'i') through terminal.c: the wire shape of the replies, and the images
 * going with far2l0.
 */
static bool far2l_test_cells(TermWin *win, int *cw, int *ch, int *ox, int *oy)
{
    (void)win;
    *cw = 8; *ch = 16; *ox = 1; *oy = 1;
    return true;
}

static void test_far2l_arming_and_images(Mock *mk)
{
    static const char on[] = "far2l1", off[] = "far2l0";
    static const char feat[] = "far2l:AQAAAAAAAAB4AA==";   /* 'x', compact input, ID 0 */
    static const char caps[] = "far2l:Y2kF";                /* 'i' 'c', ID 5 */
    /* one red pixel "img" at cell 2,3, ID 6 (the far2l description's example) */
    static const char set[] =
        "far2l:/wAA/wEAAAABAAAA/////wMAAgAAAAAAAAAAAGltZwMAAABzaQY=";
    unsigned gen;
    int changes;

    feed_apc(mk, off, strlen(off));
    changes = f2l_events_changes;
    feed_apc(mk, on, strlen(on));
    if (!mk->term->far2l_ext || mk->term->far2l_events_armed)
        fail("far2l arming", "far2l1 alone armed key events");
    if (f2l_events_changes != changes + 1)
        fail("far2l arming", "a new far2l1 did not tell the window");
    gen = mk->term->far2l_input_gen;
    changes = f2l_events_changes;
    feed_apc(mk, feat, strlen(feat));
    if (!mk->term->far2l_events_armed || mk->term->far2l_input_gen == gen)
        fail("far2l arming", "the client's 'x' did not arm key events");
    if (f2l_events_changes != changes + 1)
        fail("far2l arming", "arming did not tell the window");
    changes = f2l_events_changes;
    feed_apc(mk, feat, strlen(feat));
    if (f2l_events_changes != changes)
        fail("far2l arming", "a second 'x' while armed told the window");
    feed_apc(mk, on, strlen(on));
    if (!mk->term->far2l_events_armed)
        fail("far2l arming", "a repeated far2l1 disarmed key events");
    if (f2l_events_changes != changes)
        fail("far2l arming", "a repeated far2l1 told the window");

    /* no window: no cell size, so caps 0 and a 0 x 0 cell - 13 bytes */
    osc52_last_send[0] = '\0';
    feed_apc(mk, caps, strlen(caps));
    if (strcmp(osc52_last_send, "\033_far2lAAAAAAAAAAAAAAAABQ==\007"))
        fail("far2l image caps", "unexpected reply to 'i c' without a window");

    /* with a cell size: the set is served and answered 1 */
    kitty_far2l_cell_hook = far2l_test_cells;
    conf_set_bool(mk->term->conf, CONF_far2l_images, true);
    osc52_last_send[0] = '\0';
    feed_apc(mk, set, strlen(set));
    if (strcmp(osc52_last_send, "\033_far2lAQY=\007"))
        fail("far2l image set", "the example image was not answered 1");
    if (!mk->term->far2l_images || mk->term->far2l_images->n != 1)
        fail("far2l image set", "the image was not stored");

    /*
     * Width is popped before height, so it lies ABOVE height on the stack:
     * the wire, bottom first, is [data][height][width][bottom]... A 2x1 RGBA
     * image (red, blue) at cells 2,3-5,6, ID 9, must come out 2 wide.
     */
    {
        static const char wide[] =
            "far2l:/wAA/wAA//8BAAAAAgAAAAYABQADAAIAAAAAAAAAAAB3AQAAAHNpCQ==";
        const Far2lImage *img;
        osc52_last_send[0] = '\0';
        feed_apc(mk, wide, strlen(wide));
        img = mk->term->far2l_images && mk->term->far2l_images->n == 2 ?
            &mk->term->far2l_images->imgs[1] : NULL;
        if (strcmp(osc52_last_send, "\033_far2lAQk=\007") || !img)
            fail("far2l image set 2x1", "the non-square image was not answered 1");
        else if (img->w != 2 || img->h != 1 ||
                 memcmp(img->px + 4, "\xff\x00\x00\xff", 4))
            fail("far2l image set 2x1", "width and height were popped swapped");
    }

    /*
     * A PNG through the real decoder (WIC, which needs COM: main initialises
     * it before the first image request, because the decoder's availability
     * is probed once per process). The file is a 4x4 solid red PNG, 125
     * bytes: width 125, height 1, at cells 2,3-5,6, ID 30. With the decoder
     * the caps say PNG and the set is answered 1 with a 4x4 opaque red image;
     * without it the caps do not say PNG and the set is answered 0.
     */
    {
        static const char pngset[] =
            "far2l:iVBORw0KGgoAAAANSUhEUgAAAAQAAAAECAYAAACp8Z5+AAAAAXNSR0IArs4c6QAAAARnQU1BAACxjwv8YQUAAAAJcEhZcwAADsMAAA7DAcdvqGQAAAASSURBVBhXY/jPwPAfGTOQLgAAPEAf4VUKBeIAAAAASUVORK5CYIIBAAAAfQAAAAYABQADAAIAAgAAAAAAAABwAQAAAHNpHg==";
        /* the same with width and height in each other's place: refused */
        static const char pngswapped[] =
            "far2l:iVBORw0KGgoAAAANSUhEUgAAAAQAAAAECAYAAACp8Z5+AAAAAXNSR0IArs4c6QAAAARnQU1BAACxjwv8YQUAAAAJcEhZcwAADsMAAA7DAcdvqGQAAAASSURBVBhXY/jPwPAfGTOQLgAAPEAf4VUKBeIAAAAASUVORK5CYIJ9AAAAAQAAAAYABQADAAIAAgAAAAAAAABwAQAAAHNpHg==";
        bool wic;
        const Far2lImage *img;
        osc52_last_send[0] = '\0';
        feed_apc(mk, caps, strlen(caps));
        /* "AwgA..." = caps 0x803, "AQgA..." = caps 0x801 */
        wic = !strncmp(osc52_last_send, "\033_far2lAwgA", 11);
        if (!wic && strncmp(osc52_last_send, "\033_far2lAQgA", 11))
            fail("far2l image caps", "unexpected caps with a cell size");
        osc52_last_send[0] = '\0';
        feed_apc(mk, pngswapped, strlen(pngswapped));
        if (strcmp(osc52_last_send, "\033_far2lAB4=\007"))
            fail("far2l PNG set", "a PNG with width and height swapped was not refused");
        osc52_last_send[0] = '\0';
        feed_apc(mk, pngset, strlen(pngset));
        if (wic) {
            img = mk->term->far2l_images && mk->term->far2l_images->n == 3 ?
                &mk->term->far2l_images->imgs[2] : NULL;
            if (strcmp(osc52_last_send, "\033_far2lAR4=\007") || !img)
                fail("far2l PNG set", "a valid PNG was not answered 1");
            else if (img->w != 4 || img->h != 4 || !img->opaque ||
                     memcmp(img->px, "\x00\x00\xff\xff", 4) ||
                     memcmp(img->px + 15 * 4, "\x00\x00\xff\xff", 4))
                fail("far2l PNG set", "the PNG did not decode to 4x4 opaque red");
        } else {
            printf("NOTE: no WIC decoder here; PNG checked as refused only\n");
            if (strcmp(osc52_last_send, "\033_far2lAB4=\007"))
                fail("far2l PNG set", "a PNG was accepted without a decoder");
        }
    }

    /* images off for the session: refused, answered 0 */
    conf_set_bool(mk->term->conf, CONF_far2l_images, false);
    osc52_last_send[0] = '\0';
    feed_apc(mk, set, strlen(set));
    if (strcmp(osc52_last_send, "\033_far2lAAY=\007"))
        fail("far2l image set", "images off still accepted an image");
    conf_set_bool(mk->term->conf, CONF_far2l_images, true);

    changes = f2l_events_changes;
    feed_apc(mk, off, strlen(off));
    if (mk->term->far2l_events_armed)
        fail("far2l arming", "far2l0 left key events armed");
    if (f2l_events_changes != changes + 1)
        fail("far2l arming", "far2l0 did not tell the window");
    if (mk->term->far2l_images && mk->term->far2l_images->n != 0)
        fail("far2l images", "far2l0 left the images up");

    feed_apc(mk, on, strlen(on));
    feed_apc(mk, feat, strlen(feat));
    changes = f2l_events_changes;
    term_pwron(mk->term, true);
    if (mk->term->far2l_events_armed || mk->term->far2l_ext)
        fail("far2l arming", "a reset left key events armed");
    if (f2l_events_changes == changes)
        fail("far2l arming", "a reset did not tell the window");
    kitty_far2l_cell_hook = NULL;
}

/* One far2l request whose arguments are `fill` zero bytes under the command
 * letters: with ClipboardMaxMB 1 and a megabyte of filler it is over the
 * ceiling. Returns the number of replies. */
static int f2l_big(Mock *mk, size_t fill, char cmd, char sub, uint8_t id)
{
    F2lOut o;
    unsigned char *z = snewn(fill, unsigned char);
    int n;
    memset(z, 0, fill);
    f2l_out_init(&o);
    f2l_push_bytes(&o, z, fill);
    f2l_push_u8(&o, (uint8_t)sub);
    f2l_push_u8(&o, (uint8_t)cmd);
    f2l_push_u8(&o, id);
    n = f2l_send(mk, &o);
    f2l_out_free(&o);
    sfree(z);
    return n;
}

/*
 * far2l's notification request goes to the host-notice entry point with its
 * title and text; a request over the payload ceiling is dropped but still
 * answered - a set with status 0, a chunk breaks the upload, anything else
 * the ID alone - whatever the base64 padding at its end.
 */
static void test_far2l_notify_and_overflow(Mock *mk)
{
    static const char title[] = "Copy", text[] = "done: 3 files";
    const size_t mb = 1024 * 1024;
    int old_mb = conf_get_int(mk->term->conf, CONF_clipboard_max_mb);
    size_t k;
    F2lOut o;

    feed_apc(mk, "far2l1", 6);

    /* --- notification: title on top, text below it; the ID alone back --- */
    hn_notice_calls = 0;
    f2l_out_init(&o);
    f2l_push_bytes(&o, text, strlen(text));
    f2l_push_u32(&o, (uint32_t)strlen(text));
    f2l_push_bytes(&o, title, strlen(title));
    f2l_push_u32(&o, (uint32_t)strlen(title));
    f2l_push_u8(&o, 'n');
    f2l_push_u8(&o, 40);
    if (f2l_send(mk, &o) != 1 || f2l_rep_len[0] != 1 || f2l_rep[0][0] != 40)
        fail("far2l notification", "not answered with the ID alone");
    f2l_out_free(&o);
    if (hn_notice_calls != 1 || strcmp(hn_notice_title, title) ||
        strcmp(hn_notice_body, text))
        fail("far2l notification", "title and text did not reach the notice");

    /* --- over the ceiling: answered, never left waiting --- */
    conf_set_int(mk->term->conf, CONF_clipboard_max_mb, 1);
    for (k = 0; k < 3; k++) {          /* every base64 padding at the end */
        size_t fill = mb + k;
        if (f2l_big(mk, fill, 'i', 's', 41) != 1 || f2l_rep_len[0] != 2 ||
            f2l_rep[0][0] != 0 || f2l_rep[0][1] != 41)
            fail("far2l over the ceiling", "image set not answered 0");
        if (f2l_big(mk, fill, 'c', 's', 42) != 1 || f2l_rep_len[0] != 2 ||
            f2l_rep[0][0] != 0 || f2l_rep[0][1] != 42)
            fail("far2l over the ceiling", "clipboard set not answered 0");
        mk->term->far2l_chunks.overflow = false;
        if (f2l_big(mk, fill, 'c', 'S', 43) != 1 || f2l_rep_len[0] != 1 ||
            f2l_rep[0][0] != 43 || !mk->term->far2l_chunks.overflow)
            fail("far2l over the ceiling", "chunk not answered or upload kept");
        if (f2l_big(mk, fill, 'c', 'g', 44) != 1 || f2l_rep_len[0] != 1 ||
            f2l_rep[0][0] != 44)
            fail("far2l over the ceiling", "other request not given the ID alone");
        if (f2l_big(mk, fill, 'i', 's', 0) != 0)
            fail("far2l over the ceiling", "ID 0 was answered");
    }
    /* under the ceiling the same request is served as ever */
    if (f2l_big(mk, 16, 'c', 'g', 45) != 1 || f2l_rep[0][f2l_rep_len[0] - 1] != 45)
        fail("far2l over the ceiling", "a small request was not served");

    f2l_chunks_clear(&mk->term->far2l_chunks);
    conf_set_int(mk->term->conf, CONF_clipboard_max_mb, old_mb);
    feed_apc(mk, "far2l0", 6);
}

/* The focus rule applies to WRITES as well, which is a change to behaviour that
 * shipped working - so it gets its own test in both positions. */
static void test_write_focus_rule(Mock *mk)
{
    const char *seq = "\033]52;c;SGVsbG8=\007";

    conf_set_bool(mk->term->conf, CONF_clipboard_require_focus, true);
    mk->term->has_focus = false;
    if (feed(mk, OSC52_CLIPBOARD_ALLOW, seq, strlen(seq)) != 0)
        fail("write with no focus", "the clipboard was written anyway");

    mk->term->has_focus = true;
    if (feed(mk, OSC52_CLIPBOARD_ALLOW, seq, strlen(seq)) != 1)
        fail("write with focus", "the clipboard was not written");

    /* switched off, an unfocused write works again */
    conf_set_bool(mk->term->conf, CONF_clipboard_require_focus, false);
    mk->term->has_focus = false;
    if (feed(mk, OSC52_CLIPBOARD_ALLOW, seq, strlen(seq)) != 1)
        fail("write, focus rule off", "the clipboard was not written");

    conf_set_bool(mk->term->conf, CONF_clipboard_require_focus, true);
    mk->term->has_focus = true;
}

/* ---------------------------------------------------------------------------
 * The xterm colour queries: OSC 4;n;? and OSC 10/11/12;? are answered with
 * the live colour through the same seam as the clipboard replies, so the
 * answer cannot be swallowed by local line editing. Set direction: ignored.
 */
static void expect_reply(Mock *mk, const char *what, const char *seq,
                         const char *want)
{
    counters_reset();
    term_data(mk->term, seq, strlen(seq));
    term_update(mk->term);
    if (osc52_sends != 1) {
        fail(what, osc52_sends ? "more than one reply" : "no reply was sent");
        return;
    }
    if (strcmp(osc52_last_send, want) != 0) {
        printf("   got:  %s\n   want: %s\n", osc52_last_send + 1, want + 1);
        fail(what, "the reply differs");
    }
}

static void expect_silence(Mock *mk, const char *what, const char *seq)
{
    counters_reset();
    term_data(mk->term, seq, strlen(seq));
    term_update(mk->term);
    if (osc52_sends != 0)
        fail(what, "a reply went out where none was due");
}

static void test_colour_queries(Mock *mk)
{
    /* Distinctive colours, so a reply built from the wrong slot shows. */
    static const struct { int conf; unsigned char r, g, b; } set[] = {
        { CONF_COLOUR_fg,        0xab, 0xcd, 0xef },
        { CONF_COLOUR_bg,        0x12, 0x34, 0x56 },
        { CONF_COLOUR_cursor_bg, 0x0f, 0xf0, 0x80 },
        { CONF_COLOUR_red,       0xc0, 0x10, 0x20 },
    };
    for (size_t i = 0; i < lenof(set); i++) {
        conf_set_int_int(mk->conf, CONF_colours, set[i].conf*3+0, set[i].r);
        conf_set_int_int(mk->conf, CONF_colours, set[i].conf*3+1, set[i].g);
        conf_set_int_int(mk->conf, CONF_colours, set[i].conf*3+2, set[i].b);
    }
    term_reconfig(mk->term, mk->conf);

    expect_reply(mk, "OSC 11 background query", "\033]11;?\007",
                 "\033]11;rgb:1212/3434/5656\007");
    expect_reply(mk, "OSC 10 foreground query", "\033]10;?\007",
                 "\033]10;rgb:abab/cdcd/efef\007");
    expect_reply(mk, "OSC 12 cursor colour query", "\033]12;?\007",
                 "\033]12;rgb:0f0f/f0f0/8080\007");
    expect_reply(mk, "OSC 11 query, ST terminator", "\033]11;?\033\\",
                 "\033]11;rgb:1212/3434/5656\007");
    expect_reply(mk, "OSC 4 palette query", "\033]4;1;?\007",
                 "\033]4;1;rgb:c0c0/1010/2020\007");

    /* Setting a colour over these sequences is not accepted, and gets no
     * reply either. */
    expect_silence(mk, "OSC 11 set", "\033]11;rgb:ffff/ffff/ffff\007");
    expect_silence(mk, "OSC 10 set", "\033]10;#ffffff\007");
    expect_silence(mk, "OSC 4 set", "\033]4;1;rgb:ffff/ffff/ffff\007");
    expect_silence(mk, "OSC 111 reset", "\033]111\007");
    expect_silence(mk, "OSC 4 out of range", "\033]4;999;?\007");
    /* ... and the colour it would have set is unchanged. */
    expect_reply(mk, "OSC 11 after the refused set", "\033]11;?\007",
                 "\033]11;rgb:1212/3434/5656\007");
}

/*
 * The write confirmation box (writes set to Ask). It is modeless, so the write
 * WAITS for the answer while the parser goes on: Yes applies what waited and
 * latches Allow, No drops it and latches Deny, a read behind the write runs
 * after it, and a setting changed meanwhile (Change Settings runs beside the
 * box) wins over the answer. Each case answers its box, so none leaves a
 * write waiting for the next.
 */
static void wc_reset(Mock *mk)
{
    read_reset(mk);
    conf_set_int(mk->term->conf, CONF_clipboard_writes_per_sec, 0);
    mk->term->osc52_allowed = OSC52_CLIPBOARD_ASK;
    mk->clip_writes = 0;
    osc52_wconfirms = 0;
    osc52_set_fail = false;
    counters_reset();
}

static void feed_seq(Mock *mk, const char *seq)
{
    term_data(mk->term, seq, strlen(seq));
    term_update(mk->term);
}

/*
 * KiTTY: xterm modifyOtherKeys (XTMODKEYS, its query and its reset forms)
 * and the colour-scheme reports of private mode 2031: the query, DECRQM,
 * the unsolicited report on a flip of the dark/light verdict and ONLY then,
 * ONLY while the mode is set, and RIS resetting both.
 */
static void set_bg(Mock *mk, unsigned char r, unsigned char g, unsigned char b)
{
    conf_set_int_int(mk->conf, CONF_colours, CONF_COLOUR_bg*3+0, r);
    conf_set_int_int(mk->conf, CONF_colours, CONF_COLOUR_bg*3+1, g);
    conf_set_int_int(mk->conf, CONF_colours, CONF_COLOUR_bg*3+2, b);
    counters_reset();
    term_reconfig(mk->term, mk->conf);
}

static void expect_level(Mock *mk, const char *what, int want)
{
    if (mk->term->modify_other_keys != want) {
        printf("   level is %d, want %d\n", mk->term->modify_other_keys, want);
        fail(what, "modifyOtherKeys is not at the level it should be");
    }
}

static void test_modkeys_and_colour_scheme(Mock *mk)
{
    expect_level(mk, "modifyOtherKeys default", 0);
    feed_seq(mk, "\033[>4;2m");
    expect_level(mk, "XTMODKEYS level 2", 2);
    expect_reply(mk, "XTQMODKEYS at level 2", "\033[?4m", "\033[>4;2m");
    feed_seq(mk, "\033[>4;1m");
    expect_level(mk, "XTMODKEYS level 1", 1);
    feed_seq(mk, "\033[>4;3m");
    expect_level(mk, "XTMODKEYS level 3 is taken as 2", 2);
    feed_seq(mk, "\033[>4m");
    expect_level(mk, "XTMODKEYS Pv omitted resets", 0);
    feed_seq(mk, "\033[>4;2m\033[>4n");
    expect_level(mk, "CSI > 4 n disables", 0);
    feed_seq(mk, "\033[>4;2m\033[>m");
    expect_level(mk, "bare CSI > m resets", 0);
    feed_seq(mk, "\033[>1;2m");
    expect_level(mk, "another option leaves it alone", 0);
    expect_reply(mk, "XTQMODKEYS at level 0", "\033[?4m", "\033[>4;0m");
    expect_silence(mk, "XTQMODKEYS, another option", "\033[?1m");
    expect_silence(mk, "CSI > 1 n, another option", "\033[>1n");
    expect_level(mk, "still 0", 0);

    /* --- colour-scheme reports --- */
    set_bg(mk, 0x12, 0x34, 0x56);              /* dark */
    if (osc52_sends)
        fail("palette change with mode 2031 reset", "a report went out");
    expect_reply(mk, "CSI ? 996 n, dark", "\033[?996n", "\033[?997;1n");
    expect_reply(mk, "DECRQM 2031 reset", "\033[?2031$p", "\033[?2031;2$y");
    feed_seq(mk, "\033[?2031h");
    expect_reply(mk, "DECRQM 2031 set", "\033[?2031$p", "\033[?2031;1$y");
    set_bg(mk, 0xff, 0xff, 0xff);              /* flips to light */
    if (osc52_sends != 1 || strcmp(osc52_last_send, "\033[?997;2n") != 0) {
        printf("   sends %d, last: %s\n", osc52_sends, osc52_last_send + 1);
        fail("flip to light", "no single report of the new verdict");
    }
    set_bg(mk, 0xf0, 0xf0, 0xf0);              /* still light */
    if (osc52_sends)
        fail("palette change, same verdict", "a report went out");
    set_bg(mk, 0x00, 0x00, 0x00);              /* flips to dark */
    if (osc52_sends != 1 || strcmp(osc52_last_send, "\033[?997;1n") != 0)
        fail("flip to dark", "no single report of the new verdict");
    expect_reply(mk, "CSI ? 996 n after the flip", "\033[?996n",
                 "\033[?997;1n");
    feed_seq(mk, "\033[?2031l");
    set_bg(mk, 0xff, 0xff, 0xff);              /* flips, mode reset */
    if (osc52_sends)
        fail("flip with mode 2031 reset", "a report went out");
    expect_reply(mk, "CSI ? 996 n, light", "\033[?996n", "\033[?997;2n");
    expect_silence(mk, "CSI ? 6 n, another query", "\033[?6n");

    /* --- RIS resets both --- */
    feed_seq(mk, "\033[>4;2m\033[?2031h");
    expect_level(mk, "set before RIS", 2);
    feed_seq(mk, "\033c");
    expect_level(mk, "RIS resets modifyOtherKeys", 0);
    expect_reply(mk, "RIS resets mode 2031", "\033[?2031$p", "\033[?2031;2$y");

    set_bg(mk, 0x12, 0x34, 0x56);              /* as the colour tests left it */
}

/*
 * KiTTY: the kitty keyboard protocol's flag stacks (CSI > flags u push,
 * CSI < n u pop, CSI = flags ; mode u set, CSI ? u query): the query answers
 * the top entry, the stacks of the main and the alternate screen are
 * separate, a full stack drops its oldest entry, an emptied one is flags 0,
 * and RIS empties both.
 */
static void expect_kkp(Mock *mk, const char *what, unsigned want)
{
    char rep[32];
    snprintf(rep, sizeof(rep), "\033[?%uu", want);
    expect_reply(mk, what, "\033[?u", rep);
    if (term_kkp_flags(mk->term) != want) {
        printf("   term_kkp_flags %u, want %u\n", term_kkp_flags(mk->term),
               want);
        fail(what, "the window would read other flags than the query");
    }
}

static void test_kitty_keyboard_stack(Mock *mk)
{
    int i;

    expect_kkp(mk, "kkp: default", 0);
    feed_seq(mk, "\033[>1u");
    expect_kkp(mk, "kkp: push 1", 1);
    feed_seq(mk, "\033[>15u");
    expect_kkp(mk, "kkp: push 15", 15);
    feed_seq(mk, "\033[>u");
    expect_kkp(mk, "kkp: push nothing is 0", 0);
    feed_seq(mk, "\033[<u");
    expect_kkp(mk, "kkp: pop one", 15);
    feed_seq(mk, "\033[>3u\033[>7u\033[<2u");
    expect_kkp(mk, "kkp: pop two", 15);
    feed_seq(mk, "\033[<9u");
    expect_kkp(mk, "kkp: pop past the bottom resets", 0);
    feed_seq(mk, "\033[>99u");
    expect_kkp(mk, "kkp: unknown bits are masked", 99 & 31);
    feed_seq(mk, "\033[<u");

    /* set: replace, or, clear; with and without a stack entry */
    feed_seq(mk, "\033[=5u");
    expect_kkp(mk, "kkp: set 5 (mode 1 default)", 5);
    feed_seq(mk, "\033[=2;2u");
    expect_kkp(mk, "kkp: set 2 mode 2 ors", 7);
    feed_seq(mk, "\033[=4;3u");
    expect_kkp(mk, "kkp: set 4 mode 3 clears", 3);
    feed_seq(mk, "\033[=8;1u");
    expect_kkp(mk, "kkp: set 8 mode 1 replaces", 8);
    feed_seq(mk, "\033[<u");
    expect_kkp(mk, "kkp: pop with no entry leaves 0", 0);
    feed_seq(mk, "\033[>1u\033[=3;2u");
    expect_kkp(mk, "kkp: set changes the top entry", 3);
    feed_seq(mk, "\033[>8u\033[<u");
    expect_kkp(mk, "kkp: ...which the pop returns to", 3);
    feed_seq(mk, "\033[=0u");
    expect_kkp(mk, "kkp: set 0", 0);
    feed_seq(mk, "\033[<u");

    /* plain CSI u is still the cursor restore, and CSI ? 1 u is nothing */
    expect_silence(mk, "kkp: CSI u stays SCORC", "\033[u");
    expect_silence(mk, "kkp: CSI ? 1 u is not the query", "\033[?1u");

    /* the stacks of the two screens */
    feed_seq(mk, "\033[>1u");
    expect_kkp(mk, "kkp: main screen 1", 1);
    feed_seq(mk, "\033[?1049h");
    expect_kkp(mk, "kkp: alternate screen starts at 0", 0);
    feed_seq(mk, "\033[>11u");
    expect_kkp(mk, "kkp: alternate screen 11", 11);
    feed_seq(mk, "\033[?1049l");
    expect_kkp(mk, "kkp: back on main, still 1", 1);
    feed_seq(mk, "\033[?1049h");
    expect_kkp(mk, "kkp: alternate keeps its 11", 11);
    feed_seq(mk, "\033[<u");
    expect_kkp(mk, "kkp: alternate popped", 0);
    feed_seq(mk, "\033[?1049l");
    expect_kkp(mk, "kkp: main unchanged by the alternate's pop", 1);
    feed_seq(mk, "\033[?47h\033[>2u\033[?47l");
    expect_kkp(mk, "kkp: mode 47 is the alternate screen too", 1);
    feed_seq(mk, "\033[?47h");
    expect_kkp(mk, "kkp: ...with its own 2", 2);
    feed_seq(mk, "\033[<u\033[?47l\033[<u");
    expect_kkp(mk, "kkp: both emptied", 0);

    /* the depth: the oldest entry drops out */
    for (i = 1; i <= 20; i++) {
        char seq[16];
        snprintf(seq, sizeof(seq), "\033[>%du", i & 31);
        feed_seq(mk, seq);
    }
    expect_kkp(mk, "kkp: 20 pushes, top is the last", 20);
    feed_seq(mk, "\033[<15u");
    expect_kkp(mk, "kkp: 15 popped of the 16 kept", 5);
    feed_seq(mk, "\033[<u");
    expect_kkp(mk, "kkp: the 16th pop empties it", 0);
    feed_seq(mk, "\033[<u");
    expect_kkp(mk, "kkp: pop of an empty stack", 0);

    /* RIS empties both */
    feed_seq(mk, "\033[>1u\033[?1049h\033[>9u");
    expect_kkp(mk, "kkp: alternate before RIS", 9);
    feed_seq(mk, "\033c");
    expect_kkp(mk, "kkp: RIS, main screen, 0", 0);
    feed_seq(mk, "\033[?1049h");
    expect_kkp(mk, "kkp: RIS, alternate screen, 0", 0);
    feed_seq(mk, "\033[?1049l");
}

/*
 * KiTTY: the kitty graphics protocol through the terminal (kitty_gfx_term.c):
 * a raw image placed at the cursor, answered through the raw seam, visible
 * through term_gfx_visible, the cursor moved; the anchors across every
 * line-moving path - a scroll into the scrollback and off its end, a region
 * scroll, IL/DL, ED 2, ED 3, the alternate screen, a resize; a sequence over
 * the ceiling answered EFBIG; the setting off; CSI 16 t; a reset.
 */
static size_t gfx_b64(const unsigned char *in, size_t n, char *out)
{
    static const char tab[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i, o = 0;
    for (i = 0; i < n; i += 3) {
        unsigned v = in[i] << 16;
        if (i + 1 < n) v |= in[i + 1] << 8;
        if (i + 2 < n) v |= in[i + 2];
        out[o++] = tab[(v >> 18) & 63];
        out[o++] = tab[(v >> 12) & 63];
        out[o++] = i + 1 < n ? tab[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? tab[v & 63] : '=';
    }
    out[o] = '\0';
    return o;
}

/* Sends APC _G <ctl>;<base64 of w*h red RGB pixels> (no payload for w 0)
 * and returns what went back (osc52_last_send, "" when nothing). */
static const char *gfx_send(Mock *mk, const char *ctl, int w, int h)
{
    size_t npx = (size_t)w * h, len = 0, cap = strlen(ctl) + npx * 4 + 16;
    char *seq = snewn(cap, char);
    unsigned char *px = NULL;
    size_t i;
    memcpy(seq, "\033_G", 3); len = 3;
    memcpy(seq + len, ctl, strlen(ctl)); len += strlen(ctl);
    if (w > 0) {
        px = snewn(npx * 3, unsigned char);
        for (i = 0; i < npx; i++) {
            px[i * 3] = 255; px[i * 3 + 1] = 0; px[i * 3 + 2] = 0;
        }
        seq[len++] = ';';
        len += gfx_b64(px, npx * 3, seq + len);
        sfree(px);
    }
    seq[len++] = '\033';
    seq[len++] = '\\';
    counters_reset();
    osc52_last_send[0] = '\0';
    term_data(mk->term, seq, len);
    term_update(mk->term);
    sfree(seq);
    return osc52_last_send;
}

/* The visible placement of image id, or NULL. */
static const GfxVisible *gfx_vis(Mock *mk, unsigned id)
{
    static GfxVisible v[16];
    int n = term_gfx_visible(mk->term, v, 16), i;
    for (i = 0; i < n; i++)
        if (v[i].pl->image_id == id)
            return &v[i];
    return NULL;
}

static void gfx_expect_row(Mock *mk, const char *what, unsigned id, int row)
{
    const GfxVisible *v = gfx_vis(mk, id);
    if (!v) {
        if (row >= 0) {
            printf("   image %u not visible, want row %d\n", id, row);
            fail(what, "the placement is not where it should be");
        }
        return;
    }
    if (row < 0) {
        printf("   image %u visible at row %d, want none\n", id, v->r0);
        fail(what, "a placement that should have gone is still shown");
    } else if (v->r0 != row) {
        printf("   image %u at row %d, want %d\n", id, v->r0, row);
        fail(what, "the placement is not where it should be");
    }
}

static void test_kitty_graphics(Mock *mk)
{
    Terminal *term = mk->term;
    const GfxVisible *v;
    const char *r;
    int i;

    term_size(term, 24, 80, 100);
    term_notify_cell_size_pixels(term, 8, 16);
    feed_seq(mk, "\033[r\033[2J\033[H");

    /* CSI 16 t: the cell size, height first, through the raw seam */
    expect_reply(mk, "gfx: CSI 16 t", "\033[16t", "\033[6;16;8t");

    /* a 16 x 32 image at row 5, column 3: 2 x 2 cells, answered OK, the
     * cursor after its right edge on its last row */
    feed_seq(mk, "\033[5;3H");
    r = gfx_send(mk, "a=T,f=24,s=16,v=32,i=1", 16, 32);
    if (strcmp(r, "\033_Gi=1;OK\033\\"))
        fail("gfx: place", "the placement was not answered OK");
    v = gfx_vis(mk, 1);
    if (!v || v->c0 != 2 || v->r0 != 4 || v->c1 != 3 || v->r1 != 5 ||
        v->dx0 != 16 || v->dy0 != 64 || v->dx1 != 32 || v->dy1 != 96)
        fail("gfx: place", "the placement is not at the cursor cell");
    if (term->curs.x != 4 || term->curs.y != 5)
        fail("gfx: place", "the cursor did not move past the image");
    /* C=1 leaves the cursor alone; at the right edge it clamps */
    feed_seq(mk, "\033[1;80H");
    gfx_send(mk, "a=T,f=24,s=16,v=16,i=2,C=1", 16, 16);
    if (term->curs.x != 79 || term->curs.y != 0)
        fail("gfx: C=1", "the cursor moved");
    gfx_send(mk, "a=p,i=2", 0, 0);
    if (term->curs.x != 79 || term->curs.y != 0)
        fail("gfx: right edge", "the cursor was not clamped to the screen");
    gfx_send(mk, "a=d,d=I,i=2", 0, 0);

    /* a scroll into the scrollback: the image follows its text, is seen
     * again when the view scrolls back, and goes when its lines are
     * evicted */
    feed_seq(mk, "\033[24;1H\n\n\n");
    gfx_expect_row(mk, "gfx: scroll 3", 1, 1);
    feed_seq(mk, "\n\n");
    v = gfx_vis(mk, 1);
    if (!v || v->r0 != 0 || v->r1 != 0 || v->dy0 != -16)
        fail("gfx: half out", "the placement half above the screen");
    feed_seq(mk, "\n\n");
    gfx_expect_row(mk, "gfx: scrolled out", 1, -1);
    term->disptop = -7;
    gfx_expect_row(mk, "gfx: scrolled back", 1, 4);
    term->disptop = 0;
    if (!term->gfx || term->gfx->n_pls != 1)
        fail("gfx: scrollback", "the placement in the scrollback went");
    for (i = 0; i < 110; i++)
        feed_seq(mk, "\n");
    /* the placement goes; the image keeps its id for a later a=p */
    if (term->gfx->n_pls != 0 || !gfx_image_by_id(term->gfx, 1))
        fail("gfx: evicted", "a placement off the end of the scrollback stayed");

    /* a region scroll moves what is inside the band and leaves the rest */
    feed_seq(mk, "\033[10;1H");
    gfx_send(mk, "a=T,f=24,s=8,v=16,i=3", 8, 16);
    feed_seq(mk, "\033[20;1H");
    gfx_send(mk, "a=T,f=24,s=8,v=16,i=4", 8, 16);
    feed_seq(mk, "\033[5;15r\033[15;1H\n");
    gfx_expect_row(mk, "gfx: region scroll", 3, 8);
    gfx_expect_row(mk, "gfx: outside the region", 4, 19);
    for (i = 0; i < 5; i++)
        feed_seq(mk, "\n");
    gfx_expect_row(mk, "gfx: scrolled out of the region", 3, -1);
    gfx_expect_row(mk, "gfx: outside the region still", 4, 19);
    feed_seq(mk, "\033[r");

    /* IL and DL */
    feed_seq(mk, "\033[10;1H\033[2L");
    gfx_expect_row(mk, "gfx: IL", 4, 21);
    feed_seq(mk, "\033[M");
    gfx_expect_row(mk, "gfx: DL", 4, 20);
    feed_seq(mk, "\033[21;1H\033[M");
    gfx_expect_row(mk, "gfx: DL on the image", 4, -1);

    /* ED 2 takes the screen's images, not the scrollback's; ED 3 takes
     * the scrollback's */
    feed_seq(mk, "\033[5;1H");
    gfx_send(mk, "a=T,f=24,s=8,v=16,i=5", 8, 16);
    feed_seq(mk, "\033[24;1H\n\n\n\n\n\n");
    feed_seq(mk, "\033[12;1H");
    gfx_send(mk, "a=T,f=24,s=8,v=16,i=6", 8, 16);
    feed_seq(mk, "\033[2J");
    gfx_expect_row(mk, "gfx: ED 2", 6, -1);
    term->disptop = -6;
    gfx_expect_row(mk, "gfx: ED 2 spares the scrollback", 5, 4);
    term->disptop = 0;
    feed_seq(mk, "\033[3J");
    if (term->gfx->n_pls != 0)
        fail("gfx: ED 3", "a placement survived the scrollback's clearing");

    /* the alternate screen: its placements live while it is shown */
    feed_seq(mk, "\033[8;1H");
    gfx_send(mk, "a=T,f=24,s=8,v=16,i=7", 8, 16);
    feed_seq(mk, "\033[?1049h\033[3;1H");
    gfx_expect_row(mk, "gfx: alt hides main", 7, -1);
    gfx_send(mk, "a=T,f=24,s=8,v=16,i=8", 8, 16);
    gfx_expect_row(mk, "gfx: alt placement", 8, 2);
    /* the alternate screen scrolled back (erase_to_scrollback's virtual
     * scrollback) shows the main screen's lines: no placement is drawn */
    term->disptop = -1;
    gfx_expect_row(mk, "gfx: alt scrolled back", 8, -1);
    gfx_expect_row(mk, "gfx: alt scrolled back, main", 7, -1);
    term->disptop = 0;
    gfx_expect_row(mk, "gfx: alt placement back", 8, 2);
    feed_seq(mk, "\033[?1049l");
    gfx_expect_row(mk, "gfx: alt left", 8, -1);
    gfx_expect_row(mk, "gfx: main back", 7, 7);
    feed_seq(mk, "\033[?1049h");
    gfx_expect_row(mk, "gfx: alt starts blank", 8, -1);
    feed_seq(mk, "\033[?1049l");

    /* a resize: rows pushed into the scrollback keep the anchor, and give
     * it back when they return */
    feed_seq(mk, "\033[20;1H");
    gfx_send(mk, "a=T,f=24,s=8,v=16,i=9", 8, 16);
    feed_seq(mk, "\033[24;1H");
    term_size(term, 20, 80, 100);
    gfx_expect_row(mk, "gfx: shrink", 9, 15);
    gfx_expect_row(mk, "gfx: shrink, the other", 7, 3);
    term_size(term, 24, 80, 100);
    gfx_expect_row(mk, "gfx: grow", 9, 19);
    gfx_expect_row(mk, "gfx: grow, the other", 7, 7);
    term_size(term, 24, 80, 2);        /* the scrollback trimmed */
    gfx_expect_row(mk, "gfx: scrollback trimmed", 9, 19);
    term_size(term, 24, 80, 100);
    /* a shrink with the cursor at the top deletes the bottom rows: the
     * image on them does not come back when the screen grows */
    feed_seq(mk, "\033[22;1H");
    gfx_send(mk, "a=T,f=24,s=8,v=16,i=17", 8, 16);
    feed_seq(mk, "\033[1;1H");
    term_size(term, 20, 80, 100);
    gfx_expect_row(mk, "gfx: bottom rows cut", 17, -1);
    gfx_expect_row(mk, "gfx: the last row kept", 9, 19);
    term_size(term, 24, 80, 100);
    gfx_expect_row(mk, "gfx: cut rows stay empty", 17, -1);
    gfx_expect_row(mk, "gfx: the other survives the cut", 7, 7);

    /* a region scroll from row 1 with a scrollback: the band's rows enter
     * it, the rows below the band keep their place on the screen */
    feed_seq(mk, "\033[10;1H");
    gfx_send(mk, "a=T,f=24,s=8,v=16,i=14", 8, 16);
    feed_seq(mk, "\033[20;1H");
    gfx_send(mk, "a=T,f=24,s=8,v=16,i=15", 8, 16);
    feed_seq(mk, "\033[1;15r\033[15;1H\n");
    gfx_expect_row(mk, "gfx: region sb scroll, inside", 14, 8);
    gfx_expect_row(mk, "gfx: region sb scroll, below", 15, 19);
    feed_seq(mk, "\033[r");
    gfx_send(mk, "a=d,d=i,i=14", 0, 0);
    gfx_send(mk, "a=d,d=i,i=15", 0, 0);

    /* no scrollback at all (the harness default): a scroll drops what
     * leaves the screen instead of parking it */
    term_size(term, 24, 80, 0);
    feed_seq(mk, "\033[5;1H");
    gfx_send(mk, "a=T,f=24,s=8,v=16,i=16", 8, 16);
    feed_seq(mk, "\033[24;1H\n\n");
    gfx_expect_row(mk, "gfx: no scrollback, scrolled", 16, 2);
    feed_seq(mk, "\n\n\n\n");
    /* images 7 and 9 remain on the screen, 16 left it */
    if (term->gfx->n_pls != 2 || gfx_vis(mk, 16) || !gfx_vis(mk, 7) ||
        !gfx_vis(mk, 9))
        fail("gfx: no scrollback", "a placement off the top was kept");
    term_size(term, 24, 80, 100);

    /* over the ceiling: cut, answered EFBIG, nothing kept */
    {
        size_t big = OSC_STR_MAX_GFX + 4096, n = 0;
        char *seq = snewn(big + 64, char);
        static const char ctl[] = "\033_Ga=T,f=24,s=1,v=1,i=10;";
        memcpy(seq, ctl, sizeof(ctl) - 1); n = sizeof(ctl) - 1;
        memset(seq + n, 'A', big); n += big;
        seq[n++] = '\033';
        seq[n++] = '\\';
        counters_reset();
        osc52_last_send[0] = '\0';
        term_data(term, seq, n);
        term_update(term);
        sfree(seq);
        if (strncmp(osc52_last_send, "\033_Gi=10;EFBIG:", 14))
            fail("gfx: ceiling", "an over-long sequence was not answered EFBIG");
        if (gfx_image_by_id(term->gfx, 10) || term->gfx->pend.active)
            fail("gfx: ceiling", "something of the cut sequence was kept");
    }
    /* a chunked upload: the first chunk is silent, the last answers */
    feed_seq(mk, "\033[2;1H");
    r = gfx_send(mk, "a=T,f=24,s=8,v=16,i=11,m=1", 8, 8);
    if (r[0])
        fail("gfx: chunk", "the first chunk was answered");
    r = gfx_send(mk, "m=0", 8, 8);
    if (strcmp(r, "\033_Gi=11;OK\033\\"))
        fail("gfx: chunk", "the last chunk was not answered OK");
    gfx_expect_row(mk, "gfx: chunked placement", 11, 1);

    /* the setting off: ignored, nothing stored, nothing answered */
    conf_set_bool(term->conf, CONF_kitty_graphics, false);
    r = gfx_send(mk, "a=T,f=24,s=8,v=16,i=12", 8, 16);
    if (osc52_sends != 0 || gfx_image_by_id(term->gfx, 12))
        fail("gfx: setting off", "the sequence was served");
    conf_set_bool(term->conf, CONF_kitty_graphics, true);

    /* a=d without keys: the screen's placements; a reset: everything */
    gfx_send(mk, "a=d", 0, 0);
    if (term->gfx->n_pls != 0)
        fail("gfx: delete all", "placements remained");
    gfx_send(mk, "a=T,f=24,s=8,v=16,i=13", 8, 16);
    feed_seq(mk, "\033c");
    if (term->gfx->n_pls != 0 || term->gfx->n_imgs != 0)
        fail("gfx: RIS", "the store was not emptied");
    term_notify_cell_size_pixels(term, 0, 0);
}

/*
 * KiTTY: what chafa -f kitty sends for a 50 x 60 picture, exactly: its
 * queries, then a=T,f=32,c=5,r=3,q=2 with m=1 and no payload, the RGBA in
 * 4096-character continuation chunks, an empty m=0, then CR LF. The cursor
 * has to end right of the picture's box on its last row (5 right, 2 down)
 * before the CR LF, so the prompt comes below the picture - also when the
 * command ran on the bottom row, where the screen has to scroll for it.
 */
static void chafa_replay(Mock *mk, const char *what, int row)
{
    Terminal *term = mk->term;
    static const char *const queries[] = {
        "\033]10;?\033\\", "\033]11;?\033\\", "\033[18t", "\033[14t",
        "\033[16t", "\033[0c", "\033[?25l",
    };
    size_t i, raw = 50 * 60 * 4, blen, off;
    unsigned char *px = snewn(raw, unsigned char);
    char *b64 = snewn(raw / 3 * 4 + 8, char);
    char chunk[4200], pos[32];
    int x0, y0, want_y;
    GfxVisible v[4];

    feed_seq(mk, "\033c\033[r\033[2J\033[3J");
    sprintf(pos, "\033[%d;11H", row);
    feed_seq(mk, pos);
    x0 = term->curs.x;
    y0 = term->curs.y;
    want_y = y0 + 2 < term->rows ? y0 + 2 : term->rows - 1;
    for (i = 0; i < sizeof(queries) / sizeof(queries[0]); i++)
        feed_seq(mk, queries[i]);
    memset(px, 0, raw);
    blen = gfx_b64(px, raw, b64);
    feed_seq(mk, "\033_Ga=T,f=32,s=50,v=60,c=5,r=3,m=1,q=2\033\\");
    for (off = 0; off < blen; off += 4096) {
        size_t n = blen - off < 4096 ? blen - off : 4096;
        int k = sprintf(chunk, "\033_Gm=1;");
        memcpy(chunk + k, b64 + off, n);
        memcpy(chunk + k + n, "\033\\", 3);
        feed_seq(mk, chunk);
    }
    feed_seq(mk, "\033_Gm=0\033\\");
    if (term_gfx_visible(term, v, 4) != 1 || v[0].r1 - v[0].r0 != 2 ||
        v[0].c1 - v[0].c0 != 4 || v[0].r1 != want_y) {
        printf("   %s: picture not whole on rows %d-%d\n", what, want_y - 2,
               want_y);
        fail("gfx: chafa", "the picture is not whole on the screen");
    }
    if (term->curs.x != x0 + 5 || term->curs.y != want_y) {
        printf("   %s: cursor at %d,%d, want %d,%d\n", what, term->curs.x,
               term->curs.y, x0 + 5, want_y);
        fail("gfx: chafa", "the cursor did not move past the picture");
    }
    feed_seq(mk, "\r\n\033[?25h");
    if (term_gfx_visible(term, v, 4) != 1 || term->curs.x != 0 ||
        term->curs.y != v[0].r1 + 1) {
        printf("   %s: next line on row %d\n", what, term->curs.y);
        fail("gfx: chafa", "the next line is not below the picture");
    }
    sfree(px);
    sfree(b64);
}

static void test_kitty_graphics_chafa(Mock *mk)
{
    Terminal *term = mk->term;
    conf_set_bool(term->conf, CONF_kitty_graphics, true);
    term_size(term, 24, 80, 100);
    term_notify_cell_size_pixels(term, 8, 16);
    chafa_replay(mk, "row 6", 6);
    chafa_replay(mk, "row 23", 23);
    chafa_replay(mk, "bottom row", 24);
    feed_seq(mk, "\033c");
    term_notify_cell_size_pixels(term, 0, 0);
}

/*
 * KiTTY: iTerm2 inline images (OSC 1337 File=, MultipartFile=) through the
 * terminal: placed at the cursor into the kitty graphics store, the cursor
 * moved, nothing answered; the setting off; the ceiling raised only for
 * File= and FilePart=; other 1337 commands left alone. And the cursor move
 * after a placement at the bottom row, for both protocols: the screen
 * scrolls and the image is seen whole.
 */
static const unsigned char iterm_png_sig[8] =
    { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };

/* Stub decoders bound to the hooks: the real signature, then w and h (16
 * bits each, little endian); a red picture. */
static bool iterm_stub(const unsigned char *d, size_t len, size_t off,
                       unsigned char **px, int *w, int *h)
{
    size_t i, n;
    if (len < off + 4)
        return false;
    *w = d[off] | (d[off + 1] << 8);
    *h = d[off + 2] | (d[off + 3] << 8);
    if (*w <= 0 || *h <= 0)
        return false;
    n = (size_t)*w * *h;
    *px = malloc(n * 4);              /* the core frees it with free() */
    for (i = 0; i < n; i++) {
        (*px)[i * 4] = 0; (*px)[i * 4 + 1] = 0;
        (*px)[i * 4 + 2] = 255; (*px)[i * 4 + 3] = 255;
    }
    return true;
}

static bool iterm_stub_png(void *ctx, const unsigned char *d, size_t len,
                           unsigned char **px, int *w, int *h)
{
    (void)ctx;
    if (len < 8 || memcmp(d, iterm_png_sig, 8))
        return false;
    return iterm_stub(d, len, 8, px, w, h);
}

static bool iterm_stub_jpeg(void *ctx, int fmt, const unsigned char *d,
                            size_t len, unsigned char **px, int *w, int *h)
{
    (void)ctx;
    if (fmt != GFX_FILE_JPEG ||
        len < 3 || d[0] != 0xFF || d[1] != 0xD8 || d[2] != 0xFF)
        return false;
    return iterm_stub(d, len, 3, px, w, h);
}

/* Sends ESC ] 1337 ; <prefix><args>[:<base64 of a w x h stub PNG>] BEL
 * (no data part for w 0). */
static void iterm_send(Mock *mk, const char *prefix, const char *args,
                       int w, int h)
{
    char seq[512];
    unsigned char d[12];
    size_t len = (size_t)sprintf(seq, "\033]1337;%s%s", prefix, args);
    if (w > 0) {
        memcpy(d, iterm_png_sig, 8);
        d[8] = (unsigned char)w; d[9] = (unsigned char)(w >> 8);
        d[10] = (unsigned char)h; d[11] = (unsigned char)(h >> 8);
        seq[len++] = ':';
        len += gfx_b64(d, sizeof(d), seq + len);
    }
    seq[len++] = '\007';
    counters_reset();
    osc52_last_send[0] = '\0';
    term_data(mk->term, seq, len);
    term_update(mk->term);
}

/* The visible anonymous placement starting on screen row `row`, or NULL. */
static const GfxVisible *iterm_vis_at(Mock *mk, int row)
{
    static GfxVisible v[16];
    int n = term_gfx_visible(mk->term, v, 16), i;
    for (i = 0; i < n; i++)
        if (v[i].pl->image_id == 0 && v[i].r0 == row)
            return &v[i];
    return NULL;
}

static int iterm_count(Mock *mk)
{
    int i, n = 0;
    if (!mk->term->gfx)
        return 0;
    for (i = 0; i < mk->term->gfx->n_pls; i++)
        if (mk->term->gfx->pls[i].image_id == 0)
            n++;
    return n;
}

static void test_iterm_graphics(Mock *mk)
{
    Terminal *term = mk->term;
    const GfxVisible *v;
    int sb;

    conf_set_bool(term->conf, CONF_kitty_graphics, true);
    term_size(term, 24, 80, 100);
    term_notify_cell_size_pixels(term, 8, 16);
    feed_seq(mk, "\033c\033[r\033[2J\033[3J\033[H");
    kitty_gfx_decode_png_hook = iterm_stub_png;
    kitty_gfx_decode_file_hook = iterm_stub_jpeg;

    /* other 1337 commands: left alone, not even a store made */
    kitty_gfx_free(term);
    iterm_send(mk, "SetUserVar=", "foo=YmFy", 0, 0);
    iterm_send(mk, "CurrentDir=", "/tmp", 0, 0);
    if (term->gfx)
        fail("iterm: other commands", "a store was made for them");
    if (term->osc_str_limit != OSC_STR_MAX)
        fail("iterm: other commands", "their ceiling was raised");

    /* File=: a 16 x 32 image at row 5, column 3 - 2 x 2 cells, the cursor
     * after its right edge on its last row, nothing sent back */
    feed_seq(mk, "\033[5;3H");
    iterm_send(mk, "File=", "name=YS5wbmc=;size=12;inline=1", 16, 32);
    v = iterm_vis_at(mk, 4);
    if (!v || v->c0 != 2 || v->c1 != 3 || v->r1 != 5 || v->dx0 != 16 ||
        v->dy0 != 64 || v->dx1 != 32 || v->dy1 != 96)
        fail("iterm: File=", "the image is not at the cursor cell");
    if (term->curs.x != 4 || term->curs.y != 5)
        fail("iterm: File=", "the cursor did not move past the image");
    if (osc52_sends != 0)
        fail("iterm: File=", "something was sent back");
    if (term->osc_str_limit != OSC_STR_MAX_ITERM)
        fail("iterm: File=", "the ceiling was not raised");

    /* doNotMoveCursor */
    feed_seq(mk, "\033[10;10H");
    iterm_send(mk, "File=", "inline=1;doNotMoveCursor=1", 16, 16);
    if (!iterm_vis_at(mk, 9) || term->curs.x != 9 || term->curs.y != 9)
        fail("iterm: doNotMoveCursor", "the cursor moved");

    /* inline=0 is a download: nothing at all */
    iterm_send(mk, "File=", "inline=0", 16, 16);
    iterm_send(mk, "File=", "name=YS5wbmc=", 16, 16);
    if (iterm_count(mk) != 2 || term->curs.x != 9 || term->curs.y != 9 ||
        osc52_sends != 0)
        fail("iterm: inline=0", "something happened");

    /* multipart: only FilePart= gets the large ceiling */
    feed_seq(mk, "\033[12;1H");
    iterm_send(mk, "MultipartFile=", "inline=1", 0, 0);
    if (term->osc_str_limit != OSC_STR_MAX)
        fail("iterm: MultipartFile=", "its ceiling was raised");
    {
        unsigned char d[12];
        char b[32], part[64];
        memcpy(d, iterm_png_sig, 8);
        d[8] = 8; d[9] = 0; d[10] = 16; d[11] = 0;
        gfx_b64(d, sizeof(d), b);
        sprintf(part, "%.6s", b);
        iterm_send(mk, "FilePart=", part, 0, 0);
        if (term->osc_str_limit != OSC_STR_MAX_ITERM)
            fail("iterm: FilePart=", "the ceiling was not raised");
        iterm_send(mk, "FilePart=", b + 6, 0, 0);
        if (iterm_vis_at(mk, 11))
            fail("iterm: multipart", "placed before FileEnd");
        iterm_send(mk, "FileEnd", "", 0, 0);
        if (!iterm_vis_at(mk, 11) || term->curs.x != 1 || term->curs.y != 11)
            fail("iterm: multipart", "not placed at FileEnd");
    }

    /* the kitty deletions by position reach them, by id not */
    gfx_send(mk, "a=T,f=24,s=8,v=16,i=1", 8, 16);
    gfx_send(mk, "a=d,d=I,i=1", 0, 0);
    if (iterm_count(mk) != 3 || gfx_image_by_id(term->gfx, 1))
        fail("iterm: kitty d=I", "an iTerm2 image went, or the kitty one stayed");
    gfx_send(mk, "a=d", 0, 0);
    if (iterm_count(mk) != 0 || term->gfx->n_imgs != 0)
        fail("iterm: kitty d=a", "an iTerm2 image stayed");

    /* its setting off: ignored, the ceiling not raised */
    conf_set_bool(term->conf, CONF_iterm_images, false);
    feed_seq(mk, "\033[2;1H");
    iterm_send(mk, "File=", "inline=1", 16, 16);
    if (iterm_count(mk) != 0 || term->curs.x != 0 || term->curs.y != 1)
        fail("iterm: setting off", "the image was served");
    if (term->osc_str_limit != OSC_STR_MAX)
        fail("iterm: setting off", "the ceiling was raised");
    conf_set_bool(term->conf, CONF_iterm_images, true);
    /* the kitty protocol's setting off: iTerm2 images still served */
    conf_set_bool(term->conf, CONF_kitty_graphics, false);
    feed_seq(mk, "\033[2;1H");
    iterm_send(mk, "File=", "inline=1", 16, 16);
    if (iterm_count(mk) != 1)
        fail("iterm: kitty setting off", "the iTerm2 image was not served");
    gfx_send(mk, "a=d", 0, 0);         /* ignored: the kitty protocol is off */
    if (iterm_count(mk) != 1)
        fail("iterm: kitty setting off", "a kitty delete went through");
    conf_set_bool(term->conf, CONF_kitty_graphics, true);
    gfx_send(mk, "a=d", 0, 0);
    if (iterm_count(mk) != 0)
        fail("iterm: kitty setting on again", "a=d left the iTerm2 image");

    /* a JPEG, ended by ST */
    {
        char seq[128];
        unsigned char d[7] = { 0xFF, 0xD8, 0xFF, 8, 0, 16, 0 };
        size_t len = (size_t)sprintf(seq, "\033]1337;File=inline=1:");
        len += gfx_b64(d, sizeof(d), seq + len);
        seq[len++] = '\033';
        seq[len++] = '\\';
        feed_seq(mk, "\033[3;1H");
        term_data(term, seq, len);
        term_update(term);
        if (!iterm_vis_at(mk, 2) || term->curs.x != 1)
            fail("iterm: JPEG", "not placed");
        gfx_send(mk, "a=d", 0, 0);
    }

    /* over the ceiling: refused whole */
    {
        size_t big = OSC_STR_MAX_ITERM + 16, n;
        char *seq = snewn(big + 64, char);
        static const char head[] = "\033]1337;File=inline=1:";
        memcpy(seq, head, sizeof(head) - 1);
        n = sizeof(head) - 1;
        memset(seq + n, 'A', big);
        n += big;
        seq[n++] = '\007';
        term_data(term, seq, n);
        term_update(term);
        sfree(seq);
        if (iterm_count(mk) != 0 || term->gfx->n_imgs != 0)
            fail("iterm: ceiling", "a cut image was placed");
    }

    /* the bottom row: an image 5 rows tall placed on row 24 scrolls the
     * screen by 4, is seen whole on rows 20-24, the scrollback grows by 4 */
    feed_seq(mk, "\033[24;1H");
    sb = count234(term->scrollback);
    iterm_send(mk, "File=", "inline=1", 8, 80);
    v = iterm_vis_at(mk, 19);
    if (!v || v->r1 != 23 || v->dy0 != 19 * 16 || v->dy1 != 24 * 16)
        fail("iterm: bottom row", "the image is not whole on the screen");
    if (term->curs.y != 23 || term->curs.x != 1)
        fail("iterm: bottom row", "the cursor is not after the image");
    if (count234(term->scrollback) != sb + 4)
        fail("iterm: bottom row", "the screen did not scroll by 4");
    /* ... the same for the kitty protocol */
    feed_seq(mk, "\033[24;1H");
    sb = count234(term->scrollback);
    gfx_send(mk, "a=T,f=24,s=8,v=80,i=30", 8, 80);
    v = gfx_vis(mk, 30);
    if (!v || v->r0 != 19 || v->r1 != 23)
        fail("gfx: bottom row", "the image is not whole on the screen");
    if (term->curs.y != 23 || term->curs.x != 1 ||
        count234(term->scrollback) != sb + 4)
        fail("gfx: bottom row", "the screen did not scroll by 4");
    if (!iterm_vis_at(mk, 15))
        fail("gfx: bottom row", "the image above did not move up 4");
    /* ... C=1: the cursor stays, nothing scrolls */
    feed_seq(mk, "\033[24;1H");
    sb = count234(term->scrollback);
    gfx_send(mk, "a=T,f=24,s=8,v=80,i=31,C=1", 8, 80);
    if (term->curs.y != 23 || count234(term->scrollback) != sb)
        fail("gfx: bottom row, C=1", "the screen scrolled");
    gfx_send(mk, "a=d,d=A", 0, 0);

    /* the alternate screen: the region scroll carries them */
    feed_seq(mk, "\033[?1049h\033[24;1H");
    iterm_send(mk, "File=", "inline=1", 8, 80);
    v = iterm_vis_at(mk, 19);
    if (!v || v->r1 != 23 || term->curs.y != 23)
        fail("iterm: bottom row, alternate", "the image is not whole");
    feed_seq(mk, "\033[24;1H");
    gfx_send(mk, "a=T,f=24,s=8,v=80,i=32", 8, 80);
    v = gfx_vis(mk, 32);
    if (!v || v->r0 != 19 || v->r1 != 23 || !iterm_vis_at(mk, 15))
        fail("gfx: bottom row, alternate", "the images are not whole");
    feed_seq(mk, "\033[?1049l");

    /* no scrollback: the same */
    term_size(term, 24, 80, 0);
    feed_seq(mk, "\033[2J\033[24;1H");
    iterm_send(mk, "File=", "inline=1", 8, 80);
    v = iterm_vis_at(mk, 19);
    if (!v || v->r1 != 23 || term->curs.y != 23)
        fail("iterm: bottom row, no scrollback", "the image is not whole");
    term_size(term, 24, 80, 100);

    /* a reset empties it all */
    feed_seq(mk, "\033[5;1H");
    iterm_send(mk, "File=", "inline=1", 8, 16);
    feed_seq(mk, "\033c");
    if (term->gfx->n_pls != 0 || term->gfx->n_imgs != 0)
        fail("iterm: RIS", "the store was not emptied");

    kitty_gfx_decode_png_hook = NULL;
    kitty_gfx_decode_file_hook = NULL;
    term_notify_cell_size_pixels(term, 0, 0);
}

/*
 * KiTTY: Sixel (DCS q): streamed to the decoder, placed at the cursor as an
 * anonymous image, the cursor on its last row in the column it started in
 * (xterm; ?8452 l alone - chafa - keeps that), below it once ?7730 was set
 * or reset (mintty, mlterm, VTE, konsole; timg), ?8452 h right of it on its
 * last row, DECSDM at the top-left and the cursor stays; the bottom row
 * scrolling, the registers private or
 * shared (?1070), the setting off; DA1, XTSMGRAPHICS and XTVERSION.
 */
static void sixel_send(Mock *mk, const char *params, const char *data)
{
    size_t n = strlen(params) + strlen(data) + 8;
    char *seq = snewn(n, char);
    sprintf(seq, "\033P%sq%s\033\\", params, data);
    counters_reset();
    term_data(mk->term, seq, strlen(seq));
    term_update(mk->term);
    sfree(seq);
}

/* The BGRA pixel x, y of the image a visible placement shows. */
static const unsigned char *sixel_px(Mock *mk, const GfxVisible *v, int x,
                                     int y)
{
    const GfxImage *img = gfx_image_by_key(mk->term->gfx, v->pl->image_key);
    if (!img || x >= img->w || y >= img->h)
        return NULL;
    return img->px + ((size_t)y * img->w + x) * 4;
}

/* How many visible pictures have pixel 0,0 of this BGRA colour. */
static int sixel_count_colour(Mock *mk, int b, int g, int r)
{
    GfxVisible vis[16];
    int n = term_gfx_visible(mk->term, vis, 16), i, k = 0;
    for (i = 0; i < n; i++) {
        const unsigned char *px = sixel_px(mk, &vis[i], 0, 0);
        if (px && px[0] == b && px[1] == g && px[2] == r && px[3] == 255)
            k++;
    }
    return k;
}

static void test_sixel_graphics(Mock *mk)
{
    Terminal *term = mk->term;
    const GfxVisible *v;
    const unsigned char *px;
    int sb;

    conf_set_bool(term->conf, CONF_kitty_graphics, true);
    conf_set_bool(term->conf, CONF_sixel_images, true);
    term_size(term, 24, 80, 100);
    term_notify_cell_size_pixels(term, 8, 16);
    feed_seq(mk, "\033c\033[r\033[2J\033[3J\033[H");

    /* a 16 x 32 picture at row 5, column 3: 2 x 2 cells; ?8452 and ?7730
     * untouched: the cursor on its last row, in the column it started in
     * (xterm); nothing sent back */
    feed_seq(mk, "\033[5;3H");
    sixel_send(mk, "0;1;0", "\"1;1;16;32#1;2;100;0;0#1!16~");
    v = iterm_vis_at(mk, 4);
    if (!v || v->c0 != 2 || v->c1 != 3 || v->r1 != 5 || v->dx0 != 16 ||
        v->dy0 != 64 || v->dx1 != 32 || v->dy1 != 96)
        fail("sixel: place", "the picture is not at the cursor cell");
    if (term->curs.x != 2 || term->curs.y != 5)
        fail("sixel: place", "the cursor is not on the last row, same column");
    if (osc52_sends != 0)
        fail("sixel: place", "something was sent back");
    if (term->sixel)
        fail("sixel: place", "the decoder was left behind");
    /* its pixels: red drawn, P2=1 transparent below the sixel row */
    px = v ? sixel_px(mk, v, 0, 0) : NULL;
    if (!px || px[0] != 0 || px[1] != 0 || px[2] != 255 || px[3] != 255)
        fail("sixel: place", "the drawn pixel is not red");
    px = v ? sixel_px(mk, v, 15, 31) : NULL;
    if (!px || px[3] != 0)
        fail("sixel: place", "an undrawn pixel is not transparent with P2=1");
    /* P2 = 0: undrawn pixels are register 0, opaque */
    feed_seq(mk, "\033[8;1H");
    sixel_send(mk, "", "\"1;1;8;8@");
    v = iterm_vis_at(mk, 7);
    px = v ? sixel_px(mk, v, 7, 7) : NULL;
    if (!px || px[3] != 255 || px[0] || px[1] || px[2])
        fail("sixel: P2=0", "the background is not register 0");
    gfx_send(mk, "a=d", 0, 0);

    /* ?8452: right of the picture on its last row ... */
    feed_seq(mk, "\033[?8452h\033[10;10H");
    sixel_send(mk, "0;1", "\"1;1;16;32~");
    if (!iterm_vis_at(mk, 9) || term->curs.x != 11 || term->curs.y != 10)
        fail("sixel: ?8452", "the cursor is not right of the picture");
    /* ... and past the right edge the left edge of the next row */
    feed_seq(mk, "\033[1;79H");
    sixel_send(mk, "0;1", "\"1;1;16;32~");
    if (!iterm_vis_at(mk, 0) || term->curs.x != 0 || term->curs.y != 2)
        fail("sixel: ?8452 edge", "the cursor did not go to the next row");
    /* ... and it wins over ?7730 */
    feed_seq(mk, "\033[?7730h\033[10;10H");
    sixel_send(mk, "0;1", "\"1;1;16;32~");
    if (term->curs.x != 11 || term->curs.y != 10)
        fail("sixel: ?8452 and ?7730", "?8452 did not win");
    feed_seq(mk, "\033[?8452l");
    if (!term->sixel_7730_seen)
        fail("sixel: modes", "setting ?7730 was not noticed");
    /* ?7730 alone: the line below, at its start */
    feed_seq(mk, "\033[14;10H");
    sixel_send(mk, "0;1", "\"1;1;16;32~");
    if (!iterm_vis_at(mk, 13) || term->curs.x != 0 || term->curs.y != 15)
        fail("sixel: ?7730", "the cursor is not at the start of the line below");
    /* RIS resets it, and the untouched rule is back: the last row */
    feed_seq(mk, "\033c\033[3;5H");
    if (term->sixel_cursor_line_start || term->sixel_7730_seen)
        fail("sixel: ?7730 RIS", "the modes survived");
    sixel_send(mk, "0;1", "\"1;1;16;32~");
    if (term->curs.x != 4 || term->curs.y != 3)
        fail("sixel: RIS", "the cursor is not on the last row, same column");
    /* ?8452 l alone (chafa, before every picture): still xterm's rule, the
     * last row, the column it started in */
    feed_seq(mk, "\033[?8452l\033[8;5H");
    sixel_send(mk, "0;1", "\"1;1;16;32~");
    if (term->sixel_7730_seen || term->curs.x != 4 || term->curs.y != 8)
        fail("sixel: ?8452 l", "the cursor is not on the last row, same column");
    /* ?7730 l alone (after a reset): below, at the left column */
    feed_seq(mk, "\033c\033[?7730l\033[8;5H");
    sixel_send(mk, "0;1", "\"1;1;16;32~");
    if (term->curs.x != 4 || term->curs.y != 9)
        fail("sixel: ?7730 l", "the cursor is not below, at the left column");
    /* ?7730 h alone (after a reset): below, at column 0 */
    feed_seq(mk, "\033c\033[?7730h\033[8;5H");
    sixel_send(mk, "0;1", "\"1;1;16;32~");
    if (term->curs.x != 0 || term->curs.y != 9)
        fail("sixel: ?7730 h", "the cursor is not below, at column 0");
    /* timg: ?8452 l ?7730 h - mintty's rule */
    feed_seq(mk, "\033c\033[?8452l\033[?7730h\033[8;5H");
    sixel_send(mk, "0;1", "\"1;1;16;32~");
    if (term->curs.x != 0 || term->curs.y != 9)
        fail("sixel: timg modes", "the cursor is not below, at column 0");
    feed_seq(mk, "\033c");

    /* DECSDM: the top-left of the screen, the cursor stays, no scrolling */
    feed_seq(mk, "\033[?80h\033[12;12H");
    sixel_send(mk, "0;1", "\"1;1;16;32~");
    v = iterm_vis_at(mk, 0);
    if (!v || v->c0 != 0 || term->curs.x != 11 || term->curs.y != 11)
        fail("sixel: DECSDM", "not at the top-left, or the cursor moved");
    feed_seq(mk, "\033[24;1H");
    sb = count234(term->scrollback);
    sixel_send(mk, "0;1", "\"1;1;8;80~");
    if (term->curs.y != 23 || count234(term->scrollback) != sb)
        fail("sixel: DECSDM", "the screen scrolled");
    feed_seq(mk, "\033[?80l");
    gfx_send(mk, "a=d", 0, 0);

    /* the bottom row, modes untouched: 5 rows tall on row 24 scrolls by
     * 4, the picture fits, the cursor on its last row */
    feed_seq(mk, "\033[24;1H");
    sb = count234(term->scrollback);
    sixel_send(mk, "0;1", "\"1;1;8;80~");
    v = iterm_vis_at(mk, 19);
    if (!v || v->r1 != 23 || term->curs.y != 23 || term->curs.x != 0)
        fail("sixel: bottom row, xterm", "the picture is not whole");
    if (count234(term->scrollback) != sb + 4)
        fail("sixel: bottom row, xterm", "the screen did not scroll by 4");
    /* in the middle: the cursor on the last row */
    feed_seq(mk, "\033[10;4H");
    sixel_send(mk, "0;1", "\"1;1;8;80~");
    if (term->curs.y != 13 || term->curs.x != 3)
        fail("sixel: middle, xterm", "the cursor is not on the last row");
    gfx_send(mk, "a=d", 0, 0);
    /* ?7730 touched: 5 rows tall on row 24 scrolls by 5 - the picture and
     * the cursor line below it both on the screen */
    feed_seq(mk, "\033[?7730l\033[24;1H");
    sb = count234(term->scrollback);
    sixel_send(mk, "0;1", "\"1;1;8;80~");
    v = iterm_vis_at(mk, 18);
    if (!v || v->r1 != 22 || term->curs.y != 23 || term->curs.x != 0)
        fail("sixel: bottom row", "the picture and the cursor line do not fit");
    if (count234(term->scrollback) != sb + 5)
        fail("sixel: bottom row", "the screen did not scroll by 5");
    /* in the middle of the screen: no scrolling, the cursor below it */
    feed_seq(mk, "\033[10;4H");
    sb = count234(term->scrollback);
    sixel_send(mk, "0;1", "\"1;1;8;80~");
    if (!iterm_vis_at(mk, 9) || term->curs.y != 14 || term->curs.x != 3 ||
        count234(term->scrollback) != sb)
        fail("sixel: middle", "the cursor is not on the line below");
    gfx_send(mk, "a=d", 0, 0);

    /* streamed: far past OSC_STR_MAX, the ceiling not raised, nothing in
     * the string buffer; CR/LF inside do not end it */
    {
        size_t big = OSC_STR_MAX * 3;
        char *data = snewn(big + 16, char);
        memset(data, ' ', big);
        memcpy(data, "~\r\n", 3);
        strcpy(data + big - 4, "~~\r\n");
        feed_seq(mk, "\033[3;1H");
        sixel_send(mk, "0;1", data);
        sfree(data);
        v = iterm_vis_at(mk, 2);
        if (!v || v->dx1 - v->dx0 != 3)
            fail("sixel: streamed", "the long picture was not placed");
        if (term->osc_str_limit != OSC_STR_MAX ||
            term->osc_strsize > OSC_STR_MAX + 1 || term->osc_strlen != 0)
            fail("sixel: streamed", "the OSC buffer was used or grown");
        gfx_send(mk, "a=d", 0, 0);
    }

    /* abandoned by an ESC not followed by a backslash: nothing placed, the
     * sequence after it served */
    feed_seq(mk, "\033[5;1H\033Pq~~\033[H");
    if (iterm_count(mk) != 0 || term->sixel || term->curs.y != 0)
        fail("sixel: abandoned", "a half picture was placed or kept");

    /* not Sixel: DECRQSS, four parameters, something before the q */
    feed_seq(mk, "\033[5;1H");
    sixel_send(mk, "$", "m");
    sixel_send(mk, "1;2;3;4", "~");
    sixel_send(mk, "+", "~");
    sixel_send(mk, ">", "~");
    if (iterm_count(mk) != 0 || term->sixel || osc52_sends != 0 ||
        term->osc_str_limit != OSC_STR_MAX)
        fail("sixel: other DCS", "a DCS that is not Sixel was served");

    /* over the store's cap: nothing placed, the cursor stays */
    term->gfx->max_side = 16;
    feed_seq(mk, "\033[5;1H");
    sixel_send(mk, "", "!17~$-~~~~~");
    if (iterm_count(mk) != 0 || term->curs.y != 4 || term->curs.x != 0)
        fail("sixel: cap", "a picture over the cap was placed");
    term->gfx->max_side = GFX_MAX_SIDE;

    /* registers: private by default, shared with ?1070 reset */
    feed_seq(mk, "\033[2;1H");
    sixel_send(mk, "0;1", "#5;2;100;0;0#5~");
    feed_seq(mk, "\033[2;5H");
    sixel_send(mk, "0;1", "#5~");
    if (sixel_count_colour(mk, 0, 0, 255) != 1 ||
        sixel_count_colour(mk, 0xCC, 0xCC, 0x33) != 1)
        fail("sixel: private registers", "a define outlived its picture");
    gfx_send(mk, "a=d", 0, 0);
    feed_seq(mk, "\033[?1070l\033[2;1H");
    sixel_send(mk, "0;1", "#5;2;100;0;0#5~");
    feed_seq(mk, "\033[2;5H");
    sixel_send(mk, "0;1", "#5~");
    if (sixel_count_colour(mk, 0, 0, 255) != 2)
        fail("sixel: shared registers", "the define was not kept");
    /* RIS: private again, the shared registers back to the VT340's */
    feed_seq(mk, "\033c");
    if (term->sixel_shared_regs || term->sixel_palette || term->sixel_decsdm ||
        term->sixel_scrolls_right || term->sixel_cursor_line_start ||
        term->gfx->n_pls)
        fail("sixel: RIS", "the modes or the registers survived");

    /* XTSMGRAPHICS */
    expect_reply(mk, "XTSMGRAPHICS colours read", "\033[?1;1;0S",
                 "\033[?1;0;256S");
    expect_reply(mk, "XTSMGRAPHICS colours reset", "\033[?1;2;0S",
                 "\033[?1;0;256S");
    expect_reply(mk, "XTSMGRAPHICS colours max", "\033[?1;4;0S",
                 "\033[?1;0;256S");
    expect_reply(mk, "XTSMGRAPHICS colours set", "\033[?1;3;16S",
                 "\033[?1;3S");
    expect_reply(mk, "XTSMGRAPHICS colours bad Pa", "\033[?1;7;0S",
                 "\033[?1;2S");
    expect_reply(mk, "XTSMGRAPHICS geometry read", "\033[?2;1;0S",
                 "\033[?2;0;640;384S");
    expect_reply(mk, "XTSMGRAPHICS geometry max", "\033[?2;4;0S",
                 "\033[?2;0;4096;4096S");
    expect_reply(mk, "XTSMGRAPHICS geometry set", "\033[?2;3;100S",
                 "\033[?2;3S");
    expect_reply(mk, "XTSMGRAPHICS geometry reset", "\033[?2;2;0S",
                 "\033[?2;3S");
    expect_reply(mk, "XTSMGRAPHICS ReGIS", "\033[?3;1;0S", "\033[?3;1S");
    expect_silence(mk, "XTSMGRAPHICS two parameters", "\033[?1;1S");
    expect_silence(mk, "XTSMGRAPHICS four parameters", "\033[?1;1;0;0S");

    /* XTVERSION: the product and its version, without the suffix */
    {
        char want[128];
        const char *paren = strstr(ver, " (");
        int n = paren ? (int)(paren - ver) : (int)strlen(ver);
        sprintf(want, "\033P>|KiTTY++ %.*s\033\\", n, ver);
        expect_reply(mk, "XTVERSION", "\033[>q", want);
        expect_reply(mk, "XTVERSION 0", "\033[>0q", want);
        expect_silence(mk, "XTVERSION 1", "\033[>1q");
        expect_silence(mk, "XTVERSION two parameters", "\033[>0;0q");
        if (strchr(want, '(') || !strstr(want, "KiTTY++ 0."))
            fail("XTVERSION", "the version is not the bare number");
    }

    /* DA1: Sixel reported with the setting on */
    if (strcmp(term_da1_answer(term), "\033[?62;4;22c"))
        fail("DA1 on", "Sixel is not reported");

    /* mintty's ?7780: no scrolling - the picture cut at the bottom margin,
     * the cursor stays; both Sixel and iTerm2; RIS resets it */
    kitty_gfx_decode_png_hook = iterm_stub_png;
    feed_seq(mk, "\033[?7780h\033[22;3H");
    sb = count234(term->scrollback);
    sixel_send(mk, "0;1", "\"1;1;8;80~");
    v = iterm_vis_at(mk, 21);
    if (!v || v->r1 != 23 || v->dy1 != 24 * 16 || v->sh != 48 ||
        term->curs.x != 2 || term->curs.y != 21 ||
        count234(term->scrollback) != sb)
        fail("sixel: ?7780", "scrolled, moved the cursor or not cut");
    gfx_send(mk, "a=d", 0, 0);
    iterm_send(mk, "File=", "inline=1", 8, 80);
    v = iterm_vis_at(mk, 21);
    if (!v || v->r1 != 23 || v->dy1 != 24 * 16 || v->sh != 48 ||
        term->curs.x != 2 || term->curs.y != 21 ||
        count234(term->scrollback) != sb)
        fail("iterm: ?7780", "scrolled, moved the cursor or not cut");
    gfx_send(mk, "a=d", 0, 0);
    /* a scroll region: cut at its bottom margin, row 15 */
    feed_seq(mk, "\033[5;15r\033[13;1H");
    sixel_send(mk, "0;1", "\"1;1;8;80~");
    v = iterm_vis_at(mk, 12);
    if (!v || v->r1 != 14 || v->dy1 != 15 * 16 || term->curs.y != 12)
        fail("sixel: ?7780 region", "not cut at the bottom margin");
    gfx_send(mk, "a=d", 0, 0);
    iterm_send(mk, "File=", "inline=1", 8, 80);
    v = iterm_vis_at(mk, 12);
    if (!v || v->r1 != 14 || v->dy1 != 15 * 16 || term->curs.y != 12)
        fail("iterm: ?7780 region", "not cut at the bottom margin");
    gfx_send(mk, "a=d", 0, 0);
    feed_seq(mk, "\033[r");
    /* the kitty protocol is not affected */
    feed_seq(mk, "\033[24;1H");
    sb = count234(term->scrollback);
    gfx_send(mk, "a=T,f=24,s=8,v=80,i=40", 8, 80);
    if (count234(term->scrollback) != sb + 4 || term->curs.y != 23)
        fail("gfx: ?7780", "the kitty protocol did not scroll");
    gfx_send(mk, "a=d,d=A", 0, 0);
    /* reset: scrolls as before */
    feed_seq(mk, "\033[?7780l\033[24;1H");
    sb = count234(term->scrollback);
    iterm_send(mk, "File=", "inline=1", 8, 80);
    if (count234(term->scrollback) != sb + 4 || term->curs.y != 23)
        fail("iterm: ?7780 reset", "did not scroll");
    gfx_send(mk, "a=d", 0, 0);
    feed_seq(mk, "\033[?7780h\033c");
    if (term->gfx_no_scroll)
        fail("?7780 RIS", "the mode survived");
    kitty_gfx_decode_png_hook = NULL;

    /* the setting off: DCS q ignored, DA1 the VT102 answer, XTSMGRAPHICS
     * silent */
    conf_set_bool(term->conf, CONF_sixel_images, false);
    feed_seq(mk, "\033[6;1H");
    sixel_send(mk, "0;1", "\"1;1;16;32~");
    if (iterm_count(mk) != 0 || term->sixel || term->curs.y != 5 ||
        term->curs.x != 0)
        fail("sixel: setting off", "the picture was served");
    if (strcmp(term_da1_answer(term), "\033[?6c"))
        fail("DA1 off", "the answer is not the VT102 one");
    expect_silence(mk, "XTSMGRAPHICS off", "\033[?1;1;0S");
    conf_set_bool(term->conf, CONF_sixel_images, true);

    /* an answer set by DECSCL 50 wins, on or off */
    feed_seq(mk, "\033[50;1;2\"p");
    if (strcmp(term_da1_answer(term), "\033[?1;2c"))
        fail("DA1 CSI 50", "the custom answer does not win");
    conf_set_bool(term->conf, CONF_sixel_images, false);
    if (strcmp(term_da1_answer(term), "\033[?1;2c"))
        fail("DA1 CSI 50 off", "the custom answer does not win");
    conf_set_bool(term->conf, CONF_sixel_images, true);
    strcpy(term->id_string, "\033[?6c");
    term->da1_custom = false;

    feed_seq(mk, "\033c");
    term_notify_cell_size_pixels(term, 0, 0);
}

/*
 * KiTTY: the iTerm2 and Sixel pictures belong to their cells (xterm,
 * mintty, iTerm2): text written into a covered cell, and an erase of it,
 * cut that cell out of the picture - the painters' pieces leave it to the
 * text - and a picture with nothing left goes. Kitty placements stay whole.
 */
static int cut_pieces(Mock *mk, GfxVisible *v, int max)
{
    int n = term_gfx_visible(mk->term, v, max), i, k = 0;
    for (i = 0; i < n; i++)
        if (!v[i].pl->image_id)
            v[k++] = v[i];
    return k;
}

/* Whether a piece of a picture covers screen cell (x, y). */
static bool cut_over(const GfxVisible *v, int n, int x, int y)
{
    int i;
    for (i = 0; i < n; i++)
        if (v[i].c0 <= x && x <= v[i].c1 && v[i].r0 <= y && y <= v[i].r1)
            return true;
    return false;
}

static const GfxPlacement *cut_pl(Mock *mk)
{
    int i;
    for (i = 0; mk->term->gfx && i < mk->term->gfx->n_pls; i++)
        if (!mk->term->gfx->pls[i].image_id)
            return &mk->term->gfx->pls[i];
    return NULL;
}

static void test_cell_bound_pictures(Mock *mk)
{
    Terminal *term = mk->term;
    GfxVisible v[64];
    const GfxPlacement *pl;
    const GfxVisible *kv;
    int n, x;

    conf_set_bool(term->conf, CONF_kitty_graphics, true);
    conf_set_bool(term->conf, CONF_sixel_images, true);
    term_size(term, 24, 80, 100);
    term_notify_cell_size_pixels(term, 8, 16);
    feed_seq(mk, "\033c\033[r\033[2J\033[3J\033[H");

    /* a 32 x 48 Sixel picture at row 5, column 3: rows 4..6, columns
     * 2..5; text written into its 2nd row cuts those cells out */
    feed_seq(mk, "\033[5;3H");
    sixel_send(mk, "0;1", "\"1;1;32;48#1;2;100;0;0#1!32~");
    pl = cut_pl(mk);
    if (!pl || !pl->cell_bound || pl->cols != 4 || pl->rows != 3 ||
        term->gfx->n_cellbound != 1)
        fail("cell-bound: Sixel", "not placed as a 4 x 3 cell-bound picture");
    feed_seq(mk, "\033[6;3HAB");
    pl = cut_pl(mk);
    if (!pl || pl->n_marked != 2 || !gfx_cell_marked(pl, 0, 1) ||
        !gfx_cell_marked(pl, 1, 1) || gfx_cell_marked(pl, 2, 1))
        fail("cell-bound: text", "the written cells are not cut out");
    n = cut_pieces(mk, v, 64);
    if (n != 3 || cut_over(v, n, 2, 5) || cut_over(v, n, 3, 5) ||
        !cut_over(v, n, 4, 5) || !cut_over(v, n, 2, 4) ||
        !cut_over(v, n, 5, 6))
        fail("cell-bound: text", "the pieces do not leave the text alone");
    /* a wide character through the ordinary writer: both its cells */
    feed_seq(mk, "\033[5;5H\xe4\xb8\xad");
    pl = cut_pl(mk);
    if (!pl || !gfx_cell_marked(pl, 2, 0) || !gfx_cell_marked(pl, 3, 0) ||
        pl->n_marked != 4)
        fail("cell-bound: wide", "the wide character's cells not cut out");
    /* EL on its last row from column 5: its cells 2 and 3 there */
    feed_seq(mk, "\033[7;5H\033[K");
    pl = cut_pl(mk);
    if (!pl || !gfx_cell_marked(pl, 2, 2) || !gfx_cell_marked(pl, 3, 2) ||
        gfx_cell_marked(pl, 1, 2) || pl->n_marked != 6)
        fail("cell-bound: EL", "the erased cells not cut out");
    /* ECH of a blank cell: nothing changes in the text, the cell is still
     * drawn again (the picture's pixels must not stay there) */
    term_update(term);
    term_data(term, "\033[7;3H\033[X", 9);
    pl = cut_pl(mk);
    if (!pl || !gfx_cell_marked(pl, 0, 2) ||
        !(term->disptext[6]->chars[2].attr & ATTR_INVALID))
        fail("cell-bound: ECH", "not cut out, or the cell not drawn again");
    term_update(term);
    /* ED 2: gone */
    feed_seq(mk, "\033[2J");
    if (iterm_count(mk) != 0 || term->gfx->n_cellbound != 0)
        fail("cell-bound: ED 2", "the picture survived");

    /* the user's case: ?7780, the cursor stays on the picture, the prompt
     * written over its first row */
    feed_seq(mk, "\033[?7780h\033[3;1H");
    sixel_send(mk, "0;1", "\"1;1;32;48#1;2;100;0;0#1!32~");
    if (term->curs.y != 2 || term->curs.x != 0 || !cut_pl(mk))
        fail("cell-bound: ?7780", "not placed, or the cursor moved");
    feed_seq(mk, "root@automata:~# ");
    n = cut_pieces(mk, v, 64);
    for (x = 0; x < 4; x++)
        if (cut_over(v, n, x, 2))
            break;
    if (x < 4 || n != 1 || !cut_over(v, n, 0, 3) || !cut_over(v, n, 3, 4))
        fail("cell-bound: ?7780 prompt", "the prompt is under the picture");
    feed_seq(mk, "\033[?7780l\033[2J");

    /* an iTerm2 picture the same; DL inside it cuts the rows that moved */
    kitty_gfx_decode_png_hook = iterm_stub_png;
    feed_seq(mk, "\033[10;1H");
    iterm_send(mk, "File=", "inline=1", 32, 48);
    pl = cut_pl(mk);
    if (!pl || !pl->cell_bound || pl->abs_line - kitty_gfx_top_abs(term) != 9)
        fail("cell-bound: iTerm2", "not placed cell-bound at row 10");
    feed_seq(mk, "\033[11;1Hxy");
    pl = cut_pl(mk);
    if (!pl || pl->n_marked != 2 || !gfx_cell_marked(pl, 1, 1))
        fail("cell-bound: iTerm2 text", "the written cells not cut out");
    feed_seq(mk, "\033[12;1H\033[M");
    pl = cut_pl(mk);
    if (!pl || pl->n_marked != 6 || !gfx_cell_marked(pl, 3, 2) ||
        gfx_cell_marked(pl, 3, 0))
        fail("cell-bound: DL", "the moved row not cut out");
    /* ICH on its first row: from the cursor to its right edge */
    feed_seq(mk, "\033[10;3H\033[@");
    pl = cut_pl(mk);
    if (!pl || !gfx_cell_marked(pl, 2, 0) || !gfx_cell_marked(pl, 3, 0) ||
        gfx_cell_marked(pl, 1, 0))
        fail("cell-bound: ICH", "the shifted cells not cut out");
    /* the rest erased: the picture and its image go */
    feed_seq(mk, "\033[10;1H\033[2K\033[11;1H\033[2K");
    if (iterm_count(mk) != 0 || term->gfx->n_imgs != 0 ||
        term->gfx->n_cellbound != 0)
        fail("cell-bound: all erased", "the picture or its image stayed");
    kitty_gfx_decode_png_hook = NULL;

    /* a video (timg): frame after frame at one place - each replaces the
     * last, the store keeps one */
    {
        int f;
        bool ok = true;
        for (f = 0; f < 300 && ok; f++) {
            feed_seq(mk, "\033[2;1H");
            sixel_send(mk, "0;1", "\"1;1;64;64#1;2;100;0;0#1!64~");
            ok = iterm_count(mk) == 1 && term->gfx->n_imgs == 1;
        }
        if (!ok)
            fail("cell-bound: video", "the frames piled up in the store");
        feed_seq(mk, "\033[2J");
    }

    /* a kitty placement under the same writes stays whole */
    feed_seq(mk, "\033[15;1H");
    gfx_send(mk, "a=T,f=24,s=32,v=48,i=7,C=1", 32, 48);
    feed_seq(mk, "\033[16;1Habc\033[17;1H\033[K\033[15;2H\033[X");
    kv = gfx_vis(mk, 7);
    if (!kv || kv->c0 != 0 || kv->c1 != 3 || kv->r0 != 14 || kv->r1 != 16 ||
        kv->pl->mask || kv->pl->cell_bound)
        fail("cell-bound: kitty", "the kitty placement was cut");
    gfx_send(mk, "a=d,d=A", 0, 0);

    feed_seq(mk, "\033c");
    term_notify_cell_size_pixels(term, 0, 0);
}

static void test_write_confirm(Mock *mk)
{
    static const char *const M = "type=wdata:mime=dGV4dC9wbGFpbg==";  /* text/plain */

    /* OSC 52, Yes: nothing before the answer, ONE box for two writes, then the
     * LATEST payload (it would have overwritten the first), and Allow latched */
    wc_reset(mk);
    feed_seq(mk, "\033]52;c;SGVsbG8=\007");                            /* Hello */
    if (osc52_wconfirms != 1)
        fail("confirm OSC 52", "the box was not opened exactly once");
    if (mk->clip_writes != 0)
        fail("confirm OSC 52", "the clipboard was set before the answer");
    feed_seq(mk, "\033]52;c;V29ybGQ=\007");                            /* World */
    if (osc52_wconfirms != 1)
        fail("confirm OSC 52", "a second write opened a second box");
    term_osc52_write_answer(mk->term, true);
    if (mk->clip_writes != 1 || !mk->clip || wcscmp(mk->clip, L"World"))
        fail("confirm OSC 52, Yes", "the latest payload did not land exactly once");
    if (mk->term->osc52_allowed != OSC52_CLIPBOARD_ALLOW)
        fail("confirm OSC 52, Yes", "Allow was not latched");
    mk->clip_writes = 0;
    feed_seq(mk, "\033]52;c;SGVsbG8=\007");
    if (mk->clip_writes != 1 || osc52_wconfirms != 1)
        fail("confirm OSC 52, Yes", "the next write did not go through without a box");

    /* OSC 52, No: dropped, Deny latched, the next write refused without a box */
    wc_reset(mk);
    feed_seq(mk, "\033]52;c;SGVsbG8=\007");
    term_osc52_write_answer(mk->term, false);
    if (mk->clip_writes != 0)
        fail("confirm OSC 52, No", "the refused payload reached the clipboard");
    if (mk->term->osc52_allowed != OSC52_CLIPBOARD_DENY)
        fail("confirm OSC 52, No", "Deny was not latched");
    feed_seq(mk, "\033]52;c;SGVsbG8=\007");
    if (mk->clip_writes != 0 || osc52_wconfirms != 1)
        fail("confirm OSC 52, No", "the next write was set, or opened another box");

    /* OSC 5522, Yes after the end: the whole write is in, its reply waits */
    wc_reset(mk);
    feed_5522_raw(mk, "type=write:id=c1", NULL);
    feed_5522_raw(mk, M, "SGVsbG8=");
    feed_5522_raw(mk, "type=wdata", NULL);
    if (osc52_sends != 0 || osc52_set_calls != 0)
        fail("confirm 5522", "answered or set before the box was");
    term_osc52_write_answer(mk->term, true);
    if (osc52_sends != 1)
        fail("confirm 5522, Yes", "expected exactly one reply, DONE");
    expect_last(mk, "confirm 5522, Yes", "type=write:status=DONE:id=c1");
    if (osc52_set_calls != 1 || strcmp(osc52_set_text, "Hello"))
        fail("confirm 5522, Yes", "the payload did not land exactly once");

    /* OSC 5522, No after the end: EPERM, nothing set */
    wc_reset(mk);
    feed_5522_raw(mk, "type=write:id=c2", NULL);
    feed_5522_raw(mk, M, "SGVsbG8=");
    feed_5522_raw(mk, "type=wdata", NULL);
    term_osc52_write_answer(mk->term, false);
    expect_last(mk, "confirm 5522, No", "type=write:status=EPERM:id=c2");
    if (osc52_set_calls != 0)
        fail("confirm 5522, No", "the refused write reached the clipboard");

    /* OSC 5522, Yes while it still streams: it goes on and ends as usual */
    wc_reset(mk);
    feed_5522_raw(mk, "type=write:id=c3", NULL);
    feed_5522_raw(mk, M, "SGVs");
    term_osc52_write_answer(mk->term, true);
    if (osc52_sends != 0)
        fail("confirm 5522, Yes mid-write", "answered before the write ended");
    feed_5522_raw(mk, M, "bG8=");
    feed_5522_raw(mk, "type=wdata", NULL);
    expect_last(mk, "confirm 5522, Yes mid-write", "type=write:status=DONE:id=c3");
    if (strcmp(osc52_set_text, "Hello"))
        fail("confirm 5522, Yes mid-write", "the chunks around the answer did not join");

    /* OSC 5522, No while it still streams: EPERM at once, the rest ignored */
    wc_reset(mk);
    feed_5522_raw(mk, "type=write:id=c4", NULL);
    term_osc52_write_answer(mk->term, false);
    expect_last(mk, "confirm 5522, No mid-write", "type=write:status=EPERM:id=c4");
    counters_reset();
    feed_5522_raw(mk, M, "SGVsbG8=");
    feed_5522_raw(mk, "type=wdata", NULL);
    if (osc52_sends != 0 || osc52_set_calls != 0)
        fail("confirm 5522, No mid-write", "packets of the refused write were not ignored");

    /* A read behind the write waits and runs AFTER it: DONE first, then the
     * clipboard goes out (reads allowed here without their own dialog) */
    wc_reset(mk);
    mk->term->osc52_read_decision = 1;
    mk->term->osc52_read_remaining = -1;
    feed_5522_raw(mk, "type=write:id=c5", NULL);
    feed_5522_raw(mk, M, "SGVsbG8=");
    feed_5522_raw(mk, "type=wdata", NULL);
    feed_seq(mk, READ_SEQ);
    if (osc52_sends != 0)
        fail("confirm, read behind the write", "the read was served before the answer");
    term_osc52_write_answer(mk->term, true);
    {
        const char *done = strstr(osc52_all, "status=DONE");
        const char *read = strstr(osc52_all, "\033]52;");
        if (osc52_sends != 2 || !done || !read || read < done)
            fail("confirm, read behind the write", "expected DONE, then the read's reply");
    }

    /* Only one read waits; an OSC 5522 read beyond it is EBUSY */
    wc_reset(mk);
    feed_seq(mk, "\033]52;c;SGVsbG8=\007");
    feed_5522_raw(mk, "type=read:id=r6", "dGV4dC9wbGFpbg==");
    feed_5522_raw(mk, "type=read:id=r7", "dGV4dC9wbGFpbg==");
    if (osc52_sends != 1)
        fail("confirm, two reads behind the write", "expected one EBUSY, the first read waiting");
    expect_last(mk, "confirm, two reads behind the write", "type=read:id=r7:status=EBUSY");
    term_osc52_write_answer(mk->term, false);

    /* A setting changed meanwhile wins: Ask became Deny, then Yes - nothing set */
    wc_reset(mk);
    feed_seq(mk, "\033]52;c;SGVsbG8=\007");
    mk->term->osc52_allowed = OSC52_CLIPBOARD_DENY;
    term_osc52_write_answer(mk->term, true);
    if (mk->clip_writes != 0 || mk->term->osc52_allowed != OSC52_CLIPBOARD_DENY)
        fail("confirm, setting changed meanwhile", "the stale Yes overrode the new setting");

    /* An answer with no question open does nothing */
    wc_reset(mk);
    term_osc52_write_answer(mk->term, true);
    if (mk->term->osc52_allowed != OSC52_CLIPBOARD_ASK)
        fail("confirm, stray answer", "an answer without a box changed the setting");
    mk->term->osc52_allowed = OSC52_CLIPBOARD_ALLOW;
}

/*
 * The read box is modeless too: the request waits for the answer while the
 * parser goes on, an Allow sends what the box SHOWED (not whatever is on the
 * clipboard by then), a setting changed while it was open wins, and a second
 * request meanwhile is "not now" while the first keeps waiting.
 */
static void test_read_box_modeless(Mock *mk)
{
    osc52_read_box_manual = true;

    /* waits, then sends the clipboard as it was shown: "secret" = c2VjcmV0 */
    read_reset(mk);
    counters_reset();
    term_data(mk->term, READ_SEQ, strlen(READ_SEQ));
    term_update(mk->term);
    if (osc52_dialogs != 1 || osc52_sends != 0)
        fail("read box", "expected the box open and nothing sent yet");
    stub_clip = L"copied-meanwhile";
    osc52_read_box_open = false;
    term_osc52_read_answer(mk->term, true, GRANT_ONCE, false);
    if (osc52_sends != 1 || !strstr(osc52_last_send, "c2VjcmV0"))
        fail("read box, Allow", "did not send exactly the clipboard the box showed");

    /* reads set to Deny while the box was open: the Allow sends nothing */
    read_reset(mk);
    counters_reset();
    term_data(mk->term, READ_SEQ, strlen(READ_SEQ));
    conf_set_int(mk->term->conf, CONF_osc52_clipboard_read, OSC52_READ_DENY);
    osc52_read_box_open = false;
    term_osc52_read_answer(mk->term, true, GRANT_ONCE, false);
    if (osc52_sends != 0)
        fail("read box, setting changed meanwhile", "the stale Allow sent the clipboard");

    /* a second request while it is open: EBUSY; the first is served after */
    read_reset(mk);
    counters_reset();
    feed_5522_raw(mk, "type=read:id=q1", "dGV4dC9wbGFpbg==");
    feed_5522_raw(mk, "type=read:id=q2", "dGV4dC9wbGFpbg==");
    if (osc52_dialogs != 1)
        fail("read box, second request", "a second box was opened");
    expect_last(mk, "read box, second request", "type=read:id=q2:status=EBUSY");
    osc52_read_box_open = false;
    term_osc52_read_answer(mk->term, true, GRANT_ONCE, false);
    if (!strstr(osc52_all, "type=read:id=q1:status=OK") ||
        !strstr(osc52_all, "type=read:id=q1:status=DONE"))
        fail("read box, second request", "the first request was not served after the answer");

    /* Deny: refused, nothing sent, and an EPERM for the 5522 request */
    read_reset(mk);
    counters_reset();
    feed_5522_raw(mk, "type=read:id=q3", "dGV4dC9wbGFpbg==");
    osc52_read_box_open = false;
    term_osc52_read_answer(mk->term, false, GRANT_ONCE, false);
    expect_last(mk, "read box, Deny", "type=read:id=q3:status=EPERM");
    if (strstr(osc52_all, "status=DATA"))
        fail("read box, Deny", "clipboard data went out after a Deny");

    osc52_read_box_manual = false;
    read_reset(mk);
}

/*
 * OSC 9, 777 and 99 reach the notification handler (kitty/kitty_hostnotify.c,
 * stubbed above) with the string after the number, and OSC 99 gets the larger
 * ceiling: a 2048-byte body in base64 plus its metadata is past OSC_STR_MAX.
 * The parsing itself is tested in test/test_oscnotify.c.
 */
static void test_hostnotify_dispatch(Mock *mk)
{
    char *seq;
    size_t n = 3000, i;

    feed_seq(mk, "\033]9;4;1;50\007");
    if (hn_calls != 1 || hn_last_osc != 9 || strcmp(hn_last, "4;1;50"))
        fail("OSC 9;4 dispatch", hn_last);
    feed_seq(mk, "\033]777;notify;T;B\033\\");
    if (hn_calls != 2 || hn_last_osc != 777 || strcmp(hn_last, "notify;T;B"))
        fail("OSC 777 dispatch", hn_last);

    seq = snewn(n + 32, char);
    memcpy(seq, "\033]99;i=1;", 9);
    for (i = 0; i < n; i++)
        seq[9 + i] = 'A';
    memcpy(seq + 9 + n, "\033\\", 3);
    feed_seq(mk, seq);
    if (hn_calls != 3 || hn_last_osc != 99 || hn_last_len != 4 + n ||
        hn_last_overflow)
        fail("OSC 99 ceiling", "a 3000-byte OSC 99 did not arrive whole");
    sfree(seq);

    /* An OSC 9 notice keeps the ordinary ceiling: cut, and says so. */
    seq = snewn(n + 32, char);
    memcpy(seq, "\033]9;", 4);
    for (i = 0; i < n; i++)
        seq[4 + i] = 'A';
    memcpy(seq + 4 + n, "\007", 2);
    feed_seq(mk, seq);
    if (hn_calls != 4 || hn_last_osc != 9 || hn_last_len != OSC_STR_MAX ||
        !hn_last_overflow)
        fail("OSC 9 ceiling", "an over-long OSC 9 was not cut at OSC_STR_MAX");
    sfree(seq);
}

int main(void)
{
    Mock *mk = mock_new();
    CoInitialize(NULL);                 /* COM, as window.c: the PNG decoder */
    mk->term = term_init(mk->conf, mk->ucsdata, &mk->tw);
    term_pwron(mk->term, true);
    term_size(mk->term, 24, 80, 0);

    /* --- the ordinary case, and the selectors that are legal --- */
    expect_clip(mk, "plain text", "\033]52;c;SGVsbG8=\007", L"Hello");
    expect_clip(mk, "selector p", "\033]52;p;SGVsbG8=\007", L"Hello");
    expect_clip(mk, "selector s", "\033]52;s;SGVsbG8=\007", L"Hello");
    expect_clip(mk, "cut buffer 0", "\033]52;0;SGVsbG8=\007", L"Hello");
    expect_clip(mk, "empty selector", "\033]52;;SGVsbG8=\007", L"Hello");
    expect_clip(mk, "multiple selectors", "\033]52;cp;SGVsbG8=\007", L"Hello");
    /* ST rather than BEL must terminate it just the same */
    expect_clip(mk, "ST terminator", "\033]52;c;SGVsbG8=\033\\", L"Hello");
    /* UTF-8 in, wide characters out: "hello" */
    expect_clip(mk, "utf-8 payload", "\033]52;c;aMOpbGxv\007", L"héllo");

    /*
     * --- the READ direction ---
     * "?" asks us to send the local clipboard TO the host. It never writes the
     * clipboard, whatever the write policy says, and by default it sends nothing
     * either; the permission engine has its own tests below.
     */
    expect_refused(mk, "clipboard read request", "\033]52;c;?\007");
    expect_refused(mk, "clipboard read, no selector", "\033]52;;?\007");

    /* --- malformed: refused whole, never in part --- */
    expect_refused(mk, "invalid base64 character", "\033]52;c;SGVs*G8=\007");
    expect_refused(mk, "base64 length 1 mod 4", "\033]52;c;SGVsbG8=A\007");
    expect_refused(mk, "unknown selector", "\033]52;x;SGVsbG8=\007");
    expect_refused(mk, "no Pd field at all", "\033]52;SGVsbG8=\007");

    /* --- the policy gate --- */
    if (feed(mk, OSC52_CLIPBOARD_DENY, "\033]52;c;SGVsbG8=\007",
             strlen("\033]52;c;SGVsbG8=\007")) != 0)
        fail("policy disabled", "the clipboard was written anyway");

    /*
     * --- the buffer really grows ---
     * 256 KB of text, far past the 2 KB the OSC buffer used to be fixed at.
     * "QUFB" is the base64 of "AAA", so k copies decode to 3k characters.
     */
    {
        const int k = 87382;                    /* 262146 characters out */
        size_t seqlen = 7 + (size_t)k * 4 + 1;
        char *seq = snewn(seqlen + 1, char);
        size_t i;
        memcpy(seq, "\033]52;c;", 7);
        for (i = 0; i < (size_t)k; i++)
            memcpy(seq + 7 + i * 4, "QUFB", 4);
        seq[seqlen - 1] = '\007';
        seq[seqlen] = '\0';
        if (feed(mk, OSC52_CLIPBOARD_ALLOW, seq, seqlen) != 1)
            fail("256 KB payload", "nothing was written to the clipboard");
        else if (mk->clip_len != 3 * k + 1)     /* +1: the terminating NUL */
            fail("256 KB payload", "the payload was truncated");
        sfree(seq);
    }

    /*
     * --- and NO other sequence gained that headroom ---
     * A window title is still cut at the size the buffer used to be fixed at,
     * so the clipboard feature cannot be used to hand us a huge title.
     */
    {
        size_t big = 4000;
        char *seq = snewn(big + 16, char);
        size_t n = 0;
        memcpy(seq + n, "\033]0;", 4); n += 4;
        memset(seq + n, 'T', big); n += big;
        seq[n++] = '\007';
        strbuf_clear(mk->title);
        term_data(mk->term, seq, n);
        term_update(mk->term);
        /* "0;" is consumed as the OSC argument and its separator, so the string
         * itself is pure 'T's and stops at the ceiling. */
        if (mk->title->len != OSC_STR_MAX)
            fail("window title ceiling",
                 "a long title was not cut where it always was");
        sfree(seq);
    }

    /* --- the read permission engine, and the focus rule on writes --- */
    test_read_direction(mk);
    test_osc5522(mk);
    test_osc5522_write(mk);
    test_write_confirm(mk);
    test_read_box_modeless(mk);
    test_osc5522_paste_events(mk);
    test_write_focus_rule(mk);
    test_clipboard_write_rate(mk);
    test_far2l_ceiling(mk);
    test_far2l_focus(mk);
    test_far2l_protocol(mk);
    test_colour_queries(mk);
    test_hostnotify_dispatch(mk);
    test_far2l_arming_and_images(mk);   /* KiTTY: far2l events armed by 'x'; images */
    test_far2l_notify_and_overflow(mk); /* KiTTY: far2l 'n'; over-ceiling answers */
    test_modkeys_and_colour_scheme(mk); /* KiTTY: XTMODKEYS; mode 2031 reports */
    test_kitty_keyboard_stack(mk);      /* KiTTY: kitty keyboard protocol flags */
    test_kitty_graphics(mk);            /* KiTTY: kitty graphics placements */
    test_kitty_graphics_chafa(mk);      /* KiTTY: chafa's chunked placement */
    test_iterm_graphics(mk);            /* KiTTY: iTerm2 inline images */
    test_sixel_graphics(mk);            /* KiTTY: Sixel, DA1, XTSMGRAPHICS, XTVERSION */
    test_cell_bound_pictures(mk);       /* KiTTY: iTerm2/Sixel pictures own their cells */

    mock_free(mk);

    if (failures) {
        printf("Test suite FAILED (%d failure%s)\n",
               failures, failures == 1 ? "" : "s");
        return 1;
    }
    printf("Test suite passed\n");
    return 0;
}

