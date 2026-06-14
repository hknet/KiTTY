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

/* KiTTY logging mode toggle (originally KiTTY logging.c) */
int LogMode = 0;
int SwitchLogMode(void) { LogMode = abs(LogMode - 1); return LogMode; }

/* KiTTY crypt-file flag (originally kitty_settings.c) */
int CryptFileFlag = 0;
int SwitchCryptFlag(void) { CryptFileFlag = abs(CryptFileFlag - 1); return CryptFileFlag; }

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

#ifndef MOD_NOPASSWORD
    {
        char pst[4096];
        strcpy(pst, conf_get_str(newconf, CONF_password));
        MASKPASS(GetCryptSaltFlag(), pst);
        if (pass != NULL) strcpy(pst, pass);
        conf_set_str(newconf, CONF_password, pst);
        memset(pst, 0, strlen(pst));
    }
#else
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
    ManageProtect(hwnd, tw, (char*)conf_get_str(conf, CONF_wintitle));
}
void kitty_print(HWND hwnd) { ManagePrint(hwnd); }

void kitty_negative(HWND hwnd) { NegativeColours(hwnd); }
void kitty_bw(HWND hwnd) { BlackOnWhiteColours(hwnd); }
void kitty_showportfwd(HWND hwnd, Conf *conf) { ShowPortfwd(hwnd, conf); }
void kitty_shortcuts_toggle(HWND hwnd) { ManageShortcutsFlag(hwnd); }
void kitty_start_winscp(HWND hwnd) { StartWinSCP(hwnd, NULL, NULL, NULL); }
void kitty_send_file(HWND hwnd) { SendFile(hwnd); }

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
    while (AutoCommand[i] != '\0') {
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
