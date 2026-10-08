/*
 * kitty_termenv_hint.c - "this host refuses COLORTERM / TERM_PROGRAM" is
 * said once per host: the mark that it was, in the store in use. The rule
 * is kitty_termenv_hint_due (kitty_termenv.c); see kitty_termenv.h.
 *
 * The same shape as kitty_winpos.c's shared entries: the folder store keeps
 * one file per host in its TermEnvHint folder (beside Sessions and
 * SshHostKeys, the name munged like a host-key file's), the registry one
 * REG_SZ "1" per host under <base>\TermEnvHint. Both go with the rest of the
 * store: the folder with the portable copy, the key with cleanup_all and
 * /delreg (the whole base hive).
 *
 * Not a configuration change: no kitty_store_mark_dirty, so a mark alone
 * never makes a .sav backup.
 */
#include <winsock2.h>   /* before windows.h: putty.h pulls it in further down */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "putty.h"
#include "kitty_storage.h"
#include "kitty_termenv.h"

#define KTH_SUBDIR "TermEnvHint"   /* registry subkey and portable folder */

static void kth_reg_path(char *buf, size_t n)
{
    _snprintf(buf, n, "%s\\%s", kitty_registry_base(), KTH_SUBDIR);
    if (n) buf[n - 1] = '\0';
}

static bool kth_said(void *ctx, const char *host_id)
{
    (void)ctx;
    if (store_is_file()) {
        char *v = portable_read_text_file(KTH_SUBDIR, host_id);
        bool said = (v != NULL);
        sfree(v);
        return said;
    } else {
        char base[600];
        HKEY hk;
        bool said;
        kth_reg_path(base, sizeof(base));
        if (RegOpenKeyExA(HKEY_CURRENT_USER, base, 0, KEY_QUERY_VALUE, &hk)
            != ERROR_SUCCESS)
            return false;
        said = RegQueryValueExA(hk, host_id, NULL, NULL, NULL, NULL)
               == ERROR_SUCCESS;
        RegCloseKey(hk);
        return said;
    }
}

static bool kth_mark(void *ctx, const char *host_id)
{
    (void)ctx;
    if (store_is_file()) {
        return portable_write_text_file(KTH_SUBDIR, host_id, "1") != 0;
    } else {
        char base[600];
        HKEY hk;
        bool ok = false;
        kth_reg_path(base, sizeof(base));
        if (RegCreateKeyExA(HKEY_CURRENT_USER, base, 0, NULL, 0,
                            KEY_SET_VALUE, NULL, &hk, NULL) == ERROR_SUCCESS) {
            ok = RegSetValueExA(hk, host_id, 0, REG_SZ, (const BYTE *)"1", 2)
                 == ERROR_SUCCESS;
            RegCloseKey(hk);
        }
        return ok;
    }
}

bool kitty_termenv_hint_first(const char *host_id)
{
    static const KittyTermEnvHintStore store = { kth_said, kth_mark, NULL };
    return kitty_termenv_hint_due(&store, host_id);
}
