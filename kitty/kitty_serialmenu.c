/*
 * kitty_serialmenu.c - the terminal window's Serial submenu and Find speed
 * (hknet/KiTTY#86). See kitty_serialmenu.h.
 *
 * Ids: IDM_SERIAL_MIN + 0x10 * slot (windows/kitty_rc_additions.h). One
 * range covers every entry of the submenu, so no entry needs a shortcut
 * action of its own (tools/check-menu-ids.py, tools/check-menu-shortcuts.py).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "putty.h"
#include "kitty.h"               /* IDM_RECONF, force_reconf, the menu ids */
#include "kitty_text.h"
#include "kitty_serialports.h"
#include "kitty_serialmenu.h"

/* The slots of the range. */
#define KSM_SLOT_SPEED0  0                         /* the speeds of the list */
#define KSM_SLOT_CUSTOM  KITTY_SERIAL_NSPEEDS      /* the speed in force, not in the list */
#define KSM_SLOT_FIND    (KITTY_SERIAL_NSPEEDS + 1)
#define KSM_SLOT_PORT0   (KITTY_SERIAL_NSPEEDS + 2)
#define KSM_SLOT_SEP     63                        /* our own separator */
#define KSM_MAXPORTS     (KSM_SLOT_SEP - KSM_SLOT_PORT0)
#define KSM_ID(slot)     (IDM_SERIAL_MIN + 0x10 * (slot))

/* The submenu, one per window menu (the system menu and the context menu):
 * a submenu inserted into two menus would be destroyed twice. */
#define KSM_NMENUS 4
static HMENU ksm_menu[KSM_NMENUS];
static HMENU ksm_parent[KSM_NMENUS];
static bool ksm_sep[KSM_NMENUS];
/* The ports the submenu showed when it was opened last, by slot. */
static char *ksm_ports[KSM_MAXPORTS];
static int ksm_nports;

/* ---------------------------------------------------------- Find speed */

#define KFS_TICK      (TICKSPERSEC / 10)    /* the error poll, 100 ms */
#define KFS_TICKS     11                    /* 100 ms settling + 1 s listening */
#define KFS_MINBYTES  8                     /* fewer is no evidence */
#define KFS_READABLE  900                   /* per mille, the score of readable data */

static struct {
    bool active;
    Backend *be;
    HWND hwnd;
    Conf *conf;
    Terminal *term;
    LogContext *logctx;
    int orig;                               /* the speed before the scan */
    int pos;                                /* how far through kfs_order */
    int step, tick;                         /* step: the speed's index in the list */
    unsigned long next;
    long bytes[KITTY_SERIAL_NSPEEDS], good[KITTY_SERIAL_NSPEEDS];
    int errs[KITTY_SERIAL_NSPEEDS];
    bool refused[KITTY_SERIAL_NSPEEDS];
} kfs;

static void kfs_timer(void *ctx, unsigned long now);

/* The order of the scan: the two speeds most devices use first, then the
 * rest of the list. A speed with readable data and no error ends the scan
 * early (kfs_timer), so the usual case takes one or two seconds. */
static int kfs_order[KITTY_SERIAL_NSPEEDS];

static void kfs_make_order(void)
{
    static const int first[] = { 115200, 9600 };
    int n = 0;
    for (size_t f = 0; f < lenof(first); f++)
        for (int i = 0; i < KITTY_SERIAL_NSPEEDS; i++)
            if (kitty_serial_speeds[i] == first[f])
                kfs_order[n++] = i;
    for (int i = 0; i < KITTY_SERIAL_NSPEEDS; i++) {
        bool taken = false;
        for (int k = 0; k < n; k++)
            if (kfs_order[k] == i)
                taken = true;
        if (!taken)
            kfs_order[n++] = i;
    }
}

static void kfs_stop(void)
{
    if (kfs.be)
        serial_backend_tap(kfs.be, NULL, NULL);
    kfs.active = false;
    kfs.be = NULL;
    expire_timer_context(&kfs);
}

/* The data tap: what the port receives while the scan runs is scored here
 * and never reaches the terminal (windows/serial.c, serial_gotdata). */
