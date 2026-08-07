/*
 * KiTTY named-proxy editor (hknet/KiTTY#11).
 *
 * A modal dialog to create / edit / delete the named proxy definitions that the
 * Session panel's proxy-override droplist selects from, and that the Proxy panel
 * can load permanently into a session. Pick a definition from
 * the combo to load its fields, or type a new name; Save writes it (via
 * SaveProxyInfo), Delete removes it (after a confirm). On any change we rescan
 * (InitProxyList) so the combo — and, after the dialog closes, the config box —
 * reflect the new set. Definitions are decoupled from any session's own proxy.
 *
 * Linked only into the GUI targets. See TASK_named_proxies.md.
 */
#include "putty.h"     /* first: pulls winsock2.h before windows.h (avoids -Wcpp warning) */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "kitty_proxy.h"
#include "kitty_rc_additions.h"

/* Type combo order -> CONF_proxy_type. The SSH types make a named proxy a
 * reusable jump host (the command field is the remote command/subsystem for
 * the exec/subsystem variants, as on the Proxy panel). */
static const int   pxe_types[] =
    { PROXY_NONE, PROXY_SOCKS4, PROXY_SOCKS5, PROXY_HTTP, PROXY_TELNET, PROXY_CMD,
      PROXY_SSH_TCPIP, PROXY_SSH_EXEC, PROXY_SSH_SUBSYSTEM };
static const char *pxe_type_names[] =
    { "None", "SOCKS 4", "SOCKS 5", "HTTP", "Telnet", "Local (command)",
      "SSH jump host (port forwarding)", "SSH jump host (execute a command)",
      "SSH jump host (invoke a subsystem)" };
#define PXE_NTYPES ((int)(sizeof(pxe_types)/sizeof(pxe_types[0])))

/* DNS-at-proxy combo -> CONF_proxy_dns (No/Auto/Yes, mirroring the Proxy panel). */
static const int   pxe_dns_vals[]  = { FORCE_OFF, AUTO, FORCE_ON };
static const char *pxe_dns_names[] = { "No", "Auto", "Yes" };
#define PXE_NDNS ((int)(sizeof(pxe_dns_vals)/sizeof(pxe_dns_vals[0])))
/* Proxy-diagnostics combo -> CONF_proxy_log_to_term. */
static const int   pxe_log_vals[]  = { FORCE_OFF, FORCE_ON, AUTO };
static const char *pxe_log_names[] = { "No", "Yes", "Only until session starts" };
#define PXE_NLOG ((int)(sizeof(pxe_log_vals)/sizeof(pxe_log_vals[0])))

/*
 * What the Host field above IS -> CONF_proxy_host_kind, stored as ProxyHostIs.
 *
 * PuTTY has always tried the Host as the title of a SAVED SESSION first, and
 * only then as a hostname. That is convenient and it is also how a jump host
 * that happens to share a name with a saved session silently drags that whole
 * session's configuration - including its own proxy - into the connection.
 * Saying which one it is here removes the guess, per proxy; leaving it at the
 * first entry keeps whatever kitty.ini [KiTTY] namedproxy says, which defaults
 * to the historical behaviour.
 */
static const int   pxe_hostis_vals[]  = { -1, 1, 0 };
static const char *pxe_hostis_names[] = {
    "as globally configured (kitty.ini: namedproxy=)",
    "a hostname or IP-address",
    "possibly the name of a saved session (PuTTY's old rule)" };
#define PXE_NHOSTIS ((int)(sizeof(pxe_hostis_vals)/sizeof(pxe_hostis_vals[0])))

/* The port a proxy of this type normally listens on, or 0 where the question
 * does not arise (None, and Local, which runs a command rather than connecting
 * to anything). Used to fill an empty port box, never to overwrite one. */
static int pxe_default_port(int proxy_type)
{
    switch (proxy_type) {
      case PROXY_SOCKS4:
      case PROXY_SOCKS5:        return 1080;
      case PROXY_HTTP:          return 3128;
      case PROXY_TELNET:        return 23;
      case PROXY_SSH_TCPIP:
      case PROXY_SSH_EXEC:
      case PROXY_SSH_SUBSYSTEM: return 22;
      default:                  return 0;
    }
}

