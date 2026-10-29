/*
 * test_agentbind - regression tests for kitty/kitty_agentbind.c: the
 * session-bind@openssh.com message byte for byte (a login and a forwarded
 * connection, the sizing call, a buffer too small), the agent's answer
 * (SUCCESS binds, FAILURE, EXTENSION_FAILURE, a short or inconsistent
 * answer and none do not), and the skipping of that answer in front of a
 * forwarded agent stream (in one piece, byte by byte, with the stream's
 * own data behind it, a bad length).
 *
 * Returns non-zero on any failure.
 */
#include <stdio.h>
#include <string.h>

#include "../kitty/kitty_agentbind.h"

static int failures = 0;

static void expect(const char *what, bool got, bool want)
{
    if (got != want) {
        printf("FAIL [%s]: got %d, want %d\n", what, (int)got, (int)want);
        failures++;
    }
}

static void expect_size(const char *what, size_t got, size_t want)
{
    if (got != want) {
        printf("FAIL [%s]: got %u, want %u\n", what, (unsigned)got,
               (unsigned)want);
        failures++;
    }
}

static void expect_bytes(const char *what, const unsigned char *got,
                         const unsigned char *want, size_t len)
{
    if (memcmp(got, want, len)) {
        printf("FAIL [%s]: bytes differ\n", what);
        failures++;
    }
}

