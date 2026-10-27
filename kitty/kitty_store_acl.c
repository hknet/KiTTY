/*
 * kitty_store_acl.c - owner-only access rights for what KiTTY++ creates in
 * a folder store. The rules are in kitty_store_acl.h.
 *
 * The access list is built from SIDs (the token's user, SYSTEM,
 * BUILTIN\Administrators) with the calls Windows XP has, no SDDL string.
 * It goes in at creation through SECURITY_ATTRIBUTES; marked protected, so
 * nothing is inherited from the folder above.
 *
 * No window here: the notice goes through the function kitty.exe registers
 * (kitty_set_store_acl_report), the Event Log record (904) is written
 * whatever the Event Log switch of the program is.
 */
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kitty_store_acl.h"
#include "kitty_winevent.h"
#include "kitty_winerr.h"
#include "kitty_text.h"
#include "kitty_inifile.h"      /* kitty_store_readonly: readonly=yes */

/* kitty_storage.c (its header needs putty.h): the store root and its
 * KiTTYState values; puttymem.h: their memory is freed with sfree */
char *portable_root_dir(void);
char *kitty_portable_load_state_dup(const char *key);
int kitty_portable_store_state_string(const char *key, const char *value);
void safefree(void *);

/* ---- the decision ---- */

int kitty_store_acl_verdict(int exists, int in_profile, int remote,
                            int keeps_acls)
{
    if (exists)
        return KSA_LEAVE;
    if (in_profile)
        return KSA_PROFILE;
    /* a share is judged as a local drive: one that keeps access lists (a
     * Windows server's NTFS) gets the rights, one without them is reported */
    if (!keeps_acls)
        return remote ? KSA_SHARE : KSA_NOACLS;
    return KSA_SET;
}

int kitty_store_acl_reported(int verdict)
{
    return verdict == KSA_SHARE || verdict == KSA_NOACLS ||
        verdict == KSA_FAILED;
}

static int ksa_sep(char c)
{
    return c == '\\' || c == '/';
}

int kitty_store_acl_path_under(const char *path, const char *dir)
{
    size_t n;
    if (!path || !dir)
        return 0;
    n = strlen(dir);
    while (n > 0 && ksa_sep(dir[n - 1]))
        n--;
    if (n == 0 || _strnicmp(path, dir, n) != 0)
        return 0;
    return path[n] == '\0' || ksa_sep(path[n]);
}

int kitty_store_acl_is_unc(const char *path)
{
    if (!path || !ksa_sep(path[0]) || !ksa_sep(path[1]))
        return 0;
    /* \\?\ and \\.\: a local path unless \\?\UNC\ */
    if ((path[2] == '?' || path[2] == '.') && ksa_sep(path[3]))
        return _strnicmp(path + 4, "UNC", 3) == 0 && ksa_sep(path[7]);
    return 1;
}

int kitty_store_acl_notice_due(int marker_set, int shown)
{
    return !marker_set && !shown;
}

/* ---- what Windows answers for a path ---- */

/* printf into memory of its own (free it), held in full */
static char *ksa_fmt(const char *fmt, ...)
{
    va_list ap;
    int n;
    char *s;
    va_start(ap, fmt);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0 || (s = malloc((size_t)n + 1)) == NULL)
        return NULL;
    va_start(ap, fmt);
    vsnprintf(s, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return s;
}

/* The full path (free it); NULL when Windows cannot make one */
static char *ksa_full(const char *path)
{
    DWORD n = GetFullPathNameA(path, 0, NULL, NULL), got;
    while (n) {
        char *s = malloc(n);
        if (!s)
            return NULL;
        got = GetFullPathNameA(path, n, s, NULL);
        if (got && got < n)
            return s;
        free(s);
        n = got > n ? got : 0;
    }
    return NULL;
}

static int ksa_in_profile(const char *full)
{
    DWORD n = GetEnvironmentVariableA("USERPROFILE", NULL, 0);
    char *env, *prof;
    int r = 0;
    if (!n || (env = malloc(n)) == NULL)
        return 0;
    if (GetEnvironmentVariableA("USERPROFILE", env, n) < n && env[0] &&
        (prof = ksa_full(env)) != NULL) {
        r = kitty_store_acl_path_under(full, prof);
        free(prof);
    }
    free(env);
    return r;
}

/* The root of a UNC path, \\server\share\ (\\?\UNC\server\share\), into
 * root (as long as path + 2); 0 when the path has no share part. */
static int ksa_unc_root(const char *path, char *root)
{
    const char *p = path + 2;
    int seps = 0;
    if (!_strnicmp(path, "\\\\?\\UNC\\", 8))
        p = path + 8;
    for (; *p; p++)
        if (ksa_sep(*p) && ++seps == 2)
            break;
    if (seps == 0 && !*p)
        return 0;
    memcpy(root, path, (size_t)(p - path));
    root[p - path] = '\\';
    root[p - path + 1] = '\0';
    return 1;
}

