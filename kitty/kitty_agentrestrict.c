/*
 * kitty_agentrestrict.c - destination-restricted keys: the constraint of
 * ssh-add -h, the check of a request against its connection's
 * session-binds, and the rule store (see kitty_agentrestrict.h).
 *
 * The check follows OpenSSH's ssh-agent.c (identity_permitted,
 * permitted_by_dest_constraints, match_key_hop, parse_userauth_request),
 * written here from its rules: a restricted key is used only for the
 * destinations its rules permit, and only on a connection whose client
 * proved where it is connected to (session-bind@openssh.com).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "putty.h"
#include "ssh.h"

#include "kitty_agentrestrict.h"
#include "kitty_text.h"     /* KT_KA_DEST_CLIENT_MARK, KT_KAKEYS_DEST_* */

/* ---- rules, hops, keys ---------------------------------------------------- */

kar_rules *kar_rules_new(void)
{
    kar_rules *r = snew(kar_rules);
    memset(r, 0, sizeof(*r));
    return r;
}

void kar_hop_free(kar_hop *h)
{
    sfree(h->user);
    sfree(h->host);
    for (size_t i = 0; i < h->nkeys; i++)
        strbuf_free(h->keys[i].blob);
    sfree(h->keys);
    memset(h, 0, sizeof(*h));
}

static void kar_rule_free_contents(kar_rule *r)
{
    kar_hop_free(&r->from);
    kar_hop_free(&r->to);
}

void kar_rules_free(kar_rules *r)
{
    if (!r)
        return;
    for (size_t i = 0; i < r->n; i++)
        kar_rule_free_contents(&r->v[i]);
    sfree(r->v);
    sfree(r);
}

void kar_hop_add_key(kar_hop *h, ptrlen blob, bool is_ca)
{
    sgrowarray(h->keys, h->keysize, h->nkeys);
    h->keys[h->nkeys].blob = strbuf_dup(blob);
    h->keys[h->nkeys].is_ca = is_ca;
    h->nkeys++;
}

void kar_hop_copy(kar_hop *dst, const kar_hop *src)
{
    memset(dst, 0, sizeof(*dst));
    dst->user = src->user ? dupstr(src->user) : NULL;
    dst->host = src->host ? dupstr(src->host) : NULL;
    dst->match = src->match;
    for (size_t i = 0; i < src->nkeys; i++)
        kar_hop_add_key(dst, ptrlen_from_strbuf(src->keys[i].blob),
                        src->keys[i].is_ca);
}

void kar_rule_copy(kar_rule *dst, const kar_rule *src)
{
    kar_hop_copy(&dst->from, &src->from);
    kar_hop_copy(&dst->to, &src->to);
}

static bool str_equal(const char *a, const char *b)
{
    if (!a || !b)
        return a == b;
    return !strcmp(a, b);
}

/* Host names compare without regard to case; user names as written. */
static bool host_equal(const char *a, const char *b)
{
    if (!a || !b)
        return a == b;
    for (; *a && *b; a++, b++) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
        if (ca != cb)
            return false;
    }
    return *a == *b;
}

static bool hop_equal(const kar_hop *a, const kar_hop *b)
{
    if (!str_equal(a->user, b->user) || !host_equal(a->host, b->host) ||
        a->nkeys != b->nkeys || a->match != b->match)
        return false;
    for (size_t i = 0; i < a->nkeys; i++)
        if (a->keys[i].is_ca != b->keys[i].is_ca ||
            !ptrlen_eq_ptrlen(ptrlen_from_strbuf(a->keys[i].blob),
                              ptrlen_from_strbuf(b->keys[i].blob)))
            return false;
    return true;
}

bool kar_rule_equal(const kar_rule *a, const kar_rule *b)
{
    return hop_equal(&a->from, &b->from) && hop_equal(&a->to, &b->to);
}

bool kar_rules_add(kar_rules *rs, const kar_rule *r)
{
    if (rs->n >= KAR_MAX_RULES)
        return false;
    sgrowarray(rs->v, rs->size, rs->n);
    kar_rule_copy(&rs->v[rs->n++], r);
    return true;
}

bool kar_host_is_pattern(const char *host)
{
    return host && strpbrk(host, "*?") != NULL;
}

size_t (*kar_hostkey_names)(ptrlen hostkey, char ***names) = NULL;

void kar_names_free(char **names, size_t n)
{
    for (size_t i = 0; i < n; i++)
        sfree(names[i]);
    sfree(names);
}

bool kar_blob_valid(ptrlen blob)
{
    BinarySource src[1];
    const ssh_keyalg *alg = NULL;
    ssh_key *key;

    BinarySource_BARE_INIT_PL(src, blob);
    ptrlen type = get_string(src);
    if (!get_err(src))
        alg = find_pubkey_alg_len(type);
    if (!alg || (key = ssh_key_new_pub(alg, blob)) == NULL)
        return false;
    ssh_key_free(key);
    return true;
}

/* ---- the constraint on the wire ------------------------------------------ */

/* A name of a hop: no NUL inside (it becomes a C string), at most max
 * bytes. NULL for an empty one. */
static bool hop_name(ptrlen pl, size_t max, char **out)
{
    *out = NULL;
    if (pl.len > max || memchr(pl.ptr, '\0', pl.len))
        return false;
    if (pl.len)
        *out = mkstr(pl);
    return true;
}

