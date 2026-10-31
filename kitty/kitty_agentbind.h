/*
 * kitty_agentbind.h: session-bind@openssh.com, the message by which an SSH
 * client binds its agent connection to the server it completed key exchange
 * with (OpenSSH's PROTOCOL.agent, section 1):
 *
 *   byte    SSH_AGENTC_EXTENSION (27)
 *   string  "session-bind@openssh.com"
 *   string  the server's host key
 *   string  the session identifier (the first exchange hash)
 *   string  the server's signature over the session identifier
 *   bool    is_forwarding
 *
 * The agent answers SSH_AGENT_SUCCESS (6). Any other answer leaves the
 * connection unbound. The login goes on.
 *
 * After a successful bind our clients send kageant's two extensions on
 * the same connection (docs/PROTOCOL-AGENT-KAPPER.md):
 *
 *   byte    SSH_AGENTC_EXTENSION (27)
 *   string  "session-bind-host-v00@kapper.net"
 *   string  host (the name the user connected to)
 *   uint32  port
 *     -> SSH_AGENT_SUCCESS, or a failure (ignored)
 *
 *   byte    SSH_AGENTC_EXTENSION (27)
 *   string  "withheld-keys-v00@kapper.net"
 *     -> SSH_AGENT_SUCCESS, uint32 count of keys the agent leaves out of
 *        this connection's key list because of their destination rules;
 *        or a failure (ignored: an agent without it, a forwarded agent)
 *
 * kitty.exe's reconnect wait (kitty_reconnect.c) reads the state of its key
 * with a third one, on a connection of its own:
 *
 *   byte    SSH_AGENTC_EXTENSION (27)
 *   string  "key-state-v00@kapper.net"
 *   string  the key's public blob (absent: every key)
 *     -> SSH_AGENT_EXTENSION_RESPONSE (29), string "key-state-v00@kapper.net",
 *        uint32 count, per key string blob and uint32 flags (pageant.h
 *        LIST_EXTENDED_FLAG_*); SSH_AGENT_FAILURE from an agent without it
 *
 * This file holds the encoding and the reading of the answer, free of the
 * rest of the program (test/test_agentbind.c). The SSH side - the data from
 * the transport layer, the one switch - is kitty_agentbind_ssh.c.
 */
#ifndef KITTY_AGENTBIND_H
#define KITTY_AGENTBIND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KITTY_AGENTBIND_EXT_NAME     "session-bind@openssh.com"
#define KITTY_AGENTBIND_HOST_EXT     "session-bind-host-v00@kapper.net"
#define KITTY_AGENTBIND_WITHHELD_EXT "withheld-keys-v00@kapper.net"
#define KITTY_AGENTBIND_KEYSTATE_EXT "key-state-v00@kapper.net"
/* the longest host name session-bind-host-v00@kapper.net carries */
#define KITTY_AGENTBIND_MAX_HOST     255
#define KITTY_AGENTBIND_MSG_EXT      27   /* SSH_AGENTC_EXTENSION */
#define KITTY_AGENTBIND_MSG_SUCCESS  6    /* SSH_AGENT_SUCCESS */
#define KITTY_AGENTBIND_MSG_FAILURE  5    /* SSH_AGENT_FAILURE */
#define KITTY_AGENTBIND_MSG_EXT_RESPONSE 29   /* SSH_AGENT_EXTENSION_RESPONSE */
/* the longest agent message (pageant.h AGENT_MAX_MSGLEN) */
#define KITTY_AGENTBIND_MAX_FRAME    262144

/* The message body (no length field in front) into out, which holds cap
 * bytes. Returns the body's length; with out NULL or cap too small nothing
 * is written, so a first call with NULL sizes the buffer. */
size_t kitty_agentbind_encode(unsigned char *out, size_t cap,
                              const void *hostkey, size_t hostkey_len,
                              const void *sid, size_t sid_len,
                              const void *sig, size_t sig_len,
                              bool is_forwarding);

/* session-bind-host-v00@kapper.net's body into out (cap bytes), sized as
 * kitty_agentbind_encode is: host (host_len bytes), port. */
size_t kitty_agentbind_host_encode(unsigned char *out, size_t cap,
                                   const char *host, size_t host_len,
                                   uint32_t port);

/* withheld-keys-v00@kapper.net's body into out, sized the same way. */
size_t kitty_agentbind_withheld_encode(unsigned char *out, size_t cap);

/* The answer to withheld-keys-v00@kapper.net (length field first, as
 * agent_query() hands it over): true with *count only for SSH_AGENT_SUCCESS
 * followed by exactly one uint32. */
bool kitty_agentbind_withheld_reply(const void *reply, size_t len,
                                    uint32_t *count);

/* key-state-v00@kapper.net's body for one key (blob_len bytes of its
 * public blob; blob NULL or blob_len 0: every key) into out, sized as
 * kitty_agentbind_encode is. */
size_t kitty_agentbind_keystate_encode(unsigned char *out, size_t cap,
                                       const void *blob, size_t blob_len);

/* The answer to it (length field first, as agent_query() hands it over),
 * for the key blob: */
enum {
    KITTY_KEYSTATE_FOUND = 1,     /* its entry: *flags = the flags word */
    KITTY_KEYSTATE_ABSENT = 0,    /* answered, the key has no entry (not
                                   * held, or not for this connection) */
    KITTY_KEYSTATE_BAD = -1,      /* SSH_AGENT_EXTENSION_FAILURE, another
                                   * type, a malformed answer or none */
    KITTY_KEYSTATE_UNKNOWN = -2   /* SSH_AGENT_FAILURE: an agent without it
                                   * (an older kageant, another agent) */
};
int kitty_agentbind_keystate_reply(const void *reply, size_t len,
                                   const void *blob, size_t blob_len,
                                   uint32_t *flags);

/* The line shown for count withheld keys, "kageant: 1 key restricted to
 * other servers was not offered." (plural for more), into out (cap bytes,
 * cut to fit). The length snprintf gives. */
int kitty_agentbind_withheld_text(char *out, size_t cap, unsigned long count);

/* An agent's answer as agent_query() hands it over: the 4-byte length,
 * then the body. True only for a well-formed SSH_AGENT_SUCCESS. */
bool kitty_agentbind_reply_bound(const void *reply, size_t len);

/* The type byte of such an answer, -1 when there is none (no reply, a
 * short or inconsistent one): for the Event Log line of a refused bind. */
int kitty_agentbind_reply_type(const void *reply, size_t len);

/* Skipping framed agent messages in a byte stream: the agent's answers
 * to a bind (and the session-bind-host-v00@kapper.net after it) written
 * ahead of a forwarded agent stream, which must never reach the server.
 * Zero-initialise, set more to the number of messages after the first,
 * then hand over the stream's bytes as they come: the return value is how
 * many of them belong to the skipped messages, the rest goes on. done is
 * set once the last message has passed in full; bad when a length field is
 * 0 or above KITTY_AGENTBIND_MAX_FRAME (everything after it is then
 * unusable). */
struct kitty_agentbind_skip {
    unsigned char hdr[4];
    size_t hdr_got;
    size_t body_left;
    unsigned more;
    bool done, bad;
};
size_t kitty_agentbind_skip(struct kitty_agentbind_skip *st,
                            const void *data, size_t len);

#endif
