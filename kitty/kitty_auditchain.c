/*
 * kitty_auditchain.c - the agent log's chain: building the chain fields of
 * a line, reading the end of an existing log, the run keys and the
 * verifier. See kitty_auditchain.h for the format.
 *
 * Plain C over PuTTY's crypto and utils, no Win32: test/test_auditchain.c
 * links this file alone.
 */
#include <stdio.h>
#include <string.h>

#include "putty.h"
#include "ssh.h"
#include "mpint.h"

#include "kitty_auditchain.h"
#include "kitty_text.h"
#define KITTY_EVENTIDS_TABLE        /* the field list of every agent ID */
#include "kitty_eventids.h"

const char *const kitty_auditchain_fieldnames[KAC_NFIELDS] = {
    "result", "run", "logkey", "file", "fragment", "value",
    "rotation", "keep", "expired",
    "ts", "prev", "seq", "sig",
};

/* ---- small helpers ---- */

/* Decimal without the C library's 64-bit printf, which the 32-bit build's
 * msvcrt does not provide on old Windows. buf holds 24 bytes. */
static void kac_u64(char *buf, unsigned long long v)
{
    char tmp[24];
    int n = 0, i = 0;
    do {
        tmp[n++] = (char)('0' + (int)(v % 10));
        v /= 10;
    } while (v && n < 23);
    while (n > 0)
        buf[i++] = tmp[--n];
    buf[i] = '\0';
}

/* A decimal value; 0 for anything else (no seq is 0). */
static unsigned long long kac_parse_u64(const char *s)
{
    unsigned long long v = 0;
    if (!s || !*s)
        return 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || v > 1844674407370955160ULL)
            return 0;
        v = v * 10 + (unsigned)(*s - '0');
    }
    return v;
}

void kitty_auditchain_b64(const unsigned char *p, size_t n, char *out)
{
    size_t i;
    for (i = 0; i < n; i += 3) {
        base64_encode_atom(p + i, (int)(n - i < 3 ? n - i : 3), out);
        out += 4;
    }
    *out = '\0';
}

/* base64 text to exactly n bytes; 0 when it is not that. */
static int kac_unb64(const char *s, unsigned char *out, size_t n)
{
    strbuf *sb;
    int ok;
    if (!s || strlen(s) != 4 * ((n + 2) / 3))
        return 0;
    sb = base64_decode_sb(ptrlen_from_asciz(s));
    ok = sb->len == n;
    if (ok)
        memcpy(out, sb->u, n);
    strbuf_free(sb);
    return ok;
}

void kitty_auditchain_hash_b64(const char *line, size_t len, char *out)
{
    unsigned char h[KITTY_AUDITCHAIN_HASHLEN];
    hash_simple(&ssh_sha256, make_ptrlen(line, len), h);
    kitty_auditchain_b64(h, sizeof(h), out);
    smemclr(h, sizeof(h));
}

void kitty_auditchain_put_quoted(strbuf *sb, const char *v)
{
    put_byte(sb, '"');
    for (; *v; v++) {
        unsigned char c = (unsigned char)*v;
        if (c == '"') {
            put_byte(sb, '"');
            put_byte(sb, '"');
        } else if (c < 0x20) {
            put_byte(sb, '?');      /* CR, LF and every other control byte */
        } else {
            put_byte(sb, c);
        }
    }
    put_byte(sb, '"');
}

void kitty_auditchain_anchor_body(strbuf *sb, const char *ts,
                                  const char *const v[KAC_NCALLER])
{
    int i;
    put_fmt(sb, "ts=%s ev=", ts);
    kitty_auditchain_put_quoted(sb, "logchain");
    for (i = 0; i < KAC_NCALLER; i++)
        if (v[i] && *v[i]) {
            put_byte(sb, ' ');
            put_dataz(sb, kitty_auditchain_fieldnames[i]);
            put_byte(sb, '=');
            kitty_auditchain_put_quoted(sb, v[i]);
        }
}

/* ---- keys ---- */

struct ssh_key *kitty_auditchain_key(
    const unsigned char seed[KITTY_AUDITCHAIN_SEEDLEN])
{
    struct eddsa_key *ek = snew(struct eddsa_key);
    if (!ec_ed_alg_and_curve_by_bits(255, &ek->curve, &ek->sshk.vt)) {
        sfree(ek);
        return NULL;
    }
    ek->privateKey = mp_from_bytes_le(make_ptrlen(seed,
                                                  KITTY_AUDITCHAIN_SEEDLEN));
    ek->publicKey = eddsa_public(ek->privateKey, ek->sshk.vt);
    return &ek->sshk;
}

/* The 32-byte point of a public key blob (string type, string point). */
static int kac_blob_point(ptrlen blob, unsigned char *out, size_t n)
{
    BinarySource src[1];
    ptrlen pt;
    BinarySource_BARE_INIT_PL(src, blob);
    get_string(src);
    pt = get_string(src);
    if (get_err(src) || pt.len != n)
        return 0;
    memcpy(out, pt.ptr, n);
    return 1;
}

int kitty_auditchain_public(const unsigned char seed[KITTY_AUDITCHAIN_SEEDLEN],
                            unsigned char pub[KITTY_AUDITCHAIN_PUBLEN])
{
    struct ssh_key *k = kitty_auditchain_key(seed);
    strbuf *blob;
    int ok;
    if (!k)
        return 0;
    blob = strbuf_new();
    ssh_key_public_blob(k, BinarySink_UPCAST(blob));
    ok = kac_blob_point(ptrlen_from_strbuf(blob), pub, KITTY_AUDITCHAIN_PUBLEN);
    strbuf_free(blob);
    ssh_key_free(k);
    return ok;
}

void kitty_auditchain_sign(struct ssh_key *key, const void *data, size_t len,
                           unsigned char sig[KITTY_AUDITCHAIN_SIGLEN])
{
    strbuf *sb = strbuf_new();
    ssh_key_sign(key, make_ptrlen(data, len), 0, BinarySink_UPCAST(sb));
    if (!kac_blob_point(ptrlen_from_strbuf(sb), sig, KITTY_AUDITCHAIN_SIGLEN))
        memset(sig, 0, KITTY_AUDITCHAIN_SIGLEN);
    strbuf_free(sb);
}

int kitty_auditchain_verify_sig(const unsigned char pub[KITTY_AUDITCHAIN_PUBLEN],
                                const void *data, size_t len,
                                const unsigned char sig[KITTY_AUDITCHAIN_SIGLEN])
{
    strbuf *pb = strbuf_new(), *sb = strbuf_new();
    ssh_key *k;
    int ok = 0;
    put_stringz(pb, ssh_ecdsa_ed25519.ssh_id);
    put_string(pb, pub, KITTY_AUDITCHAIN_PUBLEN);
    k = ssh_key_new_pub(&ssh_ecdsa_ed25519, ptrlen_from_strbuf(pb));
    if (k) {
        put_stringz(sb, ssh_ecdsa_ed25519.ssh_id);
        put_string(sb, sig, KITTY_AUDITCHAIN_SIGLEN);
        ok = ssh_key_verify(k, ptrlen_from_strbuf(sb), make_ptrlen(data, len));
        ssh_key_free(k);
    }
    strbuf_free(pb);
    strbuf_free(sb);
    return ok;
}

/* ---- the fields of one line ---- */

/*
 * Walk the fields of a line: ts= (bare, to the next space), then
 * key="value" pairs, one space before each. cb gets the key, the value
 * unquoted ("" is one quote) and the offset of the key in the line. Returns
 * 0 when the line does not have this form throughout.
 */
typedef void (*kac_field_cb)(void *ctx, const char *key, size_t klen,
                             const char *val, size_t keyoff);

