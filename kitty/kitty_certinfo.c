/*
 * kitty_certinfo.c - what kageant shows of an OpenSSH certificate it holds
 * (kitty_certinfo.h).
 */
#include <stdio.h>
#include <string.h>
#include "putty.h"
#include "ssh.h"
#include "kitty_certinfo.h"
#include "kitty_text.h"     /* KT_KACERT_* */

/* "a, b, c" from an SSH string list; with values ("name=value") for the
 * name/value pairs of the critical options */
static char *kcert_list(ptrlen list, bool pairs)
{
    BinarySource src[1];
    strbuf *out = strbuf_new();
    BinarySource_BARE_INIT_PL(src, list);
    while (get_avail(src)) {
        ptrlen name = get_string(src);
        ptrlen value = make_ptrlen(NULL, 0);
        if (pairs) {
            /* the value is itself a string inside a string */
            BinarySource vs[1];
            ptrlen raw = get_string(src);
            BinarySource_BARE_INIT_PL(vs, raw);
            value = get_string(vs);
            if (get_err(vs))
                value = raw;
        }
        if (get_err(src))
            break;
        if (out->len)
            put_datapl(out, PTRLEN_LITERAL(", "));
        put_datapl(out, name);
        if (value.len) {
            put_byte(out, '=');
            put_datapl(out, value);
        }
    }
    return strbuf_to_str(out);
}

bool kcert_info_get(ptrlen blob, kcert_info *ci)
{
    BinarySource src[1], bsrc[1];
    const ssh_keyalg *alg;
    ssh_key *k;
    strbuf *base;
    ptrlen algname, ca;
    uint64_t after, before;
    ptrlen principals, critical, key_id;
    char *cafp, *sp;

    BinarySource_BARE_INIT_PL(src, blob);
    algname = get_string(src);
    alg = get_err(src) ? NULL : find_pubkey_alg_len(algname);
    if (!alg || !alg->is_certificate)
        return false;
    /* ssh_key_new_pub checks the certificate's whole layout */
    k = ssh_key_new_pub(alg, blob);
    if (!k)
        return false;
    /* After the algorithm name and the nonce come the base key's fields:
     * its public blob without its own algorithm name. */
    base = strbuf_new();
    ssh_key_public_blob(ssh_key_base_key(k), BinarySink_UPCAST(base));
    ssh_key_free(k);
    BinarySource_BARE_INIT_PL(bsrc, ptrlen_from_strbuf(base));
    get_string(bsrc);                       /* its algorithm name */
    get_string(src);                        /* nonce */
    get_data(src, get_avail(bsrc));         /* the base key's fields */
    strbuf_free(base);
    get_uint64(src);                        /* serial */
    get_uint32(src);                        /* type */
    key_id = get_string(src);
    principals = get_string(src);
    after = get_uint64(src);
    before = get_uint64(src);
    critical = get_string(src);
    get_string(src);                        /* extensions */
    get_string(src);                        /* reserved */
    ca = get_string(src);
    if (get_err(src))
        return false;

    cafp = ssh2_fingerprint_blob(ca, SSH_FPTYPE_SHA256);
    /* "<alg> <bits> SHA256:..." - the hash is the part to read */
    sp = strrchr(cafp, ' ');
    ci->ca_fp = dupstr(sp ? sp + 1 : cafp);
    sfree(cafp);
    ci->valid_after = after;
    ci->valid_before = before;
    ci->principals = kcert_list(principals, false);
    ci->critical = kcert_list(critical, true);
    ci->key_id = mkstr(key_id);
    return true;
}

strbuf *kcert_base_blob(ptrlen blob)
{
    BinarySource src[1];
    ptrlen algname;
    const ssh_keyalg *alg;
    BinarySource_BARE_INIT_PL(src, blob);
    algname = get_string(src);
    alg = get_err(src) ? NULL : find_pubkey_alg_len(algname);
    if (alg && alg->is_certificate) {
        ssh_key *k = ssh_key_new_pub(alg, blob);
        if (k) {
            strbuf *base = strbuf_new();
            ssh_key_public_blob(ssh_key_base_key(k), BinarySink_UPCAST(base));
            ssh_key_free(k);
            return base;
        }
    }
    return strbuf_dup(blob);
}

