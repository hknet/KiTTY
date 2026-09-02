/*
 * kitty_storemove.h - whole-store moves between the registry and a folder
 * (Application > Migration > KiTTY storage). See kitty_storemove.c.
 */
#ifndef KITTY_STOREMOVE_H
#define KITTY_STOREMOVE_H

#include <windows.h>

/* What one move did, for the summary. */
struct ksm_result {
    int sessions, proxies, hostkeys, settings, programs, skipped, fail;
};

/* No-UI cores, shared by the config-box buttons and the -portablecopy /
 * -takefolder command-line switches. Both fill *r and put a summary for the
 * user into msg; the return value is the failure count.
 *
 *   kitty_portable_copy_core   pw = the copy's master password (dpapi == 0),
 *                              or NULL with dpapi == 1 for "this PC only".
 *   kitty_take_folder_core     pw = the folder store's master password when
 *                              it has one, else NULL; overwrite = 1 replaces
 *                              entries of the same name, 0 keeps them. */
int kitty_portable_copy_core(const char *dir, const char *pw, int dpapi,
                             struct ksm_result *r, char *msg, size_t msglen);
int kitty_take_folder_core(const char *dir, const char *pw, int overwrite,
                           struct ksm_result *r, char *msg, size_t msglen);

/* Does the folder store need a master password to be taken in? */
int kitty_take_folder_needs_password(const char *dir);

/* The interactive versions behind the two buttons: folder picker, password
 * dialog, questions, summary box. */
void kitty_make_portable_copy(HWND hwnd);
void kitty_take_folder_store(HWND hwnd);

#endif
