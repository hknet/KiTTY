/*
 * kitty_knock.h - port knocking before a connection: one codebase for
 * kitty.exe and the command-line tools (kscp, ksftp, klink).
 *
 * The sequence (CONF_portknockingoptions, -knock) is a list of entries
 * separated by spaces, tabs, ',', ';' or '-':
 *   port          a TCP knock (1-65535)
 *   port:tcp      the same
 *   port:udp      a UDP knock
 *   N:s           a pause of N seconds (decimals allowed, at most 60)
 * One entry that is not valid stops the whole sequence before any knock.
 *
 * The knocks follow the connection's route: direct, every address the name
 * lookup returns (IPv4 and IPv6) is knocked; through a proxy or an SSH jump
 * host, each TCP knock is a connection to host:port through it. A UDP knock
 * cannot go through a proxy, so such a sequence is refused before it starts.
 */
#ifndef KITTY_KNOCK_H
#define KITTY_KNOCK_H

#include <stdbool.h>
#include <stddef.h>
#include "putty.h"

#define KNOCK_PAUSE_MAX_MS 60000u   /* a longer pause is cut to this */

typedef enum { KNOCK_TCP, KNOCK_UDP, KNOCK_PAUSE } KnockKind;

typedef struct KnockStep {
    KnockKind kind;
    unsigned port;          /* KNOCK_TCP, KNOCK_UDP */
    unsigned ms;            /* KNOCK_PAUSE */
} KnockStep;

typedef struct KnockList {
    KnockStep *steps;
    size_t n, size;
    char *bad;              /* the first entry that is not valid, else NULL */
    char *cut;              /* the first pause over the limit, as written */
} KnockList;

/* kitty_knockparse.c. False when an entry is not valid (list->bad). */
bool knock_parse(const char *text, KnockList *list);
void knock_list_free(KnockList *list);

/*
 * kitty_knock.c. Starts the sequence of CONF_portknockingoptions to
 * CONF_host and returns at once; done(ctx) is called from the event loop
 * when the last knock is out, and the KittyKnock is freed before that call.
 * NULL = nothing to knock (no sequence, or one refused with its line in the
 * Event Log): connect at once, done is never called.
 */
typedef struct KittyKnock KittyKnock;
typedef void (*kitty_knock_done_fn)(void *ctx);
KittyKnock *kitty_knock_start(Conf *conf, LogContext *logctx,
                              kitty_knock_done_fn done, void *ctx);
/* Stops a running sequence; done is not called. NULL is allowed. */
void kitty_knock_cancel(KittyKnock *k);

/* kitty_knock_cli.c: the command-line tools knock to the end, in line. */
void kitty_knock_run_cli(Conf *conf, LogContext *logctx);

/* windows/network.c: every address of a looked-up name, port 0. */
typedef void (*kitty_sk_addr_fn)(void *ctx, const void *sa, int salen);
int kitty_sk_addr_each(SockAddr *addr, kitty_sk_addr_fn fn, void *ctx);

/* proxy/proxy.c: whether a connection to host:port uses the proxy. */
bool kitty_proxy_applies(const char *host, int port, Conf *conf);

#endif
