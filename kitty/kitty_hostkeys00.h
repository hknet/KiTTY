/*
 * kitty_hostkeys00.h: OpenSSH's host key update (hostkeys-00@openssh.com /
 * hostkeys-prove-00@openssh.com, OpenSSH's UpdateHostKeys).
 *
 * After the login an OpenSSH server lists all its host keys. A connection
 * whose own host key was verified against the store (or stored by this
 * connection) requests proofs of the keys the store does not hold, then
 * stores the proven ones and removes the stored keys the server no
 * longer offers. Up to hostkeyspertype keys of one type are kept per host
 * and port, so a server can rotate its key: the new key is stored beside
 * the old one. When the server no longer lists the old one, it is
 * removed. The new one becomes key 1 (the primary, the one older readers
 * see). With hostkeyspertype=1 a proven key replaces the stored key
 * of its type.
 *
 * This file: the switches, the hooks for the store and the UI, the decisions
 * as pure functions (no Windows or PuTTY headers: test/test_hostkeys00.c
 * links it alone). The packets are handled in kitty_hostkeys00_ssh.c. The
 * transport layer records how its key was verified (ssh/kex2-client.c).
 *
 * Member of the sshcommon library, so every SSH-capable binary links it.
 */
#ifndef KITTY_HOSTKEYS00_H
#define KITTY_HOSTKEYS00_H

#include <stddef.h>
#include <stdbool.h>

/* kitty.ini [KiTTY] updatehostkeys: yes (the default: store, kitty.exe
 * shows a notice), ask (confirm all changes of a connection first), no */
enum { KHK00_MODE_NO = 0, KHK00_MODE_YES = 1, KHK00_MODE_ASK = 2 };
void kitty_hk00_set_mode(int mode);       /* anything else: yes */
int kitty_hk00_mode(void);
bool kitty_hk00_enabled(void);            /* mode != no */
/* the stored word -> the mode: "no", "ask" (any case), anything else yes */
int kitty_hk00_mode_from_word(const char *v);

/* kitty.ini [KiTTY] hostkeyspertype: keys of one type kept per host and
 * port, 1 to KHK00_PERTYPE_MAX, default 2 */
#define KHK00_PERTYPE_MAX 32
#define KHK00_PERTYPE_DEFAULT 2
void kitty_hk00_set_pertype(int n);       /* clamped to 1..KHK00_PERTYPE_MAX */
int kitty_hk00_pertype(void);

/* What the update needs from the store beyond storage.h. Set by the
 * programs that have a store (kitty.exe: kitty_params.c, klink/kscp/ksftp:
 * kitty_console_store.c). Unset, no update happens. Keys of one type are
 * numbered from 1 (the primary) without gaps. */
struct kitty_hk00_store {
    bool (*readonly)(void);
    /* how many keys of this type are stored for host and port */
    int (*count)(const char *host, int port, const char *keytype);
    /* key `n` of its type in the store's text, or NULL. Caller frees. */
    char *(*text)(const char *host, int port, const char *keytype, int n);
    /* key `n`'s fingerprint as accept-new's record shows it ("ssh-ed25519
     * 255 SHA256:..."), or NULL. Caller frees. */
    char *(*fingerprint)(const char *host, int port, const char *keytype, int n);
    /* key `n`'s stamp "First seen" (ISO local time), or NULL. Caller frees. */
    char *(*first_seen)(const char *host, int port, const char *keytype, int n);
    /* ... and its stamp "Last written", or NULL. Caller frees. */
    char *(*last_written)(const char *host, int port, const char *keytype, int n);
    /* write `key` as key `n` of its type, with its stamps */
    void (*store)(const char *host, int port, const char *keytype, int n,
                  const char *key);
    /* remove key `n` and its stamps. The keys after it move up one number
     * (key 2 becomes key 1). true if it was removed */
    bool (*remove)(const char *host, int port, const char *keytype, int n);
};
void kitty_hk00_set_store(const struct kitty_hk00_store *store);
const struct kitty_hk00_store *kitty_hk00_get_store(void);

/* updatehostkeys=ask: the changes of one connection, put to the user in
 * one question. */
enum { KHK00_CH_NEW = 1, KHK00_CH_GONE };
struct kitty_hk00_change {
    int kind;              /* KHK00_CH_NEW: a proven key to store. GONE: a
                            * stored key the server no longer offers */
    const char *fp;        /* "ssh-ed25519 255 SHA256:..." */
    const char *md5;       /* "MD5:..." of a new key, else NULL */
    const char *keytype;   /* the store's id */
    const char *beside;    /* NEW: key 1 of its type as stored, or NULL */
    const char *beside_first;  /* ... its "First seen", or NULL */
    const char *keytext;   /* NEW: the key in OpenSSH's one-line form */
    int after;             /* NEW: keys of its type once applied */
};
struct kitty_hk00_question {
    const char *host;
    int port;
    size_t n;
    const struct kitty_hk00_change *ch;
};
/* The answers. PENDING: the answer comes later through done(ctx, answer),
 * once, unless cancel(handle) came first. */
