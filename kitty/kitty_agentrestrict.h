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

/*
 * kageant's own agent extensions (docs/PROTOCOL-AGENT-KAPPER.md; their
 * names are in pageant.h KNOWN_EXTENSIONS too, so the "query" answer lists
 * them):
 *
 * withheld-keys-v00@kapper.net - no data. On a connection whose bindings
 * are all for a login, SSH_AGENT_SUCCESS and a uint32: how many keys the
 * key list of this connection leaves out because of their destination
 * rules. SSH_AGENT_FAILURE without a binding or with a binding for
 * forwarding: a remote server never learns of keys for other servers.
 *
 * session-bind-host-v00@kapper.net - string host, uint32 port: the name the
 * client connected to, sent right after its session-bind. Kept with the
 * last binding as the client's word, shown beside the host key's
 * fingerprint. The name decides nothing. The port is used for one decision
 * only: a port condition in the Valid hosts of a KAR_MATCH_CA_STORE
 * destination (kar_bind.port).
 *
 * key-state-v00@kapper.net - nothing, or string key blob: the state flags
 * (pageant.h LIST_EXTENDED_FLAG_*) of every SSH-2 key the key list of this
 * connection shows, or of that one key. SSH_AGENT_EXTENSION_RESPONSE,
 * string name, uint32 count, per key string blob and uint32 flags (RFC 9987
 * section 5.8). A key not held or left out of the list has no entry.
 */
#define KAR_EXT_WITHHELD    "withheld-keys-v00@kapper.net"
#define KAR_EXT_KEY_STATE   "key-state-v00@kapper.net"
#define KAR_EXT_BIND_HOST   "session-bind-host-v00@kapper.net"
#define KAR_MAX_BIND_HOST   255     /* bytes of the host name */
/* The names shown for one host key before "+N" (the client's included). */
#define KAR_DEST_MAX_NAMES  5

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

/*
 * How a hop's host name is matched. ssh-add -h always gives KAR_MATCH_EXACT:
 * OpenSSH's rules, unchanged. The rule editor gives a destination whose
 * host name is a pattern (* and ?, kar_wildcard) one of two choices, and
 * has a kind of its own for every server of a host CA:
 *
 * KAR_MATCH_EXACT - the hop's host keys: a plain key by equality, a host
 *   certificate signed by a CA key of the hop and valid for the host name
 *   as written.
 * KAR_MATCH_CA_DROPPED - the editor's former "Host CA only" choice, still
 *   read from a rule file of format 2. It matches no server: a pattern
 *   checked against a certificate's names was not tied to the CA store's
 *   "Valid hosts". The editor marks it and refuses OK until it is changed
 *   or removed (KAR_MATCH_CA_STORE replaces it).
 * KAR_MATCH_STORED_NOW ("Fixed list: host keys known now") - the host
 *   keys collected for the pattern when the rule was made, matched as
 *   KAR_MATCH_EXACT.
 * KAR_MATCH_STORED_LATER ("Checked at each login: any matching host") - the hop's host
 *   keys, or a bound host key stored under a name the pattern matches when
 *   the request comes (kar_hostkey_names: the KiTTY++ host-key store, and
 *   OpenSSH's known_hosts while kageant reads it). Never the name a client
 *   sends.
 * KAR_MATCH_CA_STORE ("All servers of a host CA") - the hop's host is the
 *   name of a CA of the KiTTY++ CA store, the hop holds no host key. At
 *   login the bound host key is a host certificate signed by that CA's
 *   public key, valid now, with a name the CA's "Valid hosts" expression
 *   admits. A port condition in it is checked against the port the client
 *   sent with session-bind-host-v00@kapper.net (kar_bind.port). A client
 *   that sent none (OpenSSH's ssh.exe) meets no port condition
 *   (cert_expr_match_str_noport), unless the destination carries
 *   KAR_HOP_IGNORE_PORT ("Ignore the port for clients that send none"):
 *   then the port conditions are dropped from the expression
 *   (cert_expr_match_str_dropport) and the name part decides alone. Key
 *   and expression are read from the CA store at that moment
 *   (kar_ca_lookup). A certificate without names passes only when the
 *   CA's record allows it (ca_options.permit_nameless).
 */
