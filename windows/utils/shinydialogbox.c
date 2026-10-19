/*
 * PuTTY's own reimplementation of DialogBox() and EndDialog() which
 * provide extra capabilities.
 *
 * Originally introduced in 2003 to work around a problem with our
 * message loops, in which keystrokes pressed in the 'Change Settings'
 * dialog in mid-session would _also_ be delivered to the main
 * terminal window.
 *
 * But once we had our own wrapper it was convenient to put further
 * things into it. In particular, this system allows you to provide a
 * context pointer at setup time that's easy to retrieve from the
 * window procedure.
 */

#include "putty.h"
#include <objbase.h>   /* KiTTY: CoInitializeEx for ShinyPumpCall's worker */

struct ShinyDialogBoxState {
    bool ended;
    int result;
    ShinyDlgProc proc;
    void *ctx;
    /* KiTTY: ShinyDialogBoxModeless - told the result, then the box goes */
    void (*done)(void *dctx, int result);
    void *dctx;
};

/* KiTTY: posted by ShinyEndDialog to a modeless box, which then ends itself
 * outside the procedure that asked it to */
#define WM_SHINY_MODELESS_END (WM_APP + 0x3A1)

/*
 * For use in re-entrant calls to the dialog procedure from
 * CreateDialog itself, temporarily store the state pointer that we
 * won't store in the usual window-memory slot until later.
 *
 * I don't _intend_ that this system will need to be used in multiple
 * threads at all, let alone concurrently, but just in case, declaring
 * sdb_tempstate as thread-local will protect against that possibility.
 */
static THREADLOCAL struct ShinyDialogBoxState *sdb_tempstate;

/*
 * KiTTY: the pumped box open on this thread (ShinyDialogBoxPumped), for the
 * boxes and pickers opened from INSIDE it - a confirmation, a file picker,
 * ChooseColor raised by a button of the mid-session "Change Settings" box.
 * A modal box of their own runs Windows' modal loop, which serves window
 * messages only, and the session behind stood still until it closed. With
 * this they wait through the pumped box's wait step instead (ShinyPumpUntil,
 * ShinyPumpCall below). Thread-local: a picker running on a worker thread of
 * its own never pumps another thread's waits. quit: a WM_QUIT taken off the
 * queue by one of those loops, for the pumped box to pass on.
 */
struct ShinyPump {
    struct ShinyDialogBoxState *state;
    HWND hwnd;
    void (*wait)(void);
    bool quit;
    WPARAM quitcode;
    bool held;          /* ShinyPumpCall runs: the box stays disabled */
};
static THREADLOCAL struct ShinyPump *sdb_pump;

static inline struct ShinyDialogBoxState *ShinyDialogGetState(HWND hwnd)
{
    if (sdb_tempstate)
        return sdb_tempstate;
    return (struct ShinyDialogBoxState *)GetWindowLongPtr(
        hwnd, DLGWINDOWEXTRA);
}

static INT_PTR CALLBACK ShinyRealDlgProc(HWND hwnd, UINT msg,
                                         WPARAM wParam, LPARAM lParam)
{
    struct ShinyDialogBoxState *state = ShinyDialogGetState(hwnd);
    if (!state)
        return FALSE;                  /* a modeless box after its end */
    /* KiTTY: the pumped box while a call of ShinyPumpCall runs: whoever
     * enables it (the shell's file dialog did, now and then, while it was
     * up), it is disabled again - the box must not take input under the
     * dialog opened from it */
    if (msg == WM_ENABLE && wParam && sdb_pump && sdb_pump->held &&
        sdb_pump->hwnd == hwnd) {
        EnableWindow(hwnd, FALSE);
        return TRUE;
    }
    if (msg == WM_SHINY_MODELESS_END && state->done) {
        void (*done)(void *, int) = state->done;
        void *dctx = state->dctx;
        int result = state->result;
        ShinyRemoveAuxDialog(hwnd);
        DestroyWindow(hwnd);           /* its messages still see the state */
        sfree(state);
        done(dctx, result);
        return TRUE;
    }
    return state->proc(hwnd, msg, wParam, lParam, state->ctx);
}

