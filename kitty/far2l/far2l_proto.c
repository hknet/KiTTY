/*
 * far2l_proto.c - the far2l terminal extensions without a window: the request
 * stack, client IDs, the clipboard read gate, chunked uploads, data IDs and the
 * format conversions (see far2l_proto.h). Written from the protocol
 * description; nothing here touches the Windows clipboard.
 */
#include <string.h>
#include <stdio.h>

#include "putty.h"
#include "far2l_proto.h"

/* ---------------------------------------------------------------- stack --- */

const unsigned char *f2l_pop_bytes(F2lStack *st, size_t n)
{
    if (!st || n > st->len)
        return NULL;
    st->len -= n;
    return st->data + st->len;
}

static bool f2l_pop_le(F2lStack *st, size_t n, uint64_t *v)
{
    const unsigned char *p = f2l_pop_bytes(st, n);
    uint64_t r = 0;
    size_t i;
    if (!p)
        return false;
    for (i = n; i-- > 0;)
        r = (r << 8) | p[i];
    *v = r;
    return true;
}

bool f2l_pop_u8(F2lStack *st, uint8_t *v)
{
    uint64_t r;
    if (!f2l_pop_le(st, 1, &r)) return false;
    *v = (uint8_t)r;
    return true;
}

bool f2l_pop_u16(F2lStack *st, uint16_t *v)
{
    uint64_t r;
    if (!f2l_pop_le(st, 2, &r)) return false;
    *v = (uint16_t)r;
    return true;
}

bool f2l_pop_u32(F2lStack *st, uint32_t *v)
{
    uint64_t r;
    if (!f2l_pop_le(st, 4, &r)) return false;
    *v = (uint32_t)r;
    return true;
}

bool f2l_pop_u64(F2lStack *st, uint64_t *v)
{
    return f2l_pop_le(st, 8, v);
}

bool f2l_pop_str(F2lStack *st, const unsigned char **p, size_t *n)
{
    F2lStack save = *st;
    uint32_t len;
    const unsigned char *b;
    if (!f2l_pop_u32(st, &len) || !(b = f2l_pop_bytes(st, len))) {
        *st = save;
        return false;
    }
    *p = b;
    *n = len;
    return true;
}

/* ---------------------------------------------------------------- reply --- */

void f2l_out_init(F2lOut *o)
{
    o->data = NULL;
    o->len = o->size = 0;
}

void f2l_out_free(F2lOut *o)
{
    if (o->data) {
        smemclr(o->data, o->size);
        sfree(o->data);
    }
    f2l_out_init(o);
}

void f2l_push_bytes(F2lOut *o, const void *p, size_t n)
{
    if (!n)
        return;
    if (o->len + n > o->size) {
        /* grown by hand rather than sresize, so the old block can be wiped:
         * a reply can carry the clipboard */
        size_t newsize = o->size ? o->size : 64;
        unsigned char *nd;
        while (newsize < o->len + n)
            newsize *= 2;
        nd = snewn(newsize, unsigned char);
        if (o->len)
            memcpy(nd, o->data, o->len);
        if (o->data) {
            smemclr(o->data, o->size);
            sfree(o->data);
        }
        o->data = nd;
        o->size = newsize;
    }
    memcpy(o->data + o->len, p, n);
    o->len += n;
}

static void f2l_push_le(F2lOut *o, uint64_t v, size_t n)
{
    unsigned char b[8];
    size_t i;
    for (i = 0; i < n; i++)
        b[i] = (unsigned char)(v >> (8 * i));
    f2l_push_bytes(o, b, n);
}

void f2l_push_u8(F2lOut *o, uint8_t v)   { f2l_push_le(o, v, 1); }
void f2l_push_u16(F2lOut *o, uint16_t v) { f2l_push_le(o, v, 2); }
void f2l_push_u32(F2lOut *o, uint32_t v) { f2l_push_le(o, v, 4); }
void f2l_push_u64(F2lOut *o, uint64_t v) { f2l_push_le(o, v, 8); }

/* ----------------------------------------------------------- client IDs --- */

bool f2l_client_id_valid(const unsigned char *id, size_t len)
{
    size_t i;
    if (!id || len < 32 || len > 256)
        return false;
    for (i = 0; i < len; i++) {
        unsigned char c = id[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
              c == '-' || c == '_'))
            return false;
    }
    return true;
}

bool f2l_client_ids_contains(const char *list, const unsigned char *id,
                             size_t len)
{
    const char *p = list;
    if (!list || !id || !len)
        return false;
    while (*p) {
        const char *e = strchr(p, ',');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n == len && !memcmp(p, id, len))
            return true;
        if (!e)
            break;
        p = e + 1;
    }
    return false;
}

int f2l_client_ids_count(const char *list)
{
    int n = 0;
    const char *p = list;
    if (!list || !*list)
        return 0;
    while (p) {
        const char *e = strchr(p, ',');
        if ((e ? (size_t)(e - p) : strlen(p)) > 0)
            n++;
        p = e ? e + 1 : NULL;
    }
    return n;
}

