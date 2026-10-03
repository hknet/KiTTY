/*
 * test_far2l_images.c - far2l images (kitty/kitty_far2l_image.c): the
 * request stacks as far2l builds them, the replies, the geometry of a placed
 * image, rotation and mirroring, the store's cap, and raw RGB/RGBA to
 * premultiplied BGRA. The byte strings marked "spec" are the worked examples
 * of the far2l extensions description (VTExts.md 7.6), generated there with
 * far2l's own serializer.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../kitty/kitty_far2l_image.h"

static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
    printf("\n"); } } while (0)

/* ---- building request stacks: push appends, the top is the end --------- */

typedef struct {
    unsigned char *b;
    size_t n, size;
} Stk;

static void push(Stk *s, const void *p, size_t len)
{
    if (s->n + len > s->size) {
        s->size = (s->n + len) * 2 + 64;
        s->b = realloc(s->b, s->size);
    }
    memcpy(s->b + s->n, p, len);
    s->n += len;
}

static void push_num(Stk *s, unsigned long long v, int bytes)
{
    unsigned char t[8];
    int i;
    for (i = 0; i < bytes; i++)
        t[i] = (unsigned char)(v >> (8 * i));
    push(s, t, bytes);
}

static void push_str(Stk *s, const char *str)
{
    push(s, str, strlen(str));
    push_num(s, strlen(str), 4);
}

/* An IMAGE_SET stack, the arguments pushed last-popped first. */
static void build_set(Stk *s, const char *id, unsigned long long flags,
                      int l, int t, int r, int b, unsigned w, unsigned h,
                      const void *data, size_t datalen)
{
    s->n = 0;
    push(s, data, datalen);
    push_num(s, h, 4);
    push_num(s, w, 4);
    push_num(s, (unsigned short)b, 2);
    push_num(s, (unsigned short)r, 2);
    push_num(s, (unsigned short)t, 2);
    push_num(s, (unsigned short)l, 2);
    push_num(s, flags, 8);
    push_str(s, id);
    push_num(s, 's', 1);
}

static void build_transform(Stk *s, const char *id, int l, int t, int r,
                            int b, unsigned tf)
{
    s->n = 0;
    push_num(s, tf, 2);
    push_num(s, (unsigned short)b, 2);
    push_num(s, (unsigned short)r, 2);
    push_num(s, (unsigned short)t, 2);
    push_num(s, (unsigned short)l, 2);
    push_str(s, id);
    push_num(s, 't', 1);
}

static void build_delete(Stk *s, const char *id)
{
    s->n = 0;
    push_str(s, id);
    push_num(s, 'd', 1);
}

/* ---- the environment ---------------------------------------------------- */

static int nchanged;
static Far2lRect last_before, last_after;

static void on_changed(void *ctx, const Far2lRect *before, const Far2lRect *after)
{
    (void)ctx;
    nchanged++;
    last_before = *before;
    last_after = *after;
}

/* A stand-in decoder: any data decodes to a 3x2 half-transparent picture. */
static bool fake_decode(void *ctx, unsigned fmt, const unsigned char *data, size_t len,
                        unsigned char **px, int *w, int *h)
{
    (void)ctx; (void)data; (void)fmt;
    if (len < 4)
        return false;
    *w = 3;
    *h = 2;
    *px = malloc(3 * 2 * 4);
    memset(*px, 0x40, 3 * 2 * 4);
    return true;
}

static Far2lImageEnv env_default(void)
{
    Far2lImageEnv e;
    memset(&e, 0, sizeof(e));
    e.enabled = true;
    e.cell_w = 8;
    e.cell_h = 16;
    e.changed = on_changed;
    return e;
}

static int req(Far2lImageStore *st, const Far2lImageEnv *env, const Stk *s,
               unsigned char *out)
{
    return far2l_img_request(st, env, s->b, s->n, out);
}

static const Far2lImage *find(const Far2lImageStore *st, const char *id)
{
    int i;
    for (i = 0; i < st->n; i++)
        if (st->imgs[i].idlen == strlen(id) &&
            !memcmp(st->imgs[i].id, id, strlen(id)))
            return &st->imgs[i];
    return NULL;
}

