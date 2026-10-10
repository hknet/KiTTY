/*
 * kitty_reply.c - the terminal's answers to the host: one seam for every
 * reply KiTTY++ builds itself (kitty_reply.h). Split out of kitty_osc52.c in
 * 0.85.1.15, where it had grown from the clipboard's reply into the reply of
 * every query; the clipboard keeps kitty_osc52_send_raw, which comes here.
 */
#include "putty.h"
#include "terminal.h"
#include "kitty_reply.h"

void kitty_term_send_reply(Terminal *term, const char *data, size_t len)
{
    if (!term || !data || !len)
        return;

    /*
     * Straight at the BACKEND, deliberately, and NOT through ldisc_send().
     *
     * ldisc_send() feeds the LOCAL LINE EDITOR. With local line editing on -
     * which is the default for Raw, and which anyone may switch on for any
     * protocol - our reply is not sent at all: it is appended to the line the
     * user is typing and leaves only when they press Return. Found the only way
     * it could be found, by watching the wire: an allowed clipboard read
     * produced the amber activity marker and the "clipboard sent" log line,
     * and the host got nothing until a Ctrl+D flushed the buffer minutes later.
     *
     * A protocol reply is not typed input. It must not be echoed, must not be
     * editable, and must not queue behind a half-typed command. PuTTY's own
     * DA/DSR replies do go through ldisc and have the same quirk; that is
     * upstream's business, and this is not the place to change it.
     */
    if (!term->backend)
        return;                        /* no connection to reply down */
    backend_send(term->backend, data, (int)len);
}
