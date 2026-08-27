/*
 * kitty_theme_pref.h - where the satellite binaries keep the colour theme.
 *
 * The preference is application-wide: one `[KiTTY] theme` key and one registry
 * value, read by kageant and kittygen through here and by kitty itself through
 * its own ReadParameter(). It is not per-tool, so setting it in one place
 * changes all of them.
 *
 * kitty.exe does NOT use this file: it has the full settings machinery, which
 * knows about session hives, portable modes and the read-only flag. The two
 * paths agree because they read the same value name in the same hive and the
 * same key in the same section, in the same string form.
 */
#ifndef KITTY_THEME_PREF_H
#define KITTY_THEME_PREF_H

#include <stdbool.h>

/* One of the KITTY_THEME_* values; KITTY_THEME_SYSTEM when nothing is set. */
int kitty_theme_pref_get(void);

/* Store it in both places, the way every other satellite setting is stored. */
void kitty_theme_pref_set(int pref);

/* The resolver to hand to kitty_theme_hook_dialogs(). */
bool kitty_theme_pref_dark(void);

#endif /* KITTY_THEME_PREF_H */
