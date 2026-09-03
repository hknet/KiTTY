/*
 * kitty_workplace.c: the arming channel of workplace proxy mode
 * (design/TASK_workplace_proxy.md §3, §4).
 *
 * Workplace proxy mode is a MODE, not a property of a named proxy: while it is
 * on, every connection this install starts goes through one chosen proxy,
 * whatever the session itself stores. The state of that mode is not stored
 * anywhere. It is a small shared section that the launcher creates when the
 * mode is switched on and holds open for as long as it is armed:
 *
 *   - the launcher going away releases it, so the mode is off - no shutdown
 *     handling, nothing to clean up, nothing that can be left behind;
 *   - the indicator (the tray icon) and the authority (this section) are the
 *     same process, so they cannot disagree;
 *   - a connection asking costs one OpenFileMapping. It never waits on the
 *     launcher, so a hung launcher slows no startup down and every failure
 *     answers "not armed".
 *
 * ⛔ Launcher running is NOT armed. People run the launcher all day for the
 * session list and hotkeys; the question asked here is "are you holding an
 * arming right now", which is why the arming is a separate object from the
 * launcher's window and is created only by switching the mode on.
 *
 * The section name is keyed to the INSTALL - the hash of the directory the
 * running EXE sits in - so a portable KiTTY on a stick and an installed one
 * cannot arm each other. That is the identify-by-target-path rule
 * kitty_inilight.c already follows; do not add a second scheme.
 *
 * Deliberately dependency-free (windows.h and the CRT only), like
 * kitty_inilight.c: it is called from the launcher, from the connection path
 * and from the config box, none of which should have to agree on headers.
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "kitty_workplace.h"
#include "kitty_notice.h"
#include "kitty_text.h"     /* the notice wording */

#include "kitty_oldwin.h"   /* APIs newer than the oldest Windows we load on */
#define KWP_MAGIC   0x5057494Bu   /* "KIWP" */
#define KWP_VERSION 1

struct kwp_record {
    DWORD magic;
    DWORD version;
    DWORD pid;              /* the process holding the arming */
    DWORD reserved;
    ULONGLONG expiry;       /* FILETIME, UTC; 0 = no timeout */
    char proxy[256];        /* name of the named proxy in force */
};

/* Held for as long as this process is armed. Closing the handle - or dying -
 * destroys the section, which is exactly how the mode switches itself off. */
static HANDLE kwp_map = NULL;
static struct kwp_record *kwp_view = NULL;

/* Directory of the running EXE, lowercased, without a trailing slash. */
static int kwp_exedir(char *out, int len)
{
    char path[MAX_PATH + 1];
    DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    char *slash;
    if (n == 0 || n >= MAX_PATH)
        return 0;
    path[n] = '\0';
    slash = strrchr(path, '\\');
    if (!slash)
        return 0;
    *slash = '\0';
    if ((int)strlen(path) >= len)
        return 0;
    strcpy(out, path);
    CharLowerA(out);
    return 1;
}

/* FNV-1a over the install directory: an identifier, not a secret. */
static const char *kwp_section_name(void)
{
    static char name[64];
    char dir[MAX_PATH + 1];
    unsigned int h = 2166136261u;
    const char *p;
    if (name[0])
        return name;
    if (!kwp_exedir(dir, sizeof(dir)))
        dir[0] = '\0';
    for (p = dir; *p; p++) {
        h ^= (unsigned char)*p;
        h *= 16777619u;
    }
    /* Local\ = this logon session only. */
    snprintf(name, sizeof(name), "Local\\KiTTYWorkplaceArming.%08x", h);
    return name;
}

static ULONGLONG kwp_now(void)
{
    FILETIME ft;
    ULARGE_INTEGER u;
    GetSystemTimeAsFileTime(&ft);
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart;
}

/* The holder must be an EXE from OUR install directory. Without this, any
 * process in the logon session could publish an arming and route every
 * connection through a proxy of its choosing. Anything we cannot check
 * answers no. */
static int kwp_holder_is_ours(DWORD pid)
{
    char ours[MAX_PATH + 1], theirs[MAX_PATH + 1];
    DWORD sz = MAX_PATH;
    HANDLE h;
    char *slash;
    int ok = 0;
    if (pid == GetCurrentProcessId())
        return 1;
    if (!kwp_exedir(ours, sizeof(ours)))
        return 0;
    h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h)
        return 0;
    if (kitty_process_image_path(h, theirs, sz) && sz > 0) {
        theirs[sz] = '\0';
        if ((slash = strrchr(theirs, '\\')) != NULL) {
            *slash = '\0';
            CharLowerA(theirs);
            ok = !strcmp(ours, theirs);
        }
    }
    CloseHandle(h);
    return ok;
}

