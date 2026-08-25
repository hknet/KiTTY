/*
 * kitty_foreground.c - one implementation of "put this window in front and
 * give it the keyboard", for every place that used to write it out by hand.
 *
 * The measured failure this exists to prevent: a window raised without being
 * ACTIVATED leaves the foreground where it was, and the owning application
 * takes the top back seconds later - which reads to a user as the window
 * falling behind by itself.
 */

#include <windows.h>

#include "kitty_foreground.h"

void kitty_force_foreground(HWND w)
{
    HWND fg;
    DWORD fgthread, mythread;
    BOOL attached = FALSE;

    if (!w || !IsWindow(w))
        return;

    fg = GetForegroundWindow();
    fgthread = fg ? GetWindowThreadProcessId(fg, NULL) : 0;
    mythread = GetCurrentThreadId();
    if (fgthread && fgthread != mythread)
        attached = AttachThreadInput(mythread, fgthread, TRUE);

    /* The TOPMOST->NOTOPMOST pair puts the window at the top of the z-order
     * even in the case where the activation below is still refused, so the
     * user at least SEES it; the activation calls are what make it take
     * input. Both are cheap and neither is sufficient alone. */
    SetWindowPos(w, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    SetWindowPos(w, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    SetForegroundWindow(w);
    BringWindowToTop(w);
    SetActiveWindow(w);

    if (attached)
        AttachThreadInput(mythread, fgthread, FALSE);
}
