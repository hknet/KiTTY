/*
 * kitty_droppath.h - the text typed into the terminal after a drag-and-drop
 * kscp upload: the remote path each dropped file landed at, quoted for a
 * POSIX shell and joined with spaces. No Win32 and no PuTTY types, so
 * test/test_droppath.c covers it without a window.
 */
#ifndef KITTY_DROPPATH_H
#define KITTY_DROPPATH_H

/* The remote path an upload of `localname` into `remotedir` produced, as
 * kscp resolves it. remotedir "." or empty is the home directory, giving
 * "~/<name>"; an absolute dir or one starting with "~" gives "<dir>/<name>";
 * any other dir is relative to the home directory, giving "~/<dir>/<name>".
 * name is the last component of localname (after '\' or '/'; trailing
 * separators ignored, so a folder names itself). NULL when localname has no
 * name (a drive root) or on no memory. The caller frees it with free(). */
char *kitty_droppath_remote(const char *remotedir, const char *localname);

/* path as one POSIX shell word: as it is when every character is safe,
 * else in single quotes with ' written as '\''. A leading "~/" or
 * "~user/" stays outside the quotes, so the shell still expands it. The
 * caller frees it with free(). */
char *kitty_droppath_quote(const char *path);

/* The n paths quoted and joined with single spaces, no newline at the end;
 * NULL entries are skipped. bracketed wraps it in ESC [200~ ... ESC [201~.
 * NULL when there is nothing to type. The caller frees it with free(). */
char *kitty_droppath_text(const char *const *paths, int n, int bracketed);

#endif /* KITTY_DROPPATH_H */
