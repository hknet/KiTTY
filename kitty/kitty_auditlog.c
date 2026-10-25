/*
 * KiTTY: the audit log's sinks - see kitty_auditlog.h for the format
 * contract. The file sink is deliberately boring: open-append per event,
 * rotate by size before the write that would cross the cap, expunge old
 * generations by age at rotation time, and NEVER lose the active file - a
 * failed rotation is itself logged and writing continues. The Event Log
 * sink reports the same record as one event: its kind picks the event ID,
 * its fields fill the insertion strings in a fixed order per ID.
 */

#include "putty.h"            /* dupstr, dupprintf, strbuf */
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "kitty_auditlog.h"
#include "kitty_winevent.h"     /* kitty_wev_user_sid: the record's user */

static char *g_path = NULL;        /* the whole path; NULL until configured */
static int  g_enabled = 0;
static int  g_sinks = KITTY_AUDIT_SINK_FILE;
static int  g_maxkb = 5120;
static int  g_keep = 3;
static int  g_expiredays = 90;
static int  g_rotate_warned = 0;   /* one failure event per process */

#define AUDIT_MAX_FIELDS 16

void kitty_audit_configure(const char *path, int enabled,
                           int maxkb, int keep, int expiredays)
{
    if (path) {
        sfree(g_path);
        g_path = dupstr(path);
    }
    g_enabled = enabled ? 1 : 0;
    g_maxkb = maxkb > 0 ? maxkb : 5120;
    g_keep = keep >= 1 ? keep : 1;
    if (g_keep > 99) g_keep = 99;
    g_expiredays = expiredays >= 0 ? expiredays : 0;
}

void kitty_audit_set_sinks(int sinks)
{
    g_sinks = sinks & (KITTY_AUDIT_SINK_FILE | KITTY_AUDIT_SINK_EVENTLOG);
}

int kitty_audit_sinks(void) { return g_sinks; }

int kitty_audit_enabled(void)
{
    return g_enabled &&
        (((g_sinks & KITTY_AUDIT_SINK_FILE) && g_path && g_path[0]) ||
         (g_sinks & KITTY_AUDIT_SINK_EVENTLOG));
}

const char *kitty_audit_path(void) { return g_path ? g_path : ""; }

/* The result values the agent writes for a refusal or a failure (grep the
 * kitty_audit / kageant_audit_use / kageant_audit_load calls for the full
 * set). "waiting", "shut" and "unlock-shut" are unlock refusals: during the
 * wait after a wrong passphrase, and while unlocking over IPC is shut. */
int kitty_audit_result_is_warning(const char *result)
{
    static const char *const bad[] = {
        "denied", "refused", "blocked", "failed", "fp-refused", "cancelled",
        "not-verified", "waiting", "shut", "unlock-shut", "no-door-here",
    };
    size_t i;
    if (!result || !*result)
        return 0;
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        if (!strcmp(result, bad[i]))
            return 1;
    return 0;
}

int kitty_audit_is_warning(const char *ev, const char *result)
{
    return kitty_audit_result_is_warning(result) ||
           (ev && !strcmp(ev, "keyfile-warning"));
}

/* Value quoting - the file's integrity rests on this. A comment or a file
 * path arrives from OUTSIDE (the pipe, a filename) and must never be able
 * to close the quote, start a new field, or write a raw newline that
 * forges a whole line. A quote is doubled; a control byte becomes ?; a
 * backslash is an ordinary character. */
static void audit_put_quoted(strbuf *sb, const char *v)
{
    put_byte(sb, '"');
    for (; *v; v++) {
        unsigned char c = (unsigned char)*v;
        if (c == '"') {
            put_byte(sb, '"'); put_byte(sb, '"');
        } else if (c < 0x20) {
            put_byte(sb, '?');      /* CR, LF and every other control byte */
        } else {
            put_byte(sb, c);
        }
    }
    put_byte(sb, '"');
}

/* ev="..." key="value" ... - the record without its timestamp, whole: a
 * long value never pushes the fields after it out of the line. */
static void audit_put_record(strbuf *sb, const char *ev,
                             const char *const *keys,
                             const char *const *vals, int n)
{
    int i;
    put_datapl(sb, PTRLEN_LITERAL("ev="));
    audit_put_quoted(sb, ev);
    for (i = 0; i < n; i++) {
        put_byte(sb, ' ');
        put_dataz(sb, keys[i]);
        put_byte(sb, '=');
        audit_put_quoted(sb, vals[i]);
    }
}