static int kac_fields(const char *p, size_t len, kac_field_cb cb, void *ctx)
{
    size_t i = 0;
    strbuf *v = strbuf_new();
    int ok = 1;
    while (i < len && ok) {
        size_t ks = i, kl;
        if (i > 0) {
            if (p[i] != ' ') { ok = 0; break; }
            ks = ++i;
        }
        while (i < len && p[i] != '=' && p[i] != ' ' && p[i] != '"')
            i++;
        kl = i - ks;
        if (!kl || i >= len || p[i] != '=') { ok = 0; break; }
        i++;
        strbuf_clear(v);
        if (i < len && p[i] == '"') {
            int closed = 0;
            i++;
            while (i < len) {
                if (p[i] == '"') {
                    if (i + 1 < len && p[i + 1] == '"') {
                        put_byte(v, '"');
                        i += 2;
                        continue;
                    }
                    i++;
                    closed = 1;
                    break;
                }
                put_byte(v, p[i]);
                i++;
            }
            if (!closed) { ok = 0; break; }
        } else {
            while (i < len && p[i] != ' ')
                put_byte(v, p[i++]);
        }
        cb(ctx, p + ks, kl, v->s, ks);
    }
    strbuf_free(v);
    return ok;
}

struct kac_line {
    const char *p;
    size_t len;                      /* without CRLF */
    size_t file;                     /* index into the files */
    size_t lineno;                   /* 1-based, in its file */
    int bad_eol;                     /* holds a bare LF */
    int complete;                    /* ended with CRLF */
    int wellformed;
    int chained;                     /* seq and prev */
    unsigned long long seq;
    char prev[64];
    int has_sig;
    size_t signed_len;
    char ts[32];
    int is_anchor;
    char *f[KAC_NCALLER];            /* an anchor's fields (sfree) */
    char hash[KITTY_AUDITCHAIN_B64HASH + 1];
    int fragment;                    /* accounted for by a resumed record */
    int runidx;                      /* its run (c->runs), -1 none */
    unsigned char sig[KITTY_AUDITCHAIN_SIGLEN];
    int sig_form;                    /* sig decodes to 64 bytes */
    int sig_ok;                      /* its signature fits, itself or bound
                                      * by a later one (kac_presign) */
};

static void kac_line_cb(void *ctx, const char *key, size_t klen,
                        const char *val, size_t keyoff)
{
    struct kac_line *l = (struct kac_line *)ctx;
    int i;
#define KEYIS(s) (klen == strlen(s) && !memcmp(key, s, klen))
    if (KEYIS("ts")) {
        snprintf(l->ts, sizeof(l->ts), "%s", val);
    } else if (KEYIS("ev")) {
        l->is_anchor = !strcmp(val, "logchain");
    } else if (KEYIS("seq")) {
        l->seq = kac_parse_u64(val);
    } else if (KEYIS("prev")) {
        snprintf(l->prev, sizeof(l->prev), "%s", val);
    } else if (KEYIS("sig")) {
        l->has_sig = 1;
        l->signed_len = keyoff ? keyoff - 1 : 0;
        l->sig_form = kac_unb64(val, l->sig, KITTY_AUDITCHAIN_SIGLEN);
    } else {
        for (i = 0; i < KAC_NCALLER; i++)
            if (KEYIS(kitty_auditchain_fieldnames[i]) && !l->f[i])
                l->f[i] = dupstr(val);
    }
#undef KEYIS
}

static void kac_parse_line(struct kac_line *l)
{
    l->prev[0] = '\0';
    l->wellformed = l->len > 3 && !memcmp(l->p, "ts=", 3) &&
        kac_fields(l->p, l->len, kac_line_cb, l);
    l->chained = l->wellformed && l->seq > 0 && l->prev[0];
    if (!l->chained)
        l->is_anchor = 0;
    kitty_auditchain_hash_b64(l->p, l->len, l->hash);
}

static void kac_free_line(struct kac_line *l)
{
    int i;
    for (i = 0; i < KAC_NCALLER; i++)
        sfree(l->f[i]);
}

/* ---- the writer's side ---- */

void kitty_auditchain_tail(const char *data, size_t len, int whole,
                           struct kitty_auditchain_tail *t)
{
    size_t start = 0, end, i, ls;
    memset(t, 0, sizeof(*t));
    if (!whole) {                    /* the first line may be cut: skip it */
        for (i = 0; i + 1 < len; i++)
            if (data[i] == '\r' && data[i + 1] == '\n')
                break;
        if (i + 1 >= len) {
            t->need_more = 1;
            return;
        }
        start = i + 2;
    }
    /* the last CRLF: after it, a fragment */
    end = len;
    for (i = len; i >= start + 2; i--)
        if (data[i - 2] == '\r' && data[i - 1] == '\n') {
            end = i;
            break;
        }
    if (end == len && !(len >= start + 2 && data[len - 2] == '\r' &&
                        data[len - 1] == '\n')) {
        /* no CRLF at all from start on */
        if (!whole) {
            t->need_more = 1;
            return;
        }
        t->fragment = len - start;
        return;
    }
    t->fragment = len - end;
    /* the last complete line: [ls, end - 2) */
    ls = start;
    for (i = end - 2; i >= start + 2; i--)
        if (data[i - 2] == '\r' && data[i - 1] == '\n') {
            ls = i;
            break;
        }
    if (end - 2 > ls || end - 2 == ls) {
        t->have_line = 1;
        kitty_auditchain_hash_b64(data + ls, end - 2 - ls, t->head);
    }
    /* the newest seq: from the last complete line back */
    {
        size_t le = end - 2;
        for (;;) {
            struct kac_line l;
            size_t lb = start;
            for (i = le; i >= start + 2; i--)
                if (data[i - 2] == '\r' && data[i - 1] == '\n') {
                    lb = i;
                    break;
                }
            memset(&l, 0, sizeof(l));
            l.p = data + lb;
            l.len = le - lb;
            kac_parse_line(&l);
            kac_free_line(&l);
            if (l.chained) {
                t->last_seq = l.seq;
                break;
            }
            if (lb <= start + 2)
                break;
            le = lb - 2;
        }
    }
}

unsigned kitty_auditchain_first_rotation(const char *data, size_t len)
{
    struct kac_line l;
    size_t i;
    unsigned r = 0;
    for (i = 0; i + 1 < len; i++)
        if (data[i] == '\r' && data[i + 1] == '\n')
            break;
    if (i + 1 >= len)
        return 0;
    memset(&l, 0, sizeof(l));
    l.p = data;
    l.len = i;
    kac_parse_line(&l);
    if (l.is_anchor && l.f[KAC_RESULT] && !strcmp(l.f[KAC_RESULT], "rotate"))
        r = (unsigned)kac_parse_u64(l.f[KAC_ROTATION]);
    kac_free_line(&l);
    return r;
}

void kitty_auditchain_seal(const struct kitty_auditchain_writer *w,
                           strbuf *line, struct ssh_key *key,
                           char seq_out[24], char sig_out[89])
{
    char seq[24];
    kac_u64(seq, w->seq);
    put_datapl(line, PTRLEN_LITERAL(" seq="));
    kitty_auditchain_put_quoted(line, seq);
    put_datapl(line, PTRLEN_LITERAL(" prev="));
    kitty_auditchain_put_quoted(line, w->prev[0] ? w->prev : "none");
    if (seq_out)
        memcpy(seq_out, seq, sizeof(seq));
    if (sig_out)
        sig_out[0] = '\0';
    if (key) {
        unsigned char sig[KITTY_AUDITCHAIN_SIGLEN];
        char b64[89];
        kitty_auditchain_sign(key, line->s, line->len, sig);
        kitty_auditchain_b64(sig, sizeof(sig), b64);
        put_datapl(line, PTRLEN_LITERAL(" sig="));
        kitty_auditchain_put_quoted(line, b64);
        if (sig_out)
            memcpy(sig_out, b64, sizeof(b64));
    }
}

void kitty_auditchain_commit(struct kitty_auditchain_writer *w,
                             const char *line, size_t len)
{
    kitty_auditchain_hash_b64(line, len, w->prev);
    w->seq++;
}

/* ---- the Windows Event Log copy of a record ---- */

#define KAC_MAXEVF 40                /* fields of one ID, far above any */

/* The row of an agent ID: its kind and its field names (into *buf, a
 * copy of the row's list: sfree it). */
