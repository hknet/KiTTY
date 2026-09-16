/*
 * kitty_kageant_audit.c - kageant's audit log: whether it is on, how big and
 * how many files to keep, where it lives, its one-time setup, and the
 * per-request line each mutating agent operation writes through it. Split
 * from kitty_pageant.c; the settings helpers it shares with that file are
 * declared in kitty_pageant_int.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#include "putty.h"
#include "pageant.h"

#include <shellapi.h>

#include "kitty_pageant.h"
#include "kitty_pageant_int.h"      /* what kitty_pageant.c and its split-off files share */
#include "kitty_kageant_openssh.h"  /* the Windows OpenSSH client integration */
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
int kageant_audit_get(void)
{
    return kageant_bool_get(KI_AGENT_AGENTLOG, "AgentLog", 1);
}
int kageant_audit_set(int on)
{
    kitty_inilight_write(KI_SECTION_AGENT, KI_AGENT_AGENTLOG, on ? "yes" : "no");
    kageant_reg_write("AgentLog", on ? 1 : 0);
    kageant_audit_setup();
    return 1;
}

/* The knobs, for the settings dialog. Same defaults and clamps as the
 * setup below - one source of truth for the ranges would be nicer, but
 * these two sites are three lines apart and say the same numbers. */
int kageant_audit_maxkb_get(void)
{
    return kageant_int_setting(KI_AGENT_AGENTLOGMAXKB, "AgentLogMaxKB",
                               KAGEANT_AGENTLOG_KB_DEFAULT, 16, 1048576);
}
int kageant_audit_keep_get(void)
{
    return kageant_int_setting(KI_AGENT_AGENTLOGKEEP, "AgentLogKeep",
                               KAGEANT_AGENTLOG_KEEP_DEFAULT, 1, 99);
}
int kageant_audit_expire_get(void)
{
    return kageant_int_setting(KI_AGENT_AGENTLOGEXPIREDAYS, "AgentLogExpireDays",
                               KAGEANT_AGENTLOG_DAYS_DEFAULT, 0, 3650);
}
int kageant_audit_pathsetting_get(char *buf, size_t len)
{
    return kageant_setting_str_get(KI_AGENT_AGENTLOGPATH, "AgentLogPath",
                                   buf, len);
}

void kageant_audit_cfg_set(const char *path, int maxkb, int keep,
                           int expiredays)
{
    char num[16];
    if (maxkb < 16) maxkb = 16;
    if (maxkb > 1048576) maxkb = 1048576;
    if (keep < 1) keep = 1;
    if (keep > 99) keep = 99;
    if (expiredays < 0) expiredays = 0;
    if (expiredays > 3650) expiredays = 3650;
    kageant_setting_str_set(KI_AGENT_AGENTLOGPATH, "AgentLogPath",
                            path ? path : "");
    snprintf(num, sizeof(num), "%d", maxkb);
    kitty_inilight_write(KI_SECTION_AGENT, KI_AGENT_AGENTLOGMAXKB, num);
    kageant_reg_write_dword("AgentLogMaxKB", maxkb);
    snprintf(num, sizeof(num), "%d", keep);
    kitty_inilight_write(KI_SECTION_AGENT, KI_AGENT_AGENTLOGKEEP, num);
    kageant_reg_write_dword("AgentLogKeep", keep);
    snprintf(num, sizeof(num), "%d", expiredays);
    kitty_inilight_write(KI_SECTION_AGENT, KI_AGENT_AGENTLOGEXPIREDAYS, num);
    kageant_reg_write_dword("AgentLogExpireDays", expiredays);
    kageant_audit_setup();
}

/* Resolve the log path and (re)configure the sink. Portable installs log
 * beside their kitty.ini; registry-mode installs under
 * %LOCALAPPDATA%\kapper.net\KiTTY; [Agent] auditlogpath overrides both. */
/*
 * Where the log goes when [Agent] agentlogpath is not set. Split out of
 * kageant_audit_setup() so the settings dialog can SHOW this path: the File
 * box is empty in the default case, and an empty box that says "blank =
 * default" tells nobody where the file actually is.
 *
 * `create` is what separates the two callers. The setup path wants the
 * directory to exist because it is about to write there; the dialog only
 * wants the string, and must not create directories as a side effect of
 * being opened.
 */
int kageant_audit_default_path(char *buf, size_t len, int create)
{
    const char *ini;

    if (!buf || len == 0)
        return 0;
    buf[0] = '\0';

    /* A portable install logs beside its kitty.ini, so the whole install
     * stays on the stick. */
    if (!kitty_inilight_registry_authoritative() &&
        (ini = kitty_inilight_file()) != NULL) {
        const char *sl = strrchr(ini, '\\');
        if (sl)
            snprintf(buf, len, "%.*s\\kageant.log", (int)(sl - ini), ini);
    }
    if (!buf[0]) {
        char base[MAX_PATH + 1];
        DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", base, sizeof(base));
        if (n > 0 && n < sizeof(base)) {
            char dir[MAX_PATH + 1];
            snprintf(dir, sizeof(dir), "%s\\kapper.net", base);
            if (create)
                CreateDirectoryA(dir, NULL);
            snprintf(dir, sizeof(dir), "%s\\kapper.net\\KiTTY", base);
            if (create)
                CreateDirectoryA(dir, NULL);
            snprintf(buf, len, "%s\\kageant.log", dir);
        }
    }
    return buf[0] != '\0';
}

void kageant_audit_setup(void)
{
    char path[MAX_PATH + 1];
    path[0] = '\0';
    if (!kageant_setting_str_get(KI_AGENT_AGENTLOGPATH, "AgentLogPath",
                                 path, sizeof(path)) || !path[0])
        kageant_audit_default_path(path, sizeof(path), 1);
    kitty_audit_configure(
        path[0] ? path : NULL, path[0] ? kageant_audit_get() : 0,
        kageant_audit_maxkb_get(), kageant_audit_keep_get(),
        kageant_audit_expire_get());
}

/* Best-effort requester identity for the log: the exe base name AND its
 * full path behind a pid - the base name is what the first-level view
 * shows, the full path is the audit-trail fact ("which ssh.exe?"). */
static void kageant_req_name(unsigned long pid, char *base_out, size_t bsz,
                             char *path_out, size_t psz)
{
    HANDLE h;
    base_out[0] = path_out[0] = '\0';
    if (!pid)
        return;
    h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (h) {
        char path[MAX_PATH + 1];
        DWORD n = sizeof(path);
        if (kitty_process_image_path(h, path, n) && path[0]) {
            const char *base = strrchr(path, '\\');
            snprintf(base_out, bsz, "%s", base ? base + 1 : path);
            snprintf(path_out, psz, "%s", path);
        }
        CloseHandle(h);
    }
}

/* One sign/confirm-shaped audit line; pid 0 drops the requester fields. */
void kageant_audit_use(const char *ev, const char *fp,
                              const char *comment, const char *result,
                              const char *reason, unsigned long pid)
{
    char req[80], reqpath[MAX_PATH + 1], pidbuf[16];
    kageant_req_name(pid, req, sizeof(req), reqpath, sizeof(reqpath));
    snprintf(pidbuf, sizeof(pidbuf), "%lu", pid);
    kitty_audit(ev, "fp", fp, "comment", comment, "result", result,
                "reason", reason, "req", req, "reqpath", reqpath,
                "pid", pid ? pidbuf : NULL, (const char *)NULL);
}
