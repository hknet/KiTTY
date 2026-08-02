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

void kitty_osc52_send_reply(Terminal *term, const char *b64, size_t len)
{
    osc52_sends++;
}

wchar_t *kitty_osc52_get_clipboard(int *len)
{
    size_t n;
    wchar_t *out;
    osc52_gets++;
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

bool kitty_osc52_read_dialog(Terminal *term, const wchar_t *clip, int clip_len,
                             const char *claim, int *grant, bool *always_deny)
{
    osc52_dialogs++;
    if (grant) *grant = osc52_dialog_grant;
    if (always_deny) *always_deny = false;
    return osc52_dialog_answer;
}

bool kitty_osc52_save_deny_for_host(Terminal *term) { return true; }
void kitty_osc52_notify(Terminal *term, const char *t, const char *m) { }
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
    conf_set_bool(c, CONF_osc52_require_focus, true);
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
    conf_set_bool(mk->term->conf, CONF_osc52_require_focus, false);
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
     * The hand-over rate limit, which is the one that stops a grant being turned
     * against the user. Asking faster than the limit does not merely get refused:
     * it costs the host the permission, because that pattern is harvesting rather
     * than use.
     */
    read_reset(mk);
    conf_set_int(mk->term->conf, CONF_osc52_read_interval, 3600);
    mk->term->osc52_read_decision = 1;
    mk->term->osc52_read_remaining = -1;
    mk->term->osc52_read_last_served = (unsigned long)time(NULL);
    osc52_dialog_answer = false;
    expect_read(mk, "too fast: refused and permission withdrawn", 0, 1);
    if (mk->term->osc52_read_decision > 0)
        fail("hand-over rate limit", "the grant survived being exceeded");

    /* Same for the whole-window ceiling. */
    read_reset(mk);
    conf_set_int(mk->term->conf, CONF_osc52_read_max, 2);
    mk->term->osc52_read_decision = 1;
    mk->term->osc52_read_remaining = -1;
    mk->term->osc52_read_served = 2;
    osc52_dialog_answer = false;
    expect_read(mk, "window ceiling reached", 0, 1);
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

/* The focus rule applies to WRITES as well, which is a change to behaviour that
 * shipped working - so it gets its own test in both positions. */
static void test_write_focus_rule(Mock *mk)
{
    const char *seq = "\033]52;c;SGVsbG8=\007";

    conf_set_bool(mk->term->conf, CONF_osc52_require_focus, true);
    mk->term->has_focus = false;
    if (feed(mk, OSC52_CLIPBOARD_ALLOW, seq, strlen(seq)) != 0)
        fail("write with no focus", "the clipboard was written anyway");

    mk->term->has_focus = true;
    if (feed(mk, OSC52_CLIPBOARD_ALLOW, seq, strlen(seq)) != 1)
        fail("write with focus", "the clipboard was not written");

    /* switched off, an unfocused write works again */
    conf_set_bool(mk->term->conf, CONF_osc52_require_focus, false);
    mk->term->has_focus = false;
    if (feed(mk, OSC52_CLIPBOARD_ALLOW, seq, strlen(seq)) != 1)
        fail("write, focus rule off", "the clipboard was not written");

    conf_set_bool(mk->term->conf, CONF_osc52_require_focus, true);
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
    test_write_focus_rule(mk);

    mock_free(mk);

    if (failures) {
        printf("Test suite FAILED (%d failure%s)\n",
               failures, failures == 1 ? "" : "s");
        return 1;
    }
    printf("Test suite passed\n");
    return 0;
}
