/*
 * kitty_hostkey_verify.c: see the header. A worker thread runs klink once per
 * job and parses the one-object JSON array klink -scan -json prints for a
 * single type. Only klink's own output is parsed here, so the "parser" is a
 * field extractor, not a JSON implementation.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "putty.h"
#include "kitty_hostkey_verify.h"

struct kitty_hkv_run {
    struct kitty_hkv_job *jobs;
    int n;
    volatile LONG refs;             /* the UI and the worker; last one frees */
    HANDLE thread;
};

char *kitty_hkv_klink_path(void)
{
    char exe[MAX_PATH];
    char *slash;
    if (!GetModuleFileNameA(NULL, exe, sizeof(exe)))
        return dupstr("klink.exe");
    slash = strrchr(exe, '\\');
    if (slash) slash[1] = '\0';
    return dupcat(exe, "klink.exe");
}

/* ---- the child ----------------------------------------------------------- */

/* Run a command line hidden, collect its stdout. false when it could not be
 * started (then *out holds the Windows error text). */
static bool run_hidden(char *cmdline, char **out, DWORD *exitcode)
{
    SECURITY_ATTRIBUTES sa;
    HANDLE rd = NULL, wr = NULL;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    strbuf *sb;

    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        *out = dupstr("CreatePipe failed");
        return false;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError = wr;            /* klink's diagnostics land in the same text */
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    memset(&pi, 0, sizeof(pi));
    if (!CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi)) {
        DWORD err = GetLastError();
        CloseHandle(rd); CloseHandle(wr);
        *out = dupprintf("could not start klink.exe (Windows error %lu)", (unsigned long)err);
        return false;
    }
    CloseHandle(wr);
    CloseHandle(pi.hThread);

    sb = strbuf_new();
    for (;;) {
        char buf[4096];
        DWORD got;
        if (!ReadFile(rd, buf, sizeof(buf), &got, NULL) || got == 0)
            break;
        put_data(sb, buf, got);
    }
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, 30000);
    if (!GetExitCodeProcess(pi.hProcess, exitcode))
        *exitcode = (DWORD)-1;
    CloseHandle(pi.hProcess);
    *out = strbuf_to_str(sb);
    return true;
}

/* The string value of "name" in klink's JSON, unescaped; "" when absent. */
static char *json_field(const char *text, const char *name)
{
    char *needle = dupprintf("\"%s\": \"", name);
    const char *p = strstr(text, needle);
    strbuf *sb;
    sfree(needle);
    if (!p)
        return dupstr("");
    p = strchr(p, ':') + 3;
    sb = strbuf_new();
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) {
            p++;
            if (*p == 'u' && strlen(p) >= 5) { p += 5; continue; }   /* control chars: drop */
        }
        put_byte(sb, *p++);
    }
    return strbuf_to_str(sb);
}

static char *now_iso(void)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    return dupprintf("%04d-%02d-%02dT%02d:%02d:%02d", st.wYear, st.wMonth,
                     st.wDay, st.wHour, st.wMinute, st.wSecond);
}

static void run_free(struct kitty_hkv_run *run)
{
    for (int i = 0; i < run->n; i++) {
        struct kitty_hkv_job *j = &run->jobs[i];
        sfree(j->host); sfree(j->keytype); sfree(j->status); sfree(j->sha256);
        sfree(j->md5); sfree(j->key); sfree(j->error); sfree(j->when);
    }
    sfree(run->jobs);
    if (run->thread) CloseHandle(run->thread);
    sfree(run);
}

static void run_release(struct kitty_hkv_run *run)
{
    if (InterlockedDecrement(&run->refs) == 0)
        run_free(run);
}

static DWORD WINAPI worker(LPVOID param)
{
    struct kitty_hkv_run *run = (struct kitty_hkv_run *)param;
    char *klink = kitty_hkv_klink_path();
    bool have_klink = GetFileAttributesA(klink) != INVALID_FILE_ATTRIBUTES;

    for (int i = 0; i < run->n; i++) {
        struct kitty_hkv_job *j = &run->jobs[i];
        char *out = NULL, *cmd;
        DWORD code = 0;

        InterlockedExchange(&j->state, KHKV_RUNNING);
        j->sha256 = dupstr(""); j->md5 = dupstr(""); j->key = dupstr("");
        j->error = dupstr("");
        if (!have_klink) {
            j->status = dupstr("no klink");
            sfree(j->error); j->error = dupprintf("%s was not found", klink);
        } else {
            cmd = strchr(j->host, ':') ?
                dupprintf("\"%s\" -scan -json -t %s [%s]:%d", klink, j->keytype, j->host, j->port) :
                dupprintf("\"%s\" -scan -json -t %s %s:%d", klink, j->keytype, j->host, j->port);
            if (!run_hidden(cmd, &out, &code)) {
                j->status = dupstr("no klink");
                sfree(j->error); j->error = out; out = NULL;
            } else {
                const char *arr = strchr(out, '[');
                if (!arr) {
                    j->status = dupstr("klink failed");
                    sfree(j->error);
                    j->error = dupprintf("klink exit %lu: %.200s", (unsigned long)code, out);
                } else {
                    sfree(j->sha256); j->sha256 = json_field(arr, "sha256");
                    sfree(j->md5);    j->md5 = json_field(arr, "md5");
                    sfree(j->key);    j->key = json_field(arr, "key");
                    sfree(j->error);  j->error = json_field(arr, "error");
                    j->status = json_field(arr, "status");
                    if (!strcmp(j->sha256, "-")) *j->sha256 = '\0';
                    if (!strcmp(j->md5, "-")) *j->md5 = '\0';
                    if (!strcmp(j->key, "-")) *j->key = '\0';
                    if (!strcmp(j->error, "-")) *j->error = '\0';
                }
            }
            sfree(cmd);
            sfree(out);
        }
        j->when = now_iso();
        MemoryBarrier();
        InterlockedExchange(&j->state, KHKV_DONE);
    }
    sfree(klink);
    run_release(run);
    return 0;
}

struct kitty_hkv_run *kitty_hkv_start(const struct kitty_hkv_job *jobs, int n)
{
    struct kitty_hkv_run *run = snew(struct kitty_hkv_run);
    memset(run, 0, sizeof(*run));
    run->jobs = snewn(n, struct kitty_hkv_job);
    memset(run->jobs, 0, n * sizeof(struct kitty_hkv_job));
    for (int i = 0; i < n; i++) {
        run->jobs[i].host = dupstr(jobs[i].host);
        run->jobs[i].port = jobs[i].port;
        run->jobs[i].keytype = dupstr(jobs[i].keytype);
        run->jobs[i].state = KHKV_PENDING;
    }
    run->n = n;
    run->refs = 2;
    run->thread = CreateThread(NULL, 0, worker, run, 0, NULL);
    if (!run->thread) {
        run->refs = 1;
        run_release(run);
        return NULL;
    }
    return run;
}

struct kitty_hkv_job *kitty_hkv_jobs(struct kitty_hkv_run *run, int *n)
{
    *n = run->n;
    return run->jobs;
}

bool kitty_hkv_finished(struct kitty_hkv_run *run)
{
    for (int i = 0; i < run->n; i++)
        if (InterlockedCompareExchange(&run->jobs[i].state, KHKV_DONE, KHKV_DONE) != KHKV_DONE)
            return false;
    return true;
}

void kitty_hkv_release(struct kitty_hkv_run *run)
{
    run_release(run);
}
