/*
 * kitty_startup_shortcut.h: create/remove a Windows Startup-folder shortcut
 * (.lnk) for binaries that want a registry-free autostart - kageant in
 * portable mode, and the launcher on request. See kitty_startup_shortcut.c.
 */

#ifndef KITTY_STARTUP_SHORTCUT_H
#define KITTY_STARTUP_SHORTCUT_H

#include <stddef.h>

/* Absolute path of the current user's Startup folder (common != 0 = the
 * all-users Startup folder). Returns 1 on success. */
int kitty_startup_dir(char *out, size_t len, int common);

/* Create (on != 0) or delete (on == 0) "<per-user Startup>\<name>.lnk"
 * pointing at target/args (workdir/icon may be NULL - icon defaults to the
 * target exe's own icon). Creation overwrites an existing shortcut of the
 * same name, so it is idempotent. Returns 1 on success. */
int kitty_startup_shortcut_set(const char *name, const char *target,
                               const char *args, const char *workdir,
                               const char *icon, int on);

/* 1 when "<per-user Startup>\<name>.lnk" exists (for a menu checkmark). */
int kitty_startup_shortcut_exists(const char *name);

/* Read the target exe path of the .lnk at lnkpath into out (used by the
 * kageant autostart-conflict scan). Returns 1 when a target was read. */
int kitty_startup_shortcut_target(const char *lnkpath, char *out, size_t len);

#endif