/* ---- tests -------------------------------------------------------------- */

static void test_caps(void)
{
    Far2lImageStore st = { 0 };
    Far2lImageEnv e = env_default();
    unsigned char out[F2L_IMG_REPLY_MAX];
    /* spec: request "Y2kF" = 63 69 05, i.e. 'c' under 'i' under ID 5 */
    static const unsigned char caps_req[] = { 0x63 };
    static const unsigned char want_rgba[] = {
        0x01, 0x08, 0, 0, 0, 0, 0, 0, 0x08, 0x00, 0x10, 0x00 };
    static const unsigned char want_png[] = {
        0x03, 0x08, 0, 0, 0, 0, 0, 0, 0x08, 0x00, 0x10, 0x00 };
    int n;

    n = far2l_img_request(&st, &e, caps_req, 1, out);
    CHECK(n == 13, "caps reply length %d, want 13", n);
    CHECK(!memcmp(out, want_rgba, 12), "caps without a codec: RGBA + ROTMIR, 8x16");

    e.codecs = F2L_IMGCAP_PNG;
    n = far2l_img_request(&st, &e, caps_req, 1, out);
    CHECK(n == 13 && !memcmp(out, want_png, 12),
          "caps with a codec: 0x803 (JPG = RGBA|PNG), 8x16");

    e.enabled = false;
    n = far2l_img_request(&st, &e, caps_req, 1, out);
    CHECK(n == 13 && out[0] == 0 && out[1] == 0 && out[8] == 8 && out[10] == 16,
          "images off: caps 0, the cell size still given");

    e = env_default();
    e.cell_w = 0;
    n = far2l_img_request(&st, &e, caps_req, 1, out);
    CHECK(n == 13 && out[0] == 0, "no cell size known: caps 0");
}

static void test_spec_set_and_transform(void)
{
    /* spec 7.6: one red pixel "img" at column 2, row 3, own size; the stack
     * without its ID (06) and command letter (69) */
    static const unsigned char set_req[] = {
        0xff, 0x00, 0x00, 0xff, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
        0xff, 0xff, 0xff, 0xff, 0x03, 0x00, 0x02, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x69, 0x6d, 0x67, 0x03, 0x00, 0x00, 0x00, 0x73 };
    /* spec 7.6: rotate 90 + mirror H (0x05), all four coordinates -1 */
    static const unsigned char tf_req[] = {
        0x05, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0x69, 0x6d, 0x67, 0x03, 0x00, 0x00, 0x00, 0x74 };
    Far2lImageStore st = { 0 };
    Far2lImageEnv e = env_default();
    unsigned char out[F2L_IMG_REPLY_MAX];
    const Far2lImage *img;
    Far2lRect r;
    int n;

    nchanged = 0;
    n = far2l_img_request(&st, &e, set_req, sizeof(set_req), out);
    CHECK(n == 2 && out[0] == 1, "spec set: reply 1 (got len %d, %d)", n, out[0]);
    img = find(&st, "img");
    CHECK(img != NULL, "spec set: image stored");
    if (!img)
        return;
    CHECK(img->left == 2 && img->top == 3 && img->right == -1 &&
          img->bottom == -1 && img->w == 1 && img->h == 1,
          "spec set: placement and size");
    CHECK(img->px[0] == 0 && img->px[1] == 0 && img->px[2] == 0xff &&
          img->px[3] == 0xff && img->opaque, "spec set: red as BGRA, opaque");
    CHECK(far2l_img_rect(img, 8, 16, &r) && r.x0 == 16 && r.y0 == 48 &&
          r.x1 == 17 && r.y1 == 49, "spec set: drawn at its own size at cell 2,3");
    CHECK(nchanged == 1 && last_before.x1 == 0 && last_after.x0 == 16,
          "spec set: the terminal is told where it appeared");
    CHECK(st.bytes == 4, "spec set: 4 bytes held");

    n = far2l_img_request(&st, &e, tf_req, sizeof(tf_req), out);
    CHECK(n == 2 && out[0] == 1, "spec transform: reply 1");
    img = find(&st, "img");
    CHECK(img && img->left == 2 && img->top == 3, "spec transform: -1 keeps the place");

    far2l_img_store_clear(&st, NULL);
    CHECK(st.n == 0 && st.bytes == 0, "clear: empty");
}

