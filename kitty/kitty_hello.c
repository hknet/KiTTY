/*
 * KiTTY: Windows Hello presence check and key protector - see
 * kitty_hello.h.
 *
 * Plain-C WinRT: the MinGW headers carry the C ABI for UserConsentVerifier
 * and for the KeyCredentialManager STATICS (create/open and their async
 * operations), so what is declared by hand below is only what they lack:
 * the Win32 interop interface that parents the consent prompt, and the
 * KeyCredential instance side (RequestSignAsync and its operation result),
 * which the MinGW header forward-declares but does not define.
 * combase.dll is loaded dynamically and every failure maps to a value the
 * callers treat as a denial, so a system without WinRT simply reports
 * unavailable rather than failing to start.
 */

#define COBJMACROS
#include <winsock2.h>   /* putty.h pulls it; it insists on preceding windows.h */
#include <windows.h>
#include <initguid.h>
#include <roapi.h>
#include <winstring.h>
#include <inspectable.h>
#include <asyncinfo.h>
#include <windows.security.credentials.ui.h>
#include <windows.security.credentials.h>
#include <windows.storage.streams.h>
#include <robuffer.h>

#include "putty.h"
#include "platform.h"   /* HandleWaitList: the nested WebAuthn pump must
                         * serve the app's handle waits (pipe accepts) */
#include "ssh.h"
#include "kitty_hello.h"

/* Shorten the ABI mouthfuls locally. */
typedef __x_ABI_CWindows_CSecurity_CCredentials_CUI_CIUserConsentVerifierStatics
    HelloStatics;
typedef __FIAsyncOperation_1_UserConsentVerifierAvailability  HelloAvailOp;
typedef __FIAsyncOperation_1_UserConsentVerificationResult    HelloVerifyOp;

/*
 * The Win32 interop interface (UserConsentVerifierInterop.h in the SDK,
 * absent from the MinGW headers). Documented IID; derives from
 * IInspectable. Its one method is what lets a Win32 process show the
 * prompt at all - the plain WinRT activation path needs a CoreWindow.
 */
DEFINE_GUID(IID_IUserConsentVerifierInterop,
            0x39e050c3, 0x4e74, 0x441a, 0x8d,0xc0, 0xb8,0x11,0x04,0xdf,0x94,0x9c);
typedef struct IUserConsentVerifierInterop IUserConsentVerifierInterop;
typedef struct IUserConsentVerifierInteropVtbl {
    /* IUnknown */
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(
        IUserConsentVerifierInterop *This, REFIID riid, void **ppv);
    ULONG (STDMETHODCALLTYPE *AddRef)(IUserConsentVerifierInterop *This);
    ULONG (STDMETHODCALLTYPE *Release)(IUserConsentVerifierInterop *This);
    /* IInspectable */
    HRESULT (STDMETHODCALLTYPE *GetIids)(
        IUserConsentVerifierInterop *This, ULONG *count, IID **iids);
    HRESULT (STDMETHODCALLTYPE *GetRuntimeClassName)(
        IUserConsentVerifierInterop *This, HSTRING *name);
    HRESULT (STDMETHODCALLTYPE *GetTrustLevel)(
        IUserConsentVerifierInterop *This, TrustLevel *level);
    /* IUserConsentVerifierInterop */
    HRESULT (STDMETHODCALLTYPE *RequestVerificationForWindowAsync)(
        IUserConsentVerifierInterop *This, HWND appWindow, HSTRING message,
        REFIID riid, void **asyncOp);
} IUserConsentVerifierInteropVtbl;
struct IUserConsentVerifierInterop { CONST_VTBL IUserConsentVerifierInteropVtbl *lpVtbl; };

/* combase.dll, loaded once on first use. All-or-nothing: a partial export
 * set is treated as no WinRT at all. */
typedef HRESULT (WINAPI *pRoInitialize_t)(RO_INIT_TYPE);
typedef HRESULT (WINAPI *pRoGetActivationFactory_t)(HSTRING, REFIID, void **);
typedef HRESULT (WINAPI *pWindowsCreateString_t)(PCNZWCH, UINT32, HSTRING *);
typedef HRESULT (WINAPI *pWindowsDeleteString_t)(HSTRING);
static pRoInitialize_t            fnRoInitialize;
static pRoGetActivationFactory_t  fnRoGetActivationFactory;
static pWindowsCreateString_t     fnWindowsCreateString;
static pWindowsDeleteString_t     fnWindowsDeleteString;
static int g_combase_state = 0;   /* 0 untried, 1 loaded, -1 unusable */

#ifndef RPC_E_CHANGED_MODE_LOCAL
#define RPC_E_CHANGED_MODE_LOCAL ((HRESULT)0x80010106L)
#endif

static int hello_combase_load(void)
{
    if (g_combase_state)
        return g_combase_state > 0;
    {
        HMODULE m = LoadLibraryW(L"combase.dll");
        if (m) {
            fnRoInitialize = (pRoInitialize_t)
                GetProcAddress(m, "RoInitialize");
            fnRoGetActivationFactory = (pRoGetActivationFactory_t)
                GetProcAddress(m, "RoGetActivationFactory");
            fnWindowsCreateString = (pWindowsCreateString_t)
                GetProcAddress(m, "WindowsCreateString");
            fnWindowsDeleteString = (pWindowsDeleteString_t)
                GetProcAddress(m, "WindowsDeleteString");
        }
    }
    g_combase_state = (fnRoInitialize && fnRoGetActivationFactory &&
                       fnWindowsCreateString && fnWindowsDeleteString) ? 1 : -1;
    return g_combase_state > 0;
}

/* Get an activation factory, or NULL. Initializes WinRT on this thread;
 * an already-initialized apartment of either flavour is fine (S_FALSE /
 * RPC_E_CHANGED_MODE). */
static void *hello_activation_factory(const WCHAR *cls, size_t clschars,
                                      REFIID iid)
{
    HSTRING hcls;
    void *factory = NULL;
    HRESULT hr;

    if (!hello_combase_load())
        return NULL;
    hr = fnRoInitialize(RO_INIT_SINGLETHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE_LOCAL)
        return NULL;
    if (FAILED(fnWindowsCreateString(cls, (UINT32)clschars, &hcls)))
        return NULL;
    hr = fnRoGetActivationFactory(hcls, iid, &factory);
    fnWindowsDeleteString(hcls);
    return SUCCEEDED(hr) ? factory : NULL;
}

/* The UserConsentVerifier activation factory as IInspectable, or NULL. */
static IInspectable *hello_factory(void)
{
    static const WCHAR cls[] =
        L"Windows.Security.Credentials.UI.UserConsentVerifier";
    return (IInspectable *)hello_activation_factory(
        cls, sizeof(cls)/sizeof(cls[0]) - 1, &IID_IInspectable);
}

/*
 * Diagnostic trace for the hands-on paths (selftest and friends).
 * KITTY_HELLO_DIAG is defined ONLY by the test binary's target; in the
 * shipping apps the calls compile to nothing, so no diagnostic code or
 * format strings reach a release binary.
 */
#ifdef KITTY_HELLO_DIAG
static int g_hello_trace = 0;
void kitty_hello_trace_enable(void)
{
    g_hello_trace = 1;
}
static void hello_trace(const char *fmt, ...)
{
    va_list ap;
    if (!g_hello_trace)
        return;
    fprintf(stderr, "[hello +%lums] ", (unsigned long)GetTickCount());
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}
#else
static void hello_trace(const char *fmt, ...) { (void)fmt; }
#endif

/*
 * Wait for a WinRT async operation while keeping the UI alive. The Hello
 * prompt is system UI, but OUR window owns it, so the thread must pump or
 * the prompt cannot even paint. A hard deadline turns "the system never
 * answered" into a denial rather than a wedged agent; the operation is
 * cancelled so it cannot complete into freed state later. The cancel is
 * issued ONCE and given a bounded grace period: the first version of this
 * loop re-armed the deadline on every expiry, so an operation that never
 * left Started (a wedged broker) was re-cancelled every two seconds for
 * ever and the caller simply hung.
 */
static int hello_wait(IUnknown *op_unknown, DWORD timeout_ms)
{
    IAsyncInfo *info = NULL;
    AsyncStatus st = Started;
    DWORD t0 = GetTickCount();
    int ok = 0;
    bool cancelled = false;

    if (FAILED(IUnknown_QueryInterface(op_unknown, &IID_IAsyncInfo,
                                       (void **)&info)))
        return 0;
    for (;;) {
        MSG msg;
        if (FAILED(IAsyncInfo_get_Status(info, &st))) {
            hello_trace("wait: get_Status failed");
            break;
        }
        if (st != Started) {
            ok = (st == Completed);
            break;
        }
        if (!cancelled && GetTickCount() - t0 > timeout_ms) {
            hello_trace("wait: deadline (%lums) expired, cancelling",
                        (unsigned long)timeout_ms);
            IAsyncInfo_Cancel(info);
            cancelled = true;
        }
        if (cancelled && GetTickCount() - t0 > timeout_ms + 2000) {
            hello_trace("wait: cancel did not settle, giving up");
            break;
        }
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        MsgWaitForMultipleObjects(0, NULL, FALSE, 50, QS_ALLINPUT);
    }
    hello_trace("wait: done after %lums, status %d, %s",
                (unsigned long)(GetTickCount() - t0), (int)st,
                ok ? "completed" : "NOT completed");
    IAsyncInfo_Release(info);
    return ok;
}

int kitty_hello_available(void)
{
    IInspectable *factory = hello_factory();
    HelloStatics *statics = NULL;
    HelloAvailOp *op = NULL;
    int ret = -1;

    if (!factory)
        return -1;
    if (SUCCEEDED(IInspectable_QueryInterface(
            factory,
            &IID___x_ABI_CWindows_CSecurity_CCredentials_CUI_CIUserConsentVerifierStatics,
            (void **)&statics))) {
        if (SUCCEEDED(
                __x_ABI_CWindows_CSecurity_CCredentials_CUI_CIUserConsentVerifierStatics_CheckAvailabilityAsync(
                    statics, &op))) {
            if (hello_wait((IUnknown *)op, 10000)) {
                __x_ABI_CWindows_CSecurity_CCredentials_CUI_CUserConsentVerifierAvailability a;
                if (SUCCEEDED(
                        __FIAsyncOperation_1_UserConsentVerifierAvailability_GetResults(
                            op, &a)))
                    ret = (a == UserConsentVerifierAvailability_Available)
                          ? 1 : 0;
            }
            __FIAsyncOperation_1_UserConsentVerifierAvailability_Release(op);
        }
        __x_ABI_CWindows_CSecurity_CCredentials_CUI_CIUserConsentVerifierStatics_Release(statics);
    }
    IInspectable_Release(factory);
    return ret;
}

/*
 * The consent broker needs a REAL owner window: visible, on this thread,
 * and able to come to the front. Handed a hidden window (the tray window)
 * it degrades badly - measured 2026-08-22 on the first live run: the
 * verified-OK dialog came up detached and would not self-dismiss, and the
 * SECOND request never got UI at all, wedging the broker with the camera
 * held open while the agent sat in the wait loop. So each verification
 * gets a transient host: a tiny tool window at the screen centre, shown
 * without stealing the user's typing, destroyed when the prompt is done.
 */
