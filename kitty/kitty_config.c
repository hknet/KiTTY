/*
 * config.c - the platform-independent parts of the PuTTY
 * configuration box.
 */

#include <assert.h>
#include <stdlib.h>

#include "putty.h"
#include "dialog.h"
#include "storage.h"
#include "tree234.h"
#ifndef MOD_NOREPEAT
#define MOD_NOREPEAT 0x4000
#endif
#ifdef MOD_PERSO
#include "kitty_proxy.h"   /* proxy-choice droplist: proxies[], GetProxySelectionFlag, MAX_PROXY */
#include "kitty_defs.h"    /* KITTY_DEFAULT_SESSION */
#endif

#ifdef MOD_PERSO
/* KiTTY config-box additions. This is a kitty-owned copy of config.c
 * compiled with MOD_PERSO into the kitty target; it overrides the
 * shared guiterminal config.o (which is built without MOD_PERSO) so
 * the other shipping binaries are unaffected. */
int GetPuttyFlag(void);
int GetSessionFilterFlag(void);  /* kitty.c: [ConfigBox] filter, gates the
                                  * live type-to-search session filter */
int GetTransparencyFlag(void);
int GetZModemFlag(void);
int GetAutoreconnectFlag(void);
int GetBackgroundImageFlag(void);
extern void RunConfig(Conf *conf);   /* kitty_launcher.c: launch new session, keep box open */
extern int GetDblClickFlag(void);    /* kitty.c: [ConfigBox] dblclick - 0 open here, 1 start in new window */
extern char **FolderList;            /* kitty.c: NULL-terminated folder names */
extern char CurrentFolder[];         /* kitty_commun.c: currently selected folder */
void GetSessionFolderName(const char *session_in, char *folder);  /* kitty.c */
int kitty_session_origin(const char *sessionname);   /* windows/storage.c: 0=ours,1=old KiTTY,2=PuTTY */
void kitty_set_last_session(const char *sessionname); /* windows/storage.c */
int  kitty_get_last_session(char *buf, int buflen);   /* windows/storage.c */
void kitty_set_last_folder(const char *folder);       /* windows/storage.c */
int  kitty_get_last_folder(char *buf, int buflen);    /* windows/storage.c */
/* KiTTY folder-management engine (kitty_config.c does not include kitty_tools.h/kitty.h) */
int StringList_Add(char **list, const char *name);   /* kitty_tools.c (dedupes internally) */
void StringList_Del(char **list, const char *name);  /* kitty_tools.c */
void StringList_Up(char **list, const char *name);   /* kitty_tools.c */
void InitFolderList(void);                            /* kitty.c */
void SaveFolderList(void);                            /* kitty.c */
void CleanFolderName(char *folder);                   /* kitty_commun.c */
/* Selects an editable combo's whole text so the next keystroke replaces it;
 * see the implementation comment in windows/controls.c for why the field is
 * not simply emptied instead. */
void kitty_dlg_combobox_select_all(dlgcontrol *ctrl, dlgparam *dp);

#define KITTY_LAUNCHER_REFRESH_MESSAGE "KiTTYLauncherRefreshSessionsAndHotkeys"

static void kitty_notify_launcher_sessions_changed(void)
{
    UINT msg = RegisterWindowMessageA(KITTY_LAUNCHER_REFRESH_MESSAGE);
    if (msg)
        PostMessageA(HWND_BROADCAST, msg, 0, 0);
}

/* Checkbox handler for KiTTY keys that are stored as INT (0/1) rather
 * than BOOL (the standard conf_checkbox_handler asserts on INT keys in
 * 0.84). Context is the CONF_ key. */
static void kitty_checkbox_int_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                       void *data, int event)
{
    int key = ctrl->context.i;
    Conf *conf = (Conf *)data;
    if (event == EVENT_REFRESH)
        dlg_checkbox_set(ctrl, dlg, conf_get_int(conf, key) != 0);
    else if (event == EVENT_VALCHANGE)
        conf_set_int(conf, key, dlg_checkbox_get(ctrl, dlg) ? 1 : 0);
}

/* Auto-login password editbox handler. Behaves like the stock ED_STR editbox,
 * but the first time the field is made non-empty in a dialog session it shows
 * a one-time security consent (the password is stored reversibly-encrypted; SSH
 * keys are recommended). Declining clears the field. Consent happens HERE, at
 * configuration time, so the auto-login itself stays silent at connect time. */
int kitty_autopw_warn(void);   /* kitty_win.c */
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
char *kitty_loginscript_to_text(const char *stored);     /* kitty.c */
char *kitty_loginscript_from_text(const char *text);     /* kitty.c */

/* The login-script box, captured at build time so the "Load from file..." button
 * beside it can fill it in. Same trick as g_autopw_ctrl below. */
static dlgcontrol *g_loginscript_ctrl = NULL;
int OpenFileName(HWND hFrame, char *filename, char *Title, char *Filter); /* kitty_win.c */

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
        dlg_error_msg(dlg, "The login script box is not available.");
        return;
    }
    {
        char path[4096];
        path[0] = '\0';
        if (!OpenFileName(GetActiveWindow(), path,
                          "Select a login script file",
                          "Script files (*.txt;*.ksc)|*.txt;*.ksc|All files (*.*)|*.*|"))
            return;                       /* cancelled */
        {
            FILE *fp = fopen(path, "rb");
            strbuf *sb;
            char line[4096];
            if (!fp) {
                dlg_error_msg(dlg, "That file could not be opened.");
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
         * and read the field as UTF-8 rather than the system codepage. */
        dlg_editbox_set_utf8(ctrl, dlg, conf_get_str(conf, CONF_password));
    } else if (event == EVENT_VALCHANGE) {
        char *val = dlg_editbox_get_utf8(ctrl, dlg);
        /* Warn only when a password is being SET where conf currently has none
         * (i.e. a genuinely new auto-login password). Editing a session that
         * already has a stored password leaves conf non-empty, so no warning -
         * this also covers the re-entrant VALCHANGE that dlg_editbox_set fires
         * during EVENT_REFRESH (conf already holds the loaded password then). */
        if (strlen(val) > 0 &&
            strlen(conf_get_str(conf, CONF_password)) == 0) {
            if (!kitty_autopw_warn()) {
                /* Declined: clear the field and do not store. */
                dlg_editbox_set(ctrl, dlg, "");
                conf_set_str(conf, CONF_password, "");
                sfree(val);
                return;
            }
        }
        conf_set_str(conf, CONF_password, val);
        sfree(val);
    }
}

/* "Show password" checkbox: unmasks the auto-login password editbox so the user
 * can verify the stored value. g_autopw_ctrl is the password editbox, captured
 * when the Connection/Data panel is built. */
static dlgcontrol *g_autopw_ctrl = NULL;
static void kitty_showpw_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                 void *data, int event)
{
    if (event == EVENT_REFRESH) {
        dlg_checkbox_set(ctrl, dlg, false);     /* default masked on (re)open */
        if (g_autopw_ctrl)
            dlg_editbox_set_masked(g_autopw_ctrl, dlg, false);
    } else if (event == EVENT_VALCHANGE) {
        if (g_autopw_ctrl)
            dlg_editbox_set_masked(g_autopw_ctrl, dlg, dlg_checkbox_get(ctrl, dlg));
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
static char *kitty_cfg_trim(char *s)
{
    char *e;
    while (*s == ' ' || *s == '\t') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
        *--e = '\0';
    return s;
}

static bool kitty_cfg_parse_hotkey(char *spec, UINT *mods, UINT *vk)
{
    char *tok, *keytok = NULL;
    *mods = 0; *vk = 0;
    spec = kitty_cfg_trim(spec);
    if (!*spec) return false;
    for (tok = strtok(spec, "+"); tok != NULL; tok = strtok(NULL, "+")) {
        tok = kitty_cfg_trim(tok);
        if (!stricmp(tok, "Ctrl") || !stricmp(tok, "Control")) *mods |= MOD_CONTROL;
        else if (!stricmp(tok, "Shift")) *mods |= MOD_SHIFT;
        else if (!stricmp(tok, "Alt")) *mods |= MOD_ALT;
        else if (!stricmp(tok, "Win") || !stricmp(tok, "Windows")) *mods |= MOD_WIN;
        else keytok = tok;
    }
    if (keytok == NULL) return false;
    if (strlen(keytok) == 1) {
        char c = keytok[0];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) *vk = (UINT)c;
    } else if ((keytok[0] == 'F' || keytok[0] == 'f') && keytok[1] >= '1' && keytok[1] <= '9') {
        int n = atoi(keytok + 1);
        if (n >= 1 && n <= 24) *vk = VK_F1 + n - 1;
    }
    return (*mods != 0 && *vk != 0);
}

/* KiTTY: open the modeless window-title placeholder reference (kitty_win.c).
 * Owned by the active window - the configuration box - so it stacks with it
 * rather than getting lost behind it. */
static void kitty_title_placeholders_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                             void *data, int event)
{
    extern void kitty_show_title_placeholders(HWND owner);
    (void)ctrl; (void)dlg; (void)data;
    if (event != EVENT_ACTION) return;
    kitty_show_title_placeholders(GetActiveWindow());
}

static void kitty_launcher_hotkey_check_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                                void *data, int event)
{
    Conf *conf = (Conf *)data;
    char work[256];
    UINT mods, vk;
    (void)ctrl; (void)dlg;
    if (event != EVENT_ACTION) return;
    strncpy(work, conf_get_str(conf, CONF_launcher_global_hotkey), sizeof(work)-1);
    work[sizeof(work)-1] = '\0';
    if (!kitty_cfg_parse_hotkey(work, &mods, &vk)) {
        MessageBox(NULL, "Enter a hotkey such as Ctrl+Alt+K or Ctrl+Shift+F12.",
                   "KiTTY Launcher hotkey", MB_OK | MB_ICONWARNING);
        return;
    }
    if (RegisterHotKey(NULL, 0x4B7A, mods | MOD_NOREPEAT, vk)) {
        UnregisterHotKey(NULL, 0x4B7A);
        MessageBox(NULL, "This hotkey is currently available.\n\nNote: it is only registered while KiTTY Launcher is running.",
                   "KiTTY Launcher hotkey", MB_OK | MB_ICONINFORMATION);
    } else {
        MessageBox(NULL, "This hotkey is already in use or reserved by Windows/another app.\n\nWindows does not expose which application owns a global hotkey.",
                   "KiTTY Launcher hotkey", MB_OK | MB_ICONWARNING);
    }
}
#endif

/* Proxy-choice droplist (KiTTY): lists named proxy definitions (plus the two
 * built-ins "- Session defined proxy -" / "- No proxy -") and stores the chosen
 * name in CONF_proxyselection, which kitty_proxy_select() overlays onto the
 * session's proxy settings at connect time. */
static void kitty_proxy_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                void *data, int event)
{
    Conf *conf = (Conf *)data;
    if (event == EVENT_REFRESH) {
        kitty_proxy_resolve_selection(conf);   /* drop a deleted proxy ref, etc. */
        const char *cur = conf_get_str(conf, CONF_proxyselection);
        int i, sel = 0;
        dlg_update_start(ctrl, dlg);
        dlg_listbox_clear(ctrl, dlg);
        for (i = 0; i < MAX_PROXY && proxies[i].name; i++) {
            dlg_listbox_add(ctrl, dlg, proxies[i].name);
            if (cur && !strcmp(cur, proxies[i].name)) sel = i;
        }
        dlg_listbox_select(ctrl, dlg, sel);
        dlg_update_done(ctrl, dlg);
    } else if (event == EVENT_SELCHANGE) {
        int i = dlg_listbox_index(ctrl, dlg);
        if (i >= 0 && i < MAX_PROXY && proxies[i].name)
            conf_set_str(conf, CONF_proxyselection, proxies[i].name);
    }
}

/* "Edit" button beside the Proxy-choice droplist (and at the foot of the
 * Connection/Proxy panel): open the named-proxy editor, then refresh so the
 * droplist re-lists the current set. Note: whether the Session-panel droplist
 * *exists* is fixed when the config box is built (kitty_proxy_choice_shown() in
 * auto mode), so crossing 0<->1 proxies only takes effect on the next config-box
 * open; the editor shows a one-time "reopen the configuration" note in that
 * case (kitty_proxy_gui.c). Making it live is a scoped follow-up (would require
 * rebuilding the whole ctrlbox). */
static void kitty_proxyedit_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                    void *data, int event)
{
    if (event == EVENT_ACTION) {
        extern int kitty_proxy_edit_dialog(HWND);
        if (kitty_proxy_edit_dialog(GetActiveWindow())) {
            dlg_refresh(NULL, dlg);
            /* A proxy was added / edited / deleted. Back up the config store now
             * (registry: kitty084.sav + rotation; portable: dated Backups\
             * folder) - a proxy change on its own may never be followed by
             * opening a session, which is the other backup trigger. */
            { extern void SaveRegistryKey(void); SaveRegistryKey(); }
        }
    }
}

/* WinSCP executable path (KiTTY): this is a GLOBAL app setting in kitty.ini
 * [KiTTY] WinSCPPath, NOT a per-session CONF_ key - so it cannot use
 * conf_filesel_handler. On REFRESH we show the stored path, or, if none is
 * stored yet, the auto-detected default as a display hint (we never WRITE on
 * refresh). On VALCHANGE we persist whatever the user selected/typed. Mirrors
 * the resolution order in SearchWinSCP() (kitty.c). */
void SaveRegistryKeyNow(void);   /* kitty.c - blocking config backup */
int ReadParameter(const char *key, const char *name, char *value);   /* kitty.c */
int ReadParameterN(const char *key, const char *name, char *value, size_t size); /* kitty.c */
int WriteParameter(const char *key, const char *name, char *value);  /* kitty.c */
int existfile(const char *filename);                                  /* kitty_tools.c */
/* kitty.ini [section] name (kitty_config.c does not include kitty.h). Mirror
 * the MOD_PERSO definition there so the two never drift. */
#ifndef INIT_SECTION
#define INIT_SECTION "KiTTY"
#endif
static void kitty_winscppath_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                     void *data, int event)
{
    /* dlg_editbox_set() fires a re-entrant EVENT_VALCHANGE (see the autopw
     * handler note above); this guard stops the refresh-time hint from being
     * written back to kitty.ini, so we only persist genuine user edits. */
    static int refreshing = 0;
    if (event == EVENT_REFRESH) {
        char buffer[4096];
        buffer[0] = '\0';
        refreshing = 1;
        if (ReadParameterN(INIT_SECTION, "WinSCPPath", buffer, sizeof(buffer)) == 0 ||
            !buffer[0]) {
            /* Nothing stored: offer the default location as a hint, but only
             * if it actually exists (display only - do not persist here). */
            const char *pf = getenv("ProgramFiles");
            const char *pf86 = getenv("ProgramFiles(x86)");
            const char *local = getenv("LOCALAPPDATA");
            buffer[0] = '\0';
            if (pf) {
                snprintf( buffer, sizeof(buffer), "%s\\WinSCP\\WinSCP.exe", pf);
                if (!existfile(buffer))
                    buffer[0] = '\0';
            }
            if (!buffer[0] && pf86) {
                snprintf( buffer, sizeof(buffer), "%s\\WinSCP\\WinSCP.exe", pf86);
                if (!existfile(buffer))
                    buffer[0] = '\0';
            }
            if (!buffer[0] && local) {
                snprintf( buffer, sizeof(buffer), "%s\\Programs\\WinSCP\\WinSCP.exe", local);
                if (!existfile(buffer))
                    buffer[0] = '\0';
            }
        }
        {
            Filename *fn = filename_from_str(buffer);
            dlg_filesel_set(ctrl, dlg, fn);
            filename_free(fn);
        }
        refreshing = 0;
    } else if (event == EVENT_VALCHANGE) {
        Filename *fn;
        char val[4096];
        if (refreshing)
            return;
        fn = dlg_filesel_get(ctrl, dlg);
        snprintf(val, sizeof(val), "%s", filename_to_str(fn));
        WriteParameter(INIT_SECTION, "WinSCPPath", val);
        filename_free(fn);
    }
}
#endif

#define PRINTER_DISABLED_STRING "None (printing disabled)"

#define HOST_BOX_TITLE "Host Name (or IP address)"
#define PORT_BOX_TITLE "Port"

void conf_radiobutton_handler(dlgcontrol *ctrl, dlgparam *dlg,
                              void *data, int event)
{
    int button;
    Conf *conf = (Conf *)data;

    /*
     * For a standard radio button set, the context parameter gives
     * the primary key (CONF_foo), and the extra data per button
     * gives the value the target field should take if that button
     * is the one selected.
     */
    if (event == EVENT_REFRESH) {
        int val = conf_get_int(conf, ctrl->context.i);
        for (button = 0; button < ctrl->radio.nbuttons; button++)
            if (val == ctrl->radio.buttondata[button].i)
                break;
        /* We expected that `break' to happen, in all circumstances. */
        assert(button < ctrl->radio.nbuttons);
        dlg_radiobutton_set(ctrl, dlg, button);
    } else if (event == EVENT_VALCHANGE) {
        button = dlg_radiobutton_get(ctrl, dlg);
        assert(button >= 0 && button < ctrl->radio.nbuttons);
        conf_set_int(conf, ctrl->context.i,
                     ctrl->radio.buttondata[button].i);
    }
}

void conf_radiobutton_bool_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                   void *data, int event)
{
    int button;
    Conf *conf = (Conf *)data;

    /*
     * Same as conf_radiobutton_handler, but using conf_set_bool in
     * place of conf_set_int, because it's dealing with a bool-typed
     * config option.
     */
    if (event == EVENT_REFRESH) {
        int val = conf_get_bool(conf, ctrl->context.i);
        for (button = 0; button < ctrl->radio.nbuttons; button++)
            if (val == ctrl->radio.buttondata[button].i)
                break;
        /* We expected that `break' to happen, in all circumstances. */
        assert(button < ctrl->radio.nbuttons);
        dlg_radiobutton_set(ctrl, dlg, button);
    } else if (event == EVENT_VALCHANGE) {
        button = dlg_radiobutton_get(ctrl, dlg);
        assert(button >= 0 && button < ctrl->radio.nbuttons);
        conf_set_bool(conf, ctrl->context.i,
                      ctrl->radio.buttondata[button].i);
    }
}

#define CHECKBOX_INVERT (1<<30)
void conf_checkbox_handler(dlgcontrol *ctrl, dlgparam *dlg,
                           void *data, int event)
{
    int key;
    bool invert;
    Conf *conf = (Conf *)data;

    /*
     * For a standard checkbox, the context parameter gives the
     * primary key (CONF_foo), optionally ORed with CHECKBOX_INVERT.
     */
    key = ctrl->context.i;
    if (key & CHECKBOX_INVERT) {
        key &= ~CHECKBOX_INVERT;
        invert = true;
    } else
        invert = false;

    /*
     * C lacks a logical XOR, so the following code uses the idiom
     * (!a ^ !b) to obtain the logical XOR of a and b. (That is, 1
     * iff exactly one of a and b is nonzero, otherwise 0.)
     */

    if (event == EVENT_REFRESH) {
        bool val = conf_get_bool(conf, key);
        dlg_checkbox_set(ctrl, dlg, (!val ^ !invert));
    } else if (event == EVENT_VALCHANGE) {
        conf_set_bool(conf, key, !dlg_checkbox_get(ctrl,dlg) ^ !invert);
    }
}

const struct conf_editbox_handler_type conf_editbox_str = {.type = EDIT_STR};
const struct conf_editbox_handler_type conf_editbox_int = {.type = EDIT_INT};

void conf_editbox_handler(dlgcontrol *ctrl, dlgparam *dlg,
                          void *data, int event)
{
    /*
     * The standard edit-box handler expects the main `context' field
     * to contain the primary key. The secondary `context2' field is a
     * pointer to the struct conf_editbox_handler_type defined in
     * putty.h.
     */
    int key = ctrl->context.i;
    const struct conf_editbox_handler_type *type = ctrl->context2.cp;
    Conf *conf = (Conf *)data;

    if (type->type == EDIT_STR) {
        if (event == EVENT_REFRESH) {
            bool utf8;
            char *field = conf_get_str_ambi(conf, key, &utf8);
            if (utf8)
                dlg_editbox_set_utf8(ctrl, dlg, field);
            else
                dlg_editbox_set(ctrl, dlg, field);
        } else if (event == EVENT_VALCHANGE) {
            char *field = dlg_editbox_get_utf8(ctrl, dlg);
            if (!conf_try_set_utf8(conf, key, field)) {
                sfree(field);
                field = dlg_editbox_get(ctrl, dlg);
                conf_set_str(conf, key, field);
            }
            sfree(field);
        }
    } else {
        if (event == EVENT_REFRESH) {
            char str[80];
            int value = conf_get_int(conf, key);
            if (type->type == EDIT_INT)
                snprintf( str, sizeof(str), "%d", value);
            else
                snprintf( str, sizeof(str), "%g", (double)value / type->denominator);
            dlg_editbox_set(ctrl, dlg, str);
        } else if (event == EVENT_VALCHANGE) {
            char *str = dlg_editbox_get(ctrl, dlg);
            if (type->type == EDIT_INT)
                conf_set_int(conf, key, atoi(str));
            else
                conf_set_int(conf, key, (int)(type->denominator * atof(str)));
            sfree(str);
        }
    }
}

void conf_filesel_handler(dlgcontrol *ctrl, dlgparam *dlg,
                          void *data, int event)
{
    int key = ctrl->context.i;
    Conf *conf = (Conf *)data;

    if (event == EVENT_REFRESH) {
        dlg_filesel_set(
            ctrl, dlg, conf_get_filename(conf, key));
    } else if (event == EVENT_VALCHANGE) {
        Filename *filename = dlg_filesel_get(ctrl, dlg);
        conf_set_filename(conf, key, filename);
        filename_free(filename);
    }
}

void conf_fontsel_handler(dlgcontrol *ctrl, dlgparam *dlg,
                          void *data, int event)
{
    int key = ctrl->context.i;
    Conf *conf = (Conf *)data;

    if (event == EVENT_REFRESH) {
        dlg_fontsel_set(
            ctrl, dlg, conf_get_fontspec(conf, key));
    } else if (event == EVENT_VALCHANGE) {
        FontSpec *fontspec = dlg_fontsel_get(ctrl, dlg);
        conf_set_fontspec(conf, key, fontspec);
        fontspec_free(fontspec);
    }
}

static void config_host_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                void *data, int event)
{
    Conf *conf = (Conf *)data;

    /*
     * This function works just like the standard edit box handler,
     * only it has to choose the control's label and text from two
     * different places depending on the protocol.
     */
    if (event == EVENT_REFRESH) {
        if (conf_get_int(conf, CONF_protocol) == PROT_SERIAL) {
            /*
             * This label text is carefully chosen to contain an n,
             * since that's the shortcut for the host name control.
             */
            dlg_label_change(ctrl, dlg, "Serial line");
            dlg_editbox_set(ctrl, dlg, conf_get_str(conf, CONF_serline));
        } else {
            dlg_label_change(ctrl, dlg, HOST_BOX_TITLE);
            dlg_editbox_set(ctrl, dlg, conf_get_str(conf, CONF_host));
        }
    } else if (event == EVENT_VALCHANGE) {
        char *s = dlg_editbox_get(ctrl, dlg);
        if (conf_get_int(conf, CONF_protocol) == PROT_SERIAL)
            conf_set_str(conf, CONF_serline, s);
        else
            conf_set_str(conf, CONF_host, s);
        sfree(s);
    }
}

static void config_port_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                void *data, int event)
{
    Conf *conf = (Conf *)data;
    char buf[80];

    /*
     * This function works similarly to the standard edit box handler,
     * only it has to choose the control's label and text from two
     * different places depending on the protocol.
     */
    if (event == EVENT_REFRESH) {
        if (conf_get_int(conf, CONF_protocol) == PROT_SERIAL) {
            /*
             * This label text is carefully chosen to contain a p,
             * since that's the shortcut for the port control.
             */
            dlg_label_change(ctrl, dlg, "Speed");
            snprintf( buf, sizeof(buf), "%d", conf_get_int(conf, CONF_serspeed));
        } else {
            dlg_label_change(ctrl, dlg, PORT_BOX_TITLE);
            if (conf_get_int(conf, CONF_port) != 0)
                snprintf( buf, sizeof(buf), "%d", conf_get_int(conf, CONF_port));
            else
                /* Display an (invalid) port of 0 as blank */
                buf[0] = '\0';
        }
        dlg_editbox_set(ctrl, dlg, buf);
    } else if (event == EVENT_VALCHANGE) {
        char *s = dlg_editbox_get(ctrl, dlg);
        int i = atoi(s);
        sfree(s);

        if (conf_get_int(conf, CONF_protocol) == PROT_SERIAL)
            conf_set_int(conf, CONF_serspeed, i);
        else
            conf_set_int(conf, CONF_port, i);
    }
}

struct hostport {
    dlgcontrol *host, *port, *protradio, *protlist;
    bool mid_refresh;
};

/*
 * Shared handler for protocol radio-button and drop-list controls.
 * Handles the interaction of those two controls, and also changes
 * the setting of the port box to match the protocol if necessary,
 * and refreshes both host and port boxes when switching to/from the
 * serial backend.
 */
