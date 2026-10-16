/*
 * kitty_searchbar.h - the find bar of the scrollback search
 * (kitty/kitty_searchbar.c; the terminal's half: terminal/kitty_search.c).
 */
#ifndef KITTY_SEARCHBAR_H
#define KITTY_SEARCHBAR_H

#include <windows.h>

/* Ctrl+Shift+F / Find...: open the bar of this terminal window, or give an
 * open one the focus back with its text selected. */
void kitty_searchbar_open(HWND owner);
/* The terminal window moved or changed size: the bar keeps its corner. */
void kitty_searchbar_follow(HWND owner);
bool kitty_searchbar_is_open(HWND owner);

/* window.c: the terminal behind a terminal window, or NULL. */
struct terminal_tag;
struct terminal_tag *kitty_searchbar_term(HWND owner);
/* The stored session of that window, where the bar's place is kept; "" for
 * none (window.c). */
const char *kitty_searchbar_session(HWND owner);

#endif
