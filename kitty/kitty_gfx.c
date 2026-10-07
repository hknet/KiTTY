/*
 * kitty_gfx.c - the kitty graphics protocol, the plain-C core (kitty_gfx.h):
 * control-data parser, chunk assembler and base64, image store, placements,
 * deletion, replies, and the line-movement operations of the placement list.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kitty_gfx.h"

/* ---- base64 ------------------------------------------------------------- */

static int b64_val(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

/* A quad of 2-4 chars (the rest '=' or missing) to 1-3 bytes. */
static int b64_quad(const unsigned char *q, int n, unsigned char *out)
{
    int v[4], i, bytes;
    for (i = 0; i < 4; i++) {
        if (i < n && q[i] != '=') {
            v[i] = b64_val(q[i]);
            if (v[i] < 0)
                return -1;
        } else {
            v[i] = -2;
        }
    }
    if (v[0] < 0 || v[1] < 0)
        return -1;
    out[0] = (unsigned char)((v[0] << 2) | (v[1] >> 4));
    bytes = 1;
    if (v[2] >= 0) {
        out[1] = (unsigned char)(((v[1] & 15) << 4) | (v[2] >> 2));
        bytes = 2;
        if (v[3] >= 0) {
            out[2] = (unsigned char)(((v[2] & 3) << 6) | v[3]);
            bytes = 3;
        }
    }
    return bytes;
}

bool gfx_b64_feed(const unsigned char *s, size_t len, unsigned char *carry,
                  int *ncarry, unsigned char *out, size_t *outlen)
{
    size_t o = 0, i;
    unsigned char q[4];
    for (i = 0; i < len; i++) {
        unsigned char c = s[i];
        if (c != '=' && b64_val(c) < 0)
            return false;
        if (*ncarry < 3) {
            carry[(*ncarry)++] = c;
            continue;
        }
        memcpy(q, carry, 3);
        q[3] = c;
        *ncarry = 0;
        {
            int n = b64_quad(q, 4, out + o);
            if (n < 0)
                return false;
            o += (size_t)n;
        }
    }
    *outlen = o;
    return true;
}

/* The upload ended: 2 or 3 carried chars are a final unpadded group. */
static bool b64_finish(unsigned char *carry, int *ncarry, unsigned char *out,
                       size_t *outlen)
{
    int n;
    *outlen = 0;
    if (*ncarry == 0)
        return true;
    if (*ncarry == 1)
        return false;
    n = b64_quad(carry, *ncarry, out);
    *ncarry = 0;
    if (n < 0)
        return false;
    *outlen = (size_t)n;
    return true;
}

/* ---- the parser --------------------------------------------------------- */

static bool parse_uint(const unsigned char *s, size_t len, size_t *pos,
                       uint32_t *v)
{
    uint64_t acc = 0;
    size_t start = *pos;
    while (*pos < len && s[*pos] >= '0' && s[*pos] <= '9') {
        acc = acc * 10 + (uint64_t)(s[*pos] - '0');
        if (acc > UINT32_MAX)
            return false;
        (*pos)++;
    }
    if (*pos == start)
        return false;
    *v = (uint32_t)acc;
    return true;
}

static bool parse_int(const unsigned char *s, size_t len, size_t *pos,
                      int32_t *v)
{
    bool neg = false;
    uint32_t u;
    if (*pos < len && s[*pos] == '-') {
        neg = true;
        (*pos)++;
    }
    if (!parse_uint(s, len, pos, &u))
        return false;
    if (neg) {
        if (u > 2147483648u)
            return false;
        *v = u == 2147483648u ? INT32_MIN : -(int32_t)u;
    } else {
        if (u > INT32_MAX)
            return false;
        *v = (int32_t)u;
    }
    return true;
}

static void set_err(char err[GFX_ERR_MAX], const char *msg)
{
    if (err) {
        strncpy(err, msg, GFX_ERR_MAX - 1);
        err[GFX_ERR_MAX - 1] = '\0';
    }
}

bool gfx_parse(const unsigned char *s, size_t len, GfxCmd *cmd,
               char err[GFX_ERR_MAX])
{
    size_t pos = 0;

    memset(cmd, 0, sizeof(*cmd));
    cmd->a = 't';
    cmd->t = 'd';
    cmd->f = 32;
    cmd->d = 'a';
    if (err)
        err[0] = '\0';

    while (pos < len && s[pos] != ';') {
        unsigned char key = s[pos++];
        uint32_t bit;
        uint32_t *u = NULL;
        int32_t *i32 = NULL;
        char *flag = NULL;
        const char *flagset = NULL;

        if (pos >= len || s[pos] != '=') {
            set_err(err, "malformed control data: no = after key");
            return false;
        }
        pos++;
        switch (key) {
          case 'a': bit = GFX_K_a; flag = &cmd->a; flagset = "tTpdqfac"; break;
          case 't': bit = GFX_K_t; flag = &cmd->t; flagset = "dfts"; break;
          case 'o': bit = GFX_K_o; flag = &cmd->o; flagset = "z"; break;
          case 'd': bit = GFX_K_d; flag = &cmd->d;
                    flagset = "aAiInNcCfFpPqQrRxXyYzZ"; break;
          case 'f': bit = GFX_K_f; u = &cmd->f; break;
          case 'm': bit = GFX_K_m; u = &cmd->m; break;
          case 'q': bit = GFX_K_q; u = &cmd->q; break;
          case 'i': bit = GFX_K_i; u = &cmd->i; break;
          case 'I': bit = GFX_K_I; u = &cmd->I; break;
          case 'p': bit = GFX_K_p; u = &cmd->p; break;
          case 's': bit = GFX_K_s; u = &cmd->s; break;
          case 'v': bit = GFX_K_v; u = &cmd->v; break;
          case 'S': bit = GFX_K_S; u = &cmd->S; break;
          case 'O': bit = GFX_K_O; u = &cmd->O; break;
          case 'x': bit = GFX_K_x; u = &cmd->x; break;
          case 'y': bit = GFX_K_y; u = &cmd->y; break;
          case 'w': bit = GFX_K_w; u = &cmd->w; break;
          case 'h': bit = GFX_K_h; u = &cmd->h; break;
          case 'X': bit = GFX_K_X; u = &cmd->X; break;
          case 'Y': bit = GFX_K_Y; u = &cmd->Y; break;
          case 'c': bit = GFX_K_c; u = &cmd->c; break;
          case 'r': bit = GFX_K_r; u = &cmd->r; break;
          case 'C': bit = GFX_K_C; u = &cmd->C; break;
          case 'U': bit = GFX_K_U; u = &cmd->U; break;
          case 'P': bit = GFX_K_P; u = &cmd->P; break;
          case 'Q': bit = GFX_K_Q; u = &cmd->Q; break;
          case 'N': bit = GFX_K_N; u = &cmd->N; break;
          case 'z': bit = GFX_K_z; i32 = &cmd->z; break;
          case 'H': bit = GFX_K_H; i32 = &cmd->H; break;
          case 'V': bit = GFX_K_V; i32 = &cmd->V; break;
          default:
            set_err(err, "unknown key in control data");
            return false;
        }
        if (cmd->have & bit) {
            set_err(err, "duplicate key in control data");
            return false;
        }
        cmd->have |= bit;
        if (flag) {
            if (pos >= len || !strchr(flagset, s[pos]) || s[pos] == '\0') {
                set_err(err, "bad flag value in control data");
                return false;
            }
            *flag = (char)s[pos++];
        } else if (u) {
            if (!parse_uint(s, len, &pos, u)) {
                set_err(err, "bad number in control data");
                return false;
            }
        } else {
            if (!parse_int(s, len, &pos, i32)) {
                set_err(err, "bad number in control data");
                return false;
            }
        }
        if (pos < len && s[pos] == ',') {
            pos++;
            if (pos >= len || s[pos] == ';') {
                set_err(err, "trailing comma in control data");
                return false;
            }
        } else if (pos < len && s[pos] != ';') {
            set_err(err, "junk after a value in control data");
            return false;
        }
    }
    if (pos < len) {
        pos++;                              /* ';' */
        cmd->payload = s + pos;
        cmd->payload_len = len - pos;
    }
    if ((cmd->have & GFX_K_f) && cmd->f != 24 && cmd->f != 32 && cmd->f != 100) {
        set_err(err, "unknown image format");
        return false;
    }
    if (cmd->m > 1 || cmd->q > 2 || cmd->C > 1) {
        set_err(err, "value out of range in control data");
        return false;
    }
    return true;
}

/* ---- pixels ------------------------------------------------------------- */

bool gfx_to_bgra(const unsigned char *src, int bpp, size_t npx,
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

/* ---- the store ---------------------------------------------------------- */

void gfx_store_init(GfxStore *st)
{
    memset(st, 0, sizeof(*st));
    st->next_key = 1;
    st->next_id = 1;
    st->max_side = GFX_MAX_SIDE;
    st->max_pixels = GFX_MAX_PIXELS;
    st->max_bytes = GFX_STORE_MAX_BYTES;
    st->max_images = GFX_MAX_IMAGES;
    st->max_placements = GFX_MAX_PLACEMENTS;
    st->pending_max = GFX_PENDING_MAX;
}

static void pending_free(GfxStore *st)
{
    free(st->pend.buf);
    memset(&st->pend, 0, sizeof(st->pend));
}

void gfx_store_free(GfxStore *st)
{
    int i;
    for (i = 0; i < st->n_imgs; i++)
        free(st->imgs[i].px);
    free(st->imgs);
    free(st->pls);
    pending_free(st);
    memset(st, 0, sizeof(*st));
}

static int img_index_by_key(const GfxStore *st, uint32_t key)
{
    int i;
    for (i = 0; i < st->n_imgs; i++)
        if (st->imgs[i].key == key)
            return i;
    return -1;
}

static int img_index_by_id(const GfxStore *st, uint32_t id)
{
    int i;
    if (!id)
        return -1;
    for (i = 0; i < st->n_imgs; i++)
        if (st->imgs[i].id == id)
            return i;
    return -1;
}

/* The newest image with this number. */
static int img_index_by_number(const GfxStore *st, uint32_t number)
{
    int i, best = -1;
    if (!number)
        return -1;
    for (i = 0; i < st->n_imgs; i++)
        if (st->imgs[i].number == number &&
            (best < 0 || st->imgs[i].key > st->imgs[best].key))
            best = i;
    return best;
}

const GfxImage *gfx_image_by_id(const GfxStore *st, uint32_t id)
{
    int i = img_index_by_id(st, id);
    return i < 0 ? NULL : &st->imgs[i];
}

const GfxImage *gfx_image_by_key(const GfxStore *st, uint32_t key)
{
    int i = img_index_by_key(st, key);
    return i < 0 ? NULL : &st->imgs[i];
}

static int placements_of(const GfxStore *st, uint32_t key)
{
    int i, n = 0;
    for (i = 0; i < st->n_pls; i++)
        if (st->pls[i].image_key == key)
            n++;
    return n;
}

static void pl_remove(GfxStore *st, int i)
{
    memmove(st->pls + i, st->pls + i + 1,
            (size_t)(st->n_pls - i - 1) * sizeof(*st->pls));
    st->n_pls--;
}

static void img_drop_placements(GfxStore *st, uint32_t key)
{
    int i;
    for (i = st->n_pls - 1; i >= 0; i--)
        if (st->pls[i].image_key == key)
            pl_remove(st, i);
}

static void img_remove(GfxStore *st, int i)
{
    GfxImage *img = &st->imgs[i];
    img_drop_placements(st, img->key);
    st->bytes -= (size_t)img->w * img->h * 4;
    free(img->px);
    memmove(st->imgs + i, st->imgs + i + 1,
            (size_t)(st->n_imgs - i - 1) * sizeof(*st->imgs));
    st->n_imgs--;
}

/* Images nothing can reach any more: no id, no number, no placement. */
static void sweep_anonymous(GfxStore *st)
{
    int i;
    for (i = st->n_imgs - 1; i >= 0; i--)
        if (!st->imgs[i].id && !st->imgs[i].number &&
            !placements_of(st, st->imgs[i].key))
            img_remove(st, i);
}

/* Make room for one more image of `bytes`: images without placements go
 * first, oldest use first. False when the caps still cannot be met. */
static bool make_room(GfxStore *st, size_t bytes)
{
    for (;;) {
        int i, victim = -1;
        if (st->bytes + bytes <= st->max_bytes && st->n_imgs < st->max_images)
            return true;
        for (i = 0; i < st->n_imgs; i++)
            if (!placements_of(st, st->imgs[i].key) &&
                (victim < 0 ||
                 st->imgs[i].last_use < st->imgs[victim].last_use))
                victim = i;
        if (victim < 0)
            return false;
        img_remove(st, victim);
    }
}

static uint32_t free_id(GfxStore *st)
{
    for (;;) {
        uint32_t id = st->next_id++;
        if (!st->next_id)
            st->next_id = 1;
        if (id && img_index_by_id(st, id) < 0)
            return id;
    }
}

static bool grow(void **p, int *cap, int need, size_t elem)
{
    void *n;
    int ncap;
    if (need <= *cap)
        return true;
    ncap = *cap ? *cap * 2 : 8;
    while (ncap < need)
        ncap *= 2;
    n = realloc(*p, (size_t)ncap * elem);
    if (!n)
        return false;
    *p = n;
    *cap = ncap;
    return true;
}

/* ---- placement geometry ------------------------------------------------- */

static int ceil_div(long a, int b)
{
    return (int)((a + b - 1) / b);
}

static void pl_fit(GfxPlacement *pl, int cw, int ch)
{
    long box_w, box_h;
    if (cw <= 0 || ch <= 0)
        return;
    if (pl->clipped) {
        /* fixed in pixels; only the cell counts follow the cell size */
    } else if (!pl->req_c && !pl->req_r) {
        pl->dst_w = pl->src_w;
        pl->dst_h = pl->src_h;
        pl->dst_dx = pl->dst_dy = 0;
    } else if (pl->req_c && pl->req_r) {
        double sx, sy, sc;
        box_w = (long)pl->req_c * cw - pl->X;
        box_h = (long)pl->req_r * ch - pl->Y;
        sx = (double)box_w / pl->src_w;
        sy = (double)box_h / pl->src_h;
        sc = sx < sy ? sx : sy;
        pl->dst_w = (int)(pl->src_w * sc + 0.5);
        pl->dst_h = (int)(pl->src_h * sc + 0.5);
        if (pl->dst_w < 1) pl->dst_w = 1;
        if (pl->dst_h < 1) pl->dst_h = 1;
        pl->dst_dx = (int)((box_w - pl->dst_w) / 2);
        pl->dst_dy = (int)((box_h - pl->dst_h) / 2);
    } else if (pl->req_r) {
        box_h = (long)pl->req_r * ch - pl->Y;
        pl->dst_h = (int)box_h;
        pl->dst_w = (int)((double)box_h * pl->src_w / pl->src_h + 0.5);
        if (pl->dst_w < 1) pl->dst_w = 1;
        pl->dst_dx = pl->dst_dy = 0;
    } else {
        box_w = (long)pl->req_c * cw - pl->X;
        pl->dst_w = (int)box_w;
        pl->dst_h = (int)((double)box_w * pl->src_h / pl->src_w + 0.5);
        if (pl->dst_h < 1) pl->dst_h = 1;
        pl->dst_dx = pl->dst_dy = 0;
    }
    pl->cols = pl->req_c ? pl->req_c
             : ceil_div((long)pl->X + pl->dst_dx + pl->dst_w, cw);
    pl->rows = pl->req_r && !pl->clipped ? pl->req_r
             : ceil_div((long)pl->Y + pl->dst_dy + pl->dst_h, ch);
    if (pl->cols < 1) pl->cols = 1;
    if (pl->rows < 1) pl->rows = 1;
}

void gfx_rescale(GfxStore *st, int cell_w, int cell_h)
{
    int i;
    for (i = 0; i < st->n_pls; i++)
        pl_fit(&st->pls[i], cell_w, cell_h);
}

/* ---- replies ------------------------------------------------------------ */

static void reply(GfxResult *res, const GfxCmd *c, uint32_t id, uint32_t q,
                  const char *code, const char *msg)
{
    char *b = res->reply;
    int n = 0;
    bool ok = !strcmp(code, "OK");
    res->reply[0] = '\0';
    res->reply_len = 0;
    if (q >= 2 || (q == 1 && ok))
        return;
    if (!id && !(c->have & GFX_K_I))
        return;
    n += snprintf(b + n, GFX_REPLY_MAX - (size_t)n, "\033_G");
    if (id)
        n += snprintf(b + n, GFX_REPLY_MAX - (size_t)n, "i=%lu",
                      (unsigned long)id);
    if (c->have & GFX_K_I)
        n += snprintf(b + n, GFX_REPLY_MAX - (size_t)n, "%sI=%lu",
                      id ? "," : "", (unsigned long)c->I);
    if (c->p)
        n += snprintf(b + n, GFX_REPLY_MAX - (size_t)n, ",p=%lu",
                      (unsigned long)c->p);
    if (ok)
        n += snprintf(b + n, GFX_REPLY_MAX - (size_t)n, ";OK");
    else
        n += snprintf(b + n, GFX_REPLY_MAX - (size_t)n, ";%s:%s", code, msg);
    if (n > GFX_REPLY_MAX - 3)
        n = GFX_REPLY_MAX - 3;
    b[n++] = '\033';
    b[n++] = '\\';
    b[n] = '\0';
    res->reply_len = n;
}

static void trace(const GfxEnv *env, char a, uint32_t id, const char *code)
{
    if (env->trace)
        env->trace(env->ctx, a, id, code);
}

/* ---- placing ------------------------------------------------------------ */

/* Returns an error code or NULL. */
static const char *place(GfxStore *st, const GfxEnv *env, GfxImage *img,
                         const GfxCmd *c, GfxResult *res, const char **msg)
{
    GfxPlacement pl, *slot = NULL;
    int i;
    long sx, sy, sw, sh;

    if (env->cell_w <= 0 || env->cell_h <= 0) {
        *msg = "cell size unknown";
        return "EINVAL";
    }
    sx = c->x < (uint32_t)img->w ? (long)c->x : img->w;
    sy = c->y < (uint32_t)img->h ? (long)c->y : img->h;
    sw = c->w ? (long)c->w : img->w;
    sh = c->h ? (long)c->h : img->h;
    if (sw > img->w - sx) sw = img->w - sx;
    if (sh > img->h - sy) sh = img->h - sy;
    if (sw <= 0 || sh <= 0) {
        *msg = "source rectangle outside the image";
        return "EINVAL";
    }
    memset(&pl, 0, sizeof(pl));
    pl.image_key = img->key;
    pl.image_id = img->id;
    pl.placement_id = img->id ? c->p : 0;
    pl.screen = env->screen;
    pl.abs_line = env->top_abs + env->cur_y;
    pl.col = env->cur_x;
    pl.src_x = (int)sx;
    pl.src_y = (int)sy;
    pl.src_w = (int)sw;
    pl.src_h = (int)sh;
    pl.X = c->X < (uint32_t)env->cell_w ? (int)c->X : env->cell_w - 1;
    pl.Y = c->Y < (uint32_t)env->cell_h ? (int)c->Y : env->cell_h - 1;
    pl.req_c = (int)(c->c > 10000 ? 10000 : c->c);
    pl.req_r = (int)(c->r > 10000 ? 10000 : c->r);
    pl.no_cursor = c->C == 1;
    pl.z = c->z;
    pl_fit(&pl, env->cell_w, env->cell_h);

    if (pl.placement_id)
        for (i = 0; i < st->n_pls; i++)
            if (st->pls[i].image_key == img->key &&
                st->pls[i].placement_id == pl.placement_id) {
                slot = &st->pls[i];
                break;
            }
    if (slot) {
        pl.serial = slot->serial;
    } else {
        if (st->n_pls >= st->max_placements) {
            *msg = "too many placements";
            return "ENOSPC";
        }
        if (!grow((void **)&st->pls, &st->cap_pls, st->n_pls + 1,
                  sizeof(*st->pls))) {
            *msg = "out of memory";
            return "ENOSPC";
        }
        slot = &st->pls[st->n_pls++];
        pl.serial = ++st->serial;
    }
    *slot = pl;
    img->last_use = ++st->tick;
    res->changed = true;
    if (!pl.no_cursor) {
        res->cur_dx = pl.cols;
        res->cur_dy = pl.rows - 1;
    }
    return NULL;
}

/* ---- transmitting ------------------------------------------------------- */

static void pending_start(GfxStore *st, const GfxCmd *c, const char *err,
                          const char *msg)
{
    pending_free(st);
    st->pend.active = true;
    st->pend.cmd = *c;
    st->pend.cmd.payload = NULL;
    st->pend.cmd.payload_len = 0;
    if (err) {
        st->pend.err_code = err;
        set_err(st->pend.err_msg, msg);
    }
}

static void pending_feed(GfxStore *st, const unsigned char *payload,
                         size_t len)
{
    GfxPending *p = &st->pend;
    size_t need, got;
    if (p->bad || p->big || p->err_code)
        return;
    need = p->len + len / 4 * 3 + 6;
    if (need > p->cap) {
        size_t ncap = p->cap ? p->cap * 2 : 4096;
        unsigned char *n;
        while (ncap < need)
            ncap *= 2;
        if (ncap > st->pending_max + 3)
            ncap = st->pending_max + 3;
        if (ncap < need) {
            p->big = true;
            return;
        }
        n = realloc(p->buf, ncap);
        if (!n) {
            p->big = true;
            return;
        }
        p->buf = n;
        p->cap = ncap;
    }
    if (!gfx_b64_feed(payload, len, p->carry, &p->ncarry, p->buf + p->len,
                      &got)) {
        p->bad = true;
        return;
    }
    p->len += got;
    if (p->len > st->pending_max)
        p->big = true;
}

/* The last chunk arrived: decode, store, place, reply. Returns success. */
static bool pending_finish(GfxStore *st, const GfxEnv *env, uint32_t q,
                           GfxResult *res)
{
    GfxPending *p = &st->pend;
    GfxCmd c = p->cmd;
    const char *code = NULL, *msg = "";
    char errmsg[GFX_ERR_MAX];
    unsigned char *data = NULL, *px = NULL, tailbuf[3];
    size_t datalen = 0, tail = 0;
    int w = 0, h = 0, idx;
    bool opaque = false;
    uint32_t reply_id = c.i;
    GfxImage *img = NULL;

    if (p->err_code) {
        code = p->err_code;
        memcpy(errmsg, p->err_msg, sizeof(errmsg));
        msg = errmsg;
    } else if (p->bad) {
        code = "EINVAL";
        msg = "invalid base64 payload";
    } else if (p->big) {
        code = "EFBIG";
        msg = "too much data";
    } else if (!b64_finish(p->carry, &p->ncarry, tailbuf, &tail)) {
        code = "EINVAL";
        msg = "invalid base64 payload";
    } else {
        if (tail) {
            if (p->len + tail > p->cap) {
                unsigned char *n = realloc(p->buf, p->len + tail);
                if (!n) {
                    code = "ENOSPC";
                    msg = "out of memory";
                } else {
                    p->buf = n;
                    p->cap = p->len + tail;
                }
            }
            if (!code) {
                memcpy(p->buf + p->len, tailbuf, tail);
                p->len += tail;
            }
        }
        if (!code) {
            data = p->buf;
            datalen = p->len;
            p->buf = NULL;              /* ours now */
        }
    }

    if (!code && c.o == 'z') {
        unsigned char *out = NULL;
        size_t outlen = 0, max_out = c.f == 100 ? st->pending_max
                      : (size_t)c.s * c.v * (c.f / 8);
        if (!data || !env->inflate(env->ctx, data, datalen, max_out, &out,
                                   &outlen) || !out) {
            code = "EINVAL";
            msg = "inflate failed";
        } else {
            free(data);
            data = out;
            datalen = outlen;
        }
    }
    if (!code && c.f == 100) {
        if (!data || datalen == 0 ||
            !env->decode_png(env->ctx, data, datalen, &px, &w, &h) || !px) {
            code = "EBADPNG";
            msg = "PNG decode failed";
        } else if (w <= 0 || h <= 0 || w > st->max_side || h > st->max_side ||
                   (uint64_t)w * (uint64_t)h > st->max_pixels) {
            free(px);
            px = NULL;
            code = "EFBIG";
            msg = "image too large";
        } else {
            size_t i, n = (size_t)w * h;
            opaque = true;
            for (i = 0; i < n; i++)
                if (px[i * 4 + 3] != 255) {
                    opaque = false;
                    break;
                }
        }
    } else if (!code) {
        int bpp = (int)c.f / 8;
        size_t need = (size_t)c.s * c.v * bpp;
        if (datalen != need) {
            code = "ENODATA";
            msg = "image data size does not match s*v";
        } else {
            w = (int)c.s;
            h = (int)c.v;
            px = malloc((size_t)w * h * 4);
            if (!px) {
                code = "ENOSPC";
                msg = "out of memory";
            } else {
                opaque = gfx_to_bgra(data, bpp, (size_t)w * h, px);
            }
        }
    }
    free(data);
    pending_free(st);

    if (!code && c.a == 'q') {
        free(px);
        px = NULL;
    } else if (!code) {
        size_t bytes = (size_t)w * h * 4;
        idx = -1;
        if (c.i) {
            idx = img_index_by_id(st, c.i);
            if (idx >= 0)
                img_remove(st, idx);            /* data and placements go */
        }
        if (!make_room(st, bytes) ||
            !grow((void **)&st->imgs, &st->cap_imgs, st->n_imgs + 1,
                  sizeof(*st->imgs))) {
            code = "ENOSPC";
            msg = "image store full";
            free(px);
            px = NULL;
        } else {
            img = &st->imgs[st->n_imgs++];
            memset(img, 0, sizeof(*img));
            img->key = st->next_key++;
            if (!st->next_key)
                st->next_key = 1;
            img->id = c.i ? c.i : (c.I ? free_id(st) : 0);
            img->number = c.I;
            img->w = w;
            img->h = h;
            img->px = px;
            img->opaque = opaque;
            img->serial = ++st->serial;
            img->last_use = ++st->tick;
            st->bytes += bytes;
            reply_id = img->id;
            res->changed = true;
            if (c.a == 'T')
                code = place(st, env, img, &c, res, &msg);
        }
    }
    sweep_anonymous(st);
    reply(res, &c, reply_id, q, code ? code : "OK", msg);
    trace(env, c.a, reply_id, code ? code : "OK");
    return code == NULL;
}

/* Validates a transmit command's keys; an error code or NULL. */
static const char *transmit_check(const GfxStore *st, const GfxEnv *env,
                                  const GfxCmd *c, const char **msg)
{
    if (c->t != 'd') {
        *msg = "only direct transmission (t=d) is supported";
        return "EINVAL";
    }
    if (c->U) {
        *msg = "Unicode placeholders are not supported";
        return "EINVAL";
    }
    if (c->have & (GFX_K_P | GFX_K_Q | GFX_K_H | GFX_K_V)) {
        *msg = "relative placements are not supported";
        return "EINVAL";
    }
    if (c->o == 'z' && !env->inflate) {
        *msg = "compression is not supported";
        return "EINVAL";
    }
    if (c->f == 100 && !env->decode_png) {
        *msg = "PNG is not supported";
        return "EINVAL";
    }
    if (c->s > (uint32_t)st->max_side || c->v > (uint32_t)st->max_side) {
        *msg = "image too large";
        return "EFBIG";
    }
    if (c->f != 100) {
        if (!c->s || !c->v) {
            *msg = "zero width or height";
            return "EINVAL";
        }
        if ((uint64_t)c->s * c->v > st->max_pixels ||
            (uint64_t)c->s * c->v * 4 > st->max_bytes) {
            *msg = "image too large";
            return "EFBIG";
        }
    }
    return NULL;
}

/* ---- deleting ----------------------------------------------------------- */

/* Deletes placements that pass filt; capital modes free the images left
 * without placements. */
typedef bool (*PlFilter)(const GfxPlacement *pl, const GfxEnv *env,
                         const GfxCmd *c);

static bool on_screen(const GfxPlacement *pl, const GfxEnv *env)
{
    return pl->screen == env->screen;
}

static bool f_all(const GfxPlacement *pl, const GfxEnv *env, const GfxCmd *c)
{
    (void)c;
    return on_screen(pl, env) && pl->abs_line + pl->rows > env->top_abs;
}

static bool f_id(const GfxPlacement *pl, const GfxEnv *env, const GfxCmd *c)
{
    (void)env;
    return c->i && pl->image_id == c->i &&
           (!c->p || pl->placement_id == c->p);
}

static bool f_range(const GfxPlacement *pl, const GfxEnv *env, const GfxCmd *c)
{
    (void)env;
    return pl->image_id && pl->image_id >= c->x && pl->image_id <= c->y;
}

static bool hit_col(const GfxPlacement *pl, long x1)
{
    return pl->col <= x1 - 1 && x1 - 1 < (long)pl->col + pl->cols;
}

static bool hit_row(const GfxPlacement *pl, const GfxEnv *env, long y1)
{
    long row0 = pl->abs_line - env->top_abs;
    return row0 <= y1 - 1 && y1 - 1 < row0 + pl->rows;
}

static bool f_x(const GfxPlacement *pl, const GfxEnv *env, const GfxCmd *c)
{
    return on_screen(pl, env) && hit_col(pl, c->x);
}

static bool f_y(const GfxPlacement *pl, const GfxEnv *env, const GfxCmd *c)
{
    return on_screen(pl, env) && hit_row(pl, env, c->y);
}

static bool f_point(const GfxPlacement *pl, const GfxEnv *env, const GfxCmd *c)
{
    return on_screen(pl, env) && hit_col(pl, c->x) && hit_row(pl, env, c->y);
}

static bool f_point_z(const GfxPlacement *pl, const GfxEnv *env,
                      const GfxCmd *c)
{
    return f_point(pl, env, c) && pl->z == c->z;
}

static bool f_cursor(const GfxPlacement *pl, const GfxEnv *env,
                     const GfxCmd *c)
{
    (void)c;
    return on_screen(pl, env) && hit_col(pl, (long)env->cur_x + 1) &&
           hit_row(pl, env, (long)env->cur_y + 1);
}

static bool f_z(const GfxPlacement *pl, const GfxEnv *env, const GfxCmd *c)
{
    return on_screen(pl, env) && pl->z == c->z;
}

static void delete_filtered(GfxStore *st, const GfxEnv *env, const GfxCmd *c,
                            PlFilter filt, bool free_images, GfxResult *res)
{
    uint32_t touched[GFX_MAX_PLACEMENTS];
    int ntouched = 0, i, j;
    for (i = st->n_pls - 1; i >= 0; i--) {
        if (!filt(&st->pls[i], env, c))
            continue;
        if (free_images && ntouched < GFX_MAX_PLACEMENTS)
            touched[ntouched++] = st->pls[i].image_key;
        pl_remove(st, i);
        res->changed = true;
    }
    for (i = 0; i < ntouched; i++) {
        j = img_index_by_key(st, touched[i]);
        if (j >= 0 && !placements_of(st, touched[i]))
            img_remove(st, j);
    }
}

static const char *do_delete(GfxStore *st, const GfxEnv *env, const GfxCmd *c,
                             GfxResult *res, const char **msg)
{
    char d = c->d;
    bool cap = d >= 'A' && d <= 'Z';
    int i;

    switch (d) {
      case 'a': case 'A':
        delete_filtered(st, env, c, f_all, cap, res);
        break;
      case 'i': case 'I':
        delete_filtered(st, env, c, f_id, cap, res);
        if (cap && !c->p && (i = img_index_by_id(st, c->i)) >= 0 &&
            !placements_of(st, st->imgs[i].key))
            img_remove(st, i);
        break;
      case 'n': case 'N':
        i = img_index_by_number(st, c->I);
        if (i >= 0) {
            uint32_t key = st->imgs[i].key;
            int k;
            for (k = st->n_pls - 1; k >= 0; k--)
                if (st->pls[k].image_key == key &&
                    (!c->p || st->pls[k].placement_id == c->p)) {
                    pl_remove(st, k);
                    res->changed = true;
                }
            if (cap && !placements_of(st, key))
                img_remove(st, i);
        }
        break;
      case 'r': case 'R':
        delete_filtered(st, env, c, f_range, cap, res);
        if (cap)
            for (i = st->n_imgs - 1; i >= 0; i--)
                if (st->imgs[i].id && st->imgs[i].id >= c->x &&
                    st->imgs[i].id <= c->y &&
                    !placements_of(st, st->imgs[i].key))
                    img_remove(st, i);
        break;
      case 'c': case 'C':
        delete_filtered(st, env, c, f_cursor, cap, res);
        break;
      case 'p': case 'P':
        delete_filtered(st, env, c, f_point, cap, res);
        break;
      case 'q': case 'Q':
        delete_filtered(st, env, c, f_point_z, cap, res);
        break;
      case 'x': case 'X':
        delete_filtered(st, env, c, f_x, cap, res);
        break;
      case 'y': case 'Y':
        delete_filtered(st, env, c, f_y, cap, res);
        break;
      case 'z': case 'Z':
        delete_filtered(st, env, c, f_z, cap, res);
        break;
      default:
        *msg = "animation frames are not supported";
        return "EINVAL";
    }
    sweep_anonymous(st);
    return NULL;
}

/* ---- the command -------------------------------------------------------- */

bool gfx_command(GfxStore *st, const GfxEnv *env, const unsigned char *s,
                 size_t len, GfxResult *res)
{
    GfxCmd c;
    char err[GFX_ERR_MAX];
    const char *code = NULL, *msg = "";
    int i;

    memset(res, 0, sizeof(*res));
    if (!gfx_parse(s, len, &c, err)) {
        reply(res, &c, c.i, c.q, "EINVAL", err);
        trace(env, c.a, c.i, "EINVAL");
        return false;
    }
    if (c.i && (c.have & GFX_K_I)) {
        reply(res, &c, c.i, c.q, "EINVAL",
              "image id and image number must not both be given");
        trace(env, c.a, c.i, "EINVAL");
        return false;
    }

    /* a continuation chunk: only m and q */
    if (st->pend.active && !(c.have & ~(GFX_K_m | GFX_K_q))) {
        uint32_t q = (c.have & GFX_K_q) ? c.q : st->pend.cmd.q;
        if (c.have & GFX_K_q)
            st->pend.cmd.q = c.q;
        pending_feed(st, c.payload, c.payload_len);
        if (c.m == 1)
            return true;
        return pending_finish(st, env, q, res);
    }
    if (st->pend.active)
        pending_free(st);                   /* aborted by another command */

    switch (c.a) {
      case 't': case 'T': case 'q':
        if (c.a == 'q' && !c.i)
            return false;                   /* a query needs an id */
        code = transmit_check(st, env, &c, &msg);
        pending_start(st, &c, code, msg);
        pending_feed(st, c.payload, c.payload_len);
        if (c.m == 1)
            return true;
        return pending_finish(st, env, c.q, res);
      case 'p':
        if (!c.i && !c.I)
            return false;
        if (c.U) {
            code = "EINVAL";
            msg = "Unicode placeholders are not supported";
        } else if (c.have & (GFX_K_P | GFX_K_Q | GFX_K_H | GFX_K_V)) {
            code = "EINVAL";
            msg = "relative placements are not supported";
        } else {
            i = c.i ? img_index_by_id(st, c.i) : img_index_by_number(st, c.I);
            if (i < 0) {
                code = "ENOENT";
                msg = "no image with this id";
            } else {
                GfxImage *img = &st->imgs[i];
                code = place(st, env, img, &c, res, &msg);
                reply(res, &c, img->id, c.q, code ? code : "OK", msg);
                trace(env, 'p', img->id, code ? code : "OK");
                return code == NULL;
            }
        }
        reply(res, &c, c.i, c.q, code, msg);
        trace(env, 'p', c.i, code);
        return false;
      case 'd':
        code = do_delete(st, env, &c, res, &msg);
        if (code) {
            reply(res, &c, c.i, c.q, code, msg);
            trace(env, 'd', c.i, code);
            return false;
        }
        trace(env, 'd', c.i, "OK");
        return true;                        /* deletions are not answered */
      default:                              /* a=f, a=a, a=c */
        reply(res, &c, c.i, c.q, "EINVAL", "animation is not supported");
        trace(env, c.a, c.i, "EINVAL");
        return false;
    }
}

bool gfx_command_cut(GfxStore *st, const GfxEnv *env, const unsigned char *s,
                     size_t len, GfxResult *res)
{
    GfxCmd c;
    size_t ctl = 0;

    memset(res, 0, sizeof(*res));
    while (ctl < len && s[ctl] != ';')
        ctl++;
    if (!gfx_parse(s, ctl, &c, NULL)) {
        /* the control data itself was cut: nothing to answer by */
        if (st->pend.active)
            pending_free(st);
        return false;
    }
    if (st->pend.active && !(c.have & ~(GFX_K_m | GFX_K_q))) {
        uint32_t q = (c.have & GFX_K_q) ? c.q : st->pend.cmd.q;
        if (c.have & GFX_K_q)
            st->pend.cmd.q = c.q;
        st->pend.big = true;
        if (c.m == 1)
            return false;
        pending_finish(st, env, q, res);
        return false;
    }
    if (st->pend.active)
        pending_free(st);
    if (c.a == 't' || c.a == 'T' || c.a == 'q') {
        if (c.a == 'q' && !c.i)
            return false;
        pending_start(st, &c, NULL, NULL);
        st->pend.big = true;
        if (c.m == 1)
            return false;
        pending_finish(st, env, c.q, res);
        return false;
    }
    reply(res, &c, c.i, c.q, "EFBIG", "too much data");
    trace(env, c.a, c.i, "EFBIG");
    return false;
}

/* ---- line movement ------------------------------------------------------ */

void gfx_set_base(GfxStore *st, int screen, long base_abs)
{
    int i;
    for (i = st->n_pls - 1; i >= 0; i--)
        if (st->pls[i].screen == screen &&
            st->pls[i].abs_line + st->pls[i].rows <= base_abs)
            pl_remove(st, i);
    sweep_anonymous(st);
}

void gfx_scroll_region_sb(GfxStore *st, int screen, long top_abs_after,
                          int bot, int n)
{
    long top_before = top_abs_after - n;
    int i;
    for (i = 0; i < st->n_pls; i++) {
        GfxPlacement *pl = &st->pls[i];
        long row0, row1;
        if (pl->screen != screen)
            continue;
        row0 = pl->abs_line - top_before;
        row1 = row0 + pl->rows - 1;
        if (row1 <= bot)
            continue;         /* in the band or the scrollback: moved with its text */
        pl->abs_line += n;    /* reaches below the band: kept its screen row */
    }
}

/* Cuts k rows off the top (k > 0) or bottom (k < 0) of a placement; false
 * when nothing of the image is left. */
static bool pl_clip(GfxPlacement *pl, int k, int cw, int ch)
{
    long off = (long)pl->Y + pl->dst_dy;
    if (k > 0) {
        long cut = (long)k * ch - off;      /* drawn pixels lost at the top */
        if (cut >= pl->dst_h)
            return false;
        if (cut > 0) {
            long src_cut = cut * pl->src_h / pl->dst_h;
            pl->src_y += (int)src_cut;
            pl->src_h -= (int)src_cut;
            pl->dst_h -= (int)cut;
            off = 0;
        } else {
            off = -cut;
        }
        pl->Y = (int)off;
        pl->dst_dy = 0;
        pl->abs_line += k;
    } else {
        long vis = ((long)pl->rows + k) * ch - off;
        if (vis <= 0)
            return false;
        if (vis < pl->dst_h) {
            long src_keep = vis * pl->src_h / pl->dst_h;
            pl->src_h = (int)src_keep;
            pl->dst_h = (int)vis;
        }
    }
    if (pl->src_h <= 0 || pl->dst_h <= 0)
        return false;
    pl->clipped = true;
    pl_fit(pl, cw, ch);
    return true;
}

void gfx_scroll_region(GfxStore *st, int screen, long top_abs, int top,
                       int bot, int n, int cell_w, int cell_h)
{
    int i;
    for (i = st->n_pls - 1; i >= 0; i--) {
        GfxPlacement *pl = &st->pls[i];
        long row0, row1;
        if (pl->screen != screen)
            continue;
        row0 = pl->abs_line - top_abs;
        row1 = row0 + pl->rows - 1;
        if (row0 < top || row1 > bot)
            continue;                       /* not entirely in the band */
        row0 -= n;
        row1 -= n;
        pl->abs_line -= n;
        if (row1 < top || row0 > bot) {
            pl_remove(st, i);
            continue;
        }
        if (row0 < top) {
            if (!pl_clip(pl, (int)(top - row0), cell_w, cell_h))
                pl_remove(st, i);
        } else if (row1 > bot) {
            if (!pl_clip(pl, -(int)(row1 - bot), cell_w, cell_h))
                pl_remove(st, i);
        }
    }
    sweep_anonymous(st);
}

void gfx_clear_below(GfxStore *st, int screen, long abs_line)
{
    int i;
    for (i = st->n_pls - 1; i >= 0; i--)
        if (st->pls[i].screen == screen && st->pls[i].abs_line >= abs_line)
            pl_remove(st, i);
    sweep_anonymous(st);
}

void gfx_clear_screen(GfxStore *st, int screen, long top_abs)
{
    int i;
    for (i = st->n_pls - 1; i >= 0; i--)
        if (st->pls[i].screen == screen &&
            st->pls[i].abs_line + st->pls[i].rows > top_abs)
            pl_remove(st, i);
    sweep_anonymous(st);
}

void gfx_clear_all(GfxStore *st, int screen)
{
    int i;
    for (i = st->n_pls - 1; i >= 0; i--)
        if (st->pls[i].screen == screen)
            pl_remove(st, i);
    sweep_anonymous(st);
}

void gfx_reset(GfxStore *st)
{
    GfxStore fresh;
    gfx_store_init(&fresh);
    fresh.max_side = st->max_side;
    fresh.max_pixels = st->max_pixels;
    fresh.max_bytes = st->max_bytes;
    fresh.max_images = st->max_images;
    fresh.max_placements = st->max_placements;
    fresh.pending_max = st->pending_max;
    gfx_store_free(st);
    *st = fresh;
}

/* ---- the visible list --------------------------------------------------- */

static int vis_cmp(const void *a, const void *b)
{
    const GfxVisible *x = a, *y = b;
    if (x->z != y->z)
        return x->z < y->z ? -1 : 1;
    if (x->pl->image_id != y->pl->image_id)
        return x->pl->image_id < y->pl->image_id ? -1 : 1;
    if (x->pl->serial != y->pl->serial)
        return x->pl->serial < y->pl->serial ? -1 : 1;
    return 0;
}

int gfx_visible(const GfxStore *st, int screen, long top_abs, int rows,
                int cols, int cell_w, int cell_h, GfxVisible *out, int max)
{
    int i, n = 0;
    for (i = 0; i < st->n_pls && n < max; i++) {
        const GfxPlacement *pl = &st->pls[i];
        long row0 = pl->abs_line - top_abs;
        GfxVisible *v;
        if (pl->screen != screen)
            continue;
        if (row0 >= rows || row0 + pl->rows <= 0)
            continue;
        if (pl->col >= cols || pl->col + pl->cols <= 0)
            continue;
        v = &out[n++];
        v->img = gfx_image_by_key(st, pl->image_key);
        v->pl = pl;
        v->c0 = pl->col < 0 ? 0 : pl->col;
        v->c1 = pl->col + pl->cols - 1 < cols ? pl->col + pl->cols - 1
                                              : cols - 1;
        v->r0 = row0 < 0 ? 0 : (int)row0;
        v->r1 = row0 + pl->rows - 1 < rows ? (int)(row0 + pl->rows - 1)
                                           : rows - 1;
        v->dx0 = pl->col * cell_w + pl->X + pl->dst_dx;
        v->dy0 = (int)(row0 * cell_h) + pl->Y + pl->dst_dy;
        v->dx1 = v->dx0 + pl->dst_w;
        v->dy1 = v->dy0 + pl->dst_h;
        v->sx = pl->src_x;
        v->sy = pl->src_y;
        v->sw = pl->src_w;
        v->sh = pl->src_h;
        v->z = pl->z;
    }
    if (n > 1)
        qsort(out, (size_t)n, sizeof(*out), vis_cmp);
    return n;
}
