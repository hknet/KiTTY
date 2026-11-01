/*
 * kitty_auditchain.h - the agent log's chain: every line carries seq (a
 * counter over the whole log, continued across runs and files) and prev
 * (the SHA-256 of the line before it, exactly as on disk, base64) and sig,
 * an Ed25519 signature over every byte of the line before ` sig="`, prev
 * included: each record carries the signature over itself plus the one
 * before it. The key is made for each kageant run and held in its memory
 * only. The anchor records (ev="logchain", event ID 171) carry the run's
 * public key. An anchor's own prev is the head it pins. Each anchor also
 * goes to the Windows Event Log, where a writer can add records but never
 * remove or reorder the ones written.
 *
 * One signature costs a few milliseconds with PuTTY's constant-time code
 * (test_auditchain --bench). A flood of identical records costs one: the
 * repeats are counted, not written, and one summary record
 * (ev="repeated", event ID 172) stands for them (kitty_auditchain_repeat).
 *
 * Plain C, no Win32: kageant's writer (kitty_auditlog.c) and verifier
 * (kitty_auditverify.c) build on it. test/test_auditchain.c runs it on
 * lines held in memory.
 */
#ifndef KITTY_AUDITCHAIN_H
#define KITTY_AUDITCHAIN_H

#include <stddef.h>

#define KITTY_AUDITCHAIN_SEEDLEN   32   /* an Ed25519 private key */
#define KITTY_AUDITCHAIN_PUBLEN    32
#define KITTY_AUDITCHAIN_SIGLEN    64
#define KITTY_AUDITCHAIN_HASHLEN   32
#define KITTY_AUDITCHAIN_B64HASH   44   /* base64 of 32 bytes, no NUL */
#define KITTY_AUDITCHAIN_RUNLEN    32   /* the run id: 16 bytes as hex */

/* An anchor every 15 minutes while records were written since the last. */
#define KITTY_AUDITCHAIN_HEAD_MS   (15UL * 60UL * 1000UL)
/* kageant reads at most this much of the end of the log to continue the
 * chain; a longer last line makes it read further, up to the whole file. */
#define KITTY_AUDITCHAIN_TAIL      65536

/*
 * The fields of an anchor record, in the order the line carries them. The
 * file line is ts=, ev="logchain", then these fields when not empty, then
 * seq and prev (and sig, which is not part of the signed bytes). Event 171
 * carries them in the order of its row in kitty_eventids.h (the caller's
 * fields, then seq, sig, ts and prev, as every agent event):
 * kitty_auditchain_anchor_from_event maps them.
 */
enum {
    KAC_RESULT, KAC_RUN, KAC_LOGKEY, KAC_FILE, KAC_FRAGMENT, KAC_VALUE,
    KAC_ROTATION, KAC_KEEP, KAC_EXPIRED,
    KAC_NCALLER,                         /* the fields kitty_audit is given */
    KAC_TS = KAC_NCALLER, KAC_PREV, KAC_SEQ, KAC_SIG,
    KAC_NFIELDS
};
extern const char *const kitty_auditchain_fieldnames[KAC_NFIELDS];

/* ---- keys ---- */

/* The public key of a seed; 0 on failure. */
int kitty_auditchain_public(const unsigned char seed[KITTY_AUDITCHAIN_SEEDLEN],
                            unsigned char pub[KITTY_AUDITCHAIN_PUBLEN]);
/* A live ssh_key for a seed (ssh_key_free it), NULL on failure: the writer
 * keeps it encrypted in memory between anchors (kitty_protkey). */
struct ssh_key *kitty_auditchain_key(
    const unsigned char seed[KITTY_AUDITCHAIN_SEEDLEN]);
/* The raw 64-byte Ed25519 signature of data under key. */
void kitty_auditchain_sign(struct ssh_key *key, const void *data, size_t len,
                           unsigned char sig[KITTY_AUDITCHAIN_SIGLEN]);
/* 1 when sig is pub's signature of data. */
int kitty_auditchain_verify_sig(const unsigned char pub[KITTY_AUDITCHAIN_PUBLEN],
                                const void *data, size_t len,
                                const unsigned char sig[KITTY_AUDITCHAIN_SIGLEN]);

/* A value as the log writes it: in double quotes, a quote doubled, every
 * control byte (CR and LF included) as ?. The file's integrity rests on
 * this: a comment from the pipe can never close its quote early or forge a
 * line. */
void kitty_auditchain_put_quoted(struct strbuf *sb, const char *v);
/* The bytes of an anchor record before its chain fields: ts=, ev="logchain"
 * and the fields of v (KAC_RESULT .. KAC_EXPIRED) that are not empty, in
 * that order. The writer and the verifier both build an anchor this way, so
 * an event's copy can be checked against its signature. */
void kitty_auditchain_anchor_body(struct strbuf *sb, const char *ts,
                                  const char *const v[KAC_NCALLER]);

