/*
 * kitty_progstatus.h - program status reports from the host (OSC 7501) and
 * the shell's prompt marks (OSC 133): the platform-free half. Parsing, the
 * limits, the record store and what the window shows as a whole. No Win32 and
 * no PuTTY types, so test/test_progstatus.c covers all of it without a
 * window. The Windows half (the taskbar button, the notices) is in
 * kitty/kitty_hostnotify.c.
 *
 * OSC 7501 ; key=value:key=value ST. Each report replaces one record, named
 * by its id (none = the root record); "clear" removes a record and every
 * record beneath it. OSC 7501 ; ? ST is answered with the same body.
 */
#ifndef KITTY_PROGSTATUS_H
#define KITTY_PROGSTATUS_H

#include <stddef.h>

/* The specification's limits. A report over any of them is discarded whole. */
#define PS_SEQ_MAX        4096   /* the whole sequence, OSC to ST */
#define PS_KEY_MAX        16
#define PS_ID_MAX         128
#define PS_ID_SEG_MAX     32
#define PS_ID_LEVELS      8
#define PS_APP_MAX        32
#define PS_TITLE_ENC_MAX  256
#define PS_TITLE_MAX      192    /* decoded */
#define PS_MSG_ENC_MAX    2732
#define PS_MSG_MAX        2048   /* decoded */
/* Records kept per terminal. The specification allows fewer than its 256;
 * the oldest record goes when a new one does not fit. */
#define PS_RECORDS_MAX    64

/* state= ; also the order of urgency for what the window shows, except that
 * blocked outranks error. */
#define PS_IDLE     0
#define PS_DONE     1
#define PS_WORKING  2
#define PS_ERROR    3
#define PS_BLOCKED  4
#define PS_CLEAR    5            /* not a state: removes records */

/* kind= (with blocked) */
#define PS_KIND_NONE       0
#define PS_KIND_PERMISSION 1
#define PS_KIND_QUESTION   2
#define PS_KIND_AUTH       3

typedef struct {
    char id[PS_ID_MAX + 1];      /* "" = the root record */
    int state;                   /* PS_* */
    int kind;                    /* PS_KIND_*, blocked only */
    int progress;                /* 0..100, -1 = unknown */
    char app[PS_APP_MAX + 1];    /* its own; "" = inherited */
    char title[PS_TITLE_MAX + 1];/* decoded UTF-8, no control characters */
    char msg[PS_MSG_MAX + 1];
} PsRecord;

/* What one OSC 7501 string is (ps_parse). */
#define PS_REJECT 0              /* nothing is applied */
#define PS_REPORT 1              /* *out holds the report */
#define PS_QUERY  2              /* "?": answer with PS_QUERY_REPLY */
#define PS_QUERY_REPLY "\033]7501;?\033\\"

/* s/len: everything after "7501;". */
int ps_parse(const char *s, size_t len, PsRecord *out);

/* One stored record: the report plus its order and whether the user has
 * seen it (done and error only). */
typedef struct {
    PsRecord r;
    unsigned long order;
    int seen;
} PsEntry;

typedef struct {
    PsEntry *e[PS_RECORDS_MAX];
    int n;
    unsigned long counter;
} PsStore;

/* What a report did to its record: *prev is its state before (-1 when it
 * was new or is now removed). Returns the stored entry, or NULL for a clear. */
PsEntry *ps_apply(PsStore *st, const PsRecord *r, int *prev);

/* The shell showed a prompt (OSC 133;A): working and blocked records go. */
void ps_drop_running(PsStore *st);
/* Everything goes (the terminal is going). */
void ps_free(PsStore *st);
/* The user has seen the window: done and error stop counting. */
void ps_mark_seen(PsStore *st);

/* The record's app, or the nearest parent's ("" when none has one). */
const char *ps_app_of(const PsStore *st, const PsRecord *r);

/* What the window shows: the most urgent state among the records (blocked,
 * error, working, done; done and error only while unseen; PS_IDLE when
 * nothing), its kind for blocked, and a percentage for working and blocked
 * (the most recent such record's, -1 when it gave none). */
typedef struct {
    int state;
    int kind;
    int progress;
} PsView;
void ps_view(const PsStore *st, PsView *v);

/* The title bar's marker for that summary (" (45%)", " (blocked)" ...), ""
 * when idle. buf holds PS_TITLE_MARKER_MAX bytes. */
#define PS_TITLE_MARKER_MAX 24
void ps_title_marker(const PsView *v, char *buf);

/* The overlay's rows (kitty_psoverlay.c): every record that is not idle,
 * grouped by the first segment of its id, the most urgent group first
 * (blocked, failed, working, done), a record's children right below it. */
typedef struct {
    const PsEntry *e;
    int depth;                       /* '/' in the id: 0 at the top */
    const char *name;                /* the title, else the id's last segment,
                                      * else the app */
    char parent[PS_ID_SEG_MAX + 1];  /* the parent's last segment, "" at the top */
} PsRow;
/* rows holds PS_RECORDS_MAX; returns how many were filled. */
int ps_rows(const PsStore *st, PsRow *rows);
/* Two or more rows, and something to look at: a record still working or
 * blocked, or a done or failed one not yet seen. */
int ps_overlay_due(const PsStore *st);

/* The words for a state - "Finished", "Failed", "Waiting for input" ... -
 * the taskbar overlay's text and the second half of a notice; "" for idle. */
const char *ps_state_phrase(int state, int kind);
/* A notice's line for a record: "<name>: <state>", the name its title,
 * else its app; the state alone when it has neither. */
void ps_notice_line(const PsStore *st, const PsRecord *r, char *buf,
                    size_t size);
/* The one-line notice in the window (kitty/kitty_switchnotice.c) for a
 * report that moved its record (prev: the state before, -1 = new) to
 * blocked, failed or finished - never for working or a new percentage -
 * while the status panel does not show (panel: it is up after this
 * report) and [KiTTY] osc7501notice (secs) is not 0. Returns the record's
 * state with ps_notice_line in buf; PS_IDLE (buf empty) when none is due. */
int ps_switch_notice(const PsStore *st, const PsRecord *r, int prev,
                     int panel, int secs, char *buf, size_t size);
/* The same line for OSC 9;4 progress: due when the state switches (prev:
 * the taskbar bar's state before) TO 2 (error) or 4 (paused) - not on a
 * repeat, never for 0, 1 or 3 - while no OSC 7501 notice of this window
 * shows (ps7501_up: both drive the same bar, one event is reported once)
 * and [KiTTY] osc7501notice (secs) is not 0. value: the percentage sent,
 * -1 = none (then the line carries none). Returns the icon's state,
 * PS_ERROR or PS_BLOCKED, with the line in buf; PS_IDLE (buf empty) when
 * none is due. */
int ps_progress_notice(int prev, int state, int value, int ps7501_up,
                       int secs, char *buf, size_t size);

/* OSC 133 (everything after "133;"). */
#define PS133_OTHER  0
#define PS133_PROMPT 1           /* A: a prompt starts */
#define PS133_END    2           /* D[;exit]: a command finished */
#define PS133_OUTPUT 3           /* C: a command's output starts */
#define PS133_INPUT  4           /* B: the typed command starts */
int ps_osc133(const char *s, size_t len, int *exit_code);

#endif
