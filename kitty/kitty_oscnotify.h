/*
 * kitty_oscnotify.h - the platform-free half of desktop notifications from the
 * host (OSC 9, OSC 777, OSC 99) and of taskbar progress (OSC 9;4): parsing,
 * limits, text cleaning, the show/no-show decision, the timing mapping, the
 * flood rule and the exact replies. No Win32 and no PuTTY types, so
 * test/test_oscnotify.c covers all of it without a window. The Windows half
 * (the notice, focus, the taskbar button) is kitty/kitty_hostnotify.c.
 */
#ifndef KITTY_OSCNOTIFY_H
#define KITTY_OSCNOTIFY_H

#include <stddef.h>

/* Limits, in bytes after decoding (the OSC 99 specification's body limit,
 * applied to all three protocols). Longer text is cut, never dropped. */
#define ON_TITLE_MAX 256
#define ON_BODY_MAX  2048
#define ON_ID_MAX    64          /* a sanitised OSC 99 id */

/* The per-session setting (HostNotify). */
#define ON_SET_OFF       0
#define ON_SET_UNFOCUSED 1
#define ON_SET_ALWAYS    2

/* OSC 99 o= */
#define ON_OCC_ALWAYS    0
#define ON_OCC_UNFOCUSED 1
#define ON_OCC_INVISIBLE 2

/* OSC 99 p= */
#define ON_P_TITLE  0
#define ON_P_BODY   1
#define ON_P_CLOSE  2
#define ON_P_ALIVE  3
#define ON_P_QUERY  4
#define ON_P_OTHER  5            /* icon, buttons, unknown: accepted, dropped */

/* The flood rule: at most one notice per this many ms per window. */
#define ON_FLOOD_MS 2000

/* Display time in seconds for a "sticky" notice: a long timer rather than
 * the notice window's own sticky mode (see kitty_hostnotify.c). */
#define ON_STICKY_SECONDS 86400
#define ON_DEFAULT_SECONDS 15
#define ON_SHORT_SECONDS 5

/* One OSC 99 metadata block. has_* says the key was present, so chunks can
 * be merged key by key. */
typedef struct {
    char id[ON_ID_MAX + 1];      /* sanitised; OSC 99 without one: "0" (the
                                  * spec's rule); OSC 9 / 777: "" */
    int done;                    /* d, default 1 */
    int b64;                     /* e, default 0 */
    int payload;                 /* p, ON_P_*, default ON_P_TITLE */
    int occasion, has_o;         /* o, default ON_OCC_ALWAYS */
    int urgency, has_u;          /* u, -1 = unset */
    long expire, has_w;          /* w in ms, -1 = default */
    int focus, report, has_a;    /* a: focus default on, report off */
    int close_report, has_c;     /* c */
} OnMeta;

/* A notice being put together from OSC 99 chunks (d=0 ... d=1), or a whole
 * one from OSC 9 / 777. Raw decoded bytes, capped; cleaned when shown. */
typedef struct {
    int active;
    OnMeta meta;
    char title[ON_TITLE_MAX + 1];
    size_t title_len;
    char body[ON_BODY_MAX + 1];
    size_t body_len;
} OnNotice;

/* What a whole OSC 99 sequence asks for (on_osc99). */
#define ON99_NOTHING 0           /* malformed, a chunk, or ignored */
#define ON99_SHOW    1           /* *out holds a finished notice */
#define ON99_CLOSE   2           /* close the notice with meta.id */
#define ON99_ALIVE   3           /* reply which ids are alive */
#define ON99_QUERY   4           /* reply what is supported */

/*
 * Feed one OSC 99 string (everything after "99;"). `asm_` is the per-window
 * chunk assembly, carried between calls. On ON99_SHOW the finished notice is
 * in *out (and the assembly is reset); on the others out->meta holds the
 * request's metadata (id for the reply).
 */
int on_osc99(OnNotice *asm_, const char *s, size_t len, OnNotice *out);

/* OSC 9: what the string after "9;" is. */
#define ON9_NOTICE   0
#define ON9_PROGRESS 1           /* 9;4;... - *state and *value filled */
#define ON9_IGNORE   2           /* another ConEmu command number */
int on_osc9(const char *s, size_t len, int *state, int *value);

/* OSC 777: 1 and *out filled for "notify;<title>;<body>", else 0. */
int on_osc777(const char *s, size_t len, OnNotice *out);

/* Fill a notice from plain title/body bytes (the far2l entry point, OSC 9),
 * cutting each to its limit. Either may be NULL. */
void on_notice_set(OnNotice *n, const char *title, size_t tlen,
                   const char *body, size_t blen);

/*
 * Clean text for display: invalid UTF-8 dropped, C0/C1 controls stripped
 * (TAB, LF and CR become a space), cut to `max` bytes on a character
 * boundary. Writes a NUL-terminated string of at most max bytes into out
 * (which has max + 1 bytes) and returns its length.
 */
size_t on_text_clean(const char *in, size_t len, char *out, size_t max);

/* Sanitise an OSC 99 id to A-Za-z0-9-_+. and at most ON_ID_MAX bytes. */
void on_id_sanitise(const char *in, size_t len, char *out);

/* Strict base64 (padding optional). Returns the decoded length, or -1. out
 * must hold len * 3 / 4 + 3 bytes. */
long on_base64_decode(const char *in, size_t len, unsigned char *out);

/* Is a notice shown? Setting first; o= can only narrow it. */
int on_should_show(int setting, int occasion, int focused, int visible);

/* Display time in seconds (ON_STICKY_SECONDS for sticky) and whether the
 * notice carries the critical accent. */
int on_seconds(int urgency, long expire_ms, int *critical);

/* Replies, NUL-terminated into buf (at least ON_REPLY_MAX bytes); return the
 * length. The terminator is ESC \. */
#define ON_REPLY_MAX 256
#define ON_QUERY_CAPS \
    "a=focus,report:c=1:o=always,unfocused,invisible:" \
    "p=title,body,close,alive,?:u=0,1,2:w=1"
size_t on_reply_query(const char *id, char *buf);
size_t on_reply_alive(const char *id, const char *alive_ids, char *buf);
size_t on_reply_activated(const char *id, char *buf);
size_t on_reply_closed(const char *id, char *buf);

/*
 * The flood rule, per window. offer(): a notice arrived at `now` (ms); it is
 * either shown at once (ON_FLOOD_NOW) or becomes the pending one
 * (ON_FLOOD_PEND), replacing - and counting as dropped - any pending one.
 * When ON_FLOOD_PEND, the caller arms a timer for on_flood_due(). tick(): the
 * timer ran; returns ON_FLOOD_NOW when the pending notice is shown now (and
 * the timer is armed again for on_flood_due()), or ON_FLOOD_END when the
 * burst is over, with *dropped set to how many were dropped in it (one Event
 * Log line when > 0).
 */
#define ON_FLOOD_NOW  0
#define ON_FLOOD_PEND 1
#define ON_FLOOD_END  2
typedef struct {
    unsigned long last;          /* when the last notice was shown */
    int have_last;
    int pending;                 /* a notice waits */
    int armed;                   /* the timer is set */
    int dropped;                 /* replaced pending ones, this burst */
} OnFlood;
int on_flood_offer(OnFlood *f, unsigned long now);
unsigned long on_flood_due(const OnFlood *f);
int on_flood_tick(OnFlood *f, unsigned long now, int *dropped);

#endif
