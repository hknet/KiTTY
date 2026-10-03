/*
 * kitty_far2l_image_term.h - far2l images: the terminal's side
 * (kitty_far2l_image_term.c). The store and the request parser are in
 * kitty_far2l_image.h; this joins them to a Terminal, the window's cell
 * size, the PNG/JPEG decoder (WIC, bound at run time) and repainting.
 */
#ifndef KITTY_FAR2L_IMAGE_TERM_H
#define KITTY_FAR2L_IMAGE_TERM_H

#include <stdbool.h>
#include <stddef.h>

/* Set by the window: the cell size and where cell (0, 0) starts in the
 * client area, in pixels. NULL (or false) where no window draws: images are
 * then reported unavailable. */
extern bool (*kitty_far2l_cell_hook)(TermWin *win, int *cell_w, int *cell_h,
                                     int *off_x, int *off_y);

/* Serve request 'i'. stk/len: the decoded request stack without its request
 * ID and command letter. max_data: the most image data one set may carry
 * (the far2l payload ceiling, decoded; 0 = no such limit). Returns the reply
 * (snewn'd), its last byte left for the request ID, and its length in
 * *reply_size. */
char *kitty_far2l_image_request(Terminal *term, const char *stk, int len,
                                size_t max_data, int *reply_size);

/* Remove every image of the terminal and repaint where they were (a reset,
 * far2l0, the session's end). */
void kitty_far2l_images_reset(Terminal *term);

/* Free the store (term_free). */
void kitty_far2l_images_free(Terminal *term);

/* Whether the terminal shows any image now. */
bool kitty_far2l_images_any(Terminal *term);

#endif /* KITTY_FAR2L_IMAGE_TERM_H */
