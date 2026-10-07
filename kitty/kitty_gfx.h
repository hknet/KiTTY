/*
 * kitty_gfx.h - the kitty graphics protocol (APC _G), the plain-C core.
 *
 * A client transmits an image (raw RGB/RGBA or PNG, optionally zlib'd,
 * base64 in the APC payload, chunked with m=1) and places it on the text
 * grid; placements scroll with the text. This half has no Windows and no
 * PuTTY Terminal types: the control-data parser, the chunk assembler with
 * its streaming base64 decoder, the image store with its caps and eviction,
 * the placement list anchored to absolute line numbers, the deletion modes,
 * the replies, and the list operations the terminal glue calls when lines
 * move. Decoding (PNG, inflate) comes in through the environment.
 *
 * Supported: a=t/T/p/d/q, t=d, f=24/32/100, o=z, m, q, i, I, p, x/y/w/h,
 * X/Y, c/r, C, z, every d mode, N (ignored). Refused with EINVAL so programs
 * fall back: files and shared memory (t=f/t/s), animation (a=f/a/c, d=f/F),
 * Unicode placeholders (U=1), relative placements (P/Q/H/V).
 *
 * Lines are numbered absolutely: the glue hands over the absolute number of
 * the top screen row (top_abs); a placement keeps its anchor while text
 * scrolls into the scrollback and is dropped when its lines leave it.
 *
 * iTerm2 inline images (OSC 1337 File=, MultipartFile=/FilePart=/FileEnd)
 * share the store: a PNG or JPEG placed at the cursor, sized in cells,
 * pixels or percent of the screen. Such an image is anonymous - no id, no
 * number, out of reach of every by-id command - and is freed with its last
 * placement; the kitty deletions by position apply to it.
 *
 * Sixel pictures (kitty_sixel.h decodes them) are placed the same way, at
 * their own size in pixels (gfx_place_pixels).
 *
 * The iTerm2 and Sixel pictures belong to the cells they cover, as in
 * xterm, mintty and iTerm2 (cell_bound): a character written into one of
 * those cells, or an erase of it, takes that cell's part of the picture
 * away (gfx_mark_cells, called by the terminal glue); the visible list then
 * leaves the cell out, and the picture goes when none of it is left. The
 * kitty placements are drawn over the text and stay until deleted.
 */
#ifndef KITTY_GFX_H
#define KITTY_GFX_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Limits (the store carries copies so a test can lower them). */
#define GFX_MAX_SIDE          8192
#define GFX_MAX_PIXELS        (16u * 1024u * 1024u)
#define GFX_STORE_MAX_BYTES   ((size_t)128u * 1024u * 1024u)
#define GFX_MAX_IMAGES        256
#define GFX_MAX_PLACEMENTS    1024
#define GFX_PENDING_MAX       ((size_t)48u * 1024u * 1024u)
/* The cut-out cells of a cell-bound picture, a bit a cell: at most this many
 * cells a picture, and this many bytes of them in the store. A picture over
 * either goes whole at its first cut-out cell. */
#define GFX_MASK_MAX_CELLS    (2u * 1024u * 1024u)
#define GFX_MASKS_MAX_BYTES   ((size_t)16u * 1024u * 1024u)
/* Pieces of one cut picture in a visible list (gfx_visible_cut). */
#define GFX_CUT_PIECES_MAX    64

#define GFX_REPLY_MAX         192
#define GFX_ERR_MAX           96

