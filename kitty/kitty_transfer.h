/*
 * kitty_transfer.h - kitty's file-transfer protocol (OSC 5113, the far end
 * runs `kitten transfer`).
 *
 * Two parts:
 *
 *  1. The entry points terminal.c calls. They are WEAK: terminal.c is also
 *     compiled into test targets (test_osc52) that stub the KiTTY seams and
 *     do not link kitty_transfer.c; a weak undefined reference resolves to
 *     NULL there, so the caller tests the pointer before calling.
 *
 *  2. Under KT5113_PARSE_IMPL, the dependency-free command parser and the
 *     file-name rules, as static functions: kitty_transfer.c and the unit
 *     test test/test_transfer_parse.c compile the SAME code, the way
 *     kitty_osc7_parse.h is shared with test_osc7. Nothing here touches the
 *     filesystem or Win32; that is what makes it testable on the build host.
 *
 * Wire format (docs/file-transfer-protocol.rst of kitty):
 *   ESC ] 5113 ; key=value ; key=value ... ST
 * keys are [a-zA-Z0-9_]; n/st/pw are base64 UTF-8 strings, d is base64
 * bytes, id/fid/pr are "safe strings" [0-9a-zA-Z_:./@-], sz/mod/prm/q are
 * decimal integers, the rest enums. Unknown keys are ignored. The reference
 * client encodes base64 WITHOUT padding and refuses padded input, so the
 * encoder here emits none and the decoder accepts both.
 */
#ifndef KITTY_TRANSFER_H
#define KITTY_TRANSFER_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct terminal_tag;

/* One complete OSC 5113 command has arrived in term->osc_string. Plain
 * externals: a weak declaration here made the DEFINITION weak too, and the
 * PE linker then resolved terminal.c's reference to nothing - the dispatch
 * ran, the code never did. test/test_osc52.c carries two stubs instead. */
void kitty_transfer_osc(struct terminal_tag *term);
/* The terminal is going away: drop its sessions, close and delete partial
 * files, cancel timers. */
void kitty_transfer_free(struct terminal_tag *term);

/* ---- limits (also the unit test's expectations) ---- */
#define KT5113_NAME_MAX   4096         /* a path, UTF-8 bytes, as the spec caps it */
#define KT5113_COMP_MAX   255          /* one path component */
#define KT5113_ID_MAX     64           /* id / file_id / parent */
#define KT5113_ST_MAX     256          /* a status string */
#define KT5113_DATA_MAX   (64 * 1024)  /* one data chunk after base64 */

enum {
    KT5113_AC_NONE, KT5113_AC_SEND, KT5113_AC_FILE, KT5113_AC_DATA,
    KT5113_AC_END_DATA, KT5113_AC_RECEIVE, KT5113_AC_CANCEL, KT5113_AC_STATUS,
    KT5113_AC_FINISH, KT5113_AC_UNKNOWN
};
enum { KT5113_FT_REGULAR, KT5113_FT_DIRECTORY, KT5113_FT_SYMLINK,
       KT5113_FT_LINK, KT5113_FT_UNKNOWN };
enum { KT5113_TT_SIMPLE, KT5113_TT_RSYNC, KT5113_TT_UNKNOWN };
enum { KT5113_ZIP_NONE, KT5113_ZIP_ZLIB, KT5113_ZIP_UNKNOWN };

typedef struct kt5113_cmd {
    int action, ftype, ttype, zip;
    int quiet;                          /* q: 0 all, 1 no acks, 2 nothing */
    int has_bypass;                     /* pw= was present (never honoured) */
    char id[KT5113_ID_MAX + 1];
    char fid[KT5113_ID_MAX + 1];
    char parent[KT5113_ID_MAX + 1];
    char name[KT5113_NAME_MAX + 1];     /* decoded, NUL-terminated */
    size_t name_len;
    int name_bad;                       /* bad base64, too long, or embedded NUL */
    char status[KT5113_ST_MAX + 1];
    int64_t size, mtime, perms;
    int has_size, has_mtime, has_perms;
    unsigned char data[KT5113_DATA_MAX];
    size_t data_len;
    int has_data;
    int data_bad;                       /* bad base64 or over KT5113_DATA_MAX */
    int bad;                            /* an id-class field was not a safe string
                                         * or was too long: the command is unusable */
} kt5113_cmd;

