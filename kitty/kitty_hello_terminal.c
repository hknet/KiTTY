/*
 * kitty_hello_terminal.c - the terminal's side of Hello-protected key
 * files: implementations for the three hooks in ssh/userauth2-client.c.
 * With several terminals open, the anchor card carries the asking
 * window's title and opens over it, so the user knows WHICH session
 * wants the authorization.
 */

#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <process.h>   /* _beginthreadex: kht_unlock_async */

#include "putty.h"
#include "kitty_hello.h"
#include "kitty_hello_keys.h"
#include "kitty_text.h"     /* the anchor card's context line */
#include "kitty_buildlabel.h"   /* KITTY_TEST_BUILD_LABEL: the Hello test door */
#include "kitty_gui.h"

extern char *(*kitty_hello_keyfile_unlock_hook)(const char *path);
extern char *(*kitty_hello_keyfile_translate_hook)(const char *path,
                                                   const char *typed);
extern int (*kitty_hello_keyfile_protected_hook)(const char *path);
extern int (*kitty_hello_keyfile_unlock_async_hook)(
    const char *path, void (*done)(void *ctx, char *pass), void *ctx,
    void **token);
extern void (*kitty_hello_keyfile_unlock_cancel_hook)(void *token);

static HWND kht_window;

static void kht_set_context(const char *path)
{
    char title[160];
    char line[220];
    const char *base = path, *q;
    title[0] = '\0';
    if (kht_window && IsWindow(kht_window))
        GetWindowTextA(kht_window, title, sizeof(title));
    for (q = path; *q; q++)
        if (*q == '\\' || *q == '/')
            base = q + 1;
    snprintf(line, sizeof(line), KT_HELLO_CONTEXT_LINE_FMT,
             title[0] ? title : KT_CAP_KITTY, base);
    kitty_hello_set_context(line, kht_window);
}

static char *kht_unlock(const char *path)
{
    char *pass = NULL;
    if (!kageant_hello_has_sidecar(path))
        return NULL;
    kht_set_context(path);
    if (kageant_hello_unlock(path, kht_window, &pass, NULL) ==
            KAGEANT_HELLO_OK)
        return pass;
    burnstr(pass);
    kitty_hello_set_context(NULL, NULL);   /* not consumed on failure */
    return NULL;
}

/*
 * The same unlock without stopping the session. The Hello wait pumps only
 * window messages, so on the terminal's thread it held the network and the
 * callbacks for as long as the prompt stood - up to two minutes, inside the
 * server's login grace time - and the PRF path's nested pump ran them from
 * inside the login itself. Here the unlock runs on a worker thread
 * (kitty_hello_on_worker) and its answer comes back to the terminal's thread
 * through a message-only window and a top-level callback. A cancelled job
 * (the login went meanwhile) burns its passphrase unseen.
 */
#define WM_KHT_DONE (WM_APP + 0x3A3)
struct kht_job {
    char *path;
    HWND owner;
    char *pass;
    int ret;
    volatile LONG cancelled;
    void (*done)(void *ctx, char *pass);
    void *ctx;
};
static HWND kht_sink;

static void kht_job_free(struct kht_job *j)
{
    if (j->pass)
        burnstr(j->pass);
    sfree(j->path);
    sfree(j);
}

static void kht_deliver(void *vctx)
{
    struct kht_job *j = (struct kht_job *)vctx;
    char *pass = NULL;
    if (!j->cancelled) {
        if (j->ret == KAGEANT_HELLO_OK) {
            pass = j->pass;
            j->pass = NULL;
        } else {
            kitty_hello_set_context(NULL, NULL);   /* not consumed on failure */
        }
        j->done(j->ctx, pass);
    }
    kht_job_free(j);
}

static LRESULT CALLBACK kht_sink_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KHT_DONE) {
        /* outside this window's message: the login resumes from there */
        queue_toplevel_callback(kht_deliver, (void *)lp);
        return 0;
    }
    return DefWindowProc(h, msg, wp, lp);
}