static void kfs_tap(void *ctx, const void *data, size_t len)
{
    const unsigned char *p = data;
    (void)ctx;
    if (!kfs.active)
        return;
    if (!data) {
        /* the port was closed under the scan */
        kfs.be = NULL;              /* the back end has dropped the tap */
        kfs_stop();
        if (kfs.logctx)
            logevent(kfs.logctx, KT_SERIAL_FIND_CLOSED);
        return;
    }
    /* the first tick settles: bytes still on their way were sent at the
     * previous speed */
    if (kfs.tick == 0 || kfs.step < 0 || kfs.step >= KITTY_SERIAL_NSPEEDS)
        return;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = p[i];
        kfs.bytes[kfs.step]++;
        if ((c >= 0x20 && c < 0x7F) || c == '\r' || c == '\n' || c == '\t')
            kfs.good[kfs.step]++;
    }
}

/* The speed alone, set straight on the port: the full reconfiguration
 * writes five Event Log lines per call, fifteen times over. */
static bool kfs_set_speed(HANDLE port, int speed, char **err)
{
    DCB dcb;
    DWORD e;
    *err = NULL;
    if (!GetCommState(port, &dcb)) {
        *err = dupstr(win_strerror(GetLastError()));
        return false;
    }
    dcb.BaudRate = (DWORD)speed;
    if (!SetCommState(port, &dcb)) {
        *err = dupstr(win_strerror(GetLastError()));
        return false;
    }
    PurgeComm(port, PURGE_RXCLEAR);
    ClearCommError(port, &e, NULL);
    return true;
}

static void kfs_finish(void)
{
    int best = -1, score_best = -1, speed;
    long total = 0;
    const char *fmt;
    char *msg, *line, *err = NULL;
    HANDLE port = serial_backend_port(kfs.be);

    kfs_stop();
    for (int i = 0; i < KITTY_SERIAL_NSPEEDS; i++) {
        int score;
        total += kfs.bytes[i];
        if (kfs.refused[i] || kfs.bytes[i] < KFS_MINBYTES)
            continue;
        /* the share of printable and white-space bytes, per mille, less a
         * tenth for each poll that found a framing or parity error */
        score = (int)(kfs.good[i] * 1000 / kfs.bytes[i]) - 100 * kfs.errs[i];
        if (score > score_best) {
            score_best = score;
            best = i;
        }
    }
    if (total == 0) {
        speed = kfs.orig;
        fmt = KT_SERIAL_FIND_NO_DATA_FMT;
    } else if (best < 0 || score_best < KFS_READABLE) {
        speed = kfs.orig;
        fmt = KT_SERIAL_FIND_UNREADABLE_FMT;
    } else {
        speed = kitty_serial_speeds[best];
        fmt = KT_SERIAL_FIND_CHOSEN_FMT;
    }
    /* The chosen speed, or the old one back: on the port at once (the
     * reconfiguration below is dropped while a Change Settings box is
     * open), then through the Conf and the silent reconfiguration, which
     * applies the full settings again and records them as in force. */
    if (port != INVALID_HANDLE_VALUE && !kfs_set_speed(port, speed, &err))
        sfree(err);
    conf_set_int(kfs.conf, CONF_serspeed, speed);
    force_reconf = 0;
    PostMessage(kfs.hwnd, WM_COMMAND, IDM_RECONF, 0);

    msg = dupprintf(fmt, speed);
    line = dupprintf("\r\n%s\r\n", msg);
    logevent(kfs.logctx, msg);
    term_data(kfs.term, line, strlen(line));
    sfree(line);
    sfree(msg);
}

static void kfs_step(void)
{
    HANDLE port = serial_backend_port(kfs.be);
    char *err;

    kfs.pos++;
    if (kfs.pos >= KITTY_SERIAL_NSPEEDS) {
        kfs_finish();
        return;
    }
    kfs.step = kfs_order[kfs.pos];
    kfs.tick = 0;
    if (port == INVALID_HANDLE_VALUE) {
        kfs_stop();
        logevent(kfs.logctx, KT_SERIAL_FIND_CLOSED);
        return;
    }
    if (!kfs_set_speed(port, kitty_serial_speeds[kfs.step], &err)) {
        char *msg = dupprintf(KT_SERIAL_FIND_REFUSED_FMT,
                              kitty_serial_speeds[kfs.step], err);
        logevent(kfs.logctx, msg);
        sfree(msg);
        sfree(err);
        kfs.refused[kfs.step] = true;
        kfs.tick = KFS_TICKS - 1;       /* on to the next speed at once */
    }
    kfs.next = schedule_timer(KFS_TICK, kfs_timer, &kfs);
}

