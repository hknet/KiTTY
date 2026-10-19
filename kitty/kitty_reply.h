/*
 * kitty_reply.h - the terminal's answers to the host (kitty_reply.c).
 */
#ifndef KITTY_REPLY_H
#define KITTY_REPLY_H

#include "putty.h"

/* A complete, already-built reply to something the host requested or announced
 * (DECRQM, XTGETTCAP, colour queries, Capabilities, focus events, kitty
 * graphics, file transfer, notifications ...), straight to the connection.
 * Clipboard contents do NOT go through here directly: they take
 * kitty_osc52_send_raw (kitty_osc52.c), so the one event that must never
 * happen by accident - the clipboard leaving the machine - stays a single
 * function a test can count and a breakpoint can catch. */
void kitty_term_send_reply(Terminal *term, const char *data, size_t len);

#endif
