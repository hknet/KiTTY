/*
 * KiTTY: the audit-log file sink - see kitty_auditlog.h for the format
 * contract. Everything here is deliberately boring: open-append per event,
 * rotate by size before the write that would cross the cap, expunge old
 * generations by age at rotation time, and NEVER lose the active file - a
 * failed rotation is itself logged and writing continues.
 */

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "kitty_auditlog.h"

static char g_path[MAX_PATH + 1];
static int  g_enabled = 0;
static int  g_maxkb = 5120;
static int  g_keep = 3;
static int  g_expiredays = 90;
static int  g_rotate_warned = 0;   /* one failure event per process */

void kitty_audit_configure(const char *path, int enabled,
                           int maxkb, int keep, int expiredays)
{
    if (path)
        snprintf(g_path, sizeof(g_path), "%s", path);
    g_enabled = enabled ? 1 : 0;
    g_maxkb = maxkb > 0 ? maxkb : 5120;
    g_keep = keep >= 1 ? keep : 1;
    if (g_keep > 99) g_keep = 99;
    g_expiredays = expiredays >= 0 ? expiredays : 0;
}

int kitty_audit_enabled(void) { return g_enabled && g_path[0]; }
const char *kitty_audit_path(void) { return g_path; }

/* Value escaping - the log's integrity rests on this. A comment or a file
 * path arrives from OUTSIDE (the pipe, a filename) and must never be able
 * to close the quote, start a new field, or write a raw newline that
 * forges a whole line. */
static void audit_put_quoted(char **p, char *end, const char *v)
{
    if (*p < end) *(*p)++ = '"';
    for (; *v && *p < end - 3; v++) {
        unsigned char c = (unsigned char)*v;
        if (c == '"' || c == '\\') {
            *(*p)++ = '\\'; *(*p)++ = (char)c;
        } else if (c == '\n' || c == '\r') {
            *(*p)++ = '\\'; *(*p)++ = 'n';
        } else if (c < 0x20) {
            *(*p)++ = '?';          /* other control bytes: neutered */
        } else {
            *(*p)++ = (char)c;
        }
    }
    if (*p < end) *(*p)++ = '"';
}

static void audit_gen_name(char *buf, size_t sz, int gen)
{
    snprintf(buf, sz, "%s.%d", g_path, gen);
}

/* Rotate if the active file has crossed the cap. Shift .1->.2 etc from the
 * oldest down, expunge generations past their age, then move the active
 * file to .1. Any failure leaves the ACTIVE file untouched and writing. */
static void audit_maybe_rotate(void)
{
    WIN32_FILE_ATTRIBUTE_DATA fad;
    char from[MAX_PATH + 8], to[MAX_PATH + 8];
    int i;

    if (!GetFileAttributesExA(g_path, GetFileExInfoStandard, &fad))
        return;                             /* no file yet: nothing to do */
    if (fad.nFileSizeHigh == 0 &&
        fad.nFileSizeLow <= (DWORD)g_maxkb * 1024)
        return;

    /* Age-expunge existing generations first, so an old .keep does not get
     * shifted around only to be deleted next time. */
    if (g_expiredays > 0) {
        ULONGLONG now, cutoff;
        FILETIME nowft;
        GetSystemTimeAsFileTime(&nowft);
        now = ((ULONGLONG)nowft.dwHighDateTime << 32) | nowft.dwLowDateTime;
        cutoff = (ULONGLONG)g_expiredays * 24ULL * 3600ULL * 10000000ULL;
        for (i = 1; i <= g_keep; i++) {
            WIN32_FILE_ATTRIBUTE_DATA gfad;
            audit_gen_name(from, sizeof(from), i);
            if (GetFileAttributesExA(from, GetFileExInfoStandard, &gfad)) {
                ULONGLONG mt =
                    ((ULONGLONG)gfad.ftLastWriteTime.dwHighDateTime << 32) |
                    gfad.ftLastWriteTime.dwLowDateTime;
                if (mt < now && now - mt > cutoff)
                    DeleteFileA(from);
            }
        }
    }

    audit_gen_name(from, sizeof(from), g_keep);
    DeleteFileA(from);                       /* the oldest falls off */
    for (i = g_keep - 1; i >= 1; i--) {
        audit_gen_name(from, sizeof(from), i);
        audit_gen_name(to, sizeof(to), i + 1);
        MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING);
    }
    audit_gen_name(to, sizeof(to), 1);
    if (!MoveFileExA(g_path, to, MOVEFILE_REPLACE_EXISTING) &&
        !g_rotate_warned) {
        /* Keep writing to the (oversized) active file rather than lose
         * anything; say so once, in the log itself. */
        g_rotate_warned = 1;
        kitty_audit("logrotate", "result", "failed", (const char *)NULL);
    }
}

void kitty_audit(const char *ev, ...)
{
    char line[2048];
    char *p = line, *end = line + sizeof(line) - 4;
    SYSTEMTIME st;
    va_list ap;
    HANDLE h;
    DWORD written;

    if (!kitty_audit_enabled() || !ev || !*ev)
        return;

    audit_maybe_rotate();

    GetSystemTime(&st);                      /* UTC on disk, by contract */
    p += snprintf(p, end - p,
                  "ts=%04u-%02u-%02uT%02u:%02u:%02uZ ev=",
                  st.wYear, st.wMonth, st.wDay,
                  st.wHour, st.wMinute, st.wSecond);
    audit_put_quoted(&p, end, ev);

    va_start(ap, ev);
    for (;;) {
        const char *k = va_arg(ap, const char *);
        const char *v;
        if (!k)
            break;
        v = va_arg(ap, const char *);
        if (!v || !*v)
            continue;                        /* optional field, absent */
        if (p < end) *p++ = ' ';
        for (; *k && p < end; k++)
            *p++ = *k;
        if (p < end) *p++ = '=';
        audit_put_quoted(&p, end, v);
    }
    va_end(ap);
    *p++ = '\r'; *p++ = '\n';

    h = CreateFileA(g_path, FILE_APPEND_DATA,
                    FILE_SHARE_READ, NULL, OPEN_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;                              /* never block the agent on IO */
    WriteFile(h, line, (DWORD)(p - line), &written, NULL);
    CloseHandle(h);
}
