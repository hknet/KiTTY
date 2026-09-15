/*
 * kitty_exportbundle.c - the bulk session export and import: every saved
 * session written to a folder of protected .ktx files, and such a folder read
 * back into the active store. Serves the Application > Migration buttons, the
 * -exportall / -importdir command-line switches and the whole-store move
 * (kitty_storemove.c). Also the one-time "your master password moved" notice.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "putty.h"
#include "storage.h"       /* read_setting_s/i: per-session hotkey scan */
#include "kitty.h"
#include "kitty_defs.h"    /* KITTY_DEFAULT_SESSION, KITTY_LAUNCHER_HOTKEY_MAX */
#include "kitty_commun.h"  /* GetCryptSaltFlag, MASKPASS */
#include "kitty_pwmem.h"   /* passwords wrapped in memory */
#ifdef MOD_PROXY
#include "kitty_proxy.h"   /* LoadProxyInfo, GetProxySelectionFlag */
#include "kitty_workplace.h"   /* workplace proxy mode: is an arming held? */
#include "kitty_msgbox.h"   /* themed MessageBox routing */
#include "kitty_text.h"     /* shared captions */
#include "kitty_win.h"
#include "kitty_winutil.h"
#include "kitty_storage.h"
#include "kitty_gui.h"
#include "kitty_image.h"
#include "kitty_tools.h"
#include "kitty_ssh.h"
#include "kitty_bridge.h"
#include "kitty_exportbundle.h"
#endif

/* ---- Bulk session export/import -------------------------------------------
 * Export: every saved session is decrypted through the normal read path
 * (DPAPI/MPW/legacy) and written as a .ktx bundle file with the password
 * wrapped by the portable protection policy - the first wrap prompts to
 * create/unlock the master password, making the bundle machine-independent.
 * Import: each chosen .ktx loads through the forced reader (which unlocks
 * MPW1 / decodes legacy forms) and is saved as a normal session, so the
 * storage chokepoint rewraps the password for the DESTINATION backend
 * (registry -> DPAPI1, portable -> MPW1). "Default Settings" is included in
 * the export: this is a whole-store move, and importing it restores the
 * defaults too. */
#include <commdlg.h>

static const char *ktx_ext(void) {
    return FileExtension[0] ? FileExtension : ".ktx";
}

/* Core export: write every saved session as a protected .ktx into dir. Returns
 * the count written; *failOut (optional) gets the failure count. No UI, so it
 * serves both the config-box button and the -exportall CLI flag. */
int kitty_export_all_to_dir(const char *dir, int *failOut) {
    struct sesslist sl;
    int i, n = 0, fail = 0;
    get_sesslist(&sl, true);
    for (i = 0; i < sl.nsessions; i++) {
        /* Skip the "Default Settings" pseudo-session: it is the new-session
         * template, not a saved session, and get_sesslist always forces it in at
         * index 0 (it was the "1 failed" on export). */
        if (!strcmp(sl.sessions[i], "Default Settings")) continue;
        Conf *conf = conf_new();
        if (load_settings(sl.sessions[i], conf)) {
            char *m = kitty_session_fname_munge(sl.sessions[i]);
            char *path = dupprintf("%s\\%s%s", dir, m, ktx_ext());
            save_open_settings_forced(path, conf);
            if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) n++;
            else fail++;
            sfree(m);
            sfree(path);
        } else {
            fail++;
        }
        conf_free(conf);
    }
    get_sesslist(&sl, false);
    /* Carry the named-proxy definitions alongside the sessions (Piece 7): the
     * whole-store move should bring proxies too, passwords wrapped MPW2 so they
     * work on the destination machine. */
    kitty_export_proxies_to_dir(dir);
    if (failOut) *failOut = fail;
    return n;
}

/* ---- export bundle password ----------------------------------------------
 * An export bundle is a TRANSPORT artifact, so it gets its own password rather
 * than hijacking the store's master password (which exporting used to CREATE as
 * a side effect, persisting MasterPwSalt/MasterPwVerifier into the user's
 * store). Two choices: a password that works on any PC, or DPAPI "this PC and
 * this account only". Cancel exports nothing - there is deliberately no silent
 * fallback to weaker protection.
 *
 * The password is readable while typing, so there is no confirm field: the user
 * has to be able to read what they must retype on the other machine.
 */
