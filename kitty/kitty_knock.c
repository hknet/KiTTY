/*
 * kitty_knock.c - port knocking before a connection (kitty_knock.h has the
 * sequence format and the route rules). In the network library, so
 * kitty.exe and the command-line tools run the same code; neither blocks:
 * the steps are driven by PuTTY's timers and callbacks, and each program
 * runs its own event loop meanwhile (window.c starts the backend from the
 * done callback, kitty_knock_cli.c runs the console loop to the end).
 */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <string.h>
#include "putty.h"
#include "network.h"
#include "kitty_knock.h"
#include "kitty_text.h"

#define KNOCK_GAP_MS          40     /* between two knocks, as before */
#define KNOCK_PROXY_GRACE_MS  1000   /* proxy reached: its request is out */
#define KNOCK_PROXY_CAP_MS    5000   /* one knock through a proxy, at most */
#define KNOCK_SSHPROXY_CAP_MS 15000  /* the same through an SSH jump host */

struct KittyKnock {
    KnockList list;
    size_t i;                   /* the next step */
    char *host;
    Conf *conf;
    LogContext *logctx;
    bool proxied;
    SockAddr *addr;             /* direct: the looked-up addresses */
    SOCKET *socks;              /* direct: the last TCP knock's sockets */
    size_t nsocks, socksize;
    Socket *psock;              /* proxied: the current knock */
    Plug plug;
    unsigned pport;
    bool pconnected, pended;
    char *perror;
    unsigned long next;         /* the pending timer */
    kitty_knock_done_fn done;
    kitty_knock_note_fn note;
    void *ctx;
};

static void knock_next(KittyKnock *k);
static void knock_log(KittyKnock *k, bool failure, const char *fmt, ...)
    PRINTF_LIKE(3, 4);

static void knock_log(KittyKnock *k, bool failure, const char *fmt, ...)
{
    va_list ap;
    char *msg;

    va_start(ap, fmt);
    msg = dupvprintf(fmt, ap);
    va_end(ap);
    if (k->logctx) {
        if (failure)
            logevent_always(k->logctx, msg);   /* also a tool's stderr */
        else
            logevent(k->logctx, msg);
    } else if (failure)
        fprintf(stderr, "%s\n", msg);
    sfree(msg);
}

/* a sequence refused: the failure line, also handed to the caller, which
 * then does not connect */
static void knock_refuse(KittyKnock *k, char **refusal, const char *fmt, ...)
    PRINTF_LIKE(3, 4);

static void knock_refuse(KittyKnock *k, char **refusal, const char *fmt, ...)
{
    va_list ap;
    char *msg;

    va_start(ap, fmt);
    msg = dupvprintf(fmt, ap);
    va_end(ap);
    knock_log(k, true, "%s", msg);
    if (refusal)
        *refusal = msg;
    else
        sfree(msg);
}

/* a failure the caller may also show (kitty_knock_set_note) */
static void knock_note(KittyKnock *k, const char *fmt, ...) PRINTF_LIKE(2, 3);

static void knock_note(KittyKnock *k, const char *fmt, ...)
{
    va_list ap;
    char *msg;

    va_start(ap, fmt);
    msg = dupvprintf(fmt, ap);
    va_end(ap);
    knock_log(k, true, "%s", msg);
    if (k->note)
        k->note(k->ctx, msg);
    sfree(msg);
}

static void knock_timer(void *ctx, unsigned long now);

static void knock_after(KittyKnock *k, unsigned ms)
{
    k->next = schedule_timer(ms ? (int)ms : 1, knock_timer, k);
}

static void knock_close_socks(KittyKnock *k)
{
    size_t i;

    for (i = 0; i < k->nsocks; i++) {
        struct linger lg = { 1, 0 };   /* no TIME_WAIT left behind */
        setsockopt(k->socks[i], SOL_SOCKET, SO_LINGER,
                   (const char *)&lg, sizeof(lg));
        closesocket(k->socks[i]);
    }
    k->nsocks = 0;
}

