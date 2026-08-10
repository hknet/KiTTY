/*
 * kitty_pageant.h: interface between windows/pageant.c (upstream PuTTY's
 * Windows Pageant, kept textually close to upstream) and kitty_pageant.c
 * (the fork's kageant additions: Windows OpenSSH client integration,
 * load-keys-on-startup, key-use notify/confirm prompts, persistent key
 * offer order).
 *
 * Windows-only (HWND/FILE/Filename in prototypes); include after putty.h.
 */
#ifndef KITTY_PAGEANT_H
#define KITTY_PAGEANT_H

/* HKCU ...\Run autostart value name (also shown in the tray info box). */
#define KAGEANT_RUN_NAME    "KiTTY-kageant"

/* ---- registry-backed tray toggles (HKCU, consolidated KiTTY hive) ---- */
int kageant_openssh_get(void);
void kageant_openssh_set(int on);
int kageant_startup_get(void);
void kageant_startup_set(int on);
int kageant_notify_get(void);          /* default on */
void kageant_notify_set(int on);
int kageant_confirm_get(void);         /* default off */
void kageant_confirm_set(int on);
/* [Agent] askconfirmation modes (classic three-state) */
#define KAGEANT_CONFIRM_NO    0        /* never, even per-key comment opt-ins */
#define KAGEANT_CONFIRM_YES   1        /* prompt for every key use */
#define KAGEANT_CONFIRM_AUTO  2        /* only keys whose comment asks for it */
int kageant_confirm_mode(void);
void kageant_confirm_set_mode(int mode);   /* key-list radios: yes/auto/no */
const char *kageant_ini_status(void);  /* ini path when authoritative, else NULL */
int kageant_ini_present(void);         /* a kitty.ini exists to read/write at all
                                        * (any mode) - gates the ini-only options */
/* string settings following the same store precedence as the toggles
 * (key-list window geometry + column widths live here) */
int kageant_setting_str_get(const char *inikey, const char *regname,
                            char *buf, size_t len);
void kageant_setting_str_set(const char *inikey, const char *regname,
                             const char *value);
int kitty_inilight_registry_authoritative(void);  /* kitty_inilight.c */
int kitty_inilight_read(const char *section, const char *key,
                        char *value, int len);        /* kitty_inilight.c */

/* ---- Windows OpenSSH client integration ---- */
void kageant_openssh_apply(int on);    /* add/remove the managed ~/.ssh block */
char *kageant_ssh_path(const char *leaf);  /* malloc'd %USERPROFILE%\.ssh\<leaf>, or NULL */
void kageant_write_identityagent(FILE *fp, const char *pipename);

/* ---- load-keys-on-startup + persistent key offer order ---- */
void kageant_track_keypath(const char *path, int encrypted);
int  kageant_startup_loading(void);                 /* a startup load is running */
/* Passphrase cache: 0 = do not cache; capped at KAGEANT_TTL_MAX (5 min - the
 * cache only spans a batch add, so it need not live longer). */
#define KAGEANT_TTL_MAX 300
int  kageant_passphrase_ttl(void);                  /* seconds; 0 = no backstop */
int  kageant_quiet_missing(void);                   /* [Agent] quietmissingkeys */
int  kageant_retry_keys(void);                      /* [Agent] retrykeys */
int  kageant_unload_on_remove(void);                /* [Agent] unloadonremove */
/* Setters for the four ini-only [Agent] options above. They write to the
 * suite kitty.ini (their only store); return 0 if no ini is writable
 * (registry-authoritative install), in which case the Settings dialog greys
 * them. */
int  kageant_quiet_missing_set(int on);
int  kageant_retry_keys_set(int on);
int  kageant_unload_on_remove_set(int on);
int  kageant_passphrase_ttl_set(int seconds);
void kageant_note_pending(const char *path, int encrypted, int slot);
void kageant_forget_loaded_by_blob(ptrlen blob);   /* removed in View Keys */
char *kageant_paths_of_blob(ptrlen blob);  /* every file it came from; free it */
char *kageant_file_of_blob(ptrlen blob);   /* first tracked path, for re-loading;
                                            * malloc'd or NULL */
/* pending (not-loaded) startup entries, for the key list window */
int kageant_pending_count(void);
int kageant_pending_get(int i, const char **path, int *encrypted,
                        const char **fp, int *failed);
void kageant_drop_pending(const char *path);   /* memory + stored list */
/* per-key load mode: 1 = deferred (,encrypted), 0 = decrypt at load
 * (,plain), -1 = key not tracked */
int kageant_startup_mode_get(const char *path);
void kageant_startup_mode_set(const char *path, int encrypted);
void kageant_retry_pending_keys(void);              /* on device arrival */
void kageant_media_gone(void);                      /* on device removal */
void kageant_forget_startup_key(const char *path);  /* drop one stored entry */
void kageant_notify_startup_missing(void);
void kageant_save_startup_keys(void);
void kageant_load_startup_keys(void);
void kageant_set_run_entry(int on);    /* HKCU ...\Run autostart entry */
void kageant_set_autostart(int on);    /* Startup shortcut (portable) or Run key */
int kageant_autostart_active(void);    /* is our autostart artifact actually present? */
int kageant_autostart_conflict(char *desc, size_t len);  /* another agent registered? */
void kageant_save_key_order(void);
void kageant_apply_saved_order(void);
int kageant_nloaded(void);             /* key paths tracked this session */

/* ---- key-use confirm/notify (installed as agent-core hook pointers) ---- */
int kageant_do_confirm(const char *comment);
void kageant_do_notify(const char *comment);
/* the hook pointers themselves live in the agent core (../pageant.c) */
extern int (*kageant_confirm_hook)(const char *comment);
extern void (*kageant_notify_hook)(const char *comment);

/* ---- provided by windows/pageant.c for kitty_pageant.c ---- */
HWND kageant_traywindow(void);         /* tray window, for balloon popups */
void win_add_keyfile(Filename *filename, bool encrypted);

#endif /* KITTY_PAGEANT_H */
