/*
 * kitty_inifile.h: interface of the kitty.ini resolver for
 * satellite binaries (see kitty_inifile.c for the search order and the
 * authoritative-store rule).
 */

#ifndef KITTY_INIFILE_H
#define KITTY_INIFILE_H

const char *kitty_inifile_file(void);
int kitty_inifile_find(int skip_appdata, char *out, int outlen);
void kitty_inifile_pin(const char *path);          /* kitty.exe only */
void kitty_inifile_pin_authority(int registry);    /* kitty.exe only */
int kitty_inifile_portable(void);   /* portable storage layout beside the exe? */
int kitty_inifile_registry_authoritative(void);
int kitty_inifile_state_file(char *out, int outlen);  /* <store root>\KiTTYState */
int kitty_inifile_read(const char *section, const char *key,
                        char *value, int len);
int kitty_inifile_write(const char *section, const char *key,
                         const char *value);   /* readonly=yes: skipped, 1 */
int kitty_inifile_readonly(void);   /* [KiTTY] readonly=yes in the resolved ini */
void kitty_inifile_readonly_reread(void);  /* drop the cached answer */
void kitty_inifile_readonly_force(int on); /* kitty.exe: readonly from the hive */

/* readonly=yes makes the whole settings store read-only (kitty_inifile.c).
 * A write primitive checks kitty_store_readonly() (or, for a registry key,
 * kitty_store_ro_regpath) and does nothing when it answers 1; where it
 * returns a status, KITTY_RO_REFUSED - non-zero, so a caller testing for
 * failure stays silent, and distinguishable from every Windows outcome. */
#define KITTY_RO_REFUSED (-2)
int kitty_store_readonly(void);
void kitty_store_ro_allow(int on);         /* savemode=file: kitty.sav into the hive */
void kitty_store_ro_set_regbase(const char *base);
/* hkcu: the root is HKEY_CURRENT_USER (no windows.h here: putty.h wants
 * winsock2.h first, and this header is included before it) */
int kitty_store_ro_regpath(int hkcu, const char *subkey);
/* The kitty.ini a refusal names (KT_STORE_RO_NOT_SAVED): the resolved
 * file, else "kitty.ini". */
const char *kitty_store_ro_inifile(void);
/* 1 when path is the resolved kitty.ini and that sets readonly=yes: a
 * writer naming its own file (writeINI, delINI) skips the write. */
int kitty_inifile_write_blocked(const char *path);

/* The start-up keys of the resolved kitty.ini, read in one go: which store
 * is in charge and, for the folder store, where its sessions and host keys
 * are. Read fresh at every call (a running kageant sees an edited kitty.ini).
 * The console tools set their store from it (kitty_console_store.c), kageant
 * its session menu. */
#define KITTY_INIFILE_PATHLEN (4 * 260 + 8)
struct kitty_ini_startup {
    int registry_authoritative;      /* kitty_inifile_registry_authoritative() */
    int folder_store;                /* the sessions are in the folder store */
    /* the rest is filled only for the folder store */
    char session_dir[KITTY_INIFILE_PATHLEN];
    char session_suffix[80];         /* empty = none */
    char hostkey_dir[KITTY_INIFILE_PATHLEN];  /* empty = the store's own SshHostKeys */
    char hostkey_suffix[80];
    char portable_password_protection[32];    /* empty = not set */
};
void kitty_inifile_startup(struct kitty_ini_startup *s);

#endif
