/*
 * kitty_gui.h - the KiTTY entry points of the shared Windows GUI files
 * (window.c, dialog.c, controls.c, kitty_pace.c, kitty_pace_frame.c), for
 * the KiTTY modules that call into them. Only meaningful under MOD_PERSO.
 */

#ifndef KITTY_GUI_H
#define KITTY_GUI_H
#include "putty.h"
/* dialog.h and win-gui-seat.h have no include guard, so they are not
 * included here; the types are named through their struct tags. */
struct dlgcontrol; struct dlgparam; struct controlbox; struct WinGuiSeat;

/* ---- exported from windows/controls.c ---- */
HWND kitty_cfg_item(HWND dlg, int id);
void kitty_controls_set_dir_picker(int (*fn)(HWND, char *, const char *, const char *));
void kitty_dlg_combobox_select_all(struct dlgcontrol *ctrl, struct dlgparam *dp);
extern int kitty_cfg_btn_basew_du;
extern int kitty_cfg_btn_fullw_du;
extern bool kitty_cfg_create_hidden;
extern HWND kitty_cfg_panel_host;

/* ---- exported from windows/dialog.c ---- */
HWND kitty_cfg_ctrl_hwnd(struct dlgcontrol *ctrl);
void kitty_cfg_goto_panel(const char *path);
void kitty_cfg_set_leave_guard(bool (*fn)(void));
void kitty_cfg_show_aux_box(void (*setup)(struct controlbox *, void *), void *ctx, const char *caption, HWND owner, void (*closing)(void));
void kitty_cfgbox_apply_fixed_size(void);
void kitty_cfgbox_apply_size(void);
void kitty_cfgbox_relayout_panel(const char *path);
void kitty_dlg_mark_quickconnect(struct dlgparam *dp, int on);
extern int kitty_category_expand_depth;

/* ---- exported from windows/kitty_msgbox_stub.c ---- */
int kitty_message_box(HWND owner, const char *text, const char *caption, unsigned type);

/* ---- exported from windows/kitty_pace.c ---- */
unsigned long kitty_pace_cooldown_ms(double now, double paint_ms);
int kitty_pace_effective_ms(void);
void kitty_pace_set_hidden(bool hidden);
void kitty_pace_set_setting(const char *value);
bool kitty_pace_signal_allowed(double now);
extern double kitty_present_wait_ms;

/* ---- exported from windows/kitty_pace_frame.c ---- */
void kitty_pace_frame_pump(void);
void kitty_pace_set_frame_signal(HANDLE h);
bool kitty_pace_wait_frame(void (*cb)(void *), void *ctx);

/* ---- exported from windows/window.c ---- */
void ResetWindow(int reinit);
void SendStrToTerminal(const char *str, const int len);
void do_eventlog(const char *st);
int kitty_active_seat_workplace_proxied(void);
void kitty_apply_transparency(struct WinGuiSeat *wgs);
void kitty_apply_window_pos(struct WinGuiSeat *wgs);
void kitty_painter_before_layering(HWND term_hwnd);
void kitty_set_active_seat(struct WinGuiSeat *wgs);
void resize(int height, int width);
COLORREF return_colours258(void);
extern HWND kitty_hwnd_parent;


/* ---- globals of the shared GUI files that the KiTTY modules read ---- */
extern Conf *conf;   /* window.c: the active seat's configuration */
extern void (*kitty_ctrl_focus_hook)(struct dlgcontrol *ctrl, struct dlgparam *dp);   /* controls.c */
extern void (*kitty_cfg_box_closing_hook)(void);   /* dialog.c */

#endif /* KITTY_GUI_H */
