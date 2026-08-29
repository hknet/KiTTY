#ifndef KITTY_ANCHOR_H
#define KITTY_ANCHOR_H
#include <windows.h>
#include <stddef.h>

/* Self-contained (Win32 only, no KiTTY deps) edge anchoring for resizable
 * dialogs: capture where every control sits at the size the template gave it,
 * then re-place them all against the current client size.
 *
 * An anchored edge FOLLOWS the window's edge. Anchoring both LEFT and RIGHT
 * therefore stretches the control horizontally; anchoring RIGHT alone moves it;
 * anchoring LEFT alone (the default, since 0 anchors nothing) leaves it where
 * it is. Same for TOP/BOTTOM vertically.
 *
 * Two ways in, over one implementation:
 *
 *  - the ID form, for a dialog whose controls all have template ids. The ids
 *    are re-resolved at every relayout, so a control the dialog destroyed (a
 *    Help button on a build with no help) simply drops out.
 *  - the HWND form, for windows that are not dialog items of their parent -
 *    a child dialog used as a panel host, a control created at runtime, or
 *    controls whose ids were handed out dynamically and are not in a table.
 *
 * Used by kageant's key list, key details and audit windows, and by the
 * configuration box.
 */

#define KL_ANCH_LEFT   1
#define KL_ANCH_TOP    2
#define KL_ANCH_RIGHT  4
#define KL_ANCH_BOTTOM 8

struct kl_anchor     { int id;    unsigned anchor; };
struct kl_anchor_win { HWND hwnd; unsigned anchor; };

/* Capture the current client size, each anchored control's rect, and the
 * window size (= the minimum) as the baseline a relayout re-places against.
 * `rects` must have room for `n` entries and belongs to the caller. */
void anchored_capture(HWND hwnd, const struct kl_anchor *anchors, size_t n,
                      RECT *rects, SIZE *basesize, SIZE *minsize);
void anchored_relayout(HWND hwnd, const struct kl_anchor *anchors, size_t n,
                       const RECT *rects, SIZE basesize);

void anchored_capture_windows(HWND parent, const struct kl_anchor_win *anchors,
                              size_t n, RECT *rects, SIZE *basesize,
                              SIZE *minsize);
void anchored_relayout_windows(HWND parent, const struct kl_anchor_win *anchors,
                               size_t n, const RECT *rects, SIZE basesize);

#endif
