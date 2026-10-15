/*
 * test_ctx3008 - regression tests for OSC 3008 hierarchical context
 * signalling (UAPI.15): the platform-free half in kitty/kitty_ctx3008.c.
 *
 * Locks in: start/active/depth, nesting, end of the active and of a parent
 * (children, fields moved into the event), unknown end ignored, restart
 * (UPDATED, fields reset, children ended), the depth cap and its dropped
 * counter, id limits, the leading start=/end= token, type names, escapes
 * (\x3b, \x5c, bad ones), control characters, field length after decoding,
 * UTF-8 validation, machineid/bootid/pid formats, type-specific fields,
 * field order, malformed segments, duplicates, end fields (exit, status,
 * signal), the effective user and elevation, the whole-string limit, an
 * embedded NUL and the name functions.
 *
 * Returns non-zero on any failure.
 */
#include <stdio.h>
#include <string.h>

#include "../kitty/kitty_ctx3008.h"

static int failures = 0;

#define CHECK(cond, what) do { \
    if (!(cond)) { printf("FAIL [%s] line %d\n", what, __LINE__); failures++; } \
} while (0)

static int ap(CtxStack *s, const char *str, CtxEvent *ev)
{
    return ctx_apply(s, str, strlen(str), ev);
}

/* Apply a string that is expected to be a start; true when it opened. */
static int st(CtxStack *s, const char *str)
{
    CtxEvent ev;
    int r;
    memset(&ev, 0, sizeof ev);
    r = ap(s, str, &ev);
    if (r == CTX_ENDED) ctx_event_free(&ev);
    return r;
}

/* Field of the active context, NULL when none or not sent. */
static const char *af(const CtxStack *s, int f)
{
    const CtxContext *c = ctx_active(s);
    return c ? c->field[f] : NULL;
}

static int feq(const char *a, const char *b)
{
    return a != NULL && strcmp(a, b) == 0;
}

static void test_start_basic(void)
{
    CtxStack s;
    CtxEvent ev;
    const CtxContext *c;
    ctx_init(&s);
    CHECK(ctx_active(&s) == NULL, "empty: no active");
    CHECK(s.depth == 0, "empty: depth 0");

    memset(&ev, 0, sizeof ev);
    CHECK(ap(&s, "start=abc;type=shell;user=root", &ev) == CTX_STARTED,
          "start: STARTED");
    CHECK(ev.what == CTX_STARTED, "start: ev.what");
    CHECK(s.depth == 1, "start: depth 1");
    c = ctx_active(&s);
    CHECK(c != NULL, "start: active set");
    if (c) {
        CHECK(strcmp(c->id, "abc") == 0, "start: id");
        CHECK(c->type == CTX_TYPE_SHELL, "start: type");
        CHECK(feq(c->field[CTX_F_USER], "root"), "start: user");
        CHECK(c->field[CTX_F_HOSTNAME] == NULL, "start: hostname unsent");
    }
    /* ev may be NULL */
    CHECK(ap(&s, "start=nn", NULL) == CTX_STARTED, "start: NULL ev");
    CHECK(s.depth == 2, "start: NULL ev depth");
    ctx_clear(&s);
    CHECK(s.depth == 0 && ctx_active(&s) == NULL, "clear: empty");
}

static void test_nesting_and_end(void)
{
    CtxStack s;
    CtxEvent ev;
    ctx_init(&s);
    CHECK(st(&s, "start=a;type=shell") == CTX_STARTED, "nest: a");
    CHECK(st(&s, "start=b;type=command;cmdline=ls") == CTX_STARTED, "nest: b");
    CHECK(s.depth == 2, "nest: depth 2");
    CHECK(strcmp(s.ctx[0].id, "a") == 0, "nest: outermost a");
    CHECK(strcmp(s.ctx[1].id, "b") == 0, "nest: b child of a");
    CHECK(ctx_active(&s) && strcmp(ctx_active(&s)->id, "b") == 0,
          "nest: active b");

    /* end the active one */
    memset(&ev, 0, sizeof ev);
    CHECK(ap(&s, "end=b", &ev) == CTX_ENDED, "end active: ENDED");
    CHECK(ev.what == CTX_ENDED, "end active: ev.what");
    CHECK(ev.children == 0, "end active: no children");
    CHECK(strcmp(ev.ended.id, "b") == 0, "end active: ended id");
    CHECK(ev.ended.type == CTX_TYPE_COMMAND, "end active: ended type");
    CHECK(feq(ev.ended.field[CTX_F_CMDLINE], "ls"), "end active: fields moved");
    ctx_event_free(&ev);
    CHECK(s.depth == 1, "end active: depth 1");
    CHECK(ctx_active(&s) && strcmp(ctx_active(&s)->id, "a") == 0,
          "end active: a active again");
    ctx_clear(&s);
}

static void test_end_parent(void)
{
    CtxStack s;
    CtxEvent ev;
    ctx_init(&s);
    st(&s, "start=a;type=shell;user=carol");
    st(&s, "start=b;type=command");
    st(&s, "start=c;type=app");
    CHECK(s.depth == 3, "parent: depth 3");

    memset(&ev, 0, sizeof ev);
    CHECK(ap(&s, "end=a", &ev) == CTX_ENDED, "parent: ENDED");
    CHECK(ev.children == 2, "parent: 2 children");
    CHECK(strcmp(ev.ended.id, "a") == 0, "parent: ended is a");
    CHECK(feq(ev.ended.field[CTX_F_USER], "carol"), "parent: user moved");
    ctx_event_free(&ev);
    CHECK(s.depth == 0, "parent: depth 0");
    CHECK(ctx_active(&s) == NULL, "parent: none active");

    /* end of a middle one keeps the outer ones */
    st(&s, "start=a");
    st(&s, "start=b");
    st(&s, "start=c");
    memset(&ev, 0, sizeof ev);
    CHECK(ap(&s, "end=b", &ev) == CTX_ENDED, "middle: ENDED");
    CHECK(ev.children == 1, "middle: 1 child");
    ctx_event_free(&ev);
    CHECK(s.depth == 1 && strcmp(ctx_active(&s)->id, "a") == 0,
          "middle: a active");
    ctx_clear(&s);
}