#ifdef KT5113_PARSE_IMPL

/* ---- base64, standard alphabet ---- */

static inline int kt5113_b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* Decode; padding optional; whitespace not allowed. Returns 1 on success. A
 * decode that would exceed outmax fails rather than truncating. */
static inline int kt5113_b64_decode(const char *in, size_t inlen,
                             unsigned char *out, size_t outmax, size_t *outlen)
{
    size_t i, o = 0;
    unsigned int acc = 0;
    int nbits = 0;

    while (inlen > 0 && in[inlen - 1] == '=')
        inlen--;
    for (i = 0; i < inlen; i++) {
        int v = kt5113_b64_val(in[i]);
        if (v < 0)
            return 0;
        acc = (acc << 6) | (unsigned int)v;
        nbits += 6;
        if (nbits >= 8) {
            nbits -= 8;
            if (o >= outmax)
                return 0;
            out[o++] = (unsigned char)((acc >> nbits) & 0xff);
        }
    }
    /* a single leftover sextet cannot encode anything: malformed */
    if (nbits >= 6)
        return 0;
    *outlen = o;
    return 1;
}

/* Encode without padding. out needs kt5113_b64_size(n) bytes incl. NUL. */
#define kt5113_b64_size(n) ((((n) + 2) / 3) * 4 + 1)
static inline size_t kt5113_b64_encode(const unsigned char *in, size_t n, char *out)
{
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i, o = 0;
    for (i = 0; i + 3 <= n; i += 3) {
        unsigned int v = ((unsigned int)in[i] << 16) |
                         ((unsigned int)in[i + 1] << 8) | in[i + 2];
        out[o++] = tbl[(v >> 18) & 63];
        out[o++] = tbl[(v >> 12) & 63];
        out[o++] = tbl[(v >> 6) & 63];
        out[o++] = tbl[v & 63];
    }
    if (n - i == 1) {
        unsigned int v = (unsigned int)in[i] << 16;
        out[o++] = tbl[(v >> 18) & 63];
        out[o++] = tbl[(v >> 12) & 63];
    } else if (n - i == 2) {
        unsigned int v = ((unsigned int)in[i] << 16) | ((unsigned int)in[i + 1] << 8);
        out[o++] = tbl[(v >> 18) & 63];
        out[o++] = tbl[(v >> 12) & 63];
        out[o++] = tbl[(v >> 6) & 63];
    }
    out[o] = '\0';
    return o;
}

/* ---- field types ---- */

static inline int kt5113_safe_char(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') || c == '_' || c == ':' || c == '.' ||
           c == '/' || c == '@' || c == '-';
}

/* A safe string into a fixed buffer; 0 if a character is outside the set or
 * the value is too long. Never a truncation: an id echoed back shortened would
 * match nothing on the far end and a forged one could break our framing. */
static inline int kt5113_copy_safe(const char *v, size_t n, char *out, size_t outsz)
{
    size_t i;
    if (n >= outsz)
        return 0;
    for (i = 0; i < n; i++)
        if (!kt5113_safe_char(v[i]))
            return 0;
    memcpy(out, v, n);
    out[n] = '\0';
    return 1;
}

static inline int kt5113_parse_int64(const char *v, size_t n, int64_t *out)
{
    size_t i = 0;
    int neg = 0;
    uint64_t acc = 0;
    if (n == 0)
        return 0;
    if (v[0] == '-') { neg = 1; i = 1; }
    if (i >= n || n - i > 19)
        return 0;
    for (; i < n; i++) {
        if (v[i] < '0' || v[i] > '9')
            return 0;
        acc = acc * 10 + (uint64_t)(v[i] - '0');
    }
    if (acc > (uint64_t)INT64_MAX)
        return 0;
    *out = neg ? -(int64_t)acc : (int64_t)acc;
    return 1;
}

static inline int kt5113_enum(const char *v, size_t n, const char *const *names, int unknown)
{
    int i;
    for (i = 0; names[i]; i++)
        if (strlen(names[i]) == n && !memcmp(names[i], v, n))
            return i;
    return unknown;
}

