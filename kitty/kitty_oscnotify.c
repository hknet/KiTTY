/*
 * kitty_oscnotify.c - desktop notifications from the host, the platform-free
 * half: OSC 9 / 777 / 99 parsing, limits, cleaning, the decisions and the
 * replies. See kitty_oscnotify.h; the Windows half is kitty_hostnotify.c.
 *
 * Everything a remote host sends here is hostile until cleaned: ids are
 * sanitised before they are ever echoed back (a reply is input to the host's
 * terminal program, so an id carrying ESC would be an injection), text is cut
 * to the limits, invalid UTF-8 and control characters never reach the screen.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kitty_oscnotify.h"

/* ---- small helpers ----------------------------------------------------- */

static int on_eq(const char *s, size_t len, const char *lit)
{
    size_t n = strlen(lit);
    return len == n && memcmp(s, lit, n) == 0;
}

/* A decimal number, possibly negative; 0 and *ok = 0 when it is not one.
 * Saturates rather than overflowing. */
static long on_number(const char *s, size_t len, int *ok)
{
    long v = 0;
    int neg = 0;
    size_t i = 0;
    *ok = 0;
    if (len && s[0] == '-') { neg = 1; i = 1; }
    if (i >= len)
        return 0;
    for (; i < len; i++) {
        if (s[i] < '0' || s[i] > '9')
            return 0;
        if (v < 100000000L)
            v = v * 10 + (s[i] - '0');
    }
    *ok = 1;
    return neg ? -v : v;
}

static void on_meta_default(OnMeta *m)
{
    memset(m, 0, sizeof(*m));
    m->done = 1;
    m->payload = ON_P_TITLE;
    m->occasion = ON_OCC_ALWAYS;
    m->urgency = -1;
    m->expire = -1;
    m->focus = 1;
    /* The specification: no identifier means i=0. */
    m->id[0] = '0';
    m->id[1] = '\0';
}

void on_id_sanitise(const char *in, size_t len, char *out)
{
    size_t i, o = 0;
    for (i = 0; i < len && o < ON_ID_MAX; i++) {
        char c = in[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '+' ||
            c == '.')
            out[o++] = c;
    }
    out[o] = '\0';
}

/* ---- text -------------------------------------------------------------- */

size_t on_text_clean(const char *in, size_t len, char *out, size_t max)
{
    const unsigned char *p = (const unsigned char *)in;
    size_t i = 0, o = 0;

    while (i < len) {
        unsigned char c = p[i];
        unsigned long cp;
        size_t n, k;

        if (c < 0x80) {
            n = 1; cp = c;
        } else if (c >= 0xC2 && c <= 0xDF) {
            n = 2; cp = c & 0x1F;
        } else if (c >= 0xE0 && c <= 0xEF) {
            n = 3; cp = c & 0x0F;
        } else if (c >= 0xF0 && c <= 0xF4) {
            n = 4; cp = c & 0x07;
        } else {
            i++;                         /* not a lead byte: dropped */
            continue;
        }
        if (i + n > len) {
            i++;                         /* cut short: dropped */
            continue;
        }
        for (k = 1; k < n; k++) {
            if ((p[i + k] & 0xC0) != 0x80)
                break;
            cp = (cp << 6) | (p[i + k] & 0x3F);
        }
        if (k < n ||
            (n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000) ||
            cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            i++;                         /* overlong, surrogate, bad: dropped */
            continue;
        }

        if (cp == '\t' || cp == '\n' || cp == '\r') {
            if (o + 1 > max)
                break;
            out[o++] = ' ';
        } else if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp <= 0x9F)) {
            /* C0 / DEL / C1: stripped */
        } else {
            if (o + n > max)
                break;                   /* cut on a character boundary */
            memcpy(out + o, p + i, n);
            o += n;
        }
        i += n;
    }
    out[o] = '\0';
    return o;
}