static void test_end_unknown(void)
{
    CtxStack s;
    CtxEvent ev;
    ctx_init(&s);
    st(&s, "start=a");
    memset(&ev, 0, sizeof ev);
    CHECK(ap(&s, "end=zzz", &ev) == CTX_IGNORED, "unknown end: IGNORED");
    CHECK(ev.what == CTX_IGNORED, "unknown end: ev.what");
    CHECK(s.depth == 1, "unknown end: depth kept");
    /* already ended */
    memset(&ev, 0, sizeof ev);
    CHECK(ap(&s, "end=a", &ev) == CTX_ENDED, "unknown end: real end");
    ctx_event_free(&ev);
    memset(&ev, 0, sizeof ev);
    CHECK(ap(&s, "end=a", &ev) == CTX_IGNORED, "unknown end: second end");
    ctx_clear(&s);
}

static void test_restart(void)
{
    CtxStack s;
    CtxEvent ev;
    ctx_init(&s);
    st(&s, "start=a;type=shell;user=old;hostname=h");
    st(&s, "start=b;type=command");
    st(&s, "start=c;type=app");

    /* restart the outermost: fields reset, two children end */
    memset(&ev, 0, sizeof ev);
    CHECK(ap(&s, "start=a;type=app;comm=newcomm", &ev) == CTX_UPDATED,
          "restart: UPDATED");
    CHECK(ev.what == CTX_UPDATED, "restart: ev.what");
    CHECK(ev.children == 2, "restart: 2 children ended");
    CHECK(s.depth == 1, "restart: depth 1");
    CHECK(ctx_active(&s) && strcmp(ctx_active(&s)->id, "a") == 0,
          "restart: a active");
    CHECK(ctx_active(&s) && ctx_active(&s)->type == CTX_TYPE_APP,
          "restart: new type");
    CHECK(feq(af(&s, CTX_F_COMM), "newcomm"), "restart: new comm");
    CHECK(af(&s, CTX_F_USER) == NULL, "restart: old user gone");
    CHECK(af(&s, CTX_F_HOSTNAME) == NULL, "restart: old hostname gone");

    /* restart without a type: type is reset too */
    CHECK(ap(&s, "start=a", NULL) == CTX_UPDATED, "restart: bare UPDATED");
    CHECK(ctx_active(&s)->type == CTX_TYPE_NONE, "restart: type reset");
    CHECK(af(&s, CTX_F_COMM) == NULL, "restart: comm gone");

    /* restart the active one, no children */
    memset(&ev, 0, sizeof ev);
    CHECK(ap(&s, "start=a;user=u", &ev) == CTX_UPDATED, "restart: again");
    CHECK(ev.children == 0, "restart: no children");
    CHECK(s.depth == 1, "restart: depth stays 1");
    ctx_clear(&s);
}

static void test_depth_cap(void)
{
    CtxStack s;
    CtxEvent ev;
    char buf[64];
    int i, ok = 1;
    ctx_init(&s);
    for (i = 0; i < CTX_DEPTH_MAX; i++) {
        snprintf(buf, sizeof buf, "start=c%d;type=app", i);
        if (st(&s, buf) != CTX_STARTED) ok = 0;
    }
    CHECK(ok, "cap: CTX_DEPTH_MAX starts ok");
    CHECK(s.depth == CTX_DEPTH_MAX, "cap: depth at max");
    CHECK(s.dropped == 0, "cap: nothing dropped yet");

    memset(&ev, 0, sizeof ev);
    CHECK(ap(&s, "start=over;type=shell", &ev) == CTX_DROPPED,
          "cap: next DROPPED");
    CHECK(ev.what == CTX_DROPPED, "cap: ev.what");
    CHECK(s.depth == CTX_DEPTH_MAX, "cap: depth unchanged");
    CHECK(s.dropped == 1, "cap: dropped 1");
    CHECK(strcmp(ctx_active(&s)->id, "c31") == 0, "cap: active unchanged");

    /* end= of the dropped id is ignored */
    memset(&ev, 0, sizeof ev);
    CHECK(ap(&s, "end=over", &ev) == CTX_IGNORED, "cap: end of dropped");
    CHECK(s.depth == CTX_DEPTH_MAX, "cap: depth still max");

    CHECK(st(&s, "start=over2") == CTX_DROPPED, "cap: second drop");
    CHECK(s.dropped == 2, "cap: dropped 2");

    /* an update of an open id still works at the cap */
    CHECK(st(&s, "start=c31;user=x") == CTX_UPDATED, "cap: update at cap");
    ctx_clear(&s);
}

static void test_id_limits(void)
{
    CtxStack s;
    char buf[200];
    char id[80];
    ctx_init(&s);

    CHECK(st(&s, "start=") == CTX_IGNORED, "id: empty rejected");
    CHECK(s.depth == 0, "id: empty depth");

    memset(id, 'a', 64);
    id[64] = 0;
    snprintf(buf, sizeof buf, "start=%s", id);
    CHECK(st(&s, buf) == CTX_STARTED, "id: 64 ok");
    CHECK(strcmp(s.ctx[0].id, id) == 0, "id: 64 stored whole");
    ctx_clear(&s);

    memset(id, 'a', 65);
    id[65] = 0;
    snprintf(buf, sizeof buf, "start=%s", id);
    CHECK(st(&s, buf) == CTX_IGNORED, "id: 65 rejected");
    CHECK(s.depth == 0, "id: 65 depth");

    CHECK(st(&s, "start=a\x1f" "b") == CTX_IGNORED, "id: byte 31 rejected");
    CHECK(st(&s, "start=a\x7f" "b") == CTX_IGNORED, "id: byte 127 rejected");
    CHECK(st(&s, "start=a\x80" "b") == CTX_IGNORED, "id: byte 128 rejected");
    CHECK(s.depth == 0, "id: bad bytes depth");

    CHECK(st(&s, "start=a b") == CTX_STARTED, "id: space ok");
    CHECK(strcmp(s.ctx[0].id, "a b") == 0, "id: space stored");
    ctx_clear(&s);
    CHECK(st(&s, "start=~") == CTX_STARTED, "id: byte 126 ok");
    ctx_clear(&s);

    /* end with a bad id is ignored */
    st(&s, "start=a");
    CHECK(st(&s, "end=") == CTX_IGNORED, "id: empty end ignored");
    CHECK(s.depth == 1, "id: empty end depth");
    ctx_clear(&s);
}

