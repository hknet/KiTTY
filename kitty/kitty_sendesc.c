/*
 * kitty_sendesc.c - the operand of the \k and \x escapes. See
 * kitty_sendesc.h.
 */
#include "kitty_sendesc.h"

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int kitty_sendesc_hex2(const char *s, unsigned *val)
{
    int hi, lo;

    if (!s) return 0;
    hi = hexval(s[0]);
    if (hi < 0) return 0;      /* also the NUL: s[1] is never read */
    lo = hexval(s[1]);
    if (lo < 0) return 0;
    *val = (unsigned)(hi * 16 + lo);
    return 1;
}
