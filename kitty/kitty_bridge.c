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

/* TODO: launch a session with the current settings */
void RunSessionWithCurrentSettings(HWND hwnd, Conf *oldconf, const char *host,
                                   const char *user, const char *pass,
                                   const int port, const char *remotepath) {
    (void)hwnd; (void)oldconf; (void)host; (void)user;
    (void)pass; (void)port; (void)remotepath;
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