char *f2l_client_ids_add(const char *list, const unsigned char *id, size_t len)
{
    const char *p = list ? list : "";
    int drop;
    strbuf *sb;

    if (!f2l_client_id_valid(id, len) || f2l_client_ids_contains(p, id, len))
        return dupstr(p);
    /* make room: the oldest entries are at the front */
    drop = f2l_client_ids_count(p) - (F2L_CLIENT_IDS_MAX - 1);
    while (drop > 0 && *p) {
        const char *e = strchr(p, ',');
        p = e ? e + 1 : p + strlen(p);
        drop--;
    }
    sb = strbuf_new();
    /* re-emit what is kept without empty entries, so a hand-edited list with
     * stray commas comes back clean */
    while (*p) {
        const char *e = strchr(p, ',');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n) {
            if (sb->len)
                put_byte(sb, ',');
            put_data(sb, p, n);
        }
        if (!e)
            break;
        p = e + 1;
    }
    if (sb->len)
        put_byte(sb, ',');
    put_data(sb, id, len);
    return strbuf_to_str(sb);
}

/* ------------------------------------------------------------ read gate --- */

void f2l_gate_reset(F2lReadGate *g)
{
    g->since = 0;
    g->prolongs = 0;
    g->armed = false;
    g->gesture = 0;
}

void f2l_gate_paste(F2lReadGate *g, uint32_t now)
{
    g->since = now;
    g->prolongs = 0;
    g->armed = true;
}

void f2l_gate_sync(F2lReadGate *g, bool seen, unsigned count, uint32_t tick)
{
    if (seen && (!g->armed || g->gesture != count)) {
        f2l_gate_paste(g, tick);
        g->gesture = count;
    }
}

bool f2l_gate_open(const F2lReadGate *g, uint32_t now)
{
    /* unsigned subtraction: right across the wrap of the tick counter */
    return g->armed && (uint32_t)(now - g->since) < F2L_READ_WINDOW_MS;
}

void f2l_gate_served(F2lReadGate *g, uint32_t now)
{
    if (g->prolongs >= F2L_READ_PROLONGS)
        return;
    g->prolongs++;
    g->since = now;
}

/* --------------------------------------------------------------- chunks --- */

static void f2l_chunks_wipe(F2lChunks *c)
{
    if (c->data) {
        smemclr(c->data, c->size);
        sfree(c->data);
    }
    c->data = NULL;
    c->len = c->size = 0;
}

void f2l_chunks_clear(F2lChunks *c)
{
    f2l_chunks_wipe(c);
    c->overflow = false;
}

int f2l_chunks_add(F2lChunks *c, F2lStack *st, size_t cap)
{
    uint16_t enc;
    size_t n;
    const unsigned char *p;

    if (!f2l_pop_u16(st, &enc)) {
        f2l_chunks_wipe(c);
        c->overflow = true;            /* data lost: the set must not succeed */
        return F2L_CHUNK_SHORT;
    }
    if (enc == 0) {
        f2l_chunks_clear(c);
        return F2L_CHUNK_DISCARDED;
    }
    if (c->overflow)
        return F2L_CHUNK_TOOBIG;       /* already lost: collect nothing more */
    n = (size_t)enc << 8;
    if (!(p = f2l_pop_bytes(st, n))) {
        f2l_chunks_wipe(c);
        c->overflow = true;
        return F2L_CHUNK_SHORT;
    }
    if (n > cap || c->len > cap - n) {
        f2l_chunks_wipe(c);
        c->overflow = true;
        return F2L_CHUNK_TOOBIG;
    }
    if (c->len + n > c->size) {
        size_t newsize = c->size ? c->size : 0x10000;
        unsigned char *nd;
        while (newsize < c->len + n)
            newsize *= 2;
        nd = snewn(newsize, unsigned char);
        if (c->len)
            memcpy(nd, c->data, c->len);
        if (c->data) {
            smemclr(c->data, c->size);
            sfree(c->data);
        }
        c->data = nd;
        c->size = newsize;
    }
    memcpy(c->data + c->len, p, n);
    c->len += n;
    return F2L_CHUNK_ADDED;
}

/* -------------------------------------------------------------- data ID --- */

uint64_t f2l_data_id(uint32_t fmt, const unsigned char *data, size_t len)
{
    /* FNV-1a, 64 bits, over the length and then the bytes. Any stable hash
     * does: the client only ever compares IDs we gave it. */
    uint64_t h = 0xcbf29ce484222325ULL;
    size_t i;

    if (!data)
        len = 0;
    if (fmt == F2L_CF_TEXT || fmt == F2L_CF_HTML) {
        size_t n = 0;
        while (n < len && data[n])
            n++;
        len = n;
    } else if (fmt == F2L_CF_UNICODETEXT) {
        size_t n = 0;
        while (n + 4 <= len && (data[n] | data[n + 1] | data[n + 2] | data[n + 3]))
            n += 4;
        len = n;
    }
    for (i = 0; i < 8; i++) {
        h ^= (unsigned char)((uint64_t)len >> (8 * i));
        h *= 0x100000001b3ULL;
    }
    for (i = 0; i < len; i++) {
        h ^= data[i];
        h *= 0x100000001b3ULL;
    }
    return h ? h : 1;
}

