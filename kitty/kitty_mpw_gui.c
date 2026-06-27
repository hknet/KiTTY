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
 * Linked only into the GUI targets (kitty / kitty_portable). CLI tools
 * (plink/pscp/...) don't link it, so they never pop a modal -> MPW is simply
 * unavailable there (matches the "-batch must fail-fast, no modal" rule).
 */
#include <windows.h>
#include <string.h>
#include "kitty_rc_additions.h"   /* IDD_MASTERPW, IDC_MPW_* */

extern void kitty_set_master_pw_prompt(char *(*fn)(int creating));

static int   g_first_time;   /* show + require the confirm field */
static char *g_result;       /* collected UTF-8 passphrase (malloc'd) or NULL */

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

static INT_PTR CALLBACK mpw_dlgproc(HWND hdlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
      case WM_INITDIALOG:
        SetDlgItemTextA(hdlg, IDC_MPW_PROMPT, g_first_time ?
            "Set a master password. It encrypts your saved session passwords so "
            "they can move with you to another PC. You will need it to use those "
            "sessions, and it cannot be recovered if you lose it." :
            "Enter your master password to unlock the saved session password.");
        if (!g_first_time) {
            /* Unlock mode: drop the confirm row and shrink the dialog. */
            HWND e1 = GetDlgItem(hdlg, IDC_MPW_EDIT);
            HWND e2 = GetDlgItem(hdlg, IDC_MPW_CONFIRM);
            RECT r1, r2;
            int dy;
            GetWindowRect(e1, &r1);
            GetWindowRect(e2, &r2);
            dy = r2.top - r1.top;                 /* one full label+edit row */
            ShowWindow(e2, SW_HIDE);
            ShowWindow(GetDlgItem(hdlg, IDC_MPW_CONFIRM_LBL), SW_HIDE);
            EnableWindow(e2, FALSE);
            move_ctrl_up(hdlg, IDOK, dy);
            move_ctrl_up(hdlg, IDCANCEL, dy);
            RECT rw;
            GetWindowRect(hdlg, &rw);
            SetWindowPos(hdlg, NULL, 0, 0, rw.right - rw.left,
                         (rw.bottom - rw.top) - dy, SWP_NOMOVE | SWP_NOZORDER);
        }
        SetForegroundWindow(hdlg);
        SetFocus(GetDlgItem(hdlg, IDC_MPW_EDIT));
        return FALSE;                             /* we set focus ourselves */

      case WM_COMMAND:
        switch (LOWORD(wp)) {
          case IDOK: {
            char *p1 = utf8_from_edit(hdlg, IDC_MPW_EDIT);
            if (!p1 || !p1[0]) {
                MessageBoxA(hdlg, "Please enter a master password.",
                            "KiTTY", MB_OK | MB_ICONINFORMATION);
                if (p1) free(p1);
                SetFocus(GetDlgItem(hdlg, IDC_MPW_EDIT));
                return TRUE;
            }
            if (g_first_time) {
                char *p2 = utf8_from_edit(hdlg, IDC_MPW_CONFIRM);
                int mismatch = (!p2 || strcmp(p1, p2) != 0);
                if (p2) { SecureZeroMemory(p2, strlen(p2)); free(p2); }
                if (mismatch) {
                    MessageBoxA(hdlg, "The two master passwords do not match.",
                                "KiTTY", MB_OK | MB_ICONWARNING);
                    SecureZeroMemory(p1, strlen(p1)); free(p1);
                    SetDlgItemTextA(hdlg, IDC_MPW_CONFIRM, "");
                    SetFocus(GetDlgItem(hdlg, IDC_MPW_CONFIRM));
                    return TRUE;
                }
            }
            g_result = p1;
            EndDialog(hdlg, IDOK);
            return TRUE;
          }
          case IDCANCEL:
            EndDialog(hdlg, IDCANCEL);
            return TRUE;
        }
        break;

      case WM_CLOSE:
        EndDialog(hdlg, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

/* Storage-layer callback: returns the UTF-8 master passphrase (caller frees) or
 * NULL if the user cancelled. The storage layer derives + verifies and re-calls
 * us (with creating==0) on a wrong password, so we do not loop here. */
static char *gui_master_pw_prompt(int creating)
{
    char *res;
    HWND owner = GetActiveWindow();
    if (!owner)
        owner = GetForegroundWindow();
    g_first_time = creating;
    g_result = NULL;
    if (DialogBoxA(GetModuleHandle(NULL), MAKEINTRESOURCEA(IDD_MASTERPW),
                   owner, mpw_dlgproc) != IDOK) {
        if (g_result) { free(g_result); g_result = NULL; }
    }
    res = g_result;
    g_result = NULL;
    return res;
}

static void __attribute__((constructor)) kitty_mpw_gui_autoreg(void)
{
    kitty_set_master_pw_prompt(gui_master_pw_prompt);
}
