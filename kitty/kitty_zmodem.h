/*
 * kitty_zmodem.h - ZModem transfers through the terminal (kitty_zmodem.c).
 */

#ifndef KITTY_ZMODEM_H
#define KITTY_ZMODEM_H
#include "putty.h"

/* ---- exported from kitty/kitty_zmodem.c ---- */
int kitty_zmodem_active(void);
void kitty_zmodem_cancel(void);
const char *kitty_zmodem_command(int send);
int kitty_zmodem_process(void);
int kitty_zmodem_receive(Conf *conf, Backend *backend, LogContext *logctx, Terminal *term);
size_t kitty_zmodem_recv_data(const void *data, size_t len);
int kitty_zmodem_send(HWND owner, Conf *conf, Backend *backend, LogContext *logctx, Terminal *term);

#endif /* KITTY_ZMODEM_H */
