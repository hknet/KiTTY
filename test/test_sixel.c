/*
 * test_sixel.c - the Sixel decoder (kitty/kitty_sixel.c): the VT340
 * palette, HLS and RGB colours, repeats, bands, '$', raster attributes, P2
 * transparency, private and shared registers, the caps, and garbage.
 * Builds on its own:
 *   gcc -std=c99 -Wall -Wextra -Wpedantic -fsanitize=address,undefined \
 *       -fno-sanitize-recover=all -o t test/test_sixel.c kitty/kitty_sixel.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../kitty/kitty_sixel.h"

static int failures, checks;

#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
    printf("\n"); } } while (0)

/* ---- helpers ------------------------------------------------------------ */

typedef struct Pic {
    bool ok;
    unsigned char *px;
    int w, h;
    const char *why;
} Pic;

/* Decodes data with P2 = p2, the caps given (0: the defaults). */
static Pic decode_ex(const char *data, unsigned p2, SixelPalette *shared,
                     int max_side, unsigned max_pixels)
{
    SixelDecoder d;
    unsigned params[3] = { 0, 0, 0 };
    Pic p;
    params[1] = p2;
    sixel_init(&d, params, 3, shared, max_side ? max_side : 8192,
               max_pixels ? max_pixels : 16u * 1024 * 1024);
    sixel_feed(&d, (const unsigned char *)data, strlen(data));
    p.ok = sixel_finish(&d, &p.px, &p.w, &p.h, &p.why);
    return p;
}

static Pic decode(const char *data, unsigned p2)
{
    return decode_ex(data, p2, NULL, 0, 0);
}

/* The pixel at x, y as 0xAARRGGBB (from premultiplied BGRA). */
static unsigned long pix(const Pic *p, int x, int y)
{
    const unsigned char *q = p->px + ((size_t)y * p->w + x) * 4;
    return ((unsigned long)q[3] << 24) | ((unsigned long)q[2] << 16) |
           ((unsigned long)q[1] << 8) | q[0];
}

static void pic_free(Pic *p)
{
    free(p->px);
    p->px = NULL;
}

/* ---- tests -------------------------------------------------------------- */

static void test_palette(void)
{
    SixelPalette p;
    int i, bad = 0;
    sixel_palette_default(&p);
    CHECK(p.rgb[0] == 0x000000, "register 0 black: %06lx",
          (unsigned long)p.rgb[0]);
    CHECK(p.rgb[1] == 0x3333CC, "register 1 (20,20,80)%%: %06lx",
          (unsigned long)p.rgb[1]);
    CHECK(p.rgb[2] == 0xCC2121, "register 2 (80,13,13)%%: %06lx",
          (unsigned long)p.rgb[2]);
    CHECK(p.rgb[3] == 0x33CC33, "register 3 (20,80,20)%%: %06lx",
          (unsigned long)p.rgb[3]);
    CHECK(p.rgb[7] == 0x878787, "register 7 (53,53,53)%%: %06lx",
          (unsigned long)p.rgb[7]);
    CHECK(p.rgb[15] == 0xCCCCCC, "register 15 (80,80,80)%%: %06lx",
          (unsigned long)p.rgb[15]);
    for (i = 16; i < SIXEL_REGISTERS; i++)
        if (p.rgb[i])
            bad++;
    CHECK(!bad, "registers 16-255 start black");
}

