/*
 * kitty_hostkeys00.c: OpenSSH's host key update - the switch, the store hook
 * and the decisions. See kitty_hostkeys00.h. No Windows or PuTTY headers:
 * test/test_hostkeys00.c links it alone.
 */
#include <string.h>
#include "kitty_hostkeys00.h"

static bool hk00_enabled = true;
static const struct kitty_hk00_store *hk00_store = NULL;

void kitty_hk00_set_enabled(int on)
{
    hk00_enabled = (on != 0);
}

bool kitty_hk00_enabled(void)
{
    return hk00_enabled;
}

void kitty_hk00_set_store(const struct kitty_hk00_store *store)
{
    hk00_store = store;
}

const struct kitty_hk00_store *kitty_hk00_get_store(void)
{
    return hk00_store;
}

int kitty_hk00_how(bool certificate, bool manual_list, int status_now)
{
    if (certificate)
        return KHK00_BY_CERT;
    if (manual_list)          /* the list alone decides, the store is not read */
        return KHK00_BY_MANUAL;
    return status_now == 0 ? KHK00_BY_STORE : KHK00_BY_ONCE;
}

bool kitty_hk00_eligible(bool enabled, bool readonly, int how)
{
    return enabled && !readonly && how == KHK00_BY_STORE;
}

bool kitty_hk00_plan(struct kitty_hk00_offer *o, size_t n, const char *conn_type)
{
    if (n == 0)
        return false;
    for (size_t i = 0; i < n; i++)
        for (size_t j = 0; j < i; j++)
            if (!strcmp(o[i].type, o[j].type))
                return false;    /* one key per type in the store: ambiguous */
    for (size_t i = 0; i < n; i++) {
        o[i].proof = 0;
        if (o[i].status == 0)
            o[i].action = KHK00_NOTHING;
        else if (conn_type && !strcmp(o[i].type, conn_type))
            o[i].action = KHK00_SKIP;   /* the verified type is never touched */
        else
            o[i].action = KHK00_PROVE;
    }
    return true;
}

bool kitty_hk00_decide(struct kitty_hk00_offer *o, size_t n)
{
    bool all = n > 0, failed = false;
    for (size_t i = 0; i < n; i++)
        if (o[i].action == KHK00_PROVE && o[i].proof <= 0)
            failed = true;
    for (size_t i = 0; i < n; i++) {
        if (o[i].action == KHK00_PROVE) {
            if (o[i].proof <= 0)
                o[i].action = KHK00_FAILED;
            else if (failed)     /* one failed proof: the store stays as it is */
                o[i].action = KHK00_NOTHING;
            else
                o[i].action = o[i].status == 2 ? KHK00_REPLACE : KHK00_STORE;
        }
        if (o[i].action != KHK00_NOTHING && o[i].action != KHK00_STORE &&
            o[i].action != KHK00_REPLACE)
            all = false;         /* a partial answer removes nothing */
    }
    return all && !failed;
}

bool kitty_hk00_removes(const struct kitty_hk00_offer *o, size_t n,
                        bool may_remove, const char *stored_type,
                        const char *conn_type)
{
    if (!may_remove || !stored_type)
        return false;
    if (conn_type && !strcmp(stored_type, conn_type))
        return false;
    for (size_t i = 0; i < n; i++)
        if (!strcmp(o[i].type, stored_type))
            return false;        /* still offered */
    return true;
}

static size_t put_string(unsigned char *out, size_t outlen, size_t at,
                         const unsigned char *p, size_t len)
{
    if (out && at + 4 + len <= outlen) {
        out[at] = (unsigned char)(len >> 24);
        out[at + 1] = (unsigned char)(len >> 16);
        out[at + 2] = (unsigned char)(len >> 8);
        out[at + 3] = (unsigned char)len;
        if (len)
            memcpy(out + at + 4, p, len);
    }
    return at + 4 + len;
}

size_t kitty_hk00_sigdata(unsigned char *out, size_t outlen,
                          const unsigned char *sid, size_t sidlen,
                          const unsigned char *blob, size_t bloblen)
{
    static const char name[] = "hostkeys-prove-00@openssh.com";
    size_t total = 4 + (sizeof(name) - 1) + 4 + sidlen + 4 + bloblen;
    size_t at = 0;
    if (!out || outlen < total)
        return total;
    at = put_string(out, outlen, at, (const unsigned char *)name, sizeof(name) - 1);
    at = put_string(out, outlen, at, sid, sidlen);
    at = put_string(out, outlen, at, blob, bloblen);
    return at;
}