static inline void kt5113_field(kt5113_cmd *c, const char *k, size_t kn,
                         const char *v, size_t vn)
{
    static const char *const actions[] = {
        "", "send", "file", "data", "end_data", "receive", "cancel", "status",
        "finish", NULL };
    static const char *const ftypes[] = {
        "regular", "directory", "symlink", "link", NULL };
    static const char *const ttypes[] = { "simple", "rsync", NULL };
    static const char *const zips[] = { "none", "zlib", NULL };
    int64_t iv;

    if (kn == 2 && !memcmp(k, "ac", 2)) {
        c->action = kt5113_enum(v, vn, actions, KT5113_AC_UNKNOWN);
        if (c->action == KT5113_AC_NONE) c->action = KT5113_AC_UNKNOWN;
    } else if (kn == 2 && !memcmp(k, "id", 2)) {
        if (!kt5113_copy_safe(v, vn, c->id, sizeof(c->id))) c->bad = 1;
    } else if (kn == 3 && !memcmp(k, "fid", 3)) {
        if (!kt5113_copy_safe(v, vn, c->fid, sizeof(c->fid))) c->bad = 1;
    } else if (kn == 2 && !memcmp(k, "pr", 2)) {
        if (!kt5113_copy_safe(v, vn, c->parent, sizeof(c->parent))) c->bad = 1;
    } else if (kn == 1 && k[0] == 'n') {
        size_t got = 0;
        if (kt5113_b64_decode(v, vn, (unsigned char *)c->name,
                              KT5113_NAME_MAX, &got) &&
            memchr(c->name, '\0', got) == NULL) {
            c->name_len = got;
            c->name[got] = '\0';
        } else {
            c->name_bad = 1;
            c->name_len = 0;
            c->name[0] = '\0';
        }
    } else if (kn == 2 && !memcmp(k, "st", 2)) {
        size_t got = 0;
        if (kt5113_b64_decode(v, vn, (unsigned char *)c->status,
                              KT5113_ST_MAX, &got))
            c->status[got] = '\0';
        else
            c->status[0] = '\0';
    } else if (kn == 1 && k[0] == 'd') {
        size_t got = 0;
        c->has_data = 1;
        if (kt5113_b64_decode(v, vn, c->data, KT5113_DATA_MAX, &got))
            c->data_len = got;
        else {
            c->data_bad = 1;
            c->data_len = 0;
        }
    } else if (kn == 2 && !memcmp(k, "sz", 2)) {
        if (kt5113_parse_int64(v, vn, &iv)) { c->size = iv; c->has_size = 1; }
    } else if (kn == 3 && !memcmp(k, "mod", 3)) {
        if (kt5113_parse_int64(v, vn, &iv)) { c->mtime = iv; c->has_mtime = 1; }
    } else if (kn == 3 && !memcmp(k, "prm", 3)) {
        if (kt5113_parse_int64(v, vn, &iv)) { c->perms = iv; c->has_perms = 1; }
    } else if (kn == 1 && k[0] == 'q') {
        if (kt5113_parse_int64(v, vn, &iv) && iv >= 0 && iv <= 2) c->quiet = (int)iv;
    } else if (kn == 2 && !memcmp(k, "ft", 2)) {
        c->ftype = kt5113_enum(v, vn, ftypes, KT5113_FT_UNKNOWN);
    } else if (kn == 2 && !memcmp(k, "tt", 2)) {
        c->ttype = kt5113_enum(v, vn, ttypes, KT5113_TT_UNKNOWN);
    } else if (kn == 3 && !memcmp(k, "zip", 3)) {
        c->zip = kt5113_enum(v, vn, zips, KT5113_ZIP_UNKNOWN);
    } else if (kn == 2 && !memcmp(k, "pw", 2)) {
        c->has_bypass = 1;
    }
    /* anything else: an unknown key, ignored as the spec requires */
}

/* Parse the payload after "5113;" - key=value pairs separated by ';'. A pair
 * without '=' is skipped; a key with characters outside [a-zA-Z0-9_] is
 * skipped. Fields default to "absent" (size -1 etc.). */
