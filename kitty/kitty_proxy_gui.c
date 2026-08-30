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
#include "dialog.h"     /* the ctrl_* panel API */
#include "kitty_proxy.h"
#include "kitty_text.h"   /* the words the panels show */
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

/* DNS-at-proxy combo -> CONF_proxy_dns (auto = at the proxy for far hosts, local = here, proxy = always there). */
static const int   pxe_dns_vals[]  = { AUTO, FORCE_OFF, FORCE_ON };
static const char *pxe_dns_names[] = { "auto", "local", "proxy" };
#define PXE_NDNS ((int)(sizeof(pxe_dns_vals)/sizeof(pxe_dns_vals[0])))
/* Proxy-diagnostics combo -> CONF_proxy_log_to_term. */
static const int   pxe_log_vals[]  = { FORCE_OFF, FORCE_ON, AUTO };
static const char *pxe_log_names[] = { "never", "always", "connect only" };
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

/* ------------------------------------------------------------------ *
 * The Application tab's "Named proxies" panel (design §9.3b).
 *
 * The same definitions the IDD_PROXYEDIT window edits, as an ordinary config
 * box panel: it inherits the theme, the font, the panel cache and the panel
 * area's scrolling instead of being a pop-up that has to be kept in step with
 * all four by hand.
 *
 * WHAT IS EDITED. A named proxy is a set of Proxy* fields stored under a name,
 * and the store is three calls - LoadProxyInfo, SaveProxyInfo, DeleteProxyInfo.
 * The panel keeps a scratch Conf holding the definition being edited; the
 * fields read and write THAT, never the session's own Conf, which is what
 * dp->data points at and what every other panel edits. That is the whole
 * reason these handlers exist rather than reusing conf_editbox_handler.
 *
 * UNSAVED EDITS. Nothing is written until Save. Switching panels, loading a
 * session or closing the box therefore discards what was typed, exactly as
 * closing the old window with Close did. The banner says so rather than
 * leaving it to be discovered.
 *
 * ONE PANEL AT A TIME. The panel's data hangs off a file-static, the way the
 * workplace-mode panel and the session saver do it: a configuration box builds
 * one of each panel, and the alternative is threading a pointer through every
 * handler's context, which is already spoken for by the Conf key each field
 * edits.
 */

struct pxpanel_data {
    Conf *conf;                 /* the definition being edited */
    char *name;                 /* its name, as picked or typed */
    dlgcontrol *namebox, *banner, *pwbox;
    /*
     * True while the panel is filling its own controls. Refreshing the name
     * box clears its list, which clears the edit with it, and the control
     * duly reports that its text changed - which is indistinguishable from
     * the user emptying the box unless the panel says "that was me". Without
     * this the name blanked itself the moment a definition was picked.
     */
    bool refreshing;
    /*
     * Whether the port was typed rather than filled in. An untouched port
     * follows the type's usual port; once it has been edited it is the user's
     * and nothing overwrites it.
     */
    bool port_typed;
    bool dirty;                 /* edited since the last Save */
};
static struct pxpanel_data *g_pxp = NULL;

/* The value tables above, chosen by which Conf field the control edits. */
static const int *pxp_vals_for(int key, const char *const **names, int *n)
{
    switch (key) {
      case CONF_proxy_type:
        *names = pxe_type_names; *n = PXE_NTYPES; return pxe_types;
      case CONF_proxy_dns:
        *names = pxe_dns_names;  *n = PXE_NDNS;   return pxe_dns_vals;
      case CONF_proxy_log_to_term:
        *names = pxe_log_names;  *n = PXE_NLOG;   return pxe_log_vals;
      case CONF_proxy_host_kind:
        *names = pxe_hostis_names; *n = PXE_NHOSTIS; return pxe_hostis_vals;
    }
    *names = NULL; *n = 0; return NULL;
}

static void pxp_say(dlgparam *dlg, const char *what)
{
    if (g_pxp && g_pxp->banner)
        dlg_label_change(g_pxp->banner, dlg, what);
}

static void pxp_reload(dlgparam *dlg)
{
    if (!g_pxp)
        return;
    if (g_pxp->conf)
        conf_free(g_pxp->conf);
    g_pxp->conf = pxe_new_conf();
    if (g_pxp->name && g_pxp->name[0])
        LoadProxyInfo(g_pxp->conf, g_pxp->name);
    g_pxp->port_typed = false;     /* a freshly loaded port is not typed */
    g_pxp->dirty = false;
    g_pxp->refreshing = true;
    dlg_refresh(NULL, dlg);
    g_pxp->refreshing = false;
}

