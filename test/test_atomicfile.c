/*
 * test_atomicfile.c - kitty/kitty_atomicfile.c: a file written whole keeps
 * the permissions and attributes of the file it replaces. A file whose
 * inheritance is switched off keeps exactly its own access list (a
 * ~/.ssh/config tightened by hand), one that inherits still inherits, a
 * hidden one stays hidden, a new one is simply written, and no temporary
 * file is left behind. Works in a folder of its own under %TEMP%; with
 * KITTY_TEST_FAT_DIR naming a folder on a FAT volume the writes are checked
 * there too. Exit 0 = all passed.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>

#include "kitty_atomicfile.h"

static int fails = 0;

/* printf into memory of its own (free it): the paths are held in full */
static char *strf(const char *fmt, ...)
{
    size_t size = 256;
    char *s = NULL;
    int n;
    va_list ap;
    for (;;) {
        char *t = realloc(s, size);
        if (!t) {
            free(s);
            printf("FAIL  out of memory\n");
            exit(1);
        }
        s = t;
        va_start(ap, fmt);
        n = vsnprintf(s, size, fmt, ap);
        va_end(ap);
        if (n >= 0 && (size_t)n < size)
            return s;
        size = n >= 0 ? (size_t)n + 1 : size * 2;
    }
}

/* %TEMP% with its trailing backslash, in full (free it); NULL = not read */
static char *temp_dir(void)
{
    DWORD n = GetTempPathA(0, NULL), got;
    while (n) {
        char *s = malloc(n);
        if (!s)
            return NULL;
        got = GetTempPathA(n, s);
        if (got && got < n)
            return s;
        free(s);
        n = got ? got + 1 : 0;
    }
    return NULL;
}

static void check(int ok, const char *what)
{
    printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        fails++;
}

/* the file's DACL as SDDL (LocalFree it) and whether it is protected */
static char *dacl_text(const char *path, int *protected_)
{
    PSECURITY_DESCRIPTOR sd = NULL;
    SECURITY_DESCRIPTOR_CONTROL ctl = 0;
    DWORD rev = 0;
    char *s = NULL;
    if (GetNamedSecurityInfoA((LPSTR)path, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                              NULL, NULL, NULL, NULL, &sd) != ERROR_SUCCESS)
        return NULL;
    GetSecurityDescriptorControl(sd, &ctl, &rev);
    *protected_ = (ctl & SE_DACL_PROTECTED) != 0;
    ConvertSecurityDescriptorToStringSecurityDescriptorA(sd, SDDL_REVISION_1,
                                                         DACL_SECURITY_INFORMATION, &s, NULL);
    LocalFree(sd);
    return s;
}

/* the SID of this process's user, as SDDL text (LocalFree it) */
static char *my_sid(void)
{
    HANDLE tok;
    BYTE buf[256];
    DWORD n = 0;
    char *s = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok))
        return NULL;
    if (GetTokenInformation(tok, TokenUser, buf, sizeof(buf), &n))
        ConvertSidToStringSidA(((TOKEN_USER *)buf)->User.Sid, &s);
    CloseHandle(tok);
    return s;
}

static int put(const char *path, const char *text)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) return 0;
    fputs(text, fp);
    return fclose(fp) == 0;
}

static int content_is(const char *path, const char *text)
{
    char buf[256];
    size_t n;
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    buf[n] = '\0';
    return strcmp(buf, text) == 0;
}

