/*
 * Base64 for KiTTY's at-rest secret handling: PuTTY's codec behind a
 * malloc'd interface (see kitty_b64.c). Returned buffers are owned by the
 * caller. decode returns NULL on malformed input.
 */
#ifndef KITTY_B64_H
#define KITTY_B64_H

char *ksec_b64_encode(const unsigned char *in, int len);
unsigned char *ksec_b64_decode(const char *in, int *outlen);

#endif