long on_base64_decode(const char *in, size_t len, unsigned char *out)
{
    unsigned long acc = 0;
    int bits = 0, pad = 0;
    long o = 0;
    size_t i;

    for (i = 0; i < len; i++) {
        char c = in[i];
        int v;
        if (c == '=') {
            pad++;
            continue;
        }
        if (pad)
            return -1;                   /* data after padding */
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else return -1;
        acc = ((acc << 6) | (unsigned long)v) & 0xFFFFFF;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[o++] = (unsigned char)((acc >> bits) & 0xFF);
        }
    }
    if (pad > 2 || bits >= 6)
        return -1;                       /* a lone sextet is not a byte */
    return o;
}

/* Append bytes to a capped buffer; what does not fit is cut. */
static void on_append(char *buf, size_t *blen, size_t max,
                      const char *in, size_t len)
{
    size_t room = max - *blen;
    if (len > room)
        len = room;
    memcpy(buf + *blen, in, len);
    *blen += len;
    buf[*blen] = '\0';
}

void on_notice_set(OnNotice *n, const char *title, size_t tlen,
                   const char *body, size_t blen)
{
    memset(n, 0, sizeof(*n));
    on_meta_default(&n->meta);
    /* Not an OSC 99 notice: no id, so p=alive never lists it and it sends
     * no reports. */
    n->meta.id[0] = '\0';
    n->active = 1;
    if (title)
        on_append(n->title, &n->title_len, ON_TITLE_MAX, title, tlen);
    if (body)
        on_append(n->body, &n->body_len, ON_BODY_MAX, body, blen);
}

/* ---- OSC 99 ------------------------------------------------------------ */

static void on_parse_meta(const char *s, size_t len, OnMeta *m)
{
    size_t i = 0;
    on_meta_default(m);
    while (i < len) {
        size_t j = i, eq;
        const char *k, *v;
        size_t kl, vl;
        while (j < len && s[j] != ':')
            j++;
        for (eq = i; eq < j && s[eq] != '='; eq++)
            ;
        k = s + i; kl = eq - i;
        v = eq < j ? s + eq + 1 : s + j;
        vl = eq < j ? j - eq - 1 : 0;
        if (kl == 1) {
            int ok;
            long n;
            switch (k[0]) {
              case 'i':
                on_id_sanitise(v, vl, m->id);
                if (!m->id[0])
                    strcpy(m->id, "0");      /* nothing left: no identifier */
                break;
              case 'd':
                m->done = !on_eq(v, vl, "0");
                break;
              case 'e':
                m->b64 = on_eq(v, vl, "1");
                break;
              case 'p':
                m->payload = on_eq(v, vl, "title") ? ON_P_TITLE :
                             on_eq(v, vl, "body")  ? ON_P_BODY :
                             on_eq(v, vl, "close") ? ON_P_CLOSE :
                             on_eq(v, vl, "alive") ? ON_P_ALIVE :
                             on_eq(v, vl, "?")     ? ON_P_QUERY : ON_P_OTHER;
                break;
              case 'o':
                if (on_eq(v, vl, "always"))
                    m->occasion = ON_OCC_ALWAYS, m->has_o = 1;
                else if (on_eq(v, vl, "unfocused"))
                    m->occasion = ON_OCC_UNFOCUSED, m->has_o = 1;
                else if (on_eq(v, vl, "invisible"))
                    m->occasion = ON_OCC_INVISIBLE, m->has_o = 1;
                break;
              case 'u':
                n = on_number(v, vl, &ok);
                if (ok && n >= 0 && n <= 2)
                    m->urgency = (int)n, m->has_u = 1;
                break;
              case 'w':
                n = on_number(v, vl, &ok);
                if (ok && n >= -1)
                    m->expire = n, m->has_w = 1;
                break;
              case 'c':
                m->close_report = on_eq(v, vl, "1");
                m->has_c = 1;
                break;
              case 'a': {
                size_t a = 0;
                m->has_a = 1;
                while (a < vl) {
                    size_t b = a;
                    int neg = 0;
                    while (b < vl && v[b] != ',')
                        b++;
                    if (a < b && v[a] == '-') { neg = 1; a++; }
                    if (on_eq(v + a, b - a, "focus"))
                        m->focus = !neg;
                    else if (on_eq(v + a, b - a, "report"))
                        m->report = !neg;
                    a = b + 1;
                }
                break;
              }
              default:
                break;                   /* f, g, n, s, t ...: dropped */
            }
        }
        i = j + 1;
    }
}