int ShinyDialogBox(HINSTANCE hinst, LPCTSTR tmpl, const char *winclass,
                   HWND hwndparent, ShinyDlgProc proc, void *ctx)
{
    /*
     * Register the window class ourselves in such a way that we
     * allocate an extra word of window memory to store the state
     * pointer.
     *
     * It would be nice in principle to load the dialog template
     * resource and dig the class name out of it. But DLGTEMPLATEEX is
     * a nasty variable-layout structure not declared conveniently in
     * a header file, so I think that's too much effort. Callers of
     * this function will just have to provide the right window class
     * name to match their template resource via the 'winclass'
     * parameter.
     */
    WNDCLASS wc;
    wc.style = CS_DBLCLKS | CS_SAVEBITS | CS_BYTEALIGNWINDOW;
    wc.lpfnWndProc = DefDlgProc;
    wc.cbClsExtra = 0;
    wc.cbWndExtra = DLGWINDOWEXTRA + sizeof(LONG_PTR);
    wc.hInstance = hinst;
    wc.hIcon = NULL;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH) (COLOR_BACKGROUND +1);
    wc.lpszMenuName = NULL;
    wc.lpszClassName = winclass;
    RegisterClass(&wc);

    struct ShinyDialogBoxState state[1];
    state->ended = false;
    state->proc = proc;
    state->ctx = ctx;
    state->done = NULL;
    state->dctx = NULL;

    sdb_tempstate = state;
    HWND hwnd = CreateDialog(hinst, tmpl, hwndparent, ShinyRealDlgProc);
    SetWindowLongPtr(hwnd, DLGWINDOWEXTRA, (LONG_PTR)state);
    sdb_tempstate = NULL;

    MSG msg;
    int gm;
    while ((gm = GetMessage(&msg, NULL, 0, 0)) > 0) {
        if (!state->ended && !IsDialogMessage(hwnd, &msg)) {
            /* KiTTY: keep dialog keyboard handling (Esc/Tab) working for the
             * modeless aux dialogs while this loop owns the thread's messages. */
            if (!ShinyAuxDialogMessage(&msg))
                DispatchMessage(&msg);
        }
        if (state->ended)
            break;
    }

    if (gm == 0)
        PostQuitMessage(msg.wParam); /* We got a WM_QUIT, pass it on */

    DestroyWindow(hwnd);
    return state->result;
}

/*
 * KiTTY: ShinyDialogBox for a box the terminal must keep running behind
 * ("Change Settings" mid-session). The plain loop above blocks in
 * GetMessage, which serves window messages only: network and helper events
 * (handle waits) and the queued callbacks wait until the box is closed, so
 * no output arrives meanwhile. Here wait() replaces that block - it services
 * what the terminal's main loop services and returns once a message may be
 * waiting (windows/window.c kitty_reconf_wait). It is passed in rather than
 * called, because this file sits below the libraries that know the handle
 * waits and the callbacks.
 */
