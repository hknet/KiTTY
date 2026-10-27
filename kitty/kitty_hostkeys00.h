/*
 * kitty_hostkeys00.h: OpenSSH's host key update (hostkeys-00@openssh.com /
 * hostkeys-prove-00@openssh.com, OpenSSH's UpdateHostKeys).
 *
 * After the login an OpenSSH server lists all its host keys. A connection
 * whose own host key was verified against the store (or stored by this
 * connection) asks the server to prove the keys the store does not hold,
 * then stores the proven ones, replaces a stored key of the same type, and
 * removes the stored types the server no longer offers. A later switch of
 * key type is then compared like any known key.
 *
 * This file: the switch, the store hook, and the decisions as pure functions
 * (no Windows or PuTTY headers: test/test_hostkeys00.c links it alone). The
 * packets are handled in kitty_hostkeys00_ssh.c; the transport layer records
 * how its key was verified (ssh/kex2-client.c).
 *
 * Member of the sshcommon library, so every SSH-capable binary links it.
 */
#ifndef KITTY_HOSTKEYS00_H
#define KITTY_HOSTKEYS00_H

#include <stddef.h>
#include <stdbool.h>

/* kitty.ini [KiTTY] updatehostkeys: on unless set to no */
void kitty_hk00_set_enabled(int on);
bool kitty_hk00_enabled(void);

/* What the update needs from the store beyond storage.h. Set by the
 * programs that have a store (kitty.exe: kitty_params.c, klink/kscp/ksftp:
 * kitty_console_store.c); unset, no update happens. */
struct kitty_hk00_store {
    bool (*readonly)(void);
    /* the stored key's fingerprint as accept-new's record shows it
     * ("ssh-ed25519 255 SHA256:..."), or NULL; caller frees */
    char *(*stored_fingerprint)(const char *host, int port, const char *keytype);
    /* remove the stored key and its stamps; true if it was removed */
    bool (*remove)(const char *host, int port, const char *keytype);
};
void kitty_hk00_set_store(const struct kitty_hk00_store *store);
const struct kitty_hk00_store *kitty_hk00_get_store(void);

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
    KHK00_NOTHING = 0,   /* equal to the stored key */
    KHK00_PROVE,         /* to be proven (planned) */
    KHK00_STORE,         /* proven, nothing of its type stored */
    KHK00_REPLACE,       /* proven, a different key of its type stored */
    KHK00_FAILED,        /* the proof failed or is missing */
    KHK00_SKIP           /* the connection's own type with another key */
};
struct kitty_hk00_offer {
    const char *type;    /* the store's id: rsa2, ssh-ed25519, ... */
    int status;          /* check_stored_host_key: 0 equal, 1 none, 2 differs */
    int proof;           /* 1 proven, -1 failed, 0 not asked */
    int action;
};

/* Before the proof request. false: the list is not used at all (empty, or a
 * type offered twice). Else each offer is NOTHING, PROVE or SKIP. */
bool kitty_hk00_plan(struct kitty_hk00_offer *o, size_t n, const char *conn_type);
/* After the proofs (or with none to ask for): each PROVE becomes STORE,
 * REPLACE or FAILED - or, when any proof failed, the proven ones NOTHING:
 * a failed proof leaves the store unchanged for this connection (as
 * OpenSSH's client). true: every key of the list is stored or proven now,
 * so stored types missing from the list may be removed. */
bool kitty_hk00_decide(struct kitty_hk00_offer *o, size_t n);
/* Remove this stored type? Never the connection's own type. */
bool kitty_hk00_removes(const struct kitty_hk00_offer *o, size_t n,
                        bool may_remove, const char *stored_type,
                        const char *conn_type);

/* The data a proof signs: string "hostkeys-prove-00@openssh.com", string
 * session id (the exchange hash of the first key exchange), string host key
 * blob. Writes it to `out` when it fits; returns its length. */
size_t kitty_hk00_sigdata(unsigned char *out, size_t outlen,
                          const unsigned char *sid, size_t sidlen,
                          const unsigned char *blob, size_t bloblen);

#endif
