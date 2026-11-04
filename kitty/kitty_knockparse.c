/*
 * kitty_knockparse.c - the port-knocking sequence, read and checked
 * (kitty_knock.h has the format). No network code: test/test_knock.c links
 * this file alone.
 */
#include <string.h>
#include "putty.h"
#include "kitty_knock.h"

static bool knock_sep(char c)
{
    return c == ' ' || c == '\t' || c == ',' || c == ';' || c == '-' ||
           c == '\r' || c == '\n';
}

static const char *knock_skip_seps(const char *p)
{
    while (*p && knock_sep(*p))
        p++;
    return p;
}

/* "1".."65535", digits only */
static bool knock_port(const char *s, size_t len, unsigned *port)
{
    unsigned v = 0;
    size_t i;

    if (len == 0 || len > 5)
        return false;
    for (i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9')
            return false;
        v = v * 10 + (unsigned)(s[i] - '0');
    }
    if (v < 1 || v > 65535)
        return false;
    *port = v;
    return true;
}

/* "N" or "N.F" seconds, to milliseconds; *over when above the limit */
static bool knock_pause(const char *s, size_t len, unsigned *ms, bool *over)
{
    unsigned long whole = 0, frac = 0, scale = 100;
    size_t i = 0;
    bool digits = false;

    for (; i < len && s[i] >= '0' && s[i] <= '9'; i++) {
        if (whole < 100000)
            whole = whole * 10 + (unsigned long)(s[i] - '0');
        digits = true;
    }
    if (i < len && s[i] == '.') {
        for (i++; i < len && s[i] >= '0' && s[i] <= '9'; i++) {
            frac += (unsigned long)(s[i] - '0') * scale;
            scale /= 10;
            digits = true;
        }
    }
    if (!digits || i != len)
        return false;
    if (whole * 1000 + frac > KNOCK_PAUSE_MAX_MS) {
        *ms = KNOCK_PAUSE_MAX_MS;
        *over = true;
    } else {
        *ms = (unsigned)(whole * 1000 + frac);
        *over = false;
    }
    return true;
}

static bool knock_word(const char *s, size_t len, const char *word)
{
    size_t i;

    if (len != strlen(word))
        return false;
    for (i = 0; i < len; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        if (c != word[i])
            return false;
    }
    return true;
}

static char *knock_entry_text(const char *from, const char *to)
{
    while (to > from && knock_sep(to[-1]))
        to--;
    return dupprintf("%.*s", (int)(to - from), from);
}

bool knock_parse(const char *text, KnockList *list)
{
    const char *p = text ? text : "";

    memset(list, 0, sizeof(*list));
    while (true) {
        const char *start, *num, *proto = NULL, *q;
        size_t numlen, protolen = 0;
        bool extra_colon = false;
        KnockStep st;

        p = knock_skip_seps(p);
        if (!*p)
            break;
        start = num = p;
        while (*p && !knock_sep(*p) && *p != ':')
            p++;
        numlen = (size_t)(p - num);
        /* "2001 : udp" is one entry */
        q = knock_skip_seps(p);
        if (*q == ':') {
            p = knock_skip_seps(q + 1);
            proto = p;
            while (*p && !knock_sep(*p) && *p != ':')
                p++;
            protolen = (size_t)(p - proto);
            if (*p == ':') {
                p++;           /* "2001:tcp:" - shown as the bad entry */
                extra_colon = true;
            }
        }
        if (extra_colon) {
            list->bad = knock_entry_text(start, p);
            return false;
        }

        memset(&st, 0, sizeof(st));
        if (proto && knock_word(proto, protolen, "s")) {
            bool over;
            st.kind = KNOCK_PAUSE;
            if (!knock_pause(num, numlen, &st.ms, &over)) {
                list->bad = knock_entry_text(start, p);
                return false;
            }
            if (over && !list->cut)
                list->cut = knock_entry_text(start, p);
        } else {
            if (!proto || knock_word(proto, protolen, "tcp"))
                st.kind = KNOCK_TCP;
            else if (knock_word(proto, protolen, "udp"))
                st.kind = KNOCK_UDP;
            else {
                list->bad = knock_entry_text(start, p);
                return false;
            }
            if (!knock_port(num, numlen, &st.port)) {
                list->bad = knock_entry_text(start, p);
                return false;
            }
        }
        sgrowarray(list->steps, list->size, list->n);
        list->steps[list->n++] = st;
    }
    return true;
}

void knock_list_free(KnockList *list)
{
    sfree(list->steps);
    sfree(list->bad);
    sfree(list->cut);
    memset(list, 0, sizeof(*list));
}