static bool hop_parse(ptrlen pl, kar_hop *h, const char **err)
{
    BinarySource src[1];

    memset(h, 0, sizeof(*h));
    BinarySource_BARE_INIT_PL(src, pl);
    ptrlen user = get_string(src);
    ptrlen host = get_string(src);
    ptrlen reserved = get_string(src);
    if (get_err(src)) {
        *err = "a hop does not decode";
        return false;
    }
    if (reserved.len) {
        *err = "a hop carries an unsupported extension";
        return false;
    }
    if (!hop_name(user, KAR_MAX_USER, &h->user)) {
        *err = "a user name is longer than 256 bytes or holds a NUL";
        return false;
    }
    if (!hop_name(host, KAR_MAX_HOST, &h->host)) {
        *err = "a host name is too long or holds a NUL";
        return false;
    }
    while (get_avail(src)) {
        ptrlen blob;
        unsigned char is_ca;
        if (h->nkeys >= KAR_MAX_HOPKEYS) {
            *err = "a hop has too many host keys";
            return false;
        }
        blob = get_string(src);
        is_ca = get_byte(src);
        if (get_err(src)) {
            *err = "a host key does not decode";
            return false;
        }
        if (!kar_blob_valid(blob)) {
            *err = "a host key of an unknown type";
            return false;
        }
        kar_hop_add_key(h, blob, is_ca != 0);
    }
    return true;
}

kar_rules *kar_parse_constraint(BinarySource *msg, const char **err)
{
    BinarySource all[1];
    kar_rules *r;

    ptrlen body = get_string(msg);
    if (get_err(msg)) {
        *err = "the constraint does not decode";
        return NULL;
    }
    r = kar_rules_new();
    BinarySource_BARE_INIT_PL(all, body);
    while (get_avail(all)) {
        BinarySource one[1];
        kar_rule *d;

        if (r->n >= KAR_MAX_RULES) {
            *err = "more than 1024 destinations";
            goto refused;
        }
        ptrlen dc = get_string(all);
        if (get_err(all)) {
            *err = "a destination does not decode";
            goto refused;
        }
        BinarySource_BARE_INIT_PL(one, dc);
        ptrlen from = get_string(one);
        ptrlen to = get_string(one);
        ptrlen reserved = get_string(one);
        if (get_err(one) || get_avail(one)) {
            *err = "a destination does not decode";
            goto refused;
        }
        if (reserved.len) {
            *err = "a destination carries an unsupported extension";
            goto refused;
        }
        sgrowarray(r->v, r->size, r->n);
        d = &r->v[r->n++];
        memset(d, 0, sizeof(*d));
        if (!hop_parse(from, &d->from, err) || !hop_parse(to, &d->to, err))
            goto refused;
        if ((d->from.host == NULL) != (d->from.nkeys == 0) || d->from.user) {
            *err = "an inconsistent \"from\" hop";
            goto refused;
        }
        if (!d->to.host || !d->to.nkeys) {
            *err = "a \"to\" hop without a host name or a host key";
            goto refused;
        }
    }
    if (!r->n) {
        /* OpenSSH would add the key unrestricted; here a re-add with this
         * constraint replaces the key's rules, so an empty list would lift
         * them from a client. Refused instead. */
        *err = "no destination given";
        goto refused;
    }
    return r;

  refused:
    kar_rules_free(r);
    return NULL;
}

static void hop_encode(BinarySink *bs, const kar_hop *h)
{
    strbuf *sb = strbuf_new();
    put_stringz(sb, h->user ? h->user : "");
    put_stringz(sb, h->host ? h->host : "");
    put_stringz(sb, "");
    for (size_t i = 0; i < h->nkeys; i++) {
        put_stringpl(sb, ptrlen_from_strbuf(h->keys[i].blob));
        put_byte(sb, h->keys[i].is_ca ? 1 : 0);
    }
    put_stringsb(bs, sb);
}

void kar_encode_constraint(BinarySink *bs, const kar_rules *r)
{
    strbuf *all = strbuf_new();
    for (size_t i = 0; i < r->n; i++) {
        strbuf *one = strbuf_new();
        hop_encode(BinarySink_UPCAST(one), &r->v[i].from);
        hop_encode(BinarySink_UPCAST(one), &r->v[i].to);
        put_stringz(one, "");
        put_stringsb(all, one);
    }
    put_stringsb(bs, all);
}

/* ---- the check ------------------------------------------------------------ */

bool kar_wildcard(const char *s, const char *p)
{
    for (;;) {
        if (!*p)
            return !*s;
        if (*p == '*') {
            while (*p == '*')
                p++;
            if (!*p)
                return true;
            for (; *s; s++)
                if (kar_wildcard(s, p))
                    return true;
            return false;
        }
        if (!*s || (*p != '?' && *p != *s))
            return false;
        p++;
        s++;
    }
}

/* A bound host key, decoded once for the walk: a certificate is matched
 * against CA entries by its signing key and the hop's host name. */
typedef struct kar_boundkey {
    ptrlen blob;
    ssh_key *key;               /* NULL when it does not decode */
    strbuf *ca;                 /* a certificate's CA blob, else NULL */
    char **names;               /* KAR_MATCH_STORED_LATER: the names it is */
    size_t nnames;              /* stored under, read once when needed */
    bool names_read;
} kar_boundkey;

static void boundkey_init(kar_boundkey *k, ptrlen blob)
{
    BinarySource src[1];
    const ssh_keyalg *alg = NULL;

    memset(k, 0, sizeof(*k));
    k->blob = blob;
    BinarySource_BARE_INIT_PL(src, blob);
    ptrlen type = get_string(src);
    if (!get_err(src))
        alg = find_pubkey_alg_len(type);
    if (alg)
        k->key = ssh_key_new_pub(alg, blob);
    if (k->key && alg->is_certificate) {
        k->ca = strbuf_new();
        ssh_key_ca_public_blob(k->key, BinarySink_UPCAST(k->ca));
    }
}

