/*
 * kitty_store_acl.h - owner-only access rights for what KiTTY++ creates in
 * a folder store (kitty_store_acl.c).
 *
 * A store folder (Sessions, SshHostKeys, SshHostCAs, Security, Proxies,
 * Launcher, Backups, TermEnvHint, WindowPos, a sessions= or sshhostkeys=
 * folder) and a file of the store root (KiTTYState, Jumplist, PUTTY.RND, a
 * generated kitty.ini) get, when KiTTY++ creates them, a protected access
 * list: the user, SYSTEM and the local Administrators, full control,
 * inherited by what a folder holds. Set at creation, never afterwards: what
 * already exists keeps its rights. Below %USERPROFILE% nothing is set
 * (Windows already allows only the user there). A network share is judged
 * as a local drive: one that keeps access lists (a Windows server's NTFS)
 * gets the rights. On a volume or share without access lists (FAT, exFAT,
 * a NAS without them) they cannot be set: Event Log record 904 and one
 * notice per store; a failure to set them is reported the same way.
 */
#ifndef KITTY_STORE_ACL_H
#define KITTY_STORE_ACL_H

#include <windows.h>

/* The verdict for one path about to be created */
enum {
    KSA_SET,        /* created with the owner-only rights */
    KSA_LEAVE,      /* already there, or not resolvable: as Windows makes it */
    KSA_PROFILE,    /* below %USERPROFILE%: as Windows makes it */
    KSA_SHARE,      /* a network share without access lists: not set, reported */
    KSA_NOACLS,     /* a volume without access lists: not set, reported */
    KSA_FAILED,     /* setting them failed: created without, reported */
    KSA_NVERDICTS
};

/* The decision, pure: the path exists, is below the profile, is on a
 * network share, its volume keeps access lists. */
int kitty_store_acl_verdict(int exists, int in_profile, int remote,
                            int keeps_acls);
/* 1 for a verdict that is reported (share, no access lists, failed) */
int kitty_store_acl_reported(int verdict);
/* 1 when `path` is `dir` or below it (both full paths, any case, a whole
 * component: C:\Users\hk is not above C:\Users\hkx) */
int kitty_store_acl_path_under(const char *path, const char *dir);
/* 1 for a UNC path (\\server\share, \\?\UNC\server\share) */
int kitty_store_acl_is_unc(const char *path);
/* The notice is due: the store's marker is not set and this process has
 * not shown it yet. */
int kitty_store_acl_notice_due(int marker_set, int shown);

/* CreateDirectoryA(dir, NULL) with the rights as decided above; the same
 * result and GetLastError(). */
BOOL kitty_store_acl_mkdir(const char *dir);
/* A store-root file about to be written: when it is not there and the
 * rights apply, it is created empty with them, so the write that follows
 * keeps them (an in-place write and kitty_write_file_whole both keep the
 * rights of a file that exists). 1 created; 0 not (nothing to do, or the
 * write creates it as before). */
int kitty_store_acl_new_file(const char *path);

/* The access list itself (folder: inherited by the contents). 1 with *sa
 * filled; 0 with Windows' reason in GetLastError(). */
int kitty_store_acl_attrs(int folder, SECURITY_ATTRIBUTES *sa);

/* Where the notice goes (kitty.exe, kitty_params.c): msg filled in; returns
 * 1 when it was shown, and only then the store's marker is written. NULL =
 * no notice (the console tools): the Event Log record only. */
void kitty_set_store_acl_report(int (*fn)(const char *msg));
/* The report of one verdict for `path` (as kitty_store_acl_mkdir and
 * kitty_store_acl_new_file make it; err = Windows' reason for KSA_FAILED):
 * the Event Log record once per process and verdict, always written, and
 * the notice once per store - none under readonly=yes, where the store's
 * marker cannot be written. */
void kitty_store_acl_report(int verdict, const char *path, DWORD err);
/* The process's memory of records and notices cleared (the unit test) */
void kitty_store_acl_forget(void);

/* KiTTYState: the notice was shown for this store */
#define KITTY_STORE_ACL_MARKER "StoreAclNotice"

#endif /* KITTY_STORE_ACL_H */