static int kac_id_row(unsigned id, const char **kind, char **buf,
                      const char *names[KAC_MAXEVF], size_t *nf)
{
    size_t i;
    char *p;
    for (i = 0; i < lenof(kitty_eventids); i++)
        if (kitty_eventids[i].id == id &&
            kitty_eventids[i].block == KITTY_EVB_AGENT &&
            kitty_eventids[i].kind)
            break;
    if (i == lenof(kitty_eventids))
        return 0;
    *kind = kitty_eventids[i].kind;
    *buf = dupstr(kitty_eventids[i].fields);
    *nf = 0;
    for (p = strtok(*buf, " "); p && *nf < KAC_MAXEVF; p = strtok(NULL, " "))
        names[(*nf)++] = p;
    return 1;
}

static int kac_kind_id(const char *ev, unsigned *id)
{
    size_t i;
    for (i = 0; i < lenof(kitty_eventids); i++)
        if (kitty_eventids[i].block == KITTY_EVB_AGENT &&
            kitty_eventids[i].kind && !strcmp(kitty_eventids[i].kind, ev)) {
            *id = kitty_eventids[i].id;
            return 1;
        }
    return 0;
}

static int kac_is_chain_field(const char *f)
{
    return !strcmp(f, "seq") || !strcmp(f, "sig") || !strcmp(f, "ts") ||
        !strcmp(f, "prev");
}

int kitty_auditchain_event_line(unsigned id, const char *const *s, size_t n,
                                strbuf *line, const char **sig,
                                unsigned long long *seq)
{
    const char *names[KAC_MAXEVF], *kind;
    const char *ts = NULL, *prev = NULL, *sq = NULL, *sg = NULL;
    char *buf = NULL;
    size_t nf, i;
    if (!kac_id_row(id, &kind, &buf, names, &nf) ||
        n != 1 + nf || !s[0] || strcmp(s[0], kind)) {
        sfree(buf);
        return 0;
    }
    for (i = 0; i < nf; i++) {
        const char *v = s[1 + i] ? s[1 + i] : "";
        if (!strcmp(names[i], "ts"))
            ts = v;
        else if (!strcmp(names[i], "prev"))
            prev = v;
        else if (!strcmp(names[i], "seq"))
            sq = v;
        else if (!strcmp(names[i], "sig"))
            sg = v;
    }
    if (!ts || !prev || !sq || !sg) {
        sfree(buf);
        return 0;
    }
    put_fmt(line, "ts=%s ev=", ts);
    kitty_auditchain_put_quoted(line, s[0]);
    for (i = 0; i < nf; i++)
        if (!kac_is_chain_field(names[i]) && s[1 + i] && *s[1 + i]) {
            put_byte(line, ' ');
            put_dataz(line, names[i]);
            put_byte(line, '=');
            kitty_auditchain_put_quoted(line, s[1 + i]);
        }
    put_datapl(line, PTRLEN_LITERAL(" seq="));
    kitty_auditchain_put_quoted(line, sq);
    put_datapl(line, PTRLEN_LITERAL(" prev="));
    kitty_auditchain_put_quoted(line, prev);
    if (sig)
        *sig = sg;
    if (seq)
        *seq = kac_parse_u64(sq);
    sfree(buf);
    return 1;
}

size_t kitty_auditchain_event_strings(const char *ev,
                                      const char *const *keys,
                                      const char *const *vals, int n,
                                      unsigned *id, char ***out)
{
    const char *names[KAC_MAXEVF], *kind;
    char *buf = NULL, **s, *p;
    size_t nf, i;
    int j;
    if (!kac_kind_id(ev, id) ||
        !kac_id_row(*id, &kind, &buf, names, &nf)) {
        sfree(buf);
        return 0;
    }
    s = snewn(1 + nf, char *);
    s[0] = dupstr(ev);
    for (i = 0; i < nf; i++) {
        const char *v = "";
        for (j = 0; j < n; j++)
            if (!strcmp(keys[j], names[i])) {
                v = vals[j];
                break;
            }
        s[1 + i] = dupstr(v);
    }
    for (i = 0; i < 1 + nf; i++)
        for (p = s[i]; *p; p++)
            if ((unsigned char)*p < 0x20)
                *p = '?';
    sfree(buf);
    *out = s;
    return 1 + nf;
}

void kitty_auditchain_anchor_from_event(const char *const *s, size_t n,
                                        const char *v[KAC_NFIELDS])
{
    const char *names[KAC_MAXEVF], *kind;
    char *buf = NULL;
    size_t nf, i;
    int k;
    memset((void *)v, 0, KAC_NFIELDS * sizeof(*v));
    if (!kac_id_row(171, &kind, &buf, names, &nf)) {
        sfree(buf);
        return;
    }
    for (i = 0; i < nf && 1 + i < n; i++)
        for (k = 0; k < KAC_NFIELDS; k++)
            if (!strcmp(names[i], kitty_auditchain_fieldnames[k]))
                v[k] = s[1 + i];
    sfree(buf);
}

/* ---- "previous record repeated N times" ---- */

int kitty_auditchain_repeat_flush(struct kitty_auditchain_repeat *r,
                                  unsigned long now_ms, unsigned long ms,
                                  int force,
                                  struct kitty_auditchain_summary *s)
{
    if (!r->count || (!force && now_ms - r->first_ms < ms))
        return 0;
    kac_u64(s->count, r->count);
    snprintf(s->first, sizeof(s->first), "%s", r->first);
    snprintf(s->last, sizeof(s->last), "%s", r->last);
    snprintf(s->of, sizeof(s->of), "%s", r->of);
    r->count = 0;
    return 1;
}

int kitty_auditchain_repeat_record(struct kitty_auditchain_repeat *r,
                                   const char *body, const char *ts,
                                   unsigned long now_ms, unsigned long ms,
                                   struct kitty_auditchain_summary *s)
{
    int act = 0;
    if (kitty_auditchain_repeat_flush(r, now_ms, ms, 0, s))
        act |= KAC_REPEAT_SUMMARY;   /* the first repeat is old enough */
    if (r->body && !strcmp(r->body, body)) {
        if (!r->count++) {
            snprintf(r->first, sizeof(r->first), "%s", ts);
            r->first_ms = now_ms;
        }
        snprintf(r->last, sizeof(r->last), "%s", ts);
        return act;
    }
    if (!(act & KAC_REPEAT_SUMMARY) &&
        kitty_auditchain_repeat_flush(r, now_ms, ms, 1, s))
        act |= KAC_REPEAT_SUMMARY;   /* a different record: before it */
    return act | KAC_REPEAT_WRITE;
}

void kitty_auditchain_repeat_written(struct kitty_auditchain_repeat *r,
                                     const char *body, const char *seq)
{
    sfree(r->body);
    r->body = dupstr(body);
    snprintf(r->of, sizeof(r->of), "%s", seq ? seq : "");
    r->count = 0;
}

void kitty_auditchain_repeat_forget(struct kitty_auditchain_repeat *r)
{
    sfree(r->body);
    memset(r, 0, sizeof(*r));
}

/* ---- the verifier ---- */

struct kac_run {
    char id[KITTY_AUDITCHAIN_RUNLEN + 8];
    unsigned char pub[KITTY_AUDITCHAIN_PUBLEN];
    int have_pub;
    char start_ts[32];
    int has_stop;
    char stop_ts[32];
    unsigned long long last_anchor, last_seq;
    int confirmed;
    int keydiff;
};

struct kac_rot {
    unsigned rotation, keep, expired;
    unsigned long long seq;
};

struct kac_ctx {
    const struct kitty_auditchain_verify_in *in;
    struct kac_line *l;
    size_t n;
    struct kac_run *runs;
    size_t nruns, runsize;
    struct kac_rot *rots;
    size_t nrots, rotsize;
    strbuf *probs, *warns, *cannot;
    int nprobs, ncannot;
    size_t *byseq;                   /* the chained lines by seq */
    size_t nbyseq;
    char *firstat;                   /* where the first problem is */
};

/* The problems listed one by one; the ones after are counted. A file
 * rewritten from one line on breaks the signature of every line after it. */
#define KAC_MAX_LISTED 100

static void kac_problem(struct kac_ctx *c, const char *at, char *text)
{
    if (!c->nprobs && at)
        c->firstat = dupstr(at);
    if (c->nprobs < KAC_MAX_LISTED) {
        put_dataz(c->probs, text);
        put_datapl(c->probs, PTRLEN_LITERAL("\r\n"));
    }
    c->nprobs++;
    sfree(text);
}