/* Pixel (x, y) of a test picture carries x and y in its colour bytes. */
static unsigned char *grid(int w, int h)
{
    unsigned char *px = malloc((size_t)w * h * 4);
    int x, y;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            unsigned char *p = px + ((size_t)y * w + x) * 4;
            p[0] = (unsigned char)x; p[1] = (unsigned char)y; p[2] = 0x77; p[3] = 0xff;
        }
    return px;
}

/* Where did source pixel (sx, sy) of a w x h grid end up? */
static int at(const unsigned char *px, int w, int x, int y, int sx, int sy)
{
    const unsigned char *p = px + ((size_t)y * w + x) * 4;
    return p[0] == sx && p[1] == sy;
}

static void test_transforms(void)
{
    unsigned char *px;
    int w, h;

    /* 3 x 2:  (0,0) (1,0) (2,0)
     *         (0,1) (1,1) (2,1) */
    px = grid(3, 2); w = 3; h = 2;
    CHECK(far2l_img_transform_px(&px, &w, &h, F2L_IMGTF_ROTATE90), "rot90 ok");
    /* clockwise: the bottom-left source pixel goes to the top-left */
    CHECK(w == 2 && h == 3, "rot90 swaps the sides");
    CHECK(at(px, w, 0, 0, 0, 1) && at(px, w, 1, 0, 0, 0) &&
          at(px, w, 0, 2, 2, 1) && at(px, w, 1, 2, 2, 0), "rot90 clockwise");
    free(px);

    px = grid(3, 2); w = 3; h = 2;
    far2l_img_transform_px(&px, &w, &h, F2L_IMGTF_ROTATE270);
    CHECK(w == 2 && h == 3 && at(px, w, 0, 0, 2, 0) && at(px, w, 1, 0, 2, 1) &&
          at(px, w, 0, 2, 0, 0), "rot270 = anticlockwise");
    free(px);

    px = grid(3, 2); w = 3; h = 2;
    far2l_img_transform_px(&px, &w, &h, F2L_IMGTF_ROTATE180);
    CHECK(w == 3 && h == 2 && at(px, w, 0, 0, 2, 1) && at(px, w, 2, 1, 0, 0),
          "rot180");
    free(px);

    px = grid(3, 2); w = 3; h = 2;
    far2l_img_transform_px(&px, &w, &h, F2L_IMGTF_MIRROR_H);
    CHECK(at(px, w, 0, 0, 2, 0) && at(px, w, 2, 1, 0, 1), "mirror H: left-right");
    free(px);

    px = grid(3, 2); w = 3; h = 2;
    far2l_img_transform_px(&px, &w, &h, F2L_IMGTF_MIRROR_V);
    CHECK(at(px, w, 0, 0, 0, 1) && at(px, w, 2, 1, 2, 0), "mirror V: top-bottom");
    free(px);

    /* mirroring comes first: mirror H then rotate 90 clockwise */
    px = grid(3, 2); w = 3; h = 2;
    far2l_img_transform_px(&px, &w, &h, F2L_IMGTF_ROTATE90 | F2L_IMGTF_MIRROR_H);
    CHECK(w == 2 && h == 3 && at(px, w, 0, 0, 2, 1) && at(px, w, 1, 0, 2, 0) &&
          at(px, w, 0, 2, 0, 1), "mirror H, then rot90");
    free(px);

    px = grid(3, 2); w = 3; h = 2;
    CHECK(far2l_img_transform_px(&px, &w, &h, 0) && at(px, w, 1, 1, 1, 1),
          "no transform: unchanged");
    free(px);
}

