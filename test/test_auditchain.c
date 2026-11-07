/*
 * test_auditchain - regression tests for kitty/kitty_auditchain.c, the
 * agent log's chain: lines built the way kageant writes them (seq, prev
 * and an Ed25519 signature on every line, a fixed seed so the output is the
 * same on every run), verified, then every tamper case: one byte changed, a
 * line deleted, two lines swapped, a line without chain fields inserted, a
 * line with a valid hash but no signature or another key's, a line signed
 * by another key, the problem list capped, the file cut with
 * and without the anchors of the Windows Event Log, the whole file replaced
 * by a fresh chain, rotation (every generation, a middle one gone, the
 * oldest rotated out, the oldest deleted inside the keep count, deleted by
 * age), a crash fragment accounted for and not, a mixed file of old and
 * chained lines, an old kageant's block between two runs, CRLF turned into
 * LF, the signed bytes of a value holding quotes and ` sig="`, a forged
 * anchor in the Windows Event Log, a run without a stop record, a live run,
 * a restarted chain, records cut from the start of the oldest file, a
 * deleted log, keys the Event Log does not confirm, the tail kageant reads
 * to continue a log, and the rule for repeated records (counted, one
 * summary; written at a different record, a minute after the first repeat,
 * before an anchor, at rotation and at the stop; a different pid, result or
 * fp written at once), and the Windows Event Log copy of every record: the
 * signed line rebuilt from the event's strings for every agent ID, a
 * changed string, a forged event, an event whose line is gone, another log
 * file's events.
 *
 * `test_auditchain --bench` times 1000 Ed25519 signatures as the agent
 * would make them. `test_auditchain --bench-verify` builds a full-size log
 * (4 files of 5120 KB, 5 runs, a head anchor every 15 minutes, every record
 * also as its Windows Event Log event, plus another log file's events with
 * the same seq) and times the whole verify 3 times; `--bench-verify MB
 * RUNS [full]` builds MB megabytes (split over 4 files) and verifies RUNS
 * times; full adds one verify with every line's signature checked on its
 * own (in.full).
 * Returns non-zero on any failure.
 */
#ifdef _WIN32
#include <winsock2.h>   /* before windows.h: putty.h pulls it in further down */
#include <windows.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "putty.h"
#include "ssh.h"
#include "kitty/kitty_auditchain.h"
#define KITTY_EVENTIDS_TABLE
#include "kitty/kitty_eventids.h"

const char *appname = "test_auditchain";

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

/* the key code is linked for Ed25519; nothing here draws random numbers */
void old_keyfile_warning(void) {}
void random_read(void *buf, size_t size)
{
    (void)buf;
    (void)size;
    modalfatalbox("random_read called in test_auditchain");
}

static int failures = 0, checks = 0;

#define PATH "C:\\test\\kageant.log"

/* ---- a log written as kageant writes it ---- */

#define MAXF 16
#define MAXEV 128
struct tlog {
    struct kitty_auditchain_writer w;
    strbuf *cur;
    strbuf *gens[MAXF];              /* finished generations, oldest first */
    int ngens;
    struct ssh_key *key;
    struct ssh_key *other;           /* another key, for sign_mode 2 */
    int sign_mode;                   /* records: 0 the run key, 1 none, 2 other */
    char pub[64];
    char run[40];
    int clock;
    unsigned rotation;
    struct kitty_auditchain_anchor ev[MAXEV];
    char *evs[MAXEV][KAC_NFIELDS];
    int nev;
    const char *path;                /* the anchors' file, NULL = PATH */
    /* the Event Log copies of the records written with tl_erec */
    struct kitty_auditchain_event xev[MAXEV];
    char **xs[MAXEV];
    char xtime[MAXEV][32];
    int nxev;
};

static void tl_ts(struct tlog *t, char *ts)
{
    int c = t->clock++;
    snprintf(ts, 32, "2026-11-01T%02d:%02d:%02dZ", 8 + c / 3600,
             (c / 60) % 60, c % 60);
}

static void tl_key(struct tlog *t, unsigned char seedbyte, const char *run)
{
    unsigned char seed[KITTY_AUDITCHAIN_SEEDLEN], pub[KITTY_AUDITCHAIN_PUBLEN];
    memset(seed, seedbyte, sizeof(seed));
    t->key = kitty_auditchain_key(seed);
    kitty_auditchain_public(seed, pub);
    kitty_auditchain_b64(pub, sizeof(pub), t->pub);
    snprintf(t->run, sizeof(t->run), "%s", run);
}

static void tl_init(struct tlog *t, unsigned char seedbyte, const char *run)
{
    memset(t, 0, sizeof(*t));
    tl_key(t, seedbyte, run);
    t->cur = strbuf_new();
    t->w.active = 1;
    t->w.seq = 1;
    strcpy(t->w.prev, "none");
}

/* continue a log in another run: the same files, a new key and run */
static void tl_newrun(struct tlog *t, unsigned char seedbyte, const char *run)
{
    ssh_key_free(t->key);
    tl_key(t, seedbyte, run);
}

static void tl_line(struct tlog *t, strbuf *line)
{
    kitty_auditchain_commit(&t->w, line->s, line->len);
    put_data(t->cur, line->s, line->len);
    put_datapl(t->cur, PTRLEN_LITERAL("\r\n"));
}

/* the body of a record as kageant builds it (audit_put_record): ev, then
 * the key/value pairs */
static strbuf *tl_vbody(const char *ev, va_list ap)
{
    strbuf *b = strbuf_new();
    const char *k;
    put_datapl(b, PTRLEN_LITERAL("ev="));
    kitty_auditchain_put_quoted(b, ev);
    while ((k = va_arg(ap, const char *)) != NULL) {
        const char *v = va_arg(ap, const char *);
        put_fmt(b, " %s=", k);
        kitty_auditchain_put_quoted(b, v);
    }
    return b;
}

/* a line: ts, the body, the chain fields and the signature */
static void tl_put(struct tlog *t, const char *body)
{
    strbuf *line = strbuf_new();
    char ts[32];
    tl_ts(t, ts);
    put_fmt(line, "ts=%s %s", ts, body);
    kitty_auditchain_seal(&t->w, line, t->sign_mode == 1 ? NULL :
                          t->sign_mode == 2 ? t->other : t->key, NULL, NULL);
    tl_line(t, line);
    strbuf_free(line);
}

/* a record: ev and key/value pairs, NULL-terminated */
static void tl_rec(struct tlog *t, const char *ev, ...)
{
    strbuf *b;
    va_list ap;
    va_start(ap, ev);
    b = tl_vbody(ev, ap);
    va_end(ap);
    tl_put(t, b->s);
    strbuf_free(b);
}

/* an anchor and its copy in the Windows Event Log */
static void tl_anchor_ex(struct tlog *t, const char *result,
                         const char *fragment, const char *value,
                         const char *rotation, const char *keep,
                         const char *expired)
{
    const char *v[KAC_NFIELDS];
    strbuf *line = strbuf_new();
    char ts[32], seq[24], sig[89], prev[64];
    int i;
    memset(v, 0, sizeof(v));
    v[KAC_RESULT] = result;
    v[KAC_RUN] = t->run;
    v[KAC_LOGKEY] = t->pub;
    v[KAC_FILE] = t->path ? t->path : PATH;
    v[KAC_FRAGMENT] = fragment;
    v[KAC_VALUE] = value;
    v[KAC_ROTATION] = rotation;
    v[KAC_KEEP] = keep;
    v[KAC_EXPIRED] = expired;
    tl_ts(t, ts);
    snprintf(prev, sizeof(prev), "%s", t->w.prev);
    kitty_auditchain_anchor_body(line, ts, v);
    kitty_auditchain_seal(&t->w, line, t->key, seq, sig);
    tl_line(t, line);
    strbuf_free(line);
    if (t->nev < MAXEV) {
        struct kitty_auditchain_anchor *a = &t->ev[t->nev];
        v[KAC_TS] = ts;
        v[KAC_PREV] = prev;
        v[KAC_SEQ] = seq;
        v[KAC_SIG] = sig;
        a->log = "the Windows Event Log (Application, source KiTTY++_agent)";
        a->record = 1000 + (unsigned long long)t->nev;
        a->time = "2026-11-01T08:00:00Z";
        for (i = 0; i < KAC_NFIELDS; i++) {
            t->evs[t->nev][i] = v[i] ? dupstr(v[i]) : NULL;
            a->v[i] = t->evs[t->nev][i];
        }
        t->nev++;
    }
}

static void tl_anchor(struct tlog *t, const char *result)
{
    tl_anchor_ex(t, result, NULL, NULL, NULL, NULL, NULL);
}

static void tl_rotate(struct tlog *t, int keep, int expired)
{
    char r[16], k[16], e[16];
    if (t->ngens < MAXF)
        t->gens[t->ngens++] = t->cur;
    t->cur = strbuf_new();
    t->rotation++;
    snprintf(r, sizeof(r), "%u", t->rotation);
    snprintf(k, sizeof(k), "%d", keep);
    snprintf(e, sizeof(e), "%d", expired);
    tl_anchor_ex(t, "rotate", NULL, NULL, r, k, expired ? e : NULL);
}

static void tl_free(struct tlog *t)
{
    int i, j;
    for (i = 0; i < t->ngens; i++)
        strbuf_free(t->gens[i]);
    strbuf_free(t->cur);
    for (i = 0; i < t->nev; i++)
        for (j = 0; j < KAC_NFIELDS; j++)
            sfree(t->evs[i][j]);
    for (i = 0; i < t->nxev; i++) {
        for (j = 0; j < (int)t->xev[i].n; j++)
            sfree(t->xs[i][j]);
        sfree(t->xs[i]);
    }
    ssh_key_free(t->key);
}

