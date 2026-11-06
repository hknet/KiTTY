/*
 * test_resizekeep.c - KiTTY: a resize that makes the terminal smaller and
 * then larger again keeps the rows below the cursor (terminal.c term_size,
 * kitty_below), and drops them when output, a reset, a cleared scrollback
 * or a width change came in between.
 *
 * terminal.c has to be compiled with MOD_PERSO for this, and with that it
 * needs the stubs and the mock window of test_osc52.c: that file is taken in
 * whole, its own main renamed away, and this file's main runs instead.
 */
#define main test_osc52_main_not_run
#include "test_osc52.c"
#undef main

#define RK_SAVELINES 200

static int rk_fails = 0;
static const char *rk_what = "";

static void rk_check(int line, long long got, long long want)
{
    if (got != want) {
        printf("FAIL  [%s] line %d: got %lld, expected %lld\n",
               rk_what, line, got, want);
        rk_fails++;
    }
}
#define RK_EQ(got, want) rk_check(__LINE__, (got), (want))

static void rk_feed(Mock *mk, const char *s)
{
    term_data(mk->term, s, strlen(s));
}

static unsigned long rk_ch(Mock *mk, int x, int y)
{
    termline *tl = term_get_line(mk->term, y);
    unsigned long c = (0 <= x && x < tl->cols) ? tl->chars[x].chr
                                                : mk->term->erase_char.chr;
    term_release_line(tl);
    return c;
}

/* 24 x cols, rows 0..19 hold A..T, the cursor at the start of row 5 */
static void rk_setup(Mock *mk, int cols)
{
    int i;
    char buf[8];

    term_pwron(mk->term, true);
    term_size(mk->term, 24, cols, RK_SAVELINES);
    rk_feed(mk, "\033[2J\033[H");
    for (i = 0; i < 20; i++) {
        sprintf(buf, i < 19 ? "%c\r\n" : "%c", 'A' + i);
        rk_feed(mk, buf);
    }
    rk_feed(mk, "\033[6;1H");
}

/* rows 0..19 A..T again, 20..23 blank, the cursor back on row 5 */
static void rk_expect_all(Mock *mk)
{
    int y;
    for (y = 0; y < 24; y++)
        RK_EQ(rk_ch(mk, 0, y), y < 20 ? (CSET_ASCII | ('A' + y))
                                       : (CSET_ASCII | ' '));
    RK_EQ(mk->term->curs.y, 5);
}

/* rows 0..5 A..F, everything below the cursor blank */
static void rk_expect_cut(Mock *mk)
{
    int y;
    for (y = 0; y < 24; y++)
        RK_EQ(rk_ch(mk, 0, y), y < 6 ? (CSET_ASCII | ('A' + y))
                                      : (CSET_ASCII | ' '));
}

int main(void)
{
    Mock *mk = mock_new();
    CoInitialize(NULL);
    mk->term = term_init(mk->conf, mk->ucsdata, &mk->tw);

    rk_what = "1 row and back";
    rk_setup(mk, 80);
    term_size(mk->term, 1, 80, RK_SAVELINES);
    term_size(mk->term, 24, 80, RK_SAVELINES);
    rk_expect_all(mk);

    rk_what = "3 rows and back";
    rk_setup(mk, 80);
    term_size(mk->term, 3, 80, RK_SAVELINES);
    term_size(mk->term, 24, 80, RK_SAVELINES);
    rk_expect_all(mk);

    rk_what = "3 rows, 1 row, then back";
    rk_setup(mk, 80);
    term_size(mk->term, 3, 80, RK_SAVELINES);
    term_size(mk->term, 1, 80, RK_SAVELINES);
    term_size(mk->term, 24, 80, RK_SAVELINES);
    rk_expect_all(mk);

    rk_what = "back in two steps";
    rk_setup(mk, 80);
    term_size(mk->term, 1, 80, RK_SAVELINES);
    term_size(mk->term, 10, 80, RK_SAVELINES);
    term_size(mk->term, 24, 80, RK_SAVELINES);
    rk_expect_all(mk);

    rk_what = "output while small drops them";
    rk_setup(mk, 80);
    term_size(mk->term, 1, 80, RK_SAVELINES);
    rk_feed(mk, "x");
    term_size(mk->term, 24, 80, RK_SAVELINES);
    RK_EQ(rk_ch(mk, 0, 6), CSET_ASCII | ' ');
    RK_EQ(rk_ch(mk, 0, 19), CSET_ASCII | ' ');

    rk_what = "a reset drops them";
    rk_setup(mk, 80);
    term_size(mk->term, 1, 80, RK_SAVELINES);
    term_pwron(mk->term, true);
    term_size(mk->term, 24, 80, RK_SAVELINES);
    RK_EQ(rk_ch(mk, 0, 19), CSET_ASCII | ' ');

    rk_what = "a cleared scrollback drops them";
    rk_setup(mk, 80);
    term_size(mk->term, 1, 80, RK_SAVELINES);
    term_clrsb(mk->term);
    term_size(mk->term, 24, 80, RK_SAVELINES);
    RK_EQ(rk_ch(mk, 0, 19), CSET_ASCII | ' ');

    rk_what = "a width change drops them";
    rk_setup(mk, 80);
    term_size(mk->term, 1, 80, RK_SAVELINES);
    term_size(mk->term, 24, 60, RK_SAVELINES);
    rk_expect_cut(mk);

    rk_what = "the cursor on the last row: nothing kept below it";
    rk_setup(mk, 80);
    rk_feed(mk, "\033[24;1H");
    term_size(mk->term, 1, 80, RK_SAVELINES);
    term_size(mk->term, 24, 80, RK_SAVELINES);
    RK_EQ(rk_ch(mk, 0, 0), CSET_ASCII | 'A');
    RK_EQ(rk_ch(mk, 0, 19), CSET_ASCII | 'T');
    RK_EQ(mk->term->curs.y, 23);

    mock_free(mk);
    if (rk_fails) {
        printf("%d check(s) failed\n", rk_fails);
        return 1;
    }
    printf("All tests passed\n");
    return 0;
}
