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
    /*
     * The helper's output is read through handle-io.c rather than polled.
     *
     * It used to be drained once per iteration of the main message loop, and
     * NOTHING WOKE THAT LOOP WHEN THE HELPER PRODUCED OUTPUT: the loop waits
     * with timeout INFINITE when idle. An UPLOAD therefore stalled - sz wrote
     * its file header into a pipe nobody drained, the far end retransmitted
     * ZRINIT until that traffic happened to wake us, and by then sz had given
     * up ("Retry 0: Got TIMEOUT", zero-byte file at the far end). A DOWNLOAD
     * never showed it, because the remote sends continuously and every packet
     * wakes the loop for free. hknet/KiTTY#33.
     *
     * handle_input_new puts each pipe on a reader subthread whose event is in
     * the loop's wait list, so data itself wakes us. Process exit likewise, via
     * add_handle_wait, instead of polling the exit code.
     */
    struct handle *h_stdout;
    struct handle *h_stderr;
    HandleWait *hw_process;
    bool eof_seen;        /* helper closed stdout: transfer is over */
    /*
     * The helper's stderr, line-buffered, on its way to the Event Log. It used
     * to be read and thrown away, which made a failed transfer mute while the
     * helper knew exactly what was wrong ("Retry 0: Got TIMEOUT" is what
     * eventually explained #33). The helper is the only thing here that can see
     * the protocol; not repeating what it says is throwing away the diagnosis.
     */
    LogContext *logctx;
    Terminal *term;       /* helper progress/errors go on screen, see below */
    char errline[256];
    size_t errlen;
} kitty_zmodem_state;

/* The single active transfer (NULL when idle). */
static kitty_zmodem_state *zm_active = NULL;

int kitty_zmodem_active(void)
{
    return (zm_active && zm_active->transfering) ? 1 : 0;
}

/* handle_free() first, then close the pipe: handle-io.c does not close the
 * underlying HANDLE, and its reader subthread must be told to stop before the
 * handle goes away underneath it (same order as conpty_terminate()). */
static void zm_close_handles(kitty_zmodem_state *zm)
{
    if (zm->h_stdout) { handle_free(zm->h_stdout); zm->h_stdout = NULL; }
    if (zm->h_stderr) { handle_free(zm->h_stderr); zm->h_stderr = NULL; }
    if (zm->hw_process) { delete_handle_wait(zm->hw_process); zm->hw_process = NULL; }
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
        zm_close_handles(zm);   /* readers off first, then the pipes */
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

/*
 * Helper stdout -> the session. Called on the main thread by handle-io.c once
 * its reader subthread has data, so arriving output is what wakes the message
 * loop rather than the loop happening to spin.
 *
 * len == 0 is EOF: the helper has closed stdout, i.e. the transfer is over.
 * The teardown is NOT done here - freeing the handle we are being called from
 * would pull the ground out from under handle-io.c - it is left to the process
 * exit callback, or to the next kitty_zmodem_process().
 */
static size_t zm_stdout_gotdata(struct handle *h, const void *data,
                                size_t len, int err)
{
    kitty_zmodem_state *zm = (kitty_zmodem_state *)handle_get_privdata(h);
    if (!zm || !zm->transfering)
        return 0;
    if (err || len == 0) {
        zm->eof_seen = true;
        return 0;
    }
    if (zm->backend)
        backend_send(zm->backend, data, len);
    return 0;   /* no backlog: we hand straight to the backend */
}

/* Emit one complete stderr line to the Event Log. Progress lines are dropped:
 * lrzsz repaints "Bytes Sent: ... BPS: ... ETA ..." with a bare CR many times a
 * second, and logging those would bury the one line that matters. */
static void zm_log_errline(kitty_zmodem_state *zm)
{
    if (zm->errlen == 0)
        return;
    zm->errline[zm->errlen] = '\0';
    if (zm->logctx && !strstr(zm->errline, "BPS:")) {
        char *msg = dupprintf("ZModem helper: %s", zm->errline);
        logevent(zm->logctx, msg);
        sfree(msg);
    }
    zm->errlen = 0;
}

/*
 * Helper stderr -> THE SCREEN, and complete lines also to the Event Log.
 *
 * On screen because that is where the user is looking while the transfer runs,
 * and because there is room: for the duration of a transfer the session's own
 * output is diverted to the helper (win_seat_output -> kitty_zmodem_recv_data),
 * so nothing else is drawing. lrzsz repaints "Bytes Sent: ... BPS: ... ETA ..."
 * with a bare CR, which is exactly a live progress line - and when it goes
 * wrong, "Retry 0: Got TIMEOUT" appears where the user can see it instead of
 * the transfer just doing nothing (hknet/KiTTY#33 was invisible for exactly
 * this reason). The Event Log keeps the non-progress lines as the record.
 *
 * Sanitised on the way: the helper is a program the user pointed us at, not a
 * trusted part of KiTTY, and its stderr should not be able to drive the display
 * with escape sequences. CR, LF, TAB and BS are what a progress line needs;
 * everything else below space is dropped.
 */
static size_t zm_stderr_gotdata(struct handle *h, const void *data,
                                size_t len, int err)
{
    kitty_zmodem_state *zm = (kitty_zmodem_state *)handle_get_privdata(h);
    const char *p = (const char *)data;
    char screen[512];
    size_t i, s = 0;

    if (!zm || !zm->transfering)
        return 0;
    if (err || len == 0) {          /* EOF: do not lose a last partial line */
        zm_log_errline(zm);
        return 0;
    }
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)p[i];

        if (zm->term) {
            if (c >= 0x20 || c == '\r' || c == '\n' || c == '\t' || c == '\b') {
                screen[s++] = (char)c;
                if (s == sizeof(screen)) {
                    term_data(zm->term, screen, s);
                    s = 0;
                }
            }
        }

        if (c == '\r' || c == '\n') {
            zm_log_errline(zm);
        } else if (zm->errlen < sizeof(zm->errline) - 1) {
            zm->errline[zm->errlen++] = (char)c;
        }
        /* over-long line: keep the head, drop the rest until the next break */
    }
    if (zm->term && s > 0)
        term_data(zm->term, screen, s);
    return 0;
}

