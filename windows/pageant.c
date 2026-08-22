/*
 * Pageant: the PuTTY Authentication Agent.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <ctype.h>
#include <assert.h>
#include <tchar.h>

#include "putty.h"
#include "ssh.h"
#include "misc.h"
#include "tree234.h"
#include "security-api.h"
#include "cryptoapi.h"
/* AFTER putty.h: dbt.h needs windows.h, which putty.h is what pulls in here.
 * KiTTY: DBT_DEVICEARRIVAL / DBT_DEVICEREMOVECOMPLETE. */
#include <dbt.h>
/* KiTTY: the key list window is a SysListView32 in report mode. */
#include <commctrl.h>
#include <windowsx.h>   /* KiTTY: GET_X_LPARAM/GET_Y_LPARAM (drag reorder) */
#include "pageant.h"
#include "licence.h"
#include "pageant-rc.h"
#include "../kitty/kitty_pageant.h"  /* KiTTY: the kageant additions (split out of this file) */
#include "../kitty/kitty_authenticode.h"  /* KiTTY: shared verify for New key */
#include "../kitty/kitty_title.h"     /* KiTTY: shared title-suffix composer */
#include "../kitty/kitty_inilight.h"  /* KiTTY: portable-layout probe */
#include "../kitty/kitty_protkey.h"   /* KiTTY: kitty_protkey_available (tray tip) */
#include "../kitty/kitty_hello.h"     /* KiTTY: Windows Hello presence check */

#include <shellapi.h>

#include <aclapi.h>
#ifdef DEBUG_IPC
#define _WIN32_WINNT 0x0500            /* for ConvertSidToStringSid */
#include <sddl.h>
#endif

#define WM_SYSTRAY   (WM_APP + 6)
#define WM_SYSTRAY2  (WM_APP + 7)
/* KiTTY: balloon-click notification; absent from older SDK headers. */
#ifndef NIN_BALLOONUSERCLICK
#define NIN_BALLOONUSERCLICK (WM_USER + 5)
#endif
/* KiTTY: timer id for the delayed single-left-click tray menu */
#define TID_TRAYCLICK 1
#define TID_PASSPHRASE_CACHE 2   /* KiTTY: scrub cached passphrases (see WM_TIMER) */
#define TID_KEY_LIFETIME 3       /* KiTTY: expire ssh-add -t keys (see WM_TIMER) */
#define TID_KL_RESUME    7       /* KiTTY: key-list Resume-button sync */

#define APPNAME "kageant"

/* Titles and class names for invisible windows. IPCWINTITLE and
 * IPCCLASSNAME are critical to backwards compatibility: WM_COPYDATA
 * based Pageant clients will call FindWindow with those parameters
 * and expect to find the Pageant IPC receiver. */
#define TRAYWINTITLE  "Pageant"
#define TRAYCLASSNAME "PageantSysTray"
#define IPCWINTITLE   "Pageant"
#define IPCCLASSNAME  "Pageant"

static HWND traywindow;
static HWND keylist;
static HWND aboutbox;
static HMENU systray_menu, session_menu;
static bool already_running;
static FingerprintType fptype = SSH_FPTYPE_DEFAULT;

static char *putty_path;          /* KiTTY: kitty.exe beside kageant */
static bool restrict_putty_acl = false;

/* KiTTY: gate every tray session launch through the same Authenticode/
 * version check as kittygen - but HARD: a terminal gets the agent's keys,
 * so an unverifiable kitty.exe is refused outright, no start-anyway. */
static bool kageant_kitty_launch_allowed(HWND owner)
{
    extern int kitty_verify_sibling(const char *path);
    if (putty_path && kitty_verify_sibling(putty_path))
        return true;
    MessageBox(owner,
               "The kitty.exe next to kageant could not be verified as a "
               "genuine, matching KiTTY build - its signature or version "
               "did not check out.\n\n"
               "It may have been replaced with something else. Because a "
               "terminal started from here would get access to the agent's "
               "keys, kageant will not start it.",
               "kageant - session launch blocked", MB_OK | MB_ICONERROR);
    return false;
}

/* CWD for "add key" file requester. */
static filereq_saved_dir *keypath = NULL;

/* From MSDN: In the WM_SYSCOMMAND message, the four low-order bits of
 * wParam are used by Windows, and should be masked off, so we shouldn't
 * attempt to store information in them. Hence all these identifiers have
 * the low 4 bits clear. Also, identifiers should < 0xF000. */

#define IDM_CLOSE              0x0010
#define IDM_VIEWKEYS           0x0020
#define IDM_ADDKEY             0x0030
#define IDM_ADDKEY_ENCRYPTED   0x0040
#define IDM_REMOVE_ALL         0x0050
#define IDM_REENCRYPT_ALL      0x0060
#define IDM_HELP               0x0070
#define IDM_ABOUT              0x0080
#define IDM_PUTTY              0x0090
#define IDM_OPENSSH_INTEGRATION 0x00A0   /* KiTTY: toggle Windows OpenSSH integration */
#define IDM_LOAD_ON_STARTUP    0x00B0    /* KiTTY: toggle load-keys-on-startup */
#define IDM_NOTIFY_KEYUSE      0x00C0    /* KiTTY: toggle "notify on key use" balloon */
#define IDM_CONFIRM_KEYUSE     0x00D0    /* KiTTY: toggle "confirm every key use" prompt */
#define IDM_SETTINGS           0x00E0    /* KiTTY: open the [Agent] settings dialog */
/* NB: the WM_COMMAND handler masks with ~0xF, so every IDM_ here must be a
 * multiple of 0x10. 0x00B8 silently aliased to IDM_LOAD_ON_STARTUP. */
#define IDM_LOAD_KEYS          0x0100    /* KiTTY: re-add remembered keys */
#define IDM_RESUME_CONFIRM     0x00F0    /* KiTTY: lift the confirm-suppress latch */
#define IDM_SESSIONS_BASE      0x1000
#define IDM_SESSIONS_MAX       0x2000
/* KiTTY: kageant's session submenu reads the hive where the sessions actually
 * live - which is a RUNTIME question, not a compile-time one.
 *
 * PUTTY_REG_POS is the DEFAULT hive ("Software\\kapper.net\\KiTTY"). When
 * kitty.ini says KiClassName=PuTTY the terminal keeps its sessions in
 * "Software\\SimonTatham\\PuTTY" instead, so a macro pinned at the default
 * left this menu reading an empty key: the agent offered no sessions at all
 * and looked broken, for a setting made somewhere else entirely.
 *
 * kageant cannot call kitty_registry_base(): that lives in kitty_storage.c,
 * part of the `settings` library the terminal links and this binary does not.
 * It can read kitty.ini though - kitty_inilight is exactly the resolver the
 * satellite binaries use - so the same answer is worked out here, once. */
static const char *kageant_sessions_key(void)
{
    static char key[300];
    char cls[64];
    if (key[0])
        return key;                     /* settled on the first call */
    cls[0] = '\0';
    if (kitty_inilight_read("KiTTY", "KiClassName", cls, sizeof(cls)) &&
        !stricmp(cls, "PuTTY"))
        strcpy(key, "Software\\SimonTatham\\PuTTY\\Sessions");
    else
        strcpy(key, PUTTY_REG_POS "\\Sessions");
    return key;
}
#define PUTTY_DEFAULT     "Default%20Settings"
static int initial_menuitems_count;

/*
 * Print a modal (Really Bad) message box and perform a fatal exit.
 */
void modalfatalbox(const char *fmt, ...)
{
    va_list ap;
    char *buf;

    va_start(ap, fmt);
    buf = dupvprintf(fmt, ap);
    va_end(ap);
    MessageBox(traywindow, buf, "kageant Fatal Error",
               MB_SYSTEMMODAL | MB_ICONERROR | MB_OK);
    sfree(buf);
    exit(1);
}

struct PassphraseProcStruct {
    bool modal;
    const char *help_topic;
    PageantClientDialogId *dlgid;
    char *passphrase;
    const char *comment;
    HWND over;   /* KiTTY: window to centre the prompt over (the requesting
                  * terminal, captured as the foreground window); NULL = desktop */
};

static void kageant_set_window_icon(HWND hwnd);   /* defined below */

/*
 * Dialog-box function for the Licence box.
 */
static INT_PTR CALLBACK LicenceProc(HWND hwnd, UINT msg,
                                    WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
      case WM_INITDIALOG:
        kageant_set_window_icon(hwnd);
        SetDlgItemText(hwnd, IDC_LICENCE_TEXTBOX, LICENCE_TEXT("\r\n\r\n"));
        return 1;
      case WM_COMMAND:
        switch (LOWORD(wParam)) {
          case IDOK:
          case IDCANCEL:
            EndDialog(hwnd, 1);
            return 0;
        }
        return 0;
      case WM_CLOSE:
        EndDialog(hwnd, 1);
        return 0;
    }
    return 0;
}

/* kitty_auxpos.c: DPI/monitor-safe aux-window placement + position memory. */
void kitty_auxpos_apply(HWND dlg, const char *key, HWND anchor, int near_tray);
void kitty_auxpos_save(HWND dlg, const char *key);

/* KiTTY: the KiTTY key generator, sitting next to us - kittygen.exe in a
 * release, puttygen.exe in a dev build. Returns a malloc'd path if present,
 * else NULL (so the New key button can grey itself when it is not around).
 * Mirrors the putty.exe discovery in WinMain. */
static char *find_kittygen(void)
{
    char b[2048], *r, *p, *q;
    static const char *const names[] = { "kittygen.exe", "puttygen.exe" };
    DWORD n = GetModuleFileNameA(NULL, b, sizeof(b) - 32);
    if (!n || n >= sizeof(b) - 32)
        return NULL;
    r = b;
    p = strrchr(b, '\\'); if (p && p >= r) r = p + 1;
    q = strrchr(b, ':');  if (q && q >= r) r = q + 1;
    for (size_t i = 0; i < lenof(names); i++) {
        strcpy(r, names[i]);
        if (GetFileAttributesA(b) != INVALID_FILE_ATTRIBUTES)
            return dupstr(b);
    }
    return NULL;
}

/* KiTTY: give a dialog the kageant title-bar icon. Dialogs created with
 * CreateDialog/DialogBox get no icon of their own and show the generic
 * Windows default; the tray app has one, so use it. LR_SHARED handles are
 * managed by the system - no DestroyIcon needed. */
static void kageant_set_window_icon(HWND hwnd)
{
    HICON big = (HICON)LoadImage(hinst, MAKEINTRESOURCE(IDI_MAINICON),
                                 IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                 GetSystemMetrics(SM_CYICON), LR_SHARED);
    HICON small = (HICON)LoadImage(hinst, MAKEINTRESOURCE(IDI_MAINICON),
                                   IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), LR_SHARED);
    if (big)
        SendMessage(hwnd, WM_SETICON, ICON_BIG, (LPARAM)big);
    if (small)
        SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)small);
}

/*
 * Dialog-box function for the About box.
 */
static INT_PTR CALLBACK AboutProc(HWND hwnd, UINT msg,
                                  WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
      case WM_INITDIALOG: {
        kageant_set_window_icon(hwnd);
        char *buildinfo_text = buildinfo("\r\n");
#ifdef KITTY_TEST_BUILD_LABEL
        const char *testbuild = "\r\n*** TEST BUILD: " KITTY_TEST_BUILD_LABEL " ***";
#else
        const char *testbuild = "";
#endif
        /* Branded to match the main KiTTY About box (windows/dialog.c): show the
         * kapper.net port holder, not just the upstream PuTTY copyright. UTF-8
         * source for (c) (\xc2\xa9) and em-dash (\xe2\x80\x94), rendered wide so
         * they display on any system codepage. */
        /* KiTTY: the same restricted-ACL note as the main About box - and this
         * is the process that actually holds the private keys, so it is the one
         * worth being certain about. Reports the STATE, not the ini key. */
        const char *aclnote = restricted_acl() ?
            "\r\n\r\nRunning with a restricted process ACL: other programs "
            "under your account cannot open this process." : "";
        char *text = dupprintf(
            "kageant\r\n\r\n%s%s%s\r\n\r\n%s\r\n\r\n%s\r\n\r\n%s\r\n\r\n%s",
            ver, testbuild, aclnote, buildinfo_text,
            "This PuTTY 0.85 port \xc2\xa9 KAPPER NETWORK-COMMUNICATIONS GmbH "
            "\xe2\x80\x94 https://github.com/hknet/KiTTY",
            "KiTTY \xc2\xa9 2007-2013 Cyril Dupont \xe2\x80\x94 https://www.9bis.net/kitty/",
            "Based on PuTTY \xc2\xa9 " SHORT_COPYRIGHT_DETAILS ". All rights reserved.");
        sfree(buildinfo_text);
        {
            int wn = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
            wchar_t *wtext = snewn(wn > 0 ? wn : 1, wchar_t);
            if (wn > 0 && MultiByteToWideChar(CP_UTF8, 0, text, -1, wtext, wn) > 0)
                SetDlgItemTextW(hwnd, IDC_ABOUT_TEXTBOX, wtext);
            else
                SetDlgItemText(hwnd, IDC_ABOUT_TEXTBOX, text);  /* fallback */
            sfree(wtext);
        }
        MakeDlgItemBorderless(hwnd, IDC_ABOUT_TEXTBOX);
        sfree(text);
        /* KiTTY: tray app - place the About near the notification area (or a
         * remembered spot), DPI/multi-monitor-safe, instead of screen-centre. */
        kitty_auxpos_apply(hwnd, "kageantAbout", GetWindow(hwnd, GW_OWNER), 1);
        return 1;
      }
      case WM_COMMAND:
        switch (LOWORD(wParam)) {
          case IDOK:
          case IDCANCEL:
            kitty_auxpos_save(hwnd, "kageantAbout");
            aboutbox = NULL;
            DestroyWindow(hwnd);
            return 0;
          case IDC_ABOUT_LICENCE:
            EnableWindow(hwnd, 0);
            DialogBox(hinst, MAKEINTRESOURCE(IDD_LICENCE), hwnd, LicenceProc);
            EnableWindow(hwnd, 1);
            SetActiveWindow(hwnd);
            return 0;
          case IDC_ABOUT_WEBSITE:
            /* Load web browser */
            ShellExecute(hwnd, "open",
                         "https://www.chiark.greenend.org.uk/~sgtatham/putty/",
                         0, 0, SW_SHOWDEFAULT);
            return 0;
        }
        return 0;
      case WM_CLOSE:
        aboutbox = NULL;
        DestroyWindow(hwnd);
        return 0;
    }
    return 0;
}

static HWND modal_passphrase_hwnd = NULL;
static HWND nonmodal_passphrase_hwnd = NULL;

/*
 * KiTTY: force a window to the foreground from a background process. When a
 * client (e.g. a KiTTY terminal) triggers an on-demand passphrase prompt,
 * kageant is NOT the foreground app, so a bare SetForegroundWindow is blocked
 * by the Windows foreground lock and the prompt opens unfocused behind the
 * terminal. Briefly attaching our input thread to the current foreground
 * thread bypasses that lock so the prompt comes up focused and ready to type.
 */
static void pageant_force_foreground(HWND hwnd)
{
    HWND fgwin = GetForegroundWindow();
    DWORD fgthread = fgwin ? GetWindowThreadProcessId(fgwin, NULL) : 0;
    DWORD mythread = GetCurrentThreadId();
    bool attached = (fgthread && fgthread != mythread &&
                     AttachThreadInput(mythread, fgthread, TRUE));
    SetForegroundWindow(hwnd);
    BringWindowToTop(hwnd);
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    SetForegroundWindow(hwnd);
    SetActiveWindow(hwnd);
    SetFocus(hwnd);
    if (attached)
        AttachThreadInput(mythread, fgthread, FALSE);
}

static void end_passphrase_dialog(HWND hwnd, INT_PTR result)
{
    struct PassphraseProcStruct *p = (struct PassphraseProcStruct *)
        GetWindowLongPtr(hwnd, GWLP_USERDATA);

    if (p->modal) {
        EndDialog(hwnd, result);
    } else {
        /*
         * Destroy this passphrase dialog box before passing the
         * results back to the main pageant.c, to avoid re-entrancy
         * issues.
         *
         * If we successfully got a passphrase from the user, but it
         * was _wrong_, then pageant_passphrase_request_success will
         * respond by calling back - synchronously - to our
         * ask_passphrase() implementation, which will expect the
         * previous value of nonmodal_passphrase_hwnd to have already
         * been cleaned up.
         */
        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR) NULL);
        DestroyWindow(hwnd);
        nonmodal_passphrase_hwnd = NULL;

        if (result)
            pageant_passphrase_request_success(
                p->dlgid, ptrlen_from_asciz(p->passphrase));
        else
            pageant_passphrase_request_refused(p->dlgid);

        burnstr(p->passphrase);
        sfree(p);
    }
}

/*
 * Dialog-box function for the passphrase box.
 */
static INT_PTR CALLBACK PassphraseProc(HWND hwnd, UINT msg,
                                       WPARAM wParam, LPARAM lParam)
{
    struct PassphraseProcStruct *p;

    if (msg == WM_INITDIALOG) {
        p = (struct PassphraseProcStruct *) lParam;
        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR) p);
    } else {
        p = (struct PassphraseProcStruct *)
            GetWindowLongPtr(hwnd, GWLP_USERDATA);
    }

    switch (msg) {
      case WM_INITDIALOG: {
        kageant_set_window_icon(hwnd);
        if (p->modal)
            modal_passphrase_hwnd = hwnd;

        /*
         * Centre the window.
         */
        RECT rs, rd;
        /* KiTTY: centre over the requesting terminal window if we captured it
         * (p->over = the foreground window at request time); else the desktop.
         *
         * A MINIMISED anchor is unusable: GetWindowRect reports a minimised
         * window at roughly (-32000,-32000), so centring on it parked the
         * prompt far off-screen - listed in the taskbar but impossible to
         * bring into view. That is what a session started minimised (a
         * shortcut set to "Run: minimized") hit. Ignore an iconic anchor, and
         * clamp the final position to the work area of the monitor we land on
         * so no anchor rectangle can push the prompt off-screen again. */
        HWND hw = (p->over && IsWindow(p->over) && !IsIconic(p->over)) ?
            p->over : GetDesktopWindow();
        if (GetWindowRect(hw, &rs) && GetWindowRect(hwnd, &rd)) {
            int w = rd.right - rd.left, h = rd.bottom - rd.top;
            int x = (rs.right + rs.left - w) / 2;
            int y = (rs.bottom + rs.top - h) / 2;
            RECT work;
            POINT centre;
            HMONITOR mon;
            MONITORINFO mi;

            centre.x = x + w / 2;
            centre.y = y + h / 2;
            mon = MonitorFromPoint(centre, MONITOR_DEFAULTTONEAREST);
            mi.cbSize = sizeof(mi);
            if (mon && GetMonitorInfo(mon, &mi))
                work = mi.rcWork;
            else
                SetRect(&work, 0, 0, GetSystemMetrics(SM_CXSCREEN),
                        GetSystemMetrics(SM_CYSCREEN));

            if (x + w > work.right)
                x = work.right - w;
            if (y + h > work.bottom)
                y = work.bottom - h;
            if (x < work.left)
                x = work.left;
            if (y < work.top)
                y = work.top;

            MoveWindow(hwnd, x, y, w, h, true);
        }

        SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        pageant_force_foreground(hwnd);   /* KiTTY: beat the foreground lock so
                                           * the prompt is focused and typable */
        if (p->comment)
            SetDlgItemText(hwnd, IDC_PASSPHRASE_FINGERPRINT, p->comment);
        burnstr(p->passphrase);
        p->passphrase = dupstr("");
        SetDlgItemText(hwnd, IDC_PASSPHRASE_EDITBOX, p->passphrase);
        if (!p->help_topic || !has_help()) {
            HWND item = GetDlgItem(hwnd, IDHELP);
            if (item)
                DestroyWindow(item);
        }
        return 0;
      }
      case WM_COMMAND:
        switch (LOWORD(wParam)) {
          case IDOK:
            if (p->passphrase)
                end_passphrase_dialog(hwnd, 1);
            else
                MessageBeep(0);
            return 0;
          case IDCANCEL:
            end_passphrase_dialog(hwnd, 0);
            return 0;
          case IDHELP:
            if (p->help_topic)
                launch_help(hwnd, p->help_topic);
            return 0;
          case IDC_PASSPHRASE_EDITBOX:
            if ((HIWORD(wParam) == EN_CHANGE) && p->passphrase) {
                burnstr(p->passphrase);
                p->passphrase = GetDlgItemText_alloc(
                    hwnd, IDC_PASSPHRASE_EDITBOX);
            }
            return 0;
        }
        return 0;
      case WM_CLOSE:
        end_passphrase_dialog(hwnd, 0);
        return 0;
    }
    return 0;
}

/*
 * Warn about the obsolescent key file format.
 */
void old_keyfile_warning(void)
{
    static const char mbtitle[] = "PuTTY Key File Warning";
    /* KiTTY: during the STARTUP key load nobody clicked anything, and a
     * modal here takes the agent off the air the same way the startup-load
     * failure box did (see the notice above kageant_startup_loading() in
     * win_add_keyfile) - an old-format key in the startup list would pop
     * this at every login. Same sorting as the rest of the audit: a box in
     * answer to a click stays a box; one the agent raises on its own becomes
     * a notice. */
    if (kageant_startup_loading()) {
        if (traywindow)
            kitty_notice_show(
                "kageant: a remembered key uses the old file format",
                "A key in the startup list is an SSH-2 key in the old PPK "
                "format, which is not fully tamperproof and may stop being "
                "supported. Load it into KiTTYgen and save it again to "
                "convert it.",
                KAGEANT_NOTICE_WARN, kageant_notice_seconds(12),
                traywindow, KAGEANT_WM_NOTICE_CLICK);
        return;
    }
    static const char message[] =
        "You are loading an SSH-2 private key which has an\n"
        "old version of the file format. This means your key\n"
        "file is not fully tamperproof. Future versions of\n"
        "PuTTY may stop supporting this private key format,\n"
        "so we recommend you convert your key to the new\n"
        "format.\n"
        "\n"
        "You can perform this conversion by loading the key\n"
        "into PuTTYgen and then saving it again.";

    MessageBox(NULL, message, mbtitle, MB_OK);
}

struct keylist_update_ctx {
    HWND hlist;                    /* KiTTY: the ListView being filled */
    int index;                     /* KiTTY: next row to insert */
    bool enable_remove_controls;
    bool enable_reencrypt_controls;
};

/* KiTTY: what a row's State column can say. KEYSTATE_ENCRYPTED is a deferred
 * key (no cleartext in the agent yet, passphrase asked at first use);
 * KEYSTATE_REENCRYPTABLE is loaded with an encrypted file to fall back to.
 * The last two are startup entries that are NOT in the agent at all: their
 * file is absent (missing) or present-but-unloadable (failed). */
enum { KEYSTATE_LOADED, KEYSTATE_ENCRYPTED, KEYSTATE_REENCRYPTABLE,
       KEYSTATE_MISSING, KEYSTATE_FAILED,
       /* KiTTY: the file is present and readable but is NOT the key recorded
        * for that path, so it was refused. Not an error like FAILED - it is a
        * question for the user. */
       KEYSTATE_MISMATCH };

/* KiTTY: render a lifetime countdown compactly: "47 s", "12:05", "1:02:33". */
static void kageant_fmt_seconds(unsigned s, char *buf, size_t len)
{
    if (s < 60)
        snprintf(buf, len, "%u s", s);
    else if (s < 3600)
        snprintf(buf, len, "%u:%02u", s / 60, s % 60);
    else
        snprintf(buf, len, "%u:%02u:%02u", s / 3600, (s / 60) % 60, s % 60);
}

struct keylist_display_data {
    strbuf *alg, *bits, *hash, *comment, *info;
    strbuf *expires;  /* KiTTY: the Lifetime column - countdown or "unlimited" */
    strbuf *blob;     /* KiTTY: public blob, to identify this row for reordering */
    int state;        /* KiTTY: KEYSTATE_*, for the State column and details */
    char *fp_full[SSH_N_FPTYPES];  /* KiTTY: every fingerprint form, for the
                                    * details dialog; NULL where inapplicable
                                    * (certificate forms of a plain key) */
    int confirm;      /* KiTTY: per-key confirm-on-use (from the ext flag) */
    int pending;      /* KiTTY: a not-loaded startup entry, not an agent key */
    char *pendpath;   /* KiTTY: its stored file path (pending rows only) */
    int ssh_version;  /* KiTTY: 1 or 2; SSH-1 keys cannot be re-encrypted */
};

