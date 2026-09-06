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
 *  - the policy gate is honoured (CONF_osc52_clipboard: deny/allow/ask).
 *    "Ask" is never exercised here - it raises a MessageBox, which would hang a
 *    headless run;
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

bool kitty_osc52_read_dialog(Terminal *term, const wchar_t *clip, int clip_len,
                             const char *claim, int *grant, bool *always_deny)
{
    osc52_dialogs++;
    if (grant) *grant = osc52_dialog_grant;
    if (always_deny) *always_deny = false;
    return osc52_dialog_answer;
}

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
     * line is asserted by qa_clipboard_auto.ps1 instead - it can hold the real
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

int main(void)
{
    Mock *mk = mock_new();
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
    /* UTF-8 in, wide characters out: "héllo" */
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
    test_osc5522_paste_events(mk);
    test_write_focus_rule(mk);
    test_clipboard_write_rate(mk);
    test_far2l_ceiling(mk);
    test_far2l_focus(mk);

    mock_free(mk);

    if (failures) {
        printf("Test suite FAILED (%d failure%s)\n",
               failures, failures == 1 ? "" : "s");
        return 1;
    }
    printf("Test suite passed\n");
    return 0;
}
