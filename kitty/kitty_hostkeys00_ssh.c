/*
 * kitty_hostkeys00_ssh.c: OpenSSH's host key update in the SSH-2 connection
 * layer - the server's "hostkeys-00@openssh.com" list, the
 * "hostkeys-prove-00@openssh.com" request and its answer, and what is then
 * stored and removed. The decisions are kitty_hostkeys00.c's.
 *
 * The order once the proofs are in: the changes are decided (keys to store,
 * stored keys to remove). With updatehostkeys=ask they are put to the user
 * in one question first (kitty_hk00_ui). Then the stored keys the server no
 * longer offers are removed. Last the proven keys are stored - a key of a
 * type already stored goes in beside it under the next number.
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
#include "../ssh/transport2.h"   /* ssh2_hostkey_algs: a type and its place in the host-key list */
#include "storage.h"
#include "kitty_hostkeys00.h"
#include "kitty_hostkey_acceptnew.h"   /* the record's second destination */
#include "kitty_text.h"
#include "kitty_winevent.h"              /* 213/313 */

/* More keys than any server has; a longer list is not used at all (a key
 * left out of it would read as no longer offered). */
#define HK00_MAX 32

/* A stored key the server no longer offers. */
struct hk00_gone {
    const ssh_keyalg *alg;       /* its type */
    char *key;                   /* its text in the store */
    char *fp;                    /* its fingerprint, or the type's name */
};

typedef struct hk00_req {
    struct ssh2_connection_state *s;
    char *host;
    int port;
    const char *conn_type;
    char *conn_key;
    strbuf *sid;
    size_t n;
    struct kitty_hk00_offer offers[HK00_MAX];
    ssh_key *keys[HK00_MAX];
    strbuf *blobs[HK00_MAX];
    char *keystrs[HK00_MAX];
    /* decided once the proofs are in */
    bool may_remove;
    struct hk00_gone *gone;
    size_t ngone, gone_alloc;
    /* updatehostkeys=ask: the question while it waits for its answer */
    struct kitty_hk00_change *ch;
    size_t nch;
    void *handle;
} hk00_req;

static void hk00_changes_free(hk00_req *req)
{
    for (size_t i = 0; i < req->nch; i++) {
        sfree((char *)req->ch[i].fp);
        sfree((char *)req->ch[i].md5);
        sfree((char *)req->ch[i].beside);
        sfree((char *)req->ch[i].beside_first);
        sfree((char *)req->ch[i].keytext);
    }
    sfree(req->ch);
    req->ch = NULL;
    req->nch = 0;
}

