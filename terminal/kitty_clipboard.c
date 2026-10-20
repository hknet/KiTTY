/*
 * kitty_clipboard.c - KiTTY++: the host's access to the local clipboard,
 * OSC 52 and OSC 5522 (the kitty clipboard protocol), and the gates both
 * share with far2l's clipboard: the permission boxes, the rate cap, the
 * size ceiling, the paste events. Moved out of terminal/terminal.c as it
 * was; do_osc reaches it through kitty_osc_dispatch (kitty_osc.c).
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

/*
 * KiTTY: a clipboard payload was too large for the ceiling on its sequence, so it
 * was dropped whole. Say so.
 *
 * Shared by OSC 52, OSC 5522 and far2l deliberately. It is one event - "the thing
 * you copied did not arrive" - and three protocols reporting it three different
 * ways is how far2l's 2 KB cap survived unnoticed: the parser truncated, the
 * handler dropped it, and nothing anywhere said a word, so it was indistinguishable
 * from the feature simply not working.
 *
 * Both channels are rate-limited, because a remote host chooses when this fires and
 * could otherwise use it to flood the Event Log or fire balloons at will. The
 * balloon additionally only speaks for a feature the user actually turned on:
 * someone who set a policy to Deny has already answered, and does not need telling
 * that a host they refused was refused again.
 */
#define CLIP_DROP_LOG_GAP 5            /* seconds between Event Log lines */
#define CLIP_NOTIFY_GAP  30            /* seconds between balloons */

void clip_payload_dropped(Terminal *term, const char *what, bool enabled)
{
    unsigned long now = (unsigned long)time(NULL);

    if (term->clip_dropped_logged == 0 ||
        now - term->clip_dropped_logged >= CLIP_DROP_LOG_GAP) {
        char *msg;
        if (term->clip_dropped_quiet > 0)
            msg = dupprintf(KT_CLIP_LOG_DROPPED_MORE, what,
                            term->clip_dropped_quiet,
                            term->clip_dropped_quiet == 1 ? "" : "s");
        else
            msg = dupprintf(KT_CLIP_LOG_DROPPED, what);
        logevent(term->logctx, msg);
        sfree(msg);
        term->clip_dropped_quiet = 0;
        term->clip_dropped_logged = now;
    } else {
        term->clip_dropped_quiet++;
    }

    if (!enabled || !conf_get_bool(term->conf, CONF_clipboard_notify))
        return;
    if (term->clip_notified_last != 0 &&
        now - term->clip_notified_last < CLIP_NOTIFY_GAP)
        return;
    term->clip_notified_last = now;
    /*
     * Named as a size problem, not as a refusal: the user did nothing wrong and
     * neither, necessarily, did the host - this is a limit being hit.
     *
     * It says the Event Log has the rest BECAUSE this balloon is rate-limited: it
     * can only ever report that something was dropped, never how many times or
     * which, and a notification that quietly stands for an unknown number of
     * events is worse than one that admits it. Clicking it opens the log.
     */
    kitty_osc52_notify(term, KT_CAP_CLIPBOARD,
                       KT_CLIP_NOTICE_TOO_MUCH,
                       CLIP_BALLOON_LOG);
}

/*
 * KiTTY: a remote clipboard WRITE arrived too soon after the last one, so it was
 * dropped. Same shape as clip_payload_dropped above, and separate accounting so
 * the two cannot mask each other.
 *
 * The balloon offers to BLOCK rather than pointing at the Event Log. Telling
 * somebody that a server is stamping on their clipboard is not much use without a
 * way to stop it, and "open the configuration box and find the right panel" is not
 * a way to stop it while it is still happening.
 */
/*
 * KiTTY: the host has just touched the clipboard. Light the activity marker.
 *
 * Permission says what COULD happen; this says what DID. It is the more useful of
 * the two day to day - a clipboard the host never touches and one it reads every
 * thirty seconds look identical without it.
 *
 * Repeated activity EXTENDS the deadline rather than repainting, so a host writing
 * at the permitted rate cannot make the title flicker; the window is only told
 * when the visible state actually changes.
 */
void clip_note_activity(Terminal *term, int dir)
{
    unsigned long now = (unsigned long)time(NULL);
    int secs, was_dir;

    if (!conf_get_bool(term->conf, CONF_clipboard_activity_mark))
        return;
    secs = conf_get_int(term->conf, CONF_clipboard_activity_secs);
    if (secs <= 0)
        secs = 5;

    was_dir = (term->clip_activity_until > now) ? term->clip_activity_dir : 0;
    term->clip_activity_dir = was_dir | dir;
    term->clip_activity_until = now + (unsigned long)secs;
    if (term->clip_activity_dir != was_dir)
        kitty_osc52_state_changed(term);

    /*
     * Full screen, or decorations switched off: there is no title bar to carry the
     * icon and no caption to tint, so everything above is invisible. Fall back to a
     * balloon, which is the only channel left.
     *
     * Rate-limited by the same 30-second gap as every other balloon, and sharing
     * its timestamp, so this cannot become a stream - a host writing at the
     * permitted rate would otherwise be able to raise one every few seconds. Both
     * directions, because the point of the activity marker is to show that the
     * clipboard was touched at all.
     */
    if (!kitty_osc52_title_visible() &&
        conf_get_bool(term->conf, CONF_clipboard_notify) &&
        (term->clip_notified_last == 0 ||
         now - term->clip_notified_last >= CLIP_NOTIFY_GAP)) {
        term->clip_notified_last = now;
        kitty_osc52_notify(
            term, KT_CAP_CLIPBOARD,
            (dir & CLIP_ACT_READ)
            ? KT_CLIP_NOTICE_READ_NOTITLE
            : KT_CLIP_NOTICE_WRITE_NOTITLE,
            CLIP_BALLOON_LOG);
    }
}

/*
 * May a remote clipboard write be applied right now? Counts APPLIED writes in a
 * fixed one-second window; over the cap, the write is dropped and reported.
 *
 * Returns true and books the write, or false having already reported it.
 */
bool clip_write_allowed(Terminal *term)
{
    int cap = conf_get_int(term->conf, CONF_clipboard_writes_per_sec);
    unsigned long now = (unsigned long)time(NULL);

    if (cap <= 0)
        return true;                   /* no limit, by request */

    if (term->clip_write_second != now) {
        term->clip_write_second = now;
        term->clip_write_count = 0;
    }
    if (term->clip_write_count >= cap) {
        clip_write_throttled(term);
        return false;
    }
    term->clip_write_count++;
    return true;
}

void clip_write_throttled(Terminal *term)
{
    unsigned long now = (unsigned long)time(NULL);

    if (term->clip_write_logged == 0 ||
        now - term->clip_write_logged >= CLIP_DROP_LOG_GAP) {
        char *msg;
        if (term->clip_write_quiet > 0)
            msg = dupprintf(KT_CLIP_LOG_WRITE_RATE_MORE, term->clip_write_quiet,
                            term->clip_write_quiet == 1 ? "" : "s");
        else
            msg = dupprintf(KT_CLIP_LOG_WRITE_RATE);
        logevent(term->logctx, msg);
        sfree(msg);
        term->clip_write_quiet = 0;
        term->clip_write_logged = now;
    } else {
        term->clip_write_quiet++;
    }

    if (!conf_get_bool(term->conf, CONF_clipboard_notify))
        return;
    if (term->clip_notified_last != 0 &&
        now - term->clip_notified_last < CLIP_NOTIFY_GAP)
        return;
    term->clip_notified_last = now;
    kitty_osc52_notify(term, KT_CAP_CLIPBOARD,
                       KT_CLIP_NOTICE_WRITE_REPEATED,
                       CLIP_BALLOON_BLOCK_WRITES);
}

/*
 * ===========================================================================
 * KiTTY: OSC 52 clipboard READ - a host asking for the contents of YOUR
 * clipboard, which we then send to it.
 * ===========================================================================
 *
 * This is not the same kind of feature as the write direction below it, and the
 * asymmetry in the code is deliberate. A write changes what you paste next. A
 * read hands over whatever is on the clipboard right now, which is a password
 * often enough to matter, to a host that chose the moment - typically just after
 * you pasted something into it, because then it knows there is something worth
 * taking. So it is not a feature that gets switched on. It is a permission that
 * gets given, narrowly, in the moment, and then expires.
 *
 * One fact shapes all of it: a bare OSC 52 read cannot tell us who is asking.
 * The sequence is a selector and a question mark - no program name, no token. A
 * request from your editor is byte-for-byte a request from a one-line command a
 * hostile host ran a second ago. "Remember this program and stop asking" is
 * therefore impossible here; it needs OSC 5522, which carries a name. The honest
 * choices on OSC 52 are to ask, or to refuse.
 */

/* Has the standing decision run out? A grant is bounded by a deadline, a request
 * count, or neither (rest of session); any of the three can be the live one. */
static bool osc52_read_decision_expired(Terminal *term)
{
    if (term->osc52_read_decision == 0)
        return true;
    if (term->osc52_read_until != 0 &&
        (unsigned long)time(NULL) >= term->osc52_read_until)
        return true;
    if (term->osc52_read_remaining == 0)
        return true;
    return false;
}

static void osc52_read_forget_decision(Terminal *term)
{
    bool was_granting = (term->osc52_read_decision > 0);
    term->osc52_read_decision = 0;
    term->osc52_read_until = 0;
    term->osc52_read_remaining = 0;
    if (was_granting) {
        kitty_osc52_state_changed(term);
        if (conf_get_bool(term->conf, CONF_clipboard_notify))
            kitty_osc52_notify(term, KT_CAP_CLIPBOARD,
                               KT_CLIP_NOTICE_READ_EXPIRED,
                               CLIP_BALLOON_LOG);
    }
}

/*
 * Refuse, and account for it. Refusals are Event-Logged because a record after
 * the fact is the only way to notice a host that spent an hour asking - but a
 * host that asks in a loop must not be able to fill the log with our own writing,
 * so consecutive refusals of the same kind are coalesced into one line every few
 * seconds with a count.
 *
 * Nothing is sent to the host. OSC 52 has no error reply: any reply at all tells
 * a probing host that the feature exists and, worse, that somebody was there to
 * say no. Silence tells it less.
 */
#define OSC52_REFUSE_LOG_GAP 5         /* seconds between refusal log lines */

static void osc52_read_refuse(Terminal *term, const char *why, bool tell_user)
{
    unsigned long now = (unsigned long)time(NULL);

    if (term->osc52_read_refused_logged == 0 ||
        now - term->osc52_read_refused_logged >= OSC52_REFUSE_LOG_GAP) {
        if (term->osc52_read_refused_quiet > 0) {
            /* mention what was swallowed rather than pretending it did not
             * happen: "one refusal" and "four hundred refusals" are different
             * events and the log has to be able to say which this was */
            char *msg = dupprintf(KT_CLIP_LOG_READ_REFUSED_MORE,
                                  why, term->osc52_read_refused_quiet,
                                  term->osc52_read_refused_quiet == 1 ? "" : "s");
            logevent(term->logctx, msg);
            sfree(msg);
        } else {
            char *msg = dupprintf(KT_CLIP_LOG_READ_REFUSED, why);
            logevent(term->logctx, msg);
            sfree(msg);
        }
        term->osc52_read_refused_quiet = 0;
        term->osc52_read_refused_logged = now;

        /*
         * Only tell the user out loud when the setting is "ask". Someone who set
         * it to Deny has already answered this question and does not need
         * telling again - but the Event Log above still carries it either way.
         * Rate-limited by the same gap, so refusals cannot become a way to spam
         * notifications.
         */
        if (tell_user &&
            conf_get_int(term->conf, CONF_osc52_clipboard_read) == OSC52_READ_ASK &&
            conf_get_bool(term->conf, CONF_clipboard_notify))
            kitty_osc52_notify(term, KT_CAP_CLIPBOARD,
                               KT_CLIP_NOTICE_READ_REFUSED,
                               CLIP_BALLOON_LOG);
    } else {
        term->osc52_read_refused_quiet++;
    }
}

/*
 * Actually hand the clipboard over: ESC ] 52 ; c ; <base64 UTF-8> ST, the same
 * shape we accept in the write direction. Counts the hand-over against the limits
 * and writes the Event Log line, because a read that was served is exactly the
 * thing someone will want a record of afterwards.
 */
