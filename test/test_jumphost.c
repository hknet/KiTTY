/*
 * test_jumphost - regression tests for kitty/kitty_jumphost.c: the -J
 * parser (a host, a user, a port, IPv6, a chain refused, malformed forms
 * refused), the jump host's key settings copied into the FIRST link's Conf
 * only (a union with the link's own list; accept-new switched on, never
 * off; the target's own -hostkey list never reaches the link), and the
 * proxy settings klink -scan carries into its own Conf.
 *
 * Linked with utils only. Returns non-zero on any failure.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "putty.h"
#include "kitty/kitty_jumphost.h"

void modalfatalbox(const char *p, ...)
{
    va_list ap;
    fprintf(stderr, "FATAL ERROR: ");
    va_start(ap, p);
    vfprintf(stderr, p, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}
void nonfatal(const char *p, ...)
{
    va_list ap;
    fprintf(stderr, "ERROR: ");
    va_start(ap, p);
    vfprintf(stderr, p, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static int failures = 0;

static void check(const char *what, bool ok)
{
    if (!ok) {
        printf("FAIL [%s]\n", what);
        failures++;
    }
}

static bool same(const char *a, const char *b)
{
    if (!a || !b)
        return a == b;
    return !strcmp(a, b);
}

/* one -J form: the answer, and on OK the user (NULL = none), host, port */
static void parse_case(const char *spec, int want, const char *wuser,
                       const char *whost, int wport)
{
    char *user = (char *)"unset", *host = (char *)"unset";
    int port = -1;
    int got = kitty_jump_parse(spec, &user, &host, &port);
    char *what = dupprintf("-J \"%s\": answer %d, want %d",
                           spec ? spec : "(null)", got, want);
    check(what, got == want);
    sfree(what);
    if (got == KITTY_JUMP_OK && want == KITTY_JUMP_OK) {
        what = dupprintf("-J \"%s\": user [%s] host [%s] port %d",
                         spec, user ? user : "(none)", host ? host : "(none)", port);
        check(what, same(user, wuser) && same(host, whost) && port == wport);
        sfree(what);
    }
    if (got != KITTY_JUMP_OK) {
        what = dupprintf("-J \"%s\": nothing allocated", spec ? spec : "(null)");
        check(what, user == NULL && host == NULL);
        sfree(what);
    }
    if (got == KITTY_JUMP_OK) {
        sfree(user);
        sfree(host);
    }
}

static int count_keys(Conf *conf, int primary)
{
    int n = 0;
    while (conf_get_str_nthstrkey(conf, primary, n))
        n++;
    return n;
}

/* a Conf with the keys the functions read, as do_defaults() leaves them */
static Conf *proxy_conf(void)
{
    Conf *c = conf_new();
    conf_set_int(c, CONF_proxy_type, PROXY_NONE);
    conf_set_str(c, CONF_proxy_host, "proxy");
    conf_set_int(c, CONF_proxy_port, 80);
    conf_set_str(c, CONF_proxy_username, "");
    conf_set_str(c, CONF_proxy_password, "");
    conf_set_str(c, CONF_proxy_exclude_list, "");
    conf_set_int(c, CONF_proxy_dns, AUTO);
    conf_set_bool(c, CONF_even_proxy_localhost, false);
    conf_set_str(c, CONF_proxy_telnet_command, "connect %host %port\\n");
    conf_set_int(c, CONF_proxy_chain_max, 0);
    conf_set_int(c, CONF_proxy_named_hostname, 0);
    conf_set_bool(c, CONF_proxy_accept_new_hostkeys, false);
    conf_set_bool(c, CONF_ssh_accept_new_hostkeys, false);
    return c;
}