static void kac_note(strbuf *sb, char *text)
{
    put_dataz(sb, text);
    put_datapl(sb, PTRLEN_LITERAL("\r\n"));
    sfree(text);
}

static const char *kac_fname(struct kac_ctx *c, const struct kac_line *l)
{
    return c->in->files[l->file].name;
}

/* "kageant.log line 912" */
static char *kac_at(struct kac_ctx *c, const struct kac_line *l)
{
    char n[24];
    kac_u64(n, l->lineno);
    return dupprintf(KT_KAC_AT_FMT, kac_fname(c, l), n);
}

static struct kac_run *kac_find_run(struct kac_ctx *c, const char *id)
{
    size_t i;
    if (!id)
        return NULL;
    for (i = 0; i < c->nruns; i++)
        if (!strcmp(c->runs[i].id, id))
            return &c->runs[i];
    return NULL;
}

static struct kac_ctx *kac_sort_ctx;   /* for the qsort of kac_index */

static int kac_byseq_cmp(const void *a, const void *b)
{
    unsigned long long x = kac_sort_ctx->l[*(const size_t *)a].seq;
    unsigned long long y = kac_sort_ctx->l[*(const size_t *)b].seq;
    if (x != y)
        return x < y ? -1 : 1;
    return *(const size_t *)a < *(const size_t *)b ? -1 : 1;
}

/* The chained lines by seq, once the fragments are marked: a full log has
 * tens of thousands of lines and every event looks one up. */
static void kac_index(struct kac_ctx *c)
{
    size_t i;
    sfree(c->byseq);
    c->byseq = snewn(c->n ? c->n : 1, size_t);
    c->nbyseq = 0;
    for (i = 0; i < c->n; i++)
        if (c->l[i].chained && !c->l[i].fragment)
            c->byseq[c->nbyseq++] = i;
    kac_sort_ctx = c;
    qsort(c->byseq, c->nbyseq, sizeof(size_t), kac_byseq_cmp);
}

/* The first line of a seq (in file order) */
static struct kac_line *kac_find_seq(struct kac_ctx *c, unsigned long long s)
{
    size_t lo = 0, hi = c->nbyseq;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (c->l[c->byseq[mid]].seq < s)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < c->nbyseq && c->l[c->byseq[lo]].seq == s)
        return &c->l[c->byseq[lo]];
    return NULL;
}

typedef unsigned char kac_pub[KITTY_AUDITCHAIN_PUBLEN];

/* The signatures of the lines (see kitty_auditchain_verify in the
 * header): each line's key is its own logkey for an anchor, else the key
 * of the run of the last anchor before it (the first key seen for that
 * run, as kac_walk takes it). A stretch ends where the next line is not
 * chained to it, has no signature or another key. Its last line is
 * checked. When that fits, the whole stretch is bound by it. When not,
 * every line of it is checked on its own. */
static void kac_presign(struct kac_ctx *c)
{
    struct { char id[KITTY_AUDITCHAIN_RUNLEN + 8];
             unsigned char pub[KITTY_AUDITCHAIN_PUBLEN]; int have; } *rk = NULL;
    size_t nrk = 0, rksize = 0, *idx, m = 0, i, k, s;
    kac_pub *key;
    int *have, cur = -1;

    idx = snewn(c->n ? c->n : 1, size_t);
    key = snewn(c->n ? c->n : 1, kac_pub);
    have = snewn(c->n ? c->n : 1, int);
    for (i = 0; i < c->n; i++) {
        struct kac_line *l = &c->l[i];
        if (!l->chained || l->fragment || !l->complete || l->bad_eol)
            continue;
        have[m] = 0;
        if (l->is_anchor) {
            const char *rid = l->f[KAC_RUN];
            unsigned char pub[KITTY_AUDITCHAIN_PUBLEN];
            int hp = kac_unb64(l->f[KAC_LOGKEY], pub, sizeof(pub));
            if (hp) {
                memcpy(key[m], pub, sizeof(pub));
                have[m] = 1;
            }
            if (rid) {
                for (k = 0; k < nrk; k++)
                    if (!strcmp(rk[k].id, rid))
                        break;
                if (k == nrk) {
                    sgrowarray(rk, rksize, nrk);
                    snprintf(rk[nrk].id, sizeof(rk[nrk].id), "%s", rid);
                    rk[nrk].have = hp;
                    if (hp)
                        memcpy(rk[nrk].pub, pub, sizeof(pub));
                    nrk++;
                }
                cur = (int)k;
            }
        } else if (cur >= 0 && rk[cur].have) {
            memcpy(key[m], rk[cur].pub, KITTY_AUDITCHAIN_PUBLEN);
            have[m] = 1;
        }
        idx[m++] = i;
    }
    for (s = 0, k = 0; k < m; k++) {
        struct kac_line *l = &c->l[idx[k]], *nx = k + 1 < m ? &c->l[idx[k + 1]] : NULL;
        int end = c->in->full || !nx || !have[k] || !l->has_sig ||
            !l->sig_form || !have[k + 1] || !nx->has_sig ||
            memcmp(key[k], key[k + 1], KITTY_AUDITCHAIN_PUBLEN) ||
            strcmp(nx->prev, l->hash);
        size_t j;
        if (!end)
            continue;
        if (have[k] && l->has_sig && l->sig_form &&
            kitty_auditchain_verify_sig(key[k], l->p, l->signed_len, l->sig)) {
            for (j = s; j <= k; j++)
                c->l[idx[j]].sig_ok = 1;
        } else {
            for (j = s; j < k; j++) {
                struct kac_line *b = &c->l[idx[j]];
                b->sig_ok = have[j] && b->has_sig && b->sig_form &&
                    kitty_auditchain_verify_sig(key[j], b->p, b->signed_len,
                                                b->sig);
            }
        }
        s = k + 1;
    }
    sfree(rk);
    sfree(idx);
    sfree(key);
    sfree(have);
}

static void kac_add_rot(struct kac_ctx *c, unsigned rotation, unsigned keep,
                        unsigned expired, unsigned long long seq)
{
    size_t i;
    if (!rotation)
        return;
    for (i = 0; i < c->nrots; i++)
        if (c->rots[i].rotation == rotation)
            return;
    sgrowarray(c->rots, c->rotsize, c->nrots);
    c->rots[c->nrots].rotation = rotation;
    c->rots[c->nrots].keep = keep;
    c->rots[c->nrots].expired = expired;
    c->rots[c->nrots].seq = seq;
    c->nrots++;
}

/* Split the files into lines and order the files by their first seq. */
static void kac_split(struct kac_ctx *c)
{
    const struct kitty_auditchain_verify_in *in = c->in;
    size_t *order = snewn(in->nfiles ? in->nfiles : 1, size_t);
    unsigned long long *first = snewn(in->nfiles ? in->nfiles : 1,
                                      unsigned long long);
    size_t lsize = 0, f, k;

    for (f = 0; f < in->nfiles; f++) {
        const char *d = in->files[f].data;
        size_t len = in->files[f].len, s = 0, i, lineno = 0;
        first[f] = 0;
        order[f] = f;
        while (s < len) {
            struct kac_line l;
            size_t e = s;
            int crlf = 0, badlf = 0;
            for (i = s; i < len; i++) {
                if (d[i] == '\n') {
                    if (i > s && d[i - 1] == '\r') {
                        crlf = 1;
                        e = i - 1;
                        break;
                    }
                    badlf = 1;
                }
            }
            if (!crlf)
                e = len;
            memset(&l, 0, sizeof(l));
            l.runidx = -1;
            l.p = d + s;
            l.len = e - s;
            l.file = f;
            l.lineno = ++lineno;
            l.complete = crlf;
            l.bad_eol = badlf;
            kac_parse_line(&l);
            if (l.chained && !first[f])
                first[f] = l.seq;
            sgrowarray(c->l, lsize, c->n);
            c->l[c->n++] = l;
            s = crlf ? e + 2 : len;
        }
    }
    /* stable: files without a chained line first, in the order given */
    for (f = 1; f < in->nfiles; f++)
        for (k = f; k > 0 && first[order[k - 1]] > first[order[k]]; k--) {
            size_t t = order[k];
            order[k] = order[k - 1];
            order[k - 1] = t;
        }
    {
        struct kac_line *sorted = snewn(c->n ? c->n : 1, struct kac_line);
        size_t m = 0, i;
        for (k = 0; k < in->nfiles; k++)
            for (i = 0; i < c->n; i++)
                if (c->l[i].file == order[k])
                    sorted[m++] = c->l[i];
        sfree(c->l);
        c->l = sorted;
    }
    sfree(order);
    sfree(first);
}