/* 103 lines, seq 1-103, line N holding seq N: a start anchor, the agent
 * start, 98 signatures with a head anchor at line 60, the agent stop and a
 * stop anchor. */
static void tl_standard(struct tlog *t)
{
    int i;
    char n[16];
    tl_init(t, 7, "00112233445566778899aabbccddeeff");
    tl_anchor(t, "start");
    tl_rec(t, "agent", "result", "start", (const char *)NULL);
    for (i = 3; i <= 101; i++) {
        if (i == 60) {
            tl_anchor(t, "head");
            continue;
        }
        snprintf(n, sizeof(n), "%d", i);
        tl_rec(t, "sign", "fp", "ssh-ed25519 255 SHA256:abc", "comment", n,
               "result", "allowed", "pid", "1234", (const char *)NULL);
    }
    tl_rec(t, "agent", "result", "stop", (const char *)NULL);
    tl_anchor(t, "stop");
}

/* ---- lines of a file text ---- */

struct lines {
    char *l[400];
    int n;
};

static void split_lines(const strbuf *sb, struct lines *ls)
{
    size_t s = 0, i;
    ls->n = 0;
    for (i = 0; i + 1 < sb->len; i++)
        if (sb->s[i] == '\r' && sb->s[i + 1] == '\n') {
            ls->l[ls->n++] = dupprintf("%.*s", (int)(i - s), sb->s + s);
            s = i + 2;
            i++;
        }
}

static strbuf *join_lines(const struct lines *ls, const char *eol)
{
    strbuf *sb = strbuf_new();
    int i;
    for (i = 0; i < ls->n; i++) {
        put_dataz(sb, ls->l[i]);
        put_dataz(sb, eol);
    }
    return sb;
}

static void free_lines(struct lines *ls)
{
    int i;
    for (i = 0; i < ls->n; i++)
        sfree(ls->l[i]);
    ls->n = 0;
}

static void drop_line(struct lines *ls, int i)
{
    sfree(ls->l[i]);
    memmove(&ls->l[i], &ls->l[i + 1], (ls->n - i - 1) * sizeof(char *));
    ls->n--;
}

static void insert_line(struct lines *ls, int i, char *line)
{
    memmove(&ls->l[i + 1], &ls->l[i], (ls->n - i) * sizeof(char *));
    ls->l[i] = line;
    ls->n++;
}

/* Recompute the prev values from line `from` (0-based) on, as an attacker
 * who knows the format would: only a signature can stop him. */
static void rechain(struct lines *ls, int from)
{
    int i;
    for (i = from > 0 ? from : 1; i < ls->n; i++) {
        char h[64], *p, *q, *nl;
        kitty_auditchain_hash_b64(ls->l[i - 1], strlen(ls->l[i - 1]), h);
        p = strstr(ls->l[i], " prev=\"");
        if (!p)
            continue;
        q = strchr(p + 7, '"');
        if (!q)
            continue;
        nl = dupprintf("%.*s prev=\"%s%s", (int)(p - ls->l[i]), ls->l[i], h, q);
        sfree(ls->l[i]);
        ls->l[i] = nl;
    }
}

/* ---- verification ---- */

static int g_full = 0;                  /* in.full for the next verifies */

static int run_verify(struct kitty_auditchain_file *files, size_t nfiles,
                      const struct tlog *ev, int nev, int have_anchors,
                      const char *since, const char *live, strbuf *out)
{
    struct kitty_auditchain_verify_in in;
    memset(&in, 0, sizeof(in));
    in.full = g_full;
    in.files = files;
    in.nfiles = nfiles;
    in.have_anchors = have_anchors;
    in.anchors = ev ? ev->ev : NULL;
    in.nanchors = ev ? (size_t)(nev >= 0 ? nev : ev->nev) : 0;
    in.anchors_since = since;
    in.anchors_where = "the Windows Event Log (Application, source KiTTY++_agent)";
    in.path = PATH;
    in.live_run = live;
    strbuf_clear(out);
    return kitty_auditchain_verify(&in, out);
}

static void expect(const char *what, int code, int want_code, const strbuf *out,
                   const char *want_first, const char *want_any,
                   const char *want_none)
{
    const char *e = strstr(out->s, "\r\n");
    int ok = code == want_code;
    char *first = dupprintf("%.*s",
                            (int)(e ? (size_t)(e - out->s) : out->len), out->s);
    if (want_first && !strstr(first, want_first))
        ok = 0;
    if (want_any && !strstr(out->s, want_any))
        ok = 0;
    if (want_none && strstr(out->s, want_none))
        ok = 0;
    checks++;
    if (!ok) {
        failures++;
        printf("FAIL %s: exit %d (want %d)\n  first line wanted: %s\n  "
               "text wanted: %s\n  text not wanted: %s\n--- report:\n%s---\n",
               what, code, want_code, want_first ? want_first : "-",
               want_any ? want_any : "-", want_none ? want_none : "-", out->s);
    } else {
        printf("ok   %s\n", what);
    }
    sfree(first);
}

static void one_file(struct kitty_auditchain_file *f, const strbuf *sb)
{
    f->name = "kageant.log";
    f->data = sb->s;
    f->len = sb->len;
}

/* the standard log with one change; verified against all its anchors */
static int verify_lines(const struct tlog *t, struct lines *ls, const char *eol,
                        strbuf *out)
{
    struct kitty_auditchain_file f[1];
    strbuf *mod = join_lines(ls, eol);
    int code;
    one_file(f, mod);
    code = run_verify(f, 1, t, -1, 1, NULL, NULL, out);
    strbuf_free(mod);
    free_lines(ls);
    return code;
}

/* ---- the cases ---- */

static void test_intact_and_tamper(void)
{
    struct tlog t;
    struct kitty_auditchain_file f[1];
    struct lines ls;
    strbuf *out = strbuf_new(), *mod;
    int code, i;
    char *tmp;

    tl_standard(&t);
    one_file(f, t.cur);
    code = run_verify(f, 1, &t, -1, 1, NULL, NULL, out);
    expect("1 intact: 103 lines, every anchor confirmed", code, 0, out,
           NULL, "Chain intact, every record signed.", "BROKEN");
    expect("1 the summary line", code, 0, out, NULL,
           "kageant.log: 103 records, seq 1-103.", NULL);

    /* 2: one byte of a value in line 50 */
    split_lines(t.cur, &ls);
    tmp = strstr(ls.l[49], "comment=\"50\"");
    tmp[9] = '9';
    code = verify_lines(&t, &ls, "\r\n", out);
    expect("2 one byte changed in line 50", code, 1, out,
           "kageant.log line 50 (seq 50,", "the record was changed", NULL);
    /* 2b: the same with every later prev recomputed: the line's own
     * signature shows it */
    split_lines(t.cur, &ls);
    tmp = strstr(ls.l[49], "comment=\"50\"");
    tmp[9] = '9';
    rechain(&ls, 50);
    code = verify_lines(&t, &ls, "\r\n", out);
    expect("2b changed and re-chained: the line's own signature", code, 1,
           out, "kageant.log line 50 (seq 50,", "signature does not match",
           NULL);

    /* 3: line 50 deleted */
    split_lines(t.cur, &ls);
    drop_line(&ls, 49);
    code = verify_lines(&t, &ls, "\r\n", out);
    expect("3 line 50 deleted", code, 1, out,
           "kageant.log: 1 record missing between seq 49 and 51.", NULL, NULL);

    /* 4: lines 50 and 51 swapped */
    split_lines(t.cur, &ls);
    tmp = ls.l[49];
    ls.l[49] = ls.l[50];
    ls.l[50] = tmp;
    code = verify_lines(&t, &ls, "\r\n", out);
    expect("4 lines 50 and 51 swapped", code, 1, out,
           "kageant.log line 50 (seq 51): records out of order.", NULL, NULL);

    /* 5: a well-formed line without chain fields after line 50 */
    split_lines(t.cur, &ls);
    insert_line(&ls, 50, dupstr("ts=2026-11-01T09:00:00Z ev=\"sign\" fp=\"x\" result=\"allowed\""));
    code = verify_lines(&t, &ls, "\r\n", out);
    expect("5 a line without chain fields inserted", code, 1, out,
           "kageant.log line 51: a record without chain fields inside a run",
           NULL, NULL);
    /* 5b: a forged chained line, the lines after it renumbered and
     * re-chained: the next anchor's signature */
    split_lines(t.cur, &ls);
    insert_line(&ls, 50, dupstr("ts=2026-11-01T09:00:00Z ev=\"sign\" fp=\"x\" result=\"allowed\" seq=\"51\" prev=\"x\""));
    for (i = 51; i < ls.n; i++) {
        char *p = strstr(ls.l[i], " seq=\""), *q, *nl;
        unsigned long s;
        if (!p)
            continue;
        s = strtoul(p + 6, &q, 10);
        nl = dupprintf("%.*s seq=\"%lu%s", (int)(p - ls.l[i]), ls.l[i],
                       s + 1, q);
        sfree(ls.l[i]);
        ls.l[i] = nl;
    }
    rechain(&ls, 50);
    code = verify_lines(&t, &ls, "\r\n", out);
    expect("5b a forged chained line, re-chained: it has no signature", code,
           1, out, "kageant.log line 51 (seq 51,",
           "no signature - written by another program.", NULL);

    /* 7: cut after line 80; the stop anchor is in the Event Log */
    split_lines(t.cur, &ls);
    while (ls.n > 80)
        sfree(ls.l[--ls.n]);
    mod = join_lines(&ls, "\r\n");
    free_lines(&ls);
    one_file(f, mod);
    code = run_verify(f, 1, &t, -1, 1, NULL, NULL, out);
    expect("7 cut after line 80, the anchors read", code, 1, out,
           "kageant.log ends at seq 80, ",
           "seq 103 from 2026-11-01T08:00:00Z: 23 records cut off.", NULL);
    /* 7b: the same cut, the Windows Event Log not readable: reported as the
     * limit, never "intact" */
    code = run_verify(f, 1, NULL, 0, 0, NULL, NULL, out);
    expect("7b the same cut without the anchors: reported as the limit", code,
           2, out, NULL,
           "The chain in the files is intact up to seq 80. A cut at the end or a replaced file cannot be seen without it.",
           "Chain intact");
    /* 7c: the anchors read, no anchor after the cut (start and head only):
     * the records after the last anchor are named */
    code = run_verify(f, 1, &t, 2, 1, NULL, NULL, out);
    expect("7c cut after the last anchor: the records after it named", code,
           0, out, NULL,
           "seq 61-80 after the last anchor (seq 60): signed, not yet pinned by an anchor.",
           NULL);
    expect("7c and no stop record", code, 0, out, NULL,
           "Run started 2026-11-01T08:00:00Z ended without a stop record.",
           NULL);
    /* a run still writing: no stop record expected, the open records named */
    code = run_verify(f, 1, &t, 2, 1, NULL, t.run, out);
    expect("a live run: still writing, the open records named", code, 0, out,
           NULL, "is still writing this log.", "without a stop");
    strbuf_free(mod);

    /* 12: CRLF turned to LF on every line */
    split_lines(t.cur, &ls);
    code = verify_lines(&t, &ls, "\n", out);
    expect("12 CRLF turned to LF", code, 1, out,
           "kageant.log line 1: the line ends are not CRLF", NULL, NULL);

    /* keys the Event Log does not confirm: readable, none of this run.
     * Older than the oldest record there: rolled out. Else: unconfirmed. */
    one_file(f, t.cur);
    code = run_verify(f, 1, &t, 0, 1, "2026-11-02T00:00:00Z", NULL, out);
    expect("a run older than the Event Log: rolled out, cannot verify", code,
           2, out, NULL, "has no anchor left: its key cannot be confirmed.",
           "Chain intact");
    code = run_verify(f, 1, &t, 0, 1, "2026-10-01T00:00:00Z", NULL, out);
    expect("a run key without an anchor: not confirmed, cannot verify", code,
           2, out, NULL, "no anchor in the Windows Event Log", "Chain intact");

    /* the chain starting again mid-file (prev none) */
    split_lines(t.cur, &ls);
    tmp = strstr(ls.l[69], " prev=\"");
    {
        char *nl = dupprintf("%.*s prev=\"none\"", (int)(tmp - ls.l[69]),
                             ls.l[69]);
        sfree(ls.l[69]);
        ls.l[69] = nl;
    }
    code = verify_lines(&t, &ls, "\r\n", out);
    expect("a chain that starts again mid-file", code, 1, out,
           "kageant.log line 70: the chain starts again", NULL, NULL);

    /* records removed at the start of the only file */
    split_lines(t.cur, &ls);
    for (i = 0; i < 10; i++)
        drop_line(&ls, 0);
    code = verify_lines(&t, &ls, "\r\n", out);
    expect("records removed at the start of the oldest file", code, 1, out,
           NULL, "kageant.log: starts at seq 11", NULL);

    /* the whole log deleted: an empty file, the anchors of its path */
    mod = strbuf_new();
    one_file(f, mod);
    code = run_verify(f, 1, &t, -1, 1, NULL, NULL, out);
    expect("a deleted log: an empty file, the anchors of its path", code, 1,
           out, NULL, "log restarted, the Windows Event Log", NULL);
    strbuf_free(mod);

    strbuf_free(out);
    tl_free(&t);
}

