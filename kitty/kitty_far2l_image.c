/*
 * kitty_far2l_image.c - far2l terminal extensions: images, the plain-C half
 * (kitty_far2l_image.h): request parsing, the store, geometry, transforms.
 */
#include <stdlib.h>
#include <string.h>

#include "kitty_far2l_image.h"

/* ---- the request stack -------------------------------------------------- */

/* The stack is popped from its end; a value's bytes are little-endian with
 * its first byte deepest. Popping past the bottom marks the stack bad. */
typedef struct Pop {
    const unsigned char *b;
    size_t n;
    bool bad;
} Pop;

static uint64_t pop_num(Pop *p, int bytes)
{
    uint64_t v = 0;
    int i;
    if (p->bad || p->n < (size_t)bytes) {
        p->bad = true;
        return 0;
    }
    for (i = 0; i < bytes; i++)
        v |= (uint64_t)p->b[p->n - bytes + i] << (8 * i);
    p->n -= bytes;
    return v;
}

static int16_t pop_i16(Pop *p)
{
    return (int16_t)(uint16_t)pop_num(p, 2);
}

/* Raw bytes: the top `len` bytes, in their own order. */
static const unsigned char *pop_raw(Pop *p, size_t len)
{
    if (p->bad || p->n < len) {
        p->bad = true;
        return NULL;
    }
    p->n -= len;
    return p->b + p->n;
}

/* A string: a u32 length on top of its bytes. */
static const unsigned char *pop_str(Pop *p, size_t *len)
{
    uint64_t l = pop_num(p, 4);
    if (p->bad || l > p->n) {
        p->bad = true;
        *len = 0;
        return NULL;
    }
    *len = (size_t)l;
    return pop_raw(p, (size_t)l);
}

static int put_num(unsigned char *out, int at, uint64_t v, int bytes)
{
    int i;
    for (i = 0; i < bytes; i++)
        out[at + i] = (unsigned char)(v >> (8 * i));
    return at + bytes;
}

/* ---- pixels ------------------------------------------------------------- */

bool far2l_img_to_bgra(const unsigned char *src, int bpp, size_t npx,
                       unsigned char *dst)
{
    bool opaque = true;
    size_t i;
    for (i = 0; i < npx; i++, src += bpp, dst += 4) {
        unsigned a = bpp == 4 ? src[3] : 255;
        if (a == 255) {
            dst[0] = src[2];
            dst[1] = src[1];
            dst[2] = src[0];
        } else {
            opaque = false;
            dst[0] = (unsigned char)((src[2] * a + 127) / 255);
            dst[1] = (unsigned char)((src[1] * a + 127) / 255);
            dst[2] = (unsigned char)((src[0] * a + 127) / 255);
        }
        dst[3] = (unsigned char)a;
    }
    return opaque;
}

bool far2l_img_transform_px(unsigned char **px, int *w, int *h, unsigned tf)
{
    int W = *w, H = *h, x, y;
    unsigned rot = tf & F2L_IMGTF_MASK_ROTATE;
    uint32_t *src = (uint32_t *)*px, *dst;
    int nw = (rot & 1) ? H : W, nh = (rot & 1) ? W : H;

    if (!(tf & (F2L_IMGTF_MASK_ROTATE | F2L_IMGTF_MIRROR_H | F2L_IMGTF_MIRROR_V)))
        return true;
    dst = malloc((size_t)W * H * 4);
    if (!dst)
        return false;
    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            /* mirroring first, in the source's own frame */
            int sx = (tf & F2L_IMGTF_MIRROR_H) ? W - 1 - x : x;
            int sy = (tf & F2L_IMGTF_MIRROR_V) ? H - 1 - y : y;
            int dx, dy;
            switch (rot) {
              case F2L_IMGTF_ROTATE90:  dx = H - 1 - y; dy = x;         break;
              case F2L_IMGTF_ROTATE180: dx = W - 1 - x; dy = H - 1 - y; break;
              case F2L_IMGTF_ROTATE270: dx = y;         dy = W - 1 - x; break;
              default:                  dx = x;         dy = y;         break;
            }
            dst[(size_t)dy * nw + dx] = src[(size_t)sy * W + sx];
        }
    }
    free(*px);
    *px = (unsigned char *)dst;
    *w = nw;
    *h = nh;
    return true;
}

/* ---- geometry ----------------------------------------------------------- */

