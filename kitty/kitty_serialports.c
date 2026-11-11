/*
 * kitty_serialports.c - the serial ports of this PC, the configuration box's
 * serial line and speed drop-downs, quick connect to a serial line
 * (hknet/KiTTY#86). See kitty_serialports.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "putty.h"
#include "dialog.h"
#include <setupapi.h>        /* the types only: the functions are loaded */

#include "kitty_serialports.h"
#include "kitty_params.h"        /* ReadParameterN, WriteParameter, GetQuickConnectMode */
#include "kitty_inikeys.h"       /* KI_SECTION_CONFIGBOX, KI_CONFIGBOX_QUICKSERIALSPEED */
#include "kitty_oldwin.h"        /* kitty_api */
#include "kitty_text.h"

const int kitty_serial_speeds[KITTY_SERIAL_NSPEEDS] = {
    300, 1200, 2400, 4800, 9600, 19200, 38400, 57600, 74880, 115200,
    230400, 250000, 500000, 1000000, 2000000
};

/* ---------------------------------------------------------- the port list */

/* GUID_DEVINTERFACE_COMPORT, written out: the import library that defines
 * it is not linked. */
static const GUID kitty_guid_comport = {
    0x86E0D1E0, 0x8089, 0x11D0,
    { 0x9C, 0xE4, 0x08, 0x00, 0x3E, 0x30, 0x1F, 0x73 } };

typedef HDEVINFO (WINAPI *fn_GetClassDevsA)(const GUID *, PCSTR, HWND, DWORD);
typedef BOOL (WINAPI *fn_EnumDeviceInfo)(HDEVINFO, DWORD, PSP_DEVINFO_DATA);
typedef BOOL (WINAPI *fn_GetDeviceRegistryPropertyA)(
    HDEVINFO, PSP_DEVINFO_DATA, DWORD, PDWORD, PBYTE, DWORD, PDWORD);
typedef HKEY (WINAPI *fn_OpenDevRegKey)(HDEVINFO, PSP_DEVINFO_DATA, DWORD,
                                        DWORD, DWORD, REGSAM);
typedef BOOL (WINAPI *fn_DestroyDeviceInfoList)(HDEVINFO);

/* COM5 before COM10: a run of digits compares as a number. */
static int kitty_serial_name_cmp(const char *a, const char *b)
{
    while (*a && *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            unsigned long x = strtoul(a, (char **)&a, 10);
            unsigned long y = strtoul(b, (char **)&b, 10);
            if (x != y)
                return x < y ? -1 : 1;
            continue;
        }
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b))
            return toupper((unsigned char)*a) - toupper((unsigned char)*b);
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static int kitty_serial_port_cmp(const void *av, const void *bv)
{
    const struct kitty_serial_port *a = av, *b = bv;
    return kitty_serial_name_cmp(a->name, b->name);
}

static int kitty_serial_find(struct kitty_serial_port *p, int n,
                             const char *name)
{
    for (int i = 0; i < n; i++)
        if (!stricmp(p[i].name, name))
            return i;
    return -1;
}

static void kitty_serial_add(struct kitty_serial_port **p, int *n,
                             size_t *size, const char *name)
{
    if (!name || !*name || kitty_serial_find(*p, *n, name) >= 0)
        return;
    sgrowarray(*p, *size, *n);
    (*p)[*n].name = dupstr(name);
    (*p)[*n].label = NULL;
    (*n)++;
}

/* The device names Windows has mapped now: one value per port under
 * HKLM\HARDWARE\DEVICEMAP\SERIALCOMM, its data the device name. Every
 * Windows from XP on has the key. It is missing when no port exists. */
static void kitty_serial_from_devicemap(struct kitty_serial_port **p, int *n,
                                        size_t *size)
{
    HKEY key;
    DWORD maxname = 0, maxdata = 0;
    char *vname, *data;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\SERIALCOMM",
                      0, KEY_READ, &key) != ERROR_SUCCESS)
        return;
    if (RegQueryInfoKeyA(key, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                         &maxname, &maxdata, NULL, NULL) != ERROR_SUCCESS) {
        RegCloseKey(key);
        return;
    }
    /* sized from the key: the longest value name and data it holds */
    vname = snewn(maxname + 1, char);
    data = snewn(maxdata + 1, char);
    for (DWORD i = 0; ; i++) {
        DWORD vlen = maxname + 1, dlen = maxdata, type;
        LONG r = RegEnumValueA(key, i, vname, &vlen, NULL, &type,
                               (BYTE *)data, &dlen);
        if (r == ERROR_NO_MORE_ITEMS)
            break;
        if (r != ERROR_SUCCESS || type != REG_SZ)
            continue;
        data[dlen < maxdata ? dlen : maxdata] = '\0';
        kitty_serial_add(p, n, size, data);
    }
    sfree(vname);
    sfree(data);
    RegCloseKey(key);
}

/* The descriptions, plus any port the registry above did not have: every
 * present device of the COM port interface, its "PortName" and its friendly
 * name ("USB-SERIAL CH340 (COM5)" - the "(COM5)" is dropped, the device
 * name stands in front already). SetupAPI is loaded at run time: a Windows
 * without it still starts and lists the bare device names. */
