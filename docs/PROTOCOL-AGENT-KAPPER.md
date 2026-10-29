# kageant's agent protocol extensions

This file describes the two extensions of the SSH agent protocol that
kageant (the KiTTY++ agent) adds to OpenSSH's protocol (`PROTOCOL.agent`,
draft-miller-ssh-agent). Both are sent with `SSH_AGENTC_EXTENSION` (27).
Their names follow the `name@domain` rule of RFC 4251 section 6:
`kapper.net` is the KiTTY++ domain. Each name carries a version, as
OpenSSH's `restrict-destination-v00@openssh.com` does. A changed format
gets a new name (`-v01`). kageant then answers both names while clients
move to the new one.

Both extensions build on OpenSSH's `session-bind@openssh.com` (section 1
of `PROTOCOL.agent`): a client binds its agent connection to the server it
completed key exchange with. kageant keeps the bindings per connection.
The destination rules of keys (`restrict-destination-v00@openssh.com`,
`ssh-add -h`) are checked against them.

## Discovery

kageant answers the standard `query` extension with every extension it
supports, these two included:

    byte        SSH_AGENTC_EXTENSION (27)
    string      "query"

    byte        SSH_AGENT_EXTENSION_RESPONSE (29)
    string      "query"
    string[]    extension names

A client needs no query before it sends either extension: an agent
without it answers a failure.

## withheld-keys-v00@kapper.net

The number of keys the agent leaves out of the key list of this
connection because of their destination rules.

    byte        SSH_AGENTC_EXTENSION (27)
    string      "withheld-keys-v00@kapper.net"

Answer on a connection whose bindings are all for a login (no binding
with `is_forwarding` set):

    byte        SSH_AGENT_SUCCESS (6)
    uint32      count

The count is the number of keys the agent's answer to
`SSH2_AGENTC_REQUEST_IDENTITIES` on this connection leaves out, counted
when the request comes. Keys without rules are never counted.

Answer without a binding, with a binding for forwarding, or with data
after the name:

    byte        SSH_AGENT_FAILURE (5)

**When KiTTY++ sends it.** klink, kscp, ksftp and kitty.exe send it once,
on the agent connection of a login, after a successful
`session-bind@openssh.com` and before `SSH2_AGENTC_REQUEST_IDENTITIES`.
A count above 0 is shown before the next authentication method, in the
terminal or on stderr and in the Terminal Window's Event Log:

    kageant: 1 key restricted to other servers was not offered.
    kageant: 2 keys restricted to other servers were not offered.

The line is not shown when a key of the agent logs in. A failure answer
is not shown: PuTTY's Pageant and OpenSSH's ssh-agent answer the unknown
name with a failure. Nothing else changes for them.

**Security.** The answer carries a count only, never a key, a comment or
a host. It is refused on a connection with a binding for forwarding: the
program that sends it sits on a remote server, which must not learn that the
user holds keys for other servers. It is refused without a binding: the
count only means something for the server bound. The refusal is the
plain `SSH_AGENT_FAILURE` an agent without the extension gives, so it
does not show whether keys exist. kageant writes no refusal record for
it: no key use was refused.

## session-bind-host-v00@kapper.net

The name the client connected to, for the binding sent just before it.

    byte        SSH_AGENTC_EXTENSION (27)
    string      "session-bind-host-v00@kapper.net"
    string      host
    uint32      port

    byte        SSH_AGENT_SUCCESS (6)
or
    byte        SSH_AGENT_EXTENSION_FAILURE (28)

kageant keeps the name with the last binding of the connection. It
answers `SSH_AGENT_EXTENSION_FAILURE` and writes a refused request to
its agent log (event 103, reason `session-bind-host: ...`) when:

- the connection has no binding,
- the last binding has a name already (one name per binding),
- the host is empty or longer than 255 bytes,
- the host holds a control character (a byte below 0x20, or 0x7f),
- the port is 0 or above 65535,
- the message is short or has data after the port.

**When KiTTY++ sends it.** klink, kscp, ksftp and kitty.exe send it right
after a successful `session-bind@openssh.com` on the agent connection of
a login: the host name and port the session connected to (the name its
host key is stored under). A session with a logical host name for its
host key sends that name. On a forwarded agent connection kitty.exe and
klink send it right after the binding for forwarding, with the name of
the server whose forwarded connection it is. A failure answer is not
shown. Its answer on a forwarded connection never reaches the server.

**Display.** kageant shows the name first, marked ` (client)`, then the
names its own lookup finds for the host key (the KiTTY++ host-key store,
`known_hosts`) that differ from it, at most 5 names in all, then `+N`
for the rest:

    dest="SHA256:WCvM... monitor.example.net (client) 192.0.2.21 +1"

The same text is in `dest=` and `fwd=` of the agent log, the Windows
Event Log records that carry those fields, and the confirmation's
**Destination:** and **Forwarded via:** lines. The binding's own record
(event 102) is written before the name arrives: it shows the lookup's
names only. Without the extension (OpenSSH's ssh.exe) only the lookup's
names are shown.

**Security.** The name is the client's word: kageant does not verify it
and decides nothing by it. Every check - the destination rules, the
signature over the session id - stays on the host key the binding
verified. The name is shown so the reader recognises the server. It is
marked `(client)` so it is not taken for a verified fact. Length and
control characters are checked because the name reaches the agent log,
the Windows Event Log and a dialog.
