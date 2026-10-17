/*
 * kitty_panel_path.h - old configuration-box panel paths, mapped to today's.
 *
 * A panel's path ("Window/Copy & Paste") is the name the Category tree shows,
 * component for component: documentation, -cfgpanel and the QA harnesses all
 * write the path a user sees. Six panels had a code name that differed from
 * their tree name until 0.85.1.18; their old paths stay accepted here, so a
 * "-cfgpanel Window/Selection" in a shortcut or script, a remembered panel
 * in kitty.ini, and upstream PuTTY code that still builds "Window/Selection"
 * all land on the panel of today's name.
 *
 * Matched as a path prefix on whole components; the longer entry first.
 */

typedef struct {
    const char *old_path;
    const char *path;
} KittyPanelAlias;

static const KittyPanelAlias kitty_panel_aliases[] = {
    { "Window/Selection/Copy",                  "Window/Copy & Paste/Character classes" },
    { "Window/Selection",                       "Window/Copy & Paste" },
    { "Window/Title",                           "Window/Title & Icon" },
    { "Connection/Transfers",                   "Connection/File-Transfer-Settings" },
    { "Terminal/Keyboard/Application keypad",   "Terminal/Keyboard/App Keypad & Cursor" },
    { "Application/Migration/old KiTTY Folders", "Application/Migration/(old) KiTTY Folders" },
};

/* Today's path for `path`: `path` itself when no alias applies, else the
 * rewritten path in `buf` (or `path` again if it does not fit). */
static inline const char *kitty_panel_path_resolve(const char *path,
                                                   char *buf, size_t size)
{
    if (!path)
        return path;
    for (size_t i = 0; i < sizeof(kitty_panel_aliases)/sizeof(*kitty_panel_aliases); i++) {
        const char *old = kitty_panel_aliases[i].old_path;
        size_t n = strlen(old);
        if (strncmp(path, old, n) || (path[n] && path[n] != '/'))
            continue;
        if (strlen(kitty_panel_aliases[i].path) + strlen(path + n) >= size)
            return path;
        strcpy(buf, kitty_panel_aliases[i].path);
        strcat(buf, path + n);
        return buf;
    }
    return path;
}
