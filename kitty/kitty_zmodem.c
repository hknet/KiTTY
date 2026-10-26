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
#include "kitty.h"      /* INIT_SECTION: the rz/sz paths are kitty.ini
                         * settings, not per-session conf keys */
#include "kitty_params.h"   /* ReadParameterDup */
#include <commdlg.h>        /* CommDlgExtendedError: the upload picker grows */
#include "kitty_msgbox.h"   /* themed MessageBox routing */
#include "kitty_text.h"     /* shared captions and wordings */
#include "kitty_inikeys.h"  /* KI_*: the kitty.ini key names */
#include "kitty_zmodem.h"
#include "kitty_tools.h"    /* existfile: the helper paths must name a file */
#include "kitty_winutil.h"  /* kitty_expand_env_dup: %VAR% in the rz/sz paths */
#include "kitty_notice.h"   /* zm_report: the notice window */
#include "kitty_winevent.h" /* events 240/241: the transfer's records */
#include "kitty_oldwin.h"   /* kitty_api: the change time (Vista on) */

#define ZM_PIPE_SIZE (64 * 1024)
/* Longest stderr line kept for the Event Log: only a bound on memory against
 * a helper that never ends a line - a real line is far shorter. */
#define ZM_ERRLINE_MAX (1024 * 1024)

/* Per-transfer state. KiTTY is single-window, so a single active transfer is
 * sufficient; the state is explicit (not a hidden global terminal field). */
typedef struct kitty_zmodem_state {
    int transfering;
    int send;             /* 1 = sz (files leaving), 0 = rz (files arriving) */
    char *dir;            /* what the finished-transfer balloon opens: the
                           * download folder (rz), the upload folder (sz) */
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
     * the protocol; not repeating what it prints is throwing away the diagnosis.
     */
    LogContext *logctx;
    Terminal *term;       /* helper progress/errors go on screen, see below */
    strbuf *errline;      /* the stderr line being collected */
    /* events 240/241, one per file: an upload's files (full path, the name
     * sz sends, size); a download's start (its files are found in the
     * download folder afterwards) and the names rz announced; the helper's
     * last message line, the reason of a failure */
    char **wev_paths, **wev_names;
    long long *wev_sizes;
    int wev_n;
    ULONGLONG wev_start;
    char **wev_ann;
    int wev_nann;
    char *wev_user;
    /* the transfer's end is recorded (240/241) and its notification
     * decided - once, by whichever path ends it; end_ok: the helper
     * finished its transfer (exit code 0), the one case notified */
    int ended, end_ok;
    char *lasterr;        /* NULL until the helper printed a line */
} kitty_zmodem_state;

/* The single active transfer (NULL when idle). */
static kitty_zmodem_state *zm_active = NULL;

int kitty_zmodem_active(void)
{
    return (zm_active && zm_active->transfering) ? 1 : 0;
}

/*
 * Where rz and sz live on THIS PC: kitty.ini [KiTTY] rzcommand / szcommand,
 * edited on Application > KiTTY Settings > Transfers & Tools > ZModem.
 *
 * They used to be per-session (CONF_rzcommand / CONF_szcommand), which wrote
 * the path of a program installed on this machine into every saved session
 * and made you set it again for each host. The OPTIONS stay per session -
 * those are arguments for a particular remote, not a property of the install.
 *
 * Held in full, in memory kept per direction until the next call; every
 * caller is finished with it before then. %VAR% Windows variables in the
 * stored path are expanded here, as for the other helper paths
 * (cyd01/KiTTY#472); the setting keeps them as typed, and an unknown %name%
 * stays as it is. Every caller uses the result as a path to test or start,
 * never to show or store.
 */
static const char *zmodem_expanded(char **slot, char *stored)
{
    sfree(*slot);
    if (stored[0]) {
        char *expanded = kitty_expand_env_dup(stored);
        *slot = dupstr(expanded ? expanded : stored);
        free(expanded);
        sfree(stored);
    } else {
        *slot = stored;
    }
    return *slot;
}

const char *kitty_zmodem_command(int send)
{
    static char *rz = NULL, *sz = NULL;

    /* Each key spelt out at its own call, not selected by a ternary: the
     * kitty.ini drift check reads the source for the literal beside the
     * ReadParameter, and a key it cannot see there is reported as one that
     * nothing reads. */
    if (send)
        return zmodem_expanded(&sz, ReadParameterDup(INIT_SECTION, KI_SZCOMMAND));
    return zmodem_expanded(&rz, ReadParameterDup(INIT_SECTION, KI_RZCOMMAND));
}

