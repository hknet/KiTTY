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
static int osc52_last_len;

void kitty_osc52_send_raw(Terminal *term, const char *data, size_t len)
{
    osc52_sends++;
    osc52_last_len = (int)(len < sizeof(osc52_last_send) - 1
                           ? len : sizeof(osc52_last_send) - 1);
    memcpy(osc52_last_send, data, osc52_last_len);
    osc52_last_send[osc52_last_len] = '\0';
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

/* ---------------------------------------------------------------------------
 * OSC 5522 - the kitty clipboard protocol
 * ------------------------------------------------------------------------- */

/* base64 of a plain string, for building request payloads. */
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
    osc52_gets = 0;
    osc52_sends = 0;
    osc52_dialogs = 0;
    osc52_last_send[0] = '\0';
    term_data(mk->term, seq, strlen(seq));
    term_update(mk->term);
    sfree(seq);
    sfree(p);
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

    /* The write direction is not built, and says so, so a program can fall back
     * to OSC 52 rather than hang. */
    read_reset(mk);
    feed_5522(mk, "type=write", NULL);
    expect_last(mk, "5522 write", "type=write:status=ENOSYS");
    read_reset(mk);
    feed_5522(mk, "type=walias", NULL);
    expect_last(mk, "5522 walias", "type=walias:status=ENOSYS");

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
     * --- base64 tolerance ---
     *
     * These mirror the only part of kitty's own test suite that bears on the read
     * path (kitty_tests/clipboard.py, which otherwise unit-tests their streaming
     * decoder and the write direction we do not implement). What it establishes is
     * that base64 PADDING IS OPTIONAL in practice, whatever the spec says about
     * it being required - so anything of ours that compares or decodes base64 has
     * to cope with both spellings.
     */

    /* An unpadded mime list still parses, so the request is still served.
     * "text/plain" base64s to "dGV4dC9wbGFpbg==", and the unpadded spelling has
     * to mean the same thing. */
    read_reset(mk);
    osc52_dialog_answer = true;
    osc52_dialog_grant = GRANT_ONCE;
    {
        char *seq = dupprintf("\033]5522;type=read;dGV4dC9wbGFpbg\033\\");
        osc52_sends = 0; osc52_dialogs = 0; osc52_last_send[0] = '\0';
        term_data(mk->term, seq, strlen(seq));
        term_update(mk->term);
        sfree(seq);
        if (osc52_sends != 3)
            fail("5522 unpadded mime list", "an unpadded type list was not served");
    }

    /*
     * The same password padded and unpadded is the SAME password. Without this a
     * client that pads on one request and not the next looks like a different
     * program and gets prompted about again - which is the exact fatigue the
     * password mechanism exists to remove.
     */
    read_reset(mk);
    osc52_dialog_answer = true;
    osc52_dialog_grant = GRANT_SESSION;
    feed_5522(mk, "type=read:pw=cHc=:name=bnZpbQ==", "text/plain");
    if (osc52_dialogs != 1)
        fail("5522 padding-insensitive password", "the first request did not ask");
    feed_5522(mk, "type=read:pw=cHc:name=bnZpbQ==", "text/plain");
    if (osc52_dialogs != 0)
        fail("5522 padding-insensitive password",
             "the same password unpadded was treated as a different program");

    /*
     * A "password" that is nothing but padding is not a password. Tested with a
     * ONE-OFF answer on purpose: a session-wide answer would create the ordinary
     * window-wide grant and serve the second request legitimately, which tells us
     * nothing about whether an approval was recorded. With a one-off answer,
     * nothing may be remembered by either mechanism, so the second request has to
     * ask again.
     */
    read_reset(mk);
    osc52_dialog_answer = true;
    osc52_dialog_grant = GRANT_ONCE;
    feed_5522(mk, "type=read:pw==:name=bnZpbQ==", "text/plain");
    if (osc52_dialogs != 1)
        fail("5522 empty password", "the first request did not ask");
    feed_5522(mk, "type=read:pw==:name=bnZpbQ==", "text/plain");
    if (osc52_dialogs != 1)
        fail("5522 empty password", "an all-padding password became an approval");

    /* Malformed base64 in the payload must fail CLOSED: the type list decodes to
     * something that matches nothing, so no data is served and nobody is asked. */
    read_reset(mk);
    osc52_dialog_answer = true;
    {
        char *seq = dupprintf("\033]5522;type=read;!!!not-base64!!!\033\\");
        osc52_sends = 0; osc52_dialogs = 0;
        term_data(mk->term, seq, strlen(seq));
        term_update(mk->term);
        sfree(seq);
        if (osc52_dialogs != 0)
            fail("5522 malformed payload", "a malformed request prompted the user");
        if (osc52_sends != 2)
            fail("5522 malformed payload",
                 "expected OK then DONE with no data for an unmatchable type list");
    }

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
