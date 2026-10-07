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

/* OSC 133 (everything after "133;"). */
#define PS133_OTHER  0
#define PS133_PROMPT 1           /* A: a prompt starts */
#define PS133_END    2           /* D[;exit]: a command finished */
#define PS133_OUTPUT 3           /* C: a command's output starts */
#define PS133_INPUT  4           /* B: the typed command starts */
int ps_osc133(const char *s, size_t len, int *exit_code);

#endif