int kitty_workplace_holding(void)
{
    return kwp_view != NULL;
}

int kitty_workplace_arm(const char *proxyname, unsigned int minutes)
{
    HANDLE map;
    struct kwp_record *view;

    if (!proxyname || !proxyname[0])
        return 0;
    if (strlen(proxyname) >= sizeof(kwp_view->proxy))
        return 0;

    if (kwp_view) {                     /* re-arm in place: same holder */
        strcpy(kwp_view->proxy, proxyname);
        kwp_view->expiry = minutes ? kwp_now() + (ULONGLONG)minutes * 600000000ull : 0;
        return 1;
    }

    map = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
                             sizeof(struct kwp_record), kwp_section_name());
    if (!map)
        return 0;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        /* Somebody else already holds an arming for this install. Two holders
         * would mean two answers to one question - refuse rather than guess. */
        CloseHandle(map);
        return 0;
    }
    view = (struct kwp_record *)MapViewOfFile(map, FILE_MAP_WRITE, 0, 0,
                                              sizeof(struct kwp_record));
    if (!view) {
        CloseHandle(map);
        return 0;
    }
    memset(view, 0, sizeof(struct kwp_record));
    view->pid = GetCurrentProcessId();
    view->expiry = minutes ? kwp_now() + (ULONGLONG)minutes * 600000000ull : 0;
    strcpy(view->proxy, proxyname);
    /* magic LAST: a reader that catches the section mid-creation sees no magic
     * and answers "not armed", never a half-written proxy name. */
    view->version = KWP_VERSION;
    view->magic = KWP_MAGIC;

    kwp_map = map;
    kwp_view = view;
    return 1;
}

void kitty_workplace_disarm(void)
{
    if (kwp_view) {
        kwp_view->magic = 0;
        UnmapViewOfFile(kwp_view);
        kwp_view = NULL;
    }
    if (kwp_map) {
        CloseHandle(kwp_map);
        kwp_map = NULL;
    }
}

unsigned int kitty_workplace_minutes_left(void)
{
    ULONGLONG expiry = 0, now;
    if (kwp_view) {
        expiry = kwp_view->expiry;
    } else {
        char name[sizeof(kwp_view->proxy)];
        if (!kitty_workplace_query(name, sizeof(name)))
            return 0;
        /* query() validated the record; read the expiry the same way */
        HANDLE map = OpenFileMappingA(FILE_MAP_READ, FALSE, kwp_section_name());
        if (map) {
            struct kwp_record *v = (struct kwp_record *)
                MapViewOfFile(map, FILE_MAP_READ, 0, 0, sizeof(struct kwp_record));
            if (v) {
                expiry = v->expiry;
                UnmapViewOfFile(v);
            }
            CloseHandle(map);
        }
    }
    if (!expiry)
        return 0;
    now = kwp_now();
    if (expiry <= now)
        return 0;
    return (unsigned int)((expiry - now) / 600000000ull) + 1;
}

/* Install-keyed so a broadcast reaches only launchers of this install - the
 * same rule the section name follows. RegisterWindowMessage on the same string
 * gives every process the same id. */
unsigned int kitty_workplace_message(void)
{
    static UINT msg = 0;
    if (!msg) {
        char name[96];
        snprintf(name, sizeof(name), "%s.Request", kwp_section_name());
        msg = RegisterWindowMessageA(name);
    }
    return msg;
}

/* Wait for the arming to appear or go, rather than for a reply: the arming IS
 * the state, so nothing has to be trusted to answer. Off the startup path (the
 * config box and the tray), so a few hundred ms of waiting is affordable here
 * in a way it would not be in start_backend. */
static int kwp_wait_for(int armed, int ms)
{
    char name[256];
    int waited = 0;
    for (;;) {
        if (kitty_workplace_query(name, sizeof(name)) == (armed ? 1 : 0))
            return 1;
        if (waited >= ms)
            return 0;
        Sleep(25);
        waited += 25;
    }
}

