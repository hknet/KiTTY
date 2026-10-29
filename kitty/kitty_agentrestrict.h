/*
 * kitty_agentrestrict.h - destination-restricted keys in the agent core:
 * OpenSSH's restrict-destination-v00@openssh.com key constraint (ssh-add -h,
 * PROTOCOL.agent section 2), the rules kageant keeps for a key, and the
 * check of a request against the session-binds of its connection.
 *
 * The rules live in ONE store, keyed by the key's fingerprint, as three
 * tables: destinations (one OpenSSH constraint each: an optional "from" hop,
 * the "to" hop with its host name, optional user and host keys), keys (the
 * fingerprint, comment and public blob of a key with rules) and links (which
 * key may go to which destination, with the link's own flags). A key with no
 * link is not restricted. A rule set from ssh-add -h and one made in kageant's
 * window end up in the same store, so a rule follows the key by fingerprint
 * however the key reaches the agent.
 *
 * Plain C, no Win32: the agent library carries this file (pageant.c calls
 * it), test/test_agentrestrict.c checks it. The file the store is kept in -
 * DPAPI, the path, the damaged file - is kageant's, kitty_kageant_rules.c.
 * Include after putty.h and ssh.h.
 */
#ifndef KITTY_AGENTRESTRICT_H
#define KITTY_AGENTRESTRICT_H

#define KAR_EXT_NAME "restrict-destination-v00@openssh.com"

/* OpenSSH's limits (ssh-agent.c): destinations per key, the user name's
 * length. The host name and the host keys of one hop are bounded too, so a
 * crafted constraint cannot make the store grow without end. */
#define KAR_MAX_RULES    1024
#define KAR_MAX_USER     256
#define KAR_MAX_HOST     1024
#define KAR_MAX_HOPKEYS  256

/* One host key of a hop: a plain key, or a CA key (is_ca) that a host
 * certificate for the hop's host name must be signed by. */
typedef struct kar_hostkey {
    strbuf *blob;
    bool is_ca;
} kar_hostkey;

/* One hop: user (the "to" hop only; NULL = any user), host name (NULL in a
 * "from" hop = the computer the agent runs on) and its host keys. */
typedef struct kar_hop {
    char *user;
    char *host;
    size_t nkeys, keysize;
    kar_hostkey *keys;
} kar_hop;

/* One destination: OpenSSH's dest_constraint. from.host NULL = the first
 * hop, from this computer; else the key may be used through an agent
 * forwarded to from.host, towards to.host. */
typedef struct kar_rule {
    kar_hop from, to;
} kar_rule;

typedef struct kar_rules {
    size_t n, size;
    kar_rule *v;
} kar_rules;

kar_rules *kar_rules_new(void);
void kar_rules_free(kar_rules *r);
void kar_hop_free(kar_hop *h);
void kar_hop_copy(kar_hop *dst, const kar_hop *src);
void kar_rule_copy(kar_rule *dst, const kar_rule *src);
bool kar_rule_equal(const kar_rule *a, const kar_rule *b);
/* Append a copy of r (false when the set is full). */
bool kar_rules_add(kar_rules *rs, const kar_rule *r);
/* Add one host key to a hop (a copy of blob). */
void kar_hop_add_key(kar_hop *h, ptrlen blob, bool is_ca);

/* Does this blob decode as a public key of a type we know? */
bool kar_blob_valid(ptrlen blob);

/*
 * The constraint's data as ssh-add -h sends it, after the extension name:
 * a string holding the destinations, each a string of (from hop, to hop,
 * reserved), each hop a string of (user, host, reserved, then host keys as
 * string blob + byte is_ca). Checked as OpenSSH checks it: a "from" hop
 * without a user and with a host name exactly when it has keys, a "to" hop
 * with a host name and at least one key, reserved fields empty, the limits
 * above. NULL with *err (static text) when it is refused.
 */
kar_rules *kar_parse_constraint(BinarySource *src, const char **err);
/* The same data written out, for the tests and the harness's wire form. */
void kar_encode_constraint(BinarySink *bs, const kar_rules *r);

/* A connection's verified session-binds, in the order bound (pageant.c's
 * PageantBinding): the server's host key, the session id it signed, and
 * whether it was bound for agent forwarding. */
typedef struct kar_bind {
    ptrlen hostkey;
    ptrlen sid;
    bool forwarding;
} kar_bind;

/*
 * Reasons a request is refused, as the agent log's reason field writes them.
 * unbound - no session-bind on the connection; bind-failed - a session-bind
 * was sent and refused; not-userauth - the data to sign is not a "publickey"
 * userauth request for this key; destination - no rule permits the hop;
 * forwarding-hop - a sign request on a connection bound for forwarding;
 * session-id - the data's session id is not the last bind's; hostkey - the
 * host key in a "publickey-hostbound" request is missing (more than one
 * bind) or not the last bind's; store-damaged - the rule file could not be
 * read, every restricted key is refused.
 */
#define KAR_WHY_UNBOUND       "unbound"
#define KAR_WHY_BIND_FAILED   "bind-failed"
#define KAR_WHY_NOT_USERAUTH  "not-userauth"
#define KAR_WHY_DESTINATION   "destination"
#define KAR_WHY_FWD_HOP       "forwarding-hop"
#define KAR_WHY_SESSION_ID    "session-id"
#define KAR_WHY_HOSTKEY       "hostkey"
#define KAR_WHY_STORE_DAMAGED "store-damaged"

