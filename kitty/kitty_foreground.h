/*
 * kitty_foreground.h - take the foreground the way Windows allows.
 */
#ifndef KITTY_FOREGROUND_H
#define KITTY_FOREGROUND_H

#include <windows.h>

/*
 * Bring a window to the front AND make it the active one.
 *
 * SetForegroundWindow alone is refused whenever another application owns the
 * foreground (Windows' foreground lock): the window is raised but never
 * activated, so it takes no keyboard input and the app that does own the
 * foreground puts itself back on top moments later. Attaching this thread to
 * the foreground thread's input queue for the duration of the call lends us
 * the right to hand it over; the attachment is skipped when we already own
 * the foreground, and always undone.
 *
 * Use this instead of writing the dance again: it was three copies before
 * this function existed, and a fourth was nearly added.
 */
void kitty_force_foreground(HWND w);

#endif /* KITTY_FOREGROUND_H */
