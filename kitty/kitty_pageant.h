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

/* Message the notice window posts to the tray window when a kageant notice is
 * clicked; the tray handler opens View Keys. kageant already uses WM_APP +5
 * (WM_NETEVENT), +6 (WM_SYSTRAY), +7 (WM_SYSTRAY2) and +8 (WM_DONE_WITH_SOCKET),
 * so +9. This header is included after putty.h (see the file banner), so
 * windows.h - and WM_APP - are in scope. */
#define KAGEANT_WM_NOTICE_CLICK (WM_APP + 9)

/* ---- registry-backed tray toggles (HKCU, consolidated KiTTY hive) ---- */
int kageant_openssh_get(void);
void kageant_openssh_set(int on);
int kageant_startup_get(void);
void kageant_noload_set(void);    /* -noload: clean-slate run */
int  kageant_noload(void);
/* The STORED setting (get) versus whether the mechanism runs THIS RUN
 * (active). Display and edit use the getter; behaviour uses active. */
int  kageant_startup_active(void);
void kageant_startup_set(int on);
int kageant_notify_get(void);          /* default on */
int kageant_notice_seconds(int fallback);  /* [Agent] noticetimeout */
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
/*
 * The defaults for every NUMBER the settings dialog can hold. They are here
 * rather than written out again beside each getter because the dialog needs
 * them too: an empty box stores the default, so the value has to be nameable
 * from outside this module. Two copies of "60" that mean the same thing is
 * how one of them ends up changed alone.
 */
#define KAGEANT_TTL_DEFAULT          60
#define KAGEANT_NOTICESECS_DEFAULT   0     /* 0 = each notice's own timing */
#define KAGEANT_AGENTLOG_KB_DEFAULT  5120
#define KAGEANT_AGENTLOG_KEEP_DEFAULT 3
#define KAGEANT_AGENTLOG_DAYS_DEFAULT 90
int  kageant_passphrase_ttl(void);                  /* seconds; 0 = no backstop */
int  kageant_quiet_missing(void);                   /* [Agent] quietmissingkeys */
/* [Agent] retrykeys is THREE-valued: 0 = never retry, 1 = retry from the
 * stored drive+path, 2 = also try the stored path (minus its drive) on a
 * drive that just arrived. Ini spellings: no / yes / ignoredriveletter. */
#define KAGEANT_RETRY_ANYDRIVE 2
int  kageant_retry_keys(void);                      /* [Agent] retrykeys */
int  kageant_unload_on_remove(void);                /* [Agent] unloadonremove */
/* Setters for the four ini-only [Agent] options above. They write to the
 * suite kitty.ini (their only store); return 0 if no ini is writable
 * (registry-authoritative install), in which case the Settings dialog greys
 * them. */
int  kageant_quiet_missing_set(int on);
/* [Agent] helloconfirm: gate every confirmation behind a Windows Hello
 * presence check (a per-key mode 2 does the same for one key). Boolean. */
int  kageant_hello_get(void);
int  kageant_hello_set(int on);
/* the audit log: [Agent] auditlog on/off (default on) + rotation numbers;
 * kageant_audit_setup() resolves the path and (re)configures the sink -
 * call at startup and after a settings change. */
int  kageant_audit_get(void);
int  kageant_audit_set(int on);
void kageant_audit_setup(void);
/* the numeric knobs and the path override, for the settings dialog */
int  kageant_audit_maxkb_get(void);
int  kageant_audit_keep_get(void);
int  kageant_audit_expire_get(void);
int  kageant_audit_pathsetting_get(char *buf, size_t len);
/* Where the log goes when no path is configured - so the settings dialog can
 * say so instead of leaving an empty box. `create` makes the directory; the
 * dialog passes 0, because opening a dialog must not create directories. */
int  kageant_audit_default_path(char *buf, size_t len, int create);
/* write the whole set through to both stores and re-arm the sink;
 * path NULL/"" clears the override back to the default location */
void kageant_audit_cfg_set(const char *path, int maxkb, int keep,
                           int expiredays);
int  kageant_retry_keys_set(int mode);              /* 0/1/KAGEANT_RETRY_ANYDRIVE */
int  kageant_unload_on_remove_set(int on);
int  kageant_passphrase_ttl_set(int seconds);
int  kageant_hello_ttl(void);      /* Hello KEK cache seconds; default 60 */
int  kageant_hello_ttl_set(int seconds);

/* Re-encrypt keys after idle (design/TASK_kageant_autoreencrypt.md).
 * Mode: 0 off (a key's own value still applies), 1 default for keys without
 * their own value, 2 enforced for every key. Seconds: KAGEANT_AUTOENC_USE
 * (1) = right after each use, else 30 s .. 7 d. A per-key value of -1 means
 * "agent default", 0 = off for this key. */