/* A network share (UNC or a drive Windows reports as remote), and whether
 * its volume keeps access lists - a share's root is asked as a local
 * drive's is (a Windows server's NTFS keeps them, a NAS without them or a
 * FAT volume does not). A volume that cannot be queried counts as keeping
 * them: the rights are tried, a failure is reported with Windows' reason. */
static void ksa_volume(const char *full, int *remote, int *acls)
{
    size_t n = strlen(full) + 2;
    char *root = malloc(n);
    int have;
    *remote = kitty_store_acl_is_unc(full);
    *acls = 1;
    if (!root)
        return;
    have = GetVolumePathNameA(full, root, (DWORD)n) != 0;
    if (!have && *remote)
        have = ksa_unc_root(full, root);
    if (have) {
        DWORD flags = 0;
        if (GetDriveTypeA(root) == DRIVE_REMOTE)
            *remote = 1;
        if (GetVolumeInformationA(root, NULL, 0, NULL, NULL, &flags, NULL, 0))
            *acls = (flags & FILE_PERSISTENT_ACLS) != 0;
    }
    free(root);
}

/* The verdict for `path`; *fullp its full path (free it) for a verdict
 * other than KSA_LEAVE. The cheap test first: a folder that exists - every
 * session save checks the session folder - costs one call. */
static int ksa_decide(const char *path, char **fullp)
{
    char *full;
    int remote, acls;
    *fullp = NULL;
    if (!path || !*path || GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES)
        return KSA_LEAVE;
    if ((full = ksa_full(path)) == NULL)
        return KSA_LEAVE;
    ksa_volume(full, &remote, &acls);
    *fullp = full;
    return kitty_store_acl_verdict(0, ksa_in_profile(full), remote, acls);
}

/* ---- the access list ---- */

static struct {
    int ok;
    SECURITY_DESCRIPTOR sd;
    PACL acl;
} g_sd[2];                              /* [0] a file, [1] a folder */

int kitty_store_acl_attrs(int folder, SECURITY_ATTRIBUTES *sa)
{
    int i = folder ? 1 : 0;
    if (!g_sd[i].ok) {
        SID_IDENTIFIER_AUTHORITY nt = { SECURITY_NT_AUTHORITY };
        DWORD flags = folder ? (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE) : 0;
        HANDLE tok = NULL;
        TOKEN_USER *tu = NULL;
        PSID sys = NULL, adm = NULL;
        PACL acl = NULL;
        DWORD need = 0, size, err = 0;

        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
            err = GetLastError();
        } else {
            GetTokenInformation(tok, TokenUser, NULL, 0, &need);
            if (!need || (tu = malloc(need)) == NULL)
                err = ERROR_NOT_ENOUGH_MEMORY;
            else if (!GetTokenInformation(tok, TokenUser, tu, need, &need))
                err = GetLastError();
            CloseHandle(tok);
        }
        if (!err && !AllocateAndInitializeSid(&nt, 1, SECURITY_LOCAL_SYSTEM_RID,
                                              0, 0, 0, 0, 0, 0, 0, &sys))
            err = GetLastError();
        if (!err && !AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                              DOMAIN_ALIAS_RID_ADMINS,
                                              0, 0, 0, 0, 0, 0, &adm))
            err = GetLastError();
        if (!err) {
            size = sizeof(ACL) + 3 * (sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD)) +
                GetLengthSid(tu->User.Sid) + GetLengthSid(sys) + GetLengthSid(adm);
            if ((acl = malloc(size)) == NULL)
                err = ERROR_NOT_ENOUGH_MEMORY;
            else if (!InitializeAcl(acl, size, ACL_REVISION) ||
                     !AddAccessAllowedAceEx(acl, ACL_REVISION, flags,
                                            FILE_ALL_ACCESS, tu->User.Sid) ||
                     !AddAccessAllowedAceEx(acl, ACL_REVISION, flags,
                                            FILE_ALL_ACCESS, sys) ||
                     !AddAccessAllowedAceEx(acl, ACL_REVISION, flags,
                                            FILE_ALL_ACCESS, adm) ||
                     !InitializeSecurityDescriptor(&g_sd[i].sd,
                                                   SECURITY_DESCRIPTOR_REVISION) ||
                     !SetSecurityDescriptorDacl(&g_sd[i].sd, TRUE, acl, FALSE) ||
                     !SetSecurityDescriptorControl(&g_sd[i].sd, SE_DACL_PROTECTED,
                                                   SE_DACL_PROTECTED)) {
                err = GetLastError();
                if (!err)
                    err = ERROR_INVALID_ACL;
            }
        }
        if (sys)
            FreeSid(sys);
        if (adm)
            FreeSid(adm);
        free(tu);
        if (err) {
            free(acl);
            SetLastError(err);
            return 0;
        }
        g_sd[i].acl = acl;              /* kept: the descriptor points to it */
        g_sd[i].ok = 1;
    }
    sa->nLength = sizeof(*sa);
    sa->lpSecurityDescriptor = &g_sd[i].sd;
    sa->bInheritHandle = FALSE;
    return 1;
}

