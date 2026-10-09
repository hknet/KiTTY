/*
 * kitty_console_store.h: klink, kscp and ksftp use the store kitty.exe uses
 * (see kitty_console_store.c).
 */

#ifndef KITTY_CONSOLE_STORE_H
#define KITTY_CONSOLE_STORE_H

void kitty_console_store_init(void);
/* the master-password prompt on the console (kitty_console_mpw.c) */
void kitty_console_mpw_init(void);

#endif