#define KAGEANT_AUTOENC_USE 1
#define KAGEANT_AUTOENC_MIN 30
#define KAGEANT_AUTOENC_MAX (7 * 86400)
int  kageant_autoenc_mode(void);
int  kageant_autoenc_mode_set(int mode);
int  kageant_autoenc_seconds(void);
int  kageant_autoenc_seconds_set(int seconds);
/* "10m", "2 h", "1d", "use", "off", bare seconds -> value (-1 = no parse). */
int  kageant_autoenc_parse(const char *text);
/* value -> "use" / "off" / "10 m" / "2 h" / "45 s" (buf >= 24). */
void kageant_autoenc_format(int seconds, char *buf, size_t len);
void kageant_idle_note_use(ptrlen pubblob);            /* a signature happened */
int  kageant_idle_get_key(ptrlen pubblob);             /* own value or -1 */
void kageant_idle_set_key(ptrlen pubblob, int value);  /* -1 = agent default */
int  kageant_idle_effective(ptrlen pubblob);           /* resolved seconds, 0 = none */
int  kageant_idle_tick(void);                          /* re-encrypt due keys; count */
void kageant_idle_install(void);                       /* hook the agent core */
/* The colour theme is application-wide, not the agent's own: kitty_theme_pref.h
 * declares it, and kittygen and kitty read the same setting. */
/* Which page of the settings dialog was showing when it was last closed, so
 * it reopens where it was left. Transient WINDOW STATE, not a configuration
 * option: it lives in the registry only and deliberately has no kitty.ini
 * spelling, the same rule the window geometry follows. */
int  kageant_settings_tab_get(void);
void kageant_settings_tab_set(int page);
void kageant_note_pending(const char *path, int encrypted, int slot);
void kageant_forget_loaded_by_blob(ptrlen blob);   /* removed in View Keys */
char *kageant_paths_of_blob(ptrlen blob);  /* every file it came from; free it */
char *kageant_file_of_blob(ptrlen blob);   /* first tracked path, for re-loading;
                                            * malloc'd or NULL */
/* KiTTY: Hello-protected keys - the startup list's side (kitty_pageant.c) */
int kageant_startup_replace_path(const char *oldpath, const char *newpath,
                                 int encrypted);   /* -1 = keep the mode */
int kageant_startup_forget_path(const char *path);
int kageant_keypath_encrypted(const char *path);   /* 1/0, -1 untracked */
char *kageant_hello_file_of_blob(ptrlen blob);     /* first file WITH a sidecar */
char *kageant_paths_of_blob_annotated(ptrlen blob);/* details' Loaded-from text */
/* pending (not-loaded) startup entries, for the key list window */
int kageant_pending_count(void);
int kageant_pending_get(int i, const char **path, int *encrypted,
                        const char **fp, int *failed);
/* refused because the file at that path is not the key we recorded */
int kageant_pending_mismatch(int i);
int kageant_mismatch_count(void);
/* fingerprint of the file at that path right now, or NULL; free it */
char *kageant_fp_of_file(const char *path);
/* the user accepted a changed key in the key list: load it and adopt the new
 * fingerprint. The ONLY path that ever adopts one - see the comment there. */
int kageant_accept_pending_key(const char *path);
/* the user browsed a not-loaded (absent/unparseable, never mismatch) entry to
 * a file: load it, keep slot + confirm marker, rewrite the path in place.
 * 1 = done, 0 = the file holds a different key (refused), -1 = would not load */
int kageant_locate_pending_key(const char *oldpath, const char *newpath);
/* report a load pass: keys newly refused, keys loaded with nothing to check
 * against, and keys whose path appeared on a new drive but with no recorded
 * fingerprint to admit them by. A notice, never a prompt. */
void kageant_note_verify_problem(int mismatch_new, int unchecked,
                                 int nofp_newdrive);
/* answer a "Retry unavailable keys": one notice whatever happened, "nothing" included */
void kageant_note_retry_result(int loaded, int refused, int absent,
                               int broken, int unchecked);
void kageant_drop_pending(const char *path);   /* memory + stored list */
/* per-key load mode: 1 = deferred (,encrypted), 0 = decrypt at load
 * (,plain), -1 = key not tracked */
int kageant_startup_mode_get(const char *path);
void kageant_startup_mode_set(const char *path, int encrypted);
/* On device arrival. arrived_mask is DEV_BROADCAST_VOLUME's dbcv_unitmask
 * (bit 0 = A:), naming the letter(s) that just appeared; 0 when the arrival
 * did not name a volume, in which case only the stored paths are probed. */