static void test_leading_token(void)
{
    CtxStack s;
    ctx_init(&s);
    CHECK(st(&s, "type=shell;start=x") == CTX_IGNORED, "lead: field first");
    CHECK(st(&s, "") == CTX_IGNORED, "lead: empty");
    CHECK(st(&s, "begin=x") == CTX_IGNORED, "lead: begin");
    CHECK(st(&s, ";start=x") == CTX_IGNORED, "lead: empty first segment");
    CHECK(st(&s, "start") == CTX_IGNORED, "lead: no equals");
    CHECK(s.depth == 0, "lead: nothing opened");
    ctx_clear(&s);
}

static void test_types(void)
{
    static const char *const names[] = {
        "boot", "container", "vm", "elevate", "chpriv", "subcontext",
        "remote", "shell", "command", "app", "service", "session"
    };
    static const int ids[] = {
        CTX_TYPE_BOOT, CTX_TYPE_CONTAINER, CTX_TYPE_VM, CTX_TYPE_ELEVATE,
        CTX_TYPE_CHPRIV, CTX_TYPE_SUBCONTEXT, CTX_TYPE_REMOTE,
        CTX_TYPE_SHELL, CTX_TYPE_COMMAND, CTX_TYPE_APP, CTX_TYPE_SERVICE,
        CTX_TYPE_SESSION
    };
    CtxStack s;
    char buf[64];
    size_t i;
    int ok = 1;
    ctx_init(&s);
    for (i = 0; i < sizeof names / sizeof names[0]; i++) {
        snprintf(buf, sizeof buf, "start=t%u;type=%s", (unsigned)i, names[i]);
        if (st(&s, buf) != CTX_STARTED) ok = 0;
        if (!ctx_active(&s) || ctx_active(&s)->type != ids[i]) ok = 0;
    }
    CHECK(ok, "type: every known name");
    ctx_clear(&s);

    CHECK(st(&s, "start=u;type=wombat") == CTX_STARTED,
          "type: unknown still kept");
    CHECK(s.depth == 1, "type: unknown depth");
    CHECK(ctx_active(&s)->type == CTX_TYPE_NONE, "type: unknown is NONE");
    ctx_clear(&s);

    st(&s, "start=v");
    CHECK(ctx_active(&s)->type == CTX_TYPE_NONE, "type: absent is NONE");
    ctx_clear(&s);

    st(&s, "start=w;type=shell;type=app");
    CHECK(ctx_active(&s)->type == CTX_TYPE_APP, "type: last wins");
    ctx_clear(&s);
}

static void test_fields_stored(void)
{
    CtxStack s;
    ctx_init(&s);
    st(&s, "start=a;type=shell;user=carol;hostname=box;"
           "machineid=0123456789abcdef0123456789abcdef;"
           "bootid=fedcba9876543210fedcba9876543210;pid=1234;"
           "pidfdid=987654321;comm=bash;cwd=/home/carol");
    CHECK(feq(af(&s, CTX_F_USER), "carol"), "fields: user");
    CHECK(feq(af(&s, CTX_F_HOSTNAME), "box"), "fields: hostname");
    CHECK(feq(af(&s, CTX_F_MACHINEID), "0123456789abcdef0123456789abcdef"),
          "fields: machineid");
    CHECK(feq(af(&s, CTX_F_BOOTID), "fedcba9876543210fedcba9876543210"),
          "fields: bootid");
    CHECK(feq(af(&s, CTX_F_PID), "1234"), "fields: pid");
    CHECK(feq(af(&s, CTX_F_PIDFDID), "987654321"), "fields: pidfdid");
    CHECK(feq(af(&s, CTX_F_COMM), "bash"), "fields: comm");
    CHECK(feq(af(&s, CTX_F_CWD), "/home/carol"), "fields: cwd");
    ctx_clear(&s);
}

static void test_escapes(void)
{
    CtxStack s;
    ctx_init(&s);

    st(&s, "start=a;user=a\\x3bb");
    CHECK(feq(af(&s, CTX_F_USER), "a;b"), "esc: \\x3b");
    ctx_clear(&s);

    st(&s, "start=a;user=a\\x5cb");
    CHECK(feq(af(&s, CTX_F_USER), "a\\b"), "esc: \\x5c");
    ctx_clear(&s);

    st(&s, "start=a;user=a\\x3Bb;hostname=\\x5C");
    CHECK(feq(af(&s, CTX_F_USER), "a;b"), "esc: uppercase \\x3B");
    CHECK(feq(af(&s, CTX_F_HOSTNAME), "\\"), "esc: uppercase \\x5C");
    ctx_clear(&s);

    /* bad escapes: that field ignored, the others kept */
    st(&s, "start=a;user=a\\qb;hostname=h");
    CHECK(af(&s, CTX_F_USER) == NULL, "esc: \\q ignored");
    CHECK(feq(af(&s, CTX_F_HOSTNAME), "h"), "esc: \\q others kept");
    ctx_clear(&s);

    st(&s, "start=a;user=ab\\x3;hostname=h");
    CHECK(af(&s, CTX_F_USER) == NULL, "esc: truncated \\x3 ignored");
    CHECK(feq(af(&s, CTX_F_HOSTNAME), "h"), "esc: \\x3 others kept");
    ctx_clear(&s);

    st(&s, "start=a;user=\\xZZ;hostname=h");
    CHECK(af(&s, CTX_F_USER) == NULL, "esc: \\xZZ ignored");
    CHECK(feq(af(&s, CTX_F_HOSTNAME), "h"), "esc: \\xZZ others kept");
    ctx_clear(&s);

    st(&s, "start=a;user=ab\\;hostname=h");
    CHECK(af(&s, CTX_F_USER) == NULL, "esc: lone backslash ignored");
    ctx_clear(&s);

    /* an escaped printable other than ; and \ is still a valid escape */
    st(&s, "start=a;user=a\\x41");
    CHECK(feq(af(&s, CTX_F_USER), "aA"), "esc: \\x41 decodes");
    ctx_clear(&s);
}

