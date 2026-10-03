/*
 * test_far2l - the far2l terminal extensions without a window
 * (kitty/far2l/far2l_proto.c): the request stack against the byte examples of
 * the protocol description, client IDs and the remembered list, the clipboard
 * read gate's timing, chunked upload, data IDs and the clipboard format
 * conversions. The end-to-end request handling through terminal.c is in
 * test_osc52 (test_far2l_protocol).
 *
 * Returns non-zero on any failure.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "putty.h"
#include "far2l_proto.h"

const char *appname = "test_far2l";

void modalfatalbox(const char *p, ...)
{
    va_list ap;
    fprintf(stderr, "FATAL ERROR: ");
    va_start(ap, p);
    vfprintf(stderr, p, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static int failures = 0;

static void check(bool ok, const char *what)
{
    if (!ok) {
        printf("FAIL %s\n", what);
        failures++;
    }
}

static bool out_is(const F2lOut *o, const unsigned char *want, size_t n)
{
    return o->len == n && !memcmp(o->data, want, n);
}

/* ---------------------------------------------------------------- stack --- */

static void test_stack(void)
{
    /* the open request of the protocol description (7.5): ID 1, 'c', 'o',
     * then the 32-character client ID as a string */
    static const unsigned char open_req[] = {
        '0','1','2','3','4','5','6','7','8','9','a','b','c','d','e','f',
        '0','1','2','3','4','5','6','7','8','9','a','b','c','d','e','f',
        0x20, 0x00, 0x00, 0x00, 'o', 'c', 0x01 };
    F2lStack st = { open_req, sizeof(open_req) };
    uint8_t id, cmd, sub;
    const unsigned char *s;
    size_t n;
    uint32_t v32;

    check(f2l_pop_u8(&st, &id) && id == 1, "stack: the ID is on top");
    check(f2l_pop_u8(&st, &cmd) && cmd == 'c', "stack: the command below it");
    check(f2l_pop_u8(&st, &sub) && sub == 'o', "stack: the sub-command");
    check(f2l_pop_str(&st, &s, &n) && n == 32 &&
          !memcmp(s, "0123456789abcdef0123456789abcdef", 32) && st.len == 0,
          "stack: the client ID string");
    check(f2l_client_id_valid(s, n), "client ID of the example is valid");

    /* popping past the bottom fails and reads nothing */
    check(!f2l_pop_u8(&st, &id), "stack: pop from empty fails");
    {
        static const unsigned char shortstr[] = { 'a', 0x05, 0x00, 0x00, 0x00 };
        F2lStack s2 = { shortstr, sizeof(shortstr) };
        check(!f2l_pop_str(&s2, &s, &n) && s2.len == sizeof(shortstr),
              "stack: a string longer than the stack fails and pops nothing");
        check(!f2l_pop_bytes(&s2, 6), "stack: too many raw bytes fail");
        check(f2l_pop_u32(&s2, &v32) && v32 == 5, "stack: u32 little-endian");
    }

    /* replies, against the description's bytes */
    {
        static const unsigned char maxsize[] = { 0xc8, 0x00, 0x32, 0x00, 0x01 };
        static const unsigned char setreply[] = {
            0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x01, 0x02 };
        static const unsigned char openreply[] = {
            0x03, 0, 0, 0, 0, 0, 0, 0, 0x01, 0x01 };
        F2lOut o;
        f2l_out_init(&o);
        f2l_push_u16(&o, 200);         /* width below */
        f2l_push_u16(&o, 50);          /* height on top */
        f2l_push_u8(&o, 1);
        check(out_is(&o, maxsize, sizeof(maxsize)), "reply: GET_WINDOW_MAXSIZE bytes");
        f2l_out_free(&o);
        f2l_push_u64(&o, 0x1122334455667788ULL);
        f2l_push_u8(&o, 1);
        f2l_push_u8(&o, 2);
        check(out_is(&o, setreply, sizeof(setreply)), "reply: CLIP_SETDATA bytes");
        f2l_out_free(&o);
        f2l_push_u64(&o, F2L_FEATCLIP_DATA_ID | F2L_FEATCLIP_CHUNKED_SET);
        f2l_push_u8(&o, 1);
        f2l_push_u8(&o, 1);
        check(out_is(&o, openreply, sizeof(openreply)), "reply: CLIP_OPEN bytes");
        f2l_out_free(&o);
    }
}