/* The helper exited: finish the transfer. Safe to tear down here - this is a
 * different callback from the one delivering data. */
static void zm_process_exited(void *vctx)
{
    kitty_zmodem_state *zm = (kitty_zmodem_state *)vctx;
    if (zm != zm_active)
        return;
    kitty_zmodem_done();
}

/* Spawn the helper (command + params), wiring its std handles to pipes.
 * Returns the new state on success, NULL on failure. workdir may be NULL. */
static kitty_zmodem_state *zm_spawn(const char *command, const char *params,
                                    const char *workdir, Backend *backend,
                                    LogContext *logctx, Terminal *term)
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
    zm->logctx = logctx;
    zm->term = term;
    zm->transfering = 1;
    zm->eof_seen = false;
    zm->errlen = 0;

    /* Put both pipes and the process on the main loop's wait list, so the
     * helper having something to say is itself what wakes us (see the note on
     * the state struct - this is what an upload was missing). */
    zm->h_stdout = handle_input_new(read_stdout, zm_stdout_gotdata, zm, 0);
    zm->h_stderr = handle_input_new(read_stderr, zm_stderr_gotdata, zm, 0);
    zm->hw_process = add_handle_wait(zm->pi.hProcess, zm_process_exited, zm);
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
int kitty_zmodem_receive(Conf *conf, Backend *backend, LogContext *logctx, Terminal *term)
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
    zm = zm_spawn(cmd, opts, dir, backend, logctx, term);
    if (!zm) {
        MessageBox(NULL, "Unable to start ZModem receive.", "KiTTY ZModem",
                   MB_OK | MB_ICONERROR);
        return 0;
    }
    zm_active = zm;
    return 1;
}

/* Start a ZModem SEND (sz <files>): prompt for files, spawn sz. Returns 1. */
int kitty_zmodem_send(HWND owner, Conf *conf, Backend *backend, LogContext *logctx, Terminal *term)
{
    OPENFILENAME fn;
    static char filenames[32000];
    const char *cmd = filename_to_str(conf_get_filename(conf, CONF_szcommand));
    const char *opts = conf_get_str(conf, CONF_szoptions);
    const char *dir = conf_get_str(conf, CONF_zdownloaddir);
    kitty_zmodem_state *zm;
    char params[32767];
    char *p, *cur;
    char *senddir = "";   /* directory the chosen files live in (see below) */

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
    {
        size_t off = 0, room;
        int n;
        room = sizeof(params) - off;
        n = snprintf(cur, room, "%s", opts ? opts : "");
        if (n > 0) { if ((size_t)n >= room) n = (int)room - 1; off += n; cur += n; }
        if (*(filenames + strlen(filenames) + 1) == 0) {
            /* Single selection: filenames holds the full path. Split it, and
             * pass the NAME only - see the senddir note below. */
            char *slash = strrchr(filenames, '\\');
            if (slash) {
                *slash = '\0';                    /* filenames -> directory */
                senddir = filenames;
                p = slash + 1;
            } else {
                p = filenames;                    /* no directory part */
            }
            room = sizeof(params) - off;
            snprintf(cur, room, " \"%s\"", p);
        } else {
            /* multi: first field is the dir, then each file name */
            senddir = filenames;
            p = filenames;
            for (;;) {
                p = p + strlen(p) + 1;
                if (*p == 0) break;
                room = sizeof(params) - off;
                if (room <= 1) break;   /* no space left */
                n = snprintf(cur, room, " \"%s\"", p);
                if (n <= 0) break;
                if ((size_t)n >= room) { off = sizeof(params) - 1; cur = params + off; break; }
                off += n; cur += n;
            }
        }
    }

    /*
     * Run the helper IN the directory the files came from, and give it bare
     * names.
     *
     * ZMODEM carries the name the sender was given, and the receiver is a Unix
     * box: a full Windows path is not a path to it, just an unusual filename -
     * backslashes are ordinary characters, so nothing is stripped and the file
     * lands as "C:\Users\...\thing.bin" in the remote directory. (rz strips
     * UNIX directory components, which is why this never showed up when the
     * helper was handed a /mnt/c/... path.)
     *
     * The old working directory was CONF_zdownloaddir, which belongs to the
     * RECEIVE direction and has nothing to do with where an uploaded file
     * lives.
     */
    zm = zm_spawn(cmd, params, senddir[0] ? senddir : dir, backend, logctx, term);
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

/*
 * Called from window.c's message loop. It no longer PUMPS anything: the
 * helper's output reaches the session from zm_stdout_gotdata() the moment it
 * arrives, and the exit is caught by zm_process_exited(). What is left is the
 * one case those two do not cover - the helper closed stdout but has not exited
 * yet - so the transfer is not left open until it does.
 */
int kitty_zmodem_process(void)
{
    kitty_zmodem_state *zm = zm_active;
    if (!zm || !zm->transfering) return 0;
    if (zm->eof_seen) {
        kitty_zmodem_done();
        return 1;
    }
    return 0;
}

#endif /* MOD_ZMODEM */