static void test_geometry(void)
{
    Far2lImage img;
    Far2lRect r;
    int c0, r0, c1, r1;

    memset(&img, 0, sizeof(img));
    img.w = 10; img.h = 20;
    /* stretched over cells 1,2 .. 3,4 inclusive */
    img.left = 1; img.top = 2; img.right = 3; img.bottom = 4;
    CHECK(far2l_img_rect(&img, 8, 16, &r) && r.x0 == 8 && r.y0 == 32 &&
          r.x1 == 32 && r.y1 == 80, "stretch rect (%d,%d,%d,%d)", r.x0, r.y0, r.x1, r.y1);

    /* own size, shifted 5 right and 7 down */
    img.pixel_offset = true; img.right = 5; img.bottom = 7;
    CHECK(far2l_img_rect(&img, 8, 16, &r) && r.x0 == 13 && r.y0 == 39 &&
          r.x1 == 23 && r.y1 == 59, "pixel offset rect");
    /* a zero or negative offset shifts nothing */
    img.right = 0; img.bottom = -1;
    CHECK(far2l_img_rect(&img, 8, 16, &r) && r.x0 == 8 && r.y0 == 32,
          "pixel offset 0 / -1");

    /* -1 on one axis: that axis at its own size, the other stretched */
    img.pixel_offset = false; img.right = -1; img.bottom = 4;
    CHECK(far2l_img_rect(&img, 8, 16, &r) && r.x1 - r.x0 == 10 && r.y1 - r.y0 == 48,
          "own width, stretched height");

    /* right before left: nothing shown */
    img.right = 0; img.left = 3;
    CHECK(!far2l_img_rect(&img, 8, 16, &r), "right < left: empty");

    r.x0 = -3; r.y0 = 0; r.x1 = 9; r.y1 = 17;
    CHECK(far2l_img_rect_cells(&r, 8, 16, &c0, &r0, &c1, &r1) &&
          c0 == -1 && r0 == 0 && c1 == 1 && r1 == 1, "cells of a pixel rect");
    r.x1 = r.x0;
    CHECK(!far2l_img_rect_cells(&r, 8, 16, &c0, &r0, &c1, &r1), "empty: no cells");
}

