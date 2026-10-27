/*
 * test_storeacl - kitty/kitty_store_acl.c: which folders and files of a
 * folder store get the owner-only access rights (a new one on NTFS outside
 * the profile, a share's NTFS included; never one that exists, one below
 * the profile, one on FAT or a share without access lists), the path
 * tests behind it, the notice
 * once per store with its Event Log record, and the access list a folder is
 * made with (protected, the user, SYSTEM and Administrators, inherited by
 * the contents) - made in a folder of its own under %TEMP% and removed.
 *
 * Returns non-zero on any failure.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <aclapi.h>

#include "kitty_store_acl.h"
#include "kitty_atomicfile.h"
#include "kitty_text.h"

static int failures = 0;

static void expect(const char *what, int got, int want)
{
    if (got != want) {
        printf("FAIL [%s]: got %d, want %d\n", what, got, want);
        failures++;
    }
}

/* ---- what kitty_store_acl.c takes from the rest of the suite ---- */

/* printf into memory of its own (free it) */
static char *strf(const char *fmt, ...)
{
    va_list ap;
    int n;
    char *s;
    va_start(ap, fmt);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0 || (s = malloc((size_t)n + 1)) == NULL) {
        printf("FAIL  out of memory\n");
        exit(1);
    }
    va_start(ap, fmt);
    vsnprintf(s, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return s;
}

static int nevents = 0;
static char *lastevent, *lastpath;
void kitty_wev_suite(unsigned id, unsigned type, const char *const *vals,
                     int n, const char *text)
{
    (void)type;
    nevents++;
    free(lastevent);
    free(lastpath);
    lastevent = strf("%u|%s", id, text ? text : "");
    lastpath = strf("%s", n > 0 && vals[0] ? vals[0] : "");
}

static const char *store_root = "X:\\store";
static int marker = 0, markerwrites = 0, readonly = 0;
int kitty_store_readonly(void) { return readonly; }
void safefree(void *p) { free(p); }
static char *dup(const char *s)
{
    char *d = malloc(strlen(s) + 1);
    if (d) strcpy(d, s);
    return d;
}
char *portable_root_dir(void) { return store_root ? dup(store_root) : NULL; }
char *kitty_portable_load_state_dup(const char *key)
{
    return (marker && !strcmp(key, KITTY_STORE_ACL_MARKER)) ? dup("1") : NULL;
}
int kitty_portable_store_state_string(const char *key, const char *value)
{
    if (!strcmp(key, KITTY_STORE_ACL_MARKER) && !strcmp(value, "1")) {
        marker = 1;
        markerwrites++;
    }
    return 1;
}

static int nnotices = 0, answer = 1;
static char *lastnotice;
static int notice(const char *msg)
{
    nnotices++;
    free(lastnotice);
    lastnotice = strf("%s", msg);
    return answer;
}

/* ---- the access list read back ---- */

static int is_sid(PSID sid, WELL_KNOWN_SID_TYPE t)
{
    BYTE buf[68];                       /* SECURITY_MAX_SID_SIZE */
    DWORD n = sizeof(buf);
    return CreateWellKnownSid(t, NULL, buf, &n) && EqualSid(sid, buf);
}

/* the token's user (the memory is kept) */
static PSID token_user(void)
{
    static TOKEN_USER *tu;
    HANDLE tok;
    DWORD n = 0;
    if (tu)
        return tu->User.Sid;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok))
        return NULL;
    GetTokenInformation(tok, TokenUser, NULL, 0, &n);
    if (!n || (tu = malloc(n)) == NULL ||
        !GetTokenInformation(tok, TokenUser, tu, n, &n)) {
        free(tu);
        tu = NULL;
    }
    CloseHandle(tok);
    return tu ? tu->User.Sid : NULL;
}

/* The access list of `path`. own: owner-only as made (protected, the three
 * entries its own; a folder's for its contents too); else what Windows makes
 * (not protected). */
