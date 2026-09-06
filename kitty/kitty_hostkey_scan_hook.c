/*
 * kitty_hostkey_scan_hook.c: the inert half of the host-key scan, linked
 * into every SSH-capable binary (member of the sshcommon library) because
 * ssh/common.c and ssh/transport2.c reference it. See kitty_hostkey_scan.h.
 */
#include <stddef.h>
#include <stdbool.h>
#include "kitty_hostkey_scan.h"

const char *kitty_hostkey_scan_only = NULL;

static kitty_hostkey_capture_fn capture_fn = NULL;
static void *capture_ctx = NULL;

void kitty_hostkey_scan_set_capture(kitty_hostkey_capture_fn fn, void *ctx)
{
    capture_fn = fn;
    capture_ctx = ctx;
}

bool kitty_hostkey_scan_capture(const char *host, int port, const char *keytype,
                                const char *keystr, char **fingerprints)
{
    if (!capture_fn)
        return false;
    capture_fn(capture_ctx, host, port, keytype, keystr, fingerprints);
    return true;
}
