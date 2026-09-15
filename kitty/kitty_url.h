/*
 * kitty_url.h - clickable URLs in the terminal window (kitty_url.c).
 */

#ifndef KITTY_URL_H
#define KITTY_URL_H
#include "putty.h"

/* ---- exported from kitty/kitty_url.c ---- */
int kitty_url_cell_in_link(int col, int row);
int kitty_url_click(Terminal *term, Conf *conf, int x, int y, int ctrl_down);
void kitty_url_config(Conf *conf);
int kitty_url_hover(Terminal *term, HWND hwnd, int cx, int cy, int hover_cursor);
void kitty_url_init(void);
int kitty_url_rescan(Terminal *term);
int kitty_url_row_dirty(int row);

#endif /* KITTY_URL_H */
