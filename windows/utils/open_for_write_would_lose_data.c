/*
 * Implementation of open_for_write_would_lose_data for Windows.
 */

#include "putty.h"

/*
 * This is slightly fiddly because we want to be backwards-compatible
 * with systems too old to have GetFileAttributesEx. The next best
 * thing is FindFirstFile, which will return a different data
 * structure, but one that also contains the fields we want. (But it
 * will behave more unhelpfully - for this application - in the
 * presence of wildcards, so we'd prefer to use GFAE if we can.)
 */

static inline bool open_for_write_would_lose_data_impl(
    DWORD dwFileAttributes, DWORD nFileSizeHigh, DWORD nFileSizeLow)
{
    if (dwFileAttributes & (FILE_ATTRIBUTE_DEVICE|FILE_ATTRIBUTE_DIRECTORY)) {
        /*
         * File is something other than an ordinary disk file, so
         * opening it for writing will not cause truncation. (It may
         * not _succeed_ either, but that's not our problem here!)
         */
        return false;
    }
    if (nFileSizeHigh == 0 && nFileSizeLow == 0) {
        /*
         * File is zero-length (or may be a named pipe, which
         * dwFileAttributes can't tell apart from a regular file), so
         * opening it for writing won't truncate any data away because
         * there's nothing to truncate anyway.
         */
        return false;
    }
    return true;
}

bool open_for_write_would_lose_data(const Filename *fn)
{
    static HMODULE kernel32_module;
    DECL_WINDOWS_FUNCTION(static, BOOL, GetFileAttributesExW,
                          (LPCWSTR, GET_FILEEX_INFO_LEVELS, LPVOID));
    bool ret;
    /* KiTTY: %VAR% expansion (cyd01/KiTTY#472). This check runs BEFORE
     * f_open() (logging.c logfopen decides append/overwrite/ask from its
     * answer), so it must look at the same expanded path f_open() will
     * actually open - otherwise a %VAR% log path always looks like "no such
     * file" here and silently skips the overwrite prompt. */
    wchar_t *wpath = filename_expand_wstr(fn);

    if (!kernel32_module) {
        kernel32_module = load_system32_dll("kernel32.dll");
        GET_WINDOWS_FUNCTION(kernel32_module, GetFileAttributesExW);
    }

    if (p_GetFileAttributesExW) {
        WIN32_FILE_ATTRIBUTE_DATA attrs;
        if (!p_GetFileAttributesExW(wpath, GetFileExInfoStandard, &attrs)) {
            /*
             * Generally, if we don't identify a specific reason why we
             * should return true from this function, we return false, and
             * let the subsequent attempt to open the file for real give a
             * more useful error message.
             */
            ret = false;
        } else {
            ret = open_for_write_would_lose_data_impl(
                attrs.dwFileAttributes, attrs.nFileSizeHigh, attrs.nFileSizeLow);
        }
    } else {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(wpath, &fd);
        if (h == INVALID_HANDLE_VALUE) {
            /*
             * As above, if we can't find the file at all, return false.
             */
            ret = false;
        } else {
            CloseHandle(h);
            ret = open_for_write_would_lose_data_impl(
                fd.dwFileAttributes, fd.nFileSizeHigh, fd.nFileSizeLow);
        }
    }
    sfree(wpath);
    return ret;
}