/* 6 and 8: another key; a forged anchor in the Event Log */
static void test_other_keys(void)
{
    struct tlog t, x;
    struct kitty_auditchain_file f[1];
    struct lines ls, lx;
    strbuf *out = strbuf_new();
    int code, i;

    tl_standard(&t);
    /* 6: an anchor of the same run made with another key, inserted after
     * line 50 */
    tl_init(&x, 9, t.run);
    x.w.seq = 51;
    tl_anchor(&x, "head");
    split_lines(t.cur, &ls);
    split_lines(x.cur, &lx);
    insert_line(&ls, 50, lx.l[0]);
    lx.n = 0;
    code = verify_lines(&t, &ls, "\r\n", out);
    expect("6 a line signed by another key inserted", code, 1, out, NULL,
           "kageant.log line 51: the run started 2026-11-01T08:00:00Z carries a key other than the one of its start record.",
           NULL);

    /* an anchor in the Event Log under the run's id, signed by another key:
     * listed as forged, the file itself intact */
    {
        struct tlog e = t;
        struct kitty_auditchain_anchor fa = x.ev[0];
        fa.v[KAC_LOGKEY] = t.pub;          /* the run's key, a foreign sig */
        e.ev[e.nev++] = fa;
        one_file(f, t.cur);
        code = run_verify(f, 1, &e, -1, 1, NULL, NULL, out);
        expect("a forged anchor in the Event Log (the run's key named)", code,
               1, out, NULL,
               "anchor not signed by the key of its run - written by another program.",
               NULL);
    }
    tl_free(&x);

    /* 8: the whole file replaced by a fresh chain with a new key and run;
     * the old run's anchors are in the Event Log */
    tl_init(&x, 11, "ffeeddccbbaa99887766554433221100");
    tl_anchor(&x, "start");
    for (i = 0; i < 100; i++)
        tl_rec(&x, "sign", "result", "allowed", (const char *)NULL);
    tl_anchor(&x, "stop");
    one_file(f, x.cur);
    code = run_verify(f, 1, &t, -1, 1, NULL, NULL, out);
    expect("8 whole file replaced, the old anchors read", code, 1, out,
           "kageant.log line 1 (seq 1): the anchor of",
           "is of another run - the file was replaced.", NULL);
    /* 8c: the replaced file without the Windows Event Log: the limit */
    code = run_verify(f, 1, NULL, 0, 0, NULL, NULL, out);
    expect("8c a replaced file without the anchors: reported as the limit",
           code, 2, out, NULL,
           "A cut at the end or a replaced file cannot be seen", "Chain intact");
    tl_free(&x);
    /* 8b: the run id kept, a new key */
    tl_init(&x, 11, t.run);
    tl_anchor(&x, "start");
    tl_rec(&x, "sign", "result", "allowed", (const char *)NULL);
    one_file(f, x.cur);
    code = run_verify(f, 1, &t, -1, 1, NULL, NULL, out);
    expect("8b the run's key replaced", code, 1, out, NULL,
           "its key in the file differs from the key in the Windows Event Log",
           NULL);
    tl_free(&x);
    strbuf_free(out);
    tl_free(&t);
}

/* every line signed: a line whose hash fits but whose signature is
 * missing or another key's; a record before any anchor of its run; the
 * problem list capped */
static void test_line_signatures(void)
{
    struct tlog t;
    struct kitty_auditchain_file f[1];
    struct lines ls;
    strbuf *out = strbuf_new();
    unsigned char seed[KITTY_AUDITCHAIN_SEEDLEN];
    int code, mode, i;
    char *tmp;

    for (mode = 1; mode <= 2; mode++) {
        tl_init(&t, 7, "00112233445566778899aabbccddeeff");
        memset(seed, 9, sizeof(seed));
        t.other = kitty_auditchain_key(seed);
        tl_anchor(&t, "start");
        for (i = 0; i < 3; i++)
            tl_rec(&t, "sign", "result", "allowed", (const char *)NULL);
        t.sign_mode = mode;              /* line 5: written by another */
        tl_rec(&t, "sign", "result", "allowed", (const char *)NULL);
        t.sign_mode = 0;
        tl_rec(&t, "sign", "result", "allowed", (const char *)NULL);
        tl_anchor(&t, "stop");
        one_file(f, t.cur);
        if (mode == 2) {
            /* bound by the next line's signature: found only line by line */
            code = run_verify(f, 1, &t, -1, 1, NULL, NULL, out);
            expect("a line signed by another key, bound by the next line's signature: intact without full",
                   code, 0, out, NULL, "Chain intact", "BROKEN");
            g_full = 1;
        }
        code = run_verify(f, 1, &t, -1, 1, NULL, NULL, out);
        g_full = 0;
        if (mode == 1)
            expect("a line with a valid hash and no signature", code, 1, out,
                   "kageant.log line 5 (seq 5,",
                   "no signature - written by another program.", NULL);
        else
            expect("a line with a valid hash, signed by another key", code, 1,
                   out, "kageant.log line 5 (seq 5,",
                   "signature does not match - the record was changed.",
                   NULL);
        expect("  and that line is the only problem", code, 1, out, NULL,
               "Chain BROKEN: 1 problem, at kageant.log line 5", NULL);
        ssh_key_free(t.other);
        tl_free(&t);
    }

    /* a record before the start anchor: no key to check it with */
    tl_init(&t, 7, "00112233445566778899aabbccddeeff");
    tl_rec(&t, "agent", "result", "start", (const char *)NULL);
    tl_anchor(&t, "start");
    tl_anchor(&t, "stop");
    one_file(f, t.cur);
    code = run_verify(f, 1, &t, -1, 1, NULL, NULL, out);
    expect("a record before any anchor of its run", code, 1, out,
           "kageant.log line 1 (seq 1,",
           "no anchor of its run before it - its signature cannot be checked.",
           NULL);
    tl_free(&t);

    /* 302 lines, line 3 changed and every line after re-chained: 300
     * signatures break, the stop anchor differs from its event: 100 of the
     * 301 problems are listed */
    tl_init(&t, 7, "00112233445566778899aabbccddeeff");
    tl_anchor(&t, "start");
    for (i = 0; i < 300; i++)
        tl_rec(&t, "sign", "comment", "abc", (const char *)NULL);
    tl_anchor(&t, "stop");
    split_lines(t.cur, &ls);
    tmp = strstr(ls.l[2], "comment=\"abc\"");
    tmp[9] = 'x';
    rechain(&ls, 3);
    code = verify_lines(&t, &ls, "\r\n", out);
    expect("a file rewritten from line 3 on: 100 problems listed", code, 1, out,
           "kageant.log line 3 (seq 3,", "201 more problems, not listed.",
           NULL);
    expect("  the count of them all", code, 1, out, NULL,
           "Chain BROKEN: 301 problems, the first at kageant.log line 3", NULL);
    tl_free(&t);
    strbuf_free(out);
}

