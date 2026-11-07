/*
 * kitty_agentbind_ssh.c: session-bind@openssh.com in the SSH-2 client - the
 * message built from the first key exchange of a transport layer
 * (ssh2_transport_kitty_bind_data). Sent by ssh/userauth2-client.c on the
 * agent connection a login holds (is_forwarding 0) and by
 * ssh/connection2-client.c on each forwarded agent connection
 * (is_forwarding 1), as OpenSSH's client does. After the bind, kageant's
 * session-bind-host-v00@kapper.net and withheld-keys-v00@kapper.net
 * (kitty_agentbind.h).
 *
 * Member of the sshcommon library, beside the transport layer.
 */
#include "putty.h"
#include "ssh.h"
#include "../ssh/ppl.h"
#include "kitty_agentbind.h"

/* The one switch for binding. Always on: an agent without the extension
 * answers a failure and the connection stays unbound. A setting to turn
 * it off goes here. */
static bool kitty_agentbind_wanted(void)
{
    return true;
}

strbuf *kitty_agentbind_query(PacketProtocolLayer *transport,
                              bool is_forwarding)
{
    ptrlen hostkey, sid, sig;
    strbuf *q;
    size_t n;

    if (!kitty_agentbind_wanted() || !transport ||
        !ssh2_transport_kitty_bind_data(transport, &hostkey, &sid, &sig))
        return NULL;
    n = kitty_agentbind_encode(NULL, 0, hostkey.ptr, hostkey.len,
                               sid.ptr, sid.len, sig.ptr, sig.len,
                               is_forwarding);
    q = strbuf_new_for_agent_query();
    kitty_agentbind_encode(strbuf_append(q, n), n, hostkey.ptr, hostkey.len,
                           sid.ptr, sid.len, sig.ptr, sig.len,
                           is_forwarding);
    strbuf_finalise_agent_query(q);
    return q;
}

/* session-bind-host-v00@kapper.net: the name and port the transport
 * layer connected to (its host key name), NULL when there is none or it is
 * longer than the extension takes. */
strbuf *kitty_agentbind_host_query(PacketProtocolLayer *transport)
{
    const char *type, *keystr, *host = NULL;
    int port = 0;
    ptrlen sid;
    strbuf *q;
    size_t n, hl;

    if (!transport)
        return NULL;
    ssh2_transport_kitty_hk00(transport, &type, &keystr, &host, &port, &sid);
    /* only a host name or an IP address, the same check kageant makes:
     * a session whose host is no such name (a NetBIOS name with _) sends
     * no host name at all instead of a request kageant refuses */
    if (!host || !*host || (hl = strlen(host)) > KITTY_AGENTBIND_MAX_HOST ||
        kitty_agentbind_host_check(host, hl) != NULL ||
        port < 1 || port > 65535)
        return NULL;
    n = kitty_agentbind_host_encode(NULL, 0, host, hl, (uint32_t)port);
    q = strbuf_new_for_agent_query();
    kitty_agentbind_host_encode(strbuf_append(q, n), n, host, hl,
                                (uint32_t)port);
    strbuf_finalise_agent_query(q);
    return q;
}

/* withheld-keys-v00@kapper.net */
strbuf *kitty_agentbind_withheld_query(void)
{
    size_t n = kitty_agentbind_withheld_encode(NULL, 0);
    strbuf *q = strbuf_new_for_agent_query();
    kitty_agentbind_withheld_encode(strbuf_append(q, n), n);
    strbuf_finalise_agent_query(q);
    return q;
}

char *kitty_agentbind_hostkey_fp(PacketProtocolLayer *transport)
{
    ptrlen hostkey, sid, sig;

    if (!transport ||
        !ssh2_transport_kitty_bind_data(transport, &hostkey, &sid, &sig))
        return dupstr("");
    return ssh2_fingerprint_blob(hostkey, SSH_FPTYPE_SHA256);
}