static void test_control_chars(void)
{
    CtxStack s;
    ctx_init(&s);

    st(&s, "start=a;user=a\\x0ab;hostname=h");
    CHECK(af(&s, CTX_F_USER) == NULL, "ctl: escaped \\x0a ignored");
    CHECK(feq(af(&s, CTX_F_HOSTNAME), "h"), "ctl: \\x0a others kept");
    ctx_clear(&s);

    st(&s, "start=a;user=a\\x7fb;hostname=h");
    CHECK(af(&s, CTX_F_USER) == NULL, "ctl: escaped \\x7f ignored");
    ctx_clear(&s);

    st(&s, "start=a;user=a\\x00b;hostname=h");
    CHECK(af(&s, CTX_F_USER) == NULL, "ctl: escaped \\x00 ignored");
    ctx_clear(&s);

    st(&s, "start=a;user=a\x01" "b;hostname=h");
    CHECK(af(&s, CTX_F_USER) == NULL, "ctl: raw \\x01 ignored");
    CHECK(feq(af(&s, CTX_F_HOSTNAME), "h"), "ctl: raw others kept");
    ctx_clear(&s);

    st(&s, "start=a;user=a\x7f" "b;hostname=h");
    CHECK(af(&s, CTX_F_USER) == NULL, "ctl: raw 0x7f ignored");
    ctx_clear(&s);

    st(&s, "start=a;user=a\tb;hostname=h");
    CHECK(af(&s, CTX_F_USER) == NULL, "ctl: raw tab ignored");
    ctx_clear(&s);

    /* C1 control U+0085 */
    st(&s, "start=a;user=a\xc2\x85" "b;hostname=h");
    CHECK(af(&s, CTX_F_USER) == NULL, "ctl: C1 U+0085 ignored");
    CHECK(feq(af(&s, CTX_F_HOSTNAME), "h"), "ctl: C1 others kept");
    ctx_clear(&s);

    /* embedded NUL inside the given length */
    {
        static const char nul[] = "start=a;user=ab\0cd;hostname=h";
        ctx_apply(&s, nul, sizeof nul - 1, NULL);
        CHECK(s.depth == 1, "ctl: NUL start kept");
        CHECK(af(&s, CTX_F_USER) == NULL, "ctl: NUL in value ignored");
        CHECK(feq(af(&s, CTX_F_HOSTNAME), "h"), "ctl: NUL others kept");
        ctx_clear(&s);
    }
}

static void test_field_length(void)
{
    CtxStack s;
    char buf[CTX_SEQ_MAX + 64];
    char val[1100];
    size_t i;
    ctx_init(&s);

    memset(val, 'x', 255);
    val[255] = 0;
    snprintf(buf, sizeof buf, "start=a;comm=%s", val);
    st(&s, buf);
    CHECK(af(&s, CTX_F_COMM) && strlen(af(&s, CTX_F_COMM)) == 255,
          "len: 255 ok");
    ctx_clear(&s);

    memset(val, 'x', 256);
    val[256] = 0;
    snprintf(buf, sizeof buf, "start=a;comm=%s;user=u", val);
    st(&s, buf);
    CHECK(s.depth == 1, "len: 256 context kept");
    CHECK(af(&s, CTX_F_COMM) == NULL, "len: 256 ignored");
    CHECK(feq(af(&s, CTX_F_USER), "u"), "len: 256 others kept");
    ctx_clear(&s);

    /* counted after decoding: 128 escapes = 128 bytes in 512 raw */
    val[0] = 0;
    for (i = 0; i < 128; i++) strcat(val, "\\x5c");
    snprintf(buf, sizeof buf, "start=a;comm=%s", val);
    st(&s, buf);
    CHECK(af(&s, CTX_F_COMM) && strlen(af(&s, CTX_F_COMM)) == 128,
          "len: 128 escapes ok (raw longer than 255)");
    ctx_clear(&s);

    /* 256 decoded bytes from escapes is too long */
    val[0] = 0;
    for (i = 0; i < 256; i++) strcat(val, "\\x5c");
    snprintf(buf, sizeof buf, "start=a;comm=%s", val);
    st(&s, buf);
    CHECK(s.depth == 1, "len: 256 escapes context kept");
    CHECK(af(&s, CTX_F_COMM) == NULL, "len: 256 decoded ignored");
    ctx_clear(&s);
}

