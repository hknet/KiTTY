/*
 * KiTTY <-> PuTTY 0.84 bridge (intermediate "global shim" approach).
 *
 * KiTTY's modules assume a single global Conf/Terminal; PuTTY 0.84 keeps
 * that state per-WinGuiSeat. This file supplies the self-contained KiTTY
 * glue symbols. The seat-context wrappers (global `conf`, do_eventlog,
 * resize, ResetWindow, SendStrToTerminal) live in windows/window.c where
 * the active WinGuiSeat and static helpers are visible.
 *
 * TODO(no-global phase): replace the stubs below with real implementations
 * threaded through the seat, per the planned no-global integration.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "putty.h"
#include "kitty.h"
#include "kitty_commun.h"  /* GetCryptSaltFlag, MASKPASS */
#ifdef MOD_PROXY
#include "kitty_proxy.h"   /* LoadProxyInfo, GetProxySelectionFlag */
#include "kitty_workplace.h"   /* workplace proxy mode: is an arming held? */
void debug_logevent( const char *fmt, ... ) ;   /* kitty_win.c */
#endif

/* KiTTY logging mode toggle (originally KiTTY logging.c) */
int LogMode = 0;
int SwitchLogMode(void) { LogMode = abs(LogMode - 1); return LogMode; }

/* KiTTY crypt-file flag (originally kitty_settings.c) */
int CryptFileFlag = 0;
int SwitchCryptFlag(void) { CryptFileFlag = abs(CryptFileFlag - 1); return CryptFileFlag; }

/* -loginscript path: set by putty.c during cmdline parse, consumed once by
 * window.c after kitty_set_active_seat (ReadInitScript needs the global conf). */
char *kitty_cli_loginscript = NULL;

/* KiTTY password debug log (originally settings.c) */
int DebugAddPassword(const char *fct, const char *pwd) {
    FILE *fp;
    if ((fp = fopen("kitty.password", "r")) != NULL) {
        fclose(fp);
        if ((fp = fopen("kitty.password", "a")) != NULL) {
            fprintf(fp, "%s=%s\n", fct, pwd);
            fclose(fp);
        }
        return 1;
    }
    return 0;
}

/* KiTTY "force reconfiguration" flag (originally a window.c global int) */
int force_reconf = 1;

/* Override the SSH client version string (kitty.ini 'sshversion').
 * sshver is a mutable char[40] in utils/version.c (see ssh.h). */
extern char sshver[40];
void set_sshver(const char *vers) {
    if (!vers) return;
    strncpy(sshver, vers, sizeof(sshver) - 1);
    sshver[sizeof(sshver) - 1] = '\0';
}

/* save_open_settings_forced now implemented in kitty_settings_forced.c */

/* Launch a NEW session process from an in-memory Conf, by serialising it into
 * a file-mapping and spawning "<exe> &<filemap>:<size>" — exactly the native
 * 0.84 Duplicate-Session mechanism (windows/window.c IDM_DUPSESS), which the
 * child parses via handle_special_filemapping_cmdline(). */
void RunSessionWithConfSettings(Conf *conf) {
    char b[2048];
    char *cl = NULL;
    const char *argprefix;
    STARTUPINFO si;
    PROCESS_INFORMATION pi;
    HANDLE filemap = NULL;
    SECURITY_ATTRIBUTES sa;
    strbuf *serbuf;
    void *p;
    int size;

    argprefix = restricted_acl() ? "&R" : "";

    serbuf = strbuf_new();
    conf_serialise(BinarySink_UPCAST(serbuf), conf);
    size = serbuf->len;

    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = true;
    filemap = CreateFileMapping(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE,
                                0, size, NULL);
    if (filemap && filemap != INVALID_HANDLE_VALUE) {
        p = MapViewOfFile(filemap, FILE_MAP_WRITE, 0, 0, size);
        if (p) { memcpy(p, serbuf->s, size); UnmapViewOfFile(p); }
    }
    strbuf_free(serbuf);

    cl = dupprintf("putty %s&%p:%u", argprefix, filemap, (unsigned)size);
    GetModuleFileName(NULL, b, sizeof(b) - 1);
    si.cb = sizeof(si);
    si.lpReserved = NULL; si.lpDesktop = NULL; si.lpTitle = NULL;
    si.dwFlags = 0; si.cbReserved2 = 0; si.lpReserved2 = NULL;
    CreateProcess(b, cl, NULL, NULL, true /*inherit_handles*/,
                  NORMAL_PRIORITY_CLASS, NULL, NULL, &si, &pi);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (filemap) CloseHandle(filemap);
    sfree(cl);
}

