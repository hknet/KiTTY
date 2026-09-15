/*
 * kitty_osc52.h - OSC 52 clipboard access from the terminal and its permission
 * prompts (kitty_osc52.c).
 */

#ifndef KITTY_OSC52_H
#define KITTY_OSC52_H
#include "putty.h"
#include "terminal.h"   /* KittyClipFormat */

/* ---- exported from kitty/kitty_osc52.c ---- */
int kitty_clipboard_balloon_action(void);
void kitty_frame_restore_resting(void);
bool kitty_osc52_clipboard_has_image(void);
wchar_t *kitty_osc52_get_clipboard(int *len);
wchar_t *kitty_osc52_get_clipboard_ex(int *len, bool *unavailable);
unsigned char *kitty_osc52_get_clipboard_png(size_t *len, bool *unavailable);
void kitty_osc52_notify(Terminal *term, const char *title, const char *msg, int action);
void kitty_osc52_random(unsigned char *buf, size_t len);
bool kitty_osc52_read_dialog(Terminal *term, const wchar_t *clip, int clip_len, const char *claim, int *grant, bool *always_deny);
bool kitty_osc52_save_deny_for_host(Terminal *term);
void kitty_osc52_send_raw(Terminal *term, const char *data, size_t len);
bool kitty_osc52_set_clipboard_formats(const KittyClipFormat *fmts, int n);
void kitty_osc52_state_changed(Terminal *term);
bool kitty_osc52_title_visible(void);

#endif /* KITTY_OSC52_H */