#define KHK00_ANS_UNANSWERED (-1)
#define KHK00_ANS_DECLINED   0
#define KHK00_ANS_APPLY      1
#define KHK00_ANS_PENDING    2
struct Seat;
struct kitty_hk00_ui {
    /* the question (NULL: nobody can answer, nothing changes). Returns an
     * answer, or KHK00_ANS_PENDING with *handle set. */
    int (*confirm)(struct Seat *seat, const struct kitty_hk00_question *q,
               void (*done)(void *ctx, int answer), void *ctx, void **handle);
    /* the connection is gone: done() is never called for this question */
    void (*cancel)(void *handle);
    /* updatehostkeys=yes: a key the update stored (kitty.exe's notice).
     * beside: key 1 of its type when the new key is stored beside it, else
     * NULL. replaced: the key it replaced (hostkeyspertype=1), else NULL.
     * NULL: no notice. */
    void (*stored)(struct Seat *seat, const char *host, int port,
                   const char *fp, const char *beside, const char *replaced);
};
void kitty_hk00_set_ui(const struct kitty_hk00_ui *ui);
const struct kitty_hk00_ui *kitty_hk00_get_ui(void);

/* How the connection's own host key was accepted. NONE: no host key was
 * verified (GSSAPI key exchange). */
enum {
    KHK00_BY_NONE = 0,
    KHK00_BY_STORE,      /* in the store: verified there, or stored now */
    KHK00_BY_ONCE,       /* used for this connection only (Connect Once) */
    KHK00_BY_MANUAL,     /* the session's manual host keys list */
    KHK00_BY_CERT        /* a certificate */
};
/* `status_now`: check_stored_host_key's answer AFTER the verification
 * (0 = the key is stored). */
int kitty_hk00_how(bool certificate, bool manual_list, int status_now);
bool kitty_hk00_eligible(bool enabled, bool readonly, int how);

/* One key of the server's list (certificates and unknown types left out). */
enum {
    KHK00_NOTHING = 0,   /* equal to a stored key of its type */
    KHK00_PROVE,         /* to be proven (planned) */
    KHK00_STORE,         /* proven, the first key of its type */
    KHK00_REPLACE,       /* proven, replaces the stored key (hostkeyspertype=1) */
    KHK00_FAILED,        /* the proof failed or is missing */
    KHK00_SKIP,          /* not stored: it would take the place of the key
                          * that verified the connection, or (over set)
                          * "Keys per type and host" has no room for it */
    KHK00_ADD            /* proven, stored beside the stored key(s) of its type */
};
struct kitty_hk00_offer {
    const char *type;    /* the store's id: rsa2, ssh-ed25519, ... */
    const char *key;     /* the key in the store's text */
    int status;          /* check_stored_host_key: 0 equal to a stored key of
                          * its type, 1 none of its type, 2 others of its type */
    int nstored;         /* keys of its type stored before the update */
    int proof;           /* 1 proven, -1 failed, 0 not requested */
    int action;
    int over;            /* 1: hostkeyspertype has no room for it. Proven all
                          * the same, so the limit's line lists only a key
                          * the server proved. Never stored. */
};

/* Before the proof request. conn_type / conn_key: the key that verified the
 * connection (its store id and text). Returns OK, or UNUSED (empty, or a key
 * listed twice: the list is not used at all), or OVERLIMIT (more keys of one
 * type than hostkeyspertype: *over_at is the first offer of that type,
 * *over_n how many of it). Each offer is then NOTHING, PROVE or SKIP. A key
 * the limit has no room for is PROVE with over set: kitty_hk00_decide makes
 * it SKIP once proven. With OVERLIMIT only the new keys of that type are
 * proven (over set), nothing of the list is stored. A type over the limit
 * with no new key: its first offer SKIP with over set. */
enum { KHK00_PLAN_OK = 0, KHK00_PLAN_UNUSED, KHK00_PLAN_OVERLIMIT };
int kitty_hk00_plan(struct kitty_hk00_offer *o, size_t n, const char *conn_type,
                    const char *conn_key, int pertype, size_t *over_at,
                    int *over_n);
/* After the proofs (or with none requested): each PROVE becomes STORE, ADD,
 * REPLACE or FAILED - or, when any proof failed, the proven ones NOTHING:
 * a failed proof leaves the store unchanged for this connection (as
 * OpenSSH's client). A proven key with no room left under hostkeyspertype
 * (the stored keys of its type stay, because the list is not complete)
 * becomes NOTHING. A PROVE with over set becomes SKIP when proven, FAILED
 * when not - its failed proof does not hold back the other keys, as it was
 * proven only for the limit's line. true: every key of the list is stored
 * or proven now, so stored keys missing from the list may be removed. */
bool kitty_hk00_decide(struct kitty_hk00_offer *o, size_t n, const char *conn_type,
                       const char *conn_key, int pertype);
/* Remove this stored key? Never the key that verified the connection, never
 * a key the server lists, never a key a REPLACE takes the place of. */
bool kitty_hk00_removes(const struct kitty_hk00_offer *o, size_t n,
                        bool may_remove, const char *stored_type,
                        const char *stored_key, const char *conn_type,
                        const char *conn_key);

/* The data a proof signs: string "hostkeys-prove-00@openssh.com", string
 * session id (the exchange hash of the first key exchange), string host key
 * blob. Writes it to `out` when it fits; returns its length. */
size_t kitty_hk00_sigdata(unsigned char *out, size_t outlen,
                          const unsigned char *sid, size_t sidlen,
                          const unsigned char *blob, size_t bloblen);

#endif
