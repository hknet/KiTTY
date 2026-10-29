/*
 * kitty_hostkey_blob.c - a stored host key's text back into its SSH public
 * blob (kitty_hostkey_blob.h). Moved out of kitty_hostkeys.c, which needs
 * the settings library: kageant's destination lookup
 * (kitty_kageant_dest.c) reads the store with this code too.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "putty.h"
#include "ssh.h"
#include "mpint.h"
#include "kitty_hostkey_blob.h"

/* ---- the stored text -> the public blob ---------------------------------- */

/* "0x1a,0x2b,..." (and for ECDSA a leading curve name) into its pieces. */
static int split_commas(const char *text, char **out, int max)
{
    int n = 0;
    const char *p = text;
    while (p && *p && n < max) {
        const char *c = strchr(p, ',');
        size_t len = c ? (size_t)(c - p) : strlen(p);
        out[n] = snewn(len + 1, char);
        memcpy(out[n], p, len);
        out[n][len] = '\0';
        n++;
        p = c ? c + 1 : NULL;
    }
    return n;
}

static mp_int *hex_token(const char *tok)
{
    if (strlen(tok) > 2 && tok[0] == '0' && (tok[1] == 'x' || tok[1] == 'X'))
        return mp_from_hex(tok + 2);
    return NULL;
}

/* Fixed-width big-endian bytes of x (a Weierstrass coordinate). */
static void put_mp_be_fixed(strbuf *sb, mp_int *x, size_t width)
{
    for (size_t i = 0; i < width; i++)
        put_byte(sb, mp_get_byte(x, width - 1 - i));
}

strbuf *kitty_hostkey_blob_from_text(const char *keytype, const char *text)
{
    char *tok[6] = { NULL, NULL, NULL, NULL, NULL, NULL };
    int n, i;
    strbuf *blob = NULL;

    if (!keytype || !text || !*text)
        return NULL;
    n = split_commas(text, tok, lenof(tok));

    if (!strcmp(keytype, "rsa2") && n == 2) {
        /* rsastr_fmt: exponent, modulus */
        mp_int *e = hex_token(tok[0]), *m = hex_token(tok[1]);
        if (e && m) {
            blob = strbuf_new();
            put_stringz(blob, "ssh-rsa");
            put_mp_ssh2(blob, e);
            put_mp_ssh2(blob, m);
        }
        if (e) mp_free(e);
        if (m) mp_free(m);
    } else if (!strcmp(keytype, "dss") && n == 4) {
        mp_int *v[4];
        bool ok = true;
        for (i = 0; i < 4; i++) { v[i] = hex_token(tok[i]); ok = ok && v[i]; }
        if (ok) {
            blob = strbuf_new();
            put_stringz(blob, "ssh-dss");
            for (i = 0; i < 4; i++) put_mp_ssh2(blob, v[i]);
        }
        for (i = 0; i < 4; i++) if (v[i]) mp_free(v[i]);
    } else if (!strncmp(keytype, "ecdsa-sha2-nistp", 16) && n == 3) {
        /* ecc_cache_str_shared: curve name, x, y. The point goes on the wire
         * uncompressed: 0x04, X, Y, each the curve's field width. */
        size_t width = !strcmp(tok[0], "nistp256") ? 32 :
                       !strcmp(tok[0], "nistp384") ? 48 :
                       !strcmp(tok[0], "nistp521") ? 66 : 0;
        mp_int *x = hex_token(tok[1]), *y = hex_token(tok[2]);
        if (width && x && y && !strcmp(keytype + 11, tok[0])) {
            strbuf *pt = strbuf_new();
            put_byte(pt, 0x04);
            put_mp_be_fixed(pt, x, width);
            put_mp_be_fixed(pt, y, width);
            blob = strbuf_new();
            put_stringz(blob, keytype);
            put_stringz(blob, tok[0]);
            put_stringpl(blob, ptrlen_from_strbuf(pt));
            strbuf_free(pt);
        }
        if (x) mp_free(x);
        if (y) mp_free(y);
    } else if ((!strcmp(keytype, "ssh-ed25519") || !strcmp(keytype, "ssh-ed448")) && n == 2) {
        /* Edwards: y little-endian in the curve's byte width, the top bit of
         * the last byte carrying the parity of x. (No curve name in the text:
         * the Edwards curves have name == NULL.) */
        size_t width = !strcmp(keytype, "ssh-ed25519") ? 32 : 57;
        mp_int *x = hex_token(tok[0]), *y = hex_token(tok[1]);
        if (x && y) {
            strbuf *pt = strbuf_new();
            for (size_t b = 0; b < width; b++) {
                unsigned char c = mp_get_byte(y, b);
                if (b == width - 1)
                    c = (unsigned char)((c & 0x7f) | ((mp_get_byte(x, 0) & 1) << 7));
                put_byte(pt, c);
            }
            blob = strbuf_new();
            put_stringz(blob, keytype);
            put_stringpl(blob, ptrlen_from_strbuf(pt));
            strbuf_free(pt);
        }
        if (x) mp_free(x);
        if (y) mp_free(y);
    }

    for (i = 0; i < n; i++) sfree(tok[i]);
    return blob;
}
