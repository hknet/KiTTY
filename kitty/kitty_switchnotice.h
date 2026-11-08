/*
 * kitty_switchnotice.h - the one-line notices at the top of the terminal
 * area (kitty_switchnotice.c): a moment's word of why the frame and the
 * tint just turned red or yellow (OSC 3008), and of one job's switch to
 * blocked, failed or finished (OSC 7501).
 */
#ifndef KITTY_SWITCHNOTICE_H
#define KITTY_SWITCHNOTICE_H

#include <stdbool.h>

/* The two lines at the top, the user switch above the job's, and the Event
 * Log badge at the top right (KSN_EVLOG: kitty_switchnotice_badge, the
 * only slot that takes a click). */
enum { KSN_CTX3008, KSN_PS7501, KSN_EVLOG, KSN_SLOTS };

/* How a line looks: on the root frame's dark red, on a switched user's dark
 * yellow, or in the status panel's colours with its state icon - for OSC
 * 7501 (STATUS) or OSC 9;4 progress (PROGRESS, the same look). EVLOG: the
 * panel's colours without an icon. */
enum { KSN_LOOK_ROOT, KSN_LOOK_USER, KSN_LOOK_STATUS, KSN_LOOK_PROGRESS,
       KSN_LOOK_EVLOG };

/* A click on the KSN_EVLOG notice (windows/window.c sets it): the notice
 * goes first. NULL: the click does nothing. */
extern void (*kitty_switchnotice_click)(int slot);

/* [KiTTY] osc3008ctxnotice / osc7501notice (key): seconds, 0-10, 2 when not
 * set; 0 = never. Read at every notice, so a change applies to the next. */
int kitty_switchnotice_seconds(const char *key);

/* Show text (UTF-8) in that slot for secs seconds - fading in, holding,
 * fading out, all inside that time - replacing what the slot shows. state:
 * the PS_* state whose icon KSN_LOOK_STATUS draws. line_px: the terminal's
 * line height in pixels, its height (0 = the last one given). Nothing while
 * the terminal window is hidden or minimised. */
void kitty_switchnotice_show(int slot, int look, int state, const char *text,
                             int secs, int line_px);

/* The Event Log badge (KSN_EVLOG): text (UTF-8, one line) with tip as its
 * tooltip, for secs seconds, or until kitty_switchnotice_hide when secs < 0
 * (kept over a minimise or the tray). 0: nothing. A badge already up takes
 * the new text without a second fade in. */
void kitty_switchnotice_badge(const char *text, const char *tip, int secs,
                              int line_px);

/* The terminal's line height changed (a font change, Font Up/Down, a new
 * DPI): the lines showing are drawn again at line_px. */
void kitty_switchnotice_line(int line_px);

/* That slot's line goes at once (the Event Log window opened). */
void kitty_switchnotice_hide(int slot);

/* That slot shows a line of that look now (inside its time). */
bool kitty_switchnotice_showing(int slot, int look);

/* The terminal window moved, was resized or changed its DPI. */
void kitty_switchnotice_follow(void);

/* The terminal window is being shown or hidden (WM_SHOWWINDOW: the tray):
 * hidden, the lines go. */
void kitty_switchnotice_owner_shown(bool shown);

/* The application theme changed: the lines showing are drawn again. */
void kitty_switchnotice_restyle(void);

/* The window is going: the lines are closed with it. */
void kitty_switchnotice_close(void);

/* windows/window.c: the line height of the window showing term, in pixels
 * (0 when not known). */
struct terminal_tag;
int kitty_term_line_px(struct terminal_tag *term);

#endif