static void kitty_serial_from_setupapi(struct kitty_serial_port **p, int *n,
                                       size_t *size)
{
    fn_GetClassDevsA getdevs = (fn_GetClassDevsA)(void *)kitty_api(
        "setupapi.dll", "SetupDiGetClassDevsA", KITTY_API_OPTIONAL,
        KT_WINFEAT_SERIAL_NAMES);
    fn_EnumDeviceInfo enumdev = (fn_EnumDeviceInfo)(void *)kitty_api(
        "setupapi.dll", "SetupDiEnumDeviceInfo", KITTY_API_OPTIONAL,
        KT_WINFEAT_SERIAL_NAMES);
    fn_GetDeviceRegistryPropertyA getprop =
        (fn_GetDeviceRegistryPropertyA)(void *)kitty_api(
            "setupapi.dll", "SetupDiGetDeviceRegistryPropertyA",
            KITTY_API_OPTIONAL, KT_WINFEAT_SERIAL_NAMES);
    fn_OpenDevRegKey openkey = (fn_OpenDevRegKey)(void *)kitty_api(
        "setupapi.dll", "SetupDiOpenDevRegKey", KITTY_API_OPTIONAL,
        KT_WINFEAT_SERIAL_NAMES);
    fn_DestroyDeviceInfoList destroy = (fn_DestroyDeviceInfoList)(void *)kitty_api(
        "setupapi.dll", "SetupDiDestroyDeviceInfoList", KITTY_API_OPTIONAL,
        KT_WINFEAT_SERIAL_NAMES);
    HDEVINFO devs;

    if (!getdevs || !enumdev || !getprop || !openkey || !destroy)
        return;
    devs = getdevs(&kitty_guid_comport, NULL, NULL,
                   DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devs == INVALID_HANDLE_VALUE)
        return;
    for (DWORD i = 0; ; i++) {
        SP_DEVINFO_DATA dev;
        char *name = NULL, *friendly = NULL;
        DWORD len = 0, type;
        HKEY key;
        int at;

        dev.cbSize = sizeof(dev);
        if (!enumdev(devs, i, &dev))
            break;
        key = openkey(devs, &dev, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (key == INVALID_HANDLE_VALUE || !key)
            continue;
        /* the value's size first, then the value */
        if (RegQueryValueExA(key, "PortName", NULL, &type, NULL,
                             &len) == ERROR_SUCCESS && type == REG_SZ) {
            name = snewn(len + 1, char);
            if (RegQueryValueExA(key, "PortName", NULL, &type, (BYTE *)name,
                                 &len) == ERROR_SUCCESS)
                name[len] = '\0';
            else {
                sfree(name);
                name = NULL;
            }
        }
        RegCloseKey(key);
        if (!name)
            continue;
        kitty_serial_add(p, n, size, name);
        at = kitty_serial_find(*p, *n, name);
        len = 0;
        /* the friendly name's size: the call fails with the size needed */
        getprop(devs, &dev, SPDRP_FRIENDLYNAME, NULL, NULL, 0, &len);
        if (at >= 0 && !(*p)[at].label && len > 0) {
            friendly = snewn(len + 1, char);
            if (getprop(devs, &dev, SPDRP_FRIENDLYNAME, NULL,
                        (PBYTE)friendly, len, NULL)) {
                char *paren, *tail;
                friendly[len] = '\0';
                tail = dupprintf("(%s)", name);
                paren = strstr(friendly, tail);
                if (paren) {
                    *paren = '\0';
                    while (paren > friendly && paren[-1] == ' ')
                        *--paren = '\0';
                }
                sfree(tail);
                if (friendly[0])
                    (*p)[at].label = dupprintf(KT_SERIAL_PORT_LABEL_FMT,
                                               name, friendly);
            }
            sfree(friendly);
        }
        sfree(name);
    }
    destroy(devs);
}

struct kitty_serial_port *kitty_serial_ports(int *n)
{
    struct kitty_serial_port *p = NULL;
    size_t size = 0;
    *n = 0;
    kitty_serial_from_devicemap(&p, n, &size);
    kitty_serial_from_setupapi(&p, n, &size);
    for (int i = 0; i < *n; i++)
        if (!p[i].label)
            p[i].label = dupstr(p[i].name);
    if (*n > 1)
        qsort(p, *n, sizeof(*p), kitty_serial_port_cmp);
    return p;
}

void kitty_serial_ports_free(struct kitty_serial_port *p, int n)
{
    for (int i = 0; i < n; i++) {
        sfree(p[i].name);
        sfree(p[i].label);
    }
    sfree(p);
}

char *kitty_serial_port_from_text(const char *text)
{
    const char *a = text ? text : "", *b;
    while (*a == ' ' || *a == '\t')
        a++;
    for (b = a; *b && *b != ' ' && *b != '\t'; b++)
        ;
    return dupprintf("%.*s", (int)(b - a), a);
}

/* ------------------------------------------------ the configuration box */

void kitty_serline_handler(dlgcontrol *ctrl, dlgparam *dlg, void *data,
                           int event)
{
    Conf *conf = (Conf *)data;
    /* the Session panel's pair carries its struct hostport, the
     * Connection > Serial panel nothing */
    bool session_panel = ctrl->context.p != NULL;

    if (event == EVENT_REFRESH) {
        /* on the Session panel only while Serial is selected, in place of
         * the host name box */
        if (session_panel)
            dlg_ctrl_show(ctrl, dlg,
                          conf_get_int(conf, CONF_protocol) == PROT_SERIAL);
        dlg_editbox_set(ctrl, dlg, conf_get_str(conf, CONF_serline));
    } else if (event == EVENT_VALCHANGE) {
        /* a list entry carries the description after the name: only the
         * name is the setting */
        char *s = dlg_editbox_get(ctrl, dlg);
        char *line = kitty_serial_port_from_text(s);
        conf_set_str(conf, CONF_serline, line);
        sfree(line);
        sfree(s);
    }
}

void kitty_serspeed_handler(dlgcontrol *ctrl, dlgparam *dlg, void *data,
                            int event)
{
    Conf *conf = (Conf *)data;
    bool session_panel = ctrl->context.p != NULL;

    if (event == EVENT_REFRESH) {
        char buf[32];
        if (session_panel)
            dlg_ctrl_show(ctrl, dlg,
                          conf_get_int(conf, CONF_protocol) == PROT_SERIAL);
        snprintf(buf, sizeof(buf), "%d", conf_get_int(conf, CONF_serspeed));
        dlg_editbox_set(ctrl, dlg, buf);
    } else if (event == EVENT_VALCHANGE) {
        /* an empty or half-typed field changes nothing: the speed in force
         * comes back when the field is left */
        char *s = dlg_editbox_get(ctrl, dlg);
        int speed = atoi(s);
        if (speed > 0)
            conf_set_int(conf, CONF_serspeed, speed);
        sfree(s);
    }
}

/* The list is built when it opens: a USB adapter plugged in while the box
 * is open is there the next time. What is typed in the field stays. */
void kitty_serline_dropdown(dlgcontrol *ctrl, dlgparam *dlg)
{
    char *typed = dlg_editbox_get(ctrl, dlg);
    int n;
    struct kitty_serial_port *p = kitty_serial_ports(&n);
    dlg_update_start(ctrl, dlg);
    dlg_listbox_clear(ctrl, dlg);
    for (int i = 0; i < n; i++)
        dlg_listbox_add(ctrl, dlg, p[i].label);
    dlg_update_done(ctrl, dlg);
    dlg_editbox_set(ctrl, dlg, typed);
    kitty_serial_ports_free(p, n);
    sfree(typed);
}

void kitty_serspeed_dropdown(dlgcontrol *ctrl, dlgparam *dlg)
{
    char *typed = dlg_editbox_get(ctrl, dlg);
    dlg_update_start(ctrl, dlg);
    dlg_listbox_clear(ctrl, dlg);
    for (int i = 0; i < KITTY_SERIAL_NSPEEDS; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", kitty_serial_speeds[i]);
        dlg_listbox_add(ctrl, dlg, buf);
    }
    dlg_update_done(ctrl, dlg);
    dlg_editbox_set(ctrl, dlg, typed);
    sfree(typed);
}

/* ------------------------------------------- quick connect, serial line */

int kitty_serial_quick_speed(void)
{
    char buf[32];
    int speed = 0;
    if (ReadParameterN(KI_SECTION_CONFIGBOX, KI_CONFIGBOX_QUICKSERIALSPEED,
                       buf, sizeof(buf)))
        speed = atoi(buf);
    return speed > 0 ? speed : KITTY_SERIAL_QUICK_SPEED_DEFAULT;
}

void kitty_serial_quick_prefill(Conf *conf)
{
    int n;
    struct kitty_serial_port *p;
    if (!conf || !GetQuickConnectMode())
        return;
    p = kitty_serial_ports(&n);
    if (n > 0)
        conf_set_str(conf, CONF_serline, p[0].name);
    kitty_serial_ports_free(p, n);
    conf_set_int(conf, CONF_serspeed, kitty_serial_quick_speed());
}

void kitty_serial_quick_setup(Conf *conf)
{
    if (!conf)
        return;
    conf_set_int(conf, CONF_protocol, PROT_SERIAL);
    kitty_serial_quick_prefill(conf);
}

void kitty_serial_quick_remember(Conf *conf)
{
    char buf[32];
    int speed;
    if (!conf || !GetQuickConnectMode() ||
        conf_get_int(conf, CONF_protocol) != PROT_SERIAL)
        return;
    speed = conf_get_int(conf, CONF_serspeed);
    if (speed <= 0 || speed == kitty_serial_quick_speed())
        return;
    snprintf(buf, sizeof(buf), "%d", speed);
    WriteParameter(KI_SECTION_CONFIGBOX, KI_CONFIGBOX_QUICKSERIALSPEED, buf);
}