static void knock_close_proxy(KittyKnock *k)
{
    if (k->psock) {
        sk_close(k->psock);
        k->psock = NULL;
    }
    sfree(k->perror);
    k->perror = NULL;
}

static void knock_free(KittyKnock *k)
{
    knock_close_socks(k);
    knock_close_proxy(k);
    expire_timer_context(k);
    delete_callbacks_for_context(k);
    if (k->addr)
        sk_addr_free(k->addr);
    if (k->conf)
        conf_free(k->conf);
    sfree(k->host);
    sfree(k->socks);
    knock_list_free(&k->list);
    sfree(k);
}

/* ---- direct: every address, one socket each ---- */

struct knock_direct {
    KittyKnock *k;
    const KnockStep *st;
};

static void knock_addr_text(const struct sockaddr *sa, int salen,
                            char *buf, DWORD len)
{
    if (WSAAddressToStringA((LPSOCKADDR)sa, (DWORD)salen, NULL, buf, &len))
        snprintf(buf, len, "?");
}

static void knock_one_addr(void *vctx, const void *vsa, int salen)
{
    struct knock_direct *d = (struct knock_direct *)vctx;
    KittyKnock *k = d->k;
    bool tcp = d->st->kind == KNOCK_TCP;
    const char *proto = tcp ? "TCP" : "UDP";
    struct sockaddr_storage ss;
    char text[INET6_ADDRSTRLEN + 8];
    SOCKET s;
    int err = 0;

    if (salen <= 0 || (size_t)salen > sizeof(ss))
        return;
    memset(&ss, 0, sizeof(ss));
    memcpy(&ss, vsa, (size_t)salen);
    knock_addr_text((struct sockaddr *)&ss, salen, text, sizeof(text));
    if (ss.ss_family == AF_INET)
        ((struct sockaddr_in *)&ss)->sin_port = htons((u_short)d->st->port);
    else if (ss.ss_family == AF_INET6)
        ((struct sockaddr_in6 *)&ss)->sin6_port = htons((u_short)d->st->port);
    else
        return;

    s = socket(ss.ss_family, tcp ? SOCK_STREAM : SOCK_DGRAM,
               tcp ? IPPROTO_TCP : IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        err = WSAGetLastError();
    } else if (tcp) {
        u_long nb = 1;
        ioctlsocket(s, FIONBIO, &nb);
        /* the SYN leaves now; the socket stays open until the next step */
        if (connect(s, (struct sockaddr *)&ss, salen) != 0 &&
            WSAGetLastError() != WSAEWOULDBLOCK)
            err = WSAGetLastError();
        if (!err) {
            sgrowarray(k->socks, k->socksize, k->nsocks);
            k->socks[k->nsocks++] = s;
        } else
            closesocket(s);
    } else {
        if (sendto(s, "", 0, 0, (struct sockaddr *)&ss, salen) != 0)
            err = WSAGetLastError();
        closesocket(s);
    }
    if (err)
        knock_log(k, true, KT_KNOCK_SEND_FAILED_FMT, proto, d->st->port,
                  text, win_strerror(err));
    else
        knock_log(k, false, KT_KNOCK_SENT_FMT, proto, d->st->port, text);
}

/* ---- proxied: one connection to host:port through the proxy ---- */

static void knock_plug_log(Plug *p, Socket *s, PlugLogType type,
                           SockAddr *addr, int port, const char *msg,
                           int code)
{
    KittyKnock *k = container_of(p, KittyKnock, plug);

    (void)s; (void)addr; (void)port; (void)msg; (void)code;
    if (type == PLUGLOG_CONNECT_SUCCESS && !k->pconnected && !k->pended) {
        /* the proxy is reached (or, for a jump host, the channel is
         * open): its request for host:port is out within the grace */
        k->pconnected = true;
        expire_timer_context(k);
        knock_after(k, KNOCK_PROXY_GRACE_MS);
    }
}

