/*
 * kitty_boxes-rc.h - the ids of the suite's shared boxes (the notice box and
 * the confirmation box, kitty/kitty_dlgbox.c. Templates in
 * kitty_boxes.rc2). Shared by kitty.exe (through kitty_rc_additions.h) and
 * kageant (pageant.rc, pageant.c), so both show the same themed boxes.
 */
#ifndef KITTY_BOXES_RC_H
#define KITTY_BOXES_RC_H

#ifndef IDD_NOTICEBOX
#define IDD_NOTICEBOX 146
#endif
#ifndef IDC_NOTICE_TEXT
#define IDC_NOTICE_TEXT 1274
#endif
/* Yes/No confirmation with an optional SECOND line in red. A real dialog rather
 * than MessageBox for two reasons, NEITHER of them DPI (a MessageBox is drawn by
 * Windows and scales correctly - see the corrected note in kitty_dlgbox.c): it grows
 * to fit a long explanation. A MessageBox cannot colour text. The red line is
 * for the case where the action would persist WITHOUT an explicit save, which the
 * ordinary wording of a confirmation understates. */
#ifndef IDD_CONFIRMBOX
#define IDD_CONFIRMBOX 147
#endif
#ifndef IDC_CONFIRM_TEXT
#define IDC_CONFIRM_TEXT 1275
#endif
#ifndef IDC_CONFIRM_WARN
#define IDC_CONFIRM_WARN 1276
#endif
/* The optional THIRD button of the shared confirm box: shown only in its
 * 3-way mode (Get File "files already exist"). Hidden for the Yes/No and info
 * boxes, which are byte-identical to before. */
#ifndef IDC_CONFIRM_THIRD
#define IDC_CONFIRM_THIRD 1277
#endif
/* The optional read-only DETAIL field of the shared confirm box (an OSC 8
 * link's target): hidden unless a box carries one. */
#ifndef IDC_CONFIRM_DETAIL
#define IDC_CONFIRM_DETAIL 1285
#endif
/* The optional CHECKBOX of the shared confirm box (the far2l clipboard box's
 * "Always allow this far2l"): hidden unless a box carries one. */
#ifndef IDC_CONFIRM_CHECK
#define IDC_CONFIRM_CHECK 1296
#endif

#endif
