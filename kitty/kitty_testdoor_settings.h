/*
 * kitty_testdoor_settings.h - test builds only (KITTY_TEST_SETTINGS_SET /
 * KITTY_TEST_SETTINGS_DUMP): each program's global-setting readers and
 * writers, recorded per save mode and KiClassName. A release build calls an
 * empty function. See kitty_testdoor_settings.c.
 */
#ifndef KITTY_TESTDOOR_SETTINGS_H
#define KITTY_TESTDOOR_SETTINGS_H

struct kts_probe {
    const char *name;
    char *(*get)(void);              /* the value to sfree, NULL = none */
    int (*set)(const char *value);   /* NULL = no writer in this program */
};

void kitty_testdoor_settings(const struct kts_probe *p, int n);
char *kitty_testdoor_int_dup(int v);

/* Each program's table, called where its settings are readable. */
void kitty_testdoor_settings_kitty(void);     /* kitty.exe, kitty_win.c */
void kitty_testdoor_settings_kageant(void);   /* kageant, kitty_kageant_settings.c */
void kitty_testdoor_settings_console(void);   /* klink/kscp/ksftp, kitty_console_store.c */
void kitty_testdoor_settings_kittygen(void);  /* kittygen, windows/puttygen.c */

/* The readers and writers by key path, beside each reader (test builds only:
 * a release build's return NULL or 0). */
char *kitty_testdoor_auxpos_dup(const char *key, int name);   /* kitty_auxpos.c, malloc'd */
char *kitty_testdoor_termenv_said_dup(const char *host_id);   /* kitty_termenv_hint.c */
int kitty_testdoor_termenv_mark(const char *host_id);
char *kitty_testdoor_helper_get_dup(const char *full);        /* kitty_xfer.c */
int kitty_testdoor_helper_put(const char *full, const char *hex);
int kitty_testdoor_cfgbox_restore(void);                      /* windows/dialog.c */
char *kitty_testdoor_launcher_rows_dup(void);                 /* kitty_launcher.c */

#endif
