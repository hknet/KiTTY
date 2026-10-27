/*
 * kitty_hostkeys00_ssh.c: OpenSSH's host key update in the SSH-2 connection
 * layer - the server's "hostkeys-00@openssh.com" list, the
 * "hostkeys-prove-00@openssh.com" request and its answer, and what is then
 * stored, replaced and removed. The decisions are kitty_hostkeys00.c's.
 *
 * Member of the sshclient library (it needs the connection layer's state).
 */
#include <assert.h>

#include "putty.h"
#include "ssh.h"
#include "../ssh/bpp.h"
#include "../ssh/ppl.h"
#include "../ssh/channel.h"
#include "../ssh/connection2.h"
#include "storage.h"
#include "kitty_hostkeys00.h"
#include "kitty_hostkey_acceptnew.h"   /* the record's second destination */
#include "kitty_text.h"
#include "kitty_winevent.h"              /* 213/313 */

/* More keys than any server has; a longer list is not used at all (a key
 * left out of it would read as no longer offered). */
#define HK00_MAX 32

typedef struct hk00_req {
    char *host;
    int port;
    const char *conn_type;
    strbuf *sid;
    size_t n;
    struct kitty_hk00_offer offers[HK00_MAX];
    ssh_key *keys[HK00_MAX];
    strbuf *blobs[HK00_MAX];
    char *keystrs[HK00_MAX];
} hk00_req;

static void hk00_req_free(void *vreq)
{
    hk00_req *req = (hk00_req *)vreq;
    if (!req)
        return;
    for (size_t i = 0; i < req->n; i++) {
        ssh_key_free(req->keys[i]);
        strbuf_free(req->blobs[i]);
        sfree(req->keystrs[i]);
    }
    strbuf_free(req->sid);
    sfree(req->host);
    sfree(req);
}

/* One Event Log line, and on klink, kscp and ksftp standard error too */
static void hk00_record(struct ssh2_connection_state *s, char *line)
{
    PacketProtocolLayer *ppl = &s->ppl; /* for ppl_logevent */
    ppl_logevent("%s", line);
    kitty_hostkey_accept_new_report(line);
    sfree(line);
}

/* The same outcome as a Windows Event Log record (213/313), under the
 * program's switch; a jump host's carries proxy "ssh" */
static void hk00_wev(struct ssh2_connection_state *s, hk00_req *req,
                     const char *keytype, const char *fp,
                     const char *result, const char *oldfp)
{
    const char *px = kitty_wev_seat_proxy(s->ppl.seat);
    if (px)
        kitty_wev_hostkey_update(req->host, req->port, keytype, fp, result,
                                 oldfp, px);
}

/* A signature over the proof data, by the offered key. RSA keys sign with
 * rsa-sha2-512 or rsa-sha2-256 (an SHA-1 proof is not taken). */
static bool hk00_verify(hk00_req *req, size_t i, ptrlen sig)
{
    ssh_key *key = req->keys[i], *vkey = NULL;
    ptrlen blob = ptrlen_from_strbuf(req->blobs[i]);
    bool ok;

    if (ssh_key_alg(key) == &ssh_rsa) {
        BinarySource src[1];
        BinarySource_BARE_INIT_PL(src, sig);
        ptrlen sigtype = get_string(src);
        const ssh_keyalg *valg =
            ptrlen_eq_string(sigtype, "rsa-sha2-512") ? &ssh_rsa_sha512 :
            ptrlen_eq_string(sigtype, "rsa-sha2-256") ? &ssh_rsa_sha256 : NULL;
        if (get_err(src) || !valg)
            return false;
        vkey = ssh_key_new_pub(valg, blob);
        if (!vkey)
            return false;
        key = vkey;
    }

    ptrlen sid = ptrlen_from_strbuf(req->sid);
    size_t len = kitty_hk00_sigdata(NULL, 0, sid.ptr, sid.len, blob.ptr, blob.len);
    unsigned char *data = snewn(len, unsigned char);
    kitty_hk00_sigdata(data, len, sid.ptr, sid.len, blob.ptr, blob.len);
    ok = ssh_key_verify(key, sig, make_ptrlen(data, len));
    sfree(data);
    if (vkey)
        ssh_key_free(vkey);
    return ok;
}