static void boundkey_free(kar_boundkey *k)
{
    if (k->key)
        ssh_key_free(k->key);
    if (k->ca)
        strbuf_free(k->ca);
    kar_names_free(k->names, k->nnames);
}

/* A copy of s in lower case (host names). sfree it. */
static char *lower_dup(const char *s, size_t len)
{
    char *t = snewn(len + 1, char);
    for (size_t i = 0; i < len; i++)
        t[i] = (s[i] >= 'A' && s[i] <= 'Z') ? s[i] + ('a' - 'A') : s[i];
    t[len] = '\0';
    return t;
}

/* KAR_MATCH_CA_PATTERN: a host certificate signed by a CA key of the hop,
 * valid now, with a principal the hop's pattern matches (the pattern is
 * the rule's host name, the principal the subject). */
static bool hop_matches_ca_pattern(const kar_hop *h, const kar_boundkey *k)
{
    bool ca_ok = false, ok = false;
    key_components *kc;
    char *pat;

    if (!k->ca || !k->key || !h->host)
        return false;
    for (size_t i = 0; i < h->nkeys && !ca_ok; i++)
        ca_ok = h->keys[i].is_ca &&
            ptrlen_eq_ptrlen(ptrlen_from_strbuf(h->keys[i].blob),
                             ptrlen_from_strbuf(k->ca));
    if (!ca_ok)
        return false;
    pat = lower_dup(h->host, strlen(h->host));
    kc = ssh_key_components(k->key);
    for (size_t i = 0; kc && i < kc->ncomponents && !ok; i++) {
        const key_component *c = &kc->components[i];
        char *p;
        if (c->type != KCT_TEXT ||
            strncmp(c->name, "cert_valid_principal_", 21))
            continue;
        p = lower_dup(c->str->s, c->str->len);
        if (kar_wildcard(p, pat)) {
            /* the CA signature algorithms OpenSSH's agent accepts: not
             * RSA with SHA-1; the certificate's own check of type, time
             * and signature, for this principal */
            ca_options opts;
            strbuf *error = strbuf_new();
            opts.permit_rsa_sha1 = false;
            opts.permit_rsa_sha256 = true;
            opts.permit_rsa_sha512 = true;
            ok = ssh_key_check_cert(k->key, true,
                                    ptrlen_from_strbuf(c->str),
                                    (uint64_t)time(NULL), &opts,
                                    BinarySink_UPCAST(error));
            strbuf_free(error);
        }
        sfree(p);
    }
    if (kc)
        key_components_free(kc);
    sfree(pat);
    return ok;
}

/* KAR_MATCH_STORED_LATER: is the bound host key stored under a name the
 * hop's pattern matches? A name's ":port" is left out of the match. */
static bool hop_matches_names(const kar_hop *h, kar_boundkey *k)
{
    bool ok = false;
    char *pat;

    if (!h->host)
        return false;
    if (!k->names_read) {
        k->names_read = true;
        if (kar_hostkey_names)
            k->nnames = kar_hostkey_names(k->blob, &k->names);
    }
    pat = lower_dup(h->host, strlen(h->host));
    for (size_t i = 0; i < k->nnames && !ok; i++) {
        const char *n = k->names[i], *colon = strchr(n, ':');
        size_t len = strlen(n);
        char *t;
        /* "host:port" from the stores; an IPv6 address has more colons */
        if (colon && !strchr(colon + 1, ':'))
            len = (size_t)(colon - n);
        t = lower_dup(n, len);
        ok = kar_wildcard(t, pat);
        sfree(t);
    }
    sfree(pat);
    return ok;
}

/* OpenSSH's match_key_hop: a plain key equals a plain entry; a
 * certificate is signed by a CA entry and valid as a host certificate for
 * the hop's host name now. */
static bool hop_matches_keys(const kar_hop *h, const kar_boundkey *k)
{
    for (size_t i = 0; i < h->nkeys; i++) {
        const kar_hostkey *hk = &h->keys[i];
        if (!k->ca) {
            if (!hk->is_ca &&
                ptrlen_eq_ptrlen(ptrlen_from_strbuf(hk->blob), k->blob))
                return true;
            continue;
        }
        if (!hk->is_ca || !k->key ||
            !ptrlen_eq_ptrlen(ptrlen_from_strbuf(hk->blob),
                              ptrlen_from_strbuf(k->ca)))
            continue;
        {
            /* the CA signature algorithms OpenSSH's agent accepts: not
             * RSA with SHA-1 */
            ca_options opts;
            strbuf *error = strbuf_new();
            bool ok;
            opts.permit_rsa_sha1 = false;
            opts.permit_rsa_sha256 = true;
            opts.permit_rsa_sha512 = true;
            ok = ssh_key_check_cert(
                k->key, true, ptrlen_from_asciz(h->host ? h->host : ""),
                (uint64_t)time(NULL), &opts, BinarySink_UPCAST(error));
            strbuf_free(error);
            if (ok)
                return true;
        }
    }
    return false;
}

/* A hop against a bound host key, by the hop's KAR_MATCH_* value. */
static bool hop_matches(const kar_hop *h, kar_boundkey *k)
{
    if (h->match == KAR_MATCH_CA_PATTERN)
        return hop_matches_ca_pattern(h, k);
    if (hop_matches_keys(h, k))
        return true;
    return h->match == KAR_MATCH_STORED_LATER && hop_matches_names(h, k);
}

/* OpenSSH's permitted_by_dest_constraints: a rule whose "from" hop matches
 * the previous bind (none = this computer), whose "to" hop matches this one
 * (to NULL = any), and whose user pattern matches (when both are given). */
