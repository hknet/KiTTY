/*
 * kitty_knock_cli.c - port knocking in kscp, ksftp and klink: the sequence
 * of kitty_knock.c run to its end in the console event loop, just before
 * the backend connects. A source of each tool rather than of the network
 * library, which links after the event loop.
 */
#include "putty.h"
#include "kitty_knock.h"

/* the wake-up timer's context: a timer needs one (timing.c files its
 * context in a tree, which refuses NULL) */
static char knock_cli_wake;

static void knock_cli_tick(void *ctx, unsigned long now)
{
    (void)ctx;
    (void)now;
}

static void knock_cli_done(void *ctx)
{
    *(bool *)ctx = true;
    /* The last step can end inside run_timers, after the loop asked pre()
     * and with no timer left: the loop would then wait for a handle (klink's
     * standard input) for ever. A timer due at once brings it round to the
     * done flag. */
    schedule_timer(1, knock_cli_tick, &knock_cli_wake);
}

static bool knock_cli_pre(void *ctx, const HANDLE **extra, size_t *nextra)
{
    (void)extra;
    (void)nextra;
    return !*(bool *)ctx;
}

static bool knock_cli_post(void *ctx, size_t index)
{
    (void)index;
    return !*(bool *)ctx;
}

void kitty_knock_run_cli(Conf *conf, LogContext *logctx)
{
    bool done = false;
    char *refusal = NULL;

    if (!kitty_knock_start(conf, logctx, knock_cli_done, NULL, &done,
                           &refusal)) {
        if (refusal) {
            /* the line is on standard error already: no connection */
            sfree(refusal);
            cleanup_exit(1);
        }
        return;
    }
    cli_main_loop(knock_cli_pre, knock_cli_post, &done);
}
