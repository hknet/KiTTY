/*
 * kitty_kageant_openssh.c - kageant's optional integration with the Windows
 * OpenSSH client: the registry switch behind the tray item, the
 * IdentityAgent line written to %USERPROFILE%\.ssh\kageant.conf, and the
 * marker-delimited managed block that Includes it from ~/.ssh/config,
 * written atomically with a one-time backup and removed again when the
 * integration is switched off.
 */
#include <stdio.h>
#include "kitty_atomicfile.h"   /* a file written whole or not at all */
#include <stdlib.h>
#include <ctype.h>

#include "putty.h"
#include "pageant.h"

#include <shellapi.h>

#include "kitty_pageant.h"
#include "kitty_notice.h"   /* KiTTY: near-the-clock notice window (not a balloon) */
#include "kitty_inifile.h"

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
#include "kitty_winerr.h"   /* kitty_win_reason: Windows' reason, for the reports */
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

/* Registry only, in either store mode: the flag describes THIS machine's
 * %USERPROFILE%\.ssh\config. A kitty.ini carried to another machine
 * must not claim an integration that machine never had. */
int kageant_openssh_set(int on)
{
    return kageant_set_note(kageant_reg_write(KAGEANT_REG_VALUE, on),
                            KAGEANT_REG_VALUE);
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

/* Read a whole file into a malloc'd NUL-terminated buffer: 1 read whole,
 * 0 no such file (the path or the file missing), -1 it is there but could
 * not be read whole (the Windows error left in GetLastError()). A file that
 * cannot be read must never be taken for an empty one: writing our block in
 * its place would replace the user's config. */
static int kageant_read_file(const char *path, char **out, size_t *outlen)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD size, got = 0, err;
    char *buf;
    *out = NULL; *outlen = 0;
    if (h == INVALID_HANDLE_VALUE) {
        err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
            return 0;
        SetLastError(err);
        return -1;
    }
    size = GetFileSize(h, NULL);
    if (size == INVALID_FILE_SIZE || size > 16 * 1024 * 1024) {
        err = size == INVALID_FILE_SIZE ? GetLastError() : ERROR_FILE_TOO_LARGE;
        CloseHandle(h);
        SetLastError(err);
        return -1;
    }
    buf = snewn(size + 1, char);
    if (!ReadFile(h, buf, size, &got, NULL) || got != size) {
        err = GetLastError();
        CloseHandle(h);
        sfree(buf);
        SetLastError(err ? err : ERROR_READ_FAULT);
        return -1;
    }
    CloseHandle(h);
    buf[size] = '\0';
    *out = buf; *outlen = size;
    return 1;
}

/* Replace `path` contents with `data`, whole or not at all (the shared
 * kitty_write_file_whole). 1 or 0 with the Windows error in GetLastError();
 * the file is untouched then. */
static int kageant_write_file_atomic(const char *path, const char *data)
{
    return kitty_write_file_whole(path, data, strlen(data));
}

/* One-time backup of config before kageant first edits it (never clobbers):
 * 1 there is one (made now or earlier), 0 it could not be made. */
static int kageant_backup_config_once(const char *cfgpath)
{
    char *bak = dupprintf("%s.kageant.bak", cfgpath);
    int ok = CopyFileA(cfgpath, bak, TRUE)   /* bFailIfExists = TRUE */
             || GetLastError() == ERROR_FILE_EXISTS;
    sfree(bak);
    return ok;
}

/* "<what> <path>: <Windows' reason>" into err. */
static void kageant_ossh_err(char *err, size_t errlen, const char *what,
                             const char *path)
{
    snprintf(err, errlen, "%s %s: %s", what, path,
             kitty_win_reason((int)GetLastError()));
}

/* Apply (on=1) or remove (on=0) the OpenSSH integration. Touches only our
 * managed block in ~/.ssh/config; the rest is preserved byte-for-byte. A
 * config that cannot be read whole, or is not plain text (UTF-16 carries zero
 * bytes), is never written. 1 done; 0 not, with the file and Windows' reason
 * in err and ~/.ssh/config as it was. *backup is set to 1 when a
 * config.kageant.bak exists (only when the config existed). */