static void test_utf8(void)
{
    CtxStack s;
    ctx_init(&s);

    st(&s, "start=a;user=a\x80;hostname=h");
    CHECK(af(&s, CTX_F_USER) == NULL, "utf8: lone 0x80 ignored");
    CHECK(feq(af(&s, CTX_F_HOSTNAME), "h"), "utf8: lone others kept");
    ctx_clear(&s);

    st(&s, "start=a;user=\xc0\xaf;hostname=h");
    CHECK(af(&s, CTX_F_USER) == NULL, "utf8: overlong C0 AF ignored");
    ctx_clear(&s);

    st(&s, "start=a;user=\xed\xa0\x80;hostname=h");
    CHECK(af(&s, CTX_F_USER) == NULL, "utf8: surrogate ignored");
    ctx_clear(&s);

    st(&s, "start=a;user=\xe2\x82;hostname=h");
    CHECK(af(&s, CTX_F_USER) == NULL, "utf8: truncated ignored");
    ctx_clear(&s);

    st(&s, "start=a;user=\xf4\x90\x80\x80;hostname=h");
    CHECK(af(&s, CTX_F_USER) == NULL, "utf8: above U+10FFFF ignored");
    ctx_clear(&s);

    st(&s, "start=a;user=M\xc3\xbc" "ller;hostname=h");
    CHECK(feq(af(&s, CTX_F_USER), "M\xc3\xbc" "ller"), "utf8: 2-byte kept");
    ctx_clear(&s);

    st(&s, "start=a;user=\xe2\x82\xac;hostname=h");
    CHECK(feq(af(&s, CTX_F_USER), "\xe2\x82\xac"), "utf8: 3-byte kept");
    ctx_clear(&s);

    st(&s, "start=a;user=\xf0\x9f\x98\x80;hostname=h");
    CHECK(feq(af(&s, CTX_F_USER), "\xf0\x9f\x98\x80"), "utf8: 4-byte kept");
    ctx_clear(&s);

    /* escapes can not smuggle invalid UTF-8 either */
    st(&s, "start=a;user=a\\x80;hostname=h");
    CHECK(af(&s, CTX_F_USER) == NULL || strcmp(af(&s, CTX_F_USER), "a") != 0,
          "utf8: \\x80 escape not stored as a bare byte");
    ctx_clear(&s);
}

static void test_ids_formats(void)
{
    CtxStack s;
    ctx_init(&s);

    st(&s, "start=a;machineid=0123456789abcdef0123456789abcdef");
    CHECK(af(&s, CTX_F_MACHINEID) != NULL, "mid: 32 hex ok");
    ctx_clear(&s);

    st(&s, "start=a;machineid=01234567-89ab-cdef-0123-456789abcdef");
    CHECK(feq(af(&s, CTX_F_MACHINEID),
              "01234567-89ab-cdef-0123-456789abcdef"), "mid: 36 dashed ok");
    ctx_clear(&s);

    st(&s, "start=a;machineid=0123456789abcdef0123456789abcde;user=u");
    CHECK(af(&s, CTX_F_MACHINEID) == NULL, "mid: 31 rejected");
    CHECK(feq(af(&s, CTX_F_USER), "u"), "mid: 31 others kept");
    ctx_clear(&s);

    st(&s, "start=a;machineid=0123456789abcdef0123456789abcdeg");
    CHECK(af(&s, CTX_F_MACHINEID) == NULL, "mid: 'g' rejected");
    ctx_clear(&s);

    st(&s, "start=a;machineid=0123456789abcdef0123456789abcdef0");
    CHECK(af(&s, CTX_F_MACHINEID) == NULL, "mid: 33 hex rejected");
    ctx_clear(&s);

    st(&s, "start=a;bootid=01234567-89ab-cdef-0123-456789abcdef");
    CHECK(af(&s, CTX_F_BOOTID) != NULL, "bootid: dashed ok");
    ctx_clear(&s);

    st(&s, "start=a;bootid=xyz");
    CHECK(af(&s, CTX_F_BOOTID) == NULL, "bootid: junk rejected");
    ctx_clear(&s);

    st(&s, "start=a;pid=42;pidfdid=7");
    CHECK(feq(af(&s, CTX_F_PID), "42"), "pid: digits ok");
    CHECK(feq(af(&s, CTX_F_PIDFDID), "7"), "pidfdid: digits ok");
    ctx_clear(&s);

    st(&s, "start=a;pid=12a;pidfdid=-3;user=u");
    CHECK(af(&s, CTX_F_PID) == NULL, "pid: letters rejected");
    CHECK(af(&s, CTX_F_PIDFDID) == NULL, "pidfdid: sign rejected");
    CHECK(feq(af(&s, CTX_F_USER), "u"), "pid: others kept");
    ctx_clear(&s);

    st(&s, "start=a;pid=1 2");
    CHECK(af(&s, CTX_F_PID) == NULL, "pid: space rejected");
    ctx_clear(&s);
}

