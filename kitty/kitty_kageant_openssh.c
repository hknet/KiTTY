/*
 * kitty_kageant_openssh.c - kageant's optional integration with the Windows
 * OpenSSH client: the registry switch behind the tray item, the
 * IdentityAgent line written to %USERPROFILE%\.ssh\kageant.conf, and the
 * marker-delimited managed block that Includes it from ~/.ssh/config,
 * written atomically with a one-time backup and removed again when the
 * integration is switched off.
 */
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#include "putty.h"
#include "pageant.h"

#include <shellapi.h>

#include "kitty_pageant.h"
#include "kitty_notice.h"   /* KiTTY: near-the-clock notice window (not a balloon) */
#include "kitty_inilight.h"

/* KiTTY: notice accents. Kitty's own notices are green (workplace proxy), so
 * kageant uses two distinct colours: an amber WARNING for key-load problems,
 * and a blue INFO for key use - recognisably kageant, not a stray kitty
 * notice, and not confusable with a warning. */
#include "kitty_startup_shortcut.h"
#include "kitty_protkey.h"  /* kitty_protkey_available: the unprotected-memory warning */
#include "kitty_hello.h"    /* Windows Hello presence check (confirm gating) */
#include "kitty_hello_keys.h"  /* Hello-protected keys: the sidecar test */
#include "kitty_auditlog.h" /* the audit log's file sink */
#include "kitty_theme.h"    /* KITTY_THEME_* preference values */
#include "ssh.h"

#include "kitty_oldwin.h"   /* APIs newer than the oldest Windows we load on */
#include "kitty_oldwin_reg.h"   /* XP: RegDeleteTree/RegGetValue via oldwin */
#include "kitty_text.h"     /* shared captions */
#include "kitty_inikeys.h"  /* KI_*: the kitty.ini key names */
#include "kitty_gui.h"
#include "kitty_kageant_openssh.h"
#include "kitty_pageant_int.h"   /* KAGEANT_REG_BASE */
/* ------------------------------------------------------------------ *
 * KiTTY: optional integration with the Windows OpenSSH client.        *
 *                                                                     *
 * OFF BY DEFAULT. When the user ticks the tray item "Register as      *
 * Windows OpenSSH agent" (persisted in the registry), kageant:        *
 *   - writes %USERPROFILE%\.ssh\kageant.conf with an IdentityAgent    *
 *     line pointing at its named pipe, and                            *
 *   - adds a marker-delimited managed block to %USERPROFILE%\.ssh\    *
 *     config that `Include`s kageant.conf,                            *
 * so the Windows ssh.exe uses kageant as its agent. Disabling removes *
 * the managed block and empties kageant.conf. Only the marker block   *
 * is ever touched - the rest of the user's config is preserved        *
 * byte-for-byte, written atomically (temp + rename), and a one-time   *
 * config.kageant.bak backup is taken before the first edit.           *
 * ------------------------------------------------------------------ */
#define KAGEANT_REG_VALUE  "OpenSSHIntegration"
#define KAGEANT_SSH_BEGIN  "# >>> kageant managed (KiTTY) >>>"
#define KAGEANT_SSH_END    "# <<< kageant managed (KiTTY) <<<"

int kageant_openssh_get(void)
{
    DWORD val = 0, sz = sizeof(val);
    if (RegGetValueA(HKEY_CURRENT_USER, KAGEANT_REG_BASE, KAGEANT_REG_VALUE,
                     RRF_RT_REG_DWORD, NULL, &val, &sz) != ERROR_SUCCESS)
        return 0;
    return val ? 1 : 0;
}

void kageant_openssh_set(int on)
{
    HKEY hk;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, KAGEANT_REG_BASE, 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &hk, NULL) == ERROR_SUCCESS) {
        DWORD val = on ? 1 : 0;
        RegSetValueExA(hk, KAGEANT_REG_VALUE, 0, REG_DWORD,
                       (const BYTE *)&val, sizeof(val));
        RegCloseKey(hk);
    }
}

/* Return malloc'd "%USERPROFILE%\.ssh\<leaf>", or NULL if no USERPROFILE. */
char *kageant_ssh_path(const char *leaf)
{
    const char *home = getenv("USERPROFILE");
    if (!home || !*home)
        return NULL;
    return dupprintf("%s\\.ssh\\%s", home, leaf);
}

/* Write the IdentityAgent line for `pipename` (with / separators) to fp.
 * Shared by the -openssh-config flag path and the tray-toggle path. */
void kageant_write_identityagent(FILE *fp, const char *pipename)
{
    fputs("IdentityAgent \"", fp);
    /* Some Windows OpenSSH versions prefer / to \; none object to /. */
    for (const char *p = pipename; *p; p++)
        fputc(*p == '\\' ? '/' : *p, fp);
    fputs("\"\n", fp);
}