static void pxe_combo_fill(HWND hdlg, int id, const char *const *names, int n)
{
    HWND cb = GetDlgItem(hdlg, id);
    for (int i = 0; i < n; i++) SendMessageA(cb, CB_ADDSTRING, 0, (LPARAM)names[i]);
}
static void pxe_combo_select_val(HWND hdlg, int id, const int *vals, int n, int val)
{
    int sel = 0;
    for (int i = 0; i < n; i++) if (vals[i] == val) sel = i;
    SendMessage(GetDlgItem(hdlg, id), CB_SETCURSEL, sel, 0);
}
static int pxe_combo_get_val(HWND hdlg, int id, const int *vals, int n)
{
    int sel = (int)SendMessage(GetDlgItem(hdlg, id), CB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= n) sel = 0;
    return vals[sel];
}

static int g_pxe_changed;   /* set when a definition was saved or deleted */
/*
 * The port WE last filled in from the type, or 0.
 *
 * Needed because arrowing through the type list with the keyboard sends a
 * selection change for every entry passed over: the first one (SOCKS 4) filled
 * 1080, and every later type then found a non-empty box and left it alone, so
 * the user landed on "SSH jump host" holding a SOCKS port they never typed
 *. A port we put there may be replaced; a port the user
 * typed never is, and the two are only distinguishable by remembering ours.
 */
static int g_pxe_autoport;
/* Definition to open the editor ON, or NULL for defaults. Set by
 * kitty_proxy_edit_dialog_for() and consumed in WM_INITDIALOG. */
static char *g_pxe_preselect = NULL;

static void pxe_fill_names(HWND hdlg)
{
    HWND cb = GetDlgItem(hdlg, IDC_PXE_NAME);
    SendMessage(cb, CB_RESETCONTENT, 0, 0);
    for (int i = 2; i < MAX_PROXY && proxies[i].name; i++)
        SendMessageA(cb, CB_ADDSTRING, 0, (LPARAM)proxies[i].name);
}

/* Warn (once, at the top) if any definition still has an unencrypted password;
 * blank when all are protected or in explicit legacy mode. */
static void pxe_update_banner(HWND hdlg)
{
    SetDlgItemTextA(hdlg, IDC_PXE_BANNER,
        kitty_proxy_any_plaintext_password()
        ? "Some proxies have an unencrypted password \x97 open each and Save to protect it."
        : "");
}

static void pxe_conf_to_fields(HWND hdlg, Conf *conf)
{
    int t = conf_get_int(conf, CONF_proxy_type), sel = 0;
    for (int i = 0; i < PXE_NTYPES; i++)
        if (pxe_types[i] == t) sel = i;
    SendMessage(GetDlgItem(hdlg, IDC_PXE_TYPE), CB_SETCURSEL, sel, 0);
    SetDlgItemTextA(hdlg, IDC_PXE_HOST, conf_get_str(conf, CONF_proxy_host));
    { int pp = conf_get_int(conf, CONF_proxy_port);
      if (pp > 0) SetDlgItemInt(hdlg, IDC_PXE_PORT, pp, FALSE);
      else SetDlgItemTextA(hdlg, IDC_PXE_PORT, ""); }   /* 0 -> blank (new proxy) */
    SetDlgItemTextA(hdlg, IDC_PXE_USER, conf_get_str(conf, CONF_proxy_username));
    SetDlgItemTextA(hdlg, IDC_PXE_PASS, conf_get_str(conf, CONF_proxy_password));
    SetDlgItemTextA(hdlg, IDC_PXE_COMMAND, conf_get_str(conf, CONF_proxy_telnet_command));
    SetDlgItemTextA(hdlg, IDC_PXE_EXCLUDE, conf_get_str(conf, CONF_proxy_exclude_list));
    CheckDlgButton(hdlg, IDC_PXE_LOCALHOST,
                   conf_get_bool(conf, CONF_even_proxy_localhost) ? BST_CHECKED : BST_UNCHECKED);
    pxe_combo_select_val(hdlg, IDC_PXE_DNS, pxe_dns_vals, PXE_NDNS,
                         conf_get_int(conf, CONF_proxy_dns));
    pxe_combo_select_val(hdlg, IDC_PXE_LOGTOTERM, pxe_log_vals, PXE_NLOG,
                         conf_get_int(conf, CONF_proxy_log_to_term));
    pxe_combo_select_val(hdlg, IDC_PXE_HOSTIS, pxe_hostis_vals, PXE_NHOSTIS,
                         conf_get_int(conf, CONF_proxy_host_kind));
    /* Whatever port is showing now came from the definition, not from us. */
    g_pxe_autoport = 0;
}

