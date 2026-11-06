/*
 * nosshproxy.c: stub implementation of sshproxy_new_connection().
 */

#include "putty.h"
#include "network.h"

const bool ssh_proxy_supported = false;

/* KiTTY: no jump host, so no answer of one (kitty/kitty_knock.h) */
bool kitty_sshproxy_reply(Socket *s, int *kind, unsigned *code)
{
    (void)s;
    (void)kind;
    (void)code;
    return false;
}

Socket *sshproxy_new_connection(SockAddr *addr, const char *hostname,
                                int port, bool privport,
                                bool oobinline, bool nodelay, bool keepalive,
                                Plug *plug, Conf *conf, Interactor *itr)
{
    return NULL;
}
