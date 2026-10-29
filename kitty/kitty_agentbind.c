/*
 * kitty_agentbind.c: the session-bind@openssh.com message and the agent's
 * answer to it, and kageant's two extensions sent after it (kitty_agentbind.h).
 * No PuTTY dependencies, so the unit test links this file alone.
 */
#include <stdio.h>
#include <string.h>

#include "kitty_agentbind.h"
#include "kitty_text.h"      /* KT_AGENT_MSG_WITHHELD* */

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

size_t kitty_agentbind_host_encode(unsigned char *out, size_t cap,
                                   const char *host, size_t host_len,
                                   uint32_t port)
{
    const char *name = KITTY_AGENTBIND_HOST_EXT;
    size_t need = 1 + (4 + strlen(name)) + (4 + host_len) + 4;
    size_t at;

    if (!out || cap < need)
        return need;
    out[0] = KITTY_AGENTBIND_MSG_EXT;
    at = put_str(out, 1, cap, name, strlen(name));
    at = put_str(out, at, cap, host, host_len);
    out[at++] = (unsigned char)(port >> 24);
    out[at++] = (unsigned char)(port >> 16);
    out[at++] = (unsigned char)(port >> 8);
    out[at++] = (unsigned char)port;
    return at;
}

size_t kitty_agentbind_withheld_encode(unsigned char *out, size_t cap)
{
    const char *name = KITTY_AGENTBIND_WITHHELD_EXT;
    size_t need = 1 + (4 + strlen(name));

    if (!out || cap < need)
        return need;
    out[0] = KITTY_AGENTBIND_MSG_EXT;
    return put_str(out, 1, cap, name, strlen(name));
}

bool kitty_agentbind_withheld_reply(const void *reply, size_t len,
                                    uint32_t *count)
{
    const unsigned char *p = (const unsigned char *)reply;

    if (kitty_agentbind_reply_type(reply, len) != KITTY_AGENTBIND_MSG_SUCCESS ||
        len != 4 + 1 + 4)
        return false;
    *count = ((uint32_t)p[5] << 24) | ((uint32_t)p[6] << 16) |
        ((uint32_t)p[7] << 8) | (uint32_t)p[8];
    return true;
}

int kitty_agentbind_withheld_text(char *out, size_t cap, unsigned long count)
{
    if (count == 1)
        return snprintf(out, cap, "%s", KT_AGENT_MSG_WITHHELD1);
    return snprintf(out, cap, KT_AGENT_MSG_WITHHELD_FMT, count);
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
    while (!st->done) {
        while (st->hdr_got < 4 && used < len)
            st->hdr[st->hdr_got++] = p[used++];
        if (st->hdr_got < 4)
            return used;
        if (st->hdr_got == 4) {
            uint32_t body = ((uint32_t)st->hdr[0] << 24) |
                ((uint32_t)st->hdr[1] << 16) | ((uint32_t)st->hdr[2] << 8) |
                (uint32_t)st->hdr[3];
            st->hdr_got = 5;           /* the length is read */
            if (body == 0 || body > KITTY_AGENTBIND_MAX_FRAME) {
                st->bad = true;
                return used;
            }
            st->body_left = body;
        }
        if (len - used < st->body_left) {
            st->body_left -= len - used;
            return len;
        }
        used += st->body_left;
        st->body_left = 0;
        if (st->more) {
            st->more--;                /* the next message's length next */
            st->hdr_got = 0;
        } else {
            st->done = true;
        }
    }
    return used;
}
