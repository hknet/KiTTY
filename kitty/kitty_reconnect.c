/*
 * kitty_reconnect.c: what a failed reconnect attempt means, and what to do
 * about it (hknet/KiTTY#66).
 *
 * Until 0.85.1.15 a session that had logged in once reconnected after every
 * fatal error that did not contain "authentication", every 5 s, without end.
 * An attempt that reached sshd and then waited for a signature from a locked
 * agent key was closed by the server each time - one failed login per
 * attempt, until fail2ban banned the user.
 *
 * Now each attempt records how far it got (kitty_connstage: TCP up, login
 * begun, an agent signature outstanding; window.c: a prompt open), a
 * classifier turns that into ONE condition, and the condition's setting
 * (Application > KiTTY++ Settings > Reconnect & Prompts, one droplist each,
 * kitty.ini [KiTTY] reconnect<id>=) decides what follows. Retries back off:
 * the configured wait, times the backoff factor each time (1 = fixed, the
 * default 2 doubles), up to "Max retry timer".
 *
 * One terminal per process, so the attempt state is this file's. The SSH
 * layer reports through a hook that window.c filters to the session's own
 * Seat - a jump host's connection has a Seat of its own and never counts.
 */

#include <ctype.h>

#include "putty.h"
#include "kitty_reconnect.h"
#include "kitty_inikeys.h"
#include "kitty_params.h"
#include "kitty_text.h"

/* ---- the table -------------------------------------------------------- */

static const struct kr_rule {
    const char *key;      /* kitty.ini [KiTTY] key */
    const char *label;    /* the panel's words, also the Event Log's */
    int dflt;
} kr_rules[KR_NCOND] = {
    [KR_DNS]         = { KI_RECONNECT_DNS,         KT_RC_COND_DNS,         KRA_RETRY },
    [KR_TCP]         = { KI_RECONNECT_TCP,         KT_RC_COND_TCP,         KRA_RETRY },
    [KR_PRELOGIN]    = { KI_RECONNECT_PRELOGIN,    KT_RC_COND_PRELOGIN,    KRA_THREE },
    [KR_AGENTLOCKED] = { KI_RECONNECT_AGENTLOCKED, KT_RC_COND_AGENTLOCKED, KRA_WAIT },
    [KR_PROMPT]      = { KI_RECONNECT_PROMPT,      KT_RC_COND_PROMPT,      KRA_STOP },
    [KR_NOKEY]       = { KI_RECONNECT_NOKEY,       KT_RC_COND_NOKEY,       KRA_STOP },
    [KR_LOGINCLOSED] = { KI_RECONNECT_LOGINCLOSED, KT_RC_COND_LOGINCLOSED, KRA_ONCE },
    [KR_REFUSED]     = { KI_RECONNECT_REFUSED,     KT_RC_COND_REFUSED,     KRA_STOP },
    [KR_OTHER]       = { KI_RECONNECT_OTHER,       KT_RC_COND_OTHER,       KRA_ONCE },
};

const char *kitty_reconnect_cond_label(KrCond c)
{ return (c >= 0 && c < KR_NCOND) ? kr_rules[c].label : "?"; }
const char *kitty_reconnect_cond_key(KrCond c)
{ return (c >= 0 && c < KR_NCOND) ? kr_rules[c].key : NULL; }
int kitty_reconnect_cond_default(KrCond c)
{ return (c >= 0 && c < KR_NCOND) ? kr_rules[c].dflt : KRA_STOP; }

const char *kitty_reconnect_action_label(int a)
{
    switch (a) {
      case KRA_RETRY: return KT_RC_ACT_RETRY;
      case KRA_THREE: return KT_RC_ACT_THREE;
      case KRA_ONCE:  return KT_RC_ACT_ONCE;
      case KRA_WAIT:  return KT_RC_ACT_WAIT;
      default:        return KT_RC_ACT_STOP;
    }
}

/* The stored value: retry | 3 | 1 | stop | wait. Missing or not understood:
 * the row's default. "wait" only means something for a locked agent key;
 * anywhere else it reads as the default. */
static int kr_action(KrCond c)
{
    char v[32] = "";
    if (!ReadParameterN(KI_SECTION_KITTY, kr_rules[c].key, v, sizeof(v)) || !v[0])
        return kr_rules[c].dflt;
    if (!stricmp(v, "retry")) return KRA_RETRY;
    if (!strcmp(v, "3"))      return KRA_THREE;
    if (!strcmp(v, "1"))      return KRA_ONCE;
    if (!stricmp(v, "stop"))  return KRA_STOP;
    if (!stricmp(v, "wait") && c == KR_AGENTLOCKED) return KRA_WAIT;
    return kr_rules[c].dflt;
}

/* ---- the attempt ------------------------------------------------------ */

