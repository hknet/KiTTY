/*
 * kitty_kageant_dest.h - kageant's text of a session-bind destination
 * (session-bind@openssh.com, pageant.h struct kageant_dest): what the agent
 * log and the confirmation box show of the server a key is used for.
 * Include after putty.h and pageant.h.
 */
#ifndef KITTY_KAGEANT_DEST_H
#define KITTY_KAGEANT_DEST_H

struct kageant_dest;

/* One host key as the agent log writes it: "SHA256:<hash>", then a space
 * and the host name when one is found ("web01", "web01 +2" with two more
 * names). sfree it. */
char *kageant_dest_hostkey_text(ptrlen hostkey);

/* The agent log's dest= and fwd= values of a connection's bindings: dest
 * the server the login is for (the last binding, unless that one is for
 * forwarding), fwd the forwarding hops in the order bound, ", " between
 * them. Each NULL when there is none; sfree them. */
void kageant_dest_fields(const struct kageant_dest *d, char **dest,
                         char **fwd);

/* The confirmation box's lines for a bound connection: "Destination:
 * web01 (SHA256:...)" and one "Forwarded via: ..." per hop, a newline
 * between them. NULL when the connection sent no binding; sfree it. */
char *kageant_dest_box_text(const struct kageant_dest *d);

/* One host key found for a host name: its blob, whether it is a CA key
 * (a host certificate signed by it is accepted for the host) and where it
 * was found, as the rule editor shows it. */
struct kageant_dest_found {
    strbuf *blob;
    bool is_ca;
    char *source;
};
/* Every host key stored for host and port (22 when 0): the KiTTY++
 * host-key store in charge, the user's known_hosts and the CA store (a CA
 * whose host expression takes the host). Each key once. The count, *out
 * the array (NULL for none); kageant_dest_found_free it. */
size_t kageant_dest_lookup(const char *host, int port,
                           struct kageant_dest_found **out);
void kageant_dest_found_free(struct kageant_dest_found *v, size_t n);

#endif /* KITTY_KAGEANT_DEST_H */
