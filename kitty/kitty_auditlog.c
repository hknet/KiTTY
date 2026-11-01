/*
 * KiTTY: the audit log's sinks - see kitty_auditlog.h for the format
 * contract. The file sink is deliberately boring: open-append per event,
 * rotate by size before the write that would cross the cap, expunge old
 * generations by age at rotation time, and NEVER lose the active file - a
 * failed rotation is itself logged and writing continues. The Event Log
 * sink reports the same record as one event: its kind picks the event ID,
 * its fields fill the insertion strings in a fixed order per ID.
 *
 * The file is a chain (kitty_auditchain.h): every line carries seq, prev
 * and sig, the signature with this run's key over itself plus prev. The
 * anchor records (ev="logchain") go to the Windows Event Log whatever the
 * sinks are, as long as the log is on. A record that repeats the one
 * before it is counted, not written: one summary (ev="repeated") stands
 * for the repeats (kitty_auditchain_repeat).
 */

#include "putty.h"            /* dupstr, dupprintf, strbuf */
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "ssh.h"
#include "kitty_buildlabel.h"   /* a test build's head interval (below) */
#include "kitty_auditlog.h"
#include "kitty_auditchain.h"
#include "kitty_protkey.h"      /* the run key, encrypted between anchors */
#include "kitty_winevent.h"     /* kitty_wev_user_sid: the record's user */

static char *g_path = NULL;        /* the whole path; NULL until configured */
static int  g_enabled = 0;
static int  g_sinks = KITTY_AUDIT_SINK_FILE;
static int  g_maxkb = 5120;
static int  g_keep = 3;
static int  g_expiredays = 90;
static int  g_rotate_warned = 0;   /* one failure event per process */

#define AUDIT_MAX_FIELDS 16
/* the chain's fields an event carries on top of the caller's: ts, prev,
 * seq, sig */
#define AUDIT_CHAIN_FIELDS 4

/* ---- the chain's state: one writer, kageant (kitty_auditlog.h) ---- */
static struct kitty_auditchain_writer g_chain;
static char *g_chain_path = NULL;  /* the file the chain is written to */
static void (*g_random)(void *, size_t) = NULL;
static KittyProtKey *g_key_prot = NULL;    /* the run key, encrypted */
static struct ssh_key *g_key_plain = NULL; /* only without CryptProtectMemory */
static int g_have_key = 0;
static char g_run[KITTY_AUDITCHAIN_RUNLEN + 1];
static char g_logkey[KITTY_AUDITCHAIN_B64HASH + 1];
static int g_dirty = 0;            /* records since the last anchor */
static DWORD g_anchor_tick = 0;
static struct kitty_auditchain_repeat g_repeat;   /* repeats counted */

static void audit_chain_sync(void);
static void audit_repeat_flush(int force);

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
    audit_chain_sync();
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
        "sign-refused", "remove-refused", "store-damaged",
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

/* ev="..." key="value" ... - the record without its timestamp, whole: a
 * long value never pushes the fields after it out of the line. The
 * quoting (kitty_auditchain_put_quoted) is what keeps a value from the
 * pipe from forging a field or a line. */
static void audit_put_record(strbuf *sb, const char *ev,
                             const char *const *keys,
                             const char *const *vals, int n)
{
    int i;
    put_datapl(sb, PTRLEN_LITERAL("ev="));
    kitty_auditchain_put_quoted(sb, ev);
    for (i = 0; i < n; i++) {
        put_byte(sb, ' ');
        put_dataz(sb, keys[i]);
        put_byte(sb, '=');
        kitty_auditchain_put_quoted(sb, vals[i]);
    }
}

static char *audit_gen_name(int gen)
{
    return dupprintf("%s.%d", g_path, gen);
}

/* Rotate if the active file has crossed the cap. Shift .1->.2 etc from the
 * oldest down, expunge generations past their age, then move the active
 * file to .1. Any failure leaves the ACTIVE file untouched and writing.
 * Returns 1 when the active file moved; *expired = the generations the
 * age rule deleted. */