static void test_hls(void)
{
    int r, g, b;
    /* blue at 0 degrees, red at 120, green at 240 (DEC's hue circle) */
    sixel_hls_to_rgb(0, 50, 100, &r, &g, &b);
    CHECK(r == 0 && g == 0 && b == 100, "HLS 0: blue, got %d,%d,%d", r, g, b);
    sixel_hls_to_rgb(120, 50, 100, &r, &g, &b);
    CHECK(r == 100 && g == 0 && b == 0, "HLS 120: red, got %d,%d,%d", r, g, b);
    sixel_hls_to_rgb(240, 50, 100, &r, &g, &b);
    CHECK(r == 0 && g == 100 && b == 0, "HLS 240: green, got %d,%d,%d",
          r, g, b);
    sixel_hls_to_rgb(180, 50, 100, &r, &g, &b);
    CHECK(r == 100 && g == 100 && b == 0, "HLS 180: yellow, got %d,%d,%d",
          r, g, b);
    sixel_hls_to_rgb(60, 50, 100, &r, &g, &b);
    CHECK(r == 100 && g == 0 && b == 100, "HLS 60: magenta, got %d,%d,%d",
          r, g, b);
    sixel_hls_to_rgb(300, 50, 100, &r, &g, &b);
    CHECK(r == 0 && g == 100 && b == 100, "HLS 300: cyan, got %d,%d,%d",
          r, g, b);
    sixel_hls_to_rgb(77, 40, 0, &r, &g, &b);
    CHECK(r == 40 && g == 40 && b == 40, "HLS s=0: grey, got %d,%d,%d",
          r, g, b);
    sixel_hls_to_rgb(0, 100, 100, &r, &g, &b);
    CHECK(r == 100 && g == 100 && b == 100, "HLS l=100: white, got %d,%d,%d",
          r, g, b);
    sixel_hls_to_rgb(120, 25, 100, &r, &g, &b);
    CHECK(r == 50 && g == 0 && b == 0, "HLS 120/25/100: dark red, got %d,%d,%d",
          r, g, b);

    /* through the decoder: #1;1;120;50;100 is red */
    {
        Pic p = decode("#1;1;120;50;100#1~", 1);
        CHECK(p.ok && pix(&p, 0, 0) == 0xFFFF0000UL, "HLS define: %08lx",
              p.ok ? pix(&p, 0, 0) : 0);
        pic_free(&p);
    }
}

static void test_rgb_and_default_register(void)
{
    Pic p = decode("#1;2;100;0;0#1~", 1);
    int y, bad = 0;
    CHECK(p.ok && p.w == 1 && p.h == 6, "one red sixel: 1 x 6, got %d x %d",
          p.w, p.h);
    for (y = 0; p.ok && y < 6; y++)
        if (pix(&p, 0, y) != 0xFFFF0000UL)
            bad++;
    CHECK(!bad, "all six pixels red");
    pic_free(&p);

    /* percent to 8 bits, rounded */
    p = decode("#7;2;50;20;1#7~", 1);
    CHECK(p.ok && pix(&p, 0, 0) == 0xFF803303UL, "RGB 50/20/1: %08lx",
          p.ok ? pix(&p, 0, 0) : 0);
    pic_free(&p);

    /* no register selected: 3, the VT340's green, as in xterm */
    p = decode("~", 1);
    CHECK(p.ok && pix(&p, 0, 0) == 0xFF33CC33UL, "default register 3: %08lx",
          p.ok ? pix(&p, 0, 0) : 0);
    pic_free(&p);

    /* out of range: the define is ignored, the register selected */
    p = decode("#1;2;101;0;0~", 1);
    CHECK(p.ok && pix(&p, 0, 0) == 0xFF3333CCUL, "RGB 101 ignored: %08lx",
          p.ok ? pix(&p, 0, 0) : 0);
    pic_free(&p);
    p = decode("#1;1;361;50;100~", 1);
    CHECK(p.ok && pix(&p, 0, 0) == 0xFF3333CCUL, "HLS 361 ignored: %08lx",
          p.ok ? pix(&p, 0, 0) : 0);
    pic_free(&p);
    /* an unknown colour space, four parameters, six: select only */
    p = decode("#1;3;10;10;10~#2;2;0;0~#1;2;0;0;0;5~", 1);
    CHECK(p.ok && pix(&p, 0, 0) == 0xFF3333CCUL &&
          pix(&p, 1, 0) == 0xFFCC2121UL && pix(&p, 2, 0) == 0xFF3333CCUL,
          "malformed defines select only");
    pic_free(&p);

    /* register numbers wrap at 256 */
    p = decode("#257;2;0;100;0#1~", 1);
    CHECK(p.ok && pix(&p, 0, 0) == 0xFF00FF00UL, "register 257 is 1: %08lx",
          p.ok ? pix(&p, 0, 0) : 0);
    pic_free(&p);

    /* a pixel keeps the colour it was drawn in */
    p = decode("#1;2;100;0;0#1~#1;2;0;0;100~", 1);
    CHECK(p.ok && pix(&p, 0, 0) == 0xFFFF0000UL &&
          pix(&p, 1, 0) == 0xFF0000FFUL, "redefinition recolours nothing");
    pic_free(&p);
}

