/*
 * kitty_urlsession.c - an ssh:// or telnet:// link opens a saved session
 * that matches it ([KiTTY] urlsessions=yes; cyd01/KiTTY#540).
 *
 * The link has been parsed by then (cmdline.c records what it carried,
 * cmdline_link). A saved session matches when all four hold:
 *   host     its Host Name equals the link's host (case ignored), or its own
 *            name - the leaf, the folder ignored - does;
 *   protocol ssh:// an SSH session, telnet:// a Telnet one;
 *   port     the link's port (22 / 23 when it has none) equals the session's;
 *   user     a link naming a user matches sessions with that user or with
 *            none stored; a session with another user never matches.
 * One match opens; several put up the chooser, whose Cancel ends the run;
 * none leaves today's path (Default Settings and the link's fields).
 *
 * The link overrides only what it carries: its user name applies to the
 * session found, its password is already cmdline_password (tried first, as
 * -pw's is); a field the link lacks leaves the stored one in force. Nothing
 * is written to the store. The match is what keeps a stored password safe:
 * it goes only to the host and port it was saved for.
 *
 * Runs before the saved command-line options are replayed
 * (cmdline_run_saved), so an option given with the link still applies on
 * top of the session, as it does with -load.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "putty.h"
#include "storage.h"

#include <windows.h>

#include "kitty.h"               /* INIT_SECTION */
#include "kitty_params.h"        /* ReadParameterN */
#include "kitty_inikeys.h"
#include "kitty_sessionpath.h"   /* kitty_session_names */
#include "kitty_dlgbox.h"        /* kitty_dialog_icon, kitty_fit_text */
#include "kitty_auxpos.h"
#include "kitty_text.h"
#include "kitty_urlsession.h"
#include "../windows/kitty_rc_additions.h"

extern HINSTANCE hinst;

/* [KiTTY] urlsessions: "yes" looks for a matching saved session, anything
 * else (and no key) keeps Default Settings. */
int GetUrlSessionsFlag(void)
{
    char buffer[16];
    if (!ReadParameterN(INIT_SECTION, KI_URLSESSIONS, buffer, sizeof(buffer)))
        return 0;
    return !stricmp(buffer, "yes");
}

/* [KiTTY] offernewsessioncreation: after the first login to a new host, offer to keep
 * the connection as a session. Anything but "no" (and no key) offers. */
int GetOfferSessionFlag(void)
{
    char buffer[16];
    if (!ReadParameterN(INIT_SECTION, KI_OFFERSESSION, buffer, sizeof(buffer)))
        return 1;
    return stricmp(buffer, "no") != 0;
}

/* The protocol a session's stored "Protocol" names, or -1. */
static int us_proto(const char *s)
{
    if (!s || !*s)
        return -1;
    if (!stricmp(s, "ssh"))
        return PROT_SSH;
    if (!stricmp(s, "telnet"))
        return PROT_TELNET;
    return -1;
}

/* The leaf of a session name: what follows the last folder separator. */
static const char *us_leaf(const char *name)
{
    const char *b = strrchr(name, '\\');
    return b ? b + 1 : name;
}

/* Whether the saved session `name` matches the link. A Host Name may carry
 * its own "user@" (PuTTY allows it): the host part is compared, and the user
 * part stands for a stored user when UserName is empty. */
static bool us_matches(const char *name, int proto, const char *host,
                       int port, const char *user)
{
    settings_r *r = open_settings_r(name);
    char *st_host, *st_proto, *st_user, *at;
    const char *h, *u;
    int st_prot, st_port;
    bool ok = false;

    if (!r)
        return false;
    st_host = read_setting_s(r, "HostName");
    st_proto = read_setting_s(r, "Protocol");
    st_user = read_setting_s(r, "UserName");
    st_prot = us_proto(st_proto);
    st_port = read_setting_i(r, "PortNumber",
                            st_prot == PROT_TELNET ? 23 : 22);
    close_settings_r(r);

    h = st_host ? st_host : "";
    u = st_user ? st_user : "";
    at = st_host ? strrchr(st_host, '@') : NULL;
    if (at) {
        *at = '\0';
        if (!*u)
            u = st_host;
        h = at + 1;
    }

    if (st_prot == proto && st_port == port &&
        (!stricmp(h, host) || !stricmp(us_leaf(name), host)) &&
        (!user || !*u || !strcmp(u, user)))
        ok = true;

    sfree(st_host);
    sfree(st_proto);
    sfree(st_user);
    return ok;
}

/* ---- the chooser ---- */

struct us_choose {
    const char *link;            /* shown in the text, no password */
    char **names;
    int n;
    int picked;                  /* index, -1 = Cancel */
};

