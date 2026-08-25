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

#endif /* KITTY_DEFS_H */
