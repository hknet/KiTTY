/*
 * windows/utils/defaults.c: default settings that are specific to Windows.
 */

#include "putty.h"

#include <commctrl.h>

FontSpec *platform_default_fontspec(const char *name)
{
    if (!strcmp(name, "Font"))
        return fontspec_new("Courier New", false, 10, ANSI_CHARSET);
    else
        return fontspec_new_default();
}

Filename *platform_default_filename(const char *name)
{
    /* KiTTY: a log path that works as it stands, one folder per host and a
     * file per day - instead of "putty.log" in whatever the current folder
     * happens to be. Logging itself stays off by default; this is what the
     * box offers once it is switched on. The folders are made at the first
     * open (filename_make_parent_dirs, hknet/KiTTY#65). */
    if (!strcmp(name, "LogFileName"))
        return filename_from_str("%APPDATA%\\KiTTY++\\logs\\&H\\&Y-&M-&D.log");
    else
        return filename_from_str("");
}

char *platform_default_s(const char *name)
{
    if (!strcmp(name, "SerialLine"))
        return dupstr("COM1");
    return NULL;
}

bool platform_default_b(const char *name, bool def)
{
    return def;
}

int platform_default_i(const char *name, int def)
{
    return def;
}
