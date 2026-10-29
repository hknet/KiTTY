/*
 * kitty_hk00_ui.c: kitty.exe's side of the host key update
 * (kitty_hostkeys00.h, struct kitty_hk00_ui).
 *
 * updatehostkeys=yes: a notice near the clock for every key the update
 * stores. A click on it opens Application > Security > Host Keys filtered
 * to the host (window.c, WM_KITTY_HOSTKEY_STORED).
 *
 * updatehostkeys=ask: all changes of a connection in one question, in the
 * form "Unknown host key:" (modalnewhostkeyconfirmation) sets -
 *  - "Pop-up box": the shared host key box (kitty_dlgbox.c), the terminal
 *    staying enabled behind it (the session is live).
 *  - "Prompt in the terminal": the changes as lines in the terminal, written
 *    from a top-level callback (none while the alternate screen is shown:
 *    the Terminal Window's Event Log gets them instead). The answer is on a
 *    notice with "Apply" / "Change nothing" that never takes the focus.
 * The answer goes to the SSH layer from a top-level callback. No answer (the
 * notice ran out or was replaced, a click beside its buttons, the server
 * closed) changes nothing. A hidden window gets no question at all. When the
 * connection goes first (cancel), the question goes without an answer.
 */
#include <stdio.h>
#include <string.h>

#include "putty.h"                /* windows.h, after winsock2.h */
#include "terminal.h"
#include "kitty.h"
#include "kitty_params.h"     /* GetVisibleFlag */
#include "kitty_hostkeys00.h"
#include "kitty_notice.h"
#include "kitty_dlgbox.h"
#include "kitty_text.h"
#include "kitty_hk00_ui.h"

/* windows/window.c: the terminal window of a Seat - its own, or for a jump
 * host's connection the window of this process. false: no window. */
bool kitty_hk00_window(Seat *seat, HWND *hwnd, Terminal **term,
                       LogContext **logctx);
/* windows/dialog.c: the in-terminal questions' wrapping at 76 columns, and
 * the hook its end-of-questions pass calls for the ones made here */
char *kitty_dlg_wrap_dup(const char *s);
extern void (*kitty_hk00_seat_end_hook)(Seat *seat, bool peer, bool gone);
extern int GetModalNewHostKeyConfirmationFlag(void);

#define HK00_ASK_SECONDS 60                 /* the notice of the question */
#define HK00_ASK_ACCENT  RGB(190, 110, 0)   /* amber, as every question */
#define HK00_NEW_ACCENT  RGB(0, 100, 0)     /* green, as every info notice */

struct hk00_ask {
    HWND hwnd;                 /* the terminal window it belongs to */
    Terminal *term;
    LogContext *logctx;
    void (*done)(void *ctx, int answer);
    void *ctx;
    HWND box;                  /* "Pop-up box": the box while it is up */
    bool notice;               /* "Prompt in the terminal": its notice is up */
    char *lines;               /* ... the lines, until written */
    bool answered;             /* the answer is known */
    int answer;
    bool cancelled;            /* the connection is gone: no done() */
    int refs;                  /* the question itself + each queued callback */
    struct hk00_ask *next;
};
static struct hk00_ask *hk00_open = NULL;

static void hk00_ask_unref(struct hk00_ask *a)
{
    struct hk00_ask **pp;
    if (--a->refs > 0)
        return;
    for (pp = &hk00_open; *pp; pp = &(*pp)->next)
        if (*pp == a) {
            *pp = a->next;
            break;
        }
    sfree(a->lines);
    sfree(a);
}

/* The last field of a fingerprint ("ssh-ed25519 255 SHA256:x" -> the hash),
 * in a fresh buffer. */
static char *hk00_last_word(const char *fp)
{
    const char *sp = fp ? strrchr(fp, ' ') : NULL;
    return dupstr(sp ? sp + 1 : fp ? fp : "");
}

/* The first field: the key type. */
static char *hk00_first_word(const char *fp)
{
    const char *sp = fp ? strchr(fp, ' ') : NULL;
    if (!fp)
        return dupstr("");
    return sp ? dupprintf("%.*s", (int)(sp - fp), fp) : dupstr(fp);
}

