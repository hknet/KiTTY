/*
 * kitty_onetime.h - kitty.exe's one-time steps (kitty_onetime.c), run at
 * five moments of its start (kitty_startup.c).
 */
#ifndef KITTY_ONETIME_H
#define KITTY_ONETIME_H

enum kitty_onetime_moment {
    KOT_BEFORE_INI,         /* before kitty.ini is read */
    KOT_AFTER_INI,          /* kitty.ini read, before the shortcuts */
    KOT_AFTER_HIVE_CHECK,   /* after the start noted whether the hive existed */
    KOT_STORE_LOAD,         /* the folder store, before it starts */
    KOT_END                 /* the end of the start, before any unlock */
};

/* The steps of that moment. KOT_STORE_LOAD and KOT_END: 1 when something
 * moved just now, for the start's report. */
int kitty_onetime_run(enum kitty_onetime_moment at);

/* the launcher's start runs it as well */
void MigrateOldKittyHive(void);

/* kitty_startup.c: a notice box, or the console line in CLI mode */
void StartupNotice(const char *msg, int warn);

#endif
