/*
 * kitty_config.h - the configuration box's entry points (kitty_config.c). The
 * stock targets link windows/kitty_config_stubs.c instead, whose no-op twins
 * are declared here too, so the two stay in step.
 */

#ifndef KITTY_CONFIG_H
#define KITTY_CONFIG_H
#include "putty.h"
/* dialog.h and win-gui-seat.h have no include guard, so they are not
 * included here; the types are named through their struct tags. */
struct dlgcontrol; struct dlgparam; struct controlbox; struct WinGuiSeat;

/* ---- exported from kitty/kitty_config.c ---- */
void conf_radiobutton_bool_handler(struct dlgcontrol *ctrl, struct dlgparam *dlg, void *data, int event);
bool kitty_bold_caption(const char *text);
void kitty_cfgbox_flush_pending(void);
void kitty_cfgbox_open_loaded(void);
void kitty_cfgbox_open_on_panel(const char *path);
int kitty_cfgbox_size_locked(void);
void kitty_cfgbox_store_size(int w, int h);
const char *kitty_cfgbox_wanted_panel(void);
void kitty_cfgbox_workplace_poll(struct dlgparam *dlg);
void kitty_cfgtree_folds_save(void);
int kitty_cfgtree_get_fold(const char *path);
void kitty_cfgtree_set_fold(const char *path, int expanded, int default_expanded);
void kitty_config_end_folder_rename(struct dlgparam *dp);
void kitty_config_footer_pin(const char *path);
struct dlgcontrol *kitty_config_panel_fill_ctrl(const char *path);
void kitty_config_panel_placed(const char *path);
void kitty_config_panel_shown(const char *path, bool show);
void kitty_config_pin_bottoms(void);
bool kitty_config_select_root_folder(struct dlgparam *dp);
void kitty_config_session_distribute(void);
struct dlgcontrol *kitty_config_session_filter_ctrl(void);
bool kitty_red_caption(const char *text);

/* ---- exported from windows/kitty_config_stubs.c ---- */
int GetConfigBoxHeight(void);
int GetConfigBoxWindowHeight(void);
int GetConfigBoxWindowWidth(void);
int GetModalChangedHostKeyConfirmationFlag(void);
int GetModalNewHostKeyConfirmationFlag(void);
int GetModalWeakKeyConfirmationFlag(void);
int kitty_confirm_box(HWND owner, const char *caption, const char *text, const char *warn_red);
int kitty_confirm_box_yes(HWND owner, const char *caption, const char *text, const char *warn_red);
void kitty_demo_templates(void);
void kitty_info_box(HWND owner, const char *caption, const char *text, const char *warn_red);
int kitty_message_box(HWND owner, const char *text, const char *caption, unsigned type);
int kitty_proxy_choice_shown(void);
bool kitty_proxy_panel_dirty(void);

#endif /* KITTY_CONFIG_H */