static void keylist_update_callback(
    void *vctx, char **fingerprints, const char *comment, uint32_t ext_flags,
    struct pageant_pubkey *key)
{
    struct keylist_update_ctx *ctx = (struct keylist_update_ctx *)vctx;
    FingerprintType this_type = ssh2_pick_fingerprint(fingerprints, fptype);
    ptrlen fingerprint = ptrlen_from_asciz(fingerprints[this_type]);

    struct keylist_display_data *disp = snew(struct keylist_display_data);
    disp->alg = strbuf_new();
    disp->bits = strbuf_new();
    disp->hash = strbuf_new();
    disp->comment = strbuf_new();
    disp->info = strbuf_new();
    disp->expires = strbuf_new();
    disp->blob = strbuf_dup(ptrlen_from_strbuf(key->blob));  /* KiTTY: for reordering */
    /* KiTTY: keep every fingerprint form for the details dialog. */
    for (size_t t = 0; t < SSH_N_FPTYPES; t++)
        disp->fp_full[t] = fingerprints[t] ? dupstr(fingerprints[t]) : NULL;
    /* Mode, not a flag: 0 none, 1 click, 2 Windows Hello (bit 8 rides on
     * top of bit 4, never alone). */
    disp->confirm = (ext_flags & LIST_EXTENDED_FLAG_CONFIRM_ON_USE) ?
                    ((ext_flags & LIST_EXTENDED_FLAG_CONFIRM_HELLO) ? 2 : 1)
                    : 0;
    disp->pending = 0;
    disp->pendpath = NULL;
    disp->ssh_version = key->ssh_version;

    /* There is at least one key, so the controls for removing keys
     * should be enabled */
    ctx->enable_remove_controls = true;

    switch (key->ssh_version) {
      case 1: {
        /*
         * Expect the fingerprint to contain two words: bit count and
         * hash.
         */
        put_dataz(disp->alg, "SSH-1");
        put_datapl(disp->bits, ptrlen_get_word(&fingerprint, " "));
        put_datapl(disp->hash, ptrlen_get_word(&fingerprint, " "));
        break;
      }

      case 2: {
        /*
         * Expect the fingerprint to contain three words: algorithm
         * name, bit count, hash.
         */
        const ssh_keyalg *alg = pubkey_blob_to_alg(
            ptrlen_from_strbuf(key->blob));

        ptrlen keytype_word = ptrlen_get_word(&fingerprint, " ");
        if (alg) {
            /* Use our own human-legible algorithm names if available,
             * because they fit better in the space. (Certificate key
             * algorithm names in particular are terribly long.) */
            char *alg_desc = ssh_keyalg_desc(alg);
            put_dataz(disp->alg, alg_desc);
            sfree(alg_desc);
        } else {
            put_datapl(disp->alg, keytype_word);
        }

        ptrlen bits_word = ptrlen_get_word(&fingerprint, " ");
        if (alg && ssh_keyalg_variable_size(alg))
            put_datapl(disp->bits, bits_word);

        put_datapl(disp->hash, ptrlen_get_word(&fingerprint, " "));
      }
    }

    put_dataz(disp->comment, comment);

    /*
     * KiTTY: the state - is this key usable right now, or will it ask for a
     * passphrase first? - gets a real column. It used to be tacked onto the
     * comment, where a long algorithm name or comment pushed it off the right
     * edge; measured 2026-08-08 with a DSA key: an hour spent chasing a key
     * that WAS deferred and did not look it.
     */
    if (ext_flags & LIST_EXTENDED_FLAG_HAS_NO_CLEARTEXT_KEY) {
        disp->state = KEYSTATE_ENCRYPTED;
        put_dataz(disp->info, "encrypted");
    } else if (ext_flags & LIST_EXTENDED_FLAG_HAS_ENCRYPTED_KEY_FILE) {
        disp->state = KEYSTATE_REENCRYPTABLE;
        put_dataz(disp->info, "re-encryptable");

        /* At least one key can be re-encrypted */
        ctx->enable_reencrypt_controls = true;
    } else {
        disp->state = KEYSTATE_LOADED;
        put_dataz(disp->info, "loaded");
    }

    /* KiTTY: the Lifetime column - a countdown for an ssh-add -t key,
     * "unlimited" for everything else. */
    {
        unsigned set_s, rem_s;
        if (kageant_key_lifetime_get(ptrlen_from_strbuf(disp->blob),
                                     &set_s, &rem_s)) {
            char buf[32];
            kageant_fmt_seconds(rem_s, buf, sizeof(buf));
            put_dataz(disp->expires, buf);
        } else {
            put_dataz(disp->expires, "unlimited");
        }
    }

    /* KiTTY: one ListView row per key; the display struct rides along as the
     * row's lParam, exactly as it used to ride in the listbox item data. */
    LVITEM lvi;
    memset(&lvi, 0, sizeof(lvi));
    lvi.mask = LVIF_TEXT | LVIF_PARAM;
    lvi.iItem = ctx->index;
    lvi.pszText = disp->alg->s;
    lvi.lParam = (LPARAM)disp;
    int row = ListView_InsertItem(ctx->hlist, &lvi);
    ListView_SetItemText(ctx->hlist, row, 1, disp->bits->s);
    ListView_SetItemText(ctx->hlist, row, 2, disp->hash->s);
    ListView_SetItemText(ctx->hlist, row, 3, disp->info->s);
    ListView_SetItemText(ctx->hlist, row, 4, disp->expires->s);
    ListView_SetItemText(ctx->hlist, row, 5,
                         disp->confirm == 2 ? "Hello" :
                         disp->confirm     ? "required" : "");
    ListView_SetItemText(ctx->hlist, row, 6, disp->comment->s);
    ctx->index++;
}

/* KiTTY: fetch the display struct a ListView row carries in its lParam. */
static struct keylist_display_data *keylist_row_data(HWND hlist, int row)
{
    LVITEM lvi;
    memset(&lvi, 0, sizeof(lvi));
    lvi.mask = LVIF_PARAM;
    lvi.iItem = row;
    if (row < 0 || !ListView_GetItem(hlist, &lvi))
        return NULL;
    return (struct keylist_display_data *)lvi.lParam;
}

/* KiTTY: free the display structs the ListView rows point at. Called before
 * every rebuild and once more when the window closes. */
static void keylist_free_display_data(HWND hlist)
{
    int nitems = ListView_GetItemCount(hlist);
    for (int i = 0; i < nitems; i++) {
        struct keylist_display_data *disp = keylist_row_data(hlist, i);
        if (!disp)
            continue;
        strbuf_free(disp->alg);
        strbuf_free(disp->bits);
        strbuf_free(disp->hash);
        strbuf_free(disp->comment);
        strbuf_free(disp->info);
        strbuf_free(disp->expires);
        strbuf_free(disp->blob);
        for (size_t t = 0; t < SSH_N_FPTYPES; t++)
            sfree(disp->fp_full[t]);
        sfree(disp->pendpath);
        sfree(disp);
    }
}

/* KiTTY: tick the Lifetime column in place each second while the key list
 * is open. With more than 5 time-limited keys the per-second redraw is
 * deliberately skipped, so a large list is not repainted every second: the
 * column then keeps the remaining time as of the last full list fill. */
static void keylist_tick_lifetimes(void)
{
    int nlim = kageant_lifetime_count();
    if (!keylist || nlim == 0 || nlim > 5)
        return;
    HWND hlist = GetDlgItem(keylist, IDC_KEYLIST_LISTBOX);
    if (!hlist)
        return;
    int nitems = ListView_GetItemCount(hlist);
    for (int i = 0; i < nitems; i++) {
        struct keylist_display_data *disp = keylist_row_data(hlist, i);
        unsigned set_s, rem_s;
        if (!disp || disp->pending || !disp->blob->len)
            continue;
        if (kageant_key_lifetime_get(ptrlen_from_strbuf(disp->blob),
                                     &set_s, &rem_s)) {
            char buf[32];
            kageant_fmt_seconds(rem_s, buf, sizeof(buf));
            ListView_SetItemText(hlist, i, 4, buf);
        }
    }
}

/* KiTTY: is the "Show unavailable keys" toggle on? (The stored name stays
 * showunavailablekeys - the label was reworded, the setting was not.) Persisted like the other
 * kageant settings; default on - the whole point is that these entries were
 * invisible. */
static bool keylist_show_unavail = true;

/* KiTTY: set while keylist_update() tears down and rebuilds the ListView.
 * The teardown frees every row's disp struct before deleting the items, and
 * the deletes/inserts fire LVN_ITEMCHANGED - whose handler must not then read
 * a freed lParam. The post-rebuild button refresh happens explicitly at the
 * end of keylist_update() instead. */
static bool keylist_rebuilding = false;

/*
 * KiTTY: the single, state-sensitive action button (in the Re-encrypt slot).
 *
 * One button beats a Re-encrypt on the list plus a Load-key-now hidden in
 * the details dialog. What it offers depends on the selected key:
 *   - a deferred (encrypted) key whose file is reachable -> DECRYPT it now
 *   - a re-encryptable key (has its own encrypted fallback)   -> RE-ENCRYPT
 *   - a plainly loaded key whose file is reachable            -> RE-ENCRYPT
 *     (kageant kept no encrypted form for a plainly-added key, so we re-read
 *      it from disk to recover one, then drop the cleartext)
 * SSH-1 keys cannot be re-encrypted at all; a key with no reachable file
 * offers nothing.
 */
enum { KLBTN_NONE, KLBTN_DECRYPT, KLBTN_REENCRYPT };

static int keylist_action_for(struct keylist_display_data *disp,
                              char **path_out)
{
    if (path_out)
        *path_out = NULL;
    if (!disp || disp->pending || !disp->blob->len)
        return KLBTN_NONE;

    if (disp->state == KEYSTATE_REENCRYPTABLE)
        return KLBTN_REENCRYPT;    /* has its own fallback; no file needed */

    if (disp->ssh_version != 2)
        return KLBTN_NONE;         /* SSH-1: neither decrypt-now nor re-encrypt */

    /* Both remaining actions need the key's file on disk. */
    char *p = kageant_file_of_blob(ptrlen_from_strbuf(disp->blob));
    if (!p || GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES) {
        sfree(p);
        return KLBTN_NONE;
    }
    int action;
    if (disp->state == KEYSTATE_ENCRYPTED) {
        action = KLBTN_DECRYPT;
    } else {
        /* Plainly loaded -> re-encrypt by recovering the encrypted form from
         * the file. That only works if the PPK actually HAS a passphrase; a
         * passphraseless key has nothing to re-encrypt to, and re-adding it
         * would fail ("agent refused"). */
        Filename *fn = filename_from_str(p);
        char *cmt = NULL;
        bool enc = ppk_encrypted_f(fn, &cmt);
        filename_free(fn);
        sfree(cmt);
        if (!enc) {
            sfree(p);
            return KLBTN_NONE;
        }
        action = KLBTN_REENCRYPT;
    }
    if (path_out)
        *path_out = p;
    else
        sfree(p);
    return action;
}

/*
 * KiTTY: does the button's TARGET action apply to this row, and if so what
 * file does it need? The button has ONE meaning (Decrypt or Re-encrypt, from
 * the focused row); clicking it settles every selected key to that state, it
 * does not flip each key on its own:
 *   - a Decrypt target acts only on a still-encrypted key (already-loaded
 *     keys are already there - skip);
 *   - a Re-encrypt target acts only on a loaded SSH-2 key (already-encrypted
 *     keys are already there - skip; SSH-1 cannot re-encrypt).
 * Returns 1 when it applies, with *path_out set to the key file when one is
 * needed (a Decrypt, or re-encrypting a plainly-loaded key).
 */
static int keylist_row_for_target(struct keylist_display_data *disp,
                                  int target, char **path_out)
{
    if (path_out)
        *path_out = NULL;
    if (!disp || disp->pending || !disp->blob->len)
        return 0;

    if (target == KLBTN_DECRYPT) {
        if (disp->state != KEYSTATE_ENCRYPTED || disp->ssh_version != 2)
            return 0;
    } else if (target == KLBTN_REENCRYPT) {
        if (disp->state == KEYSTATE_ENCRYPTED)
            return 0;                       /* already encrypted */
        if (disp->state == KEYSTATE_REENCRYPTABLE)
            return 1;                       /* has its own fallback; no file */
        if (disp->ssh_version != 2)
            return 0;                       /* SSH-1 can't re-encrypt */
    } else {
        return 0;
    }

    /* Both remaining cases need the key's file on disk. */
    char *p = kageant_file_of_blob(ptrlen_from_strbuf(disp->blob));
    if (!p || GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES) {
        sfree(p);
        return 0;
    }
    if (target == KLBTN_REENCRYPT) {
        /* Plainly loaded: re-encryptable only if the PPK has a passphrase
         * (see keylist_action_for). */
        Filename *fn = filename_from_str(p);
        char *cmt = NULL;
        bool enc = ppk_encrypted_f(fn, &cmt);
        filename_free(fn);
        sfree(cmt);
        if (!enc) {
            sfree(p);
            return 0;
        }
    }
    if (path_out)
        *path_out = p;
    else
        sfree(p);
    return 1;
}

/*
 * KiTTY: what the single action button means for the CURRENT selection.
 *
 * The focused row sets the direction when it can act - so the row you land on
 * decides Decrypt vs Re-encrypt. But if the focused row offers nothing (a
 * passphraseless key, SSH-1, a key with no file), the button must not grey
 * while OTHER selected keys are perfectly actionable - which happened when the
 * non-actionable key just happened to be the last one marked. In that case
 * fall back to the first selected key that can act, so the button stays live
 * and consistent regardless of marking order.
 */
static int keylist_button_target(HWND hlist)
{
    int focus = ListView_GetNextItem(hlist, -1, LVNI_FOCUSED | LVNI_SELECTED);
    if (focus >= 0) {
        int a = keylist_action_for(keylist_row_data(hlist, focus), NULL);
        if (a != KLBTN_NONE)
            return a;
    }
    int row = -1;
    while ((row = ListView_GetNextItem(hlist, row, LVNI_SELECTED)) >= 0) {
        int a = keylist_action_for(keylist_row_data(hlist, row), NULL);
        if (a != KLBTN_NONE)
            return a;
    }
    return KLBTN_NONE;
}

/* Retext + enable the action button from the current selection. */
/* KiTTY: the confirm mode is shown in two places - the key list's radios and
 * the tray item's tick - and either can change it. Each change syncs the
 * other, so an open key list does not keep showing the old answer. */
/* KiTTY: a key-use tint appeared or changed - repaint the list and keep a
 * timer running while any tint is live, so the row clears itself. */
#define KEYLIST_FLASH_TIMER 0x4B46
void kageant_keylist_flash_changed(void)
{
    if (!keylist)
        return;
    InvalidateRect(GetDlgItem(keylist, IDC_KEYLIST_LISTBOX), NULL, FALSE);
    SetTimer(keylist, KEYLIST_FLASH_TIMER, 150, NULL);
}

static void keylist_sync_confirm_radios(void)
{
    int m;
    if (!keylist)
        return;
    m = kageant_confirm_mode();
    CheckRadioButton(keylist, IDC_KEYLIST_CONFIRM_YES, IDC_KEYLIST_CONFIRM_NO,
                     m == KAGEANT_CONFIRM_YES ? IDC_KEYLIST_CONFIRM_YES :
                     m == KAGEANT_CONFIRM_NO  ? IDC_KEYLIST_CONFIRM_NO  :
                                                IDC_KEYLIST_CONFIRM_AUTO);
}

static void tray_sync_confirm_check(void)
{
    if (systray_menu)
        CheckMenuItem(systray_menu, IDM_CONFIRM_KEYUSE,
                      MF_BYCOMMAND |
                      (kageant_confirm_get() ? MF_CHECKED : MF_UNCHECKED));
}

static void keylist_refresh_actionbtn(HWND dlg)
{
    HWND hlist = GetDlgItem(dlg, IDC_KEYLIST_LISTBOX);
    int mode = keylist_button_target(hlist);
    HWND btn = GetDlgItem(dlg, IDC_KEYLIST_REENCRYPT);
    SetWindowText(btn, mode == KLBTN_DECRYPT ? "&Decrypt" : "Re-e&ncrypt");
    EnableWindow(btn, mode != KLBTN_NONE);
}

/*
 * Update the visible key list.
 */
void keylist_update(void)
{
    if (!keylist)
        return;

    HWND hlist = GetDlgItem(keylist, IDC_KEYLIST_LISTBOX);

    /*
     * KiTTY: this is called at any time - device events and the agent both
     * trigger it while the window is open - so the rebuild must not lose the
     * user's place. Remember which keys were selected (and which had focus)
     * BY BLOB and re-select them afterwards; a row index would name a
     * different key once the list has changed.
     */
    int nsel = 0;
    strbuf **selblobs;
    strbuf *focusblob = NULL;
    {
        int nitems = ListView_GetItemCount(hlist);
        selblobs = snewn(nitems ? nitems : 1, strbuf *);
        for (int i = 0; i < nitems; i++) {
            UINT st = ListView_GetItemState(hlist, i,
                                            LVIS_SELECTED | LVIS_FOCUSED);
            if (!st)
                continue;
            struct keylist_display_data *disp = keylist_row_data(hlist, i);
            /* Pending rows have no blob; an empty blob would cross-match
             * every other pending row after the rebuild, so skip them. */
            if (!disp || !disp->blob->len)
                continue;
            if (st & LVIS_SELECTED)
                selblobs[nsel++] = strbuf_dup(ptrlen_from_strbuf(disp->blob));
            if (st & LVIS_FOCUSED)
                focusblob = strbuf_dup(ptrlen_from_strbuf(disp->blob));
        }
    }

    SendMessage(hlist, WM_SETREDRAW, false, 0);
    keylist_rebuilding = true;   /* LVN_ITEMCHANGED must not read freed rows */

    keylist_free_display_data(hlist);
    ListView_DeleteAllItems(hlist);

    char *errmsg;
    struct keylist_update_ctx ctx[1];
    ctx->hlist = hlist;
    ctx->index = 0;
    ctx->enable_remove_controls = false;
    ctx->enable_reencrypt_controls = false;
    int status = pageant_enum_keys(keylist_update_callback, ctx, &errmsg);
    assert(status == PAGEANT_ACTION_OK);
    assert(!errmsg);

    /*
     * KiTTY: append the startup entries that are NOT in the agent - file
     * absent (missing) or present but unloadable (failed) - so "N keys could
     * not be loaded" is answerable, and a dead entry can be Removed, without
     * digging in the registry or the ini. Toggleable, on by default.
     */
    if (keylist_show_unavail) {
        int np = kageant_pending_count();
        for (int i = 0; i < np; i++) {
            const char *path, *fp;
            int enc, failed;
            if (!kageant_pending_get(i, &path, &enc, &fp, &failed))
                continue;
            struct keylist_display_data *disp =
                snew(struct keylist_display_data);
            disp->alg = strbuf_new();
            put_dataz(disp->alg, "(not loaded)");
            disp->bits = strbuf_new();
            disp->hash = strbuf_new();
            if (fp && *fp)
                put_dataz(disp->hash, fp);
            disp->comment = strbuf_new();
            disp->info = strbuf_new();
            /* KiTTY: a mismatch outranks the other two - the file IS there and
             * IS readable, it is simply not the key we recorded, and that is
             * the one the user has to act on. */
            int mism = kageant_pending_mismatch(i);
            /* "mismatch", not "changed!": the notices send the user here to
             * look for that word. Not "fingerprint mismatch" - the State
             * column is 66du and would show "fingerpri...", which says
             * nothing. The details dialog carries the full sentence. */
            put_dataz(disp->info, mism ? "mismatch" :
                      failed ? "failed" : "missing");
            disp->expires = strbuf_new();  /* not in the agent: no lifetime */
            disp->blob = strbuf_new();
            disp->state = mism ? KEYSTATE_MISMATCH :
                          failed ? KEYSTATE_FAILED : KEYSTATE_MISSING;
            for (size_t t = 0; t < SSH_N_FPTYPES; t++)
                disp->fp_full[t] = NULL;
            disp->confirm = 0;
            disp->pending = 1;
            disp->pendpath = dupstr(path);
            disp->ssh_version = 0;

            LVITEM lvi;
            memset(&lvi, 0, sizeof(lvi));
            lvi.mask = LVIF_TEXT | LVIF_PARAM;
            lvi.iItem = ctx->index;
            lvi.pszText = disp->alg->s;
            lvi.lParam = (LPARAM)disp;
            int row = ListView_InsertItem(hlist, &lvi);
            ListView_SetItemText(hlist, row, 2, disp->hash->s);
            ListView_SetItemText(hlist, row, 3, disp->info->s);
            /* The path is the only name these entries have - it goes in the
             * comment column, where the eye looks for "which key is this". */
            ListView_SetItemText(hlist, row, 6, disp->pendpath);
            ctx->index++;
            /* Removing a dead entry is the point of showing them. */
            ctx->enable_remove_controls = true;
        }
    }

    /* Re-select what was selected before the rebuild. */
    if (nsel || focusblob) {
        int nitems = ListView_GetItemCount(hlist);
        for (int i = 0; i < nitems; i++) {
            struct keylist_display_data *disp = keylist_row_data(hlist, i);
            if (!disp)
                continue;
            ptrlen blob = ptrlen_from_strbuf(disp->blob);
            UINT st = 0;
            for (int j = 0; j < nsel; j++) {
                if (ptrlen_eq_ptrlen(blob, ptrlen_from_strbuf(selblobs[j]))) {
                    st |= LVIS_SELECTED;
                    break;
                }
            }
            if (focusblob &&
                ptrlen_eq_ptrlen(blob, ptrlen_from_strbuf(focusblob)))
                st |= LVIS_FOCUSED;
            if (st)
                ListView_SetItemState(hlist, i, st, st);
        }
    }
    for (int j = 0; j < nsel; j++)
        strbuf_free(selblobs[j]);
    sfree(selblobs);
    if (focusblob)
        strbuf_free(focusblob);

    keylist_rebuilding = false;

    SendMessage(hlist, WM_SETREDRAW, true, 0);
    InvalidateRect(hlist, NULL, false);

    EnableWindow(GetDlgItem(keylist, IDC_KEYLIST_REMOVE),
                 ctx->enable_remove_controls);
    /* KiTTY: the Re-encrypt slot is now a state-sensitive Decrypt/Re-encrypt
     * button; its label and enabled state come from the selected key. */
    keylist_refresh_actionbtn(keylist);
    /* KiTTY: "Retry unavailable keys" only means something while a key is waiting for
     * its file. Rebuilt here because the pending list is exactly what this
     * refresh has just read. */
    EnableWindow(GetDlgItem(keylist, IDC_KEYLIST_RETRY),
                 kageant_pending_count() > 0);
}

