/*
 * "Adb" backend - Android Debug Bridge connection type (KiTTY MOD_ADB).
 *
 * Ported to PuTTY 0.84's BackendVtable from KiTTY's adb.c (a 0.71-era backend).
 * The 0.84 backend ABI drifted substantially since then; this port models the
 * vtable/Plug/Interactor wiring on otherbackends/raw.c, while preserving the
 * adb-server handshake state machine (the actual ADB protocol logic) verbatim.
 *
 * ADB protocol summary (talking to the local `adb server` on tcp:5037):
 *   1. Send "host:transport-*" (or "host:transport:<serial>") -> server replies
 *      OKAY / FAIL<len><msg>.
 *   2. Send "shell:" -> OKAY / FAIL, then the connection becomes a raw shell.
 */
#ifdef MOD_ADB

#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>

#include "putty.h"

#define ADB_MAX_BACKLOG 4096

typedef enum {
    STATE_WARMING_UP,
    STATE_SENT_HELLO,
    STATE_ASKED_FOR_SHELL,
    STATE_CONNECTED,
    STATE_WAITING_FOR_ERROR_MESSAGE,
} adb_state;

typedef struct Adb Adb;
struct Adb {
    Socket *s;
    bool closed_on_socket_error;
    size_t bufsize;
    Seat *seat;
    LogContext *logctx;
    bool socket_connected;
    char *description;

    adb_state state;
    Conf *conf;

    Plug plug;
    Backend backend;
    Interactor interactor;
};

static void c_write(Adb *adb, const void *buf, size_t len)
{
    size_t backlog = seat_stdout(adb->seat, buf, len);
    sk_set_frozen(adb->s, backlog > ADB_MAX_BACKLOG);
}

static void adb_log(Plug *plug, Socket *s, PlugLogType type, SockAddr *addr,
                    int port, const char *error_msg, int error_code)
{
    Adb *adb = container_of(plug, Adb, plug);
    backend_socket_log(adb->seat, adb->logctx, s, type, addr, port,
                       error_msg, error_code, adb->conf,
                       adb->socket_connected);
    if (type == PLUGLOG_CONNECT_SUCCESS) {
        adb->socket_connected = true;
        seat_set_trust_status(adb->seat, false);
    }
}

static void adb_closing(Plug *plug, PlugCloseType type, const char *error_msg)
{
    Adb *adb = container_of(plug, Adb, plug);

    if (adb->s) {
        sk_close(adb->s);
        adb->s = NULL;
        if (type != PLUGCLOSE_NORMAL)
            adb->closed_on_socket_error = true;
        seat_notify_remote_exit(adb->seat);
        seat_notify_remote_disconnect(adb->seat);
    }
    if (type != PLUGCLOSE_NORMAL) {
        /* A socket error has occurred. */
        logevent(adb->logctx, error_msg);
        if (type != PLUGCLOSE_USER_ABORT)
            seat_connection_fatal(adb->seat, "%s", error_msg);
    } /* Otherwise, the remote side closed the connection normally. */
}

static void do_fatal(Adb *adb, const char *data, int len)
{
    char *d = snewn(len + 1, char);
    memcpy(d, data, len);
    d[len] = '\0';
    seat_connection_fatal(adb->seat, "adb failure message: '%s'", d);
    sfree(d);
}

/* the error might not be available when the error occurs; wait a bit for more
 * data to show up then assume that's the error message. */
static void handle_fail(Adb *adb, const char *data, int len)
{
    /* FAIL<4-hex-len><message> */
    char message_length_hex[5];
    unsigned long expected;
    if (len < 8)
        return;
    memcpy(message_length_hex, data + 4, 4);
    message_length_hex[4] = 0;
    expected = strtoul(message_length_hex, NULL, 16);

    if ((unsigned long)len == expected + 8)
        do_fatal(adb, data + 8, expected);
    else
        adb->state = STATE_WAITING_FOR_ERROR_MESSAGE;
}

static void adb_receive(Plug *plug, int urgent, const char *data, size_t len)
{
    Adb *adb = container_of(plug, Adb, plug);
    if (len == 0)
        return;
    if (adb->state == STATE_SENT_HELLO) {
        if (data[0] == 'O') {              /* OKAY */
            sk_write(adb->s, "0006shell:", 10);
            adb->state = STATE_ASKED_FOR_SHELL;
        } else if (data[0] == 'F') {
            handle_fail(adb, data, len);
        } else {
            seat_connection_fatal(adb->seat, "%s",
                                  "Bad response after initial send");
        }
    } else if (adb->state == STATE_ASKED_FOR_SHELL) {
        if (data[0] == 'O') {              /* OKAY */
            adb->state = STATE_CONNECTED;  /* shell started */
        } else if (data[0] == 'F') {
            handle_fail(adb, data, len);
        } else {
            seat_connection_fatal(adb->seat, "%s",
                                  "Bad response waiting for shell start");
        }
    } else if (adb->state == STATE_WAITING_FOR_ERROR_MESSAGE) {
        do_fatal(adb, data, len);
    } else {
        c_write(adb, data, len);
    }
}