static void check_acl(const char *what, const char *path, int folder, int own)
{
    PSECURITY_DESCRIPTOR sd = NULL;
    PACL dacl = NULL;
    SECURITY_DESCRIPTOR_CONTROL ctl = 0;
    DWORD rev = 0, i;
    int user = 0, sys = 0, adm = 0, other = 0, wrong = 0;
    BYTE want = folder ? (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE) : 0;
    PSID me = token_user();
    char *t;

    if (GetNamedSecurityInfoA((LPSTR)path, SE_FILE_OBJECT,
                              DACL_SECURITY_INFORMATION, NULL, NULL, &dacl,
                              NULL, &sd) != ERROR_SUCCESS || !dacl) {
        printf("FAIL [%s: access list read]\n", what);
        failures++;
        return;
    }
    GetSecurityDescriptorControl(sd, &ctl, &rev);
    t = strf("%s: inheritance from above switched off", what);
    expect(t, (ctl & SE_DACL_PROTECTED) != 0, own);
    free(t);
    if (own) {
        for (i = 0; i < dacl->AceCount; i++) {
            ACCESS_ALLOWED_ACE *ace;
            if (!GetAce(dacl, i, (void **)&ace))
                continue;
            if (ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE) {
                other++;
                continue;
            }
            if ((ace->Header.AceFlags & (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE |
                                         INHERITED_ACE)) != want)
                wrong++;
            if ((ace->Mask & FILE_ALL_ACCESS) != FILE_ALL_ACCESS)
                wrong++;
            if (me && EqualSid((PSID)&ace->SidStart, me))
                user++;
            else if (is_sid((PSID)&ace->SidStart, WinLocalSystemSid))
                sys++;
            else if (is_sid((PSID)&ace->SidStart, WinBuiltinAdministratorsSid))
                adm++;
            else
                other++;
        }
        t = strf("%s: the user, full control", what);
        expect(t, user, 1);
        free(t);
        t = strf("%s: SYSTEM, full control", what);
        expect(t, sys, 1);
        free(t);
        t = strf("%s: Administrators, full control", what);
        expect(t, adm, 1);
        free(t);
        t = strf("%s: nobody else", what);
        expect(t, other, 0);
        free(t);
        t = strf("%s: every entry its own, full control", what);
        expect(t, wrong, 0);
        free(t);
    }
    LocalFree(sd);
}