void win_add_keyfile(Filename *filename, bool encrypted)
{
    char *err;
    int ret;

    /*
     * KiTTY: arm the backstop that scrubs cached passphrases (WM_TIMER,
     * TID_PASSPHRASE_CACHE). Re-armed on every add, so a run of adds keeps
     * pushing it out and an add that is abandoned half-way still ends with the
     * cache cleared instead of held until kageant exits.
     */
    if (traywindow) {
        int secs = kageant_passphrase_ttl();
        if (secs > 0)
            SetTimer(traywindow, TID_PASSPHRASE_CACHE, secs * 1000, NULL);
    }

    /*
     * Try loading the key without a passphrase. (Or rather, without a
     * _new_ passphrase; pageant_add_keyfile will take care of trying
     * all the passphrases we've already stored.)
     */
    ret = pageant_add_keyfile(filename, NULL, &err, encrypted);
    if (ret == PAGEANT_ACTION_OK) {
        kageant_track_keypath(filename_to_str(filename), encrypted);   /* KiTTY startup-keys */
        goto done;
    } else if (ret == PAGEANT_ACTION_FAILURE) {
        goto error;
    }

    /*
     * OK, a passphrase is needed, and we've been given the key
     * comment to use in the passphrase prompt.
     */
    while (1) {
        INT_PTR dlgret;
        struct PassphraseProcStruct pps;
        pps.modal = true;
        pps.help_topic = NULL;         /* this dialog has no help button */
        pps.dlgid = NULL;
        pps.passphrase = NULL;
        pps.comment = err;
        pps.over = GetForegroundWindow();   /* KiTTY: centre over the active window */
        dlgret = DialogBoxParam(
            hinst, MAKEINTRESOURCE(IDD_LOAD_PASSPHRASE),
            NULL, PassphraseProc, (LPARAM) &pps);
        modal_passphrase_hwnd = NULL;

        if (!dlgret) {
            burnstr(pps.passphrase);
            goto done;                 /* operation cancelled */
        }

        sfree(err);

        assert(pps.passphrase != NULL);

        ret = pageant_add_keyfile(filename, pps.passphrase, &err, false);
        burnstr(pps.passphrase);

        if (ret == PAGEANT_ACTION_OK) {
            kageant_track_keypath(filename_to_str(filename), encrypted);   /* KiTTY startup-keys */
            goto done;
        } else if (ret == PAGEANT_ACTION_FAILURE) {
            goto error;
        }
    }

  error:
    /*
     * KiTTY: say WHICH file. Upstream's message is the reason alone -
     * "Couldn't load this key (unable to open file)" - which is no help at all
     * when it appears at every start and the user has no idea which key it
     * means (cyd01/KiTTY#522). The path is right here and was simply not used.
     *
     * And when the attempt came from the startup list rather than from someone
     * choosing a file, offer to drop it: that is the case that repeats every
     * single start, and the entry is otherwise only reachable by hand-editing
     * kitty.ini or the registry.
     *
     * NOT under #ifdef MOD_PERSO: this file is built only into the pageant
     * target (packaged as kageant.exe), which does not define it - the build's
     * own coverage check refuses guarded code here, because it would silently
     * vanish. kitty/kitty_pageant.c is in that target's sources, so the
     * kageant_* calls below link.
     */
    {
        const char *path = filename_to_str(filename);
        /* Note there is no "quiet" case here. A key that reaches this point
         * EXISTS and would not load - a broken or wrong-format file - which is
         * something to tell the user about however they have configured
         * missing keys. [Agent] quietmissingkeys covers absent files, which are
         * expected on removable media, and nothing else. */
        if (kageant_startup_loading()) {
            /*
             * A NOTICE, not a message box.
             *
             * This fires while kageant is loading its own remembered keys -
             * nobody clicked anything - and a modal here stops the agent's
             * message loop, so the agent answers no requests at all until
             * somebody dismisses it. Measured 2026-08-13: an ssh-add against
             * the pipe hung indefinitely while this box was up at startup, and
             * every ssh/git/scp call would have done the same. An informational
             * dialog must never be able to take the agent off the air.
             *
             * The offer it used to carry ("Remove it from that list?") is not
             * lost: the entry shows in the key list as a not-loaded row, and
             * Remove there drops it from the startup list - the same call this
             * box made. Clicking the notice opens that window.
             */
            char *msg = dupprintf(
                "%s\n\n%s\n\n"
                "It is still in the key list, and kageant will try it again at "
                "the next start. Click to open the list, where Remove drops it "
                "for good.", err, path);
            if (traywindow)
                kitty_notice_show("kageant: a remembered key did not load",
                                  msg, KAGEANT_NOTICE_WARN,
                                  kageant_notice_seconds(12), traywindow,
                                  KAGEANT_WM_NOTICE_CLICK);
            sfree(msg);
        } else {
            char *msg = dupprintf("%s\n\n    %s", err, path);
            message_box(traywindow, msg, APPNAME, MB_OK | MB_ICONERROR, false,
                        HELPCTXID(errors_cantloadkey));
            sfree(msg);
        }
    }
  done:
    sfree(err);
    return;
}

/*
 * Prompt for a key file to add, and add it.
 */
static void prompt_add_keyfile(bool encrypted)
{
    if (!keypath)
        keypath = filereq_saved_dir_new();

    struct request_multi_file_return *rmf = request_multi_file(
        traywindow, "Select Private Key File", NULL, false,
        keypath, true, FILTER_KEY_FILES);

    if (rmf) {
        for (size_t i = 0; i < rmf->nfilenames; i++)
            win_add_keyfile(rmf->filenames[i], encrypted);
        request_multi_file_free(rmf);

        keylist_update();
        pageant_forget_passphrases();
    }
}

/*
 * KiTTY: resizable key-list window.
 *
 * Each control is anchored to the dialog edges: the list stretches both
 * ways, everything below it rides the bottom edge, and Re-encrypt/Remove
 * and the Close button also ride the right edge. Base positions are
 * captured once at the end of WM_INITDIALOG (after the ini-mode rows are
 * collapsed), and WM_SIZE places everything relative to them - so the
 * layout follows the template and does not need touching when a control
 * is added there.
 *
 * Geometry and column widths persist through the [Agent] settings layer
 * (keylistgeometry / keylistcolumns) rather than the AuxWinPos registry
 * mechanism, so a portable install remembers them in kitty.ini instead of
 * not at all. A remembered position is clamped onto the nearest monitor's
 * work area, the same rule the terminal windows follow, so a monitor that
 * no longer exists cannot strand the window off-screen.
 */
#define KL_ANCH_LEFT   1
#define KL_ANCH_TOP    2
#define KL_ANCH_RIGHT  4
#define KL_ANCH_BOTTOM 8
struct kl_anchor { int id; unsigned anchor; };

/* Capture the current client size, each anchored control's rect, and the
 * window size (= the minimum) as the baseline WM_SIZE re-places against. */
static void anchored_capture(HWND hwnd, const struct kl_anchor *anchors,
                             size_t n, RECT *rects, SIZE *basesize,
                             SIZE *minsize)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    basesize->cx = rc.right - rc.left;
    basesize->cy = rc.bottom - rc.top;
    for (size_t i = 0; i < n; i++) {
        HWND c = GetDlgItem(hwnd, anchors[i].id);
        RECT r = {0, 0, 0, 0};
        if (c) {
            GetWindowRect(c, &r);
            MapWindowPoints(NULL, hwnd, (POINT *)&r, 2);
        }
        rects[i] = r;
    }
    GetWindowRect(hwnd, &rc);
    minsize->cx = rc.right - rc.left;
    minsize->cy = rc.bottom - rc.top;
}

static void anchored_relayout(HWND hwnd, const struct kl_anchor *anchors,
                              size_t n, const RECT *rects, SIZE basesize)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    int dx = (rc.right - rc.left) - basesize.cx;
    int dy = (rc.bottom - rc.top) - basesize.cy;
    HDWP hdwp = BeginDeferWindowPos((int)n);
    for (size_t i = 0; i < n; i++) {
        HWND c = GetDlgItem(hwnd, anchors[i].id);
        if (!c)
            continue;                  /* e.g. Help destroyed when no help */
        unsigned a = anchors[i].anchor;
        RECT r = rects[i];
        int x = r.left +
            (((a & KL_ANCH_RIGHT) && !(a & KL_ANCH_LEFT)) ? dx : 0);
        int y = r.top +
            (((a & KL_ANCH_BOTTOM) && !(a & KL_ANCH_TOP)) ? dy : 0);
        int w = (r.right - r.left) +
            (((a & KL_ANCH_LEFT) && (a & KL_ANCH_RIGHT)) ? dx : 0);
        int h = (r.bottom - r.top) +
            (((a & KL_ANCH_TOP) && (a & KL_ANCH_BOTTOM)) ? dy : 0);
        hdwp = DeferWindowPos(hdwp, c, NULL, x, y, w, h,
                              SWP_NOZORDER | SWP_NOACTIVATE);
    }
    EndDeferWindowPos(hdwp);
    InvalidateRect(hwnd, NULL, true);
}

static const struct kl_anchor keylist_anchors[] = {
    {IDC_KEYLIST_LISTBOX,
     KL_ANCH_LEFT | KL_ANCH_TOP | KL_ANCH_RIGHT | KL_ANCH_BOTTOM},
    {IDC_KEYLIST_FPTYPE_STATIC, KL_ANCH_LEFT | KL_ANCH_BOTTOM},
    {IDC_KEYLIST_FPTYPE,        KL_ANCH_LEFT | KL_ANCH_BOTTOM},
    {IDC_KEYLIST_SHOWUNAVAIL,   KL_ANCH_LEFT | KL_ANCH_BOTTOM},
    {IDC_KEYLIST_ADDKEY,        KL_ANCH_LEFT | KL_ANCH_BOTTOM},
    {IDC_KEYLIST_ADDKEY_ENC,    KL_ANCH_LEFT | KL_ANCH_BOTTOM},
    {IDC_KEYLIST_MOVEUP,        KL_ANCH_LEFT | KL_ANCH_BOTTOM},
    {IDC_KEYLIST_MOVEDOWN,      KL_ANCH_LEFT | KL_ANCH_BOTTOM},
    {IDC_KEYLIST_REENCRYPT,     KL_ANCH_RIGHT | KL_ANCH_BOTTOM},
    {IDC_KEYLIST_REMOVE,        KL_ANCH_RIGHT | KL_ANCH_BOTTOM},
    {IDC_KEYLIST_INISTATUS,
     KL_ANCH_LEFT | KL_ANCH_RIGHT | KL_ANCH_BOTTOM},
    {IDC_KEYLIST_CONFIRM_LABEL, KL_ANCH_LEFT | KL_ANCH_BOTTOM},
    {IDC_KEYLIST_CONFIRM_YES,   KL_ANCH_LEFT | KL_ANCH_BOTTOM},
    {IDC_KEYLIST_CONFIRM_AUTO,  KL_ANCH_LEFT | KL_ANCH_BOTTOM},
    {IDC_KEYLIST_CONFIRM_NO,    KL_ANCH_LEFT | KL_ANCH_BOTTOM},
    {IDC_KEYLIST_HELP,          KL_ANCH_LEFT | KL_ANCH_BOTTOM},
    {IDC_KEYLIST_NEWKEY,        KL_ANCH_LEFT | KL_ANCH_BOTTOM},
    {IDC_KEYLIST_SETTINGS,      KL_ANCH_LEFT | KL_ANCH_BOTTOM},
    {IDC_KEYLIST_STOPAGENT,     KL_ANCH_LEFT | KL_ANCH_BOTTOM},
    {IDC_KEYLIST_ABOUT,         KL_ANCH_LEFT | KL_ANCH_BOTTOM},
    {IDC_KEYLIST_RESUMECONFIRM, KL_ANCH_RIGHT | KL_ANCH_BOTTOM},
    {IDC_KEYLIST_RETRY,         KL_ANCH_RIGHT | KL_ANCH_BOTTOM},
    {IDOK,                      KL_ANCH_RIGHT | KL_ANCH_BOTTOM},
};
static RECT keylist_baserects[lenof(keylist_anchors)];
static SIZE keylist_basesize;      /* client size the base rects refer to */
static SIZE keylist_minsize;       /* window minimum = the template's size */
static bool keylist_layout_ready = false;

#define KL_GEOM_INIKEY "keylistgeometry"
#define KL_GEOM_REGVAL "KeyListGeometry"
#define KL_COLS_INIKEY "keylistcolumns"
#define KL_COLS_REGVAL "KeyListColumns"
#define KL_NCOLS 7

static void keylist_capture_layout(HWND hwnd)
{
    anchored_capture(hwnd, keylist_anchors, lenof(keylist_anchors),
                     keylist_baserects, &keylist_basesize, &keylist_minsize);
    /* KiTTY: the template height would pin the minimum window at ~10 rows.
     * Lower it by the list's excess so the user can shrink the window until
     * the list shows about 8 rows - everything below the list keeps its
     * height. Header/row metrics are not up yet at capture time, so estimate
     * a row when the real one is unavailable. */
    {
        HWND hlist = GetDlgItem(hwnd, IDC_KEYLIST_LISTBOX);
        if (hlist) {
            RECT lr;
            GetClientRect(hlist, &lr);
            int list_h = lr.bottom - lr.top;
            int hdr_h = 0, row_h = 0;
            HWND hh = ListView_GetHeader(hlist);
            if (hh) { RECT hr; GetClientRect(hh, &hr); hdr_h = hr.bottom - hr.top; }
            RECT ir;
            if (ListView_GetItemCount(hlist) > 0 &&
                ListView_GetItemRect(hlist, 0, &ir, LVIR_BOUNDS))
                row_h = ir.bottom - ir.top;
            if (row_h <= 0) row_h = 18;          /* default-font estimate */
            if (hdr_h <= 0) hdr_h = row_h;
            int want = hdr_h + 8 * row_h + 4;    /* 8 rows + a little slack */
            if (want < list_h)
                keylist_minsize.cy -= (list_h - want);
        }
    }
    keylist_layout_ready = true;
}

static void keylist_relayout(HWND hwnd)
{
    anchored_relayout(hwnd, keylist_anchors, lenof(keylist_anchors),
                      keylist_baserects, keylist_basesize);
}

/* KiTTY: the details dialog resizes too (long fingerprints, long paths).
 * Fields stretch with the right edge; the paths box gets the extra height.
 * Only one details dialog exists at a time (it is modal), so statics. */
static const struct kl_anchor keydetail_anchors[] = {
    {IDC_KEYDETAIL_KEY,     KL_ANCH_LEFT | KL_ANCH_TOP | KL_ANCH_RIGHT},
    {IDC_KEYDETAIL_STATE,   KL_ANCH_LEFT | KL_ANCH_TOP | KL_ANCH_RIGHT},
    {IDC_KEYDETAIL_FPS,     KL_ANCH_LEFT | KL_ANCH_TOP | KL_ANCH_RIGHT},
    {IDC_KEYDETAIL_COMMENT, KL_ANCH_LEFT | KL_ANCH_TOP | KL_ANCH_RIGHT},
    {IDC_KEYDETAIL_PATHS,
     KL_ANCH_LEFT | KL_ANCH_TOP | KL_ANCH_RIGHT | KL_ANCH_BOTTOM},
    {IDC_KEYDETAIL_LOCATE,  KL_ANCH_RIGHT | KL_ANCH_TOP},
    {IDC_KEYDETAIL_LIFETIME_LBL, KL_ANCH_LEFT | KL_ANCH_BOTTOM},
    {IDC_KEYDETAIL_LIFETIME,
     KL_ANCH_LEFT | KL_ANCH_RIGHT | KL_ANCH_BOTTOM},
    {IDC_KEYDETAIL_DEFER,   KL_ANCH_LEFT | KL_ANCH_BOTTOM},
    {IDC_KEYDETAIL_CONFIRM_LBL, KL_ANCH_LEFT | KL_ANCH_BOTTOM},
    {IDC_KEYDETAIL_CONFIRM, KL_ANCH_LEFT | KL_ANCH_BOTTOM},
    {IDOK,                  KL_ANCH_RIGHT | KL_ANCH_BOTTOM},
};
static RECT keydetail_baserects[lenof(keydetail_anchors)];
static SIZE keydetail_basesize, keydetail_minsize;
static bool keydetail_layout_ready = false;

/* KiTTY: the Lifetime line ticks while the (modal, hence single) details
 * dialog is open. The blob is OUR copy: a list rebuild while the dialog is
 * up frees the row data the dialog was opened from. */
static strbuf *keydetail_blob = NULL;
static bool keydetail_had_lifetime = false;
/* KiTTY: the fingerprint RECORDED for a mismatch row, kept for the Accept
 * question - which shows what was recorded next to what the file holds now, so
 * the user is comparing the two things rather than trusting a sentence. */
static char keydetail_stored_fp[160] = "";
#define TID_KEYDETAIL_LIFETIME 1

static void keydetail_show_lifetime(HWND hwnd)
{
    unsigned set_s, rem_s;
    if (!keydetail_blob || !keydetail_blob->len) {
        SetDlgItemText(hwnd, IDC_KEYDETAIL_LIFETIME, "not loaded");
    } else if (kageant_key_lifetime_get(ptrlen_from_strbuf(keydetail_blob),
                                        &set_s, &rem_s)) {
        char setbuf[32], rembuf[32], line[96];
        kageant_fmt_seconds(set_s, setbuf, sizeof(setbuf));
        kageant_fmt_seconds(rem_s, rembuf, sizeof(rembuf));
        snprintf(line, sizeof(line), "set to %s - %s remaining",
                 setbuf, rembuf);
        SetDlgItemText(hwnd, IDC_KEYDETAIL_LIFETIME, line);
    } else {
        /* No entry: either never had a lifetime, or it just ran out
         * while this dialog was open and the key is gone. */
        SetDlgItemText(hwnd, IDC_KEYDETAIL_LIFETIME,
                       keydetail_had_lifetime ? "expired - key removed"
                                              : "unlimited");
    }
}

static void keylist_save_geometry(HWND hwnd)
{
    if (IsIconic(hwnd) || IsZoomed(hwnd))
        return;
    RECT r;
    if (GetWindowRect(hwnd, &r)) {
        char buf[64];
        sprintf(buf, "%ld,%ld,%ld,%ld", (long)r.left, (long)r.top,
                (long)(r.right - r.left), (long)(r.bottom - r.top));
        kageant_setting_str_set(KL_GEOM_INIKEY, KL_GEOM_REGVAL, buf);
    }
    HWND hlist = GetDlgItem(hwnd, IDC_KEYLIST_LISTBOX);
    if (hlist) {
        char cols[80];
        sprintf(cols, "%d,%d,%d,%d,%d,%d,%d",
                ListView_GetColumnWidth(hlist, 0),
                ListView_GetColumnWidth(hlist, 1),
                ListView_GetColumnWidth(hlist, 2),
                ListView_GetColumnWidth(hlist, 3),
                ListView_GetColumnWidth(hlist, 4),
                ListView_GetColumnWidth(hlist, 5),
                ListView_GetColumnWidth(hlist, 6));
        kageant_setting_str_set(KL_COLS_INIKEY, KL_COLS_REGVAL, cols);
    }
}

/* Apply a remembered window position/size, clamped onto the nearest
 * monitor's work area. Returns false if nothing (usable) is stored, in
 * which case the caller centres the window as it always did. */
