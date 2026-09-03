/*
 * kitty_authenticode.c - shared Authenticode trust + publisher-CN gate.
 * See kitty_authenticode.h. Moved verbatim from kitty_win.c's
 * kitty_verify_signature so the updater and the kageant launcher share one
 * implementation.
 */
#include <windows.h>
#include <wintrust.h>
#include <softpub.h>   /* WINTRUST_ACTION_GENERIC_VERIFY_V2 */
#include <stdlib.h>
/* wincrypt.h (CryptQueryObject / signer cert) comes in via windows.h */

#include "kitty_authenticode.h"

/* Our signing identity - the one place the publisher CN is written. */
#define KITTY_PUBLISHER_CN "KAPPER NETWORK-COMMUNICATIONS GmbH"

static int authenticode_verify_ex(const char *path, int online_revocation)
{
    wchar_t wpath[MAX_PATH];
    if (MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, MAX_PATH) == 0)
        return 0;

    /* (1) Trust chain (kills self-signed spoofs). */
    WINTRUST_FILE_INFO fi; memset(&fi, 0, sizeof(fi));
    fi.cbStruct = sizeof(fi);
    fi.pcwszFilePath = wpath;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_DATA wd; memset(&wd, 0, sizeof(wd));
    wd.cbStruct = sizeof(wd);
    wd.dwUIChoice = WTD_UI_NONE;
    if (online_revocation) {
        wd.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN;
    } else {
        /* No revocation fetch: nothing leaves the machine and nothing waits
         * for a CRL server. The chain and the publisher-CN pin below still
         * decide; see the header for which callers may use this. */
        wd.fdwRevocationChecks = WTD_REVOKE_NONE;
        wd.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
    }
    wd.dwUnionChoice = WTD_CHOICE_FILE;
    wd.pFile = &fi;
    wd.dwStateAction = WTD_STATEACTION_VERIFY;
    LONG st = WinVerifyTrust((HWND)INVALID_HANDLE_VALUE, &action, &wd);
    wd.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust((HWND)INVALID_HANDLE_VALUE, &action, &wd);
    if (st != ERROR_SUCCESS)
        return 0;

    /* (2) Signer CN pin (kills a different-but-valid certificate). */
    int matched = 0;
    HCERTSTORE hStore = NULL; HCRYPTMSG hMsg = NULL;
    if (CryptQueryObject(CERT_QUERY_OBJECT_FILE, wpath,
            CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
            CERT_QUERY_FORMAT_FLAG_BINARY, 0, NULL, NULL, NULL,
            &hStore, &hMsg, NULL)) {
        DWORD si_sz = 0;
        if (CryptMsgGetParam(hMsg, CMSG_SIGNER_INFO_PARAM, 0, NULL, &si_sz) &&
                si_sz > 0) {
            CMSG_SIGNER_INFO *si = (CMSG_SIGNER_INFO *)malloc(si_sz);
            if (si != NULL &&
                    CryptMsgGetParam(hMsg, CMSG_SIGNER_INFO_PARAM, 0, si,
                                     &si_sz)) {
                CERT_INFO ci; memset(&ci, 0, sizeof(ci));
                ci.Issuer = si->Issuer;
                ci.SerialNumber = si->SerialNumber;
                PCCERT_CONTEXT cert = CertFindCertificateInStore(hStore,
                    X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0,
                    CERT_FIND_SUBJECT_CERT, &ci, NULL);
                if (cert != NULL) {
                    char cn[256] = "";
                    if (CertGetNameStringA(cert, CERT_NAME_ATTR_TYPE, 0,
                            szOID_COMMON_NAME, cn, sizeof(cn)) > 1) {
                        if (_stricmp(cn, KITTY_PUBLISHER_CN) == 0)
                            matched = 1;
                    }
                    CertFreeCertificateContext(cert);
                }
            }
            if (si != NULL) free(si);
        }
    }
    if (hMsg != NULL) CryptMsgClose(hMsg);
    if (hStore != NULL) CertCloseStore(hStore, 0);
    return matched;
}

int kitty_authenticode_verify(const char *path)
{
    return authenticode_verify_ex(path, 1);
}

int kitty_authenticode_verify_offline(const char *path)
{
    return authenticode_verify_ex(path, 0);
}

int kitty_file_version(const char *path, unsigned long *ms, unsigned long *ls)
{
    DWORD dummy, sz = GetFileVersionInfoSizeA(path, &dummy);
    if (sz == 0)
        return 0;
    void *buf = malloc(sz);
    if (!buf)
        return 0;
    int ok = 0;
    if (GetFileVersionInfoA(path, 0, sz, buf)) {
        VS_FIXEDFILEINFO *ffi = NULL;
        UINT ffi_len = 0;
        if (VerQueryValueA(buf, "\\", (void **)&ffi, &ffi_len) &&
                ffi != NULL && ffi_len >= sizeof(*ffi)) {
            if (ms) *ms = ffi->dwFileVersionMS;
            if (ls) *ls = ffi->dwFileVersionLS;
            ok = 1;
        }
    }
    free(buf);
    return ok;
}

int kitty_verify_sibling(const char *path)
{
    char self[MAX_PATH];
    if (GetModuleFileNameA(NULL, self, sizeof(self)) == 0)
        return 0;

    /* A signed (release) binary demands a signed sibling; an unsigned dev
     * build cannot, so it checks version only. Fail-closed on the signature
     * leg: if we are signed and the sibling is not (or is signed by someone
     * else), reject. */
    if (kitty_authenticode_verify(self)) {
        if (!kitty_authenticode_verify(path))
            return 0;
    }

    /* Exact version match either way. */
    unsigned long am, al, bm, bl;
    if (!kitty_file_version(self, &am, &al))
        return 0;
    if (!kitty_file_version(path, &bm, &bl))
        return 0;
    return (am == bm) && (al == bl);
}