/* A resumed record accounts for the line before it when that line holds
 * exactly as many bytes as its fragment field. */
static void kac_mark_fragments(struct kac_ctx *c)
{
    size_t i;
    for (i = 1; i < c->n; i++) {
        struct kac_line *r = &c->l[i];
        if (r->is_anchor && r->f[KAC_RESULT] &&
            !strcmp(r->f[KAC_RESULT], "resumed") && r->f[KAC_FRAGMENT] &&
            c->l[i - 1].file == r->file &&
            c->l[i - 1].len == kac_parse_u64(r->f[KAC_FRAGMENT]))
            c->l[i - 1].fragment = 1;
    }
}

static void kac_walk(struct kac_ctx *c)
{
    size_t i, block = 0, nblock = 0, prechain = 0;
    const char *prevhash = NULL;          /* of the line before, any kind */
    const char *hash_before_frag = NULL;
    unsigned long long expect = 0;
    struct kac_run *run = NULL;
    int started = 0;

    for (i = 0; i < c->n; i++) {
        struct kac_line *l = &c->l[i];
        char sq[24], ex[24];
        kac_u64(sq, l->seq);
        if (l->bad_eol) {
            char *at = kac_at(c, l);
            kac_problem(c, at, dupprintf(KT_KAC_CRLF_FMT, at));
            sfree(at);
            prevhash = l->hash;
            continue;
        }
        if (l->fragment) {
            char *at = kac_at(c, l);
            kac_note(c->warns, dupprintf(KT_KAC_FRAGMENT_FMT, at));
            sfree(at);
            hash_before_frag = prevhash;
            continue;                     /* outside the chain */
        }
        if (!l->complete) {
            /* the last bytes of a file, no CRLF yet: kageant ended during
             * the write; its next start closes the line */
            char *at = kac_at(c, l);
            if (l->file == c->l[c->n - 1].file && i == c->n - 1)
                kac_note(c->warns, dupprintf(KT_KAC_FRAGMENT_FMT, at));
            else
                kac_problem(c, at, dupprintf(KT_KAC_FRAGMENT_BAD_FMT, at));
            sfree(at);
            continue;
        }
        if (!l->chained) {
            if (!started)
                prechain++;
            else if (!nblock++)
                block = i;
            prevhash = l->hash;
            continue;
        }

        /* an unchained block between chained lines */
        if (nblock) {
            int start = l->is_anchor && l->f[KAC_RESULT] &&
                !strcmp(l->f[KAC_RESULT], "start");
            char *at = kac_at(c, &c->l[block]);
            if (start) {
                char a[24], b[24], nb[24];
                kac_u64(a, c->l[block].lineno);
                kac_u64(b, c->l[block].lineno + nblock - 1);
                kac_u64(nb, nblock);
                kac_note(c->warns, dupprintf(KT_KAC_DOWNGRADE_FMT,
                                             kac_fname(c, &c->l[block]),
                                             a, b, nb));
            } else {
                kac_problem(c, at, dupprintf(KT_KAC_NOCHAIN_IN_RUN_FMT, at));
            }
            sfree(at);
            nblock = 0;
        }

        /* the link to the line before */
        {
            const char *want = hash_before_frag ? hash_before_frag : prevhash;
            int resumed = l->is_anchor && l->f[KAC_RESULT] &&
                !strcmp(l->f[KAC_RESULT], "resumed");
            if (resumed && l->f[KAC_FRAGMENT] && !hash_before_frag) {
                char *at = kac_at(c, l);
                kac_problem(c, at, dupprintf(KT_KAC_FRAGMENT_BAD_FMT, at));
                sfree(at);
            }
            if (!strcmp(l->prev, "none")) {
                if (prevhash) {
                    char *at = kac_at(c, l);
                    kac_problem(c, at, dupprintf(KT_KAC_RESTART_FMT, at));
                    sfree(at);
                }
            } else if (!want) {
                /* the oldest line present: its predecessor rotated out or
                 * was removed (kac_rotation decides) */
            } else if (strcmp(l->prev, want) && started &&
                       expect && l->seq == expect) {
                /* the line before was changed: name that line */
                const struct kac_line *b = NULL;
                size_t j;
                char *at;
                for (j = i; j-- > 0;)
                    if (!c->l[j].fragment) { b = &c->l[j]; break; }
                at = kac_at(c, b ? b : l);
                {
                    char bs[24];
                    kac_u64(bs, b ? b->seq : l->seq);
                    kac_problem(c, at, dupprintf(KT_KAC_CHANGED_FMT, at, bs,
                                                 b ? b->ts : l->ts));
                }
                sfree(at);
            } else if (strcmp(l->prev, want) && !started) {
                /* the first chained line does not bind the lines before */
                char *at = kac_at(c, l);
                kac_problem(c, at, dupprintf(KT_KAC_UNBOUND_FMT, at));
                sfree(at);
            }
            hash_before_frag = NULL;
        }

        /* the counter */
        if (started && l->seq != expect) {
            kac_u64(ex, expect);
            if (l->seq > expect) {
                /* the missing records later in the file: reordered */
                if (kac_find_seq(c, expect)) {
                    char *at = kac_at(c, l);
                    kac_problem(c, at, dupprintf(KT_KAC_REORDER_FMT, at, sq));
                    sfree(at);
                } else {
                    char a[24], nm[24];
                    char *at = kac_at(c, l);
                    kac_u64(a, expect - 1);
                    kac_u64(nm, l->seq - expect);
                    if (i > 0 && c->l[i - 1].file != l->file)
                        kac_problem(c, at, dupprintf(
                            l->seq - expect == 1 ? KT_KAC_GAPGEN1_FMT :
                            KT_KAC_GAPGEN_FMT, nm, a, sq,
                            kac_fname(c, &c->l[i - 1]), kac_fname(c, l)));
                    else
                        kac_problem(c, at, dupprintf(
                            l->seq - expect == 1 ? KT_KAC_MISSING1_FMT :
                            KT_KAC_MISSING_FMT, kac_fname(c, l), nm, a, sq));
                    sfree(at);
                }
            } else {
                char *at = kac_at(c, l);
                kac_problem(c, at, dupprintf(KT_KAC_REPEAT_FMT, at, sq, ex));
                sfree(at);
            }
        }

        /* an anchor: its run, its key, its signature */
        if (l->is_anchor) {
            const char *rid = l->f[KAC_RUN];
            const char *res = l->f[KAC_RESULT] ? l->f[KAC_RESULT] : "";
            unsigned char pub[KITTY_AUDITCHAIN_PUBLEN];
            int have = kac_unb64(l->f[KAC_LOGKEY], pub, sizeof(pub));
            struct kac_run *r = kac_find_run(c, rid);
            if (rid && !r) {
                sgrowarray(c->runs, c->runsize, c->nruns);
                r = &c->runs[c->nruns++];
                memset(r, 0, sizeof(*r));
                snprintf(r->id, sizeof(r->id), "%s", rid);
                snprintf(r->start_ts, sizeof(r->start_ts), "%s", l->ts);
                if (have) {
                    memcpy(r->pub, pub, sizeof(pub));
                    r->have_pub = 1;
                }
            } else if (r && have && r->have_pub &&
                       memcmp(r->pub, pub, sizeof(pub))) {
                char *at = kac_at(c, l);
                kac_problem(c, at, dupprintf(KT_KAC_KEYDIFF_FILE_FMT, at,
                                             r->start_ts));
                sfree(at);
            }
            if (!rid || !have || !l->has_sig || !l->sig_form ||
                !l->sig_ok) {
                char *at = kac_at(c, l);
                kac_problem(c, at, dupprintf(KT_KAC_SIGBAD_FMT, at, sq, l->ts));
                sfree(at);
            }
            if (r) {
                r->last_anchor = l->seq;
                if (!strcmp(res, "stop")) {
                    r->has_stop = 1;
                    snprintf(r->stop_ts, sizeof(r->stop_ts), "%s", l->ts);
                }
                run = r;
            }
            if (!strcmp(res, "rotate"))
                kac_add_rot(c, (unsigned)kac_parse_u64(l->f[KAC_ROTATION]),
                            (unsigned)kac_parse_u64(l->f[KAC_KEEP]),
                            (unsigned)kac_parse_u64(l->f[KAC_EXPIRED]),
                            l->seq);
        } else {
            /* every record is signed with the key of its run: the run of
             * the last anchor before it */
            const char *fmt = NULL;
            if (!run || !run->have_pub)
                fmt = KT_KAC_NOKEY_FMT;
            else if (!l->has_sig || !l->sig_form)
                fmt = KT_KAC_NOSIG_FMT;
            else if (!l->sig_ok)
                fmt = KT_KAC_SIGBAD_FMT;
            if (fmt) {
                char *at = kac_at(c, l);
                kac_problem(c, at, dupprintf(fmt, at, sq, l->ts));
                sfree(at);
            }
        }
        if (run) {
            run->last_seq = l->seq;
            l->runidx = (int)(run - c->runs);
        }
        started = 1;
        expect = l->seq + 1;
        prevhash = l->hash;
    }

    if (nblock) {
        char a[24], b[24], nb[24];
        kac_u64(a, c->l[block].lineno);
        kac_u64(b, c->l[block].lineno + nblock - 1);
        kac_u64(nb, nblock);
        kac_note(c->warns, dupprintf(KT_KAC_DOWNGRADE_FMT,
                                     kac_fname(c, &c->l[block]), a, b, nb));
    }
    if (prechain && started) {
        char nb[24];
        kac_u64(nb, prechain);
        kac_note(c->warns, dupprintf(KT_KAC_PRECHAIN_FMT,
                                     c->n ? kac_fname(c, &c->l[0]) : "", nb));
    }
}