static inline void kt5113_parse(const char *s, size_t len, kt5113_cmd *c)
{
    size_t i = 0;

    memset(c, 0, offsetof(kt5113_cmd, data));
    c->data_len = 0; c->has_data = 0; c->data_bad = 0; c->bad = 0;
    c->size = -1; c->mtime = -1; c->perms = -1;

    while (i < len) {
        size_t start = i, eq = (size_t)-1;
        while (i < len && s[i] != ';') {
            if (s[i] == '=' && eq == (size_t)-1)
                eq = i;
            i++;
        }
        if (eq != (size_t)-1 && eq > start) {
            size_t kn = eq - start, j;
            int keyok = 1;
            for (j = start; j < eq; j++) {
                char ch = s[j];
                if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                      (ch >= '0' && ch <= '9') || ch == '_'))
                    keyok = 0;
            }
            if (keyok)
                kt5113_field(c, s + start, kn, s + eq + 1, i - eq - 1);
        }
        if (i < len)
            i++;                        /* the ';' */
    }
}

/* ---- file names from the far end ---- */

/* Device names Windows reserves whatever the extension. */
static inline int kt5113_reserved_name(const char *c, size_t n)
{
    static const char *const names[] = {
        "CON", "PRN", "AUX", "NUL",
        "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9",
        NULL };
    size_t stem = 0, i;
    int k;
    while (stem < n && c[stem] != '.')
        stem++;
    for (k = 0; names[k]; k++) {
        size_t l = strlen(names[k]);
        if (l != stem)
            continue;
        for (i = 0; i < l; i++) {
            char ch = c[i];
            if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
            if (ch != names[k][i])
                break;
        }
        if (i == l)
            return 1;
    }
    return 0;
}

/* May this ONE component become a Windows file or folder name under a folder
 * of ours? No: empty, "." or "..", a control character, a character Windows
 * forbids (which includes ':' - an alternate data stream - and both slashes),
 * a trailing dot or space (Windows strips them, so the name written would not
 * be the name checked), a reserved device name, or over 255 bytes. */
static inline int kt5113_component_ok(const char *c, size_t n)
{
    size_t i;
    if (n == 0 || n > KT5113_COMP_MAX)
        return 0;
    if ((n == 1 && c[0] == '.') || (n == 2 && c[0] == '.' && c[1] == '.'))
        return 0;
    for (i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)c[i];
        if (ch < 0x20 || ch == 0x7f)
            return 0;
        switch (ch) {
          case '<': case '>': case ':': case '"': case '/': case '\\':
          case '|': case '?': case '*':
            return 0;
        }
    }
    if (c[n - 1] == '.' || c[n - 1] == ' ')
        return 0;
    if (kt5113_reserved_name(c, n))
        return 0;
    return 1;
}

/* Is this a path the far end may name at all? The spec's paths are POSIX,
 * so a backslash never occurs in a legitimate one and is refused outright
 * (it would otherwise become a separator on Windows). A ".." component
 * anywhere is refused: the reference client always sends absolute or
 * home-relative paths, so it never needs one, and it is the one thing that
 * could climb out of the destination. */
static inline int kt5113_path_ok(const char *p, size_t n)
{
    size_t i = 0;
    if (n == 0 || n > KT5113_NAME_MAX)
        return 0;
    if (memchr(p, '\\', n))
        return 0;
    while (i < n) {
        size_t s = i;
        while (i < n && p[i] != '/')
            i++;
        if (i - s == 2 && p[s] == '.' && p[s + 1] == '.')
            return 0;
        if (i < n)
            i++;
    }
    return 1;
}

/* The last component of a POSIX path (trailing slashes ignored). 0 if there
 * is none, or if it would not be a valid Windows name. */
static inline int kt5113_basename(const char *p, size_t n, const char **base, size_t *blen)
{
    size_t end = n, s;
    if (!kt5113_path_ok(p, n))
        return 0;
    while (end > 0 && p[end - 1] == '/')
        end--;
    s = end;
    while (s > 0 && p[s - 1] != '/')
        s--;
    if (end == s || !kt5113_component_ok(p + s, end - s))
        return 0;
    if (end - s == 1 && p[s] == '~')    /* the home marker is not a name */
        return 0;
    *base = p + s;
    *blen = end - s;
    return 1;
}

