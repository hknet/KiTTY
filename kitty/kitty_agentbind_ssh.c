/*
 * kitty_agentbind_ssh.c: session-bind@openssh.com in the SSH-2 client - the
 * message built from the first key exchange of a transport layer
 * (ssh2_transport_kitty_bind_data). Sent by ssh/userauth2-client.c on the
 * agent connection a login holds (is_forwarding 0) and by
 * ssh/connection2-client.c on each forwarded agent connection
 * (is_forwarding 1), as OpenSSH's client does.
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

char *kitty_agentbind_hostkey_fp(PacketProtocolLayer *transport)
{
    ptrlen hostkey, sid, sig;

    if (!transport ||
        !ssh2_transport_kitty_bind_data(transport, &hostkey, &sid, &sig))
        return dupstr("");
    return ssh2_fingerprint_blob(hostkey, SSH_FPTYPE_SHA256);
}
