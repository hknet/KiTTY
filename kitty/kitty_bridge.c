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
int RunSession(HWND hwnd, const char *folder_in, char *session_in);
void del_settings(const char *sessionname);
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

void RunSessionWithCurrentSettings(HWND hwnd, Conf *oldconf, const char *host,
                                   const char *user, const char *pass,
                                   const int port, const char *remotepath) {
    Conf *newconf = conf_copy(oldconf);
    (void)port;
    if (host != NULL) conf_set_str(newconf, CONF_host, host);
    if (user != NULL) conf_set_str(newconf, CONF_username, user);
    if (pass != NULL) conf_set_str(newconf, CONF_password, pass);

    /* Keep CONF_password PLAINTEXT here. newconf is serialised straight to the
     * child via the inherit-only file-mapping (RunSessionWithConfSettings) or
     * saved to __STARTUP, and the child reads CONF_password raw at connect time.
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
        save_settings("__STARTUP", newconf);
        RunSession(hwnd, conf_get_str(oldconf, CONF_folder), "__STARTUP");
        del_settings("__STARTUP");
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

/* Per-session icon (IconeFlag/CONF_icone/CONF_iconefile). Mirrors KiTTY
 * window.c: when icons are enabled (GetIconeFlag()!=-1) apply either the
 * external icon file (CONF_iconefile) or the embedded icon set indexed by
 * CONF_icone via SetNewIcon. */
void kitty_apply_icon(HWND hwnd, Conf *conf) {
    if (GetIconeFlag() == -1) return;
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
 * SetConnBreakIcon(). Unlike kitty_apply_icon() this always runs (no
 * GetIconeFlag()==-1 early return), so the broken-connection icon never sticks
 * once the session is back up. */
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
        sprintf(buffer, "KiTTY - %s\r\nTEST BUILD: %s", BuildVersionTime,
                KITTY_TEST_BUILD_LABEL);
#else
        sprintf(buffer, "KiTTY - %s", BuildVersionTime);
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

void kitty_export_all_sessions(HWND hwnd) {
    char dir[4096];
    int n, fail = 0;
    char msg[4400];
    if (!OpenDirName(hwnd, dir)) return;
    n = kitty_export_all_to_dir(dir, &fail);
    snprintf(msg, sizeof(msg),
             "Exported %d session%s (%d failed) to:\n%s\n\n"
             "Saved passwords were wrapped by the portable protection policy: "
             "master password if set (usable on another machine), otherwise "
             "Windows DPAPI (usable only by this account on this machine).",
             n, n == 1 ? "" : "s", fail, dir);
    MessageBoxA(hwnd, msg, "KiTTY session export",
                MB_OK | (fail ? MB_ICONWARNING : MB_ICONINFORMATION));
}

static int kitty_import_one_ktx(const char *path) {
    Conf *conf = conf_new();
    char *stem, *name, *err;
    const char *base, *ext = ktx_ext();
    size_t sl, el = strlen(ext);
    int ok = 0;
    load_open_settings_forced((char *)path, conf);
    base = strrchr(path, '\\');
    base = base ? base + 1 : path;
    stem = dupstr(base);
    sl = strlen(stem);
    if (sl > el && !_stricmp(stem + sl - el, ext))
        stem[sl - el] = '\0';
    name = kitty_session_fname_unmunge(stem);
    if (name && name[0]) {
        err = save_settings(name, conf);
        ok = (err == NULL);
        if (err) sfree(err);
    }
    sfree(stem);
    sfree(name);
    conf_free(conf);
    return ok;
}

void kitty_import_sessions(HWND hwnd) {
    char *buf = snewn(65536, char);
    OPENFILENAMEA ofn;
    int n = 0, fail = 0;
    char msg[512];
    buf[0] = '\0';
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = "Connection files (*.ktx)\0*.ktx\0All files (*.*)\0*.*\0\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = 65536;
    ofn.lpstrTitle = "Import sessions...";
    ofn.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST |
                OFN_HIDEREADONLY;
    if (!GetOpenFileNameA(&ofn)) { sfree(buf); return; }
    /* OFN_EXPLORER multiselect returns "dir\0file1\0file2\0\0"; a single
     * selection is just "fullpath\0". */
    {
        char *p = buf + strlen(buf) + 1;
        if (*p == '\0') {
            if (kitty_import_one_ktx(buf)) n++; else fail++;
        } else {
            for (; *p; p += strlen(p) + 1) {
                char *path = dupprintf("%s\\%s", buf, p);
                if (kitty_import_one_ktx(path)) n++; else fail++;
                sfree(path);
            }
        }
    }
    /* Also restore named proxies from the bundle's Proxies\ subfolder (Piece 7).
     * OFN_EXPLORER multiselect makes buf the directory; a single selection makes
     * buf a full path, so derive the directory from it. */
    {
        char dir[4096];
        char *pp = buf + strlen(buf) + 1;
        strncpy(dir, buf, sizeof(dir) - 1); dir[sizeof(dir) - 1] = '\0';
        if (*pp == '\0') { char *slash = strrchr(dir, '\\'); if (slash) *slash = '\0'; }
        kitty_import_proxies_from_dir(dir);
    }
    sfree(buf);
    snprintf(msg, sizeof(msg),
             "Imported %d session%s (%d failed).\n\n"
             "Saved passwords were re-protected for this storage backend "
             "(registry: Windows DPAPI; portable files: master password).",
             n, n == 1 ? "" : "s", fail);
    MessageBoxA(hwnd, msg, "KiTTY session import",
                MB_OK | (fail ? MB_ICONWARNING : MB_ICONINFORMATION));
}

/* Core import: load every .ktx in dir as a session (no UI). Returns the count
 * imported; *failOut (optional) gets the failure count. Used by -importdir. */
int kitty_import_dir(const char *dir, int *failOut) {
    char *pat = dupprintf("%s\\*%s", dir, ktx_ext());
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    int n = 0, fail = 0;
    sfree(pat);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            char *path = dupprintf("%s\\%s", dir, fd.cFileName);
            if (kitty_import_one_ktx(path)) n++; else fail++;
            sfree(path);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    kitty_import_proxies_from_dir(dir);   /* restore named proxies too (Piece 7) */
    if (failOut) *failOut = fail;
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

/* Anti-idle: fired every 30s from a repeating TIMER_ANTIIDLE. KiTTY counts
 * ticks (AntiIdleCount) and, once AntiIdleCountMax (default 6 => 180s) is
 * reached, sends the keepalive string: CONF_antiidle if set, else the
 * kitty.ini-loaded global AntiIdleStr. Mirrors KiTTY window.c:3646-3651. */
void kitty_antiidle_tick(HWND hwnd)
{
    const char *s;
    AntiIdleCount += 1;
    if (AntiIdleCount < AntiIdleCountMax)
        return;
    AntiIdleCount = 0;
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
void kitty_proxy_select(Conf *conf)
{
    const char *name;
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
}
#endif
