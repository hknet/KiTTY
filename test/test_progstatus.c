/*
 * test_progstatus - regression tests for program status reports from the
 * host (OSC 7501) and the shell's prompt marks (OSC 133): the platform-free
 * half in kitty/kitty_progstatus.c.
 *
 * Locks in: the feature-detection query; state is required and an unknown
 * one rejects the report; malformed pairs are skipped and unknown keys
 * ignored; the last repeat wins; every limit discards the report whole; ids
 * are validated; text is base64 UTF-8 without control characters; a report
 * replaces its record; clear removes the record and its subtree; app is
 * inherited; the store cap drops the oldest; the window's summary; a prompt
 * drops working and blocked; OSC 133 A, B, C and D.
 *
 * Returns non-zero on any failure.
 */
#include <stdio.h>
#include <string.h>

#include "../kitty/kitty_progstatus.h"

static int failures = 0;

#define CHECK(cond, what) do { \
    if (!(cond)) { printf("FAIL [%s] line %d\n", what, __LINE__); failures++; } \
} while (0)

static int parse(const char *s, PsRecord *r)
{
    return ps_parse(s, strlen(s), r);
}

static void test_parse(void)
{
    PsRecord r;
    char big[5000];

    CHECK(parse("?", &r) == PS_QUERY, "query");
    CHECK(parse(" ? ", &r) == PS_QUERY, "query, spaces");
    CHECK(parse("", &r) == PS_REJECT, "empty");
    CHECK(parse("app=x", &r) == PS_REJECT, "no state");
    CHECK(parse("state=sleeping", &r) == PS_REJECT, "unknown state");

    CHECK(parse("state=working:progress=40", &r) == PS_REPORT &&
          r.state == PS_WORKING && r.progress == 40 && r.id[0] == 0,
          "working 40, root");
    CHECK(parse(" state = working : progress = 7 ", &r) == PS_REPORT &&
          r.progress == 7, "spaces stripped");
    CHECK(parse("state=working:progress=101", &r) == PS_REPORT &&
          r.progress == -1, "progress over 100 ignored");
    CHECK(parse("state=done:progress=50", &r) == PS_REPORT &&
          r.progress == -1, "progress only with working/blocked");
    CHECK(parse("state=blocked:kind=permission", &r) == PS_REPORT &&
          r.kind == PS_KIND_PERMISSION, "blocked permission");
    CHECK(parse("state=working:kind=auth", &r) == PS_REPORT &&
          r.kind == PS_KIND_NONE, "kind only with blocked");
    CHECK(parse("state=idle:state=error", &r) == PS_REPORT &&
          r.state == PS_ERROR, "last repeat wins");
    CHECK(parse("garbage:state=done:=x:Key=v:app=a;b:zz=1", &r) == PS_REPORT &&
          r.state == PS_DONE && r.app[0] == 0, "malformed pairs skipped");
    CHECK(parse("state=done:app=terraform", &r) == PS_REPORT &&
          !strcmp(r.app, "terraform"), "app");
    CHECK(parse("state=done:app=a,b", &r) == PS_REPORT && r.app[0] == 0,
          "app outside its set ignored");

    /* ids */
    CHECK(parse("state=done:id=deploy/eu-west.1", &r) == PS_REPORT &&
          !strcmp(r.id, "deploy/eu-west.1"), "id path");
    CHECK(parse("state=done:id=a//b", &r) == PS_REJECT, "empty segment");
    CHECK(parse("state=done:id=a/", &r) == PS_REJECT, "trailing slash");
    CHECK(parse("state=done:id=a,b", &r) == PS_REJECT, "id character");
    CHECK(parse("state=done:id=1/2/3/4/5/6/7/8", &r) == PS_REPORT, "8 levels");
    CHECK(parse("state=done:id=1/2/3/4/5/6/7/8/9", &r) == PS_REJECT, "9 levels");
    CHECK(parse("state=done:id=abcdefghijklmnopqrstuvwxyz0123456", &r) == PS_REJECT,
          "segment of 33");

    /* text: "Apply 3?" and a newline inside */
    CHECK(parse("state=blocked:msg=QXBwbHkgMz8=", &r) == PS_REPORT &&
          !strcmp(r.msg, "Apply 3?"), "msg base64");
    CHECK(parse("state=blocked:msg=QXBwbHkgMz8", &r) == PS_REPORT &&
          !strcmp(r.msg, "Apply 3?"), "msg without padding");
    CHECK(parse("state=blocked:msg=YQpi", &r) == PS_REPORT && r.msg[0] == 0,
          "control character refused");
    CHECK(parse("state=blocked:msg=wpE=", &r) == PS_REPORT && r.msg[0] == 0,
          "C1 refused");
    CHECK(parse("state=blocked:title=w6TDtsO8", &r) == PS_REPORT &&
          !strcmp(r.title, "\xc3\xa4\xc3\xb6\xc3\xbc"), "UTF-8 title");
    CHECK(parse("state=blocked:title=/w==", &r) == PS_REPORT && r.title[0] == 0,
          "invalid UTF-8 refused");

    /* limits discard the whole report */
    CHECK(parse("state=done:abcdefghijklmnopq=1", &r) == PS_REJECT, "key of 17");
    CHECK(parse("state=done:app=abcdefghijklmnopqrstuvwxyz0123456", &r) == PS_REJECT,
          "app of 33");
    memset(big, 'A', sizeof(big));
    memcpy(big, "state=done:title=", 17);
    big[17 + 260] = '\0';
    CHECK(parse(big, &r) == PS_REJECT, "title over 256 encoded");
    memset(big, 'A', sizeof(big));
    memcpy(big, "state=done:msg=", 15);
    big[15 + 2736] = '\0';
    CHECK(parse(big, &r) == PS_REJECT, "msg over 2732 encoded");
    memset(big, 'x', sizeof(big));
    memcpy(big, "state=done:", 11);
    big[4090] = '\0';
    CHECK(parse(big, &r) == PS_REJECT, "sequence over 4096");
}

