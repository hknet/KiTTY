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

#endif /* KITTY_PAGEANT_INT_H */
