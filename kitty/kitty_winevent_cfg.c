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

extern const char *kitty_registry_base(void);   /* kitty_storage.c */

int kitty_wev_switch_on(const char *key)
{
    char v[16] = "";

    if (kitty_inifile_registry_authoritative()) {
        HKEY k;
        /* the hive in use, the one WriteParameter writes (in these
         * programs the default one: they do not apply KiClassName) */
        if (RegOpenKeyExA(HKEY_CURRENT_USER, kitty_registry_base(), 0,
                          KEY_QUERY_VALUE, &k) == ERROR_SUCCESS) {
            DWORD type = 0, n = sizeof(v) - 1;
            if (RegQueryValueExA(k, key, NULL, &type, (LPBYTE)v, &n) !=
                    ERROR_SUCCESS || type != REG_SZ)
                v[0] = '\0';
            else
                v[n < sizeof(v) ? n : sizeof(v) - 1] = '\0';
            RegCloseKey(k);
        }
    }
    if (!v[0])
        kitty_inifile_read(KI_SECTION_KITTY, key, v, sizeof(v));
    return !stricmp(v, "yes");
}