/* ---- the rule for repeated records, as kitty_audit applies it ---- */

static struct kitty_auditchain_repeat trep;
static unsigned long tnow;            /* the writer's tick, ms */

static void tw_ts(char *ts)
{
    snprintf(ts, 32, "2026-11-01T10:%02lu:%02luZ", (tnow / 60000) % 60,
             (tnow / 1000) % 60);
}

static void tw_summary(struct tlog *t, const struct kitty_auditchain_summary *s)
{
    if (s->of[0])
        tl_rec(t, "repeated", "count", s->count, "first", s->first, "last",
               s->last, "of", s->of, (const char *)NULL);
    else
        tl_rec(t, "repeated", "count", s->count, "first", s->first, "last",
               s->last, (const char *)NULL);
}

/* one record: counted when it repeats the last one written in full, else
 * written after the summary of the repeats */
static void tw_rec(struct tlog *t, const char *ev, ...)
{
    struct kitty_auditchain_summary s;
    strbuf *b;
    char ts[32], seq[24];
    int act;
    va_list ap;
    va_start(ap, ev);
    b = tl_vbody(ev, ap);
    va_end(ap);
    tw_ts(ts);
    act = kitty_auditchain_repeat_record(&trep, b->s, ts, tnow,
                                         KITTY_AUDITCHAIN_REPEAT_MS, &s);
    if (act & KAC_REPEAT_SUMMARY)
        tw_summary(t, &s);
    if (act & KAC_REPEAT_WRITE) {
        snprintf(seq, sizeof(seq), "%lu", (unsigned long)t->w.seq);
        tl_put(t, b->s);
        kitty_auditchain_repeat_written(&trep, b->s, seq);
    }
    strbuf_free(b);
}

/* the timer; with force: before an anchor, at rotation, at the stop */
static void tw_flush(struct tlog *t, int force)
{
    struct kitty_auditchain_summary s;
    if (kitty_auditchain_repeat_flush(&trep, tnow, KITTY_AUDITCHAIN_REPEAT_MS,
                                      force, &s))
        tw_summary(t, &s);
}

static void tw_init(struct tlog *t)
{
    tl_init(t, 7, "00112233445566778899aabbccddeeff");
    kitty_auditchain_repeat_forget(&trep);
    tnow = 0;
    tl_anchor(t, "start");                       /* seq 1 */
}

static void tw_refused(struct tlog *t)
{
    tw_rec(t, "request", "result", "refused", "reason",
           "unrecognised message", "req", "pwsh.exe", "pid", "1234",
           (const char *)NULL);
}

/* how many lines of text hold s */
static int count_in(const strbuf *sb, const char *s)
{
    int n = 0;
    const char *p = sb->s;
    while ((p = strstr(p, s)) != NULL) {
        n++;
        p += strlen(s);
    }
    return n;
}

static void ok_if(int cond, const char *what)
{
    checks++;
    if (!cond) {
        failures++;
        printf("FAIL %s\n", what);
    } else {
        printf("ok   %s\n", what);
    }
}

/* the line of a 1-based number, in the active file */
static int line_has(const struct tlog *t, int lineno, const char *s)
{
    struct lines ls;
    int ok;
    split_lines(t->cur, &ls);
    ok = lineno >= 1 && lineno <= ls.n && strstr(ls.l[lineno - 1], s);
    free_lines(&ls);
    return ok;
}

static void verify_intact(const struct tlog *t, const char *what)
{
    struct kitty_auditchain_file f[1];
    strbuf *out = strbuf_new();
    int code;
    one_file(f, t->cur);
    code = run_verify(f, 1, t, -1, 1, NULL, NULL, out);
    expect(what, code, 0, out, NULL, "Chain intact, every record signed.",
           "BROKEN");
    strbuf_free(out);
}

static void test_repeat(void)
{
    struct tlog t;
    int i;

    /* a flood: 100 identical refusals, then a different record */
    tw_init(&t);
    tw_refused(&t);                              /* seq 2, written */
    for (i = 1; i < 100; i++) {
        tnow += 100;
        tw_refused(&t);                          /* counted */
    }
    tw_rec(&t, "agent", "result", "stop", (const char *)NULL);
    ok_if(count_in(t.cur, "ev=\"request\"") == 1,
          "repeat: 100 identical refusals, the first written once");
    ok_if(line_has(&t, 3, "ev=\"repeated\" count=\"99\" "
                   "first=\"2026-11-01T10:00:00Z\" "
                   "last=\"2026-11-01T10:00:09Z\" of=\"2\" seq=\"3\""),
          "repeat: one summary of 99, first and last, of the record's seq");
    ok_if(line_has(&t, 4, "ev=\"agent\" result=\"stop\""),
          "repeat: the summary before the different record (the order kept)");
    tw_flush(&t, 1);
    tl_anchor(&t, "stop");
    verify_intact(&t, "repeat: the log with its summary is intact");
    tl_free(&t);

    /* a different pid, result or fp: every record in full */
    tw_init(&t);
    tw_rec(&t, "sign", "fp", "a", "result", "allowed", "pid", "1",
           (const char *)NULL);
    tw_rec(&t, "sign", "fp", "a", "result", "allowed", "pid", "2",
           (const char *)NULL);
    tw_rec(&t, "sign", "fp", "a", "result", "denied", "pid", "2",
           (const char *)NULL);
    tw_rec(&t, "sign", "fp", "b", "result", "denied", "pid", "2",
           (const char *)NULL);
    ok_if(count_in(t.cur, "ev=\"sign\"") == 4 &&
          count_in(t.cur, "ev=\"repeated\"") == 0,
          "repeat: a different pid, result, fp - each written at once");
    tl_free(&t);

    /* a minute after the first repeat: at the next repeat, and on a tick */
    tw_init(&t);
    tw_refused(&t);                              /* seq 2 */
    tnow = 1000;
    tw_refused(&t);                              /* counted, the first */
    tnow = 30000;
    tw_flush(&t, 0);
    ok_if(count_in(t.cur, "ev=\"repeated\"") == 0,
          "repeat: a tick 29 s after the first repeat writes nothing");
    tnow = 62000;
    tw_refused(&t);                              /* summary of 1, counted */
    ok_if(line_has(&t, 3, "ev=\"repeated\" count=\"1\" "
                   "first=\"2026-11-01T10:00:01Z\" "
                   "last=\"2026-11-01T10:00:01Z\" of=\"2\""),
          "repeat: a repeat 61 s after the first: the summary first");
    tnow = 70000;
    tw_flush(&t, 0);
    ok_if(count_in(t.cur, "ev=\"repeated\"") == 1,
          "repeat: then counting again, a tick 8 s later writes nothing");
    tnow = 123000;
    tw_flush(&t, 0);
    ok_if(line_has(&t, 4, "ev=\"repeated\" count=\"1\" "
                   "first=\"2026-11-01T10:01:02Z\" "
                   "last=\"2026-11-01T10:01:02Z\" of=\"2\"") &&
          count_in(t.cur, "ev=\"request\"") == 1,
          "repeat: a tick 61 s after the next first repeat: its summary");
    tl_free(&t);

    /* before an anchor, at the stop */
    tw_init(&t);
    tw_refused(&t);
    tw_refused(&t);
    tw_refused(&t);
    tw_flush(&t, 1);
    tl_anchor(&t, "head");
    ok_if(line_has(&t, 3, "ev=\"repeated\" count=\"2\"") &&
          line_has(&t, 4, "result=\"head\""),
          "repeat: the summary before a head anchor");
    tw_refused(&t);                              /* counted against seq 2 */
    tw_flush(&t, 1);
    tl_anchor(&t, "stop");
    ok_if(line_has(&t, 5, "ev=\"repeated\" count=\"1\"") &&
          line_has(&t, 6, "result=\"stop\""),
          "repeat: the summary before the stop anchor");
    verify_intact(&t, "repeat: summaries around anchors, intact");
    tl_free(&t);

    /* at rotation: the summary is the last line of the file moved to .1 */
    tw_init(&t);
    tw_refused(&t);
    tw_refused(&t);
    tw_flush(&t, 1);
    tl_rotate(&t, 3, 0);
    tw_refused(&t);                              /* counted */
    tw_flush(&t, 1);
    tl_anchor(&t, "stop");
    {
        struct kitty_auditchain_file f[2];
        strbuf *out = strbuf_new();
        int code;
        const char *last;
        f[0].name = "kageant.log.1";
        f[0].data = t.gens[0]->s;
        f[0].len = t.gens[0]->len;
        f[1].name = "kageant.log";
        f[1].data = t.cur->s;
        f[1].len = t.cur->len;
        last = t.gens[0]->len > 2 ? t.gens[0]->s : "";
        while (strstr(last, "\r\n") && strstr(last, "\r\n")[2])
            last = strstr(last, "\r\n") + 2;
        ok_if(strstr(last, "ev=\"repeated\" count=\"1\"") != NULL &&
              line_has(&t, 1, "result=\"rotate\""),
              "repeat: at rotation the summary ends the old file");
        code = run_verify(f, 2, &t, -1, 1, NULL, NULL, out);
        expect("repeat: summaries over a rotation, intact", code, 0, out, NULL,
               "Chain intact, every record signed.", "BROKEN");
        strbuf_free(out);
    }
    tl_free(&t);
    kitty_auditchain_repeat_forget(&trep);
}

