/*
 * kitty_settings.h - the forced session read and write paths (kitty_settings_load.c,
 * kitty_settings_forced.c): the .ktx export and import that write and read
 * every setting regardless of its default.
 */

#ifndef KITTY_SETTINGS_H
#define KITTY_SETTINGS_H
#include "putty.h"
#include "storage.h"

/* ---- exported from kitty/kitty_settings_load.c ---- */
Filename *read_setting_filename_forced(void *handle, const char *key);
FontSpec *read_setting_fontspec_forced(void *handle, const char *name);
int read_setting_i_forced(void *handle, const char *key, int defvalue);
char *read_setting_s_forced(void *handle, const char *key);

#endif /* KITTY_SETTINGS_H */