static bool keylist_restore_geometry(HWND hwnd)
{
    char buf[64];
    int x, y, w, h;
    if (!kageant_setting_str_get(KL_GEOM_INIKEY, KL_GEOM_REGVAL,
                                 buf, sizeof(buf)))
        return false;
    if (sscanf(buf, "%d,%d,%d,%d", &x, &y, &w, &h) != 4)
        return false;
    if (w < keylist_minsize.cx) w = keylist_minsize.cx;
    if (h < keylist_minsize.cy) h = keylist_minsize.cy;
    RECT want;
    SetRect(&want, x, y, x + w, y + h);
    HMONITOR mon = MonitorFromRect(&want, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    if (mon && GetMonitorInfo(mon, &mi)) {
        RECT wk = mi.rcWork;
        if (w > wk.right - wk.left) w = wk.right - wk.left;
        if (h > wk.bottom - wk.top) h = wk.bottom - wk.top;
        if (x + w > wk.right)  x = wk.right - w;
        if (y + h > wk.bottom) y = wk.bottom - h;
        if (x < wk.left) x = wk.left;
        if (y < wk.top)  y = wk.top;
    }
    SetWindowPos(hwnd, NULL, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    return true;
}

/*
 * KiTTY: details for one key, on double-click or Enter - a real dialog, not
 * a MessageBox, because the point of looking at a fingerprint is usually to
 * copy it and compare it with one somewhere else: every field is a read-only
 * edit control. Shows ALL fingerprint forms at once, the comment in full,
 * the state, and WHICH FILE(s) the key came from - the list shows a comment,
 * which is whatever was typed when the key was made, and says nothing about
 * which of several similar files is loaded.
 *
 * A deferred key with a known file can be decrypted on the spot with "Load
 * key now": re-adding the unencrypted form is the upstream-supported way to
 * decrypt a key that is present encrypted, so this goes through the same
 * win_add_keyfile path as the Add Key button, passphrase prompt included.
 *
 * Everything is copied OUT of the display struct in WM_INITDIALOG: the key
 * list can rebuild behind this modal dialog (device events keep arriving),
 * and the struct dies with the rebuild.
 */
static INT_PTR CALLBACK KeyDetailsProc(HWND hwnd, UINT msg,
                                       WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
      case WM_INITDIALOG: {
        struct keylist_display_data *disp =
            (struct keylist_display_data *)lParam;

        kageant_set_window_icon(hwnd);
        {
            char *t = kitty_title_compose("kageant - key details",
                                          kitty_inilight_portable(),
                                          restricted_acl(), false);
            SetWindowText(hwnd, t);
            sfree(t);
        }

        char *key = dupprintf("%.*s%s%.*s%s",
                              (int)disp->alg->len, disp->alg->s,
                              disp->bits->len ? " " : "",
                              (int)disp->bits->len, disp->bits->s,
                              disp->bits->len ? " bits" : "");
        SetDlgItemText(hwnd, IDC_KEYDETAIL_KEY, key);
        sfree(key);

        SetDlgItemText(hwnd, IDC_KEYDETAIL_STATE,
                       disp->state == KEYSTATE_ENCRYPTED ?
                           "encrypted - the passphrase is asked for at "
                           "first use" :
                       disp->state == KEYSTATE_REENCRYPTABLE ?
                           "loaded, and the key file it came from is "
                           "encrypted" :
                       disp->state == KEYSTATE_MISSING ?
                           "not loaded - the key file is not reachable "
                           "(absent media, or a path that no longer exists)" :
                       disp->state == KEYSTATE_FAILED ?
                           "not loaded - the file is present but would not "
                           "load" :
                       disp->state == KEYSTATE_MISMATCH ?
                           "NOT loaded - the file at this path is not the key "
                           "recorded for it. Either you replaced it, or "
                           "something else did. If you replaced it, use "
                           "\"Accept this key\" below; until then it stays "
                           "refused at every start and every re-plug." :
                           "loaded and ready to use");

        {
            /* All fingerprint forms at once, SHA-256 first; certificate
             * forms exist only for keys that carry a certificate. */
            static const struct { FingerprintType t; const char *label; }
            fporder[] = {
                {SSH_FPTYPE_SHA256, "SHA-256:  "},
                {SSH_FPTYPE_MD5, "MD5:  "},
                {SSH_FPTYPE_SHA256_CERT, "SHA-256 incl. certificate:  "},
                {SSH_FPTYPE_MD5_CERT, "MD5 incl. certificate:  "},
            };
            strbuf *sb = strbuf_new();
            for (size_t i = 0; i < lenof(fporder); i++) {
                if (!disp->fp_full[fporder[i].t])
                    continue;
                if (sb->len)
                    put_dataz(sb, "\r\n");
                put_dataz(sb, fporder[i].label);
                put_dataz(sb, disp->fp_full[fporder[i].t]);
            }
            /* A not-loaded entry has no live key to fingerprint; show what
             * the startup list recorded when it was last saved, if anything. */
            if (!sb->len && disp->hash->len) {
                put_dataz(sb, "recorded at last save:  ");
                put_dataz(sb, disp->hash->s);
            }
            SetDlgItemText(hwnd, IDC_KEYDETAIL_FPS, sb->s);
            strbuf_free(sb);
        }

        SetDlgItemText(hwnd, IDC_KEYDETAIL_COMMENT, disp->comment->s);

        if (disp->pending) {
            /* The stored path IS the identity of a not-loaded entry. */
            SetDlgItemText(hwnd, IDC_KEYDETAIL_PATHS, disp->pendpath);
        } else {
            /* kageant_paths_of_blob separates entries with "\n    " for the
             * old MessageBox layout; an edit control wants plain CRLFs. */
            char *paths = disp->blob->len ?
                kageant_paths_of_blob(ptrlen_from_strbuf(disp->blob)) : NULL;
            if (paths) {
                strbuf *sb = strbuf_new();
                for (const char *p = paths; *p; p++) {
                    if (*p == '\n') {
                        put_dataz(sb, "\r\n");
                        while (p[1] == ' ')
                            p++;
                    } else {
                        put_byte(sb, *p);
                    }
                }
                SetDlgItemText(hwnd, IDC_KEYDETAIL_PATHS, sb->s);
                strbuf_free(sb);
                sfree(paths);
            } else {
                SetDlgItemText(hwnd, IDC_KEYDETAIL_PATHS,
                               "not known - this key was added by another "
                               "program, or by a build that did not record "
                               "it");
            }
        }

        /* The key's tracked file path, kept in the window's user data for the
         * load-mode checkbox (any tracked key, pending ones included).
         * Decrypt/Re-encrypt now live on the key-list window's single action
         * button, not here. */
        char *keypath = NULL;
        if (disp->pending)
            keypath = dupstr(disp->pendpath);
        else if (disp->blob->len)
            keypath = kageant_file_of_blob(ptrlen_from_strbuf(disp->blob));
        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)keypath);

        /* KiTTY: how this key will be added the NEXT time the startup list
         * loads it. Greyed for keys the startup list does not track, and for
         * a passphraseless key - "deferred" means "passphrase at first use",
         * which is meaningless without a passphrase, so it always loads plain
         * and the box must not suggest otherwise. */
        {
            int mode = keypath ? kageant_startup_mode_get(keypath) : -1;
            bool can_defer = false;
            if (keypath) {
                Filename *fn = filename_from_str(keypath);
                char *cmt = NULL;
                can_defer = ppk_encrypted_f(fn, &cmt);
                filename_free(fn);
                sfree(cmt);
            }
            EnableWindow(GetDlgItem(hwnd, IDC_KEYDETAIL_DEFER),
                         mode >= 0 && can_defer);
            CheckDlgButton(hwnd, IDC_KEYDETAIL_DEFER,
                           (mode == 1 && can_defer) ?
                           BST_CHECKED : BST_UNCHECKED);
        }

        /* KiTTY: per-key confirm-on-use - a live agent-key setting, so it
         * needs a loaded key (pending rows have nothing to flag). Three
         * values (item index == mode 0/1/2), so a droplist: routed through
         * anything boolean the Hello mode would silently become "ask". */
        {
            static const char *const confirm_modes[] = {
                "no",
                "ask before each use",
                "ask with Windows Hello",
            };
            int cm;
            for (cm = 0; cm < (int)lenof(confirm_modes); cm++)
                SendDlgItemMessage(hwnd, IDC_KEYDETAIL_CONFIRM, CB_ADDSTRING,
                                   0, (LPARAM)confirm_modes[cm]);
            SendDlgItemMessage(hwnd, IDC_KEYDETAIL_CONFIRM, CB_SETCURSEL,
                               (disp->confirm >= 0 && disp->confirm <= 2) ?
                               disp->confirm : 0, 0);
            EnableWindow(GetDlgItem(hwnd, IDC_KEYDETAIL_CONFIRM),
                         !disp->pending);
        }

        /* KiTTY: accepting a changed key file. Only ever offered for a row
         * that IS a mismatch - this is the one place a new fingerprint can be
         * adopted, and it should not be reachable by accident anywhere else. */
        {
            HWND acc = GetDlgItem(hwnd, IDC_KEYDETAIL_ACCEPT);
            int is_mismatch = (disp->state == KEYSTATE_MISMATCH);
            ShowWindow(acc, is_mismatch ? SW_SHOW : SW_HIDE);
            EnableWindow(acc, is_mismatch);
            keydetail_stored_fp[0] = '\0';
            if (is_mismatch && disp->hash && disp->hash->len)
                snprintf(keydetail_stored_fp, sizeof(keydetail_stored_fp),
                         "%s", disp->hash->s);
        }

        /* KiTTY: re-pointing a not-loaded entry at a file the user browses
         * to. Absent or unparseable rows ONLY - never a mismatch, where the
         * file's presence is exactly the problem and Accept above is the one
         * sanctioned answer. */
        {
            HWND loc = GetDlgItem(hwnd, IDC_KEYDETAIL_LOCATE);
            int can_locate = disp->pending &&
                (disp->state == KEYSTATE_MISSING ||
                 disp->state == KEYSTATE_FAILED);
            ShowWindow(loc, can_locate ? SW_SHOW : SW_HIDE);
            EnableWindow(loc, can_locate);
        }

        /* KiTTY: the Lifetime line, ticking while the dialog is open. */
        keydetail_blob = strbuf_dup(ptrlen_from_strbuf(disp->blob));
        {
            unsigned set_s, rem_s;
            keydetail_had_lifetime = !disp->pending &&
                kageant_key_lifetime_get(ptrlen_from_strbuf(keydetail_blob),
                                         &set_s, &rem_s);
        }
        keydetail_show_lifetime(hwnd);
        if (keydetail_had_lifetime)
            SetTimer(hwnd, TID_KEYDETAIL_LIFETIME, 1000, NULL);

        /* Resize baseline (fields captured; the paths box takes the extra
         * height), then the same placement memory the About box uses,
         * anchored to the key list window it was opened from. */
        anchored_capture(hwnd, keydetail_anchors, lenof(keydetail_anchors),
                         keydetail_baserects, &keydetail_basesize,
                         &keydetail_minsize);
        keydetail_layout_ready = true;
        kitty_auxpos_apply(hwnd, "kageantKeyDetails",
                           GetWindow(hwnd, GW_OWNER), 0);
        return 1;
      }
      case WM_SIZE:
        if (keydetail_layout_ready && wParam != SIZE_MINIMIZED)
            anchored_relayout(hwnd, keydetail_anchors,
                              lenof(keydetail_anchors), keydetail_baserects,
                              keydetail_basesize);
        return 0;
      case WM_GETMINMAXINFO:
        if (keydetail_layout_ready && keydetail_minsize.cx) {
            MINMAXINFO *mmi = (MINMAXINFO *)lParam;
            mmi->ptMinTrackSize.x = keydetail_minsize.cx;
            mmi->ptMinTrackSize.y = keydetail_minsize.cy;
        }
        return 0;
      case WM_TIMER:
        if (wParam == TID_KEYDETAIL_LIFETIME)
            keydetail_show_lifetime(hwnd);
        return 0;
      case WM_DESTROY:
        KillTimer(hwnd, TID_KEYDETAIL_LIFETIME);
        if (keydetail_blob) {
            strbuf_free(keydetail_blob);
            keydetail_blob = NULL;
        }
        keydetail_layout_ready = false;
        return 0;
      case WM_COMMAND:
        switch (LOWORD(wParam)) {
          case IDC_KEYDETAIL_DEFER:
            /* KiTTY: flip how this key loads next time; takes effect in the
             * stored startup list immediately, current state untouched. */
            {
                char *keypath = (char *)GetWindowLongPtr(hwnd, GWLP_USERDATA);
                if (keypath)
                    kageant_startup_mode_set(
                        keypath,
                        IsDlgButtonChecked(hwnd, IDC_KEYDETAIL_DEFER) ==
                            BST_CHECKED);
            }
            return 0;
          case IDC_KEYDETAIL_CONFIRM:
            /* KiTTY: set the live per-key confirm MODE; the startup list
             * (which persists it as a ,confirm / ,helloconfirm token) is
             * rewritten when this key is tracked there. */
            if (HIWORD(wParam) == CBN_SELCHANGE &&
                keydetail_blob && keydetail_blob->len) {
                int sel = (int)SendDlgItemMessage(
                    hwnd, IDC_KEYDETAIL_CONFIRM, CB_GETCURSEL, 0, 0);
                if (sel >= 0 && sel <= 2) {
                    pageant_set_key_confirm(
                        ptrlen_from_strbuf(keydetail_blob), sel);
                    if (kageant_startup_get())
                        kageant_save_startup_keys();
                    keylist_update();
                }
            }
            return 0;
          case IDC_KEYDETAIL_ACCEPT: {
            /*
             * KiTTY: adopt a changed key file, deliberately.
             *
             * This is the consent that used to be a yes/no box during startup.
             * It asks here because the user came looking - they opened the key
             * list, opened this key, and pressed a button that only exists on a
             * mismatch row - and because the answer is permanent. Both
             * fingerprints are in the question: the one we recorded and the one
             * the file holds now.
             */
            char *keypath = (char *)GetWindowLongPtr(hwnd, GWLP_USERDATA);
            char *actual, *msg;
            int r;

            if (!keypath)
                return 0;
            actual = kageant_fp_of_file(keypath);
            if (!actual) {
                MessageBox(hwnd,
                           "The key file cannot be read right now, so there is "
                           "nothing to accept. Check the file is reachable and "
                           "try again.",
                           "kageant - cannot read that file",
                           MB_ICONWARNING | MB_OK);
                return 0;
            }
            msg = dupprintf(
                "Accept the key that is in this file NOW, and remember it?\n\n"
                "    %s\n\n"
                "Recorded before:  %s\n"
                "In the file now:  %s\n\n"
                "Only do this if YOU replaced the key. Accepting means this "
                "file is loaded now and trusted at every future start.",
                keypath, keydetail_stored_fp[0] ? keydetail_stored_fp :
                                                  "(none)", actual);
            r = MessageBox(hwnd, msg, "kageant - accept this changed key?",
                           MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2);
            sfree(msg);
            sfree(actual);
            if (r != IDYES)
                return 0;

            /* KiTTY: ratifying a changed key file is rare, deliberate and
             * permanent - with [Agent] helloconfirm on, it takes a Windows
             * Hello presence check on top of the Yes. FAIL CLOSED: an
             * unavailable Hello refuses, never downgrades to the click. */
            if (kageant_hello_get() &&
                kitty_hello_verify(hwnd, "Accept the changed key file and "
                                   "trust it from now on?")
                    != KITTY_HELLO_VERIFIED) {
                MessageBox(hwnd,
                           "The Windows Hello check did not verify, so the "
                           "key was NOT accepted and nothing was changed.",
                           "kageant - not accepted", MB_ICONWARNING | MB_OK);
                return 0;
            }

            if (!kageant_accept_pending_key(keypath)) {
                MessageBox(hwnd,
                           "The key could not be loaded, so nothing was "
                           "changed and the entry stays refused.",
                           "kageant - not accepted", MB_ICONWARNING | MB_OK);
                keylist_update();
                return 0;
            }
            keylist_update();
            kitty_auxpos_save(hwnd, "kageantKeyDetails");
            sfree(keypath);
            /* cleared, or the IDOK/WM_CLOSE paths free it a second time */
            SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)NULL);
            EndDialog(hwnd, 1);
            return 0;
          }
          case IDC_KEYDETAIL_LOCATE: {
            /*
             * KiTTY: browse for the file this entry should point at. The
             * heavy lifting - fingerprint check, in-place rewrite, collapsing
             * other unreachable locations of the same key - is
             * kageant_locate_pending_key(); this is only the file picker and
             * the two refusal messages.
             */
            char *keypath = (char *)GetWindowLongPtr(hwnd, GWLP_USERDATA);
            Filename *fn;
            char *newpath;
            int r;

            if (!keypath)
                return 0;
            fn = request_file(hwnd, "Locate the key file", NULL, false,
                              NULL, false, FILTER_KEY_FILES);
            if (!fn)
                return 0;
            newpath = dupstr(filename_to_str(fn));
            filename_free(fn);

            r = kageant_locate_pending_key(keypath, newpath);
            if (r == 1) {
                keylist_update();
                kitty_auxpos_save(hwnd, "kageantKeyDetails");
                sfree(newpath);
                sfree(keypath);
                /* cleared, or IDOK/WM_CLOSE free it a second time */
                SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)NULL);
                EndDialog(hwnd, 1);
                return 0;
            }
            MessageBox(hwnd,
                       r == 0 ?
                       "That file holds a DIFFERENT key, not the one recorded "
                       "for this entry - nothing was changed. Add Key loads "
                       "it as a new key; this button only re-points the entry "
                       "at its own key." :
                       "No key could be loaded from that file, so nothing "
                       "was changed.",
                       "kageant - not re-pointed", MB_ICONWARNING | MB_OK);
            sfree(newpath);
            keylist_update();
            return 0;
          }
          case IDOK:
          case IDCANCEL: {
            kitty_auxpos_save(hwnd, "kageantKeyDetails");
            char *loadpath = (char *)GetWindowLongPtr(hwnd, GWLP_USERDATA);
            sfree(loadpath);
            EndDialog(hwnd, 1);
            return 0;
          }
        }
        return 0;
      case WM_CLOSE: {
        kitty_auxpos_save(hwnd, "kageantKeyDetails");
        char *loadpath = (char *)GetWindowLongPtr(hwnd, GWLP_USERDATA);
        sfree(loadpath);
        EndDialog(hwnd, 1);
        return 0;
      }
    }
    return 0;
}

static void keylist_show_details(HWND hwnd, struct keylist_display_data *disp)
{
    if (!disp)
        return;
    DialogBoxParam(hinst, MAKEINTRESOURCE(IDD_KEYDETAILS), hwnd,
                   KeyDetailsProc, (LPARAM)disp);
}

/*
 * KiTTY: drag-and-drop reordering.
 *
 * The list order IS the offer order, and dragging a row is the direct way
 * to set it; Move Up/Down stay as the keyboard-accessible equivalent. The
 * dragged key is identified by its PUBLIC BLOB, never by its row index: the
 * list can rebuild mid-drag (device events), and the source row is
 * recomputed from the blob at the moment of the drop. The drop goes through
 * pageant_reorder_key() one step at a time and then persists through
 * kageant_save_key_order(), exactly as the buttons do.
 */
static bool keylist_dragging = false;
static strbuf *keylist_dragblob = NULL;

static int keylist_row_of_blob(HWND hlist, ptrlen blob)
{
    int nitems = ListView_GetItemCount(hlist);
    for (int i = 0; i < nitems; i++) {
        struct keylist_display_data *disp = keylist_row_data(hlist, i);
        if (disp && ptrlen_eq_ptrlen(ptrlen_from_strbuf(disp->blob), blob))
            return i;
    }
    return -1;
}

static void keylist_drag_cancel(HWND hwnd)
{
    if (!keylist_dragging)
        return;
    keylist_dragging = false;
    if (keylist_dragblob) {
        strbuf_free(keylist_dragblob);
        keylist_dragblob = NULL;
    }
    LVINSERTMARK im;
    im.cbSize = sizeof(im);
    im.dwFlags = 0;
    im.iItem = -1;
    ListView_SetInsertMark(GetDlgItem(hwnd, IDC_KEYLIST_LISTBOX), &im);
}

/* Where would a drop at dialog-client point pt land? Returns the row to
 * insert BEFORE (list length = append at the end), or -1 for nowhere. */
static int keylist_drop_target(HWND hwnd, POINT pt)
{
    HWND hlist = GetDlgItem(hwnd, IDC_KEYLIST_LISTBOX);
    MapWindowPoints(hwnd, hlist, &pt, 1);
    LVINSERTMARK im;
    im.cbSize = sizeof(im);
    im.dwFlags = 0;
    im.iItem = -1;
    if (ListView_InsertMarkHitTest(hlist, &pt, &im) && im.iItem >= 0)
        return im.iItem + ((im.dwFlags & LVIM_AFTER) ? 1 : 0);
    /* Above the first row means "to the front", below the last row (or in
     * the empty space of a short list) means "to the end". */
    RECT r0;
    if (ListView_GetItemCount(hlist) > 0 &&
        ListView_GetItemRect(hlist, 0, &r0, LVIR_BOUNDS) && pt.y < r0.top)
        return 0;
    LVHITTESTINFO ht;
    memset(&ht, 0, sizeof(ht));
    ht.pt = pt;
    if (ListView_HitTest(hlist, &ht) < 0) {
        RECT rc;
        GetClientRect(hlist, &rc);
        if (PtInRect(&rc, pt))
            return ListView_GetItemCount(hlist);
    }
    return -1;
}

/*
 * KiTTY: drop a key file on the window to add it (deferred, like the Add
 * Key (encrypted) button - a drop should never interrupt with a passphrase
 * prompt; the details dialog's "Load key now" decrypts on demand). A drop
 * lands on the deepest window that accepts files and goes no further, so
 * the ListView must accept and forward - it covers most of the dialog.
 */
static LRESULT CALLBACK keylist_lv_subclass(HWND hwnd, UINT msg,
                                            WPARAM wParam, LPARAM lParam,
                                            UINT_PTR id, DWORD_PTR ref)
{
    if (msg == WM_DROPFILES)
        return SendMessage(GetParent(hwnd), WM_DROPFILES, wParam, lParam);
    if (msg == WM_NCDESTROY)
        RemoveWindowSubclass(hwnd, keylist_lv_subclass, id);
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

/*
 * KiTTY: the [Agent] settings dialog, opened from the key list's Settings
 * button. It gathers the [Agent] options that are NOT already on the key
 * list window (confirm-key-use stays as the inline radios).
 *
 * The two side-effectful settings - OpenSSH integration and load-on-startup
 * - are routed through the existing tray-menu handlers when actually
 * changed, rather than reimplemented here: those handlers edit ~/.ssh files,
 * install autostart shortcuts and run conflict prompts, and there must be
 * exactly one copy of that logic. The removable-media options are honoured
 * only from kitty.ini, so they are greyed in a registry-authoritative
 * install.
 */
static INT_PTR CALLBACK KeySettingsProc(HWND hwnd, UINT msg,
                                        WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
      case WM_INITDIALOG:
        kageant_set_window_icon(hwnd);
        {
            char *t = kitty_title_compose("kageant - settings",
                                          kitty_inilight_portable(),
                                          restricted_acl(), false);
            SetWindowText(hwnd, t);
            sfree(t);
        }
        CheckDlgButton(hwnd, IDC_SET_OPENSSH,
            kageant_openssh_get() ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, IDC_SET_LOADKEYS,
                       kageant_startup_get() ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, IDC_SET_STARTUP,
            kageant_autostart_active() ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, IDC_SET_NOTIFY,
            kageant_notify_get() ? BST_CHECKED : BST_UNCHECKED);
        {
            /*
             * Retry is THREE-valued (item index == stored value 0/1/2), so it
             * is a droplist, not a checkbox - and it must never travel
             * through anything boolean on the way to the store, or the third
             * state collapses to "yes" silently.
             */
            static const char *const retry_modes[] = {
                "never",
                "from their stored drive and path",
                "from their stored path on any drive",
            };
            int rm, cur = kageant_retry_keys();
            for (rm = 0; rm < (int)lenof(retry_modes); rm++)
                SendDlgItemMessage(hwnd, IDC_SET_RETRY, CB_ADDSTRING, 0,
                                   (LPARAM)retry_modes[rm]);
            SendDlgItemMessage(hwnd, IDC_SET_RETRY, CB_SETCURSEL,
                               (cur >= 0 && cur <= KAGEANT_RETRY_ANYDRIVE) ?
                               cur : 1, 0);
        }
        CheckDlgButton(hwnd, IDC_SET_UNLOAD,
            kageant_unload_on_remove() ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, IDC_SET_QUIET,
            kageant_quiet_missing() ? BST_CHECKED : BST_UNCHECKED);
        SetDlgItemInt(hwnd, IDC_SET_TTL, kageant_passphrase_ttl(), FALSE);
        SendDlgItemMessage(hwnd, IDC_SET_TTL, EM_SETLIMITTEXT, 3, 0);
        /* Hello gating: greyed (and shown unticked) when Windows Hello has
         * no credential to check against - the availability probe is the
         * system's own answer. A remembered "yes" is preserved in the
         * store; only the control is disabled. */
        {
            int avail = kitty_hello_available();
            CheckDlgButton(hwnd, IDC_SET_HELLO,
                (avail == 1 && kageant_hello_get()) ? BST_CHECKED
                                                    : BST_UNCHECKED);
            EnableWindow(GetDlgItem(hwnd, IDC_SET_HELLO), avail == 1);
            if (avail != 1)
                SetDlgItemText(hwnd, IDC_SET_HELLO,
                               "Confirmations require Windows Hello "
                               "(not set up on this system)");
        }
        CheckDlgButton(hwnd, IDC_SET_LOCKDOWN,
            kageant_lockdown_get() ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, IDC_SET_BLOCKADD,
            kageant_blockadd_get() ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, IDC_SET_BLOCKREMOVE,
            kageant_blockremove_get() ? BST_CHECKED : BST_UNCHECKED);
        {
            int ns = kageant_notice_timeout_get();
            if (ns > 0)
                SetDlgItemInt(hwnd, IDC_SET_NOTICESECS, ns, FALSE);
        }
        SendDlgItemMessage(hwnd, IDC_SET_NOTICESECS, EM_SETLIMITTEXT, 3, 0);
        kitty_auxpos_apply(hwnd, "kageantSettings",
                           GetWindow(hwnd, GW_OWNER), 0);
        return 1;
      case WM_COMMAND:
        switch (LOWORD(wParam)) {
          case IDOK: {
            kageant_notify_set(
                IsDlgButtonChecked(hwnd, IDC_SET_NOTIFY) == BST_CHECKED);
            /* All agent settings write through to both stores now, so these
             * apply in either mode - no kitty.ini gate. */
            {
                int sel = (int)SendDlgItemMessage(hwnd, IDC_SET_RETRY,
                                                  CB_GETCURSEL, 0, 0);
                if (sel >= 0 && sel <= KAGEANT_RETRY_ANYDRIVE)
                    kageant_retry_keys_set(sel);
            }
            kageant_unload_on_remove_set(
                IsDlgButtonChecked(hwnd, IDC_SET_UNLOAD) == BST_CHECKED);
            kageant_quiet_missing_set(
                IsDlgButtonChecked(hwnd, IDC_SET_QUIET) == BST_CHECKED);
            {
                /* Blank field keeps the current value; the setter clamps. */
                BOOL ok = FALSE;
                UINT ttl = GetDlgItemInt(hwnd, IDC_SET_TTL, &ok, FALSE);
                if (ok)
                    kageant_passphrase_ttl_set((int)ttl);
            }
            /* KiTTY: IPC access control + notice timeout work in either store,
             * so they are always applied (not gated on a kitty.ini). */
            /* Only writable while the control is live - a greyed checkbox
             * must not overwrite the stored value with its display state. */
            if (IsWindowEnabled(GetDlgItem(hwnd, IDC_SET_HELLO)))
                kageant_hello_set(
                    IsDlgButtonChecked(hwnd, IDC_SET_HELLO) == BST_CHECKED);
            kageant_lockdown_set(
                IsDlgButtonChecked(hwnd, IDC_SET_LOCKDOWN) == BST_CHECKED);
            kageant_blockadd_set(
                IsDlgButtonChecked(hwnd, IDC_SET_BLOCKADD) == BST_CHECKED);
            kageant_blockremove_set(
                IsDlgButtonChecked(hwnd, IDC_SET_BLOCKREMOVE) == BST_CHECKED);
            {
                BOOL nok = FALSE;
                UINT ns = GetDlgItemInt(hwnd, IDC_SET_NOTICESECS, &nok, FALSE);
                if (nok)
                    kageant_notice_timeout_set((int)ns);   /* blank = unchanged */
            }
            /* Side-effectful toggles: fire the tray handler only on a real
             * change (it may prompt or refuse, and it is the authority). */
            int want = IsDlgButtonChecked(hwnd, IDC_SET_OPENSSH) == BST_CHECKED;
            if (want != (kageant_openssh_get() ? 1 : 0))
                SendMessage(traywindow, WM_COMMAND,
                            IDM_OPENSSH_INTEGRATION, 0);
            want = IsDlgButtonChecked(hwnd, IDC_SET_STARTUP) == BST_CHECKED;
            if (want != (kageant_autostart_active() ? 1 : 0))
                SendMessage(traywindow, WM_COMMAND, IDM_LOAD_ON_STARTUP, 0);
            want = IsDlgButtonChecked(hwnd, IDC_SET_LOADKEYS) == BST_CHECKED;
            if (want != (kageant_startup_get() ? 1 : 0))
                SendMessage(traywindow, WM_COMMAND, IDM_LOAD_KEYS, 0);
            kitty_auxpos_save(hwnd, "kageantSettings");
            EndDialog(hwnd, 1);
            return 0;
          }
          case IDCANCEL:
            kitty_auxpos_save(hwnd, "kageantSettings");
            EndDialog(hwnd, 0);
            return 0;
        }
        return 0;
      case WM_CLOSE:
        kitty_auxpos_save(hwnd, "kageantSettings");
        EndDialog(hwnd, 0);
        return 0;
    }
    return 0;
}

/*
 * Dialog-box function for the key list box.
 */
