/*
 * kitty_hostkeys00.c: OpenSSH's host key update - the switches, the store
 * and UI hooks and the decisions. See kitty_hostkeys00.h. No Windows or
 * PuTTY headers: test/test_hostkeys00.c links it alone.
 */
#include <string.h>
#include "kitty_hostkeys00.h"

static int hk00_mode = KHK00_MODE_YES;
static int hk00_pertype = KHK00_PERTYPE_DEFAULT;
static const struct kitty_hk00_store *hk00_store = NULL;
static const struct kitty_hk00_ui *hk00_ui = NULL;

void kitty_hk00_set_mode(int mode)
{
    hk00_mode = (mode == KHK00_MODE_NO || mode == KHK00_MODE_ASK) ?
        mode : KHK00_MODE_YES;
}

int kitty_hk00_mode(void)
{
    return hk00_mode;
}

bool kitty_hk00_enabled(void)
{
    return hk00_mode != KHK00_MODE_NO;
}

/* a == b, ASCII letters in any case (no stricmp: this file stays portable) */
static bool word_is(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb)
            return false;
    }
    return *a == *b;
}

int kitty_hk00_mode_from_word(const char *v)
{
    if (v && word_is(v, "no"))
        return KHK00_MODE_NO;
    if (v && word_is(v, "ask"))
        return KHK00_MODE_ASK;
    return KHK00_MODE_YES;
}

void kitty_hk00_set_pertype(int n)
{
    hk00_pertype = n < 1 ? 1 : n > KHK00_PERTYPE_MAX ? KHK00_PERTYPE_MAX : n;
}

int kitty_hk00_pertype(void)
{
    return hk00_pertype;
}

void kitty_hk00_set_store(const struct kitty_hk00_store *store)
{
    hk00_store = store;
}

const struct kitty_hk00_store *kitty_hk00_get_store(void)
{
    return hk00_store;
}

void kitty_hk00_set_ui(const struct kitty_hk00_ui *ui)
{
    hk00_ui = ui;
}

