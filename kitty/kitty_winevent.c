/*
 * kitty_winevent.c - the Windows Event Log writer for the 2xx-4xx and 9xx
 * records; see kitty_winevent.h for the contract and kitty_eventids.h for
 * the IDs and their fields.
 *
 * One record is one ReportEvent: %1 the kind, then the fields in the
 * table's order, each its own insertion string - which is what lets Event
 * Viewer filters, Windows Event Forwarding and a SIEM select on them by
 * position. Best effort throughout: a log that cannot be opened costs the
 * record, never the session.
 *
 * Windows XP era on purpose (RegisterEventSourceA, ReportEventA), like
 * kitty_renameguard.c beside it in `utils`.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define KITTY_EVENTIDS_TABLE
#include "kitty_winevent.h"

#define WEV_MAX_FIELDS 16
#define WEV_NSRC 5

static unsigned g_block;            /* KITTY_EVB_*; 0 until configured */
static int g_on;
static const void *g_seat;
static char g_session[256], g_host[256], g_proto[32];
static int g_port = -1;
static int g_open;                  /* a session-open is out, its close not */

static struct {
    int set;
    unsigned offset;
    char host[256];
    int port;
    char keytype[64];
    char fp[160];
    char proxy[16];
} g_pend;

/* An authentication attempt the server has not answered yet. */
static struct {
    int set;
    char host[256];
    int port;
    char user[256];
    char method[32];
    char methods[256];
    char proxy[16];
} g_auth;

/* SSH proxy hops (kitty_wev_hop_add) */
#define WEV_NHOPS 8
static const void *g_hops[WEV_NHOPS];

void kitty_wev_hop_add(const void *seat)
{
    int i;
    for (i = 0; i < WEV_NHOPS; i++)
        if (!g_hops[i]) {
            g_hops[i] = seat;
            return;
        }
}

void kitty_wev_hop_remove(const void *seat)
{
    int i;
    for (i = 0; i < WEV_NHOPS; i++)
        if (g_hops[i] == seat)
            g_hops[i] = NULL;
}

int kitty_wev_is_hop(const void *seat)
{
    int i;
    if (!seat)
        return 0;
    for (i = 0; i < WEV_NHOPS; i++)
        if (g_hops[i] == seat)
            return 1;
    return 0;
}

/* One registered source per name, kept for the life of the process. */
static struct { const char *name; HANDLE h; } g_src[WEV_NSRC];

static void wev_copy(char *dst, size_t sz, const char *s)
{
    size_t n = 0;
    if (s)
        for (; s[n] && n + 1 < sz; n++)
            dst[n] = s[n];
    dst[n] = '\0';
}

static const struct kitty_eventid *wev_find(unsigned id)
{
    size_t i;
    for (i = 0; i < sizeof(kitty_eventids) / sizeof(kitty_eventids[0]); i++)
        if (kitty_eventids[i].id == id)
            return &kitty_eventids[i];
    return NULL;
}

static HANDLE wev_source(const char *name)
{
    int i;
    for (i = 0; i < WEV_NSRC; i++) {
        if (!g_src[i].name) {
            g_src[i].name = name;
            g_src[i].h = RegisterEventSourceA(NULL, name);
            return g_src[i].h;
        }
        if (!strcmp(g_src[i].name, name)) {
            if (!g_src[i].h)
                g_src[i].h = RegisterEventSourceA(NULL, name);
            return g_src[i].h;
        }
    }
    return NULL;
}

void *kitty_wev_user_sid(void)
{
    /* 0 not looked up, 1 being looked up, 2 done; the SID points into buf */
    static volatile LONG state;
    static PSID sid;
    static DWORD buf[(SECURITY_MAX_SID_SIZE + sizeof(TOKEN_USER)) /
                     sizeof(DWORD) + 1];
    HANDLE tok;
    DWORD got = 0;

    if (state == 2)
        return sid;
    if (InterlockedCompareExchange(&state, 1, 0) != 0)
        return state == 2 ? sid : NULL;   /* another thread is at it */
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        TOKEN_USER *tu = (TOKEN_USER *)buf;
        if (GetTokenInformation(tok, TokenUser, tu, sizeof(buf), &got) &&
            IsValidSid(tu->User.Sid))
            sid = tu->User.Sid;
        CloseHandle(tok);
    }
    InterlockedExchange(&state, 2);
    return sid;
}

