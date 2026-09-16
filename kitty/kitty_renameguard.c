/*
 * kitty_renameguard.c - the two startup guards: see kitty_renameguard.h for
 * what each buys and what it deliberately does not.
 *
 * Everything here is Windows XP era on purpose: GetModuleFileNameA,
 * MessageBoxA, stderr, and every signature entry point resolved at RUN TIME.
 * The guards run before anything else in the process, so they cannot depend on
 * KiTTY's own startup having happened - no settings, no kitty.ini, no registry,
 * no themed message box.
 *
 * Nothing in this file adds an import library to any target. That is
 * deliberate: the file lives in `utils`, which klink, kscp, ksftp and
 * kittygen-cli link without wintrust or the version library, and a static
 * import of WinVerifyTrust would fail their link outright - and on a Windows
 * that lacks the entry point it would fail the LOADER, before main(), which is
 * the one failure mode a startup guard must never cause.
 */
#include <windows.h>
#include <wintrust.h>
#include <softpub.h>   /* WINTRUST_ACTION_GENERIC_VERIFY_V2 */
#include <stdio.h>
#include <string.h>

#include "kitty_text.h"
#include "kitty_authenticode.h"   /* KITTY_PUBLISHER_CN - the single copy */
#include "kitty_oldwin.h"         /* kitty_api_from: resolve, and RECORD */
#include "kitty_renameguard.h"

/*
 * ASCII case folding, done here rather than with the C library's: this runs
 * before anything has set a locale, and a file name is matched against
 * built-in ASCII product names, so a locale-aware fold could only surprise.
 */
static char kg_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/*
 * Is there a desktop for a message box to appear on, and somebody to click it?
 *
 * A modal box put up by a process with no visible window station is a HANG: it
 * waits for an OK that nobody can give, and the program then neither runs nor
 * exits. That is the shape of a KiTTY started by the task scheduler, by a
 * service, or in session 0. WSF_VISIBLE on the process's own window station is
 * the answer Windows gives to exactly this question, and both calls are
 * Windows 2000 and later - XP has them.
 *
 * Unknown counts as INTERACTIVE: a failed call is not evidence of a service,
 * and a desktop user who got no message at all would be left with a program
 * that exits without a word.
 */
static int kg_interactive(void)
{
    HWINSTA sta = GetProcessWindowStation();
    USEROBJECTFLAGS uof;
    DWORD got = 0;

    if (!sta)
        return 1;
    memset(&uof, 0, sizeof(uof));
    if (!GetUserObjectInformationA(sta, UOI_FLAGS, &uof, sizeof(uof), &got) ||
            got < sizeof(uof))
        return 1;
    return (uof.dwFlags & WSF_VISIBLE) != 0;
}

/*
 * One line into the Application event log, under our own source name.
 *
 * No message DLL is registered for "KiTTY++", so the Event Viewer wraps the
 * text in its "The description for Event ID ... cannot be found" sentence and
 * then prints our string as the inserted parameter. That is accepted on
 * purpose: registering a message file means writing to HKLM at install time
 * for one line that reads perfectly well as it is, and this has to work from a
 * portable copy that was never installed at all.
 *
 * Best effort throughout - a refusal must not become a second failure because
 * the log was unavailable.
 */
static void kg_eventlog(const char *msg)
{
    HANDLE h = RegisterEventSourceA(NULL, KT_RENAME_GUARD_TITLE);
    const char *strings[1];

    if (!h)
        return;
    strings[0] = msg;
    ReportEventA(h, EVENTLOG_ERROR_TYPE, 0, 0, NULL, 1, 0, strings, NULL);
    DeregisterEventSource(h);
}

/*
 * One report, one way out. `msg` is already composed.
 *
 * The refusal has to REACH somebody, and who that is depends on how the
 * program was started rather than on which program it is:
 *
 *  - a windowed program on an interactive desktop gets the box;
 *  - a windowed program without one (scheduled task, service, session 0) would
 *    hang on that box, so it gets the event log instead;
 *  - a command-line tool keeps stderr AND writes the event log line, because a
 *    scheduled klink under the wrong name has nobody reading its output: the
 *    job just fails, and the log is the only place the reason survives.
 */