const struct kitty_hk00_ui *kitty_hk00_get_ui(void)
{
    return hk00_ui;
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

static bool same_type(const char *a, const char *b)
{
    return a && b && !strcmp(a, b);
}

static bool same_key(const char *ta, const char *ka, const char *tb,
                     const char *kb)
{
    return same_type(ta, tb) && ka && kb && !strcmp(ka, kb);
}

static int clamp_pertype(int pertype)
{
    return pertype < 1 ? 1 : pertype > KHK00_PERTYPE_MAX ?
        KHK00_PERTYPE_MAX : pertype;
}

/* how many offers are of this type */
static int offers_of(const struct kitty_hk00_offer *o, size_t n,
                     const char *type)
{
    int k = 0;
    for (size_t i = 0; i < n; i++)
        if (same_type(o[i].type, type))
            k++;
    return k;
}

/* The key that verified the connection is of this type and the server does
 * not list it: it stays stored all the same, so it takes a place of its
 * type. */
static int conn_unlisted(const struct kitty_hk00_offer *o, size_t n,
                         const char *type, const char *conn_type,
                         const char *conn_key)
{
    if (!same_type(type, conn_type) || !conn_key)
        return 0;
    for (size_t i = 0; i < n; i++)
        if (same_key(o[i].type, o[i].key, conn_type, conn_key))
            return 0;
    return 1;
}

int kitty_hk00_plan(struct kitty_hk00_offer *o, size_t n, const char *conn_type,
                    const char *conn_key, int pertype, size_t *over_at,
                    int *over_n)
{
    pertype = clamp_pertype(pertype);
    if (n == 0)
        return KHK00_PLAN_UNUSED;
    for (size_t i = 0; i < n; i++)
        for (size_t j = 0; j < i; j++)
            if (same_key(o[i].type, o[i].key, o[j].type, o[j].key))
                return KHK00_PLAN_UNUSED;   /* a key listed twice (OpenSSH) */
    for (size_t i = 0; i < n; i++) {
        o[i].proof = 0;
        o[i].over = 0;
    }
    for (size_t i = 0; i < n; i++) {
        int k = offers_of(o, n, o[i].type);
        if (k > pertype) {
            /* the list is not used. The new keys of that type are proven
             * all the same, so the limit's line lists only proven keys. */
            bool any = false;
            if (over_at) *over_at = i;
            if (over_n) *over_n = k;
            for (size_t j = 0; j < n; j++) {
                o[j].action = KHK00_NOTHING;
                if (same_type(o[j].type, o[i].type) && o[j].status != 0) {
                    o[j].action = KHK00_PROVE;
                    o[j].over = 1;
                    any = true;
                }
            }
            if (!any) {                    /* only stored keys: nothing to prove */
                o[i].action = KHK00_SKIP;
                o[i].over = 1;
            }
            return KHK00_PLAN_OVERLIMIT;
        }
    }
    for (size_t i = 0; i < n; i++) {
        if (o[i].status == 0)
            o[i].action = KHK00_NOTHING;
        else if (pertype == 1)
            /* one key per type: a proven key replaces the stored one, which
             * must not be the key that verified this connection */
            o[i].action = same_type(o[i].type, conn_type) ?
                KHK00_SKIP : KHK00_PROVE;
        else if (offers_of(o, n, o[i].type) +
                 conn_unlisted(o, n, o[i].type, conn_type, conn_key) > pertype) {
            /* the verifying key stays: with it the type has no room. Proven
             * for the limit's line, never stored. */
            o[i].action = KHK00_PROVE;
            o[i].over = 1;
        } else
            o[i].action = KHK00_PROVE;
    }
    return KHK00_PLAN_OK;
}

bool kitty_hk00_decide(struct kitty_hk00_offer *o, size_t n, const char *conn_type,
                       const char *conn_key, int pertype)
{
    bool all = n > 0, failed = false, may_remove;
    pertype = clamp_pertype(pertype);
    /* a key the limit has no room for was proven only for the limit's
     * line: SKIP when proven, FAILED when not, and its failed proof does
     * not hold back the other keys */
    for (size_t i = 0; i < n; i++)
        if (o[i].action == KHK00_PROVE && o[i].over)
            o[i].action = o[i].proof > 0 ? KHK00_SKIP : KHK00_FAILED;
    for (size_t i = 0; i < n; i++)
        if (o[i].action == KHK00_PROVE && o[i].proof <= 0)
            failed = true;
    for (size_t i = 0; i < n; i++) {
        if (o[i].action == KHK00_PROVE) {
            if (o[i].proof <= 0)
                o[i].action = KHK00_FAILED;
            else if (failed)     /* one failed proof: the store stays as it is */
                o[i].action = KHK00_NOTHING;
        }
        if (o[i].action != KHK00_NOTHING && o[i].action != KHK00_PROVE)
            all = false;         /* a partial answer removes nothing */
    }
    may_remove = all && !failed;

    /* The proven keys, in list order: the number each would get. With the
     * list complete the stored keys of its type that stay are the listed
     * ones (and the verifying key, which is never removed). Otherwise every
     * stored key of the type stays. */
    for (size_t i = 0; i < n; i++) {
        int kept = 0, before = 0, nth;
        if (o[i].action != KHK00_PROVE)
            continue;
        if (pertype == 1) {
            o[i].action = o[i].status == 2 ? KHK00_REPLACE : KHK00_STORE;
            continue;
        }
        if (may_remove) {
            for (size_t j = 0; j < n; j++)
                if (same_type(o[j].type, o[i].type) && o[j].status == 0)
                    kept++;
            kept += conn_unlisted(o, n, o[i].type, conn_type, conn_key);
        } else
            kept = o[i].nstored;
        for (size_t j = 0; j < i; j++)
            if (same_type(o[j].type, o[i].type) &&
                (o[j].action == KHK00_STORE || o[j].action == KHK00_ADD))
                before++;
        nth = kept + before + 1;
        o[i].action = nth > pertype ? KHK00_NOTHING :
                      nth == 1 ? KHK00_STORE : KHK00_ADD;
    }
    return may_remove;
}

bool kitty_hk00_removes(const struct kitty_hk00_offer *o, size_t n,
                        bool may_remove, const char *stored_type,
                        const char *stored_key, const char *conn_type,
                        const char *conn_key)
{
    if (!may_remove || !stored_type || !stored_key)
        return false;
    if (same_key(stored_type, stored_key, conn_type, conn_key))
        return false;            /* the key that verified this connection */
    for (size_t i = 0; i < n; i++) {
        if (!same_type(o[i].type, stored_type))
            continue;
        if (o[i].key && !strcmp(o[i].key, stored_key))
            return false;        /* still offered */
        if (o[i].action == KHK00_REPLACE)
            return false;        /* the replacement takes its place */
    }
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
