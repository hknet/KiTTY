/*
 * gui-timing.c - the timer behind timing.c for the GUI programs.
 *
 * KiTTY: a waitable timer on the message loop's handle list instead of
 * SetTimer. SetTimer fires on the system clock interrupt, 15.6 ms apart
 * unless something asks for less, so a 12 ms timer waited 16 or 31. A
 * waitable timer created with CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
 * (Windows 10 1803 and later) fires within a fraction of a millisecond,
 * whether the window is in front or not and with no timeBeginPeriod. An
 * older Windows gets a plain waitable timer, and one without
 * CreateWaitableTimerEx (XP) the hidden window with SetTimer as before.
 */

#include "putty.h"
#include "kitty_perf.h"

#define TIMING_CLASS_NAME "PuTTYTimerWindow"
#define TIMING_TIMER_ID 1234
static long timing_next_time;
static HWND timing_hwnd;
static HANDLE timing_timer;
static HandleWait *timing_wait;
static bool timing_armed;

static void timing_fired(void)
{
    unsigned long next;
    timing_armed = false;
    if (run_timers(timing_next_time, &next))
        timer_change_notify(next);
}

/* For a modal loop (a window move, an open menu) that pumps messages but
 * never returns to our loop, where the waitable timer would be noticed:
 * run the timers if their time has come. Called from a timer message the
 * modal loop delivers (windows/window.c). */
void gui_timing_pump(void)
{
    if (timing_armed && GETTICKCOUNT() - (unsigned long)timing_next_time < INT_MAX)
        timing_fired();
}

static void timing_handle_callback(void *ctx)
{
    (void)ctx;
    timing_fired();
}

static LRESULT CALLBACK TimingWndProc(HWND hwnd, UINT message,
                                      WPARAM wParam, LPARAM lParam)
{
    switch (message) {
      case WM_TIMER:
        if ((UINT_PTR)wParam == TIMING_TIMER_ID) {
            KillTimer(hwnd, TIMING_TIMER_ID);
            timing_fired();
        }
        return 0;
    }
    return DefWindowProc(hwnd, message, wParam, lParam);
}

void setup_gui_timing(void)
{
    typedef HANDLE (WINAPI *CreateTimerEx_t)(LPSECURITY_ATTRIBUTES, LPCWSTR,
                                             DWORD, DWORD);
    CreateTimerEx_t create = (CreateTimerEx_t)GetProcAddress(
        GetModuleHandleA("kernel32.dll"), "CreateWaitableTimerExW");
    WNDCLASS wndclass;

    if (create) {
        timing_timer = create(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                              TIMER_ALL_ACCESS);
        if (!timing_timer)
            timing_timer = create(NULL, NULL, 0, TIMER_ALL_ACCESS);
    }
    if (timing_timer) {
        timing_wait = add_handle_wait(timing_timer, timing_handle_callback, NULL);
        return;
    }

    memset(&wndclass, 0, sizeof(wndclass));
    wndclass.lpfnWndProc = TimingWndProc;
    wndclass.hInstance = hinst;
    wndclass.lpszClassName = TIMING_CLASS_NAME;

    RegisterClass(&wndclass);

    timing_hwnd = CreateWindow(
        TIMING_CLASS_NAME, "PuTTY: hidden timing window",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        100, 100, NULL, NULL, hinst, NULL);
    ShowWindow(timing_hwnd, SW_HIDE);
}

void timer_change_notify(unsigned long next)
{
    unsigned long now = GETTICKCOUNT();
    long ticks;
    KP_T0;
    if (now - next < INT_MAX)
        ticks = 0;
    else
        ticks = next - now;
    if (timing_timer) {
        /* relative due time in 100 ns units; 0 would mean "never" */
        LARGE_INTEGER due;
        due.QuadPart = ticks > 0 ? -(LONGLONG)ticks * 10000 : -1;
        SetWaitableTimer(timing_timer, &due, 0, NULL, NULL, FALSE);
    } else {
        KillTimer(timing_hwnd, TIMING_TIMER_ID);
        SetTimer(timing_hwnd, TIMING_TIMER_ID, ticks, NULL);
    }
    timing_next_time = next;
    timing_armed = true;
    KP_T1(KP_TIMER);
}