/* Read a whole file into a malloc'd NUL-terminated buffer (binary), or NULL. */
static char *kageant_read_file(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return NULL;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    if (n < 0) { fclose(fp); return NULL; }
    fseek(fp, 0, SEEK_SET);
    char *buf = snewn(n + 1, char);
    size_t got = fread(buf, 1, n, fp);
    fclose(fp);
    buf[got] = '\0';
    return buf;
}

/* Atomically replace `path` contents with `data` (temp file + rename). */
static int kageant_write_file_atomic(const char *path, const char *data)
{
    char *tmp = dupprintf("%s.kageant.tmp", path);
    int ok = 0;
    FILE *fp = fopen(tmp, "wb");
    if (fp) {
        ok = (fputs(data, fp) >= 0);
        if (fclose(fp) != 0) ok = 0;
    }
    if (ok)
        ok = MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING) ? 1 : 0;
    if (!ok)
        remove(tmp);
    sfree(tmp);
    return ok;
}

/* One-time backup of config before kageant first edits it (never clobbers). */
static void kageant_backup_config_once(const char *cfgpath)
{
    char *bak = dupprintf("%s.kageant.bak", cfgpath);
    CopyFileA(cfgpath, bak, TRUE);   /* bFailIfExists = TRUE */
    sfree(bak);
}

/* Apply (on=1) or remove (on=0) the OpenSSH integration. Touches only our
 * managed block in ~/.ssh/config; the rest is preserved byte-for-byte. */
void kageant_openssh_apply(int on)
{
    char *cfgpath  = kageant_ssh_path("config");
    char *confpath = kageant_ssh_path("kageant.conf");
    char *sshdir   = kageant_ssh_path("");   /* "...\.ssh\" */
    if (!cfgpath || !confpath || !sshdir) {
        sfree(cfgpath); sfree(confpath); sfree(sshdir);
        return;
    }
    { size_t L = strlen(sshdir); if (L && sshdir[L-1] == '\\') sshdir[L-1] = '\0'; }
    CreateDirectoryA(sshdir, NULL);   /* harmless if it already exists */

    if (on) {
        /* (1) (re)write kageant.conf with the current pipe name */
        char *pipename = agent_named_pipe_name();
        FILE *fp = fopen(confpath, "wb");
        if (fp) { kageant_write_identityagent(fp, pipename); fclose(fp); }
        sfree(pipename);

        /* (2) ensure ~/.ssh/config Include's it via our managed block, but
         * never add a duplicate if a block or a manual Include already exists */
        char *cfg = kageant_read_file(cfgpath);
        int present = cfg && (strstr(cfg, KAGEANT_SSH_BEGIN) ||
                              strstr(cfg, "Include kageant.conf"));
        if (!present) {
            kageant_backup_config_once(cfgpath);
            /* block ends in \n; placed before existing content with no extra
             * separator, so a later remove restores the file byte-for-byte */
            char *block = dupprintf(
                "%s\n"
                "# kageant rewrites kageant.conf on each launch; do not edit by hand.\n"
                "Include kageant.conf\n"
                "%s\n",
                KAGEANT_SSH_BEGIN, KAGEANT_SSH_END);
            char *newcfg = dupprintf("%s%s", block, (cfg && *cfg) ? cfg : "");
            kageant_write_file_atomic(cfgpath, newcfg);
            sfree(block); sfree(newcfg);
        }
        sfree(cfg);
    } else {
        /* (1) remove ONLY our managed block from ~/.ssh/config */
        char *cfg = kageant_read_file(cfgpath);
        if (cfg) {
            char *b = strstr(cfg, KAGEANT_SSH_BEGIN);
            char *e = b ? strstr(b, KAGEANT_SSH_END) : NULL;
            if (b && e) {
                kageant_backup_config_once(cfgpath);
                e += strlen(KAGEANT_SSH_END);
                if (*e == '\r') e++;
                if (*e == '\n') e++;
                size_t headlen = (size_t)(b - cfg);
                char *newcfg = snewn(headlen + strlen(e) + 1, char);
                memcpy(newcfg, cfg, headlen);
                strcpy(newcfg + headlen, e);
                kageant_write_file_atomic(cfgpath, newcfg);
                sfree(newcfg);
            }
            sfree(cfg);
        }
        /* (2) empty kageant.conf so any lingering Include is inert */
        FILE *fp = fopen(confpath, "wb");
        if (fp) fclose(fp);
    }
    sfree(cfgpath); sfree(confpath); sfree(sshdir);
}