/* Keys as bits of GfxCmd.have. */
#define GFX_K_a  (1u << 0)
#define GFX_K_t  (1u << 1)
#define GFX_K_f  (1u << 2)
#define GFX_K_o  (1u << 3)
#define GFX_K_m  (1u << 4)
#define GFX_K_q  (1u << 5)
#define GFX_K_i  (1u << 6)
#define GFX_K_I  (1u << 7)
#define GFX_K_p  (1u << 8)
#define GFX_K_s  (1u << 9)
#define GFX_K_v  (1u << 10)
#define GFX_K_S  (1u << 11)
#define GFX_K_O  (1u << 12)
#define GFX_K_x  (1u << 13)
#define GFX_K_y  (1u << 14)
#define GFX_K_w  (1u << 15)
#define GFX_K_h  (1u << 16)
#define GFX_K_X  (1u << 17)
#define GFX_K_Y  (1u << 18)
#define GFX_K_c  (1u << 19)
#define GFX_K_r  (1u << 20)
#define GFX_K_C  (1u << 21)
#define GFX_K_z  (1u << 22)
#define GFX_K_d  (1u << 23)
#define GFX_K_U  (1u << 24)
#define GFX_K_P  (1u << 25)
#define GFX_K_Q  (1u << 26)
#define GFX_K_H  (1u << 27)
#define GFX_K_V  (1u << 28)
#define GFX_K_N  (1u << 29)

/* The control data of one APC, parsed. Defaults: a='t', t='d', f=32,
 * d='a', the rest 0. */
typedef struct GfxCmd {
    uint32_t have;
    char a, t, o, d;
    uint32_t f, m, q, i, I, p, s, v, S, O, x, y, w, h, X, Y, c, r, C, U,
             P, Q, N;
    int32_t z, H, V;
    const unsigned char *payload;      /* base64, inside the caller's buffer */
    size_t payload_len;
} GfxCmd;

typedef struct GfxImage {
    uint32_t key;              /* internal, unique, never 0 */
    uint32_t id;               /* client id (i), 0: none */
    uint32_t number;           /* client number (I), 0: none */
    int w, h;
    unsigned char *px;         /* premultiplied BGRA, top-down, w * 4 a row */
    bool opaque;
    unsigned long serial;      /* new for every change of the pixels */
    unsigned long last_use;    /* store tick of the last transmit/placement */
} GfxImage;

typedef struct GfxPlacement {
    uint32_t image_key;
    uint32_t image_id;         /* copy, for z ties and deletion by id */
    uint32_t placement_id;     /* p, 0: none */
    int screen;                /* 0 main, 1 alternate */
    long abs_line;             /* anchor: absolute line of the top-left cell */
    int col;
    int cols, rows;            /* cells covered */
    int src_x, src_y, src_w, src_h;   /* source rectangle, image pixels */
    int X, Y;                  /* offset inside the first cell */
    int req_c, req_r;          /* c / r as requested (0: not given) */
    int dst_w, dst_h;          /* the drawn image, pixels */
    int dst_dx, dst_dy;        /* letterbox offset inside the c x r box */
    bool clipped;              /* cut by a region scroll: dst fixed in pixels */
    bool fixed;                /* an iTerm2 image: dst fixed in pixels */
    bool no_cursor;            /* C=1 */
    int32_t z;
    unsigned long serial;      /* creation order, the z tie-break */
    /* an iTerm2 or Sixel picture: its cells' parts go with what is written
     * into them or erased (gfx_mark_cells). mask: a bit per covered cell,
     * row by row (cols a row), set = cut out; NULL until the first one */
    bool cell_bound;
    unsigned char *mask;
    int mask_cols, mask_rows;  /* the mask's shape, kept equal to cols/rows */
    int n_marked;              /* bits set */
} GfxPlacement;

/* A chunked upload in progress (t=d, m=1). */
typedef struct GfxPending {
    bool active;
    GfxCmd cmd;                /* the first chunk's keys (payload unused) */
    unsigned char *buf;
    size_t len, cap;
    unsigned char carry[3];    /* base64 input chars not yet decodable */
    int ncarry;
    bool bad;                  /* a base64 error: refused at the last chunk */
    bool big;                  /* over the pending cap */
    const char *err_code;      /* refused at the first chunk: answered last */
    char err_msg[GFX_ERR_MAX];
} GfxPending;