/* ----------------------------------------------------------- client IDs --- */

static void test_client_ids(void)
{
    char id[300];
    char *list, *l2;
    int i;

    memset(id, 'a', sizeof(id));
    check(!f2l_client_id_valid((unsigned char *)id, 31), "client ID: 31 is too short");
    check(f2l_client_id_valid((unsigned char *)id, 32), "client ID: 32 is valid");
    check(f2l_client_id_valid((unsigned char *)id, 256), "client ID: 256 is valid");
    check(!f2l_client_id_valid((unsigned char *)id, 257), "client ID: 257 is too long");
    memcpy(id, "host-name_0123456789", 20);
    check(f2l_client_id_valid((unsigned char *)id, 40), "client ID: - and _ are valid");
    id[3] = 'A';
    check(!f2l_client_id_valid((unsigned char *)id, 40), "client ID: upper case is not");
    id[3] = ',';
    check(!f2l_client_id_valid((unsigned char *)id, 40), "client ID: a comma is not");
    id[3] = 't';

    list = f2l_client_ids_add("", (unsigned char *)id, 40);
    check(f2l_client_ids_count(list) == 1 &&
          f2l_client_ids_contains(list, (unsigned char *)id, 40),
          "client IDs: added to the empty list");
    check(!f2l_client_ids_contains(list, (unsigned char *)id, 39),
          "client IDs: a prefix is not a match");
    l2 = f2l_client_ids_add(list, (unsigned char *)id, 40);
    check(!strcmp(l2, list), "client IDs: no duplicate");
    sfree(l2);
    l2 = f2l_client_ids_add(list, (unsigned char *)"BAD", 3);
    check(!strcmp(l2, list), "client IDs: an invalid ID is not added");
    sfree(l2);
    sfree(list);

    /* the oldest goes first past the cap */
    list = dupstr(",,");
    for (i = 0; i < F2L_CLIENT_IDS_MAX + 3; i++) {
        char *nl;
        sprintf(id, "client%02d-abcdefghijklmnopqrstuvwxyz", i);
        nl = f2l_client_ids_add(list, (unsigned char *)id, strlen(id));
        sfree(list);
        list = nl;
    }
    check(f2l_client_ids_count(list) == F2L_CLIENT_IDS_MAX, "client IDs: capped");
    sprintf(id, "client%02d-abcdefghijklmnopqrstuvwxyz", 0);
    check(!f2l_client_ids_contains(list, (unsigned char *)id, strlen(id)),
          "client IDs: the oldest was dropped");
    sprintf(id, "client%02d-abcdefghijklmnopqrstuvwxyz", F2L_CLIENT_IDS_MAX + 2);
    check(f2l_client_ids_contains(list, (unsigned char *)id, strlen(id)),
          "client IDs: the newest is kept");
    check(list[0] != ',' && !strstr(list, ",,"), "client IDs: no empty entries");
    sfree(list);
}

/* ------------------------------------------------------------ read gate --- */

