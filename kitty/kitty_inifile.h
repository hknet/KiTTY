/*
 * kitty_inifile.h: interface of the kitty.ini resolver for
 * satellite binaries (see kitty_inifile.c for the search order and the
 * authoritative-store rule).
 */

#ifndef KITTY_INIFILE_H
#define KITTY_INIFILE_H

const char *kitty_inifile_file(void);
char *kitty_inifile_find_dup(int skip_appdata);   /* NULL = none; free() it */
void kitty_inifile_pin(const char *path);          /* kitty.exe only */
void kitty_inifile_pin_authority(int registry);    /* kitty.exe only */
int kitty_inifile_portable(void);   /* portable storage layout beside the exe? */
int kitty_inifile_registry_authoritative(void);
/* the registry key the settings live under: PuTTY's with [KiTTY]
 * KiClassName=PuTTY, KiTTY++'s own otherwise */
const char *kitty_inifile_registry_base(void);
char *kitty_inifile_state_file_dup(void);  /* <store root>\KiTTYState; NULL = none; free() it */
int kitty_inifile_read(const char *section, const char *key,
                        char *value, int len);
/* klink/kscp/ksftp/kittygen: a global [KiTTY] key where kitty.exe keeps it
 * (the registry when authoritative, else kitty.ini); "" = set nowhere.
 * kitty_winevent_cfg.c. */
void kitty_console_global_read(const char *key, char *value, int len);
char *kitty_inifile_read_dup(const char *section, const char *key); /* NULL = absent or empty; free() it */
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
/* [KiTTY] conf=no (upstream KiTTY's "no files" switch): a policy key's,
 * kitty.exe's (kitty_inifile_noconf_force, the hive or kitty.ini), else
 * kitty.ini's own. */
int kitty_inifile_noconf(void);
/* conf=no holds the kitty.ini in use back: KiTTY++ neither creates nor
 * writes it. Not in the folder store, where kitty.ini IS the store and is
 * written as ever (readonly=yes stops that). */
int kitty_inifile_noconf_blocks(void);
/* kitty.exe: conf=no found by its ReadParameter. Only switched on. */
void kitty_inifile_noconf_force(int on);
/* May the kitty.ini in use be written? 0 yes (created when missing),
 * KITTY_INI_REFUSED_RO (readonly=yes), KITTY_INI_REFUSED_NOCONF (conf=no),
 * KITTY_INI_REFUSED_NOPATH (no kitty.ini path at all). */
enum { KITTY_INI_REFUSED_RO = 1, KITTY_INI_REFUSED_NOCONF, KITTY_INI_REFUSED_NOPATH };
int kitty_inifile_write_refusal(void);
/* 1 when readonly=yes comes from a policy key (kitty_globalsettings.h). */
int kitty_store_ro_by_policy(void);
/* The reason a refusal under readonly=yes gives (KT_STORE_RO_NOT_SAVED): a
 * policy key's, else the resolved kitty.ini's, else "kitty.ini". Valid until
 * the next call. */
const char *kitty_store_ro_reason(void);
/* 1 when path is the resolved kitty.ini and that sets readonly=yes: a
 * writer naming its own file (writeINI, delINI) skips the write. */
int kitty_inifile_write_blocked(const char *path);

/* The start-up keys of the resolved kitty.ini, read in one go: which store
 * is in charge and, for the folder store, where its sessions and host keys
 * are. Read fresh at every call (a running kageant sees an edited kitty.ini).
 * The console tools set their store from it (kitty_console_store.c), kageant
 * its session menu. */
/* Each string in full, never NULL after kitty_inifile_startup() ("" when
 * not set); kitty_inifile_startup_free() releases them. kitty_inifile_startup
 * does not free what the record held before: free a filled record first. */
struct kitty_ini_startup {
    int registry_authoritative;      /* kitty_inifile_registry_authoritative() */
    int folder_store;                /* the sessions are in the folder store */
    /* the rest is filled only for the folder store */
    char *session_dir;
    char *session_suffix;            /* empty = none */
    char *hostkey_dir;               /* empty = the store's own SshHostKeys */
    char *hostkey_suffix;
    char *portable_password_protection;   /* empty = not set */
};
void kitty_inifile_startup(struct kitty_ini_startup *s);
void kitty_inifile_startup_free(struct kitty_ini_startup *s);

#endif
