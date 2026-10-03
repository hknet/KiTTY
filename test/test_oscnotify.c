/*
 * test_oscnotify - regression tests for desktop notifications from the host
 * (OSC 9, OSC 777, OSC 99) and taskbar progress (OSC 9;4): the platform-free
 * half in kitty/kitty_oscnotify.c.
 *
 * Locks in: ConEmu command numbers are not notices; OSC 777 other than
 * "notify" is ignored; the OSC 99 keys, chunking, base64 and limits; ids are
 * sanitised before they are echoed; o= narrows the setting and never widens
 * it; the w/u timing; the replies byte for byte; the flood rule.
 *
 * Returns non-zero on any failure.
 */
#include <stdio.h>
#include <string.h>

#include "../kitty/kitty_oscnotify.h"

static int failures = 0;

#define CHECK(cond, what) do { \
    if (!(cond)) { printf("FAIL [%s] line %d\n", what, __LINE__); failures++; } \
} while (0)

static int feed99(OnNotice *a, const char *s, OnNotice *out)
{
    return on_osc99(a, s, strlen(s), out);
}

static void test_osc9(void)
{
    int st = -9, v = -9;
    CHECK(on_osc9("hello", 5, &st, &v) == ON9_NOTICE, "9 text");
    CHECK(on_osc9("4 files done", 12, &st, &v) == ON9_NOTICE, "9 digits then text");
    CHECK(on_osc9("1;", 2, &st, &v) == ON9_IGNORE, "9;1 ConEmu");
    CHECK(on_osc9("9;C:\\x", 6, &st, &v) == ON9_IGNORE, "9;9 ConEmu cwd");
    CHECK(on_osc9("12", 2, &st, &v) == ON9_IGNORE, "9;12 ConEmu, end");
    CHECK(on_osc9("4;1;50", 6, &st, &v) == ON9_PROGRESS && st == 1 && v == 50,
          "9;4 normal 50");
    CHECK(on_osc9("4;0", 3, &st, &v) == ON9_PROGRESS && st == 0 && v == -1,
          "9;4 hide");
    CHECK(on_osc9("4;3;", 4, &st, &v) == ON9_PROGRESS && st == 3 && v == -1,
          "9;4 indeterminate");
    CHECK(on_osc9("4;2;250", 7, &st, &v) == ON9_PROGRESS && st == 2 && v == 100,
          "9;4 clamp");
    CHECK(on_osc9("4;5;10", 6, &st, &v) == ON9_IGNORE, "9;4 bad state");
    CHECK(on_osc9("4", 1, &st, &v) == ON9_IGNORE, "9;4 no state");
}

static void test_osc777(void)
{
    OnNotice n;
    CHECK(on_osc777("notify;T;B;c", 12, &n) == 1, "777 notify");
    CHECK(!strcmp(n.title, "T") && !strcmp(n.body, "B;c"), "777 fields");
    CHECK(on_osc777("preexec;x", 9, &n) == 0, "777 other ignored");
    CHECK(on_osc777("notifyx;a;b", 11, &n) == 0, "777 prefix only ignored");
    CHECK(on_osc777("notify", 6, &n) == 0, "777 bare");
}

