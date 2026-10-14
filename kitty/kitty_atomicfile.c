/*
 * kitty_atomicfile.c - a file written whole or not at all.
 *
 * The new content goes to a temporary file that Windows names (and creates)
 * in the target's own folder - MoveFileEx cannot replace across drives - and
 * is moved over the old file only once it is complete, written through to
 * the disk. A write that fails leaves the old file as it was. Used by the
 * folder store (kitty_storage.c) and by kageant's OpenSSH config edits
 * (kitty_kageant_openssh.c), which had a copy each.
 *
 * A moved file keeps the permissions it was created with, and the temporary
 * file takes the folder's. So the old file's access list - with its
 * inheritance switched off or on as it was - and its attributes go onto the
 * temporary file before the move: a ~/.ssh/config tightened by hand must not
 * come back with the folder's wider rights (OpenSSH refuses a config others
 * can write). When they cannot be copied, nothing is replaced. A volume
 * without access lists (FAT, exFAT: a portable store on a USB stick) gets
 * the attributes only.
 */
#include <windows.h>
#include <aclapi.h>
#include <stdio.h>
#include <string.h>
#include "kitty_atomicfile.h"

/* The access list of 'from' onto 'to', inheritance as it was. A DACL that
 * inherits is set as inheriting: the ACEs it inherited are taken from the
 * folder again, its own ones are kept. Returns 0 with GetLastError() set. */
static int copy_file_dacl(const char *from, const char *to)
{
    PSECURITY_DESCRIPTOR sd = NULL;
    PACL dacl = NULL;
    SECURITY_DESCRIPTOR_CONTROL ctl = 0;
    DWORD rev = 0, e;
    SECURITY_INFORMATION si;

    e = GetNamedSecurityInfoA((LPSTR)from, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                              NULL, NULL, &dacl, NULL, &sd);
    if (e != ERROR_SUCCESS) { SetLastError(e); return 0; }
    if (!GetSecurityDescriptorControl(sd, &ctl, &rev)) {
        e = GetLastError(); LocalFree(sd); SetLastError(e); return 0;
    }
    si = DACL_SECURITY_INFORMATION |
         ((ctl & SE_DACL_PROTECTED) ? PROTECTED_DACL_SECURITY_INFORMATION
                                    : UNPROTECTED_DACL_SECURITY_INFORMATION);
    e = SetNamedSecurityInfoA((LPSTR)to, SE_FILE_OBJECT, si, NULL, NULL, dacl, NULL);
    LocalFree(sd);
    if (e != ERROR_SUCCESS) { SetLastError(e); return 0; }
    return 1;
}

/* Does the volume holding 'path' keep access lists? FAT and exFAT do not:
 * nothing to copy there. A volume that cannot be asked counts as keeping
 * them (NTFS, the usual case), so the copy is tried and its outcome stands. */
static int volume_keeps_acls(const char *path)
{
    char root[MAX_PATH];
    DWORD flags = 0;
    if (!GetVolumePathNameA(path, root, sizeof(root)))
        return 1;
    if (!GetVolumeInformationA(root, NULL, 0, NULL, NULL, &flags, NULL, 0))
        return 1;
    return (flags & FILE_PERSISTENT_ACLS) != 0;
}

int kitty_write_file_whole(const char *path, const void *data, size_t len)
{
    char dir[MAX_PATH], tmp[MAX_PATH];
    char *slash;
    HANDLE h;
    DWORD put = 0, err = 0;
    int ok = 0;

    if (!path || !*path || len > 0xFFFFFFFFu) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    snprintf(dir, sizeof(dir), "%s", path);
    slash = strrchr(dir, '\\');
    if (!slash) slash = strrchr(dir, '/');
    if (slash) *slash = '\0'; else snprintf(dir, sizeof(dir), ".");
    if (!GetTempFileNameA(dir, "kft", 0, tmp))
        return 0;   /* Windows' reason is in GetLastError() */

    h = CreateFileA(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        ok = WriteFile(h, data, (DWORD)len, &put, NULL) && put == (DWORD)len;
        if (!ok) err = GetLastError();
        if (!CloseHandle(h) && ok) { ok = 0; err = GetLastError(); }
    } else {
        err = GetLastError();
    }
    if (ok) {
        /* the old file's permissions and attributes, when there is one */
        DWORD attr = GetFileAttributesA(path);
        if (attr != INVALID_FILE_ATTRIBUTES) {
            if (volume_keeps_acls(path) && !copy_file_dacl(path, tmp)) { ok = 0; err = GetLastError(); }
            else {
                /* not READONLY: the move replaces a read-only file no more
                 * than a write would, and says so */
                DWORD keep = attr & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM |
                                     FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED);
                if (!SetFileAttributesA(tmp, keep ? keep : FILE_ATTRIBUTE_NORMAL)) { ok = 0; err = GetLastError(); }
            }
        } else {
            DWORD ae = GetLastError();
            if (ae != ERROR_FILE_NOT_FOUND && ae != ERROR_PATH_NOT_FOUND) { ok = 0; err = ae; }
        }
    }
    if (ok && !MoveFileExA(tmp, path,
                           MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        ok = 0; err = GetLastError();
    }
    if (!ok) {
        DeleteFileA(tmp);
        SetLastError(err ? err : ERROR_WRITE_FAULT);
    }
    return ok;
}
