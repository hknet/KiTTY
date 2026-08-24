/*
 * no-hello-keys.c: stub for windows GUI targets that share window.c but
 * have no SSH key authentication (pterm, the telnet build) - the
 * Hello-protected-keys hook installation is a no-op there.
 */
#include <windows.h>

void kitty_hello_terminal_init(HWND terminal_window)
{
    (void)terminal_window;
}
