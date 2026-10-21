/*
 * KiTTY: reading the agent log's lines - see kitty_auditparse.h.
 */

#include <stdlib.h>
#include <string.h>

#include "kitty_auditparse.h"

int kitty_auditparse_is_old(const char *line)
{
    const char *p;
    if (!line)
        return 0;
    for (p = line; p[0] && p[1]; p++) {
        if (p[0] != '\\' || p[1] != '\\')
            continue;
        if (p - line >= 2 && p[-1] == '"' && p[-2] == '=')
            continue;           /* a value starting with \\: a UNC path */
        return 1;
    }
    return 0;
}

char *kitty_auditparse_from_old(const char *p)
{
    /* every byte at most doubled (a quote), plus the terminator */
    char *out = malloc(2 * strlen(p) + 1), *o = out;
    if (!out)
        return NULL;
    while (*p) {
        if (*p == '"') {
            *o++ = *p++;
            while (*p && *p != '"') {
                char c = *p;
                if (c == '\\' && p[1]) {
                    p++;
                    c = (*p == 'n') ? '?' : *p;
                }
                if (c == '"')
                    *o++ = '"';
                *o++ = c;
                p++;
            }
            if (*p == '"')
                *o++ = *p++;
        } else {
            *o++ = *p++;
        }
    }
    *o = '\0';
    return out;
}

int kitty_auditparse_next(const char **pp, char *key, size_t ksz,
                          char *val, size_t vsz)
{
    const char *p = *pp;
    size_t k = 0, n = 0;
    while (*p == ' ')
        p++;
    if (!*p)
        return 0;
    while (*p && *p != '=' && *p != ' ') {
        if (k + 1 < ksz)
            key[k++] = *p;
        p++;
    }
    if (*p == '=') {
        p++;
        if (*p == '"') {
            p++;
            while (*p) {
                if (*p == '"') {
                    if (p[1] != '"') {
                        p++;
                        break;                  /* the closing quote */
                    }
                    p++;                        /* "" -> one quote */
                }
                if (val && n + 1 < vsz)
                    val[n++] = *p;
                p++;
            }
        } else {
            while (*p && *p != ' ') {
                if (val && n + 1 < vsz)
                    val[n++] = *p;
                p++;
            }
        }
    }
    if (ksz)
        key[k] = '\0';
    if (val && vsz)
        val[n] = '\0';
    *pp = p;
    return 1;
}

int kitty_auditparse_field(const char *line, const char *key,
                           char *out, size_t outsz)
{
    const char *p = line;
    char k[32];
    for (;;) {
        const char *at = p;
        if (!kitty_auditparse_next(&p, k, sizeof(k), NULL, 0))
            return 0;
        if (!strcmp(k, key)) {
            kitty_auditparse_next(&at, k, sizeof(k), out, outsz);
            return 1;
        }
    }
}
