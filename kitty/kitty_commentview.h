/*
 * kitty_commentview.h - the Session panel's comment box: the selected
 * session's comment, wrapped to the box, its links (web, file://, UNC)
 * underlined and opened with a click by the program Windows has
 * registered for them. Read-only; the comment is edited on
 * Session > Comment & Lock.
 *
 * It takes the place of the read-only EDIT (dialog.h editbox.linkview,
 * windows/controls.c) and answers WM_SETTEXT / WM_GETTEXT as the EDIT did,
 * so dlg_editbox_set / dlg_editbox_get reach it unchanged.
 */
#ifndef KITTY_COMMENTVIEW_H
#define KITTY_COMMENTVIEW_H

#include <windows.h>
#include <stdbool.h>

/* The window class; windows/controls.c spells the same name. */
#define KITTY_COMMENTVIEW_CLASS "KiTTYCommentView"

/* Register the class (once; later calls only answer). False when it could
 * not be registered - the panel then keeps the plain read-only box. */
bool kitty_commentview_register(void);

/*
 * Messages for automated tests, answered in every build: they read the
 * box or do what its menu does, nothing more.
 * Coordinates are the box's client pixels.
 *
 *  KCV_GETINFO   wParam 0 links, 1 lines, 2 first line shown, 3 lines that
 *                fit, 4 placeholder shown (1/0)
 *  KCV_HITTEST   lParam MAKELPARAM(x, y): the link there + 1, 0 none
 *  KCV_LINKPOINT wParam a link: MAKELRESULT(x, y), the middle of its first
 *                piece on screen; -1 when it is not on screen
 *  KCV_COPY      wParam a link, or -1 for the whole comment ("Copy link",
 *                "Copy comment"): 1 copied, 0 not
 */
#define KCV_GETINFO   (WM_APP + 0x3C0)
#define KCV_HITTEST   (WM_APP + 0x3C1)
#define KCV_LINKPOINT (WM_APP + 0x3C2)
#define KCV_COPY      (WM_APP + 0x3C3)

#endif