void kcert_info_free(kcert_info *ci)
{
    if (!ci)
        return;
    sfree(ci->ca_fp);
    sfree(ci->principals);
    sfree(ci->critical);
    sfree(ci->key_id);
    memset(ci, 0, sizeof(*ci));
}

/* local calendar day number of t: days since 1970-01-01 local */
static int64_t kcert_day(uint64_t t, int64_t tzoff)
{
    int64_t s;
    if (t > (uint64_t)INT64_MAX / 2)
        return INT64_MAX / 86400;
    s = (int64_t)t + tzoff;
    return s >= 0 ? s / 86400 : -((-s + 86399) / 86400);
}

int kcert_state(const kcert_info *ci, uint64_t now, int64_t tzoff, int *days)
{
    int64_t d;
    if (days)
        *days = 0;
    if (now < ci->valid_after)
        return KCERT_NOT_YET;
    if (now >= ci->valid_before)
        return KCERT_EXPIRED;
    if (ci->valid_before == UINT64_MAX)
        return KCERT_VALID;
    d = kcert_day(ci->valid_before, tzoff) - kcert_day(now, tzoff);
    if (d <= 0)
        return KCERT_EXPIRES_TODAY;
    if (d > KCERT_WARN_DAYS)
        return KCERT_VALID;
    if (days)
        *days = (int)d;
    return KCERT_EXPIRING;
}

int kcert_state_level(int state)
{
    switch (state) {
      case KCERT_EXPIRING:
      case KCERT_EXPIRES_TODAY:
        return 1;
      case KCERT_EXPIRED:
      case KCERT_NOT_YET:
        return 2;
      default:
        return 0;
    }
}

/* most urgent first: expired, not yet valid, expires today, expiring
 * (fewest days first), valid */
static int kcert_rank(int state)
{
    switch (state) {
      case KCERT_EXPIRED:       return 0;
      case KCERT_NOT_YET:       return 1;
      case KCERT_EXPIRES_TODAY: return 2;
      case KCERT_EXPIRING:      return 3;
      default:                  return 4;
    }
}

int kcert_state_cmp(int a, int adays, int b, int bdays)
{
    int ra = kcert_rank(a), rb = kcert_rank(b);
    if (ra != rb)
        return ra - rb;
    if (a == KCERT_EXPIRING)
        return adays - bdays;
    return 0;
}

char *kcert_state_text(const kcert_info *ci, int state, int days,
                       kcert_datefn date, void *ctx)
{
    char *d, *t;
    switch (state) {
      case KCERT_EXPIRING:
        return days == 1 ? dupstr(KT_KACERT_EXPIRES_1DAY) :
                           dupprintf(KT_KACERT_EXPIRES_DAYS_FMT, days);
      case KCERT_EXPIRES_TODAY:
        return dupstr(KT_KACERT_EXPIRES_TODAY);
      case KCERT_EXPIRED:
        d = date(ci->valid_before, ctx);
        t = dupprintf(KT_KACERT_EXPIRED_FMT, d);
        sfree(d);
        return t;
      case KCERT_NOT_YET:
        d = date(ci->valid_after, ctx);
        t = dupprintf(KT_KACERT_NOT_YET_FMT, d);
        sfree(d);
        return t;
      default:
        return dupstr(KT_KACERT_VALID);
    }
}

char *kcert_column_text(const kcert_info *certs, size_t n, uint64_t now,
                        int64_t tzoff, kcert_datefn date, void *ctx,
                        int *level)
{
    size_t i, top = 0;
    int topstate = KCERT_VALID, topdays = 0;
    char *t, *r;

    if (level)
        *level = 0;
    if (!n)
        return dupstr("");
    for (i = 0; i < n; i++) {
        int days, st = kcert_state(&certs[i], now, tzoff, &days);
        if (i == 0 || kcert_state_cmp(st, days, topstate, topdays) < 0) {
            top = i;
            topstate = st;
            topdays = days;
        }
    }
    if (level)
        *level = kcert_state_level(topstate);
    if (topstate == KCERT_VALID)
        return n == 1 ? dupstr(KT_KACERT_VALID) :
                        dupprintf(KT_KACERT_VALID_N_FMT, (unsigned)n);
    t = kcert_state_text(&certs[top], topstate, topdays, date, ctx);
    if (n == 1)
        return t;
    r = dupprintf(KT_KACERT_MORE_FMT, t, (unsigned)(n - 1));
    sfree(t);
    return r;
}
