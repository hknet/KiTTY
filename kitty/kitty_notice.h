#ifndef KITTY_NOTICE_H
#define KITTY_NOTICE_H

#include <windows.h>

/*
 * A small notification window of our own, near the clock: coloured, with a
 * duration we choose, and it never takes the focus. See kitty_notice.c for why
 * this exists rather than a tray balloon.
 *
 * seconds <= 0 uses the default (15). click_hwnd/click_msg are optional: when
 * both are given, clicking the notice posts that message before dismissing it,
 * which is how a notice can offer an action. Only one notice is on screen at a
 * time; a second replaces the first.
 */
void kitty_notice_show(const char *title, const char *text, COLORREF accent,
                       int seconds, HWND click_hwnd, unsigned int click_msg);
void kitty_notice_hide(void);

#endif