static void kfs_timer(void *ctx, unsigned long now)
{
    HANDLE port;
    DWORD e;
    (void)ctx;
    if (!kfs.active || now != kfs.next)
        return;
    port = serial_backend_port(kfs.be);
    /* ClearCommError reports the errors since its last call as flags, not
     * as a count: polled every tick, each poll that finds one counts */
    if (port != INVALID_HANDLE_VALUE && ClearCommError(port, &e, NULL) &&
        kfs.tick > 0 && !kfs.refused[kfs.step] &&
        (e & (CE_FRAME | CE_RXPARITY)))
        kfs.errs[kfs.step]++;
    kfs.tick++;
    if (kfs.tick >= KFS_TICKS) {
        if (!kfs.refused[kfs.step]) {
            long b = kfs.bytes[kfs.step];
            char *msg = dupprintf(KT_SERIAL_FIND_STEP_FMT,
                                  kitty_serial_speeds[kfs.step], (int)b,
                                  b ? (int)(kfs.good[kfs.step] * 100 / b) : 0,
                                  kfs.errs[kfs.step]);
            logevent(kfs.logctx, msg);
            sfree(msg);
            /* readable and free of errors: no need to try the rest */
            if (b >= KFS_MINBYTES && kfs.errs[kfs.step] == 0 &&
                kfs.good[kfs.step] * 1000 / b >= KFS_READABLE) {
                kfs_finish();
                return;
            }
        }
        kfs_step();
        return;
    }
    kfs.next = schedule_timer(KFS_TICK, kfs_timer, &kfs);
}

/* Find speed: listens only, sends nothing. Each speed of the list in turn,
 * on the session's timers - the window keeps running throughout. */
static void kfs_start(HWND hwnd, Conf *conf, Backend *be, Terminal *term,
                      LogContext *logctx)
{
    char *msg;
    if (kfs.active || serial_backend_port(be) == INVALID_HANDLE_VALUE)
        return;
    memset(&kfs, 0, sizeof(kfs));
    if (!serial_backend_tap(be, kfs_tap, &kfs))
        return;
    kfs.active = true;
    kfs.be = be;
    kfs.hwnd = hwnd;
    kfs.conf = conf;
    kfs.term = term;
    kfs.logctx = logctx;
    kfs.orig = conf_get_int(conf, CONF_serspeed);
    kfs.pos = -1;
    kfs.step = -1;
    kfs_make_order();
    msg = dupprintf(KT_SERIAL_FIND_START_FMT, KITTY_SERIAL_NSPEEDS);
    logevent(logctx, msg);
    sfree(msg);
    kfs_step();
}

/* ---------------------------------------------------------- the submenu */

/* "\\.\COM5" and "COM5" are one port. */
static const char *ksm_bare(const char *line)
{
    if (line && !strncmp(line, "\\\\.\\", 4))
        return line + 4;
    return line ? line : "";
}

/* A menu text shows '&' only doubled. */
static char *ksm_menu_text(const char *s)
{
    strbuf *sb = strbuf_new();
    for (; *s; s++) {
        if (*s == '&')
            put_byte(sb, '&');
        put_byte(sb, *s);
    }
    return strbuf_to_str(sb);
}