void kageant_retry_pending_keys(unsigned long arrived_mask);
void kageant_retry_pending_keys_now(void);          /* key list, "Retry unavailable keys" */
void kageant_media_gone(void);                      /* on device removal */
/* ssh-add -t key lifetimes: the agent core calls kageant_key_set_lifetime via
 * kageant_key_lifetime_hook when a key is added with a lifetime; a frontend
 * timer calls kageant_expire_due_keys() (returns how many it removed).
 * seconds == 0 clears a pending lifetime (key removed, or re-added without
 * -t); a NULL blob clears them all (remove-all). The _get/_count accessors
 * feed the key list's Lifetime column and the details dialog. */
extern void (*kageant_key_lifetime_hook)(ptrlen pubblob, unsigned seconds);
void kageant_key_set_lifetime(ptrlen pubblob, unsigned seconds);
int  kageant_expire_due_keys(void);
/* The heartbeat runs only while there is something for it to do (a pending
 * lifetime, a key on the idle list, or the idle policy on). The frontend
 * asks kageant_tick_wanted() after every tick and disarms the timer when it
 * says no; it installs kageant_tick_arm_hook so the agent side can ask for
 * the timer back when one of those becomes true again. */
int  kageant_tick_wanted(void);
extern void (*kageant_tick_arm_hook)(void);
int  kageant_key_lifetime_get(ptrlen pubblob, unsigned *set_seconds,
                              unsigned *remaining_seconds);
int  kageant_lifetime_count(void);
void kageant_forget_startup_key(const char *path);  /* drop one stored entry */
void kageant_notify_startup_missing(void);
void kageant_warn_unprotected_memory(void);  /* once: CryptProtectMemory dead */
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
int kageant_do_confirm(const char *comment, int key_confirm);
void kageant_do_mutation_notice(int op, const char *comment);
int  kageant_ipc_blocked(int op);   /* IPC access-control policy */
void kageant_confirm_resume(void);  /* lift the confirm-suppress latch */
int  kageant_confirm_suppressed(void);  /* is the latch engaged? */
int  kageant_lockdown_get(void);
void kageant_lockdown_set(int on);
int  kageant_blockadd_get(void);
void kageant_blockadd_set(int on);
int  kageant_blockremove_get(void);
void kageant_blockremove_set(int on);
int  kageant_notice_timeout_get(void);   /* raw seconds, 0 = default */
void kageant_notice_timeout_set(int seconds);
int kageant_comment_wants_confirm(const char *comment);
void kageant_do_notify(const char *comment, const char *fingerprint);
/* the hook pointers themselves live in the agent core (../pageant.c) */
extern int (*kageant_confirm_hook)(const char *comment, int key_confirm);
extern void (*kageant_random_hook)(void *buf, size_t size);
/* the public blob behind a deferred-decryption prompt (agent core) */
struct PageantClientDialogId;
ptrlen pageant_dlgid_pubblob(struct PageantClientDialogId *dlgid);
extern int (*kageant_comment_confirm_hook)(const char *comment);
extern void (*kageant_notify_hook)(const char *comment,
                                   const char *fingerprint);
/* KiTTY: outcome of a signing request, for the key list's tint. */
extern void (*kageant_keyuse_hook)(const char *fingerprint,
                                   const char *comment, int allowed,
                                   unsigned long req_pid);
/* KiTTY: an external client asked for the identity list. Used to speak up about
 * keys being held back, at the moment their absence costs something. */
extern void (*kageant_identities_asked_hook)(unsigned long pid);
void kageant_do_identities_asked(unsigned long pid);

/* ---- provided by windows/pageant.c for kitty_pageant.c ---- */
HWND kageant_traywindow(void);         /* tray window, for balloon popups */
int  kageant_keylist_open(void);       /* the key list is on screen */
void win_add_keyfile(Filename *filename, bool encrypted);
/* the tray tooltip carries the always-true state, so "why did my login stop
 * working?" has an answer hours later; re-composed when that state changes */
void kageant_refresh_tray_tip(void);


/* KiTTY: the two notice accents, shared with the key list's key-use tint so
 * one palette covers both. Amber warns, blue informs. */
#ifndef KAGEANT_NOTICE_WARN
#define KAGEANT_NOTICE_WARN RGB(190, 110, 0)
#define KAGEANT_NOTICE_INFO RGB(40, 70, 170)
#endif

/* KiTTY: key-use tint. kageant_note_keyuse() is the kageant_keyuse_hook;
 * the key list asks kageant_flash_get() while painting a row and
 * kageant_flash_any() to decide whether it still needs its repaint timer.
 * kageant_keylist_flash_changed() lives in windows/pageant.c. */
int kageant_flash_get(const char *fingerprint, int *allowed);
int kageant_flash_any(void);
void kageant_note_keyuse(const char *fingerprint, const char *comment,
                         int allowed, unsigned long req_pid);
void kageant_keylist_flash_changed(void);

#endif /* KITTY_PAGEANT_H */