static int audit_maybe_rotate(int *expired)
{
    WIN32_FILE_ATTRIBUTE_DATA fad;
    char *from, *to;
    int i;

    *expired = 0;
    if (!GetFileAttributesExA(g_path, GetFileExInfoStandard, &fad))
        return 0;                           /* no file yet: nothing to do */
    if (fad.nFileSizeHigh == 0 &&
        fad.nFileSizeLow <= (DWORD)g_maxkb * 1024)
        return 0;

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
                if (mt < now && now - mt > cutoff && DeleteFileA(from))
                    (*expired)++;
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
    return i != 0;
}

/* Append bytes to a file, whole; 1 when every byte is written. */
static int audit_append(const char *path, const char *data, size_t len)
{
    HANDLE h;
    DWORD written = 0;
    int ok = 0;
    h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {         /* never block the agent on IO */
        ok = WriteFile(h, data, (DWORD)len, &written, NULL) &&
            written == (DWORD)len;
        CloseHandle(h);
    }
    return ok;
}

static void audit_timestamp(char *ts)
{
    SYSTEMTIME st;
    GetSystemTime(&st);                      /* UTC on disk, by contract */
    snprintf(ts, 32, "%04u-%02u-%02uT%02u:%02u:%02uZ",
             st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond);
}

/* One line: the record, the chain fields (with key, the signature), CRLF.
 * The chain advances only when the line is on disk whole. */