int ShinyDialogBoxPumped(HINSTANCE hinst, LPCTSTR tmpl, const char *winclass,
                         HWND hwndparent, ShinyDlgProc proc, void *ctx,
                         void (*wait)(void))
{
    WNDCLASS wc;
    struct ShinyDialogBoxState state[1];
    struct ShinyPump pump, *outer = sdb_pump;
    HWND hwnd;
    MSG msg;
    bool quit = false;
    WPARAM quitcode = 0;

    wc.style = CS_DBLCLKS | CS_SAVEBITS | CS_BYTEALIGNWINDOW;
    wc.lpfnWndProc = DefDlgProc;
    wc.cbClsExtra = 0;
    wc.cbWndExtra = DLGWINDOWEXTRA + sizeof(LONG_PTR);
    wc.hInstance = hinst;
    wc.hIcon = NULL;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH) (COLOR_BACKGROUND +1);
    wc.lpszMenuName = NULL;
    wc.lpszClassName = winclass;
    RegisterClass(&wc);

    state->ended = false;
    state->proc = proc;
    state->ctx = ctx;
    state->done = NULL;
    state->dctx = NULL;

    sdb_tempstate = state;
    hwnd = CreateDialog(hinst, tmpl, hwndparent, ShinyRealDlgProc);
    SetWindowLongPtr(hwnd, DLGWINDOWEXTRA, (LONG_PTR)state);
    sdb_tempstate = NULL;

    pump.state = state;
    pump.hwnd = hwnd;
    pump.wait = wait;
    pump.quit = false;
    pump.quitcode = 0;
    pump.held = false;
    sdb_pump = &pump;

    while (!state->ended && !quit && !pump.quit) {
        int n = 0;
        wait();
        /* a handful of messages per round, so the events keep their turn */
        while (n++ < 16 && PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                quit = true;
                quitcode = msg.wParam;
                break;
            }
            if (!IsDialogMessage(hwnd, &msg) && !ShinyAuxDialogMessage(&msg))
                DispatchMessage(&msg);
            if (state->ended || pump.quit)
                break;
        }
    }

    sdb_pump = outer;
    if (pump.quit && !quit) {             /* taken by a box opened inside */
        quit = true;
        quitcode = pump.quitcode;
    }

    if (quit) {
        PostQuitMessage((int)quitcode);   /* pass the WM_QUIT on */
        if (!state->ended)
            state->result = 0;            /* the box did not finish: a Cancel */
    }

    DestroyWindow(hwnd);
    return state->result;
}

/* KiTTY: a pumped box is open on this thread (ShinyDialogBoxPumped) */
bool ShinyPumpActive(void)
{
    return sdb_pump != NULL;
}

/* KiTTY: that box, or NULL */
HWND ShinyPumpBox(void)
{
    return sdb_pump ? sdb_pump->hwnd : NULL;
}

/*
 * KiTTY: the message loop of a box opened inside the pumped box: the pumped
 * box's wait step, then a handful of messages per round, the box's own
 * dialog keys (Esc, Enter, Tab) through IsDialogMessage(dlg). Runs until
 * *done is set (true). Gives up (false) when the pumped box has ended under
 * it - the terminal was closed and took Change Settings with it - when dlg
 * is gone, or on WM_QUIT, which is kept for the pumped box to pass on.
 * Without a pumped box on this thread: false at once.
 */
bool ShinyPumpUntil(HWND dlg, volatile LONG *done)
{
    struct ShinyPump *pump = sdb_pump;
    MSG msg;

    if (!pump)
        return false;
    while (!*done) {
        int n = 0;
        if (pump->state->ended || pump->quit || (dlg && !IsWindow(dlg)))
            return false;
        pump->wait();
        while (n++ < 16 && !*done &&
               PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                pump->quit = true;
                pump->quitcode = msg.wParam;
                return false;
            }
            if (dlg && IsDialogMessage(dlg, &msg))
                continue;
            if (!ShinyAuxDialogMessage(&msg))
                DispatchMessage(&msg);
            if (pump->state->ended)
                break;
        }
    }
    return true;
}

/*
 * KiTTY: ShinyPumpCall - a blocking call (a common dialog: the file picker,
 * ChooseColor, ChooseFont) made from inside the pumped box. The call runs on
 * a worker thread of its own while this thread pumps, and returns when it
 * is done, so the caller sees it as synchronous as before. The worker's
 * dialog keeps its owner on this thread, which it disables as usual; the
 * worker posts a wake-up when it is done, since the wait step sleeps until
 * a message or an event comes. If the pumped box ends meanwhile, the
 * worker's windows get a WM_CLOSE (a Cancel) and the call is
 * still waited for - fn and ctx live on the caller's stack.
 */
struct ShinyPumpCallState {
    void (*fn)(void *ctx);
    void *ctx;
    DWORD caller;
    volatile LONG done;
};

