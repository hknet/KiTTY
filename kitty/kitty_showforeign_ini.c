/*
 * kitty_showforeign_ini.c: the console tools' reader for ONE kitty.ini key.
 *
 * klink, kscp and ksftp link the settings library, and with it
 * kitty_storage.c's kitty_get_show_foreign_sessions() - the switch that
 * decides whether a session name still loads from the old 9bis-KiTTY and
 * stock-PuTTY hives. The GUI reads its kitty.ini through kitty.c
 * (ReadParameterN); the console tools carry no kitty.c, so their copy of
 * that reader is a stub that says "not set" and they saw only the pinned
 * registry choice and the auto rule - a `showforeignsessions=yes` in
 * kitty.ini kept `kitty -load oldname` working but not `klink -load
 * oldname`.
 *
 * This strong definition replaces the weak stub in kitty_storage.c for the
 * three console targets only (windows/CMakeLists.txt lists this file and
 * kitty_inilight.c beside them). It answers through the light resolver, so
 * the ini that kageant and the GUI resolve is the ini the tools read:
 * %KITTY_INI_FILE%, then kitty.ini / putty.ini beside the exe, then the
 * per-user copies. The main section is the same [KiTTY] the GUI reads.
 */

#include <stddef.h>

#include "kitty_inilight.h"
#include "kitty_inikeys.h"

int kitty_showforeign_ini_read(char *value, size_t size);
int kitty_showforeign_may_persist(void);

int kitty_showforeign_ini_read(char *value, size_t size)
{
    if (!value || size == 0)
        return 0;
    value[0] = '\0';
    if (size > 0x7fffffff)
        size = 0x7fffffff;
    return kitty_inilight_read(KI_SECTION_KITTY, KI_SHOWFOREIGNSESSIONS,
                               value, (int)size);
}

/* A console tool evaluates "auto" READ-ONLY. Only KiTTY itself writes the
 * one-time answer (ShowForeignSessions=1) and arms the foreign-sessions
 * notice; a klink -load on a fresh install must not decide, on the GUI's
 * behalf, what the GUI's next start shows. Overrides the weak default (1)
 * in kitty_storage.c. */
int kitty_showforeign_may_persist(void)
{
    return 0;
}