static void kg_report(const char *msg, int gui)
{
    if (gui && kg_interactive()) {
        /* Owner NULL: there is no window yet, and there will not be one. */
        MessageBoxA(NULL, msg, KT_RENAME_GUARD_TITLE, MB_OK | MB_ICONERROR);
    } else if (!gui) {
        fprintf(stderr, "%s\n", msg);
        fflush(stderr);
    }
    /* In EVERY case one Application event-log line - the admin's trace,
     * whether or not the user also saw the box or the stderr line.
     * Applies to all three guards that share this. */
    kg_eventlog(msg);
}

/*
 * The two decisions above, for the integrity self-check (kitty_selfcheck.c),
 * which cannot live in this file: it needs `crypto`, and `utils` links into
 * programs that have none. One reporting path for all three guards.
 */
int kitty_guard_interactive(void)
{
    return kg_interactive();
}

void kitty_guard_report(const char *msg, int gui, int allow_box)
{
    if (!allow_box && gui) {
        /* A windowed program that must not put up a box: the log only. */
        kg_eventlog(msg);
        return;
    }
    kg_report(msg, gui);
}

int kitty_rename_guard(const char *const *prefixes, int nprefixes, int gui)
{
    /*
     * 1024 rather than MAX_PATH: a path can exceed 260 characters, and a
     * truncated read is answered with "cannot tell" below rather than with a
     * wrong verdict.
     */
    char path[1024];
    /* The format string plus one prefix plus one base name; the base name is
     * itself bounded by the path buffer, so this cannot overrun. */
    char msg[1024 + 256];
    const char *base, *p;
    DWORD len;
    int i;

    if (!prefixes || nprefixes < 1)
        return 0;

    len = GetModuleFileNameA(NULL, path, sizeof(path));
    /* Pre-Vista does not NUL-terminate when the buffer was too small. */
    path[sizeof(path) - 1] = '\0';
    if (len == 0 || len >= sizeof(path))
        return 0;                      /* cannot tell - let it run */

    base = path;
    for (p = path; *p; p++)
        if (*p == '\\' || *p == '/')
            base = p + 1;

    for (i = 0; i < nprefixes; i++) {
        const char *a = base, *b = prefixes[i];
        while (*b && kg_lower(*a) == kg_lower(*b)) {
            a++;
            b++;
        }
        if (!*b)
            return 0;                  /* the whole prefix matched */
    }

    sprintf(msg, KT_RENAME_GUARD_MSG, prefixes[0], base);
    kg_report(msg, gui);
    return 1;
}

/* ------------------------------------------------------------------------
 * The signature self-check.
 * ------------------------------------------------------------------------ */

#ifndef TRUST_E_BAD_DIGEST
#define TRUST_E_BAD_DIGEST ((LONG)0x80096010L)
#endif
#ifndef TRUST_E_NOSIGNATURE
#define TRUST_E_NOSIGNATURE ((LONG)0x800B0100L)
#endif

/*
 * Does the PE at `path` carry an Authenticode certificate table? 1 / 0, and
 * -1 when the headers could not be read or walked, which the caller treats
 * as "cannot judge" rather than as an answer. The same test the running
 * image gets below, over the file's own bytes.
 */
