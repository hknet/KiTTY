/*
 * kitty_agentcheck_console.c - the agent check of klink, kscp and ksftp: one
 * line on standard error (kitty_agentcheck.h). Linked into those three only:
 * it reads [KiTTY] verifyagent through their one reader of global keys
 * (kitty_console_global_read, kitty_winevent_cfg.c). kitty.exe has its own
 * hook in kitty_win.c.
 */
#include "putty.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "kitty_agentcheck.h"
#include "kitty_authenticode.h"   /* agent_serving_check_hook */
#include "kitty_inifile.h"        /* kitty_console_global_read */
#include "kitty_inikeys.h"        /* KI_VERIFYAGENT */
#include "kitty_text.h"           /* KT_CON_AGENT_* */

static const char *console_program = "klink";

/* [KiTTY] verifyagent where kitty.exe keeps it, through the console tools'
 * one reader (kitty_console_global_read). Only "no" turns the check off. */
static int console_check_off(void)
{
    char v[16];
    kitty_console_global_read(KI_VERIFYAGENT, v, sizeof(v));
    return !stricmp(v, "no");
}

static void console_check(unsigned long server_pid, int transport)
{
    char *what = NULL;
    unsigned long err;
    int verdict;
    if (console_check_off())
        return;
    /* the line is this program's Terminal Window's Event Log too: -v
     * prints that on standard error, where the line stands already */
    verdict = kitty_agentcheck_judge(server_pid, transport, &what, &err);
    if (verdict == KITTY_AGENT_UNVERIFIED)
        fprintf(stderr, KT_CON_AGENT_UNVERIFIED_FMT, console_program, what);
    else if (verdict == KITTY_AGENT_UNINSPECTED)
        fprintf(stderr, KT_CON_AGENT_UNINSPECTED_FMT, console_program, what);
    fflush(stderr);
    sfree(what);
}

void kitty_agentcheck_install_console(const char *program)
{
    console_program = program;
    agent_serving_check_hook = console_check;
}