static void pxe_fields_to_conf(HWND hdlg, Conf *conf)
{
    char buf[4096];
    int sel = (int)SendMessage(GetDlgItem(hdlg, IDC_PXE_TYPE), CB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= PXE_NTYPES) sel = 0;
    conf_set_int(conf, CONF_proxy_type, pxe_types[sel]);
    GetDlgItemTextA(hdlg, IDC_PXE_HOST, buf, sizeof(buf)); conf_set_str(conf, CONF_proxy_host, buf);
    conf_set_int(conf, CONF_proxy_port, GetDlgItemInt(hdlg, IDC_PXE_PORT, NULL, FALSE));
    GetDlgItemTextA(hdlg, IDC_PXE_USER, buf, sizeof(buf)); conf_set_str(conf, CONF_proxy_username, buf);
    GetDlgItemTextA(hdlg, IDC_PXE_PASS, buf, sizeof(buf)); conf_set_str(conf, CONF_proxy_password, buf);
    GetDlgItemTextA(hdlg, IDC_PXE_COMMAND, buf, sizeof(buf)); conf_set_str(conf, CONF_proxy_telnet_command, buf);
    GetDlgItemTextA(hdlg, IDC_PXE_EXCLUDE, buf, sizeof(buf)); conf_set_str(conf, CONF_proxy_exclude_list, buf);
    conf_set_bool(conf, CONF_even_proxy_localhost,
                  IsDlgButtonChecked(hdlg, IDC_PXE_LOCALHOST) == BST_CHECKED);
    conf_set_int(conf, CONF_proxy_dns,
                 pxe_combo_get_val(hdlg, IDC_PXE_DNS, pxe_dns_vals, PXE_NDNS));
    conf_set_int(conf, CONF_proxy_log_to_term,
                 pxe_combo_get_val(hdlg, IDC_PXE_LOGTOTERM, pxe_log_vals, PXE_NLOG));
    conf_set_int(conf, CONF_proxy_host_kind,
                 pxe_combo_get_val(hdlg, IDC_PXE_HOSTIS, pxe_hostis_vals, PXE_NHOSTIS));
}

/* A fresh conf with valid defaults for the proxy fields we don't expose. */
static Conf *pxe_new_conf(void)
{
    Conf *conf = conf_new();
    do_defaults(NULL, conf);
    /* do_defaults() loads the user's "Default Settings", which may itself carry a
     * proxy (host/port/creds/type). A brand-new named proxy must start BLANK, not
     * inherit that — otherwise the editor pre-fills the Default-Settings proxy.
     * Clear the per-proxy identity fields; keep neutral field defaults for the
     * rest (command / DNS / diagnostics). */
    conf_set_int(conf, CONF_proxy_type, PROXY_NONE);
    conf_set_str(conf, CONF_proxy_host, "");
    conf_set_int(conf, CONF_proxy_port, 0);
    conf_set_str(conf, CONF_proxy_username, "");
    conf_set_str(conf, CONF_proxy_password, "");
    conf_set_str(conf, CONF_proxy_exclude_list, "");
    return conf;
}

static void pxe_load_named(HWND hdlg, const char *name)
{
    Conf *conf = pxe_new_conf();
    if (name && name[0])
        LoadProxyInfo(conf, name);   /* overlays the definition's Proxy* fields */
    pxe_conf_to_fields(hdlg, conf);
    conf_free(conf);
}

/* Whether the Session-panel Proxy-choice row exists is decided when the config
 * box is built, so in "auto" mode crossing 0<->1 named proxies only takes effect
 * on the next config-box open. Tell the user once, when it matters (auto mode
 * only; "yes"/"no" have a fixed row). appearing=1: first proxy just added;
 * appearing=0: last proxy just removed. */
