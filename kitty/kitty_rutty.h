/*
 * kitty_rutty.h - the scripting bridge of the terminal (kitty_rutty.c).
 */

#ifndef KITTY_RUTTY_H
#define KITTY_RUTTY_H
#include "putty.h"

/* ---- exported from kitty/kitty_rutty.c ---- */
int kitty_script_active(void);
int kitty_script_enabled(void);
void kitty_script_remote(const void *vdata, size_t len);
int kitty_script_send_file(Conf *conf, Backend *backend, Filename *scriptfile);
void kitty_script_set_enabled(int on);
void kitty_script_stop(void);

#endif /* KITTY_RUTTY_H */