/* The arguments of an iTerm2 File= / MultipartFile= (gfx_iterm_args).
 * A side's unit: 0 auto, 'c' cells, 'p' pixels, '%' percent of the screen. */
typedef struct GfxItermArgs {
    bool inline_img;           /* inline=1; 0 (the default) is a download */
    bool preserve;             /* preserveAspectRatio, default 1 */
    bool no_move;              /* doNotMoveCursor=1 (WezTerm) */
    bool have_size;
    uint64_t size;             /* size= in bytes, if given */
    char w_unit, h_unit;
    uint32_t w, h;
} GfxItermArgs;

/* A multipart iTerm2 upload in progress (MultipartFile= ... FileEnd). */
typedef struct GfxItermUpload {
    GfxPending data;           /* the bytes; data.active: an upload is open */
    GfxItermArgs args;
    bool discard;              /* inline=0: parts swallowed, nothing placed */
} GfxItermUpload;

typedef struct GfxStore {
    GfxImage *imgs;
    int n_imgs, cap_imgs;
    size_t bytes;              /* decoded bytes held */
    GfxPlacement *pls;
    int n_pls, cap_pls;
    unsigned long serial, tick;
    uint32_t next_key;
    uint32_t next_id;          /* for images with a number but no id */
    GfxPending pend;
    GfxItermUpload iterm;      /* OSC 1337 MultipartFile= */
    int n_cellbound;           /* placements with cell_bound */
    size_t mask_bytes;         /* their masks */
    /* limits, from the defines; a test may lower them */
    int max_side;
    uint32_t max_pixels;
    size_t max_bytes;
    int max_images, max_placements;
    size_t pending_max;
} GfxStore;

/* The picture files an iTerm2 image may be besides PNG (GfxEnv.decode_file). */
#define GFX_FILE_JPEG 1
#define GFX_FILE_GIF  2
#define GFX_FILE_TIFF 3
#define GFX_FILE_BMP  4
#define GFX_FILE_WEBP 5
#define GFX_FILE_HEIF 6   /* HEIC and AVIF */

/* What a command needs from the terminal around it. */
typedef struct GfxEnv {
    int cell_w, cell_h;        /* pixels; <= 0: placements refused (ENOSPC) */
    int cols, rows;            /* the screen */
    int screen;                /* 0 main, 1 alternate */
    long top_abs;              /* absolute line of screen row 0 */
    int cur_x, cur_y;          /* the cursor cell */
    /* an iTerm2 or Sixel picture is not to scroll the screen (mintty's
     * ?7780): > 0, the rows from the cursor row to the bottom margin - a
     * picture taller is cut there - and the cursor stays; 0: off */
    int crop_rows;
    /* PNG to premultiplied BGRA (malloc'd, w * 4 a row). NULL: PNG refused. */
    bool (*decode_png)(void *ctx, const unsigned char *data, size_t len,
                       unsigned char **px, int *w, int *h);
    /* Another picture file (fmt GFX_FILE_*, told by its signature), the
     * same way; only iTerm2 images use it. NULL: those refused. */
    bool (*decode_file)(void *ctx, int fmt, const unsigned char *data,
                        size_t len, unsigned char **px, int *w, int *h);
    /* zlib (RFC 1950) inflate into a malloc'd buffer of at most max_out
     * bytes; more than max_out is a failure. NULL: o=z refused. */
    bool (*inflate)(void *ctx, const unsigned char *in, size_t len,
                    size_t max_out, unsigned char **out, size_t *outlen);
    /* One line per command for a test hook; may be NULL. a is the action,
     * code "OK" or the error code. */
    void (*trace)(void *ctx, char a, uint32_t id, const char *code);
    void *ctx;
} GfxEnv;

typedef struct GfxResult {
    char reply[GFX_REPLY_MAX]; /* the whole APC, ESC _G ... ESC \; "" if none */
    int reply_len;
    int cur_dx, cur_dy;        /* move the cursor right / down by this much */
    bool changed;              /* placements or pixels changed: repaint */
} GfxResult;

