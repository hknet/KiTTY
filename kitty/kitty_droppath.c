/*
 * kitty_droppath.c - the remote paths of a drag-and-drop upload, as the text
 * typed into the terminal. See kitty_droppath.h.
 */
#include <stdlib.h>
#include <string.h>

#include "kitty_droppath.h"

char *kitty_droppath_remote(const char *remotedir, const char *localname)
{
    const char *end, *base, *dir = remotedir ? remotedir : "";
    size_t blen, dlen;
    const char *pre;
    char *r;

    if (!localname) return NULL;
    end = localname + strlen(localname);
    while (end > localname && (end[-1] == '\\' || end[-1] == '/')) end--;
    base = end;
    while (base > localname && base[-1] != '\\' && base[-1] != '/') base--;
    blen = (size_t)(end - base);
    /* A drive root ("C:\") has no name to give the remote copy. */
    if (blen == 0 || memchr(base, ':', blen)) return NULL;

    while (dir[0] == '.' && dir[1] == '/') dir += 2;     /* "./x" = "x" */
    dlen = strlen(dir);
    while (dlen > 1 && dir[dlen - 1] == '/') dlen--;     /* "/" stays "/" */
    if (dlen == 0 || (dlen == 1 && dir[0] == '.')) {
        pre = "~/"; dlen = 0;                            /* the home directory */
    } else if (dir[0] == '/' || dir[0] == '~') {
        pre = "";
    } else {
        pre = "~/";                                      /* relative to home */
    }
    r = malloc(strlen(pre) + dlen + 1 + blen + 1);
    if (!r) return NULL;
    {
        size_t at = strlen(pre);
        memcpy(r, pre, at);
        memcpy(r + at, dir, dlen);
        at += dlen;
        if (dlen > 0 && !(dlen == 1 && dir[0] == '/')) r[at++] = '/';
        memcpy(r + at, base, blen);
        r[at + blen] = '\0';
    }
    return r;
}

static int safe_char(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || (c && strchr("_@%+=:,./-", c));
}

/* The quoted form of s[0..n) appended at out; returns the bytes written.
 * out NULL: only count them. */
static size_t quote_into(char *out, const char *s, size_t n)
{
    size_t i, k = 0;
    int plain = n > 0;
    for (i = 0; i < n; i++)
        if (!safe_char((unsigned char)s[i])) { plain = 0; break; }
    if (plain) {
        if (out) memcpy(out, s, n);
        return n;
    }
    if (out) out[k] = '\'';
    k++;
    for (i = 0; i < n; i++) {
        if (s[i] == '\'') {
            if (out) memcpy(out + k, "'\\''", 4);
            k += 4;
        } else {
            if (out) out[k] = s[i];
            k++;
        }
    }
    if (out) out[k] = '\'';
    k++;
    return k;
}

char *kitty_droppath_quote(const char *path)
{
    size_t keep = 0, n, len;
    char *r;

    if (!path) return NULL;
    n = strlen(path);
    if (path[0] == '~') {
        /* "~/" or "~user/": left for the shell to expand. */
        const char *slash = strchr(path, '/');
        size_t i, ulen = slash ? (size_t)(slash - path) : n;
        int ok = 1;
        for (i = 1; i < ulen; i++) {
            unsigned char c = (unsigned char)path[i];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
                ok = 0;
        }
        if (ok) keep = slash ? ulen + 1 : n;
    }
    /* Nothing after the kept prefix stays as it is; an empty path is ''. */
    len = keep + (keep < n || n == 0 ? quote_into(NULL, path + keep, n - keep) : 0);
    r = malloc(len + 1);
    if (!r) return NULL;
    memcpy(r, path, keep);
    if (keep < n || n == 0) quote_into(r + keep, path + keep, n - keep);
    r[len] = '\0';
    return r;
}

char *kitty_droppath_text(const char *const *paths, int n, int bracketed)
{
    static const char start[] = "\033[200~", stop[] = "\033[201~";
    size_t len = 0, at = 0;
    int i, any = 0;
    char *r;

    if (!paths || n <= 0) return NULL;
    /* Every path's quoted form is at most 2 + 4 * its length. */
    for (i = 0; i < n; i++)
        if (paths[i]) { len += 1 + 2 + 4 * strlen(paths[i]); any = 1; }
    if (!any) return NULL;
    len += sizeof(start) - 1 + sizeof(stop) - 1;
    r = malloc(len + 1);
    if (!r) return NULL;
    if (bracketed) { memcpy(r, start, sizeof(start) - 1); at = sizeof(start) - 1; }
    any = 0;
    for (i = 0; i < n; i++) {
        char *q;
        size_t ql;
        if (!paths[i]) continue;
        q = kitty_droppath_quote(paths[i]);
        if (!q) { free(r); return NULL; }
        ql = strlen(q);
        if (any) r[at++] = ' ';
        memcpy(r + at, q, ql);
        at += ql;
        free(q);
        any = 1;
    }
    if (bracketed) { memcpy(r + at, stop, sizeof(stop) - 1); at += sizeof(stop) - 1; }
    r[at] = '\0';
    return r;
}
