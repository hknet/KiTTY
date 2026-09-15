/*
 * kitty_exportbundle.h - the bulk session export and import
 * (kitty_exportbundle.c): the no-UI cores that the command line and the
 * whole-store move call, and the dialogs the configuration box opens.
 */

#ifndef KITTY_EXPORTBUNDLE_H
#define KITTY_EXPORTBUNDLE_H
#include "putty.h"

/* ---- exported from kitty/kitty_exportbundle.c ---- */
int kitty_ask_store_password(HWND hwnd, char **pwOut, int *dpapiOut);
int kitty_bundle_needs_password(const char *dir);
void kitty_export_all_sessions(HWND hwnd);
int kitty_export_all_to_dir(const char *dir, int *failOut);
int kitty_import_dir(const char *dir, int *failOut, int *proxyOut, int *skippedOut, int overwrite);
void kitty_import_sessions(HWND hwnd);
void kitty_show_mpw_moved(HWND hwnd);
int kitty_unlock_import_bundle(HWND hwnd, const char *dir, char **pwOut);

#endif /* KITTY_EXPORTBUNDLE_H */