static INT_PTR CALLBACK KeyListProc(HWND hwnd, UINT msg,
                                    WPARAM wParam, LPARAM lParam)
{
    static const struct {
        const char *name;
        FingerprintType value;
    } fptypes[] = {
        {"SHA256", SSH_FPTYPE_SHA256},
        {"MD5", SSH_FPTYPE_MD5},
        {"SHA256 including certificate", SSH_FPTYPE_SHA256_CERT},
        {"MD5 including certificate", SSH_FPTYPE_MD5_CERT},
    };

    switch (msg) {
      case WM_INITDIALOG: {
        kageant_set_window_icon(hwnd);
        /* KiTTY: opening the key list lifts any confirm-suppress latch the
         * user set during a prompt storm - they are attending to the agent. */
        kageant_confirm_resume();
        /* The Resume button shows only while blocked; a 1s timer re-syncs it
         * if a storm re-engages the latch while this window stays open. */
        EnableWindow(GetDlgItem(hwnd, IDC_KEYLIST_RESUMECONFIRM),
                     kageant_confirm_suppressed());
        SetTimer(hwnd, TID_KL_RESUME, 1000, NULL);
        /* KiTTY: mark the key list with the agent's state - this is the
         * process holding the keys, so portable/restricted get answered
         * here. Suffixes composed in kitty/kitty_title.c - one place. */
        {
            char *t = kitty_title_compose("kageant Key List",
                                          kitty_inilight_portable(),
                                          restricted_acl(), false);
            SetWindowText(hwnd, t);
            sfree(t);
        }
        /* KiTTY: the confirm-mode radios are shown in BOTH stores' modes now
         * (registry gained the third state), so nothing is hidden here. */
        /* KiTTY: the layout baseline for resizing - captured now, after the
         * ini-mode rows above may have shrunk the template. */
        keylist_capture_layout(hwnd);

        /*
         * Centre the window - unless a remembered geometry applies.
         */
        if (!keylist_restore_geometry(hwnd)) {
            RECT rs, rd;
            HWND hw;

            hw = GetDesktopWindow();
            if (GetWindowRect(hw, &rs) && GetWindowRect(hwnd, &rd))
                MoveWindow(hwnd,
                           (rs.right + rs.left + rd.left - rd.right) / 2,
                           (rs.bottom + rs.top + rd.top - rd.bottom) / 2,
                           rd.right - rd.left, rd.bottom - rd.top, true);
        }

        if (has_help())
            SetWindowLongPtr(hwnd, GWL_EXSTYLE,
                             GetWindowLongPtr(hwnd, GWL_EXSTYLE) |
                             WS_EX_CONTEXTHELP);
        else {
            HWND item = GetDlgItem(hwnd, IDC_KEYLIST_HELP);
            if (item)
                DestroyWindow(item);
        }

        keylist = hwnd;

        /* KiTTY: New key launches kittygen - grey it if kittygen is not
         * beside us (e.g. a standalone kageant). */
        {
            char *g = find_kittygen();
            EnableWindow(GetDlgItem(hwnd, IDC_KEYLIST_NEWKEY), g != NULL);
            sfree(g);
        }

        /*
         * KiTTY: set up the ListView. Full-row select and grid lines are
         * extended styles, settable only at runtime; the column widths here
         * are first-open defaults in dialog units (so they scale with the
         * dialog font/DPI) - the user can drag the header dividers.
         */
        {
            static const struct { const char *title; int du; int fmt; }
            cols[] = {
                {"Algorithm", 62, LVCFMT_LEFT},
                {"Bits", 24, LVCFMT_RIGHT},
                {"Fingerprint", 154, LVCFMT_LEFT},
                /* KiTTY: wide enough for "mismatch" - the fingerprint beside
                 * it is elided anyway, this word is the one to read. */
                {"State", 66, LVCFMT_LEFT},
                {"Lifetime", 40, LVCFMT_RIGHT},
                {"Confirm", 44, LVCFMT_LEFT},
                {"Comment", 104, LVCFMT_LEFT},
            };
            HWND hlist = GetDlgItem(hwnd, IDC_KEYLIST_LISTBOX);
            ListView_SetExtendedListViewStyle(
                hlist, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES |
                LVS_EX_LABELTIP | LVS_EX_DOUBLEBUFFER);
            for (size_t i = 0; i < lenof(cols); i++) {
                RECT r;
                r.left = r.top = r.bottom = 0;
                r.right = cols[i].du;
                MapDialogRect(hwnd, &r);
                LVCOLUMN lvc;
                memset(&lvc, 0, sizeof(lvc));
                lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
                lvc.pszText = (char *)cols[i].title;
                lvc.cx = r.right;
                lvc.fmt = cols[i].fmt;
                ListView_InsertColumn(hlist, (int)i, &lvc);
            }

            /* KiTTY: accept key files dropped anywhere on the window. */
            DragAcceptFiles(hwnd, true);
            DragAcceptFiles(hlist, true);
            SetWindowSubclass(hlist, keylist_lv_subclass, 1, 0);

            /* Remembered column widths, if any, override the defaults. */
            char colstr[80];
            if (kageant_setting_str_get(KL_COLS_INIKEY, KL_COLS_REGVAL,
                                        colstr, sizeof(colstr))) {
                int cw[KL_NCOLS];
                if (sscanf(colstr, "%d,%d,%d,%d,%d,%d,%d",
                           &cw[0], &cw[1], &cw[2], &cw[3], &cw[4], &cw[5],
                           &cw[6]) == KL_NCOLS) {
                    for (int i = 0; i < KL_NCOLS; i++)
                        if (cw[i] >= 8)
                            ListView_SetColumnWidth(hlist, i, cw[i]);
                }
            }
        }

        /* KiTTY: the show-unavailable toggle, persisted like the rest. */
        {
            char b[16];
            keylist_show_unavail = true;
            if (kageant_setting_str_get("showunavailablekeys",
                                        "ShowUnavailableKeys",
                                        b, sizeof(b)) && !stricmp(b, "no"))
                keylist_show_unavail = false;
            CheckDlgButton(hwnd, IDC_KEYLIST_SHOWUNAVAIL,
                           keylist_show_unavail ? BST_CHECKED : BST_UNCHECKED);
        }

        int selection = 0;
        for (size_t i = 0; i < lenof(fptypes); i++) {
            SendDlgItemMessage(hwnd, IDC_KEYLIST_FPTYPE, CB_ADDSTRING,
                               0, (LPARAM)fptypes[i].name);
            if (fptype == fptypes[i].value)
                selection = (int)i;
        }
        SendDlgItemMessage(hwnd, IDC_KEYLIST_FPTYPE,
                           CB_SETCURSEL, 0, selection);

        keylist_update();

        /* KiTTY: the confirm radios always reflect the live mode; the ini-path
         * line is filled only when the ini is the authoritative store. */
        {
            int m = kageant_confirm_mode();
            CheckRadioButton(hwnd, IDC_KEYLIST_CONFIRM_YES,
                IDC_KEYLIST_CONFIRM_NO,
                m == KAGEANT_CONFIRM_YES ? IDC_KEYLIST_CONFIRM_YES :
                m == KAGEANT_CONFIRM_NO  ? IDC_KEYLIST_CONFIRM_NO  :
                                           IDC_KEYLIST_CONFIRM_AUTO);
        }
        if (kageant_ini_status()) {
            char *inimsg = dupprintf("Settings file (kitty.ini mode): %s",
                                     kageant_ini_status());
            SetDlgItemText(hwnd, IDC_KEYLIST_INISTATUS, inimsg);
            sfree(inimsg);
        }
        return 0;
      }
      case WM_SIZE:
        /* KiTTY: re-place every control against its anchors. */
        if (keylist_layout_ready && wParam != SIZE_MINIMIZED)
            keylist_relayout(hwnd);
        return 0;
      case WM_GETMINMAXINFO:
        /* KiTTY: the template size is the minimum - every control stays
         * reachable. (Arrives before WM_INITDIALOG too; 0 = not known yet.) */
        if (keylist_minsize.cx) {
            MINMAXINFO *mmi = (MINMAXINFO *)lParam;
            mmi->ptMinTrackSize.x = keylist_minsize.cx;
            mmi->ptMinTrackSize.y = keylist_minsize.cy;
        }
        return 0;
      case WM_EXITSIZEMOVE:
        /* KiTTY: persist geometry as soon as a move/resize ends, not only at
         * close - a tray Exit can end the process with this window open. */
        keylist_save_geometry(hwnd);
        return 0;
      case WM_NOTIFY: {
        /* KiTTY: ListView notifications. Double-click activates a row ->
         * details. (Enter arrives as IDOK instead - see WM_COMMAND.) */
        NMHDR *nm = (NMHDR *)lParam;
        if (nm->idFrom == IDC_KEYLIST_LISTBOX &&
            nm->code == LVN_ITEMACTIVATE) {
            NMITEMACTIVATE *ia = (NMITEMACTIVATE *)lParam;
            HWND hlist = GetDlgItem(hwnd, IDC_KEYLIST_LISTBOX);
            keylist_show_details(hwnd, keylist_row_data(hlist, ia->iItem));
        }
        if (nm->idFrom == IDC_KEYLIST_LISTBOX && nm->code == NM_CUSTOMDRAW) {
            /* KiTTY: see keylist_flash_paint below for the key-use tint. */
            /* KiTTY: paint the not-loaded rows grey - they are startup
             * entries, not keys the agent holds. */
            NMLVCUSTOMDRAW *cd = (NMLVCUSTOMDRAW *)lParam;
            LRESULT res = CDRF_DODEFAULT;
            if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) {
                res = CDRF_NOTIFYITEMDRAW;
            } else if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                struct keylist_display_data *disp =
                    (struct keylist_display_data *)cd->nmcd.lItemlParam;
                /* Any colour we set here is only honoured if we say we changed
                 * something: CDRF_DODEFAULT means "nothing to see", and the
                 * control then paints in its own colours. */
                if (disp && disp->pending) {
                    cd->clrText = GetSysColor(COLOR_GRAYTEXT);
                    res = CDRF_NEWFONT;
                }
                /* KiTTY: this key was just used - the whole row takes the
                 * accent the notice windows use, amber for refused and blue
                 * for signed, with white text on it as they have. Left alone
                 * in a high-contrast theme, where the system owns the
                 * colours. (Both colours only stick because this branch
                 * returns CDRF_NEWFONT; with CDRF_DODEFAULT the control keeps
                 * its own and the row paints as if nothing was set.) */
                HIGHCONTRASTA hc;
                hc.cbSize = sizeof(hc);
                if (!SystemParametersInfoA(SPI_GETHIGHCONTRAST,
                                           sizeof(hc), &hc, 0))
                    hc.dwFlags = 0;
                if (disp && !(hc.dwFlags & HCF_HIGHCONTRASTON)) {
                    int allowed = 1, hit = 0;
                    size_t t;
                    /* Any form the row holds: the core reports one of them and
                     * which index a key's fingerprints land under is not ours
                     * to assume. */
                    for (t = 0; t < SSH_N_FPTYPES && !hit; t++)
                        if (disp->fp_full[t] &&
                            kageant_flash_get(disp->fp_full[t], &allowed))
                            hit = 1;
                    if (hit) {
                        cd->clrTextBk = allowed ? KAGEANT_NOTICE_INFO
                                                : KAGEANT_NOTICE_WARN;
                        cd->clrText = RGB(255, 255, 255);
                        /* A SELECTED row is painted in the system selection
                         * colours and ignores clrTextBk/clrText entirely -
                         * which hid the tint on precisely the row the user had
                         * just clicked, Move Up/Down leaving it selected. Drop
                         * the selection bits for this paint so our colours are
                         * the ones that get used; the selection itself is
                         * untouched and reappears when the tint expires. */
                        cd->nmcd.uItemState &= ~(CDIS_SELECTED | CDIS_FOCUS);
                        res = CDRF_NEWFONT;
                    }
                }
            }
            SetWindowLongPtr(hwnd, DWLP_MSGRESULT, res);
            return 1;
        }
        if (nm->idFrom == IDC_KEYLIST_LISTBOX && nm->code == LVN_BEGINDRAG) {
            /* Start reordering by drag: remember WHICH KEY (by blob), not
             * which row, and capture the mouse; the drop lands in
             * WM_LBUTTONUP below. */
            NMLISTVIEW *nml = (NMLISTVIEW *)lParam;
            HWND hlist = GetDlgItem(hwnd, IDC_KEYLIST_LISTBOX);
            struct keylist_display_data *disp =
                keylist_row_data(hlist, nml->iItem);
            if (disp && disp->blob->len) {
                keylist_drag_cancel(hwnd);   /* stale state, just in case */
                keylist_dragblob =
                    strbuf_dup(ptrlen_from_strbuf(disp->blob));
                keylist_dragging = true;
                ListView_SetItemState(hlist, nml->iItem,
                                      LVIS_SELECTED | LVIS_FOCUSED,
                                      LVIS_SELECTED | LVIS_FOCUSED);
                SetCapture(hwnd);
            }
        }
        if (nm->idFrom == IDC_KEYLIST_LISTBOX &&
            nm->code == LVN_ITEMCHANGED && !keylist_rebuilding) {
            /* Selection moved: retext Decrypt/Re-encrypt for the new key.
             * Suppressed during a rebuild, when the rows are being freed. */
            keylist_refresh_actionbtn(hwnd);
        }
        return 0;
      }
      case WM_MOUSEMOVE:
        if (keylist_dragging) {
            /* Show where the drop would land. */
            POINT pt;
            pt.x = GET_X_LPARAM(lParam);
            pt.y = GET_Y_LPARAM(lParam);
            HWND hlist = GetDlgItem(hwnd, IDC_KEYLIST_LISTBOX);
            int dst = keylist_drop_target(hwnd, pt);
            LVINSERTMARK im;
            im.cbSize = sizeof(im);
            im.dwFlags = 0;
            im.iItem = -1;
            if (dst >= 0) {
                int nitems = ListView_GetItemCount(hlist);
                if (dst >= nitems) {
                    im.iItem = nitems - 1;
                    im.dwFlags = LVIM_AFTER;
                } else {
                    im.iItem = dst;
                }
            }
            ListView_SetInsertMark(hlist, &im);
            SetCursor(LoadCursor(NULL, IDC_SIZENS));
        }
        return 0;
      case WM_LBUTTONUP:
        if (keylist_dragging) {
            POINT pt;
            pt.x = GET_X_LPARAM(lParam);
            pt.y = GET_Y_LPARAM(lParam);
            HWND hlist = GetDlgItem(hwnd, IDC_KEYLIST_LISTBOX);
            /* Copy what the drop needs BEFORE cancelling clears it. */
            strbuf *blob = strbuf_dup(ptrlen_from_strbuf(keylist_dragblob));
            ReleaseCapture();              /* -> WM_CAPTURECHANGED cancels */
            keylist_drag_cancel(hwnd);     /* no-op if capture already did */
            int dst = keylist_drop_target(hwnd, pt);
            /* Recompute the source row NOW - the list can have rebuilt
             * since the drag began, so only the blob can be trusted. */
            int src = keylist_row_of_blob(hlist, ptrlen_from_strbuf(blob));
            if (src >= 0 && dst >= 0) {
                if (dst > src)
                    dst--;                 /* removing src shifts the rest up */
                int dir = (dst > src) ? 1 : -1;
                int steps = (dst > src) ? dst - src : src - dst;
                bool moved = false;
                while (steps-- > 0 &&
                       pageant_reorder_key(ptrlen_from_strbuf(blob), dir))
                    moved = true;
                if (moved) {
                    kageant_save_key_order();
                    keylist_update();
                }
            }
            strbuf_free(blob);
        }
        return 0;
      case WM_CAPTURECHANGED:
        /* Capture lost to someone else mid-drag: abandon the drag. */
        keylist_drag_cancel(hwnd);
        return 0;
      case WM_DROPFILES: {
        /* KiTTY: files dropped on the window are offered to the same path
         * as Add Key (encrypted) - several at once work, and a file that
         * is not a key is refused by the loader with a box naming it. */
        HDROP drop = (HDROP)wParam;
        if (modal_passphrase_hwnd) {
            MessageBeep(MB_ICONERROR);
            SetForegroundWindow(modal_passphrase_hwnd);
            DragFinish(drop);
            return 0;
        }
        UINT nfiles = DragQueryFile(drop, 0xFFFFFFFF, NULL, 0);
        for (UINT i = 0; i < nfiles; i++) {
            UINT len = DragQueryFile(drop, i, NULL, 0);
            char *path = snewn((size_t)len + 2, char);
            if (DragQueryFile(drop, i, path, len + 1)) {
                Filename *fn = filename_from_str(path);
                win_add_keyfile(fn, true);   /* deferred, like Add Key
                                              * (encrypted) */
                filename_free(fn);
            }
            sfree(path);
        }
        DragFinish(drop);
        keylist_update();
        pageant_forget_passphrases();
        SetForegroundWindow(hwnd);
        return 0;
      }
      case WM_COMMAND:
        switch (LOWORD(wParam)) {
          case IDOK:
            /* KiTTY: Enter with the list focused means "show details".
             * Closing the window because IDOK happens to be the default
             * button was a booby trap; Escape and the Close button still
             * close. */
            {
                HWND hlist = GetDlgItem(hwnd, IDC_KEYLIST_LISTBOX);
                if (GetFocus() == hlist) {
                    keylist_show_details(
                        hwnd, keylist_row_data(
                            hlist, ListView_GetNextItem(hlist, -1,
                                                        LVNI_FOCUSED)));
                    return 0;
                }
            }
            /* fall through */
          case IDCANCEL:
            keylist = NULL;
            DestroyWindow(hwnd);
            return 0;
          case IDC_KEYLIST_ADDKEY:
          case IDC_KEYLIST_ADDKEY_ENC:
            if (HIWORD(wParam) == BN_CLICKED ||
                HIWORD(wParam) == BN_DOUBLECLICKED) {
                if (modal_passphrase_hwnd) {
                    MessageBeep(MB_ICONERROR);
                    SetForegroundWindow(modal_passphrase_hwnd);
                    break;
                }
                prompt_add_keyfile(LOWORD(wParam) == IDC_KEYLIST_ADDKEY_ENC);
            }
            return 0;
          case IDC_KEYLIST_SHOWUNAVAIL:
            /* KiTTY: toggle the not-loaded rows; remembered. */
            keylist_show_unavail =
                IsDlgButtonChecked(hwnd, IDC_KEYLIST_SHOWUNAVAIL) ==
                BST_CHECKED;
            kageant_setting_str_set("showunavailablekeys",
                                    "ShowUnavailableKeys",
                                    keylist_show_unavail ? "yes" : "no");
            keylist_update();
            return 0;
          case IDC_KEYLIST_CONFIRM_YES:
            kageant_confirm_set_mode(KAGEANT_CONFIRM_YES);
            tray_sync_confirm_check();
            return 0;
          case IDC_KEYLIST_CONFIRM_AUTO:
            kageant_confirm_set_mode(KAGEANT_CONFIRM_AUTO);
            tray_sync_confirm_check();
            return 0;
          case IDC_KEYLIST_CONFIRM_NO:
            kageant_confirm_set_mode(KAGEANT_CONFIRM_NO);
            tray_sync_confirm_check();
            return 0;
          case IDC_KEYLIST_MOVEUP:
          case IDC_KEYLIST_MOVEDOWN:
            /* KiTTY: reorder the offer order. Acts on a single selected key;
             * persists the new order. The moved key stays selected at its new
             * row because keylist_update() re-selects by blob. */
            if (HIWORD(wParam) == BN_CLICKED ||
                HIWORD(wParam) == BN_DOUBLECLICKED) {
                HWND hlist = GetDlgItem(hwnd, IDC_KEYLIST_LISTBOX);
                if (ListView_GetSelectedCount(hlist) != 1) {
                    MessageBeep(0);            /* one key at a time */
                    break;
                }
                struct keylist_display_data *disp = keylist_row_data(
                    hlist, ListView_GetNextItem(hlist, -1, LVNI_SELECTED));
                if (!disp)
                    break;
                int dir = (LOWORD(wParam) == IDC_KEYLIST_MOVEUP) ? -1 : 1;
                if (pageant_reorder_key(ptrlen_from_strbuf(disp->blob), dir)) {
                    kageant_save_key_order();
                    keylist_update();
                }
            }
            return 0;
          case IDC_KEYLIST_REMOVE:
            if (HIWORD(wParam) == BN_CLICKED ||
                HIWORD(wParam) == BN_DOUBLECLICKED) {
                HWND hlist = GetDlgItem(hwnd, IDC_KEYLIST_LISTBOX);
                int numSelected = ListView_GetSelectedCount(hlist);

                /* none selected? that was silly */
                if (numSelected == 0) {
                    MessageBeep(0);
                    break;
                }

                /*
                 * KiTTY: ask before removing.
                 *
                 * Remove used to act instantly. It now does more than unload:
                 * the key stops being loaded at startup, and its place in the
                 * offer order goes with it - so a mis-click costs a
                 * configuration change, not just a reload.
                 */
                {
                    char *msg = dupprintf(
                        numSelected == 1 ?
                        "Remove the selected key from the agent?\n\n"
                        "It will also stop being loaded at startup." :
                        "Remove the %d selected keys from the agent?\n\n"
                        "They will also stop being loaded at startup.",
                        numSelected);
                    int r = MessageBox(hwnd, msg, APPNAME,
                                       MB_YESNO | MB_ICONQUESTION |
                                       MB_DEFBUTTON2);
                    sfree(msg);
                    if (r != IDYES)
                        break;
                }

                /*
                 * KiTTY: act on each selected row's PUBLIC BLOB, never on a
                 * list position: the displayed order and the agent's own
                 * order are different things (measured 2026-08-08 - an RSA
                 * key selected, the DSA key below it removed). Only an SSH-1
                 * key, which the by-blob calls do not cover, falls back to
                 * its position: SSH-1 keys enumerate first, so its row index
                 * IS its index among them. Rows are walked backwards so that
                 * positional fallback survives earlier deletions. Neither
                 * delete rebuilds the list, so iterating it here is safe.
                 */
                for (int row = ListView_GetItemCount(hlist) - 1; row >= 0;
                     row--) {
                    if (!(ListView_GetItemState(hlist, row, LVIS_SELECTED) &
                          LVIS_SELECTED))
                        continue;
                    struct keylist_display_data *disp =
                        keylist_row_data(hlist, row);
                    if (!disp)
                        continue;
                    /* A not-loaded startup entry has nothing in the agent:
                     * Remove drops it from the startup list (memory AND
                     * store). */
                    if (disp->pending) {
                        kageant_drop_pending(disp->pendpath);
                        continue;
                    }
                    ptrlen blob = ptrlen_from_strbuf(disp->blob);
                    if (disp->blob->len &&
                        pageant_delete_ssh2_key_by_blob(blob)) {
                        /* and stop loading it at every start - the startup
                         * list is otherwise only written when a key is ADDED,
                         * so a removed key came back. */
                        kageant_forget_loaded_by_blob(blob);
                    } else if (row < pageant_count_ssh1_keys()) {
                        pageant_delete_nth_ssh1_key(row);
                    }
                }
                keylist_update();
            }
            return 0;
          case IDC_KEYLIST_REENCRYPT:
            /*
             * KiTTY: the single state-sensitive action button. It has ONE
             * meaning - Decrypt or Re-encrypt, decided by the focused row and
             * shown on the button - and clicking it settles EVERY selected key
             * to that state. It does not flip each key on its own: with a
             * mixed selection and the button reading "Decrypt", every selected
             * key ends up decrypted (the encrypted ones load; the ones already
             * loaded are left alone), and vice-versa for "Re-encrypt".
             *
             * Unlike Remove, these actions re-add a key, which loops back into
             * the agent and rebuilds the ListView synchronously - so we must
             * NOT touch a row's disp struct after the first one. Gather each
             * matching key's blob COPY and file path FIRST; then act.
             */
            if (HIWORD(wParam) == BN_CLICKED ||
                HIWORD(wParam) == BN_DOUBLECLICKED) {
                HWND hlist = GetDlgItem(hwnd, IDC_KEYLIST_LISTBOX);
                /* The button's meaning = exactly what the label shows
                 * (keylist_button_target: focused row, else the first
                 * actionable selected key). */
                int target = keylist_button_target(hlist);
                if (target == KLBTN_NONE) {
                    MessageBeep(0);
                    return 0;
                }

                int nitems = ListView_GetItemCount(hlist);
                struct keyaction { strbuf *blob; char *path; };
                struct keyaction *jobs =
                    snewn(nitems ? nitems : 1, struct keyaction);
                int njobs = 0;
                for (int row = 0; row < nitems; row++) {
                    if (!(ListView_GetItemState(hlist, row, LVIS_SELECTED) &
                          LVIS_SELECTED))
                        continue;
                    struct keylist_display_data *disp =
                        keylist_row_data(hlist, row);
                    char *path = NULL;
                    if (!keylist_row_for_target(disp, target, &path))
                        continue;           /* already in the target state */
                    jobs[njobs].blob =
                        strbuf_dup(ptrlen_from_strbuf(disp->blob));
                    jobs[njobs].path = path;   /* owned; may be NULL */
                    njobs++;
                }
                for (int j = 0; j < njobs; j++) {
                    ptrlen blob = ptrlen_from_strbuf(jobs[j].blob);
                    if (target == KLBTN_DECRYPT) {
                        if (modal_passphrase_hwnd) {
                            MessageBeep(MB_ICONERROR);
                            SetForegroundWindow(modal_passphrase_hwnd);
                        } else if (jobs[j].path) {
                            Filename *fn = filename_from_str(jobs[j].path);
                            win_add_keyfile(fn, false);   /* decrypt now */
                            filename_free(fn);
                        }
                    } else {   /* KLBTN_REENCRYPT */
                        /* A plainly-loaded key has no encrypted fallback;
                         * re-add it encrypted from its file to attach one
                         * (silent - an encrypted add never prompts), then
                         * drop the cleartext. A key that already has a
                         * fallback has no path and skips straight to that. */
                        if (jobs[j].path) {
                            Filename *fn = filename_from_str(jobs[j].path);
                            win_add_keyfile(fn, true);
                            filename_free(fn);
                        }
                        pageant_reencrypt_ssh2_key_by_blob(blob);
                    }
                    strbuf_free(jobs[j].blob);
                    sfree(jobs[j].path);
                }
                sfree(jobs);
                pageant_forget_passphrases();
                keylist_update();
            }
            return 0;
          case IDC_KEYLIST_HELP:
            if (HIWORD(wParam) == BN_CLICKED ||
                HIWORD(wParam) == BN_DOUBLECLICKED) {
                launch_help(hwnd, WINHELP_CTX_pageant_general);
            }
            return 0;
          case IDC_KEYLIST_NEWKEY:
            /* KiTTY: make a new key - launch kittygen (it is beside us; the
             * button is greyed at init when it is not).
             *
             * SECURITY GATE: only launch a kittygen that is genuinely ours -
             * the same publisher signature the updater checks, and the exact
             * same version as this kageant (kitty_verify_sibling). Refuse a
             * planted or mismatched binary rather than run it. A dev/test
             * build of kageant is itself unsigned, so the signature leg is
             * skipped there and only the version must match - see
             * kitty_authenticode.c. */
            if (HIWORD(wParam) == BN_CLICKED ||
                HIWORD(wParam) == BN_DOUBLECLICKED) {
                char *g = find_kittygen();
                if (!g) {
                    MessageBox(hwnd,
                        "The KiTTY key generator (kittygen) was not found "
                        "next to kageant.", APPNAME,
                        MB_OK | MB_ICONINFORMATION);
                } else {
                    /* Verify, but let the user override a failure: they can
                     * run the generator by hand anyway, so a hard refusal
                     * buys little - a warning that they can act on is more
                     * use. Default No, so a careless Enter does not launch an
                     * unverified binary. */
                    int go = 1;
                    if (!kitty_verify_sibling(g)) {
                        int r = MessageBox(hwnd,
                            "The key generator next to kageant could not be "
                            "verified as a genuine, matching KiTTY build - its "
                            "signature or version did not check out.\n\n"
                            "It may simply be a different version, or it may "
                            "have been replaced with something else.\n\n"
                            "Start it anyway?",
                            "kageant - key generator not verified",
                            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
                        go = (r == IDYES);
                    }
                    if (go)
                        /* KiTTY: a restricted agent must launch a restricted
                         * key generator - the new private key would otherwise
                         * sit in an unrestricted process. */
                        ShellExecute(hwnd, NULL, g,
                                     restricted_acl() ? "-restrict-acl" : NULL,
                                     NULL, SW_SHOWNORMAL);
                    sfree(g);
                }
            }
            return 0;
          case IDC_KEYLIST_SETTINGS:
            /* KiTTY: the [Agent] settings dialog. */
            if (HIWORD(wParam) == BN_CLICKED ||
                HIWORD(wParam) == BN_DOUBLECLICKED) {
                DialogBox(hinst, MAKEINTRESOURCE(IDD_KEYSETTINGS), hwnd,
                          KeySettingsProc);
            }
            return 0;
          case IDC_KEYLIST_STOPAGENT:
            /* KiTTY: quit kageant from the window that is already open,
             * rather than hunting for the tray icon. Confirm first - it
             * unloads every key and cuts off anything using the agent. */
            if (HIWORD(wParam) == BN_CLICKED ||
                HIWORD(wParam) == BN_DOUBLECLICKED) {
                if (MessageBox(hwnd,
                        "Stop the kageant agent?\n\n"
                        "Every loaded key is unloaded, and any program using "
                        "the agent (PuTTY sessions, ssh, WinSCP...) loses "
                        "access until kageant is started again.",
                        APPNAME, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2)
                    == IDYES) {
                    /* Drive the same exit path as the tray menu's Exit. */
                    PostMessage(traywindow, WM_COMMAND, IDM_CLOSE, 0);
                }
            }
            return 0;
          case IDC_KEYLIST_ABOUT:
            /* KiTTY: About was tray-only; drive the same handler. */
            if (HIWORD(wParam) == BN_CLICKED ||
                HIWORD(wParam) == BN_DOUBLECLICKED)
                PostMessage(traywindow, WM_COMMAND, IDM_ABOUT, 0);
            return 0;
          case IDC_KEYLIST_RESUMECONFIRM:
            /* KiTTY: lift the confirm-suppress latch and hide the button. */
            if (HIWORD(wParam) == BN_CLICKED ||
                HIWORD(wParam) == BN_DOUBLECLICKED) {
                kageant_confirm_resume();
                EnableWindow(GetDlgItem(hwnd, IDC_KEYLIST_RESUMECONFIRM),
                             FALSE);
            }
            return 0;
          case IDC_KEYLIST_RETRY:
            /*
             * KiTTY: try the keys whose file was not there. Otherwise they
             * wait for a device event, and a file that came back over the
             * network, or a stick that was already in when kageant started,
             * never produces one.
             *
             * No guard on "is anything pending" here: the button is disabled
             * when nothing is, but a WM_COMMAND can arrive with it disabled
             * (that is how the test harness drives this), and the retry
             * answers that case itself rather than doing nothing silently.
             */
            if (HIWORD(wParam) == BN_CLICKED ||
                HIWORD(wParam) == BN_DOUBLECLICKED) {
                kageant_retry_pending_keys_now();
                keylist_update();     /* states and the button both changed */
            }
            return 0;
          case IDC_KEYLIST_FPTYPE:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                int selection = SendDlgItemMessage(
                    hwnd, IDC_KEYLIST_FPTYPE, CB_GETCURSEL, 0, 0);
                if (selection >= 0 && (size_t)selection < lenof(fptypes)) {
                    fptype = fptypes[selection].value;
                    keylist_update();
                }
            }
            return 0;
        }
        return 0;
      case WM_HELP: {
        int id = ((LPHELPINFO)lParam)->iCtrlId;
        const char *topic = NULL;
        switch (id) {
          case IDC_KEYLIST_LISTBOX:
          case IDC_KEYLIST_FPTYPE:
          case IDC_KEYLIST_FPTYPE_STATIC:
            topic = WINHELP_CTX_pageant_keylist; break;
          case IDC_KEYLIST_ADDKEY: topic = WINHELP_CTX_pageant_addkey; break;
          case IDC_KEYLIST_REMOVE: topic = WINHELP_CTX_pageant_remkey; break;
          case IDC_KEYLIST_ADDKEY_ENC:
          case IDC_KEYLIST_REENCRYPT:
            topic = WINHELP_CTX_pageant_deferred; break;
        }
        if (topic) {
            launch_help(hwnd, topic);
        } else {
            MessageBeep(0);
        }
        break;
      }
      case WM_TIMER:
        /* KiTTY: expire the key-use tints; stop asking once none are live. */
        if (wParam == KEYLIST_FLASH_TIMER) {
            InvalidateRect(GetDlgItem(hwnd, IDC_KEYLIST_LISTBOX), NULL, FALSE);
            if (!kageant_flash_any())
                KillTimer(hwnd, KEYLIST_FLASH_TIMER);
            return 0;
        }
        if (wParam == TID_KL_RESUME)
            EnableWindow(GetDlgItem(hwnd, IDC_KEYLIST_RESUMECONFIRM),
                         kageant_confirm_suppressed());
        return 0;
      case WM_CLOSE:
        keylist = NULL;
        DestroyWindow(hwnd);
        return 0;
      case WM_DESTROY:
        KillTimer(hwnd, TID_KL_RESUME);   /* KiTTY: Resume-button sync timer */
        /* KiTTY: every close path funnels through here - the Close button,
         * Escape, and the window menu. Save the geometry and free the
         * display structs the rows still point at (the ListView children
         * are destroyed after their parent gets WM_DESTROY, so the rows
         * are still readable). */
        keylist_save_geometry(hwnd);
        keylist_free_display_data(GetDlgItem(hwnd, IDC_KEYLIST_LISTBOX));
        keylist_layout_ready = false;
        return 0;
    }
    return 0;
}

