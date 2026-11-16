/*
 * kitty_sessicon.c - session icons in the session lists (hknet/KiTTY#62),
 * see kitty_sessicon.h. The launcher's session menu takes them as menu
 * item bitmaps, Manage Sessions as list view images.
 *
 * The source of a session's icon is the one its terminal window uses
 * (SetNewIcon, kitty/kitty.c): IconeFile, %VAR% expanded, when that file
 * exists, otherwise the built-in icon number Icone of the icon library
 * (hInstIcons, kitty++.dll). In dark mode <name>.dark.ico beside the file
 * wins when it exists. So does a built-in icon's dark form (resource id +
 * 1000) where the library has one. A .dll, .exe or .icl in IconeFile is a
 * library: the number picks the icon in it (kitty_icon_from_library). Single-threaded: the launcher
 * and Manage Sessions build their lists on their UI thread.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "putty.h"
#include "storage.h"

#include "kitty_sessicon.h"
#include "kitty_storage.h"       /* kitty_settings_r_inherit */
#include "kitty_params.h"        /* ReadParameterN */
#include "kitty_inikeys.h"       /* KI_SESSIONICONS */
#include "kitty_tools.h"         /* existfile */
#include "kitty_rc_additions.h"  /* IDI_MAINICON_0 */
#include "kitty_theme.h"         /* kitty_theme_dark_for */
#include "kitty_win.h"           /* kitty_theme_app_pref */

extern HINSTANCE hInstIcons;     /* kitty_params.c: the icon library */

bool kitty_session_icons_on(void)
{
    char v[16] = "";
    ReadParameterN(KI_SECTION_KITTY, KI_SESSIONICONS, v, sizeof(v));
    return stricmp(v, "yes") == 0;
}

bool kitty_icon_is_library(const char *path)
{
    const char *dot = path ? strrchr(path, '.') : NULL;
    const char *sl = path ? strrchr(path, '\\') : NULL;
    if (!dot || (sl && dot < sl))
        return false;
    return !stricmp(dot, ".dll") || !stricmp(dot, ".exe") || !stricmp(dot, ".icl");
}

/* user32's PrivateExtractIcons reads .exe and .dll (PE) and the 16-bit
 * .icl / .exe (NE) alike, at any size, on every Windows this suite loads on.
 * The file's code never runs. */
HICON kitty_icon_from_library(const char *path, int index, int px)
{
    HICON h = NULL;
    UINT id = 0;
    if (!path || !*path || index < 0)
        return NULL;
    if (PrivateExtractIconsA(path, index, px, px, &h, &id, 1, LR_DEFAULTCOLOR) != 1 ||
        h == (HICON)INVALID_HANDLE_VALUE)
        return NULL;
    return h;
}

int kitty_icon_library_count(const char *path)
{
    UINT n;
    if (!path || !*path)
        return 0;
    n = PrivateExtractIconsA(path, 0, 0, 0, NULL, NULL, 0, 0);
    return (n == (UINT)-1) ? 0 : (int)n;
}

bool kitty_session_icons_dark(void)
{
    return kitty_theme_dark_for(kitty_theme_app_pref());
}

/* In dark mode: <name>.dark.ico beside <name>.ico, when it exists. The
 * ending of the file is replaced (the last '.' after the last '\' or '/'),
 * a name without one gets ".dark.ico" added. NULL when there is none. */
static char *sic_dark_file(const char *file)
{
    const char *slash = strrchr(file, '\\'), *fwd = strrchr(file, '/');
    const char *dot = strrchr(file, '.');
    char *d;
    if (fwd && (!slash || fwd > slash))
        slash = fwd;
    if (dot && slash && dot < slash)
        dot = NULL;
    d = dot ? dupprintf("%.*s.dark.ico", (int)(dot - file), file)
            : dupprintf("%s.dark.ico", file);
    if (existfile(d))
        return d;
    sfree(d);
    return NULL;
}

/* One icon source at one size: "f<path>" or "n<number>". */
struct sic_entry {
    char *key;
    int px;
    HICON icon;
    HBITMAP bmp;
};
static struct sic_entry *sic = NULL;
static size_t sic_cap = 0;
static int sic_n = 0;

static struct sic_entry *sic_find(const char *key, int px)
{
    int i;
    for (i = 0; i < sic_n; i++)
        if (sic[i].px == px && !strcmp(sic[i].key, key))
            return &sic[i];
    return NULL;
}

/* A built-in icon: in dark mode its dark form (resource id + 1000, where the
 * library has one: kitty++.dll), else the icon. A number the library lacks
 * is icon 0, the library's, else the exe's own. */
static HICON sic_load_builtin(int num, int px, bool dark)
{
    HINSTANCE lib = hInstIcons ? hInstIcons : GetModuleHandle(NULL);
    HICON h = NULL;
    if (dark)
        h = (HICON)LoadImageA(lib, MAKEINTRESOURCE(IDI_MAINICON_0 + num + 1000),
                              IMAGE_ICON, px, px, 0);
    if (!h)
        h = (HICON)LoadImageA(lib, MAKEINTRESOURCE(IDI_MAINICON_0 + num),
                              IMAGE_ICON, px, px, 0);
    if (!h && num != 0)
        h = (HICON)LoadImageA(lib, MAKEINTRESOURCE(IDI_MAINICON_0),
                              IMAGE_ICON, px, px, 0);
    if (!h)
        h = (HICON)LoadImageA(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_MAINICON_0),
                              IMAGE_ICON, px, px, 0);
    return h;
}