static INT_PTR CALLBACK UrlChooseProc(HWND hwnd, UINT msg,
                                      WPARAM wParam, LPARAM lParam)
{
    struct us_choose *c = (struct us_choose *)GetWindowLongPtr(hwnd, DWLP_USER);

    switch (msg) {
      case WM_INITDIALOG: {
        char *text;
        int i, dy;
        c = (struct us_choose *)lParam;
        SetWindowLongPtr(hwnd, DWLP_USER, (LONG_PTR)c);
        SetWindowText(hwnd, KT_URLSESS_CHOOSER_TITLE);
        text = dupprintf(KT_URLSESS_CHOOSER_TEXT, c->link);
        dy = kitty_fit_text(hwnd, IDC_URLCHOOSE_TEXT, text, 0);
        sfree(text);
        if (dy) {
            /* what sits below the text moves down, the window grows */
            static const int below[] = { IDC_URLCHOOSE_LIST, IDOK, IDCANCEL };
            RECT wr;
            for (i = 0; i < (int)lenof(below); i++) {
                HWND w = GetDlgItem(hwnd, below[i]);
                RECT cr;
                GetWindowRect(w, &cr);
                MapWindowPoints(NULL, hwnd, (POINT *)&cr, 2);
                SetWindowPos(w, NULL, cr.left, cr.top + dy, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER);
            }
            GetWindowRect(hwnd, &wr);
            SetWindowPos(hwnd, NULL, 0, 0, wr.right - wr.left,
                         wr.bottom - wr.top + dy, SWP_NOMOVE | SWP_NOZORDER);
        }
        for (i = 0; i < c->n; i++)
            SendDlgItemMessage(hwnd, IDC_URLCHOOSE_LIST, LB_ADDSTRING, 0,
                               (LPARAM)c->names[i]);
        SendDlgItemMessage(hwnd, IDC_URLCHOOSE_LIST, LB_SETCURSEL, 0, 0);
        kitty_dialog_icon(hwnd, NULL);
        kitty_auxpos_apply(hwnd, KR_DLGPOS_URLCHOOSE, NULL, 0);
        SetForegroundWindow(hwnd);
        SetFocus(GetDlgItem(hwnd, IDC_URLCHOOSE_LIST));
        return 0;                /* the focus is set */
      }
      case WM_COMMAND:
        switch (LOWORD(wParam)) {
          case IDC_URLCHOOSE_LIST:
            /* a double-click opens */
            if (HIWORD(wParam) == LBN_DBLCLK)
                SendMessage(hwnd, WM_COMMAND, IDOK, 0);
            return 0;
          case IDOK: {
            int sel = (int)SendDlgItemMessage(hwnd, IDC_URLCHOOSE_LIST,
                                              LB_GETCURSEL, 0, 0);
            if (sel < 0 || sel >= c->n) {
                MessageBeep(0);
                return 1;
            }
            c->picked = sel;
            EndDialog(hwnd, IDOK);
            return 1;
          }
          case IDCANCEL:
            c->picked = -1;
            EndDialog(hwnd, IDCANCEL);
            return 1;
        }
        return 0;
      case WM_DESTROY:
        kitty_auxpos_save(hwnd, KR_DLGPOS_URLCHOOSE);
        return 0;
    }
    return 0;
}

/* The link as the chooser shows it: scheme, user, host, and the port when it
 * is not the protocol's own - never the password. */
static char *us_link_text(int proto, const char *host, int port,
                          const char *user)
{
    const char *scheme = proto == PROT_TELNET ? "telnet" : "ssh";
    int dflt = proto == PROT_TELNET ? 23 : 22;
    char portpart[16] = "";
    if (port != dflt)
        snprintf(portpart, sizeof(portpart), ":%d", port);
    return dupprintf("%s://%s%s%s%s", scheme, user ? user : "",
                     user ? "@" : "", host, portpart);
}

void kitty_urlsession_apply(Conf *conf)
{
    int proto, port, n, i, nm = 0;
    const char *host, *user;
    char **names, **match;
    const char *pick = NULL;
    struct us_choose c;

    if (!cmdline_link(&proto, &host, &port, &user) || !GetUrlSessionsFlag())
        return;

    names = kitty_session_names(&n);
    match = snewn(n > 0 ? n : 1, char *);
    for (i = 0; i < n; i++) {
        if (!stricmp(names[i], "Default Settings"))
            continue;
        if (us_matches(names[i], proto, host, port, user))
            match[nm++] = names[i];
    }

    c.picked = -1;
    if (nm == 1) {
        pick = match[0];
    } else if (nm > 1) {
        char *link = us_link_text(proto, host, port, user);
        c.link = link;
        c.names = match;
        c.n = nm;
        DialogBoxParam(hinst, MAKEINTRESOURCE(IDD_URLCHOOSE), NULL,
                       UrlChooseProc, (LPARAM)&c);
        sfree(link);
        if (c.picked < 0) {
            /* Cancel opens nothing: the run ends here */
            sfree(match);
            kitty_session_names_free(names, n);
            cleanup_exit(0);
        }
        pick = match[c.picked];
    }

    if (pick) {
        Conf *conf2 = conf_new();
        if (do_defaults(pick, conf2) && conf_launchable(conf2)) {
            conf_copy_into(conf, conf2);
            if (user)
                conf_set_str(conf, CONF_username, user);
        }
        conf_free(conf2);
    }
    sfree(match);
    kitty_session_names_free(names, n);
}