/* ---- the Windows Event Log copy of every record ---- */

static int kac_test_pub(const struct tlog *t, unsigned char *pub)
{
    strbuf *sb = base64_decode_sb(ptrlen_from_asciz(t->pub));
    int ok = sb->len == KITTY_AUDITCHAIN_PUBLEN;
    if (ok)
        memcpy(pub, sb->u, KITTY_AUDITCHAIN_PUBLEN);
    strbuf_free(sb);
    return ok;
}

static int kac_test_sig(const char *b64, unsigned char *sig)
{
    strbuf *sb = base64_decode_sb(ptrlen_from_asciz(b64));
    int ok = sb->len == KITTY_AUDITCHAIN_SIGLEN;
    if (ok)
        memcpy(sig, sb->u, KITTY_AUDITCHAIN_SIGLEN);
    strbuf_free(sb);
    return ok;
}

/* A record as kageant writes it, with its event: the fields in the order
 * given (the row's order, as the writer puts them), the line signed, the
 * event's strings made from the same fields plus seq, sig, ts and prev. */
static void tl_erec(struct tlog *t, const char *ev, ...)
{
    const char *keys[32], *vals[32];
    strbuf *line = strbuf_new();
    char ts[32], seq[24], sig[89], prev[64];
    unsigned id;
    int n = 0, i;
    va_list ap;
    const char *k;
    va_start(ap, ev);
    while ((k = va_arg(ap, const char *)) != NULL && n < 28) {
        keys[n] = k;
        vals[n++] = va_arg(ap, const char *);
    }
    va_end(ap);
    tl_ts(t, ts);
    snprintf(prev, sizeof(prev), "%s", t->w.prev);
    put_fmt(line, "ts=%s ev=", ts);
    kitty_auditchain_put_quoted(line, ev);
    for (i = 0; i < n; i++)
        if (*vals[i]) {
            put_fmt(line, " %s=", keys[i]);
            kitty_auditchain_put_quoted(line, vals[i]);
        }
    kitty_auditchain_seal(&t->w, line, t->key, seq, sig);
    tl_line(t, line);
    strbuf_free(line);
    keys[n] = "seq"; vals[n++] = seq;
    keys[n] = "sig"; vals[n++] = sig;
    keys[n] = "ts"; vals[n++] = ts;
    keys[n] = "prev"; vals[n++] = prev;
    if (t->nxev < MAXEV) {
        struct kitty_auditchain_event *e = &t->xev[t->nxev];
        char **s = NULL;
        size_t ns = kitty_auditchain_event_strings(ev, keys, vals, n, &id, &s);
        memset(e, 0, sizeof(*e));
        snprintf(t->xtime[t->nxev], 32, "%s", ts);
        e->log = "the Windows Event Log (KiTTY++_Agent, source KiTTY++_agent)";
        e->record = 5000 + (unsigned long long)t->nxev;
        e->time = t->xtime[t->nxev];
        e->id = id;
        e->s = (const char *const *)s;
        e->n = ns;
        t->xs[t->nxev] = s;
        t->nxev++;
    }
}

static int run_verify_ev(struct kitty_auditchain_file *files, size_t nfiles,
                         const struct kitty_auditchain_anchor *a, size_t na,
                         const struct kitty_auditchain_event *e, size_t ne,
                         strbuf *out)
{
    struct kitty_auditchain_verify_in in;
    memset(&in, 0, sizeof(in));
    in.files = files;
    in.nfiles = nfiles;
    in.have_anchors = 1;
    in.anchors = a;
    in.nanchors = na;
    in.events = e;
    in.nevents = ne;
    in.full = g_full;
    in.anchors_where = "the Windows Event Log (KiTTY++_Agent, source KiTTY++_agent)";
    in.path = PATH;
    strbuf_clear(out);
    return kitty_auditchain_verify(&in, out);
}

/* the signed part of a line: the bytes before ` sig="` */
static size_t signed_part(const char *line)
{
    const char *p = strstr(line, " sig=\"");
    const char *q;
    while (p && (q = strstr(p + 1, " sig=\"")) != NULL)
        p = q;                           /* the last one: the chain's */
    return p ? (size_t)(p - line) : strlen(line);
}

