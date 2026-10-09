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

/*
 * KiTTY: KITTYPP_DIR is the running program's own folder, set in its own
 * environment at every start (each suite program calls this first). Fresh
 * each time, so "%KITTYPP_DIR%\..." in any file or folder setting follows a
 * USB stick to another drive letter or a folder mounted somewhere else -
 * every such setting is expanded against this environment when it is used
 * (filename.c), and child processes inherit it. No trailing backslash, the
 * drive root included ("E:"), so "%KITTYPP_DIR%\logs" is always one
 * separator.
 */
void kitty_publish_kittypp_dir(void)
{
    wchar_t p[MAX_PATH + 1], *s;
    DWORD n = GetModuleFileNameW(NULL, p, MAX_PATH);

    if (n == 0 || n >= MAX_PATH || !(s = wcsrchr(p, L'\\')))
        return;
    *s = L'\0';
    SetEnvironmentVariableW(L"KITTYPP_DIR", p);
}

/* KiTTY: whether the settings travel with the program (a folder store or
 * savemode=file, the portable build) - set at startup by the terminals and
 * the console tools. The log default then stays beside the program instead
 * of in this Windows account's profile. */
static bool log_default_beside_program = false;
void kitty_set_log_default_beside_program(bool on)
{
    log_default_beside_program = on;
}

Filename *platform_default_filename(const char *name)
{
    /* KiTTY: a log path that works as it stands, one folder per host and a
     * file per day - instead of "putty.log" in whatever the current folder
     * happens to be. Logging itself stays off by default; this is what the
     * box offers once it is switched on. The folders are made at the first
     * open (filename_make_parent_dirs, hknet/KiTTY#65). */
    if (!strcmp(name, "LogFileName"))
        return filename_from_str(log_default_beside_program ?
            "%KITTYPP_DIR%\\logs\\&H\\&Y-&M-&D.log" :
            "%APPDATA%\\KiTTY++\\logs\\&H\\&Y-&M-&D.log");
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
