/*
 * kitty_startup_shortcut.c: registry-free autostart via a Startup-folder
 * shortcut. Used by kageant (portable mode) and the launcher (on request).
 * Creating/removing the .lnk is the caller's explicit action - this module
 * never scans or touches other autostart entries.
 */

#define COBJMACROS
#include "putty.h"
#include <windows.h>
#include <shlobj.h>
#include <objbase.h>
#include <string.h>
#include <stdio.h>

#include "kitty_oldwin.h"   /* paths held whole */
#include "kitty_startup_shortcut.h"

/* Every path here is held whole, in memory of its own (sfree it): the
 * shell's folder comes back wide, in the MAX_PATH characters the call is
 * made for, and is converted whole - in a double-byte or UTF-8 code page a
 * folder of fewer characters than MAX_PATH can take more bytes than that. */
char *kitty_startup_dir_dup(int common)
{
    WCHAR w[MAX_PATH + 1];
    int csidl = common ? CSIDL_COMMON_STARTUP : CSIDL_STARTUP;
    if (SHGetFolderPathW(NULL, csidl, NULL, SHGFP_TYPE_CURRENT, w) != S_OK)
        return NULL;
    w[MAX_PATH] = L'\0';
    return kitty_wide_to_acp_dup(w);
}

/* "<Startup>\<name>.lnk" (common != 0 = all-users), or NULL. */
static char *shortcut_path_in(const char *name, int common)
{
    char *dir = kitty_startup_dir_dup(common), *p;
    if (!dir)
        return NULL;
    p = dupprintf("%s\\%s.lnk", dir, name);
    sfree(dir);
    return p;
}

/* The per-user path; used for everything this module creates/removes. */
static char *shortcut_path(const char *name)
{
    return shortcut_path_in(name, 0);
}

static int shortcut_exists_in(const char *name, int common)
{
    char *lnk = shortcut_path_in(name, common);
    DWORD a;
    if (!lnk)
        return 0;
    a = GetFileAttributesA(lnk);
    sfree(lnk);
    return (a != INVALID_FILE_ATTRIBUTES) && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

int kitty_startup_shortcut_exists(const char *name)
{
    return shortcut_exists_in(name, 0);
}

/* Compare two exe paths, normalising 8.3/long form first (an MSI target may
 * be stored short while GetModuleFileName returns the long path). */
static int same_exe_path(const char *a, const char *b)
{
    /* the long forms whole; a path without one is compared as it is */
    char *la = kitty_long_path_dup(a), *lb = kitty_long_path_dup(b);
    int same = !stricmp(la ? la : a, lb ? lb : b);
    sfree(la);
    sfree(lb);
    return same;
}

int kitty_startup_shortcut_points_to(const char *name, int common,
                                     const char *target_exe)
{
    char *lnk = shortcut_path_in(name, common), *got;
    int same;
    if (!lnk)
        return 0;
    got = kitty_startup_shortcut_target_dup(lnk);
    sfree(lnk);
    if (!got)
        return 0;
    same = same_exe_path(got, target_exe);
    sfree(got);
    return same;
}

/* The longest target a shortcut can name, in wide characters. */
#define KSS_TARGET_MAX 32768

char *kitty_startup_shortcut_target_dup(const char *lnkpath)
{
    IShellLinkW *psl = NULL;
    IPersistFile *ppf = NULL;
    HRESULT hr;
    char *res = NULL;
    int couninit = 0;

    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (hr == S_OK || hr == S_FALSE)
        couninit = 1;
    else if (hr != RPC_E_CHANGED_MODE)
        return NULL;

    /* the wide interface, into a buffer of the longest path: the ANSI one
     * read MAX_PATH bytes, and a cut target could equal this exe's path */
    hr = CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IShellLinkW, (void **)&psl);
    if (SUCCEEDED(hr) && psl) {
        hr = IShellLinkW_QueryInterface(psl, &IID_IPersistFile, (void **)&ppf);
        if (SUCCEEDED(hr) && ppf) {
            WCHAR *wlnk = kitty_acp_to_wide_dup(lnkpath);
            if (wlnk && SUCCEEDED(IPersistFile_Load(ppf, wlnk, STGM_READ))) {
                WCHAR *buf = snewn(KSS_TARGET_MAX, WCHAR);
                buf[0] = L'\0';
                if (IShellLinkW_GetPath(psl, buf, KSS_TARGET_MAX, NULL, SLGP_RAWPATH)
                        == S_OK && buf[0] && wcslen(buf) < KSS_TARGET_MAX - 1)
                    res = kitty_wide_to_acp_dup(buf);
                sfree(buf);
            }
            sfree(wlnk);
            IPersistFile_Release(ppf);
        }
        IShellLinkW_Release(psl);
    }
    if (couninit)
        CoUninitialize();
    return res;
}

int kitty_shortcut_write(const char *lnkpath, const char *target,
                         const char *args, const char *workdir,
                         const char *icon, int icon_index)
{
    IShellLinkA *psl = NULL;
    IPersistFile *ppf = NULL;
    HRESULT hr;
    int ok = 0;
    int couninit = 0;

    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (hr == S_OK || hr == S_FALSE)
        couninit = 1;
    else if (hr != RPC_E_CHANGED_MODE)
        return 0;

    hr = CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IShellLinkA, (void **)&psl);
    if (SUCCEEDED(hr) && psl) {
        IShellLinkA_SetPath(psl, target);
        if (args && *args)
            IShellLinkA_SetArguments(psl, args);
        if (workdir && *workdir)
            IShellLinkA_SetWorkingDirectory(psl, workdir);
        IShellLinkA_SetIconLocation(psl, (icon && *icon) ? icon : target,
                                    icon_index);

        hr = IShellLinkA_QueryInterface(psl, &IID_IPersistFile,
                                        (void **)&ppf);
        if (SUCCEEDED(hr) && ppf) {
            WCHAR *wlnk = kitty_acp_to_wide_dup(lnkpath);   /* whole */
            if (wlnk) {
                hr = IPersistFile_Save(ppf, wlnk, TRUE);
                ok = SUCCEEDED(hr);
                sfree(wlnk);
            }
            IPersistFile_Release(ppf);
        }
        IShellLinkA_Release(psl);
    }
    if (couninit)
        CoUninitialize();
    return ok;
}

int kitty_startup_shortcut_set(const char *name, const char *target,
                               const char *args, const char *workdir,
                               const char *icon, int on)
{
    char *lnk = shortcut_path(name);
    int ok;
    if (!lnk)
        return 0;

    if (!on) {
        DeleteFileA(lnk);           /* absent is fine */
        sfree(lnk);
        return 1;
    }
    ok = kitty_shortcut_write(lnk, target, args, workdir, icon, 0);
    sfree(lnk);
    return ok;
}