static void test_event_copies(void)
{
    struct tlog t, x;
    struct kitty_auditchain_file f[1];
    struct kitty_auditchain_anchor anch[2 * MAXEV];
    struct kitty_auditchain_event evs[MAXEV + 2];
    const char *fake[40];
    strbuf *out = strbuf_new(), *rb = strbuf_new();
    struct lines ls;
    size_t i, k, na;
    int code, bad = 0, nids = 0;
    unsigned char pub[KITTY_AUDITCHAIN_PUBLEN], sig[KITTY_AUDITCHAIN_SIGLEN];
    static const char *const vals[] = {
        "a \"quoted\" value", "C:\\Program Files\\x\\y.exe", "",
        "plain", "tab\there", "\\\\server\\share",
    };

    /* every agent ID: the line written, its event, the line rebuilt from
     * the event alone - the same bytes, the signature valid */
    tl_init(&t, 7, "00112233445566778899aabbccddeeff");
    tl_anchor(&t, "start");
    for (i = 0; i < lenof(kitty_eventids); i++) {
        const struct kitty_eventid *r = &kitty_eventids[i];
        const char *keys[32], *kv[32];
        char *buf, *p, **s = NULL;
        size_t ns, line_at;
        int n = 0, j;
        unsigned id;
        const char *sigtxt;
        unsigned long long sq;
        strbuf *line;
        char ts[32], seq[24], sg[89], prev[64];
        if (r->block != KITTY_EVB_AGENT || !r->kind)
            continue;
        nids++;
        buf = dupstr(r->fields);
        for (p = strtok(buf, " "); p; p = strtok(NULL, " "))
            if (strcmp(p, "seq") && strcmp(p, "sig") && strcmp(p, "ts") &&
                strcmp(p, "prev")) {
                keys[n] = p;
                kv[n] = vals[(n + i) % lenof(vals)];
                n++;
            }
        line = strbuf_new();
        tl_ts(&t, ts);
        snprintf(prev, sizeof(prev), "%s", t.w.prev);
        put_fmt(line, "ts=%s ev=", ts);
        kitty_auditchain_put_quoted(line, r->kind);
        for (j = 0; j < n; j++)
            if (*kv[j]) {
                put_fmt(line, " %s=", keys[j]);
                kitty_auditchain_put_quoted(line, kv[j]);
            }
        kitty_auditchain_seal(&t.w, line, t.key, seq, sg);
        line_at = signed_part(line->s);
        keys[n] = "seq"; kv[n++] = seq;
        keys[n] = "sig"; kv[n++] = sg;
        keys[n] = "ts"; kv[n++] = ts;
        keys[n] = "prev"; kv[n++] = prev;
        ns = kitty_auditchain_event_strings(r->kind, keys, kv, n, &id, &s);
        strbuf_clear(rb);
        if (!ns || id != r->id ||
            !kitty_auditchain_event_line(id, (const char *const *)s, ns, rb,
                                         &sigtxt, &sq) ||
            rb->len != line_at || memcmp(rb->s, line->s, line_at) ||
            strcmp(sigtxt, sg)) {
            bad++;
            printf("  rebuild of %u differs:\n  line  %.*s\n  event %s\n",
                   r->id, (int)line_at, line->s, rb->s);
        }
        tl_line(&t, line);
        strbuf_free(line);
        for (k = 0; k < ns; k++)
            sfree(s[k]);
        sfree(s);
        sfree(buf);
    }
    ok_if(nids == 27 && !bad,
          "events: the line rebuilt from the event, every agent ID (quotes, backslashes, empty fields, a control byte)");
    tl_free(&t);

    /* a log with its events: every event matches its line */
    tl_init(&t, 7, "00112233445566778899aabbccddeeff");
    tl_anchor(&t, "start");
    tl_erec(&t, "agent", "result", "start", (const char *)NULL);
    tl_erec(&t, "add", "fp", "ssh-ed25519 255 SHA256:abc", "comment",
            "a \"c\" \\x", "result", "done", "reason", "ipc", "req",
            "ssh-add.exe", "reqpath", "C:\\Windows\\ssh-add.exe", "pid",
            "1234", "path", "", "restrict", "", (const char *)NULL);
    tl_erec(&t, "request", "result", "refused", "reason",
            "unrecognised message", "req", "pwsh.exe", "pid", "42",
            (const char *)NULL);
    tl_erec(&t, "repeated", "count", "99", "first", "2026-11-01T08:00:04Z",
            "last", "2026-11-01T08:00:09Z", "of", "4", (const char *)NULL);
    tl_erec(&t, "agent", "result", "stop", (const char *)NULL);
    tl_anchor(&t, "stop");
    one_file(f, t.cur);
    code = run_verify_ev(f, 1, t.ev, (size_t)t.nev, t.xev, (size_t)t.nxev, out);
    expect("events: every event of the run matches its line", code, 0, out,
           NULL, "5 events of the Windows Event Log (KiTTY++_Agent, source KiTTY++_agent) match their lines in the file.",
           "BROKEN");

    /* one string of the add event changed: its signature no longer fits */
    memcpy(evs, t.xev, sizeof(t.xev[0]) * (size_t)t.nxev);
    for (k = 0; k < t.xev[1].n && k < 40; k++)
        fake[k] = t.xev[1].s[k];
    fake[2] = "an edited comment";
    evs[1].s = fake;
    strbuf_clear(rb);
    ok_if(kitty_auditchain_event_line(evs[1].id, fake, evs[1].n, rb, NULL,
                                      NULL) &&
          kac_test_pub(&t, pub) &&
          kac_test_sig(t.xev[1].s[11], sig) &&
          !kitty_auditchain_verify_sig(pub, rb->s, rb->len, sig),
          "events: a changed string - the signature does not fit");
    code = run_verify_ev(f, 1, t.ev, (size_t)t.nev, evs, (size_t)t.nxev, out);
    expect("events: an edited event reported", code, 1, out, NULL,
           "(event 110, seq 3): not signed by the key of its run - forged or edited.",
           NULL);

    /* a forged event: made-up seq, made-up sig, written during the run */
    {
        char fsig[89];
        const char *forged[14];
        memset(fsig, 'A', 86);
        strcpy(fsig + 86, "==");
        forged[0] = "add"; forged[1] = "ssh-ed25519 255 SHA256:evil";
        forged[2] = "evil"; forged[3] = "done"; forged[4] = "ipc";
        forged[5] = "ssh-add.exe"; forged[6] = "C:\\x.exe";
        forged[7] = "666"; forged[8] = ""; forged[9] = "";
        forged[10] = "9999"; forged[11] = fsig;
        forged[12] = "2026-11-01T08:00:02Z"; forged[13] = "none";
        memcpy(evs, t.xev, sizeof(t.xev[0]) * (size_t)t.nxev);
        evs[t.nxev] = t.xev[1];
        evs[t.nxev].s = forged;
        evs[t.nxev].n = lenof(forged);
        evs[t.nxev].record = 7777;
        evs[t.nxev].time = "2026-11-01T08:00:02Z";
        code = run_verify_ev(f, 1, t.ev, (size_t)t.nev, evs,
                             (size_t)t.nxev + 1, out);
        expect("events: a forged event with a made-up signature reported",
               code, 1, out, NULL,
               "Record 7777 of the Windows Event Log (KiTTY++_Agent, source KiTTY++_agent) (event 110, seq 9999): not signed by the key of its run - forged or edited.",
               NULL);
        /* the same written before the run: not this log's, not reported */
        evs[t.nxev].time = "2026-10-31T08:00:02Z";
        code = run_verify_ev(f, 1, t.ev, (size_t)t.nev, evs,
                             (size_t)t.nxev + 1, out);
        expect("events: an unsigned event before every run of the file: left alone",
               code, 0, out, NULL, "Chain intact", "forged");
    }

    /* the line of the request event deleted: the event is signed by the
     * run, no file holds its line */
    split_lines(t.cur, &ls);
    drop_line(&ls, 3);
    {
        strbuf *mod = join_lines(&ls, "\r\n");
        free_lines(&ls);
        one_file(f, mod);
        code = run_verify_ev(f, 1, t.ev, (size_t)t.nev, t.xev,
                             (size_t)t.nxev, out);
        expect("events: an event whose line is gone", code, 1, out, NULL,
               "(event 103, seq 4): signed by the run started 2026-11-01T08:00:00Z, no file holds its line.",
               NULL);
        strbuf_free(mod);
    }

    /* another log file's run, its seq the same as this file's: its own */
    tl_init(&x, 9, "ffeeddccbbaa99887766554433221100");
    x.path = "C:\\other\\kageant.log";
    x.clock = 1;
    tl_anchor(&x, "start");
    tl_erec(&x, "agent", "result", "start", (const char *)NULL);
    tl_erec(&x, "lock", "result", "done", (const char *)NULL);
    tl_anchor(&x, "stop");
    na = 0;
    for (k = 0; k < (size_t)t.nev; k++)
        anch[na++] = t.ev[k];
    for (k = 0; k < (size_t)x.nev; k++)
        anch[na++] = x.ev[k];
    memcpy(evs, t.xev, sizeof(t.xev[0]) * (size_t)t.nxev);
    memcpy(evs + t.nxev, x.xev, sizeof(x.xev[0]) * (size_t)x.nxev);
    one_file(f, t.cur);
    code = run_verify_ev(f, 1, anch, na, evs, (size_t)(t.nxev + x.nxev), out);
    expect("events: another log file's events in the same log, left alone",
           code, 0, out, NULL, "5 events of", "forged");
    tl_free(&x);

    tl_free(&t);
    strbuf_free(rb);
    strbuf_free(out);
}

/* 9: rotation */
static void tl_rotated(struct tlog *t, int keep, int expired_last)
{
    int g, i;
    tl_init(t, 7, "00112233445566778899aabbccddeeff");
    tl_anchor(t, "start");
    for (g = 0; g < 5; g++) {
        if (g)
            tl_rotate(t, keep, g == 4 ? expired_last : 0);
        for (i = 0; i < 20; i++)
            tl_rec(t, "sign", "result", "allowed", (const char *)NULL);
    }
    tl_anchor(t, "stop");
}

/* the five files of tl_rotated, oldest first: f[0] = rotation 0 */
static void tl_rotated_files(const struct tlog *t,
                             struct kitty_auditchain_file *f)
{
    static const char *const names[] = {
        "kageant.log.4", "kageant.log.3", "kageant.log.2", "kageant.log.1",
    };
    int g;
    for (g = 0; g < 4; g++) {
        f[g].name = names[g];
        f[g].data = t->gens[g]->s;
        f[g].len = t->gens[g]->len;
    }
    f[4].name = "kageant.log";
    f[4].data = t->cur->s;
    f[4].len = t->cur->len;
}

static void test_rotation(void)
{
    struct tlog t;
    struct kitty_auditchain_file f[5], r[5];
    strbuf *out = strbuf_new();
    int code, g;

    /* rotations 1-4 with keep 2: kept on disk are the active file, .1 and
     * .2 (rotations 4, 3 and 2) */
    tl_rotated(&t, 2, 0);
    tl_rotated_files(&t, f);
    code = run_verify(f, 5, &t, -1, 1, NULL, NULL, out);
    expect("9 rotation: every generation present", code, 0, out, NULL,
           "Chain intact", NULL);
    for (g = 0; g < 5; g++)
        r[g] = f[4 - g];
    code = run_verify(r, 5, &t, -1, 1, NULL, NULL, out);
    expect("9 rotation: the files given in another order", code, 0, out, NULL,
           "Chain intact", NULL);
    code = run_verify(f + 2, 3, &t, -1, 1, NULL, NULL, out);
    expect("9 rotation: the oldest rotated out (a warning only)", code, 0, out,
           NULL, "kageant.log.2: starts at seq", "BROKEN");
    expect("9 rotation: earlier records rotated out", code, 0, out, NULL,
           "earlier records rotated out (anchor of", NULL);
    r[0] = f[2];
    r[1] = f[4];
    code = run_verify(r, 2, &t, -1, 1, NULL, NULL, out);
    expect("9 rotation: a middle generation removed", code, 1, out,
           "records missing between seq",
           "a generation between kageant.log.2 and kageant.log is gone.", NULL);
    code = run_verify(f + 3, 2, &t, -1, 1, NULL, NULL, out);
    expect("9 rotation: the oldest deleted inside the keep count", code, 1, out,
           NULL, "the files of the last 2 rotations must be on disk", NULL);
    tl_free(&t);

    /* the newest rotation expired one generation by age: .1 and the active
     * file are all there may be */
    tl_rotated(&t, 2, 1);
    tl_rotated_files(&t, f);
    code = run_verify(f + 3, 2, &t, -1, 1, NULL, NULL, out);
    expect("9 rotation: a generation expired by age (rotate record)", code, 0,
           out, NULL, "earlier records rotated out", "BROKEN");
    tl_free(&t);
    strbuf_free(out);
}

