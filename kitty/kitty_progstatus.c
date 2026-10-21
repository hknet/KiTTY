/*
 * kitty_progstatus.c - program status reports from the host (OSC 7501) and
 * the shell's prompt marks (OSC 133): parsing, limits, the record store and
 * the window's summary. Platform-free; see kitty_progstatus.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kitty_oscnotify.h"     /* on_base64_decode */
#include "kitty_progstatus.h"
#include "kitty_text.h"          /* KT_TITLE_PS_*: the title bar's marker */

/* ---- parsing ------------------------------------------------------------ */

static int ps_is_space(char c)
{
    return c == ' ' || c == '\t';
}

/* The value set: [A-Za-z0-9_.,+/=-] */
static int ps_value_char(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '.' || c == ',' ||
           c == '+' || c == '/' || c == '=' || c == '-';
}

/* app and id segments: [A-Za-z0-9_.+-] */
static int ps_name_char(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '+' ||
           c == '-';
}

static int ps_eq(const char *s, size_t len, const char *lit)
{
    return strlen(lit) == len && !memcmp(s, lit, len);
}

/* id: segment("/"segment)*, 1..32 name characters each, at most 8. */
static int ps_id_valid(const char *s, size_t len)
{
    size_t i, seg = 0;
    int levels = 1;
    if (len == 0 || len > PS_ID_MAX)
        return 0;
    for (i = 0; i < len; i++) {
        if (s[i] == '/') {
            if (seg == 0 || ++levels > PS_ID_LEVELS)
                return 0;
            seg = 0;
        } else if (!ps_name_char(s[i]) || ++seg > PS_ID_SEG_MAX) {
            return 0;
        }
    }
    return seg > 0;
}

/* Decoded text is taken only when it is UTF-8 without control characters
 * (C0, DEL, C1); anything else is refused, never cleaned. */
static int ps_text_ok(const unsigned char *s, size_t len)
{
    size_t i = 0;
    while (i < len) {
        unsigned c = s[i];
        unsigned cp;
        int n;
        if (c < 0x80) {
            if (c < 0x20 || c == 0x7F)
                return 0;
            i++;
            continue;
        }
        if (c >= 0xC2 && c <= 0xDF) { n = 1; cp = c & 0x1F; }
        else if (c >= 0xE0 && c <= 0xEF) { n = 2; cp = c & 0x0F; }
        else if (c >= 0xF0 && c <= 0xF4) { n = 3; cp = c & 0x07; }
        else return 0;
        {
            int k;
            for (k = 1; k <= n; k++) {
                if (i + k >= len || (s[i + k] & 0xC0) != 0x80)
                    return 0;
                cp = (cp << 6) | (s[i + k] & 0x3F);
            }
        }
        if ((n == 2 && cp < 0x800) || (n == 3 && (cp < 0x10000 || cp > 0x10FFFF)) ||
            (cp >= 0xD800 && cp <= 0xDFFF) || (cp >= 0x80 && cp <= 0x9F))
            return 0;
        i += n + 1;
    }
    return 1;
}

/* A base64 text value. 1 = taken into out, 0 = refused (the key is
 * dropped), -1 = over a limit (the report is discarded). */
static int ps_text(const char *v, size_t vlen, size_t enc_max, size_t dec_max,
                   char *out)
{
    unsigned char *buf;
    long n;
    if (vlen > enc_max)
        return -1;
    buf = malloc(vlen * 3 / 4 + 3);
    if (!buf)
        return 0;
    n = on_base64_decode(v, vlen, buf);
    if (n < 0) {
        free(buf);
        return 0;
    }
    if ((size_t)n > dec_max) {
        free(buf);
        return -1;
    }
    if (!ps_text_ok(buf, (size_t)n)) {
        free(buf);
        return 0;
    }
    memcpy(out, buf, (size_t)n);
    out[n] = '\0';
    free(buf);
    return 1;
}