/* Open a new window's CONFIGURATION BOX on these settings, without connecting.
 *
 * Same shared-memory hand-off as RunSessionWithConfSettings above - the Conf is
 * serialised into a file mapping whose handle the child inherits - but sent as
 * an ordinary "-confmap" switch rather than the leading "&" form, for two
 * reasons: the "&" form is defined to consume the WHOLE command line, so
 * nothing else could be passed with it; and the child has to be told to stop at
 * the box, which is what "-cfgbox" says. The "&" form implies "this is a
 * complete session, launch it".
 *
 * Why not a temporary saved session, which is how this used to work: settings
 * written to the store are visible to every process on the machine, they
 * include CONF_password, they are left behind if the child dies - and the
 * writer cannot know when the reader is done, which is exactly the race that
 * made "Inherit New Session..." silently inherit nothing (measured
 * 2026-08-08). A file mapping is private to the two processes, needs no name,
 * and stays alive precisely as long as one of them holds a handle. */
void RunConfigBoxWithConfSettings(Conf *conf) {
    char exe[2048];
    char *cl = NULL;
    STARTUPINFO si;
    PROCESS_INFORMATION pi;
    HANDLE filemap = NULL, mpwmap = NULL;
    char mpwtok[80] = "";
    SECURITY_ATTRIBUTES sa;
    strbuf *serbuf;
    void *p;
    int size;

    serbuf = strbuf_new();
    conf_serialise(BinarySink_UPCAST(serbuf), conf);
    size = serbuf->len;

    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = true;
    filemap = CreateFileMapping(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE,
                                0, size, NULL);
    if (filemap && filemap != INVALID_HANDLE_VALUE) {
        p = MapViewOfFile(filemap, FILE_MAP_WRITE, 0, 0, size);
        if (p) { memcpy(p, serbuf->s, size); UnmapViewOfFile(p); }
    }
    strbuf_free(serbuf);
    if (!filemap || filemap == INVALID_HANDLE_VALUE)
        return;

    /* Pass on the restricted ACL and the master-password unlock, exactly as
     * RunSession() does - a config box opened from a restricted window must be
     * restricted too, and one opened from an unlocked window should not ask for
     * the master password again. Both switches come BEFORE -confmap so they are
     * in effect while the settings are read. */
    { extern int kitty_mpw_startup_unlock(void);
      extern HANDLE kitty_mpw_export_inherit_blob(const char*, char*, size_t);
      kitty_mpw_startup_unlock();
      mpwmap = kitty_mpw_export_inherit_blob(" -mpwkey ", mpwtok, sizeof(mpwtok)); }

    cl = dupprintf("putty%s%s -confmap %p:%u -cfgbox",
                   restricted_acl() ? " -restrict-acl" : "", mpwtok,
                   filemap, (unsigned)size);
    GetModuleFileName(NULL, exe, sizeof(exe) - 1);
    si.cb = sizeof(si);
    si.lpReserved = NULL; si.lpDesktop = NULL; si.lpTitle = NULL;
    si.dwFlags = 0; si.cbReserved2 = 0; si.lpReserved2 = NULL;
    if (CreateProcess(exe, cl, NULL, NULL, true /*inherit_handles*/,
                      NORMAL_PRIORITY_CLASS, NULL, NULL, &si, &pi)) {
        AllowSetForegroundWindow(pi.dwProcessId);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    if (mpwmap) CloseHandle(mpwmap);
    CloseHandle(filemap);
    sfree(cl);
}

void RunSessionWithCurrentSettings(HWND hwnd, Conf *oldconf, const char *host,
                                   const char *user, const char *pass,
                                   const int port, const char *remotepath) {
    Conf *newconf = conf_copy(oldconf);
    (void)port;
    if (host != NULL) conf_set_str(newconf, CONF_host, host);
    if (user != NULL) conf_set_str(newconf, CONF_username, user);
    if (pass != NULL) conf_set_str(newconf, CONF_password, pass);

    /* Keep CONF_password PLAINTEXT here. newconf is serialised straight to the
     * child through an inherit-only file mapping - whichever of the two calls
     * below is taken - and the child reads CONF_password raw at connect time.
     * It never reaches the settings store on this path.
     * The old MASKPASS here turned the (plaintext) password into high-byte
     * garbage -> Duplicate-Session / open-new-with-current auto-login sent a
     * corrupted password (even for ASCII). Runtime conf is plaintext (see
     * window.c get_userpass_input), so just pass it through. */
#ifdef MOD_NOPASSWORD
    conf_set_str(newconf, CONF_password, "");
#endif

    if (remotepath != NULL) {
        char *buf = (char*)malloc(strlen(remotepath) + 5);
        sprintf(buf, "cd %s", remotepath);
        conf_set_str(newconf, CONF_autocommand, buf);
        free(buf);
    }

    if (conf_launchable(newconf)) {
        RunSessionWithConfSettings(newconf);
    } else {
        /* Not launchable: the new window starts at the configuration box. The
         * settings go through shared memory - see RunConfigBoxWithConfSettings.
         * This used to save them to a session called "__STARTUP" and delete it
         * immediately after spawning the child, which lost the race often
         * enough that the child usually came up on empty settings. */
        (void)hwnd;
        RunConfigBoxWithConfSettings(newconf);
    }
    conf_free(newconf);
}

/* ===== kitty menu-action wrappers (window.c calls these; they may use KiTTY
 * globals/APIs declared in kitty.h, which window.c does not include) ===== */
void kitty_send_to_tray(HWND hwnd) {
    if (GetVisibleFlag() == VISIBLE_YES) {
        SetVisibleFlag(VISIBLE_TRAY);
        ManageToTray(hwnd);
    }
}
void kitty_rollup(HWND hwnd, int resize_action) {
    if (GetWinrolFlag())
        ManageWinrol(hwnd, resize_action);
}

/* window.c bridge fn (defined in window.c MOD_PERSO block) */
void ResetWindow(int reinit);
/* Safe font resize: 0.84 conf_get_fontspec returns conf's INTERNAL pointer
 * (must NOT be freed); build a new FontSpec, let conf copy it, free our copy. */
void kitty_font_resize(Terminal *term, Conf *conf, int dec) {
    FontSpec *cur = conf_get_fontspec(conf, CONF_font);
    int h = cur->height + dec;
    if (h <= 0) h = 1;
    FontSpec *nfs = fontspec_new(cur->name, cur->isbold, h, cur->charset);
    conf_set_fontspec(conf, CONF_font, nfs);
    fontspec_free(nfs);
    ResetWindow(2);
}

void kitty_protect(HWND hwnd, TermWin *tw, Conf *conf) {
    /* ManageProtect only reads title (passes to set_title); cast is safe */
    char *title = kitty_expand_wintitle(conf_get_str(conf, CONF_wintitle),
                                        conf_get_str(conf, CONF_host), conf);
    ManageProtect(hwnd, tw, title);
    sfree(title);
}
void kitty_print(HWND hwnd) { ManagePrint(hwnd); }

void kitty_negative(HWND hwnd) { NegativeColours(hwnd); }
void kitty_bw(HWND hwnd) { BlackOnWhiteColours(hwnd); }
void kitty_showportfwd(HWND hwnd, Conf *conf) { ShowPortfwd(hwnd, conf); }
void kitty_shortcuts_toggle(HWND hwnd) { ManageShortcutsFlag(hwnd); }
void kitty_start_winscp(HWND hwnd) { StartWinSCP(hwnd, NULL, NULL, NULL); }
void kitty_send_file(HWND hwnd) { SendFile(hwnd); }

/* Per-session icon: the external icon file (CONF_iconefile) if the session
 * names one, otherwise the embedded icon indexed by CONF_icone. */
void kitty_apply_icon(HWND hwnd, Conf *conf) {
    const char *iconfile = filename_to_str(conf_get_filename(conf, CONF_iconefile));
    char buf[1024];
    buf[0] = '\0';
    if (iconfile && iconfile[0]) {
        strncpy(buf, iconfile, sizeof(buf)-1);
        buf[sizeof(buf)-1] = '\0';
    }
    SetNewIcon(hwnd, buf, conf_get_int(conf, CONF_icone), SI_INIT);
}

/* KiTTY: restore the session's normal window icon after a (re)connect, undoing
 * SetConnBreakIcon(), so the broken-connection icon never sticks once the
 * session is back up. */
void kitty_restore_icon(HWND hwnd, Conf *conf) {
    const char *iconfile = filename_to_str(conf_get_filename(conf, CONF_iconefile));
    char buf[1024];
    buf[0] = '\0';
    if (iconfile && iconfile[0]) {
        strncpy(buf, iconfile, sizeof(buf)-1);
        buf[sizeof(buf)-1] = '\0';
    }
    SetNewIcon(hwnd, buf, conf_get_int(conf, CONF_icone), SI_INIT);
}

/* KiTTY About box (IDM_ABOUT). A compact KiTTY-specific dialog showing the
 * KiTTY build version and credits, with a clickable project link. Template
 * IDD_KITTYABOUT lives in windows/kitty.rc. */
extern char BuildVersionTime[256];
/* Resource IDs for the KiTTY about dialog (kept in sync with
 * windows/kitty_rc_additions.h, which isn't on this target's include path). */
#ifndef IDD_KITTYABOUT
#define IDD_KITTYABOUT 121
#endif
#ifndef IDA_VERSION
#define IDA_VERSION 1006
#endif
#ifndef IDC_WEBPAGE
#define IDC_WEBPAGE 401
#endif
static INT_PTR CALLBACK KittyAboutProc(HWND hwnd, UINT msg,
                                       WPARAM wParam, LPARAM lParam) {
    char buffer[1024];
    switch (msg) {
      case WM_INITDIALOG:
#ifdef KITTY_TEST_BUILD_LABEL
        snprintf( buffer, sizeof(buffer), "KiTTY - %s\r\nTEST BUILD: %s", BuildVersionTime,
                KITTY_TEST_BUILD_LABEL);
#else
        snprintf( buffer, sizeof(buffer), "KiTTY - %s", BuildVersionTime);
#endif
        SetDlgItemText(hwnd, IDA_VERSION, buffer);
        return 1;
      case WM_COMMAND:
        switch (LOWORD(wParam)) {
          case IDOK:
          case IDCANCEL:
            EndDialog(hwnd, 0);
            return 0;
          case IDC_WEBPAGE:
            ShellExecute(hwnd, "open", "https://www.9bis.net/kitty",
                         NULL, NULL, SW_SHOWNORMAL);
            return 0;
        }
        return 0;
      case WM_CLOSE:
        EndDialog(hwnd, 0);
        return 0;
    }
    return 0;
}

void kitty_about(HWND hwnd) {
    DialogBox(GetModuleHandle(NULL), MAKEINTRESOURCE(IDD_KITTYABOUT),
              hwnd, KittyAboutProc);
}

#ifdef MOD_BACKGROUNDIMAGE
/* Background image: load the configured image into the module-global
 * backgroundbm/backgrounddc (kitty_image.c). Enables the image machinery
 * and confirms the image LOADS; the WM_PAINT blit is intentionally not
 * wired (see note in window.c) to avoid destabilising 0.84's refactored
 * paint path. Returns nonzero if a background bitmap was created. */
extern HWND MainHwnd;
extern HBITMAP backgroundbm;
BOOL load_bg_bmp(void);
int kitty_apply_background(HWND hwnd, Conf *conf) {
    if (!GetBackgroundImageFlag()) return 0;
    if (GetPuttyFlag()) return 0;
    if (conf_get_int(conf, CONF_bg_type) == 0) return 0;  /* solid = nothing */
    MainHwnd = hwnd;
    load_bg_bmp();
    int ok = (backgroundbm != NULL);
    /* Env-gated self-test: write the load result so a harness can verify the
     * image actually loaded without needing to observe the (unwired) render. */
    {
        const char *st = getenv("KITTY_BG_SELFTEST");
        if (st && st[0]) {
            FILE *f = fopen(st, "w");
            if (f) {
                BITMAP bm; memset(&bm, 0, sizeof(bm));
                if (backgroundbm) GetObject(backgroundbm, sizeof(bm), &bm);
                fprintf(f, "load_bg_bmp ok=%d bmp=%p w=%ld h=%ld bgtype=%d file=%s\n",
                        ok, (void*)backgroundbm, bm.bmWidth, bm.bmHeight,
                        conf_get_int(conf, CONF_bg_type),
                        filename_to_str(conf_get_filename(conf, CONF_bg_image_filename)));
                fclose(f);
            }
        }
    }
    return ok;
}
#endif

/* Export current settings to a .ktx file (IDM_EXPORTSETTINGS).
 * Mirrors KiTTY's SaveCurrentSetting() but takes the seat conf (no global). */
int SaveFileName(HWND hFrame, char *filename, char *Title, char *Filter);
void save_open_settings_forced(char *filename, Conf *conf);
void kitty_export_settings(HWND hwnd, Conf *conf) {
    char filename[4096], buffer[4096];
    if (strlen(FileExtension) > 0) {
        strcpy(buffer, "Connection files (*");
        strcat(buffer, FileExtension); strcat(buffer, ")|*");
        strcat(buffer, FileExtension); strcat(buffer, "|");
    } else {
        strcpy(buffer, "Connection files (*.ktx)|*.ktx|");
    }
    strcat(buffer, "All files (*.*)|*.*|");
    if (buffer[strlen(buffer)-1] != '|') strcat(buffer, "|");
    if (SaveFileName(hwnd, filename, "Save file...", buffer)) {
        save_open_settings_forced(filename, conf);
    }
}

/* ---- Bulk session export/import (TASK_dpapi_mpw_backend_policy Step E) ----
 * Export: every saved session is decrypted through the normal read path
 * (DPAPI/MPW/legacy) and written as a .ktx bundle file with the password
 * wrapped by the portable protection policy — the first wrap prompts to
 * create/unlock the master password, making the bundle machine-independent.
 * Import: each chosen .ktx loads through the forced reader (which unlocks
 * MPW1 / decodes legacy forms) and is saved as a normal session, so the
 * storage chokepoint rewraps the password for the DESTINATION backend
 * (registry -> DPAPI1, portable -> MPW1). "Default Settings" is included in
 * the export: this is a whole-store move, and importing it restores the
 * defaults too. */
#include <commdlg.h>
int OpenDirName(HWND hFrame, char *dirname);
void load_open_settings_forced(char *filename, Conf *conf);
char *kitty_session_fname_munge(const char *);
char *kitty_session_fname_unmunge(const char *);

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

/* ---- export bundle password (design/TASK_export_password.md) --------------
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

extern void kitty_set_bundle_passphrase(const char *pass);
extern void kitty_set_bundle_dpapi_only(int on);
extern void kitty_clear_bundle_context(void);
extern int  kitty_bundle_wrap_failed(void);

#define KITTY_EXPORT_PW_MIN 5

static char *g_exp_result;      /* collected UTF-8 password (malloc'd) or NULL */
static int   g_exp_dpapi;       /* user chose "this PC only" */

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
                    "Please enter an export password of at least "
                    "5 characters.\n\n"
                    "If you do not want a password, choose \"Protect for this "
                    "PC only\" instead - those files can then only be imported "
                    "with this Windows account on this PC.",
                    "KiTTY session export", MB_OK | MB_ICONINFORMATION);
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
    r = DialogBoxA(GetModuleHandle(NULL), MAKEINTRESOURCEA(IDD_EXPORTPW),
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
                    "This folder already contains exported session files.\n\n"
                    "Export into an empty or new folder so old and new sessions "
                    "are not mixed (use the \"New Folder\" button in the picker).\n\n"
                    "Export here anyway?",
                    "KiTTY session export", MB_YESNO | MB_ICONWARNING) != IDYES)
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
             "Exported %d session%s (%d failed) to:\n%s\n\n%s",
             n, n == 1 ? "" : "s", fail, dir,
             dpapi
               ? "These sessions can only be imported with THIS Windows "
                 "account on THIS PC."
               : "This password is required to import these sessions - on ANY "
                 "PC, including this one. It is not your master password, and "
                 "nothing here was changed.");
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
extern char *portable_subdir_path(const char *subdir);   /* snewn'd */