bool far2l_img_rect(const Far2lImage *img, int cw, int ch, Far2lRect *r)
{
    int x = cw * img->left, y = ch * img->top, w, h;
    if (img->pixel_offset || img->right == -1)
        w = img->w;
    else
        w = cw * (img->right + 1 - img->left);
    if (img->pixel_offset || img->bottom == -1)
        h = img->h;
    else
        h = ch * (img->bottom + 1 - img->top);
    if (img->pixel_offset) {
        if (img->right > 0)
            x += img->right;
        if (img->bottom > 0)
            y += img->bottom;
    }
    if (w <= 0 || h <= 0 || cw <= 0 || ch <= 0) {
        r->x0 = r->y0 = r->x1 = r->y1 = 0;
        return false;
    }
    r->x0 = x;
    r->y0 = y;
    r->x1 = x + w;
    r->y1 = y + h;
    return true;
}

/* floor(a / b) for b > 0 */
static int floor_div(int a, int b)
{
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}

bool far2l_img_rect_cells(const Far2lRect *r, int cw, int ch,
                          int *c0, int *r0, int *c1, int *r1)
{
    if (!r || r->x1 <= r->x0 || r->y1 <= r->y0 || cw <= 0 || ch <= 0)
        return false;
    *c0 = floor_div(r->x0, cw);
    *r0 = floor_div(r->y0, ch);
    *c1 = floor_div(r->x1 - 1, cw);
    *r1 = floor_div(r->y1 - 1, ch);
    return true;
}

/* ---- the store ---------------------------------------------------------- */

static int store_find(const Far2lImageStore *st, const unsigned char *id,
                      size_t idlen)
{
    int i;
    for (i = 0; i < st->n; i++)
        if (st->imgs[i].idlen == idlen && !memcmp(st->imgs[i].id, id, idlen))
            return i;
    return -1;
}

static void img_rect_or_empty(const Far2lImage *img, const Far2lImageEnv *env,
                              Far2lRect *r)
{
    if (!img || !env || !far2l_img_rect(img, env->cell_w, env->cell_h, r))
        r->x0 = r->y0 = r->x1 = r->y1 = 0;
}

static void store_remove(Far2lImageStore *st, int i)
{
    Far2lImage *img = &st->imgs[i];
    st->bytes -= (size_t)img->w * img->h * 4;
    free(img->id);
    free(img->px);
    memmove(st->imgs + i, st->imgs + i + 1,
            (size_t)(st->n - i - 1) * sizeof(*st->imgs));
    st->n--;
}

void far2l_img_store_clear(Far2lImageStore *st, const Far2lImageEnv *env)
{
    static const Far2lRect none = { 0, 0, 0, 0 };
    if (!st)
        return;
    while (st->n > 0) {
        Far2lRect before;
        img_rect_or_empty(&st->imgs[st->n - 1], env, &before);
        store_remove(st, st->n - 1);
        if (env && env->changed)
            env->changed(env->ctx, &before, &none);
    }
    free(st->imgs);
    st->imgs = NULL;
    st->size = 0;
    st->bytes = 0;
}

uint64_t far2l_img_caps(const Far2lImageEnv *env)
{
    if (!env->enabled || env->cell_w <= 0 || env->cell_h <= 0)
        return 0;
    return F2L_IMGCAP_RGBA | (env->codecs & F2L_IMGCAP_PNG) | F2L_IMGCAP_ROTMIR;
}

static void trace(const Far2lImageEnv *env, char op, const unsigned char *id,
                  size_t idlen, int ok, const Far2lImage *img, uint64_t caps)
{
    if (env->trace)
        env->trace(env->ctx, op, (const char *)id, idlen, ok, img, caps);
}

