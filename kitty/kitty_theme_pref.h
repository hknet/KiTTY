/*
 * kitty_theme_pref.h - where the satellite binaries keep the colour theme.
 *
 * The preference is application-wide: one `[KiTTY] theme` key and one registry
 * value, read by kageant and kittygen through here and by kitty itself through
 * its own ReadParameter(). It is not per-tool, so setting it in one place
 * changes all of them.
 *
 * kitty.exe reads the theme through here only before its settings are loaded
 * (the dialog hook goes in first); it writes it through its own
 * WriteParameter. The two paths agree because they read the same value name
 * in the same hive and the same key in the same section, in the same string
 * form, and both leave kitty.ini alone under readonly=yes.
 */
#ifndef KITTY_THEME_PREF_H
#define KITTY_THEME_PREF_H

#include <stdbool.h>

/* One of the KITTY_THEME_* values; KITTY_THEME_SYSTEM when nothing is set. */
int kitty_theme_pref_get(void);

/* Store it in the store in charge only - kitty.ini when that is
 * authoritative, else the registry - as every other satellite setting.
 * readonly=yes: not stored, 1 (kitty_inifile_write). */
int kitty_theme_pref_set(int pref);

/* The resolver to hand to kitty_theme_hook_dialogs(). */
bool kitty_theme_pref_dark(void);

#endif /* KITTY_THEME_PREF_H */