int ps_parse(const char *s, size_t len, PsRecord *out)
{
    /* "\033]7501;" in front and "\033\\" behind count towards the limit. */
    const size_t overhead = 7 + 2;
    const char *state_v = NULL, *kind_v = NULL, *prog_v = NULL;
    const char *id_v = NULL, *app_v = NULL, *title_v = NULL, *msg_v = NULL;
    size_t state_l = 0, kind_l = 0, prog_l = 0, id_l = 0, app_l = 0;
    size_t title_l = 0, msg_l = 0;
    size_t i = 0;

    memset(out, 0, sizeof(*out));
    out->progress = -1;
    if (len + overhead > PS_SEQ_MAX)
        return PS_REJECT;

    {
        size_t a = 0, b = len;
        while (a < b && ps_is_space(s[a])) a++;
        while (b > a && ps_is_space(s[b - 1])) b--;
        if (b - a == 1 && s[a] == '?')
            return PS_QUERY;
    }

    while (i <= len) {
        size_t start = i, end, eq, ks, ke, vs, ve, k;
        int bad = 0;
        while (i < len && s[i] != ':')
            i++;
        end = i++;
        for (eq = start; eq < end && s[eq] != '='; eq++)
            ;
        if (eq == end)
            continue;                    /* no '=': skipped */
        ks = start; ke = eq;
        while (ks < ke && ps_is_space(s[ks])) ks++;
        while (ke > ks && ps_is_space(s[ke - 1])) ke--;
        vs = eq + 1; ve = end;
        while (vs < ve && ps_is_space(s[vs])) vs++;
        while (ve > vs && ps_is_space(s[ve - 1])) ve--;
        if (ke == ks)
            continue;                    /* empty key: skipped */
        if (ke - ks > PS_KEY_MAX)
            return PS_REJECT;
        for (k = ks; k < ke; k++)
            if (s[k] < 'a' || s[k] > 'z')
                bad = 1;
        for (k = vs; k < ve; k++)
            if (!ps_value_char(s[k]))
                bad = 1;
        if (bad)
            continue;                    /* a byte outside the set: skipped */
#define PS_TAKE(name, v, l) \
        if (ps_eq(s + ks, ke - ks, name)) { v = s + vs; l = ve - vs; continue; }
        PS_TAKE("state", state_v, state_l)
        PS_TAKE("kind", kind_v, kind_l)
        PS_TAKE("progress", prog_v, prog_l)
        PS_TAKE("id", id_v, id_l)
        PS_TAKE("app", app_v, app_l)
        PS_TAKE("title", title_v, title_l)
        PS_TAKE("msg", msg_v, msg_l)
#undef PS_TAKE
        /* unknown keys are ignored */
    }

    if (!state_v)
        return PS_REJECT;
    if (ps_eq(state_v, state_l, "idle"))         out->state = PS_IDLE;
    else if (ps_eq(state_v, state_l, "working")) out->state = PS_WORKING;
    else if (ps_eq(state_v, state_l, "done"))    out->state = PS_DONE;
    else if (ps_eq(state_v, state_l, "blocked")) out->state = PS_BLOCKED;
    else if (ps_eq(state_v, state_l, "error"))   out->state = PS_ERROR;
    else if (ps_eq(state_v, state_l, "clear"))   out->state = PS_CLEAR;
    else return PS_REJECT;

    if (id_v && id_l) {
        if (id_l > PS_ID_MAX || !ps_id_valid(id_v, id_l))
            return PS_REJECT;
        memcpy(out->id, id_v, id_l);
        out->id[id_l] = '\0';
    }

    if (app_v && app_l) {
        size_t k;
        int ok = 1;
        if (app_l > PS_APP_MAX)
            return PS_REJECT;
        for (k = 0; k < app_l; k++)
            if (!ps_name_char(app_v[k]))
                ok = 0;
        if (ok) {
            memcpy(out->app, app_v, app_l);
            out->app[app_l] = '\0';
        }
    }

    if (kind_v && out->state == PS_BLOCKED) {
        if (ps_eq(kind_v, kind_l, "permission"))    out->kind = PS_KIND_PERMISSION;
        else if (ps_eq(kind_v, kind_l, "question")) out->kind = PS_KIND_QUESTION;
        else if (ps_eq(kind_v, kind_l, "auth"))     out->kind = PS_KIND_AUTH;
    }

    if (prog_v && prog_l && prog_l <= 3 &&
        (out->state == PS_WORKING || out->state == PS_BLOCKED)) {
        int v = 0;
        size_t k;
        for (k = 0; k < prog_l && prog_v[k] >= '0' && prog_v[k] <= '9'; k++)
            v = v * 10 + (prog_v[k] - '0');
        if (k == prog_l && v <= 100)
            out->progress = v;
    }

    if (title_v && title_l &&
        ps_text(title_v, title_l, PS_TITLE_ENC_MAX, PS_TITLE_MAX, out->title) < 0)
        return PS_REJECT;
    if (msg_v && msg_l &&
        ps_text(msg_v, msg_l, PS_MSG_ENC_MAX, PS_MSG_MAX, out->msg) < 0)
        return PS_REJECT;

    return PS_REPORT;
}