static void config_protocols_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                     void *data, int event)
{
    Conf *conf = (Conf *)data;
    int curproto = conf_get_int(conf, CONF_protocol);
    struct hostport *hp = (struct hostport *)ctrl->context.p;

    if (event == EVENT_REFRESH) {
        /*
         * Refresh the states of the controls from Conf.
         *
         * When refreshing these controls, we have to watch out for
         * re-entrancy: because there are two controls involved, the
         * refresh is not atomic, so the VALCHANGE and/or SELCHANGE
         * callbacks resulting from our updates here might cause other
         * settings here to change unwantedly. (E.g. setting the list
         * selection shouldn't trigger the SELCHANGE side effect of
         * selecting the Other radio button; setting the radio button
         * to Other here shouldn't have the side effect of selecting
         * whatever protocol is _currently_ selected in the list box,
         * if we haven't selected the right one yet.)
         */
        hp->mid_refresh = true;

        if (ctrl == hp->protradio) {
            /* Available buttons were set up when control was created.
             * Just select one of them, possibly. */
            for (int button = 0; button < ctrl->radio.nbuttons; button++)
                /* The final button is "Other:". If we reach that one, the
                 * current protocol must be in the drop list, so we should
                 * select the "Other:" button. */
                if (curproto == ctrl->radio.buttondata[button].i ||
                    button == ctrl->radio.nbuttons-1) {
                    dlg_radiobutton_set(ctrl, dlg, button);
                    break;
                }
        } else if (ctrl == hp->protlist) {
            int curentry = -1;
            dlg_update_start(ctrl, dlg);
            dlg_listbox_clear(ctrl, dlg);
            assert(n_ui_backends > 0 && n_ui_backends < PROTOCOL_LIMIT);
            for (size_t i = n_ui_backends;
                 i < PROTOCOL_LIMIT && backends[i]; i++) {
                dlg_listbox_addwithid(ctrl, dlg,
                                      backends[i]->displayname_tc,
                                      backends[i]->protocol);
                if (backends[i]->protocol == curproto)
                    curentry = i - n_ui_backends;
            }
            if (curentry > 0) {
                /*
                 * The currently configured protocol is one of the
                 * list-box ones, so select it in protlist.
                 *
                 * (The corresponding refresh event for protradio
                 * should have selected the "Other:" radio button, to
                 * keep things consistent.)
                 */
                dlg_listbox_select(ctrl, dlg, curentry);
            } else {
                /*
                 * If the currently configured protocol is one of the
                 * radio buttons, we must still ensure *something* is
                 * selected in the list box. The sensible default is
                 * the first list element, which be_*.c ought to have
                 * arranged to be the 'runner-up' in protocol
                 * popularity out of the ones relegated to the list
                 * box.
                 *
                 * We don't make much effort to retain the state of
                 * the list box when it doesn't correspond to an
                 * actual protocol. So it's easy for this case to be
                 * reached as a side effect of other actions, e.g.
                 * loading a saved session that has a radio-button
                 * protocol configured.
                 */
                dlg_listbox_select(ctrl, dlg, 0);
            }
            dlg_update_done(ctrl, dlg);
        }

        hp->mid_refresh = false;
    } else if (!hp->mid_refresh) {
        /*
         * Potentially update Conf from the states of the controls.
         */
        int newproto = curproto;

        if (event == EVENT_VALCHANGE && ctrl == hp->protradio) {
            int button = dlg_radiobutton_get(ctrl, dlg);
            assert(button >= 0 && button < ctrl->radio.nbuttons);
            if (ctrl->radio.buttondata[button].i == -1) {
                /*
                 * The 'Other' radio button was selected, which means we
                 * have to set CONF_protocol based on the currently
                 * selected list box entry.
                 *
                 * (We conditionalise this on there _being_ a selected
                 * list box entry. I hope the case where nothing is
                 * selected can't actually come up except during
                 * initialisation, and I also hope that hp->mid_session
                 * will prevent that case from getting here. But as a
                 * last-ditch fallback, this if statement should at least
                 * guarantee that we don't pass a nonsense value to
                 * dlg_listbox_getid.)
                 */
                int i = dlg_listbox_index(hp->protlist, dlg);
                if (i >= 0)
                    newproto = dlg_listbox_getid(hp->protlist, dlg, i);
            } else {
                newproto = ctrl->radio.buttondata[button].i;
            }
        } else if (event == EVENT_SELCHANGE && ctrl == hp->protlist) {
            int i = dlg_listbox_index(ctrl, dlg);
            if (i >= 0) {
                newproto = dlg_listbox_getid(ctrl, dlg, i);
                /* Select the "Other" radio button, too */
                dlg_radiobutton_set(hp->protradio, dlg,
                                    hp->protradio->radio.nbuttons-1);
            }
        }

        if (newproto != curproto) {
            conf_set_int(conf, CONF_protocol, newproto);

            const struct BackendVtable *cvt = backend_vt_from_proto(curproto);
            const struct BackendVtable *nvt = backend_vt_from_proto(newproto);
            assert(cvt);
            assert(nvt);
            /*
             * Iff the user hasn't changed the port from the old
             * protocol's default, update it with the new protocol's
             * default.
             *
             * (This includes a "default" of 0, implying that there is
             * no sensible default for that protocol; in this case
             * it's displayed as a blank.)
             *
             * This helps with the common case of tabbing through the
             * controls in order and setting a non-default port before
             * getting to the protocol; we want that non-default port
             * to be preserved.
             */
            int port = conf_get_int(conf, CONF_port);
            if (port == cvt->default_port)
                conf_set_int(conf, CONF_port, nvt->default_port);

            dlg_refresh(hp->host, dlg);
            dlg_refresh(hp->port, dlg);
        }
    }
}

static void loggingbuttons_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                   void *data, int event)
{
    int button;
    Conf *conf = (Conf *)data;
    /* This function works just like the standard radio-button handler,
     * but it has to fall back to "no logging" in situations where the
     * configured logging type isn't applicable.
     */
    if (event == EVENT_REFRESH) {
        int logtype = conf_get_int(conf, CONF_logtype);

        for (button = 0; button < ctrl->radio.nbuttons; button++)
            if (logtype == ctrl->radio.buttondata[button].i)
                break;

        /* We fell off the end, so we lack the configured logging type */
        if (button == ctrl->radio.nbuttons) {
            button = 0;
            conf_set_int(conf, CONF_logtype, LGTYP_NONE);
        }
        dlg_radiobutton_set(ctrl, dlg, button);
    } else if (event == EVENT_VALCHANGE) {
        button = dlg_radiobutton_get(ctrl, dlg);
        assert(button >= 0 && button < ctrl->radio.nbuttons);
        conf_set_int(conf, CONF_logtype, ctrl->radio.buttondata[button].i);
    }
}

static void numeric_keypad_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                   void *data, int event)
{
    int button;
    Conf *conf = (Conf *)data;
    /*
     * This function works much like the standard radio button
     * handler, but it has to handle two fields in Conf.
     */
    if (event == EVENT_REFRESH) {
        if (conf_get_bool(conf, CONF_nethack_keypad))
            button = 2;
        else if (conf_get_bool(conf, CONF_app_keypad))
            button = 1;
        else
            button = 0;
        assert(button < ctrl->radio.nbuttons);
        dlg_radiobutton_set(ctrl, dlg, button);
    } else if (event == EVENT_VALCHANGE) {
        button = dlg_radiobutton_get(ctrl, dlg);
        assert(button >= 0 && button < ctrl->radio.nbuttons);
        if (button == 2) {
            conf_set_bool(conf, CONF_app_keypad, false);
            conf_set_bool(conf, CONF_nethack_keypad, true);
        } else {
            conf_set_bool(conf, CONF_app_keypad, (button != 0));
            conf_set_bool(conf, CONF_nethack_keypad, false);
        }
    }
}

static void cipherlist_handler(dlgcontrol *ctrl, dlgparam *dlg,
                               void *data, int event)
{
    Conf *conf = (Conf *)data;
    if (event == EVENT_REFRESH) {
        int i;

        static const struct { const char *s; int c; } ciphers[] = {
            { "ChaCha20 (SSH-2 only)",  CIPHER_CHACHA20 },
            { "AES-GCM (SSH-2 only)",   CIPHER_AESGCM },
            { "3DES",                   CIPHER_3DES },
            { "Blowfish",               CIPHER_BLOWFISH },
            { "DES",                    CIPHER_DES },
            { "AES (SSH-2 only)",       CIPHER_AES },
            { "Arcfour (SSH-2 only)",   CIPHER_ARCFOUR },
            { "-- warn below here --",  CIPHER_WARN }
        };

        /* Set up the "selected ciphers" box. */
        /* (cipherlist assumed to contain all ciphers) */
        dlg_update_start(ctrl, dlg);
        dlg_listbox_clear(ctrl, dlg);
        for (i = 0; i < CIPHER_MAX; i++) {
            int c = conf_get_int_int(conf, CONF_ssh_cipherlist, i);
            int j;
            const char *cstr = NULL;
            for (j = 0; j < (sizeof ciphers) / (sizeof ciphers[0]); j++) {
                if (ciphers[j].c == c) {
                    cstr = ciphers[j].s;
                    break;
                }
            }
            dlg_listbox_addwithid(ctrl, dlg, cstr, c);
        }
        dlg_update_done(ctrl, dlg);

    } else if (event == EVENT_VALCHANGE) {
        int i;

        /* Update array to match the list box. */
        for (i=0; i < CIPHER_MAX; i++)
            conf_set_int_int(conf, CONF_ssh_cipherlist, i,
                             dlg_listbox_getid(ctrl, dlg, i));
    }
}

#ifndef NO_GSSAPI
static void gsslist_handler(dlgcontrol *ctrl, dlgparam *dlg,
                            void *data, int event)
{
    Conf *conf = (Conf *)data;
    if (event == EVENT_REFRESH) {
        int i;

        dlg_update_start(ctrl, dlg);
        dlg_listbox_clear(ctrl, dlg);
        for (i = 0; i < ngsslibs; i++) {
            int id = conf_get_int_int(conf, CONF_ssh_gsslist, i);
            assert(id >= 0 && id < ngsslibs);
            dlg_listbox_addwithid(ctrl, dlg, gsslibnames[id], id);
        }
        dlg_update_done(ctrl, dlg);

    } else if (event == EVENT_VALCHANGE) {
        int i;

        /* Update array to match the list box. */
        for (i=0; i < ngsslibs; i++)
            conf_set_int_int(conf, CONF_ssh_gsslist, i,
                             dlg_listbox_getid(ctrl, dlg, i));
    }
}
#endif

static void kexlist_handler(dlgcontrol *ctrl, dlgparam *dlg,
                            void *data, int event)
{
    Conf *conf = (Conf *)data;
    if (event == EVENT_REFRESH) {
        int i;

        static const struct { const char *s; int k; } kexes[] = {
            { "Diffie-Hellman group 1 (1024-bit)",  KEX_DHGROUP1 },
            { "Diffie-Hellman group 14 (2048-bit)", KEX_DHGROUP14 },
            { "Diffie-Hellman group 15 (3072-bit)", KEX_DHGROUP15 },
            { "Diffie-Hellman group 16 (4096-bit)", KEX_DHGROUP16 },
            { "Diffie-Hellman group 17 (6144-bit)", KEX_DHGROUP17 },
            { "Diffie-Hellman group 18 (8192-bit)", KEX_DHGROUP18 },
            { "Diffie-Hellman group exchange",      KEX_DHGEX },
            { "RSA-based key exchange",             KEX_RSA },
            { "ECDH key exchange",                  KEX_ECDH },
            { "NTRU Prime / Curve25519 hybrid kex", KEX_NTRU_HYBRID },
            { "ML-KEM / Curve25519 hybrid kex",     KEX_MLKEM_25519_HYBRID },
            { "ML-KEM / NIST ECDH hybrid kex",      KEX_MLKEM_NIST_HYBRID },
            { "-- warn below here --",              KEX_WARN }
        };

        /* Set up the "kex preference" box. */
        /* (kexlist assumed to contain all algorithms) */
        dlg_update_start(ctrl, dlg);
        dlg_listbox_clear(ctrl, dlg);
        for (i = 0; i < KEX_MAX; i++) {
            int k = conf_get_int_int(conf, CONF_ssh_kexlist, i);
            int j;
            const char *kstr = NULL;
            for (j = 0; j < (sizeof kexes) / (sizeof kexes[0]); j++) {
                if (kexes[j].k == k) {
                    kstr = kexes[j].s;
                    break;
                }
            }
            dlg_listbox_addwithid(ctrl, dlg, kstr, k);
        }
        dlg_update_done(ctrl, dlg);

    } else if (event == EVENT_VALCHANGE) {
        int i;

        /* Update array to match the list box. */
        for (i=0; i < KEX_MAX; i++)
            conf_set_int_int(conf, CONF_ssh_kexlist, i,
                             dlg_listbox_getid(ctrl, dlg, i));
    }
}

static void hklist_handler(dlgcontrol *ctrl, dlgparam *dlg,
                           void *data, int event)
{
    Conf *conf = (Conf *)data;
    if (event == EVENT_REFRESH) {
        int i;

        static const struct { const char *s; int k; } hks[] = {
            { "Ed25519",               HK_ED25519 },
            { "Ed448",                 HK_ED448 },
            { "ECDSA",                 HK_ECDSA },
            { "DSA",                   HK_DSA },
            { "RSA",                   HK_RSA },
            { "-- warn below here --", HK_WARN }
        };

        /* Set up the "host key preference" box. */
        /* (hklist assumed to contain all algorithms) */
        dlg_update_start(ctrl, dlg);
        dlg_listbox_clear(ctrl, dlg);
        for (i = 0; i < HK_MAX; i++) {
            int k = conf_get_int_int(conf, CONF_ssh_hklist, i);
            int j;
            const char *kstr = NULL;
            for (j = 0; j < lenof(hks); j++) {
                if (hks[j].k == k) {
                    kstr = hks[j].s;
                    break;
                }
            }
            dlg_listbox_addwithid(ctrl, dlg, kstr, k);
        }
        dlg_update_done(ctrl, dlg);

    } else if (event == EVENT_VALCHANGE) {
        int i;

        /* Update array to match the list box. */
        for (i=0; i < HK_MAX; i++)
            conf_set_int_int(conf, CONF_ssh_hklist, i,
                             dlg_listbox_getid(ctrl, dlg, i));
    }
}

static void printerbox_handler(dlgcontrol *ctrl, dlgparam *dlg,
                               void *data, int event)
{
    Conf *conf = (Conf *)data;
    if (event == EVENT_REFRESH) {
        int nprinters, i;
        printer_enum *pe;
        const char *printer;

        dlg_update_start(ctrl, dlg);
        /*
         * Some backends may wish to disable the drop-down list on
         * this edit box. Be prepared for this.
         */
        if (ctrl->editbox.has_list) {
            dlg_listbox_clear(ctrl, dlg);
            dlg_listbox_add(ctrl, dlg, PRINTER_DISABLED_STRING);
#ifdef MOD_PRINTCLIP
            /* KiTTY fake printer that copies remote output to the clipboard. */
            if (!GetPuttyFlag())
                dlg_listbox_add(ctrl, dlg, "Windows clipboard");
#endif
            pe = printer_start_enum(&nprinters);
            for (i = 0; i < nprinters; i++)
                dlg_listbox_add(ctrl, dlg, printer_get_name(pe, i));
            printer_finish_enum(pe);
        }
        printer = conf_get_str(conf, CONF_printer);
        if (!printer)
            printer = PRINTER_DISABLED_STRING;
        dlg_editbox_set(ctrl, dlg, printer);
        dlg_update_done(ctrl, dlg);
    } else if (event == EVENT_VALCHANGE) {
        char *printer = dlg_editbox_get(ctrl, dlg);
        if (!strcmp(printer, PRINTER_DISABLED_STRING))
            printer[0] = '\0';
        conf_set_str(conf, CONF_printer, printer);
        sfree(printer);
    }
}

#ifdef MOD_PRINTCLIP
/* KiTTY "Print to clipboard" checkbox: toggles the printer between the fake
 * "Windows clipboard" target and "no printer", kept in sync with the printer
 * combobox above (which also lists "Windows clipboard"). */
static void kitty_printclip_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                    void *data, int event)
{
    Conf *conf = (Conf *)data;
    if (event == EVENT_REFRESH) {
        dlg_checkbox_set(ctrl, dlg,
            !strcmp(conf_get_str(conf, CONF_printer), "Windows clipboard"));
    } else if (event == EVENT_VALCHANGE) {
        bool on = dlg_checkbox_get(ctrl, dlg);
        if (on)
            conf_set_str(conf, CONF_printer, "Windows clipboard");
        else if (!strcmp(conf_get_str(conf, CONF_printer), "Windows clipboard"))
            conf_set_str(conf, CONF_printer, "");   /* no printer */
        conf_set_int(conf, CONF_printclip, on ? 1 : 0);
        dlg_refresh(NULL, dlg);   /* sync the printer combobox display */
    }
}
#endif

static void codepage_handler(dlgcontrol *ctrl, dlgparam *dlg,
                             void *data, int event)
{
    Conf *conf = (Conf *)data;
    if (event == EVENT_REFRESH) {
        int i;
        const char *cp, *thiscp;
        dlg_update_start(ctrl, dlg);
        thiscp = cp_name(decode_codepage(conf_get_str(conf,
                                                      CONF_line_codepage)));
        dlg_listbox_clear(ctrl, dlg);
        for (i = 0; (cp = cp_enumerate(i)) != NULL; i++)
            dlg_listbox_add(ctrl, dlg, cp);
        dlg_editbox_set(ctrl, dlg, thiscp);
        conf_set_str(conf, CONF_line_codepage, thiscp);
        dlg_update_done(ctrl, dlg);
    } else if (event == EVENT_VALCHANGE) {
        char *codepage = dlg_editbox_get(ctrl, dlg);
        conf_set_str(conf, CONF_line_codepage,
                     cp_name(decode_codepage(codepage)));
        sfree(codepage);
    }
}

static void sshbug_handler(dlgcontrol *ctrl, dlgparam *dlg,
                           void *data, int event)
{
    Conf *conf = (Conf *)data;
    if (event == EVENT_REFRESH) {
        /*
         * We must fetch the previously configured value from the Conf
         * before we start modifying the drop-down list, otherwise the
         * spurious SELCHANGE we trigger in the process will overwrite
         * the value we wanted to keep.
         */
        int oldconf = conf_get_int(conf, ctrl->context.i);
        dlg_update_start(ctrl, dlg);
        dlg_listbox_clear(ctrl, dlg);
        dlg_listbox_addwithid(ctrl, dlg, "Auto", AUTO);
        dlg_listbox_addwithid(ctrl, dlg, "Off", FORCE_OFF);
        dlg_listbox_addwithid(ctrl, dlg, "On", FORCE_ON);
        switch (oldconf) {
          case AUTO:      dlg_listbox_select(ctrl, dlg, 0); break;
          case FORCE_OFF: dlg_listbox_select(ctrl, dlg, 1); break;
          case FORCE_ON:  dlg_listbox_select(ctrl, dlg, 2); break;
        }
        dlg_update_done(ctrl, dlg);
    } else if (event == EVENT_SELCHANGE) {
        int i = dlg_listbox_index(ctrl, dlg);
        if (i < 0)
            i = AUTO;
        else
            i = dlg_listbox_getid(ctrl, dlg, i);
        conf_set_int(conf, ctrl->context.i, i);
    }
}

static void sshbug_handler_manual_only(dlgcontrol *ctrl, dlgparam *dlg,
                                       void *data, int event)
{
    /*
     * This is just like sshbug_handler, except that there's no 'Auto'
     * option. Used for bug workaround flags that can't be
     * autodetected, and have to be manually enabled if they're to be
     * used at all.
     */
    Conf *conf = (Conf *)data;
    if (event == EVENT_REFRESH) {
        int oldconf = conf_get_int(conf, ctrl->context.i);
        dlg_update_start(ctrl, dlg);
        dlg_listbox_clear(ctrl, dlg);
        dlg_listbox_addwithid(ctrl, dlg, "Off", FORCE_OFF);
        dlg_listbox_addwithid(ctrl, dlg, "On", FORCE_ON);
        switch (oldconf) {
          case FORCE_OFF: dlg_listbox_select(ctrl, dlg, 0); break;
          case FORCE_ON:  dlg_listbox_select(ctrl, dlg, 1); break;
        }
        dlg_update_done(ctrl, dlg);
    } else if (event == EVENT_SELCHANGE) {
        int i = dlg_listbox_index(ctrl, dlg);
        if (i < 0)
            i = FORCE_OFF;
        else
            i = dlg_listbox_getid(ctrl, dlg, i);
        conf_set_int(conf, ctrl->context.i, i);
    }
}

struct sessionsaver_data {
    dlgcontrol *editbox, *listbox, *loadbutton, *savebutton, *delbutton;
    dlgcontrol *okbutton, *cancelbutton;
#ifdef MOD_PERSO
    dlgcontrol *startbutton;     /* KiTTY: open session without closing config box */
#endif
#ifdef MOD_PERSO
    dlgcontrol *folderlist;      /* KiTTY: editable session-folder combo */
    dlgcontrol *createbutton, *delfolderbutton; /* KiTTY folder mgmt */
    dlgcontrol *commentbox;      /* KiTTY: read-only comment of selected session */
    dlgcontrol *exportbutton, *importbutton; /* KiTTY: whole-store export/import */
    int cb_top_spacers, cb_bot_spacers; /* height-scaled button distribution */
#endif
    struct sesslist sesslist;
    bool midsession;
    char *savedsession;     /* the current contents of ssd->editbox */
#ifdef MOD_PERSO
    char *newfolder;        /* typed folder name in ssd->folderlist combo */
    char *searchfilter;     /* live type-to-search filter from saved-session edit */
    int suppress_edit_valchange; /* set while programmatically updating editbox */
    int suppress_list_selchange; /* set while programmatically selecting list rows */
    int suppress_folder_valchange; /* set while programmatically updating folderlist */
    int folder_new_selected;     /* the synthetic "<new folder...>" row is picked */
    char *folder_at_load;        /* folder the loaded session came from, or NULL:
                                  * Save only re-files when this changed */
    int folder_action;           /* what ssd->createbutton currently does */
    const char *folder_button_label; /* its current label, to avoid redundant sets */
    int initial_focus_set;
#endif
};

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
int GetQuickConnectMode(void);   /* kitty.c */

/* KiTTY: the same dialog's session-saver data, for the Ctrl+G "search
 * everywhere" jump (windows/dialog.c). Registered and cleared together with
 * session_filter_ctrl above, so it can never outlive the dialog. */
static struct sessionsaver_data *session_filter_ssd = NULL;

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
    if (!ssd || GetPuttyFlag() || !ssd->folderlist || !ssd->listbox)
        return false;
    if (!strcmp(CurrentFolder, "Default"))
        return false;                    /* already showing everything */
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
     * rebuilds from CurrentFolder when it is next shown. */
    if (dlg_is_visible(ssd->folderlist, dp))
        dlg_refresh(ssd->folderlist, dp);
    if (dlg_is_visible(ssd->listbox, dp))
        dlg_refresh(ssd->listbox, dp);
    return true;
}

static void sessionsaver_data_free(void *ssdv)
{
    struct sessionsaver_data *ssd = (struct sessionsaver_data *)ssdv;
    if (session_filter_ctrl == ssd->editbox)
        session_filter_ctrl = NULL;
    if (session_filter_ssd == ssd)
        session_filter_ssd = NULL;
    get_sesslist(&ssd->sesslist, false);
    sfree(ssd->savedsession);
#ifdef MOD_PERSO
    sfree(ssd->newfolder);
    sfree(ssd->searchfilter);
    sfree(ssd->folder_at_load);
#endif
    sfree(ssd);
}

#ifdef MOD_PERSO
char *kitty_read_session_folder(const char *sessionname);   /* windows/storage.c */
static int sessionsaver_folder_visible_position(struct sessionsaver_data *ssd,
                                                int sessindex)
{
    int pos = 0;
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
        if (!GetPuttyFlag() && i > 0 && strcmp(CurrentFolder, "Default") != 0) {
            char *fld = kitty_read_session_folder(ssd->sesslist.sessions[i]);
            int match = (fld && !strcmp(fld, CurrentFolder));
            sfree(fld);
            if (!match)
                continue;
        }
        if (i == sessindex)
            return pos;
        pos++;
    }
    return -1;
}
#endif

/*
 * Helper function to load the session selected in the list box, if
 * any, as this is done in more than one place below. Returns 0 for
 * failure.
 */
#ifdef MOD_PERSO
static void sessionsaver_switch_folder(struct sessionsaver_data *ssd,
                                       dlgparam *dlg, const char *folder);
#endif

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
#ifdef MOD_PERSO
    selid = dlg_listbox_getid(ssd->listbox, dlg, i);
#endif
    if (selid < 0 || selid >= ssd->sesslist.nsessions) {
        dlg_beep(dlg);
        return false;
    }
    i = selid;
    isdef = !strcmp(ssd->sesslist.sessions[i], KITTY_DEFAULT_SESSION);
    load_settings(ssd->sesslist.sessions[i], conf);
#ifdef MOD_PERSO
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
    if (!GetPuttyFlag() && ssd->folderlist && !isdef) {
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
#endif
#ifdef MOD_PERSO
    /* KiTTY: remember this as the last-loaded session, so the config box
     * re-selects/auto-loads it next time it opens. "Default Settings" is
     * remembered too (hknet/KiTTY#23): next start recognises it and comes up in
     * quick connect instead of pre-filling anything, so loading the defaults is
     * how that mode is armed - and loading any other session disarms it. */
    kitty_set_last_session(ssd->sesslist.sessions[i]);
#endif
    sfree(ssd->savedsession);
#ifdef MOD_PERSO
    /* KiTTY: keep "Default Settings" visible in the session-name box after
     * loading the defaults, instead of stock PuTTY's clearing it. The literal
     * name is already a normal value for savedsession (single-clicking the
     * list entry puts it there too) and the Save path maps it to the defaults
     * key. PuTTY-compat mode keeps the stock behaviour. */
    ssd->savedsession = dupstr(isdef ? (GetPuttyFlag() ? "" : KITTY_DEFAULT_SESSION)
                                     : ssd->sesslist.sessions[i]);
    sfree(ssd->searchfilter);
    ssd->searchfilter = dupstr("");
#else
    ssd->savedsession = dupstr(isdef ? "" : ssd->sesslist.sessions[i]);
#endif
    if (maybe_launch)
        *maybe_launch = !isdef;
    dlg_refresh(NULL, dlg);
    /* Restore the selection, which might have been clobbered by
     * changing the value of the edit box. The listbox API wants a visible row
     * position, not the stable session id used in filtered/search lists. */
#ifdef MOD_PERSO
    {
        int row = sessionsaver_folder_visible_position(ssd, i);
        if (row >= 0)
            dlg_listbox_select(ssd->listbox, dlg, row);
    }
#else
    dlg_listbox_select(ssd->listbox, dlg, i);
#endif
#ifdef MOD_PERSO
    /* KiTTY: dlg_refresh(NULL) above refreshed the read-only comment box while
     * the listbox selection was momentarily cleared (so it blanked); refresh it
     * again now the selection is restored, so the comment stays shown after Load. */
    if (ssd->commentbox)
        dlg_refresh(ssd->commentbox, dlg);
#endif
    return true;
}

#ifdef MOD_PERSO
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
#endif

#ifdef MOD_PERSO
/* KiTTY: refresh the read-only comment box from the session currently selected
 * in the saved-sessions list. Indexing mirrors load_selected_session() so the
 * box always shows the comment of the session that Load would open. Shows the
 * empty string if nothing is selected or the session has no comment. */
