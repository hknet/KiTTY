/*
 * kitty_pace.c - what the frame pace needs from Windows.
 *
 * In the utils library so every target that links terminal.c has it: the
 * terminal core paces its window updates on these (terminal.c, the
 * kitty_framepace_ms / kitty_update_cooldown block).
 */

#include "putty.h"

/* [KiTTY] framepace, bound by the ini table in kitty/kitty.c: milliseconds
 * between window repaints while output streams in; 0 = PuTTY's fixed
 * cooldown. 16 = one frame of a 60 Hz display. */
int FramePaceMs = 16;

int GetFramePace(void)
{
    return FramePaceMs;
}

/* The millisecond clock behind GETTICKCOUNT (windows/platform.h, KiTTY
 * builds): the performance counter, so timers wait what they were asked
 * and not the next 15.6 ms step of GetTickCount. Wraps like GetTickCount
 * does; the timing module's arithmetic is wrap-safe. */
unsigned long kitty_tickcount(void)
{
    static double per_ms = 0;
    LARGE_INTEGER c;
    if (per_ms == 0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        per_ms = f.QuadPart / 1000.0;
    }
    QueryPerformanceCounter(&c);
    return (unsigned long)(c.QuadPart / per_ms);
}

/* The 1 ms timer period, held only while a window-update cooldown is
 * pending: Windows timers fire on the clock interrupt, which is 15.6 ms
 * unless somebody asks for less, and asking costs power - so it is asked
 * for during a burst of output and given back after. winmm is loaded by
 * hand: nothing else links it. */
void kitty_timer_fine(int on)
{
    typedef UINT (WINAPI *period_t)(UINT);
    static period_t begin = NULL, end = NULL;
    static bool loaded = false, armed = false;
    if (!loaded) {
        HMODULE winmm = LoadLibraryA("winmm.dll");
        loaded = true;
        if (winmm) {
            begin = (period_t)GetProcAddress(winmm, "timeBeginPeriod");
            end = (period_t)GetProcAddress(winmm, "timeEndPeriod");
        }
    }
    if (!begin || !end)
        return;
    if (on && !armed) {
        begin(1);
        armed = true;
    } else if (!on && armed) {
        end(1);
        armed = false;
    }
}