static bool by_rules(const kar_rules *r, kar_boundkey *from,
                     kar_boundkey *to, const char *user)
{
    for (size_t i = 0; i < r->n; i++) {
        const kar_rule *d = &r->v[i];
        if (!from) {
            if (d->from.host || d->from.nkeys)
                continue;
        } else if (!hop_matches(&d->from, from)) {
            continue;
        }
        if (to && !hop_matches(&d->to, to))
            continue;
        if (d->to.user && user && !kar_wildcard(user, d->to.user))
            continue;
        return true;
    }
    return false;
}

bool kar_permitted(const kar_rules *r, const kar_bind *b, size_t nb,
                   bool bind_attempted, const char *user, const char **why)
{
    kar_boundkey prev, cur;
    bool have_prev = false, ok = true;

    memset(&prev, 0, sizeof(prev));
    memset(&cur, 0, sizeof(cur));
    if (nb == 0) {
        if (bind_attempted) {
            *why = KAR_WHY_BIND_FAILED;
            return false;
        }
        return true;                    /* local use: permitted */
    }
    for (size_t i = 0; i < nb && ok; i++) {
        const char *test_user = NULL;
        if (i == nb - 1) {
            test_user = user;           /* the user on the last hop only */
            if (b[i].forwarding && user) {
                *why = KAR_WHY_FWD_HOP;
                ok = false;
                break;
            }
        } else if (!b[i].forwarding) {
            /* the core keeps no bind after one for a login */
            *why = KAR_WHY_DESTINATION;
            ok = false;
            break;
        }
        boundkey_init(&cur, b[i].hostkey);
        if (!by_rules(r, have_prev ? &prev : NULL, &cur, test_user)) {
            *why = KAR_WHY_DESTINATION;
            ok = false;
        }
        if (have_prev)
            boundkey_free(&prev);
        prev = cur;
        have_prev = true;
    }
    /* A connection whose last bind is for forwarding sees only keys that
     * may go further from there. */
    if (ok && b[nb - 1].forwarding && !user &&
        !by_rules(r, &prev, NULL, NULL)) {
        *why = KAR_WHY_DESTINATION;
        ok = false;
    }
    if (have_prev)
        boundkey_free(&prev);
    return ok;
}

bool kar_parse_userauth(ptrlen data, ptrlen keyblob, ptrlen *sid,
                        ptrlen *user, ptrlen *hostkey)
{
    BinarySource src[1];

    *hostkey = make_ptrlen(NULL, 0);
    BinarySource_BARE_INIT_PL(src, data);
    *sid = get_string(src);
    unsigned char type = get_byte(src);
    *user = get_string(src);
    ptrlen service = get_string(src);
    ptrlen method = get_string(src);
    unsigned char sig_follows = get_byte(src);
    ptrlen alg = get_string(src);
    ptrlen key = get_string(src);
    if (get_err(src))
        return false;
    if (!sid->len || type != SSH2_MSG_USERAUTH_REQUEST || sig_follows != 1 ||
        !ptrlen_eq_string(service, "ssh-connection") ||
        !ptrlen_eq_ptrlen(key, keyblob) || !find_pubkey_alg_len(alg) ||
        memchr(user->ptr, '\0', user->len))
        return false;
    if (ptrlen_eq_string(method, "publickey-hostbound-v00@openssh.com")) {
        *hostkey = get_string(src);
        if (get_err(src) || !kar_blob_valid(*hostkey))
            return false;
    } else if (!ptrlen_eq_string(method, "publickey")) {
        return false;
    }
    return get_avail(src) == 0;
}

bool kar_sign_permitted(const kar_rules *r, const kar_bind *b, size_t nb,
                        bool bind_attempted, ptrlen keyblob, ptrlen data,
                        const char **why)
{
    ptrlen sid, user, hostkey;
    char *u;
    bool ok;

    if (nb == 0) {
        *why = bind_attempted ? KAR_WHY_BIND_FAILED : KAR_WHY_UNBOUND;
        return false;
    }
    if (!kar_parse_userauth(data, keyblob, &sid, &user, &hostkey)) {
        *why = KAR_WHY_NOT_USERAUTH;
        return false;
    }
    u = mkstr(user);
    ok = kar_permitted(r, b, nb, bind_attempted, u, why);
    sfree(u);
    if (!ok)
        return false;
    if (!ptrlen_eq_ptrlen(sid, b[nb - 1].sid)) {
        *why = KAR_WHY_SESSION_ID;
        return false;
    }
    if (nb > 1 && !hostkey.len) {
        *why = KAR_WHY_HOSTKEY;
        return false;
    }
    if (hostkey.len && !ptrlen_eq_ptrlen(hostkey, b[nb - 1].hostkey)) {
        *why = KAR_WHY_HOSTKEY;
        return false;
    }
    return true;
}

int kar_list_group(const kar_rules *r, const kar_bind *b, size_t nb)
{
    kar_rules *ex;
    const char *why;
    bool exact;

    if (!r || !nb || b[nb - 1].forwarding ||
        !kar_permitted(r, b, nb, true, NULL, &why))
        return KAR_GROUP_NONE;
    ex = kar_rules_new();
    for (size_t i = 0; i < r->n; i++)
        if (r->v[i].to.match == KAR_MATCH_EXACT &&
            r->v[i].from.match == KAR_MATCH_EXACT)
            kar_rules_add(ex, &r->v[i]);
    exact = ex->n && kar_permitted(ex, b, nb, true, NULL, &why);
    kar_rules_free(ex);
    return exact ? KAR_GROUP_EXACT : KAR_GROUP_PATTERN;
}

