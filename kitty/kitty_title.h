/*
 * KiTTY: one place for the standard window-title / tooltip state suffixes -
 * "(portable)", "(RESTRICTED)" and the test-build label. See kitty_title.c.
 */
#ifndef KITTY_TITLE_H
#define KITTY_TITLE_H

char *kitty_title_compose(const char *base, int portable, int restricted,
                          int test_label);
char *kitty_title_compose_sep(const char *base, const char *sep,
                              int portable, int restricted, int test_label);

#endif