/* ---- the store ---------------------------------------------------------- */

static int ps_find(const PsStore *st, const char *id)
{
    int i;
    for (i = 0; i < st->n; i++)
        if (!strcmp(st->e[i]->r.id, id))
            return i;
    return -1;
}

static void ps_remove_at(PsStore *st, int i)
{
    free(st->e[i]);
    st->e[i] = st->e[--st->n];
}

/* Is `id` the record `top` or beneath it? The root ("") holds everything. */
static int ps_under(const char *id, const char *top)
{
    size_t n = strlen(top);
    if (!n)
        return 1;
    return !strncmp(id, top, n) && (id[n] == '\0' || id[n] == '/');
}

PsEntry *ps_apply(PsStore *st, const PsRecord *r, int *prev)
{
    int i = ps_find(st, r->id);
    PsEntry *e;

    *prev = i >= 0 ? st->e[i]->r.state : -1;
    if (r->state == PS_CLEAR) {
        for (i = st->n - 1; i >= 0; i--)
            if (ps_under(st->e[i]->r.id, r->id))
                ps_remove_at(st, i);
        *prev = -1;
        return NULL;
    }
    if (i < 0) {
        if (st->n == PS_RECORDS_MAX) {
            int k, oldest = 0;
            for (k = 1; k < st->n; k++)
                if (st->e[k]->order < st->e[oldest]->order)
                    oldest = k;
            ps_remove_at(st, oldest);
        }
        e = malloc(sizeof(*e));
        if (!e)
            return NULL;
        st->e[st->n++] = e;
    } else {
        e = st->e[i];
    }
    /* Each report replaces its record completely. */
    e->r = *r;
    e->order = ++st->counter;
    e->seen = 0;
    return e;
}

void ps_drop_running(PsStore *st)
{
    int i;
    for (i = st->n - 1; i >= 0; i--)
        if (st->e[i]->r.state == PS_WORKING || st->e[i]->r.state == PS_BLOCKED)
            ps_remove_at(st, i);
}

void ps_free(PsStore *st)
{
    while (st->n)
        ps_remove_at(st, st->n - 1);
}

void ps_mark_seen(PsStore *st)
{
    int i;
    for (i = 0; i < st->n; i++)
        st->e[i]->seen = 1;
}

const char *ps_app_of(const PsStore *st, const PsRecord *r)
{
    char id[PS_ID_MAX + 1];
    if (r->app[0])
        return r->app;
    strcpy(id, r->id);
    while (id[0]) {
        char *slash = strrchr(id, '/');
        int i;
        if (slash)
            *slash = '\0';
        else
            id[0] = '\0';
        i = ps_find(st, id);
        if (i >= 0 && st->e[i]->r.app[0])
            return st->e[i]->r.app;
    }
    return "";
}

