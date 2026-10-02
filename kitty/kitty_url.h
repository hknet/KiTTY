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
/* The frame moved rows top..bot by n (positive = up); after the frame, the
 * rows whose underline changed (then kitty_url_row_dirty). */
void kitty_url_note_shift(int top, int bot, int n);
int kitty_url_frame_done(void);
/* with_links: the links' own changes count (Underline = Always), not only
 * the rows the pointer's link changed (On hover). */
int kitty_url_row_dirty(int row, int with_links);
/* Underline hyperlinks = On hover: rows the pointer changed are waiting
 * (kitty_url_hover_dirty); the pointer left the window (kitty_url_hover_leave);
 * after the window repainted the dirty rows, kitty_url_dirty_clear. */
int kitty_url_hover_dirty(void);
void kitty_url_hover_leave(void);
void kitty_url_dirty_clear(void);
/* Is a cell drawn underlined, for CONF_url_underline = mode? */
int kitty_url_cell_underlined(int col, int row, int mode);

#endif /* KITTY_URL_H */
