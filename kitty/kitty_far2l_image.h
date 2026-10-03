/*
 * kitty_far2l_image.h - far2l terminal extensions: images (request 'i').
 *
 * far2l places pictures over the text grid: it uploads the pixels (raw RGB,
 * RGBA, or a PNG/JPEG file), says which cells they cover, and later moves,
 * rotates, mirrors or deletes them by an identity string of its choosing.
 * The images are an overlay: they stay where they were put until deleted and
 * do not scroll with the text under them.
 *
 * This half is plain C with no Windows headers: the request parser (the
 * stack of the far2l request, popped from its end), the store with its size
 * cap, the geometry and the pixel transforms, so the unit test builds it on
 * its own (test/test_far2l_images.c). The terminal glue (Terminal, the image
 * decoder, the cell size, repainting) is kitty_far2l_image_term.c; the
 * painters draw what the store holds (windows/paint.h, overlay).
 *
 * Request stacks, top (popped first) to bottom, after the request ID and the
 * command letter 'i':
 *   'c'  caps                    reply: caps u64, cell width i16, cell height i16
 *   's'  set: id string, flags u64, left i16, top i16, right i16, bottom i16,
 *        width u32, height u32, data      reply: success u8
 *   't'  transform: id string, left, top, right, bottom i16, transform u16
 *                                          reply: success u8
 *   'd'  delete: id string                 reply: success u8
 * A string is its bytes with a u32 length on top of them. Replies list the
 * bottom of the stack first; the caller puts the request ID on top.
 */
#ifndef KITTY_FAR2L_IMAGE_H
#define KITTY_FAR2L_IMAGE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Capabilities (reply to 'c'). JPG is not a bit of its own: 3 = RGBA | PNG. */
#define F2L_IMGCAP_RGBA         0x001u
#define F2L_IMGCAP_PNG          0x002u
#define F2L_IMGCAP_JPG          0x003u
#define F2L_IMGCAP_ATTACH       0x100u
#define F2L_IMGCAP_SCROLL       0x200u
#define F2L_IMGCAP_ROTMIR       0x800u

/* Flags of 'set'. The format is a number in the low 16 bits, the attach
 * mode a number in bits 16-18. */
#define F2L_IMG_RGBA            0u
#define F2L_IMG_RGB             1u
#define F2L_IMG_PNG             2u
#define F2L_IMG_JPG             3u
#define F2L_IMG_MASK_FMT        0x00FFFFu
#define F2L_IMG_MASK_ATTACH     0x070000u
#define F2L_IMG_SCROLL          0x080000u
#define F2L_IMG_PIXEL_OFFSET    0x100000u

/* Transforms of 't': the rotation is a number, mirroring is applied first. */
#define F2L_IMGTF_MASK_ROTATE   0x03u
#define F2L_IMGTF_ROTATE90      0x01u
#define F2L_IMGTF_ROTATE180     0x02u
#define F2L_IMGTF_ROTATE270     0x03u
#define F2L_IMGTF_MIRROR_H      0x04u
#define F2L_IMGTF_MIRROR_V      0x08u

/* Limits. One image at most 4096 pixels a side and 8 Mpixel (32 MB
 * decoded); all images of a terminal together at most 64 MB decoded, and at
 * most 64 of them; an identity at most 256 bytes. A request beyond a limit
 * is answered 0 and changes nothing. */
#define F2L_IMG_MAX_SIDE        4096
#define F2L_IMG_MAX_PIXELS      (8u * 1024u * 1024u)
#define F2L_IMG_STORE_MAX_BYTES ((size_t)64u * 1024u * 1024u)
#define F2L_IMG_MAX_COUNT       64
#define F2L_IMG_MAX_ID          256

/* The longest reply, request ID slot included. */
#define F2L_IMG_REPLY_MAX       16

typedef struct Far2lImage {
    char *id;
    size_t idlen;
    int w, h;                  /* pixels, as shown (after any rotation) */
    unsigned char *px;         /* premultiplied BGRA, top-down, w * 4 a row */
    bool opaque;               /* every alpha is 255 */
    bool pixel_offset;         /* F2L_IMG_PIXEL_OFFSET: own size, right/bottom shift */
    int16_t left, top, right, bottom;
    unsigned long serial;      /* new for every change of the pixels */
} Far2lImage;