/* ---- creating ---- */

BOOL kitty_store_acl_mkdir(const char *dir)
{
    char *full = NULL;
    SECURITY_ATTRIBUTES sa;
    DWORD err;
    BOOL ok;
    int v = ksa_decide(dir, &full);

    if (v == KSA_SET) {
        if (kitty_store_acl_attrs(1, &sa) && CreateDirectoryA(dir, &sa)) {
            free(full);
            return TRUE;
        }
        err = GetLastError();
        /* never block the store: created as before, the failure reported */
        if ((ok = CreateDirectoryA(dir, NULL)) != FALSE)
            kitty_store_acl_report(KSA_FAILED, full, err);
    } else {
        ok = CreateDirectoryA(dir, NULL);
        if (ok && kitty_store_acl_reported(v))
            kitty_store_acl_report(v, full, 0);
    }
    err = GetLastError();
    free(full);
    SetLastError(err);
    return ok;
}

int kitty_store_acl_new_file(const char *path)
{
    char *full = NULL;
    SECURITY_ATTRIBUTES sa;
    HANDLE h;
    DWORD err = 0;
    int v = ksa_decide(path, &full), made = 0;

    if (v == KSA_SET) {
        if (kitty_store_acl_attrs(0, &sa)) {
            h = CreateFileA(path, GENERIC_WRITE, 0, &sa, CREATE_NEW,
                            FILE_ATTRIBUTE_NORMAL, NULL);
            if (h != INVALID_HANDLE_VALUE) {
                CloseHandle(h);
                made = 1;
            } else {
                err = GetLastError();
            }
        } else {
            err = GetLastError();
        }
        /* not made: the write creates it without the rights - unless its
         * folder is missing (then nothing is written) or it came meanwhile */
        if (!made && err != ERROR_PATH_NOT_FOUND && err != ERROR_FILE_EXISTS &&
            err != ERROR_ALREADY_EXISTS)
            kitty_store_acl_report(KSA_FAILED, full, err);
    } else if (kitty_store_acl_reported(v)) {
        kitty_store_acl_report(v, full, 0);
    }
    free(full);
    return made;
}

/* ---- the report ---- */

static int (*g_report)(const char *msg) = NULL;
static int g_logged[KSA_NVERDICTS], g_shown = 0, g_busy = 0;

void kitty_set_store_acl_report(int (*fn)(const char *msg))
{
    g_report = fn;
}

void kitty_store_acl_forget(void)
{
    memset(g_logged, 0, sizeof(g_logged));
    g_shown = 0;
}

void kitty_store_acl_report(int verdict, const char *path, DWORD err)
{
    char *root, *msg, *mark;
    const char *where, *why;
    const char *vals[2];

    /* writing the marker creates KiTTYState: no report from inside one */
    if (g_busy || !path || verdict < 0 || verdict >= KSA_NVERDICTS ||
        !kitty_store_acl_reported(verdict))
        return;
    g_busy = 1;
    root = portable_root_dir();
    where = (root && kitty_store_acl_path_under(path, root)) ? root : path;
    if (verdict == KSA_FAILED) {
        why = kitty_win_reason(err);
        msg = ksa_fmt(KT_STORE_ACL_FAILED_FMT, where, why);
    } else {
        why = verdict == KSA_SHARE ? "share" : "fat";
        msg = ksa_fmt(KT_STORE_ACL_NOT_SET_FMT, where);
    }
    if (msg && !g_logged[verdict]) {
        g_logged[verdict] = 1;
        vals[0] = path;
        vals[1] = why;
        kitty_wev_suite(KITTY_EV_STORE_ACL_NOT_SET, EVENTLOG_WARNING_TYPE,
                        vals, 2, msg);
    }
    /* the notice: once per store (its KiTTYState remembers it), only where
     * the store is known and a front end shows it. Under readonly=yes the
     * marker cannot be written, so the notice would come at every start:
     * the record alone. */
    if (msg && root && g_report && !g_shown && !kitty_store_readonly()) {
        mark = kitty_portable_load_state_dup(KITTY_STORE_ACL_MARKER);
        if (kitty_store_acl_notice_due(mark != NULL, g_shown)) {
            g_shown = 1;
            if (g_report(msg))
                kitty_portable_store_state_string(KITTY_STORE_ACL_MARKER, "1");
        }
        if (mark)
            safefree(mark);
    }
    if (root)
        safefree(root);
    free(msg);
    g_busy = 0;
}
