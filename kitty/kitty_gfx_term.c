/*
 * kitty_gfx_term.c - the kitty graphics protocol (APC _G): the terminal's
 * side.
 *
 * The command is parsed and the store kept by kitty_gfx.c - also for the
 * iTerm2 inline images of OSC 1337, which share it; this file gives
 * it what only the terminal and the window know - the cell size the window
 * reported (term->cellpix_x/y), the cursor, which screen is shown, the
 * absolute number of the top screen row, the decoders the window bound -
 * sends the reply through the raw seam the OSC 52 and far2l replies use,
 * and turns every change into a repaint of the cells the images covered
 * and cover now (the painters draw them over the text at the end of the
 * frame, so the text under them has to be drawn again first, exactly as
 * for far2l images).
 *
 * Lines are numbered absolutely so that a placement keeps its anchor while
 * its text scrolls into the scrollback: term->gfx_sb_base is the absolute
 * number of scrollback entry 0, bumped by terminal.c wherever an entry is
 * dropped from the top (scroll, term_size); row 0 of the main screen is
 * then base + scrollback count, and the hooks below need no other state.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "putty.h"
#include "terminal.h"
#include "kitty_gfx.h"
#include "kitty_gfx_term.h"
#include "kitty_osc52.h"           /* kitty_osc52_send_raw: the raw reply seam */
#include "kitty_buildlabel.h"      /* the test build's trace hook */

bool (*kitty_gfx_decode_png_hook)(void *ctx, const unsigned char *data,
                                  size_t len, unsigned char **px,
                                  int *w, int *h);
bool (*kitty_gfx_inflate_hook)(void *ctx, const unsigned char *in,
                               size_t len, size_t max_out,
                               unsigned char **out, size_t *outlen);
bool (*kitty_gfx_decode_file_hook)(void *ctx, int fmt,
                                   const unsigned char *data, size_t len,
                                   unsigned char **px, int *w, int *h);

#define GFX_REPAINT_MAX 64

/* terminal.h's OSC 1337 ceiling holds the store's image cap in base64 */
typedef char gfx_iterm_ceiling_check[
    OSC_STR_MAX_ITERM >= GFX_PENDING_MAX / 3 * 4 + 4096 ? 1 : -1];

long kitty_gfx_top_abs(Terminal *term)
{
    if (term->alt_which)
        return 0;
    return term->gfx_sb_base +
           (term->scrollback ? count234(term->scrollback) : 0);
}

static int screen_of(Terminal *term)
{
    return term->alt_which ? 1 : 0;
}

#ifdef KITTY_TEST_BUILD_LABEL
/*
 * KITTY_GFX_TRACE=<file> (test builds only): one line per command, for
 * the harness: "<action> id=<id> <code>" (code OK or the error code).
 */
static void gfx_trace(void *ctx, char a, uint32_t id, const char *code)
{
    const char *path = getenv("KITTY_GFX_TRACE");
    FILE *f;
    (void)ctx;
    if (!path || !*path)
        return;
    f = fopen(path, "a");
    if (!f)
        return;
    fprintf(f, "%c id=%lu %s\n", a, (unsigned long)id, code);
    fclose(f);
}
#endif

static void env_init(Terminal *term, GfxEnv *env)
{
    memset(env, 0, sizeof(*env));
    env->cell_w = term->cellpix_x;
    env->cell_h = term->cellpix_y;
    env->cols = term->cols;
    env->rows = term->rows;
    env->screen = screen_of(term);
    env->top_abs = kitty_gfx_top_abs(term);
    env->cur_x = term->curs.x;
    env->cur_y = term->curs.y;
    env->decode_png = kitty_gfx_decode_png_hook;
    env->decode_file = kitty_gfx_decode_file_hook;
    env->inflate = kitty_gfx_inflate_hook;
#ifdef KITTY_TEST_BUILD_LABEL
    env->trace = gfx_trace;
#endif
    env->ctx = term;
}

/* The cells the visible placements cover, as rectangles (inclusive), at
 * most GFX_REPAINT_MAX; returns -1 when there are more (paint everything). */
static int covered(Terminal *term, int rects[GFX_REPAINT_MAX][4])
{
    GfxVisible vis[GFX_REPAINT_MAX + 1];
    int n, i;
    if (!term->gfx)
        return 0;
    n = term_gfx_visible(term, vis, GFX_REPAINT_MAX + 1);
    if (n > GFX_REPAINT_MAX)
        return -1;
    for (i = 0; i < n; i++) {
        rects[i][0] = vis[i].c0;
        rects[i][1] = vis[i].r0;
        rects[i][2] = vis[i].c1;
        rects[i][3] = vis[i].r1;
    }
    return n;
}

static void paint_rects(Terminal *term, int rects[GFX_REPAINT_MAX][4], int n)
{
    int i;
    if (n < 0) {
        term_paint(term, 0, 0, term->cols - 1, term->rows - 1, false);
        return;
    }
    for (i = 0; i < n; i++)
        term_paint(term, rects[i][0], rects[i][1], rects[i][2], rects[i][3],
                   false);
}

/* The cells under every visible placement are drawn again. */
static void repaint_covered(Terminal *term)
{
    int rects[GFX_REPAINT_MAX][4];
    int n = covered(term, rects);
    if (n)
        paint_rects(term, rects, n);
}

/* The two protocols' commands, served the same way. */
enum { GFX_KITTY, GFX_KITTY_CUT, GFX_ITERM, GFX_ITERM_CUT };