static void osc52_read_send(Terminal *term, const wchar_t *clip, int clip_len)
{
    char *utf8;
    strbuf *b64;
    char *msg;

    utf8 = encode_wide_string_as_utf8(clip);
    b64 = strbuf_new_nm();             /* _nm: this is clipboard content */
    /* BEL rather than ESC \ to terminate: it is what every implementation accepts,
     * and it is the terminator the request itself almost always arrives with. */
    put_dataz(b64, "\033]52;c;");
    base64_encode_bs(BinarySink_UPCAST(b64), ptrlen_from_asciz(utf8), 0);
    put_byte(b64, '\007');
    kitty_osc52_send_raw(term, b64->s, b64->len);

    smemclr(utf8, strlen(utf8));
    sfree(utf8);
    strbuf_free(b64);

    term->osc52_read_served++;
    term->osc52_read_last_served = (unsigned long)time(NULL);
    clip_note_activity(term, CLIP_ACT_READ);
    msg = dupprintf(KT_CLIP_LOG_READ_SENT,
                    clip_len, clip_len == 1 ? "" : "s",
                    term->osc52_read_served,
                    term->osc52_read_served == 1 ? "" : "s");
    logevent(term->logctx, msg);
    sfree(msg);
}

/*
 * The whole read gate, in one place and in one order, so that reading it top to
 * bottom is the specification.
 */
/*
 * OSC 5522 approvals, remembered by (password, name).
 *
 * Both halves are checked, and neither is worth anything alone. The password on
 * its own would let a later program use a token it overheard; the name on its own
 * is text the server chose, and anything can call itself "nvim". The pair is worth
 * something only because the PASSWORD was generated by us, not by the program -
 * so the pair says "the thing we approved, still calling itself the same".
 */
static bool osc5522_pw_approved(Terminal *term, const char *pw, const char *name)
{
    unsigned long now = (unsigned long)time(NULL);
    int i;
    for (i = 0; i < term->osc5522_pw_count; i++) {
        if (!term->osc5522_pw[i])
            continue;
        if (term->osc5522_pw_until[i] != 0 && now >= term->osc5522_pw_until[i])
            continue;                  /* expired; tidied up on the next write */
        if (!strcmp(term->osc5522_pw[i], pw) &&
            term->osc5522_pw_name[i] && !strcmp(term->osc5522_pw_name[i], name))
            return true;
    }
    return false;
}

static void osc5522_pw_drop(Terminal *term, int i)
{
    if (term->osc5522_pw[i]) {
        /* the password is a credential of sorts: do not leave it in freed memory */
        smemclr(term->osc5522_pw[i], strlen(term->osc5522_pw[i]));
        sfree(term->osc5522_pw[i]);
        term->osc5522_pw[i] = NULL;
    }
    sfree(term->osc5522_pw_name[i]);
    term->osc5522_pw_name[i] = NULL;
    term->osc5522_pw_until[i] = 0;
}

static void osc5522_pw_forget(Terminal *term, const char *pw)
{
    int i;
    for (i = 0; i < term->osc5522_pw_count; i++)
        if (term->osc5522_pw[i] && !strcmp(term->osc5522_pw[i], pw))
            osc5522_pw_drop(term, i);
}

static void osc5522_pw_remember(Terminal *term, const char *pw, const char *name,
                                unsigned long until)
{
    unsigned long now = (unsigned long)time(NULL);
    int i, slot = -1;

    /* reuse the entry for this password if it is already known, and clear out
     * anything that has expired while we are walking the table anyway */
    for (i = 0; i < term->osc5522_pw_count; i++) {
        if (term->osc5522_pw[i] && term->osc5522_pw_until[i] != 0 &&
            now >= term->osc5522_pw_until[i])
            osc5522_pw_drop(term, i);
        if (term->osc5522_pw[i] && !strcmp(term->osc5522_pw[i], pw))
            slot = i;
        else if (!term->osc5522_pw[i] && slot < 0)
            slot = i;
    }
    if (slot < 0) {
        if (term->osc5522_pw_count < OSC5522_MAX_APPROVALS) {
            slot = term->osc5522_pw_count++;
        } else {
            /* full: evict the first. A bounded table is the point - a host must
             * not be able to make us allocate by sending endless passwords. */
            slot = 0;
            osc5522_pw_drop(term, slot);
        }
    }
    osc5522_pw_drop(term, slot);
    term->osc5522_pw[slot] = dupstr(pw);
    term->osc5522_pw_name[slot] = dupstr(name);
    term->osc5522_pw_until[slot] = until;
}

/*
 * The gate. Protocol-neutral on purpose: OSC 52 and OSC 5522 are two front doors
 * onto exactly this function, not two permission systems that have to be kept in
 * agreement with each other.
 *
 * Returns true when the caller may send the clipboard, with *out_clip / *out_len
 * filled in (the caller wipes and frees it). Returns false otherwise, and sets
 * *err to say why in the only vocabulary a protocol can carry:
 *
 *   "EPERM"  refused - by the setting, by the focus rule, by an earlier decision,
 *            or by the user just now
 *   "EBUSY"  not refused, but we will not deal with it right now: a dialog is
 *            already open, or this host has used up its prompts
 *   NULL     nothing to send, and nothing went wrong - an empty clipboard. Not an
 *            error: kitty's own implementation answers an unavailable type with no
 *            data packet rather than a failure, and we match it.
 *
 * OSC 52 has no way to report any of these and stays silent whichever it is; only
 * OSC 5522 can pass them on, which is most of why it is worth having.
 *
 * claim is the name a program gave for itself (OSC 5522 only, NULL on OSC 52) and
 * pw is the one-time password that belongs to it. Together they are what makes
 * "do not ask me about this program again" possible at all.
 */
/* What a read asks for, and what it gets. OSC 52 asks for text only; OSC 5522
 * names MIME types, of which text/plain and image/png are the ones served. */
typedef struct ClipReadWant {
    bool text;
    bool image;
} ClipReadWant;
typedef struct ClipReadData {
    wchar_t *text;
    int text_len;
    unsigned char *png;
    size_t png_len;
} ClipReadData;

static void clip_read_free(ClipReadData *d)
{
    if (d->text) {
        smemclr(d->text, d->text_len * sizeof(wchar_t));
        sfree(d->text);
    }
    if (d->png) {
        smemclr(d->png, d->png_len);
        sfree(d->png);
    }
    memset(d, 0, sizeof(*d));
}

/* KiTTY: keep a read request to run later (ClipHeldRead, terminal.h); run by
 * clip_held_read_run, further down beside the two answers. */
static void clip_held_read_drop(ClipHeldRead *h)
{
    sfree(h->meta);
    sfree(h->payload);
    memset(h, 0, sizeof(*h));
}

static void clip_held_read_set(ClipHeldRead *h, int kind, const char *meta,
                               const char *payload, size_t len)
{
    clip_held_read_drop(h);
    h->kind = kind;
    h->meta = meta ? dupstr(meta) : NULL;
    h->payload = payload ? dupstr(payload) : NULL;
    h->len = len;
}

/* Fetch what the request wants and the clipboard has. True if anything came
 * back. *unavailable is set only when NOTHING came back because another program
 * holds the clipboard - the one case that is not an answer. */
static bool clip_read_fetch(const ClipReadWant *want, ClipReadData *d,
                            bool *unavailable)
{
    bool u1 = false, u2 = false;
    memset(d, 0, sizeof(*d));
    if (want->text) {
        d->text = kitty_osc52_get_clipboard_ex(&d->text_len, &u1);
        if (d->text && d->text_len <= 0) {
            sfree(d->text);
            d->text = NULL;
        }
    }
    if (want->image) {
        d->png = kitty_osc52_get_clipboard_png(&d->png_len, &u2);
        if (d->png && !d->png_len) {
            sfree(d->png);
            d->png = NULL;
        }
    }
    if (unavailable)
        *unavailable = (u1 || u2) && !d->text && !d->png;
    return d->text || d->png;
}

/* KiTTY: the gate's *err when the read box has been opened: not a refusal - the
 * request waits for the answer, without a reply (compared by address) */
static const char OSC52_READ_WAIT[] = "WAIT";

