#include "putty.h"
#include "kitty_perf.h"

#define TIMING_CLASS_NAME "PuTTYTimerWindow"
#define TIMING_TIMER_ID 1234
static long timing_next_time;
static HWND timing_hwnd;

static LRESULT CALLBACK TimingWndProc(HWND hwnd, UINT message,
                                      WPARAM wParam, LPARAM lParam)
{
    switch (message) {
      case WM_TIMER:
        if ((UINT_PTR)wParam == TIMING_TIMER_ID) {
            unsigned long next;

            KillTimer(hwnd, TIMING_TIMER_ID);
            if (run_timers(timing_next_time, &next)) {
                timer_change_notify(next);
            } else {
            }
        }
        return 0;
    }
    return DefWindowProc(hwnd, message, wParam, lParam);
}

void setup_gui_timing(void)
{
    WNDCLASS wndclass;

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
    KillTimer(timing_hwnd, TIMING_TIMER_ID);
    SetTimer(timing_hwnd, TIMING_TIMER_ID, ticks, NULL);
    timing_next_time = next;
    KP_T1(KP_TIMER);
}