/* Does `name` lie inside the directory `dir` (both POSIX, dir without a
 * trailing slash)? Yields the relative remainder. */
static inline int kt5113_under(const char *dir, size_t dn, const char *name, size_t nn,
                        const char **rel, size_t *rn)
{
    if (dn == 0 || nn <= dn + 1)
        return 0;
    if (memcmp(dir, name, dn) != 0 || name[dn] != '/')
        return 0;
    *rel = name + dn + 1;
    *rn = nn - dn - 1;
    while (*rn > 0 && (*rel)[*rn - 1] == '/')
        (*rn)--;
    return *rn > 0;
}

/* Every component of a relative POSIX path must be a valid Windows name.
 * Returns the component count, or 0 when any component fails. */
static inline int kt5113_rel_ok(const char *rel, size_t n)
{
    size_t i = 0;
    int count = 0;
    while (i < n) {
        size_t s = i;
        while (i < n && rel[i] != '/')
            i++;
        if (!kt5113_component_ok(rel + s, i - s))
            return 0;
        count++;
        if (i < n)
            i++;
    }
    return count;
}

/* A path the far end asks to READ, classified.
 *   KT5113_SPEC_BAD       refuse (invalid or climbing)
 *   KT5113_SPEC_RELATIVE  *rel is a POSIX path relative to the download folder
 *                         (may be empty: the folder itself)
 *   KT5113_SPEC_DRIVE     "/C:/dir/file" - *rel is "dir/file", *drive the letter
 * "~/x", "/x" and "x" are all taken relative to the download folder; only the
 * spec's own Windows form names a drive. */
enum { KT5113_SPEC_BAD, KT5113_SPEC_RELATIVE, KT5113_SPEC_DRIVE };
static inline int kt5113_spec_classify(const char *p, size_t n, const char **rel,
                                size_t *rn, char *drive)
{
    const char *r;
    size_t l;
    *drive = 0;
    if (!kt5113_path_ok(p, n))
        return KT5113_SPEC_BAD;
    if (n >= 1 && p[0] == '~') {
        if (n == 1) { *rel = p + 1; *rn = 0; return KT5113_SPEC_RELATIVE; }
        if (p[1] != '/') return KT5113_SPEC_BAD;     /* ~user: not ours */
        r = p + 2; l = n - 2;
    } else if (n >= 3 && p[0] == '/' &&
               ((p[1] >= 'A' && p[1] <= 'Z') || (p[1] >= 'a' && p[1] <= 'z')) &&
               p[2] == ':' && (n == 3 || p[3] == '/')) {
        *drive = p[1];
        r = n > 3 ? p + 4 : p + 3;
        l = n > 3 ? n - 4 : 0;
        while (l > 0 && r[l - 1] == '/') l--;
        if (l > 0 && !kt5113_rel_ok(r, l)) return KT5113_SPEC_BAD;
        *rel = r; *rn = l;
        return KT5113_SPEC_DRIVE;
    } else if (p[0] == '/') {
        r = p + 1; l = n - 1;
    } else {
        r = p; l = n;
    }
    while (l > 0 && r[0] == '/') { r++; l--; }
    while (l > 0 && r[l - 1] == '/') l--;
    if (l > 0 && !kt5113_rel_ok(r, l))
        return KT5113_SPEC_BAD;
    *rel = r; *rn = l;
    return KT5113_SPEC_RELATIVE;
}

/* Adler-32 (RFC 1950), for the zlib trailer in both directions. */
static inline uint32_t kt5113_adler32(uint32_t adler, const unsigned char *p, size_t n)
{
    uint32_t a = adler & 0xffff, b = (adler >> 16) & 0xffff;
    while (n > 0) {
        size_t k = n < 5552 ? n : 5552;
        n -= k;
        while (k--) {
            a += *p++;
            b += a;
        }
        a %= 65521;
        b %= 65521;
    }
    return (b << 16) | a;
}

#endif /* KT5113_PARSE_IMPL */

#endif /* KITTY_TRANSFER_H */