#define KAR_MATCH_EXACT        0
#define KAR_MATCH_CA_DROPPED   1
#define KAR_MATCH_STORED_NOW   2
#define KAR_MATCH_STORED_LATER 3
#define KAR_MATCH_CA_STORE     4
#define KAR_MATCH_MAX          4

/* One hop: user (the "to" hop only; NULL = any user), host name (NULL in a
 * "from" hop = the computer the agent runs on), its host keys, how the
 * host name is matched (KAR_MATCH_*) and KAR_HOP_* flags. */
#define KAR_HOP_IGNORE_PORT 1   /* KAR_MATCH_CA_STORE: a client that sends no
                                 * port is checked with the port conditions
                                 * of Valid hosts dropped */
typedef struct kar_hop {
    char *user;
    char *host;
    size_t nkeys, keysize;
    kar_hostkey *keys;
    unsigned match;
    unsigned flags;
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
/* The same for a key the hop does not hold yet (blob and is_ca compared):
 * true when it was added, false when the hop holds it already or holds
 * KAR_MAX_HOPKEYS keys. Collecting the keys of a "Fixed list: host keys
 * known now" destination again adds keys this way, never removes one. */
bool kar_hop_add_key_new(kar_hop *h, ptrlen blob, bool is_ca);

/* Does this blob decode as a public key of a type we know? */
bool kar_blob_valid(ptrlen blob);

/* Is this host name a pattern (holds * or ?)? */
bool kar_host_is_pattern(const char *host);
/* The rule editor's limit on a pattern host (":port" and [] ignored):
 * true when it ends in two labels without * or ?, with at least one label
 * holding the wildcard before them (*.customer.com). A host that is not a
 * pattern is always true. ssh-add -h rules are not checked: they keep
 * OpenSSH's semantics. */
bool kar_pattern_narrow_enough(const char *host);

/* "name", "name:port" or "[name]:port" -> the name (a new string) and the
 * port (22 when none is given). false for an empty name, a port that is
 * not 1 to 65535 or anything after "]" but ":port". */
bool kar_split_host_port(const char *text, char **name, int *port);
/* The name part of a hop's host as stored ("name", "name:port",
 * "[name]:port"): its start and length, without the port. A host with
 * more than one colon and no brackets (an IPv6 address) is all name. */
ptrlen kar_host_name_part(const char *host);

/*
 * kageant's -h: one destination as
 * OpenSSH's ssh-add -h takes it - "host", "user@host", "via>host",
 * "via>user@host" - where a host may carry ":port" or be "[name]:port".
 * The last host may be a pattern within kar_pattern_narrow_enough; the via
 * host is a plain name without a user. *via (NULL = from this computer),
 * *user (NULL = any user) and *host are new strings, the hosts as written
 * (the port kept). false with *err (static text) when it is refused.
 */
bool kar_dest_spec_parse(const char *spec, char **via, char **user,
                         char **host, const char **err);
/* A list of destinations separated by commas, blanks around each one
 * ignored: *items a new array of new strings (kar_names_free), *n their
 * number (0 for an empty list). false with *err (static text) and *bad (a
 * new string: the destination refused) when one is refused. */
bool kar_dest_list_parse(const char *list, char ***items, size_t *n,
                         char **bad, const char **err);
/* The items again as one list, ", " between them. sfree it. */
char *kar_dest_list_join(char *const *items, size_t n);

/*
 * A startup-key entry (kageant's [Agent] startupkeyN, or a line of the
 * registry's StartupKeys): the key file's path, then tokens, each after a
 * comma, in any order: "encrypted" / "plain", "confirm" / "helloconfirm",
 * "autoencrypt=<value>" and the fingerprint "SHA256:..." (also the longer
 * "alg bits SHA256:..." an earlier build wrote). The tokens are taken from
 * the right, one at a time; the first unknown one belongs to the path.
 * kar_entry_parse cuts them off entry in place: entry is the path
 * afterwards. enc -1 = no load mode given (the caller's default); autoenc
 * and fp point into entry (NULL / "" when not given).
 */
typedef struct kar_entry_tokens {
    int enc;
    int confirm;
    const char *autoenc;
    const char *fp;
} kar_entry_tokens;
void kar_entry_parse(char *entry, kar_entry_tokens *t);

/* The names a host key is stored under (the KiTTY++ host-key store, and
 * known_hosts while kageant reads it; "host" or "host:port"), for
 * KAR_MATCH_STORED_LATER: *names a new array of new strings
 * (kar_names_free), *kh a new array (sfree it; NULL when there is no name)
 * telling which names come from known_hosts, the count returned. NULL
 * (kageant sets it) = no names, so such a rule matches its host keys only. */
extern size_t (*kar_hostkey_names)(ptrlen hostkey, char ***names, bool **kh);
void kar_names_free(char **names, size_t n);

/* The CA of the KiTTY++ CA store named name, for KAR_MATCH_CA_STORE: its
 * public key blob (put into pubkey), its "Valid hosts" expression (*expr, a
 * new string) and whether its record accepts certificates without names.
 * false when the store has no CA of that name. NULL (kageant sets it) = no
 * CA store, so such a rule matches nothing. */
extern bool (*kar_ca_lookup)(const char *name, strbuf *pubkey, char **expr,
                             bool *nameless);

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
 * PageantBinding): the server's host key, the session id it signed,
 * whether it was bound for agent forwarding, and the port the client sent
 * with session-bind-host-v00@kapper.net (0 = none sent). */
