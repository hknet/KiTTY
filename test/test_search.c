/*
 * test_search - the matcher of the scrollback search
 * (kitty/kitty_search_match.h): rows joined across a soft wrap, wide
 * characters, trailing blanks, smart case, non-overlapping matches, the
 * cap, and where a match sits in cells. Returns non-zero on any failure.
 */
#include <stdio.h>
#include <string.h>

#include "../kitty/kitty_search_match.h"

static int failures = 0;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s (line %d)\n", what, __LINE__); failures++; } } while (0)

/* An ASCII row of `cols` cells from a string, blanks after it. */
static void row_from(ksm_char *cells, int cols, const char *s)
{
    int i, end = 0;
    for (i = 0; i < cols; i++) {
        if (!end && !s[i]) end = 1;          /* blanks after the text */
        cells[i] = end ? ' ' : (ksm_char)(unsigned char)s[i];
    }
}

static int needle_from(ksm_char *n, const char *s)
{
    int i;
    for (i = 0; s[i]; i++) n[i] = (ksm_char)(unsigned char)s[i];
    return i;
}

static int find_in(const char *row0, int wrapped, const char *row1,
                   const char *needle, int *out, int max)
{
    ksm_line l; ksm_char cells[10], nd[32];
    int nn, cs, n;
    memset(&l, 0, sizeof(l));
    row_from(cells, 10, row0);
    ksm_line_add_row(&l, cells, 10, 0, wrapped, 0);
    if (row1) { row_from(cells, 10, row1); ksm_line_add_row(&l, cells, 10, 1, 0, 0); }
    nn = needle_from(nd, needle);
    cs = ksm_case_sensitive(nd, nn, NULL);
    n = ksm_find(&l, nd, nn, cs, NULL, out, max);
    ksm_line_free(&l);
    return n;
}

int main(void)
{
    int out[16], n;

    /* smart case */
    CHECK(find_in("Error here", 0, NULL, "error", out, 16) == 1, "lower case finds Error");
    CHECK(find_in("ERROR here", 0, NULL, "error", out, 16) == 1, "lower case finds ERROR");
    CHECK(find_in("ERROR here", 0, NULL, "Error", out, 16) == 0, "a capital makes it exact");
    CHECK(find_in("Error here", 0, NULL, "Error", out, 16) == 1, "exact case found");
    {
        ksm_char up[] = { 0xC4 }, lo[] = { 0xE4 };   /* Ä, ä */
        CHECK(ksm_case_sensitive(up, 1, NULL) == 1, "Ä is a capital");
        CHECK(ksm_case_sensitive(lo, 1, NULL) == 0, "ä is not");
        CHECK(ksm_lower_basic(0x178) == 0xFF, "Ÿ -> ÿ");
        CHECK(ksm_lower_basic(0x41A) == 0x43A, "Cyrillic К -> к");
        CHECK(ksm_lower_basic(0x3A3) == 0x3C3, "Greek Σ -> σ");
        CHECK(ksm_lower_basic(0x4E2D) == 0x4E2D, "CJK unchanged");
    }

    /* non-overlapping, several, positions */
    n = find_in("abababab", 0, NULL, "abab", out, 16);
    CHECK(n == 2 && out[0] == 0 && out[1] == 4, "non-overlapping abab");
    n = find_in("aaaaa", 0, NULL, "aa", out, 16);
    CHECK(n == 2 && out[0] == 0 && out[1] == 2, "aa in aaaaa: 2");
    CHECK(find_in("x", 0, NULL, "", out, 16) == 0, "empty needle: nothing");
    CHECK(find_in("ab", 0, NULL, "abc", out, 16) == 0, "needle longer than the line");

    /* the cap: count all, store max */
    n = find_in("aaaaaaaaaa", 0, NULL, "a", out, 3);
    CHECK(n == 10, "count beyond max");

    /* across a soft wrap: a wrapped row is full (10 cells), "hello worl" + "d" */
    CHECK(find_in("hello worl", 1, "d", "world", out, 16) == 1, "a hit across the wrap");
    /* trailing blanks are not content on an unwrapped row */
    CHECK(find_in("abc", 0, NULL, "c ", out, 16) == 0, "trailing blank not searched");
    /* on a wrapped row they are */
    CHECK(find_in("abc       ", 1, "d", "c       d", out, 16) == 1, "blanks inside a wrapped line are content");

    /* wide characters and where a hit sits */
    {
        ksm_line l; ksm_char cells[6] = { 'a', 0x4E2D, KSM_WIDE_CONT, 'b', ' ', ' ' };
        ksm_char nd[2] = { 0x4E2D, 'b' };
        int r, c, er, ec;
        memset(&l, 0, sizeof(l));
        ksm_line_add_row(&l, cells, 6, 7, 0, 0);
        CHECK(l.n == 3, "wide character is one character");
        n = ksm_find(&l, nd, 2, 0, NULL, out, 16);
        CHECK(n == 1 && out[0] == 1, "match after a wide character");
        ksm_span(&l, out[0], 2, &r, &c, &er, &ec);
        CHECK(r == 7 && c == 1 && er == 7 && ec == 4, "span: cells 1..3 on row 7");
        ksm_span(&l, 1, 1, &r, &c, &er, &ec);
        CHECK(c == 1 && ec == 3, "the wide character covers two cells");
        ksm_line_free(&l);
    }
    /* WRAPPED2: the last cell is padding */
    {
        ksm_line l; ksm_char r0[4] = { 'x', 'y', 'z', ' ' }, r1[3] = { 0x4E2D, KSM_WIDE_CONT, 'q' };
        ksm_char nd[2] = { 'z', 0x4E2D };
        int r, c, er, ec;
        memset(&l, 0, sizeof(l));
        ksm_line_add_row(&l, r0, 4, 0, 1, 1);
        ksm_line_add_row(&l, r1, 3, 1, 0, 0);
        n = ksm_find(&l, nd, 2, 0, NULL, out, 16);
        CHECK(n == 1, "WRAPPED2 padding skipped: z then the wide character");
        ksm_span(&l, out[0], 2, &r, &c, &er, &ec);
        CHECK(r == 0 && c == 2 && er == 1 && ec == 2, "span across rows");
        ksm_line_free(&l);
    }
    /* empty cells inside a row read as blanks */
    {
        ksm_line l; ksm_char cells[5] = { 'a', 0, 'b', 0, 0 };
        ksm_char nd[3] = { 'a', ' ', 'b' };
        memset(&l, 0, sizeof(l));
        ksm_line_add_row(&l, cells, 5, 0, 0, 0);
        CHECK(l.n == 3, "trailing empty cells trimmed");
        CHECK(ksm_find(&l, nd, 3, 0, NULL, out, 16) == 1, "an empty cell inside is a blank");
        ksm_line_free(&l);
    }

    if (failures) { printf("test_search: %d FAILED\n", failures); return 1; }
    printf("test_search: all passed\n");
    return 0;
}
