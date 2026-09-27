/*
 * kitty_bridge.h - the glue between the terminal window and the KiTTY modules
 * (kitty_bridge.c): menu actions, settings hand-over to a child, the About
 * box, workplace proxy selection. The bulk session export and import are
 * declared in kitty_exportbundle.h.
 */

#ifndef KITTY_BRIDGE_H
#define KITTY_BRIDGE_H
#include "putty.h"

/* ---- exported from kitty/kitty_bridge.c ---- */
int DebugAddPassword(const char *fct, const char *pwd);
void RunConfigBoxWithConfSettings(Conf *conf);
void RunSessionWithCurrentSettings(HWND hwnd, Conf *oldconf, const char *host, const char *user, const char *pass, const int port, const char *remotepath);
int SwitchLogMode(void);
const char *get_sshver(void);
void kitty_about(HWND hwnd);
void kitty_antiidle_tick(HWND hwnd);
int kitty_apply_background(HWND hwnd, Conf *conf);
void kitty_apply_icon(HWND hwnd, Conf *conf);
void kitty_autocommand_rearm(void);
int kitty_autocommand_tick(HWND hwnd);
void kitty_bw(HWND hwnd);
void kitty_dup_session(HWND hwnd, Conf *conf);
void kitty_export_settings(HWND hwnd, Conf *conf);
void kitty_font_resize(Terminal *term, Conf *conf, int dec);
void kitty_get_file(HWND hwnd);
void kitty_launcher_hide(HWND hwnd);
void kitty_launcher_switch_hide(HWND hwnd);
void kitty_launcher_unhide(HWND hwnd);
void kitty_launcher_window_gone(HWND hwnd);
/* Test builds with KITTY_CTRLTAB_TRACE set: a Ctrl+Tab event line to
 * ctrltab.log beside the exe. Otherwise nothing is written. */
void kitty_ctrltab_trace(const char *fmt, ...);
/* One exit confirmation for windows closed together (kitty_bridge.c). */
#define TIMER_CLOSEGROUP 8716
enum { KCG_WAIT, KCG_CLOSE, KCG_STAY, KCG_SHOWBOX };
int kitty_closegroup_request(HWND hwnd);
int kitty_closegroup_poll(HWND hwnd, int *count);
void kitty_closegroup_answer(int yes);
void kitty_negative(HWND hwnd);
void kitty_port_knock(Conf *conf);
void kitty_print(HWND hwnd);
void kitty_protect(HWND hwnd, TermWin *tw, Conf *conf);
void kitty_proxy_select(Conf *conf);
void kitty_restore_icon(HWND hwnd, Conf *conf);
void kitty_rollup(HWND hwnd, int resize_action);
void kitty_send_file(HWND hwnd);
void kitty_send_to_tray(HWND hwnd);
void kitty_shortcuts_toggle(HWND hwnd);
void kitty_showportfwd(HWND hwnd, Conf *conf);
void kitty_ssh_banner_preview(char *buf, size_t size);
void kitty_start_filezilla(HWND hwnd);
void kitty_start_winscp(HWND hwnd);
extern int CryptFileFlag;
extern char *kitty_cli_loginscript;
extern int kitty_workplace_applied;

/* ---- exported from windows/window.c ---- */
#ifdef MOD_LAUNCHER
void kitty_launcher_watch_arm(HWND hwnd, HWND launcher);
#endif

/* ---- exported from kitty/kitty_bridge.c ---- */
#ifdef MOD_LAUNCHER
HWND kitty_launcher_window(void);   /* the running launcher of this install, or NULL */
BOOL kitty_launcher_spawn(void);    /* start one, do not wait for it */
#endif

#endif /* KITTY_BRIDGE_H */