/* The entry for a session's icon source, loaded on first use. */
static struct sic_entry *sic_entry_for(const char *session, int px, bool dark)
{
    settings_r *r;
    int num = 0;
    char *file = NULL, *key;
    struct sic_entry *e;

    if (px < 8)
        px = 8;
    r = open_settings_r(session);
    if (r) {
        Filename *fn;
        kitty_settings_r_inherit(r, session);
        num = read_setting_i(r, "Icone", 0);
        fn = read_setting_filename(r, "IconeFile");
        if (fn) {
            file = filename_expand_str(fn);
            filename_free(fn);
        }
        close_settings_r(r);
    }
    /* as the terminal window: the file when it is there, else the number */
    if (file && (!*file || !existfile(file))) {
        sfree(file);
        file = NULL;
    }
    if (file && dark && !kitty_icon_is_library(file)) {
        char *d = sic_dark_file(file);
        if (d) {
            sfree(file);
            file = d;
        }
    }
    num = num > 0 ? num - 1 : 0;
    /* a built-in icon with no dark form (resource id + 1000) is the same
     * picture in both themes: one cache entry, one bitmap */
    if (!file && dark) {
        HINSTANCE lib = hInstIcons ? hInstIcons : GetModuleHandle(NULL);
        if (!FindResourceA(lib, MAKEINTRESOURCE(IDI_MAINICON_0 + num + 1000),
                           MAKEINTRESOURCE(14) /* RT_GROUP_ICON */))
            dark = false;
    }
    /* a library: the number picks in it (no dark form there) */
    if (file && kitty_icon_is_library(file))
        key = dupprintf("l%s#%d", file, num);
    else
        key = file ? dupprintf("f%s", file) : dupprintf("n%d%s", num, dark ? "d" : "");
    if ((e = sic_find(key, px)) != NULL) {
        sfree(key);
        sfree(file);
        return e;
    }
    sgrowarray(sic, sic_cap, (size_t)sic_n);
    e = &sic[sic_n++];
    e->key = key;
    e->px = px;
    e->bmp = NULL;
    e->icon = NULL;
    if (file && kitty_icon_is_library(file))
        e->icon = kitty_icon_from_library(file, num, px);
    else if (file)
        e->icon = (HICON)LoadImageA(NULL, file, IMAGE_ICON, px, px,
                                    LR_LOADFROMFILE);
    if (!e->icon)       /* a file that is no icon: the default, no box */
        e->icon = sic_load_builtin(file ? 0 : num, px, dark);
    sfree(file);
    return e;
}

HICON kitty_session_icon(const char *session, int px, bool dark)
{
    return sic_entry_for(session, px, dark)->icon;
}

/* A 32-bit top-down DIB section, px square, zeroed. */
static HBITMAP sic_dib(HDC dc, int px, DWORD **bits)
{
    BITMAPINFO bi;
    HBITMAP bm;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = px;
    bi.bmiHeader.biHeight = -px;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)bits, NULL, 0);
    if (bm)
        memset(*bits, 0, (size_t)px * px * 4);
    return bm;
}

/* The icon's colour image with its alpha, premultiplied. An icon without an
 * alpha channel takes it from its mask. */
static HBITMAP sic_icon_to_pargb(HICON icon, int px)
{
    HDC sdc = GetDC(NULL), mdc;
    HBITMAP bm, mask = NULL;
    HGDIOBJ old;
    DWORD *bits, *mbits = NULL;
    int i, n = px * px, alpha = 0;

    if (!sdc)
        return NULL;
    mdc = CreateCompatibleDC(sdc);
    bm = sic_dib(sdc, px, &bits);
    if (!mdc || !bm) {
        if (bm)
            DeleteObject(bm);
        if (mdc)
            DeleteDC(mdc);
        ReleaseDC(NULL, sdc);
        return NULL;
    }
    old = SelectObject(mdc, bm);
    DrawIconEx(mdc, 0, 0, icon, px, px, 0, NULL, DI_IMAGE);
    GdiFlush();
    for (i = 0; i < n && !alpha; i++)
        alpha = (bits[i] & 0xFF000000) != 0;
    if (!alpha && (mask = sic_dib(sdc, px, &mbits)) != NULL) {
        SelectObject(mdc, mask);
        DrawIconEx(mdc, 0, 0, icon, px, px, 0, NULL, DI_MASK);
        GdiFlush();
    }
    SelectObject(mdc, old);
    DeleteDC(mdc);
    ReleaseDC(NULL, sdc);
    for (i = 0; i < n; i++) {
        DWORD p = bits[i], a;
        if (!alpha)     /* the mask: white = transparent */
            a = (mbits && (mbits[i] & 0x00FFFFFF)) ? 0 : 255;
        else
            a = p >> 24;
        bits[i] = (a << 24) |
            ((((p >> 16) & 0xFF) * a / 255) << 16) |
            ((((p >> 8) & 0xFF) * a / 255) << 8) |
            ((p & 0xFF) * a / 255);
    }
    if (mask)
        DeleteObject(mask);
    return bm;
}

HBITMAP kitty_session_icon_bitmap(const char *session, int px, bool dark)
{
    struct sic_entry *e;
    /* before Vista a menu draws a bitmap without its alpha */
    if (LOBYTE(LOWORD(GetVersion())) < 6)
        return NULL;
    e = sic_entry_for(session, px, dark);
    if (!e->bmp && e->icon)
        e->bmp = sic_icon_to_pargb(e->icon, e->px);
    return e->bmp;
}