static HWND hello_host_create(void)
{
    static ATOM cls = 0;
    HWND w;
    RECT rc;
    if (!cls) {
        WNDCLASSA wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = DefWindowProcA;
        wc.hInstance = GetModuleHandleA(NULL);
        wc.lpszClassName = "KiTTYHelloHost";
        cls = RegisterClassA(&wc);
        if (!cls)
            return NULL;
    }
    SystemParametersInfoA(SPI_GETWORKAREA, 0, &rc, 0);
    w = CreateWindowExA(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, "KiTTYHelloHost",
                        "kageant", WS_POPUP,
                        (rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2,
                        1, 1, NULL, NULL, GetModuleHandleA(NULL), NULL);
    if (w) {
        ShowWindow(w, SW_SHOWNOACTIVATE);
        /*
         * Take the foreground for real. A background process asking
         * SetForegroundWindow is normally REFUSED, and the broker then
         * never presents its window - measured 2026-08-22: the prompt
         * appeared only while the mouse hovered the tray icon, because
         * that hover was the user input granting kageant foreground
         * rights. Briefly attaching our input queue to the current
         * foreground thread is the sanctioned way to inherit those
         * rights for one activation.
         */
        {
            HWND fg = GetForegroundWindow();
            DWORD fgt = fg ? GetWindowThreadProcessId(fg, NULL) : 0;
            DWORD us = GetCurrentThreadId();
            BOOL attached = FALSE;
            if (fgt && fgt != us)
                attached = AttachThreadInput(fgt, us, TRUE);
            SetForegroundWindow(w);
            BringWindowToTop(w);
            if (attached)
                AttachThreadInput(fgt, us, FALSE);
        }
    }
    return w;
}

int kitty_hello_verify(HWND owner, const char *message_utf8)
{
    IInspectable *factory = hello_factory();
    IUserConsentVerifierInterop *interop = NULL;
    HelloVerifyOp *op = NULL;
    HSTRING hmsg = NULL;
    HWND host;
    WCHAR *wmsg;
    int wlen;
    int ret = KITTY_HELLO_ERROR;

    (void)owner;   /* see hello_host_create - a hidden owner wedges the broker */
    if (!factory)
        return KITTY_HELLO_UNAVAILABLE;

    wlen = MultiByteToWideChar(CP_UTF8, 0, message_utf8, -1, NULL, 0);
    wmsg = (WCHAR *)LocalAlloc(LMEM_FIXED, (wlen > 0 ? wlen : 1) * sizeof(WCHAR));
    if (!wmsg || wlen <= 0 ||
        !MultiByteToWideChar(CP_UTF8, 0, message_utf8, -1, wmsg, wlen)) {
        if (wmsg) LocalFree(wmsg);
        IInspectable_Release(factory);
        return KITTY_HELLO_ERROR;
    }

    host = hello_host_create();
    if (!host) {
        LocalFree(wmsg);
        IInspectable_Release(factory);
        return KITTY_HELLO_ERROR;
    }

    if (SUCCEEDED(IInspectable_QueryInterface(
            factory, &IID_IUserConsentVerifierInterop, (void **)&interop)) &&
        SUCCEEDED(fnWindowsCreateString(wmsg, (UINT32)(wlen - 1), &hmsg))) {
        HRESULT hr = interop->lpVtbl->RequestVerificationForWindowAsync(
            interop, host, hmsg,
            &IID___FIAsyncOperation_1_UserConsentVerificationResult,
            (void **)&op);
        if (SUCCEEDED(hr) && op) {
            /* Long enough for a human walking back to the desk, short
             * enough that a wedged broker cannot hold the agent (and the
             * camera) hostage. Expiry cancels and reads as a denial. */
            if (hello_wait((IUnknown *)op, 60000)) {
                __x_ABI_CWindows_CSecurity_CCredentials_CUI_CUserConsentVerificationResult r;
                if (SUCCEEDED(
                        __FIAsyncOperation_1_UserConsentVerificationResult_GetResults(
                            op, &r))) {
                    switch (r) {
                      case UserConsentVerificationResult_Verified:
                        ret = KITTY_HELLO_VERIFIED;
                        break;
                      case UserConsentVerificationResult_DeviceNotPresent:
                      case UserConsentVerificationResult_NotConfiguredForUser:
                      case UserConsentVerificationResult_DisabledByPolicy:
                        ret = KITTY_HELLO_UNAVAILABLE;
                        break;
                      case UserConsentVerificationResult_Canceled:
                      case UserConsentVerificationResult_RetriesExhausted:
                        ret = KITTY_HELLO_DENIED;
                        break;
                      default:      /* DeviceBusy and anything newer */
                        ret = KITTY_HELLO_ERROR;
                        break;
                    }
                }
            } else {
                ret = KITTY_HELLO_DENIED;    /* timed out = nobody approved */
            }
            __FIAsyncOperation_1_UserConsentVerificationResult_Release(op);
        }
    }
    if (hmsg) fnWindowsDeleteString(hmsg);
    if (interop) interop->lpVtbl->Release(interop);
    DestroyWindow(host);
    LocalFree(wmsg);
    IInspectable_Release(factory);
    return ret;
}

/* ====================================================================
 * Windows Hello as a key protector: KeyCredentialManager + the wrapped-
 * secret container. See kitty_hello.h for the design summary.
 */

/* Local shorthand for the MinGW-declared statics side. */
typedef __x_ABI_CWindows_CSecurity_CCredentials_CIKeyCredentialManagerStatics
    HelloKeyStatics;
typedef __x_ABI_CWindows_CSecurity_CCredentials_CIKeyCredentialRetrievalResult
    HelloKeyRetrieval;
typedef __FIAsyncOperation_1_Windows__CSecurity__CCredentials__CKeyCredentialRetrievalResult
    HelloKeyOpenOp;
typedef __x_ABI_CWindows_CStorage_CStreams_CIBuffer         HelloBuf;
typedef __x_ABI_CWindows_CStorage_CStreams_CIBufferFactory  HelloBufFactory;
typedef __x_Windows_CStorage_CStreams_CIBufferByteAccess    HelloBufBytes;
typedef __x_ABI_CWindows_CSecurity_CCredentials_CKeyCredentialStatus
    HelloKeyStatus;

/*
 * The KeyCredential instance side. The MinGW header only forward-declares
 * IKeyCredential and lacks IKeyCredentialOperationResult and the sign
 * async operation entirely, so their vtables are declared by hand from
 * the SDK (10.0.26100.0 winrt/windows.security.credentials.h). None of
 * them is ever QueryInterface'd for - each arrives already typed from a
 * documented out-parameter - so no IIDs are needed, but the SLOT ORDER
 * must match the SDK exactly.
 */
typedef struct HelloOpResult HelloOpResult;      /* IKeyCredentialOperationResult */
typedef struct HelloSignOp HelloSignOp;   /* IAsyncOperation<KeyCredentialOperationResult*> */
typedef struct HelloKeyCred HelloKeyCred;        /* IKeyCredential */

typedef struct HelloOpResultVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(HelloOpResult *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(HelloOpResult *);
    ULONG (STDMETHODCALLTYPE *Release)(HelloOpResult *);
    HRESULT (STDMETHODCALLTYPE *GetIids)(HelloOpResult *, ULONG *, IID **);
    HRESULT (STDMETHODCALLTYPE *GetRuntimeClassName)(HelloOpResult *, HSTRING *);
    HRESULT (STDMETHODCALLTYPE *GetTrustLevel)(HelloOpResult *, TrustLevel *);
    HRESULT (STDMETHODCALLTYPE *get_Result)(HelloOpResult *, HelloBuf **);
    HRESULT (STDMETHODCALLTYPE *get_Status)(HelloOpResult *,
                                            HelloKeyStatus *);
} HelloOpResultVtbl;
struct HelloOpResult { CONST_VTBL HelloOpResultVtbl *lpVtbl; };

typedef struct HelloSignOpVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(HelloSignOp *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(HelloSignOp *);
    ULONG (STDMETHODCALLTYPE *Release)(HelloSignOp *);
    HRESULT (STDMETHODCALLTYPE *GetIids)(HelloSignOp *, ULONG *, IID **);
    HRESULT (STDMETHODCALLTYPE *GetRuntimeClassName)(HelloSignOp *, HSTRING *);
    HRESULT (STDMETHODCALLTYPE *GetTrustLevel)(HelloSignOp *, TrustLevel *);
    HRESULT (STDMETHODCALLTYPE *put_Completed)(HelloSignOp *, void *);
    HRESULT (STDMETHODCALLTYPE *get_Completed)(HelloSignOp *, void **);
    HRESULT (STDMETHODCALLTYPE *GetResults)(HelloSignOp *, HelloOpResult **);
} HelloSignOpVtbl;
struct HelloSignOp { CONST_VTBL HelloSignOpVtbl *lpVtbl; };

typedef struct HelloKeyCredVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(HelloKeyCred *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(HelloKeyCred *);
    ULONG (STDMETHODCALLTYPE *Release)(HelloKeyCred *);
    HRESULT (STDMETHODCALLTYPE *GetIids)(HelloKeyCred *, ULONG *, IID **);
    HRESULT (STDMETHODCALLTYPE *GetRuntimeClassName)(HelloKeyCred *, HSTRING *);
    HRESULT (STDMETHODCALLTYPE *GetTrustLevel)(HelloKeyCred *, TrustLevel *);
    HRESULT (STDMETHODCALLTYPE *get_Name)(HelloKeyCred *, HSTRING *);
    HRESULT (STDMETHODCALLTYPE *RetrievePublicKeyWithDefaultBlobType)(
        HelloKeyCred *, HelloBuf **);
    HRESULT (STDMETHODCALLTYPE *RetrievePublicKeyWithBlobType)(
        HelloKeyCred *, INT32 blobType, HelloBuf **);
    HRESULT (STDMETHODCALLTYPE *RequestSignAsync)(
        HelloKeyCred *, HelloBuf *data, HelloSignOp **);
    HRESULT (STDMETHODCALLTYPE *GetAttestationAsync)(HelloKeyCred *, void **);
} HelloKeyCredVtbl;
struct HelloKeyCred { CONST_VTBL HelloKeyCredVtbl *lpVtbl; };

/*
 * One credential for all KiTTY apps, one fixed challenge. Both are
 * VERSIONED BY THE CONTAINER MARKER (HELLOK1): a future challenge or
 * naming change bumps the marker, so an old container meeting a new build
 * is detectable rather than "wrong key, for ever". The credential is RSA
 * (Windows Hello's KeyCredential kind) and its PKCS#1 v1.5 signatures are
 * deterministic, which the whole KEK derivation depends on -
 * kitty_hello_kek_selftest() proves it on a live machine.
 */
static const WCHAR HELLO_CRED_NAME[] = L"kapper.net.KiTTY";
static const char HELLO_KEK_CHALLENGE[] =
    "KiTTY Windows Hello KEK challenge, container format HELLOK1";

/* The KeyCredentialManager statics, or NULL. */
static HelloKeyStatics *hello_key_statics(void)
{
    static const WCHAR cls[] =
        L"Windows.Security.Credentials.KeyCredentialManager";
    return (HelloKeyStatics *)hello_activation_factory(
        cls, sizeof(cls)/sizeof(cls[0]) - 1,
        &IID___x_ABI_CWindows_CSecurity_CCredentials_CIKeyCredentialManagerStatics);
}

/* Make a WinRT IBuffer holding a copy of the given bytes, or NULL. */
static HelloBuf *hello_buffer_from(const void *data, UINT32 len)
{
    static const WCHAR cls[] = L"Windows.Storage.Streams.Buffer";
    HelloBufFactory *bf;
    HelloBuf *buf = NULL;
    HelloBufBytes *bytes = NULL;
    BYTE *p = NULL;

    bf = (HelloBufFactory *)hello_activation_factory(
        cls, sizeof(cls)/sizeof(cls[0]) - 1,
        &IID___x_ABI_CWindows_CStorage_CStreams_CIBufferFactory);
    if (!bf)
        return NULL;
    if (SUCCEEDED(__x_ABI_CWindows_CStorage_CStreams_CIBufferFactory_Create(
            bf, len, &buf)) && buf) {
        if (SUCCEEDED(__x_ABI_CWindows_CStorage_CStreams_CIBuffer_QueryInterface(
                buf, &IID___x_Windows_CStorage_CStreams_CIBufferByteAccess,
                (void **)&bytes)) &&
            SUCCEEDED(bytes->lpVtbl->Buffer(bytes, &p)) && p &&
            SUCCEEDED(__x_ABI_CWindows_CStorage_CStreams_CIBuffer_put_Length(
                buf, len))) {
            memcpy(p, data, len);
        } else {
            __x_ABI_CWindows_CStorage_CStreams_CIBuffer_Release(buf);
            buf = NULL;
        }
        if (bytes)
            bytes->lpVtbl->Release(bytes);
    }
    __x_ABI_CWindows_CStorage_CStreams_CIBufferFactory_Release(bf);
    return buf;
}

/* Read out an IBuffer's bytes (caller sfree). NULL on failure. */
static unsigned char *hello_buffer_bytes(HelloBuf *buf, UINT32 *len_out)
{
    HelloBufBytes *bytes = NULL;
    BYTE *p = NULL;
    UINT32 len = 0;
    unsigned char *copy = NULL;

    if (FAILED(__x_ABI_CWindows_CStorage_CStreams_CIBuffer_get_Length(
            buf, &len)) || len == 0)
        return NULL;
    if (SUCCEEDED(__x_ABI_CWindows_CStorage_CStreams_CIBuffer_QueryInterface(
            buf, &IID___x_Windows_CStorage_CStreams_CIBufferByteAccess,
            (void **)&bytes)) &&
        SUCCEEDED(bytes->lpVtbl->Buffer(bytes, &p)) && p) {
        copy = snewn(len, unsigned char);
        memcpy(copy, p, len);
        *len_out = len;
    }
    if (bytes)
        bytes->lpVtbl->Release(bytes);
    return copy;
}

int kitty_hello_key_available(void)
{
    HelloKeyStatics *statics = hello_key_statics();
    __FIAsyncOperation_1_boolean *op = NULL;
    int ret = -1;

    if (!statics)
        return -1;
    if (SUCCEEDED(
            __x_ABI_CWindows_CSecurity_CCredentials_CIKeyCredentialManagerStatics_IsSupportedAsync(
                statics, &op)) && op) {
        if (hello_wait((IUnknown *)op, 10000)) {
            boolean b = 0;
            if (SUCCEEDED(__FIAsyncOperation_1_boolean_GetResults(op, &b)))
                ret = b ? 1 : 0;
        }
        __FIAsyncOperation_1_boolean_Release(op);
    }
    __x_ABI_CWindows_CSecurity_CCredentials_CIKeyCredentialManagerStatics_Release(statics);
    return ret;
}

static int hello_keystatus_to_ret(HelloKeyStatus st)
{
    switch (st) {
      case KeyCredentialStatus_Success:
        return KITTY_HELLO_VERIFIED;
      case KeyCredentialStatus_UserCanceled:
      case KeyCredentialStatus_UserPrefersPassword:
      case KeyCredentialStatus_SecurityDeviceLocked:
        return KITTY_HELLO_DENIED;
      case KeyCredentialStatus_NotFound:
        return KITTY_HELLO_UNAVAILABLE;
      default:
        return KITTY_HELLO_ERROR;
    }
}

/* Open (or create) the shared credential. On VERIFIED, *cred_out holds a
 * reference. NEVER passes ReplaceExisting: re-creating the TPM key would
 * silently kill every secret already wrapped under it, so an existing-but-
 * unopenable credential is reported, not replaced. */
static int hello_key_open(HelloKeyStatics *statics, int create_if_missing,
                          HelloKeyCred **cred_out)
{
    HSTRING hname = NULL;
    HelloKeyOpenOp *op = NULL;
    int ret = KITTY_HELLO_ERROR;
    bool try_create = false;

    *cred_out = NULL;
    if (FAILED(fnWindowsCreateString(
            HELLO_CRED_NAME,
            (UINT32)(sizeof(HELLO_CRED_NAME)/sizeof(WCHAR) - 1), &hname)))
        return KITTY_HELLO_ERROR;

    hello_trace("open: OpenAsync starting");
    if (SUCCEEDED(
            __x_ABI_CWindows_CSecurity_CCredentials_CIKeyCredentialManagerStatics_OpenAsync(
                statics, hname, &op)) && op) {
        if (hello_wait((IUnknown *)op, 15000)) {
            HelloKeyRetrieval *res = NULL;
            if (SUCCEEDED(
                    __FIAsyncOperation_1_Windows__CSecurity__CCredentials__CKeyCredentialRetrievalResult_GetResults(
                        op, &res)) && res) {
                HelloKeyStatus st;
                if (SUCCEEDED(
                        __x_ABI_CWindows_CSecurity_CCredentials_CIKeyCredentialRetrievalResult_get_Status(
                            res, &st))) {
                    hello_trace("open: status %d", (int)st);
                    ret = hello_keystatus_to_ret(st);
                    if (ret == KITTY_HELLO_VERIFIED)
                        __x_ABI_CWindows_CSecurity_CCredentials_CIKeyCredentialRetrievalResult_get_Credential(
                            res, (__x_ABI_CWindows_CSecurity_CCredentials_CIKeyCredential **)cred_out);
                    if (st == KeyCredentialStatus_NotFound && create_if_missing)
                        try_create = true;
                }
                __x_ABI_CWindows_CSecurity_CCredentials_CIKeyCredentialRetrievalResult_Release(res);
            }
        }
        __FIAsyncOperation_1_Windows__CSecurity__CCredentials__CKeyCredentialRetrievalResult_Release(op);
        op = NULL;
    }

    if (try_create) {
        HRESULT hr;
        hello_trace("open: not found, RequestCreateAsync starting "
                    "(enrolment UI expected)");
        hr = __x_ABI_CWindows_CSecurity_CCredentials_CIKeyCredentialManagerStatics_RequestCreateAsync(
                statics, hname, KeyCredentialCreationOption_FailIfExists,
                &op);
        if (FAILED(hr) || !op) {
            hello_trace("create: call itself failed, hr=0x%08lX",
                        (unsigned long)hr);
            op = NULL;
        }
    }
    if (try_create && op) {
        /* Creation walks the user through a Hello verification; allow for
         * a human on a slow day. */
        if (hello_wait((IUnknown *)op, 120000)) {
            HelloKeyRetrieval *res = NULL;
            if (SUCCEEDED(
                    __FIAsyncOperation_1_Windows__CSecurity__CCredentials__CKeyCredentialRetrievalResult_GetResults(
                        op, &res)) && res) {
                HelloKeyStatus st;
                if (SUCCEEDED(
                        __x_ABI_CWindows_CSecurity_CCredentials_CIKeyCredentialRetrievalResult_get_Status(
                            res, &st))) {
                    hello_trace("create: status %d", (int)st);
                    ret = hello_keystatus_to_ret(st);
                    if (ret == KITTY_HELLO_VERIFIED)
                        __x_ABI_CWindows_CSecurity_CCredentials_CIKeyCredentialRetrievalResult_get_Credential(
                            res, (__x_ABI_CWindows_CSecurity_CCredentials_CIKeyCredential **)cred_out);
                }
                __x_ABI_CWindows_CSecurity_CCredentials_CIKeyCredentialRetrievalResult_Release(res);
            }
        } else {
            /* The op errored/cancelled/timed out. GetResults is where the
             * broker's actual error code lands - harvest it for the trace. */
            HelloKeyRetrieval *res = NULL;
            HRESULT hr =
                __FIAsyncOperation_1_Windows__CSecurity__CCredentials__CKeyCredentialRetrievalResult_GetResults(
                    op, &res);
            hello_trace("create: did not complete, GetResults hr=0x%08lX",
                        (unsigned long)hr);
            if (res)
                __x_ABI_CWindows_CSecurity_CCredentials_CIKeyCredentialRetrievalResult_Release(res);
            ret = KITTY_HELLO_DENIED;   /* nobody (successfully) enrolled */
        }
        __FIAsyncOperation_1_Windows__CSecurity__CCredentials__CKeyCredentialRetrievalResult_Release(op);
    }

    fnWindowsDeleteString(hname);
    if (ret == KITTY_HELLO_VERIFIED && !*cred_out)
        ret = KITTY_HELLO_ERROR;
    return ret;
}

/* Sign the fixed challenge with the credential - one Hello prompt - and
 * hand back the raw signature (caller smemclr+sfree). */
static int hello_key_sign(HelloKeyCred *cred, unsigned char **sig_out,
                          UINT32 *siglen_out)
{
    HelloBuf *challenge;
    HelloSignOp *op = NULL;
    int ret = KITTY_HELLO_ERROR;

    *sig_out = NULL;
    challenge = hello_buffer_from(HELLO_KEK_CHALLENGE,
                                  (UINT32)(sizeof(HELLO_KEK_CHALLENGE) - 1));
    if (!challenge)
        return KITTY_HELLO_ERROR;

    hello_trace("sign: RequestSignAsync starting (Hello prompt expected)");
    if (SUCCEEDED(cred->lpVtbl->RequestSignAsync(cred, challenge, &op)) &&
        op) {
        if (hello_wait((IUnknown *)op, 90000)) {
            HelloOpResult *res = NULL;
            if (SUCCEEDED(op->lpVtbl->GetResults(op, &res)) && res) {
                HelloKeyStatus st;
                if (SUCCEEDED(res->lpVtbl->get_Status(res, &st))) {
                    hello_trace("sign: status %d", (int)st);
                    ret = hello_keystatus_to_ret(st);
                    if (ret == KITTY_HELLO_VERIFIED) {
                        HelloBuf *sig = NULL;
                        ret = KITTY_HELLO_ERROR;
                        if (SUCCEEDED(res->lpVtbl->get_Result(res, &sig)) &&
                            sig) {
                            *sig_out = hello_buffer_bytes(sig, siglen_out);
                            if (*sig_out)
                                ret = KITTY_HELLO_VERIFIED;
                            __x_ABI_CWindows_CStorage_CStreams_CIBuffer_Release(sig);
                        }
                    }
                }
                res->lpVtbl->Release(res);
            }
        } else {
            HelloOpResult *res = NULL;
            HRESULT hr = op->lpVtbl->GetResults(op, &res);
            hello_trace("sign: did not complete, GetResults hr=0x%08lX",
                        (unsigned long)hr);
            if (res)
                res->lpVtbl->Release(res);
            ret = KITTY_HELLO_DENIED;    /* timed out = nobody approved */
        }
        op->lpVtbl->Release(op);
    }
    __x_ABI_CWindows_CStorage_CStreams_CIBuffer_Release(challenge);
    return ret;
}

int kitty_hello_kek(unsigned char kek[32], int create_if_missing)
{
    HelloKeyStatics *statics = hello_key_statics();
    HelloKeyCred *cred = NULL;
    HWND host;
    int ret;

    if (!statics)
        return KITTY_HELLO_UNAVAILABLE;

    /* Create and sign both show Hello UI, which needs the same visible
     * host + foreground treatment as the consent prompt (a hidden owner
     * wedges the broker; a background process never gets the UI at all). */
    host = hello_host_create();
    if (!host) {
        __x_ABI_CWindows_CSecurity_CCredentials_CIKeyCredentialManagerStatics_Release(statics);
        return KITTY_HELLO_ERROR;
    }

    ret = hello_key_open(statics, create_if_missing, &cred);
    if (ret == KITTY_HELLO_VERIFIED) {
        unsigned char *sig = NULL;
        UINT32 siglen = 0;
        ret = hello_key_sign(cred, &sig, &siglen);
        if (ret == KITTY_HELLO_VERIFIED) {
            ssh_hash *h = ssh_hash_new(&ssh_sha256);
            put_data(h, sig, siglen);
            ssh_hash_final(h, kek);
        }
        if (sig) {
            smemclr(sig, siglen);
            sfree(sig);
        }
        cred->lpVtbl->Release(cred);
    }

    DestroyWindow(host);
    __x_ABI_CWindows_CSecurity_CCredentials_CIKeyCredentialManagerStatics_Release(statics);
    return ret;
}

/* ====================================================================
 * Hands-on diagnostics - selftests and the WebAuthn probe modes. Only
 * the test binary defines KITTY_HELLO_DIAG; shipping apps compile none
 * of this. When the WebAuthn-PRF KEK source becomes a product feature,
 * what it needs moves ABOVE this fence rather than the fence moving.
 */
#ifdef KITTY_HELLO_DIAG

int kitty_hello_kek_selftest(char **msg)
{
    return kitty_hello_kek_selftest_ex(0, msg);
}

int kitty_hello_kek_selftest_ex(int nohost, char **msg)
{
    HelloKeyStatics *statics = hello_key_statics();
    HelloKeyCred *cred = NULL;
    HWND host = NULL;
    int ret;

    *msg = NULL;
    if (!statics) {
        *msg = dupstr("WinRT unavailable");
        return KITTY_HELLO_UNAVAILABLE;
    }
    /* The WebAuthn probe proved the transient 1x1 host window is what
     * breaks the CREATION UI (the broker inherits the foreground window,
     * and a synthetic barely-there popup poisons it). nohost skips the
     * trick so the caller's real foreground window rules. */
    if (!nohost) {
        host = hello_host_create();
        if (!host) {
            __x_ABI_CWindows_CSecurity_CCredentials_CIKeyCredentialManagerStatics_Release(statics);
            *msg = dupstr("no host window");
            return KITTY_HELLO_ERROR;
        }
    } else {
        hello_trace("selftest: NO host window - foreground stays as-is");
    }

    /*
     * Step 0: is key-credential support even THERE? Creation is gated far
     * harder than the consent verifier (Hello sign-in PIN required;
     * domain-joined machines additionally need Windows Hello for Business
     * policy; elevation can block it too), and attempting it anyway just
     * gets Windows' generic retrying "something went wrong" dialog. Bail
     * out with a real answer instead.
     */
    {
        int sup = kitty_hello_key_available();
        hello_trace("selftest: IsSupported = %d", sup);
        if (sup == 0) {
            *msg = dupstr("KeyCredentialManager reports NOT SUPPORTED for "
                          "this account: Hello sign-in PIN not enrolled, or "
                          "a domain-joined machine without Windows Hello "
                          "for Business policy, or blocked by policy/"
                          "elevation. Key wrapping cannot work here; "
                          "recovery-wrap-only is the fallback");
            DestroyWindow(host);
            __x_ABI_CWindows_CSecurity_CCredentials_CIKeyCredentialManagerStatics_Release(statics);
            return KITTY_HELLO_UNAVAILABLE;
        }
    }

    hello_trace("selftest: opening credential");
    ret = hello_key_open(statics, true, &cred);
    hello_trace("selftest: open result %d", ret);
    if (ret == KITTY_HELLO_VERIFIED) {
        unsigned char *sig1 = NULL, *sig2 = NULL;
        UINT32 len1 = 0, len2 = 0;
        hello_trace("selftest: first sign");
        ret = hello_key_sign(cred, &sig1, &len1);
        hello_trace("selftest: first sign result %d, %u bytes",
                    ret, (unsigned)len1);
        if (ret == KITTY_HELLO_VERIFIED) {
            hello_trace("selftest: second sign");
            ret = hello_key_sign(cred, &sig2, &len2);
            hello_trace("selftest: second sign result %d, %u bytes",
                        ret, (unsigned)len2);
        }
        if (ret == KITTY_HELLO_VERIFIED) {
            if (len1 == len2 && smemeq(sig1, sig2, len1)) {
                *msg = dupprintf("deterministic: two signatures of %u bytes, "
                                 "byte-identical", (unsigned)len1);
            } else {
                *msg = dupprintf("NOT DETERMINISTIC: %u vs %u bytes, %s - "
                                 "this machine cannot use Hello-wrapped keys",
                                 (unsigned)len1, (unsigned)len2,
                                 len1 == len2 ? "contents differ"
                                              : "lengths differ");
                ret = KITTY_HELLO_DENIED;
            }
        }
        if (sig1) { smemclr(sig1, len1); sfree(sig1); }
        if (sig2) { smemclr(sig2, len2); sfree(sig2); }
        cred->lpVtbl->Release(cred);
    }
    if (!*msg)
        *msg = dupstr(ret == KITTY_HELLO_DENIED ? "denied/cancelled"
                                                : "credential unavailable");

    if (host)
        DestroyWindow(host);
    __x_ABI_CWindows_CSecurity_CCredentials_CIKeyCredentialManagerStatics_Release(statics);
    return ret;
}

#endif /* KITTY_HELLO_DIAG (selftests) */

/* ====================================================================
 * WebAuthn / PRF: the PLATFORM authenticator (the passkey machinery,
 * webauthn.dll) as the second - and preferred - Hello-gated KEK source.
 *
 * Why it exists: KeyCredentialManager app-key creation is gated on
 * Windows Hello for Business for on-prem domain accounts (face verified,
 * then Event 360 "provisioning will not be launched" and the broker's
 * retrying generic failure dialog), while the consumer passkey path has
 * no WHfB gate. The CTAP hmac-secret extension returns a stable 32-byte
 * secret per (credential, salt) behind a Hello verification - exactly a
 * KEK. Live-proven deterministic. Two mechanics are load-bearing: the
 * owner HWND must be a REAL window (the consent broker's transient-host
 * trick BREAKS passkey creation), and PRF must be requested as the
 * "hmac-secret" BOOL extension (the WebAuthn-level bEnablePrf flag is
 * ignored by this platform authenticator; do not gate on bPrfEnabled).
 *
 * The types below are transcribed from SDK 10.0.26100 um/webauthn.h
 * (MinGW ships a stripped API-v3 header without the PRF fields, so it is
 * not included; distinct KHW_ names avoid any clash). webauthn.dll is
 * loaded dynamically: no import, and absence = unavailable. Option
 * structs are declared at the CURRENT layout but passed with the LOWEST
 * dwVersion that carries the fields we use (6): the DLL reads only what
 * the declared version implies.
 */

typedef struct KHW_RP_INFO {
    DWORD dwVersion;                   /* 1 */
    PCWSTR pwszId;
    PCWSTR pwszName;
    PCWSTR pwszIcon;
} KHW_RP_INFO;

typedef struct KHW_USER_INFO {
    DWORD dwVersion;                   /* 1 */
    DWORD cbId;
    PBYTE pbId;
    PCWSTR pwszName;
    PCWSTR pwszIcon;
    PCWSTR pwszDisplayName;
} KHW_USER_INFO;

typedef struct KHW_COSE_PARAM {
    DWORD dwVersion;                   /* 1 */
    LPCWSTR pwszCredentialType;        /* L"public-key" */
    LONG lAlg;                         /* -7 = ES256 */
} KHW_COSE_PARAM;

typedef struct KHW_COSE_PARAMS {
    DWORD cCredentialParameters;
    KHW_COSE_PARAM *pCredentialParameters;
} KHW_COSE_PARAMS;

typedef struct KHW_CLIENT_DATA {
    DWORD dwVersion;                   /* 1 */
    DWORD cbClientDataJSON;
    PBYTE pbClientDataJSON;
    LPCWSTR pwszHashAlgId;             /* L"SHA-256" */
} KHW_CLIENT_DATA;

typedef struct KHW_CREDENTIAL {
    DWORD dwVersion;                   /* 1 */
    DWORD cbId;
    PBYTE pbId;
    PCWSTR pwszCredentialType;
} KHW_CREDENTIAL;

typedef struct KHW_CREDENTIALS {
    DWORD cCredentials;
    KHW_CREDENTIAL *pCredentials;
} KHW_CREDENTIALS;

typedef struct KHW_EXTENSION {
    LPCWSTR pwszExtensionIdentifier;
    DWORD cbExtension;
    PVOID pvExtension;
} KHW_EXTENSION;

typedef struct KHW_EXTENSIONS {
    DWORD cExtensions;
    KHW_EXTENSION *pExtensions;
} KHW_EXTENSIONS;

typedef struct KHW_HMAC_SECRET_SALT {
    DWORD cbFirst;
    PBYTE pbFirst;
    DWORD cbSecond;
    PBYTE pbSecond;
} KHW_HMAC_SECRET_SALT;

typedef struct KHW_CRED_WITH_HMAC_SECRET_SALT {
    DWORD cbCredID;
    PBYTE pbCredID;
    KHW_HMAC_SECRET_SALT *pHmacSecretSalt;
} KHW_CRED_WITH_HMAC_SECRET_SALT;

typedef struct KHW_HMAC_SECRET_SALT_VALUES {
    KHW_HMAC_SECRET_SALT *pGlobalHmacSalt;
    DWORD cCredWithHmacSecretSaltList;
    KHW_CRED_WITH_HMAC_SECRET_SALT *pCredWithHmacSecretSaltList;
} KHW_HMAC_SECRET_SALT_VALUES;

#define KHW_ATTACHMENT_PLATFORM        1
#define KHW_UV_REQUIRED                1
#define KHW_UV_PREFERRED               2
#define KHW_ATTESTATION_NONE           1
#define KHW_HMAC_SECRET_VALUES_FLAG    0x00100000
#define KHW_MAKE_OPTS_VERSION_USED     6    /* through bEnablePrf */
#define KHW_ASSERT_OPTS_VERSION_USED   6    /* through pHmacSecretSaltValues */

typedef struct KHW_MAKE_OPTS {
    DWORD dwVersion;
    DWORD dwTimeoutMilliseconds;
    KHW_CREDENTIALS CredentialList;
    KHW_EXTENSIONS Extensions;
    DWORD dwAuthenticatorAttachment;
    BOOL bRequireResidentKey;
    DWORD dwUserVerificationRequirement;
    DWORD dwAttestationConveyancePreference;
    DWORD dwFlags;
    GUID *pCancellationId;                       /* v2 */
    void *pExcludeCredentialList;                /* v3 */
    DWORD dwEnterpriseAttestation;               /* v4 */
    DWORD dwLargeBlobSupport;
    BOOL bPreferResidentKey;
    BOOL bBrowserInPrivateMode;                  /* v5 */
    BOOL bEnablePrf;                             /* v6 */
    void *pLinkedDevice;                         /* v7 */
    DWORD cbJsonExt;
    PBYTE pbJsonExt;
    KHW_HMAC_SECRET_SALT *pPRFGlobalEval;        /* v8 */
    DWORD cCredentialHints;
    LPCWSTR *ppwszCredentialHints;
    BOOL bThirdPartyPayment;
    PCWSTR pwszRemoteWebOrigin;                  /* v9 */
    DWORD cbPublicKeyCredentialCreationOptionsJSON;
    PBYTE pbPublicKeyCredentialCreationOptionsJSON;
    DWORD cbAuthenticatorId;
    PBYTE pbAuthenticatorId;
} KHW_MAKE_OPTS;

typedef struct KHW_ASSERT_OPTS {
    DWORD dwVersion;
    DWORD dwTimeoutMilliseconds;
    KHW_CREDENTIALS CredentialList;
    KHW_EXTENSIONS Extensions;
    DWORD dwAuthenticatorAttachment;
    DWORD dwUserVerificationRequirement;
    DWORD dwFlags;
    PCWSTR pwszU2fAppId;                         /* v2 */
    BOOL *pbU2fAppId;
    GUID *pCancellationId;                       /* v3 */
    void *pAllowCredentialList;                  /* v4 */
    DWORD dwCredLargeBlobOperation;              /* v5 */
    DWORD cbCredLargeBlob;
    PBYTE pbCredLargeBlob;
    KHW_HMAC_SECRET_SALT_VALUES *pHmacSecretSaltValues;  /* v6 */
    BOOL bBrowserInPrivateMode;
    void *pLinkedDevice;                         /* v7 */
    BOOL bAutoFill;
    DWORD cbJsonExt;
    PBYTE pbJsonExt;
    DWORD cCredentialHints;                      /* v8 */
    LPCWSTR *ppwszCredentialHints;
    PCWSTR pwszRemoteWebOrigin;                  /* v9 */
    DWORD cbPublicKeyCredentialRequestOptionsJSON;
    PBYTE pbPublicKeyCredentialRequestOptionsJSON;
    DWORD cbAuthenticatorId;
    PBYTE pbAuthenticatorId;
} KHW_ASSERT_OPTS;

typedef struct KHW_ATTESTATION {          /* output; read-only for us */
    DWORD dwVersion;
    PCWSTR pwszFormatType;
    DWORD cbAuthenticatorData;
    PBYTE pbAuthenticatorData;
    DWORD cbAttestation;
    PBYTE pbAttestation;
    DWORD dwAttestationDecodeType;
    PVOID pvAttestationDecode;
    DWORD cbAttestationObject;
    PBYTE pbAttestationObject;
    DWORD cbCredentialId;
    PBYTE pbCredentialId;
    KHW_EXTENSIONS Extensions;                   /* v2 */
    DWORD dwUsedTransport;                       /* v3 */
    BOOL bEpAtt;                                 /* v4 */
    BOOL bLargeBlobSupported;
    BOOL bResidentKey;
    BOOL bPrfEnabled;                            /* v5 */
    /* v6+: not read by us */
} KHW_ATTESTATION;

typedef struct KHW_ASSERTION {            /* output; read-only for us */
    DWORD dwVersion;
    DWORD cbAuthenticatorData;
    PBYTE pbAuthenticatorData;
    DWORD cbSignature;
    PBYTE pbSignature;
    KHW_CREDENTIAL Credential;
    DWORD cbUserId;
    PBYTE pbUserId;
    KHW_EXTENSIONS Extensions;                   /* v2 */
    DWORD cbCredLargeBlob;
    PBYTE pbCredLargeBlob;
    DWORD dwCredLargeBlobStatus;
    KHW_HMAC_SECRET_SALT *pHmacSecret;           /* v3 */
    /* v4+: not read by us */
} KHW_ASSERTION;

typedef struct KHW_GET_CREDS_OPTS {
    DWORD dwVersion;                   /* 1 */
    PCWSTR pwszRpId;                   /* NULL = all */
    BOOL bBrowserInPrivateMode;
} KHW_GET_CREDS_OPTS;

typedef struct KHW_CRED_DETAILS {     /* output; read-only for us */
    DWORD dwVersion;
    DWORD cbCredentialID;
    PBYTE pbCredentialID;
    KHW_RP_INFO *pRpInformation;
    KHW_USER_INFO *pUserInformation;
    BOOL bRemovable;
    BOOL bBackedUp;                    /* v2 */
    PCWSTR pwszAuthenticatorName;      /* v3 */
    DWORD cbAuthenticatorLogo;
    PBYTE pbAuthenticatorLogo;
    BOOL bThirdPartyPayment;
    DWORD dwTransports;                /* v4 */
} KHW_CRED_DETAILS;

typedef struct KHW_CRED_DETAILS_LIST {
    DWORD cCredentialDetails;
    KHW_CRED_DETAILS **ppCredentialDetails;
} KHW_CRED_DETAILS_LIST;

typedef DWORD (WINAPI *pWebAuthNGetApiVersionNumber_t)(void);
typedef HRESULT (WINAPI *pWebAuthNMakeCredential_t)(
    HWND, const KHW_RP_INFO *, const KHW_USER_INFO *,
    const KHW_COSE_PARAMS *, const KHW_CLIENT_DATA *,
    const KHW_MAKE_OPTS *, KHW_ATTESTATION **);
typedef HRESULT (WINAPI *pWebAuthNGetAssertion_t)(
    HWND, LPCWSTR, const KHW_CLIENT_DATA *, const KHW_ASSERT_OPTS *,
    KHW_ASSERTION **);
typedef void (WINAPI *pWebAuthNFreeAttestation_t)(KHW_ATTESTATION *);
typedef void (WINAPI *pWebAuthNFreeAssertion_t)(KHW_ASSERTION *);
typedef HRESULT (WINAPI *pWebAuthNDeleteCred_t)(DWORD, const BYTE *);
typedef PCWSTR (WINAPI *pWebAuthNGetErrorName_t)(HRESULT);

static const WCHAR KHW_RP_ID[] = KITTY_HELLO_RP_ID;

/* One Hello-gated PRF assertion for the given raw 32-byte salt against
 * the given credential id; secret_out gets the 32-byte result. */
static HRESULT khw_prf_assert(pWebAuthNGetAssertion_t fnGetAssertion,
                              pWebAuthNFreeAssertion_t fnFreeAssertion,
                              HWND host, const WCHAR *rp_id, DWORD uv,
                              BYTE *credid, DWORD credidlen,
                              const unsigned char salt[32],
                              unsigned char secret_out[32])
{
    static const char cdata[] =
        "{\"type\":\"webauthn.get\",\"challenge\":\"a2l0dHktcHJvYmU\","
        "\"origin\":\"https://kitty.kapper.net\"}";
    KHW_CLIENT_DATA cd;
    KHW_CREDENTIAL cred;
    KHW_HMAC_SECRET_SALT gsalt;
    KHW_HMAC_SECRET_SALT_VALUES saltvals;
    KHW_ASSERT_OPTS opts;
    KHW_ASSERTION *asrt = NULL;
    HRESULT hr;

    memset(&cd, 0, sizeof(cd));
    cd.dwVersion = 1;
    cd.cbClientDataJSON = (DWORD)(sizeof(cdata) - 1);
    cd.pbClientDataJSON = (PBYTE)cdata;
    cd.pwszHashAlgId = L"SHA-256";

    memset(&cred, 0, sizeof(cred));
    cred.dwVersion = 1;
    cred.cbId = credidlen;
    cred.pbId = credid;
    cred.pwszCredentialType = L"public-key";

    memset(&gsalt, 0, sizeof(gsalt));
    gsalt.cbFirst = 32;
    gsalt.pbFirst = (PBYTE)salt;

    memset(&saltvals, 0, sizeof(saltvals));
    saltvals.pGlobalHmacSalt = &gsalt;

    memset(&opts, 0, sizeof(opts));
    opts.dwVersion = KHW_ASSERT_OPTS_VERSION_USED;
    opts.dwTimeoutMilliseconds = 120000;
    opts.CredentialList.cCredentials = 1;
    opts.CredentialList.pCredentials = &cred;
    opts.dwAuthenticatorAttachment = KHW_ATTACHMENT_PLATFORM;
    opts.dwUserVerificationRequirement = uv;
    opts.dwFlags = KHW_HMAC_SECRET_VALUES_FLAG;   /* salts are RAW */
    opts.pHmacSecretSaltValues = &saltvals;

    hr = fnGetAssertion(host, rp_id, &cd, &opts, &asrt);
    if (SUCCEEDED(hr) && asrt) {
        if (asrt->dwVersion >= 3 && asrt->pHmacSecret &&
            asrt->pHmacSecret->cbFirst == 32 && asrt->pHmacSecret->pbFirst) {
            memcpy(secret_out, asrt->pHmacSecret->pbFirst, 32);
        } else {
            hello_trace("prf assert: no hmac-secret in assertion "
                        "(version %lu)", (unsigned long)asrt->dwVersion);
            hr = E_FAIL;
        }
        fnFreeAssertion(asrt);
    }
    return hr;
}

/* The webauthn.dll product surface, loaded once. apiver >= 6 is the
 * PRF-capable API generation; anything less = unavailable. */
typedef HRESULT (WINAPI *pWebAuthNIsUVPAA_t)(BOOL *);
typedef HRESULT (WINAPI *pWebAuthNGetList_t)(const KHW_GET_CREDS_OPTS *,
                                             KHW_CRED_DETAILS_LIST **);
typedef void (WINAPI *pWebAuthNFreeList_t)(KHW_CRED_DETAILS_LIST *);
typedef struct khw_api {
    int state;                       /* 0 untried, 1 ok, -1 unusable */
    DWORD apiver;
    pWebAuthNMakeCredential_t MakeCredential;
    pWebAuthNGetAssertion_t GetAssertion;
    pWebAuthNFreeAttestation_t FreeAttestation;
    pWebAuthNFreeAssertion_t FreeAssertion;
    pWebAuthNIsUVPAA_t IsUVPAA;
    pWebAuthNGetList_t GetList;              /* optional (apiver 4+) */
    pWebAuthNFreeList_t FreeList;
    pWebAuthNDeleteCred_t DeleteCred;        /* optional */
    pWebAuthNGetErrorName_t ErrorName;       /* optional */
} khw_api;

static khw_api *khw_load(void)
{
    static khw_api api;
    if (api.state == 0) {
        HMODULE dll = LoadLibraryW(L"webauthn.dll");
        pWebAuthNGetApiVersionNumber_t ver = NULL;
        if (dll) {
            ver = (pWebAuthNGetApiVersionNumber_t)
                GetProcAddress(dll, "WebAuthNGetApiVersionNumber");
            api.MakeCredential = (pWebAuthNMakeCredential_t)
                GetProcAddress(dll, "WebAuthNAuthenticatorMakeCredential");
            api.GetAssertion = (pWebAuthNGetAssertion_t)
                GetProcAddress(dll, "WebAuthNAuthenticatorGetAssertion");
            api.FreeAttestation = (pWebAuthNFreeAttestation_t)
                GetProcAddress(dll, "WebAuthNFreeCredentialAttestation");
            api.FreeAssertion = (pWebAuthNFreeAssertion_t)
                GetProcAddress(dll, "WebAuthNFreeAssertion");
            api.IsUVPAA = (pWebAuthNIsUVPAA_t)
                GetProcAddress(dll,
                    "WebAuthNIsUserVerifyingPlatformAuthenticatorAvailable");
            api.GetList = (pWebAuthNGetList_t)
                GetProcAddress(dll, "WebAuthNGetPlatformCredentialList");
            api.FreeList = (pWebAuthNFreeList_t)
                GetProcAddress(dll, "WebAuthNFreePlatformCredentialList");
            api.DeleteCred = (pWebAuthNDeleteCred_t)
                GetProcAddress(dll, "WebAuthNDeletePlatformCredential");
            api.ErrorName = (pWebAuthNGetErrorName_t)
                GetProcAddress(dll, "WebAuthNGetErrorName");
        }
        api.apiver = ver ? ver() : 0;
        api.state = (api.MakeCredential && api.GetAssertion &&
                     api.FreeAttestation && api.FreeAssertion &&
                     api.IsUVPAA && api.apiver >= 6) ? 1 : -1;
    }
    return api.state > 0 ? &api : NULL;
}

/* The fixed PRF evaluation salt - versioned by the container marker like
 * the KCM challenge: a future salt change bumps HELLOK1, so old
 * containers are detectably old rather than silently unopenable. */
static const unsigned char KHW_KEK_SALT[32] = {
    'K','i','T','T','Y',' ','P','R','F',' ','K','E','K',' ','s','a',
    'l','t',' ','v','1',0,0,0,0,0,0,0,0,0,0,0
};

int kitty_hello_prf_available(void)
{
    khw_api *api = khw_load();
    BOOL avail = FALSE;
    if (!api)
        return 0;
    if (FAILED(api->IsUVPAA(&avail)))
        return -1;
    return avail ? 1 : 0;
}

/* Find our credential's id in the platform store (no UI), or report it
 * absent. 1 = found (caller sfree), 0 = absent, -1 = cannot tell. */
/* What the last PRF operation did, for the apps' audit lines: stage and
 * HRESULT, no secrets. Product-level (not diag) because "denied" alone
 * cannot tell a refused face from a missing credential. */
static char khw_detail[128] = "";
static void khw_set_detail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(khw_detail, sizeof(khw_detail), fmt, ap);
    va_end(ap);
}
const char *kitty_hello_last_detail(void)
{
    return khw_detail;
}