static int no_temp_left(const char *dir)
{
    char *pat = strf("%s\\kft*.tmp", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h;
    h = FindFirstFileA(pat, &fd);
    free(pat);
    if (h == INVALID_HANDLE_VALUE) return 1;
    FindClose(h);
    return 0;
}

int main(void)
{
    char *tmp, *dir, *f = NULL, sddl[512];
    char *sid, *before, *after;
    int pb = 0, pa = 0;
    PSECURITY_DESCRIPTOR sd = NULL;
    PACL dacl = NULL;
    BOOL present = FALSE, defaulted = FALSE;
    const char *body = "Host example\r\n  User someone\r\n";

    if ((tmp = temp_dir()) == NULL) { printf("FAIL  %%TEMP%% not read (%lu)\n", GetLastError()); return 1; }
    dir = strf("%stest_atomicfile_%lu", tmp, GetCurrentProcessId());
    free(tmp);
    if (!CreateDirectoryA(dir, NULL)) { printf("FAIL  cannot create %s\n", dir); return 1; }

    /* 1. a new file: written, nothing left over */
    f = strf("%s\\new.txt", dir);
    check(kitty_write_file_whole(f, body, strlen(body)), "a new file is written");
    check(content_is(f, body), "  with the content given");
    check(no_temp_left(dir), "  and no temporary file is left");

    /* 2. inheritance off, only SYSTEM and this user: kept exactly */
    sid = my_sid();
    check(sid != NULL, "this user's SID is read");
    free(f); f = strf("%s\\config", dir);
    check(put(f, "old\r\n"), "a file to replace is there");
    snprintf(sddl, sizeof(sddl), "D:P(A;;FA;;;SY)(A;;FA;;;%s)", sid ? sid : "OW");
    if (ConvertStringSecurityDescriptorToSecurityDescriptorA(sddl, SDDL_REVISION_1, &sd, NULL) &&
        GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted))
        check(SetNamedSecurityInfoA(f, SE_FILE_OBJECT,
                                    DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                    NULL, NULL, dacl, NULL) == ERROR_SUCCESS,
              "its inheritance is switched off, its list narrowed");
    else
        check(0, "the narrow access list is built");
    if (sd) LocalFree(sd);
    before = dacl_text(f, &pb);
    check(kitty_write_file_whole(f, body, strlen(body)), "the narrowed file is replaced");
    check(content_is(f, body), "  with the new content");
    after = dacl_text(f, &pa);
    check(pa, "  its inheritance is still off");
    check(before && after && strcmp(before, after) == 0, "  its access list is exactly the old one");
    if (before && after && strcmp(before, after)) printf("        was: %s\n        now: %s\n", before, after);
    LocalFree(before); LocalFree(after);
    check(no_temp_left(dir), "  and no temporary file is left");

    /* 3. a file that inherits from the folder: still inherits */
    free(f); f = strf("%s\\inherits.txt", dir);
    check(put(f, "old\r\n"), "a file that inherits is there");
    before = dacl_text(f, &pb);
    check(kitty_write_file_whole(f, body, strlen(body)), "the inheriting file is replaced");
    after = dacl_text(f, &pa);
    check(!pb && !pa, "  it still inherits");
    check(before && after && strcmp(before, after) == 0, "  its access list is the same");
    LocalFree(before); LocalFree(after);

    /* 4. a hidden file stays hidden */
    free(f); f = strf("%s\\hidden.txt", dir);
    check(put(f, "old\r\n") && SetFileAttributesA(f, FILE_ATTRIBUTE_HIDDEN), "a hidden file is there");
    check(kitty_write_file_whole(f, body, strlen(body)), "the hidden file is replaced");
    check((GetFileAttributesA(f) & FILE_ATTRIBUTE_HIDDEN) != 0, "  it is still hidden");
    SetFileAttributesA(f, FILE_ATTRIBUTE_NORMAL);
    check(no_temp_left(dir), "no temporary file is left in the folder");

    /* 5. a volume without access lists (FAT, exFAT - a portable store on a
     * USB stick), when the caller names a folder there: nothing to copy, and
     * the writes must work as everywhere else */
    {
        const char *fat = getenv("KITTY_TEST_FAT_DIR");
        if (fat && *fat) {
            char *fd = strf("%s\\test_atomicfile_%lu", fat, GetCurrentProcessId());
            char *ff = strf("%s\\config", fd);
            check(CreateDirectoryA(fd, NULL), "FAT: a folder on the volume is made");
            check(kitty_write_file_whole(ff, "old\r\n", 5), "FAT: a new file is written");
            check(kitty_write_file_whole(ff, body, strlen(body)), "FAT: an existing file is replaced");
            check(content_is(ff, body), "FAT:   with the new content");
            check(SetFileAttributesA(ff, FILE_ATTRIBUTE_HIDDEN), "FAT: the file is made hidden");
            check(kitty_write_file_whole(ff, "again\r\n", 7), "FAT: the hidden file is replaced");
            check((GetFileAttributesA(ff) & FILE_ATTRIBUTE_HIDDEN) != 0, "FAT:   it is still hidden");
            check(content_is(ff, "again\r\n"), "FAT:   with the new content");
            check(no_temp_left(fd), "FAT: no temporary file is left");
            SetFileAttributesA(ff, FILE_ATTRIBUTE_NORMAL);
            DeleteFileA(ff);
            RemoveDirectoryA(fd);
            free(ff);
            free(fd);
        } else {
            printf("NOTE  no KITTY_TEST_FAT_DIR - the FAT case is not run here\n");
        }
    }

    /* clean up */
    free(f); f = strf("%s\\new.txt", dir); DeleteFileA(f);
    free(f); f = strf("%s\\config", dir); DeleteFileA(f);
    free(f); f = strf("%s\\inherits.txt", dir); DeleteFileA(f);
    free(f); f = strf("%s\\hidden.txt", dir); DeleteFileA(f);
    free(f);
    RemoveDirectoryA(dir);
    free(dir);
    if (sid) LocalFree(sid);

    printf("%s - %d failed\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
