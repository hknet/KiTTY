/*
 * kitty_winevent_cfg.c - the [KiTTY] switches of the Windows Event Log
 * records for the programs outside kitty.exe: clieventlog (klink, kscp,
 * ksftp) and kittygeneventlog (kittygen, kittygen-cli). Read where kitty.exe
 * keeps them: the registry first when the registry is the store in charge
 * (the configuration box's checkbox writes there, WriteParameter), kitty.ini
 * otherwise and as the registry's fallback - the reading of
 * inheritdefaultsettings in kitty_console_store.c. kitty.exe itself reads
 * sessioneventlog with ReadParameterN.
 *
 * Linked into those five programs beside kitty_inifile.c (the kitty.ini
 * resolver) and kitty_storage.c (the registry base).
 */
#include <windows.h>
#include <string.h>

#include "kitty_inifile.h"
#include "kitty_inikeys.h"
#include "kitty_winevent.h"

#include "kitty_globalsettings.h"   /* kitty_console_global_read */

int kitty_wev_switch_on(const char *key)
{
    char v[16];
    /* store-pairs: read param KiTTY clieventlog,kittygeneventlog - the Event Log switches */
    kitty_console_global_read(key, v, sizeof(v));
    return !stricmp(v, "yes");
}