/* handle_free() first, then close the pipe: handle-io.c does not close the
 * underlying HANDLE, and its reader subthread must be made to stop before the
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

static void zm_end(kitty_zmodem_state *zm, DWORD exitcode);

static void kitty_zmodem_done(void)
{
    kitty_zmodem_state *zm = zm_active;
    int notify, send;
    char *dir;
    if (!zm) return;
    /* The transfer's end (events 240/241, the notification) when the
     * helper's exit callback has not recorded it: the message loop ends a
     * transfer here once the helper closed its stdout
     * (kitty_zmodem_process), which is usually BEFORE the exit callback
     * runs - and that callback then finds no active transfer. The helper
     * gets a moment to exit, for its exit code. */
    if (zm->transfering && !zm->ended && zm->pi.hProcess) {
        DWORD exitcode = 1;
        WaitForSingleObject(zm->pi.hProcess, 2000);
        if (!GetExitCodeProcess(zm->pi.hProcess, &exitcode))
            exitcode = 1;
        zm_end(zm, exitcode);
    }
    /* copied out: the tear-down frees the state */
    notify = zm->ended && zm->end_ok;
    send = zm->send;
    dir = dupstr(zm->dir ? zm->dir : "");
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
    sfree(zm->dir);
    {
        int i;
        for (i = 0; i < zm->wev_n; i++) {
            sfree(zm->wev_paths[i]);
            sfree(zm->wev_names[i]);
        }
        for (i = 0; i < zm->wev_nann; i++)
            sfree(zm->wev_ann[i]);
        sfree(zm->wev_paths);
        sfree(zm->wev_names);
        sfree(zm->wev_sizes);
        sfree(zm->wev_ann);
    }
    sfree(zm->wev_user);
    sfree(zm->lasterr);
    if (zm->errline) strbuf_free(zm->errline);
    sfree(zm);
    /* [KiTTY] transfernotification: only for a helper that finished its
     * transfer; rz does not count its files */
    if (notify)
        kitty_xfer_notify(send ? KT_XFER_WHAT_ZMODEM_SEND : KT_XFER_WHAT_ZMODEM_RECV,
                          !send, 0, dir);
    sfree(dir);
}

