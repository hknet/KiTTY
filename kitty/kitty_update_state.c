/*
 * kitty_update_state.c - the stored answer of the update check, read by a
 * program that must not ask the network itself. See kitty_update_state.h.
 *
 * Where kitty_updater.c stores it: the portable state file KiTTYState beside
 * kitty.ini when the settings live in files, else the values UpdateLatest
 * (REG_SZ) and UpdateLatestBeta (REG_DWORD) in the KiTTY hive. The same rule
 * decides here which one to read: kitty_inilight_registry_authoritative().
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kitty_update_state.h"
#include "kitty_inilight.h"
#include "kitty_inikeys.h"      /* KI_*: the kitty.ini key names */
#include "kitty_oldwin_reg.h"   /* XP: RegGetValue via oldwin */

#define KUS_REG_BASE "Software\\kapper.net\\KiTTY"

int kitty_file_version(const char *path, unsigned long *ms, unsigned long *ls);   /* kitty_authenticode.c */

/* One value of the portable state file: lines of key=value, the value
 * %XX-escaped - a version number and a 0/1 flag contain nothing to escape. */
static int kus_state_file_value(const char *key, char *out, int n)
{
    const char *ini = kitty_inilight_file();
    char path[MAX_PATH + 32], line[512], *slash;
    size_t klen = strlen(key);
    FILE *fp;
    int found = 0;

    if (!ini || strlen(ini) > MAX_PATH)
        return 0;
    strcpy(path, ini);
    slash = strrchr(path, '\\');
    if (!slash)
        return 0;
    strcpy(slash + 1, "KiTTYState");
    fp = fopen(path, "rb");
    if (!fp)
        return 0;
    while (fgets(line, sizeof(line), fp)) {
        if (!strncmp(line, key, klen) && line[klen] == '=') {
            char *v = line + klen + 1;
            v[strcspn(v, "\r\n")] = '\0';
            strncpy(out, v, n - 1);
            out[n - 1] = '\0';
            found = out[0] != '\0';
            break;
        }
    }
    fclose(fp);
    return found;
}

static int kus_stored(char *latest, int n, int *beta)
{
    *beta = 0;
    if (!kitty_inilight_registry_authoritative()) {
        char b[16];
        if (!kus_state_file_value("UpdateLatest", latest, n))
            return 0;
        if (kus_state_file_value("UpdateLatestBeta", b, sizeof(b)))
            *beta = atoi(b) != 0;
        return 1;
    } else {
        DWORD sz = (DWORD)n, bv = 0, bsz = sizeof(bv);
        if (RegGetValueA(HKEY_CURRENT_USER, KUS_REG_BASE, "UpdateLatest",
                         RRF_RT_REG_SZ, NULL, latest, &sz) != ERROR_SUCCESS)
            return 0;
        latest[n - 1] = '\0';
        if (RegGetValueA(HKEY_CURRENT_USER, KUS_REG_BASE, "UpdateLatestBeta",
                         RRF_RT_REG_DWORD, NULL, &bv, &bsz) == ERROR_SUCCESS)
            *beta = bv != 0;
        return latest[0] != '\0';
    }
}

static void kus_parse(const char *s, unsigned long v[4])
{
    int i;
    for (i = 0; i < 4; i++) {
        v[i] = strtoul(s, (char **)&s, 10);
        if (*s == '.')
            s++;
    }
}

int kitty_update_state_enabled(void)
{
    char buf[32];
    DWORD sz = sizeof(buf);
    int have = kitty_inilight_read("KiTTY", KI_CHECKUPDATE, buf, sizeof(buf));

    /* the store that is authoritative decides; the other is a fallback */
    if (kitty_inilight_registry_authoritative()) {
        char rb[32];
        if (RegGetValueA(HKEY_CURRENT_USER, KUS_REG_BASE, KI_CHECKUPDATE,
                         RRF_RT_REG_SZ, NULL, rb, &sz) == ERROR_SUCCESS) {
            rb[sizeof(rb) - 1] = '\0';
            strcpy(buf, rb);
            have = 1;
        }
    }
    if (!have)
        return 1;                       /* not set: on */
    return !(!stricmp(buf, "no") || !stricmp(buf, "0") ||
             !stricmp(buf, "false") || !stricmp(buf, "off"));
}

int kitty_update_state_newer(char *latest_out, int latest_n, int *beta_out)
{
    char self[MAX_PATH], latest[64];
    unsigned long ms, ls, cur[4], lat[4];
    int beta, i;

    if (!GetModuleFileNameA(NULL, self, sizeof(self)) ||
        !kitty_file_version(self, &ms, &ls))
        return 0;
    cur[0] = HIWORD(ms); cur[1] = LOWORD(ms);
    cur[2] = HIWORD(ls); cur[3] = LOWORD(ls);

    if (!kus_stored(latest, sizeof(latest), &beta))
        return 0;
    kus_parse(latest, lat);
    for (i = 0; i < 4 && cur[i] == lat[i]; i++)
        ;
    if (i == 4 || lat[i] < cur[i])
        return 0;                       /* not newer */
    /* KiTTY stable = x.y.M.0, beta = x.y.M.P with P > 0: a stable build is
     * not told about betas. */
    if (cur[3] == 0 && beta)
        return 0;

    if (latest_out && latest_n > 0) {
        strncpy(latest_out, latest, latest_n - 1);
        latest_out[latest_n - 1] = '\0';
    }
    if (beta_out)
        *beta_out = beta;
    return 1;
}
