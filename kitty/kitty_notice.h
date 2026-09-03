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

/* Clicking the "SSH agent not verified" notice (kitty_win.c) posts this to
 * the terminal window; window.c answers by opening a configuration window
 * on Connection/SSH/Auth, where the warning's off switch lives. Shared here
 * because sender and receiver are different files. (WM_APP+71 is the
 * clipboard balloon, +72 the workplace-disarm notice in window.c.) */
#define WM_KITTY_AGENT_UNVERIFIED (WM_APP + 73)

/* Clicking the "also showing your old sessions" notice posts this; window.c
 * answers with a configuration window on Application/Migration. */
#define WM_KITTY_FOREIGN_SESSIONS (WM_APP + 74)

/* The agent check runs its signature verifications on a worker thread
 * (kitty_win.c); when the serving program is not ours the thread posts this
 * to the terminal window with the program's file name in lParam (a dupstr,
 * the receiver frees it), and window.c shows the notice from the UI thread. */
#define WM_KITTY_AGENT_CHECKED (WM_APP + 75)
void kitty_agent_unverified_notice(char *name);   /* kitty/kitty_win.c */

#endif