/* Keys a later chunk carries override the ones before. */
static void on_merge_meta(OnMeta *to, const OnMeta *from)
{
    if (from->has_o) to->occasion = from->occasion, to->has_o = 1;
    if (from->has_u) to->urgency = from->urgency, to->has_u = 1;
    if (from->has_w) to->expire = from->expire, to->has_w = 1;
    if (from->has_a) {
        to->focus = from->focus;
        to->report = from->report;
        to->has_a = 1;
    }
    if (from->has_c) to->close_report = from->close_report, to->has_c = 1;
}

int on_osc99(OnNotice *asm_, const char *s, size_t len, OnNotice *out)
{
    const char *semi = memchr(s, ';', len);
    size_t mlen = semi ? (size_t)(semi - s) : len;
    const char *pay = semi ? semi + 1 : s + len;
    size_t plen = semi ? len - mlen - 1 : 0;
    OnMeta m;

    on_parse_meta(s, mlen, &m);
    memset(out, 0, sizeof(*out));
    out->meta = m;

    switch (m.payload) {
      case ON_P_CLOSE: return ON99_CLOSE;
      case ON_P_ALIVE: return ON99_ALIVE;
      case ON_P_QUERY: return ON99_QUERY;
      default: break;
    }

    /* A chunk of another notice: the unfinished one is dropped. */
    if (asm_->active && strcmp(asm_->meta.id, m.id) != 0)
        asm_->active = 0;
    if (!asm_->active) {
        memset(asm_, 0, sizeof(*asm_));
        asm_->active = 1;
        on_meta_default(&asm_->meta);
        memcpy(asm_->meta.id, m.id, sizeof(m.id));
    }
    on_merge_meta(&asm_->meta, &m);

    if (m.payload == ON_P_TITLE || m.payload == ON_P_BODY) {
        const char *data = pay;
        size_t dlen = plen;
        unsigned char *dec = NULL;
        if (m.b64) {
            long n;
            dec = malloc(plen * 3 / 4 + 3);
            if (!dec) {
                asm_->active = 0;
                return ON99_NOTHING;
            }
            n = on_base64_decode(pay, plen, dec);
            if (n < 0) {
                free(dec);
                asm_->active = 0;        /* a bad chunk spoils the notice */
                return ON99_NOTHING;
            }
            data = (const char *)dec;
            dlen = (size_t)n;
        }
        if (m.payload == ON_P_TITLE)
            on_append(asm_->title, &asm_->title_len, ON_TITLE_MAX, data, dlen);
        else
            on_append(asm_->body, &asm_->body_len, ON_BODY_MAX, data, dlen);
        free(dec);
    }

    if (!m.done)
        return ON99_NOTHING;
    *out = *asm_;
    out->meta.done = 1;
    asm_->active = 0;
    return ON99_SHOW;
}

/* ---- OSC 9 and OSC 777 ------------------------------------------------- */

int on_osc9(const char *s, size_t len, int *state, int *value)
{
    size_t i = 0;
    long cmd = 0;
    while (i < len && s[i] >= '0' && s[i] <= '9') {
        if (cmd < 100000)
            cmd = cmd * 10 + (s[i] - '0');
        i++;
    }
    if (i == 0 || (i < len && s[i] != ';'))
        return ON9_NOTICE;               /* text, not a ConEmu command */
    if (cmd != 4)
        return ON9_IGNORE;

    /* 4;<state>;<progress> */
    {
        const char *f = s + i + 1;
        size_t flen = i < len ? len - i - 1 : 0;
        const char *semi;
        size_t slen;
        long st, pr;
        int ok;
        if (i >= len)
            return ON9_IGNORE;
        semi = memchr(f, ';', flen);
        slen = semi ? (size_t)(semi - f) : flen;
        st = on_number(f, slen, &ok);
        if (!ok || st < 0 || st > 4)
            return ON9_IGNORE;
        *state = (int)st;
        *value = -1;
        if (semi) {
            const char *v = semi + 1;
            size_t vlen = flen - slen - 1;
            const char *end = memchr(v, ';', vlen);
            if (end)
                vlen = (size_t)(end - v);
            pr = on_number(v, vlen, &ok);
            if (ok) {
                if (pr < 0) pr = 0;
                if (pr > 100) pr = 100;
                *value = (int)pr;
            }
        }
        return ON9_PROGRESS;
    }
}