static void report(PsStore *st, const char *s, int *prev)
{
    PsRecord r;
    int p;
    if (ps_parse(s, strlen(s), &r) == PS_REPORT)
        ps_apply(st, &r, prev ? prev : &p);
    else
        printf("FAIL [report did not parse: %s]\n", s), failures++;
}

static void test_store(void)
{
    PsStore st;
    PsView v;
    int prev, i;

    memset(&st, 0, sizeof(st));
    ps_view(&st, &v);
    CHECK(v.state == PS_IDLE && v.progress == -1, "empty = idle");

    report(&st, "state=working:app=deploy:progress=10", &prev);
    CHECK(prev == -1, "new record");
    report(&st, "state=working:id=eu:progress=60", &prev);
    report(&st, "state=working:id=eu/db", &prev);
    CHECK(st.n == 3, "three records");
    CHECK(!strcmp(ps_app_of(&st, &st.e[2]->r), "deploy"), "app inherited");
    ps_view(&st, &v);
    CHECK(v.state == PS_WORKING && v.progress == 60, "latest percentage");

    report(&st, "state=working", &prev);
    CHECK(prev == PS_WORKING && st.e[0]->r.app[0] == 0 &&
          st.e[0]->r.progress == -1, "a report replaces its record");

    report(&st, "state=blocked:id=eu/db:kind=question", &prev);
    ps_view(&st, &v);
    CHECK(v.state == PS_BLOCKED && v.kind == PS_KIND_QUESTION, "blocked wins");

    report(&st, "state=error:id=us", &prev);
    ps_view(&st, &v);
    CHECK(v.state == PS_BLOCKED, "blocked outranks error");

    report(&st, "state=clear:id=eu", &prev);
    CHECK(st.n == 2, "clear takes the subtree");
    ps_view(&st, &v);
    CHECK(v.state == PS_ERROR, "then error");

    ps_mark_seen(&st);
    ps_view(&st, &v);
    CHECK(v.state == PS_WORKING, "a seen error no longer counts");

    report(&st, "state=done:id=us", &prev);
    CHECK(prev == PS_ERROR, "prev state");
    ps_view(&st, &v);
    CHECK(v.state == PS_WORKING, "working outranks done");

    ps_drop_running(&st);
    CHECK(st.n == 1 && st.e[0]->r.state == PS_DONE, "a prompt drops working");
    ps_view(&st, &v);
    CHECK(v.state == PS_DONE, "unseen done");

    report(&st, "state=clear", &prev);
    CHECK(st.n == 0, "clear at the root takes everything");

    for (i = 0; i < PS_RECORDS_MAX + 5; i++) {
        char s[64];
        sprintf(s, "state=idle:id=r%d", i);
        report(&st, s, &prev);
    }
    CHECK(st.n == PS_RECORDS_MAX, "the store is capped");
    {
        PsRecord r;
        int found0 = 0, foundlast = 0, k;
        (void)r;
        for (k = 0; k < st.n; k++) {
            if (!strcmp(st.e[k]->r.id, "r0")) found0 = 1;
            if (!strcmp(st.e[k]->r.id, "r68")) foundlast = 1;
        }
        CHECK(!found0 && foundlast, "the oldest goes");
    }
    ps_free(&st);
    CHECK(st.n == 0, "freed");
}

static void test_osc133(void)
{
    int code;
    CHECK(ps_osc133("A", 1, &code) == PS133_PROMPT, "A");
    CHECK(ps_osc133("A;cl=m;aid=7", 12, &code) == PS133_PROMPT, "A with options");
    CHECK(ps_osc133("B", 1, &code) == PS133_INPUT, "B");
    CHECK(ps_osc133("B;", 2, &code) == PS133_INPUT, "B; (fish)");
    CHECK(ps_osc133("Bx", 2, &code) == PS133_OTHER, "not B");
    CHECK(ps_osc133("C", 1, &code) == PS133_OUTPUT, "C");
    CHECK(ps_osc133("C;", 2, &code) == PS133_OUTPUT, "C; (fish)");
    CHECK(ps_osc133("D;0", 3, &code) == PS133_END && code == 0, "D;0");
    CHECK(ps_osc133("D;127", 5, &code) == PS133_END && code == 127, "D;127");
    CHECK(ps_osc133("D", 1, &code) == PS133_END && code == -1, "D alone");
    CHECK(ps_osc133("Ax", 2, &code) == PS133_OTHER, "not A");
    CHECK(ps_osc133("", 0, &code) == PS133_OTHER, "empty");
}

int main(void)
{
    test_parse();
    test_store();
    test_osc133();
    if (failures)
        printf("%d failure(s)\n", failures);
    else
        printf("test_progstatus: all passed\n");
    return failures != 0;
}
