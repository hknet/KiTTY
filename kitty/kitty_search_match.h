/*
 * kitty_search_match.h - the platform-free half of the scrollback search
 * (Ctrl+Shift+F): a logical line built from terminal rows (a soft-wrapped
 * line is one logical line, so a hit may span the wrap), and the matcher
 * (plain text, smart case: case-insensitive unless the search text holds a
 * capital letter). Dependency-free and static inline, shared verbatim with
 * test/test_search.c; terminal/kitty_search.c feeds it the real rows.
 */
#ifndef KITTY_SEARCH_MATCH_H
#define KITTY_SEARCH_MATCH_H

#include <stdlib.h>
#include <string.h>

typedef unsigned int ksm_char;            /* one Unicode code point */
#define KSM_WIDE_CONT 0xFFFFFFFFu         /* the right half of a wide character */

/* One logical line: hay[i] is a character, row[i] / col[i] the cell it
 * starts in, w[i] its width in cells (1 or 2). */
typedef struct {
    ksm_char *hay;
    int *row, *col;
    unsigned char *w;
    int n, cap;
} ksm_line;

static inline void ksm_line_free(ksm_line *l)
{
    free(l->hay); free(l->row); free(l->col); free(l->w);
    memset(l, 0, sizeof(*l));
}

static inline int ksm_line_push(ksm_line *l, ksm_char c, int row, int col, int w)
{
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 256;
        ksm_char *h = (ksm_char *)realloc(l->hay, cap * sizeof(*h));
        int *r = h ? (int *)realloc(l->row, cap * sizeof(*r)) : NULL;
        int *c2 = r ? (int *)realloc(l->col, cap * sizeof(*c2)) : NULL;
        unsigned char *w2 = c2 ? (unsigned char *)realloc(l->w, cap) : NULL;
        if (h) l->hay = h;
        if (r) l->row = r;
        if (c2) l->col = c2;
        if (!w2) return 0;
        l->w = w2;
        l->cap = cap;
    }
    l->hay[l->n] = c; l->row[l->n] = row; l->col[l->n] = col;
    l->w[l->n] = (unsigned char)w;
    l->n++;
    return 1;
}

/* Append one terminal row: cells[0..ncells), each a code point (0 = an
 * empty cell, KSM_WIDE_CONT = the right half of the wide character before
 * it). wrapped: the row continues on the next one, so its trailing blanks
 * are content; else they are not searched. wrapped2: its last cell is
 * padding (a wide character that did not fit went to the next row). */
static inline void ksm_line_add_row(ksm_line *l, const ksm_char *cells,
                                    int ncells, int row, int wrapped,
                                    int wrapped2)
{
    int end = ncells, x;
    if (wrapped2 && end > 0)
        end--;
    if (!wrapped)
        while (end > 0 && (cells[end - 1] == ' ' || cells[end - 1] == 0))
            end--;
    for (x = 0; x < end; x++) {
        int w;
        if (cells[x] == KSM_WIDE_CONT)
            continue;
        w = (x + 1 < ncells && cells[x + 1] == KSM_WIDE_CONT) ? 2 : 1;
        if (!ksm_line_push(l, cells[x] ? cells[x] : ' ', row, x, w))
            return;
    }
}

/* Lower case for the letters that have it in the scripts a terminal
 * mostly shows: ASCII, Latin-1, Latin Extended-A (pairs), Greek and
 * Cyrillic. The caller may pass a better one (towlower). */
static inline ksm_char ksm_lower_basic(ksm_char c)
{
    if (c >= 'A' && c <= 'Z') return c + 32;
    if ((c >= 0xC0 && c <= 0xDE) && c != 0xD7) return c + 32;
    if (c == 0x178) return 0xFF;                                  /* Y diaeresis */
    if (c >= 0x100 && c <= 0x17F && c != 0x130 && c != 0x131 && c != 0x138 &&
        c != 0x149 && c != 0x17F) {
        /* mostly even = upper, odd = lower; 0x139-0x148 and 0x179-0x17E
         * are the other way round */
        if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E))
            return (c & 1) ? c + 1 : c;
        return (c & 1) ? c : c + 1;
    }
    if (c >= 0x391 && c <= 0x3AB && c != 0x3A2) return c + 32;   /* Greek */
    if (c >= 0x410 && c <= 0x42F) return c + 32;                 /* Cyrillic */
    if (c >= 0x400 && c <= 0x40F) return c + 80;
    return c;
}

typedef ksm_char (*ksm_lower_fn)(ksm_char);

/* Smart case: the search is case-sensitive as soon as the text holds a
 * character that has a lower case of its own. */
static inline int ksm_case_sensitive(const ksm_char *needle, int nn,
                                     ksm_lower_fn lower)
{
    int i;
    if (!lower) lower = ksm_lower_basic;
    for (i = 0; i < nn; i++)
        if (lower(needle[i]) != needle[i])
            return 1;
    return 0;
}

/* The matches of needle in the line, left to right, not overlapping: their
 * start positions into out[0..max). Returns how many there are (it may be
 * more than max; only max are stored). An empty needle matches nothing. */
static inline int ksm_find(const ksm_line *l, const ksm_char *needle, int nn,
                           int case_sensitive, ksm_lower_fn lower,
                           int *out, int max)
{
    int i, j, count = 0;
    if (!lower) lower = ksm_lower_basic;
    if (nn <= 0)
        return 0;
    for (i = 0; i + nn <= l->n; ) {
        for (j = 0; j < nn; j++) {
            ksm_char a = l->hay[i + j], b = needle[j];
            if (a == b) continue;
            if (!case_sensitive && lower(a) == lower(b)) continue;
            break;
        }
        if (j == nn) {
            if (count < max) out[count] = i;
            count++;
            i += nn;
        } else {
            i++;
        }
    }
    return count;
}

/* Where a match at pos (of nn characters) sits: the first cell (row, col)
 * and the cell after its last one (end_row, end_col) - end_col is col +
 * width of the last character, on that character's row. */
static inline void ksm_span(const ksm_line *l, int pos, int nn,
                            int *row, int *col, int *end_row, int *end_col)
{
    int last = pos + nn - 1;
    *row = l->row[pos];
    *col = l->col[pos];
    *end_row = l->row[last];
    *end_col = l->col[last] + l->w[last];
}

#endif