static void test_osc99(void)
{
    OnNotice a, o;
    char big[5000];
    memset(&a, 0, sizeof(a));

    CHECK(feed99(&a, "i=1:d=0;Hello", &o) == ON99_NOTHING, "99 chunk 1");
    CHECK(feed99(&a, "i=1:p=body;World", &o) == ON99_SHOW, "99 chunk 2 shows");
    CHECK(!strcmp(o.title, "Hello") && !strcmp(o.body, "World") &&
          !strcmp(o.meta.id, "1"), "99 assembled");

    /* title chunks concatenate */
    feed99(&a, "i=x:d=0;ab", &o);
    CHECK(feed99(&a, "i=x;cd", &o) == ON99_SHOW && !strcmp(o.title, "abcd"),
          "99 title concatenation");

    /* a chunk of another id drops the unfinished one */
    feed99(&a, "i=a:d=0;lost", &o);
    CHECK(feed99(&a, "i=b;kept", &o) == ON99_SHOW && !strcmp(o.title, "kept"),
          "99 other id replaces");

    /* base64 */
    CHECK(feed99(&a, "e=1;SGVsbG8=", &o) == ON99_SHOW && !strcmp(o.title, "Hello"),
          "99 base64");
    CHECK(feed99(&a, "e=1;SGVsbG8", &o) == ON99_SHOW && !strcmp(o.title, "Hello"),
          "99 base64 unpadded");
    CHECK(feed99(&a, "e=1;S$Vs", &o) == ON99_NOTHING, "99 bad base64 dropped");

    /* keys */
    CHECK(feed99(&a, "i=k:o=invisible:u=2:w=1500:a=-focus,report:c=1;t", &o)
          == ON99_SHOW, "99 keys");
    CHECK(o.meta.occasion == ON_OCC_INVISIBLE && o.meta.urgency == 2 &&
          o.meta.expire == 1500 && o.meta.focus == 0 && o.meta.report == 1 &&
          o.meta.close_report == 1, "99 keys parsed");
    CHECK(feed99(&a, "p=icon;xx", &o) == ON99_SHOW && o.title_len == 0,
          "99 icon dropped, notice ends");

    /* control requests */
    CHECK(feed99(&a, "i=q:p=?;", &o) == ON99_QUERY && !strcmp(o.meta.id, "q"),
          "99 query");
    CHECK(feed99(&a, "i=q:p=alive;", &o) == ON99_ALIVE, "99 alive");
    CHECK(feed99(&a, "i=q:p=close;", &o) == ON99_CLOSE, "99 close");

    /* no identifier means i=0 (the specification) */
    CHECK(feed99(&a, "p=?;", &o) == ON99_QUERY && !strcmp(o.meta.id, "0"),
          "99 query without id is i=0");
    CHECK(feed99(&a, "a=report:c=1;t", &o) == ON99_SHOW &&
          !strcmp(o.meta.id, "0") && o.meta.report && o.meta.close_report,
          "99 notice without id is i=0");
    CHECK(feed99(&a, "i=\033\033;t", &o) == ON99_SHOW && !strcmp(o.meta.id, "0"),
          "99 id sanitised to nothing is i=0");
    /* OSC 9 / 777 notices carry no id: never listed, never reported */
    CHECK(on_osc777("notify;T;B", 10, &o) == 1 && o.meta.id[0] == '\0',
          "777 notice has no id");

    /* id sanitising */
    CHECK(feed99(&a, "i=a\033]b<c>d+e.f_g-h:p=?;", &o) == ON99_QUERY &&
          !strcmp(o.meta.id, "abcd+e.f_g-h"), "99 id sanitised on the wire");
    {
        char id[ON_ID_MAX + 1];
        on_id_sanitise("a\033]b c;d+e.f_g-h", 16, id);
        CHECK(!strcmp(id, "abcd+e.f_g-h"), "id sanitised");
        memset(big, 'z', 200);
        on_id_sanitise(big, 200, id);
        CHECK(strlen(id) == ON_ID_MAX, "id capped");
    }

    /* limits: chunked parts capped in total, cut not dropped */
    memset(big, 'b', 3000);
    big[3000] = '\0';
    {
        char seq[3100];
        snprintf(seq, sizeof(seq), "i=L:d=0:p=body;%s", big);
        feed99(&a, seq, &o);
        snprintf(seq, sizeof(seq), "i=L:p=body;%s", big);
        CHECK(feed99(&a, seq, &o) == ON99_SHOW && o.body_len == ON_BODY_MAX,
              "99 body cap over chunks");
        memset(big, 't', 400);
        big[400] = '\0';
        snprintf(seq, sizeof(seq), "i=T;%s", big);
        CHECK(feed99(&a, seq, &o) == ON99_SHOW && o.title_len == ON_TITLE_MAX,
              "99 title cap");
    }
}

static void test_clean(void)
{
    char out[64];
    size_t n;
    n = on_text_clean("a\x01" "b\x7f" "c\nd", 7, out, 32);
    CHECK(n == 5 && !strcmp(out, "abc d"), "C0/DEL stripped, LF to space");
    n = on_text_clean("x\xc2\x9by", 4, out, 32);
    CHECK(!strcmp(out, "xy"), "C1 stripped");
    n = on_text_clean("x\xff\xc3(y", 5, out, 32);
    CHECK(!strcmp(out, "x(y"), "invalid UTF-8 dropped");
    n = on_text_clean("\xc0\xaf" "a", 3, out, 32);
    CHECK(!strcmp(out, "a"), "overlong dropped");
    n = on_text_clean("\xed\xa0\x80" "a", 4, out, 32);
    CHECK(!strcmp(out, "a"), "surrogate dropped");
    n = on_text_clean("ab\xc3\xa9", 4, out, 3);
    CHECK(n == 2 && !strcmp(out, "ab"), "cut on a character boundary");
    n = on_text_clean("\xe2\x82\xac", 3, out, 32);
    CHECK(n == 3, "euro kept");
    (void)n;
}

static void test_should_show(void)
{
    int s, occ, f, v;
    /* Off wins over everything */
    for (occ = 0; occ <= 2; occ++)
        for (f = 0; f <= 1; f++)
            for (v = 0; v <= 1; v++)
                CHECK(!on_should_show(ON_SET_OFF, occ, f, v), "off");
    /* o=always never widens "when not focused" */
    CHECK(!on_should_show(ON_SET_UNFOCUSED, ON_OCC_ALWAYS, 1, 1), "unfocused+always focused");
    CHECK(on_should_show(ON_SET_UNFOCUSED, ON_OCC_ALWAYS, 0, 1), "unfocused+always unfocused");
    CHECK(on_should_show(ON_SET_ALWAYS, ON_OCC_ALWAYS, 1, 1), "always+always");
    /* o= narrows */
    CHECK(!on_should_show(ON_SET_ALWAYS, ON_OCC_UNFOCUSED, 1, 1), "always+o=unfocused focused");
    CHECK(on_should_show(ON_SET_ALWAYS, ON_OCC_UNFOCUSED, 0, 1), "always+o=unfocused unfocused");
    CHECK(!on_should_show(ON_SET_ALWAYS, ON_OCC_INVISIBLE, 0, 1), "o=invisible visible");
    CHECK(on_should_show(ON_SET_ALWAYS, ON_OCC_INVISIBLE, 0, 0), "o=invisible hidden");
    CHECK(!on_should_show(ON_SET_UNFOCUSED, ON_OCC_INVISIBLE, 0, 1), "unfocused+o=invisible visible");
    CHECK(on_should_show(ON_SET_UNFOCUSED, ON_OCC_INVISIBLE, 0, 0), "unfocused+o=invisible hidden");
    CHECK(!on_should_show(7, ON_OCC_ALWAYS, 0, 0), "unknown setting off");
    (void)s;
}