void kar_list_order(const int *group, size_t n, size_t *order)
{
    size_t at = 0;
    for (int g = KAR_GROUP_EXACT; g <= KAR_GROUP_UNRESTRICTED; g++)
        for (size_t i = 0; i < n; i++)
            if (group[i] == g)
                order[at++] = i;
    /* a group outside 1..3 keeps its place at the end */
    for (size_t i = 0; i < n; i++)
        if (group[i] < KAR_GROUP_EXACT || group[i] > KAR_GROUP_UNRESTRICTED)
            order[at++] = i;
}

/* ---- texts ---------------------------------------------------------------- */

char *kar_hostkey_text(const kar_hostkey *k)
{
    char *full = ssh2_fingerprint_blob(ptrlen_from_strbuf(k->blob),
                                       SSH_FPTYPE_SHA256);
    const char *sp = strrchr(full, ' ');
    char *out = dupprintf("%s%s", k->is_ca ? "CA " : "", sp ? sp + 1 : full);
    sfree(full);
    return out;
}

char *kar_rule_text(const kar_rule *r)
{
    strbuf *sb = strbuf_new();
    if (r->from.host)
        put_fmt(sb, "%s > ", r->from.host);
    if (r->to.user)
        put_fmt(sb, "%s@", r->to.user);
    put_dataz(sb, r->to.host ? r->to.host : "");
    if (r->to.nkeys) {
        char *k = kar_hostkey_text(&r->to.keys[0]);
        put_fmt(sb, " (%s", k);
        sfree(k);
        if (r->to.nkeys > 1)
            put_fmt(sb, " +%d", (int)(r->to.nkeys - 1));
        put_byte(sb, ')');
    }
    return strbuf_to_str(sb);
}

char *kar_rules_text(const kar_rules *r)
{
    strbuf *sb = strbuf_new();
    for (size_t i = 0; r && i < r->n; i++) {
        char *t = kar_rule_text(&r->v[i]);
        if (i)
            put_datapl(sb, PTRLEN_LITERAL(", "));
        put_dataz(sb, t);
        sfree(t);
    }
    return strbuf_to_str(sb);
}

bool kar_parse_bind_host(BinarySource *src, char **out, const char **why)
{
    ptrlen host = get_string(src);
    unsigned long port = get_uint32(src);

    *out = NULL;
    if (get_err(src)) {
        *why = "unable to decode request";
        return false;
    }
    if (get_avail(src)) {
        *why = "trailing data after the request";
        return false;
    }
    if (!host.len || host.len > KAR_MAX_BIND_HOST) {
        *why = "the host name is empty or longer than 255 bytes";
        return false;
    }
    for (size_t i = 0; i < host.len; i++) {
        unsigned char c = ((const unsigned char *)host.ptr)[i];
        if (c < 0x20 || c == 0x7f) {
            *why = "a control character in the host name";
            return false;
        }
    }
    if (port < 1 || port > 65535) {
        *why = "the port is not 1 to 65535";
        return false;
    }
    *out = (port == 22) ? mkstr(host) :
        dupprintf("%.*s:%lu", PTRLEN_PRINTF(host), port);
    return true;
}

/* ASCII case ignored: host names */
static bool kar_same_name(const char *a, const char *b)
{
    for (;; a++, b++) {
        int ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z')
            ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z')
            cb += 'a' - 'A';
        if (ca != cb)
            return false;
        if (!ca)
            return true;
    }
}

char *kar_dest_names_text(const char *client, char *const *names, size_t n)
{
    strbuf *sb;
    size_t shown = 0, rest = 0;

    if ((!client || !*client) && !n)
        return NULL;
    sb = strbuf_new();
    if (client && *client) {
        put_dataz(sb, client);
        put_dataz(sb, KT_KA_DEST_CLIENT_MARK);
        shown++;
    }
    for (size_t i = 0; i < n; i++) {
        if (client && *client && kar_same_name(names[i], client))
            continue;
        if (shown >= KAR_DEST_MAX_NAMES) {
            rest++;
            continue;
        }
        if (sb->len)
            put_byte(sb, ' ');
        put_dataz(sb, names[i]);
        shown++;
    }
    if (rest)
        put_fmt(sb, " +%d", (int)rest);
    return strbuf_to_str(sb);
}

/* ---- the store ------------------------------------------------------------ */

static kar_store *kar_the_store = NULL;

kar_store *kar_store_get(void)
{
    if (!kar_the_store)
        kar_the_store = kar_store_new();
    return kar_the_store;
}

kar_store *kar_store_new(void)
{
    kar_store *s = snew(kar_store);
    memset(s, 0, sizeof(*s));
    s->next_id = 1;
    return s;
}

void kar_store_clear(kar_store *s)
{
    for (size_t i = 0; i < s->ndest; i++)
        kar_rule_free_contents(&s->dest[i].rule);
    for (size_t i = 0; i < s->nkeys; i++) {
        sfree(s->keys[i].fp);
        sfree(s->keys[i].comment);
        strbuf_free(s->keys[i].blob);
    }
    for (size_t i = 0; i < s->nlinks; i++)
        sfree(s->links[i].fp);
    sfree(s->dest);
    sfree(s->keys);
    sfree(s->links);
    memset(s, 0, sizeof(*s));
    s->next_id = 1;
}

void kar_store_free(kar_store *s)
{
    if (!s)
        return;
    kar_store_clear(s);
    sfree(s);
}