/* 10: a crash fragment; 11: old lines, an old kageant between two runs */
static void test_crash_and_mixed(void)
{
    struct tlog t;
    struct kitty_auditchain_file f[1];
    struct kitty_auditchain_tail tail;
    strbuf *out = strbuf_new();
    int code, i, off;
    char frag[24];
    const char *cut =
        "ts=2026-11-01T09:00:00Z ev=\"sign\" fp=\"ssh-ed25519 255 SHA";

    for (off = 0; off <= 1; off++) {
        /* line 60 cut mid-value without CRLF; the next start of kageant
         * closes it with CRLF and writes a resumed record for exactly that
         * many bytes (off 1: one byte off - not accounted for) */
        tl_init(&t, 7, "00112233445566778899aabbccddeeff");
        tl_anchor(&t, "start");
        for (i = 2; i < 60; i++)
            tl_rec(&t, "sign", "result", "allowed", (const char *)NULL);
        put_dataz(t.cur, cut);
        kitty_auditchain_tail(t.cur->s, t.cur->len, 1, &tail);
        snprintf(frag, sizeof(frag), "%d", (int)tail.fragment + off);
        put_datapl(t.cur, PTRLEN_LITERAL("\r\n"));
        tl_newrun(&t, 8, "1234567890abcdef1234567890abcdef");
        tl_anchor_ex(&t, "resumed", frag, NULL, NULL, NULL, NULL);
        tl_rec(&t, "agent", "result", "start", (const char *)NULL);
        tl_rec(&t, "agent", "result", "stop", (const char *)NULL);
        tl_anchor(&t, "stop");
        one_file(f, t.cur);
        code = run_verify(f, 1, &t, -1, 1, NULL, NULL, out);
        if (!off)
            expect("10 a crash fragment with its resumed record", code, 0, out,
                   NULL, "kageant.log line 60: incomplete record, kageant ended during the write.",
                   "BROKEN");
        else
            expect("10 a fragment not accounted for", code, 1, out, NULL,
                   "not accounted for", NULL);
        tl_free(&t);
    }

    /* the last line of the active file without CRLF: a warning */
    tl_init(&t, 7, "00112233445566778899aabbccddeeff");
    tl_anchor(&t, "start");
    tl_rec(&t, "sign", "result", "allowed", (const char *)NULL);
    tl_anchor(&t, "stop");
    put_dataz(t.cur, cut);
    one_file(f, t.cur);
    code = run_verify(f, 1, &t, -1, 1, NULL, NULL, out);
    expect("10 the last line cut, kageant not started again", code, 0, out,
           NULL, "kageant.log line 4: incomplete record", "BROKEN");
    tl_free(&t);

    /* 11: 20 lines without the chain in both quoting styles, then the
     * chain, its first prev the hash of line 20 */
    tl_init(&t, 7, "00112233445566778899aabbccddeeff");
    for (i = 0; i < 20; i++) {
        put_dataz(t.cur, (i % 2) ?
                  "ts=2026-10-01T10:00:00Z ev=\"loadkey\" path=\"C:\\\\keys\\\\a.ppk\" result=\"loaded\"" :
                  "ts=2026-10-01T10:00:00Z ev=\"loadkey\" path=\"C:\\keys\\a.ppk\" result=\"loaded\"");
        put_datapl(t.cur, PTRLEN_LITERAL("\r\n"));
    }
    kitty_auditchain_tail(t.cur->s, t.cur->len, 1, &tail);
    snprintf(t.w.prev, sizeof(t.w.prev), "%s", tail.head);
    tl_anchor(&t, "start");
    tl_rec(&t, "agent", "result", "start", (const char *)NULL);
    tl_rec(&t, "agent", "result", "stop", (const char *)NULL);
    tl_anchor(&t, "stop");
    /* an old kageant appends 3 lines, then a new run continues */
    for (i = 0; i < 3; i++) {
        put_dataz(t.cur, "ts=2026-10-02T10:00:00Z ev=\"agent\" result=\"start\"");
        put_datapl(t.cur, PTRLEN_LITERAL("\r\n"));
    }
    kitty_auditchain_tail(t.cur->s, t.cur->len, 1, &tail);
    snprintf(t.w.prev, sizeof(t.w.prev), "%s", tail.head);
    t.w.seq = tail.last_seq + 1;
    tl_newrun(&t, 8, "1234567890abcdef1234567890abcdef");
    tl_anchor(&t, "start");
    tl_rec(&t, "agent", "result", "stop", (const char *)NULL);
    tl_anchor(&t, "stop");
    one_file(f, t.cur);
    code = run_verify(f, 1, &t, -1, 1, NULL, NULL, out);
    expect("11 a mixed file: the records before the chain", code, 0, out, NULL,
           "kageant.log line 1: 20 records before the chain started", "BROKEN");
    expect("11 an old kageant's block between two runs", code, 0, out, NULL,
           "kageant.log lines 25-27: 3 records without chain fields", NULL);
    tl_free(&t);
    strbuf_free(out);
}

/* 13: the signed bytes of values holding quotes and sig= */
static void test_signed_bytes(void)
{
    struct tlog t;
    struct kitty_auditchain_file f[1];
    strbuf *out = strbuf_new();
    int code;
    const char *evil = "x\" sig=\"AAAA\" y=\"? ?\" sig=\"";
    const char *p;

    tl_init(&t, 7, "00112233445566778899aabbccddeeff");
    tl_anchor(&t, "start");
    tl_rec(&t, "add", "comment", evil, (const char *)NULL);
    tl_anchor_ex(&t, "config", NULL, evil, NULL, NULL, NULL);
    tl_anchor(&t, "stop");
    one_file(f, t.cur);
    code = run_verify(f, 1, &t, -1, 1, NULL, NULL, out);
    expect("13 a value with quotes and sig=: signed over the bytes before sig",
           code, 0, out, NULL, "Chain intact", NULL);
    p = strstr(t.cur->s, "ev=\"logchain\" result=\"config\"");
    checks++;
    if (!p || !strstr(p, "value=\"x\"\" sig=\"\"AAAA\"\"")) {
        failures++;
        printf("FAIL 13 the value written with its quotes doubled\n");
    } else {
        printf("ok   13 the value written with its quotes doubled\n");
    }
    tl_free(&t);
    strbuf_free(out);
}

/* the tail kageant reads to continue a log */
static void test_tail(void)
{
    struct kitty_auditchain_tail tail;
    struct tlog t;
    char h[64];
    const char *last;

    tl_init(&t, 7, "00112233445566778899aabbccddeeff");
    tl_anchor(&t, "start");
    tl_rec(&t, "sign", "result", "allowed", (const char *)NULL);
    tl_rotate(&t, 3, 0);
    tl_rec(&t, "sign", "result", "allowed", (const char *)NULL);
    last = strstr(t.cur->s, "\r\n") + 2;
    kitty_auditchain_hash_b64(last, strlen(last) - 2, h);

    kitty_auditchain_tail(t.cur->s, t.cur->len, 1, &tail);
    checks++;
    if (!tail.have_line || strcmp(tail.head, h) || tail.last_seq != 4 ||
        tail.fragment) {
        failures++;
        printf("FAIL tail: the last line, its hash and seq (seq %u)\n",
               (unsigned)tail.last_seq);
    } else {
        printf("ok   tail: the last line, its hash and seq\n");
    }
    put_dataz(t.cur, "ts=2026-11-01T09:00:00Z ev=\"si");
    kitty_auditchain_tail(t.cur->s, t.cur->len, 1, &tail);
    checks++;
    if (!tail.have_line || strcmp(tail.head, h) || tail.fragment != 30) {
        failures++;
        printf("FAIL tail: a fragment after the last line (%u bytes)\n",
               (unsigned)tail.fragment);
    } else {
        printf("ok   tail: a fragment after the last line\n");
    }
    kitty_auditchain_tail(t.cur->s + 5, t.cur->len - 5, 0, &tail);
    checks++;
    if (!tail.have_line || strcmp(tail.head, h)) {
        failures++;
        printf("FAIL tail: a partial tail\n");
    } else {
        printf("ok   tail: a partial tail\n");
    }
    checks++;
    if (kitty_auditchain_first_rotation(t.cur->s, t.cur->len) != 1) {
        failures++;
        printf("FAIL the first rotation of the active file\n");
    } else {
        printf("ok   the first rotation of the active file\n");
    }
    tl_free(&t);
}

static int bench(void)
{
    unsigned char seed[KITTY_AUDITCHAIN_SEEDLEN], sig[KITTY_AUDITCHAIN_SIGLEN];
    char line[200];
    struct ssh_key *k;
    int i;
#ifdef _WIN32
    LARGE_INTEGER fq, t0, t1;
    double ms;
#endif
    memset(seed, 7, sizeof(seed));
    memset(line, 'a', sizeof(line));
    k = kitty_auditchain_key(seed);
#ifdef _WIN32
    QueryPerformanceFrequency(&fq);
    QueryPerformanceCounter(&t0);
#endif
    for (i = 0; i < 1000; i++) {
        line[0] = (char)('a' + i % 26);
        kitty_auditchain_sign(k, line, sizeof(line), sig);
    }
#ifdef _WIN32
    QueryPerformanceCounter(&t1);
    ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)fq.QuadPart;
    printf("1000 Ed25519 signatures: %.1f ms, %.3f ms each\n", ms, ms / 1000.0);
#endif
    ssh_key_free(k);
    return 0;
}

/* ---- --bench-verify: a full-size log ---- */

#ifdef _WIN32
static double bv_ms(void)
{
    LARGE_INTEGER fq, t;
    QueryPerformanceFrequency(&fq);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)fq.QuadPart;
}
#else
static double bv_ms(void) { return 0.0; }
#endif

struct bv_events {
    struct kitty_auditchain_event *e;
    size_t n, size;
};

/* One record in the file and as its event (kept in b) */
static void bv_rec(struct tlog *t, struct bv_events *b, const char *ev,
                   const char **keys, const char **vals, int n)
{
    strbuf *line = strbuf_new();
    char ts[32], seq[24], sig[89], prev[64], **s = NULL;
    unsigned id;
    size_t ns;
    int i, m = n;
    tl_ts(t, ts);
    snprintf(prev, sizeof(prev), "%s", t->w.prev);
    put_fmt(line, "ts=%s ev=", ts);
    kitty_auditchain_put_quoted(line, ev);
    for (i = 0; i < n; i++)
        if (*vals[i]) {
            put_fmt(line, " %s=", keys[i]);
            kitty_auditchain_put_quoted(line, vals[i]);
        }
    kitty_auditchain_seal(&t->w, line, t->key, seq, sig);
    tl_line(t, line);
    strbuf_free(line);
    keys[m] = "seq"; vals[m++] = seq;
    keys[m] = "sig"; vals[m++] = sig;
    keys[m] = "ts"; vals[m++] = ts;
    keys[m] = "prev"; vals[m++] = prev;
    ns = kitty_auditchain_event_strings(ev, keys, vals, m, &id, &s);
    if (ns) {
        struct kitty_auditchain_event *e;
        sgrowarray(b->e, b->size, b->n);
        e = &b->e[b->n++];
        memset(e, 0, sizeof(*e));
        e->log = "the Windows Event Log (KiTTY++_Agent, source KiTTY++_agent)";
        e->record = 100000 + (unsigned long long)b->n;
        e->time = dupstr(ts);
        e->id = id;
        e->s = (const char *const *)s;
        e->n = ns;
    }
}