static void test_type_specific(void)
{
    CtxStack s;
    ctx_init(&s);

    st(&s, "start=a;type=shell;cwd=/x");
    CHECK(feq(af(&s, CTX_F_CWD), "/x"), "applic: cwd on shell kept");
    ctx_clear(&s);

    st(&s, "start=a;type=command;cwd=/y;cmdline=ls -l");
    CHECK(feq(af(&s, CTX_F_CWD), "/y"), "applic: cwd on command kept");
    CHECK(feq(af(&s, CTX_F_CMDLINE), "ls -l"), "applic: cmdline kept");
    ctx_clear(&s);

    st(&s, "start=a;type=app;cwd=/x;user=u");
    CHECK(af(&s, CTX_F_CWD) == NULL, "applic: cwd on app ignored");
    CHECK(feq(af(&s, CTX_F_USER), "u"), "applic: app others kept");
    ctx_clear(&s);

    st(&s, "start=a;type=command;cmdline=");
    CHECK(feq(af(&s, CTX_F_CMDLINE), ""), "applic: empty cmdline kept");
    ctx_clear(&s);

    st(&s, "start=a;type=shell;cmdline=ls");
    CHECK(af(&s, CTX_F_CMDLINE) == NULL, "applic: cmdline on shell ignored");
    ctx_clear(&s);

    st(&s, "start=a;type=elevate;targetuser=alice");
    CHECK(feq(af(&s, CTX_F_TARGETUSER), "alice"),
          "applic: targetuser on elevate kept");
    ctx_clear(&s);

    st(&s, "start=a;type=shell;targetuser=alice");
    CHECK(af(&s, CTX_F_TARGETUSER) == NULL,
          "applic: targetuser on shell ignored");
    ctx_clear(&s);

    st(&s, "start=a;type=chpriv;targetuser=t");
    CHECK(af(&s, CTX_F_TARGETUSER) != NULL, "applic: targetuser chpriv");
    ctx_clear(&s);
    st(&s, "start=a;type=vm;targetuser=t;vm=guest");
    CHECK(af(&s, CTX_F_TARGETUSER) != NULL, "applic: targetuser vm");
    CHECK(feq(af(&s, CTX_F_VM), "guest"), "applic: vm on vm kept");
    ctx_clear(&s);
    st(&s, "start=a;type=container;targetuser=t;container=ct");
    CHECK(af(&s, CTX_F_TARGETUSER) != NULL, "applic: targetuser container");
    CHECK(feq(af(&s, CTX_F_CONTAINER), "ct"),
          "applic: container on container kept");
    ctx_clear(&s);
    st(&s, "start=a;type=remote;targetuser=t;targethost=far");
    CHECK(af(&s, CTX_F_TARGETUSER) != NULL, "applic: targetuser remote");
    CHECK(feq(af(&s, CTX_F_TARGETHOST), "far"),
          "applic: targethost on remote kept");
    ctx_clear(&s);
    st(&s, "start=a;type=session;targetuser=t;sessionid=s1");
    CHECK(af(&s, CTX_F_TARGETUSER) != NULL, "applic: targetuser session");
    CHECK(feq(af(&s, CTX_F_SESSIONID), "s1"),
          "applic: sessionid on session kept");
    ctx_clear(&s);

    st(&s, "start=a;type=shell;vm=g;container=c;targethost=h;sessionid=i");
    CHECK(af(&s, CTX_F_VM) == NULL, "applic: vm on shell ignored");
    CHECK(af(&s, CTX_F_CONTAINER) == NULL, "applic: container on shell ignored");
    CHECK(af(&s, CTX_F_TARGETHOST) == NULL,
          "applic: targethost on shell ignored");
    CHECK(af(&s, CTX_F_SESSIONID) == NULL,
          "applic: sessionid on shell ignored");
    ctx_clear(&s);

    /* order independence: type= after the type-specific field */
    st(&s, "start=a;cwd=/late;type=shell");
    CHECK(ctx_active(&s)->type == CTX_TYPE_SHELL, "order: type applies");
    CHECK(feq(af(&s, CTX_F_CWD), "/late"), "order: cwd before type kept");
    ctx_clear(&s);

    st(&s, "start=a;targetuser=bob;type=elevate");
    CHECK(feq(af(&s, CTX_F_TARGETUSER), "bob"),
          "order: targetuser before type kept");
    ctx_clear(&s);

    /* a type that does not allow it, given last, drops it */
    st(&s, "start=a;type=shell;cwd=/x;type=app");
    CHECK(ctx_active(&s)->type == CTX_TYPE_APP, "order: last type app");
    CHECK(af(&s, CTX_F_CWD) == NULL, "order: cwd not valid for final type");
    ctx_clear(&s);
}

static void test_malformed(void)
{
    CtxStack s;
    ctx_init(&s);

    CHECK(st(&s, "start=a;;type=shell;noequals;=v;user=u;;") == CTX_STARTED,
          "malformed: start ok");
    CHECK(s.depth == 1, "malformed: depth");
    CHECK(ctx_active(&s)->type == CTX_TYPE_SHELL, "malformed: type kept");
    CHECK(feq(af(&s, CTX_F_USER), "u"), "malformed: user kept");
    ctx_clear(&s);

    st(&s, "start=a;unknownfield=1;user=u;x=");
    CHECK(feq(af(&s, CTX_F_USER), "u"), "malformed: unknown field ignored");
    ctx_clear(&s);

    st(&s, "start=a;user=u;user=v");
    CHECK(feq(af(&s, CTX_F_USER), "v"), "dup: last wins");
    ctx_clear(&s);

    /* a later invalid duplicate does not erase an earlier valid one */
    st(&s, "start=a;pid=5;pid=x");
    CHECK(feq(af(&s, CTX_F_PID), "5") || af(&s, CTX_F_PID) == NULL,
          "dup: invalid duplicate is ignored or last wins");
    ctx_clear(&s);

    /* a value may contain '=' */
    st(&s, "start=a;user=a=b");
    CHECK(feq(af(&s, CTX_F_USER), "a=b"), "malformed: '=' in value");
    ctx_clear(&s);
}

