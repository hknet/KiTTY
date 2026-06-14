/*
 * kitty_zmodem.c - ZModem file transfer for KiTTY on PuTTY 0.84.
 *
 * Ported from KiTTY 0.76b zmodem/winpzmodem.c, but reworked to the 0.84
 * no-global, multi-instance model and to avoid editing the shared terminal
 * library:
 *
 *   - 0.76b hooked the receive direction inside terminal.c term_data() and
 *     the send pump inside windows/window.c's message loop, using terminal
 *     struct fields (term->xyz_transfering / term->xyz_Internals). 0.84's
 *     term_data() lives in the shared guiterminal lib (compiled without
 *     MOD_PERSO) and the Terminal struct has no xyz fields.
 *   - Here the transfer state lives in this module (keyed by the active
 *     transfer; KiTTY is effectively single-window). The receive direction is
 *     intercepted in window.c's win_seat_output() (where backend bytes arrive
 *     before term_data) and the send direction is pumped from window.c's
 *     message loop - both window.c MOD_PERSO hooks, no terminal.c changes.
 *   - 0.76b's xyz_SpawnProcess() was a no-op stub (early return 0); this
 *     implements the real pipe+CreateProcess spawn so the helper actually runs.
 *
 * The helper programs (rz.exe / sz.exe from lrzsz) are supplied by the user via
 * CONF_rzcommand / CONF_szcommand, exactly as in KiTTY.
 */
#ifdef MOD_ZMODEM

#include <winsock2.h>	/* must precede windows.h (putty.h pulls winsock2 later) */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "putty.h"

#define ZM_PIPE_SIZE (64 * 1024)

/* Per-transfer state. KiTTY is single-window, so a single active transfer is
 * sufficient; the state is explicit (not a hidden global terminal field). */
typedef struct kitty_zmodem_state {
    int transfering;
    PROCESS_INFORMATION pi;
    HANDLE read_stdout;   /* we read helper stdout -> send to backend  */
    HANDLE read_stderr;   /* we read helper stderr -> write to terminal */
    HANDLE write_stdin;   /* we write backend bytes -> helper stdin     */
    Backend *backend;     /* the seat's backend, for backend_send       */
} kitty_zmodem_state;

/* The single active transfer (NULL when idle). */
static kitty_zmodem_state *zm_active = NULL;

int kitty_zmodem_active(void)
{
    return (zm_active && zm_active->transfering) ? 1 : 0;
}

static void zm_close_handles(kitty_zmodem_state *zm)
{
    if (zm->write_stdin) { CloseHandle(zm->write_stdin); zm->write_stdin = NULL; }
    if (zm->read_stdout) { CloseHandle(zm->read_stdout); zm->read_stdout = NULL; }
    if (zm->read_stderr) { CloseHandle(zm->read_stderr); zm->read_stderr = NULL; }
}

void kitty_zmodem_done(void)
{
    kitty_zmodem_state *zm = zm_active;
    if (!zm) return;
    if (zm->transfering) {
        DWORD exitcode = 0;
        if (zm->write_stdin) { CloseHandle(zm->write_stdin); zm->write_stdin = NULL; }
        Sleep(200);
        if (zm->read_stdout) { CloseHandle(zm->read_stdout); zm->read_stdout = NULL; }
        if (zm->read_stderr) { CloseHandle(zm->read_stderr); zm->read_stderr = NULL; }
        if (zm->pi.hProcess) {
            GetExitCodeProcess(zm->pi.hProcess, &exitcode);
            if (exitcode == STILL_ACTIVE)
                TerminateProcess(zm->pi.hProcess, 0);
            CloseHandle(zm->pi.hProcess);
            if (zm->pi.hThread) CloseHandle(zm->pi.hThread);
        }
    }
    zm->transfering = 0;
    zm_active = NULL;
    sfree(zm);
}

/* Spawn the helper (command + params), wiring its std handles to pipes.
 * Returns the new state on success, NULL on failure. workdir may be NULL. */
