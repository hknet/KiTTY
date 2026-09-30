/*
 * kitty_url.h - clickable URLs in the terminal window (kitty_url.c).
 */

#ifndef KITTY_URL_H
#define KITTY_URL_H
#include "putty.h"

/* ---- exported from kitty/kitty_url.c ---- */
int kitty_url_cell_in_link(int col, int row);
int kitty_url_click(Terminal *term, Conf *conf, HWND hwnd, LogContext *logctx,
                    int x, int y, int ctrl_down);
void kitty_url_config(Conf *conf);
int kitty_url_hover(Terminal *term, Conf *conf, HWND hwnd, int cx, int cy,
                    int hover_cursor);
/* The OSC 8 target preview: take it down (pointer left the window, focus
 * lost) or re-check it after a rescan (the link under it may be gone). */
void kitty_url_preview_hide(void);
void kitty_url_preview_refresh(Terminal *term, Conf *conf, HWND hwnd);
void kitty_url_init(void);
int kitty_url_rescan(Terminal *term, Conf *conf);
int kitty_url_row_dirty(int row);

#endif /* KITTY_URL_H */
