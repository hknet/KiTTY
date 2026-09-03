/*
 * kitty_pace_frame.c - the display's ready signal, on the message loop.
 *
 * In the event-loop library with handle-wait.c, which it uses; the rest of
 * the frame pacing is windows/kitty_pace.c (utils).
 */

#include "putty.h"

int kitty_pace_effective_ms(void);

/* ---- the display's ready signal ------------------------------------- */

/* The painter's frame signal (windows/paint-d2d.c: the swap chain's
 * frame-latency waitable object), or NULL. One terminal window per
 * process is the normal case. While a cooldown is pending the signal is on
 * the message loop's handle list; it comes off again when it fires, since
 * an idle swap chain keeps it signalled and the loop would spin. */
static HANDLE frame_signal;
static HandleWait *frame_wait;
static void (*frame_cb)(void *);
static void *frame_ctx;

void kitty_pace_set_frame_signal(HANDLE h)
{
    if (frame_wait) {
        delete_handle_wait(frame_wait);
        frame_wait = NULL;
    }
    frame_signal = h;
}

static void frame_fired(void *ctx)
{
    (void)ctx;
    if (frame_wait) {
        delete_handle_wait(frame_wait);
        frame_wait = NULL;
    }
    if (frame_cb)
        frame_cb(frame_ctx);
}

/* Wait for the display's next frame slot; cb(ctx) runs when it comes.
 * False when there is no signal (GDI) or the pace is off: the caller
 * paces on the timer alone. */
bool kitty_pace_wait_frame(void (*cb)(void *), void *ctx)
{
    if (!frame_signal || kitty_pace_effective_ms() <= 0)
        return false;
    if (frame_wait)
        delete_handle_wait(frame_wait);
    frame_cb = cb;
    frame_ctx = ctx;
    frame_wait = add_handle_wait(frame_signal, frame_fired, NULL);
    return frame_wait != NULL;
}


/* A modal loop (a window move, a menu) never returns to the loop that
 * waits on the handle list: its timer message pumps the pending work
 * instead (windows/window.c), and this is the signal's share of it. */
void kitty_pace_frame_pump(void)
{
    if (frame_wait && frame_signal &&
        WaitForSingleObject(frame_signal, 0) == WAIT_OBJECT_0)
        frame_fired(NULL);
}