static void test_set_rules(void)
{
    Far2lImageStore st = { 0 };
    Far2lImageEnv e = env_default();
    unsigned char out[F2L_IMG_REPLY_MAX];
    unsigned char px[2 * 40 * 4];
    unsigned char rgb[3] = { 10, 20, 30 };
    const Far2lImage *img;
    Stk s = { 0 };
    int n;

    memset(px, 0x80, sizeof(px));

    /* a new image at -1/-1: the cursor column, ending on the cursor row */
    e.cur_x = 10; e.cur_y = 20;
    build_set(&s, "a", F2L_IMG_RGBA, -1, -1, -1, -1, 2, 40, px, sizeof(px));
    n = req(&st, &e, &s, out);
    img = find(&st, "a");
    CHECK(n == 2 && out[0] == 1 && img && img->left == 10 && img->top == 17,
          "cursor placement (top %d)", img ? img->top : -99);
    CHECK(img && !img->opaque && img->px[0] == 0x40,
          "alpha 0x80 premultiplied: 0x80 * 0x80 / 255 = 0x40");

    /* setting it again: -1 keeps the place, a value replaces it */
    build_set(&s, "a", F2L_IMG_RGB, -1, 5, -1, -1, 1, 1, rgb, 3);
    n = req(&st, &e, &s, out);
    img = find(&st, "a");
    CHECK(n == 2 && out[0] == 1 && img && img->left == 10 && img->top == 5 &&
          img->w == 1 && st.n == 1 && st.bytes == 4, "replace keeps -1 fields");
    CHECK(img && img->px[0] == 30 && img->px[1] == 20 && img->px[2] == 10 &&
          img->px[3] == 255 && img->opaque, "RGB to BGRA");

    /* a cursor near the top clamps at row 0 */
    e.cur_y = 1;
    build_set(&s, "b", F2L_IMG_RGBA, -1, -1, -1, -1, 2, 40, px, sizeof(px));
    req(&st, &e, &s, out);
    img = find(&st, "b");
    CHECK(img && img->top == 0, "cursor placement clamps at row 0");

    /* zero size: 0, nothing read */
    build_set(&s, "c", F2L_IMG_RGBA, 0, 0, -1, -1, 0, 1, NULL, 0);
    n = req(&st, &e, &s, out);
    CHECK(n == 2 && out[0] == 0 && !find(&st, "c"), "zero width: 0");

    /* an unknown format: the ID alone */
    build_set(&s, "c", 7, 0, 0, -1, -1, 1, 1, rgb, 3);
    CHECK(req(&st, &e, &s, out) == 1, "unknown format: empty reply");

    /* fewer pixels than the size says: the ID alone */
    build_set(&s, "c", F2L_IMG_RGBA, 0, 0, -1, -1, 2, 2, px, 12);
    CHECK(req(&st, &e, &s, out) == 1, "short data: empty reply");

    /* a truncated stack: the ID alone */
    build_set(&s, "c", F2L_IMG_RGB, 0, 0, -1, -1, 1, 1, rgb, 3);
    CHECK(far2l_img_request(&st, &e, s.b + 5, s.n - 5, out) == 1,
          "truncated stack: empty reply");

    /* attaching is not offered */
    build_set(&s, "a", F2L_IMG_RGB | 0x020000, 0, 0, -1, -1, 1, 1, rgb, 3);
    n = req(&st, &e, &s, out);
    CHECK(n == 2 && out[0] == 0, "attach: 0");

    /* images off: 0, nothing stored */
    e.enabled = false;
    build_set(&s, "d", F2L_IMG_RGB, 0, 0, -1, -1, 1, 1, rgb, 3);
    n = req(&st, &e, &s, out);
    CHECK(n == 2 && out[0] == 0 && !find(&st, "d"), "images off: 0");
    e.enabled = true;

    /* PNG without a decoder: 0; with one: the decoded size */
    build_set(&s, "p", F2L_IMG_PNG, 0, 0, -1, -1, 8, 1, "PNGDATA!", 8);
    n = req(&st, &e, &s, out);
    CHECK(n == 2 && out[0] == 0, "PNG without a decoder: 0");
    e.codecs = F2L_IMGCAP_PNG;
    e.decode = fake_decode;
    n = req(&st, &e, &s, out);
    img = find(&st, "p");
    CHECK(n == 2 && out[0] == 1 && img && img->w == 3 && img->h == 2 && !img->opaque,
          "PNG decoded");
    /* PNG with height other than 1: 0 */
    build_set(&s, "q", F2L_IMG_PNG, 0, 0, -1, -1, 8, 2, "PNGDATA!PNGDATA!", 16);
    n = req(&st, &e, &s, out);
    CHECK(n == 2 && out[0] == 0, "PNG height 2: 0");

    /* transform of a missing image: 0; delete: 1 then 0 */
    build_transform(&s, "zz", 1, 1, -1, -1, 0);
    n = req(&st, &e, &s, out);
    CHECK(n == 2 && out[0] == 0, "transform missing: 0");
    build_transform(&s, "p", 4, -1, -1, -1, F2L_IMGTF_ROTATE90);
    nchanged = 0;
    n = req(&st, &e, &s, out);
    img = find(&st, "p");
    CHECK(n == 2 && out[0] == 1 && img && img->w == 2 && img->h == 3 &&
          img->left == 4 && nchanged == 1 && last_before.x0 == 0 &&
          last_after.x0 == 32, "transform moves and rotates");
    build_delete(&s, "p");
    nchanged = 0;
    n = req(&st, &e, &s, out);
    CHECK(n == 2 && out[0] == 1 && !find(&st, "p") && nchanged == 1 &&
          last_before.x0 == 32 && last_after.x1 == 0, "delete");
    n = req(&st, &e, &s, out);
    CHECK(n == 2 && out[0] == 0, "delete again: 0");

    /* an unknown sub-command: the ID alone */
    s.n = 0;
    push_num(&s, 'r', 1);
    CHECK(req(&st, &e, &s, out) == 1, "unknown sub-command: empty reply");

    far2l_img_store_clear(&st, &e);
    CHECK(st.n == 0 && st.bytes == 0, "cleared");
    free(s.b);
}

