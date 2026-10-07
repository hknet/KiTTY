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

/* Serve one APC _G: s/len is everything after the 'G'. cut: the sequence
 * was cut at its ceiling (answered EFBIG). dx and dy get how far the cursor
 * moves right/down (0 when it stays); terminal.c applies them with its own
 * clamping. Creates the store on the first call. */
void kitty_gfx_apc(Terminal *term, const char *s, int len, bool cut,
                   int *dx, int *dy);

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

/* A terminal reset: everything goes, a pending upload with it. */
void kitty_gfx_reset(Terminal *term);

/* term_free. */
void kitty_gfx_free(Terminal *term);

/* The cell size changed (term_notify_cell_size_pixels): placements given
 * in cells are re-fitted and repainted. */
void kitty_gfx_cell_size_changed(Terminal *term);

/* Whether any placement exists on the screen shown. */
bool kitty_gfx_any(Terminal *term);

/* The view moved (term_scroll changed disptop) with placements on the
 * screen shown: every row is drawn again. The comparison alone redraws
 * only cells whose text changed, and a GDI window keeps an image's old
 * pixels wherever the text under them was blank before and after. */
void kitty_gfx_view_moved(Terminal *term);

#endif /* KITTY_GFX_TERM_H */
