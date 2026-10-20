/*
 * KiTTY master-password GUI prompt (DPAPI Phase 2/3 UX).
 *
 * Registers a modal passphrase dialog with the storage layer (windows/storage.c
 * via kitty_set_master_pw_prompt) so the master-password ("MPW1") at-rest scheme
 * works interactively, without the user having to manage a -masterpwfile.
 *
 * The storage layer calls back here the first time a saved secret actually needs
 * the master key (lazy unlock). creating!=0 means the store has no master
 * password yet (first-time setup) -> we show a confirm field and require a match;
 * creating==0 means unlock an existing store -> single field. The storage layer
 * owns the verifier check and the retry/decline state machine; we just collect
 * one UTF-8 passphrase per call (NULL == the user cancelled).
 *
 * Where the dialog runs (gui_master_pw_prompt): inside Change Settings it
 * waits through that box's pumped loop (kitty_dialog_box); on a terminal's
 * thread anywhere else it runs on a worker thread while the terminal's
 * thread keeps serving the session (mpw_live_*); with no terminal (the
 * configuration window at start, the launcher) it is modal, as before.
 *
 * Linked only into the GUI targets (kitty / kitty_portable). CLI tools
 * (plink/pscp/...) don't link it, so they never pop a modal -> MPW is simply
 * unavailable there (matches the "-batch must fail-fast, no modal" rule).
 */
#include <winsock2.h>   /* before windows.h: putty.h pulls it in further down */
#include <windows.h>
#include <string.h>
#include "kitty_rc_additions.h"   /* IDD_MASTERPW, IDC_MPW_* */
#include "kitty_msgbox.h"   /* themed MessageBox routing */
#include "kitty_text.h"     /* shared captions */
#include "kitty_inikeys.h"  /* KI_*: the kitty.ini key names */
#include "kitty_inifile.h"  /* kitty_inifile_write_blocked: readonly=yes */
#include "kitty_storage.h"
#include "kitty_secretstore.h"
#include "kitty_win.h"
#include "kitty_dlgbox.h"
#include "kitty.h"
#include "kitty_common.h"
#include "mini/mini.h"
#include "kitty_mpw.h"
#include "kitty_theme.h"    /* the worker's dialog: themed and its red line inked */
#include "kitty_notice.h"   /* the locked-save report in a terminal */
#include "kitty_params.h"   /* GetNoticeSeconds */
#include "kitty_winutil.h"  /* debug_logevent: the terminal's Event Log */
#include "kitty_registry.h" /* kitty_reach, KittyCliReport */

/* windows/window.c: the terminal's wait step (handle waits, callbacks) */
void kitty_reconf_wait(void);

/* The red line under the fields: made at run time, the first time there is
 * something to show (a wrong password, an empty field, a mismatch). */
#ifndef IDC_MPW_STATUS
#define IDC_MPW_STATUS 1110
#endif
/* The setup's third choice, made at run time beside OK and Cancel:
 * PortablePasswordProtection=dpapi for the store, the password DPAPI1. */
#ifndef IDC_MPW_DPAPI
#define IDC_MPW_DPAPI 1111
#endif
/* mpw_live_run: sent messages only (winuser.h has it for 0x0500 and up) */
#ifndef PM_QS_SENDMESSAGE
#define PM_QS_SENDMESSAGE (QS_SENDMESSAGE << 16)
#endif

/* ---- a box beside a running session ---------------------------------------
 * The box on a worker thread of its own, owned by the terminal (which it
 * disables, as a modal box would); the terminal's thread waits in the
 * terminal's own wait step (kitty_reconf_wait: network, helpers, callbacks)
 * and dispatches its messages, so the session runs while the box stands. The
 * answer still comes back to the caller - the storage layer's read or write
 * that needed it waits for it, as before. Each box's context starts with this. */