/* all_keyalgs[ai] names a store type an earlier entry named already (rsa:
 * three algorithms, one stored key) */
static bool hk00_seen_type(size_t ai)
{
    const ssh_keyalg *alg = all_keyalgs[ai];
    for (size_t aj = 0; aj < ai; aj++)
        if (!all_keyalgs[aj]->is_certificate &&
            !strcmp(all_keyalgs[aj]->cache_id, alg->cache_id))
            return true;
    return false;
}

static void hk00_apply(struct ssh2_connection_state *s, hk00_req *req)
{
    const struct kitty_hk00_store *st = kitty_hk00_get_store();
    bool may_remove = kitty_hk00_decide(req->offers, req->n);

    for (size_t i = 0; i < req->n; i++) {
        struct kitty_hk00_offer *o = &req->offers[i];
        char *fp, *old;
        switch (o->action) {
          case KHK00_STORE:
            store_host_key(s->ppl.seat, req->host, req->port, o->type,
                           req->keystrs[i]);
            fp = ssh2_fingerprint(req->keys[i], SSH_FPTYPE_DEFAULT);
            hk00_record(s, dupprintf(KT_HOSTKEY_UPDATE_STORED, fp,
                                     req->host, req->port));
            hk00_wev(s, req, ssh_key_alg(req->keys[i])->ssh_id, fp,
                     "stored", NULL);
            sfree(fp);
            break;
          case KHK00_REPLACE:
            old = st->stored_fingerprint(req->host, req->port, o->type);
            store_host_key(s->ppl.seat, req->host, req->port, o->type,
                           req->keystrs[i]);
            fp = ssh2_fingerprint(req->keys[i], SSH_FPTYPE_DEFAULT);
            hk00_record(s, dupprintf(KT_HOSTKEY_UPDATE_REPLACED, fp,
                                     old ? old : ssh_key_alg(req->keys[i])->ssh_id,
                                     req->host, req->port));
            hk00_wev(s, req, ssh_key_alg(req->keys[i])->ssh_id, fp,
                     "replaced", old);
            sfree(fp);
            sfree(old);
            break;
          case KHK00_FAILED:
            fp = ssh2_fingerprint(req->keys[i], SSH_FPTYPE_DEFAULT);
            hk00_record(s, dupprintf(KT_HOSTKEY_UPDATE_FAILED, fp,
                                     req->host, req->port));
            hk00_wev(s, req, ssh_key_alg(req->keys[i])->ssh_id, fp,
                     "proof-failed", NULL);
            sfree(fp);
            break;
          default:
            break;
        }
    }

    if (!may_remove)
        return;
    for (size_t ai = 0; ai < n_keyalgs; ai++) {
        const ssh_keyalg *alg = all_keyalgs[ai];
        if (alg->is_certificate || hk00_seen_type(ai))
            continue;
        if (!kitty_hk00_removes(req->offers, req->n, true, alg->cache_id,
                                req->conn_type))
            continue;
        if (!have_ssh_host_key(req->host, req->port, alg->cache_id))
            continue;
        char *old = st->stored_fingerprint(req->host, req->port, alg->cache_id);
        if (st->remove(req->host, req->port, alg->cache_id)) {
            hk00_record(s, dupprintf(KT_HOSTKEY_UPDATE_REMOVED,
                                     old ? old : alg->ssh_id,
                                     req->host, req->port));
            hk00_wev(s, req, alg->ssh_id, old, "removed", NULL);
        }
        sfree(old);
    }
}