static void hk00_ask_deliver(void *vctx)
{
    struct hk00_ask *a = (struct hk00_ask *)vctx;
    if (!a->cancelled && a->done) {
        void (*done)(void *, int) = a->done;
        a->done = NULL;
        done(a->ctx, a->answer);
    }
    hk00_ask_unref(a);           /* the queued callback */
    hk00_ask_unref(a);           /* the question */
}

/* The answer is known: to the SSH layer from a top-level callback, outside
 * the box's or the notice's own messages. */
static void hk00_ask_answer(struct hk00_ask *a, int answer)
{
    if (a->answered || a->cancelled)
        return;
    a->answered = true;
    a->answer = answer;
    a->refs++;
    queue_toplevel_callback(hk00_ask_deliver, a);
}

/* Take the box and the notice down without an answer of their own. */
static void hk00_ask_close_ui(struct hk00_ask *a)
{
    HWND box = a->box;
    a->box = NULL;
    if (box && IsWindow(box))
        DestroyWindow(box);      /* its done() comes now and is ignored */
    if (a->notice) {
        a->notice = false;
        if (kitty_notice_showing(a))
            kitty_notice_close_ctx(a);
    }
}

/* kitty_confirm_modeless_words_help: Apply (1) or anything else (0: Change
 * nothing, Esc, the box closed) */
static void hk00_box_done(int yes, void *vctx)
{
    struct hk00_ask *a = (struct hk00_ask *)vctx;
    a->box = NULL;
    hk00_ask_answer(a, yes ? KHK00_ANS_APPLY : KHK00_ANS_DECLINED);
}

/* the notice: Apply (1), Change nothing (button 2), anything else - ran
 * out, replaced, a click beside the buttons - not answered */
static void hk00_notice_closed(void *vctx, int clicked)
{
    struct hk00_ask *a = (struct hk00_ask *)vctx;
    a->notice = false;
    hk00_ask_answer(a, clicked == 1 ? KHK00_ANS_APPLY :
                    clicked == KITTY_NOTICE_BUTTON2 ? KHK00_ANS_DECLINED :
                    KHK00_ANS_UNANSWERED);
}

/* "Prompt in the terminal": the lines, from a top-level callback (the SSH
 * layer's own call stack must not write to the terminal). No line is
 * written after the answer. */
static void hk00_ask_write(void *vctx)
{
    struct hk00_ask *a = (struct hk00_ask *)vctx;
    if (!a->cancelled && !a->answered && a->lines && a->term)
        term_data(a->term, a->lines, strlen(a->lines));
    sfree(a->lines);
    a->lines = NULL;
    hk00_ask_unref(a);
}

/* The facts of the question as lines: KT_HK00_Q_* (the terminal, or the
 * Terminal Window's Event Log while the alternate screen is shown) */
static void hk00_put_line(strbuf *sb, const char *line)
{
    char *w = kitty_dlg_wrap_dup(line);
    put_fmt(sb, "%s\r\n", w);
    sfree(w);
}

static char *hk00_fact_lines(const struct kitty_hk00_question *q, bool hint)
{
    strbuf *sb = strbuf_new();
    char *line = dupprintf(KT_HK00_Q_HEAD, q->host, q->port);
    put_dataz(sb, "\r\n");
    hk00_put_line(sb, line);
    sfree(line);
    for (size_t i = 0; i < q->n; i++) {
        const struct kitty_hk00_change *c = &q->ch[i];
        /* the indented lines keep their columns: no wrapping */
        if (c->kind == KHK00_CH_NEW) {
            put_fmt(sb, KT_HK00_Q_NEW "\r\n", c->fp);
            if (c->beside)
                put_fmt(sb, KT_HK00_Q_BESIDE "\r\n", c->beside);
        } else
            put_fmt(sb, KT_HK00_Q_GONE "\r\n", c->fp);
    }
    if (hint)
        hk00_put_line(sb, KT_HK00_T_HINT);
    return strbuf_to_str(sb);
}

/* The notice's text: KT_HK00_ASK_* */
static char *hk00_notice_text(const struct kitty_hk00_question *q)
{
    strbuf *sb = strbuf_new();
    put_fmt(sb, KT_HK00_ASK_HEAD, q->host, q->port);
    for (size_t i = 0; i < q->n; i++) {
        const struct kitty_hk00_change *c = &q->ch[i];
        put_byte(sb, '\n');
        if (c->kind == KHK00_CH_NEW) {
            put_fmt(sb, KT_HK00_ASK_NEW, c->fp);
            if (c->beside) {
                put_byte(sb, '\n');
                put_fmt(sb, KT_HK00_ASK_BESIDE, c->beside);
            }
        } else
            put_fmt(sb, KT_HK00_ASK_GONE, c->fp);
    }
    return strbuf_to_str(sb);
}