/* Set up a system tray icon */
static BOOL AddTrayIcon(HWND hwnd)
{
    BOOL res;
    NOTIFYICONDATA tnid;
    HICON hicon;

#ifdef NIM_SETVERSION
    tnid.uVersion = 0;
    res = Shell_NotifyIcon(NIM_SETVERSION, &tnid);
#endif

    tnid.cbSize = sizeof(NOTIFYICONDATA);
    tnid.hWnd = hwnd;
    tnid.uID = 1;              /* unique within this systray use */
    tnid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    tnid.uCallbackMessage = WM_SYSTRAY;
    tnid.hIcon = hicon = LoadIcon(hinst, MAKEINTRESOURCE(201));
    /* KiTTY: extra tooltip lines for the agent's state - ini store,
     * portable layout, restricted ACL. The agent holds the keys, so "is the
     * lockdown actually on?" is worth answering without opening anything.
     * Composed in kitty/kitty_title.c; szTip is 128 chars, all lines fit. */
    {
        char *tip = kitty_title_compose_sep(
            kageant_ini_status()
                ? "kageant (KiTTY authentication agent)\r\n(kitty.ini mode)"
                : "kageant (KiTTY authentication agent)",
            "\r\n", kitty_inilight_portable(), restricted_acl(), false);
        strncpy(tnid.szTip, tip, sizeof(tnid.szTip) - 1);
        tnid.szTip[sizeof(tnid.szTip) - 1] = '\0';
        sfree(tip);
    }

    res = Shell_NotifyIcon(NIM_ADD, &tnid);

    if (hicon) DestroyIcon(hicon);

    return res;
}

/* Update the saved-sessions menu. */
static void update_sessions(void)
{
    int num_entries;
    HKEY hkey;
    TCHAR buf[MAX_PATH + 1];
    MENUITEMINFO mii;
    strbuf *sb;

    int index_key, index_menu;

    if (!putty_path)
        return;

    if (ERROR_SUCCESS != RegOpenKey(HKEY_CURRENT_USER, kageant_sessions_key(), &hkey))
        return;

    for (num_entries = GetMenuItemCount(session_menu);
        num_entries > initial_menuitems_count;
        num_entries--)
        RemoveMenu(session_menu, 0, MF_BYPOSITION);

    index_key = 0;
    index_menu = 0;

    sb = strbuf_new();
    while (ERROR_SUCCESS == RegEnumKey(hkey, index_key, buf, MAX_PATH)) {
        if (strcmp(buf, PUTTY_DEFAULT) != 0) {
            strbuf_clear(sb);
            unescape_registry_key(buf, sb);

            memset(&mii, 0, sizeof(mii));
            mii.cbSize = sizeof(mii);
            mii.fMask = MIIM_TYPE | MIIM_STATE | MIIM_ID;
            mii.fType = MFT_STRING;
            mii.fState = MFS_ENABLED;
            mii.wID = (index_menu * 16) + IDM_SESSIONS_BASE;
            mii.dwTypeData = sb->s;
            InsertMenuItem(session_menu, index_menu, true, &mii);
            index_menu++;
        }
        index_key++;
    }
    strbuf_free(sb);

    RegCloseKey(hkey);

    if (index_menu == 0) {
        mii.cbSize = sizeof(mii);
        mii.fMask = MIIM_TYPE | MIIM_STATE;
        mii.fType = MFT_STRING;
        mii.fState = MFS_GRAYED;
        mii.dwTypeData = _T("(No sessions)");
        InsertMenuItem(session_menu, index_menu, true, &mii);
    }
}

/*
 * Versions of Pageant prior to 0.61 expected this SID on incoming
 * communications. For backwards compatibility, and more particularly
 * for compatibility with derived works of PuTTY still using the old
 * Pageant client code, we accept it as an alternative to the one
 * returned from get_user_sid().
 */
PSID get_default_sid(void)
{
    HANDLE proc = NULL;
    DWORD sidlen;
    PSECURITY_DESCRIPTOR psd = NULL;
    PSID sid = NULL, copy = NULL, ret = NULL;

    if ((proc = OpenProcess(MAXIMUM_ALLOWED, false,
                            GetCurrentProcessId())) == NULL)
        goto cleanup;

    if (p_GetSecurityInfo(proc, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION,
                          &sid, NULL, NULL, NULL, &psd) != ERROR_SUCCESS)
        goto cleanup;

    sidlen = GetLengthSid(sid);

    copy = (PSID)smalloc(sidlen);

    if (!CopySid(sidlen, copy, sid))
        goto cleanup;

    /* Success. Move sid into the return value slot, and null it out
     * to stop the cleanup code freeing it. */
    ret = copy;
    copy = NULL;

  cleanup:
    if (proc != NULL)
        CloseHandle(proc);
    if (psd != NULL)
        LocalFree(psd);
    if (copy != NULL)
        sfree(copy);

    return ret;
}

struct WmCopydataTransaction {
    char *length, *body;
    size_t bodysize, bodylen;
    HANDLE ev_msg_ready, ev_reply_ready;
    DWORD sender_pid;    /* KiTTY: WM_COPYDATA sender, for mutation notices */
} wmct;

static struct PageantClient wmcpc;

static void wm_copydata_got_msg(void *vctx)
{
    /* KiTTY: WM_COPYDATA is an external transport - see the pipe path. */
    pageant_external_request = true;
    pageant_external_pid = wmct.sender_pid;
    pageant_handle_msg(&wmcpc, NULL, make_ptrlen(wmct.body, wmct.bodylen));
    pageant_external_request = false;
    pageant_external_pid = 0;
}

static void wm_copydata_got_response(
    PageantClient *pc, PageantClientRequestId *reqid, ptrlen response)
{
    if (response.len > wmct.bodysize) {
        /* Output would overflow message buffer. Replace with a
         * failure message. */
        static const unsigned char failure[] = { SSH_AGENT_FAILURE };
        response = make_ptrlen(failure, lenof(failure));
        assert(response.len <= wmct.bodysize);
    }

    PUT_32BIT_MSB_FIRST(wmct.length, response.len);
    memcpy(wmct.body, response.ptr, response.len);

    SetEvent(wmct.ev_reply_ready);
}

static bool ask_passphrase_common(PageantClientDialogId *dlgid,
                                  const char *comment)
{
    /* Pageant core should be serialising requests, so we never expect
     * a passphrase prompt to exist already at this point */
    assert(!nonmodal_passphrase_hwnd);

    struct PassphraseProcStruct *pps = snew(struct PassphraseProcStruct);
    pps->modal = false;
    pps->help_topic = WINHELP_CTX_pageant_deferred;
    pps->dlgid = dlgid;
    pps->passphrase = NULL;
    pps->comment = comment;
    /* KiTTY: capture the foreground window NOW (the terminal that just requested
     * the key) so the on-demand passphrase prompt opens centred over it. */
    pps->over = GetForegroundWindow();

    nonmodal_passphrase_hwnd = CreateDialogParam(
        hinst, MAKEINTRESOURCE(IDD_ONDEMAND_PASSPHRASE),
        NULL, PassphraseProc, (LPARAM)pps);

    /*
     * Try to put this passphrase prompt into the foreground.
     *
     * This will probably not succeed in giving it the actual keyboard
     * focus, because Windows is quite opposed to applications being
     * able to suddenly steal the focus on their own initiative.
     *
     * That makes sense in a lot of situations, as a defensive
     * measure. If you were about to type a password or other secret
     * data into the window you already had focused, and some
     * malicious app stole the focus, it might manage to trick you
     * into typing your secrets into _it_ instead.
     *
     * In this case it's possible to regard the same defensive measure
     * as counterproductive, because the effect if we _do_ steal focus
     * is that you type something into our passphrase prompt that
     * isn't the passphrase, and we fail to decrypt the key, and no
     * harm is done. Whereas the effect of the user wrongly _assuming_
     * the new passphrase prompt has the focus is much worse: now you
     * type your highly secret passphrase into some other window you
     * didn't mean to trust with that information - such as the
     * agent-forwarded PuTTY in which you just ran an ssh command,
     * which the _whole point_ was to avoid telling your passphrase to!
     *
     * On the other hand, I'm sure _every_ application author can come
     * up with an argument for why they think _they_ should be allowed
     * to steal the focus. Probably most of them include the claim
     * that no harm is done if their application receives data
     * intended for something else, and of course that's not always
     * true!
     *
     * In any case, I don't know of anything I can do about it, or
     * anything I _should_ do about it if I could. If anyone thinks
     * they can improve on all this, patches are welcome.
     */
    pageant_force_foreground(nonmodal_passphrase_hwnd);  /* KiTTY: beat fg lock */

    return true;
}

static bool wm_copydata_ask_passphrase(
    PageantClient *pc, PageantClientDialogId *dlgid, const char *comment)
{
    return ask_passphrase_common(dlgid, comment);
}

static const PageantClientVtable wmcpc_vtable = {
    .log = NULL, /* no logging in this client */
    .got_response = wm_copydata_got_response,
    .ask_passphrase = wm_copydata_ask_passphrase,
};

static char *answer_filemapping_message(const char *mapname)
{
    HANDLE maphandle = INVALID_HANDLE_VALUE;
    void *mapaddr = NULL;
    char *err = NULL;
    size_t mapsize;
    unsigned msglen;

    PSID mapsid = NULL;
    PSID expectedsid = NULL;
    PSID expectedsid_bc = NULL;
    PSECURITY_DESCRIPTOR psd = NULL;

    wmct.length = wmct.body = NULL;

#ifdef DEBUG_IPC
    debug("mapname = \"%s\"\n", mapname);
#endif

    maphandle = OpenFileMapping(FILE_MAP_ALL_ACCESS, false, mapname);
    if (maphandle == NULL || maphandle == INVALID_HANDLE_VALUE) {
        err = dupprintf("OpenFileMapping(\"%s\"): %s",
                        mapname, win_strerror(GetLastError()));
        goto cleanup;
    }

#ifdef DEBUG_IPC
    debug("maphandle = %p\n", maphandle);
#endif

    if (should_have_security()) {
        DWORD retd;

        if ((expectedsid = get_user_sid()) == NULL) {
            err = dupstr("unable to get user SID");
            goto cleanup;
        }

        if ((expectedsid_bc = get_default_sid()) == NULL) {
            err = dupstr("unable to get default SID");
            goto cleanup;
        }

        if ((retd = p_GetSecurityInfo(
                 maphandle, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION,
                 &mapsid, NULL, NULL, NULL, &psd) != ERROR_SUCCESS)) {
            err = dupprintf("unable to get owner of file mapping: "
                            "GetSecurityInfo returned: %s",
                            win_strerror(retd));
            goto cleanup;
        }

#ifdef DEBUG_IPC
        {
            LPTSTR ours, ours2, theirs;
            ConvertSidToStringSid(mapsid, &theirs);
            ConvertSidToStringSid(expectedsid, &ours);
            ConvertSidToStringSid(expectedsid_bc, &ours2);
            debug("got sids:\n  oursnew=%s\n  oursold=%s\n"
                  "  theirs=%s\n", ours, ours2, theirs);
            LocalFree(ours);
            LocalFree(ours2);
            LocalFree(theirs);
        }
#endif

        if (!EqualSid(mapsid, expectedsid) &&
            !EqualSid(mapsid, expectedsid_bc)) {
            err = dupstr("wrong owning SID of file mapping");
            goto cleanup;
        }
    } else {
#ifdef DEBUG_IPC
        debug("security APIs not present\n");
#endif
    }

    mapaddr = MapViewOfFile(maphandle, FILE_MAP_WRITE, 0, 0, 0);
    if (!mapaddr) {
        err = dupprintf("unable to obtain view of file mapping: %s",
                        win_strerror(GetLastError()));
        goto cleanup;
    }

#ifdef DEBUG_IPC
    debug("mapped address = %p\n", mapaddr);
#endif

    {
        MEMORY_BASIC_INFORMATION mbi;
        size_t mbiSize = VirtualQuery(mapaddr, &mbi, sizeof(mbi));
        if (mbiSize == 0) {
            err = dupprintf("unable to query view of file mapping: %s",
                            win_strerror(GetLastError()));
            goto cleanup;
        }
        if (mbiSize < (offsetof(MEMORY_BASIC_INFORMATION, RegionSize) +
                       sizeof(mbi.RegionSize))) {
            err = dupstr("VirtualQuery returned too little data to get "
                         "region size");
            goto cleanup;
        }

        mapsize = mbi.RegionSize;
    }
#ifdef DEBUG_IPC
    debug("region size = %"SIZEu"\n", mapsize);
#endif
    if (mapsize < 5) {
        err = dupstr("mapping smaller than smallest possible request");
        goto cleanup;
    }

    wmct.length = (char *)mapaddr;
    msglen = GET_32BIT_MSB_FIRST(wmct.length);

#ifdef DEBUG_IPC
    debug("msg length=%08x, msg type=%02x\n",
          msglen, (unsigned)((unsigned char *) mapaddr)[4]);
#endif

    wmct.body = wmct.length + 4;
    wmct.bodysize = mapsize - 4;

    if (msglen > wmct.bodysize) {
        /* Incoming length field is too large. Emit a failure response
         * without even trying to handle the request.
         *
         * (We know this must fit, because we checked mapsize >= 5
         * above.) */
        PUT_32BIT_MSB_FIRST(wmct.length, 1);
        *wmct.body = SSH_AGENT_FAILURE;
    } else {
        wmct.bodylen = msglen;
        SetEvent(wmct.ev_msg_ready);
        WaitForSingleObject(wmct.ev_reply_ready, INFINITE);
    }

  cleanup:
    /* expectedsid has the lifetime of the program, so we don't free it */
    sfree(expectedsid_bc);
    if (psd)
        LocalFree(psd);
    if (mapaddr)
        UnmapViewOfFile(mapaddr);
    if (maphandle != NULL && maphandle != INVALID_HANDLE_VALUE)
        CloseHandle(maphandle);
    return err;
}

static void create_keylist_window(void)
{
    if (keylist)
        return;

    keylist = CreateDialog(hinst, MAKEINTRESOURCE(IDD_KEYLIST),
                           NULL, KeyListProc);
    ShowWindow(keylist, SW_SHOWNORMAL);
}

/* ------------------------------------------------------------------ *
 * KiTTY: the kageant additions (Windows OpenSSH client integration,   *
 * load-keys-on-startup, key-use notify/confirm, persistent key offer  *
 * order) live in kitty/kitty_pageant.c. This accessor gives that file *
 * access to the tray window without exporting the static.             *
 * ------------------------------------------------------------------ */
HWND kageant_traywindow(void)
{
    return traywindow;
}

/* KiTTY: is the key list on screen? The held-back nudge stays quiet while it
 * is - the user is already looking at the thing it would point them to. */
int kageant_keylist_open(void)
{
    return keylist != NULL;
}

/*
 * KiTTY: re-compose the tray tooltip.
 *
 * The tooltip carries the state that is ALWAYS true, which is what answers
 * "why did my login stop working?" hours after a balloon came and went. So a
 * key being held back has to appear here, and has to disappear again when it
 * is resolved - hence NIM_MODIFY rather than composing it once at startup.
 */
