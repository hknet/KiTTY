/*
 * kitty_wic.h - images through the Windows Imaging Component (kitty_wic.c).
 *
 * Every picture KiTTY++ reads or writes besides BMP goes through here: far2l
 * images (kitty_far2l_image_term.c), the background image and the
 * /screenshot command (kitty_image.c). WIC is created at run time through
 * COM, so a Windows without it (XP unless .NET 3.0 or the WIC redistributable
 * is installed) only reports these formats unavailable. The caller's thread
 * has COM initialised (windows/window.c does it at start-up).
 */
#ifndef KITTY_WIC_H
#define KITTY_WIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KITTY_WIC_PNG 1
#define KITTY_WIC_JPG 2

/* Can PNG and JPEG be decoded on this Windows? */
bool kitty_wic_available(void);

/* Decode data of format fmt (KITTY_WIC_PNG or KITTY_WIC_JPG) with Windows'
 * own decoder of that format, never another codec WIC would pick from the
 * content. On success *px is 32 bpp premultiplied BGRA, top-down, w * 4 bytes
 * a row (malloc'd, the caller frees it); refused when a side exceeds max_side
 * or the picture max_pixels. */
bool kitty_wic_decode(int fmt, const unsigned char *data, size_t len,
                      unsigned max_side, uint64_t max_pixels,
                      unsigned char **px, int *w, int *h);

#ifdef _WIN32
#include <windows.h>
/* A file of format fmt, read whole (at most 256 MB) and decoded as above,
 * as a 24 bpp bottom-up DIB section; NULL when it cannot be. */
HBITMAP kitty_wic_load_file(const char *path, int fmt);

/* hbm written to path as a PNG (24 bpp, no alpha). */
bool kitty_wic_save_png(HBITMAP hbm, const char *path);
#endif

#endif