typedef struct mpw_live mpw_live;
struct mpw_live {
    volatile LONG done;         /* answered or cancelled */
    DWORD wake;                 /* the thread waiting for done, woken by WM_NULL */
    HWND owner;                 /* the terminal (worker box) */
    DWORD tid;                  /* the worker thread, 0 = none */
    int worker;                 /* runs on a worker thread */
    int tmpl;                   /* the box the worker runs ... */
    DLGPROC proc;               /* ... with its procedure */
    INT_PTR result;             /* how the worker's box ended */
    int pumped;                 /* inside Change Settings (mpw_pumped_box) */
    HWND hdlg;                  /* the box, once made */
    mpw_live *next;             /* the worker and pumped boxes standing now */
};
static mpw_live *g_live_boxes;

/* One prompt: what it shows, and its answer. The dialog procedure reads it
 * from the window (DWLP_USER), so two of them never share state. */
typedef struct {
    mpw_live live;
    int first_time;             /* show + require the confirm field */
    const char *prompt;         /* another store's import: its own text */
    int try_no, try_max;        /* try_no > 1: the try before was wrong */
    char *result;               /* collected UTF-8 passphrase (malloc'd) or NULL */
    int use_dpapi;              /* setup: "Use DPAPI" pressed */
} mpw_dlg_t;

/* The prompt standing on this process (one at a time): a second request
 * waits for its answer (gui_master_pw_prompt). */
static mpw_dlg_t *g_mpw_open;

/* Read an edit control as a malloc'd UTF-8 string (matches the UTF-8 password
 * store). The wide buffer is scrubbed before release. */
static char *utf8_from_edit(HWND hdlg, int id)
{
    HWND h = GetDlgItem(hdlg, id);
    int wlen = GetWindowTextLengthW(h);
    WCHAR *w = (WCHAR *)malloc((size_t)(wlen + 1) * sizeof(WCHAR));
    if (!w)
        return NULL;
    GetWindowTextW(h, w, wlen + 1);
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s = (char *)malloc(n > 0 ? (size_t)n : 1);
    if (s)
        WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    SecureZeroMemory(w, (size_t)(wlen + 1) * sizeof(WCHAR));
    free(w);
    return s;
}

