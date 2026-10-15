/*
 * kitty_ctx3008.c - OSC 3008 hierarchical context signalling (UAPI.15): the
 * parsing, the limits and the context stack. See kitty_ctx3008.h; the
 * terminal side is term_osc3008 in terminal/kitty_osc.c.
 */
#include <stdlib.h>
#include <string.h>

#include "kitty_ctx3008.h"

static const char *const type_names[CTX_TYPE_COUNT] = {
    "", "boot", "container", "vm", "elevate", "chpriv", "subcontext",
    "remote", "shell", "command", "app", "service", "session",
};

static const char *const field_names[CTX_F_COUNT] = {
    "user", "hostname", "machineid", "bootid", "pid", "pidfdid", "comm",
    "cwd", "cmdline", "vm", "container", "targetuser", "targethost",
    "sessionid",
};

static const char *const exit_names[] = {
    "", "success", "failure", "crash", "interrupt",
};

const char *ctx_type_name(int type)
{
    return type >= 0 && type < CTX_TYPE_COUNT ? type_names[type] : "";
}

const char *ctx_field_name(int field)
{
    return field >= 0 && field < CTX_F_COUNT ? field_names[field] : "";
}

const char *ctx_exit_name(int code)
{
    return code >= 0 && code <= CTX_EXIT_INTERRUPT ? exit_names[code] : "";
}

/* Which types a type-specific field belongs to (the specification's table);
 * the fields not listed apply to every type. */
static bool field_applies(int field, int type)
{
    switch (field) {
      case CTX_F_CWD:
        return type == CTX_TYPE_SHELL || type == CTX_TYPE_COMMAND;
      case CTX_F_CMDLINE:
        return type == CTX_TYPE_COMMAND;
      case CTX_F_VM:
        return type == CTX_TYPE_VM;
      case CTX_F_CONTAINER:
        return type == CTX_TYPE_CONTAINER;
      case CTX_F_TARGETUSER:
        return type == CTX_TYPE_ELEVATE || type == CTX_TYPE_CHPRIV ||
               type == CTX_TYPE_VM || type == CTX_TYPE_CONTAINER ||
               type == CTX_TYPE_REMOTE || type == CTX_TYPE_SESSION;
      case CTX_F_TARGETHOST:
        return type == CTX_TYPE_REMOTE;
      case CTX_F_SESSIONID:
        return type == CTX_TYPE_SESSION;
      default:
        return true;
    }
}

static void context_free(CtxContext *c)
{
    int i;
    for (i = 0; i < CTX_F_COUNT; i++) {
        free(c->field[i]);
        c->field[i] = NULL;
    }
}

void ctx_init(CtxStack *s)
{
    memset(s, 0, sizeof(*s));
}

void ctx_clear(CtxStack *s)
{
    while (s->depth > 0)
        context_free(&s->ctx[--s->depth]);
}

void ctx_event_free(CtxEvent *ev)
{
    context_free(&ev->ended);
}

const CtxContext *ctx_active(const CtxStack *s)
{
    return s->depth > 0 ? &s->ctx[s->depth - 1] : NULL;
}

/* Valid UTF-8 without control characters (C0, DEL, C1): what may reach a
 * title or a tooltip. */
