/*
 * kitty_zone.h - what Windows' security zone policy allows for a file this
 * program is asked to open: the zone from the file's path and its
 * Mark-of-the-Web, the policy from Internet Options or Group Policy.
 */
#ifndef KITTY_ZONE_H
#define KITTY_ZONE_H

#define KITTY_ZONE_ALLOW   0   /* this PC, the intranet, trusted sites (by default) */
#define KITTY_ZONE_ASK     1   /* the Internet zone (by default), or the policy unknown */
#define KITTY_ZONE_REFUSE  2   /* Restricted sites (by default) */

/* The policy of "launch applications and unsafe files"
 * (URLACTION_SHELL_EXECUTE_HIGHRISK) for the file at path, read through
 * IInternetSecurityManager::ProcessUrlAction with the file's saved zone
 * (Mark-of-the-Web) always evaluated. A failure of the call is
 * KITTY_ZONE_ASK: the safe reading. */
int kitty_zone_file_policy(const char *path);

/* Whether Group Policy hides Windows' Unblock (Attachment Manager, "Hide
 * mechanisms to remove zone information"): then only an administrator
 * can lift the zone of a file. */
int kitty_zone_unblock_hidden(void);

#endif /* KITTY_ZONE_H */
