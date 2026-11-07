# kageant's agent protocol extensions

This file describes the five extensions of the SSH agent protocol that
kageant (the KiTTY++ agent) adds to OpenSSH's protocol (`PROTOCOL.agent`,
draft-miller-ssh-agent). All five are sent with `SSH_AGENTC_EXTENSION` (27).
Their names follow the `name@domain` rule of RFC 4251 section 6:
`kapper.net` is the KiTTY++ domain. Each name carries a version, as
OpenSSH's `restrict-destination-v00@openssh.com` does. A changed format
gets a new name (`-v01`). kageant then answers both names while clients
move to the new one.

`withheld-keys-v00` and `session-bind-host-v00` build on OpenSSH's
`session-bind@openssh.com` (section 1 of `PROTOCOL.agent`): a client binds
its agent connection to the server it completed key exchange with. kageant
keeps the bindings per connection. The destination rules of keys
(`restrict-destination-v00@openssh.com`, `ssh-add -h`) are checked against
them. `key-state-v00` follows the same rules: a connection learns the
state of the keys its key list shows, of no other key.

Two more serve kageant's own command line (`kageant -l`, `kageant -R`):
`rules-list-v00` and `rules-remove-v00`. The last section describes how
kageant answers PuTTY's extensions.

## Discovery

kageant answers the standard `query` extension with every extension it
supports, these five included:

    byte        SSH_AGENTC_EXTENSION (27)
    string      "query"

    byte        SSH_AGENT_EXTENSION_RESPONSE (29)
    string      "query"
    string[]    extension names

A client needs no query before it sends one of these extensions: an
agent without it answers a failure. To a program the answer does not list
PuTTY's `list-extended`, `reencrypt` and `reencrypt-all` (see the last
section).

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

    byte        SSH_AGENT_EXTENSION_FAILURE (28)

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
count only means something for the server bound. The refusal is
`SSH_AGENT_EXTENSION_FAILURE`, as RFC 9987 section 5.8 recommends for a
known extension that fails. It does not show whether keys exist. kageant
writes no refusal record for it: no key use was refused.

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
and OpenSSH's `known_hosts` only while kageant's setting "Use OpenSSH's
known_hosts as a host-key source" is on - a name from it marked
` (known_hosts)`) that differ from it, at most 5 names in all, then `+N`
for the rest:

    dest="SHA256:WCvM... monitor.example.net (client) 192.0.2.21 +1"