static int kg_file_has_cert_table(const char *path)
{
    HANDLE h;
    unsigned char hdr[4096];
    DWORD got = 0;
    LONG e_lfanew;
    const unsigned char *nt;
    WORD magic;
    unsigned nrva_off, dd_off;

    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    memset(hdr, 0, sizeof(hdr));
    if (!ReadFile(h, hdr, sizeof(hdr), &got, NULL) || got < 64) { CloseHandle(h); return -1; }
    CloseHandle(h);
    if (hdr[0] != 'M' || hdr[1] != 'Z')
        return -1;
    e_lfanew = (LONG)(hdr[60] | (hdr[61] << 8) | (hdr[62] << 16) | ((DWORD)hdr[63] << 24));
    if (e_lfanew < 0 || (DWORD)e_lfanew + 24 + 2 > got)
        return -1;
    nt = hdr + e_lfanew;
    if (nt[0] != 'P' || nt[1] != 'E' || nt[2] != 0 || nt[3] != 0)
        return -1;
    magic = (WORD)(nt[24] | (nt[25] << 8));
    /* NumberOfRvaAndSizes and the data directory: PE32 at optional-header
     * offsets 92 / 96, PE32+ at 108 / 112. */
    if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) { nrva_off = 24 + 92; dd_off = 24 + 96; }
    else if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) { nrva_off = 24 + 108; dd_off = 24 + 112; }
    else return -1;
    if ((DWORD)e_lfanew + dd_off + 8 * (IMAGE_DIRECTORY_ENTRY_SECURITY + 1) > got)
        return -1;
    {
        DWORD nrva = nt[nrva_off] | (nt[nrva_off + 1] << 8) | (nt[nrva_off + 2] << 16) | ((DWORD)nt[nrva_off + 3] << 24);
        const unsigned char *dd = nt + dd_off + 8 * IMAGE_DIRECTORY_ENTRY_SECURITY;
        DWORD size = dd[4] | (dd[5] << 8) | (dd[6] << 16) | ((DWORD)dd[7] << 24);
        if (nrva <= IMAGE_DIRECTORY_ENTRY_SECURITY)
            return -1;
        return size != 0 ? 1 : 0;
    }
}

typedef LONG (WINAPI *kg_winverifytrust_t)(HWND, GUID *, LPVOID);
typedef CRYPT_PROVIDER_DATA *(WINAPI *kg_provdata_t)(HANDLE);
typedef CRYPT_PROVIDER_SGNR *(WINAPI *kg_provsigner_t)(CRYPT_PROVIDER_DATA *,
                                                       DWORD, BOOL, DWORD);
typedef DWORD (WINAPI *kg_certname_t)(PCCERT_CONTEXT, DWORD, DWORD, void *,
                                      LPSTR, DWORD);

/*
 * The Authenticode reading of one file: what the startup guard decides on,
 * as a value, for any path - so the Applications leaf and the guard judge
 * a file the same way and the decision exists once. See the guard's comment
 * below for why each outcome maps as it does; in short:
 *   KG_SIG_MODIFIED  BAD_DIGEST on a Windows that can compute a SHA-256
 *                    digest (CryptCATAdminAcquireContext2 present);
 *   KG_SIG_UNSIGNED  NOSIGNATURE and the PE has no certificate table;
 *   KG_SIG_OTHER     a valid chain whose signer is not our publisher
 *                    (`signer` receives the CN);
 *   KG_SIG_OURS      a valid chain, our publisher;
 *   KG_SIG_CANNOT    everything else - an algorithm or root this Windows
 *                    does not know, wintrust missing, a signer it cannot
 *                    read: no verdict, and never a refusal.
 * Cached revocation data only, nothing leaves the machine.
 */
