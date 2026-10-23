/*
 * Network proxy abstraction in PuTTY
 *
 * A proxy layer, if necessary, wedges itself between the
 * network code and the higher level backend.
 *
 * Supported proxies: HTTP CONNECT, generic telnet, SOCKS 4 & 5
 */

#ifndef PUTTY_PROXY_H
#define PUTTY_PROXY_H

typedef struct ProxySocket ProxySocket;
typedef struct ProxyNegotiator ProxyNegotiator;
typedef struct ProxyNegotiatorVT ProxyNegotiatorVT;

struct ProxySocket {
    const char *error;

    Socket *sub_socket;
    Plug *plug;
    SockAddr *remote_addr;
    int remote_port;

    /* Parameters needed to make further connections to the proxy */
    SockAddr *proxy_addr;
    int proxy_port;
    bool proxy_privport, proxy_oobinline, proxy_nodelay, proxy_keepalive;

    bufchain pending_output_data;
    bufchain pending_oob_output_data;
    bufchain pending_input_data;
    bool pending_eof;

    bool freeze; /* should we freeze the underlying socket when
                  * we are done with the proxy negotiation? this
                  * simply caches the value of sk_set_frozen calls.
                  */

    ProxyNegotiator *pn; /* non-NULL if still negotiating */
    bufchain output_from_negotiator;

    /* configuration, used to look up proxy settings */
    Conf *conf;

    /* for interaction with the Seat */
    Interactor *clientitr;
    LogPolicy *clientlp;
    Seat *clientseat;
    /* KiTTY: whether this connection's proxy records (220, 221, 225) are
     * written - decided at creation (proxy_wev_decide) */
    bool kwev_ok;
    const char *kwev_for;   /* KiTTY: session, forward or x11 */

    Socket sock;
    Plug plugimpl;
    Interactor interactor;
};

struct ProxyNegotiator {
    const ProxyNegotiatorVT *vt;

    /* Standard fields for any ProxyNegotiator. new() and free() don't
     * have to set these up; that's done centrally, to save duplication. */
    ProxySocket *ps;
    bufchain *input;
    bufchain_sink output[1];
    Interactor *itr; /* NULL if we are not able to interact with the user */

    /* Set to report success during proxy negotiation.  */
    bool done;

    /* Set to report an error during proxy negotiation. The main
     * ProxySocket will free it, and will then guarantee never to call
     * process_queue again. */
    char *error;

    /* Set to report user abort during proxy negotiation.  */
    bool aborted;

    /* Set to request the centralised code to make a fresh connection
     * to the proxy server, e.g. because an HTTP proxy slammed the
     * connection shut after sending 407 Proxy Auth Required. */
    bool reconnect;

    /* KiTTY: event 225/325 - whether a user name or password went to
     * the proxy, and the user name sent (proxy_wev_note_credentials) */
    bool kwev_credentials;
    char kwev_user[256];
};

struct ProxyNegotiatorVT {
    ProxyNegotiator *(*new)(const ProxyNegotiatorVT *);
    void (*process_queue)(ProxyNegotiator *);
    void (*free)(ProxyNegotiator *);
    const char *type;
};

static inline ProxyNegotiator *proxy_negotiator_new(
    const ProxyNegotiatorVT *vt)
{ return vt->new(vt); }
static inline void proxy_negotiator_process_queue(ProxyNegotiator *pn)
{ pn->vt->process_queue(pn); }
static inline void proxy_negotiator_free(ProxyNegotiator *pn)
{ pn->vt->free(pn); }

extern const ProxyNegotiatorVT http_proxy_negotiator_vt;
extern const ProxyNegotiatorVT socks4_proxy_negotiator_vt;
extern const ProxyNegotiatorVT socks5_proxy_negotiator_vt;
extern const ProxyNegotiatorVT telnet_proxy_negotiator_vt;

/*
 * Centralised functions to allow ProxyNegotiators to get hold of a
 * prompts_t, and to deal with SeatPromptResults coming back.
 */
prompts_t *proxy_new_prompts(ProxySocket *ps);

/* KiTTY: event 221/321 for a proxy that refused our credentials (or wants
 * some we do not have): host and port are the proxy's, user the name
 * sent (NULL: the configured proxy user name), method the one tried
 * (password, chap, none, ident), proxy its type (http, socks5, socks4).
 * Written for the program's own connection, a jump host's
 * (kitty_wev_seat_proxy) and the connections its port forwardings open
 * (proxy_wev_decide), as for the other login records. */
void proxy_wev_auth_failed(ProxySocket *ps, const char *proxytype,
                           const char *method, const char *user,
                           const char *reason);
/* KiTTY: event 220/320 for a proxy that accepted our credentials; the
 * arguments as for proxy_wev_auth_failed. */
void proxy_wev_auth_ok(ProxySocket *ps, const char *proxytype,
                       const char *method, const char *user);
/* KiTTY: whether a proxied connection writes its proxy records. With a
 * client Interactor: its seat is the program's own or a jump host's. With
 * none: a connection a port forwarding or X11 forwarding opens - always the
 * program's own session's, since a jump host's session forwards nothing
 * (sshproxy.c clears its forwardings); nobody can be asked, so only what
 * needs no question is recorded. */
bool proxy_wev_decide(Seat *clientseat, bool have_interactor);
/* KiTTY: the `for` field of the proxy records: session for a connection
 * with an Interactor, else what the forwarding announced
 * (kitty_wev_next_conn_for: forward, x11). */
const char *proxy_wev_for(bool have_interactor);
/* KiTTY: a user name or password went to the proxy (event 225/325). */
void proxy_wev_note_credentials(ProxyNegotiator *pn, const char *user);
void proxy_spr_abort(ProxyNegotiator *pn, SeatPromptResult spr);

/*
 * This is a wrapper on format_connection_setup_command which gets the
 * format string from CONF_proxy_telnet_command. Despite the name, it
 * may also be reused by local-command proxies on individual
 * platforms.
 */
char *format_telnet_command(SockAddr *addr, int port, Conf *conf,
                            unsigned *flags_out);

DeferredSocketOpener *local_proxy_opener(
    SockAddr *addr, int port, Plug *plug, Conf *conf, Interactor *itr);
void local_proxy_opener_set_socket(DeferredSocketOpener *opener,
                                   Socket *socket);
char *platform_setup_local_proxy(Socket *socket, const char *cmd);

#include "cproxy.h"

#endif
