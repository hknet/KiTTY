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
#include "kitty_globalsettings.h"
#include "kitty_inikeys.h"
#include "kitty_storage.h"
#include "kitty_secretstore.h"
#include "kitty_hostkey_acceptnew.h"
#include "kitty_hostkeys00.h"
#include "kitty_hostkeys.h"
#include "kitty_winevent.h"
#include "kitty_store_acl.h"   /* kitty_set_store_acl_report */
#include "kitty_testdoor_settings.h"   /* KITTY_TEST_SETTINGS_DUMP, test builds */

/* windows/console.c: the question of updatehostkeys=ask on the console */
int console_hk00_confirm(struct Seat *seat, const struct kitty_hk00_question *q,
                         void (*done)(void *ctx, int answer), void *ctx,
                         void **handle);

/* accept-new's record on standard error. Under -v the Event Log line is
 * printed there already (console_eventlog). */
static void console_accept_new_report(const char *line)
{
    if (cmdline_verbose())
        return;
    fprintf(stderr, "%s\n", line);
    fflush(stderr);
}

/* Access rights not set in the folder store (kitty_store_acl.c, which writes
 * the Event Log record): one line on standard error, once per store. */
static int console_store_acl_report(const char *msg)
{
    fprintf(stderr, "%s\n", msg);
    fflush(stderr);
    return 1;
}

void kitty_console_store_init(void)
{
    static struct kitty_ini_startup ini;   /* its strings freed on the next call */

    kitty_publish_kittypp_dir();
    kitty_console_mpw_init();
    kitty_set_store_acl_report(console_store_acl_report);

    kitty_inifile_startup_free(&ini);
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

    /* The registry hive kitty.exe uses (kitty_startup.c): PuTTY's with
     * [KiTTY] KiClassName=PuTTY, else KiTTY++'s own. Without this klink,
     * kscp and ksftp stayed on KiTTY++'s hive: -load missed a session
     * kitty.exe had saved. A transfer kitty.exe starts checked the host
     * key in a store kitty.exe never wrote. */
    {
        char *cls = kitty_inifile_read_dup(KI_SECTION_KITTY, KI_KICLASSNAME);
        if (cls && !stricmp(cls, "PuTTY"))
            kitty_set_registry_root(1);
        free(cls);
    }

    /* hknet/KiTTY#61: a setting a session leaves out comes from Default
     * Settings when kitty.exe's switch is set - read where kitty.exe keeps
     * it: the registry first in registry mode (WriteParameter), kitty.ini
     * otherwise and as the registry's fallback (ReadParameterN). */
    {
        char v[16] = "";
        /* store-pairs: read param KiTTY KI_INHERITDEFAULTSETTINGS - the console tools' reader of kitty.exe's switch (the hive first in registry mode, as ReadParameterN) */
        kitty_console_global_read(KI_INHERITDEFAULTSETTINGS, v, sizeof(v));
        kitty_set_inherit_default_settings(!stricmp(v, "yes"));
    }

    /* [KiTTY] clieventlog=yes: the records of klink, kscp and ksftp in the
     * Windows Event Log (kitty_winevent.h, IDs 300-331), read where
     * kitty.exe's checkbox keeps it - once, here, before any connection */
    kitty_wev_configure(KITTY_EVB_CLI, kitty_wev_switch_on(KI_CLIEVENTLOG));

    /* the settings travel with the program (folder store, or savemode=file):
     * the log default stays beside it, as in kitty.exe */
    kitty_set_log_default_beside_program(!ini.registry_authoritative);

    /* [KiTTY] autostoresshkey=yes: accept-new for every session, read as
     * kitty.exe reads it (kitty_params.c, Security > Host keys). */
    {
        char v[16] = "";
        kitty_console_global_read(KI_AUTOSTORESSHKEY, v, sizeof(v));
        kitty_hostkey_accept_new_set_all(!stricmp(v, "yes"));
    }
    kitty_hostkey_accept_new_set_report(console_accept_new_report);

    /* [KiTTY] updatehostkeys: the host key update (hostkeys-00), read as
     * kitty.exe reads it - no: off, ask: the changes put on the console
     * first, absent or any other value: on. [KiTTY] hostkeyspertype: keys of
     * one type per host and port (1-32, default 2). Its record goes where
     * accept-new's goes. */
    {
        char v[16] = "";
        kitty_console_global_read(KI_UPDATEHOSTKEYS, v, sizeof(v));
        kitty_hk00_set_mode(kitty_hk00_mode_from_word(v));
        kitty_console_global_read(KI_HOSTKEYSPERTYPE, v, sizeof(v));
        if (v[0])
            kitty_hk00_set_pertype(atoi(v));
    }
    kitty_hk00_set_store(&kitty_hostkeys_hk00_store);
    {
        static const struct kitty_hk00_ui console_hk00_ui = {
            console_hk00_confirm, NULL, NULL };
        kitty_hk00_set_ui(&console_hk00_ui);
    }

    /* test builds only - the global settings as read here, then exit
     * (KITTY_TEST_SETTINGS_DUMP). A release build calls an empty function */
    kitty_testdoor_settings_console();

    /* -masterpwfile before ANY argument is processed (cmdline.c). After the
     * store setup above: the PortablePasswordProtection conflict needs it. */
    kitty_masterpwfile_prescan();
}

/* Test builds only (kitty_testdoor_settings.c): the console tools' readers of
 * the global settings the settings move records. */
#include "kitty_testdoor_settings.h"
#define KTS_CONSOLE(fn, key) \
    static char *kts_cg_##fn(void) \
    /* store-pairs: read param KiTTY verifyagent,clieventlog,autostoresshkey,theme,checkupdate - the test door's reader (KTS_CONSOLE rows below) */ \
    { char v[64] = ""; kitty_console_global_read(key, v, sizeof(v)); return dupstr(v); }
KTS_CONSOLE(verifyagent, KI_VERIFYAGENT)
KTS_CONSOLE(clieventlog, KI_CLIEVENTLOG)
KTS_CONSOLE(autostoresshkey, KI_AUTOSTORESSHKEY)
KTS_CONSOLE(theme, KI_THEME)
KTS_CONSOLE(checkupdate, KI_CHECKUPDATE)
static char *kts_cg_showforeign(void)
{
    char v[64] = "";
    return kitty_showforeign_ini_read(v, sizeof(v)) ? dupstr(v) : dupstr("<absent>");
}
static char *kts_cg_auth(void) { return kitty_testdoor_int_dup(kitty_inifile_registry_authoritative()); }
static char *kts_cg_base(void) { return dupstr(kitty_registry_base()); }
void kitty_testdoor_settings_console(void)
{
    static const struct kts_probe p[] = {
        { "verifyagent", kts_cg_verifyagent, NULL },
        { "clieventlog", kts_cg_clieventlog, NULL },
        { "autostoresshkey", kts_cg_autostoresshkey, NULL },
        { "theme", kts_cg_theme, NULL },
        { "checkupdate", kts_cg_checkupdate, NULL },
        { "showforeignsessions", kts_cg_showforeign, NULL },
        { "store.authoritative", kts_cg_auth, NULL },
        { "store.base", kts_cg_base, NULL },
    };
    kitty_testdoor_settings(p, (int)(sizeof(p) / sizeof(p[0])));
}
