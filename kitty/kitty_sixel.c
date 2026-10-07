/*
 * kitty_sixel.c - Sixel graphics: the streaming decoder (kitty_sixel.h).
 *
 * The data is read one byte at a time, so the terminal can feed it as it
 * arrives. A command with parameters ('!', '#', '"') is read until the
 * first byte that is neither a digit, a ';' nor white space; that byte ends
 * it and is then served itself. The picture is kept as straight RGBA with
 * alpha 0 where nothing was drawn, grown as the drawing reaches past it,
 * and turned into premultiplied BGRA at the end.
 *
 * Every pixel takes the colour its register has when it is drawn: an
 * encoder that redefines registers band by band to show more than 256
 * colours is drawn as it meant to be (xterm keeps register numbers and
 * recolours earlier pixels instead).
 */
#include <stdlib.h>
#include <string.h>

#include "kitty_sixel.h"

/* a parameter or repeat count beyond this is taken as this */
#define SIXEL_PARAM_MAX (1L << 24)

static uint32_t pct_rgb(int r, int g, int b)
{
    return ((uint32_t)((r * 255 + 50) / 100) << 16) |
           ((uint32_t)((g * 255 + 50) / 100) << 8) |
           (uint32_t)((b * 255 + 50) / 100);
}

void sixel_palette_default(SixelPalette *p)
{
    /* the VT340's 16, in percent; the other registers start black */
    static const unsigned char vt340[16][3] = {
        {  0,  0,  0 }, { 20, 20, 80 }, { 80, 13, 13 }, { 20, 80, 20 },
        { 80, 20, 80 }, { 20, 80, 80 }, { 80, 80, 20 }, { 53, 53, 53 },
        { 26, 26, 26 }, { 33, 33, 60 }, { 60, 26, 26 }, { 33, 60, 33 },
        { 60, 33, 60 }, { 33, 60, 60 }, { 60, 60, 33 }, { 80, 80, 80 },
    };
    int i;
    memset(p, 0, sizeof(*p));
    for (i = 0; i < 16; i++)
        p->rgb[i] = pct_rgb(vt340[i][0], vt340[i][1], vt340[i][2]);
}

static int clamp_pct(double v)
{
    int n = (int)(v * 100 + 0.5);
    return n < 0 ? 0 : n > 100 ? 100 : n;
}

void sixel_hls_to_rgb(int h, int l, int s, int *r, int *g, int *b)
{
    double lv = l / 100.0, sv = s / 100.0, c, x, m, c2;
    double r1, g1, b1;
    int hs;

    if (s == 0) {                       /* no colour: grey of that lightness */
        *r = *g = *b = l;
        return;
    }
    h -= 120;                           /* blue at 0 degrees */
    while (h < 0)
        h += 360;
    while (h >= 360)
        h -= 360;
    hs = h % 120 - 60;
    if (hs < 0)
        hs = -hs;
    c2 = 2.0 * lv - 1.0;
    if (c2 < 0)
        c2 = -c2;
    c = (1.0 - c2) * sv;
    x = (60 - hs) / 60.0 * c;
    m = lv - 0.5 * c;
    switch (h / 60) {
      case 0: r1 = c; g1 = x; b1 = 0; break;
      case 1: r1 = x; g1 = c; b1 = 0; break;
      case 2: r1 = 0; g1 = c; b1 = x; break;
      case 3: r1 = 0; g1 = x; b1 = c; break;
      case 4: r1 = x; g1 = 0; b1 = c; break;
      default: r1 = c; g1 = 0; b1 = x; break;
    }
    *r = clamp_pct(r1 + m);
    *g = clamp_pct(g1 + m);
    *b = clamp_pct(b1 + m);
}

void sixel_init(SixelDecoder *d, const unsigned *params, int nparams,
                SixelPalette *shared, int max_side, uint32_t max_pixels)
{
    memset(d, 0, sizeof(*d));
    if (shared) {
        d->pal = shared;
    } else {
        sixel_palette_default(&d->own);
        d->pal = &d->own;
    }
    /* P1 (the aspect) and P3 (the grid) do not change square pixels */
    d->transparent = nparams >= 2 && params[1] == 1;
    d->max_side = max_side;
    d->max_pixels = max_pixels;
    d->reg = 3;
}

void sixel_free(SixelDecoder *d)
{
    free(d->px);
    d->px = NULL;
    d->alloc_w = d->alloc_h = 0;
}

static void go_over(SixelDecoder *d, bool nomem)
{
    sixel_free(d);
    d->over = true;
    d->nomem = nomem;
}

static int imax(int a, int b)
{
    return a > b ? a : b;
}

/* Room for a drawing up to need_w x need_h. False (and over) when that
 * passes a cap or memory runs out. */