char *kitty_read_session_comment(const char *sessionname);  /* windows/storage.c */
char *kitty_read_session_folder(const char *sessionname);   /* windows/storage.c */
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
        dlg_editbox_set(ssd->commentbox, dlg, "Select a session to see its comment");
        return;
    }
    /* Read "Comment" directly, scanning all hives for a non-empty value, so
     * comments authored by an older KiTTY (held only in the 9bis hive) show
     * even before the session is re-saved into the new hive. */
    c = kitty_read_session_comment(ssd->sesslist.sessions[i]);
    dlg_editbox_set(ssd->commentbox, dlg, (c && *c) ? c : "(no comment stored for this session)");
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
    bool root_view = (!CurrentFolder[0] || !strcmp(CurrentFolder, "Default"));
    char *fld = (searching || root_view) ?
        kitty_read_session_folder(sessionname) : NULL;
    bool filed = (fld && *fld && strcmp(fld, "Default"));
    if (searching || filed) {
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
        (MessageBoxTimeoutA_t)GetProcAddress(user32, "MessageBoxTimeoutA") : NULL;
    if (msgbox_timeout)
        msgbox_timeout(dlg->hwnd, "root folder can't be deleted", "KiTTY",
                       MB_OK | MB_ICONINFORMATION, 0, 5000);
    else
        MessageBoxA(dlg->hwnd, "root folder can't be deleted", "KiTTY",
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
    if (ReadParameterN(INIT_SECTION, "RootFolderLabel", buf, len) == 0 ||
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
 * treat as read-only. See design/TASK_folder_rename.md. */
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
        fld = kitty_read_session_folder(ssd->sesslist.sessions[i]);
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
        char *fld = kitty_read_session_folder(sess);
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
                     "Could not update session \"%s\":\n%s", sess,
                     errmsg ? errmsg : "unknown error");
            sfree(errmsg);
            dlg_error_msg(dlg, msg);
            return -1;
        }
        write_setting_s(w, "Folder", dest);
        close_settings_w(w);
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
                 "\"%s\" contains one session.\n\n"
                 "Delete the folder and move the session to the root list?\n"
                 "The session itself is kept.", CurrentFolder);
    else
        snprintf(msg, sizeof(msg),
                 "\"%s\" contains %d sessions.\n\n"
                 "Delete the folder and move the sessions to the root list?\n"
                 "The sessions themselves are kept.", CurrentFolder, n);
    if (MessageBoxA(dlg->hwnd, msg, "KiTTY",
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
    extern int GetDefaultSettingsFlag(void);
    extern void SetDefaultSettingsFlag(const int flag);
    extern char *GetKittyIniFile(void);
    extern void CreateDefaultIniFile(void);
    extern int GetNoKittyFileFlag(void);
    extern int GetReadOnlyFlag(void);
    extern int writeINI(const char *filename, const char *section,
                        const char *key, const char *value);
    const char *ini = GetKittyIniFile();
    char msg[1400];

    /* conf=no (no configuration file at all) or readonly=yes: the setting
     * cannot be persisted, so do not offer a choice we can't honour. */
    if (GetNoKittyFileFlag() || !ini || !ini[0] || GetReadOnlyFlag()) {
        snprintf(msg, sizeof(msg),
                 "\"%s\" cannot be deleted - it is the template every new "
                 "session starts from.\n\n"
                 "It can normally be hidden from this list, but %s, so that "
                 "setting cannot be saved right now.",
                 KITTY_DEFAULT_SESSION,
                 GetReadOnlyFlag() ? "KiTTY is running read-only"
                                   : "this KiTTY is running without a "
                                     "configuration file");
        MessageBoxA(dlg->hwnd, msg, "KiTTY", MB_OK | MB_ICONINFORMATION);
        return;
    }

    snprintf(msg, sizeof(msg),
             "\"%s\" cannot be deleted - it is the template every new session "
             "starts from.\n\n"
             "It can be hidden from this list instead. The template itself "
             "keeps working; it simply stops taking up a row.\n\n"
             "Hide it?\n\n"
             "To show it again later you have to edit the configuration file "
             "by hand and set:\n"
             "    [ConfigBox]\n"
             "    defaultsettings=yes\n\n"
             "Configuration file:\n%s",
             KITTY_DEFAULT_SESSION, ini);

    if (MessageBoxA(dlg->hwnd, msg, "KiTTY",
                    MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
        return;                          /* No = keep showing it */

    CreateDefaultIniFile();              /* no-op when it already exists */
    if (!writeINI(ini, "ConfigBox", "defaultsettings", "no")) {
        snprintf(msg, sizeof(msg),
                 "Could not write to the configuration file:\n%s\n\n"
                 "\"%s\" is still shown.", ini, KITTY_DEFAULT_SESSION);
        MessageBoxA(dlg->hwnd, msg, "KiTTY", MB_OK | MB_ICONERROR);
        return;
    }

    SetDefaultSettingsFlag(0);           /* take effect without a restart */
    get_sesslist(&ssd->sesslist, false);
    get_sesslist(&ssd->sesslist, true);
    dlg_refresh(ssd->listbox, dlg);
}

static void sessionsaver_update_folder_button(struct sessionsaver_data *ssd,
                                              dlgparam *dlg)
{
    const char *label;
    sessionsaver_recompute_folder_action(ssd);
    if (!ssd->createbutton)          /* midsession / stock variants */
        return;
    /* Keep these SHORT: the button shares a 75/25 row with the combo, and
     * anything longer than "New folder" overflows its width. */
    label = (ssd->folder_action == KITTY_FOLDER_ACTION_NEW) ?
        "New folder" : "Rename";
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
    strncpy(CurrentFolder, folder, 1023);
    CurrentFolder[1023] = '\0';
    kitty_set_last_folder(CurrentFolder);
    sfree(ssd->searchfilter);
    ssd->searchfilter = dupstr("");
    dlg_refresh(ssd->listbox, dlg);
    if (ssd->commentbox)
        dlg_refresh(ssd->commentbox, dlg);
}
#endif

static void sessionsaver_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                 void *data, int event)
{
    Conf *conf = (Conf *)data;
    struct sessionsaver_data *ssd =
        (struct sessionsaver_data *)ctrl->context.p;

    if (event == EVENT_REFRESH) {
        if (ctrl == ssd->editbox) {
#ifdef MOD_PERSO
            ssd->suppress_edit_valchange++;
#endif
            dlg_editbox_set(ctrl, dlg, ssd->savedsession);
#ifdef MOD_PERSO
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
            }
#endif
        } else if (ctrl == ssd->listbox) {
            int i;
#ifdef MOD_PERSO
            if (ssd->editbox && !ssd->midsession)
                dlg_editbox_set_updown_target(ssd->editbox, ctrl, dlg);
#endif
            dlg_update_start(ctrl, dlg);
            dlg_listbox_clear(ctrl, dlg);
#ifdef MOD_PERSO
            char lastsess[512];
            const char *filter = ssd->searchfilter ? ssd->searchfilter : "";
            bool searching = (!GetPuttyFlag() && filter[0]);
            int havelast = kitty_get_last_session(lastsess, sizeof(lastsess));
            int selpos = -1, lbpos = 0;
            for (int pass = 2; pass >= 1; pass--) {
#endif
            for (i = 0; i < ssd->sesslist.nsessions; i++) {
#ifdef MOD_PERSO
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
                /* KiTTY folder filter: hide sessions not in the selected folder,
                 * but only when a specific (non-Default) folder is chosen, and
                 * always keep entry 0 ("Default Settings"). */
                if (!GetPuttyFlag() && i > 0 &&
                    strcmp(CurrentFolder, "Default") != 0) {
                    char *fld = kitty_read_session_folder(ssd->sesslist.sessions[i]);
                    int match = (fld && !strcmp(fld, CurrentFolder));
                    sfree(fld);
                    if (!match)
                        continue;
                }
                smatch = sessionsaver_filter_match(ssd->sesslist.sessions[i], filter);
                if (searching && smatch != pass)
                    continue;
                sessionsaver_add_session_row(ctrl, dlg, ssd, i, searching);
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
#else
                dlg_listbox_add(ctrl, dlg, ssd->sesslist.sessions[i]);
#endif
            }
#ifdef MOD_PERSO
            if (!searching)
                break;
            }
#endif
            dlg_update_done(ctrl, dlg);
#ifdef MOD_PERSO
            /* KiTTY: auto-select the best visible match (chosen above):
             * typed match while searching, else the current name-box session,
             * else the remembered last session; default to row 0. */
            if (selpos < 0 && lbpos > 0)
                selpos = 0;
            if (selpos >= 0) {
                ssd->suppress_list_selchange++;
                dlg_listbox_select(ctrl, dlg, selpos);
                ssd->suppress_list_selchange--;
            }
#endif
        }
#ifdef MOD_PERSO
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
#endif
    } else if (event == EVENT_VALCHANGE) {
        int top, bottom, halfway, i;
        if (ctrl == ssd->editbox) {
            sfree(ssd->savedsession);
            ssd->savedsession = dlg_editbox_get(ctrl, dlg);
#ifdef MOD_PERSO
            if (!ssd->suppress_edit_valchange && GetSessionFilterFlag()) {
                /* [ConfigBox] filter=no keeps searchfilter empty, so typing
                 * a name never narrows the saved-sessions list. */
                sfree(ssd->searchfilter);
                ssd->searchfilter = dupstr(ssd->savedsession);
                dlg_refresh(ssd->listbox, dlg);
                if (ssd->commentbox)
                    dlg_refresh(ssd->commentbox, dlg);
                return;
            }
#endif
#ifdef MOD_PERSO
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
#else
            top = ssd->sesslist.nsessions;
            bottom = -1;
#endif
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
#ifdef MOD_PERSO
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
#endif
    } else if (event == EVENT_ACTION) {
        bool mbl = false;
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
#ifdef MOD_PERSO
                /* [ConfigBox] dblclick=start: double-click acts like the Start
                 * button - launch in a new window, keep the config box open.
                 * Default (open) falls through to stock behaviour. */
                if (GetDblClickFlag() == 1) {
                    RunConfig(conf);
                    return;
                }
#endif
                dlg_end(dlg, 1);       /* it's all over, and succeeded */
            }
        } else if (ctrl == ssd->savebutton) {
            bool isdef = !strcmp(ssd->savedsession, KITTY_DEFAULT_SESSION);
            if (!ssd->savedsession[0]) {
                int i = dlg_listbox_index(ssd->listbox, dlg);
                if (i < 0) {
                    dlg_beep(dlg);
                    return;
                }
                isdef = !strcmp(ssd->sesslist.sessions[i], KITTY_DEFAULT_SESSION);
                sfree(ssd->savedsession);
                ssd->savedsession = dupstr(isdef ? "" :
                                           ssd->sesslist.sessions[i]);
            }
            {
#ifdef MOD_PERSO
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
                if (!GetPuttyFlag() && ssd->folderlist) {
                    const char *cur = !CurrentFolder[0] ? "Default" : CurrentFolder;
                    if (isdef)
                        conf_set_str(conf, CONF_folder, "Default");
                    else if (!ssd->folder_at_load ||
                             strcmp(cur, ssd->folder_at_load))
                        conf_set_str(conf, CONF_folder, cur);
                    /* else: keep the folder the session was loaded with */
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
                    /* Tell a running KiTTY Launcher to refresh its saved-session
                     * list and re-register per-session global hotkeys. This is a
                     * best-effort broadcast; if no launcher is running, nothing
                     * happens and the next launcher start reads the new settings. */
                    kitty_notify_launcher_sessions_changed();
                }
            }
            get_sesslist(&ssd->sesslist, false);
            get_sesslist(&ssd->sesslist, true);
            dlg_refresh(ssd->editbox, dlg);
            /* KiTTY: remember the just-saved session so the listbox refresh
             * auto-selects it (with the correct visible index, even when a
             * folder filter is active). Skip the default settings pseudo-session. */
            if (ssd->savedsession && ssd->savedsession[0] &&
                strcmp(ssd->savedsession, KITTY_DEFAULT_SESSION) != 0)
                kitty_set_last_session(ssd->savedsession);
            dlg_refresh(ssd->listbox, dlg);
        } else if (!ssd->midsession &&
                   ssd->delbutton && ctrl == ssd->delbutton) {
            int i = dlg_listbox_index(ssd->listbox, dlg);
#ifdef MOD_PERSO
            if (i >= 0) i = dlg_listbox_getid(ssd->listbox, dlg, i);
#endif
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
                dlg_refresh(ssd->listbox, dlg);
            }
#ifdef MOD_PERSO
        } else if (!ssd->midsession &&
                   ssd->createbutton && ctrl == ssd->createbutton) {
            /* Dual-purpose button - its label always says which one applies
             * (sessionsaver_update_folder_button). The name comes from the
             * folder combo's edit half, never from the Saved Sessions field
             * (which holds the session name). */
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
                    WriteParameter(INIT_SECTION, "RootFolderLabel", reset);
                    sfree(ssd->newfolder);
                    ssd->newfolder = dupstr("");
                    dlg_refresh(ssd->folderlist, dlg);
                } else if (!stricmp(label, "Default") ||
                           !stricmp(label, KITTY_NEW_FOLDER_ITEM)) {
                    /* NOT the full reserved-name check: that one also refuses
                     * the current label, which would block re-casing it
                     * ("Alle Sessions" -> "alle sessions"). */
                    dlg_error_msg(dlg, "That name is reserved.");
                } else if (sessionsaver_folder_exists(label)) {
                    dlg_error_msg(dlg, "A folder of that name already exists.");
                } else {
                    WriteParameter(INIT_SECTION, "RootFolderLabel", label);
                    sfree(ssd->newfolder);
                    ssd->newfolder = dupstr("");
                    dlg_refresh(ssd->folderlist, dlg);
                }
            } else if (ssd->folder_action == KITTY_FOLDER_ACTION_RENAME) {
                /* Rename the selected folder. A folder is not a container:
                 * membership is the "Folder" value of each session, and
                 * InitFolderList() rebuilds the folder set from the Folders
                 * value PLUS a scan of those values. So the members must be
                 * rewritten FIRST - renaming only the list entry would be
                 * undone by the very next rebuild. */
                char folder[1024], old[1024];
                strncpy(folder, ssd->newfolder, sizeof(folder)-1);
                folder[sizeof(folder)-1] = '\0';
                CleanFolderName(folder);
                strncpy(old, CurrentFolder, sizeof(old)-1);
                old[sizeof(old)-1] = '\0';
                if (!folder[0]) {
                    dlg_beep(dlg);
                } else if (kitty_folder_name_reserved(folder)) {
                    dlg_error_msg(dlg, "That name is reserved for the root session list.");
                } else if (stricmp(folder, old) && sessionsaver_folder_exists(folder)) {
                    dlg_error_msg(dlg, "A folder of that name already exists.");
                } else if (sessionsaver_move_folder_sessions(ssd, dlg, old,
                                                             folder) >= 0) {
                    InitFolderList();
                    StringList_Del(FolderList, old);
                    StringList_Add(FolderList, folder);
                    SaveFolderList();
                    strncpy(CurrentFolder, folder, 1023);
                    CurrentFolder[1023] = '\0';
                    kitty_set_last_folder(CurrentFolder);
                    /* The loaded session, if it was in the renamed folder,
                     * must follow it or the next Save would put it back under
                     * the old name. */
                    if (!strcmp(conf_get_str(conf, CONF_folder), old))
                        conf_set_str(conf, CONF_folder, folder);
                    sfree(ssd->newfolder);
                    ssd->newfolder = dupstr("");
                    ssd->folder_new_selected = 0;
                    dlg_refresh(ssd->folderlist, dlg);
                    dlg_refresh(ssd->listbox, dlg);
                    kitty_notify_launcher_sessions_changed();
                }
            } else {
                /* Create. StringList_Add dedupes internally. */
                char folder[1024];
                strncpy(folder, ssd->newfolder, sizeof(folder)-1);
                folder[sizeof(folder)-1] = '\0';
                CleanFolderName(folder);
                if (!folder[0]) {
                    dlg_beep(dlg);
                } else if (kitty_folder_name_reserved(folder)) {
                    dlg_error_msg(dlg, "That name is reserved for the root session list.");
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
        } else if (!ssd->midsession &&
                   ssd->exportbutton && ctrl == ssd->exportbutton) {
            /* Whole-store export: pick a folder, write each saved session as a
             * protected .ktx (see kitty_bridge.c). GetActiveWindow() is the
             * config box, the owner for the folder picker + result dialog. */
            extern void kitty_export_all_sessions(HWND);
            kitty_export_all_sessions(GetActiveWindow());
        } else if (!ssd->midsession &&
                   ssd->importbutton && ctrl == ssd->importbutton) {
            /* Whole-store import: pick .ktx files, load+save each as a session
             * (re-protected for this backend), then refresh the list so the
             * imported sessions show up immediately. */
            extern void kitty_import_sessions(HWND);
            kitty_import_sessions(GetActiveWindow());
            get_sesslist(&ssd->sesslist, false);
            get_sesslist(&ssd->sesslist, true);
            dlg_refresh(ssd->listbox, dlg);
            kitty_notify_launcher_sessions_changed();
#endif
        } else if (ctrl == ssd->okbutton) {
#ifdef MOD_PERSO
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
#endif
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
#ifdef MOD_PERSO
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
#endif
    }
}

struct charclass_data {
    dlgcontrol *listbox, *editbox, *button;
};

static void charclass_handler(dlgcontrol *ctrl, dlgparam *dlg,
                              void *data, int event)
{
    Conf *conf = (Conf *)data;
    struct charclass_data *ccd =
        (struct charclass_data *)ctrl->context.p;

    if (event == EVENT_REFRESH) {
        if (ctrl == ccd->listbox) {
            int i;
            dlg_update_start(ctrl, dlg);
            dlg_listbox_clear(ctrl, dlg);
            for (i = 0; i < 128; i++) {
                char str[100];
                snprintf( str, sizeof(str), "%d\t(0x%02X)\t%c\t%d", i, i,
                        (i >= 0x21 && i != 0x7F) ? i : ' ',
                        conf_get_int_int(conf, CONF_wordness, i));
                dlg_listbox_add(ctrl, dlg, str);
            }
            dlg_update_done(ctrl, dlg);
        }
    } else if (event == EVENT_ACTION) {
        if (ctrl == ccd->button) {
            char *str;
            int i, n;
            str = dlg_editbox_get(ccd->editbox, dlg);
            n = atoi(str);
            sfree(str);
            for (i = 0; i < 128; i++) {
                if (dlg_listbox_issel(ccd->listbox, dlg, i))
                    conf_set_int_int(conf, CONF_wordness, i, n);
            }
            dlg_refresh(ccd->listbox, dlg);
        }
    }
}

struct colour_data {
    dlgcontrol *listbox, *redit, *gedit, *bedit, *button;
};

/* Array of the user-visible colour names defined in the list macro in
 * putty.h */
static const char *const colours[] = {
    #define CONF_COLOUR_NAME_DECL(id,name) name,
    CONF_COLOUR_LIST(CONF_COLOUR_NAME_DECL)
    #undef CONF_COLOUR_NAME_DECL
};

static void colour_handler(dlgcontrol *ctrl, dlgparam *dlg,
                            void *data, int event)
{
    Conf *conf = (Conf *)data;
    struct colour_data *cd =
        (struct colour_data *)ctrl->context.p;
    bool update = false, clear = false;
    int r, g, b;

    if (event == EVENT_REFRESH) {
        if (ctrl == cd->listbox) {
            int i;
            dlg_update_start(ctrl, dlg);
            dlg_listbox_clear(ctrl, dlg);
            for (i = 0; i < lenof(colours); i++)
                dlg_listbox_add(ctrl, dlg, colours[i]);
            dlg_update_done(ctrl, dlg);
            clear = true;
            update = true;
        }
    } else if (event == EVENT_SELCHANGE) {
        if (ctrl == cd->listbox) {
            /* The user has selected a colour. Update the RGB text. */
            int i = dlg_listbox_index(ctrl, dlg);
            if (i < 0) {
                clear = true;
            } else {
                clear = false;
                r = conf_get_int_int(conf, CONF_colours, i*3+0);
                g = conf_get_int_int(conf, CONF_colours, i*3+1);
                b = conf_get_int_int(conf, CONF_colours, i*3+2);
            }
            update = true;
        }
    } else if (event == EVENT_VALCHANGE) {
        if (ctrl == cd->redit || ctrl == cd->gedit || ctrl == cd->bedit) {
            /* The user has changed the colour using the edit boxes. */
            char *str;
            int i, cval;

            str = dlg_editbox_get(ctrl, dlg);
            cval = atoi(str);
            sfree(str);
            if (cval > 255) cval = 255;
            if (cval < 0)   cval = 0;

            i = dlg_listbox_index(cd->listbox, dlg);
            if (i >= 0) {
                if (ctrl == cd->redit)
                    conf_set_int_int(conf, CONF_colours, i*3+0, cval);
                else if (ctrl == cd->gedit)
                    conf_set_int_int(conf, CONF_colours, i*3+1, cval);
                else if (ctrl == cd->bedit)
                    conf_set_int_int(conf, CONF_colours, i*3+2, cval);
            }
        }
    } else if (event == EVENT_ACTION) {
        if (ctrl == cd->button) {
            int i = dlg_listbox_index(cd->listbox, dlg);
            if (i < 0) {
                dlg_beep(dlg);
                return;
            }
            /*
             * Start a colour selector, which will send us an
             * EVENT_CALLBACK when it's finished and allow us to
             * pick up the results.
             */
            dlg_coloursel_start(ctrl, dlg,
                                conf_get_int_int(conf, CONF_colours, i*3+0),
                                conf_get_int_int(conf, CONF_colours, i*3+1),
                                conf_get_int_int(conf, CONF_colours, i*3+2));
        }
    } else if (event == EVENT_CALLBACK) {
        if (ctrl == cd->button) {
            int i = dlg_listbox_index(cd->listbox, dlg);
            /*
             * Collect the results of the colour selector. Will
             * return nonzero on success, or zero if the colour
             * selector did nothing (user hit Cancel, for example).
             */
            if (dlg_coloursel_results(ctrl, dlg, &r, &g, &b)) {
                conf_set_int_int(conf, CONF_colours, i*3+0, r);
                conf_set_int_int(conf, CONF_colours, i*3+1, g);
                conf_set_int_int(conf, CONF_colours, i*3+2, b);
                clear = false;
                update = true;
            }
        }
    }

    if (update) {
        if (clear) {
            dlg_editbox_set(cd->redit, dlg, "");
            dlg_editbox_set(cd->gedit, dlg, "");
            dlg_editbox_set(cd->bedit, dlg, "");
        } else {
            char buf[40];
            snprintf( buf, sizeof(buf), "%d", r); dlg_editbox_set(cd->redit, dlg, buf);
            snprintf( buf, sizeof(buf), "%d", g); dlg_editbox_set(cd->gedit, dlg, buf);
            snprintf( buf, sizeof(buf), "%d", b); dlg_editbox_set(cd->bedit, dlg, buf);
        }
    }
}

struct ttymodes_data {
    dlgcontrol *valradio, *valbox, *setbutton, *listbox;
};

static void ttymodes_handler(dlgcontrol *ctrl, dlgparam *dlg,
                             void *data, int event)
{
    Conf *conf = (Conf *)data;
    struct ttymodes_data *td =
        (struct ttymodes_data *)ctrl->context.p;

    if (event == EVENT_REFRESH) {
        if (ctrl == td->listbox) {
            char *key, *val;
            dlg_update_start(ctrl, dlg);
            dlg_listbox_clear(ctrl, dlg);
            for (val = conf_get_str_strs(conf, CONF_ttymodes, NULL, &key);
                 val != NULL;
                 val = conf_get_str_strs(conf, CONF_ttymodes, key, &key)) {
                char *disp = dupprintf("%s\t%s", key,
                                       (val[0] == 'A') ? "(auto)" :
                                       ((val[0] == 'N') ? "(don't send)"
                                                        : val+1));
                dlg_listbox_add(ctrl, dlg, disp);
                sfree(disp);
            }
            dlg_update_done(ctrl, dlg);
        } else if (ctrl == td->valradio) {
            dlg_radiobutton_set(ctrl, dlg, 0);
        }
    } else if (event == EVENT_SELCHANGE) {
        if (ctrl == td->listbox) {
            int ind = dlg_listbox_index(td->listbox, dlg);
            char *val;
            if (ind < 0) {
                return; /* no item selected */
            }
            val = conf_get_str_str(conf, CONF_ttymodes,
                                   conf_get_str_nthstrkey(conf, CONF_ttymodes,
                                                          ind));
            assert(val != NULL);
            /* Do this first to defuse side-effects on radio buttons: */
            dlg_editbox_set(td->valbox, dlg, val+1);
            dlg_radiobutton_set(td->valradio, dlg,
                                val[0] == 'A' ? 0 : (val[0] == 'N' ? 1 : 2));
        }
    } else if (event == EVENT_VALCHANGE) {
        if (ctrl == td->valbox) {
            /* If they're editing the text box, we assume they want its
             * value to be used. */
            dlg_radiobutton_set(td->valradio, dlg, 2);
        }
    } else if (event == EVENT_ACTION) {
        if (ctrl == td->setbutton) {
            int ind = dlg_listbox_index(td->listbox, dlg);
            const char *key;
            char *str, *val;
            char type;

            {
                const char types[] = {'A', 'N', 'V'};
                int button = dlg_radiobutton_get(td->valradio, dlg);
                assert(button >= 0 && button < lenof(types));
                type = types[button];
            }

            /* Construct new entry */
            if (ind >= 0) {
                key = conf_get_str_nthstrkey(conf, CONF_ttymodes, ind);
                str = (type == 'V' ? dlg_editbox_get(td->valbox, dlg)
                                   : dupstr(""));
                val = dupprintf("%c%s", type, str);
                sfree(str);
                conf_set_str_str(conf, CONF_ttymodes, key, val);
                sfree(val);
                dlg_refresh(td->listbox, dlg);
                dlg_listbox_select(td->listbox, dlg, ind);
            } else {
                /* Not a multisel listbox, so this means nothing selected */
                dlg_beep(dlg);
            }
        }
    }
}

struct environ_data {
    dlgcontrol *varbox, *valbox, *addbutton, *rembutton, *listbox;
};

static void environ_handler(dlgcontrol *ctrl, dlgparam *dlg,
                            void *data, int event)
{
    Conf *conf = (Conf *)data;
    struct environ_data *ed =
        (struct environ_data *)ctrl->context.p;

    if (event == EVENT_REFRESH) {
        if (ctrl == ed->listbox) {
            char *key, *val;
            dlg_update_start(ctrl, dlg);
            dlg_listbox_clear(ctrl, dlg);
            for (val = conf_get_str_strs(conf, CONF_environmt, NULL, &key);
                 val != NULL;
                 val = conf_get_str_strs(conf, CONF_environmt, key, &key)) {
                char *p = dupprintf("%s\t%s", key, val);
                dlg_listbox_add(ctrl, dlg, p);
                sfree(p);
            }
            dlg_update_done(ctrl, dlg);
        }
    } else if (event == EVENT_ACTION) {
        if (ctrl == ed->addbutton) {
            char *key, *val, *str;
            key = dlg_editbox_get(ed->varbox, dlg);
            if (!*key) {
                sfree(key);
                dlg_beep(dlg);
                return;
            }
            val = dlg_editbox_get(ed->valbox, dlg);
            if (!*val) {
                sfree(key);
                sfree(val);
                dlg_beep(dlg);
                return;
            }
            conf_set_str_str(conf, CONF_environmt, key, val);
            str = dupcat(key, "\t", val);
            dlg_editbox_set(ed->varbox, dlg, "");
            dlg_editbox_set(ed->valbox, dlg, "");
            sfree(str);
            sfree(key);
            sfree(val);
            dlg_refresh(ed->listbox, dlg);
        } else if (ctrl == ed->rembutton) {
            int i = dlg_listbox_index(ed->listbox, dlg);
            if (i < 0) {
                dlg_beep(dlg);
            } else {
                char *key, *val;

                key = conf_get_str_nthstrkey(conf, CONF_environmt, i);
                if (key) {
                    /* Populate controls with the entry we're about to delete
                     * for ease of editing */
                    val = conf_get_str_str(conf, CONF_environmt, key);
                    dlg_editbox_set(ed->varbox, dlg, key);
                    dlg_editbox_set(ed->valbox, dlg, val);
                    /* And delete it */
                    conf_del_str_str(conf, CONF_environmt, key);
                }
            }
            dlg_refresh(ed->listbox, dlg);
        }
    }
}

