/*
 * kitty_defs.h - small, dependency-free KiTTY constants shared across the port.
 *
 * Keep this header free of includes so it can be pulled into any translation
 * unit (including windows/storage.c in libsettings) at zero cost.
 */
#ifndef KITTY_DEFS_H
#define KITTY_DEFS_H

/*
 * The canonical name of PuTTY/KiTTY's default-settings pseudo-session. This is
 * a FIXED contract, not a renameable label: it is the registry/file key the
 * default settings live under, and upstream PuTTY hardcodes the same literal in
 * settings.c / config.c / do_defaults. Changing the value would orphan every
 * existing default-settings entry and break PuTTY parity, so this macro exists
 * only to give KiTTY-added code typo-safety and a single, documented reference -
 * NOT to make the name changeable. Upstream/core files keep the bare literal to
 * stay rebase-clean against PuTTY.
 */
#define KITTY_DEFAULT_SESSION "Default Settings"

/*
 * How many per-session global hotkeys the launcher can hold at once (the size
 * of its RegisterHotKey id range). The config box refuses to enable a hotkey
 * beyond this count, so both sides must share this single definition.
 */
#define KITTY_LAUNCHER_HOTKEY_MAX 32

/*
 * The saved-session list's length in rows ([ConfigBox] height): one clamp,
 * shared by the live setter, the panel builder and the panel's own label
 * text. The floor is what still holds the button column beside the list
 * (Load, Delete, Del folder - Export/Import live on Application/Migration);
 * the ceiling only keeps a typo from asking for a window taller than any
 * screen.
 */
#define KITTY_CFG_SESSION_ROWS_MIN 6
#define KITTY_CFG_SESSION_ROWS_MAX 60

/*
 * The configuration window's template height in dialog units - its MINIMUM,
 * since windows/dialog.c floors every resize at the template. ONE definition
 * for the two places that must agree: the IDD_MAINBOX line in
 * windows/putty-common.rc2 and the layout constants in windows/dialog.c
 * (CFGBOX_H), which place the button row and the tree against this height.
 * 320 -> 306 once the Session panel gave a comment line back and the
 * proxy-choice row became conditional on named proxies existing.
 * 278 -> 216 when the on-exit block (a captionless box holding the
 * close-on-exit radio line and the save-on-exit checkbox: 8 + 21 + 11 + 6
 * units by the windows/controls.c constants) moved to Window > Behaviour,
 * and the Session panel in folder-rows navigation (no folder dropdown, one
 * combo row = 16 units less) became the layout the minimum is fitted to,
 * with the list at its six-row floor. The list then grows into any height
 * the box has to spare (kitty_cfg_panel_fill); every other panel scrolls
 * when it does not fit.
 */
#define KITTY_CFGBOX_H_DU 216

/* Two-step stringification, so a macro's VALUE lands in a string literal
 * (KITTY_STR(KITTY_CFG_SESSION_ROWS_MIN) -> "6"). */
#define KITTY_STR_(x) #x
#define KITTY_STR(x) KITTY_STR_(x)

#endif /* KITTY_DEFS_H */