static struct {
    bool tcp_up, userauth, agent_pending, prompt_open, logged_in;
    unsigned char *agent_blob;      /* the key the last signature was for */
    size_t agent_bloblen;
} st;

static int kr_tries[KR_NCOND];      /* retries per condition since the last login */
static int kr_total;                /* all retries since the last login: the backoff */

void kitty_reconnect_attempt_begin(void)
{
    st.tcp_up = st.userauth = st.agent_pending = st.prompt_open = false;
    st.logged_in = false;
    /* agent_blob stays: a wait for unlock still needs it */
}

void kitty_reconnect_stage(KittyConnStage stage, ptrlen data)
{
    switch (stage) {
      case KCS_TCP_UP:         st.tcp_up = true; break;
      case KCS_USERAUTH_START: st.userauth = true; break;
      case KCS_AGENT_SIGN_BEGIN:
        st.agent_pending = true;
        sfree(st.agent_blob);
        st.agent_blob = snewn(data.len ? data.len : 1, unsigned char);
        memcpy(st.agent_blob, data.ptr, data.len);
        st.agent_bloblen = data.len;
        break;
      case KCS_AGENT_SIGN_END: st.agent_pending = false; break;
    }
}

void kitty_reconnect_prompt_open(bool open) { st.prompt_open = open; }

void kitty_reconnect_logged_in(void)
{
    st.logged_in = true;
    memset(kr_tries, 0, sizeof(kr_tries));
    kr_total = 0;
}

/* telnet, raw and the like: no login phase, so a connection counts as in -
 * but it does not reset the backoff, or a server that accepts and closes at
 * once would be dialled every few seconds for ever */
void kitty_reconnect_connected(void) { st.logged_in = true; }

bool kitty_reconnect_attempt_logged_in(void) { return st.logged_in; }

/* the configured wait, times the backoff factor per retry since the last
 * login (1 = a fixed wait, clamped to 1..10), capped */
static int kr_backoff_ms(int base_s, int max_s, int factor)
{
    int s = base_s > 0 ? base_s : 5, i;
    if (factor < 1) factor = 1;
    if (factor > 10) factor = 10;
    if (max_s < s)
        max_s = s;
    for (i = 0; i < kr_total && s < max_s && factor > 1; i++)
        s = (s > max_s / factor) ? max_s : s * factor;
    return s * 1000;
}

int kitty_reconnect_drop_delay(int base_s, int max_s, int factor)
{
    int ms = kr_backoff_ms(base_s, max_s, factor);
    kr_total++;
    return ms;
}

/* ---- the classifier --------------------------------------------------- */

static bool kr_has(const char *msg, const char *needle)
{
    size_t n = strlen(needle);
    if (!msg)
        return false;
    for (; *msg; msg++)
        if (!strnicmp(msg, needle, n))
            return true;
    return false;
}

static KrCond kr_classify(const char *msg, bool have_agent, bool have_keyfile,
                          bool have_gssapi)
{
    /* the user's own decision (a host key declined, a prompt cancelled):
     * never a reason to try again by itself */
    if (kr_has(msg, "User aborted") || kr_has(msg, "User cancelled") ||
        kr_has(msg, "cancelled by user"))
        return KR_PROMPT;
    if (!st.tcp_up) {
        /* never reached the server, so the server logged no login: the
         * message distinguishes a name lookup from the rest, and anything not
         * recognised - a localised Windows text - is still on this side */
        if (kr_has(msg, "Host does not exist") || kr_has(msg, "Host not found"))
            return KR_DNS;
        return KR_TCP;
    }
    if (kr_has(msg, "authentication") || kr_has(msg, "Access denied") ||
        kr_has(msg, "Permission denied")) {
        /* "... available (server sent: publickey,...)": the server's list
         * shows whether GSSAPI was on offer; if it was and the session
         * tries it, a GSSAPI login was refused, not missing */
        if (!have_agent && !have_keyfile &&
            kr_has(msg, "No supported authentication methods") &&
            !(have_gssapi && kr_has(msg, "gssapi")))
            return KR_NOKEY;
        return KR_REFUSED;
    }
    if (!st.userauth)
        return KR_PRELOGIN;
    if (st.agent_pending)
        return KR_AGENTLOCKED;
    if (st.prompt_open)
        return KR_PROMPT;
    if (!msg || !*msg)
        return KR_OTHER;
    return KR_LOGINCLOSED;
}

const char *kitty_reconnect_hint(const char *msg)
{
    /* sshd's MaxAuthTries: every key the agent offers is one try, so a full
     * agent is refused before the right key comes up */
    if (kr_has(msg, "Too many authentication failures"))
        return KT_RC_HINT_MAXAUTHTRIES;
    return NULL;
}

/* ---- the decision ----------------------------------------------------- */

