/*
 * Base64 codec used by KiTTY's at-rest secret handling in windows/storage.c
 * (DPAPI blobs, MPW salts). Pure, self-contained functions with no shared
 * state, moved out of storage.c verbatim to shrink that file's divergence from
 * upstream PuTTY. Standard RFC 4648 alphabet, '=' padding; decode returns NULL
 * on malformed input. Callers own the returned malloc'd buffers.
 */
#include <stdlib.h>
#include <string.h>

#include "kitty_b64.h"

static const char ksec_b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
char *ksec_b64_encode(const unsigned char *in, int len)
{
    int olen = ((len + 2) / 3) * 4, i, o = 0;
    char *out = malloc(olen + 1);
    if (!out) return NULL;
    for (i = 0; i < len; i += 3) {
        int n = len - i;
        unsigned a = in[i], b = n > 1 ? in[i+1] : 0, c = n > 2 ? in[i+2] : 0;
        out[o++] = ksec_b64[a >> 2];
        out[o++] = ksec_b64[((a & 3) << 4) | (b >> 4)];
        out[o++] = n > 1 ? ksec_b64[((b & 15) << 2) | (c >> 6)] : '=';
        out[o++] = n > 2 ? ksec_b64[c & 63] : '=';
    }
    out[o] = '\0';
    return out;
}
int ksec_b64_val(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
unsigned char *ksec_b64_decode(const char *in, int *outlen)
{
    int len = (int)strlen(in), pad = 0, i, o = 0, olen;
    unsigned char *out;
    if (len < 4 || (len % 4) != 0) return NULL;
    if (in[len-1] == '=') pad++;
    if (in[len-2] == '=') pad++;
    olen = (len / 4) * 3 - pad;
    out = malloc(olen > 0 ? olen : 1);
    if (!out) return NULL;
    for (i = 0; i < len; i += 4) {
        int v0 = ksec_b64_val(in[i]), v1 = ksec_b64_val(in[i+1]);
        int c2 = in[i+2], c3 = in[i+3];
        int v2 = (c2 == '=') ? 0 : ksec_b64_val(c2);
        int v3 = (c3 == '=') ? 0 : ksec_b64_val(c3);
        unsigned trip;
        if (v0 < 0 || v1 < 0 || (c2 != '=' && v2 < 0) || (c3 != '=' && v3 < 0)) { free(out); return NULL; }
        trip = ((unsigned)v0 << 18) | ((unsigned)v1 << 12) | ((unsigned)v2 << 6) | (unsigned)v3;
        if (o < olen) out[o++] = (trip >> 16) & 0xff;
        if (o < olen) out[o++] = (trip >>  8) & 0xff;
        if (o < olen) out[o++] =  trip        & 0xff;
    }
    *outlen = olen;
    return out;
}
