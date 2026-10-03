/*
 * kitty_sessorg.h - the "Organize sessions" window, Arrange and the box that
 * deletes a folder holding sessions (hknet/KiTTY#55). kitty_sessorg.c.
 *
 * Every window here is modeless and owned by the configuration window: it
 * never holds that window up, and an action asked about continues from the
 * box's answer. The moves themselves are kitty_sessionrekey.h's re-key.
 */
#ifndef KITTY_SESSORG_H
#define KITTY_SESSORG_H

#include <windows.h>

/* Open the Organize sessions window (or bring it to the front). */
void kitty_sessorg_open(HWND owner);

/* Arrange (sessions filed by their Folder value move to their folder path).
 * first_start: the one-time offer - shown only when there is something to
 * arrange, recorded as made either way, and a "Later" is answered with where
 * Arrange lives. Otherwise the on-demand form (Organize > Arrange...). */
void kitty_sessorg_arrange(HWND owner, int first_start);
/* Is the one-time offer still to be checked in this store? */
int kitty_sessorg_arrange_pending(void);

/* Delete `folder`, which holds sessions: the box asks where they go (the
 * root preselected) and moves them - subfolders keep their shape below the
 * destination - with everything that hangs off their names. done(deleted,
 * ctx) runs once: 1 when the folder is gone. */
void kitty_sessorg_delete_folder(HWND owner, const char *folder,
                                 void (*done)(int deleted, void *ctx),
                                 void *ctx);

/* Move the folder `from`, with everything below it, to `to` (a rename, or
 * the delete of `from` into an existing folder): path sessions re-keyed,
 * bare sessions filed there get their Folder value moved, the stored folder
 * list follows. All or nothing. Returns 1 done; 0 with *msg (snewn'd) saying
 * why: `clash_fmt` (a format taking the destination and the list, the
 * caller's wording) for names already taken, or the failure. *rewritten
 * (snewn'd or NULL) names the sessions whose jump host was rewritten. */
int kitty_sessorg_folder_move(const char *from, const char *to,
                              const char *clash_fmt, char **msg,
                              char **rewritten);

/* kitty_config_session.c: the open configuration box follows a change made
 * here - the store (folder and session lists, launcher), and a folder that
 * moved (`to`) or was deleted (its sessions went to `to`; NULL = it was
 * empty): the level shown and the loaded session follow it. */
void kitty_config_session_store_changed(void);
void kitty_config_session_folder_moved(const char *from, const char *to,
                                       int deleted);

#endif