static void adb_sent(Plug *plug, size_t bufsize)
{
    Adb *adb = container_of(plug, Adb, plug);
    adb->bufsize = bufsize;
    seat_sent(adb->seat, adb->bufsize);
}

static const PlugVtable Adb_plugvt = {
    .log = adb_log,
    .closing = adb_closing,
    .receive = adb_receive,
    .sent = adb_sent,
};

static char *adb_description(Interactor *itr)
{
    Adb *adb = container_of(itr, Adb, interactor);
    return dupstr(adb->description);
}

static LogPolicy *adb_logpolicy(Interactor *itr)
{
    Adb *adb = container_of(itr, Adb, interactor);
    return log_get_policy(adb->logctx);
}

static Seat *adb_get_seat(Interactor *itr)
{
    Adb *adb = container_of(itr, Adb, interactor);
    return adb->seat;
}

static void adb_set_seat(Interactor *itr, Seat *seat)
{
    Adb *adb = container_of(itr, Adb, interactor);
    adb->seat = seat;
}

static const InteractorVtable Adb_interactorvt = {
    .description = adb_description,
    .logpolicy = adb_logpolicy,
    .get_seat = adb_get_seat,
    .set_seat = adb_set_seat,
};

/*
 * Called to set up the adb connection.  Returns an error message (allocated),
 * or NULL on success.  Always connects to the local adb server (port 5037),
 * regardless of the host typed - host selects the target device.
 */
static char *adb_init(const BackendVtable *vt, Seat *seat,
                      Backend **backend_handle, LogContext *logctx,
                      Conf *conf, const char *host, int port,
                      char **realhost, bool nodelay, bool keepalive)
{
    SockAddr *addr;
    const char *err;
    Adb *adb;
    int addressfamily;

    adb = snew(Adb);
    memset(adb, 0, sizeof(Adb));
    adb->plug.vt = &Adb_plugvt;
    adb->backend.vt = vt;
    adb->interactor.vt = &Adb_interactorvt;
    adb->backend.interactor = &adb->interactor;
    adb->s = NULL;
    adb->closed_on_socket_error = false;
    *backend_handle = &adb->backend;
    adb->bufsize = 0;
    adb->socket_connected = false;
    adb->conf = conf_copy(conf);
    adb->state = STATE_WARMING_UP;
    adb->seat = seat;
    adb->logctx = logctx;
    adb->description = default_description(vt, "localhost", 5037);

    addressfamily = conf_get_int(conf, CONF_addressfamily);

    /* The adb server always lives on the local machine. */
    if (port < 0 || port == 0)
        port = 5037;                       /* default adb server port */

    addr = name_lookup("localhost", port, realhost, conf, addressfamily,
                       adb->logctx, "ADB connection");
    if ((err = sk_addr_error(addr)) != NULL) {
        sk_addr_free(addr);
        return dupstr(err);
    }

    /*
     * Open socket to the adb server.
     */
    adb->s = new_main_connection(
        addr, *realhost, port, false, true, nodelay, keepalive, &adb->plug,
        conf, &adb->interactor, adb->logctx);
    if ((err = sk_socket_error(adb->s)) != NULL)
        return dupstr(err);

    /* Send the device-selection ("transport") request to the adb server. */
#define ADB_SHELL_DEFAULT_STR "0012" "host:transport-any"
#define ADB_SHELL_DEFAULT_STR_LEN (sizeof(ADB_SHELL_DEFAULT_STR)-1)
#define ADB_SHELL_USB_STR "0012" "host:transport-usb"
#define ADB_SHELL_USB_STR_LEN (sizeof(ADB_SHELL_USB_STR)-1)
#define ADB_SHELL_LOCAL_STR "0015" "host:transport-local"
#define ADB_SHELL_LOCAL_STR_LEN (sizeof(ADB_SHELL_LOCAL_STR)-1)
#define ADB_SHELL_SERIAL_PREFIX "host:transport:"
#define ADB_SHELL_SERIAL_PREFIX_LEN (sizeof(ADB_SHELL_SERIAL_PREFIX)-1)

#define write_hello(str, len) do {              \
        sk_write(adb->s, str, len);             \
        adb->state = STATE_SENT_HELLO;          \
    } while (0)

    {
        size_t len;
        if (host[0] == ':')
            ++host;
        len = strlen(host);

        if (len == 0 || !strcmp("-a", host) || !strcmp(host, "transport-any")) {
            write_hello(ADB_SHELL_DEFAULT_STR, ADB_SHELL_DEFAULT_STR_LEN);
        } else if (!strcmp("-d", host) || !strcmp(host, "transport-usb")) {
            write_hello(ADB_SHELL_USB_STR, ADB_SHELL_USB_STR_LEN);
        } else if (!strcmp("-e", host) || !strcmp(host, "transport-local")) {
            write_hello(ADB_SHELL_LOCAL_STR, ADB_SHELL_LOCAL_STR_LEN);
        } else {
            char sendbuf[512];
#define ADB_SHELL_HOST_MAX_LEN (sizeof(sendbuf)-4-ADB_SHELL_SERIAL_PREFIX_LEN)
            if (len > ADB_SHELL_HOST_MAX_LEN)
                len = ADB_SHELL_HOST_MAX_LEN;
            sprintf(sendbuf, "%04lx" ADB_SHELL_SERIAL_PREFIX,
                    (unsigned long)(len + ADB_SHELL_SERIAL_PREFIX_LEN));
            memcpy(sendbuf + 4 + ADB_SHELL_SERIAL_PREFIX_LEN, host, len);
            write_hello(sendbuf, len + 4 + ADB_SHELL_SERIAL_PREFIX_LEN);
        }
    }
    return NULL;
}