int kitty_workplace_request(int arm, unsigned int minutes)
{
    UINT msg = kitty_workplace_message();
    if (!msg)
        return 0;
    /* wParam says on/off, lParam carries the minutes - the launcher needs both
     * and a registered message has nowhere else to put them. */
    PostMessageA(HWND_BROADCAST, msg, arm ? 1 : 0, (LPARAM)minutes);
    return kwp_wait_for(arm, 750);
}

void kitty_workplace_left_text(char *out, int len)
{
    unsigned int m = kitty_workplace_minutes_left();
    if (len <= 0)
        return;
    out[0] = '\0';
    if (!m)
        return;
    if (m >= 60 && m % 60 == 0)
        snprintf(out, len, "%u h", m / 60);
    else if (m >= 60)
        snprintf(out, len, "%u h %u min", m / 60, m % 60);
    else
        snprintf(out, len, "%u min", m);
}

int kitty_workplace_start_launcher(const char *proxyname, unsigned int minutes)
{
    char exe[MAX_PATH + 1], cmd[MAX_PATH + 320];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    DWORD n;
    if (!proxyname || !proxyname[0] || strchr(proxyname, '"'))
        return 0;
    n = GetModuleFileNameA(NULL, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return 0;
    exe[n] = '\0';
    snprintf(cmd, sizeof(cmd), "\"%s\" -launcher -workplace \"%s\" -workplaceminutes %u",
             exe, proxyname, minutes);
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
        return 0;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    /* A launcher has a registry hive and a session tree to read before it takes
     * the arming, so allow rather more time than a message to a running one. */
    return kwp_wait_for(1, 4000);
}

/* ---- "the mode is not on any more", said once and only when it is news ---- */

/* kitty.c: the suite's global-parameter store (registry, or kitty.ini in
 * portable mode). Declared rather than included so this file keeps its short
 * include list; it is compiled only into the KiTTY targets. */
extern int WriteParameter(const char *key, const char *name, char *value);
extern int ReadParameterN(const char *key, const char *name, char *value, size_t size);
#ifndef INIT_SECTION
#define INIT_SECTION "KiTTY"
#endif
#define KWP_BREADCRUMB "WorkplaceWasArmed"

void kitty_workplace_mark_armed(void)
{
    WriteParameter(INIT_SECTION, KWP_BREADCRUMB, "1");
}

void kitty_workplace_notice_settled(void)
{
    WriteParameter(INIT_SECTION, KWP_BREADCRUMB, "0");
}

int kitty_workplace_notice_owed(void)
{
    char buf[16] = "", name[256];
    if (!ReadParameterN(INIT_SECTION, KWP_BREADCRUMB, buf, sizeof(buf)))
        return 0;
    if (strcmp(buf, "1"))
        return 0;
    /* Still armed: nothing has ended, so nothing is owed. */
    return kitty_workplace_query(name, sizeof(name)) ? 0 : 1;
}

void kitty_workplace_show_pending_notice(void)
{
    if (!kitty_workplace_notice_owed())
        return;
    kitty_workplace_notice_settled();      /* first, so two paths cannot both show it */
    /* No click action: the mode stopped because the thing holding it went away,
     * and somebody who wants it back knows where the switch is. Offering to
     * switch it on here would be guessing that they do. */
    kitty_notice_show(KT_CAP_WORKPLACE_INACTIVE,
                      KT_WORKPLACE_INACTIVE_TEXT,
                      RGB(0, 100, 0), 15, NULL, 0);
}

int kitty_workplace_query(char *name, int len)
{
    HANDLE map;
    struct kwp_record *view;
    int armed = 0;

    if (name && len > 0)
        name[0] = '\0';
    if (!name || len <= 0)
        return 0;

    map = OpenFileMappingA(FILE_MAP_READ, FALSE, kwp_section_name());
    if (!map)
        return 0;                       /* nobody is holding one: not armed */
    view = (struct kwp_record *)MapViewOfFile(map, FILE_MAP_READ, 0, 0,
                                              sizeof(struct kwp_record));
    if (view) {
        if (view->magic == KWP_MAGIC && view->version == KWP_VERSION &&
            view->proxy[0] &&
            memchr(view->proxy, '\0', sizeof(view->proxy)) != NULL &&
            (!view->expiry || view->expiry > kwp_now()) &&
            kwp_holder_is_ours(view->pid)) {
            if ((int)strlen(view->proxy) < len) {
                strcpy(name, view->proxy);
                armed = 1;
            }
        }
        UnmapViewOfFile(view);
    }
    CloseHandle(map);
    return armed;
}
