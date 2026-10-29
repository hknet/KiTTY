/*
 * kitty_jumphost.c: the jump host given on the command line. See the
 * header.
 */
#include <string.h>
#include "putty.h"
#include "kitty_jumphost.h"

/* "1".."65535", digits only */
static bool jump_port(const char *s, int *port)
{
    long v = 0;
    if (!*s)
        return false;
    for (; *s; s++) {
        if (*s < '0' || *s > '9')
            return false;
        v = v * 10 + (*s - '0');
        if (v > 65535)
            return false;
    }
    if (v < 1)
        return false;
    *port = (int)v;
    return true;
}

int kitty_jump_parse(const char *spec, char **user, char **host, int *port)
{
    const char *at, *h;

    *user = *host = NULL;
    *port = 22;
    if (!spec || !*spec)
        return KITTY_JUMP_BAD;
    if (strchr(spec, ','))
        return KITTY_JUMP_CHAIN;
    at = strrchr(spec, '@');
    if (at == spec)
        return KITTY_JUMP_BAD;                 /* "@host": an empty user */
    h = at ? at + 1 : spec;
    if (*h == '[') {
        const char *close = strchr(h, ']');
        if (!close || close == h + 1)
            return KITTY_JUMP_BAD;
        if (close[1] == ':') {
            if (!jump_port(close + 2, port))
                return KITTY_JUMP_BAD;
        } else if (close[1]) {
            return KITTY_JUMP_BAD;
        }
        *host = dupprintf("%.*s", (int)(close - h - 1), h + 1);
    } else {
        const char *colon = strchr(h, ':');
        if (colon && !strchr(colon + 1, ':')) {    /* one colon: a port */
            if (colon == h || !jump_port(colon + 1, port))
                return KITTY_JUMP_BAD;
            *host = dupprintf("%.*s", (int)(colon - h), h);
        } else {
            if (!*h)
                return KITTY_JUMP_BAD;
            *host = dupstr(h);                 /* none, or a bare IPv6 address */
        }
    }
    if (at)
        *user = dupprintf("%.*s", (int)(at - spec), spec);
    return KITTY_JUMP_OK;
}

void kitty_jump_hostkeys_to_hop(Conf *client, Conf *hop, int depth)
{
    const char *key;

    if (depth != 1)
        return;
    for (int i = 0; (key = conf_get_str_nthstrkey(
                         client, CONF_proxy_manual_hostkeys, i)) != NULL; i++)
        conf_set_str_str(hop, CONF_ssh_manual_hostkeys, key, "");
    if (conf_get_bool(client, CONF_proxy_accept_new_hostkeys))
        conf_set_bool(hop, CONF_ssh_accept_new_hostkeys, true);
}

void kitty_jump_copy_proxy(Conf *from, Conf *to)
{
    const char *key;

    conf_set_int(to, CONF_proxy_type, conf_get_int(from, CONF_proxy_type));
    conf_set_str(to, CONF_proxy_host, conf_get_str(from, CONF_proxy_host));
    conf_set_int(to, CONF_proxy_port, conf_get_int(from, CONF_proxy_port));
    conf_set_str(to, CONF_proxy_username,
                 conf_get_str(from, CONF_proxy_username));
    /* held wrapped (kitty/kitty_pwmem.c): copied as it is */
    conf_set_str(to, CONF_proxy_password,
                 conf_get_str(from, CONF_proxy_password));
    conf_set_str(to, CONF_proxy_exclude_list,
                 conf_get_str(from, CONF_proxy_exclude_list));
    conf_set_int(to, CONF_proxy_dns, conf_get_int(from, CONF_proxy_dns));
    conf_set_bool(to, CONF_even_proxy_localhost,
                  conf_get_bool(from, CONF_even_proxy_localhost));
    conf_set_str(to, CONF_proxy_telnet_command,
                 conf_get_str(from, CONF_proxy_telnet_command));
    conf_set_int(to, CONF_proxy_chain_max,
                 conf_get_int(from, CONF_proxy_chain_max));
    conf_set_int(to, CONF_proxy_named_hostname,
                 conf_get_int(from, CONF_proxy_named_hostname));
    while ((key = conf_get_str_nthstrkey(
                to, CONF_proxy_manual_hostkeys, 0)) != NULL)
        conf_del_str_str(to, CONF_proxy_manual_hostkeys, key);
    for (int i = 0; (key = conf_get_str_nthstrkey(
                         from, CONF_proxy_manual_hostkeys, i)) != NULL; i++)
        conf_set_str_str(to, CONF_proxy_manual_hostkeys, key, "");
    conf_set_bool(to, CONF_proxy_accept_new_hostkeys,
                  conf_get_bool(from, CONF_proxy_accept_new_hostkeys));
}