/* base64 of n bytes into out (4 * ceil(n / 3) + 1 bytes), NUL-terminated. */
void kitty_auditchain_b64(const unsigned char *p, size_t n, char *out);
/* The SHA-256 of a line's bytes, as base64 (KITTY_AUDITCHAIN_B64HASH + 1). */
void kitty_auditchain_hash_b64(const char *line, size_t len, char *out);

/* ---- the writer's state ---- */

struct kitty_auditchain_writer {
    int active;                      /* the chain is being written */
    unsigned long long seq;          /* the seq of the NEXT line */
    char prev[KITTY_AUDITCHAIN_B64HASH + 1];   /* "none" or the last hash */
    unsigned rotation;               /* rotations of this log so far */
};

/* What the end of an existing log file holds (kitty_auditchain_tail). */
struct kitty_auditchain_tail {
    int have_line;                   /* a complete line exists */
    char head[KITTY_AUDITCHAIN_B64HASH + 1];   /* its hash */
    unsigned long long last_seq;     /* the newest seq in the tail, 0 none */
    size_t fragment;                 /* bytes after the last CRLF */
    int need_more;                   /* no complete line in a partial tail */
};

/* Read the last bytes of a file (data, len; whole = 1 when that is the
 * whole file). A partial tail whose first line may be cut is read from its
 * first CRLF on. */
void kitty_auditchain_tail(const char *data, size_t len, int whole,
                           struct kitty_auditchain_tail *t);
/* The rotation number carried by the first line of a file (a rotate
 * anchor's rotation field), 0 when it carries none. */
unsigned kitty_auditchain_first_rotation(const char *data, size_t len);

/* Append the chain fields to one record without its CRLF: seq and prev,
 * then, with key, the signature. seq_out and sig_out (may be NULL) get the
 * values as written, for the Event Log copy (sig_out "" without key). The
 * writer state is NOT advanced: kitty_auditchain_commit does that once the
 * line is on disk. */
void kitty_auditchain_seal(const struct kitty_auditchain_writer *w,
                           struct strbuf *line, struct ssh_key *key,
                           char seq_out[24], char sig_out[89]);
/* The line (without CRLF) is written whole: it is the new head. */
void kitty_auditchain_commit(struct kitty_auditchain_writer *w,
                             const char *line, size_t len);

/* ---- "previous record repeated N times" ---- */

/*
 * Two records are the same when their kind and every field are equal: the
 * body compared is ev= and the fields, without ts and the chain fields. The
 * first is written in full at once. The ones after it are only counted. A
 * summary record (ev="repeated": count, first and last = the ts of the
 * first and the last repeat counted, of = the seq of the record repeated)
 * stands for them. It is written when a different record arrives, at the
 * first record or tick KITTY_AUDITCHAIN_REPEAT_MS after the first repeat,
 * and before every anchor (rotation and the stop included). A summary is a
 * normal record: chained and signed. After it the count starts again for
 * the same record.
 */
#define KITTY_AUDITCHAIN_REPEAT_MS (60UL * 1000UL)

struct kitty_auditchain_repeat {
    char *body;                      /* the last record written in full
                                      * (sfree), NULL none */
    char of[24];                     /* its seq, "" when not in the file */
    unsigned long count;             /* the repeats counted */
    char first[32], last[32];        /* their ts */
    unsigned long first_ms;          /* the tick of the first repeat */
};

/* The fields of a summary record, as text. */
struct kitty_auditchain_summary {
    char count[24], first[32], last[32], of[24];
};

#define KAC_REPEAT_SUMMARY 1         /* write the summary first */
#define KAC_REPEAT_WRITE   2         /* then the record, in full */

/* One record about to be written (body: ev= and its fields), at ts and
 * the tick now_ms. Returns KAC_REPEAT_SUMMARY (s filled) and/or
 * KAC_REPEAT_WRITE; 0 when the record is a repeat, counted and not to be
 * written. ms is the interval after the first repeat. */
int kitty_auditchain_repeat_record(struct kitty_auditchain_repeat *r,
                                   const char *body, const char *ts,
                                   unsigned long now_ms, unsigned long ms,
                                   struct kitty_auditchain_summary *s);
/* The record of body is written in full, its seq (NULL or "" when the
 * file is not written): the record repeats are compared with. */
void kitty_auditchain_repeat_written(struct kitty_auditchain_repeat *r,
                                     const char *body, const char *seq);
/* 1 and s filled when repeats are counted: with force (before an anchor,
 * at the stop) whatever their age, else only ms after the first. The
 * count starts again from 0. */
int kitty_auditchain_repeat_flush(struct kitty_auditchain_repeat *r,
                                  unsigned long now_ms, unsigned long ms,
                                  int force,
                                  struct kitty_auditchain_summary *s);
/* Everything dropped (a new chain, another file). */
void kitty_auditchain_repeat_forget(struct kitty_auditchain_repeat *r);

