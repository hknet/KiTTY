/*
 * keygen-noise.c: Windows implementation of get_random_data() for cmdgen.c.
 */

#include "putty.h"

char *get_random_data(int len, const char *device)
{
    char *buf = snewn(len, char);
    /* device is ignored on Windows; we always use the system CSPRNG */
    if (!win_read_random(buf, len)) {
        sfree(buf);
        fprintf(stderr, "kittygen: failed to read random data from system\n");
        return NULL;
    }
    return buf;
}
