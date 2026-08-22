/*
 * KiTTY: Windows Hello presence check, for gating the agent's approval
 * moments (confirm-on-use, Accept-this-key; later the agent unlock).
 *
 * A Hello verification asks the SYSTEM to prove a human is present -
 * biometrics or the device PIN, in Windows' own protected UI. A MessageBox
 * click can be synthesized by any same-user process; this cannot. It is a
 * raised bar, not a full boundary: an approval covers one request, and an
 * admin-level attacker is out of scope.
 */

#ifndef KITTY_HELLO_H
#define KITTY_HELLO_H

#include <windows.h>

/* kitty_hello_verify outcomes. Callers FAIL CLOSED on everything except
 * VERIFIED; UNAVAILABLE vs DENIED matter only for what the user is told. */
enum {
    KITTY_HELLO_VERIFIED = 0,    /* the human is present and approved */
    KITTY_HELLO_DENIED,          /* declined, cancelled, retries exhausted */
    KITTY_HELLO_UNAVAILABLE,     /* no Hello credential / policy / no device
                                  * (notably: RDP sessions) */
    KITTY_HELLO_ERROR            /* plumbing failed; treated as a denial */
};

/* Is Hello usable right now? 1 = available, 0 = not, -1 = could not tell.
 * Runs the system availability check (no UI). Used to grey the setting. */
int kitty_hello_available(void);

/* Show the Hello prompt (parented to owner) with the given UTF-8 message
 * and wait for the outcome, pumping messages meanwhile. Returns one of the
 * KITTY_HELLO_* values above; never blocks longer than its own deadline. */
int kitty_hello_verify(HWND owner, const char *message_utf8);

#endif /* KITTY_HELLO_H */