static ULONGLONG zm_ft(FILETIME ft)
{
    return ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

/* The latest of a file's creation, last-write and (Vista on) change times:
 * rz may stamp a file with the sender's modification time, and a file it
 * overwrote keeps its creation time - the change time moves either way. */
typedef BOOL (WINAPI *zm_gfibhe_t)(HANDLE, int, LPVOID, DWORD);
static ULONGLONG zm_touched(const char *path, long long *size)
{
    static zm_gfibhe_t p_gfibhe;
    static int resolved;
    WIN32_FILE_ATTRIBUTE_DATA fad;
    ULONGLONG t = 0;
    *size = -1;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fad))
        return 0;
    *size = ((long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    t = zm_ft(fad.ftCreationTime);
    if (zm_ft(fad.ftLastWriteTime) > t)
        t = zm_ft(fad.ftLastWriteTime);
    if (!resolved) {
        p_gfibhe = (zm_gfibhe_t)kitty_api("kernel32.dll",
                                          "GetFileInformationByHandleEx",
                                          KITTY_API_OPTIONAL,
                                          KT_WINFEAT_FILE_CHANGE_TIME);
        resolved = 1;
    }
    if (p_gfibhe) {
        /* FILE_BASIC_INFO: four LARGE_INTEGER times (ChangeTime the
         * fourth) and the attributes; FileBasicInfo = 0 */
        struct { LARGE_INTEGER cr, la, lw, ch; DWORD attrs; } bi;
        HANDLE h = CreateFileA(path, FILE_READ_ATTRIBUTES,
                               FILE_SHARE_READ | FILE_SHARE_WRITE |
                               FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            if (p_gfibhe(h, 0, &bi, sizeof(bi)) &&
                (ULONGLONG)bi.ch.QuadPart > t)
                t = (ULONGLONG)bi.ch.QuadPart;
            CloseHandle(h);
        }
    }
    return t;
}

/* Events 240/241 for the transfer that ends now, one per file. An upload:
 * each file picked, with its size when it went through. A download: each
 * file in the download folder touched since the receive started - full
 * path and size - with the name rz announced for it when it announced one.
 * A failed download that left nothing gets one 241 naming the folder. */
static void zm_wev(kitty_zmodem_state *zm, int ok, const char *reason)
{
    const char *user;
    int i;
    if (!zm || !kitty_wev_enabled())
        return;
    user = zm->wev_user ? zm->wev_user : "";
    if (zm->send) {
        for (i = 0; i < zm->wev_n; i++)
            kitty_wev_term_transfer(ok, user, "zmodem", "put",
                                    zm->wev_paths[i], zm->wev_names[i],
                                    ok ? zm->wev_sizes[i] : -1, reason);
        return;
    } else {
        WIN32_FIND_DATAA fd;
        HANDLE fh;
        int found = 0;
        char *pat;
        if (!zm->dir || !*zm->dir)
            return;
        pat = dupprintf("%s\\*", zm->dir);
        fh = FindFirstFileA(pat, &fd);
        sfree(pat);
        if (fh != INVALID_HANDLE_VALUE) {
            do {
                char *full;
                long long size;
                const char *remote = "";
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                    continue;
                full = dupprintf("%s\\%s", zm->dir, fd.cFileName);
                if (zm_touched(full, &size) >= zm->wev_start) {
                    for (i = 0; i < zm->wev_nann; i++)
                        if (!stricmp(zm->wev_ann[i], fd.cFileName))
                            remote = zm->wev_ann[i];
                    kitty_wev_term_transfer(ok, user, "zmodem", "get", full,
                                            remote, size, reason);
                    found++;
                }
                sfree(full);
            } while (FindNextFileA(fh, &fd));
            FindClose(fh);
        }
        if (!found && !ok)
            kitty_wev_term_transfer(0, user, "zmodem", "get", zm->dir, "", -1,
                                    reason);
    }
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
    const char *line;
    if (!zm->errline || zm->errline->len == 0)
        return;
    line = zm->errline->s;
    if (zm->logctx && !strstr(line, "BPS:")) {
        char *msg = dupprintf(KT_ZM_HELPER_LOG, line);
        /* event 241's reason: the last line the helper printed */
        sfree(zm->lasterr);
        zm->lasterr = dupstr(line);
        /* event 240's remote: the name rz announces (lrzsz with -v) */
        if (!zm->send && !strncmp(line, "Receiving: ", 11) &&
            zm->wev_nann < 4096) {
            zm->wev_ann = sresize(zm->wev_ann, zm->wev_nann + 1, char *);
            zm->wev_ann[zm->wev_nann++] = dupstr(line + 11);
        }
        logevent(zm->logctx, msg);
        sfree(msg);
    }
    strbuf_clear(zm->errline);
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
        } else if (zm->errline && zm->errline->len < ZM_ERRLINE_MAX) {
            put_byte(zm->errline, c);
        }
        /* a line past ZM_ERRLINE_MAX (a helper that never ends a line) keeps
         * its head for the Event Log; the screen got all of it */
    }
    if (zm->term && s > 0)
        term_data(zm->term, screen, s);
    return 0;
}

/* The transfer ends with the helper's exit code (STILL_ACTIVE: it had not
 * exited; it is then stopped): events 240/241, and whether the
 * notification is due. Once per transfer, whichever path comes first. */
static void zm_end(kitty_zmodem_state *zm, DWORD exitcode)
{
    char *why = NULL;
    const char *last = zm->lasterr ? zm->lasterr : "";
    if (zm->ended)
        return;
    zm->ended = 1;
    zm->end_ok = exitcode == 0;
    if (exitcode == STILL_ACTIVE)
        why = dupprintf("helper did not exit%s%s", last[0] ? ": " : "", last);
    else if (exitcode != 0)
        why = dupprintf("helper exit code %lu%s%s", (unsigned long)exitcode,
                        last[0] ? ": " : "", last);
    zm_wev(zm, exitcode == 0, why ? why : "");
    sfree(why);
}

/* The helper exited: finish the transfer. Safe to tear down here - this is a
 * different callback from the one delivering data. */