/* ---- the Windows Event Log copy of a record (100-172) ---- */

/*
 * Every agent event carries %1 = the kind, then the fields of its row in
 * kitty_eventids.h in that order, ending with seq, sig, ts and prev. The
 * signed bytes of the record's line are rebuilt from the event alone:
 *
 *   ts=<ts> ev="<kind>"
 *   then for each field of the row but seq, sig, ts and prev, in the
 *     row's order, when its string is not empty: ` <name>="<value>"`
 *   then ` seq="<seq>" prev="<prev>"`
 *
 * A value is quoted as the file quotes it (kitty_auditchain_put_quoted: a
 * quote doubled, every control byte already ? in the event). sig is the
 * Ed25519 signature of exactly those bytes under the run's logkey. The
 * writer puts a record's fields into the row's order (kitty_auditlog.c),
 * so the file line is the same text.
 */

/* The signed bytes of event id from its insertion strings s[0..n-1] (s[0]
 * = %1) into line. *sig (may be NULL) gets the sig string, *seq its seq (0
 * when empty). 0 when id is no agent ID or n is not its count. */
int kitty_auditchain_event_line(unsigned id, const char *const *s, size_t n,
                                strbuf *line, const char **sig,
                                unsigned long long *seq);
/* The insertion strings of a record's event as the writer reports them:
 * %1 = ev, then the row's fields from keys/vals (an absent one ""), every
 * control byte as ?. Returns their count (sfree each and the array), 0
 * when ev has no agent ID; *id gets the ID. */
size_t kitty_auditchain_event_strings(const char *ev,
                                      const char *const *keys,
                                      const char *const *vals, int n,
                                      unsigned *id, char ***out);
/* The fields of an anchor event (171) by their names: v[KAC_*] point into
 * s, NULL for a field the event does not carry. */
void kitty_auditchain_anchor_from_event(const char *const *s, size_t n,
                                        const char *v[KAC_NFIELDS]);

/* ---- the verifier ---- */

/* One file of the log: the active file and its generations, any order. */
struct kitty_auditchain_file {
    const char *name;                /* as reports show it (kageant.log.1) */
    const char *data;
    size_t len;
};

/* One anchor event (171) read from the Windows Event Log. */
struct kitty_auditchain_anchor {
    const char *log;                 /* KiTTY++_Agent, Application, a file */
    unsigned long long record;       /* EventRecordID, 0 unknown */
    const char *time;                /* its TimeCreated, UTC, ISO 8601 */
    const char *v[KAC_NFIELDS];      /* the insertion strings after %1 */
};

/* One agent event (100-170, 172) read from the Windows Event Log. */
struct kitty_auditchain_event {
    const char *log;                 /* as for an anchor */
    unsigned long long record;       /* EventRecordID, 0 unknown */
    const char *time;                /* its TimeCreated, UTC, ISO 8601 */
    unsigned id;
    const char *const *s;            /* its insertion strings, %1 first */
    size_t n;
};

struct kitty_auditchain_verify_in {
    const struct kitty_auditchain_file *files;
    size_t nfiles;
    /* the other agent events of the Windows Event Log: each checked by its
     * signature against its run and its line in the file */
    const struct kitty_auditchain_event *events;
    size_t nevents;
    /* the Windows Event Log: have_anchors 0 = it could not be read */
    int have_anchors;
    const struct kitty_auditchain_anchor *anchors;
    size_t nanchors;
    const char *anchors_since;       /* the oldest record of the logs read,
                                      * UTC ISO 8601; NULL unknown */
    const char *anchors_where;       /* the logs read, for the reports */
    const char *path;                /* the active file's path, as anchors
                                      * carry it in their file field */
    const char *live_run;            /* the run of a kageant writing this
                                      * log now, "*" for the last run when
                                      * a kageant runs, or NULL */
    int full;                        /* every line's signature checked on
                                      * its own (else: see below) */
};

/* Verify. The report (one problem or note per line, then the verdict line)
 * goes to out. Returns 0 intact, 1 broken, 2 cannot verify. The first
 * line of out is the first problem when there is one.
 *
 * The signatures: a line's signature covers its prev, the hash of the line
 * before it, so one valid signature binds every line of the unbroken chain
 * before it under the same key. Without full, the verifier checks the
 * signature of the last line of each such stretch (a run's last line, the
 * line before a break of the chain or a change of key) and every anchor.
 * When one does not fit, every line of its stretch is checked on its own,
 * so the report names each line that does not fit. A line whose own
 * signature is bad inside a stretch whose end fits can only come from the
 * holder of the run key, who could as well have signed it right: full
 * reports it too. The Event Log copies are checked the same way along the
 * chain of their prev fields (kac_events). */
int kitty_auditchain_verify(const struct kitty_auditchain_verify_in *in,
                            struct strbuf *out);

#endif /* KITTY_AUDITCHAIN_H */
