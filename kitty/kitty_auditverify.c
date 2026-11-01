/*
 * kitty_auditverify.c - kageant -verifylog and the agent log window's
 * Verify button: the log's files from disk, the anchors from the Windows
 * Event Log and kitty_auditchain_verify over both. See
 * kitty_auditverify.h.
 *
 * The anchors are event 171 of the source KiTTY++_agent, in the log
 * KiTTY++_Agent where the system-wide installation registered it and in
 * the Application log elsewhere. The other agent events (100-170, 172)
 * are read too: each is checked by its signature against its run and its
 * line in the file. Vista on, wevtapi.dll (EvtQuery), also
 * for an exported .evtx file. Windows XP has no wevtapi: there the classic
 * event log API (OpenEventLog, ReadEventLog) reads this computer's logs.
 */
#include "putty.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "kitty_auditchain.h"
#include "kitty_auditverify.h"
#include "kitty_auditlog.h"     /* KITTY_AUDIT_EVENT_SOURCE, _LOG */
#include "kitty_text.h"

#define AV_ANCHOR_ID 171
#define AV_MAXDATA   40              /* insertion strings kept per event */

/* wevtapi.dll, loaded at run time (Vista on). The constants are winevt.h's. */
typedef HANDLE AV_EVT;
#define AV_QUERY_CHANNEL   0x1
#define AV_QUERY_FILE      0x2
#define AV_QUERY_FORWARD   0x100
#define AV_RENDER_XML      1
static AV_EVT (WINAPI *p_EvtQuery)(AV_EVT, LPCWSTR, LPCWSTR, DWORD);
static BOOL (WINAPI *p_EvtNext)(AV_EVT, DWORD, AV_EVT *, DWORD, DWORD, PDWORD);
static BOOL (WINAPI *p_EvtRender)(AV_EVT, AV_EVT, DWORD, DWORD, PVOID, PDWORD,
                                  PDWORD);
static BOOL (WINAPI *p_EvtClose)(AV_EVT);

static int av_wevtapi(void)
{
    static int state = -1;
    if (state < 0) {
        HMODULE m = load_system32_dll("wevtapi.dll");
        state = 0;
        if (m) {
            p_EvtQuery = (AV_EVT (WINAPI *)(AV_EVT, LPCWSTR, LPCWSTR, DWORD))
                (void (*)(void))GetProcAddress(m, "EvtQuery");
            p_EvtNext = (BOOL (WINAPI *)(AV_EVT, DWORD, AV_EVT *, DWORD, DWORD,
                                         PDWORD))
                (void (*)(void))GetProcAddress(m, "EvtNext");
            p_EvtRender = (BOOL (WINAPI *)(AV_EVT, AV_EVT, DWORD, DWORD, PVOID,
                                           PDWORD, PDWORD))
                (void (*)(void))GetProcAddress(m, "EvtRender");
            p_EvtClose = (BOOL (WINAPI *)(AV_EVT))
                (void (*)(void))GetProcAddress(m, "EvtClose");
            state = p_EvtQuery && p_EvtNext && p_EvtRender && p_EvtClose;
        }
    }
    return state;
}

/* ---- what is collected ---- */

struct av_set {
    struct kitty_auditchain_anchor *a;
    size_t n, size;
    struct kitty_auditchain_event *e;    /* the other agent events */
    size_t ne, esize;
    char **owned;                    /* every string the anchors point to */
    size_t nowned, ownedsize;
    char *since;                     /* the oldest record read, UTC */
};

static const char *av_keep(struct av_set *s, char *str)
{
    sgrowarray(s->owned, s->ownedsize, s->nowned);
    s->owned[s->nowned++] = str;
    return str;
}

/* One agent event: 171 to the anchors (its fields by their names in the
 * row of kitty_eventids.h), every other ID to the events. time is kept. */
static void av_add(struct av_set *s, const char *log, unsigned long long rec,
                   unsigned id, char *time, char **data, int ndata)
{
    const char **kept;
    int i;
    av_keep(s, time);
    kept = snewn(ndata ? ndata : 1, const char *);
    av_keep(s, (char *)kept);
    for (i = 0; i < ndata; i++)
        kept[i] = av_keep(s, dupstr(data[i]));
    if (id == AV_ANCHOR_ID) {
        struct kitty_auditchain_anchor *a;
        sgrowarray(s->a, s->size, s->n);
        a = &s->a[s->n++];
        memset(a, 0, sizeof(*a));
        a->log = log;
        a->record = rec;
        a->time = time;
        kitty_auditchain_anchor_from_event(kept, (size_t)ndata, a->v);
    } else {
        struct kitty_auditchain_event *e;
        sgrowarray(s->e, s->esize, s->ne);
        e = &s->e[s->ne++];
        memset(e, 0, sizeof(*e));
        e->log = log;
        e->record = rec;
        e->time = time;
        e->id = id;
        e->s = kept;
        e->n = (size_t)ndata;
    }
}

