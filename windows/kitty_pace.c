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
#include <dwmapi.h>

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

/* The display's refresh period as DWM reports it, re-read every 250 ms
 * so a panel Windows slows down on battery is followed; 0 when it cannot
 * say. Only the period is used - never where in the cycle we are. */
typedef HRESULT (WINAPI *DwmTiming_t)(HWND, DWM_TIMING_INFO *);
static double refresh_period_ms(void)
{
    static DwmTiming_t fn = NULL;
    static bool tried = false;
    static double asked = -1e9, period = 0;
    double t = now_ms();
    if (!tried) {
        HMODULE dwm = LoadLibraryA("dwmapi.dll");
        tried = true;
        if (dwm)
            fn = (DwmTiming_t)GetProcAddress(dwm, "DwmGetCompositionTimingInfo");
    }
    if (!fn)
        return 0;
    if (t - asked > 250) {
        DWM_TIMING_INFO ti;
        memset(&ti, 0, sizeof(ti));
        ti.cbSize = sizeof(ti);
        asked = t;
        period = 0;
        if (SUCCEEDED(fn(NULL, &ti)) && ti.qpcRefreshPeriod > 0)
            period = ti.qpcRefreshPeriod / qpc_per_ms();
        if (period < 4 || period > 50)          /* 20..250 Hz, else nonsense */
            period = 0;
    }
    return period;
}

/* Energy Saver, polled at most once a second: the one explicit "less,
 * please" the user gives. Plain battery is left alone - Windows already
 * lowered what the hardware supports, the refresh rate included. */
static bool energy_saver(void)
{
    static double polled = -1e9;
    static bool saver = false;
    double t = now_ms();
    if (t - polled > 1000) {
        SYSTEM_POWER_STATUS sps;
        polled = t;
        saver = GetSystemPowerStatus(&sps) && (sps.SystemStatusFlag & 1);
    }
    return saver;
}

/* The pace in force, in ms: auto = one refresh period (16 when unknown),
 * two with Energy Saver on; a number as given; 0 = the fixed cooldown. */
static int effective_pace_ms(void)
{
    double period;
    if (pace_setting != PACE_AUTO)
        return pace_setting;
    period = refresh_period_ms();
    if (period <= 0)
        period = 16;
    if (energy_saver())
        period *= 2;
    return (int)(period + 0.5);
}

/* A minimised window paints at most once a second (windows/window.c says
 * when); WM_PAINT draws the current model the moment it is restored. */
static bool window_hidden;
void kitty_pace_set_hidden(bool hidden)
{
    window_hidden = hidden;
}

/* For the display signal (Direct2D): may a frame go now? The signal comes
 * once per refresh whatever the setting; it is honoured when the last
 * paint's cooldown floor has passed - the paint itself (duty) for auto,
 * the full cap for a number or Energy Saver. */
static double last_paint_end, signal_floor_ms;
bool kitty_pace_signal_allowed(double now)
{
    return now - last_paint_end >= signal_floor_ms;
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
    /* what the display signal has to respect before it may end this
     * cooldown early: with auto only the duty floor (the compositor's
     * cadence IS the pace); otherwise the cap */
    last_paint_end = now;
    signal_floor_ms = (pace_setting == PACE_AUTO && !energy_saver())
        ? (paint_ms > 1 ? paint_ms : 1) : d;
    if (window_hidden)
        d = 1000;                                 /* nobody is looking */
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