static void pxp_name_handler(dlgcontrol *ctrl, dlgparam *dlg,
                             void *data, int event)
{
    if (!g_pxp)
        return;
    if (event == EVENT_REFRESH) {
        int i, row = -1, sel = -1;
        dlg_update_start(ctrl, dlg);
        dlg_listbox_clear(ctrl, dlg);
        for (i = 2; i < MAX_PROXY && proxies[i].name; i++) {
            row++;
            dlg_listbox_add(ctrl, dlg, proxies[i].name);
            if (g_pxp->name && !strcmp(proxies[i].name, g_pxp->name))
                sel = row;
        }
        g_pxp->refreshing = true;
        /*
         * SELECT the row, do not merely set the text. Refilling the list left
         * the combo with nothing selected, and a drop-down shows its
         * selection - so picking a definition loaded its values while the name
         * box sat empty until a Tab moved the focus and something put the text
         * back. A name being typed for a NEW definition matches no row, and
         * that one really is just text.
         */
        if (sel >= 0)
            dlg_listbox_select(ctrl, dlg, sel);
        else
            dlg_editbox_set(ctrl, dlg, g_pxp->name ? g_pxp->name : "");
        g_pxp->refreshing = false;
        dlg_update_done(ctrl, dlg);
    } else if (event == EVENT_VALCHANGE) {
        char *typed;
        if (g_pxp->refreshing)
            return;                 /* the panel filling its own box */
        typed = dlg_editbox_get(ctrl, dlg);
        if (!typed[0]) {
            /* Emptying the box is what CLEARING THE LIST looks like from here,
             * and it is never what a user means by picking a definition. */
            sfree(typed);
            return;
        }
        if (g_pxp->name && !strcmp(g_pxp->name, typed)) {
            sfree(typed);
            return;                 /* the programmatic set above, not a user */
        }
        sfree(g_pxp->name);
        g_pxp->name = typed;
        pxp_reload(dlg);
        pxp_say(dlg, "Editing this definition. Nothing is stored until Save.");
    }
}

static void pxp_str_handler(dlgcontrol *ctrl, dlgparam *dlg,
                            void *data, int event)
{
    if (!g_pxp || !g_pxp->conf)
        return;
    if (event == EVENT_REFRESH) {
        g_pxp->refreshing = true;
        dlg_editbox_set(ctrl, dlg, conf_get_str(g_pxp->conf, ctrl->context.i));
        g_pxp->refreshing = false;
    }
    else if (event == EVENT_VALCHANGE) {
        char *s = dlg_editbox_get(ctrl, dlg);
        conf_set_str(g_pxp->conf, ctrl->context.i, s);
        sfree(s);
        if (!g_pxp->refreshing)
            g_pxp->dirty = true;
    }
}

static void pxp_int_handler(dlgcontrol *ctrl, dlgparam *dlg,
                            void *data, int event)
{
    if (!g_pxp || !g_pxp->conf)
        return;
    if (event == EVENT_REFRESH) {
        char buf[32];
        sprintf(buf, "%d", conf_get_int(g_pxp->conf, ctrl->context.i));
        g_pxp->refreshing = true;
        dlg_editbox_set(ctrl, dlg, buf);
        g_pxp->refreshing = false;
    } else if (event == EVENT_VALCHANGE) {
        char *s = dlg_editbox_get(ctrl, dlg);
        conf_set_int(g_pxp->conf, ctrl->context.i, atoi(s));
        sfree(s);
        if (!g_pxp->refreshing) {
            g_pxp->dirty = true;
            if (ctrl->context.i == CONF_proxy_port)
                g_pxp->port_typed = true;   /* the user's now; leave it alone */
        }
    }
}

static void pxp_bool_handler(dlgcontrol *ctrl, dlgparam *dlg,
                             void *data, int event)
{
    if (!g_pxp || !g_pxp->conf)
        return;
    if (event == EVENT_REFRESH) {
        g_pxp->refreshing = true;
        dlg_checkbox_set(ctrl, dlg,
                         conf_get_bool(g_pxp->conf, ctrl->context.i));
        g_pxp->refreshing = false;
    }
    else if (event == EVENT_VALCHANGE) {
        conf_set_bool(g_pxp->conf, ctrl->context.i,
                      dlg_checkbox_get(ctrl, dlg));
        if (!g_pxp->refreshing)
            g_pxp->dirty = true;
    }
}

