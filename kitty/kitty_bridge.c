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
#include "storage.h"       /* read_setting_s/i: per-session hotkey scan */
#include "kitty.h"
#include "kitty_portfwd.h"
#include "kitty_broadcast.h"
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
#include "kitty_secretstore.h"
#include "kitty_gui.h"
#include "kitty_image.h"
#include "kitty_tools.h"
#include "kitty_ssh.h"
#include "kitty_bridge.h"
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
extern bool sshver_overridden;          /* utils/version.c, beside sshver */
static char sshver_builtin[40];
void set_sshver(const char *vers) {
    if (!vers) return;
    if (!sshver_builtin[0])
        strncpy(sshver_builtin, sshver, sizeof(sshver_builtin) - 1);
    if (!vers[0]) {
        /* Cleared: back to the built-in token, banner as upstream sends it. */
        strncpy(sshver, sshver_builtin, sizeof(sshver) - 1);
        sshver_overridden = false;
        return;
    }
    strncpy(sshver, vers, sizeof(sshver) - 1);
    sshver[sizeof(sshver) - 1] = '\0';
    sshver_overridden = true;
}
/* What the banner currently carries - the settings tree shows and edits it.
 * When overridden the string IS the whole software token (ssh/verstring.c
 * drops the implementation name before it). */
const char *get_sshver(void) { return sshver_overridden ? sshver : ""; }
/* The banner exactly as a server will receive it, for the settings tree's
 * preview line: the verstring code's own recipe, minus the packet framing. */
void kitty_ssh_banner_preview(char *buf, size_t size) {
    snprintf(buf, size, "SSH-2.0-%s%s", sshver_overridden ? "" : "PuTTY", sshver);
}

/* save_open_settings_forced now implemented in kitty_settings_forced.c */

/* Launch a NEW session process from an in-memory Conf, by serialising it into
 * a file-mapping and spawning "<exe> &<filemap>:<size>" - exactly the native
 * 0.84 Duplicate-Session mechanism (windows/window.c IDM_DUPSESS), which the
 * child parses via handle_special_filemapping_cmdline(). */
