/*
 * kitty_secretmask.h - secrets masked out of a command line before it is
 * shown or logged: the failure report of a program start, the debug trace of
 * a hand-off. No Win32 and no PuTTY types, so
 * test/test_secretmask.c covers it without a window.
 */
#ifndef KITTY_SECRETMASK_H
#define KITTY_SECRETMASK_H

#include <stddef.h>

/* Every byte of line[at .. at+len) becomes '*' (clipped at the line's end). */
void kitty_mask_span(char *line, size_t at, size_t len);

/* Every occurrence of secret in line, in each form a hand-off writes it,
 * becomes '*' of the same length: as it is, Win32-quoted (a '"' as \" and
 * the backslashes before it doubled, as kitty_xfer.c qcat), with '"' doubled
 * (rawcat), and percent-encoded outside the RFC 3986 unreserved set, in
 * upper and lower case hex (urlcat). An empty or NULL secret masks nothing. */
void kitty_mask_secret(char *line, const char *secret);

/* kitty/kitty_xfer.c, the session side (Win32 + the live Conf):
 * kitty_mask_line returns a copy of line with every secret of this session
 * masked - Password, ProxyPassword and each secret noted below - for a
 * report or a log line; the caller frees it with sfree after smemclr.
 * kitty_secret_note keeps a secret a hand-off put on a command line that is
 * not in the Conf (a proxy list entry's password), held wrapped. */
char *kitty_mask_line(const char *line);
void kitty_secret_note(const char *plain);

#endif /* KITTY_SECRETMASK_H */
