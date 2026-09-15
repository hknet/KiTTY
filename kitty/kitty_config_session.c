/*
 * kitty_config_session.c - the Session tab of the configuration box: the session list with
 * its folders and filter, the per-session handlers, the host-key scan, and
 * the builders of the Session, Terminal, Window and Connection panels.
 * Connection/Proxy also emits Application/Workplace Proxy.
 */
#include <assert.h>
#include <stdlib.h>
#include "putty.h"
#include "dialog.h"
#include "storage.h"
#include "tree234.h"
#include "ssh.h"     /* KiTTY: ppk_loadpub_f + ssh2_fingerprint_blob (key pin) */
#ifndef MOD_NOREPEAT
#define MOD_NOREPEAT 0x4000
#endif
#include "kitty_proxy.h"   /* proxy-choice droplist: proxies[], GetProxySelectionFlag, MAX_PROXY */
#include "kitty_workplace.h"  /* workplace proxy mode: query/request the arming */
#include "kitty_defs.h"    /* KITTY_DEFAULT_SESSION */
#include "kitty_win.h"   /* SetTextToClipboard */
#include <limits.h>
#include "kitty_theme.h"   /* the app-wide colour theme, for Application > Config Window */
#include "kitty_storage.h" /* the one-time old-sessions notice bits */
#include "kitty_migrate.h" /* Application > Migration: the session importer */
#include "kitty_text.h"    /* the words the panels show */
#include "kitty_inikeys.h" /* KI_*: the kitty.ini key names */
#include "kitty_notes.h"   /* the application notification: its escapes and its notice */
#include "kitty_oldwin.h"   /* record what an older Windows does not have */
#include "kitty_winpos.h"   /* the remembered window position, per session and monitor layout */
#include "kitty_commun.h"
#include "kitty.h"
#include "kitty_launcher.h"
#include "kitty_tools.h"
#include "kitty_gui.h"
#include "mini/mini.h"
#include "kitty_bridge.h"
#include "kitty_registry.h"
#include "kitty_userpath.h"
#include "kitty_storemove.h"
#include "kitty_mpw.h"
#include "kitty_config.h"
#include "kitty_msgbox.h"   /* themed MessageBox routing */
#include "kitty_pwmem.h"    /* passwords wrapped in memory */
#include <commctrl.h>       /* SetWindowSubclass: the shortcut editor's key-capture field */
#include "kitty_hostkeys.h"
#include "kitty_hostkey_verify.h"
#include "kitty_config_int.h"   /* what the kitty_config_*.c files share */

#define KITTY_WORKPLACE_BOX_TITLE "Workplace proxy mode"
#define DISPLAY_RECONFIGURABLE_PROTOCOL(which_proto) \
    (backend_vt_from_proto(which_proto) && \
     (!midsession || protocol == (which_proto)))
#define DISPLAY_NON_RECONFIGURABLE_PROTOCOL(which_proto) \
    (backend_vt_from_proto(which_proto) && !midsession)

/* KiTTY: "Remember window position" keeps one entry per session and monitor
 * layout (kitty_winpos.c). With it on, the Position panel's Top/Left and the
 * Window panel's Columns/Rows show and edit that entry for the layout the
 * configuration box is on now: Load copies the entry into the conf, Save
 * writes the conf's values back into it. */
static void kitty_winpos_load_into_conf(const char *session, Conf *conf)
{
    struct kitty_termpos pos;
    if (!conf_get_bool(conf, CONF_remember_winpos)) return;
    if (!kitty_winpos_session_get(session, kitty_winpos_layout_hash(), &pos))
        return;
    conf_set_int(conf, CONF_xpos, pos.left);
    conf_set_int(conf, CONF_ypos, pos.top);
    if (pos.cols > 0 && pos.rows > 0) {
        conf_set_int(conf, CONF_width, pos.cols);
        conf_set_int(conf, CONF_height, pos.rows);
    }
}
static void kitty_winpos_save_from_conf(const char *session, Conf *conf)
{
    struct kitty_termpos pos;
    if (!conf_get_bool(conf, CONF_remember_winpos)) return;
    if (kitty_winpos_is_shared_window(session)) return;
    pos.left = conf_get_int(conf, CONF_xpos);
    pos.top = conf_get_int(conf, CONF_ypos);
    pos.cols = conf_get_int(conf, CONF_width);
    pos.rows = conf_get_int(conf, CONF_height);
    kitty_winpos_session_set(session, kitty_winpos_layout_hash(), &pos);
}

/* Auto-login password editbox handler. Behaves like the stock ED_STR editbox,
 * but the first time the field is made non-empty in a dialog session it shows
 * a one-time security consent (the password is stored reversibly-encrypted; SSH
 * keys are recommended). Declining clears the field. Consent happens HERE, at
 * configuration time, so the auto-login itself stays silent at connect time. */
/*
 * The login script box: show the script as lines, store it protected.
 *
 * The conversions live in kitty.c next to ReadInitScript, so the legacy decode -
 * and with it the compiled-in constant being retired - stays in one file.
 *
 * EVENT_REFRESH decrypts for display; EVENT_VALCHANGE re-wraps what was typed.
 * That is the same shape the auto-login password below uses: the user handles
 * plaintext and the protection is invisible, rather than the raw stored value
 * being put in front of them to edit by hand.
 */

/* The login-script box, captured at build time so the "Load from file..." button
 * beside it can fill it in. Same trick as g_autopw_ctrl below. */
static dlgcontrol *g_loginscript_ctrl = NULL;

static void kitty_loginscript_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                      void *data, int event)
{
    Conf *conf = (Conf *)data;
    if (event == EVENT_REFRESH) {
        char *txt = kitty_loginscript_to_text(
            conf_get_str(conf, CONF_scriptfilecontent));
        dlg_editbox_set(ctrl, dlg, txt ? txt : "");
        if (txt) { smemclr(txt, strlen(txt)); sfree(txt); }
    } else if (event == EVENT_VALCHANGE) {
        char *txt = dlg_editbox_get(ctrl, dlg);
        char *stored = kitty_loginscript_from_text(txt ? txt : "");
        conf_set_str(conf, CONF_scriptfilecontent, stored ? stored : "");
        if (stored) sfree(stored);
        if (txt) { smemclr(txt, strlen(txt)); sfree(txt); }
    }
}

/*
 * "Load from file..." beside the login-script box.
 *
 * Classic KiTTY had a file picker that read the script, inlined it into the
 * session and then cleared itself. This port had bound that picker to the wrong
 * setting entirely - CONF_scriptfile, which belongs to the rutty scripting - so
 * it silently changed a different feature and never loaded anything. This
 * replaces it honestly: read the file INTO THE BOX, and touch nothing else.
 *
 * It deliberately does not arm the running session or write CONF_scriptfile. The
 * file is a source of text, nothing more; what is saved with the session is
 * whatever ends up in the box, which the user can still read and edit before
 * saving. Setting the box fires the normal VALCHANGE, so the value is protected
 * and stored by the ordinary path.
 */
static void kitty_loginscript_load_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                           void *data, int event)
{
    if (event != EVENT_ACTION)
        return;
    if (!g_loginscript_ctrl) {
        dlg_error_msg(dlg, KT_CFG_LOGINSCRIPT_NO_BOX);
        return;
    }
    {
        char path[4096];
        path[0] = '\0';
        if (!OpenFileName(GetActiveWindow(), path,
                          KT_CFG_LOGINSCRIPT_SELECT,
                          "Script files (*.txt;*.ksc)|*.txt;*.ksc|All files (*.*)|*.*|"))
            return;                       /* cancelled */
        {
            FILE *fp = fopen(path, "rb");
            strbuf *sb;
            char line[4096];
            if (!fp) {
                dlg_error_msg(dlg, KT_CFG_LOGINSCRIPT_OPEN_FAILED);
                return;
            }
            sb = strbuf_new_nm();
            while (fgets(line, sizeof(line), fp)) {
                size_t n = strlen(line);
                while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r'))
                    line[--n] = '\0';
                /* Blank lines are dropped here as well as on the way to storage:
                 * an empty wait-for entry matches ANY output and would fire the
                 * next send immediately. Better to never show one. */
                if (n == 0)
                    continue;
                if (sb->len)
                    put_dataz(sb, "\r\n");
                put_dataz(sb, line);
            }
            fclose(fp);
            {
                char *txt = strbuf_to_str(sb);
                dlg_editbox_set(g_loginscript_ctrl, dlg, txt);
                smemclr(txt, strlen(txt));
                sfree(txt);
            }
        }
    }
}

static void kitty_autopw_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                 void *data, int event)
{
    Conf *conf = (Conf *)data;
    if (event == EVENT_REFRESH) {
        /* CONF_password is kept UTF-8 (the SSH password prompt is UTF-8, and the
         * storage layer normalises legacy values to UTF-8 on load), so display
         * and read the field as UTF-8 rather than the system codepage.
         * The Conf holds the value WRAPPED (kitty_pwmem.c): the plaintext
         * exists only in this buffer, burned before the handler returns. */
        char pw[KITTY_PW_MAX + 1];
        kitty_pw_get(conf, CONF_password, pw, sizeof(pw));
        dlg_editbox_set_utf8(ctrl, dlg, pw);
        smemclr(pw, sizeof(pw));
    } else if (event == EVENT_VALCHANGE) {
        char *val = dlg_editbox_get_utf8(ctrl, dlg);
        /* Warn only when a password is being SET where conf currently has none
         * (i.e. a genuinely new auto-login password). Editing a session that
         * already has a stored password leaves conf non-empty, so no warning -
         * this also covers the re-entrant VALCHANGE that dlg_editbox_set fires
         * during EVENT_REFRESH (conf already holds the loaded password then). */
        if (strlen(val) > 0 && kitty_pw_empty(conf, CONF_password)) {
            if (!kitty_autopw_warn()) {
                /* Declined: clear the field and do not store. */
                dlg_editbox_set(ctrl, dlg, "");
                conf_set_str(conf, CONF_password, "");
                smemclr(val, strlen(val));
                sfree(val);
                return;
            }
        }
        kitty_pw_set_burn(conf, CONF_password, val);
        sfree(val);
    }
}

/*
 * A password edit box bound to a Conf key, for the panels built in this file.
 *
 * The stock conf_editbox_handler cannot serve one: a password key does not
 * hold the password but the wrapped form (kitty_pwmem.h), which the field
 * would show as a blob and store back as the user's password. A private copy
 * rather than a shared helper - config.c has the same one - because the two
 * files are alternative implementations of one dialog and are never linked
 * together, and anything they could share would have to sit in the GUI
 * library that the command-line tools do not link.
 */
static void kitty_proxypw_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                  void *data, int event)
{
    Conf *conf = (Conf *)data;
    int key = ctrl->context.i;

    if (event == EVENT_REFRESH) {
        char pw[KITTY_PW_MAX + 1];
        kitty_pw_get(conf, key, pw, sizeof(pw));
        dlg_editbox_set(ctrl, dlg, pw);
        smemclr(pw, sizeof(pw));
    } else if (event == EVENT_VALCHANGE) {
        char *val = dlg_editbox_get(ctrl, dlg);
        kitty_pw_set_burn(conf, key, val);
        sfree(val);
    }
}

/* "Show password" checkbox: unmasks a password editbox so the user can read
 * back what is stored. The checkbox's context holds the ADDRESS of the
 * variable that holds its editbox, not the editbox itself: the panels are
 * built in whatever order the tree is walked, and a control captured at build
 * time is null until its own panel has been built. Going through the variable
 * means the checkbox always finds the field it belongs to, and one handler
 * serves every password field instead of a copy per panel.
 *
 * Always starts masked when a panel is (re)opened, whatever it was last time -
 * a stored password should not appear on screen because the box happens to
 * reopen on that panel. */
static dlgcontrol *g_autopw_ctrl = NULL;    /* Connection/Login auto-login */
static dlgcontrol *g_proxypw_ctrl = NULL;   /* Connection/Proxy */
static void kitty_showpw_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                 void *data, int event)
{
    dlgcontrol **field = (dlgcontrol **)ctrl->context.p;
    if (!field || !*field)
        return;
    if (event == EVENT_REFRESH) {
        dlg_checkbox_set(ctrl, dlg, false);
        dlg_editbox_set_masked(*field, dlg, false);
    } else if (event == EVENT_VALCHANGE) {
        dlg_editbox_set_masked(*field, dlg, dlg_checkbox_get(ctrl, dlg));
    }
}

/* KSCP panel: OSC 7 cwd tracking and a fixed remote upload directory are two
 * mutually exclusive ways to choose the drag-drop / WinSCP target. PuTTY's
 * dialog API has no primitive to grey a control, so exclusivity is enforced by
 * auto-toggling: ticking OSC 7 clears the fixed dir, and typing a fixed dir
 * unticks OSC 7. The sibling controls are captured when the panel is built. */
static dlgcontrol *g_osc7_track_ctrl = NULL;
static dlgcontrol *g_pscp_remotedir_ctrl = NULL;

static void kitty_osc7_track_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                     void *data, int event)
{
    Conf *conf = (Conf *)data;
    if (event == EVENT_REFRESH) {
        dlg_checkbox_set(ctrl, dlg, conf_get_bool(conf, CONF_osc7_cwd_tracking));
    } else if (event == EVENT_VALCHANGE) {
        bool on = dlg_checkbox_get(ctrl, dlg);
        conf_set_bool(conf, CONF_osc7_cwd_tracking, on);
        if (on && g_pscp_remotedir_ctrl) {
            /* switching to auto-tracking retires any fixed directory */
            conf_set_str(conf, CONF_pscpremotedir, "");
            dlg_editbox_set(g_pscp_remotedir_ctrl, dlg, "");
        }
    }
}

static void kitty_pscp_remotedir_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                         void *data, int event)
{
    Conf *conf = (Conf *)data;
    if (event == EVENT_REFRESH) {
        dlg_editbox_set(ctrl, dlg, conf_get_str(conf, CONF_pscpremotedir));
    } else if (event == EVENT_VALCHANGE) {
        char *s = dlg_editbox_get(ctrl, dlg);
        conf_set_str(conf, CONF_pscpremotedir, s);
        if (*s && g_osc7_track_ctrl) {
            /* a fixed directory and OSC 7 tracking are mutually exclusive */
            conf_set_bool(conf, CONF_osc7_cwd_tracking, false);
            dlg_checkbox_set(g_osc7_track_ctrl, dlg, false);
        }
        sfree(s);
    }
}

#ifdef MOD_LAUNCHER
/* Shared launcher-hotkey helpers (kitty_bridge.c; kitty_config.c does not
 * include kitty.h). The parser is the SAME one the launcher registers with,
 * so what this file warns about is what the launcher will actually do. */
extern int kitty_hotkey_conflict_scan(unsigned int mods, unsigned int vk,
                                      const char *exclude, char *names, int nameslen);

/* KiTTY: record the configured key file's SHA256 fingerprint as this
 * session's pin (CONF_publickey_fingerprint). Reads the PUBLIC half only -
 * no passphrase is involved. The check itself runs at connect time
 * (ssh/userauth2-client.c), before any offer or passphrase prompt. */
static void kitty_keyfile_pin_record_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                             void *data, int event)
{
    Conf *conf = (Conf *)data;
    Filename *kf;
    strbuf *blob;
    char *alg = NULL, *comment = NULL, *full, *m;
    const char *error = NULL, *bare;
    (void)ctrl;
    if (event != EVENT_ACTION) return;
    kf = conf_get_filename(conf, CONF_keyfile);
    if (!kf || filename_is_null(kf)) {
        MessageBox(GetActiveWindow(),
                   KT_CFG_PIN_CHOOSE_KEY_FIRST,
                   KT_CAP_KEY_FINGERPRINT_PIN, MB_OK | MB_ICONWARNING);
        return;
    }
    blob = strbuf_new();
    if (!ppk_loadpub_f(kf, &alg, BinarySink_UPCAST(blob), &comment, &error)) {
        m = dupprintf(KT_CFG_PIN_READ_FAILED,
                      error ? error : KT_MSG_UNKNOWN_ERROR);
        MessageBox(kitty_cfg_modal_owner(), m, KT_CAP_KEY_FINGERPRINT_PIN,
                   MB_OK | MB_ICONWARNING);
        sfree(m);
        strbuf_free(blob);
        return;
    }
    full = ssh2_fingerprint_blob(ptrlen_from_strbuf(blob), SSH_FPTYPE_SHA256);
    /* Store the bare "SHA256:..." token; the connect-time compare accepts
     * either form, but the field stays short and copyable this way. */
    bare = strstr(full, "SHA256:");
    if (!bare) bare = full;
    conf_set_str(conf, CONF_publickey_fingerprint, bare);
    /* Only the fingerprint box (the button's context): dlg_refresh(NULL)
     * redrew every control of the panel, which read as the configuration
      * window reloading. */
    dlg_refresh((dlgcontrol *)ctrl->context.p, dlg);
    m = dupprintf(KT_CFG_PIN_RECORDED, full);
    MessageBox(GetActiveWindow(), m, KT_CAP_KEY_FINGERPRINT_PIN,
               MB_OK | MB_ICONINFORMATION);
    sfree(m);
    sfree(full);
    sfree(alg);
    sfree(comment);
    strbuf_free(blob);
}

/* KiTTY: open the modeless window-title placeholder reference (kitty_win.c).
 * Owned by the active window - the configuration box - so it stacks with it
 * rather than getting lost behind it. */
static void kitty_title_placeholders_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                             void *data, int event)
{
    (void)ctrl; (void)dlg; (void)data;
    if (event != EVENT_ACTION) return;
    kitty_show_title_placeholders(GetActiveWindow());
}

static void kitty_launcher_hotkey_check_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                                void *data, int event)
{
    Conf *conf = (Conf *)data;
    unsigned int mods, vk;
    char others[512], msg[900];
    const char *self;
    int nc;
    /* The suite's own info box, not MessageBox - same face as every other
     * KiTTY window. */
    extern void kitty_info_box(HWND, const char *, const char *,
                               const char *); /* kitty_win.c */
    (void)ctrl; (void)dlg;
    if (event != EVENT_ACTION) return;
    if (!kitty_parse_hotkey_spec(conf_get_str(conf, CONF_launcher_global_hotkey),
                                 &mods, &vk)) {
        kitty_info_box(kitty_cfg_modal_owner(), KT_CAP_LAUNCHER_HOTKEY,
                       KT_CFG_HOTKEY_ENTER,
                       NULL);
        return;
    }
    /* The system-wide probe below cannot see WHICH saved session holds a
     * hotkey (a running launcher registers them under its own window), but the
     * store can: name the sessions here, where the collision is being made. */
    self = conf_get_str(conf, CONF_sessionname);
    nc = kitty_hotkey_conflict_scan(mods, vk, (self && *self) ? self : NULL,
                                    others, sizeof(others));
    if (RegisterHotKey(NULL, 0x4B7A, mods | MOD_NOREPEAT, vk)) {
        UnregisterHotKey(NULL, 0x4B7A);
        if (nc > 0) {
            snprintf(msg, sizeof(msg),
                     KT_CFG_HOTKEY_FREE_BUT_SAVED,
                     nc == 1 ? "" : "s", others);
            kitty_info_box(kitty_cfg_modal_owner(), KT_CAP_LAUNCHER_HOTKEY,
                           msg, NULL);
        } else {
            kitty_info_box(kitty_cfg_modal_owner(), KT_CAP_LAUNCHER_HOTKEY,
                           KT_CFG_HOTKEY_FREE,
                           NULL);
        }
    } else if (nc > 0) {
        snprintf(msg, sizeof(msg),
                 KT_CFG_HOTKEY_IN_USE_SAVED,
                 nc == 1 ? "" : "s", others);
        kitty_info_box(kitty_cfg_modal_owner(), KT_CAP_LAUNCHER_HOTKEY,
                       msg, NULL);
    } else {
        kitty_info_box(kitty_cfg_modal_owner(), KT_CAP_LAUNCHER_HOTKEY,
                       KT_CFG_HOTKEY_IN_USE_SYSTEM,
                       NULL);
    }
}
#endif

/* Proxy-choice droplist (KiTTY): lists named proxy definitions (plus the two
 * built-ins KITTY_PROXY_SESSION / KITTY_PROXY_NONE) and stores the chosen
 * name in CONF_proxyselection, which kitty_proxy_select() overlays onto the
 * session's proxy settings at connect time.
 *
 * WARNING: That overlay is an OPEN BUG, not a design to build on: it writes the
 * session's Proxy* fields, so a preset can destroy proxy credentials that exist
 * only in the session. Do not "fix" it by writing the fields here either - that
 * is the same data loss, moved earlier. */
/*
 * Does the session itself carry proxy settings? That is what the control's
 * neutral position is derived from, and what "would this change anything?" is
 * measured against.
 */
static bool kitty_session_has_proxy(Conf *conf)
{
    return conf_get_int(conf, CONF_proxy_type) != PROXY_NONE;
}

/* The neutral entry for this session: the one that changes nothing. */
static const char *kitty_proxy_neutral(Conf *conf)
{
    return kitty_session_has_proxy(conf) ? KITTY_PROXY_SESSION
                                         : KITTY_PROXY_NONE;
}

/* Line spacing: an ordinary integer editbox, plus a label that says so when the
 * number typed into it is outside the range the terminal will actually honour.
 * The clamp lives in init_fonts (windows/window.c) and happens regardless; this
 * is only so the box does not sit there showing 900 as though 900 were in use.
 * Repainting the label is what makes the colour follow, since dialog.c picks the
 * colour from the label's text. */
static void kitty_linespacing_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                      void *data, int event)
{
    conf_editbox_handler(ctrl, dlg, data, event);

    if (event == EVENT_REFRESH || event == EVENT_VALCHANGE) {
        int v = conf_get_int((Conf *)data, CONF_line_spacing);
        bool bad = (v < 100 || v > 300);
        if (bad != g_linespc_out_of_range) {
            g_linespc_out_of_range = bad;
            /* Same text, deliberately: SetWindowText repaints the static either
             * way, and the repaint is the whole point - the colour is decided in
             * dialog.c's WM_CTLCOLORSTATIC, which only runs on a paint. */
            dlg_label_change(ctrl, dlg, KITTY_LINESPC_LABEL);
        }
    }
}

static const char *kitty_proxy_override_label(Conf *conf)
{
    const char *cur = conf_get_str(conf, CONF_proxyselection);

    if (!cur || !*cur)
        return KT_SESSION_PROXY_LABEL_IDLE;

    /*
     * Compare EFFECTS, not strings.
     *
     * Comparing the choice against the neutral entry looked right and was wrong:
     * on a session with NO proxy, "Session defined proxy" is a different string
     * from the neutral "No proxy" while doing exactly the same nothing - and the
     * caption then claimed an override was active when none was.
     *
     *   "Session defined proxy" -> use whatever the session has. Never an
     *                              override, whether that is a proxy or nothing.
     *   "No proxy"              -> an override ONLY if the session has a proxy,
     *                              because then it suppresses it. On a session
     *                              without one it changes nothing.
     *   a named proxy           -> always an override.
     */
    if (!strcmp(cur, KITTY_PROXY_SESSION))
        return KT_SESSION_PROXY_LABEL_IDLE;
    if (!strcmp(cur, KITTY_PROXY_NONE))
        return kitty_session_has_proxy(conf) ? KITTY_PROXY_LABEL_ACTIVE
                                             : KT_SESSION_PROXY_LABEL_IDLE;
    return KITTY_PROXY_LABEL_ACTIVE;
}

/*
 * Proxy-override droplist (KiTTY): the named proxy definitions plus the two
 * built-ins, applied to THIS CONNECTION ONLY (kitty_proxy_select() in
 * kitty_bridge.c hands it to a throwaway Conf copy; it never writes the session).
 *
 * WARNING: The control STARTS NEUTRAL every time the box opens, derived from the
 * session's own proxy settings, and a value stored in the session cannot preselect
 * it. That is deliberate: a remembered override is indistinguishable from a
 * setting, and honouring it here while a double-click on the session list ignores
 * it would mean the same session connecting differently depending on how it was
 * started. Nothing invisible decides behaviour.
 */
struct pxchoice_data { bool picked; };

/*
 * The live override control of the current config box, so that LOADING a session
 * can put it back to neutral.
 *
 * `picked` means "the user chose something in this box", and it must be scoped to
 * the SESSION, not to the box. It was scoped to the box at first, and that was the
 * bug: after touching the droplist once, loading another session no longer
 * re-derived the neutral value, so a stale "No proxy" survived onto a session that
 * did have a proxy - and the caption then correctly, and confusingly, called that
 * an active override.
 *
 * Same single-instance pattern as session_filter_ssd above: cleared when the
 * config box's saved-session data is freed, so it cannot dangle.
 */
static struct pxchoice_data *pxchoice_state = NULL;

/* Put the override back to "changes nothing" for whatever conf is now loaded. */
static void kitty_proxy_override_reset(Conf *conf)
{
    if (pxchoice_state)
        pxchoice_state->picked = false;
    conf_set_str(conf, CONF_proxyselection, kitty_proxy_neutral(conf));
}

static void kitty_proxy_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                void *data, int event)
{
    Conf *conf = (Conf *)data;
    struct pxchoice_data *pc = (struct pxchoice_data *)ctrl->context.p;
    if (event == EVENT_REFRESH) {
        const char *cur;
        int i, sel = 0;
        kitty_proxy_resolve_selection(conf);   /* drop a deleted proxy ref, etc. */
        /* Until the user picks something in THIS box, the control shows the
         * session's own state - never a value the session remembered. */
        if (pc && !pc->picked)
            conf_set_str(conf, CONF_proxyselection, kitty_proxy_neutral(conf));
        cur = conf_get_str(conf, CONF_proxyselection);
        dlg_update_start(ctrl, dlg);
        dlg_listbox_clear(ctrl, dlg);
        for (i = 0; i < MAX_PROXY && proxies[i].name; i++) {
            dlg_listbox_add(ctrl, dlg, proxies[i].name);
            if (cur && !strcmp(cur, proxies[i].name)) sel = i;
        }
        dlg_listbox_select(ctrl, dlg, sel);
        dlg_update_done(ctrl, dlg);
        dlg_label_change(ctrl, dlg, kitty_proxy_override_label(conf));
    } else if (event == EVENT_SELCHANGE) {
        int i = dlg_listbox_index(ctrl, dlg);
        if (i >= 0 && i < MAX_PROXY && proxies[i].name) {
            if (pc) pc->picked = true;
            conf_set_str(conf, CONF_proxyselection, proxies[i].name);
            dlg_label_change(ctrl, dlg, kitty_proxy_override_label(conf));
        }
    }
}

/*
 * Connection/Proxy: "Named proxy settings:" + Load into this window.
 *
 * The DELIBERATE way to make a named proxy permanent for a session, and the
 * counterpart to the Session-panel override, which must never write the session
 * (see kitty_proxy_select() in kitty_bridge.c). Without this there would be no way
 * to adopt a preset at all; with it, adopting one is an explicit act with a
 * confirmation, rather than a side effect of connecting.
 *
 * Writes the preset's WHOLE set - method, host, port, username, password, exclude
 * list, DNS, telnet command - because a half-loaded proxy is one that cannot
 * authenticate. That includes clearing the password when the preset has none,
 * which is exactly why it is confirmed every time.
 *
 * Nothing is stored until the session is saved, hence the wording "this window".
 */
struct pxload_data {
    char *name;
    dlgcontrol *list;
    dlgcontrol *button;
    /* the pin: where the LAYOUT put the loader (recorded on the first pin
     * after a build), so a later resize can move it down OR back up */
    int natural_y[3], natural_bottom, have_natural;
};

/* The live box's pre-set loader, for the pin below; set whenever the Proxy
 * panel is built, so it never outlives the controls it names. */
static struct pxload_data *kitty_pxload_active = NULL;

/*
 * Map a row of the load droplist back to a proxy name.
 *
 * The list omits the two built-ins (KITTY_PROXY_NONE, KITTY_PROXY_SESSION):
 * they are choices for the override, not definitions that can be loaded into a
 * session. So row N is NOT proxies[N] and the skip has to be repeated here.
 *
 * WARNING: Do NOT reach for dlg_editbox_get() to read the current text instead: this is
 * a DROPLIST, which has no edit field, and that call asserts
 * "c->ctrl->type == CTRL_EDITBOX" - it crashed the program with a runtime
 * assertion the first time this was written that way.
 */
const char *kitty_pxload_name_at(int row)
{
    int i, n = 0;
    if (row < 0)
        return NULL;
    for (i = 0; i < MAX_PROXY && proxies[i].name; i++) {
        if (!strcmp(proxies[i].name, KITTY_PROXY_NONE) ||
            !strcmp(proxies[i].name, KITTY_PROXY_SESSION))
            continue;
        if (n++ == row)
            return proxies[i].name;
    }
    return NULL;
}

/* Confirm and write one pre-set into the live Conf. Shared by the panel's
 * inline loader below; nothing is stored until the session is saved. */
static void kitty_pxload_apply(dlgparam *dlg, Conf *conf, const char *picked)
{
    extern int kitty_confirm_box(HWND owner, const char *caption,
                                 const char *text, const char *warn_red); /* kitty_win.c */
    {
        char *q = dupprintf(
            KT_CFG_PXLOAD_Q,
            picked, picked);
        /* The one case where the above is not the whole truth. */
        const char *warn =
            conf_get_bool(conf, CONF_saveonexit)
            ? KT_CFG_PXLOAD_SAVEONEXIT_WARN
            : NULL;
        bool go = kitty_confirm_box(GetActiveWindow(),
                                    KT_CAP_LOAD_NAMED_PROXY, q, warn);
        sfree(q);
        if (!go)
            return;                     /* nothing touched at all */

        LoadProxyInfo(conf, picked);
        /* The fields ARE the preset now, so a remembered override naming it (or
         * naming something else) would only be able to disagree. Clear it back to
         * "use what the session says". */
        conf_set_str(conf, CONF_proxyselection, KITTY_PROXY_SESSION);
        dlg_refresh(NULL, dlg);         /* repaint the proxy fields we just wrote */
    }
}

/* The panel's own pre-set chooser: a droplist and a Load button at the FOOT
 * of the Proxy panel. It replaced a button that opened a separate picker
 * window - one more window for a one-row choice, and a window the theming
 * never reached. The droplist refills on every EVENT_REFRESH, so a pre-set
 * added or renamed in the Named-proxies editor is there when the user comes
 * back. */
static void kitty_pxload_inline_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                        void *data, int event)
{
    struct pxload_data *pd = (struct pxload_data *)ctrl->context.p;
    Conf *conf = (Conf *)data;

    if (ctrl == pd->list) {
        if (event == EVENT_REFRESH) {
            const char *cur = conf_get_str(conf, CONF_proxyselection);
            int i, sel = 0, n = 0;
            dlg_update_start(ctrl, dlg);
            dlg_listbox_clear(ctrl, dlg);
            for (i = 0; i < MAX_PROXY && proxies[i].name; i++) {
                if (!strcmp(proxies[i].name, KITTY_PROXY_NONE) ||
                    !strcmp(proxies[i].name, KITTY_PROXY_SESSION))
                    continue;
                dlg_listbox_add(ctrl, dlg, proxies[i].name);
                if (cur && !strcmp(cur, proxies[i].name))
                    sel = n;
                n++;
            }
            if (n)
                dlg_listbox_select(ctrl, dlg, sel);
            dlg_update_done(ctrl, dlg);
        }
        return;
    }

    if (event != EVENT_ACTION)          /* the Load button */
        return;
    {
        const char *name = kitty_pxload_name_at(dlg_listbox_index(pd->list, dlg));
        if (name)
            kitty_pxload_apply(dlg, conf, name);
    }
}

/*
 * KiTTY: the saved-session panel of the config box that is open, or NULL.
 *
 * Set as that panel's controls are declared and cleared with the dialog, the
 * same contract as kitty_cfg_session_filter_ctrl below - so it cannot dangle.
 * What needs it is the button column beside the session list: those buttons
 * are POSITIONED after the layout runs rather than spaced by blank rows, and
 * the placement has to be redone whenever the list's row count changes.
 */
struct sessionsaver_data *kitty_session_ssd = NULL;

/* The saved-session list's length in rows, clamped to something usable. */
int kitty_config_session_rows(void)
{
    extern int GetConfigBoxHeight(void);       /* kitty.c: [ConfigBox] height */
    int rows = GetConfigBoxHeight();
    if (rows < KITTY_CFG_SESSION_ROWS_MIN) rows = KITTY_CFG_SESSION_ROWS_MIN;
    if (rows > KITTY_CFG_SESSION_ROWS_MAX) rows = KITTY_CFG_SESSION_ROWS_MAX;
    return rows;
}

/*
 * Spread the buttons beside the saved-session list down the height of that
 * list, measured from the list itself once everything has been laid out.
 *
 * The rule is the same at any height: Load sits at the top and Delete +
 * Del folder are centred in what is left below it (Export/Import moved to
 * Application/Migration). Nothing here depends on the row count, the
 * font or the DPI - it reads the rectangles Windows actually produced.
 *
 * Called from windows/dialog.c after the panel is laid out and BEFORE it is
 * measured, since moving these buttons is what decides how tall the panel is.
 */
void kitty_config_session_distribute(void)
{
    struct sessionsaver_data *ssd = kitty_session_ssd;
    dlgcontrol *top[1], *mid[2], *bot[2];
    int ntop = 0, nmid = 0, nbot = 0;
    HWND hlist;
    RECT lr, br;
    int listtop, listbot, bh, gap, y, i;

    if (!ssd || !ssd->listbox)
        return;
    hlist = kitty_cfg_ctrl_hwnd(ssd->listbox);
    if (!hlist || !GetWindowRect(hlist, &lr))
        return;

    if (ssd->loadbutton)      top[ntop++] = ssd->loadbutton;
    if (ssd->delbutton)       mid[nmid++] = ssd->delbutton;
    if (ssd->delfolderbutton) mid[nmid++] = ssd->delfolderbutton;
    if (!ntop && !nmid && !nbot)
        return;                        /* mid-session, or PuTTY mode */

    /*
     * A button's height, and the gap the LAYOUT puts between two stacked
     * ones - both measured, neither guessed.
     *
     * The gap is read from the two buttons as the layout left them, before
     * anything here moves them: they are still stacked in declaration order
     * at this point, so the distance between the first two IS the engine's
     * own spacing. A constant would be wrong at the next font size, and a
     * fraction of the button height was wrong immediately - bh/3 came out at
     * 8 where the layout uses 4, which made the column half again as tall as
     * it needed to be and pushed it further past the foot of a short list
     * than the arrangement it replaced.
     */
    {
        dlgcontrol *first = ntop ? top[0] : (nmid ? mid[0] : bot[0]);
        dlgcontrol *second = (ntop && nmid) ? mid[0]
                           : (nmid > 1)     ? mid[1]
                           : (nmid && nbot) ? bot[0]
                           : (nbot > 1)     ? bot[1] : NULL;
        HWND h1 = kitty_cfg_ctrl_hwnd(first);
        if (!h1 || !GetWindowRect(h1, &br))
            return;
        bh = br.bottom - br.top;
        if (bh <= 0)
            return;
        gap = bh / 3;                          /* only if there is no second */
        if (second) {
            HWND h2 = kitty_cfg_ctrl_hwnd(second);
            RECT r2;
            if (h2 && GetWindowRect(h2, &r2) && r2.top > br.bottom)
                gap = (int)(r2.top - br.bottom);
        }
        if (gap < 2) gap = 2;
    }

    listtop = lr.top;
    listbot = lr.bottom;

    /* Positions are set in the panel host's client coordinates. */
    #define KCS_MOVE(ctrl, ytop)                                            \
        do {                                                                \
            HWND h = kitty_cfg_ctrl_hwnd(ctrl);                             \
            RECT r;                                                         \
            POINT pt;                                                       \
            if (h && GetWindowRect(h, &r)) {                                \
                pt.x = r.left; pt.y = (ytop);                               \
                ScreenToClient(GetParent(h), &pt);                          \
                SetWindowPos(h, NULL, pt.x, pt.y, 0, 0,                     \
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);   \
            }                                                               \
        } while (0)

    {
        int nall = ntop + nmid + nbot;
        int stack = nall * bh + (nall - 1) * gap;   /* all of them, touching */

        if (listbot - listtop < stack) {
            /*
             * Too short for the buttons that have to go beside it - five of
             * them need more than a very small list is tall. Stack them from
             * the top and let the column run past the list's foot: they
             * OVERLAP each other otherwise, which is the one outcome that is
             * unreadable rather than merely untidy. (The row count has a
             * floor, but a floor in ROWS cannot know the button height at
             * this font and DPI, so the check belongs here where both are
             * measured.)
             */
            y = listtop;
            for (i = 0; i < ntop; i++, y += bh + gap) KCS_MOVE(top[i], y);
            for (i = 0; i < nmid; i++, y += bh + gap) KCS_MOVE(mid[i], y);
            for (i = 0; i < nbot; i++, y += bh + gap) KCS_MOVE(bot[i], y);
        } else {
            int above, below, block, start;

            for (i = 0, y = listtop; i < ntop; i++, y += bh + gap)
                KCS_MOVE(top[i], y);

            for (i = 0, y = listbot - (nbot * bh + (nbot - 1) * gap);
                 i < nbot; i++, y += bh + gap)
                KCS_MOVE(bot[i], y);

            if (nmid) {
                above = listtop + (ntop ? ntop * (bh + gap) : 0);
                below = listbot - (nbot ? nbot * bh + (nbot - 1) * gap + gap
                                        : 0);
                block = nmid * bh + (nmid - 1) * gap;
                start = above + ((below - above) - block) / 3;  /* upper-centre */
                /* Clamped against BOTH neighbours, in that order: the bottom
                 * group is flush with the list foot and must not be walked
                 * into, and the top one is fixed. */
                if (start + block > below)
                    start = below - block;
                if (start < above)
                    start = above;
                for (i = 0, y = start; i < nmid; i++, y += bh + gap)
                    KCS_MOVE(mid[i], y);
            }
        }
    }
    #undef KCS_MOVE
}

/* KiTTY: the Session panel's name/search box of the current config dialog.
 * windows/dialog.c's Ctrl+F jump focuses it (and selects its content) from
 * any panel; registered when the panel is built, cleared with the dialog so
 * the pointer can never dangle. Stock variants get a NULL stub instead
 * (windows/kitty_config_stubs.c). */
static dlgcontrol *session_filter_ctrl = NULL;
dlgcontrol *kitty_config_session_filter_ctrl(void)
{
    return session_filter_ctrl;
}

/* KiTTY (hknet/KiTTY#23): the same dialog's "Host Name (or IP address)" box.
 * With [ConfigBox] loadlastsession=no the caret starts here rather than in the
 * saved-session name box, so a host can be typed and opened without touching
 * the mouse. Registered when the Session panel is built; the panel is rebuilt
 * on every treeview switch, so the pointer is only ever used while that panel
 * is the live one. */
static dlgcontrol *quickconnect_host_ctrl = NULL;

/* KiTTY: the same dialog's session-saver data, for the Ctrl+G "search
 * everywhere" jump (windows/dialog.c). Registered and cleared together with
 * session_filter_ctrl above, so it can never outlive the dialog. */
struct sessionsaver_data *session_filter_ssd = NULL;

/* [ConfigBox] foldernavigation - defined with the other folder-row helpers
 * below, but needed here by Ctrl+G, which sits above them. */
static bool kitty_folder_rows_on(void);
static bool kitty_folder_rows_active(struct sessionsaver_data *ssd);

/*
 * Ctrl+G: drop the folder filter back to the root list, then let the caller
 * run the ordinary Ctrl+F jump. Ctrl+F alone searches within whatever folder
 * is currently selected, so finding a session in another folder otherwise
 * means changing the folder combo by hand first.
 *
 * Only the filter moves: no session is loaded, re-filed or saved. Returns
 * true if the folder actually changed, so the caller can tell "went global"
 * from "was already global".
 */
bool kitty_config_select_root_folder(dlgparam *dp)
{
    struct sessionsaver_data *ssd = session_filter_ssd;
    /* The combo is not required: folder navigation removes it and steers by the
     * list instead, and Ctrl+G has to keep working there - it is the one route
     * back to the root that does not depend on any particular control being on
     * screen. Only the LIST is genuinely needed. */
    if (!ssd || GetPuttyFlag() || !ssd->listbox)
        return false;
    /* Mid-session with folder rows configured there is NO folder control at
     * all - no combo (rows replaced it) and no rows (they navigate nothing
     * here) - so this had nothing to steer and moved the ambient folder
     * instead: Ctrl+G set CurrentFolder to the root, and the next Save re-filed
     * the running session there. A search must never move a session. */
    if (!ssd->folderlist && !kitty_folder_rows_active(ssd))
        return false;
    /* Arm the cross-folder search BEFORE the early-out below. With folder rows
     * the root level is not "everything" any more - it is the unfiled sessions
     * - so being at the root already is no reason to do nothing: that is
     * exactly when Ctrl+G still has work to do. */
    ssd->search_all = 1;
    /* A fresh Ctrl+G is a deliberate re-entry into the search box: whatever
     * wandering the focus did before it must not count as having left. */
    ssd->search_left = 0;
    if (!strcmp(CurrentFolder, "Default")) {
        if (ssd->listbox && dlg_is_visible(ssd->listbox, dp))
            dlg_refresh(ssd->listbox, dp);
        return false;                    /* already at the top; nothing moved */
    }
    strcpy(CurrentFolder, "Default");
    kitty_set_last_folder(CurrentFolder);
    /* dlg_refresh rebuilds the combo and re-selects the row matching
     * CurrentFolder, suppressing the re-entrant VALCHANGE itself.
     *
     * Only when those controls are on screen, though. Ctrl+G comes from a
     * keyboard hook that is live on every panel, and the config box destroys
     * the controls of every panel but the visible one: dlg_refresh() itself
     * tolerates that (it calls the handler directly), but the handler goes on
     * to dlg_update_start()/dlg_listbox_clear(), which assert on a control
     * they cannot find. Skipping the refresh loses nothing - the panel
     * rebuilds from CurrentFolder when it is next shown.
     *
     * WARNING: And only when the control EXISTS: folder navigation builds no combo at
     * all, and dlg_is_visible(NULL) is not a question the dialog layer can
     * answer - it looks the control up in a tree234 and asserts. That fired as a
     * runtime assertion box the first time Ctrl+G was pressed in that mode, and
     * took the list refresh below down with it. */
    if (ssd->folderlist && dlg_is_visible(ssd->folderlist, dp))
        dlg_refresh(ssd->folderlist, dp);
    if (ssd->listbox && dlg_is_visible(ssd->listbox, dp))
        dlg_refresh(ssd->listbox, dp);
    return true;
}

/*
 * Focus tracking for the Ctrl+G cross-folder search - called by
 * winctrl_set_focus() (windows/controls.c) whenever any config-box control
 * gains the keyboard focus.
 *
 * The search stays armed while focus stays within the search box and the
 * session list: correcting the search text (even deleting all of it) and
 * picking a result are part of the search. Focus landing anywhere else marks
 * the search as LEFT but changes nothing yet - the list still shows
 * cross-folder rows, and disarming under them would remap rows to the wrong
 * sessions (a Load click gains focus too, and must act on the row that was
 * picked). The disarm happens when focus comes BACK to the search box: the
 * old search is over, the filter is dropped and the list rebuilt for the
 * current level, so what is shown and how it maps can never disagree.
 *
 * Reaches controls.c through the kitty_ctrl_focus_hook pointer, planted when
 * the session panel is built and cleared with it - controls.c also links into
 * binaries with no config code, which must not need a stub for this.
 */
static void kitty_config_ctrl_focus_gained(dlgcontrol *ctrl, dlgparam *dlg)
{
    struct sessionsaver_data *ssd = session_filter_ssd;
    if (!ssd || !ssd->search_all)
        return;
    if (ctrl == ssd->listbox)
        return;
    if (ctrl != ssd->editbox) {
        ssd->search_left = 1;
        return;
    }
    if (!ssd->search_left)
        return;
    ssd->search_all = 0;
    ssd->search_left = 0;
    sfree(ssd->searchfilter);
    ssd->searchfilter = dupstr("");
    if (ssd->listbox && dlg_is_visible(ssd->listbox, dlg))
        dlg_refresh(ssd->listbox, dlg);
    if (ssd->commentbox && dlg_is_visible(ssd->commentbox, dlg))
        dlg_refresh(ssd->commentbox, dlg);
}

/*
 * Ctrl+F / Ctrl+G: asking to search ends a pending folder rename.
 *
 * Selecting a folder row makes the name box that folder's new name, and the box
 * stops filtering while it does - otherwise the live search rebuilds the list,
 * drops the selection and disarms the rename halfway through typing it. But
 * both search keys focus that same box WITHOUT changing the selection, so
 * without this they would land the cursor in a box that no longer searches:
 * a key whose whole purpose is searching, doing nothing visible.
 *
 * So the rule reads in both directions - a folder row selected means "renaming
 * this", and changing the selection OR asking to search means "not any more".
 */
static void sessionsaver_update_save_button(struct sessionsaver_data *ssd,
                                            dlgparam *dlg);
/*
 * End a pending folder rename: drop the folder selection, and take the folder
 * name back out of the box if it is still the one we put there.
 *
 * Leaving it behind is an accident waiting to happen: the button has gone back
 * to "Save", the box still reads "tests", and one click writes the running
 * session into a NEW session called "tests", sitting beside the folder of that
 * name. Stepping into a folder is the easy way to reach that state.
 *
 * WARNING: Only when UNTOUCHED. This runs on every list rebuild, and typing in the box
 * rebuilds the list - so clearing unconditionally would wipe a search halfway
 * through typing it. Text the user has edited is theirs and stays.
 */
static void sessionsaver_end_folder_rename(struct sessionsaver_data *ssd,
                                           dlgparam *dlg)
{
    bool untouched = (ssd->folder_text_in_box && ssd->savedsession &&
                      !strcmp(ssd->folder_text_in_box, ssd->savedsession));
    sfree(ssd->selected_folder);
    ssd->selected_folder = NULL;
    sfree(ssd->folder_text_in_box);
    ssd->folder_text_in_box = NULL;
    if (untouched) {
        sfree(ssd->savedsession);
        ssd->savedsession = dupstr("");
        if (ssd->editbox)
            dlg_refresh(ssd->editbox, dlg);
    }
    sessionsaver_update_save_button(ssd, dlg);
}

void kitty_config_end_folder_rename(dlgparam *dp)
{
    struct sessionsaver_data *ssd = session_filter_ssd;
    if (!ssd || !ssd->selected_folder)
        return;
    sessionsaver_end_folder_rename(ssd, dp);
}

static void sessionsaver_data_free(void *ssdv)
{
    struct sessionsaver_data *ssd = (struct sessionsaver_data *)ssdv;
    if (session_filter_ctrl == ssd->editbox)
        session_filter_ctrl = NULL;
    if (session_filter_ssd == ssd) {
        session_filter_ssd = NULL;
        kitty_ctrl_focus_hook = NULL;
    }
    /* The override control belonged to this config box; it is ctrl_alloc'd and
     * about to go with the ctrlbox, so drop the pointer rather than leave it
     * dangling for the next box that opens. */
    pxchoice_state = NULL;
    get_sesslist(&ssd->sesslist, false);
    sfree(ssd->savedsession);
    sfree(ssd->newfolder);
    sfree(ssd->searchfilter);
    sfree(ssd->folder_at_load);
    sfree(ssd->loaded_from);
    sfree(ssd->selected_folder);
    sfree(ssd->folder_text_in_box);
    for (int fr = 0; fr < ssd->nfolderrows; fr++)
        sfree(ssd->folderrows[fr]);
    sfree(ssd->folderrows);
    sfree(ssd);
}


/*
 * KiTTY folder navigation ([ConfigBox] foldernavigation=yes, hknet/KiTTY#26).
 *
 * Folders become ROWS of the saved-session list instead of entries in a combo:
 * the folders of the current level first, then the sessions at that level, with
 * ".." to step back out. Storage does not change - a folder is still an
 * attribute of a session - so this is a VIEW, and turning the setting off puts
 * everything back exactly as it was.
 *
 * Two kinds of row now share one listbox. They are told apart by ID: a session
 * row carries its session index (>= 0, as before), a navigation row carries a
 * NEGATIVE id. Everything that consumes a selection already rejects a negative
 * id - load_selected_session beeps on selid < 0, update_comment_display and the
 * SELCHANGE handler both test i >= 0 - so a folder row cannot be loaded, saved
 * over or commented on by accident. The combo's synthetic "<new folder...>" row
 * has used id -1 for the same reason, which is why the ids below start at -2.
 */
#define KITTY_ROW_PARENT       (-2)      /* the ".." row */
#define KITTY_ROW_FOLDER_BASE  (-1000)   /* FolderList[i]  ->  BASE - i */

static bool kitty_folder_rows_on(void)
{
    return !GetPuttyFlag() && GetFolderNavigationFlag();
}

/*
 * Are session folders available in this dialog at all?
 *
 * WARNING: This exists because "the folder combo" and "folders work" USED to be the
 * same condition, and several behaviours were written as `if (ssd->folderlist)`.
 * Folder navigation sets that pointer to NULL - it steers by the list instead -
 * so every one of those became a silent no-op in the new mode. The one that
 * mattered: Save stopped re-filing a session, so loading one from a folder,
 * stepping out to the root with ".." and saving left it in the old folder with
 * no warning at all. Ask this, not for the control.
 */
static bool kitty_folder_rows_on(void);
static bool kitty_folders_available(struct sessionsaver_data *ssd)
{
    return !GetPuttyFlag() && (ssd->folderlist || kitty_folder_rows_on());
}

/*
 * Does THIS dialog navigate folders as rows?
 *
 * The setting is global; the answer is not. Mid-session (Change Settings) the
 * saved-session list exists only to name a save target - there is no load, so
 * there is nothing to navigate TO - and the rows were drawn there with nothing
 * able to activate them: ".." and every folder row were inert, because the
 * handler that steps into them sits behind the same !midsession guard the load
 * path needs. A row that does nothing when clicked is worse than no row.
 *
 * Ask this rather than kitty_folder_rows_on() for anything that decides what
 * the list LOOKS LIKE or what activating a row DOES. The plain setting still
 * answers "is this mode configured at all", which mid-session needs to know:
 * it is the reason no folder combo is built there either, and therefore the
 * reason the mid-session list must not be filtered by folder (see
 * kitty_session_on_level) - a filter with no control to change it.
 */
static bool kitty_folder_rows_active(struct sessionsaver_data *ssd)
{
    return kitty_folder_rows_on() && ssd && !ssd->midsession;
}

/* "Default" is the root list rather than a folder of that name - every filter
 * path in this file keys off that comparison, so it is spelt out once here. */
static bool kitty_at_root_level(void)
{
    return (!CurrentFolder[0] || !strcmp(CurrentFolder, "Default"));
}

/*
 * Does this session belong on the level being shown?
 *
 * Index 0 is "Default Settings": it belongs to no folder and is deliberately
 * shown at every level (see the exemption at load_selected_session), so it is
 * never filtered out here.
 *
 * The root level is where the two modes differ, and it is the visible change
 * this setting makes: classic mode shows EVERY session at the root, annotating
 * the filed ones "[folder]"; folder-navigation mode shows only the UNFILED
 * ones, because the rest are reached by stepping into their folder row.
 */
/* Is a cross-folder search actually running? Ctrl+G arms it, but it only means
 * anything while something is typed - with an empty box "search everywhere" and
 * "show me the root" are the same list. */
static bool kitty_searching_all_folders(struct sessionsaver_data *ssd)
{
    return kitty_folder_rows_active(ssd) && ssd->search_all &&
        ssd->searchfilter && ssd->searchfilter[0];
}

static bool kitty_session_on_level(struct sessionsaver_data *ssd, int i)
{
    char *fld;
    bool ok;
    if (GetPuttyFlag() || i == 0)
        return true;
    if (kitty_searching_all_folders(ssd))
        return true;                 /* Ctrl+G: every folder is in scope */
    /* Mid-session CurrentFolder is the running session's OWN folder (seeded at
     * the list's first refresh), so filtering by it shows the session's
     * neighbours - which is the level the user is actually looking at. */
    if (!kitty_at_root_level()) {
        fld = kitty_read_session_folder_cached(ssd->sesslist.sessions[i]);
        ok = (fld && !strcmp(fld, CurrentFolder));
        sfree(fld);
        return ok;
    }
    if (!kitty_folder_rows_active(ssd))
        return true;                 /* classic root list: everything shows */
    fld = kitty_read_session_folder_cached(ssd->sesslist.sessions[i]);
    ok = (!fld || !*fld || !strcmp(fld, "Default"));
    sfree(fld);
    return ok;
}

/* How many navigation/folder rows sit ABOVE the session rows. The population
 * loop and sessionsaver_folder_visible_position must agree exactly or the
 * index->row mapping drifts and the selection lands on the wrong line, so both
 * go through this rather than counting for themselves. */
static int kitty_nav_row_count(struct sessionsaver_data *ssd);

static int sessionsaver_folder_visible_position(struct sessionsaver_data *ssd,
                                                int sessindex)
{
    /* Folder navigation puts its rows above the sessions, so every session row
     * shifts down by that many. The rows themselves were worked out by the last
     * refresh, which is the only thing that can have drawn them. */
    int pos = kitty_nav_row_count(ssd);
    if (sessindex < 0 || sessindex >= ssd->sesslist.nsessions)
        return -1;
    for (int i = 0; i < ssd->sesslist.nsessions; i++) {
        /* Must mirror the listbox population loop exactly, or index->row mapping
         * drifts. defaultsettings=no hides "Default Settings", so it occupies no
         * row (without this the whole selection was off by one). */
        { extern int GetDefaultSettingsFlag(void);
          if (!GetDefaultSettingsFlag() &&
              !strcmp(ssd->sesslist.sessions[i], KITTY_DEFAULT_SESSION)) {
              if (i == sessindex) return -1;   /* the hidden row has no position */
              continue;
          } }
        if (!kitty_session_on_level(ssd, i))
            continue;
        if (i == sessindex)
            return pos;
        pos++;
    }
    return -1;
}

/*
 * Helper function to load the session selected in the list box, if
 * any, as this is done in more than one place below. Returns 0 for
 * failure.
 */
static void sessionsaver_switch_folder(struct sessionsaver_data *ssd,
                                       dlgparam *dlg, const char *folder);

static bool load_selected_session(
    struct sessionsaver_data *ssd,
    dlgparam *dlg, Conf *conf, bool *maybe_launch)
{
    int i = dlg_listbox_index(ssd->listbox, dlg);
    int selid = i;
    bool isdef;
    if (i < 0) {
        dlg_beep(dlg);
        return false;
    }
    selid = dlg_listbox_getid(ssd->listbox, dlg, i);
    if (selid < 0 || selid >= ssd->sesslist.nsessions) {
        dlg_beep(dlg);
        return false;
    }
    i = selid;
    isdef = !strcmp(ssd->sesslist.sessions[i], KITTY_DEFAULT_SESSION);
    load_settings(ssd->sesslist.sessions[i], conf);
    /* KiTTY: what is genuinely in the box now, for the Save guard. */
    sfree(ssd->loaded_from);
    ssd->loaded_from = dupstr(ssd->sesslist.sessions[i]);
    /* KiTTY: with "Remember window position" on, Top/Left and Columns/Rows
     * show this session's remembered entry for the monitor layout the box is
     * on now, so what the panel shows is what the window will open with. Only
     * then - with it off, a typed fixed position must not be overwritten by
     * an entry left behind from earlier. */
    kitty_winpos_load_into_conf(ssd->sesslist.sessions[i], conf);
    /* KiTTY: the proxy override belongs to the session that was showing, not to
     * the box. A newly loaded session starts with no override, derived from its
     * own proxy settings - so this covers Load, a double-click on the list, and
     * Enter from the search box alike, all of which come through here. */
    kitty_proxy_override_reset(conf);
    /* KiTTY: follow the loaded session into its folder, and remember which
     * folder it arrived in. The folder combo is the list filter, but Save also
     * uses it to file the session (it is the only way to move a session
     * between folders), so the two must agree: without this, loading a session
     * from a folder while viewing another one and pressing Save silently moved
     * it. The remembered value is the guard - Save only rewrites the session's
     * folder when the selection actually CHANGED after the load.
     *
     * "Default Settings" is exempt: the filter shows it under every folder, so
     * it belongs to none, and following its (meaningless, but real) stored
     * value would drag the view somewhere the user never chose. */
    if (kitty_folders_available(ssd) && !isdef) {
        const char *fld = conf_get_str(conf, CONF_folder);
        if (!fld || !*fld)
            fld = "Default";
        sessionsaver_switch_folder(ssd, dlg, fld);
        sfree(ssd->folder_at_load);
        ssd->folder_at_load = dupstr(fld);
        ssd->folder_new_selected = 0;
        sfree(ssd->newfolder);
        ssd->newfolder = dupstr("");
    }
    /* KiTTY: remember this as the last-loaded session, so the config box
     * re-selects/auto-loads it next time it opens. "Default Settings" is
     * remembered too (hknet/KiTTY#23): next start recognises it and comes up in
     * quick connect instead of pre-filling anything, so loading the defaults is
     * how that mode is armed - and loading any other session disarms it. */
    kitty_set_last_session(ssd->sesslist.sessions[i]);
    sfree(ssd->savedsession);
    /* KiTTY: keep "Default Settings" visible in the session-name box after
     * loading the defaults, instead of stock PuTTY's clearing it. The literal
     * name is already a normal value for savedsession (single-clicking the
     * list entry puts it there too) and the Save path maps it to the defaults
     * key. PuTTY-compat mode keeps the stock behaviour. */
    ssd->savedsession = dupstr(isdef ? (GetPuttyFlag() ? "" : KITTY_DEFAULT_SESSION)
                                     : ssd->sesslist.sessions[i]);
    sfree(ssd->searchfilter);
    ssd->searchfilter = dupstr("");
    if (maybe_launch)
        *maybe_launch = !isdef;
    /* Anything the panels cannot represent is noted while they refresh, and
     * reported once afterwards - one message about the session, rather than
     * one per setting. */
    kitty_conf_invalid_reset();
    kitty_conf_invalid_session_is(ssd->sesslist.sessions[i]);
    kitty_conf_validate(conf);
    dlg_refresh(NULL, dlg);
    /* Not reported here. With the panel cache a panel's controls do not exist
     * until it is first shown, and a handler that never runs cannot notice a
     * value it cannot represent - so at this moment the only settings checked
     * are the ones on panels already built. The rest are checked as the
     * background warm-up builds them, and the box reports when that finishes
     * (windows/dialog.c). */
    kitty_conf_invalid_report(dlg, ssd->sesslist.sessions[i]);
    /* Restore the selection, which might have been clobbered by
     * changing the value of the edit box. The listbox API wants a visible row
     * position, not the stable session id used in filtered/search lists. */
    {
        int row = sessionsaver_folder_visible_position(ssd, i);
        if (row >= 0)
            dlg_listbox_select(ssd->listbox, dlg, row);
    }
    /* KiTTY: dlg_refresh(NULL) above refreshed the read-only comment box while
     * the listbox selection was momentarily cleared (so it blanked); refresh it
     * again now the selection is restored, so the comment stays shown after Load. */
    if (ssd->commentbox)
        dlg_refresh(ssd->commentbox, dlg);

    /* KiTTY (hknet/KiTTY#23): loading the defaults arms quick connect NOW, in
     * this box, not only at the next start.
     *
     * kitty_set_last_session() above is what a future start reads, and that was
     * the whole of it: within the run where the user actually loaded "Default
     * Settings" nothing happened, so "load the defaults and type a host" only
     * came true after a restart. Loading any other session disarms it again, so
     * the two ways of working stay one click apart.
     *
     * With it armed, put the caret in Host Name and select what is there - the
     * same thing the box does when it opens in quick connect. The startup path
     * cannot do this one: it fires once, on the first refresh. */
    if (!GetPuttyFlag()) {
        SetQuickConnectMode(isdef ? 1 : 0);
        kitty_dlg_mark_quickconnect(dlg, isdef);
        if (isdef && quickconnect_host_ctrl)
            dlg_set_focus_later(quickconnect_host_ctrl, dlg);
    }
    return true;
}

/* KiTTY: the launch-intent rule shared by the Open and Start buttons
 * (hknet/KiTTY#18). What does the user mean to act on?
 *  - while the live search is filtering: the highlighted visible match;
 *  - else, if their last action was selecting a row in the session list:
 *    that selection;
 *  - anything else: the current (possibly tweaked) settings as they are.
 * The first two load into conf; returns false (already beeped) when that
 * load failed and the caller should give up. Keep this the ONLY place the
 * rule lives, so Open and Start can never drift apart. (The Enter-while-
 * searching case handles the filter itself before calling this - first
 * Enter selects, second starts.) */
static bool sessionsaver_resolve_launch_target(
    struct sessionsaver_data *ssd, dlgparam *dlg, Conf *conf, dlgcontrol *ctrl)
{
    /*
     * Both ways of resolving the target read the saved-sessions list, and
     * NEITHER is meaningful unless that list is on screen: the config box
     * physically destroys the controls of every panel except the visible one
     * (see the comment on dlg_is_visible()), so ssd->listbox does not exist
     * while the user is on any other panel - and dlg_listbox_index() asserts
     * on a control it cannot find.
     *
     * Start and Open live in the always-present action area, so they can be
     * pressed from anywhere. Typing a session name arms the search filter, and
     * the filter branch below used to load unconditionally: name a session,
     * switch to any other panel, press Start, and KiTTY died on the assertion
     * at windows/controls.c:2376 instead of launching. With the list not
     * displayed there is nothing highlighted to prefer, so the right answer is
     * to launch the current settings as they stand.
     */
    if (ssd->midsession || !ssd->listbox || !dlg_is_visible(ssd->listbox, dlg))
        return true;

    if (ssd->searchfilter && ssd->searchfilter[0]) {
        if (!load_selected_session(ssd, dlg, conf, NULL)) {
            dlg_beep(dlg);
            return false;
        }
        return true;
    }
    if (dlg_last_focused(ctrl, dlg) == ssd->listbox) {
        if (!load_selected_session(ssd, dlg, conf, NULL)) {
            dlg_beep(dlg);
            return false;
        }
    }
    return true;
}

/* KiTTY: refresh the read-only comment box from the session currently selected
 * in the saved-sessions list. Indexing mirrors load_selected_session() so the
 * box always shows the comment of the session that Load would open. Shows the
 * empty string if nothing is selected or the session has no comment. */
static int sessionsaver_selected_session_index(struct sessionsaver_data *ssd, dlgparam *dlg)
{
    int i = dlg_listbox_index(ssd->listbox, dlg);
    if (i < 0)
        return -1;
    return dlg_listbox_getid(ssd->listbox, dlg, i);
}
static void update_comment_display(struct sessionsaver_data *ssd, dlgparam *dlg)
{
    int i;
    char *c;
    if (!ssd->commentbox)
        return;
    i = sessionsaver_selected_session_index(ssd, dlg);
    if (i < 0 || i >= ssd->sesslist.nsessions) {
        dlg_editbox_set(ssd->commentbox, dlg, KT_CFG_COMMENT_SELECT);
        return;
    }
    /* Read "Comment" directly, scanning all hives for a non-empty value, so
     * comments authored by an older KiTTY (held only in the 9bis hive) show
     * even before the session is re-saved into the new hive. */
    c = kitty_read_session_comment(ssd->sesslist.sessions[i]);
    dlg_editbox_set(ssd->commentbox, dlg, (c && *c) ? c : KT_CFG_COMMENT_NONE);
    sfree(c);
}

static int sessionsaver_filter_match(const char *sessionname, const char *filter)
{
    char *work, *tok;
    int first = 1, prefix = 0;
    if (!filter || !*filter)
        return 1;
    if (!sessionname)
        return 0;
    work = dupstr(filter);
    for (tok = strtok(work, " \t"); tok; tok = strtok(NULL, " \t")) {
        const char *p;
        size_t n = strlen(tok);
        int found = 0;
        if (first && !strnicmp(sessionname, tok, n))
            prefix = 1;
        for (p = sessionname; *p; p++)
            if (!strnicmp(p, tok, n)) {
                found = 1;
                break;
            }
        if (!found) {
            sfree(work);
            return 0;
        }
        first = 0;
    }
    sfree(work);
    return prefix ? 2 : 1;              /* prefix-token matches before substrings */
}

/*
 * The folders visible at the current level. With one level of drill-down that
 * is "all of them, at the root, and none once you are inside one" - so this
 * doubles as the answer to "am I somewhere I can go up from".
 *
 * The filter narrows folder rows exactly as it narrows session rows: they are
 * rows of the same list, and a filter that skipped one kind would behave
 * arbitrarily. ".." is the one exemption - it is the way out, so a search that
 * matches nothing still leaves it on screen rather than a dead end.
 */
static bool kitty_folder_row_visible(const char *name, const char *filter)
{
    return name && name[0] && strcmp(name, "Default") != 0 &&
        sessionsaver_filter_match(name, filter) != 0;
}

/*
 * Work out which folders to draw, from TWO sources, because neither alone is
 * complete:
 *
 *  - the SESSIONS themselves. A folder is an attribute of a session
 *    (Folder=<name>), and that attribute is the design - kitty_read_session_folder
 *    reads it in every save mode. This is the authoritative source of "folders
 *    that have something in them".
 *
 *  - FolderList, the stored list ([KiTTY] Folders). Needed because a folder with
 *    nothing in it still exists and must still be shown; no session names it, so
 *    scanning sessions alone would silently drop it.
 *
 * The union matters in practice, not just in theory: FolderList is NOT filled
 * from sessions in portable/dir mode. InitFolderList's dir branch is guarded
 * `IniFileFlag == SAVEMODE_DIR && !DirectoryBrowseFlag` (kitty.c:676), while
 * savemode=dir sets DirectoryBrowseFlag (kitty.c:1072) - so that scan never
 * runs, and the legacy path it guards looks for folders as SUBDIRECTORIES,
 * which is not how sessions are stored (kitty_storage.c: one flat file per
 * session, carrying Folder=). Deriving the rows from the attribute sidesteps
 * that disagreement entirely rather than depending on which mode is active.
 */
static void kitty_rebuild_folder_rows(struct sessionsaver_data *ssd,
                                      const char *filter)
{
    int i, n = 0, cap;
    for (i = 0; i < ssd->nfolderrows; i++)
        sfree(ssd->folderrows[i]);
    sfree(ssd->folderrows);
    ssd->folderrows = NULL;
    ssd->nfolderrows = 0;
    if (!kitty_folder_rows_active(ssd) || !kitty_at_root_level())
        return;                          /* one level: no folders inside one */

    cap = ssd->sesslist.nsessions + 64;
    ssd->folderrows = snewn(cap, char *);

    for (i = 0; FolderList && FolderList[i] != NULL && n < cap; i++)
        if (kitty_folder_row_visible(FolderList[i], filter))
            ssd->folderrows[n++] = dupstr(FolderList[i]);

    for (i = 0; i < ssd->sesslist.nsessions && n < cap; i++) {
        char *fld = kitty_read_session_folder_cached(ssd->sesslist.sessions[i]);
        if (kitty_folder_row_visible(fld, filter)) {
            int j, seen = 0;
            for (j = 0; j < n; j++)
                if (!strcmp(ssd->folderrows[j], fld)) { seen = 1; break; }
            if (!seen)
                ssd->folderrows[n++] = dupstr(fld);
        }
        sfree(fld);
    }
    ssd->nfolderrows = n;
}

static int kitty_nav_row_count(struct sessionsaver_data *ssd)
{
    if (!kitty_folder_rows_active(ssd))
        return 0;
    if (!kitty_at_root_level())
        return 1;                        /* ".." only: one level, so no folders */
    return ssd->nfolderrows;
}

/*
 * Put the navigation rows in, above the sessions.
 *
 * Folders are drawn with a trailing "/" and the way up as "..", the filesystem
 * spelling of the Explorer metaphor. A row has to be tellable from a session by
 * looking, in a list where a session may already read "name [folder]", and text
 * is what does that: marking them with icons was considered and dropped, since
 * a picture has no name for a screen reader to announce.
 */
static void sessionsaver_add_nav_rows(dlgcontrol *ctrl, dlgparam *dlg,
                                      struct sessionsaver_data *ssd)
{
    int i;
    if (!kitty_folder_rows_active(ssd))
        return;
    if (!kitty_at_root_level()) {
        dlg_listbox_addwithid(ctrl, dlg, "..", KITTY_ROW_PARENT);
        return;
    }
    for (i = 0; i < ssd->nfolderrows; i++) {
        char disp[700];
        snprintf(disp, sizeof(disp), "%s/", ssd->folderrows[i]);
        dlg_listbox_addwithid(ctrl, dlg, disp, KITTY_ROW_FOLDER_BASE - i);
    }
}

/* The folder a navigation row stands for, or NULL if the id is not one of
 * ours. ".." maps to the root, since one level of drill-down means up is
 * always the root. */
static const char *kitty_folder_for_row_id(struct sessionsaver_data *ssd, int id)
{
    int idx;
    if (id == KITTY_ROW_PARENT)
        return "Default";
    if (id > KITTY_ROW_FOLDER_BASE)
        return NULL;
    idx = KITTY_ROW_FOLDER_BASE - id;
    if (idx < 0 || idx >= ssd->nfolderrows)
        return NULL;
    return ssd->folderrows[idx][0] ? ssd->folderrows[idx] : NULL;
}

static void sessionsaver_add_session_row(dlgcontrol *ctrl, dlgparam *dlg,
                                          struct sessionsaver_data *ssd,
                                          int session_index, bool searching)
{
    char disp[700];
    const char *sessionname = ssd->sesslist.sessions[session_index];
    int og = kitty_session_origin(sessionname);
    /* Show which folder a session is in when the list is not already narrowed
     * to one - inside a folder the bracket would just repeat the selection on
     * every row. That means while searching (results come from everywhere),
     * and in the root list, which shows EVERY session rather than only the
     * unfiled ones. Searching keeps tagging unfiled sessions "[root]" so every
     * result is accounted for; browsing the root leaves them bare, where the
     * bracket is meant to point out the ones that live somewhere else. */
    bool root_view = kitty_at_root_level();
    /* Folder navigation normally never annotates: every row on screen belongs
     * to the level you are looking at, so the bracket would repeat the same
     * folder on every row.
     *
     * The exception is the Ctrl+G search, which deliberately spans folders -
     * there the results come from everywhere, so each one has to say where it
     * lives or the list is a set of names with no way to tell them apart. */
    bool annotate_folders = !kitty_folder_rows_active(ssd) ||
        kitty_searching_all_folders(ssd);
    char *fld = (annotate_folders && (searching || root_view)) ?
        kitty_read_session_folder(sessionname) : NULL;
    bool filed = (fld && *fld && strcmp(fld, "Default"));
    if (annotate_folders && (searching || filed)) {
        const char *folder = filed ? fld : "root";
        if (og == 0)
            snprintf(disp, sizeof(disp), "%s [%s]", sessionname, folder);
        else
            snprintf(disp, sizeof(disp), "%s [%s]   (%s)", sessionname,
                     folder, og == 2 ? "PuTTY" : "old KiTTY");
        sfree(fld);
        dlg_listbox_addwithid(ctrl, dlg, disp, session_index);
        return;
    }
    sfree(fld);
    if (og == 0) {
        dlg_listbox_addwithid(ctrl, dlg, sessionname, session_index);
    } else {
        snprintf(disp, sizeof(disp), "%s   (%s)", sessionname,
                 og == 2 ? "PuTTY" : "old KiTTY");
        dlg_listbox_addwithid(ctrl, dlg, disp, session_index);
    }
}

static void kitty_root_folder_cannot_delete(dlgparam *dlg)
{
    typedef int (WINAPI *MessageBoxTimeoutA_t)(HWND,LPCSTR,LPCSTR,UINT,WORD,DWORD);
    HMODULE user32 = GetModuleHandleA("user32.dll");
    MessageBoxTimeoutA_t msgbox_timeout = user32 ?
        (MessageBoxTimeoutA_t)kitty_api_from(user32, "user32.dll", "MessageBoxTimeoutA", KITTY_API_OPTIONAL,
                                  "message boxes that close themselves") : NULL;
    if (msgbox_timeout)
        msgbox_timeout(kitty_cfg_modal_owner(), KT_CFG_ROOT_FOLDER_CANT_DELETE, KT_CAP_KITTY,
                       MB_OK | MB_ICONINFORMATION, 0, 5000);
    else
        MessageBoxA(kitty_cfg_modal_owner(), KT_CFG_ROOT_FOLDER_CANT_DELETE, KT_CAP_KITTY,
                    MB_OK | MB_ICONINFORMATION);
}

/* KiTTY folder combo, three shared pieces:
 *
 * - "Default" is not a folder but the ROOT session list, and the filter paths
 *   all key off strcmp(CurrentFolder, "Default"). Its LABEL is nevertheless
 *   user-settable ([KiTTY] RootFolderLabel in kitty.ini) - display only, the
 *   stored key stays "Default", so no comparison anywhere else changes.
 * - a synthetic "<new folder...>" row makes creating a folder an explicit
 *   choice instead of "type a name while something else is selected".
 * - ssd->createbutton relabels itself to match what it will actually do.
 */
#define KITTY_ROOT_FOLDER_LABEL_DEFAULT "All sessions (root)"
#define KITTY_NEW_FOLDER_ITEM           "<new folder...>"

/* What ssd->createbutton does with the text currently in the combo. Creating
 * has its own row, so a name typed over a SELECTION means "rename that", and
 * only the synthetic row means "create". */
#define KITTY_FOLDER_ACTION_NEW     0   /* create the typed folder */
#define KITTY_FOLDER_ACTION_RELABEL 1   /* set the root list's display label */
#define KITTY_FOLDER_ACTION_RENAME  2   /* rename the selected folder */

static void kitty_get_root_folder_label(char *buf, size_t len)
{
    buf[0] = '\0';
    if (ReadParameterN(INIT_SECTION, KI_ROOTFOLDERLABEL, buf, len) == 0 ||
        !buf[0]) {
        strncpy(buf, KITTY_ROOT_FOLDER_LABEL_DEFAULT, len - 1);
        buf[len - 1] = '\0';
    }
}

/* Names that may not become a folder, nor the root label: they either ARE the
 * root list under one of its spellings, or they are the synthetic create row. */
static bool kitty_folder_name_reserved(const char *name)
{
    char rootlabel[256];
    kitty_get_root_folder_label(rootlabel, sizeof(rootlabel));
    return !stricmp(name, "Default") || !stricmp(name, "All sessions") ||
        !stricmp(name, KITTY_ROOT_FOLDER_LABEL_DEFAULT) ||
        !stricmp(name, "root") || !stricmp(name, rootlabel) ||
        !stricmp(name, KITTY_NEW_FOLDER_ITEM);
}

/* How many saved sessions sit in a folder. Entry 0, the "Default Settings"
 * pseudo-session, is not one of them: the filter shows it under every folder,
 * so it belongs to none, and counting it would tell the user a folder holds
 * one more session than it does. It is still CLEARED by the move below, since
 * a stored folder on it feeds the folder-list rebuild.
 *
 * Membership is read with kitty_read_session_folder(), which looks in the
 * PRIMARY hive only - so sessions living solely in a legacy hive are never
 * members of anything, and the rewrite below never has to touch a hive we
 * treat as read-only. */
static int sessionsaver_folder_member_count_of(struct sessionsaver_data *ssd,
                                               const char *folder)
{
    int i, n = 0;
    if (!folder || !folder[0] || !strcmp(folder, "Default"))
        return 0;
    for (i = 0; i < ssd->sesslist.nsessions; i++) {
        char *fld;
        if (!strcmp(ssd->sesslist.sessions[i], KITTY_DEFAULT_SESSION))
            continue;                      /* belongs to no folder; see above */
        fld = kitty_read_session_folder_cached(ssd->sesslist.sessions[i]);
        if (fld && !strcmp(fld, folder))
            n++;
        sfree(fld);
    }
    return n;
}

static int sessionsaver_folder_member_count(struct sessionsaver_data *ssd)
{
    return sessionsaver_folder_member_count_of(ssd, CurrentFolder);
}

/* Move every session in `from` to `to` ("Default" = the root list), by
 * rewriting just their "Folder" value. open_settings_w() updates in place in
 * both backends - the registry key keeps every value we do not write, and the
 * portable backend preloads the session file first - so this is a surgical
 * edit, not a load/save round trip: no password re-wrap, no conf
 * normalisation, nothing else touched.
 *
 * Returns the number moved, or -1 on failure (with dlg_error_msg already
 * shown). A partial failure leaves the sessions already moved in `to`; the
 * caller must NOT then drop `from` from the folder list, or the stragglers
 * would resurrect it anyway on the next rebuild. */
static int sessionsaver_move_folder_sessions(struct sessionsaver_data *ssd,
                                             dlgparam *dlg,
                                             const char *from, const char *to)
{
    int i, moved = 0;
    for (i = 0; i < ssd->sesslist.nsessions; i++) {
        const char *sess = ssd->sesslist.sessions[i];
        bool isdef = !strcmp(sess, KITTY_DEFAULT_SESSION);
        const char *dest = isdef ? "Default" : to;
        char *fld = kitty_read_session_folder_cached(sess);
        int match = (fld && !strcmp(fld, from));
        char *errmsg = NULL;
        settings_w *w;
        sfree(fld);
        if (!match)
            continue;
        /* "Default Settings" is never carried into the new folder - it belongs
         * to none. Clearing it here is also what removes folder ghosts left by
         * earlier versions, which silently kept renamed/deleted folders alive. */
        w = open_settings_w(sess, &errmsg);
        if (!w) {
            char msg[512];
            snprintf(msg, sizeof(msg),
                     KT_CFG_SESSION_UPDATE_FAILED, sess,
                     errmsg ? errmsg : KT_MSG_UNKNOWN_ERROR);
            sfree(errmsg);
            dlg_error_msg(dlg, msg);
            return -1;
        }
        write_setting_s(w, "Folder", dest);
        close_settings_w(w);
        kitty_session_folder_cache_clear();
        if (!isdef)
            moved++;
    }
    return moved;
}

static bool sessionsaver_folder_exists(const char *name)
{
    int i;
    for (i = 0; FolderList && FolderList[i] != NULL; i++)
        if (FolderList[i][0] && !stricmp(FolderList[i], name))
            return true;
    return false;
}

/* A name typed over the current selection renames it - the root list's label
 * if that is what is selected, otherwise the folder itself. Only the synthetic
 * row creates. Recomputed wherever the button is refreshed, so it can never
 * advertise an action that no longer applies. */
static void sessionsaver_recompute_folder_action(struct sessionsaver_data *ssd)
{
    bool pending = ssd->newfolder && ssd->newfolder[0];
    bool root_sel = (!CurrentFolder[0] || !strcmp(CurrentFolder, "Default"));
    if (ssd->folder_new_selected || !pending)
        ssd->folder_action = KITTY_FOLDER_ACTION_NEW;
    else
        ssd->folder_action = root_sel ? KITTY_FOLDER_ACTION_RELABEL :
            KITTY_FOLDER_ACTION_RENAME;
}

/*
 * Rename a folder, whether or not it is the one being browsed.
 *
 * A folder is not a container: membership is the "Folder" value of each
 * session, and InitFolderList() rebuilds the folder set from the stored list
 * PLUS a scan of those values. So the members are rewritten FIRST - renaming
 * only the list entry would be undone by the very next rebuild.
 *
 * `old` is passed in rather than read from CurrentFolder because the two routes
 * disagree about it: the combo renames the folder you are INSIDE, while a
 * folder ROW is only ever shown at the root, so that route renames one you are
 * not in. CurrentFolder therefore moves only when it was the renamed folder.
 *
 * Returns true if the folder was renamed. Reports its own errors.
 */
static bool sessionsaver_rename_folder(struct sessionsaver_data *ssd,
                                       dlgparam *dlg, Conf *conf,
                                       const char *oldname, const char *wanted)
{
    char folder[1024], old[1024];

    strncpy(folder, wanted ? wanted : "", sizeof(folder)-1);
    folder[sizeof(folder)-1] = '\0';
    CleanFolderName(folder);
    strncpy(old, oldname ? oldname : "", sizeof(old)-1);
    old[sizeof(old)-1] = '\0';

    if (!folder[0] || !old[0]) {
        dlg_beep(dlg);
        return false;
    }
    if (!strcmp(folder, old))
        return false;                    /* nothing typed: not an error */
    if (kitty_folder_name_reserved(folder)) {
        dlg_error_msg(dlg, KT_CFG_FOLDER_NAME_RESERVED_ROOT);
        return false;
    }
    if (stricmp(folder, old) && sessionsaver_folder_exists(folder)) {
        dlg_error_msg(dlg, KT_CFG_FOLDER_EXISTS);
        return false;
    }
    if (sessionsaver_move_folder_sessions(ssd, dlg, old, folder) < 0)
        return false;

    InitFolderList();
    StringList_Del(FolderList, old);
    StringList_Add(FolderList, folder);
    SaveFolderList();
    /* Only follow the rename if we were standing in that folder. Renaming a row
     * at the root must leave the view where it is. */
    if (!strcmp(CurrentFolder, old)) {
        strncpy(CurrentFolder, folder, 1023);
        CurrentFolder[1023] = '\0';
        kitty_set_last_folder(CurrentFolder);
    }
    /* The loaded session, if it was in the renamed folder, must follow it or
     * the next Save would put it back under the old name. */
    if (!strcmp(conf_get_str(conf, CONF_folder), old))
        conf_set_str(conf, CONF_folder, folder);
    sfree(ssd->newfolder);
    ssd->newfolder = dupstr("");
    ssd->folder_new_selected = 0;
    dlg_refresh(ssd->folderlist, dlg);      /* NULL in folder-row mode: fine */
    dlg_refresh(ssd->listbox, dlg);
    kitty_notify_launcher_sessions_changed();
    return true;
}

/* Deleting a folder that still holds sessions: ask, then empty it by moving
 * them to the root list. Returns true if the folder is now empty and the
 * caller may drop it. */
static bool sessionsaver_confirm_empty_folder(struct sessionsaver_data *ssd,
                                              dlgparam *dlg)
{
    int n = sessionsaver_folder_member_count(ssd);
    char msg[512];
    if (n == 1)
        snprintf(msg, sizeof(msg),
                 KT_CFG_FOLDER_DELETE_ONE, CurrentFolder);
    else
        snprintf(msg, sizeof(msg),
                 KT_CFG_FOLDER_DELETE_MANY, CurrentFolder, n);
    if (MessageBoxA(kitty_cfg_modal_owner(), msg, KT_CAP_KITTY,
                    MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
        return false;
    return sessionsaver_move_folder_sessions(ssd, dlg, CurrentFolder,
                                             "Default") >= 0;
}

/*
 * KiTTY: Delete pressed on "Default Settings".
 *
 * It is the template every new session starts from, so it genuinely cannot be
 * deleted; the old behaviour was a bare beep, which reads as a broken button.
 * Explain that, and offer the one outcome the user can actually have - hiding
 * it from the list via [ConfigBox] defaultsettings=no.
 *
 * Written with writeINI() straight to kitty.ini, NOT WriteParameter(): this key
 * is declared use_readini=1 in kitty.c's ini_params[], i.e. it is always READ
 * with readINI(KittyIniFile,...) whatever the save mode. WriteParameter would
 * put it in the registry outside SAVEMODE_DIR, where nothing would ever read it
 * back and the setting would silently not stick.
 *
 * There is deliberately no UI to unhide: the entry is gone from the list, so a
 * checkbox for it would have nowhere to live. Hence the message states the file
 * path and the exact key needed to undo it.
 */
static void sessionsaver_offer_hide_default(struct sessionsaver_data *ssd,
                                            dlgparam *dlg)
{
    extern int writeINI(const char *filename, const char *section,
                        const char *key, const char *value);
    const char *ini = GetKittyIniFile();
    char msg[1400];

    /* conf=no (no configuration file at all) or readonly=yes: the setting
     * cannot be persisted, so do not offer a choice we can't honour. */
    if (GetNoKittyFileFlag() || !ini || !ini[0] || GetReadOnlyFlag()) {
        snprintf(msg, sizeof(msg),
                 KT_CFG_DEFAULT_CANT_DELETE_NOSAVE,
                 KITTY_DEFAULT_SESSION,
                 GetReadOnlyFlag() ? KT_CFG_DEFAULT_READONLY
                                   : KT_CFG_DEFAULT_NO_CONF);
        MessageBoxA(kitty_cfg_modal_owner(), msg, KT_CAP_KITTY, MB_OK | MB_ICONINFORMATION);
        return;
    }

    snprintf(msg, sizeof(msg),
             KT_CFG_DEFAULT_HIDE_Q,
             KITTY_DEFAULT_SESSION, ini);

    if (MessageBoxA(kitty_cfg_modal_owner(), msg, KT_CAP_KITTY,
                    MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
        return;                          /* No = keep showing it */

    CreateDefaultIniFile();              /* no-op when it already exists */
    if (!writeINI(ini, KI_SECTION_CONFIGBOX, KI_CONFIGBOX_DEFAULTSETTINGS, "no")) {
        snprintf(msg, sizeof(msg),
                 KT_CFG_DEFAULT_WRITE_FAILED, ini, KITTY_DEFAULT_SESSION);
        MessageBoxA(kitty_cfg_modal_owner(), msg, KT_CAP_KITTY, MB_OK | MB_ICONERROR);
        return;
    }

    SetDefaultSettingsFlag(0);           /* take effect without a restart */
    get_sesslist(&ssd->sesslist, false);
    get_sesslist(&ssd->sesslist, true);
    kitty_session_folder_cache_clear();
    dlg_refresh(ssd->listbox, dlg);
}

/*
 * Save is greyed out while the session name box is empty.
 *
 * An empty box meant "use whichever row is highlighted", and the user was never
 * shown which row that was: clicking a row copies its name INTO the box, so an
 * empty box means no row was clicked here - the highlight is the one the dialog
 * restored by itself. Reported live: a fresh box with new settings typed into
 * it, Save, and those settings landed in the last-used session with no prompt.
 * Nothing legitimate is lost - saving Default Settings still works by selecting
 * it, which fills the box with its name.
 */
static void sessionsaver_update_save_button(struct sessionsaver_data *ssd,
                                            dlgparam *dlg)
{
    if (!ssd->savebutton)
        return;
    /* KiTTY: the button says what it will do. With a folder row selected the
     * box holds that folder's name, and pressing this renames it - so the
     * caption is the only thing telling the user that Save has become
     * something else. Compared by POINTER against the last literal used, as the
     * folder button does, so a redraw is issued only when it actually changes. */
    {
        const char *label = ssd->selected_folder ? KT_CFG_RENAME : KT_SESSION_SAVE;
        if (ssd->save_button_label != label) {
            dlg_label_change(ssd->savebutton, dlg, label);
            ssd->save_button_label = label;
        }
    }
    kitty_dlg_enable_button(ssd->savebutton, dlg,
                            ssd->savedsession && ssd->savedsession[0]);
}

static void sessionsaver_update_folder_button(struct sessionsaver_data *ssd,
                                              dlgparam *dlg)
{
    const char *label;
    sessionsaver_recompute_folder_action(ssd);
    if (!ssd->createbutton)          /* midsession / stock variants */
        return;
    if (kitty_folder_rows_on()) {
        /* One meaning, one label: the button acts on whatever is in the name
         * box when it is pressed. Nothing to announce. */
        label = KT_SESSION_NEW_FOLDER;
        if (ssd->folder_button_label != label) {
            dlg_label_change(ssd->createbutton, dlg, label);
            ssd->folder_button_label = label;
        }
        return;
    }
    /* Keep these SHORT: the button shares a 75/25 row with the combo, and
     * anything longer than "New folder" overflows its width. */
    label = (ssd->folder_action == KITTY_FOLDER_ACTION_NEW) ?
        KT_SESSION_NEW_FOLDER : KT_CFG_RENAME;
    if (ssd->folder_button_label == label)
        return;                      /* both are literals; no repaint needed */
    dlg_label_change(ssd->createbutton, dlg, label);
    ssd->folder_button_label = label;
}

/* Which FolderList entry does this text name, or -1? Matching is loose (case
 * insensitive) because it exists to let people SELECT a folder by typing.
 *
 * root_alias says whether the root list's built-in spellings ("root", "All
 * sessions", "All sessions (root)") count as naming it. They must NOT when the
 * root list is already selected: there, typing one of them is how you rename
 * its label back to the default, and treating it as "you selected the root
 * again" made the default the one name you could never type. */
static int sessionsaver_folder_index_for_text(const char *text, bool root_alias)
{
    int i;
    char rootlabel[256];
    if (!text || !*text)
        return -1;
    kitty_get_root_folder_label(rootlabel, sizeof(rootlabel));
    for (i = 0; FolderList && FolderList[i] != NULL; i++) {
        bool is_root;
        if (!FolderList[i][0])
            continue;
        is_root = !strcmp(FolderList[i], "Default");
        if (!stricmp(text, FolderList[i]) ||
            (is_root && root_alias && !stricmp(text, rootlabel)) ||
            (is_root && root_alias &&
             (!stricmp(text, KITTY_ROOT_FOLDER_LABEL_DEFAULT) ||
              !stricmp(text, "root") || !stricmp(text, "All sessions"))))
            return i;
    }
    return -1;
}

static void sessionsaver_switch_folder(struct sessionsaver_data *ssd,
                                       dlgparam *dlg, const char *folder)
{
    if (!strcmp(CurrentFolder, folder))
        return;
    /* Stepping into or out of a folder is a statement about WHERE you want to
     * look, so it ends the search that spanned everywhere. */
    ssd->search_all = 0;
    strncpy(CurrentFolder, folder, 1023);
    CurrentFolder[1023] = '\0';
    kitty_set_last_folder(CurrentFolder);
    sfree(ssd->searchfilter);
    ssd->searchfilter = dupstr("");
    dlg_refresh(ssd->listbox, dlg);
    if (ssd->commentbox)
        dlg_refresh(ssd->commentbox, dlg);
}

/*
 * KiTTY folder navigation: create the folder named in the session-name box.
 *
 * Same rules and the same engine as the combo's create path - CleanFolderName,
 * the reserved-name refusal, StringList_Add + SaveFolderList - so the two ways
 * in cannot diverge in what they accept. What differs is only where the name
 * comes from, and that this one then STEPS INTO the new folder: it is a row in
 * the list now, and creating something you cannot see would be a strange
 * result. The name box is cleared, because its text was a folder name and would
 * otherwise be left behind as a session name or a search filter.
 */
static void sessionsaver_create_named_folder(struct sessionsaver_data *ssd,
                                             dlgparam *dlg)
{
    char folder[1024];
    strncpy(folder, ssd->savedsession ? ssd->savedsession : "", sizeof(folder)-1);
    folder[sizeof(folder)-1] = '\0';
    CleanFolderName(folder);
    if (!folder[0]) {
        /* Nothing typed: put the caret where the name goes rather than just
         * beeping, so the button says what it wants instead of only refusing. */
        dlg_set_focus(ssd->editbox, dlg);
        return;
    }
    if (kitty_folder_name_reserved(folder)) {
        dlg_error_msg(dlg, KT_CFG_FOLDER_NAME_RESERVED_ROOT);
        return;
    }
    if (sessionsaver_folder_exists(folder)) {
        dlg_error_msg(dlg, KT_CFG_FOLDER_EXISTS);
        return;
    }
    InitFolderList();
    StringList_Add(FolderList, folder);
    SaveFolderList();
    sfree(ssd->savedsession);
    ssd->savedsession = dupstr("");
    sfree(ssd->searchfilter);
    ssd->searchfilter = dupstr("");
    sessionsaver_update_folder_button(ssd, dlg);
    dlg_refresh(ssd->editbox, dlg);
    /* Step into it. switch_folder refreshes the list for us; it compares against
     * CurrentFolder, so a folder created while already inside another one is
     * still a real change. */
    sessionsaver_switch_folder(ssd, dlg, folder);
    kitty_notify_launcher_sessions_changed();
}

/*
 * KiTTY folder navigation: activate the highlighted row if it is a NAVIGATION
 * row, and tell the caller whether it was one.
 *
 * true  - it was a folder or "..": the level changed, nothing was loaded, and
 *         the caller must stop.
 * false - an ordinary session row (or the mode is off), so the caller's usual
 *         load/launch path should run exactly as before.
 *
 * Both ways of activating a row - double-click and Enter - come through here,
 * so they cannot drift apart.
 */
static bool sessionsaver_enter_selected_folder(struct sessionsaver_data *ssd,
                                               dlgparam *dlg)
{
    int row, id;
    const char *folder;
    if (!kitty_folder_rows_active(ssd) || !ssd->listbox)
        return false;
    row = dlg_listbox_index(ssd->listbox, dlg);
    if (row < 0)
        return false;
    id = dlg_listbox_getid(ssd->listbox, dlg, row);
    if (id >= 0)
        return false;                   /* a session row: not one of ours */
    folder = kitty_folder_for_row_id(ssd, id);
    if (!folder)
        return false;
    {
        /* Take a COPY first. switch_folder refreshes the list, and the refresh
         * rebuilds ssd->folderrows - which is where `folder` points. Passing it
         * straight through is a use-after-free the moment the name outlives the
         * array it came from. */
        char *want = dupstr(folder);
        sessionsaver_switch_folder(ssd, dlg, want);
        sfree(want);
    }
    return true;
}

static void sessionsaver_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                 void *data, int event)
{
    Conf *conf = (Conf *)data;
    struct sessionsaver_data *ssd =
        (struct sessionsaver_data *)ctrl->context.p;

    if (event == EVENT_REFRESH) {
        if (ctrl == ssd->editbox) {
            /*
             * KiTTY: mid-session, start with THIS session's name in the box.
             *
             * Change Settings exists to adjust the session you are in, so the
             * name you almost always want is the one you are already running -
             * and leaving the field blank meant retyping it to save a tweak,
             * which is both a nuisance and a chance to mistype it over some
             * other session.
             *
             * Only when the session HAS a name: CONF_sessionname is empty for a
             * typed-in host and for Default Settings, and prefilling either would
             * invent a save target the user never chose.
             *
             * Done here rather than at panel-build time because setup_config_box()
             * has no Conf to read; this is the first place the name is available.
             */
            /* No longer midsession-only: the STARTUP box can hold a loaded
             * session too - the hotkey-conflict balloon opens it that way,
             * and loadlastsession does at every start. The session label
             * reads CONF_sessionname and said "loaded" while this box sat
             * empty and Save escalated to an overwrite warning about the
             * very session in force - two truths at once. One source:
             * the box shows the loaded name, the
             * list highlight follows it (the listbox refresh selects by
             * this field), and Save-back-to-it stops warning because
             * loaded_from now equals the box. */
            if (!ssd->savedsession[0]) {
                const char *sn = conf_get_str(conf, CONF_sessionname);
                if (sn && *sn) {
                    sfree(ssd->savedsession);
                    ssd->savedsession = dupstr(sn);
                    /* -cfgloaded (the hotkey balloon): the restored session
                     * is the one the box was opened to fix, so treat it as
                     * loaded and let Save write it back without the
                     * overwrite warning. An ordinary fresh box leaves
                     * loaded_from NULL on purpose - see the guard. */
                    if (kitty_cfgbox_loaded_deliberate && !ssd->loaded_from)
                        ssd->loaded_from = dupstr(sn);
                }
            }
            ssd->suppress_edit_valchange++;
            dlg_editbox_set(ctrl, dlg, ssd->savedsession);
            if (ssd->listbox && !ssd->midsession)
                dlg_editbox_set_updown_target(ctrl, ssd->listbox, dlg);
            ssd->suppress_edit_valchange--;
            if (!ssd->initial_focus_set && !ssd->midsession) {
                ssd->initial_focus_set = 1;
                /* Quick connect (hknet/KiTTY#23): the box has come up on
                 * Default Settings and the first thing wanted is a host, not a
                 * session name. dlg_set_focus_later() selects the box's
                 * contents, so a host left over from last time is replaced by
                 * typing and kept by pressing Enter. */
                dlg_set_focus_later((GetQuickConnectMode() &&
                                     quickconnect_host_ctrl) ?
                                    quickconnect_host_ctrl : ctrl, dlg);
                /* ...and say so in the title bar, for the box that comes UP in
                 * quick connect. The Load path marks it when the mode changes
                 * later; this is the one case that happens before any Load. */
                if (!GetPuttyFlag())
                    kitty_dlg_mark_quickconnect(dlg, GetQuickConnectMode());
            }
            /* Re-applied on every refresh, not just when the text changes: a
             * panel switch rebuilds the controls, and an EnableWindow() made
             * before that would be lost. */
            sessionsaver_update_save_button(ssd, dlg);
        } else if (ctrl == ssd->listbox) {
            int i;
            /*
             * KiTTY: mid-session the list opens on the RUNNING SESSION'S OWN
             * folder, not on the ambient browse state.
             *
             * CurrentFolder is otherwise seeded from the persisted LastFolder,
             * which names whatever was last browsed - possibly in an earlier
             * run - so Change Settings showed a folder that had nothing to do
             * with the session being changed. The folder comes from STORAGE for
             * the same reason the save does: the running Conf was filled at
             * launch and is stale if the session has been moved from another
             * window. CONF_folder is the fallback for a session that is not in
             * the store at all.
             *
             * Once per dialog: the user may still change folder with the combo
             * where there is one, and re-seeding on every refresh would undo
             * that. LastFolder is deliberately NOT written - a settings dialog
             * should not decide where the next config box opens.
             */
            if (ssd->midsession && !ssd->midsession_level_set &&
                !GetPuttyFlag()) {
                const char *sn = conf_get_str(conf, CONF_sessionname);
                const char *cf = conf_get_str(conf, CONF_folder);
                char *fld = (sn && *sn) ? kitty_read_session_folder(sn) : NULL;
                const char *lvl = (fld && *fld) ? fld :
                                  ((cf && *cf) ? cf : "Default");
                strncpy(CurrentFolder, lvl, 1023);
                CurrentFolder[1023] = '\0';
                sfree(fld);
                ssd->midsession_level_set = 1;
            }
            if (ssd->editbox && !ssd->midsession)
                dlg_editbox_set_updown_target(ssd->editbox, ctrl, dlg);
            dlg_update_start(ctrl, dlg);
            dlg_listbox_clear(ctrl, dlg);
            char lastsess[512];
            const char *filter = ssd->searchfilter ? ssd->searchfilter : "";
            bool searching = (!GetPuttyFlag() && filter[0]);
            int havelast = kitty_get_last_session(lastsess, sizeof(lastsess));
            int selpos = -1, lbpos = 0;
            /* Folder navigation: the rows of the current level go in first, and
             * the sessions follow. They are ordinary rows - selectable, reached
             * with the arrow keys, activated with Enter - not chrome. */
            kitty_rebuild_folder_rows(ssd, filter);
            sessionsaver_add_nav_rows(ctrl, dlg, ssd);
            lbpos = kitty_nav_row_count(ssd);
            int firstsession = -1;
            for (int pass = 2; pass >= 1; pass--) {
            for (i = 0; i < ssd->sesslist.nsessions; i++) {
                int smatch;
                /* KiTTY [ConfigBox] defaultsettings=no: hide "Default Settings"
                 * from the saved-session list. It still exists as the new-session
                 * template (loaded by name). Row IDs are session indices, not
                 * positions, so skipping it here keeps the selection mapping
                 * correct. */
                { extern int GetDefaultSettingsFlag(void);
                  if (!GetDefaultSettingsFlag() &&
                      !strcmp(ssd->sesslist.sessions[i], KITTY_DEFAULT_SESSION))
                      continue; }
                /* KiTTY folder filter: show only the sessions of the level
                 * being displayed, always keeping entry 0 ("Default Settings").
                 * What "this level" means differs between the classic combo and
                 * folder navigation - kitty_session_on_level knows, and
                 * sessionsaver_folder_visible_position asks the same function so
                 * the two can never disagree. */
                if (!kitty_session_on_level(ssd, i))
                    continue;
                smatch = sessionsaver_filter_match(ssd->sesslist.sessions[i], filter);
                if (searching && smatch != pass)
                    continue;
                sessionsaver_add_session_row(ctrl, dlg, ssd, i, searching);
                if (firstsession < 0)
                    firstsession = lbpos;
                /* Which row to auto-select: while searching, the exact typed
                 * name; otherwise the session currently in the name box (what
                 * the user last loaded/selected THIS dialog), falling back to
                 * the registry-remembered last session only while the box is
                 * still empty (i.e. at dialog open, matching the startup
                 * auto-load). Previously this always chose the remembered
                 * session, so a listbox refresh (e.g. returning from another
                 * panel) yanked the highlight off "Default Settings" back to
                 * the previously loaded session. */
                if ((searching ? !strcmp(ssd->sesslist.sessions[i], ssd->savedsession) :
                     (ssd->savedsession[0] ?
                      !strcmp(ssd->sesslist.sessions[i], ssd->savedsession) :
                      (havelast && !strcmp(ssd->sesslist.sessions[i], lastsess)))) &&
                    selpos < 0)
                    selpos = lbpos;
                lbpos++;
            }
            if (!searching)
                break;
            }
            dlg_update_done(ctrl, dlg);
            /* KiTTY: auto-select the best visible match (chosen above):
             * typed match while searching, else the current name-box session,
             * else the remembered last session; default to row 0.
             *
             * With folder rows present, row 0 is a NAVIGATION row, and the
             * default has to skip past them to the first session: Enter on the
             * highlighted row loads it, so highlighting ".." or a folder would
             * turn that keystroke into "go somewhere else" - the one thing the
             * user did not type for. Falling back to row 0 is still right when
             * the level holds no sessions at all, because then the only row
             * there is the way out. */
            if (selpos < 0 && kitty_folder_rows_active(ssd) && firstsession >= 0)
                selpos = firstsession;
            if (selpos < 0 && lbpos > 0)
                selpos = 0;
            if (selpos >= 0) {
                ssd->suppress_list_selchange++;
                dlg_listbox_select(ctrl, dlg, selpos);
                ssd->suppress_list_selchange--;
            }
            /* A rebuilt list forgets that a folder row was picked, so the button
             * goes back to "Save". The selection above is made with the change
             * event suppressed, so nothing else would clear it - and a stale
             * "Rename" pointing at a folder the user can no longer see is
             * exactly the silent-mode failure this design avoids. Clicking the
             * row again arms it, deliberately. */
            sessionsaver_end_folder_rename(ssd, dlg);
        }
        else if (ssd->folderlist && ctrl == ssd->folderlist) {
            /* Row 0 is the synthetic create row (id -1, never looked up); the
             * real folders follow, so a folder's row is its position + 1. */
            int i, sel = -1, pos = 1;
            char rootlabel[256];
            kitty_get_root_folder_label(rootlabel, sizeof(rootlabel));
            dlg_update_start(ctrl, dlg);
            dlg_listbox_clear(ctrl, dlg);
            if (ssd->createbutton)      /* creating needs the button to exist */
                dlg_listbox_addwithid(ctrl, dlg, KITTY_NEW_FOLDER_ITEM, -1);
            else
                pos = 0;
            for (i = 0; FolderList && FolderList[i] != NULL; i++)
                if (FolderList[i][0]) {
                    const char *disp = !strcmp(FolderList[i], "Default") ?
                        rootlabel : FolderList[i];
                    dlg_listbox_addwithid(ctrl, dlg, disp, i);
                    if (!strcmp(FolderList[i], CurrentFolder)) sel = pos;
                    pos++;
                }
            dlg_update_done(ctrl, dlg);
            /* Selecting a row writes it into the edit half, which fires a
             * re-entrant VALCHANGE; suppress it so a refresh never looks like
             * the user typing.
             *
             * A typed-but-not-yet-applied name SURVIVES the refresh. Losing
             * focus fires CBN_KILLFOCUS -> EVENT_REFRESH (windows/controls.c),
             * so re-selecting the row here would wipe what the user typed while
             * ssd->newfolder still held it - the button would then act on a
             * name no longer visible anywhere, which is exactly the confusion
             * this rework removes. Keep text and pending state in agreement. */
            ssd->suppress_folder_valchange++;
            if (ssd->newfolder && ssd->newfolder[0])
                dlg_editbox_set(ctrl, dlg, ssd->newfolder);
            else if (ssd->folder_new_selected && ssd->createbutton)
                dlg_listbox_select(ctrl, dlg, 0);   /* stay on the create row */
            else if (sel >= 0)
                dlg_listbox_select(ctrl, dlg, sel);
            ssd->suppress_folder_valchange--;
            sessionsaver_update_folder_button(ssd, dlg);
        }
        else if (ssd->commentbox && ctrl == ssd->commentbox) {
            update_comment_display(ssd, dlg);
        }
    } else if (event == EVENT_VALCHANGE) {
        int top, bottom, halfway, i;
        if (ctrl == ssd->editbox) {
            sfree(ssd->savedsession);
            ssd->savedsession = dlg_editbox_get(ctrl, dlg);
            /* Every keystroke: emptying the box must grey Save immediately, and
             * this runs before the early returns further down. */
            sessionsaver_update_save_button(ssd, dlg);
            /*
             * While a folder row is selected the box holds that folder's NEW
             * NAME, so typing must not also run the live search: the search
             * rebuilds the list, and a rebuild drops the selection - which
             * disarmed the rename halfway through typing it. The button then
             * said "Save" again and the click created a SESSION named after the
             * folder. Filtering is meaningless here anyway; picking any other
             * row ends the rename and restores it.
             */
            if (ssd->selected_folder && !ssd->suppress_edit_valchange)
                return;
            if (!ssd->suppress_edit_valchange && GetSessionFilterFlag()) {
                /* [ConfigBox] filter=no keeps searchfilter empty, so typing
                 * a name never narrows the saved-sessions list. */
                sfree(ssd->searchfilter);
                ssd->searchfilter = dupstr(ssd->savedsession);
                /* An emptied box does NOT end the Ctrl+G search any more:
                 * deleting a mistyped letter (or the whole word) to correct it
                 * is part of the same search, and with the box empty the level
                 * list and "search everywhere" show the same rows anyway. The
                 * search ends when the level changes or when focus comes back
                 * after leaving the box - kitty_config_ctrl_focus_gained(). */
                dlg_refresh(ssd->listbox, dlg);
                if (ssd->commentbox)
                    dlg_refresh(ssd->commentbox, dlg);
                return;
            }
            /* We put this text in the box ourselves (a click on the list, or a
             * refresh) - the user did not type it, so the type-ahead search
             * below must not move their selection. Without this, clicking a
             * row fired EN_CHANGE synchronously and the search dragged the
             * highlight straight off the row that was just clicked. */
            if (ssd->suppress_edit_valchange)
                return;
            /* The search below is a binary chop, so it needs a SORTED list -
             * but "Default Settings" is forced to index 0 and does not sort
             * with the rest ('D' comes after every digit). Any name starting
             * with a digit therefore steered the chop left every time, landing
             * on 0, and the guard further down turned that into 1: with
             * IP-address session names, every click selected the FIRST stored
             * session. Chop over the sorted part only. */
            bottom = -1;
            if (ssd->sesslist.nsessions > 1 &&
                !strcmp(ssd->sesslist.sessions[0], KITTY_DEFAULT_SESSION) &&
                strcmp(ssd->savedsession, KITTY_DEFAULT_SESSION) != 0)
                bottom = 0;
            top = ssd->sesslist.nsessions;
            while (top-bottom > 1) {
                halfway = (top+bottom)/2;
                i = strcmp(ssd->savedsession, ssd->sesslist.sessions[halfway]);
                if (i <= 0 ) {
                    top = halfway;
                } else {
                    bottom = halfway;
                }
            }
            if (top == ssd->sesslist.nsessions) {
                top -= 1;
            }
            /* KiTTY: "Default Settings" is forced to index 0, so a real
             * session name that sorts before it lands the binary search on 0.
             * Move the highlight to the first real stored session instead. */
            if (top == 0 && ssd->sesslist.nsessions > 1 &&
                !strcmp(ssd->sesslist.sessions[0], KITTY_DEFAULT_SESSION) &&
                ssd->savedsession[0] &&
                strcmp(ssd->savedsession, KITTY_DEFAULT_SESSION) != 0)
                top = 1;
            /* `top` is a session INDEX from the binary search; the listbox wants a
             * VISIBLE ROW, which differs once a folder filter or defaultsettings=no
             * hides rows. Convert it (was the off-by-one that jumped the highlight
             * one row down per click when Default Settings is hidden). */
            { int row = sessionsaver_folder_visible_position(ssd, top);
              dlg_listbox_select(ssd->listbox, dlg, row >= 0 ? row : top); }
        }
        else if (ssd->folderlist && ctrl == ssd->folderlist) {
            char *text;
            if (ssd->suppress_folder_valchange)
                return;              /* our own dlg_listbox_select/editbox_set */
            text = dlg_editbox_get(ctrl, dlg);
            if (ssd->createbutton && !strcmp(text, KITTY_NEW_FOLDER_ITEM)) {
                /* The synthetic create row: select its label so the first
                 * keystroke replaces it. Nothing pending until then - the
                 * label itself is never treated as a folder name. */
                ssd->folder_new_selected = 1;
                sfree(ssd->newfolder);
                ssd->newfolder = dupstr("");
                ssd->suppress_folder_valchange++;
                kitty_dlg_combobox_select_all(ctrl, dlg);
                ssd->suppress_folder_valchange--;
            } else {
                /* Compare against what the combo currently DISPLAYS, exactly:
                 * anything else - including a change of case only - is a name
                 * the user wants to act on, not the selection they already had.
                 */
                bool root_sel = (!CurrentFolder[0] ||
                                 !strcmp(CurrentFolder, "Default"));
                char rootlabel[256];
                const char *curdisp;
                int idx;
                kitty_get_root_folder_label(rootlabel, sizeof(rootlabel));
                curdisp = root_sel ? rootlabel : CurrentFolder;
                idx = strcmp(text, curdisp) ?
                    sessionsaver_folder_index_for_text(text, !root_sel) : -2;
                if (idx >= 0 && !strcmp(FolderList[idx], CurrentFolder))
                    idx = -1;    /* a different spelling of what is selected is
                                  * a rename of it, not a re-selection of it */
                if (idx == -2) {
                    /* Unchanged. Clear any pending name: this used to be left
                     * set, so text that was typed and then reverted (e.g. the
                     * combo restoring the selection on focus change) was still
                     * acted on by the button, under a name no longer visible
                     * anywhere. */
                    ssd->folder_new_selected = 0;
                    sfree(ssd->newfolder);
                    ssd->newfolder = dupstr("");
                } else if (idx >= 0) {
                    sessionsaver_switch_folder(ssd, dlg, FolderList[idx]);
                    ssd->folder_new_selected = 0;
                    sfree(ssd->newfolder);
                    ssd->newfolder = dupstr("");
                } else {
                    sfree(ssd->newfolder);
                    ssd->newfolder = dupstr(text);
                }
            }
            sessionsaver_update_folder_button(ssd, dlg);   /* recomputes first */
            sfree(text);
        }
    } else if (event == EVENT_SELCHANGE && ctrl == ssd->listbox) {
        /* KiTTY: single-clicking a saved session copies its name into the
         * "Saved Sessions" edit box, so Save/Load act on it without retyping
         * (e.g. selecting "Default Settings" lets you re-save it directly).
         * Also refreshes the read-only comment display. */
        int i = sessionsaver_selected_session_index(ssd, dlg);
        /*
         * KiTTY: selecting a FOLDER row puts its name in the box and turns Save
         * into Rename. Type a new name, press it, and the folder is renamed.
         *
         * Derived from the selection rather than armed by it: clicking a
         * session, stepping into a folder, searching, creating or deleting one
         * all move or rebuild the selection, and each of those either lands
         * here again or clears this in the list's refresh. An "armed" flag
         * would need every one of those exits handled by hand, and the failure
         * mode - Save silently meaning Rename - is the one worth engineering
         * out rather than remembering.
         */
        if (!ssd->suppress_list_selchange && kitty_folder_rows_active(ssd)) {
            const char *fld = NULL;
            int row = dlg_listbox_index(ssd->listbox, dlg);
            if (row >= 0) {
                int id = dlg_listbox_getid(ssd->listbox, dlg, row);
                /* ".." is a folder row too, but it names the level above rather
                 * than a folder anyone can rename. */
                if (id < 0 && id != KITTY_ROW_PARENT)
                    fld = kitty_folder_for_row_id(ssd, id);
            }
            sfree(ssd->selected_folder);
            ssd->selected_folder = fld ? dupstr(fld) : NULL;
            if (!fld) {
                /* Moving to a session row ends any rename. Drop the marker
                 * here rather than leaving it: a folder and a session may share
                 * a name ("tests/" and "tests" are different things and both
                 * show at the root), and a stale marker would then match the
                 * session name the user has just deliberately chosen - and take
                 * it back out of the box on the next rebuild. */
                sfree(ssd->folder_text_in_box);
                ssd->folder_text_in_box = NULL;
            }
            if (ssd->selected_folder) {
                sfree(ssd->savedsession);
                ssd->savedsession = dupstr(ssd->selected_folder);
                /* Remember what we put there, so it can be taken back out if
                 * the rename ends without the user having edited it. */
                sfree(ssd->folder_text_in_box);
                ssd->folder_text_in_box = dupstr(ssd->selected_folder);
                sfree(ssd->searchfilter);
                ssd->searchfilter = dupstr("");
                dlg_refresh(ssd->editbox, dlg);
            }
            sessionsaver_update_save_button(ssd, dlg);
        }
        if (!ssd->suppress_list_selchange && i >= 0 && i < ssd->sesslist.nsessions) {
            if (ssd->searchfilter && ssd->searchfilter[0]) {
                /* In live-search mode, arrowing through the filtered list must
                 * not overwrite the user's search text or collapse the filter.
                 * Load/Enter makes the highlighted result the saved-session
                 * edit value explicitly. */
            } else {
                sfree(ssd->savedsession);
                ssd->savedsession = dupstr(ssd->sesslist.sessions[i]);
                sfree(ssd->searchfilter);
                ssd->searchfilter = dupstr("");
                dlg_refresh(ssd->editbox, dlg);
            }
        }
        if (ssd->commentbox)
            update_comment_display(ssd, dlg);
    } else if (event == EVENT_ACTION) {
        bool mbl = false;
        /* KiTTY folder navigation: a double-click on a folder row STEPS INTO
         * it - it does not load anything, so the name box, the loaded session
         * and the Save target are all left alone. Handled before the load path
         * below rather than inside it, because "activate this row" means two
         * different things now and only the id says which. */
        if (!ssd->midsession && ctrl == ssd->listbox &&
            sessionsaver_enter_selected_folder(ssd, dlg))
            return;
        if (!ssd->midsession &&
            (ctrl == ssd->listbox ||
             (ssd->loadbutton && ctrl == ssd->loadbutton))) {
            /*
             * The user has double-clicked a session, or hit Load.
             * We must load the selected session, and then
             * terminate the configuration dialog _if_ there was a
             * double-click on the list box _and_ that session
             * contains a hostname.
             */
            if (load_selected_session(ssd, dlg, conf, &mbl) &&
                (mbl && ctrl == ssd->listbox && conf_launchable(conf))) {
                /* [ConfigBox] dblclick=start: double-click acts like the Start
                 * button - launch in a new window, keep the config box open.
                 * Default (open) falls through to stock behaviour. */
                if (GetDblClickFlag() == 1) {
                    RunConfig(conf);
                    return;
                }
                dlg_end(dlg, 1);       /* it's all over, and succeeded */
            }
        } else if (ctrl == ssd->savebutton) {
            bool isdef;
            /* KiTTY: with a folder row selected this button is "Rename" and
             * acts on the folder, not on a session. Checked before anything
             * reads savedsession as a save target - it holds the FOLDER name
             * here, and saving a session under it would be the accident this
             * whole route has to avoid. */
            if (ssd->selected_folder) {
                char *oldname = dupstr(ssd->selected_folder);
                bool done = sessionsaver_rename_folder(ssd, dlg, conf, oldname,
                                                       ssd->savedsession);
                sfree(oldname);
                if (done) {
                    /* The list was rebuilt under us, which cleared the
                     * selection: the button is "Save" again and the box holds
                     * the new name, ready to be picked afresh. */
                    sfree(ssd->savedsession);
                    ssd->savedsession = dupstr("");
                    dlg_refresh(ssd->editbox, dlg);
                    sessionsaver_update_save_button(ssd, dlg);
                }
                return;
            }
            isdef = !strcmp(ssd->savedsession, KITTY_DEFAULT_SESSION);
            /* An EMPTY name box means the target is being taken from whichever
             * row happens to be highlighted, and the user was never shown it:
             * clicking a row copies its name INTO the box, so an empty box means
             * no row was clicked in this dialog - the highlight is the one this
             * box restored by itself. Reported live: a fresh box with settings
             * typed into it, Save, and those settings landed in the last-used
             * session with no prompt at all. Force the confirmation for this
             * path whatever else says it is safe. */
            bool target_from_highlight = !ssd->savedsession[0];
            if (!ssd->savedsession[0]) {
                /* The VISIBLE row index is not the index into sesslist: a folder
                 * filter or a search filter shows a subset, and the id attached
                 * to each row is what maps back. Without the mapping, saving
                 * with an empty name box while a folder was selected picked
                 * whatever sat at that position in the UNFILTERED list - so the
                 * overwrite warning named a session the user could not see, and
                 * agreeing to it would have written over that one. The delete
                 * path has always mapped; this one did not. */
                int i = sessionsaver_selected_session_index(ssd, dlg);
                if (i < 0 || i >= ssd->sesslist.nsessions) {
                    dlg_beep(dlg);
                    return;
                }
                isdef = !strcmp(ssd->sesslist.sessions[i], KITTY_DEFAULT_SESSION);
                sfree(ssd->savedsession);
                ssd->savedsession = dupstr(isdef ? "" :
                                           ssd->sesslist.sessions[i]);
            }
            {
                /* Which folder does this save file the session under?
                 *
                 * The folder combo is the list filter, so it must NOT quietly
                 * re-file a session just because of what was being viewed:
                 * loading a session from one folder while looking at another
                 * and pressing Save used to move it. It is however the only
                 * way to move a session between folders, so a selection the
                 * user CHANGED after loading still counts as "put it there".
                 *
                 * "Default Settings" is never filed anywhere: the filter shows
                 * it under every folder, so a stored folder on it is invisible
                 * yet still feeds the folder-list rebuild, which resurrects
                 * deleted and renamed folders. Normalising it here also clears
                 * any such value left by earlier versions. */
                /*
                 * KiTTY: mid-session, Save never re-files.
                 *
                 * CurrentFolder is the ambient BROWSE state, seeded from the
                 * persisted LastFolder whenever a config box is built, so
                 * mid-session it names whatever folder was last browsed - quite
                 * possibly in an earlier run - and never the session's own.
                 * Treating that as a choice moved sessions silently: start one
                 * from a shortcut, change a setting, Save, and it was re-filed
                 * into an unrelated folder; Ctrl+G re-filed it to the root,
                 * because searching sets CurrentFolder.
                 *
                 * So mid-session the folder comes from STORAGE, read here at
                 * save time. Not from the running Conf: that was filled at
                 * launch and goes stale the moment the session is moved from
                 * another window, and writing it back would silently undo the
                 * move. A name that does not exist yet has no stored folder and
                 * keeps the running session's, so a copy lands beside the
                 * session it was copied from.
                 *
                 * Moving a session stays the startup config box's job, where the
                 * folder is on screen and choosing it is an act of its own.
                 */
                if (ssd->midsession && !GetPuttyFlag()) {
                    if (isdef) {
                        conf_set_str(conf, CONF_folder, "Default");
                    } else if (ssd->savedsession[0]) {
                        char *stored =
                            kitty_read_session_folder(ssd->savedsession);
                        if (stored && *stored)
                            conf_set_str(conf, CONF_folder, stored);
                        sfree(stored);
                    }
                } else if (kitty_folders_available(ssd)) {
                    const char *cur = !CurrentFolder[0] ? "Default" : CurrentFolder;
                    if (isdef)
                        conf_set_str(conf, CONF_folder, "Default");
                    else if (!ssd->folder_at_load ||
                             strcmp(cur, ssd->folder_at_load))
                        conf_set_str(conf, CONF_folder, cur);
                    /* else: keep the folder the session was loaded with */
                }
                /*
                 * KiTTY: confirm before REPLACING a session you never loaded.
                 *
                 * The accident this stops, reported with an exact repro: load
                 * and start session A, then single-click session B in the list
                 * and press Save. Clicking a name only copies the NAME into the
                 * edit box - it does not load that session - so Save writes A's
                 * entire configuration over B. Host, port, protocol, everything,
                 * with no prompt and nothing on screen to say it happened. Stock
                 * PuTTY has always behaved this way; it is still somebody's whole
                 * session gone, and the person it happened to could not tell what
                 * they had done wrong.
                 *
                 * Asked only when BOTH hold: the target already exists, and it is
                 * not what was loaded into this box. So "load it, change it, save
                 * it" never sees this, nor does saving under a new name, nor two
                 * saves in a row. Default Settings is exempt - a freshly opened
                 * box has loaded nothing, and prompting there would nag the one
                 * save people make most deliberately.
                 */
                /*
                 * Mid-session (Change Settings) the box was never "Loaded" from
                 * the list - the settings came from the RUNNING SESSION - so
                 * loaded_from is NULL and the guard below would warn about
                 * saving your own session back over itself. That is the most
                 * ordinary thing to do mid-session, and a warning there is
                 * worse than no warning at all: it makes a safe action look
                 * destructive. Seed it from the session's own name instead.
                 */
                /* Only MID-SESSION, which is the case this seeding is for. In a
                 * fresh config box CONF_sessionname can still name the session
                 * that was restored at startup, and seeding from it there made
                 * the guard below believe that session had been loaded - so a
                 * box the user had typed new settings into saved over it in
                 * silence. */
                if (ssd->midsession && !ssd->loaded_from) {
                    const char *sn = conf_get_str(conf, CONF_sessionname);
                    if (sn && *sn)
                        ssd->loaded_from = dupstr(sn);
                }
                if (!isdef && ssd->savedsession[0] &&
                    (target_from_highlight ||
                     !ssd->loaded_from ||
                     strcmp(ssd->loaded_from, ssd->savedsession) != 0)) {
                    /*
                     * The PRIMARY store only. open_settings_r() walks the
                     * precedence chain (ours, then the old 9bis KiTTY hive,
                     * then stock PuTTY), so a name that was free in the list
                     * - foreign sessions hidden - still answered "exists" for
                     * a session living only in an old store, and the box
                     * threatened to replace something Save never touches.
                     */
                    if (kitty_own_session_exists(ssd->savedsession)) {
                        char *q = target_from_highlight ? dupprintf(
                            KT_CFG_OVERWRITE_HIGHLIGHT_Q,
                            ssd->savedsession) : dupprintf(
                            KT_CFG_OVERWRITE_UNLOADED_Q,
                            ssd->savedsession);
                        bool go = kitty_dlg_confirm(
                            dlg, KT_CAP_OVERWRITE_SESSION, q);
                        sfree(q);
                        if (!go)
                            return;
                    } else if (!kitty_root_is_putty() &&
                               kitty_get_show_foreign_sessions()) {
                        /*
                         * Only an old store has the name, and that store is
                         * shown in the list: the new session is saved beside
                         * the old one and hides it from now on (loading is
                         * first-hive-wins). Not a replace - the prompt
                         * states what happens. With foreign sessions hidden
                         * the old one is not on screen, so no prompt.
                         */
                        const char *where = NULL;
                        settings_r *f = kitty_open_settings_r_hive(
                            ssd->savedsession, KSEC_HIVE_OLDKITTY);
                        if (f) {
                            where = KT_CFG_HIDE_FOREIGN_OLDKITTY;
                        } else {
                            f = kitty_open_settings_r_hive(
                                ssd->savedsession, KSEC_HIVE_PUTTY);
                            if (f)
                                where = KT_CFG_HIDE_FOREIGN_PUTTY;
                        }
                        if (f) {
                            close_settings_r(f);
                            char *q = dupprintf(KT_CFG_HIDE_FOREIGN_Q,
                                                ssd->savedsession, where);
                            bool go = kitty_dlg_confirm(
                                dlg, KT_CAP_HIDE_FOREIGN_SESSION, q);
                            sfree(q);
                            if (!go)
                                return;
                        }
                    }
                }
#ifdef MOD_LAUNCHER
                /* A launcher hotkey is a machine-wide claim, so saving one is
                 * where a conflict is CREATED - and where it must be said.
                 * Two cases, decided against the store before the write:
                 * over the slot limit, the hotkey is disabled and the rest of
                 * the save goes through (silently keeping a hotkey that can
                 * never register would be worse); merely duplicated, the save
                 * is untouched and a warning after it names the other holders
                 * (the box may hold settings mid-edit - do not rewrite them). */
                unsigned int hk_mods = 0, hk_vk = 0;
                int hk_warn = 0;
                if (ssd->savedsession[0] &&
                    conf_get_bool(conf, CONF_launcher_global_hotkey_enabled) &&
                    kitty_parse_hotkey_spec(
                        conf_get_str(conf, CONF_launcher_global_hotkey),
                        &hk_mods, &hk_vk)) {
                    if (kitty_hotkey_enabled_count(ssd->savedsession) >=
                        KITTY_LAUNCHER_HOTKEY_MAX) {
                        char m[300];
                        snprintf(m, sizeof(m),
                                 KT_CFG_HOTKEY_SLOTS_FULL,
                                 KITTY_LAUNCHER_HOTKEY_MAX);
                        MessageBox(GetActiveWindow(), m,
                                   KT_CAP_LAUNCHER_HOTKEY,
                                   MB_OK | MB_ICONWARNING);
                        conf_set_bool(conf,
                                      CONF_launcher_global_hotkey_enabled,
                                      false);
                        dlg_refresh(NULL, dlg);
                    } else {
                        hk_warn = 1;
                    }
                }
#endif
                /* Back up the store before OVERWRITING a saved session, so its
                 * previous contents stay recoverable. Blocking on purpose - an
                 * async snapshot could land after the write. Skipped when the
                 * name does not exist yet: a new session overwrites nothing, so
                 * there is nothing to preserve, and the routine snapshot after
                 * the save records the addition anyway. An empty name means
                 * Default Settings, which does exist and is worth preserving. */
                {
                    settings_r *existing = open_settings_r(ssd->savedsession);
                    if (existing) {
                        close_settings_r(existing);
                        SaveRegistryKeyNow();
                    }
                }
                char *errmsg = save_settings(ssd->savedsession, conf);
                if (errmsg) {
                    dlg_error_msg(dlg, errmsg);
                    sfree(errmsg);
                } else {
                    /* KiTTY: a Save under a NEW name is a copy, and the
                     * loaded session's remembered positions - every monitor
                     * layout's entry - go with it, so the copy opens where
                     * its original does. */
                    if (ssd->savedsession[0] && ssd->loaded_from &&
                        strcmp(ssd->loaded_from, ssd->savedsession) != 0)
                        kitty_winpos_session_copy(ssd->loaded_from, ssd->savedsession);
                    /* What is in the box now IS this session, so a second Save
                     * in a row must not ask to overwrite it again. */
                    if (ssd->savedsession[0]) {
                        sfree(ssd->loaded_from);
                        ssd->loaded_from = dupstr(ssd->savedsession);
                    }
                    /* KiTTY: Top/Left and Columns/Rows edit this session's
                     * remembered entry for the current monitor layout, so a
                     * Save writes it back too, after the copy, so what the
                     * panel shows wins for this layout (a named session only;
                     * Default Settings has no entry of its own). */
                    kitty_winpos_save_from_conf(ssd->savedsession, conf);
                    /* Tell a running KiTTY Launcher to refresh its saved-session
                     * list and re-register per-session global hotkeys. This is a
                     * best-effort broadcast; if no launcher is running, nothing
                     * happens and the next launcher start reads the new settings. */
                    kitty_notify_launcher_sessions_changed();
#ifdef MOD_LAUNCHER
                    /* Warn AFTER the save (the save itself is fine and kept)
                     * when other saved sessions hold this same hotkey. */
                    if (hk_warn) {
                        char hk_others[512];
                        if (kitty_hotkey_conflict_scan(hk_mods, hk_vk,
                                                       ssd->savedsession,
                                                       hk_others,
                                                       sizeof(hk_others)) > 0) {
                            char *m = dupprintf(
                                KT_CFG_HOTKEY_ALSO_ASSIGNED,
                                conf_get_str(conf, CONF_launcher_global_hotkey),
                                hk_others);
                            MessageBox(GetActiveWindow(), m,
                                       KT_CAP_LAUNCHER_HOTKEY,
                                       MB_OK | MB_ICONWARNING);
                            sfree(m);
                        }
                    }
#endif
                }
            }
            get_sesslist(&ssd->sesslist, false);
            get_sesslist(&ssd->sesslist, true);
            kitty_session_folder_cache_clear();
            dlg_refresh(ssd->editbox, dlg);
            /* KiTTY: remember the just-saved session so the listbox refresh
             * auto-selects it (with the correct visible index, even when a
             * folder filter is active). Skip the default settings pseudo-session. */
            if (ssd->savedsession && ssd->savedsession[0] &&
                strcmp(ssd->savedsession, KITTY_DEFAULT_SESSION) != 0)
                kitty_set_last_session(ssd->savedsession);
            dlg_refresh(ssd->listbox, dlg);
            /* KiTTY: the read-only comment display reads the comment back from
             * the STORE, so editing the comment on the Comment panel and saving
             * left it showing the previous text - the box looked as though the
             * edit had not been saved. The store has just been written, so
             * re-read it here. */
            if (ssd->commentbox)
                update_comment_display(ssd, dlg);
        } else if (!ssd->midsession &&
                   ssd->delbutton && ctrl == ssd->delbutton) {
            int i = dlg_listbox_index(ssd->listbox, dlg);
            if (i >= 0) i = dlg_listbox_getid(ssd->listbox, dlg, i);
            if (i == 0) {
                /* KiTTY: "Default Settings" is the template every new session
                 * starts from, so it cannot be deleted - but silently beeping
                 * looks like a broken button. Explain, and offer the one thing
                 * the user can actually have: hide it from the list. */
                sessionsaver_offer_hide_default(ssd, dlg);
            } else if (i < 0) {
                dlg_beep(dlg);          /* nothing selected */
            } else {
                /* Deleting a session is the case a backup exists for. Must be
                 * on disk BEFORE the delete, hence the blocking variant. */
                SaveRegistryKeyNow();
                del_settings(ssd->sesslist.sessions[i]);
                get_sesslist(&ssd->sesslist, false);
                get_sesslist(&ssd->sesslist, true);
                kitty_session_folder_cache_clear();
                dlg_refresh(ssd->listbox, dlg);
            }
        } else if (!ssd->midsession &&
                   ssd->createbutton && ctrl == ssd->createbutton) {
            /* Dual-purpose button - its label always says which one applies
             * (sessionsaver_update_folder_button). The name comes from the
             * folder combo's edit half, never from the Saved Sessions field
             * (which holds the session name). */
            /*
             * KiTTY folder navigation: there IS no combo, so the name comes
             * from the session-name box - the third meaning that field gets
             * back once the combo is gone. Typing a name and pressing this
             * button says what the user wants plainly enough; an earlier
             * version made the first click "arm" the field and the second
             * create, which guarded against creating a folder out of a live
             * search filter and cost a hidden mode on every single use to do it.
             * The text that is there is the name.
             */
            if (kitty_folder_rows_on()) {
                sessionsaver_create_named_folder(ssd, dlg);
                return;
            }
            if (!ssd->newfolder || !ssd->newfolder[0]) {
                dlg_beep(dlg);
            } else if (ssd->folder_action == KITTY_FOLDER_ACTION_RELABEL) {
                /* Rename the ROOT LIST'S LABEL only. Display-only: the stored
                 * folder key stays "Default", so nothing that filters on it
                 * changes. Not run through CleanFolderName - it is never used
                 * as a folder name, let alone a directory name. */
                char label[256];
                strncpy(label, ssd->newfolder, sizeof(label)-1);
                label[sizeof(label)-1] = '\0';
                if (!stricmp(label, KITTY_ROOT_FOLDER_LABEL_DEFAULT) ||
                    !stricmp(label, "All sessions") ||
                    !stricmp(label, "root")) {
                    /* Typing the built-in name back RESTORES the default label.
                     * Without this the reserved-name check below would make the
                     * default the one name you could never return to once you
                     * had renamed it. Stored empty = "use the default". */
                    char reset[1] = "";
                    WriteParameter(INIT_SECTION, KI_ROOTFOLDERLABEL, reset);
                    sfree(ssd->newfolder);
                    ssd->newfolder = dupstr("");
                    dlg_refresh(ssd->folderlist, dlg);
                } else if (!stricmp(label, "Default") ||
                           !stricmp(label, KITTY_NEW_FOLDER_ITEM)) {
                    /* NOT the full reserved-name check: that one also refuses
                     * the current label, which would block re-casing it
                     * ("Alle Sessions" -> "alle sessions"). */
                    dlg_error_msg(dlg, KT_CFG_FOLDER_NAME_RESERVED);
                } else if (sessionsaver_folder_exists(label)) {
                    dlg_error_msg(dlg, KT_CFG_FOLDER_EXISTS);
                } else {
                    WriteParameter(INIT_SECTION, KI_ROOTFOLDERLABEL, label);
                    sfree(ssd->newfolder);
                    ssd->newfolder = dupstr("");
                    dlg_refresh(ssd->folderlist, dlg);
                }
            } else if (ssd->folder_action == KITTY_FOLDER_ACTION_RENAME) {
                /* The combo renames the folder currently being browsed. */
                (void)sessionsaver_rename_folder(ssd, dlg, conf, CurrentFolder,
                                                 ssd->newfolder);
            } else {
                /* Create. StringList_Add dedupes internally. */
                char folder[1024];
                strncpy(folder, ssd->newfolder, sizeof(folder)-1);
                folder[sizeof(folder)-1] = '\0';
                CleanFolderName(folder);
                if (!folder[0]) {
                    dlg_beep(dlg);
                } else if (kitty_folder_name_reserved(folder)) {
                    dlg_error_msg(dlg, KT_CFG_FOLDER_NAME_RESERVED_ROOT);
                } else {
                    InitFolderList();
                    StringList_Add(FolderList, folder);
                    SaveFolderList();
                    strncpy(CurrentFolder, folder, 1023);
                    CurrentFolder[1023] = '\0';
                    kitty_set_last_folder(CurrentFolder);
                    /* The new folder is now the SELECTION, not a pending
                     * create: clear the pending name so the refresh below
                     * selects its row instead of restoring typed text, and so
                     * a second click can't act on it again. */
                    sfree(ssd->newfolder);
                    ssd->newfolder = dupstr("");
                    ssd->folder_new_selected = 0;
                    ssd->folder_action = KITTY_FOLDER_ACTION_NEW;
                    dlg_refresh(ssd->folderlist, dlg);
                    dlg_refresh(ssd->listbox, dlg);
                }
            }
        } else if (!ssd->midsession &&
                   ssd->delfolderbutton && ctrl == ssd->delfolderbutton) {
            /* Delete the currently selected folder. */
            /* Back up before the chain below is even evaluated: its condition
             * has side effects - the member-count branch already rewrites the
             * sessions' Folder values - so a backup taken later would miss
             * exactly the state worth keeping. It therefore also runs when the
             * delete is refused or declined, which is a cheap price. */
            SaveRegistryKeyNow();
            if (!CurrentFolder[0] || !strcmp(CurrentFolder, "Default")) {
                kitty_root_folder_cannot_delete(dlg);
            } else if (sessionsaver_folder_member_count(ssd) > 0 ?
                       !sessionsaver_confirm_empty_folder(ssd, dlg) :
                       /* No members, but "Default Settings" may still carry
                        * this folder - invisible in the UI, yet enough to
                        * rebuild the folder straight back. Clear it. */
                       sessionsaver_move_folder_sessions(ssd, dlg,
                                                         CurrentFolder,
                                                         "Default") < 0) {
                /* Declined, or the rewrite failed - either way the folder
                 * keeps something pointing at it and must NOT be dropped from
                 * the list: a folder is not a container, so InitFolderList()
                 * would rebuild it from those "Folder" values and the delete
                 * would only look like it worked. */
            } else {
                StringList_Del(FolderList, CurrentFolder);
                SaveFolderList();
                InitFolderList();
                strcpy(CurrentFolder, "Default");
                kitty_set_last_folder(CurrentFolder);
                sfree(ssd->savedsession);
                ssd->savedsession = dupstr("");
                dlg_refresh(ssd->editbox, dlg);
                dlg_refresh(ssd->folderlist, dlg);
                dlg_refresh(ssd->listbox, dlg);
            }
        } else if (ctrl == ssd->okbutton) {
            /* Enter with a folder row highlighted steps into it, exactly as a
             * double-click does. This has to come before the keyboard-hub flow
             * below, which would otherwise try to launch the highlighted row -
             * and before the search-filter branch, so that Enter in the name
             * box reaches a folder the filter has narrowed to. */
            if (!ssd->midsession &&
                dlg_last_focused(ctrl, dlg) == ssd->listbox &&
                sessionsaver_enter_selected_folder(ssd, dlg))
                return;
            if (!ssd->midsession &&
                (dlg_last_focused(ctrl, dlg) == ssd->editbox ||
                 dlg_last_focused(ctrl, dlg) == ssd->listbox)) {
                if (dlg_is_focused(ctrl, dlg)) {
                    /* The user went to the Open button itself (mouse click,
                     * or Tab + activate): classic Open - the resolved target
                     * opens in THIS window and the config box closes. */
                    if (!sessionsaver_resolve_launch_target(ssd, dlg, conf,
                                                            ctrl))
                        return;
                    if (conf_launchable(conf))
                        dlg_end(dlg, 1);
                    else
                        dlg_beep(dlg);
                    return;
                }
                /* Enter from the name box or the list: the keyboard hub
                 * flow - sessions start in a new window, the box stays. */
                if (ssd->searchfilter && ssd->searchfilter[0]) {
                    /* Enter while searching selects (loads), it does not
                     * launch; a second Enter starts the loaded session. */
                    bool loaded = load_selected_session(ssd, dlg, conf, NULL);
                    if (loaded)
                        dlg_set_focus(ssd->editbox, dlg);
                    else
                        dlg_beep(dlg);
                    return;
                }
                if (!sessionsaver_resolve_launch_target(ssd, dlg, conf,
                                                        ctrl))
                    return;
                if (conf_launchable(conf))
                    RunConfig(conf);
                else
                    dlg_beep(dlg);
                return;
            }
            if (ssd->midsession) {
                /* In a mid-session Change Settings, Apply is always OK. */
                dlg_end(dlg, 1);
                return;
            }
            /*
             * Annoying special case. If the `Open' button is
             * pressed while no host name is currently set, _and_
             * the session list previously had the focus, _and_
             * there was a session selected in that which had a
             * valid host name in it, then load it and go.
             */
            if (dlg_last_focused(ctrl, dlg) == ssd->listbox &&
                !conf_launchable(conf) && dlg_is_visible(ssd->listbox, dlg)) {
                Conf *conf2 = conf_new();
                bool mbl = false;
                if (!load_selected_session(ssd, dlg, conf2, &mbl)) {
                    dlg_beep(dlg);
                    conf_free(conf2);
                    return;
                }
                /* If at this point we have a valid session, go! */
                if (mbl && conf_launchable(conf2)) {
                    conf_copy_into(conf, conf2);
                    dlg_end(dlg, 1);
                } else
                    dlg_beep(dlg);

                conf_free(conf2);
                return;
            }

            /*
             * Otherwise, do the normal thing: if we have a valid
             * session, get going.
             */
            if (conf_launchable(conf)) {
                dlg_end(dlg, 1);
            } else
                dlg_beep(dlg);
        } else if (ctrl == ssd->cancelbutton) {
            dlg_end(dlg, 0);
        }
        else if (ssd->startbutton && ctrl == ssd->startbutton) {
            /* Launch in a new window; keep box open. Shares the launch-intent
             * rule with the Open button: a highlighted search match or a
             * just-selected list row is loaded first, anything else starts
             * the current settings as they are. */
            if (!sessionsaver_resolve_launch_target(ssd, dlg, conf, ctrl))
                return;
            if (conf_launchable(conf))
                RunConfig(conf);
            else
                dlg_beep(dlg);
        }
    }
}

/* KiTTY: the CA editor is a panel of the Application tab now (Security >
 * Certificate Authorities) - CAs are stored once per user, not per session -
 * so the button on the session's Host keys panel JUMPS there instead of
 * opening upstream's pop-up. */
static void host_ca_jump_handler(dlgcontrol *ctrl, dlgparam *dp,
                                 void *data, int event)
{
    if (event == EVENT_ACTION)
        kitty_cfg_goto_panel("Application/Security/Certificate Authorities");
}

/* KiTTY: the WinSCP executable path is an application setting (KiTTY++
 * Settings > Transfers & Tools > WinSCP); the session's WinSCP panel carries a
 * button that jumps there. */
static void winscp_global_jump_handler(dlgcontrol *ctrl, dlgparam *dp,
                                       void *data, int event)
{
    if (event == EVENT_ACTION)
        kitty_cfg_goto_panel("Application/KiTTY++ Settings/Transfers & Tools/WinSCP");
}

static void filezilla_global_jump_handler(dlgcontrol *ctrl, dlgparam *dp,
                                          void *data, int event)
{
    if (event == EVENT_ACTION)
        kitty_cfg_goto_panel("Application/KiTTY++ Settings/Transfers & Tools/FileZilla");
}

/* KiTTY: the file-copy helper (kscp path, port, folders) lives on KiTTY++
 * Settings > Transfers & Tools; the session's KSCP panel jumps there. */
static void kscp_global_jump_handler(dlgcontrol *ctrl, dlgparam *dp,
                                     void *data, int event)
{
    if (event == EVENT_ACTION)
        kitty_cfg_goto_panel("Application/KiTTY++ Settings/Transfers & Tools");
}

/* KiTTY: the broadcast master switch, the installation's group key and the
 * send console live on KiTTY++ Settings > Automation > Broadcast; the
 * session's Broadcast panel carries a button that jumps there. */
static void broadcast_global_jump_handler(dlgcontrol *ctrl, dlgparam *dp,
                                          void *data, int event)
{
    if (event == EVENT_ACTION)
        kitty_cfg_goto_panel("Application/KiTTY++ Settings/Automation/Broadcast");
}

/* ---- Connection > SSH > Host keys: "Scan this host" ------------------------ */

/*
 * A secondary box (windows/dialog.c, kitty_cfg_show_aux_box) that asks the
 * session's host for every key type it has - one hidden klink per type in the
 * background, as Verify does - and lets the user take the answers into the
 * store: Accept stores a new key, or replaces a differing one after a second
 * question that names both fingerprints; Decline leaves the store as it is
 * and, on a MISMATCH, offers to delete the stored key so the next connection
 * asks afresh. Nothing is written without a click.
 */
struct hks_row {
    const char *keytype;            /* the store's id */
    char *type_display;
    int bits;
    char *sha256, *md5, *key;       /* presented */
    char *status;                   /* scanning / stored / new / MISMATCH / not offered / unreachable / ... */
    char *error;
    char *stored_sha256;            /* what the store holds, for the MISMATCH question; "" when nothing */
};

struct hks_data {
    char *host; int port;
    dlgcontrol *listbox, *detail, *banner, *accept, *decline, *closebtn;
    struct hks_row *rows; int nrows;
    struct kitty_hkv_run *run;
    LONG *shown_state;
    UINT_PTR timer;
    dlgparam *dlg;
    bool changed;                   /* the store was written or a key deleted */
    Conf *conf;                     /* the session, for Pin (manual host keys) */
    int pinned;                     /* fingerprints added to the session by Pin */
};
static struct hks_data *hks_active;     /* the one box at a time */
static dlgcontrol *kitty_manual_hk_list;/* the panel's manual-host-key list, refreshed after a Pin */

static void hks_row_set_status(struct hks_row *r, const char *status)
{
    sfree(r->status);
    r->status = dupstr(status);
}

static char *hks_row_text(const struct hks_row *r)
{
    const char *sha = r->sha256 && r->sha256[0] ? r->sha256 : "-";
    char bits[16];
    if (!strncmp(sha, "SHA256:", 7)) sha += 7;
    if (r->bits) sprintf(bits, "%d", r->bits); else strcpy(bits, "-");
    return dupprintf("%s\t%s\t%s\t%s", r->type_display, bits, sha, r->status);
}

static void hks_fill(struct hks_data *d, dlgparam *dlg)
{
    int i;
    dlg_update_start(d->listbox, dlg);
    dlg_listbox_clear(d->listbox, dlg);
    dlg_listbox_addwithid(d->listbox, dlg, KT_HKS_COL_HEAD, -1);
    for (i = 0; i < d->nrows; i++) {
        char *row = hks_row_text(&d->rows[i]);
        dlg_listbox_addwithid(d->listbox, dlg, row, i);
        sfree(row);
    }
    dlg_update_done(d->listbox, dlg);
}

static void hks_update_row(struct hks_data *d, int idx)
{
    HWND h = kitty_dlg_ctrl_hwnd(d->dlg, d->listbox);
    int n, r;
    if (!h || idx < 0 || idx >= d->nrows) return;
    n = (int)SendMessage(h, LB_GETCOUNT, 0, 0);
    for (r = 1; r < n; r++) {
        if ((int)SendMessage(h, LB_GETITEMDATA, r, 0) == idx) {
            char *row = hks_row_text(&d->rows[idx]);
            bool sel = SendMessage(h, LB_GETSEL, r, 0) > 0;
            SendMessage(h, WM_SETREDRAW, FALSE, 0);
            SendMessage(h, LB_DELETESTRING, r, 0);
            SendMessage(h, LB_INSERTSTRING, r, (LPARAM)row);
            SendMessage(h, LB_SETITEMDATA, r, idx);
            if (sel) SendMessage(h, LB_SETSEL, TRUE, r);
            SendMessage(h, WM_SETREDRAW, TRUE, 0);
            InvalidateRect(h, NULL, TRUE);
            sfree(row);
            break;
        }
    }
}

static int hks_selected(struct hks_data *d, dlgparam *dlg)
{
    int i;
    for (i = 1; i <= d->nrows; i++)
        if (dlg_listbox_issel(d->listbox, dlg, i)) {
            int id = dlg_listbox_getid(d->listbox, dlg, i);
            if (id >= 0 && id < d->nrows) return id;
        }
    return -1;
}

/* All selected rows, in list order; how many. */
static int hks_selected_all(struct hks_data *d, dlgparam *dlg, int *idx, int max)
{
    int i, n = 0;
    for (i = 1; i <= d->nrows && n < max; i++)
        if (dlg_listbox_issel(d->listbox, dlg, i)) {
            int id = dlg_listbox_getid(d->listbox, dlg, i);
            if (id >= 0 && id < d->nrows) idx[n++] = id;
        }
    return n;
}

static void hks_show_detail(struct hks_data *d, dlgparam *dlg, int idx)
{
    strbuf *sb = strbuf_new();
    if (idx < 0)
        put_dataz(sb, KT_HKS_DETAIL_NONE);
    else {
        const struct hks_row *r = &d->rows[idx];
        put_fmt(sb, "%s  %s", r->type_display, r->status);
        if (r->sha256 && r->sha256[0])
            put_fmt(sb, "\r\n" KT_HKS_DETAIL_PRESENTED, r->sha256, r->md5);
        if (r->stored_sha256 && r->stored_sha256[0] && strcmp(r->status, "stored"))
            put_fmt(sb, "\r\n" KT_HKS_DETAIL_STORED, r->stored_sha256);
        if (r->error && r->error[0])
            put_fmt(sb, "\r\n%s", r->error);
    }
    dlg_editbox_set(d->detail, dlg, sb->s);
    strbuf_free(sb);
}

/* The stored key's SHA256 for this host, port and type, or "". */
static char *hks_stored_sha256(const struct hks_data *d, const char *keytype)
{
    struct kitty_hostkey_list *l = kitty_hostkeys_enumerate();
    char *out = dupstr("");
    for (int i = 0; i < l->n; i++) {
        const struct kitty_hostkey_entry *e = &l->items[i];
        if (e->port == d->port && !strcmp(e->keytype, keytype) && !strcmp(e->host, d->host)) {
            sfree(out); out = dupstr(e->sha256); break;
        }
    }
    kitty_hostkeys_free(l);
    return out;
}

static bool hks_row_ink(dlgcontrol *ctrl, int id, bool dark, COLORREF *ink)
{
    struct hks_data *d = (struct hks_data *)ctrl->context.p;
    if (!d || id < 0 || id >= d->nrows || !d->rows[id].status) return false;
    if (strcmp(d->rows[id].status, "MISMATCH")) return false;
    *ink = dark ? RGB(255, 110, 110) : RGB(192, 0, 0);
    return true;
}

static void hks_run_stop(struct hks_data *d)
{
    if (d->timer) { KillTimer(NULL, d->timer); d->timer = 0; }
    if (d->run) { kitty_hkv_release(d->run); d->run = NULL; }
}

/* A finished job -> its row, judged against THIS store. */
static void hks_judge(struct hks_data *d, int idx, const struct kitty_hkv_job *j)
{
    struct hks_row *r = &d->rows[idx];
    sfree(r->sha256); sfree(r->md5); sfree(r->key); sfree(r->error);
    r->sha256 = dupstr(j->sha256); r->md5 = dupstr(j->md5);
    r->key = dupstr(j->key); r->error = dupstr(j->error);
    if (j->key[0]) {
        struct kitty_hostkey_entry e;
        int cmp = check_stored_host_key(d->host, d->port, r->keytype, j->key);
        memset(&e, 0, sizeof(e));
        kitty_hostkey_describe_text(r->keytype, j->key, &e);
        r->bits = e.bits;
        sfree(e.keytype); sfree(e.type_display); sfree(e.sha256); sfree(e.md5);
        hks_row_set_status(r, cmp == 0 ? "stored" : cmp == 1 ? "new" : "MISMATCH");
    } else
        hks_row_set_status(r, j->status);
}

static void CALLBACK hks_timer_proc(HWND hwnd, UINT msg, UINT_PTR id, DWORD now)
{
    struct hks_data *d = hks_active;
    struct kitty_hkv_job *jobs;
    int n, i, done = 0;
    if (!d || !d->run || !d->dlg) return;
    jobs = kitty_hkv_jobs(d->run, &n);
    for (i = 0; i < n; i++) {
        LONG st = InterlockedCompareExchange(&jobs[i].state, 0, 0);
        if (st == KHKV_DONE && d->shown_state[i] != KHKV_DONE) {
            hks_judge(d, i, &jobs[i]);
            hks_update_row(d, i);
            d->shown_state[i] = KHKV_DONE;
            if (hks_selected(d, d->dlg) == i) hks_show_detail(d, d->dlg, i);
        }
        if (st == KHKV_DONE) done++;
    }
    if (done == n) {
        int nnew = 0, nbad = 0, nstored = 0;
        for (i = 0; i < d->nrows; i++) {
            if (!strcmp(d->rows[i].status, "new")) nnew++;
            else if (!strcmp(d->rows[i].status, "MISMATCH")) nbad++;
            else if (!strcmp(d->rows[i].status, "stored")) nstored++;
        }
        {
            char *line = dupprintf(KT_HKS_DONE, nstored, nnew, nbad);
            dlg_label_change(d->banner, d->dlg, line);
            sfree(line);
        }
        hks_run_stop(d);
    } else {
        char *line = dupprintf(KT_HKS_SCANNING, done, n);
        dlg_label_change(d->banner, d->dlg, line);
        sfree(line);
    }
}

static void hks_box_closing(void)
{
    struct hks_data *d = hks_active;
    if (d) {
        hks_run_stop(d);
        for (int i = 0; i < d->nrows; i++) {
            struct hks_row *r = &d->rows[i];
            sfree(r->type_display); sfree(r->sha256); sfree(r->md5); sfree(r->key);
            sfree(r->status); sfree(r->error); sfree(r->stored_sha256);
        }
        sfree(d->rows); d->rows = NULL; d->nrows = 0;
        sfree(d->shown_state); d->shown_state = NULL;
    }
    hks_active = NULL;
}

static void kitty_hks_handler(dlgcontrol *ctrl, dlgparam *dlg, void *data, int event)
{
    struct hks_data *d = (struct hks_data *)ctrl->context.p;
    int which = ctrl->context2.i;      /* 0 list, 1 detail, 2 accept, 3 decline, 4 close, 5 pin */
    int idx, sel[16], nsel, k;

    if (!d) return;
    d->dlg = dlg;
    switch (which) {
      case 0:
        if (event == EVENT_REFRESH) {
            hks_fill(d, dlg);
            hks_show_detail(d, dlg, -1);
            if (!d->run && !d->timer) {
                /* the first refresh starts the scan */
                struct kitty_hkv_job *jobs = snewn(d->nrows, struct kitty_hkv_job);
                int i;
                memset(jobs, 0, d->nrows * sizeof(*jobs));
                for (i = 0; i < d->nrows; i++) {
                    jobs[i].host = d->host; jobs[i].port = d->port;
                    jobs[i].keytype = (char *)d->rows[i].keytype;
                }
                d->run = kitty_hkv_start(jobs, d->nrows);
                sfree(jobs);
                if (d->run) {
                    d->shown_state = snewn(d->nrows, LONG);
                    for (i = 0; i < d->nrows; i++) d->shown_state[i] = KHKV_PENDING;
                    hks_active = d;
                    d->timer = SetTimer(NULL, 0, 250, hks_timer_proc);
                    {
                        char *line = dupprintf(KT_HKS_SCANNING, 0, d->nrows);
                        dlg_label_change(d->banner, dlg, line);
                        sfree(line);
                    }
                } else
                    dlg_label_change(d->banner, dlg, KT_HK_VERIFY_NOTHREAD);
            }
        } else if (event == EVENT_SELCHANGE) {
            HWND h = kitty_dlg_ctrl_hwnd(dlg, ctrl);
            if (h && SendMessage(h, LB_GETSEL, 0, 0) > 0)
                SendMessage(h, LB_SETSEL, FALSE, 0);   /* the header row */
            hks_show_detail(d, dlg, hks_selected(d, dlg));
        }
        break;
      case 1:
        break;
      case 2: {                        /* Accept: every selected row */
        int stored = 0, nokey = 0, left = 0;
        if (event != EVENT_ACTION) break;
        nsel = hks_selected_all(d, dlg, sel, lenof(sel));
        if (!nsel) { dlg_label_change(d->banner, dlg, KT_HKS_SELECT_ROW); break; }
        for (k = 0; k < nsel; k++) {
            struct hks_row *r = &d->rows[sel[k]];
            if (!r->key || !r->key[0]) { nokey++; continue; }
            if (!strcmp(r->status, "stored")) continue;
            if (!strcmp(r->status, "MISMATCH")) {
                /* the man-in-the-middle question: never one click, once per key */
                char *was = hks_stored_sha256(d, r->keytype);
                char *q = dupprintf(KT_HKS_CONFIRM_REPLACE, d->host, d->port, r->type_display,
                                    was[0] ? was : "-", r->sha256);
                int answer = MessageBoxA(kitty_cfg_modal_owner(), q, KT_CAP_KITTY,
                                         MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
                sfree(q); sfree(was);
                if (answer != IDYES) { left++; continue; }
            }
            store_host_key(NULL, d->host, d->port, r->keytype, r->key);
            d->changed = true;
            hks_row_set_status(r, "stored");
            sfree(r->stored_sha256); r->stored_sha256 = dupstr(r->sha256);
            hks_update_row(d, sel[k]);
            stored++;
        }
        if (nsel == 1) hks_show_detail(d, dlg, sel[0]);
        if (stored) {
            char *line = left ? dupprintf(KT_HKS_STORED_LEFT, stored, stored == 1 ? "" : "s", left)
                              : dupprintf(KT_HKS_STORED_N, stored, stored == 1 ? "" : "s");
            dlg_label_change(d->banner, dlg, line);
            sfree(line);
        } else if (nokey == nsel)
            dlg_label_change(d->banner, dlg, KT_HKS_NOTHING_TO_ACCEPT);
        else if (left)
            dlg_label_change(d->banner, dlg, KT_HKS_LEFT);
        else
            dlg_label_change(d->banner, dlg, KT_HKS_ALREADY_STORED);
        break;
      }
      case 3: {                        /* Delete stored: the selected rows' stored keys */
        int nstored = 0, deleted = 0;
        strbuf *types;
        if (event != EVENT_ACTION) break;
        nsel = hks_selected_all(d, dlg, sel, lenof(sel));
        if (!nsel) { dlg_label_change(d->banner, dlg, KT_HKS_SELECT_ROW); break; }
        types = strbuf_new();
        for (k = 0; k < nsel; k++) {
            struct hks_row *r = &d->rows[sel[k]];
            if (!r->stored_sha256 || !r->stored_sha256[0]) continue;
            if (nstored++) put_dataz(types, ", ");
            put_dataz(types, r->type_display);
        }
        if (!nstored) {
            dlg_label_change(d->banner, dlg, KT_HKS_NONE_STORED);
            strbuf_free(types);
            break;
        }
        {
            /* one question for the lot, naming the types */
            char *q = dupprintf(KT_HKS_CONFIRM_DELETE_N, nstored == 1 ? "" : "s", types->s,
                                d->host, d->port);
            int answer = MessageBoxA(kitty_cfg_modal_owner(), q, KT_CAP_KITTY,
                                     MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
            sfree(q);
            if (answer == IDYES) {
                for (k = 0; k < nsel; k++) {
                    struct hks_row *r = &d->rows[sel[k]];
                    if (!r->stored_sha256 || !r->stored_sha256[0]) continue;
                    if (kitty_hostkey_delete(d->host, d->port, r->keytype)) deleted++;
                    d->changed = true;
                    sfree(r->stored_sha256); r->stored_sha256 = dupstr("");
                    /* what the host presents is now simply unknown to the store */
                    if (r->key && r->key[0]) hks_row_set_status(r, "new");
                    hks_update_row(d, sel[k]);
                }
            }
        }
        strbuf_free(types);
        if (nsel == 1) hks_show_detail(d, dlg, sel[0]);
        if (deleted) {
            char *line = dupprintf(KT_HKS_DELETED_N, deleted, deleted == 1 ? "" : "s");
            dlg_label_change(d->banner, dlg, line);
            sfree(line);
        } else
            dlg_label_change(d->banner, dlg, KT_HKS_LEFT);
        break;
      }
      case 5: {                        /* Pin: the presented fingerprints into the session's manual keys */
        int pinned = 0, listed = 0, nokey = 0;
        if (event != EVENT_ACTION) break;
        nsel = hks_selected_all(d, dlg, sel, lenof(sel));
        if (!nsel) { dlg_label_change(d->banner, dlg, KT_HKS_SELECT_ROW); break; }
        for (k = 0; k < nsel; k++) {
            struct hks_row *r = &d->rows[sel[k]];
            char *fp;
            if (!r->sha256 || !r->sha256[0] || !d->conf) { nokey++; continue; }
            fp = dupstr(r->sha256);
            if (!validate_manual_hostkey(fp)) { sfree(fp); nokey++; continue; }
            if (conf_get_str_str_opt(d->conf, CONF_ssh_manual_hostkeys, fp))
                listed++;
            else {
                conf_set_str_str(d->conf, CONF_ssh_manual_hostkeys, fp, "");
                pinned++;
            }
            sfree(fp);
        }
        d->pinned += pinned;
        if (pinned || listed) {
            char *line = dupprintf(KT_HKS_PINNED, pinned, pinned == 1 ? "" : "s", listed);
            dlg_label_change(d->banner, dlg, line);
            sfree(line);
        } else
            dlg_label_change(d->banner, dlg, KT_HKS_PIN_NONE);
        break;
      }
      case 4:                          /* Close */
        if (event == EVENT_ACTION)
            dlg_end(dlg, d->changed ? 1 : 0);
        break;
    }
}

/* The box's controls (kitty_cfg_show_aux_box builds path "Main"). */
static void hks_setup(struct controlbox *b, void *ctx)
{
    struct hks_data *d = (struct hks_data *)ctx;
    struct controlset *s;
    dlgcontrol *c;
    char *line;

    s = ctrl_getset(b, "Main", "keys", NULL);
    line = dupprintf(KT_HKS_INTRO, d->host, d->port);
    ctrl_text(s, line, HELPCTX(kitty_host_keys));
    sfree(line);
    d->listbox = ctrl_listbox(s, NULL, NO_SHORTCUT, HELPCTX(kitty_host_keys),
                              kitty_hks_handler, P(d));
    d->listbox->context2 = I(0);
    d->listbox->listbox.height = d->nrows + 1;
    d->listbox->listbox.multisel = 2;    /* extended: the arrow keys select; issel needs a multi list */
    d->listbox->listbox.headerrow = true;
    d->listbox->listbox.rowink = hks_row_ink;
    d->listbox->listbox.ncols = 4;
    d->listbox->listbox.percentages = snewn(4, int);
    d->listbox->listbox.percentages[0] = 30;   /* Type */
    d->listbox->listbox.percentages[1] = 10;   /* Bits */
    d->listbox->listbox.percentages[2] = 40;   /* SHA256 */
    d->listbox->listbox.percentages[3] = 20;   /* Status */
    d->detail = ctrl_editbox_multiline(s, NULL, NO_SHORTCUT, 4, true,
                                       HELPCTX(kitty_host_keys),
                                       kitty_hks_handler, P(d), P(NULL));
    d->detail->context2 = I(1);
    d->banner = ctrl_text(s, " ", HELPCTX(kitty_host_keys));
    ctrl_columns(s, 4, 25, 25, 25, 25);
    c = ctrl_pushbutton(s, KT_HKS_ACCEPT, NO_SHORTCUT, HELPCTX(kitty_host_keys),
                        kitty_hks_handler, P(d));
    c->context2 = I(2); c->column = 0; d->accept = c;
    c = ctrl_pushbutton(s, KT_HKS_DECLINE, NO_SHORTCUT, HELPCTX(kitty_host_keys),
                        kitty_hks_handler, P(d));
    c->context2 = I(3); c->column = 1; d->decline = c;
    c = ctrl_pushbutton(s, KT_HKS_PIN, NO_SHORTCUT, HELPCTX(kitty_host_keys),
                        kitty_hks_handler, P(d));
    c->context2 = I(5); c->column = 2;
    c = ctrl_pushbutton(s, KT_HKS_CLOSE, NO_SHORTCUT, HELPCTX(kitty_host_keys),
                        kitty_hks_handler, P(d));
    c->context2 = I(4); c->column = 3; c->button.iscancel = true; d->closebtn = c;
    ctrl_columns(s, 1, 100);
}

/* Open the box for host:port. Returns true when the store was changed;
 * *pinned counts the fingerprints Pin added to the session. */
static bool kitty_hostkey_scan_box(const char *host, int port, Conf *conf, int *pinned)
{
    extern void kitty_cfg_show_aux_box(void (*setup)(struct controlbox *, void *),
                                       void *ctx, const char *caption, HWND owner,
                                       void (*closing)(void));   /* windows/dialog.c */
    struct hks_data d;
    const char *const *types;
    int ntypes, i;
    char *caption;
    bool changed;

    memset(&d, 0, sizeof(d));
    d.host = dupstr(host); d.port = port; d.conf = conf;
    types = kitty_hostkey_scan_types(&ntypes);
    d.rows = snewn(ntypes, struct hks_row);
    memset(d.rows, 0, ntypes * sizeof(struct hks_row));
    d.nrows = ntypes;
    for (i = 0; i < ntypes; i++) {
        struct kitty_hostkey_entry e;
        memset(&e, 0, sizeof(e));
        kitty_hostkey_describe_text(types[i], "", &e);
        d.rows[i].keytype = types[i];
        d.rows[i].type_display = e.type_display; e.type_display = NULL;
        sfree(e.keytype); sfree(e.sha256); sfree(e.md5);
        d.rows[i].status = dupstr(KT_HKS_STATUS_SCANNING);
        d.rows[i].sha256 = dupstr(""); d.rows[i].md5 = dupstr("");
        d.rows[i].key = dupstr(""); d.rows[i].error = dupstr("");
        d.rows[i].stored_sha256 = hks_stored_sha256(&d, types[i]);
    }
    caption = dupprintf(KT_HKS_CAPTION, host, port);
    hks_active = &d;
    kitty_cfg_show_aux_box(hks_setup, &d, caption, kitty_cfg_modal_owner(), hks_box_closing);
    sfree(caption);
    changed = d.changed;
    if (pinned) *pinned = d.pinned;
    sfree(d.host);
    return changed;
}

/* ---- the panel side: the line and the button ---------------------------- */

static dlgcontrol *hkscan_line;         /* "Stored keys for host:port: n" */
static dlgcontrol *hkscan_types;        /* the types on the next line(s), or blank */

static void hkscan_refresh_line(dlgparam *dlg, Conf *conf)
{
    const char *host = conf_get_str(conf, CONF_host);
    int port = conf_get_int(conf, CONF_port);
    char *line, *typeline;
    if (!hkscan_line) return;
    if (!host || !*host) {
        line = dupstr(KT_HKSCAN_NO_HOST);
        typeline = dupstr(" ");
    } else {
        struct kitty_hostkey_list *l = kitty_hostkeys_enumerate();
        strbuf *types = strbuf_new();
        int n = 0;
        for (int i = 0; i < l->n; i++) {
            const struct kitty_hostkey_entry *e = &l->items[i];
            if (e->port == port && !strcmp(e->host, host)) {
                if (n++) put_dataz(types, ", ");
                put_dataz(types, e->type_display);
            }
        }
        line = n ? dupprintf(KT_HKSCAN_STORED, host, port, n)
                 : dupprintf(KT_HKSCAN_NONE, host, port);
        typeline = dupstr(n ? types->s : " ");
        strbuf_free(types);
        kitty_hostkeys_free(l);
    }
    dlg_label_change(hkscan_line, dlg, line);
    if (hkscan_types) dlg_label_change(hkscan_types, dlg, typeline);
    sfree(line); sfree(typeline);
}

static void kitty_hkscan_handler(dlgcontrol *ctrl, dlgparam *dlg, void *data, int event)
{
    Conf *conf = (Conf *)data;
    if (event == EVENT_REFRESH) {
        hkscan_refresh_line(dlg, conf);
    } else if (event == EVENT_ACTION) {
        const char *host = conf_get_str(conf, CONF_host);
        if (!host || !*host) {
            dlg_label_change(hkscan_line, dlg, KT_HKSCAN_NO_HOST);
            return;
        }
        int pinned = 0;
        kitty_hostkey_scan_box(host, conf_get_int(conf, CONF_port), conf, &pinned);
        hkscan_refresh_line(dlg, conf);
        if (pinned && kitty_manual_hk_list)
            dlg_refresh(kitty_manual_hk_list, dlg);   /* the list above shows the new pins */
    }
}
#define SCB_TITLE_APPNAME (scb_title_appname())

/* The bottom button bar (Open/Start/Updates/Cancel) and the Session panel. */
void scb_panel_session(struct controlbox *b, bool midsession)
{
    struct sessionsaver_data *ssd;
    struct controlset *s;
    dlgcontrol *c;
    char *str;

    ssd = (struct sessionsaver_data *)
        ctrl_alloc_with_free(b, sizeof(struct sessionsaver_data),
                             sessionsaver_data_free);
    memset(ssd, 0, sizeof(*ssd));
    ssd->savedsession = dupstr("");
    ssd->newfolder = dupstr("");
    if (!GetPuttyFlag()) {
        char lastfolder[1024];
        if (kitty_get_last_folder(lastfolder, sizeof(lastfolder))) {
            strncpy(CurrentFolder, lastfolder, 1023);
            CurrentFolder[1023] = '\0';
        }
    }
    ssd->midsession = midsession;

    /*
     * The standard panel that appears at the bottom of all panels:
     * Open, Cancel, Apply etc.
     */
    s = ctrl_getset(b, "", "", "");
    ctrl_columns(s, 5, 20, 20, 20, 20, 20);
    ssd->okbutton = ctrl_pushbutton(s,
                                    (midsession ? KT_CFG_BTN_APPLY : KT_CFG_BTN_OPEN),
                                    (char)(midsession ? 'a' : 'o'),
                                    HELPCTX(no_help),
                                    sessionsaver_handler, P(ssd));
    ssd->okbutton->button.isdefault = true;
    ssd->okbutton->column = 3;
    /* KiTTY "Start": launch the session in a new window without closing the
     * config box (only when launchable). col 2 is free in this 5-col row. */
    if (!midsession && !GetPuttyFlag()) {
        ssd->startbutton = ctrl_pushbutton(s, KT_KITTY_START, NO_SHORTCUT,
                                           HELPCTX(no_help),
                                           sessionsaver_handler, P(ssd));
        ssd->startbutton->column = 2;
    } else {
        ssd->startbutton = NULL;
    }
    /* The "Updates" button that used to sit in this row is now "Check for
     * updates now" on the Application/Updates panel, where the update setting
     * is. It was abbreviated to one word only because a button in a five-column
     * row has no space for a sentence; on a panel it can say what it does. */
    ssd->cancelbutton = ctrl_pushbutton(s, KT_KITTY_CANCEL, 'c', HELPCTX(no_help),
                                        sessionsaver_handler, P(ssd));
    ssd->cancelbutton->button.iscancel = true;
    ssd->cancelbutton->column = 4;
    /* We carefully don't close the 5-column part, so that platform-
     * specific add-ons can put extra buttons alongside Open and Cancel. */

    /*
     * The Session panel. Titled like every other panel - it was the one
     * panel with no title element, which read as an omission next to the
     * rest.
     */
    str = dupprintf(KT_CFG_SESSION_TITLE_FMT, SCB_TITLE_APPNAME);
    ctrl_settitle(b, "Session", str);
    sfree(str);

    if (!midsession) {
        struct hostport *hp = (struct hostport *)
            ctrl_alloc(b, sizeof(struct hostport));
        memset(hp, 0, sizeof(*hp));

        s = ctrl_getset(b, "Session", "hostport",
                        KT_SESSION_SPECIFY_THE_DESTINATION);
        ctrl_columns(s, 2, 75, 25);
        c = ctrl_editbox(s, HOST_BOX_TITLE, 'n', 100,
                         HELPCTX(session_hostname),
                         config_host_handler, I(0), I(0));
        c->column = 0;
        hp->host = c;
        quickconnect_host_ctrl = c;   /* [ConfigBox] loadlastsession=no target */
        c = ctrl_editbox(s, PORT_BOX_TITLE, 'p', 100,
                         HELPCTX(session_hostname),
                         config_port_handler, I(0), I(0));
        c->column = 1;
        hp->port = c;

        ctrl_columns(s, 1, 100);
        ctrl_columns(s, 2, 62, 38);
        c = ctrl_radiobuttons(s, NULL, NO_SHORTCUT, 3,
                              HELPCTX(session_hostname),
                              config_protocols_handler, P(hp));
        c->column = 0;
        hp->protradio = c;
        c->radio.buttons = sresize(c->radio.buttons, PROTOCOL_LIMIT, char *);
        c->radio.shortcuts = sresize(c->radio.shortcuts, PROTOCOL_LIMIT, char);
        c->radio.buttondata = sresize(c->radio.buttondata, PROTOCOL_LIMIT,
                                      intorptr);
        assert(c->radio.nbuttons == 0);
        /* UI design assumes there exists at least one 'real' radio button */
        assert(n_ui_backends > 0 && n_ui_backends < PROTOCOL_LIMIT);
        for (size_t i = 0; i < n_ui_backends; i++) {
            assert(backends[i]);
            c->radio.buttons[c->radio.nbuttons] =
                dupstr(backends[i]->displayname_tc);
            c->radio.shortcuts[c->radio.nbuttons] =
                (backends[i]->protocol == PROT_SSH ? 's' :
                 backends[i]->protocol == PROT_SERIAL ? 'r' :
                 backends[i]->protocol == PROT_RAW ? 'w' :  /* FIXME unused */
                 NO_SHORTCUT);
            c->radio.buttondata[c->radio.nbuttons] =
                I(backends[i]->protocol);
            c->radio.nbuttons++;
        }
        /* UI design assumes there exists at least one droplist entry */
        assert(backends[c->radio.nbuttons]);

        c->radio.buttons[c->radio.nbuttons] = dupstr(KT_CFG_PROTO_OTHER);
        c->radio.shortcuts[c->radio.nbuttons] = 't';
        c->radio.buttondata[c->radio.nbuttons] = I(-1);
        c->radio.nbuttons++;

        c = ctrl_droplist(s, NULL, NO_SHORTCUT, 100,
                          HELPCTX(session_hostname),
                          config_protocols_handler, P(hp));
        hp->protlist = c;
        /* droplist is populated in config_protocols_handler */
        c->column = 1;

        /* Vertically centre the two protocol controls w.r.t. each other */
        hp->protlist->align_next_to = hp->protradio;

        ctrl_columns(s, 1, 100);
    }

    /*
     * The Load/Save panel is available even in mid-session.
     */
    s = ctrl_getset(b, "Session", "savedsessions",
                    midsession ? KT_CFG_SESSION_SAVE_CURRENT :
                    KT_CFG_SESSION_LOAD_SAVE_DELETE);
    /* KiTTY folder navigation: the name box shares its row with Save AND with
     * New folder, because the combo that used to carry folder creation is gone.
     * Three columns only in that mode, so the classic layout is untouched.
     *
     * The widths are not even thirds, and not proportional to the captions
     * either: the middle button carries "Save" OR "Rename", so it is sized for
     * the longer of the two, while "New folder" had spare room to give up. Both
     * are measured rather than judged by eye - the QA harness fails the build if
     * either caption comes within ten pixels of its border.
     * Icon buttons would have made them narrow and equal; they were dropped
     * rather than deferred, because a labelled button names itself to a screen
     * reader and an icon does not. */
    if (kitty_folder_rows_active(ssd))
        ctrl_columns(s, 3, 50, 22, 28);
    else
        ctrl_columns(s, 2, 75, 25);
    get_sesslist(&ssd->sesslist, true);
    kitty_session_folder_cache_clear();
    ssd->editbox = ctrl_editbox(s, NULL, 'e', 100,
                                HELPCTX(session_saved),
                                sessionsaver_handler, P(ssd), P(NULL));
    ssd->editbox->column = 0;
    session_filter_ctrl = ssd->editbox;   /* Ctrl+F jump target, see above */
    session_filter_ssd = ssd;             /* Ctrl+G root-folder jump */
    kitty_ctrl_focus_hook = kitty_config_ctrl_focus_gained; /* and its focus tracking */
    ssd->savebutton = ctrl_pushbutton(s, KT_SESSION_SAVE, 'v',
                                      HELPCTX(session_saved),
                                      sessionsaver_handler, P(ssd));
    ssd->savebutton->column = 1;
    ssd->savebutton->align_next_to = ssd->editbox;   /* centre on the name field */
    /* KiTTY folder navigation: creation lives here now, as a TEXT button and
     * permanently so - an icon button would carry no name for a screen reader
     * to announce, which a caption gives for nothing. */
    if (kitty_folder_rows_active(ssd)) {
        ssd->createbutton = ctrl_pushbutton(s, KT_SESSION_NEW_FOLDER, NO_SHORTCUT,
                                            HELPCTX(session_saved),
                                            sessionsaver_handler, P(ssd));
        ssd->createbutton->column = 2;
        ssd->createbutton->align_next_to = ssd->editbox;
    }
    ctrl_columns(s, 1, 100);
    ctrl_columns(s, 2, 75, 25);
    /* Folder selector + create button share their own synchronized row. */
    /* KiTTY: editable folder selector. Selecting a folder filters the list.
     * Creating one is an explicit choice - pick the synthetic "<new folder...>"
     * row, type the name, press New folder. With the root list selected instead,
     * a typed name renames its LABEL (display only) and the button says so. */
    /* KiTTY folder navigation: the LIST is the navigation in that mode, so the
     * combo goes - two navigation models in one window is the thing to avoid.
     * "New folder" goes with it rather than on its own account: the folder name
     * it acts on comes from the combo's edit half, so without the combo it has
     * no input. It returns beside the name box in a later step; until then this
     * mode creates no folders, which is why the setting is opt-in and off by
     * default. "Del folder" is a separate button beside the session list and is
     * NOT affected - it works on the folder you are currently inside. */
    if (!GetPuttyFlag() && !kitty_folder_rows_on()) {
        ssd->folderlist = ctrl_combobox(s, NULL, NO_SHORTCUT, 100,
                                        HELPCTX(session_saved),
                                        sessionsaver_handler, P(ssd), P(NULL));
        ssd->folderlist->column = 0;
        if (!midsession) {
            ssd->createbutton = ctrl_pushbutton(s, KT_SESSION_NEW_FOLDER, NO_SHORTCUT,
                                                HELPCTX(session_saved),
                                                sessionsaver_handler, P(ssd));
            ssd->createbutton->column = 1;
            ssd->createbutton->align_next_to = ssd->folderlist; /* centre on combo */
        }
    } else {
        ssd->folderlist = NULL;
    }
    /* KiTTY proxy choice: pick a named proxy definition to overlay onto this
     * session. Shown per [ConfigBox] proxyselection (yes/no/auto): auto shows it
     * when the user has proxy definitions (hknet/KiTTY#11). The droplist shares a
     * 75/25 row with an Edit button (create/edit/delete definitions), like the
     * folder combo + New folder. */
    if (!GetPuttyFlag() && kitty_proxy_choice_shown()) {
        ctrl_columns(s, 1, 100);
        ctrl_columns(s, 2, 75, 25);
        /* Caption is set at runtime by the handler - it states whether an override
         * is ACTIVE, and is a function of (choice, session). "Proxy choice" was
         * the old caption and was wrong: it read as a setting rather than as
         * something that amends only the next connection. */
        struct pxchoice_data *pcd = (struct pxchoice_data *)
            ctrl_alloc(b, sizeof(struct pxchoice_data));
        memset(pcd, 0, sizeof(*pcd));
        pxchoice_state = pcd;          /* so a session load can reset it */
        dlgcontrol *pc = ctrl_droplist(s, KT_SESSION_PROXY_OVERRIDE_OPTIONS, NO_SHORTCUT, 100,
                                       HELPCTX(kitty_proxy_override),
                                       kitty_proxy_handler, P(pcd));
        pc->column = 0;
        if (!midsession) {
            dlgcontrol *pe = ctrl_pushbutton(s, KT_SESSION_EDIT, NO_SHORTCUT,
                                             HELPCTX(session_saved),
                                             kitty_proxyedit_handler, P(NULL));
            pe->column = 1;
            pe->align_next_to = pc;   /* centre the button on the droplist */
        }
        /* leave the row at 2 columns; the ctrl_columns(1) after #endif merges it,
         * matching the no-proxy path (a 1->1 transition would assert). */
    }
    ctrl_columns(s, 1, 100);
    ctrl_columns(s, 2, 75, 25);
    ssd->listbox = ctrl_listbox(s, NULL, NO_SHORTCUT,
                                HELPCTX(session_saved),
                                sessionsaver_handler, P(ssd));
    ssd->listbox->column = 0;
    /*
     * KiTTY: the saved-session list height is user-configurable via kitty.ini
     * [ConfigBox] height (GetConfigBoxHeight()). The buttons beside it are
     * spread down that height AFTER the layout has run, by
     * kitty_config_session_distribute() - Load at the top, Delete and Del
     * folder centred below it (Export/Import live on Application/Migration).
     *
     * They used to be spaced by blank ctrl_text rows whose COUNT was computed
     * from the height here. That worked, and it welded the row count into the
     * CONTROLBOX: the number of controls in this set depended on it, so the
     * setting could not be changed without building the whole box again -
     * which is why it only ever took effect in the next window. Measuring the
     * list and placing the buttons against it costs nothing, holds at any
     * height, font and DPI, and can be redone whenever the number changes.
     */
    kitty_session_ssd = ssd;
    ssd->listbox->listbox.height = kitty_config_session_rows();
    if (!midsession) {
        ssd->loadbutton = ctrl_pushbutton(s, KT_SESSION_LOAD, 'l',       /* top */
                                          HELPCTX(session_saved),
                                          sessionsaver_handler, P(ssd));
        ssd->loadbutton->column = 1;
        ssd->delbutton = ctrl_pushbutton(s, KT_SESSION_DELETE, 'd',      /* upper-centre */
                                         HELPCTX(session_saved),
                                         sessionsaver_handler, P(ssd));
        ssd->delbutton->column = 1;
    } else {
        /* We can't offer the Load button mid-session, as it would allow the
         * user to load and subsequently save settings they can't see. (And
         * also change otherwise immutable settings underfoot; that probably
         * shouldn't be a problem, but.) */
        ssd->loadbutton = NULL;
        /* Disable the Delete button mid-session too, for UI consistency. */
        ssd->delbutton = NULL;
    }
    /* KiTTY: destructive folder button lives with the other destructive action
     * beside the session list. */
    if (!midsession && !GetPuttyFlag()) {
        ssd->delfolderbutton = ctrl_pushbutton(s, KT_SESSION_DEL_FOLDER, NO_SHORTCUT,
                                               HELPCTX(session_saved),
                                               sessionsaver_handler, P(ssd));
        ssd->delfolderbutton->column = 1;      /* upper-centre, just under Delete */
        /* The whole-store Export all / Import all buttons used to sit at the
         * foot of this column; they are store management, not session
         * editing, and live on Application > Migration now. */
    } else {
        /* Defensive only: setup_config_box already memsets ssd to 0. */
        ssd->createbutton = NULL;
        ssd->delfolderbutton = NULL;
    }
    ctrl_columns(s, 1, 100);
    /*
     * KiTTY: why the list has sessions in it that were never created here.
     *
     * Shown ONCE - the bit is cleared as the control is built, so the next
     * configuration window comes up without the row and everything below it
     * moves back up. Registry modes only, and only while there is an old hive
     * to explain, which is the same gate the Migration panel is under.
     */
    if (!midsession && !GetPuttyFlag() &&
        kitty_foreign_notice_pending(KITTY_FOREIGN_NOTICE_LIST)) {
        if (GetIniFileFlag() == 0 /* SAVEMODE_REG */ &&
            kitty_has_foreign_sessions() &&
            kitty_get_show_foreign_sessions()) {
            dlgcontrol *nt, *nb;
            ctrl_columns(s, 2, 74, 26);
            /* PAST TENSE. The bit that puts this line here may have been
             * armed on an earlier run, so by the time anyone reads it they
             * may well have sessions of their own - present tense would then
             * be plainly untrue about the list they are looking at. */
            nt = ctrl_text(s, KT_SESSION_THIS_LIST_ALSO_HOLDS_SESSIONS, HELPCTX(kitty_old_sessions));
            nt->column = 0;
            nb = ctrl_pushbutton(s, KT_SESSION_OLD_SESSIONS, NO_SHORTCUT,
                                 HELPCTX(kitty_old_sessions),
                                 kitty_foreignnotice_handler, P(NULL));
            nb->column = 1;
            nb->align_next_to = nt;
            ctrl_columns(s, 1, 100);
            kitty_foreign_notice_clear(KITTY_FOREIGN_NOTICE_LIST);
        }
    }
    /* KiTTY: read-only display of the selected session's comment, below the list.
     * Empty comments show a clear placeholder directly inside the field. */
    if (!GetPuttyFlag()) {
        ssd->commentbox = ctrl_editbox_multiline(
            s, NULL, NO_SHORTCUT, 2, true,
            HELPCTX(session_saved), sessionsaver_handler, P(ssd), P(NULL));
    } else {
        ssd->commentbox = NULL;
    }

    /* "Close window on exit" and "Save settings on exit" are on Window >
     * Behaviour ("Closing the window" / "Remembering"): both decide what the
     * WINDOW does when its session ends, and the closing hook in window.c
     * treats "Save settings on exit" and "Remember window position" as a
     * pair. "Hide this session from the launcher" is on Session/Startup. */

    /* KiTTY: settings about the APPLICATION rather than this connection, in
     * their own box so they stop reading as session options. They ended up on
     * the Session panel because there is nowhere else for app-wide settings
     * yet; the KiTTY++ Settings tree is their home now, and both are genuinely
     * global: "Check for updates" is the [KiTTY] checkupdate key
     * (kitty_check_update_enabled), no longer CONF_check_update_startup. */
    /* The old-sessions switch used to sit here, in an "Application" box at
     * the foot of this panel, because there was nowhere else for it. It is
     * now Application > Migration - it is about the installation and its
     * history, not about this session. */
}

/* KiTTY: the pattern the "use a default" button fills in. ISO-style date so it
 * sorts, seconds because a session log without them is hard to correlate, and
 * a trailing space so the stamp does not run into the logged line. */
#define KITTY_LOGTIMESTAMP_DEFAULT "%Y-%m-%d %H:%M:%S "

/*
 * One button doing both halves of the same decision: with the field empty it
 * offers a working pattern, with the field set it clears it. The label always
 * says which, so the button is never a surprise.
 */
static void logtimestamp_button_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                        void *data, int event)
{
    Conf *conf = (Conf *)data;
    const char *cur = conf_get_str(conf, CONF_logtimestamp);
    bool empty = (!cur || !*cur);

    if (event == EVENT_REFRESH) {
        dlg_label_change(ctrl, dlg, empty ? KT_LOGGING_USE_A_DEFAULT_TIMESTAMP
                                          : KT_CFG_LOG_CLEAR_TIMESTAMP);
    } else if (event == EVENT_ACTION) {
        conf_set_str(conf, CONF_logtimestamp,
                     empty ? KITTY_LOGTIMESTAMP_DEFAULT : "");
        /* NULL: refresh every control. The edit box above has to show the new
         * value, and this button's own label has to flip with it. */
        dlg_refresh(NULL, dlg);
    }
}

/* The Session/Logging panel. */
void scb_panel_logging(struct controlbox *b, bool midsession, int protocol)
{
    struct controlset *s;
    dlgcontrol *c;                     /* the two-column rotation row */

    /*
     * The Session/Logging panel.
     */
    ctrl_settitle(b, "Session/Logging", KT_LOGGING_OPTIONS_CONTROLLING_SESSION_LOGGING);

    s = ctrl_getset(b, "Session/Logging", "main", NULL);
    /*
     * The logging buttons change depending on whether SSH packet
     * logging can sensibly be available.
     */
    {
        const char *sshlogname, *sshrawlogname;
        if ((midsession && protocol == PROT_SSH) ||
            (!midsession && backend_vt_from_proto(PROT_SSH))) {
            sshlogname = KT_CFG_LOG_SSH_PACKETS;
            sshrawlogname = KT_CFG_LOG_SSH_PACKETS_RAW;
        } else {
            sshlogname = NULL;         /* this will disable both buttons */
            sshrawlogname = NULL;      /* this will just placate optimisers */
        }
        ctrl_radiobuttons(s, KT_LOGGING_SESSION_LOGGING, NO_SHORTCUT, 2,
                          HELPCTX(logging_main),
                          loggingbuttons_handler,
                          I(CONF_logtype),
                          KT_LOGGING_NONE, 't', I(LGTYP_NONE),
                          KT_LOGGING_PRINTABLE_OUTPUT, 'p', I(LGTYP_ASCII),
                          KT_LOGGING_ALL_SESSION_OUTPUT, 'l', I(LGTYP_DEBUG),
                          sshlogname, 's', I(LGTYP_PACKETS),
                          sshrawlogname, 'r', I(LGTYP_SSHRAW));
    }
    ctrl_filesel(s, KT_LOGGING_LOG_FILE_NAME, 'f',
                 FILTER_ALL_FILES, true, KT_LOGGING_SELECT_SESSION_LOG_FILE_NAME,
                 HELPCTX(logging_filename),
                 conf_filesel_handler, I(CONF_logfilename));
    ctrl_text(s, KT_LOGGING_LOG_FILE_NAME_CAN_CONTAIN,
              HELPCTX(logging_filename));
    ctrl_radiobuttons(s, KT_LOGGING_WHAT_TO_DO, 'e', 1,
                      HELPCTX(logging_exists),
                      conf_radiobutton_handler, I(CONF_logxfovr),
                      KT_LOGGING_ALWAYS_OVERWRITE, I(LGXF_OVR),
                      KT_LOGGING_ALWAYS_APPEND_TO_THE_END, I(LGXF_APN),
                      KT_LOGGING_ASK_THE_USER_EVERY_TIME, I(LGXF_ASK));
    ctrl_checkbox(s, KT_LOGGING_FLUSH_LOG_FILE_FREQUENTLY, 'u',
                  HELPCTX(logging_flush),
                  conf_checkbox_handler, I(CONF_logflush));
    ctrl_checkbox(s, KT_LOGGING_INCLUDE_HEADER, 'i',
                  HELPCTX(logging_header),
                  conf_checkbox_handler, I(CONF_logheader));
    if (!GetPuttyFlag()) {
        /* Two columns so the unit sits AFTER the field, reading "every [ 0 ]
         * sec." The old one-line label "Log rotation delay (sec, 0=off)" was
         * clipped to "Log rotation delay (sec," - the edit box took half the
         * row and the rest of the label had nowhere to go. */
        /* "Automatic logrotation every [ 0 ] sec." on ONE baseline.
         *
         * All three parts are the same KIND of control on purpose. An editbox
         * with a label draws that label as a STATIC positioned differently
         * from the box's own text, so a trailing word ends up ~9px above the
         * words it belongs to - it reads as hanging in the air. An UNWRAPPED
         * ctrl_text is laid out as a borderless read-only edit box, and a
         * label-less ctrl_editbox is a bare edit box, so all three are
         * EDITHEIGHT boxes on the same row and their text lines up. */
        ctrl_columns(s, 3, 52, 24, 24);
        c = ctrl_text(s, KT_LOGGING_AUTOMATIC_LOGROTATION_EVERY, HELPCTX(kitty_log_rotation));
        c->column = 0;
        c->text.wrap = false;
        c = ctrl_editbox(s, NULL, NO_SHORTCUT, 100,
                         HELPCTX(kitty_logging_stamps),
                         conf_editbox_handler, I(CONF_logtimerotation), ED_INT);
        c->column = 1;
        c = ctrl_text(s, KT_LOGGING_SEC, HELPCTX(kitty_log_rotation));
        c->column = 2;
        c->text.wrap = false;
        ctrl_columns(s, 1, 100);
        ctrl_text(s, KT_LOGGING_0_OFF_THE_LOG_FILE,
                  HELPCTX(kitty_log_rotation));
        ctrl_editbox(s, KT_LOGGING_TIMESTAMP_STRFTIME_FORMAT, NO_SHORTCUT, 100,
                     HELPCTX(kitty_logging_stamps),
                     conf_editbox_handler, I(CONF_logtimestamp), ED_STR);
        /* One button, two jobs: fill in a working pattern when the field is
         * empty, clear it when it is not - so the feature can be tried, and
         * undone, without knowing strftime. Its label says which it will do. */
        ctrl_pushbutton(s, KT_LOGGING_USE_A_DEFAULT_TIMESTAMP, NO_SHORTCUT,
                        HELPCTX(kitty_logging_stamps), logtimestamp_button_handler, P(NULL));
        /* The file-name field above says what its &-codes mean; this one said
         * nothing at all, so the only way to learn the format was to guess. */
        ctrl_text(s, KT_LOGGING_WRITTEN_AT_THE_START,
                  HELPCTX(kitty_logging_stamps));
    }

    if ((midsession && protocol == PROT_SSH) ||
        (!midsession && backend_vt_from_proto(PROT_SSH))) {
        s = ctrl_getset(b, "Session/Logging", "ssh",
                        KT_LOGGING_OPTIONS_SPECIFIC_TO_SSH_PACKET);
        ctrl_checkbox(s, KT_LOGGING_OMIT_KNOWN_PASSWORD_FIELDS, 'k',
                      HELPCTX(logging_ssh_omit_password),
                      conf_checkbox_handler, I(CONF_logomitpass));
        ctrl_checkbox(s, KT_LOGGING_OMIT_SESSION_DATA, 'd',
                      HELPCTX(logging_ssh_omit_data),
                      conf_checkbox_handler, I(CONF_logomitdata));
    }
}

/* The Session/Scripting panel (KiTTY rutty scripting). */

/* kitty.c: this installation's broadcast key (generated, or sendcmdgroup). */
/* ...and whether that key came from kitty.ini rather than being derived: the
 * provenance line says which, so a missing declaration here would silently be
 * an implicit int() call. */

/* ---- Session > Broadcast: the broadcast key -------------------------------
 *
 * The field shows the key this session listens for: its own if set, otherwise
 * the install's generated one. It is LOCKED by default so it cannot be changed
 * by a stray keystroke - the key is the aiming mechanism, and a typo silently
 * stops every broadcast reaching this session.
 *
 * PuTTY's config framework has no generic "disable this control", so the lock is
 * enforced here: while locked, an edit is reverted on the spot. Pressing Edit
 * unlocks and relabels the button to Save; pressing Save writes the typed value
 * into the session and locks the field again. Unsaved edits live in the live
 * conf only, so they revert when the session is next loaded.
 */
static void kitty_bkey_box_handler(dlgcontrol *ctrl, dlgparam *dp,
                                   void *data, int event);

static void kitty_bkey_clear_handler(dlgcontrol *ctrl, dlgparam *dp,
                                     void *data, int event);

/* State for one config box. Allocated with ctrl_alloc so it lives and dies with
 * the controlbox: file-scope statics would be shared by two config boxes open at
 * once (two terminals in Change Settings), and the second one built would leave
 * the first pointing at controls that are not in its dialog. */
struct kitty_bkey_state {
    dlgcontrol *box;
    dlgcontrol *prov;    /* the "where this key came from" line */
    bool setting;        /* we are writing the box ourselves - see set_box */
};

/* Write the box WITHOUT it counting as a user edit.
 *
 * SetDlgItemText makes the edit control notify EN_CHANGE, which the config
 * framework turns into EVENT_VALCHANGE - indistinguishable from typing. This
 * field is the one place that DISPLAYS something other than what it stores (the
 * inherited key, when the session has none of its own), so that reflex wrote the
 * displayed key straight back as a custom key: Clear looked like it did nothing,
 * and any later visit to the page - returning to it, or loading a session -
 * turned an inheriting session into a custom one at the next save. The
 * notification is sent, not posted, so it arrives inside this call and a plain
 * flag is enough.
 */
static void kitty_bkey_set_box(struct kitty_bkey_state *st, dlgparam *dp,
                               const char *text)
{
    st->setting = true;
    dlg_editbox_set(st->box, dp, text);
    st->setting = false;
}

/* Update that line from what is STORED. Three states, and it moves: pressing
 * Clear must not leave "Custom key for this session" sitting under a field that
 * now shows the installation's key.
 *
 * WARNING: dlg_label_change on a CTRL_TEXT cannot change the control's HEIGHT - that
 * was fixed at layout time (windows/controls.c). All three wordings are
 * therefore kept to one line of similar length; a longer one would be cut off.
 */
static void kitty_bkey_update_prov(struct kitty_bkey_state *st,
                                  dlgparam *dp, Conf *conf)
{
    const char *own;
    if (!st || !st->prov || !conf) return;
    own = conf_get_str(conf, CONF_kitty_broadcast_key);
    if (own && *own)
        dlg_label_change(st->prov, dp,
                         KT_CFG_BROADCAST_KEY_CUSTOM);
    else if (kitty_broadcast_group_from_ini())
        dlg_label_change(st->prov, dp,
                         KT_CFG_BROADCAST_KEY_INI);
    else
        dlg_label_change(st->prov, dp,
                         KT_LOGGING_DEFAULT_KEY_GENERATED);
}

/* Put the key on the clipboard. The field is ordinary and selectable, but the
 * key is a long generated string and its whole purpose is to be carried to
 * another session or into kitty.ini - one click beats a careful drag. */
static void kitty_bkey_copy_handler(dlgcontrol *ctrl, dlgparam *dp,
                                    void *data, int event)
{
    Conf *conf = (Conf *)data;
    if (event != EVENT_ACTION) return;
    {
        const char *k = conf_get_str(conf, CONF_kitty_broadcast_key);
        if (!k || !*k) k = kitty_broadcast_group();     /* the install's key */
        SetTextToClipboard(k);
    }
}

static void kitty_broadcast_key_controls(struct controlbox *b, struct controlset *s)
{
    struct kitty_bkey_state *st = (struct kitty_bkey_state *)
        ctrl_alloc(b, sizeof(struct kitty_bkey_state));
    dlgcontrol *c;
    st->box = NULL; st->prov = NULL; st->setting = false;
    /* An ordinary edit box, like every other field on this page: selectable,
     * double-clickable, copyable - which is the whole point, since the key
     * exists to be carried elsewhere. An earlier version locked it behind an
     * Edit button, which made the value awkward to copy and gave one control the
     * full width of the page for no benefit.
     *
     * Field and button share a row (75/25) so neither sprawls. */
    /* The label goes on its OWN full-width line, and the row below holds a
     * label-less box and the button. With the label attached to the editbox the
     * two controls start at different heights - the box sits under its label
     * while the button starts at the top of the row - and the button appears to
     * float above the field. */
    ctrl_text(s, KT_LOGGING_BROADCAST_KEY, HELPCTX(kitty_broadcast_key));
    ctrl_columns(s, 3, 60, 20, 20);
    c = ctrl_editbox(s, NULL, NO_SHORTCUT, 100,
                     HELPCTX(no_help), kitty_bkey_box_handler,
                     P(st), ED_STR);
    c->column = 0;
    st->box = c;
    c = ctrl_pushbutton(s, KT_LOGGING_COPY, NO_SHORTCUT,
                        HELPCTX(kitty_broadcast_key), kitty_bkey_copy_handler, P(st));
    c->column = 1;
    c = ctrl_pushbutton(s, KT_LOGGING_CLEAR, NO_SHORTCUT,
                        HELPCTX(kitty_broadcast_key), kitty_bkey_clear_handler, P(st));
    c->column = 2;
    ctrl_columns(s, 1, 100);
    /* WHERE the value in front of the user came from. Three states, decided
     * when the panel is built - it does not follow typing, and does not need
     * to: what matters is that the field is never a mystery string on open.
     * Typing makes it custom (Clear puts it back), so the wording is chosen
     * from what is stored, not from what the box currently shows. */
    /* Built with a full-length placeholder because the panel builder has no
     * Conf to read: the first EVENT_REFRESH replaces it with the real state.
     * The placeholder sets the control's HEIGHT, which cannot change later, so
     * it must be as long as the longest wording. */
    st->prov = ctrl_text(s, KT_LOGGING_DEFAULT_KEY_GENERATED, HELPCTX(no_help));
}

static void kitty_bkey_box_handler(dlgcontrol *ctrl, dlgparam *dp,
                                   void *data, int event)
{
    struct kitty_bkey_state *st = (struct kitty_bkey_state *)ctrl->context.p;
    Conf *conf = (Conf *)data;
    if (event == EVENT_REFRESH) {
        const char *k = conf_get_str(conf, CONF_kitty_broadcast_key);
        /* Empty means "the install's key" - SHOW that, rather than an empty box
         * the user cannot copy anything out of. */
        kitty_bkey_set_box(st, dp, (k && *k) ? k : kitty_broadcast_group());
        kitty_bkey_update_prov(st, dp, conf);
    } else if (event == EVENT_VALCHANGE) {
        /* ANYTHING typed is a custom key, stored exactly as typed - even when it
         * equals the installation's own. Guessing intent from the text is the
         * dilemma this avoids: "follow the installation" is a separate ACTION
         * (the Clear button), not a string that happens to match. */
        char *typed;
        if (st->setting)
            return;              /* our own text, echoed back - not an edit */
        typed = dlg_editbox_get(ctrl, dp);
        conf_set_str(conf, CONF_kitty_broadcast_key, typed ? typed : "");
        sfree(typed);
        kitty_bkey_update_prov(st, dp, conf);
    }
}

/* Back to the installation's key: stores empty, which is what "no key of my
 * own" means everywhere else in the code. */
static void kitty_bkey_clear_handler(dlgcontrol *ctrl, dlgparam *dp,
                                     void *data, int event)
{
    struct kitty_bkey_state *st = (struct kitty_bkey_state *)ctrl->context.p;
    Conf *conf = (Conf *)data;
    if (event != EVENT_ACTION) return;
    conf_set_str(conf, CONF_kitty_broadcast_key, "");
    kitty_bkey_set_box(st, dp, kitty_broadcast_group());   /* inherited again */
    kitty_bkey_update_prov(st, dp, conf);   /* ...and the line stops saying custom */
}


void scb_panel_scripting(struct controlbox *b, bool midsession)
{
    struct controlset *s;
    dlgcontrol *c;

    /*
     * The Session/Scripting panel (KiTTY rutty scripting). Placed under Session
     * to match upstream KiTTY's layout, immediately after Session/Logging so the
     * config treeview builds in a valid order (no implicit parent path).
     */
    if (!GetPuttyFlag()) {
        ctrl_settitle(b, "Session/Scripting",
                      KT_SCRIPTING_OPTIONS_CONTROLLING_AUTOMATED_SCRIPTING);
        s = ctrl_getset(b, "Session/Scripting", "main",
                        KT_SCRIPTING_SEND_A_SCRIPT_FILE);
        ctrl_checkbox(s, KT_SCRIPTING_RUN_THE_SCRIPT_ON_CONNECT, NO_SHORTCUT,
                      HELPCTX(kitty_scriptfile), kitty_checkbox_int_handler,
                      I(CONF_script_mode));
        ctrl_filesel(s, KT_SCRIPTING_SCRIPT_FILE, NO_SHORTCUT,
                     FILTER_ALL_FILES, false, KT_SCRIPTING_SELECT_SCRIPT_FILE,
                     HELPCTX(kitty_scriptfile),
                     conf_filesel_handler, I(CONF_scriptfile));
        ctrl_checkbox(s, KT_SCRIPTING_WAIT_FOR_A_PROMPT_BEFORE, NO_SHORTCUT,
                      HELPCTX(kitty_scriptfile), kitty_checkbox_int_handler,
                      I(CONF_script_enable));
        /* Belongs to the WAIT feature above, not to CR/LF where it used to
         * sit: it skips the wait for the FIRST line only. */
        ctrl_checkbox(s, KT_SCRIPTING_EXCEPT_FOR_FIRST_COMMAND, NO_SHORTCUT,
                      HELPCTX(kitty_scriptfile), kitty_checkbox_int_handler,
                      I(CONF_script_except));
        ctrl_editbox(s, KT_SCRIPTING_WAIT_FOR_TEXT, NO_SHORTCUT, 60,
                     HELPCTX(kitty_scriptfile), conf_editbox_handler,
                     I(CONF_script_waitfor), ED_STR);
        ctrl_editbox(s, KT_SCRIPTING_HALT_ON_TEXT, NO_SHORTCUT, 60,
                     HELPCTX(kitty_scriptfile), conf_editbox_handler,
                     I(CONF_script_halton), ED_STR);
        ctrl_editbox(s, KT_SCRIPTING_LINE_DELAY_MS, NO_SHORTCUT, 30,
                     HELPCTX(kitty_scriptfile), conf_editbox_handler,
                     I(CONF_script_line_delay), ED_INT);
        ctrl_editbox(s, KT_SCRIPTING_TIMEOUT_S, NO_SHORTCUT, 30,
                     HELPCTX(kitty_scriptfile), conf_editbox_handler,
                     I(CONF_script_timeout), ED_INT);
        ctrl_editbox(s, KT_SCRIPTING_CHARACTER_DELAY_MS, NO_SHORTCUT, 30,
                     HELPCTX(kitty_scriptfile), conf_editbox_handler,
                     I(CONF_script_char_delay), ED_INT);
        ctrl_editbox(s, KT_SCRIPTING_START_OF_CONDITION_COMMENT_LINE, NO_SHORTCUT, 30,
                     HELPCTX(kitty_scriptfile), conf_editbox_handler,
                     I(CONF_script_cond_line), ED_STR);
        ctrl_radiobuttons(s, KT_SCRIPTING_CR_LF_TRANSLATION, NO_SHORTCUT, 4,
                          HELPCTX(kitty_scriptfile), conf_radiobutton_handler,
                          I(CONF_script_crlf),
                          KT_SCRIPTING_OFF,   NO_SHORTCUT, I(0),   /* SCRIPT_OFF  */
                          KT_CFG_SCRIPT_NOLF, NO_SHORTCUT, I(1),   /* SCRIPT_NOLF */
                          KT_CFG_SCRIPT_CR,    NO_SHORTCUT, I(2),   /* SCRIPT_CR   */
                          KT_CFG_SCRIPT_REC,   NO_SHORTCUT, I(3));
 /* SCRIPT_REC  */
        ctrl_checkbox(s, KT_SCRIPTING_USE_CONDITIONS_FROM_FILE, NO_SHORTCUT,
                      HELPCTX(kitty_scriptfile), kitty_checkbox_int_handler,
                      I(CONF_script_cond_use));
        /* ---- receiving broadcasts: its own leaf under Session -----------
         * Scripting drives THIS session from a file; a broadcast is another
         * window typing into it. Separate concerns, separate panels - and
         * Scripting was carrying both boxes on one tall page. */
        ctrl_settitle(b, "Session/Broadcast",
                      KT_BROADCAST_OPTIONS_CONTROLLING_BROADCASTS);
        s = ctrl_getset(b, "Session/Broadcast", "broadcast",
                        KT_SCRIPTING_ACCEPT_BROADCASTS_FROM_OTHER_KITTY);
        ctrl_checkbox(s, KT_SCRIPTING_ACCEPT_BROADCAST_MESSAGES, NO_SHORTCUT,
                      HELPCTX(kitty_sendcmd), conf_checkbox_handler,
                      I(CONF_kitty_accept_broadcast));
        ctrl_text(s, KT_SCRIPTING_ANOTHER_KITTY_CAN_TYPE_INTO,
                  HELPCTX(kitty_sendcmd));
        /* The installation's master switch is an application setting; the
         * note says where. From the start-up window the button jumps there
         * (the same mechanism as the Certificate Authorities jump on Host
         * keys). A running terminal's Change Settings window has no
         * Application tab (scb_panel_kitty_settings builds nothing
         * mid-session), so there the button is not created at all and a
         * second note says where the leaf is instead. */
        ctrl_text(s, KT_CFG_BROADCAST_GLOBAL_NOTE, HELPCTX(kitty_sendcmd));
        if (midsession) {
            ctrl_text(s, KT_CFG_BROADCAST_GLOBAL_MIDSESSION, HELPCTX(kitty_sendcmd));
        } else {
            ctrl_columns(s, 2, 50, 50);
            c = ctrl_pushbutton(s, KT_CFG_BROADCAST_GLOBAL_JUMP, NO_SHORTCUT,
                                HELPCTX(kitty_sendcmd),
                                broadcast_global_jump_handler, I(0));
            c->column = 0;
            ctrl_columns(s, 1, 100);
        }
        ctrl_text(s, KT_SCRIPTING_ONLY_MESSAGES_CARRYING_THE_KEY,
                  HELPCTX(kitty_sendcmd));
        kitty_broadcast_key_controls(b, s);
    }

#ifdef MOD_LAUNCHER
    /* KiTTY: the Session/Startup panel - how a session can be STARTED from
     * outside its own window. Holds the launcher global hotkey, moved here
     * from Window/Behaviour: a machine-wide launch key is a session-startup
     * concern, and Behaviour had grown past the dialog's command buttons. */
    if (!GetPuttyFlag()) {
        ctrl_settitle(b, "Session/Startup",
                      KT_STARTUP_OPTIONS_CONTROLLING_HOW_THIS_SESSION);
        s = ctrl_getset(b, "Session/Startup", "launcher_hotkey",
                        KT_STARTUP_KITTY_LAUNCHER_GLOBAL_HOTKEY);
        ctrl_checkbox(s, KT_STARTUP_ENABLE_GLOBAL_HOTKEY, NO_SHORTCUT,
                      HELPCTX(kitty_launcher), conf_checkbox_handler,
                      I(CONF_launcher_global_hotkey_enabled));
        ctrl_editbox(s, KT_STARTUP_HOTKEY, NO_SHORTCUT, 40,
                     HELPCTX(kitty_launcher), conf_editbox_handler,
                     I(CONF_launcher_global_hotkey), ED_STR);
        ctrl_pushbutton(s, KT_STARTUP_CHECK_HOTKEY_AVAILABILITY, NO_SHORTCUT,
                        HELPCTX(kitty_launcher), kitty_launcher_hotkey_check_handler,
                        I(0));
        ctrl_text(s, KT_STARTUP_EXAMPLE_CTRL_ALT_K,
                  HELPCTX(kitty_launcher));
        s = ctrl_getset(b, "Session/Startup", "launcher_config",
                        KT_STARTUP_LAUNCHER_CONFIGURATION);
        /* KiTTY: exclude this session from the kitty -launcher tray menu.
         * Moved here from the Session panel - it configures the launcher's
         * view of the session, not the session itself. */
        ctrl_checkbox(s, KT_SESSION_HIDE_THIS_SESSION, NO_SHORTCUT,
                      HELPCTX(kitty_hide_launcher), conf_checkbox_handler,
                      I(CONF_launcherhide));
    }
#endif
}

/* The Terminal panel and its Keyboard/Bell/Features sub-panels. */
void scb_panel_terminal(struct controlbox *b)
{
    struct controlset *s;

    /*
     * The Terminal panel.
     */
    ctrl_settitle(b, "Terminal", KT_TERMINAL_OPTIONS_CONTROLLING_THE_TERMINAL_EMULATION);

    s = ctrl_getset(b, "Terminal", "general", KT_TERMINAL_SET_VARIOUS_TERMINAL_OPTIONS);
    ctrl_checkbox(s, KT_TERMINAL_AUTO_WRAP_MODE_INITIALLY, 'w',
                  HELPCTX(terminal_autowrap),
                  conf_checkbox_handler, I(CONF_wrap_mode));
    ctrl_checkbox(s, KT_TERMINAL_DEC_ORIGIN_MODE_INITIALLY, 'd',
                  HELPCTX(terminal_decom),
                  conf_checkbox_handler, I(CONF_dec_om));
    ctrl_checkbox(s, KT_TERMINAL_IMPLICIT_CR_IN_EVERY_LF, 'r',
                  HELPCTX(terminal_lfhascr),
                  conf_checkbox_handler, I(CONF_lfhascr));
    ctrl_checkbox(s, KT_TERMINAL_IMPLICIT_LF_IN_EVERY_CR, 'f',
                  HELPCTX(terminal_crhaslf),
                  conf_checkbox_handler, I(CONF_crhaslf));
    ctrl_checkbox(s, KT_TERMINAL_USE_BACKGROUND_COLOUR_TO_ERASE, 'e',
                  HELPCTX(terminal_bce),
                  conf_checkbox_handler, I(CONF_bce));
    ctrl_checkbox(s, KT_TERMINAL_ENABLE_BLINKING_TEXT, 'n',
                  HELPCTX(terminal_blink),
                  conf_checkbox_handler, I(CONF_blinktext));
    ctrl_editbox(s, KT_TERMINAL_ANSWERBACK_TO_E, 's', 100,
                 HELPCTX(terminal_answerback),
                 conf_editbox_handler, I(CONF_answerback), ED_STR);

    s = ctrl_getset(b, "Terminal", "ldisc", KT_TERMINAL_LINE_DISCIPLINE_OPTIONS);
    ctrl_radiobuttons(s, KT_TERMINAL_LOCAL_ECHO, 'l', 3,
                      HELPCTX(terminal_localecho),
                      conf_radiobutton_handler,I(CONF_localecho),
                      KT_TERMINAL_AUTO, I(AUTO),
                      KT_TERMINAL_FORCE, I(FORCE_ON),
                      KT_TERMINAL_FORCE_OFF, I(FORCE_OFF));
    ctrl_radiobuttons(s, KT_TERMINAL_LOCAL_LINE_EDITING, 't', 3,
                      HELPCTX(terminal_localedit),
                      conf_radiobutton_handler,I(CONF_localedit),
                      KT_TERMINAL_AUTO, I(AUTO),
                      KT_TERMINAL_FORCE, I(FORCE_ON),
                      KT_TERMINAL_FORCE_OFF, I(FORCE_OFF));

    s = ctrl_getset(b, "Terminal", "printing", KT_TERMINAL_REMOTE_CONTROLLED_PRINTING);
    ctrl_combobox(s, KT_TERMINAL_PRINTER_TO_SEND_ANSI_PRINTER, 'p', 100,
                  HELPCTX(terminal_printing),
                  printerbox_handler, P(NULL), P(NULL));
#ifdef MOD_PRINTCLIP
    if (!GetPuttyFlag()) {
        ctrl_checkbox(s, KT_TERMINAL_PRINT_TO_CLIPBOARD_INSTEAD, NO_SHORTCUT,
                      HELPCTX(kitty_printclip), kitty_printclip_handler,
                      I(CONF_printclip));
    }
#endif

    /*
     * The Terminal/Keyboard panel.
     */
    ctrl_settitle(b, "Terminal/Keyboard",
                  KT_KEYBOARD_OPTIONS_CONTROLLING_THE_EFFECTS);

    s = ctrl_getset(b, "Terminal/Keyboard", "mappings",
                    KT_KEYBOARD_CHANGE_THE_SEQUENCES_SENT_BY);
    ctrl_radiobuttons(s, KT_KEYBOARD_THE_BACKSPACE_KEY, 'b', 2,
                      HELPCTX(keyboard_backspace),
                      conf_radiobutton_bool_handler,
                      I(CONF_bksp_is_delete),
                      KT_KEYBOARD_CONTROL_H, I(0), KT_KEYBOARD_CONTROL_127, I(1));
    ctrl_radiobuttons(s, KT_KEYBOARD_THE_HOME_AND_END_KEYS, 'e', 2,
                      HELPCTX(keyboard_homeend),
                      conf_radiobutton_bool_handler,
                      I(CONF_rxvt_homeend),
                      KT_KEYBOARD_STANDARD, I(false), KT_KEYBOARD_RXVT, I(true));
    ctrl_radiobuttons(s, KT_KEYBOARD_THE_FUNCTION_KEYS_AND_KEYPAD, 'f', 4,
                      HELPCTX(keyboard_funkeys),
                      conf_radiobutton_handler,
                      I(CONF_funky_type),
                      KT_KEYBOARD_ESC_N, I(FUNKY_TILDE),
                      KT_KEYBOARD_LINUX, I(FUNKY_LINUX),
                      KT_KEYBOARD_XTERM_R6, I(FUNKY_XTERM),
                      KT_KEYBOARD_VT400, I(FUNKY_VT400),
                      KT_KEYBOARD_VT100, I(FUNKY_VT100P),
                      KT_KEYBOARD_SCO, I(FUNKY_SCO),
                      KT_KEYBOARD_XTERM_216, I(FUNKY_XTERM_216));
    ctrl_radiobuttons(s, KT_KEYBOARD_SHIFT_CTRL_ALT, 'w', 2,
                      HELPCTX(keyboard_sharrow),
                      conf_radiobutton_handler,
                      I(CONF_sharrow_type),
                      KT_KEYBOARD_CTRL_TOGGLES_APP_MODE, I(SHARROW_APPLICATION),
                      KT_KEYBOARD_XTERM_STYLE_BITMAP, I(SHARROW_BITMAP));
    ctrl_radiobuttons(s, KT_KEYBOARD_WORD_NAVIGATION_LEFT_RIGHT_ARROWS, 'v', 3,
                      HELPCTX(kitty_wordnav),
                      conf_radiobutton_handler,
                      I(CONF_word_nav_modifier),
                      KT_KEYBOARD_ALT, I(WORDNAV_ALT),
                      KT_KEYBOARD_CTRL, I(WORDNAV_CTRL),
                      KT_KEYBOARD_BOTH, I(WORDNAV_BOTH));
    if (!GetPuttyFlag())
        ctrl_checkbox(s, KT_KEYBOARD_ENTER_KEY_SENDS_CR_LF, NO_SHORTCUT,
                      HELPCTX(kitty_crlf), kitty_checkbox_int_handler,
                      I(CONF_enter_sends_crlf));
#ifdef MOD_DISABLEALTGR
    /* KiTTY: this TURNS AltGr OFF - measured, after the label first shipped
     * claiming the opposite. PuTTY already composes AltGr correctly: the key
     * handler treats a keystroke as Alt only when right-Alt is NOT down
     * (window.c, both the KF_ALTDOWN and the key_down tests), which is what
     * lets AltGr+Q produce "@" out of the box. Clearing the right-Alt state -
     * classic KiTTY's one-line implementation - removes that exemption, so
     * right Alt behaves like left Alt and composition stops.
     *
     * Worth having for anyone who wants Alt shortcuts from EITHER Alt key and
     * never composes; it is not a fix for AltGr, it is the off switch. */
    if (!GetPuttyFlag()) {
        /* Short on purpose: the long form clipped at the panel's right edge
         * (caught by the documentation screenshots); the text below carries
         * the detail. */
        ctrl_checkbox(s, KT_KEYBOARD_DISABLE_ALTGR_ACTS_AS_PLAIN,
                      NO_SHORTCUT, HELPCTX(kitty_altgr), kitty_checkbox_int_handler,
                      I(CONF_disablealtgr));
        ctrl_text(s, KT_KEYBOARD_OFF_DEFAULT_ALTGR_COMPOSES_CHARACTERS,
                  HELPCTX(kitty_altgr));
    }
#endif

    /* KiTTY: the application-keypad pair is its own leaf, which gives the
     * key-mapping block above the room the extra rows need. */
    ctrl_settitle(b, "Terminal/Keyboard/Application keypad",
                  KT_KEYBOARD_OPTIONS_CONTROLLING_THE_APPLICATION_KEYPAD);
    s = ctrl_getset(b, "Terminal/Keyboard/Application keypad", "appkeypad",
                    KT_KEYBOARD_APPLICATION_KEYPAD_SETTINGS);
    ctrl_radiobuttons(s, KT_KEYBOARD_INITIAL_STATE_OF_CURSOR_KEYS, 'r', 3,
                      HELPCTX(keyboard_appcursor),
                      conf_radiobutton_bool_handler,
                      I(CONF_app_cursor),
                      KT_KEYBOARD_NORMAL, I(0), KT_KEYBOARD_APPLICATION, I(1));
    ctrl_radiobuttons(s, KT_KEYBOARD_INITIAL_STATE_OF_NUMERIC_KEYPAD, 'n', 3,
                      HELPCTX(keyboard_appkeypad),
                      numeric_keypad_handler, P(NULL),
                      KT_KEYBOARD_NORMAL, I(0), KT_KEYBOARD_APPLICATION, I(1), KT_KEYBOARD_NETHACK, I(2));

    /*
     * The Terminal/Bell panel.
     */
    ctrl_settitle(b, "Terminal/Bell",
                  KT_BELL_OPTIONS_CONTROLLING_THE_TERMINAL_BELL);

    s = ctrl_getset(b, "Terminal/Bell", "style", KT_BELL_SET_THE_STYLE_OF_BELL);
    ctrl_radiobuttons(s, KT_BELL_ACTION_TO_HAPPEN_WHEN, 'b', 1,
                      HELPCTX(bell_style),
                      conf_radiobutton_handler, I(CONF_beep),
                      KT_BELL_NONE_BELL_DISABLED, I(BELL_DISABLED),
                      KT_BELL_MAKE_DEFAULT_SYSTEM_ALERT_SOUND, I(BELL_DEFAULT),
                      KT_BELL_VISUAL_BELL_FLASH_WINDOW, I(BELL_VISUAL));
    if (!GetPuttyFlag()) {
        ctrl_checkbox(s, KT_BELL_PUT_WINDOW_IN_FOREGROUND, NO_SHORTCUT,
                      HELPCTX(kitty_fgbell), conf_checkbox_handler,
                      I(CONF_foreground_on_bell));
    }

    s = ctrl_getset(b, "Terminal/Bell", "overload",
                    KT_BELL_CONTROL_THE_BELL_OVERLOAD_BEHAVIOUR);
    ctrl_checkbox(s, KT_BELL_BELL_IS_TEMPORARILY_DISABLED_WHEN, 'd',
                  HELPCTX(bell_overload),
                  conf_checkbox_handler, I(CONF_bellovl));
    ctrl_editbox(s, KT_BELL_OVER_USE_MEANS_THIS_MANY, 'm', 20,
                 HELPCTX(bell_overload),
                 conf_editbox_handler, I(CONF_bellovl_n), ED_INT);

    static const struct conf_editbox_handler_type conf_editbox_tickspersec = {
        .type = EDIT_FIXEDPOINT, .denominator = TICKSPERSEC};

    ctrl_editbox(s, KT_BELL_IN_THIS_MANY_SECONDS, 't', 20,
                 HELPCTX(bell_overload),
                 conf_editbox_handler, I(CONF_bellovl_t),
                 CP(&conf_editbox_tickspersec));
    ctrl_text(s, KT_BELL_THE_BELL_IS_RE_ENABLED,
              HELPCTX(bell_overload));
    ctrl_editbox(s, KT_BELL_SECONDS_OF_SILENCE_REQUIRED, 's', 20,
                 HELPCTX(bell_overload),
                 conf_editbox_handler, I(CONF_bellovl_s),
                 CP(&conf_editbox_tickspersec));

    /*
     * The Terminal/Features panel.
     */
    ctrl_settitle(b, "Terminal/Features",
                  KT_FEATURES_ENABLING_AND_DISABLING_ADVANCED_TERMINAL);

    s = ctrl_getset(b, "Terminal/Features", "main", NULL);
    ctrl_checkbox(s, KT_FEATURES_DISABLE_APPLICATION_CURSOR_KEYS_MODE, 'u',
                  HELPCTX(features_application),
                  conf_checkbox_handler, I(CONF_no_applic_c));
    ctrl_checkbox(s, KT_FEATURES_DISABLE_APPLICATION_KEYPAD_MODE, 'k',
                  HELPCTX(features_application),
                  conf_checkbox_handler, I(CONF_no_applic_k));
    ctrl_checkbox(s, KT_FEATURES_DISABLE_XTERM_STYLE_MOUSE_REPORTING, 'x',
                  HELPCTX(features_mouse),
                  conf_checkbox_handler, I(CONF_no_mouse_rep));
    ctrl_checkbox(s, KT_FEATURES_DISABLE_REMOTE_CONTROLLED_TERMINAL_RESIZING, 's',
                  HELPCTX(features_resize),
                  conf_checkbox_handler,
                  I(CONF_no_remote_resize));
    ctrl_checkbox(s, KT_FEATURES_DISABLE_SWITCHING_TO_ALTERNATE_TERMINAL, 'w',
                  HELPCTX(features_altscreen),
                  conf_checkbox_handler, I(CONF_no_alt_screen));
    ctrl_checkbox(s, KT_FEATURES_DISABLE_REMOTE_CONTROLLED_WINDOW_TITLE, 't',
                  HELPCTX(features_retitle),
                  conf_checkbox_handler,
                  I(CONF_no_remote_wintitle));
    ctrl_radiobuttons(s, KT_FEATURES_RESPONSE_TO_REMOTE_TITLE_QUERY, 'q', 3,
                      HELPCTX(features_qtitle),
                      conf_radiobutton_handler,
                      I(CONF_remote_qtitle_action),
                      KT_LOGGING_NONE, I(TITLE_NONE),
                      KT_FEATURES_EMPTY_STRING, I(TITLE_EMPTY),
                      KT_FEATURES_WINDOW_TITLE, I(TITLE_REAL));
    ctrl_checkbox(s, KT_FEATURES_DISABLE_REMOTE_CONTROLLED_CLEARING, 'e',
                  HELPCTX(features_clearscroll),
                  conf_checkbox_handler,
                  I(CONF_no_remote_clearscroll));
    ctrl_checkbox(s, KT_FEATURES_DISABLE_DESTRUCTIVE_BACKSPACE_ON_SERVER,'b',
                  HELPCTX(features_dbackspace),
                  conf_checkbox_handler, I(CONF_no_dbackspace));
    ctrl_checkbox(s, KT_FEATURES_DISABLE_REMOTE_CONTROLLED_CHARACTER_SET,
                  'r', HELPCTX(features_charset), conf_checkbox_handler,
                  I(CONF_no_remote_charset));
    ctrl_checkbox(s, KT_FEATURES_DISABLE_ARABIC_TEXT_SHAPING,
                  'l', HELPCTX(features_arabicshaping), conf_checkbox_handler,
                  I(CONF_no_arabicshaping));
    ctrl_checkbox(s, KT_FEATURES_DISABLE_BIDIRECTIONAL_TEXT_DISPLAY,
                  'd', HELPCTX(features_bidi), conf_checkbox_handler,
                  I(CONF_no_bidi));
    ctrl_checkbox(s, KT_FEATURES_DISABLE_BRACKETED_PASTE_MODE,
                  'p', HELPCTX(features_bracketed_paste), conf_checkbox_handler,
                  I(CONF_no_bracketed_paste));
    if (!GetPuttyFlag())
        ctrl_checkbox(s, KT_FEATURES_DISABLE_FOCUS_REPORTING, NO_SHORTCUT,
                      HELPCTX(kitty_nofocusrep), conf_checkbox_handler,
                      I(CONF_no_focus_rep));
}

/* The Window panel and its Appearance/Behaviour sub-panels, plus the
 * KiTTY Transparency/Hyperlinks/position+icon/Background-image panels. */
/* Window/Hyperlinks: put KiTTY's shipped URL pattern back into the
 * custom-regex field. Confirmed first - the button OVERWRITES whatever the
 * field holds, and a hand-built regex is real work. Written to the conf and
 * then refreshed into the field, so the display and the store agree. */
static void kitty_urlregex_reset_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                         void *data, int event)
{
    extern const char *urlhack_default_regex;   /* kitty/url/urlhack.c */
    extern int kitty_confirm_box(HWND owner, const char *caption,
                                 const char *text, const char *warn_red); /* kitty_win.c */
    Conf *conf = (Conf *)data;
    if (event != EVENT_ACTION)
        return;
    if (!kitty_confirm_box(GetActiveWindow(),
                           KT_CAP_RESET_URL_REGEX,
                           KT_CFG_RESET_URL_REGEX_Q, NULL))
        return;
    conf_set_str(conf, CONF_url_regex, urlhack_default_regex);
    dlg_refresh(NULL, dlg);
}

/* KiTTY: does the Background leaf (transparency + background image) exist? */
static bool scb_background_leaf_wanted(void)
{
    if (GetTransparencyFlag())
        return true;
#ifdef MOD_BACKGROUNDIMAGE
    if (GetBackgroundImageFlag())
        return true;
#endif
    return false;
}

/*
 * KiTTY: the Window branch of the Category tree, in display order.
 *
 * A leaf appears in the tree where its title set is created, so the order of
 * the whole branch is decided here and nowhere else; the panel functions below
 * only add their control sets to these paths. Paths are IDENTIFIERS - the
 * settings code, remembered panels, help and the QA harnesses key on them -
 * and kitty_tree_text.h holds what the tree shows: "Window/Selection" reads
 * "Copy & Paste", "Window/Title" reads "Title & Icon". A leaf whose feature
 * is switched off is not created at all.
 */
static void scb_window_tree(struct controlbox *b)
{
    char *str;

    str = dupprintf(KT_CFG_APPEARANCE_TITLE_FMT, SCB_TITLE_APPNAME);
    ctrl_settitle(b, "Window/Appearance", str);
    sfree(str);
    if (!GetPuttyFlag())
        ctrl_settitle(b, "Window/Appearance/Position",
                      KT_APPEARANCE_OPTIONS_CONTROLLING_WHERE_THE_WINDOW);
    ctrl_settitle(b, "Window/Appearance/Colours",
                  KT_COLOURS_OPTIONS_CONTROLLING_USE_OF_COLOURS);
    str = dupprintf(KT_CFG_PRECISE_COLOURS_TITLE_FMT, SCB_TITLE_APPNAME);
    ctrl_settitle(b, "Window/Appearance/Colours/Precise colours", str);
    sfree(str);
    if (!GetPuttyFlag() && scb_background_leaf_wanted())
        ctrl_settitle(b, "Window/Appearance/Background", KT_BACKGROUND_TITLE);
    str = dupprintf(KT_CFG_BEHAVIOUR_TITLE_FMT, SCB_TITLE_APPNAME);
    ctrl_settitle(b, "Window/Behaviour", str);
    sfree(str);
    ctrl_settitle(b, "Window/Selection",
                  KT_SELECTION_OPTIONS_CONTROLLING_COPY_AND_PASTE);
    ctrl_settitle(b, "Window/Selection/Copy",
                  KT_COPY_CLASSES_OF_CHARACTER_THAT_GROUP);
    ctrl_settitle(b, "Window/Selection/Remote clipboard",
                  KT_REMOTE_CLIPBOARD_WHAT_A_REMOTE_HOST_MAY);
    ctrl_settitle(b, "Window/Selection/Remote clipboard/Limits",
                  KT_LIMITS_BOUNDS_ON_WHAT_A_PERMITTED);
    ctrl_settitle(b, "Window/Selection/Remote clipboard/Notices",
                  KT_NOTICES_BEING_TOLD_ABOUT_REMOTE_CLIPBOARD);
    if (!GetPuttyFlag())
        ctrl_settitle(b, "Window/Hyperlinks",
                      KT_HYPERLINKS_OPTIONS_CONTROLLING_CLICKABLE_URL_HYPERLINKS);
    ctrl_settitle(b, "Window/Charset translation",
                  KT_CHARSET_TRANSLATION_OPTIONS_CONTROLLING_CHARACTER_SET_TRANSLATION);
    ctrl_settitle(b, "Window/Title", KT_TITLE_WINDOW_TITLE_AND_ICON_OPTIONS);
}

void scb_panel_window(struct controlbox *b, bool midsession, int protocol)
{
    const struct BackendVtable *backvt;
    struct controlset *s;
    dlgcontrol *c;
    bool resize_forbidden = false;
    char *str;

    /*
     * The Window panel.
     */
    str = dupprintf(KT_CFG_WINDOW_TITLE_FMT, SCB_TITLE_APPNAME);
    ctrl_settitle(b, "Window", str);
    sfree(str);
    scb_window_tree(b);          /* every Window leaf, in display order */

    backvt = backend_vt_from_proto(protocol);
    if (backvt)
        resize_forbidden = (backvt->flags & BACKEND_RESIZE_FORBIDDEN);

    if (!resize_forbidden || !midsession) {
        s = ctrl_getset(b, "Window", "size", KT_WINDOW_SET_THE_SIZE);
        ctrl_columns(s, 2, 50, 50);
        c = ctrl_editbox(s, KT_WINDOW_COLUMNS, 'm', 100,
                         HELPCTX(window_size),
                         conf_editbox_handler, I(CONF_width), ED_INT);
        c->column = 0;
        c = ctrl_editbox(s, KT_WINDOW_ROWS, 'r', 100,
                         HELPCTX(window_size),
                         conf_editbox_handler, I(CONF_height),ED_INT);
        c->column = 1;
        ctrl_columns(s, 1, 100);
    }

    s = ctrl_getset(b, "Window", "scrollback",
                    KT_WINDOW_CONTROL_THE_SCROLLBACK);
    ctrl_editbox(s, KT_WINDOW_LINES_OF_SCROLLBACK, 's', 50,
                 HELPCTX(window_scrollback),
                 conf_editbox_handler, I(CONF_savelines), ED_INT);
    ctrl_checkbox(s, KT_WINDOW_DISPLAY_SCROLLBAR, 'd',
                  HELPCTX(window_scrollback),
                  conf_checkbox_handler, I(CONF_scrollbar));
    if (!GetPuttyFlag()) {
        ctrl_editbox(s, KT_WINDOW_LINES_SCROLLED_PER_WHEEL_TURN, NO_SHORTCUT, 50,
                     HELPCTX(kitty_wheel),
                     conf_editbox_handler, I(CONF_scrolllines), ED_INT);
        /* Lines of their own rather than a longer label: an editbox label is a
         * static, laid out once at the width of its first text, so anything
         * longer is simply cut off.
         *
         * TWO controls rather than one that wraps, so the break falls where it
         * reads best - the two special values together, the ordinary case on
         * its own line - instead of wherever the panel width happens to put
         * it. */
        ctrl_text(s, KT_WINDOW_1_HALF_A_SCREEN,
                  HELPCTX(no_help));
        ctrl_text(s, KT_WINDOW_OR_A_POSITIVE_NUMBER, HELPCTX(no_help));
    }
    ctrl_checkbox(s, KT_WINDOW_RESET_SCROLLBACK_ON_KEYPRESS, 'k',
                  HELPCTX(window_scrollback),
                  conf_checkbox_handler, I(CONF_scroll_on_key));
    ctrl_checkbox(s, KT_WINDOW_RESET_SCROLLBACK_ON_DISPLAY_ACTIVITY, 'p',
                  HELPCTX(window_scrollback),
                  conf_checkbox_handler, I(CONF_scroll_on_disp));
    ctrl_checkbox(s, KT_WINDOW_PUSH_ERASED_TEXT_INTO_SCROLLBACK, 'e',
                  HELPCTX(window_erased),
                  conf_checkbox_handler,
                  I(CONF_erase_to_scrollback));
    /* What the setting is FOR, which the wording does not say: with it on, a
     * screen that was cleared can still be scrolled back to. */
    if (!GetPuttyFlag())
        ctrl_text(s, KT_WINDOW_TURN_OFF_WHERE_A_CLEARED,
                  HELPCTX(window_erased));

    /*
     * The Window/Appearance panel.
     */
    s = ctrl_getset(b, "Window/Appearance", "cursor",
                    KT_APPEARANCE_ADJUST_THE_USE);
    ctrl_radiobuttons(s, KT_APPEARANCE_CURSOR_APPEARANCE, NO_SHORTCUT, 3,
                      HELPCTX(appearance_cursor),
                      conf_radiobutton_handler,
                      I(CONF_cursor_type),
                      KT_APPEARANCE_BLOCK, 'l', I(CURSOR_BLOCK),
                      KT_APPEARANCE_UNDERLINE, 'u', I(CURSOR_UNDERLINE),
                      KT_APPEARANCE_VERTICAL_LINE, 'v', I(CURSOR_VERTICAL_LINE));
    ctrl_checkbox(s, KT_APPEARANCE_CURSOR_BLINKS, 'b',
                  HELPCTX(appearance_cursor),
                  conf_checkbox_handler, I(CONF_blink_cur));

    s = ctrl_getset(b, "Window/Appearance", "font",
                    KT_APPEARANCE_FONT_SETTINGS);
    ctrl_fontsel(s, KT_APPEARANCE_FONT_USED_IN_THE_TERMINAL, 'n',
                 HELPCTX(appearance_font),
                 conf_fontsel_handler, I(CONF_font));
    /* KiTTY: line spacing as a percentage of the font's own line height
     * (cyd01/KiTTY#524). 100 leaves the metrics untouched; a percentage rather
     * than pixels so it survives a font change or a different-DPI monitor. */
    if (!GetPuttyFlag()) {
        /* One row: the Appearance panel is already at its full height, and a
         * paragraph here pushed the icon controls off the bottom of the window.
         * The caveat (line-drawing characters stop joining up above 100) lives
         * in FEATURES.md instead. */
        ctrl_editbox(s, KITTY_LINESPC_LABEL, NO_SHORTCUT,
                     25, HELPCTX(no_help),
                     kitty_linespacing_handler, I(CONF_line_spacing), ED_INT);
    }

    s = ctrl_getset(b, "Window/Appearance", "mouse",
                    KT_APPEARANCE_ADJUST_THE_USE_2);
    ctrl_checkbox(s, KT_APPEARANCE_HIDE_MOUSE_POINTER_WHEN_TYPING, 'p',
                  HELPCTX(appearance_hidemouse),
                  conf_checkbox_handler, I(CONF_hide_mouseptr));

    s = ctrl_getset(b, "Window/Appearance", "border",
                    KT_APPEARANCE_ADJUST_THE_WINDOW_BORDER);
    ctrl_editbox(s, KT_APPEARANCE_GAP_BETWEEN_TEXT_AND_WINDOW, 'e', 20,
                 HELPCTX(appearance_border),
                 conf_editbox_handler,
                 I(CONF_window_border), ED_INT);

    /*
     * The Window/Behaviour panel.
     */
    /* First what the window does when its session ends, then what it
     * remembers - the pair kitty_on_window_closing() acts on - then the
     * Windows-convention switches. "Close window on exit" is stock PuTTY's
     * and used to sit on the Session panel; a dropped link is re-dialled by
     * auto-reconnect before this setting is consulted, so it decides only
     * what happens once the session has ENDED. */
    s = ctrl_getset(b, "Window/Behaviour", "closing",
                    KT_BEHAVIOUR_CLOSING_THE_WINDOW);
    ctrl_radiobuttons(s, KT_SESSION_CLOSE_TERMINAL_WINDOW_ON_EXIT, 'x', 4,
                      HELPCTX(session_coe),
                      conf_radiobutton_handler,
                      I(CONF_close_on_exit),
                      KT_SESSION_ALWAYS, I(FORCE_ON),
                      KT_SESSION_NEVER, I(FORCE_OFF),
                      KT_SESSION_ONLY_ON_CLEAN_EXIT, I(AUTO));
    ctrl_checkbox(s, KT_BEHAVIOUR_WARN_BEFORE_CLOSING_WINDOW, 'w',
                  HELPCTX(behaviour_closewarn),
                  conf_checkbox_handler, I(CONF_warn_on_close));
    if (!GetPuttyFlag()) {
        s = ctrl_getset(b, "Window/Behaviour", "remember",
                        KT_BEHAVIOUR_REMEMBERING);
        /* KiTTY: "Remember window position" used to sit here too; it is on
         * Window > Appearance > Position now, beside the fixed position it
         * combines with (the Window/Appearance/Position leaf below). */
        /* KiTTY: writes this session back when its window closes - the settings
         * as they stand at that moment, so a font or colour changed mid-session
         * survives, plus the window's size, position and maximised state. An
         * unnamed session and "Default Settings" are deliberately never
         * written. */
        ctrl_checkbox(s, KT_SESSION_SAVE_SETTINGS_ON_EXIT, NO_SHORTCUT,
                      HELPCTX(kitty_save_on_exit), conf_checkbox_handler,
                      I(CONF_saveonexit));
    }

    /* The Window/Title panel ("Title & Icon" in the tree): the title group,
     * then the icon group - both are about how the window is told apart. */
    s = ctrl_getset(b, "Window/Title", "title",
                    KT_TITLE_ADJUST_THE_BEHAVIOUR);
    ctrl_editbox(s, KT_TITLE_WINDOW_TITLE, 't', 100,
                 HELPCTX(appearance_title),
                 conf_editbox_handler, I(CONF_wintitle), ED_STR);
    /* KiTTY: the title understands placeholders (%%h, %%s, ...). Classic KiTTY
     * printed all eight as static lines here, which is a lot of panel for a
     * reference you need once; this opens a modeless list you can copy from and
     * leave open while typing the title. */
    if (!GetPuttyFlag())
        ctrl_pushbutton(s, KT_TITLE_PLACEHOLDERS_H_S, NO_SHORTCUT,
                        HELPCTX(appearance_title),
                        kitty_title_placeholders_handler, I(0));
    ctrl_checkbox(s, KT_TITLE_SEPARATE_WINDOW_AND_ICON_TITLES, 'i',
                  HELPCTX(appearance_title),
                  conf_checkbox_handler,
                  I(CHECKBOX_INVERT | CONF_win_name_always));

    s = ctrl_getset(b, "Window/Behaviour", "main", NULL);
    /* KiTTY startup window state. All three are honoured in window.c (maximize
     * and fullscreen at the ShowWindow, send-to-tray once the session is up),
     * and all three are saved with the session - but the config-box controls
     * were lost in the 0.84 port, leaving the settings unreachable and existing
     * sessions looking as if they had forgotten them. NO_SHORTCUT: this panel
     * has run out of free accelerator letters. */
    if (!GetPuttyFlag()) {
        /* All three are INT keys, so they need kitty_checkbox_int_handler:
         * conf_checkbox_handler asserts in conf_get_bool on a non-BOOL key. */
        ctrl_checkbox(s, KT_BEHAVIOUR_SEND_TO_TRAY_ON_STARTUP, NO_SHORTCUT,
                      HELPCTX(kitty_behaviour),
                      kitty_checkbox_int_handler, I(CONF_sendtotray));
        ctrl_checkbox(s, KT_BEHAVIOUR_MAXIMIZE_ON_STARTUP, NO_SHORTCUT,
                      HELPCTX(kitty_behaviour),
                      kitty_checkbox_int_handler, I(CONF_maximize));
        ctrl_checkbox(s, KT_BEHAVIOUR_FULL_SCREEN_ON_STARTUP, NO_SHORTCUT,
                      HELPCTX(kitty_behaviour),
                      kitty_checkbox_int_handler, I(CONF_fullscreen));
        /* KiTTY: Ctrl+Tab between windows. Two gates, as in classic KiTTY:
         * [KiTTY] ctrltab (or -noctrltab) decides whether the feature exists at
         * all, and this per-session box turns it on for a session. The port
         * kept both gates but not this box, which left CONF_ctrl_tab_switch at
         * its default 0 with no way to change it - so ctrltab=yes did nothing.
         * Offered only when the feature is enabled, and not mid-session,
         * matching classic (0.76b windows/config.c). */
        if (!midsession && GetCtrlTabFlag())
            ctrl_checkbox(s, KT_BEHAVIOUR_SWITCH_KITTY_WINDOWS_WITH_CTRL, NO_SHORTCUT,
                          HELPCTX(kitty_behaviour),
                          kitty_checkbox_int_handler, I(CONF_ctrl_tab_switch));
    }

    /*
     * KiTTY (classic parity): which of the window's own buttons exist. A kiosk /
     * embedding feature - KiTTY hosted in another application's tab has no use for
     * a Close button that would strand the host.
     *
     * The system-menu box comes FIRST and says what it does to the others, because
     * Windows will not draw any caption button without WS_SYSMENU. Classic KiTTY
     * greyed the other three to show that dependency; this port has no dlg_enable()
     * to grey a control with, so the label carries it instead - the behaviour is
     * the same either way, since it is Windows enforcing it rather than us.
     */
    if (!GetPuttyFlag()) {
        s = ctrl_getset(b, "Window/Behaviour", "windowbuttons",
                        KT_BEHAVIOUR_WINDOW_BUTTONS_FOR_KIOSK);
        /* Short enough to fit the panel. The dependency still has to be stated -
         * Windows draws no caption button without WS_SYSMENU - and with no
         * dlg_enable() to grey the other three, the label is the only place left
         * to say it. */
        ctrl_checkbox(s, KT_BEHAVIOUR_SYSTEM_MENU_OFF_HIDES_ALL,
                      NO_SHORTCUT, HELPCTX(kitty_behaviour),
                      conf_checkbox_handler, I(CONF_window_has_sysmenu));
        ctrl_checkbox(s, KT_BEHAVIOUR_ALLOW_CLOSING_ALSO_DISABLES,
                      NO_SHORTCUT, HELPCTX(kitty_behaviour),
                      conf_checkbox_handler, I(CONF_window_closable));
        ctrl_checkbox(s, KT_BEHAVIOUR_MINIMIZE_BUTTON, NO_SHORTCUT, HELPCTX(kitty_behaviour),
                      conf_checkbox_handler, I(CONF_window_minimizable));
        ctrl_checkbox(s, KT_BEHAVIOUR_MAXIMIZE_BUTTON, NO_SHORTCUT, HELPCTX(kitty_behaviour),
                      conf_checkbox_handler, I(CONF_window_maximizable));
    }

    /*
     * KiTTY: which entries the window menu's Tools submenu holds. It decides
     * what the MENU shows, not how a file moves, which is why it is window
     * behaviour and not a transfer setting. Off = the entry is not built
     * (window.c, WM_INITMENUPOPUP) and its [Shortcuts] key does nothing
     * (kitty_shortcuts.c), so the key reaches the terminal instead.
     */
    if (!GetPuttyFlag()) {
        s = ctrl_getset(b, "Window/Behaviour", "tools", KT_BEHAVIOUR_TOOLS_MENU);
        ctrl_checkbox(s, KT_BEHAVIOUR_TOOLS_SENDFILE, NO_SHORTCUT,
                      HELPCTX(kitty_behaviour),
                      conf_checkbox_handler, I(CONF_tools_sendfile));
        /* F1 on Get File lands on its walk-through, not on this panel. */
        ctrl_checkbox(s, KT_BEHAVIOUR_TOOLS_GETFILE, NO_SHORTCUT,
                      HELPCTX(kitty_transfers_getfile),
                      conf_checkbox_handler, I(CONF_tools_getfile));
        ctrl_checkbox(s, KT_BEHAVIOUR_TOOLS_WINSCP, NO_SHORTCUT,
                      HELPCTX(kitty_behaviour),
                      conf_checkbox_handler, I(CONF_tools_winscp));
        ctrl_checkbox(s, KT_BEHAVIOUR_TOOLS_FILEZILLA, NO_SHORTCUT,
                      HELPCTX(kitty_behaviour),
                      conf_checkbox_handler, I(CONF_tools_filezilla));
    }

    /*
     * The Window/Appearance/Background leaf, first group: transparency (KiTTY).
     */
    if (!GetPuttyFlag() && GetTransparencyFlag()) {
        s = ctrl_getset(b, "Window/Appearance/Background", "bg_transparency",
                        KT_TRANSPARENCY_TRANSPARENCY_SETTING);
        ctrl_editbox(s, KT_TRANSPARENCY_TRANSPARENCY, NO_SHORTCUT, 20,
                     HELPCTX(kitty_transparency),
                     conf_editbox_handler,
                     I(CONF_transparencynumber), ED_INT);
        ctrl_text(s, KT_TRANSPARENCY_FROM_0_VISIBLE_TO_255,
                  HELPCTX(kitty_transparency));
        ctrl_text(s, KT_TRANSPARENCY_1_TO_DISABLE_COMPLETELY, HELPCTX(kitty_transparency));
    }

    /*
     * The Window/Hyperlinks panel (KiTTY).
     */
    if (!GetPuttyFlag()) {
        s = ctrl_getset(b, "Window/Hyperlinks", "main",
                        KT_HYPERLINKS_HYPERLINK_BEHAVIOUR);
        ctrl_checkbox(s, KT_HYPERLINKS_REQUIRE_CTRL_KEY_TO_CLICK, NO_SHORTCUT,
                      HELPCTX(kitty_hyperlinks), kitty_checkbox_int_handler,
                      I(CONF_url_ctrl_click));
        ctrl_checkbox(s, KT_HYPERLINKS_UNDERLINE_HYPERLINKS, NO_SHORTCUT,
                      HELPCTX(kitty_hyperlinks), kitty_checkbox_int_handler,
                      I(CONF_url_underline));
        ctrl_checkbox(s, KT_HYPERLINKS_SHOW_HAND_CURSOR_WHEN_HOVERING, NO_SHORTCUT,
                      HELPCTX(kitty_hyperlinks), kitty_checkbox_int_handler,
                      I(CONF_url_hover_cursor));
        ctrl_checkbox(s, KT_HYPERLINKS_USE_THE_DEFAULT_BROWSER, NO_SHORTCUT,
                      HELPCTX(kitty_hyperlinks), kitty_checkbox_int_handler,
                      I(CONF_url_defbrowser));
        ctrl_filesel(s, KT_HYPERLINKS_OTHER_BROWSER, NO_SHORTCUT,
                     FILTER_ALL_FILES, false, KT_HYPERLINKS_SELECT_BROWSER_EXECUTABLE,
                     HELPCTX(kitty_hyperlinks),
                     conf_filesel_handler, I(CONF_url_browser));
        ctrl_checkbox(s, KT_HYPERLINKS_USE_THE_DEFAULT_REGULAR_EXPRESSION, NO_SHORTCUT,
                      HELPCTX(kitty_hyperlinks), kitty_checkbox_int_handler,
                      I(CONF_url_defregex));
        /* Short label + a wide field: a regex deserves the room. */
        ctrl_editbox(s, KT_HYPERLINKS_CUSTOM_REGEX, NO_SHORTCUT, 80,
                     HELPCTX(kitty_hyperlinks), conf_editbox_handler,
                     I(CONF_url_regex), ED_STR);
        /* Reset the field to KiTTY's shipped pattern - behind a
         * confirmation, because it OVERWRITES whatever is typed there. */
        {
            dlgcontrol *rc;
            ctrl_columns(s, 2, 60, 40);
            rc = ctrl_pushbutton(s, KT_HYPERLINKS_RESET_REGEX, NO_SHORTCUT,
                                 HELPCTX(kitty_hyperlinks),
                                 kitty_urlregex_reset_handler, I(0));
            rc->column = 1;
            ctrl_columns(s, 1, 100);
        }
    }

    /*
     * The Window/Appearance panel: window icon + remember position (KiTTY).
     */
    if (!GetPuttyFlag()) {
        /* KiTTY: this PINS the window to typed coordinates - it remembers
         * nothing. It was labelled "Remember window position", the same words
         * as the genuinely-remembering option in Window > Behaviour, in a
         * different panel, doing the opposite thing. The checkbox now also
         * gates the pin: it was ignored entirely, so coordinates >= 0 pinned
         * the window whether or not the box was ticked (window.c). */
        /* Its own leaf: fixed coordinates are a placement concern, not a
         * looks one, and Appearance was full. */
        s = ctrl_getset(b, "Window/Appearance/Position", "position",
                        KT_APPEARANCE_WHERE_THE_WINDOW_OPENS);
        /* KiTTY: the genuinely-remembering option, moved here from Window >
         * Behaviour so the two ways of placing a window sit together. It
         * remembers PER SESSION and per monitor LAYOUT - position and columns
         * x rows - so docking or unplugging a screen restores the window
         * where it belonged on that layout instead of stranding it
         * off-screen; it deliberately does not restore a maximised or
         * minimised state (windows/window.c, kitty/kitty_winpos.c). Top and
         * Left below show this session's entry for the layout the box is on
         * now (the Load path copies it into the conf). */
        ctrl_checkbox(s, KT_APPEARANCE_REMEMBER_WINDOW_POSITION, NO_SHORTCUT,
                      HELPCTX(kitty_winpos_remember), conf_checkbox_handler,
                      I(CONF_remember_winpos));
        ctrl_text(s, KT_APPEARANCE_REMEMBER_NOTE, HELPCTX(kitty_winpos_remember));
        ctrl_checkbox(s, KT_APPEARANCE_OPEN_THE_WINDOW, NO_SHORTCUT,
                      HELPCTX(kitty_winpos), conf_checkbox_handler,
                      I(CONF_set_windowpos));
        ctrl_editbox(s, KT_APPEARANCE_TOP, NO_SHORTCUT, 20, HELPCTX(kitty_winpos),
                     conf_editbox_handler, I(CONF_ypos), ED_INT);
        ctrl_editbox(s, KT_APPEARANCE_LEFT, NO_SHORTCUT, 20, HELPCTX(kitty_winpos),
                     conf_editbox_handler, I(CONF_xpos), ED_INT);
        ctrl_text(s, KT_APPEARANCE_A_FIXED_POSITION_WINS_OVER, HELPCTX(kitty_winpos));

        /* KiTTY: the icon group shares the "Title & Icon" leaf with the
         * window title (appended to Appearance it once pushed that panel
         * past the dialog's command buttons). */
        s = ctrl_getset(b, "Window/Title", "icon",
                        KT_ICON_DEFINE_THE_WINDOW_ICON);
        ctrl_editbox(s, KT_ICON_ICON_FROM_INTERNAL_RESOURCES, NO_SHORTCUT, 40,
                     HELPCTX(kitty_icon), conf_editbox_handler,
                     I(CONF_icone), ED_INT);
        ctrl_filesel(s, KT_ICON_EXTERNAL_ICON_FILE, NO_SHORTCUT,
                     FILTER_ICON_FILES, false, KT_ICON_SELECT_ICON_FILE,
                     HELPCTX(kitty_icon),
                     conf_filesel_handler, I(CONF_iconefile));
    }

#ifdef MOD_BACKGROUNDIMAGE
    /* The Background leaf's image groups (KiTTY). Engine: kitty_image.c. */
    if (!GetPuttyFlag() && GetBackgroundImageFlag()) {
        s = ctrl_getset(b, "Window/Appearance/Background", "bg_style",
                        KT_BACK_IMAGE_BACKGROUND_SETTINGS);
        ctrl_radiobuttons(s, KT_BACK_IMAGE_BACKGROUND_STYLE, NO_SHORTCUT, 3,
                          HELPCTX(kitty_bgimage),
                          conf_radiobutton_handler, I(CONF_bg_type),
                          KT_BACK_IMAGE_SOLID, NO_SHORTCUT, I(0),
                          KT_BACK_IMAGE_DESKTOP, NO_SHORTCUT, I(1),
                          KT_BACK_IMAGE_IMAGE, NO_SHORTCUT, I(2));

        s = ctrl_getset(b, "Window/Appearance/Background", "bg_wp_img_settings",
                        KT_BACK_IMAGE_DESKTOP_AND_IMAGE_SETTINGS);
        ctrl_editbox(s, KT_BACK_IMAGE_OPACITY_NEGATIVE_WITH_IMAGE,
                     NO_SHORTCUT, 20, HELPCTX(kitty_bgimage), conf_editbox_handler,
                     I(CONF_bg_opacity), ED_INT);
        ctrl_editbox(s, KT_BACK_IMAGE_SLIDESHOW, NO_SHORTCUT, 20,
                     HELPCTX(kitty_bgimage), conf_editbox_handler,
                     I(CONF_bg_slideshow), ED_INT);

        s = ctrl_getset(b, "Window/Appearance/Background", "bg_img_settings",
                        KT_BACK_IMAGE_IMAGE_SETTINGS);
        ctrl_filesel(s, KT_BACK_IMAGE_IMAGE_FILE_OR_RRGGBB, NO_SHORTCUT,
                     FILTER_ALL_FILES, false, KT_BACK_IMAGE_SELECT_BACKGROUND_IMAGE_FILE,
                     HELPCTX(kitty_bgimage),
                     conf_filesel_handler, I(CONF_bg_image_filename));
        ctrl_radiobuttons(s, KT_BACK_IMAGE_IMAGE_PLACEMENT, NO_SHORTCUT, 3,
                          HELPCTX(kitty_bgimage),
                          conf_radiobutton_handler, I(CONF_bg_image_style),
                          KT_BACK_IMAGE_TILE, NO_SHORTCUT, I(0),
                          KT_BACK_IMAGE_CENTER, NO_SHORTCUT, I(1),
                          KT_BACK_IMAGE_STRETCH, NO_SHORTCUT, I(2),
                          KT_BACK_IMAGE_ABSOLUTE_X_Y, NO_SHORTCUT, I(3),
                          KT_BACK_IMAGE_BLANK_BACK, NO_SHORTCUT, I(4),
                          KT_BACK_IMAGE_STRETCH_2, NO_SHORTCUT, I(5));
        ctrl_editbox(s, KT_BACK_IMAGE_ABSOLUTE_LEFT_X, NO_SHORTCUT, 20,
                     HELPCTX(kitty_bgimage), conf_editbox_handler,
                     I(CONF_bg_image_abs_x), ED_INT);
        ctrl_editbox(s, KT_BACK_IMAGE_ABSOLUTE_TOP_Y, NO_SHORTCUT, 20,
                     HELPCTX(kitty_bgimage), conf_editbox_handler,
                     I(CONF_bg_image_abs_y), ED_INT);
        ctrl_radiobuttons(s, KT_BACK_IMAGE_IMAGE_PLACEMENT_IS_RELATIVE, NO_SHORTCUT, 2,
                          HELPCTX(kitty_bgimage),
                          conf_radiobutton_handler, I(CONF_bg_image_abs_fixed),
                          KT_BACK_IMAGE_DESKTOP, NO_SHORTCUT, I(0),
                          KT_BACK_IMAGE_TERMINAL_WINDOW, NO_SHORTCUT, I(1));
    }
#endif
}

/* The Window/Charset translation, Window/Selection (Copy & Paste, with its
 * Character classes and Remote clipboard leaves) and Colours panels. */
void scb_panel_selection(struct controlbox *b)
{
    struct charclass_data *ccd;
    struct colour_data *cd;
    struct controlset *s;
    dlgcontrol *c;
    char *str;

    /*
     * The Window/Charset translation panel.
     */
    s = ctrl_getset(b, "Window/Charset translation", "trans",
                    KT_CHARSET_TRANSLATION_CHARACTER_SET_TRANSLATION);
    ctrl_combobox(s, KT_CHARSET_TRANSLATION_REMOTE_CHARACTER_SET,
                  'r', 100, HELPCTX(translation_codepage),
                  codepage_handler, P(NULL), P(NULL));

    s = ctrl_getset(b, "Window/Charset translation", "tweaks", NULL);
    ctrl_checkbox(s, KT_CHARSET_TRANSLATION_TREAT_CJK_AMBIGUOUS_CHARACTERS, 'w',
                  HELPCTX(translation_cjk_ambig_wide),
                  conf_checkbox_handler, I(CONF_cjk_ambig_wide));

    str = dupprintf(KT_CFG_LINEDRAW_TITLE_FMT, SCB_TITLE_APPNAME);
    s = ctrl_getset(b, "Window/Charset translation", "linedraw", str);
    sfree(str);
    ctrl_radiobuttons(
        s, KT_CHARSET_TRANSLATION_HANDLING_OF_LINE_DRAWING_CHARACTERS, NO_SHORTCUT,1,
        HELPCTX(translation_linedraw),
        conf_radiobutton_handler, I(CONF_vtmode),
        KT_CHARSET_TRANSLATION_USE_UNICODE_LINE_DRAWING_CODE,'u',I(VT_UNICODE),
        KT_CHARSET_TRANSLATION_POOR_MAN_S_LINE_DRAWING,'p',I(VT_POORMAN));
    ctrl_checkbox(s, KT_CHARSET_TRANSLATION_COPY_AND_PASTE_LINE_DRAWING,'d',
                  HELPCTX(selection_linedraw),
                  conf_checkbox_handler, I(CONF_rawcnp));
    ctrl_checkbox(s, KT_CHARSET_TRANSLATION_ENABLE_VT100_LINE_DRAWING_EVEN,'8',
                  HELPCTX(translation_utf8linedraw),
                  conf_checkbox_handler, I(CONF_utf8linedraw));

    /*
     * The Window/Selection panel.
     */
    s = ctrl_getset(b, "Window/Selection", "mouse",
                    KT_SELECTION_CONTROL_USE_OF_MOUSE);
    ctrl_checkbox(s, KT_SELECTION_SHIFT_OVERRIDES_APPLICATION_S_USE, 'p',
                  HELPCTX(selection_shiftdrag),
                  conf_checkbox_handler, I(CONF_mouse_override));
    ctrl_radiobuttons(s,
                      KT_SELECTION_DEFAULT_SELECTION_MODE_ALT_DRAG,
                      NO_SHORTCUT, 2,
                      HELPCTX(selection_rect),
                      conf_radiobutton_bool_handler,
                      I(CONF_rect_select),
                      KT_KEYBOARD_NORMAL, 'n', I(false),
                      KT_SELECTION_RECTANGULAR_BLOCK, 'r', I(true));

    s = ctrl_getset(b, "Window/Selection", "clipboards",
                    KT_SELECTION_ASSIGN_COPY_PASTE_ACTIONS);
    ctrl_checkbox(s, KT_CFG_AUTOCOPY_SELECTED_TEXT_TO
                  CLIPNAME_EXPLICIT_OBJECT,
                  NO_SHORTCUT, HELPCTX(selection_autocopy),
                  conf_checkbox_handler, I(CONF_mouseautocopy));
    clipboard_control(s, KT_CFG_MOUSE_PASTE_ACTION, NO_SHORTCUT, 60,
                      HELPCTX(selection_clipactions),
                      CONF_mousepaste, CONF_mousepaste_custom);
    clipboard_control(s, KT_CFG_CTRL_SHIFT_INS, NO_SHORTCUT, 60,
                      HELPCTX(selection_clipactions),
                      CONF_ctrlshiftins, CONF_ctrlshiftins_custom);
    clipboard_control(s, KT_CFG_CTRL_SHIFT_CV, NO_SHORTCUT, 60,
                      HELPCTX(selection_clipactions),
                      CONF_ctrlshiftcv, CONF_ctrlshiftcv_custom);
    s = ctrl_getset(b, "Window/Selection", "paste",
                    KT_SELECTION_CONTROL_PASTING_OF_TEXT);
    ctrl_checkbox(s, KT_SELECTION_PERMIT_CONTROL_CHARACTERS_IN_PASTED,
                  NO_SHORTCUT, HELPCTX(selection_pastectrl),
                  conf_checkbox_handler, I(CONF_paste_controls));

    /* "Formatting of copied characters" (the RTF box) is Windows-side, in
     * windows/config.c; the set is made here so it sits above the
     * clipboard-command group instead of at the foot of the panel. */
    ctrl_getset(b, "Window/Selection", "format",
                KT_COPY_FORMATTING_OF_COPIED_CHARACTERS);

    s = ctrl_getset(b, "Window/Selection", "runclipcmd",
                    KT_SELECTION_RUNNING_THE_CLIPBOARD);
    ctrl_checkbox(s, KT_SELECTION_CONFIRM_BEFORE_RUNNING_THE_CLIPBOARD,
                  NO_SHORTCUT, HELPCTX(kitty_clipcmd),
                  conf_checkbox_handler, I(CONF_runcmdconfirm));
    ctrl_checkbox(s, KT_SELECTION_SHOW_A_TRAY_NOTIFICATION_AFTER,
                  NO_SHORTCUT, HELPCTX(kitty_clipcmd),
                  conf_checkbox_handler, I(CONF_runcmdnotify));

    /*
     * The Window/Selection/Remote clipboard panels (KiTTY).
     *
     * These controls were all on Window/Selection until they outgrew it: the
     * clipboard-read work added eleven settings to a panel that already carried
     * the mouse buttons and the clipboard assignments, and the result ran off
     * the bottom of the dialog with half the labels truncated. Split three ways
     * by the question each answers - may the host do it, how much of it, and do
     * I get told - which also keeps every label short enough to read.
     */
    s = ctrl_getset(b, "Window/Selection/Remote clipboard", "policy",
                    KT_REMOTE_CLIPBOARD_PERMISSIONS);
#ifdef MOD_FAR2L
    /* KiTTY (far2l): let a remote far2l session read/write the local clipboard.
     * Triples (label, NO_SHORTCUT, I(val)) - 0.84 ctrl_radiobuttons needs the
     * per-button shortcut slot. */
    /* Deny/Allow/Ask rather than Disabled/Enabled: these grant a permission,
     * they do not switch a feature on. The SHARED_CLIPBOARD_* value names still
     * read DISABLED/ENABLED - stored values are unchanged either way. */
    ctrl_radiobuttons(s, KT_REMOTE_CLIPBOARD_FAR2L_SHARED_CLIPBOARD, NO_SHORTCUT, 3,
                      HELPCTX(kitty_osc52), conf_radiobutton_handler,
                      I(CONF_shared_clipboard),
                      KT_REMOTE_CLIPBOARD_DENY, NO_SHORTCUT, I(SHARED_CLIPBOARD_DISABLED),
                      KT_REMOTE_CLIPBOARD_ALLOW, NO_SHORTCUT, I(SHARED_CLIPBOARD_ENABLED),
                      KT_REMOTE_CLIPBOARD_ASK, NO_SHORTCUT, I(SHARED_CLIPBOARD_ASK));
#endif
    /* KiTTY (OSC 52): let the remote host put text on the local clipboard.
     * Same three-way shape as the far2l control above, on purpose. "Ask"
     * answers latch for the rest of the session. */
    ctrl_radiobuttons(s, KT_REMOTE_CLIPBOARD_WRITES_HOST_SETS_YOUR_CLIPBOARD,
                      NO_SHORTCUT, 3,
                      HELPCTX(kitty_osc52), conf_radiobutton_handler,
                      I(CONF_osc52_clipboard),
                      KT_REMOTE_CLIPBOARD_DENY, NO_SHORTCUT, I(OSC52_CLIPBOARD_DENY),
                      KT_REMOTE_CLIPBOARD_ALLOW, NO_SHORTCUT, I(OSC52_CLIPBOARD_ALLOW),
                      KT_REMOTE_CLIPBOARD_ASK, NO_SHORTCUT, I(OSC52_CLIPBOARD_ASK));
    /* KiTTY (OSC 52 read): the other direction - a host asking for the CONTENTS
     * of the clipboard, which we then send to it. Directly under the write
     * control, so both clipboard permissions are found in one place.
     *
     * TWO buttons where the write control has three, and the missing one is
     * "Allow", deliberately. A standing, unattended permission to read the
     * clipboard is the exact thing this control exists to prevent: a clipboard
     * holds a password for half a minute at a time, and the host chooses when it
     * asks. A read can still be allowed, or a bounded run of them, but only from
     * the dialog, with the request on screen, and it always expires. There is no
     * hidden kitty.ini key for it either. Documented so it does not read as
     * something that was forgotten. */
    ctrl_radiobuttons(s, KT_REMOTE_CLIPBOARD_READS_HOST_ASKS_FOR_YOUR,
                      NO_SHORTCUT, 2,
                      HELPCTX(kitty_osc52), conf_radiobutton_handler,
                      I(CONF_osc52_clipboard_read),
                      KT_REMOTE_CLIPBOARD_DENY, NO_SHORTCUT, I(OSC52_READ_DENY),
                      KT_REMOTE_CLIPBOARD_ASK, NO_SHORTCUT, I(OSC52_READ_ASK));
    /* Covers OSC 52, OSC 5522 AND far2l, which is why the label names none of
     * them: a rule with an exception in it is not the rule people remember. */
    ctrl_checkbox(s, KT_REMOTE_CLIPBOARD_ONLY_WHILE_THIS_WINDOW_HAS,
                  NO_SHORTCUT, HELPCTX(kitty_osc52),
                  conf_checkbox_handler, I(CONF_clipboard_require_focus));

    /*
     * The Window/Selection/Remote clipboard/Limits panel.
     */
    s = ctrl_getset(b, "Window/Selection/Remote clipboard/Limits", "size",
                    KT_LIMITS_ANY_PROTOCOL_OSC_52_OSC);
    /* One ceiling for OSC 52 and far2l both. Clamped in code (CLIP_MAX_MB_CAP):
     * lowering it only ever helps, but it must not be possible to type a number
     * here that turns a bounded denial of service into an unbounded one. */
    /* 18, not 25, on every box in this panel: these hold two to four digits, and
     * the 25% the other panels use was starving the labels - "Unanswered prompt
     * gives up after, in seconds (0 = never)" lost its unit off the right edge,
     * which is precisely the word that makes the number mean anything. */
    ctrl_editbox(s, KT_LIMITS_LARGEST_PAYLOAD_IN_MB, NO_SHORTCUT, 18,
                 HELPCTX(kitty_osc52), conf_editbox_handler,
                 I(CONF_clipboard_max_mb), ED_INT);
    /* A cap per second rather than a gap between writes: a gap would make the
     * FIRST write of a burst win, leaving a stale clipboard, which is backwards. */
    ctrl_editbox(s, KT_LIMITS_MOST_WRITES_PER_SECOND_0,
                 NO_SHORTCUT, 18, HELPCTX(kitty_osc52), conf_editbox_handler,
                 I(CONF_clipboard_writes_per_sec), ED_INT);

    /* The numbers behind the read dialog. They are settings because the values
     * shipped are guesses - no other terminal implements a hand-over rate limit
     * to copy from - and because someone who wants a five-minute grant instead of
     * ten should not have to argue with us about it. */
    s = ctrl_getset(b, "Window/Selection/Remote clipboard/Limits", "read",
                    KT_LIMITS_A_GRANTED_CLIPBOARD_READ);
    ctrl_editbox(s, KT_LIMITS_GRANT_OFFERED_IN_MINUTES, NO_SHORTCUT, 18,
                 HELPCTX(kitty_osc52), conf_editbox_handler,
                 I(CONF_osc52_read_minutes), ED_INT);
    ctrl_editbox(s, KT_LIMITS_GRANT_OFFERED_IN_REQUESTS, NO_SHORTCUT, 18,
                 HELPCTX(kitty_osc52), conf_editbox_handler,
                 I(CONF_osc52_read_requests), ED_INT);
    ctrl_editbox(s, KT_LIMITS_SHORTEST_GAP_BETWEEN_READS,
                 NO_SHORTCUT, 18, HELPCTX(kitty_osc52), conf_editbox_handler,
                 I(CONF_osc52_read_interval), ED_INT);
    ctrl_editbox(s, KT_LIMITS_MOST_READS_PER_WINDOW_0,
                 NO_SHORTCUT, 18, HELPCTX(kitty_osc52), conf_editbox_handler,
                 I(CONF_osc52_read_max), ED_INT);
    /* "in seconds" has to survive: without a unit the number is meaningless, and
     * it was the unit that fell off the edge. The "(0 = never)" the longer wording
     * carried is the natural reading of a zero timeout anyway. */
    ctrl_editbox(s, KT_LIMITS_UNANSWERED_PROMPT_EXPIRES_IN_SECONDS,
                 NO_SHORTCUT, 18, HELPCTX(kitty_osc52), conf_editbox_handler,
                 I(CONF_osc52_read_timeout), ED_INT);
    ctrl_editbox(s, KT_LIMITS_MOST_PROMPTS_PER_TEN_SECONDS, NO_SHORTCUT, 18,
                 HELPCTX(kitty_osc52), conf_editbox_handler,
                 I(CONF_osc52_read_dialogs), ED_INT);
    /* OSC 5522 paste events: a privilege an application arms with one mode
     * switch and a keypress then exercises without a dialog, so it gets a
     * clock; the mode itself outlives the clock, the reads then ask. */
    ctrl_text(s, KT_LIMITS_PASTE_EVENTS_TEXT, HELPCTX(kitty_osc52));
    ctrl_editbox(s, KT_LIMITS_PASTE_EVENTS_DISARM_MINUTES, NO_SHORTCUT, 18,
                 HELPCTX(kitty_osc52), conf_editbox_handler,
                 I(CONF_osc5522_paste_minutes), ED_INT);
    ctrl_editbox(s, KT_LIMITS_PASTE_TOKEN_SECONDS, NO_SHORTCUT, 18,
                 HELPCTX(kitty_osc52), conf_editbox_handler,
                 I(CONF_osc5522_paste_token_secs), ED_INT);

    /*
     * The Window/Selection/Remote clipboard/Notices panel.
     */
    s = ctrl_getset(b, "Window/Selection/Remote clipboard/Notices", "title",
                    KT_NOTICES_TITLE_BAR);
    /* The two markers answer different questions: permission says what COULD
     * happen, activity says what DID. Icons rather than words - the title bar is
     * shared with the connection name - with the standing one bracketed at the end
     * and the transient one bare at the front. */
    ctrl_checkbox(s, KT_NOTICES_MARK_WHILE_A_PERMISSION,
                  NO_SHORTCUT, HELPCTX(kitty_osc52),
                  conf_checkbox_handler, I(CONF_osc52_title_mark));
    ctrl_checkbox(s, KT_NOTICES_ALSO_MARK_A_STANDING_ALLOW,
                  NO_SHORTCUT, HELPCTX(kitty_osc52),
                  conf_checkbox_handler, I(CONF_clipboard_mark_always));
    ctrl_checkbox(s, KT_NOTICES_MARK_WHEN_THE_HOST_ACTUALLY,
                  NO_SHORTCUT, HELPCTX(kitty_osc52),
                  conf_checkbox_handler, I(CONF_clipboard_activity_mark));
    ctrl_editbox(s, KT_NOTICES_THAT_MARKER_STAYS_UP,
                 NO_SHORTCUT, 18, HELPCTX(kitty_osc52), conf_editbox_handler,
                 I(CONF_clipboard_activity_secs), ED_INT);
    /* Windows 11 build 22000+ only; silently does nothing on Windows 10, which
     * is why the title marker above has to carry the meaning by itself. */
    ctrl_checkbox(s, KT_NOTICES_TINT_THE_TITLE_BAR,
                  NO_SHORTCUT, HELPCTX(kitty_osc52),
                  conf_checkbox_handler, I(CONF_osc52_colour_frame));

    s = ctrl_getset(b, "Window/Selection/Remote clipboard/Notices", "tray",
                    KT_NOTICES_NOTIFICATION_AREA);
    /* One switch for every remote-clipboard balloon - permission granted or
     * expired, request refused, payload too large - across all three protocols.
     * The balloons are rate-limited and say so, and clicking one opens the Event
     * Log, which is where the events actually all are. */
    ctrl_checkbox(s, KT_NOTICES_SHOW_TRAY_NOTIFICATIONS_FOR_CLIPBOARD,
                  NO_SHORTCUT, HELPCTX(kitty_osc52),
                  conf_checkbox_handler, I(CONF_clipboard_notify));

    /*
     * The Window/Selection/Copy panel ("Character classes" in the tree): the
     * class list alone - the RTF box moved up to the Copy & Paste panel.
     */
    s = ctrl_getset(b, "Window/Selection/Copy", "charclass",
                    KT_COPY_CLASSES_OF_CHARACTER_THAT_GROUP);
    ctrl_text(s, KT_COPY_CLASSES_EXPLAIN, HELPCTX(copy_charclasses));
    ccd = (struct charclass_data *)
        ctrl_alloc(b, sizeof(struct charclass_data));
    ccd->listbox = ctrl_listbox(s, KT_COPY_CHARACTER_CLASSES, 'e',
                                HELPCTX(copy_charclasses),
                                charclass_handler, P(ccd));
    ccd->listbox->listbox.multisel = 1;
    ccd->listbox->listbox.ncols = 4;
    ccd->listbox->listbox.percentages = snewn(4, int);
    ccd->listbox->listbox.percentages[0] = 15;
    ccd->listbox->listbox.percentages[1] = 25;
    ccd->listbox->listbox.percentages[2] = 20;
    ccd->listbox->listbox.percentages[3] = 40;
    ctrl_columns(s, 2, 67, 33);
    ccd->editbox = ctrl_editbox(s, KT_COPY_SET_TO_CLASS, 't', 50,
                                HELPCTX(copy_charclasses),
                                charclass_handler, P(ccd), P(NULL));
    ccd->editbox->column = 0;
    ccd->button = ctrl_pushbutton(s, KT_COPY_SET, 's',
                                  HELPCTX(copy_charclasses),
                                  charclass_handler, P(ccd));
    ccd->button->column = 1;
    ctrl_columns(s, 1, 100);

    /*
     * The Window/Colours panel.
     */
    s = ctrl_getset(b, "Window/Appearance/Colours", "general",
                    KT_COLOURS_GENERAL_OPTIONS_FOR_COLOUR_USAGE);
    ctrl_checkbox(s, KT_COLOURS_ALLOW_TERMINAL_TO_SPECIFY_ANSI, 'i',
                  HELPCTX(colours_ansi),
                  conf_checkbox_handler, I(CONF_ansi_colour));
    ctrl_checkbox(s, KT_COLOURS_ALLOW_TERMINAL_TO_USE_XTERM, '2',
                  HELPCTX(colours_xterm256), conf_checkbox_handler,
                  I(CONF_xterm_256_colour));
    ctrl_checkbox(s, KT_COLOURS_ALLOW_TERMINAL_TO_USE_24, '4',
                  HELPCTX(colours_truecolour), conf_checkbox_handler,
                  I(CONF_true_colour));
    ctrl_radiobuttons(s, KT_COLOURS_INDICATE_BOLDED_TEXT_BY_CHANGING, 'b', 3,
                      HELPCTX(colours_bold),
                      conf_radiobutton_handler, I(CONF_bold_style),
                      KT_COLOURS_THE_FONT, I(BOLD_STYLE_FONT),
                      KT_COLOURS_THE_COLOUR, I(BOLD_STYLE_COLOUR),
                      KT_KEYBOARD_BOTH, I(BOLD_STYLE_FONT | BOLD_STYLE_COLOUR));
#ifdef MOD_TUTTYCOLOR
    /* KiTTY (TuTTY): extra colour toggles. The palette gains "Underlined Text",
     * "Selected Text" and "Selected Background" slots (editable in the list
     * below). under_colour colours underlined text; sel_colour rendering is
     * deferred (slots exist + are editable). */
    if (!GetPuttyFlag()) {
        ctrl_checkbox(s, KT_COLOURS_COLOUR_UNDERLINED_TEXT, NO_SHORTCUT,
                      HELPCTX(kitty_colour_extra), kitty_checkbox_int_handler,
                      I(CONF_under_colour));
        ctrl_checkbox(s, KT_COLOURS_COLOUR_SELECTED_TEXT, NO_SHORTCUT,
                      HELPCTX(kitty_colour_extra), kitty_checkbox_int_handler,
                      I(CONF_sel_colour));
    }
#endif

    /* KiTTY: the adjust block is its own leaf under Colours - the general
     * switches and the palette editor are different errands, and together
     * they made one tall panel. */
    s = ctrl_getset(b, "Window/Appearance/Colours/Precise colours", "adjust",
                    KT_COLOURS_PRECISE_COLOURS);
    ctrl_text(s, KT_COLOURS_SELECT_A_COLOUR,
              HELPCTX(colours_config));
    ctrl_columns(s, 2, 67, 33);
    cd = (struct colour_data *)ctrl_alloc(b, sizeof(struct colour_data));
    cd->listbox = ctrl_listbox(s, KT_COLOURS_SELECT_A_COLOUR_TO_ADJUST, 'u',
                               HELPCTX(colours_config), colour_handler, P(cd));
    cd->listbox->column = 0;
    cd->listbox->listbox.height = 7;
    c = ctrl_text(s, KT_COLOURS_RGB_VALUE, HELPCTX(colours_config));
    c->column = 1;
    cd->redit = ctrl_editbox(s, KT_COLOURS_RED, 'r', 50, HELPCTX(colours_config),
                             colour_handler, P(cd), P(NULL));
    cd->redit->column = 1;
    cd->gedit = ctrl_editbox(s, KT_COLOURS_GREEN, 'n', 50, HELPCTX(colours_config),
                             colour_handler, P(cd), P(NULL));
    cd->gedit->column = 1;
    cd->bedit = ctrl_editbox(s, KT_COLOURS_BLUE, 'e', 50, HELPCTX(colours_config),
                             colour_handler, P(cd), P(NULL));
    cd->bedit->column = 1;
    cd->button = ctrl_pushbutton(s, KT_COLOURS_MODIFY, 'm', HELPCTX(colours_config),
                                 colour_handler, P(cd));
    cd->button->column = 1;
    ctrl_columns(s, 1, 100);
}

/* The Connection panel and Connection/Login sub-panel (network utilities
 * only: the whole body is guarded by protocol >= 0). */
void scb_panel_connection(struct controlbox *b, bool midsession, int protocol)
{
    struct environ_data *ed;
    struct controlset *s;
    dlgcontrol *c;

    /*
     * The Connection panel. This doesn't show up if we're in a
     * non-network utility such as pterm. We tell this by being
     * passed a protocol < 0.
     */
    if (protocol >= 0) {
        ctrl_settitle(b, "Connection", KT_CONNECTION_OPTIONS_CONTROLLING_THE_CONNECTION);

        s = ctrl_getset(b, "Connection", "keepalive",
                        KT_CONNECTION_SENDING_OF_NULL_PACKETS);
        ctrl_editbox(s, KT_CONNECTION_SECONDS_BETWEEN_KEEPALIVES_0, 'k', 20,
                     HELPCTX(connection_keepalive),
                     conf_editbox_handler, I(CONF_ping_interval), ED_INT);
        if (!GetPuttyFlag()) {
            ctrl_editbox(s, KT_CONNECTION_ANTI_IDLE_STRING, NO_SHORTCUT, 50,
                         HELPCTX(kitty_antiidle), conf_editbox_handler,
                         I(CONF_antiidle), ED_STR);
        }

        if (!midsession) {
            s = ctrl_getset(b, "Connection", "tcp",
                            KT_CONNECTION_LOW_LEVEL_TCP_CONNECTION_OPTIONS);
            ctrl_checkbox(s, KT_CONNECTION_DISABLE_NAGLE_S_ALGORITHM_TCP,
                          'n', HELPCTX(connection_nodelay),
                          conf_checkbox_handler,
                          I(CONF_tcp_nodelay));
            ctrl_checkbox(s, KT_CONNECTION_ENABLE_TCP_KEEPALIVES_SO_KEEPALIVE,
                          'p', HELPCTX(connection_tcpkeepalive),
                          conf_checkbox_handler,
                          I(CONF_tcp_keepalives));
#ifndef NO_IPV6
            s = ctrl_getset(b, "Connection", "ipversion",
                            KT_CONNECTION_INTERNET_PROTOCOL_VERSION);
            ctrl_radiobuttons(s, NULL, NO_SHORTCUT, 3,
                              HELPCTX(connection_ipversion),
                              conf_radiobutton_handler,
                              I(CONF_addressfamily),
                              KT_TERMINAL_AUTO, 'u', I(ADDRTYPE_UNSPEC),
                              KT_CONNECTION_IPV4, '4', I(ADDRTYPE_IPV4),
                              KT_CONNECTION_IPV6, '6', I(ADDRTYPE_IPV6));
#endif

#ifdef MOD_RECONNECT
            /* KiTTY auto-reconnect (INT keys -> kitty_checkbox_int_handler;
             * engine wired separately). Gated by GetAutoreconnectFlag(). */
            if (!GetPuttyFlag()) {
                s = ctrl_getset(b, "Connection", "reconnect",
                                KT_CONNECTION_RECONNECT_OPTIONS);
                if (GetAutoreconnectFlag()) {
                    ctrl_checkbox(s, KT_CONNECTION_ATTEMPT_TO_RECONNECT_ON_SYSTEM,
                                  NO_SHORTCUT, HELPCTX(kitty_reconnect),
                                  kitty_checkbox_int_handler,
                                  I(CONF_wakeup_reconnect));
                    ctrl_checkbox(s, KT_CONNECTION_ATTEMPT_TO_RECONNECT_ON_CONNECTION,
                                  NO_SHORTCUT, HELPCTX(kitty_reconnect),
                                  kitty_checkbox_int_handler,
                                  I(CONF_failure_reconnect));
                } else {
                    /* The master switch is off (it is on by default): one
                     * short line saying so and where it is, rather than
                     * leaving the group out and the feature unfindable.
                     * With the switch on, nothing - the boxes speak for
                     * themselves. */
                    ctrl_text(s, KT_CONNECTION_RECONNECT_GLOBAL_OFF,
                              HELPCTX(kitty_reconnect));
                }
            }
#endif

            /* One block for the three fields that prepare a connection
             * before it authenticates - three single-field frames each
             * repeating its field's own words were furniture, not
             * structure. */
            s = ctrl_getset(b, "Connection", "authopts",
                            KT_CONNECTION_CONNECTION_PREPARATION);
            {
                const char *label = backend_vt_from_proto(PROT_SSH) ?
                    KT_CFG_LOGHOST_SSH :
                    KT_CFG_LOGHOST;
                ctrl_editbox(s, label, 'm', 100,
                             HELPCTX(connection_loghost),
                             conf_editbox_handler, I(CONF_loghost), ED_STR);
            }
            ctrl_editbox(s, KT_CONNECTION_COMMAND_TO_RUN_BEFORE_CONNECTION, 'b', 100,
                         HELPCTX(connection_pre_hook),
                         conf_editbox_handler, I(CONF_pre_connect_command), ED_STR);

#ifdef MOD_PORTKNOCKING
            /* KiTTY: port-knocking sequence. Backend = kitty_port_knock() /
             * ManagePortKnocking(), fired from start_backend() before connect. */
            if (!GetPuttyFlag()) {
                ctrl_editbox(s, KT_CONNECTION_PORT_KNOCKING_SEQUENCE, NO_SHORTCUT, 100,
                             HELPCTX(kitty_knocking), conf_editbox_handler,
                             I(CONF_portknockingoptions), ED_STR);
                ctrl_text(s, KT_CONNECTION_A_COMMA_SEPARATED_LIST, HELPCTX(kitty_knocking));
                ctrl_text(s, KT_CONNECTION_EXAMPLE_2001_TCP_1_S,
                          HELPCTX(kitty_knocking));
            }
#endif
        }

        /*
         * A sub-panel Connection/Login, containing options that
         * decide on data to send to the server.
         */
        if (!midsession) {
            ctrl_settitle(b, "Connection/Login", KT_LOGIN_OPTIONS_CONTROLLING_THE_LOGIN);

            s = ctrl_getset(b, "Connection/Login", "login",
                            KT_DATA_LOGIN_DETAILS);
            ctrl_editbox(s, KT_DATA_AUTO_LOGIN_USERNAME, 'u', 50,
                         HELPCTX(connection_username),
                         conf_editbox_handler, I(CONF_username), ED_STR);
            /* No "Alternate host name (HostAlt)" box here. HostAlt never had a
             * reader: KiTTY wrote the host into it when Inherit New Session
             * cleared CONF_host, and the only code that ever read it back was
             * commented out upstream. This port then gave it an editbox, so
             * users could type into a field that did nothing. The job it was
             * meant to do is now done properly by CONF_host_inherited
             * (cyd01/KiTTY#519). The conf key itself is kept so existing saved
             * sessions still load without complaint. */
            {
                /* We assume the local username is sufficiently stable
                 * to include on the dialog box. */
                char *user = get_username();
                /* KiTTY: in demo-screenshot mode a NEUTRAL name - the real
                 * account name of whoever renders the documentation shots
                 * must not end up in a published image. */
                extern Filename *dialog_box_demo_screenshot_filename;
                char *userlabel = dupprintf(KT_CFG_USE_SYSTEM_USERNAME,
                                            dialog_box_demo_screenshot_filename
                                            ? "user"
                                            : (user ? user : ""));
                sfree(user);
                ctrl_radiobuttons(s, KT_DATA_WHEN_USERNAME_IS_NOT_SPECIFIED, 'n', 4,
                                  HELPCTX(connection_username_from_env),
                                  conf_radiobutton_bool_handler,
                                  I(CONF_username_from_env),
                                  KT_DATA_PROMPT, I(false),
                                  userlabel, I(true));
                sfree(userlabel);
            }

            /* KiTTY auto-command: sent automatically after login. */
            if (!GetPuttyFlag()) {
                dlgcontrol *cpw;
                cpw = ctrl_editbox(s, KT_DATA_AUTO_LOGIN_PASSWORD, NO_SHORTCUT, 50,
                                   HELPCTX(kitty_autologin), kitty_autopw_handler,
                                   I(CONF_password), ED_STR);
                cpw->editbox.password = true;
                g_autopw_ctrl = cpw;
                ctrl_checkbox(s, KT_DATA_SHOW_PASSWORD, NO_SHORTCUT,
                              HELPCTX(kitty_autologin), kitty_showpw_handler,
                              P(&g_autopw_ctrl));
                ctrl_editbox(s, KT_DATA_AUTO_COMMAND_AFTER_LOGIN, NO_SHORTCUT,
                             50, HELPCTX(kitty_autologin),
                             conf_editbox_handler,
                             I(CONF_autocommand), ED_STR);
                /*
                 * The login script, readable and editable.
                 *
                 * It used to be a single-line box bound straight to the STORED
                 * value, which meant it showed protected gibberish and - worse -
                 * writing in it produced a value no decoder could open, so the
                 * script silently stopped working. It now shows the script the
                 * way it is written in a script file: one entry per line,
                 * alternating what to wait for and what to send.
                 *
                 * The "Login script file:" picker that used to sit above this is
                 * GONE. It was bound to CONF_scriptfile, which belongs to the
                 * rutty scripting on Session > Scripting - so choosing a file
                 * here silently changed THAT setting and did nothing whatever for
                 * the login script, which nothing ever read it for. Load a file
                 * with -loginscript; edit it here afterwards.
                 */
                g_loginscript_ctrl =
                    ctrl_editbox_multiline(s, KT_DATA_LOGIN_SCRIPT_WAIT,
                                           NO_SHORTCUT, 8, false,
                                           HELPCTX(kitty_loginscript),
                                           kitty_loginscript_handler,
                                           I(CONF_scriptfilecontent), ED_STR);
                /* Replaces classic's picker, which read a file into the session
                 * and cleared itself. This only fills the box - what gets saved
                 * is what you can see and edit above. */
                ctrl_pushbutton(s, KT_DATA_LOAD_SCRIPT_FROM_FILE, NO_SHORTCUT,
                                HELPCTX(kitty_loginscript),
                                kitty_loginscript_load_handler, I(0));
            }

            /* KiTTY: terminal identification and the environment block each
             * get a leaf under Data - the login settings alone fill the
             * parent panel. */
            ctrl_settitle(b, "Connection/Login/Terminal details",
                          KT_DATA_TERMINAL_DETAILS_SENT);
            s = ctrl_getset(b, "Connection/Login/Terminal details", "term",
                            KT_DATA_TERMINAL_DETAILS);
            ctrl_editbox(s, KT_DATA_TERMINAL_TYPE_STRING, 't', 50,
                         HELPCTX(connection_termtype),
                         conf_editbox_handler, I(CONF_termtype), ED_STR);
            ctrl_editbox(s, KT_DATA_TERMINAL_SPEEDS, 's', 50,
                         HELPCTX(connection_termspeed),
                         conf_editbox_handler, I(CONF_termspeed), ED_STR);

            ctrl_settitle(b, "Connection/Login/Environment",
                          KT_DATA_ENVIRONMENT_VARIABLES_SENT);
            s = ctrl_getset(b, "Connection/Login/Environment", "env",
                            KT_DATA_ENVIRONMENT_VARIABLES);
            ctrl_columns(s, 2, 80, 20);
            ed = (struct environ_data *)
                ctrl_alloc(b, sizeof(struct environ_data));
            ed->varbox = ctrl_editbox(s, KT_DATA_VARIABLE, 'v', 60,
                                      HELPCTX(telnet_environ),
                                      environ_handler, P(ed), P(NULL));
            ed->varbox->column = 0;
            ed->valbox = ctrl_editbox(s, KT_DATA_VALUE, 'l', 60,
                                      HELPCTX(telnet_environ),
                                      environ_handler, P(ed), P(NULL));
            ed->valbox->column = 0;
            ed->addbutton = ctrl_pushbutton(s, KT_DATA_ADD, 'd',
                                            HELPCTX(telnet_environ),
                                            environ_handler, P(ed));
            ed->addbutton->column = 1;
            ed->rembutton = ctrl_pushbutton(s, KT_DATA_REMOVE, 'r',
                                            HELPCTX(telnet_environ),
                                            environ_handler, P(ed));
            ed->rembutton->column = 1;
            ctrl_columns(s, 1, 100);
            /* A header row over the list's two columns, in the columns'
             * own proportions - the listbox control has no header of its
             * own. */
            ctrl_columns(s, 2, 30, 70);
            c = ctrl_text(s, KT_DATA_HEADER_NAME, HELPCTX(telnet_environ));
            c->column = 0;
            c = ctrl_text(s, KT_DATA_HEADER_CONTENT, HELPCTX(telnet_environ));
            c->column = 1;
            ctrl_columns(s, 1, 100);
            ed->listbox = ctrl_listbox(s, NULL, NO_SHORTCUT,
                                       HELPCTX(telnet_environ),
                                       environ_handler, P(ed));
            /* On its own leaf the list is the panel's point: give it real
             * room rather than the three rows it had in Data's corner. */
            ed->listbox->listbox.height = 9;
            ed->listbox->listbox.ncols = 2;
            ed->listbox->listbox.percentages = snewn(2, int);
            ed->listbox->listbox.percentages[0] = 30;
            ed->listbox->listbox.percentages[1] = 70;
        }

    }
}

/* The Connection/Proxy panel (not available mid-session). */
void scb_panel_proxy(struct controlbox *b, bool midsession)
{
    struct controlset *s;
    dlgcontrol *c;

    if (!midsession) {
        /*
         * The Connection/Proxy panel.
         */
        ctrl_settitle(b, "Connection/Proxy",
                      KT_PROXY_OPTIONS_CONTROLLING_PROXY_USAGE);

        /* KiTTY: the named-proxy EDITOR first, because it is where the reusable
         * definitions come from, and the two things below it both consume one.
         * (It used to sit at the foot of the panel, under the session's own
         * fields.) */
        if (!GetPuttyFlag() && kitty_proxy_editor_available()) {
            s = ctrl_getset(b, "Connection/Proxy", "editnamed",
                            KT_PROXY_NAMED_PROXIES_PROXY_TEMPLATES);
            ctrl_pushbutton(s, KT_PROXY_EDIT_NAMED_PROXIES, NO_SHORTCUT,
                            HELPCTX(kitty_proxy_buttons), kitty_proxyedit_handler, P(NULL));
        }
        s = ctrl_getset(b, "Connection/Proxy", "basics",
                        KT_PROXY_THIS_SESSION_S_OWN_PROXY);
        /* KiTTY: the notice used to be a three-line paragraph HERE, added
         * only while the mode was armed. Two things were wrong with it and both
         * came from the same mistake - it was built at panel-construction time:
         *  - it could not change, so switching the mode off left it insisting
         *    the mode was on;
         *  - the three extra lines pushed the workplace box's own button off the
         *    bottom of the config box, behind Open/Cancel.
         * The live state now sits in the workplace box below, on a text control
         * that is relabelled as the state moves. Nothing is said twice. */
        c = ctrl_droplist(s, KT_PROXY_PROXY_TYPE, 't', 70,
                          HELPCTX(proxy_type), proxy_type_handler, I(0));
        ctrl_columns(s, 2, 80, 20);
        c = ctrl_editbox(s, KT_PROXY_PROXY_HOSTNAME, 'y', 100,
                         HELPCTX(proxy_main),
                         conf_editbox_handler,
                         I(CONF_proxy_host), ED_STR);
        c->column = 0;
        c = ctrl_editbox(s, KT_PROXY_PORT, 'p', 100,
                         HELPCTX(proxy_main),
                         conf_editbox_handler,
                         I(CONF_proxy_port),
                         ED_INT);
        c->column = 1;
        ctrl_columns(s, 1, 100);
        ctrl_editbox(s, KT_PROXY_EXCLUDE_HOSTS_IPS, 'e', 100,
                     HELPCTX(proxy_exclude),
                     conf_editbox_handler,
                     I(CONF_proxy_exclude_list), ED_STR);
        ctrl_checkbox(s, KT_PROXY_CONSIDER_PROXYING_LOCAL_HOST_CONNECTIONS, 'x',
                      HELPCTX(proxy_exclude),
                      conf_checkbox_handler,
                      I(CONF_even_proxy_localhost));
        ctrl_radiobuttons(s, KT_PROXY_DO_DNS_NAME_LOOKUP, 'd', 3,
                          HELPCTX(proxy_dns),
                          conf_radiobutton_handler,
                          I(CONF_proxy_dns),
                          KT_PROXY_NO, I(FORCE_OFF),
                          KT_TERMINAL_AUTO, I(AUTO),
                          KT_PROXY_YES, I(FORCE_ON));
        ctrl_editbox(s, KT_PROXY_USERNAME, 'u', 60,
                     HELPCTX(proxy_auth),
                     conf_editbox_handler,
                     I(CONF_proxy_username), ED_STR);
        c = ctrl_editbox(s, KT_PROXY_PASSWORD, 'w', 60,
                         HELPCTX(proxy_auth),
                         kitty_proxypw_handler,
                         I(CONF_proxy_password), ED_STR);
        c->editbox.password = true;
        g_proxypw_ctrl = c;
        ctrl_checkbox(s, KT_DATA_SHOW_PASSWORD, NO_SHORTCUT,
                      HELPCTX(proxy_auth), kitty_showpw_handler,
                      P(&g_proxypw_ctrl));
        ctrl_editbox(s, KT_PROXY_COMMAND_TO_SEND_TO_PROXY, 'm', 100,
                     HELPCTX(proxy_command),
                     conf_editbox_handler,
                     I(CONF_proxy_telnet_command), ED_STR);

        ctrl_radiobuttons(s, KT_PROXY_PRINT_PROXY_DIAGNOSTICS, 'r', 5,
                          HELPCTX(proxy_logging),
                          conf_radiobutton_handler,
                          I(CONF_proxy_log_to_term),
                          KT_PROXY_NO, I(FORCE_OFF),
                          KT_PROXY_YES, I(FORCE_ON),
                          KT_PROXY_ONLY_UNTIL_SESSION_STARTS, I(AUTO));
        /* KiTTY: the pre-set loader, after the session's own fields. It
         * adopts a template into this session, so it is not one of those
         * fields - it is something the user reaches for on purpose. */
        if (!GetPuttyFlag() && kitty_has_proxy_definitions()) {
            struct pxload_data *pd = (struct pxload_data *)
                ctrl_alloc(b, sizeof(struct pxload_data));
            memset(pd, 0, sizeof(*pd));
            s = ctrl_getset(b, "Connection/Proxy", "loadnamed",
                            KT_PROXY_NAMED_PROXY_PRE_SETS);
            ctrl_columns(s, 2, 75, 25);
            pd->list = ctrl_droplist(s, NULL, NO_SHORTCUT, 100,
                                     HELPCTX(kitty_proxy_buttons),
                                     kitty_pxload_inline_handler, P(pd));
            pd->list->column = 0;
            pd->button = ctrl_pushbutton(s, KT_PROXY_LOAD, NO_SHORTCUT,
                                         HELPCTX(kitty_proxy_buttons),
                                         kitty_pxload_inline_handler, P(pd));
            pd->button->column = 1;
            ctrl_columns(s, 1, 100);
            kitty_pxload_active = pd;
        }
        /* KiTTY: workplace proxy mode, LAST on the panel and in a box of its own.
         * Everything above it is this session's configuration; this is not. It is
         * an application-wide switch that overrides every session at once, and
         * putting it between the preset loader and the session's own fields (where
         * it first sat) read as though it were part of them. */
        /* The leaf is ALWAYS in the tree now. Without a named proxy the
         * controls grey out and the info line under the state says so -
         * a leaf that comes and goes with the proxy count read as a bug,
         * and gave no hint where the feature had gone. */
        /* An Application leaf, so [ConfigBox] applicationsettings=no
         * takes it away with the rest of that tab. */
        if (!GetPuttyFlag() && GetConfigBoxApplicationSettingsFlag()) {
            struct wpmode_data *wd = (struct wpmode_data *)
                ctrl_alloc(b, sizeof(struct wpmode_data));
            memset(wd, 0, sizeof(*wd));
            kitty_wpmode_active = wd;   /* what the tray-change poll repaints */
            /* KiTTY: its OWN leaf under Proxy. It is an application-wide
             * switch, not a session setting, and sharing the Proxy panel
             * both crowded the panel and made it read like one. */
            ctrl_settitle(b, "Application/Workplace Proxy",
                          KT_WORKPLACE_PROXY_WORKPLACE_PROXY_MODE_APPLICATION_WIDE);
            s = ctrl_getset(b, "Application/Workplace Proxy", "workplace",
                            KITTY_WORKPLACE_BOX_TITLE);
            /* The "not a setting of this session" lead is gone: on the
             * Application tab that is what EVERY panel is, so the sentence
             * said nothing here any more. */
            /* The live state, drawn BOLD RED while the mode is on so it is seen
             * rather than read: this is the one line on the panel that says
             * something is overriding every session right now.
             *
             * WARNING: Relabelled in place (see dlg_label_change), so the two wordings
             * must occupy the same number of lines - the control's height was
             * fixed when the panel was built. Both are one line at this width.
             *
             * WARNING: It says the connection WILL USE the workplace proxy; it does
             * not say "these settings are ignored". That would not be true in
             * every case - a proxy Host naming a saved session still drags that
             * session's configuration in - and the first person to hit a chained
             * case would find the notice lying to them. */
            wd->state = ctrl_text(s, KITTY_WORKPLACE_STATE_OFF, HELPCTX(kitty_workplace));
            ctrl_text(s, KT_WORKPLACE_PROXY_WHILE_IT_IS_ON_EVERY,
                      HELPCTX(kitty_workplace));
            /* Relabelled in place like the state line above, so both wordings
             * must stay one line at panel width. */
            wd->noproxy = ctrl_text(s, kitty_has_proxy_definitions() ? " " :
                                    KT_WORKPLACE_PROXY_NEEDS_NAMED,
                                    HELPCTX(kitty_workplace));
            /* Label kept to the length of "Named proxy settings:" above: at 60%
             * droplist width the label gets the other 40%, and "Proxy for every
             * connection:" was clipped to "Proxy for every" - the same squeeze
             * that once clipped the "Load into this window" button. */
            wd->list = ctrl_droplist(s, KT_WORKPLACE_PROXY_PROXY_FOR_EVERYTHING, NO_SHORTCUT,
                                     60, HELPCTX(kitty_workplace),
                                     kitty_wpmode_handler, P(wd));
            wd->hours = ctrl_droplist(s, KT_WORKPLACE_PROXY_SWITCH_OFF_AFTER, NO_SHORTCUT,
                                      60, HELPCTX(kitty_workplace),
                                      kitty_wpmode_handler, P(wd));
            wd->button = ctrl_pushbutton(s, KT_WORKPLACE_PROXY_SWITCH, NO_SHORTCUT,
                                         HELPCTX(kitty_workplace),
                                         kitty_wpmode_handler, P(wd));
        }
    }
}

/*
 * One protocol and one port per transfer tool (kscp, WinSCP, FileZilla).
 *
 * The port field is per session and empty by default; empty means the
 * standard port of the protocol chosen beside it, which for SFTP/SCP is the
 * session's own port and for kscp the global kscp port before that.
 * kitty_xfer_default_port() is the one place that rule lives, and the field
 * shows its answer as a CUE BANNER, so an empty box always states the port
 * actually in use. A cue banner needs a Unicode edit and comctl32 v6:
 * Windows before Vista show nothing there, and the port still applies.
 *
 * The banner follows a click on the radios because the radio handler
 * refreshes the port control, whose EVENT_REFRESH is the only place that
 * writes it.
 */
#define XFER_TOOL_KSCP      0
#define XFER_TOOL_WINSCP    1
#define XFER_TOOL_FILEZILLA 2

static int xfer_tool_protocol_key(int tool)
{
    switch (tool) {
      case XFER_TOOL_KSCP:   return CONF_kscp_protocol;
      case XFER_TOOL_WINSCP: return CONF_winscpprot;
      default:               return CONF_filezilla_protocol;
    }
}

/* The EDIT window of a labelled edit box. The control reserves ids for its
 * label too, and the first of them is the label, so the edit is a couple of
 * ids along - found by class name rather than by counting. */
static HWND xfer_port_edit_hwnd(dlgcontrol *ctrl, dlgparam *dlg)
{
    HWND h = kitty_dlg_ctrl_hwnd(dlg, ctrl), parent;
    char cls[16];
    int base, k;

    if (!h)
        return NULL;
    parent = GetParent(h);
    base = GetDlgCtrlID(h);
    for (k = 0; k < 3; k++) {
        HWND c = GetDlgItem(parent, base + k);
        if (c && GetClassNameA(c, cls, sizeof(cls)) && !stricmp(cls, "Edit"))
            return c;
    }
    return NULL;
}

static void xfer_port_hint(dlgcontrol *ctrl, dlgparam *dlg, Conf *conf, int tool)
{
    /* kitty/kitty.h states the rules; kitty_xfer.c applies them. Declared here
     * rather than by including kitty.h, which this file does not. */
    HWND h = xfer_port_edit_hwnd(ctrl, dlg);
    wchar_t hint[24];
    char num[24];
    LONG_PTR style;
    int port;

    if (!h)
        return;
    /* A port is a number: let the control itself refuse the rest. ES_NUMBER
     * works on every Windows this runs on, XP included, and needs no message
     * handling of ours. Five digits is the whole of 1..65535. Setting it here
     * costs nothing when it is already set. */
    style = GetWindowLongPtr(h, GWL_STYLE);
    if (!(style & ES_NUMBER))
        SetWindowLongPtr(h, GWL_STYLE, style | ES_NUMBER);
    SendMessage(h, EM_SETLIMITTEXT, 5, 0);
    port = kitty_xfer_default_port(conf, tool,
                                   conf_get_int(conf, xfer_tool_protocol_key(tool)));
    if (port > 0)
        sprintf(num, "%d", port);
    else
        num[0] = '\0';
    if (MultiByteToWideChar(CP_ACP, 0, num, -1, hint, lenof(hint)))
        SendMessageW(h, EM_SETCUEBANNER, TRUE, (LPARAM)hint);
}

/* context: the port's CONF_ key. context2: the tool. */
static void xfer_port_handler(dlgcontrol *ctrl, dlgparam *dlg,
                              void *data, int event)
{
    Conf *conf = (Conf *)data;
    int key = ctrl->context.i;

    if (event == EVENT_REFRESH) {
        /* This set fires a VALCHANGE that stores the same string back; the
         * hint is written afterwards so the reentry cannot undo it. */
        dlg_editbox_set(ctrl, dlg, conf_get_str(conf, key));
        xfer_port_hint(ctrl, dlg, conf, ctrl->context2.i);
    } else if (event == EVENT_VALCHANGE) {
        /* ES_NUMBER on the control is what keeps the field numeric. This is
         * the safety net for anything that reaches the text another way: what
         * is stored is digits only, at most five, and the control is left
         * alone so the caret does not move under the typist. */
        char *s = dlg_editbox_get(ctrl, dlg);
        char clean[8];
        int i, n = 0;

        for (i = 0; s[i] && n < 5; i++)
            if (s[i] >= '0' && s[i] <= '9')
                clean[n++] = s[i];
        clean[n] = '\0';
        conf_set_str(conf, key, clean);
        sfree(s);
    }
}

/* The tool's protocol radios. context2 holds the port control beside them,
 * so the hint follows the choice. */
static void kitty_xfer_protocol_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                        void *data, int event)
{
    conf_radiobutton_handler(ctrl, dlg, data, event);
    if (event == EVENT_VALCHANGE && ctrl->context2.p)
        dlg_refresh((dlgcontrol *)ctrl->context2.p, dlg);
}

/* Protocol radios + "Port:" for one tool, in that order, wired together. */
static void xfer_protocol_port_controls(struct controlset *s, int tool,
                                        HelpCtx helpctx)
{
    dlgcontrol *radio = NULL, *port;

    /* So the out-of-range sweep in windows/dialog.c covers these radios too:
     * they store their int exactly as conf_radiobutton_handler does. */
    kitty_conf_register_radio_handler(kitty_xfer_protocol_handler);

    switch (tool) {
      case XFER_TOOL_KSCP:
        radio = ctrl_radiobuttons(s, KT_XFER_PROTOCOL, NO_SHORTCUT, 2,
                                  helpctx, kitty_xfer_protocol_handler,
                                  I(CONF_kscp_protocol),
                                  KT_XFER_PROTO_SFTP, NO_SHORTCUT, I(1),
                                  KT_XFER_PROTO_SCP,  NO_SHORTCUT, I(0));
        port = ctrl_editbox(s, KT_XFER_PORT, NO_SHORTCUT, 20, helpctx,
                            xfer_port_handler, I(CONF_kscp_port), I(tool));
        break;
      case XFER_TOOL_WINSCP:
        /* Two abreast: "FTPS (implicit TLS, legacy)" does not fit three. */
        radio = ctrl_radiobuttons(s, KT_XFER_PROTOCOL, NO_SHORTCUT, 2,
                                  helpctx, kitty_xfer_protocol_handler,
                                  I(CONF_winscpprot),
                                  KT_XFER_PROTO_SFTP,  NO_SHORTCUT, I(1),
                                  KT_XFER_PROTO_SCP,   NO_SHORTCUT, I(0),
                                  KT_XFER_PROTO_FTP,   NO_SHORTCUT, I(2),
                                  KT_XFER_PROTO_FTPES, NO_SHORTCUT, I(4),
                                  KT_XFER_PROTO_FTPS,  NO_SHORTCUT, I(3),
                                  KT_XFER_PROTO_HTTP,  NO_SHORTCUT, I(5),
                                  KT_XFER_PROTO_HTTPS, NO_SHORTCUT, I(6));
        port = ctrl_editbox(s, KT_XFER_PORT, NO_SHORTCUT, 20, helpctx,
                            xfer_port_handler, I(CONF_winscp_port), I(tool));
        break;
      default:
        radio = ctrl_radiobuttons(s, KT_XFER_PROTOCOL, NO_SHORTCUT, 2,
                                  helpctx, kitty_xfer_protocol_handler,
                                  I(CONF_filezilla_protocol),
                                  KT_XFER_PROTO_SFTP,  NO_SHORTCUT, I(1),
                                  KT_XFER_PROTO_FTP,   NO_SHORTCUT, I(2),
                                  KT_XFER_PROTO_FTPES, NO_SHORTCUT, I(4),
                                  KT_XFER_PROTO_FTPS,  NO_SHORTCUT, I(3));
        port = ctrl_editbox(s, KT_XFER_PORT, NO_SHORTCUT, 20, helpctx,
                            xfer_port_handler, I(CONF_filezilla_port), I(tool));
        break;
    }
    radio->context2.p = port;
}

/* The Connection/SSH panel tree: SSH core, Kex, Host keys, Cipher, Auth
 * (+Credentials/GSSAPI), TTY, X11, Tunnels, Bugs, and the KiTTY PSCP/WinSCP
 * panel. Upstream's "More bugs" is folded into Bugs as a second group box:
 * that split was its answer to height, and this box scrolls. Kept as one helper so the shared protocol/
 * midsession guard structure stays verbatim. */
void scb_panel_ssh(struct controlbox *b, bool midsession, int protocol, int protcfginfo)
{
    struct ttymodes_data *td;
    struct portfwd_data *pfd;
    struct manual_hostkey_data *mh;
    struct controlset *s;
    dlgcontrol *c;

    if (DISPLAY_RECONFIGURABLE_PROTOCOL(PROT_SSH) ||
        DISPLAY_RECONFIGURABLE_PROTOCOL(PROT_SSHCONN)) {
        /*
         * The Connection/SSH panel.
         */
        ctrl_settitle(b, "Connection/SSH",
                      KT_SSH_OPTIONS_CONTROLLING_SSH_CONNECTIONS);

        /* SSH-1 or connection-sharing downstream */
        if (midsession && (protcfginfo == 1 || protcfginfo == -1)) {
            s = ctrl_getset(b, "Connection/SSH", "disclaimer", NULL);
            ctrl_text(s, KT_SSH_NOTHING_ON_THIS_PANEL_MAY, HELPCTX(no_help));
        }

        if (!midsession) {

            s = ctrl_getset(b, "Connection/SSH", "data",
                            KT_DATA_DATA_TO_SEND);
            ctrl_editbox(s, KT_SSH_REMOTE_COMMAND, 'r', 100,
                         HELPCTX(ssh_command),
                         conf_editbox_handler, I(CONF_remote_cmd), ED_STR);

            s = ctrl_getset(b, "Connection/SSH", "protocol", KT_SSH_PROTOCOL_OPTIONS);
            ctrl_checkbox(s, KT_SSH_DON_T_START_A_SHELL, 'n',
                          HELPCTX(ssh_noshell),
                          conf_checkbox_handler,
                          I(CONF_ssh_no_shell));
        }

        if (!midsession || !(protcfginfo == 1 || protcfginfo == -1)) {
            s = ctrl_getset(b, "Connection/SSH", "protocol", KT_SSH_PROTOCOL_OPTIONS);

            ctrl_checkbox(s, KT_SSH_ENABLE_COMPRESSION, 'e',
                          HELPCTX(ssh_compress),
                          conf_checkbox_handler,
                          I(CONF_compression));
        }

        if (!midsession) {
            /* KiTTY: these two strings name the OTHER PROCESS in a shared
             * connection, not the PuTTY project - upstream calls it "the
             * upstream PuTTY", which reads here as if it meant our upstream.
             * Keep them saying KiTTY on a rebase. */
            s = ctrl_getset(b, "Connection/SSH", "sharing", KT_SSH_SHARING_AN_SSH_CONNECTION_BETWEEN);

            ctrl_checkbox(s, KT_SSH_SHARE_SSH_CONNECTIONS_IF_POSSIBLE, 's',
                          HELPCTX(ssh_share),
                          conf_checkbox_handler,
                          I(CONF_ssh_connection_sharing));

            ctrl_text(s, KT_SSH_PERMITTED_ROLES_IN_A_SHARED,
                      HELPCTX(ssh_share));
            ctrl_checkbox(s, KT_SSH_UPSTREAM_CONNECTING_TO_THE_REAL, 'u',
                          HELPCTX(ssh_share),
                          conf_checkbox_handler,
                          I(CONF_ssh_connection_sharing_upstream));
            ctrl_checkbox(s, KT_SSH_DOWNSTREAM_CONNECTING_TO_THE_UPSTREAM, 'd',
                          HELPCTX(ssh_share),
                          conf_checkbox_handler,
                          I(CONF_ssh_connection_sharing_downstream));
        }

        if (!midsession) {
            s = ctrl_getset(b, "Connection/SSH", "protocol", KT_SSH_PROTOCOL_OPTIONS);

            ctrl_radiobuttons(s, KT_SSH_SSH_PROTOCOL_VERSION, NO_SHORTCUT, 2,
                              HELPCTX(ssh_protocol),
                              conf_radiobutton_handler,
                              I(CONF_sshprot),
                              KT_SSH_2, '2', I(3),
                              KT_SSH_1_INSECURE, '1', I(0));
        }
        if (!midsession) {

            /*
             * The Connection/SSH/Auth panel.
             */
            ctrl_settitle(b, "Connection/SSH/Auth",
                          KT_AUTH_OPTIONS_CONTROLLING_SSH_AUTHENTICATION);

            s = ctrl_getset(b, "Connection/SSH/Auth", "main", NULL);
            ctrl_checkbox(s, KT_AUTH_DISPLAY_PRE_AUTHENTICATION_BANNER_SSH,
                          'd', HELPCTX(ssh_auth_banner),
                          conf_checkbox_handler,
                          I(CONF_ssh_show_banner));
            ctrl_checkbox(s, KT_AUTH_BYPASS_AUTHENTICATION_ENTIRELY_SSH_2, 'b',
                          HELPCTX(ssh_auth_bypass),
                          conf_checkbox_handler,
                          I(CONF_ssh_no_userauth));
            ctrl_checkbox(s, KT_AUTH_DISCONNECT_IF_AUTHENTICATION_SUCCEEDS_TRIVIALLY,
                          'n', HELPCTX(ssh_no_trivial_userauth),
                          conf_checkbox_handler,
                          I(CONF_ssh_no_trivial_userauth));

            s = ctrl_getset(b, "Connection/SSH/Auth", "methods",
                            KT_AUTH_AUTHENTICATION_METHODS);
            ctrl_checkbox(s, KT_AUTH_ATTEMPT_AUTHENTICATION_USING_KAGEANT_PAGEANT, 'p',
                          HELPCTX(ssh_auth_pageant),
                          conf_checkbox_handler,
                          I(CONF_tryagent));
            ctrl_checkbox(s, KT_AUTH_ATTEMPT_TIS_OR_CRYPTOCARD_AUTH, 'm',
                          HELPCTX(ssh_auth_tis),
                          conf_checkbox_handler,
                          I(CONF_try_tis_auth));
            ctrl_checkbox(s, KT_AUTH_ATTEMPT_KEYBOARD_INTERACTIVE_AUTH_SSH,
                          'i', HELPCTX(ssh_auth_ki),
                          conf_checkbox_handler,
                          I(CONF_try_ki_auth));

            s = ctrl_getset(b, "Connection/SSH/Auth", "aux",
                            KT_AUTH_OTHER_AUTHENTICATION_RELATED_OPTIONS);
            ctrl_checkbox(s, KT_AUTH_ALLOW_AGENT_FORWARDING, 'f',
                          HELPCTX(ssh_auth_agentfwd),
                          conf_checkbox_handler, I(CONF_agentfwd));
            ctrl_checkbox(s, KT_AUTH_ALLOW_ATTEMPTED_CHANGES_OF_USERNAME, NO_SHORTCUT,
                          HELPCTX(ssh_auth_changeuser),
                          conf_checkbox_handler,
                          I(CONF_change_username));

            /* KiTTY: the serving-agent warning's off switch, surfaced where
             * agent authentication is configured. Announced the way the
             * WinSCP path and the workplace-proxy box announce it - the
             * shared bold lead line - so it cannot be read as one more
             * session option ([KiTTY] verifyagent; see the handler). */
            /* The unverified-agent warning is on Application > Security now:
             * it is one switch for the whole application, and it used to need
             * a bold "not a session setting" line to say so here. */

            ctrl_settitle(b, "Connection/SSH/Auth/Credentials",
                          KT_CREDENTIALS_CREDENTIALS_TO_AUTHENTICATE);

            s = ctrl_getset(b, "Connection/SSH/Auth/Credentials", "publickey",
                            KT_CREDENTIALS_PUBLIC_KEY_AUTHENTICATION);
            ctrl_filesel(s, KT_CREDENTIALS_PRIVATE_KEY_FILE_FOR_AUTHENTICATION, 'k',
                         FILTER_KEY_FILES, false, KT_CREDENTIALS_SELECT_PRIVATE_KEY_FILE,
                         HELPCTX(ssh_auth_privkey),
                         conf_filesel_handler, I(CONF_keyfile));
            ctrl_filesel(s, KT_CREDENTIALS_CERTIFICATE_TO_USE, 'e',
                         FILTER_ALL_FILES, false, KT_CREDENTIALS_SELECT_CERTIFICATE_FILE,
                         HELPCTX(ssh_auth_cert),
                         conf_filesel_handler, I(CONF_detached_cert));
            /* KiTTY: opt-in pin of the key FILE. Always the key's own
             * fingerprint, never the certificate's - certificates rotate by
             * design and must not break the pin. */
            {
                /* the button refreshes exactly this box after recording */
                dlgcontrol *fpbox = ctrl_editbox(s, KT_CREDENTIALS_PINNED_KEY_FINGERPRINT_EMPTY_NO,
                             NO_SHORTCUT, 100, HELPCTX(kitty_keypin),
                             conf_editbox_handler, I(CONF_publickey_fingerprint),
                             ED_STR);
                ctrl_pushbutton(s, KT_CREDENTIALS_RECORD_FINGERPRINT_OF_THE_KEY,
                                NO_SHORTCUT, HELPCTX(kitty_keypin),
                                kitty_keyfile_pin_record_handler, P(fpbox));
            }

            s = ctrl_getset(b, "Connection/SSH/Auth/Credentials", "plugin",
                            KT_CREDENTIALS_PLUGIN_TO_PROVIDE_AUTHENTICATION_RESPONSES);
            ctrl_editbox(s, KT_CREDENTIALS_PLUGIN_COMMAND_TO_RUN, NO_SHORTCUT, 100,
                         HELPCTX(ssh_auth_plugin),
                         conf_editbox_handler, I(CONF_auth_plugin), ED_STR);
#ifndef NO_GSSAPI
            /*
             * Connection/SSH/Auth/GSSAPI, which sadly won't fit on
             * the main Auth panel.
             */
            ctrl_settitle(b, "Connection/SSH/Auth/GSSAPI",
                          KT_GSSAPI_OPTIONS_CONTROLLING_GSSAPI_AUTHENTICATION);
            s = ctrl_getset(b, "Connection/SSH/Auth/GSSAPI", "gssapi", NULL);

            ctrl_checkbox(s, KT_GSSAPI_ATTEMPT_GSSAPI_AUTHENTICATION_SSH_2,
                          't', HELPCTX(ssh_gssapi),
                          conf_checkbox_handler,
                          I(CONF_try_gssapi_auth));

            ctrl_checkbox(s, KT_GSSAPI_ATTEMPT_GSSAPI_KEY_EXCHANGE_SSH,
                          'k', HELPCTX(ssh_gssapi),
                          conf_checkbox_handler,
                          I(CONF_try_gssapi_kex));

            ctrl_checkbox(s, KT_GSSAPI_ALLOW_GSSAPI_CREDENTIAL_DELEGATION, 'l',
                          HELPCTX(ssh_gssapi_delegation),
                          conf_checkbox_handler,
                          I(CONF_gssapifwd));

            /*
             * GSSAPI library selection.
             */
            if (ngsslibs > 1) {
                c = ctrl_draglist(s, KT_GSSAPI_PREFERENCE_ORDER_FOR_GSSAPI_LIBRARIES,
                                  'p', HELPCTX(ssh_gssapi_libraries),
                                  gsslist_handler, P(NULL));
                c->listbox.height = ngsslibs;

                /*
                 * I currently assume that if more than one GSS
                 * library option is available, then one of them is
                 * 'user-supplied' and so we should present the
                 * following file selector. This is at least half-
                 * reasonable, because if we're using statically
                 * linked GSSAPI then there will only be one option
                 * and no way to load from a user-supplied library,
                 * whereas if we're using dynamic libraries then
                 * there will almost certainly be some default
                 * option in addition to a user-supplied path. If
                 * anyone ever ports PuTTY to a system on which
                 * dynamic-library GSSAPI is available but there is
                 * absolutely no consensus on where to keep the
                 * libraries, there'll need to be a flag alongside
                 * ngsslibs to control whether the file selector is
                 * displayed.
                 */

                ctrl_filesel(s, KT_GSSAPI_USER_SUPPLIED_GSSAPI_LIBRARY_PATH, 's',
                             FILTER_DYNLIB_FILES, false, KT_GSSAPI_SELECT_LIBRARY_FILE,
                             HELPCTX(ssh_gssapi_libraries),
                             conf_filesel_handler,
                             I(CONF_ssh_gss_custom));
            }
#endif
        }


        /*
         * The Connection/SSH/Kex panel. (Owing to repeat key
         * exchange, much of this is meaningful in mid-session _if_
         * we're using SSH-2 and are not a connection-sharing
         * downstream, or haven't decided yet.)
         */
        if (protcfginfo != 1 && protcfginfo != -1) {
            ctrl_settitle(b, "Connection/SSH/Kex",
                          KT_KEX_OPTIONS_CONTROLLING_SSH_KEY_EXCHANGE);

            s = ctrl_getset(b, "Connection/SSH/Kex", "main",
                            KT_KEX_KEY_EXCHANGE_ALGORITHM_OPTIONS);
            c = ctrl_draglist(s, KT_KEX_ALGORITHM_SELECTION_POLICY, 's',
                              HELPCTX(ssh_kexlist),
                              kexlist_handler, P(NULL));
            /* 6 rows like the cipher list, not one per algorithm - the list
             * scrolls, and the panel stops paying for a dozen rows. */
            c->listbox.height = 6;
            ctrl_checkbox(s, KT_KEX_WARN_IF_KEY_EXCHANGE, 'q', HELPCTX(ssh_kexlist),
                          conf_checkbox_handler,
                          I(CONF_ssh_warn_pre_quantum));
#ifndef NO_GSSAPI
            ctrl_checkbox(s, KT_KEX_ATTEMPT_GSSAPI_KEY_EXCHANGE,
                          'k', HELPCTX(ssh_gssapi),
                          conf_checkbox_handler,
                          I(CONF_try_gssapi_kex));
#endif

            s = ctrl_getset(b, "Connection/SSH/Kex", "repeat",
                            KT_KEX_OPTIONS_CONTROLLING_KEY_RE_EXCHANGE);

            ctrl_editbox(s, KT_KEX_MAX_MINUTES_BEFORE_REKEY_0, 't', 20,
                         HELPCTX(ssh_kex_repeat),
                         conf_editbox_handler,
                         I(CONF_ssh_rekey_time),
                         ED_INT);
#ifndef NO_GSSAPI
            ctrl_editbox(s, KT_KEX_MINUTES_BETWEEN_GSS_CHECKS_0, NO_SHORTCUT, 20,
                         HELPCTX(ssh_kex_repeat),
                         conf_editbox_handler,
                         I(CONF_gssapirekey),
                         ED_INT);
#endif
            ctrl_editbox(s, KT_KEX_MAX_DATA_BEFORE_REKEY_0, 'x', 20,
                         HELPCTX(ssh_kex_repeat),
                         conf_editbox_handler,
                         I(CONF_ssh_rekey_data),
                         ED_STR);
            ctrl_text(s, KT_KEX_USE_1M_FOR_1_MEGABYTE,
                      HELPCTX(ssh_kex_repeat));
        }

        /*
         * The 'Connection/SSH/Host keys' panel.
         */
        if (protcfginfo != 1 && protcfginfo != -1) {
            ctrl_settitle(b, "Connection/SSH/Host keys",
                          KT_HOST_KEYS_OPTIONS_CONTROLLING_SSH_HOST_KEYS);

            s = ctrl_getset(b, "Connection/SSH/Host keys", "main",
                            KT_HOST_KEYS_HOST_KEY_ALGORITHM_PREFERENCE);
            c = ctrl_draglist(s, KT_KEX_ALGORITHM_SELECTION_POLICY, 's',
                              HELPCTX(ssh_hklist),
                              hklist_handler, P(NULL));
            c->listbox.height = HK_MAX;    /* tall enough to show every algorithm */

            ctrl_checkbox(s, KT_HOST_KEYS_PREFER_ALGORITHMS_FOR_WHICH,
                          'p', HELPCTX(ssh_hk_known), conf_checkbox_handler,
                          I(CONF_ssh_prefer_known_hostkeys));
        }

        /*
         * Manual host key configuration is irrelevant mid-session,
         * as we enforce that the host key for rekeys is the
         * same as that used at the start of the session.
         */
        if (!midsession) {
            s = ctrl_getset(b, "Connection/SSH/Host keys", "hostkeys",
                            KT_HOST_KEYS_MANUALLY_CONFIGURE_HOST_KEYS);

            ctrl_columns(s, 2, 75, 25);
            c = ctrl_text(s, KT_HOST_KEYS_HOST_KEYS_OR_FINGERPRINTS,
                          HELPCTX(ssh_kex_manual_hostkeys));
            c->column = 0;
            /* You want to select from the list, _then_ hit Remove. So
             * tab order should be that way round. */
            mh = (struct manual_hostkey_data *)
                ctrl_alloc(b,sizeof(struct manual_hostkey_data));
            mh->rembutton = ctrl_pushbutton(s, KT_DATA_REMOVE, 'r',
                                            HELPCTX(ssh_kex_manual_hostkeys),
                                            manual_hostkey_handler, P(mh));
            mh->rembutton->column = 1;
            mh->rembutton->delay_taborder = true;
            mh->listbox = ctrl_listbox(s, NULL, NO_SHORTCUT,
                                       HELPCTX(ssh_kex_manual_hostkeys),
                                       manual_hostkey_handler, P(mh));
            /* This list box can't be very tall, because there's not
             * much room in the pane on Windows at least. This makes
             * it become really unhelpful if a horizontal scrollbar
             * appears, so we suppress that. */
            mh->listbox->listbox.height = 2;
            mh->listbox->listbox.hscroll = false;
            kitty_manual_hk_list = mh->listbox;   /* refreshed after a Pin from the scan box */
            ctrl_tabdelay(s, mh->rembutton);
            mh->keybox = ctrl_editbox(s, KT_HOST_KEYS_KEY, 'k', 80,
                                      HELPCTX(ssh_kex_manual_hostkeys),
                                      manual_hostkey_handler, P(mh), P(NULL));
            mh->keybox->column = 0;
            mh->addbutton = ctrl_pushbutton(s, KT_HOST_KEYS_ADD_KEY, 'y',
                                            HELPCTX(ssh_kex_manual_hostkeys),
                                            manual_hostkey_handler, P(mh));
            mh->addbutton->column = 1;
            /* Centre it on the FIELD it adds from. Without this the button
             * lines up with the top of the control beside it - which is the
             * "Key" label, not the box - and the row reads as two unrelated
             * things at different heights. */
            mh->addbutton->align_next_to = mh->keybox;
            ctrl_columns(s, 1, 100);
        }

        /*
         * But there's no reason not to forbid access to the host CA
         * configuration box, which is common across sessions in any
         * case.
         */
        s = ctrl_getset(b, "Connection/SSH/Host keys", "ca",
                        KT_HOST_KEYS_CONFIGURE_TRUSTED_CERTIFICATION_AUTHORITIES);
        {
            /* KiTTY: the CA editor lives on the Application tab; the button
             * jumps there. Without that tab (applicationsettings=no) there is
             * nowhere to jump, and the line says where the setting is. */
            if (GetPuttyFlag())
                c = ctrl_pushbutton(s, KT_HOST_KEYS_CONFIGURE_HOST_CAS, NO_SHORTCUT,
                                    HELPCTX(ssh_kex_cert),
                                    host_ca_button_handler, I(0));
            else if (GetConfigBoxApplicationSettingsFlag())
                c = ctrl_pushbutton(s, KT_HOST_KEYS_CONFIGURE_HOST_CAS, NO_SHORTCUT,
                                    HELPCTX(ssh_kex_cert),
                                    host_ca_jump_handler, I(0));
            else
                ctrl_text(s, KT_HOST_KEYS_CAS_JUMP, HELPCTX(ssh_kex_cert));
        }

        /* KiTTY: ask the host for its keys now and take new ones in - the
         * store's side of this panel (kitty_hostkey_scan_box). */
        if (!GetPuttyFlag()) {
            s = ctrl_getset(b, "Connection/SSH/Host keys", "scan", KT_HKSCAN_GROUP);
            hkscan_line = ctrl_text(s, " ", HELPCTX(kitty_host_keys));
            /* The types line is laid out for the LONGEST list there can be
             * (every type KiTTY knows), so it never gets clipped when the
             * real list wraps onto a second line; the refresh puts the
             * real text in. */
            hkscan_types = ctrl_text(s, KT_HKSCAN_TYPES_WORST, HELPCTX(kitty_host_keys));
            ctrl_columns(s, 3, 34, 33, 33);
            c = ctrl_pushbutton(s, KT_HKSCAN_BUTTON, NO_SHORTCUT, HELPCTX(kitty_host_keys),
                                kitty_hkscan_handler, P(NULL));
            c->column = 2;
            ctrl_columns(s, 1, 100);
        }

        if (!midsession || !(protcfginfo == 1 || protcfginfo == -1)) {
            /*
             * The Connection/SSH/Cipher panel.
             */
            ctrl_settitle(b, "Connection/SSH/Cipher",
                          KT_CIPHER_OPTIONS_CONTROLLING_SSH_ENCRYPTION);

            s = ctrl_getset(b, "Connection/SSH/Cipher",
                            "encryption", KT_CIPHER_ENCRYPTION_OPTIONS);
            c = ctrl_draglist(s, KT_CIPHER_ENCRYPTION_CIPHER_SELECTION_POLICY, 's',
                              HELPCTX(ssh_ciphers),
                              cipherlist_handler, P(NULL));
            c->listbox.height = CIPHER_MAX;  /* tall enough to show every cipher */

            ctrl_checkbox(s, KT_CIPHER_ENABLE_LEGACY_USE_OF_SINGLE, 'i',
                          HELPCTX(ssh_ciphers),
                          conf_checkbox_handler,
                          I(CONF_ssh2_des_cbc));
        }

        if (!midsession) {
            /*
             * The Connection/SSH/TTY panel.
             */
            ctrl_settitle(b, "Connection/SSH/TTY", KT_TTY_REMOTE_TERMINAL_SETTINGS);

            s = ctrl_getset(b, "Connection/SSH/TTY", "sshtty", NULL);
            ctrl_checkbox(s, KT_TTY_DON_T_ALLOCATE_A_PSEUDO, 'p',
                          HELPCTX(ssh_nopty),
                          conf_checkbox_handler,
                          I(CONF_nopty));

            s = ctrl_getset(b, "Connection/SSH/TTY", "ttymodes",
                            KT_TTY_TERMINAL_MODES);
            td = (struct ttymodes_data *)
                ctrl_alloc(b, sizeof(struct ttymodes_data));
            ctrl_text(s, KT_TTY_TERMINAL_MODES_TO_SEND, HELPCTX(ssh_ttymodes));
            td->listbox = ctrl_listbox(s, NULL, NO_SHORTCUT,
                                       HELPCTX(ssh_ttymodes),
                                       ttymodes_handler, P(td));
            td->listbox->listbox.height = 8;
            td->listbox->listbox.ncols = 2;
            td->listbox->listbox.percentages = snewn(2, int);
            td->listbox->listbox.percentages[0] = 40;
            td->listbox->listbox.percentages[1] = 60;
            ctrl_columns(s, 2, 75, 25);
            c = ctrl_text(s, KT_TTY_FOR_SELECTED_MODE_SEND, HELPCTX(ssh_ttymodes));
            c->column = 0;
            td->setbutton = ctrl_pushbutton(s, KT_COPY_SET, 's',
                                            HELPCTX(ssh_ttymodes),
                                            ttymodes_handler, P(td));
            td->setbutton->column = 1;
            td->setbutton->delay_taborder = true;
            ctrl_columns(s, 1, 100);        /* column break */
            /* Bit of a hack to get the value radio buttons and
             * edit-box on the same row. */
            ctrl_columns(s, 2, 75, 25);
            td->valradio = ctrl_radiobuttons(s, NULL, NO_SHORTCUT, 3,
                                             HELPCTX(ssh_ttymodes),
                                             ttymodes_handler, P(td),
                                             KT_TERMINAL_AUTO, NO_SHORTCUT, P(NULL),
                                             KT_TTY_NOTHING, NO_SHORTCUT, P(NULL),
                                             KT_TTY_THIS, NO_SHORTCUT, P(NULL));
            td->valradio->column = 0;
            td->valbox = ctrl_editbox(s, NULL, NO_SHORTCUT, 100,
                                      HELPCTX(ssh_ttymodes),
                                      ttymodes_handler, P(td), P(NULL));
            td->valbox->column = 1;
            td->valbox->align_next_to = td->valradio;
            ctrl_tabdelay(s, td->setbutton);
        }

        if (!midsession) {
            /*
             * The Connection/SSH/X11 panel.
             */
            ctrl_settitle(b, "Connection/SSH/X11",
                          KT_X11_OPTIONS_CONTROLLING_SSH_X11_FORWARDING);

            s = ctrl_getset(b, "Connection/SSH/X11", "x11", KT_X11_X11_FORWARDING);
            ctrl_checkbox(s, KT_X11_ENABLE_X11_FORWARDING, 'e',
                          HELPCTX(ssh_tunnels_x11),
                          conf_checkbox_handler,I(CONF_x11_forward));
            ctrl_editbox(s, KT_X11_X_DISPLAY_LOCATION, 'x', 50,
                         HELPCTX(ssh_tunnels_x11),
                         conf_editbox_handler, I(CONF_x11_display), ED_STR);
            ctrl_radiobuttons(s, KT_X11_REMOTE_X11_AUTHENTICATION_PROTOCOL, 'u', 2,
                              HELPCTX(ssh_tunnels_x11auth),
                              conf_radiobutton_handler,
                              I(CONF_x11_auth),
                              KT_X11_MIT_MAGIC_COOKIE_1, I(X11_MIT),
                              KT_X11_XDM_AUTHORIZATION_1, I(X11_XDM));
        }

        /*
         * The Tunnels panel _is_ still available in mid-session.
         */
        ctrl_settitle(b, "Connection/SSH/Tunnels",
                      KT_TUNNELS_OPTIONS_CONTROLLING_SSH_PORT_FORWARDING);

        s = ctrl_getset(b, "Connection/SSH/Tunnels", "portfwd",
                        KT_TUNNELS_PORT_FORWARDING);
        ctrl_checkbox(s, KT_TUNNELS_LOCAL_PORTS_ACCEPT_CONNECTIONS,'t',
                      HELPCTX(ssh_tunnels_portfwd_localhost),
                      conf_checkbox_handler,
                      I(CONF_lport_acceptall));
        ctrl_checkbox(s, KT_TUNNELS_REMOTE_PORTS_DO_THE_SAME, 'p',
                      HELPCTX(ssh_tunnels_portfwd_localhost),
                      conf_checkbox_handler,
                      I(CONF_rport_acceptall));
        if (!GetPuttyFlag())
            ctrl_checkbox(s, KT_TUNNELS_PRINT_DYNAMIC_PORTS_IN_WINDOW, NO_SHORTCUT,
                          HELPCTX(kitty_dynports), conf_checkbox_handler,
                          I(CONF_ssh_tunnel_print_in_title));

        ctrl_columns(s, 3, 55, 20, 25);
        c = ctrl_text(s, KT_TUNNELS_FORWARDED_PORTS, HELPCTX(ssh_tunnels_portfwd));
        c->column = COLUMN_FIELD(0,2);
        /* You want to select from the list, _then_ hit Remove. So tab order
         * should be that way round. */
        pfd = (struct portfwd_data *)ctrl_alloc(b,sizeof(struct portfwd_data));
        pfd->rembutton = ctrl_pushbutton(s, KT_DATA_REMOVE, 'r',
                                         HELPCTX(ssh_tunnels_portfwd),
                                         portfwd_handler, P(pfd));
        pfd->rembutton->column = 2;
        pfd->rembutton->delay_taborder = true;
        pfd->listbox = ctrl_listbox(s, NULL, NO_SHORTCUT,
                                    HELPCTX(ssh_tunnels_portfwd),
                                    portfwd_handler, P(pfd));
        pfd->listbox->listbox.height = 3;
        pfd->listbox->listbox.ncols = 2;
        pfd->listbox->listbox.percentages = snewn(2, int);
        pfd->listbox->listbox.percentages[0] = 20;
        pfd->listbox->listbox.percentages[1] = 80;
        ctrl_tabdelay(s, pfd->rembutton);
        ctrl_text(s, KT_TUNNELS_ADD_NEW_FORWARDED_PORT, HELPCTX(ssh_tunnels_portfwd));
        /* You want to enter source, destination and type, _then_ hit Add.
         * Again, we adjust the tab order to reflect this. */
        pfd->addbutton = ctrl_pushbutton(s, KT_DATA_ADD, 'd',
                                         HELPCTX(ssh_tunnels_portfwd),
                                         portfwd_handler, P(pfd));
        pfd->addbutton->column = 2;
        pfd->addbutton->delay_taborder = true;
        pfd->sourcebox = ctrl_editbox(s, KT_TUNNELS_SOURCE_PORT, 's', 40,
                                      HELPCTX(ssh_tunnels_portfwd),
                                      portfwd_handler, P(pfd), P(NULL));
        pfd->sourcebox->column = 0;
        pfd->destbox = ctrl_editbox(s, KT_TUNNELS_DESTINATION, 'i', 67,
                                    HELPCTX(ssh_tunnels_portfwd),
                                    portfwd_handler, P(pfd), P(NULL));
        pfd->direction = ctrl_radiobuttons(s, NULL, NO_SHORTCUT, 3,
                                           HELPCTX(ssh_tunnels_portfwd),
                                           portfwd_handler, P(pfd),
                                           KT_TUNNELS_LOCAL, 'l', P(NULL),
                                           KT_TUNNELS_REMOTE, 'm', P(NULL),
                                           KT_TUNNELS_DYNAMIC, 'y', P(NULL));
#ifndef NO_IPV6
        pfd->addressfamily =
            ctrl_radiobuttons(s, NULL, NO_SHORTCUT, 3,
                              HELPCTX(ssh_tunnels_portfwd_ipversion),
                              portfwd_handler, P(pfd),
                              KT_TERMINAL_AUTO, 'u', I(ADDRTYPE_UNSPEC),
                              KT_CONNECTION_IPV4, '4', I(ADDRTYPE_IPV4),
                              KT_CONNECTION_IPV6, '6', I(ADDRTYPE_IPV6));
#endif
        ctrl_tabdelay(s, pfd->addbutton);
        ctrl_columns(s, 1, 100);

        if (!midsession) {
            /*
             * The Connection/SSH/Bugs panels.
             */
            ctrl_settitle(b, "Connection/SSH/Bugs",
                          KT_BUGS_WORKAROUNDS_FOR_SSH_SERVER_BUGS);

            s = ctrl_getset(b, "Connection/SSH/Bugs", "main",
                            KT_BUGS_DETECTION_OF_KNOWN_BUGS);
            ctrl_droplist(s, KT_BUGS_CHOKES_ON_SSH_2_IGNORE, '2', 20,
                          HELPCTX(ssh_bugs_ignore2),
                          sshbug_handler, I(CONF_sshbug_ignore2));
            ctrl_droplist(s, KT_BUGS_HANDLES_SSH_2_KEY_RE, 'k', 20,
                          HELPCTX(ssh_bugs_rekey2),
                          sshbug_handler, I(CONF_sshbug_rekey2));
            ctrl_droplist(s, KT_BUGS_CHOKES_ON_PUTTY_S_SSH, 'j',
                          20, HELPCTX(ssh_bugs_winadj),
                          sshbug_handler, I(CONF_sshbug_winadj));
            ctrl_droplist(s, KT_BUGS_REPLIES_TO_REQUESTS_ON_CLOSED, 'q', 20,
                          HELPCTX(ssh_bugs_chanreq),
                          sshbug_handler, I(CONF_sshbug_chanreq));
            ctrl_droplist(s, KT_BUGS_IGNORES_SSH_2_MAXIMUM_PACKET, 'x', 20,
                          HELPCTX(ssh_bugs_maxpkt2),
                          sshbug_handler, I(CONF_sshbug_maxpkt2));

            s = ctrl_getset(b, "Connection/SSH/Bugs", "more",
                            KT_BUGS_FURTHER_DETECTION_OF_KNOWN_BUGS);
            ctrl_droplist(s, KT_BUGS_OLD_RSA_SHA2_CERT_ALGORITHM, 'l', 20,
                          HELPCTX(ssh_bugs_rsa_sha2_cert_userauth),
                          sshbug_handler,
                          I(CONF_sshbug_rsa_sha2_cert_userauth));
            ctrl_droplist(s, KT_BUGS_REQUIRES_PADDING_ON_SSH_2, 'u', 20,
                          HELPCTX(ssh_bugs_rsapad2),
                          sshbug_handler, I(CONF_sshbug_rsapad2));
            ctrl_droplist(s, KT_BUGS_ONLY_SUPPORTS_PRE_RFC4419_SSH, 'f', 20,
                          HELPCTX(ssh_bugs_oldgex2),
                          sshbug_handler, I(CONF_sshbug_oldgex2));
            ctrl_droplist(s, KT_BUGS_MISCOMPUTES_SSH_2_HMAC_KEYS, 'm', 20,
                          HELPCTX(ssh_bugs_hmac2),
                          sshbug_handler, I(CONF_sshbug_hmac2));
            ctrl_droplist(s, KT_BUGS_MISUSES_THE_SESSION_ID, 'n', 20,
                          HELPCTX(ssh_bugs_pksessid2),
                          sshbug_handler, I(CONF_sshbug_pksessid2));
            ctrl_droplist(s, KT_BUGS_MISCOMPUTES_SSH_2_ENCRYPTION_KEYS, 'e', 20,
                          HELPCTX(ssh_bugs_derivekey2),
                          sshbug_handler, I(CONF_sshbug_derivekey2));
            ctrl_droplist(s, KT_BUGS_CHOKES_ON_SSH_1_IGNORE, 'i', 20,
                          HELPCTX(ssh_bugs_ignore1),
                          sshbug_handler, I(CONF_sshbug_ignore1));
            ctrl_droplist(s, KT_BUGS_REFUSES_ALL_SSH_1_PASSWORD, 's', 20,
                          HELPCTX(ssh_bugs_plainpw1),
                          sshbug_handler, I(CONF_sshbug_plainpw1));
            ctrl_droplist(s, KT_BUGS_CHOKES_ON_SSH_1_RSA, 'r', 20,
                          HELPCTX(ssh_bugs_rsa1),
                          sshbug_handler, I(CONF_sshbug_rsa1));

            s = ctrl_getset(b, "Connection/SSH/Bugs", "manual",
                            KT_BUGS_MANUALLY_ENABLED_WORKAROUNDS);
            ctrl_droplist(s, KT_BUGS_DISCARDS_DATA_SENT_BEFORE_ITS, 'd', 20,
                          HELPCTX(ssh_bugs_dropstart),
                          sshbug_handler_manual_only,
                          I(CONF_sshbug_dropstart));
            ctrl_droplist(s, KT_BUGS_CHOKES_ON_PUTTY_S_FULL, 'p', 20,
                          HELPCTX(ssh_bugs_filter_kexinit),
                          sshbug_handler_manual_only,
                          I(CONF_sshbug_filter_kexinit));
        }

        /* KiTTY: kscp / WinSCP integration. Backend = StartWinSCP / SendFile.
         * TWO panels, deliberately: together the controls overflowed the
         * panel area into the dialog's command buttons (caught by the
         * documentation screenshots), and the content is genuinely two
         * topics - kscp transfers and the WinSCP hand-off. */
        if (!GetPuttyFlag()) {
            ctrl_settitle(b, "Connection/SSH/KSCP",
                          KT_KSCP_KSCP_FILE_TRANSFER_INTEGRATION);

            s = ctrl_getset(b, "Connection/SSH/KSCP",
                            "pscp", KT_KSCP_KSCP_INTEGRATION);
            /* What the two kscp Tools menu entries do, with their keys. */
            ctrl_text(s, KT_KSCP_SEND_FILE_LINE, HELPCTX(kitty_winscp));
            ctrl_text(s, KT_KSCP_GET_FILE_LINE, HELPCTX(kitty_winscp));
            /* kscp's own protocol and port (KscpProtocol / KscpPort): SCP
             * runs kscp with -scp, SFTP with -sftp. WinSCP and FileZilla
             * have their own pair on their own panels. */
            xfer_protocol_port_controls(s, XFER_TOOL_KSCP, HELPCTX(kitty_winscp));
            /* Drops on the terminal window. Off unregisters the window for
             * drops, so the cursor says no before a file is let go; the
             * Tools menu entries have their own switches on Window >
             * Behaviour. */
            ctrl_checkbox(s, KT_KSCP_DRAGDROP_UPLOAD, NO_SHORTCUT,
                          HELPCTX(kitty_winscp),
                          conf_checkbox_handler, I(CONF_kscp_dragdrop));
            /* The remote directory (OSC 7 / fixed) and the download folder
             * live on Connection > Transfers: they serve ZModem and transfers
             * over the session too, which are not SSH-only. */
            ctrl_text(s, KT_KSCP_DIRS_ON_TRANSFERS, HELPCTX(kitty_winscp));
            ctrl_editbox(s, KT_KSCP_KSCP_OPTIONS, NO_SHORTCUT, 100,
                         HELPCTX(kitty_winscp),
                         conf_editbox_handler, I(CONF_pscpoptions), ED_STR);
            ctrl_text(s, KT_KSCP_FLAGS_PASSED_TO_KSCP_DEFAULT, HELPCTX(kitty_winscp));
            ctrl_checkbox(s, KT_KSCP_KEEP_THE_TRANSFER_WINDOW_OPEN,
                          NO_SHORTCUT, HELPCTX(kitty_winscp),
                          conf_checkbox_handler, I(CONF_pscp_keep_window));
            /* The helper's path and port are application settings (KiTTY++
             * Settings > Transfers & Tools); say so, and offer the way there
             * when the Application tab exists. */
            {
                dlgcontrol *note, *btn;
                s = ctrl_getset(b, "Connection/SSH/KSCP", "global", NULL);
                if (GetConfigBoxApplicationSettingsFlag()) {
                    ctrl_columns(s, 2, 55, 45);
                    note = ctrl_text(s, KT_KSCP_HELPER_IS_GLOBAL, HELPCTX(kitty_winscp));
                    note->column = 0;
                    btn = ctrl_pushbutton(s, KT_KSCP_OPEN_GLOBAL_PANEL, NO_SHORTCUT,
                                          HELPCTX(kitty_winscp),
                                          kscp_global_jump_handler, I(0));
                    btn->column = 1;
                    ctrl_columns(s, 1, 100);
                } else {
                    ctrl_text(s, KT_KSCP_HELPER_IS_GLOBAL, HELPCTX(kitty_winscp));
                }
            }

            ctrl_settitle(b, "Connection/SSH/WinSCP",
                          KT_WINSCP_WINSCP_INTEGRATION);

            s = ctrl_getset(b, "Connection/SSH/WinSCP",
                            "WinSCP", KT_WINSCP_WINSCP_INTEGRATION);
            /* WinSCP's own protocol and port (WinSCPProtocol / WinSCPPort):
             * the whole list WinSCP takes. kscp and FileZilla have their
             * own pair on their own panels. */
            xfer_protocol_port_controls(s, XFER_TOOL_WINSCP,
                                        HELPCTX(kitty_winscp_session));
            /* The executable PATH is on KiTTY++ Settings > Transfers & Tools > WinSCP
             * now. It never belonged here - it is a property of this PC, which
             * is why it needed a bold "not a session setting" note to itself.
             * The target override left too: it serves FileZilla as well, so it
             * cannot live on one of the two leaves (Connection > Transfers,
             * "Remote target"). Everything in this group IS per session and
             * WinSCP's alone. */
            ctrl_editbox(s, KT_WINSCP_WINSCP_ADDITIONAL_OPTIONS, NO_SHORTCUT, 100,
                         HELPCTX(kitty_winscp_session),
                         conf_editbox_handler, I(CONF_winscpoptions), ED_STR);
            ctrl_editbox(s, KT_WINSCP_WINSCP_ADDITIONAL_RAWSETTINGS, NO_SHORTCUT, 100,
                         HELPCTX(kitty_winscp_session),
                         conf_editbox_handler, I(CONF_winscprawsettings), ED_STR);
            ctrl_editbox(s, KT_WINSCP_SHELL_SCP_MODE_ONLY, NO_SHORTCUT, 100,
                         HELPCTX(kitty_winscp_session),
                         conf_editbox_handler, I(CONF_pscpshell), ED_STR);
            /* Where the executable path went, and a way there. Without the
             * Application tab (applicationsettings=no) there is nowhere to
             * jump, so only the note stays. */
            {
                dlgcontrol *note, *btn;
                s = ctrl_getset(b, "Connection/SSH/WinSCP", "global", NULL);
                if (GetConfigBoxApplicationSettingsFlag()) {
                    ctrl_columns(s, 2, 62, 38);
                    note = ctrl_text(s, KT_WINSCP_PATH_IS_GLOBAL,
                                     HELPCTX(kitty_winscp_session));
                    note->column = 0;
                    btn = ctrl_pushbutton(s, KT_WINSCP_OPEN_GLOBAL_PANEL, NO_SHORTCUT,
                                          HELPCTX(kitty_winscp_session),
                                          winscp_global_jump_handler, I(0));
                    btn->column = 1;
                    ctrl_columns(s, 1, 100);
                } else {
                    ctrl_text(s, KT_WINSCP_PATH_IS_GLOBAL, HELPCTX(kitty_winscp_session));
                }
            }

            /* FileZilla beside WinSCP: its own protocol and port
             * (FileZillaProtocol / FileZillaPort), the four FileZilla takes.
             * The target is still the shared setting. What is FileZilla's own
             * besides that is how the password reaches it, a user-facing
             * security choice with each consequence stated (kitty_xfer.c
             * StartFileZilla). */
            ctrl_settitle(b, "Connection/SSH/FileZilla", KT_FZ_INTEGRATION);
            s = ctrl_getset(b, "Connection/SSH/FileZilla", "FileZilla", KT_FZ_INTEGRATION);
            xfer_protocol_port_controls(s, XFER_TOOL_FILEZILLA,
                                        HELPCTX(kitty_filezilla_session));
            ctrl_text(s, KT_FZ_TARGET_NOTE, HELPCTX(kitty_filezilla_session));
            ctrl_editbox(s, KT_FZ_ADDITIONAL_OPTIONS, NO_SHORTCUT, 100,
                         HELPCTX(kitty_filezilla_session),
                         conf_editbox_handler, I(CONF_filezilla_options), ED_STR);
            s = ctrl_getset(b, "Connection/SSH/FileZilla", "password", KT_FZ_PASSWORD_GROUP);
            ctrl_radiobuttons(s, KT_FZ_PASSWORD_HOW, NO_SHORTCUT, 1,
                              HELPCTX(kitty_filezilla_session),
                              conf_radiobutton_handler, I(CONF_filezilla_pwmode),
                              KT_FZ_PW_ASK,     NO_SHORTCUT, I(0),
                              KT_FZ_PW_TEMPCFG, NO_SHORTCUT, I(1),
                              KT_FZ_PW_CMDLINE, NO_SHORTCUT, I(2));
            ctrl_text(s, KT_FZ_KEY_PREFERRED, HELPCTX(kitty_filezilla_session));
            {
                dlgcontrol *note, *btn;
                s = ctrl_getset(b, "Connection/SSH/FileZilla", "global", NULL);
                if (GetConfigBoxApplicationSettingsFlag()) {
                    ctrl_columns(s, 2, 62, 38);
                    note = ctrl_text(s, KT_FZ_PATH_IS_GLOBAL, HELPCTX(kitty_filezilla_session));
                    note->column = 0;
                    btn = ctrl_pushbutton(s, KT_FZ_OPEN_GLOBAL_PANEL, NO_SHORTCUT,
                                          HELPCTX(kitty_filezilla_session),
                                          filezilla_global_jump_handler, I(0));
                    btn->column = 1;
                    ctrl_columns(s, 1, 100);
                } else {
                    ctrl_text(s, KT_FZ_PATH_IS_GLOBAL, HELPCTX(kitty_filezilla_session));
                }
            }
        }
    }
}

/* The Connection/Serial panel. */
void scb_panel_serial(struct controlbox *b, bool midsession, int protocol)
{
    struct controlset *s;

    if (DISPLAY_RECONFIGURABLE_PROTOCOL(PROT_SERIAL)) {
        const BackendVtable *ser_vt = backend_vt_from_proto(PROT_SERIAL);

        /*
         * The Connection/Serial panel.
         */
        ctrl_settitle(b, "Connection/Serial",
                      KT_SERIAL_OPTIONS_CONTROLLING_LOCAL_SERIAL_LINES);

        if (!midsession) {
            /*
             * We don't permit switching to a different serial port in
             * midflight, although we do allow all other
             * reconfiguration.
             */
            s = ctrl_getset(b, "Connection/Serial", "serline",
                            KT_SERIAL_SELECT_A_SERIAL_LINE);
            ctrl_editbox(s, KT_SERIAL_SERIAL_LINE_TO_CONNECT, 'l', 40,
                         HELPCTX(serial_line),
                         conf_editbox_handler, I(CONF_serline), ED_STR);
        }

        s = ctrl_getset(b, "Connection/Serial", "sercfg", KT_SERIAL_CONFIGURE_THE_SERIAL_LINE);
        ctrl_editbox(s, KT_SERIAL_SPEED_BAUD, 's', 40,
                     HELPCTX(serial_speed),
                     conf_editbox_handler, I(CONF_serspeed), ED_INT);
        ctrl_editbox(s, KT_SERIAL_DATA_BITS, 'b', 40,
                     HELPCTX(serial_databits),
                     conf_editbox_handler, I(CONF_serdatabits), ED_INT);
        /*
         * Stop bits come in units of one half.
         */
        static const struct conf_editbox_handler_type conf_editbox_stopbits = {
            .type = EDIT_FIXEDPOINT, .denominator = 2};

        ctrl_editbox(s, KT_SERIAL_STOP_BITS, 't', 40,
                     HELPCTX(serial_stopbits),
                     conf_editbox_handler, I(CONF_serstopbits),
                     CP(&conf_editbox_stopbits));
        ctrl_droplist(s, KT_SERIAL_PARITY, 'p', 40,
                      HELPCTX(serial_parity), serial_parity_handler,
                      I(ser_vt->serial_parity_mask));
        ctrl_droplist(s, KT_SERIAL_FLOW_CONTROL, 'f', 40,
                      HELPCTX(serial_flow), serial_flow_handler,
                      I(ser_vt->serial_flow_mask));
    }

}

/* The Connection/SUPDUP, Rlogin and Telnet panels - the rarely used
 * protocols, so they close the Connection subtree after ZModem. */
void scb_panel_other_protocols(struct controlbox *b, bool midsession, int protocol)
{
    struct controlset *s;

    if (DISPLAY_NON_RECONFIGURABLE_PROTOCOL(PROT_SUPDUP)) {
        /*
         * The Connection/SUPDUP panel.
         */
        ctrl_settitle(b, "Connection/SUPDUP",
                      KT_SUPDUP_OPTIONS_CONTROLLING_SUPDUP_CONNECTIONS);

        /* KiTTY: say what this protocol IS, in one line with the help behind
         * it. The panel is inherited from upstream and sits in the tree beside
         * Telnet and Rlogin, where it reads as something a person might
         * plausibly need - and then every option on it ("Location string",
         * "WAITS", "**MORE** processing") means nothing without knowing it is a
         * 1970s PDP-10 protocol. The manual already answers that (using-supdup),
         * so the line points there rather than repeating it. */
        s = ctrl_getset(b, "Connection/SUPDUP", "what", NULL);
        ctrl_text(s, KT_SUPDUP_WHAT, HELPCTX(using_supdup));

        s = ctrl_getset(b, "Connection/SUPDUP", "main", NULL);

        ctrl_editbox(s, KT_SUPDUP_LOCATION_STRING, 'l', 70,
                     HELPCTX(supdup_location),
                     conf_editbox_handler, I(CONF_supdup_location),
                     ED_STR);

        ctrl_radiobuttons(s, KT_SUPDUP_EXTENDED_ASCII_CHARACTER_SET, 'e', 4,
                          HELPCTX(supdup_ascii),
                          conf_radiobutton_handler,
                          I(CONF_supdup_ascii_set),
                          KT_LOGGING_NONE, I(SUPDUP_CHARSET_ASCII),
                          KT_SUPDUP_ITS, I(SUPDUP_CHARSET_ITS),
                          KT_SUPDUP_WAITS, I(SUPDUP_CHARSET_WAITS));

        ctrl_checkbox(s, KT_SUPDUP_MORE_PROCESSING, 'm',
                      HELPCTX(supdup_more),
                      conf_checkbox_handler,
                      I(CONF_supdup_more));

        ctrl_checkbox(s, KT_SUPDUP_TERMINAL_SCROLLING, 's',
                      HELPCTX(supdup_scroll),
                      conf_checkbox_handler,
                      I(CONF_supdup_scroll));
    }
    if (DISPLAY_NON_RECONFIGURABLE_PROTOCOL(PROT_RLOGIN)) {
        /*
         * The Connection/Rlogin panel.
         */
        ctrl_settitle(b, "Connection/Rlogin",
                      KT_RLOGIN_OPTIONS_CONTROLLING_RLOGIN_CONNECTIONS);

        s = ctrl_getset(b, "Connection/Rlogin", "data",
                        KT_DATA_DATA_TO_SEND);
        ctrl_editbox(s, KT_RLOGIN_LOCAL_USERNAME, 'l', 50,
                     HELPCTX(rlogin_localuser),
                     conf_editbox_handler, I(CONF_localusername), ED_STR);

    }

    if (DISPLAY_RECONFIGURABLE_PROTOCOL(PROT_TELNET)) {
        /*
         * The Connection/Telnet panel.
         */
        ctrl_settitle(b, "Connection/Telnet",
                      KT_TELNET_OPTIONS_CONTROLLING_TELNET_CONNECTIONS);

        s = ctrl_getset(b, "Connection/Telnet", "protocol",
                        KT_TELNET_TELNET_PROTOCOL_ADJUSTMENTS);

        if (!midsession) {
            ctrl_radiobuttons(s, KT_TELNET_HANDLING_OF_OLD_ENVIRON_AMBIGUITY,
                              NO_SHORTCUT, 2,
                              HELPCTX(telnet_oldenviron),
                              conf_radiobutton_bool_handler,
                              I(CONF_rfc_environ),
                              KT_TELNET_BSD_COMMONPLACE, 'b', I(false),
                              KT_TELNET_RFC_1408_UNUSUAL, 'f', I(true));
            ctrl_radiobuttons(s, KT_TELNET_TELNET_NEGOTIATION_MODE, 't', 2,
                              HELPCTX(telnet_passive),
                              conf_radiobutton_bool_handler,
                              I(CONF_passive_telnet),
                              KT_TELNET_PASSIVE, I(true), KT_TELNET_ACTIVE, I(false));
        }
        ctrl_checkbox(s, KT_TELNET_KEYBOARD_SENDS_TELNET_SPECIAL_COMMANDS, 'k',
                      HELPCTX(telnet_specialkeys),
                      conf_checkbox_handler,
                      I(CONF_telnet_keyboard));
        ctrl_checkbox(s, KT_TELNET_RETURN_KEY_SENDS_TELNET_NEW,
                      'm', HELPCTX(telnet_newline),
                      conf_checkbox_handler,
                      I(CONF_telnet_newline));
    }

}

/* The settings tree's table-driven handler, defined further down with the
 * KiTTY++ Settings leaves; the ZModem panel borrows it for the global switch. */
struct kset_key;

/* "Locate..." beside the session's local download folder: the folder picker
 * opens on the folder that applies today, the pick is stored like a typed
 * value and the box (the button's context) refreshed. */

/* The session's download folder as a folder row (label, box and Locate...
 * on ONE aligned line - the file-select control with FILTER_FOLDERS), its
 * Filename mapped to the plain string the setting is. */
/* The "Global: ..." line beneath the folder row. Captured so the folder
 * row's EVENT_REFRESH - sent every time the panel is shown - can restate the
 * global folder, which the Transfers & Tools leaf may have changed since the
 * panel was built. */
static dlgcontrol *g_xfer_global_ctrl = NULL;         /* download */
static dlgcontrol *g_xfer_global_upload_ctrl = NULL;  /* upload */

static void xfer_global_line(char *buf, size_t len)
{
    char gdir[4096];
    gdir[0] = '\0';
    ReadParameterN(INIT_SECTION, KI_DOWNLOADDIR, gdir, sizeof(gdir));
    snprintf(buf, len, KT_TRANSFERS_GLOBAL_IS,
             gdir[0] ? gdir : KT_TRANSFERS_GLOBAL_UNSET);
}

static void xfer_global_upload_line(char *buf, size_t len)
{
    char gdir[4096];
    gdir[0] = '\0';
    ReadParameterN(INIT_SECTION, KI_UPLOADDIR, gdir, sizeof(gdir));
    snprintf(buf, len, KT_TRANSFERS_GLOBAL_IS,
             gdir[0] ? gdir : KT_TRANSFERS_GLOBAL_UPLOAD_UNSET);
}

static void xfer_downloaddir_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                     void *data, int event)
{
    Conf *conf = (Conf *)data;
    if (event == EVENT_REFRESH) {
        Filename *fn = filename_from_str(conf_get_str(conf, CONF_zdownloaddir));
        dlg_filesel_set(ctrl, dlg, fn);
        filename_free(fn);
        if (g_xfer_global_ctrl) {
            char line[4200];
            xfer_global_line(line, sizeof(line));
            dlg_label_change(g_xfer_global_ctrl, dlg, line);
        }
    } else if (event == EVENT_VALCHANGE) {
        Filename *fn = dlg_filesel_get(ctrl, dlg);
        conf_set_str(conf, CONF_zdownloaddir, filename_to_str(fn));
        filename_free(fn);
    }
}

/* The upload folder row: the download row's twin, on zUploadDir. */
static void xfer_uploaddir_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                   void *data, int event)
{
    Conf *conf = (Conf *)data;
    if (event == EVENT_REFRESH) {
        Filename *fn = filename_from_str(conf_get_str(conf, CONF_zuploaddir));
        dlg_filesel_set(ctrl, dlg, fn);
        filename_free(fn);
        if (g_xfer_global_upload_ctrl) {
            char line[4200];
            xfer_global_upload_line(line, sizeof(line));
            dlg_label_change(g_xfer_global_upload_ctrl, dlg, line);
        }
    } else if (event == EVENT_VALCHANGE) {
        Filename *fn = dlg_filesel_get(ctrl, dlg);
        conf_set_str(conf, CONF_zuploaddir, filename_to_str(fn));
        filename_free(fn);
    }
}

/* "Max transfer size (MB)": the box shows the session's own value, or
 * nothing while the session follows the global default (-1). An emptied
 * box goes back to the global default; 0 = no limit. */
/* The global "Max transfer size" line beneath the session field: the folder
 * rows' "Global: ..." line for [KiTTY] transfermaxmb (empty = 1024, the
 * default kitty_transfer.c applies; 0 = no limit). */
static dlgcontrol *g_xfer_global_maxmb_ctrl = NULL;

static void xfer_global_maxmb_line(char *buf, size_t len)
{
    char v[64];
    int mb = 1024;
    v[0] = '\0';
    if (ReadParameterN(INIT_SECTION, KI_TRANSFERMAXMB, v, sizeof(v)) && v[0])
        mb = atoi(v);
    if (mb <= 0)
        snprintf(buf, len, "%s", KT_TRANSFERS_GLOBAL_MAX_MB_NONE);
    else
        snprintf(buf, len, KT_TRANSFERS_GLOBAL_MAX_MB, mb);
}

static void xfer_maxmb_handler(dlgcontrol *ctrl, dlgparam *dlg,
                               void *data, int event)
{
    Conf *conf = (Conf *)data;
    if (event == EVENT_REFRESH) {
        char buf[32];
        int v = conf_get_int(conf, CONF_xfer_max_mb);
        if (v < 0)
            buf[0] = '\0';
        else
            sprintf(buf, "%d", v);
        dlg_editbox_set(ctrl, dlg, buf);
        if (g_xfer_global_maxmb_ctrl) {
            char line[128];
            xfer_global_maxmb_line(line, sizeof(line));
            dlg_label_change(g_xfer_global_maxmb_ctrl, dlg, line);
        }
    } else if (event == EVENT_VALCHANGE) {
        char *s = dlg_editbox_get(ctrl, dlg);
        char *p = s;
        while (*p == ' ') p++;
        if (!*p)
            conf_set_int(conf, CONF_xfer_max_mb, -1);
        else {
            int v = atoi(p);
            conf_set_int(conf, CONF_xfer_max_mb, v < 0 ? 0 : v);
        }
        sfree(s);
    }
}
static void xfer_fullpath_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                  void *data, int event)
{
    Conf *conf = (Conf *)data;
    if (event == EVENT_REFRESH) {
        int v = conf_get_int(conf, CONF_xfer_full_path);
        if (v < 0) {
            const struct kset_key *k = kset_find(KI_TRANSFERFULLPATH);
            v = k ? kset_get_int(k) : 0;
        }
        dlg_checkbox_set(ctrl, dlg, v != 0);
    } else if (event == EVENT_VALCHANGE) {
        conf_set_int(conf, CONF_xfer_full_path, dlg_checkbox_get(ctrl, dlg) ? 1 : 0);
    }
}

/* The Connection/Transfers panel (KiTTY++): where received files go and
 * where files are sent from, the remote directory uploads go to, the
 * settings for transfers over the session (OSC 5113, kitten transfer) and
 * which Tools menu entries the session shows. Top level under Connection
 * because ZModem and file transfers over the session work on telnet and
 * serial sessions too - a destination shared by them cannot live under
 * SSH. kscp Get file, Send File, ZModem and kitten transfer all read these
 * values. One help context per group, so F1 lands on the group's own
 * paragraph. */
void scb_panel_transfers(struct controlbox *b)
{
    struct controlset *s;
    char line[4200];

    if (GetPuttyFlag()) return;

    ctrl_settitle(b, "Connection/Transfers", KT_TRANSFERS_TITLE);

    /* First, because it says WHICH HOST the tool-specific matter below is
     * about. Moved off the WinSCP panel: it serves FileZilla too, and a
     * setting for two tools cannot sit on one of their leaves. Stored as
     * SFTPConnect. A port inside the string wins over the tool's Port field
     * (kitty_xfer.c). */
    s = ctrl_getset(b, "Connection/Transfers", "target", KT_TRANSFERS_TARGET_GROUP);
    ctrl_editbox(s, KT_TRANSFERS_TARGET_OVERRIDE, NO_SHORTCUT, 100,
                 HELPCTX(kitty_transfers),
                 conf_editbox_handler, I(CONF_sftpconnect), ED_STR);
    /* Two lines reserved: the note wraps at the narrower panel widths. */
    ctrl_text(s, KT_TRANSFERS_TARGET_NOTE,
              HELPCTX(kitty_transfers))->text.lines = 2;

    s = ctrl_getset(b, "Connection/Transfers", "received", KT_TRANSFERS_RECEIVED);
    kitty_controls_set_dir_picker(OpenDirNameFrom);   /* the rows' Locate... */
    ctrl_filesel(s, KT_TRANSFERS_LOCAL_DOWNLOAD_FOLDER, NO_SHORTCUT,
                 FILTER_FOLDERS, false, NULL, HELPCTX(kitty_transfers_folders),
                 xfer_downloaddir_handler, P(NULL));
    /* The global folder, read when the panel is built and again at every
     * EVENT_REFRESH of the folder row above, so the value that already
     * applies is visible before it is overridden and stays current when the
     * panel is shown again. Two lines reserved: the folder set later may be
     * longer than the one the line was built with, and the static wraps
     * only into the height it has. */
    xfer_global_line(line, sizeof(line));
    g_xfer_global_ctrl = ctrl_text(s, line, HELPCTX(kitty_transfers_folders));
    g_xfer_global_ctrl->text.lines = 2;
    /* The upload folder: where Send File opens and where a name the far end
     * asks to read is looked up. Same shape, same refresh. */
    ctrl_filesel(s, KT_TRANSFERS_UPLOAD_FOLDER, NO_SHORTCUT,
                 FILTER_FOLDERS, false, NULL, HELPCTX(kitty_transfers_folders),
                 xfer_uploaddir_handler, P(NULL));
    xfer_global_upload_line(line, sizeof(line));
    g_xfer_global_upload_ctrl = ctrl_text(s, line, HELPCTX(kitty_transfers_folders));
    g_xfer_global_upload_ctrl->text.lines = 2;

    /* Moved from the KSCP panel: the two are mutually exclusive, the
     * handlers keep them so through the captured sibling controls. */
    s = ctrl_getset(b, "Connection/Transfers", "remote", KT_TRANSFERS_REMOTE_DIR);
    g_osc7_track_ctrl = ctrl_checkbox(s,
                  KT_KSCP_TRACK_REMOTE_DIRECTORY_OSC_7,
                  NO_SHORTCUT, HELPCTX(kitty_transfers_remote),
                  kitty_osc7_track_handler, P(NULL));
    ctrl_text(s, KT_KSCP_OSC7_NEEDS_SHELL, HELPCTX(kitty_transfers_remote));
    ctrl_text(s, KT_KSCP_DRAG_DROP_UPLOADS_AND_WINSCP, HELPCTX(kitty_transfers_remote));
    g_pscp_remotedir_ctrl = ctrl_editbox(s,
                 KT_KSCP_FIXED_REMOTE_UPLOAD_DIRECTORY, NO_SHORTCUT, 100,
                 HELPCTX(kitty_transfers_remote),
                 kitty_pscp_remotedir_handler, P(NULL), P(NULL));
    ctrl_text(s, KT_KSCP_ALWAYS_UPLOAD_HERE_INSTEAD_MUTUALLY, HELPCTX(kitty_transfers_remote));

    /* Transfers over the session (kitten transfer): the permission, the
     * save dialog, the per-file ceiling for files arriving and the full-path
     * rule for files leaving. The last two default to the OSC 5113 (kitten)
     * leaf under Transfers & Tools. */
    s = ctrl_getset(b, "Connection/Transfers", "kitten", KT_TRANSFERS_KITTEN);
    ctrl_text(s, KT_TRANSFERS_FAR_END_NOTE, HELPCTX(kitty_transfers_kitten));
    /* Two columns: three abreast cut "Ask once per session" at 200%. */
    ctrl_radiobuttons(s, NULL, NO_SHORTCUT, 2,
                      HELPCTX(kitty_transfers_kitten),
                      conf_radiobutton_handler, I(CONF_xfer_permission),
                      KT_TRANSFERS_PERM_ALWAYS, NO_SHORTCUT, I(0),
                      KT_TRANSFERS_PERM_FIRST,  NO_SHORTCUT, I(1),
                      KT_TRANSFERS_PERM_NEVER,  NO_SHORTCUT, I(2));
    ctrl_checkbox(s, KT_TRANSFERS_ALWAYS_ASK_DESTINATION, NO_SHORTCUT,
                  HELPCTX(kitty_transfers_kitten),
                  conf_checkbox_handler, I(CONF_xfer_ask_destination));
    ctrl_editbox(s, KT_TRANSFERS_MAX_MB, NO_SHORTCUT, 25,
                 HELPCTX(kitty_transfers_kitten),
                 xfer_maxmb_handler, P(NULL), P(NULL));
    /* The global value, read at build time and at every EVENT_REFRESH of
     * the field above, like the folder rows' lines. */
    xfer_global_maxmb_line(line, sizeof(line));
    g_xfer_global_maxmb_ctrl = ctrl_text(s, line, HELPCTX(kitty_transfers_kitten));
    ctrl_text(s, KT_TRANSFERS_MAX_MB_NOTE, HELPCTX(kitty_transfers_kitten));
    ctrl_checkbox(s, KT_TRANSFERS_FULL_PATH, NO_SHORTCUT,
                  HELPCTX(kitty_transfers_kitten),
                  xfer_fullpath_handler, P(NULL));

}

/* The Connection/ZModem panels (KiTTY). */
void scb_panel_zmodem(struct controlbox *b)
{
#ifdef MOD_ZMODEM
    struct controlset *s;

    /* The panel is ALWAYS there, and opens with the global switch
     * ([KiTTY] zmodem): the switch used to hide the very panel it would sit
     * on, which left no place to turn it back on. With the switch off the
     * session settings below are not built - they are declared into the
     * controlbox once, so the next configuration window shows them. */
    if (!GetPuttyFlag()) {
        ctrl_settitle(b, "Connection/ZModem",
                      KT_ZMODEM_OPTIONS_CONTROLLING_Z_MODEM_TRANSFERS);
        s = ctrl_getset(b, "Connection/ZModem", "global", NULL);
        ctrl_checkbox(s, KT_ZMODEM_GLOBAL_ENABLE, NO_SHORTCUT,
                      HELPCTX(kitty_zmodem), kitty_kset_handler,
                      P((void *)kset_find(KI_ZMODEM)));
        if (!GetZModemFlag())
            ctrl_text(s, KT_ZMODEM_GLOBAL_OFF_NOTE, HELPCTX(kitty_zmodem));
    }

    /* The Connection/ZModem panels (KiTTY). Backend = kitty_zmodem_*. */
    if ((!GetPuttyFlag()) && GetZModemFlag()) {
        /* The download folder moved to Connection > Transfers, where every
         * receiving feature reads it (same key, zDownloadDir). */
        s = ctrl_getset(b, "Connection/ZModem", "download",
                        KT_ZMODEM_DOWNLOAD_FOLDER);
        ctrl_text(s, KT_ZMODEM_FOLDER_ON_TRANSFERS, HELPCTX(kitty_zmodem));

        s = ctrl_getset(b, "Connection/ZModem", "receive",
                        KT_ZMODEM_RECEIVE_COMMAND_RZ);
        ctrl_editbox(s, KT_ZMODEM_OPTIONS, NO_SHORTCUT, 50,
                     HELPCTX(kitty_zmodem),
                     conf_editbox_handler, I(CONF_rzoptions), ED_STR);
        ctrl_text(s, KT_ZMODEM_CTRL_X_TO_QUIT_RZ,
                  HELPCTX(kitty_zmodem));

        s = ctrl_getset(b, "Connection/ZModem", "send",
                        KT_ZMODEM_SEND_COMMAND_SZ);
        ctrl_editbox(s, KT_ZMODEM_OPTIONS, NO_SHORTCUT, 50,
                     HELPCTX(kitty_zmodem),
                     conf_editbox_handler, I(CONF_szoptions), ED_STR);
    }
#else
    (void)b;
#endif
}

/* The Comment panel (KiTTY): a free-text note attached to this session. */
void scb_panel_comment(struct controlbox *b)
{
    struct controlset *s;

    /*
     * The Comment panel (KiTTY): a free-text note attached to this session.
     * The FIRST leaf under Session - the note about a session leads its
     * subtree. (Classic KiTTY had it as a top-level category.)
     */
    if (!GetPuttyFlag()) {
        ctrl_settitle(b, "Session/Comment", KT_COMMENT_COMMENT_FOR_THIS_SESSION);
        s = ctrl_getset(b, "Session/Comment", "main", NULL);
        /* Multiline (~5 lines). Newlines round-trip to storage: REG_SZ holds
         * CRLF directly, and file/dir mode mungestr()-encodes control chars. */
        ctrl_editbox_multiline(s, KT_COMMENT_SESSION_COMMENT, NO_SHORTCUT, 5, false,
                               HELPCTX(kitty_comment), conf_editbox_handler,
                               I(CONF_comment), ED_STR);
        /* Printed into the terminal, framed, once, before the connection
         * starts - see kitty_print_session_comment. */
        ctrl_checkbox(s, KT_COMMENT_NOTIFY, NO_SHORTCUT, HELPCTX(kitty_comment),
                      conf_checkbox_handler, I(CONF_comment_notify));
    }
}