static void pxp_list_handler(dlgcontrol *ctrl, dlgparam *dlg,
                             void *data, int event)
{
    const char *const *names;
    const int *vals;
    int n, i;

    if (!g_pxp || !g_pxp->conf)
        return;
    vals = pxp_vals_for(ctrl->context.i, &names, &n);
    if (!vals)
        return;
    if (event == EVENT_REFRESH) {
        int cur = conf_get_int(g_pxp->conf, ctrl->context.i);
        g_pxp->refreshing = true;
        dlg_update_start(ctrl, dlg);
        dlg_listbox_clear(ctrl, dlg);
        for (i = 0; i < n; i++)
            dlg_listbox_addwithid(ctrl, dlg, names[i], vals[i]);
        for (i = 0; i < n; i++)
            if (vals[i] == cur)
                dlg_listbox_select(ctrl, dlg, i);
        dlg_update_done(ctrl, dlg);
        g_pxp->refreshing = false;
    } else if (event == EVENT_SELCHANGE) {
        int idx = dlg_listbox_index(ctrl, dlg);
        if (idx < 0 || idx >= n)
            return;
        conf_set_int(g_pxp->conf, ctrl->context.i, vals[idx]);
        if (g_pxp->refreshing)
            return;
        g_pxp->dirty = true;
        /* A port nobody has typed follows the type: choosing SOCKS 5 fills in
         * 1080, choosing HTTP fills in 8080. Once the port has been typed it
         * is the user's and the type stops touching it. */
        if (ctrl->context.i == CONF_proxy_type && !g_pxp->port_typed) {
            int want = pxe_default_port(vals[idx]);
            conf_set_int(g_pxp->conf, CONF_proxy_port, want);
            g_pxp->refreshing = true;
            dlg_refresh(NULL, dlg);
            g_pxp->refreshing = false;
        }
    }
}

static void pxp_save_handler(dlgcontrol *ctrl, dlgparam *dlg,
                             void *data, int event)
{
    int was_empty, want;

    if (event != EVENT_ACTION || !g_pxp || !g_pxp->conf)
        return;
    if (!g_pxp->name || !g_pxp->name[0]) {
        dlg_error_msg(dlg, "Give the proxy a name first.");
        return;
    }
    /* An empty port on a type that has a usual one is filled in rather than
     * queried. The window asked; a panel that puts up a question box while the
     * user is looking at a page is worse than one that does the obvious thing
     * and says so. */
    want = pxe_default_port(conf_get_int(g_pxp->conf, CONF_proxy_type));
    if (want > 0 && conf_get_int(g_pxp->conf, CONF_proxy_port) == 0)
        conf_set_int(g_pxp->conf, CONF_proxy_port, want);

    was_empty = !kitty_has_proxy_definitions();
    SaveProxyInfo(g_pxp->conf, g_pxp->name);
    g_pxp->dirty = false;
    InitProxyList();                /* rescan, so the name list shows it */
    dlg_refresh(NULL, dlg);
    if (was_empty && kitty_has_proxy_definitions())
        dlg_error_msg(dlg,
            "Proxy defined.\r\n\r\nThe proxy-override droplist appears in the "
            "Session panel the next time a configuration window is opened.");
    else
        pxp_say(dlg, "Saved.");
}