/* A copy of v with control bytes made ?, taken from the pool; "" once the
 * pool is spent. */
static const char *wev_scrub(char **pool, char *end, const char *v)
{
    char *s = *pool;
    if (!v || *pool >= end - 1)
        return "";
    for (; *v && *pool < end - 1; v++)
        *(*pool)++ = ((unsigned char)*v < 0x20) ? '?' : *v;
    *(*pool)++ = '\0';
    return s;
}

/* Decimal without the C library's 64-bit printf, which the 32-bit build's
 * msvcrt does not provide on old Windows. */
static void wev_u64(char *buf, size_t sz, unsigned long long v)
{
    char tmp[24];
    int n = 0;
    size_t i = 0;
    do {
        tmp[n++] = (char)('0' + (int)(v % 10));
        v /= 10;
    } while (v && n < (int)sizeof(tmp));
    while (n > 0 && i + 1 < sz)
        buf[i++] = tmp[--n];
    buf[i] = '\0';
}

/* The record: the kind, then every field of the table - "pid" this
 * process's, the others from vals in order (vals holds every field but
 * pid; a missing one is ""). */
static void wev_write(unsigned id, WORD type, const char *const *vals, int n)
{
    const struct kitty_eventid *e = wev_find(id);
    const char *strings[1 + WEV_MAX_FIELDS];
    char pool[8192], *pp = pool, *pend = pool + sizeof(pool);
    char pid[16];
    const char *f;
    int i = 0;
    WORD ns = 0;
    HANDLE h;

    if (!e || !e->kind)
        return;
    snprintf(pid, sizeof(pid), "%lu", (unsigned long)GetCurrentProcessId());
    strings[ns++] = e->kind;
    for (f = e->fields; *f && ns < (WORD)(sizeof(strings) / sizeof(strings[0])); ) {
        const char *name = f;
        while (*f && *f != ' ')
            f++;
        if (f - name == 3 && !strncmp(name, "pid", 3)) {
            strings[ns++] = pid;
        } else {
            strings[ns++] = wev_scrub(&pp, pend, i < n ? vals[i] : NULL);
            i++;
        }
        while (*f == ' ')
            f++;
    }
    h = wev_source(e->source);
    if (!h)
        return;
    ReportEventA(h, type, 0, id, kitty_wev_user_sid(), ns, 0, strings, NULL);
}

void kitty_wev_configure(unsigned block, int enabled)
{
    if (g_block)
        return;
    if (block != KITTY_EVB_KITTY && block != KITTY_EVB_CLI &&
        block != KITTY_EVB_KITTYGEN)
        return;
    g_block = block;
    g_on = enabled ? 1 : 0;
}

int kitty_wev_enabled(void) { return g_on; }

void kitty_wev_main_seat(const void *seat) { g_seat = seat; }

int kitty_wev_is_main(const void *seat)
{
    return !g_seat || seat == g_seat;
}

void kitty_wev_session_name(const char *session)
{
    wev_copy(g_session, sizeof(g_session), session);
}

void kitty_wev_target(const char *host, int port, const char *proto)
{
    wev_copy(g_host, sizeof(g_host), host);
    g_port = port;
    wev_copy(g_proto, sizeof(g_proto), proto);
}

/* The session blocks only: the kittygen block has none of these kinds. */
static int wev_session_block(void)
{
    return g_on && (g_block == KITTY_EVB_KITTY || g_block == KITTY_EVB_CLI);
}

static void wev_port(char *buf, size_t sz, int port)
{
    if (port >= 0)
        snprintf(buf, sz, "%d", port);
    else
        buf[0] = '\0';
}

void kitty_wev_session_open(const void *seat, const char *addr,
                            const char *from)
{
    char port[16];
    const char *v[6];
    if (!wev_session_block() || !kitty_wev_is_main(seat) || g_open)
        return;
    g_open = 1;
    wev_port(port, sizeof(port), g_port);
    v[0] = g_session; v[1] = g_host; v[2] = port; v[3] = g_proto;
    v[4] = addr; v[5] = from;
    wev_write(g_block + KITTY_EVO_SESSION_OPEN, EVENTLOG_INFORMATION_TYPE,
              v, 6);
}

