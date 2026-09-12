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
        return;
    }
    if (!gui) {
        fprintf(stderr, "%s\n", msg);
        fflush(stderr);
    }
    kg_eventlog(msg);
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

#ifdef KITTY_RELEASE_SIGNED

/*
 * What we REFUSE on - three outcomes, and nothing else:
 *
 *   TRUST_E_BAD_DIGEST    the file's own hash does not match what the
 *                         signature covers: it was modified after signing.
 *                         The case this check exists for.
 *   TRUST_E_NOSIGNATURE   AND the PE carries no certificate table at all
 *                         (kg_has_cert_table below). The second half of that
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
#ifndef TRUST_E_BAD_DIGEST
#define TRUST_E_BAD_DIGEST ((LONG)0x80096010L)
#endif
#ifndef TRUST_E_NOSIGNATURE
#define TRUST_E_NOSIGNATURE ((LONG)0x800B0100L)
#endif

/*
 * Does this process's own file carry a certificate table?
 *
 * The one fact that separates "the signature was stripped" from "this Windows
 * could not read the signature", and it needs no crypto at all: the PE
 * optional header's data directory entry 4 (IMAGE_DIRECTORY_ENTRY_SECURITY)
 * has a nonzero Size on every signed image and a zero Size on an unsigned one.
 * Read straight out of the mapped image, because the module is already in
 * memory - no file handle, no allocation, nothing that can fail for reasons of
 * its own.
 *
 * Returns 1 = there is a table, 0 = there is none, and -1 = the headers could
 * not be walked, which the caller treats like "cannot judge" rather than as an
 * answer. PE32 and PE32+ differ only in where the data directory starts, so
 * both are read through their own optional-header type.
 */
static int kg_has_cert_table(void)
{
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)GetModuleHandle(NULL);
    const IMAGE_NT_HEADERS *nt;
    const IMAGE_DATA_DIRECTORY *dd;
    WORD magic;

    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE)
        return -1;
    nt = (const IMAGE_NT_HEADERS *)((const char *)dos + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return -1;

    magic = nt->OptionalHeader.Magic;
    if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        const IMAGE_NT_HEADERS32 *nt32 = (const IMAGE_NT_HEADERS32 *)nt;
        if (nt32->OptionalHeader.NumberOfRvaAndSizes <=
                IMAGE_DIRECTORY_ENTRY_SECURITY)
            return -1;
        dd = &nt32->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_SECURITY];
    } else if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        const IMAGE_NT_HEADERS64 *nt64 = (const IMAGE_NT_HEADERS64 *)nt;
        if (nt64->OptionalHeader.NumberOfRvaAndSizes <=
                IMAGE_DIRECTORY_ENTRY_SECURITY)
            return -1;
        dd = &nt64->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_SECURITY];
    } else {
        return -1;
    }
    return dd->Size != 0 ? 1 : 0;
}

typedef LONG (WINAPI *kg_winverifytrust_t)(HWND, GUID *, LPVOID);
typedef CRYPT_PROVIDER_DATA *(WINAPI *kg_provdata_t)(HANDLE);
typedef CRYPT_PROVIDER_SGNR *(WINAPI *kg_provsigner_t)(CRYPT_PROVIDER_DATA *,
                                                       DWORD, BOOL, DWORD);
typedef DWORD (WINAPI *kg_certname_t)(PCCERT_CONTEXT, DWORD, DWORD, void *,
                                      LPSTR, DWORD);

/*
 * Verify this process's own file. Fills `reason` with the short technical
 * words the message shows and returns 1 when the program must NOT run.
 */
static int kg_signature_verdict(char *reason, size_t reasonsz)
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
    int refuse = 0;
    DWORD len;

    len = GetModuleFileNameW(NULL, wpath, 1024);
    wpath[1023] = L'\0';
    if (len == 0 || len >= 1024)
        return 0;                      /* cannot tell - let it run */

    wintrust = LoadLibraryA("wintrust.dll");
    if (!wintrust)
        return 0;                      /* cannot judge */
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
        return 0;                      /* cannot judge */
    }

    memset(&fi, 0, sizeof(fi));
    fi.cbStruct = sizeof(fi);
    fi.pcwszFilePath = wpath;
    memset(&wd, 0, sizeof(wd));
    wd.cbStruct = sizeof(wd);
    wd.dwUIChoice = WTD_UI_NONE;
    /* Cached revocation data only: a start-up check must not wait on a CRL
     * server, and nothing about this process leaves the machine. */
    wd.fdwRevocationChecks = WTD_REVOKE_NONE;
    wd.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
    wd.dwUnionChoice = WTD_CHOICE_FILE;
    wd.pFile = &fi;
    /* VERIFY rather than IGNORE, because the signer certificate is read out of
     * the state data below - which only exists while the state is open. */
    wd.dwStateAction = WTD_STATEACTION_VERIFY;
    st = p_verify((HWND)INVALID_HANDLE_VALUE, &action, &wd);

    if (st == TRUST_E_BAD_DIGEST) {
        strncpy(reason, "bad digest", reasonsz - 1);
        reason[reasonsz - 1] = '\0';
        refuse = 1;
    } else if (st == TRUST_E_NOSIGNATURE && kg_has_cert_table() == 0) {
        /* The header agrees there is nothing to verify: the signature was
         * stripped. A NOSIGNATURE with a table present is a wintrust that
         * could not read ours, and falls through to "run". */
        strncpy(reason, "not signed", reasonsz - 1);
        reason[reasonsz - 1] = '\0';
        refuse = 1;
    } else if (st == ERROR_SUCCESS) {
        /*
         * The chain is good. Now the only question left: is it OUR chain? A
         * different but perfectly valid certificate is exactly what a
         * re-signed copy carries.
         *
         * A signer we cannot READ is not a signer we may condemn: if any of
         * these steps is unavailable (an old wintrust without the helpers, no
         * crypt32) this is another "cannot judge" and the program runs.
         */
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
                           (void *)szOID_COMMON_NAME, cn, sizeof(cn)) > 1 &&
                    _stricmp(cn, KITTY_PUBLISHER_CN) != 0) {
                /* Names the CN found, because that is the one fact worth
                 * knowing: a re-signed copy is identified by whose it is. */
                _snprintf(reason, reasonsz, "signer: %s", cn);
                reason[reasonsz - 1] = '\0';
                refuse = 1;
            }
        }
        if (crypt32)
            FreeLibrary(crypt32);
    }
    /* Everything else: cannot judge, run. */

    wd.dwStateAction = WTD_STATEACTION_CLOSE;
    p_verify((HWND)INVALID_HANDLE_VALUE, &action, &wd);
    FreeLibrary(wintrust);
    return refuse;
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
