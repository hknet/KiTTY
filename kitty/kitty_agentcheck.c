/*
 * kitty_agentcheck.c - which program answers our SSH agent requests. See
 * kitty_agentcheck.h.
 */
#include "putty.h"
#include <windows.h>
#include <string.h>

#include "kitty_agentcheck.h"
#include "kitty_authenticode.h"   /* kitty_authenticode_verify_offline */
#include "kitty_oldwin.h"         /* kitty_module_path_dup, kitty_process_image_path_dup */
#include "kitty_text.h"           /* KT_AGENT_PROCESS_ERROR_FMT */
#include "kitty_buildlabel.h"     /* KITTY_TEST_BUILD_LABEL: the test door */
#include "kitty_winevent.h"       /* events 226/326, 227/327 */

#ifndef PROCESS_QUERY_LIMITED_INFORMATION
#define PROCESS_QUERY_LIMITED_INFORMATION 0x1000
#endif

/* Is this binary signed by us? Once per process: it hashes the whole file. */
static int agentcheck_own_signed(void)
{
    static int own = -1;
    if (own < 0) {
        char *self = kitty_module_path_dup(NULL);
        own = self ? kitty_authenticode_verify_offline(self) : 0;
        sfree(self);
#ifdef KITTY_TEST_BUILD_LABEL
        /* Test builds only: KITTY_TEST_AGENTCHECK=1 judges as a signed build
         * would - test builds carry no signature, so nothing else reaches
         * the check. */
        if (!own) {
            char v[8];
            DWORD n = GetEnvironmentVariableA("KITTY_TEST_AGENTCHECK", v,
                                              sizeof(v));
            if (n && n < sizeof(v) && !strcmp(v, "1"))
                own = 1;
        }
#endif
    }
    return own;
}

int kitty_agentcheck_judge(unsigned long pid, int transport, char **what,
                           unsigned long *err)
{
    HANDLE h;
    char *path;
    int window = (transport == KITTY_AGENT_TRANSPORT_WMCOPYDATA);

    *what = NULL;
    *err = 0;
    if (!agentcheck_own_signed())
        return KITTY_AGENT_VERIFIED;
    /* PROCESS_QUERY_LIMITED_INFORMATION reaches an elevated agent from a
     * normal one (measured on Windows 11 25H2). Windows XP does not know
     * that right: PROCESS_QUERY_INFORMATION there. */
    h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (!h)
        h = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, (DWORD)pid);
    if (!h) {
        *err = GetLastError();
        *what = dupprintf(KT_AGENT_PROCESS_ERROR_FMT, pid, *err);
        kitty_wev_agent_uninspected(pid, *err, window);
        return KITTY_AGENT_UNINSPECTED;
    }
    path = kitty_process_image_path_dup(h);
    if (!path) {
        *err = GetLastError();
        CloseHandle(h);
        *what = dupprintf(KT_AGENT_PROCESS_ERROR_FMT, pid, *err);
        kitty_wev_agent_uninspected(pid, *err, window);
        return KITTY_AGENT_UNINSPECTED;
    }
    CloseHandle(h);
    if (kitty_authenticode_verify_offline(path)) {
        sfree(path);
        return KITTY_AGENT_VERIFIED;
    }
    kitty_wev_agent_unverified(path, window);
    *what = path;
    return KITTY_AGENT_UNVERIFIED;
}