static int khw_find_credential(khw_api *api, unsigned char **credid_out,
                               size_t *credidlen_out)
{
    KHW_GET_CREDS_OPTS opts;
    KHW_CRED_DETAILS_LIST *list = NULL;
    HRESULT hr;
    int ret = 0;

    *credid_out = NULL;
    if (!api->GetList || !api->FreeList)
        return -1;
    memset(&opts, 0, sizeof(opts));
    opts.dwVersion = 1;
    opts.pwszRpId = KHW_RP_ID;
    hr = api->GetList(&opts, &list);
    if (hr == (HRESULT)0x80090011 /* NTE_NOT_FOUND */)
        return 0;
    if (FAILED(hr))
        return -1;
    if (list) {
        DWORD i;
        for (i = 0; i < list->cCredentialDetails && !*credid_out; i++) {
            KHW_CRED_DETAILS *d = list->ppCredentialDetails[i];
            if (d && d->cbCredentialID > 0) {
                *credid_out = snewn(d->cbCredentialID, unsigned char);
                memcpy(*credid_out, d->pbCredentialID, d->cbCredentialID);
                *credidlen_out = d->cbCredentialID;
                ret = 1;
            }
        }
        api->FreeList(list);
    }
    return ret;
}

/*
 * Every WebAuthn call runs on a dedicated worker thread while the calling
 * (UI) thread keeps pumping messages. The live-proven case - the test
 * binary - calls from a plain thread that owns no windows and runs no
 * dialog loop; the agent's UI thread, calling from inside a modal
 * dialog with its key list disabled, got NTE_DEVICE_NOT_FOUND from the
 * SAME assertion three times (2026-08-23), whatever owner window it
 * passed. The owner handed to the platform is the real foreground
 * window captured on the UI thread, exactly as the passing test does.
 */
