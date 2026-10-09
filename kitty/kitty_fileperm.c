/*
 * kitty_fileperm.c: is a secret file readable by anyone but its user?
 *
 * -masterpwfile takes the master password from a file, and that file is the
 * whole protection of every password the master password wraps. Windows
 * OpenSSH refuses a private key that others can read; this is the same rule
 * for the master-password file: the owner is the current user, Administrators
 * or SYSTEM, and every ALLOW entry that grants read access belongs to one of
 * those three. Deny entries change nothing (they only take access away), and
 * inherit-only entries do not apply to the file itself.
 *
 * A file system without permissions (FAT, exFAT - a USB stick) is refused as
 * well: every user of the PC can read such a file, and a password should not
 * travel with the files it protects anyway. A network share is judged by the
 * NTFS permissions the server reports; share permissions are not visible.
 *
 * NT 4 API throughout (advapi32), so the 32-bit build still starts on XP.
 */

#include <windows.h>
#include <aclapi.h>
#include <stdio.h>
#include "kitty_fileperm.h"

static void kfp_name(PSID sid, char *who, size_t wholen)
{
    wchar_t name[256], dom[256];
    DWORD nl = 256, dl = 256;
    SID_NAME_USE use;

    if (!who || !wholen)
        return;
    if (LookupAccountSidW(NULL, sid, name, &nl, dom, &dl, &use)) {
        wchar_t full[520];
        if (dom[0])
            _snwprintf(full, 519, L"%ls\\%ls", dom, name);
        else
            _snwprintf(full, 519, L"%ls", name);
        full[519] = L'\0';
        /* the ANSI code page: the message goes to cmdline_error, whose box
         * (kitty.exe) and console line show ANSI - in UTF-8 a German
         * "NT-AUTORITÄT" came out garbled */
        if (WideCharToMultiByte(CP_ACP, 0, full, -1, who, (int)wholen,
                                NULL, NULL) > 0)
            return;
    }
    snprintf(who, wholen, "an unknown account");
}

/* the current user, Administrators or SYSTEM */
static BOOL kfp_trusted(PSID sid, PSID me, PSID admins, PSID system)
{
    return EqualSid(sid, me) || EqualSid(sid, admins) || EqualSid(sid, system);
}

KittyFilePerm kitty_secret_file_check(const wchar_t *path,
                                      char *who, size_t wholen)
{
    static SID_IDENTIFIER_AUTHORITY nt = { SECURITY_NT_AUTHORITY };
    wchar_t vol[MAX_PATH + 1];
    DWORD fsflags = 0, err;
    PSID owner = NULL, admins = NULL, system = NULL;
    PACL dacl = NULL;
    PSECURITY_DESCRIPTOR sd = NULL;
    HANDLE tok = NULL;
    TOKEN_USER *tu = NULL;
    DWORD tulen = 0, i;
    KittyFilePerm res = KFP_UNKNOWN;

    if (who && wholen)
        who[0] = '\0';

    /* no permissions on this file system at all */
    if (GetVolumePathNameW(path, vol, MAX_PATH) &&
        GetVolumeInformationW(vol, NULL, 0, NULL, NULL, &fsflags, NULL, 0) &&
        !(fsflags & FS_PERSISTENT_ACLS))
        return KFP_NO_ACL;

    err = GetNamedSecurityInfoW((LPWSTR)path, SE_FILE_OBJECT,
                                OWNER_SECURITY_INFORMATION |
                                DACL_SECURITY_INFORMATION,
                                &owner, NULL, &dacl, NULL, &sd);
    if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
        return KFP_MISSING;
    if (err != ERROR_SUCCESS) {
        if (who && wholen)
            snprintf(who, wholen, "error %lu", (unsigned long)err);
        return KFP_UNKNOWN;
    }

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok))
        goto out;
    GetTokenInformation(tok, TokenUser, NULL, 0, &tulen);
    tu = (TOKEN_USER *)LocalAlloc(LPTR, tulen ? tulen : 1);
    if (!tu || !GetTokenInformation(tok, TokenUser, tu, tulen, &tulen))
        goto out;
    if (!AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                  DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0,
                                  &admins) ||
        !AllocateAndInitializeSid(&nt, 1, SECURITY_LOCAL_SYSTEM_RID,
                                  0, 0, 0, 0, 0, 0, 0, &system))
        goto out;

    if (!owner || !kfp_trusted(owner, tu->User.Sid, admins, system)) {
        if (owner)
            kfp_name(owner, who, wholen);
        res = KFP_OWNER;
        goto out;
    }
    if (!dacl) {
        res = KFP_NULL_DACL;
        goto out;
    }
    for (i = 0; i < dacl->AceCount; i++) {
        ACE_HEADER *h;
        ACCESS_ALLOWED_ACE *a;
        PSID sid;
        if (!GetAce(dacl, i, (LPVOID *)&h))
            continue;
        if (h->AceType != ACCESS_ALLOWED_ACE_TYPE ||
            (h->AceFlags & INHERIT_ONLY_ACE))
            continue;
        a = (ACCESS_ALLOWED_ACE *)h;
        if (!(a->Mask & (FILE_READ_DATA | GENERIC_READ | GENERIC_ALL)))
            continue;
        sid = (PSID)&a->SidStart;
        if (!kfp_trusted(sid, tu->User.Sid, admins, system)) {
            kfp_name(sid, who, wholen);
            res = KFP_READABLE;
            goto out;
        }
    }
    res = KFP_PRIVATE;

  out:
    if (admins) FreeSid(admins);
    if (system) FreeSid(system);
    if (tu) LocalFree(tu);
    if (tok) CloseHandle(tok);
    if (sd) LocalFree(sd);
    return res;
}
