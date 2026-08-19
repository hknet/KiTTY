/*
 * bench_term - feed a file through term_data() and report the time.
 *
 * Built for Windows so that mingw and MSVC can be compared on identical source.
 * No window, no network: the mock TermWin does nothing, exactly as
 * test/fuzzterm.c does, so what is measured is terminal.c and nothing else.
 *
 *   bench_term <file>
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdarg.h>

#include "putty.h"
#include "terminal.h"

static const TermWinVtable bench_termwin_vt;

/* The three symbols terminal.c needs from whatever program hosts it. Same set
 * test_terminal.c provides, and for the same reason: this is a host, not a
 * terminal application. appname is not const-qualified in this fork because
 * KiClassName rewrites it at runtime. */
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
const char *appname = "bench_term";
const struct BackendVtable *const backends[] = { NULL };

int main(int argc, char **argv)
{
    char blk[4096];
    size_t len, total = 0;
    Terminal *term;
    Conf *conf;
    struct unicode_data ucsdata;
    TermWin termwin;
    FILE *fp;
    clock_t t0, t1;

    if (argc < 2) {
        fprintf(stderr, "usage: bench_term <file>\n");
        return 2;
    }
    fp = fopen(argv[1], "rb");
    if (!fp) {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }

    termwin.vt = &bench_termwin_vt;
    conf = conf_new();
    do_defaults(NULL, conf);
    init_ucs_generic(conf, &ucsdata);
    term = term_init(conf, &ucsdata, &termwin);
    term_size(term, 24, 80, 2000);
    term->ldisc = NULL;

    t0 = clock();
    while (!feof(fp)) {
        len = fread(blk, 1, sizeof(blk), fp);
        if (len == 0) break;
        term_data(term, blk, len);
        total += len;
    }
    term_update(term);
    t1 = clock();

    printf("%.3f s for %.2f MB (%.0f KB/s)\n",
           (double)(t1 - t0) / CLOCKS_PER_SEC,
           (double)total / (1024.0 * 1024.0),
           (double)total / 1024.0 / ((double)(t1 - t0) / CLOCKS_PER_SEC));

    term_free(term);
    conf_free(conf);
    fclose(fp);
    return 0;
}

/* ---- the do-nothing window, same as fuzzterm's ------------------------- */
static bool bench_setup_draw_ctx(TermWin *tw) { return true; }
static void bench_draw_text(TermWin *tw, int x, int y, wchar_t *text, int len,
                            unsigned long attr, int lattr, truecolour tc) {}
static void bench_draw_cursor(TermWin *tw, int x, int y, wchar_t *text, int len,
                              unsigned long attr, int lattr, truecolour tc) {}
static void bench_draw_trust_sigil(TermWin *tw, int x, int y) {}
static int  bench_char_width(TermWin *tw, int uc) { return 1; }
static void bench_free_draw_ctx(TermWin *tw) {}
static void bench_set_cursor_pos(TermWin *tw, int x, int y) {}
static void bench_set_raw_mouse_mode(TermWin *tw, bool enable) {}
static void bench_set_raw_mouse_mode_pointer(TermWin *tw, bool enable) {}
static void bench_set_scrollbar(TermWin *tw, int total, int start, int page) {}
static void bench_bell(TermWin *tw, int mode) {}
static void bench_clip_write(TermWin *tw, int clipboard, wchar_t *text,
                             int *attrs, truecolour *colours, int len,
                             bool must_deselect) {}
static void bench_clip_request_paste(TermWin *tw, int clipboard) {}
static void bench_refresh(TermWin *tw) {}
static void bench_request_resize(TermWin *tw, int w, int h) {}
static void bench_set_title(TermWin *tw, const char *title, int codepage) {}
static void bench_set_icon_title(TermWin *tw, const char *icontitle, int codepage) {}
static void bench_set_minimised(TermWin *tw, bool minimised) {}
static void bench_set_maximised(TermWin *tw, bool maximised) {}
static void bench_move(TermWin *tw, int x, int y) {}
static void bench_set_zorder(TermWin *tw, bool top) {}
static void bench_palette_set(TermWin *tw, unsigned start, unsigned ncolours,
                              const rgb *colours) {}
static void bench_palette_get_overrides(TermWin *tw, Terminal *term) {}
static void bench_unthrottle(TermWin *tw, size_t bufsize) {}

static const TermWinVtable bench_termwin_vt = {
    .setup_draw_ctx = bench_setup_draw_ctx,
    .draw_text = bench_draw_text,
    .draw_cursor = bench_draw_cursor,
    .draw_trust_sigil = bench_draw_trust_sigil,
    .char_width = bench_char_width,
    .free_draw_ctx = bench_free_draw_ctx,
    .set_cursor_pos = bench_set_cursor_pos,
    .set_raw_mouse_mode = bench_set_raw_mouse_mode,
    .set_raw_mouse_mode_pointer = bench_set_raw_mouse_mode_pointer,
    .set_scrollbar = bench_set_scrollbar,
    .bell = bench_bell,
    .clip_write = bench_clip_write,
    .clip_request_paste = bench_clip_request_paste,
    .refresh = bench_refresh,
    .request_resize = bench_request_resize,
    .set_title = bench_set_title,
    .set_icon_title = bench_set_icon_title,
    .set_minimised = bench_set_minimised,
    .set_maximised = bench_set_maximised,
    .move = bench_move,
    .set_zorder = bench_set_zorder,
    .palette_set = bench_palette_set,
    .palette_get_overrides = bench_palette_get_overrides,
    .unthrottle = bench_unthrottle,
};