static void test_bits_and_bands(void)
{
    Pic p;
    /* '@' is bit 0 only: one row; 'A' bit 1: two rows, the top unset */
    p = decode("#1;2;100;0;0#1@", 1);
    CHECK(p.ok && p.w == 1 && p.h == 1, "'@': 1 x 1, got %d x %d", p.w, p.h);
    pic_free(&p);
    p = decode("#1;2;100;0;0#1A", 1);
    CHECK(p.ok && p.h == 2 && pix(&p, 0, 0) == 0 &&
          pix(&p, 0, 1) == 0xFFFF0000UL, "'A': row 1 only");
    pic_free(&p);
    /* '_' is bit 5: the band's last row */
    p = decode("_", 1);
    CHECK(p.ok && p.h == 6 && pix(&p, 0, 4) == 0 && pix(&p, 0, 5) != 0,
          "'_': row 5");
    pic_free(&p);

    /* '-' the next band; trailing '?' and '-' do not grow the picture */
    p = decode("~-~?" "?" "?-", 1);
    CHECK(p.ok && p.w == 1 && p.h == 12, "two bands: 1 x 12, got %d x %d",
          p.w, p.h);
    pic_free(&p);
    p = decode("~-@", 1);
    CHECK(p.ok && p.h == 7, "second band one row: 7 high, got %d", p.h);
    pic_free(&p);

    /* '$' back to the left: a second colour over the same band */
    p = decode("#1;2;100;0;0#2;2;0;100;0#1~~$#2?~", 1);
    CHECK(p.ok && p.w == 2 && pix(&p, 0, 0) == 0xFFFF0000UL &&
          pix(&p, 1, 0) == 0xFF00FF00UL, "'$': overprint");
    pic_free(&p);
    /* '?' skips a column without drawing */
    p = decode("?~", 1);
    CHECK(p.ok && p.w == 2 && pix(&p, 0, 0) == 0 && pix(&p, 1, 0) != 0,
          "'?' skips");
    pic_free(&p);
    /* the image starts at the left even after a '-' */
    p = decode("~~~-~", 1);
    CHECK(p.ok && p.w == 3 && pix(&p, 0, 6) != 0 && pix(&p, 1, 6) == 0,
          "'-' to the left edge");
    pic_free(&p);
}

static void test_repeat(void)
{
    Pic p;
    p = decode("!5~", 1);
    CHECK(p.ok && p.w == 5 && p.h == 6, "!5~: 5 wide, got %d", p.w);
    pic_free(&p);
    p = decode("!~", 1);
    CHECK(p.ok && p.w == 1, "!~: 1 wide, got %d", p.w);
    pic_free(&p);
    p = decode("!0~", 1);
    CHECK(p.ok && p.w == 1, "!0~: 1 wide, got %d", p.w);
    pic_free(&p);
    p = decode("!3?~", 1);
    CHECK(p.ok && p.w == 4 && pix(&p, 2, 0) == 0 && pix(&p, 3, 0) != 0,
          "!3?: a skip of three");
    pic_free(&p);
    /* white space and line breaks inside are skipped */
    p = decode("!1\r\n0 ~", 1);
    CHECK(p.ok && p.w == 10, "!1<CRLF>0 ~: 10 wide, got %d", p.w);
    pic_free(&p);
    /* a repeat not followed by a sixel is dropped */
    p = decode("!5$~", 1);
    CHECK(p.ok && p.w == 1, "!5$~: 1 wide, got %d", p.w);
    pic_free(&p);
}