static const char *g_mpwm_path;

static INT_PTR CALLBACK mpwmoved_dlgproc(HWND hdlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
      case WM_INITDIALOG:
        SetDlgItemTextA(hdlg, IDC_MPWM_TEXT,
            "This portable KiTTY kept its master password in the Windows "
            "registry of this PC. It has now been moved into a Security folder "
            "next to your sessions, so this copy works the same way on any PC.\r\n"
            "\r\n"
            "If you use other portable copies of KiTTY that share this master "
            "password, copy this Security folder into each of them as well - "
            "without it they cannot open their saved passwords on another PC.\r\n"
            "\r\n"
            "If you never knowingly set a master password: earlier versions "
            "quietly turned the password you typed when exporting sessions into "
            "one. That is most likely what this is.");
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

/* ---- import: is the bundle protected, and with what? (design/TASK_export_
 * password.md SS5) -----------------------------------------------------------
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
extern int  ksec_unprotect(const char *stored, char **out);
extern void kitty_set_bundle_import(int on);

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
                "These sessions are password-protected.\n\nEnter the import "
                "password - the one that was shown when they were exported. "
                "It is not your master password.";
        } else {
            snprintf(again, sizeof(again),
                     "That password did not open these files.%s",
                     left == 1 ? " This is the last try."
                               : " Two tries left.");
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
        "That password does not open these sessions, so nothing was "
        "imported.\n\n"
        "The import password is the one that was shown when the files were "
        "exported - not your master password.",
        "KiTTY session import", MB_OK | MB_ICONWARNING);
    return 0;
}

/* Work out how this bundle is protected and obtain what is needed to open it.
 * 1 = go ahead (*pwOut is the bundle password, or NULL when none is needed),
 * 0 = abandon the import without touching anything. */
static int kitty_unlock_import_bundle(HWND hwnd, const char *dir, char **pwOut)
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
                "These sessions were exported with \"this PC only\" "
                "protection, and this is not the Windows account or the PC "
                "they were exported from, so their saved passwords cannot be "
                "read.\n\n"
                "Nothing was imported. Export them again with a password to "
                "move them to another PC.",
                "KiTTY session import", MB_OK | MB_ICONWARNING);
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
                "Some sessions or proxy definitions in this folder already exist "
                "here (%d in total).\n\n"
                "Yes  -  overwrite all matching sessions and proxies\n"
                "No  -  import only new sessions and proxies\n"
                "Cancel  -  do nothing",
                collide);
            int r = MessageBoxA(hwnd, q, "KiTTY session import",
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
    snprintf(counts, sizeof(counts), "Imported %d session%s and %d prox%s",
             n, n == 1 ? "" : "s", prox, prox == 1 ? "y" : "ies");
    if (skipped > 0) { char t[80]; snprintf(t, sizeof(t), ", %d kept (already existed)", skipped); strncat(counts, t, sizeof(counts)-strlen(counts)-1); }
    if (fail > 0)    { char t[48]; snprintf(t, sizeof(t), ", %d failed", fail); strncat(counts, t, sizeof(counts)-strlen(counts)-1); }
    snprintf(msg, sizeof(msg),
             "%s from:\n%s\n\n"
             "Saved passwords were re-protected for this storage backend "
             "(registry: Windows DPAPI; portable files: master password).",
             counts, dir);
    MessageBoxA(hwnd, msg, "KiTTY session import",
                MB_OK | (fail ? MB_ICONWARNING : MB_ICONINFORMATION));
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

/* Duplicate the current session into a new process (filemap-serialised conf). */
void kitty_dup_session(HWND hwnd, Conf *conf) {
    RunSessionWithCurrentSettings(hwnd, conf, NULL, NULL, NULL, 0, NULL);
}

/* Auto-command: on each TIMER_AUTOCOMMAND fire, peel one line off the global
 * AutoCommand buffer (lazily initialised from CONF_autocommand) and send it.
 * Mirrors KiTTY window.c's TIMER_AUTOCOMMAND handler (line-split on \n / \\n,
 * \\\\ literal backslash, \p / \s passthrough). Returns 1 if more lines remain
 * (caller re-arms the timer), 0 when the command is exhausted.
 * autocommand_delay (ms) is exposed for the caller's SetTimer interval. */
extern Conf *conf;             /* active-seat global (window.c) */
int del(char *ch, const int start, const int length);

/* Reset the auto-command for a NEW connection: drop whatever is left of the
 * previous run's copy so the next tick re-reads CONF_autocommand from the
 * start. Without this a reconnect either resumed half-way through the old
 * copy or - the common case - never ran again at all, because the timer was
 * armed once in WinMain: the command ran once per PROCESS instead of once
 * per connection. Same defect the rutty script and the login script had
 * (hknet/KiTTY#36); this is the third of the three senders. */
void kitty_autocommand_rearm(void)
{
    if (AutoCommand != NULL) {
        free(AutoCommand);
        AutoCommand = NULL;
    }
}

int kitty_autocommand_tick(HWND hwnd)
{
    char buffer[8192] = "";
    int i = 0;
    if (AutoCommand == NULL) {
        const char *src = conf_get_str(conf, CONF_autocommand);
        if (src == NULL || src[0] == '\0')
            return 0;
        AutoCommand = (char *)malloc(strlen(src) + 10);
        strcpy(AutoCommand, src);
    }
    /* SECURITY: dest is written by the source index i, so bound i to the
     * buffer (leaving room for the 2-char appends + NUL) to stop a single
     * >8KB autocommand line from smashing the stack. */
    while (AutoCommand[i] != '\0' && i < (int)sizeof(buffer) - 4) {
        if (AutoCommand[i] == '\n') { i++; break; }
        else if (AutoCommand[i] == '\\' && AutoCommand[i + 1] == '\\') {
            strcat(buffer, "\\\\"); i += 2;
        } else if (AutoCommand[i] == '\\' && AutoCommand[i + 1] == 'n') {
            i += 2; break;
        } else if (AutoCommand[i] == '\\' && AutoCommand[i + 1] == 'p') {
            strcat(buffer, "\\p"); i += 2; break;
        } else if (AutoCommand[i] == '\\' && AutoCommand[i + 1] == 's') {
            strcat(buffer, "\\s"); i += 2;
            buffer[i] = AutoCommand[i]; buffer[i + 1] = '\0'; i++;
            buffer[i] = AutoCommand[i]; buffer[i + 1] = '\0'; i++;
            break;
        } else {
            buffer[i] = AutoCommand[i]; buffer[i + 1] = '\0'; i++;
        }
    }
    del(AutoCommand, 1, i);
    if (strlen(buffer) > 0)
        SendAutoCommand(hwnd, buffer);
    if (AutoCommand[0] == '\0') {
        free(AutoCommand); AutoCommand = NULL;
        return 0;
    }
    return 1;
}

/* Anti-idle: fired by TIMER_ANTIIDLE, whose period is the configured
 * interval ([KiTTY] antiidledelay, seconds, default 180) - so every firing
 * sends the keepalive: CONF_antiidle if the session sets one, else the
 * kitty.ini global AntiIdleStr. It used to be a fixed 30s timer with a tick
 * counter, which is why the ini value meant three times what it said. */
void kitty_antiidle_tick(HWND hwnd)
{
    const char *s;
    s = conf_get_str(conf, CONF_antiidle);
    if (s != NULL && s[0] != '\0')
        SendAutoCommand(hwnd, s);
    else if (AntiIdleStr[0] != '\0')
        SendAutoCommand(hwnd, AntiIdleStr);
}

#ifdef MOD_PORTKNOCKING
/* KiTTY feature: port-knocking. Before the connection is opened, send the
 * configured knock sequence (CONF_portknockingoptions) to the target host.
 * ManagePortKnocking (kitty_ssh.c) opens a socket to each host:port[:proto]
 * entry in turn (with a small inter-knock delay), which is exactly what a
 * port-knock daemon listens for. Called from start_backend() before
 * backend_init(). No-global: takes the seat conf. */
int ManagePortKnocking(char *host, char *portstr);
void kitty_port_knock(Conf *conf)
{
    const char *host = conf_get_str(conf, CONF_host);
    const char *seq  = conf_get_str(conf, CONF_portknockingoptions);
    if (seq == NULL || seq[0] == '\0')
        return;
    if (host == NULL || host[0] == '\0')
        return;
    ManagePortKnocking((char *)host, (char *)seq);
}
#endif

#ifdef MOD_PROXY
/* KiTTY feature: proxy selection. Before connecting, if the proxy-selector is
 * enabled, overlay a named proxy definition (saved under the Proxies\ subtree)
 * onto this seat's conf. LoadProxyInfo writes CONF_proxy_* from the named entry.
 * "- Session defined proxy -" is a no-op. Called from start_backend() before
 * backend_init(). No-global: takes the seat conf. */
/* Set by kitty_proxy_select() for the call that has just happened: 1 when the
 * proxy applied came from workplace proxy mode. start_backend() reads it
 * immediately afterwards and records it on the seat, because from then on it is
 * a fact about that connection - the mode may be switched off while the
 * connection lives, and a connection may outlive several switchings. */
int kitty_workplace_applied = 0;

/*
 * The proxy THIS CONNECTION is actually going through.
 *
 * A named proxy or workplace proxy mode amends a throwaway copy of the Conf and
 * never the session (see start_backend()) - which is right for the session file
 * and wrong for everything downstream that wants to know how we got there. The
 * transfer helpers read the session Conf, so a session routed through a named
 * proxy was handing WinSCP the session's own proxy fields: usually none at all,
 * so WinSCP tried to reach a host only the proxy can see.
 *
 * So the connection records what it resolved, the same way the seat already
 * records workplace_proxied: it is a fact about the connection, and it must
 * survive the throwaway copy being freed the moment backend_init() returns.
 * Only the proxy fields are kept - nothing here needs the rest of the Conf, and
 * a whole copy would be a second place the session's own password lives.
 *
 * NULL contents mean "no override": callers fall back to the session Conf.
 */
static struct kitty_proxy_snapshot kitty_conn_proxy = { PROXY_NONE, 0, NULL, NULL, NULL, NULL };

static void kitty_proxy_snapshot_clear(void)
{
    if (kitty_conn_proxy.host) { sfree(kitty_conn_proxy.host); }
    if (kitty_conn_proxy.username) { sfree(kitty_conn_proxy.username); }
    if (kitty_conn_proxy.password) {
        smemclr(kitty_conn_proxy.password, strlen(kitty_conn_proxy.password));
        sfree(kitty_conn_proxy.password);
    }
    if (kitty_conn_proxy.telnet_command) { sfree(kitty_conn_proxy.telnet_command); }
    kitty_conn_proxy.type = PROXY_NONE;
    kitty_conn_proxy.port = 0;
    kitty_conn_proxy.host = kitty_conn_proxy.username = NULL;
    kitty_conn_proxy.password = kitty_conn_proxy.telnet_command = NULL;
}

/* resolved == NULL: the session's own proxy settings are what we connected
 * with, so there is nothing to remember. Called on every connect, including
 * auto-reconnect and Restart Session, so a reconnect re-answers the question
 * instead of leaving the previous answer standing. */
void kitty_proxy_record_connection(Conf *resolved)
{
    kitty_proxy_snapshot_clear();
    if (!resolved)
        return;
    kitty_conn_proxy.type = conf_get_int(resolved, CONF_proxy_type);
    kitty_conn_proxy.port = conf_get_int(resolved, CONF_proxy_port);
    kitty_conn_proxy.host = dupstr(conf_get_str(resolved, CONF_proxy_host));
    kitty_conn_proxy.username = dupstr(conf_get_str(resolved, CONF_proxy_username));
    kitty_conn_proxy.password = dupstr(conf_get_str(resolved, CONF_proxy_password));
    kitty_conn_proxy.telnet_command = dupstr(conf_get_str(resolved, CONF_proxy_telnet_command));
}

const struct kitty_proxy_snapshot *kitty_proxy_connection(void)
{
    return kitty_conn_proxy.host ? &kitty_conn_proxy : NULL;
}

/*
 * Resolve, for the connection about to be made, whether the proxy Host is a
 * hostname or may be the title of a saved session. The named proxy's own
 * setting decides; when it says nothing, kitty.ini [KiTTY] namedproxy does, and
 * that defaults to PuTTY's long-standing saved-session-first behaviour.
 *
 * proxy/sshproxy.c reads the answer out of the Conf, because it compiles into
 * the shared crypto library without MOD_PERSO.
 */
static void kitty_proxy_apply_host_kind(Conf *conf)
{
    int kind = conf_get_int(conf, CONF_proxy_host_kind);   /* set by LoadProxyInfo */
    if (kind < 0)
        kind = kitty_named_proxy_default_hostname();
    conf_set_int(conf, CONF_proxy_named_hostname, kind ? 1 : 0);
}

void kitty_proxy_select(Conf *conf)
{
    const char *name;

    kitty_workplace_applied = 0;
    /* A session's OWN proxy host keeps upstream behaviour; only a named proxy
     * can say otherwise, below. */
    conf_set_int(conf, CONF_proxy_named_hostname, 0);
    /* Workplace proxy mode wins over everything the session says
     * (design/TASK_workplace_proxy.md §2, §4). It is a mode about where the
     * user is sitting today, so it applies to every connection this install
     * starts, and it applies whether or not the Session-panel selector is
     * shown - that gate is about the droplist, not about this.
     *
     * Same rule as the droplist override: the connection is amended, the
     * stored session is not. start_backend() calls us on a throwaway copy.
     *
     * A proxy that has since been deleted leaves the connection unproxied, so
     * say so in the Event Log rather than connecting direct in silence. */
    {
        char wp[256];
        if (kitty_workplace_query(wp, sizeof(wp))) {
            /* kitty_proxy_name_exists() rather than LoadProxyInfo()'s return
             * value: in portable (dir) mode LoadProxyInfo reports success for a
             * definition file that is not there. */
            if (kitty_proxy_name_exists(wp)) {
                LoadProxyInfo(conf, wp);
                kitty_proxy_apply_host_kind(conf);
                conf_set_str(conf, CONF_proxyselection, wp);
                kitty_workplace_applied = 1;
                debug_logevent("workplace proxy mode: connecting through \"%s\"", wp);
            } else {
                debug_logevent("workplace proxy mode: proxy \"%s\" is not defined "
                               "- connection NOT proxied", wp);
            }
            return;
        }
    }
    /* Refresh proxies[] from the store first: this runs at connect time, which
     * may be a different context than the startup InitProxyList() (spawned
     * session, -load, auto-reconnect), and both the "shown" gate below and the
     * resolve/lookup need the current set — otherwise a chosen named proxy is
     * silently dropped. */
    InitProxyList();
    /* Apply only when the selector is shown (yes, or auto with proxies defined)
     * — matches the config box, so a proxy chosen in the droplist takes effect
     * and proxyselection=no fully disables it (hknet/KiTTY#11). */
    if (!kitty_proxy_choice_shown())
        return;
    kitty_proxy_resolve_selection(conf);   /* a deleted named proxy -> fallback */
    name = conf_get_str(conf, CONF_proxyselection);
    if (name == NULL || name[0] == '\0')
        return;
    LoadProxyInfo(conf, name);
    kitty_proxy_apply_host_kind(conf);
}
#endif