/* IMAGE_SET. Returns 1 or 0, or -1 for a malformed request (ID alone). */
static int img_set(Far2lImageStore *st, const Far2lImageEnv *env, Pop *p,
                   const unsigned char **id_out, size_t *idlen_out,
                   const Far2lImage **shown)
{
    size_t idlen, need;
    const unsigned char *id = pop_str(p, &idlen), *data;
    uint64_t flags = pop_num(p, 8);
    int16_t left = pop_i16(p), top = pop_i16(p);
    int16_t right = pop_i16(p), bottom = pop_i16(p);
    uint64_t width = pop_num(p, 4), height = pop_num(p, 4);
    unsigned fmt = (unsigned)(flags & F2L_IMG_MASK_FMT);
    unsigned char *px = NULL;
    int w, h, i;
    bool opaque;
    size_t bytes, held;
    Far2lImage *img;
    Far2lRect before, after;

    *id_out = id;
    *idlen_out = idlen;
    if (p->bad)
        return -1;
    if (!width || !height)
        return 0;                      /* nothing more is read, as far2l does */
    switch (fmt) {
      case F2L_IMG_PNG: case F2L_IMG_JPG: need = (size_t)width;  break;
      case F2L_IMG_RGB:                   need = 3;              break;
      case F2L_IMG_RGBA:                  need = 4;              break;
      default:
        return -1;                     /* an unknown format fails the request */
    }
    if (fmt == F2L_IMG_RGB || fmt == F2L_IMG_RGBA) {
        /* Sizes checked before they are multiplied: each one is at most the
         * stack length then, so the product cannot overflow. Pixels the
         * stack does not hold make the request malformed. */
        if (width > p->n || height > p->n || width * height > p->n / need)
            return -1;
        need *= (size_t)(width * height);
        if (width > F2L_IMG_MAX_SIDE || height > F2L_IMG_MAX_SIDE ||
            width * height > F2L_IMG_MAX_PIXELS) {
            pop_raw(p, need);
            return 0;
        }
    }
    data = pop_raw(p, need);
    if (!data)
        return -1;

    if (!env->enabled || idlen > F2L_IMG_MAX_ID ||
        (env->max_data && need > env->max_data) ||
        (flags & (F2L_IMG_MASK_ATTACH | F2L_IMG_SCROLL)))
        return 0;                      /* attaching is not offered */

    if (fmt == F2L_IMG_PNG || fmt == F2L_IMG_JPG) {
        if (height != 1 || !(env->codecs & F2L_IMGCAP_PNG) || !env->decode)
            return 0;
        if (!env->decode(env->ctx, fmt, data, need, &px, &w, &h) || !px)
            return 0;
        if (w <= 0 || h <= 0 || w > F2L_IMG_MAX_SIDE || h > F2L_IMG_MAX_SIDE ||
            (uint64_t)w * h > F2L_IMG_MAX_PIXELS) {
            free(px);
            return 0;
        }
        opaque = true;
        for (i = 0; i < w * h; i++)
            if (px[(size_t)i * 4 + 3] != 255) {
                opaque = false;
                break;
            }
    } else {
        w = (int)width;
        h = (int)height;
        px = malloc((size_t)w * h * 4);
        if (!px)
            return 0;
        opaque = far2l_img_to_bgra(data, fmt == F2L_IMG_RGB ? 3 : 4,
                                   (size_t)w * h, px);
    }

    /* the cap, with a replaced image's own bytes given back first */
    bytes = (size_t)w * h * 4;
    i = store_find(st, id, idlen);
    held = st->bytes - (i >= 0 ? (size_t)st->imgs[i].w * st->imgs[i].h * 4 : 0);
    if (held + bytes > F2L_IMG_STORE_MAX_BYTES ||
        (i < 0 && st->n >= F2L_IMG_MAX_COUNT)) {
        free(px);
        return 0;
    }

    if (i >= 0) {
        img = &st->imgs[i];
        img_rect_or_empty(img, env, &before);
        st->bytes -= (size_t)img->w * img->h * 4;
        free(img->px);
        /* -1 keeps what the image had */
        if (left != -1) img->left = left;
        if (top != -1) img->top = top;
        if (right != -1) img->right = right;
        if (bottom != -1) img->bottom = bottom;
    } else {
        char *idcopy = malloc(idlen ? idlen : 1);
        if (!idcopy) {
            free(px);
            return 0;
        }
        if (st->n >= st->size) {
            int nsize = st->size ? st->size * 2 : 8;
            Far2lImage *n = realloc(st->imgs, (size_t)nsize * sizeof(*n));
            if (!n) {
                free(idcopy);
                free(px);
                return 0;
            }
            st->imgs = n;
            st->size = nsize;
        }
        img = &st->imgs[st->n++];
        memset(img, 0, sizeof(*img));
        memcpy(idcopy, id, idlen);
        img->id = idcopy;
        img->idlen = idlen;
        before.x0 = before.y0 = before.x1 = before.y1 = 0;
        /* a new image at -1/-1 starts at the cursor column and ends on the
         * cursor row */
        if (left == -1)
            left = (int16_t)env->cur_x;
        if (top == -1) {
            int rows = env->cell_h > 0 ? (h + env->cell_h - 1) / env->cell_h : 0;
            top = (int16_t)(env->cur_y - rows < 0 ? 0 : env->cur_y - rows);
        }
        img->left = left;
        img->top = top;
        img->right = right;
        img->bottom = bottom;
    }
    img->px = px;
    img->w = w;
    img->h = h;
    img->opaque = opaque;
    img->pixel_offset = (flags & F2L_IMG_PIXEL_OFFSET) != 0;
    img->serial = ++st->serial;
    st->bytes += bytes;

    img_rect_or_empty(img, env, &after);
    if (env->changed)
        env->changed(env->ctx, &before, &after);
    *shown = img;
    return 1;
}