static void test_raster_and_background(void)
{
    Pic p;
    /* "Pan;Pad;Ph;Pv: the size, even where nothing is drawn */
    p = decode("\"1;1;10;20#1;2;100;0;0#1@", 0);
    CHECK(p.ok && p.w == 10 && p.h == 20, "raster 10 x 20, got %d x %d",
          p.w, p.h);
    /* P2 = 0: what is not drawn is register 0 (black), opaque */
    CHECK(p.ok && pix(&p, 0, 0) == 0xFFFF0000UL &&
          pix(&p, 9, 19) == 0xFF000000UL && pix(&p, 0, 1) == 0xFF000000UL,
          "P2=0: background register 0");
    pic_free(&p);
    /* register 0 redefined: its colour fills */
    p = decode("#0;2;0;0;100\"1;1;2;2#1@", 2);
    CHECK(p.ok && pix(&p, 1, 1) == 0xFF0000FFUL, "P2=2: background %08lx",
          p.ok ? pix(&p, 1, 1) : 0);
    pic_free(&p);
    /* P2 = 1: transparent */
    p = decode("\"1;1;4;4@", 1);
    CHECK(p.ok && p.w == 4 && p.h == 4 && pix(&p, 3, 3) == 0 &&
          pix(&p, 0, 0) != 0, "P2=1: transparent");
    pic_free(&p);
    /* drawing past the declared size grows the picture */
    p = decode("\"1;1;2;2!4~", 1);
    CHECK(p.ok && p.w == 4 && p.h == 6, "past Ph/Pv: 4 x 6, got %d x %d",
          p.w, p.h);
    pic_free(&p);
    /* Pan;Pad alone: no size */
    p = decode("\"2;1@", 1);
    CHECK(p.ok && p.w == 1 && p.h == 1, "aspect only, got %d x %d", p.w, p.h);
    pic_free(&p);
    /* raster attributes without any sixel: nothing to place */
    p = decode("\"1;1;10;10", 0);
    CHECK(!p.ok && !strcmp(p.why, ""), "raster only: nothing placed");
    p = decode("", 0);
    CHECK(!p.ok && !strcmp(p.why, ""), "empty: nothing placed");
    p = decode("#1;2;0;0;0$-", 0);
    CHECK(!p.ok && !strcmp(p.why, ""), "no sixel: nothing placed");
    /* '?' only, with raster: a background rectangle (VT340) */
    p = decode("\"1;1;3;3?", 0);
    CHECK(p.ok && p.w == 3 && p.h == 3 && pix(&p, 2, 2) == 0xFF000000UL,
          "'?' with raster: filled");
    pic_free(&p);
}

static void test_registers(void)
{
    SixelPalette shared;
    Pic p;
    sixel_palette_default(&shared);
    /* private (the default): a define does not outlive its picture */
    p = decode("#5;2;100;0;0#5~", 1);
    pic_free(&p);
    p = decode("#5~", 1);
    CHECK(p.ok && pix(&p, 0, 0) == 0xFF33CCCCUL, "private: 5 back to cyan %08lx",
          p.ok ? pix(&p, 0, 0) : 0);
    pic_free(&p);
    /* shared: it does */
    p = decode_ex("#5;2;100;0;0#5~", 1, &shared, 0, 0);
    pic_free(&p);
    CHECK(shared.rgb[5] == 0xFF0000, "shared: written back");
    p = decode_ex("#5~", 1, &shared, 0, 0);
    CHECK(p.ok && pix(&p, 0, 0) == 0xFFFF0000UL, "shared: 5 stays red");
    pic_free(&p);
}

static void test_caps(void)
{
    Pic p;
    /* a side */
    p = decode_ex("!17~", 1, NULL, 16, 0);
    CHECK(!p.ok && !strcmp(p.why, "EFBIG"), "17 wide over 16: EFBIG");
    p = decode_ex("!16~", 1, NULL, 16, 0);
    CHECK(p.ok && p.w == 16, "16 wide at 16: placed");
    pic_free(&p);
    p = decode_ex("~-~-~", 1, NULL, 16, 0);
    CHECK(!p.ok && !strcmp(p.why, "EFBIG"), "18 high over 16: EFBIG");
    p = decode_ex("~-~-@", 1, NULL, 16, 0);
    CHECK(p.ok && p.h == 13, "13 high at 16: placed");
    pic_free(&p);
    /* the rest of the sequence after the cap is swallowed */
    p = decode_ex("!17~$-~~~~", 1, NULL, 16, 0);
    CHECK(!p.ok && !strcmp(p.why, "EFBIG"), "swallowed after the cap");
    /* the pixel count */
    p = decode_ex("!11~-!11~", 1, NULL, 0, 100);
    CHECK(!p.ok && !strcmp(p.why, "EFBIG"), "11 x 12 over 100 px: EFBIG");
    p = decode_ex("!10~-!10~-!10@", 1, NULL, 0, 130);
    CHECK(p.ok && p.w == 10 && p.h == 13, "10 x 13 at 130 px: placed");
    pic_free(&p);
    /* raster attributes over a cap */
    p = decode_ex("\"1;1;17;1@", 1, NULL, 16, 0);
    CHECK(!p.ok && !strcmp(p.why, "EFBIG"), "Ph over the side: EFBIG");
    p = decode_ex("\"1;1;11;11@", 1, NULL, 0, 100);
    CHECK(!p.ok && !strcmp(p.why, "EFBIG"), "Ph x Pv over the count: EFBIG");
    /* declared width and drawn height together over the count */
    p = decode_ex("\"1;1;10;1~-~", 1, NULL, 0, 100);
    CHECK(!p.ok && !strcmp(p.why, "EFBIG"), "10 x 12 over 100 at the end");
    /* absurd numbers do not overflow */
    p = decode("!99999999999999999999~", 1);
    CHECK(!p.ok && !strcmp(p.why, "EFBIG"), "huge repeat: EFBIG");
    p = decode("!99999999999999999999?~", 1);
    CHECK(!p.ok && !strcmp(p.why, "EFBIG"), "huge skip, then a sixel: EFBIG");
    p = decode("!99999999999999999999?", 1);
    CHECK(!p.ok && !strcmp(p.why, ""), "huge skip alone: nothing drawn");
    p = decode("#99999999999999999999;2;100;0;0~", 1);
    CHECK(p.ok, "huge register number");
    pic_free(&p);
    /* a large picture grown band by band, every pixel kept */
    {
        char *s = malloc(200 * 12 + 1), *q = s;
        int b, x, bad = 0;
        for (b = 0; b < 200; b++) {
            q += sprintf(q, "#%d!300~-", b % 2 + 1);
        }
        *q = '\0';
        p = decode(s, 1);
        CHECK(p.ok && p.w == 300 && p.h == 1200, "300 x 1200, got %d x %d",
              p.w, p.h);
        for (b = 0; p.ok && b < 200; b++)
            for (x = 0; x < 300; x += 37)
                if (pix(&p, x, b * 6 + 3) !=
                    (b % 2 ? 0xFFCC2121UL : 0xFF3333CCUL))
                    bad++;
        CHECK(!bad, "bands kept their colours (%d bad)", bad);
        pic_free(&p);
        free(s);
    }
}

