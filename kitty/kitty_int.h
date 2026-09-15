/*
 * kitty_int.h - what kitty.c and the files split off it share and nothing
 * else needs. kitty.h is the public face of the module; this header carries
 * the file-scope state the split files still reach into directly.
 */
#ifndef KITTY_INT_H
#define KITTY_INT_H

#include <windows.h>

/* The terminal window of this process (kitty.c); GetMainHwnd() is the
 * accessor the rest of the suite uses. */
extern HWND MainHwnd ;

#endif /* KITTY_INT_H */