static void serve(Terminal *term, int which, const char *s, int len,
                  int *dx, int *dy)
{
    GfxEnv env;
    GfxResult res;
    int before[GFX_REPAINT_MAX][4];
    int nbefore;
    const unsigned char *u = (const unsigned char *)s;
    size_t n = len > 0 ? (size_t)len : 0;

    *dx = *dy = 0;
    if (!term->gfx) {
        term->gfx = snew(GfxStore);
        gfx_store_init(term->gfx);
    }
    env_init(term, &env);
    nbefore = covered(term, before);
    switch (which) {
      case GFX_KITTY_CUT:
        gfx_command_cut(term->gfx, &env, u, n, &res);
        break;
      case GFX_KITTY:
        gfx_command(term->gfx, &env, u, n, &res);
        break;
      default:
        gfx_iterm(term->gfx, &env, u, n, which == GFX_ITERM_CUT, &res);
        break;
    }
    if (res.reply_len > 0)
        kitty_osc52_send_raw(term, res.reply, (size_t)res.reply_len);
    if (res.changed) {
        if (nbefore)
            paint_rects(term, before, nbefore);
        repaint_covered(term);
    }
    *dx = res.cur_dx;
    *dy = res.cur_dy;
}

void kitty_gfx_apc(Terminal *term, const char *s, int len, bool cut,
                   int *dx, int *dy)
{
    serve(term, cut ? GFX_KITTY_CUT : GFX_KITTY, s, len, dx, dy);
}

void kitty_gfx_iterm(Terminal *term, const char *s, int len, bool cut,
                     int *dx, int *dy)
{
    *dx = *dy = 0;
    /* the other OSC 1337 commands stay ignored, and create no store */
    if (len <= 0 || !gfx_iterm_is_image((const unsigned char *)s, (size_t)len))
        return;
    serve(term, cut ? GFX_ITERM_CUT : GFX_ITERM, s, len, dx, dy);
}

int term_gfx_visible(Terminal *term, struct GfxVisible *out, int max)
{
    if (!term->gfx || max <= 0)
        return 0;
    /* the alternate screen scrolled back (erase_to_scrollback's virtual
     * scrollback): the view shows no placement, the numbering is the main
     * screen's */
    if (term->alt_which && term->disptop < 0)
        return 0;
    return gfx_visible(term->gfx, screen_of(term),
                       kitty_gfx_top_abs(term) + term->disptop,
                       term->rows, term->cols, term->cellpix_x,
                       term->cellpix_y, out, max);
}

/* ---- line movement ------------------------------------------------------ */

void kitty_gfx_scrolled(Terminal *term, int top, int bot, int lines,
                        long before)
{
    long after;
    int band = bot - top + 1;
    if (!term->gfx || lines == 0)
        return;
    if (lines > band)
        lines = band;
    if (lines < -band)
        lines = -band;
    after = kitty_gfx_top_abs(term);
    if (after != before) {
        /* lines entered the scrollback: anchors stay, the text moved */
        if (bot >= term->rows - 1)
            gfx_set_base(term->gfx, 0, term->gfx_sb_base);
        else
            gfx_scroll_region_sb(term->gfx, 0, after, bot, (int)(after - before));
    } else {
        gfx_scroll_region(term->gfx, screen_of(term), after, top, bot, lines,
                          term->cellpix_x, term->cellpix_y,
                          bot >= term->rows - 1);
    }
}

void kitty_gfx_sb_cleared(Terminal *term, int sblen)
{
    term->gfx_sb_base += sblen;
    if (term->gfx)
        gfx_set_base(term->gfx, 0, term->gfx_sb_base);
}

void kitty_gfx_sb_trimmed(Terminal *term)
{
    if (term->gfx)
        gfx_set_base(term->gfx, 0, term->gfx_sb_base);
}

void kitty_gfx_row_deleted(Terminal *term, long abs_line)
{
    if (term->gfx)
        gfx_clear_below(term->gfx, 0, abs_line);
}

void kitty_gfx_clear_screen(Terminal *term)
{
    if (!term->gfx)
        return;
    repaint_covered(term);
    gfx_clear_screen(term->gfx, screen_of(term), kitty_gfx_top_abs(term));
}

void kitty_gfx_alt_cleared(Terminal *term, bool repaint)
{
    bool had;
    if (!term->gfx)
        return;
    /* repaint (the screen switch): with any picture on either screen the
     * switch shows the other screen's pictures (or none), and the GDI
     * overlay draws only inside cells a frame touches - blank cells under a
     * dropped picture would keep it. Not from term_size, which rebuilds
     * the display buffer itself while the grid is half resized. */
    had = term->gfx->n_pls > 0;
    gfx_clear_all(term->gfx, 1);
    if (repaint && had)
        term_paint(term, 0, 0, term->cols - 1, term->rows - 1, false);
}

void kitty_gfx_reset(Terminal *term)
{
    if (term->gfx)
        gfx_reset(term->gfx);
}

void kitty_gfx_free(Terminal *term)
{
    if (!term->gfx)
        return;
    gfx_store_free(term->gfx);
    sfree(term->gfx);
    term->gfx = NULL;
}

void kitty_gfx_cell_size_changed(Terminal *term)
{
    if (!term->gfx)
        return;
    repaint_covered(term);
    gfx_rescale(term->gfx, term->cellpix_x, term->cellpix_y);
    repaint_covered(term);
}

void kitty_gfx_view_moved(Terminal *term)
{
    if (kitty_gfx_any(term))
        term_paint(term, 0, 0, term->cols - 1, term->rows - 1, false);
}

bool kitty_gfx_any(Terminal *term)
{
    int i, screen;
    if (!term || !term->gfx)
        return false;
    screen = screen_of(term);
    for (i = 0; i < term->gfx->n_pls; i++)
        if (term->gfx->pls[i].screen == screen)
            return true;
    return false;
}