static bool osc52_read_gate(Terminal *term, const char *claim, const char *pw,
                            const ClipReadWant *want, ClipReadData *out,
                            const char **err)
{
    const wchar_t *clip;
    int clip_len = 0;
    unsigned long now = (unsigned long)time(NULL);
    int interval, max_served, dialog_cap;
    int grant = OSC52_GRANT_ONCE;
    bool always_deny = false, allowed = false;

    memset(out, 0, sizeof(*out));
    *err = "EPERM";

    /* 1. The setting says no. Nothing else is even looked at. */
    if (conf_get_int(term->conf, CONF_osc52_clipboard_read) != OSC52_READ_ASK) {
        osc52_read_refuse(term, KT_CLIP_WHY_DENY_SETTING, false);
        return false;
    }

    /*
     * 2. The window is not the one you are working in.
     *
     * This comes before any stored permission, because the rule is that nothing
     * leaves the window while you are elsewhere - not merely that you are not
     * asked. The stored permission is deliberately left ALONE: it is suspended,
     * not thrown away, and resumes without a second dialog when you come back.
     */
    if (conf_get_bool(term->conf, CONF_clipboard_require_focus) && !term->has_focus) {
        if (term->osc52_read_decision > 0)
            kitty_osc52_state_changed(term);   /* show it as paused */
        osc52_read_refuse(term, KT_CLIP_WHY_NO_FOCUS, false);
        return false;
    }

    /* 3. A refusal is in force. "Deny for the next ten minutes" is also how you
     * get rid of a host that will not stop asking, which is why the dialog's
     * duration choice applies to Deny and not only to Allow. */
    if (term->osc52_read_decision < 0 && !osc52_read_decision_expired(term)) {
        if (term->osc52_read_remaining > 0)
            term->osc52_read_remaining--;
        osc52_read_refuse(term, KT_CLIP_WHY_REFUSED_EARLIER, false);
        return false;
    }
    if (osc52_read_decision_expired(term))
        osc52_read_forget_decision(term);

    /*
     * 3a. A paste event's one-time token. The user pressed Paste, and that
     * keypress is the permission: the application was told the clipboard's
     * types together with this token, and reads with it once, within seconds,
     * under the name the spec prescribes ("Paste event"). Single use whether
     * or not it is still fresh - a token that expired is not tried again. A
     * stale token falls through to the ordinary rules, which ask.
     *
     * Placed after the standing refusal on purpose: "deny this host for ten
     * minutes" is a decision the user made, and a paste while it stands gets
     * the text pasted (the event is not sent at all when reads are Deny) or a
     * refusal, never a silent hand-over.
     */
    if (pw && *pw && claim && !strcmp(claim, "Paste event") &&
        term->osc5522_paste_pw && !strcmp(pw, term->osc5522_paste_pw)) {
        bool fresh = now <= term->osc5522_paste_pw_until;
        smemclr(term->osc5522_paste_pw, strlen(term->osc5522_paste_pw));
        sfree(term->osc5522_paste_pw);
        term->osc5522_paste_pw = NULL;
        if (fresh) {
            if (!clip_read_fetch(want, out, NULL)) {
                *err = NULL;
                return false;
            }
            return true;
        }
    }

    /*
     * 3b. This exact program has been approved before, by name and by the
     * one-time password it was given, and that approval has not expired.
     *
     * This is the whole point of OSC 5522 and the one thing OSC 52 cannot do: a
     * bare OSC 52 read carries no identity, so there is nothing to attach an
     * approval to. It is also the answer to the real-world failure this design was
     * built around - an editor polling the clipboard over SSH producing a prompt
     * storm - because the editor is now something we can recognise.
     *
     * The approval is checked on BOTH the password and the name. The password
     * alone would let a later program claim a token it overheard; the name alone
     * is text the server chose and anything can call itself nvim. Neither is
     * worth much by itself and the pair is only worth anything because the
     * terminal, not the program, generated the password.
     *
     * It grants nothing that the rest of this function would not: the focus rule
     * above has already been applied, and the hand-over limits below still are.
     */
    if (pw && *pw && claim && *claim && osc5522_pw_approved(term, pw, claim)) {
        max_served = conf_get_int(term->conf, CONF_osc52_read_max);
        interval = conf_get_int(term->conf, CONF_osc52_read_interval);
        /* Same split as the dialog-granted permission below: pacing refuses the
         * request, the ceiling ends the approval. An approved program that is
         * merely chatty must not lose its approval and start prompting again. */
        if (interval > 0 && term->osc52_read_last_served != 0 &&
            now - term->osc52_read_last_served < (unsigned long)interval) {
            osc52_read_refuse(term, KT_CLIP_WHY_PROGRAM_TOO_SOON, false);
            return false;
        }
        if (max_served > 0 && term->osc52_read_served >= max_served) {
            osc5522_pw_forget(term, pw);
            logevent(term->logctx, KT_CLIP_LOG_APPROVAL_WITHDRAWN);
            osc52_read_refuse(term, KT_CLIP_WHY_PROGRAM_LIMIT, true);
            return false;
        }
        if (!clip_read_fetch(want, out, NULL)) {
            *err = NULL;
            return false;
        }
        return true;
    }

    /* 4. A grant is in force - but a grant is permission for a read, not for a
     * tap on the clipboard. */
    if (term->osc52_read_decision > 0) {
        max_served = conf_get_int(term->conf, CONF_osc52_read_max);
        interval = conf_get_int(term->conf, CONF_osc52_read_interval);

        /*
         * The two limits are NOT the same kind of thing, and they were treated
         * as one until a tester met the result.
         *
         * The minimum gap is PACING. Tripping it means the host asked again too
         * soon - a chatty editor does this - and the answer is to refuse that one
         * request and carry on. It must NOT touch the grant. Withdrawing here and
         * falling through to a fresh dialog was the original behaviour and it was
         * wrong twice over: it overrode a decision the user had explicitly made
         * ("ten minutes" means ten minutes, not "until the host is impatient"),
         * and it turned a rate limit into the prompt storm this whole design
         * exists to prevent - a host asking every second produced a dialog every
         * second, which is worse than no rate limit at all.
         *
         * The count is a CEILING, not pacing: it is the whole-window backstop
         * that stops "allow for ten minutes" amounting to everything the user
         * copies in ten minutes. Reaching it does end the grant, and the next
         * request asks again - once, because the count then starts over.
         */
        if (max_served > 0 && term->osc52_read_served >= max_served) {
            logevent(term->logctx, KT_CLIP_LOG_PERMISSION_WITHDRAWN);
            osc52_read_forget_decision(term);
        } else if (interval > 0 && term->osc52_read_last_served != 0 &&
                   now - term->osc52_read_last_served < (unsigned long)interval) {
            /* refuse this one; the grant stands and the user is not asked */
            osc52_read_refuse(term, KT_CLIP_WHY_TOO_SOON, false);
            return false;
        } else {
            if (term->osc52_read_remaining > 0)
                term->osc52_read_remaining--;
            if (!clip_read_fetch(want, out, NULL)) {
                *err = NULL;
                return false;
            }
            return true;
        }
    }

    /* KiTTY: the read box has been answered, and this is the request that
     * waited for it, run again (term_osc52_read_answer). The stored answer
     * decides, over the clipboard the box showed. The rules above were
     * applied again on the way, so a setting changed while the box was open
     * has had its say. */
    if (term->osc52_read_answered) {
        term->osc52_read_answered = false;
        allowed = term->osc52_read_ans_allowed;
        grant = term->osc52_read_ans_grant;
        always_deny = term->osc52_read_ans_always_deny;
        out->text = term->osc52_read_held_text;
        out->text_len = term->osc52_read_held_text_len;
        out->png = term->osc52_read_held_png;
        out->png_len = term->osc52_read_held_png_len;
        term->osc52_read_held_text = NULL;
        term->osc52_read_held_text_len = 0;
        term->osc52_read_held_png = NULL;
        term->osc52_read_held_png_len = 0;
        goto decided;
    }

    /* 5. The prompt itself is an attack surface, so it is rationed. A dialog
     * already open for this window means the next request is refused, not
     * stacked behind it. */
    if (term->osc52_read_asking) {
        osc52_read_refuse(term, KT_CLIP_WHY_DIALOG_OPEN, false);
        *err = "EBUSY";
        return false;
    }
    dialog_cap = conf_get_int(term->conf, CONF_osc52_read_dialogs);
    if (term->osc52_read_prompt_window == 0 ||
        now - term->osc52_read_prompt_window >= 10) {
        term->osc52_read_prompt_window = now;
        term->osc52_read_prompts = 0;
    }
    if (dialog_cap > 0 && term->osc52_read_prompts >= dialog_cap) {
        osc52_read_refuse(term, KT_CLIP_WHY_ASKING_REPEATEDLY, true);
        *err = "EBUSY";
        return false;
    }

    /* 6. Ask. The clipboard is fetched now because the dialog shows a masked
     * summary of it - how much, and the first few characters - and because there
     * is no point asking about an empty clipboard. */
    bool clip_unavailable = false;
    bool clip_any = clip_read_fetch(want, out, &clip_unavailable);
    {
        if (clip_unavailable) {
            /* Another program has the clipboard open. That is not a decision
             * about this request, so it is refused WITH a reason rather than in
             * the silence an empty clipboard gets - otherwise a request that
             * lands at the wrong moment vanishes without trace. */
            osc52_read_refuse(term, KT_CLIP_WHY_UNAVAILABLE, true);
            *err = "EBUSY";
            return false;
        }
    }
    if (!clip_any) {
        /* Nothing to send. Refuse in silence and do NOT ask: a dialog about an
         * empty clipboard is a dialog that trains people to click Allow. */
        *err = NULL;
        return false;
    }

    {
        wchar_t *preview = NULL;

        term->osc52_read_asking = true;
        term->osc52_read_prompts++;
        /* claim is NULL on OSC 52, which carries no program name, and the dialog
         * must not imply one. On OSC 5522 it is the name the program chose for
         * itself, which the dialog words as a claim rather than a fact.
         * The dialog previews the clipboard as text; an image gets a one-line
         * description in its place. */
        clip = out->text;
        clip_len = out->text_len;
        if (!clip) {
            char *s = dupprintf(KT_CLIP_PREVIEW_IMAGE, (unsigned long)out->png_len);
            preview = decode_utf8_to_wide_string(s);
            sfree(s);
            clip = preview;
            clip_len = (int)wcslen(preview);
        }
        kitty_osc52_read_confirm(term, clip, clip_len, claim);
        sfree(preview);
        clip = NULL;
        /*
         * KiTTY: the box is MODELESS. It used to be a modal DialogBoxParam
         * opened from here, inside the parser: output stopped until it was
         * answered, and keys and repaints could reach the terminal in the
         * middle of a parse. Now the request waits (the callers keep it in
         * osc52_read_wait) and so does what the box shows - an Allow sends
         * exactly that. The answer runs the request again, which arrives
         * above with osc52_read_answered set.
         */
        term->osc52_read_held_text = out->text;
        term->osc52_read_held_text_len = out->text_len;
        term->osc52_read_held_png = out->png;
        term->osc52_read_held_png_len = out->png_len;
        memset(out, 0, sizeof(*out));
        *err = OSC52_READ_WAIT;
        return false;
    }

  decided:
    {
        if (always_deny) {
            if (kitty_osc52_save_deny_for_host(term)) {
                conf_set_int(term->conf, CONF_osc52_clipboard_read,
                             OSC52_READ_DENY);
                logevent(term->logctx, KT_CLIP_LOG_DENY_SAVED);
            } else {
                /* There is nowhere to write it: someone typed a hostname into
                 * the config box and connected. Say so (the dialog does) and let
                 * the refusal stand for the rest of this window - we do not
                 * invent an invisible host list, because an unseen permission
                 * record is the thing this design exists to avoid. Under
                 * readonly=yes the refusal has its own line already. */
                if (!kitty_osc52_store_readonly())
                    logevent(term->logctx, KT_CLIP_LOG_DENY_UNSAVED);
                allowed = false;
                grant = OSC52_GRANT_SESSION;
            }
        }

        /*
         * Record the answer and when it stops applying. A timeout or a dismissed
         * dialog arrives here as deny-once, which is the right reading of it:
         * nobody decided anything, so nothing is remembered and the next request
         * asks again.
         *
         * identifiable: this request said who it was, and was believed enough to
         * be worth remembering. An ALLOW then applies to THAT PROGRAM ONLY and is
         * NOT recorded as a permission for the window - otherwise saying yes to
         * "nvim, for the rest of the session" would also let an anonymous OSC 52
         * read succeed for the rest of the session, which is not what was
         * approved and is not what the dialog said.
         *
         * A REFUSAL is recorded window-wide either way. Refusing more than was
         * asked about is never the wrong way to be wrong.
         */
        bool identifiable = (allowed && pw && *pw && claim && *claim &&
                             grant != OSC52_GRANT_ONCE);
        term->osc52_read_until = 0;
        term->osc52_read_remaining = 0;
        switch (grant) {
          case OSC52_GRANT_MINUTES: {
            int mins = conf_get_int(term->conf, CONF_osc52_read_minutes);
            if (mins <= 0) mins = 10;
            term->osc52_read_until = now + (unsigned long)mins * 60;
            if (!identifiable) {
                term->osc52_read_remaining = -1;
                term->osc52_read_decision = allowed ? 1 : -1;
            }
            break;
          }
          case OSC52_GRANT_REQUESTS: {
            int n = conf_get_int(term->conf, CONF_osc52_read_requests);
            if (n <= 0) n = 25;
            if (!identifiable) {
                term->osc52_read_remaining = n;
                term->osc52_read_decision = allowed ? 1 : -1;
            }
            break;
          }
          case OSC52_GRANT_SESSION:
            if (!identifiable) {
                term->osc52_read_remaining = -1;
                term->osc52_read_decision = allowed ? 1 : -1;
            }
            break;
          case OSC52_GRANT_ONCE:
          default:
            /* one request, and this is it: nothing is stored either way */
            term->osc52_read_decision = 0;
            break;
        }

        if (!allowed) {
            clip_read_free(out);
            osc52_read_refuse(term, KT_CLIP_WHY_USER_NO, false);
            return false;
        }
        if (term->osc52_read_decision > 0) {
            kitty_osc52_state_changed(term);
            if (conf_get_bool(term->conf, CONF_clipboard_notify))
                kitty_osc52_notify(term, KT_CAP_CLIPBOARD,
                                   KT_CLIP_NOTICE_READ_GRANTED,
                                   CLIP_BALLOON_LOG);
        }
        /*
         * A named program that was allowed for more than this one request gets its
         * password remembered, so it is not asked about again while that lasts.
         * In memory only, and for exactly as long as the grant the user chose -
         * the design's rule that no permission to read is ever written to disk
         * applies to this as much as to the per-window grant. A request-counted
         * grant becomes a time-unbounded approval for that program, because a
         * count is kept on the window and there is nowhere to hang it here; the
         * hand-over limits still apply to it, which is what actually bounds it.
         */
        if (identifiable)
            osc5522_pw_remember(term, pw, claim, term->osc52_read_until);
        return true;
    }
}

/*
 * OSC 52's front door onto the gate above. OSC 52 cannot report an error, so
 * every refusal looks the same from the host's side: silence. That is deliberate -
 * any reply would confirm to a probing host both that the feature exists and that
 * somebody was there to answer.
 */
static void osc52_read_clipboard(Terminal *term)
{
    ClipReadWant want = { true, false };   /* OSC 52 carries text and nothing else */
    ClipReadData data;
    const char *err = NULL;

    if (!osc52_read_gate(term, NULL, NULL, &want, &data, &err)) {
        /* KiTTY: the read box is open - this request waits for its answer */
        if (err == OSC52_READ_WAIT)
            clip_held_read_set(&term->osc52_read_wait, 1, NULL, NULL, 0);
        return;
    }
    osc52_read_send(term, data.text, data.text_len);
    clip_read_free(&data);
}

/*
 * ===========================================================================
 * KiTTY: OSC 5522 - the kitty clipboard protocol
 * ===========================================================================
 *
 * Form:  OSC 5522 ; <metadata> ; <payload> ST
 *
 * The metadata is COLON-separated key=value pairs; the semicolon separates the
 * metadata from the payload. That distinction is easy to get backwards from a
 * prose description and it is why this was written against the specification
 * rather than a summary of it. So is which values are base64: `mime`, `pw` and
 * `name` are base64-encoded, while `type`, `status`, `loc` and `id` are plain.
 *
 * What this handles: the READ direction, which is the reason the protocol is worth
 * having to us. It fixes the three real weaknesses of an OSC 52 read - it can say
 * who is asking (`pw` + `name`, hence the approvals table above), it has real
 * error codes instead of silence, and it can describe content types. It is the
 * same permission gate; there is deliberately not a second one.
 *
 * And the WRITE direction (`type=write` / `wdata` / `walias`), further down: a
 * chunked multi-sequence transaction that may carry any MIME type, each of which
 * becomes a clipboard format. It goes through the OSC 52 write gate, so the one
 * decision "may this host set my clipboard" covers both protocols.
 *
 * base64 is validated STRICTLY here (RFC 4648: the standard alphabet, padding
 * required, nothing after it), as the specification's "Encoding of payloads"
 * section requires: a terminal must not silently drop invalid characters, which
 * is what PuTTY's decoder does - it skips line breaks and yields nothing for a bad
 * group. So nothing reaches that decoder without passing osc5522_b64_valid()
 * first. An invalid READ is ignored without a reply; an invalid WRITE packet is
 * answered EINVAL and the transaction discarded - both as specified. (OSC 52 is
 * different on purpose: it tolerates missing padding, as kitty does, because it
 * cannot report anything and its senders are older and sloppier.)
 *
 * And paste-events mode (private mode 5522), further down: while an application
 * has it set, a local Paste sends the clipboard's type list and a one-time token
 * instead of the text. Bounded by a token lifetime, a title marker and an
 * auto-disarm clock (OSC5522PasteMinutes).
 */
