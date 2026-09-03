/*
 * kitty_pace.c - frame pacing: when the window may repaint again.
 *
 * In the utils library so every target that links terminal.c has it. The
 * terminal calls kitty_pace_cooldown_ms after each window update with the
 * time the update took, and schedules the answer; the timing module
 * (windows/utils/gui-timing.c) fires it exactly.
 *
 * The setting, [KiTTY] framepace:
 *   auto     one frame per display refresh, never more often than every
 *            16 ms; 33 ms on battery, 50 ms with Energy Saver on (default)
 *   N        at most one frame per N ms, whatever the power state
 *   0        PuTTY's fixed 20 ms cooldown
 * In every case the cooldown is never shorter than the paint itself, so
 * painting takes at most half the time. A painter with a display signal
 * (Direct2D) does not time the next frame at all: the compositor says when
 * it is ready for one (kitty_pace_wait_frame); GDI paces on the timer.
 */

#include "putty.h"

/* The millisecond clock behind GETTICKCOUNT (windows/platform.h): the
 * performance counter, so timers wait what they were asked and not the
 * next 15.6 ms step of GetTickCount. Wraps like GetTickCount does; the
 * timing module's arithmetic is wrap-safe. */
static double qpc_per_ms(void)
{
    static double per_ms = 0;
    if (per_ms == 0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        per_ms = f.QuadPart / 1000.0;
    }
    return per_ms;
}

unsigned long kitty_tickcount(void)
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (unsigned long)(c.QuadPart / qpc_per_ms());
}

static double now_ms(void)
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart / qpc_per_ms();
}

/* ---- the setting ---------------------------------------------------- */

#define PACE_AUTO (-1)
static int pace_setting = PACE_AUTO;

void kitty_pace_set_setting(const char *value)
{
    const char *e = getenv("KITTY_UPDATE_PACE_MS");   /* the measuring knob */
    if (e && atoi(e) > 0)
        value = e;
    if (!value || !*value || !stricmp(value, "auto")) {
        pace_setting = PACE_AUTO;
    } else {
        int n = atoi(value);
        pace_setting = n < 0 ? 0 : n > 1000 ? 1000 : n;
    }
}

/* auto follows the power state, polled at most once a second: on battery
 * half the frames, with Energy Saver on a quarter. An explicit number is
 * the user's word and is not scaled. */
static int effective_pace_ms(void)
{
    static double polled = -1e9;
    static int auto_pace = 16;
    double t;
    if (pace_setting != PACE_AUTO)
        return pace_setting;
    t = now_ms();
    if (t - polled > 1000) {
        SYSTEM_POWER_STATUS sps;
        polled = t;
        auto_pace = 16;
        if (GetSystemPowerStatus(&sps)) {
            if (sps.SystemStatusFlag & 1)          /* Energy Saver */
                auto_pace = 50;
            else if (sps.ACLineStatus == 0)        /* on battery */
                auto_pace = 33;
        }
    }
    return auto_pace;
}

/* The pace in force, for the frame-signal wait (windows/kitty_pace_frame.c,
 * in the event-loop library because that is where the handle list lives). */
int kitty_pace_effective_ms(void)
{
    return effective_pace_ms();
}

/* ---- the answer ------------------------------------------------------ */

/* Time the painter spent WAITING inside the last update - a Direct2D
 * Present blocks when the compositor still holds the previous frame. That
 * is not painting: the painter records it here (windows/paint-d2d.c) and
 * it is taken off the paint cost, once. */
double kitty_present_wait_ms = 0;

unsigned long kitty_pace_cooldown_ms(double now, double paint_ms)
{
    int pace = effective_pace_ms();
    double d, target;

    if (pace <= 0)
        return 20;                              /* PuTTY's UPDATE_DELAY */
    paint_ms -= kitty_present_wait_ms;
    if (paint_ms < 0) paint_ms = 0;

    /* frames every `pace` ms, and never a cooldown shorter than the paint
     * itself: painting takes at most half the time */
    d = pace - paint_ms;
    if (d < paint_ms) d = paint_ms;
    if (d < 1) d = 1;
    target = now + d;

    d = target - now;
    {
        static FILE *trace = NULL;
        static bool tried = false;
        static double asked_for = 0;
        if (!tried) {
            const char *f = getenv("KITTY_PACE_TRACE");
            tried = true;
            if (f && *f) trace = fopen(f, "w");
        }
        if (trace) {
            double started = now - paint_ms - kitty_present_wait_ms;
            fprintf(trace, "now %.1f  late-by %.1f  paint %.1f  wait %.1f  cooldown %.1f\n",
                    now, asked_for > 0 ? started - asked_for : 0, paint_ms,
                    kitty_present_wait_ms, d);
            fflush(trace);
            asked_for = now + (d < 1 ? 1 : d);
        }
    }
    kitty_present_wait_ms = 0;
    return (unsigned long)(d < 1 ? 1 : d + 0.5);
}
