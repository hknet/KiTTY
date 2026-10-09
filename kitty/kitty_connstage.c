/*
 * kitty_connstage.c: the hook behind kitty_connstage() (see the header).
 * In utils, so the network and SSH layers of every program link it; only a
 * KiTTY++ terminal sets it.
 */

#include "putty.h"
#include "kitty_connstage.h"

void (*kitty_connstage_hook)(Seat *seat, KittyConnStage stage,
                             ptrlen data) = NULL;