void kitty_wev_session_close(const char *result, const char *reason,
                             int exitcode)
{
    char port[16], code[16];
    const char *v[6];
    if (!wev_session_block())
        return;
    /* The server ended the connection instead of answering the last
     * attempt (its disconnect message, or it closed the connection): that
     * attempt was refused, and the close message is why. A failure on this
     * side (a network error) leaves the attempt's outcome unknown and
     * writes nothing. */
    if (g_auth.set && result &&
        (!strcmp(result, "remote-closed") ||
         (reason && strstr(reason, "Remote side sent disconnect message"))))
        kitty_wev_auth_failed(g_auth.host, g_auth.port, g_auth.user,
                              g_auth.method, reason, g_auth.methods,
                              g_auth.proxy);
    g_auth.set = 0;
    if (!g_open)
        return;
    g_open = 0;
    wev_port(port, sizeof(port), g_port);
    wev_port(code, sizeof(code), exitcode);
    v[0] = g_session; v[1] = g_host; v[2] = port; v[3] = result;
    v[4] = reason; v[5] = code;
    wev_write(g_block + KITTY_EVO_SESSION_CLOSE,
              (result && !strcmp(result, "fatal")) ?
              EVENTLOG_WARNING_TYPE : EVENTLOG_INFORMATION_TYPE, v, 6);
}

const char *kitty_wev_fp_hash(const char *fp)
{
    const char *sp;
    if (!fp)
        return "";
    sp = strrchr(fp, ' ');
    return sp ? sp + 1 : fp;
}

const char *kitty_wev_close_result(const char *msg)
{
    /* PuTTY's texts: "Remote side unexpectedly closed network connection",
     * "Server unexpectedly closed network connection" */
    return (msg && strstr(msg, "closed network connection")) ?
        "remote-closed" : "fatal";
}

void kitty_wev_hostkey(unsigned offset, const char *host, int port,
                       const char *keytype, const char *fp,
                       const char *basis_or_result, const char *proxy)
{
    char p[16], kt[64], h1[80];
    const char *v[6], *sp, *hash;
    int warn;
    if (!wev_session_block())
        return;
    if (offset != KITTY_EVO_HOSTKEY_KNOWN && offset != KITTY_EVO_HOSTKEY_NEW &&
        offset != KITTY_EVO_HOSTKEY_MISMATCH)
        return;
    warn = offset == KITTY_EVO_HOSTKEY_MISMATCH ||
        (basis_or_result && !strcmp(basis_or_result, "refused"));
    wev_port(p, sizeof(p), port);
    /* The key type as the fingerprint names it ("ssh-rsa 2048 SHA256:...":
     * ssh-rsa), not the host-key cache's id ("rsa2"); the caller's when the
     * fingerprint has no type in front. An SSH-1 key's fingerprint is
     * "<bits> <MD5 hash>" (rsa_ssh1_fingerprint) with no type at all: its
     * type is rsa1, as kittygen's records name an SSH-1 key, and its hash
     * the second word. */
    sp = fp ? strchr(fp, ' ') : NULL;
    hash = kitty_wev_fp_hash(fp);
    if (sp && fp[0] >= '0' && fp[0] <= '9') {
        const char *he = strchr(sp + 1, ' ');
        size_t hl = he ? (size_t)(he - (sp + 1)) : strlen(sp + 1);
        wev_copy(kt, sizeof(kt), "rsa1");
        if (hl >= sizeof(h1))
            hl = sizeof(h1) - 1;
        memcpy(h1, sp + 1, hl);
        h1[hl] = '\0';
        hash = h1;
    } else if (sp && (size_t)(sp - fp) < sizeof(kt)) {
        memcpy(kt, fp, (size_t)(sp - fp));
        kt[sp - fp] = '\0';
    } else {
        wev_copy(kt, sizeof(kt), keytype);
    }
    v[0] = host; v[1] = p; v[2] = kt; v[3] = hash;
    v[4] = basis_or_result; v[5] = proxy;
    wev_write(g_block + offset,
              warn ? EVENTLOG_WARNING_TYPE : EVENTLOG_INFORMATION_TYPE, v, 6);
}

void kitty_wev_hostkey_pending(unsigned offset, const char *host, int port,
                               const char *keytype, const char *fp,
                               const char *proxy)
{
    if (!wev_session_block())
        return;
    g_pend.set = 1;
    g_pend.offset = offset;
    wev_copy(g_pend.host, sizeof(g_pend.host), host);
    g_pend.port = port;
    wev_copy(g_pend.keytype, sizeof(g_pend.keytype), keytype);
    wev_copy(g_pend.fp, sizeof(g_pend.fp), fp);
    wev_copy(g_pend.proxy, sizeof(g_pend.proxy), proxy);
}

