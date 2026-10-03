/*
 * kitty_sessionrekey.h - move or rename saved sessions, and everything that
 * hangs off their names (hknet/KiTTY#55).
 *
 * A session's identity is its folder path plus its name ("Linux\web\srv01",
 * kitty_sessionpath.h). Moving it to another folder or renaming it is one
 * operation - a RE-KEY from one identity to another - and it must carry along
 * what is keyed by the name:
 *
 *   - the session itself: the registry key (copied with all its values and
 *     subkeys, so the saved password, the window positions and the
 *     per-session commands go with it untouched) or the session file (moved);
 *   - its Folder value, rewritten to the new path's folder ("Default" at the
 *     root), so older copies and the bundle files agree with the path;
 *   - the folder store's per-session commands (Sessions_Commands\<name>);
 *   - the jump list entry, if it had one;
 *   - the launcher's copy of the session list (registry Launcher key or the
 *     folder store's Launcher folder): entries naming it are rewritten;
 *   - the remembered last session;
 *   - every OTHER session that names it as its SSH jump host (ProxyHost with
 *     an SSH proxy type): rewritten to the new identity and reported.
 *
 * No user interface: nothing is asked and no box is shown. Callers (the
 * session list's folder rename/delete, the Organize window, Arrange, the
 * folder-delete box) decide what to tell the user from the results.
 * Callers also own the follow-up: kitty_session_folder_cache_clear(), the
 * session list refresh and kitty_notify_launcher_sessions_changed(), once
 * per batch.
 */
#ifndef KITTY_SESSIONREKEY_H
#define KITTY_SESSIONREKEY_H

#include "putty.h"

#define KITTY_REKEY_OK      0
#define KITTY_REKEY_CLASH   1   /* a target exists: NOTHING was moved */
#define KITTY_REKEY_FAILED  2   /* a move failed part-way; *errmsg says which */

/*
 * Re-key n sessions at once: from[i] -> to[i], in the store in use.
 *
 * Checked before anything moves: every from[i] must exist in this KiTTY's
 * own store, and no to[i] may exist already (other than as from[i] itself,
 * which makes a change of case only a legal rename) or appear twice. On any
 * clash the call returns KITTY_REKEY_CLASH, moves NOTHING, and appends the
 * clashing target identities to `clashes` (one per line). A from[i] that does
 * not exist is reported the same way, under its own name.
 *
 * On success, `rewritten` (may be NULL) receives the identities of the
 * sessions whose jump-host reference was rewritten, one per line, and the
 * return is KITTY_REKEY_OK. A failure part-way (a file or key that cannot be
 * moved) stops at that session, returns KITTY_REKEY_FAILED and sets *errmsg
 * (snewn'd); the sessions before it HAVE been moved and re-keyed, and the
 * jump-host pass still runs for those.
 *
 * Default Settings can be neither source nor target.
 */
int kitty_session_rekey_many(char *const *from, char *const *to, int n,
                             strbuf *rewritten, strbuf *clashes,
                             char **errmsg);

/* The one-session form of the above. */
int kitty_session_rekey(const char *from, const char *to,
                        strbuf *rewritten, strbuf *clashes, char **errmsg);

/* Does this identity exist in this KiTTY's own store (not a fallback hive)? */
bool kitty_session_exists_own(const char *name);

#endif /* KITTY_SESSIONREKEY_H */