static void knock_proxy_end(KittyKnock *k);

static void knock_proxy_end_cb(void *ctx)
{
    knock_proxy_end((KittyKnock *)ctx);
}

static void knock_plug_closing(Plug *p, PlugCloseType type,
                               const char *msg)
{
    KittyKnock *k = container_of(p, KittyKnock, plug);

    if (k->pended)
        return;
    k->pended = true;
    if (type != PLUGCLOSE_NORMAL && msg)
        k->perror = dupstr(msg);
    expire_timer_context(k);
    /* not sk_close from inside the socket's own callback */
    queue_toplevel_callback(knock_proxy_end_cb, k);
}

static void knock_plug_receive(Plug *p, int urgent, const char *data,
                               size_t len)
{
    (void)p; (void)urgent; (void)data; (void)len;
}

static void knock_plug_sent(Plug *p, size_t bufsize)
{
    (void)p; (void)bufsize;
}

static const PlugVtable knock_plugvt = {
    .log = knock_plug_log,
    .closing = knock_plug_closing,
    .receive = knock_plug_receive,
    .sent = knock_plug_sent,
};

static bool knock_ssh_proxy(Conf *conf)
{
    int t = conf_get_int(conf, CONF_proxy_type);
    return t == PROXY_SSH_TCPIP || t == PROXY_SSH_EXEC ||
           t == PROXY_SSH_SUBSYSTEM;
}

static void knock_proxied(KittyKnock *k, unsigned port)
{
    char *canon = NULL;
    const char *err;
    SockAddr *a;

    k->pport = port;
    k->pconnected = k->pended = false;
    a = name_lookup(k->host, (int)port, &canon, k->conf,
                    conf_get_int(k->conf, CONF_addressfamily), NULL,
                    "port knocking");
    sfree(canon);
    if ((err = sk_addr_error(a)) != NULL) {
        knock_note(k, KT_KNOCK_PROXY_FAILED_FMT, port, err);
        sk_addr_free(a);
        knock_after(k, KNOCK_GAP_MS);
        return;
    }
    k->plug.vt = &knock_plugvt;
    /* no Interactor: a proxy or jump host that needs a typed answer fails
     * here, with its line, rather than prompting for a knock */
    k->psock = new_connection(a, k->host, (int)port, false, true, false,
                              false, &k->plug, k->conf, NULL);
    if ((err = sk_socket_error(k->psock)) != NULL) {
        knock_note(k, KT_KNOCK_PROXY_FAILED_FMT, port, err);
        sk_close(k->psock);
        k->psock = NULL;
        knock_after(k, KNOCK_GAP_MS);
        return;
    }
    knock_after(k, knock_ssh_proxy(k->conf) ? KNOCK_SSHPROXY_CAP_MS
                                             : KNOCK_PROXY_CAP_MS);
}

static void knock_proxy_end(KittyKnock *k)
{
    if (k->pconnected)
        knock_log(k, false, KT_KNOCK_PROXY_SENT_FMT, k->pport, k->host);
    else if (k->perror)
        knock_note(k, KT_KNOCK_PROXY_FAILED_FMT, k->pport, k->perror);
    else
        knock_note(k, KT_KNOCK_PROXY_WAIT_FMT, k->pport,
                  (unsigned)(knock_ssh_proxy(k->conf) ? KNOCK_SSHPROXY_CAP_MS
                                                      : KNOCK_PROXY_CAP_MS)
                  / 1000u);
    knock_next(k);
}

/* ---- the sequence ---- */

static void knock_timer(void *ctx, unsigned long now)
{
    KittyKnock *k = (KittyKnock *)ctx;

    if (now != k->next)
        return;
    if (k->psock && !k->pended) {
        k->pended = true;
        knock_proxy_end(k);
    } else
        knock_next(k);
}

