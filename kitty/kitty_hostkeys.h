/*
 * kitty_hostkeys.h: the stored SSH host keys, listed and described.
 *
 * The store (windows/storage.c) can only answer "is this exact key known for
 * host:port". This module enumerates what is stored, turns each stored text
 * back into the public-key blob it came from - so its fingerprints and bit
 * length can be shown - and reads the two stamps the store keeps beside a
 * key since 0.85.1.8. Windows-only; include after
 * putty.h and ssh.h.
 */
#ifndef KITTY_HOSTKEYS_H
#define KITTY_HOSTKEYS_H

struct kitty_hostkey_entry {
    char *host;            /* as stored (a name or an address) */
    int port;
    char *keytype;         /* the store's id: rsa2, dss, ssh-ed25519, ecdsa-sha2-nistp256, ... */
    char *type_display;    /* the SSH wire name: ssh-rsa, ssh-dss, ssh-ed25519, ... */
    int bits;              /* key size where it means something, else 0 */
    char *sha256, *md5;    /* "SHA256:..." / "MD5:..." or "" when the text could not be parsed */
    char *first_seen;      /* ISO local time or "" */
    char *last_written;    /* ISO local time or "" */
};

struct kitty_hostkey_list {
    struct kitty_hostkey_entry *items;
    int n;
    size_t alloc;
};

/* Every key in the store in use (registry hive or SshHostKeys folder), in
 * store order. Never NULL; free with kitty_hostkeys_free. */
struct kitty_hostkey_list *kitty_hostkeys_enumerate(void);
void kitty_hostkeys_free(struct kitty_hostkey_list *l);

/* Remove one key and its stamps. true if something was removed. */
bool kitty_hostkey_delete(const char *host, int port, const char *keytype);

/* The store's text of a key -> the SSH public-key blob (caller frees), or
 * NULL when the type is unknown or the text does not parse. */
strbuf *kitty_hostkey_blob_from_text(const char *keytype, const char *text);

/* The key types a scan asks for, in the order tried: cache ids. */
const char *const *kitty_hostkey_scan_types(int *n);

/* Describe a key from its store text alone (klink -scan: the key just
 * presented, not yet stored): fills keytype, type_display, bits, sha256, md5
 * of a zeroed entry; the stamps stay empty. Free the strings yourself. */
void kitty_hostkey_describe_text(const char *keytype, const char *text,
                                 struct kitty_hostkey_entry *e);

/* One line describing a key the way the Host keys leaf and klink print it:
 * "host:port  type  bits  SHA256:...  MD5:..." (no stamps). Caller frees. */
char *kitty_hostkey_describe(const struct kitty_hostkey_entry *e);

/* The stamp names the store writes beside a key: "<name>:first" (set once)
 * and "<name>:when" (every write). ISO local time, caller frees. */
char *kitty_hostkey_now_iso(void);
#define KITTY_HOSTKEY_STAMP_FIRST ":first"
#define KITTY_HOSTKEY_STAMP_WHEN  ":when"

#endif /* KITTY_HOSTKEYS_H */