static bool ensure(SixelDecoder *d, int need_w, int need_h)
{
    int keep_w, keep_h, nw, nh, y;
    unsigned char *n;

    if (need_w <= d->alloc_w && need_h <= d->alloc_h)
        return true;
    keep_w = imax(d->used_w, need_w);
    keep_h = imax(d->used_h, need_h);
    if (need_w > d->max_side || need_h > d->max_side ||
        (uint64_t)keep_w * (uint64_t)keep_h > d->max_pixels) {
        go_over(d, false);
        return false;
    }
    /* grow by doubling, at least to the declared size, within the caps */
    nw = need_w <= d->alloc_w ? d->alloc_w
       : imax(need_w, d->alloc_w * 2 < d->max_side ? d->alloc_w * 2
                                                   : d->max_side);
    nh = need_h <= d->alloc_h ? d->alloc_h
       : imax(need_h, d->alloc_h * 2 < d->max_side ? d->alloc_h * 2
                                                   : d->max_side);
    nw = imax(nw, imax(d->decl_w, 16));
    nh = imax(nh, imax(d->decl_h, 6));
    if (nw > d->max_side)
        nw = d->max_side;
    if (nh > d->max_side)
        nh = d->max_side;
    /* over the pixel count: the side that grows takes what is left */
    if ((uint64_t)nw * (uint64_t)nh > d->max_pixels) {
        if (nh > d->alloc_h && keep_w > 0)
            nh = imax(keep_h, (int)(d->max_pixels / (uint32_t)imax(nw, keep_w)));
        if ((uint64_t)nw * (uint64_t)nh > d->max_pixels && keep_h > 0)
            nw = imax(keep_w, (int)(d->max_pixels / (uint32_t)nh));
    }
    if ((uint64_t)nw * (uint64_t)nh > d->max_pixels) {
        nw = keep_w;
        nh = keep_h;
    }
    n = calloc((size_t)nw * (size_t)nh, 4);
    if (!n) {
        go_over(d, true);
        return false;
    }
    if (d->px)
        for (y = 0; y < d->used_h; y++)
            memcpy(n + (size_t)y * nw * 4, d->px + (size_t)y * d->alloc_w * 4,
                   (size_t)d->used_w * 4);
    free(d->px);
    d->px = n;
    d->alloc_w = nw;
    d->alloc_h = nh;
    return true;
}

/* One sixel (bits, bit 0 on top), n times, in the current colour. */
static void draw(SixelDecoder *d, int bits, long n)
{
    uint32_t rgb;
    int top, b, x0, x1, x;

    if (!bits) {
        long c = d->col + n;
        d->col = c > d->max_side ? d->max_side + 1 : (int)c;
        return;
    }
    for (top = 5; !(bits & (1 << top)); top--)
        ;
    if (d->col + n > d->max_side || d->row + top + 1 > d->max_side) {
        go_over(d, false);
        return;
    }
    x0 = d->col;
    x1 = d->col + (int)n;
    if (!ensure(d, x1, d->row + top + 1))
        return;
    rgb = d->pal->rgb[d->reg];
    for (b = 0; b <= top; b++) {
        unsigned char *p;
        if (!(bits & (1 << b)))
            continue;
        p = d->px + ((size_t)(d->row + b) * d->alloc_w + x0) * 4;
        for (x = x0; x < x1; x++, p += 4) {
            p[0] = (unsigned char)(rgb >> 16);
            p[1] = (unsigned char)(rgb >> 8);
            p[2] = (unsigned char)rgb;
            p[3] = 255;
        }
    }
    if (x1 > d->used_w)
        d->used_w = x1;
    if (d->row + top + 1 > d->used_h)
        d->used_h = d->row + top + 1;
    d->col = x1;
}

/* A '#' read: select the register, define it when all five are there. */
static void end_colour(SixelDecoder *d)
{
    const long *p = d->params;
    int r, g, b;

    if (p[0] < 0)
        return;                         /* no register: ignored */
    d->reg = (int)(p[0] % SIXEL_REGISTERS);
    if (d->np != 5 || p[1] < 0 || p[2] < 0 || p[3] < 0 || p[4] < 0)
        return;
    if (p[1] == 1) {
        if (p[2] > 360 || p[3] > 100 || p[4] > 100)
            return;
        sixel_hls_to_rgb((int)p[2], (int)p[3], (int)p[4], &r, &g, &b);
    } else if (p[1] == 2) {
        if (p[2] > 100 || p[3] > 100 || p[4] > 100)
            return;
        r = (int)p[2];
        g = (int)p[3];
        b = (int)p[4];
    } else {
        return;
    }
    d->pal->rgb[d->reg] = pct_rgb(r, g, b);
}

