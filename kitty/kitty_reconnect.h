/*
 * kitty_reconnect.h: the reconnect decision by failure condition
 * (hknet/KiTTY#66; how it works: kitty_reconnect.c).
 */

#ifndef KITTY_RECONNECT_H
#define KITTY_RECONNECT_H

#include "kitty_connstage.h"

/* The conditions, in table order. New symptoms get a new entry here, a row
 * in kitty_reconnect.c and a branch in the classifier - nothing else moves. */
typedef enum {
    KR_DNS,           /* the host name does not resolve */
    KR_TCP,           /* no TCP connection: refused, timed out, unreachable */
    KR_PRELOGIN,      /* connected, closed before the login began */
    KR_AGENTLOCKED,   /* closed while the agent held a signature (locked key) */
    KR_PROMPT,        /* closed while a prompt of ours was open, or cancelled */
    KR_NOKEY,         /* no agent and no key: nothing to log in with */
    KR_LOGINCLOSED,   /* closed during the login, nothing outstanding here */
    KR_REFUSED,       /* the server refused the login */
    KR_OTHER,         /* none of the above */
    KR_NCOND
} KrCond;

/* the actions a condition can be set to (the stored values in brackets) */
#define KRA_STOP    0     /* "stop" */
#define KRA_ONCE    1     /* "1" */
#define KRA_THREE   3     /* "3" */
#define KRA_RETRY  (-1)   /* "retry": keep retrying, with the backoff */
#define KRA_WAIT   (-2)   /* "wait": wait until the agent's key is usable */

/* One attempt, as the connection reports it (the session's own Seat only). */
void kitty_reconnect_attempt_begin(void);
void kitty_reconnect_stage(KittyConnStage stage, ptrlen data);
void kitty_reconnect_prompt_open(bool open);
void kitty_reconnect_logged_in(void);      /* a login: all counters reset */
void kitty_reconnect_connected(void);      /* non-SSH: in, the backoff kept */
bool kitty_reconnect_attempt_logged_in(void);
/* a session that was in has dropped: the wait before the first attempt */
int kitty_reconnect_drop_delay(int base_s, int max_s, int factor);

typedef struct {
    KrCond cond;
    int action;          /* KRA_*, as decided (the session override applied) */
    bool retry;          /* schedule another attempt in delay_ms */
    bool wait_unlock;    /* poll the agent instead, then one attempt */
    int delay_ms;
    int attempt;         /* how many retries this condition has had */
} KrDecision;

/* The decision after a failed attempt. have_agent: the session uses the
 * agent and it holds a key; have_keyfile: a key or certificate file;
 * have_gssapi: the session tries GSSAPI. login_never: the session's
 * "Retries after a failed login: never"; base_s / max_s / factor: the
 * backoff (the first wait, the cap, the factor per retry - 1 = fixed). */
KrDecision kitty_reconnect_decide(const char *msg, bool have_agent,
                                  bool have_keyfile, bool have_gssapi,
                                  bool login_never,
                                  int base_s, int max_s, int factor);

/* The key the last attempt's signature was for: 1 still locked in the agent,
 * 0 usable now, -1 gone from the agent or no agent (stop waiting). */
int kitty_reconnect_key_locked(void);

/* How many keys the agent holds, locked ones included (0 = an empty agent,
 * nothing to log in with); -1 no agent or no answer. */
int kitty_reconnect_agent_keys(void);

/* A cause worth mentioning beside the condition in the Event Log (the remedy
 * differs from the condition's), or NULL. */
const char *kitty_reconnect_hint(const char *msg);

/* "Name does not resolve" etc. - the panel's own words, for the Event Log */
const char *kitty_reconnect_cond_label(KrCond cond);
const char *kitty_reconnect_action_label(int action);

/* the setting's kitty.ini key and default, for the configuration panel */
const char *kitty_reconnect_cond_key(KrCond cond);
int kitty_reconnect_cond_default(KrCond cond);

#endif