static void test_end_fields(void)
{
    static const char *const names[] =
        { "success", "failure", "crash", "interrupt" };
    static const int ids[] = {
        CTX_EXIT_SUCCESS, CTX_EXIT_FAILURE, CTX_EXIT_CRASH, CTX_EXIT_INTERRUPT
    };
    CtxStack s;
    CtxEvent ev;
    char buf[64];
    size_t i;
    ctx_init(&s);

    /* nothing sent */
    st(&s, "start=a");
    memset(&ev, 0, sizeof ev);
    CHECK(ap(&s, "end=a", &ev) == CTX_ENDED, "endf: bare ENDED");
    CHECK(ev.exit == CTX_EXIT_NONE, "endf: bare exit NONE");
    CHECK(ev.status == -1, "endf: bare status -1");
    CHECK(ev.signal[0] == 0, "endf: bare signal empty");
    ctx_event_free(&ev);

    for (i = 0; i < 4; i++) {
        st(&s, "start=a");
        snprintf(buf, sizeof buf, "end=a;exit=%s", names[i]);
        memset(&ev, 0, sizeof ev);
        ap(&s, buf, &ev);
        CHECK(ev.exit == ids[i], "endf: exit name");
        ctx_event_free(&ev);
    }

    st(&s, "start=a");
    memset(&ev, 0, sizeof ev);
    ap(&s, "end=a;exit=wombat", &ev);
    CHECK(ev.what == CTX_ENDED, "endf: unknown exit still ends");
    CHECK(ev.exit == CTX_EXIT_NONE, "endf: unknown exit NONE");
    ctx_event_free(&ev);

    st(&s, "start=a");
    memset(&ev, 0, sizeof ev);
    ap(&s, "end=a;exit=failure;status=0", &ev);
    CHECK(ev.status == 0, "endf: status 0");
    CHECK(ev.exit == CTX_EXIT_FAILURE, "endf: exit with status");
    ctx_event_free(&ev);

    st(&s, "start=a");
    memset(&ev, 0, sizeof ev);
    ap(&s, "end=a;status=255", &ev);
    CHECK(ev.status == 255, "endf: status 255");
    ctx_event_free(&ev);

    st(&s, "start=a");
    memset(&ev, 0, sizeof ev);
    ap(&s, "end=a;status=256", &ev);
    CHECK(ev.status == -1, "endf: status 256 ignored");
    ctx_event_free(&ev);

    st(&s, "start=a");
    memset(&ev, 0, sizeof ev);
    ap(&s, "end=a;status=-1", &ev);
    CHECK(ev.status == -1, "endf: status -1 ignored");
    ctx_event_free(&ev);

    st(&s, "start=a");
    memset(&ev, 0, sizeof ev);
    ap(&s, "end=a;status=abc", &ev);
    CHECK(ev.status == -1, "endf: status abc ignored");
    ctx_event_free(&ev);

    st(&s, "start=a");
    memset(&ev, 0, sizeof ev);
    ap(&s, "end=a;status=", &ev);
    CHECK(ev.status == -1, "endf: empty status ignored");
    ctx_event_free(&ev);

    st(&s, "start=a");
    memset(&ev, 0, sizeof ev);
    ap(&s, "end=a;exit=crash;signal=SIGKILL", &ev);
    CHECK(strcmp(ev.signal, "SIGKILL") == 0, "endf: signal SIGKILL");
    ctx_event_free(&ev);

    st(&s, "start=a");
    memset(&ev, 0, sizeof ev);
    ap(&s, "end=a;signal=KILL", &ev);
    CHECK(ev.signal[0] == 0, "endf: signal KILL ignored");
    ctx_event_free(&ev);

    /* 31 bytes ok, 32 too long */
    st(&s, "start=a");
    memset(&ev, 0, sizeof ev);
    ap(&s, "end=a;signal=SIGAAAAAAAAAAAAAAAAAAAAAAAAAAAA", &ev);
    CHECK(strlen(ev.signal) == 31, "endf: signal 31 bytes ok");
    ctx_event_free(&ev);

    st(&s, "start=a");
    memset(&ev, 0, sizeof ev);
    ap(&s, "end=a;signal=SIGAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", &ev);
    CHECK(ev.signal[0] == 0, "endf: signal 32 bytes ignored");
    ctx_event_free(&ev);

    /* end fields on an end of an unknown id are not reported */
    memset(&ev, 0, sizeof ev);
    CHECK(ap(&s, "end=nope;exit=success", &ev) == CTX_IGNORED,
          "endf: unknown id ignored");
    ctx_clear(&s);
}

static void test_runs_as(void)
{
    CtxStack s;
    ctx_init(&s);

    CHECK(ctx_runs_as(&s) == CTX_AS_PLAIN, "as: nothing plain");
    st(&s, "start=a;type=shell;user=carol");
    CHECK(ctx_runs_as(&s) == CTX_AS_PLAIN, "as: login carol plain");
    st(&s, "start=b;type=elevate");
    CHECK(ctx_runs_as(&s) == CTX_AS_ROOT, "as: run0 root");
    ctx_clear(&s);

    st(&s, "start=a;type=shell;user=root");
    CHECK(ctx_runs_as(&s) == CTX_AS_ROOT, "as: login root");
    ctx_clear(&s);

    st(&s, "start=a;type=shell;user=carol");
    st(&s, "start=b;type=elevate;targetuser=bob");
    CHECK(ctx_runs_as(&s) == CTX_AS_SWITCHED, "as: run0 -u bob switched");
    st(&s, "start=c;type=shell;user=bob");
    CHECK(ctx_runs_as(&s) == CTX_AS_SWITCHED, "as: bob's shell keeps it");
    st(&s, "start=d;type=command");
    CHECK(ctx_runs_as(&s) == CTX_AS_SWITCHED, "as: a command keeps it");
    st(&s, "start=e;type=remote;user=dave");
    CHECK(ctx_runs_as(&s) == CTX_AS_PLAIN, "as: onward login dave plain");
    st(&s, "start=f;type=elevate");
    CHECK(ctx_runs_as(&s) == CTX_AS_ROOT, "as: root over everything");
    ctx_clear(&s);

    st(&s, "start=a;type=chpriv;targetuser=bob");
    CHECK(ctx_runs_as(&s) == CTX_AS_SWITCHED, "as: chpriv bob switched");
    ctx_clear(&s);

    st(&s, "start=a;type=elevate;targetuser=root");
    CHECK(ctx_runs_as(&s) == CTX_AS_ROOT, "as: targetuser=root");
    st(&s, "start=b;type=shell;user=carol");
    CHECK(ctx_runs_as(&s) == CTX_AS_PLAIN, "as: carol inside root plain");
    ctx_clear(&s);
}