/* ---- the XML of one event (EvtRender) ---- */

/* A run of the XML as this machine's ANSI code page - the bytes the agent
 * gave ReportEventA - with the five entities and &#N; undone. */
static char *av_text(const WCHAR *p, size_t n)
{
    strbuf *w = strbuf_new();
    WCHAR *u;
    char *out;
    size_t i;
    int len;
    for (i = 0; i < n; i++) {
        WCHAR c = p[i];
        if (c == L'&') {
            const WCHAR *e = p + i;
            size_t j = i;
            while (j < n && p[j] != L';')
                j++;
            if (j - i == 3 && !wcsncmp(e, L"&lt", 3))
                c = L'<';
            else if (j - i == 3 && !wcsncmp(e, L"&gt", 3))
                c = L'>';
            else if (j - i == 4 && !wcsncmp(e, L"&amp", 4))
                c = L'&';
            else if (j - i == 5 && !wcsncmp(e, L"&quot", 5))
                c = L'"';
            else if (j - i == 5 && !wcsncmp(e, L"&apos", 5))
                c = L'\'';
            else if (j - i >= 3 && e[1] == L'#') {
                unsigned long v = 0;
                size_t k = i + 2;
                int hex = k < j && (p[k] == L'x' || p[k] == L'X');
                if (hex)
                    k++;
                for (; k < j; k++)
                    v = v * (hex ? 16 : 10) +
                        (unsigned long)(p[k] <= L'9' ? p[k] - L'0' :
                                        (p[k] | 0x20) - L'a' + 10);
                c = (WCHAR)v;
            }
            if (j < n && c != L'&')
                i = j;
        }
        put_data(w, &c, sizeof(c));
    }
    {
        WCHAR z = 0;
        put_data(w, &z, sizeof(z));
    }
    u = (WCHAR *)w->u;
    len = WideCharToMultiByte(CP_ACP, 0, u, -1, NULL, 0, NULL, NULL);
    out = snewn(len > 0 ? len : 1, char);
    out[0] = '\0';
    if (len > 0)
        WideCharToMultiByte(CP_ACP, 0, u, -1, out, len, NULL, NULL);
    strbuf_free(w);
    return out;
}

/* The text of the first element `tag` from *from on; NULL when none. A
 * self-closing <tag/> is "". *from moves past the element. */
static char *av_element(const WCHAR *x, const WCHAR **from, const WCHAR *tag)
{
    size_t tl = wcslen(tag);
    const WCHAR *p = *from;
    (void)x;
    while ((p = wcschr(p, L'<')) != NULL) {
        if (!wcsncmp(p + 1, tag, tl) &&
            (p[1 + tl] == L'>' || p[1 + tl] == L' ' || p[1 + tl] == L'/')) {
            const WCHAR *gt = wcschr(p, L'>'), *end;
            if (!gt)
                return NULL;
            if (gt[-1] == L'/') {
                *from = gt + 1;
                return dupstr("");
            }
            end = wcsstr(gt + 1, L"</");
            if (!end)
                return NULL;
            *from = end;
            return av_text(gt + 1, (size_t)(end - (gt + 1)));
        }
        p++;
    }
    return NULL;
}

/* SystemTime='2026-11-01T10:00:00.1234567Z' as 2026-11-01T10:00:00Z */
static char *av_systemtime(const WCHAR *x)
{
    const WCHAR *p = wcsstr(x, L"SystemTime=");
    char *t, *dot;
    if (!p)
        return dupstr("");
    p += 12;
    t = av_text(p, 19 <= wcslen(p) ? 19 : wcslen(p));
    dot = strchr(t, '.');
    if (dot)
        *dot = '\0';
    {
        char *r = dupprintf("%sZ", t);
        sfree(t);
        return r;
    }
}