void kar_store_copy(kar_store *dst, const kar_store *src)
{
    kar_store_clear(dst);
    for (size_t i = 0; i < src->ndest; i++) {
        sgrowarray(dst->dest, dst->destsize, dst->ndest);
        dst->dest[dst->ndest].id = src->dest[i].id;
        kar_rule_copy(&dst->dest[dst->ndest].rule, &src->dest[i].rule);
        dst->ndest++;
    }
    for (size_t i = 0; i < src->nkeys; i++)
        kar_store_note_key(dst, src->keys[i].fp, src->keys[i].comment,
                           ptrlen_from_strbuf(src->keys[i].blob));
    for (size_t i = 0; i < src->nlinks; i++)
        kar_store_link(dst, src->links[i].fp, src->links[i].dest,
                       src->links[i].flags);
    dst->next_id = src->next_id;
    dst->damaged = src->damaged;
}

char *kar_key_fp(ptrlen pubblob)
{
    return ssh2_fingerprint_blob(pubblob, SSH_FPTYPE_SHA256);
}

bool kar_store_restricted(const kar_store *s, const char *fp)
{
    for (size_t i = 0; i < s->nlinks; i++)
        if (!strcmp(s->links[i].fp, fp))
            return true;
    return false;
}

kar_dest *kar_store_dest_by_id(const kar_store *s, unsigned id)
{
    for (size_t i = 0; i < s->ndest; i++)
        if (s->dest[i].id == id)
            return &s->dest[i];
    return NULL;
}

kar_skey *kar_store_key(const kar_store *s, const char *fp)
{
    for (size_t i = 0; i < s->nkeys; i++)
        if (!strcmp(s->keys[i].fp, fp))
            return &s->keys[i];
    return NULL;
}

kar_rules *kar_store_rules(const kar_store *s, const char *fp)
{
    kar_rules *r = NULL;

    for (size_t i = 0; i < s->nlinks; i++) {
        const kar_link *l = &s->links[i];
        const kar_dest *d;
        if (strcmp(l->fp, fp) || (d = kar_store_dest_by_id(s, l->dest)) == NULL)
            continue;
        if (!r)
            r = kar_rules_new();
        kar_rules_add(r, &d->rule);
        if (!(l->flags & KAR_LINK_FORWARD))
            continue;
        /* Forwarding allowed: from this destination to each of the key's
         * other destinations reached from this computer. */
        for (size_t j = 0; j < s->nlinks; j++) {
            const kar_link *l2 = &s->links[j];
            const kar_dest *d2;
            kar_rule fw;
            if (j == i || strcmp(l2->fp, fp) ||
                (d2 = kar_store_dest_by_id(s, l2->dest)) == NULL ||
                d2->rule.from.host)
                continue;
            memset(&fw, 0, sizeof(fw));
            kar_hop_copy(&fw.from, &d->rule.to);
            sfree(fw.from.user);        /* a "from" hop has no user */
            fw.from.user = NULL;
            kar_hop_copy(&fw.to, &d2->rule.to);
            kar_rules_add(r, &fw);
            kar_rule_free_contents(&fw);
        }
    }
    return r;
}

char *kar_store_dest_lines(const kar_store *s, const char *fp,
                           size_t *count)
{
    strbuf *sb = NULL;

    *count = 0;
    for (size_t i = 0; i < s->nlinks; i++) {
        const kar_link *l = &s->links[i];
        const kar_dest *d;
        if (strcmp(l->fp, fp) || (d = kar_store_dest_by_id(s, l->dest)) == NULL)
            continue;
        if (!sb)
            sb = strbuf_new();
        else
            put_datapl(sb, PTRLEN_LITERAL("\r\n"));
        put_dataz(sb, d->rule.to.host ? d->rule.to.host : "");
        if (d->rule.to.user)
            put_fmt(sb, KT_KAKEYS_DEST_USER_FMT, d->rule.to.user);
        if (d->rule.from.host)
            put_fmt(sb, KT_KAKEYS_DEST_VIA_FMT, d->rule.from.host);
        if (l->flags & KAR_LINK_FORWARD)
            put_dataz(sb, KT_KAKEYS_DEST_FORWARDING);
        if (d->rule.to.match == KAR_MATCH_CA_PATTERN)
            put_dataz(sb, KT_KAKEYS_DEST_MATCH_CA);
        else if (d->rule.to.match == KAR_MATCH_STORED_NOW)
            put_dataz(sb, KT_KAKEYS_DEST_MATCH_NOW);
        else if (d->rule.to.match == KAR_MATCH_STORED_LATER)
            put_dataz(sb, KT_KAKEYS_DEST_MATCH_LATER);
        (*count)++;
    }
    return sb ? strbuf_to_str(sb) : NULL;
}

kar_skey *kar_store_note_key(kar_store *s, const char *fp,
                             const char *comment, ptrlen blob)
{
    kar_skey *k = kar_store_key(s, fp);
    if (!k) {
        sgrowarray(s->keys, s->keysize, s->nkeys);
        k = &s->keys[s->nkeys++];
        k->fp = dupstr(fp);
        k->comment = dupstr(comment ? comment : "");
        k->blob = strbuf_new();
    } else if (comment && *comment) {
        sfree(k->comment);
        k->comment = dupstr(comment);
    }
    if (blob.len) {
        strbuf_clear(k->blob);
        put_datapl(k->blob, blob);
    }
    return k;
}