static void move_ctrl_up(HWND hdlg, int id, int dy)
{
    HWND h = GetDlgItem(hdlg, id);
    RECT r;
    POINT p;
    GetWindowRect(h, &r);
    p.x = r.left;
    p.y = r.top - dy;
    ScreenToClient(hdlg, &p);
    SetWindowPos(h, NULL, p.x, p.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
}

/* The answer is in: whoever waits for it (a second request, the worker's
 * caller) goes on. Set before the box is gone - a request waiting inside the
 * box's own loop sees it there. */
static void mpw_live_finish(HWND hdlg, mpw_live *l, INT_PTR r)
{
    if (l) {
        InterlockedExchange(&l->done, 1);
        if (l->wake && l->wake != GetCurrentThreadId())
            PostThreadMessage(l->wake, WM_NULL, 0, 0);
    }
    kitty_dialog_end(hdlg, r);   /* EndDialog, or the pumped box's end */
}

/* The red line: under the last field shown; the buttons and the box move
 * down by one label row the first time. No box of its own - a message box
 * on the worker's thread would not take the theme. */
static void mpw_show_line(HWND hdlg, mpw_dlg_t *d, const char *text)
{
    HWND h = GetDlgItem(hdlg, IDC_MPW_STATUS);
    if (!h) {
        HWND last = GetDlgItem(hdlg, d && d->first_time ? IDC_MPW_CONFIRM : IDC_MPW_EDIT);
        RECT rl, r2, rf, rw;
        POINT p;
        int row, lh;
        GetWindowRect(GetDlgItem(hdlg, IDC_MPW_CONFIRM_LBL), &rl);
        GetWindowRect(GetDlgItem(hdlg, IDC_MPW_CONFIRM), &r2);
        GetWindowRect(last, &rf);
        row = r2.top - rl.top;                    /* a label's row */
        lh = rl.bottom - rl.top;
        p.x = rf.left;
        p.y = rf.bottom + (row - lh);
        ScreenToClient(hdlg, &p);
        h = CreateWindowExA(0, "STATIC", "", WS_CHILD | SS_LEFT | SS_NOPREFIX,
                            p.x, p.y, rf.right - rf.left, lh, hdlg,
                            (HMENU)(INT_PTR)IDC_MPW_STATUS,
                            GetModuleHandle(NULL), NULL);
        if (!h) {
            MessageBeep(MB_ICONWARNING);
            return;
        }
        SendMessage(h, WM_SETFONT, (WPARAM)SendMessage(hdlg, WM_GETFONT, 0, 0), FALSE);
        kitty_theme_mark_ink(h, KITTY_INK_BAD);
        move_ctrl_up(hdlg, IDOK, -row);
        move_ctrl_up(hdlg, IDCANCEL, -row);
        if (GetDlgItem(hdlg, IDC_MPW_DPAPI))
            move_ctrl_up(hdlg, IDC_MPW_DPAPI, -row);
        GetWindowRect(hdlg, &rw);
        SetWindowPos(hdlg, NULL, 0, 0, rw.right - rw.left,
                     (rw.bottom - rw.top) + row, SWP_NOMOVE | SWP_NOZORDER);
    }
    SetWindowTextA(h, text);
    ShowWindow(h, SW_SHOW);
    InvalidateRect(h, NULL, TRUE);
}

static INT_PTR CALLBACK mpw_dlgproc(HWND hdlg, UINT msg, WPARAM wp, LPARAM lp)
{
    mpw_dlg_t *d = (mpw_dlg_t *)GetWindowLongPtr(hdlg, DWLP_USER);
    switch (msg) {
      case WM_INITDIALOG:
        d = (mpw_dlg_t *)lp;
        SetWindowLongPtr(hdlg, DWLP_USER, (LONG_PTR)d);
        if (d)
            d->live.hdlg = hdlg;
        {
            /* The prompt as tall as its text at the box's font (kitty_fit_text:
             * pixels, so any DPI and any length fit); what is below moves
             * with it and the box grows or shrinks by the same. */
            static const int below[] = { IDC_MPW_EDIT_LBL, IDC_MPW_EDIT,
                                         IDC_MPW_CONFIRM_LBL, IDC_MPW_CONFIRM,
                                         IDOK, IDCANCEL, 0 };
            const char *text = d && d->prompt ? d->prompt :
                               d && d->first_time ? KT_MPW_PROMPT_SET
                                                  : KT_MPW_PROMPT_UNLOCK;
            RECT rw;
            int dh, i;
            SetDlgItemTextA(hdlg, IDC_MPW_PROMPT, text);
            dh = kitty_fit_text(hdlg, IDC_MPW_PROMPT, text, 0);
            if (dh) {
                for (i = 0; below[i]; i++)
                    move_ctrl_up(hdlg, below[i], -dh);
                GetWindowRect(hdlg, &rw);
                SetWindowPos(hdlg, NULL, 0, 0, rw.right - rw.left,
                             (rw.bottom - rw.top) + dh, SWP_NOMOVE | SWP_NOZORDER);
            }
        }
        if (!d || !d->first_time) {
            /* Unlock mode (and another store's import): one field - drop the
             * confirm row, moving the buttons up and shrinking the dialog
             * to match. */
            HWND e1 = GetDlgItem(hdlg, IDC_MPW_EDIT);
            HWND e2 = GetDlgItem(hdlg, IDC_MPW_CONFIRM);
            RECT r1, r2, rw;
            int dy;
            GetWindowRect(e1, &r1);
            GetWindowRect(e2, &r2);
            dy = r2.top - r1.top;                 /* one full label+edit row */
            ShowWindow(e2, SW_HIDE);
            ShowWindow(GetDlgItem(hdlg, IDC_MPW_CONFIRM_LBL), SW_HIDE);
            EnableWindow(e2, FALSE);
            move_ctrl_up(hdlg, IDOK, dy);
            move_ctrl_up(hdlg, IDCANCEL, dy);
            GetWindowRect(hdlg, &rw);
            SetWindowPos(hdlg, NULL, 0, 0, rw.right - rw.left,
                         (rw.bottom - rw.top) - dy,
                         SWP_NOMOVE | SWP_NOZORDER);
        }
        if (d && d->first_time && !d->prompt) {
            /* setup: "Use DPAPI" on the buttons' row, at the fields' left */
            RECT ro, re;
            POINT p;
            HWND b;
            GetWindowRect(GetDlgItem(hdlg, IDOK), &ro);
            GetWindowRect(GetDlgItem(hdlg, IDC_MPW_EDIT), &re);
            p.x = re.left;
            p.y = ro.top;
            ScreenToClient(hdlg, &p);
            b = CreateWindowExA(0, "BUTTON", KT_MPW_USE_DPAPI,
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                p.x, p.y, ro.right - ro.left, ro.bottom - ro.top,
                                hdlg, (HMENU)(INT_PTR)IDC_MPW_DPAPI,
                                GetModuleHandle(NULL), NULL);
            if (b) {
                int w;
                SendMessage(b, WM_SETFONT, (WPARAM)SendMessage(hdlg, WM_GETFONT, 0, 0), FALSE);
                w = kitty_theme_button_width(b, ro.right - ro.left);
                if (w > ro.left - re.left - (ro.bottom - ro.top) / 2)
                    w = ro.left - re.left - (ro.bottom - ro.top) / 2;
                SetWindowPos(b, NULL, 0, 0, w, ro.bottom - ro.top,
                             SWP_NOMOVE | SWP_NOZORDER);
            }
        }
        if (d && d->try_no > 1) {                 /* the try before was wrong */
            char line[160];
            snprintf(line, sizeof(line), KT_MPW_WRONG_TRY_FMT,
                     d->try_no, d->try_max);
            mpw_show_line(hdlg, d, line);
        }
        /* the worker's thread has no dialog hook of its own: the theme and
         * the subclass that inks the red line, as the hook gives them - after
         * the controls made here, so they take it too */
        if (d && d->live.worker)
            kitty_theme_apply_hooked(hdlg);
        SetForegroundWindow(hdlg);
        SetFocus(GetDlgItem(hdlg, IDC_MPW_EDIT));
        {
            kitty_centre_on_owner(hdlg);              /* over the owner window */
        }
        return FALSE;                             /* we set focus ourselves */

      case WM_COMMAND:
        switch (LOWORD(wp)) {
          case IDOK: {
            char *p1 = utf8_from_edit(hdlg, IDC_MPW_EDIT);
            if (!p1 || !p1[0]) {
                mpw_show_line(hdlg, d, KT_MPW_EMPTY);
                if (p1) free(p1);
                SetFocus(GetDlgItem(hdlg, IDC_MPW_EDIT));
                return TRUE;
            }
            if (d && d->first_time) {
                char *p2 = utf8_from_edit(hdlg, IDC_MPW_CONFIRM);
                int mismatch = (!p2 || strcmp(p1, p2) != 0);
                if (p2) { SecureZeroMemory(p2, strlen(p2)); free(p2); }
                if (mismatch) {
                    mpw_show_line(hdlg, d, KT_MPW_MISMATCH);
                    SecureZeroMemory(p1, strlen(p1)); free(p1);
                    SetDlgItemTextA(hdlg, IDC_MPW_CONFIRM, "");
                    SetFocus(GetDlgItem(hdlg, IDC_MPW_CONFIRM));
                    return TRUE;
                }
            }
            if (d && !d->live.done)
                d->result = p1;
            else {
                SecureZeroMemory(p1, strlen(p1)); free(p1);
            }
            mpw_live_finish(hdlg, d ? &d->live : NULL, IDOK);
            return TRUE;
          }
          case IDC_MPW_DPAPI:
            /* the store's policy is written by the caller, on its thread */
            if (d && !d->live.done)
                d->use_dpapi = 1;
            mpw_live_finish(hdlg, d ? &d->live : NULL, IDCANCEL);
            return TRUE;
          case IDCANCEL:
            mpw_live_finish(hdlg, d ? &d->live : NULL, IDCANCEL);
            return TRUE;
        }
        break;

      case WM_CLOSE:
        mpw_live_finish(hdlg, d ? &d->live : NULL, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

/* The terminal of this thread, or NULL: the window window.c keeps as
 * MainHwnd, holding its seat (the launcher's own window holds none). */
static HWND mpw_live_owner(void)
{
    HWND h = GetMainHwnd();
    if (!h || !IsWindow(h) ||
        GetWindowThreadProcessId(h, NULL) != GetCurrentThreadId() ||
        !GetWindowLongPtr(h, GWLP_USERDATA))
        return NULL;
    return h;
}

static BOOL CALLBACK mpw_live_close(HWND hwnd, LPARAM lp)
{
    (void)lp;
    if (IsWindowVisible(hwnd))
        PostMessage(hwnd, WM_CLOSE, 0, 0);
    return TRUE;
}

/* The session behind ended (windows/window.c close_session): a box standing
 * beside it is cancelled - a prompt's caller gets "locked", the migration
 * question "keep". On a worker thread every window of that thread; inside
 * Change Settings the box alone (its thread is the terminal's), and Change
 * Settings goes on as after a Cancel. */
void kitty_mpw_session_ended(void)
{
    mpw_live *l;
    for (l = g_live_boxes; l; l = l->next)
        if (l->worker && l->tid && !l->done)
            EnumThreadWindows(l->tid, mpw_live_close, 0);
        else if (l->pumped && !l->done && l->hdlg && IsWindow(l->hdlg))
            PostMessage(l->hdlg, WM_CLOSE, 0, 0);
}

/* The box through kitty_dialog_box: inside Change Settings (pumped) it is
 * listed with the worker boxes while it stands, so the session's end
 * cancels it too; elsewhere modal, as before. */
static INT_PTR mpw_pumped_box(mpw_live *l, HWND owner, LPARAM lp)
{
    mpw_live **pp;
    INT_PTR r;
    int pumped = ShinyPumpActive();
    if (pumped) {
        l->pumped = 1;
        l->next = g_live_boxes;
        g_live_boxes = l;
    }
    r = kitty_dialog_box(owner, l->tmpl, l->proc, lp);
    if (pumped) {
        for (pp = &g_live_boxes; *pp; pp = &(*pp)->next)
            if (*pp == l) {
                *pp = l->next;
                break;
            }
        l->pumped = 0;
    }
    l->hdlg = NULL;
    return r;
}

/* Serve the terminal until the box l is answered. The terminal closed or a
 * WM_QUIT came meanwhile: the box is cancelled (once), still waited for, and
 * the WM_QUIT passed on after. */
static void mpw_live_pump(mpw_live *l)
{
    int closing = 0, quit = 0;
    WPARAM quitcode = 0;
    MSG msg;
    while (!l->done) {
        int n = 0;
        if (!closing && (quit || !IsWindow(l->owner))) {
            closing = 1;
            if (l->tid)
                EnumThreadWindows(l->tid, mpw_live_close, 0);
        }
        kitty_reconf_wait();
        /* a handful of messages per round, so the events keep their turn */
        while (n++ < 16 && !l->done &&
               PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                quit = 1;
                quitcode = msg.wParam;
                break;
            }
            if (!ShinyAuxDialogMessage(&msg))
                DispatchMessage(&msg);
        }
    }
    if (quit)
        PostQuitMessage((int)quitcode);
}

static DWORD WINAPI mpw_live_thread(void *v)
{
    mpw_live *l = (mpw_live *)v;
    INT_PTR r = DialogBoxParamA(GetModuleHandle(NULL),
                                MAKEINTRESOURCEA(l->tmpl), l->owner,
                                l->proc, (LPARAM)l);
    /* l lives until the caller has waited for this thread */
    l->result = r;
    if (r == -1 || !l->done) {                   /* no box, or gone unanswered */
        InterlockedExchange(&l->done, 1);
        PostThreadMessage(l->wake, WM_NULL, 0, 0);
    }
    return 0;
}

/* 1 = the box ran on the worker (answered or cancelled; l->result); 0 = no
 * thread: the caller shows it here */
static int mpw_live_run(mpw_live *l, HWND owner)
{
    HANDLE th;
    mpw_live **pp;
    l->owner = owner;
    l->worker = 1;
    l->wake = GetCurrentThreadId();
    l->result = IDCANCEL;
    th = CreateThread(NULL, 0, mpw_live_thread, l, 0, &l->tid);
    if (!th) {
        l->worker = 0;
        l->tid = 0;
        return 0;
    }
    l->next = g_live_boxes;
    g_live_boxes = l;
    mpw_live_pump(l);
    /* done is set before the worker's EndDialog, which then re-enables and
     * activates the terminal - messages sent to this thread. A plain wait
     * for the worker here deadlocks with it (the box stays, the terminal
     * hangs): serve sent messages until the worker has ended. */
    while (MsgWaitForMultipleObjects(1, &th, FALSE, INFINITE, QS_SENDMESSAGE)
           == WAIT_OBJECT_0 + 1) {
        MSG msg;
        PeekMessage(&msg, NULL, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
    }
    CloseHandle(th);
    for (pp = &g_live_boxes; *pp; pp = &(*pp)->next)
        if (*pp == l) {
            *pp = l->next;
            break;
        }
    return 1;
}

/* Storage-layer callback: returns the UTF-8 master passphrase (caller frees) or
 * NULL if the user cancelled. The storage layer derives + verifies and re-calls
 * us (with creating==0) on a wrong password, so we do not loop here. */
static char *gui_master_pw_prompt(int creating)
{
    mpw_dlg_t d, *outer = g_mpw_open;
    HWND owner, live;
    char *res;

    /* One prompt at a time. A second request comes while the first stands
     * (the terminal's events and Change Settings' loop run meanwhile): it
     * waits for the same answer and gets a copy of it. A modal box's own
     * loop cannot be waited in: that one is cancelled. So is a request
     * that comes after the answer (a second request whose copy was wrong
     * prompting again): only the first request's own next prompt shows. */
    if (outer) {
        if (outer->live.done)
            return NULL;
        if (outer->live.worker && GetCurrentThreadId() == outer->live.wake)
            mpw_live_pump(&outer->live);
        else if (ShinyPumpActive())
            ShinyPumpUntil(NULL, &outer->live.done);
        if (!outer->live.done || !outer->result)
            return NULL;
        return dupstr(outer->result);
    }

    memset(&d, 0, sizeof(d));
    d.first_time = creating;
    d.live.tmpl = IDD_MASTERPW;
    d.live.proc = mpw_dlgproc;
    kitty_mpw_prompt_try(&d.try_no, &d.try_max);
    g_mpw_open = &d;
    live = ShinyPumpActive() ? NULL : mpw_live_owner();
    if (!live || !mpw_live_run(&d.live, live)) {
        owner = GetActiveWindow();
        if (!owner)
            owner = GetForegroundWindow();
        d.live.wake = GetCurrentThreadId();
        /* from Change Settings mid-session (a Save): the session keeps
         * running while the box waits (kitty_dialog_box), and its end
         * cancels the box; else modal */
        if (mpw_pumped_box(&d.live, owner, (LPARAM)&d) != IDOK) {
            if (d.result) {
                SecureZeroMemory(d.result, strlen(d.result));
                free(d.result);
                d.result = NULL;
            }
        }
    }
    g_mpw_open = NULL;
    if (d.use_dpapi && !d.result) {
        /* "Use DPAPI": the store's policy from now on, in its kitty.ini and
         * in this process at once; the save that needed it then wraps DPAPI1
         * (ksec_protect_portable_x), and no master password is created;
         * readonly=yes: this process only */
        char *ini = GetKittyIniFile();
        if (ini && ini[0] && !kitty_inifile_write_blocked(ini))
            writeINI(ini, KI_SECTION_KITTY, KI_PORTABLEPASSWORDPROTECTION, "dpapi");
        kitty_set_portable_password_protection("dpapi");
    }
    res = d.result;
    return res;
}

/*
 * The same dialog, opened by the folder IMPORT for the master password of the
 * store being imported. Only the widget is shared: the answer goes back to the
 * import, which keeps and uses it itself - nothing here touches this store's
 * unlock state. malloc'd UTF-8, or NULL if cancelled.
 */
char *kitty_mpw_gui_ask_import(HWND owner, const char *prompt)
{
    mpw_dlg_t d;
    if (!owner) owner = GetActiveWindow();
    memset(&d, 0, sizeof(d));
    d.prompt = prompt;
    d.live.wake = GetCurrentThreadId();
    if (kitty_dialog_box(owner, IDD_MASTERPW, mpw_dlgproc, (LPARAM)&d) != IDOK) {
        if (d.result) {
            SecureZeroMemory(d.result, strlen(d.result));
            free(d.result);
            d.result = NULL;
        }
    }
    return d.result;
}

/* ---- legacy->protected migration consent (portable saves) ----------------
 * The storage layer calls back here before it rewrites a password that is
 * stored in the old unprotected form into a protected one (one way: an old
 * KiTTY 0.76 cannot read it afterwards, so the question comes first). Returns 1 =
 * re-encrypt, 0 = keep the stored value as-is. Default is to prompt; kitty.ini
 * [KiTTY] WarnLegacyPasswordUpgrade=no (also settable via the dialog's
 * checkbox, persisted only with Yes) skips the dialog and migrates silently.
 * CLI tools never register this callback, so batch saves keep the old form.
 * Where it runs: as the master password prompt (gui_master_pw_prompt). */

typedef struct {
    mpw_live live;
    int noask;
} mig_dlg_t;
static int g_mig_open;          /* one at a time: a second save keeps */

static INT_PTR CALLBACK migwarn_dlgproc(HWND hdlg, UINT msg, WPARAM wp, LPARAM lp)
{
    mig_dlg_t *m = (mig_dlg_t *)GetWindowLongPtr(hdlg, DWLP_USER);
    switch (msg) {
      case WM_INITDIALOG:
        m = (mig_dlg_t *)lp;
        SetWindowLongPtr(hdlg, DWLP_USER, (LONG_PTR)m);
        if (m)
            m->live.hdlg = hdlg;
        if (m && m->live.worker)
            kitty_theme_apply_hooked(hdlg);       /* no dialog hook there */
        SetForegroundWindow(hdlg);
        return TRUE;
      case WM_COMMAND:
        switch (LOWORD(wp)) {
          case IDOK:
            if (m)
                m->noask =
                    (IsDlgButtonChecked(hdlg, IDC_MIG_NOASK) == BST_CHECKED);
            mpw_live_finish(hdlg, m ? &m->live : NULL, IDOK);
            return TRUE;
          case IDCANCEL:
            mpw_live_finish(hdlg, m ? &m->live : NULL, IDCANCEL);
            return TRUE;
        }
        break;
      case WM_CLOSE:
        mpw_live_finish(hdlg, m ? &m->live : NULL, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

static int gui_legacy_migrate_warn(void)
{
    char buf[16] = "";
    char *ini = get_param_str("INI");
    mig_dlg_t m;
    HWND live;
    INT_PTR r;
    if (ini && readINI(ini, KI_SECTION_KITTY, KI_WARNLEGACYPASSWORDUPGRADE,
                       buf, sizeof(buf)) &&
        (!_stricmp(buf, "no") || !_stricmp(buf, "0")))
        return 1;                       /* warning opted out: migrate silently */
    if (g_mig_open)
        return 0;
    memset(&m, 0, sizeof(m));
    m.live.tmpl = IDD_MIGRATEWARN;
    m.live.proc = migwarn_dlgproc;
    g_mig_open = 1;
    /* beside a running session: on a worker (mpw_live_run); inside Change
     * Settings: kitty_dialog_box keeps the session running; else modal */
    live = ShinyPumpActive() ? NULL : mpw_live_owner();
    if (live && mpw_live_run(&m.live, live))
        r = m.live.result;
    else {
        HWND owner = GetActiveWindow();
        if (!owner)
            owner = GetForegroundWindow();
        m.live.wake = GetCurrentThreadId();
        r = mpw_pumped_box(&m.live, owner, (LPARAM)&m);
    }
    g_mig_open = 0;
    if (r == IDOK) {
        if (m.noask && ini && !kitty_inifile_write_blocked(ini))
            writeINI(ini, KI_SECTION_KITTY, KI_WARNLEGACYPASSWORDUPGRADE, "no");
        return 1;
    }
    return 0;
}

/* The master password's salt or verifier could not be stored (a save had just
 * set it up): the save keeps what was stored - report that. Beside a running
 * session or the configuration window, a modeless box (a modal one would stop
 * the session); a do-and-exit run, at its prompt. */
static void gui_master_pw_store_failed(const char *reason)
{
    char msg[1024];
    HWND owner = mpw_live_owner();
    snprintf(msg, sizeof(msg), KT_MPW_STORE_FAILED_FMT, reason ? reason : "");
    if (!owner)
        owner = GetActiveWindow();
    switch (kitty_cli_do_and_exit() ? KITTY_REACH_SHELL : kitty_reach()) {
      case KITTY_REACH_SESSION:
      case KITTY_REACH_DESKTOP:
        if (!kitty_info_modeless(owner, KT_CAP_ERROR, msg, NULL, NULL))
            MessageBox(owner, msg, KT_CAP_ERROR, MB_OK | MB_ICONERROR);
        break;
      default:
        KittyCliReport(KT_CAP_ERROR, msg, 1);
        break;
    }
}

/* A save kept a stored secret: the master password is locked or was never
 * set up, or DPAPI failed (ksec_report_locked_save, which words it and gives
 * the caption). In a terminal a notice
 * and an Event Log line; a do-and-exit run (-importdir) on its redirected
 * output or at its prompt, never a box - with neither, the line waits for
 * the run's result (ksec_kept_hold); elsewhere (the configuration window at
 * start) a modeless box. */
static void gui_master_pw_locked_save(const char *cap, const char *text)
{
    char *msg = dupstr(text ? text : "");
    /* a do-and-exit run first: KittyCliReport's last resort is a box, which
     * stood behind every window while the run waited for it */
    if (kitty_cli_do_and_exit()) {
        if (!KittyCliOutput(msg, 1))
            ksec_kept_hold(msg);
        sfree(msg);
        return;
    }
    switch (kitty_reach()) {
      case KITTY_REACH_SESSION:
        kitty_notice_show(cap, msg, RGB(190, 110, 0),
                          GetNoticeSeconds(), NULL, 0);
        debug_logevent("%s", msg);
        break;
      case KITTY_REACH_DESKTOP:
        if (!kitty_info_modeless(GetActiveWindow(), cap, msg, NULL, NULL))
            kitty_info_box(GetActiveWindow(), cap, msg, NULL);
        break;
      default:
        KittyCliReport(cap, msg, 1);
        break;
    }
    sfree(msg);
}

static void __attribute__((constructor)) kitty_mpw_gui_autoreg(void)
{
    kitty_set_master_pw_prompt(gui_master_pw_prompt);
    kitty_set_legacy_migrate_warn(gui_legacy_migrate_warn);
    kitty_set_master_pw_store_failed(gui_master_pw_store_failed);
    kitty_set_master_pw_locked_save(gui_master_pw_locked_save);
}