void kitty_wev_hostkey_answer(int accepted)
{
    if (!g_pend.set)
        return;
    g_pend.set = 0;
    kitty_wev_hostkey(g_pend.offset, g_pend.host, g_pend.port,
                      g_pend.keytype, g_pend.fp,
                      accepted ? "accepted" : "refused", g_pend.proxy);
}

/* The `for` field of a login record that is not a proxy's own: empty for
 * the connection's own login, session for a jump host's (a hop only ever
 * carries the session; its forwardings are cleared). */
static const char *wev_login_for(const char *proxy)
{
    return (proxy && *proxy) ? "session" : "";
}

/* What the next proxied connection is for (kitty_wev_next_conn_for). */
static const char *g_next_for;

void kitty_wev_next_conn_for(const char *purpose) { g_next_for = purpose; }

const char *kitty_wev_take_conn_for(void)
{
    const char *f = g_next_for;
    g_next_for = NULL;
    return f;
}

void kitty_wev_auth_ok_for(const char *host, int port, const char *user,
                           const char *method, const char *fp,
                           const char *proxy, const char *purpose)
{
    char p[16];
    const char *v[7];
    g_auth.set = 0;
    if (!wev_session_block())
        return;
    wev_port(p, sizeof(p), port);
    v[0] = host; v[1] = p; v[2] = user; v[3] = method;
    v[4] = fp ? kitty_wev_fp_hash(fp) : "";
    v[5] = proxy; v[6] = purpose;
    wev_write(g_block + KITTY_EVO_AUTH_OK, EVENTLOG_INFORMATION_TYPE, v, 7);
}

void kitty_wev_auth_ok(const char *host, int port, const char *user,
                       const char *method, const char *fp,
                       const char *proxy)
{
    kitty_wev_auth_ok_for(host, port, user, method, fp, proxy,
                          wev_login_for(proxy));
}

void kitty_wev_proxy_used(const char *proxy, const char *host, int port,
                          const char *user, int credentials,
                          const char *purpose)
{
    char p[16];
    const char *v[6];
    if (!wev_session_block())
        return;
    wev_port(p, sizeof(p), port);
    v[0] = proxy; v[1] = host; v[2] = p; v[3] = user;
    v[4] = credentials ? "yes" : "no";
    v[5] = purpose;
    wev_write(g_block + KITTY_EVO_PROXY_USED, EVENTLOG_INFORMATION_TYPE,
              v, 6);
}

void kitty_wev_auth_failed_for(const char *host, int port, const char *user,
                               const char *method, const char *reason,
                               const char *methods, const char *proxy,
                               const char *purpose)
{
    char p[16];
    const char *v[8];
    g_auth.set = 0;
    if (!wev_session_block())
        return;
    wev_port(p, sizeof(p), port);
    v[0] = host; v[1] = p; v[2] = user; v[3] = method; v[4] = reason;
    v[5] = methods; v[6] = proxy; v[7] = purpose;
    wev_write(g_block + KITTY_EVO_AUTH_FAILED, EVENTLOG_WARNING_TYPE, v, 8);
}

void kitty_wev_auth_failed(const char *host, int port, const char *user,
                           const char *method, const char *reason,
                           const char *methods, const char *proxy)
{
    kitty_wev_auth_failed_for(host, port, user, method, reason, methods,
                              proxy, wev_login_for(proxy));
}

void kitty_wev_auth_sent(const char *host, int port, const char *user,
                         const char *method, const char *methods,
                         const char *proxy)
{
    if (!wev_session_block())
        return;
    wev_copy(g_auth.proxy, sizeof(g_auth.proxy), proxy);
    g_auth.set = 1;
    wev_copy(g_auth.host, sizeof(g_auth.host), host);
    g_auth.port = port;
    wev_copy(g_auth.user, sizeof(g_auth.user), user);
    wev_copy(g_auth.method, sizeof(g_auth.method), method);
    wev_copy(g_auth.methods, sizeof(g_auth.methods), methods);
}

void kitty_wev_auth_settled(void)
{
    g_auth.set = 0;
}