void kageant_refresh_tray_tip(void)
{
    NOTIFYICONDATA tnid;
    char *tip;
    int held;

    if (!traywindow)
        return;

    memset(&tnid, 0, sizeof(tnid));
    tnid.cbSize = sizeof(tnid);
    tnid.hWnd = traywindow;
    tnid.uID = 1;
    tnid.uFlags = NIF_TIP;

    tip = kitty_title_compose_sep(
        kageant_ini_status()
            ? "kageant (KiTTY authentication agent)\r\n(kitty.ini mode)"
            : "kageant (KiTTY authentication agent)",
        "\r\n", kitty_inilight_portable(), restricted_acl(), false);

    /*
     * The existing lines - kitty.ini mode, portable, restricted ACL - all stay:
     * this ADDS a line, it does not replace the state that was already there.
     *
     * It goes SECOND, straight after the name, rather than at the end. szTip is
     * 128 characters, and a portable + restricted + kitty.ini install already
     * spends most of them, so a line appended last is the one Windows cuts off.
     * Of everything in here, "a key is not loaded" is the line someone is
     * actually looking for.
     */
    /* KiTTY: when the in-memory protection is not working, the tip carries
     * that permanently - the startup notice comes and goes, and this is the
     * condition a user should be able to re-check any time. Composed into
     * the SAME second-line slot as the mismatch line (both can be present;
     * the mismatch stays first, it is the actionable one). */
    if (!kitty_protkey_available()) {
        const char *rest = strstr(tip, "\r\n");
        char merged[256];
        snprintf(merged, sizeof(merged), "%.*s\r\nkeys UNPROTECTED in memory%s",
                 rest ? (int)(rest - tip) : (int)strlen(tip), tip,
                 rest ? rest : "");
        sfree(tip);
        tip = dupstr(merged);
    }
    held = kageant_mismatch_count();
    if (held > 0) {
        char line[64];
        const char *rest = strstr(tip, "\r\n");
        snprintf(line, sizeof(line),
                 held == 1 ? "1 key NOT loaded - fingerprint mismatch"
                           : "%d keys NOT loaded - fingerprint mismatch",
                 held);
        snprintf(tnid.szTip, sizeof(tnid.szTip), "%.*s\r\n%s%s",
                 rest ? (int)(rest - tip) : (int)strlen(tip), tip,
                 line, rest ? rest : "");
    } else {
        strncpy(tnid.szTip, tip, sizeof(tnid.szTip) - 1);
        tnid.szTip[sizeof(tnid.szTip) - 1] = '\0';
    }
    sfree(tip);

    Shell_NotifyIcon(NIM_MODIFY, &tnid);
}

