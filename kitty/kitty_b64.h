/*
 * Base64 codec for KiTTY at-rest secret handling (see kitty_b64.c).
 * Returned buffers are malloc'd and owned by the caller. decode returns NULL
 * on malformed input.
 */
#ifndef KITTY_B64_H
#define KITTY_B64_H

char *ksec_b64_encode(const unsigned char *in, int len);
int ksec_b64_val(int c);
unsigned char *ksec_b64_decode(const char *in, int *outlen);

#endif