void kitty_wev_farend_refused(const char *request, const char *reason,
                              const char *detail)
{
    const char *v[5];
    if (!g_on || g_block != KITTY_EVB_KITTY)
        return;
    v[0] = g_session; v[1] = g_host; v[2] = request; v[3] = reason;
    v[4] = detail;
    wev_write(g_block + KITTY_EVO_FAREND_REFUSED, EVENTLOG_WARNING_TYPE, v, 5);
}

/* A transfer's local file as a full path (relative names are taken from
 * the current folder, as the tool itself took them); unchanged when that
 * cannot be made. */
static const char *wev_full(char *buf, DWORD sz, const char *path)
{
    DWORD n;
    if (!path || !*path)
        return "";
    n = GetFullPathNameA(path, sz, buf, NULL);
    return (n && n < sz) ? buf : path;
}

void kitty_wev_transfer(int ok, const char *user, const char *direction,
                        const char *local, const char *remote,
                        long long bytes, const char *reason)
{
    char nb[24], full[MAX_PATH * 2];
    const char *v[8];
    if (!g_on || g_block != KITTY_EVB_CLI)
        return;
    nb[0] = '\0';
    if (bytes >= 0)
        wev_u64(nb, sizeof(nb), (unsigned long long)bytes);
    v[0] = g_session; v[1] = g_host; v[2] = user; v[3] = direction;
    v[4] = wev_full(full, sizeof(full), local); v[5] = remote; v[6] = nb;
    v[7] = reason;
    if (ok)
        wev_write(g_block + KITTY_EVO_TRANSFER_DONE,
                  EVENTLOG_INFORMATION_TYPE, v, 7);
    else
        wev_write(g_block + KITTY_EVO_TRANSFER_FAILED,
                  EVENTLOG_WARNING_TYPE, v, 8);
}

void kitty_wev_term_transfer(int ok, const char *user, const char *method,
                             const char *direction, const char *local,
                             const char *remote, long long bytes,
                             const char *reason)
{
    char p[16], nb[24], full[MAX_PATH * 2];
    const char *v[10];
    if (!g_on || g_block != KITTY_EVB_KITTY)
        return;
    wev_port(p, sizeof(p), g_port);
    nb[0] = '\0';
    if (bytes >= 0)
        wev_u64(nb, sizeof(nb), (unsigned long long)bytes);
    v[0] = g_session; v[1] = g_host; v[2] = p; v[3] = user; v[4] = method;
    v[5] = direction; v[6] = wev_full(full, sizeof(full), local);
    v[7] = remote; v[8] = nb; v[9] = reason;
    if (ok)
        wev_write(g_block + KITTY_EVO_TERM_TRANSFER_DONE,
                  EVENTLOG_INFORMATION_TYPE, v, 9);
    else
        wev_write(g_block + KITTY_EVO_TERM_TRANSFER_FAILED,
                  EVENTLOG_WARNING_TYPE, v, 10);
}

void kitty_wev_report(unsigned offset, int warning,
                      const char *const *vals, int n)
{
    if (!g_on)
        return;
    wev_write(g_block + offset,
              warning ? EVENTLOG_WARNING_TYPE : EVENTLOG_INFORMATION_TYPE,
              vals, n);
}

#define WEV_SUITE_KEY \
    "SYSTEM\\CurrentControlSet\\Services\\EventLog\\Application\\" \
    KITTY_EVSRC_SUITE

int kitty_wev_suite_registered(void)
{
    static int registered = -1;
    if (registered < 0) {
        HKEY k;
        registered = 0;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, WEV_SUITE_KEY, 0,
                          KEY_QUERY_VALUE, &k) == ERROR_SUCCESS) {
            registered = RegQueryValueExA(k, "EventMessageFile", NULL, NULL,
                                          NULL, NULL) == ERROR_SUCCESS;
            RegCloseKey(k);
        }
    }
    return registered;
}

void kitty_wev_suite(unsigned id, unsigned type,
                     const char *const *vals, int n, const char *text)
{
    if (id < KITTY_EVB_SUITE || id > KITTY_EVB_SUITE + 99)
        return;
    if (!kitty_wev_suite_registered() && text) {
        /* a portable copy: the line as it reads, event ID 0 */
        HANDLE h = wev_source(KITTY_EVSRC_SUITE);
        const char *strings[1];
        if (!h)
            return;
        strings[0] = text;
        ReportEventA(h, (WORD)type, 0, 0, kitty_wev_user_sid(), 1, 0,
                     strings, NULL);
        return;
    }
    wev_write(id, (WORD)type, vals, n);
}
