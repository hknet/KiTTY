/*
 * KiTTY: Windows Hello presence check - see kitty_hello.h.
 *
 * Plain-C WinRT: the MinGW headers carry the full C ABI for
 * UserConsentVerifier (statics interface, the parameterized async
 * operations and their IIDs), so only the Win32 interop interface - which
 * parents the prompt to one of our windows - is declared by hand below.
 * combase.dll is loaded dynamically and every failure maps to a value the
 * callers treat as a denial, so a system without WinRT simply reports
 * unavailable rather than failing to start.
 */

#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <roapi.h>
#include <winstring.h>
#include <inspectable.h>
#include <asyncinfo.h>
#include <windows.security.credentials.ui.h>

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

/* Get the UserConsentVerifier activation factory as IInspectable, or NULL.
 * Initializes WinRT on this thread; an already-initialized apartment of
 * either flavour is fine (S_FALSE / RPC_E_CHANGED_MODE). */
static IInspectable *hello_factory(void)
{
    static const WCHAR cls[] =
        L"Windows.Security.Credentials.UI.UserConsentVerifier";
    HSTRING hcls;
    IInspectable *factory = NULL;
    HRESULT hr;

    if (!hello_combase_load())
        return NULL;
    hr = fnRoInitialize(RO_INIT_SINGLETHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE_LOCAL)
        return NULL;
    if (FAILED(fnWindowsCreateString(
            cls, (UINT32)(sizeof(cls)/sizeof(cls[0]) - 1), &hcls)))
        return NULL;
    hr = fnRoGetActivationFactory(hcls, &IID_IInspectable, (void **)&factory);
    fnWindowsDeleteString(hcls);
    return SUCCEEDED(hr) ? factory : NULL;
}

/*
 * Wait for a WinRT async operation while keeping the UI alive. The Hello
 * prompt is system UI, but OUR window owns it, so the thread must pump or
 * the prompt cannot even paint. A hard deadline turns "the system never
 * answered" into a denial rather than a wedged agent; the operation is
 * cancelled so it cannot complete into freed state later.
 */
static int hello_wait(IUnknown *op_unknown, DWORD timeout_ms)
{
    IAsyncInfo *info = NULL;
    AsyncStatus st = Started;
    DWORD t0 = GetTickCount();
    int ok = 0;

    if (FAILED(IUnknown_QueryInterface(op_unknown, &IID_IAsyncInfo,
                                       (void **)&info)))
        return 0;
    for (;;) {
        MSG msg;
        if (FAILED(IAsyncInfo_get_Status(info, &st)))
            break;
        if (st != Started) {
            ok = (st == Completed);
            break;
        }
        if (GetTickCount() - t0 > timeout_ms) {
            IAsyncInfo_Cancel(info);
            /* keep pumping briefly so the cancel can settle */
            timeout_ms += 2000;
            if (GetTickCount() - t0 > timeout_ms)
                break;
        }
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        MsgWaitForMultipleObjects(0, NULL, FALSE, 50, QS_ALLINPUT);
    }
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
