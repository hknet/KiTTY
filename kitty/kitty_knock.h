/*
 * kitty_knock.h - port knocking before a connection: one codebase for
 * kitty.exe and the command-line tools (kscp, ksftp, klink).
 *
 * The sequence (CONF_portknockingoptions, -knock) is a list of entries
 * separated by spaces, tabs, ',' or ';' ("2001-2005" is no range and
 * not two knocks: it is refused):
 *   port          a TCP knock (1-65535)
 *   port:tcp      the same
 *   port:udp      a UDP knock
 *   N:s           a pause of N seconds (decimals allowed, at most 60)
 * A sequence is refused - no knock, no connection - when an entry is not
 * valid or when it holds more than KNOCK_MAX_STEPS entries.
 *
 * The knocks follow the connection's route: direct, every address the name
 * lookup returns (IPv4 and IPv6) is knocked; through a proxy or an SSH jump
 * host, each TCP knock is a connection to host:port through it. A UDP knock
 * cannot go through a proxy: such a sequence is not sent, the connection
 * goes ahead without knocks (from a place behind a proxy, such as an office,
 * the knocks are usually not needed).
 */
#ifndef KITTY_KNOCK_H
#define KITTY_KNOCK_H

#include <stdbool.h>
#include <stddef.h>
#include "putty.h"

#define KNOCK_PAUSE_MAX_MS 60000u   /* a longer pause is cut to this */
#define KNOCK_MAX_STEPS    64u      /* knocks and pauses, at most */

typedef enum { KNOCK_TCP, KNOCK_UDP, KNOCK_PAUSE } KnockKind;

typedef struct KnockStep {
    KnockKind kind;
    unsigned port;          /* KNOCK_TCP, KNOCK_UDP */
    unsigned ms;            /* KNOCK_PAUSE */
} KnockStep;

typedef struct KnockList {
    KnockStep *steps;
    size_t n, size;
    char *bad;              /* the first entry that is not valid, else NULL;
                             * bytes outside printable ASCII as \xNN */
    bool too_many;          /* more than KNOCK_MAX_STEPS entries */
    char *cut;              /* the first pause over the limit, as written */
} KnockList;

/* kitty_knockparse.c. False when an entry is not valid (list->bad) or the
 * sequence is too long (list->too_many). */
bool knock_parse(const char *text, KnockList *list);
void knock_list_free(KnockList *list);

/*
 * kitty_knock.c. Starts the sequence of CONF_portknockingoptions to
 * CONF_host and returns at once; done(ctx) is called from the event loop
 * when the last knock is out, and the KittyKnock is freed before that call.
 * NULL with *refusal NULL = nothing to knock (no sequence, the address not
 * found, or a UDP knock through a proxy, with its line): connect at once,
 * done is never called.
 * NULL with *refusal set = the sequence refused (see above): the failure
 * line, also in the Event Log; the caller frees it and does not connect.
 *
 * note (may be NULL) gets each failure the user should see beside the
 * Event Log: a knock through the proxy that failed or got no answer, which
 * a later connection failure may stem from, and a sequence not sent because
 * of a UDP knock through a proxy. kitty.exe prints it in the terminal (the
 * tools have it on standard error already).
 */
typedef struct KittyKnock KittyKnock;
typedef void (*kitty_knock_done_fn)(void *ctx);
typedef void (*kitty_knock_note_fn)(void *ctx, const char *line);
KittyKnock *kitty_knock_start(Conf *conf, LogContext *logctx,
                              kitty_knock_done_fn done,
                              kitty_knock_note_fn note, void *ctx,
                              char **refusal);
/* Stops a running sequence; done is not called. NULL is allowed. */
void kitty_knock_cancel(KittyKnock *k);

/* kitty_knock_cli.c: the command-line tools knock to the end, in line; a
 * refused sequence ends the tool with exit code 1. */
void kitty_knock_run_cli(Conf *conf, LogContext *logctx);

/* windows/network.c: every address of a looked-up name, port 0. */
typedef void (*kitty_sk_addr_fn)(void *ctx, const void *sa, int salen);
int kitty_sk_addr_each(SockAddr *addr, kitty_sk_addr_fn fn, void *ctx);

/* proxy/proxy.c: whether a connection to host:port uses the proxy. */
bool kitty_proxy_applies(const char *host, int port, Conf *conf);

#endif