static bool utf8_clean(const unsigned char *p, size_t n)
{
    size_t i = 0;
    while (i < n) {
        unsigned c = p[i], cp;
        size_t k, more;
        if (c < 0x80) {
            if (c < 0x20 || c == 0x7F)
                return false;
            i++;
            continue;
        }
        if ((c & 0xE0) == 0xC0) { more = 1; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { more = 2; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { more = 3; cp = c & 0x07; }
        else return false;
        if (n - i <= more)
            return false;
        for (k = 1; k <= more; k++) {
            if ((p[i + k] & 0xC0) != 0x80)
                return false;
            cp = (cp << 6) | (p[i + k] & 0x3F);
        }
        if ((more == 1 && cp < 0x80) || (more == 2 && cp < 0x800) ||
            (more == 3 && cp < 0x10000) || cp > 0x10FFFF ||
            (cp >= 0xD800 && cp < 0xE000) || (cp >= 0x80 && cp < 0xA0))
            return false;
        i += more + 1;
    }
    return true;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* A field value: \xHH escapes decoded (the specification sends ';' and '\'
 * this way), at most CTX_FIELD_MAX bytes, valid UTF-8, no control
 * character raw or escaped. Returns a new string, or NULL: the field is
 * ignored. */
static char *decode_value(const char *v, size_t n)
{
    char buf[CTX_FIELD_MAX + 1];
    size_t i, o = 0;
    char *r;

    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)v[i];
        if (c == '\\') {
            int h, l;
            if (i + 3 >= n)
                return NULL;
            if (v[i + 1] != 'x' || (h = hexval(v[i + 2])) < 0 ||
                (l = hexval(v[i + 3])) < 0)
                return NULL;
            c = (unsigned char)(h * 16 + l);
            i += 3;
        } else if (c < 0x20 || c == 0x7F) {
            return NULL;
        }
        if (o >= CTX_FIELD_MAX)
            return NULL;
        buf[o++] = (char)c;
    }
    if (!utf8_clean((const unsigned char *)buf, o))
        return NULL;
    r = malloc(o + 1);
    if (!r)
        return NULL;
    memcpy(r, buf, o);
    r[o] = '\0';
    return r;
}

static bool all_chars(const char *s, const char *set, size_t min, size_t max)
{
    size_t n = strlen(s);
    if (n < min || n > max)
        return false;
    return strspn(s, set) == n;
}

/* A 128-bit id as systemd writes it: 32 hex digits, or the UUID form
 * 8-4-4-4-12. */
static bool id128_ok(const char *v)
{
    static const char hex[] = "0123456789abcdefABCDEF";
    size_t i, n = strlen(v);
    if (n == 32)
        return strspn(v, hex) == 32;
    if (n != 36)
        return false;
    for (i = 0; i < 36; i++) {
        bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        if (dash ? v[i] != '-' : !strchr(hex, v[i]))
            return false;
    }
    return true;
}

/* The format a field's value must have (the specification's ABNF). */
static bool field_value_ok(int field, const char *v)
{
    switch (field) {
      case CTX_F_MACHINEID:
      case CTX_F_BOOTID:
        return id128_ok(v);
      case CTX_F_PID:
      case CTX_F_PIDFDID:
        return all_chars(v, "0123456789", 1, 20);
      case CTX_F_CMDLINE:
        return true;                   /* may be empty */
      default:
        return *v != '\0';
    }
}

static bool id_ok(const char *id, size_t n)
{
    size_t i;
    if (n < 1 || n > CTX_ID_MAX)
        return false;
    for (i = 0; i < n; i++)
        if ((unsigned char)id[i] < 32 || (unsigned char)id[i] > 126)
            return false;
    return true;
}

/* The ';'-separated segments after the first one, as (key, value) pairs;
 * calls fn for each well-formed one. */
typedef void (*pair_fn)(void *ctx, const char *k, size_t kn,
                        const char *v, size_t vn);
static void each_pair(const char *s, size_t n, pair_fn fn, void *ctx)
{
    size_t i = 0;
    while (i < n) {
        const char *seg = s + i, *eq;
        size_t sn;
        const char *semi = memchr(seg, ';', n - i);
        sn = semi ? (size_t)(semi - seg) : n - i;
        eq = memchr(seg, '=', sn);
        if (eq && eq > seg)
            fn(ctx, seg, (size_t)(eq - seg), eq + 1, sn - (size_t)(eq - seg) - 1);
        i += sn + 1;
    }
}

static bool key_is(const char *k, size_t kn, const char *name)
{
    return strlen(name) == kn && !memcmp(k, name, kn);
}

static void pair_type(void *vctx, const char *k, size_t kn,
                      const char *v, size_t vn)
{
    int *type = (int *)vctx, t;
    if (!key_is(k, kn, "type"))
        return;
    *type = CTX_TYPE_NONE;             /* the last type= wins, known or not */
    for (t = 1; t < CTX_TYPE_COUNT; t++)
        if (key_is(v, vn, type_names[t]))
            *type = t;
}

static void pair_field(void *vctx, const char *k, size_t kn,
                       const char *v, size_t vn)
{
    CtxContext *c = (CtxContext *)vctx;
    int f;
    char *val;
    for (f = 0; f < CTX_F_COUNT; f++)
        if (key_is(k, kn, field_names[f]))
            break;
    if (f == CTX_F_COUNT || !field_applies(f, c->type))
        return;
    val = decode_value(v, vn);
    if (!val)
        return;
    if (!field_value_ok(f, val)) {
        free(val);
        return;
    }
    free(c->field[f]);                 /* the last one wins */
    c->field[f] = val;
}

static void pair_end(void *vctx, const char *k, size_t kn,
                     const char *v, size_t vn)
{
    CtxEvent *ev = (CtxEvent *)vctx;
    if (key_is(k, kn, "exit")) {
        int e;
        for (e = CTX_EXIT_SUCCESS; e <= CTX_EXIT_INTERRUPT; e++)
            if (key_is(v, vn, exit_names[e]))
                ev->exit = e;
    } else if (key_is(k, kn, "status")) {
        int st = 0;
        size_t i;
        if (vn < 1 || vn > 3)
            return;
        for (i = 0; i < vn; i++) {
            if (v[i] < '0' || v[i] > '9')
                return;
            st = st * 10 + (v[i] - '0');
        }
        if (st <= 255)
            ev->status = st;
    } else if (key_is(k, kn, "signal")) {
        size_t i;
        if (vn < 4 || vn > CTX_SIGNAL_MAX || memcmp(v, "SIG", 3) != 0)
            return;
        for (i = 3; i < vn; i++)
            if (!((v[i] >= 'A' && v[i] <= 'Z') || (v[i] >= '0' && v[i] <= '9') ||
                  v[i] == '+' || v[i] == '-'))
                return;
        memcpy(ev->signal, v, vn);
        ev->signal[vn] = '\0';
    }
}

static int find_id(const CtxStack *s, const char *id, size_t n)
{
    int i;
    for (i = 0; i < s->depth; i++)
        if (strlen(s->ctx[i].id) == n && !memcmp(s->ctx[i].id, id, n))
            return i;
    return -1;
}

/* End every context above index i (exclusive); returns how many. */
static int end_above(CtxStack *s, int i)
{
    int n = 0;
    while (s->depth > i + 1) {
        context_free(&s->ctx[--s->depth]);
        n++;
    }
    return n;
}

int ctx_apply(CtxStack *s, const char *str, size_t len, CtxEvent *ev)
{
    CtxEvent dummy;
    const char *semi, *id, *rest;
    size_t first, idn, restn;
    bool start;
    int at;

    if (!ev)
        ev = &dummy;
    memset(ev, 0, sizeof(*ev));
    ev->status = -1;
    ev->what = CTX_IGNORED;
    if (len > CTX_SEQ_MAX)
        return CTX_IGNORED;
    semi = memchr(str, ';', len);
    first = semi ? (size_t)(semi - str) : len;
    rest = semi ? semi + 1 : str + len;
    restn = semi ? len - first - 1 : 0;
    if (first >= 6 && !memcmp(str, "start=", 6)) {
        start = true;
        id = str + 6;
        idn = first - 6;
    } else if (first >= 4 && !memcmp(str, "end=", 4)) {
        start = false;
        id = str + 4;
        idn = first - 4;
    } else {
        return CTX_IGNORED;
    }
    if (!id_ok(id, idn))
        return CTX_IGNORED;
    at = find_id(s, id, idn);

    if (start) {
        CtxContext c;
        memset(&c, 0, sizeof(c));
        memcpy(c.id, id, idn);
        c.id[idn] = '\0';
        each_pair(rest, restn, pair_type, &c.type);
        each_pair(rest, restn, pair_field, &c);
        if (at >= 0) {
            ev->children = end_above(s, at);
            context_free(&s->ctx[at]);
            s->ctx[at] = c;
            ev->what = CTX_UPDATED;
        } else if (s->depth >= CTX_DEPTH_MAX) {
            context_free(&c);
            s->dropped++;
            ev->what = CTX_DROPPED;
        } else {
            s->ctx[s->depth++] = c;
            ev->what = CTX_STARTED;
        }
        if (ev == &dummy)
            ctx_event_free(ev);
        return ev->what;
    }

    if (at < 0)
        return CTX_IGNORED;
    each_pair(rest, restn, pair_end, ev);
    ev->children = end_above(s, at);
    ev->ended = s->ctx[at];
    memset(&s->ctx[at], 0, sizeof(s->ctx[at]));
    s->depth = at;
    ev->what = CTX_ENDED;
    if (ev == &dummy)
        ctx_event_free(ev);
    return CTX_ENDED;
}

const char *ctx_effective_user(const CtxStack *s)
{
    const char *u = NULL;
    int i;
    for (i = 0; i < s->depth; i++) {
        const CtxContext *c = &s->ctx[i];
        if (c->field[CTX_F_TARGETUSER])
            u = c->field[CTX_F_TARGETUSER];
        else if (c->type == CTX_TYPE_ELEVATE)
            u = "root";
        else if (c->field[CTX_F_USER])
            u = c->field[CTX_F_USER];
    }
    return u;
}

bool ctx_elevated(const CtxStack *s)
{
    const char *u = ctx_effective_user(s);
    return u && !strcmp(u, "root");
}

/* The same walk as ctx_effective_user, remembering whether the user came
 * from a switch. */
bool ctx_moved_machine(const CtxStack *s, const char **where)
{
    const CtxContext *outer = NULL, *inner = NULL;
    int i;

    if (where)
        *where = NULL;
    for (i = s->depth - 1; i >= 0; i--) {
        if (s->ctx[i].type == CTX_TYPE_REMOTE) {
            if (where)
                *where = s->ctx[i].field[CTX_F_TARGETHOST];
            return true;
        }
    }
    for (i = 0; i < s->depth; i++) {
        if (s->ctx[i].field[CTX_F_MACHINEID]) {
            if (!outer)
                outer = &s->ctx[i];
            inner = &s->ctx[i];
        }
    }
    if (!outer || inner == outer ||
        !strcmp(outer->field[CTX_F_MACHINEID], inner->field[CTX_F_MACHINEID]))
        return false;
    if (where)
        *where = inner->field[CTX_F_HOSTNAME];
    return true;
}

CtxAs ctx_runs_as(const CtxStack *s)
{
    const char *u = NULL;
    bool switched = false;
    int i;

    for (i = 0; i < s->depth; i++) {
        const CtxContext *c = &s->ctx[i];
        if (c->field[CTX_F_TARGETUSER]) {
            u = c->field[CTX_F_TARGETUSER];
            switched = true;
        } else if (c->type == CTX_TYPE_ELEVATE) {
            u = "root";
            switched = true;
        } else if (c->field[CTX_F_USER]) {
            if (!u || strcmp(u, c->field[CTX_F_USER]))
                switched = false;
            u = c->field[CTX_F_USER];
        }
    }
    if (!u)
        return CTX_AS_PLAIN;
    if (!strcmp(u, "root"))
        return CTX_AS_ROOT;
    return switched ? CTX_AS_SWITCHED : CTX_AS_PLAIN;
}