unsigned kar_store_dest_for(kar_store *s, const kar_rule *r)
{
    kar_dest *d;
    for (size_t i = 0; i < s->ndest; i++)
        if (kar_rule_equal(&s->dest[i].rule, r))
            return s->dest[i].id;
    if (s->ndest >= KAR_MAX_RULES * 16)
        return 0;
    sgrowarray(s->dest, s->destsize, s->ndest);
    d = &s->dest[s->ndest++];
    while (!s->next_id || kar_store_dest_by_id(s, s->next_id))
        s->next_id++;
    d->id = s->next_id++;
    kar_rule_copy(&d->rule, r);
    return d->id;
}

void kar_store_link(kar_store *s, const char *fp, unsigned dest,
                    unsigned flags)
{
    for (size_t i = 0; i < s->nlinks; i++)
        if (s->links[i].dest == dest && !strcmp(s->links[i].fp, fp)) {
            s->links[i].flags = flags;
            return;
        }
    sgrowarray(s->links, s->linksize, s->nlinks);
    s->links[s->nlinks].fp = dupstr(fp);
    s->links[s->nlinks].dest = dest;
    s->links[s->nlinks].flags = flags;
    s->nlinks++;
}

void kar_store_unlink_key(kar_store *s, const char *fp)
{
    size_t j = 0;
    for (size_t i = 0; i < s->nlinks; i++) {
        if (!strcmp(s->links[i].fp, fp)) {
            sfree(s->links[i].fp);
            continue;
        }
        s->links[j++] = s->links[i];
    }
    s->nlinks = j;
}

void kar_store_set_key_rules(kar_store *s, const char *fp,
                             const char *comment, ptrlen blob,
                             const kar_rules *r)
{
    kar_store_note_key(s, fp, comment, blob);
    kar_store_unlink_key(s, fp);
    for (size_t i = 0; r && i < r->n; i++) {
        unsigned id = kar_store_dest_for(s, &r->v[i]);
        if (id)
            kar_store_link(s, fp, id, 0);
    }
}

void kar_store_tidy(kar_store *s)
{
    size_t j = 0;
    for (size_t i = 0; i < s->ndest; i++) {
        bool used = false;
        for (size_t k = 0; k < s->nlinks && !used; k++)
            used = s->links[k].dest == s->dest[i].id;
        if (!used) {
            kar_rule_free_contents(&s->dest[i].rule);
            continue;
        }
        s->dest[j++] = s->dest[i];
    }
    s->ndest = j;
    j = 0;
    for (size_t i = 0; i < s->nkeys; i++) {
        if (!kar_store_restricted(s, s->keys[i].fp)) {
            sfree(s->keys[i].fp);
            sfree(s->keys[i].comment);
            strbuf_free(s->keys[i].blob);
            continue;
        }
        s->keys[j++] = s->keys[i];
    }
    s->nkeys = j;
}

/* ---- the store's bytes ---------------------------------------------------- */

/* Format 1: the three tables. Format 2 (KiTTY++ 0.85.1.20 on): the same,
 * each destination followed by its "to" hop's KAR_MATCH_* value as a
 * uint32. Format 1 still loads, every destination KAR_MATCH_EXACT. */
#define KAR_STORE_MAGIC_1 "kageant-rules-1"
#define KAR_STORE_MAGIC_2 "kageant-rules-2"

static void store_hop_put(strbuf *sb, const kar_hop *h)
{
    put_stringz(sb, h->user ? h->user : "");
    put_stringz(sb, h->host ? h->host : "");
    put_uint32(sb, (unsigned long)h->nkeys);
    for (size_t i = 0; i < h->nkeys; i++) {
        put_stringpl(sb, ptrlen_from_strbuf(h->keys[i].blob));
        put_bool(sb, h->keys[i].is_ca);
    }
}

void kar_store_encode(const kar_store *s, strbuf *out)
{
    kar_store_encode_format(s, out, 2);
}

void kar_store_encode_format(const kar_store *s, strbuf *out, int format)
{
    strbuf *sb = strbuf_new();
    unsigned char hash[32];
    ssh_hash *h;

    put_stringz(sb, format == 1 ? KAR_STORE_MAGIC_1 : KAR_STORE_MAGIC_2);
    put_uint32(sb, s->next_id);
    put_uint32(sb, (unsigned long)s->ndest);
    for (size_t i = 0; i < s->ndest; i++) {
        put_uint32(sb, s->dest[i].id);
        store_hop_put(sb, &s->dest[i].rule.from);
        store_hop_put(sb, &s->dest[i].rule.to);
        if (format != 1)
            put_uint32(sb, s->dest[i].rule.to.match);
    }
    put_uint32(sb, (unsigned long)s->nkeys);
    for (size_t i = 0; i < s->nkeys; i++) {
        put_stringz(sb, s->keys[i].fp);
        put_stringz(sb, s->keys[i].comment ? s->keys[i].comment : "");
        put_stringpl(sb, ptrlen_from_strbuf(s->keys[i].blob));
    }
    put_uint32(sb, (unsigned long)s->nlinks);
    for (size_t i = 0; i < s->nlinks; i++) {
        put_stringz(sb, s->links[i].fp);
        put_uint32(sb, s->links[i].dest);
        put_uint32(sb, s->links[i].flags);
    }
    h = ssh_hash_new(&ssh_sha256);
    put_datapl(h, ptrlen_from_strbuf(sb));
    ssh_hash_final(h, hash);
    put_datapl(out, ptrlen_from_strbuf(sb));
    put_string(out, hash, sizeof(hash));
    strbuf_free(sb);
}

/* A string of the file as a C string: no NUL inside, at most max bytes;
 * empty_null = an empty one becomes NULL. */
static bool store_str(BinarySource *src, size_t max, bool empty_null,
                      char **out)
{
    ptrlen pl = get_string(src);
    *out = NULL;
    if (get_err(src) || pl.len > max || memchr(pl.ptr, '\0', pl.len))
        return false;
    if (pl.len || !empty_null)
        *out = mkstr(pl);
    return true;
}