#define OSC5522_CHUNK 4096             /* spec: bytes per chunk BEFORE base64 */
/* clip_write_gate's answer: refused, allowed, or waiting for the write
 * confirmation box (the caller holds the write until term_osc52_write_answer) */
enum { CLIP_GATE_NO, CLIP_GATE_YES, CLIP_GATE_WAIT };
static int clip_write_gate(Terminal *term, const char **err);

/* Pull one plain (not base64) metadata value out of the colon-separated list.
 * 1 = found, 0 = absent, -1 = present but longer than the buffer. The last is
 * reported rather than truncated: under strict validation a value we could not
 * hold whole is a value we cannot judge, and a request carrying it is refused or
 * ignored, never acted on in part. */
static int osc5522_meta(const char *meta, const char *key, char *out, size_t outlen)
{
    size_t keylen = strlen(key);
    const char *p = meta;

    out[0] = '\0';
    while (*p) {
        const char *end = strchr(p, ':');
        size_t seglen = end ? (size_t)(end - p) : strlen(p);
        if (seglen > keylen && !memcmp(p, key, keylen) && p[keylen] == '=') {
            size_t vlen = seglen - keylen - 1;
            if (vlen >= outlen)
                return -1;
            memcpy(out, p + keylen + 1, vlen);
            out[vlen] = '\0';
            return 1;
        }
        if (!end)
            break;
        p = end + 1;
    }
    return 0;
}

/* The value of one base64 character, or -1. The standard alphabet only: the
 * spec names RFC 4648 section 4, so the URL-safe alphabet is not accepted. */
static int osc5522_b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* Decode one complete quad. Returns the number of bytes (1..3), or 0 for a quad
 * that is not base64: a character outside the alphabet, '=' anywhere but the
 * last two places, or "=x". *ended is set when the quad carried padding, after
 * which nothing more may follow for that value. */
static int osc5522_b64_quad(const char q[4], unsigned char *out, bool *ended)
{
    int v[4], i, n = 3;
    if (q[0] == '=' || q[1] == '=')
        return 0;
    if (q[2] == '=') {
        if (q[3] != '=')
            return 0;
        n = 1;
    } else if (q[3] == '=') {
        n = 2;
    }
    for (i = 0; i < 4; i++) {
        v[i] = q[i] == '=' ? 0 : osc5522_b64_val(q[i]);
        if (v[i] < 0)
            return 0;
    }
    out[0] = (unsigned char)((v[0] << 2) | (v[1] >> 4));
    out[1] = (unsigned char)(((v[1] & 15) << 4) | (v[2] >> 2));
    out[2] = (unsigned char)(((v[2] & 3) << 6) | v[3]);
    *ended = (n < 3);
    return n;
}

/* Strict RFC 4648 validity of a whole value: length a multiple of four, the
 * standard alphabet, padding only at the end. The empty string is valid; callers
 * decide whether empty is meaningful. Whitespace and line breaks are invalid, as
 * the spec requires - they must not be silently skipped. */
static bool osc5522_b64_valid(const char *s, size_t len)
{
    size_t i;
    unsigned char scratch[3];
    bool ended = false;
    if (len % 4)
        return false;
    for (i = 0; i < len; i += 4) {
        if (ended)
            return false;              /* data after padding */
        if (!osc5522_b64_quad(s + i, scratch, &ended))
            return false;
    }
    return true;
}

/* Decode an already-validated base64 value. Caller frees the strbuf. */
static strbuf *osc5522_b64_decode(const char *s, size_t len)
{
    strbuf *sb = strbuf_new_nm();
    size_t i;
    unsigned char out[3];
    bool ended;
    for (i = 0; i < len; i += 4)
        put_data(sb, out, osc5522_b64_quad(s + i, out, &ended));
    return sb;
}

/* Is this valid UTF-8 with no control characters? Used on the names a write may
 * carry - MIME types and their aliases - which become Windows clipboard format
 * names. kitty rejects an alias list that is not valid UTF-8 with EINVAL, and so
 * does this; control characters are refused on top because a format name with a
 * line break in it is never anything but trouble. */
static bool osc5522_utf8_ok(const char *s, size_t len)
{
    size_t i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        int extra, k;
        if (c < 0x20 || c == 0x7F)
            return false;
        if (c < 0x80) { i++; continue; }
        if ((c & 0xE0) == 0xC0 && c >= 0xC2) extra = 1;
        else if ((c & 0xF0) == 0xE0) extra = 2;
        else if ((c & 0xF8) == 0xF0 && c <= 0xF4) extra = 3;
        else return false;
        if (i + extra >= len)
            return false;              /* truncated sequence */
        for (k = 1; k <= extra; k++)
            if (((unsigned char)s[i + k] & 0xC0) != 0x80)
                return false;
        i += extra + 1;
    }
    return true;
}

/* A single name (a MIME type or an alias): non-empty, bounded, and the above. */
static bool osc5522_name_ok(const char *s, size_t len)
{
    return len > 0 && len <= 255 && osc5522_utf8_ok(s, len);
}

/* Decode a base64 metadata value that names something (mime=). NULL if it is not
 * strict base64 or not an acceptable name. Caller frees. */
static char *osc5522_decode_name(const char *b64)
{
    size_t len = strlen(b64);
    strbuf *sb;
    char *ret;
    if (!len || !osc5522_b64_valid(b64, len))
        return NULL;
    sb = osc5522_b64_decode(b64, len);
    if (!osc5522_name_ok(sb->s, sb->len) || strlen(sb->s) != sb->len) {
        strbuf_free(sb);
        return NULL;
    }
    ret = strbuf_to_str(sb);
    return ret;
}

/* The request id, if it is one we may echo. `id` is plain and its character set
 * is restricted by the spec ([a-zA-Z0-9-_+.]); anything else is NOT echoed,
 * because it ends up in sequences we generate and must not be able to carry a
 * separator into them. Returns the id in out ("" if none or unusable). The one
 * place this is decided, for reads and writes alike - the two used to disagree
 * about '+' and '.'. */
