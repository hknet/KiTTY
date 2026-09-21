/*
 * kitty_theme.h - dark-mode support for KiTTY's Win32 dialogs.
 *
 * Three-valued preference (follow the system / force light / force dark) plus
 * the plumbing needed to make a plain dialog template honour it. Everything
 * that is not available on the running Windows degrades to "light", so a
 * Win7/2008R2 build takes this code and simply never goes dark.
 */
#ifndef KITTY_THEME_H
#define KITTY_THEME_H

#include <windows.h>
#include <stdbool.h>

/* Preference values. Stored as a DWORD, so the numbering is on the wire. */
#define KITTY_THEME_SYSTEM 0
#define KITTY_THEME_LIGHT  1
#define KITTY_THEME_DARK   2

/*
 * Is dark mode possible at all here? False on anything before Windows 10
 * 1809, and on a build where the undocumented uxtheme entry points have
 * moved. Callers use it to grey the preference control rather than offering
 * a setting that cannot do anything.
 */
bool kitty_theme_available(void);

/* Does the system itself currently ask for dark? (AppsUseLightTheme == 0.) */
bool kitty_theme_system_is_dark(void);

/* Resolve a preference to the colours actually to be painted. */
bool kitty_theme_dark_for(int pref);

/*
 * The PROCESS-wide app mode, which is what paints the popup menus: context
 * menus, menu-bar drop-downs, submenus, the system menu and tray menus are
 * drawn by Windows itself (window class #32768), dark or light according to
 * this mode, with the text, checkmarks, arrows and mnemonics Windows draws.
 * Dark -> ForceDark, light -> ForceLight, system -> AllowDark (follows the
 * system while running). Call once at startup, before the first menu exists,
 * and again whenever the preference changes. A no-op before Windows 10 1809
 * and wherever the undocumented entry points are missing.
 */
void kitty_theme_app_mode(int pref);
/* The system switched between light and dark: drop the cached menu theme, so
 * a long-lived process following the system follows it NOW. Call it on
 * WM_SETTINGCHANGE "ImmersiveColorSet". */
void kitty_theme_system_changed(void);
/* Is this WM_SETTINGCHANGE that switch? Compares the area name in lParam in
 * the receiving window's own character set - a Unicode window gets a wide
 * string whatever the program was compiled as. */
bool kitty_theme_is_system_switch(HWND w, LPARAM lParam);

/*
 * The preference's wire form, shared by every store so a value written by one
 * binary reads back the same in the next. from_string returns -1 for anything
 * it does not recognise, which is how a caller tells "not set" or "set to
 * nonsense" from a real choice.
 */
int kitty_theme_pref_from_string(const char *s);
const char *kitty_theme_pref_to_string(int pref);

/*
 * Apply a resolved theme to a dialog and everything in it: the title bar, the
 * per-control themes, and the brushes kitty_theme_ctlcolor() will hand back.
 * Safe to call again on the same window - that is how a live preview works.
 */
void kitty_theme_apply(HWND dlg, bool dark);
/* The same for a preference the dialog shows but has NOT stored yet (a theme
 * droplist before OK). The window then follows THAT preference, not the stored
 * one, on a system theme switch and on its activations, until it is gone. */
void kitty_theme_preview(HWND dlg, int pref);
/* The same, in the theme the dialog hook would give this window on its
 * activation (kitty_theme_hook_dialogs' resolver). For a dialog that builds
 * its controls hidden and wants its FIRST frame themed: called at the end of
 * WM_INITDIALOG, before the window is shown. Does nothing without a hook. */
void kitty_theme_apply_hooked(HWND dlg);
/* A tooltip in the theme. It is a top-level popup that kitty_theme_apply's
 * pass over the children never reaches: its creator calls this, again
 * whenever the tip is about to be shown, so it follows a live theme change. */
void kitty_theme_tooltip(HWND tip, bool dark);
/* A non-dialog window's title bar and border in the theme (the terminal):
 * dark = the theme's caption colours, light = the system's default. */
void kitty_theme_frame(HWND w, bool dark);

/*
 * Re-theme the children of a window kitty_theme_apply() has already been
 * given. A dialog that builds controls after it first appeared - the
 * configuration box, whose panels are created on demand and cached - has
 * children the first pass never saw, and everything that must be SENT to a
 * control (its theme class, a tree view's own colours) reaches only what
 * existed at the time. Cheap, and safe to call as often as panels are built.
 */
void kitty_theme_refresh(HWND dlg);

/*
 * WM_CTLCOLOR* handler. Returns the brush to use (already cast), or NULL when
 * the message is not one of ours or the window is not dark - in which case the
 * dialog proc must fall through to the default handling.
 */
HBRUSH kitty_theme_ctlcolor(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam);

/* The window background brush for a dark window, for WM_ERASEBKGND and for
 * owner-drawn pieces. NULL when the window is not dark. */
HBRUSH kitty_theme_backbrush(HWND dlg);

/* Colours a caller needs when it draws something itself (the tab strip). */
COLORREF kitty_theme_text_colour(bool dark);

/* Is this window currently painted dark? For code that draws its own rows and
 * has to pick colours - a list view's custom draw cannot ask the brush. */
bool kitty_theme_window_dark(HWND w);

/* Config-box category tree: paint the selected row in full highlight colour
 * even when the tree is unfocused (dialog.c forwards NM_CUSTOMDRAW here).
 * Guarded: the tree custom-draw types need commctrl.h, which not every
 * includer of this header pulls in. */
#ifdef NM_CUSTOMDRAW
LRESULT kitty_theme_tree_customdraw(LPNMTVCUSTOMDRAW cd);
#endif

