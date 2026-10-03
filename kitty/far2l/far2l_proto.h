/*
 * far2l_proto.h - the far2l terminal extensions, the parts that need no window:
 * the request stack, client IDs, the clipboard read gate, chunked uploads, data
 * IDs and the clipboard format conversions. terminal/terminal.c drives them; the
 * Windows clipboard itself is behind seams on the platform side
 * (kitty/kitty_far2l.c), so all of this is covered by test/test_far2l.c.
 *
 * Plain types only: terminal.h embeds the state structs, and terminal.h is
 * included by translation units built with and without MOD_FAR2L, which must
 * all see the same Terminal layout.
 */
#ifndef KITTY_FAR2L_PROTO_H
#define KITTY_FAR2L_PROTO_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Formats as far2l numbers them on the wire. 15 is far2l's HTML, which on
 * Windows is the registered "HTML Format" (15 there is CF_HDROP). */
#define F2L_CF_TEXT          1          /* UTF-8 */
#define F2L_CF_UNICODETEXT   13         /* UTF-32, little-endian */
#define F2L_CF_HTML          15         /* HTML text, UTF-8 */
#define F2L_CF_REGISTERED    0xC000     /* first registered format */

/* What a CLIP_OPEN reply reports below its status. */
#define F2L_FEATCLIP_DATA_ID      0x1
#define F2L_FEATCLIP_CHUNKED_SET  0x2

/* The read gate: a clipboard read is served only this long after a paste
 * gesture, and each served read restarts the period at most this many times. */
#define F2L_READ_WINDOW_MS   5000
#define F2L_READ_PROLONGS    3

/* Remembered client IDs per session, at most. The oldest goes first. */
#define F2L_CLIENT_IDS_MAX   16

/*
 * A request stack, read from the top. The top byte is the LAST byte of the
 * decoded payload; `len` is what is left. Every pop fails cleanly (false or
 * NULL) when the stack holds too little, so a short request can never read
 * outside the buffer.
 */
typedef struct F2lStack {
    const unsigned char *data;
    size_t len;
} F2lStack;

bool f2l_pop_u8(F2lStack *st, uint8_t *v);
bool f2l_pop_u16(F2lStack *st, uint16_t *v);
bool f2l_pop_u32(F2lStack *st, uint32_t *v);
bool f2l_pop_u64(F2lStack *st, uint64_t *v);
/* n raw bytes off the top; NULL if fewer are left. The bytes stay in the
 * caller's buffer. */
const unsigned char *f2l_pop_bytes(F2lStack *st, size_t n);
/* A string: its uint32 length on top, the bytes below it. */
bool f2l_pop_str(F2lStack *st, const unsigned char **p, size_t *n);

/* A reply under construction: the bytes in wire order (bottom of the stack
 * first). Pushes append. */
typedef struct F2lOut {
    unsigned char *data;
    size_t len, size;
} F2lOut;
void f2l_out_init(F2lOut *o);
void f2l_out_free(F2lOut *o);           /* wipes: it can hold clipboard data */
void f2l_push_bytes(F2lOut *o, const void *p, size_t n);
void f2l_push_u8(F2lOut *o, uint8_t v);
void f2l_push_u16(F2lOut *o, uint16_t v);
void f2l_push_u32(F2lOut *o, uint32_t v);
void f2l_push_u64(F2lOut *o, uint64_t v);

/* Client IDs: 32 to 256 characters of 0-9, a-z, '-' and '_'. */
bool f2l_client_id_valid(const unsigned char *id, size_t len);
/* The remembered list, as kept in the session: IDs separated by commas (a
 * comma cannot occur in an ID). NULL and "" are the empty list. */
bool f2l_client_ids_contains(const char *list, const unsigned char *id,
                             size_t len);
/* A new list (caller frees with sfree) with `id` added at the end, the oldest
 * dropped beyond F2L_CLIENT_IDS_MAX; unchanged copy if already listed or if
 * the ID is not valid. */
char *f2l_client_ids_add(const char *list, const unsigned char *id, size_t len);
int f2l_client_ids_count(const char *list);

/*
 * The read gate. Times are a 32-bit millisecond tick counter compared with
 * wrap-around. Unlike upstream it starts CLOSED: a fresh activation reads
 * nothing until the user has pasted.
 */
typedef struct F2lReadGate {
    uint32_t since;            /* the last gesture, or the last prolonged read */
    int prolongs;              /* reads that restarted the period since then */
    bool armed;                /* a gesture has happened at all */
    unsigned gesture;          /* the number of the gesture it was armed from */
} F2lReadGate;
void f2l_gate_reset(F2lReadGate *g);              /* activation: closed */
void f2l_gate_paste(F2lReadGate *g, uint32_t now); /* a paste gesture */
/* Follow the terminal's gesture stamp: `count` numbers the gestures, `tick`
 * is when the last one happened. A number the gate has not armed from yet
 * is a new gesture (two gestures can share a tick). */
void f2l_gate_sync(F2lReadGate *g, bool seen, unsigned count, uint32_t tick);
bool f2l_gate_open(const F2lReadGate *g, uint32_t now);
/* A read was served: restarts the period, at most F2L_READ_PROLONGS times
 * per gesture. Only a new gesture resets the count - not CLIP_CLOSE, or a
 * client cycling open/read/close could keep the gate open for ever. */
void f2l_gate_served(F2lReadGate *g, uint32_t now);

/*
 * Chunked upload: CLIP_SETDATACHUNK pieces collected until the CLIP_SETDATA
 * that ends them. `cap` bounds the total; past it the collection is dropped
 * and `overflow` makes the closing CLIP_SETDATA fail rather than set a part.
 */
typedef struct F2lChunks {
    unsigned char *data;
    size_t len, size;
    bool overflow;
} F2lChunks;
enum {
    F2L_CHUNK_ADDED,
    F2L_CHUNK_DISCARDED,       /* size 0: the client cancelled */
    F2L_CHUNK_SHORT,           /* the request held less than it announced */
    F2L_CHUNK_TOOBIG,          /* past the cap: dropped, overflow set */
};
int f2l_chunks_add(F2lChunks *c, F2lStack *st, size_t cap);
void f2l_chunks_clear(F2lChunks *c);   /* wipes; also clears overflow */

/* The data ID of what a read of `fmt` would hand out: never 0. Text formats
 * count up to their first NUL, so the ID of a set and of the read back agree
 * whether or not a terminator travelled. */
uint64_t f2l_data_id(uint32_t fmt, const unsigned char *data, size_t len);

/* UTF-32LE bytes to UTF-16 units, NUL-terminated (stops at a NUL in the
 * input); *units excludes the terminator. Caller frees. */
uint16_t *f2l_utf32_to_utf16(const unsigned char *p, size_t len, size_t *units);
/* UTF-16 units (up to n or a NUL) to UTF-32LE bytes with a terminating NUL
 * code point; *len includes it. Caller frees. */
unsigned char *f2l_utf16_to_utf32(const uint16_t *s, size_t n, size_t *len);

/* far2l HTML to the Windows "HTML Format" (the header with byte offsets, the
 * text as the fragment), NUL-terminated; *outlen includes the NUL. */
unsigned char *f2l_html_wrap(const unsigned char *html, size_t len,
                             size_t *outlen);
/* The other way: the fragment of an "HTML Format" block, NUL-terminated
 * (*outlen includes the NUL). NULL if the header is not usable. */
unsigned char *f2l_html_unwrap(const unsigned char *cf, size_t len,
                               size_t *outlen);

#endif /* KITTY_FAR2L_PROTO_H */
