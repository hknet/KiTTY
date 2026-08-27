/*
 * kitty_theme_pref.c - the colour theme's store, for the satellite binaries.
 *
 * See kitty_theme_pref.h for why kitty.exe does not come through here.
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
#include "kitty_inilight.h"

/* The consolidated KiTTY hive, and the same value name kitty.exe writes. */
#define KITTY_THEME_REG_BASE  "Software\\kapper.net\\KiTTY"
#define KITTY_THEME_REG_VALUE "theme"
#define KITTY_THEME_INI_SECTION "KiTTY"
#define KITTY_THEME_INI_KEY   "theme"

static int theme_reg_read(int *pref_out)
{
    char buf[32];
    DWORD sz = sizeof(buf);
    int v;
    if (RegGetValueA(HKEY_CURRENT_USER, KITTY_THEME_REG_BASE,
                     KITTY_THEME_REG_VALUE, RRF_RT_REG_SZ, NULL,
                     buf, &sz) != ERROR_SUCCESS)
        return 0;
    buf[sizeof(buf) - 1] = '\0';
    v = kitty_theme_pref_from_string(buf);
    if (v < 0)
        return 0;               /* present but unreadable: as if unset */
    *pref_out = v;
    return 1;
}

static void theme_reg_write(int pref)
{
    HKEY hk;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, KITTY_THEME_REG_BASE, 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &hk, NULL) == ERROR_SUCCESS) {
        const char *s = kitty_theme_pref_to_string(pref);
        RegSetValueExA(hk, KITTY_THEME_REG_VALUE, 0, REG_SZ,
                       (const BYTE *)s, (DWORD)(strlen(s) + 1));
        RegCloseKey(hk);
    }
}

/*
 * Which store wins is the rule every other satellite setting follows: when the
 * ini kitty_inilight resolved is authoritative (a portable install), it decides
 * and the registry is only a fallback; otherwise the registry decides and the
 * ini acts as a first-run default.
 */
int kitty_theme_pref_get(void)
{
    char buf[32];
    int ini_v = -1, reg_v;

    if (kitty_inilight_read(KITTY_THEME_INI_SECTION, KITTY_THEME_INI_KEY,
                            buf, sizeof(buf)))
        ini_v = kitty_theme_pref_from_string(buf);

    if (kitty_inilight_registry_authoritative()) {
        if (theme_reg_read(&reg_v))
            return reg_v;
        return ini_v >= 0 ? ini_v : KITTY_THEME_SYSTEM;
    }
    if (ini_v >= 0)
        return ini_v;
    if (theme_reg_read(&reg_v))
        return reg_v;
    return KITTY_THEME_SYSTEM;
}

void kitty_theme_pref_set(int pref)
{
    if (pref < KITTY_THEME_SYSTEM || pref > KITTY_THEME_DARK)
        pref = KITTY_THEME_SYSTEM;
    kitty_inilight_write(KITTY_THEME_INI_SECTION, KITTY_THEME_INI_KEY,
                         kitty_theme_pref_to_string(pref));
    theme_reg_write(pref);
}

bool kitty_theme_pref_dark(void)
{
    return kitty_theme_dark_for(kitty_theme_pref_get());
}