The same text is in `dest=` and `fwd=` of the agent log, the Windows
Event Log records that carry those fields, and the confirmation's
**Destination:** and **Forwarded via:** lines. The binding's own record
(event 102) is written before the name arrives: it shows the lookup's
names only. Without the extension (OpenSSH's ssh.exe) only the lookup's
names are shown.

**Security.** The name and the port are the client's word. kageant
cannot verify them: `session-bind@openssh.com` carries neither. The name
decides nothing. The port is used for one decision only: a port
condition in the **Valid hosts** of an "All servers of a host CA"
destination is checked against it. A port condition therefore holds
against a mistake, not against a client that sends a false port. Without
the extension a port condition is never met, unless the destination is
set to "Ignore the port for clients that send none" (the port conditions
are then dropped from the expression). Every other check - the
destination rules, the signature over the session id - stays on the host
key the binding verified. The name is shown so the reader recognises the
server. It is
marked `(client)` so it is not taken for a verified fact. Length and
control characters are checked because the name reaches the agent log,
the Windows Event Log and a dialog.

## key-state-v00@kapper.net

The state of one key, or of every SSH-2 key the agent holds.

    byte        SSH_AGENTC_EXTENSION (27)
    string      "key-state-v00@kapper.net"
    string      key blob (optional)

Without the key blob the answer covers every SSH-2 key, with it that one
key:

    byte        SSH_AGENT_EXTENSION_RESPONSE (29)
    string      "key-state-v00@kapper.net"
    uint32      count
    count times:
        string  key blob
        uint32  flags

The flags (the bits of PuTTY's `list-extended`):

    1   the agent holds the encrypted key file: it can re-encrypt the key
    2   no cleartext key: the key stays encrypted until its passphrase is
        typed (also a key decrypted for another program that has not used
        it yet)
    4   every use of the key is confirmed
    8   that confirmation is a Windows Hello check (set with 4)

Other bits are 0. A client ignores a bit it does not know.

The answer covers the keys the key list of this connection shows
(`SSH2_AGENTC_REQUEST_IDENTITIES`). A key its destination rules leave out
has no entry, as a key the agent does not hold has none: the count is 0
for both.

Answer when the data after the name is not one string (cut short, or
bytes after it):

    byte        SSH_AGENT_EXTENSION_FAILURE (28)

kageant then writes a refused request to its agent log (event 103,
reason `key-state: ...`). While the agent is locked the answer is
`SSH_AGENT_FAILURE` (5), as for every request other than the key lists
(answered empty), the lock and the unlock.

**When KiTTY++ sends it.** kitty.exe sends it while it waits to reconnect
after a login failed on an agent key that is still encrypted: every 5
seconds, with the blob of that key, on an agent connection of its own.
Flag 2 set: it waits. Flag 2 clear: it reconnects once. No entry: it
stops and reports the condition. The server is not contacted while it
waits. An agent that answers `SSH_AGENT_FAILURE` (an older kageant,
another agent): kitty.exe then sends PuTTY's `list-extended` instead.

**Security.** The answer carries public keys and their flags, only for
the keys the connection may see. It shows whether a key is still
encrypted and whether its use is confirmed. A remote host behind a
forwarded agent learns that much about the keys it is offered.

## rules-list-v00@kapper.net

Every key of the rule file with its destination rules, then every SSH-2
key the agent holds without rules (`kageant -l`).

    byte        SSH_AGENTC_EXTENSION (27)
    string      "rules-list-v00@kapper.net"

    byte        SSH_AGENT_EXTENSION_RESPONSE (29)
    string      "rules-list-v00@kapper.net"
    uint32      count
    count times:
        string  fingerprint ("ssh-ed25519 255 SHA256:...")
        string  comment
        byte    1 = the agent holds the key, 0 = not loaded
        string  the rules as Key details lists them: one line per
                destination, its origin on an indented line below,
                CR LF between the lines; empty for a key without rules

Only kageant's own command line (`kageant -l`) may ask: the program file
of the requesting process, by the process id Windows reports for the
pipe, must be kageant's own (the same file, through a junction or an 8.3
name too). A request by window messages carries no such process id. A
forwarding client that sends no `session-bind@openssh.com` (PuTTY,
WinSCP, an older KiTTY) looks like a local program, so the binding check
alone would let its remote peer read the user's servers.

Answer to any other program, on a connection with a binding for
forwarding, with data after the name, or while the rule file is damaged:

    byte        SSH_AGENT_EXTENSION_FAILURE (28)

with a refused request in the agent log (event 103, reason
`rules-list: ...`).

## rules-remove-v00@kapper.net

Every destination rule of one key removed from the rule file, loaded or
not (`kageant -R`), once the person at the desktop confirms it.

    byte        SSH_AGENTC_EXTENSION (27)
    string      "rules-remove-v00@kapper.net"
    string      the key: its public key blob, or the text "SHA256:..."

    byte        SSH_AGENT_EXTENSION_RESPONSE (29)
    string      "rules-remove-v00@kapper.net"
    uint32      0 removed, 1 not confirmed (the rules stay), 2 the key has
                no rules, 3 no such key (neither in the rule file nor held),
                4 no interactive desktop for the confirmation (the rules
                stay)

kageant puts up a confirmation on the desktop (system-modal, No the
default), or a Windows Hello check while its setting "Confirmations
require Windows Hello" is on. No, a refused Hello check, another
confirmation already up or no interactive desktop (locked, a service
session) is answer 1. The agent log records 1 and 0 as event 116, result
`refused` or `removed`, reason `cli`. Refused like `rules-list-v00` on a
connection with a binding for forwarding, for data that does not decode
or while the rule file is damaged.

**Security.** The rules name the user's servers: a program behind a
forwarded agent gets neither extension. A program on this computer reads
the list as it reads the key list. A removal takes the person at the
desktop: a program alone cannot lift a key's rules.

## PuTTY's extensions

kageant answers PuTTY's `add-ppk@putty.projects.tartarus.org` from every
program. PuTTY's `list-extended@putty.projects.tartarus.org`,
`reencrypt@putty.projects.tartarus.org` and
`reencrypt-all@putty.projects.tartarus.org` serve kageant's own window
only (its key list, Re-encrypt and Re-encrypt All Keys). To a program on
the pipe or through `WM_COPYDATA` they are unknown names:

    byte        SSH_AGENT_FAILURE (5)

That is the answer RFC 9987 section 5.8 specifies for an extension the
agent does not support. The `query` answer to a program leaves the three
out. kageant writes a refused request to its agent log (event 103, reason
`unrecognised extension name '...'`).

Why: `list-extended` answers `SSH_AGENT_SUCCESS` followed by data. RFC 9987
section 5.8 allows `SSH_AGENT_SUCCESS` alone or
`SSH_AGENT_EXTENSION_RESPONSE` with the extension's name for a successful
extension request. `key-state-v00` carries the same flags in that format.
`reencrypt` and `reencrypt-all` are refused because a remote host behind a
forwarded agent must not re-encrypt the user's keys: every key would then
require its passphrase again, at that host's will.

On a connection bound for agent forwarding (a binding with
`is_forwarding` set) kageant refuses `add-ppk` with
`SSH_AGENT_EXTENSION_FAILURE` while its setting `blockforwardedchanges`
is on (the default), as it refuses the standard adds, removals, the lock
and the unlock there with `SSH_AGENT_FAILURE`. The agent log records the
refusal with reason `forwarded`.

PuTTY's Unix `pageant` client against kageant: `pageant -l` lists the keys
without " (encrypted)". `pageant -d` removes a key as before (the standard
key list and removal). `pageant -r` and `--reencrypt-all` fail.
