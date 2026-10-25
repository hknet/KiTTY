/*
 * kitty_pageant_int.h - what kitty_pageant.c and the files split off it
 * share and nothing else needs: the registry hive kageant keeps its
 * settings in.
 */
#ifndef KITTY_PAGEANT_INT_H
#define KITTY_PAGEANT_INT_H

#define KAGEANT_REG_BASE   "Software\\kapper.net\\KiTTY"  /* consolidated KiTTY hive */


/* Settings helpers that kitty_kageant_audit.c shares with kitty_pageant.c. */
int kageant_reg_write(const char *name, int on);
int kageant_reg_write_dword(const char *name, int val);
/* an [Agent] setting, to the store in charge only; 1 stored, 0 recorded for
 * kageant_setting_failure_box */
int kageant_setting_write(const char *inikey, const char *ini_text,
                          const char *regname, int regval);
int kageant_bool_get(const char *inikey, const char *regname, int def);
int kageant_int_setting(const char *inikey, const char *regname, int def, int lo, int hi);
/* The audit sink, called from the mutating agent paths in kitty_pageant.c. */
void kageant_audit_use(const char *ev, const char *fp, const char *comment,
                       const char *result, const char *reason, unsigned long pid);

int kageant_autoenc_clamp(int v);
const char *kageant_confirm_token(int mode);
/* the three below return memory of their own (sfree it) */
char *kageant_inidir_dup(void);           /* NULL: no authoritative ini */
int kageant_path_under(const char *dir, const char *path);
int kageant_policy_get(const char *inikey, const char *regname);
char *kageant_resolve_form(const char *stored);
char *kageant_store_form(const char *abspath);
/* an [Agent] kitty.ini value whole; NULL when absent or empty (sfree it) */
char *kageant_ini_read_dup(const char *key);

/* KAGEANT registry value names, shared by the engine and the settings facade. */
#define KAGEANT_REG_STARTUP "LoadKeysOnStartup"
#define KAGEANT_REG_KEYS    "StartupKeys"
#define KAGEANT_REG_ORDER   "KeyOrder"   /* SHA256 fingerprints in offer order */
#define KAGEANT_RUN_KEY     "Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define KAGEANT_RUN_NAME    "KiTTY-kageant"
#define KAGEANT_REG_NOTIFY "NotifyOnKeyUse"
#define KAGEANT_REG_CONFIRM "ConfirmKeyUse"
#define KAGEANT_REG_NOTICESECS "NoticeTimeout"  /* notice display seconds */

int kageant_autoenc_mode_read(void);
int kageant_reg_read(const char *name, int *val_out);
#endif /* KITTY_PAGEANT_INT_H */
