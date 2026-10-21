/*
 * kitty_psoverlay.h - the program status overlay (kitty_psoverlay.c): a small
 * panel at the top right of the terminal window while two or more jobs report
 * their status (OSC 7501).
 */
#ifndef KITTY_PSOVERLAY_H
#define KITTY_PSOVERLAY_H

#include <stdbool.h>
#include <windows.h>
#include "kitty_progstatus.h"
#include "kitty_overlay_draw.h"   /* KovCanvas */

/* The records changed (kitty_hostnotify.c, after every report, a prompt, a
 * focus change and the end of the session). session: the stored session's
 * name, where the overlay's place is kept; title: what the header shows.
 * on: [KiTTY] psoverlay. */
void kitty_psoverlay_update(const PsStore *ps, const char *session,
                            const char *title, bool on);

/* The terminal window moved, was resized, minimised or restored: the overlay
 * follows it (window.c). */
void kitty_psoverlay_follow(void);

/* The panel's screen rectangle while it is shown (the find bar keeps clear
 * of it); false when hidden. */
bool kitty_psoverlay_rect(RECT *r);

/* The terminal window is being shown or hidden (WM_SHOWWINDOW: the tray). */
void kitty_psoverlay_owner_shown(bool shown);

/* The window is going: the overlay is closed with it. */
void kitty_psoverlay_close(void);

/* For the one-line notice of a job's switch (kitty_switchnotice.c): the
 * panel's neutral colours in the current theme (the system colours in
 * high contrast), and a row's state icon (PS_*), sz pixels, top-left at
 * (x,y). */
void kitty_psoverlay_look(COLORREF *bg, COLORREF *fg, COLORREF *line);
void kitty_psoverlay_icon(KovCanvas *c, int state, double x, double y,
                          double sz);

#endif
