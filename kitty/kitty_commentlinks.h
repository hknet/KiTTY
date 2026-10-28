/*
 * kitty_commentlinks.h - the links in a session comment and the comment
 * wrapped to a width: the part of the Session panel's comment box
 * (kitty_commentview.c) that needs no window, so the unit test
 * (test/test_commentlinks.c) runs it as it is.
 *
 * Text is UTF-16 (wchar_t), offsets are in UTF-16 units.
 */
#ifndef KITTY_COMMENTLINKS_H
#define KITTY_COMMENTLINKS_H

#include <wchar.h>

/* One link: text[start .. end). */
typedef struct { int start, end; } kcl_span;

/*
 * Every link in text[0 .. n), in text order, none overlapping:
 *  - what the terminal's default expression finds (KITTY_URL_REGEX_DEFAULT:
 *    http, https, ftp, www.);
 *  - file:// links (file:///C:/doc.pdf, file://server/share/x);
 *  - UNC paths, \\server\share\... (server and share required).
 * A link ends at white space, except inside double quotes or <...>, where
 * it is everything between them ("\\srv\docs\my runbook.pdf" is one link,
 * without the quotes). A trailing . , ; : ) is not part of a file:// or UNC
 * link (the expression has the same rule for web links).
 * *out is malloc'd (free it), NULL when there is none. Returns the count.
 */
int kcl_find_links(const wchar_t *text, int n, kcl_span **out);

/* What opening the link starts: its text, "www." given "http://" in front.
 * malloc'd, NUL-terminated; NULL when out of memory. */
wchar_t *kcl_link_target(const wchar_t *text, const kcl_span *link);

/*
 * The text wrapped to maxw: a run is one piece of one line, either plain
 * text or (link >= 0) a piece of links[link]. A link wrapped over two
 * lines has a run on each - each run is a rectangle for hit-testing.
 * x and w are the run's place and width on its line, from width().
 * CR LF, CR and LF end a line; a line too long for maxw breaks after its
 * last space, a word longer than the line within the word. White space at
 * a wrap is dropped. Every line is counted, empty ones too (nlines).
 */
typedef struct { int line, start, end, x, w, link; } kcl_run;
typedef struct { kcl_run *runs; int nruns, nlines; } kcl_layout;
/* The width of s[0 .. n) in the units of maxw (pixels in the window). */
typedef int (*kcl_width_fn)(void *ctx, const wchar_t *s, int n);

void kcl_layout_build(kcl_layout *out, const wchar_t *text, int n,
                      const kcl_span *links, int nlinks, int maxw,
                      kcl_width_fn width, void *ctx);
void kcl_layout_free(kcl_layout *l);

#endif
