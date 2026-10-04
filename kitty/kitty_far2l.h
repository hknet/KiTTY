/*
 * kitty_far2l.h - the platform half of the far2l terminal extensions
 * (kitty/kitty_far2l.c, and kitty_far2l_max_cells in windows/window.c).
 *
 * terminal/terminal.c holds the protocol and the permission logic and reaches
 * the Windows clipboard, the permission box and the window only through these
 * seams, so the test targets that compile terminal.c stub them and never touch
 * the real clipboard or open a window.
 *
 * Formats are far2l's wire numbers (F2L_CF_* in far2l/far2l_proto.h, or a
 * registered format >= 0xC000); data is in far2l's wire encoding (CF_TEXT
 * UTF-8, CF_UNICODETEXT UTF-32, HTML as plain HTML). Every call opens and
 * closes the Windows clipboard itself: it is never held between requests.
 */
#ifndef KITTY_FAR2L_H
#define KITTY_FAR2L_H

#include "putty.h"

/* Register a clipboard format by name (UTF-8); 0 if it failed. */
uint32_t kitty_far2l_clip_register(const char *name, size_t len);
/* Is a format on the clipboard? */
bool kitty_far2l_clip_available(uint32_t fmt);
/* Empty the clipboard. */
bool kitty_far2l_clip_empty(void);
/* Put one format on the clipboard, after emptying it when `empty_first`;
 * the other formats stay otherwise. */
bool kitty_far2l_clip_set(uint32_t fmt, const unsigned char *data, size_t len,
                          bool empty_first);
/* A format's data in wire encoding (text NUL-terminated), or NULL when there
 * is none. Caller wipes and frees. */
unsigned char *kitty_far2l_clip_get(uint32_t fmt, size_t *len);

/* The permission box, modeless, owned by the terminal window. The answer
 * goes to term_far2l_open_answer later. `offer_always`: show "Always allow
 * this far2l" (the request carried a valid client ID). _end: the terminal is
 * going away or far2l left; the box closes without answering. */
void kitty_far2l_confirm(Terminal *term, bool offer_always);
void kitty_far2l_confirm_end(Terminal *term);
/* Write CONF_far2l_client_ids into the saved session. False when there is
 * none to write into (an unsaved session, Default Settings). */
bool kitty_far2l_save_client_ids(Terminal *term);

/* windows/window.c: the largest terminal this window can have, in cells -
 * maximised on its monitor's work area at the current font. False if the
 * window is not known; the caller then answers the current size. */
bool kitty_far2l_max_cells(Terminal *term, int *rows, int *cols);

/* windows/window.c: the key events may have turned on or off (armed,
 * disarmed, far2l left, a reset) - every far2l_input_gen step calls it. The
 * window re-reads the state from a toplevel callback: the title suffix and
 * the notice (kitty_far2l_keys_notice in kitty/kitty_hostnotify.c). */
void kitty_far2l_events_changed(Terminal *term);

#endif /* KITTY_FAR2L_H */
