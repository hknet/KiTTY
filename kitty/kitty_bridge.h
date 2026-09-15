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

#endif /* KITTY_BRIDGE_H */
