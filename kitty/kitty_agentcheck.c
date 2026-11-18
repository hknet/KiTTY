/*
 * kitty_agentcheck.c - which program answers our SSH agent requests. See
 * kitty_agentcheck.h.
 */
#include "putty.h"
#include <windows.h>
#include <string.h>

#include "kitty_agentcheck.h"
#include "kitty_authenticode.h"   /* the transports */
#include "kitty_renameguard.h"    /* kitty_signature_reading */
#include "kitty_selfcheck.h"      /* kitty_selfcheck_file */
#include "kitty_oldwin.h"         /* kitty_module_path_dup, kitty_process_image_path_dup */
#include "kitty_text.h"           /* KT_AGENT_PROCESS_ERROR_FMT */
#include "kitty_buildlabel.h"     /* KITTY_TEST_BUILD_LABEL: the test door */
#include "kitty_winevent.h"       /* events 226/326, 227/327 */

#ifndef PROCESS_QUERY_LIMITED_INFORMATION
#define PROCESS_QUERY_LIMITED_INFORMATION 0x1000
#endif

/* Is a file ours? Authenticode first (kitty_signature_reading: offline, the
 * publisher's name pinned). Otherwise the release's integrity stamp
 * (kitty_selfcheck_file: Ed25519 with this release's key, our own code, so
 * it runs where Windows cannot judge our SHA-256 signature, as on Windows
 * XP). The stamp covers the file's bytes: a modified file
 * fails both. 1 ours, 0 not. */
static int agentcheck_ours(const char *path)
{
    char reason[64];
    if (kitty_signature_reading(path, NULL, 0) == KG_SIG_OURS)
        return 1;
    return kitty_selfcheck_file(path, reason, sizeof(reason), NULL, 0) == 0;
}

/* Is this binary ours? Once per process: it hashes the whole file. A dev or
 * test build is not. It cannot demand that the agent be ours. */
static int agentcheck_own_signed(void)
{
    static int own = -1;
    if (own < 0) {
        char *self = kitty_module_path_dup(NULL);
        own = (self && agentcheck_ours(self)) ? 1 : 0;
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
    int window = transport;   /* the record's transport (kitty_winevent.c) */

    *what = NULL;
    *err = 0;
    if (!agentcheck_own_signed())
        return KITTY_AGENT_VERIFIED;
    /* PROCESS_QUERY_LIMITED_INFORMATION reaches an elevated agent from a
     * normal one. Windows XP does not know
     * that right. Its path lookup (GetModuleFileNameEx in
     * kitty_process_image_path_dup) reads the process: QUERY_INFORMATION
     * with VM_READ there, as kitty_broadcast.c opens it (with
     * QUERY_INFORMATION alone the lookup fails with error 5). */
    h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (!h)
        h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE,
                        (DWORD)pid);
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
    if (agentcheck_ours(path)) {
        sfree(path);
        return KITTY_AGENT_VERIFIED;
    }
    kitty_wev_agent_unverified(path, window);
    *what = path;
    return KITTY_AGENT_UNVERIFIED;
}
