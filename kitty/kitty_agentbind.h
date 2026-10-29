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
#define KITTY_AGENTBIND_MSG_EXT      27   /* SSH_AGENTC_EXTENSION */
#define KITTY_AGENTBIND_MSG_SUCCESS  6    /* SSH_AGENT_SUCCESS */
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

/* An agent's answer as agent_query() hands it over: the 4-byte length,
 * then the body. True only for a well-formed SSH_AGENT_SUCCESS. */
bool kitty_agentbind_reply_bound(const void *reply, size_t len);

/* The type byte of such an answer, -1 when there is none (no reply, a
 * short or inconsistent one): for the Event Log line of a refused bind. */
int kitty_agentbind_reply_type(const void *reply, size_t len);

/* Skipping one framed agent message in a byte stream: the agent's answer
 * to a bind written ahead of a forwarded agent stream, which must never
 * reach the server. Zero-initialise, then hand over the stream's bytes as
 * they come: the return value is how many of them belong to the skipped
 * message, the rest goes on. done is set once the message has passed in
 * full; bad when its length field is 0 or above KITTY_AGENTBIND_MAX_FRAME
 * (everything after it is then unusable). */
struct kitty_agentbind_skip {
    unsigned char hdr[4];
    size_t hdr_got;
    size_t body_left;
    bool done, bad;
};
size_t kitty_agentbind_skip(struct kitty_agentbind_skip *st,
                            const void *data, size_t len);

#endif