static void test_gate(void)
{
    F2lReadGate g;
    f2l_gate_reset(&g);
    check(!f2l_gate_open(&g, 0) && !f2l_gate_open(&g, 1000),
          "gate: closed before any paste (even at tick 0)");
    f2l_gate_paste(&g, 1000);
    check(f2l_gate_open(&g, 1000) && f2l_gate_open(&g, 5999), "gate: open for 5 s");
    check(!f2l_gate_open(&g, 6000), "gate: closed after 5 s");

    /* each served read restarts the period, three times at most */
    f2l_gate_paste(&g, 10000);
    f2l_gate_served(&g, 14000);
    check(f2l_gate_open(&g, 18999), "gate: a read prolongs");
    f2l_gate_served(&g, 18000);
    f2l_gate_served(&g, 22000);
    check(f2l_gate_open(&g, 26999), "gate: three prolongs");
    f2l_gate_served(&g, 26000);
    check(!f2l_gate_open(&g, 27000), "gate: a fourth read does not prolong");
    f2l_gate_paste(&g, 30000);
    f2l_gate_served(&g, 34000);
    check(f2l_gate_open(&g, 38000), "gate: a new paste resets the count");

    /* across the wrap of the tick counter */
    f2l_gate_paste(&g, 0xFFFFF000U);
    check(f2l_gate_open(&g, 0x00000100U), "gate: open across the wrap");
    check(!f2l_gate_open(&g, 0x00000400U), "gate: closes across the wrap");

    f2l_gate_reset(&g);
    check(!f2l_gate_open(&g, 0x00000100U), "gate: a new activation is closed");

    /* following the terminal's gesture stamp */
    f2l_gate_sync(&g, false, 1, 50000);
    check(!f2l_gate_open(&g, 50001), "gate sync: no stamp, no gesture");
    f2l_gate_sync(&g, true, 1, 50000);
    check(f2l_gate_open(&g, 54999), "gate sync: a stamp arms it");
    f2l_gate_served(&g, 54000);
    f2l_gate_served(&g, 58000);
    f2l_gate_served(&g, 62000);
    f2l_gate_sync(&g, true, 1, 50000);
    check(f2l_gate_open(&g, 66999) && g.prolongs == 3,
          "gate sync: the same stamp again is not a new gesture");
    f2l_gate_served(&g, 66000);
    check(!f2l_gate_open(&g, 67000), "gate sync: the prolong limit holds");
    f2l_gate_sync(&g, true, 2, 66000);
    check(f2l_gate_open(&g, 70999) && g.prolongs == 0,
          "gate sync: a new gesture in the same tick is still new");
}

/* --------------------------------------------------------------- chunks --- */

static int chunk(F2lChunks *c, uint16_t enc, size_t have, unsigned char fill,
                 size_t cap)
{
    unsigned char *buf = snewn(have + 2, unsigned char);
    F2lStack st;
    int r;
    memset(buf, fill, have);
    buf[have] = (unsigned char)enc;
    buf[have + 1] = (unsigned char)(enc >> 8);
    st.data = buf;
    st.len = have + 2;
    r = f2l_chunks_add(c, &st, cap);
    sfree(buf);
    return r;
}

static void test_chunks(void)
{
    F2lChunks c = { 0 };
    check(chunk(&c, 0x40, 0x4000, 'a', 1 << 20) == F2L_CHUNK_ADDED && c.len == 0x4000,
          "chunks: a 16 KiB piece");
    check(chunk(&c, 1, 256, 'b', 1 << 20) == F2L_CHUNK_ADDED && c.len == 0x4100 &&
          c.data[0x4000] == 'b', "chunks: appended in order");
    check(chunk(&c, 0, 0, 0, 1 << 20) == F2L_CHUNK_DISCARDED && c.len == 0 &&
          !c.overflow, "chunks: size 0 discards");
    check(chunk(&c, 2, 256, 'c', 1 << 20) == F2L_CHUNK_SHORT && c.overflow && c.len == 0,
          "chunks: a short piece loses the upload");
    f2l_chunks_clear(&c);
    check(chunk(&c, 4, 1024, 'd', 1024) == F2L_CHUNK_ADDED, "chunks: up to the cap");
    check(chunk(&c, 1, 256, 'e', 1024) == F2L_CHUNK_TOOBIG && c.overflow && c.len == 0,
          "chunks: past the cap");
    check(chunk(&c, 1, 256, 'f', 1 << 20) == F2L_CHUNK_TOOBIG && c.len == 0,
          "chunks: nothing collected after an overflow");
    check(chunk(&c, 0, 0, 0, 1 << 20) == F2L_CHUNK_DISCARDED && !c.overflow,
          "chunks: a cancel ends the overflow");
    f2l_chunks_clear(&c);
}

/* -------------------------------------------------------------- data ID --- */

