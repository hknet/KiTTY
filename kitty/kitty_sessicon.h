/*
 * KiTTY: session icons in the session lists (hknet/KiTTY#62). With
 * [KiTTY] sessionicons=yes (Application > Appearance > Icons) the launcher's
 * session menu and Manage Sessions show each session's icon in front of its
 * name: the icon its terminal window shows (IconeFile when that file exists,
 * else the built-in icon Icone, 0 = the default), read through Default
 * Settings when the session inherits from it. In dark mode an icon file
 * <name>.dark.ico beside <name>.ico is taken instead, when it exists.
 *
 * The icons are cached per source and size for the life of the process and
 * owned by the cache: callers never destroy them.
 */
#ifndef KITTY_SESSICON_H
#define KITTY_SESSICON_H

#include <stdbool.h>
#include <windows.h>

/* The setting, read from the store at each call. */
bool kitty_session_icons_on(void);

/* KiTTY++'s own theme ([KiTTY] theme) paints dark now: the lists without a
 * window of their own to query (the launcher's menu) use this. */
bool kitty_session_icons_dark(void);

/* The session's icon, px pixels square, for the dark or the light theme.
 * NULL only when not even the default icon loads. */
HICON kitty_session_icon(const char *session, int px, bool dark);

/* The same icon as a 32-bit premultiplied ARGB bitmap for a menu item
 * (MENUITEMINFO.hbmpItem). NULL before Vista (a menu there draws no alpha)
 * or when nothing loads. */
HBITMAP kitty_session_icon_bitmap(const char *session, int px, bool dark);

/* An icon LIBRARY in a session's IconeFile: a .dll, .exe or .icl (by its
 * ending). The session's number then picks the icon in that file: number n
 * (1 and up, 0 = 1) is the file's icon n - 1 in the order Windows lists
 * them (PrivateExtractIcons). Any other file is one picture, an .ico. */
bool kitty_icon_is_library(const char *path);
/* The icon at index (0 = the first) of a library, px square. NULL when the
 * file has none there. The caller destroys it. */
HICON kitty_icon_from_library(const char *path, int index, int px);
/* How many icons a library holds (0 = none or not readable). */
int kitty_icon_library_count(const char *path);

#endif