void ps_view(const PsStore *st, PsView *v)
{
    unsigned long best_prog_order = 0;
    unsigned long best_order = 0;
    int i;

    v->state = PS_IDLE;
    v->kind = PS_KIND_NONE;
    v->progress = -1;
    for (i = 0; i < st->n; i++) {
        const PsEntry *e = st->e[i];
        int s = e->r.state;
        if ((s == PS_DONE || s == PS_ERROR) && e->seen)
            continue;
        if (s == PS_IDLE)
            continue;
        if (s > v->state || (s == v->state && e->order > best_order)) {
            if (s != v->state)
                best_prog_order = 0;
            v->state = s;
            v->kind = e->r.kind;
            best_order = e->order;
        }
    }
    if (v->state != PS_WORKING && v->state != PS_BLOCKED)
        return;
    for (i = 0; i < st->n; i++) {
        const PsEntry *e = st->e[i];
        if (e->r.state == v->state && e->r.progress >= 0 &&
            e->order > best_prog_order) {
            v->progress = e->r.progress;
            best_prog_order = e->order;
        }
    }
}

/* ---- the overlay's rows ------------------------------------------------- */

/* The id's first segment: the group a record belongs to. */
static size_t ps_top_len(const char *id)
{
    const char *slash = strchr(id, '/');
    return slash ? (size_t)(slash - id) : strlen(id);
}

/* The most urgent state in a group (PS_* rise with urgency: done, working,
 * error, blocked). */
static int ps_group_rank(const PsStore *st, const char *id)
{
    size_t n = ps_top_len(id);
    int i, best = PS_IDLE;
    for (i = 0; i < st->n; i++) {
        const char *o = st->e[i]->r.id;
        if (ps_top_len(o) == n && !strncmp(o, id, n) && st->e[i]->r.state > best)
            best = st->e[i]->r.state;
    }
    return best;
}

/* Ids compared with '/' below every other character, so a record's children
 * follow it straight away ("build", "build/test", "build-x"). */
static int ps_id_cmp(const char *a, const char *b)
{
    for (;; a++, b++) {
        int ca = *a == '/' ? 1 : (unsigned char)*a;
        int cb = *b == '/' ? 1 : (unsigned char)*b;
        if (ca != cb || !ca)
            return ca - cb;
    }
}

static int ps_row_before(const PsStore *st, const PsEntry *a, const PsEntry *b)
{
    int ra = ps_group_rank(st, a->r.id), rb = ps_group_rank(st, b->r.id);
    size_t na = ps_top_len(a->r.id), nb = ps_top_len(b->r.id);
    if (ra != rb)
        return ra > rb;
    if (na != nb || strncmp(a->r.id, b->r.id, na)) {
        int c = strncmp(a->r.id, b->r.id, na < nb ? na : nb);
        return c ? c < 0 : na < nb;
    }
    return ps_id_cmp(a->r.id, b->r.id) < 0;
}

int ps_rows(const PsStore *st, PsRow *rows)
{
    int i, j, n = 0;
    for (i = 0; i < st->n; i++) {
        const PsEntry *e = st->e[i];
        const char *slash, *seg;
        PsRow row;
        if (e->r.state == PS_IDLE)
            continue;
        memset(&row, 0, sizeof(row));
        row.e = e;
        for (seg = e->r.id; *seg; seg++)
            if (*seg == '/')
                row.depth++;
        slash = strrchr(e->r.id, '/');
        if (e->r.title[0])
            row.name = e->r.title;
        else if (e->r.id[0])
            row.name = slash ? slash + 1 : e->r.id;
        else
            row.name = ps_app_of(st, &e->r);
        if (slash) {
            const char *p = slash;
            size_t len;
            while (p > e->r.id && p[-1] != '/')
                p--;
            len = (size_t)(slash - p);
            if (len > PS_ID_SEG_MAX)
                len = PS_ID_SEG_MAX;
            memcpy(row.parent, p, len);
            row.parent[len] = '\0';
        }
        /* Insertion: there are at most PS_RECORDS_MAX. */
        for (j = n; j > 0 && ps_row_before(st, e, rows[j - 1].e); j--)
            rows[j] = rows[j - 1];
        rows[j] = row;
        n++;
    }
    return n;
}

int ps_overlay_due(const PsStore *st)
{
    int i, rows = 0, live = 0;
    for (i = 0; i < st->n; i++) {
        int s = st->e[i]->r.state;
        if (s == PS_IDLE)
            continue;
        rows++;
        if (s == PS_WORKING || s == PS_BLOCKED || !st->e[i]->seen)
            live = 1;
    }
    return rows >= 2 && live;
}