int kageant_openssh_apply(int on, int *backup, char *err, size_t errlen)
{
    char *cfgpath  = kageant_ssh_path("config");
    char *confpath = kageant_ssh_path("kageant.conf");
    char *sshdir   = kageant_ssh_path("");   /* "...\.ssh\" */
    char *cfg = NULL;
    size_t cfglen = 0;
    int ok = 0, r;
    if (backup) *backup = 0;
    err[0] = '\0';
    if (!cfgpath || !confpath || !sshdir) {
        snprintf(err, errlen, "%s", KT_KA_OPENSSH_ERR_NOPROFILE);
        goto done;
    }
    { size_t L = strlen(sshdir); if (L && sshdir[L-1] == '\\') sshdir[L-1] = '\0'; }
    if (!CreateDirectoryA(sshdir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        kageant_ossh_err(err, errlen, KT_KA_OPENSSH_ERR_DIR, sshdir);
        goto done;
    }

    r = kageant_read_file(cfgpath, &cfg, &cfglen);
    if (r < 0) {
        kageant_ossh_err(err, errlen, KT_KA_OPENSSH_ERR_READ, cfgpath);
        goto done;
    }
    if (r > 0 && strlen(cfg) != cfglen) {
        snprintf(err, errlen, KT_KA_OPENSSH_ERR_NOTTEXT_FMT, cfgpath);
        goto done;
    }

    if (on) {
        /* (1) (re)write kageant.conf with the current pipe name */
        char *pipename = agent_named_pipe_name();
        char *line = dupprintf("IdentityAgent \"%s\"\n", pipename);
        for (char *p = line; *p; p++) if (*p == '\\') *p = '/';
        sfree(pipename);
        r = kageant_write_file_atomic(confpath, line);
        sfree(line);
        if (!r) {
            kageant_ossh_err(err, errlen, KT_KA_OPENSSH_ERR_WRITE, confpath);
            goto done;
        }

        /* (2) ensure ~/.ssh/config Include's it via our managed block, but
         * never add a duplicate if a block or a manual Include already exists */
        if (cfg && (strstr(cfg, KAGEANT_SSH_BEGIN) ||
                    strstr(cfg, "Include kageant.conf"))) {
            if (backup && cfg) {
                char *bak = dupprintf("%s.kageant.bak", cfgpath);
                *backup = GetFileAttributesA(bak) != INVALID_FILE_ATTRIBUTES;
                sfree(bak);
            }
            ok = 1;
            goto done;
        }
        if (cfg) {
            if (!kageant_backup_config_once(cfgpath)) {
                kageant_ossh_err(err, errlen, KT_KA_OPENSSH_ERR_BACKUP, cfgpath);
                goto done;
            }
            if (backup) *backup = 1;
        }
        /* block ends in \n; placed before existing content with no extra
         * separator, so a later remove restores the file byte-for-byte */
        char *newcfg = dupprintf(
            "%s\n"
            "# kageant rewrites kageant.conf on each launch; do not edit by hand.\n"
            "Include kageant.conf\n"
            "%s\n%s",
            KAGEANT_SSH_BEGIN, KAGEANT_SSH_END, cfg ? cfg : "");
        r = kageant_write_file_atomic(cfgpath, newcfg);
        sfree(newcfg);
        if (!r) {
            kageant_ossh_err(err, errlen, KT_KA_OPENSSH_ERR_WRITE, cfgpath);
            goto done;
        }
    } else {
        /* (1) remove ONLY our managed block from ~/.ssh/config */
        char *b = cfg ? strstr(cfg, KAGEANT_SSH_BEGIN) : NULL;
        char *e = b ? strstr(b, KAGEANT_SSH_END) : NULL;
        if (b && e) {
            if (!kageant_backup_config_once(cfgpath)) {
                kageant_ossh_err(err, errlen, KT_KA_OPENSSH_ERR_BACKUP, cfgpath);
                goto done;
            }
            e += strlen(KAGEANT_SSH_END);
            if (*e == '\r') e++;
            if (*e == '\n') e++;
            size_t headlen = (size_t)(b - cfg);
            char *newcfg = snewn(headlen + strlen(e) + 1, char);
            memcpy(newcfg, cfg, headlen);
            strcpy(newcfg + headlen, e);
            r = kageant_write_file_atomic(cfgpath, newcfg);
            sfree(newcfg);
            if (!r) {
                kageant_ossh_err(err, errlen, KT_KA_OPENSSH_ERR_WRITE, cfgpath);
                goto done;
            }
        }
        /* (2) empty kageant.conf so any lingering Include is inert */
        if (GetFileAttributesA(confpath) != INVALID_FILE_ATTRIBUTES &&
            !kageant_write_file_atomic(confpath, "")) {
            kageant_ossh_err(err, errlen, KT_KA_OPENSSH_ERR_WRITE, confpath);
            goto done;
        }
    }
    ok = 1;
  done:
    sfree(cfg);
    sfree(cfgpath); sfree(confpath); sfree(sshdir);
    return ok;
}