static void test_data_id(void)
{
    uint64_t a = f2l_data_id(F2L_CF_TEXT, (const unsigned char *)"hello", 5);
    static const unsigned char u32[] = { 'h',0,0,0, 'i',0,0,0, 0,0,0,0, 'x',0,0,0 };
    check(a != 0, "data ID: never 0");
    check(f2l_data_id(F2L_CF_TEXT, NULL, 0) != 0, "data ID: not 0 for no data either");
    check(a == f2l_data_id(F2L_CF_TEXT, (const unsigned char *)"hello\0junk", 10),
          "data ID: text stops at the NUL");
    check(a != f2l_data_id(F2L_CF_TEXT, (const unsigned char *)"hellp", 5),
          "data ID: differs with the data");
    check(f2l_data_id(F2L_CF_UNICODETEXT, u32, 16) == f2l_data_id(F2L_CF_UNICODETEXT, u32, 8),
          "data ID: UTF-32 text stops at the NUL code point");
    check(f2l_data_id(0xC001, (const unsigned char *)"ab\0", 3) !=
          f2l_data_id(0xC001, (const unsigned char *)"ab", 2),
          "data ID: a registered format counts every byte");
}

/* ---------------------------------------------------------- conversions --- */

static void test_conversions(void)
{
    /* "a", U+1F600, an invalid code point, then a NUL and something after it */
    static const unsigned char in[] = {
        'a',0,0,0, 0x00,0xF6,0x01,0x00, 0x00,0x00,0x11,0x00, 0,0,0,0, 'z',0,0,0 };
    size_t units, len;
    uint16_t *u = f2l_utf32_to_utf16(in, sizeof(in), &units);
    unsigned char *back;
    check(units == 4 && u[0] == 'a' && u[1] == 0xD83D && u[2] == 0xDE00 &&
          u[3] == 0xFFFD && u[4] == 0, "UTF-32 to UTF-16: pairs, FFFD, stop at NUL");
    back = f2l_utf16_to_utf32(u, units, &len);
    check(len == 16 && !memcmp(back, in, 8) && back[8] == 0xFD && back[9] == 0xFF &&
          !memcmp(back + 12, "\0\0\0\0", 4), "UTF-16 to UTF-32: back, NUL-terminated");
    sfree(back);
    sfree(u);

    {
        static const char html[] = "<b>bold</b> &amp; text";
        size_t n, m;
        unsigned char *cf = f2l_html_wrap((const unsigned char *)html, sizeof(html), &n);
        unsigned char *frag;
        check(cf && n == strlen((char *)cf) + 1 &&
              !memcmp(cf, "Version:0.9\r\nStartHTML:0000000105\r\n", 35),
              "HTML: the Windows header");
        frag = cf ? f2l_html_unwrap(cf, n, &m) : NULL;
        check(frag && m == sizeof(html) && !strcmp((char *)frag, html),
              "HTML: the fragment comes back as it went");
        sfree(frag);
        sfree(cf);
    }
    {
        /* as a browser writes it */
        static const char cf[] =
            "Version:0.9\r\nStartHTML:0000000105\r\nEndHTML:0000000178\r\n"
            "StartFragment:0000000139\r\nEndFragment:0000000144\r\n"
            "<html><body>\r\n<!--StartFragment-->hello<!--EndFragment-->\r\n</body></html>";
        size_t m;
        unsigned char *frag = f2l_html_unwrap((const unsigned char *)cf, sizeof(cf) - 1, &m);
        check(frag && !strcmp((char *)frag, "hello") && m == 6, "HTML: a browser's fragment");
        sfree(frag);
        frag = f2l_html_unwrap((const unsigned char *)"<html>x</html>", 14, &m);
        check(!frag, "HTML: no header, no data");
        sfree(frag);
    }
}

int main(void)
{
    test_stack();
    test_client_ids();
    test_gate();
    test_chunks();
    test_data_id();
    test_conversions();
    if (failures) {
        printf("Test suite FAILED (%d failure%s)\n", failures, failures == 1 ? "" : "s");
        return 1;
    }
    printf("Test suite passed\n");
    return 0;
}