static void adb_free(Backend *be)
{
    Adb *adb = container_of(be, Adb, backend);
    if (is_tempseat(adb->seat))
        tempseat_free(adb->seat);
    if (adb->s)
        sk_close(adb->s);
    conf_free(adb->conf);
    sfree(adb->description);
    sfree(adb);
}

static void adb_reconfig(Backend *be, Conf *conf)
{
}

static void adb_send(Backend *be, const char *buf, size_t len)
{
    Adb *adb = container_of(be, Adb, backend);
    if (adb->s == NULL)
        return;
    adb->bufsize = sk_write(adb->s, buf, len);
}

static size_t adb_sendbuffer(Backend *be)
{
    Adb *adb = container_of(be, Adb, backend);
    return adb->bufsize;
}

static void adb_size(Backend *be, int width, int height)
{
    /* Do nothing! */
}

static void adb_special(Backend *be, SessionSpecialCode code, int arg)
{
    /* Do nothing! */
}

static const SessionSpecial *adb_get_specials(Backend *be)
{
    return NULL;
}

static bool adb_connected(Backend *be)
{
    Adb *adb = container_of(be, Adb, backend);
    return adb->s != NULL;
}

static bool adb_sendok(Backend *be)
{
    Adb *adb = container_of(be, Adb, backend);
    return adb->socket_connected;
}

static void adb_unthrottle(Backend *be, size_t backlog)
{
    Adb *adb = container_of(be, Adb, backend);
    sk_set_frozen(adb->s, backlog > ADB_MAX_BACKLOG);
}

static bool adb_ldisc(Backend *be, int option)
{
    /* Don't allow line discipline options (raw shell). */
    return false;
}

static void adb_provide_ldisc(Backend *be, Ldisc *ldisc)
{
    /* This is a stub. */
}

static int adb_exitcode(Backend *be)
{
    Adb *adb = container_of(be, Adb, backend);
    if (adb->s != NULL)
        return -1;                         /* still connected */
    else if (adb->closed_on_socket_error)
        return INT_MAX;
    else
        return 0;                          /* meaningless concept for ADB */
}

static int adb_cfg_info(Backend *be)
{
    return 0;
}

const BackendVtable adb_backend = {
    .init = adb_init,
    .free = adb_free,
    .reconfig = adb_reconfig,
    .send = adb_send,
    .sendbuffer = adb_sendbuffer,
    .size = adb_size,
    .special = adb_special,
    .get_specials = adb_get_specials,
    .connected = adb_connected,
    .exitcode = adb_exitcode,
    .sendok = adb_sendok,
    .ldisc_option_state = adb_ldisc,
    .provide_ldisc = adb_provide_ldisc,
    .unthrottle = adb_unthrottle,
    .cfg_info = adb_cfg_info,
    .id = "adb",
    .displayname_tc = "ADB",
    .displayname_lc = "adb",
    .protocol = PROT_ADB,
    .default_port = 5037,
};

#endif /* MOD_ADB */