/* The box's detail field: label TAB value (B8 of the review) */
static char *hk00_box_detail(const struct kitty_hk00_question *q)
{
    strbuf *sb = strbuf_new();
    put_fmt(sb, "%s\t%s\r\n%s\t%d", KT_HKD_L_HOST, q->host, KT_HKD_L_PORT, q->port);
    for (size_t i = 0; i < q->n; i++) {
        const struct kitty_hk00_change *c = &q->ch[i];
        if (c->kind != KHK00_CH_NEW)
            continue;
        put_fmt(sb, "\r\n%s\t%s", KT_HKD_L_NEWKEY, c->fp);
        if (c->md5) {
            char *md5 = hk00_last_word(c->md5);
            put_fmt(sb, "\r\n\t" KT_HKD_V_MD5, md5);
            sfree(md5);
        }
        if (c->beside) {
            put_fmt(sb, "\r\n%s\t%s", KT_HKD_L_STOREDTYPE, c->beside);
            if (c->beside_first && *c->beside_first) {
                char *when = dupstr(c->beside_first);
                if (strlen(when) > 10 && when[10] == 'T')
                    when[10] = ' ';
                if (strlen(when) > 16)
                    when[16] = '\0';
                put_fmt(sb, "\r\n%s\t%s", KT_HKD_L_FIRSTSEEN_SUB, when);
                sfree(when);
            }
        }
        {
            char *type = hk00_first_word(c->fp);
            put_fmt(sb, "\r\n%s\t" KT_HKD_L_AFTER_FMT, KT_HKD_L_AFTER, c->after, type);
            sfree(type);
        }
    }
    for (size_t i = 0; i < q->n; i++)
        if (q->ch[i].kind == KHK00_CH_GONE)
            put_fmt(sb, "\r\n%s\t%s", KT_HKD_L_REMOVE, q->ch[i].fp);
    for (size_t i = 0; i < q->n; i++)
        if (q->ch[i].kind == KHK00_CH_NEW && q->ch[i].keytext)
            put_fmt(sb, "\r\n\r\n%s\r\n%s", KT_HKD_L_FULLKEY_N, q->ch[i].keytext);
    return strbuf_to_str(sb);
}

static int hk00_confirm(Seat *seat, const struct kitty_hk00_question *q,
                        void (*done)(void *ctx, int answer), void *ctx,
                        void **handle)
{
    HWND hwnd;
    Terminal *term;
    LogContext *logctx;
    struct hk00_ask *a;

    /* no window to show it: a hidden kitty.exe gets no question */
    if (!kitty_hk00_window(seat, &hwnd, &term, &logctx) ||
        GetVisibleFlag() == VISIBLE_NO || !IsWindow(hwnd))
        return KHK00_ANS_UNANSWERED;

    a = snew(struct hk00_ask);
    memset(a, 0, sizeof(*a));
    a->hwnd = hwnd;
    a->term = term;
    a->logctx = logctx;
    a->done = done;
    a->ctx = ctx;
    a->refs = 1;
    a->next = hk00_open;
    hk00_open = a;

    if (GetModalNewHostKeyConfirmationFlag()) {
        /* "Pop-up box": Change nothing is the default. The terminal stays
         * enabled behind it, the session is live. */
        char *text = dupprintf(KT_HKD_NEW_TEXT, q->host, q->port);
        char *detail = hk00_box_detail(q);
        a->box = kitty_confirm_modeless_words_help(
            hwnd, KT_HKD_NEW_CAP, text, detail, NULL, KT_HKD_APPLY,
            KT_HKD_NOTHING, HELPCTX(kitty_hostkey_update), hk00_box_done, a);
        sfree(text);
        sfree(detail);
        if (!a->box) {
            hk00_ask_unref(a);
            return KHK00_ANS_UNANSWERED;
        }
    } else {
        /* "Prompt in the terminal": the lines, the answer on a notice */
        KittyNoticeExtras x;
        char *text = hk00_notice_text(q);
        if (term && term_alt_screen_shown(term)) {
            /* a full-screen program has the screen: the facts go to the
             * Terminal Window's Event Log instead */
            char *facts = hk00_fact_lines(q, false);
            char *p = facts, *eol;
            while (logctx && *p) {
                eol = strstr(p, "\r\n");
                if (eol)
                    *eol = '\0';
                if (*p)
                    logevent(logctx, p);
                if (!eol)
                    break;
                p = eol + 2;
            }
            sfree(facts);
        } else {
            a->lines = hk00_fact_lines(q, true);
            a->refs++;
            queue_toplevel_callback(hk00_ask_write, a);
        }
        memset(&x, 0, sizeof(x));
        x.action = KT_HK00_ASK_APPLY;
        x.action2 = KT_HK00_ASK_NOTHING;
        a->notice = true;
        kitty_notice_show_x(KT_HK00_ASK_TITLE, text, HK00_ASK_ACCENT,
                            HK00_ASK_SECONDS, NULL, 0, hk00_notice_closed, a, &x);
        sfree(text);
    }
    *handle = a;
    return KHK00_ANS_PENDING;
}

