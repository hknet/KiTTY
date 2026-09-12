#ifndef KITTY_NOTICE_H
#define KITTY_NOTICE_H

#include <windows.h>

/*
 * A small notification window of our own, near the clock: coloured, with a
 * duration we choose, and it never takes the focus. See kitty_notice.c for why
 * this exists rather than a tray balloon.
 *
 * seconds <= 0 uses the default (15), except KITTY_NOTICE_STICKY, which sets
 * no timer at all: the notice stays until it is clicked or a later notice
 * replaces it. click_hwnd/click_msg are optional: when both are given,
 * clicking the notice posts that message before dismissing it, which is how a
 * notice can offer an action. Only one notice is on screen at a time; a second
 * replaces the first.
 */
#define KITTY_NOTICE_STICKY (-1)

void kitty_notice_show(const char *title, const char *text, COLORREF accent,
                       int seconds, HWND click_hwnd, unsigned int click_msg);

/* The same, plus a callback run when the notice is FINISHED with - `clicked`
 * distinguishes a dismissal from a notice that merely ran out. A notice that
 * holds something for as long as it is on screen (the application
 * notification holds a desktop-wide mutex) releases it there.
 *
 * A STICKY notice displaced by a later, ordinary one is PARKED, not finished:
 * on_close does not run, whatever it holds stays held, and it goes back on
 * screen when the notice that displaced it goes away. Only a second sticky
 * notice ends a parked one. */
void kitty_notice_show_ex(const char *title, const char *text, COLORREF accent,
                          int seconds, HWND click_hwnd, unsigned int click_msg,
                          void (*on_close)(void *ctx, int clicked), void *ctx);

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
