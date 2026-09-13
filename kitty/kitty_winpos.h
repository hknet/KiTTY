/*
 * kitty_winpos.h - where a terminal window is remembered, per session and
 * per monitor layout (see kitty_winpos.c).
 */
#ifndef KITTY_WINPOS_H
#define KITTY_WINPOS_H

#include <stddef.h>

/* One remembered placement: the window's top-left in screen pixels and the
 * terminal grid in columns x rows. cols/rows of 0 mean "position only" (a
 * value written by an earlier version, which stored pixels). */
struct kitty_termpos {
    int left, top;
    int cols, rows;
};

/* Order-independent hash of the current monitor layout. */
unsigned long kitty_winpos_layout_hash(void);

/* "<prefix>_<hash as 8 hex digits>" into buf. */
void kitty_winpos_layout_key(char *buf, size_t n, const char *prefix,
                             unsigned long layout);

/* Text form "left,top,cols,rows" <-> struct. Parse returns 1 on success. */
void kitty_winpos_format(const struct kitty_termpos *pos, char *buf, size_t n);
int kitty_winpos_parse(const char *s, struct kitty_termpos *out);

/* The session's own entry (TermPos_<layout> inside the session). get returns
 * 1 when an entry exists; set returns 1 when it was written. A session that
 * does not exist in the store is never created by set. */
int kitty_winpos_session_get(const char *session, unsigned long layout,
                             struct kitty_termpos *out);
int kitty_winpos_session_set(const char *session, unsigned long layout,
                             const struct kitty_termpos *pos);

/* Copy every TermPos_* entry of one session into another (a Save under a
 * new name), so the copy opens where its original does. Returns the number
 * of entries copied; entries the target already holds are overwritten. */
int kitty_winpos_session_copy(const char *from, const char *to);

/* The SHARED entry (WindowPos\WinPos_<layout> in the registry, or the
 * WindowPos folder of a portable store), written only by windows that have
 * no session of their own. get returns 1 for a current entry, 2 for a
 * position-only entry written by an earlier version, 0 for none. */
int kitty_winpos_shared_get(unsigned long layout, struct kitty_termpos *out);
int kitty_winpos_shared_set(unsigned long layout, const struct kitty_termpos *pos);

/* How many monitor layouts hold a shared entry; remove them all (returns the
 * number removed). The configuration window's own position and every
 * session's entries are not touched. */
int kitty_winpos_shared_count(void);
int kitty_winpos_shared_reset(void);

/* True for a window that writes the SHARED entry rather than a session's
 * own: an unnamed session, or one opened as "Default Settings". */
int kitty_winpos_is_shared_window(const char *sessionname);

#endif
