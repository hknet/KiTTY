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
#include "kitty_atomicfile.h"  /* kitty_write_file_whole: the Event Viewer view file */
int kageant_audit_get(void)
{
    return kageant_bool_get(KI_AGENT_AGENTLOG, "AgentLog", 1);
}
int kageant_audit_set(int on)
{
    int ok = kageant_setting_write(KI_AGENT_AGENTLOG, on ? "yes" : "no",
                                   "AgentLog", on ? 1 : 0);
    kageant_audit_setup();
    return ok;
}

/* The knobs, for the settings dialog. Same defaults and clamps as the
 * setup below - one source of truth for the ranges would be nicer, but
 * these two sites are three lines apart and use the same numbers. */
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
char *kageant_audit_pathsetting_dup(void)
{
    return kageant_setting_str_dup(KI_AGENT_AGENTLOGPATH, "AgentLogPath");
}

/*
 * [Agent] agentlogsink = file | eventlog | both. An explicit value always
 * wins. Without one the sink follows the installation: the system-wide
 * installer registers the KiTTY++_Agent event log, and where it exists
 * the records go to the file AND that log; elsewhere (per-user install,
 * portable copy) to the file only. The probe runs once per process, so the
 * default is decided when kageant starts.
 */
#define KAGEANT_EVENTLOG_KEY \
    "SYSTEM\\CurrentControlSet\\Services\\EventLog\\" KITTY_AUDIT_EVENT_LOG

int kageant_audit_eventlog_exists(void)
{
    static int exists = -1;
    if (exists < 0) {
        HKEY k;
        exists = RegOpenKeyExA(HKEY_LOCAL_MACHINE, KAGEANT_EVENTLOG_KEY, 0,
                               KEY_QUERY_VALUE, &k) == ERROR_SUCCESS;
        if (exists)
            RegCloseKey(k);
    }
    return exists;
}

int kageant_audit_sink_explicit(void)
{
    char *buf = kageant_setting_str_dup(KI_AGENT_AGENTLOGSINK, "AgentLogSink");
    int sinks = -1;                          /* unknown text: the default */
    if (!buf)
        return -1;
    if (!stricmp(buf, "file"))
        sinks = KITTY_AUDIT_SINK_FILE;
    else if (!stricmp(buf, "eventlog"))
        sinks = KITTY_AUDIT_SINK_EVENTLOG;
    else if (!stricmp(buf, "both"))
        sinks = KITTY_AUDIT_SINK_FILE | KITTY_AUDIT_SINK_EVENTLOG;
    sfree(buf);
    return sinks;
}

int kageant_audit_sink_get(void)
{
    int s = kageant_audit_sink_explicit();
    if (s > 0)
        return s;
    return kageant_audit_eventlog_exists() ?
        KITTY_AUDIT_SINK_FILE | KITTY_AUDIT_SINK_EVENTLOG :
        KITTY_AUDIT_SINK_FILE;
}

int kageant_audit_sink_set(int sinks)
{
    const char *s =
        sinks == KITTY_AUDIT_SINK_EVENTLOG ? "eventlog" :
        sinks == (KITTY_AUDIT_SINK_FILE | KITTY_AUDIT_SINK_EVENTLOG) ?
                                             "both" : "file";
    int ok = kageant_setting_str_set(KI_AGENT_AGENTLOGSINK, "AgentLogSink", s);
    kageant_audit_setup();
    return ok;
}

/*
 * Event Viewer on a custom view of the agent's events: the source
 * KiTTY++_agent in the KiTTY++_Agent log (only when that log exists - a
 * query naming a missing log fails the whole view) and in the Application
 * log, where the source lands unregistered. The view is a file in the temp
 * folder handed to eventvwr /v: (Vista on); Windows XP's Event Viewer takes
 * no view, so there - or when the view cannot be written or started - the
 * Event Viewer opens plainly.
 */
void kageant_eventviewer_open(HWND owner)
{
    char view[1024];
    char *tmp = kitty_temp_path_dup();
    int len;

    len = snprintf(view, sizeof(view),
        "<ViewerConfig><QueryConfig><QueryParams><UserQuery/></QueryParams>"
        "<QueryNode><Name>" KITTY_AUDIT_EVENT_SOURCE "</Name><QueryList>"
        "<Query Id=\"0\" Path=\"Application\">%s"
        "<Select Path=\"Application\">"
        "*[System[Provider[@Name='" KITTY_AUDIT_EVENT_SOURCE "']]]</Select>"
        "</Query></QueryList></QueryNode></QueryConfig></ViewerConfig>",
        kageant_audit_eventlog_exists() ?
        "<Select Path=\"" KITTY_AUDIT_EVENT_LOG "\">"
        "*[System[Provider[@Name='" KITTY_AUDIT_EVENT_SOURCE "']]]</Select>" :
        "");
    if (LOBYTE(LOWORD(GetVersion())) >= 6 && tmp &&
        len > 0 && len < (int)sizeof(view)) {
        char *path = dupprintf("%sKiTTYpp-agent-events.xml", tmp);
        char *args = dupprintf("/v:\"%s\"", path);
        int shown = kitty_write_file_whole(path, view, (size_t)len) &&
            (INT_PTR)ShellExecuteA(owner, "open", "eventvwr.exe", args, NULL,
                                   SW_SHOWNORMAL) > 32;
        sfree(path);
        sfree(args);
        if (shown) {
            sfree(tmp);
            return;
        }
    }
    sfree(tmp);
    ShellExecuteA(owner, "open", "eventvwr.msc", NULL, NULL, SW_SHOWNORMAL);
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
    kageant_setting_write(KI_AGENT_AGENTLOGMAXKB, num, "AgentLogMaxKB", maxkb);
    snprintf(num, sizeof(num), "%d", keep);
    kageant_setting_write(KI_AGENT_AGENTLOGKEEP, num, "AgentLogKeep", keep);
    snprintf(num, sizeof(num), "%d", expiredays);
    kageant_setting_write(KI_AGENT_AGENTLOGEXPIREDAYS, num,
                          "AgentLogExpireDays", expiredays);
    kageant_audit_setup();
}