/* One visible placement for the painter. */
typedef struct GfxVisible {
    const GfxImage *img;
    const GfxPlacement *pl;
    int c0, r0, c1, r1;        /* cells inside the view, inclusive */
    int dx0, dy0, dx1, dy1;    /* the drawn image, view pixels, unclipped */
    int sx, sy, sw, sh;        /* its source rectangle, image pixels */
    int32_t z;
} GfxVisible;

void gfx_store_init(GfxStore *st);
void gfx_store_free(GfxStore *st);

/* The control data and payload (everything between "G" and ESC \).
 * False: malformed, err holds the EINVAL message. */
bool gfx_parse(const unsigned char *s, size_t len, GfxCmd *cmd,
               char err[GFX_ERR_MAX]);

/* Serve one APC. Returns whether the command was accepted (a chunk with
 * more to come counts as accepted); res carries the reply, if any. */
bool gfx_command(GfxStore *st, const GfxEnv *env, const unsigned char *s,
                 size_t len, GfxResult *res);

/* The APC was cut by the terminal's per-sequence ceiling: its control data
 * is intact, its payload is not. A transmit (or a chunk of one) is taken as
 * over the pending cap and answered EFBIG at its last chunk, anything else
 * EFBIG at once. Returns false. */
bool gfx_command_cut(GfxStore *st, const GfxEnv *env, const unsigned char *s,
                     size_t len, GfxResult *res);

/* Images with no placement, when the image has no id and no number, are
 * freed; capital deletions free the ones they emptied. When the store is
 * full, images without a placement go first, then anonymous images whose
 * every placement is in the scrollback, oldest use first. */

/* ---- iTerm2 inline images (OSC 1337) ----------------------------------- */

/* Whether s (the OSC 1337 string after "1337;") is one of the image
 * commands: File=, MultipartFile=, FilePart=, FileEnd. */
bool gfx_iterm_is_image(const unsigned char *s, size_t len);

/* The ';'-separated key=value list of File= / MultipartFile= (everything
 * before the ':'). Unknown keys are ignored, a malformed value of a known
 * key is false. */
bool gfx_iterm_args(const unsigned char *s, size_t len, GfxItermArgs *a);

/* Serve one OSC 1337 string. False when it is not an image command (left
 * alone). cut: the terminal dropped bytes past its ceiling - the image is
 * refused whole. No reply is ever made; res carries the cursor move and
 * whether to repaint. Traced: 'F' File=, 'E' FileEnd, with OK or the
 * error code. */
bool gfx_iterm(GfxStore *st, const GfxEnv *env, const unsigned char *s,
               size_t len, bool cut, GfxResult *res);

/* ---- Sixel (kitty_sixel.h decodes) ------------------------------------- */

/* Places a decoded picture - premultiplied BGRA, malloc'd, taken over (freed
 * on failure) - as an anonymous image at the cursor, drawn at its own size
 * in pixels; the kitty deletions by position apply to it. res carries the
 * cursor move of a placement (right past it, down to its last row) unless
 * no_cursor, and whether to repaint. Returns an error code (EINVAL: no cell
 * size, EFBIG, ENOSPC) or NULL. Traced 'S'. */
const char *gfx_place_pixels(GfxStore *st, const GfxEnv *env,
                             unsigned char *px, int w, int h, bool no_cursor,
                             GfxResult *res);

/* ---- line movement, called by the terminal glue ------------------------ */

/* Lines before base_abs are gone (scrolled off the scrollback, scrollback
 * cleared): placements entirely above it are dropped. */
void gfx_set_base(GfxStore *st, int screen, long base_abs);

/* Screen rows 0..bot scrolled up by n lines into the scrollback, top_abs
 * grew by n: placements ending inside the band (or in the scrollback) keep
 * their anchor, those reaching below it follow their screen row (+n). */