struct khw_job {
    void (*fn)(void *arg);
    void *arg;
};

static DWORD WINAPI khw_job_thread(LPVOID p)
{
    struct khw_job *j = (struct khw_job *)p;
    j->fn(j->arg);
    return 0;
}

static bool khw_busy = false;

static bool khw_run_on_thread(void (*fn)(void *), void *arg, HWND caller)
{
    struct khw_job j;
    HANDLE h;
    bool disabled = false;
    if (khw_busy)
        return false;          /* no re-entry from the pumped messages */
    j.fn = fn;
    j.arg = arg;
    h = CreateThread(NULL, 0, khw_job_thread, &j, 0, NULL);
    if (!h)
        return false;
    khw_busy = true;
    /* The pump below dispatches to the caller's windows while their own
     * modal loop is suspended underneath - their buttons would be live
     * mid-operation. Disable the caller for the duration, as a modal
     * dialog would. */
    if (caller && IsWindow(caller) && IsWindowEnabled(caller)) {
        EnableWindow(caller, FALSE);
        disabled = true;
    }
    /* This nested wait must serve everything the app's REAL main loop
     * serves - window messages, the toplevel callbacks AND the handle
     * waits (named-pipe accepts arrive as handle events). Pumping only
     * window messages left the agent's pipe refusing clients (one busy
     * instance, never turned over) for as long as a Hello prompt was
     * open: every ssh/git call hung. Measured 2026-08-23:
     * ERROR_PIPE_BUSY at 2/6/15 s into an unanswered startup prompt. */
    for (;;) {
        HandleWaitList *hwl = get_handle_wait_list();
        HANDLE waits[MAXIMUM_WAIT_OBJECTS];
        int nw = hwl->nhandles;
        DWORD w;
        MSG msg;
        bool worker_done;
        memcpy(waits, hwl->handles, nw * sizeof(HANDLE));
        waits[nw] = h;
        w = MsgWaitForMultipleObjects(nw + 1, waits, FALSE,
                                      toplevel_callback_pending() ? 0
                                                                  : INFINITE,
                                      QS_ALLINPUT);
        if ((unsigned)(w - WAIT_OBJECT_0) < (unsigned)nw)
            handle_wait_activate(hwl, w - WAIT_OBJECT_0);
        worker_done = (w == WAIT_OBJECT_0 + (DWORD)nw);
        handle_wait_list_free(hwl);
        if (worker_done)
            break;
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        run_toplevel_callbacks();
    }
    if (disabled)
        EnableWindow(caller, TRUE);
    khw_busy = false;
    CloseHandle(h);
    return true;
}

/*
 * The window the platform's credential UI attaches to. The UI presents
 * reliably only on a real, visible window holding the FOREGROUND -
 * measured 2026-08-24: with an arbitrary foreground window of another
 * process the NGC prompt sometimes never presents at all (camera on, no
 * UI, until the 120 s timeout). So each call gets a small visible host
 * window of our own, granted the foreground by briefly attaching to the
 * current foreground thread's input queue (the consent host's proven
 * trick). Destroyed when the call returns.
 */
/*
 * The anchor is drawn as a current-Windows card: borderless, rounded
 * (DWMWA_WINDOW_CORNER_PREFERENCE), acrylic behind it
 * (DWMWA_SYSTEMBACKDROP_TYPE = transient), dark-mode aware, Segoe UI
 * type with an accent-coloured Hello glyph and an animated status line.
 * Everything newer than base Win32 is loaded dynamically and skipped
 * where absent - a machine with Windows Hello has all of it anyway.
 * Text is drawn with DrawThemeTextEx(DTT_COMPOSITED): plain GDI text
 * would punch alpha holes into the DWM-composed surface.
 */
#include <dwmapi.h>
#include <uxtheme.h>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#endif
#define KHW_DWMWCP_ROUND 2
#define KHW_DWMSBT_TRANSIENT 3

typedef HRESULT (WINAPI *pDwmSetAttr_t)(HWND, DWORD, LPCVOID, DWORD);
typedef HRESULT (WINAPI *pDwmExtend_t)(HWND, const MARGINS *);
typedef HRESULT (WINAPI *pDwmColor_t)(DWORD *, BOOL *);
typedef HTHEME (WINAPI *pOpenTheme_t)(HWND, LPCWSTR);
typedef HRESULT (WINAPI *pCloseTheme_t)(HTHEME);
typedef HRESULT (WINAPI *pDrawThemeTextEx_t)(HTHEME, HDC, int, int, LPCWSTR,
                                             int, DWORD, RECT *,
                                             const DTTOPTS *);
typedef UINT (WINAPI *pGetDpiForWindow_t)(HWND);

static struct {
    int loaded;
    pDwmSetAttr_t SetAttr;
    pDwmExtend_t Extend;
    pDwmColor_t Color;
    pOpenTheme_t OpenTheme;
    pCloseTheme_t CloseTheme;
    pDrawThemeTextEx_t DrawTextEx;
    pGetDpiForWindow_t DpiForWindow;
} khw_ui;

static void khw_ui_load(void)
{
    if (khw_ui.loaded)
        return;
    khw_ui.loaded = 1;
    {
        HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
        if (dwm) {
            khw_ui.SetAttr = (pDwmSetAttr_t)
                GetProcAddress(dwm, "DwmSetWindowAttribute");
            khw_ui.Extend = (pDwmExtend_t)
                GetProcAddress(dwm, "DwmExtendFrameIntoClientArea");
            khw_ui.Color = (pDwmColor_t)
                GetProcAddress(dwm, "DwmGetColorizationColor");
        }
    }
    {
        HMODULE ux = LoadLibraryW(L"uxtheme.dll");
        if (ux) {
            khw_ui.OpenTheme = (pOpenTheme_t)
                GetProcAddress(ux, "OpenThemeData");
            khw_ui.CloseTheme = (pCloseTheme_t)
                GetProcAddress(ux, "CloseThemeData");
            khw_ui.DrawTextEx = (pDrawThemeTextEx_t)
                GetProcAddress(ux, "DrawThemeTextEx");
        }
    }
    khw_ui.DpiForWindow = (pGetDpiForWindow_t)
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
}

/*
 * An anchor window on its OWN pumping thread: credential CREATION must
 * run on the calling thread (a worker gets RPC_E_WRONG_THREAD from the
 * platform), and while that thread blocks inside the call, the owner
 * window still has to live on a thread that answers messages - the
 * proven shape (a console window's conhost) reproduced in-process.
 */
static HWND khw_host_create(void);
static void khw_host_destroy(HWND w);

/*
 * The context line: WHICH window/session is asking. Set (one-shot,
 * consumed by the next Hello operation) by an app that can have several
 * instances - the terminal - so the card identifies the asker. While a
 * context is set the card is ALWAYS shown, and it opens over the asking
 * window instead of the screen centre.
 */
static char khw_context[220];
static HWND khw_context_near;

void kitty_hello_set_context(const char *line, HWND near_window)
{
    if (line)
        snprintf(khw_context, sizeof(khw_context), "%s", line);
    else
        khw_context[0] = '\0';
    khw_context_near = near_window;
}

struct khw_anchor {
    HANDLE ready;              /* window created (or failed) */
    HANDLE thread;
    HWND hwnd;                 /* NULL = creation failed */
    DWORD tid;
};

static DWORD WINAPI khw_anchor_thread(LPVOID p)
{
    struct khw_anchor *a = (struct khw_anchor *)p;
    MSG msg;
    a->hwnd = khw_host_create();
    SetEvent(a->ready);
    if (!a->hwnd)
        return 0;
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        if (msg.message == WM_QUIT)
            break;
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    khw_host_destroy(a->hwnd);
    return 0;
}

static void khw_anchor_start(struct khw_anchor *a)
{
    memset(a, 0, sizeof(*a));
    a->ready = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!a->ready)
        return;
    a->thread = CreateThread(NULL, 0, khw_anchor_thread, a, 0, &a->tid);
    if (a->thread)
        WaitForSingleObject(a->ready, 10000);
}

static void khw_anchor_stop(struct khw_anchor *a)
{
    if (a->thread) {
        PostThreadMessage(a->tid, WM_QUIT, 0, 0);
        WaitForSingleObject(a->thread, 5000);
        CloseHandle(a->thread);
    }
    if (a->ready)
        CloseHandle(a->ready);
}

/* Animated ellipsis state, one host at a time (khw_busy serialises). */
static int khw_host_phase;

static void khw_host_paint(HWND w)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(w, &ps);
    RECT rc;
    GetClientRect(w, &rc);
    {
        UINT dpi = khw_ui.DpiForWindow ? khw_ui.DpiForWindow(w) : 96;
        int px = (int)dpi;   /* scale helper: (v * px / 96) */
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HBITMAP oldbmp = (HBITMAP)SelectObject(mem, bmp);
        HTHEME th = khw_ui.OpenTheme ? khw_ui.OpenTheme(w, L"TEXTSTYLE")
                                     : NULL;

        /* Black = fully transparent to the acrylic backdrop. */
        {
            HBRUSH b = (HBRUSH)GetStockObject(BLACK_BRUSH);
            FillRect(mem, &rc, b);
        }

        /* Accent-coloured Hello glyph (fingerprint, Segoe Fluent/MDL2). */
        {
            DWORD argb = 0;
            BOOL opaque = FALSE;
            COLORREF accent = RGB(96, 205, 255);
            if (khw_ui.Color && SUCCEEDED(khw_ui.Color(&argb, &opaque)))
                accent = RGB((argb >> 16) & 0xff, (argb >> 8) & 0xff,
                             argb & 0xff);
            if (th && khw_ui.DrawTextEx) {
                static const WCHAR glyph[] = { 0xE928, 0 };
                HFONT f = CreateFontW(-(36 * px / 96), 0, 0, 0, FW_NORMAL,
                                      FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                      CLEARTYPE_QUALITY, DEFAULT_PITCH,
                                      L"Segoe Fluent Icons");
                HFONT of = (HFONT)SelectObject(mem, f);
                DTTOPTS o;
                RECT gr = rc;
                memset(&o, 0, sizeof(o));
                o.dwSize = sizeof(o);
                o.dwFlags = DTT_COMPOSITED | DTT_TEXTCOLOR;
                o.crText = accent;
                gr.left = 22 * px / 96;
                gr.top = 20 * px / 96;
                khw_ui.DrawTextEx(th, mem, 0, 0, glyph, -1,
                                  DT_LEFT | DT_TOP | DT_SINGLELINE, &gr, &o);
                SelectObject(mem, of);
                DeleteObject(f);
            }
        }

        /* Title + animated status line. */
        if (th && khw_ui.DrawTextEx) {
            static const WCHAR *dots[] = { L"", L".", L"..", L"..." };
            WCHAR status[64];
            DTTOPTS o;
            RECT tr = rc;
            HFONT f1 = CreateFontW(-(15 * px / 96), 0, 0, 0, FW_SEMIBOLD,
                                   FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                   OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   CLEARTYPE_QUALITY, DEFAULT_PITCH,
                                   L"Segoe UI Variable Display");
            HFONT f2 = CreateFontW(-(12 * px / 96), 0, 0, 0, FW_NORMAL,
                                   FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                   OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   CLEARTYPE_QUALITY, DEFAULT_PITCH,
                                   L"Segoe UI");
            HFONT of = (HFONT)SelectObject(mem, f1);
            memset(&o, 0, sizeof(o));
            o.dwSize = sizeof(o);
            o.dwFlags = DTT_COMPOSITED | DTT_TEXTCOLOR;
            o.crText = RGB(255, 255, 255);
            tr.left = 76 * px / 96;
            tr.top = 22 * px / 96;
            khw_ui.DrawTextEx(th, mem, 0, 0, L"Windows Hello", -1,
                              DT_LEFT | DT_TOP | DT_SINGLELINE, &tr, &o);
            SelectObject(mem, f2);
            o.crText = RGB(190, 190, 190);
            tr.top = 48 * px / 96;
            wsprintfW(status, L"Authentication processing%s",
                      dots[khw_host_phase & 3]);
            khw_ui.DrawTextEx(th, mem, 0, 0, status, -1,
                              DT_LEFT | DT_TOP | DT_SINGLELINE, &tr, &o);
            if (khw_context[0]) {
                WCHAR wctx[220];
                MultiByteToWideChar(CP_ACP, 0, khw_context, -1, wctx, 220);
                o.crText = RGB(150, 150, 150);
                tr.top = 66 * px / 96;
                khw_ui.DrawTextEx(th, mem, 0, 0, wctx, -1,
                                  DT_LEFT | DT_TOP | DT_SINGLELINE |
                                  DT_END_ELLIPSIS, &tr, &o);
            }
            SelectObject(mem, of);
            DeleteObject(f1);
            DeleteObject(f2);
        }

        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        if (th && khw_ui.CloseTheme)
            khw_ui.CloseTheme(th);
        SelectObject(mem, oldbmp);
        DeleteObject(bmp);
        DeleteDC(mem);
    }
    EndPaint(w, &ps);
}