/* A record of the usual mix: signatures, refusals, adds and removes */
static void bv_some(struct tlog *t, struct bv_events *b, unsigned long k)
{
    const char *keys[24], *vals[24];
    char pid[16], cmt[40];
    snprintf(pid, sizeof(pid), "%lu", 1000 + k % 97);
    snprintf(cmt, sizeof(cmt), "user@host-%lu \"work\" key", k % 13);
    keys[0] = "fp"; vals[0] = "ssh-ed25519 255 SHA256:l9+TL7y3IbXVDDFm6grzb/8NwJglZ4ckgviz7wG+3yQ";
    keys[1] = "comment"; vals[1] = cmt;
    switch (k % 4) {
      case 0: case 1:
        keys[2] = "result"; vals[2] = "allowed";
        keys[3] = "reason"; vals[3] = "";
        keys[4] = "req"; vals[4] = "ssh.exe";
        keys[5] = "reqpath"; vals[5] = "C:\\Windows\\System32\\OpenSSH\\ssh.exe";
        keys[6] = "pid"; vals[6] = pid;
        keys[7] = "dest"; vals[7] = "SHA256:0XQmu5UMO038hgHilnK4Ng0u/xV3d4PeHZZmXNHmFWs build.example (client)";
        keys[8] = "fwd"; vals[8] = "";
        bv_rec(t, b, "sign", keys, vals, 9);
        break;
      case 2:
        keys[0] = "result"; vals[0] = "refused";
        keys[1] = "reason"; vals[1] = "unrecognised message";
        keys[2] = "req"; vals[2] = "pwsh.exe";
        keys[3] = "reqpath"; vals[3] = "C:\\Program Files\\PowerShell\\7\\pwsh.exe";
        keys[4] = "pid"; vals[4] = pid;
        keys[5] = "dest"; vals[5] = "";
        keys[6] = "fwd"; vals[6] = "";
        bv_rec(t, b, "request", keys, vals, 7);
        break;
      default:
        keys[2] = "result"; vals[2] = "done";
        keys[3] = "reason"; vals[3] = "ipc";
        keys[4] = "req"; vals[4] = "ssh-add.exe";
        keys[5] = "reqpath"; vals[5] = "C:\\Windows\\System32\\OpenSSH\\ssh-add.exe";
        keys[6] = "pid"; vals[6] = pid;
        keys[7] = "path"; vals[7] = "";
        keys[8] = "restrict"; vals[8] = "";
        bv_rec(t, b, "add", keys, vals, 9);
        break;
    }
}

static int bench_verify(int mb, int nruns, int full)
{
    struct tlog t, x;
    struct bv_events b, bx;
    struct kitty_auditchain_file f[MAXF + 1];
    struct kitty_auditchain_anchor anch[2 * MAXEV];
    struct kitty_auditchain_event *all;
    strbuf *out = strbuf_new();
    size_t total = 0, na = 0, nf = 0, i, lines = 0, perrun;
    unsigned long k = 0;
    int run, lastclk = 0, code = 0, r;
    double t0, t1;
    static const char *const runs[5] = {
        "00112233445566778899aabbccddeef0", "00112233445566778899aabbccddeef1",
        "00112233445566778899aabbccddeef2", "00112233445566778899aabbccddeef3",
        "00112233445566778899aabbccddeef4",
    };
    /* 4 files, the last nearly full: the default 20 MB is agentlogmaxkb's
     * default 5120 KB a file */
    const size_t want = (size_t)mb * 1024 * 1024 - (size_t)mb * 16 * 1024;
    const size_t cap = (size_t)mb * 1024 * 1024 / 4;

    memset(&b, 0, sizeof(b));
    memset(&bx, 0, sizeof(bx));
    t0 = bv_ms();
    tl_init(&t, 7, runs[0]);
    /* about 415 bytes a line (measured): the records each run writes */
    perrun = want / 415 / 5;
    for (run = 0; run < 5; run++) {
        size_t n;
        if (run)
            tl_newrun(&t, (unsigned char)(7 + run), runs[run]);
        tl_anchor(&t, "start");
        lastclk = t.clock;
        for (n = 0; n < perrun; n++, k++) {
            if (t.cur->len >= cap && t.ngens < 3) {
                tl_rotate(&t, 3, 0);
                lastclk = t.clock;
            }
            bv_some(&t, &b, k);
            if (t.clock - lastclk >= 900) {          /* 15 minutes */
                tl_anchor(&t, "head");
                lastclk = t.clock;
            }
        }
        tl_anchor(&t, "stop");
    }
    /* another log file of this computer, its seq the same as this one's,
     * written at the same time: its events are in the same log */
    tl_init(&x, 99, "ffeeddccbbaa99887766554433221100");
    x.path = "C:\\Users\\other\\kageant.log";
    tl_anchor(&x, "start");
    for (i = 0; i < 2000; i++)
        bv_some(&x, &bx, (unsigned long)i * 7 + 3);
    tl_anchor(&x, "stop");
    t1 = bv_ms();

    for (i = 0; i < (size_t)t.ngens; i++) {
        static const char *const nm[] = { "kageant.log.3", "kageant.log.2",
                                          "kageant.log.1" };
        f[nf].name = nm[(size_t)(3 - t.ngens) + i];
        f[nf].data = t.gens[i]->s;
        f[nf].len = t.gens[i]->len;
        total += t.gens[i]->len;
        nf++;
    }
    f[nf].name = "kageant.log";
    f[nf].data = t.cur->s;
    f[nf].len = t.cur->len;
    total += t.cur->len;
    nf++;
    for (i = 0; i < nf; i++) {
        size_t j;
        for (j = 0; j + 1 < f[i].len; j++)
            if (f[i].data[j] == '\r' && f[i].data[j + 1] == '\n')
                lines++;
    }
    for (i = 0; i < (size_t)t.nev; i++)
        anch[na++] = t.ev[i];
    for (i = 0; i < (size_t)x.nev && na < lenof(anch); i++)
        anch[na++] = x.ev[i];
    all = snewn(b.n + bx.n, struct kitty_auditchain_event);
    memcpy(all, b.e, b.n * sizeof(*all));
    memcpy(all + b.n, bx.e, bx.n * sizeof(*all));
    printf("bench-verify: %u files, %.1f MB, %u lines, %u anchors, %u events "
           "of the log + %u of another log file; built in %.1f s\n",
           (unsigned)nf, (double)total / 1048576.0, (unsigned)lines,
           (unsigned)na, (unsigned)b.n, (unsigned)bx.n, (t1 - t0) / 1000.0);

    for (r = 0; r < nruns; r++) {
        double a0, a1, a2;
        int c1, c2;
        a0 = bv_ms();
        c1 = run_verify_ev(f, nf, anch, na, NULL, 0, out);
        a1 = bv_ms();
        c2 = run_verify_ev(f, nf, anch, na, all, b.n + bx.n, out);
        a2 = bv_ms();
        printf("run %d: files and anchors only %.1f s (exit %d); full verify, "
               "every event too %.1f s (exit %d)\n", r + 1,
               (a1 - a0) / 1000.0, c1, (a2 - a1) / 1000.0, c2);
        if (c1 || c2)
            code = 1;
    }
    if (full) {
        double a0, a1;
        int c3;
        g_full = 1;
        a0 = bv_ms();
        c3 = run_verify_ev(f, nf, anch, na, all, b.n + bx.n, out);
        a1 = bv_ms();
        g_full = 0;
        printf("full: every line's signature on its own %.1f s (exit %d)\n",
               (a1 - a0) / 1000.0, c3);
        if (c3)
            code = 1;
    }
    if (code)
        printf("--- the last report:\n%s", out->s);
    else {
        const char *last = out->s, *p;
        while ((p = strstr(last, "\r\n")) != NULL && p[2])
            last = p + 2;
        printf("verdict: %s", last);
    }

    for (i = 0; i < b.n; i++) {
        size_t j;
        for (j = 0; j < b.e[i].n; j++)
            sfree((char *)b.e[i].s[j]);
        sfree((char **)b.e[i].s);
        sfree((char *)b.e[i].time);
    }
    for (i = 0; i < bx.n; i++) {
        size_t j;
        for (j = 0; j < bx.e[i].n; j++)
            sfree((char *)bx.e[i].s[j]);
        sfree((char **)bx.e[i].s);
        sfree((char *)bx.e[i].time);
    }
    sfree(b.e);
    sfree(bx.e);
    sfree(all);
    tl_free(&x);
    tl_free(&t);
    strbuf_free(out);
    return code;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--bench"))
        return bench();
    if (argc > 1 && !strcmp(argv[1], "--bench-verify"))
        return bench_verify(argc > 2 && atoi(argv[2]) > 0 ? atoi(argv[2]) : 20,
                            argc > 3 && atoi(argv[3]) > 0 ? atoi(argv[3]) : 3,
                            argc > 4 && !strcmp(argv[4], "full"));
    test_intact_and_tamper();
    test_other_keys();
    test_line_signatures();
    test_rotation();
    test_crash_and_mixed();
    test_signed_bytes();
    test_tail();
    test_repeat();
    test_event_copies();
    printf("%d of %d checks failed\n", failures, checks);
    return failures ? 1 : 0;
}
