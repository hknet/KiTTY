/*
 * kitty_theme_pref.c - the colour theme's store, for the satellite binaries.
 *
 * See kitty_theme_pref.h for what kitty.exe takes from here.
 *
 * The value is a STRING in both stores - "system", "light" or "dark" - and not
 * a DWORD, because kitty.exe's ReadParameter() reads the registry side as a
 * string and there is only one value name to go round. A registry value cannot
 * be both types at once, so a DWORD here would be a setting the main program
 * could never read.
 */
#include <windows.h>
#include <string.h>

#include "kitty_theme.h"
#include "kitty_theme_pref.h"
#include "kitty_inifile.h"
#include "kitty_globalsettings.h"
#include "kitty_inikeys.h"  /* KI_*: the kitty.ini key names */
#include "kitty_oldwin_reg.h"   /* XP: RegDeleteTree/RegGetValue via oldwin */

/* The consolidated KiTTY hive, and the same value name kitty.exe writes. */
/* the key kitty.exe keeps its settings under (KiClassName=PuTTY: PuTTY's) */
#define KITTY_THEME_REG_VALUE "theme"

/* readonly=yes: the choice theme_reg_write did not store, held for this
 * process (-1: none) */
static int theme_ro_pref = -1;

static int theme_valid(const char *s)
{
    return kitty_theme_pref_from_string(s) >= 0;
}

/* Which store wins is the rule every other satellite setting follows: when the
 * ini kitty_inifile resolved is authoritative (a portable install), it decides
 * and the registry is only a fallback; otherwise the registry decides and the
 * ini acts as a first-run default. A value that is not a theme counts as
 * unset in its store (kitty_globalsettings.c, KS_RULE_CHARGE_FIRST). */
static const struct kitty_setting theme_setting = {
    KI_SECTION_KITTY, KI_THEME, KITTY_THEME_REG_VALUE,
    KS_HIVE_INICLASS, KS_RULE_CHARGE_FIRST, theme_valid
};

int kitty_theme_pref_get(void)
{
    char *v;
    int pref;
    if (theme_ro_pref >= 0 && kitty_inifile_registry_authoritative() &&
        !kitty_settings_policy_source(KITTY_THEME_REG_VALUE, 0))
        return theme_ro_pref;
    /* store-pairs: read param KiTTY KI_THEME - the satellites' reader (kitty_globalsettings.c): the store in charge first, the other one as the fallback */
    v = kitty_settings_read_dup(&theme_setting);
    pref = v ? kitty_theme_pref_from_string(v) : KITTY_THEME_SYSTEM;
    free(v);
    return pref;
}

/* To the store in charge only, the same rule: it used to be written to both,
 * so a portable install wrote the registry. With readonly=yes the choice holds
 * for this run only. 1 when stored or skipped so, 0 with the reason in
 * GetLastError(). */
int kitty_theme_pref_set(int pref)
{
    int rc;
    if (pref < KITTY_THEME_SYSTEM || pref > KITTY_THEME_DARK)
        pref = KITTY_THEME_SYSTEM;
    /* store-pairs: write param KiTTY KI_THEME - the satellites' writer (kitty_globalsettings.c): kitty.ini or the hive, by the store in charge */
    rc = kitty_settings_write(&theme_setting, kitty_theme_pref_to_string(pref));
    if (rc == KITTY_RO_REFUSED && kitty_inifile_registry_authoritative())
        theme_ro_pref = pref;
    return rc;
}

bool kitty_theme_pref_dark(void)
{
    return kitty_theme_dark_for(kitty_theme_pref_get());
}