static int audit_write_line(strbuf *line, struct ssh_key *key,
                            char seq[24], char sig[89])
{
    size_t body;
    int ok;
    seq[0] = sig[0] = '\0';
    if (g_chain.active)
        kitty_auditchain_seal(&g_chain, line, key, seq, sig);
    body = line->len;
    put_datapl(line, PTRLEN_LITERAL("\r\n"));
    ok = audit_append(g_path, line->s, line->len);
    if (ok && g_chain.active)
        kitty_auditchain_commit(&g_chain, line->s, body);
    return ok;
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
 * appended at its end, here, in the table of kitty/kitty_eventids.h and in
 * the message (tools/check-kitty-events.py holds the three together and
 * every list to the last release's, docs/event-fields-baseline.txt).
 *
 * seq, sig, ts and prev end every list: the record's line in the file's
 * chain, its signature, its time and the hash of the line before it (all
 * empty when the file is not written). With them the signed line is
 * rebuilt from the event alone (kitty_auditchain_event_line): a record's
 * fields are written in the order of its list (audit_order_fields).
 */
static const char *const af_use[] = {
    "fp", "comment", "result", "reason", "req", "reqpath", "pid",
    "seq", "sig", "ts", "prev", NULL
};
/* sign and confirm: also the server of a verified session-bind */
static const char *const af_sign[] = {
    "fp", "comment", "result", "reason", "req", "reqpath", "pid",
    "dest", "fwd", "seq", "sig", "ts", "prev", NULL
};
/* a verified session-bind: the server bound to */
static const char *const af_bind[] = {
    "result", "dest", "forwarding", "req", "reqpath", "pid", "seq", "sig", "ts", "prev",
    NULL
};
static const char *const af_add[] = {
    "fp", "comment", "result", "reason", "req", "reqpath", "pid", "path",
    "restrict", "seq", "sig", "ts", "prev", NULL
};
/* a request refused because of the key's destination rules (ssh-add -h,
 * the rule editor), or the rule file found damaged at the start */
static const char *const af_restricted[] = {
    "fp", "comment", "result", "reason", "req", "reqpath", "pid",
    "dest", "fwd", "rule", "seq", "sig", "ts", "prev", NULL
};
/* the rule editor's OK changed a key's destination rules */
static const char *const af_rules[] = {
    "fp", "comment", "result", "reason", "req", "reqpath", "pid", "restrict",
    "seq", "sig", "ts", "prev", NULL
};
static const char *const af_load[] = {
    "path", "result", "enc", "seq", "sig", "ts", "prev", NULL
};
static const char *const af_forget[] = { "path", "result", "seq", "sig", "ts", "prev", NULL };
static const char *const af_retry[] = {
    "trigger", "loaded", "refused", "absent", "broken", "seq", "sig", "ts", "prev", NULL
};
static const char *const af_hello[] = {
    "path", "result", "detail", "seq", "sig", "ts", "prev", NULL
};
static const char *const af_protect[] = {
    "path", "dest", "result", "detail", "err", "seq", "sig", "ts", "prev", NULL
};
static const char *const af_keywarn[] = {
    "path", "warning", "seq", "sig", "ts", "prev", NULL
};
static const char *const af_decrypt[] = {
    "fp", "comment", "result", "trigger", "req", "reqpath", "pid",
    "seq", "sig", "ts", "prev", NULL
};
static const char *const af_result[] = { "result", "seq", "sig", "ts", "prev", NULL };
/* a refused request (kageant_refused_hook): the agent's reason, the
 * requester, and the server of a verified session-bind */
static const char *const af_request[] = {
    "result", "reason", "req", "reqpath", "pid", "dest", "fwd", "seq", "sig", "ts", "prev",
    NULL
};
/* an anchor of the chain: kitty_auditchain_fieldnames, in that order */
static const char *const af_logchain[] = {
    "result", "run", "logkey", "file", "fragment", "value", "rotation",
    "keep", "expired", "seq", "sig", "ts", "prev", NULL
};
/* the summary of a record's repeats (kitty_auditchain_repeat) */
static const char *const af_repeated[] = {
    "count", "first", "last", "of", "seq", "sig", "ts", "prev", NULL
};

static const struct audit_kind {
    const char *ev;
    WORD id;
    const char *const *fields;
} audit_kinds[] = {
    { "sign",            100, af_sign },
    { "confirm",         101, af_sign },
    { "session-bind",    102, af_bind },
    { "request",         103, af_request },
    { "restricted",      104, af_restricted },
    { "add",             110, af_add },
    { "loadkey",         111, af_load },
    { "forget-path",     112, af_forget },
    { "retry",           113, af_retry },
    { "remove",          114, af_use },
    { "remove-all",      115, af_use },
    { "rules",           116, af_rules },
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
    { "logchain",        171, af_logchain },
    { "repeated",        172, af_repeated },
};

static const struct audit_kind *audit_find_kind(const char *ev)
{
    size_t k;
    for (k = 0; k < sizeof(audit_kinds) / sizeof(audit_kinds[0]); k++)
        if (!strcmp(audit_kinds[k].ev, ev))
            return &audit_kinds[k];
    return NULL;
}

/* A record's fields in the order of its kind's list, the ones the list
 * does not hold after them in the caller's order: the file line and the
 * event carry the fields in the same order, so the signed line can be
 * rebuilt from the event. */
static void audit_order_fields(const char *ev, const char **keys,
                               const char **vals, int n)
{
    const char *k2[AUDIT_MAX_FIELDS], *v2[AUDIT_MAX_FIELDS];
    char used[AUDIT_MAX_FIELDS];
    const struct audit_kind *kind = audit_find_kind(ev);
    int i, j, m = 0;
    if (!kind || n > AUDIT_MAX_FIELDS)
        return;
    memset(used, 0, sizeof(used));
    for (j = 0; kind->fields[j]; j++)
        for (i = 0; i < n; i++)
            if (!used[i] && !strcmp(keys[i], kind->fields[j])) {
                k2[m] = keys[i];
                v2[m++] = vals[i];
                used[i] = 1;
                break;
            }
    for (i = 0; i < n; i++)
        if (!used[i]) {
            k2[m] = keys[i];
            v2[m++] = vals[i];
        }
    for (i = 0; i < n; i++) {
        keys[i] = k2[i];
        vals[i] = v2[i];
    }
}

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
 * as no other failure of this best-effort sink is.
 *
 * A kind the table does not list is not written to the Event Log at all
 * (the file still has it): every Event Log record carries an ID of its own
 * with its fields, which is what filters, forwarding and a SIEM select on.
 * tools/check-kitty-events.py fails the build on a kitty_audit kind that is
 * missing from audit_kinds, so this only guards a kind built at run time. */
static void audit_write_event(const char *ev, const char *const *keys,
                              const char *const *vals, int n)
{
    char *strings[1 + AUDIT_MAX_FIELDS];
    const char *result = "";
    const struct audit_kind *kind = audit_find_kind(ev);
    WORD ns = 0;
    HANDLE h;
    int i, j;

    if (!kind)
        return;

    for (i = 0; i < n; i++)
        if (!strcmp(keys[i], "result"))
            result = vals[i];
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

    h = RegisterEventSourceA(NULL, KITTY_AUDIT_EVENT_SOURCE);
    if (h) {                                 /* best effort, like the file */
        ReportEventA(h, (WORD)(kitty_audit_is_warning(ev, result) ?
                               EVENTLOG_WARNING_TYPE :
                               EVENTLOG_INFORMATION_TYPE),
                     0, kind->id, kitty_wev_user_sid(), ns, 0,
                     (const char **)strings, NULL);
        DeregisterEventSource(h);
    }
    while (ns > 0)
        sfree(strings[--ns]);
}

/* ---- the run key ---- */

void kitty_audit_set_random(void (*fn)(void *buf, size_t len))
{
    g_random = fn;
}

/* One key per kageant run, made at the first start of the chain, in this
 * process's memory only (encrypted with CryptProtectMemory between
 * anchors where Windows has it). Never written anywhere. */
static int audit_run_key(void)
{
    unsigned char seed[KITTY_AUDITCHAIN_SEEDLEN], rid[16],
        pub[KITTY_AUDITCHAIN_PUBLEN];
    struct ssh_key *key;
    int i;
    if (g_have_key)
        return 1;
    if (!g_random)
        return 0;
    g_random(seed, sizeof(seed));
    g_random(rid, sizeof(rid));
    for (i = 0; i < 16; i++)
        snprintf(g_run + 2 * i, 3, "%02x", rid[i]);
    key = kitty_auditchain_key(seed);
    if (!key || !kitty_auditchain_public(seed, pub)) {
        smemclr(seed, sizeof(seed));
        if (key)
            ssh_key_free(key);
        return 0;
    }
    smemclr(seed, sizeof(seed));
    kitty_auditchain_b64(pub, sizeof(pub), g_logkey);
    g_key_prot = kitty_protkey_from_key(key);
    if (g_key_prot)
        ssh_key_free(key);
    else
        g_key_plain = key;          /* no worse than the keys the agent holds */
    g_have_key = 1;
    return 1;
}

static struct ssh_key *audit_key_take(void)
{
    return g_key_prot ? kitty_protkey_to_temp_key(g_key_prot) : g_key_plain;
}

static void audit_key_drop(struct ssh_key *k)
{
    if (g_key_prot && k)
        ssh_key_free(k);
}

const char *kitty_audit_run_id(void)
{
    return g_have_key ? g_run : NULL;
}

/* ---- the anchors ---- */

/* An anchor record: in the file (signed, chained) and in the Windows Event
 * Log, the latter whatever the sinks, so the file-only setting has its
 * anchors too. Only once the line is in the file: an anchor pins a line. */
static void audit_anchor(const char *result, const char *fragment,
                         const char *value, unsigned rotation,
                         unsigned keep, unsigned expired)
{
    const char *v[KAC_NFIELDS];
    const char *keys[KAC_NFIELDS], *vals[KAC_NFIELDS];
    char ts[32], seq[24], sig[89], prev[KITTY_AUDITCHAIN_B64HASH + 1];
    char rot[16], kp[16], ex[16];
    strbuf *line;
    struct ssh_key *key;
    int i, n = 0, ok;

    if (!g_chain.active)
        return;
    audit_repeat_flush(1);           /* the repeats before the anchor */
    memset(v, 0, sizeof(v));
    snprintf(rot, sizeof(rot), "%u", rotation);
    snprintf(kp, sizeof(kp), "%u", keep);
    snprintf(ex, sizeof(ex), "%u", expired);
    v[KAC_RESULT] = result;
    v[KAC_RUN] = g_run;
    v[KAC_LOGKEY] = g_logkey;
    v[KAC_FILE] = g_chain_path;
    v[KAC_FRAGMENT] = fragment;
    v[KAC_VALUE] = value;
    v[KAC_ROTATION] = rotation ? rot : NULL;
    v[KAC_KEEP] = rotation ? kp : NULL;
    v[KAC_EXPIRED] = expired ? ex : NULL;
    audit_timestamp(ts);
    snprintf(prev, sizeof(prev), "%s", g_chain.prev[0] ? g_chain.prev : "none");

    line = strbuf_new();
    kitty_auditchain_anchor_body(line, ts, v);
    key = audit_key_take();
    if (!key) {                      /* the key cannot be had: no anchor */
        strbuf_free(line);
        return;
    }
    ok = audit_write_line(line, key, seq, sig);
    audit_key_drop(key);
    strbuf_free(line);
    if (!ok)
        return;
    g_dirty = 0;
    g_anchor_tick = GetTickCount();

    v[KAC_TS] = ts;
    v[KAC_PREV] = prev;
    v[KAC_SEQ] = seq;
    v[KAC_SIG] = sig;
    for (i = 0; i < KAC_NFIELDS; i++)
        if (v[i] && *v[i]) {
            keys[n] = kitty_auditchain_fieldnames[i];
            vals[n++] = v[i];
        }
    audit_write_event("logchain", keys, vals, n);
}

/* The bytes at the start and at the end of the active file, for the chain
 * to continue from. 0 when the file is missing or cannot be read. */
static int audit_read_ends(struct kitty_auditchain_tail *t, unsigned *rotation)
{
    HANDLE h;
    LARGE_INTEGER size;
    size_t want = KITTY_AUDITCHAIN_TAIL;
    memset(t, 0, sizeof(*t));
    *rotation = 0;
    h = CreateFileA(g_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0) {
        CloseHandle(h);
        return 0;
    }
    {
        /* the first line: a rotate record carries this log's rotations */
        char *buf = snewn(KITTY_AUDITCHAIN_TAIL, char);
        DWORD got = 0;
        if (ReadFile(h, buf, KITTY_AUDITCHAIN_TAIL, &got, NULL))
            *rotation = kitty_auditchain_first_rotation(buf, got);
        sfree(buf);
    }
    for (;;) {
        unsigned long long sz = (unsigned long long)size.QuadPart;
        size_t len = sz < want ? (size_t)sz : want;
        int whole = len == sz;
        char *buf = snewn(len ? len : 1, char);
        LARGE_INTEGER at;
        DWORD got = 0;
        at.QuadPart = (LONGLONG)(sz - len);
        if (!SetFilePointerEx(h, at, NULL, FILE_BEGIN) ||
            !ReadFile(h, buf, (DWORD)len, &got, NULL) || got != len) {
            sfree(buf);
            CloseHandle(h);
            return 0;
        }
        kitty_auditchain_tail(buf, len, whole, t);
        sfree(buf);
        if (!t->need_more || whole || want > 256u * 1024u * 1024u)
            break;
        want *= 4;                     /* a last line longer than the tail */
    }
    CloseHandle(h);
    return 1;
}

/* The chain starts on the active file: from its last complete line, or
 * from nothing. A fragment a crash left after that line is closed with
 * CRLF (append only, never a rewrite) and accounted for by the resumed
 * record. */
static void audit_chain_begin(void)
{
    struct kitty_auditchain_tail t;
    unsigned rotation;
    char frag[24];

    if (!audit_run_key())
        return;
    memset(&g_chain, 0, sizeof(g_chain));
    /* a new chain: the first record of it is written in full */
    kitty_auditchain_repeat_forget(&g_repeat);
    if (audit_read_ends(&t, &rotation) && t.have_line) {
        snprintf(g_chain.prev, sizeof(g_chain.prev), "%s", t.head);
        g_chain.seq = t.last_seq + 1;
    } else {
        strcpy(g_chain.prev, "none");
        g_chain.seq = t.last_seq + 1;
    }
    g_chain.rotation = rotation;
    sfree(g_chain_path);
    g_chain_path = dupstr(g_path);
    g_chain.active = 1;
    if (t.fragment && audit_append(g_path, "\r\n", 2)) {
        snprintf(frag, sizeof(frag), "%lu", (unsigned long)t.fragment);
        audit_anchor("resumed", frag, NULL, 0, 0, 0);
    } else {
        audit_anchor("start", NULL, NULL, 0, 0, 0);
    }
}

/* After a configuration change: the chain runs while the file is written. */
static void audit_chain_sync(void)
{
    int want = g_enabled && (g_sinks & KITTY_AUDIT_SINK_FILE) &&
        g_path && g_path[0];
    if (!want) {
        g_chain.active = 0;
        return;
    }
    if (g_chain.active && g_chain_path && !strcmp(g_chain_path, g_path))
        return;
    audit_chain_begin();
}

unsigned long kitty_audit_chain_head_ms(void)
{
#ifdef KITTY_TEST_BUILD_LABEL
    /* a TEST BUILD takes the interval in ms from KITTY_LOGCHAIN_HEAD_MS, so
     * a harness need not wait 15 minutes for a head anchor */
    const char *ms = getenv("KITTY_LOGCHAIN_HEAD_MS");
    if (ms && atoi(ms) >= 1000)
        return (unsigned long)atoi(ms);
#endif
    return KITTY_AUDITCHAIN_HEAD_MS;
}

void kitty_audit_chain_tick(void)
{
    if (g_enabled)
        audit_repeat_flush(0);       /* KITTY_AUDITCHAIN_REPEAT_MS after the
                                      * first repeat */
    if (g_chain.active && g_dirty && g_enabled &&
        GetTickCount() - g_anchor_tick >= kitty_audit_chain_head_ms())
        audit_anchor("head", NULL, NULL, 0, 0, 0);
}

void kitty_audit_chain_config(const char *value)
{
    if (g_enabled)
        audit_anchor("config", NULL, value, 0, 0, 0);
}

void kitty_audit_chain_end(void)
{
    if (g_enabled)
        audit_repeat_flush(1);       /* also without the file */
    if (g_chain.active && g_enabled)
        audit_anchor("stop", NULL, NULL, 0, 0, 0);
    kitty_auditchain_repeat_forget(&g_repeat);
    g_chain.active = 0;
    if (g_key_prot)
        kitty_protkey_free(g_key_prot);
    if (g_key_plain)
        ssh_key_free(g_key_plain);
    g_key_prot = NULL;
    g_key_plain = NULL;
    g_have_key = 0;
}

/* ---- one record ---- */

/* One record to its sinks: the file line (the file rotated first when it
 * is due), chained and signed, and the Event Log event, which carries the
 * line's seq, sig, ts and prev. keys and vals have room for
 * AUDIT_CHAIN_FIELDS more than n. seq gets the seq written, "" when the
 * file is not written. */
static void audit_emit(const char *ev, const char **keys, const char **vals,
                       int n, char seq[24])
{
    char sig[89], ts[32], prev[KITTY_AUDITCHAIN_B64HASH + 1];
    seq[0] = sig[0] = ts[0] = '\0';
    strcpy(prev, "none");
    /* Independent sinks: each gets every record it is switched on for. */
    if ((g_sinks & KITTY_AUDIT_SINK_FILE) && g_path && g_path[0]) {
        strbuf *line = strbuf_new();
        struct ssh_key *key;
        int expired;
        if (audit_maybe_rotate(&expired) && g_chain.active) {
            /* the record that triggers the move starts the new file,
             * after the rotate record that binds it to .1 */
            g_chain.rotation++;
            audit_anchor("rotate", NULL, NULL, g_chain.rotation,
                         (unsigned)g_keep, (unsigned)expired);
        }
        audit_timestamp(ts);
        if (g_chain.prev[0])
            snprintf(prev, sizeof(prev), "%s", g_chain.prev);
        put_fmt(line, "ts=%s ", ts);
        audit_put_record(line, ev, keys, vals, n);
        /* without the key (it could not be decrypted) the line is still
         * written: the verifier reports it unsigned, nothing is lost */
        key = g_chain.active ? audit_key_take() : NULL;
        if (audit_write_line(line, key, seq, sig) && g_chain.active)
            g_dirty = 1;
        else
            seq[0] = sig[0] = '\0';
        audit_key_drop(key);
        strbuf_free(line);
    }
    if (g_sinks & KITTY_AUDIT_SINK_EVENTLOG) {
        if (seq[0]) {
            keys[n] = "seq";
            vals[n++] = seq;
        }
        if (sig[0]) {
            keys[n] = "sig";
            vals[n++] = sig;
        }
        if (seq[0]) {
            keys[n] = "ts";
            vals[n++] = ts;
            keys[n] = "prev";
            vals[n++] = prev;
        }
        audit_write_event(ev, keys, vals, n);
    }
}

/* The summary of a record's repeats: ev="repeated", a chained, signed
 * record of its own. */
static void audit_summary(const struct kitty_auditchain_summary *s)
{
    const char *keys[4 + AUDIT_CHAIN_FIELDS], *vals[4 + AUDIT_CHAIN_FIELDS];
    char seq[24];
    int n = 0;
    if (!kitty_audit_enabled())
        return;
    keys[n] = "count";
    vals[n++] = s->count;
    keys[n] = "first";
    vals[n++] = s->first;
    keys[n] = "last";
    vals[n++] = s->last;
    if (s->of[0]) {
        keys[n] = "of";
        vals[n++] = s->of;
    }
    audit_emit("repeated", keys, vals, n, seq);
}

/* The summary of the repeats counted, when there are any: with force
 * whatever their age, else KITTY_AUDITCHAIN_REPEAT_MS after the first. */
static void audit_repeat_flush(int force)
{
    struct kitty_auditchain_summary s;
    if (kitty_auditchain_repeat_flush(&g_repeat, GetTickCount(),
                                      KITTY_AUDITCHAIN_REPEAT_MS, force, &s))
        audit_summary(&s);
}

void kitty_audit(const char *ev, ...)
{
    const char *keys[AUDIT_MAX_FIELDS + AUDIT_CHAIN_FIELDS];
    const char *vals[AUDIT_MAX_FIELDS + AUDIT_CHAIN_FIELDS];
    struct kitty_auditchain_summary s;
    strbuf *body;
    char seq[24], ts[32];
    int n = 0, act;
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

    audit_order_fields(ev, keys, vals, n);
    /* A repeat of the last record written in full is only counted; a
     * different one is written after the summary of the repeats. */
    body = strbuf_new();
    audit_put_record(body, ev, keys, vals, n);
    audit_timestamp(ts);
    act = kitty_auditchain_repeat_record(&g_repeat, body->s, ts,
                                         GetTickCount(),
                                         KITTY_AUDITCHAIN_REPEAT_MS, &s);
    if (act & KAC_REPEAT_SUMMARY)
        audit_summary(&s);
    if (act & KAC_REPEAT_WRITE) {
        audit_emit(ev, keys, vals, n, seq);
        kitty_auditchain_repeat_written(&g_repeat, body->s, seq);
    }
    strbuf_free(body);
}