typedef struct Far2lImageStore {
    Far2lImage *imgs;
    int n, size;
    size_t bytes;              /* decoded bytes held, against the cap */
    unsigned long serial;      /* the last serial handed out */
} Far2lImageStore;

/* A rectangle in pixels, right and bottom exclusive; empty when x1 <= x0 or
 * y1 <= y0. Relative to the top-left corner of cell (0, 0). */
typedef struct Far2lRect {
    int x0, y0, x1, y1;
} Far2lRect;

/* What a request needs from the terminal around it. */
typedef struct Far2lImageEnv {
    bool enabled;              /* "Show far2l images", and a cell size known */
    int cell_w, cell_h;        /* pixels */
    int cur_x, cur_y;          /* the cursor cell, for a new image at -1/-1 */
    unsigned codecs;           /* F2L_IMGCAP_PNG when PNG and JPEG decode */
    /* The most image data (raw pixels or the file) one set may carry: what
     * fits the terminal's far2l payload ceiling once base64'd. 0: no limit
     * beyond the ones above. A larger set is answered 0. */
    size_t max_data;
    /* Decode a file of format fmt (F2L_IMG_PNG or F2L_IMG_JPG, and nothing
     * else: data of another format is refused) into premultiplied BGRA
     * (malloc'd, w * 4 a row), within F2L_IMG_MAX_SIDE / F2L_IMG_MAX_PIXELS. */
    bool (*decode)(void *ctx, unsigned fmt, const unsigned char *data,
                   size_t len, unsigned char **px, int *w, int *h);
    /* An image changed: what it covered before and covers now (either may be
     * empty); the terminal repaints both. */
    void (*changed)(void *ctx, const Far2lRect *before, const Far2lRect *after);
    /* One line per request for a test hook; may be NULL. op is 'c', 's',
     * 't', 'd'; ok is 1, 0 (refused) or -1 (malformed: the ID alone, id
     * may be NULL); img is the image afterwards (NULL: none); caps for 'c'. */
    void (*trace)(void *ctx, char op, const char *id, size_t idlen, int ok,
                  const Far2lImage *img, uint64_t caps);
    void *ctx;
} Far2lImageEnv;

/*
 * Serve one image request. stk/len is the decoded request stack without its
 * request ID and command letter (the sub-command letter is its last byte).
 * The reply goes into out, bottom of the stack first, with one byte left
 * free at its end for the request ID; returns the reply length counting that
 * byte (1 = the ID alone: a malformed request or an unknown sub-command).
 */
int far2l_img_request(Far2lImageStore *st, const Far2lImageEnv *env,
                      const unsigned char *stk, size_t len,
                      unsigned char out[F2L_IMG_REPLY_MAX]);

/* Remove every image (env may be NULL: nothing is told). */
void far2l_img_store_clear(Far2lImageStore *st, const Far2lImageEnv *env);

/* The capabilities 'c' reports for this environment. */
uint64_t far2l_img_caps(const Far2lImageEnv *env);

/* Where an image is drawn, for cells of cw x ch pixels. False (and an empty
 * rectangle) when it covers nothing. */
bool far2l_img_rect(const Far2lImage *img, int cw, int ch, Far2lRect *r);

/* Raw pixels (bpp 3: R G B, 4: R G B A) to premultiplied BGRA. Returns
 * whether every pixel was opaque. */
bool far2l_img_to_bgra(const unsigned char *src, int bpp, size_t npx,
                       unsigned char *dst);

/* Mirror (first) and rotate premultiplied BGRA pixels; *px is replaced and
 * *w / *h swapped for a quarter turn. False when out of memory (nothing
 * changed). */
bool far2l_img_transform_px(unsigned char **px, int *w, int *h, unsigned tf);

/* The cells a pixel rectangle touches (inclusive), for repainting. False for
 * an empty rectangle. */
bool far2l_img_rect_cells(const Far2lRect *r, int cw, int ch,
                          int *c0, int *r0, int *c1, int *r1);

#endif /* KITTY_FAR2L_IMAGE_H */
