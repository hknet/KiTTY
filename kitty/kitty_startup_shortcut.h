/*
 * kitty_startup_shortcut.h: create/remove a Windows Startup-folder shortcut
 * (.lnk) for binaries that want a registry-free autostart - kageant in
 * portable mode, and the launcher on request. See kitty_startup_shortcut.c.
 */

#ifndef KITTY_STARTUP_SHORTCUT_H
#define KITTY_STARTUP_SHORTCUT_H

#include <stddef.h>

/* Absolute path of the current user's Startup folder (common != 0 = the
 * all-users Startup folder), held whole (sfree it); NULL on failure. */
char *kitty_startup_dir_dup(int common);

/* Create (on != 0) or delete (on == 0) "<per-user Startup>\<name>.lnk"
 * pointing at target/args (workdir/icon may be NULL - icon defaults to the
 * target exe's own icon). Creation overwrites an existing shortcut of the
 * same name, so it is idempotent. Returns 1 on success. */
int kitty_startup_shortcut_set(const char *name, const char *target,
                               const char *args, const char *workdir,
                               const char *icon, int on);

/* Write the shortcut file lnkpath (any folder; overwritten when present):
 * target with args, workdir (NULL = none), its icon from icon (NULL = the
 * target) at icon_index - a NEGATIVE index is a resource id, so -9907 is
 * IDI_MANAGEICON inside kitty.exe. Returns 1 on success. Used by the Startup
 * shortcuts above and by System > Shortcuts (kitty_config_app.c). */
int kitty_shortcut_write(const char *lnkpath, const char *target,
                         const char *args, const char *workdir,
                         const char *icon, int icon_index);

/* 1 when "<per-user Startup>\<name>.lnk" exists (for a menu checkmark). */
int kitty_startup_shortcut_exists(const char *name);

/* The target exe path of the .lnk at lnkpath, held whole (sfree it); NULL
 * when none was read. Used by the kageant autostart-conflict scan, System >
 * Shortcuts and the updater's install-type test. */
char *kitty_startup_shortcut_target_dup(const char *lnkpath);

/* 1 when "<Startup>\<name>.lnk" (common != 0 = all-users) exists AND its
 * target resolves to target_exe. Lets a caller distinguish "I am set to
 * autostart" from "some other install of the same name is". */
int kitty_startup_shortcut_points_to(const char *name, int common,
                                     const char *target_exe);

#endif