static LRESULT CALLBACK khw_host_wndproc(HWND w, UINT msg, WPARAM wp,
                                         LPARAM lp)
{
    switch (msg) {
      case WM_PAINT:
        khw_host_paint(w);
        return 0;
      case WM_TIMER:
        khw_host_phase++;
        InvalidateRect(w, NULL, FALSE);
        return 0;
      case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

static HWND khw_host_create(void)
{
    static ATOM cls = 0;
    HWND w;
    RECT rc;
    UINT dpi;
    int cx, cy;

    khw_ui_load();
    if (!cls) {
        WNDCLASSW wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = khw_host_wndproc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.lpszClassName = L"KiTTYHelloKeyHost";
        cls = RegisterClassW(&wc);
        if (!cls)
            return NULL;
    }
    SystemParametersInfoA(SPI_GETWORKAREA, 0, &rc, 0);
    if (khw_context_near && IsWindow(khw_context_near) &&
        IsWindowVisible(khw_context_near)) {
        RECT nr;
        if (GetWindowRect(khw_context_near, &nr) &&
            nr.right > nr.left && nr.bottom > nr.top) {
            /* Open over the ASKING window - with several instances the
             * card must say and show where the question comes from. */
            rc = nr;
        }
    }
    dpi = 96;
    cx = 340;
    cy = khw_context[0] ? 112 : 96;
    w = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"KiTTYHelloKeyHost",
                        L"Windows Hello", WS_POPUP,
                        (rc.left + rc.right - cx) / 2,
                        (rc.top + rc.bottom - cy) / 2, cx, cy,
                        NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (!w)
        return NULL;
    if (khw_ui.DpiForWindow) {
        dpi = khw_ui.DpiForWindow(w);
        if (dpi != 96) {
            cx = cx * (int)dpi / 96;
            cy = cy * (int)dpi / 96;
            SetWindowPos(w, NULL, (rc.left + rc.right - cx) / 2,
                         (rc.top + rc.bottom - cy) / 2, cx, cy,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
    if (khw_ui.SetAttr) {
        BOOL dark = TRUE;
        DWORD corner = KHW_DWMWCP_ROUND;
        DWORD backdrop = KHW_DWMSBT_TRANSIENT;
        khw_ui.SetAttr(w, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark,
                       sizeof(dark));
        khw_ui.SetAttr(w, DWMWA_WINDOW_CORNER_PREFERENCE, &corner,
                       sizeof(corner));
        khw_ui.SetAttr(w, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop,
                       sizeof(backdrop));
    }
    if (khw_ui.Extend) {
        MARGINS m = { -1, -1, -1, -1 };
        khw_ui.Extend(w, &m);
    }
    khw_host_phase = 0;
    SetTimer(w, 1, 400, NULL);
    ShowWindow(w, SW_SHOWNOACTIVATE);
    {
        HWND fg = GetForegroundWindow();
        DWORD fgt = fg ? GetWindowThreadProcessId(fg, NULL) : 0;
        DWORD us = GetCurrentThreadId();
        BOOL attached = FALSE;
        if (fgt && fgt != us)
            attached = AttachThreadInput(fgt, us, TRUE);
        SetForegroundWindow(w);
        BringWindowToTop(w);
        if (attached)
            AttachThreadInput(fgt, us, FALSE);
        UpdateWindow(w);
    }
    return w;
}

static void khw_host_destroy(HWND w)
{
    if (w && IsWindow(w))
        DestroyWindow(w);
    /* one-shot: the context belongs to the operation that just ended */
    khw_context[0] = '\0';
    khw_context_near = NULL;
}

/* Is the caller's window good enough to anchor the credential UI - a
 * visible window of THIS process? Then no extra window appears (the
 * interactive flows: a dialog or main window is right there). The
 * anchor host is only for the windowless moments - agent startup,
 * tray-only unlocks - where it is also the visible sign of WHO is
 * asking for the verification. */
static bool khw_owner_usable(HWND w)
{
    DWORD pid = 0;
    if (khw_context[0])
        return false;    /* a context is set: ALWAYS show the card - the
                          * identification is its purpose */
    return w && IsWindow(w) && IsWindowVisible(w) &&
           GetWindowThreadProcessId(w, &pid) &&
           pid == GetCurrentProcessId();
}

struct khw_assert_job {
    khw_api *api;
    HWND owner;
    const unsigned char *credid;
    size_t credidlen;
    unsigned char *out;
    HRESULT hr;
};

static void khw_assert_job_run(void *arg)
{
    struct khw_assert_job *a = (struct khw_assert_job *)arg;
    a->hr = khw_prf_assert(a->api->GetAssertion, a->api->FreeAssertion,
                           a->owner, KHW_RP_ID, KHW_UV_REQUIRED,
                           (BYTE *)a->credid, (DWORD)a->credidlen,
                           KHW_KEK_SALT, a->out);
}

/*
 * Get the id of the KiTTY PRF credential, creating it (one Hello prompt
 * plus Windows' save flow) only when absent AND asked to. The
 * find-first order is load-bearing: MakeCredential with the same RP and
 * user id REPLACES an existing credential, which would orphan every
 * secret already wrapped under it - the same data-loss trap as
 * ReplaceExisting on the KCM side, avoided the same way.
 * owner must be a REAL window (the caller's); a synthetic host window
 * breaks the save flow. Returns a KITTY_HELLO_* code; on VERIFIED,
 * credid_out is filled (caller sfree).
 */
int kitty_hello_prf_credential(HWND owner, int create_if_missing,
                               unsigned char **credid_out,
                               size_t *credidlen_out)
{
    khw_api *api = khw_load();
    static const char cdata[] =
        "{\"type\":\"webauthn.create\",\"challenge\":\"a2l0dHkta2V5cw\","
        "\"origin\":\"https://kapper.net\"}";
    KHW_RP_INFO rp;
    KHW_USER_INFO user;
    KHW_COSE_PARAM alg;
    KHW_COSE_PARAMS algs;
    KHW_CLIENT_DATA cd;
    KHW_MAKE_OPTS mkopts;
    KHW_ATTESTATION *att = NULL;
    static BOOL hmac_on = TRUE;
    static KHW_EXTENSION hmac_ext = {
        L"hmac-secret", sizeof(BOOL), &hmac_on
    };
    static BYTE uid[] = "kitty.hello.keys";
    HRESULT hr;
    int found;
    int ret = KITTY_HELLO_ERROR;

    *credid_out = NULL;
    if (!api)
        return KITTY_HELLO_UNAVAILABLE;

    found = khw_find_credential(api, credid_out, credidlen_out);
    khw_set_detail("prf find=%d", found);
    if (found == 1)
        return KITTY_HELLO_VERIFIED;
    if (!create_if_missing)
        return KITTY_HELLO_UNAVAILABLE;

    memset(&rp, 0, sizeof(rp));
    rp.dwVersion = 1;
    rp.pwszId = KHW_RP_ID;
    rp.pwszName = L"kitty++";

    memset(&user, 0, sizeof(user));
    user.dwVersion = 1;
    user.cbId = (DWORD)(sizeof(uid) - 1);
    user.pbId = uid;
    user.pwszName = L"kitty++ key protection";
    user.pwszDisplayName = L"kitty++ key protection";

    memset(&alg, 0, sizeof(alg));
    alg.dwVersion = 1;
    alg.pwszCredentialType = L"public-key";
    alg.lAlg = -7;                       /* ES256; irrelevant to PRF */
    algs.cCredentialParameters = 1;
    algs.pCredentialParameters = &alg;

    memset(&cd, 0, sizeof(cd));
    cd.dwVersion = 1;
    cd.cbClientDataJSON = (DWORD)(sizeof(cdata) - 1);
    cd.pbClientDataJSON = (PBYTE)cdata;
    cd.pwszHashAlgId = L"SHA-256";

    memset(&mkopts, 0, sizeof(mkopts));
    mkopts.dwVersion = KHW_MAKE_OPTS_VERSION_USED;
    mkopts.dwTimeoutMilliseconds = 120000;
    mkopts.dwAuthenticatorAttachment = KHW_ATTACHMENT_PLATFORM;
    mkopts.dwUserVerificationRequirement = KHW_UV_REQUIRED;
    mkopts.dwAttestationConveyancePreference = KHW_ATTESTATION_NONE;
    mkopts.bPreferResidentKey = TRUE;
    mkopts.Extensions.cExtensions = 1;
    mkopts.Extensions.pExtensions = &hmac_ext;

    {
        /* Direct call on THIS thread (a worker gets RPC_E_WRONG_THREAD);
         * the anchor window pumps on its own thread meanwhile. When the
         * caller's own visible window is on some OTHER live thread it
         * would also do, but one shape for every case is simpler and it
         * is the proven one. */
        struct khw_anchor anchor;
        khw_anchor_start(&anchor);
        hr = api->MakeCredential(anchor.hwnd ? anchor.hwnd : owner,
                                 &rp, &user, &algs, &cd, &mkopts, &att);
        if (anchor.hwnd)
            owner = anchor.hwnd;   /* for the detail line */
        khw_anchor_stop(&anchor);
    }
    hello_trace("prf create: hr=0x%08lX", (unsigned long)hr);
    khw_set_detail("prf find=%d create hr=0x%08lX owner=%p", found,
                   (unsigned long)hr, (void *)owner);
    if (SUCCEEDED(hr) && att) {
        if (att->cbCredentialId > 0) {
            *credidlen_out = att->cbCredentialId;
            *credid_out = snewn(*credidlen_out, unsigned char);
            memcpy(*credid_out, att->pbCredentialId, *credidlen_out);
            ret = KITTY_HELLO_VERIFIED;
        }
        api->FreeAttestation(att);
    } else {
        ret = (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED) ||
               hr == (HRESULT)0x80090036 /* NTE_DEVICE_NOT_FOUND: how a
                                          * cancelled save flow reports */)
              ? KITTY_HELLO_DENIED : KITTY_HELLO_ERROR;
    }
    return ret;
}

/*
 * The KEK cache: the PRF KEK is the same for every key (one credential,
 * fixed salt), so one Hello gesture can serve a whole BATCH of unlocks
 * and, when the app allows a TTL, quick successive ones. The KEK is held
 * CryptProtectMemory'd; expiry is checked lazily on use and the app is
 * expected to call kitty_hello_cache_wipe() from a timer for hygiene.
 * TTL 0 = batch-only (the cache dies when the last batch ends).
 */
typedef BOOL (WINAPI *pCryptMem_t)(LPVOID, DWORD, DWORD);
static struct {
    unsigned char kek[32];       /* protected in place while resident */
    unsigned char credid_hash[32];
    int valid;
    int protected_ok;
    int batch_depth;
    int ttl_seconds;
    ULONGLONG expiry;            /* GetTickCount64() deadline; 0 = none */
    pCryptMem_t protect, unprotect;
    int fns_loaded;
} khw_kekcache;
static int khw_last_cached = 0;

static void khw_kekcache_fns(void)
{
    if (!khw_kekcache.fns_loaded) {
        HMODULE m = LoadLibraryW(L"crypt32.dll");
        khw_kekcache.fns_loaded = 1;
        if (m) {
            khw_kekcache.protect = (pCryptMem_t)
                GetProcAddress(m, "CryptProtectMemory");
            khw_kekcache.unprotect = (pCryptMem_t)
                GetProcAddress(m, "CryptUnprotectMemory");
        }
    }
}

static void khw_credid_hash(const unsigned char *credid, size_t credidlen,
                            unsigned char out[32])
{
    ssh_hash *h = ssh_hash_new(&ssh_sha256);
    put_data(h, credid, credidlen);
    ssh_hash_final(h, out);
}

void kitty_hello_cache_wipe(void)
{
    smemclr(khw_kekcache.kek, sizeof(khw_kekcache.kek));
    smemclr(khw_kekcache.credid_hash, sizeof(khw_kekcache.credid_hash));
    khw_kekcache.valid = 0;
    khw_kekcache.expiry = 0;
}

void kitty_hello_cache_ttl_set(int seconds)
{
    if (seconds < 0)
        seconds = 0;
    /* A cached KEK carries the OLD deadline; a changed policy must apply
     * NOW, not at the next gesture - measured: TTL set to 0 and the next
     * unlock was still served silently from the old 60 s entry. */
    if (seconds != khw_kekcache.ttl_seconds)
        kitty_hello_cache_wipe();
    khw_kekcache.ttl_seconds = seconds;
}

void kitty_hello_batch_begin(void)
{
    khw_kekcache.batch_depth++;
}

void kitty_hello_batch_end(void)
{
    if (khw_kekcache.batch_depth > 0)
        khw_kekcache.batch_depth--;
    if (khw_kekcache.batch_depth == 0 && khw_kekcache.ttl_seconds == 0)
        kitty_hello_cache_wipe();
}

int kitty_hello_last_was_cached(void)
{
    return khw_last_cached;
}

static int khw_kekcache_get(const unsigned char *credid, size_t credidlen,
                            unsigned char kek[32])
{
    unsigned char h[32];
    if (!khw_kekcache.valid)
        return 0;
    if (khw_kekcache.batch_depth == 0 &&
        (khw_kekcache.expiry == 0 ||
         GetTickCount64() > khw_kekcache.expiry)) {
        kitty_hello_cache_wipe();
        return 0;
    }
    khw_credid_hash(credid, credidlen, h);
    if (memcmp(h, khw_kekcache.credid_hash, 32) != 0)
        return 0;
    memcpy(kek, khw_kekcache.kek, 32);
    if (khw_kekcache.protected_ok && khw_kekcache.unprotect)
        khw_kekcache.unprotect(kek, 32, 0 /* SAME_PROCESS */);
    return 1;
}

static void khw_kekcache_put(const unsigned char *credid, size_t credidlen,
                             const unsigned char kek[32])
{
    kitty_hello_cache_wipe();
    khw_kekcache_fns();
    memcpy(khw_kekcache.kek, kek, 32);
    khw_kekcache.protected_ok = 0;
    if (khw_kekcache.protect &&
        khw_kekcache.protect(khw_kekcache.kek, 32, 0 /* SAME_PROCESS */))
        khw_kekcache.protected_ok = 1;
    khw_credid_hash(credid, credidlen, khw_kekcache.credid_hash);
    khw_kekcache.expiry = khw_kekcache.ttl_seconds > 0 ?
        GetTickCount64() + (ULONGLONG)khw_kekcache.ttl_seconds * 1000 : 0;
    khw_kekcache.valid = 1;
}

/* Derive the PRF KEK for the given credential - one Hello prompt, unless
 * the cache still holds this credential's KEK (a batch in progress, or
 * within the app's TTL). KEK = SHA-256(hmac-secret output), mirroring
 * the KCM side's KEK = SHA-256(signature). */
int kitty_hello_prf_kek(HWND owner, const unsigned char *credid,
                        size_t credidlen, unsigned char kek[32])
{
    khw_api *api = khw_load();
    unsigned char prf_out[32];
    HRESULT hr;

    if (!khw_detail[0] || strstr(khw_detail, "assert"))
        khw_detail[0] = '\0';   /* a fresh operation: start its line clean */

    if (!api)
        return KITTY_HELLO_UNAVAILABLE;
    khw_last_cached = 0;
    if (khw_kekcache_get(credid, credidlen, prf_out)) {
        ssh_hash *h = ssh_hash_new(&ssh_sha256);
        put_data(h, prf_out, sizeof(prf_out));
        ssh_hash_final(h, kek);
        smemclr(prf_out, sizeof(prf_out));
        khw_last_cached = 1;
        khw_set_detail("kek=cached");
        return KITTY_HELLO_VERIFIED;
    }
    {
        struct khw_assert_job a;
        HWND host = khw_owner_usable(owner) ? NULL : khw_host_create();
        memset(&a, 0, sizeof(a));
        a.api = api; a.owner = host ? host : owner;
        a.credid = credid; a.credidlen = credidlen; a.out = prf_out;
        a.hr = E_FAIL;
        if (!khw_run_on_thread(khw_assert_job_run, &a, owner))
            a.hr = E_FAIL;
        hr = a.hr;
        owner = a.owner;   /* for the detail line */
        khw_host_destroy(host);
    }
    hello_trace("prf kek: assert hr=0x%08lX", (unsigned long)hr);
    {
        char prev[sizeof(khw_detail)];
        snprintf(prev, sizeof(prev), "%s", khw_detail);   /* no overlap */
        khw_set_detail("%s; assert hr=0x%08lX owner=%p", prev,
                       (unsigned long)hr, (void *)owner);
    }
    if (SUCCEEDED(hr)) {
        ssh_hash *h = ssh_hash_new(&ssh_sha256);
        khw_kekcache_put(credid, credidlen, prf_out);
        put_data(h, prf_out, sizeof(prf_out));
        ssh_hash_final(h, kek);
        smemclr(prf_out, sizeof(prf_out));
        return KITTY_HELLO_VERIFIED;
    }
    smemclr(prf_out, sizeof(prf_out));
    return (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED) ||
            hr == (HRESULT)0x80090036)
           ? KITTY_HELLO_DENIED : KITTY_HELLO_ERROR;
}

#ifdef KITTY_HELLO_DIAG

int kitty_hello_webauthn_probe(char **msg)
{
    return kitty_hello_webauthn_probe_ex(KHW_RP_ID, KITTY_HELLO_PRF_ENABLE,
                                         0, msg);
}

/* Enumerate the credentials that actually live in the PLATFORM (NGC)
 * store - no UI, no Hello prompt. Diagnostic: distinguishes "passkeys
 * work in the browser" from "passkeys are created in Windows' own
 * store", which are different claims when third-party passkey provider
 * plugins or phone-stored passkeys are in play. */
int kitty_hello_webauthn_list(char **out)
{
    typedef HRESULT (WINAPI *pGetList_t)(const KHW_GET_CREDS_OPTS *,
                                         KHW_CRED_DETAILS_LIST **);
    typedef void (WINAPI *pFreeList_t)(KHW_CRED_DETAILS_LIST *);
    HMODULE dll;
    pGetList_t fnGetList;
    pFreeList_t fnFreeList;
    KHW_GET_CREDS_OPTS opts;
    KHW_CRED_DETAILS_LIST *list = NULL;
    HRESULT hr;
    strbuf *sb;

    *out = NULL;
    dll = LoadLibraryW(L"webauthn.dll");
    if (!dll) {
        *out = dupstr("webauthn.dll not present");
        return -1;
    }
    fnGetList = (pGetList_t)
        GetProcAddress(dll, "WebAuthNGetPlatformCredentialList");
    fnFreeList = (pFreeList_t)
        GetProcAddress(dll, "WebAuthNFreePlatformCredentialList");
    if (!fnGetList || !fnFreeList) {
        *out = dupstr("platform credential list API unavailable");
        return -1;
    }

    memset(&opts, 0, sizeof(opts));
    opts.dwVersion = 1;                /* all RPs */
    hr = fnGetList(&opts, &list);
    if (hr == (HRESULT)0x80090011 /* NTE_NOT_FOUND */ ||
        (SUCCEEDED(hr) && (!list || list->cCredentialDetails == 0))) {
        if (list) fnFreeList(list);
        *out = dupstr("the platform (NGC) store holds NO credentials");
        return 0;
    }
    if (FAILED(hr)) {
        *out = dupprintf("WebAuthNGetPlatformCredentialList failed: "
                         "0x%08lX", (unsigned long)hr);
        return -1;
    }

    sb = strbuf_new();
    put_fmt(sb, "%lu platform credential(s):\n",
            (unsigned long)list->cCredentialDetails);
    {
        DWORD i;
        for (i = 0; i < list->cCredentialDetails; i++) {
            KHW_CRED_DETAILS *d = list->ppCredentialDetails[i];
            if (!d) continue;
            put_fmt(sb, "  rp=%ls (%ls)  user=%ls  authenticator=%ls  "
                    "removable=%d backedup=%d\n",
                    d->pRpInformation ? d->pRpInformation->pwszId : L"?",
                    d->pRpInformation ? d->pRpInformation->pwszName : L"?",
                    d->pUserInformation ? d->pUserInformation->pwszName
                                        : L"?",
                    d->dwVersion >= 3 && d->pwszAuthenticatorName
                        ? d->pwszAuthenticatorName : L"?",
                    (int)d->bRemovable,
                    d->dwVersion >= 2 ? (int)d->bBackedUp : -1);
        }
    }
    fnFreeList(list);
    *out = strbuf_to_str(sb);
    return (int)1;
}

int kitty_hello_webauthn_probe_ex(const WCHAR *rp_id, int prf_mode,
                                  int flags, char **msg)
{
    HMODULE dll;
    pWebAuthNGetApiVersionNumber_t fnVer;
    pWebAuthNMakeCredential_t fnMake;
    pWebAuthNGetAssertion_t fnGetAssertion;
    pWebAuthNFreeAttestation_t fnFreeAtt;
    pWebAuthNFreeAssertion_t fnFreeAssertion;
    pWebAuthNDeleteCred_t fnDelete;
    pWebAuthNGetErrorName_t fnErrName;
    DWORD apiver;
    HWND host;
    int ret = KITTY_HELLO_ERROR;

    static const char cdata[] =
        "{\"type\":\"webauthn.create\",\"challenge\":\"a2l0dHktcHJvYmU\","
        "\"origin\":\"https://kitty.kapper.net\"}";
    static const unsigned char salt[32] = {
        'K','i','T','T','Y',' ','P','R','F',' ','K','E','K',' ','s','a',
        'l','t',' ','v','1',0,0,0,0,0,0,0,0,0,0,0
    };

    *msg = NULL;
    dll = LoadLibraryW(L"webauthn.dll");
    if (!dll) {
        *msg = dupstr("webauthn.dll not present");
        return KITTY_HELLO_UNAVAILABLE;
    }
    fnVer = (pWebAuthNGetApiVersionNumber_t)
        GetProcAddress(dll, "WebAuthNGetApiVersionNumber");
    fnMake = (pWebAuthNMakeCredential_t)
        GetProcAddress(dll, "WebAuthNAuthenticatorMakeCredential");
    fnGetAssertion = (pWebAuthNGetAssertion_t)
        GetProcAddress(dll, "WebAuthNAuthenticatorGetAssertion");
    fnFreeAtt = (pWebAuthNFreeAttestation_t)
        GetProcAddress(dll, "WebAuthNFreeCredentialAttestation");
    fnFreeAssertion = (pWebAuthNFreeAssertion_t)
        GetProcAddress(dll, "WebAuthNFreeAssertion");
    fnDelete = (pWebAuthNDeleteCred_t)
        GetProcAddress(dll, "WebAuthNDeletePlatformCredential");
    fnErrName = (pWebAuthNGetErrorName_t)
        GetProcAddress(dll, "WebAuthNGetErrorName");
    if (!fnVer || !fnMake || !fnGetAssertion || !fnFreeAtt ||
        !fnFreeAssertion) {
        *msg = dupstr("webauthn.dll lacks required exports");
        return KITTY_HELLO_UNAVAILABLE;
    }

    apiver = fnVer();
    hello_trace("webauthn: api version %lu", (unsigned long)apiver);
    if (apiver < 6) {
        *msg = dupprintf("webauthn API version %lu has no PRF support "
                         "(need 6+)", (unsigned long)apiver);
        return KITTY_HELLO_UNAVAILABLE;
    }

    {
        bool own_host = true;
        DWORD uv = (flags & KITTY_HELLO_WA_UVPREF) ? KHW_UV_PREFERRED
                                                   : KHW_UV_REQUIRED;
        host = NULL;
        if (flags & KITTY_HELLO_WA_FGWND) {
            host = GetForegroundWindow();
            own_host = false;
            hello_trace("webauthn: owner = real foreground window %p",
                        (void *)host);
        }
        if (!host) {
            host = hello_host_create();
            own_host = true;
        }
        if (!host) {
            *msg = dupstr("no host window");
            return KITTY_HELLO_ERROR;
        }

    {
        KHW_RP_INFO rp;
        KHW_USER_INFO user;
        KHW_COSE_PARAM alg;
        KHW_COSE_PARAMS algs;
        KHW_CLIENT_DATA cd;
        KHW_MAKE_OPTS mkopts;
        KHW_ATTESTATION *att = NULL;
        static BYTE uid[] = "kageant-hello-probe";
        BYTE *credid = NULL;
        DWORD credidlen = 0;
        HRESULT hr;

        memset(&rp, 0, sizeof(rp));
        rp.dwVersion = 1;
        rp.pwszId = rp_id;
        rp.pwszName = L"KiTTY (kageant probe)";

        memset(&user, 0, sizeof(user));
        user.dwVersion = 1;
        user.cbId = (DWORD)(sizeof(uid) - 1);
        user.pbId = uid;
        user.pwszName = L"kageant probe";
        user.pwszDisplayName = L"kageant probe";

        memset(&alg, 0, sizeof(alg));
        alg.dwVersion = 1;
        alg.pwszCredentialType = L"public-key";
        alg.lAlg = -7;                     /* ES256; irrelevant to PRF */
        algs.cCredentialParameters = 1;
        algs.pCredentialParameters = &alg;

        memset(&cd, 0, sizeof(cd));
        cd.dwVersion = 1;
        cd.cbClientDataJSON = (DWORD)(sizeof(cdata) - 1);
        cd.pbClientDataJSON = (PBYTE)cdata;
        cd.pwszHashAlgId = L"SHA-256";

        memset(&mkopts, 0, sizeof(mkopts));
        mkopts.dwVersion = KHW_MAKE_OPTS_VERSION_USED;
        mkopts.dwTimeoutMilliseconds = 120000;
        mkopts.dwAuthenticatorAttachment = KHW_ATTACHMENT_PLATFORM;
        mkopts.dwUserVerificationRequirement = uv;
        /* Match what browsers send. Attestation NONE is load-bearing:
         * with the default (ANY) the platform may attempt full TPM
         * attestation - AIK certificate provisioning - after the
         * gesture, which fails on many corporate networks and surfaces
         * as a retrying generic failure dialog. A discoverable
         * credential likewise mirrors the browser-created passkeys that
         * are known to work. */
        mkopts.dwAttestationConveyancePreference = KHW_ATTESTATION_NONE;
        mkopts.bPreferResidentKey = TRUE;
        if (prf_mode == KITTY_HELLO_PRF_ENABLE) {
            mkopts.bEnablePrf = TRUE;
        } else if (prf_mode == KITTY_HELLO_PRF_HMAC_EXT) {
            /* The CTAP-flavoured spelling: the "hmac-secret" BOOL
             * extension instead of the WebAuthn-level PRF enable. */
            static BOOL hmac_on = TRUE;
            static KHW_EXTENSION ext = {
                L"hmac-secret", sizeof(BOOL), &hmac_on
            };
            mkopts.Extensions.cExtensions = 1;
            mkopts.Extensions.pExtensions = &ext;
        }

        hello_trace("webauthn: MakeCredential starting (Hello prompt "
                    "expected)");
        hr = fnMake(host, &rp, &user, &algs, &cd, &mkopts, &att);
        hello_trace("webauthn: MakeCredential hr=0x%08lX (%ls)",
                    (unsigned long)hr,
                    fnErrName ? fnErrName(hr) : L"?");
        if (SUCCEEDED(hr) && att) {
            hello_trace("webauthn: attestation version %lu, "
                        "prfEnabled=%d, credid %lu bytes",
                        (unsigned long)att->dwVersion,
                        att->dwVersion >= 5 ? (int)att->bPrfEnabled : -1,
                        (unsigned long)att->cbCredentialId);
            if (att->cbCredentialId > 0) {
                credidlen = att->cbCredentialId;
                credid = snewn(credidlen, BYTE);
                memcpy(credid, att->pbCredentialId, credidlen);
            }
            if (prf_mode == KITTY_HELLO_PRF_NONE) {
                *msg = dupstr("creation succeeded WITHOUT any PRF/"
                              "hmac-secret request - if the PRF variants "
                              "fail, the PRF enable itself is what breaks "
                              "creation");
                ret = KITTY_HELLO_UNAVAILABLE;   /* diagnostic only */
            } else if (prf_mode == KITTY_HELLO_PRF_ENABLE &&
                       !(att->dwVersion >= 5 && att->bPrfEnabled)) {
                *msg = dupstr("credential created but the platform "
                              "authenticator did not enable PRF");
                ret = KITTY_HELLO_UNAVAILABLE;
            }
            fnFreeAtt(att);
        } else {
            *msg = dupprintf("MakeCredential failed: 0x%08lX (%ls)",
                             (unsigned long)hr,
                             fnErrName ? fnErrName(hr) : L"?");
            ret = (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED))
                  ? KITTY_HELLO_DENIED : KITTY_HELLO_ERROR;
        }

        if (credid) {
            unsigned char s1[32], s2[32];
            if (!*msg) {   /* no verdict yet: run the PRF assertions */
            hello_trace("webauthn: first PRF assertion (Hello prompt "
                        "expected)");
            hr = khw_prf_assert(fnGetAssertion, fnFreeAssertion, host,
                                rp_id, uv, credid, credidlen, salt, s1);
            hello_trace("webauthn: first assertion hr=0x%08lX",
                        (unsigned long)hr);
            if (SUCCEEDED(hr)) {
                hello_trace("webauthn: second PRF assertion (Hello "
                            "prompt expected)");
                hr = khw_prf_assert(fnGetAssertion, fnFreeAssertion, host,
                                    rp_id, uv, credid, credidlen, salt,
                                    s2);
                hello_trace("webauthn: second assertion hr=0x%08lX",
                            (unsigned long)hr);
            }
            if (SUCCEEDED(hr)) {
                if (smemeq(s1, s2, 32)) {
                    *msg = dupstr("PRF DETERMINISTIC: two Hello-gated "
                                  "assertions returned identical 32-byte "
                                  "secrets - viable as the KEK source");
                    ret = KITTY_HELLO_VERIFIED;
                } else {
                    *msg = dupstr("PRF NOT deterministic: secrets differ "
                                  "- not usable as a KEK");
                    ret = KITTY_HELLO_DENIED;
                }
            } else if (!*msg) {
                *msg = dupprintf("GetAssertion failed: 0x%08lX (%ls)",
                                 (unsigned long)hr,
                                 fnErrName ? fnErrName(hr) : L"?");
                ret = (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED))
                      ? KITTY_HELLO_DENIED : KITTY_HELLO_ERROR;
            }
            smemclr(s1, sizeof(s1));
            smemclr(s2, sizeof(s2));
            }   /* if (!*msg) */

            /* Leave no state behind: the probe credential dies here. */
            if (fnDelete) {
                hr = fnDelete(credidlen, credid);
                hello_trace("webauthn: probe credential deleted, "
                            "hr=0x%08lX", (unsigned long)hr);
            }
            sfree(credid);
        }
    }

        if (own_host)
            DestroyWindow(host);
    }
    if (!*msg)
        *msg = dupstr("probe did not run");
    return ret;
}

#endif /* KITTY_HELLO_DIAG */

/* ====================================================================
 * The wrapped-secret container. Pure crypto on in-tree primitives - no
 * WinRT, no UI - so this whole layer is exercised by the unit test
 * (test_hello_container.c) with fixed KEKs and by kittygen-cli headless.
 *
 *   HELLOK1:H<b64 hello-blob>.R<mem>,<passes>,<parallel>,<b64 salt>,<b64 blob>
 *
 * Either field may be absent (never both). Unknown fields are ignored so
 * HELLOK1 can grow. Each blob is nonce(12) || AES-256-GCM ciphertext of
 * the 32-byte secret || tag(16), with the format marker as associated
 * data; the recovery key is Argon2id of the passphrase with its cost
 * RECORDED in the container - the MPW lesson: a cost baked only into the
 * binary cannot ever be tuned without locking old data out. Recorded
 * parameters are validated (argon2_params_bad) before EVER reaching the
 * KDF. GCM is safe here where the MPW envelope chose CBC: every wrap is
 * a fresh random nonce under a key that wraps only this one secret.
 */

#define HELLO_MARK          "HELLOK1:"
#define HELLO_AAD           HELLO_MARK      /* binds blobs to the format */
#define HELLO_NONCE_LEN     12
#define HELLO_TAG_LEN       16
#define HELLO_BLOB_LEN      (HELLO_NONCE_LEN + KITTY_HELLO_SECRET_LEN + \
                             HELLO_TAG_LEN)