static void test_cap(void)
{
    Far2lImageStore st = { 0 };
    Far2lImageEnv e = env_default();
    unsigned char out[F2L_IMG_REPLY_MAX];
    unsigned char rgb[3] = { 1, 2, 3 };
    Stk s = { 0 };
    unsigned char *big;
    size_t bigsz = (size_t)4096 * 2048 * 3;   /* 8 Mpixel: 32 MB decoded */
    char id[8];
    int i, n;

    big = calloc(bigsz, 1);
    build_set(&s, "big1", F2L_IMG_RGB, 0, 0, -1, -1, 4096, 2048, big, bigsz);
    n = req(&st, &e, &s, out);
    CHECK(n == 2 && out[0] == 1, "first 32 MB image");
    build_set(&s, "big2", F2L_IMG_RGB, 0, 0, -1, -1, 4096, 2048, big, bigsz);
    n = req(&st, &e, &s, out);
    CHECK(n == 2 && out[0] == 1 && st.bytes == F2L_IMG_STORE_MAX_BYTES,
          "second: exactly the 64 MB cap");
    /* one more pixel is over the cap; the store is unchanged */
    build_set(&s, "x", F2L_IMG_RGB, 0, 0, -1, -1, 1, 1, rgb, 3);
    n = req(&st, &e, &s, out);
    CHECK(n == 2 && out[0] == 0 && st.n == 2, "over the cap: 0");
    /* replacing an image gives its own bytes back first */
    build_set(&s, "big2", F2L_IMG_RGB, 0, 0, -1, -1, 1, 1, rgb, 3);
    n = req(&st, &e, &s, out);
    CHECK(n == 2 && out[0] == 1 && st.bytes == (size_t)32 * 1024 * 1024 + 4,
          "replace within the cap");
    /* a side over 4096 or more than 8 Mpixel: 0 */
    free(big);
    bigsz = (size_t)4097 * 3;
    big = calloc(bigsz, 1);
    build_set(&s, "wide", F2L_IMG_RGB, 0, 0, -1, -1, 4097, 1, big, bigsz);
    n = req(&st, &e, &s, out);
    CHECK(n == 2 && out[0] == 0 && !find(&st, "wide"), "side over 4096: 0");
    free(big);
    far2l_img_store_clear(&st, NULL);

    /* the count limit */
    for (i = 0; i < F2L_IMG_MAX_COUNT; i++) {
        sprintf(id, "i%d", i);
        build_set(&s, id, F2L_IMG_RGB, 0, 0, -1, -1, 1, 1, rgb, 3);
        req(&st, &e, &s, out);
    }
    CHECK(st.n == F2L_IMG_MAX_COUNT, "%d images held", st.n);
    build_set(&s, "one-more", F2L_IMG_RGB, 0, 0, -1, -1, 1, 1, rgb, 3);
    n = req(&st, &e, &s, out);
    CHECK(n == 2 && out[0] == 0, "over the count: 0");
    build_set(&s, "i3", F2L_IMG_RGB, 0, 0, -1, -1, 1, 1, rgb, 3);
    n = req(&st, &e, &s, out);
    CHECK(n == 2 && out[0] == 1, "replacing at the count limit: 1");
    far2l_img_store_clear(&st, NULL);
    CHECK(st.bytes == 0, "all bytes given back");

    /* the data limit that follows the payload ceiling: raw pixels and a
     * file alike, at the limit accepted, one byte over refused */
    e.max_data = 12;
    {
        unsigned char px12[12] = { 0 }, big12[16] = { 0 };
        build_set(&s, "m", F2L_IMG_RGB, 0, 0, -1, -1, 4, 1, px12, 12);
        n = req(&st, &e, &s, out);
        CHECK(n == 2 && out[0] == 1, "data at the limit: 1");
        build_set(&s, "m2", F2L_IMG_RGBA, 0, 0, -1, -1, 4, 1, big12, 16);
        n = req(&st, &e, &s, out);
        CHECK(n == 2 && out[0] == 0 && !find(&st, "m2"), "data over the limit: 0");
        e.codecs = F2L_IMGCAP_PNG;
        e.decode = fake_decode;
        build_set(&s, "m3", F2L_IMG_PNG, 0, 0, -1, -1, 13, 1, "PNGDATA!12345", 13);
        n = req(&st, &e, &s, out);
        CHECK(n == 2 && out[0] == 0 && !find(&st, "m3"), "file over the limit: 0");
    }
    far2l_img_store_clear(&st, NULL);
    free(s.b);
}

