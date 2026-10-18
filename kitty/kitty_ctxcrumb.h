/*
 * kitty_ctxcrumb.h - the OSC 3008 breadcrumb (kitty_ctxcrumb.c): one line in
 * the title bar or at the top right of the terminal area -
 * "alice@web01 > root > nginx-test" - while
 * the host's contexts show that something has switched (root, another user, a
 * container, a VM, ssh onward). Information, never a security signal.
 */
#ifndef KITTY_CTXCRUMB_H
#define KITTY_CTXCRUMB_H

#include <stdbool.h>
#include <windows.h>
#include "kitty_ctx3008.h"

/* The chain changed, the terminal changed screens, or the setting changed
 * (window.c). on: the session shows it (ContextBreadcrumb); alt: the
 * alternate screen is up (it hides there only in the terminal area);
 * shown with two segments or more. session: where its in-window place is
 * kept; line_px: the terminal's line height in pixels. */
void kitty_ctxcrumb_update(const CtxSeg *seg, int n, bool on, bool alt,
                           const char *session, int line_px);

/* The terminal window moved, was resized, minimised or restored. */
void kitty_ctxcrumb_follow(void);

/* The terminal window is being shown or hidden (WM_SHOWWINDOW: the tray). */
void kitty_ctxcrumb_owner_shown(bool shown);

/* The application theme or [KiTTY] ctxbreadcrumbopacity changed: drawn again. */
void kitty_ctxcrumb_restyle(void);

/* Its screen rectangle while it is shown IN THE TERMINAL AREA (the top
 * right, ctxbreadcrumbplace=window or no title bar): the program status
 * panel keeps clear of it. False over the title bar or when hidden. */
bool kitty_ctxcrumb_rect(RECT *r);

#endif
