/*
 * kitty_userpath.h - PATH handling for the program's own directory (kitty_userpath.c).
 */

#ifndef KITTY_USERPATH_H
#define KITTY_USERPATH_H

/* ---- exported from kitty/kitty_userpath.c ---- */
bool kitty_userpath_contains_exe_dir(void);
bool kitty_userpath_set_exe_dir(bool on, char **err);

#endif /* KITTY_USERPATH_H */
