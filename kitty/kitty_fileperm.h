/*
 * kitty_fileperm.h: is a secret file readable by anyone but its user?
 */

#ifndef KITTY_FILEPERM_H
#define KITTY_FILEPERM_H

#include <wchar.h>
#include <stddef.h>

typedef enum {
    KFP_PRIVATE,      /* only the user, Administrators and SYSTEM */
    KFP_READABLE,     /* *who (DOMAIN\name) can read it */
    KFP_OWNER,        /* *who owns it - and an owner can grant itself access */
    KFP_NO_ACL,       /* a file system without permissions (FAT, exFAT) */
    KFP_NULL_DACL,    /* no permission list at all: everyone has full access */
    KFP_MISSING,      /* no such file (the caller's open reports it) */
    KFP_UNKNOWN       /* the permissions could not be read; *who = the error */
} KittyFilePerm;

/* The check Windows OpenSSH makes on a private key: the owner is the current
 * user, Administrators or SYSTEM, and every ALLOW entry granting read access
 * belongs to one of them. */
KittyFilePerm kitty_secret_file_check(const wchar_t *path,
                                      char *who, size_t wholen);

#endif