static void osc5522_id(const char *meta, char *out, size_t outlen)
{
    const char *q;
    out[0] = '\0';
    if (osc5522_meta(meta, "id", out, outlen) != 1)
        goto bad;
    for (q = out; *q; q++)
        if (!((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') ||
              (*q >= '0' && *q <= '9') ||
              *q == '-' || *q == '_' || *q == '+' || *q == '.'))
            goto bad;
    return;
  bad:
    out[0] = '\0';
}

/* Send one OSC 5522 sequence. payload_b64 may be NULL for a metadata-only reply.
 * ST (ESC \) terminates it, which is what the specification uses throughout. */
static void osc5522_send(Terminal *term, const char *meta,
                         const char *payload_b64, size_t payload_len)
{
    strbuf *sb = strbuf_new_nm();
    put_dataz(sb, "\033]5522;");
    put_dataz(sb, meta);
    if (payload_b64) {
        put_byte(sb, ';');
        put_data(sb, payload_b64, payload_len);
    }
    put_dataz(sb, "\033\\");
    kitty_osc52_send_raw(term, sb->s, sb->len);
    strbuf_free(sb);
}

/* base64 of a plain string, for the mime= field and for payloads. */
static char *osc5522_b64(const char *s, size_t len)
{
    strbuf *sb = strbuf_new_nm();
    char *ret;
    base64_encode_bs(BinarySink_UPCAST(sb), make_ptrlen(s, len), 0);
    ret = strbuf_to_str(sb);
    return ret;
}

/*
 * The MIME types we can actually produce. Windows has one clipboard and OSC 5522
 * gets text out of it; anything else belongs to the write direction, which is not
 * built. Both spellings are offered because an application may ask for either and
 * refusing the parameterised form over a detail of spelling would be unhelpful -
 * kitty itself does an exact string match, so being liberal here can only help.
 */
static bool osc5522_mime_is_text(const char *mime, size_t len)
{
    static const char *const texts[] = {
        "text/plain",
        "text/plain;charset=utf-8",
        "text/plain;charset=UTF-8",
    };
    size_t i;
    for (i = 0; i < lenof(texts); i++)
        if (strlen(texts[i]) == len && !memcmp(mime, texts[i], len))
            return true;
    return false;
}

/* Does the requested space-separated list contain a type we can produce? */
static bool osc5522_want_has_text(const char *want)
{
    const char *p = want;
    while (*p) {
        const char *sp = strchr(p, ' ');
        size_t n = sp ? (size_t)(sp - p) : strlen(p);
        if (n && osc5522_mime_is_text(p, n))
            return true;
        if (!sp)
            break;
        p = sp + 1;
    }
    return false;
}

/* ...and image/png, the one image type served: the Windows clipboard offers a
 * PNG directly when a browser or image editor put it there, and a bitmap is
 * encoded to PNG otherwise. Other image types are not offered - a client that
 * wants one has a PNG to convert from, and one honest type beats a list of
 * conversions nobody asked for. */
static bool osc5522_want_has_png(const char *want)
{
    const char *p = want;
    while (*p) {
        const char *sp = strchr(p, ' ');
        size_t n = sp ? (size_t)(sp - p) : strlen(p);
        if (n == 9 && !memcmp(p, "image/png", 9))
            return true;
        if (!sp)
            break;
        p = sp + 1;
    }
    return false;
}

/* One type's DATA packets, 4096 bytes each before base64. */
static void osc5522_send_chunks(Terminal *term, const char *idpart,
                                const char *mime, const char *bytes, size_t total)
{
    char *m = osc5522_b64(mime, strlen(mime));
    char reply[128];
    size_t off = 0;
    while (off < total) {
        size_t n = total - off;
        char *p;
        if (n > OSC5522_CHUNK)
            n = OSC5522_CHUNK;
        p = osc5522_b64(bytes + off, n);
        snprintf(reply, sizeof(reply), "type=read%s:status=DATA:mime=%s", idpart, m);
        osc5522_send(term, reply, p, strlen(p));
        smemclr(p, strlen(p));
        sfree(p);
        off += n;
    }
    sfree(m);
}

static void osc5522_read(Terminal *term, const char *meta,
                         const char *payload, size_t payload_len)
{
    char pwb64[512], nameb64[256], loc[32];
    char *pw = NULL, *name = NULL, *want = NULL;
    wchar_t *clip = NULL;
    int clip_len = 0;
    const char *err = NULL;
    char reply[128];
    char idpart[64];
    strbuf *decoded;
    ClipReadWant rw;
    ClipReadData data;

    /* `id` is echoed back on every packet of this transaction so a multiplexer can
     * match replies to requests. Plain, and its character set is restricted by the
     * spec - anything else is not echoed, because it ends up in sequences we
     * generate and must not be able to carry a separator into them. */
    idpart[0] = '\0';
    {
        char id[48];
        osc5522_id(meta, id, sizeof(id));
        if (*id)
            snprintf(idpart, sizeof(idpart), ":id=%s", id);
    }

    /*
     * STRICT base64 first, before anything is decoded or compared: the payload
     * (the wanted types), the password and the name. A read that fails it is
     * IGNORED - no reply - which is what the spec says an invalid read gets, and
     * what kitty's tests pin ("a malformed read packet must not abort the write
     * request" is checked below too: this returns before touching anything).
     * A value too long for its buffer counts as invalid for the same reason.
     */
    if (payload && payload_len && !osc5522_b64_valid(payload, payload_len))
        return;
    {
        int rp = osc5522_meta(meta, "pw", pwb64, sizeof(pwb64));
        int rn = osc5522_meta(meta, "name", nameb64, sizeof(nameb64));
        if (rp < 0 || rn < 0)
            return;
        if (rp > 0 && *pwb64 && !osc5522_b64_valid(pwb64, strlen(pwb64)))
            return;
        if (rn > 0 && *nameb64 && !osc5522_b64_valid(nameb64, strlen(nameb64)))
            return;
    }

    /*
     * We serve the system clipboard only. loc=primary is the X11 primary
     * selection, which Windows does not have, so it is answered ENOSYS rather
     * than quietly served from the wrong place - a host asking for the selection
     * and receiving the clipboard would be a worse answer than a refusal.
     */
    if (osc5522_meta(meta, "loc", loc, sizeof(loc)) == 1 && !strcmp(loc, "primary")) {
        snprintf(reply, sizeof(reply), "type=read%s:status=ENOSYS", idpart);
        osc5522_send(term, reply, NULL, 0);
        return;
    }

    if (*pwb64) {
        /*
         * The password is kept in its base64 form for comparison - we never need
         * its plaintext, and not decoding it is one less copy of a credential
         * lying around. It is compared VERBATIM: base64 is validated strictly
         * above, so a padded and an unpadded spelling can no longer both arrive,
         * and there is nothing left to normalise.
         */
        pw = pwb64;
        if (*nameb64) {
            decoded = osc5522_b64_decode(nameb64, strlen(nameb64));
            name = dupstr(decoded->s);
            strbuf_free(decoded);
        }
    }

    /* The payload is a base64 space-separated list of wanted MIME types, or "."
     * to ask what is available. */
    if (payload && payload_len) {
        decoded = osc5522_b64_decode(payload, payload_len);
        want = dupstr(decoded->s);
        strbuf_free(decoded);
    } else {
        want = dupstr("");
    }

    /*
     * Asking what types are available is specified to happen WITHOUT a permission
     * prompt, so that an application is not asked twice for one paste. It is still
     * gated on the setting and on focus: the answer tells a host whether there is
     * text on your clipboard, which is little, but it is not nothing, and the
     * focus rule has no exceptions in it by design.
     */
    if (!strcmp(want, ".")) {
        bool have_text, have_image;
        if (conf_get_int(term->conf, CONF_osc52_clipboard_read) != OSC52_READ_ASK ||
            (conf_get_bool(term->conf, CONF_clipboard_require_focus) &&
             !term->has_focus)) {
            snprintf(reply, sizeof(reply), "type=read%s:status=EPERM", idpart);
            osc5522_send(term, reply, NULL, 0);
            goto out;
        }
        clip = kitty_osc52_get_clipboard(&clip_len);
        have_text = (clip && clip_len > 0);
        if (clip) {
            smemclr(clip, clip_len * sizeof(wchar_t));
            sfree(clip);
            clip = NULL;
        }
        have_image = kitty_osc52_clipboard_has_image();
        snprintf(reply, sizeof(reply), "type=read%s:status=OK", idpart);
        osc5522_send(term, reply, NULL, 0);
        if (have_text || have_image) {
            /* mime= carries "." for this reply and the payload is the space
             * separated list with a trailing newline, matching kitty. */
            char *m = osc5522_b64(".", 1);
            char list[128];
            char *p;
            snprintf(list, sizeof(list), "%s%s%s\n",
                     have_text ? "text/plain text/plain;charset=utf-8" : "",
                     have_text && have_image ? " " : "",
                     have_image ? "image/png" : "");
            p = osc5522_b64(list, strlen(list));
            snprintf(reply, sizeof(reply),
                     "type=read%s:status=DATA:mime=%s", idpart, m);
            osc5522_send(term, reply, p, strlen(p));
            sfree(m);
            sfree(p);
        }
        snprintf(reply, sizeof(reply), "type=read%s:status=DONE", idpart);
        osc5522_send(term, reply, NULL, 0);
        goto out;
    }

    /* A request for actual content. Anything we cannot produce simply gets no data
     * packet - kitty answers an unavailable type that way rather than with a
     * failure, and an error here would be a worse description of what happened. */
    rw.text = osc5522_want_has_text(want);
    rw.image = osc5522_want_has_png(want);
    if (!*want || (!rw.text && !rw.image)) {
        snprintf(reply, sizeof(reply), "type=read%s:status=OK", idpart);
        osc5522_send(term, reply, NULL, 0);
        snprintf(reply, sizeof(reply), "type=read%s:status=DONE", idpart);
        osc5522_send(term, reply, NULL, 0);
        goto out;
    }

    if (!osc52_read_gate(term, name, pw, &rw, &data, &err)) {
        if (err == OSC52_READ_WAIT) {
            /* KiTTY: the read box is open - this request waits for its
             * answer, its reply with it (term_osc52_read_answer runs it again) */
            clip_held_read_set(&term->osc52_read_wait, 2, meta, payload,
                               payload_len);
        } else if (err) {
            snprintf(reply, sizeof(reply), "type=read%s:status=%s", idpart, err);
            osc5522_send(term, reply, NULL, 0);
        } else {
            /* nothing to send, and nothing wrong: an empty clipboard */
            snprintf(reply, sizeof(reply), "type=read%s:status=OK", idpart);
            osc5522_send(term, reply, NULL, 0);
            snprintf(reply, sizeof(reply), "type=read%s:status=DONE", idpart);
            osc5522_send(term, reply, NULL, 0);
        }
        goto out;
    }

    /* Serve it, chunked at the size the specification requires: all of one
     * type, then the next, as the spec orders them. One hand-over however many
     * types went out - the limits count requests, not formats. */
    snprintf(reply, sizeof(reply), "type=read%s:status=OK", idpart);
    osc5522_send(term, reply, NULL, 0);
    if (data.text) {
        char *utf8 = encode_wide_string_as_utf8(data.text);
        osc5522_send_chunks(term, idpart, "text/plain", utf8, strlen(utf8));
        smemclr(utf8, strlen(utf8));
        sfree(utf8);
    }
    if (data.png)
        osc5522_send_chunks(term, idpart, "image/png",
                            (const char *)data.png, data.png_len);
    snprintf(reply, sizeof(reply), "type=read%s:status=DONE", idpart);
    osc5522_send(term, reply, NULL, 0);

    term->osc52_read_served++;
    term->osc52_read_last_served = (unsigned long)time(NULL);
    clip_note_activity(term, CLIP_ACT_READ);
    if (data.text) {
        char *msg = dupprintf(KT_CLIP_LOG_READ_SENT_5522,
                              data.text_len, data.text_len == 1 ? "" : "s",
                              name ? KT_CLIP_LOG_PROGRAM_CALLS_ITSELF : "", name ? name : "",
                              term->osc52_read_served,
                              term->osc52_read_served == 1 ? "" : "s");
        logevent(term->logctx, msg);
        sfree(msg);
    }
    if (data.png) {
        char *msg = dupprintf(KT_CLIP_LOG_READ_SENT_5522_IMAGE,
                              (unsigned long)data.png_len,
                              name ? KT_CLIP_LOG_PROGRAM_CALLS_ITSELF : "", name ? name : "",
                              term->osc52_read_served,
                              term->osc52_read_served == 1 ? "" : "s");
        logevent(term->logctx, msg);
        sfree(msg);
    }
    clip_read_free(&data);

  out:
    if (clip) {
        smemclr(clip, clip_len * sizeof(wchar_t));
        sfree(clip);
    }
    sfree(name);
    sfree(want);
}

/*
 * ---------------------------------------------------------------------------
 * OSC 5522 WRITE direction: the host putting data on the local clipboard.
 *
 *   type=write[:id=..][:loc=..]                  open a transaction
 *   type=wdata:mime=<b64>;<b64 chunk>            data for one MIME type
 *   type=walias:mime=<b64 target>;<b64 "a b">    extra names for a type's data
 *   type=wdata                                   end: set the clipboard, DONE
 *
 * Replies carry the id of the type=write packet, status first, the way kitty lays
 * its own out: type=write:status=EINVAL:id=w1. After any error the transaction is
 * dropped and the rest of its packets are ignored in silence until the next
 * type=write - the spec's rule, so a client that missed the error is not told
 * twice. A new type=write replaces whatever was in flight.
 *
 * The permission gate is the OSC 52 write gate, applied when the transaction
 * OPENS rather than when it commits: a refused write should be refused before
 * 64 MB of it has arrived, the spec allows an error at any time, and the user
 * decision "may this host set my clipboard" is one decision for both protocols.
 *
 * base64 is checked per character as chunks arrive and for padding on the
 * concatenation of a type's chunks, so a chunk boundary may fall anywhere (kitty
 * splits at odd offsets in its tests). Decoding is streamed a quad at a time and
 * only the decoded bytes are kept, so a 64 MB write costs 64 MB, not 64 MB plus
 * its base64.
 *
 * Errors: EINVAL - a malformed packet (bad base64 anywhere, data without a type,
 * an alias without a target, a name that is not UTF-8), the whole write is
 * discarded as specified; EFBIG - more than the limit, incrementally, so the
 * excess is never held; ENOSYS - loc=primary, which Windows has not got; EPERM /
 * EBUSY - the gate; EIO - the clipboard could not be set.
 */

/* The most one write may carry, decoded. The spec's 64 MB floor or the user's
 * ClipboardMaxMB, whichever is larger: the setting may raise it, never lower it
 * into non-compliance. (The setting stays exactly what it was for OSC 52 and
 * far2l.) */
static size_t osc5522_write_limit(Terminal *term)
{
    size_t lim = clip_ceiling_bytes(term);
    size_t floor_bytes = (size_t)OSC5522_WRITE_MIN_MB * 1024 * 1024;
    if (term->osc5522_w_limit_override)
        return term->osc5522_w_limit_override;
    return lim > floor_bytes ? lim : floor_bytes;
}

void osc5522_write_reset(Terminal *term)
{
    int i;
    for (i = 0; i < term->osc5522_w_ntypes; i++) {
        sfree(term->osc5522_w_type[i].mime);
        if (term->osc5522_w_type[i].data) {
            /* wiped, not merely freed: what a host puts on a clipboard is as
             * likely to be a password as anything else */
            smemclr(term->osc5522_w_type[i].data->u,
                    term->osc5522_w_type[i].data->len);
            strbuf_free(term->osc5522_w_type[i].data);
        }
        term->osc5522_w_type[i].mime = NULL;
        term->osc5522_w_type[i].data = NULL;
    }
    term->osc5522_w_ntypes = 0;
    for (i = 0; i < term->osc5522_w_naliases; i++) {
        sfree(term->osc5522_w_alias[i]);
        sfree(term->osc5522_w_alias_target[i]);
        term->osc5522_w_alias[i] = NULL;
        term->osc5522_w_alias_target[i] = NULL;
    }
    term->osc5522_w_naliases = 0;
    term->osc5522_w_bytes = 0;
    term->osc5522_w_active = false;
    term->osc5522_w_pending = false;
    term->osc5522_w_held_commit = false;
    term->osc5522_w_id[0] = '\0';
}

static void osc5522_write_reply(Terminal *term, const char *status)
{
    char reply[112];
    if (term->osc5522_w_id[0])
        snprintf(reply, sizeof(reply), "type=write:status=%s:id=%s",
                 status, term->osc5522_w_id);
    else
        snprintf(reply, sizeof(reply), "type=write:status=%s", status);
    osc5522_send(term, reply, NULL, 0);
}

/* Answer an error and drop the transaction. The Event Log line is optional: the
 * gate's own refusals have already written theirs. */
static void osc5522_write_fail(Terminal *term, const char *status,
                               const char *logline)
{
    if (logline)
        logevent(term->logctx, logline);
    osc5522_write_reply(term, status);
    osc5522_write_reset(term);
}

/* Account for DATA bytes we are about to keep; false (and EFBIG sent) if that
 * would pass the limit. Only the data counts, as in kitty - the names are bounded
 * by the type and alias caps (64 each, 255 bytes at most), so they cannot be
 * used to allocate outside the budget either. */
static bool osc5522_write_budget(Terminal *term, size_t n)
{
    if (term->osc5522_w_bytes + n > osc5522_write_limit(term)) {
        char *msg = dupprintf(KT_CLIP_LOG_WRITE_5522_TOO_BIG,
                              (int)(osc5522_write_limit(term) / (1024 * 1024)));
        osc5522_write_fail(term, "EFBIG", msg);
        sfree(msg);
        return false;
    }
    term->osc5522_w_bytes += n;
    return true;
}

static void osc5522_write_begin(Terminal *term, const char *meta)
{
    char loc[32];
    const char *err = NULL;
    int r;

    osc5522_write_reset(term);
    term->osc5522_w_active = true;
    osc5522_id(meta, term->osc5522_w_id, sizeof(term->osc5522_w_id));

    if (term->osc_str_overflow) {
        osc5522_write_fail(term, "EINVAL", KT_CLIP_LOG_WRITE_5522_INVALID);
        return;
    }
    r = osc5522_meta(meta, "loc", loc, sizeof(loc));
    if (r < 0) {
        osc5522_write_fail(term, "EINVAL", KT_CLIP_LOG_WRITE_5522_INVALID);
        return;
    }
    /* The primary selection is an X11 thing Windows has not got: ENOSYS rather
     * than quietly writing the clipboard the client did not ask for. */
    if (r > 0 && !strcmp(loc, "primary")) {
        osc5522_write_fail(term, "ENOSYS", NULL);
        return;
    }
    r = clip_write_gate(term, &err);
    if (r == CLIP_GATE_NO) {
        osc5522_write_fail(term, err, NULL);
        return;
    }
    /* KiTTY: waiting for the confirmation box, the transaction still streams
     * in under its limits; only its reply waits for the answer (commit). */
    if (r == CLIP_GATE_WAIT)
        term->osc5522_w_pending = true;
}

/* Find or add the accumulator for a MIME type. NULL after EFBIG has been sent. */
static int osc5522_write_type(Terminal *term, char *mime)
{
    int i;
    for (i = 0; i < term->osc5522_w_ntypes; i++)
        if (!strcmp(term->osc5522_w_type[i].mime, mime)) {
            sfree(mime);
            return i;
        }
    if (term->osc5522_w_ntypes >= OSC5522_WRITE_MAX_TYPES) {
        osc5522_write_fail(term, "EFBIG", KT_CLIP_LOG_WRITE_5522_TOO_BIG_TYPES);
        sfree(mime);
        return -1;
    }
    i = term->osc5522_w_ntypes++;
    term->osc5522_w_type[i].mime = mime;
    term->osc5522_w_type[i].data = strbuf_new_nm();
    term->osc5522_w_type[i].carry_len = 0;
    return i;
}

static void osc5522_write_commit(Terminal *term);

static void osc5522_write_data(Terminal *term, const char *meta,
                               const char *payload, size_t payload_len)
{
    char mimeb64[512];
    char *mime;
    int r, ti;
    size_t i;

    if (!term->osc5522_w_active || term->osc5522_w_held_commit)
        return;                        /* no transaction, a failed one, or an
                                        * ended one waiting for the box */
    if (term->osc_str_overflow) {
        osc5522_write_fail(term, "EINVAL", KT_CLIP_LOG_WRITE_5522_INVALID);
        return;
    }
    r = osc5522_meta(meta, "mime", mimeb64, sizeof(mimeb64));
    if (r < 0) {
        osc5522_write_fail(term, "EINVAL", KT_CLIP_LOG_WRITE_5522_INVALID);
        return;
    }
    if (r == 0 || !*mimeb64) {
        if (payload_len) {
            /* data with no type is a packet we cannot file anywhere */
            osc5522_write_fail(term, "EINVAL", KT_CLIP_LOG_WRITE_5522_INVALID);
            return;
        }
        osc5522_write_commit(term);    /* the empty wdata: end of transmission */
        return;
    }
    mime = osc5522_decode_name(mimeb64);
    if (!mime) {
        osc5522_write_fail(term, "EINVAL", KT_CLIP_LOG_WRITE_5522_INVALID);
        return;
    }
    ti = osc5522_write_type(term, mime);
    if (ti < 0)
        return;

    /*
     * Per character as it arrives: the alphabet, then a quad at a time. A padded
     * quad followed by more data for the same type is ACCEPTED, as kitty accepts
     * it (its EFBIG test sends exactly that): the read direction pads every
     * chunk, so clients built against it pad theirs too, and "the concatenation
     * must be correctly padded" is read as "must end padded", which the commit
     * checks. Padding in the wrong place within a quad is still EINVAL.
     */
    for (i = 0; i < payload_len; i++) {
        char c = payload[i];
        if (c != '=' && osc5522_b64_val(c) < 0) {
            osc5522_write_fail(term, "EINVAL", KT_CLIP_LOG_WRITE_5522_INVALID);
            return;
        }
        term->osc5522_w_type[ti].carry[term->osc5522_w_type[ti].carry_len++] = c;
        if (term->osc5522_w_type[ti].carry_len == 4) {
            unsigned char out[3];
            bool padded;
            int n = osc5522_b64_quad(term->osc5522_w_type[ti].carry, out, &padded);
            term->osc5522_w_type[ti].carry_len = 0;
            if (!n) {
                osc5522_write_fail(term, "EINVAL", KT_CLIP_LOG_WRITE_5522_INVALID);
                return;
            }
            if (!osc5522_write_budget(term, n))
                return;                /* EFBIG sent, transaction gone */
            put_data(term->osc5522_w_type[ti].data, out, n);
        }
    }
}

static void osc5522_write_alias(Terminal *term, const char *meta,
                                const char *payload, size_t payload_len)
{
    char mimeb64[512];
    char *target;
    strbuf *list;
    char *p, *end;
    int r;

    if (!term->osc5522_w_active || term->osc5522_w_held_commit)
        return;
    if (term->osc_str_overflow) {
        osc5522_write_fail(term, "EINVAL", KT_CLIP_LOG_WRITE_5522_INVALID);
        return;
    }
    r = osc5522_meta(meta, "mime", mimeb64, sizeof(mimeb64));
    if (r <= 0 || !*mimeb64 || !payload_len ||
        !osc5522_b64_valid(payload, payload_len)) {
        osc5522_write_fail(term, "EINVAL", KT_CLIP_LOG_WRITE_5522_INVALID);
        return;
    }
    target = osc5522_decode_name(mimeb64);
    if (!target) {
        osc5522_write_fail(term, "EINVAL", KT_CLIP_LOG_WRITE_5522_INVALID);
        return;
    }
    list = osc5522_b64_decode(payload, payload_len);
    if (strlen(list->s) != list->len ||
        !osc5522_utf8_ok(list->s, list->len)) {
        /* an embedded NUL, a control character, or not UTF-8 */
        strbuf_free(list);
        sfree(target);
        osc5522_write_fail(term, "EINVAL", KT_CLIP_LOG_WRITE_5522_INVALID);
        return;
    }
    p = list->s;
    while (*p) {
        end = strchr(p, ' ');
        if (end)
            *end = '\0';
        if (*p) {
            int i;
            if (!osc5522_name_ok(p, strlen(p))) {
                osc5522_write_fail(term, "EINVAL", KT_CLIP_LOG_WRITE_5522_INVALID);
                goto out;
            }
            if (term->osc5522_w_naliases >= OSC5522_WRITE_MAX_ALIASES) {
                osc5522_write_fail(term, "EFBIG", KT_CLIP_LOG_WRITE_5522_TOO_BIG_TYPES);
                goto out;
            }
            i = term->osc5522_w_naliases++;
            term->osc5522_w_alias[i] = dupstr(p);
            term->osc5522_w_alias_target[i] = dupstr(target);
        }
        if (!end)
            break;
        p = end + 1;
    }
  out:
    strbuf_free(list);
    sfree(target);
}

static void osc5522_write_commit(Terminal *term)
{
    KittyClipFormat *fmts;
    int i, j, n = 0;
    bool ok;

    /* Padding is judged on the CONCATENATION of a type's chunks, so it is only
     * here, with all of them in, that "not padded" can be told from "not yet". */
    for (i = 0; i < term->osc5522_w_ntypes; i++)
        if (term->osc5522_w_type[i].carry_len) {
            osc5522_write_fail(term, "EINVAL", KT_CLIP_LOG_WRITE_5522_INVALID);
            return;
        }

    /* KiTTY: the write confirmation box is still open - the whole write is in
     * and waits, its reply with it (term_osc52_write_answer finishes it). */
    if (term->osc5522_w_pending) {
        term->osc5522_w_held_commit = true;
        return;
    }

    /* A write that carried nothing sets nothing - the same rule OSC 52 has for
     * an empty payload: a multiplexer's empty selection must not wipe what the
     * user copied. It still completed, so DONE. */
    if (term->osc5522_w_ntypes == 0) {
        osc5522_write_reply(term, "DONE");
        osc5522_write_reset(term);
        return;
    }

    fmts = snewn(term->osc5522_w_ntypes + term->osc5522_w_naliases, KittyClipFormat);
    for (i = 0; i < term->osc5522_w_ntypes; i++) {
        fmts[n].mime = term->osc5522_w_type[i].mime;
        fmts[n].data = term->osc5522_w_type[i].data->s;
        fmts[n].len = term->osc5522_w_type[i].data->len;
        n++;
    }
    for (i = 0; i < term->osc5522_w_naliases; i++) {
        /* an alias is the target's bytes under another name; one whose target
         * never sent data, or that merely repeats a type's own name, adds nothing */
        int t = -1;
        for (j = 0; j < term->osc5522_w_ntypes; j++) {
            if (!strcmp(term->osc5522_w_type[j].mime, term->osc5522_w_alias_target[i]))
                t = j;
            if (!strcmp(term->osc5522_w_type[j].mime, term->osc5522_w_alias[i]))
                t = -2;
        }
        if (t < 0)
            continue;
        fmts[n].mime = term->osc5522_w_alias[i];
        fmts[n].data = term->osc5522_w_type[t].data->s;
        fmts[n].len = term->osc5522_w_type[t].data->len;
        n++;
    }

    ok = kitty_osc52_set_clipboard_formats(fmts, n);
    if (ok) {
        strbuf *names = strbuf_new();
        char *msg;
        for (i = 0; i < n; i++) {
            if (i)
                put_dataz(names, ", ");
            put_dataz(names, fmts[i].mime);
        }
        msg = dupprintf(KT_CLIP_LOG_WRITE_5522_SET, names->s,
                        (unsigned long)term->osc5522_w_bytes);
        logevent(term->logctx, msg);
        sfree(msg);
        strbuf_free(names);
        clip_note_activity(term, CLIP_ACT_WRITE);
        osc5522_write_reply(term, "DONE");
        osc5522_write_reset(term);
    } else {
        osc5522_write_fail(term, "EIO", KT_CLIP_LOG_WRITE_5522_FAILED);
    }
    sfree(fmts);
}

/*
 * ---------------------------------------------------------------------------
 * OSC 5522 paste events (private mode 5522; kitty 0.44.1, the ancillary spec
 * "Private Mode for Automatic Paste Notifications").
 *
 * An application sets CSI ? 5522 h. From then on a local Paste sends it, instead
 * of the text, the reply a `.` type-list read would get - OK carrying a one-time
 * token as pw=, DATA with the MIME list, DONE - and the application reads the
 * types it wants with that token under the name "Paste event", without a dialog.
 * That is how an image pastes into a program on the far end.
 *
 * Bounds: the token is single use and lives
 * OSC5522PasteTokenSeconds; the mode is shown in the title marker like the other
 * clipboard permissions; and tokens are issued for OSC5522PasteMinutes after the
 * mode was set (default 30, 0 = always). After that the mode STAYS SET and the
 * event still goes out, but without a token, so the application's read meets
 * the ordinary dialog: a privilege created by a keypress gets a clock, the
 * feature does not die of it - an application cannot know about a terminal
 * switching its mode off and would simply stop working. Reads set to Deny
 * suspend the whole thing: no event, Paste pastes text, as without the mode.
 */
/* The token lifetime is the setting OSC5522PasteTokenSeconds (default 10). */
static unsigned long osc5522_paste_token_secs(Terminal *term)
{
    int s = conf_get_int(term->conf, CONF_osc5522_paste_token_secs);
    return s < 1 ? 1 : (unsigned long)s;
}

static void osc5522_paste_forget_token(Terminal *term)
{
    if (term->osc5522_paste_pw) {
        smemclr(term->osc5522_paste_pw, strlen(term->osc5522_paste_pw));
        sfree(term->osc5522_paste_pw);
        term->osc5522_paste_pw = NULL;
    }
    term->osc5522_paste_pw_until = 0;
}

void osc5522_paste_disarm(Terminal *term, bool timed)
{
    bool was = term->osc5522_paste_events;
    (void)timed;                  /* the clock no longer clears the mode */
    term->osc5522_paste_events = false;
    term->osc5522_paste_tokens_until = 0;
    term->osc5522_paste_expiry_pending = false;
    term->osc5522_paste_expiry_logged = false;
    osc5522_paste_forget_token(term);
    if (was)
        kitty_osc52_state_changed(term);
}

void osc5522_paste_set(Terminal *term, bool state)
{
    int minutes = conf_get_int(term->conf, CONF_osc5522_paste_minutes);
    if (!state) {
        if (term->osc5522_paste_events)
            logevent(term->logctx, KT_CLIP_LOG_PASTE_EVENTS_OFF);
        osc5522_paste_disarm(term, false);
        return;
    }
    /* Setting it again restarts the clock: the application asked again. */
    term->osc5522_paste_events = true;
    term->osc5522_paste_expiry_logged = false;
    osc5522_paste_forget_token(term);
    if (minutes > 0) {
        term->osc5522_paste_tokens_until = (unsigned long)time(NULL) + (unsigned long)minutes * 60;
        term->osc5522_paste_expiry_tick =
            schedule_timer(minutes * 60 * TICKSPERSEC, kitty_term_timer_fn(),
                           term);
        term->osc5522_paste_expiry_pending = true;
        {
            char *msg = dupprintf(KT_CLIP_LOG_PASTE_EVENTS_ON, minutes);
            logevent(term->logctx, msg);
            sfree(msg);
        }
    } else {
        term->osc5522_paste_tokens_until = 0;
        term->osc5522_paste_expiry_pending = false;
        logevent(term->logctx, KT_CLIP_LOG_PASTE_EVENTS_ON_NOLIMIT);
    }
    kitty_osc52_state_changed(term);
}

/* Is the mode set? Exactly what the application set; the clock does not touch it. */
bool term_osc5522_paste_events(Terminal *term)
{
    return term && term->osc5522_paste_events;
}

/* Are pastes still carrying a token? The wall clock is judged here, so a machine
 * that slept through the timer still comes to the same answer on the next paste
 * or title refresh. The expiry is logged once, whichever path notices first. */
bool term_osc5522_paste_tokens(Terminal *term)
{
    if (!term || !term->osc5522_paste_events)
        return false;
    if (term->osc5522_paste_tokens_until == 0)
        return true;
    if ((unsigned long)time(NULL) < term->osc5522_paste_tokens_until)
        return true;
    if (!term->osc5522_paste_expiry_logged) {
        char *msg = dupprintf(KT_CLIP_LOG_PASTE_TOKENS_EXPIRED,
                              conf_get_int(term->conf, CONF_osc5522_paste_minutes));
        logevent(term->logctx, msg);
        sfree(msg);
        term->osc5522_paste_expiry_logged = true;
        osc5522_paste_forget_token(term);
        kitty_osc52_state_changed(term);
    }
    return false;
}

/* The event itself. False when it is not sent, so the caller pastes as usual. */
bool osc5522_paste_event(Terminal *term)
{
    unsigned char raw[16];
    char *token, *m, *p, *msg;
    char reply[160];
    char pwpart[48];
    wchar_t *clip;
    int clip_len = 0;
    bool have_text, have_image;
    char list[128];

    if (conf_get_int(term->conf, CONF_osc52_clipboard_read) != OSC52_READ_ASK) {
        logevent(term->logctx, KT_CLIP_LOG_PASTE_EVENT_DENIED);
        return false;
    }

    clip = kitty_osc52_get_clipboard(&clip_len);
    have_text = (clip && clip_len > 0);
    if (clip) {
        smemclr(clip, clip_len * sizeof(wchar_t));
        sfree(clip);
    }
    have_image = kitty_osc52_clipboard_has_image();

    /*
     * The token only while the clock allows. Afterwards the event still goes
     * out - the application must keep working, and it cannot know about a
     * clock the protocol does not have - but without pw=, so its read meets the
     * ordinary permission dialog. pw is optional in the spec ("SHOULD").
     */
    osc5522_paste_forget_token(term);
    if (term_osc5522_paste_tokens(term)) {
        kitty_osc52_random(raw, sizeof(raw));
        token = osc5522_b64((const char *)raw, sizeof(raw));
        smemclr(raw, sizeof(raw));
        term->osc5522_paste_pw = token;           /* kept in base64, as compared */
        term->osc5522_paste_pw_until = (unsigned long)time(NULL) + osc5522_paste_token_secs(term);
        snprintf(pwpart, sizeof(pwpart), ":pw=%s", token);
    } else {
        pwpart[0] = '\0';
    }

    snprintf(list, sizeof(list), "%s%s%s\n",
             have_text ? "text/plain text/plain;charset=utf-8" : "",
             have_text && have_image ? " " : "",
             have_image ? "image/png" : "");

    snprintf(reply, sizeof(reply), "type=read:status=OK%s", pwpart);
    osc5522_send(term, reply, NULL, 0);
    if (have_text || have_image) {
        m = osc5522_b64(".", 1);
        p = osc5522_b64(list, strlen(list));
        snprintf(reply, sizeof(reply), "type=read:status=DATA:mime=%s%s", m, pwpart);
        osc5522_send(term, reply, p, strlen(p));
        sfree(m);
        sfree(p);
    }
    snprintf(reply, sizeof(reply), "type=read:status=DONE%s", pwpart);
    osc5522_send(term, reply, NULL, 0);

    msg = dupprintf(KT_CLIP_LOG_PASTE_EVENT_SENT,
                    (have_text || have_image) ? list : KT_CLIP_LOG_PASTE_EVENT_EMPTY);
    {
        char *nl = strchr(msg, '\n');
        if (nl)
            *nl = '\0';
    }
    logevent(term->logctx, msg);
    sfree(msg);
    return true;
}

void osc5522_process(Terminal *term)
{
    char *meta, *payload = NULL;
    size_t payload_len = 0;
    char type[32];
    char idpart[40] = "";
    char *sep;

    /* osc_string holds "<metadata>" or "<metadata>;<payload>". */
    meta = term->osc_string;
    sep = strchr(meta, ';');
    if (sep) {
        *sep = '\0';                   /* osc_string is ours until the next OSC */
        payload = sep + 1;
        payload_len = strlen(payload);
    }

    /*
     * A request we cannot identify gets NO reply.
     *
     * The temptation is to answer EINVAL, and it is wrong twice over: EINVAL is
     * specified only for the write direction, and every reply has to carry a
     * `type=`, so answering a sequence whose type we could not read would mean
     * inventing one. Silence is what a malformed OSC 52 gets too.
     */
    if (osc5522_meta(meta, "type", type, sizeof(type)) != 1)
        return;

    /*
     * Echo the request id on EVERY reply this function sends, not only on the
     * ones osc5522_read() builds.
     *
     * The id is how a client matches an answer to a question, and it may have
     * several in flight. The ENOSYS and EBUSY replies below were sending
     * "type=write:status=ENOSYS" with no id at all, so a pipelining client could
     * see the refusal but not learn WHICH request had been refused. Caught by a
     * test asking for "id=4:status=ENOSYS" and getting the status without it.
     */
    {
        char id[32];
        osc5522_id(meta, id, sizeof(id));
        if (*id)
            snprintf(idpart, sizeof(idpart), ":id=%s", id);
    }

    if (!strcmp(type, "read")) {
        /*
         * A read that did not fit is refused whole - same rule as OSC 52, since
         * half a transaction is worse than none. EBUSY rather than silence,
         * because the host is waiting for a reply and it is the one code in the
         * read set that means "not now": the read direction has only ENOSYS,
         * EPERM and EBUSY, and the other two would both say something untrue.
         */
        if (term->osc_str_overflow) {
            clip_payload_dropped(
                term, KT_CLIP_WHAT_OSC5522,
                conf_get_int(term->conf, CONF_osc52_clipboard_read) ==
                OSC52_READ_ASK);
            char ebusy[64];
            snprintf(ebusy, sizeof(ebusy), "type=read%s:status=EBUSY", idpart);
            osc5522_send(term, ebusy, NULL, 0);
            return;
        }
        /* KiTTY: behind a write that waits for its confirmation box, the read
         * waits too and runs after the answer - it must see the clipboard the
         * write left, as it did while that box was modal. One waits; a further
         * one is "not now" (EBUSY). */
        if (term->osc52_w_asking) {
            if (term->osc52_w_held_read.kind) {
                char ebusy[64];
                snprintf(ebusy, sizeof(ebusy), "type=read%s:status=EBUSY", idpart);
                osc5522_send(term, ebusy, NULL, 0);
                return;
            }
            clip_held_read_set(&term->osc52_w_held_read, 2, meta, payload,
                               payload_len);
            return;
        }
        osc5522_read(term, meta, payload, payload_len);
        return;
    }
    if (!strcmp(type, "write")) {
        osc5522_write_begin(term, meta);
        return;
    }
    if (!strcmp(type, "wdata")) {
        osc5522_write_data(term, meta, payload, payload_len);
        return;
    }
    if (!strcmp(type, "walias")) {
        osc5522_write_alias(term, meta, payload, payload_len);
        return;
    }
    /* A type we do not implement at all. ENOSYS, reported against the type the
     * host actually named, so nothing is invented. */
    {
        char reply[112];
        const char *q;
        for (q = type; *q; q++)
            if (!((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z')))
                return;                /* not a plausible type name: say nothing */
        snprintf(reply, sizeof(reply), "type=%s%s:status=ENOSYS", type, idpart);
        osc5522_send(term, reply, NULL, 0);
    }
}
#ifdef MOD_PERSO
/*
 * KiTTY: OSC 52 - the remote host asks to put text on the LOCAL clipboard.
 *
 * The sequence is  ESC ] 52 ; Pc ; Pd ST , so by the time we get here
 * osc_string holds "Pc;Pd": Pc names the selection(s), Pd is base64 text.
 *
 * Upstream PuTTY omits OSC 52 on purpose, and the reason is worth stating: a
 * remote host writing your clipboard is a genuine risk, because what you paste
 * next may not be what you think you copied. So this is policy-gated
 * (CONF_osc52_clipboard: disabled / enabled / ask-once-per-session) and the
 * READ direction is not implemented at all.
 *
 * Classic KiTTY's version differs in three ways we did not carry over: it drove
 * the Win32 clipboard API directly from this cross-platform file, it honoured an
 * OSC52ALLOWED environment variable that silently suppressed the prompt (an
 * undiscoverable override of a security decision), and it asked on every single
 * payload rather than latching per session.
 */
/*
 * The remote clipboard WRITE gate, shared by OSC 52 and OSC 5522: may this host
 * set the clipboard right now? Policy (Deny / Ask-once-and-latch / Allow), the
 * focus rule, then the rate cap - in that order, so a refused or unfocused write
 * costs nobody any budget. `err`, if wanted, receives the 5522 status a refusal
 * maps to: EPERM for policy and focus, EBUSY for the cap. CLIP_GATE_WAIT: Ask,
 * the confirmation box is open - the caller holds the write for its answer.
 */
static int clip_write_gate(Terminal *term, const char **err)
{
    if (err)
        *err = "EPERM";
    if (term->osc52_allowed == OSC52_CLIPBOARD_DENY)
        return CLIP_GATE_NO;

    /*
     * KiTTY: the focus rule applies to writes as well as reads.
     * "KiTTY does not touch your clipboard unless you are looking at
     * that window" is worth more for being one sentence with no exceptions in it,
     * and it also means no host can change what you are about to paste at a
     * moment you were not watching.
     *
     * It costs something and the setting exists because of that: a background job
     * that copies its own output stops working while you are in another window.
     * Nothing is queued - the sequence is dropped, because replaying a stale
     * clipboard write when focus comes back is worse than not doing it.
     */
    if (conf_get_bool(term->conf, CONF_clipboard_require_focus) && !term->has_focus) {
        logevent(term->logctx, KT_CLIP_LOG_WRITE_NO_FOCUS);
        return CLIP_GATE_NO;
    }

    if (term->osc52_allowed == OSC52_CLIPBOARD_ASK) {
        /*
         * KiTTY: the confirmation box is MODELESS. It used to be a modal
         * MessageBox opened from here, inside the parser: output stopped until
         * it was answered, keys and repaints could reach the terminal in the
         * middle of a parse, and its default button was OK - an Enter typed as
         * it appeared allowed every write for the rest of the session. Now the
         * write waits and the answer arrives later (term_osc52_write_answer),
         * from a box whose default is No. One box: a write that arrives while it
         * is open waits for the same answer.
         */
        if (!term->osc52_w_asking) {
            term->osc52_w_asking = true;
            kitty_osc52_write_confirm(term);
        }
        return CLIP_GATE_WAIT;
    }

    /* Rate cap. Checked after the permission gates and immediately before the
     * write, so a refused or unfocused write costs nobody any budget. */
    if (!clip_write_allowed(term)) {
        if (err)
            *err = "EBUSY";
        return CLIP_GATE_NO;
    }
    return CLIP_GATE_YES;
}

/* Put a decoded OSC 52 payload on the clipboard. The caller frees `decoded`. */
static void osc52_write_text(Terminal *term, strbuf *decoded)
{
    wchar_t *wide;
    size_t wlen;

    /* strbuf keeps its contents NUL-terminated, and decode_utf8_to_wide_string
     * substitutes U+FFFD for anything malformed rather than failing. */
    wide = decode_utf8_to_wide_string(decoded->s);
    wlen = wcslen(wide);
    /* SELECTION_NUL_TERMINATED: the same length convention clipme() uses, i.e.
     * on Windows the terminating NUL is part of what is handed over. */
    win_clip_write(term->win, CLIP_SYSTEM, wide, NULL, NULL,
                   (int)(wlen + SELECTION_NUL_TERMINATED), false);
    clip_note_activity(term, CLIP_ACT_WRITE);
    sfree(wide);
}

void osc52_set_clipboard(Terminal *term)
{
    const char *sep, *pd, *p;
    size_t pdlen;
    strbuf *decoded;
    int gate;

    sep = strchr(term->osc_string, ';');
    if (!sep)
        return;                        /* no Pd field at all */
    pd = sep + 1;

    /*
     * Pc: xterm's selection characters. Windows has exactly one clipboard, so
     * every one of these means the same thing to us. An unrecognised character
     * means a sequence we do not understand, which is refused rather than
     * guessed at. Empty Pc is legal and means the default selection.
     */
    for (p = term->osc_string; p < sep; p++)
        if (!strchr("cpqs01234567", *p))
            return;

    /*
     * Pd == "?" is the READ direction - it asks us to send the local clipboard
     * back to the host. It is the dangerous half of OSC 52 and the reason
     * upstream leaves the sequence out altogether, so it has its own permission
     * machinery above and its own setting, which defaults to Deny. It is not a
     * variation on the write below and does not share its policy.
     */
    if (!strcmp(pd, "?")) {
        /* KiTTY: behind a write that waits for its confirmation box, after it
         * (term_osc52_write_answer); one read waits, a further one is dropped,
         * as every OSC 52 refusal is - in silence. */
        if (term->osc52_w_asking) {
            if (!term->osc52_w_held_read.kind)
                clip_held_read_set(&term->osc52_w_held_read, 1, NULL, NULL, 0);
            return;
        }
        osc52_read_clipboard(term);
        return;
    }

    /*
     * A payload that did not fit is refused whole, never pasted in part. Half a
     * clipboard is worse than none: it looks like it worked, and pasting half a
     * command line is how that becomes somebody's bad day.
     *
     * It used to be refused in total silence, which is worse again - the feature
     * simply appeared not to work, with nothing anywhere to say why.
     */
    if (term->osc_str_overflow) {
        clip_payload_dropped(term, KT_CLIP_WHAT_OSC52,
                             term->osc52_allowed != OSC52_CLIPBOARD_DENY);
        return;
    }

    /*
     * Same rule for a malformed payload. base64_decode_atom silently yields
     * nothing for an invalid 4-character group, so decoding without checking
     * first would hand over a clipboard with holes punched in it.
     */
    pdlen = strlen(pd);
    if (pdlen % 4 == 1)
        return;
    for (p = pd; *p; p++)
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
              (*p >= '0' && *p <= '9') || *p == '+' || *p == '/' || *p == '='))
            return;

    /*
     * An EMPTY payload does nothing at all - it does not clear the clipboard.
     *
     * "ESC ] 52 ; c ; ST" is what a terminal multiplexer sends when a selection
     * gesture selected nothing, which happens constantly: a double-click below
     * the prompt, a drag that did not move. Treating it as a write would throw
     * away what the user had copied, in response to a click they made by
     * accident, and with the setting on Ask it would raise a permission dialog
     * to do it. Windows itself works the other way round - copying an empty
     * selection leaves the clipboard alone - and so does this.
     *
     * Before the permission gate deliberately: there is nothing to ask about,
     * and nothing to spend a rate-limit budget on. Reported against classic
     * KiTTY as cyd01/KiTTY#495, where the same case crashed outright; ours never
     * could, because the decode below cannot return NULL, but clearing the
     * clipboard was wrong regardless.
     */
    if (pdlen == 0) {
        logevent(term->logctx, KT_CLIP_LOG_WRITE_EMPTY);
        return;
    }

    gate = clip_write_gate(term, NULL);
    if (gate == CLIP_GATE_NO)
        return;

    decoded = base64_decode_sb(make_ptrlen(pd, pdlen));
    if (gate == CLIP_GATE_WAIT) {
        /* KiTTY: the confirmation box is open. The latest payload waits for
         * its answer: an earlier one still waiting would have been overwritten
         * on the clipboard straight away, so it is replaced here. */
        if (term->osc52_w_held) {
            smemclr(term->osc52_w_held->u, term->osc52_w_held->len);
            strbuf_free(term->osc52_w_held);
        }
        term->osc52_w_held = decoded;
        return;
    }
    osc52_write_text(term, decoded);
    strbuf_free(decoded);
}

void osc52_write_held_drop(Terminal *term)
{
    if (term->osc52_w_held) {
        /* wiped, not merely freed: it may well be a password */
        smemclr(term->osc52_w_held->u, term->osc52_w_held->len);
        strbuf_free(term->osc52_w_held);
        term->osc52_w_held = NULL;
    }
    clip_held_read_drop(&term->osc52_w_held_read);
    term->osc52_w_asking = false;
}

/* What the read box showed: wiped, not merely freed. */
static void osc52_read_held_wipe(Terminal *term)
{
    if (term->osc52_read_held_text) {
        smemclr(term->osc52_read_held_text,
                term->osc52_read_held_text_len * sizeof(wchar_t));
        sfree(term->osc52_read_held_text);
    }
    if (term->osc52_read_held_png) {
        smemclr(term->osc52_read_held_png, term->osc52_read_held_png_len);
        sfree(term->osc52_read_held_png);
    }
    term->osc52_read_held_text = NULL;
    term->osc52_read_held_text_len = 0;
    term->osc52_read_held_png = NULL;
    term->osc52_read_held_png_len = 0;
}

void osc52_read_wait_drop(Terminal *term)
{
    osc52_read_held_wipe(term);
    clip_held_read_drop(&term->osc52_read_wait);
    term->osc52_read_answered = false;
    term->osc52_read_asking = false;
}

/* Run a kept read request now, through the ordinary path. */
static void clip_held_read_run(Terminal *term, ClipHeldRead *h)
{
    ClipHeldRead r = *h;
    memset(h, 0, sizeof(*h));          /* taken: the run may keep a new one */
    if (r.kind == 1)
        osc52_read_clipboard(term);
    else if (r.kind == 2)
        osc5522_read(term, r.meta, r.payload, r.len);
    sfree(r.meta);
    sfree(r.payload);
}

/*
 * KiTTY: the answer of the read box, from the platform side after the box has
 * closed (kitty_osc52.c), never from inside the parser. The request that
 * waited runs again through the ordinary path - so the setting and the focus
 * rule are applied again - and the gate takes this answer instead of opening
 * another box.
 */
void term_osc52_read_answer(Terminal *term, bool allowed, int grant,
                            bool always_deny)
{
    if (!term || !term->osc52_read_asking)
        return;
    term->osc52_read_asking = false;
    term->osc52_read_answered = true;
    term->osc52_read_ans_allowed = allowed;
    term->osc52_read_ans_grant = grant;
    term->osc52_read_ans_always_deny = always_deny;
    clip_held_read_run(term, &term->osc52_read_wait);
    /* Refused before the gate reached the answer (the setting became Deny
     * meanwhile, say): nothing is sent, and what the box showed is wiped. */
    term->osc52_read_answered = false;
    osc52_read_held_wipe(term);
}

/*
 * KiTTY: the answer of the write confirmation box. It comes from the platform
 * side after the box has closed (kitty_osc52.c), never from inside the parser,
 * and finishes what waited for it, in the order it arrived: the writes, then
 * the read behind them.
 */
void term_osc52_write_answer(Terminal *term, bool yes)
{
    bool ok;

    if (!term || !term->osc52_w_asking)
        return;
    term->osc52_w_asking = false;

    /*
     * The answer latches for the rest of the session either way: a box per
     * payload would let any host raise one as often as it liked. Only while
     * the setting is still Ask, though - Change Settings runs beside the box,
     * and a setting chosen there meanwhile is the newer decision.
     */
    if (term->osc52_allowed == OSC52_CLIPBOARD_ASK) {
        term->osc52_allowed = yes ? OSC52_CLIPBOARD_ALLOW : OSC52_CLIPBOARD_DENY;
        /*
         * Yes is a GRANT with a lifetime - exactly like allowing a read for the
         * session - so the window has to start showing it. Without this the
         * "(clip write)" marker did not appear until the session next set its
         * own title, which for a shell that never sets one is never: the
         * permission was live and invisible, which is the one thing the marker
         * exists to prevent.
         */
        if (yes)
            kitty_osc52_state_changed(term);
    }
    ok = (term->osc52_allowed == OSC52_CLIPBOARD_ALLOW);

    /* The held writes. The focus rule was met when they arrived and is not
     * applied again: the box had the focus. The rate cap still is. */
    if (term->osc52_w_held) {
        strbuf *held = term->osc52_w_held;
        term->osc52_w_held = NULL;
        if (ok && clip_write_allowed(term))
            osc52_write_text(term, held);
        smemclr(held->u, held->len);
        strbuf_free(held);
    }
    if (term->osc5522_w_pending) {
        term->osc5522_w_pending = false;
        if (!ok)
            osc5522_write_fail(term, "EPERM", NULL);
        else if (!clip_write_allowed(term))
            osc5522_write_fail(term, "EBUSY", NULL);
        else if (term->osc5522_w_held_commit) {
            term->osc5522_w_held_commit = false;
            osc5522_write_commit(term);
        }
        /* else still streaming, and now allowed: it ends like any other */
    }

    /* Then the read that came in behind them. */
    clip_held_read_run(term, &term->osc52_w_held_read);
}
#endif /* MOD_PERSO */

#ifdef MOD_PERSO
/*
 * KiTTY: what clipboard permission is live right now, for the title marker and
 * the window colouring. Agreeing to be read once is not agreeing to be read
 * invisibly from then on, so while a permission is in force it is on the window.
 *
 * Only permissions GRANTED IN THE MOMENT count here. A setting of Allow is a
 * configuration, not a grant: writes default to Allow, so marking that would put
 * "clip write" in every title of every session and the marker would stop meaning
 * anything within a day. A write permission latched by answering the Ask dialog
 * does count - the user was asked, and this is the reminder that they said yes.
 *
 * *read and *write are set to whether each is live; the return value is
 * OSC52_PERM_NONE / _ACTIVE / _PAUSED for the pair.
 */
/*
 * KiTTY: which directions the host has touched the clipboard in recently
 * (CLIP_ACT_* bits), or 0 once that has lapsed. The window asks on every title
 * refresh, so this also decides when the marker disappears.
 */
int term_clipboard_activity(Terminal *term)
{
    if (!term || !term->clip_activity_dir)
        return 0;
    if ((unsigned long)time(NULL) >= term->clip_activity_until) {
        term->clip_activity_dir = 0;
        return 0;
    }
    return term->clip_activity_dir;
}

int term_osc52_perm_state(Terminal *term, bool *read, bool *write)
{
    bool r, w;
    if (!term) {
        if (read) *read = false;
        if (write) *write = false;
        return OSC52_PERM_NONE;
    }
    r = (term->osc52_read_decision > 0) && !osc52_read_decision_expired(term);
    /* A standing "Allow" is a configuration, not a grant, so it is not marked -
     * unless the user asked for the title to be a complete statement, because
     * without that "no marker" means "nothing granted in the moment" rather than
     * "no access is possible". */
    w = (term->osc52_allowed == OSC52_CLIPBOARD_ALLOW &&
         (conf_get_int(term->conf, CONF_osc52_clipboard) == OSC52_CLIPBOARD_ASK ||
          conf_get_bool(term->conf, CONF_clipboard_mark_always)));
    if (read) *read = r;
    if (write) *write = w;
    if (!r && !w)
        return OSC52_PERM_NONE;
    if (conf_get_bool(term->conf, CONF_clipboard_require_focus) && !term->has_focus)
        return OSC52_PERM_PAUSED;
    return OSC52_PERM_ACTIVE;
}
#endif

#endif /* MOD_PERSO */
