#include <assert.h>
#include <string.h>

#include "putty.h"
#include "network.h"
#include "kitty/kitty_connstage.h"   /* KiTTY: how far a connection got */
#include "kitty/kitty_winevent.h"    /* KiTTY: the Windows Event Log records */

/* KiTTY: the records of this program's own connection only (a proxy or
 * jump host's runs on a Seat of its own); a TempSeat stands for the real
 * one, as in the reconnect hook (windows/window.c). */
int kitty_wev_own_seat(Seat *seat)
{
    if (!kitty_wev_enabled())
        return 0;
    if (seat && is_tempseat(seat))
        seat = tempseat_get_real(seat);
    return kitty_wev_is_main(seat);
}

void backend_socket_log(Seat *seat, LogContext *logctx, Socket *sock,
                        PlugLogType type, SockAddr *addr, int port,
                        const char *error_msg, int error_code, Conf *conf,
                        bool session_started)
{
    char addrbuf[256], *msg;

    switch (type) {
      case PLUGLOG_CONNECT_TRYING:
        sk_getaddr(addr, addrbuf, lenof(addrbuf));
        if (sk_addr_needs_port(addr)) {
            msg = dupprintf("Connecting to %s port %d", addrbuf, port);
        } else {
            msg = dupprintf("Connecting to %s", addrbuf);
        }
        break;
      case PLUGLOG_CONNECT_FAILED:
        sk_getaddr(addr, addrbuf, lenof(addrbuf));
        msg = dupprintf("Failed to connect to %s: %s", addrbuf, error_msg);
        break;
      case PLUGLOG_CONNECT_SUCCESS: {
        char *from = NULL;                 /* KiTTY: for event 200/300 */
        if (addr)
            sk_getaddr(addr, addrbuf, lenof(addrbuf));
        else /* fallback if address unavailable */
            sprintf(addrbuf, "remote host");
        msg = dupprintf("Connected to %s", addrbuf);
        /* KiTTY: the attempt reached the server (hknet/KiTTY#66) */
        kitty_connstage(seat, KCS_TCP_UP, PTRLEN_LITERAL(""));
        if (sock) {
            SocketEndpointInfo *local_end = sk_endpoint_info(sock, false);
            if (local_end) {
                char *newmsg = dupprintf("%s (from %s)", msg,
                                         local_end->log_text);
                sfree(msg);
                msg = newmsg;
                from = dupstr(local_end->log_text);
                sk_free_endpoint_info(local_end);
            }
        }
        /* KiTTY: event 200/300 (kitty_winevent.h) */
        if (kitty_wev_own_seat(seat))
            kitty_wev_session_open(
                (seat && is_tempseat(seat)) ? tempseat_get_real(seat) : seat,
                addrbuf, from ? from : "");
        sfree(from);
        break;
      }
      case PLUGLOG_PROXY_MSG: {
        /* Proxy-related log messages have their own identifying
         * prefix already, put on by our caller. */
        int len, log_to_term;

        /* Suffix \r\n temporarily, so we can log to the terminal. */
        msg = dupprintf("%s\r\n", error_msg);
        len = strlen(msg);
        assert(len >= 2);

        log_to_term = conf_get_int(conf, CONF_proxy_log_to_term);
        if (log_to_term == AUTO)
            log_to_term = session_started ? FORCE_OFF : FORCE_ON;
        if (log_to_term == FORCE_ON)
            seat_stderr(seat, msg, len);

        msg[len-2] = '\0';         /* remove the \r\n again */
        break;
      }
      default:
        msg = NULL;  /* shouldn't happen, but placate optimiser */
        break;
    }

    if (msg) {
        logevent(logctx, msg);
        sfree(msg);
    }
}