static LRESULT CALLBACK TrayWndProc(HWND hwnd, UINT message,
                                    WPARAM wParam, LPARAM lParam)
{
    static bool menuinprogress;
    static UINT msgTaskbarCreated = 0;
    /* KiTTY: cursor position captured at the single left click, used
     * when the delayed menu timer fires */
    static POINT trayclickpos;
    /* KiTTY: a double click arrives as down/up/dblclk/up - swallow the
     * trailing button-up so it doesn't re-arm the single-click timer */
    static bool trayignoreup;

    switch (message) {
      case WM_CREATE:
        msgTaskbarCreated = RegisterWindowMessage(_T("TaskbarCreated"));
        break;
      default:
        if (message==msgTaskbarCreated) {
            /*
             * Explorer has been restarted, so the tray icon will
             * have been lost.
             */
            AddTrayIcon(hwnd);
            kageant_refresh_tray_tip();   /* explorer restarted: state too */
        }
        break;

      case WM_SYSTRAY:
        if (lParam == WM_RBUTTONUP) {
            POINT cursorpos;
            GetCursorPos(&cursorpos);
            PostMessage(hwnd, WM_SYSTRAY2, cursorpos.x, cursorpos.y);
        } else if (lParam == WM_LBUTTONUP) {
            /* KiTTY: a plain left click opens the tray menu too, but
             * delayed by the double-click time: the menu pops up
             * bottom-right-aligned at the cursor, so opening it on the
             * first click of a double click would put the second click
             * on the bottom menu item. */
            if (trayignoreup) {
                trayignoreup = false;
            } else {
                GetCursorPos(&trayclickpos);
                SetTimer(hwnd, TID_TRAYCLICK, GetDoubleClickTime(), NULL);
            }
        } else if (lParam == WM_LBUTTONDBLCLK) {
            /* Run the default menu item. */
            UINT menuitem = GetMenuDefaultItem(systray_menu, false, 0);
            /* KiTTY: cancel the pending single-click menu and swallow
             * the button-up that follows the double click */
            KillTimer(hwnd, TID_TRAYCLICK);
            trayignoreup = true;
            if (menuitem != -1)
                PostMessage(hwnd, WM_COMMAND, menuitem, 0);
        } else if (lParam == NIN_BALLOONUSERCLICK) {
            /* KiTTY: clicking a balloon - "N keys not loaded", "key used" -
             * opens the window that answers it, instead of doing nothing. */
            PostMessage(hwnd, WM_COMMAND, IDM_VIEWKEYS, 0);
        }
        break;
      case KAGEANT_WM_NOTICE_CLICK:
        /* KiTTY: a kageant notice window was clicked - open View Keys, the
         * window that answers "which keys?" for both the key-used and the
         * startup-keys-missing notices. Also lift any confirm-suppress latch
         * (a no-op when not suppressed), so clicking the "confirmations
         * blocked" notice resumes even when the key list is already open. */
        kageant_confirm_resume();
        PostMessage(hwnd, WM_COMMAND, IDM_VIEWKEYS, 0);
        break;
      case WM_DEVICECHANGE:
        /*
         * KiTTY: keys on removable media.
         *
         * A key on a USB stick or a network share is absent at login and
         * present later. Windows tells us when that changes, so there is no
         * polling and no "retry on every agent request": DBT_DEVICEARRIVAL is
         * the moment to try the startup keys that were not there, and
         * DBT_DEVICEREMOVECOMPLETE the moment their media has gone.
         *
         * Both halves are opt-out/opt-in through [Agent] retrykeys and
         * unloadonremove, and both are no-ops unless a startup key is actually
         * on media that comes and goes.
         */
        if (wParam == DBT_DEVICEARRIVAL) {
            /*
             * Which letter(s) arrived, when the broadcast says. A volume
             * arrival carries a DEV_BROADCAST_VOLUME whose dbcv_unitmask
             * names the letters (bit 0 = A:) - `subst` sends one too, with
             * DBTF_NET set. Retry mode (c) tries the pending keys' stored
             * paths on exactly those letters; an arrival with no volume
             * mask (mask 0) probes nothing beyond the stored paths.
             */
            unsigned long mask = 0;
            DEV_BROADCAST_HDR *hdr = (DEV_BROADCAST_HDR *)lParam;
            if (hdr && hdr->dbch_devicetype == DBT_DEVTYP_VOLUME)
                mask = ((DEV_BROADCAST_VOLUME *)hdr)->dbcv_unitmask;
            kageant_retry_pending_keys(mask);
        } else if (wParam == DBT_DEVICEREMOVECOMPLETE)
            kageant_media_gone();
        if (wParam == DBT_DEVICEARRIVAL || wParam == DBT_DEVICEREMOVECOMPLETE)
            keylist_update();          /* the View Keys window, if it is open */
        break;
      case WM_TIMER:
        /* KiTTY: no double click arrived - deliver the left-click menu */
        if (wParam == TID_TRAYCLICK) {
            KillTimer(hwnd, TID_TRAYCLICK);
            PostMessage(hwnd, WM_SYSTRAY2, trayclickpos.x, trayclickpos.y);
        }
        /*
         * KiTTY: backstop for the passphrase cache.
         *
         * Passphrases typed while adding keys are kept in plain memory so that
         * adding several keys at once only asks once, and are scrubbed when the
         * add finishes (see the pageant_forget_passphrases() calls). "When the
         * add finishes" is doing a lot of work there: an add left half-done -
         * the file dialog still open, a passphrase prompt abandoned - never
         * finishes, and the typed passphrases stay until kageant exits.
         *
         * So the add also arms this, and it scrubs them regardless. The default
         * is a minute, which is far longer than any add that is actually being
         * attended to.
         */
        if (wParam == TID_PASSPHRASE_CACHE) {
            KillTimer(hwnd, TID_PASSPHRASE_CACHE);
            pageant_forget_passphrases();
        }
        /* KiTTY: remove any ssh-add -t keys whose lifetime has run out. */
        if (wParam == TID_KEY_LIFETIME) {
            if (kageant_expire_due_keys())
                keylist_update();          /* refresh the window if it is open */
            else
                keylist_tick_lifetimes();  /* live countdown column */
        }
        break;
      case WM_SYSTRAY2:
        if (!menuinprogress) {
            menuinprogress = true;
            update_sessions();
            /* KiTTY: the menu is built once at startup, but the ini-backed
             * settings can change behind our back (kitty.ini edits, store
             * switches between registry and ini mode) - re-sync the
             * checkmarks with the live store every time the menu opens. */
            CheckMenuItem(systray_menu, IDM_NOTIFY_KEYUSE, MF_BYCOMMAND |
                          (kageant_notify_get() ? MF_CHECKED : MF_UNCHECKED));
            CheckMenuItem(systray_menu, IDM_CONFIRM_KEYUSE, MF_BYCOMMAND |
                          (kageant_confirm_get() ? MF_CHECKED : MF_UNCHECKED));
            CheckMenuItem(systray_menu, IDM_LOAD_ON_STARTUP, MF_BYCOMMAND |
                          (kageant_autostart_active() ? MF_CHECKED : MF_UNCHECKED));
            CheckMenuItem(systray_menu, IDM_OPENSSH_INTEGRATION, MF_BYCOMMAND |
                          (kageant_openssh_get() ? MF_CHECKED : MF_UNCHECKED));
            EnableMenuItem(systray_menu, IDM_RESUME_CONFIRM, MF_BYCOMMAND |
                           (kageant_confirm_suppressed() ? MF_ENABLED
                                                         : MF_GRAYED));
            SetForegroundWindow(hwnd);
            TrackPopupMenu(systray_menu,
                           TPM_RIGHTALIGN | TPM_BOTTOMALIGN |
                           TPM_RIGHTBUTTON,
                           wParam, lParam, 0, hwnd, NULL);
            menuinprogress = false;
        }
        break;
      case WM_COMMAND:
      case WM_SYSCOMMAND: {
        unsigned command = wParam & ~0xF; /* low 4 bits reserved to Windows */
        switch (command) {
          case IDM_PUTTY: {
            TCHAR cmdline[10];
            if (!kageant_kitty_launch_allowed(hwnd))
                break;
            cmdline[0] = '\0';
            if (restrict_putty_acl)
                strcat(cmdline, "&R");

            if ((INT_PTR)ShellExecute(hwnd, NULL, putty_path, cmdline,
                                      _T(""), SW_SHOW) <= 32) {
                MessageBox(NULL, "Unable to execute KiTTY!",
                           "Error", MB_OK | MB_ICONERROR);
            }
            break;
          }
          case IDM_CLOSE:
            if (modal_passphrase_hwnd)
                SendMessage(modal_passphrase_hwnd, WM_CLOSE, 0, 0);
            SendMessage(hwnd, WM_CLOSE, 0, 0);
            break;
          case IDM_VIEWKEYS:
            create_keylist_window();
            /*
             * Sometimes the window comes up minimised / hidden for
             * no obvious reason. Prevent this. This also brings it
             * to the front if it's already present (the user
             * selected View Keys because they wanted to _see_ the
             * thing).
             */
            SetForegroundWindow(keylist);
            SetWindowPos(keylist, HWND_TOP, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
            break;
          case IDM_ADDKEY:
          case IDM_ADDKEY_ENCRYPTED:
            if (modal_passphrase_hwnd) {
                MessageBeep(MB_ICONERROR);
                SetForegroundWindow(modal_passphrase_hwnd);
                break;
            }
            prompt_add_keyfile(command == IDM_ADDKEY_ENCRYPTED);
            break;
          case IDM_REMOVE_ALL:
            pageant_delete_all();
            keylist_update();
            break;
          case IDM_REENCRYPT_ALL:
            pageant_reencrypt_all();
            keylist_update();
            break;
          case IDM_ABOUT:
            if (!aboutbox) {
                aboutbox = CreateDialog(hinst, MAKEINTRESOURCE(IDD_ABOUT),
                                        NULL, AboutProc);
                ShowWindow(aboutbox, SW_SHOWNORMAL);
                /*
                 * Sometimes the window comes up minimised / hidden
                 * for no obvious reason. Prevent this.
                 */
                SetForegroundWindow(aboutbox);
                SetWindowPos(aboutbox, HWND_TOP, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
            }
            break;
          case IDM_SETTINGS:
            /* KiTTY: the [Agent] settings dialog, straight from the tray.
             * Modal, no owner window here - auxpos anchors to the foreground
             * window / notification area. */
            DialogBox(hinst, MAKEINTRESOURCE(IDD_KEYSETTINGS), NULL,
                      KeySettingsProc);
            break;
          case IDM_HELP:
            launch_help(hwnd, WINHELP_CTX_pageant_general);
            break;
          case IDM_OPENSSH_INTEGRATION: {
            /* KiTTY: toggle Windows OpenSSH integration. The click is the
             * consent; we only ever touch our own managed block. */
            int on = !kageant_openssh_get();
            if (on && !should_have_security()) {
                MessageBox(NULL, "Cannot register as the Windows OpenSSH agent: "
                           "this kageant has no named-pipe listener.",
                           "kageant", MB_ICONERROR | MB_OK);
                break;
            }
            kageant_openssh_set(on);
            kageant_openssh_apply(on);
            CheckMenuItem(systray_menu, IDM_OPENSSH_INTEGRATION,
                          MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
            if (on) {
                char *cfg = kageant_ssh_path("config");
                char *msg = dupprintf(
                    "kageant is now the Windows OpenSSH agent.\n\n"
                    "Added an \"Include kageant.conf\" block to:\n%s\n\n"
                    "(A one-time .kageant.bak backup was saved. Untick this "
                    "item to remove the block again.)", cfg ? cfg : "~/.ssh/config");
                MessageBox(NULL, msg, "kageant", MB_ICONINFORMATION | MB_OK);
                sfree(msg); sfree(cfg);
            }
            break;
          }
          case IDM_LOAD_ON_STARTUP: {
            /* KiTTY: toggle load-keys-on-startup. Enabling snapshots the
             * currently-loaded keys and installs an autostart Run entry. */
            /* Base the toggle on the real autostart artifact, not just the
             * saved flag, so a hand-deleted shortcut re-syncs correctly. */
            int on = !kageant_autostart_active();
            int portable = !kitty_inilight_registry_authoritative();

            /* Pre-flight: when enabling, if another agent is already set to
             * start at login, ASK before adding ours - do not silently create
             * a second autostart and the race that comes with it. Default No. */
            if (on) {
                char cdesc[512];
                if (kageant_autostart_conflict(cdesc, sizeof(cdesc))) {
                    char *w = dupprintf(
                        "Another SSH agent is already set to start at login:\n\n"
                        "    %s\n\n"
                        "If you add this kageant too, BOTH start at login and "
                        "only one wins (single-instance) - the other exits "
                        "without loading its keys, and which wins is a race.\n\n"
                        "Add this kageant to autostart anyway?\n\n"
                        "Choose No to leave autostart unchanged. To make THIS "
                        "kageant your login agent, disable the other one in "
                        "Settings > Apps > Startup (that stops it starting, for "
                        "both Run entries and Startup shortcuts) or delete its "
                        "Run-registry value / Startup shortcut.", cdesc);
                    int r = MessageBox(NULL, w, "kageant - autostart conflict",
                                       MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2);
                    sfree(w);
                    if (r != IDYES)
                        break;              /* aborted: nothing changed */
                }
            }

            /* Autostart at login: a registry-free Startup-folder shortcut in
             * portable mode, the HKCU ...\Run entry otherwise. Either way it is
             * machine-local (an absolute path) and does not travel, but that is
             * what unattended/fixed installs want; disabling removes it. */
            kageant_set_autostart(on);
            CheckMenuItem(systray_menu, IDM_LOAD_ON_STARTUP,
                          MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
            if (on) {
                char *msg = portable ? dupprintf(
                    "kageant will load your current %d key(s) at startup, added "
                    "encrypted (passphrase asked on first use).\n\n"
                    "The key list is saved to kitty.ini so it travels with this "
                    "portable install (keys inside the install folder are stored "
                    "relative to it; a key added from elsewhere prompts to be "
                    "copied in or referenced).\n\n"
                    "A login shortcut to this kageant was placed in your Startup "
                    "folder (no registry entry). It is machine-local and pinned "
                    "to the current path, so it does not follow the stick to "
                    "another machine or drive letter - disable this here to "
                    "remove it.",
                    kageant_nloaded())
                  : dupprintf(
                    "kageant will load your current %d key(s) at login, added "
                    "encrypted (passphrase asked on first use).\n\n"
                    "An autostart entry was added (HKCU ...\\Run\\%s), so you can "
                    "remove any manual kageant Startup shortcut. Newly added keys "
                    "are remembered automatically while this stays enabled.",
                    kageant_nloaded(), KAGEANT_RUN_NAME);
                MessageBox(NULL, msg, "kageant", MB_ICONINFORMATION | MB_OK);
                sfree(msg);
            }
            break;
          }
          case IDM_LOAD_KEYS: {
            /* KiTTY: remember the loaded keys and re-add them next start. Only
             * the flag and the snapshot - starting kageant itself is the other
             * item. Key FILE PATHS are stored, never passphrases or key
             * material, and they come back encrypted (deferred). */
            int on = !kageant_startup_get();
            kageant_startup_set(on);
            /* Snapshot the current key set - but ONLY if there is one. Turning
             * this on means "remember what I have loaded"; with nothing loaded
             * it must not mean "forget what you remembered". Disable, restart
             * (so nothing is re-added), re-enable, and an empty snapshot used
             * to overwrite the stored list. Removing keys still empties it -
             * that path snapshots after an explicit removal. */
            if (on && kageant_nloaded() > 0)
                kageant_save_startup_keys();
            CheckMenuItem(systray_menu, IDM_LOAD_KEYS,
                          MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
            break;
          }
          case IDM_NOTIFY_KEYUSE: {
            /* KiTTY: toggle the "a key was used to authenticate" tray balloon. */
            int on = !kageant_notify_get();
            kageant_notify_set(on);
            CheckMenuItem(systray_menu, IDM_NOTIFY_KEYUSE,
                          MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
            break;
          }
          case IDM_CONFIRM_KEYUSE: {
            /* KiTTY: toggle the confirm-before-every-key-use prompt. */
            int on = !kageant_confirm_get();
            kageant_confirm_set(on);
            CheckMenuItem(systray_menu, IDM_CONFIRM_KEYUSE,
                          MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
            keylist_sync_confirm_radios();   /* an open key list, too */
            break;
          }
          case IDM_RESUME_CONFIRM:
            /* KiTTY: lift the confirm-suppress latch from the tray, reachable
             * whether or not the key list is open. */
            kageant_confirm_resume();
            break;
          default: {
            if (wParam >= IDM_SESSIONS_BASE && wParam <= IDM_SESSIONS_MAX) {
                MENUITEMINFO mii;
                TCHAR buf[MAX_PATH + 1];
                TCHAR param[MAX_PATH + 1];
                memset(&mii, 0, sizeof(mii));
                mii.cbSize = sizeof(mii);
                mii.fMask = MIIM_TYPE;
                mii.cch = MAX_PATH;
                mii.dwTypeData = buf;
                GetMenuItemInfo(session_menu, wParam, false, &mii);
                if (!kageant_kitty_launch_allowed(hwnd))
                    break;
                param[0] = '\0';
                if (restrict_putty_acl)
                    strcat(param, "&R");
                strcat(param, "@");
                strcat(param, mii.dwTypeData);
                if ((INT_PTR)ShellExecute(hwnd, NULL, putty_path, param,
                                          _T(""), SW_SHOW) <= 32) {
                    MessageBox(NULL, "Unable to execute KiTTY!", "Error",
                               MB_OK | MB_ICONERROR);
                }
            }
            break;
          }
        }
        break;
      }
      case WM_NETEVENT:
      case WM_DONE_WITH_SOCKET:
        winselgui_response(message, wParam, lParam);
        return 0;
      case WM_DESTROY:
        quit_help(hwnd);
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hwnd, message, wParam, lParam);
}

static LRESULT CALLBACK wm_copydata_WndProc(HWND hwnd, UINT message,
                                            WPARAM wParam, LPARAM lParam)
{
    switch (message) {
      case WM_COPYDATA: {
        COPYDATASTRUCT *cds;
        char *mapname, *err;

        cds = (COPYDATASTRUCT *) lParam;
        if (cds->dwData != AGENT_COPYDATA_ID)
            return 0;              /* not our message, mate */
        /* KiTTY: wParam is the sending window, by the WM_COPYDATA contract -
         * best-effort requester identity for the mutation notices. */
        wmct.sender_pid = 0;
        if (wParam)
            GetWindowThreadProcessId((HWND)wParam, &wmct.sender_pid);
        mapname = (char *) cds->lpData;
        if (!mapname || cds->cbData == 0)
            return 0;              /* KiTTY: reject empty/NULL before indexing */
        if (mapname[cds->cbData - 1] != '\0')
            return 0;              /* failure to be ASCIZ! */
        err = answer_filemapping_message(mapname);
        if (err) {
#ifdef DEBUG_IPC
            debug("IPC failed: %s\n", err);
#endif
            sfree(err);
            return 0;
        }
        return 1;
      }
    }

    return DefWindowProc(hwnd, message, wParam, lParam);
}

static DWORD WINAPI wm_copydata_threadfunc(void *param)
{
    HINSTANCE inst = *(HINSTANCE *)param;

    HWND ipchwnd = CreateWindow(IPCCLASSNAME, IPCWINTITLE,
                                WS_OVERLAPPEDWINDOW | WS_VSCROLL,
                                CW_USEDEFAULT, CW_USEDEFAULT,
                                100, 100, NULL, NULL, inst, NULL);
    ShowWindow(ipchwnd, SW_HIDE);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) == 1) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return 0;
}

/*
 * Fork and Exec the command in cmdline. [DBW]
 */
void spawn_cmd(const char *cmdline, const char *args, int show)
{
    if (ShellExecute(NULL, _T("open"), cmdline,
                     args, NULL, show) <= (HINSTANCE) 32) {
        char *msg;
        msg = dupprintf("Failed to run \"%s\": %s", cmdline,
                        win_strerror(GetLastError()));
        MessageBox(NULL, msg, APPNAME, MB_OK | MB_ICONEXCLAMATION);
        sfree(msg);
    }
}

void noise_ultralight(NoiseSourceId id, unsigned long data)
{
    /* Pageant doesn't use random numbers, so we ignore this */
}

void cleanup_exit(int code)
{
    shutdown_help();
    exit(code);
}

static bool winpgnt_listener_ask_passphrase(
    PageantListenerClient *plc, PageantClientDialogId *dlgid,
    const char *comment)
{
    return ask_passphrase_common(dlgid, comment);
}

struct winpgnt_client {
    PageantListenerClient plc;
};
static const PageantListenerClientVtable winpgnt_vtable = {
    .log = NULL, /* no logging */
    .ask_passphrase = winpgnt_listener_ask_passphrase,
};

static struct winpgnt_client wpc[1];

HINSTANCE hinst;

static NORETURN void opt_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *msg = dupvprintf(fmt, ap);
    va_end(ap);

    MessageBox(NULL, msg, "kageant command line error", MB_ICONERROR | MB_OK);

    exit(1);
}

/* KiTTY: -h / -help / --help. If the caller has a console (cmd,
 * PowerShell), print the summary inline there - a GUI-subsystem exe is
 * not attached to it by default, so attach explicitly. Started with no
 * console (the Run box, a shortcut), fall back to a message box, the
 * same way the command-line errors do. */
static void show_cmdline_help(void)
{
    static const char help[] =
        "kageant - the KiTTY SSH authentication agent\n"
        "\n"
        "Usage:  kageant [options] [keyfile ...]\n"
        "\n"
        "Key files named on the command line are loaded at startup.\n"
        "\n"
        "-encrypted, -no-decrypt\n"
        "        load the key files that follow deferred: the\n"
        "        passphrase is asked at first use\n"
        "-keylist\n"
        "        open the key list window at startup\n"
        "-noload\n"
        "        clean slate: do not load the stored startup keys (no\n"
        "        passphrase prompts) and leave the stored list untouched;\n"
        "        key files named on the command line still load\n"
        "-c command [args ...]\n"
        "        run the command once the agent is up; everything\n"
        "        after -c is the command line\n"
        "-openssh-config FILE\n"
        "        write an OpenSSH client config file pointing ssh at\n"
        "        this agent's named pipe\n"
        "-unix PATH\n"
        "        also serve an AF_UNIX agent socket at PATH\n"
        "-restrict-acl\n"
        "        restrict the ACL of the kageant process\n"
        "-restrict-putty-acl\n"
        "        pass -restrict-acl on to KiTTY sessions started\n"
        "        from the tray menu\n"
        "-pgpfp\n"
        "        show the PGP fingerprints of the PuTTY release keys\n"
        "        (deprecated)\n"
        "-h, -help, --help\n"
        "        this summary\n";

    /* A redirected stdout (`kageant -h > file`, a pipe) is inherited even
     * by a GUI-subsystem exe - and it must be looked at BEFORE any
     * AttachConsole, which would replace the std handles with the
     * console's and send the text to the screen instead of the file. */
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    bool attached = false, opened = false;
    if (h == NULL || h == INVALID_HANDLE_VALUE) {
        /* No redirection. If the caller has a console, print on it. */
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            attached = true;
            h = CreateFile("CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
            opened = (h != INVALID_HANDLE_VALUE);
        }
    }
    if (h != NULL && h != INVALID_HANDLE_VALUE) {
        DWORD written;
        if (attached)   /* the shell's prompt is already mid-line */
            WriteFile(h, "\r\n", 2, &written, NULL);
        WriteFile(h, help, (DWORD)strlen(help), &written, NULL);
        if (opened)
            CloseHandle(h);
        if (attached)
            FreeConsole();
        return;
    }
    if (attached)
        FreeConsole();

    MessageBox(NULL, help, "kageant command line",
               MB_ICONINFORMATION | MB_OK);
}

#ifdef LEGACY_WINDOWS
BOOL sw_PeekMessage(LPMSG msg, HWND hwnd, UINT min, UINT max, UINT remove)
{
    static bool unicode_unavailable = false;
    if (!unicode_unavailable) {
        BOOL ret = PeekMessageW(msg, hwnd, min, max, remove);
        if (!ret && GetLastError() == ERROR_CALL_NOT_IMPLEMENTED)
            unicode_unavailable = true; /* don't try again */
        else
            return ret;
    }
    return PeekMessageA(msg, hwnd, min, max, remove);
}
#else
#define sw_PeekMessage PeekMessageW
#endif

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show)
{
    MSG msg;
    const char *command = NULL;
    const char *unixsocket = NULL;
    bool show_keylist_on_startup = false;
    Filename *openssh_config_file = NULL;

    typedef struct CommandLineKey {
        Filename *fn;
        bool add_encrypted;
    } CommandLineKey;

    CommandLineKey *clkeys = NULL;
    size_t nclkeys = 0, clkeysize = 0;

    dll_hijacking_protection();
    enable_dit();

    hinst = inst;

    /* KiTTY: the key list window uses a SysListView32. The Common Controls 6
     * manifest alone does not register the class - without this call the
     * dialog silently fails to create. */
    {
        INITCOMMONCONTROLSEX icc;
        icc.dwSize = sizeof(icc);
        icc.dwICC = ICC_LISTVIEW_CLASSES;
        InitCommonControlsEx(&icc);
    }

    /*
     * KiTTY: [KiTTY] restrictacl=yes in kitty.ini hardens kageant too.
     *
     * This is the process that actually holds private key material, so a user
     * who turns the setting on expects the agent covered - not just the
     * terminal windows. Done here, at the top of WinMain, so the ACL is in
     * place before any key is loaded.
     *
     * Read through kitty_inilight (the satellite-binary resolver, keyed off
     * OUR exe's folder) and deliberately WITHOUT consulting
     * kitty_inilight_registry_authoritative(): that rule decides whether the
     * ini or the registry owns a *setting*, and for this one the ini is the
     * only place it is ever honoured - in kitty.exe too, where the key is
     * read with readINI rather than the registry-first ReadParameterN. See
     * SetRestrictAclFlag in kitty/kitty.c for why registry-first would fail
     * open here.
     *
     * -restrict-acl on the command line (below) remains independent; applying
     * both is harmless.
     */
    {
        char b[16];
        if (kitty_inilight_read("KiTTY", "restrictacl", b, sizeof(b)) &&
            !stricmp(b, "yes"))
            restrict_process_acl();
    }

    if (should_have_security()) {
        /*
         * Attempt to get the security API we need.
         */
        if (!got_advapi()) {
            MessageBox(NULL,
                       "Unable to access security APIs. kageant will\n"
                       "not run, in case it causes a security breach.",
                       "kageant Fatal Error", MB_ICONERROR | MB_OK);
            return 1;
        }
    }

    /*
     * See if we can find our Help file.
     */
    init_help();

    /*
     * Look for the PuTTY binary (we will enable the saved session
     * submenu if we find it).
     */
    {
        char b[2048], *p, *q, *r;
        FILE *fp;
        GetModuleFileName(NULL, b, sizeof(b) - 16);
        r = b;
        p = strrchr(b, '\\');
        if (p && p >= r) r = p+1;
        q = strrchr(b, ':');
        if (q && q >= r) r = q+1;
        strcpy(r, "kitty.exe");   /* KiTTY: we launch our own terminal */
        if ( (fp = fopen(b, "r")) != NULL) {
            putty_path = dupstr(b);
            fclose(fp);
        } else
            putty_path = NULL;
    }

    /*
     * Process the command line, handling anything that can be done
     * immediately, but deferring adding keys until after we've
     * started up the main agent. Details of keys to be added are
     * stored in the 'clkeys' array.
     */
    bool add_keys_encrypted = false;
    AuxMatchOpt amo = aux_match_opt_init(opt_error);
    while (!aux_match_done(&amo)) {
        CmdlineArg *valarg;
        #define match_opt(...) aux_match_opt( \
            &amo, NULL, __VA_ARGS__, (const char *)NULL)
        #define match_optval(...) aux_match_opt( \
            &amo, &valarg, __VA_ARGS__, (const char *)NULL)

        if (aux_match_arg(&amo, &valarg)) {
            /*
             * Non-option arguments are expected to be key files, and
             * added to clkeys.
             */
            sgrowarray(clkeys, clkeysize, nclkeys);
            CommandLineKey *clkey = &clkeys[nclkeys++];
            clkey->fn = cmdline_arg_to_filename(valarg);
            clkey->add_encrypted = add_keys_encrypted;
        } else if (match_opt("-pgpfp")) {
            pgp_fingerprints_msgbox(NULL);
            return 0;
        } else if (match_opt("-h", "-help")) {
            /* --help arrives here too: the matcher folds a GNU-style
             * double dash onto the single-dash form. */
            show_cmdline_help();
            return 0;
        } else if (match_opt("-restrict-acl", "-restrict_acl",
                             "-restrictacl")) {
            restrict_process_acl();
        } else if (match_opt("-restrict-putty-acl", "-restrict_putty_acl")) {
            restrict_putty_acl = true;
        } else if (match_opt("-no-decrypt", "-no_decrypt",
                             "-nodecrypt", "-encrypted")) {
            add_keys_encrypted = true;
        } else if (match_opt("-keylist")) {
            show_keylist_on_startup = true;
        } else if (match_opt("-noload", "-no-load", "-clean")) {
            /* Clean slate: ignore the stored startup keys (no loads, no
             * passphrase prompts) and leave the stored list untouched. */
            kageant_noload_set();
        } else if (match_optval("-openssh-config", "-openssh_config")) {
            openssh_config_file = cmdline_arg_to_filename(valarg);
        } else if (match_optval("-unix")) {
            /* UNICODE: should this be a Unicode filename? Is there a
             * Unicode version of connect() that lets you give a
             * Unicode pathname when making an AF_UNIX socket? */
            unixsocket = cmdline_arg_to_str(valarg);
        } else if (match_opt("-c")) {
            /*
             * If we see `-c', then the rest of the command line
             * should be treated as a command to be spawned.
             */
            if (amo.arglist->args[amo.index]) {
                /* UNICODE: should use the UTF-8 or wide version, and
                 * CreateProcessW, to pass through arbitrary command lines */
                command = cmdline_arg_remainder_acp(
                    amo.arglist->args[amo.index]);
            } else {
                command = "";
            }
            break;
        } else {
            opt_error("unrecognised option '%s'\n",
                      cmdline_arg_to_str(amo.arglist->args[amo.index]));
        }
    }

    /*
     * Create and lock an interprocess mutex while we figure out
     * whether we're going to be the Pageant server or a client. That
     * way, two Pageant processes started up simultaneously will be
     * able to agree on which one becomes the server without a race
     * condition.
     */
    HANDLE mutex;
    {
        char *err;
        char *mutexname = agent_mutex_name();
        mutex = lock_interprocess_mutex(mutexname, &err);
        sfree(mutexname);
        if (!mutex) {
            MessageBox(NULL, err, "kageant Error", MB_ICONERROR | MB_OK);
            return 1;
        }
    }

    /*
     * Find out if Pageant is already running.
     */
    already_running = agent_exists();

    /*
     * If it isn't, we're going to be the primary Pageant that stays
     * running, so set up all the machinery to answer requests.
     */
    if (!already_running) {
        /* KiTTY: ask the MSI Restart Manager to relaunch us (kageant.exe, no
         * args) after an in-place upgrade closes us; we reload startup keys on
         * our own. Without this, RM closes the tray agent and never brings it
         * back. (Only the primary instance registers.) */
        RegisterApplicationRestart(L"", 0);
        /*
         * Set up the window class for the hidden window that receives
         * all the messages to do with our presence in the system tray.
         */

        if (!prev) {
            WNDCLASS wndclass;

            memset(&wndclass, 0, sizeof(wndclass));
            wndclass.lpfnWndProc = TrayWndProc;
            wndclass.hInstance = inst;
            wndclass.hIcon = LoadIcon(inst, MAKEINTRESOURCE(IDI_MAINICON));
            wndclass.lpszClassName = TRAYCLASSNAME;

            RegisterClass(&wndclass);
        }

        keylist = NULL;

        traywindow = CreateWindow(TRAYCLASSNAME, TRAYWINTITLE,
                                  WS_OVERLAPPEDWINDOW | WS_VSCROLL,
                                  CW_USEDEFAULT, CW_USEDEFAULT,
                                  100, 100, NULL, NULL, inst, NULL);
        winselgui_set_hwnd(traywindow);

        /*
         * Initialise the cross-platform Pageant code.
         */
        pageant_init();

        /* KiTTY: enable private-key usage confirmation for keys whose comment
         * requests it (see kageant_do_confirm). */
        kageant_confirm_hook = kageant_do_confirm;
        kageant_comment_confirm_hook = kageant_comment_wants_confirm;
        kageant_mutation_notice_hook = kageant_do_mutation_notice;
        kageant_ipc_blocked_hook = kageant_ipc_blocked;
        kageant_notify_hook = kageant_do_notify;
        kageant_keyuse_hook = kageant_note_keyuse;
        /* KiTTY: speak up about held-back keys when a client asks for the
         * identity list - see kageant_do_identities_asked for the rate limit. */
        kageant_identities_asked_hook = kageant_do_identities_asked;
        kageant_key_lifetime_hook = kageant_key_set_lifetime;   /* ssh-add -t */

        /*
         * Set up a named-pipe listener.
         */
        wpc->plc.vt = &winpgnt_vtable;
        wpc->plc.suppress_logging = true;
        if (should_have_security()) {
            Plug *pl_plug;
            struct pageant_listen_state *pl =
                pageant_listener_new(&pl_plug, &wpc->plc);
            char *pipename = agent_named_pipe_name();
            Socket *sock = new_named_pipe_listener(pipename, pl_plug);
            if (sk_socket_error(sock)) {
                char *err = dupprintf("Unable to open named pipe at %s "
                                      "for SSH agent:\n%s", pipename,
                                      sk_socket_error(sock));
                MessageBox(NULL, err, "kageant Error", MB_ICONERROR | MB_OK);
                return 1;
            }
            pageant_listener_got_socket(pl, sock);

            /*
             * If we've been asked to write out an OpenSSH config file
             * pointing at the named pipe, do so.
             */
            if (openssh_config_file) {
                FILE *fp = f_open(openssh_config_file, "w", true);
                if (!fp) {
                    char *err = dupprintf(
                        "Unable to write OpenSSH config file to %s",
                        filename_to_str(openssh_config_file));
                    MessageBox(NULL, err, "kageant Error",
                               MB_ICONERROR | MB_OK);
                    return 1;
                }
                kageant_write_identityagent(fp, pipename);
                fclose(fp);
            }

            sfree(pipename);

            /* KiTTY: if the user has enabled OpenSSH integration (tray
             * toggle, default off), refresh ~/.ssh/kageant.conf + the managed
             * Include now that the pipe listener is up. */
            if (kageant_openssh_get())
                kageant_openssh_apply(1);
        }

        /*
         * Set up an AF_UNIX listener too, if we were asked to.
         */
        if (unixsocket) {
            sk_init();

            /* FIXME: diagnose any error except file-not-found. Also,
             * check the file type if possible? */
            remove(unixsocket);

            Plug *pl_plug;
            struct pageant_listen_state *pl =
                pageant_listener_new(&pl_plug, &wpc->plc);
            Socket *sock = sk_newlistener_unix(unixsocket, pl_plug);
            if (sk_socket_error(sock)) {
                char *err = dupprintf("Unable to open AF_UNIX socket at %s "
                                      "for SSH agent:\n%s", unixsocket,
                                      sk_socket_error(sock));
                MessageBox(NULL, err, "kageant Error", MB_ICONERROR | MB_OK);
                return 1;
            }
            pageant_listener_got_socket(pl, sock);
        }

        /*
         * Set up the window class for the hidden window that receives
         * the WM_COPYDATA message used by the old-style Pageant IPC
         * system.
         */
        if (!prev) {
            WNDCLASS wndclass;

            memset(&wndclass, 0, sizeof(wndclass));
            wndclass.lpfnWndProc = wm_copydata_WndProc;
            wndclass.hInstance = inst;
            wndclass.lpszClassName = IPCCLASSNAME;

            RegisterClass(&wndclass);
        }

        /*
         * And launch the subthread which will open that hidden window and
         * handle WM_COPYDATA messages on it.
         */
        wmcpc.vt = &wmcpc_vtable;
        wmcpc.suppress_logging = true;
        pageant_register_client(&wmcpc);
        DWORD wm_copydata_threadid;
        wmct.ev_msg_ready = CreateEvent(NULL, false, false, NULL);
        wmct.ev_reply_ready = CreateEvent(NULL, false, false, NULL);
        HANDLE hThread = CreateThread(NULL, 0, wm_copydata_threadfunc,
                                      &inst, 0, &wm_copydata_threadid);
        if (hThread)
            CloseHandle(hThread); /* we don't need the thread handle */
        add_handle_wait(wmct.ev_msg_ready, wm_copydata_got_msg, NULL);
    }

    /*
     * Now we're either a fully set up Pageant server, or we know one
     * is running somewhere else. Either way, now it's safe to unlock
     * the mutex.
     */
    unlock_interprocess_mutex(mutex);

    /*
     * Add any keys provided on the command line.
     */
    for (size_t i = 0; i < nclkeys; i++) {
        CommandLineKey *clkey = &clkeys[i];
        win_add_keyfile(clkey->fn, clkey->add_encrypted);
        filename_free(clkey->fn);
    }
    sfree(clkeys);
    /* And forget any passphrases we stashed during that loop. */
    pageant_forget_passphrases();

    /* KiTTY: if "load keys on startup" is enabled, re-add the remembered keys
     * (encrypted/deferred). Primary instance only - a second invocation just
     * forwards to the running agent and exits. */
    if (!already_running && kageant_startup_get()) {
        kageant_load_startup_keys();
        pageant_forget_passphrases();
        kageant_apply_saved_order();   /* restore the user's offer order */
    }

    /*
     * Now our keys are present, spawn a command, if we were asked to.
     */
    if (command) {
        char *args;
        if (command[0] == '"')
            args = strchr(++command, '"');
        else
            args = strchr(command, ' ');
        if (args) {
            *args++ = 0;
            while (*args && isspace((unsigned char)*args)) args++;
        }
        spawn_cmd(command, args, show);
    }

    /*
     * If Pageant was already running, we leave now. If we haven't
     * even taken any auxiliary action (spawned a command or added
     * keys), complain.
     */
    if (already_running) {
        /* A BARE second instance exits SILENTLY now. The old "kageant is
         * already running" box treated it as a user error, but a bare
         * duplicate is an EXPECTED event on this fork: the MSI's Restart
         * Manager relaunches kageant.exe with no arguments after an upgrade,
         * and Windows' own restart-apps machinery does the same at login -
         * both while the autostart entry starts one too, so every upgrade or
         * restart popped the box at whoever logged in next. Keys or a
         * command on the line were always handed to the running agent and
         * exited quietly (that is the path above); the bare start now gets
         * the same quiet exit - the tray icon of the running agent is the
         * answer to "did it start?". */
        return 0;
    }

    /* Set up a system tray icon */
    AddTrayIcon(traywindow);
    /* KiTTY: the startup load ran BEFORE this icon existed, so anything it
     * refused could not reach the tooltip then. Compose it once here, or a key
     * held back at login would be missing from the one surface that is meant to
     * still be true hours later. */
    kageant_refresh_tray_tip();
    kageant_notify_startup_missing();
    /* KiTTY: if the in-memory key protection is not working, say so NOW,
     * once - and the tray tip above carries it for as long as it holds. */
    kageant_warn_unprotected_memory();
    /* KiTTY: a 1-second heartbeat to expire ssh-add -t keys. Cheap, and only
     * the primary instance (which owns traywindow) runs it. */
    SetTimer(traywindow, TID_KEY_LIFETIME, 1000, NULL);

    /* Accelerators used: nsvkxaol */
    systray_menu = CreatePopupMenu();
    if (putty_path) {
        session_menu = CreateMenu();
        AppendMenu(systray_menu, MF_ENABLED, IDM_PUTTY, "&New Session");
        AppendMenu(systray_menu, MF_POPUP | MF_ENABLED,
                   (UINT_PTR) session_menu, "Save&d Sessions");
        AppendMenu(systray_menu, MF_SEPARATOR, 0, 0);
    }
    AppendMenu(systray_menu, MF_ENABLED, IDM_VIEWKEYS,
               "&View Keys");
    AppendMenu(systray_menu, MF_ENABLED, IDM_ADDKEY, "Add &Key");
    AppendMenu(systray_menu, MF_ENABLED, IDM_ADDKEY_ENCRYPTED,
               "Add key (encrypted)");
    AppendMenu(systray_menu, MF_SEPARATOR, 0, 0);
    AppendMenu(systray_menu, MF_ENABLED, IDM_REMOVE_ALL,
               "Remove All Keys");
    AppendMenu(systray_menu, MF_ENABLED, IDM_REENCRYPT_ALL,
               "Re-encrypt All Keys");
    AppendMenu(systray_menu, MF_SEPARATOR, 0, 0);
    /* KiTTY: opt-in Windows OpenSSH integration (default off). */
    AppendMenu(systray_menu, MF_ENABLED |
               (kageant_openssh_get() ? MF_CHECKED : MF_UNCHECKED),
               IDM_OPENSSH_INTEGRATION, "Register as Windows &OpenSSH agent");
    /* KiTTY: two separate things, one each. "Start at login" is an artifact
     * (Run entry or Startup shortcut, verified to be THIS kageant); "load
     * remembered keys" is a stored flag. They used to share one command, so
     * neither could be had without the other. */
    AppendMenu(systray_menu, MF_ENABLED |
               (kageant_autostart_active() ? MF_CHECKED : MF_UNCHECKED),
               IDM_LOAD_ON_STARTUP, "&Start kageant at login");
    AppendMenu(systray_menu, MF_ENABLED |
               (kageant_startup_get() ? MF_CHECKED : MF_UNCHECKED),
               IDM_LOAD_KEYS, "&Load remembered keys at startup");
    /* KiTTY: opt-in (default on) tray balloon when a key is used to sign. */
    AppendMenu(systray_menu, MF_ENABLED |
               (kageant_notify_get() ? MF_CHECKED : MF_UNCHECKED),
               IDM_NOTIFY_KEYUSE, "&Notify when a key is used");
    /* KiTTY: opt-in (default off) yes/no prompt before any key may sign
     * (classic [Agent] askconfirmation; per-key comment opt-in still works). */
    AppendMenu(systray_menu, MF_ENABLED |
               (kageant_confirm_get() ? MF_CHECKED : MF_UNCHECKED),
               IDM_CONFIRM_KEYUSE, "Ask &confirmation before each key use");
    AppendMenu(systray_menu, MF_ENABLED,
               IDM_RESUME_CONFIRM, "Res&ume key-use confirmations");
    /* KiTTY: the same [Agent] settings dialog the key list window opens,
     * reachable straight from the tray. */
    AppendMenu(systray_menu, MF_ENABLED, IDM_SETTINGS, "Settin&gs...");
    AppendMenu(systray_menu, MF_SEPARATOR, 0, 0);
    if (has_help())
        AppendMenu(systray_menu, MF_ENABLED, IDM_HELP, "&Help");
    AppendMenu(systray_menu, MF_ENABLED, IDM_ABOUT, "&About");
    AppendMenu(systray_menu, MF_SEPARATOR, 0, 0);
    AppendMenu(systray_menu, MF_ENABLED, IDM_CLOSE, "E&xit");
    initial_menuitems_count = GetMenuItemCount(session_menu);

    /* Set the default menu item. */
    SetMenuDefaultItem(systray_menu, IDM_VIEWKEYS, false);

    ShowWindow(traywindow, SW_HIDE);

    /* KiTTY: land the user on the key list when there is nothing to work
     * with yet - started with -noload, or a fresh agent with no keys loaded
     * and none waiting on absent media (a new install). Beats hunting for the
     * tray icon to add the first key. */
    if (!already_running && !show_keylist_on_startup &&
        (kageant_noload() ||
         (pageant_count_ssh1_keys() + pageant_count_ssh2_keys() == 0 &&
          kageant_pending_count() == 0)))
        show_keylist_on_startup = true;

    /* Open the visible key list window, if we've been asked to. */
    if (show_keylist_on_startup)
        create_keylist_window();

    /*
     * Main message loop.
     */
    while (true) {
        int n;

        HandleWaitList *hwl = get_handle_wait_list();

        DWORD timeout = toplevel_callback_pending() ? 0 : INFINITE;
        n = MsgWaitForMultipleObjects(hwl->nhandles, hwl->handles, false,
                                      timeout, QS_ALLINPUT);

        if ((unsigned)(n - WAIT_OBJECT_0) < (unsigned)hwl->nhandles)
            handle_wait_activate(hwl, n - WAIT_OBJECT_0);
        handle_wait_list_free(hwl);

        while (sw_PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT)
                goto finished;         /* two-level break */

            if (IsWindow(keylist) && IsDialogMessage(keylist, &msg))
                continue;
            if (IsWindow(aboutbox) && IsDialogMessage(aboutbox, &msg))
                continue;
            if (IsWindow(nonmodal_passphrase_hwnd) &&
                IsDialogMessage(nonmodal_passphrase_hwnd, &msg))
                continue;

            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        run_toplevel_callbacks();
    }
  finished:

    /* Clean up the system tray icon */
    {
        NOTIFYICONDATA tnid;

        tnid.cbSize = sizeof(NOTIFYICONDATA);
        tnid.hWnd = traywindow;
        tnid.uID = 1;

        Shell_NotifyIcon(NIM_DELETE, &tnid);

        DestroyMenu(systray_menu);
    }

    if (keypath)
        filereq_saved_dir_free(keypath);

    if (openssh_config_file) {
        /*
         * Leave this file around, but empty it, so that it doesn't
         * refer to a pipe we aren't listening on any more.
         */
        FILE *fp = f_open(openssh_config_file, "w", true);
        if (fp)
            fclose(fp);
    }

    /* KiTTY: same for the toggle-managed kageant.conf - empty it on exit so a
     * lingering Include doesn't point ssh at a pipe we no longer serve. */
    if (kageant_openssh_get()) {
        char *kc = kageant_ssh_path("kageant.conf");
        if (kc) {
            FILE *fp = fopen(kc, "wb");
            if (fp) fclose(fp);
            sfree(kc);
        }
    }

    cleanup_exit(msg.wParam);
    return msg.wParam;                 /* just in case optimiser complains */
}