/* The connection is gone: the question goes, done() is never called. */
static void hk00_cancel(void *handle)
{
    struct hk00_ask *a = (struct hk00_ask *)handle;
    if (!a)
        return;
    a->cancelled = true;
    hk00_ask_close_ui(a);
    if (!a->answered)
        hk00_ask_unref(a);       /* the question (a queued delivery has its own) */
}

/* windows/dialog.c's end-of-questions pass for a Seat. The server closed
 * (peer): the question ends unanswered while the SSH layer can still take
 * the answer. The SSH layers are freed (gone): it goes without one. The
 * cancel that follows frees it. */
static void hk00_seat_end(Seat *seat, bool peer, bool gone)
{
    HWND hwnd;
    Terminal *term;
    LogContext *logctx;
    struct hk00_ask *a;
    (void)peer;
    if (!kitty_hk00_window(seat, &hwnd, &term, &logctx))
        return;
    for (a = hk00_open; a; a = a->next) {
        if (a->hwnd != hwnd || a->answered || a->cancelled)
            continue;
        if (gone) {
            a->cancelled = true; /* freed by the cancel that follows */
            hk00_ask_close_ui(a);
            continue;
        }
        hk00_ask_answer(a, KHK00_ANS_UNANSWERED);
        hk00_ask_close_ui(a);
    }
}

/* updatehostkeys=yes: the notice of a key stored. Its click opens the Host
 * Keys leaf filtered to this host with the new key selected. */
static char *hk00_last_host = NULL, *hk00_last_hash = NULL;

static void hk00_stored(Seat *seat, const char *host, int port,
                        const char *fp, const char *beside, const char *replaced)
{
    HWND hwnd;
    Terminal *term;
    LogContext *logctx;
    char *text;
    if (!kitty_hk00_window(seat, &hwnd, &term, &logctx))
        return;
    sfree(hk00_last_host);
    sfree(hk00_last_hash);
    hk00_last_host = dupstr(host);
    hk00_last_hash = hk00_last_word(fp);
    text = replaced ? dupprintf(KT_HK00_NOTICE_TEXT_REPLACED, host, port, fp, replaced)
         : beside ? dupprintf(KT_HK00_NOTICE_TEXT, host, port, fp, beside)
                  : dupprintf(KT_HK00_NOTICE_TEXT_TYPE, host, port, fp);
    kitty_notice_show(KT_HK00_NOTICE_TITLE, text, HK00_NEW_ACCENT,
                      GetNoticeSeconds(), hwnd, WM_KITTY_HOSTKEY_STORED);
    sfree(text);
}

bool kitty_hk00_ui_last_stored(const char **host, const char **hash)
{
    if (!hk00_last_host)
        return false;
    *host = hk00_last_host;
    *hash = hk00_last_hash;
    return true;
}

void kitty_hk00_ui_install(void)
{
    static const struct kitty_hk00_ui ui = {
        hk00_confirm, hk00_cancel, hk00_stored };
    kitty_hk00_set_ui(&ui);
    kitty_hk00_seat_end_hook = hk00_seat_end;
}