int kitty_signature_reading(const char *path, char *signer, size_t signersz)
{
    wchar_t wpath[1024];
    WINTRUST_FILE_INFO fi;
    WINTRUST_DATA wd;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    HMODULE wintrust, crypt32;
    kg_winverifytrust_t p_verify;
    kg_provdata_t p_provdata;
    kg_provsigner_t p_provsigner;
    kg_certname_t p_certname;
    CRYPT_PROVIDER_DATA *pd;
    CRYPT_PROVIDER_SGNR *sgnr;
    PCCERT_CONTEXT cert;
    char cn[256];
    LONG st;
    int result = KG_SIG_CANNOT;

    if (signer && signersz) signer[0] = '\0';
    if (MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, 1024) == 0)
        return KG_SIG_CANNOT;

    wintrust = LoadLibraryA("wintrust.dll");
    if (!wintrust)
        return KG_SIG_CANNOT;
    p_verify = (kg_winverifytrust_t)kitty_api_from(
        wintrust, "wintrust.dll", "WinVerifyTrust", KITTY_API_OPTIONAL,
        "the signature self-check");
    p_provdata = (kg_provdata_t)kitty_api_from(
        wintrust, "wintrust.dll", "WTHelperProvDataFromStateData",
        KITTY_API_OPTIONAL, "the signature self-check");
    p_provsigner = (kg_provsigner_t)kitty_api_from(
        wintrust, "wintrust.dll", "WTHelperGetProvSignerFromChain",
        KITTY_API_OPTIONAL, "the signature self-check");
    if (!p_verify) {
        FreeLibrary(wintrust);
        return KG_SIG_CANNOT;
    }

    memset(&fi, 0, sizeof(fi));
    fi.cbStruct = sizeof(fi);
    fi.pcwszFilePath = wpath;
    memset(&wd, 0, sizeof(wd));
    wd.cbStruct = sizeof(wd);
    wd.dwUIChoice = WTD_UI_NONE;
    wd.fdwRevocationChecks = WTD_REVOKE_NONE;
    wd.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
    wd.dwUnionChoice = WTD_CHOICE_FILE;
    wd.pFile = &fi;
    wd.dwStateAction = WTD_STATEACTION_VERIFY;
    st = p_verify((HWND)INVALID_HANDLE_VALUE, &action, &wd);

    if (st == TRUST_E_BAD_DIGEST) {
        if (kitty_api_from(wintrust, "wintrust.dll",
                           "CryptCATAdminAcquireContext2", KITTY_API_OPTIONAL,
                           "the signature self-check") != NULL)
            result = KG_SIG_MODIFIED;
    } else if (st == TRUST_E_NOSIGNATURE && kg_file_has_cert_table(path) == 0) {
        result = KG_SIG_UNSIGNED;
    } else if (st == ERROR_SUCCESS) {
        crypt32 = LoadLibraryA("crypt32.dll");
        p_certname = crypt32 ? (kg_certname_t)kitty_api_from(
            crypt32, "crypt32.dll", "CertGetNameStringA", KITTY_API_OPTIONAL,
            "the signature self-check") : NULL;
        if (p_provdata && p_provsigner && p_certname &&
                (pd = p_provdata(wd.hWVTStateData)) != NULL &&
                (sgnr = p_provsigner(pd, 0, FALSE, 0)) != NULL &&
                sgnr->csCertChain > 0 && sgnr->pasCertChain != NULL &&
                (cert = sgnr->pasCertChain[0].pCert) != NULL) {
            cn[0] = '\0';
            if (p_certname(cert, CERT_NAME_ATTR_TYPE, 0,
                           (void *)szOID_COMMON_NAME, cn, sizeof(cn)) > 1) {
                if (signer && signersz) { strncpy(signer, cn, signersz - 1); signer[signersz - 1] = '\0'; }
                result = _stricmp(cn, KITTY_PUBLISHER_CN) == 0 ? KG_SIG_OURS : KG_SIG_OTHER;
            }
        }
        if (crypt32)
            FreeLibrary(crypt32);
    }

    wd.dwStateAction = WTD_STATEACTION_CLOSE;
    p_verify((HWND)INVALID_HANDLE_VALUE, &action, &wd);
    FreeLibrary(wintrust);
    return result;
}

int kitty_file_has_cert_table(const char *path)
{
    return kg_file_has_cert_table(path);
}


#ifdef KITTY_RELEASE_SIGNED