const char *ps_state_phrase(int state, int kind)
{
    switch (state) {
      case PS_WORKING: return KT_PST_WORKING;
      case PS_DONE:    return KT_PST_DONE;
      case PS_ERROR:   return KT_PST_ERROR;
      case PS_BLOCKED:
        return kind == PS_KIND_PERMISSION ? KT_PST_BLOCKED_PERMISSION :
               kind == PS_KIND_QUESTION   ? KT_PST_BLOCKED_QUESTION :
               kind == PS_KIND_AUTH       ? KT_PST_BLOCKED_AUTH :
                                            KT_PST_BLOCKED;
      default:         return "";
    }
}

void ps_notice_line(const PsStore *st, const PsRecord *r, char *buf,
                    size_t size)
{
    const char *name = r->title[0] ? r->title : ps_app_of(st, r);
    const char *phrase = ps_state_phrase(r->state, r->kind);
    if (name[0])
        snprintf(buf, size, KT_PST_NOTICE_FMT, name, phrase);
    else
        snprintf(buf, size, "%s", phrase);
}

int ps_switch_notice(const PsStore *st, const PsRecord *r, int prev,
                     int panel, int secs, char *buf, size_t size)
{
    if (size)
        buf[0] = '\0';
    if (!r || secs <= 0 || panel || r->state == prev ||
        (r->state != PS_BLOCKED && r->state != PS_ERROR &&
         r->state != PS_DONE))
        return PS_IDLE;
    ps_notice_line(st, r, buf, size);
    return r->state;
}

int ps_progress_notice(int prev, int state, int value, int ps7501_up,
                       int secs, char *buf, size_t size)
{
    if (size)
        buf[0] = '\0';
    if (secs <= 0 || ps7501_up || state == prev || (state != 2 && state != 4))
        return PS_IDLE;
    if (state == 2) {
        if (value >= 0)
            snprintf(buf, size, KT_PROG94_ERROR_AT, value);
        else
            snprintf(buf, size, "%s", KT_PROG94_ERROR);
        return PS_ERROR;
    }
    if (value >= 0)
        snprintf(buf, size, KT_PROG94_PAUSED_AT, value);
    else
        snprintf(buf, size, "%s", KT_PROG94_PAUSED);
    return PS_BLOCKED;
}

void ps_title_marker(const PsView *v, char *buf)
{
    const char *t;
    switch (v->state) {
      case PS_WORKING:
        if (v->progress >= 0) {
            snprintf(buf, PS_TITLE_MARKER_MAX, KT_TITLE_PS_PERCENT, v->progress);
            return;
        }
        t = KT_TITLE_PS_WORKING;
        break;
      case PS_BLOCKED: t = KT_TITLE_PS_BLOCKED; break;
      case PS_ERROR:   t = KT_TITLE_PS_FAILED;  break;
      case PS_DONE:    t = KT_TITLE_PS_DONE;    break;
      default:         t = "";                  break;
    }
    snprintf(buf, PS_TITLE_MARKER_MAX, "%s", t);
}

/* ---- OSC 133 ------------------------------------------------------------ */

int ps_osc133(const char *s, size_t len, int *exit_code)
{
    *exit_code = -1;
    if (len == 0 || (len > 1 && s[1] != ';'))
        return PS133_OTHER;
    if (s[0] == 'A')
        return PS133_PROMPT;
    if (s[0] == 'B')
        return PS133_INPUT;
    if (s[0] == 'C')
        return PS133_OUTPUT;
    if (s[0] == 'D') {
        if (len > 2) {
            int v = 0;
            size_t k = 2;
            int neg = 0;
            if (s[k] == '-') { neg = 1; k++; }
            if (k < len && s[k] >= '0' && s[k] <= '9') {
                while (k < len && s[k] >= '0' && s[k] <= '9' && v < 100000)
                    v = v * 10 + (s[k++] - '0');
                *exit_code = neg ? -v : v;
            }
        }
        return PS133_END;
    }
    return PS133_OTHER;
}