static void RunSessionWithConfSettings(Conf *conf) {
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

    /* The password fields travel WRAPPED FOR THE LOGON: the reader is another
     * process, so the process-scoped wrapping this window uses would be
     * unreadable there, and the cleartext must not be what lies in a shared
     * section. Done on a copy - the live Conf keeps its own wrapping - and the
     * serialised bytes go into a burn-on-free strbuf. The child re-wraps for
     * itself right after conf_deserialise (windows/window.c, windows/putty.c).
     * The mapping itself is unmapped and closed below but never zeroed; what
     * makes that acceptable is that it now carries only the wrapped form. */
    {
        Conf *wire = conf_copy(conf);
        kitty_pw_seal_for_handoff(wire);
        serbuf = strbuf_new_nm();
        conf_serialise(BinarySink_UPCAST(serbuf), wire);
        kitty_pw_wipe(wire);   /* conf_free does not clear what it frees */
        conf_free(wire);
    }
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

    /* Password fields wrapped for the logon, on a copy - see the same block in
     * RunSessionWithConfSettings above for why. */
    {
        Conf *wire = conf_copy(conf);
        kitty_pw_seal_for_handoff(wire);
        serbuf = strbuf_new_nm();
        conf_serialise(BinarySink_UPCAST(serbuf), wire);
        kitty_pw_wipe(wire);   /* conf_free does not clear what it frees */
        conf_free(wire);
    }
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
    if (pass != NULL) kitty_pw_set(newconf, CONF_password, pass);

    /* The password is stored here in the same wrapped in-memory form as
     * everywhere else (kitty_pwmem.c), and the two launchers below re-wrap it
     * for the logon before it enters the file mapping the child inherits. It
     * never reaches the settings store on this path.
     * The old MASKPASS here turned the password into high-byte garbage ->
     * Duplicate-Session / open-new-with-current auto-login sent a corrupted
     * password (even for ASCII), so it is stored exactly as handed over. */

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
    kitty_pw_wipe(newconf);   /* conf_free does not clear what it frees */
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
/* The launcher's Hide all, Unhide all and session entries (kitty_launcher.c
 * ManageHideOne, ManageUnHideOne, ManageSwitch) post IDM_HIDE, IDM_UNHIDE and
 * IDM_SWITCH_HIDE to every terminal window. A window sent to the tray
 * (VISIBLE_TRAY) is left alone: its tray icon is the way back. Showing uses
 * SW_SHOW rather than SW_RESTORE, so a window hidden while maximized or
 * minimized comes back in that state. */
void kitty_launcher_hide(HWND hwnd) {
    if (GetVisibleFlag() == VISIBLE_YES) {
        ShowWindow(hwnd, SW_HIDE);
        SetVisibleFlag(VISIBLE_NO);
    }
}
void kitty_launcher_unhide(HWND hwnd) {
    if (GetVisibleFlag() == VISIBLE_NO) {
        ShowWindow(hwnd, SW_SHOW);
        SetVisibleFlag(VISIBLE_YES);
    }
}
void kitty_launcher_switch_hide(HWND hwnd) {
    if (GetVisibleFlag() == VISIBLE_YES)
        kitty_launcher_hide(hwnd);
    else if (GetVisibleFlag() == VISIBLE_NO)
        kitty_launcher_unhide(hwnd);
}
void kitty_rollup(HWND hwnd, int resize_action) {
    if (GetWinrolFlag())
        ManageWinrol(hwnd, resize_action);
}

/* window.c bridge fn (defined in window.c MOD_PERSO block) */
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
void kitty_get_file(HWND hwnd) { GetFile(hwnd); }
void kitty_start_filezilla(HWND hwnd) { StartFileZilla(hwnd); }

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
        snprintf( buffer, sizeof(buffer), KT_BRIDGE_ABOUT_VERSION_TEST, BuildVersionTime,
                KITTY_TEST_BUILD_LABEL);
#else
        snprintf( buffer, sizeof(buffer), KT_BRIDGE_ABOUT_VERSION, BuildVersionTime);
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
            ShellExecute(hwnd, "open", "https://github.com/hknet/KiTTY",
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
    if (SaveFileName(hwnd, filename, KT_CAP_SAVE_FILE, buffer)) {
        save_open_settings_forced(filename, conf);
    }
}

/* ---------------------------------------------------------------------------
 * Launcher global-hotkey helpers.
 *
 * A per-session hotkey is a machine-wide claim, not an ordinary session
 * setting: RegisterHotKey hands each (modifiers, key) pair to exactly one
 * window, so two sessions carrying the same spec means one of them silently
 * never fires. These helpers give every producer of that state (config-box
 * save, session import, the launcher's registration loop) one shared parser
 * and one shared way to ask "who else holds this combination?".
 * ------------------------------------------------------------------------- */

static char *hotkey_trim(char *s) {
    char *e;
    while (*s == ' ' || *s == '\t') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
        *--e = '\0';
    return s;
}

/* Parse a spec like "Ctrl+Alt+K" into RegisterHotKey (modifiers, vk).
 * Non-destructive (works on a copy). Usable specs need at least one modifier
 * plus A-Z, 0-9 or F1-F24; returns nonzero only for those, so comparing the
 * parsed pair - never the string - is what detects a conflict ("ctrl+alt+k"
 * and "Ctrl+Alt+K" are the same claim). */
int kitty_parse_hotkey_spec(const char *spec, unsigned int *mods, unsigned int *vk)
{
    char work[256], *p, *tok, *keytok = NULL;
    *mods = 0; *vk = 0;
    if (!spec) return 0;
    strncpy(work, spec, sizeof(work)-1); work[sizeof(work)-1] = '\0';
    p = hotkey_trim(work);
    if (!*p) return 0;
    for (tok = strtok(p, "+"); tok != NULL; tok = strtok(NULL, "+")) {
        tok = hotkey_trim(tok);
        if (!stricmp(tok, "Ctrl") || !stricmp(tok, "Control")) *mods |= MOD_CONTROL;
        else if (!stricmp(tok, "Shift")) *mods |= MOD_SHIFT;
        else if (!stricmp(tok, "Alt")) *mods |= MOD_ALT;
        else if (!stricmp(tok, "Win") || !stricmp(tok, "Windows")) *mods |= MOD_WIN;
        else keytok = tok;
    }
    if (keytok == NULL) return 0;
    if (strlen(keytok) == 1) {
        char c = keytok[0];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) *vk = (unsigned int)c;
    } else if ((keytok[0] == 'F' || keytok[0] == 'f') &&
               keytok[1] >= '1' && keytok[1] <= '9') {
        int n = atoi(keytok + 1);
        if (n >= 1 && n <= 24) *vk = VK_F1 + n - 1;
    }
    return (*mods != 0 && *vk != 0);
}

/* One saved session's enabled hotkey, read straight from the store (two keys,
 * not a full conf load - the scans below visit every session). Fills
 * (mods,vk) and returns nonzero only when the hotkey is enabled AND parses. */
static int hotkey_of_session(const char *name, unsigned int *mods, unsigned int *vk)
{
    settings_r *r = open_settings_r(name);
    char *spec;
    int en, ok = 0;
    if (!r) return 0;
    en = read_setting_i(r, "LauncherGlobalHotkeyEnabled", 0);
    spec = read_setting_s(r, "LauncherGlobalHotkey");
    close_settings_r(r);
    if (en && spec) ok = kitty_parse_hotkey_spec(spec, mods, vk);
    if (spec) sfree(spec);
    return ok;
}

/* List (", "-separated into `names`, which may be NULL) every saved session
 * other than `exclude` whose enabled hotkey parses to the same (mods,vk).
 * Returns the number of matches. */