/* The oldest file present and the rotate records: earlier records rotated
 * out, or deleted early (the keep count). */
static void kac_rotation(struct kac_ctx *c)
{
    const struct kac_line *first = NULL;
    size_t i;
    unsigned r = 0, m = 0, k = 0, allowed = 0, needed;
    for (i = 0; i < c->n; i++)
        if (c->l[i].chained && !c->l[i].fragment) {
            first = &c->l[i];
            break;
        }
    if (!first || !strcmp(first->prev, "none"))
        r = 0;
    else if (i > 0)
        r = 0;                      /* lines before the chain bind it */
    else if (first->is_anchor && first->f[KAC_RESULT] &&
             !strcmp(first->f[KAC_RESULT], "rotate"))
        r = (unsigned)kac_parse_u64(first->f[KAC_ROTATION]);
    else {
        char sq[24];
        char *at = kac_at(c, first);
        kac_u64(sq, first->seq);
        kac_problem(c, at, dupprintf(KT_KAC_HEADCUT_FMT, kac_fname(c, first),
                                     sq));
        sfree(at);
        return;
    }
    for (i = 0; i < c->nrots; i++)
        if (c->rots[i].rotation > m) {
            m = c->rots[i].rotation;
            k = c->rots[i].keep;
        }
    if (!m || !r)
        return;
    needed = m > k ? m - k : 0;
    for (i = 0; i < c->nrots; i++)
        if (c->rots[i].rotation > needed && c->rots[i].rotation <= m)
            allowed += c->rots[i].expired;
    if (r > needed + allowed) {
        char a[24], b[24];
        char *at = kac_at(c, first);
        kac_u64(a, r);
        kac_u64(b, k);
        kac_problem(c, at, dupprintf(KT_KAC_KEEP_FMT, kac_fname(c, first),
                                     b, a));
        sfree(at);
    } else {
        char sq[24];
        kac_u64(sq, first->seq);
        kac_note(c->warns, dupprintf(KT_KAC_ROTATED_OUT_FMT,
                                     kac_fname(c, first), sq, first->ts));
    }
}

/* The anchors of the Windows Event Log against the files. */
static void kac_anchors(struct kac_ctx *c)
{
    const struct kitty_auditchain_verify_in *in = c->in;
    unsigned long long minseq = 0, maxseq = 0, cut = 0;
    const struct kitty_auditchain_anchor *cuta = NULL;
    size_t i;
    for (i = 0; i < c->n; i++)
        if (c->l[i].chained && !c->l[i].fragment) {
            if (!minseq || c->l[i].seq < minseq)
                minseq = c->l[i].seq;
            if (c->l[i].seq > maxseq)
                maxseq = c->l[i].seq;
        }
    for (i = 0; i < in->nanchors; i++) {
        const struct kitty_auditchain_anchor *a = &in->anchors[i];
        const char *const *v = a->v;
        struct kac_run *r = kac_find_run(c, v[KAC_RUN]);
        unsigned char pub[KITTY_AUDITCHAIN_PUBLEN], sig[KITTY_AUDITCHAIN_SIGLEN];
        unsigned long long s = kac_parse_u64(v[KAC_SEQ]);
        strbuf *text;
        int ok;
        char rec[24], sq[24];
        struct kac_line *l;
        if (!r && !(v[KAC_FILE] && in->path &&
                    !stricmp(v[KAC_FILE], in->path)))
            continue;                     /* another log's anchor */
        kac_u64(rec, a->record);
        kac_u64(sq, s);
        text = strbuf_new();
        kitty_auditchain_anchor_body(text, v[KAC_TS] ? v[KAC_TS] : "", v);
        put_datapl(text, PTRLEN_LITERAL(" seq="));
        kitty_auditchain_put_quoted(text, v[KAC_SEQ] ? v[KAC_SEQ] : "");
        put_datapl(text, PTRLEN_LITERAL(" prev="));
        kitty_auditchain_put_quoted(text, v[KAC_PREV] ? v[KAC_PREV] : "");
        ok = s && kac_unb64(v[KAC_LOGKEY], pub, sizeof(pub)) &&
            kac_unb64(v[KAC_SIG], sig, sizeof(sig));
        if (ok) {
            /* the very line of the file, its signature already fitting:
             * no second check */
            const struct kac_line *fl = kac_find_seq(c, s);
            if (!(fl && fl->sig_ok && fl->is_anchor && fl->sig_form &&
                  fl->signed_len == text->len &&
                  !memcmp(fl->p, text->s, text->len) &&
                  !memcmp(fl->sig, sig, sizeof(sig))))
                ok = kitty_auditchain_verify_sig(pub, text->s, text->len, sig);
        }
        if (!ok) {
            kac_problem(c, NULL, dupprintf(KT_KAC_FORGED_FMT, a->log, rec));
            strbuf_free(text);
            continue;
        }
        if (v[KAC_RESULT] && !strcmp(v[KAC_RESULT], "rotate"))
            kac_add_rot(c, (unsigned)kac_parse_u64(v[KAC_ROTATION]),
                        (unsigned)kac_parse_u64(v[KAC_KEEP]),
                        (unsigned)kac_parse_u64(v[KAC_EXPIRED]), s);
        if (r) {
            if (r->have_pub && memcmp(r->pub, pub, sizeof(pub))) {
                if (!r->keydiff++)
                    kac_problem(c, NULL, dupprintf(KT_KAC_KEYDIFF_EVLOG_FMT,
                                                   r->start_ts, a->log));
                strbuf_free(text);
                continue;
            }
            r->confirmed = 1;
        }
        l = kac_find_seq(c, s);
        if (l) {
            if (!r || !l->is_anchor || l->signed_len != text->len ||
                memcmp(l->p, text->s, text->len)) {
                char *at = kac_at(c, l);
                kac_problem(c, at, dupprintf(
                    r ? KT_KAC_ANCHOR_DIFF_FMT : KT_KAC_OTHER_RUN_FMT,
                    at, sq, a->time ? a->time : "", a->log));
                sfree(at);
            }
        } else if (s > maxseq) {
            if (s > cut) {
                cut = s;
                cuta = a;
            }
        } else if (s >= minseq && r) {
            kac_problem(c, NULL, dupprintf(KT_KAC_ANCHOR_MISSING_FMT, sq,
                                           a->log, a->time ? a->time : ""));
        }
        strbuf_free(text);
    }
    if (cuta) {
        char a[24], b[24], nb[24];
        const char *name = c->in->nfiles ? c->in->files[0].name : "";
        size_t j;
        for (j = c->n; j-- > 0;)
            if (c->l[j].chained) { name = kac_fname(c, &c->l[j]); break; }
        kac_u64(a, maxseq);
        kac_u64(b, cut);
        kac_u64(nb, cut - maxseq);
        if (!maxseq)
            kac_problem(c, NULL, dupprintf(KT_KAC_RESTARTED_FMT, name,
                                           cuta->log, b,
                                           cuta->time ? cuta->time : ""));
        else
            kac_problem(c, NULL, dupprintf(KT_KAC_CUTOFF_FMT, name, a,
                                           cuta->log, b,
                                           cuta->time ? cuta->time : "", nb));
    }
}

