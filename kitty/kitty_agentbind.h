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

/*
 * May this be the host name of session-bind-host-v00@kapper.net? NULL =
 * yes, else the rule it breaks (static text). One definition for kageant
 * (which refuses anything else) and for kitty.exe and klink (which send
 * the extension only for a valid name):
 *   - a host name as RFC 1123 (2.1) defines it: labels of letters, digits
 *     and '-' separated by dots, each 1 to 63 characters, no '-' at either
 *     end of a label, at most 253 characters, one trailing dot allowed. An
 *     international name only as its A-label (xn--..., RFC 5890). An IPv4
 *     address is such a name too (digits and dots).
 *   - an IPv6 address (RFC 4291 text form: hex digits and ':', "::" at
 *     most once, an IPv4 tail allowed), in brackets or not, with an
 *     optional zone after '%' (RFC 6874: letters, digits, '.', '_', '-').
 * Anything else - '_', a space, a byte from 0x80 up - is no host name.
 */
static inline const char *kitty_agentbind_host_check(const char *h, size_t n)
{
    size_t i, lab = 0;
    if (!h || n == 0)
        return "the host name is empty";
    if (n > KITTY_AGENTBIND_MAX_HOST)
        return "the host name is longer than 255 bytes";
    for (i = 0; i < n; i++)
        if (h[i] == ':')
            break;
    if (i < n) {                          /* an IPv6 address */
        size_t a = 0, b = n, colons = 0, hex = 0;
        int dbl = 0;
        if (h[0] == '[') {
            if (n < 2 || h[n - 1] != ']')
                return "an IPv6 address with an unclosed bracket";
            a = 1; b = n - 1;
        }
        for (i = a; i < b && h[i] != '%'; i++) {
            char c = h[i];
            if (c == ':') {
                colons++;
                if (i + 1 < b && h[i + 1] == ':') {
                    if (dbl)
                        return "an IPv6 address with \"::\" twice";
                    dbl = 1;
                }
                hex = 0;
            } else if (c == '.') {
                hex = 0;                  /* the IPv4 tail: a group per dot */
            } else if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                       (c >= 'A' && c <= 'F')) {
                if (++hex > 4)
                    return "an IPv6 address with a group of more than 4 digits";
            } else
                return "a character outside an IPv6 address";
        }
        if (colons < 2 || colons > 7)
            return "an IPv6 address with a wrong number of groups";
        if (i < b) {                      /* %zone */
            if (++i == b)
                return "an IPv6 zone that is empty";
            for (; i < b; i++) {
                char c = h[i];
                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
                    return "a character outside an IPv6 zone";
            }
        }
        return NULL;
    }
    if (h[n - 1] == '.')
        n--;                              /* the trailing dot of a full name */
    if (n == 0)
        return "the host name is empty";
    if (n > 253)
        return "the host name is longer than 253 characters";
    for (i = 0; i <= n; i++) {
        char c = i < n ? h[i] : '.';
        if (c == '.') {
            if (lab == 0)
                return "an empty label in the host name";
            if (lab > 63)
                return "a label of the host name longer than 63 characters";
            if (h[i - 1] == '-' || h[i - lab] == '-')
                return "a label of the host name that starts or ends with '-'";
            lab = 0;
        } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '-') {
            lab++;
        } else
            return "a character outside a host name";
    }
    return NULL;
}
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