/*
 * What we REFUSE on - three outcomes, and nothing else:
 *
 *   TRUST_E_BAD_DIGEST    the file's own hash does not match what the
 *                         signature covers: it was modified after signing.
 *                         The case this check exists for - on a Windows
 *                         that can compute a SHA-256 Authenticode digest.
 *                         One that cannot (XP; Vista and 7 without the
 *                         2012 SHA-2 update) answers BAD_DIGEST for every
 *                         intact file, so there it is "cannot judge" (see
 *                         the CryptCATAdminAcquireContext2 probe below).
 *   TRUST_E_NOSIGNATURE   AND the PE carries no certificate table at all
 *                         (kitty_file_has_cert_table above). The second half of that
 *                         condition is not decoration: this code does not only
 *                         mean "unsigned". wintrust returns it whenever the
 *                         provider cannot interpret the file as a signed
 *                         SUBJECT, which an XP-era wintrust can do with a
 *                         SHA-256 chain it has no idea how to parse - it fails
 *                         at subject recognition, before it ever reaches the
 *                         algorithm-specific errors listed below. So the PE
 *                         header decides: no certificate table means the
 *                         signature really was stripped, while a table this
 *                         Windows could not read means it cannot judge, and
 *                         then we run.
 *   a VERIFIED signature whose signer CN is not our publisher - a re-signed
 *                         copy carries a perfectly valid signature belonging
 *                         to somebody else.
 *
 * Everything else means "cannot judge", and a program that cannot be judged
 * must still run - that is what keeps Windows XP and an offline Windows 7
 * working. Deliberately NOT refused: TRUST_E_SUBJECT_NOT_TRUSTED (the
 * signature parsed and was not trusted, which is a statement about this
 * machine's policy rather than evidence about this file), CERT_E_UNTRUSTEDROOT
 * and CERT_E_CHAINING (the root is not in this machine's store - an old or
 * unpatched Windows), CERT_E_EXPIRED and CERT_E_REVOCATION_FAILURE (a wrong
 * clock, or no revocation data offline), TRUST_E_PROVIDER_UNKNOWN and
 * TRUST_E_ACTION_UNKNOWN, NTE_BAD_ALGID and the CRYPT_E_* family for a hash or
 * signature algorithm this Windows does not implement, CRYPT_E_SECURITY_SETTINGS
 * where policy switched the check off, and wintrust.dll not being loadable at
 * all. So is every code not named above: an unclassified error is a "cannot
 * judge", never a refusal.
 *
 * The version of Windows is never consulted. Compatibility mode fakes it, and
 * "can this machine verify an Authenticode signature" is a question you answer
 * by asking it, not by guessing from a version number.
 */



/*
 * Verify this process's own file. Fills `reason` with the short technical
 * words the message shows and returns 1 when the program must NOT run - the
 * reading above, applied to the own path, with the guard's decisions:
 * MODIFIED, UNSIGNED and OTHER refuse, OURS and CANNOT run.
 */
static int kg_signature_verdict(char *reason, size_t reasonsz)
{
    char path[1024];
    char signer[256];
    DWORD len = GetModuleFileNameA(NULL, path, sizeof(path));
    if (len == 0 || len >= sizeof(path))
        return 0;                      /* cannot tell - let it run */
    path[len] = '\0';
    switch (kitty_signature_reading(path, signer, sizeof(signer))) {
      case KG_SIG_MODIFIED:
        strncpy(reason, "bad digest", reasonsz - 1); reason[reasonsz - 1] = '\0';
        return 1;
      case KG_SIG_UNSIGNED:
        strncpy(reason, "not signed", reasonsz - 1); reason[reasonsz - 1] = '\0';
        return 1;
      case KG_SIG_OTHER:
        /* Names the CN found, because that is the one fact worth knowing: a
         * re-signed copy is identified by whose it is. */
        _snprintf(reason, reasonsz, "signer: %s", signer); reason[reasonsz - 1] = '\0';
        return 1;
      default:
        return 0;                      /* ours, or cannot judge: run */
    }
}

int kitty_signature_guard(int gui)
{
    /* Once per process: the verification reads and hashes the whole file, and
     * every entry point calls this exactly once anyway. */
    static int decided = 0;
    static int verdict = 0;
    char reason[300];
    char msg[600];

    if (decided)
        return verdict;
    decided = 1;

    reason[0] = '\0';
    if (!kg_signature_verdict(reason, sizeof(reason)))
        return 0;

    sprintf(msg, KT_SIGNATURE_GUARD_MSG, reason);
    kg_report(msg, gui);
    verdict = 1;
    return 1;
}

#else /* !KITTY_RELEASE_SIGNED */

/*
 * Dev and test builds are not signed, so there is nothing here to check and
 * nothing of the check in the binary. The call sites stay unconditional: one
 * shape of startup code, and a build flag decides whether it does anything.
 */
int kitty_signature_guard(int gui)
{
    (void)gui;
    return 0;
}

#endif /* KITTY_RELEASE_SIGNED */