struct portfwd_data {
    dlgcontrol *addbutton, *rembutton, *listbox;
    dlgcontrol *sourcebox, *destbox, *direction;
#ifndef NO_IPV6
    dlgcontrol *addressfamily;
#endif
};

static void portfwd_handler(dlgcontrol *ctrl, dlgparam *dlg,
                            void *data, int event)
{
    Conf *conf = (Conf *)data;
    struct portfwd_data *pfd =
        (struct portfwd_data *)ctrl->context.p;

    if (event == EVENT_REFRESH) {
        if (ctrl == pfd->listbox) {
            char *key, *val;
            dlg_update_start(ctrl, dlg);
            dlg_listbox_clear(ctrl, dlg);
            for (val = conf_get_str_strs(conf, CONF_portfwd, NULL, &key);
                 val != NULL;
                 val = conf_get_str_strs(conf, CONF_portfwd, key, &key)) {
                char *p;
                if (!strcmp(val, "D")) {
                    char *L;
                    /*
                     * A dynamic forwarding is stored as L12345=D or
                     * 6L12345=D (since it's mutually exclusive with
                     * L12345=anything else), but displayed as D12345
                     * to match the fiction that 'Local', 'Remote' and
                     * 'Dynamic' are three distinct modes and also to
                     * align with OpenSSH's command line option syntax
                     * that people will already be used to. So, for
                     * display purposes, find the L in the key string
                     * and turn it into a D.
                     */
                    p = dupprintf("%s\t", key);
                    L = strchr(p, 'L');
                    if (L) *L = 'D';
                } else
                    p = dupprintf("%s\t%s", key, val);
                dlg_listbox_add(ctrl, dlg, p);
                sfree(p);
            }
            dlg_update_done(ctrl, dlg);
        } else if (ctrl == pfd->direction) {
            /*
             * Default is Local.
             */
            dlg_radiobutton_set(ctrl, dlg, 0);
#ifndef NO_IPV6
        } else if (ctrl == pfd->addressfamily) {
            dlg_radiobutton_set(ctrl, dlg, 0);
#endif
        }
    } else if (event == EVENT_ACTION) {
        if (ctrl == pfd->addbutton) {
            const char *family, *type;
            char *src, *key, *val;
            int whichbutton;

#ifndef NO_IPV6
            whichbutton = dlg_radiobutton_get(pfd->addressfamily, dlg);
            if (whichbutton == 1)
                family = "4";
            else if (whichbutton == 2)
                family = "6";
            else
#endif
                family = "";

            whichbutton = dlg_radiobutton_get(pfd->direction, dlg);
            if (whichbutton == 0)
                type = "L";
            else if (whichbutton == 1)
                type = "R";
            else
                type = "D";

            src = dlg_editbox_get(pfd->sourcebox, dlg);
            if (!*src) {
                dlg_error_msg(dlg, "You need to specify a source port number");
                sfree(src);
                return;
            }
            if (*type != 'D') {
                val = dlg_editbox_get(pfd->destbox, dlg);
                if (!*val || !host_strchr(val, ':')) {
                    dlg_error_msg(dlg,
                                  "You need to specify a destination address\n"
                                  "in the form \"host.name:port\"");
                    sfree(src);
                    sfree(val);
                    return;
                }
            } else {
                type = "L";
                val = dupstr("D");     /* special case */
            }

            key = dupcat(family, type, src);
            sfree(src);

            if (conf_get_str_str_opt(conf, CONF_portfwd, key)) {
                dlg_error_msg(dlg, "Specified forwarding already exists");
            } else {
                conf_set_str_str(conf, CONF_portfwd, key, val);
            }

            sfree(key);
            sfree(val);
            dlg_refresh(pfd->listbox, dlg);
        } else if (ctrl == pfd->rembutton) {
            int i = dlg_listbox_index(pfd->listbox, dlg);
            if (i < 0) {
                dlg_beep(dlg);
            } else {
                char *key, *p;
                const char *val;

                key = conf_get_str_nthstrkey(conf, CONF_portfwd, i);
                if (key) {
                    static const char *const afs = "A46";
                    static const char *const dirs = "LRD";
                    const char *afp;
                    int dir;
#ifndef NO_IPV6
                    int idx;
#endif

                    /* Populate controls with the entry we're about to delete
                     * for ease of editing */
                    p = key;

                    afp = strchr(afs, *p);
#ifndef NO_IPV6
                    idx = afp ? afp-afs : 0;
#endif
                    if (afp)
                        p++;
#ifndef NO_IPV6
                    dlg_radiobutton_set(pfd->addressfamily, dlg, idx);
#endif

                    dir = *p;

                    val = conf_get_str_str(conf, CONF_portfwd, key);
                    if (!strcmp(val, "D")) {
                        dir = 'D';
                        val = "";
                    }

                    dlg_radiobutton_set(pfd->direction, dlg,
                                        strchr(dirs, dir) - dirs);
                    p++;

                    dlg_editbox_set(pfd->sourcebox, dlg, p);
                    dlg_editbox_set(pfd->destbox, dlg, val);
                    /* And delete it */
                    conf_del_str_str(conf, CONF_portfwd, key);
                }
            }
            dlg_refresh(pfd->listbox, dlg);
        }
    }
}

struct manual_hostkey_data {
    dlgcontrol *addbutton, *rembutton, *listbox, *keybox;
};

static void manual_hostkey_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                   void *data, int event)
{
    Conf *conf = (Conf *)data;
    struct manual_hostkey_data *mh =
        (struct manual_hostkey_data *)ctrl->context.p;

    if (event == EVENT_REFRESH) {
        if (ctrl == mh->listbox) {
            char *key, *val;
            dlg_update_start(ctrl, dlg);
            dlg_listbox_clear(ctrl, dlg);
            for (val = conf_get_str_strs(conf, CONF_ssh_manual_hostkeys,
                                         NULL, &key);
                 val != NULL;
                 val = conf_get_str_strs(conf, CONF_ssh_manual_hostkeys,
                                         key, &key)) {
                dlg_listbox_add(ctrl, dlg, key);
            }
            dlg_update_done(ctrl, dlg);
        }
    } else if (event == EVENT_ACTION) {
        if (ctrl == mh->addbutton) {
            char *key;

            key = dlg_editbox_get(mh->keybox, dlg);
            if (!*key) {
                dlg_error_msg(dlg, "You need to specify a host key or "
                              "fingerprint");
                sfree(key);
                return;
            }

            if (!validate_manual_hostkey(key)) {
                dlg_error_msg(dlg, "Host key is not in a valid format");
            } else if (conf_get_str_str_opt(conf, CONF_ssh_manual_hostkeys,
                                            key)) {
                dlg_error_msg(dlg, "Specified host key is already listed");
            } else {
                conf_set_str_str(conf, CONF_ssh_manual_hostkeys, key, "");
            }

            sfree(key);
            dlg_refresh(mh->listbox, dlg);
        } else if (ctrl == mh->rembutton) {
            int i = dlg_listbox_index(mh->listbox, dlg);
            if (i < 0) {
                dlg_beep(dlg);
            } else {
                char *key;

                key = conf_get_str_nthstrkey(conf, CONF_ssh_manual_hostkeys, i);
                if (key) {
                    dlg_editbox_set(mh->keybox, dlg, key);
                    /* And delete it */
                    conf_del_str_str(conf, CONF_ssh_manual_hostkeys, key);
                }
            }
            dlg_refresh(mh->listbox, dlg);
        }
    }
}

static void clipboard_selector_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                       void *data, int event)
{
    Conf *conf = (Conf *)data;
    int setting = ctrl->context.i;
#ifdef NAMED_CLIPBOARDS
    int strsetting = ctrl->context2.i;
#endif

    static const struct {
        const char *name;
        int id;
    } options[] = {
        {"No action", CLIPUI_NONE},
        {CLIPNAME_IMPLICIT, CLIPUI_IMPLICIT},
        {CLIPNAME_EXPLICIT, CLIPUI_EXPLICIT},
    };

    if (event == EVENT_REFRESH) {
        int i, val = conf_get_int(conf, setting);

        dlg_update_start(ctrl, dlg);
        dlg_listbox_clear(ctrl, dlg);

#ifdef NAMED_CLIPBOARDS
        for (i = 0; i < lenof(options); i++)
            dlg_listbox_add(ctrl, dlg, options[i].name);
        if (val == CLIPUI_CUSTOM) {
            const char *sval = conf_get_str(conf, strsetting);
            for (i = 0; i < lenof(options); i++)
                if (!strcmp(sval, options[i].name))
                    break;             /* needs escaping */
            if (i < lenof(options) || sval[0] == '=') {
                char *escaped = dupcat("=", sval);
                dlg_editbox_set(ctrl, dlg, escaped);
                sfree(escaped);
            } else {
                dlg_editbox_set(ctrl, dlg, sval);
            }
        } else {
            dlg_editbox_set(ctrl, dlg, options[0].name); /* fallback */
            for (i = 0; i < lenof(options); i++)
                if (val == options[i].id)
                    dlg_editbox_set(ctrl, dlg, options[i].name);
        }
#else
        for (i = 0; i < lenof(options); i++)
            dlg_listbox_addwithid(ctrl, dlg, options[i].name, options[i].id);
        dlg_listbox_select(ctrl, dlg, 0); /* fallback */
        for (i = 0; i < lenof(options); i++)
            if (val == options[i].id)
                dlg_listbox_select(ctrl, dlg, i);
#endif
        dlg_update_done(ctrl, dlg);
    } else if (event == EVENT_SELCHANGE
#ifdef NAMED_CLIPBOARDS
               || event == EVENT_VALCHANGE
#endif
        ) {
#ifdef NAMED_CLIPBOARDS
        char *sval = dlg_editbox_get(ctrl, dlg);
        int i;

        for (i = 0; i < lenof(options); i++)
            if (!strcmp(sval, options[i].name)) {
                conf_set_int(conf, setting, options[i].id);
                conf_set_str(conf, strsetting, "");
                break;
            }
        if (i == lenof(options)) {
            conf_set_int(conf, setting, CLIPUI_CUSTOM);
            if (sval[0] == '=')
                sval++;
            conf_set_str(conf, strsetting, sval);
        }

        sfree(sval);
#else
        int index = dlg_listbox_index(ctrl, dlg);
        if (index >= 0) {
            int val = dlg_listbox_getid(ctrl, dlg, index);
            conf_set_int(conf, setting, val);
        }
#endif
    }
}

static void clipboard_control(struct controlset *s, const char *label,
                              char shortcut, int percentage, HelpCtx helpctx,
                              int setting, int strsetting)
{
#ifdef NAMED_CLIPBOARDS
    ctrl_combobox(s, label, shortcut, percentage, helpctx,
                  clipboard_selector_handler, I(setting), I(strsetting));
#else
    /* strsetting isn't needed in this case */
    ctrl_droplist(s, label, shortcut, percentage, helpctx,
                  clipboard_selector_handler, I(setting));
#endif
}

static void serial_parity_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                  void *data, int event)
{
    static const struct {
        const char *name;
        int val;
    } parities[] = {
        {"None", SER_PAR_NONE},
        {"Odd", SER_PAR_ODD},
        {"Even", SER_PAR_EVEN},
        {"Mark", SER_PAR_MARK},
        {"Space", SER_PAR_SPACE},
    };
    int mask = ctrl->context.i;
    int i, j;
    Conf *conf = (Conf *)data;

    if (event == EVENT_REFRESH) {
        /* Fetching this once at the start of the function ensures we
         * remember what the right value is supposed to be when
         * operations below cause reentrant calls to this function. */
        int oldparity = conf_get_int(conf, CONF_serparity);

        dlg_update_start(ctrl, dlg);
        dlg_listbox_clear(ctrl, dlg);
        for (i = 0; i < lenof(parities); i++)  {
            if (mask & (1 << parities[i].val))
                dlg_listbox_addwithid(ctrl, dlg, parities[i].name,
                                      parities[i].val);
        }
        for (i = j = 0; i < lenof(parities); i++) {
            if (mask & (1 << parities[i].val)) {
                if (oldparity == parities[i].val) {
                    dlg_listbox_select(ctrl, dlg, j);
                    break;
                }
                j++;
            }
        }
        if (i == lenof(parities)) {    /* an unsupported setting was chosen */
            dlg_listbox_select(ctrl, dlg, 0);
            oldparity = SER_PAR_NONE;
        }
        dlg_update_done(ctrl, dlg);
        conf_set_int(conf, CONF_serparity, oldparity);    /* restore */
    } else if (event == EVENT_SELCHANGE) {
        int i = dlg_listbox_index(ctrl, dlg);
        if (i < 0)
            i = SER_PAR_NONE;
        else
            i = dlg_listbox_getid(ctrl, dlg, i);
        conf_set_int(conf, CONF_serparity, i);
    }
}

static void serial_flow_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                void *data, int event)
{
    static const struct {
        const char *name;
        int val;
    } flows[] = {
        {"None", SER_FLOW_NONE},
        {"XON/XOFF", SER_FLOW_XONXOFF},
        {"RTS/CTS", SER_FLOW_RTSCTS},
        {"DSR/DTR", SER_FLOW_DSRDTR},
    };
    int mask = ctrl->context.i;
    int i, j;
    Conf *conf = (Conf *)data;

    if (event == EVENT_REFRESH) {
        /* Fetching this once at the start of the function ensures we
         * remember what the right value is supposed to be when
         * operations below cause reentrant calls to this function. */
        int oldflow = conf_get_int(conf, CONF_serflow);

        dlg_update_start(ctrl, dlg);
        dlg_listbox_clear(ctrl, dlg);
        for (i = 0; i < lenof(flows); i++)  {
            if (mask & (1 << flows[i].val))
                dlg_listbox_addwithid(ctrl, dlg, flows[i].name, flows[i].val);
        }
        for (i = j = 0; i < lenof(flows); i++) {
            if (mask & (1 << flows[i].val)) {
                if (oldflow == flows[i].val) {
                    dlg_listbox_select(ctrl, dlg, j);
                    break;
                }
                j++;
            }
        }
        if (i == lenof(flows)) {       /* an unsupported setting was chosen */
            dlg_listbox_select(ctrl, dlg, 0);
            oldflow = SER_FLOW_NONE;
        }
        dlg_update_done(ctrl, dlg);
        conf_set_int(conf, CONF_serflow, oldflow);/* restore */
    } else if (event == EVENT_SELCHANGE) {
        int i = dlg_listbox_index(ctrl, dlg);
        if (i < 0)
            i = SER_FLOW_NONE;
        else
            i = dlg_listbox_getid(ctrl, dlg, i);
        conf_set_int(conf, CONF_serflow, i);
    }
}

void proxy_type_handler(dlgcontrol *ctrl, dlgparam *dlg,
                        void *data, int event)
{
    Conf *conf = (Conf *)data;
    if (event == EVENT_REFRESH) {
        /*
         * We must fetch the previously configured value from the Conf
         * before we start modifying the drop-down list, otherwise the
         * spurious SELCHANGE we trigger in the process will overwrite
         * the value we wanted to keep.
         */
        int proxy_type = conf_get_int(conf, CONF_proxy_type);

        dlg_update_start(ctrl, dlg);
        dlg_listbox_clear(ctrl, dlg);

        int index_to_select = 0, current_index = 0;

#define ADD(id, title) do {                                     \
            dlg_listbox_addwithid(ctrl, dlg, title, id);        \
            if (id == proxy_type)                               \
                index_to_select = current_index;                \
            current_index++;                                    \
        } while (0)

        ADD(PROXY_NONE, "None");
        ADD(PROXY_SOCKS5, "SOCKS 5");
        ADD(PROXY_SOCKS4, "SOCKS 4");
        ADD(PROXY_HTTP, "HTTP CONNECT");
        if (ssh_proxy_supported) {
            ADD(PROXY_SSH_TCPIP, "SSH to proxy and use port forwarding");
            ADD(PROXY_SSH_EXEC, "SSH to proxy and execute a command");
            ADD(PROXY_SSH_SUBSYSTEM, "SSH to proxy and invoke a subsystem");
        }
        if (ctrl->context.i & PROXY_UI_FLAG_LOCAL) {
            ADD(PROXY_CMD, "Local (run a subprogram to connect)");
        }
        ADD(PROXY_TELNET, "'Telnet' (send an ad-hoc command)");

#undef ADD

        dlg_listbox_select(ctrl, dlg, index_to_select);

        dlg_update_done(ctrl, dlg);
    } else if (event == EVENT_SELCHANGE) {
        int i = dlg_listbox_index(ctrl, dlg);
        if (i < 0)
            i = AUTO;
        else
            i = dlg_listbox_getid(ctrl, dlg, i);
        conf_set_int(conf, CONF_proxy_type, i);
    }
}

static void host_ca_button_handler(dlgcontrol *ctrl, dlgparam *dp,
                                   void *data, int event)
{
    if (event == EVENT_ACTION)
        show_ca_config_box(dp);
}

#ifdef MOD_PERSO
void CheckVersionFromWebSite(HWND hwnd, int is_terminal);   /* kitty_win.c: query GitHub releases */
static void checkupdate_button_handler(dlgcontrol *ctrl, dlgparam *dp,
                                       void *data, int event)
{
    if (event == EVENT_ACTION)
        CheckVersionFromWebSite(GetActiveWindow(), 0);   /* config box: no live terminal */
}
#endif

#ifdef MOD_PERSO
/* KiTTY: checkbox to also show (and thus allow deleting) sessions stored in the
 * read-only fallback hives (old 9bis KiTTY + stock PuTTY). Off by default so the
 * list shows only KiTTY's own sessions and a stock-PuTTY session is never
 * deleted unless the user deliberately reveals it. Toggling re-enumerates the
 * list immediately. The flag lives in the registry (windows/storage.c). */
int  kitty_get_show_foreign_sessions(void);   /* windows/storage.c */
void kitty_set_show_foreign_sessions(int on); /* windows/storage.c */
static void kitty_showforeign_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                      void *data, int event)
{
    struct sessionsaver_data *ssd =
        (struct sessionsaver_data *)ctrl->context.p;
    if (event == EVENT_REFRESH) {
        dlg_checkbox_set(ctrl, dlg, kitty_get_show_foreign_sessions());
    } else if (event == EVENT_VALCHANGE) {
        kitty_set_show_foreign_sessions(dlg_checkbox_get(ctrl, dlg));
        /* re-enumerate so the list shows/hides the foreign sessions at once */
        get_sesslist(&ssd->sesslist, false);
        get_sesslist(&ssd->sesslist, true);
        dlg_refresh(ssd->listbox, dlg);
    }
}
#endif


/*
 * setup_config_box, split into one static helper per panel group. The
 * helper bodies are the exact former contents of the single big function
 * (moved verbatim, in the same order - panel creation order defines the
 * treeview); only the signatures and per-helper local declarations are
 * new. setup_config_box itself is now just the dispatcher at the bottom.
 */

/*
 * Each per-protocol configuration GUI panel is conditionally
 * displayed. We don't display it if this binary doesn't contain a
 * backend for its protocol at all; we don't display it if we're
 * already in mid-session with a different protocol selected; and
 * even if we _do_ have this protocol selected, we don't display
 * the panel if the protocol doesn't permit any mid-session
 * reconfiguration anyway. (Used by the SSH and serial/telnet/
 * rlogin/SUPDUP helpers below; midsession/protocol are their
 * parameters.)
 */

#define DISPLAY_RECONFIGURABLE_PROTOCOL(which_proto) \
    (backend_vt_from_proto(which_proto) && \
     (!midsession || protocol == (which_proto)))
#define DISPLAY_NON_RECONFIGURABLE_PROTOCOL(which_proto) \
    (backend_vt_from_proto(which_proto) && !midsession)

/* The bottom button bar (Open/Start/Updates/Cancel) and the Session panel. */
static void scb_panel_session(struct controlbox *b, bool midsession)
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
#ifdef MOD_PERSO
    ssd->newfolder = dupstr("");
    if (!GetPuttyFlag()) {
        char lastfolder[1024];
        if (kitty_get_last_folder(lastfolder, sizeof(lastfolder))) {
            strncpy(CurrentFolder, lastfolder, 1023);
            CurrentFolder[1023] = '\0';
        }
    }
#endif
    ssd->midsession = midsession;

    /*
     * The standard panel that appears at the bottom of all panels:
     * Open, Cancel, Apply etc.
     */
    s = ctrl_getset(b, "", "", "");
    ctrl_columns(s, 5, 20, 20, 20, 20, 20);
    ssd->okbutton = ctrl_pushbutton(s,
                                    (midsession ? "Apply" : "Open"),
                                    (char)(midsession ? 'a' : 'o'),
                                    HELPCTX(no_help),
                                    sessionsaver_handler, P(ssd));
    ssd->okbutton->button.isdefault = true;
    ssd->okbutton->column = 3;
#ifdef MOD_PERSO
    /* KiTTY "Start": launch the session in a new window without closing the
     * config box (only when launchable). col 2 is free in this 5-col row. */
    if (!midsession && !GetPuttyFlag()) {
        ssd->startbutton = ctrl_pushbutton(s, "Start", NO_SHORTCUT,
                                           HELPCTX(no_help),
                                           sessionsaver_handler, P(ssd));
        ssd->startbutton->column = 2;
    } else {
        ssd->startbutton = NULL;
    }
    /* KiTTY "Check for updates": col 1 sits between About (col 0, added by
     * win_setup_config_box) and Start (col 2). KiTTY ships no Help button
     * (has_help() is false — no embedded CHM), so col 1 is free. */
    if (!midsession && !GetPuttyFlag()) {
        /* Short label: the button is only ~20% of the dialog width (one of 5
         * columns), so "Check for updates" overflows. */
        c = ctrl_pushbutton(s, "Updates", NO_SHORTCUT,
                            HELPCTX(no_help), checkupdate_button_handler, P(NULL));
        c->column = 1;
    }