static DWORD WINAPI shiny_pump_call_thread(void *vstate)
{
    struct ShinyPumpCallState *c = (struct ShinyPumpCallState *)vstate;
    DWORD caller = c->caller;   /* c is gone once done is set */
    /* COM for this thread: the file dialogs are shell windows */
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED |
                                COINIT_DISABLE_OLE1DDE);
    c->fn(c->ctx);
    if (SUCCEEDED(hr))
        CoUninitialize();
    InterlockedExchange(&c->done, 1);
    PostThreadMessage(caller, WM_NULL, 0, 0);
    return 0;
}

static BOOL CALLBACK shiny_pump_call_close(HWND hwnd, LPARAM lp)
{
    (void)lp;
    if (IsWindowVisible(hwnd))
        PostMessage(hwnd, WM_CLOSE, 0, 0);
    return TRUE;
}

void ShinyPumpCall(void (*fn)(void *ctx), void *ctx)
{
    struct ShinyPumpCallState c;
    HANDLE th;
    DWORD tid = 0;
    bool closing = false;
    HWND box;
    struct ShinyPump *held = NULL;

    if (!sdb_pump) {
        fn(ctx);
        return;
    }
    c.fn = fn;
    c.ctx = ctx;
    c.caller = GetCurrentThreadId();
    c.done = 0;
    /* The pumped box is disabled here, as a modal box would leave it: the
     * classic common dialogs (ChooseFont, ChooseColor) disable their owner
     * from the worker too, the shell's file dialog does not - and the
     * settings box stayed usable under it. BEFORE the worker starts: a
     * dialog that finds its owner disabled leaves it alone at both ends, so
     * this function alone enables it again (disabled after the worker had
     * started, the two raced over who enables it). */
    box = sdb_pump->hwnd;
    if (box && IsWindowEnabled(box)) {
        EnableWindow(box, FALSE);
        held = sdb_pump;
        held->held = true;               /* and kept so: ShinyRealDlgProc */
    } else
        box = NULL;
    th = CreateThread(NULL, 0, shiny_pump_call_thread, &c, 0, &tid);
    if (!th) {
        if (box) {
            held->held = false;
            EnableWindow(box, TRUE);
        }
        fn(ctx);                         /* no thread: the call as before */
        return;
    }
    while (!c.done) {
        if (!ShinyPumpUntil(NULL, &c.done) && !c.done) {
            /* the pumped box ended or WM_QUIT came: cancel the worker's
             * dialog once, then wait for it to come back */
            struct ShinyPump *pump = sdb_pump;
            MSG msg;
            if (!closing) {
                closing = true;
                EnumThreadWindows(tid, shiny_pump_call_close, 0);
            }
            pump->wait();
            while (!c.done && PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) {
                    pump->quit = true;
                    pump->quitcode = msg.wParam;
                    continue;
                }
                if (!ShinyAuxDialogMessage(&msg))
                    DispatchMessage(&msg);
            }
        }
    }
    WaitForSingleObject(th, INFINITE);
    CloseHandle(th);
    /* enabled again, and back in front: the dialog's owner was disabled
     * when it closed, so Windows may have activated another window */
    if (held)
        held->held = false;
    if (box && IsWindow(box)) {
        EnableWindow(box, TRUE);
        if (IsWindowVisible(box))
            SetActiveWindow(box);
    }
}

/*
 * KiTTY: ShinyDialogBox without a message loop of its own, for a question
 * asked while the terminal's session has to keep running (the SSH host key:
 * the server's close must be seen while the box is up). The box is shown and
 * the call returns; the caller's main loop drives it, and Esc, Tab and Enter
 * reach it as an aux dialog. When the procedure calls ShinyEndDialog,
 * done(dctx, result) runs once, after the box is gone. NULL: no box.
 */
