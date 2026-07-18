/*
 * kitty_startup_shortcut.c: registry-free autostart via a Startup-folder
 * shortcut. Used by kageant (portable mode) and the launcher (on request).
 * Creating/removing the .lnk is the caller's explicit action - this module
 * never scans or touches other autostart entries.
 */

#define COBJMACROS
#include <windows.h>
#include <shlobj.h>
#include <objbase.h>
#include <string.h>
#include <stdio.h>

#include "kitty_startup_shortcut.h"

int kitty_startup_dir(char *out, size_t len, int common)
{
    char path[MAX_PATH];
    int csidl = common ? CSIDL_COMMON_STARTUP : CSIDL_STARTUP;
    if (SHGetFolderPathA(NULL, csidl, NULL, SHGFP_TYPE_CURRENT, path) != S_OK)
        return 0;
    if (strlen(path) >= len)
        return 0;
    strcpy(out, path);
    return 1;
}

/* Build "<Startup>\<name>.lnk" (common != 0 = all-users). Returns 1 on ok. */
static int shortcut_path_in(const char *name, int common, char *out, size_t len)
{
    char dir[MAX_PATH];
    if (!kitty_startup_dir(dir, sizeof(dir), common))
        return 0;
    if (strlen(dir) + strlen(name) + 6 >= len)
        return 0;
    snprintf(out, len, "%s\\%s.lnk", dir, name);
    return 1;
}

/* The per-user path; used for everything this module creates/removes. */
static int shortcut_path(const char *name, char *out, size_t len)
{
    return shortcut_path_in(name, 0, out, len);
}

static int shortcut_exists_in(const char *name, int common)
{
    char lnk[MAX_PATH];
    DWORD a;
    if (!shortcut_path_in(name, common, lnk, sizeof(lnk)))
        return 0;
    a = GetFileAttributesA(lnk);
    return (a != INVALID_FILE_ATTRIBUTES) && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

int kitty_startup_shortcut_exists(const char *name)
{
    return shortcut_exists_in(name, 0);
}

int kitty_startup_shortcut_exists_common(const char *name)
{
    return shortcut_exists_in(name, 1);
}

/* Compare two exe paths, normalising 8.3/long form first (an MSI target may
 * be stored short while GetModuleFileName returns the long path). */
static int same_exe_path(const char *a, const char *b)
{
    char la[MAX_PATH], lb[MAX_PATH];
    if (!GetLongPathNameA(a, la, sizeof(la))) snprintf(la, sizeof(la), "%s", a);
    if (!GetLongPathNameA(b, lb, sizeof(lb))) snprintf(lb, sizeof(lb), "%s", b);
    return !stricmp(la, lb);
}

int kitty_startup_shortcut_points_to(const char *name, int common,
                                     const char *target_exe)
{
    char lnk[MAX_PATH], got[MAX_PATH];
    if (!shortcut_path_in(name, common, lnk, sizeof(lnk)))
        return 0;
    if (!kitty_startup_shortcut_target(lnk, got, sizeof(got)))
        return 0;
    return same_exe_path(got, target_exe);
}

int kitty_startup_shortcut_target(const char *lnkpath, char *out, size_t len)
{
    IShellLinkA *psl = NULL;
    IPersistFile *ppf = NULL;
    HRESULT hr;
    int ok = 0, couninit = 0;

    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (hr == S_OK || hr == S_FALSE)
        couninit = 1;
    else if (hr != RPC_E_CHANGED_MODE)
        return 0;

    hr = CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IShellLinkA, (void **)&psl);
    if (SUCCEEDED(hr) && psl) {
        hr = IShellLinkA_QueryInterface(psl, &IID_IPersistFile, (void **)&ppf);
        if (SUCCEEDED(hr) && ppf) {
            WCHAR wlnk[MAX_PATH];
            if (MultiByteToWideChar(CP_ACP, 0, lnkpath, -1, wlnk, MAX_PATH) > 0 &&
                SUCCEEDED(IPersistFile_Load(ppf, wlnk, STGM_READ))) {
                char buf[MAX_PATH];
                if (IShellLinkA_GetPath(psl, buf, MAX_PATH, NULL, SLGP_RAWPATH)
                        == S_OK && buf[0] && strlen(buf) < len) {
                    strcpy(out, buf);
                    ok = 1;
                }
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
    char lnk[MAX_PATH];
    if (!shortcut_path(name, lnk, sizeof(lnk)))
        return 0;

    if (!on) {
        DeleteFileA(lnk);           /* absent is fine */
        return 1;
    }

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
            IShellLinkA_SetIconLocation(psl, (icon && *icon) ? icon : target, 0);

            hr = IShellLinkA_QueryInterface(psl, &IID_IPersistFile,
                                            (void **)&ppf);
            if (SUCCEEDED(hr) && ppf) {
                WCHAR wlnk[MAX_PATH];
                if (MultiByteToWideChar(CP_ACP, 0, lnk, -1, wlnk, MAX_PATH) > 0) {
                    hr = IPersistFile_Save(ppf, wlnk, TRUE);
                    ok = SUCCEEDED(hr);
                }
                IPersistFile_Release(ppf);
            }
            IShellLinkA_Release(psl);
        }
        if (couninit)
            CoUninitialize();
        return ok;
    }
}
