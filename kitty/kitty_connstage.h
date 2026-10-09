/*
 * kitty_connstage.h: how far a connection attempt got, reported to whoever
 * listens (hknet/KiTTY#66).
 *
 * The network and SSH layers are upstream code compiled into libraries every
 * program links, without MOD_PERSO. They call kitty_connstage() at a few
 * points; it does nothing until a KiTTY++ terminal sets kitty_connstage_hook,
 * which feeds the reconnect decision (kitty/kitty_reconnect.c): an attempt
 * that never reached the server is retried differently from one that failed
 * at the login.
 */

#ifndef KITTY_CONNSTAGE_H
#define KITTY_CONNSTAGE_H

typedef enum {
    KCS_TCP_UP,             /* the connection to the server (or proxy) is up */
    KCS_USERAUTH_START,     /* SSH user authentication has begun */
    KCS_AGENT_SIGN_BEGIN,   /* a signature requested from the agent; data = key blob */
    KCS_AGENT_SIGN_END,     /* ...and the agent answered */
} KittyConnStage;

/* seat: the Seat of the connection (a jump host's has its own); data: see
 * the stage, else empty */
extern void (*kitty_connstage_hook)(Seat *seat, KittyConnStage stage,
                                    ptrlen data);

static inline void kitty_connstage(Seat *seat, KittyConnStage stage,
                                   ptrlen data)
{
    if (kitty_connstage_hook)
        kitty_connstage_hook(seat, stage, data);
}

#endif
