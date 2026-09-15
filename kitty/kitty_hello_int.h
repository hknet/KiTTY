/*
 * kitty_hello_int.h - what kitty_hello.c (the credential code) and
 * kitty_hello_host.c (the anchor host window) share and nothing else needs:
 * the anchor thread that hosts the window while a credential call blocks the
 * calling thread, and the host window itself.
 */
#ifndef KITTY_HELLO_INT_H
#define KITTY_HELLO_INT_H

#include <windows.h>
#include <stdbool.h>

struct khw_anchor {
    HANDLE ready;              /* window created (or failed) */
    HANDLE thread;
    HWND hwnd;                 /* NULL = creation failed */
    DWORD tid;
};

/* The anchor: the host window on its own pumping thread, started before a
 * credential call and stopped after it (hwnd NULL = creation failed). */
void khw_anchor_start(struct khw_anchor *a);
void khw_anchor_stop(struct khw_anchor *a);
/* The host window itself, for the calls that pump on the calling thread. */
HWND khw_host_create(void);
void khw_host_destroy(HWND w);
/* Is the caller's window good enough to anchor the credential UI - a visible
 * window of THIS process, and no context line set? */
bool khw_owner_usable(HWND w);

#endif /* KITTY_HELLO_INT_H */
