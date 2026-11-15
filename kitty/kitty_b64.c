/*
 * The base64 of KiTTY's at-rest secret handling: DPAPI blobs and
 * master-password salts in windows/storage.c, kitty_secretstore.c, kitty.c
 * and kitty_storemove.c.
 *
 * No codec of its own: both functions are PuTTY's base64 (utils/base64_*)
 * behind the interface the callers use - a malloc'd buffer the caller frees.
 * Encoding is PuTTY's without line breaks: standard RFC 4648 alphabet, '='
 * padding, byte for byte what the own codec here wrote before, so every
 * stored value reads as it did.
 *
 * Decoding stays strict, because a damaged stored secret must be caught,
 * not half decoded: whole groups of four, no line breaks, nothing outside the
 * alphabet, '=' only at the end (base64_valid). Anything else is NULL.
 * PuTTY's own decoder alone would skip what it cannot read.
 *
 * Base64 is an ENCODING, not a secrecy layer: what protects a stored secret
 * is the DPAPI or master-password wrapping around it, never this. libb64
 * under kitty/far2l/ (cdecode.c, cencode.c) serves only the far2l
 * extensions and is not used here, so a far2l update never touches the
 * secret store's path.
 */
#include <stdlib.h>
#include <string.h>

#include "misc.h"
#include "kitty_b64.h"

char *ksec_b64_encode(const unsigned char *in, int len)
{
    strbuf *sb;
    char *out;

    if (!in || len < 0)
        return NULL;
    sb = base64_encode_sb(make_ptrlen(in, (size_t)len), 0);
    out = malloc(sb->len + 1);
    if (out) {
        memcpy(out, sb->s, sb->len);
        out[sb->len] = '\0';
    }
    strbuf_free(sb);
    return out;
}

unsigned char *ksec_b64_decode(const char *in, int *outlen)
{
    ptrlen pl;
    strbuf *sb;
    unsigned char *out;

    if (!in)
        return NULL;
    pl = ptrlen_from_asciz(in);
    /* base64_valid skips line breaks and accepts a missing final '=';
     * neither ever comes out of ksec_b64_encode, so neither is let in */
    if (pl.len < 4 || pl.len % 4 != 0 || strpbrk(in, "\r\n") ||
        !base64_valid(pl))
        return NULL;
    sb = base64_decode_sb(pl);        /* strbuf_new_nm: wiped when freed */
    out = malloc(sb->len ? sb->len : 1);
    if (out) {
        memcpy(out, sb->u, sb->len);
        *outlen = (int)sb->len;
    }
    strbuf_free(sb);
    return out;
}
