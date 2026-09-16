/*
 * kitty_pageant_int.h - what kitty_pageant.c and the files split off it
 * share and nothing else needs: the registry hive kageant keeps its
 * settings in.
 */
#ifndef KITTY_PAGEANT_INT_H
#define KITTY_PAGEANT_INT_H

#define KAGEANT_REG_BASE   "Software\\kapper.net\\KiTTY"  /* consolidated KiTTY hive */


/* Settings helpers that kitty_kageant_audit.c shares with kitty_pageant.c. */
void kageant_reg_write(const char *name, int on);
void kageant_reg_write_dword(const char *name, int val);
int kageant_bool_get(const char *inikey, const char *regname, int def);
int kageant_int_setting(const char *inikey, const char *regname, int def, int lo, int hi);
/* The audit sink, called from the mutating agent paths in kitty_pageant.c. */
void kageant_audit_use(const char *ev, const char *fp, const char *comment,
                       const char *result, const char *reason, unsigned long pid);

int kageant_autoenc_clamp(int v);
const char *kageant_confirm_token(int mode);
int kageant_inidir(char *out, size_t outlen);
int kageant_path_under(const char *dir, const char *path);
int kageant_policy_get(const char *inikey, const char *regname);
void kageant_resolve_form(const char *stored, char *out, size_t outlen);
void kageant_store_form(const char *abspath, char *out, size_t outlen);

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