KrDecision kitty_reconnect_decide(const char *msg, bool have_agent,
                                  bool have_keyfile, bool have_gssapi,
                                  bool login_never,
                                  int base_s, int max_s, int factor)
{
    KrDecision d;
    int a;

    memset(&d, 0, sizeof(d));
    d.cond = kr_classify(msg, have_agent, have_keyfile, have_gssapi);
    a = kr_action(d.cond);
    /* the session's "Retries after a failed login: never": every condition
     * where the server saw a login (or the start of one) stops */
    if (login_never && (d.cond == KR_PRELOGIN || d.cond == KR_LOGINCLOSED ||
                        d.cond == KR_REFUSED || d.cond == KR_OTHER))
        a = KRA_STOP;
    /* a locked key: is it still locked? If the agent now has it usable
     * (unlocked meanwhile) this was a login closed with nothing outstanding;
     * if the agent or the key is gone, there is nothing to log in with */
    if (d.cond == KR_AGENTLOCKED) {
        int locked = kitty_reconnect_key_locked();
        if (locked == 0) {
            d.cond = KR_LOGINCLOSED;
            a = login_never ? KRA_STOP : kr_action(KR_LOGINCLOSED);
        } else if (locked < 0) {
            d.cond = KR_NOKEY;
            a = kr_action(KR_NOKEY);
        }
    }
    d.action = a;

    if (a == KRA_WAIT) {
        d.wait_unlock = true;
        d.delay_ms = 5000;          /* the local poll, no server contact */
    } else if (a == KRA_RETRY ||
               (a > 0 && kr_tries[d.cond] < a)) {
        d.retry = true;
        d.delay_ms = kr_backoff_ms(base_s, max_s, factor);
        kr_tries[d.cond]++;
        kr_total++;
    }
    d.attempt = kr_tries[d.cond];
    return d;
}

/* ---- the agent check -------------------------------------------------- */

/* list-extended (PuTTY's agent extension, which kageant answers): per key a
 * flags word; HAS_NO_CLEARTEXT_KEY (2) = it is encrypted until someone types
 * its passphrase. A local query, no network. */
/* One request, answered synchronously: no callback (what aqsync.c's wrapper
 * does - that file is in the agent library, which the terminals do not
 * link). Takes and frees req; the reply is the caller's to sfree. */
static void *kr_agent_ask(strbuf *req, int *replylen)
{
    void *reply = NULL;
    agent_pending_query *pending;

    *replylen = 0;
    pending = agent_query(req, &reply, replylen, NULL, NULL);
    if (pending) {                 /* cannot happen without a callback; never wait */
        agent_cancel_query(pending);
        reply = NULL;
    }
    strbuf_free(req);
    return reply;
}

int kitty_reconnect_agent_keys(void)
{
    strbuf *req;
    void *reply;
    int replylen, result = -1;

    if (!agent_exists())
        return -1;
    req = strbuf_new_for_agent_query();
    put_byte(req, 11 /* SSH2_AGENTC_REQUEST_IDENTITIES */);
    reply = kr_agent_ask(req, &replylen);
    if (reply) {
        BinarySource src[1];
        BinarySource_BARE_INIT(src, reply, replylen);
        get_uint32(src);                            /* the length word */
        if (get_byte(src) == 12 /* SSH2_AGENT_IDENTITIES_ANSWER */) {
            uint32_t n = get_uint32(src);
            if (!get_err(src))
                result = n > 1000 ? 1000 : (int)n;
        }
        sfree(reply);
    }
    return result;
}

int kitty_reconnect_key_locked(void)
{
    strbuf *req;
    void *reply;
    int replylen, result = -1;
    static const char ext[] = "list-extended@putty.projects.tartarus.org";

    if (!st.agent_blob || !st.agent_bloblen || !agent_exists())
        return -1;
    req = strbuf_new_for_agent_query();
    put_byte(req, 27 /* SSH2_AGENTC_EXTENSION */);
    put_stringz(req, ext);
    reply = kr_agent_ask(req, &replylen);
    if (reply) {
        BinarySource src[1];
        BinarySource_BARE_INIT(src, reply, replylen);
        get_uint32(src);                            /* the length word */
        if (get_byte(src) == 6 /* SSH_AGENT_SUCCESS */) {
            uint32_t n = get_uint32(src), i;
            for (i = 0; i < n && !get_err(src); i++) {
                ptrlen blob = get_string(src);
                get_string(src);                    /* comment */
                ptrlen extra = get_string(src);
                if (get_err(src))
                    break;
                if (blob.len == st.agent_bloblen &&
                    !memcmp(blob.ptr, st.agent_blob, blob.len)) {
                    BinarySource xs[1];
                    BinarySource_BARE_INIT_PL(xs, extra);
                    uint32_t flags = get_uint32(xs);
                    result = (flags & 2) ? 1 : 0;
                    break;
                }
            }
        }
        sfree(reply);
    }
    return result;
}