static kitty_zmodem_state *zm_spawn(const char *command, const char *params,
                                    const char *workdir, Backend *backend)
{
    STARTUPINFO si;
    SECURITY_ATTRIBUTES sa;
    HANDLE read_stdout = NULL, read_stderr = NULL, write_stdin = NULL;
    HANDLE newstdin = NULL, newstdout = NULL, newstderr = NULL;
    kitty_zmodem_state *zm;

    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    if (!CreatePipe(&newstdin, &write_stdin, &sa, ZM_PIPE_SIZE))
        return NULL;
    if (!CreatePipe(&read_stdout, &newstdout, &sa, ZM_PIPE_SIZE)) {
        CloseHandle(newstdin); CloseHandle(write_stdin); return NULL;
    }
    if (!CreatePipe(&read_stderr, &newstderr, &sa, ZM_PIPE_SIZE)) {
        CloseHandle(newstdin); CloseHandle(write_stdin);
        CloseHandle(newstdout); CloseHandle(read_stdout); return NULL;
    }

    /* Our ends must not be inherited by the child. */
    SetHandleInformation(write_stdin, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(read_stdout, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(read_stderr, HANDLE_FLAG_INHERIT, 0);

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = newstdin;
    si.hStdOutput = newstdout;
    si.hStdError = newstderr;

    zm = snew(kitty_zmodem_state);
    memset(zm, 0, sizeof(*zm));

    {
        char cmdline[2048];
        const char *base, *p;
        /* argv[0] = bare program name (lrzsz inspects it). */
        base = command;
        for (p = command; *p; p++)
            if (*p == '\\' || *p == '/' || *p == ':') base = p + 1;
        _snprintf(cmdline, sizeof(cmdline) - 1, "%s %s", base, params ? params : "");
        cmdline[sizeof(cmdline) - 1] = '\0';

        if (!CreateProcess(command, cmdline, NULL, NULL, TRUE,
                           CREATE_NEW_CONSOLE, NULL,
                           (workdir && workdir[0]) ? workdir : NULL,
                           &si, &zm->pi)) {
            CloseHandle(newstdin); CloseHandle(write_stdin);
            CloseHandle(newstdout); CloseHandle(read_stdout);
            CloseHandle(newstderr); CloseHandle(read_stderr);
            sfree(zm);
            return NULL;
        }
    }

    /* Close the child's ends in our process. */
    CloseHandle(newstdin);
    CloseHandle(newstdout);
    CloseHandle(newstderr);

    zm->write_stdin = write_stdin;
    zm->read_stdout = read_stdout;
    zm->read_stderr = read_stderr;
    zm->backend = backend;
    zm->transfering = 1;
    return zm;
}

static int existfile(const char *filename)
{
    FILE *f;
    if (!filename || !filename[0]) return 0;
    f = fopen(filename, "rb");
    if (f) { fclose(f); return 1; }
    return 0;
}

/* Start a ZModem RECEIVE (rz): the remote 'sz' has begun; we spawn rz and feed
 * it the incoming stream. Returns 1 on success. */
int kitty_zmodem_receive(Conf *conf, Backend *backend)
{
    const char *cmd = filename_to_str(conf_get_filename(conf, CONF_rzcommand));
    const char *opts = conf_get_str(conf, CONF_rzoptions);
    const char *dir = conf_get_str(conf, CONF_zdownloaddir);
    kitty_zmodem_state *zm;

    if (kitty_zmodem_active()) return 0;
    if (!existfile(cmd)) {
        char b[1024];
        _snprintf(b, sizeof(b) - 1, "Unable to find ZModem receive program:\n%s",
                  cmd ? cmd : "(unset)");
        MessageBox(NULL, b, "KiTTY ZModem", MB_OK | MB_ICONERROR);
        return 0;
    }
    zm = zm_spawn(cmd, opts, dir, backend);
    if (!zm) {
        MessageBox(NULL, "Unable to start ZModem receive.", "KiTTY ZModem",
                   MB_OK | MB_ICONERROR);
        return 0;
    }
    zm_active = zm;
    return 1;
}

/* Start a ZModem SEND (sz <files>): prompt for files, spawn sz. Returns 1. */
int kitty_zmodem_send(HWND owner, Conf *conf, Backend *backend)
{
    OPENFILENAME fn;
    static char filenames[32000];
    const char *cmd = filename_to_str(conf_get_filename(conf, CONF_szcommand));
    const char *opts = conf_get_str(conf, CONF_szoptions);
    const char *dir = conf_get_str(conf, CONF_zdownloaddir);
    kitty_zmodem_state *zm;
    char params[32767];
    char *p, *cur;

    if (kitty_zmodem_active()) return 0;
    if (!existfile(cmd)) {
        char b[1024];
        _snprintf(b, sizeof(b) - 1, "Unable to find ZModem send program:\n%s",
                  cmd ? cmd : "(unset)");
        MessageBox(NULL, b, "KiTTY ZModem", MB_OK | MB_ICONERROR);
        return 0;
    }

    memset(&fn, 0, sizeof(fn));
    memset(filenames, 0, sizeof(filenames));
    fn.lStructSize = sizeof(fn);
    fn.hwndOwner = owner;
    fn.lpstrFile = filenames;
    fn.nMaxFile = sizeof(filenames) - 1;
    fn.lpstrTitle = "Select file(s) to send by ZModem...";
    fn.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST |
               OFN_HIDEREADONLY | OFN_PATHMUSTEXIST;
    if (!GetOpenFileName(&fn))
        return 0;   /* user cancelled */

    cur = params;
    cur += sprintf(cur, "%s", opts ? opts : "");
    if (*(filenames + strlen(filenames) + 1) == 0) {
        /* single selection: filenames holds the full path */
        sprintf(cur, " \"%s\"", filenames);
    } else {
        /* multi: first field is the dir, then each file name */
        p = filenames;
        for (;;) {
            p = p + strlen(p) + 1;
            if (*p == 0) break;
            cur += sprintf(cur, " \"%s\\%s\"", filenames, p);
        }
    }

    zm = zm_spawn(cmd, params, dir, backend);
    if (!zm) {
        MessageBox(NULL, "Unable to start ZModem send.", "KiTTY ZModem",
                   MB_OK | MB_ICONERROR);
        return 0;
    }
    zm_active = zm;
    return 1;
}

void kitty_zmodem_cancel(void)
{
    kitty_zmodem_done();
}

/* RECEIVE direction: backend bytes arrive in win_seat_output(); while a
 * transfer is active, write them to the helper's stdin instead of the
 * terminal. Returns the byte count (so win_seat_output can return it). */
size_t kitty_zmodem_recv_data(const void *data, size_t len)
{
    kitty_zmodem_state *zm = zm_active;
    DWORD written = 0;
    if (!zm || !zm->transfering || !zm->write_stdin) return 0;
    WriteFile(zm->write_stdin, data, (DWORD)len, &written, NULL);
    return len;
}

/* SEND pump: called from window.c's message loop. Drains helper stdout to the
 * backend and stderr to nowhere (helper progress); detects helper exit and
 * finishes the transfer. Returns 1 if it did work (caller may loop). */
int kitty_zmodem_process(void)
{
    kitty_zmodem_state *zm = zm_active;
    char buf[1024];
    DWORD bread, avail;
    int did = 0;

    if (!zm || !zm->transfering) return 0;

    /* stdout -> backend */
    if (zm->read_stdout) {
        bread = 0; avail = 0;
        if (PeekNamedPipe(zm->read_stdout, buf, 1, &bread, &avail, NULL) && avail > 0) {
            if (ReadFile(zm->read_stdout, buf, sizeof(buf), &bread, NULL) && bread > 0) {
                if (zm->backend)
                    backend_send(zm->backend, buf, bread);
                did = 1;
            }
        }
    }
    /* stderr -> discard (helper diagnostics) */
    if (zm->read_stderr) {
        bread = 0; avail = 0;
        if (PeekNamedPipe(zm->read_stderr, buf, 1, &bread, &avail, NULL) && avail > 0) {
            if (ReadFile(zm->read_stderr, buf, sizeof(buf), &bread, NULL) && bread > 0)
                did = 1;
        }
    }

    /* Has the helper exited? */
    {
        DWORD exitcode = STILL_ACTIVE;
        if (zm->pi.hProcess)
            GetExitCodeProcess(zm->pi.hProcess, &exitcode);
        if (exitcode != STILL_ACTIVE && !did) {
            kitty_zmodem_done();
            return 1;
        }
    }
    return did;
}

#endif /* MOD_ZMODEM */