/*
 * OpenSSH's identity_permitted(): may a restricted key be listed, removed
 * (user NULL) or used for a login as user on this connection? No bind is
 * local use (permitted) unless a bind was attempted and failed. Each bind
 * must be matched by a rule from the hop before it (the first one from this
 * computer); the user is checked on the last hop only; a key may not sign
 * on a hop bound for forwarding; a connection whose last bind is for
 * forwarding lists only keys with a rule from that host onwards.
 */
bool kar_permitted(const kar_rules *r, const kar_bind *b, size_t nb,
                   bool bind_attempted, const char *user, const char **why);

/*
 * The sign check of a restricted key (OpenSSH's process_sign_request2): a
 * bound connection, data that parses as a publickey userauth request for
 * keyblob, the binds permitted for the request's user, the data's session
 * id equal to the last bind's, and with more than one bind the
 * publickey-hostbound method carrying the last bind's host key.
 */
bool kar_sign_permitted(const kar_rules *r, const kar_bind *b, size_t nb,
                        bool bind_attempted, ptrlen keyblob, ptrlen data,
                        const char **why);

/* The data of a userauth signature: session id, 50, user, "ssh-connection",
 * "publickey" or "publickey-hostbound-v00@openssh.com", TRUE, algorithm, the
 * key (equal to keyblob), for the hostbound method the server's host key,
 * nothing after. *hostkey is empty for the plain method. */
bool kar_parse_userauth(ptrlen data, ptrlen keyblob, ptrlen *sid,
                        ptrlen *user, ptrlen *hostkey);

/* OpenSSH's match_pattern(): '*' any run, '?' one character. */
bool kar_wildcard(const char *s, const char *pattern);

/* A host key as the texts show it: "SHA256:<hash>", "CA SHA256:<hash>" for
 * a CA key. sfree it. */
char *kar_hostkey_text(const kar_hostkey *k);
/* One rule as text: "web01 (SHA256:...)", "alice@web01 (SHA256:...)",
 * "jump01 > web01 (SHA256:...)", "+N" after the first key when it has more.
 * sfree it. */
char *kar_rule_text(const kar_rule *r);
/* Every rule, ", " between them; "" for none. sfree it. */
char *kar_rules_text(const kar_rules *r);

/* ---- the store --------------------------------------------------------- */

#define KAR_LINK_FORWARD 1      /* the key may also be used through an agent
                                 * forwarded to this destination, towards the
                                 * key's other destinations */

typedef struct kar_dest {
    unsigned id;                /* never 0, unique in the store */
    kar_rule rule;
} kar_dest;

typedef struct kar_skey {
    char *fp;                   /* ssh2_fingerprint_blob(blob, SHA256) */
    char *comment;
    strbuf *blob;               /* the public blob; empty when not known */
} kar_skey;

typedef struct kar_link {
    char *fp;
    unsigned dest;
    unsigned flags;             /* KAR_LINK_* */
} kar_link;

typedef struct kar_store {
    size_t ndest, destsize, nkeys, keysize, nlinks, linksize;
    kar_dest *dest;
    kar_skey *keys;
    kar_link *links;
    unsigned next_id;
    /* The file could not be read: nothing is known about any key's rules,
     * so every key is treated as restricted with no rule (signing refused,
     * listed only on an unbound connection). Set by kageant's file layer. */
    bool damaged;
} kar_store;

kar_store *kar_store_new(void);
void kar_store_free(kar_store *s);
void kar_store_clear(kar_store *s);
/* dst becomes a copy of src (damaged flag included). */
void kar_store_copy(kar_store *dst, const kar_store *src);
/* The agent's one store (an empty one until something fills it). */
kar_store *kar_store_get(void);

/* The fingerprint the store files a key under. sfree it. */
char *kar_key_fp(ptrlen pubblob);

/* Has this key a link (it is restricted)? */
bool kar_store_restricted(const kar_store *s, const char *fp);
/* The rules of a key as the check takes them: its destinations, and for a
 * link with KAR_LINK_FORWARD a rule from that destination to each of the
 * key's other destinations reached from this computer. NULL when the key
 * has no link. kar_rules_free it. */
kar_rules *kar_store_rules(const kar_store *s, const char *fp);
kar_dest *kar_store_dest_by_id(const kar_store *s, unsigned id);
kar_skey *kar_store_key(const kar_store *s, const char *fp);
/* Add or update a key's row (comment, blob when given). */
kar_skey *kar_store_note_key(kar_store *s, const char *fp,
                             const char *comment, ptrlen blob);
/* The destination equal to r, added when there is none; its id. 0 when the
 * store is full. */
unsigned kar_store_dest_for(kar_store *s, const kar_rule *r);
void kar_store_link(kar_store *s, const char *fp, unsigned dest,
                    unsigned flags);
void kar_store_unlink_key(kar_store *s, const char *fp);
/* ssh-add -h on a key: its links are replaced by the given rules (a re-add
 * without -h leaves them, decision D8). */
void kar_store_set_key_rules(kar_store *s, const char *fp,
                             const char *comment, ptrlen blob,
                             const kar_rules *r);
/* Drop destinations no link uses and keys with no link. */
void kar_store_tidy(kar_store *s);

/* The store as the file holds it before DPAPI: a magic string, the three
 * tables and a SHA-256 of all of it. kar_store_decode fills s only when
 * every check passes; *err (static text) otherwise. */
void kar_store_encode(const kar_store *s, strbuf *out);
bool kar_store_decode(kar_store *s, ptrlen in, const char **err);

#endif /* KITTY_AGENTRESTRICT_H */
