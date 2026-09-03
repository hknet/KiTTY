/*
 * paint.h - the terminal window's painter.
 *
 * The terminal core never draws; it calls the window's TermWin table, and
 * the drawing half of that table (setup_draw_ctx, draw_text, draw_cursor,
 * draw_trust_sigil, free_draw_ctx, char_width) ends in a dozen kinds of GDI
 * call. This puts those calls behind one table so the same window can paint
 * with GDI or, later, with a GPU renderer chosen when the window is created.
 * The attribute logic (fonts, bold, underline, wide and combining
 * characters, the cursor shapes, the fork's background image and TuTTY
 * colours) stays in window.c; only the drawing primitives go through here.
 *
 * Coordinates are window client pixels, exactly as the GDI calls took them.
 * Colours are COLORREFs. Fonts are the window's HFONTs (a non-GDI painter
 * maps them to its own faces by the LOGFONT it can read off them).
 */
#ifndef PUTTY_WINDOWS_PAINT_H
#define PUTTY_WINDOWS_PAINT_H

#include <windows.h>
#include <stdbool.h>

typedef struct KittyPainter KittyPainter;

typedef struct KittyPainterVtable {
    /* A frame. `given` is the DC WM_PAINT already holds (BeginPaint), or
     * NULL for a paint the terminal asked for itself; the painter keeps
     * whatever it needs until end(). Returns false if it cannot draw now. */
    bool (*begin)(KittyPainter *p, HDC given);
    void (*end)(KittyPainter *p);
    /* The client area changed size (WM_SIZE); a painter with its own
     * surfaces re-creates them. GDI has nothing to do. */
    void (*resize)(KittyPainter *p, int width, int height);
    /* Free the painter and everything it holds. */
    void (*destroy)(KittyPainter *p);

    /* Text state for the runs that follow: font, colours, whether the run's
     * background is filled (opaque) and whether glyphs are centred in their
     * cell (variable-pitch fonts) or laid left-to-right. */
    void (*style)(KittyPainter *p, HFONT font, COLORREF fg, COLORREF bg,
                  bool opaque, bool centre);
    /* Change only the opaque/transparent state (the shadow-bold second
     * pass and the "stop erasing after the first run" rule). */
    void (*opaque)(KittyPainter *p, bool opaque);

    /* One run of glyphs at (x, y), clipped to `clip`, with per-glyph
     * advances `dx` (NULL: the font's own). `opaque` fills the clip
     * rectangle with the background first. text_w takes UTF-16, text_a the
     * single-byte form the DIRECT_FONT path uses. text_general is the
     * RTL-aware variant: runs of right-to-left characters are placed glyph
     * by glyph so Windows does no shaping of its own (`varpitch` disables
     * the advances, as the variable-pitch path requires). */
    void (*text_w)(KittyPainter *p, int x, int y, const RECT *clip,
                   bool opaque, const wchar_t *s, int n, const int *dx);
    void (*text_a)(KittyPainter *p, int x, int y, const RECT *clip,
                   bool opaque, const char *s, int n, const int *dx);
    void (*text_general)(KittyPainter *p, int x, int y, const RECT *clip,
                         bool opaque, bool varpitch,
                         const wchar_t *s, int n, const int *dx);

    /* The background image behind a run: copy the rectangle `dst` from the
     * image DC at (sx, sy) - GDI's BitBlt. A GPU painter that draws the
     * image once per frame may make this a no-op. */
    void (*blit_background)(KittyPainter *p, const RECT *dst,
                            HDC src, int sx, int sy);

    /* Lines and points: underline / strike-through / hyperlink underline,
     * the cursor's line forms and the passive cursor's dotted line. */
    void (*line)(KittyPainter *p, int x0, int y0, int x1, int y1, COLORREF c);
    void (*rect_outline)(KittyPainter *p, const RECT *r, COLORREF c);
    void (*pixel)(KittyPainter *p, int x, int y, COLORREF c);

    /* The trust sigil. */
    void (*icon)(KittyPainter *p, int x, int y, HICON ic, int w, int h);

    /* WM_PAINT's border: fill `paint` except the terminal area `keep`. */
    void (*fill_outside)(KittyPainter *p, const RECT *paint,
                         const RECT *keep, COLORREF c);

    /* The advance width of `ch` in `font`, for the dual-width check
     * (`wide`: the character is UTF-16, else a font-direct byte). Returns
     * false when the font cannot say. */
    bool (*char_width)(KittyPainter *p, HFONT font, unsigned ch, bool wide,
                       int *width);

    /* The GDI device context of the current frame, or NULL for a painter
     * that has none. The escape hatch for the one caller that still draws
     * with GDI directly (the font-fallback module's per-font runs). */
    HDC (*hdc)(KittyPainter *p);
} KittyPainterVtable;

struct KittyPainter {
    const KittyPainterVtable *vt;
};

/* The GDI painter: what the window always had. `pal` points at the window's
 * palette handle, read at every frame (it can be created later). */
KittyPainter *kitty_painter_gdi_new(HWND hwnd, HPALETTE *pal);

/* The Direct2D + DirectWrite painter (paint-d2d.c, KiTTY targets only):
 * NULL when the machine cannot provide it, and the caller stays on GDI.
 * font_quality is the session's FQ_* setting (the antialiasing mode). */
KittyPainter *kitty_painter_d2d_new(HWND hwnd, int font_quality);

#define kitty_painter_free(p)            ((p)->vt->destroy(p))

/* Convenience wrappers so call sites read as drawing, not as tables. */
#define kp_begin(p, dc)                  ((p)->vt->begin((p), (dc)))
#define kp_end(p)                        ((p)->vt->end(p))
#define kp_resize(p, w, h)               ((p)->vt->resize((p), (w), (h)))
#define kp_style(p, f, fg, bg, op, ce)   ((p)->vt->style((p), (f), (fg), (bg), (op), (ce)))
#define kp_opaque(p, op)                 ((p)->vt->opaque((p), (op)))
#define kp_text_w(p, x, y, c, op, s, n, dx)   ((p)->vt->text_w((p), (x), (y), (c), (op), (s), (n), (dx)))
#define kp_text_a(p, x, y, c, op, s, n, dx)   ((p)->vt->text_a((p), (x), (y), (c), (op), (s), (n), (dx)))
#define kp_text_general(p, x, y, c, op, vp, s, n, dx) \
    ((p)->vt->text_general((p), (x), (y), (c), (op), (vp), (s), (n), (dx)))
#define kp_blit_background(p, d, src, sx, sy) ((p)->vt->blit_background((p), (d), (src), (sx), (sy)))
#define kp_line(p, x0, y0, x1, y1, c)    ((p)->vt->line((p), (x0), (y0), (x1), (y1), (c)))
#define kp_rect_outline(p, r, c)         ((p)->vt->rect_outline((p), (r), (c)))
#define kp_pixel(p, x, y, c)             ((p)->vt->pixel((p), (x), (y), (c)))
#define kp_icon(p, x, y, ic, w, h)       ((p)->vt->icon((p), (x), (y), (ic), (w), (h)))
#define kp_fill_outside(p, pr, kr, c)    ((p)->vt->fill_outside((p), (pr), (kr), (c)))
#define kp_char_width(p, f, ch, w, out)  ((p)->vt->char_width((p), (f), (ch), (w), (out)))
#define kp_hdc(p)                        ((p)->vt->hdc(p))

#endif /* PUTTY_WINDOWS_PAINT_H */