HWND ShinyDialogBoxModeless(HINSTANCE hinst, LPCTSTR tmpl, const char *winclass,
                            HWND hwndparent, ShinyDlgProc proc, void *ctx,
                            void (*done)(void *dctx, int result), void *dctx)
{
    WNDCLASS wc;
    struct ShinyDialogBoxState *state;
    HWND hwnd;

    wc.style = CS_DBLCLKS | CS_SAVEBITS | CS_BYTEALIGNWINDOW;
    wc.lpfnWndProc = DefDlgProc;
    wc.cbClsExtra = 0;
    wc.cbWndExtra = DLGWINDOWEXTRA + sizeof(LONG_PTR);
    wc.hInstance = hinst;
    wc.hIcon = NULL;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH) (COLOR_BACKGROUND +1);
    wc.lpszMenuName = NULL;
    wc.lpszClassName = winclass;
    RegisterClass(&wc);

    state = snew(struct ShinyDialogBoxState);
    state->ended = false;
    state->result = 0;
    state->proc = proc;
    state->ctx = ctx;
    state->done = done;
    state->dctx = dctx;

    sdb_tempstate = state;
    hwnd = CreateDialog(hinst, tmpl, hwndparent, ShinyRealDlgProc);
    sdb_tempstate = NULL;
    if (!hwnd) {
        sfree(state);
        return NULL;
    }
    SetWindowLongPtr(hwnd, DLGWINDOWEXTRA, (LONG_PTR)state);
    ShinyAddAuxDialog(hwnd);
    if (state->ended)                  /* ended from WM_INITDIALOG already */
        PostMessage(hwnd, WM_SHINY_MODELESS_END, 0, 0);
    return hwnd;
}

void ShinyEndDialog(HWND hwnd, int ret)
{
    struct ShinyDialogBoxState *state = ShinyDialogGetState(hwnd);
    if (state->done) {
        /* modeless: the first end counts; the box goes from its own
         * message, not from inside the procedure asking for it */
        if (!state->ended) {
            state->result = ret;
            state->ended = true;
            PostMessage(hwnd, WM_SHINY_MODELESS_END, 0, 0);
        }
        return;
    }
    state->result = ret;
    state->ended = true;
}

/* KiTTY: the modeless aux dialogs (the About box, the /help command list, the
 * input box, the title placeholders, the two OSC 5113 permission requests).
 * Message loops route messages through IsDialogMessage() for each so Esc/Tab
 * keep working; lives here (bottom-most lib) so both window.c's pump and the
 * loop above see them. A dialog unregisters itself on WM_DESTROY; the
 * IsWindow() checks are only a safety net against a missed removal. The table
 * is a fixed size and a registration beyond it is DROPPED - silently, and the
 * dialog that lost it simply stops answering Esc and Tab - so it has room to
 * spare over the number of windows that can be open together. */
#define SHINY_MAX_AUX_DIALOGS 8
static HWND shiny_aux_dialogs[SHINY_MAX_AUX_DIALOGS];

void ShinyAddAuxDialog(HWND hwnd)
{
    int i;
    for (i = 0; i < SHINY_MAX_AUX_DIALOGS; i++)
        if (shiny_aux_dialogs[i] == hwnd)
            return;
    for (i = 0; i < SHINY_MAX_AUX_DIALOGS; i++) {
        if (!shiny_aux_dialogs[i] || !IsWindow(shiny_aux_dialogs[i])) {
            shiny_aux_dialogs[i] = hwnd;
            return;
        }
    }
}

void ShinyRemoveAuxDialog(HWND hwnd)
{
    int i;
    for (i = 0; i < SHINY_MAX_AUX_DIALOGS; i++)
        if (shiny_aux_dialogs[i] == hwnd)
            shiny_aux_dialogs[i] = NULL;
}

bool ShinyAuxDialogMessage(MSG *msg)
{
    int i;
    for (i = 0; i < SHINY_MAX_AUX_DIALOGS; i++) {
        HWND aux = shiny_aux_dialogs[i];
        if (aux && IsWindow(aux) && IsDialogMessage(aux, msg))
            return true;
    }
    return false;
}