/* An event that is not its file line, for kac_events */
struct kac_ev {
    const struct kitty_auditchain_event *e;
    strbuf *line;                    /* its signed line */
    unsigned long long seq;
    unsigned char sig[KITTY_AUDITCHAIN_SIGLEN];
    int sigform;
    char prev[64];
    char hash[KITTY_AUDITCHAIN_B64HASH + 1];   /* of its full line */
    long parent;                     /* the event whose hash is its prev */
    int haschild, done, who;
    struct kac_run *mine;
};

static struct kac_ev *kac_sort_ev;

static int kac_ev_hash_cmp(const void *a, const void *b)
{
    return strcmp(kac_sort_ev[*(const size_t *)a].hash,
                  kac_sort_ev[*(const size_t *)b].hash);
}

/* The other agent events against their runs and the file. An event whose
 * bytes are its line's is that line, whose signature the walk checked. Any
 * other is checked by its signature: under a run of another log file it is
 * that log's. Under a run of this file it must have its line here. Signed
 * by no run known and written while a run of this file was writing, it is
 * forged or edited. The events form chains by their prev fields: only the
 * end of a chain is checked by its signature, the events it binds take its
 * run. A forged event added anywhere is the end of a chain of its own. */
static void kac_events(struct kac_ctx *c)
{
    const struct kitty_auditchain_verify_in *in = c->in;
    struct { char id[KITTY_AUDITCHAIN_RUNLEN + 8];
             unsigned char pub[KITTY_AUDITCHAIN_PUBLEN]; } *oth = NULL;
    size_t noth = 0, othsize = 0, i, j, k;
    unsigned long long nok = 0, minseq = 0;
    struct kac_ev *ev = NULL;
    size_t nev = 0, evsize = 0;

    for (i = 0; i < c->n; i++)
        if (c->l[i].chained && !c->l[i].fragment &&
            (!minseq || c->l[i].seq < minseq))
            minseq = c->l[i].seq;

    /* the run keys of the other log files, from their anchors that are
     * signed by those keys */
    for (i = 0; i < in->nanchors; i++) {
        const char *const *v = in->anchors[i].v;
        unsigned char pub[KITTY_AUDITCHAIN_PUBLEN], sig[KITTY_AUDITCHAIN_SIGLEN];
        strbuf *text;
        int ok, dup = 0;
        if (!v[KAC_RUN] || kac_find_run(c, v[KAC_RUN]) ||
            (v[KAC_FILE] && in->path && !stricmp(v[KAC_FILE], in->path)))
            continue;
        for (j = 0; j < noth; j++)
            if (!strcmp(oth[j].id, v[KAC_RUN]))
                dup = 1;
        if (dup || !kac_unb64(v[KAC_LOGKEY], pub, sizeof(pub)) ||
            !kac_unb64(v[KAC_SIG], sig, sizeof(sig)))
            continue;
        text = strbuf_new();
        kitty_auditchain_anchor_body(text, v[KAC_TS] ? v[KAC_TS] : "", v);
        put_datapl(text, PTRLEN_LITERAL(" seq="));
        kitty_auditchain_put_quoted(text, v[KAC_SEQ] ? v[KAC_SEQ] : "");
        put_datapl(text, PTRLEN_LITERAL(" prev="));
        kitty_auditchain_put_quoted(text, v[KAC_PREV] ? v[KAC_PREV] : "");
        ok = kitty_auditchain_verify_sig(pub, text->s, text->len, sig);
        strbuf_free(text);
        if (!ok)
            continue;
        sgrowarray(oth, othsize, noth);
        snprintf(oth[noth].id, sizeof(oth[noth].id), "%s", v[KAC_RUN]);
        memcpy(oth[noth].pub, pub, sizeof(pub));
        noth++;
    }

    /* phase 1: the events that are their file line pass; the others are
     * kept with their signed line, its full hash and their prev */
    for (i = 0; i < in->nevents; i++) {
        const struct kitty_auditchain_event *e = &in->events[i];
        strbuf *line = strbuf_new();
        const char *sigtxt = NULL;
        unsigned long long seq = 0;
        unsigned char sig[KITTY_AUDITCHAIN_SIGLEN];
        struct kac_line *fl;
        struct kac_ev *u;
        int sigform;
        if (!kitty_auditchain_event_line(e->id, e->s, e->n, line, &sigtxt,
                                         &seq) || !seq) {
            strbuf_free(line);               /* not in the file's chain */
            continue;
        }
        sigform = kac_unb64(sigtxt, sig, sizeof(sig));
        fl = kac_find_seq(c, seq);
        if (fl && fl->has_sig && fl->sig_form && sigform &&
            fl->signed_len == line->len && !memcmp(fl->p, line->s, line->len) &&
            !memcmp(fl->sig, sig, sizeof(sig))) {
            nok++;                            /* the very line */
            strbuf_free(line);
            continue;
        }
        sgrowarray(ev, evsize, nev);
        u = &ev[nev++];
        memset(u, 0, sizeof(*u));
        u->e = e;
        u->seq = seq;
        u->sigform = sigform;
        memcpy(u->sig, sig, sizeof(sig));
        u->parent = -1;
        /* its prev: the last string (the row ends with seq sig ts prev) */
        snprintf(u->prev, sizeof(u->prev), "%s", e->s[e->n - 1]);
        {
            strbuf *full = strbuf_new();
            put_data(full, line->s, line->len);
            put_datapl(full, PTRLEN_LITERAL(" sig="));
            kitty_auditchain_put_quoted(full, sigtxt ? sigtxt : "");
            kitty_auditchain_hash_b64(full->s, full->len, u->hash);
            strbuf_free(full);
        }
        u->line = line;
    }

    /* phase 2: an event whose full line is another's prev is bound by that
     * one's signature. Only the ends of such chains are checked, and the
     * events before an end that fits take its run. */
    if (nev) {
        size_t *byhash = snewn(nev, size_t);
        for (i = 0; i < nev; i++)
            byhash[i] = i;
        kac_sort_ev = ev;
        qsort(byhash, nev, sizeof(size_t), kac_ev_hash_cmp);
        for (i = 0; i < nev; i++) {
            size_t lo = 0, hi = nev;
            while (lo < hi) {
                size_t mid = lo + (hi - lo) / 2;
                if (strcmp(ev[byhash[mid]].hash, ev[i].prev) < 0)
                    lo = mid + 1;
                else
                    hi = mid;
            }
            if (lo < nev && !strcmp(ev[byhash[lo]].hash, ev[i].prev) &&
                byhash[lo] != i) {
                ev[i].parent = (long)byhash[lo];
                ev[byhash[lo]].haschild = 1;
            }
        }
        sfree(byhash);
        for (k = 0; k < 2; k++)          /* the chain ends, then the rest */
            for (i = 0; i < nev; i++) {
                struct kac_ev *u = &ev[i];
                int who = 0;               /* 1 other, 2 mine */
                struct kac_run *mine = NULL;
                long v;
                if (u->done || (k == 0 && u->haschild))
                    continue;
                if (u->sigform) {
                    for (j = 0; j < noth && !who; j++)
                        if (kitty_auditchain_verify_sig(oth[j].pub, u->line->s,
                                                        u->line->len, u->sig))
                            who = 1;
                    for (j = 0; j < c->nruns && !who; j++)
                        if (c->runs[j].have_pub &&
                            kitty_auditchain_verify_sig(c->runs[j].pub,
                                                        u->line->s,
                                                        u->line->len,
                                                        u->sig)) {
                            who = 2;
                            mine = &c->runs[j];
                        }
                }
                u->done = 1;
                u->who = who;
                u->mine = mine;
                if (!who)
                    continue;
                for (v = u->parent; v >= 0 && !ev[v].done; v = ev[v].parent) {
                    ev[v].done = 1;
                    ev[v].who = who;
                    ev[v].mine = mine;
                }
            }
    }

    /* the report, in the order the events were read */
    for (i = 0; i < nev; i++) {
        struct kac_ev *u = &ev[i];
        const struct kitty_auditchain_event *e = u->e;
        const char *etime = e->time ? e->time : "";
        struct kac_line *fl = kac_find_seq(c, u->seq);
        char rec[24], sq[24], idt[24];
        strbuf_free(u->line);
        if (u->who == 1)
            continue;                         /* another log file's */
        kac_u64(rec, e->record);
        kac_u64(sq, u->seq);
        kac_u64(idt, e->id);
        if (u->who == 2) {
            if (!fl && u->seq < minseq) {
                /* its line rotated out with an older generation */
            } else if (!fl) {
                kac_problem(c, NULL, dupprintf(KT_KAC_EV_MISSING_FMT, rec,
                                               e->log, idt, sq,
                                               u->mine->start_ts));
            } else {
                char *at = kac_at(c, fl);
                kac_problem(c, NULL, dupprintf(KT_KAC_EV_DIFF_FMT, rec, e->log,
                                               idt, sq, at));
                sfree(at);
            }
            continue;
        }
        /* signed by no run known: forged when a run of this file was
         * writing at its time */
        for (j = 0; j < c->nruns; j++) {
            const struct kac_run *r = &c->runs[j];
            if (*etime && strcmp(etime, r->start_ts) >= 0 &&
                (!r->has_stop || strcmp(etime, r->stop_ts) <= 0)) {
                kac_problem(c, NULL, dupprintf(KT_KAC_EV_FORGED_FMT, rec,
                                               e->log, idt, sq));
                break;
            }
        }
    }
    sfree(ev);
    if (nok) {
        char n[24];
        kac_u64(n, nok);
        kac_note(c->warns, dupprintf(nok == 1 ? KT_KAC_EV_OK1_FMT :
                                     KT_KAC_EV_OK_FMT, n,
                                     in->anchors_where ? in->anchors_where :
                                     ""));
    }
    sfree(oth);
}

