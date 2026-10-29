/*
 * kitty_agentbind.c: the session-bind@openssh.com message and the agent's
 * answer to it (kitty_agentbind.h). No PuTTY dependencies, so the unit test
 * links this file alone.
 */
#include <string.h>

#include "kitty_agentbind.h"

/* one SSH string: uint32 length, then the bytes */
static size_t put_str(unsigned char *out, size_t at, size_t cap,
                      const void *data, size_t len)
{
    if (out && at + 4 + len <= cap) {
        out[at] = (unsigned char)(len >> 24);
        out[at + 1] = (unsigned char)(len >> 16);
        out[at + 2] = (unsigned char)(len >> 8);
        out[at + 3] = (unsigned char)len;
        if (len)
            memcpy(out + at + 4, data, len);
    }
    return at + 4 + len;
}

size_t kitty_agentbind_encode(unsigned char *out, size_t cap,
                              const void *hostkey, size_t hostkey_len,
                              const void *sid, size_t sid_len,
                              const void *sig, size_t sig_len,
                              bool is_forwarding)
{
    const char *name = KITTY_AGENTBIND_EXT_NAME;
    size_t need = 1 + (4 + strlen(name)) + (4 + hostkey_len) +
        (4 + sid_len) + (4 + sig_len) + 1;
    size_t at;

    if (!out || cap < need)
        return need;
    out[0] = KITTY_AGENTBIND_MSG_EXT;
    at = put_str(out, 1, cap, name, strlen(name));
    at = put_str(out, at, cap, hostkey, hostkey_len);
    at = put_str(out, at, cap, sid, sid_len);
    at = put_str(out, at, cap, sig, sig_len);
    out[at++] = is_forwarding ? 1 : 0;
    return at;
}

int kitty_agentbind_reply_type(const void *reply, size_t len)
{
    const unsigned char *p = (const unsigned char *)reply;
    uint32_t body;

    if (!p || len < 5)
        return -1;
    body = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
        ((uint32_t)p[2] << 8) | (uint32_t)p[3];
    if (body != len - 4)
        return -1;
    return p[4];
}

bool kitty_agentbind_reply_bound(const void *reply, size_t len)
{
    return kitty_agentbind_reply_type(reply, len) ==
        KITTY_AGENTBIND_MSG_SUCCESS;
}

size_t kitty_agentbind_skip(struct kitty_agentbind_skip *st,
                            const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    size_t used = 0;

    if (st->done || st->bad)
        return 0;
    while (st->hdr_got < 4 && used < len)
        st->hdr[st->hdr_got++] = p[used++];
    if (st->hdr_got < 4)
        return used;
    if (st->hdr_got == 4) {
        uint32_t body = ((uint32_t)st->hdr[0] << 24) |
            ((uint32_t)st->hdr[1] << 16) | ((uint32_t)st->hdr[2] << 8) |
            (uint32_t)st->hdr[3];
        st->hdr_got = 5;               /* the length is read */
        if (body == 0 || body > KITTY_AGENTBIND_MAX_FRAME) {
            st->bad = true;
            return used;
        }
        st->body_left = body;
    }
    if (len - used >= st->body_left) {
        used += st->body_left;
        st->body_left = 0;
        st->done = true;
    } else {
        st->body_left -= len - used;
        used = len;
    }
    return used;
}