typedef struct kar_bind {
    ptrlen hostkey;
    ptrlen sid;
    bool forwarding;
    unsigned port;
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
 * read, every restricted key is refused; no-port - a CA destination's
 * Valid hosts has a port condition, the client sent no port (the
 * destination would admit the server with the port ignored).
 */
#define KAR_WHY_UNBOUND       "unbound"
#define KAR_WHY_BIND_FAILED   "bind-failed"
#define KAR_WHY_NOT_USERAUTH  "not-userauth"
#define KAR_WHY_DESTINATION   "destination"
#define KAR_WHY_FWD_HOP       "forwarding-hop"
#define KAR_WHY_SESSION_ID    "session-id"
#define KAR_WHY_HOSTKEY       "hostkey"
#define KAR_WHY_STORE_DAMAGED "store-damaged"
#define KAR_WHY_NO_PORT       "no-port"

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
/* The same, *via (may be NULL) set to KAR_VIA_* bits when it permits:
 * KAR_VIA_KNOWN_HOSTS - a hop passed only through a name read from
 * known_hosts (KAR_MATCH_STORED_LATER), which the agent log records. */
#define KAR_VIA_KNOWN_HOSTS 1
bool kar_permitted_ex(const kar_rules *r, const kar_bind *b, size_t nb,
                      bool bind_attempted, const char *user,
                      const char **why, unsigned *via);

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
bool kar_sign_permitted_ex(const kar_rules *r, const kar_bind *b, size_t nb,
                           bool bind_attempted, ptrlen keyblob, ptrlen data,
                           const char **why, unsigned *via);

/*
 * The key list's order on a connection bound for a login: keys whose rules
 * permit this server by host keys alone (KAR_MATCH_EXACT, a CA certificate
 * for the name as written included) first, then keys permitted only by a
 * pattern rule (the other KAR_MATCH_*), then the unrestricted keys.
 * kar_list_group answers for a restricted key: KAR_GROUP_EXACT,
 * KAR_GROUP_PATTERN, or KAR_GROUP_NONE (not permitted, or the last binding
 * is not for a login).
 */
#define KAR_GROUP_NONE         0
#define KAR_GROUP_EXACT        1
#define KAR_GROUP_PATTERN      2
#define KAR_GROUP_UNRESTRICTED 3
int kar_list_group(const kar_rules *r, const kar_bind *b, size_t nb);
/* order[] = the indexes 0..n-1 sorted by group[] (1, 2, 3), each group in
 * the order given (stable). */
void kar_list_order(const int *group, size_t n, size_t *order);

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

/* The data of a session-bind-host-v00@kapper.net request after its name:
 * string host, uint32 port, nothing after. The host must be 1 to
 * KAR_MAX_BIND_HOST bytes without a control character, the port 1 to
 * 65535. *out = the name as shown: "host", "host:port" for another port
 * than 22 (sfree it). False with *why (static text) when it is refused. */
bool kar_parse_bind_host(BinarySource *src, char **out, const char **why);
/* The same, *port (may be NULL) set to the port sent. */
bool kar_parse_bind_host_port(BinarySource *src, char **out, unsigned *port,
                              const char **why);

/* The data of a key-state-v00@kapper.net request after its name: nothing
 * (every key) or string key blob (that key), nothing after. *one = a blob
 * was sent, *blob = it. False with *why (static text) when it is
 * refused. */
bool kar_parse_key_state(BinarySource *src, bool *one, ptrlen *blob,
                         const char **why);

/* PuTTY's list-extended, reencrypt and reencrypt-all: served to the
 * agent's own windows only (the in-process client). To a program over the
 * pipe or WM_COPYDATA they are unknown names (SSH_AGENT_FAILURE), and the
 * "query" answer leaves them out: list-extended answers SSH_AGENT_SUCCESS
 * with data, outside RFC 9987 section 5.8, and a remote host behind a
 * forwarded agent must not re-encrypt the user's keys. Programs read the
 * key state through key-state-v00@kapper.net. */
bool kar_ext_local_only(ptrlen name);

/* Why an external client's SSH-2 key list went out empty although the
 * agent holds SSH-2 keys (kageant's notice, event 104 with result
 * no-key-offered): store-damaged when the rule file cannot be read,
 * bind-failed when a session-bind was refused and none is kept,
 * forwarding-hop when the last binding is for forwarding, else
 * destination. NULL for a connection that never sent a session-bind: its
 * list leaves no key out. */
const char *kar_nokey_reason(bool store_damaged, const kar_bind *b,
                             size_t nb, bool bind_attempted);

/* The host names of one host key as the agent log and the confirmation
 * show them: the client's name first, marked " (client)", then the names
 * of the lookup (names[]) that differ from it, ignoring case, at most
 * KAR_DEST_MAX_NAMES names in all, then " +N" for the rest. client NULL:
 * the lookup's names alone. NULL when there is no name at all. sfree it. */
char *kar_dest_names_text(const char *client, char *const *names, size_t n);
/* The same with kh[i] (may be NULL) marking a name read from known_hosts:
 * shown with KT_KA_DEST_KH_MARK after it, compared without the mark. */
char *kar_dest_names_text_kh(const char *client, char *const *names,
                             const bool *kh, size_t n);

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

/*
 * Where a link came from, stored with it (format 3 of the file) and shown
 * in the rule editor's tree and in Key details (kar_origin_text):
 * KAR_ORIGIN_PROGRAM - a program over the pipe (ssh-add -h): its path as
 *   kageant found it behind the request (NULL when not known) and the time;
 *   the host keys are the ones that program looked up.
 * KAR_ORIGIN_EDITOR - set or changed in kageant's rule editor.
 * KAR_ORIGIN_UNKNOWN - a link of a file of format 1 or 2.
 */
#define KAR_ORIGIN_UNKNOWN 0
#define KAR_ORIGIN_PROGRAM 1
#define KAR_ORIGIN_EDITOR  2
#define KAR_ORIGIN_MAX     2
typedef struct kar_origin {
    unsigned kind;
    char *program;
    uint64_t when;              /* seconds since 1970, 0 = not known */
} kar_origin;

typedef struct kar_link {
    char *fp;
    unsigned dest;
    unsigned flags;             /* KAR_LINK_* */
    kar_origin origin;
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
/* Key details' list of a key's destinations as the store holds them (not
 * as kar_store_rules compiles them): per link a line "web01  user deploy
 * via jump01  forwarding" ("CA: <name> (Valid hosts: <expr>)" for a CA
 * destination, kar_ca_dest_text) and an indented line with its origin
 * (kar_origin_text),
 * "\r\n" between the lines. *count = the number of links. NULL (and 0)
 * when the key has no link. sfree it. */
char *kar_store_dest_lines(const kar_store *s, const char *fp,
                           size_t *count);
/* The same lines without their origin lines, the first max joined by
 * ", ", then " +N" for the rest: kageant's notices and hints. NULL when
 * the key has no link. sfree it. */
char *kar_store_dest_summary(const kar_store *s, const char *fp, size_t max);
/* A link's origin as the tree and Key details show it: "Set by <path>,
 * <date time> (host keys looked up by that program)", "Set in the rule
 * editor" or "Origin not recorded". For a program's rule, while known_hosts
 * is read (kitty_knownhosts.h), the rule's host keys compared with the ones
 * stored there under its host name: ", host keys as in known_hosts" or ",
 * host keys not in known_hosts: N of M". sfree it. */
char *kar_origin_text(const kar_origin *o, const kar_rule *r);
/* A CA destination's text: "CA: <name> (Valid hosts: <expr>)" as the CA
 * store holds it now, or "CA: <name> (not in the CA store: matches no
 * server)". ignore_port (KAR_HOP_IGNORE_PORT) adds ", port ignored for
 * clients that send none". sfree it. */
char *kar_ca_dest_text(const char *name, bool ignore_port);
void kar_origin_copy(kar_origin *dst, const kar_origin *src);
void kar_origin_free(kar_origin *o);
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
/* The same with its origin (a copy is kept; NULL = not recorded). An
 * existing link takes the new flags and origin. */
void kar_store_link_origin(kar_store *s, const char *fp, unsigned dest,
                           unsigned flags, const kar_origin *o);
void kar_store_unlink_key(kar_store *s, const char *fp);
/* ssh-add -h on a key: its links are replaced by the given rules (a re-add
 * without -h leaves them, decision D8), each with origin o (NULL = not
 * recorded). */
void kar_store_set_key_rules(kar_store *s, const char *fp,
                             const char *comment, ptrlen blob,
                             const kar_rules *r, const kar_origin *o);
/* Drop destinations no link uses and keys with no link. */
void kar_store_tidy(kar_store *s);

/* The store as the file holds it before DPAPI: a magic string, the three
 * tables and a SHA-256 of all of it. kar_store_decode fills s only when
 * every check passes; *err (static text) otherwise. */
void kar_store_encode(const kar_store *s, strbuf *out);
bool kar_store_decode(kar_store *s, ptrlen in, const char **err);
/* The bytes in a given format: 4 (what kar_store_encode writes: format 3,
 * each destination's KAR_MATCH_* value followed by its "to" hop's
 * KAR_HOP_* flags), 3 (format 2, each link followed by its origin), 2
 * (each destination ends in its KAR_MATCH_* value, KAR_MATCH_CA_STORE not
 * yet among them) or 1 (no match value: every destination
 * KAR_MATCH_EXACT). kar_store_decode reads all four; a link of format 1 or
 * 2 has its origin unknown, a destination of formats 1 to 3 no flags.
 * Formats 1 to 3 are written by the tests only. */
void kar_store_encode_format(const kar_store *s, strbuf *out, int format);

#endif /* KITTY_AGENTRESTRICT_H */