#endif
    ssd->cancelbutton = ctrl_pushbutton(s, "Cancel", 'c', HELPCTX(no_help),
                                        sessionsaver_handler, P(ssd));
    ssd->cancelbutton->button.iscancel = true;
    ssd->cancelbutton->column = 4;
    /* We carefully don't close the 5-column part, so that platform-
     * specific add-ons can put extra buttons alongside Open and Cancel. */

    /*
     * The Session panel.
     */
    str = dupprintf("Basic options for your %s session", appname);

    if (!midsession) {
        struct hostport *hp = (struct hostport *)
            ctrl_alloc(b, sizeof(struct hostport));
        memset(hp, 0, sizeof(*hp));

        s = ctrl_getset(b, "Session", "hostport", str);
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

        c->radio.buttons[c->radio.nbuttons] = dupstr("Other:");
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
    sfree(str);

    /*
     * The Load/Save panel is available even in mid-session.
     */
    s = ctrl_getset(b, "Session", "savedsessions",
                    midsession ? "Save the current session settings" :
                    "Load, save or delete a stored session");
    ctrl_columns(s, 2, 75, 25);
    get_sesslist(&ssd->sesslist, true);
    ssd->editbox = ctrl_editbox(s, NULL, 'e', 100,
                                HELPCTX(session_saved),
                                sessionsaver_handler, P(ssd), P(NULL));
    ssd->editbox->column = 0;
    session_filter_ctrl = ssd->editbox;   /* Ctrl+F jump target, see above */
    session_filter_ssd = ssd;             /* Ctrl+G root-folder jump */
    ssd->savebutton = ctrl_pushbutton(s, "Save", 'v',
                                      HELPCTX(session_saved),
                                      sessionsaver_handler, P(ssd));
    ssd->savebutton->column = 1;
    ssd->savebutton->align_next_to = ssd->editbox;   /* centre on the name field */
    ctrl_columns(s, 1, 100);
    ctrl_columns(s, 2, 75, 25);
    /* Folder selector + create button share their own synchronized row. */
#ifdef MOD_PERSO
    /* KiTTY: editable folder selector. Selecting a folder filters the list.
     * Creating one is an explicit choice - pick the synthetic "<new folder...>"
     * row, type the name, press New folder. With the root list selected instead,
     * a typed name renames its LABEL (display only) and the button says so. */
    if (!GetPuttyFlag()) {
        ssd->folderlist = ctrl_combobox(s, NULL, NO_SHORTCUT, 100,
                                        HELPCTX(session_saved),
                                        sessionsaver_handler, P(ssd), P(NULL));
        ssd->folderlist->column = 0;
        if (!midsession) {
            ssd->createbutton = ctrl_pushbutton(s, "New folder", NO_SHORTCUT,
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
        dlgcontrol *pc = ctrl_droplist(s, "Proxy choice", NO_SHORTCUT, 100,
                                       HELPCTX(session_saved),
                                       kitty_proxy_handler, P(NULL));
        pc->column = 0;
        if (!midsession) {
            dlgcontrol *pe = ctrl_pushbutton(s, "Edit", NO_SHORTCUT,
                                             HELPCTX(session_saved),
                                             kitty_proxyedit_handler, P(NULL));
            pe->column = 1;
            pe->align_next_to = pc;   /* centre the button on the droplist */
        }
        /* leave the row at 2 columns; the ctrl_columns(1) after #endif merges it,
         * matching the no-proxy path (a 1->1 transition would assert). */
    }
#endif
    ctrl_columns(s, 1, 100);
    ctrl_columns(s, 2, 75, 25);
    ssd->listbox = ctrl_listbox(s, NULL, NO_SHORTCUT,
                                HELPCTX(session_saved),
                                sessionsaver_handler, P(ssd));
    ssd->listbox->column = 0;
    /* KiTTY: the saved-session list height is user-configurable via kitty.ini
     * [ConfigBox] height (GetConfigBoxHeight(), default 21). The column-1
     * buttons are distributed down that height — Load at the top, Delete +
     * Del folder in the upper-centre, Export/Import at the bottom — using blank
     * ctrl_text spacer rows whose COUNT scales with the height so the layout
     * holds at any configured size. Calibrated at height 16 (2 spacers each
     * side); each extra list row adds ~1 spacer, split between the two gaps. */
    {
        extern int GetConfigBoxHeight(void);   /* kitty.c: [ConfigBox] height */
        int cbh = GetConfigBoxHeight();
        if (cbh < 7) cbh = 7;                  /* keep a usable minimum */
        ssd->listbox->listbox.height = cbh;
        ssd->cb_top_spacers = (cbh - 12) > 0 ? (cbh - 12) / 2 : 0;
        ssd->cb_bot_spacers = (cbh - 12) > 0 ? (cbh - 12) - ssd->cb_top_spacers : 0;
    }
    if (!midsession) {
        ssd->loadbutton = ctrl_pushbutton(s, "Load", 'l',       /* top */
                                          HELPCTX(session_saved),
                                          sessionsaver_handler, P(ssd));
        ssd->loadbutton->column = 1;
        for (int k = 0; k < ssd->cb_top_spacers; k++)
            ctrl_text(s, "", HELPCTX(no_help))->column = 1;
        ssd->delbutton = ctrl_pushbutton(s, "Delete", 'd',      /* upper-centre */
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
#ifdef MOD_PERSO
    /* KiTTY: destructive folder button lives with the other destructive action
     * beside the session list. */
    if (!midsession && !GetPuttyFlag()) {
        ssd->delfolderbutton = ctrl_pushbutton(s, "Del folder", NO_SHORTCUT,
                                               HELPCTX(session_saved),
                                               sessionsaver_handler, P(ssd));
        ssd->delfolderbutton->column = 1;      /* upper-centre, just under Delete */
        for (int k = 0; k < ssd->cb_bot_spacers; k++)
            ctrl_text(s, "", HELPCTX(no_help))->column = 1;   /* anchor bottom group */
        /* Whole-store move (portable/new-PC): export every saved session to a
         * folder as protected .ktx files, or import .ktx files back. Bottom of
         * the column so Import's foot lines up with the listbox bottom. Store
         * management, not a per-connection action (cf. "Export current"). */
        ssd->exportbutton = ctrl_pushbutton(s, "Export all...", NO_SHORTCUT,
                                            HELPCTX(session_saved),
                                            sessionsaver_handler, P(ssd));
        ssd->exportbutton->column = 1;         /* bottom */
        ssd->importbutton = ctrl_pushbutton(s, "Import all...", NO_SHORTCUT,
                                            HELPCTX(session_saved),
                                            sessionsaver_handler, P(ssd));
        ssd->importbutton->column = 1;         /* bottom */
    } else {
        /* Defensive only: setup_config_box already memsets ssd to 0. */
        ssd->createbutton = NULL;
        ssd->delfolderbutton = NULL;
        ssd->exportbutton = NULL;
        ssd->importbutton = NULL;
    }
#endif
    ctrl_columns(s, 1, 100);
#ifdef MOD_PERSO
    /* KiTTY: read-only display of the selected session's comment, below the list.
     * Empty comments show a clear placeholder directly inside the field. */
    if (!GetPuttyFlag()) {
        ssd->commentbox = ctrl_editbox_multiline(
            s, NULL, NO_SHORTCUT, 3, true,
            HELPCTX(session_saved), sessionsaver_handler, P(ssd), P(NULL));
    } else {
        ssd->commentbox = NULL;
    }
#endif

    s = ctrl_getset(b, "Session", "otheropts", NULL);
    /* Stock PuTTY's "Close window on exit" comes FIRST so users coming from
     * PuTTY find the familiar control where they expect it; all KiTTY-added
     * per-session options are grouped below it (hknet/KiTTY#11). */
    ctrl_radiobuttons(s, "Close window on exit:", 'x', 4,
                      HELPCTX(session_coe),
                      conf_radiobutton_handler,
                      I(CONF_close_on_exit),
                      "Always", I(FORCE_ON),
                      "Never", I(FORCE_OFF),
                      "Only on clean exit", I(AUTO));
#ifdef MOD_PERSO
    if (!GetPuttyFlag()) {
        /* KiTTY: writes this session back when its window closes - the settings
         * as they stand at that moment, so a font or colour changed mid-session
         * survives, plus the window's size, position and maximised state.
         *
         * It stays on the SESSION panel: it saves the whole session, which is a
         * session concern, not window behaviour (only "Remember window
         * position" moved to Window > Behaviour). Its implementation had been
         * lost in the port - the box did nothing at all - and classic KiTTY's
         * version rewrote the session through load_settings()+save_settings()
         * merely to keep the coordinates. An unnamed session and "Default
         * Settings" are deliberately never written. */
        ctrl_checkbox(s, "Save settings on exit", NO_SHORTCUT,
                      HELPCTX(no_help), conf_checkbox_handler,
                      I(CONF_saveonexit));
        /* KiTTY: exclude this session from the kitty -launcher tray menu. */
        ctrl_checkbox(s, "Hide this session from the launcher", NO_SHORTCUT,
                      HELPCTX(no_help), conf_checkbox_handler,
                      I(CONF_launcherhide));
    }

    /* KiTTY: settings about the APPLICATION rather than this connection, in
     * their own box so they stop reading as session options. They ended up on
     * the Session panel because there is nowhere else for app-wide settings
     * yet; the planned kitty-settings page is their proper home.
     *
     * NOTE the two are not alike underneath: the foreign-session toggle is
     * genuinely global (straight to storage, immediate effect), while
     * "Check for updates" is still CONF_check_update_startup - saved into each
     * session and read from the session's conf when its window opens. Hence the
     * neutral "Application" title rather than a claim about how they are
     * stored. Making update-check truly global is a compat change (every saved
     * session already carries CheckUpdateStartup) and is parked for the
     * kitty-settings page. */
    if (!GetPuttyFlag()) {
        s = ctrl_getset(b, "Session", "kittyapp", "Application");
        /* On startup, check for a newer release and show a one-line notice in
         * the terminal when a session opens. */
        ctrl_checkbox(s, "Check for updates", NO_SHORTCUT,
                      HELPCTX(no_help), conf_checkbox_handler,
                      I(CONF_check_update_startup));
        {
            extern int GetIniFileFlag(void);   /* kitty_commun.c (SAVEMODE_REG/FILE/DIR) */
            extern int kitty_has_foreign_sessions(void); /* windows/storage.c */
            /* Registry-only, and only when there is actually an old 9bis-KiTTY /
             * stock-PuTTY hive with sessions to reveal (a no-op portable,
             * pointless on a machine that never had old KiTTY or PuTTY). Last
             * in the box so that when it is hidden the box still ends cleanly
             * on the checkbox above, with no gap. */
            if (GetIniFileFlag() == 0 /* SAVEMODE_REG */ &&
                kitty_has_foreign_sessions()) {
                ctrl_checkbox(s, "show / edit / delete old putty/kitty sessions",
                              NO_SHORTCUT, HELPCTX(no_help),
                              kitty_showforeign_handler, P(ssd));
            }
        }
    }
#endif
}

/* The Session/Logging panel. */
static void scb_panel_logging(struct controlbox *b, bool midsession, int protocol)
{
    struct controlset *s;

    /*
     * The Session/Logging panel.
     */
    ctrl_settitle(b, "Session/Logging", "Options controlling session logging");

    s = ctrl_getset(b, "Session/Logging", "main", NULL);
    /*
     * The logging buttons change depending on whether SSH packet
     * logging can sensibly be available.
     */
    {
        const char *sshlogname, *sshrawlogname;
        if ((midsession && protocol == PROT_SSH) ||
            (!midsession && backend_vt_from_proto(PROT_SSH))) {
            sshlogname = "SSH packets";
            sshrawlogname = "SSH packets and raw data";
        } else {
            sshlogname = NULL;         /* this will disable both buttons */
            sshrawlogname = NULL;      /* this will just placate optimisers */
        }
        ctrl_radiobuttons(s, "Session logging:", NO_SHORTCUT, 2,
                          HELPCTX(logging_main),
                          loggingbuttons_handler,
                          I(CONF_logtype),
                          "None", 't', I(LGTYP_NONE),
                          "Printable output", 'p', I(LGTYP_ASCII),
                          "All session output", 'l', I(LGTYP_DEBUG),
                          sshlogname, 's', I(LGTYP_PACKETS),
                          sshrawlogname, 'r', I(LGTYP_SSHRAW));
    }
    ctrl_filesel(s, "Log file name:", 'f',
                 FILTER_ALL_FILES, true, "Select session log file name",
                 HELPCTX(logging_filename),
                 conf_filesel_handler, I(CONF_logfilename));
    ctrl_text(s, "(Log file name can contain &Y, &M, &D for date,"
              " &T for time, &H for host name, and &P for port number)",
              HELPCTX(logging_filename));
    ctrl_radiobuttons(s, "What to do if the log file already exists:", 'e', 1,
                      HELPCTX(logging_exists),
                      conf_radiobutton_handler, I(CONF_logxfovr),
                      "Always overwrite it", I(LGXF_OVR),
                      "Always append to the end of it", I(LGXF_APN),
                      "Ask the user every time", I(LGXF_ASK));
    ctrl_checkbox(s, "Flush log file frequently", 'u',
                  HELPCTX(logging_flush),
                  conf_checkbox_handler, I(CONF_logflush));
    ctrl_checkbox(s, "Include header", 'i',
                  HELPCTX(logging_header),
                  conf_checkbox_handler, I(CONF_logheader));
#ifdef MOD_PERSO
    if (!GetPuttyFlag()) {
        ctrl_editbox(s, "Log rotation delay (sec, 0=off)", NO_SHORTCUT, 50,
                     HELPCTX(no_help),
                     conf_editbox_handler, I(CONF_logtimerotation), ED_INT);
        ctrl_editbox(s, "Timestamp (strftime format)", NO_SHORTCUT, 100,
                     HELPCTX(no_help),
                     conf_editbox_handler, I(CONF_logtimestamp), ED_STR);
    }
#endif

    if ((midsession && protocol == PROT_SSH) ||
        (!midsession && backend_vt_from_proto(PROT_SSH))) {
        s = ctrl_getset(b, "Session/Logging", "ssh",
                        "Options specific to SSH packet logging");
        ctrl_checkbox(s, "Omit known password fields", 'k',
                      HELPCTX(logging_ssh_omit_password),
                      conf_checkbox_handler, I(CONF_logomitpass));
        ctrl_checkbox(s, "Omit session data", 'd',
                      HELPCTX(logging_ssh_omit_data),
                      conf_checkbox_handler, I(CONF_logomitdata));
    }
}

/* The Session/Scripting panel (KiTTY rutty scripting). */
static void scb_panel_scripting(struct controlbox *b)
{
    struct controlset *s;

    /*
     * The Session/Scripting panel (KiTTY rutty scripting). Placed under Session
     * to match upstream KiTTY's layout, immediately after Session/Logging so the
     * config treeview builds in a valid order (no implicit parent path).
     */
    if (!GetPuttyFlag()) {
        ctrl_settitle(b, "Session/Scripting",
                      "Options controlling automated scripting");
        s = ctrl_getset(b, "Session/Scripting", "main",
                        "Send a script file to the host");
        ctrl_checkbox(s, "Run the script on connect", NO_SHORTCUT,
                      HELPCTX(no_help), kitty_checkbox_int_handler,
                      I(CONF_script_mode));
        ctrl_filesel(s, "Script file:", NO_SHORTCUT,
                     FILTER_ALL_FILES, false, "Select script file",
                     HELPCTX(no_help),
                     conf_filesel_handler, I(CONF_scriptfile));
        ctrl_checkbox(s, "Wait for a prompt before each line", NO_SHORTCUT,
                      HELPCTX(no_help), kitty_checkbox_int_handler,
                      I(CONF_script_enable));
        ctrl_editbox(s, "Wait-for text:", NO_SHORTCUT, 60,
                     HELPCTX(no_help), conf_editbox_handler,
                     I(CONF_script_waitfor), ED_STR);
        ctrl_editbox(s, "Halt-on text:", NO_SHORTCUT, 60,
                     HELPCTX(no_help), conf_editbox_handler,
                     I(CONF_script_halton), ED_STR);
        ctrl_editbox(s, "Line delay (ms):", NO_SHORTCUT, 30,
                     HELPCTX(no_help), conf_editbox_handler,
                     I(CONF_script_line_delay), ED_INT);
        ctrl_editbox(s, "Timeout (s):", NO_SHORTCUT, 30,
                     HELPCTX(no_help), conf_editbox_handler,
                     I(CONF_script_timeout), ED_INT);
        ctrl_editbox(s, "Character delay (ms):", NO_SHORTCUT, 30,
                     HELPCTX(no_help), conf_editbox_handler,
                     I(CONF_script_char_delay), ED_INT);
        ctrl_editbox(s, "Start of condition/comment line:", NO_SHORTCUT, 30,
                     HELPCTX(no_help), conf_editbox_handler,
                     I(CONF_script_cond_line), ED_STR);
        ctrl_radiobuttons(s, "CR/LF translation:", NO_SHORTCUT, 4,
                          HELPCTX(no_help), conf_radiobutton_handler,
                          I(CONF_script_crlf),
                          "Off",   NO_SHORTCUT, I(0),   /* SCRIPT_OFF  */
                          "no LF", NO_SHORTCUT, I(1),   /* SCRIPT_NOLF */
                          "CR",    NO_SHORTCUT, I(2),   /* SCRIPT_CR   */
                          "Rec",   NO_SHORTCUT, I(3));  /* SCRIPT_REC  */
        ctrl_checkbox(s, "Except for first command", NO_SHORTCUT,
                      HELPCTX(no_help), kitty_checkbox_int_handler,
                      I(CONF_script_except));
        ctrl_checkbox(s, "Use conditions from file", NO_SHORTCUT,
                      HELPCTX(no_help), kitty_checkbox_int_handler,
                      I(CONF_script_cond_use));
    }
}

/* The Terminal panel and its Keyboard/Bell/Features sub-panels. */
static void scb_panel_terminal(struct controlbox *b)
{
    struct controlset *s;

    /*
     * The Terminal panel.
     */
    ctrl_settitle(b, "Terminal", "Options controlling the terminal emulation");

    s = ctrl_getset(b, "Terminal", "general", "Set various terminal options");
    ctrl_checkbox(s, "Auto wrap mode initially on", 'w',
                  HELPCTX(terminal_autowrap),
                  conf_checkbox_handler, I(CONF_wrap_mode));
    ctrl_checkbox(s, "DEC Origin Mode initially on", 'd',
                  HELPCTX(terminal_decom),
                  conf_checkbox_handler, I(CONF_dec_om));
    ctrl_checkbox(s, "Implicit CR in every LF", 'r',
                  HELPCTX(terminal_lfhascr),
                  conf_checkbox_handler, I(CONF_lfhascr));
    ctrl_checkbox(s, "Implicit LF in every CR", 'f',
                  HELPCTX(terminal_crhaslf),
                  conf_checkbox_handler, I(CONF_crhaslf));
    ctrl_checkbox(s, "Use background colour to erase screen", 'e',
                  HELPCTX(terminal_bce),
                  conf_checkbox_handler, I(CONF_bce));
    ctrl_checkbox(s, "Enable blinking text", 'n',
                  HELPCTX(terminal_blink),
                  conf_checkbox_handler, I(CONF_blinktext));
    ctrl_editbox(s, "Answerback to ^E:", 's', 100,
                 HELPCTX(terminal_answerback),
                 conf_editbox_handler, I(CONF_answerback), ED_STR);

    s = ctrl_getset(b, "Terminal", "ldisc", "Line discipline options");
    ctrl_radiobuttons(s, "Local echo:", 'l', 3,
                      HELPCTX(terminal_localecho),
                      conf_radiobutton_handler,I(CONF_localecho),
                      "Auto", I(AUTO),
                      "Force on", I(FORCE_ON),
                      "Force off", I(FORCE_OFF));
    ctrl_radiobuttons(s, "Local line editing:", 't', 3,
                      HELPCTX(terminal_localedit),
                      conf_radiobutton_handler,I(CONF_localedit),
                      "Auto", I(AUTO),
                      "Force on", I(FORCE_ON),
                      "Force off", I(FORCE_OFF));

    s = ctrl_getset(b, "Terminal", "printing", "Remote-controlled printing");
    ctrl_combobox(s, "Printer to send ANSI printer output to:", 'p', 100,
                  HELPCTX(terminal_printing),
                  printerbox_handler, P(NULL), P(NULL));
#ifdef MOD_PRINTCLIP
    if (!GetPuttyFlag()) {
        ctrl_checkbox(s, "Print to clipboard instead of printer", NO_SHORTCUT,
                      HELPCTX(no_help), kitty_printclip_handler,
                      I(CONF_printclip));
    }
#endif

    /*
     * The Terminal/Keyboard panel.
     */
    ctrl_settitle(b, "Terminal/Keyboard",
                  "Options controlling the effects of keys");

    s = ctrl_getset(b, "Terminal/Keyboard", "mappings",
                    "Change the sequences sent by:");
    ctrl_radiobuttons(s, "The Backspace key", 'b', 2,
                      HELPCTX(keyboard_backspace),
                      conf_radiobutton_bool_handler,
                      I(CONF_bksp_is_delete),
                      "Control-H", I(0), "Control-? (127)", I(1));
    ctrl_radiobuttons(s, "The Home and End keys", 'e', 2,
                      HELPCTX(keyboard_homeend),
                      conf_radiobutton_bool_handler,
                      I(CONF_rxvt_homeend),
                      "Standard", I(false), "rxvt", I(true));
    ctrl_radiobuttons(s, "The Function keys and keypad", 'f', 4,
                      HELPCTX(keyboard_funkeys),
                      conf_radiobutton_handler,
                      I(CONF_funky_type),
                      "ESC[n~", I(FUNKY_TILDE),
                      "Linux", I(FUNKY_LINUX),
                      "Xterm R6", I(FUNKY_XTERM),
                      "VT400", I(FUNKY_VT400),
                      "VT100+", I(FUNKY_VT100P),
                      "SCO", I(FUNKY_SCO),
                      "Xterm 216+", I(FUNKY_XTERM_216));
    ctrl_radiobuttons(s, "Shift/Ctrl/Alt with the arrow keys", 'w', 2,
                      HELPCTX(keyboard_sharrow),
                      conf_radiobutton_handler,
                      I(CONF_sharrow_type),
                      "Ctrl toggles app mode", I(SHARROW_APPLICATION),
                      "xterm-style bitmap", I(SHARROW_BITMAP));
    ctrl_radiobuttons(s, "Word navigation (Left/Right arrows)", 'v', 3,
                      HELPCTX(no_help),
                      conf_radiobutton_handler,
                      I(CONF_word_nav_modifier),
                      "Alt", I(WORDNAV_ALT),
                      "Ctrl", I(WORDNAV_CTRL),
                      "Both", I(WORDNAV_BOTH));
#ifdef MOD_PERSO
    if (!GetPuttyFlag())
        ctrl_checkbox(s, "Enter key sends CR LF", NO_SHORTCUT,
                      HELPCTX(no_help), kitty_checkbox_int_handler,
                      I(CONF_enter_sends_crlf));
#endif
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
        ctrl_checkbox(s, "Disable AltGr: right Alt acts as Alt, not as a character key",
                      NO_SHORTCUT, HELPCTX(no_help), kitty_checkbox_int_handler,
                      I(CONF_disablealtgr));
        ctrl_text(s, "Off (default): AltGr composes characters on international "
                     "layouts (AltGr+Q = @). On: AltGr+key sends Alt+key instead. "
                     "Unrelated to \"AltGr acts as Compose key\" above, which is "
                     "PuTTY's two-keystroke Compose feature.",
                  HELPCTX(no_help));
    }
#endif

    s = ctrl_getset(b, "Terminal/Keyboard", "appkeypad",
                    "Application keypad settings:");
    ctrl_radiobuttons(s, "Initial state of cursor keys:", 'r', 3,
                      HELPCTX(keyboard_appcursor),
                      conf_radiobutton_bool_handler,
                      I(CONF_app_cursor),
                      "Normal", I(0), "Application", I(1));
    ctrl_radiobuttons(s, "Initial state of numeric keypad:", 'n', 3,
                      HELPCTX(keyboard_appkeypad),
                      numeric_keypad_handler, P(NULL),
                      "Normal", I(0), "Application", I(1), "NetHack", I(2));

    /*
     * The Terminal/Bell panel.
     */
    ctrl_settitle(b, "Terminal/Bell",
                  "Options controlling the terminal bell");

    s = ctrl_getset(b, "Terminal/Bell", "style", "Set the style of bell");
    ctrl_radiobuttons(s, "Action to happen when a bell occurs:", 'b', 1,
                      HELPCTX(bell_style),
                      conf_radiobutton_handler, I(CONF_beep),
                      "None (bell disabled)", I(BELL_DISABLED),
                      "Make default system alert sound", I(BELL_DEFAULT),
                      "Visual bell (flash window)", I(BELL_VISUAL));
#ifdef MOD_PERSO
    if (!GetPuttyFlag()) {
        ctrl_checkbox(s, "Put window in foreground on bell", NO_SHORTCUT,
                      HELPCTX(no_help), conf_checkbox_handler,
                      I(CONF_foreground_on_bell));
    }
#endif

    s = ctrl_getset(b, "Terminal/Bell", "overload",
                    "Control the bell overload behaviour");
    ctrl_checkbox(s, "Bell is temporarily disabled when over-used", 'd',
                  HELPCTX(bell_overload),
                  conf_checkbox_handler, I(CONF_bellovl));
    ctrl_editbox(s, "Over-use means this many bells...", 'm', 20,
                 HELPCTX(bell_overload),
                 conf_editbox_handler, I(CONF_bellovl_n), ED_INT);

    static const struct conf_editbox_handler_type conf_editbox_tickspersec = {
        .type = EDIT_FIXEDPOINT, .denominator = TICKSPERSEC};

    ctrl_editbox(s, "... in this many seconds", 't', 20,
                 HELPCTX(bell_overload),
                 conf_editbox_handler, I(CONF_bellovl_t),
                 CP(&conf_editbox_tickspersec));
    ctrl_text(s, "The bell is re-enabled after a few seconds of silence.",
              HELPCTX(bell_overload));
    ctrl_editbox(s, "Seconds of silence required", 's', 20,
                 HELPCTX(bell_overload),
                 conf_editbox_handler, I(CONF_bellovl_s),
                 CP(&conf_editbox_tickspersec));

    /*
     * The Terminal/Features panel.
     */
    ctrl_settitle(b, "Terminal/Features",
                  "Enabling and disabling advanced terminal features");

    s = ctrl_getset(b, "Terminal/Features", "main", NULL);
    ctrl_checkbox(s, "Disable application cursor keys mode", 'u',
                  HELPCTX(features_application),
                  conf_checkbox_handler, I(CONF_no_applic_c));
    ctrl_checkbox(s, "Disable application keypad mode", 'k',
                  HELPCTX(features_application),
                  conf_checkbox_handler, I(CONF_no_applic_k));
    ctrl_checkbox(s, "Disable xterm-style mouse reporting", 'x',
                  HELPCTX(features_mouse),
                  conf_checkbox_handler, I(CONF_no_mouse_rep));
    ctrl_checkbox(s, "Disable remote-controlled terminal resizing", 's',
                  HELPCTX(features_resize),
                  conf_checkbox_handler,
                  I(CONF_no_remote_resize));
    ctrl_checkbox(s, "Disable switching to alternate terminal screen", 'w',
                  HELPCTX(features_altscreen),
                  conf_checkbox_handler, I(CONF_no_alt_screen));
    ctrl_checkbox(s, "Disable remote-controlled window title changing", 't',
                  HELPCTX(features_retitle),
                  conf_checkbox_handler,
                  I(CONF_no_remote_wintitle));
    ctrl_radiobuttons(s, "Response to remote title query (SECURITY):", 'q', 3,
                      HELPCTX(features_qtitle),
                      conf_radiobutton_handler,
                      I(CONF_remote_qtitle_action),
                      "None", I(TITLE_NONE),
                      "Empty string", I(TITLE_EMPTY),
                      "Window title", I(TITLE_REAL));
    ctrl_checkbox(s, "Disable remote-controlled clearing of scrollback", 'e',
                  HELPCTX(features_clearscroll),
                  conf_checkbox_handler,
                  I(CONF_no_remote_clearscroll));
    ctrl_checkbox(s, "Disable destructive backspace on server sending ^?",'b',
                  HELPCTX(features_dbackspace),
                  conf_checkbox_handler, I(CONF_no_dbackspace));
    ctrl_checkbox(s, "Disable remote-controlled character set configuration",
                  'r', HELPCTX(features_charset), conf_checkbox_handler,
                  I(CONF_no_remote_charset));
    ctrl_checkbox(s, "Disable Arabic text shaping",
                  'l', HELPCTX(features_arabicshaping), conf_checkbox_handler,
                  I(CONF_no_arabicshaping));
    ctrl_checkbox(s, "Disable bidirectional text display",
                  'd', HELPCTX(features_bidi), conf_checkbox_handler,
                  I(CONF_no_bidi));
    ctrl_checkbox(s, "Disable bracketed paste mode",
                  'p', HELPCTX(features_bracketed_paste), conf_checkbox_handler,
                  I(CONF_no_bracketed_paste));
#ifdef MOD_PERSO
    if (!GetPuttyFlag())
        ctrl_checkbox(s, "Disable focus reporting", NO_SHORTCUT,
                      HELPCTX(no_help), conf_checkbox_handler,
                      I(CONF_no_focus_rep));
#endif
}

/* The Window panel and its Appearance/Behaviour sub-panels, plus the
 * KiTTY Transparency/Hyperlinks/position+icon/Background-image panels. */
static void scb_panel_window(struct controlbox *b, bool midsession, int protocol)
{
    const struct BackendVtable *backvt;
    struct controlset *s;
    dlgcontrol *c;
    bool resize_forbidden = false;
    char *str;

    /*
     * The Window panel.
     */
    str = dupprintf("Options controlling %s's window", appname);
    ctrl_settitle(b, "Window", str);
    sfree(str);

    backvt = backend_vt_from_proto(protocol);
    if (backvt)
        resize_forbidden = (backvt->flags & BACKEND_RESIZE_FORBIDDEN);

    if (!resize_forbidden || !midsession) {
        s = ctrl_getset(b, "Window", "size", "Set the size of the window");
        ctrl_columns(s, 2, 50, 50);
        c = ctrl_editbox(s, "Columns", 'm', 100,
                         HELPCTX(window_size),
                         conf_editbox_handler, I(CONF_width), ED_INT);
        c->column = 0;
        c = ctrl_editbox(s, "Rows", 'r', 100,
                         HELPCTX(window_size),
                         conf_editbox_handler, I(CONF_height),ED_INT);
        c->column = 1;
        ctrl_columns(s, 1, 100);
    }

    s = ctrl_getset(b, "Window", "scrollback",
                    "Control the scrollback in the window");
    ctrl_editbox(s, "Lines of scrollback", 's', 50,
                 HELPCTX(window_scrollback),
                 conf_editbox_handler, I(CONF_savelines), ED_INT);
    ctrl_checkbox(s, "Display scrollbar", 'd',
                  HELPCTX(window_scrollback),
                  conf_checkbox_handler, I(CONF_scrollbar));
#ifdef MOD_PERSO
    if (!GetPuttyFlag())
        ctrl_editbox(s, "Lines scrolled per wheel turn"
                     " (-1 half / -2 full screen)", NO_SHORTCUT, 50,
                     HELPCTX(no_help),
                     conf_editbox_handler, I(CONF_scrolllines), ED_INT);
#endif
    ctrl_checkbox(s, "Reset scrollback on keypress", 'k',
                  HELPCTX(window_scrollback),
                  conf_checkbox_handler, I(CONF_scroll_on_key));
    ctrl_checkbox(s, "Reset scrollback on display activity", 'p',
                  HELPCTX(window_scrollback),
                  conf_checkbox_handler, I(CONF_scroll_on_disp));
    ctrl_checkbox(s, "Push erased text into scrollback", 'e',
                  HELPCTX(window_erased),
                  conf_checkbox_handler,
                  I(CONF_erase_to_scrollback));

    /*
     * The Window/Appearance panel.
     */
    str = dupprintf("Configure the appearance of %s's window", appname);
    ctrl_settitle(b, "Window/Appearance", str);
    sfree(str);

    s = ctrl_getset(b, "Window/Appearance", "cursor",
                    "Adjust the use of the cursor");
    ctrl_radiobuttons(s, "Cursor appearance:", NO_SHORTCUT, 3,
                      HELPCTX(appearance_cursor),
                      conf_radiobutton_handler,
                      I(CONF_cursor_type),
                      "Block", 'l', I(CURSOR_BLOCK),
                      "Underline", 'u', I(CURSOR_UNDERLINE),
                      "Vertical line", 'v', I(CURSOR_VERTICAL_LINE));
    ctrl_checkbox(s, "Cursor blinks", 'b',
                  HELPCTX(appearance_cursor),
                  conf_checkbox_handler, I(CONF_blink_cur));

    s = ctrl_getset(b, "Window/Appearance", "font",
                    "Font settings");
    ctrl_fontsel(s, "Font used in the terminal window", 'n',
                 HELPCTX(appearance_font),
                 conf_fontsel_handler, I(CONF_font));

    s = ctrl_getset(b, "Window/Appearance", "mouse",
                    "Adjust the use of the mouse pointer");
    ctrl_checkbox(s, "Hide mouse pointer when typing in window", 'p',
                  HELPCTX(appearance_hidemouse),
                  conf_checkbox_handler, I(CONF_hide_mouseptr));

    s = ctrl_getset(b, "Window/Appearance", "border",
                    "Adjust the window border");
    ctrl_editbox(s, "Gap between text and window edge:", 'e', 20,
                 HELPCTX(appearance_border),
                 conf_editbox_handler,
                 I(CONF_window_border), ED_INT);

    /*
     * The Window/Behaviour panel.
     */
    str = dupprintf("Configure the behaviour of %s's window", appname);
    ctrl_settitle(b, "Window/Behaviour", str);
    sfree(str);

    s = ctrl_getset(b, "Window/Behaviour", "title",
                    "Adjust the behaviour of the window title");
    ctrl_editbox(s, "Window title:", 't', 100,
                 HELPCTX(appearance_title),
                 conf_editbox_handler, I(CONF_wintitle), ED_STR);
#ifdef MOD_PERSO
    /* KiTTY: the title understands placeholders (%%h, %%s, ...). Classic KiTTY
     * printed all eight as static lines here, which is a lot of panel for a
     * reference you need once; this opens a modeless list you can copy from and
     * leave open while typing the title. */
    if (!GetPuttyFlag())
        ctrl_pushbutton(s, "Placeholders (%h, %s, ...)", NO_SHORTCUT,
                        HELPCTX(appearance_title),
                        kitty_title_placeholders_handler, I(0));
#endif
    ctrl_checkbox(s, "Separate window and icon titles", 'i',
                  HELPCTX(appearance_title),
                  conf_checkbox_handler,
                  I(CHECKBOX_INVERT | CONF_win_name_always));

    s = ctrl_getset(b, "Window/Behaviour", "main", NULL);
    ctrl_checkbox(s, "Warn before closing window", 'w',
                  HELPCTX(behaviour_closewarn),
                  conf_checkbox_handler, I(CONF_warn_on_close));
#ifdef MOD_PERSO
    /* KiTTY startup window state. All three are honoured in window.c (maximize
     * and fullscreen at the ShowWindow, send-to-tray once the session is up),
     * and all three are saved with the session - but the config-box controls
     * were lost in the 0.84 port, leaving the settings unreachable and existing
     * sessions looking as if they had forgotten them. NO_SHORTCUT: this panel
     * has run out of free accelerator letters. */
    if (!GetPuttyFlag()) {
        /* All three are INT keys, so they need kitty_checkbox_int_handler:
         * conf_checkbox_handler asserts in conf_get_bool on a non-BOOL key. */
        ctrl_checkbox(s, "Send to tray on startup", NO_SHORTCUT,
                      HELPCTX(no_help),
                      kitty_checkbox_int_handler, I(CONF_sendtotray));
        ctrl_checkbox(s, "Maximize on startup", NO_SHORTCUT,
                      HELPCTX(no_help),
                      kitty_checkbox_int_handler, I(CONF_maximize));
        ctrl_checkbox(s, "Full screen on startup", NO_SHORTCUT,
                      HELPCTX(no_help),
                      kitty_checkbox_int_handler, I(CONF_fullscreen));
        /* KiTTY: where a window OPENS is window behaviour, not a property of
         * the connection - this used to sit on the Session panel, among the
         * host and port. Classic KiTTY's equivalent ("Save position and size on
         * exit") lived in this panel too. Ours remembers the position per
         * monitor LAYOUT, so docking or unplugging a screen restores the window
         * where it belonged on that layout instead of stranding it off-screen;
         * it deliberately does not restore a maximised or minimised state. */
        ctrl_checkbox(s, "Remember window position (per monitor layout)", NO_SHORTCUT,
                      HELPCTX(no_help), conf_checkbox_handler,
                      I(CONF_remember_winpos));
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
                        "Window buttons (for kiosk or embedded use)");
        ctrl_checkbox(s, "System menu, and therefore any caption buttons at all",
                      NO_SHORTCUT, HELPCTX(no_help),
                      conf_checkbox_handler, I(CONF_window_has_sysmenu));
        ctrl_checkbox(s, "Allow closing (also disables the X and Alt+F4)",
                      NO_SHORTCUT, HELPCTX(no_help),
                      conf_checkbox_handler, I(CONF_window_closable));
        ctrl_checkbox(s, "Minimize button", NO_SHORTCUT, HELPCTX(no_help),
                      conf_checkbox_handler, I(CONF_window_minimizable));
        ctrl_checkbox(s, "Maximize button", NO_SHORTCUT, HELPCTX(no_help),
                      conf_checkbox_handler, I(CONF_window_maximizable));
    }
#endif

#ifdef MOD_LAUNCHER
    if (!GetPuttyFlag()) {
        s = ctrl_getset(b, "Window/Behaviour", "launcher_hotkey",
                        "KiTTY Launcher global hotkey");
        ctrl_checkbox(s, "Enable global hotkey for this session", NO_SHORTCUT,
                      HELPCTX(no_help), conf_checkbox_handler,
                      I(CONF_launcher_global_hotkey_enabled));
        ctrl_editbox(s, "Hotkey:", NO_SHORTCUT, 40,
                     HELPCTX(no_help), conf_editbox_handler,
                     I(CONF_launcher_global_hotkey), ED_STR);
        ctrl_pushbutton(s, "Check hotkey availability", NO_SHORTCUT,
                        HELPCTX(no_help), kitty_launcher_hotkey_check_handler,
                        I(0));
        ctrl_text(s, "Example: Ctrl+Alt+K or Ctrl+Shift+F12. Registered only while KiTTY Launcher is running.",
                  HELPCTX(no_help));
    }
#endif

#ifdef MOD_PERSO
    /*
     * The Window/Transparency panel (KiTTY).
     */
    if (!GetPuttyFlag() && GetTransparencyFlag()) {
        ctrl_settitle(b, "Window/Transparency",
                      "Options controlling transparency");
        s = ctrl_getset(b, "Window/Transparency", "bg_transparency",
                        "Transparency setting");
        ctrl_editbox(s, "Transparency:", NO_SHORTCUT, 20,
                     HELPCTX(no_help),
                     conf_editbox_handler,
                     I(CONF_transparencynumber), ED_INT);
        ctrl_text(s, "from 0 (visible) to 255 (transparent)",
                  HELPCTX(no_help));
        ctrl_text(s, "-1 to disable completely", HELPCTX(no_help));
    }

    /*
     * The Window/Hyperlinks panel (KiTTY).
     */
    if (!GetPuttyFlag()) {
        ctrl_settitle(b, "Window/Hyperlinks",
                      "Options controlling clickable URL hyperlinks");
        s = ctrl_getset(b, "Window/Hyperlinks", "main",
                        "Hyperlink behaviour");
        ctrl_checkbox(s, "Require Ctrl key to click hyperlinks", NO_SHORTCUT,
                      HELPCTX(no_help), kitty_checkbox_int_handler,
                      I(CONF_url_ctrl_click));
        ctrl_checkbox(s, "Underline hyperlinks", NO_SHORTCUT,
                      HELPCTX(no_help), kitty_checkbox_int_handler,
                      I(CONF_url_underline));
        ctrl_checkbox(s, "Show hand cursor when hovering over hyperlinks", NO_SHORTCUT,
                      HELPCTX(no_help), kitty_checkbox_int_handler,
                      I(CONF_url_hover_cursor));
        ctrl_checkbox(s, "Use the default browser", NO_SHORTCUT,
                      HELPCTX(no_help), kitty_checkbox_int_handler,
                      I(CONF_url_defbrowser));
        ctrl_filesel(s, "Other browser:", NO_SHORTCUT,
                     FILTER_ALL_FILES, false, "Select browser executable",
                     HELPCTX(no_help),
                     conf_filesel_handler, I(CONF_url_browser));
        ctrl_checkbox(s, "Use the default regular expression", NO_SHORTCUT,
                      HELPCTX(no_help), kitty_checkbox_int_handler,
                      I(CONF_url_defregex));
        ctrl_editbox(s, "Custom regular expression:", NO_SHORTCUT, 60,
                     HELPCTX(no_help), conf_editbox_handler,
                     I(CONF_url_regex), ED_STR);
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
        s = ctrl_getset(b, "Window/Appearance", "position",
                        "Where the window opens");
        ctrl_checkbox(s, "Open the window at a fixed position", NO_SHORTCUT,
                      HELPCTX(no_help), conf_checkbox_handler,
                      I(CONF_set_windowpos));
        ctrl_editbox(s, "Top:", NO_SHORTCUT, 20, HELPCTX(no_help),
                     conf_editbox_handler, I(CONF_ypos), ED_INT);
        ctrl_editbox(s, "Left:", NO_SHORTCUT, 20, HELPCTX(no_help),
                     conf_editbox_handler, I(CONF_xpos), ED_INT);
        ctrl_text(s, "A fixed position wins over \"Remember window position\" "
                     "(Window > Behaviour). If it lands off-screen - a screen "
                     "that is no longer there - the window is moved onto the "
                     "nearest monitor.", HELPCTX(no_help));

        s = ctrl_getset(b, "Window/Appearance", "icon",
                        "Define the window icon");
        ctrl_editbox(s, "Icon (from internal resources)", NO_SHORTCUT, 40,
                     HELPCTX(no_help), conf_editbox_handler,
                     I(CONF_icone), ED_INT);
        ctrl_filesel(s, "External icon file:", NO_SHORTCUT,
                     FILTER_ALL_FILES, false, "Select icon file",
                     HELPCTX(no_help),
                     conf_filesel_handler, I(CONF_iconefile));
    }
#endif

#ifdef MOD_BACKGROUNDIMAGE
    /* The Window/Back.&Image panel (KiTTY). Engine: kitty_image.c. */
    if (!GetPuttyFlag() && GetBackgroundImageFlag()) {
        str = dupprintf("Configure the background of %s's window", appname);
        ctrl_settitle(b, "Window/Back.&Image", str);
        sfree(str);

        s = ctrl_getset(b, "Window/Back.&Image", "bg_style",
                        "Background settings");
        ctrl_radiobuttons(s, "Background Style:", NO_SHORTCUT, 3,
                          HELPCTX(no_help),
                          conf_radiobutton_handler, I(CONF_bg_type),
                          "Solid", NO_SHORTCUT, I(0),
                          "Desktop", NO_SHORTCUT, I(1),
                          "Image", NO_SHORTCUT, I(2));

        s = ctrl_getset(b, "Window/Back.&Image", "bg_wp_img_settings",
                        "Desktop and image settings");
        ctrl_editbox(s, "Opacity: (negative with Image for gradient)",
                     NO_SHORTCUT, 20, HELPCTX(no_help), conf_editbox_handler,
                     I(CONF_bg_opacity), ED_INT);
        ctrl_editbox(s, "Slideshow:", NO_SHORTCUT, 20,
                     HELPCTX(no_help), conf_editbox_handler,
                     I(CONF_bg_slideshow), ED_INT);

        s = ctrl_getset(b, "Window/Back.&Image", "bg_img_settings",
                        "Image settings");
        ctrl_filesel(s, "Image file: (or #RRGGBB for gradient)", NO_SHORTCUT,
                     FILTER_ALL_FILES, false, "Select background image file",
                     HELPCTX(no_help),
                     conf_filesel_handler, I(CONF_bg_image_filename));
        ctrl_radiobuttons(s, "Image placement:", NO_SHORTCUT, 3,
                          HELPCTX(no_help),
                          conf_radiobutton_handler, I(CONF_bg_image_style),
                          "Tile", NO_SHORTCUT, I(0),
                          "Center", NO_SHORTCUT, I(1),
                          "Stretch", NO_SHORTCUT, I(2),
                          "Absolute (X,Y)", NO_SHORTCUT, I(3),
                          "Blank back.", NO_SHORTCUT, I(4),
                          "Stretch+", NO_SHORTCUT, I(5));
        ctrl_editbox(s, "Absolute Left (X):", NO_SHORTCUT, 20,
                     HELPCTX(no_help), conf_editbox_handler,
                     I(CONF_bg_image_abs_x), ED_INT);
        ctrl_editbox(s, "Absolute Top (Y):", NO_SHORTCUT, 20,
                     HELPCTX(no_help), conf_editbox_handler,
                     I(CONF_bg_image_abs_y), ED_INT);
        ctrl_radiobuttons(s, "Image placement is relative to:", NO_SHORTCUT, 2,
                          HELPCTX(no_help),
                          conf_radiobutton_handler, I(CONF_bg_image_abs_fixed),
                          "Desktop", NO_SHORTCUT, I(0),
                          "Terminal Window", NO_SHORTCUT, I(1));
    }
#endif
}

/* The Window/Translation, Window/Selection(+Copy) and Window/Colours panels. */
static void scb_panel_selection(struct controlbox *b)
{
    struct charclass_data *ccd;
    struct colour_data *cd;
    struct controlset *s;
    dlgcontrol *c;
    char *str;

    /*
     * The Window/Translation panel.
     */
    ctrl_settitle(b, "Window/Translation",
                  "Options controlling character set translation");

    s = ctrl_getset(b, "Window/Translation", "trans",
                    "Character set translation");
    ctrl_combobox(s, "Remote character set:",
                  'r', 100, HELPCTX(translation_codepage),
                  codepage_handler, P(NULL), P(NULL));

    s = ctrl_getset(b, "Window/Translation", "tweaks", NULL);
    ctrl_checkbox(s, "Treat CJK ambiguous characters as wide", 'w',
                  HELPCTX(translation_cjk_ambig_wide),
                  conf_checkbox_handler, I(CONF_cjk_ambig_wide));

    str = dupprintf("Adjust how %s handles line drawing characters", appname);
    s = ctrl_getset(b, "Window/Translation", "linedraw", str);
    sfree(str);
    ctrl_radiobuttons(
        s, "Handling of line drawing characters:", NO_SHORTCUT,1,
        HELPCTX(translation_linedraw),
        conf_radiobutton_handler, I(CONF_vtmode),
        "Use Unicode line drawing code points",'u',I(VT_UNICODE),
        "Poor man's line drawing (+, - and |)",'p',I(VT_POORMAN));
    ctrl_checkbox(s, "Copy and paste line drawing characters as lqqqk",'d',
                  HELPCTX(selection_linedraw),
                  conf_checkbox_handler, I(CONF_rawcnp));
    ctrl_checkbox(s, "Enable VT100 line drawing even in UTF-8 mode",'8',
                  HELPCTX(translation_utf8linedraw),
                  conf_checkbox_handler, I(CONF_utf8linedraw));

    /*
     * The Window/Selection panel.
     */
    ctrl_settitle(b, "Window/Selection", "Options controlling copy and paste");

    s = ctrl_getset(b, "Window/Selection", "mouse",
                    "Control use of mouse");
    ctrl_checkbox(s, "Shift overrides application's use of mouse", 'p',
                  HELPCTX(selection_shiftdrag),
                  conf_checkbox_handler, I(CONF_mouse_override));
    ctrl_radiobuttons(s,
                      "Default selection mode (Alt+drag does the other one):",
                      NO_SHORTCUT, 2,
                      HELPCTX(selection_rect),
                      conf_radiobutton_bool_handler,
                      I(CONF_rect_select),
                      "Normal", 'n', I(false),
                      "Rectangular block", 'r', I(true));

    s = ctrl_getset(b, "Window/Selection", "clipboards",
                    "Assign copy/paste actions to clipboards");
    ctrl_checkbox(s, "Auto-copy selected text to "
                  CLIPNAME_EXPLICIT_OBJECT,
                  NO_SHORTCUT, HELPCTX(selection_autocopy),
                  conf_checkbox_handler, I(CONF_mouseautocopy));
    clipboard_control(s, "Mouse paste action:", NO_SHORTCUT, 60,
                      HELPCTX(selection_clipactions),
                      CONF_mousepaste, CONF_mousepaste_custom);
    clipboard_control(s, "{Ctrl,Shift} + Ins:", NO_SHORTCUT, 60,
                      HELPCTX(selection_clipactions),
                      CONF_ctrlshiftins, CONF_ctrlshiftins_custom);
    clipboard_control(s, "Ctrl + Shift + {C,V}:", NO_SHORTCUT, 60,
                      HELPCTX(selection_clipactions),
                      CONF_ctrlshiftcv, CONF_ctrlshiftcv_custom);
#ifdef MOD_FAR2L
    /* KiTTY (far2l): let a remote far2l session read/write the local clipboard.
     * Triples (label, NO_SHORTCUT, I(val)) — 0.84 ctrl_radiobuttons needs the
     * per-button shortcut slot. */
    /* Deny/Allow/Ask rather than Disabled/Enabled: these grant a permission,
     * they do not switch a feature on. The SHARED_CLIPBOARD_* value names still
     * read DISABLED/ENABLED - stored values are unchanged either way. */
    ctrl_radiobuttons(s, "far2l shared clipboard:", NO_SHORTCUT, 3,
                      HELPCTX(no_help), conf_radiobutton_handler,
                      I(CONF_shared_clipboard),
                      "Deny", NO_SHORTCUT, I(SHARED_CLIPBOARD_DISABLED),
                      "Allow", NO_SHORTCUT, I(SHARED_CLIPBOARD_ENABLED),
                      "Ask", NO_SHORTCUT, I(SHARED_CLIPBOARD_ASK));
#endif
    /* KiTTY (OSC 52): let the remote host put text on the local clipboard.
     * Same three-way shape as the far2l control above, on purpose. "Ask"
     * answers latch for the rest of the session. Only the write direction
     * exists: OSC 52's clipboard-read request is always refused. */
    ctrl_radiobuttons(s, "Remote clipboard writes (OSC 52):", NO_SHORTCUT, 3,
                      HELPCTX(no_help), conf_radiobutton_handler,
                      I(CONF_osc52_clipboard),
                      "Deny", NO_SHORTCUT, I(OSC52_CLIPBOARD_DENY),
                      "Allow", NO_SHORTCUT, I(OSC52_CLIPBOARD_ALLOW),
                      "Ask", NO_SHORTCUT, I(OSC52_CLIPBOARD_ASK));
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
    ctrl_radiobuttons(s, "Remote clipboard reads (OSC 52):", NO_SHORTCUT, 2,
                      HELPCTX(no_help), conf_radiobutton_handler,
                      I(CONF_osc52_clipboard_read),
                      "Deny", NO_SHORTCUT, I(OSC52_READ_DENY),
                      "Ask", NO_SHORTCUT, I(OSC52_READ_ASK));
    /* Covers OSC 52, OSC 5522 AND far2l, which is why the label names none of
     * them: a rule with an exception in it is not the rule people remember. */
    ctrl_checkbox(s, "Only allow remote clipboard access while this window has focus",
                  NO_SHORTCUT, HELPCTX(no_help),
                  conf_checkbox_handler, I(CONF_clipboard_require_focus));
    /* One ceiling for OSC 52 and far2l both. Clamped in code (CLIP_MAX_MB_CAP):
     * lowering it only ever helps, but it must not be possible to type a number
     * here that turns a bounded denial of service into an unbounded one. */
    ctrl_editbox(s, "Largest remote clipboard payload, in MB:", NO_SHORTCUT, 25,
                 HELPCTX(no_help), conf_editbox_handler,
                 I(CONF_clipboard_max_mb), ED_INT);
    /* A cap per second rather than a gap between writes: a gap would make the
     * FIRST write of a burst win, leaving a stale clipboard, which is backwards. */
    ctrl_editbox(s, "Most clipboard changes a server may make per second (0 = no limit):",
                 NO_SHORTCUT, 25, HELPCTX(no_help), conf_editbox_handler,
                 I(CONF_clipboard_writes_per_sec), ED_INT);

    /* The numbers behind the read dialog. They are settings because the values
     * shipped are guesses - no other terminal implements a hand-over rate limit
     * to copy from - and because someone who wants a five-minute grant instead of
     * ten should not have to argue with us about it. */
    s = ctrl_getset(b, "Window/Selection", "osc52read",
                    "Limits on a granted clipboard read (OSC 52)");
    ctrl_editbox(s, "Grant length offered, in minutes:", NO_SHORTCUT, 25,
                 HELPCTX(no_help), conf_editbox_handler,
                 I(CONF_osc52_read_minutes), ED_INT);
    ctrl_editbox(s, "Grant length offered, in requests:", NO_SHORTCUT, 25,
                 HELPCTX(no_help), conf_editbox_handler,
                 I(CONF_osc52_read_requests), ED_INT);
    ctrl_editbox(s, "Shortest gap between hand-overs, in seconds (0 = none):",
                 NO_SHORTCUT, 25, HELPCTX(no_help), conf_editbox_handler,
                 I(CONF_osc52_read_interval), ED_INT);
    ctrl_editbox(s, "Most hand-overs in one window (0 = no limit):",
                 NO_SHORTCUT, 25, HELPCTX(no_help), conf_editbox_handler,
                 I(CONF_osc52_read_max), ED_INT);
    ctrl_editbox(s, "Seconds before an unanswered prompt gives up (0 = never):",
                 NO_SHORTCUT, 25, HELPCTX(no_help), conf_editbox_handler,
                 I(CONF_osc52_read_timeout), ED_INT);
    ctrl_editbox(s, "Most prompts in any ten seconds:", NO_SHORTCUT, 25,
                 HELPCTX(no_help), conf_editbox_handler,
                 I(CONF_osc52_read_dialogs), ED_INT);

    s = ctrl_getset(b, "Window/Selection", "osc52show",
                    "Showing a live clipboard permission");
    /* The two markers answer different questions: permission says what COULD
     * happen, activity says what DID. Icons rather than words - the title bar is
     * shared with the connection name - with the standing one bracketed at the end
     * and the transient one bare at the front. */
    ctrl_checkbox(s, "Mark the title while a permission is live  (end, in brackets)",
                  NO_SHORTCUT, HELPCTX(no_help),
                  conf_checkbox_handler, I(CONF_osc52_title_mark));
    ctrl_checkbox(s, "Also mark a standing \"Allow\", not just a granted permission",
                  NO_SHORTCUT, HELPCTX(no_help),
                  conf_checkbox_handler, I(CONF_clipboard_mark_always));
    ctrl_checkbox(s, "Mark the title when the server actually uses the clipboard  (front)",
                  NO_SHORTCUT, HELPCTX(no_help),
                  conf_checkbox_handler, I(CONF_clipboard_activity_mark));
    ctrl_editbox(s, "How long that activity marker stays up, in seconds:",
                 NO_SHORTCUT, 25, HELPCTX(no_help), conf_editbox_handler,
                 I(CONF_clipboard_activity_secs), ED_INT);
    /* Windows 11 build 22000+ only; silently does nothing on Windows 10, which
     * is why the title marker above has to carry the meaning by itself. */
    ctrl_checkbox(s, "Tint the title bar and border too (Windows 11 only)",
                  NO_SHORTCUT, HELPCTX(no_help),
                  conf_checkbox_handler, I(CONF_osc52_colour_frame));
    /* One switch for every remote-clipboard balloon - permission granted or
     * expired, request refused, payload too large - across all three protocols.
     * The balloons are rate-limited and say so, and clicking one opens the Event
     * Log, which is where the events actually all are. */
    ctrl_checkbox(s, "Show tray notifications for remote clipboard events",
                  NO_SHORTCUT, HELPCTX(no_help),
                  conf_checkbox_handler, I(CONF_clipboard_notify));

    s = ctrl_getset(b, "Window/Selection", "paste",
                    "Control pasting of text from clipboard to terminal");
    ctrl_checkbox(s, "Permit control characters in pasted text",
                  NO_SHORTCUT, HELPCTX(selection_pastectrl),
                  conf_checkbox_handler, I(CONF_paste_controls));

    s = ctrl_getset(b, "Window/Selection", "runclipcmd",
                    "Running the clipboard as a local command (Ctrl+F5)");
    ctrl_checkbox(s, "Confirm before running the clipboard as a command",
                  NO_SHORTCUT, HELPCTX(no_help),
                  conf_checkbox_handler, I(CONF_runcmdconfirm));
    ctrl_checkbox(s, "Show a tray notification after running a clipboard command",
                  NO_SHORTCUT, HELPCTX(no_help),
                  conf_checkbox_handler, I(CONF_runcmdnotify));

    /*
     * The Window/Selection/Copy panel.
     */
    ctrl_settitle(b, "Window/Selection/Copy",
                  "Options controlling copying from terminal to clipboard");

    s = ctrl_getset(b, "Window/Selection/Copy", "charclass",
                    "Classes of character that group together");
    ccd = (struct charclass_data *)
        ctrl_alloc(b, sizeof(struct charclass_data));
    ccd->listbox = ctrl_listbox(s, "Character classes:", 'e',
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
    ccd->editbox = ctrl_editbox(s, "Set to class", 't', 50,
                                HELPCTX(copy_charclasses),
                                charclass_handler, P(ccd), P(NULL));
    ccd->editbox->column = 0;
    ccd->button = ctrl_pushbutton(s, "Set", 's',
                                  HELPCTX(copy_charclasses),
                                  charclass_handler, P(ccd));
    ccd->button->column = 1;
    ctrl_columns(s, 1, 100);

    /*
     * The Window/Colours panel.
     */
    ctrl_settitle(b, "Window/Colours", "Options controlling use of colours");

    s = ctrl_getset(b, "Window/Colours", "general",
                    "General options for colour usage");
    ctrl_checkbox(s, "Allow terminal to specify ANSI colours", 'i',
                  HELPCTX(colours_ansi),
                  conf_checkbox_handler, I(CONF_ansi_colour));
    ctrl_checkbox(s, "Allow terminal to use xterm 256-colour mode", '2',
                  HELPCTX(colours_xterm256), conf_checkbox_handler,
                  I(CONF_xterm_256_colour));
    ctrl_checkbox(s, "Allow terminal to use 24-bit colours", '4',
                  HELPCTX(colours_truecolour), conf_checkbox_handler,
                  I(CONF_true_colour));
    ctrl_radiobuttons(s, "Indicate bolded text by changing:", 'b', 3,
                      HELPCTX(colours_bold),
                      conf_radiobutton_handler, I(CONF_bold_style),
                      "The font", I(BOLD_STYLE_FONT),
                      "The colour", I(BOLD_STYLE_COLOUR),
                      "Both", I(BOLD_STYLE_FONT | BOLD_STYLE_COLOUR));
#ifdef MOD_TUTTYCOLOR
    /* KiTTY (TuTTY): extra colour toggles. The palette gains "Underlined Text",
     * "Selected Text" and "Selected Background" slots (editable in the list
     * below). under_colour colours underlined text; sel_colour rendering is
     * deferred (slots exist + are editable). */
    if (!GetPuttyFlag()) {
        ctrl_checkbox(s, "Colour underlined text", NO_SHORTCUT,
                      HELPCTX(no_help), kitty_checkbox_int_handler,
                      I(CONF_under_colour));
        ctrl_checkbox(s, "Colour selected text", NO_SHORTCUT,
                      HELPCTX(no_help), kitty_checkbox_int_handler,
                      I(CONF_sel_colour));
    }
#endif

    str = dupprintf("Adjust the precise colours %s displays", appname);
    s = ctrl_getset(b, "Window/Colours", "adjust", str);
    sfree(str);
    ctrl_text(s, "Select a colour from the list, and then click the"
              " Modify button to change its appearance.",
              HELPCTX(colours_config));
    ctrl_columns(s, 2, 67, 33);
    cd = (struct colour_data *)ctrl_alloc(b, sizeof(struct colour_data));
    cd->listbox = ctrl_listbox(s, "Select a colour to adjust:", 'u',
                               HELPCTX(colours_config), colour_handler, P(cd));
    cd->listbox->column = 0;
    cd->listbox->listbox.height = 7;
    c = ctrl_text(s, "RGB value:", HELPCTX(colours_config));
    c->column = 1;
    cd->redit = ctrl_editbox(s, "Red", 'r', 50, HELPCTX(colours_config),
                             colour_handler, P(cd), P(NULL));
    cd->redit->column = 1;
    cd->gedit = ctrl_editbox(s, "Green", 'n', 50, HELPCTX(colours_config),
                             colour_handler, P(cd), P(NULL));
    cd->gedit->column = 1;
    cd->bedit = ctrl_editbox(s, "Blue", 'e', 50, HELPCTX(colours_config),
                             colour_handler, P(cd), P(NULL));
    cd->bedit->column = 1;
    cd->button = ctrl_pushbutton(s, "Modify", 'm', HELPCTX(colours_config),
                                 colour_handler, P(cd));
    cd->button->column = 1;
    ctrl_columns(s, 1, 100);
}

/* The Connection panel and Connection/Data sub-panel (network utilities
 * only: the whole body is guarded by protocol >= 0). */
static void scb_panel_connection(struct controlbox *b, bool midsession, int protocol)
{
    struct environ_data *ed;
    struct controlset *s;

    /*
     * The Connection panel. This doesn't show up if we're in a
     * non-network utility such as pterm. We tell this by being
     * passed a protocol < 0.
     */
    if (protocol >= 0) {
        ctrl_settitle(b, "Connection", "Options controlling the connection");

        s = ctrl_getset(b, "Connection", "keepalive",
                        "Sending of null packets to keep session active");
        ctrl_editbox(s, "Seconds between keepalives (0 to turn off)", 'k', 20,
                     HELPCTX(connection_keepalive),
                     conf_editbox_handler, I(CONF_ping_interval), ED_INT);
#ifdef MOD_PERSO
        if (!GetPuttyFlag()) {
            ctrl_editbox(s, "Anti-idle string", NO_SHORTCUT, 50,
                         HELPCTX(no_help), conf_editbox_handler,
                         I(CONF_antiidle), ED_STR);
        }
#endif

        if (!midsession) {
            s = ctrl_getset(b, "Connection", "tcp",
                            "Low-level TCP connection options");
            ctrl_checkbox(s, "Disable Nagle's algorithm (TCP_NODELAY option)",
                          'n', HELPCTX(connection_nodelay),
                          conf_checkbox_handler,
                          I(CONF_tcp_nodelay));
            ctrl_checkbox(s, "Enable TCP keepalives (SO_KEEPALIVE option)",
                          'p', HELPCTX(connection_tcpkeepalive),
                          conf_checkbox_handler,
                          I(CONF_tcp_keepalives));
#ifndef NO_IPV6
            s = ctrl_getset(b, "Connection", "ipversion",
                            "Internet protocol version");
            ctrl_radiobuttons(s, NULL, NO_SHORTCUT, 3,
                              HELPCTX(connection_ipversion),
                              conf_radiobutton_handler,
                              I(CONF_addressfamily),
                              "Auto", 'u', I(ADDRTYPE_UNSPEC),
                              "IPv4", '4', I(ADDRTYPE_IPV4),
                              "IPv6", '6', I(ADDRTYPE_IPV6));
#endif

#ifdef MOD_RECONNECT
            /* KiTTY auto-reconnect (INT keys -> kitty_checkbox_int_handler;
             * engine wired separately). Gated by GetAutoreconnectFlag(). */
            if (!GetPuttyFlag() && GetAutoreconnectFlag()) {
                s = ctrl_getset(b, "Connection", "reconnect",
                                "Reconnect options");
                ctrl_checkbox(s, "Attempt to reconnect on system wakeup",
                              NO_SHORTCUT, HELPCTX(no_help),
                              kitty_checkbox_int_handler,
                              I(CONF_wakeup_reconnect));
                ctrl_checkbox(s, "Attempt to reconnect on connection failure",
                              NO_SHORTCUT, HELPCTX(no_help),
                              kitty_checkbox_int_handler,
                              I(CONF_failure_reconnect));
            }
#endif

            {
                const char *label = backend_vt_from_proto(PROT_SSH) ?
                    "Logical name of remote host (e.g. for SSH key lookup):" :
                    "Logical name of remote host:";
                s = ctrl_getset(b, "Connection", "identity",
                                "Logical name of remote host");
                ctrl_editbox(s, label, 'm', 100,
                             HELPCTX(connection_loghost),
                             conf_editbox_handler, I(CONF_loghost), ED_STR);
            }

            s = ctrl_getset(b, "Connection", "hooks",
                            "Command to run at connection event");
            ctrl_editbox(s, "Command to run before connection", 'b', 100,
                         HELPCTX(connection_pre_hook),
                         conf_editbox_handler, I(CONF_pre_connect_command), ED_STR);

#ifdef MOD_PORTKNOCKING
            /* KiTTY: port-knocking sequence. Backend = kitty_port_knock() /
             * ManagePortKnocking(), fired from start_backend() before connect. */
            if (!GetPuttyFlag()) {
                s = ctrl_getset(b, "Connection", "PortKnocking",
                                "Port knocking sequence");
                ctrl_editbox(s, "Sequence:", NO_SHORTCUT, 100,
                             HELPCTX(no_help), conf_editbox_handler,
                             I(CONF_portknockingoptions), ED_STR);
                ctrl_text(s, "A comma-separated list of port:protocol knocks. "
                          "Protocols are tcp and udp; use s for a pause between "
                          "knocks.", HELPCTX(no_help));
                ctrl_text(s, "Example:  2001:tcp, 1:s, 2002:udp",
                          HELPCTX(no_help));
            }
#endif
        }

        /*
         * A sub-panel Connection/Data, containing options that
         * decide on data to send to the server.
         */
        if (!midsession) {
            ctrl_settitle(b, "Connection/Data", "Data to send to the server");

            s = ctrl_getset(b, "Connection/Data", "login",
                            "Login details");
            ctrl_editbox(s, "Auto-login username", 'u', 50,
                         HELPCTX(connection_username),
                         conf_editbox_handler, I(CONF_username), ED_STR);
#ifdef MOD_PERSO
            if (!GetPuttyFlag())
                ctrl_editbox(s, "Alternate host name (HostAlt)", NO_SHORTCUT,
                             50, HELPCTX(no_help),
                             conf_editbox_handler, I(CONF_host_alt), ED_STR);
#endif
            {
                /* We assume the local username is sufficiently stable
                 * to include on the dialog box. */
                char *user = get_username();
                char *userlabel = dupprintf("Use system username (%s)",
                                            user ? user : "");
                sfree(user);
                ctrl_radiobuttons(s, "When username is not specified:", 'n', 4,
                                  HELPCTX(connection_username_from_env),
                                  conf_radiobutton_bool_handler,
                                  I(CONF_username_from_env),
                                  "Prompt", I(false),
                                  userlabel, I(true));
                sfree(userlabel);
            }

#ifdef MOD_PERSO
            /* KiTTY auto-command: sent automatically after login. */
            if (!GetPuttyFlag()) {
                dlgcontrol *cpw;
                cpw = ctrl_editbox(s, "Auto-login password", NO_SHORTCUT, 50,
                                   HELPCTX(no_help), kitty_autopw_handler,
                                   I(CONF_password), ED_STR);
                cpw->editbox.password = true;
                g_autopw_ctrl = cpw;
                ctrl_checkbox(s, "Show password", NO_SHORTCUT,
                              HELPCTX(no_help), kitty_showpw_handler, P(NULL));
                ctrl_editbox(s, "Auto-command after login", NO_SHORTCUT,
                             50, HELPCTX(no_help),
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
                    ctrl_editbox_multiline(s, "Login script (wait-for and"
                                           " send-text, one per line):",
                                           NO_SHORTCUT, 8, false,
                                           HELPCTX(no_help),
                                           kitty_loginscript_handler,
                                           I(CONF_scriptfilecontent), ED_STR);
                /* Replaces classic's picker, which read a file into the session
                 * and cleared itself. This only fills the box - what gets saved
                 * is what you can see and edit above. */
                ctrl_pushbutton(s, "Load script from file...", NO_SHORTCUT,
                                HELPCTX(no_help),
                                kitty_loginscript_load_handler, I(0));
            }
#endif

            s = ctrl_getset(b, "Connection/Data", "term",
                            "Terminal details");
            ctrl_editbox(s, "Terminal-type string", 't', 50,
                         HELPCTX(connection_termtype),
                         conf_editbox_handler, I(CONF_termtype), ED_STR);
            ctrl_editbox(s, "Terminal speeds", 's', 50,
                         HELPCTX(connection_termspeed),
                         conf_editbox_handler, I(CONF_termspeed), ED_STR);

            s = ctrl_getset(b, "Connection/Data", "env",
                            "Environment variables");
            ctrl_columns(s, 2, 80, 20);
            ed = (struct environ_data *)
                ctrl_alloc(b, sizeof(struct environ_data));
            ed->varbox = ctrl_editbox(s, "Variable", 'v', 60,
                                      HELPCTX(telnet_environ),
                                      environ_handler, P(ed), P(NULL));
            ed->varbox->column = 0;
            ed->valbox = ctrl_editbox(s, "Value", 'l', 60,
                                      HELPCTX(telnet_environ),
                                      environ_handler, P(ed), P(NULL));
            ed->valbox->column = 0;
            ed->addbutton = ctrl_pushbutton(s, "Add", 'd',
                                            HELPCTX(telnet_environ),
                                            environ_handler, P(ed));
            ed->addbutton->column = 1;
            ed->rembutton = ctrl_pushbutton(s, "Remove", 'r',
                                            HELPCTX(telnet_environ),
                                            environ_handler, P(ed));
            ed->rembutton->column = 1;
            ctrl_columns(s, 1, 100);
            ed->listbox = ctrl_listbox(s, NULL, NO_SHORTCUT,
                                       HELPCTX(telnet_environ),
                                       environ_handler, P(ed));
            ed->listbox->listbox.height = 3;
            ed->listbox->listbox.ncols = 2;
            ed->listbox->listbox.percentages = snewn(2, int);
            ed->listbox->listbox.percentages[0] = 30;
            ed->listbox->listbox.percentages[1] = 70;
        }

    }
}

/* The Connection/Proxy panel (not available mid-session). */
static void scb_panel_proxy(struct controlbox *b, bool midsession)
{
    struct controlset *s;
    dlgcontrol *c;

    if (!midsession) {
        /*
         * The Connection/Proxy panel.
         */
        ctrl_settitle(b, "Connection/Proxy",
                      "Options controlling proxy usage");

        s = ctrl_getset(b, "Connection/Proxy", "basics", NULL);
        c = ctrl_droplist(s, "Proxy type:", 't', 70,
                          HELPCTX(proxy_type), proxy_type_handler, I(0));
        ctrl_columns(s, 2, 80, 20);
        c = ctrl_editbox(s, "Proxy hostname", 'y', 100,
                         HELPCTX(proxy_main),
                         conf_editbox_handler,
                         I(CONF_proxy_host), ED_STR);
        c->column = 0;
        c = ctrl_editbox(s, "Port", 'p', 100,
                         HELPCTX(proxy_main),
                         conf_editbox_handler,
                         I(CONF_proxy_port),
                         ED_INT);
        c->column = 1;
        ctrl_columns(s, 1, 100);
        ctrl_editbox(s, "Exclude Hosts/IPs", 'e', 100,
                     HELPCTX(proxy_exclude),
                     conf_editbox_handler,
                     I(CONF_proxy_exclude_list), ED_STR);
        ctrl_checkbox(s, "Consider proxying local host connections", 'x',
                      HELPCTX(proxy_exclude),
                      conf_checkbox_handler,
                      I(CONF_even_proxy_localhost));
        ctrl_radiobuttons(s, "Do DNS name lookup at proxy end:", 'd', 3,
                          HELPCTX(proxy_dns),
                          conf_radiobutton_handler,
                          I(CONF_proxy_dns),
                          "No", I(FORCE_OFF),
                          "Auto", I(AUTO),
                          "Yes", I(FORCE_ON));
        ctrl_editbox(s, "Username", 'u', 60,
                     HELPCTX(proxy_auth),
                     conf_editbox_handler,
                     I(CONF_proxy_username), ED_STR);
        c = ctrl_editbox(s, "Password", 'w', 60,
                         HELPCTX(proxy_auth),
                         conf_editbox_handler,
                         I(CONF_proxy_password), ED_STR);
        c->editbox.password = true;
        ctrl_editbox(s, "Command to send to proxy (for some types)", 'm', 100,
                     HELPCTX(proxy_command),
                     conf_editbox_handler,
                     I(CONF_proxy_telnet_command), ED_STR);

        ctrl_radiobuttons(s, "Print proxy diagnostics "
                          "in the terminal window", 'r', 5,
                          HELPCTX(proxy_logging),
                          conf_radiobutton_handler,
                          I(CONF_proxy_log_to_term),
                          "No", I(FORCE_OFF),
                          "Yes", I(FORCE_ON),
                          "Only until session starts", I(AUTO));
#ifdef MOD_PERSO
        /* KiTTY: reusable named proxy definitions (hknet/KiTTY#11), managed
         * independently of this session's own proxy above. The chosen one is
         * applied via the Session panel's "Proxy choice". Shown unless
         * proxyselection=no, so it is the entry point to create the first
         * proxy even before the Session-panel droplist appears. */
        if (!GetPuttyFlag() && kitty_proxy_editor_available()) {
            ctrl_text(s, "Named proxies are reusable proxy definitions; pick one "
                      "for a session with \"Proxy choice\" in the Session panel.",
                      HELPCTX(no_help));
            ctrl_pushbutton(s, "Edit named proxies...", NO_SHORTCUT,
                            HELPCTX(no_help), kitty_proxyedit_handler, P(NULL));
        }
#endif
    }
}

/* The Connection/SSH panel tree: SSH core, Kex, Host keys, Cipher, Auth
 * (+Credentials/GSSAPI), TTY, X11, Tunnels, Bugs, More bugs, and the
 * KiTTY PSCP/WinSCP panel. Kept as one helper so the shared protocol/
 * midsession guard structure stays verbatim. */
static void scb_panel_ssh(struct controlbox *b, bool midsession, int protocol, int protcfginfo)
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
                      "Options controlling SSH connections");

        /* SSH-1 or connection-sharing downstream */
        if (midsession && (protcfginfo == 1 || protcfginfo == -1)) {
            s = ctrl_getset(b, "Connection/SSH", "disclaimer", NULL);
            ctrl_text(s, "Nothing on this panel may be reconfigured in mid-"
                      "session; it is only here so that sub-panels of it can "
                      "exist without looking strange.", HELPCTX(no_help));
        }

        if (!midsession) {

            s = ctrl_getset(b, "Connection/SSH", "data",
                            "Data to send to the server");
            ctrl_editbox(s, "Remote command:", 'r', 100,
                         HELPCTX(ssh_command),
                         conf_editbox_handler, I(CONF_remote_cmd), ED_STR);

            s = ctrl_getset(b, "Connection/SSH", "protocol", "Protocol options");
            ctrl_checkbox(s, "Don't start a shell or command at all", 'n',
                          HELPCTX(ssh_noshell),
                          conf_checkbox_handler,
                          I(CONF_ssh_no_shell));
        }

        if (!midsession || !(protcfginfo == 1 || protcfginfo == -1)) {
            s = ctrl_getset(b, "Connection/SSH", "protocol", "Protocol options");

            ctrl_checkbox(s, "Enable compression", 'e',
                          HELPCTX(ssh_compress),
                          conf_checkbox_handler,
                          I(CONF_compression));
        }

        if (!midsession) {
            s = ctrl_getset(b, "Connection/SSH", "sharing", "Sharing an SSH connection between PuTTY tools");

            ctrl_checkbox(s, "Share SSH connections if possible", 's',
                          HELPCTX(ssh_share),
                          conf_checkbox_handler,
                          I(CONF_ssh_connection_sharing));

            ctrl_text(s, "Permitted roles in a shared connection:",
                      HELPCTX(ssh_share));
            ctrl_checkbox(s, "Upstream (connecting to the real server)", 'u',
                          HELPCTX(ssh_share),
                          conf_checkbox_handler,
                          I(CONF_ssh_connection_sharing_upstream));
            ctrl_checkbox(s, "Downstream (connecting to the upstream PuTTY)", 'd',
                          HELPCTX(ssh_share),
                          conf_checkbox_handler,
                          I(CONF_ssh_connection_sharing_downstream));
        }

        if (!midsession) {
            s = ctrl_getset(b, "Connection/SSH", "protocol", "Protocol options");

            ctrl_radiobuttons(s, "SSH protocol version:", NO_SHORTCUT, 2,
                              HELPCTX(ssh_protocol),
                              conf_radiobutton_handler,
                              I(CONF_sshprot),
                              "2", '2', I(3),
                              "1 (INSECURE)", '1', I(0));
        }

        /*
         * The Connection/SSH/Kex panel. (Owing to repeat key
         * exchange, much of this is meaningful in mid-session _if_
         * we're using SSH-2 and are not a connection-sharing
         * downstream, or haven't decided yet.)
         */
        if (protcfginfo != 1 && protcfginfo != -1) {
            ctrl_settitle(b, "Connection/SSH/Kex",
                          "Options controlling SSH key exchange");

            s = ctrl_getset(b, "Connection/SSH/Kex", "main",
                            "Key exchange algorithm options");
            c = ctrl_draglist(s, "Algorithm selection policy:", 's',
                              HELPCTX(ssh_kexlist),
                              kexlist_handler, P(NULL));
            c->listbox.height = KEX_MAX;   /* tall enough to show every algorithm */
            ctrl_checkbox(s, "Warn if Key Exchange is not post-quantum secure", 'q', HELPCTX(ssh_kexlist),
                          conf_checkbox_handler,
                          I(CONF_ssh_warn_pre_quantum));
#ifndef NO_GSSAPI
            ctrl_checkbox(s, "Attempt GSSAPI key exchange",
                          'k', HELPCTX(ssh_gssapi),
                          conf_checkbox_handler,
                          I(CONF_try_gssapi_kex));
#endif

            s = ctrl_getset(b, "Connection/SSH/Kex", "repeat",
                            "Options controlling key re-exchange");

            ctrl_editbox(s, "Max minutes before rekey (0 for no limit)", 't', 20,
                         HELPCTX(ssh_kex_repeat),
                         conf_editbox_handler,
                         I(CONF_ssh_rekey_time),
                         ED_INT);
#ifndef NO_GSSAPI
            ctrl_editbox(s, "Minutes between GSS checks (0 for never)", NO_SHORTCUT, 20,
                         HELPCTX(ssh_kex_repeat),
                         conf_editbox_handler,
                         I(CONF_gssapirekey),
                         ED_INT);
#endif
            ctrl_editbox(s, "Max data before rekey (0 for no limit)", 'x', 20,
                         HELPCTX(ssh_kex_repeat),
                         conf_editbox_handler,
                         I(CONF_ssh_rekey_data),
                         ED_STR);
            ctrl_text(s, "(Use 1M for 1 megabyte, 1G for 1 gigabyte etc)",
                      HELPCTX(ssh_kex_repeat));
        }

        /*
         * The 'Connection/SSH/Host keys' panel.
         */
        if (protcfginfo != 1 && protcfginfo != -1) {
            ctrl_settitle(b, "Connection/SSH/Host keys",
                          "Options controlling SSH host keys");

            s = ctrl_getset(b, "Connection/SSH/Host keys", "main",
                            "Host key algorithm preference");
            c = ctrl_draglist(s, "Algorithm selection policy:", 's',
                              HELPCTX(ssh_hklist),
                              hklist_handler, P(NULL));
            c->listbox.height = HK_MAX;    /* tall enough to show every algorithm */

            ctrl_checkbox(s, "Prefer algorithms for which a host key is known",
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
                            "Manually configure host keys for this connection");

            ctrl_columns(s, 2, 75, 25);
            c = ctrl_text(s, "Host keys or fingerprints to accept:",
                          HELPCTX(ssh_kex_manual_hostkeys));
            c->column = 0;
            /* You want to select from the list, _then_ hit Remove. So
             * tab order should be that way round. */
            mh = (struct manual_hostkey_data *)
                ctrl_alloc(b,sizeof(struct manual_hostkey_data));
            mh->rembutton = ctrl_pushbutton(s, "Remove", 'r',
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
            ctrl_tabdelay(s, mh->rembutton);
            mh->keybox = ctrl_editbox(s, "Key", 'k', 80,
                                      HELPCTX(ssh_kex_manual_hostkeys),
                                      manual_hostkey_handler, P(mh), P(NULL));
            mh->keybox->column = 0;
            mh->addbutton = ctrl_pushbutton(s, "Add key", 'y',
                                            HELPCTX(ssh_kex_manual_hostkeys),
                                            manual_hostkey_handler, P(mh));
            mh->addbutton->column = 1;
            ctrl_columns(s, 1, 100);
        }

        /*
         * But there's no reason not to forbid access to the host CA
         * configuration box, which is common across sessions in any
         * case.
         */
        s = ctrl_getset(b, "Connection/SSH/Host keys", "ca",
                        "Configure trusted certification authorities");
        c = ctrl_pushbutton(s, "Configure host CAs", NO_SHORTCUT,
                            HELPCTX(ssh_kex_cert),
                            host_ca_button_handler, I(0));

        if (!midsession || !(protcfginfo == 1 || protcfginfo == -1)) {
            /*
             * The Connection/SSH/Cipher panel.
             */
            ctrl_settitle(b, "Connection/SSH/Cipher",
                          "Options controlling SSH encryption");

            s = ctrl_getset(b, "Connection/SSH/Cipher",
                            "encryption", "Encryption options");
            c = ctrl_draglist(s, "Encryption cipher selection policy:", 's',
                              HELPCTX(ssh_ciphers),
                              cipherlist_handler, P(NULL));
            c->listbox.height = CIPHER_MAX;  /* tall enough to show every cipher */

            ctrl_checkbox(s, "Enable legacy use of single-DES in SSH-2", 'i',
                          HELPCTX(ssh_ciphers),
                          conf_checkbox_handler,
                          I(CONF_ssh2_des_cbc));
        }

        if (!midsession) {

            /*
             * The Connection/SSH/Auth panel.
             */
            ctrl_settitle(b, "Connection/SSH/Auth",
                          "Options controlling SSH authentication");

            s = ctrl_getset(b, "Connection/SSH/Auth", "main", NULL);
            ctrl_checkbox(s, "Display pre-authentication banner (SSH-2 only)",
                          'd', HELPCTX(ssh_auth_banner),
                          conf_checkbox_handler,
                          I(CONF_ssh_show_banner));
            ctrl_checkbox(s, "Bypass authentication entirely (SSH-2 only)", 'b',
                          HELPCTX(ssh_auth_bypass),
                          conf_checkbox_handler,
                          I(CONF_ssh_no_userauth));
            ctrl_checkbox(s, "Disconnect if authentication succeeds trivially",
                          'n', HELPCTX(ssh_no_trivial_userauth),
                          conf_checkbox_handler,
                          I(CONF_ssh_no_trivial_userauth));

            s = ctrl_getset(b, "Connection/SSH/Auth", "methods",
                            "Authentication methods");
            ctrl_checkbox(s, "Attempt authentication using kageant (Pageant)", 'p',
                          HELPCTX(ssh_auth_pageant),
                          conf_checkbox_handler,
                          I(CONF_tryagent));
            ctrl_checkbox(s, "Attempt TIS or CryptoCard auth (SSH-1)", 'm',
                          HELPCTX(ssh_auth_tis),
                          conf_checkbox_handler,
                          I(CONF_try_tis_auth));
            ctrl_checkbox(s, "Attempt \"keyboard-interactive\" auth (SSH-2)",
                          'i', HELPCTX(ssh_auth_ki),
                          conf_checkbox_handler,
                          I(CONF_try_ki_auth));

            s = ctrl_getset(b, "Connection/SSH/Auth", "aux",
                            "Other authentication-related options");
            ctrl_checkbox(s, "Allow agent forwarding", 'f',
                          HELPCTX(ssh_auth_agentfwd),
                          conf_checkbox_handler, I(CONF_agentfwd));
            ctrl_checkbox(s, "Allow attempted changes of username in SSH-2", NO_SHORTCUT,
                          HELPCTX(ssh_auth_changeuser),
                          conf_checkbox_handler,
                          I(CONF_change_username));

            ctrl_settitle(b, "Connection/SSH/Auth/Credentials",
                          "Credentials to authenticate with");

            s = ctrl_getset(b, "Connection/SSH/Auth/Credentials", "publickey",
                            "Public-key authentication");
            ctrl_filesel(s, "Private key file for authentication:", 'k',
                         FILTER_KEY_FILES, false, "Select private key file",
                         HELPCTX(ssh_auth_privkey),
                         conf_filesel_handler, I(CONF_keyfile));
            ctrl_filesel(s, "Certificate to use with the private key "
                         "(optional):", 'e',
                         FILTER_ALL_FILES, false, "Select certificate file",
                         HELPCTX(ssh_auth_cert),
                         conf_filesel_handler, I(CONF_detached_cert));

            s = ctrl_getset(b, "Connection/SSH/Auth/Credentials", "plugin",
                            "Plugin to provide authentication responses");
            ctrl_editbox(s, "Plugin command to run", NO_SHORTCUT, 100,
                         HELPCTX(ssh_auth_plugin),
                         conf_editbox_handler, I(CONF_auth_plugin), ED_STR);
#ifndef NO_GSSAPI
            /*
             * Connection/SSH/Auth/GSSAPI, which sadly won't fit on
             * the main Auth panel.
             */
            ctrl_settitle(b, "Connection/SSH/Auth/GSSAPI",
                          "Options controlling GSSAPI authentication");
            s = ctrl_getset(b, "Connection/SSH/Auth/GSSAPI", "gssapi", NULL);

            ctrl_checkbox(s, "Attempt GSSAPI authentication (SSH-2 only)",
                          't', HELPCTX(ssh_gssapi),
                          conf_checkbox_handler,
                          I(CONF_try_gssapi_auth));

            ctrl_checkbox(s, "Attempt GSSAPI key exchange (SSH-2 only)",
                          'k', HELPCTX(ssh_gssapi),
                          conf_checkbox_handler,
                          I(CONF_try_gssapi_kex));

            ctrl_checkbox(s, "Allow GSSAPI credential delegation", 'l',
                          HELPCTX(ssh_gssapi_delegation),
                          conf_checkbox_handler,
                          I(CONF_gssapifwd));

            /*
             * GSSAPI library selection.
             */
            if (ngsslibs > 1) {
                c = ctrl_draglist(s, "Preference order for GSSAPI libraries:",
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

                ctrl_filesel(s, "User-supplied GSSAPI library path:", 's',
                             FILTER_DYNLIB_FILES, false, "Select library file",
                             HELPCTX(ssh_gssapi_libraries),
                             conf_filesel_handler,
                             I(CONF_ssh_gss_custom));
            }
#endif
        }

        if (!midsession) {
            /*
             * The Connection/SSH/TTY panel.
             */
            ctrl_settitle(b, "Connection/SSH/TTY", "Remote terminal settings");

            s = ctrl_getset(b, "Connection/SSH/TTY", "sshtty", NULL);
            ctrl_checkbox(s, "Don't allocate a pseudo-terminal", 'p',
                          HELPCTX(ssh_nopty),
                          conf_checkbox_handler,
                          I(CONF_nopty));

            s = ctrl_getset(b, "Connection/SSH/TTY", "ttymodes",
                            "Terminal modes");
            td = (struct ttymodes_data *)
                ctrl_alloc(b, sizeof(struct ttymodes_data));
            ctrl_text(s, "Terminal modes to send:", HELPCTX(ssh_ttymodes));
            td->listbox = ctrl_listbox(s, NULL, NO_SHORTCUT,
                                       HELPCTX(ssh_ttymodes),
                                       ttymodes_handler, P(td));
            td->listbox->listbox.height = 8;
            td->listbox->listbox.ncols = 2;
            td->listbox->listbox.percentages = snewn(2, int);
            td->listbox->listbox.percentages[0] = 40;
            td->listbox->listbox.percentages[1] = 60;
            ctrl_columns(s, 2, 75, 25);
            c = ctrl_text(s, "For selected mode, send:", HELPCTX(ssh_ttymodes));
            c->column = 0;
            td->setbutton = ctrl_pushbutton(s, "Set", 's',
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
                                             "Auto", NO_SHORTCUT, P(NULL),
                                             "Nothing", NO_SHORTCUT, P(NULL),
                                             "This:", NO_SHORTCUT, P(NULL));
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
                          "Options controlling SSH X11 forwarding");

            s = ctrl_getset(b, "Connection/SSH/X11", "x11", "X11 forwarding");
            ctrl_checkbox(s, "Enable X11 forwarding", 'e',
                          HELPCTX(ssh_tunnels_x11),
                          conf_checkbox_handler,I(CONF_x11_forward));
            ctrl_editbox(s, "X display location", 'x', 50,
                         HELPCTX(ssh_tunnels_x11),
                         conf_editbox_handler, I(CONF_x11_display), ED_STR);
            ctrl_radiobuttons(s, "Remote X11 authentication protocol", 'u', 2,
                              HELPCTX(ssh_tunnels_x11auth),
                              conf_radiobutton_handler,
                              I(CONF_x11_auth),
                              "MIT-Magic-Cookie-1", I(X11_MIT),
                              "XDM-Authorization-1", I(X11_XDM));
        }

        /*
         * The Tunnels panel _is_ still available in mid-session.
         */
        ctrl_settitle(b, "Connection/SSH/Tunnels",
                      "Options controlling SSH port forwarding");

        s = ctrl_getset(b, "Connection/SSH/Tunnels", "portfwd",
                        "Port forwarding");
        ctrl_checkbox(s, "Local ports accept connections from other hosts",'t',
                      HELPCTX(ssh_tunnels_portfwd_localhost),
                      conf_checkbox_handler,
                      I(CONF_lport_acceptall));
        ctrl_checkbox(s, "Remote ports do the same (SSH-2 only)", 'p',
                      HELPCTX(ssh_tunnels_portfwd_localhost),
                      conf_checkbox_handler,
                      I(CONF_rport_acceptall));
#ifdef MOD_PERSO
        if (!GetPuttyFlag())
            ctrl_checkbox(s, "Print dynamic ports in window title", NO_SHORTCUT,
                          HELPCTX(no_help), conf_checkbox_handler,
                          I(CONF_ssh_tunnel_print_in_title));
#endif

        ctrl_columns(s, 3, 55, 20, 25);
        c = ctrl_text(s, "Forwarded ports:", HELPCTX(ssh_tunnels_portfwd));
        c->column = COLUMN_FIELD(0,2);
        /* You want to select from the list, _then_ hit Remove. So tab order
         * should be that way round. */
        pfd = (struct portfwd_data *)ctrl_alloc(b,sizeof(struct portfwd_data));
        pfd->rembutton = ctrl_pushbutton(s, "Remove", 'r',
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
        ctrl_text(s, "Add new forwarded port:", HELPCTX(ssh_tunnels_portfwd));
        /* You want to enter source, destination and type, _then_ hit Add.
         * Again, we adjust the tab order to reflect this. */
        pfd->addbutton = ctrl_pushbutton(s, "Add", 'd',
                                         HELPCTX(ssh_tunnels_portfwd),
                                         portfwd_handler, P(pfd));
        pfd->addbutton->column = 2;
        pfd->addbutton->delay_taborder = true;
        pfd->sourcebox = ctrl_editbox(s, "Source port", 's', 40,
                                      HELPCTX(ssh_tunnels_portfwd),
                                      portfwd_handler, P(pfd), P(NULL));
        pfd->sourcebox->column = 0;
        pfd->destbox = ctrl_editbox(s, "Destination", 'i', 67,
                                    HELPCTX(ssh_tunnels_portfwd),
                                    portfwd_handler, P(pfd), P(NULL));
        pfd->direction = ctrl_radiobuttons(s, NULL, NO_SHORTCUT, 3,
                                           HELPCTX(ssh_tunnels_portfwd),
                                           portfwd_handler, P(pfd),
                                           "Local", 'l', P(NULL),
                                           "Remote", 'm', P(NULL),
                                           "Dynamic", 'y', P(NULL));
#ifndef NO_IPV6
        pfd->addressfamily =
            ctrl_radiobuttons(s, NULL, NO_SHORTCUT, 3,
                              HELPCTX(ssh_tunnels_portfwd_ipversion),
                              portfwd_handler, P(pfd),
                              "Auto", 'u', I(ADDRTYPE_UNSPEC),
                              "IPv4", '4', I(ADDRTYPE_IPV4),
                              "IPv6", '6', I(ADDRTYPE_IPV6));
#endif
        ctrl_tabdelay(s, pfd->addbutton);
        ctrl_columns(s, 1, 100);

        if (!midsession) {
            /*
             * The Connection/SSH/Bugs panels.
             */
            ctrl_settitle(b, "Connection/SSH/Bugs",
                          "Workarounds for SSH server bugs");

            s = ctrl_getset(b, "Connection/SSH/Bugs", "main",
                            "Detection of known bugs in SSH servers");
            ctrl_droplist(s, "Chokes on SSH-2 ignore messages", '2', 20,
                          HELPCTX(ssh_bugs_ignore2),
                          sshbug_handler, I(CONF_sshbug_ignore2));
            ctrl_droplist(s, "Handles SSH-2 key re-exchange badly", 'k', 20,
                          HELPCTX(ssh_bugs_rekey2),
                          sshbug_handler, I(CONF_sshbug_rekey2));
            ctrl_droplist(s, "Chokes on PuTTY's SSH-2 'winadj' requests", 'j',
                          20, HELPCTX(ssh_bugs_winadj),
                          sshbug_handler, I(CONF_sshbug_winadj));
            ctrl_droplist(s, "Replies to requests on closed channels", 'q', 20,
                          HELPCTX(ssh_bugs_chanreq),
                          sshbug_handler, I(CONF_sshbug_chanreq));
            ctrl_droplist(s, "Ignores SSH-2 maximum packet size", 'x', 20,
                          HELPCTX(ssh_bugs_maxpkt2),
                          sshbug_handler, I(CONF_sshbug_maxpkt2));

            s = ctrl_getset(b, "Connection/SSH/Bugs", "manual",
                            "Manually enabled workarounds");
            ctrl_droplist(s, "Discards data sent before its greeting", 'd', 20,
                          HELPCTX(ssh_bugs_dropstart),
                          sshbug_handler_manual_only,
                          I(CONF_sshbug_dropstart));
            ctrl_droplist(s, "Chokes on PuTTY's full KEXINIT", 'p', 20,
                          HELPCTX(ssh_bugs_filter_kexinit),
                          sshbug_handler_manual_only,
                          I(CONF_sshbug_filter_kexinit));

            ctrl_settitle(b, "Connection/SSH/More bugs",
                          "Further workarounds for SSH server bugs");

            s = ctrl_getset(b, "Connection/SSH/More bugs", "main",
                            "Detection of known bugs in SSH servers");
            ctrl_droplist(s, "Old RSA/SHA2 cert algorithm naming", 'l', 20,
                          HELPCTX(ssh_bugs_rsa_sha2_cert_userauth),
                          sshbug_handler,
                          I(CONF_sshbug_rsa_sha2_cert_userauth));
            ctrl_droplist(s, "Requires padding on SSH-2 RSA signatures", 'p', 20,
                          HELPCTX(ssh_bugs_rsapad2),
                          sshbug_handler, I(CONF_sshbug_rsapad2));
            ctrl_droplist(s, "Only supports pre-RFC4419 SSH-2 DH GEX", 'd', 20,
                          HELPCTX(ssh_bugs_oldgex2),
                          sshbug_handler, I(CONF_sshbug_oldgex2));
            ctrl_droplist(s, "Miscomputes SSH-2 HMAC keys", 'm', 20,
                          HELPCTX(ssh_bugs_hmac2),
                          sshbug_handler, I(CONF_sshbug_hmac2));
            ctrl_droplist(s, "Misuses the session ID in SSH-2 PK auth", 'n', 20,
                          HELPCTX(ssh_bugs_pksessid2),
                          sshbug_handler, I(CONF_sshbug_pksessid2));
            ctrl_droplist(s, "Miscomputes SSH-2 encryption keys", 'e', 20,
                          HELPCTX(ssh_bugs_derivekey2),
                          sshbug_handler, I(CONF_sshbug_derivekey2));
            ctrl_droplist(s, "Chokes on SSH-1 ignore messages", 'i', 20,
                          HELPCTX(ssh_bugs_ignore1),
                          sshbug_handler, I(CONF_sshbug_ignore1));
            ctrl_droplist(s, "Refuses all SSH-1 password camouflage", 's', 20,
                          HELPCTX(ssh_bugs_plainpw1),
                          sshbug_handler, I(CONF_sshbug_plainpw1));
            ctrl_droplist(s, "Chokes on SSH-1 RSA authentication", 'r', 20,
                          HELPCTX(ssh_bugs_rsa1),
                          sshbug_handler, I(CONF_sshbug_rsa1));
        }

#ifdef MOD_PERSO
        /* KiTTY: PSCP / WinSCP integration. Backend = StartWinSCP / SendFile. */
        if (!GetPuttyFlag()) {
            ctrl_settitle(b, "Connection/SSH/KSCP and WinSCP",
                          "KSCP and WinSCP integration");

            s = ctrl_getset(b, "Connection/SSH/KSCP and WinSCP",
                            "winSCPproto", "General protocol setting");
            ctrl_radiobuttons(s, "Prefered protocol:", NO_SHORTCUT, 4,
                              HELPCTX(no_help),
                              conf_radiobutton_handler,
                              I(CONF_winscpprot),
                              "scp",   NO_SHORTCUT, I(0),
                              "sftp",  NO_SHORTCUT, I(1),
                              "ftp",   NO_SHORTCUT, I(2),
                              "ftps",  NO_SHORTCUT, I(3),
                              "ftpes", NO_SHORTCUT, I(4),
                              "http",  NO_SHORTCUT, I(5),
                              "https", NO_SHORTCUT, I(6));

            s = ctrl_getset(b, "Connection/SSH/KSCP and WinSCP",
                            "pscp", "KSCP integration");
            g_osc7_track_ctrl = ctrl_checkbox(s,
                          "Track remote directory (OSC 7 shell integration)",
                          NO_SHORTCUT, HELPCTX(no_help),
                          kitty_osc7_track_handler, P(NULL));
            ctrl_text(s, "Drag-drop uploads and WinSCP open in the shell's "
                         "current remote directory (via OSC 7) instead of your "
                         "home. Passive - nothing runs remotely.",
                      HELPCTX(no_help));
            g_pscp_remotedir_ctrl = ctrl_editbox(s,
                         "Fixed remote upload directory", NO_SHORTCUT, 100,
                         HELPCTX(no_help),
                         kitty_pscp_remotedir_handler, P(NULL), P(NULL));
            ctrl_text(s, "Always upload here instead - mutually exclusive with "
                         "OSC 7 tracking above.", HELPCTX(no_help));
            ctrl_editbox(s, "KSCP options", NO_SHORTCUT, 100,
                         HELPCTX(no_help),
                         conf_editbox_handler, I(CONF_pscpoptions), ED_STR);
            ctrl_text(s, "Flags passed to kscp; default -r uploads dropped "
                         "folders recursively.", HELPCTX(no_help));
            ctrl_checkbox(s, "Keep the transfer window open after success",
                          NO_SHORTCUT, HELPCTX(no_help),
                          conf_checkbox_handler, I(CONF_pscp_keep_window));

            s = ctrl_getset(b, "Connection/SSH/KSCP and WinSCP",
                            "WinSCP", "WinSCP integration");
            /* Global app setting (kitty.ini [KiTTY] WinSCPPath), not per-session;
             * uses a custom handler rather than conf_filesel_handler. */
            ctrl_filesel(s, "WinSCP executable:", NO_SHORTCUT,
                         FILTER_ALL_FILES, false, "Select WinSCP executable",
                         HELPCTX(no_help),
                         kitty_winscppath_handler, P(NULL));
            ctrl_editbox(s, "SFTP connect ([user@]hostname[:port])",
                         NO_SHORTCUT, 100,
                         HELPCTX(no_help),
                         conf_editbox_handler, I(CONF_sftpconnect), ED_STR);
            ctrl_editbox(s, "WinSCP additional options", NO_SHORTCUT, 100,
                         HELPCTX(no_help),
                         conf_editbox_handler, I(CONF_winscpoptions), ED_STR);
            ctrl_editbox(s, "WinSCP additional rawsettings", NO_SHORTCUT, 100,
                         HELPCTX(no_help),
                         conf_editbox_handler, I(CONF_winscprawsettings), ED_STR);
            ctrl_editbox(s, "Shell (scp mode only)", NO_SHORTCUT, 100,
                         HELPCTX(no_help),
                         conf_editbox_handler, I(CONF_pscpshell), ED_STR);
        }
#endif
    }
}

/* The Connection/Serial, Telnet, Rlogin and SUPDUP panels. */
static void scb_panel_other_protocols(struct controlbox *b, bool midsession, int protocol)
{
    struct controlset *s;

    if (DISPLAY_RECONFIGURABLE_PROTOCOL(PROT_SERIAL)) {
        const BackendVtable *ser_vt = backend_vt_from_proto(PROT_SERIAL);

        /*
         * The Connection/Serial panel.
         */
        ctrl_settitle(b, "Connection/Serial",
                      "Options controlling local serial lines");

        if (!midsession) {
            /*
             * We don't permit switching to a different serial port in
             * midflight, although we do allow all other
             * reconfiguration.
             */
            s = ctrl_getset(b, "Connection/Serial", "serline",
                            "Select a serial line");
            ctrl_editbox(s, "Serial line to connect to", 'l', 40,
                         HELPCTX(serial_line),
                         conf_editbox_handler, I(CONF_serline), ED_STR);
        }

        s = ctrl_getset(b, "Connection/Serial", "sercfg", "Configure the serial line");
        ctrl_editbox(s, "Speed (baud)", 's', 40,
                     HELPCTX(serial_speed),
                     conf_editbox_handler, I(CONF_serspeed), ED_INT);
        ctrl_editbox(s, "Data bits", 'b', 40,
                     HELPCTX(serial_databits),
                     conf_editbox_handler, I(CONF_serdatabits), ED_INT);
        /*
         * Stop bits come in units of one half.
         */
        static const struct conf_editbox_handler_type conf_editbox_stopbits = {
            .type = EDIT_FIXEDPOINT, .denominator = 2};

        ctrl_editbox(s, "Stop bits", 't', 40,
                     HELPCTX(serial_stopbits),
                     conf_editbox_handler, I(CONF_serstopbits),
                     CP(&conf_editbox_stopbits));
        ctrl_droplist(s, "Parity", 'p', 40,
                      HELPCTX(serial_parity), serial_parity_handler,
                      I(ser_vt->serial_parity_mask));
        ctrl_droplist(s, "Flow control", 'f', 40,
                      HELPCTX(serial_flow), serial_flow_handler,
                      I(ser_vt->serial_flow_mask));
    }

    if (DISPLAY_RECONFIGURABLE_PROTOCOL(PROT_TELNET)) {
        /*
         * The Connection/Telnet panel.
         */
        ctrl_settitle(b, "Connection/Telnet",
                      "Options controlling Telnet connections");

        s = ctrl_getset(b, "Connection/Telnet", "protocol",
                        "Telnet protocol adjustments");

        if (!midsession) {
            ctrl_radiobuttons(s, "Handling of OLD_ENVIRON ambiguity:",
                              NO_SHORTCUT, 2,
                              HELPCTX(telnet_oldenviron),
                              conf_radiobutton_bool_handler,
                              I(CONF_rfc_environ),
                              "BSD (commonplace)", 'b', I(false),
                              "RFC 1408 (unusual)", 'f', I(true));
            ctrl_radiobuttons(s, "Telnet negotiation mode:", 't', 2,
                              HELPCTX(telnet_passive),
                              conf_radiobutton_bool_handler,
                              I(CONF_passive_telnet),
                              "Passive", I(true), "Active", I(false));
        }
        ctrl_checkbox(s, "Keyboard sends Telnet special commands", 'k',
                      HELPCTX(telnet_specialkeys),
                      conf_checkbox_handler,
                      I(CONF_telnet_keyboard));
        ctrl_checkbox(s, "Return key sends Telnet New Line instead of ^M",
                      'm', HELPCTX(telnet_newline),
                      conf_checkbox_handler,
                      I(CONF_telnet_newline));
    }

    if (DISPLAY_NON_RECONFIGURABLE_PROTOCOL(PROT_RLOGIN)) {
        /*
         * The Connection/Rlogin panel.
         */
        ctrl_settitle(b, "Connection/Rlogin",
                      "Options controlling Rlogin connections");

        s = ctrl_getset(b, "Connection/Rlogin", "data",
                        "Data to send to the server");
        ctrl_editbox(s, "Local username:", 'l', 50,
                     HELPCTX(rlogin_localuser),
                     conf_editbox_handler, I(CONF_localusername), ED_STR);

    }

    if (DISPLAY_NON_RECONFIGURABLE_PROTOCOL(PROT_SUPDUP)) {
        /*
         * The Connection/SUPDUP panel.
         */
        ctrl_settitle(b, "Connection/SUPDUP",
                      "Options controlling SUPDUP connections");

        s = ctrl_getset(b, "Connection/SUPDUP", "main", NULL);

        ctrl_editbox(s, "Location string", 'l', 70,
                     HELPCTX(supdup_location),
                     conf_editbox_handler, I(CONF_supdup_location),
                     ED_STR);

        ctrl_radiobuttons(s, "Extended ASCII Character set:", 'e', 4,
                          HELPCTX(supdup_ascii),
                          conf_radiobutton_handler,
                          I(CONF_supdup_ascii_set),
                          "None", I(SUPDUP_CHARSET_ASCII),
                          "ITS", I(SUPDUP_CHARSET_ITS),
                          "WAITS", I(SUPDUP_CHARSET_WAITS));

        ctrl_checkbox(s, "**MORE** processing", 'm',
                      HELPCTX(supdup_more),
                      conf_checkbox_handler,
                      I(CONF_supdup_more));

        ctrl_checkbox(s, "Terminal scrolling", 's',
                      HELPCTX(supdup_scroll),
                      conf_checkbox_handler,
                      I(CONF_supdup_scroll));
    }
}

/* The Connection/ZModem panels (KiTTY). */
static void scb_panel_zmodem(struct controlbox *b)
{
#ifdef MOD_ZMODEM
    struct controlset *s;

    /* The Connection/ZModem panels (KiTTY). Backend = kitty_zmodem_*. */
    if ((!GetPuttyFlag()) && GetZModemFlag()) {
        ctrl_settitle(b, "Connection/ZModem",
                      "Options controlling Z Modem transfers");
        s = ctrl_getset(b, "Connection/ZModem", "download",
                        "Download folder");
        ctrl_editbox(s, "Location:", NO_SHORTCUT, 100,
                     HELPCTX(no_help),
                     conf_editbox_handler, I(CONF_zdownloaddir), ED_STR);

        ctrl_settitle(b, "Connection/ZModem/rz", "rz path and options");
        s = ctrl_getset(b, "Connection/ZModem/rz", "receive",
                        "Receive command");
        ctrl_filesel(s, "Command rz:", NO_SHORTCUT,
                     FILTER_ALL_FILES, false,
                     "Select command to receive zmodem data",
                     HELPCTX(no_help),
                     conf_filesel_handler, I(CONF_rzcommand));
        ctrl_editbox(s, "Options", NO_SHORTCUT, 50,
                     HELPCTX(no_help),
                     conf_editbox_handler, I(CONF_rzoptions), ED_STR);
        ctrl_text(s, "Ctrl+X to quit rz before completing",
                  HELPCTX(no_help));

        ctrl_settitle(b, "Connection/ZModem/sz", "sz path and options");
        s = ctrl_getset(b, "Connection/ZModem/sz", "send",
                        "Send command");
        ctrl_filesel(s, "Command sz:", NO_SHORTCUT,
                     FILTER_ALL_FILES, false,
                     "Select command to send zmodem data",
                     HELPCTX(no_help),
                     conf_filesel_handler, I(CONF_szcommand));
        ctrl_editbox(s, "Options", NO_SHORTCUT, 50,
                     HELPCTX(no_help),
                     conf_editbox_handler, I(CONF_szoptions), ED_STR);
    }
#else
    (void)b;
#endif
}

/* The Comment panel (KiTTY): a free-text note attached to this session. */
static void scb_panel_comment(struct controlbox *b)
{
    struct controlset *s;

    /*
     * The Comment panel (KiTTY): a free-text note attached to this session.
     * Top-level category, created last, to match upstream KiTTY's layout.
     */
    if (!GetPuttyFlag()) {
        ctrl_settitle(b, "Comment", "Comment for this session");
        s = ctrl_getset(b, "Comment", "main", NULL);
        /* Multiline (~5 lines). Newlines round-trip to storage: REG_SZ holds
         * CRLF directly, and file/dir mode mungestr()-encodes control chars. */
        ctrl_editbox_multiline(s, "Session comment", NO_SHORTCUT, 5, false,
                               HELPCTX(no_help), conf_editbox_handler,
                               I(CONF_comment), ED_STR);
    }
}

void setup_config_box(struct controlbox *b, bool midsession,
                      int protocol, int protcfginfo)
{
    scb_panel_session(b, midsession);
    scb_panel_logging(b, midsession, protocol);
    scb_panel_scripting(b);
    scb_panel_terminal(b);
    scb_panel_window(b, midsession, protocol);
    scb_panel_selection(b);
    scb_panel_connection(b, midsession, protocol);
    scb_panel_proxy(b, midsession);
    scb_panel_ssh(b, midsession, protocol, protcfginfo);
    scb_panel_other_protocols(b, midsession, protocol);
    scb_panel_zmodem(b);
    scb_panel_comment(b);
}
