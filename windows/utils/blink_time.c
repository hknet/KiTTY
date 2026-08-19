/*
 * Wrapper for GetCaretBlinkTime() which turns it into a signed integer,
 * with 0 meaning "no blinking".
 */

#include "putty.h"
#include <winuser.h>

/*
 * CACHED, because this is called from the terminal's per-character path.
 *
 * GetCaretBlinkTime() is a USER32 call and therefore a SYSCALL: measured with a
 * sampling profiler on 2026-08-19, 80% of the whole process's samples during
 * bulk output were sitting in NtUserGetCaretBlinkTime, and half the CPU time of
 * a plain "cat a file" was kernel time because of it. term_schedule_cblink()
 * evaluates CBLINK_DELAY - this function - every time the cursor moves.
 *
 * The blink rate is a Control Panel setting that changes about never, so it is
 * read at most once a second. A user who changes it sees the new rate within a
 * second, which is indistinguishable from immediately for a blinking cursor.
 */
int get_caret_blink_time(void)
{
    static int cached = -1;
    static DWORD cached_at = 0;
    DWORD now = GetTickCount();
    UINT blinktime;

    if (cached >= 0 && (now - cached_at) < 1000)
        return cached;

    blinktime = GetCaretBlinkTime();
    cached_at = now;
    cached = (blinktime == INFINITE) ? 0 : (int)blinktime;
    return cached;
}

static int get_caret_blink_time_uncached(void)
{
    UINT blinktime = GetCaretBlinkTime();
    if (blinktime == INFINITE)
        /* Windows' registry representation for 'no caret blinking'
         * is the string "-1", but we may as well use 0 as the sentinel
         * value, as it'd be bad to attempt blinking with period 0
         * in any case. */
        return 0;
    else
        /* assume this won't be so big that casting is a problem */
        return (int) blinktime;
}