#define HELLO_SALT_LEN      16
#define HELLO_KEK_LEN       32

/* Recovery-wrap Argon2id cost written by THIS build (RFC 9106's second
 * recommended parameter set). Recorded in the container, so raising it
 * later affects only newly written containers. */
#define HELLO_ARGON_MEM       65536     /* KiB */
#define HELLO_ARGON_PASSES    3
#define HELLO_ARGON_PARALLEL  4

/* System CSPRNG for nonces, salts and generated secrets. PuTTY's own pool
 * is not usable here: pageant.c deliberately stubs random_read() with a
 * fatal error, and this file runs inside kageant. RtlGenRandom is the
 * documented-stable export underneath CryptGenRandom. */
BOOLEAN NTAPI SystemFunction036(PVOID, ULONG);
static bool hello_random(void *buf, size_t len)
{
    return SystemFunction036(buf, (ULONG)len);
}

/* One-shot AES-256-GCM. blob = nonce || ct || tag; call order per
 * test/cryptsuite.py: cipher setkey, setiv(nonce || 4 zero bytes), mac
 * setkey (derives its mask from the cipher), prefix lengths, encrypt,
 * MAC over aad || ciphertext. */
static bool hello_gcm_wrap(const unsigned char kek[HELLO_KEK_LEN],
                           const unsigned char secret[KITTY_HELLO_SECRET_LEN],
                           unsigned char blob[HELLO_BLOB_LEN])
{
    ssh_cipher *c = ssh_cipher_new(&ssh_aes256_gcm);
    ssh2_mac *m;
    unsigned char iv[16];
    unsigned char *ct = blob + HELLO_NONCE_LEN;

    if (!c)
        return false;
    m = ssh2_mac_new(&ssh2_aesgcm_mac, c);
    if (!m) {
        ssh_cipher_free(c);
        return false;
    }
    if (!hello_random(blob, HELLO_NONCE_LEN)) {
        ssh2_mac_free(m);
        ssh_cipher_free(c);
        return false;
    }
    memcpy(iv, blob, HELLO_NONCE_LEN);
    memset(iv + HELLO_NONCE_LEN, 0, 4);
    ssh_cipher_setkey(c, kek);
    ssh_cipher_setiv(c, iv);
    ssh2_mac_setkey(m, PTRLEN_LITERAL(""));
    aesgcm_set_prefix_lengths(m, 0, sizeof(HELLO_AAD) - 1);

    memcpy(ct, secret, KITTY_HELLO_SECRET_LEN);
    ssh_cipher_encrypt(c, ct, KITTY_HELLO_SECRET_LEN);
    ssh2_mac_start(m);
    put_data(m, HELLO_AAD, sizeof(HELLO_AAD) - 1);
    put_data(m, ct, KITTY_HELLO_SECRET_LEN);
    ssh2_mac_genresult(m, ct + KITTY_HELLO_SECRET_LEN);

    ssh2_mac_free(m);
    ssh_cipher_free(c);
    smemclr(iv, sizeof(iv));
    return true;
}

