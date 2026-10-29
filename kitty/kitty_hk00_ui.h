/*
 * kitty_hk00_ui.h - kitty.exe's side of the host key update
 * (kitty_hk00_ui.c): the question of updatehostkeys=ask and the notice of a
 * key stored. Include after putty.h.
 */
#ifndef KITTY_HK00_UI_H
#define KITTY_HK00_UI_H

/* Install the UI hook (kitty_hostkeys00.h kitty_hk00_set_ui) and the
 * end-of-questions hook of windows/dialog.c. Once, at startup. */
void kitty_hk00_ui_install(void);

/* The host and the key hash (SHA256:...) of the last key the notice was
 * shown for. false when none. For the notice's click (window.c). */
bool kitty_hk00_ui_last_stored(const char **host, const char **hash);

#endif
