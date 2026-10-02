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

struct ShinyDialogBoxState {
    bool ended;
    int result;
    ShinyDlgProc proc;
    void *ctx;
};

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

    sdb_tempstate = state;
    hwnd = CreateDialog(hinst, tmpl, hwndparent, ShinyRealDlgProc);
    SetWindowLongPtr(hwnd, DLGWINDOWEXTRA, (LONG_PTR)state);
    sdb_tempstate = NULL;

    while (!state->ended && !quit) {
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
            if (state->ended)
                break;
        }
    }

    if (quit) {
        PostQuitMessage((int)quitcode);   /* pass the WM_QUIT on */
        if (!state->ended)
            state->result = 0;            /* the box did not finish: a Cancel */
    }

    DestroyWindow(hwnd);
    return state->result;
}

void ShinyEndDialog(HWND hwnd, int ret)
{
    struct ShinyDialogBoxState *state = ShinyDialogGetState(hwnd);
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