static void pxe_note_reopen(HWND hdlg, int appearing)
{
    if (GetProxySelectionFlag() != 0)    /* only "auto" shows/hides by count */
        return;
    MessageBoxA(hdlg, appearing
        ? "Proxy defined.\r\n\r\nThe proxy-override droplist will appear in the "
          "Session panel the next time a configuration window is opened - start "
          "a new KiTTY, or use the system-menu \"Change Settings...\" of a running "
          "session (that just reopens the config box; it does NOT disconnect or "
          "affect the live session)."
        : "The last named proxy was removed.\r\n\r\nThe proxy-override droplist "
          "will disappear from the Session panel the next time a configuration "
          "window is opened - start a new KiTTY, or use the system-menu \"Change "
          "Settings...\" of a running session (that just reopens the config box; "
          "it does NOT disconnect or affect the live session).",
        "KiTTY named proxy", MB_OK | MB_ICONINFORMATION);
}

static INT_PTR CALLBACK pxe_dlgproc(HWND hdlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
      case WM_INITDIALOG: {
        HWND tc = GetDlgItem(hdlg, IDC_PXE_TYPE);
        for (int i = 0; i < PXE_NTYPES; i++)
            SendMessageA(tc, CB_ADDSTRING, 0, (LPARAM)pxe_type_names[i]);
        pxe_combo_fill(hdlg, IDC_PXE_DNS, pxe_dns_names, PXE_NDNS);
        pxe_combo_fill(hdlg, IDC_PXE_LOGTOTERM, pxe_log_names, PXE_NLOG);
        pxe_combo_fill(hdlg, IDC_PXE_HOSTIS, pxe_hostis_names, PXE_NHOSTIS);
        pxe_fill_names(hdlg);
        /* Open on the definition the caller named - normally the one selected in
         * the override droplist that the Edit button sits beside. Built-ins are
         * not definitions, so the caller passes NULL for those. */
        if (g_pxe_preselect) {
            HWND cb = GetDlgItem(hdlg, IDC_PXE_NAME);
            int idx = (int)SendMessageA(cb, CB_FINDSTRINGEXACT,
                                        (WPARAM)-1, (LPARAM)g_pxe_preselect);
            if (idx != CB_ERR) {
                SendMessage(cb, CB_SETCURSEL, idx, 0);
                pxe_load_named(hdlg, g_pxe_preselect);
            } else {
                pxe_load_named(hdlg, NULL);
            }
        } else {
            pxe_load_named(hdlg, NULL);   /* defaults (incl. command / DNS / diagnostics) */
        }
        pxe_update_banner(hdlg);
        SetForegroundWindow(hdlg);
        return TRUE;
      }

      case WM_COMMAND:
        switch (LOWORD(wp)) {
          case IDC_PXE_TYPE:
            /* KiTTY: picking a type fills in that type's usual port, but only
             * into an EMPTY box - a port somebody typed is never overwritten.
             * An unset port used to travel all the way to the connection as
             * port 0 and surface as "Cannot assign requested address", which
             * reads as a network fault rather than a blank field. */
            if (HIWORD(wp) == CBN_SELCHANGE) {
                BOOL ok = FALSE;
                UINT cur = GetDlgItemInt(hdlg, IDC_PXE_PORT, &ok, FALSE);
                /* Empty, or still showing the port we filled in ourselves. */
                if (!ok || cur == 0 || (g_pxe_autoport && (int)cur == g_pxe_autoport)) {
                    int sel = (int)SendMessage(GetDlgItem(hdlg, IDC_PXE_TYPE),
                                               CB_GETCURSEL, 0, 0);
                    int port = pxe_default_port(sel >= 0 && sel < PXE_NTYPES
                                                ? pxe_types[sel] : PROXY_NONE);
                    if (port > 0) {
                        SetDlgItemInt(hdlg, IDC_PXE_PORT, port, FALSE);
                        g_pxe_autoport = port;
                    }
                }
            }
            return TRUE;

          case IDC_PXE_NAME:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                HWND cb = GetDlgItem(hdlg, IDC_PXE_NAME);
                int idx = (int)SendMessage(cb, CB_GETCURSEL, 0, 0);
                if (idx >= 0) {
                    char name[512];
                    SendMessageA(cb, CB_GETLBTEXT, idx, (LPARAM)name);
                    pxe_load_named(hdlg, name);
                }
            }
            return TRUE;

          case IDC_PXE_SHOWPW: {
            BOOL show = (IsDlgButtonChecked(hdlg, IDC_PXE_SHOWPW) == BST_CHECKED);
            HWND pw = GetDlgItem(hdlg, IDC_PXE_PASS);
            SendMessage(pw, EM_SETPASSWORDCHAR, show ? 0 : (WPARAM)'*', 0);
            InvalidateRect(pw, NULL, TRUE);
            return TRUE;
          }

          case IDC_PXE_SAVE: {
            char name[512];
            GetDlgItemTextA(hdlg, IDC_PXE_NAME, name, sizeof(name));
            /* trim trailing/leading spaces */
            char *p = name; while (*p == ' ') p++;
            size_t l = strlen(p); while (l && p[l-1] == ' ') p[--l] = '\0';
            if (!p[0]) {
                MessageBoxA(hdlg, "Please enter a name for the proxy.", "KiTTY",
                            MB_OK | MB_ICONINFORMATION);
                return TRUE;
            }
            if (!strcmp(p, KITTY_PROXY_SESSION) || !strcmp(p, KITTY_PROXY_NONE)) {
                MessageBoxA(hdlg, "That name is reserved.", "KiTTY",
                            MB_OK | MB_ICONWARNING);
                return TRUE;
            }
            /* KiTTY: a type that connects somewhere needs a port, and leaving
             * the box empty is easy to do. Ask rather than saving a definition
             * that can only fail later, at connect time, as a network error. */
            {
                int tsel = (int)SendMessage(GetDlgItem(hdlg, IDC_PXE_TYPE),
                                            CB_GETCURSEL, 0, 0);
                int ttype = (tsel >= 0 && tsel < PXE_NTYPES)
                            ? pxe_types[tsel] : PROXY_NONE;
                int want = pxe_default_port(ttype);
                BOOL ok = FALSE;
                UINT port = GetDlgItemInt(hdlg, IDC_PXE_PORT, &ok, FALSE);
                if (want > 0 && (!ok || port == 0)) {
                    char q[256];
                    snprintf(q, sizeof(q),
                             "This proxy has no port. Use the usual port %d for "
                             "this type?\n\nChoose No to go back and type one.",
                             want);
                    if (MessageBoxA(hdlg, q, "KiTTY",
                                    MB_YESNO | MB_ICONQUESTION) != IDYES) {
                        SetFocus(GetDlgItem(hdlg, IDC_PXE_PORT));
                        return TRUE;
                    }
                    SetDlgItemInt(hdlg, IDC_PXE_PORT, want, FALSE);
                }
            }
            int was_empty = !kitty_has_proxy_definitions();
            Conf *conf = pxe_new_conf();
            LoadProxyInfo(conf, p);        /* preserve unexposed fields when editing */
            pxe_fields_to_conf(hdlg, conf);
            SaveProxyInfo(conf, p);
            conf_free(conf);
            g_pxe_changed = 1;
            InitProxyList();               /* rescan so the combo reflects it */
            pxe_fill_names(hdlg);
            pxe_update_banner(hdlg);       /* saving this one may clear the warning */
            if (was_empty && kitty_has_proxy_definitions())
                pxe_note_reopen(hdlg, 1);  /* 0->1: Session-panel row needs a reopen */
            int idx = (int)SendMessageA(GetDlgItem(hdlg, IDC_PXE_NAME),
                                        CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)p);
            if (idx >= 0)
                SendMessage(GetDlgItem(hdlg, IDC_PXE_NAME), CB_SETCURSEL, idx, 0);
            else
                SetDlgItemTextA(hdlg, IDC_PXE_NAME, p);
            return TRUE;
          }

          case IDC_PXE_DELETE: {
            char name[512];
            GetDlgItemTextA(hdlg, IDC_PXE_NAME, name, sizeof(name));
            if (!name[0]) return TRUE;
            char msg[600];
            snprintf(msg, sizeof(msg), "Delete the named proxy \"%s\"?", name);
            if (MessageBoxA(hdlg, msg, "KiTTY named proxy",
                            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
                return TRUE;
            DeleteProxyInfo(name);
            g_pxe_changed = 1;
            InitProxyList();
            pxe_fill_names(hdlg);
            pxe_update_banner(hdlg);
            if (!kitty_has_proxy_definitions())
                pxe_note_reopen(hdlg, 0);  /* 1->0: Session-panel row needs a reopen */
            SetDlgItemTextA(hdlg, IDC_PXE_NAME, "");
            pxe_load_named(hdlg, NULL);    /* clear fields to defaults */
            return TRUE;
          }

          case IDCANCEL:
            EndDialog(hdlg, g_pxe_changed);
            return TRUE;
        }
        break;

      case WM_CLOSE:
        EndDialog(hdlg, g_pxe_changed);
        return TRUE;
    }
    return FALSE;
}