int main(void)
{
    char *tmp, *room, *sub, *vol, *file, *other, *prof;
    DWORD flags = 0, n;

    /* the decision: exists x profile x share x access lists */
    expect("new, NTFS, outside the profile: set",
           kitty_store_acl_verdict(0, 0, 0, 1), KSA_SET);
    expect("exists: left as it is", kitty_store_acl_verdict(1, 0, 0, 1), KSA_LEAVE);
    expect("exists on FAT: left, no report", kitty_store_acl_verdict(1, 0, 0, 0), KSA_LEAVE);
    expect("exists on a share: left, no report", kitty_store_acl_verdict(1, 0, 1, 1), KSA_LEAVE);
    expect("below the profile: as Windows makes it",
           kitty_store_acl_verdict(0, 1, 0, 1), KSA_PROFILE);
    expect("below the profile on FAT: no report",
           kitty_store_acl_verdict(0, 1, 0, 0), KSA_PROFILE);
    expect("share with access lists (a Windows server's NTFS): set",
           kitty_store_acl_verdict(0, 0, 1, 1), KSA_SET);
    expect("share without access lists (a NAS, FAT on the server): reported as a share",
           kitty_store_acl_verdict(0, 0, 1, 0), KSA_SHARE);
    expect("exists on a share without access lists: left, no report",
           kitty_store_acl_verdict(1, 0, 1, 0), KSA_LEAVE);
    expect("share below the profile (a redirected profile): as Windows makes it",
           kitty_store_acl_verdict(0, 1, 1, 0), KSA_PROFILE);
    expect("FAT: not set", kitty_store_acl_verdict(0, 0, 0, 0), KSA_NOACLS);
    expect("reported: set", kitty_store_acl_reported(KSA_SET), 0);
    expect("reported: exists", kitty_store_acl_reported(KSA_LEAVE), 0);
    expect("reported: profile", kitty_store_acl_reported(KSA_PROFILE), 0);
    expect("reported: share", kitty_store_acl_reported(KSA_SHARE), 1);
    expect("reported: FAT", kitty_store_acl_reported(KSA_NOACLS), 1);
    expect("reported: failed", kitty_store_acl_reported(KSA_FAILED), 1);

    /* the paths */
    expect("under: itself", kitty_store_acl_path_under("C:\\Users\\hk", "C:\\Users\\hk"), 1);
    expect("under: below", kitty_store_acl_path_under("C:\\Users\\hk\\x\\Sessions", "C:\\Users\\hk"), 1);
    expect("under: any case", kitty_store_acl_path_under("c:\\USERS\\HK\\x", "C:\\Users\\hk"), 1);
    expect("under: a trailing backslash", kitty_store_acl_path_under("C:\\Users\\hk\\x", "C:\\Users\\hk\\"), 1);
    expect("under: a longer name is not below", kitty_store_acl_path_under("C:\\Users\\hkx\\x", "C:\\Users\\hk"), 0);
    expect("under: another folder", kitty_store_acl_path_under("D:\\KiTTY++\\Sessions", "C:\\Users\\hk"), 0);
    expect("under: an empty profile", kitty_store_acl_path_under("C:\\x", ""), 0);
    expect("unc: \\\\server\\share", kitty_store_acl_is_unc("\\\\server\\share\\KiTTY"), 1);
    expect("unc: //server/share", kitty_store_acl_is_unc("//server/share"), 1);
    expect("unc: \\\\?\\UNC\\server", kitty_store_acl_is_unc("\\\\?\\UNC\\server\\share"), 1);
    expect("unc: \\\\?\\C: is local", kitty_store_acl_is_unc("\\\\?\\C:\\KiTTY"), 0);
    expect("unc: a drive letter", kitty_store_acl_is_unc("C:\\KiTTY"), 0);
    expect("unc: relative", kitty_store_acl_is_unc("Sessions"), 0);

    /* the notice once per store */
    expect("notice due: no marker, not shown", kitty_store_acl_notice_due(0, 0), 1);
    expect("notice due: marker set", kitty_store_acl_notice_due(1, 0), 0);
    expect("notice due: shown in this process", kitty_store_acl_notice_due(0, 1), 0);

    /* without a front end: the record alone, no marker */
    kitty_store_acl_report(KSA_NOACLS, "X:\\store\\Sessions", 0);
    expect("no front end: one record", nevents, 1);
    expect("no front end: no marker", markerwrites, 0);
    kitty_store_acl_forget();
    nevents = 0;

    kitty_set_store_acl_report(notice);
    kitty_store_acl_report(KSA_NOACLS, "X:\\store\\Sessions", 0);
    expect("FAT: one notice", nnotices, 1);
    expect("FAT: the marker written", markerwrites, 1);
    expect("FAT: one record", nevents, 1);
    {
        char *want = strf("904|" KT_STORE_ACL_NOT_SET_FMT, "X:\\store");
        expect("FAT: the record's text, the store root in it",
               strcmp(lastevent, want) == 0, 1);
        expect("FAT: the record's path, the folder made",
               strcmp(lastpath, "X:\\store\\Sessions") == 0, 1);
        free(want);
        want = strf(KT_STORE_ACL_NOT_SET_FMT, "X:\\store");
        expect("FAT: the notice's text", strcmp(lastnotice, want) == 0, 1);
        free(want);
    }
    kitty_store_acl_report(KSA_NOACLS, "X:\\store\\SshHostKeys", 0);
    expect("FAT again: no second notice", nnotices, 1);
    expect("FAT again: no second record in this process", nevents, 1);

    /* the next start of the same store: the marker holds */
    kitty_store_acl_forget();
    kitty_store_acl_report(KSA_SHARE, "X:\\store\\Jumplist", 0);
    expect("next start: no notice, the store remembers", nnotices, 1);
    expect("next start: the record again", nevents, 2);

    /* a failure, another store: the notice with Windows' reason */
    kitty_store_acl_forget();
    marker = 0;
    store_root = "Y:\\other";
    kitty_store_acl_report(KSA_FAILED, "Y:\\other\\KiTTYState", ERROR_ACCESS_DENIED);
    expect("failed: one notice", nnotices, 2);
    expect("failed: one record", nevents, 3);
    expect("failed: the reason named", strstr(lastnotice, "(error 5)") != NULL, 1);
    {
        const char *start = "Access rights not set for Y:\\other (";
        expect("failed: the text's start",
               strncmp(lastnotice, start, strlen(start)) == 0, 1);
    }

    /* a notice nobody saw: no marker, tried again by the next process */
    kitty_store_acl_forget();
    marker = 0;
    markerwrites = 0;
    answer = 0;
    kitty_store_acl_report(KSA_NOACLS, "Y:\\other\\Sessions", 0);
    expect("not shown: no marker", markerwrites, 0);
    answer = 1;
    kitty_store_acl_forget();
    kitty_store_acl_report(KSA_NOACLS, "Y:\\other\\Sessions", 0);
    expect("not shown before: shown now", markerwrites, 1);

    /* no store known yet (the first kitty.ini): the record, no notice */
    kitty_store_acl_forget();
    marker = 0;
    markerwrites = 0;
    store_root = NULL;
    nnotices = 0;
    kitty_store_acl_report(KSA_SHARE, "\\\\srv\\share\\kitty.ini", 0);
    expect("no store: no notice", nnotices, 0);
    expect("no store: no marker", markerwrites, 0);
    expect("no store: the record names the file",
           strcmp(lastpath, "\\\\srv\\share\\kitty.ini") == 0, 1);

    /* readonly=yes: the marker cannot be written - the record, no notice,
     * also at the next start */
    kitty_store_acl_forget();
    store_root = "R:\\ro";
    marker = 0;
    markerwrites = 0;
    nnotices = 0;
    nevents = 0;
    readonly = 1;
    kitty_store_acl_report(KSA_NOACLS, "R:\\ro\\Sessions", 0);
    expect("readonly: the record", nevents, 1);
    expect("readonly: no notice", nnotices, 0);
    expect("readonly: no marker", markerwrites, 0);
    kitty_store_acl_forget();
    kitty_store_acl_report(KSA_FAILED, "R:\\ro\\KiTTYState", ERROR_ACCESS_DENIED);
    expect("readonly, next start: the record again", nevents, 2);
    expect("readonly, next start: still no notice", nnotices, 0);
    readonly = 0;

    /* the verdicts no one reports */
    kitty_store_acl_forget();
    nevents = 0;
    kitty_store_acl_report(KSA_SET, "Z:\\a", 0);
    kitty_store_acl_report(KSA_LEAVE, "Z:\\a", 0);
    kitty_store_acl_report(KSA_PROFILE, "Z:\\a", 0);
    expect("set / exists / profile: no record", nevents, 0);

    /* a folder made with the access list, on this machine */
    n = GetTempPathA(0, NULL);
    if (!n || (tmp = malloc(n + 1)) == NULL || !GetTempPathA(n + 1, tmp)) {
        printf("FAIL [no %%TEMP%%]\n");
        return 1;
    }
    room = strf("%sqa_storeacl_%lu", tmp, (unsigned long)GetCurrentProcessId());
    sub = strf("%s\\Sessions", room);
    vol = malloc(strlen(room) + 2);
    if (vol && GetVolumePathNameA(room, vol, (DWORD)strlen(room) + 2) &&
        GetVolumeInformationA(vol, NULL, 0, NULL, NULL, &flags, NULL, 0) &&
        !(flags & FILE_PERSISTENT_ACLS)) {
        printf("NOTE: %%TEMP%% has no access lists, the folder leg not run\n");
    } else if (!CreateDirectoryA(room, NULL)) {
        printf("FAIL [room %s not made: %lu]\n", room, GetLastError());
        failures++;
    } else {
        /* %USERPROFILE% of this process elsewhere: the room counts as
         * outside the profile, the rights are set (put back below) */
        n = GetEnvironmentVariableA("USERPROFILE", NULL, 0);
        prof = n ? malloc(n) : NULL;
        if (prof && !GetEnvironmentVariableA("USERPROFILE", prof, n)) {
            free(prof);
            prof = NULL;
        }
        file = strf("%s\\KiTTYState", room);
        other = strf("%s\\Backups", room);
        {
            char *away = strf("%s\\notprofile", room);
            SetEnvironmentVariableA("USERPROFILE", away);
            free(away);
        }
        nevents = 0;
        expect("outside the profile: the folder made", kitty_store_acl_mkdir(sub) != 0, 1);
        check_acl("new folder", sub, 1, 1);
        expect("outside the profile: the file made", kitty_store_acl_new_file(file), 1);
        check_acl("new file", file, 0, 1);
        expect("the file written whole afterwards",
               kitty_write_file_whole(file, "LastSession=x\n", 14), 1);
        check_acl("new file, rewritten", file, 0, 1);
        expect("a file that exists: not made again", kitty_store_acl_new_file(file), 0);
        expect("set: no record", nevents, 0);
        /* a folder that exists: CreateDirectory's own answer, nothing set */
        expect("mkdir on an existing folder: refused as Windows does",
               kitty_store_acl_mkdir(room), 0);
        expect("mkdir on an existing folder: already exists",
               (int)GetLastError(), ERROR_ALREADY_EXISTS);
        check_acl("existing room", room, 1, 0);
        expect("exists: no record", nevents, 0);
        /* below the profile: as Windows makes it */
        DeleteFileA(file);
        SetEnvironmentVariableA("USERPROFILE", room);
        expect("below the profile: the folder made", kitty_store_acl_mkdir(other) != 0, 1);
        check_acl("folder below the profile", other, 1, 0);
        expect("below the profile: no file made ahead", kitty_store_acl_new_file(file), 0);
        expect("profile: no record", nevents, 0);
        SetEnvironmentVariableA("USERPROFILE", prof);
        free(prof);
        DeleteFileA(file);
        RemoveDirectoryA(other);
        RemoveDirectoryA(sub);
        RemoveDirectoryA(room);
        free(file);
        free(other);
    }
    free(room);
    free(sub);
    free(vol);
    free(tmp);

    if (failures)
        printf("%d failure(s)\n", failures);
    else
        printf("test_storeacl: all passed\n");
    return failures ? 1 : 0;
}