static char *audit_gen_name(int gen)
{
    return dupprintf("%s.%d", g_path, gen);
}

/* Rotate if the active file has crossed the cap. Shift .1->.2 etc from the
 * oldest down, expunge generations past their age, then move the active
 * file to .1. Any failure leaves the ACTIVE file untouched and writing. */
static void audit_maybe_rotate(void)
{
    WIN32_FILE_ATTRIBUTE_DATA fad;
    char *from, *to;
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
            from = audit_gen_name(i);
            if (GetFileAttributesExA(from, GetFileExInfoStandard, &gfad)) {
                ULONGLONG mt =
                    ((ULONGLONG)gfad.ftLastWriteTime.dwHighDateTime << 32) |
                    gfad.ftLastWriteTime.dwLowDateTime;
                if (mt < now && now - mt > cutoff)
                    DeleteFileA(from);
            }
            sfree(from);
        }
    }

    from = audit_gen_name(g_keep);
    DeleteFileA(from);                       /* the oldest falls off */
    sfree(from);
    for (i = g_keep - 1; i >= 1; i--) {
        from = audit_gen_name(i);
        to = audit_gen_name(i + 1);
        MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING);
        sfree(from);
        sfree(to);
    }
    to = audit_gen_name(1);
    i = MoveFileExA(g_path, to, MOVEFILE_REPLACE_EXISTING);
    sfree(to);
    if (!i && !g_rotate_warned) {
        /* Keep writing to the (oversized) active file rather than lose
         * anything; record it once, in the log itself. */
        g_rotate_warned = 1;
        kitty_audit("logrotate", "result", "failed", (const char *)NULL);
    }
}

