/*
 * test_strbuf_catf - regression test for utils/dupprintf.c
 * dupvprintf_inner: an append (put_fmt / strbuf_catf) into a strbuf that
 * already holds text. vsnprintf is given the space left, and its result was
 * compared with the whole buffer: a piece longer than the space left but
 * shorter than the buffer was cut and taken as complete.
 *
 * Every combination of text already there (0..3000 bytes) and piece length
 * (1..5000) in steps, each checked byte for byte.
 *
 * Returns non-zero on any failure.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#include "putty.h"

static int failures = 0;

/* libutils' out_of_memory reports through it (the program has its own) */
void modalfatalbox(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static void one(size_t before, size_t piece)
{
    strbuf *sb = strbuf_new();
    char *a = snewn(before + 1, char), *b = snewn(piece + 1, char);
    size_t i;
    memset(a, 'a', before); a[before] = '\0';
    for (i = 0; i < piece; i++) b[i] = (char)('0' + i % 10);
    b[piece] = '\0';
    if (before) put_fmt(sb, "%s", a);
    put_fmt(sb, "%s\n", b);
    put_fmt(sb, "%s", "END");
    if (sb->len != before + piece + 1 + 3 ||
        memcmp(sb->s, a, before) ||
        memcmp(sb->s + before, b, piece) ||
        sb->s[before + piece] != '\n' ||
        memcmp(sb->s + before + piece + 1, "END", 4)) {
        if (failures < 10)
            printf("FAIL: %u bytes there, a %u-character piece: length %u, want %u\n",
                   (unsigned)before, (unsigned)piece, (unsigned)sb->len,
                   (unsigned)(before + piece + 4));
        failures++;
    }
    sfree(a);
    sfree(b);
    strbuf_free(sb);
}

int main(void)
{
    size_t before, piece;
    int cases = 0;
    for (before = 0; before <= 3000; before += 37)
        for (piece = 1; piece <= 5000; piece += 53) {
            one(before, piece);
            cases++;
        }
    /* the case the URL handler report hit: a 1500-character command after
     * about 1600 bytes of report */
    one(1600, 1500);
    cases++;
    if (failures) {
        printf("FAILED: %d of %d appends cut\n", failures, cases);
        return 1;
    }
    printf("PASSED: %d appends complete\n", cases);
    return 0;
}