static int img_transform(Far2lImageStore *st, const Far2lImageEnv *env,
                         Pop *p, const unsigned char **id_out,
                         size_t *idlen_out, const Far2lImage **shown)
{
    size_t idlen;
    const unsigned char *id = pop_str(p, &idlen);
    int16_t left = pop_i16(p), top = pop_i16(p);
    int16_t right = pop_i16(p), bottom = pop_i16(p);
    unsigned tf = (unsigned)pop_num(p, 2);
    int i;
    Far2lImage *img;
    Far2lRect before, after;

    *id_out = id;
    *idlen_out = idlen;
    if (p->bad)
        return -1;
    i = store_find(st, id, idlen);
    if (i < 0)
        return 0;
    img = &st->imgs[i];
    img_rect_or_empty(img, env, &before);
    if (tf & (F2L_IMGTF_MASK_ROTATE | F2L_IMGTF_MIRROR_H | F2L_IMGTF_MIRROR_V)) {
        if (!far2l_img_transform_px(&img->px, &img->w, &img->h, tf))
            return 0;
        img->serial = ++st->serial;
    }
    if (left != -1) img->left = left;
    if (top != -1) img->top = top;
    if (right != -1) img->right = right;
    if (bottom != -1) img->bottom = bottom;
    img_rect_or_empty(img, env, &after);
    if (env->changed)
        env->changed(env->ctx, &before, &after);
    *shown = img;
    return 1;
}

static int img_delete(Far2lImageStore *st, const Far2lImageEnv *env, Pop *p,
                      const unsigned char **id_out, size_t *idlen_out)
{
    static const Far2lRect none = { 0, 0, 0, 0 };
    size_t idlen;
    const unsigned char *id = pop_str(p, &idlen);
    Far2lRect before;
    int i;

    *id_out = id;
    *idlen_out = idlen;
    if (p->bad)
        return -1;
    i = store_find(st, id, idlen);
    if (i < 0)
        return 0;
    img_rect_or_empty(&st->imgs[i], env, &before);
    store_remove(st, i);
    if (env->changed)
        env->changed(env->ctx, &before, &none);
    return 1;
}

int far2l_img_request(Far2lImageStore *st, const Far2lImageEnv *env,
                      const unsigned char *stk, size_t len,
                      unsigned char out[F2L_IMG_REPLY_MAX])
{
    Pop p;
    unsigned char sub;
    const unsigned char *id = NULL;
    size_t idlen = 0;
    const Far2lImage *shown = NULL;
    int ok, n;

    p.b = stk;
    p.n = len;
    p.bad = false;
    sub = (unsigned char)pop_num(&p, 1);
    if (p.bad)
        return 1;
    switch (sub) {
      case 'c': {
        uint64_t caps = far2l_img_caps(env);
        n = put_num(out, 0, caps, 8);
        n = put_num(out, n, (uint16_t)(int16_t)env->cell_w, 2);
        n = put_num(out, n, (uint16_t)(int16_t)env->cell_h, 2);
        trace(env, 'c', NULL, 0, 1, NULL, caps);
        return n + 1;
      }
      case 's':
        ok = img_set(st, env, &p, &id, &idlen, &shown);
        break;
      case 't':
        ok = img_transform(st, env, &p, &id, &idlen, &shown);
        break;
      case 'd':
        ok = img_delete(st, env, &p, &id, &idlen);
        break;
      default:
        return 1;                      /* unknown: the ID alone */
    }
    /* a malformed request is traced too (ok -1, answered with the ID alone),
     * so every set, transform and delete leaves a line */
    trace(env, (char)sub, id, idlen, ok, ok > 0 ? shown : NULL, 0);
    if (ok < 0)
        return 1;
    out[0] = (unsigned char)ok;
    return 2;
}