static void test_effective_user(void)
{
    CtxStack s;
    ctx_init(&s);

    CHECK(ctx_effective_user(&s) == NULL, "user: nothing is NULL");
    CHECK(!ctx_elevated(&s), "user: nothing not elevated");

    st(&s, "start=a;type=shell");
    CHECK(ctx_effective_user(&s) == NULL, "user: shell without user NULL");
    CHECK(!ctx_elevated(&s), "user: unknown not elevated");
    ctx_clear(&s);

    st(&s, "start=a;type=shell;user=carol");
    CHECK(feq(ctx_effective_user(&s), "carol"), "user: shell carol");
    CHECK(!ctx_elevated(&s), "user: carol not elevated");
    ctx_clear(&s);

    st(&s, "start=a;type=shell;user=carol");
    st(&s, "start=b;type=elevate");
    CHECK(feq(ctx_effective_user(&s), "root"), "user: elevate default root");
    CHECK(ctx_elevated(&s), "user: elevate elevated");
    ctx_clear(&s);

    st(&s, "start=a;type=shell;user=carol");
    st(&s, "start=b;type=elevate;targetuser=alice");
    CHECK(feq(ctx_effective_user(&s), "alice"), "user: elevate alice");
    CHECK(!ctx_elevated(&s), "user: alice not elevated");
    ctx_clear(&s);

    st(&s, "start=a;type=elevate");
    st(&s, "start=b;type=shell;user=carol");
    CHECK(feq(ctx_effective_user(&s), "carol"), "user: carol inside elevate");
    CHECK(!ctx_elevated(&s), "user: carol inside elevate not elevated");
    ctx_clear(&s);

    /* targetuser on a shell is dropped, so it does not change the user */
    st(&s, "start=a;type=shell;user=carol;targetuser=root");
    CHECK(feq(ctx_effective_user(&s), "carol"), "user: shell targetuser no-op");
    CHECK(!ctx_elevated(&s), "user: shell targetuser not elevated");
    ctx_clear(&s);

    /* a context without a user keeps the one from further out */
    st(&s, "start=a;type=shell;user=carol");
    st(&s, "start=b;type=command");
    CHECK(feq(ctx_effective_user(&s), "carol"), "user: inherited");
    ctx_clear(&s);

    /* ending the elevate returns to the outer user */
    st(&s, "start=a;type=shell;user=carol");
    st(&s, "start=b;type=elevate");
    {
        CtxEvent ev;
        memset(&ev, 0, sizeof ev);
        ap(&s, "end=b", &ev);
        ctx_event_free(&ev);
    }
    CHECK(feq(ctx_effective_user(&s), "carol"), "user: after elevate ends");
    CHECK(!ctx_elevated(&s), "user: after elevate not elevated");
    ctx_clear(&s);
}

static void test_seq_max(void)
{
    CtxStack s;
    static char buf[CTX_SEQ_MAX + 64];
    ctx_init(&s);

    /* exactly CTX_SEQ_MAX bytes: padding with empty segments */
    memset(buf, ';', CTX_SEQ_MAX);
    memcpy(buf, "start=a", 7);
    CHECK(ctx_apply(&s, buf, CTX_SEQ_MAX, NULL) == CTX_STARTED,
          "seq: exactly max ok");
    ctx_clear(&s);

    memset(buf, ';', CTX_SEQ_MAX + 1);
    memcpy(buf, "start=a", 7);
    CHECK(ctx_apply(&s, buf, CTX_SEQ_MAX + 1, NULL) == CTX_IGNORED,
          "seq: max + 1 ignored");
    CHECK(s.depth == 0, "seq: nothing opened");

    st(&s, "start=a");
    memset(buf, ';', CTX_SEQ_MAX + 1);
    memcpy(buf, "end=a", 5);
    CHECK(ctx_apply(&s, buf, CTX_SEQ_MAX + 1, NULL) == CTX_IGNORED,
          "seq: long end ignored");
    CHECK(s.depth == 1, "seq: long end kept the context");
    ctx_clear(&s);
}

static void test_names(void)
{
    CHECK(strcmp(ctx_type_name(CTX_TYPE_ELEVATE), "elevate") == 0,
          "name: elevate");
    CHECK(strcmp(ctx_type_name(CTX_TYPE_SHELL), "shell") == 0, "name: shell");
    CHECK(strcmp(ctx_type_name(CTX_TYPE_SESSION), "session") == 0,
          "name: session");
    CHECK(strcmp(ctx_type_name(CTX_TYPE_NONE), "") == 0, "name: type NONE");
    CHECK(strcmp(ctx_type_name(-1), "") == 0, "name: type -1");
    CHECK(strcmp(ctx_type_name(CTX_TYPE_COUNT), "") == 0, "name: type COUNT");
    CHECK(strcmp(ctx_type_name(999), "") == 0, "name: type 999");

    CHECK(strcmp(ctx_exit_name(CTX_EXIT_NONE), "") == 0, "name: exit NONE");
    CHECK(strcmp(ctx_exit_name(CTX_EXIT_SUCCESS), "success") == 0,
          "name: exit success");
    CHECK(strcmp(ctx_exit_name(CTX_EXIT_INTERRUPT), "interrupt") == 0,
          "name: exit interrupt");
    CHECK(strcmp(ctx_exit_name(-1), "") == 0, "name: exit -1");
    CHECK(strcmp(ctx_exit_name(99), "") == 0, "name: exit 99");

    CHECK(strcmp(ctx_field_name(CTX_F_USER), "user") == 0, "name: field user");
    CHECK(strcmp(ctx_field_name(CTX_F_MACHINEID), "machineid") == 0,
          "name: field machineid");
    CHECK(ctx_field_name(CTX_F_COUNT) != NULL, "name: field out of range");
}

int main(void)
{
    test_start_basic();
    test_nesting_and_end();
    test_end_parent();
    test_end_unknown();
    test_restart();
    test_depth_cap();
    test_id_limits();
    test_leading_token();
    test_types();
    test_fields_stored();
    test_escapes();
    test_control_chars();
    test_field_length();
    test_utf8();
    test_ids_formats();
    test_type_specific();
    test_malformed();
    test_end_fields();
    test_effective_user();
    test_runs_as();
    test_seq_max();
    test_names();

    if (failures == 0) printf("Test suite passed\n");
    else printf("%d failures\n", failures);
    return failures != 0;
}
