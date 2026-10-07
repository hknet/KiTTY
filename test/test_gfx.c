/*
 * test_gfx.c - the kitty graphics protocol core (kitty/kitty_gfx.c): the
 * control-data parser, base64 chunking, the store and its caps, raw and
 * (fake) PNG decoding, replies and q, every deletion mode, the placement
 * geometry and cursor movement, every line-movement operation, the
 * visible list. Builds on its own:
 *   gcc -std=c99 -Wall -Wextra -Wpedantic -fsanitize=address,undefined \
 *       -fno-sanitize-recover=all -o t test/test_gfx.c kitty/kitty_gfx.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../kitty/kitty_gfx.h"

static int failures, checks;

#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
    printf("\n"); } } while (0)

/* ---- helpers ------------------------------------------------------------ */

static const char b64c[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t b64enc(const unsigned char *in, size_t n, char *out)
{
    size_t i, o = 0;
    for (i = 0; i < n; i += 3) {
        unsigned v = in[i] << 16;
        if (i + 1 < n) v |= in[i + 1] << 8;
        if (i + 2 < n) v |= in[i + 2];
        out[o++] = b64c[(v >> 18) & 63];
        out[o++] = b64c[(v >> 12) & 63];
        out[o++] = i + 1 < n ? b64c[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? b64c[v & 63] : '=';
    }
    out[o] = '\0';
    return o;
}

/* Fake PNG: "PNG" w h (bytes) then one RGBA pixel colour for all. */
static bool fake_png(void *ctx, const unsigned char *d, size_t len,
                     unsigned char **px, int *w, int *h)
{
    size_t i, n;
    (void)ctx;
    if (len < 9 || memcmp(d, "PNG", 3))
        return false;
    *w = d[3] | (d[4] << 8);
    *h = d[5] | (d[6] << 8);
    if (*w <= 0 || *h <= 0)
        return false;
    n = (size_t)*w * *h;
    *px = malloc(n * 4);
    for (i = 0; i < n; i++)
        gfx_to_bgra(d + 7, 4, 1, *px + i * 4);
    return true;
}

/* Fake zlib: "Z" then the data, every byte XOR 0x55. */
static bool fake_inflate(void *ctx, const unsigned char *in, size_t len,
                         size_t max_out, unsigned char **out, size_t *outlen)
{
    size_t i;
    (void)ctx;
    if (len < 1 || in[0] != 'Z' || len - 1 > max_out)
        return false;
    *out = malloc(len);
    for (i = 1; i < len; i++)
        (*out)[i - 1] = in[i] ^ 0x55;
    *outlen = len - 1;
    return true;
}

static int ntrace;
static char trace_a;
static uint32_t trace_id;
static char trace_code[16];

static void on_trace(void *ctx, char a, uint32_t id, const char *code)
{
    (void)ctx;
    ntrace++;
    trace_a = a;
    trace_id = id;
    strncpy(trace_code, code, sizeof(trace_code) - 1);
}

static GfxEnv env_default(void)
{
    GfxEnv e;
    memset(&e, 0, sizeof(e));
    e.cell_w = 8;
    e.cell_h = 16;
    e.cols = 80;
    e.rows = 24;
    e.decode_png = fake_png;
    e.inflate = fake_inflate;
    e.trace = on_trace;
    return e;
}

static GfxResult last;

/* Sends "<ctl>;<base64 of data>" (no payload part when data is NULL);
 * returns the reply without ESC _G / ESC \ ("" when none). */
static const char *send(GfxStore *st, const GfxEnv *env, const char *ctl,
                        const void *data, size_t n)
{
    static char buf[1 << 17], out[GFX_REPLY_MAX];
    size_t len = strlen(ctl);
    memcpy(buf, ctl, len);
    if (data) {
        buf[len++] = ';';
        len += b64enc(data, n, buf + len);
    }
    gfx_command(st, env, (unsigned char *)buf, len, &last);
    out[0] = '\0';
    if (last.reply_len) {
        int l = last.reply_len - 5;
        memcpy(out, last.reply + 3, (size_t)l);
        out[l] = '\0';
    }
    return out;
}

/* Sends the control data with a raw payload string (already base64). */
static const char *send_raw(GfxStore *st, const GfxEnv *env, const char *ctl,
                            const char *payload)
{
    static char buf[1 << 17], out[GFX_REPLY_MAX];
    size_t len = strlen(ctl);
    memcpy(buf, ctl, len);
    if (payload) {
        buf[len++] = ';';
        memcpy(buf + len, payload, strlen(payload));
        len += strlen(payload);
    }
    gfx_command(st, env, (unsigned char *)buf, len, &last);
    out[0] = '\0';
    if (last.reply_len) {
        int l = last.reply_len - 5;
        memcpy(out, last.reply + 3, (size_t)l);
        out[l] = '\0';
    }
    return out;
}

static const unsigned char red[3] = { 255, 0, 0 };
static const unsigned char half_green[4] = { 0, 255, 0, 128 };

/* A w x h red image with id. */
static void put_red(GfxStore *st, const GfxEnv *env, uint32_t id, int w, int h)
{
    char ctl[64];
    unsigned char *d = malloc((size_t)w * h * 3);
    int i;
    for (i = 0; i < w * h; i++)
        memcpy(d + i * 3, red, 3);
    sprintf(ctl, "a=t,f=24,s=%d,v=%d,i=%lu", w, h, (unsigned long)id);
    send(st, env, ctl, d, (size_t)w * h * 3);
    free(d);
}

static const GfxPlacement *pl_of(const GfxStore *st, uint32_t id, uint32_t p)
{
    int i;
    for (i = 0; i < st->n_pls; i++)
        if (st->pls[i].image_id == id && st->pls[i].placement_id == p)
            return &st->pls[i];
    return NULL;
}

/* ---- the parser --------------------------------------------------------- */

static bool parse(const char *s, GfxCmd *c, char *err)
{
    return gfx_parse((const unsigned char *)s, strlen(s), c, err);
}

static void test_parser(void)
{
    GfxCmd c;
    char err[GFX_ERR_MAX];

    CHECK(parse("", &c, err) && c.a == 't' && c.t == 'd' && c.f == 32 &&
          c.d == 'a' && c.have == 0 && !c.payload, "defaults");
    CHECK(parse("a=T,f=100,i=7,I=0,p=3,q=2,m=1,s=10,v=20,x=1,y=2,w=3,h=4,"
                "X=5,Y=6,c=7,r=8,C=1,z=-5,o=z,t=d,S=9,O=8,N=1;QUJD", &c, err),
          "every key: %s", err);
    CHECK(c.a == 'T' && c.f == 100 && c.i == 7 && c.p == 3 && c.q == 2 &&
          c.m == 1 && c.s == 10 && c.v == 20 && c.x == 1 && c.y == 2 &&
          c.w == 3 && c.h == 4 && c.X == 5 && c.Y == 6 && c.c == 7 &&
          c.r == 8 && c.C == 1 && c.z == -5 && c.o == 'z' && c.N == 1,
          "every key: values");
    CHECK((c.have & GFX_K_I) && c.I == 0, "I=0 counts as given");
    CHECK(c.payload && c.payload_len == 4 && !memcmp(c.payload, "QUJD", 4),
          "payload after ;");
    CHECK(parse("a=d,d=Z,z=2147483647", &c, err) && c.z == INT32_MAX, "z max");
    CHECK(parse("z=-2147483648", &c, err) && c.z == INT32_MIN, "z min");
    CHECK(!parse("z=2147483648", &c, err), "z over");
    CHECK(parse("i=4294967295", &c, err) && c.i == 4294967295u, "i max");
    CHECK(!parse("i=4294967296", &c, err), "i over");
    CHECK(!parse("i=1,i=2", &c, err), "duplicate key");
    CHECK(!parse("i=1,k=2", &c, err), "unknown key");
    CHECK(!parse("i", &c, err), "no =");
    CHECK(!parse("i=", &c, err), "empty value");
    CHECK(!parse("i=x", &c, err), "non-number");
    CHECK(!parse("i=1,", &c, err), "trailing comma");
    CHECK(!parse(",i=1", &c, err), "leading comma");
    CHECK(!parse("i=1 ,p=2", &c, err), "junk after value");
    CHECK(!parse("a=X", &c, err), "bad action");
    CHECK(parse("a=f", &c, err) && c.a == 'f', "animation action parses");
    CHECK(!parse("t=x", &c, err), "bad medium");
    CHECK(parse("t=f", &c, err) && c.t == 'f', "t=f parses (refused later)");
    CHECK(!parse("o=y", &c, err), "bad compression");
    CHECK(!parse("d=k", &c, err), "bad delete mode");
    CHECK(!parse("f=16", &c, err), "bad format");
    CHECK(!parse("m=2", &c, err), "m range");
    CHECK(!parse("q=3", &c, err), "q range");
    CHECK(!parse("C=2", &c, err), "C range");
    CHECK(parse("U=1,P=2,Q=3,H=-1,V=1", &c, err) && c.U == 1 && c.H == -1,
          "refused keys parse");
    CHECK(parse("a=d", &c, err) && !c.payload && c.payload_len == 0, "no payload");
    CHECK(parse("m=0;", &c, err) && c.payload && c.payload_len == 0,
          "empty payload");
}

/* ---- base64 ------------------------------------------------------------- */

static void test_b64(void)
{
    unsigned char carry[3], out[64];
    int nc = 0;
    size_t n, total = 0;
    /* "Hello, World" = SGVsbG8sIFdvcmxk, split 5 + 7 + 4 */
    CHECK(gfx_b64_feed((unsigned char *)"SGVsb", 5, carry, &nc, out, &n) &&
          n == 3 && nc == 1, "chunk 1: 3 bytes, 1 carried (got %lu, %d)",
          (unsigned long)n, nc);
    total += n;
    CHECK(gfx_b64_feed((unsigned char *)"G8sIFdv", 7, carry, &nc, out + total,
                       &n) && n == 6 && nc == 0, "chunk 2: 6 bytes");
    total += n;
    CHECK(gfx_b64_feed((unsigned char *)"cmxk", 4, carry, &nc, out + total,
                       &n) && n == 3, "chunk 3");
    total += n;
    CHECK(total == 12 && !memcmp(out, "Hello, World", 12), "joined");
    nc = 0;
    CHECK(gfx_b64_feed((unsigned char *)"QQ==", 4, carry, &nc, out, &n) &&
          n == 1 && out[0] == 'A', "padding ==");
    CHECK(gfx_b64_feed((unsigned char *)"QUI=", 4, carry, &nc, out, &n) &&
          n == 2 && out[1] == 'B', "padding =");
    CHECK(!gfx_b64_feed((unsigned char *)"QU*I", 4, carry, &nc, out, &n),
          "bad char");
}

/* ---- transmit, raw formats, store --------------------------------------- */

static void test_transmit_raw(void)
{
    GfxStore st;
    GfxEnv e = env_default();
    const GfxImage *img;
    const char *r;
    unsigned char two[6] = { 255, 0, 0, 0, 0, 255 };

    gfx_store_init(&st);
    r = send(&st, &e, "a=t,f=24,s=1,v=1,i=1", red, 3);
    CHECK(!strcmp(r, "i=1;OK"), "raw f=24 reply '%s'", r);
    img = gfx_image_by_id(&st, 1);
    CHECK(img && img->w == 1 && img->h == 1 && img->opaque &&
          img->px[0] == 0 && img->px[1] == 0 && img->px[2] == 255 &&
          img->px[3] == 255, "red is BGRA 0 0 255 255");
    CHECK(st.bytes == 4 && st.n_imgs == 1, "store counts");
    CHECK(ntrace == 1 && trace_a == 't' && trace_id == 1 &&
          !strcmp(trace_code, "OK"), "trace line");

    r = send(&st, &e, "a=t,f=32,s=1,v=1,i=2", half_green, 4);
    img = gfx_image_by_id(&st, 2);
    CHECK(!strcmp(r, "i=2;OK") && img && !img->opaque &&
          img->px[1] == 128 && img->px[3] == 128, "f=32 premultiplied");

    r = send(&st, &e, "a=t,s=1,v=1,i=3", half_green, 4);
    CHECK(!strcmp(r, "i=3;OK"), "f defaults to 32");

    r = send(&st, &e, "a=t,f=24,s=2,v=1,i=4", red, 3);
    CHECK(!strncmp(r, "i=4;ENODATA:", 12), "short data: '%s'", r);
    CHECK(!gfx_image_by_id(&st, 4), "nothing stored on ENODATA");
    r = send(&st, &e, "a=t,f=24,s=1,v=1,i=4", two, 6);
    CHECK(!strncmp(r, "i=4;ENODATA:", 12), "long data: '%s'", r);
    r = send(&st, &e, "a=t,f=24,s=0,v=1,i=4", red, 3);
    CHECK(!strncmp(r, "i=4;EINVAL:", 11), "s=0: '%s'", r);
    r = send(&st, &e, "a=t,f=24,s=9000,v=1,i=4", red, 3);
    CHECK(!strncmp(r, "i=4;EFBIG:", 10), "side over cap: '%s'", r);
    r = send(&st, &e, "a=t,f=24,s=8192,v=8192,i=4", red, 3);
    CHECK(!strncmp(r, "i=4;EFBIG:", 10), "pixels over cap: '%s'", r);
    r = send(&st, &e, "a=t,f=24,s=8192,v=2048,i=4", red, 3);
    CHECK(!strncmp(r, "i=4;ENODATA:", 12), "16 Mpx exactly passes the cap");

    /* no id: no reply, image unreachable and swept */
    r = send(&st, &e, "a=t,f=24,s=1,v=1", red, 3);
    CHECK(r[0] == '\0' && st.n_imgs == 3, "a=t without id: silent, swept");

    /* a retransmit replaces data and drops placements */
    e.cur_x = 2;
    e.cur_y = 3;
    r = send(&st, &e, "a=p,i=1,p=9", NULL, 0);
    CHECK(!strcmp(r, "i=1,p=9;OK") && st.n_pls == 1, "placed: '%s'", r);
    put_red(&st, &e, 1, 2, 2);
    img = gfx_image_by_id(&st, 1);
    CHECK(img && img->w == 2 && st.n_pls == 0 && st.n_imgs == 3 &&
          st.bytes == 16 + 4 + 4, "retransmit: new data, placements gone");

    /* i and I together */
    r = send(&st, &e, "a=t,f=24,s=1,v=1,i=5,I=6", red, 3);
    CHECK(!strncmp(r, "i=5,I=6;EINVAL:", 15), "i and I: '%s'", r);

    /* a=q: validated, not stored, not replacing */
    r = send(&st, &e, "a=q,f=24,s=1,v=1,i=1", red, 3);
    img = gfx_image_by_id(&st, 1);
    CHECK(!strcmp(r, "i=1;OK") && img && img->w == 2, "query keeps image 1");
    r = send(&st, &e, "a=q,f=24,s=1,v=1,i=77", red, 3);
    CHECK(!strcmp(r, "i=77;OK") && !gfx_image_by_id(&st, 77), "query stores nothing");
    r = send(&st, &e, "a=q,f=24,s=2,v=1,i=77", red, 3);
    CHECK(!strncmp(r, "i=77;ENODATA", 12), "query reports errors");
    r = send(&st, &e, "a=q,f=24,s=1,v=1", red, 3);
    CHECK(r[0] == '\0', "query without id: ignored");
    gfx_store_free(&st);
}

static void test_numbers(void)
{
    GfxStore st;
    GfxEnv e = env_default();
    const GfxImage *a, *b;
    const char *r;

    gfx_store_init(&st);
    put_red(&st, &e, 1, 1, 1);
    put_red(&st, &e, 2, 1, 1);
    r = send(&st, &e, "a=t,f=24,s=1,v=1,I=13", red, 3);
    CHECK(!strcmp(r, "i=3,I=13;OK"), "assigned id skips 1 and 2: '%s'", r);
    a = gfx_image_by_id(&st, 3);
    CHECK(a && a->number == 13, "number kept");
    r = send(&st, &e, "a=t,f=24,s=1,v=1,I=13", red, 3);
    CHECK(!strcmp(r, "i=4,I=13;OK") && st.n_imgs == 4,
          "same number: a new image: '%s'", r);
    b = gfx_image_by_id(&st, 4);
    r = send(&st, &e, "a=p,I=13", NULL, 0);
    CHECK(!strcmp(r, "i=4,I=13;OK") && st.n_pls == 1 &&
          st.pls[0].image_key == b->key, "put by number: newest: '%s'", r);
    r = send(&st, &e, "a=p,I=99", NULL, 0);
    CHECK(!strncmp(r, "I=99;ENOENT:", 12), "unknown number: '%s'", r);
    r = send(&st, &e, "a=p,i=99", NULL, 0);
    CHECK(!strncmp(r, "i=99;ENOENT:", 12), "unknown id: '%s'", r);
    r = send(&st, &e, "a=p", NULL, 0);
    CHECK(r[0] == '\0', "put without id or number: ignored");
    gfx_store_free(&st);
}

static void test_store_caps(void)
{
    GfxStore st;
    GfxEnv e = env_default();
    const char *r;
    int i;

    gfx_store_init(&st);
    st.max_bytes = 5 * 4;               /* five 1x1 images */
    st.max_images = 3;
    for (i = 1; i <= 3; i++)
        put_red(&st, &e, (uint32_t)i, 1, 1);
    CHECK(st.n_imgs == 3, "three held");
    /* image 2 is used again: 1 is the oldest unplaced */
    r = send(&st, &e, "a=p,i=2", NULL, 0);
    put_red(&st, &e, 4, 1, 1);
    CHECK(st.n_imgs == 3 && !gfx_image_by_id(&st, 1) && gfx_image_by_id(&st, 4),
          "count cap: oldest unplaced (1) evicted");
    /* bytes cap: 2 (placed) and 3, 4 held = 12 bytes; a 2x1 (8) needs 20 */
    put_red(&st, &e, 5, 2, 1);
    CHECK(gfx_image_by_id(&st, 5) && !gfx_image_by_id(&st, 3) &&
          gfx_image_by_id(&st, 4), "bytes cap: 3 evicted, 4 kept");
    /* place everything, then nothing can be evicted */
    send(&st, &e, "a=p,i=4", NULL, 0);
    send(&st, &e, "a=p,i=5", NULL, 0);
    r = send(&st, &e, "a=t,f=24,s=1,v=1,i=6", red, 3);
    CHECK(!strncmp(r, "i=6;ENOSPC:", 11) && st.n_imgs == 3,
          "all placed: ENOSPC '%s'", r);
    /* an image over the whole store cap is refused before decoding */
    r = send(&st, &e, "a=t,f=24,s=3,v=2,i=6", red, 3);
    CHECK(!strncmp(r, "i=6;EFBIG:", 10), "over the store cap: '%s'", r);

    /* placements cap */
    st.max_placements = 2;
    r = send(&st, &e, "a=p,i=4", NULL, 0);
    CHECK(!strncmp(r, "i=4;ENOSPC:", 11) && st.n_pls == 3,
          "placement cap: '%s' (%d held)", r, st.n_pls);
    gfx_store_free(&st);
}

/* ---- chunking ----------------------------------------------------------- */

static void test_chunks(void)
{
    GfxStore st;
    GfxEnv e = env_default();
    const char *r;
    char enc[64];
    unsigned char px[12] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
    const GfxImage *img;

    gfx_store_init(&st);
    b64enc(px, 12, enc);                /* 16 chars */
    {
        char c1[8], c2[8], c3[8];
        memcpy(c1, enc, 5); c1[5] = 0;      /* mid-quad split */
        memcpy(c2, enc + 5, 7); c2[7] = 0;
        memcpy(c3, enc + 12, 4); c3[4] = 0;
        r = send_raw(&st, &e, "a=T,f=24,s=2,v=2,i=1,m=1", c1);
        CHECK(r[0] == '\0' && st.pend.active && st.n_imgs == 0,
              "first chunk: silent, pending");
        r = send_raw(&st, &e, "m=1", c2);
        CHECK(r[0] == '\0' && st.pend.active, "middle chunk: silent");
        r = send_raw(&st, &e, "m=0", c3);
        CHECK(!strcmp(r, "i=1;OK") && !st.pend.active, "last chunk: '%s'", r);
        img = gfx_image_by_id(&st, 1);
        CHECK(img && img->w == 2 && img->h == 2 && img->px[0] == 3 &&
              img->px[2] == 1 && img->px[12] == 12 && img->px[14] == 10,
              "pixels reassembled across the split");
        CHECK(st.n_pls == 1 && last.cur_dx == 1 && last.cur_dy == 0,
              "a=T placed at the end, cursor moved");
    }

    /* q from a later chunk wins */
    r = send_raw(&st, &e, "a=t,f=24,s=2,v=2,i=2,m=1", enc);
    r = send_raw(&st, &e, "m=0,q=1", "");
    CHECK(r[0] == '\0' && gfx_image_by_id(&st, 2), "q=1 on the last chunk");
    r = send_raw(&st, &e, "a=t,f=24,s=2,v=2,i=3,m=1,q=2", enc);
    r = send_raw(&st, &e, "m=0", "");
    CHECK(r[0] == '\0' && gfx_image_by_id(&st, 3), "q=2 from the first chunk");

    /* a stray key aborts the pending upload, the command runs */
    r = send_raw(&st, &e, "a=t,f=24,s=2,v=2,i=4,m=1", enc);
    r = send(&st, &e, "a=t,f=24,s=1,v=1,i=5", red, 3);
    CHECK(!strcmp(r, "i=5;OK") && !st.pend.active && !gfx_image_by_id(&st, 4),
          "pending aborted by another transmit: '%s'", r);
    r = send_raw(&st, &e, "a=t,f=24,s=2,v=2,i=4,m=1", enc);
    r = send(&st, &e, "a=d,d=i,i=5", NULL, 0);
    CHECK(!st.pend.active, "pending aborted by a delete");
    r = send_raw(&st, &e, "m=0", enc);
    CHECK(!strcmp(r, "") && !gfx_image_by_id(&st, 4),
          "chunk without a pending upload: a zero-size transmit, no id");

    /* a refusal at the first chunk is answered at the last */
    r = send_raw(&st, &e, "a=t,f=24,s=0,v=2,i=6,m=1", enc);
    CHECK(r[0] == '\0' && st.pend.active, "refused first chunk: silent");
    r = send_raw(&st, &e, "m=1", enc);
    r = send_raw(&st, &e, "m=0", enc);
    CHECK(!strncmp(r, "i=6;EINVAL:", 11), "answered at the last chunk: '%s'", r);

    /* base64 errors */
    r = send_raw(&st, &e, "a=t,f=24,s=2,v=2,i=7,m=1", "QU*I");
    r = send_raw(&st, &e, "m=0", enc);
    CHECK(!strncmp(r, "i=7;EINVAL:", 11), "bad base64: '%s'", r);
    r = send_raw(&st, &e, "a=t,f=24,s=1,v=1,i=7", "/wAA");
    CHECK(!strcmp(r, "i=7;OK"), "unpadded-free quad");
    r = send_raw(&st, &e, "a=t,f=24,s=1,v=1,i=8", "/wA");
    CHECK(!strncmp(r, "i=8;ENODATA", 11), "3-char tail: 2 bytes, short");
    r = send_raw(&st, &e, "a=t,f=24,s=1,v=1,i=8", "/wAAQ");
    CHECK(!strncmp(r, "i=8;EINVAL", 10), "1-char tail: invalid");

    /* the pending cap */
    st.pending_max = 8;
    r = send_raw(&st, &e, "a=t,f=24,s=2,v=2,i=9,m=1", enc);
    r = send_raw(&st, &e, "m=0", "");
    CHECK(!strncmp(r, "i=9;EFBIG:", 10) && !st.pend.active,
          "over the pending cap: '%s'", r);
    st.pending_max = GFX_PENDING_MAX;

    /* the terminal cut the APC at its ceiling (gfx_command_cut) */
    {
        static const char one[] = "a=T,f=24,s=1,v=1,i=10;/wA";
        static const char first[] = "a=t,f=24,s=2,v=2,i=11,m=1;QUFB";
        static const char more[] = "m=1;QUFB", last_c[] = "m=0,q=0;QU";
        static const char del[] = "a=d,d=i,i=12,q=0";
        static const char cut_ctl[] = "a=T,f=24,s=1,v=1,i=1";
        GfxResult rr;
        bool ok;
        ok = gfx_command_cut(&st, &e, (const unsigned char *)one,
                             sizeof(one) - 1, &rr);
        CHECK(!ok && !strncmp(rr.reply, "\033_Gi=10;EFBIG:", 14) &&
              !st.pend.active && !gfx_image_by_id(&st, 10),
              "cut single transmit: EFBIG '%s'", rr.reply);
        ok = gfx_command_cut(&st, &e, (const unsigned char *)first,
                             sizeof(first) - 1, &rr);
        CHECK(!ok && rr.reply_len == 0 && st.pend.active && st.pend.big,
              "cut first chunk: waits, marked big");
        r = send_raw(&st, &e, "m=1", enc);
        CHECK(r[0] == '\0' && st.pend.active, "an intact chunk after it: waits");
        ok = gfx_command_cut(&st, &e, (const unsigned char *)more,
                             sizeof(more) - 1, &rr);
        CHECK(!ok && rr.reply_len == 0 && st.pend.active, "a cut middle chunk");
        ok = gfx_command_cut(&st, &e, (const unsigned char *)last_c,
                             sizeof(last_c) - 1, &rr);
        CHECK(!ok && !strncmp(rr.reply, "\033_Gi=11;EFBIG:", 14) &&
              !st.pend.active && !gfx_image_by_id(&st, 11),
              "cut last chunk: EFBIG '%s'", rr.reply);
        ok = gfx_command_cut(&st, &e, (const unsigned char *)del,
                             sizeof(del) - 1, &rr);
        CHECK(!ok && !strncmp(rr.reply, "\033_Gi=12;EFBIG:", 14),
              "cut delete: EFBIG '%s'", rr.reply);
        /* the control data itself cut: silent, and a pending upload gone */
        r = send_raw(&st, &e, "a=t,f=24,s=2,v=2,i=13,m=1", enc);
        ok = gfx_command_cut(&st, &e, (const unsigned char *)cut_ctl,
                             sizeof(cut_ctl) - 3, &rr);
        CHECK(!ok && rr.reply_len == 0 && !st.pend.active,
              "cut control data: silent, pending dropped");
    }
    /* a deleted bottom row: placements from it down go, the rest stay */
    {
        int before = st.n_pls, i, have20 = 0, have21 = 0;
        e.cur_x = 0;
        e.cur_y = 5;
        r = send(&st, &e, "a=T,f=24,s=1,v=1,i=20", red, 3);
        e.cur_y = 7;
        r = send(&st, &e, "a=T,f=24,s=1,v=1,i=21", red, 3);
        gfx_clear_below(&st, 0, e.top_abs + 7);
        for (i = 0; i < st.n_pls; i++) {
            have20 += st.pls[i].image_id == 20;
            have21 += st.pls[i].image_id == 21;
        }
        CHECK(st.n_pls == before + 1 && have20 == 1 && have21 == 0,
              "clear below: row 7 gone, row 5 kept (%d left)", st.n_pls);
    }
    gfx_store_free(&st);
}

/* ---- PNG and compression ------------------------------------------------ */

static void test_png_zlib(void)
{
    GfxStore st;
    GfxEnv e = env_default();
    const char *r;
    unsigned char png[11] = { 'P', 'N', 'G', 3, 0, 2, 0, 0, 0, 255, 255 };
    unsigned char z[4], zp[12];
    const GfxImage *img;
    size_t i;

    gfx_store_init(&st);
    r = send(&st, &e, "a=t,f=100,i=1", png, 11);
    img = gfx_image_by_id(&st, 1);
    CHECK(!strcmp(r, "i=1;OK") && img && img->w == 3 && img->h == 2 &&
          img->px[0] == 255 && img->px[2] == 0 && img->opaque,
          "PNG via the decoder: '%s'", r);
    r = send(&st, &e, "a=t,f=100,s=99,v=99,i=1", png, 11);
    CHECK(!strcmp(r, "i=1;OK"), "s/v with PNG are ignored");
    r = send(&st, &e, "a=t,f=100,i=2", red, 3);
    CHECK(!strncmp(r, "i=2;EBADPNG:", 12), "bad PNG: '%s'", r);
    r = send(&st, &e, "a=t,f=100,i=2", NULL, 0);
    CHECK(!strncmp(r, "i=2;EBADPNG:", 12), "empty PNG: '%s'", r);
    png[3] = 0; png[4] = 0x21;              /* 8448 wide */
    r = send(&st, &e, "a=t,f=100,i=2", png, 11);
    CHECK(!strncmp(r, "i=2;EFBIG:", 10), "decoded too wide: '%s'", r);
    png[3] = 3; png[4] = 0;
    e.decode_png = NULL;
    r = send(&st, &e, "a=t,f=100,i=2", png, 11);
    CHECK(!strncmp(r, "i=2;EINVAL:", 11), "no decoder: '%s'", r);
    e.decode_png = fake_png;

    /* o=z around raw */
    z[0] = 'Z';
    for (i = 0; i < 3; i++)
        z[1 + i] = red[i] ^ 0x55;
    r = send(&st, &e, "a=t,f=24,s=1,v=1,o=z,i=3", z, 4);
    img = gfx_image_by_id(&st, 3);
    CHECK(!strcmp(r, "i=3;OK") && img && img->px[2] == 255, "o=z raw: '%s'", r);
    r = send(&st, &e, "a=t,f=24,s=1,v=1,o=z,i=4", red, 3);
    CHECK(!strncmp(r, "i=4;EINVAL:", 11), "inflate failure: '%s'", r);
    r = send(&st, &e, "a=t,f=24,s=1,v=1,o=z,i=4", png, 11);
    CHECK(!strncmp(r, "i=4;EINVAL:", 11), "inflate over expected size");
    /* o=z around PNG */
    zp[0] = 'Z';
    for (i = 0; i < 11; i++)
        zp[1 + i] = png[i] ^ 0x55;
    r = send(&st, &e, "a=t,f=100,o=z,i=5", zp, 12);
    CHECK(!strcmp(r, "i=5;OK") && gfx_image_by_id(&st, 5), "o=z PNG: '%s'", r);
    e.inflate = NULL;
    r = send(&st, &e, "a=t,f=24,s=1,v=1,o=z,i=6", z, 4);
    CHECK(!strncmp(r, "i=6;EINVAL:", 11), "no inflate: '%s'", r);
    gfx_store_free(&st);
}

/* ---- refusals and q ----------------------------------------------------- */

static void test_refusals_and_quiet(void)
{
    GfxStore st;
    GfxEnv e = env_default();
    const char *r;

    gfx_store_init(&st);
    put_red(&st, &e, 1, 1, 1);
    r = send(&st, &e, "a=t,f=24,s=1,v=1,t=f,i=2", red, 3);
    CHECK(!strncmp(r, "i=2;EINVAL:", 11), "t=f: '%s'", r);
    r = send(&st, &e, "a=t,f=24,s=1,v=1,t=s,i=2", red, 3);
    CHECK(!strncmp(r, "i=2;EINVAL:", 11), "t=s: '%s'", r);
    r = send(&st, &e, "a=T,f=24,s=1,v=1,U=1,i=2", red, 3);
    CHECK(!strncmp(r, "i=2;EINVAL:", 11), "U=1: '%s'", r);
    r = send(&st, &e, "a=p,i=1,U=1", NULL, 0);
    CHECK(!strncmp(r, "i=1;EINVAL:", 11), "U=1 on put: '%s'", r);
    r = send(&st, &e, "a=p,i=1,P=3,Q=1", NULL, 0);
    CHECK(!strncmp(r, "i=1;EINVAL:", 11), "relative: '%s'", r);
    r = send(&st, &e, "a=p,i=1,H=2", NULL, 0);
    CHECK(!strncmp(r, "i=1;EINVAL:", 11), "H alone: '%s'", r);
    r = send(&st, &e, "a=f,i=1,f=24,s=1,v=1", red, 3);
    CHECK(!strncmp(r, "i=1;EINVAL:", 11), "a=f: '%s'", r);
    r = send(&st, &e, "a=a,i=1,s=3", NULL, 0);
    CHECK(!strncmp(r, "i=1;EINVAL:", 11), "a=a: '%s'", r);
    r = send(&st, &e, "a=c,i=1,r=1,c=2", NULL, 0);
    CHECK(!strncmp(r, "i=1;EINVAL:", 11), "a=c: '%s'", r);
    r = send(&st, &e, "a=d,d=f,i=1", NULL, 0);
    CHECK(!strncmp(r, "i=1;EINVAL:", 11) && gfx_image_by_id(&st, 1),
          "d=f: '%s'", r);
    r = send(&st, &e, "a=T,f=24,s=1,v=1,N=1,i=3", red, 3);
    CHECK(!strcmp(r, "i=3;OK"), "N is accepted and ignored: '%s'", r);
    CHECK(st.n_pls == 1, "placed with N");

    /* malformed control data with an id still answers */
    r = send(&st, &e, "a=p,i=1,i=1", NULL, 0);
    CHECK(!strncmp(r, "i=1;EINVAL:", 11), "duplicate key answered: '%s'", r);
    r = send(&st, &e, "a=p,i=1,k=1", NULL, 0);
    CHECK(!strncmp(r, "i=1;EINVAL:", 11), "unknown key answered: '%s'", r);
    r = send(&st, &e, "k=1", NULL, 0);
    CHECK(r[0] == '\0', "unknown key without id: silent");

    /* q */
    r = send(&st, &e, "a=p,i=1,q=1", NULL, 0);
    CHECK(r[0] == '\0' && st.n_pls == 2, "q=1 hides OK");
    r = send(&st, &e, "a=p,i=99,q=1", NULL, 0);
    CHECK(!strncmp(r, "i=99;ENOENT:", 12), "q=1 keeps errors: '%s'", r);
    r = send(&st, &e, "a=p,i=99,q=2", NULL, 0);
    CHECK(r[0] == '\0', "q=2 hides errors");
    r = send(&st, &e, "a=t,f=24,s=1,v=1,i=4,q=2", red, 3);
    CHECK(r[0] == '\0' && gfx_image_by_id(&st, 4), "q=2 hides OK too");
    r = send(&st, &e, "a=t,f=24,s=0,v=1,i=4,q=1", red, 3);
    CHECK(!strncmp(r, "i=4;EINVAL:", 11), "q=1 error on transmit");
    /* the APC framing of a reply */
    send(&st, &e, "a=p,i=1,p=2", NULL, 0);
    CHECK(last.reply_len == (int)strlen("\033_Gi=1,p=2;OK\033\\") &&
          !memcmp(last.reply, "\033_Gi=1,p=2;OK\033\\", (size_t)last.reply_len),
          "reply framing");
    gfx_store_free(&st);
}

/* ---- placement geometry ------------------------------------------------- */

static void test_placement(void)
{
    GfxStore st;
    GfxEnv e = env_default();
    const char *r;
    const GfxPlacement *pl;

    gfx_store_init(&st);
    put_red(&st, &e, 1, 20, 40);        /* 20 x 40 px; cells 8 x 16 */
    e.cur_x = 5;
    e.cur_y = 7;
    e.top_abs = 100;

    r = send(&st, &e, "a=p,i=1,p=1", NULL, 0);
    pl = pl_of(&st, 1, 1);
    CHECK(pl && pl->abs_line == 107 && pl->col == 5, "anchor at the cursor");
    CHECK(pl->cols == 3 && pl->rows == 3 && pl->dst_w == 20 && pl->dst_h == 40,
          "native: ceil(20/8)=3, ceil(40/16)=3 (got %d x %d)", pl->cols, pl->rows);
    CHECK(last.cur_dx == 3 && last.cur_dy == 2, "cursor: +cols, +rows-1");
    CHECK(pl->src_x == 0 && pl->src_w == 20 && pl->src_h == 40, "whole source");

    r = send(&st, &e, "a=p,i=1,p=2,X=4,Y=10", NULL, 0);
    pl = pl_of(&st, 1, 2);
    CHECK(pl && pl->X == 4 && pl->Y == 10 && pl->cols == 3 && pl->rows == 4,
          "offsets count: ceil(24/8)=3, ceil(50/16)=4 (got %d x %d)",
          pl->cols, pl->rows);
    r = send(&st, &e, "a=p,i=1,p=3,X=99,Y=99", NULL, 0);
    pl = pl_of(&st, 1, 3);
    CHECK(pl && pl->X == 7 && pl->Y == 15, "X/Y clamped to the cell");

    r = send(&st, &e, "a=p,i=1,p=4,r=2", NULL, 0);
    pl = pl_of(&st, 1, 4);
    CHECK(pl && pl->rows == 2 && pl->dst_h == 32 && pl->dst_w == 16 &&
          pl->cols == 2, "r only: aspect gives 16 px, 2 cols (got %d, %d)",
          pl->dst_w, pl->cols);
    CHECK(last.cur_dx == 2 && last.cur_dy == 1, "cursor for r=2");

    r = send(&st, &e, "a=p,i=1,p=5,c=5", NULL, 0);
    pl = pl_of(&st, 1, 5);
    CHECK(pl && pl->cols == 5 && pl->dst_w == 40 && pl->dst_h == 80 &&
          pl->rows == 5, "c only: 40 px wide, 80 tall, 5 rows (got %d)", pl->rows);

    r = send(&st, &e, "a=p,i=1,p=6,c=10,r=2", NULL, 0);
    pl = pl_of(&st, 1, 6);
    CHECK(pl && pl->cols == 10 && pl->rows == 2 && pl->dst_h == 32 &&
          pl->dst_w == 16 && pl->dst_dx == 32 && pl->dst_dy == 0,
          "letterbox: 80x32 box, image 16x32 centred (dx %d)", pl->dst_dx);
    CHECK(last.cur_dx == 10 && last.cur_dy == 1, "cursor follows the box");

    r = send(&st, &e, "a=p,i=1,p=7,c=2,r=10", NULL, 0);
    pl = pl_of(&st, 1, 7);
    CHECK(pl && pl->dst_w == 16 && pl->dst_h == 32 && pl->dst_dy == 64,
          "pillarbox the other way (dy %d)", pl->dst_dy);

    r = send(&st, &e, "a=p,i=1,p=8,C=1", NULL, 0);
    CHECK(last.cur_dx == 0 && last.cur_dy == 0 && pl_of(&st, 1, 8)->no_cursor,
          "C=1: no cursor movement");

    r = send(&st, &e, "a=p,i=1,p=9,x=10,y=30,w=100,h=5", NULL, 0);
    pl = pl_of(&st, 1, 9);
    CHECK(pl && pl->src_x == 10 && pl->src_y == 30 && pl->src_w == 10 &&
          pl->src_h == 5 && pl->cols == 2 && pl->rows == 1,
          "source rect clipped to the image");
    r = send(&st, &e, "a=p,i=1,p=10,x=20", NULL, 0);
    CHECK(!strncmp(r, "i=1,p=10;EINVAL:", 16) && !pl_of(&st, 1, 10),
          "source outside the image: '%s'", r);

    r = send(&st, &e, "a=p,i=1,p=11,z=-3", NULL, 0);
    CHECK(pl_of(&st, 1, 11)->z == -3, "z kept");

    /* same (i, p) replaces, p=0 multiplies */
    CHECK(st.n_pls == 10, "ten placements so far (%d)", st.n_pls);
    e.cur_x = 0;
    r = send(&st, &e, "a=p,i=1,p=1", NULL, 0);
    pl = pl_of(&st, 1, 1);
    CHECK(st.n_pls == 10 && pl && pl->col == 0, "same p replaces");
    send(&st, &e, "a=p,i=1", NULL, 0);
    send(&st, &e, "a=p,i=1", NULL, 0);
    CHECK(st.n_pls == 12, "p=0 twice: two placements");

    /* p is ignored for an image without id */
    r = send(&st, &e, "a=T,f=24,s=1,v=1,p=5", red, 3);
    CHECK(r[0] == '\0' && st.n_pls == 13 && st.pls[12].placement_id == 0 &&
          st.pls[12].image_id == 0, "anonymous a=T: placed, p dropped");

    /* no cell size */
    e.cell_w = 0;
    r = send(&st, &e, "a=p,i=1,p=20", NULL, 0);
    CHECK(!strncmp(r, "i=1,p=20;EINVAL:", 16), "no cell size: '%s'", r);
    e.cell_w = 8;

    /* a cell-size change refits placements given in cells */
    gfx_rescale(&st, 16, 32);
    pl = pl_of(&st, 1, 4);
    CHECK(pl->rows == 2 && pl->dst_h == 64 && pl->dst_w == 32 && pl->cols == 2,
          "rescale r=2 at 16x32");
    pl = pl_of(&st, 1, 2);
    CHECK(pl->dst_w == 20 && pl->cols == 2 && pl->rows == 2,
          "rescale native: ceil(24/16)=2, ceil(50/32)=2");
    gfx_store_free(&st);
}

/* ---- deletion ----------------------------------------------------------- */

static void test_delete(void)
{
    GfxStore st;
    GfxEnv e = env_default();
    const char *r;

    gfx_store_init(&st);
    put_red(&st, &e, 1, 8, 16);           /* one cell each */
    put_red(&st, &e, 2, 8, 16);
    put_red(&st, &e, 3, 8, 16);
    e.top_abs = 50;

    /* d=i / d=I */
    e.cur_x = 0; e.cur_y = 0;
    send(&st, &e, "a=p,i=1,p=1", NULL, 0);
    send(&st, &e, "a=p,i=1,p=2", NULL, 0);
    send(&st, &e, "a=p,i=2,p=1", NULL, 0);
    r = send(&st, &e, "a=d,d=i,i=1,p=1", NULL, 0);
    CHECK(r[0] == '\0' && st.n_pls == 2 && !pl_of(&st, 1, 1) &&
          pl_of(&st, 1, 2), "d=i with p: one placement, no reply");
    send(&st, &e, "a=d,d=i,i=1", NULL, 0);
    CHECK(st.n_pls == 1 && gfx_image_by_id(&st, 1), "d=i: placements, image kept");
    send(&st, &e, "a=d,d=I,i=1", NULL, 0);
    CHECK(!gfx_image_by_id(&st, 1), "d=I on an unplaced image frees it");
    send(&st, &e, "a=d,d=I,i=2", NULL, 0);
    CHECK(st.n_pls == 0 && !gfx_image_by_id(&st, 2), "d=I frees image 2");
    send(&st, &e, "a=d,d=i", NULL, 0);
    CHECK(st.n_imgs == 1, "d=i without i: nothing");

    /* d=a / d=A: not entirely above the screen */
    put_red(&st, &e, 1, 8, 16);
    e.cur_y = 0;  send(&st, &e, "a=p,i=1,p=1", NULL, 0);       /* abs 50 */
    e.cur_y = 23; send(&st, &e, "a=p,i=3,p=1", NULL, 0);       /* abs 73 */
    e.cur_y = 0;
    st.pls[0].abs_line = 40;                                    /* scrollback */
    send(&st, &e, "a=d", NULL, 0);
    CHECK(st.n_pls == 1 && st.pls[0].abs_line == 40, "a=d: scrollback one kept");
    e.cur_y = 1; send(&st, &e, "a=p,i=3,p=2", NULL, 0);
    send(&st, &e, "a=d,d=A", NULL, 0);
    CHECK(st.n_pls == 1 && !gfx_image_by_id(&st, 3) && gfx_image_by_id(&st, 1),
          "d=A frees the emptied image, keeps the scrollback one");
    send(&st, &e, "a=d,d=I,i=1", NULL, 0);
    CHECK(st.n_pls == 0 && !gfx_image_by_id(&st, 1), "I frees by id anywhere");

    /* d=n / d=N */
    send(&st, &e, "a=t,f=24,s=1,v=1,I=7", red, 3);              /* id 1 */
    send(&st, &e, "a=t,f=24,s=1,v=1,I=7", red, 3);              /* id 2 */
    send(&st, &e, "a=p,I=7,p=1", NULL, 0);
    send(&st, &e, "a=p,i=1,p=1", NULL, 0);
    send(&st, &e, "a=d,d=n,I=7", NULL, 0);
    CHECK(st.n_pls == 1 && pl_of(&st, 1, 1) && st.n_imgs == 2,
          "d=n: newest number's placements only, image kept");
    send(&st, &e, "a=d,d=N,I=7", NULL, 0);
    CHECK(st.n_imgs == 1 && gfx_image_by_id(&st, 1), "d=N frees the newest");
    send(&st, &e, "a=d,d=N,I=7,p=9", NULL, 0);
    CHECK(st.n_imgs == 1 && st.n_pls == 1, "d=N with a p that misses: nothing");
    send(&st, &e, "a=d,d=N,I=7,p=1", NULL, 0);
    CHECK(st.n_imgs == 0, "d=N with p frees when emptied");

    /* d=r / d=R */
    put_red(&st, &e, 1, 8, 16);
    put_red(&st, &e, 5, 8, 16);
    put_red(&st, &e, 9, 8, 16);
    send(&st, &e, "a=p,i=5,p=1", NULL, 0);
    send(&st, &e, "a=p,i=9,p=1", NULL, 0);
    send(&st, &e, "a=d,d=r,x=1,y=5", NULL, 0);
    CHECK(st.n_pls == 1 && st.n_imgs == 3, "d=r: placements of 1..5 gone");
    send(&st, &e, "a=d,d=R,x=1,y=5", NULL, 0);
    CHECK(st.n_imgs == 1 && gfx_image_by_id(&st, 9),
          "d=R frees unplaced images in the range");
    send(&st, &e, "a=d,d=R,x=9,y=9", NULL, 0);
    CHECK(st.n_imgs == 0 && st.n_pls == 0, "d=R frees placed ones too");

    /* position modes: x/y are 1-based cells */
    put_red(&st, &e, 1, 16, 32);          /* 2 x 2 cells */
    e.cur_x = 2; e.cur_y = 3;
    send(&st, &e, "a=p,i=1,p=1", NULL, 0);                      /* cols 2-3, rows 3-4 */
    e.cur_x = 10; e.cur_y = 10;
    send(&st, &e, "a=p,i=1,p=2,z=4", NULL, 0);                  /* cols 10-11, rows 10-11 */
    send(&st, &e, "a=d,d=p,x=3,y=4", NULL, 0);
    CHECK(st.n_pls == 1 && !pl_of(&st, 1, 1), "d=p hits the far corner cell");
    send(&st, &e, "a=p,i=1,p=1", NULL, 0);                      /* at 10,10 again */
    send(&st, &e, "a=d,d=p,x=13,y=11", NULL, 0);
    CHECK(st.n_pls == 2, "d=p one cell right misses");
    send(&st, &e, "a=d,d=q,x=11,y=11,z=4", NULL, 0);
    CHECK(st.n_pls == 1 && !pl_of(&st, 1, 2), "d=q: cell and z");
    send(&st, &e, "a=d,d=q,x=11,y=11,z=5", NULL, 0);
    CHECK(st.n_pls == 1, "d=q wrong z misses");
    e.cur_x = 11; e.cur_y = 11;
    send(&st, &e, "a=d,d=c", NULL, 0);
    CHECK(st.n_pls == 0 && gfx_image_by_id(&st, 1), "d=c at the cursor");
    e.cur_x = 2; e.cur_y = 3;
    send(&st, &e, "a=p,i=1,p=1", NULL, 0);
    send(&st, &e, "a=d,d=x,x=5", NULL, 0);
    CHECK(st.n_pls == 1, "d=x misses column 5");
    send(&st, &e, "a=d,d=x,x=3", NULL, 0);
    CHECK(st.n_pls == 0, "d=x column 3");
    send(&st, &e, "a=p,i=1,p=1", NULL, 0);
    send(&st, &e, "a=d,d=y,y=6", NULL, 0);
    CHECK(st.n_pls == 1, "d=y misses row 6");
    send(&st, &e, "a=d,d=Y,y=5", NULL, 0);
    CHECK(st.n_pls == 0 && !gfx_image_by_id(&st, 1), "d=Y row 5, frees");
    put_red(&st, &e, 1, 8, 16);
    send(&st, &e, "a=p,i=1,p=1,z=-1", NULL, 0);
    send(&st, &e, "a=p,i=1,p=2,z=2", NULL, 0);
    send(&st, &e, "a=d,d=z,z=-1", NULL, 0);
    CHECK(st.n_pls == 1 && pl_of(&st, 1, 2), "d=z");
    send(&st, &e, "a=d,d=Z,z=2", NULL, 0);
    CHECK(st.n_pls == 0 && st.n_imgs == 0, "d=Z frees");

    /* position modes only see the current screen */
    put_red(&st, &e, 1, 8, 16);
    e.screen = 1;
    e.cur_x = 0; e.cur_y = 0;
    send(&st, &e, "a=p,i=1,p=1", NULL, 0);
    e.screen = 0;
    send(&st, &e, "a=p,i=1,p=2", NULL, 0);
    send(&st, &e, "a=d,d=c", NULL, 0);
    CHECK(st.n_pls == 1 && st.pls[0].screen == 1, "d=c spares the other screen");
    send(&st, &e, "a=d,d=a", NULL, 0);
    CHECK(st.n_pls == 1, "d=a spares the other screen");
    send(&st, &e, "a=d,d=i,i=1", NULL, 0);
    CHECK(st.n_pls == 0, "d=i reaches both screens");

    /* anonymous images go with their last placement */
    send(&st, &e, "a=T,f=24,s=1,v=1", red, 3);
    CHECK(st.n_imgs == 2 && st.n_pls == 1, "anonymous placed");
    send(&st, &e, "a=d,d=c", NULL, 0);
    CHECK(st.n_imgs == 1 && st.n_pls == 0 && gfx_image_by_id(&st, 1),
          "anonymous freed with its placement, image 1 kept");
    gfx_store_free(&st);
}

/* ---- line movement ------------------------------------------------------ */

static void test_scroll(void)
{
    GfxStore st;
    GfxEnv e = env_default();
    const GfxPlacement *pl;

    gfx_store_init(&st);
    put_red(&st, &e, 1, 8, 48);           /* 1 x 3 cells */
    e.top_abs = 100;

    /* (a) whole-screen scroll into the scrollback: anchors stay */
    e.cur_y = 5;  send(&st, &e, "a=p,i=1,p=1", NULL, 0);        /* 105..107 */
    e.cur_y = 20; send(&st, &e, "a=p,i=1,p=2", NULL, 0);        /* 120..122 */
    e.top_abs = 110;                                            /* 10 lines scrolled */
    CHECK(pl_of(&st, 1, 1)->abs_line == 105, "anchor invariant");
    gfx_set_base(&st, 0, 106);
    CHECK(st.n_pls == 2, "base inside the image: kept");
    gfx_set_base(&st, 0, 108);
    CHECK(st.n_pls == 1 && !pl_of(&st, 1, 1), "base past the image: dropped");

    /* (b) top-anchored region with scrollback: rows 0..10, 2 lines */
    e.top_abs = 110;
    e.cur_y = 2;  send(&st, &e, "a=p,i=1,p=3", NULL, 0);        /* rows 2-4 inside */
    e.cur_y = 9;  send(&st, &e, "a=p,i=1,p=4", NULL, 0);        /* rows 9-11 straddles */
    e.cur_y = 15; send(&st, &e, "a=p,i=1,p=5", NULL, 0);        /* rows 15-17 below */
    e.cur_y = 0;  send(&st, &e, "a=p,i=1,p=6", NULL, 0);
    st.pls[st.n_pls - 1].abs_line = 100;                        /* scrollback */
    e.cur_y = 0;  send(&st, &e, "a=p,i=1,p=7", NULL, 0);
    st.pls[st.n_pls - 1].abs_line = 109;                        /* rows -1..1 */
    gfx_scroll_region_sb(&st, 0, 112, 10, 2);
    CHECK(pl_of(&st, 1, 3)->abs_line == 112, "inside the band: anchor kept");
    CHECK(pl_of(&st, 1, 6)->abs_line == 100, "in the scrollback: anchor kept");
    CHECK(pl_of(&st, 1, 7)->abs_line == 109, "straddling row 0: anchor kept");
    CHECK(pl_of(&st, 1, 4)->abs_line == 121, "straddler: keeps its row (+2)");
    CHECK(pl_of(&st, 1, 5)->abs_line == 127, "below the band: +2");
    CHECK(pl_of(&st, 1, 2)->abs_line == 122, "earlier screen-full one: +2");

    /* (c) region without scrollback: rows 5..15 */
    gfx_clear_all(&st, 0);
    e.top_abs = 200;
    e.cur_y = 4;  send(&st, &e, "a=p,i=1,p=1", NULL, 0);        /* 4-6 straddles top */
    e.cur_y = 8;  send(&st, &e, "a=p,i=1,p=2", NULL, 0);        /* 8-10 inside */
    e.cur_y = 13; send(&st, &e, "a=p,i=1,p=3", NULL, 0);        /* 13-15 inside, at bottom */
    e.cur_y = 16; send(&st, &e, "a=p,i=1,p=4", NULL, 0);        /* 16-18 below */
    e.cur_y = 5;  send(&st, &e, "a=p,i=1,p=5", NULL, 0);        /* 5-7 inside, at top */
    gfx_scroll_region(&st, 0, 200, 5, 15, 2, 8, 16, false);
    CHECK(pl_of(&st, 1, 1)->abs_line == 204, "straddler untouched");
    CHECK(pl_of(&st, 1, 4)->abs_line == 216, "below untouched");
    CHECK(pl_of(&st, 1, 2)->abs_line == 206, "inside: moved up 2");
    CHECK(pl_of(&st, 1, 3)->abs_line == 211, "inside at the bottom: moved up 2");
    pl = pl_of(&st, 1, 5);
    CHECK(pl && pl->abs_line == 205 && pl->rows == 1 && pl->clipped &&
          pl->src_y == 32 && pl->src_h == 16 && pl->dst_h == 16,
          "clipped at the top: last row remains (abs %ld rows %d src_y %d)",
          pl ? pl->abs_line : 0L, pl ? pl->rows : 0, pl ? pl->src_y : 0);
    gfx_scroll_region(&st, 0, 200, 5, 15, 1, 8, 16, false);
    CHECK(!pl_of(&st, 1, 5), "clipped out entirely: dropped");
    /* scroll down: the bottom one is clipped */
    gfx_scroll_region(&st, 0, 200, 5, 15, -3, 8, 16, false);
    pl = pl_of(&st, 1, 3);                /* was 210-212, now 213-215 */
    CHECK(pl && pl->abs_line == 213 && pl->rows == 3 && !pl->clipped,
          "down 3: fits");
    gfx_scroll_region(&st, 0, 200, 5, 15, -2, 8, 16, false);
    pl = pl_of(&st, 1, 3);                /* 215-217: rows 16,17 cut */
    CHECK(pl && pl->abs_line == 215 && pl->rows == 1 && pl->clipped &&
          pl->src_y == 0 && pl->src_h == 16 && pl->dst_h == 16,
          "clipped at the bottom: first row remains");
    pl = pl_of(&st, 1, 2);                /* 206 -> 205 -> 208 -> 210 */
    CHECK(pl && pl->abs_line == 210, "inside follows every scroll");

    /* clipping a letterboxed placement keeps the proportion */
    gfx_clear_all(&st, 0);
    e.cur_y = 5; send(&st, &e, "a=p,i=1,p=6,c=3,r=3", NULL, 0);  /* 8x48 in 24x48: dx 8 */
    gfx_scroll_region(&st, 0, 200, 5, 15, 1, 8, 16, false);
    pl = pl_of(&st, 1, 6);
    CHECK(pl && pl->rows == 2 && pl->cols == 3 && pl->dst_dx == 8 &&
          pl->src_y == 16 && pl->src_h == 32 && pl->dst_h == 32,
          "letterboxed clip: cols kept, a third of the source cut");

    /* ED 2 clears what is on screen, keeps the scrollback */
    gfx_clear_all(&st, 0);
    e.cur_y = 0; send(&st, &e, "a=p,i=1,p=1", NULL, 0);
    e.cur_y = 10; send(&st, &e, "a=p,i=1,p=2", NULL, 0);
    e.top_abs = 202;                       /* p=1 at 200-202: one row visible */
    gfx_clear_screen(&st, 0, 202);
    CHECK(st.n_pls == 0, "ED 2: a partly visible one goes too");
    e.top_abs = 200;
    e.cur_y = 0; send(&st, &e, "a=p,i=1,p=1", NULL, 0);
    gfx_clear_screen(&st, 0, 203);
    CHECK(st.n_pls == 1, "ED 2: entirely in the scrollback: kept");

    /* the alternate screen */
    e.screen = 1;
    e.top_abs = 0;
    e.cur_y = 1; send(&st, &e, "a=p,i=1,p=7", NULL, 0);
    CHECK(st.n_pls == 2, "alt placement");
    gfx_clear_all(&st, 1);
    CHECK(st.n_pls == 1 && st.pls[0].screen == 0, "alt cleared, main kept");

    /* reset */
    send(&st, &e, "a=t,f=24,s=1,v=1,i=5,m=1", red, 3);
    st.max_images = 7;
    gfx_reset(&st);
    CHECK(st.n_pls == 0 && st.n_imgs == 0 && !st.pend.active &&
          st.max_images == 7 && st.bytes == 0, "reset: empty, caps kept");
    e.screen = 0;
    put_red(&st, &e, 1, 8, 16);
    CHECK(st.n_imgs == 1, "usable after reset");
    gfx_store_free(&st);
}

/* ---- the visible list --------------------------------------------------- */

static void test_visible(void)
{
    GfxStore st;
    GfxEnv e = env_default();
    GfxVisible v[8];
    int n;

    gfx_store_init(&st);
    put_red(&st, &e, 1, 16, 32);          /* 2 x 2 cells */
    put_red(&st, &e, 2, 16, 32);
    e.top_abs = 100;
    e.cur_x = 3; e.cur_y = 2;
    send(&st, &e, "a=p,i=2,p=1,z=1", NULL, 0);
    send(&st, &e, "a=p,i=1,p=1,z=1", NULL, 0);
    send(&st, &e, "a=p,i=2,p=2,z=-1", NULL, 0);
    e.cur_y = 30; send(&st, &e, "a=p,i=1,p=2", NULL, 0);           /* off screen below */
    e.cur_y = 0; send(&st, &e, "a=p,i=1,p=3", NULL, 0);
    st.pls[st.n_pls - 1].abs_line = 97;                             /* 97-98: above */
    e.cur_y = 0; send(&st, &e, "a=p,i=1,p=4,X=3,Y=5", NULL, 0);
    st.pls[st.n_pls - 1].abs_line = 99;                             /* 99-100: half in */
    e.cur_x = 79; e.cur_y = 10; send(&st, &e, "a=p,i=1,p=5", NULL, 0); /* right edge */
    e.screen = 1; e.cur_x = 0; e.cur_y = 0;
    send(&st, &e, "a=p,i=1,p=6", NULL, 0);                          /* alt */

    n = gfx_visible(&st, 0, 100, 24, 80, 8, 16, v, 8);
    CHECK(n == 5, "five visible (%d)", n);
    CHECK(v[0].z == -1 && v[1].pl->placement_id == 4 &&
          v[1].pl->image_id == 1 && v[2].pl->placement_id == 5,
          "order: z, then image id");
    CHECK(v[3].pl->image_id == 1 && v[3].z == 1 && v[4].pl->image_id == 2 &&
          v[4].z == 1, "same z: lower id first");
    /* the half one: X=3/Y=5 widen it to 3 x 3 cells, rows 99-101; the cell
     * rect is clipped to rows 0-1, the dest rect is not */
    CHECK(v[1].r0 == 0 && v[1].r1 == 1 && v[1].c0 == 3 && v[1].c1 == 5 &&
          v[1].dy0 == -16 + 5 && v[1].dx0 == 24 + 3 && v[1].dy1 == -11 + 32 &&
          v[1].sw == 16 && v[1].sh == 32, "half visible: cells clipped, dest raw");
    /* the right-edge one: col 79, 2 wide */
    CHECK(v[2].c0 == 79 && v[2].c1 == 79 && v[2].dx0 == 632 && v[2].dx1 == 648,
          "right edge clipped to the last column");
    CHECK(v[3].img && v[3].img->id == 1 && v[3].dx0 == 24 && v[3].dy0 == 32 &&
          v[3].dx1 == 40 && v[3].dy1 == 64 && v[3].r0 == 2 && v[3].r1 == 3,
          "full one: dest rect and cells");
    n = gfx_visible(&st, 1, 100, 24, 80, 8, 16, v, 8);
    CHECK(n == 1 && v[0].pl->placement_id == 6, "alt screen list");
    n = gfx_visible(&st, 0, 100, 24, 80, 8, 16, v, 2);
    CHECK(n == 2, "max honoured");
    /* scrolled back view: top 97 shows the one above */
    n = gfx_visible(&st, 0, 97, 24, 80, 8, 16, v, 8);
    CHECK(n == 6, "scrolled back: six (%d)", n);
    gfx_store_free(&st);
}

/* ---- iTerm2 inline images (OSC 1337) ------------------------------------ */

static const unsigned char png_sig[8] =
    { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };

/* Stub PNG: the real 8-byte signature, w and h (16 bits each, little
 * endian), one RGBA colour for all pixels. */
static size_t mk_png(unsigned char *b, int w, int h)
{
    memcpy(b, png_sig, 8);
    b[8] = (unsigned char)w; b[9] = (unsigned char)(w >> 8);
    b[10] = (unsigned char)h; b[11] = (unsigned char)(h >> 8);
    b[12] = 0; b[13] = 0; b[14] = 255; b[15] = 255;
    return 16;
}

/* Stub JPEG: FF D8 FF, then w, h, a colour as above. */
static size_t mk_jpg(unsigned char *b, int w, int h)
{
    b[0] = 0xFF; b[1] = 0xD8; b[2] = 0xFF;
    b[3] = (unsigned char)w; b[4] = (unsigned char)(w >> 8);
    b[5] = (unsigned char)h; b[6] = (unsigned char)(h >> 8);
    b[7] = 255; b[8] = 0; b[9] = 0; b[10] = 255;
    return 11;
}

static bool stub_decode(const unsigned char *d, size_t len, size_t off,
                        unsigned char **px, int *w, int *h)
{
    size_t i, n;
    if (len < off + 8)
        return false;
    *w = d[off] | (d[off + 1] << 8);
    *h = d[off + 2] | (d[off + 3] << 8);
    if (*w <= 0 || *h <= 0)
        return false;
    n = (size_t)*w * *h;
    *px = malloc(n * 4);
    for (i = 0; i < n; i++)
        gfx_to_bgra(d + off + 4, 4, 1, *px + i * 4);
    return true;
}

static int n_png, n_jpg;

static bool stub_png(void *ctx, const unsigned char *d, size_t len,
                     unsigned char **px, int *w, int *h)
{
    (void)ctx;
    n_png++;
    if (len < 8 || memcmp(d, png_sig, 8))
        return false;
    return stub_decode(d, len, 8, px, w, h);
}

static int last_fmt;

/* JPEG decodes (the stub's own layout after the signature); GIF, TIFF and
 * BMP only count and fail, which names the format that was asked for */
static bool stub_jpg(void *ctx, int fmt, const unsigned char *d, size_t len,
                     unsigned char **px, int *w, int *h)
{
    (void)ctx;
    n_jpg++;
    last_fmt = fmt;
    if (fmt != GFX_FILE_JPEG ||
        len < 3 || d[0] != 0xFF || d[1] != 0xD8 || d[2] != 0xFF)
        return false;
    return stub_decode(d, len, 3, px, w, h);
}

static GfxEnv env_iterm(void)
{
    GfxEnv e = env_default();
    e.decode_png = stub_png;
    e.decode_file = stub_jpg;
    return e;
}

/* Serves one OSC 1337 string; returns the trace code ("" when none). */
static const char *iterm_s(GfxStore *st, const GfxEnv *env, const char *s,
                           bool cut, bool *handled)
{
    bool h;
    ntrace = 0;
    trace_code[0] = '\0';
    trace_a = 0;
    h = gfx_iterm(st, env, (const unsigned char *)s, strlen(s), cut, &last);
    if (handled)
        *handled = h;
    return trace_code;
}

/* "<prefix><args>:<base64 of data>" (no ':' part for data NULL). */
static const char *iterm_file(GfxStore *st, const GfxEnv *env,
                              const char *prefix, const char *args,
                              const unsigned char *data, size_t n, bool cut)
{
    static char buf[1 << 16];
    size_t len = (size_t)sprintf(buf, "%s%s", prefix, args);
    if (data) {
        buf[len++] = ':';
        len += b64enc(data, n, buf + len);
    }
    buf[len] = '\0';
    return iterm_s(st, env, buf, cut, NULL);
}

static const char *ifile(GfxStore *st, const GfxEnv *env, const char *args,
                         const unsigned char *data, size_t n)
{
    return iterm_file(st, env, "File=", args, data, n, false);
}

static int anon_images(const GfxStore *st)
{
    int i, n = 0;
    for (i = 0; i < st->n_imgs; i++)
        if (!st->imgs[i].id && !st->imgs[i].number)
            n++;
    return n;
}

static void test_iterm_args(void)
{
    GfxItermArgs a;
    GfxStore st;
    GfxEnv e = env_iterm();
    bool h = true;
    static const char *const bad[] = {
        "width=abc", "width=0", "width=10em", "width=-1", "height=5pxx",
        "width=", "inline=2", "inline=", "preserveAspectRatio=yes",
        "doNotMoveCursor=11", "size=12a", "size=", "width=10001",
        "width=100001px", "width=1001%", "height=%", "height=px",
        "inline=1;width=5;height=bogus",
    };
    size_t i;

#define ARGS(str) gfx_iterm_args((const unsigned char *)(str), strlen(str), &a)
    CHECK(ARGS("") && !a.inline_img && a.preserve && !a.no_move &&
          !a.have_size && !a.w_unit && !a.h_unit, "defaults");
    CHECK(ARGS("inline=1;width=10;height=5px;preserveAspectRatio=0;"
               "doNotMoveCursor=1;size=1234;name=Zm9vLnBuZw==") &&
          a.inline_img && !a.preserve && a.no_move && a.have_size &&
          a.size == 1234 && a.w_unit == 'c' && a.w == 10 &&
          a.h_unit == 'p' && a.h == 5, "every key");
    CHECK(ARGS("width=50%;height=auto") && a.w_unit == '%' && a.w == 50 &&
          a.h_unit == 0, "percent and auto");
    CHECK(ARGS("width=10000;height=100000px") && a.w == 10000 &&
          a.h == 100000, "the largest sizes");
    CHECK(ARGS("foo=bar;inline=1;junk;;x") && a.inline_img,
          "unknown keys, no '=', empty: ignored");
    CHECK(ARGS("inline=0;inline=1") && a.inline_img, "the last one wins");
    CHECK(ARGS("size=99999999999999999999999") && a.size == UINT64_MAX,
          "a huge size saturates");
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        CHECK(!ARGS(bad[i]), "malformed: %s", bad[i]);
#undef ARGS

#define IS(str) gfx_iterm_is_image((const unsigned char *)(str), strlen(str))
    CHECK(IS("File=inline=1:") && IS("MultipartFile=") && IS("FilePart=AA") &&
          IS("FileEnd"), "image commands");
    CHECK(!IS("SetMark") && !IS("FileEndX") && !IS("File") &&
          !IS("CurrentDir=/tmp") && !IS("file=inline=1:"), "others");
#undef IS

    gfx_store_init(&st);
    CHECK(!*iterm_s(&st, &e, "SetUserVar=foo=YmFy", false, &h) && !h &&
          ntrace == 0 && st.n_imgs == 0, "other 1337 commands: not handled");
    gfx_store_free(&st);
}

static void test_iterm_place(void)
{
    GfxStore st;
    GfxEnv e = env_iterm();
    unsigned char d[64];
    char wrapped[256];
    size_t n;
    const GfxPlacement *pl;

    gfx_store_init(&st);
    e.top_abs = 100;
    e.cur_x = 3;
    e.cur_y = 2;

    /* a 16 x 32 PNG: 2 x 2 cells at the cursor, anonymous, no reply */
    n = mk_png(d, 16, 32);
    CHECK(!strcmp(ifile(&st, &e, "inline=1", d, n), "OK") && trace_a == 'F' &&
          trace_id == 0 && ntrace == 1, "PNG placed (%s)", trace_code);
    pl = st.n_pls ? &st.pls[0] : NULL;
    CHECK(st.n_imgs == 1 && st.imgs[0].id == 0 && st.imgs[0].number == 0 &&
          st.imgs[0].w == 16 && st.imgs[0].h == 32, "anonymous image");
    CHECK(pl && pl->image_id == 0 && pl->placement_id == 0 &&
          pl->abs_line == 102 && pl->col == 3 && pl->cols == 2 &&
          pl->rows == 2 && pl->dst_w == 16 && pl->dst_h == 32 &&
          pl->z == 0 && pl->fixed && pl->screen == 0, "placement");
    CHECK(last.cur_dx == 2 && last.cur_dy == 1 && last.changed &&
          last.reply_len == 0, "cursor right 2, down 1; no reply");

    /* doNotMoveCursor */
    ifile(&st, &e, "inline=1;doNotMoveCursor=1", d, n);
    CHECK(!strcmp(trace_code, "OK") && last.cur_dx == 0 && last.cur_dy == 0 &&
          last.changed && st.n_pls == 2, "doNotMoveCursor: placed, stays");

    /* JPEG; without a JPEG decoder refused */
    n = mk_jpg(d, 8, 16);
    n_jpg = 0;
    CHECK(!strcmp(ifile(&st, &e, "inline=1", d, n), "OK") && n_jpg == 1 &&
          st.n_pls == 3 && last.cur_dx == 1 && last.cur_dy == 0, "JPEG placed");
    e.decode_file = NULL;
    CHECK(!strcmp(ifile(&st, &e, "inline=1", d, n), "EINVAL") &&
          st.n_pls == 3 && st.n_imgs == 3 && !last.changed &&
          last.cur_dx == 0, "JPEG without a decoder: refused");
    e.decode_file = stub_jpg;

    /* unknown formats, failed decodes */
    memcpy(d, "GIF89a\1\0\1\0\0\0", 12);
    n_png = n_jpg = 0;
    CHECK(!strcmp(ifile(&st, &e, "inline=1", d, 12), "EBADIMG") &&
          n_png == 0 && n_jpg == 1 && last_fmt == GFX_FILE_GIF &&
          st.n_imgs == 3, "GIF89a: to the file decoder as GIF");
    memcpy(d, "GIF87a\1\0\1\0\0\0", 12);
    CHECK(!strcmp(ifile(&st, &e, "inline=1", d, 12), "EBADIMG") &&
          last_fmt == GFX_FILE_GIF, "GIF87a: as GIF");
    memcpy(d, "II*\0\10\0\0\0\0\0\0\0", 12);
    CHECK(!strcmp(ifile(&st, &e, "inline=1", d, 12), "EBADIMG") &&
          last_fmt == GFX_FILE_TIFF, "TIFF little-endian: as TIFF");
    memcpy(d, "MM\0*\0\0\0\10\0\0\0\0", 12);
    CHECK(!strcmp(ifile(&st, &e, "inline=1", d, 12), "EBADIMG") &&
          last_fmt == GFX_FILE_TIFF, "TIFF big-endian: as TIFF");
    memcpy(d, "BM\0\0\0\0\0\0\0\0\0\0", 12);
    CHECK(!strcmp(ifile(&st, &e, "inline=1", d, 12), "EBADIMG") &&
          last_fmt == GFX_FILE_BMP, "BMP: as BMP");
    memcpy(d, "RIFF\0\0\0\0WEBP", 12);
    CHECK(!strcmp(ifile(&st, &e, "inline=1", d, 12), "EBADIMG") &&
          last_fmt == GFX_FILE_WEBP, "WebP: as WebP");
    memcpy(d, "\0\0\0\30ftypheic", 12);
    CHECK(!strcmp(ifile(&st, &e, "inline=1", d, 12), "EBADIMG") &&
          last_fmt == GFX_FILE_HEIF, "HEIC: as HEIF");
    memcpy(d, "\0\0\0\30ftypavif", 12);
    CHECK(!strcmp(ifile(&st, &e, "inline=1", d, 12), "EBADIMG") &&
          last_fmt == GFX_FILE_HEIF, "AVIF: as HEIF");
    memcpy(d, "\0\0\0\30ftypisom", 12);
    n_jpg = 0;
    CHECK(!strcmp(ifile(&st, &e, "inline=1", d, 12), "EINVAL") &&
          n_png == 0 && n_jpg == 0 && st.n_imgs == 3,
          "an MP4 (ftyp isom): refused, no decoder asked");
    memcpy(d, "RIFF\0\0\0\0WAVE", 12);
    CHECK(!strcmp(ifile(&st, &e, "inline=1", d, 12), "EINVAL") &&
          n_jpg == 0, "a RIFF that is not WebP: refused");
    CHECK(!strcmp(ifile(&st, &e, "inline=1", png_sig, 8), "EBADPNG") &&
          st.n_imgs == 3, "a PNG that does not decode");
    CHECK(!strcmp(ifile(&st, &e, "inline=1", d, 2), "EINVAL"),
          "two bytes: no format");

    /* inline=0 (or none): a download - nothing at all */
    n = mk_png(d, 8, 8);
    n_png = 0;
    CHECK(!strcmp(ifile(&st, &e, "inline=0", d, n), "ENOTINLINE") &&
          n_png == 0 && st.n_imgs == 3 && !last.changed && last.cur_dx == 0,
          "inline=0: nothing");
    CHECK(!strcmp(ifile(&st, &e, "name=YQ==;size=16", d, n), "ENOTINLINE") &&
          n_png == 0 && st.n_imgs == 3, "no inline: nothing");

    /* malformed arguments, no ':', bad base64, cut */
    CHECK(!strcmp(ifile(&st, &e, "inline=1;width=x", d, n), "EINVAL") &&
          st.n_imgs == 3, "malformed width: refused");
    CHECK(!strcmp(iterm_s(&st, &e, "File=inline=1", false, NULL), "EINVAL"),
          "no ':': refused");
    CHECK(!strcmp(iterm_s(&st, &e, "File=inline=1:iVBO@w==", false, NULL),
                  "EINVAL") && st.n_imgs == 3, "bad base64: refused");
    CHECK(!strcmp(iterm_s(&st, &e, "File=inline=1:iVBOR", false, NULL),
                  "EINVAL"), "a dangling base64 char: refused");
    n_png = 0;
    CHECK(!strcmp(iterm_file(&st, &e, "File=", "inline=1", d, n, true),
                  "EFBIG") && n_png == 0 && st.n_imgs == 3, "cut: refused");
    CHECK(!strcmp(iterm_s(&st, &e, "File=inline=1;wid", true, NULL), "EFBIG"),
          "cut in the arguments: refused");

    /* base64 wrapped by a script: white space skipped */
    {
        char b[64];
        b64enc(d, n, b);
        sprintf(wrapped, "File=inline=1:%.5s\r\n%.7s \t%s", b, b + 5, b + 12);
        CHECK(!strcmp(iterm_s(&st, &e, wrapped, false, NULL), "OK") &&
              st.n_imgs == 4, "wrapped base64: placed");
    }

    /* the size cap: size= over it refused at once, data over it too */
    st.pending_max = 15;
    n_png = 0;
    CHECK(!strcmp(ifile(&st, &e, "inline=1;size=16", d, n), "EFBIG") &&
          n_png == 0, "size= over the cap: refused");
    CHECK(!strcmp(ifile(&st, &e, "inline=1", d, n), "EFBIG") && n_png == 0,
          "data over the cap: refused");
    st.pending_max = 24;                    /* the decoder's slack */
    CHECK(!strcmp(ifile(&st, &e, "inline=1;size=16", d, n), "OK"),
          "under the cap: placed");
    st.pending_max = GFX_PENDING_MAX;

    /* the limits of kitty images */
    st.max_side = 7;
    CHECK(!strcmp(ifile(&st, &e, "inline=1", d, n), "EFBIG"), "max side");
    st.max_side = GFX_MAX_SIDE;
    e.cell_w = 0;
    CHECK(!strcmp(ifile(&st, &e, "inline=1", d, n), "EINVAL"),
          "cell size unknown");
    e.cell_w = 8;
    CHECK(st.n_imgs == 5 && st.n_pls == 5, "nothing left behind (%d %d)",
          st.n_imgs, st.n_pls);
    gfx_store_free(&st);
}

/* Draws a w x h image with args; returns its placement (the newest). */
static const GfxPlacement *geo(GfxStore *st, const GfxEnv *e, const char *args,
                               int w, int h)
{
    unsigned char d[16];
    size_t n = mk_png(d, w, h);
    char a[128];
    sprintf(a, "inline=1;%s", args);
    gfx_clear_all(st, 0);
    if (strcmp(ifile(st, e, a, d, n), "OK"))
        return NULL;
    return st->n_pls ? &st->pls[st->n_pls - 1] : NULL;
}

#define GEO(args, w, h, ew, eh, ec, er) do { \
    const GfxPlacement *p_ = geo(&st, &e, args, w, h); \
    CHECK(p_ && p_->dst_w == (ew) && p_->dst_h == (eh) && p_->cols == (ec) && \
          p_->rows == (er) && p_->dst_dx == 0 && p_->dst_dy == 0, \
          "%s %dx%d: got %dx%d %dx%d cells", args, w, h, \
          p_ ? p_->dst_w : -1, p_ ? p_->dst_h : -1, p_ ? p_->cols : -1, \
          p_ ? p_->rows : -1); } while (0)

static void test_iterm_geometry(void)
{
    GfxStore st;
    GfxEnv e = env_iterm();         /* 8 x 16 cells, 80 x 24: 640 x 384 */
    const GfxPlacement *pl;

    gfx_store_init(&st);
    GEO("", 100, 50, 100, 50, 13, 4);                       /* native */
    GEO("width=10", 100, 50, 80, 40, 10, 3);                /* cells */
    GEO("height=2", 100, 50, 64, 32, 8, 2);
    GEO("width=10;preserveAspectRatio=0", 100, 50, 80, 50, 10, 4);
    GEO("height=40px;preserveAspectRatio=0", 100, 50, 100, 40, 13, 3);
    GEO("width=200px", 100, 50, 200, 100, 25, 7);           /* pixels */
    GEO("width=10;height=10", 100, 50, 80, 40, 10, 3);      /* fit the box */
    GEO("width=40;height=2", 100, 50, 64, 32, 8, 2);
    GEO("width=10;height=10;preserveAspectRatio=0", 100, 50, 80, 160, 10, 10);
    GEO("width=50%", 100, 50, 320, 160, 40, 10);            /* percent */
    GEO("height=25%", 100, 50, 192, 96, 24, 6);
    GEO("width=auto;height=auto", 100, 50, 100, 50, 13, 4);
    /* an auto width wider than the screen comes down to it */
    GEO("", 1000, 100, 640, 64, 80, 4);
    GEO("height=200px", 1000, 100, 640, 64, 80, 4);
    GEO("height=200px;preserveAspectRatio=0", 1000, 100, 640, 200, 80, 13);
    /* an explicit width is kept, also past the screen */
    GEO("width=1000px", 1000, 100, 1000, 100, 125, 7);
    GEO("width=200%", 100, 50, 1280, 640, 160, 40);

    /* the cell size changes: the pixels stay, the cells follow */
    pl = geo(&st, &e, "width=10", 100, 50);
    gfx_rescale(&st, 16, 32);
    CHECK(pl && pl->dst_w == 80 && pl->dst_h == 40 && pl->cols == 5 &&
          pl->rows == 2, "rescale: fixed in pixels");
    gfx_store_free(&st);
}

static void test_iterm_multipart(void)
{
    GfxStore st;
    GfxEnv e = env_iterm();
    unsigned char d[16];
    char b[64], part[128];
    size_t n;

    gfx_store_init(&st);
    n = mk_png(d, 16, 16);
    b64enc(d, n, b);                        /* 24 chars */

    /* three parts, split off the 4-char groups */
    CHECK(!*iterm_s(&st, &e, "MultipartFile=inline=1;size=16", false, NULL) &&
          ntrace == 0 && st.iterm.data.active && !last.changed,
          "MultipartFile: opened, silent");
    sprintf(part, "FilePart=%.5s", b);
    CHECK(!*iterm_s(&st, &e, part, false, NULL) && ntrace == 0, "part 1");
    sprintf(part, "FilePart=%.11s", b + 5);
    iterm_s(&st, &e, part, false, NULL);
    sprintf(part, "FilePart=%s", b + 16);
    iterm_s(&st, &e, part, false, NULL);
    CHECK(st.n_imgs == 0, "nothing before FileEnd");
    CHECK(!strcmp(iterm_s(&st, &e, "FileEnd", false, NULL), "OK") &&
          trace_a == 'E' && st.n_pls == 1 && !st.iterm.data.active &&
          last.changed && last.cur_dx == 2 && last.cur_dy == 0,
          "FileEnd: placed");

    /* a kitty command in between does not end it */
    iterm_s(&st, &e, "MultipartFile=inline=1", false, NULL);
    sprintf(part, "FilePart=%.12s", b);
    iterm_s(&st, &e, part, false, NULL);
    send(&st, &e, "a=t,f=24,s=1,v=1,i=9", red, 3);
    sprintf(part, "FilePart=%s", b + 12);
    iterm_s(&st, &e, part, false, NULL);
    CHECK(!strcmp(iterm_s(&st, &e, "FileEnd", false, NULL), "OK") &&
          st.n_pls == 2, "kitty in between: placed");

    /* abandoned by a new File= and by a new MultipartFile= */
    iterm_s(&st, &e, "MultipartFile=inline=1", false, NULL);
    sprintf(part, "FilePart=%.12s", b);
    iterm_s(&st, &e, part, false, NULL);
    CHECK(!strcmp(ifile(&st, &e, "inline=1", d, n), "OK") && st.n_pls == 3 &&
          !st.iterm.data.active, "File= abandons the upload");
    sprintf(part, "FilePart=%s", b + 12);
    CHECK(!strcmp(iterm_s(&st, &e, part, false, NULL), "ENOENT") &&
          trace_a == 'P', "a part with no upload: ignored");
    CHECK(!strcmp(iterm_s(&st, &e, "FileEnd", false, NULL), "ENOENT") &&
          st.n_pls == 3, "an end with no upload: ignored");
    iterm_s(&st, &e, "MultipartFile=inline=1", false, NULL);
    sprintf(part, "FilePart=%.12s", b);
    iterm_s(&st, &e, part, false, NULL);
    iterm_s(&st, &e, "MultipartFile=inline=1", false, NULL);
    sprintf(part, "FilePart=%s", b);
    iterm_s(&st, &e, part, false, NULL);
    CHECK(!strcmp(iterm_s(&st, &e, "FileEnd", false, NULL), "OK") &&
          st.n_pls == 4, "a new MultipartFile= starts over");

    /* a cut part, a bad part, size= over the cap, inline=0 */
    iterm_s(&st, &e, "MultipartFile=inline=1", false, NULL);
    sprintf(part, "FilePart=%.12s", b);
    iterm_s(&st, &e, part, true, NULL);
    sprintf(part, "FilePart=%s", b + 12);
    iterm_s(&st, &e, part, false, NULL);
    CHECK(!strcmp(iterm_s(&st, &e, "FileEnd", false, NULL), "EFBIG") &&
          st.n_pls == 4, "a cut part: refused at the end");
    iterm_s(&st, &e, "MultipartFile=inline=1", false, NULL);
    iterm_s(&st, &e, "FilePart=*AAA", false, NULL);
    CHECK(!strcmp(iterm_s(&st, &e, "FileEnd", false, NULL), "EINVAL") &&
          st.n_pls == 4, "bad base64: refused at the end");
    sprintf(part, "MultipartFile=inline=1;size=%lu",
            (unsigned long)GFX_PENDING_MAX + 1);
    iterm_s(&st, &e, part, false, NULL);
    sprintf(part, "FilePart=%s", b);
    iterm_s(&st, &e, part, false, NULL);
    CHECK(!strcmp(iterm_s(&st, &e, "FileEnd", false, NULL), "EFBIG") &&
          st.n_pls == 4, "size= over the cap: refused");
    iterm_s(&st, &e, "MultipartFile=inline=1;width=?", false, NULL);
    iterm_s(&st, &e, part, false, NULL);
    CHECK(!strcmp(iterm_s(&st, &e, "FileEnd", false, NULL), "EINVAL") &&
          st.n_pls == 4, "malformed arguments: refused");
    n_png = 0;
    iterm_s(&st, &e, "MultipartFile=inline=0", false, NULL);
    iterm_s(&st, &e, part, false, NULL);
    CHECK(st.iterm.data.len == 0 && st.iterm.data.buf == NULL,
          "inline=0: parts not kept");
    CHECK(!strcmp(iterm_s(&st, &e, "FileEnd", false, NULL), "ENOTINLINE") &&
          st.n_pls == 4 && n_png == 0 && !last.changed, "inline=0: nothing");

    /* a reset ends an upload */
    iterm_s(&st, &e, "MultipartFile=inline=1", false, NULL);
    iterm_s(&st, &e, part, false, NULL);
    gfx_reset(&st);
    CHECK(!st.iterm.data.active && st.n_imgs == 0, "reset ends the upload");
    CHECK(!strcmp(iterm_s(&st, &e, "FileEnd", false, NULL), "ENOENT"),
          "nothing to end after a reset");
    gfx_store_free(&st);
}

static void test_iterm_anonymous(void)
{
    GfxStore st;
    GfxEnv e = env_iterm();
    unsigned char d[16];
    size_t n;
    int i;

    gfx_store_init(&st);
    n = mk_png(d, 8, 16);
    e.top_abs = 100;
    put_red(&st, &e, 1, 8, 16);             /* kitty id 1 at 0,0 */
    send(&st, &e, "a=p,i=1", NULL, 0);
    e.cur_x = 10; e.cur_y = 5;
    ifile(&st, &e, "inline=1", d, n);       /* iTerm2 at 10,5 */
    CHECK(st.n_imgs == 2 && anon_images(&st) == 1 && st.n_pls == 2,
          "one of each");
    CHECK(!gfx_image_by_id(&st, 0), "id 0 finds nothing");

    /* the by-id commands do not reach it */
    send(&st, &e, "a=d,d=I,i=1", NULL, 0);
    send(&st, &e, "a=d,d=R,x=0,y=4294967295", NULL, 0);
    send(&st, &e, "a=d,d=N,I=1", NULL, 0);
    CHECK(st.n_pls == 1 && anon_images(&st) == 1, "by id: untouched");
    CHECK(!strcmp(send(&st, &e, "a=p,i=0", NULL, 0), "") && st.n_pls == 1,
          "a=p,i=0: nothing");
    /* the kitty images get ids of their own, never 0 */
    send(&st, &e, "a=t,f=24,s=1,v=1,I=7", red, 3);
    CHECK(st.n_imgs == 2 && gfx_image_by_id(&st, 1) &&
          gfx_image_by_id(&st, 1)->number == 7, "I= next to it: id 1");

    /* by position: d=p at a cell it covers, d=c at the cursor, d=a */
    send(&st, &e, "a=d,d=p,x=11,y=6", NULL, 0);
    CHECK(st.n_pls == 0 && anon_images(&st) == 0, "d=p: placement and image");
    e.cur_x = 10; e.cur_y = 5;
    ifile(&st, &e, "inline=1", d, n);
    send(&st, &e, "a=d,d=c", NULL, 0);
    CHECK(anon_images(&st) == 0, "d=c: gone");
    ifile(&st, &e, "inline=1", d, n);
    send(&st, &e, "a=d", NULL, 0);
    CHECK(anon_images(&st) == 0, "d=a: gone");
    ifile(&st, &e, "inline=1", d, n);
    send(&st, &e, "a=d,d=z,z=0", NULL, 0);
    CHECK(anon_images(&st) == 0, "d=z: gone");

    /* line movement */
    ifile(&st, &e, "inline=1", d, n);       /* 105 */
    gfx_set_base(&st, 0, 106);
    CHECK(anon_images(&st) == 0, "scrolled off the scrollback: freed");
    ifile(&st, &e, "inline=1", d, n);
    gfx_clear_screen(&st, 0, 100);
    CHECK(anon_images(&st) == 0, "ED 2: freed");
    e.screen = 1; e.top_abs = 0;
    ifile(&st, &e, "inline=1", d, n);
    gfx_clear_all(&st, 1);
    CHECK(anon_images(&st) == 0, "alternate screen left: freed");
    e.cur_y = 23;
    ifile(&st, &e, "inline=1", d, n);
    gfx_scroll_region(&st, 1, 0, 0, 23, 1, 8, 16, true);
    gfx_scroll_region(&st, 1, 0, 0, 23, 1, 8, 16, true);
    CHECK(st.n_pls == 1 && st.pls[0].abs_line == 21, "region scroll moves it");
    gfx_scroll_region(&st, 1, 0, 0, 23, 30, 8, 16, true);
    CHECK(anon_images(&st) == 0, "scrolled out of the region: freed");
    e.screen = 0; e.top_abs = 100;
    ifile(&st, &e, "inline=1", d, n);
    gfx_reset(&st);
    CHECK(st.n_imgs == 0, "reset: freed");

    /* eviction: images in the scrollback only make room */
    st.max_images = 2;
    for (i = 0; i < 2; i++) {
        e.cur_y = i;
        ifile(&st, &e, "inline=1", d, n);
    }
    e.cur_y = 0;
    CHECK(!strcmp(ifile(&st, &e, "inline=1", d, n), "ENOSPC") &&
          st.n_imgs == 2, "full, both on the screen: refused");
    e.top_abs = 101;                        /* the one on row 0 scrolled out */
    CHECK(!strcmp(ifile(&st, &e, "inline=1", d, n), "OK") && st.n_imgs == 2 &&
          st.n_pls == 2 && st.pls[0].abs_line == 101, "the parked one went");
    e.top_abs = 200;
    send(&st, &e, "a=d,d=a", NULL, 0);      /* nothing on screen */
    CHECK(st.n_pls == 2, "the parked ones stay");
    gfx_reset(&st);
    /* a kitty image with an id is never taken */
    st.max_images = 2;
    e.top_abs = 100; e.cur_y = 0;
    put_red(&st, &e, 3, 8, 16);
    send(&st, &e, "a=p,i=3", NULL, 0);
    ifile(&st, &e, "inline=1", d, n);
    e.top_abs = 300;
    CHECK(!strcmp(send(&st, &e, "a=t,f=24,s=1,v=1,i=5", red, 3), "i=5;OK") &&
          gfx_image_by_id(&st, 3) && anon_images(&st) == 0,
          "the parked anonymous one went, id 3 stayed");
    gfx_reset(&st);
    /* the placement cap the same way */
    st.max_placements = 2;
    e.top_abs = 100;
    ifile(&st, &e, "inline=1", d, n);
    ifile(&st, &e, "inline=1", d, n);
    CHECK(!strcmp(ifile(&st, &e, "inline=1", d, n), "ENOSPC"),
          "placements full: refused");
    e.top_abs = 120;
    CHECK(!strcmp(ifile(&st, &e, "inline=1", d, n), "OK") && st.n_pls == 2 &&
          anon_images(&st) == 2, "placements full, parked: one went");
    gfx_store_free(&st);
}

/* A placement reaching below the screen's last row moves with the band. */
static void test_scroll_to_bottom(void)
{
    GfxStore st;
    GfxEnv e = env_default();
    const GfxPlacement *pl;

    gfx_store_init(&st);
    put_red(&st, &e, 1, 8, 80);             /* 1 x 5 cells */
    e.screen = 1;
    e.cur_y = 22;
    send(&st, &e, "a=p,i=1,p=1", NULL, 0);  /* rows 22..26 */
    gfx_scroll_region(&st, 1, 0, 0, 23, 1, 8, 16, false);
    pl = pl_of(&st, 1, 1);
    CHECK(pl && pl->abs_line == 22, "band only: stays (the old rule)");
    gfx_scroll_region(&st, 1, 0, 0, 23, 3, 8, 16, true);
    pl = pl_of(&st, 1, 1);
    CHECK(pl && pl->abs_line == 19 && pl->rows == 5 && !pl->clipped,
          "to the bottom: moved up 3, whole");
    gfx_scroll_region(&st, 1, 0, 0, 23, -2, 8, 16, true);
    pl = pl_of(&st, 1, 1);
    CHECK(pl && pl->abs_line == 21 && pl->rows == 3 && pl->clipped,
          "scrolled down: cut at the bottom (%ld %d)",
          pl ? pl->abs_line : 0L, pl ? pl->rows : 0);
    /* a band above the bottom keeps the old rule */
    send(&st, &e, "a=p,i=1,p=2", NULL, 0);  /* rows 22..26 */
    gfx_scroll_region(&st, 1, 0, 5, 22, 1, 8, 16, false);
    CHECK(pl_of(&st, 1, 2)->abs_line == 22, "straddling a band: stays");
    gfx_store_free(&st);
}

int main(void)
{
    test_parser();
    test_b64();
    test_transmit_raw();
    test_numbers();
    test_store_caps();
    test_chunks();
    test_png_zlib();
    test_refusals_and_quiet();
    test_placement();
    test_delete();
    test_scroll();
    test_visible();
    test_scroll_to_bottom();
    test_iterm_args();
    test_iterm_place();
    test_iterm_geometry();
    test_iterm_multipart();
    test_iterm_anonymous();
    if (failures) {
        printf("test_gfx: %d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("test_gfx: all %d checks passed\n", checks);
    return 0;
}
