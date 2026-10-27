/*
 * Helpful wrapper functions around the raw logevent().
 *
 * This source file lives in 'utils' because it's conceptually a
 * convenience utility rather than core functionality. But it can't
 * live in the utils _library_, because then it might refer to
 * logevent() in an earlier library after Unix ld had already finished
 * searching that library, and cause a link failure. So it must live
 * alongside logging.c.
 */

#include "putty.h"

void logevent_and_free(LogContext *ctx, char *event)
{
    logevent(ctx, event);
    sfree(event);
}

/* KiTTY: an Event Log line the user of a console tool sees also without
 * -v: the policy's eventlog_always shows it there. The GUI has none - its
 * Event Log holds every line already. Here, on logevent and
 * log_get_policy only, so a program linking stubs/no-logging.c needs
 * nothing more. */
void logevent_always(LogContext *ctx, const char *event)
{
    LogPolicy *lp;
    if (!ctx)
        return;
    logevent(ctx, event);
    lp = log_get_policy(ctx);
    if (lp && lp->vt->eventlog_always)
        lp->vt->eventlog_always(lp, event);
}

void logevent_always_and_free(LogContext *ctx, char *event)
{
    logevent_always(ctx, event);
    sfree(event);
}

void logeventvf(LogContext *ctx, const char *fmt, va_list ap)
{
    logevent_and_free(ctx, dupvprintf(fmt, ap));
}

void logeventf(LogContext *ctx, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    logeventvf(ctx, fmt, ap);
    va_end(ap);
}
