/*
 * kitty_sixel.h - Sixel graphics (DEC VT330/VT340, as xterm implements it):
 * the plain-C decoder.
 *
 * The terminal recognises DCS P1 ; P2 ; P3 q and feeds every byte after the
 * 'q' to a decoder as it arrives - nothing is buffered as a string. The
 * decoder keeps the picture as it grows band by band and, at the end of the
 * sequence, hands over premultiplied BGRA (top-down, w * 4 a row) for the
 * image store (kitty_gfx.h). No Windows and no PuTTY types here.
 *
 * Understood: '?'..'~' (six pixels, bit 0 on top, in the current colour),
 * '!n' repeat, '#n' select and '#n;u;x;y;z' define a colour register
 * (u 1: HLS, 2: RGB, both in percent), '$' back to the left edge, '-' the
 * next band of six rows, '"Pan;Pad;Ph;Pv' raster attributes (the size; the
 * aspect is ignored, pixels are square as in xterm). White space and other
 * control bytes are skipped, unknown commands ignored.
 *
 * 256 colour registers, starting from the VT340 palette (16 colours, the
 * rest black); the current register starts at 3, as in xterm. A pixel takes
 * the colour its register has when it is drawn. P2 = 1: pixels never drawn
 * stay transparent; otherwise they get register 0's colour.
 *
 * The picture is w x h with w the widest column drawn (or Ph, if larger)
 * and h the lowest row drawn (or Pv, if larger). Over a cap (a side, the
 * pixel count) the decoder drops what it has and ignores the rest: nothing
 * is placed.
 */
#ifndef KITTY_SIXEL_H
#define KITTY_SIXEL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define SIXEL_REGISTERS 256

/* The colour registers, 0x00RRGGBB. */
typedef struct SixelPalette {
    uint32_t rgb[SIXEL_REGISTERS];
} SixelPalette;

typedef struct SixelDecoder {
    SixelPalette own;          /* private registers */
    SixelPalette *pal;         /* &own, or the terminal's shared registers */
    bool transparent;          /* P2 = 1 */
    int max_side;
    uint32_t max_pixels;
    /* the picture so far: straight RGBA, alpha 0 where nothing was drawn;
     * alloc_w x alloc_h, the drawn part is used_w x used_h */
    unsigned char *px;
    int alloc_w, alloc_h;
    int used_w, used_h;
    int decl_w, decl_h;        /* Ph, Pv */
    int col, row;              /* the next sixel: column, top row of the band */
    int reg;                   /* the current register */
    bool seen_data;            /* a sixel character arrived */
    bool over;                 /* over a cap: everything else ignored */
    bool nomem;                /* ... or out of memory */
    /* a command being read: '!', '#' or '"', 0 none */
    char cmd;
    int np;                    /* parameters started (1 + the ';' seen) */
    long params[5];            /* -1: not given */
    long repeat;               /* a '!' count waiting for its sixel, 0 none */
} SixelDecoder;

/* The VT340's default registers, as xterm sets them. */
void sixel_palette_default(SixelPalette *p);

/* HLS (hue in degrees, blue at 0; lightness, saturation in percent) to RGB
 * in percent, xterm's conversion. */
void sixel_hls_to_rgb(int h, int l, int s, int *r, int *g, int *b);

/* Starts a picture: params / nparams are P1;P2;P3 of the DCS (at most three
 * read; missing ones 0). shared: the registers to use and keep (?1070
 * reset), NULL: private registers from the default palette. The caps: a
 * side, the pixel count. */
void sixel_init(SixelDecoder *d, const unsigned *params, int nparams,
                SixelPalette *shared, int max_side, uint32_t max_pixels);

/* The bytes of the sixel data as they arrive. */
void sixel_feed(SixelDecoder *d, const unsigned char *s, size_t len);

/* The end of the sequence: the picture as premultiplied BGRA, malloc'd,
 * w * 4 a row. False when there is nothing to place (no sixel character,
 * an empty picture, over a cap, out of memory); *why then says which:
 * "EFBIG" for a cap, "ENOMEM", or "" for nothing drawn. The decoder is
 * freed either way. */
bool sixel_finish(SixelDecoder *d, unsigned char **px, int *w, int *h,
                  const char **why);

/* Frees what the decoder holds (an abandoned sequence). */
void sixel_free(SixelDecoder *d);

#endif /* KITTY_SIXEL_H */