int main(void)
{
    /* ---- -J [user@]host[:port] ---- */
    parse_case("jump", KITTY_JUMP_OK, NULL, "jump", 22);
    parse_case("deploy@jump", KITTY_JUMP_OK, "deploy", "jump", 22);
    parse_case("deploy@jump:2200", KITTY_JUMP_OK, "deploy", "jump", 2200);
    parse_case("jump:2200", KITTY_JUMP_OK, NULL, "jump", 2200);
    parse_case("192.0.2.20:65535", KITTY_JUMP_OK, NULL, "192.0.2.20", 65535);
    parse_case("[2001:db8::1]:2200", KITTY_JUMP_OK, NULL, "2001:db8::1", 2200);
    parse_case("deploy@[2001:db8::1]", KITTY_JUMP_OK, "deploy", "2001:db8::1", 22);
    parse_case("2001:db8::1", KITTY_JUMP_OK, NULL, "2001:db8::1", 22);
    parse_case("a@b@jump", KITTY_JUMP_OK, "a@b", "jump", 22);
    /* a chain is refused, never cut at the comma */
    parse_case("jump1,jump2", KITTY_JUMP_CHAIN, NULL, NULL, 0);
    parse_case("u@j1:22,u@j2", KITTY_JUMP_CHAIN, NULL, NULL, 0);
    parse_case("jump,", KITTY_JUMP_CHAIN, NULL, NULL, 0);
    /* malformed */
    parse_case("", KITTY_JUMP_BAD, NULL, NULL, 0);
    parse_case(NULL, KITTY_JUMP_BAD, NULL, NULL, 0);
    parse_case("@jump", KITTY_JUMP_BAD, NULL, NULL, 0);
    parse_case("user@", KITTY_JUMP_BAD, NULL, NULL, 0);
    parse_case("jump:", KITTY_JUMP_BAD, NULL, NULL, 0);
    parse_case(":22", KITTY_JUMP_BAD, NULL, NULL, 0);
    parse_case("jump:0", KITTY_JUMP_BAD, NULL, NULL, 0);
    parse_case("jump:65536", KITTY_JUMP_BAD, NULL, NULL, 0);
    parse_case("jump:22x", KITTY_JUMP_BAD, NULL, NULL, 0);
    parse_case("jump:-1", KITTY_JUMP_BAD, NULL, NULL, 0);
    parse_case("[::1", KITTY_JUMP_BAD, NULL, NULL, 0);
    parse_case("[]", KITTY_JUMP_BAD, NULL, NULL, 0);
    parse_case("[::1]x", KITTY_JUMP_BAD, NULL, NULL, 0);
    parse_case("[::1]:", KITTY_JUMP_BAD, NULL, NULL, 0);

    /* ---- the key settings into the first link ---- */
    {
        Conf *client = proxy_conf(), *hop = proxy_conf();
        conf_set_str_str(client, CONF_proxy_manual_hostkeys, "SHA256:jumpA", "");
        conf_set_str_str(client, CONF_proxy_manual_hostkeys, "SHA256:jumpB", "");
        conf_set_bool(client, CONF_proxy_accept_new_hostkeys, true);
        /* the target's own pin: never the jump host's */
        conf_set_str_str(client, CONF_ssh_manual_hostkeys, "SHA256:target", "");
        /* the link's own list, as a saved jump session carries it */
        conf_set_str_str(hop, CONF_ssh_manual_hostkeys, "SHA256:own", "");

        kitty_jump_hostkeys_to_hop(client, hop, 1);
        check("link 1: its own key and both -proxyhostkey keys",
              count_keys(hop, CONF_ssh_manual_hostkeys) == 3 &&
              conf_get_str_str_opt(hop, CONF_ssh_manual_hostkeys, "SHA256:own") &&
              conf_get_str_str_opt(hop, CONF_ssh_manual_hostkeys, "SHA256:jumpA") &&
              conf_get_str_str_opt(hop, CONF_ssh_manual_hostkeys, "SHA256:jumpB"));
        check("link 1: the target's -hostkey is not among them",
              !conf_get_str_str_opt(hop, CONF_ssh_manual_hostkeys, "SHA256:target"));
        check("link 1: accept-new on", conf_get_bool(hop, CONF_ssh_accept_new_hostkeys));
        check("link 1: the session's own lists unchanged",
              count_keys(client, CONF_proxy_manual_hostkeys) == 2 &&
              count_keys(client, CONF_ssh_manual_hostkeys) == 1);
        conf_free(hop);

        /* a deeper link and the session itself take nothing */
        for (int depth = 0; depth <= 3; depth += 2) {
            char what[64];
            hop = proxy_conf();
            kitty_jump_hostkeys_to_hop(client, hop, depth);
            snprintf(what, sizeof(what), "depth %d: no keys, accept-new off", depth);
            check(what, count_keys(hop, CONF_ssh_manual_hostkeys) == 0 &&
                  !conf_get_bool(hop, CONF_ssh_accept_new_hostkeys));
            conf_free(hop);
        }

        /* without -proxy-accept-new a saved jump session keeps its own */
        conf_set_bool(client, CONF_proxy_accept_new_hostkeys, false);
        hop = proxy_conf();
        conf_set_bool(hop, CONF_ssh_accept_new_hostkeys, true);
        kitty_jump_hostkeys_to_hop(client, hop, 1);
        check("link 1: the session's AcceptNewHostKeys=1 stays on",
              conf_get_bool(hop, CONF_ssh_accept_new_hostkeys));
        conf_free(hop);
        hop = proxy_conf();
        kitty_jump_hostkeys_to_hop(client, hop, 1);
        check("link 1: accept-new stays off without the switch",
              !conf_get_bool(hop, CONF_ssh_accept_new_hostkeys));
        conf_free(hop);
        conf_free(client);
    }

    /* ---- the proxy settings -scan carries ---- */
    {
        Conf *from = proxy_conf(), *to = proxy_conf();
        conf_set_int(from, CONF_proxy_type, PROXY_SSH_TCPIP);
        conf_set_str(from, CONF_proxy_host, "jump.example.com");
        conf_set_int(from, CONF_proxy_port, 2200);
        conf_set_str(from, CONF_proxy_username, "deploy");
        conf_set_bool(from, CONF_even_proxy_localhost, true);
        conf_set_int(from, CONF_proxy_chain_max, 3);
        conf_set_str_str(from, CONF_proxy_manual_hostkeys, "SHA256:jumpA", "");
        conf_set_bool(from, CONF_proxy_accept_new_hostkeys, true);
        conf_set_str_str(to, CONF_proxy_manual_hostkeys, "SHA256:stale", "");

        kitty_jump_copy_proxy(from, to);
        check("copy: type, host, port, user",
              conf_get_int(to, CONF_proxy_type) == PROXY_SSH_TCPIP &&
              same(conf_get_str(to, CONF_proxy_host), "jump.example.com") &&
              conf_get_int(to, CONF_proxy_port) == 2200 &&
              same(conf_get_str(to, CONF_proxy_username), "deploy"));
        check("copy: loopback and the chain limit",
              conf_get_bool(to, CONF_even_proxy_localhost) &&
              conf_get_int(to, CONF_proxy_chain_max) == 3);
        check("copy: the jump host's keys replace what was there",
              count_keys(to, CONF_proxy_manual_hostkeys) == 1 &&
              conf_get_str_str_opt(to, CONF_proxy_manual_hostkeys, "SHA256:jumpA"));
        check("copy: -proxy-accept-new", conf_get_bool(to, CONF_proxy_accept_new_hostkeys));
        conf_free(from);
        conf_free(to);
    }

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("test_jumphost: all passed\n");
    return 0;
}
