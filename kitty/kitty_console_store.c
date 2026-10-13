/*
 * kitty_console_store.c: klink, kscp and ksftp use the store kitty.exe uses.
 *
 * Until 0.85.1.15 the console tools read the registry only: the folder store
 * (savemode=dir, the portable build) is switched on by kitty.exe's startup
 * (kitty_startup.c), which they do not carry. A klink beside a portable
 * KiTTY++ therefore did not find the sessions that copy had just saved.
 *
 * This runs first in their main()s. It resolves kitty.ini the way kageant and
 * kitty.exe do (kitty_inifile.c: %KITTY_INI_FILE%, beside the exe, the
 * per-user copies) and, for a folder store, sets what kitty.exe sets from the
 * same keys: the session folder and file ending, the host-key folder and
 * ending, the portable password policy. It writes nothing - no folder is
 * created, no ini key copied over; kitty.exe's first start does that.
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

void kitty_console_store_init(void)
{
    char dir[4 * MAX_PATH + 8], suffix[80], hkdir[4 * MAX_PATH + 8], hksuf[80];
    char pp[32];

    kitty_publish_kittypp_dir();
    kitty_console_mpw_init();

    if (kitty_inifile_folder_store(dir, sizeof(dir), suffix, sizeof(suffix))) {
        kitty_set_storage_mode(2);      /* SAVEMODE_DIR, as kitty_startup.c */
        kitty_set_session_dir(dir);
        kitty_set_session_suffix(suffix);
        if (kitty_inifile_hostkey_store(hkdir, sizeof(hkdir), hksuf, sizeof(hksuf))) {
            kitty_set_hostkey_dir(hkdir);
            kitty_set_hostkey_suffix(hksuf);
        }
        if (kitty_inifile_read(KI_SECTION_KITTY, KI_PORTABLEPASSWORDPROTECTION,
                                pp, sizeof(pp)))
            kitty_set_portable_password_protection(pp);
    }

    /* the settings travel with the program (folder store, or savemode=file):
     * the log default stays beside it, as in kitty.exe */
    kitty_set_log_default_beside_program(!kitty_inifile_registry_authoritative());

    /* -masterpwfile before ANY argument is processed (cmdline.c). After the
     * store setup above: the PortablePasswordProtection conflict needs it. */
    kitty_masterpwfile_prescan();
}