static void test_to_bgra(void)
{
    unsigned char rgba[8] = { 200, 100, 50, 128, 1, 2, 3, 255 };
    unsigned char rgb[6] = { 9, 8, 7, 6, 5, 4 };
    unsigned char out[8];

    CHECK(!far2l_img_to_bgra(rgba, 4, 2, out), "RGBA with alpha: not opaque");
    CHECK(out[0] == 25 && out[1] == 50 && out[2] == 100 && out[3] == 128,
          "premultiplied: %d %d %d %d", out[0], out[1], out[2], out[3]);
    CHECK(out[4] == 3 && out[5] == 2 && out[6] == 1 && out[7] == 255,
          "opaque pixel kept as is");
    CHECK(far2l_img_to_bgra(rgb, 3, 2, out), "RGB: opaque");
    CHECK(out[0] == 7 && out[1] == 8 && out[2] == 9 && out[3] == 255 &&
          out[4] == 4 && out[6] == 6, "RGB to BGRA");
}

/* Every set, transform and delete is traced: served, refused and malformed. */
static char tr_op;
static int tr_ok, tr_n;
static char tr_id[16];

static void on_trace(void *ctx, char op, const char *id, size_t idlen, int ok,
                     const Far2lImage *img, uint64_t caps)
{
    (void)ctx; (void)img; (void)caps;
    tr_n++;
    tr_op = op;
    tr_ok = ok;
    if (idlen >= sizeof(tr_id))
        idlen = sizeof(tr_id) - 1;
    if (id)
        memcpy(tr_id, id, idlen);
    tr_id[id ? idlen : 0] = '\0';
}

static void test_trace(void)
{
    Far2lImageStore st = { 0 };
    Far2lImageEnv e = env_default();
    unsigned char out[F2L_IMG_REPLY_MAX];
    unsigned char rgb[3] = { 1, 2, 3 };
    Stk s = { 0 };

    e.trace = on_trace;
    tr_n = 0;
    build_set(&s, "a", F2L_IMG_RGB, 0, 0, -1, -1, 1, 1, rgb, 3);
    req(&st, &e, &s, out);
    CHECK(tr_n == 1 && tr_op == 's' && tr_ok == 1 && !strcmp(tr_id, "a"),
          "trace: served set");

    /* refused: a PNG with no decoder */
    tr_n = 0;
    build_set(&s, "p", F2L_IMG_PNG, 0, 0, -1, -1, 8, 1, "PNGDATA!", 8);
    req(&st, &e, &s, out);
    CHECK(tr_n == 1 && tr_op == 's' && tr_ok == 0 && !strcmp(tr_id, "p"),
          "trace: refused set");

    /* malformed: an unknown format, short data, a truncated stack */
    tr_n = 0;
    build_set(&s, "u", 7, 0, 0, -1, -1, 1, 1, rgb, 3);
    CHECK(req(&st, &e, &s, out) == 1 && tr_n == 1 && tr_ok == -1 &&
          !strcmp(tr_id, "u"), "trace: unknown format, ok -1");
    tr_n = 0;
    build_set(&s, "v", F2L_IMG_RGBA, 0, 0, -1, -1, 2, 2, rgb, 3);
    CHECK(req(&st, &e, &s, out) == 1 && tr_n == 1 && tr_ok == -1,
          "trace: short data, ok -1");
    tr_n = 0;
    build_delete(&s, "a");
    CHECK(far2l_img_request(&st, &e, s.b + 3, s.n - 3, out) == 1 &&
          tr_n == 1 && tr_op == 'd' && tr_ok == -1 && tr_id[0] == '\0',
          "trace: truncated delete, ok -1, no id");

    /* an unknown sub-command has no name to trace */
    tr_n = 0;
    s.n = 0;
    push_num(&s, 'r', 1);
    CHECK(req(&st, &e, &s, out) == 1 && tr_n == 0, "trace: unknown sub-command");

    far2l_img_store_clear(&st, NULL);
    free(s.b);
}

int main(void)
{
    test_caps();
    test_spec_set_and_transform();
    test_transforms();
    test_geometry();
    test_set_rules();
    test_trace();
    test_cap();
    test_to_bgra();
    if (failures) {
        printf("test_far2l_images: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_far2l_images: all passed\n");
    return 0;
}