int main(void)
{
    static const unsigned char hk[] = { 0x00, 0x00, 0x00, 0x0b, 's', 's', 'h',
                                        '-', 'e', 'd', '2', '5', '5', '1', '9' };
    static const unsigned char sid[] = { 0x11, 0x22, 0x33 };
    static const unsigned char sig[] = { 0xa1, 0xb2 };
    /* 27, "session-bind@openssh.com", hk, sid, sig, bool */
    static const unsigned char want_login[] = {
        27,
        0, 0, 0, 24, 's', 'e', 's', 's', 'i', 'o', 'n', '-', 'b', 'i', 'n',
        'd', '@', 'o', 'p', 'e', 'n', 's', 's', 'h', '.', 'c', 'o', 'm',
        0, 0, 0, 15, 0x00, 0x00, 0x00, 0x0b, 's', 's', 'h', '-', 'e', 'd',
        '2', '5', '5', '1', '9',
        0, 0, 0, 3, 0x11, 0x22, 0x33,
        0, 0, 0, 2, 0xa1, 0xb2,
        0 };
    unsigned char buf[128];
    size_t n;

    /* the message */
    n = kitty_agentbind_encode(NULL, 0, hk, sizeof(hk), sid, sizeof(sid),
                               sig, sizeof(sig), false);
    expect_size("sizing call: the body's length", n, sizeof(want_login));
    memset(buf, 0xee, sizeof(buf));
    n = kitty_agentbind_encode(buf, 10, hk, sizeof(hk), sid, sizeof(sid),
                               sig, sizeof(sig), false);
    expect_size("buffer too small: the length needed", n, sizeof(want_login));
    expect("buffer too small: nothing written", buf[0] == 0xee, true);
    n = kitty_agentbind_encode(buf, sizeof(buf), hk, sizeof(hk), sid,
                               sizeof(sid), sig, sizeof(sig), false);
    expect_size("a login: its length", n, sizeof(want_login));
    expect_bytes("a login: byte for byte", buf, want_login,
                 sizeof(want_login));
    n = kitty_agentbind_encode(buf, sizeof(buf), hk, sizeof(hk), sid,
                               sizeof(sid), sig, sizeof(sig), true);
    expect_bytes("forwarding: the same up to the bool", buf, want_login,
                 sizeof(want_login) - 1);
    expect("forwarding: is_forwarding 1", buf[n - 1] == 1, true);
    n = kitty_agentbind_encode(buf, sizeof(buf), hk, sizeof(hk), sid, 0,
                               sig, 0, false);
    expect_size("empty strings are written as empty strings", n,
                sizeof(want_login) - sizeof(sid) - sizeof(sig));

    /* the answer */
    {
        static const unsigned char ok[] = { 0, 0, 0, 1, 6 };
        static const unsigned char fail[] = { 0, 0, 0, 1, 5 };
        static const unsigned char extfail[] = { 0, 0, 0, 1, 28 };
        static const unsigned char longer[] = { 0, 0, 0, 2, 6, 0 };
        static const unsigned char wronglen[] = { 0, 0, 0, 5, 6 };
        expect("SUCCESS binds", kitty_agentbind_reply_bound(ok, 5), true);
        expect("FAILURE (5) does not",
               kitty_agentbind_reply_bound(fail, 5), false);
        expect("EXTENSION_FAILURE (28) does not",
               kitty_agentbind_reply_bound(extfail, 5), false);
        expect("its type is read",
               kitty_agentbind_reply_type(extfail, 5) == 28, true);
        expect("a SUCCESS with a body still binds",
               kitty_agentbind_reply_bound(longer, 6), true);
        expect("a length field that does not fit: no",
               kitty_agentbind_reply_bound(wronglen, 5), false);
        expect("a length field that does not fit: no type",
               kitty_agentbind_reply_type(wronglen, 5) == -1, true);
        expect("a short answer: no", kitty_agentbind_reply_bound(ok, 4),
               false);
        expect("no answer: no", kitty_agentbind_reply_bound(NULL, 0), false);
        expect("no answer: no type",
               kitty_agentbind_reply_type(NULL, 0) == -1, true);
    }

    /* skipping the answer in front of a forwarded stream */
    {
        static const unsigned char stream[] = { 0, 0, 0, 1, 6,
                                                0, 0, 0, 1, 12 };
        struct kitty_agentbind_skip st;
        size_t used, i, total;

        memset(&st, 0, sizeof(st));
        used = kitty_agentbind_skip(&st, stream, sizeof(stream));
        expect_size("one piece: the answer's 5 bytes are taken", used, 5);
        expect("one piece: done", st.done, true);
        expect_size("done: nothing more is taken",
                    kitty_agentbind_skip(&st, stream, sizeof(stream)), 0);

        memset(&st, 0, sizeof(st));
        total = 0;
        for (i = 0; i < 5; i++) {
            used = kitty_agentbind_skip(&st, stream + i, 1);
            total += used;
            expect("byte by byte: done only at the last byte", st.done,
                   i == 4);
        }
        expect_size("byte by byte: 5 bytes taken", total, 5);

        memset(&st, 0, sizeof(st));
        used = kitty_agentbind_skip(&st, stream, 3);
        expect_size("a split length field: 3 taken", used, 3);
        used = kitty_agentbind_skip(&st, stream + 3, sizeof(stream) - 3);
        expect_size("the rest of the answer: 2 taken", used, 2);
        expect("a split length field: done", st.done, true);

        {
            static const unsigned char longans[] = { 0, 0, 0, 4, 29, 'x',
                                                     'y', 'z' };
            memset(&st, 0, sizeof(st));
            used = kitty_agentbind_skip(&st, longans, 6);
            expect_size("a longer answer, first part: all taken", used, 6);
            expect("a longer answer, first part: not done", st.done, false);
            used = kitty_agentbind_skip(&st, longans + 6, 2);
            expect_size("a longer answer, last part: 2 taken", used, 2);
            expect("a longer answer: done", st.done, true);
        }
        {
            static const unsigned char zero[] = { 0, 0, 0, 0, 6 };
            static const unsigned char huge[] = { 0, 0x10, 0, 0, 6 };
            memset(&st, 0, sizeof(st));
            used = kitty_agentbind_skip(&st, zero, sizeof(zero));
            expect("a zero length is bad", st.bad, true);
            expect_size("a zero length: the length field taken", used, 4);
            memset(&st, 0, sizeof(st));
            (void)kitty_agentbind_skip(&st, huge, sizeof(huge));
            expect("a length above the agent maximum is bad", st.bad, true);
            expect_size("bad: nothing more is taken",
                        kitty_agentbind_skip(&st, huge, sizeof(huge)), 0);
        }
    }

    if (failures) {
        printf("test_agentbind: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_agentbind: all passed\n");
    return 0;
}