static void pxp_delete_handler(dlgcontrol *ctrl, dlgparam *dlg,
                               void *data, int event)
{
    if (event != EVENT_ACTION || !g_pxp)
        return;
    if (!g_pxp->name || !g_pxp->name[0])
        return;
    /* Deletion is the one thing here that cannot be undone by not saving, so
     * it keeps its confirmation. */
    {
        char msg[600];
        snprintf(msg, sizeof(msg), "Delete the named proxy \"%s\"?",
                 g_pxp->name);
        if (MessageBoxA(kitty_cfg_modal_owner(), msg, "KiTTY named proxy",
                        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
            return;
    }
    DeleteProxyInfo(g_pxp->name);
    InitProxyList();
    sfree(g_pxp->name);
    g_pxp->name = dupstr("");
    pxp_reload(dlg);
    if (!kitty_has_proxy_definitions())
        dlg_error_msg(dlg,
            "The last named proxy was removed.\r\n\r\nThe proxy-override "
            "droplist disappears from the Session panel the next time a "
            "configuration window is opened.");
    else
        pxp_say(dlg, "Deleted.");
}

/* "Show password" for the panel's own password box - the same idea as the one
 * on Connection/Login, kept local because that one's state lives in the config
 * file this panel does not belong to. */
static void pxp_showpw_handler(dlgcontrol *ctrl, dlgparam *dlg,
                               void *data, int event)
{
    if (!g_pxp || !g_pxp->pwbox)
        return;
    if (event == EVENT_REFRESH) {
        dlg_checkbox_set(ctrl, dlg, false);
        dlg_editbox_set_masked(g_pxp->pwbox, dlg, false);
    } else if (event == EVENT_VALCHANGE) {
        dlg_editbox_set_masked(g_pxp->pwbox, dlg, dlg_checkbox_get(ctrl, dlg));
    }
}

/* Which definition the panel should come up on, set by whoever sends the user
 * there. Consumed on the panel's next refresh so a later visit is not still
 * being steered by an old click. */
static char *g_pxp_want = NULL;

void kitty_proxy_panel_preselect(const char *name)
{
    sfree(g_pxp_want);
    g_pxp_want = (name && name[0]) ? dupstr(name) : NULL;
    if (g_pxp && g_pxp_want) {
        sfree(g_pxp->name);
        g_pxp->name = dupstr(g_pxp_want);
        if (g_pxp->conf)
            conf_free(g_pxp->conf);
        g_pxp->conf = pxe_new_conf();
        LoadProxyInfo(g_pxp->conf, g_pxp->name);
    }
}

/*
 * "You have unsaved changes" - asked when the user tries to leave the panel
 * with an edit in hand. Nothing on this panel reaches the store until Save, so
 * the alternative is losing it silently, which is not a choice an application
 * setting should make for someone.
 *
 * A message box rather than a banner: this is a question whose answer changes
 * what happens next, and it is asked only when there is actually something to
 * lose.
 */
/*
 * Is there an unsaved edit on this panel?
 *
 * Asked by the config box's width reflow, which lays a panel out again by
 * DESTROYING its controls - so it must not touch this one while it holds
 * something the store has not got. Unlike pxp_may_leave() this asks nothing
 * and changes nothing: a window being dragged is not the moment to interrupt
 * with a question, and the reflow can leave this one panel clipped until it
 * is saved or left.
 */
bool kitty_proxy_panel_dirty(void)
{
    return g_pxp && g_pxp->dirty;
}

static bool pxp_may_leave(void)
{
    if (!g_pxp || !g_pxp->dirty)
        return true;
    if (MessageBoxA(kitty_cfg_modal_owner(),
        "This named proxy has changes that have not been saved.\r\n\r\n"
        "Leave the panel and discard them?",
        "KiTTY named proxy",
        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
        return false;
    /*
     * Discard means DISCARD: drop the edited copy and take the stored
     * definition back. Leaving the flag set would also mean every later panel
     * switch asked the same question, because the guard is registered for the
     * box rather than for whichever panel is showing.
     */
    if (g_pxp->conf)
        conf_free(g_pxp->conf);
    g_pxp->conf = pxe_new_conf();
    if (g_pxp->name && g_pxp->name[0])
        LoadProxyInfo(g_pxp->conf, g_pxp->name);
    g_pxp->dirty = false;
    g_pxp->port_typed = false;
    return true;
}

int GetPuttyFlag(void);   /* kitty_commun.c */

void kitty_proxy_build_panel(struct controlbox *b)
{
    struct controlset *s;
    struct pxpanel_data *pd;
    dlgcontrol *c;

    /*
     * NOT gated on proxyselection.
     *
     * This is an APPLICATION panel: it is where the definitions live, and an
     * application panel does not come and go with a setting. It used to
     * disappear on proxyselection=no, which is the setting for whether
     * SESSIONS are offered a chooser - so turning that off took away the only
     * place the definitions can be edited, and turning it back on again meant
     * finding the key in kitty.ini. What proxyselection=no still does is
     * remove the chooser and the Edit button from the session panels, which
     * is what it is for.
     */
    if (GetPuttyFlag())
        return;

    pd = (struct pxpanel_data *)ctrl_alloc(b, sizeof(struct pxpanel_data));
    memset(pd, 0, sizeof(*pd));
    pd->name = dupstr("");
    pd->conf = pxe_new_conf();
    g_pxp = pd;

    {
        extern void kitty_cfg_set_leave_guard(bool (*fn)(void));
        kitty_cfg_set_leave_guard(pxp_may_leave);
    }

    ctrl_settitle(b, "Application/Named proxies",
                  KT_NAMED_PROXIES_PROXY_DEFINITIONS_SHARED_BY_EVERY);

    s = ctrl_getset(b, "Application/Named proxies", "which", KT_NAMED_PROXIES_DEFINITION);
    pd->namebox = ctrl_combobox(s, KT_NAMED_PROXIES_NAME_PICK_ONE_TO_EDIT,
                                NO_SHORTCUT, 100, HELPCTX(kitty_named_proxies),
                                pxp_name_handler, P(NULL), P(NULL));
    ctrl_droplist(s, KT_NAMED_PROXIES_TYPE, NO_SHORTCUT, 60, HELPCTX(kitty_named_proxies),
                  pxp_list_handler, I(CONF_proxy_type));

    s = ctrl_getset(b, "Application/Named proxies", "where",
                    KT_NAMED_PROXIES_PROXY_JUMP_HOST_SESSION_NAME);
    ctrl_editbox(s, KT_NAMED_PROXIES_NAME_IP, NO_SHORTCUT, 70, HELPCTX(kitty_named_proxies),
                 pxp_str_handler, I(CONF_proxy_host), ED_STR);
    ctrl_editbox(s, KT_NAMED_PROXIES_PORT, NO_SHORTCUT, 30, HELPCTX(kitty_named_proxies),
                 pxp_int_handler, I(CONF_proxy_port), ED_STR);
    ctrl_droplist(s, KT_NAMED_PROXIES_THIS, NO_SHORTCUT, 70, HELPCTX(kitty_named_proxies),
                  pxp_list_handler, I(CONF_proxy_host_kind));
    ctrl_editbox(s, KT_NAMED_PROXIES_USERNAME, NO_SHORTCUT, 70, HELPCTX(kitty_named_proxies),
                 pxp_str_handler, I(CONF_proxy_username), ED_STR);
    c = ctrl_editbox(s, KT_NAMED_PROXIES_PASSWORD, NO_SHORTCUT, 70, HELPCTX(kitty_named_proxies),
                     pxp_str_handler, I(CONF_proxy_password), ED_STR);
    c->editbox.password = true;
    pd->pwbox = c;
    ctrl_checkbox(s, KT_DATA_SHOW_PASSWORD, NO_SHORTCUT, HELPCTX(kitty_named_proxies),
                  pxp_showpw_handler, P(NULL));

    s = ctrl_getset(b, "Application/Named proxies", "opts", KT_ZMODEM_OPTIONS);
    ctrl_editbox(s, KT_NAMED_PROXIES_COMMAND_TO_SEND_TELNET_LOCAL, NO_SHORTCUT, 100, HELPCTX(kitty_named_proxies),
                 pxp_str_handler, I(CONF_proxy_telnet_command), ED_STR);
    ctrl_editbox(s, KT_NAMED_PROXIES_EXCLUDE_HOSTS_IPS_SEPARATE,
                 NO_SHORTCUT, 100, HELPCTX(kitty_named_proxies),
                 pxp_str_handler, I(CONF_proxy_exclude_list), ED_STR);
    ctrl_checkbox(s, KT_PROXY_CONSIDER_PROXYING_LOCAL_HOST_CONNECTIONS, NO_SHORTCUT,
                  HELPCTX(kitty_named_proxies), pxp_bool_handler, I(CONF_even_proxy_localhost));
    /* DNS lookup and Diagnostics side by side, half the panel each - two
     * short labels with their drop-downs right beside them, one line for
     * what used to take two. */
    ctrl_columns(s, 2, 50, 50);
    c = ctrl_droplist(s, KT_NAMED_PROXIES_DNS_LOOKUP_AT_PROXY, NO_SHORTCUT, 55,
                      HELPCTX(kitty_named_proxies), pxp_list_handler,
                      I(CONF_proxy_dns));
    c->column = 0;
    c = ctrl_droplist(s, KT_NAMED_PROXIES_PRINT_DIAGNOSTICS, NO_SHORTCUT, 55,
                      HELPCTX(kitty_named_proxies), pxp_list_handler,
                      I(CONF_proxy_log_to_term));
    c->column = 1;
    ctrl_columns(s, 1, 100);

    s = ctrl_getset(b, "Application/Named proxies", "act", NULL);
    pd->banner = ctrl_text(s, KT_NAMED_PROXIES_NOTHING_IS_STORED_UNTIL_SAVE, HELPCTX(kitty_named_proxies));
    ctrl_columns(s, 2, 50, 50);
    c = ctrl_pushbutton(s, KT_SESSION_SAVE, NO_SHORTCUT, HELPCTX(kitty_named_proxies),
                        pxp_save_handler, P(NULL));
    c->column = 0;
    c = ctrl_pushbutton(s, KT_SESSION_DELETE, NO_SHORTCUT, HELPCTX(kitty_named_proxies),
                        pxp_delete_handler, P(NULL));
    c->column = 1;
    ctrl_columns(s, 1, 100);
}

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