/*
 * The meanings a row of text can carry, so the places that colour their own
 * rows name the MEANING and let this module pick the colour for the theme in
 * force. The alternative - a literal RGB at each site - is what left the
 * agent log painting dark green on white inside a dark window.
 */
typedef enum {
    KITTY_INK_NORMAL,
    KITTY_INK_GOOD,     /* loaded, allowed, done */
    KITTY_INK_WARN,     /* unavailable, rotated, degraded */
    KITTY_INK_BAD,      /* denied, blocked, failed */
    KITTY_INK_INFO      /* agent lifecycle, retries */
} kitty_ink;
COLORREF kitty_theme_ink(bool dark, kitty_ink which);

/*
 * Mark one STATIC control as carrying a meaning, so its text is drawn in the
 * ink for the theme in force - the warning line of a confirmation or of a
 * permission request, and anything else that is not ordinary text.
 *
 * The dialog itself does not answer WM_CTLCOLORSTATIC for such a line any
 * more: the mark is read where every other colour decision is already made. A
 * private handler cannot work anyway, because while the window is dark this
 * module answers that message before the dialog procedure ever sees it -
 * which is how three hand-written red lines came to lose their colour in dark
 * mode, each of them returning the LIGHT system face brush as well.
 *
 * Call once per control, after it has its text; the mark stays on the control
 * and survives a re-theme.
 */
void kitty_theme_mark_ink(HWND ctl, kitty_ink which);

/*
 * The width a push button needs for the caption it is CARRYING RIGHT NOW.
 *
 * A template gives a button the width its template caption needed, and a
 * button whose caption is set (or whose wording is changed) at run time is
 * then too narrow for it: the label does not fit the content rectangle and
 * what reaches the screen is a half-drawn caption. So a window that lays its
 * own buttons out sizes them from the measurement, never from the template.
 *
 * Measured with the button's own font, so it is right at any DPI and any
 * font. `min_w` is the floor - pass the template's width to keep a row of
 * buttons from turning ragged where the captions are short.
 */
int kitty_theme_button_width(HWND btn, int min_w);

/* A list row's background. `alternate` is the banding every other row. */
COLORREF kitty_theme_row_colour(bool dark, bool alternate);

/*
 * Take over the painting of a tab strip.
 *
 * SysTabControl32 is the one common control with NO dark theme: handed any
 * theme class it still paints its strip from the light system face, which
 * puts a white band across an otherwise dark dialog. This subclasses the
 * control and draws the strip itself while the window is dark, and passes
 * everything through unchanged while it is light - so the classic look stays
 * exactly the control's own and only dark mode is hand-drawn.
 *
 * Call it once, after the tabs have been inserted. Calling it again on the
 * same control is harmless.
 */
void kitty_theme_attach_tabs(HWND tab);

/*
 * Theme EVERY dialog this thread creates, from now on - including the ones
 * whose window procedure lives inside Windows, which is what makes the modal
 * message boxes follow the theme without rewriting their call sites.
 *
 * `want_dark` is called each time a dialog appears and answers whether it
 * should be dark. It is passed in rather than read here because this module
 * has no business knowing where the preference is stored.
 *
 * Call once, at startup, before any window exists. Safe to call again.
 *
 * The common file dialogs are deliberately NOT touched: they are the same
 * window class but are full of list views and toolbars, and the shell already
 * themes them itself.
 */
void kitty_theme_hook_dialogs(bool (*want_dark)(void));

/*
 * Name another window class the hook should treat as one of our dialogs.
 *
 * The hook recognises "#32770", which is every window built from a dialog
 * template - but a dialog created with a class of its own is not that class,
 * and KiTTY's configuration box is exactly that. Without this it is skipped
 * and comes up light inside an otherwise dark application.
 *
 * Deliberately an explicit list rather than "any window this process owns":
 * the terminal is also our window and must keep the colours its session asks
 * for. Call before the class's first window exists; a name registered twice
 * is ignored.
 */
void kitty_theme_hook_class(const char *classname);

/*
 * The same for a PLAIN window class - one whose procedure is not the dialog
 * manager's (DefWindowProc, a class background brush), such as the kscp
 * transfer window. Such a window never sends WM_CTLCOLORDLG: its background is
 * the class brush, painted on WM_ERASEBKGND, so the hook also fills that with
 * the dark background while the window is dark. Its child controls are themed
 * exactly as a dialog's are.
 */
void kitty_theme_hook_window_class(const char *classname);

/* Forget the per-window state when a themed dialog is destroyed. */
void kitty_theme_forget(HWND dlg);

/*
 * THE ROW-ALIGNER, once for every window in the suite.
 *
 * A template cannot line a label up with the field beside it: a closed
 * combo sizes itself from the font whatever height it is given, an edit
 * box draws its text at the top of whatever height it is given, and
 * dialog units round differently per control class - at 200% the
 * difference is a visible step. So the FIELD's real rectangle is the
 * row, and every other control of the row (labels with SS_CENTERIMAGE,
 * checkboxes, buttons - they centre their content anyway) is moved to
 * that top and height. Call it in WM_INITDIALOG, BEFORE any layout
 * capture that resizes replay (anchors would scatter the row again).
 *
 * ids: the other controls of the row, terminated by 0. A missing id is
 * skipped, so one table serves a template whose rows vary by mode.
 */
void kitty_theme_align_row(HWND dlg, int field_id, const int *ids);

/* Several rows at once: {field, {labels..., 0}} entries, n of them. */
struct kitty_theme_row { int field; int ids[6]; };
void kitty_theme_align_rows(HWND dlg, const struct kitty_theme_row *rows,
                            size_t n);

#endif /* KITTY_THEME_H */