static void av_event_xml(struct av_set *s, const char *log, const WCHAR *x)
{
    const WCHAR *p = x;
    const WCHAR *pi = x;
    char *idt = av_element(x, &pi, L"EventID");
    char *rec = av_element(x, &p, L"EventRecordID");
    char *data[AV_MAXDATA];
    unsigned id = idt ? (unsigned)strtoul(idt, NULL, 10) : 0;
    int nd = 0, i;
    const WCHAR *ed = wcsstr(x, L"<EventData");
    if (ed) {
        const WCHAR *q = ed;
        char *d;
        while (nd < (int)lenof(data) && (d = av_element(x, &q, L"Data")) != NULL)
            data[nd++] = d;
    }
    if (nd > 1 && id >= 100 && id <= 199)
        av_add(s, log, rec ? strtoull(rec, NULL, 10) : 0, id,
               av_systemtime(x), data, nd);
    for (i = 0; i < nd; i++)
        sfree(data[i]);
    sfree(rec);
    sfree(idt);
}

/* Every agent event of a log or a file; 1 when the query ran. */
static int av_query(struct av_set *s, const char *log, const WCHAR *path,
                    DWORD flags)
{
    static const WCHAR q[] =
        L"*[System[Provider[@Name='" L"KiTTY++_agent" L"'] and "
        L"EventID>=100 and EventID<=199]]";
    AV_EVT rs = p_EvtQuery(NULL, path, q, flags | AV_QUERY_FORWARD);
    AV_EVT ev[16];
    DWORD got, i;
    if (!rs)
        return 0;
    while (p_EvtNext(rs, lenof(ev), ev, INFINITE, 0, &got)) {
        for (i = 0; i < got; i++) {
            DWORD used = 0, props = 0;
            WCHAR *buf;
            p_EvtRender(NULL, ev[i], AV_RENDER_XML, 0, NULL, &used, &props);
            buf = snewn(used / sizeof(WCHAR) + 2, WCHAR);
            if (p_EvtRender(NULL, ev[i], AV_RENDER_XML, used, buf, &used,
                            &props))
                av_event_xml(s, log, buf);
            sfree(buf);
            p_EvtClose(ev[i]);
        }
    }
    p_EvtClose(rs);
    return 1;
}

/* The time of the oldest record of a log, for "rolled out". */
static char *av_oldest(const WCHAR *path, DWORD flags)
{
    AV_EVT rs = p_EvtQuery(NULL, path, L"*", flags | AV_QUERY_FORWARD), ev;
    DWORD got = 0, used = 0, props = 0;
    char *t = NULL;
    if (!rs)
        return NULL;
    if (p_EvtNext(rs, 1, &ev, INFINITE, 0, &got) && got == 1) {
        WCHAR *buf;
        p_EvtRender(NULL, ev, AV_RENDER_XML, 0, NULL, &used, &props);
        buf = snewn(used / sizeof(WCHAR) + 2, WCHAR);
        if (p_EvtRender(NULL, ev, AV_RENDER_XML, used, buf, &used, &props))
            t = av_systemtime(buf);
        sfree(buf);
        p_EvtClose(ev);
    }
    p_EvtClose(rs);
    return t;
}

/* Windows XP: the classic event log API. */
static int av_query_classic(struct av_set *s, const char *log,
                            const char *logname)
{
    HANDLE h = OpenEventLogA(NULL, logname);
    DWORD size = 65536, got, need;
    char *buf;
    int first = 1;
    if (!h)
        return 0;
    buf = snewn(size, char);
    for (;;) {
        if (!ReadEventLogA(h, EVENTLOG_SEQUENTIAL_READ | EVENTLOG_FORWARDS_READ,
                           0, buf, size, &got, &need)) {
            if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && need > size) {
                size = need;
                buf = sresize(buf, size, char);
                continue;
            }
            break;
        }
        {
            DWORD off = 0;
            while (off + sizeof(EVENTLOGRECORD) <= got) {
                EVENTLOGRECORD *r = (EVENTLOGRECORD *)(buf + off);
                const char *src = (const char *)(r + 1);
                time_t tt = (time_t)r->TimeGenerated;
                struct tm *tm = gmtime(&tt);
                char *when = tm ? dupprintf("%04d-%02d-%02dT%02d:%02d:%02dZ",
                                            tm->tm_year + 1900, tm->tm_mon + 1,
                                            tm->tm_mday, tm->tm_hour,
                                            tm->tm_min, tm->tm_sec) :
                    dupstr("");
                if (first) {
                    if (!s->since)
                        s->since = dupstr(when);
                    first = 0;
                }
                if ((r->EventID & 0xFFFF) >= 100 &&
                    (r->EventID & 0xFFFF) <= 199 &&
                    !strcmp(src, KITTY_AUDIT_EVENT_SOURCE)) {
                    char *data[AV_MAXDATA];
                    const char *p = (const char *)r + r->StringOffset;
                    int nd = 0;
                    WORD k;
                    for (k = 0; k < r->NumStrings && nd < (int)lenof(data);
                         k++) {
                        data[nd++] = (char *)p;
                        p += strlen(p) + 1;
                    }
                    if (nd > 1) {
                        av_add(s, log, r->RecordNumber,
                               (unsigned)(r->EventID & 0xFFFF), when, data,
                               nd);
                        when = NULL;
                    }
                }
                sfree(when);
                if (!r->Length)
                    break;
                off += r->Length;
            }
        }
    }
    sfree(buf);
    CloseEventLog(h);
    return 1;
}