#ifdef KITTY_TEST_BUILD_LABEL
/* Test builds only: KITTY_HELLO_TEST_UNLOCK="<ms>:ok:<passphrase>" or
 * "<ms>:deny" stands in for Windows Hello (which no test lane has) - the
 * worker waits <ms>, then answers so. Unset: the real thing. */
static bool kht_test_door(struct kht_job *j)
{
    char buf[512], *p, *q;
    DWORD n = GetEnvironmentVariableA("KITTY_HELLO_TEST_UNLOCK", buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf))
        return false;
    p = strchr(buf, ':');
    if (!p)
        return false;
    *p++ = '\0';
    Sleep((DWORD)atoi(buf));
    q = strchr(p, ':');
    if (q)
        *q++ = '\0';
    if (!strcmp(p, "ok") && q) {
        j->pass = dupstr(q);
        j->ret = KAGEANT_HELLO_OK;
    } else {
        j->ret = KAGEANT_HELLO_DENIED;
    }
    return true;
}
#endif

static unsigned __stdcall kht_thread(void *v)
{
    struct kht_job *j = (struct kht_job *)v;
    kitty_hello_on_worker();
#ifdef KITTY_TEST_BUILD_LABEL
    if (!kht_test_door(j))
#endif
        j->ret = kageant_hello_unlock(j->path, j->owner, &j->pass, NULL);
    PostMessage(kht_sink, WM_KHT_DONE, 0, (LPARAM)j);
    return 0;
}

/* Start the unlock of `path`; done(ctx, passphrase or NULL) arrives on this
 * thread later. 0: nothing to unlock this way (no Hello sidecar), go on to
 * the prompt; *token then stays NULL. */
static int kht_unlock_async(const char *path,
                            void (*done)(void *ctx, char *pass), void *ctx,
                            void **token)
{
    struct kht_job *j;
    uintptr_t th;
    *token = NULL;
#ifdef KITTY_TEST_BUILD_LABEL
    if (!GetEnvironmentVariableA("KITTY_HELLO_TEST_UNLOCK", NULL, 0))
#endif
    if (!kageant_hello_has_sidecar(path))
        return 0;
    if (!kht_sink) {
        WNDCLASSA wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = kht_sink_proc;
        wc.hInstance = GetModuleHandle(NULL);
        wc.lpszClassName = "KiTTYHelloSink";
        RegisterClassA(&wc);
        kht_sink = CreateWindowExA(0, "KiTTYHelloSink", "", 0, 0, 0, 0, 0,
                                   HWND_MESSAGE, NULL, GetModuleHandle(NULL),
                                   NULL);
        if (!kht_sink)
            return 0;
    }
    kht_set_context(path);
    j = snew(struct kht_job);
    memset(j, 0, sizeof(*j));
    j->path = dupstr(path);
    j->owner = kht_window;
    j->ret = KAGEANT_HELLO_ERROR;
    j->done = done;
    j->ctx = ctx;
    th = _beginthreadex(NULL, 0, kht_thread, j, 0, NULL);
    if (!th) {
        kitty_hello_set_context(NULL, NULL);
        kht_job_free(j);
        return 0;
    }
    CloseHandle((HANDLE)th);
    *token = j;
    return 1;
}

/* The login that asked is gone: its done() must not run. */
static void kht_unlock_cancel(void *token)
{
    if (token)
        InterlockedExchange(&((struct kht_job *)token)->cancelled, 1);
}

static char *kht_translate(const char *path, const char *typed)
{
    return kageant_hello_translate(path, typed, NULL);
}

static int kht_protected(const char *path)
{
    return kageant_hello_has_sidecar(path);
}

/* Install the hooks; the window is the terminal's, for the anchor card
 * and its context line. Call once the window exists. */
void kitty_hello_terminal_init(HWND terminal_window)
{
    kht_window = terminal_window;
    kitty_hello_keyfile_unlock_hook = kht_unlock;
    kitty_hello_keyfile_unlock_async_hook = kht_unlock_async;
    kitty_hello_keyfile_unlock_cancel_hook = kht_unlock_cancel;
    kitty_hello_keyfile_translate_hook = kht_translate;
    kitty_hello_keyfile_protected_hook = kht_protected;
}