/* Resolve the log path and (re)configure the sink. Portable installs log
 * beside their kitty.ini; registry-mode installs under
 * %LOCALAPPDATA%\kapper.net\KiTTY; [Agent] auditlogpath overrides both. */
/*
 * Where the log goes when [Agent] agentlogpath is not set. Split out of
 * kageant_audit_setup() so the settings dialog can SHOW this path: the File
 * box is empty in the default case, and an empty box that reads "blank =
 * default" shows nobody where the file actually is.
 *
 * `create` is what separates the two callers. The setup path wants the
 * directory to exist because it is about to write there; the dialog only
 * wants the string, and must not create directories as a side effect of
 * being opened.
 */
/* An environment variable whole, NULL when not set or empty (sfree it). */
static char *kageant_env_dup(const char *name)
{
    DWORD need = GetEnvironmentVariableA(name, NULL, 0), n;
    int tries;
    for (tries = 0; need && tries < 4; tries++) {
        char *buf = snewn(need, char);
        n = GetEnvironmentVariableA(name, buf, need);
        if (n > 0 && n < need)
            return buf;
        sfree(buf);
        need = n;                            /* 0: gone; else it grew */
    }
    return NULL;
}

char *kageant_audit_default_path_dup(int create)
{
    const char *ini;
    char *base;

    /* A portable install logs beside its kitty.ini, so the whole install
     * stays on the stick. */
    if (!kitty_inifile_registry_authoritative() &&
        (ini = kitty_inifile_file()) != NULL) {
        const char *sl = strrchr(ini, '\\');
        if (sl)
            return dupprintf("%.*s\\kageant.log", (int)(sl - ini), ini);
    }
    if ((base = kageant_env_dup("LOCALAPPDATA")) != NULL) {
        char *dir = dupprintf("%s\\kapper.net", base), *out;
        if (create)
            CreateDirectoryA(dir, NULL);
        sfree(dir);
        dir = dupprintf("%s\\kapper.net\\KiTTY", base);
        if (create)
            CreateDirectoryA(dir, NULL);
        out = dupprintf("%s\\kageant.log", dir);
        sfree(dir);
        sfree(base);
        return out;
    }
    return NULL;
}

void kageant_audit_setup(void)
{
    char *path = kageant_audit_pathsetting_dup();
    if (!path)
        path = kageant_audit_default_path_dup(1);
    /* The on/off switch governs both sinks; a missing path only silences
     * the file (kitty_audit_enabled). */
    kitty_audit_set_sinks(kageant_audit_sink_get());
    kitty_audit_configure(
        path, kageant_audit_get(),
        kageant_audit_maxkb_get(), kageant_audit_keep_get(),
        kageant_audit_expire_get());
    sfree(path);
}

/* Best-effort requester identity for the log: the exe base name AND its
 * full path behind a pid - the base name is what the first-level view
 * shows, the full path is the audit-trail fact ("which ssh.exe?"). */
/* Both NULL when unknown; sfree them. */
static void kageant_req_name(unsigned long pid, char **base_out,
                             char **path_out)
{
    HANDLE h;
    *base_out = *path_out = NULL;
    if (!pid)
        return;
    h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (h) {
        char *path = kitty_process_image_path_dup(h);
        if (path && path[0]) {
            const char *base = strrchr(path, '\\');
            *base_out = dupstr(base ? base + 1 : path);
            *path_out = path;
        } else {
            sfree(path);
        }
        CloseHandle(h);
    }
}

/* Event 152: a key decrypted (result done, failed - a wrong passphrase -
 * or cancelled); trigger first-use (the prompt a signature opened, pid =
 * its requester) or gui (the key list's Decrypt). Never the passphrase. */
void kageant_audit_decrypt(const char *fp, const char *comment,
                           const char *result, const char *trigger,
                           unsigned long pid)
{
    char *req, *reqpath, pidbuf[16];
    kageant_req_name(pid, &req, &reqpath);
    snprintf(pidbuf, sizeof(pidbuf), "%lu", pid);
    kitty_audit("decrypt", "fp", fp, "comment", comment, "result", result,
                "trigger", trigger, "req", req, "reqpath", reqpath,
                "pid", pid ? pidbuf : NULL, (const char *)NULL);
    sfree(req);
    sfree(reqpath);
}

/* One sign/confirm-shaped audit line; pid 0 drops the requester fields. */
void kageant_audit_use(const char *ev, const char *fp,
                              const char *comment, const char *result,
                              const char *reason, unsigned long pid)
{
    char *req, *reqpath, pidbuf[16];
    kageant_req_name(pid, &req, &reqpath);
    snprintf(pidbuf, sizeof(pidbuf), "%lu", pid);
    kitty_audit(ev, "fp", fp, "comment", comment, "result", result,
                "reason", reason, "req", req, "reqpath", reqpath,
                "pid", pid ? pidbuf : NULL, (const char *)NULL);
    sfree(req);
    sfree(reqpath);
}