static int av_log_exists(const char *log)
{
    HKEY k;
    char *key = dupprintf("SYSTEM\\CurrentControlSet\\Services\\EventLog\\%s",
                          log);
    int ok = RegOpenKeyExA(HKEY_LOCAL_MACHINE, key, 0, KEY_QUERY_VALUE, &k) ==
        ERROR_SUCCESS;
    if (ok)
        RegCloseKey(k);
    sfree(key);
    return ok;
}

static WCHAR *av_wide(const char *s)
{
    int n = MultiByteToWideChar(CP_ACP, 0, s, -1, NULL, 0);
    WCHAR *w = snewn(n > 0 ? n : 1, WCHAR);
    w[0] = 0;
    if (n > 0)
        MultiByteToWideChar(CP_ACP, 0, s, -1, w, n);
    return w;
}

/* ---- the files ---- */

/* A whole file; NULL with *err set when it cannot be read. */
static char *av_read(const char *path, size_t *len, DWORD *err)
{
    HANDLE h = CreateFileA(path, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE |
                           FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    LARGE_INTEGER sz;
    char *buf;
    DWORD got = 0;
    *len = 0;
    if (h == INVALID_HANDLE_VALUE) {
        *err = GetLastError();
        return NULL;
    }
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart > 0x7FFFFFFF) {
        *err = GetLastError();
        CloseHandle(h);
        return NULL;
    }
    buf = snewn((size_t)sz.QuadPart + 1, char);
    if (sz.QuadPart && (!ReadFile(h, buf, (DWORD)sz.QuadPart, &got, NULL) ||
                        got != (DWORD)sz.QuadPart)) {
        *err = GetLastError();
        sfree(buf);
        CloseHandle(h);
        return NULL;
    }
    CloseHandle(h);
    *len = (size_t)sz.QuadPart;
    return buf;
}

static char *av_reason(DWORD err)
{
    char *msg = NULL, *r;
    DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM |
                             FORMAT_MESSAGE_ALLOCATE_BUFFER |
                             FORMAT_MESSAGE_IGNORE_INSERTS, NULL, err, 0,
                             (LPSTR)&msg, 0, NULL);
    while (n && (msg[n - 1] == '\r' || msg[n - 1] == '\n' || msg[n - 1] == '.'))
        msg[--n] = '\0';
    r = n ? dupstr(msg) : dupprintf("error %lu", (unsigned long)err);
    if (msg)
        LocalFree(msg);
    return r;
}

