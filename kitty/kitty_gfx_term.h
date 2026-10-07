/*
 * kitty_gfx_term.h - the kitty graphics protocol: the terminal's side
 * (kitty_gfx_term.c). The parser, the store and the placement list are in
 * kitty_gfx.h; this joins them to a Terminal: the APC _G dispatch, the cell
 * size the window reports, the decoders the window binds, the absolute line
 * numbering the anchors use, and the repaint of the cells an image covers.
 * The hooks below are what terminal.c calls where lines move.
 */
#ifndef KITTY_GFX_TERM_H
#define KITTY_GFX_TERM_H

#include <stdbool.h>
#include <stddef.h>

/* Set by the window (windows/window.c): PNG to premultiplied BGRA, and zlib
 * inflate with an output cap. NULL where no window decodes (tests): the
 * core answers PNG and compression with EINVAL. The GfxEnv signatures. */
extern bool (*kitty_gfx_decode_png_hook)(void *ctx, const unsigned char *data,
                                         size_t len, unsigned char **px,
                                         int *w, int *h);
extern bool (*kitty_gfx_inflate_hook)(void *ctx, const unsigned char *in,
                                      size_t len, size_t max_out,
                                      unsigned char **out, size_t *outlen);
/* JPEG, GIF, TIFF, BMP (fmt GFX_FILE_*) to premultiplied BGRA, for iTerm2
 * images; NULL: those refused. */
extern bool (*kitty_gfx_decode_file_hook)(void *ctx, int fmt,
                                          const unsigned char *data,
                                          size_t len, unsigned char **px,
                                          int *w, int *h);

/* Serve one APC _G: s/len is everything after the 'G'. cut: the sequence
 * was cut at its ceiling (answered EFBIG). dx and dy get how far the cursor
 * moves right/down (0 when it stays); terminal.c applies them, the rows as
 * line feeds (scrolling at the bottom), the columns clamped to the line.
 * Creates the store on the first call. */
void kitty_gfx_apc(Terminal *term, const char *s, int len, bool cut,
                   int *dx, int *dy);

/* Serve one OSC 1337 (iTerm2 inline images): s/len is the string after
 * "1337;". Other 1337 commands are left alone. cut: the sequence was cut at
 * its ceiling (the image is refused). No reply; dx and dy as above. */
void kitty_gfx_iterm(Terminal *term, const char *s, int len, bool cut,
                     int *dx, int *dy);

/* Sixel (DCS P1 ; P2 ; P3 q ... ST, kitty/kitty_sixel.h). terminal.c calls
 * start when the 'q' arrives (params: P1;P2;P3 as given, at most three),
 * feed with every byte after it, end at the ST, abort when the sequence is
 * abandoned (CR/LF do not abandon it; an ESC not followed by '' does).
 * end places the picture through the store as an anonymous image and gives
 * the cursor move: DECSDM set (term->sixel_decsdm) - at the top-left of the
 * screen, the cursor stays; otherwise at the cursor. With ?8452 set
 * (term->sixel_scrolls_right) the cursor goes just right of the picture on
 * its last row - and to the left edge of the next row when that is past the
 * right edge (xterm). Otherwise, until a program sets or resets ?7730
 * (term->sixel_7730_seen), to the picture's last row, in the column where
 * it started (xterm; chafa sends ?8452 l and expects this); after that to
 * the line below the picture, in that column (mintty, mlterm, VTE,
 * konsole), with ?7730 set (term->sixel_cursor_line_start) at the start of
 * that line. dy is applied as line feeds (scrolling at the bottom), as for
 * the other two protocols. mintty's ?7780 (term->gfx_no_scroll), for
 * Sixel and iTerm2 pictures alike: nothing scrolls - a picture passing the
 * bottom margin is cut there - and the cursor stays. */
void kitty_gfx_sixel_start(Terminal *term, const unsigned *params,
                           int nparams);
void kitty_gfx_sixel_feed(Terminal *term, unsigned char c);
void kitty_gfx_sixel_end(Terminal *term, int *dx, int *dy);
void kitty_gfx_sixel_abort(Terminal *term);

/* The absolute line number of row 0 of the screen shown (alt_which):
 * gfx_sb_base + the scrollback count on the main screen, 0 on the alternate. */
long kitty_gfx_top_abs(Terminal *term);

/* scroll() moved rows top..bot by lines (> 0 up); before is the top_abs
 * read before the move. Decides between "into the scrollback" and a region
 * scroll from how top_abs changed. */
void kitty_gfx_scrolled(Terminal *term, int top, int bot, int lines,
                        long before);

/* The scrollback was cleared (term_clrsb): sblen entries went, the screen
 * keeps its numbers. */
void kitty_gfx_sb_cleared(Terminal *term, int sblen);

/* Scrollback entries were dropped from the top (term_size): the base has
 * been bumped, the placements above it go. */
void kitty_gfx_sb_trimmed(Terminal *term);

/* term_size deleted the main screen's bottom row, absolute number abs_line:
 * the placements starting on or below it go (they would come back over
 * blank rows when the screen grows). */
void kitty_gfx_row_deleted(Terminal *term, long abs_line);

/* ED 2: every placement on the screen shown goes. */
void kitty_gfx_clear_screen(Terminal *term);

/* The alternate screen was entered, left or rebuilt: its placements go. */
void kitty_gfx_alt_cleared(Terminal *term, bool repaint);

/* A terminal reset: everything goes, a pending upload with it, a Sixel
 * being received and the shared Sixel registers too. */
void kitty_gfx_reset(Terminal *term);

/* term_free. */
void kitty_gfx_free(Terminal *term);

/* The cell size changed (term_notify_cell_size_pixels): placements given
 * in cells are re-fitted and repainted. */
void kitty_gfx_cell_size_changed(Terminal *term);

/* Cells x0..x1 of screen rows y0..y1 (the screen shown) were written into
 * or erased: the iTerm2 and Sixel pictures there lose those cells, a
 * picture with none left goes, and the cells are drawn again. terminal.c
 * calls it through KITTY_GFX_CELLS, which costs one test while no such
 * picture exists. */
void kitty_gfx_cells_changed(Terminal *term, int y0, int y1, int x0, int x1);
#define KITTY_GFX_CELLS(term, y0, y1, x0, x1) do {                                  if ((term)->gfx && (term)->gfx->n_cellbound > 0)                                kitty_gfx_cells_changed(term, y0, y1, x0, x1);                      } while (0)

/* Whether any placement exists on the screen shown. */
bool kitty_gfx_any(Terminal *term);

/* The view moved (term_scroll changed disptop) with placements on the
 * screen shown: every row is drawn again. The comparison alone redraws
 * only cells whose text changed, and a GDI window keeps an image's old
 * pixels wherever the text under them was blank before and after. */
void kitty_gfx_view_moved(Terminal *term);

#endif /* KITTY_GFX_TERM_H */