#include "kitty_rc_additions.h"   /* IDD_EXPORTPW, IDD_EXPORTDONE, IDC_EXP_* */


#define KITTY_EXPORT_PW_MIN 5

static char *g_exp_result;      /* collected UTF-8 password (malloc'd) or NULL */
static int   g_exp_dpapi;       /* user chose "this PC only" */
static int   g_exp_template = IDD_EXPORTPW; /* dialog template in use */

/* Read an edit control as a malloc'd UTF-8 string; the wide buffer is scrubbed
 * before release. (Same approach as the master-password prompt: the password
 * store is UTF-8.) */
static char *exp_utf8_from_edit(HWND hdlg, int id)
{
    HWND h = GetDlgItem(hdlg, id);
    int wlen = GetWindowTextLengthW(h);
    WCHAR *w = (WCHAR *)malloc((size_t)(wlen + 1) * sizeof(WCHAR));
    char *s;
    int n;
    if (!w) return NULL;
    GetWindowTextW(h, w, wlen + 1);
    n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    s = (char *)malloc(n > 0 ? (size_t)n : 1);
    if (s) WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    SecureZeroMemory(w, (size_t)(wlen + 1) * sizeof(WCHAR));
    free(w);
    return s;
}

static void exp_sync_mode(HWND hdlg)
{
    BOOL pw = (IsDlgButtonChecked(hdlg, IDC_EXP_MODEPW) == BST_CHECKED);
    EnableWindow(GetDlgItem(hdlg, IDC_EXP_PASS), pw);
    EnableWindow(GetDlgItem(hdlg, IDC_EXP_PASS_LBL), pw);
    EnableWindow(GetDlgItem(hdlg, IDC_EXP_SHOWPW), pw);
    EnableWindow(GetDlgItem(hdlg, IDC_EXP_DPAPIWARN), !pw);
}