static void zm_process_exited(void *vctx)
{
    kitty_zmodem_state *zm = (kitty_zmodem_state *)vctx;
    DWORD exitcode = 1;
    if (zm != zm_active)
        return;
    /* The exit code is read before the tear-down closes the process handle;
     * 0 = the helper finished its transfer. The end is recorded here; the
     * tear-down sends the notification. */
    if (zm->pi.hProcess && !GetExitCodeProcess(zm->pi.hProcess, &exitcode))
        exitcode = 1;
    zm_end(zm, exitcode);
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
        char *cmdline;
        const char *base, *p;
        BOOL started;
        /* argv[0] = bare program name (lrzsz inspects it). The line is held
         * in full: one Windows cannot take (over 32767 characters) makes
         * CreateProcess fail, and the start is reported as failed. */
        base = command;
        for (p = command; *p; p++)
            if (*p == '\\' || *p == '/' || *p == ':') base = p + 1;
        cmdline = dupprintf("%s %s", base, params ? params : "");

        /* rz/sz start only through the helper signature check (kitty_xfer.c) */
        started = kitty_helper_start(command, cmdline, TRUE, CREATE_NEW_CONSOLE,
                                     workdir, &si, &zm->pi);
        sfree(cmdline);
        if (!started) {
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
    zm->errline = strbuf_new();
    zm->lasterr = NULL;

    /* Put both pipes and the process on the main loop's wait list, so the
     * helper having output is itself what wakes us (see the note on
     * the state struct - this is what an upload was missing). */
    zm->h_stdout = handle_input_new(read_stdout, zm_stdout_gotdata, zm, 0);
    zm->h_stderr = handle_input_new(read_stderr, zm_stderr_gotdata, zm, 0);
    zm->hw_process = add_handle_wait(zm->pi.hProcess, zm_process_exited, zm);
    return zm;
}

/* A helper that cannot be found or started is reported in the notice window
 * near the clock, which never takes the focus: a receive starts when the
 * host's sz begins, in the middle of the session's output, and a box then
 * stopped the session until it was answered. Amber, the suite's warning
 * accent. */
static void zm_report(const char *text)
{
    kitty_notice_show(KT_CAP_ZMODEM, text, RGB(190, 110, 0),
                      GetNoticeSeconds(), NULL, 0);
}

/* A Yes in the helper box (kitty_helper_ready): the menu command again. */
typedef struct { HWND owner; UINT cmd; } zm_again_t;
static void zm_again(void *v)
{
    zm_again_t *g = (zm_again_t *)v;
    PostMessage(g->owner, WM_COMMAND, g->cmd, 0);
}
static void zm_again_free(void *v) { sfree(v); }
static zm_again_t *zm_again_new(HWND owner, UINT cmd)
{
    zm_again_t *g = snew(zm_again_t);
    g->owner = owner; g->cmd = cmd;
    return g;
}

/* Start a ZModem RECEIVE (rz): the remote 'sz' has begun; we spawn rz and feed
 * it the incoming stream. Returns 1 on success. */
int kitty_zmodem_receive(HWND owner, UINT again, Conf *conf, Backend *backend, LogContext *logctx, Terminal *term)
{
    ULONGLONG rz_start;
    const char *cmd = kitty_zmodem_command(0);
    const char *opts = conf_get_str(conf, CONF_rzoptions);
    char *dir;
    kitty_zmodem_state *zm;

    if (kitty_zmodem_active()) return 0;
    if (!existfile(cmd)) {
        char *b = dupprintf(KT_ZM_RZ_NOT_FOUND, cmd ? cmd : KT_ZM_UNSET);
        zm_report(b);
        sfree(b);
        return 0;
    }
    /* rz checked first: a Yes in its confirmation box starts the receive again */
    if (!kitty_helper_ready(owner, cmd, 0, 1, zm_again, zm_again_new(owner, again), zm_again_free))
        return 0;
    /* The session's download folder, or the global one (Connection > File-Transfer-Settings). */
    dir = kitty_xfer_download_dir_dup(conf);
    {   /* event 240: what rz saves is what changes from now on */
        FILETIME now;
        GetSystemTimeAsFileTime(&now);
        rz_start = zm_ft(now);
    }
    zm = zm_spawn(cmd, opts, dir, backend, logctx, term);
    if (!zm) {
        sfree(dir);
        zm_report(KT_ZM_RZ_START_FAILED);
        return 0;
    }
    zm->send = 0;
    zm->wev_start = rz_start;
    zm->dir = dir;                      /* the balloon's click opens it */
    zm->wev_user = dupstr(conf_get_str_ambi(conf, CONF_username, NULL));   /* event 240 */
    zm_active = zm;
    return 1;
}

/*
 * The upload's file picker runs beside the session (kitty_call_async):
 * GetOpenFileName's own loop serves no network events, and the session stood
 * still while it was open. Its answer comes back to the terminal's thread,
 * is kept in zm_picked, and the menu command is posted again: the upload
 * then starts from kitty_zmodem_send with the session as it is at that
 * moment. One picker at a time (zm_picking).
 *
 * The answer is held in full: the picker starts with room for a long
 * selection, and a selection that still does not fit (FNERR_BUFFERTOOSMALL,
 * the needed size in the buffer's first WORD when it can say it) opens the
 * picker again with the room it asked for - doubled when it cannot say it -
 * up to ZM_FILES_MAX characters, past which Windows' refusal stands and
 * nothing is sent.
 */
#define ZM_FILES_START (1024 * 1024)
#define ZM_FILES_MAX (16 * 1024 * 1024)
typedef struct {
    HWND owner;
    UINT cmd;
    OPENFILENAME fn;
    char *files;   /* the picker's answer: a path, or dir\0name\0...\0\0 */
    int ok;
} zm_pick_t;
static int zm_picking = 0;
static char *zm_picked = NULL;   /* the files picked, for the posted command */

static void zm_picked_drop(void)
{
    sfree(zm_picked);
    zm_picked = NULL;
}

/* GetOpenFileName with a buffer that grows until the selection fits.
 * *files is the caller's (sfree it, also on failure); 1 = picked. */
static int zm_pick_files(OPENFILENAME *fn, char **files)
{
    DWORD size = ZM_FILES_START;
    for (;;) {
        DWORD need;
        *files = snewn(size, char);
        memset(*files, 0, size);
        fn->lpstrFile = *files;
        fn->nMaxFile = size - 1;   /* the list's second NUL always fits */
        if (GetOpenFileName(fn))
            return 1;
        if (CommDlgExtendedError() != FNERR_BUFFERTOOSMALL || size >= ZM_FILES_MAX)
            return 0;
        need = *(WORD *)*files;
        if (need <= size)
            need = size * 2;
        if (need > ZM_FILES_MAX)
            need = ZM_FILES_MAX;
        size = need + 2;
        sfree(*files);
        *files = NULL;
    }
}

/* The length of a picker answer, its final double NUL included. */
static size_t zm_files_len(const char *files)
{
    const char *p = files;
    while (*p)
        p += strlen(p) + 1;
    return (size_t)(p - files) + 1;
}

static void zm_pick_run(void *v)
{
    zm_pick_t *p = (zm_pick_t *)v;
    p->ok = zm_pick_files(&p->fn, &p->files);
}

static void zm_pick_done(void *v)
{
    zm_pick_t *p = (zm_pick_t *)v;
    zm_picking = 0;
    if (p->ok && IsWindow(p->owner)) {
        size_t n = zm_files_len(p->files);
        zm_picked_drop();
        zm_picked = snewn(n + 1, char);
        memcpy(zm_picked, p->files, n);
        zm_picked[n] = '\0';
        PostMessage(p->owner, WM_COMMAND, p->cmd, 0);
    }
    if (p->files)
        sfree(p->files);
    smemclr(p, sizeof(*p));
    sfree(p);
}

/* Event 240: one file of an upload - its full path, the bare name sz
 * sends, its size. */
typedef struct { char **paths, **names; long long *sizes; int n; } zm_list;
static void zm_note_file(const char *dir, const char *name, zm_list *l)
{
    WIN32_FILE_ATTRIBUTE_DATA fad;
    l->paths = sresize(l->paths, l->n + 1, char *);
    l->names = sresize(l->names, l->n + 1, char *);
    l->sizes = sresize(l->sizes, l->n + 1, long long);
    l->paths[l->n] = (dir && *dir) ? dupprintf("%s\\%s", dir, name) :
                                     dupstr(name);
    l->names[l->n] = dupstr(name);
    l->sizes[l->n] = -1;
    if (GetFileAttributesExA(l->paths[l->n], GetFileExInfoStandard, &fad))
        l->sizes[l->n] = ((long long)fad.nFileSizeHigh << 32) |
                         fad.nFileSizeLow;
    l->n++;
}

/* Start a ZModem SEND (sz <files>): prompt for files, spawn sz. Returns 1. */
int kitty_zmodem_send(HWND owner, UINT again, Conf *conf, Backend *backend, LogContext *logctx, Terminal *term)
{
    OPENFILENAME fn;
    char *filenames = NULL;   /* the picker's answer, held in full */
    const char *cmd = kitty_zmodem_command(1);
    const char *opts = conf_get_str(conf, CONF_szoptions);
    char *dir;
    kitty_zmodem_state *zm;
    strbuf *params;
    char *p;
    char *senddir = "";   /* directory the chosen files live in (see below) */
    zm_list wev_list = { NULL, NULL, NULL, 0 };     /* event 240 */

    if (kitty_zmodem_active()) { zm_picked_drop(); return 0; }
    if (!existfile(cmd)) {
        char *b = dupprintf(KT_ZM_SZ_NOT_FOUND, cmd ? cmd : KT_ZM_UNSET);
        zm_picked_drop();
        zm_report(b);
        sfree(b);
        return 0;
    }
    /* sz checked before the picker: a Yes in its confirmation box re-opens the upload */
    if (!kitty_helper_ready(owner, cmd, 0, 1, zm_again, zm_again_new(owner, again), zm_again_free)) {
        zm_picked_drop();
        return 0;
    }

    if (zm_picked) {
        /* the picker has answered (zm_pick_done): its files */
        filenames = zm_picked;
        zm_picked = NULL;
    } else {
        zm_pick_t *pk;
        if (zm_picking)
            return 0;               /* its picker is open already */
        memset(&fn, 0, sizeof(fn));
        fn.lStructSize = sizeof(fn);
        fn.hwndOwner = owner;
        fn.lpstrTitle = KT_ZM_SELECT_FILES;
        fn.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST |
                   OFN_HIDEREADONLY | OFN_PATHMUSTEXIST;
        pk = snew(zm_pick_t);
        memset(pk, 0, sizeof(*pk));
        pk->owner = owner;
        pk->cmd = again;
        pk->fn = fn;
        pk->files = NULL;
        if (kitty_call_async(zm_pick_run, zm_pick_done, pk)) {
            zm_picking = 1;
            return 0;               /* the upload follows the picker's answer */
        }
        sfree(pk);
        /* no thread for the picker: on this one, as before */
        if (!zm_pick_files(&fn, &filenames)) {
            sfree(filenames);
            return 0;   /* user cancelled */
        }
    }

    params = strbuf_new();
    {
        put_fmt(params, "%s", opts ? opts : "");
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
            put_fmt(params, " \"%s\"", p);
            zm_note_file(senddir, p, &wev_list);
        } else {
            /* multi: first field is the dir, then each file name */
            senddir = filenames;
            p = filenames;
            for (;;) {
                p = p + strlen(p) + 1;
                if (*p == 0) break;
                put_fmt(params, " \"%s\"", p);
                zm_note_file(senddir, p, &wev_list);
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
    dir = kitty_xfer_download_dir_dup(conf);
    zm = zm_spawn(cmd, params->s, senddir[0] ? senddir : dir, backend, logctx, term);
    sfree(dir);
    strbuf_free(params);
    sfree(filenames);   /* senddir pointed into it: not used from here on */
    if (!zm) {
        int i;
        for (i = 0; i < wev_list.n; i++) {
            sfree(wev_list.paths[i]);
            sfree(wev_list.names[i]);
        }
        sfree(wev_list.paths);
        sfree(wev_list.names);
        sfree(wev_list.sizes);
        zm_report(KT_ZM_SZ_START_FAILED);
        return 0;
    }
    zm->send = 1;
    zm->wev_paths = wev_list.paths;     /* event 240, one per file */
    zm->wev_names = wev_list.names;
    zm->wev_sizes = wev_list.sizes;
    zm->wev_n = wev_list.n;
    zm->wev_user = dupstr(conf_get_str_ambi(conf, CONF_username, NULL));
    /* files leaving: the balloon's click opens the local upload folder */
    zm->dir = kitty_xfer_upload_dir_dup(conf);
    zm_active = zm;
    return 1;
}

void kitty_zmodem_cancel(void)
{
    if (zm_active && zm_active->transfering && !zm_active->ended) {
        zm_active->ended = 1;   /* no notification for a cancel */
        zm_wev(zm_active, 0, "cancelled");   /* event 241 */
    }
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
