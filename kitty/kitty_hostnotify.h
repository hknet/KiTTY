/*
 * kitty_hostnotify.h - desktop notifications from the host (OSC 9, OSC 777,
 * OSC 99) and taskbar progress (OSC 9;4): the Windows half
 * (kitty_hostnotify.c). The parsing and the decisions are in
 * kitty_oscnotify.c, which has no Windows in it.
 */
#ifndef KITTY_HOSTNOTIFY_H
#define KITTY_HOSTNOTIFY_H

#include <stddef.h>
#include <stdbool.h>

struct terminal_tag;

/* terminal.c's do_osc hands OSC 9, 777 and 99 here, with the string after the
 * number and its first ';'. overflow: the string hit its ceiling and was cut
 * (an OSC 99 is then dropped whole; OSC 9 and 777 are cut anyway). */
void kitty_hostnotify_osc(struct terminal_tag *term, unsigned osc,
                          const char *s, size_t len, bool overflow);

/*
 * A notice from the host through any other channel (the far2l extensions):
 * the same per-session setting (HostNotify, with no narrowing), the same
 * limits, cleaning and flood rule, the same notice. title and body are UTF-8
 * and may be NULL or empty; with neither there is nothing to show.
 */
void kitty_host_notice(struct terminal_tag *term, const char *title,
                       const char *body);

/* KiTTY++'s own notice that far2l's key events turned on or off (window.c
 * calls it once the state has held for a moment). Titled like the host's
 * notices, but not subject to their setting or limits. */
void kitty_far2l_keys_notice(struct terminal_tag *term, bool on);

/* The terminal is going: forget it (pending notice, timer, tracked notice). */
void kitty_hostnotify_term_free(struct terminal_tag *term);

/* The session ended (the window stays) or the window is closing: the
 * taskbar progress is cleared. window.c calls both. */
void kitty_hostnotify_session_ended(void);

/* The window is going for good: the taskbar interface is released. */
void kitty_hostnotify_shutdown(void);

/* Every terminal window message passes here first: on the registered
 * "TaskbarButtonCreated" (Explorer restarted, the button is new) the taskbar
 * interface is created afresh and the last progress state put back. */
void kitty_hostnotify_taskbar_message(unsigned int msg);

#endif