int kitty_hotkey_conflict_scan(unsigned int mods, unsigned int vk,
                               const char *exclude, char *names, int nameslen)
{
    struct sesslist sl;
    int i, n = 0;
    unsigned int m2, v2;
    if (names && nameslen > 0) names[0] = '\0';
    get_sesslist(&sl, true);
    for (i = 0; i < sl.nsessions; i++) {
        const char *nm = sl.sessions[i];
        if (!strcmp(nm, KITTY_DEFAULT_SESSION)) continue;
        if (exclude && !strcmp(nm, exclude)) continue;
        if (!hotkey_of_session(nm, &m2, &v2)) continue;
        if (m2 != mods || v2 != vk) continue;
        if (names && nameslen > 0) {
            if (n) strncat(names, ", ", nameslen - strlen(names) - 1);
            strncat(names, nm, nameslen - strlen(names) - 1);
        }
        n++;
    }
    get_sesslist(&sl, false);
    return n;
}

/* How many saved sessions other than `exclude` hold a usable enabled hotkey.
 * The config box compares this against KITTY_LAUNCHER_HOTKEY_MAX before
 * letting another one be enabled. */
int kitty_hotkey_enabled_count(const char *exclude)
{
    struct sesslist sl;
    int i, n = 0;
    unsigned int m, v;
    get_sesslist(&sl, true);
    for (i = 0; i < sl.nsessions; i++) {
        const char *nm = sl.sessions[i];
        if (!strcmp(nm, KITTY_DEFAULT_SESSION)) continue;
        if (exclude && !strcmp(nm, exclude)) continue;
        if (hotkey_of_session(nm, &m, &v)) n++;
    }
    get_sesslist(&sl, false);
    return n;
}

/* Whole-store view for batch operations (import): one line per hotkey held by
 * more than one session, "spec: name, name, ...". Returns the number of
 * conflicting hotkeys; the report is truncated silently if buf runs out. */
int kitty_hotkey_conflict_report(char *buf, int buflen)
{
    struct sesslist sl;
    int i, j, nconf = 0;
    unsigned int *pm, *pv;
    unsigned char *has;
    if (buf && buflen > 0) buf[0] = '\0';
    get_sesslist(&sl, true);
    pm = snewn(sl.nsessions, unsigned int);
    pv = snewn(sl.nsessions, unsigned int);
    has = snewn(sl.nsessions, unsigned char);
    for (i = 0; i < sl.nsessions; i++) {
        has[i] = 0;
        if (!strcmp(sl.sessions[i], KITTY_DEFAULT_SESSION)) continue;
        has[i] = (unsigned char)hotkey_of_session(sl.sessions[i], &pm[i], &pv[i]);
    }
    for (i = 0; i < sl.nsessions; i++) {
        int dupes = 0, first_earlier = 0;
        if (!has[i]) continue;
        for (j = 0; j < i; j++)
            if (has[j] && pm[j] == pm[i] && pv[j] == pv[i]) { first_earlier = 1; break; }
        if (first_earlier) continue;    /* group already reported from j */
        for (j = i + 1; j < sl.nsessions; j++)
            if (has[j] && pm[j] == pm[i] && pv[j] == pv[i]) dupes++;
        if (!dupes) continue;
        nconf++;
        if (buf && buflen > 0) {
            settings_r *r = open_settings_r(sl.sessions[i]);
            char *spec = r ? read_setting_s(r, "LauncherGlobalHotkey") : NULL;
            if (r) close_settings_r(r);
            if (buf[0]) strncat(buf, "\n", buflen - strlen(buf) - 1);
            strncat(buf, spec ? spec : "?", buflen - strlen(buf) - 1);
            strncat(buf, ": ", buflen - strlen(buf) - 1);
            strncat(buf, sl.sessions[i], buflen - strlen(buf) - 1);
            for (j = i + 1; j < sl.nsessions; j++)
                if (has[j] && pm[j] == pm[i] && pv[j] == pv[i]) {
                    strncat(buf, ", ", buflen - strlen(buf) - 1);
                    strncat(buf, sl.sessions[j], buflen - strlen(buf) - 1);
                }
            if (spec) sfree(spec);
        }
    }
    sfree(pm); sfree(pv); sfree(has);
    get_sesslist(&sl, false);
    return nconf;
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
    /* Kept in the WRAPPED form the Conf holds (kitty_pwmem.c) - this snapshot
     * outlives the connect, and its one reader (kitty_xfer.c) unwraps it into
     * its own buffer at the moment it builds a command. */
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
     * It is a mode about where the
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
     * resolve/lookup need the current set - otherwise a chosen named proxy is
     * silently dropped. */
    InitProxyList();
    /* Apply only when the selector is shown (yes, or auto with proxies defined)
     * - matches the config box, so a proxy chosen in the droplist takes effect
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