static void hk00_reply(struct ssh2_connection_state *s, PktIn *pktin,
                       void *ctx)
{
    hk00_req *req = (hk00_req *)ctx;

    if (s->kitty_hk00_pending == req)
        s->kitty_hk00_pending = NULL;
    if (pktin->type == SSH2_MSG_REQUEST_SUCCESS) {
        /* one signature per key asked for, in the order asked; a missing
         * one fails its key */
        for (size_t i = 0; i < req->n; i++) {
            if (req->offers[i].action != KHK00_PROVE)
                continue;
            ptrlen sig = get_string(pktin);
            bool ok = !get_err(pktin) && hk00_verify(req, i, sig);
            req->offers[i].proof = ok ? 1 : -1;
        }
        hk00_apply(s, req);
    } else {
        hk00_record(s, dupprintf(KT_HOSTKEY_UPDATE_REFUSED, req->host,
                                 req->port));
        hk00_wev(s, req, "", NULL, "refused", NULL);
    }
    hk00_req_free(req);
}

void kitty_hk00_received(struct ssh2_connection_state *s, PktIn *pktin)
{
    const struct kitty_hk00_store *st = kitty_hk00_get_store();
    const char *host, *conn_type;
    int port, how;
    ptrlen sid;
    bool usable = true;

    if (s->kitty_hk00_seen)
        return;                         /* once per connection */
    s->kitty_hk00_seen = true;
    if (!s->kitty_transport || !st)
        return;
    how = ssh2_transport_kitty_hk00(s->kitty_transport, &conn_type, &host,
                                    &port, &sid);
    if (!kitty_hk00_eligible(kitty_hk00_enabled(), st->readonly(), how))
        return;

    hk00_req *req = snew(hk00_req);
    memset(req, 0, sizeof(*req));
    req->host = dupstr(host);
    req->port = port;
    req->conn_type = conn_type;
    req->sid = strbuf_new();
    put_datapl(req->sid, sid);

    while (get_avail(pktin)) {
        ptrlen blob = get_string(pktin);
        if (get_err(pktin)) {
            usable = false;             /* a cut list: not used */
            break;
        }
        BinarySource src[1];
        BinarySource_BARE_INIT_PL(src, blob);
        ptrlen name = get_string(src);
        const ssh_keyalg *alg = get_err(src) ? NULL : find_pubkey_alg_len(name);
        if (!alg || alg->is_certificate)
            continue;                   /* a type this program has no use for */
        ssh_key *key = ssh_key_new_pub(alg, blob);
        if (!key || req->n == HK00_MAX) {
            if (key)
                ssh_key_free(key);
            usable = false;             /* a known type that does not parse */
            break;
        }
        size_t i = req->n++;
        req->keys[i] = key;
        req->blobs[i] = strbuf_new();
        put_datapl(req->blobs[i], blob);
        req->keystrs[i] = ssh_key_cache_str(key);
        req->offers[i].type = ssh_key_cache_id(key);
        req->offers[i].status = check_stored_host_key(
            req->host, req->port, req->offers[i].type, req->keystrs[i]);
    }

    if (!usable || !kitty_hk00_plan(req->offers, req->n, req->conn_type)) {
        hk00_req_free(req);
        return;
    }

    size_t nprove = 0;
    for (size_t i = 0; i < req->n; i++)
        if (req->offers[i].action == KHK00_PROVE)
            nprove++;
    if (!nprove) {                      /* nothing new: removals only */
        hk00_apply(s, req);
        hk00_req_free(req);
        return;
    }

    PktOut *pktout = ssh_bpp_new_pktout(s->ppl.bpp, SSH2_MSG_GLOBAL_REQUEST);
    put_stringz(pktout, "hostkeys-prove-00@openssh.com");
    put_bool(pktout, true);             /* want reply */
    for (size_t i = 0; i < req->n; i++)
        if (req->offers[i].action == KHK00_PROVE)
            put_stringpl(pktout, ptrlen_from_strbuf(req->blobs[i]));
    pq_push(s->ppl.out_pq, pktout);
    ssh2_queue_global_request_handler(s, hk00_reply, req);
    s->kitty_hk00_pending = req;
    s->kitty_hk00_free = hk00_req_free;
}