static void audit_write_file(const char *ev, const char *const *keys,
                             const char *const *vals, int n)
{
    strbuf *line = strbuf_new();
    SYSTEMTIME st;
    HANDLE h;
    DWORD written;

    audit_maybe_rotate();

    GetSystemTime(&st);                      /* UTC on disk, by contract */
    put_fmt(line, "ts=%04u-%02u-%02uT%02u:%02u:%02uZ ",
            st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute, st.wSecond);
    audit_put_record(line, ev, keys, vals, n);
    put_datapl(line, PTRLEN_LITERAL("\r\n"));

    h = CreateFileA(g_path, FILE_APPEND_DATA,
                    FILE_SHARE_READ, NULL, OPEN_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {         /* never block the agent on IO */
        WriteFile(h, line->s, (DWORD)line->len, &written, NULL);
        CloseHandle(h);
    }
    strbuf_free(line);
}

/*
 * The Event Log sink's table: one event ID per kind of record, and the
 * fields that kind carries, in the order of their insertion strings. %1 is
 * always the kind itself (the ev value), the listed fields are %2 onwards;
 * a field the record does not carry is an empty string, so every event of
 * an ID has the same number of strings in the same places - which is what
 * lets Event Viewer filters, Windows Event Forwarding and a SIEM select on
 * them by position. windows/kitty_events.mc holds the matching message
 * texts. The order of an existing list never changes; a new field is
 * appended at its end, here and in the message.
 */
static const char *const af_use[] = {
    "fp", "comment", "result", "reason", "req", "reqpath", "pid", NULL
};
static const char *const af_add[] = {
    "fp", "comment", "result", "reason", "req", "reqpath", "pid", "path", NULL
};
static const char *const af_load[] = { "path", "result", "enc", NULL };
static const char *const af_forget[] = { "path", "result", NULL };
static const char *const af_retry[] = {
    "trigger", "loaded", "refused", "absent", "broken", NULL
};
static const char *const af_hello[] = { "path", "result", "detail", NULL };
static const char *const af_protect[] = {
    "path", "dest", "result", "detail", "err", NULL
};
static const char *const af_keywarn[] = { "path", "warning", NULL };
static const char *const af_decrypt[] = {
    "fp", "comment", "result", "trigger", "req", "reqpath", "pid", NULL
};
static const char *const af_result[] = { "result", NULL };

static const struct audit_kind {
    const char *ev;
    WORD id;
    const char *const *fields;
} audit_kinds[] = {
    { "sign",            100, af_use },
    { "confirm",         101, af_use },
    { "add",             110, af_add },
    { "loadkey",         111, af_load },
    { "forget-path",     112, af_forget },
    { "retry",           113, af_retry },
    { "remove",          114, af_use },
    { "remove-all",      115, af_use },
    { "lock",            120, af_use },
    { "unlock",          121, af_use },
    { "hello-enrol",     130, af_hello },
    { "hello-protect",   131, af_protect },
    { "hello-unlock",    132, af_hello },
    { "keyfile-warning", 140, af_keywarn },
    { "reencrypt-all",   150, af_use },
    { "reencrypt",       151, af_use },
    { "decrypt",         152, af_decrypt },
    { "agent",           160, af_result },
    { "logrotate",       170, af_result },
};

/* A copy of v with control bytes made ?, as in the file, whole (sfree
 * it). */
static char *audit_event_string(const char *v)
{
    char *s = dupstr(v), *p;
    for (p = s; *p; p++)
        if ((unsigned char)*p < 0x20)
            *p = '?';
    return s;
}

/* Every insertion string is whole. A record longer than the Event Log
 * takes is refused by ReportEvent itself; that refusal is not reported,
 * as no other failure of this best-effort sink is. */
static void audit_write_event(const char *ev, const char *const *keys,
                              const char *const *vals, int n)
{
    char *strings[1 + AUDIT_MAX_FIELDS];
    const char *result = "";
    const struct audit_kind *kind = NULL;
    WORD ns = 0, id = 0;
    HANDLE h;
    size_t k;
    int i, j;

    for (i = 0; i < n; i++)
        if (!strcmp(keys[i], "result"))
            result = vals[i];
    for (k = 0; k < sizeof(audit_kinds) / sizeof(audit_kinds[0]); k++)
        if (!strcmp(audit_kinds[k].ev, ev)) {
            kind = &audit_kinds[k];
            break;
        }

    if (kind) {
        id = kind->id;
        strings[ns++] = audit_event_string(ev);
        for (j = 0; kind->fields[j] &&
                    ns < (WORD)(sizeof(strings) / sizeof(strings[0])); j++) {
            const char *v = "";
            for (i = 0; i < n; i++)
                if (!strcmp(keys[i], kind->fields[j])) {
                    v = vals[i];
                    break;
                }
            strings[ns++] = audit_event_string(v);
        }
    } else {
        /* A kind the table does not list: the whole record, as the file
         * has it, in the one string of message 0 - never dropped. */
        strbuf *sb = strbuf_new();
        audit_put_record(sb, ev, keys, vals, n);
        strings[ns++] = strbuf_to_str(sb);
    }

    h = RegisterEventSourceA(NULL, KITTY_AUDIT_EVENT_SOURCE);
    if (h) {                                 /* best effort, like the file */
        ReportEventA(h, (WORD)(kitty_audit_is_warning(ev, result) ?
                               EVENTLOG_WARNING_TYPE :
                               EVENTLOG_INFORMATION_TYPE),
                     0, id, kitty_wev_user_sid(), ns, 0,
                     (const char **)strings, NULL);
        DeregisterEventSource(h);
    }
    while (ns > 0)
        sfree(strings[--ns]);
}

void kitty_audit(const char *ev, ...)
{
    const char *keys[AUDIT_MAX_FIELDS], *vals[AUDIT_MAX_FIELDS];
    int n = 0;
    va_list ap;

    if (!kitty_audit_enabled() || !ev || !*ev)
        return;

    va_start(ap, ev);
    for (;;) {
        const char *k = va_arg(ap, const char *);
        const char *v;
        if (!k)
            break;
        v = va_arg(ap, const char *);
        if (!v || !*v)
            continue;                        /* optional field, absent */
        if (n < AUDIT_MAX_FIELDS) {
            keys[n] = k;
            vals[n] = v;
            n++;
        }
    }
    va_end(ap);

    /* Independent sinks: each gets every record it is switched on for. */
    if ((g_sinks & KITTY_AUDIT_SINK_FILE) && g_path && g_path[0])
        audit_write_file(ev, keys, vals, n);
    if (g_sinks & KITTY_AUDIT_SINK_EVENTLOG)
        audit_write_event(ev, keys, vals, n);
}
