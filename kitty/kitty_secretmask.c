/*
 * kitty_secretmask.c - secrets masked out of a command line. See
 * kitty_secretmask.h.
 */
#include <stdlib.h>
#include <string.h>

#include "kitty_secretmask.h"

void smemclr(void *b, size_t len);   /* utils/smemclr.c (misc.h): a wipe the compiler cannot drop */

void kitty_mask_span(char *line, size_t at, size_t len)
{
    size_t n;
    if (!line || len == 0)
        return;
    n = strlen(line);
    if (at >= n)
        return;
    if (len > n - at)
        len = n - at;
    memset(line + at, '*', len);
}

static void mask_all(char *line, const char *form)
{
    size_t fl;
    char *p;
    if (!form || !*form)
        return;
    fl = strlen(form);
    for (p = strstr(line, form); p; p = strstr(p + fl, form))
        memset(p, '*', fl);
}

/* the body qcat writes between its quotes */
static char *form_qcat(const char *s)
{
    size_t n = strlen(s), nbs = 0, j = 0;
    char *o = malloc(4 * n + 1);
    if (!o)
        return NULL;
    for (const char *p = s; *p; p++) {
        if (*p == '\\') {
            nbs++;
        } else if (*p == '"') {
            for (size_t k = 0; k < 2 * nbs + 1; k++) o[j++] = '\\';
            o[j++] = '"';
            nbs = 0;
        } else {
            for (size_t k = 0; k < nbs; k++) o[j++] = '\\';
            nbs = 0;
            o[j++] = *p;
        }
    }
    for (size_t k = 0; k < 2 * nbs; k++) o[j++] = '\\';
    o[j] = '\0';
    return o;
}

/* the body rawcat writes between its quotes */
static char *form_rawcat(const char *s)
{
    size_t j = 0;
    char *o = malloc(2 * strlen(s) + 1);
    if (!o)
        return NULL;
    for (const char *p = s; *p; p++) {
        if (*p == '"')
            o[j++] = '"';
        o[j++] = *p;
    }
    o[j] = '\0';
    return o;
}

/* urlcat's percent-encoding, upper or lower case hex */
static char *form_url(const char *s, int lower)
{
    const char *hexd = lower ? "0123456789abcdef" : "0123456789ABCDEF";
    size_t j = 0;
    char *o = malloc(3 * strlen(s) + 1);
    if (!o)
        return NULL;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' ||
            c == '~') {
            o[j++] = (char)c;
        } else {
            o[j++] = '%';
            o[j++] = hexd[(c >> 4) & 0xF];
            o[j++] = hexd[c & 0xF];
        }
    }
    o[j] = '\0';
    return o;
}

void kitty_mask_secret(char *line, const char *secret)
{
    char *forms[4];
    if (!line || !secret || !*secret)
        return;
    forms[0] = form_qcat(secret);       /* longest forms first: a plain match */
    forms[1] = form_rawcat(secret);     /* inside one of them is masked anyway */
    forms[2] = form_url(secret, 0);
    forms[3] = form_url(secret, 1);
    for (int i = 0; i < 4; i++) {
        mask_all(line, forms[i]);
        if (forms[i]) {
            smemclr(forms[i], strlen(forms[i]));
            free(forms[i]);
        }
    }
    mask_all(line, secret);
}
