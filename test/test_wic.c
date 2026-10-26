/*
 * test_wic.c - kitty/kitty_wic.c without a window: a picture written as a
 * PNG (the /screenshot path) and read back (the background image path) pixel
 * for pixel, and the refusals - a PNG handed over as a JPEG (each format only
 * through its own decoder), a cut-off file, a picture over the size limit.
 * Exit 0 = all passed; 2 = this Windows has no imaging component (XP as
 * shipped), nothing judged.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "kitty_wic.h"

static int fails = 0;

/* printf into memory of its own (free it): the paths are held in full */
static char *strf(const char *fmt, ...)
{
    size_t size = 256;
    char *s = NULL;
    int n;
    va_list ap;
    for (;;) {
        char *t = realloc(s, size);
        if (!t) {
            free(s);
            printf("FAIL  out of memory\n");
            exit(1);
        }
        s = t;
        va_start(ap, fmt);
        n = vsnprintf(s, size, fmt, ap);
        va_end(ap);
        if (n >= 0 && (size_t)n < size)
            return s;
        size = n >= 0 ? (size_t)n + 1 : size * 2;
    }
}

/* %TEMP% with its trailing backslash, in full (free it); NULL = not read */
static char *temp_dir(void)
{
    DWORD n = GetTempPathA(0, NULL), got;
    while (n) {
        char *s = malloc(n);
        if (!s)
            return NULL;
        got = GetTempPathA(n, s);
        if (got && got < n)
            return s;
        free(s);
        n = got ? got + 1 : 0;
    }
    return NULL;
}

static void check(bool ok, const char *what)
{
    printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        fails++;
}

#define W 61   /* odd: a row needs padding in a 24 bpp DIB */
#define H 37

static unsigned char pixel(int x, int y, int c)
{
    return (unsigned char)(c == 0 ? x * 4 : c == 1 ? y * 6 : (x * y) & 0xFF);
}

int main(void)
{
    char *dir, *png, *cut;
    BITMAPINFOHEADER bih;
    BYTE *bits = NULL, *back = NULL;
    HBITMAP hbm, loaded;
    BITMAP bm;
    int x, y, stride = ((W * 3) + 3) & ~3, same = 1;
    unsigned char *data, *px = NULL;
    long size;
    int w = 0, h = 0;
    FILE *fp;

    if (FAILED(CoInitialize(NULL)))
        return 1;
    if (!kitty_wic_available()) {
        printf("SKIP  no Windows Imaging Component here\n");
        return 2;
    }
    if ((dir = temp_dir()) == NULL) {
        printf("FAIL  %%TEMP%% not read (%lu)\n", GetLastError());
        return 1;
    }
    png = strf("%stest_wic_%lu.png", dir, GetCurrentProcessId());
    cut = strf("%stest_wic_%lu_cut.png", dir, GetCurrentProcessId());
    free(dir);

    /* a 24 bpp bottom-up DIB with a pattern in every channel */
    memset(&bih, 0, sizeof(bih));
    bih.biSize = sizeof(bih);
    bih.biWidth = W;
    bih.biHeight = H;
    bih.biPlanes = 1;
    bih.biBitCount = 24;
    bih.biCompression = BI_RGB;
    hbm = CreateDIBSection(NULL, (BITMAPINFO *)&bih, DIB_RGB_COLORS,
                           (void **)&bits, NULL, 0);
    if (!hbm || !bits)
        return 1;
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) {
            BYTE *p = bits + (size_t)(H - 1 - y) * stride + x * 3;
            p[0] = pixel(x, y, 0);
            p[1] = pixel(x, y, 1);
            p[2] = pixel(x, y, 2);
        }

    check(kitty_wic_save_png(hbm, png), "a bitmap is written as a PNG");
    loaded = kitty_wic_load_file(png, KITTY_WIC_PNG);
    check(loaded != NULL, "the PNG is read back");
    if (loaded && GetObject(loaded, sizeof(bm), &bm) && bm.bmBits) {
        check(bm.bmWidth == W && bm.bmHeight == H && bm.bmBitsPixel == 24,
              "... at its size, 24 bpp");
        back = bm.bmBits;
        for (y = 0; y < H && same; y++)
            if (memcmp(back + (size_t)y * stride, bits + (size_t)y * stride, W * 3))
                same = 0;
        check(same, "... pixel for pixel");
    } else {
        check(false, "... as a DIB section");
    }
    if (loaded)
        DeleteObject(loaded);

    check(kitty_wic_load_file(png, KITTY_WIC_JPG) == NULL,
          "a PNG handed over as a JPEG is refused (no other decoder tried)");

    /* the file in memory: a cut-off copy, and the size limit */
    fp = fopen(png, "rb");
    data = NULL;
    size = 0;
    if (fp && fseek(fp, 0, SEEK_END) == 0 && (size = ftell(fp)) > 0 &&
        fseek(fp, 0, SEEK_SET) == 0 && (data = malloc(size)) != NULL &&
        fread(data, 1, size, fp) != (size_t)size) {
        free(data);
        data = NULL;
    }
    if (fp)
        fclose(fp);
    check(data != NULL, "the PNG reads back as bytes");
    if (data) {
        check(kitty_wic_decode(KITTY_WIC_PNG, data, size, 4096, 1u << 20,
                               &px, &w, &h) && w == W && h == H,
              "decoding from memory gives its size");
        free(px);
        px = NULL;
        check(!kitty_wic_decode(KITTY_WIC_PNG, data, size, W - 1, 1u << 20,
                                &px, &w, &h) && px == NULL,
              "a side over the limit is refused");
        check(!kitty_wic_decode(KITTY_WIC_PNG, data, size, 4096,
                                (uint64_t)W * H - 1, &px, &w, &h) && px == NULL,
              "a picture over the pixel limit is refused");
        /* Windows' PNG decoder returns what a cut-off file holds (the rest
         * filled): either answer is fine, a crash is not */
        fp = fopen(cut, "wb");
        if (fp) {
            fwrite(data, 1, size / 2, fp);
            fclose(fp);
        }
        loaded = kitty_wic_load_file(cut, KITTY_WIC_PNG);
        printf("NOTE  a cut-off PNG is %s\n", loaded ? "decoded (Windows fills the rest)" : "refused");
        check(true, "a cut-off PNG is handled without a crash");
        if (loaded)
            DeleteObject(loaded);
        /* bytes that are no picture at all */
        {
            size_t i;
            for (i = 0; i < (size_t)size; i++)
                data[i] = (unsigned char)(i * 131 + 7);
        }
        fp = fopen(cut, "wb");
        if (fp) {
            fwrite(data, 1, size, fp);
            fclose(fp);
        }
        loaded = kitty_wic_load_file(cut, KITTY_WIC_PNG);
        check(loaded == NULL, "bytes that are no PNG are refused");
        if (loaded)
            DeleteObject(loaded);
        free(data);
    }

    DeleteFileA(png);
    DeleteFileA(cut);
    free(png);
    free(cut);
    DeleteObject(hbm);
    CoUninitialize();
    printf("%s - %d check(s) failed\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