/* ---------------------------------------------------------- conversions --- */

uint16_t *f2l_utf32_to_utf16(const unsigned char *p, size_t len, size_t *units)
{
    size_t n = len / 4, i, o = 0;
    uint16_t *out = snewn(2 * n + 1, uint16_t);
    for (i = 0; i < n; i++) {
        uint32_t c = (uint32_t)p[4 * i] | ((uint32_t)p[4 * i + 1] << 8) |
                     ((uint32_t)p[4 * i + 2] << 16) | ((uint32_t)p[4 * i + 3] << 24);
        if (c == 0)
            break;
        if (c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF))
            c = 0xFFFD;
        if (c >= 0x10000) {
            c -= 0x10000;
            out[o++] = (uint16_t)(0xD800 | (c >> 10));
            out[o++] = (uint16_t)(0xDC00 | (c & 0x3FF));
        } else {
            out[o++] = (uint16_t)c;
        }
    }
    out[o] = 0;
    if (units)
        *units = o;
    return out;
}

unsigned char *f2l_utf16_to_utf32(const uint16_t *s, size_t n, size_t *len)
{
    unsigned char *out = snewn(4 * n + 4, unsigned char);
    size_t i, o = 0;
    for (i = 0; i < n && s[i]; i++) {
        uint32_t c = s[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n &&
            s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            i++;
        }
        out[o++] = (unsigned char)c;
        out[o++] = (unsigned char)(c >> 8);
        out[o++] = (unsigned char)(c >> 16);
        out[o++] = (unsigned char)(c >> 24);
    }
    memset(out + o, 0, 4);
    o += 4;
    if (len)
        *len = o;
    return out;
}

/* The Windows "HTML Format" header: fixed width, so the offsets it holds can
 * be computed before it is written. */
#define F2L_HTML_HDR "Version:0.9\r\nStartHTML:%010u\r\nEndHTML:%010u\r\n" \
                     "StartFragment:%010u\r\nEndFragment:%010u\r\n"
#define F2L_HTML_HDR_LEN 105
#define F2L_HTML_PRE  "<html><body>\r\n<!--StartFragment-->"
#define F2L_HTML_POST "<!--EndFragment-->\r\n</body></html>"

unsigned char *f2l_html_wrap(const unsigned char *html, size_t len,
                             size_t *outlen)
{
    size_t n = 0, pre = strlen(F2L_HTML_PRE), post = strlen(F2L_HTML_POST);
    size_t sfrag, efrag, ehtml, total;
    unsigned char *out;
    char hdr[F2L_HTML_HDR_LEN + 1];

    while (n < len && html[n])
        n++;                           /* far2l sends it NUL-terminated */
    sfrag = F2L_HTML_HDR_LEN + pre;
    efrag = sfrag + n;
    ehtml = efrag + post;
    total = ehtml + 1;
    if (ehtml > 0xFFFFFFFFu)
        return NULL;
    snprintf(hdr, sizeof(hdr), F2L_HTML_HDR, (unsigned)F2L_HTML_HDR_LEN,
             (unsigned)ehtml, (unsigned)sfrag, (unsigned)efrag);
    out = snewn(total, unsigned char);
    memcpy(out, hdr, F2L_HTML_HDR_LEN);
    memcpy(out + F2L_HTML_HDR_LEN, F2L_HTML_PRE, pre);
    if (n)
        memcpy(out + sfrag, html, n);
    memcpy(out + efrag, F2L_HTML_POST, post);
    out[ehtml] = '\0';
    if (outlen)
        *outlen = total;
    return out;
}

/* The decimal value after `key` in the header part of `cf`, or -1. */
static long f2l_html_field(const unsigned char *cf, size_t len, const char *key)
{
    size_t klen = strlen(key), i, hdr = len < 1024 ? len : 1024;
    for (i = 0; i + klen < hdr; i++) {
        if (cf[i] == '<')
            break;                     /* the header is over */
        if (!memcmp(cf + i, key, klen)) {
            long v = 0;
            size_t j = i + klen;
            if (j >= hdr || cf[j] < '0' || cf[j] > '9')
                return -1;
            while (j < hdr && cf[j] >= '0' && cf[j] <= '9' && v < 0x7FFFFFFF / 10)
                v = v * 10 + (cf[j++] - '0');
            return v;
        }
    }
    return -1;
}

unsigned char *f2l_html_unwrap(const unsigned char *cf, size_t len,
                               size_t *outlen)
{
    long s = f2l_html_field(cf, len, "StartFragment:");
    long e = f2l_html_field(cf, len, "EndFragment:");
    unsigned char *out;
    size_t n;

    if (s < 0 || e < s) {
        /* no usable fragment: the whole document */
        s = f2l_html_field(cf, len, "StartHTML:");
        e = f2l_html_field(cf, len, "EndHTML:");
    }
    if (s < 0 || e < s || (size_t)e > len)
        return NULL;
    n = (size_t)(e - s);
    out = snewn(n + 1, unsigned char);
    if (n)
        memcpy(out, cf + s, n);
    out[n] = '\0';
    if (outlen)
        *outlen = n + 1;
    return out;
}
