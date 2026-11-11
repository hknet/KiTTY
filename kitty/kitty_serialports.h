/*
 * kitty_serialports.h - the serial ports of this PC and the speeds offered
 * for them (hknet/KiTTY#86): the port list, the configuration box's serial
 * line and speed drop-downs, quick connect to a serial line.
 *
 * The terminal window's Serial submenu and Find speed are in
 * kitty_serialmenu.c. The hooks the serial back end offers them are at the
 * end of this file.
 */
#ifndef KITTY_SERIALPORTS_H
#define KITTY_SERIALPORTS_H

#include "putty.h"   /* Conf, Backend, dlgcontrol, dlgparam */

/* The speeds of the Serial submenu and the speed drop-downs, ascending. */
#define KITTY_SERIAL_NSPEEDS 15
extern const int kitty_serial_speeds[KITTY_SERIAL_NSPEEDS];

/* The speed of a first quick connect to a serial line. */
#define KITTY_SERIAL_QUICK_SPEED_DEFAULT 9600

/* A port of this PC: its device name ("COM5") and the text the lists show
 * ("COM5 - USB-SERIAL CH340", or the bare name when Windows has no
 * description for it). */
struct kitty_serial_port {
    char *name;
    char *label;
};

/* The ports present now, sorted by name with the number compared as a
 * number (COM2 before COM10): an snewn'd array, *n entries, NULL when
 * none. The device names come from HKLM\HARDWARE\DEVICEMAP\SERIALCOMM,
 * the descriptions from SetupAPI (the COM port device interface), loaded
 * at run time. A port SetupAPI lists beyond the registry's is added. Free
 * with kitty_serial_ports_free. */
struct kitty_serial_port *kitty_serial_ports(int *n);
void kitty_serial_ports_free(struct kitty_serial_port *p, int n);

/* The device part of a list text: everything up to the first blank
 * ("COM5 - USB-SERIAL CH340" -> "COM5", "\\.\CNCA0" stays), snewn'd. */
char *kitty_serial_port_from_text(const char *text);

/* The configuration box. Context P(struct hostport *): the pair on the
 * Session panel, shown only while Serial is selected. Context P(NULL): the
 * Connection > Serial panel, always shown. */
void kitty_serline_handler(dlgcontrol *ctrl, dlgparam *dlg, void *data, int event);
void kitty_serspeed_handler(dlgcontrol *ctrl, dlgparam *dlg, void *data, int event);
void kitty_serline_dropdown(dlgcontrol *ctrl, dlgparam *dlg);
void kitty_serspeed_dropdown(dlgcontrol *ctrl, dlgparam *dlg);

/* Quick connect to a serial line. [ConfigBox] quickserialspeed is the speed
 * of the last one (KITTY_SERIAL_QUICK_SPEED_DEFAULT before the first). */
int kitty_serial_quick_speed(void);
/* In quick connect: the first port detected and the remembered speed. Does
 * nothing outside quick connect. */
void kitty_serial_quick_prefill(Conf *conf);
/* kitty.exe -quickconnectserial: Serial selected, then the prefill. */
void kitty_serial_quick_setup(Conf *conf);
/* A quick connect to a serial line was opened: remember its speed. Does
 * nothing for any other connection. */
void kitty_serial_quick_remember(Conf *conf);

/*
 * The hooks of the serial back end (windows/serial.c, MOD_PERSO).
 *
 * A data tap takes what the port receives instead of the terminal while it
 * is set (Find speed listens through it). It is called with data NULL when
 * the port is closed under it. serial_backend_tap returns false when the
 * back end is not the serial one.
 */
typedef void (*serial_tap_fn)(void *ctx, const void *data, size_t len);
bool serial_backend_tap(Backend *be, serial_tap_fn fn, void *ctx);
/* The open port, or INVALID_HANDLE_VALUE (not serial, or closed). */
HANDLE serial_backend_port(Backend *be);
/* The settings in force on the port (the last ones SetCommState took): the
 * speed, data bits, stop bits (in halves, as CONF_serstopbits), parity and
 * flow control, as Conf stores them. False when not serial or never set. */
bool serial_backend_applied(Backend *be, int *speed, int *databits,
                            int *stopbits, int *parity, int *flow);

#endif
