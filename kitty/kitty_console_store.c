/*
 * kitty_console_store.c: klink, kscp and ksftp use the store kitty.exe uses.
 *
 * Until 0.85.1.15 the console tools read the registry only: the folder store
 * (savemode=dir, the portable build) is switched on by kitty.exe's startup
 * (kitty_startup.c), which they do not carry. A klink beside a portable
 * KiTTY++ therefore did not find the sessions that copy had just saved.
 *
 * This runs first in their main()s. It takes kitty.ini's start-up keys from
 * the resolver (kitty_inifile.c kitty_inifile_startup: %KITTY_INI_FILE%,
 * beside the exe, the per-user copy) and, for a folder store, sets what
 * kitty.exe sets from the same keys: the session folder and file ending, the
 * host-key folder and ending, the portable password policy. It writes
 * nothing - no folder is created, no ini key copied over; kitty.exe's first
 * start does that.
 *
 * A script that wants the registry instead points KITTY_INI_FILE at an ini
 * without savemode=dir for that one call.
 *
 * Master-password-protected passwords (MPW2) of a portable store are unlocked
 * since 0.85.1.15: the tools link the MPW crypto (kitty_mpw.c), take
 * -masterpwfile (cmdline.c) and otherwise prompt on the console
 * (kitty_console_mpw.c, installed below).
 */

#include "putty.h"
#include "kitty_console_store.h"
#include "kitty_inifile.h"
#include "kitty_inikeys.h"
#include "kitty_storage.h"
#include "kitty_secretstore.h"
#include "kitty_hostkey_acceptnew.h"

/* accept-new's record on standard error. Under -v the Event Log line is
 * printed there already (console_eventlog). */
static void console_accept_new_report(const char *line)
{
    if (cmdline_verbose())
        return;
    fprintf(stderr, "%s\n", line);
    fflush(stderr);
}

void kitty_console_store_init(void)
{
    static struct kitty_ini_startup ini;   /* static: two paths' worth */

    kitty_publish_kittypp_dir();
    kitty_console_mpw_init();

    kitty_inifile_startup(&ini);
    if (ini.folder_store) {
        kitty_set_storage_mode(2);      /* SAVEMODE_DIR, as kitty_startup.c */
        kitty_set_session_dir(ini.session_dir);
        kitty_set_session_suffix(ini.session_suffix);
        kitty_set_hostkey_dir(ini.hostkey_dir);
        kitty_set_hostkey_suffix(ini.hostkey_suffix);
        if (ini.portable_password_protection[0])
            kitty_set_portable_password_protection(ini.portable_password_protection);
    }

    /* hknet/KiTTY#61: a setting a session leaves out comes from Default
     * Settings when kitty.exe's switch is set - read where kitty.exe keeps
     * it: the registry first in registry mode (WriteParameter), kitty.ini
     * otherwise and as the registry's fallback (ReadParameterN). */
    {
        char v[16] = "";
        if (ini.registry_authoritative) {
            HKEY k;
            /* the hive in use, the one WriteParameter writes (in the console
             * tools the default one: they do not apply KiClassName) */
            extern const char *kitty_registry_base(void);
            if (RegOpenKeyExA(HKEY_CURRENT_USER, kitty_registry_base(), 0,
                              KEY_QUERY_VALUE, &k) == ERROR_SUCCESS) {
                DWORD type = 0, n = sizeof(v) - 1;
                if (RegQueryValueExA(k, KI_INHERITDEFAULTSETTINGS, NULL, &type,
                                     (LPBYTE)v, &n) != ERROR_SUCCESS || type != REG_SZ)
                    v[0] = '\0';
                else
                    v[n < sizeof(v) ? n : sizeof(v) - 1] = '\0';
                RegCloseKey(k);
            }
        }
        if (!v[0])
            kitty_inifile_read(KI_SECTION_KITTY, KI_INHERITDEFAULTSETTINGS, v, sizeof(v));
        kitty_set_inherit_default_settings(!stricmp(v, "yes"));
    }

    /* the settings travel with the program (folder store, or savemode=file):
     * the log default stays beside it, as in kitty.exe */
    kitty_set_log_default_beside_program(!ini.registry_authoritative);

    /* [KiTTY] autostoresshkey=yes: accept-new for every session. kitty.ini
     * only, as kitty.exe reads it (kitty_params.c). */
    {
        char v[16] = "";
        kitty_inifile_read(KI_SECTION_KITTY, KI_AUTOSTORESSHKEY, v, sizeof(v));
        kitty_hostkey_accept_new_set_all(!stricmp(v, "yes"));
    }
    kitty_hostkey_accept_new_set_report(console_accept_new_report);

    /* -masterpwfile before ANY argument is processed (cmdline.c). After the
     * store setup above: the PortablePasswordProtection conflict needs it. */
    kitty_masterpwfile_prescan();
}