static void test_seconds(void)
{
    int crit;
    CHECK(on_seconds(-1, -1, &crit) == ON_DEFAULT_SECONDS && !crit, "default");
    CHECK(on_seconds(1, -1, &crit) == ON_DEFAULT_SECONDS, "u=1");
    CHECK(on_seconds(0, -1, &crit) == ON_SHORT_SECONDS, "u=0 short");
    CHECK(on_seconds(2, -1, &crit) == ON_STICKY_SECONDS && crit, "u=2 sticky red");
    CHECK(on_seconds(2, 3000, &crit) == ON_STICKY_SECONDS && crit, "u=2 beats w");
    CHECK(on_seconds(-1, 0, &crit) == ON_STICKY_SECONDS && !crit, "w=0 sticky");
    CHECK(on_seconds(-1, 1, &crit) == 1, "w=1 rounds up");
    CHECK(on_seconds(-1, 1500, &crit) == 2, "w=1500 -> 2");
    CHECK(on_seconds(0, 3000, &crit) == 3, "w beats u=0");
    CHECK(on_seconds(-1, 999999999, &crit) == ON_STICKY_SECONDS, "w capped");
}

static void test_replies(void)
{
    char buf[ON_REPLY_MAX];
    on_reply_query("q1", buf);
    CHECK(!strcmp(buf, "\033]99;i=q1:p=?;a=focus,report:c=1:o=always,unfocused,"
                       "invisible:p=title,body,close,alive,?:u=0,1,2:w=1\033\\"),
          "p=? reply");
    on_reply_query("", buf);
    CHECK(!strncmp(buf, "\033]99;i=0:p=?;a=focus", 20), "p=? reply without id: i=0");
    on_reply_alive("", "", buf);
    CHECK(!strcmp(buf, "\033]99;i=0:p=alive;\033\\"), "alive reply without id: i=0");
    on_reply_activated("", buf);
    CHECK(!strcmp(buf, "\033]99;i=0;\033\\"), "activated reply without id: i=0");
    on_reply_closed("", buf);
    CHECK(!strcmp(buf, "\033]99;i=0:p=close;\033\\"), "closed reply without id: i=0");
    on_reply_alive("q", "n1", buf);
    CHECK(!strcmp(buf, "\033]99;i=q:p=alive;n1\033\\"), "alive reply");
    on_reply_alive("q", "", buf);
    CHECK(!strcmp(buf, "\033]99;i=q:p=alive;\033\\"), "alive reply empty");
    on_reply_activated("n1", buf);
    CHECK(!strcmp(buf, "\033]99;i=n1;\033\\"), "activated reply");
    on_reply_closed("n1", buf);
    CHECK(!strcmp(buf, "\033]99;i=n1:p=close;\033\\"), "closed reply");
}

static void test_flood(void)
{
    OnFlood f;
    int d;
    memset(&f, 0, sizeof(f));
    CHECK(on_flood_offer(&f, 1000) == ON_FLOOD_NOW, "first shows");
    CHECK(on_flood_offer(&f, 1500) == ON_FLOOD_PEND && f.pending, "second pends");
    CHECK(on_flood_due(&f) == 3000, "due 2 s after the last");
    f.armed = 1;
    CHECK(on_flood_offer(&f, 1600) == ON_FLOOD_PEND && f.dropped == 1, "third replaces");
    CHECK(on_flood_offer(&f, 1700) == ON_FLOOD_PEND && f.dropped == 2, "fourth replaces");
    CHECK(on_flood_tick(&f, 3000, &d) == ON_FLOOD_NOW && !f.pending, "pending shown");
    CHECK(on_flood_due(&f) == 5000, "re-armed");
    CHECK(on_flood_tick(&f, 5000, &d) == ON_FLOOD_END && d == 2 && !f.armed,
          "burst ends with the count");
    CHECK(on_flood_offer(&f, 5001) == ON_FLOOD_NOW, "after the gap, at once");
    CHECK(on_flood_offer(&f, 9000) == ON_FLOOD_NOW, "far apart, at once");
}

int main(void)
{
    test_osc9();
    test_osc777();
    test_osc99();
    test_clean();
    test_should_show();
    test_seconds();
    test_replies();
    test_flood();
    if (failures)
        printf("%d failure(s)\n", failures);
    else
        printf("test_oscnotify: all passed\n");
    return failures != 0;
}