static INT_PTR CALLBACK exportpw_dlgproc(HWND hdlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
      case WM_INITDIALOG:
        CheckRadioButton(hdlg, IDC_EXP_MODEPW, IDC_EXP_MODEDPAPI, IDC_EXP_MODEPW);
        SendMessage(GetDlgItem(hdlg, IDC_EXP_PASS), EM_SETPASSWORDCHAR, (WPARAM)'*', 0);
        exp_sync_mode(hdlg);
        SetForegroundWindow(hdlg);
        SetFocus(GetDlgItem(hdlg, IDC_EXP_PASS));
        return FALSE;                          /* we set focus ourselves */

      case WM_COMMAND:
        switch (LOWORD(wp)) {
          case IDC_EXP_MODEPW:
          case IDC_EXP_MODEDPAPI:
            /* Set the selection explicitly rather than relying on the automatic
             * radio grouping: the password field, its label and the Show
             * checkbox sit BETWEEN the two radios, and those intervening
             * controls stop the auto-uncheck walk - so clicking the second
             * radio left the first one checked too, and everything keyed off
             * "which mode is selected" then read the wrong answer. */
            CheckRadioButton(hdlg, IDC_EXP_MODEPW, IDC_EXP_MODEDPAPI,
                             (int)LOWORD(wp));
            exp_sync_mode(hdlg);
            return TRUE;

          case IDC_EXP_SHOWPW: {
            BOOL show = (IsDlgButtonChecked(hdlg, IDC_EXP_SHOWPW) == BST_CHECKED);
            HWND pw = GetDlgItem(hdlg, IDC_EXP_PASS);
            SendMessage(pw, EM_SETPASSWORDCHAR, show ? 0 : (WPARAM)'*', 0);
            InvalidateRect(pw, NULL, TRUE);
            return TRUE;
          }

          case IDOK: {
            char *p;
            if (IsDlgButtonChecked(hdlg, IDC_EXP_MODEDPAPI) == BST_CHECKED) {
                g_exp_dpapi = 1;
                EndDialog(hdlg, IDOK);
                return TRUE;
            }
            p = exp_utf8_from_edit(hdlg, IDC_EXP_PASS);
            if (!p || (int)strlen(p) < KITTY_EXPORT_PW_MIN) {
                MessageBoxA(hdlg,
                    KT_BRIDGE_EXPORT_PW_SHORT,
                    KT_CAP_SESSION_EXPORT, MB_OK | MB_ICONINFORMATION);
                if (p) { SecureZeroMemory(p, strlen(p)); free(p); }
                SetFocus(GetDlgItem(hdlg, IDC_EXP_PASS));
                return TRUE;
            }
            g_exp_result = p;
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

/* Ask how to protect the bundle. Returns 1 to go ahead (*pwOut = malloc'd
 * password, or NULL when "this PC only" was chosen), 0 if cancelled. */
static int kitty_ask_export_password(HWND hwnd, char **pwOut, int *dpapiOut)
{
    INT_PTR r;
    g_exp_result = NULL;
    g_exp_dpapi = 0;
    r = DialogBoxA(GetModuleHandle(NULL), MAKEINTRESOURCEA(g_exp_template),
                   hwnd, exportpw_dlgproc);
    if (r != IDOK) {
        if (g_exp_result) {
            SecureZeroMemory(g_exp_result, strlen(g_exp_result));
            free(g_exp_result);
            g_exp_result = NULL;
        }
        return 0;
    }
    *pwOut = g_exp_result;
    *dpapiOut = g_exp_dpapi;
    g_exp_result = NULL;
    return 1;
}

/* The same question for a portable copy of the store (kitty_storemove.c):
 * same controls, its own words - there the password IS the copy's master
 * password, the opposite of what the export dialog says. */
int kitty_ask_store_password(HWND hwnd, char **pwOut, int *dpapiOut)
{
    int r;
    g_exp_template = IDD_STOREMOVEPW;
    r = kitty_ask_export_password(hwnd, pwOut, dpapiOut);
    g_exp_template = IDD_EXPORTPW;
    return r;
}

/* ---- export summary (shows the password once, with Copy) ---- */
static const char *g_expd_text;
static const char *g_expd_pw;      /* NULL in "this PC only" mode */

static INT_PTR CALLBACK exportdone_dlgproc(HWND hdlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
      case WM_INITDIALOG:
        SetDlgItemTextA(hdlg, IDC_EXPD_TEXT, g_expd_text ? g_expd_text : "");
        if (g_expd_pw) {
            SetDlgItemTextA(hdlg, IDC_EXPD_PW, g_expd_pw);
        } else {
            /* No password to show: hide the whole readout row. */
            ShowWindow(GetDlgItem(hdlg, IDC_EXPD_PWLBL), SW_HIDE);
            ShowWindow(GetDlgItem(hdlg, IDC_EXPD_PW), SW_HIDE);
            ShowWindow(GetDlgItem(hdlg, IDC_EXPD_COPY), SW_HIDE);
            EnableWindow(GetDlgItem(hdlg, IDC_EXPD_PW), FALSE);
            EnableWindow(GetDlgItem(hdlg, IDC_EXPD_COPY), FALSE);
        }
        SetForegroundWindow(hdlg);
        return TRUE;

      case WM_COMMAND:
        switch (LOWORD(wp)) {
          case IDC_EXPD_COPY: {
            /* Put the password on the clipboard - this is the moment the user
             * wants to stash it. It stays there until something overwrites it. */
            size_t n;
            HGLOBAL h;
            if (!g_expd_pw || !OpenClipboard(hdlg)) return TRUE;
            n = strlen(g_expd_pw) + 1;
            h = GlobalAlloc(GMEM_MOVEABLE, n);
            if (h) {
                void *p = GlobalLock(h);
                if (p) {
                    memcpy(p, g_expd_pw, n);
                    GlobalUnlock(h);
                    EmptyClipboard();
                    if (!SetClipboardData(CF_TEXT, h)) GlobalFree(h);
                } else {
                    GlobalFree(h);
                }
            }
            CloseClipboard();
            return TRUE;
          }
          case IDOK:
          case IDCANCEL:
            EndDialog(hdlg, IDOK);
            return TRUE;
        }
        break;

      case WM_CLOSE:
        EndDialog(hdlg, IDOK);
        return TRUE;
    }
    return FALSE;
}

void kitty_export_all_sessions(HWND hwnd) {
    char dir[4096];
    int n, fail = 0;
    char msg[4400];
    char *bundlepw = NULL;
    int dpapi = 0, wrapfailed;
    if (!OpenDirName(hwnd, dir)) return;
    /* Export writes one file per session (plus a Proxies\ subfolder). Warn when
     * the chosen folder already holds exported files, so old and new sessions
     * don't get mixed - the picker's "New Folder" button makes a clean one. */
    {
        char pat[4200]; WIN32_FIND_DATAA fd; HANDLE h;
        snprintf(pat, sizeof(pat), "%s\\*%s", dir, ktx_ext());
        h = FindFirstFileA(pat, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            FindClose(h);
            if (MessageBoxA(hwnd,
                    KT_BRIDGE_EXPORT_FOLDER_USED,
                    KT_CAP_SESSION_EXPORT, MB_YESNO | MB_ICONWARNING) != IDYES)
                return;
        }
    }
    /* Ask BEFORE exporting: cancel here must leave nothing behind. */
    if (!kitty_ask_export_password(hwnd, &bundlepw, &dpapi)) return;

    if (dpapi) kitty_set_bundle_dpapi_only(1);
    else       kitty_set_bundle_passphrase(bundlepw);
    n = kitty_export_all_to_dir(dir, &fail);
    wrapfailed = kitty_bundle_wrap_failed();
    kitty_clear_bundle_context();

    /* A password wrap that fell back to DPAPI produces a bundle that only works
     * on this PC. Say so instead of showing a password that does not open it. */
    if (wrapfailed && bundlepw) {
        SecureZeroMemory(bundlepw, strlen(bundlepw));
        free(bundlepw);
        bundlepw = NULL;
        dpapi = 1;
    }

    snprintf(msg, sizeof(msg),
             KT_BRIDGE_EXPORTED,
             n, n == 1 ? "" : "s", fail, dir,
             dpapi
               ? KT_BRIDGE_EXPORTED_DPAPI
               : KT_BRIDGE_EXPORTED_PW);
    g_expd_text = msg;
    g_expd_pw = bundlepw;
    DialogBoxA(GetModuleHandle(NULL), MAKEINTRESOURCEA(IDD_EXPORTDONE),
               hwnd, exportdone_dlgproc);
    g_expd_text = NULL;
    g_expd_pw = NULL;

    if (bundlepw) { SecureZeroMemory(bundlepw, strlen(bundlepw)); free(bundlepw); }
}

/* Does a saved session with this exact name already exist in the active store? */
static int kitty_session_exists(const char *name) {
    struct sesslist sl; int i, found = 0;
    get_sesslist(&sl, true);
    for (i = 0; i < sl.nsessions; i++)
        if (!strcmp(sl.sessions[i], name)) { found = 1; break; }
    get_sesslist(&sl, false);
    return found;
}

/* Session name a .ktx filename maps to (caller frees), or NULL. */
static char *kitty_ktx_session_name(const char *filename) {
    const char *ext = ktx_ext();
    size_t el = strlen(ext), sl;
    char *stem = dupstr(filename);
    sl = strlen(stem);
    if (sl > el && !_stricmp(stem + sl - el, ext)) stem[sl - el] = '\0';
    char *name = kitty_session_fname_unmunge(stem);
    sfree(stem);
    return name;
}

/* Import one .ktx. Returns 1 imported, 0 failed, 2 skipped (already exists and
 * overwrite==0). */
static int kitty_import_one_ktx(const char *path, int overwrite) {
    Conf *conf = conf_new();
    char *name, *err;
    const char *base;
    int ret = 0;
    /* Populate every setting with its default first: a .ktx only carries the
     * fields it was written with, and load_open_settings_forced() (unlike the
     * normal load path) does NOT seed defaults - so any key the file omits would
     * stay unset and later conf_get_*() would assert (utils/conf.c "entry"). */
    do_defaults(NULL, conf);
    load_open_settings_forced((char *)path, conf);
    base = strrchr(path, '\\');
    base = base ? base + 1 : path;
    name = kitty_ktx_session_name(base);
    if (name && name[0]) {
        if (!overwrite && kitty_session_exists(name)) {
            ret = 2;                       /* keep the existing session */
        } else {
            err = save_settings(name, conf);
            ret = (err == NULL) ? 1 : 0;
            if (err) sfree(err);
        }
    }
    sfree(name);
    conf_free(conf);
    return ret;
}

int kitty_import_dir(const char *dir, int *failOut, int *proxyOut,
                     int *skippedOut, int overwrite);   /* defined below */

/* ---- "your master password moved" notice ---------------------------------
 * Shown once, after a portable store has had the master-password state copied
 * out of the registry into its own Security\ folder. The folder is the point of
 * the message - the user has to carry it to any other portable copy of KiTTY
 * that shares the same master password - so it is spelled out, selectable, and
 * one click from Explorer rather than described in prose.
 */

static const char *g_mpwm_path;

static INT_PTR CALLBACK mpwmoved_dlgproc(HWND hdlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
      case WM_INITDIALOG:
        SetDlgItemTextA(hdlg, IDC_MPWM_TEXT,
            KT_BRIDGE_MPW_MOVED);
        SetDlgItemTextA(hdlg, IDC_MPWM_PATH, g_mpwm_path ? g_mpwm_path : "");
        SetForegroundWindow(hdlg);
        return TRUE;

      case WM_COMMAND:
        switch (LOWORD(wp)) {
          case IDC_MPWM_COPY: {
            size_t n;
            HGLOBAL h;
            if (!g_mpwm_path || !OpenClipboard(hdlg)) return TRUE;
            n = strlen(g_mpwm_path) + 1;
            h = GlobalAlloc(GMEM_MOVEABLE, n);
            if (h) {
                void *p = GlobalLock(h);
                if (p) {
                    memcpy(p, g_mpwm_path, n);
                    GlobalUnlock(h);
                    EmptyClipboard();
                    if (!SetClipboardData(CF_TEXT, h)) GlobalFree(h);
                } else {
                    GlobalFree(h);
                }
            }
            CloseClipboard();
            return TRUE;
          }
          case IDC_MPWM_OPEN:
            if (g_mpwm_path)
                ShellExecuteA(hdlg, "open", g_mpwm_path, NULL, NULL, SW_SHOWDEFAULT);
            return TRUE;
          case IDOK:
          case IDCANCEL:
            EndDialog(hdlg, IDOK);
            return TRUE;
        }
        break;

      case WM_CLOSE:
        EndDialog(hdlg, IDOK);
        return TRUE;
    }
    return FALSE;
}

void kitty_show_mpw_moved(HWND hwnd)
{
    char *dir = portable_subdir_path("Security");
    g_mpwm_path = dir;
    DialogBoxA(GetModuleHandle(NULL), MAKEINTRESOURCEA(IDD_MPWMOVED),
               hwnd, mpwmoved_dlgproc);
    g_mpwm_path = NULL;
    if (dir) sfree(dir);
}

/* ---- import: is the bundle protected, and with what? ---------------------
 * Whether to ask for a password is decided from the FILES, never guessed: scan
 * the bundle for a value carrying a protection marker. MPW2: means an export
 * password is needed and works on any PC; DPAPI1: means the bundle is tied to
 * one Windows account on one PC and must never raise a password dialog.
 *
 * The sampled value is also the test. The candidate password is checked against
 * it BEFORE a single session is written, so a wrong password abandons the
 * import with nothing half-imported.
 *
 * Reading the raw text (rather than loading each session) is what makes that
 * possible, and it is safe: both writers munge their values, so a marker only
 * ever appears at the start of a value and the value ends at the next literal
 * backslash.
 */
extern int  ksec_unwrap_with_passphrase(const char *stored,
                                        const char *passphrase, char **out);

#define KITTY_IMPORT_PW_TRIES 3

/* Value starting at p (a munged token), unmunged. snewn'd. */
static char *bundle_take_value(const char *p)
{
    const char *e = p;
    char *munged, *out;
    size_t n;
    while (*e && *e != '\\' && *e != '\r' && *e != '\n') e++;
    n = (size_t)(e - p);
    munged = snewn(n + 1, char);
    memcpy(munged, p, n);
    munged[n] = '\0';
    out = snewn(n + 1, char);               /* unmunging never grows a string */
    unmungestr(munged, out, (int)n + 1);
    sfree(munged);
    return out;
}

static void bundle_scan_text(const char *txt, char **mpw2Out, char **dpapiOut)
{
    /* Both spellings: the .ktx and proxy writers munge (':' -> "%3A"), but a
     * hand-written or externally generated file may not have. */
    static const char *const marks[] = {
        "MPW2%3A", "MPW2:", "DPAPI1%3A", "DPAPI1:"
    };
    unsigned i;
    for (i = 0; i < lenof(marks); i++) {
        const char *hit = strstr(txt, marks[i]);
        char **slot = (i < 2) ? mpw2Out : dpapiOut;
        if (hit && !*slot) *slot = bundle_take_value(hit);
    }
}

static char *bundle_read_file(const char *path)
{
    FILE *fp = fopen(path, "rb");
    long len;
    size_t got;
    char *buf;
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    len = ftell(fp);
    /* A session file is a few kB; the cap just stops a stray huge file in the
     * chosen folder from being slurped whole. */
    if (len < 0 || len > 4L * 1024 * 1024) { fclose(fp); return NULL; }
    rewind(fp);
    buf = snewn((size_t)len + 1, char);
    got = fread(buf, 1, (size_t)len, fp);
    buf[got] = '\0';
    fclose(fp);
    return buf;
}

/* Scan every file matching dir\pattern; stops early once an MPW2 sample is in
 * hand, since that already decides the question. */
static void bundle_scan_dir(const char *dir, const char *pattern,
                            char **mpw2Out, char **dpapiOut)
{
    char *pat = dupprintf("%s\\%s", dir, pattern);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    sfree(pat);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        char *path, *txt;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (*mpw2Out) break;
        path = dupprintf("%s\\%s", dir, fd.cFileName);
        txt = bundle_read_file(path);
        if (txt) {
            bundle_scan_text(txt, mpw2Out, dpapiOut);
            smemclr(txt, strlen(txt));
            sfree(txt);
        }
        sfree(path);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

/* Does this bundle need a password to import? 1 = yes (an MPW2 value is in
 * there), 0 = no. For the CLI, which has no one to ask. */
int kitty_bundle_needs_password(const char *dir)
{
    char *mpw2 = NULL, *dpapi = NULL, *pat, *sub;
    int need;
    pat = dupprintf("*%s", ktx_ext());
    bundle_scan_dir(dir, pat, &mpw2, &dpapi);
    sfree(pat);
    sub = dupprintf("%s\\Proxies", dir);
    bundle_scan_dir(sub, "*", &mpw2, &dpapi);
    sfree(sub);
    need = (mpw2 != NULL);
    sfree(mpw2);
    sfree(dpapi);
    return need;
}

/* ---- import password prompt ---- */
static char *g_imp_result;          /* collected UTF-8 password (malloc'd) */
static const char *g_imp_prompt;

static INT_PTR CALLBACK importpw_dlgproc(HWND hdlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
      case WM_INITDIALOG:
        SetDlgItemTextA(hdlg, IDC_IMP_PROMPT, g_imp_prompt ? g_imp_prompt : "");
        SendMessage(GetDlgItem(hdlg, IDC_IMP_PASS), EM_SETPASSWORDCHAR,
                    (WPARAM)'*', 0);
        SetForegroundWindow(hdlg);
        SetFocus(GetDlgItem(hdlg, IDC_IMP_PASS));
        return FALSE;                          /* we set focus ourselves */

      case WM_COMMAND:
        switch (LOWORD(wp)) {
          case IDC_IMP_SHOWPW: {
            BOOL show = (IsDlgButtonChecked(hdlg, IDC_IMP_SHOWPW) == BST_CHECKED);
            HWND pw = GetDlgItem(hdlg, IDC_IMP_PASS);
            SendMessage(pw, EM_SETPASSWORDCHAR, show ? 0 : (WPARAM)'*', 0);
            InvalidateRect(pw, NULL, TRUE);
            return TRUE;
          }
          case IDOK:
            g_imp_result = exp_utf8_from_edit(hdlg, IDC_IMP_PASS);
            EndDialog(hdlg, IDOK);
            return TRUE;
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

static void imp_wipe(char **p)
{
    if (*p) { SecureZeroMemory(*p, strlen(*p)); free(*p); *p = NULL; }
}

/* Ask for the bundle password and verify it against `sample` before returning.
 * 1 with *pwOut set (malloc'd) on success; 0 on cancel or after 3 wrong tries -
 * in which case nothing must be imported. */
static int kitty_ask_import_password(HWND hwnd, const char *sample, char **pwOut)
{
    int tries;
    for (tries = 0; tries < KITTY_IMPORT_PW_TRIES; tries++) {
        char *plain = NULL;
        INT_PTR r;
        int left = KITTY_IMPORT_PW_TRIES - tries;
        char again[200];
        if (tries == 0) {
            g_imp_prompt =
                KT_BRIDGE_IMPORT_PW_PROMPT;
        } else {
            snprintf(again, sizeof(again),
                     KT_BRIDGE_IMPORT_PW_WRONG,
                     left == 1 ? KT_BRIDGE_IMPORT_PW_LAST
                               : KT_BRIDGE_IMPORT_PW_TWO);
            g_imp_prompt = again;
        }
        g_imp_result = NULL;
        r = DialogBoxA(GetModuleHandle(NULL), MAKEINTRESOURCEA(IDD_IMPORTPW),
                       hwnd, importpw_dlgproc);
        if (r != IDOK) { imp_wipe(&g_imp_result); return 0; }
        if (g_imp_result && g_imp_result[0] &&
            ksec_unwrap_with_passphrase(sample, g_imp_result, &plain) == 1) {
            imp_wipe(&plain);                  /* only ever needed as a check */
            *pwOut = g_imp_result;
            g_imp_result = NULL;
            return 1;
        }
        imp_wipe(&plain);
        imp_wipe(&g_imp_result);
    }
    MessageBoxA(hwnd,
        KT_BRIDGE_IMPORT_PW_FAILED,
        KT_CAP_SESSION_IMPORT, MB_OK | MB_ICONWARNING);
    return 0;
}

/* Work out how this bundle is protected and obtain what is needed to open it.
 * 1 = go ahead (*pwOut is the bundle password, or NULL when none is needed),
 * 0 = abandon the import without touching anything. */
int kitty_unlock_import_bundle(HWND hwnd, const char *dir, char **pwOut)
{
    char *mpw2 = NULL, *dpapi = NULL, *pat, *sub;
    int ok = 1;
    *pwOut = NULL;
    pat = dupprintf("*%s", ktx_ext());
    bundle_scan_dir(dir, pat, &mpw2, &dpapi);
    sfree(pat);
    /* Named proxies are exported alongside, under Proxies\, with no extension -
     * and one password covers both (point 17), so they count as evidence too. */
    sub = dupprintf("%s\\Proxies", dir);
    bundle_scan_dir(sub, "*", &mpw2, &dpapi);
    sfree(sub);

    if (mpw2) {
        ok = kitty_ask_import_password(hwnd, mpw2, pwOut);
    } else if (dpapi) {
        /* No prompt: a DPAPI bundle either opens silently on this account and
         * PC, or cannot be opened at all. Say which, rather than importing
         * sessions with silently blank passwords. */
        char *plain = NULL;
        int rv = ksec_unprotect(dpapi, &plain);
        imp_wipe(&plain);
        if (rv != 1) {
            MessageBoxA(hwnd,
                KT_BRIDGE_IMPORT_DPAPI_FOREIGN,
                KT_CAP_SESSION_IMPORT, MB_OK | MB_ICONWARNING);
            ok = 0;
        }
    }
    sfree(mpw2);
    sfree(dpapi);
    return ok;
}


void kitty_import_sessions(HWND hwnd) {
    char dir[4096];
    int n, fail = 0, prox = 0, skipped = 0, overwrite = 1;
    char msg[700], counts[320];
    char *bundlepw = NULL;
    /* Folder-based, to match Export all (both pick a folder): imports every .ktx
     * in the chosen folder plus its Proxies\ subfolder. */
    if (!OpenDirName(hwnd, dir)) return;
    /* Settle the bundle's protection first: a wrong password, or a "this PC
     * only" bundle from elsewhere, must abandon the import before anything is
     * written or any other question is asked. */
    if (!kitty_unlock_import_bundle(hwnd, dir, &bundlepw)) return;
    /* Import overwrites a saved session/proxy of the same name. Count the
     * collisions across BOTH and let the user choose once: overwrite all, import
     * only new, or cancel. */
    {
        int collide = 0;
        char pat[4200]; WIN32_FIND_DATAA fd; HANDLE h;
        snprintf(pat, sizeof(pat), "%s\\*%s", dir, ktx_ext());
        h = FindFirstFileA(pat, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                char *nm = kitty_ktx_session_name(fd.cFileName);
                if (nm && nm[0] && kitty_session_exists(nm)) collide++;
                sfree(nm);
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        collide += kitty_proxies_dir_collisions(dir);
        if (collide > 0) {
            char q[440];
            snprintf(q, sizeof(q),
                KT_BRIDGE_IMPORT_COLLIDE,
                collide);
            int r = MessageBoxA(hwnd, q, KT_CAP_SESSION_IMPORT,
                                MB_YESNOCANCEL | MB_ICONQUESTION);
            if (r == IDCANCEL) { imp_wipe(&bundlepw); return; }
            overwrite = (r == IDYES) ? 1 : 0;
        }
    }
    /* Import direction: the bundle password OPENS the files; what gets saved
     * is re-protected by the destination store, not by the transport password. */
    if (bundlepw) {
        kitty_set_bundle_import(1);
        kitty_set_bundle_passphrase(bundlepw);
    }
    n = kitty_import_dir(dir, &fail, &prox, &skipped, overwrite);
    kitty_clear_bundle_context();
    imp_wipe(&bundlepw);
    snprintf(counts, sizeof(counts), KT_BRIDGE_IMPORTED_COUNTS,
             n, n == 1 ? "" : "s", prox, prox == 1 ? "y" : "ies");
    if (skipped > 0) { char t[80]; snprintf(t, sizeof(t), KT_BRIDGE_IMPORTED_KEPT, skipped); strncat(counts, t, sizeof(counts)-strlen(counts)-1); }
    if (fail > 0)    { char t[48]; snprintf(t, sizeof(t), KT_BRIDGE_IMPORTED_FAILED, fail); strncat(counts, t, sizeof(counts)-strlen(counts)-1); }
    snprintf(msg, sizeof(msg),
             KT_BRIDGE_IMPORTED_FROM,
             counts, dir);
    MessageBoxA(hwnd, msg, KT_CAP_SESSION_IMPORT,
                MB_OK | (fail ? MB_ICONWARNING : MB_ICONINFORMATION));
    /* Imported sessions bring their hotkeys along, and nothing above checked
     * those against the store: say NOW if the store ended up with a hotkey
     * held twice, or with more enabled hotkeys than the launcher has slots -
     * the alternative is a session whose hotkey silently never fires. */
    if (n > 0) {
        char rep[1200];
        int nc = kitty_hotkey_conflict_report(rep, sizeof(rep));
        int en = kitty_hotkey_enabled_count(NULL);
        if (nc > 0 || en > KITTY_LAUNCHER_HOTKEY_MAX) {
            char warn[1600];
            warn[0] = '\0';
            if (nc > 0)
                snprintf(warn, sizeof(warn),
                         KT_BRIDGE_IMPORT_HOTKEY_SHARED, rep);
            if (en > KITTY_LAUNCHER_HOTKEY_MAX) {
                char t[220];
                snprintf(t, sizeof(t),
                         KT_BRIDGE_IMPORT_HOTKEY_LIMIT, warn[0] ? "\n\n" : "",
                         en, KITTY_LAUNCHER_HOTKEY_MAX);
                strncat(warn, t, sizeof(warn) - strlen(warn) - 1);
            }
            MessageBoxA(hwnd, warn, KT_CAP_LAUNCHER_HOTKEY,
                        MB_OK | MB_ICONWARNING);
        }
    }
}

/* Core import: load every .ktx in dir as a session (no UI). Returns the count
 * imported; optional outs: *failOut, *proxyOut (named proxies restored),
 * *skippedOut (existing sessions kept because overwrite==0). Used by -importdir
 * (which always overwrites). */
int kitty_import_dir(const char *dir, int *failOut, int *proxyOut,
                     int *skippedOut, int overwrite) {
    char *pat = dupprintf("%s\\*%s", dir, ktx_ext());
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    int n = 0, fail = 0, skipped = 0;
    sfree(pat);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            char *path = dupprintf("%s\\%s", dir, fd.cFileName);
            int r = kitty_import_one_ktx(path, overwrite);
            if (r == 1) n++; else if (r == 2) skipped++; else fail++;
            sfree(path);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    { int psk = 0;
      int pc = kitty_import_proxies_from_dir(dir, overwrite, &psk);   /* honor overwrite too */
      if (proxyOut) *proxyOut = pc;
      skipped += psk; }                                              /* proxies kept count too */
    if (failOut) *failOut = fail;
    if (skippedOut) *skippedOut = skipped;
    return n;
}