/* Inverse: verify the tag FIRST, then decrypt. false = refused. */
static bool hello_gcm_unwrap(const unsigned char kek[HELLO_KEK_LEN],
                             const unsigned char blob[HELLO_BLOB_LEN],
                             unsigned char secret[KITTY_HELLO_SECRET_LEN])
{
    ssh_cipher *c = ssh_cipher_new(&ssh_aes256_gcm);
    ssh2_mac *m;
    unsigned char iv[16], tag[HELLO_TAG_LEN];
    const unsigned char *ct = blob + HELLO_NONCE_LEN;
    bool ok;

    if (!c)
        return false;
    m = ssh2_mac_new(&ssh2_aesgcm_mac, c);
    if (!m) {
        ssh_cipher_free(c);
        return false;
    }
    memcpy(iv, blob, HELLO_NONCE_LEN);
    memset(iv + HELLO_NONCE_LEN, 0, 4);
    ssh_cipher_setkey(c, kek);
    ssh_cipher_setiv(c, iv);
    ssh2_mac_setkey(m, PTRLEN_LITERAL(""));
    aesgcm_set_prefix_lengths(m, 0, sizeof(HELLO_AAD) - 1);

    ssh2_mac_start(m);
    put_data(m, HELLO_AAD, sizeof(HELLO_AAD) - 1);
    put_data(m, ct, KITTY_HELLO_SECRET_LEN);
    ssh2_mac_genresult(m, tag);
    ok = smemeq(tag, ct + KITTY_HELLO_SECRET_LEN, HELLO_TAG_LEN);
    if (ok) {
        memcpy(secret, ct, KITTY_HELLO_SECRET_LEN);
        ssh_cipher_decrypt(c, secret, KITTY_HELLO_SECRET_LEN);
    }

    ssh2_mac_free(m);
    ssh_cipher_free(c);
    smemclr(iv, sizeof(iv));
    smemclr(tag, sizeof(tag));
    return ok;
}

/* Derive the recovery KEK. Validates the (recorded or compiled-in) cost
 * before running the KDF; false = illegal parameters, refused. */
static bool hello_recovery_kek(const char *passphrase,
                               const unsigned char salt[HELLO_SALT_LEN],
                               uint32_t mem, uint32_t passes,
                               uint32_t parallel,
                               unsigned char kek[HELLO_KEK_LEN])
{
    char *bad = argon2_params_bad(mem, passes, parallel, HELLO_KEK_LEN,
                                  strlen(passphrase), HELLO_SALT_LEN, 0, 0);
    strbuf *sb;
    ptrlen empty = PTRLEN_LITERAL("");

    if (bad) {
        sfree(bad);
        return false;
    }
    sb = strbuf_new_nm();
    argon2(Argon2id, mem, passes, parallel, HELLO_KEK_LEN,
           ptrlen_from_asciz(passphrase),
           make_ptrlen(salt, HELLO_SALT_LEN), empty, empty, sb);
    memcpy(kek, sb->u, HELLO_KEK_LEN);
    strbuf_free(sb);
    return true;
}

static char *hello_w_field(const unsigned char prf_kek[32],
                           const unsigned char *prf_credid,
                           size_t prf_credidlen, const char *owner,
                           const unsigned char secret[KITTY_HELLO_SECRET_LEN]);

char *kitty_hello_container_create(const unsigned char hello_kek[32],
                                   const char *recovery_passphrase,
                                   const unsigned char
                                       secret[KITTY_HELLO_SECRET_LEN])
{
    return kitty_hello_container_create_ex(hello_kek, NULL, NULL, 0,
                                           NULL, recovery_passphrase, secret);
}

char *kitty_hello_container_create_ex(const unsigned char hello_kek[32],
                                      const unsigned char prf_kek[32],
                                      const unsigned char *prf_credid,
                                      size_t prf_credidlen,
                                      const char *prf_owner,
                                      const char *recovery_passphrase,
                                      const unsigned char
                                          secret[KITTY_HELLO_SECRET_LEN])
{
    strbuf *out;
    bool first = true;

    if (!hello_kek && !prf_kek && !recovery_passphrase)
        return NULL;
    if ((prf_kek != NULL) != (prf_credid != NULL && prf_credidlen > 0))
        return NULL;      /* the W wrap needs BOTH the KEK and the id */

    out = strbuf_new_nm();
    put_dataz(out, HELLO_MARK);

    if (hello_kek) {
        unsigned char blob[HELLO_BLOB_LEN];
        strbuf *b64;
        if (!hello_gcm_wrap(hello_kek, secret, blob)) {
            strbuf_free(out);
            return NULL;
        }
        b64 = base64_encode_sb(make_ptrlen(blob, HELLO_BLOB_LEN), 0);
        put_dataz(out, "H");
        put_dataz(out, b64->s);
        strbuf_free(b64);
        smemclr(blob, sizeof(blob));
        first = false;
    }

    if (prf_kek) {
        /* W<b64 credential id>,<b64 blob>[,<b64 owner>]: the WebAuthn-PRF
         * wrap. The credential id travels IN the container so unwrap can
         * pick the field that belongs to this account; the owner tag is
         * informational text for the "wrapped elsewhere" message. */
        char *w = hello_w_field(prf_kek, prf_credid, prf_credidlen,
                                prf_owner, secret);
        if (!w) {
            strbuf_free(out);
            return NULL;
        }
        if (!first)
            put_dataz(out, ".");
        put_dataz(out, w);
        sfree(w);
        first = false;
    }

    if (recovery_passphrase) {
        unsigned char salt[HELLO_SALT_LEN], kek[HELLO_KEK_LEN];
        unsigned char blob[HELLO_BLOB_LEN];
        strbuf *b64salt, *b64blob;
        if (!hello_random(salt, sizeof(salt)) ||
            !hello_recovery_kek(recovery_passphrase, salt,
                                HELLO_ARGON_MEM, HELLO_ARGON_PASSES,
                                HELLO_ARGON_PARALLEL, kek) ||
            !hello_gcm_wrap(kek, secret, blob)) {
            smemclr(kek, sizeof(kek));
            strbuf_free(out);
            return NULL;
        }
        smemclr(kek, sizeof(kek));
        b64salt = base64_encode_sb(make_ptrlen(salt, sizeof(salt)), 0);
        b64blob = base64_encode_sb(make_ptrlen(blob, HELLO_BLOB_LEN), 0);
        if (!first)
            put_dataz(out, ".");
        put_fmt(out, "R%u,%u,%u,%s,%s",
                    (unsigned)HELLO_ARGON_MEM, (unsigned)HELLO_ARGON_PASSES,
                    (unsigned)HELLO_ARGON_PARALLEL, b64salt->s, b64blob->s);
        strbuf_free(b64salt);
        strbuf_free(b64blob);
        smemclr(blob, sizeof(blob));
    }

    return strbuf_to_str(out);
}

/* Find the body of the index-th field (0-based) starting with the given
 * letter (the text up to the next '.' or end). NULL if absent. A letter
 * may repeat: every enrolled account+machine adds its own W field. */
static const char *hello_container_field_n(const char *container,
                                           char letter, int index,
                                           size_t *len_out)
{
    const char *p;
    if (!container ||
        strncmp(container, HELLO_MARK, sizeof(HELLO_MARK) - 1) != 0)
        return NULL;
    p = container + sizeof(HELLO_MARK) - 1;
    while (*p) {
        const char *end = strchr(p, '.');
        size_t flen = end ? (size_t)(end - p) : strlen(p);
        if (flen > 0 && *p == letter && index-- == 0) {
            *len_out = flen - 1;
            return p + 1;
        }
        p += flen + (end ? 1 : 0);
    }
    return NULL;
}

static const char *hello_container_field(const char *container, char letter,
                                         size_t *len_out)
{
    return hello_container_field_n(container, letter, 0, len_out);
}

/* Split one W field body into its comma parts: credential id, blob and
 * the optional owner tag (all base64). false = malformed. */
static bool hello_w_parts(const char *f, size_t flen,
                          ptrlen *id, ptrlen *blob, ptrlen *owner)
{
    const char *c1 = memchr(f, ',', flen), *c2;
    if (!c1 || c1 == f)
        return false;
    *id = make_ptrlen(f, c1 - f);
    c2 = memchr(c1 + 1, ',', flen - (c1 + 1 - f));
    if (c2) {
        *blob = make_ptrlen(c1 + 1, c2 - (c1 + 1));
        *owner = make_ptrlen(c2 + 1, flen - (c2 + 1 - f));
    } else {
        *blob = make_ptrlen(c1 + 1, flen - (c1 + 1 - f));
        *owner = make_ptrlen("", 0);
    }
    return blob->len > 0;
}

/* One W field as text: W<b64 id>,<b64 blob>[,<b64 owner>]. NULL on a
 * wrap failure. */
static char *hello_w_field(const unsigned char prf_kek[32],
                           const unsigned char *prf_credid,
                           size_t prf_credidlen, const char *owner,
                           const unsigned char secret[KITTY_HELLO_SECRET_LEN])
{
    unsigned char blob[HELLO_BLOB_LEN];
    strbuf *b64id, *b64blob, *out;
    if (!hello_gcm_wrap(prf_kek, secret, blob))
        return NULL;
    b64id = base64_encode_sb(make_ptrlen(prf_credid, prf_credidlen), 0);
    b64blob = base64_encode_sb(make_ptrlen(blob, HELLO_BLOB_LEN), 0);
    out = strbuf_new();
    put_fmt(out, "W%s,%s", b64id->s, b64blob->s);
    if (owner && *owner) {
        strbuf *b64own = base64_encode_sb(ptrlen_from_asciz(owner), 0);
        put_fmt(out, ",%s", b64own->s);
        strbuf_free(b64own);
    }
    strbuf_free(b64id);
    strbuf_free(b64blob);
    smemclr(blob, sizeof(blob));
    return strbuf_to_str(out);
}

int kitty_hello_container_w_count(const char *container)
{
    size_t len;
    int n = 0;
    while (hello_container_field_n(container, 'W', n, &len))
        n++;
    return n;
}

char *kitty_hello_container_w_owner(const char *container, int index)
{
    size_t flen;
    const char *f = hello_container_field_n(container, 'W', index, &flen);
    ptrlen id, blob, owner;
    strbuf *raw;
    char *s;
    if (!f || !hello_w_parts(f, flen, &id, &blob, &owner) || !owner.len)
        return NULL;
    raw = base64_decode_sb(owner);
    if (!raw)
        return NULL;
    s = dupprintf("%.*s", (int)raw->len, (const char *)raw->u);
    strbuf_free(raw);
    return s;
}

int kitty_hello_container_w_credid(const char *container, int index,
                                   unsigned char **credid_out,
                                   size_t *credidlen_out)
{
    size_t flen;
    const char *f = hello_container_field_n(container, 'W', index, &flen);
    ptrlen id, blob, owner;
    strbuf *raw;

    *credid_out = NULL;
    if (!f)
        return 0;
    if (!hello_w_parts(f, flen, &id, &blob, &owner))
        return -1;
    raw = base64_decode_sb(id);
    if (!raw || raw->len == 0) {
        if (raw) strbuf_free(raw);
        return -1;
    }
    *credidlen_out = raw->len;
    *credid_out = snewn(raw->len, unsigned char);
    memcpy(*credid_out, raw->u, raw->len);
    strbuf_free(raw);
    return 1;
}

int kitty_hello_container_find_w(const char *container,
                                 const unsigned char *credid,
                                 size_t credidlen)
{
    int i, n = kitty_hello_container_w_count(container);
    for (i = 0; i < n; i++) {
        unsigned char *id = NULL;
        size_t idlen = 0;
        bool match;
        if (kitty_hello_container_w_credid(container, i, &id, &idlen) != 1)
            continue;
        match = idlen == credidlen && !memcmp(id, credid, credidlen);
        sfree(id);
        if (match)
            return i;
    }
    return -1;
}

/*
 * Remove the index-th W field - pure string surgery, no secret needed:
 * taking a door AWAY must not require opening one. Refuses to remove
 * the last door of the container (a sidecar with no doors is a lie
 * beside the key file - delete the file instead). Caller sfree; NULL =
 * refused or no such field.
 */
char *kitty_hello_container_remove_w(const char *container, int index)
{
    const char *mark_end;
    const char *p;
    strbuf *out;
    int wi = 0, doors = 0, removed = 0;

    if (!kitty_hello_container_valid(container))
        return NULL;
    doors = kitty_hello_container_w_count(container) +
            (kitty_hello_container_has_hello(container) ? 1 : 0) +
            (kitty_hello_container_has_recovery(container) ? 1 : 0);
    if (doors <= 1)
        return NULL;            /* never leave a doorless sidecar */
    if (index < 0 || index >= kitty_hello_container_w_count(container))
        return NULL;

    mark_end = container + sizeof(HELLO_MARK) - 1;
    out = strbuf_new();
    put_data(out, container, mark_end - container);
    p = mark_end;
    while (*p) {
        const char *end = strchr(p, '.');
        size_t flen = end ? (size_t)(end - p) : strlen(p);
        int skip = 0;
        if (flen > 0 && *p == 'W') {
            if (wi == index)
                skip = 1;
            wi++;
        }
        if (!skip) {
            if (out->len > (size_t)(mark_end - container))
                put_dataz(out, ".");
            put_data(out, p, flen);
        } else {
            removed = 1;
        }
        p += flen + (end ? 1 : 0);
    }
    if (!removed) {
        strbuf_free(out);
        return NULL;
    }
    return strbuf_to_str(out);
}

char *kitty_hello_container_append_prf(const char *container,
                                       const unsigned char prf_kek[32],
                                       const unsigned char *prf_credid,
                                       size_t prf_credidlen,
                                       const char *owner,
                                       const unsigned char
                                           secret[KITTY_HELLO_SECRET_LEN])
{
    char *w, *out;
    if (!kitty_hello_container_valid(container) || !prf_credid ||
        !prf_credidlen)
        return NULL;
    /* Every wrap is an independent GCM blob under the marker as AAD, so
     * the existing text is kept byte for byte: other accounts' doors are
     * never re-encoded, only ours is added. */
    w = hello_w_field(prf_kek, prf_credid, prf_credidlen, owner, secret);
    if (!w)
        return NULL;
    out = dupprintf("%s%s%s", container,
                    container[strlen(container) - 1] == ':' ? "" : ".", w);
    sfree(w);
    return out;
}

int kitty_hello_container_valid(const char *container)
{
    return kitty_hello_container_has_hello(container) ||
           kitty_hello_container_has_prf(container) ||
           kitty_hello_container_has_recovery(container);
}

int kitty_hello_container_has_hello(const char *container)
{
    size_t len;
    return hello_container_field(container, 'H', &len) != NULL;
}

int kitty_hello_container_has_recovery(const char *container)
{
    size_t len;
    return hello_container_field(container, 'R', &len) != NULL;
}

int kitty_hello_container_has_prf(const char *container)
{
    size_t len;
    return hello_container_field(container, 'W', &len) != NULL;
}

/* Decode a b64 field expected to be exactly wantlen bytes. */
static bool hello_b64_fixed(const char *s, size_t slen, unsigned char *out,
                            size_t wantlen)
{
    strbuf *raw = base64_decode_sb(make_ptrlen(s, slen));
    bool ok = raw && raw->len == wantlen;
    if (ok)
        memcpy(out, raw->u, wantlen);
    if (raw) {
        smemclr(raw->u, raw->len);
        strbuf_free(raw);
    }
    return ok;
}

int kitty_hello_container_open_kek(const char *container,
                                   const unsigned char hello_kek[32],
                                   unsigned char
                                       secret_out[KITTY_HELLO_SECRET_LEN])
{
    size_t flen;
    const char *f = hello_container_field(container, 'H', &flen);
    unsigned char blob[HELLO_BLOB_LEN];
    int ret;

    if (!f)
        return 0;
    if (!hello_b64_fixed(f, flen, blob, HELLO_BLOB_LEN))
        return -1;
    ret = hello_gcm_unwrap(hello_kek, blob, secret_out) ? 1 : -1;
    smemclr(blob, sizeof(blob));
    return ret;
}

/* Open ONE R field body with the given passphrase. 1/-1. */
static int hello_open_recovery_field(const char *f, size_t flen,
                                     const char *passphrase,
                                     unsigned char
                                         secret_out[KITTY_HELLO_SECRET_LEN])
{
    unsigned mem, passes, parallel;
    int consumed = 0;
    unsigned char salt[HELLO_SALT_LEN], kek[HELLO_KEK_LEN];
    unsigned char blob[HELLO_BLOB_LEN];
    const char *b64salt, *b64blob, *comma;
    int ret = -1;

    /* R<mem>,<passes>,<parallel>,<b64 salt>,<b64 blob> - the numbers are
     * whatever the WRITING build used; they go through argon2_params_bad
     * before the KDF ever sees them. */
    if (sscanf(f, "%u,%u,%u,%n", &mem, &passes, &parallel, &consumed) != 3 ||
        consumed <= 0 || (size_t)consumed >= flen)
        return -1;
    b64salt = f + consumed;
    comma = memchr(b64salt, ',', flen - consumed);
    if (!comma)
        return -1;
    b64blob = comma + 1;

    {
        /* An optional trailing ",c" tags a CODE door (the sidecar-bound
         * printout); the blob ends at that comma. Unknown tags are
         * ignored the same way. */
        const char *bend = memchr(b64blob, ',', flen - (b64blob - f));
        size_t bloblen = bend ? (size_t)(bend - b64blob)
                              : flen - (b64blob - f);
        if (!hello_b64_fixed(b64salt, comma - b64salt, salt,
                             HELLO_SALT_LEN) ||
            !hello_b64_fixed(b64blob, bloblen, blob, HELLO_BLOB_LEN))
            return -1;
    }

    if (hello_recovery_kek(passphrase, salt, mem, passes, parallel, kek))
        ret = hello_gcm_unwrap(kek, blob, secret_out) ? 1 : -1;

    smemclr(kek, sizeof(kek));
    smemclr(blob, sizeof(blob));
    return ret;
}

