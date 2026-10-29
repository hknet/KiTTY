/*
 * kitty_jumphost.h: the jump host given on the command line (klink, kscp,
 * ksftp, kitty.exe).
 *
 * -J [user@]host[:port] is the short form of -proxytype ssh -proxyhost
 * -proxyport -proxyuser, for ONE jump host. -proxyhostkey and
 * -proxy-accept-new set the jump host's host key check: they fill
 * CONF_proxy_manual_hostkeys and CONF_proxy_accept_new_hostkeys of the
 * session's Conf, and proxy/sshproxy.c copies them into the first link's
 * own Conf, where verify_ssh_host_key() reads them as for any connection.
 * A changed key stays a stop: a manual list refuses every other key, and
 * accept-new never takes a changed one.
 *
 * Member of the utils library, without MOD_PERSO: cmdline.c (the settings
 * library and the kitty targets), proxy/sshproxy.c (the crypto library) and
 * klink -scan call it.
 */
#ifndef KITTY_JUMPHOST_H
#define KITTY_JUMPHOST_H

struct conf_tag;

/* kitty_jump_parse's answers */
#define KITTY_JUMP_OK     0
#define KITTY_JUMP_CHAIN  1   /* a comma: a chain of jump hosts */
#define KITTY_JUMP_BAD    2   /* not [user@]host[:port] */

/* "[user@]host[:port]"; the host also as "[v6addr]" or as a bare IPv6
 * address (no port then). The user is the text before the LAST '@'. On
 * KITTY_JUMP_OK: *host is a fresh string, *user a fresh string or NULL
 * without one, *port the port or 22. Any other answer allocates nothing. */
int kitty_jump_parse(const char *spec, char **user, char **host, int *port);

/* The jump host's key check, from the session's Conf into a link's Conf:
 * every -proxyhostkey entry joins the link's own manual host keys (a saved
 * jump session keeps its list), -proxy-accept-new switches its accept-new
 * on (never off). depth = the link's place in the chain: only link 1 takes
 * them, a deeper link's Conf comes from its own saved session. */
void kitty_jump_hostkeys_to_hop(struct conf_tag *client, struct conf_tag *hop,
                                int depth);

/* The proxy settings of `from` into `to`: type, host, port, user, password,
 * the exclusion list, DNS, loopback, the telnet command, the chain limit
 * and the jump host's key settings. klink -scan builds a fresh Conf per key
 * type and reaches the host the way a login would. */
void kitty_jump_copy_proxy(struct conf_tag *from, struct conf_tag *to);

#endif