void gfx_scroll_region_sb(GfxStore *st, int screen, long top_abs_after,
                          int bot, int n);

/* Screen rows top..bot scrolled by n (> 0 up, < 0 down) with nothing
 * entering the scrollback: placements entirely inside the band move,
 * those leaving it are clipped or dropped. to_bottom: bot is the screen's
 * last row - a placement starting in the band and reaching below the
 * screen (an image placed near the bottom) moves with the band too, and
 * is not cut at the bottom while the band scrolls up. */
void gfx_scroll_region(GfxStore *st, int screen, long top_abs, int top,
                       int bot, int n, int cell_w, int cell_h,
                       bool to_bottom);

/* A screen row was deleted outright (a resize that cut the bottom): the
 * placements starting at or below abs_line are dropped. */
void gfx_clear_below(GfxStore *st, int screen, long abs_line);

/* ED 2 and a=d without keys: every placement not entirely above the screen. */
void gfx_clear_screen(GfxStore *st, int screen, long top_abs);

/* Every placement of one screen (alternate screen entered / left). */
void gfx_clear_all(GfxStore *st, int screen);

/* Full reset: placements and images gone, a pending upload aborted. */
void gfx_reset(GfxStore *st);

/* The cell size changed: placements given in cells are re-fitted. */
void gfx_rescale(GfxStore *st, int cell_w, int cell_h);

/* The placements that touch the view (rows x cols cells from top_abs),
 * ordered for painting: z, then image id, then creation. Returns the count
 * (at most max written). A cell-bound picture with cells cut out counts
 * whole here (the cells to repaint). */
int gfx_visible(const GfxStore *st, int screen, long top_abs, int rows,
                int cols, int cell_w, int cell_h, GfxVisible *out, int max);

/* The same for the painters: a cell-bound picture with cells cut out comes
 * in pieces that leave those cells out - a band of rows with none cut out
 * as one piece, each run of kept cells of the other rows as one - each with
 * its part of the source rectangle (edges from one mapping, so neighbouring
 * pieces meet without a seam). More than GFX_CUT_PIECES_MAX pieces, or more
 * than max has room for: only the bands of whole rows, and if those do not
 * fit either the picture is left out. Never a piece over a cut-out cell. */
int gfx_visible_cut(const GfxStore *st, int screen, long top_abs, int rows,
                    int cols, int cell_w, int cell_h, GfxVisible *out,
                    int max);

/* ---- cells of the cell-bound pictures ---------------------------------- */

/* Cells c0..c1 of absolute lines line0..line1 (inclusive) of a screen were
 * written into or erased: the cell-bound pictures there lose those cells,
 * and a picture with none left goes (its anonymous image with it). Returns
 * whether a picture changed. */
bool gfx_mark_cells(GfxStore *st, int screen, long line0, long line1,
                    int c0, int c1);

/* Whether cell (c, r) of a placement (relative to its top-left) is cut out. */
bool gfx_cell_marked(const GfxPlacement *pl, int c, int r);

/* ---- helpers also used by the test ------------------------------------- */

const GfxImage *gfx_image_by_id(const GfxStore *st, uint32_t id);
const GfxImage *gfx_image_by_key(const GfxStore *st, uint32_t key);

/* Raw pixels (bpp 3: R G B, 4: R G B A) to premultiplied BGRA. Returns
 * whether every pixel was opaque. */
bool gfx_to_bgra(const unsigned char *src, int bpp, size_t npx,
                 unsigned char *dst);

/* Streaming base64: decodes s into out (room for len * 3 / 4 + 3), carrying
 * 0-3 unconsumed input chars across calls. False on a bad character. */
bool gfx_b64_feed(const unsigned char *s, size_t len, unsigned char *carry,
                  int *ncarry, unsigned char *out, size_t *outlen);

#endif /* KITTY_GFX_H */