int on_osc777(const char *s, size_t len, OnNotice *out)
{
    const char *a = memchr(s, ';', len);
    const char *t, *b;
    size_t tlen;
    if (!a || !on_eq(s, (size_t)(a - s), "notify"))
        return 0;
    t = a + 1;
    b = memchr(t, ';', len - (size_t)(t - s));
    if (b) {
        tlen = (size_t)(b - t);
        b++;
        on_notice_set(out, t, tlen, b, len - (size_t)(b - s));
    } else {
        on_notice_set(out, t, len - (size_t)(t - s), NULL, 0);
    }
    return 1;
}

/* ---- decisions --------------------------------------------------------- */

int on_should_show(int setting, int occasion, int focused, int visible)
{
    if (setting == ON_SET_OFF)
        return 0;
    if (setting == ON_SET_UNFOCUSED && focused)
        return 0;
    if (setting != ON_SET_UNFOCUSED && setting != ON_SET_ALWAYS)
        return 0;                        /* an unknown value: off */
    if (occasion == ON_OCC_UNFOCUSED && focused)
        return 0;
    if (occasion == ON_OCC_INVISIBLE && (focused || visible))
        return 0;
    return 1;
}

int on_seconds(int urgency, long expire_ms, int *critical)
{
    *critical = (urgency == 2);
    if (urgency == 2 || expire_ms == 0)
        return ON_STICKY_SECONDS;
    if (expire_ms > 0) {
        long s = (expire_ms + 999) / 1000;
        return s > ON_STICKY_SECONDS ? ON_STICKY_SECONDS : (int)s;
    }
    return urgency == 0 ? ON_SHORT_SECONDS : ON_DEFAULT_SECONDS;
}

/* ---- replies ----------------------------------------------------------- */

/* The id a reply names: "0" when there is none, as the specification
 * requires of a terminal that got no identifier. */
static const char *on_reply_id(const char *id)
{
    return id && *id ? id : "0";
}

size_t on_reply_query(const char *id, char *buf)
{
    int n = snprintf(buf, ON_REPLY_MAX, "\033]99;i=%s:p=?;" ON_QUERY_CAPS "\033\\",
                     on_reply_id(id));
    return n > 0 ? (size_t)n : 0;
}

size_t on_reply_alive(const char *id, const char *alive_ids, char *buf)
{
    int n = snprintf(buf, ON_REPLY_MAX, "\033]99;i=%s:p=alive;%s\033\\",
                     on_reply_id(id), alive_ids ? alive_ids : "");
    return n > 0 ? (size_t)n : 0;
}

size_t on_reply_activated(const char *id, char *buf)
{
    int n = snprintf(buf, ON_REPLY_MAX, "\033]99;i=%s;\033\\", on_reply_id(id));
    return n > 0 ? (size_t)n : 0;
}

size_t on_reply_closed(const char *id, char *buf)
{
    int n = snprintf(buf, ON_REPLY_MAX, "\033]99;i=%s:p=close;\033\\",
                     on_reply_id(id));
    return n > 0 ? (size_t)n : 0;
}

/* ---- flood rule -------------------------------------------------------- */

int on_flood_offer(OnFlood *f, unsigned long now)
{
    if (f->pending) {
        f->dropped++;                    /* the newer one replaces it */
        return ON_FLOOD_PEND;
    }
    if (!f->have_last || now - f->last >= ON_FLOOD_MS) {
        f->last = now;
        f->have_last = 1;
        return ON_FLOOD_NOW;
    }
    f->pending = 1;
    return ON_FLOOD_PEND;
}

unsigned long on_flood_due(const OnFlood *f)
{
    return f->last + ON_FLOOD_MS;
}

int on_flood_tick(OnFlood *f, unsigned long now, int *dropped)
{
    *dropped = 0;
    if (f->pending) {
        f->pending = 0;
        f->last = now;
        f->have_last = 1;
        return ON_FLOOD_NOW;
    }
    f->armed = 0;
    *dropped = f->dropped;
    f->dropped = 0;
    return ON_FLOOD_END;
}