static void knock_next_cb(void *ctx)
{
    knock_next((KittyKnock *)ctx);
}

static void knock_next(KittyKnock *k)
{
    const KnockStep *st;

    knock_close_socks(k);
    knock_close_proxy(k);
    if (k->i >= k->list.n) {
        kitty_knock_done_fn done = k->done;
        void *ctx = k->ctx;
        knock_log(k, false, KT_KNOCK_DONE);
        knock_free(k);
        done(ctx);
        return;
    }
    st = &k->list.steps[k->i++];
    if (st->kind == KNOCK_PAUSE) {
        knock_log(k, false, KT_KNOCK_PAUSE_FMT, st->ms);
        knock_after(k, st->ms);
    } else if (k->proxied) {
        knock_proxied(k, st->port);     /* TCP only: UDP refused up front */
    } else {
        struct knock_direct d;
        d.k = k;
        d.st = st;
        kitty_sk_addr_each(k->addr, knock_one_addr, &d);
        knock_after(k, KNOCK_GAP_MS);
    }
}

KittyKnock *kitty_knock_start(Conf *conf, LogContext *logctx,
                              kitty_knock_done_fn done,
                              kitty_knock_note_fn note, void *ctx,
                              char **refusal)
{
    const char *seq = conf_get_str(conf, CONF_portknockingoptions);
    const char *host = conf_get_str(conf, CONF_host);
    int port = conf_get_int(conf, CONF_port);
    KittyKnock *k;
    size_t i;

    if (refusal)
        *refusal = NULL;
    if (!seq || !seq[0] || !host || !host[0] ||
        conf_get_int(conf, CONF_protocol) == PROT_SERIAL)
        return NULL;
    k = snew(KittyKnock);
    memset(k, 0, sizeof(*k));
    k->logctx = logctx;
    k->note = note;
    k->ctx = ctx;
    if (!knock_parse(seq, &k->list)) {
        if (k->list.too_many)
            knock_refuse(k, refusal, KT_KNOCK_TOO_MANY_FMT, KNOCK_MAX_STEPS);
        else
            knock_refuse(k, refusal, KT_KNOCK_BAD_FMT, k->list.bad);
        knock_free(k);
        return NULL;
    }
    if (k->list.n == 0) {
        knock_free(k);
        return NULL;
    }
    if (k->list.cut)
        knock_log(k, false, KT_KNOCK_PAUSE_CUT_FMT, k->list.cut,
                  KNOCK_PAUSE_MAX_MS / 1000u, KNOCK_PAUSE_MAX_MS / 1000u);

    k->proxied = kitty_proxy_applies(host, port, conf);
    if (k->proxied) {
        for (i = 0; i < k->list.n; i++)
            if (k->list.steps[i].kind == KNOCK_UDP) {
                /* no knock, the connection goes ahead */
                knock_note(k, KT_KNOCK_UDP_PROXY_FMT, k->list.steps[i].port);
                knock_free(k);
                return NULL;
            }
    } else {
        char *canon = NULL;
        const char *err;
        k->addr = sk_namelookup(host, &canon,
                                conf_get_int(conf, CONF_addressfamily));
        sfree(canon);
        if ((err = sk_addr_error(k->addr)) != NULL) {
            knock_log(k, true, KT_KNOCK_LOOKUP_FMT, host, err);
            knock_free(k);
            return NULL;
        }
    }
    k->host = dupstr(host);
    k->conf = conf_copy(conf);
    k->done = done;
    if (k->proxied)
        knock_log(k, false, KT_KNOCK_ROUTE_PROXY_FMT, host);
    /* the first step from the event loop: done never runs inside here */
    queue_toplevel_callback(knock_next_cb, k);
    return k;
}

void kitty_knock_cancel(KittyKnock *k)
{
    if (k)
        knock_free(k);
}
