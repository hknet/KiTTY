/*
 * kitty_agentcheck.h - which program answers our SSH agent requests
 * ([KiTTY] verifyagent). Shared by kitty.exe and klink/kscp/ksftp.
 *
 * windows/agent-client.c calls agent_serving_check_hook with the answering
 * process's id BEFORE the first request leaves: on the pipe from
 * GetNamedPipeServerProcessId, on window messages from the owner of the
 * "Pageant" window. The hook judges that program here and warns: kitty.exe
 * with a notice, the console tools with a line on standard error. A warning,
 * never a refusal: the request goes on.
 */
#ifndef KITTY_AGENTCHECK_H
#define KITTY_AGENTCHECK_H

enum {
    KITTY_AGENT_VERIFIED = 0,   /* ours, or nothing to judge (see below) */
    KITTY_AGENT_UNVERIFIED,     /* *what = the program's full path */
    KITTY_AGENT_UNINSPECTED     /* *what = "process <pid>: Windows error <n>" */
};

/* Judge the program behind process `pid`. An unsigned build of ours (dev or
 * test) cannot demand a signed agent: VERIFIED. Test builds open that with
 * KITTY_TEST_AGENTCHECK=1 in the environment. *what is ours to free (sfree),
 * NULL for VERIFIED. *err: Windows' error number for UNINSPECTED, else 0.
 * Both other verdicts write their Windows Event Log record (226/326,
 * 227/327) when the program's switch has it on. */
int kitty_agentcheck_judge(unsigned long pid, int transport, char **what,
                           unsigned long *err);

/* klink/kscp/ksftp: install the hook that writes the warning to standard
 * error ("<program>: ..."). Reads [KiTTY] verifyagent from the store
 * kitty.exe writes it to. */
void kitty_agentcheck_install_console(const char *program);

#endif /* KITTY_AGENTCHECK_H */