int kitty_auditchain_verify(const struct kitty_auditchain_verify_in *in,
                            strbuf *out)
{
    struct kac_ctx c;
    size_t i;
    unsigned long long nrec = 0, minseq = 0, maxseq = 0;
    int code;
    const char *where = in->anchors_where ? in->anchors_where : "";

    memset(&c, 0, sizeof(c));
    c.in = in;
    c.probs = strbuf_new();
    c.warns = strbuf_new();
    c.cannot = strbuf_new();
    kac_split(&c);
    kac_mark_fragments(&c);
    kac_index(&c);
    kac_presign(&c);
    kac_walk(&c);
    kac_rotation(&c);
    if (in->have_anchors) {
        kac_anchors(&c);
        kac_events(&c);
    }

    for (i = 0; i < c.n; i++)
        if (c.l[i].complete && !c.l[i].fragment) {
            nrec++;
            if (c.l[i].chained) {
                if (!minseq || c.l[i].seq < minseq)
                    minseq = c.l[i].seq;
                if (c.l[i].seq > maxseq)
                    maxseq = c.l[i].seq;
            }
        }

    /* every run: its stop record, its records after the last anchor, its
     * key confirmed by the Windows Event Log */
    for (i = 0; i < c.nruns; i++) {
        struct kac_run *r = &c.runs[i];
        int live = in->live_run && i == c.nruns - 1 &&
            (!strcmp(in->live_run, "*") || !strcmp(in->live_run, r->id));
        if (live)
            kac_note(c.warns, dupprintf(KT_KAC_LIVE_FMT, r->start_ts));
        else if (!r->has_stop)
            kac_note(c.warns, dupprintf(KT_KAC_NOSTOP_FMT, r->start_ts));
        if (r->last_seq > r->last_anchor) {
            char a[24], b[24], s[24];
            kac_u64(a, r->last_anchor + 1);
            kac_u64(b, r->last_seq);
            kac_u64(s, r->last_anchor);
            kac_note(c.warns, dupprintf(
                r->last_seq == r->last_anchor + 1 ? KT_KAC_UNSIGNED1_FMT :
                KT_KAC_UNSIGNED_FMT, a, b, s));
        }
        if (in->have_anchors && !r->confirmed && !r->keydiff) {
            c.ncannot++;
            if (in->anchors_since && strcmp(r->start_ts, in->anchors_since) < 0)
                kac_note(c.cannot, dupprintf(KT_KAC_ROLLED_OUT_FMT,
                                             in->anchors_since, where,
                                             r->start_ts));
            else
                kac_note(c.cannot, dupprintf(KT_KAC_KEY_UNCONFIRMED_FMT,
                                             r->start_ts, where));
        }
    }
    if (!maxseq) {
        c.ncannot++;
        kac_note(c.cannot, dupprintf(KT_KAC_NOTHING_FMT,
                                     in->nfiles ? in->files[0].name : ""));
    } else if (!in->have_anchors) {
        char s[24];
        c.ncannot++;
        kac_u64(s, maxseq);
        kac_note(c.cannot, dupprintf(KT_KAC_NO_EVLOG_FMT, where, s));
    }

    put_data(out, c.probs->s, c.probs->len);
    if (c.nprobs > KAC_MAX_LISTED) {
        char n[24];
        kac_u64(n, (unsigned long long)(c.nprobs - KAC_MAX_LISTED));
        kac_note(out, dupprintf(c.nprobs - KAC_MAX_LISTED == 1 ?
                                KT_KAC_MORE1_FMT : KT_KAC_MORE_FMT, n));
    }
    put_data(out, c.cannot->s, c.cannot->len);
    put_data(out, c.warns->s, c.warns->len);
    {
        strbuf *names = strbuf_new();
        char a[24], b[24], nr[24];
        size_t f;
        for (f = 0; f < in->nfiles; f++) {
            if (f)
                put_datapl(names, PTRLEN_LITERAL(", "));
            put_dataz(names, in->files[in->nfiles - 1 - f].name);
        }
        kac_u64(nr, nrec);
        kac_u64(a, minseq);
        kac_u64(b, maxseq);
        kac_note(out, dupprintf(KT_KAC_SUMMARY_FMT, names->s, nr, a, b));
        strbuf_free(names);
    }
    if (c.nprobs) {
        char n[24];
        kac_u64(n, (unsigned long long)c.nprobs);
        kac_note(out, dupprintf(c.nprobs == 1 ? KT_KAC_BROKEN1_FMT :
                                KT_KAC_BROKEN_FMT, n,
                                c.firstat ? c.firstat :
                                in->nfiles ? in->files[in->nfiles - 1].name :
                                where));
        code = 1;
    } else if (c.ncannot) {
        kac_note(out, dupprintf(KT_KAC_CANNOT_FMT));
        code = 2;
    } else {
        char n[24];
        kac_u64(n, (unsigned long long)c.nruns);
        kac_note(out, dupprintf(c.nruns == 1 ? KT_KAC_INTACT1_FMT :
                                KT_KAC_INTACT_FMT, n, where));
        code = 0;
    }

    for (i = 0; i < c.n; i++)
        kac_free_line(&c.l[i]);
    sfree(c.l);
    sfree(c.runs);
    sfree(c.rots);
    sfree(c.firstat);
    sfree(c.byseq);
    strbuf_free(c.probs);
    strbuf_free(c.warns);
    strbuf_free(c.cannot);
    return code;
}