static bool store_hop_get(BinarySource *src, kar_hop *h)
{
    unsigned long n;
    memset(h, 0, sizeof(*h));
    if (!store_str(src, KAR_MAX_USER, true, &h->user) ||
        !store_str(src, KAR_MAX_HOST, true, &h->host))
        return false;
    n = get_uint32(src);
    if (get_err(src) || n > KAR_MAX_HOPKEYS)
        return false;
    for (unsigned long i = 0; i < n; i++) {
        ptrlen blob = get_string(src);
        bool is_ca = get_bool(src);
        if (get_err(src) || !kar_blob_valid(blob))
            return false;
        kar_hop_add_key(h, blob, is_ca);
    }
    return true;
}

bool kar_store_decode(kar_store *s, ptrlen in, const char **err)
{
    BinarySource src[1];
    kar_store *t = kar_store_new();
    unsigned char hash[32];
    unsigned long n;
    ssh_hash *h;
    ptrlen body;
    int format;

    *err = NULL;
    /* the hash at the end first: a file cut short or changed is damaged
     * before anything in it is believed */
    if (in.len < 36) {
        *err = "too short";
        goto damaged;
    }
    body = make_ptrlen(in.ptr, in.len - 36);
    {
        BinarySource tail[1];
        BinarySource_BARE_INIT_PL(tail, make_ptrlen(
                                      (const char *)in.ptr + body.len, 36));
        ptrlen stored = get_string(tail);
        if (get_err(tail) || stored.len != 32) {
            *err = "no checksum";
            goto damaged;
        }
        h = ssh_hash_new(&ssh_sha256);
        put_datapl(h, body);
        ssh_hash_final(h, hash);
        if (!smemeq(hash, stored.ptr, 32)) {
            *err = "checksum does not match";
            goto damaged;
        }
    }

    BinarySource_BARE_INIT_PL(src, body);
    {
        ptrlen magic = get_string(src);
        if (ptrlen_eq_string(magic, KAR_STORE_MAGIC_2)) {
            format = 2;
        } else if (ptrlen_eq_string(magic, KAR_STORE_MAGIC_1)) {
            format = 1;
        } else {
            *err = "not a rule file of this version";
            goto damaged;
        }
    }
    t->next_id = get_uint32(src);
    n = get_uint32(src);
    if (get_err(src) || n > KAR_MAX_RULES * 16) {
        *err = "destination table";
        goto damaged;
    }
    for (unsigned long i = 0; i < n; i++) {
        kar_dest *d;
        unsigned id = get_uint32(src);
        if (get_err(src) || !id || kar_store_dest_by_id(t, id)) {
            *err = "destination table";
            goto damaged;
        }
        sgrowarray(t->dest, t->destsize, t->ndest);
        d = &t->dest[t->ndest++];
        memset(d, 0, sizeof(*d));
        d->id = id;
        if (!store_hop_get(src, &d->rule.from) ||
            !store_hop_get(src, &d->rule.to)) {
            *err = "destination table";
            goto damaged;
        }
        if (format == 2) {
            unsigned long m = get_uint32(src);
            if (get_err(src) || m > KAR_MATCH_MAX) {
                *err = "destination table";
                goto damaged;
            }
            d->rule.to.match = (unsigned)m;
        }
        /* a destination needs a host key, except one matched by the names
         * stored later (KAR_MATCH_STORED_LATER) */
        if ((d->rule.from.host == NULL) != (d->rule.from.nkeys == 0) ||
            d->rule.from.user || !d->rule.to.host ||
            (!d->rule.to.nkeys &&
             d->rule.to.match != KAR_MATCH_STORED_LATER)) {
            *err = "destination table";
            goto damaged;
        }
    }
    n = get_uint32(src);
    if (get_err(src) || n > KAR_MAX_RULES * 16) {
        *err = "key table";
        goto damaged;
    }
    for (unsigned long i = 0; i < n; i++) {
        char *fp, *comment;
        ptrlen blob;
        if (!store_str(src, 1024, false, &fp)) {
            *err = "key table";
            goto damaged;
        }
        if (!store_str(src, 65536, false, &comment)) {
            sfree(fp);
            *err = "key table";
            goto damaged;
        }
        blob = get_string(src);
        if (get_err(src) || !*fp || kar_store_key(t, fp)) {
            sfree(fp);
            sfree(comment);
            *err = "key table";
            goto damaged;
        }
        kar_store_note_key(t, fp, comment, blob);
        sfree(fp);
        sfree(comment);
    }
    n = get_uint32(src);
    if (get_err(src) || n > KAR_MAX_RULES * 64) {
        *err = "link table";
        goto damaged;
    }
    for (unsigned long i = 0; i < n; i++) {
        char *fp;
        unsigned dest, flags;
        if (!store_str(src, 1024, false, &fp)) {
            *err = "link table";
            goto damaged;
        }
        dest = get_uint32(src);
        flags = get_uint32(src);
        if (get_err(src) || !kar_store_dest_by_id(t, dest) ||
            !kar_store_key(t, fp) || (flags & ~KAR_LINK_FORWARD)) {
            sfree(fp);
            *err = "link table";
            goto damaged;
        }
        kar_store_link(t, fp, dest, flags);
        sfree(fp);
    }
    if (get_avail(src)) {
        *err = "trailing data";
        goto damaged;
    }

    kar_store_copy(s, t);
    s->damaged = false;
    kar_store_free(t);
    return true;

  damaged:
    kar_store_free(t);
    return false;
}