int kitty_auditverify(const char *path, const char *evtx,
                      const char *live_run, strbuf *out)
{
    struct kitty_auditchain_file files[100];
    char *bufs[100], *names[100];
    size_t nf = 0, i;
    struct av_set s;
    struct kitty_auditchain_verify_in in;
    const char *base;
    char *where = NULL, *whereagent = NULL, *whereapp = NULL, *full;
    int gen, code, have = 0;
    DWORD err = 0, n;

    /* the anchors carry the full path kageant wrote to */
    n = GetFullPathNameA(path, 0, NULL, NULL);
    full = snewn(n ? n + 1 : 1, char);
    if (!n || GetFullPathNameA(path, n + 1, full, NULL) >= n + 1) {
        sfree(full);
        full = dupstr(path);
    }
    path = full;
    base = strrchr(path, '\\');
    base = base ? base + 1 : path;
    memset(&s, 0, sizeof(s));
    /* the anchors and the events, BEFORE the files: a record the running
     * kageant writes meanwhile is then in the file, never an anchor beyond
     * its end */
    if (evtx) {
        WCHAR *w = av_wide(evtx);
        where = dupprintf(KT_KAC_WHERE_EVTX_FMT, evtx);
        if (av_wevtapi())
            have = av_query(&s, where, w, AV_QUERY_FILE);
        if (!have) {
            char *why = av_reason(GetLastError());
            put_fmt(out, KT_KA_CLI_ANCHORS_UNREADABLE_FMT, evtx, why);
            put_datapl(out, PTRLEN_LITERAL("\r\n"));
            sfree(why);
        } else {
            s.since = av_oldest(w, AV_QUERY_FILE);
        }
        sfree(w);
    } else {
        int agentlog = av_log_exists(KITTY_AUDIT_EVENT_LOG);
        whereagent = dupprintf(KT_KAC_WHERE_FMT, KITTY_AUDIT_EVENT_LOG);
        whereapp = dupprintf(KT_KAC_WHERE_FMT, "Application");
        where = agentlog ?
            dupprintf(KT_KAC_WHERE_FMT,
                      KITTY_AUDIT_EVENT_LOG " and Application") :
            dupstr(whereapp);
        if (av_wevtapi()) {
            if (agentlog && av_query(&s, whereagent, L"" KITTY_AUDIT_EVENT_LOG,
                                     AV_QUERY_CHANNEL)) {
                have = 1;
                s.since = av_oldest(L"" KITTY_AUDIT_EVENT_LOG,
                                    AV_QUERY_CHANNEL);
            }
            if (av_query(&s, whereapp, L"Application", AV_QUERY_CHANNEL)) {
                if (!have)
                    s.since = av_oldest(L"Application", AV_QUERY_CHANNEL);
                have = 1;
            }
        } else {
            if (agentlog &&
                av_query_classic(&s, whereagent, KITTY_AUDIT_EVENT_LOG))
                have = 1;
            if (av_query_classic(&s, whereapp, "Application"))
                have = 1;
        }
    }

    /* the generations, oldest first, then the active file */
    for (gen = 99; gen >= 0; gen--) {
        char *p = gen ? dupprintf("%s.%d", path, gen) : dupstr(path);
        size_t len;
        char *data;
        if (gen && GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES) {
            sfree(p);
            continue;
        }
        data = av_read(p, &len, &err);
        if (!data) {
            char *why = av_reason(err);
            put_fmt(out, KT_KA_CLI_VERIFY_UNREADABLE_FMT, p, why);
            put_datapl(out, PTRLEN_LITERAL("\r\n"));
            sfree(why);
            sfree(p);
            for (i = 0; i < nf; i++) {
                sfree(bufs[i]);
                sfree(names[i]);
            }
            for (i = 0; i < s.nowned; i++)
                sfree(s.owned[i]);
            sfree(s.owned);
            sfree(s.a);
            sfree(s.e);
            sfree(s.since);
            sfree(where);
            sfree(whereagent);
            sfree(whereapp);
            sfree(full);
            return 2;
        }
        /* the log a kageant writes now: a line it is writing at this
         * moment is not complete yet - it is left out */
        if (!gen && live_run)
            while (len && !(len >= 2 && data[len - 2] == '\r' &&
                            data[len - 1] == '\n'))
                len--;
        names[nf] = gen ? dupprintf("%s.%d", base, gen) : dupstr(base);
        bufs[nf] = data;
        files[nf].name = names[nf];
        files[nf].data = data;
        files[nf].len = len;
        nf++;
        sfree(p);
    }

    memset(&in, 0, sizeof(in));
    in.files = files;
    in.nfiles = nf;
    in.have_anchors = have;
    in.anchors = s.a;
    in.nanchors = s.n;
    in.events = s.e;
    in.nevents = s.ne;
    in.anchors_since = s.since;
    in.anchors_where = where ? where : KT_KAC_WHERE_NONE;
    in.path = path;
    in.live_run = live_run;
    code = kitty_auditchain_verify(&in, out);

    for (i = 0; i < nf; i++) {
        sfree(bufs[i]);
        sfree(names[i]);
    }
    for (i = 0; i < s.nowned; i++)
        sfree(s.owned[i]);
    sfree(s.owned);
    sfree(s.a);
    sfree(s.e);
    sfree(s.since);
    sfree(where);
    sfree(whereagent);
    sfree(whereapp);
    sfree(full);
    return code;
}