/* Launch the editor. Returns 1 if any definition was created/edited/deleted, so
 * the caller can rescan/refresh the config box's proxy droplist.
 *
 * `preselect` is the definition to open on, or NULL to start on defaults. The Edit
 * button beside the override droplist passes whatever is selected there: if a
 * named proxy is showing, that is overwhelmingly the one the user means to edit,
 * and making them pick it again in a second combo is busywork. */
int kitty_proxy_edit_dialog_for(HWND owner, const char *preselect)
{
    g_pxe_changed = 0;
    InitProxyList();
    sfree(g_pxe_preselect);
    g_pxe_preselect = (preselect && *preselect) ? dupstr(preselect) : NULL;
    DialogBoxA(GetModuleHandle(NULL), MAKEINTRESOURCEA(IDD_PROXYEDIT), owner, pxe_dlgproc);
    sfree(g_pxe_preselect);
    g_pxe_preselect = NULL;
    return g_pxe_changed;
}

int kitty_proxy_edit_dialog(HWND owner)
{
    return kitty_proxy_edit_dialog_for(owner, NULL);
}

/* ---- "Load named proxy pre-sets": pick one definition ---- */

static char g_pxp_picked[512];

static INT_PTR CALLBACK pxp_dlgproc(HWND hdlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
      case WM_INITDIALOG: {
        HWND cb = GetDlgItem(hdlg, IDC_PXP_LIST);
        for (int i = 2; i < MAX_PROXY && proxies[i].name; i++)
            SendMessageA(cb, CB_ADDSTRING, 0, (LPARAM)proxies[i].name);
        /* Preselect the one named on the way in (the session's current choice),
         * else the first, so OK always means something. */
        int sel = 0;
        if (g_pxp_picked[0]) {
            int f = (int)SendMessageA(cb, CB_FINDSTRINGEXACT, (WPARAM)-1,
                                      (LPARAM)g_pxp_picked);
            if (f != CB_ERR) sel = f;
        }
        SendMessage(cb, CB_SETCURSEL, sel, 0);
        return TRUE;
      }
      case WM_COMMAND:
        if (LOWORD(wParam) == IDOK) {
            HWND cb = GetDlgItem(hdlg, IDC_PXP_LIST);
            int sel = (int)SendMessage(cb, CB_GETCURSEL, 0, 0);
            g_pxp_picked[0] = '\0';
            if (sel != CB_ERR)
                SendMessageA(cb, CB_GETLBTEXT, sel, (LPARAM)g_pxp_picked);
            EndDialog(hdlg, g_pxp_picked[0] ? IDOK : IDCANCEL);
            return TRUE;
        } else if (LOWORD(wParam) == IDCANCEL) {
            EndDialog(hdlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

/* 1 and the chosen name in out[] when the user picked one and pressed OK.
 * out may carry a name on the way in, which is preselected. */
int kitty_proxy_pick_dialog(HWND owner, char *out, int len)
{
    INT_PTR r;
    InitProxyList();
    if (!proxies[2].name)
        return 0;                      /* nothing to pick */
    g_pxp_picked[0] = '\0';
    if (out && len > 0 && out[0] && strlen(out) < sizeof(g_pxp_picked))
        strcpy(g_pxp_picked, out);
    r = DialogBoxA(GetModuleHandle(NULL), MAKEINTRESOURCEA(IDD_PROXYPICK),
                   owner, pxp_dlgproc);
    if (r != IDOK || !g_pxp_picked[0] || (int)strlen(g_pxp_picked) >= len)
        return 0;
    strcpy(out, g_pxp_picked);
    return 1;
}