static void hk00_req_free(void *vreq)
{
    hk00_req *req = (hk00_req *)vreq;
    if (!req)
        return;
    if (req->handle) {
        /* the connection goes while its question is open: no answer is
         * delivered after this */
        const struct kitty_hk00_ui *ui = kitty_hk00_get_ui();
        if (ui && ui->cancel)
            ui->cancel(req->handle);
        req->handle = NULL;
    }
    for (size_t i = 0; i < req->n; i++) {
        ssh_key_free(req->keys[i]);
        strbuf_free(req->blobs[i]);
        sfree(req->keystrs[i]);
    }
    for (size_t i = 0; i < req->ngone; i++) {
        sfree(req->gone[i].key);
        sfree(req->gone[i].fp);
    }
    sfree(req->gone);
    hk00_changes_free(req);
    strbuf_free(req->sid);
    sfree(req->conn_key);
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

/* An offered key this program does not take from hostkeys-00, or NULL:
 *   - a DSA key: its proof is an SHA-1 signature, which session-bind
 *     refuses as well (no SHA-1 proof is taken);
 *   - a type below the warn line of the session's host-key list
 *     (CONF_ssh_hklist): a type the connection itself would only accept
 *     after a warning is not stored silently through an update. */
static const char *hk00_skip_reason(Conf *conf, const ssh_keyalg *alg)
{
    int id = -1;
    if (alg == &ssh_dsa)
        return KT_HOSTKEY_UPDATE_SKIP_DSA;
    for (size_t j = 0; j < N_HOSTKEY_ALGORITHMS; j++)
        if (!ssh2_hostkey_algs[j].alg->is_certificate &&
            !strcmp(ssh2_hostkey_algs[j].alg->cache_id, alg->cache_id)) {
            id = ssh2_hostkey_algs[j].id;
            break;
        }
    if (id < 0)
        return NULL;
    for (int i = 0; i < HK_MAX; i++) {
        int pref = conf_get_int_int(conf, CONF_ssh_hklist, i);
        if (pref == id)
            return NULL;               /* above the warn line */
        if (pref == HK_WARN)
            return KT_HOSTKEY_UPDATE_SKIP_WARN;
    }
    return NULL;
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

/* The limit hostkeyspertype blocks the change of the type of offer `at`:
 * the line with the reason, the stored and the offered keys of the type, one
 * line each, and event 213/313 over-limit. */
static void hk00_overlimit(struct ssh2_connection_state *s, hk00_req *req,
                           size_t at)
{
    const struct kitty_hk00_store *st = kitty_hk00_get_store();
    const char *type = req->offers[at].type;
    const char *ssh_id = ssh_key_alg(req->keys[at])->ssh_id;
    int count = st->count(req->host, req->port, type);
    hk00_record(s, dupprintf(KT_HOSTKEY_UPDATE_OVERLIMIT, req->host, req->port,
                             kitty_hk00_pertype(), ssh_id));
    for (int k = 1; k <= count; k++) {
        char *fp = st->fingerprint(req->host, req->port, type, k);
        hk00_record(s, dupprintf(KT_HOSTKEY_UPDATE_OVERLIMIT_STORED,
                                 fp ? fp : ssh_id));
        sfree(fp);
    }
    for (size_t i = 0; i < req->n; i++) {
        char *fp;
        if (strcmp(req->offers[i].type, type))
            continue;
        fp = ssh2_fingerprint(req->keys[i], SSH_FPTYPE_DEFAULT);
        hk00_record(s, dupprintf(KT_HOSTKEY_UPDATE_OVERLIMIT_OFFERED, fp));
        sfree(fp);
    }
    hk00_wev(s, req, ssh_id, NULL, "over-limit", NULL);
}

static bool hk00_is_new(int action)
{
    return action == KHK00_STORE || action == KHK00_ADD ||
        action == KHK00_REPLACE;
}

/* Is this stored key one the server no longer offers (req->gone)? */
static bool hk00_going(hk00_req *req, const char *cache_id, const char *key)
{
    for (size_t i = 0; i < req->ngone; i++)
        if (!strcmp(req->gone[i].alg->cache_id, cache_id) &&
            !strcmp(req->gone[i].key, key))
            return true;
    return false;
}

/* The stored keys the server no longer offers, when the list may remove
 * any: every stored key of every type, against the decision. */
static void hk00_collect_gone(hk00_req *req)
{
    const struct kitty_hk00_store *st = kitty_hk00_get_store();
    if (!req->may_remove)
        return;
    for (size_t ai = 0; ai < n_keyalgs; ai++) {
        const ssh_keyalg *alg = all_keyalgs[ai];
        int count;
        if (alg->is_certificate || hk00_seen_type(ai))
            continue;
        count = st->count(req->host, req->port, alg->cache_id);
        for (int k = 1; k <= count; k++) {
            char *text = st->text(req->host, req->port, alg->cache_id, k);
            if (text && kitty_hk00_removes(req->offers, req->n, true,
                                           alg->cache_id, text,
                                           req->conn_type, req->conn_key)) {
                struct hk00_gone *g;
                char *fp = st->fingerprint(req->host, req->port,
                                           alg->cache_id, k);
                sgrowarray(req->gone, req->gone_alloc, req->ngone);
                g = &req->gone[req->ngone++];
                g->alg = alg;
                g->key = text;
                g->fp = fp ? fp : dupstr(alg->ssh_id);
                text = NULL;
            }
            sfree(text);
        }
    }
}

/* The stored key of this type that stays first once the removals are done
 * (the key a new key is stored beside), as its number. 0 = none. */
static int hk00_beside_nth(hk00_req *req, const char *cache_id)
{
    const struct kitty_hk00_store *st = kitty_hk00_get_store();
    int count = st->count(req->host, req->port, cache_id);
    for (int k = 1; k <= count; k++) {
        char *text = st->text(req->host, req->port, cache_id, k);
        bool going = text && hk00_going(req, cache_id, text);
        sfree(text);
        if (!going)
            return k;
    }
    return 0;
}

/* The changes of this connection, for the question of updatehostkeys=ask. */
static void hk00_build_changes(hk00_req *req)
{
    const struct kitty_hk00_store *st = kitty_hk00_get_store();
    size_t cap = req->n + req->ngone;
    hk00_changes_free(req);
    if (!cap)
        return;
    req->ch = snewn(cap, struct kitty_hk00_change);
    memset(req->ch, 0, cap * sizeof(*req->ch));
    for (size_t i = 0; i < req->n; i++) {
        struct kitty_hk00_offer *o = &req->offers[i];
        struct kitty_hk00_change *c;
        ssh2_userkey uk = { .key = req->keys[i], .comment = NULL };
        int beside, after = 0;
        if (!hk00_is_new(o->action))
            continue;
        c = &req->ch[req->nch++];
        c->kind = KHK00_CH_NEW;
        c->keytype = o->type;
        c->fp = ssh2_fingerprint(req->keys[i], SSH_FPTYPE_DEFAULT);
        c->md5 = ssh2_fingerprint(req->keys[i], SSH_FPTYPE_MD5);
        c->keytext = ssh2_pubkey_openssh_str(&uk);
        beside = o->action == KHK00_REPLACE ? 1 :
                 o->action == KHK00_ADD ? hk00_beside_nth(req, o->type) : 0;
        if (beside) {
            c->beside = st->fingerprint(req->host, req->port, o->type, beside);
            c->beside_first = st->first_seen(req->host, req->port, o->type,
                                             beside);
        }
        /* the keys of its type once every change is applied */
        if (o->action == KHK00_REPLACE)
            after = 1;
        else {
            after = st->count(req->host, req->port, o->type);
            for (size_t g = 0; g < req->ngone; g++)
                if (!strcmp(req->gone[g].alg->cache_id, o->type))
                    after--;
            for (size_t j = 0; j < req->n; j++)
                if (!strcmp(req->offers[j].type, o->type) &&
                    (req->offers[j].action == KHK00_STORE ||
                     req->offers[j].action == KHK00_ADD))
                    after++;
        }
        c->after = after;
    }
    for (size_t g = 0; g < req->ngone; g++) {
        struct kitty_hk00_change *c = &req->ch[req->nch++];
        c->kind = KHK00_CH_GONE;
        c->keytype = req->gone[g].alg->cache_id;
        c->fp = dupstr(req->gone[g].fp);
    }
}

/* The stored key now first of its type, as text. NULL = none. */
static char *hk00_primary(hk00_req *req, const char *cache_id)
{
    const struct kitty_hk00_store *st = kitty_hk00_get_store();
    return st->text(req->host, req->port, cache_id, 1);
}

/* Remove the stored keys the server no longer offers, then store the proven
 * keys. notify: kitty.exe's notice for each key stored, with
 * updatehostkeys=yes (an answered question needs none). */
static void hk00_apply(struct ssh2_connection_state *s, hk00_req *req,
                       bool notify)
{
    const struct kitty_hk00_store *st = kitty_hk00_get_store();
    const struct kitty_hk00_ui *ui = kitty_hk00_get_ui();
    int pertype = kitty_hk00_pertype();

    /* the removals first: a key stored now takes the number a removed key
     * frees (kitty_hk00_decide counted that way). The primary of each type
     * before them, to see which key became key 1. */
    for (size_t g = 0; g < req->ngone; g++) {
        const char *cache_id = req->gone[g].alg->cache_id;
        bool first_of_type = true;
        char *before = NULL, *after;
        for (size_t h = 0; h < g; h++)
            if (!strcmp(req->gone[h].alg->cache_id, cache_id))
                first_of_type = false;
        if (!first_of_type)
            continue;                    /* the type is done with its first */
        before = hk00_primary(req, cache_id);
        for (size_t h = g; h < req->ngone; h++) {
            struct hk00_gone *gn = &req->gone[h];
            int count;
            if (strcmp(gn->alg->cache_id, cache_id))
                continue;
            count = st->count(req->host, req->port, cache_id);
            for (int k = 1; k <= count; k++) {
                char *text = st->text(req->host, req->port, cache_id, k);
                bool match = text && !strcmp(text, gn->key);
                sfree(text);
                if (!match)
                    continue;
                if (st->remove(req->host, req->port, cache_id, k)) {
                    hk00_record(s, dupprintf(KT_HOSTKEY_UPDATE_REMOVED, gn->fp,
                                             req->host, req->port));
                    hk00_wev(s, req, gn->alg->ssh_id, gn->fp, "removed", NULL);
                }
                break;
            }
        }
        after = hk00_primary(req, cache_id);
        if (before && after && strcmp(before, after)) {
            /* key 2 (or the next that stayed) moved to key 1's value */
            char *fp = st->fingerprint(req->host, req->port, cache_id, 1);
            hk00_record(s, dupprintf(KT_HOSTKEY_UPDATE_PRIMARY,
                                     fp ? fp : req->gone[g].alg->ssh_id,
                                     req->host, req->port));
            hk00_wev(s, req, req->gone[g].alg->ssh_id, fp, "made-primary", NULL);
            sfree(fp);
        }
        sfree(before);
        sfree(after);
    }

    for (size_t i = 0; i < req->n; i++) {
        struct kitty_hk00_offer *o = &req->offers[i];
        const char *ssh_id = ssh_key_alg(req->keys[i])->ssh_id;
        char *fp, *old;
        int nth;
        if (!hk00_is_new(o->action))
            continue;
        fp = ssh2_fingerprint(req->keys[i], SSH_FPTYPE_DEFAULT);
        if (o->action == KHK00_REPLACE) {
            /* hostkeyspertype=1: key 1 replaced. A key 2 left from a larger
             * setting goes with it. */
            old = st->fingerprint(req->host, req->port, o->type, 1);
            st->store(req->host, req->port, o->type, 1, req->keystrs[i]);
            while (st->count(req->host, req->port, o->type) > 1) {
                char *extra = st->fingerprint(req->host, req->port, o->type, 2);
                if (!st->remove(req->host, req->port, o->type, 2)) {
                    sfree(extra);
                    break;
                }
                hk00_record(s, dupprintf(KT_HOSTKEY_UPDATE_REMOVED,
                                         extra ? extra : ssh_id,
                                         req->host, req->port));
                hk00_wev(s, req, ssh_id, extra, "removed", NULL);
                sfree(extra);
            }
            hk00_record(s, dupprintf(KT_HOSTKEY_UPDATE_REPLACED, fp,
                                     old ? old : ssh_id, req->host, req->port));
            hk00_wev(s, req, ssh_id, fp, "replaced", old);
            if (notify && ui && ui->stored)
                ui->stored(s->ppl.seat, req->host, req->port, fp, NULL,
                           old ? old : ssh_id);
            sfree(old);
            sfree(fp);
            continue;
        }
        nth = st->count(req->host, req->port, o->type) + 1;
        if (nth > pertype || nth > KHK00_PERTYPE_MAX) {
            sfree(fp);                   /* no room left: never over the limit */
            continue;
        }
        st->store(req->host, req->port, o->type, nth, req->keystrs[i]);
        if (nth == 1) {
            hk00_record(s, dupprintf(KT_HOSTKEY_UPDATE_STORED, fp,
                                     req->host, req->port));
            hk00_wev(s, req, ssh_id, fp, "stored", NULL);
            if (notify && ui && ui->stored)
                ui->stored(s->ppl.seat, req->host, req->port, fp, NULL, NULL);
        } else {
            old = st->fingerprint(req->host, req->port, o->type, 1);
            hk00_record(s, dupprintf(KT_HOSTKEY_UPDATE_ADDED, nth, fp,
                                     old ? old : ssh_id, req->host, req->port));
            hk00_wev(s, req, ssh_id, fp, "added", old);
            if (notify && ui && ui->stored)
                ui->stored(s->ppl.seat, req->host, req->port, fp,
                           old ? old : ssh_id, NULL);
            sfree(old);
        }
        sfree(fp);
    }
}

/* updatehostkeys=ask, answered no or not at all: nothing changes. Each
 * change gets its line. */
static void hk00_refused_changes(struct ssh2_connection_state *s,
                                 hk00_req *req, int answer)
{
    bool declined = answer == KHK00_ANS_DECLINED;
    for (size_t i = 0; i < req->nch; i++) {
        const struct kitty_hk00_change *c = &req->ch[i];
        const char *fmt = c->kind == KHK00_CH_NEW ?
            (declined ? KT_HOSTKEY_UPDATE_DECLINED : KT_HOSTKEY_UPDATE_UNANSWERED) :
            (declined ? KT_HOSTKEY_UPDATE_KEPT : KT_HOSTKEY_UPDATE_KEPT_UNANSWERED);
        hk00_record(s, dupprintf(fmt, c->fp, req->host, req->port));
        hk00_wev(s, req, c->keytype, c->fp, declined ? "declined" : "unanswered",
                 NULL);
    }
}

static void hk00_conclude(hk00_req *req, int answer)
{
    if (answer == KHK00_ANS_APPLY)
        hk00_apply(req->s, req, false);
    else
        hk00_refused_changes(req->s, req, answer);
}

/* The question's answer (kitty_hk00_ui confirm, KHK00_ANS_PENDING): from a
 * top-level callback, while the connection is still there. */
static void hk00_answered(void *vreq, int answer)
{
    hk00_req *req = (hk00_req *)vreq;
    struct ssh2_connection_state *s = req->s;
    req->handle = NULL;
    if (s->kitty_hk00_pending == req)
        s->kitty_hk00_pending = NULL;
    hk00_conclude(req, answer);
    hk00_req_free(req);
}

/* The proofs are in (or none was needed). Returns true when the request
 * waits for the answer of updatehostkeys=ask and must not be freed. */
static bool hk00_finish(struct ssh2_connection_state *s, hk00_req *req)
{
    const struct kitty_hk00_ui *ui = kitty_hk00_get_ui();
    size_t nnew = 0;
    int answer;

    req->may_remove = kitty_hk00_decide(req->offers, req->n, req->conn_type,
                                        req->conn_key, kitty_hk00_pertype());
    for (size_t i = 0; i < req->n; i++) {
        struct kitty_hk00_offer *o = &req->offers[i];
        if (o->action == KHK00_FAILED) {
            char *fp = ssh2_fingerprint(req->keys[i], SSH_FPTYPE_DEFAULT);
            hk00_record(s, dupprintf(KT_HOSTKEY_UPDATE_FAILED, fp,
                                     req->host, req->port));
            hk00_wev(s, req, ssh_key_alg(req->keys[i])->ssh_id, fp,
                     "proof-failed", NULL);
            sfree(fp);
        } else if (hk00_is_new(o->action))
            nnew++;
    }
    /* "Keys per type and host" has no room: the reason as lines, once per
     * type, only for a key the server proved (or one already stored) */
    for (size_t i = 0; i < req->n; i++) {
        bool first = true;
        if (req->offers[i].action != KHK00_SKIP || !req->offers[i].over)
            continue;
        for (size_t j = 0; j < i; j++)
            if (req->offers[j].action == KHK00_SKIP && req->offers[j].over &&
                !strcmp(req->offers[j].type, req->offers[i].type))
                first = false;
        if (first)
            hk00_overlimit(s, req, i);
    }
    hk00_collect_gone(req);
    if (!nnew && !req->ngone)
        return false;                    /* nothing changes */

    if (kitty_hk00_mode() != KHK00_MODE_ASK) {
        hk00_apply(s, req, true);
        return false;
    }

    /* one question for all changes of this connection */
    hk00_build_changes(req);
    if (!ui || !ui->confirm) {
        answer = KHK00_ANS_UNANSWERED;   /* nobody to answer it */
    } else {
        struct kitty_hk00_question q;
        q.host = req->host;
        q.port = req->port;
        q.n = req->nch;
        q.ch = req->ch;
        req->s = s;
        answer = ui->confirm(s->ppl.seat, &q, hk00_answered, req, &req->handle);
        if (answer == KHK00_ANS_PENDING) {
            s->kitty_hk00_pending = req;
            s->kitty_hk00_free = hk00_req_free;
            return true;
        }
        req->handle = NULL;
    }
    req->s = s;
    hk00_conclude(req, answer);
    return false;
}

static void hk00_reply(struct ssh2_connection_state *s, PktIn *pktin,
                       void *ctx)
{
    hk00_req *req = (hk00_req *)ctx;

    if (s->kitty_hk00_pending == req)
        s->kitty_hk00_pending = NULL;
    if (pktin->type == SSH2_MSG_REQUEST_SUCCESS) {
        /* one signature per key requested, in the order requested. A
         * missing one fails its key. */
        for (size_t i = 0; i < req->n; i++) {
            if (req->offers[i].action != KHK00_PROVE)
                continue;
            ptrlen sig = get_string(pktin);
            bool ok = !get_err(pktin) && hk00_verify(req, i, sig);
            req->offers[i].proof = ok ? 1 : -1;
        }
        if (hk00_finish(s, req))
            return;                      /* waits for the answer */
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
    const char *host, *conn_type, *conn_key;
    int port, how, plan, over_n = 0;   /* over_n: kitty_hk00_plan's count */
    size_t over_at = 0;
    ptrlen sid;
    bool usable = true;

    if (s->kitty_hk00_seen)
        return;                         /* once per connection */
    s->kitty_hk00_seen = true;
    if (!s->kitty_transport || !st)
        return;
    how = ssh2_transport_kitty_hk00(s->kitty_transport, &conn_type, &conn_key,
                                    &host, &port, &sid);
    if (!kitty_hk00_eligible(kitty_hk00_enabled(), st->readonly(), how))
        return;

    hk00_req *req = snew(hk00_req);
    memset(req, 0, sizeof(*req));
    req->s = s;
    req->host = dupstr(host);
    req->port = port;
    req->conn_type = conn_type;
    req->conn_key = conn_key ? dupstr(conn_key) : NULL;
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
        {
            const char *skip = hk00_skip_reason(s->conf, alg);
            if (skip) {                 /* not taken: one line with the reason */
                hk00_record(s, dupprintf(skip, alg->ssh_id, req->host,
                                         req->port));
                continue;
            }
        }
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
        req->offers[i].key = req->keystrs[i];
        req->offers[i].status = check_stored_host_key(
            req->host, req->port, req->offers[i].type, req->keystrs[i]);
        req->offers[i].nstored = st->count(req->host, req->port,
                                           req->offers[i].type);
    }

    plan = usable ? kitty_hk00_plan(req->offers, req->n, req->conn_type,
                                    req->conn_key, kitty_hk00_pertype(),
                                    &over_at, &over_n) : KHK00_PLAN_UNUSED;
    /* OVERLIMIT (more keys of one type than hostkeyspertype) goes on: the
     * new keys of that type are proven first, the limit's lines come in
     * hk00_finish and nothing of the list is stored. The same for a key the
     * verifying key leaves no room for. */
    if (plan == KHK00_PLAN_UNUSED) {
        hk00_req_free(req);
        return;
    }

    size_t nprove = 0;
    for (size_t i = 0; i < req->n; i++)
        if (req->offers[i].action == KHK00_PROVE)
            nprove++;
    if (!nprove) {                      /* nothing new: removals only */
        if (!hk00_finish(s, req))
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