/* A '"' read: Pan;Pad (the aspect, ignored); Ph;Pv the size. */
static void end_raster(SixelDecoder *d)
{
    long ph = d->np >= 3 && d->params[2] > 0 ? d->params[2] : 0;
    long pv = d->np >= 4 && d->params[3] > 0 ? d->params[3] : 0;
    if (ph > d->max_side || pv > d->max_side ||
        (uint64_t)ph * (uint64_t)pv > d->max_pixels) {
        go_over(d, false);
        return;
    }
    if (ph > d->decl_w)
        d->decl_w = (int)ph;
    if (pv > d->decl_h)
        d->decl_h = (int)pv;
}

static void end_command(SixelDecoder *d)
{
    char cmd = d->cmd;
    d->cmd = 0;
    switch (cmd) {
      case '!':
        d->repeat = d->params[0] > 0 ? d->params[0] : 1;
        break;
      case '#':
        end_colour(d);
        break;
      case '"':
        end_raster(d);
        break;
    }
}

static void start_command(SixelDecoder *d, char cmd)
{
    d->cmd = cmd;
    d->np = 1;
    d->params[0] = -1;
}

static void feed_byte(SixelDecoder *d, unsigned char c)
{
    if (c >= '0' && c <= '9') {
        if (d->cmd && d->np <= 5) {
            long *p = &d->params[d->np - 1];
            if (*p < 0)
                *p = 0;
            *p = *p * 10 + (c - '0');
            if (*p > SIXEL_PARAM_MAX)
                *p = SIXEL_PARAM_MAX;
        }
        return;                         /* a stray digit: ignored */
    }
    if (c == ';') {
        if (d->cmd) {
            if (d->np < 5)
                d->params[d->np] = -1;
            if (d->np <= 5)
                d->np++;                /* 6: too many, a define is ignored */
        }
        return;
    }
    if (c <= ' ' || c >= 0x7F)
        return;                         /* white space, controls: skipped */
    if (d->cmd)
        end_command(d);
    if (d->over)
        return;
    if (c >= 0x3F && c <= 0x7E) {
        long n = d->repeat ? d->repeat : 1;
        d->repeat = 0;
        d->seen_data = true;
        draw(d, c - 0x3F, n);
        return;
    }
    d->repeat = 0;                      /* a repeat not followed by a sixel */
    switch (c) {
      case '$':                         /* DECGCR: back to the left edge */
        d->col = 0;
        break;
      case '-':                         /* DECGNL: the next band */
        d->col = 0;
        if (d->row <= d->max_side)
            d->row += 6;
        break;
      case '!':                         /* DECGRI: repeat */
      case '#':                         /* DECGCI: colour */
      case '"':                         /* DECGRA: raster attributes */
        start_command(d, (char)c);
        break;
      default:                          /* unknown: ignored */
        break;
    }
}

void sixel_feed(SixelDecoder *d, const unsigned char *s, size_t len)
{
    size_t i;
    for (i = 0; i < len && !d->over; i++)
        feed_byte(d, s[i]);
}

bool sixel_finish(SixelDecoder *d, unsigned char **px, int *w, int *h,
                  const char **why)
{
    unsigned char *out, *o;
    uint32_t bg = d->pal->rgb[0];
    int W, H, x, y;

    *px = NULL;
    *w = *h = 0;
    *why = "";
    if (d->cmd && !d->over)
        end_command(d);                 /* "...#1;2;0;0;0" then ST */
    if (d->over) {
        *why = d->nomem ? "ENOMEM" : "EFBIG";
        sixel_free(d);
        return false;
    }
    W = imax(d->used_w, d->decl_w);
    H = imax(d->used_h, d->decl_h);
    if (!d->seen_data || W <= 0 || H <= 0) {
        sixel_free(d);
        return false;
    }
    if (W > d->max_side || H > d->max_side ||
        (uint64_t)W * (uint64_t)H > d->max_pixels) {
        *why = "EFBIG";
        sixel_free(d);
        return false;
    }
    out = malloc((size_t)W * (size_t)H * 4);
    if (!out) {
        *why = "ENOMEM";
        sixel_free(d);
        return false;
    }
    for (y = 0, o = out; y < H; y++) {
        const unsigned char *p = y < d->alloc_h ?
            d->px + (size_t)y * d->alloc_w * 4 : NULL;
        for (x = 0; x < W; x++, o += 4) {
            if (p && x < d->alloc_w && p[x * 4 + 3]) {
                o[0] = p[x * 4 + 2];
                o[1] = p[x * 4 + 1];
                o[2] = p[x * 4];
                o[3] = 255;
            } else if (d->transparent) {
                o[0] = o[1] = o[2] = o[3] = 0;
            } else {
                o[0] = (unsigned char)bg;
                o[1] = (unsigned char)(bg >> 8);
                o[2] = (unsigned char)(bg >> 16);
                o[3] = 255;
            }
        }
    }
    sixel_free(d);
    *px = out;
    *w = W;
    *h = H;
    return true;
}