void kitty_serial_menu_update(HMENU *menus, int nmenus, Conf *conf,
                              UINT before, bool own_separator)
{
    bool want = conf && conf_get_int(conf, CONF_protocol) == PROT_SERIAL;
    if (nmenus > KSM_NMENUS)
        nmenus = KSM_NMENUS;
    for (int j = 0; j < nmenus; j++) {
        /* out first, wherever it was: the specials around it have just
         * been rebuilt */
        if (ksm_menu[j] && ksm_parent[j]) {
            RemoveMenu(ksm_parent[j], (UINT)(UINT_PTR)ksm_menu[j], MF_BYCOMMAND);
            if (ksm_sep[j])
                RemoveMenu(ksm_parent[j], KSM_ID(KSM_SLOT_SEP), MF_BYCOMMAND);
            ksm_parent[j] = NULL;
            ksm_sep[j] = false;
        }
        if (!want || !menus[j])
            continue;
        if (!ksm_menu[j])
            ksm_menu[j] = CreatePopupMenu();
        if (!ksm_menu[j])
            continue;
        InsertMenu(menus[j], before, MF_BYCOMMAND | MF_POPUP | MF_ENABLED,
                   (UINT_PTR)ksm_menu[j], KT_SYSMENU_SERIAL);
        if (own_separator) {
            InsertMenu(menus[j], before, MF_BYCOMMAND | MF_SEPARATOR,
                       KSM_ID(KSM_SLOT_SEP), NULL);
            ksm_sep[j] = true;
        }
        ksm_parent[j] = menus[j];
    }
}

bool kitty_serial_menu_popup(HMENU m, Conf *conf, Backend *be)
{
    int j, cur, nports, slot;
    struct kitty_serial_port *ports;
    const char *line;
    bool open, line_listed = false, custom_done = false;
    UINT busy;

    for (j = 0; j < KSM_NMENUS; j++)
        if (ksm_menu[j] && ksm_menu[j] == m)
            break;
    if (j == KSM_NMENUS || !conf)
        return false;

    /* DELETE FIRST, then add: the content is decided now */
    while (GetMenuItemCount(m) > 0)
        DeleteMenu(m, 0, MF_BYPOSITION);
    for (int i = 0; i < ksm_nports; i++)
        sfree(ksm_ports[i]);
    ksm_nports = 0;

    cur = conf_get_int(conf, CONF_serspeed);
    open = serial_backend_port(be) != INVALID_HANDLE_VALUE;
    busy = kfs.active ? (MF_DISABLED | MF_GRAYED) : MF_ENABLED;

    /* the speeds, the one in force ticked. One not in the list is shown in
     * its place in the order. */
    for (int i = 0; i < KITTY_SERIAL_NSPEEDS; i++) {
        char text[64];
        if (!custom_done && cur > 0 && cur < kitty_serial_speeds[i]) {
            bool listed = false;
            for (int k = 0; k < KITTY_SERIAL_NSPEEDS; k++)
                if (kitty_serial_speeds[k] == cur)
                    listed = true;
            if (!listed) {
                snprintf(text, sizeof(text), KT_SERIAL_MENU_SPEED_FMT, cur);
                AppendMenu(m, MF_STRING | busy | MF_CHECKED,
                           IDM_SERIAL_MIN + 0x10 * KSM_SLOT_CUSTOM, text);
            }
            custom_done = true;
        }
        snprintf(text, sizeof(text), KT_SERIAL_MENU_SPEED_FMT,
                 kitty_serial_speeds[i]);
        AppendMenu(m, MF_STRING | busy |
                   (kitty_serial_speeds[i] == cur ? MF_CHECKED : 0),
                   IDM_SERIAL_MIN + 0x10 * (KSM_SLOT_SPEED0 + i), text);
    }
    if (!custom_done && cur > kitty_serial_speeds[KITTY_SERIAL_NSPEEDS - 1]) {
        char text[64];
        snprintf(text, sizeof(text), KT_SERIAL_MENU_SPEED_FMT, cur);
        AppendMenu(m, MF_STRING | busy | MF_CHECKED,
                   IDM_SERIAL_MIN + 0x10 * KSM_SLOT_CUSTOM, text);
    }
    /* Find speed, in the speed section: needs the port open */
    AppendMenu(m, MF_STRING | ((open && !kfs.active) ? MF_ENABLED :
                               (MF_DISABLED | MF_GRAYED)),
               IDM_SERIAL_MIN + 0x10 * KSM_SLOT_FIND, KT_SERIAL_MENU_FIND_SPEED);
    AppendMenu(m, MF_SEPARATOR, 0, NULL);

    /* the ports present now, the one in use ticked - and the one in use
     * even when it is not detected (a pipe, a virtual port, unplugged) */
    line = ksm_bare(conf_get_str(conf, CONF_serline));
    ports = kitty_serial_ports(&nports);
    for (int i = 0; i < nports; i++)
        if (!stricmp(ports[i].name, line))
            line_listed = true;
    slot = 0;
    if (!line_listed && *line) {
        char *t = ksm_menu_text(line);
        ksm_ports[slot] = dupstr(line);
        AppendMenu(m, MF_STRING | MF_ENABLED | MF_CHECKED,
                   IDM_SERIAL_MIN + 0x10 * (KSM_SLOT_PORT0 + slot), t);
        sfree(t);
        slot++;
    }
    for (int i = 0; i < nports && slot < KSM_MAXPORTS; i++) {
        char *t = ksm_menu_text(ports[i].label);
        ksm_ports[slot] = dupstr(ports[i].name);
        AppendMenu(m, MF_STRING | MF_ENABLED |
                   (!stricmp(ports[i].name, line) ? MF_CHECKED : 0),
                   IDM_SERIAL_MIN + 0x10 * (KSM_SLOT_PORT0 + slot), t);
        sfree(t);
        slot++;
    }
    ksm_nports = slot;
    if (slot == 0)
        AppendMenu(m, MF_STRING | MF_DISABLED | MF_GRAYED,
                   IDM_SERIAL_MIN + 0x10 * KSM_SLOT_PORT0,
                   KT_SERIAL_MENU_NO_PORTS);
    kitty_serial_ports_free(ports, nports);
    return true;
}

