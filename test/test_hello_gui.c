/*
 * test_hello_gui.c - the PRF assertion in kageant's clothes.
 *
 * Hands-on diagnostic only (KITTY_HELLO_DIAG target, never shipped). The
 * same assertion that passes from the console test binary fails from
 * kageant; this binary is the console test re-dressed with the three
 * process-level properties kageant has and the console test does not:
 *
 *   - GUI subsystem (WinMain, no console)
 *   - kageant's manifest (Common Controls 6, PerMonitorV2 DPI)
 *   - dll_hijacking_protection() before any WebAuthn call
 *
 * One run = one assertion against the existing KiTTY credential. The
 * verdict goes to a message box AND to hello_gui.log beside the exe.
 * Command line: "nodllprot" skips the DLL-search hardening, "nomanifest"
 * cannot be done at runtime (build without the .rc for that).
 */

#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "putty.h"
#include "ssh.h"
#include "kitty/kitty_hello.h"

const char *appname = "test_hello_gui";

void modalfatalbox(const char *p, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, p);
    vsnprintf(buf, sizeof(buf), p, ap);
    va_end(ap);
    MessageBoxA(NULL, buf, "test_hello_gui - fatal", MB_ICONERROR | MB_OK);
    exit(1);
}

void old_keyfile_warning(void) {}

void random_read(void *buf, size_t size)
{
    unsigned char *p = (unsigned char *)buf;
    while (size > 0) {
        unsigned char chunk[KITTY_HELLO_SECRET_LEN];
        size_t n = size < sizeof(chunk) ? size : sizeof(chunk);
        if (!kitty_hello_new_secret(chunk))
            modalfatalbox("random generator failed");
        memcpy(p, chunk, n);
        p += n;
        size -= n;
    }
}

static void note(FILE *log, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (log) {
        vfprintf(log, fmt, ap);
        fputc('\n', log);
        fflush(log);
    }
    va_end(ap);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show)
{
    FILE *log = fopen("hello_gui.log", "a");
    HWND owner;
    unsigned char *credid = NULL;
    size_t credidlen = 0;
    unsigned char kek[32];
    int found, r;
    char msg[1024];
    bool dllprot = !(cmdline && strstr(cmdline, "nodllprot"));

    note(log, "--- run: cmdline=\"%s\"", cmdline ? cmdline : "");
    if (dllprot) {
        dll_hijacking_protection();
        note(log, "dll_hijacking_protection: applied");
    } else {
        note(log, "dll_hijacking_protection: SKIPPED");
    }

    owner = GetForegroundWindow();
    note(log, "owner = foreground window %p", (void *)owner);
    note(log, "prf available = %d", kitty_hello_prf_available());

    found = kitty_hello_prf_my_credid(&credid, &credidlen);
    note(log, "my credential: found=%d len=%u", found, (unsigned)credidlen);
    if (found != 1) {
        snprintf(msg, sizeof(msg),
                 "No KiTTY credential in the platform store (find=%d). "
                 "Nothing to assert against.", found);
        note(log, "%s", msg);
        MessageBoxA(NULL, msg, "test_hello_gui", MB_ICONWARNING | MB_OK);
        if (log) fclose(log);
        return 2;
    }

    r = kitty_hello_prf_kek(owner, credid, credidlen, kek);
    smemclr(kek, sizeof(kek));
    sfree(credid);
    snprintf(msg, sizeof(msg),
             "assert result code %d (%s)\ndetail: %s\n\n"
             "dll protection: %s, GUI subsystem + kageant manifest: yes",
             r, r == KITTY_HELLO_VERIFIED ? "VERIFIED - PASS" :
                r == KITTY_HELLO_DENIED ? "DENIED" :
                r == KITTY_HELLO_UNAVAILABLE ? "UNAVAILABLE" : "ERROR",
             kitty_hello_last_detail(), dllprot ? "on" : "off");
    note(log, "%s", msg);
    MessageBoxA(NULL, msg, r == KITTY_HELLO_VERIFIED ?
                "test_hello_gui - PASS" : "test_hello_gui - FAIL",
                (r == KITTY_HELLO_VERIFIED ? MB_ICONINFORMATION : MB_ICONERROR)
                | MB_OK);
    if (log) fclose(log);
    return r == KITTY_HELLO_VERIFIED ? 0 : 1;
}