static void test_streaming_and_garbage(void)
{
    static const char data[] =
        "\"1;1;7;9#1;2;100;0;0#2;1;240;50;100#1!3~$#2?" "?!2@-#1A";
    SixelDecoder d;
    unsigned params[3] = { 0, 1, 0 };
    Pic whole, bytes;
    size_t i;
    unsigned seed = 12345;
    int k;

    whole = decode(data, 1);
    sixel_init(&d, params, 3, NULL, 8192, 16u * 1024 * 1024);
    for (i = 0; data[i]; i++)
        sixel_feed(&d, (const unsigned char *)data + i, 1);
    bytes.ok = sixel_finish(&d, &bytes.px, &bytes.w, &bytes.h, &bytes.why);
    CHECK(whole.ok && bytes.ok && whole.w == bytes.w && whole.h == bytes.h &&
          !memcmp(whole.px, bytes.px, (size_t)whole.w * whole.h * 4),
          "byte by byte equals all at once");
    CHECK(whole.ok && whole.w == 7 && whole.h == 9, "7 x 9, got %d x %d",
          whole.w, whole.h);
    pic_free(&whole);
    pic_free(&bytes);

    /* a command cut by the end of the sequence is still served */
    {
        Pic p = decode("~#1;2;0;0;100", 1);
        CHECK(p.ok && p.w == 1, "trailing define: placed");
        pic_free(&p);
    }

    /* random bytes: no crash, a sane answer */
    for (k = 0; k < 300; k++) {
        unsigned char buf[600];
        int n = 0;
        while (n < (int)sizeof(buf) - 1) {
            seed = seed * 1103515245u + 12345u;
            buf[n++] = (unsigned char)(seed >> 16);
        }
        buf[n] = 0;
        sixel_init(&d, params, 3, NULL, 512, 65536);
        sixel_feed(&d, buf, (size_t)n);
        whole.ok = sixel_finish(&d, &whole.px, &whole.w, &whole.h,
                                &whole.why);
        if (whole.ok)
            CHECK(whole.w > 0 && whole.h > 0 && whole.w <= 512 &&
                  whole.h <= 512, "garbage: sane size");
        pic_free(&whole);
    }
    /* and an abandoned decoder frees what it holds */
    sixel_init(&d, params, 3, NULL, 8192, 16u * 1024 * 1024);
    sixel_feed(&d, (const unsigned char *)"!100~-!100~", 11);
    sixel_free(&d);
    CHECK(d.px == NULL, "sixel_free");
}

int main(void)
{
    test_palette();
    test_hls();
    test_rgb_and_default_register();
    test_bits_and_bands();
    test_repeat();
    test_raster_and_background();
    test_registers();
    test_caps();
    test_streaming_and_garbage();
    if (failures) {
        printf("test_sixel: %d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("test_sixel: all %d checks passed\n", checks);
    return 0;
}