bool kitty_serial_menu_command(HWND hwnd, WPARAM id, Conf *conf, Backend *be,
                               Terminal *term, LogContext *logctx)
{
    int slot;
    if (id < IDM_SERIAL_MIN || id >= IDM_SERIAL_MAX || !conf)
        return false;
    slot = (int)((id - IDM_SERIAL_MIN) / 0x10);

    if (slot < KITTY_SERIAL_NSPEEDS) {
        /* a speed: the open port takes it at once (silent reconfiguration);
         * a refused one is reported there and the old one stays */
        if (kfs.active)
            return true;
        conf_set_int(conf, CONF_serspeed, kitty_serial_speeds[slot]);
        force_reconf = 0;
        PostMessage(hwnd, WM_COMMAND, IDM_RECONF, 0);
    } else if (slot == KSM_SLOT_FIND) {
        kfs_start(hwnd, conf, be, term, logctx);
    } else if (slot >= KSM_SLOT_PORT0 && slot < KSM_SLOT_PORT0 + ksm_nports) {
        /* a port: the session restarts on it. Closing the port drops DTR,
         * which resets many boards (an Arduino among them). */
        const char *name = ksm_ports[slot - KSM_SLOT_PORT0];
        if (!stricmp(name, ksm_bare(conf_get_str(conf, CONF_serline))) &&
            serial_backend_port(be) != INVALID_HANDLE_VALUE)
            return true;            /* that port is open already */
        if (kfs.active)
            kfs_stop();
        conf_set_str(conf, CONF_serline, name);
        PostMessage(hwnd, WM_COMMAND, IDM_RESTARTSESSION, 0);
    }
    return true;
}

void kitty_serial_after_reconfig(Conf *conf, Backend *be)
{
    int speed, databits, stopbits, parity, flow;
    /* only while the port is open: a closed session keeps what was set
     * for its next start */
    if (!conf || serial_backend_port(be) == INVALID_HANDLE_VALUE ||
        !serial_backend_applied(be, &speed, &databits, &stopbits,
                                &parity, &flow))
        return;
    conf_set_int(conf, CONF_serspeed, speed);
    conf_set_int(conf, CONF_serdatabits, databits);
    conf_set_int(conf, CONF_serstopbits, stopbits);
    conf_set_int(conf, CONF_serparity, parity);
    conf_set_int(conf, CONF_serflow, flow);
}
