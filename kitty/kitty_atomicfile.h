#ifndef KITTY_ATOMICFILE_H
#define KITTY_ATOMICFILE_H

#include <stddef.h>

/* Replace the file `path` with `len` bytes of `data`, whole or not at all
 * (kitty_atomicfile.c). 1 written; 0 not, the old file untouched and
 * Windows' reason in GetLastError(). */
int kitty_write_file_whole(const char *path, const void *data, size_t len);

#endif