int kitty_hello_container_open_recovery(const char *container,
                                        const char *passphrase,
                                        unsigned char
                                            secret_out[KITTY_HELLO_SECRET_LEN])
{
    /* A container may carry SEVERAL R doors (a typed recovery passphrase
     * and a sidecar-bound recovery code are both R wraps); the given
     * text opens exactly the one keyed on it - the KDF+GCM refuse the
     * others - so trying each in turn is correct and cheap enough (one
     * Argon2 run per R). */
    int i = 0, ret = 0;
    size_t flen;
    const char *f;
    while ((f = hello_container_field_n(container, 'R', i++, &flen))
               != NULL) {
        int r = hello_open_recovery_field(f, flen, passphrase, secret_out);
        if (r == 1)
            return 1;
        ret = -1;
    }
    return ret;
}

/*
 * The RECOVERY CODE's printed form - deliberately unmistakable for the
 * printed passphrase (his report: the two looked identical and were
 * mixed up): "KRC1-" prefix, 8 groups of 4 hex (16 random bytes - a
 * KDF-protected door does not need 256 bits), and a 2-hex check group.
 * The parser is as tolerant as the passphrase one (case, dashes,
 * spaces) but REQUIRES the prefix.
 */
char *kitty_hello_code_text(const unsigned char code[16])
{
    static const char hexd[] = "0123456789ABCDEF";
    unsigned char chk[32];
    strbuf *sb = strbuf_new_nm();
    int i;
    ssh_hash *h = ssh_hash_new(&ssh_sha256);
    put_data(h, code, 16);
    ssh_hash_final(h, chk);
    put_dataz(sb, "KRC1");
    for (i = 0; i < 16; i++) {
        if ((i & 1) == 0)
            put_byte(sb, '-');
        put_byte(sb, hexd[code[i] >> 4]);
        put_byte(sb, hexd[code[i] & 15]);
    }
    put_fmt(sb, "-%c%c", hexd[chk[0] >> 4], hexd[chk[0] & 15]);
    return strbuf_to_str(sb);
}

int kitty_hello_code_from_text(const char *text, unsigned char code_out[16])
{
    unsigned char digits[34];
    unsigned char chk[32];
    int nd = 0, i;
    const char *p = text;

    if (!text)
        return 0;
    while (*p == ' ' || *p == '\t')
        p++;
    if ((p[0] != 'K' && p[0] != 'k') || (p[1] != 'R' && p[1] != 'r') ||
        (p[2] != 'C' && p[2] != 'c') || p[3] != '1')
        return 0;
    p += 4;
    for (; *p; p++) {
        int v;
        if (*p == '-' || *p == ' ' || *p == '\t')
            continue;
        if (*p >= '0' && *p <= '9') v = *p - '0';
        else if (*p >= 'a' && *p <= 'f') v = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F') v = *p - 'A' + 10;
        else return 0;
        if (nd >= 34)
            return 0;
        digits[nd++] = (unsigned char)v;
    }
    if (nd != 34)
        return 0;
    for (i = 0; i < 16; i++)
        code_out[i] = (unsigned char)((digits[i * 2] << 4) | digits[i * 2 + 1]);
    {
        ssh_hash *h = ssh_hash_new(&ssh_sha256);
        put_data(h, code_out, 16);
        ssh_hash_final(h, chk);
    }
    if (((digits[32] << 4) | digits[33]) != chk[0]) {
        smemclr(code_out, 16);
        return 0;
    }
    return 1;
}

int kitty_hello_container_r_count(const char *container)
{
    size_t len;
    int n = 0;
    while (hello_container_field_n(container, 'R', n, &len))
        n++;
    return n;
}

/* Is the index-th R door a CODE door (",c" tag)? 1/0; -1 = no such R. */
int kitty_hello_container_r_is_code(const char *container, int index)
{
    size_t flen;
    const char *f = hello_container_field_n(container, 'R', index, &flen);
    if (!f)
        return -1;
    return (flen >= 2 && f[flen - 2] == ',' && f[flen - 1] == 'c') ? 1 : 0;
}

/* Append one more R door to an existing container - the existing text is
 * kept byte for byte (the sidecar-bound recovery code is such a door:
 * an R keyed on a random printed code instead of a typed passphrase).
 * Caller sfree; NULL on failure. */
char *kitty_hello_container_append_recovery(
    const char *container, const char *passphrase,
    const unsigned char secret[KITTY_HELLO_SECRET_LEN])
{
    unsigned char salt[HELLO_SALT_LEN], kek[HELLO_KEK_LEN];
    unsigned char blob[HELLO_BLOB_LEN];
    strbuf *b64salt, *b64blob;
    char *out;

    if (!kitty_hello_container_valid(container) || !passphrase ||
        !*passphrase)
        return NULL;
    if (!hello_random(salt, sizeof(salt)) ||
        !hello_recovery_kek(passphrase, salt, HELLO_ARGON_MEM,
                            HELLO_ARGON_PASSES, HELLO_ARGON_PARALLEL, kek) ||
        !hello_gcm_wrap(kek, secret, blob)) {
        smemclr(kek, sizeof(kek));
        return NULL;
    }
    smemclr(kek, sizeof(kek));
    b64salt = base64_encode_sb(make_ptrlen(salt, sizeof(salt)), 0);
    b64blob = base64_encode_sb(make_ptrlen(blob, HELLO_BLOB_LEN), 0);
    /* ",c": this R is a CODE door (the sidecar-bound printout) - the
     * label distinction the door list needs; crypto-wise identical. */
    out = dupprintf("%s.R%u,%u,%u,%s,%s,c", container,
                    (unsigned)HELLO_ARGON_MEM, (unsigned)HELLO_ARGON_PASSES,
                    (unsigned)HELLO_ARGON_PARALLEL, b64salt->s, b64blob->s);
    strbuf_free(b64salt);
    strbuf_free(b64blob);
    smemclr(blob, sizeof(blob));
    return out;
}

/* Pull the PRF credential id out of the W field (caller sfree).
 * 1 = filled, 0 = no W field, -1 = malformed. */
int kitty_hello_container_prf_credid(const char *container,
                                     unsigned char **credid_out,
                                     size_t *credidlen_out)
{
    return kitty_hello_container_w_credid(container, 0, credid_out,
                                          credidlen_out);
}

int kitty_hello_container_open_prf(const char *container,
                                   const unsigned char prf_kek[32],
                                   unsigned char
                                       secret_out[KITTY_HELLO_SECRET_LEN])
{
    int i, n = kitty_hello_container_w_count(container);
    int ret = 0;

    /* Try every W: a KEK opens exactly the blob wrapped under it (the GCM
     * tag refuses the others), so no bookkeeping beyond the loop. */
    for (i = 0; i < n; i++) {
        size_t flen;
        const char *f = hello_container_field_n(container, 'W', i, &flen);
        ptrlen id, blob, owner;
        unsigned char raw[HELLO_BLOB_LEN];
        if (!f || !hello_w_parts(f, flen, &id, &blob, &owner) ||
            !hello_b64_fixed(blob.ptr, blob.len, raw, HELLO_BLOB_LEN)) {
            ret = -1;
            continue;
        }
        if (hello_gcm_unwrap(prf_kek, raw, secret_out)) {
            smemclr(raw, sizeof(raw));
            return 1;
        }
        smemclr(raw, sizeof(raw));
        ret = -1;
    }
    return ret;
}

/* The informational owner tag for a W written by this account:
 * DOMAIN\user@MACHINE. Caller sfree; never NULL. */
char *kitty_hello_owner_tag(void)
{
    char user[256 + 1], dom[256 + 1], host[256 + 1];
    DWORD ulen = sizeof(user), dlen = sizeof(dom), hlen = sizeof(host);
    if (!GetUserNameA(user, &ulen))
        strcpy(user, "?");
    if (!GetEnvironmentVariableA("USERDOMAIN", dom, dlen))
        strcpy(dom, "?");
    if (!GetComputerNameA(host, &hlen))
        strcpy(host, "?");
    return dupprintf("%s\%s@%s", dom, user, host);
}

char *kitty_hello_container_owners_text(const char *container)
{
    strbuf *sb = strbuf_new();
    int i, n = kitty_hello_container_w_count(container);
    for (i = 0; i < n; i++) {
        char *o = kitty_hello_container_w_owner(container, i);
        if (i)
            put_dataz(sb, ", ");
        put_dataz(sb, o ? o : "(untagged)");
        sfree(o);
    }
    if (kitty_hello_container_has_hello(container)) {
        if (n)
            put_dataz(sb, ", ");
        put_dataz(sb, "(Windows Hello key credential)");
    }
    return strbuf_to_str(sb);
}

int kitty_hello_prf_my_credid(unsigned char **credid_out,
                              size_t *credidlen_out)
{
    khw_api *api = khw_load();
    *credid_out = NULL;
    if (!api)
        return 0;
    return khw_find_credential(api, credid_out, credidlen_out);
}

int kitty_hello_container_my_w(const char *container)
{
    unsigned char *mine = NULL;
    size_t minelen = 0;
    int idx;
    if (kitty_hello_prf_my_credid(&mine, &minelen) != 1)
        return -1;
    idx = kitty_hello_container_find_w(container, mine, minelen);
    sfree(mine);
    return idx;
}

/*
 * The printed form of the secret (the BitLocker-recovery-key pattern):
 * 64 hex digits in 8 groups plus a 4-digit check group (the first two
 * bytes of SHA-256 over the secret), dash-separated. This same string
 * IS the PPK's literal passphrase, so a printout opens the key in ANY
 * PuTTY-compatible tool with no KiTTY code involved - which is why the
 * encoding is fixed for ever alongside the container marker.
 */
char *kitty_hello_secret_text(const unsigned char
                                  secret[KITTY_HELLO_SECRET_LEN])
{
    static const char hex[] = "0123456789ABCDEF";
    unsigned char check[32];
    strbuf *sb = strbuf_new_nm();
    int i;
    ssh_hash *h = ssh_hash_new(&ssh_sha256);

    put_data(h, secret, KITTY_HELLO_SECRET_LEN);
    ssh_hash_final(h, check);

    for (i = 0; i < KITTY_HELLO_SECRET_LEN; i++) {
        if (i > 0 && i % 4 == 0)
            put_byte(sb, '-');
        put_byte(sb, hex[secret[i] >> 4]);
        put_byte(sb, hex[secret[i] & 15]);
    }
    put_fmt(sb, "-%c%c%c%c", hex[check[0] >> 4], hex[check[0] & 15],
            hex[check[1] >> 4], hex[check[1] & 15]);
    smemclr(check, sizeof(check));
    return strbuf_to_str(sb);
}

/* Parse the printed form back: case-insensitive, dashes/spaces ignored,
 * check group verified. 1 = secret_out filled, 0 = not a valid printed
 * secret (wrong length, non-hex, or failed check). */
int kitty_hello_secret_from_text(const char *text,
                                 unsigned char
                                     secret_out[KITTY_HELLO_SECRET_LEN])
{
    unsigned char bytes[KITTY_HELLO_SECRET_LEN + 2];
    unsigned char check[32];
    int nibbles = 0;
    const char *p;
    bool ok;
    ssh_hash *h;

    for (p = text; *p; p++) {
        int v;
        if (*p == '-' || *p == ' ' || *p == '\t')
            continue;
        if (*p >= '0' && *p <= '9') v = *p - '0';
        else if (*p >= 'a' && *p <= 'f') v = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F') v = *p - 'A' + 10;
        else return 0;
        if (nibbles >= 2 * (int)sizeof(bytes))
            return 0;
        if (nibbles % 2 == 0)
            bytes[nibbles / 2] = (unsigned char)(v << 4);
        else
            bytes[nibbles / 2] |= (unsigned char)v;
        nibbles++;
    }
    if (nibbles != 2 * (int)sizeof(bytes))
        return 0;

    h = ssh_hash_new(&ssh_sha256);
    put_data(h, bytes, KITTY_HELLO_SECRET_LEN);
    ssh_hash_final(h, check);
    ok = (bytes[KITTY_HELLO_SECRET_LEN] == check[0] &&
          bytes[KITTY_HELLO_SECRET_LEN + 1] == check[1]);
    if (ok)
        memcpy(secret_out, bytes, KITTY_HELLO_SECRET_LEN);
    smemclr(bytes, sizeof(bytes));
    smemclr(check, sizeof(check));
    return ok ? 1 : 0;
}

/*
 * The wrap/unwrap POLICY: PRF where available, KCM as the fallback,
 * otherwise refuse (decided 2026-08-22). owner must be the calling
 * app's REAL window. These sit above both KEK sources and the
 * container, so apps need no knowledge of either mechanism.
 */
int kitty_hello_wrap_auto(HWND owner,
                          const unsigned char secret[KITTY_HELLO_SECRET_LEN],
                          const char *recovery_passphrase,
                          char **container_out, int *source_out)
{
    unsigned char kek[32];
    int ret;

    *container_out = NULL;
    if (source_out)
        *source_out = KITTY_HELLO_SOURCE_NONE;

    if (kitty_hello_prf_available() == 1) {
        unsigned char *credid = NULL;
        size_t credidlen = 0;
        ret = kitty_hello_prf_credential(owner, true, &credid, &credidlen);
        if (ret == KITTY_HELLO_VERIFIED) {
            ret = kitty_hello_prf_kek(owner, credid, credidlen, kek);
            if (ret == KITTY_HELLO_VERIFIED) {
                char *tag = kitty_hello_owner_tag();
                *container_out = kitty_hello_container_create_ex(
                    NULL, kek, credid, credidlen, tag, recovery_passphrase,
                    secret);
                sfree(tag);
                smemclr(kek, sizeof(kek));
                if (source_out)
                    *source_out = KITTY_HELLO_SOURCE_PRF;
            }
            smemclr(credid, credidlen);
            sfree(credid);
            if (ret == KITTY_HELLO_VERIFIED)
                return *container_out ? KITTY_HELLO_VERIFIED
                                      : KITTY_HELLO_ERROR;
            return ret;    /* a DENIED prompt is final, not a fallthrough */
        }
        if (ret != KITTY_HELLO_UNAVAILABLE)
            return ret;    /* the PRF layer FAILED: say so, do not start a
                            * second, unrelated prompt sequence on top */
        /* PRF layer unavailable after all: fall through to KCM. */
    }

    ret = kitty_hello_kek(kek, true);
    if (ret == KITTY_HELLO_VERIFIED) {
        *container_out = kitty_hello_container_create_ex(
            kek, NULL, NULL, 0, NULL, recovery_passphrase, secret);
        smemclr(kek, sizeof(kek));
        if (source_out)
            *source_out = KITTY_HELLO_SOURCE_KCM;
        return *container_out ? KITTY_HELLO_VERIFIED : KITTY_HELLO_ERROR;
    }
    smemclr(kek, sizeof(kek));
    return ret;
}

int kitty_hello_unwrap_auto(HWND owner, const char *container,
                            unsigned char
                                secret_out[KITTY_HELLO_SECRET_LEN])
{
    unsigned char kek[32];
    int ret;

    if (kitty_hello_container_has_prf(container)) {
        /* Only ever assert with a credential this account actually holds:
         * the W fields of other accounts/machines are not ours to try,
         * and asserting a foreign id would just fail noisily. */
        unsigned char *mine = NULL;
        size_t minelen = 0;
        int found = kitty_hello_prf_my_credid(&mine, &minelen);
        if (found == 1 &&
            kitty_hello_container_find_w(container, mine, minelen) >= 0) {
            ret = kitty_hello_prf_kek(owner, mine, minelen, kek);
            smemclr(mine, minelen);
            sfree(mine);
            if (ret == KITTY_HELLO_VERIFIED) {
                ret = (kitty_hello_container_open_prf(container, kek,
                                                      secret_out) == 1)
                      ? KITTY_HELLO_VERIFIED : KITTY_HELLO_ERROR;
            }
            smemclr(kek, sizeof(kek));
            return ret;
        }
        if (found == 1)
            sfree(mine);
        /* No W of ours here: fall through to a KCM wrap if any, else
         * UNAVAILABLE - the caller names the owners and offers the
         * passphrase doors. */
    }

    if (kitty_hello_container_has_hello(container)) {
        ret = kitty_hello_kek(kek, false);
        if (ret == KITTY_HELLO_VERIFIED) {
            ret = (kitty_hello_container_open_kek(container, kek,
                                                  secret_out) == 1)
                  ? KITTY_HELLO_VERIFIED : KITTY_HELLO_ERROR;
        }
        smemclr(kek, sizeof(kek));
        return ret;
    }

    return KITTY_HELLO_UNAVAILABLE;   /* no door of ours in this container */
}

int kitty_hello_enrol_auto(HWND owner, const char *container,
                           const unsigned char secret[KITTY_HELLO_SECRET_LEN],
                           char **container_out)
{
    unsigned char kek[32];
    unsigned char *credid = NULL;
    size_t credidlen = 0;
    int ret;

    *container_out = NULL;
    if (!kitty_hello_container_valid(container))
        return KITTY_HELLO_ERROR;
    if (kitty_hello_prf_available() != 1)
        return KITTY_HELLO_UNAVAILABLE;   /* enrolment is PRF-only */
    ret = kitty_hello_prf_credential(owner, true, &credid, &credidlen);
    if (ret != KITTY_HELLO_VERIFIED)
        return ret;
    if (kitty_hello_container_find_w(container, credid, credidlen) >= 0) {
        sfree(credid);
        *container_out = dupstr(container);   /* already enrolled */
        return KITTY_HELLO_VERIFIED;
    }
    ret = kitty_hello_prf_kek(owner, credid, credidlen, kek);
    if (ret == KITTY_HELLO_VERIFIED) {
        char *tag = kitty_hello_owner_tag();
        *container_out = kitty_hello_container_append_prf(
            container, kek, credid, credidlen, tag, secret);
        sfree(tag);
        if (!*container_out)
            ret = KITTY_HELLO_ERROR;
    }
    smemclr(kek, sizeof(kek));
    smemclr(credid, credidlen);
    sfree(credid);
    return ret;
}

/* A fresh secret from the OS CSPRNG. 1 = filled, 0 = the generator
 * failed (never use the buffer then). */
int kitty_hello_new_secret(unsigned char secret[KITTY_HELLO_SECRET_LEN])
{
    return hello_random(secret, KITTY_HELLO_SECRET_LEN) ? 1 : 0;
}
